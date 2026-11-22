#pragma once

#include <nvs.h>
#include <string>
#include <mutex>
#include <cJSON.h>
#include <memory>
#include <cstring>
#include "pocket_path.h"

namespace pocket_media {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
inline std::mutex lock;
inline std::string Read(const char* key) {
    nvs_handle_t db;
    if (nvs_open_from_partition("omni_ui", "media", NVS_READONLY, &db) != ESP_OK) return {};
    size_t size = 0;
    auto err = nvs_get_blob(db, key, nullptr, &size);
    std::string result;
    if (err == ESP_OK && size > 0 && size <= 16384) {
        result.resize(size);
        if (nvs_get_blob(db, key, result.data(), &size) == ESP_OK) result.resize(size);
        else result.clear();
    }
    nvs_close(db);
    return result;
}
inline bool Save(const char* key, const cJSON* object) {
    std::unique_ptr<char, decltype(&cJSON_free)> data(cJSON_PrintUnformatted(object), cJSON_free);
    if (!data || strlen(data.get()) >= 16384) return false;
    nvs_handle_t db;
    if (nvs_open_from_partition("omni_ui", "media", NVS_READWRITE, &db) != ESP_OK) return false;
    bool ok = nvs_set_blob(db, key, data.get(), strlen(data.get())) == ESP_OK && nvs_commit(db) == ESP_OK;
    nvs_close(db);
    return ok;
}
inline Json Load(const char* key) { return Json(cJSON_Parse(Read(key).c_str()), cJSON_Delete); }
inline std::string Text(const cJSON* data, const char* key, const char* fallback = "") {
    auto* item = cJSON_GetObjectItemCaseSensitive(data, key);
    return cJSON_IsString(item) ? item->valuestring : fallback;
}
inline int Number(const cJSON* data, const char* key, int fallback) {
    auto* item = cJSON_GetObjectItemCaseSensitive(data, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}
struct Config {
    std::string music, stories, messages;
    int minutes, record_seconds;
    bool chapter_stop;
    Config() {
        auto data = Load("config");
        music = Text(data.get(), "music");
        stories = Text(data.get(), "stories", "故事");
        messages = Text(data.get(), "messages", "留言");
        minutes = Number(data.get(), "minutes", 0);
        record_seconds = Number(data.get(), "record_seconds", 60);
        chapter_stop = cJSON_IsTrue(cJSON_GetObjectItem(data.get(), "chapter_stop")) || !data;
    }
    void Export(cJSON* data) const {
        cJSON_AddStringToObject(data,"music",music.c_str());
        cJSON_AddStringToObject(data,"stories",stories.c_str());
        cJSON_AddStringToObject(data,"messages",messages.c_str());
        cJSON_AddNumberToObject(data,"minutes",minutes);
        cJSON_AddNumberToObject(data,"record_seconds",record_seconds);
        cJSON_AddBoolToObject(data,"chapter_stop",chapter_stop);
    }
};
}
