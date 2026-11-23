#include "voice/control_policy.h"
#include "phone_setup.h"
#include "factory_reset.h"
#include "application.h"
#include "board.h"
#include "pocket_library.h"
#include "setup_ble.h"
#include "setup_access.h"
#include "wifi_station.h"
#include <mutex>
#include "lwip/sockets.h"
#include <mdns.h>
#include "offline_commands.h"

#include <esp_https_server.h>
#include <esp_http_server.h>
#include <esp_check.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <cassert>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <memory>
#include <cstring>

#include "custom_lcd_display.h"
#include "voice/voice_profile.h"
#include "settings.h"
#include "standby.h"
#include "pocket_update.h"

void SetPocketWakeEnabled(bool enabled) {
    Settings("omni_ui", true).SetInt("wake", enabled);
    Application::GetInstance().Schedule([]() {
        auto& app = Application::GetInstance();
        bool allowed = Settings("omni_ui").GetInt("wake", 1);
        auto& audio = app.GetAudioService();
        audio.SetWakeWordAllowed(allowed);
        if (allowed && (app.GetDeviceState() == kDeviceStateIdle || app.GetDeviceState() == kDeviceStateWifiConfiguring))
            audio.EnableWakeWordDetection(true);
        static_cast<CustomLcdDisplay*>(Board::GetInstance().GetDisplay())->RefreshHome();
    });
}

namespace {
std::mutex profile_mutex;
}
bool TogglePocketVoiceService() {
    std::lock_guard<std::mutex> guard(profile_mutex);
    if (!VoiceProfile::ApplyPendingWifi()) return false;
    auto profile = VoiceProfile::Load();
    if (profile.url.empty()) return false;
    profile.default_selected = profile.Custom();
    return profile.Save();
}

namespace {
std::atomic<bool> offline_home{false};
std::atomic<TaskHandle_t> setup_task{nullptr};
std::mutex prompt_mutex;
std::string shown_ticket;
constexpr uint32_t show_setup = 1, settings_saved = 2, leave_setup = 4;

extern const char page[] asm("_binary_index_html_start");
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

esp_err_t Reply(httpd_req_t* req, const char* type, const char* body) {
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_type(req, type);
    return httpd_resp_sendstr(req, body);
}

esp_err_t Bad(httpd_req_t* req, const char* reason) {
    httpd_resp_set_status(req, "400 Bad Request");
    return Reply(req, "text/plain; charset=utf-8", reason);
}

// lan_background retains the original manual settings (0 and 1).
enum LanMode { ManualOff, ManualOn, FirstConnection, FirstConnectionUsed };
std::mutex policy_mutex;
LanMode lan_mode = FirstConnection;
httpd_handle_t first_server = nullptr;
std::string first_browser;
int64_t first_deadline = 0;
constexpr int64_t presence_grace_us = 90000000;

std::string BrowserKey(httpd_req_t* req) {
    char key[33];
    return httpd_req_get_hdr_value_str(req,"X-Pocket-Key",key,sizeof(key)) == ESP_OK ? key : "";
}

// Hold standby during requests, including slow SD operations. Between requests
// a management page renews its presence every 20 seconds.
std::mutex activity_mutex;
unsigned active_requests = 0;
int64_t awake_until_us = 0;
class ManagementRequest {
    httpd_handle_t server_;
public:
    explicit ManagementRequest(httpd_req_t* req) : server_(req->handle) {
        std::lock_guard<std::mutex> lock(activity_mutex);
        ++active_requests;
    }
    ~ManagementRequest() {
        std::lock_guard<std::mutex> lock(activity_mutex);
        --active_requests;
        awake_until_us = esp_timer_get_time() + presence_grace_us;
        std::lock_guard<std::mutex> policy(policy_mutex);
        if (server_ == first_server && first_deadline) first_deadline = awake_until_us;
    }
};

esp_err_t Saver(httpd_req_t* req, CustomLcdDisplay::Saver slot) {
    if (!SetupRequestAllowed(req)) return ESP_OK;
    ManagementRequest activity(req);
    auto display = static_cast<CustomLcdDisplay*>(req->user_ctx);
    std::vector<uint8_t> pixels(CustomLcdDisplay::saver_bytes);
    if (req->method == HTTP_GET) {
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        display->ReadSaver(slot, pixels.data());
        httpd_resp_set_type(req, "application/octet-stream");
        return httpd_resp_send(req, reinterpret_cast<char*>(pixels.data()), pixels.size());
    }
    char type[40];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK ||
        strcmp(type, "application/octet-stream") || (req->content_len && req->content_len != pixels.size()))
        return Bad(req, "图片格式或大小不正确");
    size_t received = 0;
    while (received < req->content_len) {
        int got = httpd_req_recv(req, reinterpret_cast<char*>(pixels.data()) + received, pixels.size() - received);
        if (got <= 0) return ESP_FAIL;
        received += got;
    }
    if (!display->SaveSaver(slot, received ? pixels.data() : nullptr))
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "屏保保存失败");
    return Reply(req, "text/plain; charset=utf-8", received ? "屏保已保存" : "已恢复默认小鸭");
}

