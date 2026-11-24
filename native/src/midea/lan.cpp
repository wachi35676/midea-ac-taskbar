#include "lan.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstring>
#include <ctime>
#include <thread>

#include "crypto.h"

#pragma comment(lib, "ws2_32.lib")

namespace midea {
namespace {

const Bytes kSignKey = [] {
    const char* s = "xhdiwjnchekd4d512chdjx5d8e4c394D2D7S";
    return Bytes(s, s + strlen(s));
}();
const Bytes& EncKey() {
    static const Bytes k = crypto::Md5(kSignKey);
    return k;
}

constexpr int kRetries = 3;
constexpr int kReadTimeoutMs = 2000;
constexpr int kConnectTimeoutMs = 5000;
constexpr auto kAuthLifetime = std::chrono::hours(12);

enum PacketType : uint8_t {
    HandshakeRequest = 0x0, HandshakeResponse = 0x1, EncryptedResponse = 0x3, EncryptedRequest = 0x6, Error = 0xF
};

void SetErr(std::string* e, const std::string& msg) {
    if (e) *e = msg;
}

Bytes Header(uint16_t length, uint8_t extra) {
    return Bytes{0x83, 0x70, static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length), 0x20, extra};
}

struct WsaInit {
    WsaInit() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
    ~WsaInit() { WSACleanup(); }
};
void EnsureWsa() { static WsaInit init; }

}  // namespace

// ---------------- security ----------------
namespace security {
Bytes EncryptAes(const Bytes& data) { return crypto::AesEcbEncrypt(EncKey(), crypto::Pkcs7Pad(data)); }

bool DecryptAes(const Bytes& data, Bytes& out) {
    out = crypto::AesEcbDecrypt(EncKey(), data);
    return !out.empty() && crypto::Pkcs7Unpad(out);
}

Bytes Sign(const Bytes& data) {
    Bytes d = data;
    Append(d, kSignKey);
    return crypto::Md5(d);
}

Bytes Udpid(const Bytes& deviceId) {
    Bytes h = crypto::Sha256(deviceId), out(16);
    for (int i = 0; i < 16; ++i) out[i] = h[i] ^ h[i + 16];
    return out;
}
}  // namespace security

// ---------------- V2 packet ----------------
namespace packet {
Bytes Encode(uint64_t deviceId, const Bytes& frame, const Bytes& timestamp) {
    Bytes enc = security::EncryptAes(frame);
    const uint16_t length = static_cast<uint16_t>(40 + enc.size() + 16);

    Bytes p = {0x5A, 0x5A, 0x01, 0x11, static_cast<uint8_t>(length), static_cast<uint8_t>(length >> 8), 0x20, 0x00};
    p.insert(p.end(), 4, 0);  // message id
    if (timestamp.size() == 8) {
        Append(p, timestamp);
    } else {
        // YYYYMMDDHHMMSSmm, least significant first, each a 2-digit component (UTC)
        SYSTEMTIME t;
        GetSystemTime(&t);
        Append(p, Bytes{static_cast<uint8_t>(t.wMilliseconds / 10), static_cast<uint8_t>(t.wSecond),
                        static_cast<uint8_t>(t.wMinute), static_cast<uint8_t>(t.wHour), static_cast<uint8_t>(t.wDay),
                        static_cast<uint8_t>(t.wMonth), static_cast<uint8_t>(t.wYear % 100),
                        static_cast<uint8_t>(t.wYear / 100)});
    }
    for (int i = 0; i < 8; ++i) p.push_back(static_cast<uint8_t>(deviceId >> (8 * i)));
    p.insert(p.end(), 12, 0);
    Append(p, enc);
    Append(p, security::Sign(p));
    return p;
}

bool Decode(const Bytes& data, Bytes& frame, std::string* error) {
    if (data.size() < 6) return SetErr(error, "packet too short"), false;
    if (data[0] != 0x5A || data[1] != 0x5A) return SetErr(error, "unsupported packet " + ToHex(Slice(data, 0, 8))), false;
    const size_t length = data[4] | (data[5] << 8);
    if (data.size() < length || length < 56) return SetErr(error, "packet truncated"), false;
    Bytes p = Slice(data, 0, length);
    Bytes body = Slice(p, 0, length - 16);
    if (security::Sign(body) != Slice(p, length - 16, length))
        return SetErr(error, "packet MD5 mismatch"), false;
    if (!security::DecryptAes(Slice(p, 40, length - 16), frame)) return SetErr(error, "packet decrypt failed"), false;
    return true;
}
}  // namespace packet

