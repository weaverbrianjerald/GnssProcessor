#!/usr/bin/env python3
"""Truth-free level read: per-satellite median IF(C1C,C2W) post-fit code residual in windows
(default 10 min), the per-window median across satellites removed; prints the rms across
satellites per window for each run.  A level that converges drives this towards the code noise
of a median (~ a few cm); a frozen or drifting level keeps it at the level error.
    python scripts/clock_level/ifwin.py [--win 600] <run> [<run> ...]"""
import csv, os, statistics as st, sys
from collections import defaultdict

F1 = 1575.42e6; F2 = 1227.60e6; A = F1 * F1 / (F1 * F1 - F2 * F2); B = F2 * F2 / (F1 * F1 - F2 * F2)
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/')
RUNS = ROOT + '/runs'


def resolve(a):
    for c in (a, os.path.join(RUNS, a), os.path.join(RUNS, '_rap_' + a)):
        if os.path.isdir(c):
            return c
    raise SystemExit('no run ' + a)


def windows(d, win):
    pend = {}; per = defaultdict(lambda: defaultdict(list)); t0 = None
    with open(os.path.join(d, 'logs', 'network_pf_rows.csv')) as f:
        rd = csv.reader(f); h = next(rd); ix = {k: i for i, k in enumerate(h)}
        for r in rd:
            if r[ix['kind']] != 'code' or r[ix['sat']][0] != 'G':
                continue
            t = float(r[ix['tow']])
            if t0 is None:
                t0 = t
            try:
                v = float(r[ix['resid_m']]); w = float(r[ix['w']])
            except ValueError:
                continue
            if w < 1e-2:
                continue
            k = (r[ix['station']], r[ix['sat']], t); sg = r[ix['signal']]
            a, b = pend.get(k, (None, None))
            if sg == 'C1C':
                a = v
            elif sg == 'C2W':
                b = v
            else:
                continue
            if a is not None and b is not None:
                per[int((t - t0) // win)][r[ix['sat']]].append(A * a - B * b); pend.pop(k, None)
            else:
                pend[k] = (a, b)
    out = []
    for wi in sorted(per):
        meds = {s: st.median(v) for s, v in per[wi].items() if len(v) >= 30}
        if len(meds) < 4:
            out.append(float('nan')); continue
        m = st.median(meds.values())
        out.append((sum((x - m) ** 2 for x in meds.values()) / len(meds)) ** 0.5)
    return out


def main():
    args = sys.argv[1:]; win = 600.0
    if args and args[0] == '--win':
        win = float(args[1]); args = args[2:]
    for a in args:
        d = resolve(a)
        print(f'{os.path.basename(d):26s} ' + ' '.join(f'{x:.3f}' for x in windows(d, win)))


if __name__ == '__main__':
    main()
