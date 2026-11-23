#pragma once

#include <string>
#include <cstring>
#include <memory>
#include <set>
#include <vector>
#include <mutex>
#include <algorithm>
#include "cJSON.h"
#include "nvs.h"

namespace voice_control {
inline constexpr size_t max_policy_bytes = 4096;
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

inline Json Load(bool* reliable = nullptr) {
    nvs_handle_t db;
    std::string text;
    auto err = nvs_open_from_partition("omni_ui", "voice_controls", NVS_READONLY, &db);
    if (err == ESP_OK) {
        size_t n = 0;
        err = nvs_get_blob(db, "policy", nullptr, &n);
        if (err == ESP_OK && n && n <= max_policy_bytes) {
            text.resize(n);
            err = nvs_get_blob(db, "policy", text.data(), &n);
        }
        nvs_close(db);
    }
    Json policy(err == ESP_OK ? cJSON_Parse(text.c_str()) : nullptr, cJSON_Delete);
    bool readable = policy || err == ESP_ERR_NVS_NOT_FOUND;
    if (!policy) {
        policy.reset(cJSON_CreateObject());
        for (auto key : {"music","volume","state"}) cJSON_AddBoolToObject(policy.get(), key, true);
        cJSON_AddArrayToObject(policy.get(), "skills");
    }
    if (!cJSON_GetObjectItemCaseSensitive(policy.get(), "network"))
        cJSON_AddBoolToObject(policy.get(), "network", true);
    if (reliable) {
        *reliable = readable && cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(policy.get(), "skills"));
        for (auto key : {"music","volume","state","network"})
            *reliable &= cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(policy.get(), key)) != 0;
    }
    return policy;
}

enum class Group { Music, Volume, State, Network };
inline constexpr const char* group_names[] = {"music","volume","state","network"};

struct Match {
    Group group;
    std::string song;
    int volume = -1;
};

class RuntimePolicy {
    struct Entry {
        size_t phrase;
        size_t song;
        int volume;
        Group group;
    };
    bool enabled_[4]{};
    std::vector<Entry> entries_;
    std::string strings_;
    size_t Append(const char* text) {
        size_t offset = strings_.size();
        strings_.append(text, strlen(text) + 1);
        return offset;
    }
public:
    explicit RuntimePolicy(const cJSON* policy) {
        for (size_t i=0; i<4; ++i)
            enabled_[i] = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(policy, group_names[i]));
        auto skills = cJSON_GetObjectItemCaseSensitive(policy, "skills");
        entries_.reserve(cJSON_GetArraySize(skills));
        cJSON* skill;
        cJSON_ArrayForEach(skill, skills) {
            auto phrase = cJSON_GetObjectItemCaseSensitive(skill, "phrase");
            auto action = cJSON_GetObjectItemCaseSensitive(skill, "action");
            if (!cJSON_IsString(phrase) || !cJSON_IsString(action)) continue;
            Group group;
            if (!strcmp(action->valuestring, "music")) group = Group::Music;
            else if (!strcmp(action->valuestring, "volume")) group = Group::Volume;
            else if (!strcmp(action->valuestring, "state")) group = Group::State;
            else continue;
            if (!Allows(group)) continue;
            auto arg = cJSON_GetObjectItemCaseSensitive(skill, "argument");
            entries_.push_back({Append(phrase->valuestring),
                cJSON_IsString(arg) ? Append(arg->valuestring) : std::string::npos,
                cJSON_IsNumber(arg) ? arg->valueint : -1, group});
        }
        std::sort(entries_.begin(), entries_.end(), [this](const Entry& a, const Entry& b) {
            return strcmp(strings_.data()+a.phrase, strings_.data()+b.phrase) < 0;
        });
    }
    bool Allows(Group group) const { return enabled_[static_cast<size_t>(group)]; }
    bool Find(const std::string& phrase, Match& result) const {
        auto it = std::lower_bound(entries_.begin(), entries_.end(), phrase,
            [this](const Entry& entry, const std::string& key) {
                return key.compare(strings_.data()+entry.phrase) > 0;
            });
        if (it == entries_.end() || phrase != strings_.data()+it->phrase) return false;
        result = {it->group, it->song == std::string::npos ? "" : strings_.data()+it->song, it->volume};
        return true;
    }
};

inline std::mutex runtime_mutex;
inline std::unique_ptr<RuntimePolicy> runtime;
// Callers hold runtime_mutex while reading or replacing this snapshot.
inline const RuntimePolicy& RuntimeLocked() {
    if (!runtime) {
        bool reliable;
        auto policy = Load(&reliable);
        if (!reliable) {
            static const RuntimePolicy unavailable(nullptr);
            return unavailable;
        }
        runtime = std::make_unique<RuntimePolicy>(policy.get());
    }
    return *runtime;
}

inline bool Find(const std::string& phrase, Match& result) {
    std::lock_guard<std::mutex> guard(runtime_mutex);
    return RuntimeLocked().Find(phrase, result);
}

inline bool Save(cJSON* policy) {
    std::lock_guard<std::mutex> guard(runtime_mutex);
    for (auto key : {"music","volume","state"})
        if (!cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(policy, key))) return false;
    auto network = cJSON_GetObjectItemCaseSensitive(policy, "network");
    if (network && !cJSON_IsBool(network)) return false;
    if (!network && !cJSON_AddBoolToObject(policy, "network", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(Load().get(), "network")))) return false;
    auto skills = cJSON_GetObjectItemCaseSensitive(policy, "skills");
    if (!cJSON_IsArray(skills)) return false;
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
    if (!data || strlen(data.get()) > max_policy_bytes) return false;
    nvs_handle_t db;
    if (nvs_open_from_partition("omni_ui","voice_controls",NVS_READWRITE,&db) != ESP_OK) return false;
    bool ok = nvs_set_blob(db,"policy",data.get(),strlen(data.get())) == ESP_OK && nvs_commit(db) == ESP_OK;
    nvs_close(db);
    if (ok) runtime.reset();
    return ok;
}

inline bool ToolEnabled(const std::string& tool) {
    Group group;
    if (tool == "self.music.playlist" || tool == "self.music.play_file" || tool == "self.music.control") group = Group::Music;
    else if (tool == "self.audio_speaker.set_volume" || tool == "self.audio_speaker.adjust_volume") group = Group::Volume;
    else if (tool == "self.voice.enter_local_mode") group = Group::State;
    else if (tool == "self.disp.network" || tool == "self.radio.disable_wifi" || tool == "self.radio.status" || tool == "self.radio.bluetooth") group = Group::Network;
    else return true;
    std::lock_guard<std::mutex> guard(runtime_mutex);
    return RuntimeLocked().Allows(group);
}
}
