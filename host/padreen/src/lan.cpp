#include "lan.h"

#include <algorithm>
#include <map>

namespace ph {

bool g_debugPair = false;

static const Bytes B(const char* s) { return Bytes(s, s + strlen(s)); }

static bool recvAll(SOCKET s, uint8_t* p, size_t n) {
    while (n) {
        int r = recv(s, (char*)p, (int)std::min<size_t>(n, 1 << 16), 0);
        if (r <= 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static bool sendAll(SOCKET s, const uint8_t* p, size_t n) {
    while (n) {
        int r = send(s, (const char*)p, (int)std::min<size_t>(n, 1 << 16), 0);
        if (r <= 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

// plain handshake frame: u8 type | u8 flags | u16 rsv | u32 len | payload (small: <= 1 KB)
static bool recvFrame(SOCKET s, uint8_t& type, Bytes& payload) {
    uint8_t h[8];
    if (!recvAll(s, h, 8)) return false;
    uint32_t len = h[4] | (h[5] << 8) | (h[6] << 16) | ((uint32_t)h[7] << 24);
    if (len > 1024) return false;
    payload.resize(len);
    if (len && !recvAll(s, payload.data(), len)) return false;
    type = h[0];
    return true;
}

static bool sendFrame(SOCKET s, uint8_t type, const Bytes& payload) {
    Bytes f(8 + payload.size(), 0);
    f[0] = type;
    uint32_t n = (uint32_t)payload.size();
    memcpy(&f[4], &n, 4);
    if (n) memcpy(&f[8], payload.data(), n);
    return sendAll(s, f.data(), f.size());
}

static void sendFail(SOCKET s, uint8_t reason) { sendFrame(s, PD_FAIL, Bytes{reason}); }

static Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes o;
    for (const Bytes& p : parts) o.insert(o.end(), p.begin(), p.end());
    return o;
}

bool LanServer::start(Devices* dev, uint16_t tcpPort, uint16_t udpPort, std::string& err) {
    stop();
    dev_ = dev;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    tcp_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    udp_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (tcp_ == INVALID_SOCKET || udp_ == INVALID_SOCKET) {
        err = "socket() failed";
        stop();
        return false;
    }
    int one = 1;
    setsockopt(tcp_, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    setsockopt(udp_, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    setsockopt(udp_, SOL_SOCKET, SO_BROADCAST, (const char*)&one, sizeof(one));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(tcpPort);
    if (bind(tcp_, (sockaddr*)&a, sizeof(a)) != 0 || listen(tcp_, 8) != 0) {
        err = "cannot listen on TCP " + std::to_string(tcpPort) + " (" + std::to_string(WSAGetLastError()) + ")";
        stop();
        return false;
    }
    a.sin_port = htons(udpPort);
    if (bind(udp_, (sockaddr*)&a, sizeof(a)) != 0) {
        err = "cannot bind UDP " + std::to_string(udpPort) + " (" + std::to_string(WSAGetLastError()) + ")";
        stop();
        return false;
    }
    tcpPort_ = tcpPort;
    run_ = true;
    tcpTh_ = std::thread([this] { tcpLoop(); });
    udpTh_ = std::thread([this] { udpLoop(); });
    logf("lan: listening on TCP %u / UDP %u", tcpPort, udpPort);
    return true;
}

void LanServer::stop() {
    run_ = false;
    if (tcp_ != INVALID_SOCKET) closesocket(tcp_);
    if (udp_ != INVALID_SOCKET) closesocket(udp_);
    if (tcpTh_.joinable()) tcpTh_.join();
    if (udpTh_.joinable()) udpTh_.join();
    tcp_ = udp_ = INVALID_SOCKET;
    for (int i = 0; i < 80 && active_ > 0; ++i) Sleep(100);      // handshake threads end within their 6 s timeouts
    std::lock_guard<std::mutex> lk(mu_);
    for (AcceptedLink& l : queue_)
        if (l.sock != INVALID_SOCKET) closesocket(l.sock);
    queue_.clear();
}

// ---------------------------------------------------------------------------------------------------------------- pairing window
std::string LanServer::openPairing(int seconds) {
    std::string code;
    while (code.size() < 9) {
        uint8_t b[16];
        randomBytes(b, sizeof(b));
        for (uint8_t v : b)
            if (v < 250 && code.size() < 9) code += (char)('0' + v % 10);     // rejection sampling: no modulo bias
    }
    {
        std::lock_guard<std::mutex> lk(pm_);
        pairOpen_ = true;
        code_ = code;
        pairExpiresNs_ = nowNs() + (int64_t)seconds * 1000000000LL;
        attempts_ = 5;
    }
    logf("lan: pairing window opened (%d s)", seconds);
    if (g_debugPair) logf("DEBUG pairing code: %s", code.c_str());
    if (onActivity) onActivity();
    return code;
}

void LanServer::closePairing() {
    {
        std::lock_guard<std::mutex> lk(pm_);
        pairOpen_ = false;
        code_.clear();
    }
    if (onActivity) onActivity();
}

LanServer::PairState LanServer::pairing() {
    std::lock_guard<std::mutex> lk(pm_);
    PairState s;
    if (pairOpen_ && nowNs() >= pairExpiresNs_) {
        pairOpen_ = false;
        code_.clear();
    }
    s.open = pairOpen_;
    s.code = code_;
    s.secondsLeft = pairOpen_ ? (int)((pairExpiresNs_ - nowNs()) / 1000000000LL) : 0;
    s.attemptsLeft = attempts_;
    return s;
}

// ---------------------------------------------------------------------------------------------------------------- UDP discovery
void LanServer::udpLoop() {
    std::map<uint32_t, int64_t> last;                       // per-source rate limit
    while (run_) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(udp_, &rf);
        timeval tv = {0, 500000};
        if (select(0, &rf, nullptr, nullptr, &tv) <= 0) continue;
        uint8_t buf[256];
        sockaddr_in from = {};
        int fl = sizeof(from);
        int n = recvfrom(udp_, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (n < 20 || memcmp(buf, "PDQ2", 4) != 0 || !enabled_) continue;
        int64_t now = nowNs();
        int64_t& l = last[from.sin_addr.s_addr];
        if (now - l < 200000000LL) continue;
        l = now;
        if (last.size() > 512) last.clear();
        PairState ps = pairing();
        bool busyForAsker = busy_;
        if (busyForAsker) {                                  // a session that waits for this very tablet to reconnect is not busy for it
            std::lock_guard<std::mutex> lk(em_);
            busyForAsker = !(!expected_.empty() && n >= 20 && expected_.size() == 16 && memcmp(buf + 4, expected_.data(), 16) == 0);
        }
        std::string name = dev_->pcName();
        Bytes id = dev_->pcId();
        Bytes r;
        r.insert(r.end(), {'P', 'D', 'R', '2', 2});
        r.push_back((uint8_t)((ps.open ? 1 : 0) | (busyForAsker ? 2 : 0)));
        r.push_back((uint8_t)(tcpPort_ & 0xff));
        r.push_back((uint8_t)(tcpPort_ >> 8));
        r.insert(r.end(), id.begin(), id.end());
        r.push_back((uint8_t)name.size());
        r.insert(r.end(), name.begin(), name.end());
        sendto(udp_, (const char*)r.data(), (int)r.size(), 0, (sockaddr*)&from, sizeof(from));
    }
}

// ---------------------------------------------------------------------------------------------------------------- TCP
void LanServer::tcpLoop() {
    while (run_) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(tcp_, &rf);
        timeval tv = {0, 500000};
        if (select(0, &rf, nullptr, nullptr, &tv) <= 0) continue;
        sockaddr_in from = {};
        int fl = sizeof(from);
        SOCKET s = accept(tcp_, (sockaddr*)&from, &fl);
        if (s == INVALID_SOCKET) continue;
        if (active_ >= 8 || !enabled_) {
            closesocket(s);
            continue;
        }
        char ip[64] = {};
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        active_++;
        std::thread([this, s, ipStr = std::string(ip)] {
            handle(s, ipStr);
            active_--;
        }).detach();
    }
}

void LanServer::handle(SOCKET s, std::string ip) {
    DWORD to = 6000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&to, sizeof(to));
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    uint8_t type = 0;
    Bytes hello;
    bool handedOver = false;                                  // true once the socket belongs to the accepted-link queue
    if (recvFrame(s, type, hello) && type == PD_HELLO && hello.size() >= 19) {
        int nameLen = hello[18];
        if (19 + nameLen <= (int)hello.size()) {
            Bytes padId(hello.begin() + 2, hello.begin() + 18);
            std::string padName((const char*)&hello[19], (size_t)nameLen);
            if (hello[0] != 2) sendFail(s, FAIL_PROTO);
            else if (!enabled_) sendFail(s, FAIL_DISABLED);
            else if (hello[1] == 0) handedOver = connectFlow(s, hello, padId, padName, ip);
            else if (hello[1] == 1) pairFlow(s, hello, padId, padName, ip);
            else sendFail(s, FAIL_PROTO);
        }
    }
    if (!handedOver) closesocket(s);
}
bool LanServer::connectFlow(SOCKET s, const Bytes& hello, const Bytes& padId, const std::string& padName, const std::string& ip) {
    (void)hello;
    if (busy_) {
        bool mine;
        {
            std::lock_guard<std::mutex> lk(em_);
            mine = !expected_.empty() && expected_ == padId;
        }
        if (!mine) {
            sendFail(s, FAIL_BUSY);
            return false;
        }
    }
    PairedPad pad;
    if (!dev_->find(padId, pad)) {
        logf("lan: %s (%s) is not paired", padName.c_str(), ip.c_str());
        sendFail(s, FAIL_UNPAIRED);
        return false;
    }
    Bytes pcId = dev_->pcId(), nonceS(16);
    randomBytes(nonceS.data(), 16);
    std::string pcName = dev_->pcName();
    Bytes ch = cat({pcId, nonceS, Bytes{(uint8_t)pcName.size()}, Bytes(pcName.begin(), pcName.end())});
    uint8_t type = 0;
    Bytes auth;
    if (!sendFrame(s, PD_CHALLENGE, ch) || !recvFrame(s, type, auth) || type != PD_AUTH || auth.size() != 48) return false;
    Bytes nonceC(auth.begin(), auth.begin() + 16), macC(auth.begin() + 16, auth.end());
    Bytes expect = hmacSha256(pad.key, {B("PD2-auth-c"), pcId, padId, nonceS, nonceC});
    if (!equalCt(expect, macC)) {
        logf("lan: wrong proof from %s (%s)", padName.c_str(), ip.c_str());
        Sleep(500);
        sendFail(s, FAIL_AUTH);
        return false;
    }
    Bytes macS = hmacSha256(pad.key, {B("PD2-auth-s"), padId, pcId, nonceC, nonceS});
    if (!sendFrame(s, PD_AUTH_OK, macS)) return false;
    Bytes salt = cat({nonceS, nonceC});
    Bytes prk = hmacSha256(salt, {pad.key});
    Bytes c2s = hmacSha256(prk, {B("pd2 c2s"), Bytes{1}});
    Bytes s2c = hmacSha256(prk, {B("pd2 s2c"), Bytes{1}});
    AcceptedLink al;
    al.sock = s;
    al.sendKey.assign(s2c.begin(), s2c.begin() + 16);      // what we encrypt with (PC -> tablet)
    al.recvKey.assign(c2s.begin(), c2s.begin() + 16);      // what the tablet encrypts with
    al.padId = padId;
    al.padName = padName;
    al.ip = ip;
    al.atNs = nowNs();
    dev_->touch(padId, ip);
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(std::move(al));
    }
    logf("lan: %s (%s) authenticated", padName.c_str(), ip.c_str());
    if (onActivity) onActivity();
    return true;
}

void LanServer::pairFlow(SOCKET s, const Bytes& hello, const Bytes& padId, const std::string& padName, const std::string& ip) {
    PairState ps = pairing();
    if (!ps.open) {
        sendFail(s, FAIL_NOT_PAIRING);
        return;
    }
    Ecdh ecdh;
    if (!ecdh.generate()) {
        sendFail(s, FAIL_PROTO);
        return;
    }
    Bytes pcId = dev_->pcId(), salt(16);
    randomBytes(salt.data(), 16);
    std::string pcName = dev_->pcName();
    Bytes pubS = ecdh.publicKey();
    Bytes ch = cat({pcId, Bytes{(uint8_t)pcName.size()}, Bytes(pcName.begin(), pcName.end()), pubS, salt, Bytes{9}});
    uint8_t type = 0;
    Bytes proof;
    if (!sendFrame(s, PD_PAIR_CH, ch) || !recvFrame(s, type, proof) || type != PD_PAIR_PROOF || proof.size() != 97) return;
    Bytes pubC(proof.begin(), proof.begin() + 65), proofC(proof.begin() + 65, proof.end());
    std::string code;
    {
        std::lock_guard<std::mutex> lk(pm_);
        if (!pairOpen_ || attempts_ <= 0 || nowNs() >= pairExpiresNs_) {
            sendFail(s, FAIL_CODE);
            return;
        }
        code = code_;
    }
    Bytes kpw = pbkdf2Sha256(code, salt, 100000, 32);
    Bytes transcript = sha256(cat({B("PD2-pair"), hello, ch, pubC}));
    Bytes expect = hmacSha256(kpw, {B("PD2-pair-c"), transcript});
    if (!equalCt(expect, proofC)) {
        bool closed = false;
        {
            std::lock_guard<std::mutex> lk(pm_);
            if (--attempts_ <= 0) {
                pairOpen_ = false;
                code_.clear();
                closed = true;
            }
        }
        logf("lan: wrong pairing code from %s (%s)%s", padName.c_str(), ip.c_str(), closed ? " - pairing window closed (too many attempts)" : "");
        Sleep(1000);
        sendFail(s, FAIL_CODE);
        if (onActivity) onActivity();
        return;
    }
    Bytes z;
    if (!ecdh.agree(pubC, z)) {
        sendFail(s, FAIL_PROTO);
        return;
    }
    Bytes k = hmacSha256(z, {B("PD2-key"), transcript});
    PairedPad pad;
    pad.id = padId;
    pad.name = padName;
    pad.key = k;
    pad.ip = ip;
    dev_->put(pad);
    Bytes proofS = hmacSha256(kpw, {B("PD2-pair-s"), transcript});
    sendFrame(s, PD_PAIR_OK, proofS);
    closePairing();
    logf("lan: paired with %s (%s)", padName.c_str(), ip.c_str());
    if (onPaired) onPaired(pad);
}

bool LanServer::pop(AcceptedLink& out) {
    std::lock_guard<std::mutex> lk(mu_);
    while (!queue_.empty()) {
        AcceptedLink l = std::move(queue_.front());
        queue_.pop_front();
        if (nowNs() - l.atNs > 15000000000LL) {                // nobody took it in time (a USB session was running)
            closesocket(l.sock);
            continue;
        }
        out = std::move(l);
        return true;
    }
    return false;
}

}  // namespace ph
