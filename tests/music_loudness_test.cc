#include "music_loudness.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <numbers>
#include <vector>

namespace {
constexpr int rate = 24000;

std::vector<int16_t> Tone(int amplitude) {
    std::vector<int16_t> pcm(480);
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = static_cast<int16_t>(amplitude * std::sin(2 * std::numbers::pi * i / 24));
    return pcm;
}

double Rms(const std::vector<int16_t>& pcm) {
    double energy = 0;
    for (double sample : pcm) energy += sample * sample;
    return std::sqrt(energy / pcm.size());
}

double Settle(MusicLoudness& loudness, int amplitude) {
    const auto input = Tone(amplitude);
    auto block = input;
    for (int i = 0; i < 300; ++i) {
        block = input;
        loudness.Process(block);
    }
    return Rms(block);
}
}

int main() {
    MusicLoudness loudness(rate);
    const auto quiet = Settle(loudness, 1400);
    assert(quiet > 3700 && quiet < 4100);
    loudness.Reset();
    const auto loud = Settle(loudness, 18000);
    assert(std::abs(quiet - loud) < 150);

    loudness.Reset();
    auto noise = Tone(50);
    const auto unchanged = noise;
    loudness.Process(noise);
    assert(noise == unchanged);
    assert(Settle(loudness, 600) < Rms(Tone(600)) * 4.01);
    std::vector<int16_t> silence(480);
    loudness.Process(silence);
    assert(std::all_of(silence.begin(), silence.end(), [](int sample) { return sample == 0; }));
    auto transient = Tone(1400);
    transient.front() = -32768;
    transient.back() = 32767;
    loudness.Process(transient);
    assert(std::all_of(transient.begin(), transient.end(), [](int sample) { return std::abs(sample) <= 29200; }));

    loudness.Reset();
    MusicLoudness fresh(rate);
    auto expected = Tone(1400), actual = expected;
    loudness.Process(actual);
    fresh.Process(expected);
    assert(actual == expected);
}
