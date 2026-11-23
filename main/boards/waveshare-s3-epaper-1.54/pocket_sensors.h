#pragma once

#include "driver/i2c_master.h"
#include <string>

namespace pocket_sensors {
void Initialize(i2c_master_bus_handle_t bus);
void ClockSynchronized();
void SaveClockIfNeeded();
bool ReadEnvironment(float& temperature, float& humidity);
std::string Status();
}
