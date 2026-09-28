#!/usr/bin/env python
"""The fourth member: whether the per-epoch ZWD state closes the two-band budget of the DD-ambiguity/iono co-motion.
(SPEC_amb_iono §6.15.3a item 5, peer derivation 18:25.)

With a band-common metre term T (DD troposphere) beside the iono deviation ΔI and a cycle-common ambiguity A:
    band 1:  λ1·A − ΔI       + T = 0
    band 2:  λ2·A − 1.6469·ΔI + T = 0     ⇒  A = 2.283·ΔI_cyc  and  T = −1.283·ΔI   (exactly)
Predictions (pre-registered before running): per chord-arc on the field control,
    (i)  the DD-ZWD deviation regressed on the DD-iono deviation (both metres) has slope ≈ −1.28, high |corr|;
    (ii) the two-regressor fit  e1_dev = a·ΔI_cyc + t·ΔT_cyc  gives a → ~2.28 (up from the attenuated 1.7) and
         t → ~−1  (band-1 phase: λ1·ΔN1 = ΔI − ΔT ⇒ e1 = ΔI_cyc − ΔT_cyc, i.e. (+1, −1) if the tropo term is the closer).
DD tropo per chord:  DD_T = Z_a·(mf_a^i − mf_a^ref) − Z_b·(mf_b^i − mf_b^ref),  mf = 1/sin(el)  (adequate for the test),
Z from network_zwd.csv (per station-epoch, metres, x_lin ≡ 0), elevations from network_ionosphere.csv.

Usage: python tropo_arc_wander.py <run_dir> [--settle-h 6] [--min-arc 20] [--gap-ep 2] [--trim 10]
"""
import argparse, csv, io, math, os, statistics as st, sys
from collections import defaultdict

LAM1 = 0.190294

def q(v, f): return v[int(f * (len(v) - 1))] if v else float('nan')
def arcs_of(series, gap_s, min_arc, trim):
    series.sort(); out, cur = [], [series[0]]
    for prev, nxt in zip(series, series[1:]):
        if nxt[0] - prev[0] > gap_s + 0.5: out.append(cur); cur = [nxt]
        else: cur.append(nxt)
    out.append(cur)
    return [a[trim:] for a in out if len(a) - trim >= min_arc]
def dev(v):
    m = st.median(v); return [x - m for x in v]
def ols1(x, y):
    sxx = sum(a * a for a in x); sxy = sum(a * b for a, b in zip(x, y)); syy = sum(b * b for b in y)
    if sxx <= 1e-12 or syy <= 1e-12: return None
    return sxy / sxx, sxy / (sxx * syy) ** 0.5