// ---------------- V3 envelope ----------------
namespace v3 {
Bytes EncodeHandshake(uint16_t packetId, const Bytes& token) {
    Bytes p = Header(static_cast<uint16_t>(token.size()), HandshakeRequest);
    p.push_back(static_cast<uint8_t>(packetId >> 8));
    p.push_back(static_cast<uint8_t>(packetId));
    Append(p, token);
    return p;
}

Bytes EncodeEncrypted(uint16_t packetId, const Bytes& localKey, const Bytes& data, const Bytes* fixedPad) {
    const size_t remainder = (data.size() + 2) % 16;
    const uint8_t pad = static_cast<uint8_t>(remainder ? 16 - remainder : 0);
    const uint16_t length = static_cast<uint16_t>(data.size() + pad + 32);
    Bytes header = Header(length, static_cast<uint8_t>(pad << 4 | EncryptedRequest));

    Bytes payload = {static_cast<uint8_t>(packetId >> 8), static_cast<uint8_t>(packetId)};
    Append(payload, data);
    Append(payload, fixedPad ? Slice(*fixedPad, 0, pad) : crypto::Random(pad));

    Bytes hashInput = header;
    Append(hashInput, payload);
    Bytes out = header;
    Append(out, crypto::AesCbcEncrypt(localKey, payload));
    Append(out, crypto::Sha256(hashInput));
    return out;
}

bool DeriveLocalKey(const Bytes& key, const Bytes& response, Bytes& localKey) {
    if (response.size() != 64 || key.size() != 32) return false;
    Bytes dec = crypto::AesCbcDecrypt(key, Slice(response, 0, 32));
    if (dec.size() != 32 || crypto::Sha256(dec) != Slice(response, 32, 64)) return false;
    localKey.resize(32);
    for (int i = 0; i < 32; ++i) localKey[i] = dec[i] ^ key[i];
    return true;
}

bool DecodePacket(const Bytes& localKey, const Bytes& pkt, Bytes& out, std::string* error) {
    if (pkt.size() < 8 || pkt[0] != 0x83 || pkt[1] != 0x70) return SetErr(error, "invalid start of packet"), false;
    if (pkt[4] != 0x20) return SetErr(error, "invalid magic byte"), false;
    const uint8_t type = pkt[5] & 0xF;
    if (type == HandshakeResponse) {
        out = Slice(pkt, 8, pkt.size());
        return true;
    }
    if (type == Error) return SetErr(error, "error packet received"), false;
    if (type != EncryptedResponse) return SetErr(error, "unexpected packet type " + std::to_string(type)), false;
    if (pkt.size() < 6 + 32 + 16) return SetErr(error, "encrypted packet too short"), false;

    Bytes header = Slice(pkt, 0, 6);
    Bytes dec = crypto::AesCbcDecrypt(localKey, Slice(pkt, 6, pkt.size() - 32));
    if (dec.empty()) return SetErr(error, "decrypt failed"), false;
    Bytes hashInput = header;
    Append(hashInput, dec);
    if (crypto::Sha256(hashInput) != Slice(pkt, pkt.size() - 32, pkt.size()))
        return SetErr(error, "SHA256 mismatch"), false;
    const size_t pad = header[5] >> 4;
    if (dec.size() < 2 + pad) return SetErr(error, "bad padding"), false;
    out = Slice(dec, 2, dec.size() - pad);
    return true;
}
}  // namespace v3

// ---------------- session ----------------
Lan::Lan(std::string ip, int port, uint64_t deviceId)
    : ip_(std::move(ip)), port_(port), deviceId_(deviceId), sock_(INVALID_SOCKET) {
    EnsureWsa();
}

Lan::~Lan() { Disconnect(); }

void Lan::SetAddress(const std::string& ip, int port) {
    if (ip != ip_ || port != port_) Disconnect();
    ip_ = ip;
    port_ = port;
}

void Lan::Disconnect() {
    if (sock_ != INVALID_SOCKET) closesocket(static_cast<SOCKET>(sock_));
    sock_ = INVALID_SOCKET;
    buffer_.clear();
    localKey_.clear();
}

bool Lan::Authenticated() const {
    return !localKey_.empty() && std::chrono::steady_clock::now() < authExpiry_;
}

bool Lan::Connect(std::string* error) {
    Disconnect();
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return SetErr(error, "socket failed"), false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port_));
    if (inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr) != 1) {
        closesocket(s);
        return SetErr(error, "bad IP address " + ip_), false;
    }

    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    fd_set w, e;
    FD_ZERO(&w); FD_SET(s, &w);
    FD_ZERO(&e); FD_SET(s, &e);
    timeval tv{kConnectTimeoutMs / 1000, (kConnectTimeoutMs % 1000) * 1000};
    if (select(0, nullptr, &w, &e, &tv) <= 0 || FD_ISSET(s, &e)) {
        closesocket(s);
        return SetErr(error, "connect to " + ip_ + " failed or timed out"), false;
    }
    nonBlocking = 0;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    BOOL noDelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    sock_ = static_cast<uintptr_t>(s);
    packetId_ = 0;
    return true;
}

