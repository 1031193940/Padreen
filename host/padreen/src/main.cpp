// Padreen: tray application that turns a MatePad Paper into an external e-ink monitor.
//   tablet plugged in  ->  (install/start the tablet app) -> plug in the virtual monitor -> duplicate it -> stream to the tablet
//   tablet unplugged / quit -> the virtual monitor disappears, nothing else on the PC was changed.
#include "common.h"
#include "../res/resource.h"

#include <shellapi.h>

#include <algorithm>
#include <condition_variable>

#include "adb.h"
#include "devices.h"
#include "driver.h"
#include "input.h"
#include "lan.h"
#include "net.h"
#include "power.h"
#include <tlhelp32.h>
#include "session.h"
#include "ui.h"
#include "vdd.h"
#include "wallpaper.h"

using namespace ph;

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
enum : UINT { IDM_STATUS = 1000, IDM_FAST, IDM_GRAY, IDM_FULL, IDM_WALLPAPER, IDM_RECONNECT, IDM_LOG, IDM_QUIT, IDM_AUTOSTART, IDM_LAN, IDM_TOUCH, IDM_PAIR, IDM_DEVICES, IDM_FIREWALL, IDM_OPEN, IDM_USB, IDM_ROTATE, IDM_DRIVER };
// fast-mode waveform choices: IDM_WAVE_BASE + protocol mode (1 = DU, 5 = A2/MIX, 6..9 = A2 with 6..9 frames)
constexpr UINT IDM_WAVE_BASE = 1100;
// automatic gray settle: IDM_SETTLE_BASE + 0 (off) / 1 (light) / 2 (full)
constexpr UINT IDM_SETTLE_BASE = 1200;
struct SettleChoice {
    const wchar_t* key;
    const wchar_t* label;
};
const SettleChoice kSettle[] = {{L"off", L"关闭（默认：不自动局部刷新，灰阶只在整屏刷新时出现）"}, {L"light", L"少量（停手 3 秒后补 2 小块文字）"}, {L"full", L"完整（停手后逐块补满全屏灰阶）"}};
struct WaveChoice {
    int mode;
    const wchar_t* key;
    const wchar_t* label;
};
const WaveChoice kWaves[] = {{1, L"du", L"DU（稳妥，残影少）"}, {6, L"a2f6", L"A2 6 帧（默认，更跟手）"}, {7, L"a2f7", L"A2 7 帧"}, {8, L"a2f8", L"A2 8 帧"}, {9, L"a2f9", L"A2 9 帧"}, {5, L"a2", L"A2 / MIX"}};

struct Config {
    std::wstring adbPath;                     // resolved at start: ini "adb" > adb\adb.exe next to the program > the developer SDK; empty = no USB support
    std::wstring apkPath;
    std::string deviceModel = "HMW-W09";   // empty = any adb device
    bool bw = true;
    int settleMs = 1200;
    int settleLevel = 0;                      // 0 off (default), 1 light, 2 full: automatic gray settle = partial GL16 refreshes
    bool cursor = true;
    int fullRefreshMinutes = 0;
    bool autoClean = false;
    bool autoFull = false;                    // whole-panel gray refreshes on their own (after big redraws / re-sync); default off: only on request
    int fastMode = 6;                         // protocol mode of the fast 1-bit updates (see kWaves); 6 = A2 6 frames (the user's pick, 2026-11-03)
    bool plainWallpaper = true;               // plain white wallpaper on the tablet's virtual monitor only (restored afterwards)
    bool gestures = true;                     // the tablet's three-finger swipes become keyboard shortcuts (up = Win+Tab, down = window switcher)
    int gestureDown = 0;                      // swipe down: 0 = Ctrl+Alt+Tab (the switcher stays open for a touch), 1 = plain Alt+Tab (previous window)
    bool rotate = true;                       // the tablet turned to portrait / landscape: the virtual monitor changes shape with it (1404x1872 <-> 1872x1404)
    bool lidKeepAwake = true;                 // while a session runs, closing the lid must not put the PC to sleep (needs the power helper service)
    bool touch = true;                        // finger / stylus input on the tablet is replayed on the PC
    bool touchInject = true;                  // true = Windows touch injection (multi-touch); false = mouse emulation (first finger)
    bool usb = true;                          // use a USB-connected tablet (0 = ignore the cable, wireless only: for testing / preference)
    bool lan = true;                          // let paired tablets connect over the local network (PD2); needs a firewall rule for TCP/UDP below
    int lanPort = 38702;                      // TCP: the tablet connects here
    int lanDiscoveryPort = 38703;             // UDP: the tablet looks for PCs here
    int pairMinutes = 5;
    int lanGraceSeconds = 45;                 // a LAN session whose link broke keeps the virtual monitor this long, waiting for the tablet to reconnect                      // how long a pairing window (code) stays open
    int port = 38700;
    int requiredAppBuild = 16;
    int modeHz = 60;

    void load(const std::wstring& ini) {
        wchar_t b[1024];
        auto str = [&](const wchar_t* key, const std::wstring& def) {
            GetPrivateProfileStringW(L"padreen", key, def.c_str(), b, 1024, ini.c_str());
            return std::wstring(b);
        };
        auto num = [&](const wchar_t* key, int def) { return (int)GetPrivateProfileIntW(L"padreen", key, def, ini.c_str()); };
        std::wstring explicitAdb = str(L"adb", L"");
        if (!explicitAdb.empty()) adbPath = explicitAdb;
        apkPath = str(L"apk", apkPath);
        deviceModel = narrow(str(L"model", widen(deviceModel)));
        bw = num(L"fast_bw", bw ? 1 : 0) != 0;
        settleMs = num(L"settle_ms", settleMs);
        {
            std::wstring sl = str(L"settle", L"off");
            settleLevel = _wcsicmp(sl.c_str(), L"full") == 0 ? 2 : _wcsicmp(sl.c_str(), L"light") == 0 ? 1 : 0;
        }
        cursor = num(L"cursor", cursor ? 1 : 0) != 0;
        fullRefreshMinutes = num(L"full_refresh_minutes", fullRefreshMinutes);
        autoClean = num(L"auto_clean", autoClean ? 1 : 0) != 0;
        autoFull = num(L"auto_full", autoFull ? 1 : 0) != 0;
        plainWallpaper = num(L"plain_wallpaper", plainWallpaper ? 1 : 0) != 0;
        std::wstring fw = str(L"fast_wave", L"a2f6");
        fastMode = 6;
        for (const WaveChoice& c : kWaves)
            if (_wcsicmp(fw.c_str(), c.key) == 0) fastMode = c.mode;
        port = num(L"port", port);
        usb = num(L"usb", usb ? 1 : 0) != 0;
        touch = num(L"touch", touch ? 1 : 0) != 0;
        lidKeepAwake = num(L"lid_keep_awake", lidKeepAwake ? 1 : 0) != 0;
        gestures = num(L"gestures", gestures ? 1 : 0) != 0;
        gestureDown = _wcsicmp(str(L"gesture_down", L"switcher").c_str(), L"alttab") == 0 ? 1 : 0;
        rotate = num(L"rotate", rotate ? 1 : 0) != 0;
        touchInject = _wcsicmp(str(L"touch_mode", L"touch").c_str(), L"mouse") != 0;
        lan = num(L"lan", lan ? 1 : 0) != 0;
        lanPort = num(L"lan_port", lanPort);
        lanDiscoveryPort = num(L"lan_discovery_port", lanDiscoveryPort);
        pairMinutes = std::max(1, num(L"pair_minutes", pairMinutes));
        lanGraceSeconds = std::max(0, num(L"lan_grace_seconds", lanGraceSeconds));
        modeHz = num(L"mode_hz", modeHz);
    }
};

struct App {
    Config cfg;
    std::unique_ptr<Adb> adb;
    HWND hwnd = nullptr;
    NOTIFYICONDATAW nid = {};
    HICON icons[4] = {};
    std::wstring logPath, iniPath;
    std::atomic<bool> supervisorDone{false};

    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::string> devices;
    std::vector<AdbPending> pendingDevices;   // plugged in, but adb cannot use them yet (e.g. the tablet still waits for "Allow USB debugging"); guarded by mu
    std::atomic<bool> quit{false}, reconnect{false};
    std::wstring status = L"启动中…";
    std::atomic<int> color{0};                // 0 grey, 1 orange, 2 green, 3 red
    Session* session = nullptr;               // valid while running (guarded by mu)
    std::atomic<uint64_t> lastUpdates{0};
    int64_t lastStatsNs = 0;
    double fps = 0;
    std::thread supervisor;
    DeviceWatcher watcher;

