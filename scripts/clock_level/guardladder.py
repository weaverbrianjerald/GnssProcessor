#!/usr/bin/env python3
"""Card CTU section 14: read logs/network_anchor_guard.csv (phase_shadow / phase) and report,
for anchored rows from epoch 30 on, the |e_phi| distribution and the would-trip count per hour
of the phase statistic at k = 6 for several sqrt(S) floors (trip if |e_phi| > k*max(floor,
sqrtS_phi)), next to the clock statistic's would-trips (|w_clk| > 6).
    python scripts/clock_level/guardladder.py <run> [<run> ...]"""
import csv, os, statistics as st, sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/')
RUNS = ROOT + '/runs'
FLOORS = [0.005, 0.01, 0.02, 0.03, 0.05]


def main():
    for a in sys.argv[1:]:
        d = a if os.path.isdir(a) else os.path.join(RUNS, a if a.startswith('_') else '_rap_' + a)
        rows = list(csv.DictReader(open(os.path.join(d, 'logs', 'network_anchor_guard.csv'))))
        tows = sorted({float(r['tow']) for r in rows})
        t30 = tows[29] if len(tows) > 29 else tows[-1]
        hours = (tows[-1] - t30) / 3600.0 if tows else 1.0
        e, clk_trip, n = [], 0, 0
        trips = defaultdict(int)
        worst = defaultdict(int)
        for r in rows:
            if float(r['tow']) < t30 or r['anchored'] != '1':
                continue
            n += 1
            if abs(float(r['w_clk'])) > 6.0:
                clk_trip += 1
            if not r['e_phi']:
                continue
            ep = abs(float(r['e_phi'])); sp = float(r['sqrtS_phi'])
            e.append(ep)
            for f in FLOORS:
                if ep > 6.0 * max(f, sp):
                    trips[f] += 1
                    if f == 0.02:
                        worst[r['sat']] += 1
        e.sort()
        q = lambda p: e[min(len(e) - 1, int(p * len(e)))] if e else float('nan')
        print(f"== {os.path.basename(d)}: {n} anchored rows from epoch 30 ({hours:.2f} h), {len(e)} with e_phi")
        print(f"   |e_phi| p50 {1000*q(.5):.1f} p90 {1000*q(.9):.1f} p99 {1000*q(.99):.1f} max {1000*(e[-1] if e else 0):.1f} mm")
        print(f"   clock-statistic would-trips {clk_trip} ({clk_trip / max(hours, 1e-9):.1f}/h)")
        print('   phase would-trips per hour by floor: ' + ' '.join(f"{100*f:.1f}cm:{trips[f] / max(hours, 1e-9):.1f}" for f in FLOORS))
        if worst:
            print('   at 2 cm: ' + ' '.join(f"{s}:{c}" for s, c in sorted(worst.items(), key=lambda kv: -kv[1])[:8]))


if __name__ == '__main__':
    main()
