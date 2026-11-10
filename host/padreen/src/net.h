#pragma once
#include "common.h"
#include "crypto.h"

namespace ph {

enum : uint8_t {
    T_HELLO = 0x01, T_UPDATE = 0x10, T_GEOM = 0x11, T_ACK = 0x12, T_PING = 0x20, T_PONG = 0x21, T_TOUCH = 0x50, T_REFRESH = 0x51, T_LEAVE = 0x52, T_GESTURE = 0x53,
    T_ORIENT = 0x54, T_MEDIAKEY = 0x55,
    T_PAIR = 0x70, T_CHALLENGE = 0x71, T_AUTH = 0x72, T_AUTH_OK = 0x73, T_AUTH_FAIL = 0x74, T_PAIR_OK = 0x75,
    T_BYE = 0x7F,
};
enum : uint8_t {
    MODE_DU4 = 0, MODE_DU = 1, MODE_GL16 = 2, MODE_GC16_FULL = 3, MODE_DU4_5BIT = 4,
    MODE_A2 = 5, MODE_A2_F6 = 6, MODE_A2_F7 = 7, MODE_A2_F8 = 8, MODE_A2_F9 = 9   // fast direct-path variants (tablet app build >= 5)
};
enum : uint8_t { CODEC_RAW = 0, CODEC_ZLIB = 1, CODEC_MONO1 = 2, CODEC_LZ4 = 3, CODEC_LZ4_MONO = 4 };
// CAP_RESUME: the tablet still shows the picture of the previous session (a reconnect); CAP_ORIENT: it follows the device orientation (T_ORIENT /
// T_GEOM); CAP_MEDIAKEYS: its volume keys are sent as T_MEDIAKEY while the desktop is shown
enum : uint32_t { CAP_ZLIB = 1, CAP_MONO1 = 2, CAP_LZ4 = 4, CAP_LZ4_MONO = 8, CAP_TOUCH = 16, CAP_RESUME = 32, CAP_ORIENT = 64, CAP_MEDIAKEYS = 128 };

// TCP link to the tablet app.
//   USB:   connect("127.0.0.1", 38700) through `adb forward`; plain frames (only adb can reach that port).
//   LAN:   adopt() a connection the LanServer already authenticated (PD2: the tablet connected to us); every frame is an AES-128-GCM record.
// One-way by design: after HELLO the tablet only sends ACKs (used for measurements, never waited for).
class Link {
public:
    ~Link() { close(); }
    bool connect(const char* host, uint16_t port, int helloTimeoutMs, std::string& err);
    // LAN: take over an authenticated PD2 socket; reads the tablet's first record (the data-plane HELLO) and starts the 50 ms keepalive
    bool adopt(SOCKET s, const Bytes& sendKey16, const Bytes& recvKey16, std::string& err);
    // USB only: hand the pairing key + this PC's identity to the tablet and wait for its confirmation
    bool pair(const Bytes& key, const Bytes& pcId, const std::string& pcName, int timeoutMs);
    void close();
    bool connected() const { return s_ != INVALID_SOCKET && alive_; }
    bool secure() const { return secure_; }

    int panelWidth() const { return w_; }
    int panelHeight() const { return h_; }
    // The desktop the PC delivers changed shape (the tablet was turned): from now on this is the size of every UPDATE.
    void setPanelSize(int w, int h) {
        w_ = w;
        h_ = h;
    }
    int appBuild() const { return appBuild_; }
    uint32_t caps() const { return caps_; }
    const Bytes& deviceId() const { return deviceId_; }      // 16 bytes (empty from an old app)
    const Bytes& keyId() const { return keyId_; }            // 4 bytes: SHA-256 prefix of the key the tablet holds for this PC (zeros = unpaired)
    const std::string& padName() const { return padName_; }
    bool tabletLeft() const { return left_; }               // the tablet said it leaves on purpose (T_LEAVE): do not wait for it to come back
    const std::vector<Bytes>& padKeyIds() const { return padKeyIds_; }     // ids (SHA-256 prefix) of every PC key the tablet holds (USB HELLO)

