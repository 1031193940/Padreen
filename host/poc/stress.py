#!/usr/bin/env python3
"""T1000 / e-ink stress harness (synthetic content only, never touches the desktop).

Sends scripted UPDATE traffic to the Padreen app and, after every scenario, scans a *streamed* logcat file
(`adb logcat -v threadtime > file`, started by the caller before this script) for T1000 resets
("wait readypin timeout,reset t1000") and I2C errors ("Remote I/O error").  The tablet is the only thing observed here;
the point is to find which update patterns the panel controller survives.

    python stress.py --log D:\\...\\lc-stream.txt --plan "flip:w=900,h=720,hz=30,sec=8,mode=du4;idle:sec=3"

Scenario kinds (all rect coordinates are multiples of 36, like the real host's tiles):
    flip : checkerboard (36 px cells) in the rect, inverted every frame (worst case: every pixel switches every frame)
    grad : diagonal gradient that moves every frame (all pixels change, compresses well)
    drag : a 'window' (w x h) moving over a static page; each frame updates old|new union (what a real drag does)
    full : like flip but always the full 1872x1404 panel
    idle : send nothing
Common keys: hz (frames/s), sec, mode (du4|du|gl16|full|du4b), x, y, w, h, step (drag, px per frame)
"""
import argparse
import os
import queue
import re
import socket
import struct
import subprocess
import sys
import threading
import time
import zlib

sys.path.insert(0, r'D:\Workspace\MatePadPaper\.pydeps')
import numpy as np  # noqa: E402

ADB = r'D:\Android\android-sdk\platform-tools\adb.exe'
PORT = 38700
T_HELLO, T_UPDATE, T_ACK, T_PING, T_PONG, T_BYE = 0x01, 0x10, 0x12, 0x20, 0x21, 0x7F
T_MARK, T_WAKE, T_WAKER, T_RAWWAVE = 0x60, 0x61, 0x62, 0x63
MODES = {'du4': 0, 'du': 1, 'gl16': 2, 'full': 3, 'du4b': 4}
WAVE = {'du4': 5, 'du': 1, 'gl16': 3, 'full': 98, 'du4b': 7}      # raw waveform ids for the WAKE probe
W, H = 1872, 1404


def pack(t, payload=b''):
    return struct.pack('<BBHI', t, 0, 0, len(payload)) + payload


def recvn(sock, n):
    parts = []
    while n:
        c = sock.recv(n)
        if not c:
            raise EOFError('closed')
        parts.append(c)
        n -= len(c)
    return b''.join(parts)


