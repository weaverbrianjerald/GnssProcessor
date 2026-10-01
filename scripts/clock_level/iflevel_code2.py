#!/usr/bin/env python3
"""The ionosphere-free CODE side of the wire against CODE, per satellite, for a C1C and a
C1W user: X_fam(s,t) = -c0 + a1 (B(fam) - OSB(fam)) - a2 (B(C2W) - OSB(C2W)), c0 the wire clock residual against the CODE a-priori,
the per-epoch median over satellites removed (a receiver clock takes the common mode).
X is the level error a consumer on that pair sees.  Reported per hour: the rms across
satellites of the per-satellite hourly mean, and the median absolute value.
    python scripts/clock_level/iflevel_code2.py <seed dir> <run> [<run> ...]"""
import os, statistics as st, sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import wire_vs_code_census as W

F1 = 1575.42e6; F2 = 1227.60e6
A1 = F1 * F1 / (F1 * F1 - F2 * F2); A2 = F2 * F2 / (F1 * F1 - F2 * F2)
C = 299792458.0


def main():
    seed = sys.argv[1]
    clk_paths, bia, doy_key, _ = W.resolve_code_products(seed)
    clk, _ = W.parse_clk(clk_paths)
    osb, _ = W.parse_bia(bia, doy_key)
    for run in sys.argv[2:]:
        eps, _ = W.parse_products(os.path.join(run, 'products.txt'))
        t0 = eps[0]['tow']
        acc = {f: defaultdict(lambda: defaultdict(list)) for f in ('C1C', 'C1W')}
        for e in eps:
            t = e['tow']; h = int((t - t0) // 3600)
            week = e.get('week')
            for fam in ('C1C', 'C1W'):
                x = {}
                for s, cv in e['C'].items():
                    c0 = cv[0] if isinstance(cv, tuple) else cv
                    b1 = e['B'].get((s, fam)); b2 = e['B'].get((s, 'C2W'))
                    o1 = osb.get((s, fam)); o2 = osb.get((s, 'C2W'))
                    if None in (b1, b2, o1, o2):
                        continue
                    tg = int(round(week * 604800 + t)) if week is not None else int(round(t))
                    cc, _ = W.clk_lookup(clk, s, tg)
                    if cc is None:
                        continue
                    # the wire clock is the residual against the CODE a-priori clock, so
                    # network minus CODE on the code side is -c0 + (B - OSB) per signal
                    x[s] = -c0 + A1 * (b1[0] - o1) - A2 * (b2[0] - o2)
                if len(x) < 4:
                    continue
                m = st.median(x.values())
                for s, v in x.items():
                    acc[fam][h][s].append(v - m)
        out = [os.path.basename(run)]
        for fam in ('C1C', 'C1W'):
            parts = []
            for h in sorted(acc[fam]):
                means = [st.mean(v) for v in acc[fam][h].values() if len(v) >= 20]
                if len(means) < 4:
                    continue
                rms = (sum(m * m for m in means) / len(means)) ** 0.5
                parts.append(f'{rms:.3f}/{st.median(abs(m) for m in means):.3f}')
            out.append(f'{fam}: ' + ' '.join(parts))
        print('  '.join(out))


if __name__ == '__main__':
    main()