esp_err_t Networks(httpd_req_t* req) {
    if (!SetupRequestAllowed(req)) return ESP_OK;
    ManagementRequest activity(req);
    // An unprovisioned device normally leaves the Wi-Fi radio off.
    struct TemporaryStation {
        bool initialized = false;
        ~TemporaryStation() {
            if (initialized) { esp_wifi_stop(); esp_wifi_deinit(); }
        }
    } temporary;
    if (offline_home) {
        wifi_mode_t mode;
        if (esp_wifi_get_mode(&mode)==ESP_ERR_WIFI_NOT_INIT) {
            wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();
            temporary.initialized=esp_wifi_init(&init)==ESP_OK;
            if (!temporary.initialized || esp_wifi_set_storage(WIFI_STORAGE_RAM)!=ESP_OK ||
                esp_wifi_set_mode(WIFI_MODE_STA)!=ESP_OK || esp_wifi_start()!=ESP_OK)
                return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Wi-Fi 扫描暂不可用");
        }
    }
    wifi_scan_config_t scan{};
    scan.scan_time.active.max = 100;
    if (esp_wifi_scan_start(&scan, true) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "扫描失败，请重试");
    uint16_t count = 32;
    auto records = std::make_unique<wifi_ap_record_t[]>(count);
    if (esp_wifi_scan_get_ap_records(&count, records.get()) != ESP_OK) {
        esp_wifi_clear_ap_list();
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "读取扫描结果失败");
    }
    Json names(cJSON_CreateArray(), cJSON_Delete);
    if (!names) return ESP_ERR_NO_MEM;
    for (unsigned i = 0; i < count; ++i) {
        auto name = reinterpret_cast<const char*>(records[i].ssid);
        if (!*name) continue;
        bool duplicate = false;
        for (unsigned j = 0; j < i; ++j) {
            if (!strcmp(name, reinterpret_cast<const char*>(records[j].ssid))) duplicate = true;
        }
        if (!duplicate) cJSON_AddItemToArray(names.get(), cJSON_CreateString(name));
    }
    std::unique_ptr<char, decltype(&cJSON_free)> data(cJSON_PrintUnformatted(names.get()), cJSON_free);
    return data ? Reply(req, "application/json", data.get()) : ESP_ERR_NO_MEM;
}

int BuildProfile(std::string& body) {
    auto profile = VoiceProfile::Load();
    Json object(cJSON_CreateObject(), cJSON_Delete);
    if (!object) { body="生成设备设置时内存不足"; return 503; }
    cJSON_AddNumberToObject(object.get(), "voice_policy_bytes", voice_control::max_policy_bytes);
    cJSON_AddTrueToObject(object.get(), "factory_reset");
    cJSON_AddBoolToObject(object.get(), "wake_enabled", Settings("omni_ui").GetInt("wake", 1));
    cJSON_AddBoolToObject(object.get(),"bluetooth_enabled",SetupBluetoothEnabled());
    if (!pocket_library::AddProfile(object.get())) { body="读取媒体设置失败"; return 500; }
    cJSON_AddStringToObject(object.get(), "offline_words", ReadOfflineWords().c_str());
    int minutes = Settings("omni_ui").GetInt("standby_min", standby::default_minutes);
    cJSON_AddNumberToObject(object.get(), "standby_minutes", standby::Valid(minutes) ? minutes : standby::default_minutes);
    cJSON_AddBoolToObject(object.get(),"auto_update",pocket_update::AutomaticChecks());
    cJSON_AddStringToObject(object.get(), "update_url", pocket_update::Source().c_str());
    cJSON_AddItemToObject(object.get(), "voice_controls", voice_control::Load().release());
    cJSON_AddTrueToObject(object.get(), "separate_model_settings");
    cJSON_AddBoolToObject(object.get(), "voice_configured", profile.Configured());
    cJSON_AddStringToObject(object.get(), "voice_mode", profile.Custom() ? "custom" : profile.default_selected ? "default" : "none");
    profile.AddModelSettings(object.get());
    cJSON_AddStringToObject(object.get(), "url", profile.url.c_str());
    cJSON_AddBoolToObject(object.get(), "has_token", !profile.token.empty());
    std::vector<std::string> names;
    if (!VoiceProfile::ReadWifiNames(names)) { body="读取网络列表失败"; return 500; }
    auto* list = cJSON_AddArrayToObject(object.get(), "saved_wifi");
    if (!list) { body="生成设备设置时内存不足"; return 503; }
    for (const auto& name : names) cJSON_AddItemToArray(list, cJSON_CreateString(name.c_str()));
    cJSON_AddBoolToObject(object.get(), "has_wifi", !names.empty());
    std::unique_ptr<char, decltype(&cJSON_free)> data(cJSON_PrintUnformatted(object.get()), cJSON_free);
    if (!data) { body="生成设备设置时内存不足"; return 503; }
    body.assign(data.get());
    return 200;
}

