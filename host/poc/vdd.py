#!/usr/bin/env python3
"""Parsec VDD client (PoC): plug a virtual monitor in, give it the e-ink panel's native mode, keep it alive.

The driver (Microsoft-signed IddCx user-mode driver, installed once with tools/vdd/pkg/install-vdd.ps1) exposes a
device interface that any user may open.  IOCTLs (see nomi-san/parsec-vdd, core/parsec-vdd.h):
    ADD     -> returns the index of the new virtual display
    UPDATE  -> keep-alive ping; every display vanishes ~1 s after the last ping (so a crash cannot leave one behind)
    REMOVE  -> unplug a display by index
Nothing global is modified: the monitor exists only while this process pings.

    python vdd.py                 # add a 1872x1404@60 display, ping until Ctrl-C, then remove it
    python vdd.py --status        # only report the driver / monitors
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import sys
import threading
import time

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
setupapi = ctypes.WinDLL('setupapi', use_last_error=True)
user32 = ctypes.WinDLL('user32', use_last_error=True)

HANDLE = wt.HANDLE
INVALID_HANDLE_VALUE = HANDLE(-1).value


class GUID(ctypes.Structure):
    _fields_ = [('Data1', wt.DWORD), ('Data2', wt.WORD), ('Data3', wt.WORD), ('Data4', ctypes.c_ubyte * 8)]


def make_guid(s):
    import uuid
    u = uuid.UUID(s)
    g = GUID()
    g.Data1, g.Data2, g.Data3 = u.time_low, u.time_mid, u.time_hi_version
    for i, b in enumerate(u.bytes[8:]):
        g.Data4[i] = b
    return g


VDD_ADAPTER_GUID = make_guid('00b41627-04c4-429e-a26e-0265cf50c8fa')
IOCTL_ADD, IOCTL_REMOVE, IOCTL_UPDATE, IOCTL_VERSION = 0x0022e004, 0x0022a008, 0x0022a00c, 0x0022e010


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [('cbSize', wt.DWORD), ('InterfaceClassGuid', GUID), ('Flags', wt.DWORD), ('Reserved', ctypes.c_void_p)]


class OVERLAPPED(ctypes.Structure):
    _fields_ = [('Internal', ctypes.c_void_p), ('InternalHigh', ctypes.c_void_p), ('Offset', wt.DWORD),
                ('OffsetHigh', wt.DWORD), ('hEvent', HANDLE)]


setupapi.SetupDiGetClassDevsA.restype = HANDLE
setupapi.SetupDiGetClassDevsA.argtypes = [ctypes.POINTER(GUID), ctypes.c_char_p, wt.HWND, wt.DWORD]
setupapi.SetupDiEnumDeviceInterfaces.argtypes = [HANDLE, ctypes.c_void_p, ctypes.POINTER(GUID), wt.DWORD, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA)]
setupapi.SetupDiGetDeviceInterfaceDetailA.argtypes = [HANDLE, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA), ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD), ctypes.c_void_p]
setupapi.SetupDiDestroyDeviceInfoList.argtypes = [HANDLE]
k32.CreateFileA.restype = HANDLE
k32.CreateFileA.argtypes = [ctypes.c_char_p, wt.DWORD, wt.DWORD, ctypes.c_void_p, wt.DWORD, wt.DWORD, HANDLE]
k32.DeviceIoControl.argtypes = [HANDLE, wt.DWORD, ctypes.c_void_p, wt.DWORD, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD), ctypes.POINTER(OVERLAPPED)]
k32.GetOverlappedResultEx.argtypes = [HANDLE, ctypes.POINTER(OVERLAPPED), ctypes.POINTER(wt.DWORD), wt.DWORD, wt.BOOL]
k32.CreateEventA.restype = HANDLE
k32.CreateEventA.argtypes = [ctypes.c_void_p, wt.BOOL, wt.BOOL, ctypes.c_char_p]
k32.CloseHandle.argtypes = [HANDLE]


def open_device():
    DIGCF_PRESENT, DIGCF_DEVICEINTERFACE = 0x2, 0x10
    dev = setupapi.SetupDiGetClassDevsA(ctypes.byref(VDD_ADAPTER_GUID), None, None, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
    if dev == INVALID_HANDLE_VALUE:
        return None
    handle = None
    try:
        i = 0
        while True:
            idata = SP_DEVICE_INTERFACE_DATA()
            idata.cbSize = ctypes.sizeof(idata)
            if not setupapi.SetupDiEnumDeviceInterfaces(dev, None, ctypes.byref(VDD_ADAPTER_GUID), i, ctypes.byref(idata)):
                break
            need = wt.DWORD(0)
            setupapi.SetupDiGetDeviceInterfaceDetailA(dev, ctypes.byref(idata), None, 0, ctypes.byref(need), None)
            buf = ctypes.create_string_buffer(need.value)
            ctypes.memmove(buf, ctypes.byref(wt.DWORD(8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 5)), 4)   # cbSize of the detail struct
            if setupapi.SetupDiGetDeviceInterfaceDetailA(dev, ctypes.byref(idata), buf, need.value, ctypes.byref(need), None):
                path = ctypes.string_at(ctypes.addressof(buf) + 4)
                GENERIC_RW, SHARE_RW, OPEN_EXISTING = 0xC0000000, 3, 3
                FLAGS = 0x80 | 0x20000000 | 0x40000000 | 0x80000000          # NORMAL | NO_BUFFERING | OVERLAPPED | WRITE_THROUGH
                h = k32.CreateFileA(path, GENERIC_RW, SHARE_RW, None, OPEN_EXISTING, FLAGS, None)
                if h != INVALID_HANDLE_VALUE and h:
                    handle = h
                    break
            i += 1
    finally:
        setupapi.SetupDiDestroyDeviceInfoList(dev)
    return handle


def ioctl(handle, code, data=b''):
    inbuf = ctypes.create_string_buffer(32)
    if data:
        ctypes.memmove(inbuf, data, min(len(data), 32))
    out = wt.DWORD(0)
    ov = OVERLAPPED()
    ov.hEvent = k32.CreateEventA(None, True, False, None)
    got = wt.DWORD(0)
    k32.DeviceIoControl(handle, code, inbuf, 32, ctypes.byref(out), 4, None, ctypes.byref(ov))
    ok = k32.GetOverlappedResultEx(handle, ctypes.byref(ov), ctypes.byref(got), 5000, False)
    k32.CloseHandle(ov.hEvent)
    return out.value if ok else None


class Vdd:
    def __init__(self):
        self.h = open_device()
        if not self.h:
            raise RuntimeError('Parsec Virtual Display Adapter not found - run tools/vdd/pkg/install-vdd.ps1 (admin) first')
        self.alive = threading.Event()
        self.alive.set()
        self.thread = threading.Thread(target=self._ping, daemon=True)
        self.thread.start()
        self.indices = []

    def _ping(self):
        while self.alive.is_set():
            ioctl(self.h, IOCTL_UPDATE)
            time.sleep(0.05)

    def version(self):
        return ioctl(self.h, IOCTL_VERSION)

    def add(self):
        idx = ioctl(self.h, IOCTL_ADD)
        if idx is not None:
            self.indices.append(idx)
        return idx

    def remove(self, idx):
        data = bytes([(idx >> 8) & 0xFF, idx & 0xFF])         # 16-bit big endian
        ioctl(self.h, IOCTL_REMOVE, data)
        if idx in self.indices:
            self.indices.remove(idx)

    def close(self):
        for i in list(reversed(self.indices)):                # unplug right-to-left (Windows caches display combos)
            self.remove(i)
        self.alive.clear()
        self.thread.join(timeout=1)
        k32.CloseHandle(self.h)


# --------------------------------------------------------------------------- Windows display helpers
class DISPLAY_DEVICEW(ctypes.Structure):
    _fields_ = [('cb', wt.DWORD), ('DeviceName', wt.WCHAR * 32), ('DeviceString', wt.WCHAR * 128), ('StateFlags', wt.DWORD),
                ('DeviceID', wt.WCHAR * 128), ('DeviceKey', wt.WCHAR * 128)]


class DEVMODEW(ctypes.Structure):
    _fields_ = [('dmDeviceName', wt.WCHAR * 32), ('dmSpecVersion', wt.WORD), ('dmDriverVersion', wt.WORD), ('dmSize', wt.WORD),
                ('dmDriverExtra', wt.WORD), ('dmFields', wt.DWORD), ('dmPositionX', wt.LONG), ('dmPositionY', wt.LONG),
                ('dmDisplayOrientation', wt.DWORD), ('dmDisplayFixedOutput', wt.DWORD), ('dmColor', ctypes.c_short),
                ('dmDuplex', ctypes.c_short), ('dmYResolution', ctypes.c_short), ('dmTTOption', ctypes.c_short),
                ('dmCollate', ctypes.c_short), ('dmFormName', wt.WCHAR * 32), ('dmLogPixels', wt.WORD), ('dmBitsPerPel', wt.DWORD),
                ('dmPelsWidth', wt.DWORD), ('dmPelsHeight', wt.DWORD), ('dmDisplayFlags', wt.DWORD), ('dmDisplayFrequency', wt.DWORD),
                ('dmICMMethod', wt.DWORD), ('dmICMIntent', wt.DWORD), ('dmMediaType', wt.DWORD), ('dmDitherType', wt.DWORD),
                ('dmReserved1', wt.DWORD), ('dmReserved2', wt.DWORD), ('dmPanningWidth', wt.DWORD), ('dmPanningHeight', wt.DWORD)]


user32.EnumDisplayDevicesW.argtypes = [wt.LPCWSTR, wt.DWORD, ctypes.POINTER(DISPLAY_DEVICEW), wt.DWORD]
user32.EnumDisplaySettingsW.argtypes = [wt.LPCWSTR, wt.DWORD, ctypes.POINTER(DEVMODEW)]
user32.ChangeDisplaySettingsExW.argtypes = [wt.LPCWSTR, ctypes.POINTER(DEVMODEW), wt.HWND, wt.DWORD, ctypes.c_void_p]
ENUM_CURRENT_SETTINGS = -1
DM_POSITION, DM_PELSWIDTH, DM_PELSHEIGHT, DM_DISPLAYFREQUENCY = 0x20, 0x80000, 0x100000, 0x400000
CDS_UPDATEREGISTRY, CDS_NORESET, CDS_FULLSCREEN = 1, 0x10000000, 4
DISP_CHANGE_SUCCESSFUL = 0


def list_displays():
    """[(device name, adapter string, monitor id string, attached?, WxH@Hz)] for every display Windows knows."""
    out = []
    i = 0
    while True:
        a = DISPLAY_DEVICEW()
        a.cb = ctypes.sizeof(a)
        if not user32.EnumDisplayDevicesW(None, i, ctypes.byref(a), 0):
            break
        i += 1
        attached = bool(a.StateFlags & 1)
        mon_id = ''
        m = DISPLAY_DEVICEW()
        m.cb = ctypes.sizeof(m)
        if user32.EnumDisplayDevicesW(a.DeviceName, 0, ctypes.byref(m), 1):
            mon_id = m.DeviceID
        mode = ''
        if attached:
            dm = DEVMODEW()
            dm.dmSize = ctypes.sizeof(dm)
            if user32.EnumDisplaySettingsW(a.DeviceName, ENUM_CURRENT_SETTINGS, ctypes.byref(dm)):
                mode = '%dx%d@%d at (%d,%d)' % (dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency, dm.dmPositionX, dm.dmPositionY)
        out.append((a.DeviceName, a.DeviceString, mon_id, attached, mode))
    return out


def find_virtual():
    for name, adapter, mon, attached, mode in list_displays():
        if attached and ('PSCCDD0' in mon.upper() or 'Parsec' in adapter):
            return name
    return None


def display_rect(devname):
    """(x, y, w, h) of an attached display in virtual-desktop pixels, or None."""
    dm = DEVMODEW()
    dm.dmSize = ctypes.sizeof(dm)
    if not user32.EnumDisplaySettingsW(devname, ENUM_CURRENT_SETTINGS, ctypes.byref(dm)):
        return None
    return dm.dmPositionX, dm.dmPositionY, dm.dmPelsWidth, dm.dmPelsHeight


def set_mode(devname, w, h, hz, persist=True):
    """Give one display (and only that one) the requested mode.  persist=True remembers it for this virtual monitor only."""
    dm = DEVMODEW()
    dm.dmSize = ctypes.sizeof(dm)
    if not user32.EnumDisplaySettingsW(devname, ENUM_CURRENT_SETTINGS, ctypes.byref(dm)):
        raise RuntimeError('EnumDisplaySettings failed for ' + devname)
    dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency = w, h, hz
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_POSITION
    flags = CDS_UPDATEREGISTRY if persist else CDS_FULLSCREEN
    return user32.ChangeDisplaySettingsExW(devname, ctypes.byref(dm), None, flags, None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--status', action='store_true')
    ap.add_argument('--width', type=int, default=1872)
    ap.add_argument('--height', type=int, default=1404)
    ap.add_argument('--hz', type=int, default=60)
    ap.add_argument('--seconds', type=float, default=0, help='0 = until Ctrl-C')
    ap.add_argument('--no-persist', action='store_true', help='do not remember the mode for the virtual monitor')
    args = ap.parse_args()

    if args.status:
        for d in list_displays():
            print('%-16s attached=%d  %-34s %-40s %s' % (d[0], d[3], d[1][:34], d[2][:40], d[4]))
        return 0
    v = Vdd()
    print('driver minor version:', v.version())
    idx = v.add()
    print('added virtual display, index =', idx)
    name = None
    for _ in range(100):
        name = find_virtual()
        if name:
            break
        time.sleep(0.1)
    if not name:
        print('the virtual monitor did not show up in Windows')
        v.close()
        return 1
    print('Windows display name:', name)
    rc = set_mode(name, args.width, args.height, args.hz, persist=not args.no_persist)
    print('set mode %dx%d@%d -> %s' % (args.width, args.height, args.hz, 'OK' if rc == DISP_CHANGE_SUCCESSFUL else 'code %d' % rc))
    time.sleep(1.0)
    for d in list_displays():
        if d[3]:
            print('  %-16s %-34s %s' % (d[0], d[1][:34], d[4]))
    try:
        t_end = time.time() + args.seconds if args.seconds > 0 else float('inf')
        while time.time() < t_end:
            time.sleep(0.2)
    except KeyboardInterrupt:
        pass
    finally:
        v.close()
        print('virtual display removed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
