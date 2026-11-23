#include "offline_commands.h"
#include "application.h"
#include "board.h"
#include "display.h"
#include "music_sd.h"
#include "music/music_index.h"

#include <algorithm>
#include <cstdio>
#include <cerrno>
#include <sys/stat.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <cstring>
#include <set>
#include <vector>
#include <esp_mn_models.h>
#include <esp_mn_speech_commands.h>
#include <esp_timer.h>
#include <model_path.h>
#include <esp_heap_caps.h>
#include <nvs.h>
#include <mbedtls/sha256.h>

// Expose this command's verified PSRAM buffers to MultiNet. Other tasks
// continue to see the flash-backed WakeNet models.
static thread_local srmodel_list_t* command_models = nullptr;
extern "C" char* __real_get_model_base_path();
extern "C" srmodel_list_t* __real_get_static_srmodels();
extern "C" char* __wrap_get_model_base_path() {
    return command_models ? nullptr : __real_get_model_base_path();
}
extern "C" srmodel_list_t* __wrap_get_static_srmodels() {
    return command_models ? command_models : __real_get_static_srmodels();
}

namespace {
constexpr char defaults[] = "播放\n暂停\n下一首\n上一首\n加音量\n降音量";
constexpr char root[] = MUSIC_SD_ROOT "/omni-models";
struct Word { std::string phrase, song; };
using Words = std::vector<Word>;

#include "offline_pinyin.inc"

std::string Pronunciation(const std::string& text) {
    std::string result;
    for (size_t pos = 0; pos < text.size();) {
        unsigned char lead = text[pos];
        if (lead == ' ') { ++pos; continue; }
        std::string unit;
        if (lead >= 'a' && lead <= 'z') {
            size_t start = pos++;
            while (pos < text.size() && text[pos] >= 'a' && text[pos] <= 'z') ++pos;
            unit = text.substr(start, pos - start);
            if (!std::binary_search(std::begin(syllables) + 1, std::end(syllables), unit)) return {};
        } else {
            if (pos + 3 > text.size() || (lead & 0xf0) != 0xe0 ||
                (text[pos+1] & 0xc0) != 0x80 || (text[pos+2] & 0xc0) != 0x80) return {};
            unsigned code = ((lead & 15) << 12) | ((text[pos+1] & 63) << 6) | (text[pos+2] & 63);
            if (code < 0x4e00 || code >= 0xa000) return {};
            unit = syllables[han_readings[code - 0x4e00]];
            if (unit.empty()) return {};
            pos += 3;
        }
        if (!result.empty()) result += ' ';
        result += unit;
    }
    return result;
}

bool Parse(const std::string& text, Words& words) {
    std::set<std::string> phrases;
    size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        auto phrase = text.substr(start, end - start);
        std::string song;
        if (words.size() >= 6) {
            auto split = phrase.find('|');
            song = phrase.substr(0, split);
            if (song.empty() || song.size() >= MUSIC_PATH_CAPACITY) return false;
            phrase = Pronunciation(split == std::string::npos ? song : phrase.substr(split + 1));
            if (phrase.empty()) return false;
            phrase = "bo fang " + phrase;
        } else {
            phrase = Pronunciation(phrase);
        }
        if (phrase.size() < ESP_MN_MIN_PHRASE_LEN || phrase.size() > ESP_MN_MAX_PHRASE_LEN || phrase.front() == ' ' || phrase.back() == ' ' ||
            phrase.find("  ") != std::string::npos || phrase.find_first_not_of("abcdefghijklmnopqrstuvwxyz ") != std::string::npos ||
            !phrases.insert(phrase).second || words.size() >= 300) return false;
        words.push_back({phrase, song});
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return words.size() >= 6;
}

void Message(const char* text) {
    Board::GetInstance().GetDisplay()->ShowVoiceHint(text, 6500);
}

struct ModelFile {
    const char* path;
    size_t bytes;
    const char* digest;
};

constexpr ModelFile files[] = {
    {"LICENSE.txt", 1187, "7d916fb00bc0742c47cafb0d0144b67f826d76779730b1cb8796045ea6ba1b9a"},
    {"mn7_cn/_MODEL_INFO_", 158, "251cee555d800867c5279cb5c88ef2cca10d0d250a17baf48f87830d502244aa"},
    {"mn7_cn/mn7_data", 2659972, "21bf13f3dc4792bdac9a9d6d6d3e69c782ded4071bba24df3da98ee14151120f"},
    {"mn7_cn/mn7_index", 4656, "8a018cc23671dcee23a80ff343c37e87f3b893f144198a2dba09bcb9f745af75"},
    {"mn7_cn/vocab", 6601, "7505578a7543455554a3d0a45cad7e2fd2f258097ba3b5f381d18d20d3f7ed51"},
    {"fst/commands_cn.txt", 7298, "a516f97ec1a826c7cd8396156857169aa3b73df122596f7854d90b23a15f2252"},
    {"fst/commands_en.txt", 2350, "517b7df8f3471105ae638cd910f4d1c85f9ba7eab9e960aad2c6ca82713ad47e"},
};

bool DigestMatches(const unsigned char digest[32], const ModelFile& entry) {
    char hex[65];
    for (unsigned i = 0; i < 32; ++i) snprintf(hex+2*i,3,"%02x",digest[i]);
    return strcmp(hex,entry.digest) == 0;
}

bool Verify(const std::string& path, const ModelFile& entry, const std::function<bool()>& cancelled) {
    struct stat info{};
    if (stat(path.c_str(), &info) || info.st_size != entry.bytes) return false;
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return false;
    unsigned char buffer[2048], digest[32];
    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    int result = mbedtls_sha256_starts(&hash, 0);
    size_t size;
    while (!result && !cancelled() && (size = fread(buffer, 1, sizeof(buffer), file)) > 0)
        result = mbedtls_sha256_update(&hash, buffer, size);
    bool complete = feof(file) && !ferror(file) && !result && !mbedtls_sha256_finish(&hash, digest);
    mbedtls_sha256_free(&hash);
    fclose(file);
    return complete && DigestMatches(digest,entry);
}

class CommandModels {
    static constexpr size_t file_count = std::size(files) - 1;
    static_assert(file_count == 6, "Update the MultiNet and FST groups when changing the model files");
    char* names_[2] = {};
    char* file_names_[file_count] = {};
    char* data_[file_count] = {};
    int sizes_[file_count] = {};
    srmodel_data_t groups_[2] = {};
    srmodel_data_t* groups_ptr_[2] = {&groups_[0], &groups_[1]};
    srmodel_list_t list_{};
public:
    ~CommandModels() {
        command_models = nullptr;
        for (auto* bytes : data_) free(bytes);
    }
    const char* Load(const std::function<bool()>& cancelled) {
        static char mn[] = "mn7_cn", fst[] = "fst";
        names_[0] = mn;
        names_[1] = fst;
        groups_[0] = {4, file_names_, data_, sizes_};
        groups_[1] = {2, file_names_+4, data_+4, sizes_+4};
        list_.num = 2;
        list_.model_name = names_;
        list_.model_data = groups_ptr_;
        for (size_t i = 0; i < file_count; ++i) {
            const auto& entry = files[i+1]; // The first entry is the license.
            file_names_[i] = const_cast<char*>(strchr(entry.path,'/')+1);
            sizes_[i] = entry.bytes;
            std::unique_ptr<FILE,decltype(&fclose)> file(fopen((std::string(root)+"/"+entry.path).c_str(),"rb"),fclose);
            if (!file) return "请先在语音互动中安装离线模型";
            data_[i] = static_cast<char*>(heap_caps_malloc(entry.bytes+1,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!data_[i]) return "外部内存不足，无法加载离线模型";
            size_t pos = 0;
            while (pos < entry.bytes && !cancelled()) {
                size_t n = fread(data_[i]+pos,1,std::min(size_t(8192),entry.bytes-pos),file.get());
                if (!n) return "模型文件不完整，请重新安装";
                pos += n;
            }
            if (cancelled()) return "已退出离线口令";
            data_[i][pos] = 0;
            unsigned char digest[32];
            if (fgetc(file.get()) != EOF || ferror(file.get()) ||
                mbedtls_sha256(reinterpret_cast<unsigned char*>(data_[i]),pos,digest,0) != 0)
                return "离线模型读取失败";
            if (!DigestMatches(digest,entry)) return "离线模型校验失败，请重新安装";
        }
        command_models = &list_;
        return nullptr;
    }
};

bool MakeDirectory(const std::string& path) {
    if (mkdir(path.c_str(), 0775) == 0) return true;
    struct stat info{};
    return errno == EEXIST && stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

const char* Download(const ModelFile& entry, const std::string& stage,
                     const std::function<bool()>& cancelled) {
    std::string url = "https://raw.githubusercontent.com/espressif/esp-sr/"
                      "dec03a644fc1b4b412851f425f2a91f1083d1e0b/";
    url += strcmp(entry.path, "LICENSE.txt") == 0 ? "LICENSE" : std::string("model/multinet_model/") + entry.path;
    esp_http_client_config_t config{};
    config.url = url.c_str();
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 8000;
    auto client = esp_http_client_init(&config);
    if (!client) return "下载初始化失败，请稍后重试";
    const char* error = "下载失败，请检查网络后重新安装";
    if (esp_http_client_open(client, 0) == ESP_OK &&
        esp_http_client_fetch_headers(client) >= 0 && esp_http_client_get_status_code(client) == 200) {
        FILE* out = fopen(stage.c_str(), "wb");
        if (!out) error = "SD 卡无法写入，请检查卡和剩余空间";
        else {
            char buffer[4096];
            size_t total = 0;
            int last_quarter = 0;
            while (total < entry.bytes && !cancelled()) {
                int n = esp_http_client_read(client, buffer, std::min(sizeof(buffer), entry.bytes - total));
                if (n <= 0) break;
                if (fwrite(buffer, 1, n, out) != static_cast<size_t>(n)) {
                    error = "SD 卡写入失败，请检查剩余空间";
                    break;
                }
                total += n;
                int quarter = total * 4 / entry.bytes;
                if (entry.bytes > 1000000 && quarter > last_quarter && quarter < 4) {
                    last_quarter = quarter;
                    Message(("正在下载识别模型 " + std::to_string(quarter * 25) + "%").c_str());
                }
            }
            if (fclose(out)) error = "SD 卡写入失败，请检查剩余空间";
            else if (total == entry.bytes) error = nullptr;
        }
    }
    esp_http_client_cleanup(client);
    return error;
}
}

void InstallOfflineModel(const std::function<bool()>& cancelled) {
    if (!MakeDirectory(root) || !MakeDirectory(std::string(root) + "/mn7_cn") ||
        !MakeDirectory(std::string(root) + "/fst")) {
        Message("无法创建模型目录，请检查 SD 卡");
        return;
    }
    Message("正在检查 SD 卡上的模型");
    for (const auto& entry : files) {
        std::string path = std::string(root) + "/" + entry.path;
        std::string stage = path + ".part";
        if (Verify(path, entry, cancelled)) continue;
        if (cancelled()) return;
        Message(("正在安装 " + std::string(entry.path)).c_str());
        const char* error = Download(entry, stage, cancelled);
        if (!error && !Verify(stage, entry, cancelled)) error = "文件校验失败，请重新安装";
        if (!error && !cancelled()) {
            // FAT cannot replace an existing destination. Only remove files
            // already verified as invalid, after the replacement is complete.
            if ((remove(path.c_str()) && errno != ENOENT) || rename(stage.c_str(), path.c_str()))
                error = "无法保存模型文件，请检查 SD 卡";
        }
        remove(stage.c_str());
        if (cancelled()) return;
        if (error) { Message(error); return; }
    }
    Message("模型已就绪，菜单选“语音互动 → 说一句音乐口令”");
}

std::string ReadOfflineWords() {
    nvs_handle_t handle;
    if (nvs_open_from_partition("omni_ui", "local_voice", NVS_READONLY, &handle) != ESP_OK) return defaults;
    size_t size = 0;
    std::string words;
    if (nvs_get_blob(handle, "words", nullptr, &size) == ESP_OK && size && size <= 24000) {
        words.resize(size);
        if (nvs_get_blob(handle, "words", words.data(), &size) != ESP_OK) words.clear();
    }
    nvs_close(handle);
    return words.empty() ? defaults : words;
}

bool SaveOfflineWords(const std::string& text, std::string& error) {
    Words words;
    if (text.size() > 24000 || !Parse(text, words)) {
        error = "请检查拼音、重复口令和歌名，最多 300 条口令";
        return false;
    }
    nvs_handle_t handle;
    auto result = nvs_open_from_partition("omni_ui", "local_voice", NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_blob(handle, "words", text.data(), text.size());
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
    }
    if (result != ESP_OK) error = "口令保存失败，请检查设置空间";
    return result == ESP_OK;
}

OfflineCommand ListenOffline(const std::function<bool()>& cancelled) {
    // The player owns the SD mount and serializes listening with decoding.
    auto& audio = Application::GetInstance().GetAudioService();
    if (audio.IsAudioProcessorRunning()) {
        Message("请先结束通话，再说音乐口令");
        return {};
    }
    Message("正在读取 SD 卡语音模型");
    CommandModels models;
    if (const char* error = models.Load(cancelled)) {
        if (!cancelled()) Message(error);
        return {};
    }
    const auto configured = ReadOfflineWords();
    Words words;
    if (!Parse(configured, words)) {
        Message("请在手机设置中检查离线口令");
        return {};
    }
    static char name[] = "mn7_cn";
    auto* engine = esp_mn_handle_from_name(name);
    auto* model = engine ? engine->create(name, 5000) : nullptr;
    if (!model) {
        Message("语音模型加载失败");
        return {};
    }
    // Return PSRAM to the online voice service after this single command.
    std::unique_ptr<model_iface_data_t, decltype(engine->destroy)> release(model, engine->destroy);
    esp_mn_commands_clear();
    for (size_t i = 0; i < words.size(); ++i) {
        if (esp_mn_commands_add(i + 1, words[i].phrase.c_str()) != ESP_OK) {
            Message("模型无法识别词表，请检查口令拼音");
            return {};
        }
    }
    // Preserve existing command IDs and any user-defined pronunciation.
    const int story_id = words.size() + 1;
    auto add_alias = [&](const char* phrase, int id) {
        for (const auto& word : words) if (word.phrase == phrase) return true;
        return esp_mn_commands_add(id, phrase) == ESP_OK;
    };
    if (!add_alias("bo fang yin yue", 1) || !add_alias("bo fang gu shi", story_id)) {
        Message("无法加载默认播放口令");
        return {};
    }
    if (esp_mn_commands_update()) {
        Message("模型无法识别词表，请检查口令拼音");
        return {};
    }
    auto input_lock = audio.AcquireCommandMicrophone();
    if (cancelled() || audio.IsAudioProcessorRunning()) return {};
    const auto listen_prompt = "请说离线口令，例如“" + configured.substr(0, configured.find('\n')) + "”";
    engine->clean(model);
    Message(listen_prompt.c_str());
    auto samples = engine->get_samp_chunksize(model);
    std::vector<int16_t> frame;
    auto deadline = esp_timer_get_time() + 5000000;
    while (!cancelled() && !audio.IsAudioProcessorRunning() && esp_timer_get_time() < deadline) {
        if (!audio.ReadAudioData(frame, 16000, samples) || frame.size() != static_cast<size_t>(samples)) break;
        if (engine->detect(model, frame.data()) == ESP_MN_STATE_DETECTED) {
            auto result = engine->get_results(model);
            int id = result && result->num ? result->command_id[0] - 1 : -1;
            if (id == story_id - 1) return {id, {}};
            if (id >= 0 && static_cast<size_t>(id) < words.size()) return {id, words[id].song};
        }
    }
    if (!cancelled()) Message("没有听清，请再试一次");
    return {};
}
