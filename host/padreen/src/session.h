#pragma once
#include "capture.h"
#include "common.h"
#include "convert.h"
#include "net.h"

namespace ph {

struct SessionStats {
    std::atomic<uint64_t> updates{0}, bytes{0}, settles{0}, big{0}, full{0};     // big = updates over half of the panel, full = over 90%
    std::atomic<double> procMs{0};       // frame acquired -> update on the wire (EMA)
    std::atomic<double> rttMs{0};
};

struct SessionConfig {
    std::wstring gdiName;        // output to duplicate (the virtual monitor)
    std::function<std::wstring()> resolveGdi;   // current GDI name of the virtual monitor (it can be renamed when the display topology changes, e.g. lid closed)
    bool bw = true;              // true: 1-bit DU fast updates + gray settle; false: gray8 DU4 updates (+ settle)
    int settleMs = 1200;         // idle time before the gray (GL16) settle pass (level 2)
    int settleLevel = 0;         // automatic gray settle: 0 off (default: gray only on a manual full refresh), 1 light, 2 full
    bool cursor = true;
    int fastMode = 1;            // protocol mode of the fast 1-bit updates (1 = DU, 5..9 = A2 variants, see net.h)
    int fullRefreshMinutes = 0;  // 0 = never (manual only)
    bool debug = false;          // extra per-frame diagnostics in the log
    bool autoClean = false;      // one full refresh after heavy 1-bit drawing + 8 s of calm
    bool quickFirst = false;     // a session that continues after a short link loss: the picture is settled, send the first frame soon
    bool autoFull = false;       // allow whole-panel gray refreshes on its own (after big redraws / re-sync); default: only on request
};

// The whole PC-side pipeline for one tablet: Desktop Duplication -> gray -> 1-bit -> tile diff -> wire.
class Session {
public:
    Session(Link& link, SessionConfig cfg) : link_(link), cfg_(std::move(cfg)) {}
    // Blocks until stop is set or the link / capture fails.  Returns a short reason.
    std::string run(const std::atomic<bool>& stop);

    void requestFullRefresh() { fullRefresh_ = true; }
    // The tablet was turned (T_ORIENT): run() returns "geometry change" at its next iteration (within ~0.1 s); the caller then re-shapes the virtual
    // monitor and continues with a NEW Session (all pictures have the old shape).  Thread safe.
    void requestGeometryChange() { geomChange_ = true; }
    void setBw(bool bw) { bwRequested_ = bw ? 1 : 0; }
    void setFastMode(int m) { fastMode_ = m; }
    void setSettleLevel(int l) { settleLevel_ = l; }
    // The next run() continues a session whose link was lost and re-established while the tablet kept showing the old picture: it only sends what
    // changed in between (no whole-panel refresh).  Only valid if the tablet says so (CAP_RESUME).
    void resumeAfterReconnect() {
        resume_ = true;
        quickNext_ = true;
    }
    SessionStats stats;

private:
    bool sendGray(uint8_t mode, const RECT& r, const std::vector<uint8_t>& gray, int W);
    bool sendMono(const RECT& r, const std::vector<uint8_t>& bw, int W);

    Link& link_;
    SessionConfig cfg_;
    void countSize(const RECT& r, int W, int H);              // statistics: updates over half / nearly all of the panel
    std::atomic<bool> fullRefresh_{false};
    std::atomic<bool> geomChange_{false};
    std::atomic<int> bwRequested_{-1};
    std::atomic<int> fastMode_{1};
    std::atomic<int> settleLevel_{0};
    std::vector<uint8_t> tmp_, comp_;
    // the pictures the pipeline works with; kept between run() calls (see resumeAfterReconnect)
    struct Screen {
        std::vector<uint8_t> clean, gray, bw, prevBw, prevGray, settleBuf, dirtyMap;
        int W = 0, H = 0;
        bool valid = false;
    } screen_;
    std::atomic<bool> resume_{false}, quickNext_{false};
};

}  // namespace ph
