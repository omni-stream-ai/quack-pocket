#ifdef OMNI_MUSIC_HIL
#include "music_hil.h"
#endif
#include "music_player.h"
#include "offline_commands.h"
#include <algorithm>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <esp_random.h>
#include <esp_log.h>
#include "music/music_resampler.h"

#include "application.h"
#include "board.h"
#include "config.h"
#include "display.h"
#include "music/mp3_stream.h"
#include "music/build_index.h"
#include "music_sd.h"

namespace {
void Show(const std::string& message) {
    auto* display = Board::GetInstance().GetDisplay();
    display->SetChatMessage("system", message.c_str());
    display->ShowVoiceHint(message.c_str(), 3500);
}

uint32_t NextTrack(uint32_t current, uint32_t count, bool shuffle) {
    if (!shuffle || count == 1) {
        return (current + 1) % count;
    }
    uint32_t choices = count - 1;
    uint32_t threshold = (0u - choices) % choices;
    uint32_t random;
    do {
        random = esp_random();
    } while (random < threshold);
    return (current + 1 + random % choices) % count;
}
}

bool MusicPlayer::Start(std::function<void()> power_off) {
    power_off_ = std::move(power_off);
    commands_ = xQueueCreate(3, sizeof(Command));
    if (!commands_) {
        return false;
    }
    if (xTaskCreate([](void *arg) { static_cast<MusicPlayer*>(arg)->Run(); },
                    "sd_player", 32768, this, 3, nullptr) != pdPASS) {
        vQueueDelete(commands_);
        commands_ = nullptr;
        return false;
    }
    return true;
}

bool MusicPlayer::RequestPowerOff() {
    if (!commands_) return false;
    powering_off_.store(true);
    Command wake{MusicAction::Stop, {}};
    xQueueSendToFront(commands_, &wake, 0);
    return true;
}

bool MusicPlayer::Request(MusicAction action, const char *path) {
    if (powering_off_.load() || !commands_ || !path || strlen(path) >= MUSIC_PATH_CAPACITY) {
        return false;
    }
    Command command{action, {}};
    strcpy(command.path, path);
    return xQueueSend(commands_, &command, 0) == pdTRUE;
}

