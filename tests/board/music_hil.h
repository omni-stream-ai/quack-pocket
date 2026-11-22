#pragma once
#include <cstdint>
class MusicPlayer;

namespace music_hil {
void StorageReleased();
void ScanProgress();
void PowerOffReached();
void Queued(uint32_t track, bool shuffle);
void Paused();
void OutputWritten();
void Run(MusicPlayer& player);
}
