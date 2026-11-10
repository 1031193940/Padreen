#include "convert.h"

#include <algorithm>

namespace ph {

void bgraToGray(const uint8_t* bgra, int bgraStride, uint8_t* gray, int W, const RECT& r) {
    for (LONG y = r.top; y < r.bottom; ++y) {
        const uint8_t* s = bgra + (size_t)y * bgraStride + (size_t)r.left * 4;
        uint8_t* d = gray + (size_t)y * W + r.left;
        for (LONG x = r.left; x < r.right; ++x, s += 4, ++d) *d = (uint8_t)((s[2] * 77 + s[1] * 150 + s[0] * 29) >> 8);
    }
}

HybridDither::HybridDither(int W, int H) : W_(W), H_(H), thr_((size_t)W * H) {
    static const uint8_t bayer[8][8] = {{0, 32, 8, 40, 2, 34, 10, 42}, {48, 16, 56, 24, 50, 18, 58, 26}, {12, 44, 4, 36, 14, 46, 6, 38},
                                        {60, 28, 52, 20, 62, 30, 54, 22}, {3, 35, 11, 43, 1, 33, 9, 41}, {51, 19, 59, 27, 49, 17, 57, 25},
                                        {15, 47, 7, 39, 13, 45, 5, 37}, {63, 31, 55, 23, 61, 29, 53, 21}};
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) thr_[(size_t)y * W + x] = (uint8_t)(((bayer[y & 7][x & 7] + 0.5f) * (255.0f / 64.0f)) + 0.5f);
}

void HybridDither::run(const uint8_t* gray, uint8_t* bw, const RECT& r) const {
    for (LONG by = r.top; by < r.bottom; by += 6) {
        for (LONG bx = r.left; bx < r.right; bx += 6) {
            int mn = 255, mx = 0;
            for (int y = 0; y < 6; ++y) {
                const uint8_t* p = gray + (size_t)(by + y) * W_ + bx;
                for (int x = 0; x < 6; ++x) {
                    int v = p[x];
                    mn = v < mn ? v : mn;
                    mx = v > mx ? v : mx;
                }
            }
            int mid = (mx + mn) >> 1;
            if (mx - mn > 64) {
                for (int y = 0; y < 6; ++y) {
                    const uint8_t* p = gray + (size_t)(by + y) * W_ + bx;
                    uint8_t* o = bw + (size_t)(by + y) * W_ + bx;
                    for (int x = 0; x < 6; ++x) o[x] = p[x] >= mid ? 255 : 0;
                }
            } else if (mid < 56 || mid > 200) {
                uint8_t v = mid < 56 ? 0 : 255;
                for (int y = 0; y < 6; ++y) memset(bw + (size_t)(by + y) * W_ + bx, v, 6);
            } else {
                for (int y = 0; y < 6; ++y) {
                    const uint8_t* p = gray + (size_t)(by + y) * W_ + bx;
                    const uint8_t* t = thr_.data() + (size_t)(by + y) * W_ + bx;
                    uint8_t* o = bw + (size_t)(by + y) * W_ + bx;
                    for (int x = 0; x < 6; ++x) o[x] = p[x] > t[x] ? 255 : 0;
                }
            }
        }
    }
}

RECT diffTiles(const uint8_t* a, const uint8_t* b, int W, const RECT& r) {
    RECT out{INT_MAX, INT_MAX, INT_MIN, INT_MIN};
    for (LONG ty = r.top; ty < r.bottom; ty += kTile) {
        for (LONG tx = r.left; tx < r.right; tx += kTile) {
            LONG tw = std::min<LONG>(kTile, r.right - tx), th = std::min<LONG>(kTile, r.bottom - ty);
            bool diff = false;
            for (LONG y = 0; y < th && !diff; ++y) diff = memcmp(a + (size_t)(ty + y) * W + tx, b + (size_t)(ty + y) * W + tx, tw) != 0;
            if (diff) {
                out.left = std::min(out.left, tx);
                out.top = std::min(out.top, ty);
                out.right = std::max(out.right, tx + tw);
                out.bottom = std::max(out.bottom, ty + th);
            }
        }
    }
    if (out.right == INT_MIN) return RECT{0, 0, 0, 0};
    return out;
}

void copyRect(const uint8_t* src, uint8_t* dst, int W, const RECT& r) {
    for (LONG y = r.top; y < r.bottom; ++y) memcpy(dst + (size_t)y * W + r.left, src + (size_t)y * W + r.left, (size_t)(r.right - r.left));
}

void packMono(const uint8_t* bw, int W, const RECT& r, std::vector<uint8_t>& out) {
    int w = r.right - r.left, h = r.bottom - r.top, stride = (w + 7) / 8;
    out.assign((size_t)stride * h, 0);
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = bw + (size_t)(r.top + y) * W + r.left;
        uint8_t* d = out.data() + (size_t)y * stride;
        for (int x = 0; x < w; ++x)
            if (s[x] > 127) d[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
    }
}

RECT cursorRect(const CursorShape& c, int px, int py, int W, int H) {
    RECT r{px - c.hotX, py - c.hotY, px - c.hotX + c.w, py - c.hotY + c.h};
    r.left = std::max<LONG>(0, r.left);
    r.top = std::max<LONG>(0, r.top);
    r.right = std::min<LONG>(W, r.right);
    r.bottom = std::min<LONG>(H, r.bottom);
    if (r.right <= r.left || r.bottom <= r.top) return RECT{0, 0, 0, 0};
    return r;
}

