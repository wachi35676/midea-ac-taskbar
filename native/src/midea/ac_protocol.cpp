#include "ac_protocol.h"

#include <cmath>
#include <functional>
#include <initializer_list>

namespace midea::ac {
namespace {

uint8_t g_messageId = 0;

const uint8_t* CrcTable() {
    // CRC-8/MAXIM (Dallas), reflected polynomial 0x8C
    static uint8_t table[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; ++i) {
            uint8_t c = static_cast<uint8_t>(i);
            for (int b = 0; b < 8; ++b) c = (c & 1) ? static_cast<uint8_t>((c >> 1) ^ 0x8C) : static_cast<uint8_t>(c >> 1);
            table[i] = c;
        }
        init = true;
    }
    return table;
}

Bytes Frame(FrameType type, const Bytes& data) {
    Bytes f(10, 0);
    f[0] = 0xAA;
    f[1] = static_cast<uint8_t>(data.size() + 10);
    f[2] = kDeviceType;
    f[8] = 0;  // protocol version
    f[9] = type;
    Append(f, data);
    f.push_back(Checksum(f, 1));
    return f;
}

Bytes Command(FrameType type, const Bytes& data) {
    Bytes payload = data;
    payload.push_back(++g_messageId);
    payload.push_back(Crc8(payload));
    return Frame(type, payload);
}

void SetErr(std::string* e, const std::string& m) {
    if (e) *e = m;
}

bool In(int v, std::initializer_list<int> set) {
    for (int s : set) if (s == v) return true;
    return false;
}

Bytes EncodeProp(Prop p, int v) {
    switch (p) {
        case Prop::BreezeAway: return {static_cast<uint8_t>(v ? 2 : 1)};
        case Prop::Cascade: return {static_cast<uint8_t>(v ? 1 : 0), static_cast<uint8_t>(v)};
        case Prop::FreshAir: return {static_cast<uint8_t>(v ? 1 : 0), static_cast<uint8_t>(v), 0xFF};
        case Prop::OutSilent: return {static_cast<uint8_t>(v ? 3 : 0)};
        default: return {static_cast<uint8_t>(v)};
    }
}

// msmart only decodes these properties; others are skipped as unsupported.
bool DecodeProp(uint16_t id, const Bytes& d, int& out) {
    if (d.empty()) return false;
    switch (static_cast<Prop>(id)) {
        case Prop::Breezeless: case Prop::Flash: case Prop::SelfClean: case Prop::Sound: out = d[0] ? 1 : 0; return true;
        case Prop::BreezeAway: out = d[0] == 2; return true;
        case Prop::Buzzer: return false;
        case Prop::Cascade: case Prop::FreshAir: out = d[0] ? (d.size() > 1 ? d[1] : 0) : 0; return true;
        case Prop::Ieco: out = d.size() > 1 && d[1] ? 1 : 0; return true;
        case Prop::OutSilent: out = d[0] == 3; return true;
        case Prop::SwingUdAngle: case Prop::SwingLrAngle: case Prop::BreezeControl: case Prop::RateSelect:
        case Prop::BreezeAwayDirection: out = d[0]; return true;
        default: return false;
    }
}

bool KnownProp(uint16_t id) {
    for (int k : {0x0009, 0x000A, 0x0015, 0x0018, 0x001A, 0x0039, 0x0042, 0x0043, 0x0048, 0x004B, 0x0058,
                  0x0059, 0x0067, 0x00CD, 0x00E3, 0x021E, 0x022C})
        if (k == id) return true;
    return false;
}

std::optional<double> ParseTemperature(int data, double decimals, bool fahrenheit) {
    if (data == 0xFF) return std::nullopt;
    const double t = (data - 50) / 2.0;
    const double whole = std::trunc(t);
    if (!fahrenheit && decimals != 0) return whole + (t >= 0 ? decimals : -decimals);
    if (decimals >= 0.5) return whole + (t >= 0 ? 0.5 : -0.5);
    return t;
}

void ParseState(const Bytes& p, StateResponse& s) {
    s.power = p[1] & 0x1;
    s.targetTemperature = (p[2] & 0xF) + 16.0 + ((p[2] & 0x10) ? 0.5 : 0.0);
    s.mode = (p[2] >> 5) & 0x7;
    s.fanSpeed = p[3] & 0x7F;
    s.swingMode = p[7] & 0xF;
    s.turbo = p[8] & 0x20;
    s.independentAuxHeat = p[8] & 0x40;
    s.followMe = p[8] & 0x80;
    s.eco = p[9] & 0x10;
    s.purifier = p[9] & 0x20;
    s.auxHeat = p[9] & 0x08;
    s.sleep = p[10] & 0x1;
    s.turbo = s.turbo || (p[10] & 0x2);
    s.fahrenheit = p[10] & 0x4;
    s.indoorTemperature = ParseTemperature(p[11], (p[15] & 0xF) / 10.0, s.fahrenheit);
    s.outdoorTemperature = ParseTemperature(p[12], (p[15] >> 4) / 10.0, s.fahrenheit);
    const int alt = p[13] & 0x1F;
    if (alt != 0) s.targetTemperature = alt + 12 + ((p[2] & 0x10) ? 0.5 : 0.0);
    s.filterAlert = p[13] & 0x20;
    s.displayOn = p[14] != 0x70;
    s.errorCode = p[16];
    if (p.size() < 20) return;
    s.targetHumidity = p[19] & 0x7F;
    if (p.size() < 22) return;
    s.freezeProtection = (p[21] & 0x80) != 0;
}

void ParseCapabilities(const Bytes& payload, CapabilitiesResponse& r) {
    using Reader = std::function<void(const Bytes&, std::map<std::string, double>&)>;
    auto val = [](const char* name, int w) -> Reader {
        return [=](const Bytes& v, std::map<std::string, double>& m) { m[name] = v[0] == w; };
    };
    auto any = [](const char* name, std::initializer_list<int> set) -> Reader {
        std::vector<int> s(set);
        return [=](const Bytes& v, std::map<std::string, double>& m) {
            bool hit = false;
            for (int x : s) hit |= v[0] == x;
            m[name] = hit;
        };
    };
    std::map<uint16_t, std::vector<Reader>> readers = {
        {0x021E, {val("anion", 1)}},
        {0x0219, {val("aux_electric_heat", 1)}},
        {0x0093, {val("aux_fan_speed", 1)}},
        {0x0094, {val("aux_heat_fan_speed", 1)}},
        {0x0042, {val("breeze_away", 1)}},
        {0x0058, {any("breeze_away_direction", {1, 2, 3})}},
        {0x0043, {val("breeze_control", 1)}},
        {0x0018, {val("breezeless", 1)}},
        {0x022C, {val("sound", 1)}},
        {0x0059, {val("cascade", 1)}},
        {0x0224, {any("display_control", {1, 2, 100})}},
        {0x0216, {any("energy_stats", {2, 3, 4, 5}), any("energy_setting", {3, 5}), any("energy_bcd", {2, 3})}},
        {0x0222, {val("fahrenheit", 0)}},
        {0x0210, {any("fan_silent", {6, 9}), any("fan_low", {3, 4, 5, 6, 7, 9}), any("fan_medium", {5, 6, 7}),
                  any("fan_high", {3, 4, 5, 6, 7, 9}), any("fan_auto", {4, 5, 6, 9}), val("fan_custom", 1)}},
        {0x0217, {any("filter_notice", {1, 2, 4}), any("filter_clean", {3, 4})}},
        {0x0067, {any("flash", {1, 2, 3, 4})}},
        {0x004B, {val("fresh_air", 1)}},
        {0x021F, {any("humidity_auto_set", {1, 2}), any("humidity_manual_set", {2, 3})}},
        {0x0214, {any("heat_mode", {1, 2, 4, 6, 7, 9, 10, 11, 12, 13}),
                  any("cool_mode", {0, 1, 3, 4, 5, 6, 7, 8, 9, 11, 13, 14, 15}),
                  any("dry_mode", {0, 1, 5, 6, 9, 11, 13, 14, 15}), any("auto_mode", {0, 1, 2, 7, 8, 9, 13, 14}),
                  val("aux_heat_mode", 9), any("aux_mode", {9, 10, 11, 13, 14, 15})}},
        {0x00CD, {any("out_silent", {1, 3})}},
        {0x0212, {any("eco", {1, 2})}},
        {0x0213, {val("freeze_protection", 1)}},
        {0x00E3, {[](const Bytes& v, std::map<std::string, double>& m) {
                      m["ieco"] = v[0];
                      if (v.size() > 1) m["ieco_end"] = v[1];
                  }}},
        {0x021A, {any("turbo_heat", {1, 3}), any("turbo_cool", {0, 1})}},
        {0x0048, {val("rate_select_2_level", 1), any("rate_select_5_level", {2, 3})}},
        {0x0039, {val("self_clean", 1)}},
        {0x0030, {val("smart_eye", 1)}},
        {0x000A, {val("swing_horizontal_angle", 1)}},
        {0x0009, {val("swing_vertical_angle", 1)}},
        {0x0215, {any("swing_horizontal", {1, 3}), any("swing_vertical", {0, 1})}},
        {0x0033, {val("wind_off_me", 1)}},
        {0x0032, {val("wind_on_me", 1)}},
    };

    const int count = payload.size() > 1 ? payload[1] : 0;
    size_t pos = 2;
    for (int i = 0; i < count; ++i) {
        if (payload.size() < pos + 3) break;
        const size_t size = payload[pos + 2];
        if (size == 0) { pos += 3; continue; }
        const uint16_t id = static_cast<uint16_t>(payload[pos] | (payload[pos + 1] << 8));
        const Bytes value = Slice(payload, pos + 3, pos + 3 + size);
        auto it = readers.find(id);
        if (it != readers.end()) {
            if (!value.empty())
                for (auto& rd : it->second) rd(value, r.caps);
        } else if (id == 0x0225) {  // temperature ranges
            if (size >= 6 && payload.size() >= pos + 3 + size) {  // whole value present (up to pos + 9)
                r.caps["cool_min_temperature"] = payload[pos + 3] * 0.5;
                r.caps["cool_max_temperature"] = payload[pos + 4] * 0.5;
                r.caps["auto_min_temperature"] = payload[pos + 5] * 0.5;
                r.caps["auto_max_temperature"] = payload[pos + 6] * 0.5;
                r.caps["heat_min_temperature"] = payload[pos + 7] * 0.5;
                r.caps["heat_max_temperature"] = payload[pos + 8] * 0.5;
                r.caps["decimals"] = (size > 6 ? payload[pos + 9] : payload[pos + 2]) != 0;
            } else {
                continue;  // msmart skips without advancing (it then re-reads the same bytes); mirror it
            }
        }
        pos += 3 + size;
    }
    if (payload.size() > pos + 1) r.additional = payload[payload.size() - 2] != 0;
}

void ParseProperties(const Bytes& payload, PropertiesResponse& r) {
    const int count = payload.size() > 1 ? payload[1] : 0;
    size_t pos = 2;
    for (int i = 0; i < count; ++i) {
        if (payload.size() < pos + 4) break;
        const size_t size = payload[pos + 3];
        if (size == 0) { pos += 4; continue; }
        const uint16_t id = static_cast<uint16_t>(payload[pos] | (payload[pos + 1] << 8));
        if (KnownProp(id)) {
            if (payload[pos + 2] & 0x10) r.errors[id] = payload[pos + 2];
            int v = 0;
            if (DecodeProp(id, Slice(payload, pos + 4, payload.size()), v)) r.values[id] = v;
        }
        pos += 4 + size;
    }
}

}  // namespace

