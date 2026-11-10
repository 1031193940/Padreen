#include "common.h"

#include <avrt.h>

namespace ph {

static std::mutex g_logMu;
static FILE* g_log = nullptr;
static int64_t g_t0 = 0;

int64_t nowNs() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    int64_t sec = c.QuadPart / freq.QuadPart;
    int64_t rem = c.QuadPart % freq.QuadPart;
    return sec * 1000000000LL + rem * 1000000000LL / freq.QuadPart;
}

void setLogFile(const wchar_t* path) {
    std::lock_guard<std::mutex> lk(g_logMu);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 2u * 1024 * 1024) {   // keep the log small: one old copy
        std::wstring old = std::wstring(path) + L".old";
        MoveFileExW(path, old.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    if (g_log) fclose(g_log);
    g_log = _wfsopen(path, L"ab", _SH_DENYNO);
    g_t0 = nowNs();
}

void logf(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[2200];
    snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    std::lock_guard<std::mutex> lk(g_logMu);
    if (g_log) {
        fputs(line, g_log);
        fflush(g_log);
    }
    OutputDebugStringA(line);
    fputs(line, stderr);
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring exeDir() {
    wchar_t p[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, p, (DWORD)(sizeof(p) / sizeof(p[0])));
    std::wstring s(p, n);
    size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}

namespace {
struct ProcessPowerThrottlingState {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
};
}  // namespace

std::string boostProcess() {
    // PROCESS_INFORMATION_CLASS::ProcessPowerThrottling == 4
    ProcessPowerThrottlingState st{1, 0x1 /*EXECUTION_SPEED*/ | 0x4 /*IGNORE_TIMER_RESOLUTION*/, 0};
    typedef BOOL(WINAPI * SetProcInfo)(HANDLE, int, LPVOID, DWORD);
    auto fn = (SetProcInfo)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation");
    bool okPt = fn && fn(GetCurrentProcess(), 4, &st, sizeof(st));
    typedef UINT(WINAPI * TimeBegin)(UINT);
    HMODULE winmm = LoadLibraryW(L"winmm.dll");
    auto tb = winmm ? (TimeBegin)GetProcAddress(winmm, "timeBeginPeriod") : nullptr;
    bool okT = tb && tb(1) == 0;
    bool okP = SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS) != 0;
    char buf[128];
    snprintf(buf, sizeof(buf), "power-throttling-off=%d timer-1ms=%d high-priority=%d", okPt, okT, okP);
    return buf;
}

void* beginThreadBoost(const wchar_t* task) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    DWORD idx = 0;
    HANDLE h = AvSetMmThreadCharacteristicsW(task, &idx);
    return h;
}

void endThreadBoost(void* h) {
    if (h) AvRevertMmThreadCharacteristics((HANDLE)h);
}

}  // namespace ph
