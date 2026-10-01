#!/usr/bin/env python3
"""Card SMH G3: the predicted hidden fraction rho_D = P_D / (P_D + W) per clone-epoch, with P_D the
stated variance of the phase-visible clock at the clone's birth (card PVN's functional: the
ionosphere-free phase clock on the constellation contrast, network_phase_visible.csv sqrt_sp_m)
and W the fold inflation since birth (the clone trace), weighted like the measurement (the square
of the injected de-commoned increment).  Prints the weighted rho_D and, beside the measured
hidden fraction of g3grade.py, the implied f = measured / rho_D.
    python scripts/clock_level/g3pred.py <walk_csv> <run> [<run> ...]"""
import csv, math, os, statistics as st, sys
from collections import defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import g3grade as g


def main():
    w = g.load_walk(sys.argv[1])
    for a in sys.argv[2:]:
        d = g.resolve(a)
        C, P = g.load_wire(d)
        dw, dt = {}, {}
        for t in sorted(set(C) & set(P)):
            sats = [s for s in C[t] if s in P[t]]
            dw[t] = g.decommon({s: C[t][s] - P[t][s][0] for s in sats})
            dt[t] = g.decommon({s: -g.walk_at(w, s, t) for s in sats})
        sp = {}
        for r in csv.DictReader(open(os.path.join(d, 'logs', 'network_phase_visible.csv'))):
            try:
                sp[(round(float(r['tow']), 1), r['sat'])] = float(r['sqrt_sp_m']) ** 2
            except ValueError:
                pass
        num = den = 0.0
        rhos, pds = [], []
        pairs = []
        for r in csv.DictReader(open(os.path.join(d, 'logs', 'network_pb_hold_clone.csv'))):
            if r['sig'] != 'L1C':
                continue
            t = round(float(r['tow']), 1)
            tb = round(t - float(r['age_s']), 1)
            s = r['sat']
            if s not in dw.get(t, {}) or s not in dw.get(tb, {}) or s not in dt.get(tb, {}):
                continue
            pd = sp.get((tb, s))
            if pd is None:
                continue
            x = dt[t][s] - dt[tb][s]
            y = dw[t][s] - dw[tb][s]
            W = float(r['w_m2'])
            rho = pd / (pd + W) if pd + W > 0 else float('nan')
            num += rho * x * x
            den += x * x
            rhos.append(rho); pds.append(pd)
            pairs.append((x, y))
        sxy = sum(x * y for x, y in pairs); sxx = sum(x * x for x, y in pairs)
        hid = 1 - sxy / sxx if sxx > 0 else float('nan')
        rbar = num / den if den > 0 else float('nan')
        print(f'{os.path.basename(d)}: {len(pairs)} clone-epochs with P_D at birth; P_D p50 '
              f'{st.median(pds):.2e} m2 (sd {math.sqrt(st.median(pds)):.4f} m); rho_D weighted '
              f'{rbar:.3f}; hidden measured {hid:.3f}; implied f {hid / rbar if rbar > 0 else float("nan"):.3f}')


if __name__ == '__main__':
    main()
