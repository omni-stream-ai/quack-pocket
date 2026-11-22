#pragma once
#include <array>
#include <cstdint>
#include <vector>

class MusicResampler {
public:
    void Configure(int input_rate, int output_rate);
    void Process(const int16_t* input, int count, std::vector<int16_t>& output);

private:
    static constexpr int taps = 32;
    static constexpr int phases = 64;
    std::array<std::array<int16_t, taps>, phases> coefficients_{};
    std::array<int16_t, taps> history_{};
    int input_rate_ = 0, output_rate_ = 0;
    int cursor_ = 0, phase_ = 0;
};
