#include "input.h"

#include <algorithm>

namespace ph {

bool g_traceTouch = false;   // set by --debug-touch

void injectGestureShortcut(int kind, int downMode) {
    struct K {
        WORD vk;
        bool down;
    };
    std::vector<K> seq;
    if (kind == 1) {
        seq = {{VK_LWIN, true}, {VK_TAB, true}, {VK_TAB, false}, {VK_LWIN, false}};
    } else if (kind == 2) {
        if (downMode == 1) seq = {{VK_LMENU, true}, {VK_TAB, true}, {VK_TAB, false}, {VK_LMENU, false}};
        else seq = {{VK_LCONTROL, true}, {VK_LMENU, true}, {VK_TAB, true}, {VK_TAB, false}, {VK_LMENU, false}, {VK_LCONTROL, false}};
    } else {
        return;
    }
    std::vector<INPUT> in(seq.size());
    for (size_t i = 0; i < seq.size(); ++i) {
        ZeroMemory(&in[i], sizeof(INPUT));
        in[i].type = INPUT_KEYBOARD;
        in[i].ki.wVk = seq[i].vk;
        in[i].ki.wScan = (WORD)MapVirtualKeyW(seq[i].vk, MAPVK_VK_TO_VSC);
        in[i].ki.dwFlags = (seq[i].down ? 0 : KEYEVENTF_KEYUP) | (seq[i].vk == VK_LWIN ? KEYEVENTF_EXTENDEDKEY : 0);
    }
    UINT sent = SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
    logf("gesture: swipe %s -> %s (%u/%zu key events sent)", kind == 1 ? "up" : "down", kind == 1 ? "Win+Tab" : downMode == 1 ? "Alt+Tab" : "Ctrl+Alt+Tab", sent, in.size());
}

void injectMediaKey(int kind) {
    WORD vk = kind == 1 ? VK_VOLUME_UP : kind == 2 ? VK_VOLUME_DOWN : kind == 3 ? VK_VOLUME_MUTE : 0;
    if (!vk) return;
    INPUT in[2];
    ZeroMemory(in, sizeof(in));
    for (int i = 0; i < 2; ++i) {
        in[i].type = INPUT_KEYBOARD;
        in[i].ki.wVk = vk;
        in[i].ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        in[i].ki.dwFlags = KEYEVENTF_EXTENDEDKEY | (i ? KEYEVENTF_KEYUP : 0);
    }
    UINT sent = SendInput(2, in, sizeof(INPUT));
    static int64_t lastLogNs = 0;                      // holding a volume key repeats ten steps a second: log at most about one line a second
    int64_t now = nowNs();
    if (sent != 2 || now - lastLogNs > 1000000000LL) {
        lastLogNs = now;
        logf("media key: volume %s (%u/2 key events sent)", kind == 1 ? "up" : kind == 2 ? "down" : "mute", sent);
    }
}
bool TouchInjector::start(int panelW, int panelH, bool touchMode) {
    stop();
    panelW_ = panelW;
    panelH_ = panelH;
    touchMode_ = touchMode;
    {
        std::lock_guard<std::mutex> lk(mu_);
        q_.clear();
        for (auto& c : contacts_) c = Contact();
    }
    failures_ = 0;
    run_ = true;
    std::atomic<int> ready{0};          // 1 = ok, -1 = touch injection unavailable
    th_ = std::thread([this, &ready] {
        if (touchMode_) {
            // dwMode: no visual feedback ring for injected touches (this is per process, not a system setting)
            if (!InitializeTouchInjection(10, TOUCH_FEEDBACK_NONE)) {
                logf("touch: InitializeTouchInjection failed (%lu) - falling back to mouse input", GetLastError());
                touchMode_ = false;
            }
        }
        ready = 1;
        loop();
    });
    while (ready == 0) Sleep(1);
    logf("touch: input injection ready (%s)", touchMode_ ? "touch" : "mouse");
    return true;
}

void TouchInjector::setTarget(const RECT& r) {
    std::lock_guard<std::mutex> lk(mu_);
    target_ = r;
}

void TouchInjector::submit(const uint8_t* p, size_t n) {
    if (!run_ || n < 6) return;
    int count = p[5];
    if (count < 0 || count > 10 || n < 6 + (size_t)count * 16) return;
    std::vector<Pt> pts;
    for (int i = 0; i < count; ++i) {
        const uint8_t* o = p + 6 + i * 16;
        Pt q;
        q.id = o[0];
        q.state = o[1];
        q.tool = o[2];
        q.buttons = o[3];
        q.x = (uint16_t)(o[4] | (o[5] << 8));
        q.y = (uint16_t)(o[6] | (o[7] << 8));
        q.pressure = (uint16_t)(o[8] | (o[9] << 8));
        q.major = (uint16_t)(o[10] | (o[11] << 8));
        q.minor = (uint16_t)(o[12] | (o[13] << 8));
        pts.push_back(q);
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        q_.push_back(std::move(pts));
        if (q_.size() > 512) q_.pop_front();
    }
    cv_.notify_one();
}

void TouchInjector::stop() {
    if (!run_ && !th_.joinable()) return;
    run_ = false;
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void TouchInjector::toScreen(const Pt& p, LONG& sx, LONG& sy) {
    RECT t = target_;
    sx = t.left + (LONG)((int64_t)p.x * (t.right - t.left) / std::max(1, panelW_.load()));
    sy = t.top + (LONG)((int64_t)p.y * (t.bottom - t.top) / std::max(1, panelH_.load()));
}

void TouchInjector::releaseContacts() {
    releaseReq_ = true;
    cv_.notify_all();
}

void TouchInjector::loop() {
    void* boost = beginThreadBoost(L"Games");
    while (run_) {
        std::vector<std::vector<Pt>> frames;
        // a held finger must be re-reported every ~40 ms; with no finger down there is nothing to do but wait
        bool anyDown = false;
        for (const Contact& c : contacts_) anyDown = anyDown || c.active;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, std::chrono::milliseconds(anyDown ? 40 : 1000), [&] { return !q_.empty() || !run_ || releaseReq_; });
            if (!run_) break;
            // collapse runs of "everything just moved" frames: only the newest position matters
            while (!q_.empty()) {
                std::vector<Pt> f = std::move(q_.front());
                q_.pop_front();
                bool onlyMove = std::all_of(f.begin(), f.end(), [](const Pt& p) { return p.state == 0; });
                bool nextOnlyMove = !q_.empty() && std::all_of(q_.front().begin(), q_.front().end(), [](const Pt& p) { return p.state == 0; });
                if (onlyMove && nextOnlyMove) continue;
                frames.push_back(std::move(f));
            }
        }
        if (releaseReq_.exchange(false)) releaseAll();
        if (frames.empty()) {
            refresh();
            continue;
        }
        for (auto& f : frames) process(f);
    }
    releaseAll();
    endThreadBoost(boost);
}

