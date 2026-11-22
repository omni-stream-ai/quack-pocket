#pragma once

#include <atomic>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "music/music_index.h"

enum class MusicAction { RecordMemo, HearMemo, Story, InstallModel, Listen, Play, Pause, Previous, Toggle, Next, ChangeMode, Sequential, Shuffle, PlayFile, Prepare, Refresh, Stop };

class MusicPlayer {
public:
    bool Start(std::function<void()> power_off);
    static MusicPlayer* Active() { return active_.load(); }
    bool FinishRecording() { return recording_.exchange(false); }
    bool RequestPowerOff();
    bool IsBusy() const { return commands_ && (busy_ || powering_off_ || uxQueueMessagesWaiting(commands_)); }
    bool Request(MusicAction action, const char *path = "");

private:
    std::atomic<bool> recording_{false};
    inline static std::atomic<MusicPlayer*> active_{nullptr};
    struct Command {
        MusicAction action;
        char path[MUSIC_PATH_CAPACITY];
    };
    QueueHandle_t commands_ = nullptr;
    std::atomic<bool> busy_{true};
    std::atomic<bool> powering_off_{false};
    std::function<void()> power_off_;
    void Run();
};
