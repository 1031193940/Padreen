"""Keep Windows from throttling this process (EcoQoS / timer coalescing / low priority).

A background process on a laptop can be power-throttled: its timers fire late and its threads run on slow cores.  For a
latency-critical host that showed up as multi-second stalls in a long camera test, so every Padreen process opts out:
  * SetProcessInformation(ProcessPowerThrottling): EXECUTION_SPEED throttling OFF, IGNORE_TIMER_RESOLUTION on
  * timeBeginPeriod(1): 1 ms system timer
  * HIGH_PRIORITY_CLASS
Nothing global is changed: all three only affect the calling process (the timer request ends with the process).
"""
import ctypes
import ctypes.wintypes as wt

_k32 = ctypes.WinDLL('kernel32', use_last_error=True)
_winmm = ctypes.WinDLL('winmm')


class _PTS(ctypes.Structure):
    _fields_ = [('Version', wt.ULONG), ('ControlMask', wt.ULONG), ('StateMask', wt.ULONG)]


_k32.SetProcessInformation.argtypes = [wt.HANDLE, ctypes.c_int, ctypes.c_void_p, wt.DWORD]
_k32.GetCurrentProcess.restype = wt.HANDLE
_k32.SetPriorityClass.argtypes = [wt.HANDLE, wt.DWORD]

PROCESS_POWER_THROTTLING_EXECUTION_SPEED = 0x1
PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION = 0x4
ProcessPowerThrottling = 4
HIGH_PRIORITY_CLASS = 0x80


def boost():
    """Returns a short status string (for logging)."""
    me = _k32.GetCurrentProcess()
    st = _PTS(1, PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, 0)
    ok_pt = bool(_k32.SetProcessInformation(me, ProcessPowerThrottling, ctypes.byref(st), ctypes.sizeof(st)))
    ok_t = _winmm.timeBeginPeriod(1) == 0
    ok_p = bool(_k32.SetPriorityClass(me, HIGH_PRIORITY_CLASS))
    return 'power-throttling-off=%s timer=1ms=%s high-priority=%s' % (ok_pt, ok_t, ok_p)
