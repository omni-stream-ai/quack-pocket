#include "voice/control_policy.h"
#include <cassert>

static std::string stored;
static int reads = 0;
static bool read_failure = false;
int nvs_open_from_partition(const char*, const char*, int, nvs_handle_t* handle) {
    *handle = 1;
    return ESP_OK;
}
int nvs_get_blob(nvs_handle_t, const char*, void* dst, size_t* size) {
    ++reads;
    if (read_failure) return ESP_FAIL;
    if (stored.empty()) return ESP_ERR_NVS_NOT_FOUND;
    if (dst) {
        if (*size < stored.size()) return ESP_ERR_INVALID_SIZE;
        memcpy(dst, stored.data(), stored.size());
    }
    *size = stored.size();
    return ESP_OK;
}
int nvs_set_blob(nvs_handle_t, const char*, const void* data, size_t size) {
    stored.assign(static_cast<const char*>(data), size);
    return ESP_OK;
}
int nvs_commit(nvs_handle_t) { return ESP_OK; }
void nvs_close(nvs_handle_t) {}

int main() {
    auto policy = voice_control::Load();
    auto skills = cJSON_GetObjectItem(policy.get(), "skills");
    for (int i = 0; i < 60; ++i) {
        auto skill = cJSON_CreateObject();
        cJSON_AddStringToObject(skill, "phrase", ("技能" + std::to_string(i)).c_str());
        cJSON_AddStringToObject(skill, "action", i == 0 ? "music" : "volume");
        if (i == 0) cJSON_AddStringToObject(skill, "argument", "晴天");
        else cJSON_AddNumberToObject(skill, "argument", i);
        cJSON_AddItemToArray(skills, skill);
    }
    assert(voice_control::Save(policy.get()));
    voice_control::Match match;
    read_failure = true;
    assert(!voice_control::ToolEnabled("self.music.control"));
    read_failure = false;
    assert(voice_control::Find("技能0", match) && match.song == "晴天");
    auto warm_reads = reads;
    assert(voice_control::Find("技能59", match) && match.volume == 59);
    assert(!voice_control::Find("不存在", match));
    assert(voice_control::ToolEnabled("self.music.control") && reads == warm_reads);
    cJSON_ReplaceItemInObject(policy.get(), "music", cJSON_CreateFalse());
    assert(voice_control::Save(policy.get()));
    assert(!voice_control::Find("技能0", match));
    assert(!voice_control::ToolEnabled("self.music.control"));
    assert(voice_control::Find("技能59", match));
    auto previous = stored;
    cJSON_AddStringToObject(policy.get(), "extra", std::string(4096, 'x').c_str());
    assert(!voice_control::Save(policy.get()) && stored == previous);
    assert(voice_control::Find("技能59", match));
}