void MarkProfileRead(httpd_handle_t server, std::string browser) {
    std::lock_guard<std::mutex> guard(policy_mutex);
    if (lan_mode == FirstConnection) {
        Settings("omni_ui",true).SetInt("lan_background",FirstConnectionUsed);
        lan_mode = FirstConnectionUsed;
        if (server) {
            first_server = server;
            first_browser = std::move(browser);
            first_deadline = esp_timer_get_time() + presence_grace_us;
        }
    }
}

esp_err_t Profile(httpd_req_t* req) {
    if (!SetupRequestAllowed(req)) return ESP_OK;
    ManagementRequest activity(req);
    std::string body;
    int code = BuildProfile(body);
    if (code != 200) {
        httpd_resp_set_status(req, code == 503 ? "503 Service Unavailable" : "500 Internal Server Error");
        return Reply(req, "text/plain; charset=utf-8", body.c_str());
    }
    auto browser = req->user_ctx ? BrowserKey(req) : std::string();
    auto result = Reply(req, "application/json", body.c_str());
    if (result == ESP_OK) MarkProfileRead(req->user_ctx ? req->handle : nullptr, std::move(browser));
    return result;
}

int StoreVoiceControls(const cJSON* data, std::string& result) {
    if (!voice_control::Save(cJSON_GetObjectItemCaseSensitive(data, "policy"))) {
        result = "无法保存，请检查重复口令、长度和设置空间";
        return 400;
    }
    result = "语音功能已保存";
    return 200;
}

