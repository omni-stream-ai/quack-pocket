#pragma once

#include <cstdint>
#include <span>

class MusicLoudness {
public:
    explicit MusicLoudness(int sample_rate) : sample_rate_(sample_rate) {}
    void Reset();
    void Process(std::span<int16_t> pcm);

private:
    const int sample_rate_;
    float mean_square_ = 0.0f;
    float gain_ = 1.0f;
};
