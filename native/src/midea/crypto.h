#pragma once
// Crypto primitives for the Midea LAN protocol, on Windows CNG (bcrypt.dll). No third-party code.
#include "bytes.h"

namespace midea::crypto {

Bytes Md5(const Bytes& data);
Bytes Sha256(const Bytes& data);
Bytes Random(size_t n);

// AES with no padding; data must be a multiple of 16 bytes. Returns empty on failure.
Bytes AesEcbEncrypt(const Bytes& key, const Bytes& data);
Bytes AesEcbDecrypt(const Bytes& key, const Bytes& data);
// CBC with an all-zero IV, as the V3 protocol uses.
Bytes AesCbcEncrypt(const Bytes& key, const Bytes& data);
Bytes AesCbcDecrypt(const Bytes& key, const Bytes& data);

Bytes Pkcs7Pad(const Bytes& data);
bool Pkcs7Unpad(Bytes& data);  // false if the padding is invalid

}  // namespace midea::crypto
