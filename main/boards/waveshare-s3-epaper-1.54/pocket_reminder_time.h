#pragma once

#include <charconv>
#include <chrono>
#include <optional>
#include <string_view>

namespace pocket_reminders {

inline constexpr int daily = -1;
inline constexpr int minutes_per_day = 1440;
inline constexpr int first_day = 19723;  // 2024-01-01
inline constexpr int last_day = 47481;   // 2099-12-31

struct LocalTime {
    int day;
    int minute;
};

inline std::optional<int> Date(std::string_view date, int today) {
    if (date == "today") return today;
    if (date == "tomorrow") return today + 1;
    if (date == "day_after_tomorrow") return today + 2;
    if (date.size() != 10 || date[4] != '-' || date[7] != '-') return std::nullopt;
    auto number = [&](size_t start, size_t length) {
        int value = 0;
        const char* end = date.data() + start + length;
        auto [ptr, error] = std::from_chars(date.data() + start, end, value);
        return ptr == end && error == std::errc{} ? value : -1;
    };
    using namespace std::chrono;
    year_month_day calendar{year(number(0, 4)), month(number(5, 2)), day(number(8, 2))};
    if (!calendar.ok()) return std::nullopt;
    const auto days = sys_days(calendar).time_since_epoch().count();
    if (days < first_day || days > last_day) return std::nullopt;
    return days;
}

inline std::optional<int> ScheduleDay(LocalTime now, int minute, std::string_view date, bool repeat_daily) {
    if (minute < 0 || minute >= minutes_per_day) return std::nullopt;
    if (repeat_daily) return date.empty() ? std::optional<int>(daily) : std::nullopt;
    auto day = date.empty() ? std::optional<int>(now.day + (minute <= now.minute)) : Date(date, now.day);
    if (!day || *day > last_day || *day < now.day || (*day == now.day && minute <= now.minute)) return std::nullopt;
    return day;
}

enum class State { Pending, Due, Expired };

inline State Status(LocalTime now, int day, int minute, int last_rung) {
    if (day == daily)
        return now.minute == minute && last_rung < now.day ? State::Due : State::Pending;
    if (day < now.day || (day == now.day && minute < now.minute)) return State::Expired;
    return day == now.day && minute == now.minute ? State::Due : State::Pending;
}

} // namespace pocket_reminders
