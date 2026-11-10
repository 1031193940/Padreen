#pragma once
#include "common.h"

#include <shobjidl.h>
#include <wrl/client.h>

namespace ph {

// Gives ONE monitor (the tablet's virtual screen) a plain white wallpaper through IDesktopWallpaper, and puts the previous
// wallpaper of that monitor back afterwards.  Photos are the worst case for a 1-bit e-ink display (every window move uncovers
// a field of dither dots that then has to be refreshed again), plain white is the best case.
//
// The PC's own screens are never touched: only SetWallpaper(<id of the virtual monitor>, ...) is ever called for the tablet; the global
// calls (no monitor id, SetPosition, SetBackgroundColor, SPI_SETDESKWALLPAPER) are not used at all.  As a second line of defence the guard
// watches every other monitor Windows knows (also ones that are switched off right now, e.g. the laptop panel while the lid is closed): if
// one of them ever shows OUR white picture (Windows can adopt the wallpaper of the primary monitor as the common one when the tablet is the
// PC's only screen), it is put back the way it was.  Our white picture is also taken out of Windows' "recent images" list afterwards.
// The tablet monitor's previous wallpaper is kept in a small state file so a crash cannot lose it.
class WallpaperGuard {
public:
    ~WallpaperGuard() { restore(); }
    // Remembers the parameters; if Windows does not know the new monitor yet, keep() retries once a second (up to a minute).
    bool apply(const RECT& monitorRect, const std::wstring& whiteImage, const std::wstring& stateFile, bool quiet = false);
    void restore();
    // Called every second while connected: if something (Settings, a slideshow tick, a theme change) put another picture on the tablet's
    // monitor, put the white one back (at most once per 10 s); every 2 s make sure none of the PC's own screens shows the white one.
    void keep();
    // The monitor got another size / position (the tablet was turned): the retry path of apply() must look for the new rectangle.
    void setRect(const RECT& r) { rect_ = r; }
    // Is this path one of our white pictures (any install folder, current or former file name)?
    static bool isOurWhite(const std::wstring& path);
    // Removes our white picture from "Settings > Personalization > Background > Recent images" (the list is shared by all monitors).
    static void cleanHistory();

private:
    struct Other {
        std::wstring id, path;          // monitor device path, and the wallpaper it showed when the session began
    };
    void snapshotOthers(IDesktopWallpaper* wp, const std::wstring& vddId);
    int healOthers(IDesktopWallpaper* wp, bool announce);
    std::wstring describeOthers(IDesktopWallpaper* wp);

    std::wstring monitorId_, original_, ref_, stateFile_, white_;
    std::vector<Other> others_;
    RECT rect_{};
    int64_t lastFixNs_ = 0, lastTryNs_ = 0, lastOthersNs_ = 0;
    int tries_ = 0;
    int healLogs_ = 0;                  // repairs of the PC's own screens logged so far (capped)
    bool applied_ = false, wanted_ = false;
};

}  // namespace ph