    void setStatus(const std::wstring& s, int c) {
        bool changed;
        {
            std::lock_guard<std::mutex> lk(mu);
            changed = status != s;
            status = s;
        }
        color = c;
        if (changed) logf("status: %s", narrow(s).c_str());          // the supervisor repeats the same status while it waits
        if (hwnd) PostMessageW(hwnd, WM_TIMER, 1, 0);
        uiRefresh();
    }
    bool deviceStillThere(const std::string& serial) {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& d : devices)
            if (d == serial) return true;
        return false;
    }
    bool prepareTablet(const std::string& serial, Link& link, std::wstring& err);
    bool createVirtualDisplay(Vdd& vdd, int w, int h, std::wstring& gdi, std::wstring& err);
    void superviseLoop();
    // LAN: this PC's identity + the paired tablets, the listener that tablets connect to, pairing over USB
    Devices store;                            // this PC's identity + the paired tablets
    UiHost uiHost;                            // what the control / pairing windows need from the app
    LanServer lan;
    void pairAndRemember(const std::string& serial, Link& link);
    std::string runSession(Link& link, const std::string& serial, std::wstring& err, bool& vdFailed);
    void startLan();
    bool firewallReady();
    bool askFirewall(bool manual);
    bool lanBlocked = false;                  // the firewall rule is missing and the user has not (yet) allowed it
    bool manualFirewall = false;
    void offerDriver(bool atStart);           // the virtual display driver (Parsec's, downloaded from Parsec on request): ask, install (one UAC prompt), let the tablet retry
    bool driverBusy = false;                  // offerDriver is open (its message boxes run a nested message loop: a second tray click must not start a second installer)
    std::atomic<bool> inSession{false};       // runSession is running: the virtual monitor exists (also while a Wi-Fi session waits for its tablet)
    std::atomic<bool> driverRetry{false};     // the driver was just installed: the supervisor forgets that a tablet was refused for lack of it
};

App* g = nullptr;
bool g_debug = false;
std::wstring dataDir();
void applyFlag(UiFlag f, bool v);

// Diagnostics for a hang at process exit: which threads (and where did they start) are still alive when wWinMain is done?
void logRemainingThreads() {
    DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    typedef LONG(WINAPI * NtQIT)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    NtQIT query = (NtQIT)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    THREADENTRY32 te = {sizeof(te)};
    int n = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        ++n;
        void* start = nullptr;
        ULONG rl = 0;
        if (HANDLE th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID)) {
            if (query) query(th, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof(start), &rl);
            CloseHandle(th);
        }
        wchar_t mod[MAX_PATH] = L"?";
        HMODULE hm = nullptr;
        if (start && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)start, &hm)) GetModuleFileNameW(hm, mod, MAX_PATH);
        logf("exit: thread %lu still alive, started at %p in %s +0x%llx", (unsigned long)te.th32ThreadID, start, narrow(mod).c_str(), (unsigned long long)((char*)start - (char*)hm));
    }
    CloseHandle(snap);
    logf("exit: %d other thread(s) alive", n);
}

HICON loadIcon(int id, int size) {
    return (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(id), IMAGE_ICON, size, size, LR_DEFAULTCOLOR);
}

void updateTray(bool balloon = false) {
    if (!g || !g->hwnd) return;
    std::wstring s;
    {
        std::lock_guard<std::mutex> lk(g->mu);
        s = g->status;
        if (g->session) {
            int64_t now = nowNs();
            uint64_t u = g->session->stats.updates.load();
            if (g->lastStatsNs && now - g->lastStatsNs > 500000000LL) {
                g->fps = (double)(u - g->lastUpdates) * 1e9 / (double)(now - g->lastStatsNs);
                g->lastUpdates = u;
                g->lastStatsNs = now;
            } else if (!g->lastStatsNs) {
                g->lastUpdates = u;
                g->lastStatsNs = now;
            }
            wchar_t extra[96];
            swprintf(extra, 96, L" · %.0f 次/秒 · 处理 %.0f ms · 往返 %.0f ms", g->fps, g->session->stats.procMs.load(), g->session->stats.rttMs.load());
            s += extra;
        } else {
            g->lastStatsNs = 0;
        }
    }
    g->nid.uFlags = NIF_TIP | NIF_ICON;
    g->nid.hIcon = g->icons[std::clamp(g->color.load(), 0, 3)];
    wcsncpy_s(g->nid.szTip, L"Padreen — ", _TRUNCATE);
    wcsncat_s(g->nid.szTip, s.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &g->nid);
    (void)balloon;
}

const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool autostartEnabled() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return false;
    bool ok = RegQueryValueExW(k, L"Padreen", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

// Per-user "start with Windows" entry owned by this program; only touched when the user ticks the menu item.
void setAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t path[MAX_PATH * 2];
        GetModuleFileNameW(nullptr, path, (DWORD)(sizeof(path) / sizeof(path[0])));
        std::wstring v = std::wstring(L"\"") + path + L"\"";
        RegSetValueExW(k, L"Padreen", 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"Padreen");
    }
    RegCloseKey(k);
}

void saveSettings() {
    if (!g || g->iniPath.empty()) return;
    WritePrivateProfileStringW(L"padreen", L"fast_bw", g->cfg.bw ? L"1" : L"0", g->iniPath.c_str());
    WritePrivateProfileStringW(L"padreen", L"plain_wallpaper", g->cfg.plainWallpaper ? L"1" : L"0", g->iniPath.c_str());
    WritePrivateProfileStringW(L"padreen", L"lan", g->cfg.lan ? L"1" : L"0", g->iniPath.c_str());
    WritePrivateProfileStringW(L"padreen", L"touch", g->cfg.touch ? L"1" : L"0", g->iniPath.c_str());
    WritePrivateProfileStringW(L"padreen", L"gestures", g->cfg.gestures ? L"1" : L"0", g->iniPath.c_str());
    WritePrivateProfileStringW(L"padreen", L"rotate", g->cfg.rotate ? L"1" : L"0", g->iniPath.c_str());
    WritePrivateProfileStringW(L"padreen", L"lid_keep_awake", g->cfg.lidKeepAwake ? L"1" : L"0", g->iniPath.c_str());
    WritePrivateProfileStringW(L"padreen", L"usb", g->cfg.usb ? L"1" : L"0", g->iniPath.c_str());
    WritePrivateProfileStringW(L"padreen", L"settle", kSettle[std::min(2, std::max(0, g->cfg.settleLevel))].key, g->iniPath.c_str());
    for (const WaveChoice& c : kWaves)
        if (c.mode == g->cfg.fastMode) WritePrivateProfileStringW(L"padreen", L"fast_wave", c.key, g->iniPath.c_str());
}

