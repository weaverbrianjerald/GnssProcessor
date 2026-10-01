#!/usr/bin/env python3
"""Per-satellite datum-free level error path on the sim bed (the wire clock plus the injected
IF(C1C,C2W) OSB share, per-epoch median removed), at chosen epochs, for several runs side by
side.  Also the rms across satellites per epoch window.
    python scripts/clock_level/levelpath.py <run> [<run> ...]"""
import os, statistics as st, sys
import importlib.util
spec = importlib.util.spec_from_file_location('clklevel', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'clklevel.py'))
sys_argv = sys.argv
sys.argv = ['clklevel.py']          # the module runs its CLI on import; give it nothing to do
cl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cl)
sys.argv = sys_argv

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/')
EP = [15, 30, 45, 59, 75, 90, 105, 119, 150, 180, 210, 239]


def paths(d):
    ds = cl.datum_share()
    c, tows = cl.clk(d)
    out = {}
    for i in range(len(tows)):
        t = tows[i]
        v = {s: c[s][t] + ds[s] for s in c if t in c[s] and s in ds}
        if len(v) < 3:
            continue
        m = st.median(v.values())
        out[i] = {s: x - m for s, x in v.items()}
    return out


def main():
    runs = [a if os.path.isdir(a) else ROOT + '/runs/' + a for a in sys.argv[1:]]
    P = {r: paths(r) for r in runs}
    sats = sorted({s for r in runs for i in P[r] for s in P[r][i]})
    for r in runs:
        print('==', os.path.basename(r))
        print('sat  ' + ' '.join(f'{i + 1:>6d}' for i in EP))
        for s in sats:
            print(f'{s}  ' + ' '.join(f"{P[r][i][s]:+6.2f}" if i in P[r] and s in P[r][i] else '     -' for i in EP))
        rms = []
        for i in EP:
            v = list(P[r].get(i, {}).values())
            rms.append((sum(x * x for x in v) / len(v)) ** 0.5 if v else float('nan'))
        print('rms  ' + ' '.join(f'{x:6.2f}' for x in rms))


if __name__ == '__main__':
    main()
