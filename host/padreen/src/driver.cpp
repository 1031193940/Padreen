#include "driver.h"

#include <sddl.h>
#include <shellapi.h>
#include <winhttp.h>

#include "crypto.h"
#include "vdd.h"
#include "vddpack.h"

namespace ph {

namespace {

// Parsec's official downloads.  0.41 is the driver Padreen is built and tested with; nefconw.exe (nefcon 1.10.0, signed by Parsec) is only in the 0.45 installer.
struct Source {
    const wchar_t* file;      // the name it is kept under in the cache folder
    const wchar_t* url;
    const char* sha256;       // of the installer itself, lower case hex
};
const Source kSources[] = {
    {L"parsec-vdd-0.45.0.0.exe", L"https://builds.parsec.app/vdd/parsec-vdd-0.45.0.0.exe", "e23332448fdaf5aa017cb308db5ef6855fac526a7ded05d80c039404126d5362"},
    {L"parsec-vdd-0.41.0.0.exe", L"https://builds.parsec.app/vdd/parsec-vdd-0.41.0.0.exe", "317977c5abd58f1c3f7b27ef7f96947efd07e6d35b27e415cea5daa6007629b1"},
};
constexpr size_t kSourceCount = sizeof(kSources) / sizeof(kSources[0]);

struct Pin {
    const wchar_t* rel;    // path inside the package folder
    const char* sha256;    // lower case hex
    size_t src;            // index into kSources: the installer the file is taken from
};
// The package: Parsec VDD 0.41 (driver) and nefcon 1.10.0 (nefconw.exe, signed by Parsec).  A different driver version means different values
// here AND in tools\fetch-vdd.ps1 (the development helper that unpacks the same installers with 7-Zip).
const Pin kPins[] = {
    {L"nefconw.exe", "cf746d1b0bbb713993d4a90dccd774c78d9fff8c2ba5a054b6c8f56c77e1eee1", 0},
    {L"driver\\mm.inf", "b003d2fe10c366fb454324149b46d6b14fb17abb75f79fa8bc9e0043fc56c942", 1},
    {L"driver\\mm.dll", "7f2e8860d56461ea5eb2d2d54a63fc934f3694b32eda72170e069797d846d6c7", 1},
    {L"driver\\mm.cat", "5852d0c23ec74a2f5594bc498525e7beb29ce25bd7adac4906ddbdcbf539b1f9", 1},
};
constexpr size_t kPinCount = sizeof(kPins) / sizeof(kPins[0]);

// the same arguments Parsec's own vddinstall.bat uses
const wchar_t* const kDeviceArgs = L"--hardware-id Root\\Parsec\\VDA --class-guid \"4D36E968-E325-11CE-BFC1-08002BE10318\"";

// dir empty = the place next to the program, <program folder>\driver
std::wstring packageDir(const std::wstring& dir) { return dir.empty() ? exeDir() + L"\\driver" : dir; }

bool isElevated() {
    HANDLE t = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return false;
    TOKEN_ELEVATION e = {};
    DWORD n = 0;
    bool ok = GetTokenInformation(t, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
    CloseHandle(t);
    return ok;
}

// Opened without write sharing: a file somebody else is writing to cannot be read (fails closed).
bool readAll(const std::wstring& path, Bytes& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz = {};
    bool ok = GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && sz.QuadPart <= 16 * 1024 * 1024;
    if (ok) {
        out.resize((size_t)sz.QuadPart);
        size_t got = 0;
        while (ok && got < out.size()) {
            DWORD n = 0;
            ok = ReadFile(f, out.data() + got, (DWORD)(out.size() - got), &n, nullptr) && n > 0;
            got += n;
        }
    }
    CloseHandle(f);
    return ok;
}

// Reads every file of the package and compares it with its pin.  The checked bytes are kept: the protected copy is written from them, so what
// gets installed is exactly what was checked.  quiet: a missing file is not worth a log line (looking for a package that may not be there).
int loadPackage(const std::wstring& dir, std::vector<Bytes>& files, bool quiet = false) {
    files.assign(kPinCount, Bytes());
    for (size_t i = 0; i < kPinCount; ++i) {
        const std::wstring p = packageDir(dir) + L"\\" + kPins[i].rel;
        if (!readAll(p, files[i])) {
            if (!quiet) logf("driver: cannot read %s", narrow(p).c_str());
            return DRV_NO_PACKAGE;
        }
        const Bytes h = sha256(files[i]);
        const std::string hex = toHex(h.data(), h.size());
        if (hex != kPins[i].sha256) {
            logf("driver: %s is not the file this Padreen was built for (sha256 %s)", narrow(kPins[i].rel).c_str(), hex.c_str());
            return DRV_BAD_PACKAGE;
        }
    }
    return DRV_SUCCESS;
}

// A fresh folder below C:\Windows\Temp that only SYSTEM and administrators can use (ordinary users can create things there, but cannot touch
// what an administrator created): no process of an ordinary user can swap a file, or the folder, between the check and the use.
class Stage {
public:
    ~Stage() { remove(); }
    bool create(const std::vector<Bytes>& files) {
        wchar_t win[MAX_PATH];
        UINT wl = GetSystemWindowsDirectoryW(win, MAX_PATH);
        if (!wl || wl >= MAX_PATH) return false;
        uint8_t rnd[8];
        if (!randomBytes(rnd, sizeof(rnd))) return false;
        dir_ = std::wstring(win) + L"\\Temp\\Padreen-vdd-" + widen(toHex(rnd, sizeof(rnd)));
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1, &sd, nullptr)) return false;
        SECURITY_ATTRIBUTES sa = {sizeof(sa), sd, FALSE};
        bool ok = true;
        for (const std::wstring& d : {dir_, dir_ + L"\\driver"}) {
            ok = ok && CreateDirectoryW(d.c_str(), &sa) != FALSE;
            if (ok) dirs_.push_back(d);
        }
        for (size_t i = 0; ok && i < kPinCount; ++i) {
            const std::wstring p = dir_ + L"\\" + kPins[i].rel;
            HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE) {
                ok = false;
                break;
            }
            files_.push_back(p);
            size_t put = 0;
            while (ok && put < files[i].size()) {
                DWORD n = 0;
                ok = WriteFile(f, files[i].data() + put, (DWORD)(files[i].size() - put), &n, nullptr) && n > 0;
                put += n;
            }
            CloseHandle(f);
        }
        LocalFree(sd);
        return ok;
    }
    const std::wstring& dir() const { return dir_; }

private:
    void remove() {
        for (const std::wstring& f : files_) DeleteFileW(f.c_str());
        for (size_t i = dirs_.size(); i-- > 0;) RemoveDirectoryW(dirs_[i].c_str());
        files_.clear();
        dirs_.clear();
    }
    std::wstring dir_;
    std::vector<std::wstring> files_, dirs_;
};

