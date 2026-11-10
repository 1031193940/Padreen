#include "crypto.h"

#include <bcrypt.h>
#include <wincrypt.h>

namespace ph {

static BCRYPT_ALG_HANDLE hmacAlg() {
    static BCRYPT_ALG_HANDLE a = [] {
        BCRYPT_ALG_HANDLE h = nullptr;
        BCryptOpenAlgorithmProvider(&h, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
        return h;
    }();
    return a;
}

static BCRYPT_ALG_HANDLE gcmAlg() {
    static BCRYPT_ALG_HANDLE a = [] {
        BCRYPT_ALG_HANDLE h = nullptr;
        if (BCryptOpenAlgorithmProvider(&h, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) return (BCRYPT_ALG_HANDLE) nullptr;
        BCryptSetProperty(h, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
        return h;
    }();
    return a;
}

bool randomBytes(uint8_t* p, size_t n) { return BCryptGenRandom(nullptr, p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0; }

Bytes sha256(const Bytes& data) {
    static BCRYPT_ALG_HANDLE alg = [] {
        BCRYPT_ALG_HANDLE h = nullptr;
        BCryptOpenAlgorithmProvider(&h, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        return h;
    }();
    Bytes out(32, 0);
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) != 0) return Bytes();
    if (!data.empty()) BCryptHashData(h, (PUCHAR)data.data(), (ULONG)data.size(), 0);
    BCryptFinishHash(h, out.data(), 32, 0);
    BCryptDestroyHash(h);
    return out;
}
Bytes hmacSha256(const Bytes& key, const std::vector<Bytes>& parts) {
    Bytes out(32, 0);
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptCreateHash(hmacAlg(), &h, nullptr, 0, (PUCHAR)key.data(), (ULONG)key.size(), 0) != 0) return Bytes();
    for (const Bytes& p : parts)
        if (!p.empty()) BCryptHashData(h, (PUCHAR)p.data(), (ULONG)p.size(), 0);
    BCryptFinishHash(h, out.data(), 32, 0);
    BCryptDestroyHash(h);
    return out;
}

bool GcmKey::init(const uint8_t key[16]) {
    reset();
    BCRYPT_KEY_HANDLE k = nullptr;
    if (!gcmAlg() || BCryptGenerateSymmetricKey(gcmAlg(), &k, nullptr, 0, (PUCHAR)key, 16, 0) != 0) return false;
    key_ = k;
    return true;
}

void GcmKey::reset() {
    if (key_) BCryptDestroyKey((BCRYPT_KEY_HANDLE)key_);
    key_ = nullptr;
}

static void nonceOf(uint64_t ctr, uint8_t n[12]) {
    memset(n, 0, 12);
    for (int i = 0; i < 8; ++i) n[4 + i] = (uint8_t)(ctr >> (8 * i));
}

bool GcmKey::seal(const uint8_t* aad, size_t aadLen, uint64_t counter, const uint8_t* in, size_t n, uint8_t* out) {
    if (!key_) return false;
    uint8_t nonce[12];
    nonceOf(counter, nonce);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = nonce;
    info.cbNonce = 12;
    info.pbAuthData = (PUCHAR)aad;
    info.cbAuthData = (ULONG)aadLen;
    info.pbTag = out + n;
    info.cbTag = 16;
    ULONG got = 0;
    return BCryptEncrypt((BCRYPT_KEY_HANDLE)key_, (PUCHAR)in, (ULONG)n, &info, nullptr, 0, out, (ULONG)n, &got, 0) == 0;
}

bool GcmKey::open(const uint8_t* aad, size_t aadLen, uint64_t counter, const uint8_t* in, size_t n, uint8_t* out) {
    if (!key_ || n < 16) return false;
    uint8_t nonce[12];
    nonceOf(counter, nonce);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = nonce;
    info.cbNonce = 12;
    info.pbAuthData = (PUCHAR)aad;
    info.cbAuthData = (ULONG)aadLen;
    info.pbTag = (PUCHAR)in + (n - 16);
    info.cbTag = 16;
    ULONG got = 0;
    return BCryptDecrypt((BCRYPT_KEY_HANDLE)key_, (PUCHAR)in, (ULONG)(n - 16), &info, nullptr, 0, out, (ULONG)(n - 16), &got, 0) == 0;
}

bool equalCt(const Bytes& a, const Bytes& b) {
    if (a.size() != b.size()) return false;
    uint8_t d = 0;
    for (size_t i = 0; i < a.size(); ++i) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

Bytes pbkdf2Sha256(const std::string& password, const Bytes& salt, uint32_t iterations, size_t outLen) {
    Bytes out(outLen, 0);
    if (BCryptDeriveKeyPBKDF2(hmacAlg(), (PUCHAR)password.data(), (ULONG)password.size(), (PUCHAR)salt.data(), (ULONG)salt.size(), iterations, out.data(),
                              (ULONG)outLen, 0) != 0)
        return Bytes();
    return out;
}

static BCRYPT_ALG_HANDLE ecdhAlg() {
    static BCRYPT_ALG_HANDLE a = [] {
        BCRYPT_ALG_HANDLE h = nullptr;
        if (BCryptOpenAlgorithmProvider(&h, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0) != 0) return (BCRYPT_ALG_HANDLE) nullptr;
        return h;
    }();
    return a;
}

bool Ecdh::generate() {
    reset();
    BCRYPT_KEY_HANDLE k = nullptr;
    if (!ecdhAlg() || BCryptGenerateKeyPair(ecdhAlg(), &k, 256, 0) != 0) return false;
    if (BCryptFinalizeKeyPair(k, 0) != 0) {
        BCryptDestroyKey(k);
        return false;
    }
    key_ = k;
    return true;
}

void Ecdh::reset() {
    if (key_) BCryptDestroyKey((BCRYPT_KEY_HANDLE)key_);
    key_ = nullptr;
}

Bytes Ecdh::publicKey() const {
    if (!key_) return Bytes();
    ULONG n = 0;
    if (BCryptExportKey((BCRYPT_KEY_HANDLE)key_, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &n, 0) != 0) return Bytes();
    Bytes blob(n);
    if (BCryptExportKey((BCRYPT_KEY_HANDLE)key_, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob.data(), n, &n, 0) != 0) return Bytes();
    auto* hdr = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    if (hdr->cbKey != 32 || n < sizeof(BCRYPT_ECCKEY_BLOB) + 64) return Bytes();
    Bytes pub(65);
    pub[0] = 0x04;
    memcpy(&pub[1], blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), 64);    // X || Y, already big endian
    return pub;
}

bool Ecdh::agree(const Bytes& peer, Bytes& secret) {
    if (!key_ || peer.size() != 65 || peer[0] != 0x04) return false;
    Bytes blob(sizeof(BCRYPT_ECCKEY_BLOB) + 64);
    auto* hdr = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    hdr->dwMagic = BCRYPT_ECDH_PUBLIC_P256_MAGIC;
    hdr->cbKey = 32;
    memcpy(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), &peer[1], 64);
    BCRYPT_KEY_HANDLE pk = nullptr;
    if (BCryptImportKeyPair(ecdhAlg(), nullptr, BCRYPT_ECCPUBLIC_BLOB, &pk, blob.data(), (ULONG)blob.size(), 0) != 0) return false;   // validates the point
    BCRYPT_SECRET_HANDLE sec = nullptr;
    bool ok = false;
    if (BCryptSecretAgreement((BCRYPT_KEY_HANDLE)key_, pk, &sec, 0) == 0) {
        ULONG got = 0;
        Bytes raw(32);
        if (BCryptDeriveKey(sec, BCRYPT_KDF_RAW_SECRET, nullptr, raw.data(), 32, &got, 0) == 0 && got == 32) {
            secret.assign(raw.rbegin(), raw.rend());           // CNG hands the raw secret out little endian
            ok = true;
        }
        BCryptDestroySecret(sec);
    }
    BCryptDestroyKey(pk);
    return ok;
}
std::string toHex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

bool fromHex(const std::string& s, Bytes& out) {
    out.clear();
    if (s.size() % 2) return false;
    auto v = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (size_t i = 0; i < s.size(); i += 2) {
        int a = v(s[i]), b = v(s[i + 1]);
        if (a < 0 || b < 0) return false;
        out.push_back((uint8_t)(a * 16 + b));
    }
    return true;
}

std::string dpapiProtectHex(const Bytes& data) {
    DATA_BLOB in = {(DWORD)data.size(), (BYTE*)data.data()}, out = {};
    if (!CryptProtectData(&in, L"Padreen pairing key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return "";
    std::string s = toHex(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return s;
}

bool dpapiUnprotectHex(const std::string& hex, Bytes& outBytes) {
    Bytes blob;
    if (!fromHex(hex, blob) || blob.empty()) return false;
    DATA_BLOB in = {(DWORD)blob.size(), blob.data()}, out = {};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    outBytes.assign(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return true;
}

}  // namespace ph
