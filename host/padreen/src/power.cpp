#include "power.h"

#include <powrprof.h>
#include <sddl.h>
#include <winsvc.h>

namespace ph {
namespace {

const wchar_t* kServiceName = L"PadreenPower";
const wchar_t* kHoldEvent = L"Global\\PadreenLidHold";
const wchar_t* kReleaseEvent = L"Global\\PadreenLidRelease";
// the lid-close action: SUB_BUTTONS / LIDACTION (hidden on some devices, still readable and writable by GUID)
const GUID kSubButtons = {0x4f971e89, 0xeebd, 0x4455, {0xa8, 0xde, 0x9e, 0x59, 0x04, 0x0e, 0x73, 0x47}};
const GUID kLidClose = {0x5ca83367, 0x6e45, 0x459f, {0xa2, 0x7b, 0x47, 0x6b, 0x1d, 0x01, 0xc9, 0x36}};
const DWORD kHoldTimeoutMs = 15000;

std::wstring guidToString(const GUID& g) {
    wchar_t b[64];
    StringFromGUID2(g, b, 64);
    return b;
}

bool stringToGuid(const std::wstring& s, GUID& g) { return CLSIDFromString(s.c_str(), &g) == S_OK; }

}  // namespace

// ---------------------------------------------------------------------------------------------------------------- session power request
SessionPower::SessionPower(const wchar_t* reason) {
    REASON_CONTEXT rc = {};
    rc.Version = POWER_REQUEST_CONTEXT_VERSION;
    rc.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    rc.Reason.SimpleReasonString = const_cast<LPWSTR>(reason);
    HANDLE h = PowerCreateRequest(&rc);
    if (h == INVALID_HANDLE_VALUE || !h) return;
    PowerSetRequest(h, PowerRequestSystemRequired);          // no idle sleep
    PowerSetRequest(h, PowerRequestExecutionRequired);       // keeps running through Modern Standby
    h_ = h;
}

void SessionPower::displayRequired(bool on) {
    if (!h_ || on == display_) return;
    if (on) PowerSetRequest(h_, PowerRequestDisplayRequired);
    else PowerClearRequest(h_, PowerRequestDisplayRequired);
    display_ = on;
}

SessionPower::~SessionPower() {
    if (!h_) return;
    PowerClearRequest(h_, PowerRequestDisplayRequired);
    PowerClearRequest(h_, PowerRequestSystemRequired);
    PowerClearRequest(h_, PowerRequestExecutionRequired);
    CloseHandle(h_);
}

// ---------------------------------------------------------------------------------------------------------------- lid hold (client side)
void LidHold::start() {
    if (run_.exchange(true)) return;
    th_ = std::thread([this] {
        HANDLE hold = nullptr;
        bool logged = false;
        while (run_) {
            if (!hold) hold = OpenEventW(EVENT_MODIFY_STATE, FALSE, kHoldEvent);
            if (hold) {
                SetEvent(hold);
                if (!logged) {
                    logged = true;
                    logf("lid: power helper reached - closing the lid will not put the PC to sleep during this session");
                }
            } else if (!logged) {
                logged = true;
                logf("lid: power helper service is not installed/running - closing the lid may put the PC to sleep");
            }
            for (int i = 0; i < 30 && run_; ++i) Sleep(100);   // heartbeat every 3 s
        }
        if (hold) CloseHandle(hold);
        HANDLE rel = OpenEventW(EVENT_MODIFY_STATE, FALSE, kReleaseEvent);
        if (rel) {
            SetEvent(rel);
            CloseHandle(rel);
        }
    });
}

void LidHold::stop() {
    if (!run_.exchange(false)) return;
    if (th_.joinable()) th_.join();
}

// ---------------------------------------------------------------------------------------------------------------- queries
PowerHelper powerHelperState() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return PowerHelper::Missing;
    PowerHelper r = PowerHelper::Missing;
    if (SC_HANDLE sv = OpenServiceW(scm, kServiceName, SERVICE_QUERY_STATUS)) {
        SERVICE_STATUS st = {};
        r = PowerHelper::Stopped;
        if (QueryServiceStatus(sv, &st) && st.dwCurrentState == SERVICE_RUNNING) r = PowerHelper::Running;
        CloseServiceHandle(sv);
    }
    CloseServiceHandle(scm);
    return r;
}