// Runs a tool without a window and waits for it.  Returns its exit code (-1 = could not start, -2 = did not finish in time).
int runTool(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd, DWORD timeoutMs) {
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi)) {
        logf("driver: cannot start %s (error %lu)", narrow(exe).c_str(), GetLastError());
        return -1;
    }
    int rc = -2;
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        rc = (int)code;
    } else {
        TerminateProcess(pi.hProcess, 1);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return rc;
}

// The modes the driver offers for the tablet's panel (it reads them from HKLM\SOFTWARE\Parsec\vdd): landscape and portrait.
bool setMode(int index, DWORD w, DWORD h, DWORD hz) {
    HKEY k = nullptr;
    const std::wstring key = L"SOFTWARE\\Parsec\\vdd\\" + std::to_wstring(index);
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    const struct {
        const wchar_t* name;
        DWORD value;
    } vals[] = {{L"width", w}, {L"height", h}, {L"hz", hz}};
    bool ok = true;
    for (const auto& v : vals) ok = RegSetValueExW(k, v.name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v.value), sizeof(DWORD)) == ERROR_SUCCESS && ok;
    RegCloseKey(k);
    return ok;
}

std::string hashHex(const Bytes& data) {
    const Bytes h = sha256(data);
    return toHex(h.data(), h.size());
}

