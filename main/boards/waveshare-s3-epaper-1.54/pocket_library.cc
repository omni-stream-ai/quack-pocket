#include "pocket_library.h"
#include "card_files.h"
#include "pocket_checklist.h"
#include "pocket_media.h"
#include "music_player.h"
#include "music_sd.h"

#include <algorithm>
#include <cerrno>
#include <dirent.h>

namespace pocket_library {
using pocket_media::Json;

bool AddProfile(cJSON* profile) {
    {
        std::lock_guard<std::mutex> guard(pocket_list::lock);
        pocket_list::List items;
        if (pocket_list::Read(items) != ESP_OK) return false;
        auto* list = cJSON_AddArrayToObject(profile, "checklist");
        for (auto& item : items) cJSON_AddItemToArray(list, cJSON_CreateString(item.text.c_str()));
    }
    auto favorites = pocket_media::Load("favorites");
    cJSON_AddItemToObject(profile,"favorites",favorites ? favorites.release() : cJSON_CreateArray());
    pocket_media::Config().Export(cJSON_AddObjectToObject(profile, "media"));
    cJSON_AddBoolToObject(profile, "sd_browser", true);
    cJSON_AddBoolToObject(profile, "sd_manage", true);
    return true;
}

namespace {
struct CardLease {
    bool ready = music_sd_acquire() == ESP_OK;
    ~CardLease() { if (ready) music_sd_release(); }
};

Result JsonResult(const cJSON* object) {
    std::unique_ptr<char, decltype(&cJSON_free)> body(cJSON_PrintUnformatted(object), cJSON_free);
    if (!body) return {false, "生成列表失败"};
    return {true, body.get(), true};
}

Result BrowseCard(const std::string& path, const cJSON* cursor) {
    if (!ValidPocketPath(path) || !cJSON_IsNumber(cursor) || cursor->valueint < 0 ||
        cursor->valuedouble != cursor->valueint) return Result{false, "目录或页码无效"};
    CardLease lease;
    if (!lease.ready) return Result{false, "请插入可读取的 FAT32 卡"};
    std::unique_ptr<DIR, decltype(&closedir)> dir(opendir((std::string(MUSIC_SD_ROOT "/") + path).c_str()), closedir);
    if (!dir) return Result{false, "无法打开目录"};
    Json result(cJSON_CreateObject(), cJSON_Delete);
    auto* entries = cJSON_AddArrayToObject(result.get(), "entries");
    int index = 0, count = 0;
    for (;;) {
        errno = 0;
        auto* entry = readdir(dir.get());
        if (!entry) {
            if (errno) return Result{false, "读取目录失败"};
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (index++ < cursor->valueint) continue;
        if (count == 24) {
            cJSON_AddNumberToObject(result.get(), "next", index - 1);
            break;
        }
        auto* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", entry->d_name);
        cJSON_AddBoolToObject(item, "directory", entry->d_type == DT_DIR);
        cJSON_AddItemToArray(entries, item);
        ++count;
    }
    return JsonResult(result.get());
}

Result SearchCard(const std::string& query, const cJSON* cursor) {
    if (query.size() > 255 || !cJSON_IsNumber(cursor) || cursor->valueint < 0 ||
        cursor->valueint != cursor->valuedouble) return Result{false, "搜索参数无效"};
    CardLease lease;
    if (!lease.ready) return Result{false, "请插入 SD 卡"};
    music_index_t index{};
    for (auto path : {MUSIC_SD_ROOT "/.omni-music/catalog-v2", MUSIC_SD_ROOT "/.omni-music/previous-v2", MUSIC_SD_ROOT "/omni.idx"})
        if (music_index_open(&index,path)) break;
    if (!index.file) return Result{false, "请在设备菜单刷新曲库"};
    struct IndexGuard { music_index_t* index; ~IndexGuard() { music_index_close(index); } } guard{&index};
    uint32_t first, end;
    if (!music_index_directory(&index,pocket_media::Config().music.c_str(),&first,&end)) return Result{false, "曲库读取失败"};
    uint32_t pos = std::max(first,static_cast<uint32_t>(cursor->valueint));
    Json result(cJSON_CreateObject(), cJSON_Delete);
    auto* items = cJSON_AddArrayToObject(result.get(),"entries");
    char path[MUSIC_PATH_CAPACITY];
    int count = 0;
    for (int scanned = 0; pos < end && count < 16 && scanned < 128; ++pos, ++scanned) {
        if (!music_index_get(&index,pos,path)) return Result{false, "曲库读取失败"};
        if (strcasestr(path,query.c_str())) {
            cJSON_AddItemToArray(items,cJSON_CreateString(path)); ++count;
        }
    }
    if (pos < end) cJSON_AddNumberToObject(result.get(),"next",pos);
    return JsonResult(result.get());
}

} // namespace

std::optional<Result> Handle(const cJSON* data) {
    auto get = [&](const char* key) { return pocket_media::Text(data,key); };
    const std::string action = get("action");
    if (action=="sd_reindex") {
        auto* player=MusicPlayer::Active();
        if (!player || !player->Request(MusicAction::Refresh)) return Result{false,"播放器正忙，请稍后重试"};
        return Result{true,"已请求刷新曲库"};
    }
    if (action.rfind("sd_",0)==0) return CardFileAction(action,data);
    if (action == "search_music") return SearchCard(get("query"),cJSON_GetObjectItem(data,"cursor"));
    if (action == "play_media" || action == "favorite") {
        auto path = get("path");
        if (path.empty() || !ValidPocketPath(path) || path.size() < 4 ||
            strcasecmp(path.c_str()+path.size()-4,".mp3")) return Result{false, "请选择 MP3 文件"};
        if (action == "favorite") {
            std::lock_guard<std::mutex> guard(pocket_media::lock);
            auto favorites = pocket_media::Load("favorites");
            if (!favorites) favorites.reset(cJSON_CreateArray());
            auto* enable = cJSON_GetObjectItem(data,"enabled");
            if (!cJSON_IsBool(enable)) return Result{false, "收藏参数无效"};
            int index = 0;
            while (index < cJSON_GetArraySize(favorites.get()) &&
                   path != cJSON_GetArrayItem(favorites.get(),index)->valuestring) ++index;
            if (cJSON_IsFalse(enable)) {
                if (index == cJSON_GetArraySize(favorites.get())) return Result{true, "收藏已更新"};
                cJSON_DeleteItemFromArray(favorites.get(),index);
            }
            else if (index == cJSON_GetArraySize(favorites.get())) {
                auto dir = pocket_media::Config().music;
                if (!dir.empty() && path.rfind(dir + "/",0) != 0) return Result{false, "歌曲不在音乐目录内"};
                if (index >= 48) return Result{false, "收藏夹已满"};
                cJSON_AddItemToArray(favorites.get(),cJSON_CreateString(path.c_str()));
            }
            if (!pocket_media::Save("favorites",favorites.get())) return Result{false, "收藏空间不足或保存失败"};
            return Result{true, "收藏已更新"};
        }
        if (get("kind") != "story" && get("kind") != "music") return Result{false, "请选择音乐或故事"};
        bool story = get("kind") == "story";
        pocket_media::Config config;
        auto dir = story ? config.stories : config.music;
        if (!dir.empty() && path.rfind(dir + "/",0) != 0) return Result{false, "文件不在选定的播放目录内"};
        auto* player = MusicPlayer::Active();
        if (!player || !player->Request(story ? MusicAction::Story : MusicAction::PlayFile,path.c_str()))
            return Result{false, "播放器正忙，请稍后重试"};
        return Result{true, "已发送播放请求"};
    }
    if (action == "media_config") {
        auto* config = cJSON_GetObjectItem(data, "media");
        for (const char* key : {"music","stories","messages"}) {
            auto* value = cJSON_GetObjectItem(config, key);
            if (!cJSON_IsString(value) || !ValidPocketPath(value->valuestring)) return Result{false, "请选择有效的卡内目录"};
        }
        for (const char* key : {"minutes","record_seconds"}) {
            auto* value = cJSON_GetObjectItem(config,key);
            int min = strcmp(key,"minutes") ? 1 : 0;
            if (!cJSON_IsNumber(value) || value->valueint != value->valuedouble ||
                value->valueint < min || value->valueint > (min ? 300 : 180)) return Result{false, "时长超出范围"};
        }
        if (!cJSON_IsBool(cJSON_GetObjectItem(config,"chapter_stop")) || !pocket_media::Save("config",config))
            return Result{false, "保存媒体设置失败"};
        return Result{true, "媒体设置已保存"};
    }
    if (action == "checklist") {
        auto* labels = cJSON_GetObjectItem(data, "items");
        if (!cJSON_IsArray(labels) || cJSON_GetArraySize(labels) > 24) return Result{false, "最多 24 项"};
        std::lock_guard<std::mutex> guard(pocket_list::lock);
        pocket_list::List previous, items;
        if (pocket_list::Read(previous) != ESP_OK) return Result{false, "读取清单失败"};
        cJSON* label;
        cJSON_ArrayForEach(label, labels) {
            if (!cJSON_IsString(label)) return Result{false, "清单格式错误"};
            std::string text = label->valuestring;
            if (text.empty() || text.size() > 96 || text.find_first_of("\r\n") != std::string::npos)
                return Result{false, "每项最多 96 字节且须为单行"};
            auto matches = [&](const auto& item) { return item.text == text; };
            if (std::any_of(items.begin(), items.end(), matches)) return Result{false, "清单项目重复"};
            auto old = std::find_if(previous.begin(), previous.end(), matches);
            items.push_back({text, old != previous.end() && old->checked});
        }
        if (pocket_list::Write(items) != ESP_OK) return Result{false, "保存清单失败"};
        return Result{true, "清单已保存"};
    }
    if (action == "browse_sd" && !cJSON_IsString(cJSON_GetObjectItem(data,"path"))) return Result{false,"缺少目录路径"};
    if (action == "browse_sd") return BrowseCard(get("path"), cJSON_GetObjectItem(data, "cursor"));
    return std::nullopt;
}
}
