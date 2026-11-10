#!/usr/bin/env python3
"""Hand-held-camera friendly latency analysis: finds the e-ink screen and the reference window in every frame.

Layout assumed (as filmed so far, phone in portrait): tablet in the upper part of the frame, the reference window
(bright, on the PC monitor) in the lower part.  Both show the same big black/white square (latency_pattern.py --mode blink).
    python analyze_auto.py clip.mp4 [--split-y 630] [--period 1.2]
Prints, per reference flip, the delay until the e-ink square starts changing / is half / 90% changed, and statistics.
"""
import argparse
import statistics
import sys

sys.path.insert(0, r'D:\Workspace\MatePadPaper\host\poc')
sys.path.insert(0, r'D:\Workspace\MatePadPaper\.pydeps')
import numpy as np  # noqa: E402
import analyze_video as av  # noqa: E402

# the big square inside a window of width W / height H (same fractions for the 1872x1404 e-ink window and the scaled reference window)
SQ_X0, SQ_X1, SQ_Y0, SQ_Y1 = 0.107, 0.374, 0.214, 0.570


def bbox(mask, lo=3, hi=97):
    ys, xs = np.nonzero(mask)
    if len(xs) < 2000:
        return None
    return (np.percentile(xs, lo), np.percentile(ys, lo), np.percentile(xs, hi), np.percentile(ys, hi))


def window_box(box, bias=0.03, aspect=4.0 / 3.0):
    """Percentile box -> estimated true window (x0, y0, W, H): height drives the size (the hand only pollutes the right side)."""
    x0, y0, x1, y1 = box
    H = (y1 - y0) / (1 - 2 * bias)
    W = H * aspect
    return x0 - bias * W, y0 - bias * H, W, H


def roi_of(win):
    x0, y0, W, H = win
    cx0 = x0 + (SQ_X0 + 0.45 * (SQ_X1 - SQ_X0)) * W        # central ~45% of the square, away from its edges
    cx1 = x0 + (SQ_X1 - 0.45 * (SQ_X1 - SQ_X0)) * W
    cy0 = y0 + (SQ_Y0 + 0.30 * (SQ_Y1 - SQ_Y0)) * H
    cy1 = y0 + (SQ_Y1 - 0.30 * (SQ_Y1 - SQ_Y0)) * H
    return int(cx0), int(cy0), int(cx1), int(cy1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('video')
    ap.add_argument('--split-y', type=int, default=630, help='row separating the tablet (above) from the PC window (below)')
    ap.add_argument('--bright', type=int, default=192, help='gray level above which a pixel belongs to the bright reference window')
    ap.add_argument('--smooth', type=int, default=15, help='frames of median smoothing of the ROI position')
    args = ap.parse_args()

    meta = av.meta_of(args.video)
    fps = meta['fps']
    ms = 1000.0 / fps
    print('video: %dx%d @ %.2f fps, %.1f s' % (meta['size'][0], meta['size'][1], fps, meta.get('duration', 0)))
    rois_e, rois_r = [], []
    frames = []
    for fr in av.frames(args.video):
        top, bot = fr[:args.split_y], fr[args.split_y:]
        be = bbox((top > 135) & (top < 190))
        br = bbox(bot > args.bright)
        rois_e.append(roi_of(window_box(be)) if be else None)
        rois_r.append(roi_of(window_box((br[0], br[1], br[2], br[3]))) if br else None)
        # store only the means we need later: recompute ROI means after smoothing -> keep downsampled frame (1/2) for that
        frames.append(fr[::2, ::2].copy())
    n = len(frames)

    def smooth(rois, offset_y=0):
        arr = np.array([r if r else (np.nan,) * 4 for r in rois], dtype=float)
        out = np.empty_like(arr)
        for i in range(n):
            a, b = max(0, i - args.smooth // 2), min(n, i + args.smooth // 2 + 1)
            out[i] = np.nanmedian(arr[a:b], axis=0)
        return out
    se, sr = smooth(rois_e), smooth(rois_r)
    # ROI of the reference window was found in the bottom crop: shift y by split_y
    sr[:, 1] += args.split_y
    sr[:, 3] += args.split_y
    e, r = np.empty(n), np.empty(n)
    for i, fr2 in enumerate(frames):
        for arr, roi, series in ((e, se[i], 0), (r, sr[i], 1)):
            x0, y0, x1, y1 = [int(v) // 2 for v in roi]
            sub = fr2[max(0, y0):max(1, y1), max(0, x0):max(1, x1)]
            arr[i] = float(sub.mean()) if sub.size else np.nan
    ok = ~(np.isnan(e) | np.isnan(r))
    print('frames with both ROIs: %d / %d' % (int(ok.sum()), n))
    np.save(r'D:\Workspace\MatePadPaper\.tmp\auto_r.npy', r)
    np.save(r'D:\Workspace\MatePadPaper\.tmp\auto_e.npy', e)

    seg = int(10 * fps)
    rows = []
    for s0 in range(0, n - seg // 2, seg // 2):
        rs, es = r[s0:s0 + seg], e[s0:s0 + seg]
        if len(rs) < int(4 * fps) or np.isnan(rs).any() or np.isnan(es).any():
            continue
        if np.percentile(rs, 95) - np.percentile(rs, 5) < 40 or np.percentile(es, 95) - np.percentile(es, 5) < 25:
            continue
        for t, rising, lat in av.analyze(rs, es, fps):
            rows.append((s0 + t, rising, lat))
    rows.sort(key=lambda x: x[0])
    uniq = []
    for row in rows:
        if uniq and abs(row[0] - uniq[-1][0]) < 3:
            continue
        uniq.append(row)
    print('reference flips analysed: %d' % len(uniq))
    for name, k in (('starts changing', 0), ('half changed', 1), ('90% changed', 2)):
        v = sorted(l[2][k] for l in uniq if l[2][k] is not None and 100 < l[2][k] < 1500)
        if v:
            print('%-16s n=%3d  min %4.0f  p10 %4.0f  median %4.0f  mean %4.0f  p90 %4.0f  max %4.0f ms   (frame %.1f ms)'
                  % (name, len(v), v[0], v[len(v) // 10], statistics.median(v), statistics.mean(v), v[int(len(v) * 0.9)], v[-1], ms))
    return 0


if __name__ == '__main__':
    sys.exit(main())
