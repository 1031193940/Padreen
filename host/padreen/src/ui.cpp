#include "ui.h"

#include <iphlpapi.h>
#include <commctrl.h>
#include <shellscalingapi.h>

#include <map>

#include <algorithm>

#include "../res/resource.h"

namespace ph {
namespace {

enum : UINT { WM_UI_REFRESH = WM_APP + 40 };
const wchar_t* kFont = L"Microsoft YaHei UI";

struct Fonts {
    HFONT normal = nullptr, bold = nullptr, big = nullptr, huge = nullptr;
    void make(HWND w) {
        free();
        UINT dpi = GetDpiForWindow(w);
        auto mk = [&](int pt, bool b, const wchar_t* face) {
            return CreateFontW(-MulDiv(pt, (int)dpi, 72), 0, 0, 0, b ? FW_SEMIBOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
        };
        normal = mk(10, false, kFont);
        bold = mk(10, true, kFont);
        big = mk(15, true, kFont);
        huge = mk(40, true, L"Consolas");
    }
    void free() {
        for (HFONT* f : {&normal, &bold, &big, &huge})
            if (*f) DeleteObject(*f), *f = nullptr;
    }
};

inline int S(HWND w, int px96) { return MulDiv(px96, (int)GetDpiForWindow(w), 96); }

// every child control with its design rectangle (96-dpi units) and font, so a window can be re-laid out when it moves to a monitor with another DPI
struct ChildInfo {
    HWND h;
    int x, y, w, ht;
    HFONT font;
};
std::map<HWND, std::vector<ChildInfo>> g_children;

HWND mk(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT font, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(parent, x), S(parent, y), S(parent, w), S(parent, h), parent, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    if (font) SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    g_children[parent].push_back({c, x, y, w, h, font});
    return c;
}

// the window moved to a monitor with another DPI (or the DPI changed): new fonts, new control rectangles
void relayout(HWND w, Fonts& f) {
    Fonts old = f;
    f.make(w);
    for (ChildInfo& c : g_children[w]) {
        SetWindowPos(c.h, nullptr, S(w, c.x), S(w, c.y), S(w, c.w), S(w, c.ht), SWP_NOZORDER | SWP_NOACTIVATE);
        if (!c.font) continue;
        HFONT nf = c.font == old.normal ? f.normal : c.font == old.bold ? f.bold : c.font == old.big ? f.big : c.font == old.huge ? f.huge : nullptr;
        if (nf) {
            SendMessageW(c.h, WM_SETFONT, (WPARAM)nf, TRUE);
            c.font = nf;
        }
    }
    InvalidateRect(w, nullptr, TRUE);
}

HWND createTop(const wchar_t* cls, const wchar_t* title, int w96, int h96, void* param) {
    // open on the monitor the user is working on (where the mouse is - the tray click), sized for that monitor's DPI
    POINT cur = {};
    GetCursorPos(&cur);
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpi = 96, dpiY = 96;
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi, &dpiY)) || dpi == 0) dpi = GetDpiForSystem();
    const RECT& wa = mi.rcWork;
    int w = MulDiv(w96, (int)dpi, 96), h = MulDiv(h96, (int)dpi, 96);
    RECT r = {0, 0, w, h};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    int ww = r.right - r.left, hh = r.bottom - r.top;
    int x = wa.left + std::max(0, ((int)(wa.right - wa.left) - ww) / 2);
    int y = wa.top + std::max(0, ((int)(wa.bottom - wa.top) - hh) / 2);
    return CreateWindowExW(WS_EX_CONTROLPARENT, cls, title, style, x, y, ww, hh, nullptr, nullptr, GetModuleHandleW(nullptr), param);
}

bool g_registered = false;
void registerClass(const wchar_t* name, WNDPROC proc) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = name;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    RegisterClassW(&wc);
}