esp_err_t Save(httpd_req_t* req) {
    if (!SetupRequestAllowed(req)) return ESP_OK;
    ManagementRequest activity(req);
    char type[64];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK ||
        strcmp(type, "application/json") != 0 || !req->content_len || req->content_len > 25000) {
        return Bad(req, "设置格式或长度不正确");
    }
    std::string body(req->content_len, '\0');
    size_t pos = 0;
    while (pos < body.size()) {
        int got = httpd_req_recv(req, body.data() + pos, body.size() - pos);
        if (got <= 0) return ESP_FAIL;
        pos += got;
    }
    Json data(cJSON_ParseWithOpts(body.c_str(), nullptr, true), cJSON_Delete);
    if (!cJSON_IsObject(data.get())) return Bad(req, "无法读取设置");
    auto get = [&](const char* key) -> std::string {
        auto item = cJSON_GetObjectItemCaseSensitive(data.get(), key);
        return cJSON_IsString(item) ? item->valuestring : "";
    };
    if (auto result = pocket_library::Handle(data.get())) {
        if (!result->ok) return Bad(req, result->body.c_str());
        return Reply(req, result->json ? "application/json" : "text/plain; charset=utf-8", result->body.c_str());
    }
    if (get("action") == "auto_update") {
        auto enabled = cJSON_GetObjectItemCaseSensitive(data.get(),"enabled");
        if (!cJSON_IsBool(enabled)) return Bad(req,"请选择是否自动检查");
        pocket_update::SetAutomaticChecks(cJSON_IsTrue(enabled));
        return Reply(req,"text/plain; charset=utf-8","更新设置已保存");
    }
    if (get("action") == "update_source") {
        if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(data.get(), "url"))) return Bad(req, "缺少更新地址");
        auto url = get("url");
        if (!url.empty() && !pocket_update::ValidSource(url)) return Bad(req, "请填写 HTTPS 发布清单网址");
        Settings("omni_ui", true).SetString("update_url", url);
        return Reply(req, "text/plain; charset=utf-8", "更新地址已保存");
    }
    if (get("action") == "wake") {
        auto value = cJSON_GetObjectItemCaseSensitive(data.get(), "enabled");
        if (!cJSON_IsBool(value)) return Bad(req, "请选择语音唤醒状态");
        SetPocketWakeEnabled(cJSON_IsTrue(value));
        return Reply(req, "text/plain; charset=utf-8", "语音唤醒设置已保存");
    }
    if (get("action") == "bluetooth") {
        auto value=cJSON_GetObjectItemCaseSensitive(data.get(),"enabled");
        if (!cJSON_IsBool(value)) return Bad(req,"蓝牙开关参数无效");
        if (!SetSetupBluetoothEnabled(cJSON_IsTrue(value)))
            return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"蓝牙切换失败");
        return Reply(req,"text/plain; charset=utf-8",cJSON_IsTrue(value) ? "蓝牙已开启" : "蓝牙已关闭");
    }
    if (get("action") == "standby") {
        auto minutes = cJSON_GetObjectItemCaseSensitive(data.get(), "minutes");
        if (!cJSON_IsNumber(minutes) || minutes->valuedouble != minutes->valueint || !standby::Valid(minutes->valueint))
            return Bad(req, "请选择列表中的待机时间");
        Settings("omni_ui", true).SetInt("standby_min", minutes->valueint);
        standby::configured_minutes = minutes->valueint;
        return Reply(req, "text/plain; charset=utf-8", "待机时间已保存");
    }
    if (get("action") == "voice_controls") {
        std::string message;
        if (StoreVoiceControls(data.get(), message) != 200) return Bad(req, message.c_str());
        return Reply(req,"text/plain; charset=utf-8",message.c_str());
    }
    if (get("action") == "offline_words") {
        std::string error;
        if (!SaveOfflineWords(get("words"), error)) return Bad(req, error.c_str());
        return Reply(req, "text/plain; charset=utf-8", "离线口令已保存");
    }
    if (req->content_len > 3072) return Bad(req, "设置过长");
    // Hotspot, LAN and Bluetooth can dispatch writes from different server tasks.
    std::lock_guard<std::mutex> guard(profile_mutex);
    auto action = get("action");
    if (action == "save_llm" || action == "save_speech") {
        if (!VoiceProfile::ApplyPendingWifi())
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "请先完成已有网络配置");
        auto current = VoiceProfile::Load();
        if (current.url.empty() || current.url != get("url")) return Bad(req, "请先保存自建网关连接");
        auto field = [&](const char* key, std::string& target) {
            auto item = cJSON_GetObjectItemCaseSensitive(data.get(), key);
            if (!cJSON_IsString(item)) return false;
            target = item->valuestring;
            return true;
        };
        bool complete = action == "save_llm" ? field("model", current.model) :
            field("asr", current.asr) && field("tts", current.tts) && field("voice", current.voice);
        if (action == "save_llm" && cJSON_HasObjectItem(data.get(),"protocol"))
            complete = complete && field("protocol",current.protocol);
        if (!complete || !current.Valid()) return Bad(req, "请检查模型名称或音色");
        if (!current.Save()) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "保存失败");
        return Reply(req, "text/plain; charset=utf-8", "已保存，下次连接语音网关时生效");
    }
    auto mode = get("mode"), ssid = get("ssid"), password = get("password");
    std::vector<std::string> names;
    if (!VoiceProfile::ReadWifiNames(names)) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "读取网络列表失败");
    if (get("action") == "forget_wifi") {
        if (ssid.empty() || ssid.size() > 32) return Bad(req, "请选择要移除的网络");
        if (!VoiceProfile::ForgetWifi(ssid)) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "移除失败");
        return Reply(req, "text/plain; charset=utf-8", "已移除网络");
    }
    if (!ssid.empty() && names.size() >= 10 && std::find(names.begin(), names.end(), ssid) == names.end())
        return Bad(req, "已保存 10 个网络，请先移除一个");
    if ((ssid.empty() && (names.empty() || !password.empty())) ||
        ssid.size() > 32 || (!password.empty() && (password.size() < 8 || password.size() > 63))) {
        return Bad(req, "请检查 Wi-Fi 名称和密码");
    }
    if (get("action") == "add_wifi") {
        if (ssid.empty()) return Bad(req, "请填写网络名称");
        if (!VoiceProfile::Load().Save(ssid, password) || !VoiceProfile::ApplyPendingWifi())
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "保存网络失败");
        return Reply(req, "text/plain; charset=utf-8", "网络已保存，可继续添加");
    }
    VoiceProfile profile;
    if (mode == "custom") {
        profile = {get("url"), get("token"), get("model"), get("asr"), get("tts"), get("voice")};
        auto current = VoiceProfile::Load();
        if (cJSON_HasObjectItem(data.get(),"protocol")) profile.protocol = get("protocol");
        else if (current.url == profile.url) profile.protocol = current.protocol;
        if (profile.token.empty() && profile.url == current.url) profile.token = current.token;
        if (!profile.Custom() || !profile.Valid()) return Bad(req, "请检查 WSS 地址、令牌和模型名称");
    } else if (mode == "default") {
        profile = VoiceProfile::Load();
        profile.default_selected = true;
    }
    else if (mode != "none") return Bad(req, "请选择语音服务");
    if (!profile.Save(ssid, password) || !VoiceProfile::ApplyPendingWifi()) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "保存失败");
    auto result = Reply(req, "text/plain; charset=utf-8", "已保存，设备正在重启。");
    if(req->user_ctx) xTaskNotify(static_cast<TaskHandle_t>(req->user_ctx), settings_saved, eSetBits);
    else { vTaskDelay(pdMS_TO_TICKS(700)); esp_restart(); }
    return result;
}
}