void TouchInjector::process(const std::vector<Pt>& pts) {
    ++frames_;
    if (g_traceTouch) {
        std::string d;
        for (const Pt& p : pts) {
            char b[64];
            snprintf(b, sizeof(b), " [id %u st %u tool %u %u,%u p%u]", p.id, p.state, p.tool, p.x, p.y, p.pressure);
            d += b;
        }
        logf("touch frame:%s", d.c_str());
    }
    if (touchMode_) injectTouch(pts);
    else injectMouse(pts);
}

void TouchInjector::fillInfo(POINTER_TOUCH_INFO& t, const Pt& p, UINT32 flags) {
    memset(&t, 0, sizeof(t));
    t.pointerInfo.pointerType = PT_TOUCH;
    t.pointerInfo.pointerId = p.id;
    LONG sx, sy;
    toScreen(p, sx, sy);
    t.pointerInfo.ptPixelLocation.x = sx;
    t.pointerInfo.ptPixelLocation.y = sy;
    // contact area (fat finger): Windows uses it to pick the control that was meant
    LONG half = std::max<LONG>(4, (LONG)(std::max<int>(p.major, p.minor) * (target_.right - target_.left) / std::max(1, panelW_.load()) / 2));
    half = std::min<LONG>(half, 60);
    t.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_PRESSURE;
    t.rcContact = {sx - half, sy - half, sx + half, sy + half};
    t.pressure = std::clamp<UINT32>(p.pressure ? p.pressure : 512, 1, 1024);
    t.pointerInfo.pointerFlags = (POINTER_FLAGS)flags;
}

// Windows rejects inconsistent contact sequences (ERROR_INVALID_PARAMETER): start over with a clean slate.
void TouchInjector::resetContacts() {
    POINTER_TOUCH_INFO info[10];
    int n = 0;
    for (int i = 0; i < 10; ++i)
        if (contacts_[i].active) {
            fillInfo(info[n++], contacts_[i].pt, POINTER_FLAG_UP | POINTER_FLAG_CANCELED);
            contacts_[i].active = false;
        }
    if (n) InjectTouchInput((UINT32)n, info);
}

