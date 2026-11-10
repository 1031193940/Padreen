#!/usr/bin/env python3
"""Send a tray-menu command to a running Padreen:  python phctl.py quit|full|reconnect|fast|gray|wallpaper|lan|pair|devices|touch|rotate|du|a2|a2f6|a2f7|a2f8|a2f9"""
import ctypes
import sys

IDS = {'fast': 1001, 'gray': 1002, 'full': 1003, 'wallpaper': 1004, 'reconnect': 1005, 'quit': 1007, 'lan': 1009, 'touch': 1010, 'open': 1014, 'usb': 1015, 'rotate': 1016, 'pair': 1011, 'devices': 1012, 'settle-off': 1200, 'settle-light': 1201, 'settle-full': 1202,
       'du': 1101, 'a2': 1105, 'a2f6': 1106, 'a2f7': 1107, 'a2f8': 1108, 'a2f9': 1109}
u = ctypes.windll.user32
u.FindWindowW.restype = ctypes.c_void_p
u.FindWindowW.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
u.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in IDS:
        print(__doc__)
        return 2
    h = u.FindWindowW('PadreenTray', None)
    if not h:
        print('Padreen is not running')
        return 1
    u.PostMessageW(h, 0x0111, IDS[sys.argv[1]], 0)       # WM_COMMAND
    print('sent', sys.argv[1])
    return 0


if __name__ == '__main__':
    sys.exit(main())