int lidActionNow() {
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme) return -1;
    DWORD v = 0;
    bool ac = true;
    SYSTEM_POWER_STATUS sps = {};
    if (GetSystemPowerStatus(&sps) && sps.ACLineStatus == 0) ac = false;      // on battery
    DWORD rc = ac ? PowerReadACValueIndex(nullptr, scheme, &kSubButtons, &kLidClose, &v) : PowerReadDCValueIndex(nullptr, scheme, &kSubButtons, &kLidClose, &v);
    LocalFree(scheme);
    return rc == ERROR_SUCCESS ? (int)v : -1;
}

std::wstring lidActionName(int a) {
    switch (a) {
        case 0: return L"不采取任何操作";
        case 1: return L"睡眠";
        case 2: return L"休眠";
        case 3: return L"关机";
    }
    return L"未知";
}

// ---------------------------------------------------------------------------------------------------------------- service side
namespace {

std::wstring stateFilePath(const wchar_t* folder = L"Padreen", bool create = true) {
    wchar_t pd[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH)) wcscpy_s(pd, L"C:\\ProgramData");
    std::wstring dir = std::wstring(pd) + L"\\" + folder;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, FALSE};
    if (create && ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)(A;OICI;GR;;;BU)", SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr)) {
        CreateDirectoryW(dir.c_str(), &sa);         // only SYSTEM / administrators may write the saved original values
        LocalFree(sa.lpSecurityDescriptor);
    }
    return dir + L"\\lid-state.ini";
}

// Set the lid action of the active plan to "do nothing" and remember the original values on disk first.
bool lidApply() {
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme) return false;
    DWORD ac = 0, dc = 0;
    PowerReadACValueIndex(nullptr, scheme, &kSubButtons, &kLidClose, &ac);
    PowerReadDCValueIndex(nullptr, scheme, &kSubButtons, &kLidClose, &dc);
    std::wstring f = stateFilePath();
    WritePrivateProfileStringW(L"lid", L"scheme", guidToString(*scheme).c_str(), f.c_str());
    WritePrivateProfileStringW(L"lid", L"ac", std::to_wstring(ac).c_str(), f.c_str());
    WritePrivateProfileStringW(L"lid", L"dc", std::to_wstring(dc).c_str(), f.c_str());
    if (ac != 0) PowerWriteACValueIndex(nullptr, scheme, &kSubButtons, &kLidClose, 0);
    if (dc != 0) PowerWriteDCValueIndex(nullptr, scheme, &kSubButtons, &kLidClose, 0);
    PowerSetActiveScheme(nullptr, scheme);
    LocalFree(scheme);
    return true;
}

// Put the saved original values back (also after a crash / reboot: the state file survives) and forget them.  `folder` = the ProgramData
// subfolder that holds the state file (the old product name's folder is checked too: a crash of the old service must not leave the lid action changed).
void lidRestore(const wchar_t* folder = L"Padreen", bool create = true) {
    std::wstring f = stateFilePath(folder, create);
    if (GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    wchar_t buf[128] = {};
    GetPrivateProfileStringW(L"lid", L"scheme", L"", buf, 128, f.c_str());
    GUID scheme;
    if (stringToGuid(buf, scheme)) {
        DWORD ac = GetPrivateProfileIntW(L"lid", L"ac", 0, f.c_str());
        DWORD dc = GetPrivateProfileIntW(L"lid", L"dc", 1, f.c_str());
        PowerWriteACValueIndex(nullptr, &scheme, &kSubButtons, &kLidClose, ac);
        PowerWriteDCValueIndex(nullptr, &scheme, &kSubButtons, &kLidClose, dc);
        GUID* active = nullptr;
        if (PowerGetActiveScheme(nullptr, &active) == ERROR_SUCCESS && active) {
            PowerSetActiveScheme(nullptr, active);          // apply (the restored plan may or may not still be the active one)
            LocalFree(active);
        }
    }
    DeleteFileW(f.c_str());
}

SERVICE_STATUS_HANDLE g_ssh = nullptr;
SERVICE_STATUS g_ss = {};
HANDLE g_stop = nullptr;

void setState(DWORD state) {
    g_ss.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ss.dwCurrentState = state;
    g_ss.dwControlsAccepted = state == SERVICE_RUNNING ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0;
    SetServiceStatus(g_ssh, &g_ss);
}

void WINAPI serviceCtrl(DWORD ctl) {
    if (ctl == SERVICE_CONTROL_STOP || ctl == SERVICE_CONTROL_SHUTDOWN) {
        setState(SERVICE_STOP_PENDING);
        if (g_stop) SetEvent(g_stop);
    }
}

HANDLE makeEvent(const wchar_t* name) {
    // SYSTEM / administrators: everything; every signed-in user: signal + wait (EVENT_MODIFY_STATE | SYNCHRONIZE)
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, FALSE};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00100002;;;AU)", SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr)) return nullptr;
    HANDLE h = CreateEventW(&sa, FALSE, FALSE, name);
    LocalFree(sa.lpSecurityDescriptor);
    return h;
}

