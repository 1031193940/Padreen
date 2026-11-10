// Interop test of the PC crypto against Python (PBKDF2 / HMAC) and Java (ECDH).  Not part of the product build.
#include "../src/crypto.h"
#include <cstdio>
#include <fstream>
#include <iostream>
using namespace ph;

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    Bytes salt(16);
    for (int i = 0; i < 16; ++i) salt[i] = (uint8_t)(i * 7 + 1);
    Bytes dk = pbkdf2Sha256("123456789", salt, 100000, 32);
    printf("pbkdf2 %s\n", toHex(dk.data(), dk.size()).c_str());
    Bytes key(32, 0x0b);
    Bytes mac = hmacSha256(key, {Bytes{'a', 'b'}, Bytes{'c'}});
    printf("hmac %s\n", toHex(mac.data(), mac.size()).c_str());
    Bytes h = sha256(Bytes{'a', 'b', 'c'});
    printf("sha256 %s\n", toHex(h.data(), h.size()).c_str());
    if (argc >= 2) {                       // ECDH with the Java side's public key (hex file)
        std::ifstream f(argv[1]);
        std::string hex;
        f >> hex;
        Bytes javaPub;
        if (!fromHex(hex, javaPub)) { printf("bad hex\n"); return 1; }
        Ecdh e;
        if (!e.generate()) { printf("generate failed\n"); return 1; }
        Bytes pub = e.publicKey(), secret;
        bool ok = e.agree(javaPub, secret);
        printf("agree %d\n", ok);
        std::ofstream("pc_pub.hex") << toHex(pub.data(), pub.size());
        if (ok) std::ofstream("pc_secret.hex") << toHex(secret.data(), secret.size());
        Bytes bad = javaPub; bad[40] ^= 1;
        Bytes s2;
        printf("off-curve rejected: %d\n", !e.agree(bad, s2));
    }
    return 0;
}
