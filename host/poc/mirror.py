#!/usr/bin/env python3
"""Padreen PoC sender (Phase 1 prototype, Python).

Mirrors a rectangle of the primary monitor to the MatePad Paper over `adb forward` and prints a latency breakdown.
It only reads the screen; it changes no system setting. The production host (C++, virtual monitor + DXGI dirty
rects) replaces this, but speaks the same protocol (see android/app/.../Receiver.java).

    python mirror.py --seconds 30 --mode du4
"""
import argparse
import collections
import ctypes
import queue
import socket
import struct
import subprocess
import sys
import threading
import time
import zlib

sys.path.insert(0, r'D:\Workspace\MatePadPaper\.pydeps')
sys.path.insert(0, r'D:\Workspace\MatePadPaper\host\poc')
import numpy as np  # noqa: E402
import mss  # noqa: E402

ADB = r'D:\Android\android-sdk\platform-tools\adb.exe'
PORT = 38700
T_HELLO, T_UPDATE, T_ACK, T_PING, T_PONG, T_VERIFY, T_VERIFYR, T_BYE = 0x01, 0x10, 0x12, 0x20, 0x21, 0x30, 0x31, 0x7F
T_SELFTEST, T_SELFTESTR = 0x40, 0x41
T_RAWWAVE = 0x63
MODES = {'du4': 0, 'du': 1, 'gl16': 2, 'full': 3, 'du4b': 4}


def pack(t, payload=b''):
    return struct.pack('<BBHI', t, 0, 0, len(payload)) + payload


def recvn(sock, n):
    parts = []
    while n:
        c = sock.recv(n)
        if not c:
            raise EOFError('connection closed')
        parts.append(c)
        n -= len(c)
    return b''.join(parts)


def read_frame(sock):
    hdr = recvn(sock, 8)
    t, _f, _r, ln = struct.unpack('<BBHI', hdr)
    return t, recvn(sock, ln) if ln else b''


