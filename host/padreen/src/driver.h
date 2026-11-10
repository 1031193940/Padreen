#pragma once
#include "common.h"

namespace ph {

// The virtual display driver: Parsec VDD 0.41 (an IddCx user-mode driver, signed by Microsoft) and nefcon 1.10.0 (nefconw.exe, signed by Parsec).
// It is Parsec's software, NOT part of Padreen's downloads.  When the user agrees (first start, or the tray menu) the program fetches it from
// Parsec's own download address (builds.parsec.app), checks the downloads and every file against the SHA-256 values built into this exe, and
// installs it with one UAC prompt.  The package is four files:
//   nefconw.exe                    device / driver installer tool (taken from Parsec's 0.45 installer)
//   driver\mm.inf, mm.dll, mm.cat  the driver package (taken from Parsec's 0.41 installer)
// Where the program looks for them, in this order: <program folder>\driver (a copy somebody put there: offline use, development builds),
// <cache>\pkg (unpacked by an earlier run), else the two installers in <cache> (kept from an earlier download, or put there by hand), else
// Parsec's download address.  <cache> is a folder of the user (next to settings and log).
// Installing a driver changes the system and needs administrator rights.  The program does it by itself, with one UAC prompt and without a
// script: it starts an elevated copy of itself (--install-driver --package <folder>) that checks every file of the package against the SHA-256
// values built into this exe, copies the package into a folder only administrators can write to, and runs nefconw.exe from there.

enum DriverExit {
    DRV_SUCCESS = 0,          // (not DRV_OK: that is a macro of mmsystem.h)
    DRV_NOT_ELEVATED = 2,
    DRV_NO_PACKAGE = 3,       // a file of the package is missing
    DRV_BAD_PACKAGE = 4,      // a file differs from the one this version of Padreen was built for
    DRV_STAGE_FAILED = 5,     // the package could not be copied into the protected folder
    DRV_INSTALL_FAILED = 6,   // nefconw.exe reported an error (Windows or a security product refused)
    DRV_NOT_RUNNING = 7,      // installed, but the adapter did not come up
    DRV_DOWNLOAD_FAILED = 8,  // Parsec's download address could not be reached, or did not answer with a file
    DRV_UNEXPECTED_FILE = 9,  // what was downloaded is not the file this Padreen was built for (Parsec replaced it?): not used
    DRV_EXTRACT_FAILED = 10,  // the files could not be taken out of the installers, or written to the cache
};

enum class DriverAsk { Declined, Failed, Installed };

using DriverStatusFn = std::function<void(const std::wstring&)>;     // progress text for the status line (may be called from a worker thread)

// Explains what will be downloaded and installed, asks, gets the package (downloads it if needed, while the message loop keeps running), starts
// the elevated installer and waits for it.  repair = the driver is there already: offer to install it again.  Shows its own message boxes
// (parent may be null).  logPath: the log file the elevated installer appends to.  cacheDir: where downloads and the unpacked package are kept.
// cancel: set when the program quits, ends a download early.
DriverAsk installDriverInteractive(HWND parent, const std::wstring& logPath, bool repair, const std::wstring& cacheDir, const DriverStatusFn& status,
                                   const std::atomic<bool>* cancel);

// command line modes of padreen.exe (all return the process exit code):
int verifyDriverPackage(const std::wstring& dir);                  // --verify-driver [--package dir]   no administrator needed, changes nothing: is the package complete and the one this exe expects?  (dir empty: <program folder>\driver)
int fetchDriverPackage(const std::wstring& cacheDir);              // --fetch-driver --cache dir        no administrator needed: gets the package the way the first start does (writes below dir only); make-dist.ps1 uses it as a release check
int runInstallDriver(const std::wstring& log, const std::wstring& dir);   // --install-driver [--package dir] [--log f]   needs administrator: the elevated part of installDriverInteractive
std::wstring driverExitText(int code);

}  // namespace ph
