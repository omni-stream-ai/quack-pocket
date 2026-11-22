#include "music_resampler.h"

#include <algorithm>
#include <cmath>

void MusicResampler::Configure(int input_rate, int output_rate) {
    input_rate_ = input_rate;
    output_rate_ = output_rate;
    cursor_ = phase_ = 0;
    history_.fill(0);
    constexpr double pi = 3.14159265358979323846;
    const double cutoff = 0.94 * std::min(1.0, double(output_rate) / input_rate);
    for (int p = 0; p < phases; ++p) {
        std::array<double, taps> filter;
        double sum = 0;
        for (int k = 0; k < taps; ++k) {
            const double distance = k - (taps - 1) * 0.5 - double(p) / phases;
            const double x = pi * distance * cutoff;
            const double sinc = std::abs(x) < 1e-9 ? cutoff : cutoff * std::sin(x) / x;
            const double window = 0.42 - 0.5 * std::cos(2 * pi * k / (taps - 1)) +
                                  0.08 * std::cos(4 * pi * k / (taps - 1));
            filter[k] = sinc * window;
            sum += filter[k];
        }
        for (int k = 0; k < taps; ++k) {
            coefficients_[p][k] = std::lround(filter[k] * 16384 / sum);
        }
    }
}

void MusicResampler::Process(const int16_t* input, int count, std::vector<int16_t>& output) {
    output.clear();
    output.reserve((count * output_rate_ + input_rate_ - 1) / input_rate_ + 1);
    for (int n = 0; n < count; ++n) {
        history_[cursor_] = input[n];
        while (phase_ <= 0) {
            const auto& filter = coefficients_[-phase_ * phases / output_rate_];
            int64_t sum = 0;
            for (int k = 0; k < taps; ++k) {
                sum += int32_t(history_[(cursor_ - k + taps) % taps]) * filter[k];
            }
            output.push_back(std::clamp<int64_t>((sum + 8192) >> 14, -32768, 32767));
            phase_ += input_rate_;
        }
        cursor_ = (cursor_ + 1) % taps;
        phase_ -= output_rate_;
    }
}