    bool sendUpdate(uint8_t mode, uint8_t codec, const RECT& r, const uint8_t* data, size_t len);
    bool sendBye();

    // Wi-Fi: small PING/PONG exchanges every intervalMs keep both radios out of power-save (each frame resets the inactivity timer)
    void startKeepalive(int intervalMs);
    double lastPingMs() const { return pingMs_; }
    // Touch frames from the tablet (T_TOUCH payloads) are handed to  on the reader thread; keep it quick.
    void setTouchHandler(std::function<void(const uint8_t*, size_t)> f);
    // The tablet's one-key full refresh (volume key / three-finger tap).
    void setGestureHandler(std::function<void(int)> f);     // T_GESTURE: 1 = three-finger swipe up, 2 = swipe down
    // T_ORIENT: the device was turned, please deliver a desktop of w x h (answer with sendGeom and the same seq).  Reader thread: keep it quick.
    void setOrientHandler(std::function<void(int w, int h, int seq)> f);
    void setMediaKeyHandler(std::function<void(int key)> f);   // T_MEDIAKEY: 1 = volume up, 2 = volume down, 3 = mute
    // T_GEOM: the size of the desktop the PC delivers (seq = the request it answers, 0 = announcement: at the start of a session, after a reconnect and
    // whenever the PC's rotation switch changes; refused = it could not switch to the size the tablet asked for, w x h is the size that stays;
    // noRotate = this PC does not follow the tablet's orientation right now: the desktop keeps this shape, the tablet locks to it).
    bool sendGeom(int w, int h, int seq, bool refused, bool noRotate = false);
    void setRefreshHandler(std::function<void()> f);
    double lastRttMs() const { return rttMs_; }
    uint32_t acked() const { return acked_; }
    uint32_t sent() const { return seq_; }

private:
    bool openSocket(const char* host, uint16_t port, int timeoutMs, std::string& err);
    bool readPlain(uint8_t& type, Bytes& payload);       // handshake frames (always plain)
    bool readFrame(uint8_t& type, Bytes& payload);       // plain or decrypted
    bool sendRaw(const uint8_t* p, size_t n);
    bool sendBuffer(const uint8_t* frame, size_t n);     // a complete plain frame; encrypted when secure (caller holds sendMu_)
    bool sendFramePlain(uint8_t type, const uint8_t* p, size_t n);
    void parseHello(const Bytes& pl);
    void startReader();
    void readerLoop();

    SOCKET s_ = INVALID_SOCKET;
    std::atomic<bool> alive_{false};
    std::thread reader_;
    std::mutex sendMu_;
    uint32_t seq_ = 0;
    std::atomic<uint32_t> acked_{0};
    std::atomic<double> rttMs_{0}, pingMs_{0};
    std::thread keepalive_;
    std::mutex touchMu_;
    std::function<void(const uint8_t*, size_t)> touchHandler_;
    std::function<void()> refreshHandler_;
    std::function<void(int)> gestureHandler_;
    std::function<void(int, int, int)> orientHandler_;
    std::function<void(int)> mediaKeyHandler_;
    std::atomic<int> keepaliveMs_{0};
    int w_ = 0, h_ = 0, appBuild_ = 0;
    uint32_t caps_ = 0;
    Bytes deviceId_, keyId_;
    std::string padName_;
    std::atomic<bool> left_{false};
    std::vector<Bytes> padKeyIds_;
    bool secure_ = false;
    GcmKey sendKey_, recvKey_;
    uint64_t sendCtr_ = 0, recvCtr_ = 0;
    std::atomic<bool> pairOk_{false};
    static constexpr int kRing = 256;
    std::atomic<int64_t> sendNs_[kRing];
    std::vector<uint8_t> msg_, rec_, rbuf_;
};

}  // namespace ph