namespace {
std::atomic<bool> hotspot_ready{false}, pairing_visible{false};
std::atomic<TaskHandle_t> prompt_task{nullptr};
esp_err_t RegisterSetupRoutes(httpd_handle_t server, CustomLcdDisplay* display, bool lan, TaskHandle_t owner) {
    if (lan) {
        httpd_uri_t presence{};
        presence.uri = "/presence";
        presence.method = HTTP_GET;
        presence.handler = [](httpd_req_t* req) {
            if (!SetupRequestAllowed(req)) return ESP_OK;
            ManagementRequest activity(req);
            httpd_resp_set_status(req,"204 No Content");
            return Reply(req,"text/plain","");
        };
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server,&presence),"setup","register presence");
    }
    httpd_uri_t reset{};
    reset.uri = "/factory-reset";
    reset.method = HTTP_POST;
    reset.handler = [](httpd_req_t* req) {
        if (!SetupRequestAllowed(req)) return ESP_OK;
        char type[32];
        if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK ||
            strcmp(type, "application/json") || !req->content_len || req->content_len > 64)
            return Bad(req, "恢复请求无效");
        std::string confirmation(req->content_len, '\0'), result;
        size_t pos = 0;
        while (pos < confirmation.size()) {
            int n = httpd_req_recv(req, confirmation.data()+pos, confirmation.size()-pos);
            if (n <= 0) return ESP_FAIL;
            pos += n;
        }
        int code = RequestPocketReset(confirmation, result);
        if (code != 200) httpd_resp_set_status(req, code == 400 ? "400 Bad Request" : "503 Service Unavailable");
        return Reply(req, "text/plain; charset=utf-8", result.c_str());
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server,&reset),"setup","register reset");
    httpd_uri_t access{};
    access.uri = "/access"; access.method = HTTP_POST; access.handler = [](httpd_req_t* req) {
        if (!FirstManagementRequestAllowed(req)) return ESP_OK;
        auto result=RequestSetupAccess(req);
        if(auto task=prompt_task.load()) xTaskNotifyGive(task);
        return result;
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server,&access),"setup","register route");
    httpd_uri_t saver{};
    saver.user_ctx = display;
    saver.uri = "/saver";
    saver.handler = [](httpd_req_t* req) { return Saver(req, CustomLcdDisplay::Saver::Standby); };
    saver.method = HTTP_GET;
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &saver),"setup","register route");
    saver.method = HTTP_POST;
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &saver),"setup","register route");
    saver.uri = "/shutdown-saver";
    saver.handler = [](httpd_req_t* req) { return Saver(req, CustomLcdDisplay::Saver::Shutdown); };
    saver.method = HTTP_GET;
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &saver),"setup","register route");
    saver.method = HTTP_POST;
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &saver),"setup","register route");
    httpd_uri_t home{}, read{}, write{}, networks{}, leave{};
    leave.uri = "/exit"; leave.method = HTTP_POST; leave.user_ctx = owner;
    leave.handler = [](httpd_req_t* req) {
        if (!SetupRequestAllowed(req)) return ESP_OK;
        ManagementRequest activity(req);
        char type[32];
        if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK ||
            strcmp(type, "application/json")) return Bad(req, "退出请求格式不正确");
        auto result = Reply(req, "text/plain; charset=utf-8", "已退出设置，正在返回首页");
        if (req->user_ctx) xTaskNotify(static_cast<TaskHandle_t>(req->user_ctx), leave_setup, eSetBits);
        else if (result == ESP_OK) {
            std::lock_guard<std::mutex> guard(policy_mutex);
            if (req->handle == first_server) first_deadline = 0;
        }
        return result;
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &leave),"setup","register route");
    networks.uri = "/networks"; networks.method = HTTP_GET; networks.handler = Networks;
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &networks),"setup","register route");
    home.uri = "/"; home.method = HTTP_GET;
    home.user_ctx = lan ? display : nullptr;
    home.handler = [](httpd_req_t* req) {
        if (!req->user_ctx) return Reply(req,"text/html; charset=utf-8",page);
        const char* head = strstr(page,"<head>") + 6;
        httpd_resp_set_type(req,"text/html; charset=utf-8");
        httpd_resp_set_hdr(req,"Cache-Control","no-store");
        httpd_resp_set_hdr(req,"X-Frame-Options","DENY");
        uint8_t mac[6]; esp_read_mac(mac,ESP_MAC_WIFI_STA);
        char marker[128];
        snprintf(marker,sizeof(marker),"<meta name='quack-transport' content='lan'><meta name='quack-unit' content='%02x%02x%02x%02x%02x%02x'>",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
        if (httpd_resp_send_chunk(req,page,head-page) != ESP_OK ||
            httpd_resp_sendstr_chunk(req,marker) != ESP_OK ||
            httpd_resp_sendstr_chunk(req,head) != ESP_OK) return ESP_FAIL;
        return httpd_resp_send_chunk(req,nullptr,0);
    };
    read.uri = "/profile"; read.method = HTTP_GET; read.handler = Profile;
    write.uri = "/profile"; write.method = HTTP_POST; write.handler = Save;
    read.user_ctx = lan && !owner ? display : nullptr;
    write.user_ctx = owner;
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &home),"setup","register route");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &read),"setup","register route");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &write),"setup","register route");

    return ESP_OK;
}
// This plaintext listener serves the local GATT bridge exclusively.
esp_err_t LoopbackOnly(httpd_handle_t, int fd) {
    sockaddr_storage peer{};
    socklen_t len=sizeof(peer);
    if (getpeername(fd,reinterpret_cast<sockaddr*>(&peer),&len)) return ESP_FAIL;
    if(peer.ss_family==AF_INET)
        return reinterpret_cast<sockaddr_in*>(&peer)->sin_addr.s_addr==htonl(INADDR_LOOPBACK) ? ESP_OK : ESP_FAIL;
    if(peer.ss_family==AF_INET6) {
        const uint8_t mapped[]={0,0,0,0,0,0,0,0,0,0,255,255,127,0,0,1};
        return !memcmp(&reinterpret_cast<sockaddr_in6*>(&peer)->sin6_addr,mapped,16) ? ESP_OK : ESP_FAIL;
    }
    return ESP_FAIL;
}
}