void drawCursor(uint8_t* gray, int W, int H, const CursorShape& c, int px, int py) {
    if (!c.valid()) return;
    int ox = px - c.hotX, oy = py - c.hotY;
    for (int y = 0; y < c.h; ++y) {
        int gy = oy + y;
        if (gy < 0 || gy >= H) continue;
        for (int x = 0; x < c.w; ++x) {
            int gx = ox + x;
            if (gx < 0 || gx >= W) continue;
            size_t o = (size_t)y * c.w + x;
            uint8_t a = c.alpha[o];
            if (!c.inv.empty() && c.inv[o]) gray[(size_t)gy * W + gx] = (uint8_t)(255 - gray[(size_t)gy * W + gx]);
            else if (a > 127) gray[(size_t)gy * W + gx] = c.gray[o];
        }
    }
}


RECT diffTilesMap(const uint8_t* a, const uint8_t* b, int W, const RECT& r, uint8_t* map) {
    RECT out{INT_MAX, INT_MAX, INT_MIN, INT_MIN};
    int cols = W / kTile;
    for (LONG ty = r.top; ty < r.bottom; ty += kTile) {
        for (LONG tx = r.left; tx < r.right; tx += kTile) {
            LONG tw = std::min<LONG>(kTile, r.right - tx), th = std::min<LONG>(kTile, r.bottom - ty);
            bool diff = false;
            for (LONG y = 0; y < th && !diff; ++y) diff = memcmp(a + (size_t)(ty + y) * W + tx, b + (size_t)(ty + y) * W + tx, tw) != 0;
            if (diff) {
                map[(ty / kTile) * cols + (tx / kTile)] = 1;
                out.left = std::min(out.left, tx);
                out.top = std::min(out.top, ty);
                out.right = std::max(out.right, tx + tw);
                out.bottom = std::max(out.bottom, ty + th);
            }
        }
    }
    if (out.right == INT_MIN) return RECT{0, 0, 0, 0};
    return out;
}

bool tileNeedsSettle(const uint8_t* gray, const uint8_t* bw, int W, int tx, int ty) {
    int x0 = tx * kTile, y0 = ty * kTile;
    int sum = 0;
    for (int y = 0; y < kTile; ++y) {
        const uint8_t* g = gray + (size_t)(y0 + y) * W + x0;
        const uint8_t* b = bw + (size_t)(y0 + y) * W + x0;
        for (int x = 0; x < kTile; ++x) {
            int e = b[x] ? 200 - (int)g[x] : (int)g[x] - 56;
            if (e > 0) sum += e;
        }
    }
    return sum >= 1500;
}

int tileContrast(const uint8_t* gray, int W, int tx, int ty) {
    int mn = 255, mx = 0;
    for (int y = 0; y < kTile; ++y) {
        const uint8_t* g = gray + (size_t)(ty * kTile + y) * W + (size_t)tx * kTile;
        for (int x = 0; x < kTile; ++x) {
            mn = g[x] < mn ? g[x] : mn;
            mx = g[x] > mx ? g[x] : mx;
        }
    }
    return mx - mn;
}
void snapForSettle(const uint8_t* gray, uint8_t* out, int W, const RECT& r) {
    for (LONG y = r.top; y < r.bottom; ++y) {
        const uint8_t* g = gray + (size_t)y * W + r.left;
        uint8_t* o = out + (size_t)y * W + r.left;
        for (LONG x = r.left; x < r.right; ++x, ++g, ++o) *o = *g < 48 ? 0 : (*g > 208 ? 255 : *g);
    }
}

namespace {
struct TileXY {
    int x, y;
};
void splitRec(std::vector<TileXY>& pts, int maxTiles, std::vector<RECT>& out) {
    int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
    for (auto& p : pts) {
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x);
        y1 = std::max(y1, p.y);
    }
    int w = x1 - x0 + 1, h = y1 - y0 + 1;
    if (w * h <= maxTiles || pts.size() <= 1) {
        out.push_back(RECT{x0 * kTile, y0 * kTile, (x1 + 1) * kTile, (y1 + 1) * kTile});
        return;
    }
    std::vector<TileXY> a, b;
    if (w >= h) {
        int mid = x0 + w / 2;
        for (auto& p : pts) (p.x < mid ? a : b).push_back(p);
    } else {
        int mid = y0 + h / 2;
        for (auto& p : pts) (p.y < mid ? a : b).push_back(p);
    }
    if (a.empty() || b.empty()) {
        out.push_back(RECT{x0 * kTile, y0 * kTile, (x1 + 1) * kTile, (y1 + 1) * kTile});
        return;
    }
    splitRec(a, maxTiles, out);
    splitRec(b, maxTiles, out);
}
}  // namespace

std::vector<RECT> clusterTiles(const std::vector<uint8_t>& flags, int cols, int rows, int maxTiles) {
    std::vector<RECT> out;
    std::vector<uint8_t> seen(flags.size(), 0);
    for (int ty = 0; ty < rows; ++ty) {
        for (int tx = 0; tx < cols; ++tx) {
            int i = ty * cols + tx;
            if (!flags[i] || seen[i]) continue;
            std::vector<TileXY> comp, stack;
            stack.push_back({tx, ty});
            seen[i] = 1;
            while (!stack.empty()) {
                TileXY t = stack.back();
                stack.pop_back();
                comp.push_back(t);
                for (int dy = -2; dy <= 2; ++dy)
                    for (int dx = -2; dx <= 2; ++dx) {
                        int nx = t.x + dx, ny = t.y + dy;
                        if (nx < 0 || ny < 0 || nx >= cols || ny >= rows) continue;
                        int j = ny * cols + nx;
                        if (flags[j] && !seen[j]) {
                            seen[j] = 1;
                            stack.push_back({nx, ny});
                        }
                    }
            }
            splitRec(comp, maxTiles, out);
        }
    }
    return out;
}
}  // namespace ph
