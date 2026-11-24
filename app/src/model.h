#pragma once
// Turning snapshots into what the strip and panel show, and applying a change to a local state copy
// (so clicks show at once, before the AC confirms).
#include <cmath>
#include <cstdio>
#include <string>

#include "../../native/src/midea/ac_controller.h"
#include "widget.h"
#include "worker.h"

namespace model {

inline std::wstring Deg(double t) {
    wchar_t b[16];
    if (t == static_cast<int>(t)) swprintf(b, 16, L"%d°", static_cast<int>(t));
    else swprintf(b, 16, L"%.1f°", t);
    return b;
}

[[maybe_unused]] inline gfx::Icon ModeIcon(int mode) {
    switch (mode) {
        case 1: return gfx::Icon::Auto;
        case 2: return gfx::Icon::Cool;
        case 3: return gfx::Icon::Dry;
        case 4: return gfx::Icon::Heat;
        case 5: return gfx::Icon::Fan;
        default: return gfx::Icon::Auto;
    }
}

inline const wchar_t* ModeVerb(int mode) {
    switch (mode) {
        case 1: return L"Auto";
        case 2: return L"Cooling";
        case 3: return L"Drying";
        case 4: return L"Heating";
        case 5: return L"Fan only";
        default: return L"Running";
    }
}

inline bool ParseBool(const std::string& v) { return v == "true" || v == "1" || v == "on"; }

// Mirrors what the AC does with each change, including the side effects measured in docs/AC_API.md.
inline void Apply(midea::ac::State& s, const midea::ac::Changes& changes) {
    for (const auto& [k, v] : changes) {
        if (k == "power") s.power = ParseBool(v);
        else if (k == "target_temperature") s.targetTemperature = atof(v.c_str());
        else if (k == "mode") {
            const char* names[] = {"", "auto", "cool", "dry", "heat", "fan"};
            for (int i = 1; i <= 5; ++i) if (v == names[i]) s.mode = i;
        } else if (k == "fan_speed") {
            const struct { const char* n; int v; } fans[] = {{"silent", 20}, {"low", 40}, {"medium", 60}, {"high", 80}, {"max", 100}, {"auto", 102}};
            bool named = false;
            for (auto& f : fans) if (v == f.n) { s.fanSpeed = f.v; named = true; }
            if (!named) s.fanSpeed = atoi(v.c_str());
        } else if (k == "swing") {
            if (v == "off") s.swing = 0;
            else if (v == "horizontal") { s.swing = 3; s.horizontalAngle = 0; }
            else if (v == "vertical") { s.swing = 12; s.verticalAngle = 0; }
            else if (v == "both") { s.swing = 15; s.verticalAngle = s.horizontalAngle = 0; }
        } else if (k == "vertical_angle") {
            if (v == "release") s.verticalAngle = 0;
            else { s.verticalAngle = atoi(v.c_str()); s.swing = 0; }
        } else if (k == "horizontal_angle") {
            s.horizontalAngle = v == "release" ? 0 : atoi(v.c_str());
        } else if (k == "breeze_away") {
            s.breezeAway = ParseBool(v);
            if (s.breezeAway) { s.fanSpeed = 1; s.swing = 0; }
        }
        else if (k == "eco") s.eco = ParseBool(v);
        else if (k == "turbo") s.turbo = ParseBool(v);
        else if (k == "sleep") s.sleep = ParseBool(v);
        else if (k == "purifier") s.purifier = ParseBool(v);
        else if (k == "freeze_protection") s.freezeProtection = ParseBool(v);
        else if (k == "fahrenheit") s.fahrenheit = ParseBool(v);
        else if (k == "beep") s.sound = ParseBool(v);
    }
}

inline std::wstring Power(int w) {
    wchar_t b[24];
    if (w >= 1000) swprintf(b, 24, L"%.1f kW", w / 1000.0);
    else swprintf(b, 24, L"%d W", w);
    return b;
}

// What the taskbar strip says and how its glyph moves. Running: "Cooling to 24°" / "Room 26.3° · 1.2 kW",
// animated. Off: "Off" / "Room 24.6°", still. Offline / not set up: a quiet grey glyph.
inline StripModel Strip(const Snapshot& snap) {
    StripModel m;
    const auto& s = snap.state;
    m.scene.mode = (s.mode >= 1 && s.mode <= 5) ? s.mode : art::Cool;
    m.scene.fromMode = m.scene.mode;
    m.scene.targetTemperature = static_cast<float>(s.targetTemperature);
    const std::wstring room = s.indoorTemperature ? L"Room " + Deg(*s.indoorTemperature) : L"Room --";

    if (snap.status == Snapshot::Status::NoCredentials) {
        m.scene.online = false; m.scene.power = 0;
        m.line1 = L"AC"; m.line2 = L"Not set up"; m.dim = true;
        m.tooltip = L"Air conditioner isn't paired yet.\nRun tools\\get_key.py once to create ac_creds.json.";
        return m;
    }
    if (!snap.hasData || (snap.status != Snapshot::Status::Online && snap.status != Snapshot::Status::Connecting)) {
        m.scene.online = snap.status == Snapshot::Status::Connecting;
        m.scene.power = 0;
        m.line1 = L"AC";
        m.line2 = snap.status == Snapshot::Status::Connecting ? L"Connecting…" : L"Can't reach it";
        m.dim = true;
        m.tooltip = snap.status == Snapshot::Status::Connecting ? L"Connecting to the air conditioner…"
                                                                 : L"Can't reach the air conditioner. Retrying.";
        if (!snap.error.empty()) m.tooltip += L"\n" + std::wstring(snap.error.begin(), snap.error.end());
        return m;
    }

    const bool on = s.power;
    m.scene.power = on ? 1.0f : 0.0f;
    m.scene.fan = s.breezeAway ? 0.15f : (s.fanSpeed == 102 ? 0.55f : std::fmax(0.15f, std::fmin(1.0f, s.fanSpeed / 100.0f)));
    m.scene.eco = s.eco; m.scene.turbo = s.turbo; m.scene.sleep = s.sleep;
    m.animate = on;
    std::wstring extra;
    if (on && s.outdoorUnitPower && *s.outdoorUnitPower >= 10) extra = L" · " + Power(*s.outdoorUnitPower);

    // At a glance: the room temperature big (what you look for), and what the AC is doing under it.
    const std::wstring roomBig = s.indoorTemperature ? Deg(*s.indoorTemperature) : L"--";
    const wchar_t* modeName = s.mode == 1 ? L"Auto" : s.mode == 2 ? L"Cool" : s.mode == 3 ? L"Dry" : s.mode == 4 ? L"Heat" : L"Fan";
    m.line1 = roomBig;
    if (s.errorCode) {
        m.line2 = L"Unit code " + std::to_wstring(s.errorCode);
        m.alert = true;
    } else if (!on) {
        m.line2 = L"AC off";
        m.dim = true;
    } else if (s.mode == 5) {
        m.line2 = L"Fan only";
    } else {
        m.line2 = std::wstring(modeName) + L" → " + Deg(s.targetTemperature);
    }
    (void)extra;
    m.tooltip = std::wstring(L"Air conditioner: ") +
                (on ? (s.mode == 5 ? std::wstring(L"fan only") : std::wstring(ModeVerb(s.mode)) + L" to " + Deg(s.targetTemperature)) : L"off");
    m.tooltip += L"\n" + room;
    if (s.outdoorTemperature) m.tooltip += L", outside " + Deg(*s.outdoorTemperature);
    if (on && s.outdoorUnitPower) m.tooltip += L"\nDrawing " + Power(*s.outdoorUnitPower);
    m.tooltip += L"\nClick for controls";
    return m;
}

}  // namespace model
