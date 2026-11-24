#pragma once
// Midea LAN transport: V3 ("8370") encrypted session over TCP 6444, carrying V2 ("5A5A") packets that
// wrap AA command frames. Blocking calls with timeouts; use from a worker thread, never a UI thread.
// Ported from msmart-ng (mill1000/midea-msmart) lan.py.
#include <chrono>
#include <string>

#include "bytes.h"

namespace midea {

namespace security {
Bytes EncryptAes(const Bytes& data);          // AES-128-ECB, fixed key, PKCS7
bool DecryptAes(const Bytes& data, Bytes& out);
Bytes Sign(const Bytes& data);                // MD5(data + sign key)
Bytes Udpid(const Bytes& deviceId);           // SHA256 halves XORed (cloud token lookup)
}  // namespace security

namespace packet {
// V2 packet with an encrypted command frame. `timestamp` (8 bytes) is filled from the clock when empty.
Bytes Encode(uint64_t deviceId, const Bytes& frame, const Bytes& timestamp = {});
bool Decode(const Bytes& packet, Bytes& frame, std::string* error = nullptr);
}  // namespace packet

namespace v3 {
// Pure helpers for the V3 envelope, exposed for tests.
Bytes EncodeHandshake(uint16_t packetId, const Bytes& token);
Bytes EncodeEncrypted(uint16_t packetId, const Bytes& localKey, const Bytes& data, const Bytes* fixedPad = nullptr);
bool DeriveLocalKey(const Bytes& key, const Bytes& response64, Bytes& localKey);
// Decodes one complete 8370 packet: returns the inner payload (handshake data or decrypted V2 packet).
bool DecodePacket(const Bytes& localKey, const Bytes& pkt, Bytes& out, std::string* error = nullptr);
}  // namespace v3

class Lan {
public:
    Lan(std::string ip, int port, uint64_t deviceId);
    ~Lan();
    Lan(const Lan&) = delete;
    Lan& operator=(const Lan&) = delete;

    // Connects (if needed) and performs the V3 handshake with the cloud-issued token and key.
    bool Authenticate(const Bytes& token, const Bytes& key, std::string* error = nullptr);
    // Sends one AA frame and returns every response frame received (empty on timeout or error).
    std::vector<Bytes> Send(const Bytes& frame, std::string* error = nullptr);
    void Disconnect();

    const std::string& Ip() const { return ip_; }
    void SetAddress(const std::string& ip, int port);

private:
    bool Connect(std::string* error);
    bool Write(const Bytes& data);
    // Reads one complete 8370 packet. timeoutMs = 0 only returns what has already arrived.
    enum class ReadResult { Ok, Timeout, Error };
    ReadResult ReadPacket(int timeoutMs, Bytes& pkt);
    bool Authenticated() const;

    std::string ip_;
    int port_;
    uint64_t deviceId_;
    uintptr_t sock_;
    Bytes buffer_;
    uint16_t packetId_ = 0;
    Bytes token_, key_, localKey_;
    std::chrono::steady_clock::time_point authExpiry_{};
};

}  // namespace midea
