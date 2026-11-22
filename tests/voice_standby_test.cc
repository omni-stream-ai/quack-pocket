#include "../main/boards/waveshare-s3-epaper-1.54/standby.h"
#include <cassert>

int main() {
    standby::Timer timer;
    assert(timer.Input(500));
    assert(!timer.Tick(60499, 1, true));
    assert(timer.Tick(60500, 1, true));
    assert(!timer.Input(65000));
    assert(!timer.Tick(90000, 1, false));
    assert(timer.Asleep());
    assert(timer.Wake(91000));
    assert(timer.Input(92000));
    assert(!timer.Tick(190000, 1, false));  // active playback or unavailable wake detector
    assert(!timer.Tick(249999, 1, true));
    assert(timer.Tick(250000, 1, true));
    timer.Wake(250001);
    assert(!timer.Tick(1000000, 0, true));  // automatic standby disabled
    assert(timer.Input(UINT32_MAX - 999));
    assert(!timer.Tick(58999, 1, true));
    assert(timer.Tick(59000, 1, true));
    assert(standby::Valid(standby::default_minutes));
    assert(!standby::Valid(-1));
    for (int value : standby::minutes) assert(standby::Valid(standby::Next(value)));
    assert(standby::Next(30) == 0);
}
