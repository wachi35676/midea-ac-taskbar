#include "ac_controller.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>

#include "discover.h"

namespace midea::ac {
namespace {

struct Named { const char* name; int value; };
const Named kModes[] = {{"auto", 1}, {"cool", 2}, {"dry", 3}, {"heat", 4}, {"fan", 5}, {"smart_dry", 6}};
const Named kFans[] = {{"silent", 20}, {"low", 40}, {"medium", 60}, {"high", 80}, {"max", 100}, {"auto", 102}};
const Named kSwings[] = {{"off", 0x0}, {"horizontal", 0x3}, {"vertical", 0xC}, {"both", 0xF}};

template <size_t N>
bool Lookup(const Named (&t)[N], const std::string& s, int& out) {
    for (const auto& n : t) if (s == n.name) { out = n.value; return true; }
    return false;
}
template <size_t N>
const char* NameOf(const Named (&t)[N], int v, const char* fallback) {
    for (const auto& n : t) if (n.value == v) return n.name;
    return fallback;
}

bool ParseBool(const std::string& s, bool& out) {
    if (s == "true" || s == "1" || s == "on") { out = true; return true; }
    if (s == "false" || s == "0" || s == "off") { out = false; return true; }
    return false;
}

bool ParseNumber(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = strtod(s.c_str(), &end);
    return end && *end == 0 && std::isfinite(out);
}

std::string Num(double v) {
    char b[32];
    snprintf(b, sizeof(b), "%.10g", v);
    return b;
}
template <class T>
std::string Opt(const std::optional<T>& v) { return v ? Num(static_cast<double>(*v)) : "null"; }
const char* Bool(bool b) { return b ? "true" : "false"; }

std::string NowUtc() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_s(&tm, &t);
    char b[32];
    strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return b;
}

void SetErr(std::string* e, const std::string& m) {
    if (e) *e = m;
}

}  // namespace

const char* ModeName(int m) { return NameOf(kModes, m, "unknown"); }
const char* FanName(int f) { return NameOf(kFans, f, "custom"); }
const char* SwingName(int s) { return NameOf(kSwings, s, "unknown"); }

Controller::Controller(std::wstring credsPath) : credsPath_(std::move(credsPath)) {}

const State& Controller::state() const {
    static const State empty;
    return dev_ ? dev_->state() : empty;
}
const Capabilities& Controller::capabilities() const {
    static const Capabilities empty;
    return dev_ ? dev_->capabilities() : empty;
}

bool Controller::Rediscover() {
    for (const auto& d : Discover(3000, 3)) {
        if (d.id != creds_.id) continue;
        if (d.ip != creds_.ip || (d.port && d.port != creds_.port)) {
            creds_.ip = d.ip;
            if (d.port) creds_.port = d.port;
            SaveCreds(credsPath_, creds_);
        }
        return true;
    }
    return false;
}

bool Controller::Connect(std::string* error) {
    if (!LoadCreds(credsPath_, creds_, error)) return false;
    Bytes token, key;
    if (!FromHex(creds_.tokenHex, token) || !FromHex(creds_.keyHex, key))
        return SetErr(error, "token or key is not valid hex"), false;

    for (int attempt = 0; attempt < 2; ++attempt) {
        dev_ = std::make_unique<Device>(creds_.ip, creds_.port, creds_.id);
        // Groups that carry data on this unit: compressor/coils (1), indoor fan (2), energy counters (4,
        // unreliable), outdoor fan/defrost (5), live power draw in watts (7), louver angles (11).
        for (int g : {1, 2, 4, 5, 7, 11}) dev_->EnableGroup(g, true);
        dev_->SetBeep(beep_);  // a new connection starts with the remembered beep choice
        std::string e;
        if (dev_->Authenticate(token, key, &e) && dev_->GetCapabilities(&e)) return true;
        SetErr(error, e.empty() ? "AC unreachable" : e);
        if (attempt == 0 && !Rediscover()) break;
    }
    dev_.reset();
    online_ = false;
    return false;
}

