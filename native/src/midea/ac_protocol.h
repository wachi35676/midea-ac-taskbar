#pragma once
// AC (device type 0xAC) command frames and response parsers.
// Ported from msmart-ng (mill1000/midea-msmart) frame.py, crc8.py and device/AC/command.py.
#include <map>
#include <optional>
#include <string>

#include "bytes.h"

namespace midea::ac {

constexpr uint8_t kDeviceType = 0xAC;
enum FrameType : uint8_t { Control = 0x02, Query = 0x03 };

uint8_t Crc8(const Bytes& data);
uint8_t Checksum(const Bytes& data, size_t from = 0, size_t to = SIZE_MAX);

// Property IDs (16-bit, little-endian on the wire)
enum class Prop : uint16_t {
    SwingUdAngle = 0x0009, SwingLrAngle = 0x000A, IndoorHumidity = 0x0015, Breezeless = 0x0018, Buzzer = 0x001A,
    SelfClean = 0x0039, BreezeAway = 0x0042, BreezeControl = 0x0043, RateSelect = 0x0048, FreshAir = 0x004B,
    BreezeAwayDirection = 0x0058, Cascade = 0x0059, Flash = 0x0067, OutSilent = 0x00CD, Ieco = 0x00E3,
    Anion = 0x021E, Sound = 0x022C,
};

// ---------- commands ----------
// Every command carries a rolling message id (1, 2, 3 ... wraps at 255). Tests can pin it.
void SetNextMessageId(uint8_t id);  // the next command will use `id`

Bytes GetCapabilitiesCommand(bool additional);
Bytes GetStateCommand();
Bytes GetGroupDataCommand(int group);
Bytes GetPropertiesCommand(const std::vector<Prop>& props);
Bytes SetPropertiesCommand(const std::vector<std::pair<Prop, int>>& props);  // values encoded per property
Bytes ToggleDisplayCommand(bool beep);

struct SetState {
    bool beep = true, power = false;
    double targetTemperature = 25.0;
    int mode = 0, fanSpeed = 0, swingMode = 0;
    bool eco = true, turbo = false, fahrenheit = true, sleep = false, freezeProtection = false;
    bool followMe = false, purifier = false;
    int targetHumidity = 40;
    bool auxHeat = false, forceAuxHeat = false, independentAuxHeat = false;
};
Bytes SetStateCommand(const SetState& s);

// ---------- responses ----------
enum class ResponseKind { Unknown, State, Capabilities, Properties, Group1, Group2, Group4, Group5, Group7, Group11 };

struct StateResponse {
    bool power = false;
    double targetTemperature = 0;
    int mode = 0, fanSpeed = 0, swingMode = 0;
    bool turbo = false, eco = false, sleep = false, fahrenheit = false;
    std::optional<double> indoorTemperature, outdoorTemperature;
    bool filterAlert = false, displayOn = false, followMe = false, purifier = false;
    bool auxHeat = false, independentAuxHeat = false;
    std::optional<int> targetHumidity;
    std::optional<bool> freezeProtection;
    int errorCode = 0;
};

// Raw capability values by name, as msmart's CapabilitiesResponse.raw_capabilities.
struct CapabilitiesResponse {
    std::map<std::string, double> caps;  // bools stored as 0/1, temperatures as °C, ieco as numbers
    bool additional = false;             // device has a second capabilities page
    bool Has(const std::string& k) const { auto it = caps.find(k); return it != caps.end() && it->second != 0; }
    double Get(const std::string& k, double d) const { auto it = caps.find(k); return it == caps.end() ? d : it->second; }
    void Merge(const CapabilitiesResponse& o) { for (auto& kv : o.caps) caps[kv.first] = kv.second; }
};

struct PropertiesResponse {
    std::map<uint16_t, int> values;    // decoded values (bools as 0/1)
    std::map<uint16_t, uint8_t> errors;  // properties the device refused, with the result byte
};

struct Group1 { int compressorFrequency, targetCompressorFrequency, compressorCurrent, compressorVoltage;
                double indoorTemperature, indoorCoil, outdoorCoil, outdoorTemperature; int dischargePipe; };
struct Group2 { int targetIndoorFan, indoorFan; bool waterPump; };
struct Group4 { bool valid; double totalBcd, currentBcd, realtimeBcd, totalBin, currentBin, realtimeBin; };
struct Group5 { std::optional<int> humidity; int outdoorFan; bool defrost; };
struct Group7 { int outdoorUnitPower; };
struct Group11 { int horizontalLouvers, verticalLouvers; };

struct Response {
    ResponseKind kind = ResponseKind::Unknown;
    Bytes payload;  // frame[10:-2], as msmart keeps it
    StateResponse state;
    CapabilitiesResponse capabilities;
    PropertiesResponse properties;
    Group1 g1{}; Group2 g2{}; Group4 g4{}; Group5 g5{}; Group7 g7{}; Group11 g11{};
};

// Validates and parses one AA frame. Returns false (with a reason) for frames msmart would reject.
bool ParseResponse(const Bytes& frame, Response& out, std::string* error = nullptr);

}  // namespace midea::ac
