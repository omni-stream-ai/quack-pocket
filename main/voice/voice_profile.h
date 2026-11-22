#pragma once
#include <string>
#include <vector>
#include <cJSON.h>

struct VoiceProfile {
    std::string url, token, model, asr, tts, voice;
    bool Custom() const { return !url.empty(); }
    bool Valid() const;
    bool Save(const std::string& ssid = {}, const std::string& password = {}) const;
    static bool ApplyPendingWifi();
    static VoiceProfile Load();
    static bool ReadWifiNames(std::vector<std::string>& names);
    static bool ForgetWifi(const std::string& ssid);
    bool AddModelSettings(cJSON* object) const;
};
