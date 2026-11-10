#!/usr/bin/env python3
"""Per-waveform latency from a (possibly slow-motion / long / hand-held) phone video + the logs of tools/latency-modes.ps1.

    python analyze_modes.py clip.mp4 --flips .tmp\\modes-flips.csv --waves .tmp\\modes-waves.csv [--scale 108] [--cache .tmp\\small.npy]

1. The video is decoded ONCE into small gray frames (shorter side --scale px, a few hundred MB for 10 minutes).
2. The two flipping squares (tablet above, reference window below) are located per 5 s chunk from the temporal variance inside the
   chunk, so a moving phone/tablet and the footage before/after the test do not matter; every frame is reduced to two brightness numbers.
3. Video flips are matched to the PC flip log by their irregular interval sequence (also detects a wrong time base, e.g. slow-motion
   files stored with 30 fps timestamps), so the wave mode in effect for every flip is known.
Latency = reference-window flip -> tablet square starts changing / half changed / 90% changed.
"""
import argparse
import os
import statistics
import subprocess
import sys

sys.path.insert(0, r'D:\Workspace\MatePadPaper\host\poc')
sys.path.insert(0, r'D:\Workspace\MatePadPaper\.pydeps')
import numpy as np  # noqa: E402
import imageio_ffmpeg  # noqa: E402
import analyze_video as av  # noqa: E402

WAVE_NAMES = {1: 'DU (1)', 8: 'A2 6-frame (8)', 9: 'A2 7-frame (9)', 10: 'A2 8-frame (10)', 11: 'A2 9-frame (11)', 6: 'A2/MIX (6)', 5: 'DU4 (5)', 3: 'GL16 (3)',
              -1: 'default'}
FFMPEG = imageio_ffmpeg.get_ffmpeg_exe()


def probe(path):
    rd = imageio_ffmpeg.read_frames(path, pix_fmt='gray', bits_per_pixel=8)
    meta = next(rd)
    rd.close()
    return meta


def decode_all(path, ow, oh, expected):
    """All frames of the (auto-rotated) video reduced to ow x oh gray -> uint8 array (n, oh, ow)."""
    vf = 'scale=%d:%d:flags=area,format=gray' % (ow, oh)
    cmd = [FFMPEG, '-hide_banner', '-loglevel', 'error', '-i', path, '-vf', vf, '-vsync', '0', '-f', 'rawvideo', '-pix_fmt', 'gray', '-']
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, bufsize=1 << 24)
    n = ow * oh
    out = np.empty((expected + 64, oh, ow), np.uint8)
    i = 0
    try:
        while True:
            buf = p.stdout.read(n)
            if len(buf) < n:
                break
            if i >= len(out):
                out = np.concatenate([out, np.empty((1024, oh, ow), np.uint8)])
            out[i] = np.frombuffer(buf, np.uint8).reshape(oh, ow)
            i += 1
    finally:
        p.stdout.close()
        p.kill()
        p.wait()
    return out[:i]


def two_blobs(var, min_var):
    """Rows (y0, y1) groups of strongly varying pixels -> ROIs of the two biggest blobs (upper = tablet, lower = reference), or None."""
    oh, ow = var.shape
    vmax = float(var.max())
    if vmax < min_var:
        return None
    gap = max(2, int(round(15 * oh / 640.0)))
    min_px = max(12, int(0.002 * ow * oh))
    for frac in (0.30, 0.18, 0.10, 0.05, 0.025):
        thr = max(frac * vmax, 0.2 * min_var)
        mask = var > thr
        rows = mask.sum(axis=1)
        ys = np.where(rows > 1)[0]
        if len(ys) == 0:
            continue
        groups, g0, prev = [], ys[0], ys[0]
        for y in ys[1:]:
            if y - prev > gap:
                groups.append((g0, prev))
                g0 = y
            prev = y
        groups.append((g0, prev))
        big = [g for g in groups if int(rows[g[0]:g[1] + 1].sum()) >= min_px]
        big = sorted(big, key=lambda g: -int(rows[g[0]:g[1] + 1].sum()))[:2]
        if len(big) == 2:
            big.sort()
            rois = []
            for (y0, y1) in big:
                cols = np.where(mask[y0:y1 + 1].sum(axis=0) > 1)[0]
                if len(cols) == 0:
                    rois = None
                    break
                x0, x1 = float(np.percentile(cols, 3)), float(np.percentile(cols, 97))
                cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
                hw, hh = max(1.5, (x1 - x0) * 0.25), max(1.5, (y1 - y0) * 0.25)
                rois.append((int(cx - hw), int(cy - hh), int(cx + hw) + 1, int(cy + hh) + 1))
            if rois is None:
                continue
            return rois[0], rois[1]
    return None


