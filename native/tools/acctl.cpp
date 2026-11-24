// acctl: command-line driver for the native Midea AC library.
//   acctl discover                      list devices on the LAN
//   acctl query    [--creds FILE]       capabilities + full snapshot as JSON
//   acctl set k=v [k=v ...] [--creds FILE]
//   acctl selftest FILE                 golden vectors generated from msmart-ng (tools/gen_vectors.py)
//   acctl parse HEXFRAME                print a parsed response
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../src/midea/ac_controller.h"
#include "../src/midea/ac_protocol.h"
#include "../src/midea/crypto.h"
#include "../src/midea/discover.h"
#include "../src/midea/lan.h"

using namespace midea;

namespace {

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

std::wstring DefaultCreds() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring p(exe);
    // native/build/acctl.exe -> project root
    for (int i = 0; i < 3; ++i) p = p.substr(0, p.find_last_of(L"\\/"));
    return p + L"\\ac_creds.json";
}

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

std::string N(double v) {
    char b[32];
    snprintf(b, sizeof(b), "%.6g", v);
    return b;
}
template <class T>
std::string ON(const std::optional<T>& v) { return v ? N(static_cast<double>(*v)) : "none"; }

// Canonical "k=v;k=v" description of a parsed response, matched by tools/gen_vectors.py.
std::string Describe(const ac::Response& r) {
    using K = ac::ResponseKind;
    std::vector<std::pair<std::string, std::string>> f;
    auto add = [&](const char* k, const std::string& v) { f.emplace_back(k, v); };
    switch (r.kind) {
        case K::State: {
            const auto& s = r.state;
            add("kind", "state");
            add("power", N(s.power)); add("target_temperature", N(s.targetTemperature)); add("mode", N(s.mode));
            add("fan_speed", N(s.fanSpeed)); add("swing_mode", N(s.swingMode)); add("turbo", N(s.turbo));
            add("eco", N(s.eco)); add("sleep", N(s.sleep)); add("fahrenheit", N(s.fahrenheit));
            add("indoor_temperature", ON(s.indoorTemperature)); add("outdoor_temperature", ON(s.outdoorTemperature));
            add("filter_alert", N(s.filterAlert)); add("display_on", N(s.displayOn)); add("follow_me", N(s.followMe));
            add("purifier", N(s.purifier)); add("aux_heat", N(s.auxHeat)); add("independent_aux_heat", N(s.independentAuxHeat));
            add("target_humidity", ON(s.targetHumidity)); add("freeze_protection", ON(s.freezeProtection));
            add("error_code", N(s.errorCode));
            break;
        }
        case K::Capabilities:
            add("kind", "capabilities");
            for (const auto& kv : r.capabilities.caps) f.emplace_back(kv.first, N(kv.second));
            add("additional", N(r.capabilities.additional));
            break;
        case K::Properties: {
            add("kind", "properties");
            for (const auto& kv : r.properties.values) {
                char k[8];
                snprintf(k, sizeof(k), "%04x", kv.first);
                f.emplace_back(k, N(kv.second));
            }
            break;
        }
        case K::Group1:
            add("kind", "group1");
            add("compressor_frequency", N(r.g1.compressorFrequency)); add("target_compressor_frequency", N(r.g1.targetCompressorFrequency));
            add("compressor_current", N(r.g1.compressorCurrent)); add("compressor_voltage", N(r.g1.compressorVoltage));
            add("indoor_temperature", N(r.g1.indoorTemperature)); add("indoor_coil_temperature", N(r.g1.indoorCoil));
            add("outdoor_coil_temperature", N(r.g1.outdoorCoil)); add("outdoor_temperature", N(r.g1.outdoorTemperature));
            add("discharge_pipe_temperature", N(r.g1.dischargePipe));
            break;
        case K::Group2:
            add("kind", "group2");
            add("target_indoor_fan_speed", N(r.g2.targetIndoorFan)); add("indoor_fan_speed", N(r.g2.indoorFan));
            add("water_pump_running", N(r.g2.waterPump));
            break;
        case K::Group4: {
            add("kind", "group4");
            auto v = [&](double x) { return r.g4.valid ? N(x) : std::string("none"); };
            add("total_energy", v(r.g4.totalBcd)); add("current_energy", v(r.g4.currentBcd)); add("real_time_power", v(r.g4.realtimeBcd));
            add("total_energy_binary", v(r.g4.totalBin)); add("current_energy_binary", v(r.g4.currentBin));
            add("real_time_power_binary", v(r.g4.realtimeBin));
            break;
        }
        case K::Group5:
            add("kind", "group5");
            add("humidity", ON(r.g5.humidity)); add("outdoor_fan_speed", N(r.g5.outdoorFan)); add("defrost", N(r.g5.defrost));
            break;
        case K::Group7: add("kind", "group7"); add("outdoor_unit_power", N(r.g7.outdoorUnitPower)); break;
        case K::Group11:
            add("kind", "group11");
            add("horizontal_louvers_angle", N(r.g11.horizontalLouvers)); add("vertical_louvers_angle", N(r.g11.verticalLouvers));
            break;
        default: add("kind", "unknown"); break;
    }
    std::string out;
    for (const auto& [k, v] : f) out += (out.empty() ? "" : ";") + k + "=" + v;
    return out;
}