void showMenu(HWND h) {
    HMENU m = CreatePopupMenu();
    std::wstring st;
    bool bw;
    {
        std::lock_guard<std::mutex> lk(g->mu);
        st = g->status;
    }
    bw = g->cfg.bw;
    AppendMenuW(m, MF_STRING | MF_GRAYED, IDM_STATUS, st.c_str());
    AppendMenuW(m, MF_STRING, IDM_OPEN, L"打开 Padreen 主界面");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (bw ? MF_CHECKED : 0), IDM_FAST, L"极速模式（纯黑白，约 0.1 秒一次）");
    AppendMenuW(m, MF_STRING | (!bw ? MF_CHECKED : 0), IDM_GRAY, L"画质模式（4 级灰度，约 0.6 秒一次）");
    HMENU waves = CreatePopupMenu();
    for (const WaveChoice& c : kWaves) AppendMenuW(waves, MF_STRING | (g->cfg.fastMode == c.mode ? MF_CHECKED : 0), IDM_WAVE_BASE + c.mode, c.label);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)waves, L"极速模式的刷新方式");
    HMENU settle = CreatePopupMenu();
    for (int i = 0; i < 3; ++i) AppendMenuW(settle, MF_STRING | (g->cfg.settleLevel == i ? MF_CHECKED : 0), IDM_SETTLE_BASE + i, kSettle[i].label);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)settle, L"自动补灰阶（会局部刷新）");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_FULL, L"立即全屏刷新（清除残影）");
    AppendMenuW(m, MF_STRING | (g->cfg.plainWallpaper ? MF_CHECKED : 0), IDM_WALLPAPER, L"平板屏使用纯白壁纸（下次连接生效）");
    AppendMenuW(m, MF_STRING | (g->cfg.touch ? MF_CHECKED : 0), IDM_TOUCH, L"触控输入（在平板上用手指点按、拖动）");
    AppendMenuW(m, MF_STRING | (g->cfg.rotate ? MF_CHECKED : 0), IDM_ROTATE, L"自动旋转（平板横屏 / 竖屏切换时，电脑里的平板屏同步旋转）");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_PAIR, L"配对新平板…（显示配对码）");
    AppendMenuW(m, MF_STRING, IDM_DEVICES, L"已配对的平板…");
    AppendMenuW(m, MF_STRING | (g->cfg.lan ? MF_CHECKED : 0), IDM_LAN, L"允许平板通过局域网连接");
    AppendMenuW(m, MF_STRING | (g->cfg.usb ? MF_CHECKED : 0), IDM_USB, L"插着 USB 线时用 USB 连接（取消 = 始终走 Wi-Fi）");
    if (g->lanBlocked) AppendMenuW(m, MF_STRING, IDM_FIREWALL, L"启用局域网连接（需要允许防火墙）…");
    AppendMenuW(m, MF_STRING, IDM_DRIVER, Vdd::installed() ? L"重新安装虚拟显示器驱动（修复）…" : L"安装虚拟显示器驱动（从 Parsec 官方下载，需要管理员授权）…");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (autostartEnabled() ? MF_CHECKED : 0), IDM_AUTOSTART, L"开机自动启动（只为当前用户）");
    AppendMenuW(m, MF_STRING, IDM_RECONNECT, L"重新连接");
    AppendMenuW(m, MF_STRING, IDM_LOG, L"打开日志");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_QUIT, L"退出");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(h);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, p.x, p.y, 0, h, nullptr);
    PostMessageW(h, WM_NULL, 0, 0);
    DestroyMenu(m);
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_TRAY:
            if (LOWORD(l) == WM_RBUTTONUP) showMenu(h);
            else if (LOWORD(l) == WM_LBUTTONUP) uiShowMain(&g->uiHost);
            return 0;
        case WM_TIMER:
            updateTray();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(w)) {
                case IDM_FAST:
                case IDM_GRAY: {
                    bool bw = LOWORD(w) == IDM_FAST;
                    g->cfg.bw = bw;
                    saveSettings();
                    std::lock_guard<std::mutex> lk(g->mu);
                    if (g->session) g->session->setBw(bw);
                    break;
                }
                case IDM_WAVE_BASE + 1:
                case IDM_WAVE_BASE + 5:
                case IDM_WAVE_BASE + 6:
                case IDM_WAVE_BASE + 7:
                case IDM_WAVE_BASE + 8:
                case IDM_WAVE_BASE + 9: {
                    g->cfg.fastMode = (int)(LOWORD(w) - IDM_WAVE_BASE);
                    saveSettings();
                    std::lock_guard<std::mutex> lk(g->mu);
                    if (g->session) g->session->setFastMode(g->cfg.fastMode);
                    break;
                }
                case IDM_FULL: {
                    std::lock_guard<std::mutex> lk(g->mu);
                    if (g->session) g->session->requestFullRefresh();
                    break;
                }
                case IDM_WALLPAPER:
                    g->cfg.plainWallpaper = !g->cfg.plainWallpaper;
                    saveSettings();
                    break;
                case IDM_LAN:
                    g->cfg.lan = !g->cfg.lan;
                    saveSettings();
                    g->lan.setEnabled(g->cfg.lan);
                    if (g->cfg.lan && !g->lan.running()) g->startLan();
                    break;
                case IDM_USB:
                    applyFlag(UF_USB, !g->cfg.usb);
                    break;
                case IDM_ROTATE:
                    applyFlag(UF_ROTATE, !g->cfg.rotate);
                    break;
                case IDM_FIREWALL:
                    g->manualFirewall = true;
                    g->startLan();
                    g->manualFirewall = false;
                    break;
                case IDM_DRIVER:
                    g->offerDriver(false);
                    break;
                case IDM_OPEN:
                    uiShowMain(&g->uiHost);
                    break;
                case IDM_PAIR:
                    uiShowPair(&g->uiHost);
                    break;
                case IDM_DEVICES:
                    uiShowDevices(&g->uiHost);
                    break;                case IDM_SETTLE_BASE + 0:
                case IDM_SETTLE_BASE + 1:
                case IDM_SETTLE_BASE + 2: {
                    g->cfg.settleLevel = (int)(LOWORD(w) - IDM_SETTLE_BASE);
                    saveSettings();
                    std::lock_guard<std::mutex> lk(g->mu);
                    if (g->session) g->session->setSettleLevel(g->cfg.settleLevel);
                    break;
                }
                case IDM_AUTOSTART:
                    setAutostart(!autostartEnabled());
                    break;
                case IDM_RECONNECT:
                    g->reconnect = true;
                    g->cv.notify_all();
                    break;
                case IDM_LOG:
                    ShellExecuteW(nullptr, L"open", L"notepad.exe", g->logPath.c_str(), nullptr, SW_SHOW);
                    break;
                case IDM_QUIT:
                    g->quit = true;
                    g->cv.notify_all();
                    DestroyWindow(h);
                    break;
            }
            return 0;
        case WM_POWERBROADCAST:
            if (w == PBT_POWERSETTINGCHANGE && l) {
                auto* ps = reinterpret_cast<POWERBROADCAST_SETTING*>(l);
                static const GUID kLidSwitch = {0xBA3E0F4D, 0xB817, 0x4094, {0xA8, 0x5D, 0xD4, 0xC9, 0xBB, 0x70, 0xC9, 0xD2}};
                if (IsEqualGUID(ps->PowerSetting, kLidSwitch) && ps->DataLength >= 4) logf("lid: %s", *(DWORD*)ps->Data ? "opened" : "closed");
            }
            return TRUE;
        case WM_DISPLAYCHANGE:
            logf("display: the monitor layout changed (%ux%u)", (unsigned)LOWORD(l), (unsigned)HIWORD(l));
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (w) {            // logging off / shutting down: give the supervisor a few seconds to restore the wallpaper and unplug the monitor
                g->quit = true;
                g->cv.notify_all();
                for (int i = 0; i < 40 && !g->supervisorDone; ++i) Sleep(100);
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

// ------------------------------------------------------------------------------------------------ supervisor
bool App::prepareTablet(const std::string& serial, Link& link, std::wstring& err) {
    const std::string pkg = "com.padreen.app", comp = "com.padreen.app/.MainActivity";
    std::string log;
    // puts the bundled APK on the tablet; false (with the reason in `err`) when that does not work
    auto installApk = [&](const wchar_t* what) {
        if (GetFileAttributesW(cfg.apkPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            err = L"找不到平板 App 的安装包 padreen.apk（要和 padreen.exe 放在同一个文件夹）";
            return false;
        }
        if (adb->install(serial, cfg.apkPath, log)) return true;
        logf("tablet: apk install failed: %s", log.c_str());
        err = quit ? L"Padreen 正在退出" : std::wstring(what) + explainInstallFailure(log);
        return false;
    };
    if (!adb->packageInstalled(serial, pkg)) {
        setStatus(L"正在安装到平板：请在平板上点「继续安装」（系统安全确认，只需这一次）", 1);
        if (!installApk(L"安装平板 App 失败：")) return false;
        logf("tablet: app installed");
    }
    // the tablet app was called PaperDisplay (package com.paperdisplay.app) before it became Padreen: keep a single app on the tablet
    static const char* const kLegacyPkg = "com.paperdisplay.app";
    if (adb->packageInstalled(serial, kLegacyPkg)) {
        setStatus(L"正在移除平板上旧版的「PaperDisplay」App…", 1);
        std::string ulog;
        bool removed = adb->uninstall(serial, kLegacyPkg, ulog);
        logf("tablet: the old PaperDisplay app %s (%s)", removed ? "was removed" : "could not be removed", ulog.c_str());
    }
    adb->forward(serial, cfg.port);
    for (int attempt = 0; attempt < 2 && !quit; ++attempt) {
        adb->startActivity(serial, comp);
        setStatus(L"正在连接平板 App…", 1);
        int64_t deadline = nowNs() + 12LL * 1000000000LL;
        std::string e;
        bool ok = false;
        while (nowNs() < deadline && !quit && deviceStillThere(serial)) {
            if (link.connect("127.0.0.1", (uint16_t)cfg.port, 1500, e)) {
                ok = true;
                break;
            }
            Sleep(300);
        }
        if (!ok) {
            err = L"连不上平板 App：" + widen(e);
            return false;
        }
        if (link.appBuild() >= cfg.requiredAppBuild) {
            pairAndRemember(serial, link);
            return true;
        }
        logf("tablet app build %d < required %d: updating", link.appBuild(), cfg.requiredAppBuild);
        link.close();
        setStatus(L"正在更新平板上的 App：请在平板上点「继续安装」（系统安全确认）", 1);
        if (!installApk(L"更新平板 App 失败：")) return false;
        adb->forceStop(serial, pkg);
        adb->forward(serial, cfg.port);
    }
    err = L"平板 App 版本过旧";
    return false;
}

bool App::createVirtualDisplay(Vdd& vdd, int w, int h, std::wstring& gdi, std::wstring& err) {
    if (!vdd.open()) {
        err = L"没有安装虚拟显示器驱动（托盘右键 →「安装虚拟显示器驱动」，只需管理员授权一次）";
        return false;
    }
    int idx = vdd.addDisplay();
    if (idx < 0) {
        err = L"虚拟显示器创建失败";
        return false;
    }
    gdi.clear();
    for (int i = 0; i < 100 && gdi.empty(); ++i) {
        if (!findVirtualDisplay(gdi)) Sleep(100);
    }
    if (gdi.empty()) {
        err = L"Windows 没有识别到虚拟显示器";
        return false;
    }
    if (!setDisplayMode(gdi, w, h, cfg.modeHz)) logf("setDisplayMode(%dx%d@%d) failed", w, h, cfg.modeHz);
    RECT rc{};
    for (int i = 0; i < 30; ++i) {
        if (displayRect(gdi, rc) && rc.right - rc.left == w && rc.bottom - rc.top == h) break;
        Sleep(100);
    }
    if (rc.right - rc.left != w || rc.bottom - rc.top != h) {
        err = L"虚拟显示器分辨率不是 " + std::to_wstring(w) + L"×" + std::to_wstring(h);
        return false;
    }
    logf("virtual monitor %s %dx%d at (%ld,%ld)", narrow(gdi).c_str(), w, h, rc.left, rc.top);
    return true;
}

// Over the USB connection (physical trust): make sure the tablet and this PC trust each other, so the tablet can later connect over the
// LAN without a code.  The tablet lists the key ids it holds in its HELLO; we only (re-)pair when ours is missing there.
void App::pairAndRemember(const std::string& serial, Link& link) {
    if (link.appBuild() < 9 || link.deviceId().size() != 16) return;
    PairedPad pad;
    bool have = store.find(link.deviceId(), pad);
    bool tabletHasIt = false;
    if (have) {
        Bytes h = sha256(pad.key);
        h.resize(4);
        for (const Bytes& k : link.padKeyIds()) tabletHasIt = tabletHasIt || k == h;
    }
    std::string ip = adb->wifiAddress(serial);
    if (have && tabletHasIt) {
        store.touch(link.deviceId(), ip);
        return;
    }
    Bytes k(32);
    randomBytes(k.data(), 32);
    setStatus(L"正在与平板配对（以后可通过局域网连接）…", 1);
    if (!link.pair(k, store.pcId(), store.pcName(), 3000)) {
        logf("lan: pairing over USB failed");
        return;
    }
    PairedPad np;
    np.id = link.deviceId();
    np.name = link.padName().empty() ? std::string("MatePad Paper") : link.padName();
    np.key = k;
    np.ip = ip;
    // the tablet app was reinstalled under a new identity (e.g. the PaperDisplay -> Padreen switch): drop the entry of its old identity (same model, same address)
    if (!ip.empty()) {
        for (const PairedPad& o : store.list()) {
            if (o.id != np.id && o.ip == ip && o.name == np.name) {
                store.remove(o.id);
                logf("lan: the earlier pairing of '%s' (%s) was replaced", o.name.c_str(), o.ip.c_str());
            }
        }
    }
    store.put(np);
    logf("lan: tablet '%s' paired over USB", np.name.c_str());
}

// ---------------------------------------------------------------------------------------------------------------- Windows Firewall
// The tablet connects to this PC (TCP 38702) and looks for it with UDP broadcasts (38703): inbound traffic needs a firewall rule.  The
// installer adds it; a portable copy asks once (UAC) - restricted to this program and the local subnet.
// The rule names contain the program path: several copies (installed, portable, development) can each have their own rules, and a rule that
// was made for another copy never makes this copy believe it is allowed.
static std::wstring exePath() {
    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, (DWORD)(sizeof(exe) / sizeof(exe[0])));
    return exe;
}
static std::wstring fwTcpName() { return L"Padreen LAN TCP (" + exePath() + L")"; }
static std::wstring fwUdpName() { return L"Padreen LAN UDP (" + exePath() + L")"; }

static bool fwRuleExists(const std::wstring& name) {
    std::wstring cmd = L"netsh.exe advfirewall firewall show rule name=\"" + name + L"\" dir=in";
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

bool App::firewallReady() { return fwRuleExists(fwTcpName()) && fwRuleExists(fwUdpName()); }

// Asks the user (once, or when they choose the tray item) and adds the two rules through an elevated netsh.
bool App::askFirewall(bool manual) {
    const std::wstring exe = exePath();
    std::wstring msg = L"要让平板在局域网里找到并连接这台电脑，Padreen 需要在 Windows 防火墙里添加两条入站规则：\n\n"
                       L"    TCP " + std::to_wstring(cfg.lanPort) + L"（平板连接）\n    UDP " + std::to_wstring(cfg.lanDiscoveryPort) +
                       L"（平板搜索电脑）\n\n只对本程序、只允许本地网络内的设备。需要管理员授权：点「是」后会弹出 Windows 的授权窗口。\n（只用 USB 线的话可以点「否」。）";
    if (MessageBoxW(hwnd, msg.c_str(), L"Padreen - 局域网连接", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND) != IDYES) return false;
    std::wstring tcp = L"netsh advfirewall firewall add rule name=\"" + fwTcpName() + L"\" dir=in action=allow protocol=TCP localport=" + std::to_wstring(cfg.lanPort) +
                       L" program=\"" + exe + L"\" profile=any remoteip=localsubnet enable=yes";
    std::wstring udp = L"netsh advfirewall firewall add rule name=\"" + fwUdpName() + L"\" dir=in action=allow protocol=UDP localport=" + std::to_wstring(cfg.lanDiscoveryPort) +
                       L" program=\"" + exe + L"\" profile=any remoteip=localsubnet enable=yes";
    std::wstring args = L"/c " + tcp + L" & " + udp;
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = hwnd;
    sei.lpVerb = L"runas";
    sei.lpFile = L"cmd.exe";
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        logf("firewall: elevation refused or failed (%lu)", GetLastError());
        return false;
    }
    WaitForSingleObject(sei.hProcess, 60000);
    CloseHandle(sei.hProcess);
    bool ok = firewallReady();
    logf("firewall: rules %s", ok ? "added" : "NOT added");
    (void)manual;
    return ok;
}

void App::startLan() {
    lan.setEnabled(cfg.lan);
    lan.onActivity = [this] {
        cv.notify_all();
        if (hwnd) PostMessageW(hwnd, WM_TIMER, 1, 0);
    };
    lan.onPaired = [this](const PairedPad& p) { setStatus(L"已与平板配对：" + widen(p.name), 2); };
    if (!cfg.lan || lan.running()) return;
    if (!firewallReady()) {
        wchar_t asked[8] = {};
        GetPrivateProfileStringW(L"padreen", L"firewall_asked", L"0", asked, 8, iniPath.c_str());
        if (asked[0] == L'1' && !manualFirewall) {
            lanBlocked = true;
            setStatus(L"局域网连接未启用：需要防火墙规则（托盘右键 →「启用局域网连接（防火墙）」）", 3);
            return;
        }
        WritePrivateProfileStringW(L"padreen", L"firewall_asked", L"1", iniPath.c_str());
        if (!askFirewall(false)) {
            lanBlocked = true;
            setStatus(L"局域网连接未启用：需要防火墙规则（托盘右键 →「启用局域网连接（防火墙）」）", 3);
            return;
        }
    }
    lanBlocked = false;
    std::string err;
    if (!lan.start(&store, (uint16_t)cfg.lanPort, (uint16_t)cfg.lanDiscoveryPort, err)) {
        logf("lan: %s", err.c_str());
        setStatus(L"局域网连接不可用：" + widen(err) + L"（端口被占用？）", 3);
    }
}
// The virtual display driver is Parsec's software and is not part of Padreen's download.  At the first start without it the user is asked once;
// "yes" downloads it from Parsec's own address, checks it, and installs it (one UAC prompt, no script).  A "no" is remembered (the tray menu
// keeps offering it); a cancelled or failed installation is asked again at the next start.  Any time later: tray menu (install, or repair when
// it is there already).  A tablet that was refused for lack of the driver tries again.
void App::offerDriver(bool atStart) {
    if (driverBusy) return;
    struct Busy {
        bool& b;
        explicit Busy(bool& x) : b(x) { b = true; }
        ~Busy() { b = false; }
    } busy(driverBusy);
    const wchar_t* title = L"Padreen - 虚拟显示器驱动";
    const bool repair = Vdd::installed();
    if (atStart) {
        if (repair) return;
        wchar_t declined[8] = {};
        GetPrivateProfileStringW(L"padreen", L"driver_declined", L"0", declined, 8, iniPath.c_str());
        if (declined[0] == L'1') {
            logf("driver: not installed, and the user declined it earlier (tray menu: install)");
            return;
        }
    } else if (repair && inSession) {
        MessageBoxW(hwnd, L"平板正在使用中。请先断开平板（或退出平板上的 Padreen），再重新安装驱动。", title, MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
        return;
    }
    const std::wstring cacheDir = iniPath.substr(0, iniPath.find_last_of(L"\\/")) + L"\\driver-cache";      // next to the settings and the log
    switch (installDriverInteractive(hwnd, logPath, repair, cacheDir, [this](const std::wstring& s) { setStatus(s, 1); }, &quit)) {
        case DriverAsk::Installed:
            WritePrivateProfileStringW(L"padreen", L"driver_declined", L"0", iniPath.c_str());
            setStatus(L"虚拟显示器驱动已安装", 0);
            driverRetry = true;
            cv.notify_all();
            break;
        case DriverAsk::Declined:
            if (atStart) WritePrivateProfileStringW(L"padreen", L"driver_declined", L"1", iniPath.c_str());
            break;
        case DriverAsk::Failed:
            break;
    }
}
// Virtual monitor + wallpaper + capture/stream session + teardown.  `serial` empty = Wi-Fi session.  Returns the end reason.
// A Wi-Fi session whose link breaks (not on purpose) keeps the virtual monitor for cfg.lanGraceSeconds and continues when the same tablet
// reconnects: Wi-Fi hiccups, the tablet app being restarted or the tablet sleeping do not end the session, and windows stay where they are.
std::string App::runSession(Link& link, const std::string& serial, std::wstring& err, bool& vdFailed) {
    vdFailed = false;
    struct BusyGuard {
        LanServer& l;
        explicit BusyGuard(LanServer& x) : l(x) { l.setBusy(true); }
        ~BusyGuard() { l.setBusy(false); }
    } busyGuard(lan);                                  // inbound LAN connections get "busy" while a session runs
    struct SessionFlag {
        std::atomic<bool>& f;
        explicit SessionFlag(std::atomic<bool>& x) : f(x) { f = true; }
        ~SessionFlag() { f = false; }
    } sessionFlag(inSession);                          // the driver cannot be reinstalled while the virtual monitor is in use
    // The desktop is created in the shape the tablet has right now (it can be turned to portrait).  Without "rotate" it is always landscape: a
    // tablet that is in portrait then gets T_GEOM "refused" and turns back by itself.
    int vw = link.panelWidth(), vh = link.panelHeight();
    if (!cfg.rotate && vh > vw) std::swap(vw, vh);
    Vdd vdd;
    std::wstring gdi;
    if (!createVirtualDisplay(vdd, vw, vh, gdi, err)) {
        logf("virtual display failed: %s", narrow(err).c_str());
        vdd.close();
        link.sendBye();
        link.close();
        vdFailed = true;
        return "virtual display failed";
    }
    link.setPanelSize(vw, vh);
    // keep the PC awake while the e-ink display is in use (application power request); the lid is handled by the power helper service
    SessionPower sessionPower(L"Padreen: 墨水屏正在作为第二块显示器使用");
    LidHold lidHold;
    if (cfg.lidKeepAwake) lidHold.start();
    // the virtual monitor can get another GDI name when the display topology changes (lid closed / opened): always ask for the current one
    auto curGdi = [gdi]() {
        std::wstring n;
        return findVirtualDisplay(n) ? n : gdi;
    };
    SessionConfig sc;
    sc.gdiName = gdi;
    sc.resolveGdi = curGdi;
    sc.bw = cfg.bw;
    sc.settleMs = cfg.settleMs;
    sc.cursor = cfg.cursor;
    sc.fullRefreshMinutes = cfg.fullRefreshMinutes;
    sc.autoClean = cfg.autoClean;
    sc.autoFull = cfg.autoFull;
    sc.settleLevel = cfg.settleLevel;
    sc.fastMode = cfg.fastMode;
    sc.debug = g_debug;
    std::unique_ptr<Session> sess = std::make_unique<Session>(link, sc);
    {
        std::lock_guard<std::mutex> lk(mu);
        this->session = sess.get();
    }
    // the tablet's one-key full refresh (three-finger tap) = the tray's "立即全屏刷新" (asks whichever session is current)
    link.setRefreshHandler([this]() {
        std::lock_guard<std::mutex> lk(mu);
        if (this->session) this->session->requestFullRefresh();
    });
    // the tablet's three-finger swipes: up = Win+Tab (multiple desktops), down = the window switcher
    link.setGestureHandler([this](int kind) {
        if (cfg.gestures) injectGestureShortcut(kind, cfg.gestureDown);
    });
    // the tablet's hardware volume keys (while it shows the desktop) = the PC's volume keys
    link.setMediaKeyHandler([](int key) { injectMediaKey(key); });
    // the tablet was turned: it asks for a desktop of its new shape (answered by runSession's loop below once the session has stopped)
    struct GeomWant {
        std::mutex m;
        bool want = false;
        int w = 0, h = 0, seq = 0;
    };
    auto geomWant = std::make_shared<GeomWant>();
    link.setOrientHandler([this, geomWant](int w, int h, int seq) {
        logf("orient: the tablet asks for a %dx%d desktop (#%d)", w, h, seq);
        {
            std::lock_guard<std::mutex> lk(geomWant->m);
            geomWant->want = true;
            geomWant->w = w;
            geomWant->h = h;
            geomWant->seq = seq;
        }
        std::lock_guard<std::mutex> lk(mu);
        if (this->session) this->session->requestGeometryChange();
    });
    // touch feedback: the tablet's finger events become touch (or mouse) input on the virtual monitor
    TouchInjector injector;
    bool touchOn = (link.caps() & CAP_TOUCH) != 0;
    if (touchOn) {
        RECT tr{};
        if (displayRect(curGdi(), tr)) injector.setTarget(tr);
        injector.start(link.panelWidth(), link.panelHeight(), cfg.touchInject);
        link.setTouchHandler([this, &injector](const uint8_t* p, size_t n) {
            if (cfg.touch) injector.submit(p, n);
        });
    }
    WallpaperGuard wallpaper;
    if (cfg.plainWallpaper) {
        RECT vr{};
        if (displayRect(gdi, vr)) wallpaper.apply(vr, exeDir() + L"\\padreen-white.png", dataDir() + L"\\wallpaper-original.txt");
    }
    setStatus(link.secure() ? L"已连接（无线）：平板是第 3 块显示器" : L"已连接：平板是第 3 块显示器", 2);
    std::atomic<bool> stop{false};
    bool onlyScreen = false;                      // guard thread only
    // current size and rectangle of the virtual monitor (false = Windows does not report it)
    auto monitorSize = [&](int& w, int& h, RECT& rc) {
        w = h = 0;
        if (!displayRect(curGdi(), rc)) return false;
        w = (int)(rc.right - rc.left);
        h = (int)(rc.bottom - rc.top);
        return true;
    };
    std::thread guard([&] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        int tick = 0;
        bool lastRotate = cfg.rotate;
        while (!stop && !quit) {
            if ((!serial.empty() && !deviceStillThere(serial)) || reconnect) stop = true;
            ++tick;
            if (cfg.rotate != lastRotate) {                                  // "auto rotate" was flipped in the control panel / the tray menu
                lastRotate = cfg.rotate;
                RECT rc{};
                int cw = 0, ch = 0;
                bool known = monitorSize(cw, ch, rc) && cw > 0 && ch > 0;
                logf("orient: automatic rotation switched %s", lastRotate ? "on" : "off");
                if (!lastRotate && known && ch > cw) {                       // a portrait desktop but rotation is off now: back to landscape
                    {
                        std::lock_guard<std::mutex> lk(geomWant->m);
                        geomWant->want = true;
                        geomWant->w = ch;
                        geomWant->h = cw;
                        geomWant->seq = 0;                                   // nobody asked: the announcement that follows tells the tablet
                    }
                    std::lock_guard<std::mutex> lk(mu);
                    if (this->session) this->session->requestGeometryChange();
                } else if (known) {
                    link.sendGeom(cw, ch, 0, false, !lastRotate);            // same shape, new policy: the tablet locks / unlocks its orientation
                }
            }
            if (tick % 5 == 0) {                                             // every ~1 s
                // the e-ink display is the PC's only screen (lid closed, no external monitor): it must not blank or lock after the display timeout
                bool only = attachedRealDisplays() == 0;
                if (only != onlyScreen) {
                    onlyScreen = only;
                    sessionPower.displayRequired(only);
                    logf("power: the e-ink display is %s the only screen - %s", only ? "now" : "no longer", only ? "keeping the display on" : "display timeouts apply again");
                }
            }
            if (cfg.plainWallpaper && tick % 5 == 0) wallpaper.keep();    // every ~1 s (retries until Windows knows the monitor, then verifies)
            if (touchOn && tick % 5 == 0) {                                  // the monitor may have been moved by the user
                RECT tr{};
                if (displayRect(curGdi(), tr)) injector.setTarget(tr);
            }
            Sleep(200);
        }
        stop = true;
        CoUninitialize();
    });
    // Re-shape the virtual monitor to w x h (the tablet was turned).  true = it has that size afterwards.  The touch mapping follows the monitor.
    auto applyGeometry = [&](int w, int h) -> bool {
        RECT rc{};
        int cw = 0, ch = 0;
        if (monitorSize(cw, ch, rc) && cw == w && ch == h) return true;
        if (!cfg.rotate && h > w) {                                    // rotation is switched off: the desktop is always landscape (landscape itself is always fine)
            logf("orient: automatic rotation is off: the monitor stays landscape (it is %dx%d), not %dx%d", cw, ch, w, h);
            return false;
        }
        logf("orient: virtual monitor %dx%d -> %dx%d", cw, ch, w, h);
        if (!setDisplayMode(curGdi(), w, h, cfg.modeHz)) {
            logf("orient: Windows refused %dx%d@%d (is that mode registered for the virtual display driver? see driver\\install-vdd.ps1)", w, h, cfg.modeHz);
            return false;
        }
        for (int i = 0; i < 100 && !stop && !quit; ++i) {              // up to ~5 s until Windows reports the new size
            if (monitorSize(cw, ch, rc) && cw == w && ch == h) break;
            Sleep(50);
        }
        if (cw != w || ch != h) {
            logf("orient: the monitor is %dx%d, not %dx%d", cw, ch, w, h);
            return false;
        }
        Sleep(150);                                                    // let Windows and the driver settle before the duplication is created again
        return true;
    };
    // After a geometry change: tell the tablet which size the PC delivers now (seq = the request it answers, 0 = nobody asked), and make the
    // rest of the pipeline (link, touch mapping) agree with the monitor.  Returns false if the monitor is not the size that was wanted.
    auto settleGeometry = [&](int wantW, int wantH, int seq, bool announceOnly) -> bool {
        RECT rc{};
        int aw = 0, ah = 0;
        if (!monitorSize(aw, ah, rc) || aw <= 0 || ah <= 0) {
            aw = link.panelWidth();
            ah = link.panelHeight();
        } else {
            if (touchOn) {
                injector.setPanel(aw, ah);
                injector.setTarget(rc);
            }
            wallpaper.setRect(rc);
        }
        link.setPanelSize(aw, ah);
        bool ok = aw == wantW && ah == wantH;
        link.sendGeom(aw, ah, seq, !ok && !announceOnly, !cfg.rotate);
        logf("orient: the PC delivers %dx%d%s", aw, ah, ok ? "" : " (not what the tablet asked for)");
        return ok;
    };
    link.sendGeom(vw, vh, 0, false, !cfg.rotate);      // announcement: the size this PC delivers, and whether it follows the tablet's orientation
    std::string reason = sess->run(stop);
    // the link broke by itself (not BYE / the tablet leaving on purpose / the user): wait for the same tablet to come back (Wi-Fi only)
    const Bytes padId = link.deviceId();
    int sizeFixes = 0;                                 // Windows changed the monitor on its own: at most 3 repairs in 20 s
    int64_t sizeFixSince = 0;
    for (;;) {
        if (quit || stop) break;
        if (reason == "geometry change" || reason == "virtual monitor changed size") {
            int w = link.panelWidth(), h = link.panelHeight(), seq = 0;
            bool asked = false;
            if (reason == "geometry change") {
                std::lock_guard<std::mutex> lk(geomWant->m);
                if (geomWant->want) {
                    w = geomWant->w;
                    h = geomWant->h;
                    seq = geomWant->seq;
                    geomWant->want = false;
                    asked = true;
                }
            } else {                                   // Windows (or the user) re-shaped the virtual monitor: put it back to what the tablet shows
                int64_t now = nowNs();
                if (now - sizeFixSince > 20000000000LL) {
                    sizeFixSince = now;
                    sizeFixes = 0;
                }
                if (++sizeFixes > 3) {
                    logf("orient: the virtual monitor keeps changing size - giving up");
                    break;
                }
            }
            bool ok = applyGeometry(w, h);
            if (!ok && !asked) {                       // could not repair it: keep the size Windows chose and let the tablet decide (announcement)
                logf("orient: could not bring the virtual monitor back to %dx%d", w, h);
            }
            settleGeometry(w, h, asked ? seq : 0, !asked);
            SessionConfig sc2 = sc;
            sc2.quickFirst = true;
            {
                std::lock_guard<std::mutex> lk(mu);
                this->session = nullptr;                                    // nobody may touch the old session while it is destroyed
            }
            sess = std::make_unique<Session>(link, sc2);
            {
                std::lock_guard<std::mutex> lk(mu);
                this->session = sess.get();
            }
            {
                std::lock_guard<std::mutex> lk(geomWant->m);
                if (geomWant->want) sess->requestGeometryChange();          // turned again while we were switching
            }
            reason = sess->run(stop);
            continue;
        }
        if (!(serial.empty() && cfg.lanGraceSeconds > 0 && !padId.empty() && !link.tabletLeft() && (reason == "tablet link closed" || reason == "send failed"))) break;
        logf("session: link lost (%s) - keeping the virtual monitor for %d s, waiting for the tablet to reconnect", reason.c_str(), cfg.lanGraceSeconds);
        setStatus(L"平板连接中断：等待它自动重连…（虚拟屏暂时保留，窗口不会被移走）", 1);
        {
            std::lock_guard<std::mutex> lk(mu);
            this->session = nullptr;
        }
        if (touchOn) injector.releaseContacts();          // a finger that was down when the link broke must not stay down
        link.close();
        lan.setExpected(padId);
        AcceptedLink al;
        bool back = false;
        const int64_t deadline = nowNs() + (int64_t)cfg.lanGraceSeconds * 1000000000LL;
        while (!quit && !stop && !reconnect && nowNs() < deadline) {
            if (lan.pop(al)) {
                if (al.padId == padId) {
                    back = true;
                    break;
                }
                closesocket(al.sock);                       // another tablet: not now
                continue;
            }
            Sleep(200);
        }
        lan.setExpected(Bytes());
        if (!back) {
            reason = quit || stop || reconnect ? "stopped" : "tablet did not come back";
            break;
        }
        std::string e;
        if (!link.adopt(al.sock, al.sendKey, al.recvKey, e)) {
            logf("lan: could not continue the session: %s", e.c_str());
            reason = "tablet link closed";
            break;
        }
        logf("session: the tablet is back ('%s') - continuing", al.padName.c_str());
        // the tablet may have been turned (or restarted in another orientation) while it was away: its HELLO tells which shape it shows now
        bool shapeSame = true;
        {
            int tw = link.panelWidth(), th = link.panelHeight();
            if (!cfg.rotate && th > tw) std::swap(tw, th);
            RECT mrc{};
            int cw = 0, ch = 0;
            monitorSize(cw, ch, mrc);
            if (cw != tw || ch != th) {
                shapeSame = false;
                applyGeometry(tw, th);
            }
            settleGeometry(tw, th, 0, true);                 // announces the size the PC delivers (the tablet asks again if that is not its shape)
        }
        if ((link.caps() & CAP_RESUME) && shapeSame) {       // the tablet still shows our last picture: send only what changed meanwhile
            sess->resumeAfterReconnect();
        } else {                                             // it was restarted / turned (shows something else): start over with a proper first frame
            SessionConfig sc2 = sc;
            sc2.quickFirst = true;
            sess = std::make_unique<Session>(link, sc2);
        }
        {
            std::lock_guard<std::mutex> lk(mu);
            this->session = sess.get();
        }
        setStatus(L"已连接（无线）：平板是第 3 块显示器", 2);
        reason = sess->run(stop);
    }
    stop = true;
    guard.join();
    {
        std::lock_guard<std::mutex> lk(mu);
        this->session = nullptr;
    }
    sess.reset();
    logf("session ended: %s", reason.c_str());
    link.setRefreshHandler(nullptr);
    link.setGestureHandler(nullptr);
    link.setMediaKeyHandler(nullptr);
    link.setOrientHandler(nullptr);
    if (touchOn) {
        link.setTouchHandler(nullptr);
        injector.stop();                               // lifts every finger that is still down
    }
    wallpaper.restore();                           // while the virtual monitor still exists
    vdd.close();                                   // the virtual monitor disappears here
    if (quit && link.connected()) link.sendBye();  // quitting Padreen closes the tablet app
    link.close();
    return reason;
}

// A cable is plugged in but adb cannot use the device yet: what has to happen on the tablet (null: nothing to say).
static const wchar_t* usbWaitingHint(const std::vector<AdbPending>& pending) {
    bool offline = false;
    for (const AdbPending& p : pending) {
        if (p.state == "unauthorized") return L"平板已连上 USB，但还没允许这台电脑调试：请在平板屏幕上的「允许 USB 调试」提示里点「允许／确定」（可勾选「始终允许」），之后会自动安装并连接";
        if (p.state == "offline") offline = true;
    }
    return offline ? L"平板的 USB 连接处于离线状态：请重新插拔数据线（还不行，就在平板上关掉再打开「USB 调试」）" : nullptr;
}

void App::superviseLoop() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::string pausedSerial;                 // no more attempts with this tablet until re-plug / manual reconnect: its app was closed on purpose, or preparing it keeps failing
    std::wstring pausedMsg;                   // what the status says meanwhile
    int pausedColor = 0;
    auto pauseOn = [&](const std::string& s, const std::wstring& msg, int color) {
        pausedSerial = s;
        pausedMsg = msg;
        pausedColor = color;
    };
    std::string rejected;                     // serial of a device that is not a MatePad Paper
    int failures = 0;
    while (!quit) {
        if (driverRetry.exchange(false)) {        // the driver was installed just now: a tablet that was refused for lack of it tries again
            pausedSerial.clear();
            failures = 0;
        }
        std::string serial;
        {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait_for(lk, std::chrono::milliseconds(500), [&] { return quit.load() || reconnect.load(); });
            if (quit) break;
            if (devices.empty() || !cfg.usb) {
                pausedSerial.clear();
                rejected.clear();
                failures = 0;
            } else {
                for (auto& d : devices)
                    if (d != rejected) {
                        serial = d;
                        break;
                    }
            }
        }
        if (serial.empty()) {
            // no cable: a paired tablet that connected over the LAN (authenticated by the LanServer) becomes a session
            AcceptedLink al;
            if (cfg.lan && lan.pop(al)) {
                Link link;
                std::string e;
                if (link.adopt(al.sock, al.sendKey, al.recvKey, e)) {
                    logf("lan: session with '%s' (%s)", al.padName.c_str(), al.ip.c_str());
                    reconnect = false;
                    std::wstring err;
                    bool vdFailed = false;
                    std::string reason = runSession(link, "", err, vdFailed);
                    if (vdFailed) setStatus(err, 3);
                    else if (!quit) setStatus(L"平板已断开（" + widen(reason) + L"）；在平板上重新打开 Padreen 即可再连", 0);
                } else {
                    logf("lan: adopt failed: %s", e.c_str());
                }
                continue;
            }
            const wchar_t* usbHint = nullptr;
            if (cfg.usb) {
                std::lock_guard<std::mutex> lk(mu);
                usbHint = usbWaitingHint(pendingDevices);
            }
            if (usbHint) setStatus(usbHint, 1);
            else if (!Vdd::installed()) setStatus(L"还没有安装虚拟显示器驱动：托盘右键 →「安装虚拟显示器驱动」（从 Parsec 官方地址下载），装好后才能把平板当显示器", 3);
            else if (!cfg.lan) setStatus(L"等待平板（USB 线连接；局域网连接已关闭）", 0);
            else if (!lan.running()) setStatus(L"等待平板（请用 USB 线连接）", 0);
            else if (store.list().empty()) setStatus(L"等待平板：右键托盘 →「配对新平板」，在平板上打开 Padreen 输入配对码（或插 USB 线自动配对）", 0);
            else setStatus(L"等待平板：在平板上打开 Padreen 即可连接（或插 USB 线）", 0);
            continue;
        }        if (reconnect.exchange(false)) {
            pausedSerial.clear();
            failures = 0;
        }
        if (serial == pausedSerial) {
            setStatus(pausedMsg, pausedColor);
            continue;
        }
        if (!cfg.deviceModel.empty()) {
            std::string model = adb->shell(serial, "getprop ro.product.model");
            while (!model.empty() && (model.back() == '\r' || model.back() == '\n' || model.back() == ' ')) model.pop_back();
            if (model != cfg.deviceModel) {
                logf("ignoring adb device %s (model '%s')", serial.c_str(), model.c_str());
                rejected = serial;
                continue;
            }
        }
        // ---- prepare ----
        setStatus(L"检测到平板，正在准备…", 1);
        Link link;
        std::wstring err;
        if (!prepareTablet(serial, link, err)) {
            logf("prepare failed: %s", narrow(err).c_str());
            setStatus(err, 3);
            if (++failures >= 3) pauseOn(serial, err + L"。已暂停重试：托盘右键 → 重新连接，或重新插线", 3);
            for (int i = 0; i < 30 && !quit && !reconnect; ++i) Sleep(100);
            continue;
        }
        // ---- run ----
        bool vdFailed = false;
        std::string reason = runSession(link, serial, err, vdFailed);
        if (vdFailed) {
            setStatus(err, 3);
            pauseOn(serial, err, 3);
            continue;
        }
        bool unplugged = !deviceStillThere(serial);
        if (unplugged) {
            setStatus(L"平板已拔出，虚拟显示器已移除", 0);
            failures = 0;
        } else if (reason == "tablet link closed") {
            pauseOn(serial, L"平板上的 App 已关闭（右键托盘图标 → 重新连接）", 0);     // the app was closed on the tablet
        } else if (!quit) {
            setStatus(L"连接中断：" + widen(reason), 3);
            if (++failures >= 3) pauseOn(serial, L"连接中断：" + widen(reason) + L"。已暂停重试：托盘右键 → 重新连接，或重新插线", 3);
            for (int i = 0; i < 30 && !quit && !reconnect; ++i) Sleep(100);
        }
    }
    CoUninitialize();
    supervisorDone = true;
}
// Where settings, the log and the paired-tablet list live: next to the program when that folder is writable (portable use, installs on D:),
// else %LOCALAPPDATA%\Padreen (e.g. a copy under "Program Files" used by a standard user).
std::wstring dataDir() {
    std::wstring d = exeDir();
    std::wstring probe = d + L"\\.write-test";
    HANDLE f = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        CloseHandle(f);
        return d;
    }
    wchar_t base[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return d;
    std::wstring r = std::wstring(base) + L"\\Padreen";
    CreateDirectoryW(r.c_str(), nullptr);
    return r;
}
// Padreen was called PaperDisplay (its tray program PaperHost) before version 1.1.  Take over what the old installation left behind: the settings,
// the paired tablets (this PC's identity stays the same, so tablets that still run the old app keep connecting), the log, and - once the old
// program itself is gone - its "start with Windows" entry.  Runs before the log file is opened: `notes` are logged afterwards.
void migrateLegacy(const std::wstring& data, std::vector<std::string>& notes) {
    auto exists = [](const std::wstring& f) { return GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES; };
    std::vector<std::wstring> dirs{data};              // a portable folder updated in place keeps the old files next to the new program
    wchar_t base[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) {
        std::wstring legacy = std::wstring(base) + L"\\PaperDisplay";
        if (_wcsicmp(legacy.c_str(), data.c_str()) != 0) dirs.push_back(legacy);
    }
    const std::wstring newIni = data + L"\\padreen.ini";
    for (const std::wstring& old : dirs) {
        const std::wstring oldIni = old + L"\\paperhost.ini";
        if (exists(oldIni)) {
            if (!exists(newIni)) {                     // the settings section was called [paperhost]
                std::vector<wchar_t> sec(32768);
                DWORD n = GetPrivateProfileSectionW(L"paperhost", sec.data(), (DWORD)sec.size(), oldIni.c_str());
                for (const wchar_t* p = sec.data(); (DWORD)(p - sec.data()) < n && *p; p += wcslen(p) + 1) {
                    std::wstring kv = p;
                    size_t eq = kv.find(L'=');
                    if (eq == std::wstring::npos || eq == 0) continue;
                    WritePrivateProfileStringW(L"padreen", kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str(), newIni.c_str());
                }
                notes.push_back("migrate: settings taken over from " + narrow(oldIni));
            }
            if (exists(newIni)) DeleteFileW(oldIni.c_str());
        }
        struct Mv {
            const wchar_t* from;
            const wchar_t* to;
        };
        static const Mv files[] = {{L"devices.ini", L"devices.ini"}, {L"wallpaper-original.txt", L"wallpaper-original.txt"}, {L"paperhost.log", L"padreen.log"}, {L"paperhost.log.old", L"padreen.log.old"}};
        for (const Mv& f : files) {
            std::wstring from = old + L"\\" + f.from, to = data + L"\\" + f.to;
            if (_wcsicmp(from.c_str(), to.c_str()) == 0 || !exists(from) || exists(to)) continue;
            if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_COPY_ALLOWED)) notes.push_back("migrate: " + narrow(from) + " -> " + narrow(to));
        }
        if (_wcsicmp(old.c_str(), data.c_str()) != 0) RemoveDirectoryW(old.c_str());      // only succeeds when nothing is left in it
    }
    // "start with Windows": the old entry belongs to the old program; when that file is gone the user's choice moves over to this program
    bool moveAutostart = false;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        wchar_t val[1024] = {};
        DWORD sz = sizeof(val) - sizeof(wchar_t), type = 0;
        if (RegQueryValueExW(k, L"PaperDisplay", nullptr, &type, (BYTE*)val, &sz) == ERROR_SUCCESS && type == REG_SZ) {
            std::wstring path = val;
            if (!path.empty() && path[0] == L'"') {
                size_t q = path.find(L'"', 1);
                path = path.substr(1, q == std::wstring::npos ? std::wstring::npos : q - 1);
            }
            if (!exists(path)) {
                RegDeleteValueW(k, L"PaperDisplay");
                moveAutostart = true;
            }
        }
        RegCloseKey(k);
    }
    if (moveAutostart) {
        setAutostart(true);
        notes.push_back("migrate: 'start with Windows' now starts Padreen");
    }
}
std::wstring findApk(const std::wstring& configured) {
    std::vector<std::wstring> cand;
    if (!configured.empty()) cand.push_back(configured);
    std::wstring d = exeDir();
    cand.push_back(d + L"\\padreen.apk");
    cand.push_back(d + L"\\..\\..\\..\\android\\app\\build\\padreen.apk");
    for (auto& c : cand)
        if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) {
            wchar_t full[MAX_PATH * 2];
            GetFullPathNameW(c.c_str(), (DWORD)(sizeof(full) / sizeof(full[0])), full, nullptr);
            return full;
        }
    return configured;
}

