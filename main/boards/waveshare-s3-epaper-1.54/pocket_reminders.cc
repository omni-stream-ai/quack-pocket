#include "pocket_reminders.h"
#include "pocket_media.h"
#include "settings.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <limits>
#include <esp_log.h>

namespace pocket_reminders {
namespace {
constexpr size_t capacity = 8;
constexpr size_t max_message_bytes = 144;
constexpr const char* storage_key = "reminders";

bool ValidMessage(const std::string& message) {
    return message.size() <= max_message_bytes &&
        std::none_of(message.begin(), message.end(), [](unsigned char c) { return c < 32 || c == 127; });
}

std::string FormatDate(int day, const char* format) {
    const time_t time = static_cast<int64_t>(day) * 86400;
    tm date{};
    gmtime_r(&time, &date);
    char text[28];
    strftime(text, sizeof(text), format, &date);
    return text;
}
}

std::optional<LocalTime> Now() {
    constexpr int64_t seconds_per_day = 86400;
    const int64_t utc = time(nullptr);
    if (utc < first_day * seconds_per_day || utc >= (last_day + 1) * seconds_per_day) return std::nullopt;
    const auto local_minutes = utc / 60 + Settings("omni_ui").GetInt("utc_offset", 480);
    const int day = local_minutes / minutes_per_day;
    if (day < first_day || day > last_day) return std::nullopt;
    return LocalTime{day, static_cast<int>(local_minutes % minutes_per_day)};
}

Schedule& Schedule::Instance() {
    static Schedule schedule;
    return schedule;
}

Schedule::Schedule() {
    const auto saved = pocket_media::Read(storage_key);
    if (saved.empty()) {
        loaded_ = true;
        return;
    }
    pocket_media::Json data(cJSON_Parse(saved.c_str()), cJSON_Delete);
    auto* items = cJSON_GetObjectItemCaseSensitive(data.get(), "entries");
    const int next_id = pocket_media::Number(data.get(), "next_id", 0);
    if (!cJSON_IsArray(items) || cJSON_GetArraySize(items) > static_cast<int>(capacity) || next_id <= 0) return;
    std::vector<Entry> entries;
    const cJSON* item;
    cJSON_ArrayForEach(item, items) {
        Entry entry{pocket_media::Number(item, "id", 0), pocket_media::Number(item, "day", 0),
            pocket_media::Number(item, "minute", -1), pocket_media::Number(item, "last_rung", daily),
            pocket_media::Text(item, "message")};
        if (entry.id <= 0 || entry.id >= next_id || entry.minute < 0 || entry.minute >= minutes_per_day ||
            (entry.day != daily && (entry.day < first_day || entry.day > last_day)) ||
            !ValidMessage(entry.message) ||
            std::any_of(entries.begin(), entries.end(), [&](const Entry& other) { return other.id == entry.id; })) return;
        entries.push_back(std::move(entry));
    }
    entries_ = std::move(entries);
    next_id_ = next_id;
    loaded_ = true;
}

bool Schedule::Empty() {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.empty();
}

bool Schedule::Save(const std::vector<Entry>& entries, int next_id) {
    pocket_media::Json data(cJSON_CreateObject(), cJSON_Delete);
    if (!data || !cJSON_AddNumberToObject(data.get(), "next_id", next_id)) return false;
    auto* items = cJSON_AddArrayToObject(data.get(), "entries");
    if (!items) return false;
    for (const auto& entry : entries) {
        auto* item = cJSON_CreateObject();
        if (!item) return false;
        cJSON_AddItemToArray(items, item);
        if (!cJSON_AddNumberToObject(item, "id", entry.id) ||
            !cJSON_AddNumberToObject(item, "day", entry.day) ||
            !cJSON_AddNumberToObject(item, "minute", entry.minute) ||
            !cJSON_AddNumberToObject(item, "last_rung", entry.last_rung) ||
            !cJSON_AddStringToObject(item, "message", entry.message.c_str())) return false;
    }
    return pocket_media::Save(storage_key, data.get());
}

std::string Schedule::Describe(const Entry& entry) {
    char hours[16];
    snprintf(hours, sizeof(hours), "%02d:%02d", entry.minute / 60, entry.minute % 60);
    std::string result = std::to_string(entry.id) + " 号，";
    result += entry.day == daily ? "每天 " : FormatDate(entry.day, "%Y年%m月%d日 ");
    result += hours;
    result += entry.message.empty() ? " 的闹钟" : "，提醒" + entry.message;
    return result;
}

std::vector<Schedule::Entry> Schedule::Pending(std::optional<LocalTime> now) const {
    auto entries = entries_;
    if (now) {
        std::erase_if(entries, [&](const Entry& entry) {
            return Status(*now, entry.day, entry.minute, entry.last_rung) == State::Expired;
        });
    }
    return entries;
}

std::optional<std::string> Schedule::ListJson() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!loaded_) return std::nullopt;
    const auto now = Now();
    pocket_media::Json data(cJSON_CreateObject(), cJSON_Delete);
    if (!data || !cJSON_AddStringToObject(data.get(), "today", now ? FormatDate(now->day, "%Y-%m-%d").c_str() : ""))
        return std::nullopt;
    auto* items = cJSON_AddArrayToObject(data.get(), "entries");
    if (!items) return std::nullopt;
    for (const auto& entry : Pending(now)) {
        auto* item = cJSON_CreateObject();
        if (!item) return std::nullopt;
        cJSON_AddItemToArray(items, item);
        if (!cJSON_AddNumberToObject(item, "id", entry.id) ||
            !cJSON_AddNumberToObject(item, "hour", entry.minute / 60) ||
            !cJSON_AddNumberToObject(item, "minute", entry.minute % 60) ||
            !cJSON_AddBoolToObject(item, "daily", entry.day == daily) ||
            !cJSON_AddStringToObject(item, "date", entry.day == daily ? "" : FormatDate(entry.day, "%Y-%m-%d").c_str()) ||
            !cJSON_AddStringToObject(item, "message", entry.message.c_str())) return std::nullopt;
    }
    std::unique_ptr<char, decltype(&cJSON_free)> json(cJSON_PrintUnformatted(data.get()), cJSON_free);
    if (!json) return std::nullopt;
    return std::string(json.get());
}

