#include "wallpaper.h"

namespace ph {

using Microsoft::WRL::ComPtr;

namespace {

const wchar_t* kHistoryKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Wallpapers";
const int kHistorySlots = 10;                        // Windows keeps BackgroundHistoryPath0..4

struct SavedState {
    std::wstring vdd;                                // the tablet monitor's wallpaper before we made it white
    std::wstring real;                               // what the PC's own screens show (fallback for repairs after a crash)
};

std::wstring readFileText(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    char buf[4096];
    DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);
    buf[got] = 0;
    return widen(std::string(buf, got));
}

void writeFileText(const std::wstring& path, const std::wstring& value) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string s = narrow(value);
    DWORD put = 0;
    WriteFile(h, s.data(), (DWORD)s.size(), &put, nullptr);
    CloseHandle(h);
}

// "vdd=<path>\r\nreal=<path>\r\n"; the first version of the file held just the tablet monitor's path
SavedState readState(const std::wstring& file) {
    SavedState st;
    std::wstring t = readFileText(file);
    if (t.empty()) return st;
    if (t.compare(0, 4, L"vdd=") != 0 && t.compare(0, 5, L"real=") != 0) {
        while (!t.empty() && (t.back() == L'\r' || t.back() == L'\n' || t.back() == L' ')) t.pop_back();
        st.vdd = t;
        return st;
    }
    size_t pos = 0;
    while (pos < t.size()) {
        size_t e = t.find_first_of(L"\r\n", pos);
        std::wstring line = t.substr(pos, e == std::wstring::npos ? std::wstring::npos : e - pos);
        if (line.compare(0, 4, L"vdd=") == 0) st.vdd = line.substr(4);
        else if (line.compare(0, 5, L"real=") == 0) st.real = line.substr(5);
        if (e == std::wstring::npos) break;
        pos = e + 1;
    }
    return st;
}

void writeState(const std::wstring& file, const SavedState& st) { writeFileText(file, L"vdd=" + st.vdd + L"\r\nreal=" + st.real + L"\r\n"); }

bool sameIgnoreCase(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

std::wstring lowered(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::wstring baseName(const std::wstring& p) {
    size_t cut = p.find_last_of(L"\\/");
    return cut == std::wstring::npos ? p : p.substr(cut + 1);
}

// the virtual display driver's monitors ("...DISPLAY#PSCCDD0#..."): ours, never one of the PC's own screens
bool isVirtualId(const std::wstring& id) { return lowered(id).find(L"psccdd0") != std::wstring::npos; }

// "\\?\DISPLAY#LGD06B1#4&..." -> "LGD06B1"
std::wstring shortId(const std::wstring& id) {
    size_t a = id.find(L"DISPLAY#");
    if (a == std::wstring::npos) return id.empty() ? L"?" : id;
    a += 8;
    size_t b = id.find(L'#', a);
    return id.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a);
}

std::wstring monitorWallpaper(IDesktopWallpaper* wp, const std::wstring& id) {
    LPWSTR cur = nullptr;
    std::wstring out;
    if (SUCCEEDED(wp->GetWallpaper(id.c_str(), &cur)) && cur) out = cur;
    if (cur) CoTaskMemFree(cur);
    return out;
}

// the first picture of the "recent images" list that is not ours and still exists: where the PC's own wallpaper most likely came from
std::wstring historyReference() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kHistoryKey, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return L"";
    std::wstring found;
    for (int i = 0; i < kHistorySlots && found.empty(); ++i) {
        wchar_t name[40];
        swprintf(name, 40, L"BackgroundHistoryPath%d", i);
        wchar_t buf[1024] = {};
        DWORD sz = sizeof(buf) - sizeof(wchar_t), type = 0;
        if (RegQueryValueExW(k, name, nullptr, &type, (BYTE*)buf, &sz) != ERROR_SUCCESS || type != REG_SZ) continue;
        std::wstring p = buf;
        if (!p.empty() && !WallpaperGuard::isOurWhite(p) && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) found = p;
    }
    RegCloseKey(k);
    return found;
}

}  // namespace

bool WallpaperGuard::isOurWhite(const std::wstring& path) {
    std::wstring n = lowered(baseName(path));
    return n == L"padreen-white.png" || n == L"paperhost-white.png";     // the file had the program's former name before 1.1.0
}

void WallpaperGuard::cleanHistory() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kHistoryKey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    std::vector<std::wstring> keepers;
    int existing = 0;
    for (int i = 0; i < kHistorySlots; ++i) {
        wchar_t name[40];
        swprintf(name, 40, L"BackgroundHistoryPath%d", i);
        wchar_t buf[1024] = {};
        DWORD sz = sizeof(buf) - sizeof(wchar_t), type = 0;
        if (RegQueryValueExW(k, name, nullptr, &type, (BYTE*)buf, &sz) != ERROR_SUCCESS || type != REG_SZ) continue;
        ++existing;
        if (!isOurWhite(buf)) keepers.push_back(buf);
    }
    if ((int)keepers.size() != existing) {
        for (int i = 0; i < kHistorySlots; ++i) {
            wchar_t name[40];
            swprintf(name, 40, L"BackgroundHistoryPath%d", i);
            if (i < (int)keepers.size()) RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)keepers[i].c_str(), (DWORD)((keepers[i].size() + 1) * sizeof(wchar_t)));
            else RegDeleteValueW(k, name);
        }
        logf("wallpaper: took our white picture out of Windows' recent-images list (%d -> %d entries)", existing, (int)keepers.size());
    }
    RegCloseKey(k);
}

