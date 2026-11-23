#include "pocket_sensors.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include <atomic>
#include <mutex>
#include <ctime>
#include <sys/time.h>

namespace pocket_sensors {
namespace {
i2c_master_dev_handle_t rtc, sensor;
std::atomic<bool> save_clock{false};
constexpr int timeout_ms = 30;

i2c_master_dev_handle_t Attach(i2c_master_bus_handle_t bus, uint8_t address) {
    i2c_device_config_t cfg{};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = address;
    cfg.scl_speed_hz = 100000;
    i2c_master_dev_handle_t device = nullptr;
    return i2c_master_bus_add_device(bus, &cfg, &device) == ESP_OK ? device : nullptr;
}

int Decode(uint8_t bcd) {
    return (bcd & 15) > 9 ? -1 : (bcd >> 4) * 10 + (bcd & 15);
}

uint8_t Encode(int value) { return (value / 10) * 16 + value % 10; }

time_t ReadClock() {
    uint8_t reg = 0, data[11];
    if (!rtc || i2c_master_transmit_receive(rtc, &reg, 1, data, sizeof(data), timeout_ms) != ESP_OK ||
        (data[0] & 0x20) || (data[4] & 0x80)) return 0;
    int year = Decode(data[10]), month = Decode(data[9]), day = Decode(data[7]);
    int hour = Decode(data[6]), minute = Decode(data[5]), second = Decode(data[4]);
    if (data[0] & 2) {
        int h = Decode(data[6] & 0x1f);
        if (h < 1 || h > 12) return 0;
        hour = h % 12 + ((data[6] & 0x20) ? 12 : 0);
    }
    if (year < 24 || year > 99 || month < 1 || month > 12 || day < 1 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) return 0;
    int lengths[] = {31, 28 + (year % 4 == 0), 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (day > lengths[month - 1]) return 0;
    int days = year * 365 + (year + 3) / 4 + day - 1;
    for (int i = 0; i < month - 1; ++i) days += lengths[i];
    return 946684800LL + days * 86400LL + hour * 3600 + minute * 60 + second;
}

bool ValidWord(const uint8_t* bytes) {
    uint8_t crc = 0xff;
    for (int i = 0; i < 2; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc << 1) ^ ((crc & 0x80) ? 0x31 : 0);
    }
    return crc == bytes[2];
}
}

void Initialize(i2c_master_bus_handle_t bus) {
    rtc = Attach(bus, 0x51);
    sensor = Attach(bus, 0x70);
    timeval tv{ReadClock(), 0};
    if (tv.tv_sec) settimeofday(&tv, nullptr);
}

void ClockSynchronized() { save_clock = true; }

void SaveClockIfNeeded() {
    if (!rtc || !save_clock.exchange(false)) return;
    time_t now = time(nullptr);
    tm utc{};
    gmtime_r(&now, &utc);
    if (utc.tm_year < 124 || utc.tm_year > 199) return;
    uint8_t reg = 0, control;
    if (i2c_master_transmit_receive(rtc, &reg, 1, &control, 1, timeout_ms) == ESP_OK) {
        uint8_t mode[] = {0, static_cast<uint8_t>(control & ~0x22)};
        uint8_t bytes[] = {4, Encode(utc.tm_sec), Encode(utc.tm_min), Encode(utc.tm_hour),
            Encode(utc.tm_mday), Encode(utc.tm_wday), Encode(utc.tm_mon + 1), Encode(utc.tm_year - 100)};
        if ((mode[1] == control || i2c_master_transmit(rtc, mode, sizeof(mode), timeout_ms) == ESP_OK) &&
            i2c_master_transmit(rtc, bytes, sizeof(bytes), timeout_ms) == ESP_OK) return;
    }
    save_clock = true;
}

bool ReadEnvironment(float& temperature, float& humidity) {
    static std::mutex mutex;
    static int64_t next = 0;
    static bool valid = false;
    static float t, rh;
    std::lock_guard<std::mutex> lock(mutex);
    int64_t now = esp_timer_get_time();
    if (sensor && now >= next) {
        uint8_t wake[] = {0x35, 0x17}, measure[] = {0x78, 0x66}, sleep[] = {0xb0, 0x98}, data[6];
        valid = false;
        if (i2c_master_transmit(sensor, wake, 2, timeout_ms) == ESP_OK) {
            esp_rom_delay_us(250);
            if (i2c_master_transmit(sensor, measure, 2, timeout_ms) == ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(13) + 1);
                valid = i2c_master_receive(sensor, data, 6, timeout_ms) == ESP_OK &&
                    ValidWord(data) && ValidWord(data + 3);
            }
            i2c_master_transmit(sensor, sleep, 2, timeout_ms);
        }
        if (valid) {
            t = -45 + 175.f * (data[0] * 256 + data[1]) / 65536;
            rh = 100.f * (data[3] * 256 + data[4]) / 65536;
        }
        next = now + (valid ? 59000000 : 5000000);
    }
    temperature = t;
    humidity = rh;
    return valid;
}

std::string Status() {
    char clock[80] = "时间待同步", climate[80] = "温湿度暂无读数";
    time_t now = time(nullptr);
    if (now >= 1704067200) {
        now += Settings("omni_ui").GetInt("utc_offset", 480) * 60;
        tm local{};
        gmtime_r(&now, &local);
        strftime(clock, sizeof(clock), "%Y年%m月%d日 %H:%M", &local);
    }
    float t, rh;
    if (ReadEnvironment(t, rh)) snprintf(climate, sizeof(climate), "温度%.1f摄氏度，相对湿度%.0f%%", t, rh);
    return std::string(clock) + "，" + climate;
}
}