bool makeDirs(const std::wstring& dir) {
    for (size_t i = 3; i <= dir.size(); ++i) {              // from behind "C:\"
        if (i != dir.size() && dir[i] != L'\\') continue;
        const std::wstring part = dir.substr(0, i);
        if (!CreateDirectoryW(part.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    return true;
}

bool writeFile(const std::wstring& path, const Bytes& data) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    size_t put = 0;
    bool ok = true;
    while (ok && put < data.size()) {
        DWORD n = 0;
        ok = WriteFile(f, data.data() + put, (DWORD)(data.size() - put), &n, nullptr) && n > 0;
        put += n;
    }
    CloseHandle(f);
    return ok;
}

std::wstring httpErrorText(DWORD e) {
    switch (e) {
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: return L"找不到服务器（没有联网，或域名解析失败）";
        case ERROR_WINHTTP_TIMEOUT: return L"连接超时";
        case ERROR_WINHTTP_CANNOT_CONNECT: return L"连不上服务器";
        case ERROR_WINHTTP_CONNECTION_ERROR: return L"连接中断";
        case ERROR_WINHTTP_SECURE_FAILURE:
        case ERROR_WINHTTP_SECURE_INVALID_CERT:
        case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
        case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
        case ERROR_WINHTTP_SECURE_INVALID_CA:
        case ERROR_WINHTTP_SECURE_CHANNEL_ERROR: return L"安全连接失败（证书、代理或安全软件的问题）";
    }
    return L"错误 " + std::to_wstring(e);
}

// GET of an https:// address into memory (at most maxBytes).  detail = what went wrong, for the message box.  The system's proxy settings and
// certificate store are used (WinHTTP); every step has a time limit.
bool httpGet(const std::wstring& url, size_t maxBytes, Bytes& out, std::wstring& detail, const std::atomic<bool>* cancel) {
    out.clear();
    URL_COMPONENTS uc = {sizeof(uc)};
    wchar_t host[256] = {}, path[1024] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = (DWORD)(sizeof(host) / sizeof(host[0]));
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = (DWORD)(sizeof(path) / sizeof(path[0]));
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        detail = L"地址无效";
        return false;
    }
    struct Handle {
        HINTERNET h = nullptr;
        ~Handle() {
            if (h) WinHttpCloseHandle(h);
        }
    } ses, con, req;
    ses.h = WinHttpOpen(L"Padreen", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses.h) ses.h = WinHttpOpen(L"Padreen", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses.h) {
        detail = httpErrorText(GetLastError());
        return false;
    }
    WinHttpSetTimeouts(ses.h, 10000, 10000, 15000, 30000);          // resolve, connect, send, receive (milliseconds)
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(ses.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    con.h = WinHttpConnect(ses.h, host, uc.nPort, 0);
    if (con.h) req.h = WinHttpOpenRequest(con.h, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req.h || !WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(req.h, nullptr)) {
        detail = std::wstring(host) + L"：" + httpErrorText(GetLastError());
        return false;
    }
    DWORD code = 0, size = sizeof(code);
    if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX) || code != 200) {
        detail = std::wstring(host) + L" 返回 HTTP " + std::to_wstring(code);
        return false;
    }
    for (;;) {
        if (cancel && *cancel) {
            detail = L"已取消";
            return false;
        }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail)) {
            detail = L"下载中断：" + httpErrorText(GetLastError());
            return false;
        }
        if (avail == 0) return true;
        if (out.size() + avail > maxBytes) {
            detail = L"文件比预期的大";
            out.clear();
            return false;
        }
        const size_t at = out.size();
        out.resize(at + avail);
        DWORD got = 0;
        if (!WinHttpReadData(req.h, out.data() + at, avail, &got)) {
            detail = L"下载中断：" + httpErrorText(GetLastError());
            return false;
        }
        out.resize(at + got);
    }
}

