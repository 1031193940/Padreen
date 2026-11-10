#pragma once
#include <deque>

#include "devices.h"

namespace ph {

extern bool g_debugPair;   // --debug: the pairing code is written to the log (tests only; never in normal runs)

// PD2 handshake frame types / failure reasons (see protocol/PROTOCOL.md, "局域网连接与配对")
enum : uint8_t {
    PD_HELLO = 0x80, PD_CHALLENGE = 0x81, PD_AUTH = 0x82, PD_AUTH_OK = 0x83, PD_FAIL = 0x84,
    PD_PAIR_CH = 0x85, PD_PAIR_PROOF = 0x86, PD_PAIR_OK = 0x87,
};
enum : uint8_t { FAIL_UNPAIRED = 1, FAIL_AUTH = 2, FAIL_BUSY = 3, FAIL_NOT_PAIRING = 4, FAIL_CODE = 5, FAIL_DISABLED = 6, FAIL_PROTO = 7 };

// An authenticated connection from a paired tablet, waiting for the supervisor to run a session on it.
struct AcceptedLink {
    SOCKET sock = INVALID_SOCKET;
    Bytes sendKey, recvKey;      // AES-128 keys: PC->tablet and tablet->PC
    Bytes padId;
    std::string padName, ip;
    int64_t atNs = 0;
};

// The PC side of the LAN connection: answers the tablet's UDP discovery, accepts its TCP connections, runs the PD2 handshake
// (authenticated connect, or pairing with the code shown in the pairing window).  Inbound only on purpose: it is the one place where
// Padreen needs a Windows Firewall rule (TCP + UDP, local subnet).
class LanServer {
public:
    ~LanServer() { stop(); }
    bool start(Devices* dev, uint16_t tcpPort, uint16_t udpPort, std::string& err);
    void stop();
    bool running() const { return run_; }

    // ---- pairing window ----
    std::string openPairing(int seconds);              // returns the 9-digit code; replaces a previous one
    void closePairing();
    struct PairState {
        bool open = false;
        std::string code;
        int secondsLeft = 0;
        int attemptsLeft = 0;
    };
    PairState pairing();
    std::function<void(const PairedPad&)> onPaired;    // a tablet was paired (called on a handshake thread)
    std::function<void()> onActivity;                  // something the UI / supervisor should look at (accepted link, pairing finished)

    // ---- sessions ----
    bool pop(AcceptedLink& out);                       // next authenticated connection (stale ones are dropped)
    void setBusy(bool b) { busy_ = b; }                // a session is running: new connections are told "busy"
    // while a session waits for its tablet to reconnect after a link loss, that tablet (and only it) may connect although the session is "busy"
    void setExpected(const Bytes& padId) {
        std::lock_guard<std::mutex> lk(em_);
        expected_ = padId;
    }
    void setEnabled(bool e) { enabled_ = e; }          // false: no answers at all (settings: LAN connection off)

private:
    void udpLoop();
    void tcpLoop();
    void handle(SOCKET s, std::string ip);
    bool connectFlow(SOCKET s, const Bytes& hello, const Bytes& padId, const std::string& padName, const std::string& ip);   // true = the socket moved to the queue
    void pairFlow(SOCKET s, const Bytes& hello, const Bytes& padId, const std::string& padName, const std::string& ip);

    Devices* dev_ = nullptr;
    SOCKET tcp_ = INVALID_SOCKET, udp_ = INVALID_SOCKET;
    uint16_t tcpPort_ = 0;
    std::atomic<bool> run_{false}, busy_{false}, enabled_{true};
    std::thread tcpTh_, udpTh_;
    std::atomic<int> active_{0};

    std::mutex mu_;                                    // queue_
    std::deque<AcceptedLink> queue_;

    std::mutex em_;                                    // expected_
    Bytes expected_;

    std::mutex pm_;                                    // pairing window
    bool pairOpen_ = false;
    std::string code_;
    int64_t pairExpiresNs_ = 0;
    int attempts_ = 0;
};

}  // namespace ph
