#pragma once
#include "capture.h"
#include "common.h"

namespace ph {

constexpr int kTile = 36;   // diff granularity; also a multiple of the 6x6 dither block

inline RECT alignTiles(RECT r, int W, int H) {
    r.left = std::max<LONG>(0, r.left / kTile * kTile);
    r.top = std::max<LONG>(0, r.top / kTile * kTile);
    r.right = std::min<LONG>(W, (r.right + kTile - 1) / kTile * kTile);
    r.bottom = std::min<LONG>(H, (r.bottom + kTile - 1) / kTile * kTile);
    return r;
}
inline RECT unionRect(const RECT& a, const RECT& b) {
    return RECT{std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right), std::max(a.bottom, b.bottom)};
}
inline bool emptyRect(const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }

// BGRA (stride in bytes) -> gray8 (stride W) for rect r: luma = (77 R + 150 G + 29 B) >> 8
void bgraToGray(const uint8_t* bgra, int bgraStride, uint8_t* gray, int W, const RECT& r);

// 1-bit conversion tuned for a desktop: per 6x6 block
//   high contrast (text / edges)      -> threshold at the block's mid level
//   flat and dark (<56) / light (>200)-> snap to black / white
//   flat mid grays                    -> ordered (Bayer 8x8) dither, indexed by absolute position => static input stays static
class HybridDither {
public:
    HybridDither(int W, int H);
    void run(const uint8_t* gray, uint8_t* bw, const RECT& r) const;   // r is tile aligned; bw gets 0 / 255
private:
    int W_, H_;
    std::vector<uint8_t> thr_;
};

// Compare two 1-bit (or gray) frames tile by tile inside r; returns the bounding rect of tiles that differ (empty if none).
RECT diffTiles(const uint8_t* a, const uint8_t* b, int W, const RECT& r);
// Copy rect r from src to dst (both W wide).
void copyRect(const uint8_t* src, uint8_t* dst, int W, const RECT& r);
// 0/255 -> packed 1 bit per pixel, rows byte aligned, MSB first, 1 = white
void packMono(const uint8_t* bw, int W, const RECT& r, std::vector<uint8_t>& out);
// Paint the pointer into a gray frame (clipped)
void drawCursor(uint8_t* gray, int W, int H, const CursorShape& c, int px, int py);
RECT cursorRect(const CursorShape& c, int px, int py, int W, int H);
// ---- tile bookkeeping for the settle (gray) pass -------------------------------------------------------------------
inline int tileCols(int W) { return W / kTile; }
inline int tileRows(int H) { return H / kTile; }

// Like diffTiles, but also ORs a 1 into map[ty*cols+tx] for every differing tile (map has tileCols*tileRows entries).
RECT diffTilesMap(const uint8_t* a, const uint8_t* b, int W, const RECT& r, uint8_t* map);
// Does tile (tx,ty) look noticeably worse in 1-bit than in gray?  Flat dark / light areas that were snapped to black / white
// (the same 56 / 200 limits as HybridDither) count as exact, antialiased text and pictures do not.
bool tileNeedsSettle(const uint8_t* gray, const uint8_t* bw, int W, int tx, int ty);
// Gray data for the settle pass: like gray, but near-black / near-white pixels are snapped so flat dark or light backgrounds keep
// the shade the fast path already shows (otherwise GL16 would visibly lighten every dark window).
void snapForSettle(const uint8_t* gray, uint8_t* out, int W, const RECT& r);
// max - min gray level inside a tile (text and edges have a high value, photos and gradients a low one)
int tileContrast(const uint8_t* gray, int W, int tx, int ty);
// Group the flagged tiles into rectangles: connected (gap tolerance 1 tile), then split until each box covers at most maxTiles.
std::vector<RECT> clusterTiles(const std::vector<uint8_t>& flags, int cols, int rows, int maxTiles);

}  // namespace ph