// IPv4 addresses of this PC's active network adapters (for "type the PC's address" on the tablet when the automatic search cannot see it)
std::wstring lanAddresses() {
    ULONG sz = 16384;
    std::vector<uint8_t> buf(sz);
    auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    ULONG r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, aa, &sz);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(sz);
        aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, aa, &sz);
    }
    if (r != NO_ERROR) return L"";
    std::wstring out;
    for (auto* a = aa; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            auto* sa = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            if (!sa || sa->sin_family != AF_INET) continue;
            if ((ntohl(sa->sin_addr.s_addr) >> 16) == 0xA9FE) continue;       // 169.254.x.x (no DHCP)
            wchar_t ip[32] = {};
            InetNtopW(AF_INET, &sa->sin_addr, ip, 32);
            if (!out.empty()) out += L"    ";
            out += ip;
        }
    }
    return out;
}

std::wstring groupCode(const std::string& c) {
    if (c.size() != 9) return widen(c);
    return widen(c.substr(0, 3) + " " + c.substr(3, 3) + " " + c.substr(6, 3));
}

HWND g_main = nullptr, g_pair = nullptr, g_devs = nullptr;
UiHost* g_host = nullptr;

// ================================================================================================================ pairing window
struct PairState {
    Fonts f;
    HWND code, hint, info, state, again, close;
    std::vector<Bytes> before;                // paired tablets when the window opened
    int64_t doneAtMs = 0;
};

void pairStart(HWND w, PairState* s) {
    s->before.clear();
    for (const PairedPad& p : g_host->devices->list()) s->before.push_back(p.id);
    g_host->lan->openPairing(g_host->pairMinutes * 60);
    s->doneAtMs = 0;
    SetWindowTextW(s->again, L"重新生成");
    ShowWindow(s->again, SW_HIDE);
    SendMessageW(w, WM_TIMER, 1, 0);
}

void pairUpdate(HWND w, PairState* s) {
    // a tablet that was not paired before = success
    for (const PairedPad& p : g_host->devices->list()) {
        bool known = false;
        for (const Bytes& b : s->before) known = known || b == p.id;
        if (!known) {
            SetWindowTextW(s->code, L"✓");
            SetWindowTextW(s->state, (L"已配对：" + widen(p.name)).c_str());
            SetWindowTextW(s->info, L"");
            ShowWindow(s->again, SW_HIDE);
            if (!s->doneAtMs) s->doneAtMs = (int64_t)GetTickCount64();
            if ((int64_t)GetTickCount64() - s->doneAtMs > 3500) DestroyWindow(w);
            return;
        }
    }
    LanServer::PairState st = g_host->lan->pairing();
    if (st.open) {
        SetWindowTextW(s->code, groupCode(st.code).c_str());
        wchar_t b[160];
        swprintf(b, 160, L"剩余 %d:%02d", st.secondsLeft / 60, st.secondsLeft % 60);
        SetWindowTextW(s->info, b);
        SetWindowTextW(s->state, L"等待平板输入…");
        ShowWindow(s->again, SW_HIDE);
    } else {
        SetWindowTextW(s->code, L"— — —");
        SetWindowTextW(s->info, L"");
        SetWindowTextW(s->state, st.attemptsLeft <= 0 ? L"输错太多次，配对码已失效" : L"配对码已过期");
        ShowWindow(s->again, SW_SHOW);
    }
}

