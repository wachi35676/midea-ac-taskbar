#pragma once
// UDP discovery of Midea devices on the local network (broadcast to ports 6445 and 20086).
// No cloud access: it only reports what each device announces.
#include <string>
#include <vector>

#include "bytes.h"

namespace midea {

struct DiscoveredDevice {
    std::string ip;      // address the reply came from
    int port = 0;
    uint64_t id = 0;
    int version = 0;     // 2 or 3
    std::string sn, name;
    int type = 0;        // 0xAC = air conditioner
};

// Parses one discovery reply. Exposed for tests.
bool ParseDiscoveryReply(const std::string& fromIp, const Bytes& data, DiscoveredDevice& out, std::string* error = nullptr);

// Broadcasts `packets` times over `timeoutMs` and returns each device once.
// `target` may be a single IP instead of the broadcast address.
std::vector<DiscoveredDevice> Discover(int timeoutMs = 3000, int packets = 3,
                                       const std::string& target = "255.255.255.255");

}  // namespace midea
