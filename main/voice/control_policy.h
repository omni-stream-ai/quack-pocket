#pragma once

#include <string>
#include <cstring>
#include <memory>
#include <set>
#include "cJSON.h"
#include "nvs.h"

namespace voice_control {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

inline Json Load() {
    nvs_handle_t db;
    std::string text;
    if (nvs_open_from_partition("omni_ui", "voice_controls", NVS_READONLY, &db) == ESP_OK) {
        size_t n = 0;
        if (nvs_get_blob(db, "policy", nullptr, &n) == ESP_OK && n <= 4096) {
            text.resize(n);
            if (nvs_get_blob(db, "policy", text.data(), &n) != ESP_OK) text.clear();
        }
        nvs_close(db);
    }
    Json policy(cJSON_Parse(text.c_str()), cJSON_Delete);
    if (!policy) {
        policy.reset(cJSON_CreateObject());
        for (auto key : {"music","volume","state"}) cJSON_AddBoolToObject(policy.get(), key, true);
        cJSON_AddArrayToObject(policy.get(), "skills");
    }
    return policy;
}

inline bool Save(const cJSON* policy) {
    for (auto key : {"music","volume","state"})
        if (!cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(policy, key))) return false;
    auto skills = cJSON_GetObjectItemCaseSensitive(policy, "skills");
    if (!cJSON_IsArray(skills) || cJSON_GetArraySize(skills) > 24) return false;
    std::set<std::string> phrases;
    cJSON* skill;
    cJSON_ArrayForEach(skill, skills) {
        auto phrase = cJSON_GetObjectItemCaseSensitive(skill, "phrase");
        auto action = cJSON_GetObjectItemCaseSensitive(skill, "action");
        if (!cJSON_IsString(phrase) || !cJSON_IsString(action)) return false;
        std::string name = phrase->valuestring, value = action->valuestring;
        auto arg = cJSON_GetObjectItemCaseSensitive(skill,"argument");
        if (arg) {
            if (value == "music") {
                if (!cJSON_IsString(arg) || !arg->valuestring[0] || strlen(arg->valuestring)>192) return false;
            } else if (value == "volume") {
                if (!cJSON_IsNumber(arg) || arg->valuedouble != arg->valueint ||
                    arg->valueint < 0 || arg->valueint > 100) return false;
            } else return false;
        }
        if (name.empty() || name.size() > 96 || !phrases.insert(name).second ||
            (value != "music" && value != "volume" && value != "state")) return false;
    }
    std::unique_ptr<char, decltype(&cJSON_free)> data(cJSON_PrintUnformatted(policy), cJSON_free);
    if (!data || strlen(data.get()) > 4096) return false;
    nvs_handle_t db;
    if (nvs_open_from_partition("omni_ui","voice_controls",NVS_READWRITE,&db) != ESP_OK) return false;
    bool ok = nvs_set_blob(db,"policy",data.get(),strlen(data.get())) == ESP_OK && nvs_commit(db) == ESP_OK;
    nvs_close(db);
    return ok;
}

inline bool ToolEnabled(const std::string& tool) {
    const char* group = nullptr;
    if (tool == "self.music.play_file" || tool == "self.music.control") group = "music";
    if (tool == "self.audio_speaker.set_volume" || tool == "self.audio_speaker.adjust_volume") group = "volume";
    if (tool == "self.voice.enter_local_mode") group = "state";
    return !group || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(Load().get(), group));
}
}
