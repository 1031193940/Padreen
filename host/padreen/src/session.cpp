#include "session.h"

#include <algorithm>

#include "lz4.h"

namespace ph {

bool Session::sendGray(uint8_t mode, const RECT& r, const std::vector<uint8_t>& gray, int W) {
    int w = r.right - r.left, h = r.bottom - r.top;
    tmp_.resize((size_t)w * h);
    for (int y = 0; y < h; ++y) memcpy(&tmp_[(size_t)y * w], &gray[(size_t)(r.top + y) * W + r.left], (size_t)w);
    const uint8_t* data = tmp_.data();
    size_t len = tmp_.size();
    uint8_t codec = CODEC_RAW;
    if (link_.caps() & CAP_LZ4) {
        comp_.resize((size_t)LZ4_compressBound((int)tmp_.size()));
        int c = LZ4_compress_default((const char*)tmp_.data(), (char*)comp_.data(), (int)tmp_.size(), (int)comp_.size());
        if (c > 0 && (size_t)c < tmp_.size()) {
            data = comp_.data();
            len = (size_t)c;
            codec = CODEC_LZ4;
        }
    }
    bool ok = link_.sendUpdate(mode, codec, r, data, len);
    if (ok) {
        stats.updates++;
        stats.bytes += len + 26;
        countSize(r, W, (int)gray.size() / W);
    }
    return ok;
}

bool Session::sendMono(const RECT& r, const std::vector<uint8_t>& bw, int W) {
    packMono(bw.data(), W, r, tmp_);
    const uint8_t* data = tmp_.data();
    size_t len = tmp_.size();
    uint8_t codec = CODEC_MONO1;
    if ((link_.caps() & CAP_LZ4_MONO) && tmp_.size() > 256) {          // big flat areas shrink 10-100x: matters on Wi-Fi, helps on USB
        comp_.resize((size_t)LZ4_compressBound((int)tmp_.size()));
        int c = LZ4_compress_default((const char*)tmp_.data(), (char*)comp_.data(), (int)tmp_.size(), (int)comp_.size());
        if (c > 0 && (size_t)c < tmp_.size() * 9 / 10) {
            data = comp_.data();
            len = (size_t)c;
            codec = CODEC_LZ4_MONO;
        }
    }
    bool ok = link_.sendUpdate((uint8_t)fastMode_.load(), codec, r, data, len);
    if (ok) {
        stats.updates++;
        stats.bytes += len + 26;
        countSize(r, W, (int)bw.size() / W);
    }
    return ok;
}

static inline int64_t rectArea(const RECT& r) { return emptyRect(r) ? 0 : (int64_t)(r.right - r.left) * (r.bottom - r.top); }

void Session::countSize(const RECT& r, int W, int H) {
    int64_t a = rectArea(r), screen = (int64_t)W * H;
    if (a * 2 >= screen) stats.big++;
    if (a * 10 >= screen * 9) stats.full++;
}

std::string Session::run(const std::atomic<bool>& stop) {
    fastMode_ = cfg_.fastMode;
    settleLevel_ = cfg_.settleLevel;
    void* boost = beginThreadBoost(L"Games");
    Duplicator dup;
    bool dupOk = false;
    for (int i = 0; i < 25 && !stop && !dupOk; ++i) {        // right after a mode change (the tablet was turned) the duplication can take a moment to be offered
        dupOk = dup.init(cfg_.resolveGdi ? cfg_.resolveGdi() : cfg_.gdiName);
        if (!dupOk) Sleep(200);
    }
    if (!dupOk) {
        endThreadBoost(boost);
        return stop ? "stopped" : "cannot duplicate the virtual monitor";
    }
    const int W = dup.width(), H = dup.height();
    if (W != link_.panelWidth() || H != link_.panelHeight()) logf("session: WARNING virtual monitor %dx%d != panel %dx%d", W, H, link_.panelWidth(), link_.panelHeight());
    if (W % kTile || H % kTile) {
        endThreadBoost(boost);
        return "frame size is not a multiple of the tile size";
    }
    const int cols = tileCols(W), rows = tileRows(H);
    const int64_t screen = (int64_t)W * H;
    const int maxSettleTiles = cols * rows * 12 / 100;     // one settle post covers at most ~12% of the panel
    const bool resume = resume_.exchange(false) && screen_.valid && screen_.W == W && screen_.H == H;      // keep what the tablet shows
    const bool quick = cfg_.quickFirst || quickNext_.exchange(false);
    if (!resume) {
        screen_.clean.assign((size_t)W * H, 0);
        screen_.gray.assign((size_t)W * H, 0);
        screen_.bw.assign((size_t)W * H, 0);
        screen_.prevBw.assign((size_t)W * H, 0);
        screen_.prevGray.assign((size_t)W * H, 0);
        screen_.settleBuf.assign((size_t)W * H, 0);
        screen_.dirtyMap.assign((size_t)cols * rows, 0);
        screen_.W = W;
        screen_.H = H;
    }
    screen_.valid = true;
    std::vector<uint8_t>& clean = screen_.clean;
    std::vector<uint8_t>& gray = screen_.gray;
    std::vector<uint8_t>& bw = screen_.bw;
    std::vector<uint8_t>& prevBw = screen_.prevBw;
    std::vector<uint8_t>& prevGray = screen_.prevGray;
    std::vector<uint8_t>& settleBuf = screen_.settleBuf;
    std::vector<uint8_t>& dirtyMap = screen_.dirtyMap;   // tiles redrawn by the fast path since their last settle
    HybridDither dither(W, H);
    CursorShape fallback = fallbackArrow();
    auto shapeNow = [&]() -> const CursorShape& { return dup.cursor().valid() ? dup.cursor() : fallback; };

    bool bwMode = cfg_.bw;
    bool haveFirst = false, firstEver = !resume;
    int64_t lastChange = nowNs(), lastFull = nowNs(), quietSince = nowNs();
    // A quick session (the tablet was turned / reconnected) must not wait for the desktop to settle for long: after a mode change Windows
    // re-lays the windows out and animates them for 1-2 s, and the first frame used to wait for all of that.  At the latest 0.9 s after the
    // duplication exists the first picture goes out; what still moves afterwards is sent as ordinary fast updates.
    int64_t firstDeadline = nowNs() + (quick ? 900000000LL : 12000000000LL);
    int64_t minFirstNs = nowNs() + (quick ? 300000000LL : 4500000000LL);     // Windows fades the new monitor's wallpaper in ~3 s after plug-in: let it finish first
    int64_t nextSettleNs = 0;
    int64_t lastBigFrameNs = 0;      // whole-panel gray frames are rationed: a flash storm helps nobody
    int settleSent = 0;              // settle boxes sent since the last real activity
    int64_t bigLast = 0, bigSince = 0;
    RECT bigRect{0, 0, 0, 0};
    uint64_t ghostPixels = 0;
    RECT curRect{0, 0, 0, 0};        // where the pointer is currently painted into `gray`
    std::string reason = "stopped";
    int lostRetries = 0;
    double ema = 0;
    int64_t lastLog = nowNs();
    uint64_t logUpdates = 0, logBytes = 0, logBig = 0, logFull = 0;

    // Binarize `rect`, send what changed as a fast (DU) update and remember the tiles for the settle pass.
    auto fastUpdate = [&](RECT rect, int64_t tAcq) -> bool {
        rect = alignTiles(rect, W, H);
        if (emptyRect(rect)) return true;
        RECT sendR{0, 0, 0, 0};
        if (bwMode) {
            dither.run(gray.data(), bw.data(), rect);
            sendR = diffTilesMap(bw.data(), prevBw.data(), W, rect, dirtyMap.data());
            copyRect(gray.data(), prevGray.data(), W, rect);     // what we have looked at (used to verify "whole-panel" claims)
            if (emptyRect(sendR)) return true;
            copyRect(bw.data(), prevBw.data(), W, rect);
            if (!sendMono(sendR, prevBw, W)) return false;
        } else {
            sendR = diffTilesMap(gray.data(), prevGray.data(), W, rect, dirtyMap.data());
            if (emptyRect(sendR)) return true;
            copyRect(gray.data(), prevGray.data(), W, rect);
            if (!sendGray(MODE_DU4, sendR, gray, W)) return false;
        }
        ghostPixels += (uint64_t)rectArea(sendR);
        lastChange = nowNs();
        settleSent = 0;
        double p = (nowNs() - tAcq) / 1e6;
        ema = ema == 0 ? p : ema * 0.9 + p * 0.1;
        stats.procMs = ema;
        return true;
    };
    auto fullFrame = [&](uint8_t mode, bool snapped) -> bool {
        RECT all{0, 0, W, H};
        const std::vector<uint8_t>* src = &gray;
        if (snapped) {
            snapForSettle(gray.data(), settleBuf.data(), W, all);
            src = &settleBuf;
        }
        if (!sendGray(mode, all, *src, W)) return false;
        dither.run(gray.data(), prevBw.data(), all);
        prevGray = gray;
        std::fill(dirtyMap.begin(), dirtyMap.end(), 0);
        ghostPixels = 0;
        bigRect = RECT{0, 0, 0, 0};
        lastChange = lastFull = nowNs();
        settleSent = 0;
        return true;
    };

    while (!stop) {
        if (!link_.connected()) {
            reason = link_.tabletLeft() ? "tablet left" : "tablet link closed";
            break;
        }
        if (geomChange_.exchange(false)) {          // the tablet was turned: the owner switches the virtual monitor and starts a new session of the new shape
            reason = "geometry change";
            break;
        }
        // automatic gray settle (partial GL16 refreshes): 0 = off, 1 = light (2 boxes after 3 s of calm), 2 = full (staged, after settleMs)
        const int level = settleLevel_.load();
        const int64_t settleDelayNs = level == 1 ? 3000000000LL : (int64_t)cfg_.settleMs * 1000000LL;
        const bool settleOn = level > 0 && settleDelayNs > 0;
        int want = bwRequested_.exchange(-1);
        if (want >= 0 && (want == 1) != bwMode) {
            bwMode = want == 1;
            fullRefresh_ = true;
            logf("session: mode -> %s", bwMode ? "1-bit fast" : "4-gray");
        }
        int64_t now = nowNs();
        UINT timeout = 100;
        auto dueIn = [&](int64_t atNs) { timeout = (UINT)std::max<int64_t>(1, std::min<int64_t>(timeout, (atNs - now) / 1000000 + 1)); };
        if (!haveFirst) dueIn(std::max(quietSince + (quick ? 200000000LL : 1000000000LL), minFirstNs));
        if (!emptyRect(bigRect)) dueIn(std::min(bigLast + 120000000LL, bigSince + 500000000LL));
        if (settleOn && std::any_of(dirtyMap.begin(), dirtyMap.end(), [](uint8_t v) { return v != 0; })) dueIn(std::max(lastChange + settleDelayNs, nextSettleNs));

        CapFrame f;
        CapResult cr = dup.acquire(timeout, f);
        if (cr == CapResult::Lost || cr == CapResult::Fail) {
            // desktop switch (UAC / lock screen) or mode change: re-create the duplication, retry for a while
            if (lostRetries == 0) logf("session: desktop duplication %s - re-initialising", cr == CapResult::Lost ? "lost" : "failed");
            Sleep(200);
            if (++lostRetries > 150) {
                reason = "desktop duplication lost";
                break;
            }
            if (!dup.init(cfg_.resolveGdi ? cfg_.resolveGdi() : cfg_.gdiName)) continue;      // the monitor may have a new name after a topology change
            if (dup.width() != W || dup.height() != H) {
                reason = "virtual monitor changed size";
                break;
            }
            haveFirst = false;                // re-sync everything with the next frames
            quietSince = nowNs();
            firstDeadline = nowNs() + 8000000000LL;
            minFirstNs = nowNs() + 500000000LL;
            continue;
        }
        lostRetries = 0;
        now = nowNs();
        if (cr == CapResult::Ok) {
            int64_t tAcq = now;
            RECT touch{0, 0, 0, 0};
            int64_t changedArea = 0;
            for (const RECT& r : f.rects) {
                bgraToGray(dup.bgra(), dup.stride(), clean.data(), W, r);
                touch = emptyRect(touch) ? r : unionRect(touch, r);
                changedArea += rectArea(r);
            }
            if (!f.rects.empty()) quietSince = now;
            if (cfg_.debug && changedArea > screen / 20) {
                static int64_t lastDbg = 0;
                if (now - lastDbg > 100000000LL) {
                    lastDbg = now;
                    logf("dbg frame: %zu rects, %.0f%% of the panel, bbox %ld,%ld-%ld,%ld, pointer moved=%d vis=%d at %d,%d", f.rects.size(), 100.0 * (double)changedArea / (double)screen, touch.left, touch.top, touch.right, touch.bottom, f.pointer.moved, f.pointer.visible, f.pointer.x, f.pointer.y);
                }
            }
            if (haveFirst) {
                // The pieces that must be redrawn: the changed desktop area, and (separately, so that a pointer jump does not
                // create one huge bounding box) the old and the new pointer rectangle.
                RECT imgPart = emptyRect(touch) ? RECT{0, 0, 0, 0} : alignTiles(touch, W, H);
                std::vector<RECT> parts;
                bool imgIsBig = changedArea > screen * 8 / 10;
                if (!emptyRect(imgPart)) parts.push_back(imgPart);
                if (cfg_.cursor && (f.pointer.moved || f.pointer.shapeUpdated)) {
                    if (!emptyRect(curRect)) parts.push_back(alignTiles(curRect, W, H));
                    RECT nr = f.pointer.visible ? cursorRect(shapeNow(), f.pointer.x, f.pointer.y, W, H) : RECT{0, 0, 0, 0};
                    if (!emptyRect(nr)) parts.push_back(alignTiles(nr, W, H));
                }
                if (!parts.empty()) {
                    for (const RECT& p : parts) copyRect(clean.data(), gray.data(), W, p);     // erases the old pointer
                    curRect = RECT{0, 0, 0, 0};
                    if (cfg_.cursor && f.pointer.visible) {
                        drawCursor(gray.data(), W, H, shapeNow(), f.pointer.x, f.pointer.y);
                        curRect = cursorRect(shapeNow(), f.pointer.x, f.pointer.y, W, H);
                    }
                    if (imgIsBig && !emptyRect(imgPart)) {
                        // DXGI says "everything changed" - believe it only if the pixels agree (a re-present of an unchanged desktop
                        // would otherwise cost a whole-panel refresh for nothing)
                        std::vector<uint8_t> changedTiles((size_t)cols * rows, 0);
                        diffTilesMap(gray.data(), prevGray.data(), W, imgPart, changedTiles.data());
                        long n = 0;
                        for (uint8_t v : changedTiles) n += v;
                        double real = 100.0 * (double)n / (double)(cols * rows);
                        if (real < 50.0) {
                            if (cfg_.debug) logf("dbg: DXGI reported %.0f%% but only %.0f%% of the tiles really changed - treated as a normal update", 100.0 * (double)changedArea / (double)screen, real);
                            imgIsBig = false;
                        }
                    }
                    bool failed = false;
                    for (size_t i = 0; i < parts.size() && !failed; ++i) {
                        if (i == 0 && !emptyRect(imgPart) && imgIsBig) {
                            // big transition (maximise, wallpaper fade, desktop switch): a burst of full-panel flashes looks awful and
                            // gains nothing, so wait until it calms down and send the end result once.  The pointer is NOT held back.
                            bigRect = emptyRect(bigRect) ? imgPart : unionRect(bigRect, imgPart);
                            bigLast = now;
                            if (!bigSince) bigSince = now;
                        } else if (!fastUpdate(parts[i], tAcq)) {
                            failed = true;
                        }
                    }
                    if (failed) {
                        reason = "send failed";
                        break;
                    }
                }
            }
            // before the first frame we only keep `clean` up to date and wait for the desktop to stop changing
        }
        now = nowNs();
        // the desktop is calm: first frame (or re-sync after a duplication loss)
        if (!haveFirst && ((now >= minFirstNs && now - quietSince >= (quick ? 200000000LL : 1000000000LL)) || now >= firstDeadline)) {
            RECT all{0, 0, W, H};
            bgraToGray(dup.bgra(), dup.stride(), clean.data(), W, all);
            gray = clean;
            curRect = RECT{0, 0, 0, 0};
            if (cfg_.cursor && dup.pointer().visible) {
                drawCursor(gray.data(), W, H, shapeNow(), dup.pointer().x, dup.pointer().y);
                curRect = cursorRect(shapeNow(), dup.pointer().x, dup.pointer().y, W, H);
            }
            bool synced;
            if (firstEver || cfg_.autoFull) synced = fullFrame(firstEver ? MODE_GC16_FULL : MODE_GL16, false);
            else synced = fastUpdate(RECT{0, 0, W, H}, nowNs());      // quiet re-sync: only what really differs, no whole-panel refresh
            if (!synced) {
                reason = "send failed";
                break;
            }
            logf("session: first frame sent (%s)", firstEver ? "full refresh" : cfg_.autoFull ? "re-sync, full" : resume ? "resumed after reconnect, changes only" : "re-sync, changes only");
            firstEver = false;
            haveFirst = true;
            continue;
        }
        if (!haveFirst) continue;
        // end of a big transition
        if (!emptyRect(bigRect) && (now - bigLast >= 120000000LL || now - bigSince >= 500000000LL)) {
            RECT r = bigRect;
            bigRect = RECT{0, 0, 0, 0};
            bigSince = 0;
            if (cfg_.autoFull && now - lastBigFrameNs >= 3000000000LL) {
                logf("session: full-panel change settled -> one gray refresh");
                // a whole-panel redraw is shown once, properly (GL16), instead of a 1-bit flash followed by a long clean-up
                if (!fullFrame(MODE_GL16, true)) {
                    reason = "send failed";
                    break;
                }
                lastBigFrameNs = nowNs();
            } else if (!fastUpdate(r, now)) {
                // by default (and for another one within 3 s) the finished picture goes out as an ordinary fast update: no whole-panel refresh
                reason = "send failed";
                break;
            }
        }
        stats.rttMs = link_.lastRttMs();
        if (now - lastLog > 5000000000LL) {
            uint64_t u = stats.updates.load(), b = stats.bytes.load();
            if (u != logUpdates)
                logf("session: %llu updates (%.1f/s) %.1f KB/s, proc %.1f ms, rtt %.1f ms, ping %.1f ms, settles %llu, big(>=50%%) %llu, full(>=90%%) %llu", (unsigned long long)(u - logUpdates),
                     (u - logUpdates) * 1e9 / (double)(now - lastLog), (b - logBytes) / 1024.0 * 1e9 / (double)(now - lastLog), stats.procMs.load(),
                     stats.rttMs.load(), link_.lastPingMs(), (unsigned long long)stats.settles.load(), (unsigned long long)(stats.big.load() - logBig), (unsigned long long)(stats.full.load() - logFull));
            logBig = stats.big.load();
            logFull = stats.full.load();
            logUpdates = u;
            logBytes = b;
            lastLog = now;
        }
        // settle: GL16 over the tiles that look worse in 1-bit than in gray (text edges, pictures), a few boxes at a time
        // Staged so the T1000 is not kept busy while the user is still working: 2 boxes after the settle delay, 5 after 4 s of calm,
        // everything after 10 s.
        int64_t idleNs = now - lastChange;
        int allowed = level == 1 ? (idleNs >= settleDelayNs ? 2 : 0)
                                 : idleNs >= 10000000000LL ? 1000 : idleNs >= 4000000000LL ? 3 : idleNs >= settleDelayNs ? 1 : 0;
        const int boxTiles = level == 1 ? cols * rows * 8 / 100 : maxSettleTiles;
        if (settleOn && allowed > settleSent && now >= nextSettleNs &&
            std::any_of(dirtyMap.begin(), dirtyMap.end(), [](uint8_t v) { return v != 0; })) {
            std::vector<uint8_t> flags((size_t)cols * rows, 0);
            for (int ty = 0; ty < rows; ++ty)
                for (int tx = 0; tx < cols; ++tx) {
                    size_t i = (size_t)ty * cols + tx;
                    if (!dirtyMap[i]) continue;
                    if (!bwMode || tileNeedsSettle(gray.data(), prevBw.data(), W, tx, ty)) flags[i] = 1;
                    else dirtyMap[i] = 0;                                        // exact already: nothing to improve
                }
            std::vector<RECT> boxes = clusterTiles(flags, cols, rows, boxTiles);
            if (!boxes.empty()) {
                // most valuable box first: tiles with hard edges (text) weigh 4, soft areas (photos, gradients) 1
                size_t best = 0;
                long bestScore = -1;
                for (size_t b = 0; b < boxes.size(); ++b) {
                    long score = 0;
                    for (int ty = boxes[b].top / kTile; ty < boxes[b].bottom / kTile; ++ty)
                        for (int tx = boxes[b].left / kTile; tx < boxes[b].right / kTile; ++tx)
                            if (flags[(size_t)ty * cols + tx]) score += tileContrast(gray.data(), W, tx, ty) >= 120 ? 4 : 1;
                    if (score > bestScore) {
                        bestScore = score;
                        best = b;
                    }
                }
                std::swap(boxes[0], boxes[best]);
                RECT r = boxes[0];
                snapForSettle(gray.data(), settleBuf.data(), W, r);
                if (!sendGray(MODE_GL16, r, settleBuf, W)) {
                    reason = "send failed";
                    break;
                }
                stats.settles++;
                for (int ty = r.top / kTile; ty < r.bottom / kTile; ++ty)
                    for (int tx = r.left / kTile; tx < r.right / kTile; ++tx) dirtyMap[(size_t)ty * cols + tx] = 0;
                settleSent++;
                nextSettleNs = nowNs() + 900000000LL;                           // let the T1000 finish the GL16 waveform
                if (boxes.size() > 1) logf("session: settle box %ldx%ld, %zu more waiting", r.right - r.left, r.bottom - r.top, boxes.size() - 1);
            }
        }
        // ghost clean-up: after a lot of 1-bit drawing, one calm moment gets a full (flashing) refresh
        if (cfg_.autoClean && ghostPixels > (uint64_t)screen * 12 && now - lastChange >= 8000000000LL) {
            logf("session: auto clean-up refresh");
            if (!fullFrame(MODE_GC16_FULL, false)) {
                reason = "send failed";
                break;
            }
        }
        if (fullRefresh_.exchange(false) || (cfg_.fullRefreshMinutes > 0 && (now - lastFull) / 60000000000LL >= cfg_.fullRefreshMinutes)) {
            logf("session: full refresh (manual / scheduled)");
            if (!fullFrame(MODE_GC16_FULL, false)) {
                reason = "send failed";
                break;
            }
        }
    }
    dup.shutdown();
    endThreadBoost(boost);
    return reason;
}

}  // namespace ph