uint8_t Crc8(const Bytes& data) {
    const uint8_t* t = CrcTable();
    uint8_t c = 0;
    for (uint8_t b : data) c = t[(c ^ b) & 0xFF];
    return c;
}

uint8_t Checksum(const Bytes& data, size_t from, size_t to) {
    if (to > data.size()) to = data.size();
    unsigned sum = 0;
    for (size_t i = from; i < to; ++i) sum += data[i];
    return static_cast<uint8_t>((~sum + 1) & 0xFF);
}

void SetNextMessageId(uint8_t id) { g_messageId = static_cast<uint8_t>(id - 1); }

Bytes GetCapabilitiesCommand(bool additional) {
    return Command(Query, additional ? Bytes{0xB5, 0x01, 0x01, 0x01} : Bytes{0xB5, 0x01, 0x00});
}

Bytes GetStateCommand() {
    return Command(Query, {0x41, 0x81, 0x00, 0xFF, 0x03, 0xFF, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                           0x00, 0x00, 0x00, 0x00, 0x00, 0x03});
}

Bytes GetGroupDataCommand(int group) {
    Bytes p(20, 0);
    p[0] = 0x41; p[1] = 0x21; p[2] = 0x01; p[3] = static_cast<uint8_t>(0x40 | group);
    return Command(Query, p);
}