int RequestPocketReset(const std::string& confirmation, std::string& result) {
    Json data(cJSON_ParseWithOpts(confirmation.c_str(), nullptr, true), cJSON_Delete);
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data.get(), "confirmed"))) {
        result = "请先确认恢复出厂设置";
        return 400;
    }
    static std::mutex reset_mutex;
    static bool scheduled = false;
    std::lock_guard<std::mutex> guard(reset_mutex);
    if (!scheduled) {
        esp_timer_handle_t timer;
        esp_timer_create_args_t args{};
        args.callback = [](void*) { esp_restart(); };
        args.name = "factory_reset";
        if (esp_timer_create(&args, &timer) != ESP_OK) {
            result = "无法安排重启，请重试";
            return 503;
        }
        auto err = pocket_reset::Mark("nvs");
        if (err != ESP_OK) {
            esp_timer_delete(timer);
            result = std::string("无法保存恢复请求：") + esp_err_to_name(err);
            return 503;
        }
        scheduled = true;
        if (esp_timer_start_once(timer, 2000000) != ESP_OK) esp_restart();
    }
    result = "恢复请求已接受，设备将重启。SD 卡内容会保留。";
    return 200;
}

int SavePairedVoiceControls(const std::string& body, std::string& result) {
    Json data(cJSON_ParseWithOpts(body.c_str(), nullptr, true), cJSON_Delete);
    return StoreVoiceControls(data.get(), result);
}

int ReadPairedBluetoothProfile(std::string& body) {
    int code = BuildProfile(body);
    if (code == 200) MarkProfileRead(nullptr, {});
    return code;
}

int BluetoothSetupPort(CustomLcdDisplay* display) {
    static std::mutex port_mutex;
    std::lock_guard<std::mutex> guard(port_mutex);
    if (hotspot_ready) return 80;
    static httpd_handle_t server=nullptr;
    constexpr int port=8871;
    if (!server) {
        if (esp_netif_init()!=ESP_OK) return 0;
        httpd_config_t conf=HTTPD_DEFAULT_CONFIG();
        conf.server_port=port;conf.ctrl_port+=2;conf.open_fn=LoopbackOnly;
        conf.max_uri_handlers=12;conf.max_open_sockets=1;conf.stack_size=8192;
        if(httpd_start(&server,&conf)!=ESP_OK)return 0;
        if(RegisterSetupRoutes(server,display,false,nullptr)!=ESP_OK){
            httpd_stop(server);server=nullptr;return 0;
        }
    }
    return port;
}

void ShowBluetoothPairing(bool visible) {
    pairing_visible=visible;
    if(auto task=prompt_task.load()) xTaskNotifyGive(task);
}

