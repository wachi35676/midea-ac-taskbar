#include "discover.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>

#include "lan.h"

namespace midea {
namespace {

const Bytes kDiscoveryMsg = {
    0x5a, 0x5a, 0x01, 0x11, 0x48, 0x00, 0x92, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7f, 0x75, 0xbd, 0x6b, 0x3e, 0x4f, 0x8b, 0x76,
    0x2e, 0x84, 0x9c, 0x6e, 0x57, 0x8d, 0x65, 0x90, 0x03, 0x6e, 0x9d, 0x43, 0x42, 0xa5, 0x0f, 0x1f,
    0x56, 0x9e, 0xb8, 0xec, 0x91, 0x8e, 0x92, 0xe5};

void SetErr(std::string* e, const char* msg) {
    if (e) *e = msg;
}

}  // namespace

bool ParseDiscoveryReply(const std::string& fromIp, const Bytes& raw, DiscoveredDevice& out, std::string* error) {
    if (raw.size() < 2) return SetErr(error, "reply too short"), false;
    int version = 0;
    if (raw[0] == 0x5A && raw[1] == 0x5A) version = 2;
    else if (raw[0] == 0x83 && raw[1] == 0x70) version = 3;
    else return SetErr(error, "not a V2/V3 reply"), false;

    Bytes data = version == 3 ? Slice(raw, 8, raw.size() >= 16 ? raw.size() - 16 : 8) : raw;
    if (data.size() < 40 + 16) return SetErr(error, "reply too short"), false;

    uint64_t id = 0;
    for (int i = 0; i < 6; ++i) id |= static_cast<uint64_t>(data[20 + i]) << (8 * i);

    Bytes dec;
    if (!security::DecryptAes(Slice(data, 40, data.size() - 16), dec) || dec.size() < 41)
        return SetErr(error, "failed to decrypt reply"), false;

    out = {};
    out.ip = fromIp;
    out.port = dec[4] | (dec[5] << 8);
    out.id = id;
    out.version = version;
    out.sn.assign(dec.begin() + 8, dec.begin() + 40);
    const size_t nameLen = dec[40];
    if (41 + nameLen <= dec.size()) out.name.assign(dec.begin() + 41, dec.begin() + 41 + nameLen);
    // Name looks like "net_ac_AF4C": the second part is the device type in hex.
    const size_t a = out.name.find('_'), b = out.name.find('_', a + 1);
    if (a != std::string::npos) {
        try { out.type = std::stoi(out.name.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1), nullptr, 16); }
        catch (...) { out.type = 0; }
    }
    return true;
}

std::vector<DiscoveredDevice> Discover(int timeoutMs, int packets, const std::string& target) {
    std::vector<DiscoveredDevice> found;
    { Lan warmup("127.0.0.1", 0, 0); }  // makes sure Winsock is initialised

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return found;
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local));

    auto sendAll = [&] {
        for (u_short port : {static_cast<u_short>(6445), static_cast<u_short>(20086)}) {
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_port = htons(port);
            inet_pton(AF_INET, target.c_str(), &to.sin_addr);
            sendto(s, reinterpret_cast<const char*>(kDiscoveryMsg.data()), static_cast<int>(kDiscoveryMsg.size()), 0,
                   reinterpret_cast<sockaddr*>(&to), sizeof(to));
        }
    };

    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    const auto end = start + std::chrono::milliseconds(timeoutMs);
    const auto gap = std::chrono::milliseconds(packets > 0 ? timeoutMs / packets : timeoutMs);
    int sent = 0;
    auto nextSend = start;

    while (clock::now() < end) {
        if (sent < packets && clock::now() >= nextSend) {
            sendAll();
            ++sent;
            nextSend += gap;
        }
        const auto wake = (sent < packets && nextSend < end) ? nextSend : end;
        long ms = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(wake - clock::now()).count());
        if (ms < 0) ms = 0;
        fd_set r;
        FD_ZERO(&r);
        FD_SET(s, &r);
        timeval tv{ms / 1000, (ms % 1000) * 1000};
        if (select(0, &r, nullptr, nullptr, &tv) <= 0) continue;

        uint8_t buf[1024];
        sockaddr_in from{};
        int fromLen = sizeof(from);
        const int n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n <= 0) continue;
        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));

        DiscoveredDevice d;
        if (!ParseDiscoveryReply(ip, Bytes(buf, buf + n), d)) continue;
        bool dup = false;
        for (const auto& f : found) dup |= f.id == d.id;
        if (!dup) found.push_back(d);
    }
    closesocket(s);
    return found;
}

}  // namespace midea
