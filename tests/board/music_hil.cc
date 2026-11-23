#include "music_hil.h"

#include "boards/waveshare-s3-epaper-1.54/music_player.h"
#include "boards/waveshare-s3-epaper-1.54/music_sd.h"
#include <esp_log.h>
#include <esp_timer.h>
#include "application.h"
#include "assets.h"
#include "boards/waveshare-s3-epaper-1.54/pocket_library.h"
#include "boards/waveshare-s3-epaper-1.54/pocket_media.h"
#include <cstring>
#include <cmath>
#include <atomic>
#include <esp_crc.h>
#include <sys/stat.h>
#include <string>

namespace music_hil {
namespace {
std::atomic<unsigned> releases, queued, written, pauses;
std::atomic<uint32_t> current;
std::atomic<bool> random_mode, scan_active, shutdown_unmounted;

template<class Predicate>
bool Await(Predicate check, unsigned milliseconds = 10000) {
    int64_t end = esp_timer_get_time() + 1000LL * milliseconds;
    while (!check() && esp_timer_get_time() < end) vTaskDelay(pdMS_TO_TICKS(20));
    return check();
}

bool Check(bool ok, const char* step) {
    if (ok) ESP_LOGI("music_hil", "PASS %s", step);
    else ESP_LOGE("music_hil", "FAIL %s", step);
    return ok;
}

// Measure two bins over a 20 ms microphone frame. Keep no recorded audio.
double TonePower(const std::vector<int16_t>& frame, unsigned hz) {
    double coefficient = 2 * cos(2 * 3.141592653589793 * hz / 24000);
    double prev = 0, older = 0;
    for (int16_t sample : frame) {
        double value = sample + coefficient * prev - older;
        older = prev;
        prev = value;
    }
    return (prev * prev + older * older - coefficient * prev * older) /
        (frame.size() * frame.size());
}

bool CheckDecoderReset() {
    auto& audio = Application::GetInstance().GetAudioService();
    unsigned before = 0;
    for (unsigned turn = 0; turn < 40; ++turn) {
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = turn % 2 ? 24000 : 16000;
        packet->frame_duration = 20;
        packet->payload = {0xf8, 0xff, 0xfe}; // 20 ms Opus silence
        if (turn == 39) before = written.load();
        if (!audio.PushPacketToDecodeQueue(std::move(packet),true)) return false;
        if (turn != 39) {
            vTaskDelay(1);
            audio.ResetDecoder();
        }
    }
    return Check(Await([&] { return written.load() > before; }), "playback after repeated decoder resets");
}

bool CheckAudioLoop() {
    auto& audio = Application::GetInstance().GetAudioService();
    std::vector<int16_t> frame;
    for (int i = 0; i < 20; ++i) if (!audio.ReadAudioData(frame, 24000, 480)) return false;
    for (unsigned hz : {1000u, 1600u}) {
        double baseline = 0;
        for (int i = 0; i < 10; ++i) {
            if (!audio.ReadAudioData(frame, 24000, 480)) return false;
            baseline += TonePower(frame, hz) / 10;
        }
        std::vector<int16_t> tone(14400);
        for (size_t i = 0; i < tone.size(); ++i)
            tone[i] = std::lround(8000 * sin(2 * 3.141592653589793 * hz * i / 24000));
        if (!audio.PushMusicPcm(tone)) return false;
        unsigned detected = 0;
        for (int i = 0; i < 45; ++i) {
            if (!audio.ReadAudioData(frame, 24000, 480)) return false;
            double signal = TonePower(frame, hz);
            double other = TonePower(frame, hz == 1000 ? 1600 : 1000);
            if (signal > 4 * (1 + baseline) && signal > 8 * (1 + other)) ++detected;
        }
        ESP_LOGI("music_hil", "Microphone %u Hz matched %u frames", hz, detected);
        if (!Check(detected >= 8, "microphone detects emitted tone")) return false;
    }
    return true;
}

// wake_phrase.pcm is 24 kHz mono signed LE16, synthesized from “小鸭小鸭”
// using Piper zh_CN-huayan-medium. Embedded only in the board test.
bool CheckWakeWord() {
    extern const uint8_t phrase_start[] asm("_binary_wake_phrase_pcm_start");
    extern const uint8_t phrase_end[] asm("_binary_wake_phrase_pcm_end");
    static std::atomic<bool> detected;
    auto& audio = Application::GetInstance().GetAudioService();
    if (!Check(Assets::GetInstance().Apply(), "apply production model assets")) return false;
    AudioServiceCallbacks callbacks{};
    callbacks.on_wake_word_detected = [](const std::string& word) { detected = !word.empty(); };
    audio.SetCallbacks(callbacks);
    audio.EnableWakeWordDetection(true);
    if (!Check(audio.IsWakeWordRunning(), "start microphone WakeNet")) return false;
    vTaskDelay(pdMS_TO_TICKS(1200));
    if (!Check(!detected.load(), "quiet interval before phrase")) return false;
    for (int attempt = 0; attempt < 3 && !detected.load(); ++attempt) {
        std::vector<int16_t> phrase((phrase_end - phrase_start) / sizeof(int16_t));
        memcpy(phrase.data(), phrase_start, phrase.size() * sizeof(int16_t));
        if (!audio.PushMusicPcm(phrase)) return false;
        Await([] { return detected.load(); }, 3200);
    }
    audio.EnableWakeWordDetection(false);
    return Check(detected.load(), "synthetic wake phrase through speaker and microphone");
}

bool CatalogChecksum(uint32_t& sum) {
    if (music_sd_mount() != ESP_OK) return false;
    FILE* file = fopen(MUSIC_SD_ROOT "/.omni-music/catalog-v2", "rb");
    bool ok = file != nullptr;
    if (file) {
        uint8_t bytes[512];
        size_t n;
        sum = 0;
        while ((n = fread(bytes, 1, sizeof(bytes), file)) > 0)
            sum = esp_crc32_le(sum, bytes, n);
        ok = !ferror(file);
        fclose(file);
    }
    return music_sd_unmount() == ESP_OK && ok;
}

bool CheckShutdown(MusicPlayer& player) {
    uint32_t before, after;
    if (!Check(CatalogChecksum(before), "read catalog before shutdown")) return false;
    scan_active = false;
    if (!Check(player.Request(MusicAction::Refresh) && Await([] { return scan_active.load(); }),
               "index rebuild in progress")) return false;
    if (!Check(player.RequestPowerOff() && Await([] { return shutdown_unmounted.load(); }),
               "shutdown callback after SD unmount")) return false;
    return Check(!player.Request(MusicAction::Toggle) && CatalogChecksum(after) && before == after,
                 "previous catalog intact after interrupted rebuild");
}

bool Exercise(MusicPlayer& player) {
    if (!Check(Await([] { return releases.load() > 0; }, 180000), "background catalog ready")) return false;

    // The player has released its mount and is waiting for the first command.
    if (!Check(music_sd_mount() == ESP_OK, "mount card for fixture selection")) return false;
    music_index_t index{};
    for (auto file : {MUSIC_SD_ROOT "/.omni-music/catalog-v2", MUSIC_SD_ROOT "/.omni-music/previous-v2", MUSIC_SD_ROOT "/omni.idx"}) {
        if (music_index_open(&index, file)) break;
    }
    uint32_t count = index.count, selected = 0;
    char path[MUSIC_PATH_CAPACITY]{};
    bool readable = false, previous_has_data = false;
    for (uint32_t track = 0; track < count; ++track) {
        if (!music_index_get(&index, track, path)) break;
        struct stat info{};
        bool has_data = stat((std::string(MUSIC_SD_ROOT "/") + path).c_str(), &info) == 0 && info.st_size > 10;
        if (has_data && previous_has_data) {
            selected = track - 1;
            readable = music_index_get(&index, selected, path);
            break;
        }
        previous_has_data = has_data;
    }
    std::string title;
    uint32_t title_track = 0;
    for (uint32_t candidate = selected + 1; candidate < count && title.empty(); ++candidate) {
        char full[MUSIC_PATH_CAPACITY];
        if (!music_index_get(&index, candidate, full)) break;
        std::string name(full);
        name.erase(0, name.find_last_of('/') + 1);
        if (name.size() <= 4) continue;
        name.resize(name.size() - 4);
        if (music_index_find_title(&index, name.c_str(), &title_track, nullptr, nullptr) == MUSIC_TITLE_FOUND &&
            title_track == candidate) title = name;
    }
    char canonical[MUSIC_PATH_CAPACITY]{};
    if (!title.empty()) readable = readable && music_index_get(&index,title_track,canonical);
    music_index_close(&index);
    music_sd_unmount();
    if (!Check(readable, "two consecutive nonempty indexed MP3 files")) return false;

    // Exercise playback after the normal 15-second codec idle timeout.
    vTaskDelay(pdMS_TO_TICKS(16500));
    auto plays = [&](uint32_t wanted, bool shuffle) {
        unsigned before = written.load();
        return Await([&] { return current.load() == wanted && random_mode.load() == shuffle && written.load() >= before + 6; });
    };
    if (!Check(player.Request(MusicAction::PlayFile, path) && plays(selected, false), "exact file through ES8311")) return false;
    uint32_t next = (selected + 1) % count;
    if (!Check(player.Request(MusicAction::Next) && plays(next, false), "next sequential track")) return false;
    if (!Check(player.Request(MusicAction::Toggle) && Await([] { return pauses.load() != 0; }), "pause acknowledged")) return false;
    vTaskDelay(pdMS_TO_TICKS(350));
    unsigned silent = written.load();
    vTaskDelay(pdMS_TO_TICKS(300));
    if (!Check(written.load() == silent, "output drained while paused")) return false;
    if (!Check(player.Request(MusicAction::Toggle) && plays(next, false), "resume same track")) return false;
    if (!Check(player.Request(MusicAction::ChangeMode) && plays(next, true), "switch to shuffle")) return false;
    if (!Check(player.Request(MusicAction::Next) && Await([&] { return random_mode.load() && current.load() != next; }),
        "shuffle selects another track")) return false;
    if (!Check(!title.empty() && player.Request(MusicAction::PlayFile, title.c_str()) &&
               plays(title_track, true), "basename without extension through ES8311")) return false;
    unsigned before = releases.load();
    if (!Check(player.Request(MusicAction::Stop) && Await([&] { return releases.load() > before; }), "stop and unmount")) return false;
    auto name = "HIL-" + std::to_string(esp_timer_get_time());
    using pocket_library::Playlist;
    if (!Playlist("create",name,"").ok) return false;
    struct Cleanup {
        std::string name;
        ~Cleanup() { pocket_library::Playlist("delete",name,""); }
    } cleanup{name};
    if (!Playlist("add",name,title).ok) return false;
    auto lists = pocket_media::Load("playlists");
    auto* saved = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(lists.get(),name.c_str()),0);
    if (!Check(cJSON_IsString(saved) && std::string(saved->valuestring) == canonical,
               "spoken song title saved as catalog path")) return false;
    if (!Playlist("play",name,"").ok || !plays(title_track,false)) return false;
    // Older firmware stored spoken titles verbatim.
    if (!Playlist("remove",name,canonical).ok || !Playlist("add",name,title,true).ok ||
        !Playlist("play",name,"").ok || !plays(title_track,false)) return false;
    before = releases.load();
    return Check(player.Request(MusicAction::Stop) && Await([&] { return releases.load() > before; }),
                 "play canonical and legacy playlist entries");
}
}

void ScanProgress() { scan_active = true; }
void PowerOffReached() {
    ESP_LOGI("music_hil", "Player stack minimum free: %u bytes", (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    shutdown_unmounted = music_sd_drive() < 0; }
void StorageReleased() { ++releases; }
void Queued(uint32_t track, bool shuffle) { current = track; random_mode = shuffle; ++queued; }
void Paused() { ++pauses; }
void OutputWritten() { ++written; }

void Run(MusicPlayer& player) {
    bool passed = CheckWakeWord() && Exercise(player) && CheckDecoderReset() && CheckAudioLoop() && CheckShutdown(player);
    player.Request(MusicAction::Stop);
    ESP_LOGI("music_hil", "RESULT %s, queued=%u codec_writes=%u", passed ? "PASS" : "FAIL", queued.load(), written.load());
    for (;;) vTaskDelay(portMAX_DELAY);
}
}