Result Schedule::Manage(const Request& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!loaded_) return {false, "无法读取已保存的闹钟和提醒。"};
    const auto now = Now();
    auto list = [](const std::vector<Entry>& entries) {
        std::string result;
        for (const auto& entry : entries) {
            if (!result.empty()) result += "\n";
            result += Describe(entry);
        }
        return result;
    };
    auto entries = Pending(now);
    if (request.operation == "list") return {true, entries.empty() ? "还没有闹钟或定时提醒。" : list(entries)};
    if (!ValidMessage(request.message)) return {false, "提醒内容须为单行，最长 144 字节。"};
    const bool editing = request.operation == "edit";
    if (request.operation == "set" || editing) {
        if (!now) return {false, "设备时间还没校准，请联网同步时间后再设置。"};
        if (request.hour < 0 || request.hour > 23 || request.minute < -1 || request.minute > 59)
            return {false, "请指定有效的时间。"};
        const int minute = request.hour * 60 + std::max(0, request.minute);
        const auto day = ScheduleDay(*now, minute, request.date, request.repeat_daily);
        if (!day) return {false, "请选择有效的未来时间。"};
        auto existing = std::find_if(entries.begin(), entries.end(), [&](const Entry& entry) {
            return entry.day == *day && entry.minute == minute && entry.message == request.message;
        });
        if (existing != entries.end() && (!editing || existing->id == request.id))
            return {true, "已设置 " + Describe(*existing) + "。"};
        if (editing && existing != entries.end()) return {false, "已有一项相同的闹钟或提醒。"};
        // An alarm created in its own minute starts tomorrow.
        int last_rung = *day == daily && minute == now->minute ? now->day : daily;
        int next = next_id_;
        Entry entry{editing ? request.id : next_id_, *day, minute, last_rung, request.message};
        if (editing) {
            auto original = std::find_if(entries.begin(), entries.end(), [&](const Entry& item) { return item.id == request.id; });
            if (original == entries.end()) return {false, "这项闹钟或提醒已不存在，请刷新列表。"};
            if (*day == daily && original->day == daily && original->minute == minute)
                entry.last_rung = std::max(last_rung, original->last_rung);
            *original = entry;
        } else {
            if (entries.size() >= capacity || next_id_ == std::numeric_limits<int>::max())
                return {false, "闹钟和提醒已满，请先取消一项。"};
            entries.push_back(entry);
            ++next;
        }
        if (!Save(entries, next)) return {false, "保存提醒失败，请重试。"};
        next_id_ = next;
        entries_ = std::move(entries);
        return {true, "已设置 " + Describe(entry) + "。"};
    }
    if (request.operation != "cancel") return {false, "可以设置、查询或取消闹钟与提醒。"};
    if (!request.id && request.hour < 0 && request.message.empty()) return {false, "请说要取消的编号、时间或提醒内容。"};
    std::optional<int> day;
    if (!request.id && !request.date.empty()) {
        if (!now) return {false, "设备时间还没校准，请按编号取消。"};
        day = Date(request.date, now->day);
        if (!day) return {false, "请指定有效日期，或按编号取消。"};
    }
    auto matches = [&](const Entry& entry) {
        if (request.id) return entry.id == request.id;
        return (request.hour < 0 || entry.minute / 60 == request.hour) &&
            (request.minute < 0 || entry.minute % 60 == request.minute) &&
            (!day || entry.day == *day) && (!request.repeat_daily || entry.day == daily) &&
            (request.message.empty() || entry.message.find(request.message) != std::string::npos);
    };
    std::vector<Entry> found;
    std::copy_if(entries.begin(), entries.end(), std::back_inserter(found), matches);
    if (found.empty()) return {false, "没有找到要取消的闹钟或提醒。"};
    if (found.size() > 1) return {false, "找到多项，请说要取消的编号。\n" + list(found)};
    std::erase_if(entries, matches);
    if (!Save(entries, next_id_)) return {false, "取消失败，请重试。"};
    entries_ = std::move(entries);
    return {true, "已取消 " + Describe(found.front()) + "。"};
}

std::vector<std::string> Schedule::TakeDue(LocalTime now) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> due;
    bool changed = false;
    for (auto& entry : entries_) {
        if (Status(now, entry.day, entry.minute, entry.last_rung) == State::Due) {
            due.push_back(entry.message);
            if (entry.day == daily) {
                entry.last_rung = now.day;
                changed = true;
            }
        }
    }
    auto removed = std::erase_if(entries_, [&](const Entry& entry) {
        return entry.day != daily && Status(now, entry.day, entry.minute, entry.last_rung) != State::Pending;
    });
    if ((changed || removed) && !Save(entries_, next_id_))
        ESP_LOGW("reminders", "Could not persist fired or expired reminders");
    return due;
}

} // namespace pocket_reminders
