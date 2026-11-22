#include "setup_ble.h"
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

namespace {
// One authenticated characteristic. UUID bytes are in BLE wire order.
const ble_uuid128_t service = BLE_UUID128_INIT(1,0,0,0,0,0,0,0x80,0,0x40,0,0,0x49,0x4e,0x4d,0x4f);
const ble_uuid128_t channel = BLE_UUID128_INIT(2,0,0,0,0,0,0,0x80,0,0x40,0,0,0x49,0x4e,0x4d,0x4f);
unsigned pairing_code;
uint8_t address_type;
struct Route { const char* path; bool post; };
constexpr Route routes[] = {{"/profile",false},{"/networks",false},{"/saver",false},
                            {"/profile",true},{"/saver",true},{"/exit",true}};
struct Transaction {
    unsigned generation;
    unsigned route;
    std::string body;
};
std::mutex lock;
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
    auto route = routes[request->route];
    std::string url = std::string("http://127.0.0.1") + route.path;
    std::string result;
    esp_http_client_config_t config{};
    config.url = url.c_str();
    config.timeout_ms = 10000;
    config.event_handler = Collect;
    config.user_data = &result;
    auto client = esp_http_client_init(&config);
    int code = 503;
    if (client) {
        if (route.post) {
            esp_http_client_set_method(client, HTTP_METHOD_POST);
            esp_http_client_set_header(client, "Content-Type", request->route == 4 ?
                                       "application/octet-stream" : "application/json");
            esp_http_client_set_post_field(client, request->body.data(), request->body.size());
        }
        if (esp_http_client_perform(client) == ESP_OK) code = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
    }
    {
        std::lock_guard<std::mutex> guard(lock);
        if (generation == request->generation) {
            reply = code == 503 ? "设备暂时忙，请重试" : std::move(result);
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
    if (ctx->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        unsigned mtu = ble_att_mtu(conn);
        uint8_t header[] = {uint8_t(status),uint8_t(status >> 8),uint8_t(reply.size()),
                            uint8_t(reply.size() >> 8),uint8_t(std::min(240u, mtu - 4))};
        auto size = std::min<size_t>(std::min(mtu, 247u) - sizeof(header) - 2, reply.size() - offset);
        if (os_mbuf_append(ctx->om, header, sizeof(header)) ||
            os_mbuf_append(ctx->om, reply.data() + offset, size)) return BLE_ATT_ERR_INSUFFICIENT_RES;
        return 0;
    }
    uint8_t packet[244];
    auto len = OS_MBUF_PKTLEN(ctx->om);
    if (!len || len > sizeof(packet) || os_mbuf_copydata(ctx->om, 0, len, packet))
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    switch (packet[0]) {
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
    if (event->type == BLE_GAP_EVENT_PASSKEY_ACTION &&
        event->passkey.params.action == BLE_SM_IOACT_DISP) {
        ble_sm_io io{};
        io.action = BLE_SM_IOACT_DISP;
        io.passkey = pairing_code;
        return ble_sm_inject_io(event->passkey.conn_handle, &io);
    }
    if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        std::lock_guard<std::mutex> guard(lock);
        ++generation; incoming.reset(); reply.clear(); status = offset = 0;
    }
    if (event->type == BLE_GAP_EVENT_DISCONNECT ||
        (event->type == BLE_GAP_EVENT_CONNECT && event->connect.status)) Advertise();
    return 0;
}
void Advertise() {
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
    if (ble_hs_id_infer_auto(0,&address_type) || ble_gap_adv_set_fields(&fields) ||
        ble_gap_adv_rsp_set_fields(&scan) ||
        ble_gap_adv_start(address_type,nullptr,BLE_HS_FOREVER,&params,Gap,nullptr))
        ESP_LOGE("setup_ble","Advertising failed");
}
}

void StartSetupBluetooth(const char* device_name, unsigned passkey) {
    if (nimble_port_init() != ESP_OK) { ESP_LOGE("setup_ble","Initialization failed"); return; }
    pairing_code = passkey;
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
        ESP_LOGE("setup_ble","Service registration failed"); return;
    }
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_mitm = ble_hs_cfg.sm_sc = ble_hs_cfg.sm_sc_only = 1;
    ble_hs_cfg.sync_cb = Advertise;
    nimble_port_freertos_init([](void*) { nimble_port_run(); nimble_port_freertos_deinit(); });
}
