#pragma once
// App-side timers: a one-shot sleep timer, plus any number of scheduled turn-ons / turn-offs, each either
// once on a specific date or repeating on chosen weekdays. They run from this app (so they need the PC on
// and the app running) and are saved under HKCU\Software\ACTaskbar.
#include <windows.h>

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

struct ScheduleEntry {
    bool enabled = true;
    bool turnOn = true;     // turn the AC on (true) or off (false)
    int minutes = 7 * 60;   // time of day
    bool once = true;       // once on `date`, or every day in `days`
    int date = 0;           // yyyymmdd (once)
    uint8_t days = 0x1F;    // bit 0 = Monday ... bit 6 = Sunday (repeating)
    int lastFiredDay = -1;  // yyyymmdd it last ran

    bool operator==(const ScheduleEntry&) const = default;
};

struct Schedule {
    time_t offAt = 0;  // sleep timer: turn off at this time (0 = none)
    std::vector<ScheduleEntry> entries;

    bool operator==(const Schedule&) const = default;
};

namespace schedule {

Schedule Load();
void Save(const Schedule& s);

// Checks the clock; returns +1 to turn the AC on, -1 to turn it off, 0 for nothing. Updates `s`
// (one-time entries switch themselves off once they have run).
int Due(Schedule& s, time_t now);

// Helpers for the panel.
int Today(time_t now);                     // yyyymmdd
int AddDays(int yyyymmdd, int days);       // calendar arithmetic
std::wstring DateLabel(int yyyymmdd);      // "Tue 25 Nov" (with the year if it isn't this year)
std::wstring DaysLabel(uint8_t days);      // "Every day", "Weekdays", "Weekends", "Mon, Wed, Fri"
bool Missed(const ScheduleEntry& e, time_t now);  // a one-time entry whose moment has passed unrun
// The next moment the AC will be switched by a timer or an enabled schedule. False when nothing is set.
bool Next(const Schedule& s, time_t now, time_t& when, bool& turnOn);

}  // namespace schedule