// Gets the package into a folder the elevated installer can read; pkgDir is that folder.  detail = more about a failure (for the message box).
// Nothing downloaded or unpacked is trusted before it matched its SHA-256: the installers, then every file taken out of them.
int preparePackage(const std::wstring& cacheDir, const DriverStatusFn& status, const std::atomic<bool>* cancel, std::wstring& pkgDir, std::wstring& detail) {
    std::vector<Bytes> probe;
    if (loadPackage(L"", probe, true) == DRV_SUCCESS) {
        pkgDir = packageDir(L"");
        logf("driver: using the package next to the program (%s)", narrow(pkgDir).c_str());
        return DRV_SUCCESS;
    }
    const std::wstring unpacked = cacheDir + L"\\pkg";
    if (loadPackage(unpacked, probe, true) == DRV_SUCCESS) {
        pkgDir = unpacked;
        logf("driver: using the package unpacked earlier (%s)", narrow(pkgDir).c_str());
        return DRV_SUCCESS;
    }
    if (!makeDirs(cacheDir)) {
        logf("driver: cannot create %s (error %lu)", narrow(cacheDir).c_str(), GetLastError());
        detail = cacheDir;
        return DRV_EXTRACT_FAILED;
    }
    for (size_t s = 0; s < kSourceCount; ++s) {
        const Source& src = kSources[s];
        Bytes inst;
        const std::wstring kept = cacheDir + L"\\" + src.file;        // an installer from an earlier download, or put there by hand
        if (readAll(kept, inst) && hashHex(inst) == src.sha256) {
            logf("driver: %s found in the cache (checksum ok)", narrow(src.file).c_str());
        } else {
            if (cancel && *cancel) {
                detail = L"已取消";
                return DRV_DOWNLOAD_FAILED;
            }
            if (status) status(L"正在下载驱动…");
            logf("driver: downloading %s", narrow(src.url).c_str());
            if (!httpGet(src.url, 4u << 20, inst, detail, cancel)) {
                logf("driver: download failed: %s", narrow(detail).c_str());
                return DRV_DOWNLOAD_FAILED;
            }
            const std::string got = hashHex(inst);
            if (got != src.sha256) {
                logf("driver: %s has sha256 %s, expected %s: not used", narrow(src.file).c_str(), got.c_str(), src.sha256);
                return DRV_UNEXPECTED_FILE;
            }
            logf("driver: %s downloaded (%zu bytes, checksum ok)", narrow(src.file).c_str(), inst.size());
            if (!writeFile(kept, inst)) logf("driver: cannot keep a copy in %s (error %lu)", narrow(kept).c_str(), GetLastError());      // a copy for the next time, nothing more
        }
        if (status) status(L"正在解包驱动…");
        std::vector<size_t> idx;
        std::vector<std::string> wanted;
        for (size_t i = 0; i < kPinCount; ++i)
            if (kPins[i].src == s) {
                idx.push_back(i);
                wanted.push_back(kPins[i].sha256);
            }
        std::vector<Bytes> found;
        if (!nsisFindFiles(inst, wanted, found)) {
            logf("driver: the expected files are not in %s", narrow(src.file).c_str());
            return DRV_EXTRACT_FAILED;
        }
        for (size_t k = 0; k < idx.size(); ++k) {
            const std::wstring dst = unpacked + L"\\" + kPins[idx[k]].rel;
            if (!makeDirs(dst.substr(0, dst.find_last_of(L'\\'))) || !writeFile(dst, found[k])) {
                logf("driver: cannot write %s (error %lu)", narrow(dst).c_str(), GetLastError());
                detail = unpacked;
                return DRV_EXTRACT_FAILED;
            }
        }
    }
    if (loadPackage(unpacked, probe) != DRV_SUCCESS) return DRV_EXTRACT_FAILED;
    pkgDir = unpacked;
    logf("driver: the package is ready in %s", narrow(pkgDir).c_str());
    return DRV_SUCCESS;
}

// Runs `work` on another thread and keeps this thread's message loop going meanwhile: the tray program does not freeze during a download.
template <class F>
void runPumped(F&& work) {
    std::atomic<bool> done{false};
    std::thread t([&] {
        work();
        done = true;
    });
    bool quitSeen = false;
    int quitCode = 0;
    while (!done) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) {
                quitSeen = true;
                quitCode = (int)m.wParam;
                continue;
            }
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
    }
    t.join();
    if (quitSeen) PostQuitMessage(quitCode);        // the main message loop has to see it too
}

}  // namespace

std::wstring driverExitText(int code) {
    switch (code) {
        case DRV_SUCCESS: return L"成功";
        case DRV_NOT_ELEVATED: return L"没有管理员权限";
        case DRV_NO_PACKAGE: return L"缺少驱动文件";
        case DRV_BAD_PACKAGE: return L"驱动文件与预期不符";
        case DRV_STAGE_FAILED: return L"无法准备安装文件";
        case DRV_INSTALL_FAILED: return L"Windows 拒绝安装（可能被安全软件拦截）";
        case DRV_NOT_RUNNING: return L"驱动未启动，请重启电脑";
        case DRV_DOWNLOAD_FAILED: return L"无法下载驱动";
        case DRV_UNEXPECTED_FILE: return L"下载的文件与预期不符，已放弃";
        case DRV_EXTRACT_FAILED: return L"无法解出驱动文件";
    }
    return L"未知错误（代码 " + std::to_wstring(code) + L"）";
}

