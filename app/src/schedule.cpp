#include "schedule.h"

#include <cstdio>
#include <cwchar>
#include <utility>

namespace schedule {
namespace {

constexpr wchar_t kKey[] = L"Software\\ACTaskbar";

DWORD Get(const wchar_t* name, DWORD def) {
    DWORD v = 0, size = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS ? v : def;
}
unsigned long long Get64(const wchar_t* name) {
    unsigned long long v = 0;
    DWORD size = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_QWORD, nullptr, &v, &size) == ERROR_SUCCESS ? v : 0;
}
std::wstring GetString(const wchar_t* name) {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || !size) return {};
    std::wstring s(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_SZ, nullptr, s.data(), &size) != ERROR_SUCCESS) return {};
    s.resize(wcslen(s.c_str()));
    return s;
}

tm Local(time_t t) {
    tm out{};
    localtime_s(&out, &t);
    return out;
}
int Ymd(const tm& t) { return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday; }
int Weekday(const tm& t) { return (t.tm_wday + 6) % 7; }  // Monday = 0
tm FromYmd(int ymd) {
    tm t{};
    t.tm_year = ymd / 10000 - 1900;
    t.tm_mon = (ymd / 100) % 100 - 1;
    t.tm_mday = ymd % 100;
    t.tm_hour = 12;  // midday: immune to DST edges
    t.tm_isdst = -1;
    return t;
}

}  // namespace

int Today(time_t now) { return Ymd(Local(now)); }

int AddDays(int ymd, int days) {
    tm t = FromYmd(ymd);
    t.tm_mday += days;
    mktime(&t);  // normalises month/year
    return Ymd(t);
}

