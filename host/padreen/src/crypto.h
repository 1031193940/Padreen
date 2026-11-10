#pragma once
#include "common.h"

namespace ph {

using Bytes = std::vector<uint8_t>;

bool randomBytes(uint8_t* p, size_t n);

Bytes sha256(const Bytes& data);

// HMAC-SHA256 over the concatenation of parts.
Bytes hmacSha256(const Bytes& key, const std::vector<Bytes>& parts);

// AES-128-GCM with a counter nonce (4 zero bytes + u64 little endian), as the tablet does it.
class GcmKey {
public:
    GcmKey() = default;
    ~GcmKey() { reset(); }
    GcmKey(const GcmKey&) = delete;
    GcmKey& operator=(const GcmKey&) = delete;
    bool init(const uint8_t key[16]);
    void reset();
    // out must have room for n + 16 (ciphertext + tag)
    bool seal(const uint8_t* aad, size_t aadLen, uint64_t counter, const uint8_t* in, size_t n, uint8_t* out);
    // in = ciphertext + tag (n bytes in total); out gets n - 16 bytes; false if the tag does not verify
    bool open(const uint8_t* aad, size_t aadLen, uint64_t counter, const uint8_t* in, size_t n, uint8_t* out);

private:
    void* key_ = nullptr;    // BCRYPT_KEY_HANDLE
};

// PBKDF2-HMAC-SHA256 (the pairing code is stretched with it before it is used as a password).
Bytes pbkdf2Sha256(const std::string& password, const Bytes& salt, uint32_t iterations, size_t outLen);

// ECDH on NIST P-256 (CNG).  Public keys travel as the 65-byte uncompressed point 0x04 | X | Y (big endian), the shared secret is the
// 32-byte X coordinate (big endian) - the same bytes javax.crypto's KeyAgreement("ECDH") returns on the tablet.
class Ecdh {
public:
    Ecdh() = default;
    ~Ecdh() { reset(); }
    Ecdh(const Ecdh&) = delete;
    Ecdh& operator=(const Ecdh&) = delete;
    bool generate();
    void reset();
    Bytes publicKey() const;                                      // 65 bytes (empty if not generated)
    bool agree(const Bytes& peerPublic65, Bytes& secret32);       // false for a malformed / off-curve peer key

private:
    void* key_ = nullptr;    // BCRYPT_KEY_HANDLE
};

// constant-time comparison (proofs, MACs)
bool equalCt(const Bytes& a, const Bytes& b);
// Per-user protection of the pairing key kept in padreen.ini (Windows DPAPI): hex string <-> bytes.
std::string dpapiProtectHex(const Bytes& data);
bool dpapiUnprotectHex(const std::string& hex, Bytes& out);

std::string toHex(const uint8_t* p, size_t n);
bool fromHex(const std::string& s, Bytes& out);

}  // namespace ph