int verifyDriverPackage(const std::wstring& dir) {
    std::vector<Bytes> files;
    int rc = loadPackage(dir, files);
    logf("driver: package check -> %s", narrow(driverExitText(rc)).c_str());
    return rc;
}

int fetchDriverPackage(const std::wstring& cacheDir) {
    if (cacheDir.empty()) {
        logf("driver: --fetch-driver needs --cache <folder>");
        return DRV_NO_PACKAGE;
    }
    std::wstring pkg, detail;
    const int rc = preparePackage(cacheDir, [](const std::wstring& s) { logf("driver: %s", narrow(s).c_str()); }, nullptr, pkg, detail);
    logf("driver: fetch -> %s %s", narrow(driverExitText(rc)).c_str(), narrow(detail).c_str());
    return rc;
}

int runInstallDriver(const std::wstring& log, const std::wstring& dir) {
    // this process runs elevated and the paths come from the command line: the log is only ever appended to when it is a plain *.log file, and
    // the package folder is only read (every file is checked against its SHA-256 and the checked bytes are what gets installed)
    const wchar_t* ext = wcsrchr(log.c_str(), L'.');
    if (ext && _wcsicmp(ext, L".log") == 0) setLogFile(log.c_str());
    logf("---- driver install (elevated helper)");
    if (!isElevated()) {
        logf("driver: administrator rights are needed");
        return DRV_NOT_ELEVATED;
    }
    std::vector<Bytes> files;
    int rc = loadPackage(dir, files);
    if (rc != DRV_SUCCESS) return rc;
    logf("driver: the package is the expected one (%zu files)", files.size());
    Stage stage;
    if (!stage.create(files)) {
        logf("driver: cannot prepare the protected folder (error %lu)", GetLastError());
        return DRV_STAGE_FAILED;
    }
    const std::wstring nef = stage.dir() + L"\\nefconw.exe";
    const std::wstring inf = stage.dir() + L"\\driver\\mm.inf";
    if (!setMode(0, 1872, 1404, 60) || !setMode(1, 1404, 1872, 60)) {
        logf("driver: cannot write the display modes to HKLM\\SOFTWARE\\Parsec\\vdd (error %lu)", GetLastError());
        return DRV_INSTALL_FAILED;
    }
    // Parsec's own installer does the same three steps: drop an old device node, create the device, put the driver on it
    int c = runTool(nef, std::wstring(L"--remove-device-node ") + kDeviceArgs, stage.dir(), 120000);
    logf("driver: remove-device-node -> %d (an error here only means there was nothing to remove)", c);
    c = runTool(nef, std::wstring(L"--create-device-node --class-name Display ") + kDeviceArgs, stage.dir(), 120000);
    logf("driver: create-device-node -> %d", c);
    if (c != 0) return DRV_INSTALL_FAILED;
    c = runTool(nef, L"--install-driver --inf-path \"" + inf + L"\"", stage.dir(), 300000);
    logf("driver: install-driver -> %d", c);
    if (c != 0) {
        runTool(nef, std::wstring(L"--remove-device-node ") + kDeviceArgs, stage.dir(), 120000);   // do not leave a device without a driver behind
        return DRV_INSTALL_FAILED;
    }
    for (int i = 0; i < 40 && !Vdd::installed(); ++i) Sleep(500);
    if (!Vdd::installed()) {
        logf("driver: installed, but the adapter did not start");
        return DRV_NOT_RUNNING;
    }
    logf("driver: installed and running");
    return DRV_SUCCESS;
}