bool Controller::Poll(std::string* error) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!dev_ && !Connect(error)) break;
        std::string e;
        if (dev_->Refresh(&e)) {
            online_ = true;
            updatedAt_ = NowUtc();
            return true;
        }
        SetErr(error, e.empty() ? "no response from the AC" : e);
        dev_.reset();  // reconnect (and rediscover if needed) on the next attempt
    }
    online_ = false;
    return false;
}

bool Controller::Validate(const Changes& changes, bool stage, std::string* error) {
    Device& d = *dev_;
    const Capabilities& c = d.capabilities();
    for (const auto& [key, value] : changes) {
        bool b = false;
        double n = 0;
        int v = 0;
        auto boolKey = [&](void (Device::*set)(bool)) {
            if (!ParseBool(value, b)) return SetErr(error, key + " must be true or false"), false;
            if (stage) (d.*set)(b);
            return true;
        };
        if (key == "power") { if (!boolKey(&Device::SetPower)) return false; }
        else if (key == "eco") { if (!boolKey(&Device::SetEco)) return false; }
        else if (key == "turbo") { if (!boolKey(&Device::SetTurbo)) return false; }
        else if (key == "sleep") { if (!boolKey(&Device::SetSleep)) return false; }
        else if (key == "purifier") { if (!boolKey(&Device::SetPurifier)) return false; }
        else if (key == "freeze_protection") { if (!boolKey(&Device::SetFreezeProtection)) return false; }
        else if (key == "breeze_away") { if (!boolKey(&Device::SetBreezeAway)) return false; }
        else if (key == "fahrenheit") { if (!boolKey(&Device::SetFahrenheit)) return false; }
        else if (key == "beep") {
            if (!boolKey(&Device::SetBeep)) return false;
            if (stage) beep_ = b;
        }
        else if (key == "mode") {
            if (!Lookup(kModes, value, v) || std::find(c.modes.begin(), c.modes.end(), static_cast<Mode>(v)) == c.modes.end())
                return SetErr(error, "mode '" + value + "' is not supported"), false;
            if (stage) d.SetMode(static_cast<Mode>(v));
        } else if (key == "target_temperature") {
            if (!ParseNumber(value, n) || n < c.minTemperature || n > c.maxTemperature || std::fmod(n * 2, 1.0) != 0)
                return SetErr(error, "target_temperature " + value + " must be " + Num(c.minTemperature) + "-" +
                                         Num(c.maxTemperature) + " in 0.5 steps"), false;
            if (stage) d.SetTargetTemperature(n);
        } else if (key == "fan_speed") {
            if (Lookup(kFans, value, v) &&
                std::find(c.fanSpeeds.begin(), c.fanSpeeds.end(), static_cast<FanSpeed>(v)) != c.fanSpeeds.end()) {
                if (stage) d.SetFanSpeed(v);
            } else if (ParseNumber(value, n) && c.customFanSpeed && n >= 1 && n <= 100 && std::floor(n) == n) {
                if (stage) d.SetFanSpeed(static_cast<int>(n));
            } else {
                return SetErr(error, "fan_speed '" + value + "' is not supported"), false;
            }
        } else if (key == "swing") {
            if (!Lookup(kSwings, value, v) ||
                std::find(c.swingModes.begin(), c.swingModes.end(), static_cast<Swing>(v)) == c.swingModes.end())
                return SetErr(error, "swing '" + value + "' is not supported"), false;
            if (stage) d.SetSwing(static_cast<Swing>(v));
        } else if (key == "vertical_angle" || key == "horizontal_angle") {
            const bool vertical = key == "vertical_angle";
            if (!(vertical ? c.verticalAngle : c.horizontalAngle)) return SetErr(error, key + " is not supported"), false;
            if (value == "release") continue;  // handled after Apply (needs a swing round-trip)
            bool ok = ParseNumber(value, n);
            if (ok) {
                ok = false;
                for (int a : kSwingAngles) ok |= n == a;
            }
            if (!ok) return SetErr(error, key + " must be one of 1, 25, 50, 75, 100; choose a swing mode to release it"), false;
            if (stage) vertical ? d.SetVerticalAngle(static_cast<int>(n)) : d.SetHorizontalAngle(static_cast<int>(n));
        } else if (key == "display" && value == "toggle") {
            // handled after Apply
        } else if (key == "self_clean" && value == "start") {
            // handled after Apply
        } else {
            return SetErr(error, "unknown setting '" + key + "'"), false;
        }
    }
    return true;
}

