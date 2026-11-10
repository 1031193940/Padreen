#pragma once
#include "common.h"

#include <condition_variable>
#include <deque>

namespace ph {

// Touch feedback: finger / stylus events from the tablet are replayed on the PC at the matching position of the virtual monitor.
//  - touch mode (default): Windows touch injection (InitializeTouchInjection / InjectTouchInput) - real multi-touch, so taps,
//    press-and-hold (right click), drags, pans, pinch and scrollbars behave like on a touch screen, and no mouse cursor is moved.
//  - mouse mode (fallback): SendInput absolute mouse events from the first finger (left button while touching).
// Injection runs on its own thread; the network thread only queues frames.
extern bool g_traceTouch;   // log every touch frame (--debug-touch)

// The tablet's three-finger swipes become keyboard shortcuts on the PC: swipe up (kind 1) = Win+Tab (Task View, multiple desktops),
// swipe down (kind 2) = window switcher: downMode 0 = Ctrl+Alt+Tab (the switcher stays open, pick a window by touch), 1 = plain Alt+Tab
// (jump to the previous window).
void injectGestureShortcut(int kind, int downMode);

// The tablet's hardware volume keys (while it shows the desktop) become the Windows media keys: kind 1 = volume up, 2 = volume down, 3 = mute.
// The shell handles them exactly like the keys of a keyboard (volume change + the usual on-screen indicator).
void injectMediaKey(int kind);

class TouchInjector {
public:
    ~TouchInjector() { stop(); }
    bool start(int panelW, int panelH, bool touchMode);
    // The tablet was turned: its touch coordinates now span panelW x panelH pixels (call together with setTarget for the re-shaped monitor).
    void setPanel(int panelW, int panelH) {
        panelW_ = panelW;
        panelH_ = panelH;
    }
    // Where the tablet's panel currently is on the (physical pixel) desktop; call again when the monitor moves.
    void setTarget(const RECT& screenRect);
    // One T_TOUCH payload: u32 timeMs, u8 actionMasked, u8 count, count x 16 bytes (see Receiver.sendTouch).
    void submit(const uint8_t* payload, size_t n);
    void stop();
    void releaseContacts();     // lift every finger that is still down (the tablet vanished mid-touch); thread safe
    bool touchMode() const { return touchMode_; }
    uint64_t frames() const { return frames_; }

private:
    struct Pt {
        uint8_t id, state, tool, buttons;
        uint16_t x, y, pressure, major, minor;
    };
    struct Contact {
        bool active = false;
        Pt pt{};
    };
    void loop();
    void process(const std::vector<Pt>& pts);
    void injectTouch(const std::vector<Pt>& pts);
    void fillInfo(POINTER_TOUCH_INFO& t, const Pt& p, UINT32 flags);
    void resetContacts();
    void injectMouse(const std::vector<Pt>& pts);
    void refresh();            // keep held contacts alive
    void releaseAll();
    void toScreen(const Pt& p, LONG& sx, LONG& sy);

    std::thread th_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::vector<Pt>> q_;
    std::atomic<bool> run_{false};
    std::atomic<bool> releaseReq_{false};
    bool touchMode_ = true;
    std::atomic<int> panelW_{1872}, panelH_{1404};
    RECT target_{0, 0, 1872, 1404};
    Contact contacts_[10];
    bool mouseDown_ = false;
    std::atomic<uint64_t> frames_{0};
    int failures_ = 0;
};

}  // namespace ph