void InitializeBluetoothPrompts(CustomLcdDisplay* display) {
    TaskHandle_t handle=nullptr;
    auto run=[](void* argument) {
        auto* display=static_cast<CustomLcdDisplay*>(argument);
        std::string rendered;
        for(;;){
            std::string message,ticket;
            if(pairing_visible){
                char code[7];snprintf(code,sizeof(code),"%06u",SetupBluetoothPasskey());
                message=std::string("蓝牙配对\n\n请在手机输入\n")+code;
            } else if(!PhoneSetupActive()){
                auto prompt=PendingSetupPrompt();
                if(!prompt.id.empty()){
                    ticket=prompt.id+prompt.code;
                    message="允许管理设备？\n"+prompt.name+"\n核对码 "+prompt.code+"\n确认键 同意 · 电源键 拒绝";
                }
            }
            if(message!=rendered){
                display->ShowBluetoothPrompt(message);
                if(!PhoneSetupActive()){
                    std::lock_guard<std::mutex> guard(prompt_mutex);
                    shown_ticket=ticket;
                }
                rendered=message;
            }
            ulTaskNotifyTake(pdTRUE, message.empty() ? portMAX_DELAY : pdMS_TO_TICKS(350));
        }
    };
    if(xTaskCreate(run,"bt_prompt",4096,display,3,&handle)==pdPASS) prompt_task=handle;
}

void EnableOfflineSetupScan() { offline_home=true; }

bool PhoneSetupActive() { return setup_task.load() != nullptr; }

bool ShowPhoneSetupIfActive(bool approve) {
    auto task = setup_task.load();
    std::lock_guard<std::mutex> guard(prompt_mutex);
    if (!task && shown_ticket.empty()) return false;
    if (!shown_ticket.empty()) {
        AnswerSetupPrompt(shown_ticket, approve);
        shown_ticket.clear();
    } else if (!approve && PendingSetupPrompt().id.empty()) {
        xTaskNotify(task, leave_setup, eSetBits);
    }
    if (task) xTaskNotify(task, show_setup, eSetBits);
    if(auto handler=prompt_task.load()) xTaskNotifyGive(handler);
    return true;
}

namespace {
esp_err_t OpenLanManagement(CustomLcdDisplay* display, TaskHandle_t owner, std::string& hostname,
                            std::string& fingerprint, httpd_handle_t& server) {
    uint8_t mac[6];
    esp_read_mac(mac,ESP_MAC_WIFI_STA);
    char name[24];
    snprintf(name,sizeof(name),"quack-%02x%02x%02x",mac[3],mac[4],mac[5]);
    hostname=std::string(name)+".local";
    auto result=StartLanSetupServer(&server,fingerprint,hostname);
    if (result!=ESP_OK) return result;
    result=RegisterSetupRoutes(server,display,true,owner);
    if (result!=ESP_OK) {
        httpd_ssl_stop(server);
        server=nullptr;
        return result;
    }
    if (mdns_init()!=ESP_OK || mdns_hostname_set(name)!=ESP_OK) hostname.clear();
    else mdns_service_add("Quack Pocket","_https","_tcp",443,nullptr,0);
    return ESP_OK;
}
}
bool ManagementInUse() {
    if (SetupBluetoothConnected() || PhoneSetupActive()) return true;
    std::lock_guard<std::mutex> lock(activity_mutex);
    return active_requests || esp_timer_get_time() < awake_until_us;
}

bool FirstManagementRequestAllowed(httpd_req_t* req) {
    std::lock_guard<std::mutex> guard(policy_mutex);
    if (req->handle != first_server ||
        (first_deadline && esp_timer_get_time() < first_deadline && BrowserKey(req) == first_browser)) return true;
    httpd_resp_send_err(req,HTTPD_403_FORBIDDEN,"首次管理已结束，请在设备上开启后台设置服务");
    return false;
}

void InitializeBackgroundManagement() {
    std::lock_guard<std::mutex> guard(policy_mutex);
    Settings settings("omni_ui",true);
    int saved = settings.GetInt("lan_background",-1);
    if (saved < 0) {
        lan_mode = SetupPeers().empty() ? FirstConnection : FirstConnectionUsed;
        settings.SetInt("lan_background",lan_mode);
    } else lan_mode = static_cast<LanMode>(saved);
}

bool BackgroundManagementEnabled() {
    std::lock_guard<std::mutex> guard(policy_mutex);
    return lan_mode == ManualOn || lan_mode == FirstConnection;
}

