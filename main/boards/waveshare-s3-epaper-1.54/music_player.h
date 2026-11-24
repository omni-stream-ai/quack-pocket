#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "music/music_index.h"

enum class MusicAction { RecordMemo, HearMemo, Story, StoryBook, StoryRestart, StoryChapter, RepeatStory, RepeatFile, PlaybackSettings, InstallModel, Listen, Play, Pause, Previous, Toggle, Next, ChangeMode, Sequential, Shuffle, Playlist, PlayFile, Prepare, Refresh, Stop, VoiceSelect };

class MusicPlayer {
public:
    bool Start(std::function<void()> power_off);
    static MusicPlayer* Active() { return active_.load(); }
    bool FinishRecording() { return recording_.exchange(false); }
    bool RequestPowerOff();
    bool IsBusy() const { return commands_ && (busy_ || powering_off_ || uxQueueMessagesWaiting(commands_)); }
    bool Request(MusicAction action, const char *path = "");
    bool LoudnessEnabled() const { return loudness_enabled_.load(); }
    bool SetLoudnessEnabled(bool enabled);
    enum class VoiceResult { Playing, Choice, Retry, Message };
    using VoiceReply = std::function<void(VoiceResult, std::string)>;
    bool RequestVoice(MusicAction action, const std::string& path, uint32_t session, VoiceReply reply);

private:
    std::atomic<bool> loudness_enabled_{true};
    std::atomic<bool> recording_{false};
    inline static std::atomic<MusicPlayer*> active_{nullptr};
    struct VoiceRequest {
        uint32_t session;
        VoiceReply reply;
    };
    struct Command {
        MusicAction action;
        char path[MUSIC_PATH_CAPACITY];
        VoiceRequest* voice = nullptr;
    };
    QueueHandle_t commands_ = nullptr;
    std::atomic<bool> busy_{true};
    std::atomic<bool> powering_off_{false};
    std::function<void()> power_off_;
    void Run();
};
