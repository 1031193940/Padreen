#include "adb.h"

#include <algorithm>

namespace ph {

// The adb server's port: ANDROID_ADB_SERVER_PORT, exactly as adb itself reads it (5037 by default).
static int serverPort() {
    char v[16] = {};
    DWORD n = GetEnvironmentVariableA("ANDROID_ADB_SERVER_PORT", v, sizeof(v));
    int p = n && n < sizeof(v) ? atoi(v) : 0;
    return p > 0 && p < 65536 ? p : 5037;
}

static bool serverUp() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)serverPort());
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    bool ok = ::connect(s, (sockaddr*)&a, sizeof(a)) == 0;
    closesocket(s);
    return ok;
}

bool Adb::ensureServer(std::string& err) {
    if (serverUp()) return true;
    std::wstring cmd = L"\"" + exe_ + L"\" -P " + std::to_wstring(serverPort()) + L" start-server";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
        err = "cannot start adb.exe (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    WaitForSingleObject(pi.hProcess, 8000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    for (int i = 0; i < 40; ++i) {
        if (serverUp()) return true;
        Sleep(100);
    }
    err = "adb server did not come up";
    return false;
}

int Adb::run(const std::wstring& args, std::string& out, int timeoutMs) {
    out.clear();
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    std::wstring cmd = L"\"" + exe_ + L"\" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return -1;
    }
    std::string text;
    int64_t deadline = nowNs() + (int64_t)timeoutMs * 1000000LL;
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail) {
            char tmp[4096];
            DWORD got = 0;
            if (ReadFile(rd, tmp, (DWORD)std::min<DWORD>(avail, sizeof(tmp)), &got, nullptr) && got) text.append(tmp, got);
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
            // drain what is left
            while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail) {
                char tmp[4096];
                DWORD got = 0;
                if (!ReadFile(rd, tmp, (DWORD)std::min<DWORD>(avail, sizeof(tmp)), &got, nullptr) || !got) break;
                text.append(tmp, got);
            }
            break;
        }
        if (nowNs() > deadline || (abort_ && *abort_)) {
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            CloseHandle(rd);
            out = text;
            return -1;
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    out = text;
    return (int)code;
}

std::string Adb::shell(const std::string& serial, const std::string& cmd, int timeoutMs) {
    std::string out;
    run(L"-s " + widen(serial) + L" shell " + widen(cmd), out, timeoutMs);
    return out;
}

bool Adb::forward(const std::string& serial, int port) {
    std::string out;
    std::wstring p = std::to_wstring(port);
    return run(L"-s " + widen(serial) + L" forward tcp:" + p + L" tcp:" + p, out) == 0;
}

bool Adb::removeForward(const std::string& serial, int port) {
    std::string out;
    return run(L"-s " + widen(serial) + L" forward --remove tcp:" + std::to_wstring(port), out) == 0;
}

bool Adb::packageInstalled(const std::string& serial, const std::string& pkg) {
    std::string o = shell(serial, "pm path " + pkg);
    return o.find("package:") != std::string::npos;
}

bool Adb::install(const std::string& serial, const std::wstring& apk, std::string& log) {
    int rc = run(L"-s " + widen(serial) + L" install -r \"" + apk + L"\"", log, 600000);   // the tablet asks the user to confirm: be patient
    return rc == 0 && log.find("Success") != std::string::npos;
}

std::wstring explainInstallFailure(const std::string& out) {
    auto has = [&](const char* s) { return out.find(s) != std::string::npos; };
    if (has("INSTALL_FAILED_UPDATE_INCOMPATIBLE") || has("signatures do not match")) return L"平板上的 Padreen 签名不同，请先在平板上卸载它";
    if (has("INSTALL_FAILED_VERSION_DOWNGRADE")) return L"平板上的 Padreen 更新，无法降级";
    if (has("INSUFFICIENT_STORAGE")) return L"平板存储空间不足";
    if (has("INSTALL_FAILED_OLDER_SDK")) return L"平板系统版本太低";
    if (has("USER_RESTRICTED") || has("INSTALL_FAILED_ABORTED") || has("INSTALL_CANCELED_BY_USER") || has("canceled by user") || has("User rejected"))
        return L"平板上没有确认安装，请重新插线并点「继续安装」";
    if (has("device unauthorized")) return L"请在平板上点「允许 USB 调试」";
    if (has("device offline")) return L"USB 离线，请重新插拔数据线";
    if (has("no devices/emulators") || has("error: device '")) return L"平板已断开";
    // anything else: the last line adb printed is the one that names the problem
    std::string last;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t nl = out.find('\n', pos);
        std::string line = out.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? out.size() : nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) last = line;
    }
    if (last.empty()) return L"没有收到安装结果，请重新插线";
    if (last.size() > 200) last.resize(200);
    return L"adb：" + widen(last);
}

