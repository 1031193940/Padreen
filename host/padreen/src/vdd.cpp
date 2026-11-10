#include "vdd.h"

#include <setupapi.h>

namespace ph {

static const GUID kVddGuid = {0x00b41627, 0x04c4, 0x429e, {0xa2, 0x6e, 0x02, 0x65, 0xcf, 0x50, 0xc8, 0xfa}};
static const DWORD IOCTL_ADD = 0x0022e004, IOCTL_REMOVE = 0x0022a008, IOCTL_UPDATE = 0x0022a00c, IOCTL_VERSION = 0x0022e010;

bool Vdd::open() {
    HDEVINFO dev = SetupDiGetClassDevsA(&kVddGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (dev == INVALID_HANDLE_VALUE) return false;
    HANDLE found = INVALID_HANDLE_VALUE;
    for (DWORD i = 0;; ++i) {
        SP_DEVICE_INTERFACE_DATA idata;
        ZeroMemory(&idata, sizeof(idata));
        idata.cbSize = sizeof(idata);
        if (!SetupDiEnumDeviceInterfaces(dev, nullptr, &kVddGuid, i, &idata)) break;
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailA(dev, &idata, nullptr, 0, &need, nullptr);
        std::vector<char> buf(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_A*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!SetupDiGetDeviceInterfaceDetailA(dev, &idata, detail, need, &need, nullptr)) continue;
        HANDLE h = CreateFileA(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            found = h;
            break;
        }
    }
    SetupDiDestroyDeviceInfoList(dev);
    if (found == INVALID_HANDLE_VALUE) return false;
    h_ = found;
    run_ = true;
    ping_ = std::thread([this] {
        while (run_) {
            ioctl(IOCTL_UPDATE, nullptr, 0);
            Sleep(50);
        }
    });
    return true;
}

bool Vdd::installed() {
    HDEVINFO dev = SetupDiGetClassDevsA(&kVddGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (dev == INVALID_HANDLE_VALUE) return false;
    SP_DEVICE_INTERFACE_DATA idata;
    ZeroMemory(&idata, sizeof(idata));
    idata.cbSize = sizeof(idata);
    bool found = SetupDiEnumDeviceInterfaces(dev, nullptr, &kVddGuid, 0, &idata) != FALSE;
    SetupDiDestroyDeviceInfoList(dev);
    return found;
}

int Vdd::ioctl(DWORD code, const void* in, size_t inSize) {
    if (h_ == INVALID_HANDLE_VALUE) return -1;
    BYTE inbuf[32] = {0}, outbuf[32] = {0};
    if (in && inSize) memcpy(inbuf, in, inSize > sizeof(inbuf) ? sizeof(inbuf) : inSize);
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    DWORD got = 0;
    BOOL ok = DeviceIoControl(h_, code, inbuf, sizeof(inbuf), outbuf, sizeof(outbuf), &got, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) GetOverlappedResultEx(h_, &ov, &got, 5000, FALSE);
    CloseHandle(ov.hEvent);
    return *reinterpret_cast<int*>(outbuf);
}

int Vdd::addDisplay() {
    std::lock_guard<std::mutex> lk(mu_);
    int idx = ioctl(IOCTL_ADD, nullptr, 0);
    ioctl(IOCTL_UPDATE, nullptr, 0);
    if (idx >= 0) added_.push_back(idx);
    return idx;
}

void Vdd::removeDisplay(int idx) {
    std::lock_guard<std::mutex> lk(mu_);
    UINT16 be = (UINT16)(((idx & 0xFF) << 8) | ((idx >> 8) & 0xFF));
    ioctl(IOCTL_REMOVE, &be, sizeof(be));
    ioctl(IOCTL_UPDATE, nullptr, 0);
    for (size_t i = 0; i < added_.size(); ++i)
        if (added_[i] == idx) {
            added_.erase(added_.begin() + i);
            break;
        }
}

int Vdd::version() { return ioctl(IOCTL_VERSION, nullptr, 0); }

void Vdd::close() {
    if (h_ == INVALID_HANDLE_VALUE) return;
    std::vector<int> mine;
    {
        std::lock_guard<std::mutex> lk(mu_);
        mine = added_;
    }
    for (size_t i = mine.size(); i-- > 0;) removeDisplay(mine[i]);   // unplug right-to-left (Windows caches display combinations)
    run_ = false;
    if (ping_.joinable()) ping_.join();
    CloseHandle(h_);
    h_ = INVALID_HANDLE_VALUE;
}

// ------------------------------------------------------------------------------------------------ Windows display helpers
int attachedRealDisplays() {
    int n = 0;
    for (DWORD i = 0;; ++i) {
        DISPLAY_DEVICEW a;
        ZeroMemory(&a, sizeof(a));
        a.cb = sizeof(a);
        if (!EnumDisplayDevicesW(nullptr, i, &a, 0)) break;
        if (!(a.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) || (a.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER)) continue;
        DISPLAY_DEVICEW m;
        ZeroMemory(&m, sizeof(m));
        m.cb = sizeof(m);
        std::wstring id;
        if (EnumDisplayDevicesW(a.DeviceName, 0, &m, 1)) id = m.DeviceID;
        for (auto& c : id) c = (wchar_t)towupper(c);
        if (id.find(L"PSCCDD0") != std::wstring::npos || wcsstr(a.DeviceString, L"Parsec")) continue;
        ++n;
    }
    return n;
}
bool findVirtualDisplay(std::wstring& gdiName) {
    for (DWORD i = 0;; ++i) {
        DISPLAY_DEVICEW a;
        ZeroMemory(&a, sizeof(a));
        a.cb = sizeof(a);
        if (!EnumDisplayDevicesW(nullptr, i, &a, 0)) break;
        if (!(a.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        DISPLAY_DEVICEW m;
        ZeroMemory(&m, sizeof(m));
        m.cb = sizeof(m);
        std::wstring id;
        if (EnumDisplayDevicesW(a.DeviceName, 0, &m, 1)) id = m.DeviceID;
        std::wstring up = id;
        for (auto& c : up) c = (wchar_t)towupper(c);
        if (up.find(L"PSCCDD0") != std::wstring::npos || wcsstr(a.DeviceString, L"Parsec")) {
            gdiName = a.DeviceName;
            return true;
        }
    }
    return false;
}

bool setDisplayMode(const std::wstring& gdiName, int w, int h, int hz) {
    DEVMODEW dm;
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(gdiName.c_str(), ENUM_CURRENT_SETTINGS, &dm)) return false;
    dm.dmPelsWidth = w;
    dm.dmPelsHeight = h;
    dm.dmDisplayFrequency = hz;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_POSITION;
    // CDS_UPDATEREGISTRY: remembered for this virtual monitor only (it is a new, separate monitor entry)
    LONG r = ChangeDisplaySettingsExW(gdiName.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY, nullptr);
    if (r == DISP_CHANGE_SUCCESSFUL) return true;
    // the new shape may overlap a neighbour at the old position (the tablet was turned): let Windows place the monitor itself
    logf("setDisplayMode: %dx%d at the old position failed (%ld), retrying without a position", w, h, (long)r);
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    return ChangeDisplaySettingsExW(gdiName.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY, nullptr) == DISP_CHANGE_SUCCESSFUL;
}

bool displayRect(const std::wstring& gdiName, RECT& rc) {
    DEVMODEW dm;
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(gdiName.c_str(), ENUM_CURRENT_SETTINGS, &dm)) return false;
    rc.left = dm.dmPosition.x;
    rc.top = dm.dmPosition.y;
    rc.right = rc.left + (LONG)dm.dmPelsWidth;
    rc.bottom = rc.top + (LONG)dm.dmPelsHeight;
    return true;
}

}  // namespace ph
