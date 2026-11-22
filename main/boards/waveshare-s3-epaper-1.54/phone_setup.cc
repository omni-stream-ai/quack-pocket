#include "phone_setup.h"
#include "setup_ble.h"
#include "offline_commands.h"

#include <esp_http_server.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_wifi.h>
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

namespace {
std::atomic<TaskHandle_t> setup_task{nullptr};
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

esp_err_t Saver(httpd_req_t* req) {
    auto display = static_cast<CustomLcdDisplay*>(req->user_ctx);
    std::vector<uint8_t> pixels(CustomLcdDisplay::saver_bytes);
    if (req->method == HTTP_GET) {
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        if (!display->ReadSaver(pixels.data())) {
            httpd_resp_set_status(req, "204 No Content");
            return httpd_resp_send(req, nullptr, 0);
        }
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
    if (!display->SaveSaver(received ? pixels.data() : nullptr))
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "屏保保存失败");
    return Reply(req, "text/plain; charset=utf-8", received ? "屏保已保存" : "屏保已关闭");
}

esp_err_t Networks(httpd_req_t* req) {
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

esp_err_t Profile(httpd_req_t* req) {
    auto profile = VoiceProfile::Load();
    Json object(cJSON_CreateObject(), cJSON_Delete);
    if (!object) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(object.get(), "offline_words", ReadOfflineWords().c_str());
    profile.AddModelSettings(object.get());
    cJSON_AddStringToObject(object.get(), "url", profile.url.c_str());
    cJSON_AddBoolToObject(object.get(), "has_token", !profile.token.empty());
    std::vector<std::string> names;
    if (!VoiceProfile::ReadWifiNames(names)) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "读取网络列表失败");
    auto* list = cJSON_AddArrayToObject(object.get(), "saved_wifi");
    if (!list) return ESP_ERR_NO_MEM;
    for (const auto& name : names) cJSON_AddItemToArray(list, cJSON_CreateString(name.c_str()));
    cJSON_AddBoolToObject(object.get(), "has_wifi", !names.empty());
    std::unique_ptr<char, decltype(&cJSON_free)> data(cJSON_PrintUnformatted(object.get()), cJSON_free);
    return data ? Reply(req, "application/json", data.get()) : ESP_ERR_NO_MEM;
}

esp_err_t Save(httpd_req_t* req) {
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
    if (get("action") == "offline_words") {
        std::string error;
        if (!SaveOfflineWords(get("words"), error)) return Bad(req, error.c_str());
        return Reply(req, "text/plain; charset=utf-8", "离线口令已保存");
    }
    if (req->content_len > 3072) return Bad(req, "设置过长");
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
        if (profile.token.empty() && profile.url == current.url) profile.token = current.token;
        if (!profile.Custom() || !profile.Valid()) return Bad(req, "请检查 WSS 地址、令牌和模型名称");
    } else if (mode != "default") return Bad(req, "请选择语音服务");
    if (!profile.Save(ssid, password) || !VoiceProfile::ApplyPendingWifi()) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "保存失败");
    auto result = Reply(req, "text/plain; charset=utf-8", "已保存，设备正在重启。");
    xTaskNotify(static_cast<TaskHandle_t>(req->user_ctx), settings_saved, eSetBits);
    return result;
}
}

void RebootToLocalMode() {
    {
        Settings settings("omni_mode", true);
        settings.SetInt("offline", 1);
    }
    esp_restart();
}

bool ShowPhoneSetupIfActive() {
    auto task = setup_task.load();
    if (!task) return false;
    xTaskNotify(task, show_setup, eSetBits);
    return true;
}

void RunPhoneSetup(CustomLcdDisplay* display) {
    ESP_ERROR_CHECK(esp_netif_init());
    auto* net = esp_netif_create_default_wifi_ap();
    assert(net);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    wifi_config_t config{};
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
    snprintf(reinterpret_cast<char*>(config.ap.ssid), sizeof(config.ap.ssid), "Omni-%02X%02X", mac[4], mac[5]);
    char password[9];
    uint32_t random;
    do { random = esp_random(); } while (random >= 4200000000u);
    snprintf(password, sizeof(password), "%08u", unsigned(random % 100000000u));
    memcpy(config.ap.password, password, sizeof(password));
    config.ap.channel = 1;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.max_connection = 1;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
    ESP_ERROR_CHECK(esp_wifi_start());

    httpd_handle_t server = nullptr;
    httpd_config_t settings = HTTPD_DEFAULT_CONFIG();
    settings.stack_size = 8192;
    settings.lru_purge_enable = true;
    ESP_ERROR_CHECK(httpd_start(&server, &settings));
    httpd_uri_t saver{};
    saver.uri = "/saver"; saver.handler = Saver; saver.user_ctx = display;
    saver.method = HTTP_GET;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &saver));
    saver.method = HTTP_POST;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &saver));
    httpd_uri_t home{}, read{}, write{}, networks{}, leave{};
    leave.uri = "/exit"; leave.method = HTTP_POST; leave.user_ctx = xTaskGetCurrentTaskHandle();
    leave.handler = [](httpd_req_t* req) {
        char type[32];
        if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK ||
            strcmp(type, "application/json")) return Bad(req, "退出请求格式不正确");
        auto result = Reply(req, "text/plain; charset=utf-8", "已退出设置，正在返回首页");
        xTaskNotify(static_cast<TaskHandle_t>(req->user_ctx), leave_setup, eSetBits);
        return result;
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &leave));
    networks.uri = "/networks"; networks.method = HTTP_GET; networks.handler = Networks;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &networks));
    home.uri = "/"; home.method = HTTP_GET;
    home.handler = [](httpd_req_t* req) { return Reply(req, "text/html; charset=utf-8", page); };
    read.uri = "/profile"; read.method = HTTP_GET; read.handler = Profile;
    write.uri = "/profile"; write.method = HTTP_POST; write.handler = Save;
    write.user_ctx = xTaskGetCurrentTaskHandle();
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &home));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &read));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &write));

    const unsigned pairing_code = esp_random() % 1000000;
    StartSetupBluetooth(reinterpret_cast<const char*>(config.ap.ssid), pairing_code);
    setup_task.store(xTaskGetCurrentTaskHandle());
    uint32_t event = 0;
    do {
        display->SetStatus("手机设置");
        display->ShowSetupCode(reinterpret_cast<const char*>(config.ap.ssid), password, pairing_code);
        xTaskNotifyWait(0, UINT32_MAX, &event, portMAX_DELAY);
    } while (!(event & (settings_saved | leave_setup)));
    if (!(event & settings_saved)) {
        vTaskDelay(pdMS_TO_TICKS(500));
        RebootToLocalMode();
    }
    { Settings mode("omni_mode", true); mode.SetInt("offline", 0); }
    display->SetChatMessage("system", "设置已保存，正在重启联网");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}
