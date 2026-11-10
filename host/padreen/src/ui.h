#pragma once
#include "common.h"
#include "devices.h"
#include "lan.h"

namespace ph {

// Settings that exist as plain on/off switches in the UI.
enum UiFlag { UF_LAN, UF_TOUCH, UF_WALLPAPER, UF_AUTOSTART, UF_LIDHOLD, UF_USB, UF_GESTURES, UF_ROTATE };

struct UiChoice {
    int value;
    std::wstring label;
};

// Everything the windows need from the application; filled in by main.cpp (keeps the UI independent of the App internals).
struct UiHost {
    std::function<std::wstring()> statusText;
    std::function<int()> statusColor;                       // 0 grey, 1 amber, 2 green, 3 red
    std::function<bool(UiFlag)> flag;
    std::function<void(UiFlag, bool)> setFlag;
    std::vector<UiChoice> waves, settles;                  // fast-mode refresh waveforms / automatic gray settle levels
    std::function<int()> wave, settle;
    std::function<void(int)> setWave, setSettle;
    std::function<void()> fullRefresh, openLog, enableLan;  // enableLan: (re)start the LAN listener incl. the firewall question
    std::function<bool()> lanBlocked;
    std::function<std::wstring()> lidInfo;                  // one line: what closing the lid does now, and whether the power helper covers it
    LanServer* lan = nullptr;
    Devices* devices = nullptr;
    int pairMinutes = 5;
    std::wstring version;
};

void uiShowMain(UiHost* host);       // control panel (tray double click)
void uiShowPair(UiHost* host);       // pairing window: shows the 9-digit code
void uiShowDevices(UiHost* host);    // list of paired tablets, unpair
void uiRefresh();                    // the status / state changed: update the open windows (any thread)
bool uiDialogMessage(MSG* m);        // call from the message loop before TranslateMessage (tab / enter keys)

}  // namespace ph
