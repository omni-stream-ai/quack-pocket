#pragma once

#include "pocket_reminder_time.h"
#include <mutex>
#include <string>
#include <vector>

namespace pocket_reminders {

struct Request {
    std::string operation;
    int hour = -1;
    int minute = -1;
    std::string date;
    bool repeat_daily = false;
    std::string message;
    int id = 0;
};

struct Result {
    bool ok;
    std::string message;
};

std::optional<LocalTime> Now();

class Schedule {
public:
    static Schedule& Instance();
    bool Empty();
    Result Manage(const Request& request);
    std::optional<std::string> ListJson();
    std::vector<std::string> TakeDue(LocalTime now);

private:
    Schedule();
    struct Entry {
        int id, day, minute, last_rung;
        std::string message;
    };
    std::vector<Entry> entries_;
    int next_id_ = 1;
    bool loaded_ = false;
    std::mutex mutex_;

    static bool Save(const std::vector<Entry>& entries, int next_id);
    static std::string Describe(const Entry& entry);
    std::vector<Entry> Pending(std::optional<LocalTime> now) const;
};

} // namespace pocket_reminders
