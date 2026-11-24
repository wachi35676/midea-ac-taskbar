#include "creds.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace midea {
namespace {

// Finds "key": and returns the raw value text (string contents without quotes, or the number).
bool Field(const std::string& json, const std::string& key, std::string& out) {
    const std::string needle = "\"" + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) return false;
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < json.size() && isspace(static_cast<unsigned char>(json[p]))) ++p;
    if (p >= json.size()) return false;
    if (json[p] == '"') {
        const size_t e = json.find('"', p + 1);
        if (e == std::string::npos) return false;
        out = json.substr(p + 1, e - p - 1);
    } else {
        size_t e = p;
        while (e < json.size() && (isdigit(static_cast<unsigned char>(json[e])) || json[e] == '-')) ++e;
        out = json.substr(p, e - p);
    }
    return !out.empty();
}

}  // namespace

bool LoadCreds(const std::wstring& path, Creds& c, std::string* error) {
    // _wfopen rather than wide-path fstreams: MinGW's libstdc++ has no wstring stream constructors.
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        if (error) *error = "cannot open credentials file";
        return false;
    }
    std::string j;
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) j.append(buf, n);
    fclose(f);

    std::string ip, port, id, token, key;
    if (!Field(j, "ip", ip) || !Field(j, "id", id) || !Field(j, "token", token) || !Field(j, "key", key)) {
        if (error) *error = "credentials file is missing ip, id, token or key";
        return false;
    }
    c.ip = ip;
    c.port = Field(j, "port", port) ? atoi(port.c_str()) : 6444;
    c.id = strtoull(id.c_str(), nullptr, 10);
    c.tokenHex = token;
    c.keyHex = key;
    return true;
}

bool SaveCreds(const std::wstring& path, const Creds& c) {
    const std::wstring tmp = path + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    const int ok = fprintf(f, "{\n  \"ip\": \"%s\",\n  \"port\": %d,\n  \"id\": %llu,\n  \"token\": \"%s\",\n  \"key\": \"%s\"\n}",
                           c.ip.c_str(), c.port, static_cast<unsigned long long>(c.id), c.tokenHex.c_str(), c.keyHex.c_str());
    if (fclose(f) != 0 || ok < 0) return false;
    return _wrename(tmp.c_str(), path.c_str()) == 0 ||
           (_wremove(path.c_str()) == 0 && _wrename(tmp.c_str(), path.c_str()) == 0);
}

}  // namespace midea
