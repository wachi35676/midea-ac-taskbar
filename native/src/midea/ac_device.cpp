#include "ac_device.h"

#include <algorithm>

namespace midea::ac {
namespace {

bool OneOf(int v, std::initializer_list<int> set) { return std::find(set.begin(), set.end(), v) != set.end(); }

int ModeFromValue(int v) { return OneOf(v, {1, 2, 3, 4, 5, 6}) ? v : static_cast<int>(Mode::Fan); }
int SwingFromValue(int v) { return OneOf(v, {0x0, 0x3, 0xC, 0xF}) ? v : static_cast<int>(Swing::Off); }
int FanFromValue(int v) { return OneOf(v, {20, 40, 60, 80, 100, 102}) ? v : static_cast<int>(FanSpeed::Auto); }
int AngleFromValue(int v) { return OneOf(v, {0, 1, 25, 50, 75, 100}) ? v : 0; }

}  // namespace

Device::Device(const std::string& ip, int port, uint64_t id) : lan_(ip, port, id) {}

bool Device::Authenticate(const Bytes& token, const Bytes& key, std::string* error) {
    return lan_.Authenticate(token, key, error);
}

std::vector<Response> Device::SendAll(const std::vector<Bytes>& commands, std::string* error) {
    std::vector<Bytes> raw;
    for (const auto& c : commands) {
        auto r = lan_.Send(c, error);
        // Stop at the first unanswered command: the connection is down, and every further command would
        // wait out its own connect/read timeouts (tens of seconds for a full refresh). msmart carries on.
        if (r.empty()) break;
        raw.insert(raw.end(), r.begin(), r.end());
    }
    online_ = !raw.empty();
    std::vector<Response> valid;
    for (const auto& f : raw) {
        Response r;
        if (ParseResponse(f, r)) valid.push_back(std::move(r));
    }
    return valid;
}

void Device::SetBreezeAway(bool on) {
    state_.breezeAway = on;
    updated_.insert(caps_.breezeControl ? Prop::BreezeControl : Prop::BreezeAway);
}

void Device::SetBeep(bool on) {
    state_.sound = on;
    if (caps_.sound) updated_.insert(Prop::Sound);
}

void Device::EnableGroup(int g, bool on) {
    switch (g) {
        case 1: group1_ = on; break;
        case 2: group2_ = on; break;
        case 4: group4_ = on; break;
        case 5: group5_ = on; break;
        case 7: group7_ = on; break;
        case 11: group11_ = on; break;
        default: break;
    }
}

bool Device::GetCapabilities(std::string* error) {
    CapabilitiesResponse merged;
    bool got = false;
    for (const auto& r : SendAll({GetCapabilitiesCommand(false)}, error)) {
        if (r.kind == ResponseKind::Capabilities) { merged = r.capabilities; got = true; break; }
    }
    if (!got) {
        if (error && error->empty()) *error = "no capabilities response";
        return false;
    }
    if (merged.additional) {
        for (const auto& r : SendAll({GetCapabilitiesCommand(true)}, error)) {
            if (r.kind == ResponseKind::Capabilities) { merged.Merge(r.capabilities); break; }
        }
    }
    UpdateCapabilities(merged);
    return true;
}

void Device::UpdateCapabilities(const CapabilitiesResponse& r) {
    Capabilities c;
    c.received = true;

    c.modes = {Mode::Fan};
    if (r.Has("dry_mode")) c.modes.push_back(Mode::Dry);
    if (r.Has("cool_mode")) c.modes.push_back(Mode::Cool);
    if (r.Has("heat_mode")) c.modes.push_back(Mode::Heat);
    if (r.Has("auto_mode")) c.modes.push_back(Mode::Auto);
    c.targetHumidity = r.Has("humidity_manual_set");
    if (c.targetHumidity) c.modes.push_back(Mode::SmartDry);

    c.swingModes = {Swing::Off};
    if (r.Has("swing_horizontal")) c.swingModes.push_back(Swing::Horizontal);
    if (r.Has("swing_vertical")) c.swingModes.push_back(Swing::Vertical);
    if (r.Has("swing_horizontal") && r.Has("swing_vertical")) c.swingModes.push_back(Swing::Both);

    const bool anyFan = std::any_of(r.caps.begin(), r.caps.end(),
                                    [](const auto& kv) { return kv.first.rfind("fan_", 0) == 0; });
    auto fan = [&](const char* name, const char* key) {
        if (anyFan) return r.Has(key) || r.Has("fan_custom");
        return std::string(name) == "low" || std::string(name) == "medium" || std::string(name) == "high" ||
               std::string(name) == "auto";
    };
    if (fan("silent", "fan_silent")) c.fanSpeeds.push_back(FanSpeed::Silent);
    if (fan("low", "fan_low")) c.fanSpeeds.push_back(FanSpeed::Low);
    if (fan("medium", "fan_medium")) c.fanSpeeds.push_back(FanSpeed::Medium);
    if (fan("high", "fan_high")) c.fanSpeeds.push_back(FanSpeed::High);
    if (fan("auto", "fan_auto")) c.fanSpeeds.push_back(FanSpeed::Auto);
    c.customFanSpeed = r.Has("fan_custom");
    if (c.customFanSpeed) c.fanSpeeds.push_back(FanSpeed::Max);

    c.eco = r.Has("eco");
    c.turboCool = r.Has("turbo_cool");
    c.turboHeat = r.Has("turbo_heat");
    c.freezeProtection = r.Has("freeze_protection");
    c.displayControl = r.Has("display_control");
    c.filterReminder = r.Has("filter_notice");
    c.purifier = r.Has("anion");
    c.humidity = r.Has("humidity_auto_set") || r.Has("humidity_manual_set");
    c.verticalAngle = r.Has("swing_vertical_angle");
    c.horizontalAngle = r.Has("swing_horizontal_angle");
    c.cascade = r.Has("cascade");
    c.flash = r.Has("flash");
    c.freshAir = r.Has("fresh_air");
    c.selfClean = r.Has("self_clean");
    c.breezeControl = r.Has("breeze_control");
    if (!c.breezeControl) {
        c.breezeAway = r.Has("breeze_away");
        c.breezeless = r.Has("breezeless");
    }
    c.breezeAwayDirection = r.Has("breeze_away_direction");
    const int ieco = static_cast<int>(r.Get("ieco", 0)), iecoEnd = static_cast<int>(r.Get("ieco_end", 0));
    c.iecoCool = OneOf(ieco, {1, 3, 8}) || OneOf(iecoEnd, {1, 2, 8});
    c.iecoHeat = OneOf(ieco, {3, 4, 8}) || OneOf(iecoEnd, {2, 3, 8});
    c.outSilent = r.Has("out_silent");
    c.sound = r.Has("sound");
    c.energyStats = r.Has("energy_stats");

    double mn = 1e9, mx = -1e9;
    for (const char* m : {"cool", "auto", "heat"}) {
        mn = std::min(mn, r.Get(std::string(m) + "_min_temperature", 16));
        mx = std::max(mx, r.Get(std::string(m) + "_max_temperature", 30));
    }
    c.minTemperature = mn;
    c.maxTemperature = mx;

    // Capabilities may enable energy requests but never disable them (some units report data anyway).
    group4_ |= c.energyStats;
    caps_ = c;

    supportedProps_.clear();
    if (c.breezeAway || c.breezeControl) supportedProps_.insert(c.breezeControl ? Prop::BreezeControl : Prop::BreezeAway);
    if (c.breezeAwayDirection) supportedProps_.insert(Prop::BreezeAwayDirection);
    if (c.breezeless) supportedProps_.insert(Prop::Breezeless);
    if (c.cascade) supportedProps_.insert(Prop::Cascade);
    if (c.flash) supportedProps_.insert(Prop::Flash);
    if (c.freshAir) supportedProps_.insert(Prop::FreshAir);
    if (c.iecoCool || c.iecoHeat) supportedProps_.insert(Prop::Ieco);
    if (c.outSilent) supportedProps_.insert(Prop::OutSilent);
    if (c.selfClean) supportedProps_.insert(Prop::SelfClean);
    if (c.sound) supportedProps_.insert(Prop::Sound);
    if (c.horizontalAngle) supportedProps_.insert(Prop::SwingLrAngle);
    if (c.verticalAngle) supportedProps_.insert(Prop::SwingUdAngle);
    if (r.Has("rate_select_2_level") || r.Has("rate_select_5_level")) supportedProps_.insert(Prop::RateSelect);
}

void Device::Update(const Response& r) {
    switch (r.kind) {
        case ResponseKind::State: {
            const StateResponse& s = r.state;
            state_.power = s.power;
            state_.targetTemperature = s.targetTemperature;
            state_.mode = ModeFromValue(s.mode);
            state_.fanSpeed = caps_.customFanSpeed ? s.fanSpeed : FanFromValue(s.fanSpeed);
            state_.swing = SwingFromValue(s.swingMode);
            state_.eco = s.eco;
            state_.turbo = s.turbo;
            if (s.freezeProtection) state_.freezeProtection = *s.freezeProtection;
            state_.sleep = s.sleep;
            state_.indoorTemperature = s.indoorTemperature;
            state_.outdoorTemperature = s.outdoorTemperature;
            state_.displayOn = s.displayOn;
            state_.fahrenheit = s.fahrenheit;
            state_.filterAlert = s.filterAlert;
            state_.followMe = s.followMe;
            state_.purifier = s.purifier;
            if (s.targetHumidity) state_.targetHumidity = *s.targetHumidity;
            state_.auxMode = s.independentAuxHeat ? 2 : (s.auxHeat ? 1 : 0);
            state_.errorCode = s.errorCode;
            break;
        }
        case ResponseKind::Properties: {
            auto get = [&](Prop p, int& out) {
                auto it = r.properties.values.find(static_cast<uint16_t>(p));
                if (it == r.properties.values.end()) return false;
                out = it->second;
                return true;
            };
            int v;
            if (get(Prop::SwingLrAngle, v)) state_.horizontalAngle = AngleFromValue(v);
            if (get(Prop::SwingUdAngle, v)) state_.verticalAngle = AngleFromValue(v);
            if (get(Prop::SelfClean, v)) state_.selfCleanActive = v != 0;
            if (get(Prop::BreezeControl, v)) state_.breezeAway = v == 2;
            else if (get(Prop::BreezeAway, v)) state_.breezeAway = v != 0;
            if (get(Prop::Sound, v)) state_.sound = v != 0;
            break;
        }
        case ResponseKind::Group1:
            state_.targetCompressorFrequency = r.g1.targetCompressorFrequency;
            state_.compressorFrequency = r.g1.compressorFrequency;
            state_.compressorCurrent = r.g1.compressorCurrent;
            state_.compressorVoltage = r.g1.compressorVoltage;
            state_.indoorCoil = r.g1.indoorCoil;
            state_.outdoorCoil = r.g1.outdoorCoil;
            state_.dischargePipe = r.g1.dischargePipe;
            break;
        case ResponseKind::Group2:
            state_.targetIndoorFan = r.g2.targetIndoorFan;
            state_.indoorFan = r.g2.indoorFan;
            break;
        case ResponseKind::Group4:
            if (r.g4.valid) {
                state_.energyTotal = r.g4.totalBcd;
                state_.energyCurrent = r.g4.currentBcd;
                state_.powerRealtime = r.g4.realtimeBcd;
            } else {
                state_.energyTotal = state_.energyCurrent = state_.powerRealtime = std::nullopt;
            }
            break;
        case ResponseKind::Group5:
            state_.indoorHumidity = r.g5.humidity;
            state_.outdoorFan = r.g5.outdoorFan;
            state_.defrost = r.g5.defrost;
            break;
        case ResponseKind::Group7: state_.outdoorUnitPower = r.g7.outdoorUnitPower; break;
        case ResponseKind::Group11:
            state_.horizontalLouvers = r.g11.horizontalLouvers;
            state_.verticalLouvers = r.g11.verticalLouvers;
            break;
        default: break;
    }
}

bool Device::Refresh(std::string* error) {
    std::vector<Bytes> cmds = {GetStateCommand()};
    if (group1_) cmds.push_back(GetGroupDataCommand(1));
    if (group2_) cmds.push_back(GetGroupDataCommand(2));
    if (group4_) cmds.push_back(GetGroupDataCommand(4));
    if (caps_.humidity || group5_) cmds.push_back(GetGroupDataCommand(5));
    if (group7_) cmds.push_back(GetGroupDataCommand(7));
    if (group11_) cmds.push_back(GetGroupDataCommand(11));
    if (!supportedProps_.empty())
        cmds.push_back(GetPropertiesCommand(std::vector<Prop>(supportedProps_.begin(), supportedProps_.end())));
    for (const auto& r : SendAll(cmds, error)) Update(r);
    return online_;
}

bool Device::RefreshState(std::string* error) {
    for (const auto& r : SendAll({GetStateCommand()}, error)) Update(r);
    return online_;
}

int Device::PropertyValue(Prop p) const {
    switch (p) {
        case Prop::SwingUdAngle: return state_.verticalAngle;
        case Prop::SwingLrAngle: return state_.horizontalAngle;
        case Prop::BreezeAway: return state_.breezeAway ? 1 : 0;
        case Prop::BreezeControl: return state_.breezeAway ? 2 : 1;
        case Prop::Sound: return state_.sound ? 1 : 0;
        case Prop::SelfClean: return state_.selfCleanActive ? 1 : 0;
        default: return 0;
    }
}

bool Device::Apply(std::string* error) {
    SetState cmd;
    cmd.beep = state_.sound;
    cmd.power = state_.power;
    cmd.targetTemperature = state_.targetTemperature;
    cmd.mode = state_.mode;
    cmd.fanSpeed = state_.fanSpeed;
    cmd.swingMode = state_.swing;
    cmd.eco = state_.eco;
    cmd.turbo = state_.turbo;
    cmd.freezeProtection = state_.freezeProtection;
    cmd.sleep = state_.sleep;
    cmd.fahrenheit = state_.fahrenheit;
    cmd.followMe = state_.followMe;
    cmd.purifier = state_.purifier;
    cmd.targetHumidity = state_.targetHumidity;
    cmd.auxHeat = state_.auxMode == 1;
    cmd.independentAuxHeat = state_.auxMode == 2;
    for (const auto& r : SendAll({SetStateCommand(cmd)}, error)) Update(r);
    if (!online_) return false;

    if (updated_.empty()) return true;
    std::vector<std::pair<Prop, int>> props;
    for (Prop p : updated_) props.emplace_back(p, PropertyValue(p));
    props.emplace_back(Prop::Buzzer, state_.sound ? 1 : 0);
    updated_.clear();
    for (const auto& r : SendAll({SetPropertiesCommand(props)}, error)) Update(r);
    return online_;
}

bool Device::ToggleDisplay(std::string* error) {
    SendAll({ToggleDisplayCommand(state_.sound)}, error);
    return Refresh(error);
}

bool Device::StartSelfClean(std::string* error) {
    std::vector<std::pair<Prop, int>> props = {{Prop::SelfClean, 1}, {Prop::Buzzer, state_.sound ? 1 : 0}};
    for (const auto& r : SendAll({SetPropertiesCommand(props)}, error)) Update(r);
    return online_;
}

}  // namespace midea::ac
