#include "crypto.h"

#include <windows.h>
#include <bcrypt.h>

#include <cwchar>

#pragma comment(lib, "bcrypt.lib")

namespace midea::crypto {
namespace {

Bytes Hash(LPCWSTR alg, const Bytes& data) {
    BCRYPT_ALG_HANDLE h = nullptr;
    Bytes out;
    if (BCryptOpenAlgorithmProvider(&h, alg, nullptr, 0) != 0) return out;
    DWORD len = 0, got = 0;
    BCryptGetProperty(h, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&len), sizeof(len), &got, 0);
    BCRYPT_HASH_HANDLE hh = nullptr;
    if (BCryptCreateHash(h, &hh, nullptr, 0, nullptr, 0, 0) == 0) {
        out.resize(len);
        if (BCryptHashData(hh, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0) != 0 ||
            BCryptFinishHash(hh, out.data(), len, 0) != 0)
            out.clear();
        BCryptDestroyHash(hh);
    }
    BCryptCloseAlgorithmProvider(h, 0);
    return out;
}

Bytes Aes(LPCWSTR mode, bool encrypt, const Bytes& key, const Bytes& data) {
    Bytes out;
    if (data.size() % 16 || (key.size() != 16 && key.size() != 24 && key.size() != 32)) return out;
    BCRYPT_ALG_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&h, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) return out;
    BCRYPT_KEY_HANDLE k = nullptr;
    const DWORD modeBytes = static_cast<DWORD>((wcslen(mode) + 1) * sizeof(wchar_t));
    if (BCryptSetProperty(h, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<LPWSTR>(mode)), modeBytes, 0) == 0 &&
        BCryptGenerateSymmetricKey(h, &k, nullptr, 0, const_cast<PUCHAR>(key.data()),
                                   static_cast<ULONG>(key.size()), 0) == 0) {
        uint8_t iv[16] = {};
        const bool cbc = wcscmp(mode, BCRYPT_CHAIN_MODE_CBC) == 0;
        out.resize(data.size());
        ULONG got = 0;
        NTSTATUS st = encrypt
            ? BCryptEncrypt(k, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), nullptr,
                            cbc ? iv : nullptr, cbc ? sizeof(iv) : 0, out.data(), static_cast<ULONG>(out.size()), &got, 0)
            : BCryptDecrypt(k, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), nullptr,
                            cbc ? iv : nullptr, cbc ? sizeof(iv) : 0, out.data(), static_cast<ULONG>(out.size()), &got, 0);
        if (st != 0 || got != data.size()) out.clear();
        BCryptDestroyKey(k);
    }
    BCryptCloseAlgorithmProvider(h, 0);
    return out;
}

}  // namespace

Bytes Md5(const Bytes& data) { return Hash(BCRYPT_MD5_ALGORITHM, data); }
Bytes Sha256(const Bytes& data) { return Hash(BCRYPT_SHA256_ALGORITHM, data); }

Bytes Random(size_t n) {
    Bytes out(n);
    if (n && BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        out.assign(n, 0);
    return out;
}

Bytes AesEcbEncrypt(const Bytes& key, const Bytes& data) { return Aes(BCRYPT_CHAIN_MODE_ECB, true, key, data); }
Bytes AesEcbDecrypt(const Bytes& key, const Bytes& data) { return Aes(BCRYPT_CHAIN_MODE_ECB, false, key, data); }
Bytes AesCbcEncrypt(const Bytes& key, const Bytes& data) { return Aes(BCRYPT_CHAIN_MODE_CBC, true, key, data); }
Bytes AesCbcDecrypt(const Bytes& key, const Bytes& data) { return Aes(BCRYPT_CHAIN_MODE_CBC, false, key, data); }

Bytes Pkcs7Pad(const Bytes& data) {
    Bytes out = data;
    const uint8_t pad = static_cast<uint8_t>(16 - data.size() % 16);
    out.insert(out.end(), pad, pad);
    return out;
}

bool Pkcs7Unpad(Bytes& data) {
    if (data.empty() || data.size() % 16) return false;
    const uint8_t pad = data.back();
    if (pad == 0 || pad > 16 || pad > data.size()) return false;
    for (size_t i = data.size() - pad; i < data.size(); ++i)
        if (data[i] != pad) return false;
    data.resize(data.size() - pad);
    return true;
}

}  // namespace midea::crypto