void WallpaperGuard::snapshotOthers(IDesktopWallpaper* wp, const std::wstring& vddId) {
    others_.clear();
    UINT n = 0;
    if (FAILED(wp->GetMonitorDevicePathCount(&n))) return;
    for (UINT i = 0; i < n; ++i) {
        LPWSTR p = nullptr;
        if (FAILED(wp->GetMonitorDevicePathAt(i, &p)) || !p) continue;
        std::wstring id = p;
        CoTaskMemFree(p);
        if (id.empty() || sameIgnoreCase(id, vddId) || isVirtualId(id)) continue;
        others_.push_back({id, monitorWallpaper(wp, id)});
    }
}

std::wstring WallpaperGuard::describeOthers(IDesktopWallpaper* wp) {
    std::wstring s;
    UINT n = 0;
    if (FAILED(wp->GetMonitorDevicePathCount(&n))) return s;
    for (UINT i = 0; i < n; ++i) {
        LPWSTR p = nullptr;
        if (FAILED(wp->GetMonitorDevicePathAt(i, &p)) || !p) continue;
        std::wstring id = p;
        CoTaskMemFree(p);
        if (id.empty() || sameIgnoreCase(id, monitorId_) || isVirtualId(id)) continue;
        if (!s.empty()) s += L", ";
        s += shortId(id) + L"=" + baseName(monitorWallpaper(wp, id));
    }
    return s.empty() ? L"(none)" : s;
}

// Every monitor except the tablet's: none may show our white picture.  Returns how many were put back.
int WallpaperGuard::healOthers(IDesktopWallpaper* wp, bool announce) {
    int fixed = 0;
    UINT n = 0;
    if (FAILED(wp->GetMonitorDevicePathCount(&n))) return 0;
    for (UINT i = 0; i < n; ++i) {
        LPWSTR p = nullptr;
        if (FAILED(wp->GetMonitorDevicePathAt(i, &p)) || !p) continue;
        std::wstring id = p;
        CoTaskMemFree(p);
        if (id.empty() || sameIgnoreCase(id, monitorId_) || isVirtualId(id)) continue;
        if (!isOurWhite(monitorWallpaper(wp, id))) continue;
        std::wstring target;
        for (const Other& o : others_)
            if (sameIgnoreCase(o.id, id) && !o.path.empty() && !isOurWhite(o.path)) target = o.path;
        if (target.empty()) target = ref_;
        if (target.empty()) {
            if (announce) logf("wallpaper: the PC's own screen %s shows our white picture but no earlier wallpaper is known to put back", narrow(shortId(id)).c_str());
            continue;
        }
        HRESULT hr = wp->SetWallpaper(id.c_str(), target.c_str());
        if (healLogs_++ < 20) {                       // (if something keeps putting it back every 2 s the log must not fill up)
            logf("wallpaper: the PC's own screen %s showed our white picture - put '%s' back (hr=0x%08x)", narrow(shortId(id)).c_str(), narrow(target).c_str(), (unsigned)hr);
        }
        ++fixed;
    }
    return fixed;
}

