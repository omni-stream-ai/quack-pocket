#include "voice_profile.h"
#include <nvs.h>
#include <memory>
#include <algorithm>

namespace {
bool printable(const std::string& value, size_t maximum) {
    if (value.size() > maximum) return false;
    for (unsigned char c : value) if (c < 33 || c == 127) return false;
    return true;
}

std::string string_value(cJSON* object, const char* key) {
    auto item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) ? item->valuestring : "";
}
}

bool VoiceProfile::Valid() const {
    if (url.empty()) return token.empty() && model.empty() && asr.empty() && tts.empty() && voice.empty();
    if (url.rfind("wss://", 0) != 0) return false;
    auto host_end = url.find_first_of("/?#", 6);
    auto host = url.substr(6, host_end == std::string::npos ? host_end : host_end - 6);
    return printable(url, 384) && !host.empty() &&
        host.find('@') == std::string::npos && url.find('#') == std::string::npos &&
        !token.empty() && printable(token, 512) && !model.empty() && printable(model, 96) &&
        printable(asr, 96) && printable(tts, 96) && printable(voice, 64);
}

bool VoiceProfile::AddModelSettings(cJSON* object) const {
    return cJSON_AddStringToObject(object, "model", model.c_str()) &&
        cJSON_AddStringToObject(object, "asr", asr.c_str()) &&
        cJSON_AddStringToObject(object, "tts", tts.c_str()) &&
        cJSON_AddStringToObject(object, "voice", voice.c_str());
}

bool VoiceProfile::Save(const std::string& ssid, const std::string& password) const {
    if (!Valid()) return false;
    if (!ssid.empty()) {
        std::vector<std::string> names;
        if (!ReadWifiNames(names) ||
            (names.size() >= 10 && std::find(names.begin(), names.end(), ssid) == names.end())) return false;
    }
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(cJSON_CreateObject(), cJSON_Delete);
    if (!json) return false;
    if (!AddModelSettings(json.get()) || !cJSON_AddStringToObject(json.get(), "url", url.c_str()) ||
        !cJSON_AddStringToObject(json.get(), "token", token.c_str())) return false;
    if (!ssid.empty() && (!cJSON_AddStringToObject(json.get(), "wifi_ssid", ssid.c_str()) ||
        !cJSON_AddStringToObject(json.get(), "wifi_password", password.c_str()))) return false;
    std::unique_ptr<char, decltype(&cJSON_free)> data(cJSON_PrintUnformatted(json.get()), cJSON_free);
    nvs_handle_t nvs;
    if (!data || nvs_open("omni_voice", NVS_READWRITE, &nvs) != ESP_OK) return false;
    bool ok = nvs_set_str(nvs, "profile", data.get()) == ESP_OK && nvs_commit(nvs) == ESP_OK;
    nvs_close(nvs);
    return ok;
}

namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

esp_err_t ReadProfile(Json& json) {
    nvs_handle_t nvs;
    auto err = nvs_open("omni_voice", NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;
    size_t size = 0;
    err = nvs_get_str(nvs, "profile", nullptr, &size);
    std::string data;
    if (err == ESP_OK) {
        if (!size || size > 4000) err = ESP_ERR_INVALID_SIZE;
        else {
            data.resize(size);
            err = nvs_get_str(nvs, "profile", data.data(), &size);
        }
    }
    nvs_close(nvs);
    if (err != ESP_OK) return err;
    json.reset(cJSON_Parse(data.c_str()));
    return cJSON_IsObject(json.get()) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

VoiceProfile Decode(cJSON* json) {
    return {string_value(json,"url"), string_value(json,"token"), string_value(json,"model"),
            string_value(json,"asr"), string_value(json,"tts"), string_value(json,"voice")};
}
}

VoiceProfile VoiceProfile::Load() {
    Json json(nullptr, cJSON_Delete);
    if (ReadProfile(json) != ESP_OK) return {};
    auto profile = Decode(json.get());
    return profile.Valid() ? profile : VoiceProfile{};
}

bool VoiceProfile::ApplyPendingWifi() {
    Json json(nullptr, cJSON_Delete);
    auto err = ReadProfile(json);
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;
    if (err != ESP_OK) return false;
    auto ssid = string_value(json.get(), "wifi_ssid");
    if (ssid.empty()) return true;
    auto password = string_value(json.get(), "wifi_password");
    auto profile = Decode(json.get());
    if (!profile.Valid()) return false;

    // Mirror into the ten slots read by esp-wifi-connect before its singleton starts.
    // The profile retains the pending pair until both keys are committed.
    nvs_handle_t wifi;
    if (nvs_open("wifi", NVS_READWRITE, &wifi) != ESP_OK) return false;
    int slot = -1;
    for (int i = 0; i < 10; ++i) {
        auto suffix = i ? std::to_string(i) : "";
        char existing[33];
        size_t length = sizeof(existing);
        err = nvs_get_str(wifi, ("ssid" + suffix).c_str(), existing, &length);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            if (slot < 0) slot = i;
        } else if (err != ESP_OK) {
            nvs_close(wifi);
            return false;
        } else if (ssid == existing) {
            slot = i;
            break;
        }
    }
    if (slot < 0) { nvs_close(wifi); return false; }
    auto suffix = slot ? std::to_string(slot) : "";
    bool ok = nvs_set_str(wifi, ("password" + suffix).c_str(), password.c_str()) == ESP_OK &&
              nvs_set_str(wifi, ("ssid" + suffix).c_str(), ssid.c_str()) == ESP_OK &&
              nvs_commit(wifi) == ESP_OK;
    nvs_close(wifi);
    return ok && profile.Save();
}

namespace {
bool ReadWifiSlot(nvs_handle_t wifi, int slot, std::string& name) {
    char value[33];
    size_t size = sizeof(value);
    auto suffix = slot ? std::to_string(slot) : "";
    auto err = nvs_get_str(wifi, ("ssid" + suffix).c_str(), value, &size);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return false;
    name = err == ESP_OK ? value : "";
    return true;
}
}

bool VoiceProfile::ReadWifiNames(std::vector<std::string>& names) {
    names.clear();
    nvs_handle_t wifi;
    auto err = nvs_open("wifi", NVS_READONLY, &wifi);
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;
    if (err != ESP_OK) return false;
    bool ok = true;
    for (int slot = 0; slot < 10; ++slot) {
        std::string name;
        if (!ReadWifiSlot(wifi, slot, name)) { ok = false; break; }
        if (!name.empty()) names.push_back(name);
    }
    nvs_close(wifi);
    return ok;
}

bool VoiceProfile::ForgetWifi(const std::string& ssid) {
    if (ssid.empty()) return false;
    nvs_handle_t wifi;
    auto err = nvs_open("wifi", NVS_READWRITE, &wifi);
    if (err != ESP_OK) return false;
    for (int slot = 0; slot < 10; ++slot) {
        std::string name;
        if (!ReadWifiSlot(wifi, slot, name)) { nvs_close(wifi); return false; }
        if (name != ssid) continue;
        auto suffix = slot ? std::to_string(slot) : "";
        err = nvs_erase_key(wifi, ("ssid" + suffix).c_str());
        if (err == ESP_OK) err = nvs_erase_key(wifi, ("password" + suffix).c_str());
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        if (err == ESP_OK) err = nvs_commit(wifi);
        break;
    }
    nvs_close(wifi);
    return err == ESP_OK;
}
