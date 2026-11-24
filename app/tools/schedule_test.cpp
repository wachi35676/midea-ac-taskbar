// Tests for app/src/schedule.cpp: firing rules, once-only, missed detection, save/load round trip.
// Saves to the real HKCU\Software\ACTaskbar key, so it backs up and restores the user's schedules.
#include <cstdio>
#include <ctime>

#include "../src/schedule.h"

namespace {
int g_fail = 0;
void Check(const char* what, bool ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}
time_t At(int ymd, int hh, int mm) {
    tm t{};
    t.tm_year = ymd / 10000 - 1900;
    t.tm_mon = (ymd / 100) % 100 - 1;
    t.tm_mday = ymd % 100;
    t.tm_hour = hh;
    t.tm_min = mm;
    t.tm_isdst = -1;
    return mktime(&t);
}
}  // namespace

int main() {
    const int day = 20261125;  // a Wednesday
    // Once, turn on at 07:30 on that date
    {
        Schedule s;
        ScheduleEntry e;
        e.turnOn = true; e.minutes = 7 * 60 + 30; e.once = true; e.date = day;
        s.entries.push_back(e);
        Check("once: nothing at 07:29", schedule::Due(s, At(day, 7, 29)) == 0);
        Check("once: fires at 07:30", schedule::Due(s, At(day, 7, 30)) == +1);
        Check("once: switched itself off", !s.entries[0].enabled);
        Check("once: not again at 07:31", schedule::Due(s, At(day, 7, 31)) == 0);
        Check("once: not on the next day", schedule::Due(s, At(schedule::AddDays(day, 1), 7, 30)) == 0);
        Check("once: shows as done, not missed", !schedule::Missed(s.entries[0], At(day, 9, 0)));
    }
    // Once, a late tick still fires inside the 2-minute window; outside it is missed
    {
        Schedule s;
        ScheduleEntry e;
        e.turnOn = false; e.minutes = 22 * 60; e.once = true; e.date = day;
        s.entries.push_back(e);
        Check("window: fires at 22:01 (late tick)", schedule::Due(s, At(day, 22, 1)) == -1);
        Schedule s2;
        s2.entries.push_back(e);
        Check("window: not at 22:02", schedule::Due(s2, At(day, 22, 2)) == 0);
        Check("missed after the window", schedule::Missed(s2.entries[0], At(day, 22, 5)));
        Check("missed on a later day", schedule::Missed(s2.entries[0], At(schedule::AddDays(day, 1), 8, 0)));
        Check("not missed before its time", !schedule::Missed(s2.entries[0], At(day, 21, 0)));
    }
    // Repeating on weekdays: Wed fires, Sat doesn't, fires again the next weekday
    {
        Schedule s;
        ScheduleEntry e;
        e.turnOn = true; e.minutes = 6 * 60; e.once = false; e.days = 0x1F;  // Mon-Fri
        s.entries.push_back(e);
        Check("repeat: Wed 06:00 fires", schedule::Due(s, At(day, 6, 0)) == +1);
        Check("repeat: stays enabled", s.entries[0].enabled);
        Check("repeat: once per day", schedule::Due(s, At(day, 6, 1)) == 0);
        Check("repeat: Thu 06:00 fires", schedule::Due(s, At(schedule::AddDays(day, 1), 6, 0)) == +1);
        Check("repeat: Sat 06:00 doesn't", schedule::Due(s, At(schedule::AddDays(day, 3), 6, 0)) == 0);
    }
    // Several entries the same day: each fires at its own minute
    {
        Schedule s;
        ScheduleEntry a; a.turnOn = true; a.minutes = 8 * 60; a.once = true; a.date = day;
        ScheduleEntry b; b.turnOn = false; b.minutes = 9 * 60; b.once = true; b.date = day;
        s.entries = {a, b};
        Check("multi: on at 08:00", schedule::Due(s, At(day, 8, 0)) == +1);
        Check("multi: off at 09:00", schedule::Due(s, At(day, 9, 0)) == -1);
    }
    // Sleep timer: fires when due; a stale one (PC asleep) is dropped
    {
        Schedule s;
        const time_t now = At(day, 23, 0);
        s.offAt = now;
        Check("sleep timer fires", schedule::Due(s, now) == -1 && s.offAt == 0);
        s.offAt = now - 3600;
        Check("stale sleep timer dropped", schedule::Due(s, now) == 0 && s.offAt == 0);
    }
    // Labels
    Check("DaysLabel weekdays", schedule::DaysLabel(0x1F) == L"Weekdays");
    Check("DaysLabel mixed", schedule::DaysLabel(0x15) == L"Mon, Wed, Fri");
    Check("AddDays across a month", schedule::AddDays(20261130, 1) == 20261201);
    Check("AddDays across a year", schedule::AddDays(20261231, 1) == 20270101);
    // Save/load round trip (restoring the user's own schedules afterwards)
    {
        const Schedule original = schedule::Load();
        Schedule s;
        ScheduleEntry a; a.turnOn = true; a.minutes = 7 * 60 + 15; a.once = true; a.date = 20271231;
        ScheduleEntry b; b.enabled = false; b.turnOn = false; b.minutes = 23 * 60 + 45; b.once = false; b.days = 0x60; b.lastFiredDay = 20261122;
        s.entries = {a, b};
        s.offAt = 0;
        schedule::Save(s);
        const Schedule back = schedule::Load();
        Check("round trip keeps every field", back.entries.size() == 2 && back.entries[0] == a && back.entries[1] == b);
        schedule::Save(original);
        Check("user's schedules restored", schedule::Load() == original);
    }
    printf(g_fail ? "\n%d FAILED\n" : "\nALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