Bytes BuildCommand(const std::string& kind, const std::string& args) {
    if (kind == "caps0") return ac::GetCapabilitiesCommand(false);
    if (kind == "caps1") return ac::GetCapabilitiesCommand(true);
    if (kind == "state") return ac::GetStateCommand();
    if (kind == "group") return ac::GetGroupDataCommand(std::stoi(args));
    if (kind == "display") return ac::ToggleDisplayCommand(args == "1");
    if (kind == "getprops") {
        std::vector<ac::Prop> ids;
        for (auto& s : Split(args, ',')) ids.push_back(static_cast<ac::Prop>(std::stoi(s, nullptr, 16)));
        return ac::GetPropertiesCommand(ids);
    }
    if (kind == "setprops") {
        std::vector<std::pair<ac::Prop, int>> props;
        for (auto& kv : Split(args, ',')) {
            auto p = Split(kv, '=');
            props.emplace_back(static_cast<ac::Prop>(std::stoi(p[0], nullptr, 16)), std::stoi(p[1]));
        }
        return ac::SetPropertiesCommand(props);
    }
    if (kind == "setstate") {
        ac::SetState s;
        for (auto& kv : Split(args, ',')) {
            auto p = Split(kv, '=');
            const std::string& k = p[0];
            const double v = std::stod(p[1]);
            if (k == "beep") s.beep = v != 0;
            else if (k == "power") s.power = v != 0;
            else if (k == "temp") s.targetTemperature = v;
            else if (k == "mode") s.mode = static_cast<int>(v);
            else if (k == "fan") s.fanSpeed = static_cast<int>(v);
            else if (k == "swing") s.swingMode = static_cast<int>(v);
            else if (k == "eco") s.eco = v != 0;
            else if (k == "turbo") s.turbo = v != 0;
            else if (k == "fahrenheit") s.fahrenheit = v != 0;
            else if (k == "sleep") s.sleep = v != 0;
            else if (k == "freeze") s.freezeProtection = v != 0;
            else if (k == "followme") s.followMe = v != 0;
            else if (k == "purifier") s.purifier = v != 0;
            else if (k == "humidity") s.targetHumidity = static_cast<int>(v);
            else if (k == "aux") s.auxHeat = v != 0;
            else if (k == "indaux") s.independentAuxHeat = v != 0;
        }
        return ac::SetStateCommand(s);
    }
    return {};
}

