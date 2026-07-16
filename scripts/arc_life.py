import csv, sys, statistics as st
from collections import defaultdict
# Arc-life estimator: per traced DD pair, split series into hold-segments at
# |d(dd_cyc)| >= 0.5 cyc or missing epoch; report segment-length stats.
path, t0, dt, max_ep = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), int(sys.argv[4])
series = defaultdict(dict)
for r in csv.DictReader(open(path)):
    e = round((float(r['tow']) - t0) / dt)
    if 0 <= e < max_ep:
        key = (r['sta_a'], r['sta_b'], r['sat_i'], r['ref_sat'], r['signal'])
        series[key][e] = float(r['dd_cyc'])
segs, holds, breaks = [], 0, 0
for key, by_e in series.items():
    es = sorted(by_e)
    cur = 1
    for a, b in zip(es, es[1:]):
        if b - a == 1 and abs(by_e[b] - by_e[a]) < 0.5:
            cur += 1; holds += 1
        else:
            if b - a == 1: breaks += 1
            segs.append(cur); cur = 1
    segs.append(cur)
n = len(segs)
segs.sort()
adj = holds + breaks
print(f"pairs={len(series)} segments={n} adjacent_transitions={adj} hold_frac={holds/max(adj,1):.3f}")
print(f"segment length (ep): median={st.median(segs):.1f} mean={sum(segs)/n:.2f} p90={segs[int(n*0.9)]} max={segs[-1]}  [x{dt/60:.0f} min/ep; window-censored at {max_ep} ep]")