void applyFlag(UiFlag f, bool v) {
    switch (f) {
        case UF_LAN:
            g->cfg.lan = v;
            saveSettings();
            g->lan.setEnabled(v);
            if (v && !g->lan.running()) g->startLan();
            break;
        case UF_TOUCH:
            g->cfg.touch = v;
            saveSettings();
            break;
        case UF_WALLPAPER:
            g->cfg.plainWallpaper = v;
            saveSettings();
            break;
        case UF_AUTOSTART:
            setAutostart(v);
            break;
        case UF_LIDHOLD:
            g->cfg.lidKeepAwake = v;
            saveSettings();
            break;
        case UF_GESTURES:
            g->cfg.gestures = v;
            saveSettings();
            break;
        case UF_ROTATE:
            g->cfg.rotate = v;                    // a running session notices it within 0.2 s (runSession's guard thread) and tells the tablet
            saveSettings();
            break;
        case UF_USB:
            if (v && g->cfg.adbPath.empty()) break;           // no adb: nothing to switch on
            g->cfg.usb = v;
            saveSettings();
            g->cv.notify_all();
            break;
    }
}

void setupUiHost(App& app) {
    UiHost& h = app.uiHost;
    h.statusText = [&app] {
        std::lock_guard<std::mutex> lk(app.mu);
        return app.status;
    };
    h.statusColor = [&app] { return app.color.load(); };
    h.flag = [&app](UiFlag f) {
        switch (f) {
            case UF_LAN: return app.cfg.lan;
            case UF_TOUCH: return app.cfg.touch;
            case UF_WALLPAPER: return app.cfg.plainWallpaper;
            case UF_AUTOSTART: return autostartEnabled();
            case UF_LIDHOLD: return app.cfg.lidKeepAwake;
            case UF_GESTURES: return app.cfg.gestures;
            case UF_ROTATE: return app.cfg.rotate;
            case UF_USB: return app.cfg.usb;
        }
        return false;
    };
    h.lidInfo = [&app]() -> std::wstring {
        PowerHelper st = powerHelperState();
        int a = lidActionNow();
        std::wstring s = L"合上盖子时（当前电源）：" + lidActionName(a) + L"。";
        if (!app.cfg.lidKeepAwake) return s + L"（此功能已关闭）";
        if (st == PowerHelper::Running) return s + L"电源助手在运行：连接期间合盖不会睡眠，断开后自动还原。";
        if (st == PowerHelper::Stopped) return s + L"电源助手已安装但没有运行（重启电脑或在服务里启动 PadreenPower）。";
        return s + (a == 0 ? L"这样合盖本来就不会睡眠。" : L"没有电源助手，合盖会让电脑睡眠；用安装包安装即可启用。");
    };
    h.setFlag = [](UiFlag f, bool v) { applyFlag(f, v); };
    for (const WaveChoice& c : kWaves) h.waves.push_back({c.mode, c.label});
    for (int i = 0; i < 3; ++i) h.settles.push_back({i, kSettle[i].label});
    h.wave = [&app] { return app.cfg.fastMode; };
    h.setWave = [&app](int m) {
        app.cfg.fastMode = m;
        saveSettings();
        std::lock_guard<std::mutex> lk(app.mu);
        if (app.session) app.session->setFastMode(m);
    };
    h.settle = [&app] { return app.cfg.settleLevel; };
    h.setSettle = [&app](int l) {
        app.cfg.settleLevel = l;
        saveSettings();
        std::lock_guard<std::mutex> lk(app.mu);
        if (app.session) app.session->setSettleLevel(l);
    };
    h.fullRefresh = [&app] {
        std::lock_guard<std::mutex> lk(app.mu);
        if (app.session) app.session->requestFullRefresh();
    };
    h.openLog = [&app] { ShellExecuteW(nullptr, L"open", L"notepad.exe", app.logPath.c_str(), nullptr, SW_SHOW); };
    h.enableLan = [&app] {
        app.manualFirewall = true;
        app.startLan();
        app.manualFirewall = false;
    };
    h.lanBlocked = [&app] { return app.lanBlocked; };
    h.lan = &app.lan;
    h.devices = &app.store;
    h.pairMinutes = app.cfg.pairMinutes;
    h.version = L"v" + widen(PD_VER_STRING);
}
}  // namespace

