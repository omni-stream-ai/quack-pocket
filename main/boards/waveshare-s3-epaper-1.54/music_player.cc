#ifdef OMNI_MUSIC_HIL
#include "music_hil.h"
#endif
#include "music_player.h"
#include "offline_commands.h"
#include "pocket_media.h"
#include "pocket_memo.h"
#include "custom_lcd_display.h"
#include <esp_timer.h>
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
void Show(const std::string& message, int duration_ms = 3500) {
    auto* display = Board::GetInstance().GetDisplay();
    display->SetChatMessage("system", message.c_str());
    display->ShowVoiceHint(message.c_str(), duration_ms);
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
    active_.store(this);
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
    char path[MUSIC_PATH_CAPACITY]{};
    std::string story_folder;

    uint32_t first = 0, end = 0;
    bool story = false, chapter_stop = true;
    long resume_offset = 0;
    int64_t deadline = 0, last_save = 0;
    auto save_position = [&](const char* name, long offset) {
        pocket_media::Json mark(cJSON_CreateObject(), cJSON_Delete);
        cJSON_AddStringToObject(mark.get(), "path", name);
        cJSON_AddNumberToObject(mark.get(), "offset", std::max(0L, offset));
        if (!pocket_media::Save("bookmark", mark.get())) Show("故事进度保存失败");
        last_save = esp_timer_get_time();
    };
    auto checkpoint = [&]() {
        if (story && decoder) save_position(path, mp3_stream_position(decoder.get()));
    };

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
        if (exact_request || ++failed_tracks >= end-first) {
            release();
            Show(exact_request ? std::string("无法播放指定曲目 ") + path : "曲库中没有可播放的 MP3");
            return;
        }
        // Walk forward after a failure so an all-invalid shuffle list terminates.
        track = first + (track-first + 1) % (end-first);
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
            checkpoint();
            release();
            music_sd_shutdown();
            power_off_();
            vTaskSuspend(nullptr);
        }
        if (received) {
            if (command.action == MusicAction::RecordMemo || command.action == MusicAction::HearMemo) {
                checkpoint(); release(); story = false; deadline = 0;
                auto interrupted = [this] { return powering_off_.load() || uxQueueMessagesWaiting(commands_); };
                if (command.action == MusicAction::RecordMemo) {
                    recording_ = true;
                    Show("正在录留言，按 BOOT 保存", (pocket_media::Config().record_seconds + 1) * 1000);
                    Show(RecordPocketMemo([&] { return !recording_ || interrupted(); }));
                    recording_ = false;
                } else Show(PlayPocketMemo(interrupted));
                static_cast<CustomLcdDisplay*>(Board::GetInstance().GetDisplay())->RefreshHome();
                continue;
            }
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
                checkpoint(); story = false; deadline = 0;
                release();
                Show("音乐已停止");
                continue;
            }
            if (command.action == MusicAction::Pause || (command.action == MusicAction::Toggle && playing)) {
                checkpoint();
                playing = false;
                Show("音乐已暂停");
#ifdef OMNI_MUSIC_HIL
                music_hil::Paused();
#endif
                continue;
            }
            if (command.action == MusicAction::Refresh) {
                checkpoint(); story = false; deadline = 0;
                release();
            }
            if (!index.file) {
                esp_err_t mounted = music_sd_mount();
                if (mounted != ESP_OK) {
                    ESP_LOGW("music_sd", "Mount failed: %s", esp_err_to_name(mounted));
                    if (!background) {
                        if (mounted == ESP_ERR_NO_MEM) Show("设备内存不足，无法打开 SD 卡");
                        else if (mounted == ESP_ERR_TIMEOUT) Show("SD 卡未响应，请检查插卡");
                        else Show(std::string("SD 卡挂载失败：") + esp_err_to_name(mounted));
                    }
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
            pocket_media::Config config;
            bool start_story = command.action == MusicAction::Story;
            bool use_story = start_story || (story && command.action != MusicAction::PlayFile &&
                command.action != MusicAction::Shuffle && command.action != MusicAction::Sequential);
            uint32_t low, high;
            if (!music_index_directory(&index, (use_story ? config.stories : config.music).c_str(), &low, &high)
                || low == high) {
                Show("所选目录没有已索引的 MP3，请核对路径并刷新曲库");
                continue;
            }
            uint32_t requested_track = low;
            if (command.action == MusicAction::PlayFile) {
                if (!music_index_find(&index,command.path,&requested_track) || requested_track < low || requested_track >= high) {
                    if (strchr(command.path,'/')) { Show("所选目录中未找到指定曲目"); continue; }
                    auto result = music_index_find_title_in(&index,command.path,&requested_track,low,high,[](void* context) {
                        auto* player = static_cast<MusicPlayer*>(context);
                        vTaskDelay(1);
                        return !player->powering_off_.load() && !uxQueueMessagesWaiting(player->commands_);
                    },this);
                    if (result != MUSIC_TITLE_FOUND) {
                        if (result == MUSIC_TITLE_AMBIGUOUS) Show("匹配到多首，请指定文件夹和文件名");
                        if (result == MUSIC_TITLE_MISSING) Show("未找到指定曲目");
                        if (result == MUSIC_TITLE_INDEX_ERROR) Show("曲库索引读取失败");
                        continue;
                    }
                }
            }
            if (start_story && command.path[0] && (!music_index_find(&index,command.path,&requested_track) ||
                requested_track < low || requested_track >= high)) { Show("所选故事目录中未找到章节"); continue; }
            if (use_story && !start_story && !story_folder.empty()) {
                uint32_t book_first, book_end;
                if (!music_index_directory(&index,story_folder.c_str(),&book_first,&book_end)) {
                    Show("故事目录读取失败"); continue;
                }
                low = std::max(low,book_first);
                high = std::min(high,book_end);
                if (low >= high) { Show("故事目录已更改，请重新选择故事"); continue; }
            }
            if (start_story || story != use_story || track < low || track >= high) {
                checkpoint(); close_track();
            }
            story = use_story; first = low; end = high;
            if (track < first || track >= end) track = first;
            if (!story) { deadline = 0; resume_offset = 0; }
            if (start_story) {
                track = first; resume_offset = 0; shuffle = false;
                auto mark = pocket_media::Load("bookmark");
                auto wanted = command.path[0] ? std::string(command.path) : pocket_media::Text(mark.get(), "path");
                uint32_t saved;
                if (music_index_find(&index,wanted.c_str(),&saved) && saved >= first && saved < end) {
                    track = saved;
                    if (wanted == pocket_media::Text(mark.get(),"path"))
                        resume_offset = pocket_media::Number(mark.get(),"offset",0);
                } else if (command.path[0]) { playing = false; Show("未找到指定章节"); continue; }
                char chapter[MUSIC_PATH_CAPACITY];
                if (!music_index_get(&index,track,chapter)) { release(); Show("章节索引读取失败"); continue; }
                std::string chapter_path(chapter);
                auto slash = chapter_path.rfind('/');
                story_folder = slash == std::string::npos ? "" : chapter_path.substr(0,slash);
                if (!music_index_directory(&index,story_folder.c_str(),&first,&end) || first >= end) {
                    release(); Show("书目索引读取失败"); continue;
                }
                deadline = config.minutes ? esp_timer_get_time() + config.minutes * 60000000LL : 0;
                chapter_stop = config.chapter_stop;
            }
            exact_request = command.action == MusicAction::PlayFile;
            failed_tracks = 0;
            switch (command.action) {
                case MusicAction::PlayFile:
                    track = requested_track;
                    close_track();
                    break;
                case MusicAction::Previous:
                    track = track > first ? track - 1 : end - 1;
                    close_track();
                    break;
                case MusicAction::Next:
                    track = first + NextTrack(track-first, end-first, shuffle);
                    close_track();
                    break;
                case MusicAction::ChangeMode: if (!story) shuffle = !shuffle; break;
                case MusicAction::Shuffle:
                    shuffle = true;
                    if (!decoder && end-first > 1) {
                        track = first + esp_random() % (end-first);
                    }
                    break;
                case MusicAction::Sequential: shuffle = false; break;
                default: break;
            }
            playing = true;
            announce = true;
        }
        if (story && deadline && esp_timer_get_time() >= deadline) {
            checkpoint(); playing = false; deadline = 0; Show("故事定时已结束"); continue;
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
            if (decoder && resume_offset > 0) {
                long start = ftell(file.get());
                if (fseek(file.get(),0,SEEK_END) == 0) {
                    long size = ftell(file.get());
                    fseek(file.get(), resume_offset < size ? resume_offset : start, SEEK_SET);
                }
            }
            resume_offset = 0;
            if (!decoder) {
                ESP_LOGW("music_sd", "Decoder initialization failed, errno=%d", errno);
                skip_unreadable();
                continue;
            }
        }
        if (announce) {
            Show(std::string(story ? "睡前故事\n" : shuffle ? "随机播放\n" : "顺序播放\n") + path);
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
                bool last = track + 1 == end;
                track = first + NextTrack(track-first, end-first, shuffle);
                close_track();
                if (story) {
                    char next[MUSIC_PATH_CAPACITY];
                    if (music_index_get(&index,track,next)) save_position(next,0);
                    if (last || chapter_stop) { playing = false; Show("本章已播完"); }
                }
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
        if (story && esp_timer_get_time()-last_save >= 60000000LL) checkpoint();
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
