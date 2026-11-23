#include "setup_ble.h"
#include "phone_setup.h"
#include "settings.h"
#include "esp_random.h"
#include "esp_mac.h"
#include <atomic>
#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "esp_http_client.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" void ble_store_config_init(void);
namespace {
// One authenticated characteristic. UUID bytes are in BLE wire order.
const ble_uuid128_t service = BLE_UUID128_INIT(1,0,0,0,0,0,0,0x80,0,0x40,0,0,0x49,0x4e,0x4d,0x4f);
const ble_uuid128_t channel = BLE_UUID128_INIT(2,0,0,0,0,0,0,0x80,0,0x40,0,0,0x49,0x4e,0x4d,0x4f);
std::atomic<bool> enabled{false};
std::atomic<uint16_t> active_connection{BLE_HS_CONN_HANDLE_NONE};
std::atomic<unsigned> pairing_code{0};
bool initialized;
std::mutex radio_mutex;
CustomLcdDisplay* setup_display;
uint8_t address_type;
struct Route { const char* path; bool post; };
constexpr Route routes[] = {{"/profile",false},{"/networks",false},{"/saver",false},
                            {"/profile",true},{"/saver",true},{"/exit",true},
                            {"/shutdown-saver",false},{"/shutdown-saver",true},{"/access",true},{"/factory-reset",true},{"/voice-controls",true}};
struct Transaction {
    unsigned generation;
    unsigned route;
    std::string body;
};
std::mutex lock;
// Never sent over GATT. Only the authenticated BLE worker attaches this
// capability to requests it forwards to the local HTTP handlers.
const std::string& BridgeSecret() {
    static const std::string secret = [] {
        uint8_t bytes[32];
        esp_fill_random(bytes,sizeof(bytes));
        char hex[65];
        for (unsigned i=0;i<sizeof(bytes);++i) snprintf(hex+2*i,3,"%02x",bytes[i]);
        return std::string(hex);
    }();
    return secret;
}
unsigned generation = 0;
std::unique_ptr<Transaction> incoming;
std::string reply;
uint16_t status, offset;
bool running;

esp_err_t Collect(esp_http_client_event_t* event) {
    if (event->event_id == HTTP_EVENT_ON_DATA) {
        auto& body = *static_cast<std::string*>(event->user_data);
        if (body.size() + event->data_len <= 65535)
            body.append(static_cast<const char*>(event->data), event->data_len);
    }
    return ESP_OK;
}

// Dispatch only the allowlisted operations to the device's existing setup server.
void Dispatch(void* argument) {
    std::unique_ptr<Transaction> request(static_cast<Transaction*>(argument));
    std::string result;
    int code = enabled ? DispatchManagement(request->route,request->body,result) : 503;
    if (result.size() > UINT16_MAX) {
        code = 413;
        result = "设置数据超过蓝牙传输上限，请使用同网管理";
    }
    if (code == 503 && result.empty()) result = "设备蓝牙管理未就绪";
    if (code >= 400) ESP_LOGW("setup_ble", "route %u returned %d", request->route, code);
    {
        std::lock_guard<std::mutex> guard(lock);
        if (generation == request->generation) {
            reply = std::move(result);
            status = code;
        }
        running = false;
    }
}

void Worker(void* argument) {
    Dispatch(argument);
    vTaskDelete(nullptr);
}

int Access(uint16_t conn, uint16_t, ble_gatt_access_ctxt* ctx, void*) {
    std::lock_guard<std::mutex> guard(lock);
    if (!enabled) return BLE_ATT_ERR_UNLIKELY;
    ble_gap_conn_desc peer{};
    if (ble_gap_conn_find(conn,&peer) || !peer.sec_state.encrypted || !peer.sec_state.authenticated)
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    if (ctx->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        unsigned mtu = ble_att_mtu(conn);
        uint8_t header[] = {uint8_t(status),uint8_t(status >> 8),uint8_t(reply.size()),
                            uint8_t(reply.size() >> 8),uint8_t(std::min(240u, mtu - 4))};
        auto size = std::min<size_t>(std::min(mtu, 247u) - sizeof(header) - 2, reply.size() - offset);
        if (os_mbuf_append(ctx->om, header, sizeof(header)) ||
            os_mbuf_append(ctx->om, reply.data() + offset, size)) return BLE_ATT_ERR_INSUFFICIENT_RES;
        if (!status && !incoming && !running && reply.empty()) {
            uint8_t paired_access = 1;
            if (os_mbuf_append(ctx->om,&paired_access,1)) return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return 0;
    }
    uint8_t packet[244];
    auto len = OS_MBUF_PKTLEN(ctx->om);
    if (!len || len > sizeof(packet) || os_mbuf_copydata(ctx->om, 0, len, packet))
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    switch (packet[0]) {
    case 4: {
        if (len != 17 || running || incoming) break;
        // Older setup pages send a browser key after pairing.
        return 0;
    }
    case 0: // Begin, discarding an incomplete upload.
        if (running || len != 2 || packet[1] >= sizeof(routes)/sizeof(Route)) break;
        incoming = std::make_unique<Transaction>(Transaction{generation,packet[1],{}});
        reply.clear(); status = offset = 0;
        return 0;
    case 1:
        if (!incoming || incoming->body.size() + len - 1 > 25000) break;
        incoming->body.append(reinterpret_cast<char*>(packet + 1), len - 1);
        return 0;
    case 2: {
        if (!incoming || len != 1) break;
        auto* request = incoming.release();
        running = true;
        if (xTaskCreate(Worker,"setup_rpc",6144,request,4,nullptr) != pdPASS) {
            delete request; running = false; return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return 0;
    }
    case 3:
        if (len != 3 || size_t(packet[1] | packet[2] << 8) > reply.size()) break;
        offset = packet[1] | packet[2] << 8;
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

ble_gatt_chr_def characteristics[2]{};
ble_gatt_svc_def services[2]{};
void Advertise();
int Gap(ble_gap_event* event, void*) {
    if (event->type == BLE_GAP_EVENT_CONNECT && !event->connect.status) {
        active_connection=event->connect.conn_handle;
        if (!enabled) ble_gap_terminate(event->connect.conn_handle,BLE_ERR_REM_USER_CONN_TERM);
    }
    if (event->type == BLE_GAP_EVENT_ENC_CHANGE) ShowBluetoothPairing(false);
    if (event->type == BLE_GAP_EVENT_REPEAT_PAIRING) {
        ble_gap_conn_desc desc{};
        if (!ble_gap_conn_find(event->repeat_pairing.conn_handle,&desc) &&
            !ble_store_util_delete_peer(&desc.peer_id_addr)) return BLE_GAP_REPEAT_PAIRING_RETRY;
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }
    if (event->type == BLE_GAP_EVENT_PASSKEY_ACTION &&
        event->passkey.params.action == BLE_SM_IOACT_DISP) {
        ble_sm_io io{};
        io.action = BLE_SM_IOACT_DISP;
        io.passkey = pairing_code;
        ShowBluetoothPairing(true);
        return ble_sm_inject_io(event->passkey.conn_handle, &io);
    }
    if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        std::lock_guard<std::mutex> guard(lock);
        active_connection=BLE_HS_CONN_HANDLE_NONE;
        ShowBluetoothPairing(false);
        ++generation; incoming.reset(); reply.clear(); status = offset = 0;
    }
    if (event->type == BLE_GAP_EVENT_DISCONNECT ||
        (event->type == BLE_GAP_EVENT_CONNECT && event->connect.status)) Advertise();
    return 0;
}
void Advertise() {
    if (!enabled || active_connection != BLE_HS_CONN_HANDLE_NONE || ble_gap_adv_active()) return;
    ble_hs_adv_fields fields{};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = const_cast<ble_uuid128_t*>(&service);
    fields.num_uuids128 = fields.uuids128_is_complete = 1;
    ble_hs_adv_fields scan{};
    scan.name = reinterpret_cast<const uint8_t*>(ble_svc_gap_device_name());
    scan.name_len = strlen(ble_svc_gap_device_name());
    scan.name_is_complete = 1;
    ble_gap_adv_params params{};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    int error=ble_hs_id_infer_auto(0,&address_type);
    if (!error) error=ble_gap_adv_set_fields(&fields);
    if (!error) error=ble_gap_adv_rsp_set_fields(&scan);
    if (!error) error=ble_gap_adv_start(address_type,nullptr,BLE_HS_FOREVER,&params,Gap,nullptr);
    if (error && error!=BLE_HS_EALREADY) ESP_LOGE("setup_ble","Advertising failed: %d",error);
    if (!enabled) ble_gap_adv_stop();
}
}

bool IsSetupBluetoothSecret(const char* value) { return enabled && BridgeSecret()==value; }

bool SetupBluetoothConnected() { return active_connection.load() != BLE_HS_CONN_HANDLE_NONE; }

bool SetupBluetoothEnabled() { return enabled.load(); }
int DispatchManagement(unsigned index,const std::string& body,std::string& result) {
    if (index >= sizeof(routes)/sizeof(Route) || body.size()>25000) return 400;
    auto route = routes[index];
    if (!strcmp(route.path,"/access")) { result="已授权"; return 200; }
    if (!route.post && !strcmp(route.path,"/profile")) return ReadPairedBluetoothProfile(result);
    if (route.post && !strcmp(route.path,"/factory-reset")) return RequestPocketReset(body,result);
    if (route.post && !strcmp(route.path,"/voice-controls")) return SavePairedVoiceControls(body,result);
    auto port = BluetoothSetupPort(setup_display);
    if (!port) { result="设备内的管理服务启动失败"; return 503; }
    std::string url = "http://127.0.0.1:" + std::to_string(port) + route.path;
    esp_http_client_config_t config{};
    config.url = url.c_str();
    config.timeout_ms = 10000;
    config.event_handler = Collect;
    config.user_data = &result;
    auto client = esp_http_client_init(&config);
    if (!client) { result="内存不足，无法创建管理请求"; return 503; }
    int code = 503;
    if (client) {
        esp_http_client_set_header(client,"X-Pocket-BLE",BridgeSecret().c_str());
        if (route.post) {
            esp_http_client_set_method(client, HTTP_METHOD_POST);
            esp_http_client_set_header(client, "Content-Type", (index == 4 || index == 7) ?
                                       "application/octet-stream" : "application/json");
            esp_http_client_set_post_field(client, body.data(), body.size());
        }
        auto err = esp_http_client_perform(client);
        if (err == ESP_OK) code = esp_http_client_get_status_code(client);
        else result = std::string("设备内部管理连接失败 · ") + esp_err_to_name(err);
        esp_http_client_cleanup(client);
    }
    return code;
}

unsigned SetupBluetoothPasskey() { return pairing_code; }

static bool ApplyBluetoothEnabled(bool on) {
    if (!on) {
        enabled=false;
        if (initialized && ble_hs_synced()) {
            ble_gap_adv_stop();
            auto conn=active_connection.load();
            if(conn!=BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(conn,BLE_ERR_REM_USER_CONN_TERM);
        }
        return true;
    }
    enabled=true;
    if(initialized) {
        if(ble_hs_synced()) Advertise();
        return true;
    }
    uint8_t mac[6];
    esp_read_mac(mac,ESP_MAC_WIFI_SOFTAP);
    char device_name[16];
    snprintf(device_name,sizeof(device_name),"Omni-%02X%02X",mac[4],mac[5]);
    if (nimble_port_init() != ESP_OK) { ESP_LOGE("setup_ble","Initialization failed"); enabled=false; return false; }
    pairing_code = esp_random()%1000000;
    ble_svc_gap_init(); ble_svc_gatt_init();
    ble_svc_gap_device_name_set(device_name);
    characteristics[0].uuid = &channel.u;
    characteristics[0].access_cb = Access;
    characteristics[0].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                               BLE_GATT_CHR_F_READ_AUTHEN | BLE_GATT_CHR_F_WRITE_AUTHEN |
                               BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE_ENC;
    services[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    services[0].uuid = &service.u;
    services[0].characteristics = characteristics;
    if (ble_gatts_count_cfg(services) || ble_gatts_add_svcs(services)) {
        ESP_LOGE("setup_ble","Service registration failed"); nimble_port_deinit(); enabled=false; return false;
    }
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_mitm = ble_hs_cfg.sm_sc = ble_hs_cfg.sm_sc_only = 1;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_our_key_dist = ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();
    ble_hs_cfg.sync_cb = Advertise;
    initialized=true;
    nimble_port_freertos_init([](void*) { nimble_port_run(); nimble_port_freertos_deinit(); });
    return true;
}

void InitializeSetupBluetooth(CustomLcdDisplay* display) {
    setup_display=display;
    InitializeBluetoothPrompts(display);
    std::lock_guard<std::mutex> guard(radio_mutex);
    if(Settings("omni_ui").GetInt("bluetooth",1)) ApplyBluetoothEnabled(true);
}

bool SetSetupBluetoothEnabled(bool on) {
    std::lock_guard<std::mutex> guard(radio_mutex);
    if (!ApplyBluetoothEnabled(on)) return false;
    Settings("omni_ui",true).SetInt("bluetooth",on);
    return true;
}
