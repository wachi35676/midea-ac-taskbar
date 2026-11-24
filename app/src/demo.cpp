#include "demo.h"

#include <cmath>
#include <ctime>

namespace demo {
namespace {

using midea::ac::FanSpeed;
using midea::ac::Mode;
using midea::ac::Swing;

struct Scenario {
    const wchar_t* name;
    int mode;
    bool power;
    double target, room, outside;
    int watts, hz, amps;
    double indoorCoil, outdoorCoil;
    int discharge;
};

// Plausible numbers for each mode of a 1.5 t inverter split.
const Scenario kScenarios[] = {
    {L"cool", 2, true, 22.0, 26.4, 33.5, 780, 46, 4, 11.5, 46.0, 62},
    {L"heat", 4, true, 26.0, 19.2, 8.5, 1240, 58, 6, 44.5, 2.5, 71},
    {L"dry", 3, true, 24.0, 25.6, 29.0, 520, 30, 3, 14.0, 38.5, 55},
    {L"fan", 5, true, 24.0, 24.8, 27.5, 45, 0, 1, 24.8, 27.5, 27},
    {L"auto", 1, true, 24.0, 23.2, 24.0, 610, 38, 3, 20.5, 30.0, 49},
    {L"off", 4, false, 24.0, 24.6, 19.0, 3, 0, 1, 24.0, 19.0, 19},
};

const Scenario& Pick(const std::wstring& mode) {
    for (const auto& s : kScenarios)
        if (mode == s.name) return s;
    return kScenarios[0];
}

}  // namespace

Snapshot MakeSnapshot(const std::wstring& mode) {
    const Scenario& sc = Pick(mode);
    Snapshot s;
    s.status = Snapshot::Status::Online;
    s.hasData = true;
    const bool offline = mode == L"offline", unpaired = mode == L"unpaired";
    if (offline) {
        s.status = Snapshot::Status::Offline;
        s.error = "no response from the AC";
    }
    if (unpaired) {
        s.status = Snapshot::Status::NoCredentials;
        s.hasData = false;
    }
    s.ip = "192.168.1.50";
    s.updatedAt = "12:00:00";

    auto& c = s.caps;
    c.received = true;
    c.modes = {Mode::Fan, Mode::Dry, Mode::Cool, Mode::Heat, Mode::Auto};
    c.swingModes = {Swing::Off, Swing::Horizontal, Swing::Vertical, Swing::Both};
    c.fanSpeeds = {FanSpeed::Silent, FanSpeed::Low, FanSpeed::Medium, FanSpeed::High, FanSpeed::Auto, FanSpeed::Max};
    c.minTemperature = 16;
    c.maxTemperature = 30;
    c.customFanSpeed = c.eco = c.turboCool = c.turboHeat = c.freezeProtection = c.purifier = true;
    c.verticalAngle = c.horizontalAngle = c.selfClean = c.breezeAway = true;

    auto& st = s.state;
    st.power = sc.power;
    st.mode = sc.mode;
    st.targetTemperature = sc.target;
    st.fanSpeed = static_cast<int>(FanSpeed::Auto);
    st.swing = static_cast<int>(Swing::Vertical);
    st.displayOn = true;
    st.indoorTemperature = sc.room;
    st.outdoorTemperature = sc.outside;
    st.outdoorUnitPower = sc.watts;
    st.compressorFrequency = sc.hz;
    st.targetCompressorFrequency = sc.hz ? sc.hz + 6 : 0;
    st.compressorCurrent = sc.amps;
    st.compressorVoltage = 231;
    st.indoorCoil = sc.indoorCoil;
    st.outdoorCoil = sc.outdoorCoil;
    st.dischargePipe = sc.discharge;
    st.outdoorFan = sc.power && sc.hz ? 720 : 0;
    st.indoorFan = sc.power ? 880 : 0;
    st.sound = true;  // beep on, as in the screenshots of the Features tab
    return s;
}

Schedule MakeSchedule() {
    Schedule sch;
    const time_t now = time(nullptr);
    const int today = schedule::Today(now);

    ScheduleEntry morning;  // weekdays 07:30, turn on
    morning.turnOn = true;
    morning.minutes = 7 * 60 + 30;
    morning.once = false;
    morning.days = 0x1F;

    ScheduleEntry night;  // every day 23:00, turn off
    night.turnOn = false;
    night.minutes = 23 * 60;
    night.once = false;
    night.days = 0x7F;

    ScheduleEntry guests;  // once, tomorrow evening, turn on
    guests.turnOn = true;
    guests.minutes = 18 * 60 + 30;
    guests.once = true;
    guests.date = schedule::AddDays(today, 1);

    ScheduleEntry weekend;  // weekends 10:00, turn on
    weekend.turnOn = true;
    weekend.minutes = 10 * 60;
    weekend.once = false;
    weekend.days = 0x60;

    // The order the app keeps them in: repeating by time, then one-time by date.
    sch.entries = {morning, weekend, night, guests};
    return sch;
}

std::vector<float> PowerHistory(const std::wstring& mode) {
    const Scenario& sc = Pick(mode);
    std::vector<float> w;
    for (int i = 0; i < 60; ++i) {
        // Start-up surge, a settled draw with a slow wobble, and a short dip when the compressor eases off.
        const double t = i / 59.0;
        double v = sc.watts * (0.45 + 0.55 * std::min(1.0, t * 3.0));
        v += 0.07 * sc.watts * std::sin(t * 17.0) + 0.04 * sc.watts * std::sin(t * 41.0);
        if (!sc.power) v = sc.watts;
        w.push_back(static_cast<float>(std::fmax(2.0, v)));
    }
    return w;
}

}  // namespace demo