void TouchInjector::injectTouch(const std::vector<Pt>& pts) {
    POINTER_TOUCH_INFO info[10];
    int n = 0;
    for (const Pt& p : pts) {
        if (p.id >= 10 || n >= 10) continue;
        Contact& c = contacts_[p.id];
        if (p.state == 1 && c.active) {
            // a new finger with an id Windows still considers down (e.g. a lost "up"): lift the old one first
            POINTER_TOUCH_INFO up;
            fillInfo(up, c.pt, POINTER_FLAG_UP | POINTER_FLAG_CANCELED);
            InjectTouchInput(1, &up);
            c.active = false;
        }
        POINTER_TOUCH_INFO& t = info[n];
        if (p.state == 1 || (p.state == 0 && !c.active)) {
            fillInfo(t, p, POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT);
            c.active = true;
        } else if (p.state == 2) {
            if (!c.active) continue;
            Pt last = c.pt;                    // lift where the finger last was reported (a jump in the "up" frame can be rejected)
            last.id = p.id;
            fillInfo(t, last, POINTER_FLAG_UP);
            c.active = false;
            ++n;
            continue;
        } else if (p.state == 3) {
            if (!c.active) continue;
            fillInfo(t, p, POINTER_FLAG_UP | POINTER_FLAG_CANCELED);
            c.active = false;
        } else {
            fillInfo(t, p, POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT);
        }
        c.pt = p;
        ++n;
    }
    if (n && !InjectTouchInput((UINT32)n, info)) {
        DWORD ec = GetLastError();
        if (++failures_ <= 12) {
            std::string d;
            for (int i = 0; i < n; ++i) {
                char b[96];
                snprintf(b, sizeof(b), " [id %u flags 0x%x at %ld,%ld press %u rc %ldx%ld]", info[i].pointerInfo.pointerId, (unsigned)info[i].pointerInfo.pointerFlags,
                         info[i].pointerInfo.ptPixelLocation.x, info[i].pointerInfo.ptPixelLocation.y, info[i].pressure, info[i].rcContact.right - info[i].rcContact.left,
                         info[i].rcContact.bottom - info[i].rcContact.top);
                d += b;
            }
            logf("touch: InjectTouchInput failed (%lu) - resetting the contacts:%s", ec, d.c_str());
        }
        resetContacts();
    }
}

void TouchInjector::injectMouse(const std::vector<Pt>& pts) {
    // first finger only: move, and the left button while it touches
    const Pt* p = nullptr;
    for (const Pt& q : pts)
        if (q.id == 0 || !p) p = &q;
    if (!p) return;
    LONG sx, sy;
    toScreen(*p, sx, sy);
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    INPUT in[2] = {};
    int n = 0;
    in[n].type = INPUT_MOUSE;
    in[n].mi.dx = (LONG)((int64_t)(sx - vx) * 65535 / std::max(1, vw - 1));
    in[n].mi.dy = (LONG)((int64_t)(sy - vy) * 65535 / std::max(1, vh - 1));
    in[n].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    ++n;
    if (p->state == 1 && !mouseDown_) {
        in[n].type = INPUT_MOUSE;
        in[n].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        mouseDown_ = true;
        ++n;
    } else if ((p->state == 2 || p->state == 3) && mouseDown_) {
        in[n].type = INPUT_MOUSE;
        in[n].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        mouseDown_ = false;
        ++n;
    }
    SendInput((UINT)n, in, sizeof(INPUT));
}

// A held finger that does not move must still be reported, otherwise Windows drops the contact.
void TouchInjector::refresh() {
    if (!touchMode_) return;
    std::vector<Pt> pts;
    for (int i = 0; i < 10; ++i)
        if (contacts_[i].active) {
            Pt p = contacts_[i].pt;
            p.state = 0;
            pts.push_back(p);
        }
    if (!pts.empty()) injectTouch(pts);
}

void TouchInjector::releaseAll() {
    std::vector<Pt> pts;
    for (int i = 0; i < 10; ++i)
        if (contacts_[i].active) {
            Pt p = contacts_[i].pt;
            p.state = 3;
            pts.push_back(p);
        }
    if (touchMode_ && !pts.empty()) injectTouch(pts);
    if (!touchMode_ && mouseDown_) {
        INPUT in = {};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(1, &in, sizeof(INPUT));
        mouseDown_ = false;
    }
}

}  // namespace ph
