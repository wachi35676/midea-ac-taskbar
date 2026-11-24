#pragma once
// High-level AC control for the taskbar app: connect, poll, validated writes, rediscovery by device ID,
// and a JSON snapshot in the shape of docs/CONTRACT.md. Blocking: call from one worker thread only.
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ac_device.h"
#include "creds.h"

namespace midea::ac {

// One requested change, e.g. {"target_temperature", "24"}, {"mode", "cool"}, {"power", "true"}.
using Changes = std::vector<std::pair<std::string, std::string>>;

class Controller {
public:
    explicit Controller(std::wstring credsPath);

    // Connects, authenticates and reads capabilities. Rediscovers the unit if its address changed.
    bool Connect(std::string* error = nullptr);
    // Full read (state, diagnostics, properties). Reconnects/rediscovers once on failure.
    bool Poll(std::string* error = nullptr);
    // Light read, validate, write, keep the AC's reply. Returns false with a user-readable reason.
    bool Set(const Changes& changes, std::string* error = nullptr);

    bool Online() const { return online_; }
    // Beep on changes. This unit never reports it (it's a flag sent with each command), so the controller
    // keeps it and applies it to every new connection; the app saves it between runs.
    void SetBeep(bool on) { beep_ = on; if (dev_) dev_->SetBeep(on); }
    bool Beep() const { return beep_; }
    const State& state() const;
    const Capabilities& capabilities() const;
    std::string SnapshotJson() const;      // CONTRACT.md snapshot
    std::string CapabilitiesJson() const;  // CONTRACT.md capabilities

    // Test hooks (acctl watch): drop the TCP session, or point the live session at another address.
    void TestDropConnection() { if (dev_) dev_->Disconnect(); }
    void TestSetAddress(const std::string& ip) { if (dev_) dev_->SetAddress(ip, creds_.port); }

private:
    bool Rediscover();
    // Checks every change; stages them on the device only when `stage` is true (after all passed).
    bool Validate(const Changes& changes, bool stage, std::string* error);

    std::wstring credsPath_;
    Creds creds_;
    std::unique_ptr<Device> dev_;
    bool online_ = false;
    bool beep_ = false;
    std::string updatedAt_;
};

// Names used in the JSON contract.
const char* ModeName(int mode);
const char* FanName(int fan);  // "custom" for 1-100 values that aren't presets
const char* SwingName(int swing);

}  // namespace midea::ac
