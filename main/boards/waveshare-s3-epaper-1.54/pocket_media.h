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
inline std::string StoryBook(const std::string& path) {
    auto slash = path.rfind('/');
    return slash == std::string::npos ? "" : path.substr(0, slash);
}

inline Json StoryBookmarks() {
    auto books = Load("story_marks");
    if (cJSON_IsArray(books.get())) return books;
    books.reset(cJSON_CreateArray());
    auto legacy = Load("bookmark");
    if (!Text(legacy.get(), "path").empty()) cJSON_AddItemToArray(books.get(), legacy.release());
    return books;
}

inline Json LatestStoryBookmark() {
    auto books = StoryBookmarks();
    return Json(cJSON_Duplicate(cJSON_GetArrayItem(books.get(), 0), true), cJSON_Delete);
}

inline Json StoryBookmark(const std::string& book) {
    auto books = StoryBookmarks();
    cJSON* mark;
    cJSON_ArrayForEach(mark, books.get()) {
        auto path = Text(mark, "path");
        if (!path.empty() && StoryBook(path) == book) return Json(cJSON_Duplicate(mark, true), cJSON_Delete);
    }
    return Json(nullptr, cJSON_Delete);
}

inline bool SaveStoryBookmark(const cJSON* mark) {
    auto books = StoryBookmarks();
    const auto book = StoryBook(Text(mark, "path"));
    for (int i = cJSON_GetArraySize(books.get()); i-- > 0;) {
        auto existing = Text(cJSON_GetArrayItem(books.get(), i), "path");
        if (StoryBook(existing) == book) cJSON_DeleteItemFromArray(books.get(), i);
    }
    if (!cJSON_InsertItemInArray(books.get(), 0, cJSON_Duplicate(mark, true))) return false;
    constexpr int max_books = 12;
    while (cJSON_GetArraySize(books.get()) > max_books) cJSON_DeleteItemFromArray(books.get(), max_books);
    return Save("story_marks", books.get());
}

enum class Repeat { Off, All, One };

struct PlaybackOptions {
    int music_minutes;
    bool track_stop;
    Repeat music_repeat, story_repeat;

    PlaybackOptions() {
        auto data = Load("playback");
        track_stop = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data.get(), "track_stop"));
        music_minutes = Number(data.get(), "music_minutes", 0);
        music_repeat = static_cast<Repeat>(Number(data.get(), "music_repeat", static_cast<int>(Repeat::All)));
        story_repeat = static_cast<Repeat>(Number(data.get(), "story_repeat", static_cast<int>(Repeat::Off)));
    }

    bool Save() const {
        Json data(cJSON_CreateObject(), cJSON_Delete);
        cJSON_AddNumberToObject(data.get(), "music_minutes", music_minutes);
        cJSON_AddBoolToObject(data.get(), "track_stop", track_stop);
        cJSON_AddNumberToObject(data.get(), "music_repeat", static_cast<int>(music_repeat));
        cJSON_AddNumberToObject(data.get(), "story_repeat", static_cast<int>(story_repeat));
        return pocket_media::Save("playback", data.get());
    }
};

struct Config {
    std::string music, stories, messages;
    int minutes, record_seconds;
    bool chapter_stop, music_set, stories_set;
    Config() {
        auto data = Load("config");
        music = Text(data.get(), "music");
        stories = Text(data.get(), "stories", "故事");
        messages = Text(data.get(), "messages", "留言");
        auto selected = [&](const char* key, bool fallback) {
            auto flag = cJSON_GetObjectItemCaseSensitive(data.get(),key);
            return cJSON_IsBool(flag) ? cJSON_IsTrue(flag) != 0 : fallback;
        };
        music_set = selected("music_set", !music.empty());
        stories_set = selected("stories_set", data && stories != "故事");
        minutes = Number(data.get(), "minutes", 0);
        record_seconds = Number(data.get(), "record_seconds", 60);
        chapter_stop = cJSON_IsTrue(cJSON_GetObjectItem(data.get(), "chapter_stop")) || !data;
    }
    void Export(cJSON* data) const {
        cJSON_AddStringToObject(data,"music",music.c_str());
        cJSON_AddStringToObject(data,"stories",stories.c_str());
        cJSON_AddStringToObject(data,"messages",messages.c_str());
        cJSON_AddBoolToObject(data,"music_set",music_set);
        cJSON_AddBoolToObject(data,"stories_set",stories_set);
        cJSON_AddNumberToObject(data,"minutes",minutes);
        cJSON_AddNumberToObject(data,"record_seconds",record_seconds);
        cJSON_AddBoolToObject(data,"chapter_stop",chapter_stop);
    }
};
}
