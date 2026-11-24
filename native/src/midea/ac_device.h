#pragma once
// A Midea air conditioner on the LAN: capabilities, state, refresh and apply.
// Ported from msmart-ng (mill1000/midea-msmart) device/AC/device.py. Blocking; one worker thread per device.
#include <optional>
#include <set>
#include <string>

#include "ac_protocol.h"
#include "lan.h"

namespace midea::ac {

enum class Mode : int { Auto = 1, Cool = 2, Dry = 3, Heat = 4, Fan = 5, SmartDry = 6 };
enum class FanSpeed : int { Silent = 20, Low = 40, Medium = 60, High = 80, Max = 100, Auto = 102 };
enum class Swing : int { Off = 0x0, Horizontal = 0x3, Vertical = 0xC, Both = 0xF };
// Fixed louver positions; 0 means none fixed (swinging or default) and can't be written.
constexpr int kSwingAngles[] = {1, 25, 50, 75, 100};

struct Capabilities {
    bool received = false;
    std::vector<Mode> modes;
    std::vector<Swing> swingModes;
    std::vector<FanSpeed> fanSpeeds;
    double minTemperature = 16, maxTemperature = 30;
    bool customFanSpeed = false, eco = false, turboCool = false, turboHeat = false, freezeProtection = false;
    bool displayControl = false, filterReminder = false, purifier = false, humidity = false, targetHumidity = false;
    bool verticalAngle = false, horizontalAngle = false, cascade = false, flash = false, freshAir = false;
    bool selfClean = false, breezeControl = false, breezeAway = false, breezeless = false, breezeAwayDirection = false;
    bool iecoCool = false, iecoHeat = false, outSilent = false, sound = false, energyStats = false;
    bool Turbo() const { return turboCool || turboHeat; }
};

struct State {
    bool power = false;
    double targetTemperature = 17.0;
    int mode = static_cast<int>(Mode::Auto);
    int fanSpeed = static_cast<int>(FanSpeed::Auto);  // preset value or custom 1-100
    int swing = static_cast<int>(Swing::Off);
    int verticalAngle = 0, horizontalAngle = 0;
    bool eco = false, turbo = false, sleep = false, freezeProtection = false, purifier = false, followMe = false;
    bool fahrenheit = false, displayOn = false, sound = false, breezeAway = false, selfCleanActive = false;
    int targetHumidity = 40;
    int auxMode = 0;  // 0 off, 1 aux+heat, 2 aux only
    std::optional<double> indoorTemperature, outdoorTemperature;
    std::optional<int> indoorHumidity;
    bool filterAlert = false;
    int errorCode = 0;
    bool defrost = false;
    // Diagnostics (data groups)
    std::optional<int> compressorFrequency, targetCompressorFrequency, compressorCurrent, compressorVoltage;
    std::optional<double> indoorCoil, outdoorCoil;
    std::optional<int> dischargePipe, outdoorFan, indoorFan, targetIndoorFan, outdoorUnitPower;
    std::optional<int> horizontalLouvers, verticalLouvers;
    std::optional<double> energyTotal, energyCurrent, powerRealtime;  // BCD decoding; units unverified
};

class Device {
public:
    Device(const std::string& ip, int port, uint64_t id);

    bool Authenticate(const Bytes& token, const Bytes& key, std::string* error = nullptr);
    bool GetCapabilities(std::string* error = nullptr);
    // Reads state, the enabled data groups and the supported properties. False when the AC didn't answer.
    bool Refresh(std::string* error = nullptr);
    // Light refresh: state only (used right before a write).
    bool RefreshState(std::string* error = nullptr);
    // Sends the whole desired state (fields set through the setters below), then any changed properties.
    bool Apply(std::string* error = nullptr);
    bool ToggleDisplay(std::string* error = nullptr);
    bool StartSelfClean(std::string* error = nullptr);

    // Setters: change the desired state; Apply() sends it.
    void SetPower(bool on) { state_.power = on; }
    void SetTargetTemperature(double t) { state_.targetTemperature = t; }
    void SetMode(Mode m) { state_.mode = static_cast<int>(m); }
    void SetFanSpeed(int s) { state_.fanSpeed = s; }
    void SetSwing(Swing s) { state_.swing = static_cast<int>(s); }
    void SetVerticalAngle(int a) { state_.verticalAngle = a; updated_.insert(Prop::SwingUdAngle); }
    void SetHorizontalAngle(int a) { state_.horizontalAngle = a; updated_.insert(Prop::SwingLrAngle); }
    void SetBreezeAway(bool on);
    void SetEco(bool on) { state_.eco = on; }
    void SetTurbo(bool on) { state_.turbo = on; }
    void SetSleep(bool on) { state_.sleep = on; }
    void SetFreezeProtection(bool on) { state_.freezeProtection = on; }
    void SetPurifier(bool on) { state_.purifier = on; }
    void SetFahrenheit(bool on) { state_.fahrenheit = on; }
    void SetBeep(bool on);

    void EnableGroup(int group, bool on);  // 1, 2, 4, 5, 7, 11
    void DiscardPendingProperties() { updated_.clear(); }
    void Disconnect() { lan_.Disconnect(); }

    const State& state() const { return state_; }
    const Capabilities& capabilities() const { return caps_; }
    bool online() const { return online_; }
    const std::string& ip() const { return lan_.Ip(); }
    void SetAddress(const std::string& ip, int port) { lan_.SetAddress(ip, port); }
    std::set<Prop> supportedProperties() const { return supportedProps_; }

private:
    std::vector<Response> SendAll(const std::vector<Bytes>& commands, std::string* error);
    void Update(const Response& r);
    void UpdateCapabilities(const CapabilitiesResponse& r);
    int PropertyValue(Prop p) const;

    Lan lan_;
    State state_;
    Capabilities caps_;
    bool online_ = false;
    std::set<Prop> supportedProps_, updated_;
    bool group1_ = false, group2_ = false, group4_ = false, group5_ = false, group7_ = false, group11_ = false;
};

}  // namespace midea::ac
