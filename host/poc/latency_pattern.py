#!/usr/bin/env python3
"""End-to-end latency test pattern for a phone-camera measurement (needs the mirror running: mirror.py --vdd ...).

Two windows are driven from the SAME timer tick:
  * a reference window on the primary monitor (Dell/Surface) - the "PC truth"
  * a full-screen window on the virtual monitor - what the e-ink mirrors
Film both (phone, 60 fps or better, e-ink tablet next to the reference window).  Frame-by-frame, the time between
the reference square changing and the e-ink square changing is the end-to-end latency (software chain + SurfaceFlinger +
panel waveform).  Modes:
    blink    one big square flips black<->white every --period seconds (isolated events, like a keystroke after a pause)
    counter  16-bit binary counter in 90 px squares, +1 every 20 ms (continuous load; decode both rows offline)
Close with Esc or Ctrl-C.
"""
import argparse
import ctypes
import os
import sys
import time
import tkinter as tk

try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)      # physical pixels, so window geometry == pixels on every monitor
except Exception:
    ctypes.windll.user32.SetProcessDPIAware()
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vdd as vddmod  # noqa: E402
import winperf  # noqa: E402


def main():
    print('process tuning:', winperf.boost())
    ap = argparse.ArgumentParser()
    ap.add_argument('--mode', default='blink', choices=['blink', 'counter'])
    ap.add_argument('--period', type=float, default=1.2, help='blink: seconds between flips')
    ap.add_argument('--ref-x', type=int, default=200)
    ap.add_argument('--ref-y', type=int, default=150)
    ap.add_argument('--ref-scale', type=float, default=0.5, help='size of the reference window relative to the e-ink one')
    ap.add_argument('--seconds', type=float, default=0, help='0 = until Esc')
    ap.add_argument('--jitter', type=float, default=0.0, help='blink: add a seeded pseudo-random 0..jitter seconds to every interval (makes the flip sequence unique for video alignment)')
    ap.add_argument('--warmup', type=float, default=0.0, help='seconds of warm-up shown to the person filming before the first phase')
    ap.add_argument('--phase-names', default='', help='semicolon separated names of the phases that follow the warm-up')
    ap.add_argument('--phase-seconds', type=float, default=25.0)
    ap.add_argument('--flip-log', default='', help='append the absolute time.perf_counter_ns() of every flip to this file')
    args = ap.parse_args()

    name = vddmod.find_virtual()
    rect = vddmod.display_rect(name) if name else None
    if not rect:
        print('virtual monitor not found - start mirror.py --vdd first (it must keep running)')
        return 1
    vx, vy, vw, vh = rect
    sc = args.ref_scale
    rw, rh = int(vw * sc), int(vh * sc)

    root = tk.Tk()
    root.withdraw()

    def make(x, y, w, h, scale, border):
        win = tk.Toplevel(root)
        win.overrideredirect(True)
        win.attributes('-topmost', True)
        win.geometry('%dx%d+%d+%d' % (w, h, x, y))
        cv = tk.Canvas(win, width=w, height=h, bg='white', highlightthickness=border, highlightbackground='black')
        cv.pack()
        win.bind('<Escape>', lambda e: root.destroy())
        win.focus_force()
        return win, cv

    tab, ct = make(vx, vy, vw, vh, 1.0, 0)
    ref, cr = make(args.ref_x, args.ref_y, rw, rh + 120, sc, 4)
    cr.config(height=rh)
    status = tk.Label(ref, text='', font=('Microsoft YaHei UI', 15), justify='left', anchor='w', bg='white', wraplength=rw - 20)
    status.place(x=10, y=rh + 4, width=rw - 20, height=112)

    items = []   # (canvas, rect id) pairs

    def build(cv, scale):
        ids = []
        if args.mode == 'blink':
            s = int(500 * scale)
            x0, y0 = int(200 * scale), int(300 * scale)
            ids.append(cv.create_rectangle(x0, y0, x0 + s, y0 + s, fill='white', outline='black', width=max(2, int(6 * scale))))
            # always-black locator in the corner (lets the video analysis find the window)
            cv.create_rectangle(int(30 * scale), int(30 * scale), int(130 * scale), int(130 * scale), fill='black', outline='')
        else:
            q = int(90 * scale)
            gap = int(6 * scale)
            x0, y0 = int(40 * scale), int(300 * scale)
            for i in range(16):
                ids.append(cv.create_rectangle(x0 + i * (q + gap), y0, x0 + i * (q + gap) + q, y0 + q, fill='white', outline='black', width=max(1, int(3 * scale))))
            cv.create_rectangle(int(30 * scale), int(30 * scale), int(130 * scale), int(130 * scale), fill='black', outline='')
        return ids

    ids_t = build(ct, 1.0)
    ids_r = build(cr, sc)
    import random
    rng = random.Random(20261102)
    state = {'last': None, 'k': 0, 'next': args.period}
    flog = open(args.flip_log, 'a') if args.flip_log else None
    t0 = time.perf_counter()
    t_end = t0 + args.seconds if args.seconds > 0 else float('inf')

    def tick():
        now = time.perf_counter()
        if now > t_end:
            root.destroy()
            return
        el = now - t0
        if args.warmup or args.phase_names:
            names = [n for n in args.phase_names.split(';') if n]
            total = args.warmup + len(names) * args.phase_seconds
            if args.seconds > 0:
                total = min(total, args.seconds)
            left = max(0, total - el)
            if el < args.warmup:
                status.config(text='预热中：请现在开始录像，手机架稳别动，同时拍到本窗口和平板。\n第一个测试约 %d 秒后开始；总共还剩 %d:%02d' % (args.warmup - el, left // 60, left % 60))
            elif el < total:
                i = min(len(names) - 1, int((el - args.warmup) // args.phase_seconds))
                status.config(text='测试进行中（%d/%d）：%s\n请继续录像；总共还剩 %d:%02d，窗口消失后再停止录像。' % (i + 1, len(names), names[i], left // 60, left % 60))
            else:
                status.config(text='测试结束，现在可以停止录像了。')
        if args.mode == 'blink' and args.jitter > 0:
            while el >= state['next']:
                state['k'] += 1
                state['next'] += args.period + rng.random() * args.jitter
            v = state['k'] & 1
            if v != state['last']:
                col = 'black' if v else 'white'
                ct.itemconfig(ids_t[0], fill=col)
                cr.itemconfig(ids_r[0], fill=col)
                root.update_idletasks()               # paint now
                state['last'] = v
                tns = time.perf_counter_ns()
                if flog:
                    flog.write('%d,%s\n' % (tns, col))
                    flog.flush()
                print('%.3f flip -> %s' % (el, col))
        elif args.mode == 'blink':
            v = int(el / args.period) & 1
            if v != state['last']:
                col = 'black' if v else 'white'
                ct.itemconfig(ids_t[0], fill=col)
                cr.itemconfig(ids_r[0], fill=col)
                root.update_idletasks()               # paint now
                state['last'] = v
                tns = time.perf_counter_ns()
                if flog:
                    flog.write('%d,%s\n' % (tns, col))
                    flog.flush()
                print('%.3f flip -> %s' % (el, col))
        else:
            v = int(el / 0.02) & 0xFFFF
            if v != state['last']:
                for i in range(16):
                    col = 'black' if (v >> i) & 1 else 'white'
                    ct.itemconfig(ids_t[i], fill=col)
                    cr.itemconfig(ids_r[i], fill=col)
                state['last'] = v
        root.after(1, tick)

    root.after(10, tick)
    print('pattern running (%s). e-ink window %dx%d at (%d,%d); reference window %dx%d at (%d,%d). Esc to quit.'
          % (args.mode, vw, vh, vx, vy, rw, rh, args.ref_x, args.ref_y))
    try:
        root.mainloop()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
