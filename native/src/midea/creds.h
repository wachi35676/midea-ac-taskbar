#pragma once
// Reads and writes ac_creds.json: {"ip": "...", "port": 6444, "id": 123, "token": "hex", "key": "hex"}.
// A deliberately tiny reader for that flat object only.
#include <string>

#include "bytes.h"

namespace midea {

struct Creds {
    std::string ip;
    int port = 6444;
    uint64_t id = 0;
    std::string tokenHex, keyHex;
};

bool LoadCreds(const std::wstring& path, Creds& out, std::string* error = nullptr);
bool SaveCreds(const std::wstring& path, const Creds& c);

}  // namespace midea
