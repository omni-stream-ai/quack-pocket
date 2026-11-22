#include "voice_profile.h"
#include "nvs.h"
#include <cassert>
#include <cstring>
#include <map>

using Entries = std::map<std::string, std::string>;
static Entries spaces[2];
static int failure, operations;

int nvs_open(const char* name, int, nvs_handle_t* handle) {
    *handle = strcmp(name, "wifi") == 0;
    return ESP_OK;
}
int nvs_get_str(int h, const char* key, char* value, size_t* size) {
    auto it = spaces[h].find(key);
    if (it == spaces[h].end()) return ESP_ERR_NVS_NOT_FOUND;
    size_t needed = it->second.size() + 1;
    if (value && *size < needed) return ESP_ERR_INVALID_SIZE;
    if (value) memcpy(value, it->second.c_str(), needed);
    *size = needed;
    return ESP_OK;
}
int nvs_set_str(int h, const char* key, const char* value) {
    if (++operations == failure) return ESP_FAIL;
    spaces[h][key] = value;
    return ESP_OK;
}
int nvs_erase_key(int h, const char* key) { return spaces[h].erase(key) ? ESP_OK : ESP_ERR_NVS_NOT_FOUND; }
int nvs_commit(int) { return ++operations == failure ? ESP_FAIL : ESP_OK; }
void nvs_close(int) {}

int main() {
    VoiceProfile profile{"wss://example.com", "device-token", "chat", "asr", "tts", "voice"};
    for (int interruption = 1; interruption <= 5; ++interruption) {
        spaces[0].clear();
        spaces[1] = {{"ssid", "previous"}, {"password", "previous-pass"}};
        failure = 0;
        assert(profile.Save("office", "office-pass"));
        operations = 0;
        failure = interruption;
        assert(!VoiceProfile::ApplyPendingWifi());
        failure = 0;
        assert(VoiceProfile::ApplyPendingWifi());
        assert(spaces[1].at("ssid1") == "office" && spaces[1].at("password1") == "office-pass");
        assert(spaces[1].at("ssid") == "previous" && spaces[1].at("password") == "previous-pass");
        assert(VoiceProfile::Load().model == "chat");
        assert(spaces[0].at("profile").find("office-pass") == std::string::npos);
        operations = 0;
        assert(VoiceProfile::ApplyPendingWifi() && operations == 0);
    }
    assert(profile.Save("office", "changed-pass") && VoiceProfile::ApplyPendingWifi());
    assert(spaces[1].size() == 4 && spaces[1].at("password1") == "changed-pass");
    for (int i = 2; i < 10; ++i) {
        assert(profile.Save("network" + std::to_string(i), "password") && VoiceProfile::ApplyPendingWifi());
    }
    auto original = spaces[1];
    assert(!profile.Save("eleventh", "pass") && VoiceProfile::ApplyPendingWifi());
    assert(original == spaces[1]);
    assert(profile.Save("office", "updated") && VoiceProfile::ApplyPendingWifi());
    assert(VoiceProfile::ForgetWifi("office"));
    assert(profile.Save("replacement", "new-pass") && VoiceProfile::ApplyPendingWifi());
    std::vector<std::string> names;
    assert(VoiceProfile::ReadWifiNames(names) && names.size() == 10);
    assert(names[1] == "replacement" && names[0] == "previous");
}