DriverAsk installDriverInteractive(HWND parent, const std::wstring& logPath, bool repair, const std::wstring& cacheDir, const DriverStatusFn& status,
                                   const std::atomic<bool>* cancel) {
    const wchar_t* title = L"虚拟显示器驱动";
    const UINT flags = MB_TOPMOST | MB_SETFOREGROUND;
    std::vector<Bytes> probe;
    const bool haveFiles = loadPackage(L"", probe, true) == DRV_SUCCESS || loadPackage(cacheDir + L"\\pkg", probe, true) == DRV_SUCCESS;
    // Short on purpose, but it says what matters: whose software it is, where it comes from, that it needs administrator rights.
    const std::wstring ask =
        repair ? std::wstring(L"重新安装虚拟显示器驱动？\n需要管理员授权；重装期间请不要使用平板屏。")
               : std::wstring(L"需要安装「虚拟显示器」驱动（Parsec 出品）。\n") +
                     (haveFiles ? L"需要管理员授权。" : L"将从 Parsec 官网下载（约 0.7 MB）并安装，需要管理员授权。") + L"\n\n现在安装？";
    if (MessageBoxW(parent, ask.c_str(), title, MB_YESNO | MB_ICONQUESTION | flags) != IDYES) return DriverAsk::Declined;

    // the package: next to the program, unpacked earlier, or downloaded now (the message loop keeps running meanwhile)
    std::wstring pkg, detail;
    int prep = DRV_SUCCESS;
    runPumped([&] { prep = preparePackage(cacheDir, status, cancel, pkg, detail); });
    if (prep != DRV_SUCCESS) {
        logf("driver: no package (%d)", prep);
        if (cancel && *cancel) return DriverAsk::Failed;               // the program is quitting
        std::wstring msg = driverExitText(prep);
        if (prep == DRV_DOWNLOAD_FAILED) {
            msg += L"：" + detail + L"\n\n请检查网络后重试。也可以手动下载下面两个文件，放进\n" + cacheDir + L"\n后再试：\n";
            for (const Source& s : kSources) msg += std::wstring(L"    ") + s.url + L"\n";
        } else if (prep == DRV_UNEXPECTED_FILE) {
            msg += L"\n请更新 Padreen：https://github.com/1031193940/Padreen/releases";
        } else if (prep == DRV_EXTRACT_FAILED && !detail.empty()) {
            msg += L"：" + detail;
        }
        MessageBoxW(parent, msg.c_str(), title, MB_OK | MB_ICONWARNING | flags);
        return DriverAsk::Failed;
    }

    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, (DWORD)(sizeof(exe) / sizeof(exe[0])));
    const std::wstring dir = exeDir();
    const std::wstring args = L"--install-driver --package \"" + pkg + L"\" --log \"" + logPath + L"\"";
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    // A thread that has not initialized COM gets an ShellExecuteEx that hands the work to a background thread and returns at once, without a
    // process handle (the UAC prompt would still be open when we look for the result): initialize COM here and ask to wait.
    const bool comReady = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.hwnd = parent;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = args.c_str();
    sei.lpDirectory = dir.c_str();
    sei.nShow = SW_HIDE;
    logf("driver: asking Windows for administrator rights to install the virtual display driver");
    const BOOL started = ShellExecuteExW(&sei);
    const DWORD startError = GetLastError();
    if (comReady) CoUninitialize();
    if (!started) {
        logf("driver: elevation refused or failed (%lu)", (unsigned long)startError);
        MessageBoxW(parent, startError == ERROR_CANCELLED ? L"未获得管理员授权，驱动没有安装。" : L"无法提权，驱动没有安装。", title, MB_OK | MB_ICONWARNING | flags);
        return DriverAsk::Failed;
    }
    DWORD code = DRV_INSTALL_FAILED;
    if (sei.hProcess) {
        logf("driver: the elevated installer is running");
        if (WaitForSingleObject(sei.hProcess, 5 * 60 * 1000) == WAIT_OBJECT_0) GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
        logf("driver: the elevated installer finished (exit code %lu)", (unsigned long)code);
    } else {
        logf("driver: no process handle for the elevated installer: waiting for the driver to appear");
        for (int i = 0; i < 240 && !Vdd::installed(); ++i) Sleep(500);   // the UAC prompt and the installation: up to two minutes
    }
    for (int i = 0; i < 20 && !Vdd::installed(); ++i) Sleep(500);   // the adapter may need a moment after the installer has returned
    if (Vdd::installed()) {
        logf("driver: the virtual display driver is installed and running");
        MessageBoxW(parent, L"驱动已安装。", title, MB_OK | MB_ICONINFORMATION | flags);
        return DriverAsk::Installed;
    }
    logf("driver: installation failed (%lu)", (unsigned long)code);
    const std::wstring msg = L"驱动安装失败：" + driverExitText((int)code);
    MessageBoxW(parent, msg.c_str(), title, MB_OK | MB_ICONERROR | flags);
    return DriverAsk::Failed;
}

}  // namespace ph
