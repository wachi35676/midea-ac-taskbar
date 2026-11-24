#pragma once
// Small byte-buffer helpers shared by the Midea protocol code.
#include <cstdint>
#include <string>
#include <vector>

namespace midea {

using Bytes = std::vector<uint8_t>;

inline Bytes Slice(const Bytes& b, size_t from, size_t to) {
    if (from > b.size()) from = b.size();
    if (to > b.size()) to = b.size();
    if (to < from) to = from;
    return Bytes(b.begin() + from, b.begin() + to);
}

inline void Append(Bytes& dst, const Bytes& src) { dst.insert(dst.end(), src.begin(), src.end()); }

inline std::string ToHex(const Bytes& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(b.size() * 2);
    for (uint8_t c : b) { s += d[c >> 4]; s += d[c & 0xF]; }
    return s;
}

inline bool FromHex(const std::string& s, Bytes& out) {
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if (s.size() % 2) return false;
    out.clear();
    out.reserve(s.size() / 2);
    for (size_t i = 0; i < s.size(); i += 2) {
        int hi = nib(s[i]), lo = nib(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<uint8_t>(hi << 4 | lo));
    }
    return true;
}

}  // namespace midea