int SelfTest(const std::string& path) {
    std::ifstream in(path);
    if (!in) { fprintf(stderr, "cannot open %s\n", path.c_str()); return 2; }
    int pass = 0, fail = 0, line = 0;
    std::string l;
    while (std::getline(in, l)) {
        ++line;
        if (l.empty() || l[0] == '#') continue;
        if (!l.empty() && l.back() == '\r') l.pop_back();
        auto t = Split(l, ' ');
        const std::string& k = t[0];
        std::string got, want;
        Bytes a, b, c;
        if (k == "crc8") { FromHex(t[1], a); got = ToHex({ac::Crc8(a)}); want = t[2]; }
        else if (k == "aes_ecb") { FromHex(t[1], a); got = ToHex(security::EncryptAes(a)); want = t[2];
                                   if (got == want) { Bytes d; FromHex(t[2], b); security::DecryptAes(b, d); if (d != a) got = "decrypt-mismatch"; } }
        else if (k == "sign") { FromHex(t[1], a); got = ToHex(security::Sign(a)); want = t[2]; }
        else if (k == "udpid") { FromHex(t[1], a); got = ToHex(security::Udpid(a)); want = t[2]; }
        else if (k == "v2decode") { FromHex(t[1], a); std::string e; Bytes f; got = packet::Decode(a, f, &e) ? ToHex(f) : "error:" + e; want = t[2]; }
        else if (k == "v2encode") { FromHex(t[2], a); FromHex(t[3], b); got = ToHex(packet::Encode(std::stoull(t[1]), a, b)); want = t[4]; }
        else if (k == "v3hand") { FromHex(t[2], a); got = ToHex(v3::EncodeHandshake(static_cast<uint16_t>(std::stoi(t[1])), a)); want = t[3]; }
        else if (k == "v3enc") { FromHex(t[2], a); FromHex(t[3], b); FromHex(t[4], c);
                                 got = ToHex(v3::EncodeEncrypted(static_cast<uint16_t>(std::stoi(t[1])), a, b, &c)); want = t[5]; }
        else if (k == "v3dec") { FromHex(t[1], a); FromHex(t[2], b); std::string e; Bytes o; got = v3::DecodePacket(a, b, o, &e) ? ToHex(o) : "error:" + e; want = t[3]; }
        else if (k == "v3key") { FromHex(t[1], a); FromHex(t[2], b); Bytes lk; got = v3::DeriveLocalKey(a, b, lk) ? ToHex(lk) : "error"; want = t[3]; }
        else if (k == "cmd") { ac::SetNextMessageId(static_cast<uint8_t>(std::stoi(t[1]))); got = ToHex(BuildCommand(t[2], t[3])); want = t[4]; }
        else if (k == "resp") { FromHex(t[1], a); ac::Response r; std::string e; got = ac::ParseResponse(a, r, &e) ? Describe(r) : "error"; want = t[2]; }
        else if (k == "disc") {
            FromHex(t[2], a);
            DiscoveredDevice d;
            got = ParseDiscoveryReply(t[1], a, d)
                ? std::to_string(d.id) + "," + std::to_string(d.port) + "," + d.sn + "," + d.name + "," +
                      std::to_string(d.version) + "," + std::to_string(d.type)
                : "error";
            want = t[3];
        } else { fprintf(stderr, "line %d: unknown vector '%s'\n", line, k.c_str()); ++fail; continue; }

        if (got == want) ++pass;
        else {
            ++fail;
            printf("FAIL line %d (%s)\n  want %s\n  got  %s\n", line, k.c_str(), want.c_str(), got.c_str());
        }
    }
    printf("%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: acctl discover | query | set k=v ... | selftest FILE | parse HEX  [--creds FILE]\n");
        return 2;
    }
    const std::string cmd = argv[1];
    std::wstring creds = DefaultCreds();
    std::vector<std::string> args;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--creds" && i + 1 < argc) creds = Widen(argv[++i]);
        else args.push_back(argv[i]);
    }
    using clock = std::chrono::steady_clock;
    auto ms = [](clock::time_point t) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t).count();
    };

    if (cmd == "selftest" && !args.empty()) return SelfTest(args[0]);

    if (cmd == "parse" && !args.empty()) {
        Bytes f;
        ac::Response r;
        std::string e;
        if (!FromHex(args[0], f) || !ac::ParseResponse(f, r, &e)) { printf("error: %s\n", e.c_str()); return 1; }
        printf("%s\n", Describe(r).c_str());
        return 0;
    }

    if (cmd == "discover") {
        const auto t = clock::now();
        auto found = Discover(3000, 3);
        printf("[");
        for (size_t i = 0; i < found.size(); ++i) {
            const auto& d = found[i];
            printf("%s{\"ip\":\"%s\",\"port\":%d,\"id\":%llu,\"version\":%d,\"type\":%d,\"name\":\"%s\"}", i ? "," : "",
                   d.ip.c_str(), d.port, static_cast<unsigned long long>(d.id), d.version, d.type, d.name.c_str());
        }
        printf("]\n");
        fprintf(stderr, "discover: %lld ms\n", static_cast<long long>(ms(t)));
        return found.empty() ? 1 : 0;
    }

    if (cmd == "watch") {
        // One long-lived Controller, as the app will hold it: polls, writes, a dropped session, a dead address.
        const int polls = args.size() > 0 ? std::stoi(args[0]) : 30;
        const int gapMs = args.size() > 1 ? std::stoi(args[1]) : 2000;
        ac::Controller c(creds);
        std::string e;
        auto t = clock::now();
        if (!c.Connect(&e)) { fprintf(stderr, "connect failed: %s\n", e.c_str()); return 1; }
        printf("connect %lld ms\n", static_cast<long long>(ms(t)));
        const double target = [&] { c.Poll(); return c.state().targetTemperature; }();
        int failures = 0;
        long long worst = 0;
        for (int i = 1; i <= polls; ++i) {
            std::string event;
            if (i == polls / 4) {
                t = clock::now();
                const bool ok = c.Set({{"target_temperature", std::to_string(target - 1)}}, &e);
                event = std::string(" | set target-1 ") + (ok ? "ok" : "FAILED " + e) + " " + std::to_string(ms(t)) + " ms";
            } else if (i == polls / 4 + 1) {
                t = clock::now();
                const bool ok = c.Set({{"target_temperature", std::to_string(target)}}, &e);
                event = std::string(" | restore target ") + (ok ? "ok" : "FAILED " + e) + " " + std::to_string(ms(t)) + " ms";
            } else if (i == polls / 2) {
                c.TestDropConnection();
                event = " | dropped TCP session before this poll";
            } else if (i == (3 * polls) / 4) {
                c.TestSetAddress("192.0.2.1");
                event = " | pointed session at a dead IP before this poll";
            }
            t = clock::now();
            const bool ok = c.Poll(&e);
            const long long dt = ms(t);
            worst = dt > worst ? dt : worst;
            failures += ok ? 0 : 1;
            const auto& s = c.state();
            printf("poll %2d: %5lld ms %s power=%d target=%.1f room=%s ip=%s%s\n", i, dt, ok ? "ok  " : "FAIL",
                   s.power, s.targetTemperature, s.indoorTemperature ? std::to_string(*s.indoorTemperature).substr(0, 4).c_str() : "-",
                   c.Online() ? "up" : "down", event.c_str());
            fflush(stdout);
            Sleep(gapMs);
        }
        printf("polls=%d failures=%d worst=%lld ms final_target=%.1f (started %.1f)\n", polls, failures, worst,
               c.state().targetTemperature, target);
        return failures ? 1 : 0;
    }

    if (cmd == "query" || cmd == "set") {
        ac::Controller c(creds);
        std::string e;
        auto t = clock::now();
        if (!c.Connect(&e)) { fprintf(stderr, "connect failed: %s\n", e.c_str()); printf("{\"online\":false,\"error\":\"%s\"}\n", e.c_str()); return 1; }
        fprintf(stderr, "connect+auth+caps: %lld ms\n", static_cast<long long>(ms(t)));

        if (cmd == "set") {
            ac::Changes changes;
            for (const auto& a : args) {
                const size_t eq = a.find('=');
                if (eq == std::string::npos) { fprintf(stderr, "bad change '%s' (use key=value)\n", a.c_str()); return 2; }
                changes.emplace_back(a.substr(0, eq), a.substr(eq + 1));
            }
            t = clock::now();
            const bool ok = c.Set(changes, &e);
            fprintf(stderr, "set: %lld ms\n", static_cast<long long>(ms(t)));
            if (!ok) { printf("{\"ok\":false,\"error\":\"%s\",\"state\":%s}\n", e.c_str(), c.SnapshotJson().c_str()); return 1; }
            printf("{\"ok\":true,\"state\":%s}\n", c.SnapshotJson().c_str());
            return 0;
        }
        t = clock::now();
        if (!c.Poll(&e)) { fprintf(stderr, "poll failed: %s\n", e.c_str()); return 1; }
        fprintf(stderr, "full refresh: %lld ms\n", static_cast<long long>(ms(t)));
        printf("{\"capabilities\":%s,\"state\":%s}\n", c.CapabilitiesJson().c_str(), c.SnapshotJson().c_str());
        return 0;
    }

    fprintf(stderr, "unknown command '%s'\n", cmd.c_str());
    return 2;
}
