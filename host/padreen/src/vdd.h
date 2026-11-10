#pragma once
#include "common.h"

namespace ph {

// Parsec Virtual Display Driver client (IddCx user-mode driver).  It is installed once: by the installer, by the program itself (driver.cpp: first
// start / tray menu) or by hand with driver/install-vdd.ps1.
// Displays vanish ~1 s after the last UPDATE ping, so a crash cannot leave one behind.
class Vdd {
public:
    ~Vdd() { close(); }
    bool open();                 // false when the driver is not installed
    static bool installed();     // the driver is installed and its device runs (the device interface exists); opens nothing, starts nothing
    void close();                // removes every display this object added
    int addDisplay();            // index of the new virtual display, or -1
    void removeDisplay(int idx);
    int version();

private:
    int ioctl(DWORD code, const void* in, size_t inSize);
    HANDLE h_ = INVALID_HANDLE_VALUE;
    std::thread ping_;
    std::atomic<bool> run_{false};
    std::vector<int> added_;
    std::mutex mu_;
};

// Windows-side helpers for the virtual monitor.
int attachedRealDisplays();                                              // attached displays that are not the virtual one (0 = the e-ink display is the only screen)
bool findVirtualDisplay(std::wstring& gdiName);                       // attached display whose monitor id contains PSCCDD0
bool setDisplayMode(const std::wstring& gdiName, int w, int h, int hz);
bool displayRect(const std::wstring& gdiName, RECT& rc);

}  // namespace ph
