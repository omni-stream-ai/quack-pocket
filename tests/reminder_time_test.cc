#include "pocket_reminder_time.h"
#include <cassert>

int main() {
    using namespace pocket_reminders;
    const int today = *Date("2026-11-24", first_day);
    constexpr int nine = 9 * 60;
    assert(ScheduleDay({today, 7 * 60}, nine, "tomorrow", false) == today + 1);
    assert(ScheduleDay({today, 7 * 60}, nine, "", false) == today);
    assert(ScheduleDay({today, nine}, nine, "", false) == today + 1);
    assert(!ScheduleDay({today, nine + 1}, nine, "today", false));
    assert(!ScheduleDay({today, nine}, nine, "tomorrow", true));
    assert(ScheduleDay({today, nine}, nine, "", true) == daily);
    assert(!ScheduleDay({last_day, nine}, nine, "tomorrow", false));
    assert(!Date("2026-02-29", today) && Date("2028-02-29", today));
    assert(Date("2099-12-31", today) == last_day && !Date("2100-01-01", today));
    const int year_end = *Date("2026-12-31", today);
    assert(ScheduleDay({year_end, 1439}, 0, "tomorrow", false) == Date("2027-01-01", today));

    assert(Status({today, nine - 1}, today, nine, daily) == State::Pending);
    assert(Status({today, nine}, today, nine, daily) == State::Due);
    assert(Status({today, nine + 1}, today, nine, daily) == State::Expired);
    assert(Status({today + 1, nine}, today, nine, daily) == State::Expired);
    assert(Status({today, nine}, daily, nine, today - 1) == State::Due);
    assert(Status({today, nine}, daily, nine, today) == State::Pending);
    assert(Status({today + 1, nine}, daily, nine, today) == State::Due);
}
