"""Robustness checks of the PC LAN server (PD2) without a tablet: discovery, unknown tablet, pairing limits.
   D:\\Python3.8\\python.exe host\\poc\\pd2_probe.py [pc_ip]"""
import socket, struct, sys, os, time, subprocess

PC = sys.argv[1] if len(sys.argv) > 1 else '127.0.0.1'
PHCTL = r'D:\Workspace\MatePadPaper\host\poc\phctl.py'
PADID = os.urandom(16)


def frame(t, payload=b''):
    return struct.pack('<BBHI', t, 0, 0, len(payload)) + payload


def recv_frame(s):
    h = b''
    while len(h) < 8:
        c = s.recv(8 - len(h))
        if not c: return None, b''
        h += c
    t, _, _, n = struct.unpack('<BBHI', h)
    p = b''
    while len(p) < n:
        c = s.recv(n - len(p))
        if not c: break
        p += c
    return t, p


def hello(mode, name=b'probe'):
    return frame(0x80, bytes([2, mode]) + PADID + bytes([len(name)]) + name)


def tcp(port=38702):
    s = socket.create_connection((PC, port), timeout=8)
    s.settimeout(8)
    return s


# 1) discovery
u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
u.settimeout(2)
u.sendto(b'PDQ2' + PADID, (PC, 38703))
d, a = u.recvfrom(512)
assert d[:4] == b'PDR2', d[:8]
flags, port = d[5], struct.unpack('<H', d[6:8])[0]
name = d[25:25 + d[24]].decode()
print('1 discovery OK: name=%s tcp=%d flags=%d id=%s' % (name, port, flags, d[8:24].hex()))

# 2) unknown tablet, mode connect -> FAIL(1)
s = tcp(); s.sendall(hello(0)); t, p = recv_frame(s)
print('2 unknown tablet ->', hex(t) if t is not None else None, p[:1].hex()); assert t == 0x84 and p[0] == 1
s.close()

# 3) pairing window closed -> FAIL(4)
s = tcp(); s.sendall(hello(1)); t, p = recv_frame(s)
print('3 pairing closed ->', hex(t), p[:1].hex()); assert t == 0x84 and p[0] == 4
s.close()

# 4) open the window, 5 wrong codes -> FAIL(5) each, then the window is closed (FAIL(4))
subprocess.run([sys.executable, PHCTL, 'pair'], capture_output=True)
time.sleep(1.0)
for i in range(5):
    s = tcp(); h = hello(1); s.sendall(h)
    t, ch = recv_frame(s)
    assert t == 0x85, hex(t)
    s.sendall(frame(0x86, b'\x04' + os.urandom(64) + os.urandom(32)))      # wrong proof (dummy public key: never reached)
    t, p = recv_frame(s)
    print('4.%d wrong code -> %s %s' % (i + 1, hex(t) if t else None, p[:1].hex() if p else ''))
    assert t == 0x84 and p[0] == 5
    s.close()
s = tcp(); s.sendall(hello(1)); t, p = recv_frame(s)
print('4.6 after 5 failures ->', hex(t), p[:1].hex()); assert t == 0x84 and p[0] == 4
s.close()

# 5) silent client: the server drops it after ~6 s
s = tcp(); t0 = time.time()
try:
    r = s.recv(1)
except Exception as e:
    r = b'timeout'
print('5 silent client: recv=%r after %.1f s' % (r, time.time() - t0))
print('ALL OK')