LRESULT CALLBACK PairProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    PairState* s = (PairState*)GetWindowLongPtrW(w, GWLP_USERDATA);
    switch (m) {
        case WM_CREATE: {
            s = new PairState();
            SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)s);
            s->f.make(w);
            s->hint = mk(w, L"STATIC", L"在平板上打开 Padreen，点这台电脑旁的「配对」，输入下面的数字", SS_LEFT, 24, 20, 472, 24, 0, s->f.normal);
            s->code = mk(w, L"STATIC", L"", SS_CENTER | SS_CENTERIMAGE | WS_BORDER, 24, 56, 472, 96, 0, s->f.huge);
            s->info = mk(w, L"STATIC", L"", SS_CENTER, 24, 160, 472, 22, 0, s->f.normal);
            s->state = mk(w, L"STATIC", L"", SS_CENTER, 24, 188, 472, 28, 0, s->f.bold);
            s->again = mk(w, L"BUTTON", L"重新生成", BS_PUSHBUTTON | WS_TABSTOP, 24, 232, 120, 36, 101, s->f.normal);
            s->close = mk(w, L"BUTTON", L"关闭", BS_DEFPUSHBUTTON | WS_TABSTOP, 396, 232, 100, 36, IDCANCEL, s->f.normal);
            pairStart(w, s);
            SetTimer(w, 1, 500, nullptr);
            return 0;
        }
        case WM_TIMER:
            if (s) pairUpdate(w, s);
            return 0;
        case WM_DPICHANGED: {
            const RECT* nr = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(w, nullptr, nr->left, nr->top, nr->right - nr->left, nr->bottom - nr->top, SWP_NOZORDER | SWP_NOACTIVATE);
            if (s) relayout(w, s->f);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == 101 && s) pairStart(w, s);
            else if (LOWORD(wp) == IDCANCEL) DestroyWindow(w);
            return 0;
        case WM_CLOSE:
            DestroyWindow(w);
            return 0;
        case WM_DESTROY:
            KillTimer(w, 1);
            if (g_host && g_host->lan) g_host->lan->closePairing();
            if (s) {
                s->f.free();
                delete s;
            }
            g_children.erase(w);
            g_pair = nullptr;
            return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

// ================================================================================================================ paired tablets
struct DevState {
    Fonts f;
    HWND list, del, info;
    std::vector<PairedPad> pads;
};

std::wstring ago(int64_t ms) {
    if (ms <= 0) return L"未连接过";
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    int64_t now = (int64_t)((u.QuadPart - 116444736000000000ULL) / 10000ULL);
    int64_t d = (now - ms) / 1000;
    if (d < 90) return L"刚刚";
    if (d < 5400) return std::to_wstring(d / 60) + L" 分钟前";
    if (d < 36 * 3600) return std::to_wstring(d / 3600) + L" 小时前";
    return std::to_wstring(d / 86400) + L" 天前";
}

void devFill(DevState* s) {
    SendMessageW(s->list, LB_RESETCONTENT, 0, 0);
    s->pads = g_host->devices->list();
    for (const PairedPad& p : s->pads) {
        std::wstring t = widen(p.name) + L"      " + (p.ip.empty() ? L"" : widen(p.ip) + L"      ") + ago(p.lastMs);
        SendMessageW(s->list, LB_ADDSTRING, 0, (LPARAM)t.c_str());
    }
    SetWindowTextW(s->info, s->pads.empty() ? L"还没有配对的平板" : L"");
    EnableWindow(s->del, FALSE);
}

