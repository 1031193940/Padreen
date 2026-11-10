#!/usr/bin/env python3
"""Group a threadtime logcat dump by PaperProbe command and print every line with its
offset (ms) from the moment the probe returned from its post call.

usage: analyze_sf.py <logfile> [--all]
"""
import re
import sys
from datetime import datetime

LINE = re.compile(r'^(\d\d-\d\d)\s+(\d\d:\d\d:\d\d\.\d{3})\s+(\d+)\s+(\d+)\s+([VDIWEF])\s+(\S+?)\s*:\s(.*)$')
KEEP = re.compile(
    r'PaperProbe|handleRefresh|epdcMode|waveform|dither|Dither|postRefreshMode|add_updatelist|'
    r'copy buffer|set_mipi_mode|update_marker|direct_update|wait mPending|display ready|spi|SPI|'
    r'FBHand|notifyMotion|T1000|mode|Mode', re.I)


def ts(s):
    return datetime.strptime(s, '%H:%M:%S.%f')


def summary(groups):
    """One line per post: mode, post->handleRefresh, dither cost, post->postRefreshMode, mode switch."""
    print('%-34s %6s %8s %8s %9s  %s' % ('cmd', 'mode', 'toHandle', 'dither', 'toPostRef', 'notes'))
    for g in groups:
        if g['post'] is None:
            continue
        base = g['post']
        handle = gen = post_ref = dith_done = None
        mode = None
        notes = []
        for t, tag, msg in g['lines']:
            d = (t - base).total_seconds() * 1000.0
            if d < -400:
                continue
            if 'handleRefresh enter' in msg and handle is None:
                handle = d
            elif msg.startswith('current epdcMode is') and mode is None:
                mode = int(re.search(r'is (\d+)', msg).group(1))
            elif 'generateRefreshModeAndDither epdcMode' in msg and gen is None:
                gen = d
            elif tag.startswith('DISPLAY_DITHER') and ('Done' in msg or 'No Dither' in msg) and dith_done is None:
                dith_done = d
                notes.append(re.sub(r'.*\] ', '', msg)[:28])
            elif 'postRefreshMode enter' in msg and post_ref is None:
                post_ref = d
            elif 'mainmode' in msg:
                notes.append(re.sub(r'.*success ', 'sw ', msg)[:34])
            elif 'wakeup first' in msg:
                notes.append('T1000 WAKE')
            elif 'postRefreshMode T1000' in msg:
                notes.append(msg.split('postRefreshMode ')[1])
        dith = (dith_done - gen) if (dith_done is not None and gen is not None) else None
        f = lambda v: '   n/a' if v is None else '%6.0f' % v
        print('%-34s %6s %8s %8s %9s  %s' % (g['cmd'][:34], mode, f(handle), f(dith), f(post_ref), '; '.join(notes)))


def main():
    path = sys.argv[1]
    show_all = '--all' in sys.argv
    groups = []
    cur = None
    with open(path, encoding='utf-8', errors='replace') as f:
        for raw in f:
            m = LINE.match(raw.rstrip('\n'))
            if not m:
                continue
            _, t, pid, tid, lvl, tag, msg = m.groups()
            if tag == 'PaperProbe' and msg.startswith('>> '):
                if re.match(r'>> (rect|fill|blink)\b', msg):
                    cur = {'cmd': msg[3:], 'lines': [], 'post': None}
                    groups.append(cur)
                continue
            if cur is None:
                continue
            if tag == 'PaperProbe' and (msg.startswith('plain rect') or msg.startswith('hw rect')) and cur['post'] is None:
                cur['post'] = ts(t)
            cur['lines'].append((ts(t), tag, msg))
    if '--summary' in sys.argv:
        summary([g for g in groups if g['post'] is not None])
        return
    for g in groups:
        if not any(('rect' in l[2] and l[1] == 'PaperProbe') for l in g['lines']):
            continue
        print('=' * 100)
        print('CMD:', g['cmd'])
        base = g['post'] or g['lines'][0][0]
        for t, tag, msg in g['lines']:
            if not show_all and not KEEP.search(tag + ' ' + msg):
                continue
            if tag == 'PaperProbe' and (msg.startswith('<<') or msg.startswith('useSurface')):
                continue
            d = (t - base).total_seconds() * 1000.0
            print('%+8.0f ms  %-22s %s' % (d, tag[:22], msg[:150]))


if __name__ == '__main__':
    main()