bool Controller::Set(const Changes& changes, std::string* error) {
    if (changes.empty()) return SetErr(error, "no changes given"), false;
    if (!dev_ && !Connect(error)) return online_ = false, false;

    if (!Validate(changes, false, error)) return false;  // nothing staged, nothing sent

    std::string e;
    if (!dev_->RefreshState(&e)) {  // fresh state first: Apply sends the whole frame
        dev_.reset();
        if (!Connect(error) || !dev_->RefreshState(&e)) {
            online_ = false;
            return SetErr(error, "AC unreachable"), false;
        }
    }
    Validate(changes, true, nullptr);
    if (!dev_->Apply(&e)) {
        online_ = false;
        return SetErr(error, e.empty() ? "AC did not confirm the change" : e), false;
    }
    bool propertyWrite = false;
    int releaseAxes = 0;  // swing bits of the axes whose fixed louver angle should be released
    for (const auto& [key, value] : changes) {
        if (key == "display") dev_->ToggleDisplay(&e);
        if (key == "self_clean") dev_->StartSelfClean(&e);
        if (value == "release") releaseAxes |= key == "vertical_angle" ? static_cast<int>(Swing::Vertical)
                                                                       : static_cast<int>(Swing::Horizontal);
        propertyWrite |= key == "vertical_angle" || key == "horizontal_angle" || key == "breeze_away" ||
                         key == "self_clean" || key == "beep";
    }
    // A fixed angle can't be written back to 0; the AC clears it when a swing mode covering that axis is
    // chosen. Swing through such a mode, then settle on the swing the user wants.
    if (releaseAxes) {
        const int wanted = dev_->state().swing;
        dev_->SetSwing(static_cast<Swing>(wanted | releaseAxes));
        dev_->Apply(&e);
        dev_->SetSwing(static_cast<Swing>(wanted));
        dev_->Apply(&e);
    }
    // A property write can change state fields (a fixed louver angle stops swing on that axis), and the
    // reply to the state write predates it: read everything again so the snapshot is true.
    if (propertyWrite) dev_->Refresh(&e);
    online_ = true;
    updatedAt_ = NowUtc();
    return true;
}

std::string Controller::CapabilitiesJson() const {
    const Capabilities& c = capabilities();
    std::string modes, fans, swings;
    for (Mode m : c.modes) modes += std::string(modes.empty() ? "" : ",") + "\"" + ModeName(static_cast<int>(m)) + "\"";
    for (FanSpeed f : c.fanSpeeds) fans += std::string(fans.empty() ? "" : ",") + "\"" + FanName(static_cast<int>(f)) + "\"";
    for (Swing s : c.swingModes) swings += std::string(swings.empty() ? "" : ",") + "\"" + SwingName(static_cast<int>(s)) + "\"";
    std::string j = "{";
    j += "\"modes\":[" + modes + "],\"fan_speeds\":[" + fans + "],\"swing_modes\":[" + swings + "]";
    j += ",\"min_temperature\":" + Num(c.minTemperature) + ",\"max_temperature\":" + Num(c.maxTemperature);
    j += std::string(",\"custom_fan_speed\":") + Bool(c.customFanSpeed);
    j += std::string(",\"vertical_angle\":") + Bool(c.verticalAngle) + ",\"horizontal_angle\":" + Bool(c.horizontalAngle);
    j += std::string(",\"eco\":") + Bool(c.eco) + ",\"turbo\":" + Bool(c.Turbo()) + ",\"purifier\":" + Bool(c.purifier);
    j += std::string(",\"breeze_away\":") + Bool(c.breezeAway || c.breezeControl);
    j += std::string(",\"freeze_protection\":") + Bool(c.freezeProtection) + ",\"self_clean\":" + Bool(c.selfClean);
    j += std::string(",\"display_control\":") + Bool(c.displayControl) + ",\"humidity\":" + Bool(c.humidity);
    j += ",\"energy\":true}";
    return j;
}