LRESULT CALLBACK DevProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    DevState* s = (DevState*)GetWindowLongPtrW(w, GWLP_USERDATA);
    switch (m) {
        case WM_CREATE: {
            s = new DevState();
            SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)s);
            s->f.make(w);
            s->list = mk(w, L"LISTBOX", L"", LBS_NOTIFY | WS_BORDER | WS_VSCROLL | WS_TABSTOP, 20, 16, 520, 196, 100, s->f.normal);
            s->info = mk(w, L"STATIC", L"", SS_LEFT, 20, 220, 520, 24, 0, s->f.normal);
            s->del = mk(w, L"BUTTON", L"取消配对", BS_PUSHBUTTON | WS_TABSTOP, 20, 256, 140, 34, 101, s->f.normal);
            mk(w, L"BUTTON", L"关闭", BS_DEFPUSHBUTTON | WS_TABSTOP, 440, 256, 100, 34, IDCANCEL, s->f.normal);
            devFill(s);
            return 0;
        }
        case WM_COMMAND:
            if (!s) return 0;
            if (LOWORD(wp) == 100 && HIWORD(wp) == LBN_SELCHANGE) EnableWindow(s->del, SendMessageW(s->list, LB_GETCURSEL, 0, 0) != LB_ERR);
            if (LOWORD(wp) == 101) {
                int i = (int)SendMessageW(s->list, LB_GETCURSEL, 0, 0);
                if (i >= 0 && i < (int)s->pads.size()) {
                    std::wstring q = L"取消「" + widen(s->pads[(size_t)i].name) + L"」的配对？";
                    if (MessageBoxW(w, q.c_str(), L"取消配对", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                        g_host->devices->remove(s->pads[(size_t)i].id);
                        devFill(s);
                        uiRefresh();
                    }
                }
            }
            if (LOWORD(wp) == IDCANCEL) DestroyWindow(w);
            return 0;
        case WM_UI_REFRESH:
            if (s) devFill(s);
            return 0;
        case WM_DPICHANGED: {
            const RECT* nr = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(w, nullptr, nr->left, nr->top, nr->right - nr->left, nr->bottom - nr->top, SWP_NOZORDER | SWP_NOACTIVATE);
            if (s) relayout(w, s->f);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(w);
            return 0;
        case WM_DESTROY:
            if (s) {
                s->f.free();
                delete s;
            }
            g_children.erase(w);
            g_devs = nullptr;
            return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

// ================================================================================================================ control panel
enum : int { ID_PAIR = 201, ID_DEVS, ID_FULL, ID_LAN, ID_TOUCH, ID_WALL, ID_AUTO, ID_WAVE, ID_SETTLE, ID_LOG, ID_FIX, ID_LID, ID_USB, ID_GEST, ID_ROT, ID_DRV };

struct MainState {
    Fonts f;
    HWND status, dot, lan, usb, touch, gest, rot, wall, autoStart, lid, wave, settle, fix, drv;
};

void mainSync(HWND w, MainState* s) {
    std::wstring st = g_host->statusText();
    SetWindowTextW(s->status, st.c_str());
    InvalidateRect(s->dot, nullptr, TRUE);
    SendMessageW(s->lan, BM_SETCHECK, g_host->flag(UF_LAN) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s->touch, BM_SETCHECK, g_host->flag(UF_TOUCH) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s->wall, BM_SETCHECK, g_host->flag(UF_WALLPAPER) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s->autoStart, BM_SETCHECK, g_host->flag(UF_AUTOSTART) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s->gest, BM_SETCHECK, g_host->flag(UF_GESTURES) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s->rot, BM_SETCHECK, g_host->flag(UF_ROTATE) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s->usb, BM_SETCHECK, g_host->flag(UF_USB) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s->lid, BM_SETCHECK, g_host->flag(UF_LIDHOLD) ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_host->lidAvailable) {          // without the power helper the switch does nothing: grey it out and say why (the one place a note is worth it)
        const bool ok = g_host->lidAvailable();
        const wchar_t* label = ok ? L"合盖不休眠" : L"合盖不休眠（未安装电源助手）";
        wchar_t cur[64] = {};
        GetWindowTextW(s->lid, cur, 64);
        if (wcscmp(cur, label) != 0) SetWindowTextW(s->lid, label);
        EnableWindow(s->lid, ok ? TRUE : FALSE);
    }
    ShowWindow(s->fix, g_host->lanBlocked && g_host->lanBlocked() ? SW_SHOW : SW_HIDE);
    ShowWindow(s->drv, g_host->driverMissing && g_host->driverMissing() ? SW_SHOW : SW_HIDE);
    int cw = (int)SendMessageW(s->wave, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < cw; ++i)
        if ((int)SendMessageW(s->wave, CB_GETITEMDATA, (WPARAM)i, 0) == g_host->wave()) SendMessageW(s->wave, CB_SETCURSEL, (WPARAM)i, 0);
    int cs = (int)SendMessageW(s->settle, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < cs; ++i)
        if ((int)SendMessageW(s->settle, CB_GETITEMDATA, (WPARAM)i, 0) == g_host->settle()) SendMessageW(s->settle, CB_SETCURSEL, (WPARAM)i, 0);
    (void)w;
}

LRESULT CALLBACK MainProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    MainState* s = (MainState*)GetWindowLongPtrW(w, GWLP_USERDATA);
    switch (m) {
        case WM_CREATE: {
            s = new MainState();
            SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)s);
            s->f.make(w);
            HICON ic = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, S(w, 48), S(w, 48), LR_DEFAULTCOLOR);
            HWND pic = mk(w, L"STATIC", L"", SS_ICON, 22, 16, 48, 48, 0, nullptr);
            SendMessageW(pic, STM_SETICON, (WPARAM)ic, 0);
            mk(w, L"STATIC", (L"Padreen  " + g_host->version).c_str(), SS_LEFT, 84, 16, 400, 30, 0, s->f.big);
            s->dot = mk(w, L"STATIC", L"", SS_OWNERDRAW, 84, 54, 14, 14, 0, nullptr);
            s->status = mk(w, L"STATIC", L"", SS_LEFT, 106, 50, 326, 44, 0, s->f.normal);
            s->drv = mk(w, L"BUTTON", L"安装驱动…", BS_PUSHBUTTON | WS_TABSTOP, 440, 46, 82, 28, ID_DRV, s->f.normal);
            mk(w, L"STATIC", L"平板", SS_LEFT, 22, 106, 200, 22, 0, s->f.bold);
            mk(w, L"BUTTON", L"配对…", BS_PUSHBUTTON | WS_TABSTOP, 22, 132, 160, 36, ID_PAIR, s->f.normal);
            mk(w, L"BUTTON", L"已配对…", BS_PUSHBUTTON | WS_TABSTOP, 192, 132, 160, 36, ID_DEVS, s->f.normal);
            mk(w, L"BUTTON", L"全屏刷新", BS_PUSHBUTTON | WS_TABSTOP, 362, 132, 160, 36, ID_FULL, s->f.normal);
            mk(w, L"STATIC", L"设置", SS_LEFT, 22, 188, 200, 22, 0, s->f.bold);
            s->lan = mk(w, L"BUTTON", L"Wi-Fi 连接", BS_AUTOCHECKBOX | WS_TABSTOP, 22, 214, 400, 24, ID_LAN, s->f.normal);
            s->fix = mk(w, L"BUTTON", L"启用…", BS_PUSHBUTTON | WS_TABSTOP, 440, 210, 82, 28, ID_FIX, s->f.normal);
            s->usb = mk(w, L"BUTTON", L"优先 USB", BS_AUTOCHECKBOX | WS_TABSTOP, 22, 242, 500, 24, ID_USB, s->f.normal);
            s->touch = mk(w, L"BUTTON", L"触控", BS_AUTOCHECKBOX | WS_TABSTOP, 22, 270, 500, 24, ID_TOUCH, s->f.normal);
            s->gest = mk(w, L"BUTTON", L"三指手势", BS_AUTOCHECKBOX | WS_TABSTOP, 22, 298, 500, 24, ID_GEST, s->f.normal);
            s->rot = mk(w, L"BUTTON", L"自动旋转", BS_AUTOCHECKBOX | WS_TABSTOP, 22, 326, 500, 24, ID_ROT, s->f.normal);
            s->wall = mk(w, L"BUTTON", L"纯白壁纸", BS_AUTOCHECKBOX | WS_TABSTOP, 22, 354, 500, 24, ID_WALL, s->f.normal);
            s->autoStart = mk(w, L"BUTTON", L"开机启动", BS_AUTOCHECKBOX | WS_TABSTOP, 22, 382, 500, 24, ID_AUTO, s->f.normal);
            s->lid = mk(w, L"BUTTON", L"合盖不休眠", BS_AUTOCHECKBOX | WS_TABSTOP, 22, 410, 500, 24, ID_LID, s->f.normal);
            mk(w, L"STATIC", L"刷新方式", SS_LEFT, 22, 456, 100, 22, 0, s->f.normal);
            s->wave = mk(w, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 130, 452, 392, 200, ID_WAVE, s->f.normal);
            mk(w, L"STATIC", L"补灰阶", SS_LEFT, 22, 492, 100, 22, 0, s->f.normal);
            s->settle = mk(w, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 130, 488, 392, 200, ID_SETTLE, s->f.normal);
            for (const UiChoice& c : g_host->waves) SendMessageW(s->wave, CB_SETITEMDATA, SendMessageW(s->wave, CB_ADDSTRING, 0, (LPARAM)c.label.c_str()), c.value);
            for (const UiChoice& c : g_host->settles) SendMessageW(s->settle, CB_SETITEMDATA, SendMessageW(s->settle, CB_ADDSTRING, 0, (LPARAM)c.label.c_str()), c.value);
            std::wstring addrs = lanAddresses();
            mk(w, L"STATIC", (L"本机 IP  " + (addrs.empty() ? std::wstring(L"—") : addrs)).c_str(), SS_LEFT, 22, 532, 500, 38, 0, s->f.normal);
            mk(w, L"BUTTON", L"日志", BS_PUSHBUTTON | WS_TABSTOP, 22, 580, 100, 34, ID_LOG, s->f.normal);
            mk(w, L"BUTTON", L"关闭", BS_DEFPUSHBUTTON | WS_TABSTOP, 422, 580, 100, 34, IDCANCEL, s->f.normal);
            mainSync(w, s);
            SetTimer(w, 1, 1000, nullptr);
            return 0;
        }
        case WM_TIMER:
            if (s) mainSync(w, s);
            return 0;
        case WM_DPICHANGED: {
            const RECT* nr = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(w, nullptr, nr->left, nr->top, nr->right - nr->left, nr->bottom - nr->top, SWP_NOZORDER | SWP_NOACTIVATE);
            if (s) relayout(w, s->f);
            return 0;
        }
        case WM_UI_REFRESH:
            if (s) mainSync(w, s);
            return 0;
        case WM_DRAWITEM: {
            DRAWITEMSTRUCT* d = (DRAWITEMSTRUCT*)lp;
            if (s && d->hwndItem == s->dot) {
                static const COLORREF col[4] = {RGB(150, 150, 150), RGB(240, 160, 0), RGB(30, 190, 70), RGB(225, 50, 50)};
                int c = std::clamp(g_host->statusColor ? g_host->statusColor() : 0, 0, 3);
                HBRUSH bg = GetSysColorBrush(COLOR_BTNFACE);
                FillRect(d->hDC, &d->rcItem, bg);
                HBRUSH br = CreateSolidBrush(col[c]);
                HGDIOBJ ob = SelectObject(d->hDC, br);
                HGDIOBJ op = SelectObject(d->hDC, GetStockObject(NULL_PEN));
                Ellipse(d->hDC, d->rcItem.left, d->rcItem.top, d->rcItem.right, d->rcItem.bottom);
                SelectObject(d->hDC, ob);
                SelectObject(d->hDC, op);
                DeleteObject(br);
            }
            return TRUE;
        }
        case WM_COMMAND: {
            if (!s) return 0;
            int id = LOWORD(wp);
            switch (id) {
                case ID_PAIR:
                    uiShowPair(g_host);
                    break;
                case ID_DEVS:
                    uiShowDevices(g_host);
                    break;
                case ID_FULL:
                    if (g_host->fullRefresh) g_host->fullRefresh();
                    break;
                case ID_LAN:
                    g_host->setFlag(UF_LAN, SendMessageW(s->lan, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    break;
                case ID_GEST:
                    g_host->setFlag(UF_GESTURES, SendMessageW(s->gest, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    break;
                case ID_ROT:
                    g_host->setFlag(UF_ROTATE, SendMessageW(s->rot, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    break;
                case ID_USB:
                    g_host->setFlag(UF_USB, SendMessageW(s->usb, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    break;
                case ID_TOUCH:
                    g_host->setFlag(UF_TOUCH, SendMessageW(s->touch, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    break;
                case ID_WALL:
                    g_host->setFlag(UF_WALLPAPER, SendMessageW(s->wall, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    break;
                case ID_AUTO:
                    g_host->setFlag(UF_AUTOSTART, SendMessageW(s->autoStart, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    break;
                case ID_LID:
                    g_host->setFlag(UF_LIDHOLD, SendMessageW(s->lid, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    break;
                case ID_FIX:
                    if (g_host->enableLan) g_host->enableLan();
                    break;
                case ID_DRV:
                    if (g_host->installDriver) g_host->installDriver();
                    break;
                case ID_LOG:
                    if (g_host->openLog) g_host->openLog();
                    break;
                case ID_WAVE:
                case ID_SETTLE:
                    if (HIWORD(wp) == CBN_SELCHANGE) {
                        HWND cb = id == ID_WAVE ? s->wave : s->settle;
                        int i = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
                        if (i >= 0) {
                            int v = (int)SendMessageW(cb, CB_GETITEMDATA, (WPARAM)i, 0);
                            if (id == ID_WAVE) g_host->setWave(v);
                            else g_host->setSettle(v);
                        }
                    }
                    break;
                case IDCANCEL:
                    DestroyWindow(w);
                    break;
            }
            mainSync(w, s);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(w);
            return 0;
        case WM_DESTROY:
            KillTimer(w, 1);
            if (s) {
                s->f.free();
                delete s;
            }
            g_children.erase(w);
            g_main = nullptr;
            return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

void ensureClasses() {
    if (g_registered) return;
    g_registered = true;
    INITCOMMONCONTROLSEX ic = {sizeof(ic), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&ic);
    registerClass(L"PadreenMain", MainProc);
    registerClass(L"PadreenPair", PairProc);
    registerClass(L"PadreenDevs", DevProc);
}

void showOrFocus(HWND* slot, const wchar_t* cls, const wchar_t* title, int w, int h) {
    ensureClasses();
    if (*slot && IsWindow(*slot)) {
        if (IsIconic(*slot)) ShowWindow(*slot, SW_RESTORE);
        SetForegroundWindow(*slot);
        return;
    }
    *slot = createTop(cls, title, w, h, nullptr);
    ShowWindow(*slot, SW_SHOWNORMAL);
    if (!IsWindowVisible(*slot)) ShowWindow(*slot, SW_SHOWNORMAL);       // the first ShowWindow of a process obeys STARTUPINFO (hidden launch): repeat
    SetForegroundWindow(*slot);
}

}  // namespace

void uiShowMain(UiHost* host) {
    g_host = host;
    showOrFocus(&g_main, L"PadreenMain", L"Padreen", 544, 640);
}

void uiShowPair(UiHost* host) {
    g_host = host;
    showOrFocus(&g_pair, L"PadreenPair", L"配对", 520, 296);
}

void uiShowDevices(UiHost* host) {
    g_host = host;
    showOrFocus(&g_devs, L"PadreenDevs", L"已配对", 560, 312);
}

void uiRefresh() {
    for (HWND w : {g_main, g_devs})
        if (w && IsWindow(w)) PostMessageW(w, WM_UI_REFRESH, 0, 0);
}

bool uiDialogMessage(MSG* m) {
    for (HWND w : {g_main, g_pair, g_devs})
        if (w && IsWindow(w) && IsDialogMessageW(w, m)) return true;
    return false;
}

}  // namespace ph
