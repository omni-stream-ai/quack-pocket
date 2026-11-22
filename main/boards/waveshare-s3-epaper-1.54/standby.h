#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

namespace standby {
inline constexpr std::array<int, 6> minutes{0, 1, 3, 5, 10, 30};
inline constexpr int default_minutes = 5;

inline bool Valid(int value) {
    return std::find(minutes.begin(), minutes.end(), value) != minutes.end();
}

inline std::atomic<int> configured_minutes{default_minutes};

inline int Next(int value) {
    auto it = std::find(minutes.begin(), minutes.end(), value);
    return it == minutes.end() || ++it == minutes.end() ? minutes.front() : *it;
}

// Called under the board's input lock. Unsigned tick differences survive wraparound.
class Timer {
public:
    bool Input(uint32_t now) {
        if (asleep_) return false;
        last_activity_ = now;
        return true;
    }
    bool Tick(uint32_t now, int timeout, bool ready) {
        if (asleep_) return false;
        if (!ready || !timeout) {
            last_activity_ = now;
            return false;
        }
        asleep_ = now - last_activity_ >= static_cast<uint32_t>(timeout) * 60000;
        return asleep_;
    }
    bool Asleep() const { return asleep_; }
    bool Wake(uint32_t now) {
        bool was_asleep = asleep_;
        asleep_ = false;
        last_activity_ = now;
        return was_asleep;
    }
private:
    uint32_t last_activity_ = 0;
    bool asleep_ = false;
};
}