bool WallpaperGuard::apply(const RECT& mr, const std::wstring& whiteImage, const std::wstring& stateFile, bool quiet) {
    restore();
    rect_ = mr;
    white_ = whiteImage;
    stateFile_ = stateFile;
    wanted_ = true;
    if (GetFileAttributesW(whiteImage.c_str()) == INVALID_FILE_ATTRIBUTES) {
        logf("wallpaper: %s is missing - leaving the wallpaper alone", narrow(whiteImage).c_str());
        return false;
    }
    ComPtr<IDesktopWallpaper> wp;
    if (FAILED(CoCreateInstance(__uuidof(DesktopWallpaper), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&wp)))) {
        logf("wallpaper: IDesktopWallpaper not available");
        return false;
    }
    UINT n = 0;
    wp->GetMonitorDevicePathCount(&n);
    std::wstring id;
    int best = 0;                                    // 3 = virtual display driver monitor with the right rectangle, 2 = right rectangle, 1 = driver monitor
    for (UINT i = 0; i < n; ++i) {
        LPWSTR p = nullptr;
        if (FAILED(wp->GetMonitorDevicePathAt(i, &p)) || !p) continue;
        std::wstring cand = p;
        CoTaskMemFree(p);
        if (cand.empty()) continue;
        RECT rc{};
        bool rectOk = SUCCEEDED(wp->GetMonitorRECT(cand.c_str(), &rc)) && rc.left == mr.left && rc.top == mr.top && rc.right == mr.right && rc.bottom == mr.bottom;
        int score = (isVirtualId(cand) ? 1 : 0) + (rectOk ? 2 : 0);
        if (score > best) {
            best = score;
            id = cand;
        }
    }
    if (id.empty()) {
        if (!quiet) logf("wallpaper: the virtual monitor is not known to IDesktopWallpaper (yet) - will retry");
        return false;
    }
    SavedState saved = readState(stateFile);
    // what the PC's own screens show (they are only looked at, never changed - except to undo a leftover of an earlier run, see below)
    snapshotOthers(wp.Get(), id);
    std::wstring ref;
    for (const Other& o : others_)
        if (!o.path.empty() && !isOurWhite(o.path)) {
            ref = o.path;
            break;
        }
    if (ref.empty() && !saved.real.empty() && !isOurWhite(saved.real)) ref = saved.real;     // all of them show our white: a crash left it so
    if (ref.empty()) ref = historyReference();
    ref_ = ref;
    monitorId_ = id;
    for (Other& o : others_)
        if (isOurWhite(o.path)) {            // an earlier run (or an older version) left our white picture on a screen of the PC: give it back
            if (ref.empty()) {
                logf("wallpaper: the PC's own screen %s shows our white picture but no earlier wallpaper is known to put back", narrow(shortId(o.id)).c_str());
                continue;
            }
            HRESULT hr = wp->SetWallpaper(o.id.c_str(), ref.c_str());
            logf("wallpaper: the PC's own screen %s still showed our white picture from an earlier run - put '%s' back (hr=0x%08x)", narrow(shortId(o.id)).c_str(), narrow(ref).c_str(),
                 (unsigned)hr);
            o.path = ref;
        }
    // the tablet's monitor: what it showed before (never our own white picture: a leftover of an earlier run is replaced by the PC's wallpaper)
    std::wstring current = monitorWallpaper(wp.Get(), id);
    if (isOurWhite(current)) original_ = (!saved.vdd.empty() && !isOurWhite(saved.vdd)) ? saved.vdd : ref;
    else original_ = current;
    if (original_.empty()) original_ = ref;
    writeState(stateFile, {original_, ref_});
    bool already = sameIgnoreCase(current, whiteImage);
    if (!already && FAILED(wp->SetWallpaper(id.c_str(), whiteImage.c_str()))) {
        logf("wallpaper: SetWallpaper failed");
        return false;
    }
    applied_ = true;
    logf("wallpaper: virtual monitor -> plain white%s (original '%s'); the PC's own screens are left alone: %s", already ? " (it already was)" : "", narrow(original_).c_str(),
         narrow(describeOthers(wp.Get())).c_str());
    cleanHistory();
    return true;
}

void WallpaperGuard::keep() {
    if (!wanted_) return;
    int64_t now = nowNs();
    if (!applied_) {
        if (now - lastTryNs_ < 1000000000LL || tries_ >= 60) return;
        lastTryNs_ = now;
        ++tries_;
        apply(rect_, white_, stateFile_, true);
        return;
    }
    ComPtr<IDesktopWallpaper> wp;
    if (FAILED(CoCreateInstance(__uuidof(DesktopWallpaper), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&wp)))) return;
    if (now - lastOthersNs_ >= 2000000000LL) {        // none of the PC's own screens may show our white picture (also not a newly plugged one)
        lastOthersNs_ = now;
        healOthers(wp.Get(), healLogs_ < 3);
    }
    if (now - lastFixNs_ < 10000000000LL) return;
    std::wstring current = monitorWallpaper(wp.Get(), monitorId_);
    if (!sameIgnoreCase(current, white_)) {
        logf("wallpaper: the tablet's monitor now shows '%s' - putting the plain white one back", narrow(current).c_str());
        HRESULT hr = wp->SetWallpaper(monitorId_.c_str(), white_.c_str());
        std::wstring after = monitorWallpaper(wp.Get(), monitorId_);
        logf("wallpaper: SetWallpaper hr=0x%08x, reads back '%s'", (unsigned)hr, narrow(after).c_str());
        lastFixNs_ = now;
    }
}

void WallpaperGuard::restore() {
    wanted_ = false;
    if (!applied_) return;
    applied_ = false;
    ComPtr<IDesktopWallpaper> wp;
    if (SUCCEEDED(CoCreateInstance(__uuidof(DesktopWallpaper), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&wp)))) {
        const std::wstring target = original_.empty() ? ref_ : original_;
        if (!target.empty() && SUCCEEDED(wp->SetWallpaper(monitorId_.c_str(), target.c_str()))) {
            logf("wallpaper: restored '%s'", narrow(target).c_str());
            DeleteFileW(stateFile_.c_str());
        } else {
            logf("wallpaper: could not restore (original '%s'); the state file keeps it", narrow(original_).c_str());
        }
        healOthers(wp.Get(), true);                   // last look at the PC's own screens before the tablet's monitor goes away
        logf("wallpaper: the PC's own screens now: %s", narrow(describeOthers(wp.Get())).c_str());
    }
    cleanHistory();
}

}  // namespace ph
