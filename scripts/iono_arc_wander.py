#!/usr/bin/env python
"""Independent check of the DD-ambiguity wander against the ESTIMATED SLANT IONOSPHERE (SPEC_amb_iono §6.15.3a).

If the per-band DD wander were iono-borne it must appear in the estimated slant iono of the same chords over the
same arcs.  network_ionosphere.csv carries iono_total_m = x_lin + x_r (the full state, metres of L1 delay), so the
frame caveat on iono_m (x_r alone) does not apply here.  For every chord (sat_i, ref_sat, sta_a, sta_b) of the DD
trace's L1 rows, form  DD_I(t) = [I(a,i) - I(a,ref)] - [I(b,i) - I(b,ref)]  and take, per contiguous arc (same
rules as dd_arc_stability.py: gap <= --gap-ep, first --trim epochs dropped, >= --min-arc epochs, settled >= h6),
the MAD and the tail-median drift, in metres AND in L1 cycles (/0.190294) so it reads beside e1's MAD.
Also printed: the single-LOS iono_total_m arc MAD (per station-satellite) for scale.

Usage: python iono_arc_wander.py <run_dir> [--settle-h 6] [--min-arc 20] [--gap-ep 2] [--trim 10]
"""
import argparse, csv, io, os, statistics as st, sys
from collections import defaultdict

LAM1 = 0.190294

def q(v, f): return v[int(f * (len(v) - 1))] if v else float('nan')
def mad(v):
    m = st.median(v); return 1.4826 * st.median([abs(x - m) for x in v])