void MusicPlayer::Run() {
    music_index_t index{};
    std::unique_ptr<FILE, int(*)(FILE*)> file(nullptr, fclose);
    std::unique_ptr<mp3_stream_t, decltype(&mp3_stream_close)> decoder(nullptr, mp3_stream_close);
    std::vector<int16_t> pending;
    MusicResampler resampler;
    int input_rate = 0;
    uint32_t track = 0;
    bool shuffle = false;
    bool playing = false;
    bool announce = true;
    bool exact_request = false;
    uint32_t failed_tracks = 0;
    char path[MUSIC_PATH_CAPACITY];

    bool prepare_on_boot = true;

    auto close_track = [&]() {
        decoder.reset();
        file.reset();
        pending.clear();
        input_rate = 0;
        announce = true;
    };
    auto release = [&]() {
        close_track();
        music_index_close(&index);
        music_sd_unmount();
        playing = false;
#ifdef OMNI_MUSIC_HIL
        music_hil::StorageReleased();
#endif
    };

    auto skip_unreadable = [&]() {
        if (exact_request || ++failed_tracks >= index.count) {
            release();
            Show(exact_request ? std::string("无法播放指定曲目 ") + path : "曲库中没有可播放的 MP3");
            return;
        }
        // Walk forward after a failure so an all-invalid shuffle list terminates.
        track = (track + 1) % index.count;
        close_track();
        vTaskDelay(1);
    };

    for (;;) {
        Command command{};
        bool received = true;
        if (prepare_on_boot) {
            command.action = MusicAction::Prepare;
            prepare_on_boot = false;
        } else {
            busy_ = playing;
            received = xQueueReceive(commands_, &command, playing ? 0 : portMAX_DELAY) == pdTRUE;
        }
        busy_ = true;
        const bool background = received && command.action == MusicAction::Prepare;
        if (powering_off_.load()) {
            release();
            power_off_();
            vTaskSuspend(nullptr);
        }
        if (received) {
            if (command.action == MusicAction::InstallModel) {
                if (music_sd_mount() != ESP_OK) Show("无法挂载 SD 卡，请插入 FAT32 卡后重试");
                else {
                    InstallOfflineModel([this] { return powering_off_.load(); });
                    if (!index.file) music_sd_unmount();
                }
                continue;
            }
            if (command.action == MusicAction::Listen) {
                if (music_sd_mount() != ESP_OK) {
                    Show("未发现模型卡，请检查 SD 卡");
                    continue;
                }
                auto heard = ListenOffline([this] {
                    return powering_off_.load() || uxQueueMessagesWaiting(commands_) != 0;
                });
                if (!index.file) music_sd_unmount();
                if (heard.id < 0 || powering_off_.load()) continue;
                if (heard.id == 4 || heard.id == 5) {
                    auto* codec = Board::GetInstance().GetAudioCodec();
                    int volume = std::clamp(codec->output_volume() + (heard.id == 4 ? 10 : -10), 0, 100);
                    codec->SetOutputVolume(volume);
                    Show("音量 " + std::to_string(volume));
                    continue;
                }
                static constexpr MusicAction actions[] = {MusicAction::Play, MusicAction::Pause,
                                                          MusicAction::Next, MusicAction::Previous};
                if (heard.id < 4) command.action = actions[heard.id];
                else {
                    command.action = MusicAction::PlayFile;
                    strcpy(command.path, heard.song.c_str());
                }
            }
            if (command.action == MusicAction::Stop) {
                release();
                Show("音乐已停止");
                continue;
            }
            if (command.action == MusicAction::Pause || (command.action == MusicAction::Toggle && playing)) {
                playing = false;
                Show("音乐已暂停");
#ifdef OMNI_MUSIC_HIL
                music_hil::Paused();
#endif
                continue;
            }
            if (command.action == MusicAction::Refresh) {
                release();
            }
            if (!index.file) {
                esp_err_t mounted = music_sd_mount();
                if (mounted != ESP_OK) {
                    ESP_LOGW("music_sd", "Mount failed: %s", esp_err_to_name(mounted));
                    if (!background) Show("无法挂载存储卡，请检查 FAT32 格式");
                    continue;
                }
                bool open = false;
                if (command.action != MusicAction::Refresh) {
                    for (auto name : {MUSIC_SD_ROOT "/.omni-music/catalog-v2", MUSIC_SD_ROOT "/.omni-music/previous-v2", MUSIC_SD_ROOT "/omni.idx"}) {
                        if (music_index_open(&index, name)) {
                            open = true;
                            break;
                        }
                    }
                }
                if (!open) {
                    ESP_LOGI("music_sd", "Building card index");
                    if (!background) Show("正在扫描存储卡上的 MP3");
                    if (!BuildMusicIndex(MUSIC_SD_ROOT, [this] {
#ifdef OMNI_MUSIC_HIL
                            music_hil::ScanProgress();
#endif
                            vTaskDelay(1);
                            return !powering_off_.load();
                        }, music_sd_drive()) ||
                        !music_index_open(&index, MUSIC_SD_ROOT "/.omni-music/catalog-v2")) {
                        release();
                        if (powering_off_.load()) continue;
                        ESP_LOGW("music_sd", "Could not build or reopen index");
                        if (!background) Show("曲库扫描失败，请检查卡空间或在电脑上生成索引");
                        continue;
                    }
                }
                ESP_LOGI("music_sd", "Catalog ready: %u tracks", static_cast<unsigned>(index.count));
                if (background) {
                    release();
                    continue;
                }
                if (!index.count) {
                    release();
                    Show("存储卡里没有 MP3，加歌后请三击 BOOT");
                    continue;
                }
            }
            if (command.action == MusicAction::Refresh) {
                Show("曲库已更新，共 " + std::to_string(index.count) + " 首");
                continue;
            }
            if (track >= index.count) {
                track = 0;
            }
            exact_request = command.action == MusicAction::PlayFile;
            failed_tracks = 0;
            switch (command.action) {
                case MusicAction::PlayFile:
                    if (!music_index_find(&index, command.path, &track)) {
                        if (strchr(command.path, '/')) {
                            Show("未找到指定曲目");
                            continue;
                        }
                        auto result = music_index_find_title(&index, command.path, &track, [](void* context) {
                            auto* player = static_cast<MusicPlayer*>(context);
                            vTaskDelay(1);
                            return !player->powering_off_.load() && !uxQueueMessagesWaiting(player->commands_);
                        }, this);
                        if (result != MUSIC_TITLE_FOUND) {
                            if (result == MUSIC_TITLE_AMBIGUOUS) Show("匹配到多首，请指定文件夹和文件名");
                            if (result == MUSIC_TITLE_MISSING) Show("未找到指定曲目");
                            if (result == MUSIC_TITLE_INDEX_ERROR) Show("曲库索引读取失败");
                            continue;
                        }
                    }
                    close_track();
                    break;
                case MusicAction::Previous:
                    track = track ? track - 1 : index.count - 1;
                    close_track();
                    break;
                case MusicAction::Next:
                    track = NextTrack(track, index.count, shuffle);
                    close_track();
                    break;
                case MusicAction::ChangeMode: shuffle = !shuffle; break;
                case MusicAction::Shuffle:
                    shuffle = true;
                    if (!decoder && index.count > 1) {
                        track = esp_random() % index.count;
                    }
                    break;
                case MusicAction::Sequential: shuffle = false; break;
                default: break;
            }
            playing = true;
            announce = true;
        }
        auto& app = Application::GetInstance();
        auto state = app.GetDeviceState();
        if (state != kDeviceStateIdle && state != kDeviceStateWifiConfiguring &&
            state != kDeviceStateActivating) {
            announce = true;
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }
        if (!decoder) {
            if (!music_index_get(&index, track, path)) {
                release();
                ESP_LOGW("music_sd", "Index record read failed");
                Show("音乐索引损坏，请重新生成");
                continue;
            }
            file.reset(fopen((std::string(MUSIC_SD_ROOT "/") + path).c_str(), "rb"));
            if (!file) ESP_LOGW("music_sd", "Track open failed, errno=%d", errno);
            decoder.reset(file ? mp3_stream_open(file.get()) : nullptr);
            if (!decoder) {
                ESP_LOGW("music_sd", "Decoder initialization failed, errno=%d", errno);
                skip_unreadable();
                continue;
            }
        }
        if (announce) {
            Show(std::string(shuffle ? "随机播放\n" : "顺序播放\n") + path);
            announce = false;
        }
        if (pending.empty()) {
            const int16_t *mono;
            int rate;
            int samples = mp3_stream_read(decoder.get(), &mono, &rate);
            if (samples < 0) {
                ESP_LOGW("music_sd", "MP3 stream decode failed");
                skip_unreadable();
                continue;
            }
            if (!samples) {
                exact_request = false;
                track = NextTrack(track, index.count, shuffle);
                close_track();
                continue;
            }
            failed_tracks = 0;
            if (rate == AUDIO_OUTPUT_SAMPLE_RATE) {
                pending.assign(mono, mono + samples);
            } else {
                if (input_rate != rate) {
                    resampler.Configure(rate, AUDIO_OUTPUT_SAMPLE_RATE);
                    input_rate = rate;
                }
                resampler.Process(mono, samples, pending);
            }
        }
        if (!app.GetAudioService().PushMusicPcm(pending)) {
            vTaskDelay(1);
        }
#ifdef OMNI_MUSIC_HIL
        else {
            music_hil::Queued(track, shuffle);
        }
#endif
    }
}