void WINAPI serviceMain(DWORD, LPWSTR*) {
    g_ssh = RegisterServiceCtrlHandlerW(kServiceName, serviceCtrl);
    if (!g_ssh) return;
    setState(SERVICE_START_PENDING);
    lidRestore();                                          // crash / power loss in the middle of a session: back to the user's values first
    lidRestore(L"PaperDisplay", false);                    // (the same for a session of the old PaperDisplay service)
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE hold = makeEvent(kHoldEvent), rel = makeEvent(kReleaseEvent);
    if (!g_stop || !hold || !rel) {
        setState(SERVICE_STOPPED);
        return;
    }
    setState(SERVICE_RUNNING);
    bool overridden = false;
    HANDLE hs[3] = {g_stop, rel, hold};
    for (;;) {
        DWORD r = WaitForMultipleObjects(3, hs, FALSE, overridden ? kHoldTimeoutMs : INFINITE);
        if (r == WAIT_OBJECT_0) break;                                         // service stop / shutdown
        if (r == WAIT_OBJECT_0 + 1 || r == WAIT_TIMEOUT) {                     // released, or the heartbeats stopped
            if (overridden) {
                lidRestore();
                overridden = false;
            }
        } else if (r == WAIT_OBJECT_0 + 2) {                                   // heartbeat
            if (!overridden) overridden = lidApply();
        } else {
            break;
        }
    }
    if (overridden) lidRestore();
    CloseHandle(hold);
    CloseHandle(rel);
    CloseHandle(g_stop);
    setState(SERVICE_STOPPED);
}

}  // namespace

int runPowerService() {
    wchar_t name[64];
    wcscpy_s(name, kServiceName);
    SERVICE_TABLE_ENTRYW table[] = {{name, serviceMain}, {nullptr, nullptr}};
    return StartServiceCtrlDispatcherW(table) ? 0 : 1;
}

int installPowerService() {
    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, (DWORD)(sizeof(exe) / sizeof(exe[0])));
    std::wstring bin = L"\"" + std::wstring(exe) + L"\" --power-service";
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) return 2;                                     // not elevated
    int rc = 0;
    SC_HANDLE sv = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
    if (sv) {
        ChangeServiceConfigW(sv, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, bin.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    } else {
        sv = CreateServiceW(scm, kServiceName, L"Padreen 电源助手", SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, bin.c_str(),
                            nullptr, nullptr, nullptr, nullptr, nullptr);
    }
    if (!sv) {
        CloseServiceHandle(scm);
        return 3;
    }
    wchar_t desc[] = L"Padreen 连接平板期间合盖不休眠，结束后自动还原。";
    SERVICE_DESCRIPTIONW d = {desc};
    ChangeServiceConfig2W(sv, SERVICE_CONFIG_DESCRIPTION, &d);
    SC_ACTION acts[1] = {{SC_ACTION_RESTART, 5000}};
    SERVICE_FAILURE_ACTIONSW fa = {86400, nullptr, nullptr, 1, acts};
    ChangeServiceConfig2W(sv, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
    SERVICE_STATUS st = {};
    if (QueryServiceStatus(sv, &st) && st.dwCurrentState != SERVICE_RUNNING) {
        if (!StartServiceW(sv, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) rc = 4;
    }
    CloseServiceHandle(sv);
    CloseServiceHandle(scm);
    return rc;
}

int removePowerService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) return 2;
    int rc = 0;
    if (SC_HANDLE sv = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS)) {
        SERVICE_STATUS st = {};
        ControlService(sv, SERVICE_CONTROL_STOP, &st);       // stopping restores the lid action
        for (int i = 0; i < 50 && QueryServiceStatus(sv, &st) && st.dwCurrentState != SERVICE_STOPPED; ++i) Sleep(200);
        if (!DeleteService(sv)) rc = 4;
        CloseServiceHandle(sv);
    }
    CloseServiceHandle(scm);
    return rc;
}

}  // namespace ph
