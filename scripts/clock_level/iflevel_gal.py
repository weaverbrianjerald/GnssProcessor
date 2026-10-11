#!/usr/bin/env python3
"""Galileo analogue of iflevel_code2.py (card GFC): the ionosphere-free CODE side of the wire against CODE,
per satellite, for a pilot-family user (C1C, C5Q) and a combined-family user (C1X, C5X):
X_fam(s,t) = -c0 + a1 (B(sig1) - OSB(sig1)) - a2 (B(sig2) - OSB(sig2)), with c0 the wire clock residual
against the CODE a priori; the per-epoch median over satellites is removed (a receiver clock takes the common
mode).  X is the level error a consumer on that family's pair sees.  Reported per hour from the first wire
epoch: rms across satellites of the per-satellite hourly mean / median absolute value; then the
per-satellite mean over hours 2 to 6.
    python scripts/clock_level/iflevel_gal.py <run> [<run> ...]"""
import os, statistics as st, sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import wire_vs_code_census as W

F1 = 1575.42e6; F5 = 1176.45e6
A1 = F1 * F1 / (F1 * F1 - F5 * F5); A2 = F5 * F5 / (F1 * F1 - F5 * F5)
FAMS = {'pilot': ('C1C', 'C5Q'), 'combined': ('C1X', 'C5X')}


def main():
    for run in sys.argv[1:]:
        clk_paths, bia, doy_key, _ = W.resolve_code_products(os.path.join(run, 'arm.json'))
        osb, _ = W.parse_bia(bia, doy_key, ('E',))
        eps, _ = W.parse_products(os.path.join(run, 'products.txt'))
        t0 = eps[0]['tow']
        acc = {f: defaultdict(lambda: defaultdict(list)) for f in FAMS}
        for e in eps:
            h = int((e['tow'] - t0) // 3600)
            for fam, (s1, s2) in FAMS.items():
                x = {}
                for s, cv in e['C'].items():
                    if s[0] != 'E':
                        continue
                    c0 = cv[0] if isinstance(cv, tuple) else cv
                    b1 = e['B'].get((s, s1)); b2 = e['B'].get((s, s2))
                    o1 = osb.get((s, s1)); o2 = osb.get((s, s2))
                    if None in (b1, b2, o1, o2):
                        continue
                    x[s] = -c0 + A1 * (b1[0] - o1) - A2 * (b2[0] - o2)
                if len(x) < 4:
                    continue
                m = st.median(x.values())
                for s, v in x.items():
                    acc[fam][h][s].append(v - m)
        for fam in FAMS:
            parts = []
            for h in sorted(acc[fam]):
                means = [st.mean(v) for v in acc[fam][h].values() if len(v) >= 20]
                if len(means) < 4:
                    continue
                rms = (sum(m * m for m in means) / len(means)) ** 0.5
                parts.append(f'{rms:.3f}/{st.median(abs(m) for m in means):.3f}')
            print(f'{os.path.basename(run)} {fam} {FAMS[fam]}: rms/med|mean| by hour ' + ' '.join(parts))
            per = defaultdict(list)
            for h in acc[fam]:
                if h >= 1:
                    for s, v in acc[fam][h].items():
                        per[s] += v
            print('   per satellite, hours 2-6 mean: '
                  + ' '.join(f'{s}:{st.mean(v):+.2f}' for s, v in sorted(per.items()) if len(v) >= 20))


if __name__ == '__main__':
    main()