Bytes GetPropertiesCommand(const std::vector<Prop>& props) {
    Bytes p = {0xB1, static_cast<uint8_t>(props.size())};
    for (Prop id : props) { p.push_back(static_cast<uint16_t>(id) & 0xFF); p.push_back(static_cast<uint16_t>(id) >> 8); }
    return Command(Query, p);
}

Bytes SetPropertiesCommand(const std::vector<std::pair<Prop, int>>& props) {
    Bytes p = {0xB0, static_cast<uint8_t>(props.size())};
    for (auto& [id, v] : props) {
        p.push_back(static_cast<uint16_t>(id) & 0xFF);
        p.push_back(static_cast<uint16_t>(id) >> 8);
        Bytes enc = EncodeProp(id, v);
        p.push_back(static_cast<uint8_t>(enc.size()));
        Append(p, enc);
    }
    return Command(Control, p);
}

Bytes ToggleDisplayCommand(bool beep) {
    Bytes p = {0x41, static_cast<uint8_t>(0x02 | (beep ? 0x40 : 0)), 0x00, 0xFF, 0x02, 0x00, 0x02};
    p.resize(21, 0);
    return Command(Query, p);
}

Bytes SetStateCommand(const SetState& s) {
    double integral = 0;
    const double fractional = std::modf(s.targetTemperature, &integral);
    const int whole = static_cast<int>(integral);
    uint8_t temperature, temperatureAlt;
    if (whole >= 17 && whole <= 30) { temperature = (whole - 16) & 0xF; temperatureAlt = 0; }
    else { temperature = 0; temperatureAlt = (whole - 12) & 0x1F; }
    if (fractional > 0) temperature |= 0x10;

    const uint8_t mode = static_cast<uint8_t>((s.mode & 0x7) << 5);
    return Command(Control, {
        0x40,
        static_cast<uint8_t>(0x02 | (s.beep ? 0x40 : 0) | (s.power ? 0x01 : 0)),
        static_cast<uint8_t>(temperature | mode),
        static_cast<uint8_t>(s.fanSpeed),
        0x7F, 0x7F, 0x00,
        static_cast<uint8_t>(0x30 | (s.swingMode & 0x3F)),
        static_cast<uint8_t>((s.followMe ? 0x80 : 0) | (s.turbo ? 0x20 : 0)),
        static_cast<uint8_t>((s.eco ? 0x80 : 0) | (s.purifier ? 0x20 : 0) | (s.forceAuxHeat ? 0x10 : 0) | (s.auxHeat ? 0x08 : 0)),
        static_cast<uint8_t>((s.sleep ? 0x01 : 0) | (s.turbo ? 0x02 : 0) | (s.fahrenheit ? 0x04 : 0)),
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        temperatureAlt,
        static_cast<uint8_t>(s.targetHumidity & 0x7F),
        0x00,
        static_cast<uint8_t>(s.freezeProtection ? 0x80 : 0),
        static_cast<uint8_t>(s.independentAuxHeat ? 0x08 : 0),
        0x00,
    });
}