def run_adb(*args):
    return subprocess.run([ADB, *args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20).stdout.decode(errors='replace').strip()


class Stats:
    def __init__(self):
        self.lock = threading.Lock()
        self.pending = {}                       # seq -> dict(send_start, cap_done)
        self.acks = collections.deque(maxlen=4000)
        self.trace = []                         # per-update absolute times on the PC clock (ns), for --trace-csv
        self.offset = 0                         # tablet_ns - pc_ns
        self.vq = queue.Queue()                 # VERIFY replies
        self.sq = queue.Queue()                 # SELFTEST replies

    def on_ack(self, seq, th, tb, td, tdr, tp):
        with self.lock:
            p = self.pending.pop(seq, None)
        if p is None:
            return
        o = self.offset
        self.trace.append((seq, p['cap_done'], p['send_start'], th - o, tb - o, td - o, tdr - o, tp - o, p['bytes'], p.get('rect')))
        self.acks.append({
            'seq': seq,
            'send_to_hdr': ((th - o) - p['send_start']) / 1e6,
            'hdr_to_body': (tb - th) / 1e6,
            'decode': (td - tb) / 1e6,
            'draw': (tdr - td) / 1e6,
            'post': (tp - tdr) / 1e6,
            'cap_to_posted': ((tp - o) - p['cap_done']) / 1e6,
            'bytes': p['bytes'],
        })


def reader(sock, stats, stop):
    try:
        while not stop.is_set():
            t, pl = read_frame(sock)
            if t == T_ACK and len(pl) >= 44:
                seq, th, tb, td, tdr, tp = struct.unpack('<IQQQQQ', pl[:44])
                stats.on_ack(seq, th, tb, td, tdr, tp)
            elif t == T_VERIFYR:
                stats.vq.put(pl)
            elif t == T_SELFTESTR:
                stats.sq.put(pl)
    except Exception:
        pass


def selftest(sock, stats):
    """Tablet writes a synthetic pattern (R=x, G=y) through GraphicBuffer -> drawGraphic and reads the FB back."""
    while not stats.sq.empty():
        stats.sq.get_nowait()
    sock.sendall(pack(T_SELFTEST))
    try:
        pl = stats.sq.get(timeout=30)
    except queue.Empty:
        print('SELFTEST: no reply')
        return False
    txt = pl.decode('utf-8', 'replace')
    print('SELFTEST:', txt)
    return 'WRONG' not in txt and 'failed' not in txt


def verify(sock, stats, gray, x, y, w, h, label):
    """Ask the tablet to read its FB back and compare with what we sent (mean luma + CRC32)."""
    while not stats.vq.empty():
        stats.vq.get_nowait()
    sock.sendall(pack(T_VERIFY, struct.pack('<HHHH', x, y, w, h)))
    try:
        pl = stats.vq.get(timeout=8)
    except queue.Empty:
        print('VERIFY %s: no reply' % label)
        return False
    mean100, crc = struct.unpack('<qQ', pl[:16])
    region = np.ascontiguousarray(gray[y:y + h, x:x + w])
    my_mean = int(region.astype(np.uint64).sum() * 100 // (w * h))
    my_crc = zlib.crc32(region.tobytes()) & 0xFFFFFFFF
    ok = (mean100 == my_mean and crc == my_crc)
    print('VERIFY %-18s rect=(%d,%d %dx%d)  tablet mean=%.2f crc=%08x | pc mean=%.2f crc=%08x  -> %s'
          % (label, x, y, w, h, mean100 / 100.0, crc, my_mean / 100.0, my_crc, 'OK' if ok else 'MISMATCH'))
    return ok


def clock_sync(sock, stats, n=12):
    best = None
    for _ in range(n):
        t0 = time.perf_counter_ns()
        sock.sendall(pack(T_PING, struct.pack('<Q', t0)))
        while True:
            t, pl = read_frame(sock)
            if t == T_PONG:
                break
        t1 = time.perf_counter_ns()
        echo, tab = struct.unpack('<QQ', pl[:16])
        rtt = t1 - t0
        off = tab - (t0 + t1) // 2
        if best is None or rtt < best[0]:
            best = (rtt, off)
    stats.offset = best[1]
    return best[0] / 1e6


BAYER8 = np.array([[0, 32, 8, 40, 2, 34, 10, 42], [48, 16, 56, 24, 50, 18, 58, 26], [12, 44, 4, 36, 14, 46, 6, 38],
                   [60, 28, 52, 20, 62, 30, 54, 22], [3, 35, 11, 43, 1, 33, 9, 41], [51, 19, 59, 27, 49, 17, 57, 25],
                   [15, 47, 7, 39, 13, 45, 5, 37], [63, 31, 55, 23, 61, 29, 53, 21]], dtype=np.float32)


def make_bayer(H, W):
    t = (BAYER8 + 0.5) * (255.0 / 64.0)
    return np.tile(t, (H // 8 + 1, W // 8 + 1))[:H, :W].astype(np.float32)


def binarize(gray, method, thr, blk=6):
    """gray uint8 HxW -> uint8 {0,255}.  hybrid = per-6x6-block: high contrast (text/edges) -> adaptive threshold,
    flat dark/light -> snap, flat mid grays -> ordered (Bayer) dither.  Static input gives static output (no noise)."""
    H, W = gray.shape
    if method == 'thresh':
        return np.where(gray >= 128, 255, 0).astype(np.uint8)
    if method == 'bayer':
        return np.where(gray > thr, 255, 0).astype(np.uint8)
    g = gray.reshape(H // blk, blk, W // blk, blk)
    mx = g.max(axis=(1, 3)).astype(np.int16)
    mn = g.min(axis=(1, 3)).astype(np.int16)
    mid = (mx + mn) >> 1
    text = (mx - mn) > 64

    def up(m):
        return np.repeat(np.repeat(m, blk, axis=0), blk, axis=1)
    mid_f = up(mid.astype(np.uint8))
    out_text = gray >= mid_f
    out_flat = np.where(up(mid < 56), False, np.where(up(mid > 200), True, gray > thr))
    return np.where(np.where(up(text), out_text, out_flat), 255, 0).astype(np.uint8)


ARROW = [
    "X...........", "XX..........", "X.X.........", "X..X........", "X...X.......", "X....X......", "X.....X.....",
    "X......X....", "X.......X...", "X........X..", "X.....XXXXX.", "X..X..X.....", "X.X.X..X....", "XX..X..X....",
    "X....X..X...", ".....X..X...", "......X..X..", "......X..X..", ".......XX...",
]
ARROW_MASK = np.array([[c == 'X' for c in row] for row in ARROW], dtype=bool)
# white halo (dilate by 1) so the arrow stays visible on black and on white
_pad = np.pad(ARROW_MASK, 1)
ARROW_HALO = (_pad | np.roll(_pad, 1, 0) | np.roll(_pad, -1, 0) | np.roll(_pad, 1, 1) | np.roll(_pad, -1, 1))


class POINT(ctypes.Structure):
    _fields_ = [('x', ctypes.c_long), ('y', ctypes.c_long)]


class CURSORINFO(ctypes.Structure):
    _fields_ = [('cbSize', ctypes.c_uint32), ('flags', ctypes.c_uint32), ('hCursor', ctypes.c_void_p), ('ptScreenPos', POINT)]


def cursor_pos():
    """Screen position of the visible mouse pointer or None."""
    ci = CURSORINFO()
    ci.cbSize = ctypes.sizeof(ci)
    if not ctypes.windll.user32.GetCursorInfo(ctypes.byref(ci)) or not (ci.flags & 1):
        return None
    return ci.ptScreenPos.x, ci.ptScreenPos.y


def draw_cursor(gray, left, top):
    """Paint an arrow into the gray frame where the pointer is (mss does not capture the pointer)."""
    pos = cursor_pos()
    if pos is None:
        return
    x, y = pos[0] - left, pos[1] - top
    H, W = gray.shape

    def blit(mask, ox, oy, val):
        mh, mw = mask.shape
        x0, y0 = max(0, ox), max(0, oy)
        x1, y1 = min(W, ox + mw), min(H, oy + mh)
        if x1 <= x0 or y1 <= y0:
            return
        gray[y0:y1, x0:x1][mask[y0 - oy:y1 - oy, x0 - ox:x1 - ox]] = val
    blit(ARROW_HALO, x - 1, y - 1, 255)
    blit(ARROW_MASK, x, y, 0)

def pct(vals, p):
    if not vals:
        return float('nan')
    s = sorted(vals)
    return s[min(len(s) - 1, int(len(s) * p / 100.0))]


def summarize(stats, label):
    a = list(stats.acks)
    if not a:
        print('  (no ACKs yet)')
        return
    def col(k):
        v = [x[k] for x in a]
        return '%6.1f/%6.1f' % (pct(v, 50), pct(v, 95))
    print('  %s  n=%d   [median/p95 ms]  send->hdr %s | hdr->body %s | decode %s | draw %s | post %s | cap->posted %s | avg %.0f B'
          % (label, len(a), col('send_to_hdr'), col('hdr_to_body'), col('decode'), col('draw'), col('post'), col('cap_to_posted'),
             sum(x['bytes'] for x in a) / len(a)))


def main():
    import winperf
    print('process tuning:', winperf.boost())
    ap = argparse.ArgumentParser()
    ap.add_argument('--seconds', type=float, default=30)
    ap.add_argument('--mode', default='du4', choices=['du4', 'du', 'gl16', 'du4b'])
    ap.add_argument('--settle-ms', type=int, default=1200, help='GL16 pass after this much idle time (0 = never)')
    ap.add_argument('--left', type=int, default=0)
    ap.add_argument('--top', type=int, default=0)
    ap.add_argument('--tile', type=int, default=36)
    ap.add_argument('--no-launch', action='store_true')
    ap.add_argument('--bye', action='store_true', help='send BYE at the end (tablet closes the app)')
    ap.add_argument('--no-zlib', action='store_true')
    ap.add_argument('--verify', action='store_true', help='read the tablet FB back after the first frames and compare')
    ap.add_argument('--luma', action='store_true', help='weighted luma instead of the (much faster) green channel')
    ap.add_argument('--selftest', action='store_true', help='run the tablet FB self-test (synthetic pattern) first')
    ap.add_argument('--fast', default='gray', choices=['gray', 'bw'], help='bw = 1-bit DU fast updates (+ GL16 gray settle); gray = --mode waveform on gray8')
    ap.add_argument('--dither', default='hybrid', choices=['hybrid', 'thresh', 'bayer'])
    ap.add_argument('--vdd', action='store_true', help='plug in the Parsec virtual monitor (1872x1404) and mirror THAT instead of a region of a real monitor')
    ap.add_argument('--no-cursor', action='store_true', help='do not paint the mouse pointer into the frame')
    ap.add_argument('--quiet', action='store_true', help='no periodic statistics')
    ap.add_argument('--wave-schedule', default='', help='debug: cycle raw EinkMode values, e.g. 1:25,8:25,6:25 (value:seconds), starting --wave-start s after the mirror starts')
    ap.add_argument('--wave-start', type=float, default=14.0)
    ap.add_argument('--wave-log', default='', help='write the PC-clock time of every raw-wave change here')
    ap.add_argument('--trace-csv', default='', help='write per-update timestamps (PC clock, ns) here when finished')
    args = ap.parse_args()

    print('adb forward:', run_adb('forward', 'tcp:%d' % PORT, 'tcp:%d' % PORT) or 'ok')
    if not args.no_launch:
        run_adb('shell', 'am', 'start', '-n', 'com.padreen.app/.MainActivity')
        time.sleep(1.2)

    sock = None
    for i in range(30):
        try:
            sock = socket.create_connection(('127.0.0.1', PORT), timeout=3)
            t, pl = read_frame(sock)
            if t == T_HELLO:
                break
            sock.close()
            sock = None
        except Exception:
            sock = None
            time.sleep(0.4)
    if sock is None:
        print('cannot connect to the tablet app')
        return 1
    proto, W, H, dpi = struct.unpack('<HHHH', pl[:8])
    caps = struct.unpack('<I', pl[12:16])[0]
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)
    sock.settimeout(None)
    print('tablet: proto=%d panel=%dx%d dpi=%d caps=%d' % (proto, W, H, dpi, caps))

    stats = Stats()
    print('clock sync: best RTT %.2f ms' % clock_sync(sock, stats))
    stop = threading.Event()
    threading.Thread(target=reader, args=(sock, stats, stop), daemon=True).start()
    if args.selftest:
        selftest(sock, stats)

    mode = MODES[args.mode]
    ts = args.tile
    if W % ts or H % ts:
        ts = 36 if (W % 36 == 0 and H % 36 == 0) else 12
    rows, cols = H // ts, W // ts
    vdd = None
    if args.vdd:
        # plug the virtual monitor in only now that the tablet answered; it vanishes by itself ~1 s after we stop pinging
        import vdd as vddmod
        vdd = vddmod.Vdd()
        idx = vdd.add()
        name = None
        for _ in range(100):
            name = vddmod.find_virtual()
            if name:
                break
            time.sleep(0.1)
        if not name:
            print('the virtual monitor did not appear')
            vdd.close()
            return 1
        rc = vddmod.set_mode(name, W, H, 60, persist=True)
        time.sleep(1.0)
        rect = vddmod.display_rect(name)
        print('virtual monitor %s (index %s): mode rc=%s, rect=%s' % (name, idx, rc, rect))
        if not rect or rect[2] != W or rect[3] != H:
            print('unexpected virtual monitor size')
            vdd.close()
            return 1
        args.left, args.top = rect[0], rect[1]
    mon = {'left': args.left, 'top': args.top, 'width': W, 'height': H}

    seq = 0
    prev = None
    dirty = None           # union of changed rect since last settle: [x0,y0,x1,y1]
    last_change = time.perf_counter()
    t_end = time.perf_counter() + args.seconds if args.seconds > 0 else float('inf')
    loops = 0
    tcap = tdiff = tcomp = tsend = 0.0
    last_print = time.perf_counter()
    sent_bytes = 0
    sent_msgs = 0

    def send_update(gray, x0, y0, x1, y1, md, cap_done, mono=False):
        nonlocal seq, tcomp, tsend, sent_bytes, sent_msgs
        w, h = x1 - x0, y1 - y0
        region = np.ascontiguousarray(gray[y0:y1, x0:x1])
        t0 = time.perf_counter()
        if mono:     # 1 bit/pixel, rows byte aligned, MSB first, 1 = white
            codec, data = 2, np.packbits(region > 127, axis=1).tobytes()
        else:
            raw = region.tobytes()
            if args.no_zlib:
                codec, data = 0, raw
            else:
                comp = zlib.compress(raw, 1)
                codec, data = (1, comp) if len(comp) < len(raw) else (0, raw)
        t1 = time.perf_counter()
        seq += 1
        msg = pack(T_UPDATE, struct.pack('<IBBHHHHI', seq, md, codec, x0, y0, w, h, len(data)) + data)
        send_start = time.perf_counter_ns()
        with stats.lock:
            stats.pending[seq] = {'send_start': send_start, 'cap_done': cap_done, 'bytes': len(msg), 'rect': '%d,%d,%dx%d,m%d' % (x0, y0, w, h, md)}
        sock.sendall(msg)
        t2 = time.perf_counter()
        tcomp += t1 - t0
        tsend += t2 - t1
        sent_bytes += len(msg)
        sent_msgs += 1

    bw_mode = args.fast == 'bw'
    thr = make_bayer(H, W) if bw_mode else None
    print('mirroring %dx%d from monitor origin (%d,%d), %s, tile=%d ... Ctrl-C to stop'
          % (W, H, args.left, args.top, ('1-bit DU fast (%s dither) + GL16 gray settle' % args.dither) if bw_mode else 'mode=' + args.mode, ts))
    try:
        with mss.mss() as sct:
            sched = []
            if args.wave_schedule:
                t_acc = time.perf_counter() + args.wave_start
                for item in args.wave_schedule.split(','):
                    v, d = item.split(':')
                    sched.append((t_acc, int(v, 0)))
                    t_acc += float(d)
                sched.append((t_acc, -1))
            wlog = open(args.wave_log, 'w') if args.wave_log else None
            while time.perf_counter() < t_end:
                while sched and time.perf_counter() >= sched[0][0]:
                    _, wv = sched.pop(0)
                    sock.sendall(pack(T_RAWWAVE, struct.pack('<i', wv)))
                    print('raw wave ->', wv)
                    if wlog:
                        wlog.write('%d,%d\n' % (time.perf_counter_ns(), wv))
                        wlog.flush()
                t0 = time.perf_counter()
                shot = sct.grab(mon)
                a = np.frombuffer(shot.raw, dtype=np.uint8).reshape(H, W, 4)
                cap_done = time.perf_counter_ns()
                gray = ((a[:, :, 2].astype(np.uint16) * 77 + a[:, :, 1].astype(np.uint16) * 150 + a[:, :, 0].astype(np.uint16) * 29) >> 8).astype(np.uint8) if args.luma else np.ascontiguousarray(a[:, :, 1])
                if not args.no_cursor:
                    draw_cursor(gray, args.left, args.top)
                t1 = time.perf_counter()
                tcap += t1 - t0
                if prev is None:
                    send_update(gray, 0, 0, W, H, MODES['full'], cap_done)
                    prev = gray
                    if bw_mode:
                        prev_bw = binarize(gray, args.dither, thr)
                    last_change = time.perf_counter()
                    loops += 1
                    if args.verify:
                        time.sleep(0.5)
                        verify(sock, stats, gray, 0, 0, W, H, 'full frame')
                        verify(sock, stats, gray, 300, 200, 500, 400, 'sub-rect')
                    continue
                d = gray != prev                      # cheap gray diff first; the 1-bit conversion only runs on that rect
                tiles = d.reshape(rows, ts, cols, ts).any(axis=(1, 3))
                tdiff += time.perf_counter() - t1
                send_rect = None
                if tiles.any():
                    rr = np.where(tiles.any(axis=1))[0]
                    cc = np.where(tiles.any(axis=0))[0]
                    x0, x1 = int(cc[0]) * ts, (int(cc[-1]) + 1) * ts
                    y0, y1 = int(rr[0]) * ts, (int(rr[-1]) + 1) * ts
                    if bw_mode:
                        t2 = time.perf_counter()
                        bw_r = binarize(np.ascontiguousarray(gray[y0:y1, x0:x1]), args.dither, thr[y0:y1, x0:x1])
                        ch = (bw_r != prev_bw[y0:y1, x0:x1]).reshape((y1 - y0) // ts, ts, (x1 - x0) // ts, ts).any(axis=(1, 3))
                        prev_bw[y0:y1, x0:x1] = bw_r
                        tdiff += time.perf_counter() - t2
                        prev = gray
                        if ch.any():                  # tighten to the tiles whose 1-bit pixels really changed
                            r2 = np.where(ch.any(axis=1))[0]
                            c2 = np.where(ch.any(axis=0))[0]
                            nx0, nx1 = x0 + int(c2[0]) * ts, x0 + (int(c2[-1]) + 1) * ts
                            ny0, ny1 = y0 + int(r2[0]) * ts, y0 + (int(r2[-1]) + 1) * ts
                            send_update(prev_bw, nx0, ny0, nx1, ny1, MODES['du'], cap_done, mono=True)
                            send_rect = (nx0, ny0, nx1, ny1)
                    else:
                        send_update(gray, x0, y0, x1, y1, mode, cap_done)
                        prev = gray
                        send_rect = (x0, y0, x1, y1)
                if send_rect is not None:
                    last_change = time.perf_counter()
                    x0, y0, x1, y1 = send_rect
                    dirty = [x0, y0, x1, y1] if dirty is None else [min(dirty[0], x0), min(dirty[1], y0), max(dirty[2], x1), max(dirty[3], y1)]
                elif dirty is not None and args.settle_ms > 0 and (time.perf_counter() - last_change) * 1000 > args.settle_ms:
                    send_update(gray, dirty[0], dirty[1], dirty[2], dirty[3], MODES['gl16'], cap_done)
                    dirty = None
                else:
                    time.sleep(0.002)
                loops += 1
                now = time.perf_counter()
                if now - last_print > 4.0 and args.quiet:
                    loops = 0
                    tcap = tdiff = tcomp = tsend = 0.0
                    sent_bytes = sent_msgs = 0
                    last_print = now
                elif now - last_print > 4.0:
                    el = now - last_print
                    print('[%5.1fs] loops/s %.1f | grab+gray %.1f ms | diff %.1f ms | zlib %.1f ms | send %.1f ms | %d msgs, %.0f KB/s'
                          % (now - (t_end - args.seconds) if args.seconds > 0 else 0, loops / el, tcap / max(loops, 1) * 1000,
                             tdiff / max(loops, 1) * 1000, tcomp / max(sent_msgs, 1) * 1000, tsend / max(sent_msgs, 1) * 1000,
                             sent_msgs, sent_bytes / 1024.0 / el))
                    summarize(stats, 'tablet')
                    loops = 0
                    tcap = tdiff = tcomp = tsend = 0.0
                    sent_bytes = sent_msgs = 0
                    last_print = now
    except KeyboardInterrupt:
        pass
    finally:
        print('--- overall ---')
        summarize(stats, 'tablet')
        if args.trace_csv:
            with open(args.trace_csv, 'w') as f:
                f.write('seq,cap_done_ns,send_start_ns,tab_hdr_ns,tab_body_ns,tab_decoded_ns,tab_drawn_ns,tab_posted_ns,bytes,rect\n')
                for row in stats.trace:
                    f.write(','.join(str(v) for v in row) + '\n')
            print('trace written:', args.trace_csv, len(stats.trace), 'updates')
        if vdd is not None:
            vdd.close()
            print('virtual monitor removed')
        if args.bye:
            try:
                sock.sendall(pack(T_BYE))
            except Exception:
                pass
        stop.set()
        time.sleep(0.2)
        sock.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
