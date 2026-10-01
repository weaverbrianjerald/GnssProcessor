#!/usr/bin/env python3
"""Sim bed: the level error of each satellite in the pivot frame (its wire clock plus the
injected IF(C1C,C2W) OSB share, minus the pivot's), against the monitor's P_L and the
code-only bound 1/I_L (logs/network_level_bound.csv).  Per epoch: rms error, the median
normalized error e^2/P_L and the median e^2/(1/I_L).
    python scripts/clock_level/levelnees.py <run> [<run> ...]"""
import csv, os, statistics as st, sys
from collections import defaultdict
import importlib.util
spec = importlib.util.spec_from_file_location('clklevel', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'clklevel.py'))
argv = sys.argv; sys.argv = ['clklevel.py']
cl = importlib.util.module_from_spec(spec); spec.loader.exec_module(cl)
sys.argv = argv
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/')
EP = [15, 30, 59, 90, 119, 150, 180, 210, 239]


def main():
    ds = cl.datum_share()
    for a in sys.argv[1:]:
        d = a if os.path.isdir(a) else ROOT + '/runs/' + a
        c, tows = cl.clk(d)
        mon = defaultdict(dict)
        for r in csv.DictReader(open(os.path.join(d, 'logs', 'network_level_bound.csv'))):
            mon[round(float(r['tow']), 3)][r['sat']] = (float(r['p_level_m2']), float(r['inv_info_m2']))
        print('==', os.path.basename(d))
        print('  ep   n   rms_e   med e2/P_L   med e2/(1/I_L)   med P_L    med 1/I_L   pivot')
        for i in EP:
            if i >= len(tows):
                break
            t = tows[i]
            m = mon.get(t, {})
            # the pivot: the satellite with a wire clock and no monitor row
            piv = [s for s in c if t in c[s] and s in ds and s not in m]
            if len(piv) != 1:
                continue
            p = piv[0]
            e0 = c[p][t] + ds[p]
            es, n1, n2, pl, il = [], [], [], [], []
            for s, (P_L, inv) in m.items():
                if t not in c.get(s, {}) or s not in ds:
                    continue
                e = c[s][t] + ds[s] - e0
                es.append(e); n1.append(e * e / P_L); n2.append(e * e / inv); pl.append(P_L); il.append(inv)
            if not es:
                continue
            rms = (sum(x * x for x in es) / len(es)) ** 0.5
            print(f'  {i + 1:3d} {len(es):3d}  {rms:6.3f}   {st.median(n1):10.2f}   {st.median(n2):12.2f}   '
                  f'{st.median(pl):.2e}   {st.median(il):.2e}   {p}')


if __name__ == '__main__':
    main()