bool Lan::Write(const Bytes& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = send(static_cast<SOCKET>(sock_), reinterpret_cast<const char*>(data.data() + sent),
                     static_cast<int>(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += n;
    }
    packetId_ = (packetId_ + 1) & 0xFFF;
    return true;
}

Lan::ReadResult Lan::ReadPacket(int timeoutMs, Bytes& pkt) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        // Complete packet already buffered?
        for (size_t i = 0; i + 1 < buffer_.size(); ++i) {
            if (buffer_[i] == 0x83 && buffer_[i + 1] == 0x70) {
                if (i) buffer_.erase(buffer_.begin(), buffer_.begin() + i);  // junk before a packet
                break;
            }
        }
        if (buffer_.size() >= 6 && buffer_[0] == 0x83 && buffer_[1] == 0x70) {
            const size_t total = ((buffer_[2] << 8) | buffer_[3]) + 8;
            if (buffer_.size() >= total) {
                pkt.assign(buffer_.begin(), buffer_.begin() + total);
                buffer_.erase(buffer_.begin(), buffer_.begin() + total);
                return ReadResult::Ok;
            }
        }

        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        const long waitMs = timeoutMs == 0 ? 0 : static_cast<long>(left > 0 ? left : 0);
        if (timeoutMs != 0 && left <= 0) return ReadResult::Timeout;

        fd_set r;
        FD_ZERO(&r);
        FD_SET(static_cast<SOCKET>(sock_), &r);
        timeval tv{waitMs / 1000, (waitMs % 1000) * 1000};
        const int sel = select(0, &r, nullptr, nullptr, &tv);
        if (sel < 0) return ReadResult::Error;
        if (sel == 0) return ReadResult::Timeout;

        uint8_t chunk[2048];
        const int n = recv(static_cast<SOCKET>(sock_), reinterpret_cast<char*>(chunk), sizeof(chunk), 0);
        if (n <= 0) return ReadResult::Error;  // closed or failed
        buffer_.insert(buffer_.end(), chunk, chunk + n);
    }
}

bool Lan::Authenticate(const Bytes& token, const Bytes& key, std::string* error) {
    if (token.empty() || key.empty()) return SetErr(error, "token and key must be supplied"), false;
    if (sock_ == INVALID_SOCKET && !Connect(error)) return false;
    buffer_.clear();

    for (int retries = kRetries; retries > 0; --retries) {
        if (!Write(v3::EncodeHandshake(packetId_, token))) {
            Disconnect();
            return SetErr(error, "handshake send failed"), false;
        }
        Bytes pkt, response;
        const ReadResult rr = ReadPacket(kReadTimeoutMs, pkt);
        if (rr == ReadResult::Timeout && retries > 1) continue;
        if (rr != ReadResult::Ok) {
            Disconnect();
            return SetErr(error, rr == ReadResult::Timeout ? "no response from host" : "connection lost"), false;
        }
        std::string e;
        if (!v3::DecodePacket({}, pkt, response, &e) || !v3::DeriveLocalKey(key, response, localKey_)) {
            Disconnect();
            return SetErr(error, e.empty() ? "authentication rejected" : e), false;
        }
        token_ = token;
        key_ = key;
        authExpiry_ = std::chrono::steady_clock::now() + kAuthLifetime;
        std::this_thread::sleep_for(std::chrono::seconds(1));  // the device needs a moment after the handshake
        return true;
    }
    return false;
}

std::vector<Bytes> Lan::Send(const Bytes& frame, std::string* error) {
    std::vector<Bytes> responses;
    if (sock_ == INVALID_SOCKET && !Connect(error)) return responses;
    if (!Authenticated() && !Authenticate(token_, key_, error)) return responses;

    const Bytes v2 = packet::Encode(deviceId_, frame);

    auto takeAvailable = [&](bool& failed) {
        Bytes pkt, inner, f;
        while (ReadPacket(0, pkt) == ReadResult::Ok) {
            std::string e;
            if (!v3::DecodePacket(localKey_, pkt, inner, &e) || !packet::Decode(inner, f, &e)) {
                SetErr(error, e);
                failed = true;
                return;
            }
            responses.push_back(f);
        }
    };

    bool failed = false;
    takeAvailable(failed);  // anything the device sent on its own
    if (failed) { Disconnect(); return {}; }

    for (int retries = kRetries; retries > 0; --retries) {
        if (!Write(v3::EncodeEncrypted(packetId_, localKey_, v2))) {
            Disconnect();
            SetErr(error, "send failed");
            return {};
        }
        Bytes pkt;
        const ReadResult rr = ReadPacket(kReadTimeoutMs, pkt);
        if (rr == ReadResult::Timeout) {
            if (retries > 1) continue;
            Disconnect();
            SetErr(error, "no response from host");
            return responses;
        }
        if (rr == ReadResult::Error) {
            Disconnect();
            SetErr(error, "connection lost");
            return {};
        }
        Bytes inner, f;
        std::string e;
        if (!v3::DecodePacket(localKey_, pkt, inner, &e) || !packet::Decode(inner, f, &e)) {
            Disconnect();
            SetErr(error, e);
            return {};
        }
        responses.push_back(f);
        break;
    }

    takeAvailable(failed);
    if (failed) Disconnect();
    return responses;
}

}  // namespace midea
