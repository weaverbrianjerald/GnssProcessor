#!/usr/bin/env python3
"""Decompose a run's emitted satellite-clock series into drift / residual / step noise.

    python scripts/clock_decompose.py <run_dir> [tail_seconds=3600]

Reads ``<run>/logs/network_solution.csv`` CLK rows over the final window and
reports, per satellite (cross-sat de-medianed first — the datum-invariant read):

* ``|linear drift|`` (m/hour)   — the PHYSICAL clock class (af1, us/day scale;
  on this producer it lives mostly in the COD a-priori the state is linearized
  against, so it is NOT what q_clock constrains);
* ``post-linear residual`` (cm) — everything a linear fit does not explain:
  clock noise + estimation churn + emit-anchor tension;
* ``2nd-diff sigma_eq`` (cm/step) — drift-immune white-equivalent step noise;
  under WFM, q1 = sigma_eq^2 / dt — compare against the Hadamard card's
  measured q1 (~3.5e-3 GPS) and against filter.process_noise.sat_clock_q.

METRIC TRAP this tool exists to prevent (2026-07-31): a plain temporal std of
the emitted clock is DOMINATED by the linear drift (~2-7 m/h -> ~0.3 m std per
30 min window alone).  Any historical "clock wander" number computed without a
per-sat de-trend (e.g. the lever-C 0.368 m reading) conflates physical drift
with estimation churn.  Grade clock changes on the DE-TRENDED columns, and on
total_IF per the promotion-ledger convention — never raw rms_clock_m.
"""
import csv
import math
import statistics as st
import sys
from collections import defaultdict


def main():
    run = sys.argv[1]
    tail_s = float(sys.argv[2]) if len(sys.argv) > 2 else 3600.0
    rows = list(csv.DictReader(open(f'{run}/logs/network_solution.csv')))
    cols = rows[0].keys()
    typek = 'type' if 'type' in cols else ('kind' if 'kind' in cols else None)
    clk = [r for r in rows if (typek is None or r[typek].strip().upper().startswith('CLK'))]
    if not clk:
        sys.exit(f'no CLK rows (columns: {list(cols)})')
    vk = 'value_m' if 'value_m' in cols else 'value'
    tmax = max(float(r['tow']) for r in clk)
    clk = [r for r in clk if float(r['tow']) >= tmax - tail_s]

    by_epoch = defaultdict(dict)
    for r in clk:
        by_epoch[float(r['tow'])][r['sat']] = float(r[vk])
    series = defaultdict(list)
    for t in sorted(by_epoch):
        med = st.median(by_epoch[t].values())
        for s, v in by_epoch[t].items():
            series[s].append((t, v - med))

    slopes, resid, sd = [], [], []
    for s, pts in series.items():
        if len(pts) < 60:
            continue
        ts = [p[0] for p in pts]
        vs = [p[1] for p in pts]
        tm = sum(ts) / len(ts)
        vm = sum(vs) / len(vs)
        sxx = sum((t - tm) ** 2 for t in ts)
        b = sum((t - tm) * (v - vm) for t, v in pts) / sxx
        a = vm - b * tm
        res = [v - (a + b * t) for t, v in pts]
        slopes.append(abs(b) * 3600.0)
        resid.append(st.pstdev(res))
        d2 = [vs[k + 2] - 2 * vs[k + 1] + vs[k] for k in range(len(vs) - 2)]
        sd.append(math.sqrt(sum(x * x for x in d2) / len(d2) / 6.0))

    def med(x):
        x = sorted(x)
        return x[len(x) // 2]

    def p90(x):
        x = sorted(x)
        return x[int(len(x) * 0.9)]

    print(f'{run}: n={len(slopes)} sats, tail {tail_s:.0f} s')
    print(f'  |linear drift|      med {med(slopes):7.3f}  p90 {p90(slopes):7.3f}  m/hour')
    print(f'  post-linear resid   med {med(resid)*100:7.1f}  p90 {p90(resid)*100:7.1f}  cm')
    print(f'  2nd-diff sigma_eq   med {med(sd)*100:7.1f}  p90 {p90(sd)*100:7.1f}  cm/step'
          f'   (WFM q1 ~ {med(sd)**2/30.0:.2e} m^2/s at 30 s)')


if __name__ == '__main__':
    main()
