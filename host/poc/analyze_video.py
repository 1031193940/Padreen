#!/usr/bin/env python3
"""Measure end-to-end latency from a phone video that shows the reference window (PC) and the e-ink tablet together.

Workflow (run latency_pattern.py --mode blink while mirror.py --vdd is running, film both screens):
    python analyze_video.py clip.mp4 --dump 100 frame100.png          # look at the picture, pick two boxes
    python analyze_video.py clip.mp4 --ref x,y,w,h --eink x,y,w,h     # x,y,w,h in video pixels, inside the big squares
Prints, per flip of the reference square, the delay until the e-ink square starts changing / is half changed / is 90% done.
"""
import argparse
import statistics
import sys

sys.path.insert(0, r'D:\Workspace\MatePadPaper\.pydeps')
import numpy as np  # noqa: E402
import imageio_ffmpeg  # noqa: E402


def box(s):
    x, y, w, h = [int(v) for v in s.split(',')]
    return x, y, w, h


def frames(path):
    rd = imageio_ffmpeg.read_frames(path, pix_fmt='gray', bits_per_pixel=8)
    meta = next(rd)
    w, h = meta['size']
    fps = meta['fps']
    for raw in rd:
        yield np.frombuffer(raw, np.uint8).reshape(h, w)
    return


def meta_of(path):
    rd = imageio_ffmpeg.read_frames(path, pix_fmt='gray', bits_per_pixel=8)
    meta = next(rd)
    rd.close()
    return meta


def series(path, ref, eink):
    r, e = [], []
    for fr in frames(path):
        x, y, w, h = ref
        r.append(float(fr[y:y + h, x:x + w].mean()))
        x, y, w, h = eink
        e.append(float(fr[y:y + h, x:x + w].mean()))
    return np.array(r), np.array(e)


def norm(a):
    lo, hi = np.percentile(a, 5), np.percentile(a, 95)
    return np.clip((a - lo) / max(hi - lo, 1e-6), 0, 1)


def crossings(a, level=0.5):
    """Sub-frame times (in frames) where the normalised series crosses `level`; +1 rising, -1 falling."""
    out = []
    for i in range(1, len(a)):
        if (a[i - 1] < level) != (a[i] < level):
            f = (level - a[i - 1]) / (a[i] - a[i - 1]) if a[i] != a[i - 1] else 0.5
            out.append((i - 1 + f, 1 if a[i] > a[i - 1] else -1))
    return out


def first_cross(e, i0, level, rising, i1):
    """First frame index >= i0 where e reaches `level` moving in the requested direction; interpolated."""
    for i in range(max(1, i0), min(len(e), i1)):
        a, b = e[i - 1], e[i]
        hit = (a < level <= b) if rising else (a > level >= b)
        if hit:
            return i - 1 + (level - a) / (b - a)
    return None


def analyze(r, e, fps):
    r, e = norm(r), norm(e)
    ms = 1000.0 / fps
    rows = []
    for t_ref, d in crossings(r):
        rising = d > 0
        # e-ink: window from just before the reference flip to 1.5 s later (or until the next reference flip)
        i0 = int(t_ref) - 1
        i1 = min(len(e), int(t_ref + 1.5 * fps))
        base = e[max(0, i0 - 2):i0 + 1].mean() if i0 > 0 else e[0]
        # start: leaves the starting level by 10% of the swing; 50%; 90% of the way to the final level
        start = first_cross(e, i0, 0.1, rising, i1) if rising else first_cross(e, i0, 0.9, rising, i1)
        half = first_cross(e, i0, 0.5, rising, i1)
        done = first_cross(e, i0, 0.9, rising, i1) if rising else first_cross(e, i0, 0.1, rising, i1)
        rows.append((t_ref, rising, [None if v is None else (v - t_ref) * ms for v in (start, half, done)]))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('video')
    ap.add_argument('--ref', type=box)
    ap.add_argument('--eink', type=box)
    ap.add_argument('--dump', nargs=2, metavar=('FRAME', 'PNG'))
    args = ap.parse_args()
    m = meta_of(args.video)
    print('video: %dx%d @ %.2f fps, %.1f s' % (m['size'][0], m['size'][1], m['fps'], m.get('duration', 0)))
    if args.dump:
        n = int(args.dump[0])
        from PIL import Image
        rd = imageio_ffmpeg.read_frames(args.video, pix_fmt='rgb24')
        meta = next(rd)
        w, h = meta['size']
        for i, raw in enumerate(rd):
            if i == n:
                Image.frombytes('RGB', (w, h), raw).save(args.dump[1])
                print('saved frame', n, 'to', args.dump[1])
                break
        rd.close()
        return 0
    if not args.ref or not args.eink:
        print('give --ref and --eink boxes (see --dump)')
        return 1
    r, e = series(args.video, args.ref, args.eink)
    rows = analyze(r, e, m['fps'])
    print('%d reference flips found' % len(rows))
    cols = [[], [], []]
    for t, rising, lat in rows:
        print('  t=%6.2f s  %-7s start %-8s 50%% %-8s 90%% %-8s' % (t / m['fps'], 'to black' if not rising else 'to white',
              *[('%.0f ms' % v) if v is not None else '-' for v in lat]))
        for i, v in enumerate(lat):
            if v is not None and 0 <= v < 1500:
                cols[i].append(v)
    for name, c in zip(('starts changing', 'half changed', '90% changed'), cols):
        if c:
            c.sort()
            print('%-16s n=%2d  median %.0f ms  min %.0f  max %.0f  (frame = %.1f ms)' % (name, len(c), statistics.median(c), c[0], c[-1], 1000.0 / m['fps']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