std::string Controller::SnapshotJson() const {
    if (!dev_ && !online_) return "{\"online\":false}";
    const State& s = state();
    std::string j = "{";
    j += std::string("\"online\":") + Bool(online_);
    j += ",\"updated_at\":\"" + updatedAt_ + "\",\"ip\":\"" + creds_.ip + "\"";
    j += std::string(",\"power\":") + Bool(s.power) + ",\"mode\":\"" + ModeName(s.mode) + "\"";
    j += ",\"target_temperature\":" + Num(s.targetTemperature);
    j += ",\"indoor_temperature\":" + Opt(s.indoorTemperature) + ",\"outdoor_temperature\":" + Opt(s.outdoorTemperature);
    j += std::string(",\"fan_speed\":\"") + FanName(s.fanSpeed) + "\",\"fan_percent\":" + Num(s.fanSpeed);
    j += std::string(",\"swing\":\"") + SwingName(s.swing) + "\"";
    j += ",\"vertical_angle\":" + Num(s.verticalAngle) + ",\"horizontal_angle\":" + Num(s.horizontalAngle);
    j += std::string(",\"eco\":") + Bool(s.eco) + ",\"turbo\":" + Bool(s.turbo) + ",\"sleep\":" + Bool(s.sleep);
    j += std::string(",\"purifier\":") + Bool(s.purifier) + ",\"freeze_protection\":" + Bool(s.freezeProtection);
    j += std::string(",\"breeze_away\":") + Bool(s.breezeAway) + ",\"fahrenheit\":" + Bool(s.fahrenheit);
    j += std::string(",\"display_on\":") + Bool(s.displayOn) + ",\"self_clean_active\":" + Bool(s.selfCleanActive);
    j += ",\"error_code\":" + Num(s.errorCode) + std::string(",\"defrost\":") + Bool(s.defrost);
    j += ",\"power_watts\":" + Opt(s.outdoorUnitPower);  // group 7: live draw, ~2 W idle, ~2 kW peak heating
    j += ",\"diagnostics\":{\"indoor_coil\":" + Opt(s.indoorCoil) + ",\"outdoor_coil\":" + Opt(s.outdoorCoil);
    j += ",\"compressor_hz\":" + Opt(s.compressorFrequency) + ",\"compressor_target_hz\":" + Opt(s.targetCompressorFrequency);
    j += ",\"compressor_current\":" + Opt(s.compressorCurrent) + ",\"mains_voltage\":" + Opt(s.compressorVoltage);
    j += ",\"indoor_fan\":" + Opt(s.indoorFan) + ",\"outdoor_fan\":" + Opt(s.outdoorFan);
    j += ",\"discharge_pipe\":" + Opt(s.dischargePipe) + ",\"outdoor_unit_power\":" + Opt(s.outdoorUnitPower);
    j += ",\"horizontal_louvers\":" + Opt(s.horizontalLouvers) + ",\"vertical_louvers\":" + Opt(s.verticalLouvers) + "}";
    j += ",\"energy\":{\"current\":" + Opt(s.energyCurrent) + ",\"realtime\":" + Opt(s.powerRealtime);
    j += ",\"total_raw\":" + Opt(s.energyTotal) + ",\"verified_units\":false}";
    j += "}";
    return j;
}

}  // namespace midea::ac