// --console: write to the console the program was started from (the program itself is a windowless GUI program)
static void attachConsoleIfAsked(const std::wstring& commandLine) {
    if (commandLine.find(L"--console") == std::wstring::npos) return;
    if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()) {
        FILE* f;
        freopen_s(&f, "CONOUT$", "w", stderr);
        freopen_s(&f, "CONOUT$", "w", stdout);
    }
}

// the argument that follows `name` on the command line ("" when there is none)
static std::wstring commandLineValue(const wchar_t* name) {
    int n = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &n);
    std::wstring v;
    for (int i = 1; argv && i + 1 < n; ++i)
        if (wcscmp(argv[i], name) == 0) {
            v = argv[i + 1];
            break;
        }
    if (argv) LocalFree(argv);
    return v;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR cmdLine, int) {
    // physical pixel coordinates everywhere (touch injection must hit the right pixel even if the user scales the tablet screen)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    {   // used by the installer / uninstaller (run as the real user): switch the per-user "start with Windows" entry and quit
        std::wstring early = cmdLine ? cmdLine : L"";
        if (early.find(L"--power-service") != std::wstring::npos) return runPowerService();           // started by the Service Control Manager
        if (early.find(L"--install-power-service") != std::wstring::npos) return installPowerService();  // elevated: installer / the app's button
        if (early.find(L"--remove-power-service") != std::wstring::npos) return removePowerService();
        if (early.find(L"--autostart-on") != std::wstring::npos) { setAutostart(true); return 0; }
        if (early.find(L"--autostart-off") != std::wstring::npos) { setAutostart(false); return 0; }
        if (early.find(L"--verify-driver") != std::wstring::npos) {            // development: is the package in --package <folder> (default <program folder>\driver) the one this exe expects?
            attachConsoleIfAsked(early);
            return verifyDriverPackage(commandLineValue(L"--package"));
        }
        if (early.find(L"--fetch-driver") != std::wstring::npos) {             // make-dist.ps1: can this exe get the driver from Parsec by itself?  (writes below --cache only)
            attachConsoleIfAsked(early);
            return fetchDriverPackage(commandLineValue(L"--cache"));
        }
        if (early.find(L"--install-driver") != std::wstring::npos) {           // elevated: started by App::offerDriver (UAC prompt)
            attachConsoleIfAsked(early);
            return runInstallDriver(commandLineValue(L"--log"), commandLineValue(L"--package"));
        }
    }
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\PadreenSingleton");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {       // started a second time (shortcut double click): show the running instance's control panel instead
        if (HWND other = FindWindowW(L"PadreenTray", nullptr)) PostMessageW(other, WM_COMMAND, IDM_OPEN, 0);
        return 0;
    }
    std::wstring cl = cmdLine ? cmdLine : L"";
    bool console = cl.find(L"--console") != std::wstring::npos;
    if (console && (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole())) {
        FILE* f;
        freopen_s(&f, "CONOUT$", "w", stderr);
        freopen_s(&f, "CONOUT$", "w", stdout);
    }
    App app;
    g = &app;
    std::wstring dir = exeDir();
    const std::wstring data = dataDir();
    std::vector<std::string> migrated;
    migrateLegacy(data, migrated);                      // settings / pairings / log of the PaperDisplay days (before the log file is opened: it moves too)
    app.logPath = data + L"\\padreen.log";
    setLogFile(app.logPath.c_str());
    logf("---- Padreen start (%s)", boostProcess().c_str());
    for (const std::string& m : migrated) logf("%s", m.c_str());
    app.iniPath = data + L"\\padreen.ini";
    app.cfg.load(app.iniPath);
    {   // adb: explicit ini setting > adb\adb.exe next to the program > the developer's SDK location; none = wireless only
        auto exists = [](const std::wstring& f) { return !f.empty() && GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES; };
        std::wstring local = dir + L"\\adb\\adb.exe";
        const std::wstring sdk = L"D:\\Android\\android-sdk\\platform-tools\\adb.exe";
        if (!exists(app.cfg.adbPath)) app.cfg.adbPath = exists(local) ? local : exists(sdk) ? sdk : L"";
        if (app.cfg.adbPath.empty()) {
            app.cfg.usb = false;
            logf("adb not found: USB mode is off (the tablet can still connect over the LAN)");
        }
    }    if (cl.find(L"--gray") != std::wstring::npos) app.cfg.bw = false;
    g_debug = cl.find(L"--debug") != std::wstring::npos;
    g_debugPair = g_debug;
    g_traceTouch = cl.find(L"--debug-touch") != std::wstring::npos;
    app.cfg.apkPath = findApk(app.cfg.apkPath);
    logf("adb=%s apk=%s model=%s", narrow(app.cfg.adbPath).c_str(), narrow(app.cfg.apkPath).c_str(), app.cfg.deviceModel.c_str());
    app.adb = std::make_unique<Adb>(app.cfg.adbPath);
    app.adb->abortWhen(&app.quit);
    app.store.load(data + L"\\devices.ini");
    app.store.pcId();                                  // creates the identity on the first run

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"PadreenTray";
    wc.hIcon = loadIcon(IDI_APP, GetSystemMetrics(SM_CXICON));

    RegisterClassW(&wc);
    app.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Padreen", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);   // hidden top-level: receives WM_ENDSESSION
    const int trayPx = GetSystemMetrics(SM_CXSMICON);
    app.icons[0] = loadIcon(IDI_TRAY_GRAY, trayPx);
    app.icons[1] = loadIcon(IDI_TRAY_AMBER, trayPx);
    app.icons[2] = loadIcon(IDI_TRAY_GREEN, trayPx);
    app.icons[3] = loadIcon(IDI_TRAY_RED, trayPx);
    {   // lid open/close events (only logged: the session survives them, see runSession)
        static const GUID kLidSwitch = {0xBA3E0F4D, 0xB817, 0x4094, {0xA8, 0x5D, 0xD4, 0xC9, 0xBB, 0x70, 0xC9, 0xD2}};
        RegisterPowerSettingNotification(app.hwnd, &kLidSwitch, DEVICE_NOTIFY_WINDOW_HANDLE);
    }
    app.nid.cbSize = sizeof(app.nid);
    app.nid.hWnd = app.hwnd;
    app.nid.uID = 1;
    app.nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    app.nid.uCallbackMessage = WM_TRAY;
    app.nid.hIcon = app.icons[0];
    wcscpy_s(app.nid.szTip, L"Padreen");
    Shell_NotifyIconW(NIM_ADD, &app.nid);
    SetTimer(app.hwnd, 1, 1000, nullptr);

    if (!app.cfg.adbPath.empty()) app.watcher.start(app.adb.get(), [&app](const std::vector<std::string>& d, const std::vector<AdbPending>& p) {
        {
            std::lock_guard<std::mutex> lk(app.mu);
            app.devices = d;
            app.pendingDevices = p;
        }
        logf("adb devices: %zu ready, %zu waiting", d.size(), p.size());
        for (const AdbPending& w : p) logf("adb device %s is '%s'", w.serial.c_str(), w.state.c_str());
        app.cv.notify_all();
    });
    setupUiHost(app);
    app.startLan();
    app.offerDriver(true);                              // virtual display driver missing: ask once, before the supervisor can run into it
    app.supervisor = std::thread([&app] { app.superviseLoop(); });
    {   // first run with nothing paired yet: show the pairing window right away (closing it is fine - USB works without)
        wchar_t welcomed[8] = {};
        GetPrivateProfileStringW(L"padreen", L"welcomed", L"0", welcomed, 8, app.iniPath.c_str());
        if (welcomed[0] != L'1' && app.store.list().empty()) {
            WritePrivateProfileStringW(L"padreen", L"welcomed", L"1", app.iniPath.c_str());
            if (app.lan.running()) uiShowPair(&app.uiHost);
            else uiShowMain(&app.uiHost);
        }
    }

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (uiDialogMessage(&m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    app.quit = true;
    app.cv.notify_all();
    app.watcher.stop();
    if (app.supervisor.joinable()) app.supervisor.join();
    app.lan.stop();
    Shell_NotifyIconW(NIM_DELETE, &app.nid);
    for (HICON i : app.icons) DestroyIcon(i);
    logf("---- Padreen exit");
    logRemainingThreads();
    if (single) CloseHandle(single);
    // End the process right here: running the C runtime / DLL shutdown has been seen to hang for minutes (a ghost process that keeps the
    // single-instance mutex and the exe locked), and everything that needs cleaning up has been cleaned up above.
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