bool ParseResponse(const Bytes& f, Response& out, std::string* error) {
    if (f.size() < 10) return SetErr(error, "frame too short"), false;
    if (Checksum(f, 1, f.size() - 1) != f.back()) return SetErr(error, "frame checksum failed: " + ToHex(f)), false;
    if (f[2] != kDeviceType) return SetErr(error, "wrong device type"), false;
    if (f.size() < 13) return SetErr(error, "frame has no payload"), false;

    out = Response{};
    const uint8_t frameType = f[9], id = f[10];
    if (id == 0xC0) out.kind = ResponseKind::State;
    else if (id == 0xB5 && frameType == Query) out.kind = ResponseKind::Capabilities;
    else if (id == 0xB1 || id == 0xB0) out.kind = ResponseKind::Properties;
    else if (id == 0xC1 && f.size() > 13) {
        switch (f[13] & 0xF) {
            case 1: out.kind = ResponseKind::Group1; break;
            case 2: out.kind = ResponseKind::Group2; break;
            case 4: out.kind = ResponseKind::Group4; break;
            case 5: out.kind = ResponseKind::Group5; break;
            case 7: out.kind = ResponseKind::Group7; break;
            case 11: out.kind = ResponseKind::Group11; break;
            default: break;
        }
    }

    // Payload CRC (some devices use a second checksum instead); properties are exempt, some send bad CRCs.
    if (out.kind != ResponseKind::Properties) {
        const Bytes p = Slice(f, 10, f.size() - 1);
        const Bytes body = Slice(p, 0, p.size() - 1);
        if (Crc8(body) != p.back() && Checksum(body) != p.back())
            return SetErr(error, "payload CRC and checksum failed: " + ToHex(p)), false;
    }

    out.payload = Slice(f, 10, f.size() - 2);
    const Bytes& p = out.payload;
    auto need = [&](size_t n) { return p.size() >= n; };

    switch (out.kind) {
        case ResponseKind::State:
            if (!need(17)) return SetErr(error, "state payload too short"), false;
            ParseState(p, out.state);
            break;
        case ResponseKind::Capabilities: ParseCapabilities(p, out.capabilities); break;
        case ResponseKind::Properties: ParseProperties(p, out.properties); break;
        case ResponseKind::Group1:
            if (!need(15)) return SetErr(error, "group 1 too short"), false;
            out.g1 = {p[4], p[5], p[7], p[8], (p[10] - 30) / 2.0, (p[11] - 30) / 2.0, (p[12] - 50) / 2.0,
                      (p[13] - 50) / 2.0, p[14]};
            break;
        case ResponseKind::Group2:
            if (!need(9)) return SetErr(error, "group 2 too short"), false;
            out.g2 = {p[4] * 8, p[5] * 8, (p[8] & 0x10) != 0};
            break;
        case ResponseKind::Group4: {
            if (!need(19)) return SetErr(error, "group 4 too short"), false;
            auto bcd = [](uint8_t d) { return 10 * (d >> 4) + (d & 0xF); };
            auto energy = [&](size_t o, double& b, double& n) {
                b = 10000.0 * bcd(p[o]) + 100.0 * bcd(p[o + 1]) + bcd(p[o + 2]) + 0.01 * bcd(p[o + 3]);
                n = ((static_cast<uint32_t>(p[o]) << 24) + (p[o + 1] << 16) + (p[o + 2] << 8) + p[o + 3]) / 10.0;
            };
            Group4& g = out.g4;
            energy(4, g.totalBcd, g.totalBin);
            energy(12, g.currentBcd, g.currentBin);
            g.realtimeBcd = 1000.0 * bcd(p[16]) + 10.0 * bcd(p[17]) + 0.1 * bcd(p[18]);
            g.realtimeBin = ((p[16] << 16) + (p[17] << 8) + p[18]) / 10.0;
            g.valid = g.totalBcd != 0 || g.currentBcd != 0 || g.realtimeBcd != 0;
            break;
        }
        case ResponseKind::Group5:
            if (!need(11)) return SetErr(error, "group 5 too short"), false;
            out.g5.humidity = p[4] ? std::optional<int>(p[4]) : std::nullopt;
            out.g5.outdoorFan = 8 * p[8];
            out.g5.defrost = p[10] != 0;
            break;
        case ResponseKind::Group7:
            if (!need(12)) return SetErr(error, "group 7 too short"), false;
            out.g7 = {p[10] + 256 * p[11]};
            break;
        case ResponseKind::Group11:
            if (!need(13)) return SetErr(error, "group 11 too short"), false;
            out.g11 = {p[9], p[12]};
            break;
        default: break;
    }
    return true;
}

}  // namespace midea::ac
