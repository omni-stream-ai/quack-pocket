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
    auto playlists = pocket_media::Load("playlists");
    cJSON_AddItemToObject(profile,"playlists",playlists ? playlists.release() : cJSON_CreateObject());
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

struct CardIndex {
    CardLease lease;
    music_index_t index{};
    CardIndex() {
        if (!lease.ready) return;
        for (auto path : {MUSIC_SD_ROOT "/.omni-music/catalog-v2", MUSIC_SD_ROOT "/.omni-music/previous-v2", MUSIC_SD_ROOT "/omni.idx"})
            if (music_index_open(&index,path)) break;
    }
    ~CardIndex() { music_index_close(&index); }
};

Result ResolvePlaylistTrack(const std::string& title) {
    CardIndex card;
    if (!card.lease.ready) return {false, "请插入 SD 卡"};
    if (!card.index.file) return {false, "请在设备菜单刷新曲库"};
    uint32_t low, high, id;
    if (!music_index_directory(&card.index,pocket_media::Config().music.c_str(),&low,&high))
        return {false, "曲库读取失败"};
    if (title.find('/') != std::string::npos) {
        if (!music_index_find(&card.index,title.c_str(),&id) || id < low || id >= high)
            return {false, "音乐目录中未找到这个路径，请核对路径或刷新曲库"};
    } else {
        auto result = music_index_find_title_in(&card.index,title.c_str(),&id,low,high,
            [](void*) { vTaskDelay(1); return true; },nullptr);
        if (result == MUSIC_TITLE_AMBIGUOUS) return {false, "匹配到多首，请指定文件夹和文件名"};
        if (result == MUSIC_TITLE_MISSING) return {false, "曲库中未找到这首歌"};
        if (result != MUSIC_TITLE_FOUND) return {false, "曲库读取失败"};
    }
    char path[MUSIC_PATH_CAPACITY];
    if (!music_index_get(&card.index,id,path)) return {false, "曲库读取失败"};
    return {true, path};
}

Result SearchCard(const std::string& query, const cJSON* cursor) {
    if (query.size() > 255 || !cJSON_IsNumber(cursor) || cursor->valueint < 0 ||
        cursor->valueint != cursor->valuedouble) return Result{false, "搜索参数无效"};
    CardIndex card;
    if (!card.lease.ready) return {false, "请插入 SD 卡"};
    auto& index = card.index;
    if (!index.file) return {false, "请在设备菜单刷新曲库"};
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

Result Playlist(const std::string& operation, const std::string& name, std::string track, bool file_selected) {
    if (operation == "add" && !file_selected) {
        if (track.empty() || !ValidPocketPath(track)) return {false, "歌曲名称或路径无效"};
        auto result = ResolvePlaylistTrack(track);
        if (!result.ok) return result;
        track = std::move(result.body);
    }
    std::lock_guard<std::mutex> guard(pocket_media::lock);
    auto lists = pocket_media::Load("playlists");
    if (!lists) lists.reset(cJSON_CreateObject());
    if (operation == "list" && name.empty()) return JsonResult(lists.get());
    if (operation == "names") {
        std::string names;
        for (auto* list = lists->child; list; list = list->next) {
            if (!names.empty()) names += "、";
            names += list->string;
        }
        return {true, names.empty() ? "还没有歌单" : "已有歌单：" + names};
    }
    if (name.empty() || name.size() > 64 || name.find_first_of("\r\n") != std::string::npos)
        return {false, "列表名须为单行，最长 64 字节"};
    auto* songs = cJSON_GetObjectItemCaseSensitive(lists.get(),name.c_str());
    if (operation == "create") {
        if (songs) return {false, "列表已存在"};
        if (cJSON_GetArraySize(lists.get()) >= 16 || !cJSON_AddArrayToObject(lists.get(),name.c_str()))
            return {false, "无法新建列表（最多 16 个）"};
    } else if (!songs) return {false, "未找到播放列表"};
    else if (operation == "list") {
        std::string titles;
        for (auto* song = songs->child; song; song = song->next) {
            std::string path = song->valuestring;
            if (!titles.empty()) titles += "、";
            titles += path.substr(path.rfind('/') + 1);
        }
        return {true, titles.empty() ? "歌单为空" : name + "：" + titles};
    } else if (operation == "play") {
        if (!cJSON_GetArraySize(songs)) return {false, "播放列表为空"};
        auto* player = MusicPlayer::Active();
        if (!player || !player->Request(MusicAction::Playlist,name.c_str())) return {false, "播放器正忙"};
        return {true, "已请求播放 " + name};
    } else if (operation == "delete") {
        cJSON_DeleteItemFromObjectCaseSensitive(lists.get(),name.c_str());
    } else if (operation == "add" || operation == "remove") {
        if (track.empty() || !ValidPocketPath(track)) return {false, "歌曲名称或路径无效"};
        int count = cJSON_GetArraySize(songs), i = 0;
        while (i < count && track != cJSON_GetArrayItem(songs,i)->valuestring) ++i;
        if (operation == "remove") {
            if (i == count && track.find('/') == std::string::npos) {
                auto stem = [](std::string title) {
                    title.erase(0,title.rfind('/') + 1);
                    if (title.size() >= 4 && !strcasecmp(title.c_str()+title.size()-4,".mp3"))
                        title.resize(title.size()-4);
                    return title;
                };
                auto wanted = stem(track);
                for (int j = 0; j < count; ++j) {
                    if (strcasecmp(stem(cJSON_GetArrayItem(songs,j)->valuestring).c_str(),wanted.c_str())) continue;
                    if (i != count) return {false, "歌单中有同名歌曲，请指定路径"};
                    i = j;
                }
            }
            if (i == count) return {false, "列表中没有这首歌"};
            cJSON_DeleteItemFromArray(songs,i);
        } else {
            if (i != count) return {false, "歌曲已在列表中"};
            if (count >= 48) return {false, "列表最多 48 首"};
            if (!cJSON_AddItemToArray(songs,cJSON_CreateString(track.c_str()))) return {false, "内存不足"};
        }
    } else return {false, "未知列表操作"};
    if (!pocket_media::Save("playlists",lists.get())) return {false, "播放列表保存失败"};
    return {true, "播放列表已更新"};
}

std::optional<Result> Handle(const cJSON* data) {
    auto get = [&](const char* key) { return pocket_media::Text(data,key); };
    const std::string action = get("action");
    if (action == "playlist") return Playlist(get("operation"),get("name"),get("track"),true);
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