def ols2(x1, x2, y):
    s11 = sum(a * a for a in x1); s22 = sum(b * b for b in x2); s12 = sum(a * b for a, b in zip(x1, x2))
    s1y = sum(a * c for a, c in zip(x1, y)); s2y = sum(b * c for b, c in zip(x2, y))
    det = s11 * s22 - s12 * s12
    if det <= 1e-12 * max(1e-12, s11 * s22): return None
    return (s22 * s1y - s12 * s2y) / det, (s11 * s2y - s12 * s1y) / det

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run_dir'); ap.add_argument('--cons', default='G'); ap.add_argument('--hi', default='L1C,L1W')
    ap.add_argument('--settle-h', type=float, default=6.0); ap.add_argument('--min-arc', type=int, default=20)
    ap.add_argument('--gap-ep', type=int, default=2); ap.add_argument('--trim', type=int, default=10)
    a = ap.parse_args(); his = [s.strip() for s in a.hi.split(',')]
    L = lambda f: os.path.join(a.run_dir, 'logs', f)
    iono, el = {}, {}; t0 = None
    for row in csv.DictReader(io.open(L('network_ionosphere.csv'), encoding='utf-8', errors='replace')):
        if not row['sat'].startswith(a.cons): continue
        try: tow = float(row['tow']); tot = float(row['iono_total_m']); e = float(row['elevation_deg'])
        except ValueError: continue
        if t0 is None: t0 = tow
        if tow - t0 < a.settle_h * 3600.0: continue
        iono[(tow, row['station'], row['sat'])] = tot; el[(tow, row['station'], row['sat'])] = e
    zwd = {}
    for row in csv.DictReader(io.open(L('network_zwd.csv'), encoding='utf-8', errors='replace')):
        try: tow = float(row['tow']); z = float(row['zwd_m'])
        except ValueError: continue
        if tow - t0 < a.settle_h * 3600.0: continue
        zwd[(tow, row['station'])] = z
    mf = lambda e: 1.0 / math.sin(math.radians(max(e, 3.0)))
    chords = defaultdict(list); n_rows = n_join = 0
    for row in csv.DictReader(io.open(L('network_dd_fractional.csv'), encoding='utf-8', errors='replace')):
        if row['cons'] != a.cons or row['signal'] not in his: continue
        try: tow = float(row['tow']); e1 = float(row['dd_cyc'])
        except ValueError: continue
        if tow - t0 < a.settle_h * 3600.0: continue
        n_rows += 1
        sa, sb, si, sr = row['sta_a'], row['sta_b'], row['sat_i'], row['ref_sat']
        try:
            dI = (iono[(tow, sa, si)] - iono[(tow, sa, sr)]) - (iono[(tow, sb, si)] - iono[(tow, sb, sr)])
            dT = zwd[(tow, sa)] * (mf(el[(tow, sa, si)]) - mf(el[(tow, sa, sr)])) - zwd[(tow, sb)] * (mf(el[(tow, sb, si)]) - mf(el[(tow, sb, sr)]))
        except KeyError: continue
        n_join += 1; chords[(si, sr, sa, sb)].append((tow, e1, dI, dT))
    sl_TI, co_TI, a_I, t_T, a_I1, madT, madI = [], [], [], [], [], [], []
    n_arcs = 0
    for key, ser in chords.items():
        for arc in arcs_of(ser, 30.0 * a.gap_ep, a.min_arc, a.trim):
            n_arcs += 1
            e1 = dev([x[1] for x in arc]); dI = dev([x[2] for x in arc]); dT = dev([x[3] for x in arc])
            madI.append(1.4826 * st.median(abs(x) for x in dI)); madT.append(1.4826 * st.median(abs(x) for x in dT))
            r = ols1(dI, dT)
            if r: sl_TI.append(r[0]); co_TI.append(r[1])
            r1 = ols1([x / LAM1 for x in dI], e1)
            if r1: a_I1.append(r1[0])
            r2 = ols2([x / LAM1 for x in dI], [x / LAM1 for x in dT], e1)
            if r2: a_I.append(r2[0]); t_T.append(r2[1])
    name = os.path.basename(a.run_dir.rstrip('/\\'))
    print(f'{name} {a.cons}: DD rows {n_rows}, joined (iono+zwd+el) {n_join} on {len(chords)} chords, {n_arcs} arcs, settled >= h{a.settle_h:g}')
    print(f'  arc MAD: DD-iono {q(sorted(madI),.5):.3f} m | DD-tropo (1/sin el mapping) {q(sorted(madT),.5):.3f} m')
    print(f'  (i)  DD-tropo dev on DD-iono dev (m/m): slope p50 {q(sorted(sl_TI),.5):+.3f} (p10 {q(sorted(sl_TI),.1):+.3f}, p90 {q(sorted(sl_TI),.9):+.3f}), corr p50 {q(sorted(co_TI),.5):+.3f}   [predicted −1.283 if ZWD closes the budget]')
    print(f'  (ii) e1 on DD-iono alone: a p50 {q(sorted(a_I1),.5):+.3f}  |  e1 on (DD-iono, DD-tropo) jointly [L1 cyc]: a p50 {q(sorted(a_I),.5):+.3f} (p10 {q(sorted(a_I),.1):+.3f}, p90 {q(sorted(a_I),.9):+.3f}), t p50 {q(sorted(t_T),.5):+.3f} (p10 {q(sorted(t_T),.1):+.3f}, p90 {q(sorted(t_T),.9):+.3f})   [predicted a → +2.28 (from 1.7), t ≈ −1]')

if __name__ == '__main__':
    sys.exit(main())
