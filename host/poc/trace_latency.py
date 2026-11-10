#!/usr/bin/env python3
"""Per-flip latency timeline: pattern flip (PC) -> capture -> USB -> tablet render -> SurfaceFlinger stages (logcat).

Inputs written by:  latency_pattern.py --flip-log flips.csv,  mirror.py --trace-csv trace.csv,  filtered logcat (app run with
`--ez trace true` so every post is logged as "POSTED ... nano=").  All times are put on the PC perf_counter clock.
    python trace_latency.py --flips flips.csv --trace trace.csv --log lc-eink.txt
"""
import argparse
import re
import statistics

TS = re.compile(r'^\d\d-\d\d (\d\d):(\d\d):(\d\d)\.(\d\d\d)\s+\d+\s+\d+\s+\w\s+(\S+?)\s*:\s*(.*)')


def parse_log(path):
    rows = []
    for line in open(path, encoding='utf-8', errors='replace'):
        m = TS.match(line.lstrip('\ufeff'))
        if not m:
            continue
        h, mi, s, ms, tag, msg = m.groups()
        rows.append((int(h) * 3600 + int(mi) * 60 + int(s) + int(ms) / 1000.0, tag, msg))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--flips', required=True)
    ap.add_argument('--trace', required=True)
    ap.add_argument('--log', required=True)
    args = ap.parse_args()

    flips = [int(l.split(',')[0]) for l in open(args.flips) if l.strip()]
    trace = []
    for l in list(open(args.trace))[1:]:
        p = l.strip().split(',')
        trace.append({'seq': int(p[0]), 'cap': int(p[1]), 'send': int(p[2]), 'hdr': int(p[3]), 'body': int(p[4]), 'dec': int(p[5]),
                      'drawn': int(p[6]), 'posted': int(p[7]), 'bytes': int(p[8]), 'rect': p[9]})
    log = parse_log(args.log)

    # tablet nano -> logcat wall clock via the POSTED lines, then logcat -> PC clock via the matching trace rows
    posted_lines = [(t, int(re.search(r'nano=(\d+)', msg).group(1)), msg) for t, tag, msg in log if tag == 'Padreen' and msg.startswith('POSTED')]
    if not posted_lines:
        print('no POSTED lines in the log - start the app with --ez trace true')
        return 1
    # The ACK's tPosted and the "POSTED nano=" log line are the same System.nanoTime(); the trace stores it minus the
    # PC<->tablet clock offset o.  Pairs (log nano N_i, trace P_j) with N_i - P_j == o are the same post: find o as the
    # most common difference, then map logcat time <-> PC clock with those pairs.
    import collections
    posted_pc = [t['posted'] for t in trace]
    cnt = collections.Counter(n - p for _t, n, _m in posted_lines for p in posted_pc)
    o, hits = cnt.most_common(1)[0]
    by_nano = {n: t for t, n, _m in posted_lines}
    diffs = []
    for p in posted_pc:
        w = by_nano.get(p + o)
        if w is not None:
            diffs.append(w - p / 1e9)
    offset = statistics.median(diffs)          # logcat_seconds = pc_seconds + offset
    print('POSTED lines %d, trace rows %d, paired %d (clock offset spread %.1f ms)' % (len(posted_lines), len(trace), len(diffs), (max(diffs) - min(diffs)) * 1000))
    def to_log(pc_ns):
        return pc_ns / 1e9 + offset

    stage_patterns = [('handleRefresh', 'EinkSfpatchData_T1000', 'handleRefresh enter'),
                      ('dither start', 'DISPLAY_DITHER_V201_ALGO', 'Start HistSierraDither'),
                      ('dither done', 'DISPLAY_DITHER_V201_ALGO', 'HW Dither Done'),
                      ('postRefresh', 'EinkSfpatchData_T1000', 'postRefreshMode enter'),
                      ('setdisplaymode', 't1000_lib', 'mutex setdisplaymode')]
    rows = []
    for tf in flips:
        nxt = [t for t in trace if t['cap'] >= tf - 150000000]     # the window is already on screen while Tk finishes update_idletasks()
        if not nxt:
            continue
        u = nxt[0]
        base = tf / 1e9
        r = {'capture': (u['cap'] - tf) / 1e6, 'send': (u['send'] - tf) / 1e6, 'tab_hdr': (u['hdr'] - tf) / 1e6, 'decoded': (u['dec'] - tf) / 1e6,
             'drawn': (u['drawn'] - tf) / 1e6, 'posted': (u['posted'] - tf) / 1e6, 'rect': u['rect']}
        tp_log = to_log(u['posted'])
        for name, tag, pat in stage_patterns:
            hit = next((t for t, tg, msg in log if t >= tp_log - 0.02 and tg == tag and pat in msg), None)
            r[name] = (hit - to_log(tf)) * 1000.0 if hit is not None else None
        rows.append(r)
    print('flips: %d, matched updates: %d' % (len(flips), len(rows)))
    cols = ['capture', 'send', 'tab_hdr', 'decoded', 'drawn', 'posted', 'handleRefresh', 'dither start', 'dither done', 'postRefresh', 'setdisplaymode']
    print('%-4s ' % '#' + ' '.join('%13s' % c for c in cols) + '   rect')
    for i, r in enumerate(rows):
        print('%-4d ' % i + ' '.join('%13s' % ('%.0f' % r[c] if r[c] is not None else '-') for c in cols) + '   ' + r['rect'])
    print('\nmedian (ms after the flip):')
    for c in cols:
        v = [r[c] for r in rows if r[c] is not None]
        if v:
            print('  %-14s %6.0f   (min %.0f, max %.0f, n=%d)' % (c, statistics.median(v), min(v), max(v), len(v)))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
