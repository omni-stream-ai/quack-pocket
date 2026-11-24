#include "music_loudness.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float target_rms = 4000.0f; // About -18 dBFS.
constexpr float gate_rms = 180.0f;
constexpr float peak_ceiling = 29200.0f; // Leave 1 dB of headroom.

float Smooth(float current, float target, float seconds, float time_constant) {
    return current + (target - current) * seconds / (seconds + time_constant);
}
}

void MusicLoudness::Reset() {
    mean_square_ = 0.0f;
    gain_ = 1.0f;
}

void MusicLoudness::Process(std::span<int16_t> pcm) {
    if (pcm.empty()) return;

    float energy = 0.0f;
    int peak = 0;
    for (int sample : pcm) {
        energy += static_cast<float>(sample) * sample;
        peak = std::max(peak, std::abs(sample));
    }
    if (!peak) return;

    const float seconds = static_cast<float>(pcm.size()) / sample_rate_;
    const float block_power = energy / pcm.size();
    if (block_power >= gate_rms * gate_rms) {
        // Keep quiet gaps out of the estimate and follow rises faster than falls.
        mean_square_ = mean_square_ == 0.0f ? block_power :
            Smooth(mean_square_, block_power, seconds, block_power > mean_square_ ? 0.3f : 2.0f);
    }
    const float target = mean_square_ > 0.0f ? std::clamp(target_rms / std::sqrt(mean_square_), 0.25f, 4.0f) : 1.0f;

    // This block's peak is known before playback. Cap both ends of the gain ramp.
    const float peak_gain = peak_ceiling / peak;
    float applied = std::min(gain_, peak_gain);
    const float wanted = std::min(target, peak_gain);
    gain_ = Smooth(applied, wanted, seconds, wanted < applied ? 0.15f : 0.5f);
    const float step = (gain_ - applied) / pcm.size();
    for (auto& sample : pcm) {
        applied += step;
        sample = static_cast<int16_t>(std::clamp(sample * applied, -peak_ceiling, peak_ceiling));
    }
}