std::wstring DateLabel(int ymd) {
    tm t = FromYmd(ymd);
    mktime(&t);
    static const wchar_t* wd[] = {L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"};
    static const wchar_t* mo[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    const int today = Today(time(nullptr));
    wchar_t b[40];
    if (ymd == today) swprintf(b, 40, L"Today, %d %s", t.tm_mday, mo[t.tm_mon]);
    else if (ymd == AddDays(today, 1)) swprintf(b, 40, L"Tomorrow, %d %s", t.tm_mday, mo[t.tm_mon]);
    else if (ymd / 10000 == today / 10000) swprintf(b, 40, L"%s %d %s", wd[t.tm_wday], t.tm_mday, mo[t.tm_mon]);
    else swprintf(b, 40, L"%s %d %s %d", wd[t.tm_wday], t.tm_mday, mo[t.tm_mon], t.tm_year + 1900);
    return b;
}

std::wstring DaysLabel(uint8_t days) {
    days &= 0x7F;
    if (days == 0x7F) return L"Every day";
    if (days == 0x1F) return L"Weekdays";
    if (days == 0x60) return L"Weekends";
    static const wchar_t* names[] = {L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat", L"Sun"};
    std::wstring s;
    for (int i = 0; i < 7; ++i)
        if (days & (1 << i)) s += (s.empty() ? L"" : L", ") + std::wstring(names[i]);
    return s.empty() ? L"No days" : s;
}

bool Missed(const ScheduleEntry& e, time_t now) {
    if (!e.once || e.lastFiredDay == e.date) return false;
    const tm t = Local(now);
    const int today = Ymd(t), minute = t.tm_hour * 60 + t.tm_min;
    return e.date < today || (e.date == today && minute >= e.minutes + 2);
}

bool Next(const Schedule& s, time_t now, time_t& when, bool& turnOn) {
    bool found = false;
    auto consider = [&](time_t t, bool on) {
        if (t > now && (!found || t < when)) {
            when = t;
            turnOn = on;
            found = true;
        }
    };
    if (s.offAt > now) consider(s.offAt, false);
    const int today = Today(now);
    for (const auto& e : s.entries) {
        if (!e.enabled) continue;
        if (e.once) {
            tm t = FromYmd(e.date);
            t.tm_hour = e.minutes / 60;
            t.tm_min = e.minutes % 60;
            t.tm_isdst = -1;
            consider(mktime(&t), e.turnOn);
            continue;
        }
        for (int d = 0; d < 8; ++d) {  // the first day from today with one of its weekdays, still ahead
            tm t = FromYmd(today);
            t.tm_mday += d;
            t.tm_hour = e.minutes / 60;
            t.tm_min = e.minutes % 60;
            t.tm_isdst = -1;
            const time_t tt = mktime(&t);
            if (!(e.days & (1 << Weekday(t))) || tt <= now) continue;
            consider(tt, e.turnOn);
            break;
        }
    }
    return found;
}

Schedule Load() {
    Schedule s;
    s.offAt = static_cast<time_t>(Get64(L"SleepOffAt"));
    // Format: entries separated by ';', fields by ',': enabled,turnOn,minutes,once,date,days,lastFiredDay
    const std::wstring list = GetString(L"Schedules");
    size_t pos = 0;
    while (pos < list.size()) {
        size_t end = list.find(L';', pos);
        if (end == std::wstring::npos) end = list.size();
        const std::wstring item = list.substr(pos, end - pos);
        int en = 0, on = 0, mins = 0, once = 0, date = 0, days = 0, fired = -1;
        if (swscanf_s(item.c_str(), L"%d,%d,%d,%d,%d,%d,%d", &en, &on, &mins, &once, &date, &days, &fired) >= 6) {
            ScheduleEntry e;
            e.enabled = en != 0;
            e.turnOn = on != 0;
            e.minutes = (mins % 1440 + 1440) % 1440;
            e.once = once != 0;
            e.date = date;
            e.days = static_cast<uint8_t>(days & 0x7F);
            e.lastFiredDay = fired;
            s.entries.push_back(e);
        }
        pos = end + 1;
    }
    // Carry over the earlier fixed daily on/off rows, once.
    if (list.empty() && !Get(L"SchedulesMigrated", 0)) {
        for (auto [prefix, on] : {std::pair{L"On", true}, std::pair{L"Off", false}}) {
            const std::wstring p(prefix);
            if (!Get((p + L"Enabled").c_str(), 0)) continue;
            ScheduleEntry e;
            e.turnOn = on;
            e.once = false;
            e.minutes = static_cast<int>(Get((p + L"Minutes").c_str(), on ? 7 * 60 : 23 * 60));
            e.days = static_cast<uint8_t>(Get((p + L"Days").c_str(), 0x7F));
            s.entries.push_back(e);
        }
    }
    return s;
}

void Save(const Schedule& s) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    const unsigned long long off = static_cast<unsigned long long>(s.offAt);
    RegSetValueExW(k, L"SleepOffAt", 0, REG_QWORD, reinterpret_cast<const BYTE*>(&off), sizeof(off));
    std::wstring list;
    for (const auto& e : s.entries) {
        wchar_t b[96];
        swprintf(b, 96, L"%d,%d,%d,%d,%d,%d,%d;", e.enabled ? 1 : 0, e.turnOn ? 1 : 0, e.minutes, e.once ? 1 : 0, e.date,
                 static_cast<int>(e.days), e.lastFiredDay);
        list += b;
    }
    RegSetValueExW(k, L"Schedules", 0, REG_SZ, reinterpret_cast<const BYTE*>(list.c_str()),
                   static_cast<DWORD>((list.size() + 1) * sizeof(wchar_t)));
    const DWORD migrated = 1;
    RegSetValueExW(k, L"SchedulesMigrated", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&migrated), sizeof(migrated));
    RegCloseKey(k);
}

int Due(Schedule& s, time_t now) {
    if (s.offAt && now >= s.offAt) {
        // A sleep timer that expired more than 10 minutes ago (PC was asleep) is dropped, not fired late.
        const bool fresh = now - s.offAt < 600;
        s.offAt = 0;
        if (fresh) return -1;
    }
    const tm t = Local(now);
    const int minute = t.tm_hour * 60 + t.tm_min, today = Ymd(t), bit = 1 << Weekday(t);
    for (auto& e : s.entries) {
        if (!e.enabled || e.lastFiredDay == today) continue;
        if (e.once ? e.date != today : !(e.days & bit)) continue;
        if (minute >= e.minutes && minute < e.minutes + 2) {  // a 2-minute window, in case a tick is late
            e.lastFiredDay = today;
            if (e.once) e.enabled = false;  // done
            return e.turnOn ? +1 : -1;
        }
    }
    return 0;
}

}  // namespace schedule
