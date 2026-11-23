#pragma once
#include <string>
#include <vector>
#include <cJSON.h>

struct VoiceProfile {
    std::string url, token, model, asr, tts, voice;
    std::string protocol = "chat_completions";
    bool default_selected = false;
    bool Configured() const { return Custom() || default_selected; }
    bool Custom() const { return !default_selected && !url.empty(); }
    bool Valid() const;
    bool Save(const std::string& ssid = {}, const std::string& password = {}) const;
    static bool ApplyPendingWifi();
    static VoiceProfile Load();
    static bool ReadWifiNames(std::vector<std::string>& names);
    static bool ForgetWifi(const std::string& ssid);
    bool AddModelSettings(cJSON* object) const;
};