bool Adb::uninstall(const std::string& serial, const std::string& pkg, std::string& log) {
    int rc = run(L"-s " + widen(serial) + L" uninstall " + widen(pkg), log, 60000);
    return rc == 0 && log.find("Success") != std::string::npos;
}

std::string Adb::wifiAddress(const std::string& serial) {
    std::string o = shell(serial, "ip -f inet addr show wlan0");
    size_t p = o.find("inet ");
    if (p == std::string::npos) return "";
    p += 5;
    size_t e = p;
    while (e < o.size() && (isdigit((unsigned char)o[e]) || o[e] == '.')) ++e;
    std::string ip = o.substr(p, e - p);
    return ip.find('.') == std::string::npos ? "" : ip;
}
bool Adb::startActivity(const std::string& serial, const std::string& component) {
    std::string o = shell(serial, "am start -n " + component);
    return o.find("Error") == std::string::npos;
}

bool Adb::forceStop(const std::string& serial, const std::string& pkg) {
    shell(serial, "am force-stop " + pkg);
    return true;
}

// ------------------------------------------------------------------------------------------------ device watcher
static bool readExact(SOCKET s, char* p, int n) {
    while (n > 0) {
        int r = recv(s, p, n, 0);
        if (r <= 0) return false;
        p += r;
        n -= r;
    }
    return true;
}

void DeviceWatcher::start(Adb* adb, Callback onChange) {
    stop();
    adb_ = adb;
    cb_ = std::move(onChange);
    run_ = true;
    th_ = std::thread([this] { loop(); });
}

void DeviceWatcher::stop() {
    run_ = false;
    SOCKET s = sock_.exchange(INVALID_SOCKET);
    if (s != INVALID_SOCKET) {
        shutdown(s, SD_BOTH);
        closesocket(s);
    }
    if (th_.joinable()) th_.join();
}

void DeviceWatcher::loop() {
    while (run_) {
        std::string err;
        if (!adb_->ensureServer(err)) {
            logf("adb: %s", err.c_str());
            Sleep(2000);
            continue;
        }
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_port = htons((u_short)serverPort());
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (s == INVALID_SOCKET || ::connect(s, (sockaddr*)&a, sizeof(a)) != 0) {
            if (s != INVALID_SOCKET) closesocket(s);
            Sleep(1000);
            continue;
        }
        sock_ = s;
        const char* req = "0012host:track-devices";
        send(s, req, (int)strlen(req), 0);
        char ok[4];
        if (!readExact(s, ok, 4) || memcmp(ok, "OKAY", 4) != 0) {
            closesocket(s);
            sock_ = INVALID_SOCKET;
            Sleep(1000);
            continue;
        }
        while (run_) {
            char lenHex[5] = {0};
            if (!readExact(s, lenHex, 4)) break;
            int len = (int)strtol(lenHex, nullptr, 16);
            std::string body(len, '\0');
            if (len && !readExact(s, &body[0], len)) break;
            std::vector<std::string> serials;
            std::vector<AdbPending> pending;
            size_t pos = 0;
            while (pos < body.size()) {
                size_t nl = body.find('\n', pos);
                std::string line = body.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
                pos = nl == std::string::npos ? body.size() : nl + 1;
                size_t tab = line.find('\t');
                if (tab == std::string::npos) continue;
                std::string serial = line.substr(0, tab), state = line.substr(tab + 1);
                if (state == "device") serials.push_back(serial);
                else pending.push_back({serial, state});
            }
            if (cb_) cb_(serials, pending);
        }
        SOCKET cur = sock_.exchange(INVALID_SOCKET);
        if (cur != INVALID_SOCKET) closesocket(cur);
        if (run_) Sleep(500);
    }
}

}  // namespace ph