namespace {
std::mutex lan_mutex;
httpd_handle_t background_server = nullptr;
bool ConfigureLanService(CustomLcdDisplay* display, bool enable) {
    if (enable && !background_server && WifiStation::GetInstance().IsConnected()) {
        std::string hostname,fingerprint;
        httpd_handle_t server = nullptr;
        if (OpenLanManagement(display,nullptr,hostname,fingerprint,server)!=ESP_OK) return false;
        background_server = server;
    } else if (!enable && background_server) {
        if (httpd_ssl_stop(background_server)!=ESP_OK) return false;
        background_server = nullptr;
        {
            std::lock_guard<std::mutex> policy(policy_mutex);
            first_server = nullptr;
            first_browser.clear();
        }
        mdns_service_remove("_https","_tcp");
    }
    return true;
}
}
bool ApplyBackgroundManagement(CustomLcdDisplay* display, bool enable) {
    std::lock_guard<std::mutex> lock(lan_mutex);
    if (!ConfigureLanService(display,enable)) return false;
    std::lock_guard<std::mutex> policy(policy_mutex);
    lan_mode = enable ? ManualOn : ManualOff;
    first_server = nullptr;
    first_browser.clear();
    Settings("omni_ui",true).SetInt("lan_background",lan_mode);
    return true;
}
void FinishFirstManagement() {
    std::lock_guard<std::mutex> lock(lan_mutex);
    if (!background_server) return;
    {
        std::lock_guard<std::mutex> policy(policy_mutex);
        if (lan_mode != FirstConnectionUsed || (first_server && esp_timer_get_time() < first_deadline)) return;
    }
    {
        std::lock_guard<std::mutex> activity(activity_mutex);
        if (active_requests) return;
    }
    ConfigureLanService(nullptr,false);
}
void StartBackgroundManagement(CustomLcdDisplay* display) {
    std::lock_guard<std::mutex> lock(lan_mutex);
    if (!ConfigureLanService(display,BackgroundManagementEnabled()))
        ESP_LOGE("setup","Cannot start network management listener");
}

void RunPhoneSetup(CustomLcdDisplay* display, bool lan) {
    httpd_handle_t server = nullptr;
    std::string fingerprint, hostname;
    wifi_config_t config{};
    char password[9]{};
    if (lan) {
        if (OpenLanManagement(display,xTaskGetCurrentTaskHandle(),hostname,fingerprint,server)!=ESP_OK) {
            display->SetChatMessage("system","同网管理启动失败，正在返回首页");
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        }
    } else {
        ESP_ERROR_CHECK(esp_netif_init());
        auto* net = esp_netif_create_default_wifi_ap();
        assert(net);
        wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&init));
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        uint8_t mac[6];
        ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
        snprintf(reinterpret_cast<char*>(config.ap.ssid), sizeof(config.ap.ssid), "Omni-%02X%02X", mac[4], mac[5]);
        uint32_t random;
        do { random = esp_random(); } while (random >= 4200000000u);
        snprintf(password, sizeof(password), "%08u", unsigned(random % 100000000u));
        memcpy(config.ap.password, password, sizeof(password));
        config.ap.channel = 1;
        config.ap.authmode = WIFI_AUTH_WPA2_PSK;
        config.ap.max_connection = 1;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
        ESP_ERROR_CHECK(esp_wifi_start());

        httpd_config_t settings = HTTPD_DEFAULT_CONFIG();
        settings.max_uri_handlers = 12;
        settings.stack_size = 8192;
        settings.lru_purge_enable = true;
        ESP_ERROR_CHECK(httpd_start(&server, &settings));
    }
    if (!lan) ESP_ERROR_CHECK(RegisterSetupRoutes(server,display,false,xTaskGetCurrentTaskHandle()));
    if (!lan) hotspot_ready=true;

    setup_task.store(xTaskGetCurrentTaskHandle());
    std::string rendered = "initial";
    uint32_t event = 0;
    do {
        auto prompt = PendingSetupPrompt();
        auto ticket = prompt.id + prompt.code;
        auto address = lan ? WifiStation::GetInstance().GetIpAddress() : "";
        const unsigned pairing_code=SetupBluetoothEnabled() ? SetupBluetoothPasskey() : 1000000;
        auto view = ticket + "/" + address + std::to_string(pairing_code);
        if (view != rendered) {
            { std::lock_guard<std::mutex> guard(prompt_mutex); shown_ticket.clear(); }
            if (!ticket.empty()) {
                std::string message = "允许管理设备？\n" + prompt.name + "\n核对码 " + prompt.code +
                                      "\n确认键 同意 · 电源键 拒绝";
                display->SetChatMessage("system","");
                display->ShowNetworkPage(message.c_str());
            } else {
                display->ShowNetworkPage(nullptr);
                display->SetChatMessage("system","");
                display->SetStatus("手机设置");
                display->ShowSetupCode(reinterpret_cast<const char*>(config.ap.ssid), password, pairing_code,
                                       lan ? "https://" + (hostname.empty() ? address : hostname) + "/" : "", fingerprint, address);
            }
            { std::lock_guard<std::mutex> guard(prompt_mutex); shown_ticket=ticket; }
            rendered=view;
        }
        xTaskNotifyWait(0, UINT32_MAX, &event, pdMS_TO_TICKS(350));
    } while (!(event & (settings_saved | leave_setup)));
    display->ShowNetworkPage(nullptr);
    display->SetChatMessage("system", (event & settings_saved) ? "设置已保存，正在重启联网" : "正在返回首页");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}