def tail_drift(v):
    k = min(10, len(v) // 2); return abs(st.median(v[-k:]) - st.median(v[:k]))
def arcs_of(series, gap_s, min_arc, trim):
    series.sort(); out, cur = [], [series[0]]
    for prev, nxt in zip(series, series[1:]):
        if nxt[0] - prev[0] > gap_s + 0.5: out.append(cur); cur = [nxt]
        else: cur.append(nxt)
    out.append(cur)
    return [a[trim:] for a in out if len(a) - trim >= min_arc]
def line(name, mads, drifts, unit):
    mads = sorted(mads); drifts = sorted(drifts)
    if not mads: return f'  {name}: no arcs'
    return f'  {name:28s} MAD p50 {q(mads,.5):8.4f} p90 {q(mads,.9):8.4f} | drift p50 {q(drifts,.5):8.4f} p90 {q(drifts,.9):8.4f} {unit} (n {len(mads)})'

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run_dir'); ap.add_argument('--cons', default='G'); ap.add_argument('--hi', default='L1C,L1W'); ap.add_argument('--lo', default='L2W')
    ap.add_argument('--settle-h', type=float, default=6.0); ap.add_argument('--min-arc', type=int, default=20)
    ap.add_argument('--gap-ep', type=int, default=2); ap.add_argument('--trim', type=int, default=10)
    a = ap.parse_args()
    his = [s.strip() for s in a.hi.split(',')]
    iono = {}; t0 = None
    with io.open(os.path.join(a.run_dir, 'logs', 'network_ionosphere.csv'), encoding='utf-8', errors='replace') as f:
        for row in csv.DictReader(f):
            if not row['sat'].startswith(a.cons): continue
            try: tow = float(row['tow']); tot = float(row['iono_total_m'])
            except ValueError: continue
            if t0 is None: t0 = tow
            if tow - t0 < a.settle_h * 3600.0: continue
            iono[(tow, row['station'], row['sat'])] = tot
    # single-LOS arcs
    los = defaultdict(list)
    for (tow, sta, sat), tot in iono.items(): los[(sta, sat)].append((tow, tot))
    lm, ld = [], []
    for key, ser in los.items():
        for arc in arcs_of(ser, 30.0 * a.gap_ep, a.min_arc, a.trim):
            v = [x[1] for x in arc]; lm.append(mad(v)); ld.append(tail_drift(v))
    # chords from the DD trace
    chords = defaultdict(list); n_rows = n_join = 0
    with io.open(os.path.join(a.run_dir, 'logs', 'network_dd_fractional.csv'), encoding='utf-8', errors='replace') as f:
        for row in csv.DictReader(f):
            if row['cons'] != a.cons or row['signal'] not in his: continue
            try: tow = float(row['tow'])
            except ValueError: continue
            if tow - t0 < a.settle_h * 3600.0: continue
            n_rows += 1
            k = (row['sat_i'], row['ref_sat'], row['sta_a'], row['sta_b'])
            try:
                ddI = (iono[(tow, k[2], k[0])] - iono[(tow, k[2], k[1])]) - (iono[(tow, k[3], k[0])] - iono[(tow, k[3], k[1])])
            except KeyError: continue
            n_join += 1; chords[k].append((tow, ddI))
    cm, cd = [], []
    for key, ser in chords.items():
        for arc in arcs_of(ser, 30.0 * a.gap_ep, a.min_arc, a.trim):
            v = [x[1] for x in arc]; cm.append(mad(v)); cd.append(tail_drift(v))
    name = os.path.basename(a.run_dir.rstrip('/\\'))
    print(f'{name} {a.cons}: iono_total_m rows {len(iono)} (settled >= h{a.settle_h:g}); DD-trace L1 rows {n_rows}, joined to iono {n_join} on {len(chords)} chords')
    print(line('single-LOS iono_total_m', lm, ld, 'm'))
    print(line('DD iono over the chords', cm, cd, 'm'))
    print(line('DD iono in L1 cycles', [x / LAM1 for x in cm], [x / LAM1 for x in cd], 'cyc'))
    # joint test on the same chords: per arc, regress the DD-ambiguity deviation e1 (cycles) on the DD-iono
    # deviation (in L1 cycles).  If the ambiguity wander COMPENSATED an iono-state error the slope would be -1
    # (phase held, iono wrong); if the iono state TRACKED a real change the ambiguity would not move (slope 0).
    amb = defaultdict(dict)
    with io.open(os.path.join(a.run_dir, 'logs', 'network_dd_fractional.csv'), encoding='utf-8', errors='replace') as f:
        for row in csv.DictReader(f):
            if row['cons'] != a.cons or row['signal'] not in his: continue
            try: tow = float(row['tow']); dd = float(row['dd_cyc'])
            except ValueError: continue
            amb[(row['sat_i'], row['ref_sat'], row['sta_a'], row['sta_b'])][tow] = dd
    slopes, corrs = [], []
    for key, ser in chords.items():
        e = amb.get(key, {})
        for arc in arcs_of(ser, 30.0 * a.gap_ep, a.min_arc, a.trim):
            pts = [(x[1] / LAM1, e[x[0]]) for x in arc if x[0] in e]
            if len(pts) < a.min_arc: continue
            mi = st.median(pt[0] for pt in pts); me = st.median(pt[1] for pt in pts)
            d1 = [pt[0] - mi for pt in pts]; d2 = [pt[1] - me for pt in pts]
            sxx = sum(x * x for x in d1); sxy = sum(x * y for x, y in zip(d1, d2)); syy = sum(y * y for y in d2)
            if sxx > 1e-9 and syy > 1e-9: slopes.append(sxy / sxx); corrs.append(sxy / (sxx * syy) ** 0.5)
    ss = sorted(slopes); cc = sorted(corrs)
    print(f'  joint per-arc regression e1(cyc) on DD-iono(cyc): slope p50 {q(ss,.5):+.3f} (p10 {q(ss,.1):+.3f}, p90 {q(ss,.9):+.3f}), corr p50 {q(cc,.5):+.3f} (n {len(ss)})   [-1 = ambiguity compensating an iono-state error; 0 = unrelated]')

    # A/B decomposition (pre-registered, SPEC s6.15.3a item 5): per chord-epoch with BOTH bands,
    #   e_i = A + B*(lambda_i/lambda_1)  =>  B = (e2 - e1)/(lambda2/lambda1 - 1),  A = e1 - B
    # then regress the arc deviations of A and of B separately on the DD-iono deviation (L1 cycles).
    LAM2 = 0.244210
    amb2 = defaultdict(dict)
    with io.open(os.path.join(a.run_dir, 'logs', 'network_dd_fractional.csv'), encoding='utf-8', errors='replace') as f:
        for row in csv.DictReader(f):
            if row['cons'] != a.cons or row['signal'] != a.lo: continue
            try: tow = float(row['tow']); dd = float(row['dd_cyc'])
            except ValueError: continue
            amb2[(row['sat_i'], row['ref_sat'], row['sta_a'], row['sta_b'])][tow] = dd
    ratio = LAM2 / LAM1 - 1.0
    res = {'A': ([], []), 'B': ([], [])}
    nA = 0
    for key, ser in chords.items():
        e1s = amb.get(key, {}); e2s = amb2.get(key, {})
        for arc in arcs_of(ser, 30.0 * a.gap_ep, a.min_arc, a.trim):
            pts = [(x[1] / LAM1, e1s[x[0]], e2s[x[0]]) for x in arc if x[0] in e1s and x[0] in e2s]
            if len(pts) < a.min_arc: continue
            nA += 1
            Bv = [(e2 - e1) / ratio for _, e1, e2 in pts]; Av = [e1 - b for (_, e1, _), b in zip(pts, Bv)]
            Iv = [pt[0] for pt in pts]
            mI = st.median(Iv); dI = [x - mI for x in Iv]; sxx = sum(x * x for x in dI)
            for name, v in (('A', Av), ('B', Bv)):
                mv = st.median(v); dv = [x - mv for x in v]
                sxy = sum(x * y for x, y in zip(dI, dv)); syy = sum(y * y for y in dv)
                if sxx > 1e-9 and syy > 1e-9:
                    res[name][0].append(sxy / sxx); res[name][1].append(sxy / (sxx * syy) ** 0.5)
    for name in ('A', 'B'):
        sl = sorted(res[name][0]); co = sorted(res[name][1])
        print(f'  A/B test: {name} (cycle-common) deviation on DD-iono(cyc): slope p50 {q(sl,.5):+.3f} (p10 {q(sl,.1):+.3f}, p90 {q(sl,.9):+.3f}), corr p50 {q(co,.5):+.3f} (n {len(sl)})' if name == 'A' else
              f'  A/B test: {name} (iono-shaped) deviation on DD-iono(cyc): slope p50 {q(sl,.5):+.3f} (p10 {q(sl,.1):+.3f}, p90 {q(sl,.9):+.3f}), corr p50 {q(co,.5):+.3f} (n {len(sl)})')

if __name__ == '__main__':
    sys.exit(main())