def adb(*args):
    return subprocess.run([ADB, *args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30).stdout.decode(errors='replace').strip()


class Acks:
    def __init__(self):
        self.lock = threading.Lock()
        self.sent = {}          # seq -> perf_counter at send start
        self.lat = []           # (seq, ack latency s, tablet post ms)
        self.last_ack = time.perf_counter()
        self.wake = queue.Queue()

    def on(self, seq, post_ms):
        now = time.perf_counter()
        with self.lock:
            t0 = self.sent.pop(seq, None)
            if t0 is not None:
                self.lat.append((seq, now - t0, post_ms))
            self.last_ack = now


def reader(sock, acks, stop):
    """Drain tablet->PC frames. Silence is normal (idle scenarios): receive timeouts must not end the loop."""
    def rd(n):
        parts = []
        while n:
            try:
                c = sock.recv(n)
            except socket.timeout:
                if stop.is_set():
                    raise EOFError('stopped')
                continue
            if not c:
                raise EOFError('closed')
            parts.append(c)
            n -= len(c)
        return b''.join(parts)
    try:
        while not stop.is_set():
            hdr = rd(8)
            t, _f, _r, ln = struct.unpack('<BBHI', hdr)
            pl = rd(ln) if ln else b''
            if t == T_ACK and len(pl) >= 44:
                seq, th, tb, td, tdr, tp = struct.unpack('<IQQQQQ', pl[:44])
                acks.on(seq, (tp - tdr) / 1e6)
            elif t == T_WAKER:
                acks.wake.put(pl.decode('utf-8', 'replace'))
    except Exception:
        pass


def parse_kv(s):
    d = {}
    for part in s.split(','):
        if '=' in part:
            k, v = part.split('=', 1)
            d[k.strip()] = v.strip()
    return d


def checker(w, h, phase):
    yy, xx = np.indices((h, w))
    return np.where((((xx // 36) + (yy // 36) + phase) & 1) == 0, 0, 255).astype(np.uint8)


def grad(w, h, phase):
    yy, xx = np.indices((h, w))
    return ((xx + yy + phase * 29) & 255).astype(np.uint8)


class Runner:
    def __init__(self, sock, acks):
        self.sock = sock
        self.acks = acks
        self.seq = 0
        self.sent = 0
        self.t_send_total = 0.0
        self.notes = []
        self.checkflip = 0

    def mark(self, text):
        self.sock.sendall(pack(T_MARK, text.encode('utf-8')))

    def send_rect(self, gray, x, y, mode):
        h, w = gray.shape
        raw = np.ascontiguousarray(gray).tobytes()
        comp = zlib.compress(raw, 1)
        codec, data = (1, comp) if len(comp) < len(raw) else (0, raw)
        self.seq += 1
        msg = pack(T_UPDATE, struct.pack('<IBBHHHHI', self.seq, mode, codec, x, y, w, h, len(data)) + data)
        t0 = time.perf_counter()
        with self.acks.lock:
            self.acks.sent[self.seq] = t0
        self.sock.sendall(msg)
        self.t_send_total += time.perf_counter() - t0
        self.sent += 1

    def run(self, kind, kv):
        hz = float(kv.get('hz', 30))
        sec = float(kv.get('sec', 5))
        mode = MODES[kv.get('mode', 'du4')]
        x = int(kv.get('x', 972)) // 36 * 36
        y = int(kv.get('y', 252)) // 36 * 36
        w = int(kv.get('w', 900)) // 36 * 36
        h = int(kv.get('h', 720)) // 36 * 36
        step = int(kv.get('step', 36)) // 36 * 36 or 36
        if kind == 'idle':
            time.sleep(sec)
            return
        if kind == 'raw':
            # debug: use this raw EinkMode value (decimal or 0x..) for every following post; -1 restores protocol modes
            wave = int(kv.get('wave', '-1'), 0)
            self.sock.sendall(pack(T_RAWWAVE, struct.pack('<i', wave)))
            time.sleep(0.2)
            return
        if kind == 'wake':
            # WAKE probe: method 1 forceRefresh, 2 fb.invalidate, 3 fb.post (control), 4 normal view refresh.
            # Then, after `settle` seconds, an ordinary FB post shows whether the T1000 really is awake: if it is not,
            # the SF thread logs "wait display ready error" ~3 s later (and drops the update).
            method = int(kv.get('method', 3))
            wave = WAVE[kv.get('mode', 'du4')]
            self.mark('wake method %d' % method)
            self.sock.sendall(pack(T_WAKE, bytes([method, wave])))
            try:
                self.notes.append(self.acks.wake.get(timeout=float(kv.get('wait', 20))))
            except queue.Empty:
                self.notes.append('wake: no reply')
            settle = float(kv.get('settle', 0.5))
            time.sleep(settle)
            if int(kv.get('check', 1)):
                self.mark('check post after settle %.2f' % settle)
                tile = np.full((36, 36), 0 if (self.checkflip & 1) else 255, np.uint8)
                self.checkflip += 1
                self.send_rect(tile, W - 108, H - 36, MODES['du4'])
                time.sleep(float(kv.get('checkwait', 4.5)))
            return
        if kind == 'keep':
            # keep-alive candidate: tiny corner tile toggling by dv gray levels every `period` seconds
            period = float(kv.get('period', 5))
            dv = int(kv.get('dv', 1))
            t_start = time.perf_counter()
            n = 0
            while time.perf_counter() - t_start < sec:
                tile = np.full((36, 36), 255 - dv * (n & 1), np.uint8)
                self.send_rect(tile, W - 36, H - 36, mode)
                n += 1
                time.sleep(period)
            return
        if kind == 'full':
            x, y, w, h = 0, 0, W, H
        period = 1.0 / hz
        t_start = time.perf_counter()
        n = 0
        # drag state
        page = np.full((H, W), 255, np.uint8)
        px, py, dx, dy = x, y, step, step
        win = np.full((h, w), 70, np.uint8)
        win[:36, :] = 0
        win[36::72, 36:-36] = 200          # text-like stripes
        while True:
            now = time.perf_counter()
            if now - t_start >= sec:
                break
            due = t_start + n * period
            if now < due:
                time.sleep(max(0.0, due - now - 0.0005))
                continue
            if kind == 'flip':
                self.send_rect(checker(w, h, n & 1), x, y, mode)
            elif kind == 'full':
                self.send_rect(checker(W, H, n & 1), 0, 0, mode)
            elif kind == 'grad':
                self.send_rect(grad(w, h, n), x, y, mode)
            elif kind == 'drag':
                ox, oy = px, py
                px += dx
                py += dy
                if px < 0 or px + w > W:
                    dx = -dx
                    px += 2 * dx
                if py < 0 or py + h > H:
                    dy = -dy
                    py += 2 * dy
                page[:] = 255
                page[py:py + h, px:px + w] = win
                ux0, uy0 = min(ox, px), min(oy, py)
                ux1, uy1 = max(ox, px) + w, max(oy, py) + h
                self.send_rect(page[uy0:uy1, ux0:ux1], ux0, uy0, mode)
            else:
                raise SystemExit('unknown scenario ' + kind)
            n += 1


RESET_RX = re.compile(r'wait readypin timeout|reset t1000')
I2C_RX = re.compile(r'Remote I/O error')
SLEEP_RX = re.compile(r'sleep or poweroff|wait display ready error')
NORMAL_RX = re.compile(r'handleRefresh enter')
HANG_RX = re.compile(r'ioctl err 11\b')
GOOD_RX = re.compile(r'set_mipi_mode success')
POWER_RX = re.compile(r'POWER t1000=(-?\d+) pmic=(-?\d+)')
TS_RX = re.compile(r'^\d\d-\d\d (\d\d):(\d\d):(\d\d)\.(\d\d\d)')


def log_scan(path, offset):
    """Return (new_offset, resets, i2c_error_count, sleep_hits, power_transitions, text)."""
    with open(path, 'rb') as f:
        f.seek(offset)
        data = f.read()
    text = data.decode('utf-8', 'replace')
    resets, i2c, sleeps, power = [], 0, 0, []
    normal = 0
    hang = good = 0
    for line in text.splitlines():
        line = line.lstrip('\ufeff')
        if RESET_RX.search(line):
            m = TS_RX.match(line)
            resets.append(m.group(0)[6:] if m else line[:18])
        if I2C_RX.search(line):
            i2c += 1
        if SLEEP_RX.search(line):
            sleeps += 1
        if NORMAL_RX.search(line):
            normal += 1
        if HANG_RX.search(line):
            hang += 1
        if GOOD_RX.search(line):
            good += 1
        m = POWER_RX.search(line)
        if m:
            t = TS_RX.match(line)
            power.append('%s=%s@%s' % (m.group(1), m.group(2), t.group(0)[6:14] if t else '?'))
    return offset + len(data), resets, i2c, sleeps, power, (normal, hang, good), text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--plan', required=True, help='scenarios separated by ;  kind:key=val,key=val')
    ap.add_argument('--log', required=True, help='streamed logcat file')
    ap.add_argument('--gap', type=float, default=6.0, help='quiet seconds between scenarios')
    ap.add_argument('--slices', default=r'D:\Workspace\MatePadPaper\.tmp\slices', help='directory for per-scenario log slices')
    ap.add_argument('--no-launch', action='store_true')
    ap.add_argument('--launch-extra', default='', help='extra args for `am start`, e.g. "--ei pw 50"')
    ap.add_argument('--stop-on-reset', action='store_true')
    args = ap.parse_args()

    os.makedirs(args.slices, exist_ok=True)
    for old in os.listdir(args.slices):
        if old.endswith('.log'):
            os.remove(os.path.join(args.slices, old))
    adb('forward', 'tcp:%d' % PORT, 'tcp:%d' % PORT)
    if not args.no_launch:
        adb('shell', 'am', 'force-stop', 'com.padreen.app')
        time.sleep(0.5)
        adb(*(['shell', 'am', 'start', '-n', 'com.padreen.app/.MainActivity'] + args.launch_extra.split()))
        time.sleep(2.0)
    sock = None
    for _ in range(30):
        try:
            sock = socket.create_connection(('127.0.0.1', PORT), timeout=3)
            hdr = recvn(sock, 8)
            t, _f, _r, ln = struct.unpack('<BBHI', hdr)
            recvn(sock, ln)
            if t == T_HELLO:
                break
            sock.close()
            sock = None
        except Exception:
            sock = None
            time.sleep(0.4)
    if sock is None:
        print('cannot connect')
        return 1
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.settimeout(30)
    acks = Acks()
    stop = threading.Event()
    threading.Thread(target=reader, args=(sock, acks, stop), daemon=True).start()
    runner = Runner(sock, acks)

    offset = os.path.getsize(args.log)
    print('%-38s %5s %5s %6s %6s %6s %6s  %s' % ('scenario', 'sent', 'acked', 'lat50', 'lat95', 'latMx', 'postMx', 'verdict'))
    idx = 0
    for item in [s.strip() for s in args.plan.split(';') if s.strip()]:
        kind, _, rest = item.partition(':')
        kv = parse_kv(rest)
        idx += 1
        with acks.lock:
            acks.lat.clear()
            acks.sent.clear()
        runner.notes = []
        sent0 = runner.sent
        err = ''
        try:
            runner.mark('BEGIN #%d %s' % (idx, item))
            runner.run(kind, kv)
            runner.mark('END #%d' % idx)
        except Exception as e:     # send timeout etc.
            err = ' ERR %s' % e
        time.sleep(1.5)
        offset, resets, i2c, sleeps, power, (normal, hang, good), text = log_scan(args.log, offset)
        with acks.lock:
            lats = sorted(x[1] for x in acks.lat)
            posts = [x[2] for x in acks.lat]

        def pc(p):
            return lats[min(len(lats) - 1, int(len(lats) * p / 100))] * 1000 if lats else float('nan')
        with open(os.path.join(args.slices, 'sc%02d-%s.log' % (idx, kind)), 'w', encoding='utf-8') as f:
            f.write(item + '\n' + text)
        verdict = ('RESET x%d @%s' % (len(resets), resets[0]) if resets else 'ok')
        if i2c:
            verdict += ' i2c=%d' % i2c
        if sleeps:
            verdict += ' sleepErr=%d' % sleeps
        if normal:
            verdict += ' normalPath=%d' % normal
        if hang:
            verdict += ' i2cHang=%d' % hang
        verdict += ' goodModeSets=%d' % good
        if power:
            verdict += ' power[%s]' % ' '.join(power[:8])
        print('%-38s %5d %5d %6.0f %6.0f %6.0f %6.0f  %s%s' % (item[:38], runner.sent - sent0, len(lats), pc(50), pc(95),
              lats[-1] * 1000 if lats else float('nan'), max(posts) if posts else float('nan'), verdict, err))
        for note in runner.notes:
            print('      >> ' + note)
        sys.stdout.flush()
        if resets:
            time.sleep(10)          # recovery: stay quiet until the controller answers again
            if args.stop_on_reset:
                break
        time.sleep(args.gap)
    stop.set()
    try:
        sock.close()
    except Exception:
        pass
    return 0


if __name__ == '__main__':
    sys.exit(main())