def chunk_series(frames, fps, chunk_s=5.0, hop_s=2.5, debug=None):
    """Brightness of the tablet square (e) and the reference square (r) per frame, ROIs tracked per chunk."""
    n = len(frames)
    cl = max(8, int(chunk_s * fps))
    hop = max(4, int(hop_s * fps))
    sub = max(1, int(round(fps / 12.0)))                     # variance from ~12 frames per second is plenty
    rois = {}
    for c0 in range(0, n, hop):
        c1 = min(n, c0 + cl)
        if c1 - c0 < cl // 2:
            break
        v = frames[c0:c1:sub].astype(np.float32).var(axis=0)
        r = two_blobs(v, min_var=1200.0)
        if r:
            rois[c0] = r
    if not rois:
        raise SystemExit('never saw two flipping squares (tablet + reference window); is the pattern in view?')
    keys = sorted(rois)
    e = np.full(n, np.nan)
    r = np.full(n, np.nan)
    for c0 in range(0, n, hop):
        k = min(keys, key=lambda x: abs(x - c0))              # nearest chunk that found the squares
        if abs(k - c0) > 3 * cl:
            continue                                         # far from any test footage
        (ex0, ey0, ex1, ey1), (rx0, ry0, rx1, ry1) = rois[k]
        c1 = min(n, c0 + hop)
        e[c0:c1] = frames[c0:c1, ey0:ey1, ex0:ex1].mean(axis=(1, 2))
        r[c0:c1] = frames[c0:c1, ry0:ry1, rx0:rx1].mean(axis=(1, 2))
    if debug:
        print('chunks with both squares: %d / %d' % (len(rois), (n + hop - 1) // hop))
    return r, e, rois


def win_diff(x, win):
    """D[t] = mean(x[t:t+win]) - mean(x[t-win:t]) (NaN-free x)."""
    c = np.concatenate([[0.0], np.cumsum(x)])
    n = len(x)
    t = np.arange(n)
    a = np.clip(t - win, 0, n)
    b = np.clip(t + win, 0, n)
    m = np.clip(t, 0, n)
    before = (c[m] - c[a]) / np.maximum(m - a, 1)
    after = (c[b] - c[m]) / np.maximum(b - m, 1)
    return after - before


def align_flips(r, fps, pc_t, pc_dir):
    """Offset (seconds) such that the PC flip i is seen in the reference square at video time (pc_t[i]-pc_t[0]) + offset.

    Direction-aware matching of the whole irregular flip sequence: only the right offset makes ALL flips agree."""
    win = max(1, int(round(0.12 * fps)))
    D = win_diff(np.nan_to_num(r, nan=float(np.nanmedian(r))), win)
    rel = (pc_t - pc_t[0]) * fps
    n = len(r)
    best, boff = -1e18, 0
    shift = int(0.06 * fps)                                   # the reference LCD shows the new frame ~60 ms after the drawing call
    span = float(pc_t[-1] - pc_t[0])
    for off in range(-int(0.5 * span * fps), n):                # the video may start up to half-way into the test
        idx = np.rint(rel + off + shift).astype(int)
        ok = (idx >= win) & (idx < n - win)
        if ok.sum() < 0.5 * len(pc_t):
            continue
        sc = float(np.clip(pc_dir[ok] * D[idx[ok]], -40, 80).sum())
        if sc > best:
            best, boff = sc, off
    return boff / fps, best, D


def refine_flip(r, fps, centre, win_s=0.3):
    """Sub-frame time (frames) at which the reference square crosses 50% near `centre` (frames); None if there is no flip."""
    i0, i1 = max(2, int(centre - win_s * fps)), min(len(r) - 2, int(centre + win_s * fps))
    if i1 - i0 < 4:
        return None
    pre = np.nanmedian(r[max(0, i0 - int(0.4 * fps)):i0 + 1])
    post = np.nanmedian(r[i1:min(len(r), i1 + int(0.4 * fps))])
    if not np.isfinite(pre) or not np.isfinite(post) or abs(post - pre) < 15:
        return None
    rising = post > pre
    return av.first_cross(np.clip((r - pre) / (post - pre), 0, 1), i0, 0.5, True, i1) if True else None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('video', nargs='?', default='')
    ap.add_argument('--flips', required=True)
    ap.add_argument('--waves', required=True)
    ap.add_argument('--scale', type=int, default=108, help='reduce the shorter side of the video to this many pixels')
    ap.add_argument('--time-scale', type=float, default=1.0, help='slow-motion factor: the video plays this many times slower than reality (4 = 120 fps captured, stored as 30 fps)')
    ap.add_argument('--cache', default='', help='save/load the decoded small frames (npy) so re-analysis does not decode again')
    args = ap.parse_args()

    pc = [(int(l.split(',')[0]) / 1e9, l.strip().split(',')[1]) for l in open(args.flips) if l.strip()]
    pc_t = np.array([t for t, _ in pc])
    pc_dir = np.array([-1.0 if c == 'black' else 1.0 for _, c in pc])       # the reference square gets darker on "black"
    waves = [(int(l.split(',')[0]) / 1e9, int(l.split(',')[1])) for l in open(args.waves) if l.strip()]
    print('PC flips: %d, wave changes: %s' % (len(pc), [(round(t - pc_t[0], 1), w) for t, w in waves]))

    meta = probe(args.video)
    dw, dh = meta['size']
    fps_file = float(meta['fps'])
    print('video: %dx%d (rotated) @ %.2f fps, %.1f s, rotate=%s' % (dw, dh, fps_file, meta.get('duration', 0), meta.get('rotate')))
    s = args.scale / float(min(dw, dh))
    ow, oh = int(dw * s) // 2 * 2, int(dh * s) // 2 * 2
    if args.cache and os.path.exists(args.cache):
        frames = np.load(args.cache)
        print('loaded %d cached frames %dx%d' % (len(frames), frames.shape[2], frames.shape[1]))
    else:
        print('decoding at %dx%d ...' % (ow, oh))
        frames = decode_all(args.video, ow, oh, int(meta.get('duration', 0) * fps_file) + 10)
        print('frames decoded: %d' % len(frames))
        if args.cache:
            np.save(args.cache, frames)
    r, e, rois = chunk_series(frames, fps_file, debug=True)
    fps = fps_file * args.time_scale                         # real frames per real second (slow motion: video plays slower than reality)
    ms = 1000.0 / fps
    off, score, D = align_flips(r, fps, pc_t, pc_dir)
    n_ok = 0
    print('alignment: PC flip #0 at video time %.2f s (score %.0f for %d flips)' % (off, score, len(pc_t)))

    def wave_at(t):
        cur = None
        for tw, w in waves:
            if tw <= t:
                cur = w
        return cur

    rows = {}
    ref_lag = []
    for i in range(len(pc_t)):
        centre = ((pc_t[i] - pc_t[0]) + off) * fps + 0.06 * fps
        if centre < 3 * fps or centre > len(r) - 2 * fps:
            continue
        w = wave_at(pc_t[i])
        if w is None:
            continue
        # reference flip time near the expected one (direction aware), then the tablet response relative to it
        i0, i1 = int(centre - 0.3 * fps), int(centre + 0.3 * fps)
        seg = pc_dir[i] * D[i0:i1]
        if len(seg) < 4 or seg.max() < 25:
            continue                                         # no flip visible there (camera moved / hand in the way)
        tc = i0 + int(np.argmax(seg))
        pre = np.nanmedian(r[max(0, tc - int(0.5 * fps)):max(1, tc - int(0.1 * fps))])
        post = np.nanmedian(r[tc + int(0.1 * fps):tc + int(0.5 * fps)])
        if not (np.isfinite(pre) and np.isfinite(post)) or abs(post - pre) < 15:
            continue
        t_ref = av.first_cross(np.clip((r - pre) / (post - pre), 0, 1), max(1, tc - int(0.3 * fps)), 0.5, True, tc + int(0.3 * fps))
        if t_ref is None:
            continue
        rising = pc_dir[i] > 0
        # tablet levels: before the flip and ~1 s later (the next flip is >= 1.2 s away; the e-ink needs < 0.8 s)
        ea = np.nanmedian(e[max(0, int(t_ref - 0.5 * fps)):max(1, int(t_ref - 0.05 * fps))])
        eb = np.nanmedian(e[int(t_ref + 0.95 * fps):int(t_ref + 1.15 * fps)])
        if not (np.isfinite(ea) and np.isfinite(eb)) or abs(eb - ea) < 50:          # tablet square must really swing (ghost / hand noise otherwise)
            rows.setdefault(w, []).append(None)
            continue
        en = np.clip((e - ea) / (eb - ea), 0, 1)
        i_a = int(t_ref) - 1
        i_b = min(len(e), int(t_ref + 1.2 * fps))
        start = av.first_cross(en, i_a, 0.1, True, i_b)
        half = av.first_cross(en, i_a, 0.5, True, i_b)
        done = av.first_cross(en, i_a, 0.9, True, i_b)
        if start is None or half is None or done is None:
            rows.setdefault(w, []).append(None)
            continue
        n_ok += 1
        rows.setdefault(w, []).append(((start - t_ref) * ms, (half - t_ref) * ms, (done - t_ref) * ms))
    print('flips measured: %d of %d' % (n_ok, len(pc_t)))
    print('\n%-18s %4s %5s | %-28s | %-28s | %s' % ('mode', 'n', 'miss', 'starts changing (ms)', 'half changed (ms)', '90% changed (ms)'))
    for w, lst in sorted(rows.items(), key=lambda kv: [x[0] for x in waves].index(next(tw for tw, ww in waves if ww == kv[0]))):
        ok2 = [x for x in lst if x is not None and 120 < x[0] < 1000 and (x[2] - x[0]) < 500]    # physically plausible, clean transitions only
        miss = len(lst) - len(ok2)
        if not ok2:
            print('%-18s %4d %5d | no usable flips' % (WAVE_NAMES.get(w, str(w)), len(lst), miss))
            continue

        def st(idx):
            v = sorted(x[idx] for x in ok2)
            q = lambda f: v[min(len(v) - 1, int(f * (len(v) - 1) + 0.5))]
            return 'med %4.0f  p25 %4.0f  p75 %4.0f' % (statistics.median(v), q(0.25), q(0.75))
        print('%-18s %4d %5d | %-28s | %-28s | %s' % (WAVE_NAMES.get(w, str(w)), len(ok2), miss, st(0), st(1), st(2)))
    print('(frame resolution %.1f ms; the reference-window display lag, ~50-70 ms, is NOT subtracted)' % ms)
    return 0

if __name__ == '__main__':
    sys.exit(main())
