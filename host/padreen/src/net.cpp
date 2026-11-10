#include "net.h"

#include <algorithm>

namespace ph {

static bool recvAll(SOCKET s, uint8_t* p, size_t n) {
    while (n) {
        int r = recv(s, (char*)p, (int)std::min<size_t>(n, 1 << 20), 0);
        if (r <= 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

bool Link::openSocket(const char* host, uint16_t port, int timeoutMs, std::string& err) {
    close();
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        err = "socket() failed";
        return false;
    }
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) {
        err = "bad address";
        closesocket(s);
        return false;
    }
    // connect with a timeout (a dead Wi-Fi address would otherwise block for ~20 s)
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    int rc = ::connect(s, (sockaddr*)&a, sizeof(a));
    if (rc != 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set wf, ef;
        FD_ZERO(&wf);
        FD_ZERO(&ef);
        FD_SET(s, &wf);
        FD_SET(s, &ef);
        timeval tv = {timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        int n = select(0, nullptr, &wf, &ef, &tv);
        rc = (n > 0 && FD_ISSET(s, &wf)) ? 0 : -1;
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    if (rc != 0) {
        err = "connect failed (" + std::to_string(WSAGetLastError()) + ")";
        closesocket(s);
        return false;
    }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    int sndbuf = 256 * 1024;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbuf, sizeof(sndbuf));
    DWORD to = (DWORD)timeoutMs;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    s_ = s;
    secure_ = false;
    return true;
}

bool Link::readPlain(uint8_t& type, Bytes& payload) {
    uint8_t hdr[8];
    if (!recvAll(s_, hdr, 8)) return false;
    uint32_t len = le32(hdr + 4);
    if (len > (1u << 20)) return false;
    payload.resize(len);
    if (len && !recvAll(s_, payload.data(), len)) return false;
    type = hdr[0];
    return true;
}

bool Link::readFrame(uint8_t& type, Bytes& payload) {
    if (!secure_) return readPlain(type, payload);
    uint8_t lenb[4];
    if (!recvAll(s_, lenb, 4)) return false;
    uint32_t clen = le32(lenb);
    if (clen < 8 + 16 || clen > (1u << 20)) return false;
    rbuf_.resize(clen);
    if (!recvAll(s_, rbuf_.data(), clen)) return false;
    Bytes plain(clen - 16);
    if (!recvKey_.open(lenb, 4, recvCtr_++, rbuf_.data(), clen, plain.data())) return false;     // forged / replayed / corrupted
    uint32_t len = le32(plain.data() + 4);
    if (len != plain.size() - 8) return false;
    type = plain[0];
    payload.assign(plain.begin() + 8, plain.end());
    return true;
}

void Link::parseHello(const Bytes& pl) {
    w_ = pl[2] | (pl[3] << 8);
    h_ = pl[4] | (pl[5] << 8);
    appBuild_ = pl[9];
    caps_ = le32(pl.data() + 12);
    deviceId_.clear();
    keyId_.clear();
    padName_.clear();
    if (pl.size() >= 36) {
        deviceId_.assign(pl.begin() + 16, pl.begin() + 32);
        keyId_.assign(pl.begin() + 32, pl.begin() + 36);
    }
    padKeyIds_.clear();
    if (pl.size() >= 37 && 37u + pl[36] <= pl.size()) {
        padName_.assign((const char*)&pl[37], pl[36]);
        size_t o = 37u + pl[36];
        if (o < pl.size()) {
            size_t n = pl[o++];
            for (size_t i = 0; i < n && o + 4 <= pl.size(); ++i, o += 4) padKeyIds_.emplace_back(pl.begin() + o, pl.begin() + o + 4);
        }
    }
}

void Link::startReader() {
    for (auto& v : sendNs_) v = 0;
    seq_ = 0;
    acked_ = 0;
    pairOk_ = false;
    DWORD none = 0;
    setsockopt(s_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&none, sizeof(none));
    alive_ = true;
    reader_ = std::thread([this] { readerLoop(); });
}

bool Link::connect(const char* host, uint16_t port, int helloTimeoutMs, std::string& err) {
    if (!openSocket(host, port, helloTimeoutMs, err)) return false;
    // HELLO: the app sends it right after accept().  `adb forward` accepts the PC side even when nothing listens on the
    // tablet, so a closed socket / timeout here means "app not ready", not "connected".
    uint8_t type = 0;
    Bytes pl;
    if (!readPlain(type, pl) || type != T_HELLO || pl.size() < 16) {
        err = "no HELLO from the tablet app";
        close();
        return false;
    }
    parseHello(pl);
    startReader();
    return true;
}

bool Link::adopt(SOCKET s, const Bytes& sendKey, const Bytes& recvKey, std::string& err) {
    close();
    s_ = s;
    secure_ = false;
    DWORD to = 6000;
    setsockopt(s_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    int one = 1;
    setsockopt(s_, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    int sndbuf = 256 * 1024;
    setsockopt(s_, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbuf, sizeof(sndbuf));
    if (sendKey.size() != 16 || recvKey.size() != 16 || !sendKey_.init(sendKey.data()) || !recvKey_.init(recvKey.data())) {
        err = "crypto failure";
        s_ = INVALID_SOCKET;
        closesocket(s);
        return false;
    }
    sendCtr_ = recvCtr_ = 0;
    left_ = false;
    secure_ = true;
    uint8_t type = 0;
    Bytes pl;
    if (!readFrame(type, pl) || type != T_HELLO || pl.size() < 16) {       // the tablet's first record is the data-plane HELLO
        err = "no HELLO from the tablet";
        close();
        return false;
    }
    parseHello(pl);
    startReader();
    startKeepalive(50);
    return true;
}

bool Link::pair(const Bytes& key, const Bytes& pcId, const std::string& pcName, int timeoutMs) {
    if (!connected() || secure_ || key.size() != 32 || pcId.size() != 16) return false;
    pairOk_ = false;
    Bytes pl = key;
    pl.insert(pl.end(), pcId.begin(), pcId.end());
    std::string nm = pcName.substr(0, 40);
    pl.push_back((uint8_t)nm.size());
    pl.insert(pl.end(), nm.begin(), nm.end());
    if (!sendFramePlain(T_PAIR, pl.data(), pl.size())) return false;
    int64_t deadline = nowNs() + (int64_t)timeoutMs * 1000000LL;
    while (nowNs() < deadline) {
        if (pairOk_) return true;
        Sleep(10);
    }
    return pairOk_;
}
void Link::startKeepalive(int intervalMs) {
    if (keepalive_.joinable() || intervalMs <= 0) return;
    keepaliveMs_ = intervalMs;
    keepalive_ = std::thread([this] {
        while (alive_) {
            int64_t t = nowNs();
            sendFramePlain(T_PING, reinterpret_cast<const uint8_t*>(&t), 8);
            Sleep((DWORD)keepaliveMs_.load());
        }
    });
}
void Link::setTouchHandler(std::function<void(const uint8_t*, size_t)> f) {
    std::lock_guard<std::mutex> lk(touchMu_);
    touchHandler_ = std::move(f);
}

void Link::setGestureHandler(std::function<void(int)> f) {
    std::lock_guard<std::mutex> lk(touchMu_);
    gestureHandler_ = std::move(f);
}

void Link::setRefreshHandler(std::function<void()> f) {
    std::lock_guard<std::mutex> lk(touchMu_);
    refreshHandler_ = std::move(f);
}

void Link::setOrientHandler(std::function<void(int, int, int)> f) {
    std::lock_guard<std::mutex> lk(touchMu_);
    orientHandler_ = std::move(f);
}

void Link::setMediaKeyHandler(std::function<void(int)> f) {
    std::lock_guard<std::mutex> lk(touchMu_);
    mediaKeyHandler_ = std::move(f);
}

bool Link::sendGeom(int w, int h, int seq, bool refused, bool noRotate) {
    if (!connected()) return false;
    uint8_t p[8] = {0};
    p[0] = (uint8_t)(w & 0xff);
    p[1] = (uint8_t)((w >> 8) & 0xff);
    p[2] = (uint8_t)(h & 0xff);
    p[3] = (uint8_t)((h >> 8) & 0xff);
    p[4] = (uint8_t)seq;
    p[5] = (uint8_t)((refused ? 1 : 0) | (noRotate ? 2 : 0));      // bit0 refused, bit1 NO_ROTATE (this PC keeps the shape whatever the tablet does)
    return sendFramePlain(T_GEOM, p, sizeof(p));
}

void Link::readerLoop() {
    Bytes pl;
    while (alive_) {
        uint8_t type = 0;
        if (!readFrame(type, pl)) break;
        if (type == T_ACK && pl.size() >= 44) {
            uint32_t seq = le32(pl.data());
            int64_t t0 = sendNs_[seq % kRing];
            if (t0) rttMs_ = (nowNs() - t0) / 1e6;
            acked_ = seq;
        } else if (type == T_PAIR_OK) {
            pairOk_ = true;
        } else if (type == T_LEAVE) {
            left_ = true;
        } else if (type == T_GESTURE && !pl.empty()) {
            std::lock_guard<std::mutex> lk(touchMu_);
            if (gestureHandler_) gestureHandler_(pl[0]);
        } else if (type == T_ORIENT && pl.size() >= 5) {
            std::lock_guard<std::mutex> lk(touchMu_);
            if (orientHandler_) orientHandler_(pl[0] | (pl[1] << 8), pl[2] | (pl[3] << 8), pl[4]);
        } else if (type == T_MEDIAKEY && !pl.empty()) {
            std::lock_guard<std::mutex> lk(touchMu_);
            if (mediaKeyHandler_) mediaKeyHandler_(pl[0]);
        } else if (type == T_REFRESH) {
            std::lock_guard<std::mutex> lk(touchMu_);
            if (refreshHandler_) refreshHandler_();
        } else if (type == T_TOUCH) {
            std::lock_guard<std::mutex> lk(touchMu_);
            if (touchHandler_) touchHandler_(pl.data(), pl.size());
        } else if (type == T_PONG && pl.size() >= 8) {
            int64_t t0;
            memcpy(&t0, pl.data(), 8);
            pingMs_ = (nowNs() - t0) / 1e6;
        }
    }
    alive_ = false;
}

bool Link::sendRaw(const uint8_t* p, size_t n) {
    while (n) {
        int r = send(s_, (const char*)p, (int)std::min<size_t>(n, 1 << 20), 0);
        if (r <= 0) {
            alive_ = false;
            return false;
        }
        p += r;
        n -= (size_t)r;
    }
    return true;
}

bool Link::sendBuffer(const uint8_t* frame, size_t n) {
    if (!secure_) return sendRaw(frame, n);
    rec_.resize(4 + n + 16);
    put32(rec_.data(), (uint32_t)(n + 16));
    if (!sendKey_.seal(rec_.data(), 4, sendCtr_++, frame, n, rec_.data() + 4)) {
        alive_ = false;
        return false;
    }
    return sendRaw(rec_.data(), rec_.size());
}

bool Link::sendFramePlain(uint8_t type, const uint8_t* p, size_t n) {
    std::lock_guard<std::mutex> lk(sendMu_);
    std::vector<uint8_t> f(8 + n, 0);
    f[0] = type;
    put32(f.data() + 4, (uint32_t)n);
    if (n) memcpy(f.data() + 8, p, n);
    return sendBuffer(f.data(), f.size());
}

bool Link::sendUpdate(uint8_t mode, uint8_t codec, const RECT& r, const uint8_t* data, size_t len) {
    if (!connected()) return false;
    std::lock_guard<std::mutex> lk(sendMu_);
    uint32_t payload = 18 + (uint32_t)len;
    msg_.resize(8 + payload);
    uint8_t* m = msg_.data();
    m[0] = T_UPDATE;
    m[1] = 0;
    m[2] = m[3] = 0;
    put32(m + 4, payload);
    uint32_t seq = ++seq_;
    put32(m + 8, seq);
    m[12] = mode;
    m[13] = codec;
    uint16_t x = (uint16_t)r.left, y = (uint16_t)r.top, w = (uint16_t)(r.right - r.left), h = (uint16_t)(r.bottom - r.top);
    memcpy(m + 14, &x, 2);
    memcpy(m + 16, &y, 2);
    memcpy(m + 18, &w, 2);
    memcpy(m + 20, &h, 2);
    put32(m + 22, (uint32_t)len);
    if (len) memcpy(m + 26, data, len);
    sendNs_[seq % kRing] = nowNs();
    return sendBuffer(m, msg_.size());
}

bool Link::sendBye() {
    if (!connected()) return false;
    return sendFramePlain(T_BYE, nullptr, 0);
}

void Link::close() {
    alive_ = false;
    if (s_ != INVALID_SOCKET) {
        shutdown(s_, SD_BOTH);
        closesocket(s_);
    }
    if (keepalive_.joinable()) keepalive_.join();
    if (reader_.joinable()) reader_.join();
    s_ = INVALID_SOCKET;
    secure_ = false;
    sendKey_.reset();
    recvKey_.reset();
}

}  // namespace ph
