#include "music_resampler.h"
#include <cassert>
#include <cmath>
#include <vector>

int main() {
    for (int rate : {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000}) {
        for (int hz : {440, 15000}) {
            if (hz * 2 >= rate) continue;
            std::vector<int16_t> input(rate);
            for (int i = 0; i < rate; ++i) input[i] = 15000 * std::sin(i * 6.28318530718 * hz / rate);
            MusicResampler converter;
            converter.Configure(rate, 24000);
            std::vector<int16_t> joined, frame;
            for (int i = 0; i < rate; i += 1152) {
                converter.Process(input.data() + i, std::min(1152, rate - i), frame);
                joined.insert(joined.end(), frame.begin(), frame.end());
            }
            assert(std::abs(int(joined.size()) - 24000) <= 3);
            int crossings = 0, peak = 0;
            for (size_t i = 100; i < joined.size(); ++i) {
                peak = std::max(peak, std::abs(int(joined[i])));
                if (joined[i - 1] < 0 && joined[i] >= 0) ++crossings;
            }
            if (hz == 440) assert(crossings >= 436 && crossings <= 440 && peak > 14500 && peak < 15500);
            else assert(peak < 80);
        }
    }
}
