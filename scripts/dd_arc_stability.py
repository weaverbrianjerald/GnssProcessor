#!/usr/bin/env python
"""UNWRAPPED, truth-free DD scale readout: per-chord ARC STABILITY of the traced DD estimate — v3.

A chord (sat_i, ref_sat, sta_a, sta_b) has ONE true integer DD per band for as long as nothing re-references it,
so its traced dd_cyc(t) should be constant.  The SAME chords are measured on the SAME arcs in both bands:

    e1(t) = dd_cyc(L1 band), e2(t) = dd_cyc(L2 band), wl(t) = e1 - e2        (paired epochs only)

per contiguous arc (gap <= --gap-ep epochs, length >= --min-arc), after dropping the first --trim epochs of every
arc (the newborn leg's cold-birth convergence would otherwise dominate an endpoint drift):
    MAD    1.4826 * median|x - median(x)|                 unwrapped wander / noise scale, cycles
    drift  |median(last 10 epochs) - median(first 10)|    slow walk over the arc, cycles (tail medians, not endpoints)
    slope  a in (e2 - med e2) = a * (e1 - med e1)         cross-band fingerprint: 1.283 = iono-driven (f1/f2),
           0.779 = range-common (lambda1/lambda2), 1.0 = cycle-common
REF-FREE: within each (epoch, sta_a, sta_b) group of chords inside a valid arc, the median over satellites of
the arc-relative deviation is the reference satellite's own wander (common to the group); subtract it
(groups >= --min-group) and re-take the arc statistics.  ⚠ Subtracting a group's OWN median shrinks the
statistic by itself (the estimator trap), so a size-matched NULL is printed beside it: every chord's deviation
series is replaced by iid Gaussian noise of that chord's own raw MAD (same epochs, same group membership), the
same subtraction is applied, and the null's ref-free/raw ratio is what a pure-estimator shrinkage looks like.
Quote the GAP between the real ratio and the null ratio, never the ref-free number alone.
Arcs are keyed on ref_sat, so a reference switch always starts a new arc (never enters a drift as a step).

--hi accepts a comma list (e.g. L1C,L1W): the L1 band is split across two signals by receiver type on the field
(a chord exists per signal), so the union represents the network where L1C alone is a receiver-type subset.

Neither readout sees a CONSTANT bias (a frozen wrong level is perfectly stable) — that is the wrapped fraction
test's job; together they split a multi-cycle DD error into BIAS (stable, wrapped-uniform) vs WANDER.

Usage: python dd_arc_stability.py <run_dir> [--hi L1C,L1W] [--lo L2W] [--settle-h 6] [--min-arc 20] [--gap-ep 2]
                                  [--min-group 3] [--trim 10] [--cons G] [--seed 1]
"""
import argparse, csv, io, os, random, statistics as st, sys
from collections import defaultdict

def q(v, f):
    return v[int(f * (len(v) - 1))] if v else float('nan')

def mad(vals):
    m = st.median(vals)
    return 1.4826 * st.median([abs(x - m) for x in vals])

def arcs_of(series, gap_s, min_arc, trim):
    series.sort()
    out, cur = [], [series[0]]
    for prev, nxt in zip(series, series[1:]):
        if nxt[0] - prev[0] > gap_s + 0.5: out.append(cur); cur = [nxt]
        else: cur.append(nxt)
    out.append(cur)
    return [a[trim:] for a in out if len(a) - trim >= min_arc]

def tail_drift(v):
    k = min(10, len(v) // 2)
    return abs(st.median(v[-k:]) - st.median(v[:k]))

def stats_line(name, mads, drifts):
    mads = sorted(mads); drifts = sorted(drifts)
    if not mads: return f'  {name:10s} (no arcs)'
    sh = lambda v, t: sum(1 for x in v if x < t) / len(v)
    return (f'  {name:10s} MAD p50 {q(mads,.5):7.3f} p90 {q(mads,.9):7.3f} <0.1 {sh(mads,.1):5.1%} <0.3 {sh(mads,.3):5.1%} >1 {1-sh(mads,1):5.1%}'
            f' | drift p50 {q(drifts,.5):7.3f} p90 {q(drifts,.9):7.3f} <0.3 {sh(drifts,.3):5.1%} >1 {1-sh(drifts,1):5.1%}  (n {len(mads)})')

def reffree_pass(dev, min_group, min_arc):
    """dev[(t, sa, sb)][sat] = (d1, d2, dw, arckey) -> per-arc ref-free MADs/drifts per name."""
    resid = defaultdict(lambda: defaultdict(list)); n_groups = n_small = 0
    for g, sats in dev.items():
        if len(sats) < min_group: n_small += 1; continue
        n_groups += 1
        meds = [st.median(x[i] for x in sats.values()) for i in range(3)]
        for sat, (d1, d2, dw, ak) in sats.items():
            for i, name in enumerate(('e1', 'e2', 'wl')):
                resid[ak][name].append((g[0], (d1, d2, dw)[i] - meds[i]))
    out = {k: ([], []) for k in ('e1', 'e2', 'wl')}
    for ak, d in resid.items():
        for name, ser in d.items():
            if len(ser) < min_arc: continue
            ser.sort(); v = [x[1] for x in ser]
            out[name][0].append(mad(v)); out[name][1].append(tail_drift(v))
    return out, n_groups, n_small

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run_dir'); ap.add_argument('--cons', default='G')
    ap.add_argument('--hi', default='L1C,L1W'); ap.add_argument('--lo', default='L2W')
    ap.add_argument('--settle-h', type=float, default=6.0); ap.add_argument('--min-arc', type=int, default=20)
    ap.add_argument('--gap-ep', type=int, default=2); ap.add_argument('--min-group', type=int, default=3)
    ap.add_argument('--trim', type=int, default=10); ap.add_argument('--seed', type=int, default=1)
    a = ap.parse_args()
    his = [s.strip() for s in a.hi.split(',') if s.strip()]
    p = os.path.join(a.run_dir, 'logs', 'network_dd_fractional.csv')
    hi, lo = defaultdict(dict), defaultdict(dict); t0 = None; n_hi = n_lo = 0
    with io.open(p, encoding='utf-8', errors='replace') as f:
        r = csv.DictReader(f)
        for row in r:
            if row['cons'] != a.cons: continue
            sig = row['signal']
            if sig not in his and sig != a.lo: continue
            try: tow = float(row['tow']); dd = float(row['dd_cyc'])
            except ValueError: continue
            if t0 is None: t0 = tow
            if tow - t0 < a.settle_h * 3600.0: continue
            key = (row['sat_i'], row['ref_sat'], row['sta_a'], row['sta_b'])
            if sig == a.lo: lo[key][tow] = dd; n_lo += 1
            else: hi[key][tow] = dd; n_hi += 1
    paired = {}
    for key, s1 in hi.items():
        s2 = lo.get(key)
        if not s2: continue
        common = sorted(set(s1) & set(s2))
        if len(common) >= a.min_arc + a.trim: paired[key] = [(t, s1[t], s2[t]) for t in common]
    n_pair = sum(len(v) for v in paired.values())
    print(f'{os.path.basename(a.run_dir.rstrip("/\\"))} {a.cons} {"+".join(his)}/{a.lo}: rows {n_hi}/{n_lo}, paired rows {n_pair} on {len(paired)} chords, settled >= h{a.settle_h:g}, arcs >= {a.min_arc} ep after trimming {a.trim}, gap <= {a.gap_ep} ep')
    raw = {k: ([], []) for k in ('e1', 'e2', 'wl')}
    slopes, corrs = [], []
    dev = defaultdict(dict); rawmad = {}
    n_arcs = 0
    for key, ser in paired.items():
        for ai, arc in enumerate(arcs_of(ser, 30.0 * a.gap_ep, a.min_arc, a.trim)):
            n_arcs += 1; ak = (key, ai)
            e1 = [x[1] for x in arc]; e2 = [x[2] for x in arc]; wl = [x[1] - x[2] for x in arc]
            m1, m2, mw = st.median(e1), st.median(e2), st.median(wl)
            for name, v in (('e1', e1), ('e2', e2), ('wl', wl)):
                raw[name][0].append(mad(v)); raw[name][1].append(tail_drift(v))
            rawmad[ak] = (mad(e1), mad(e2), mad(wl))
            d1 = [x - m1 for x in e1]; d2 = [x - m2 for x in e2]
            sxx = sum(x * x for x in d1); sxy = sum(x * y for x, y in zip(d1, d2)); syy = sum(y * y for y in d2)
            if sxx > 1e-12 and syy > 1e-12:
                slopes.append(sxy / sxx); corrs.append(sxy / (sxx * syy) ** 0.5)
            for (t, x1, x2), a1, a2 in zip(arc, d1, d2):
                dev[(t, key[2], key[3])][key[0]] = (a1, a2, (x1 - x2) - mw, ak)
    print(f'  arcs {n_arcs}; cross-band slope (e2 vs e1 deviations) p50 {q(sorted(slopes),.5):.3f} p10 {q(sorted(slopes),.1):.3f} p90 {q(sorted(slopes),.9):.3f}; corr p50 {q(sorted(corrs),.5):.3f}   [1.283 iono-driven, 0.779 range-common, 1.0 cycle-common]')
    print(f'RAW (as-traced, reference wander included; drift = |median(last 10) - median(first 10)| after trimming {a.trim} birth epochs)')
    for name in ('e1', 'e2', 'wl'): print(stats_line(name, *raw[name]))
    rf, n_groups, n_small = reffree_pass(dev, a.min_group, a.min_arc)
    # size-matched NULL: iid Gaussian deviations with each chord's own raw MAD, same epochs/groups
    rnd = random.Random(a.seed)
    dev_null = defaultdict(dict)
    for g, sats in dev.items():
        for sat, (d1, d2, dw, ak) in sats.items():
            s1, s2, sw = rawmad[ak]
            dev_null[g][sat] = (rnd.gauss(0, s1), rnd.gauss(0, s2), rnd.gauss(0, sw), ak)
    nf, _, _ = reffree_pass(dev_null, a.min_group, a.min_arc)
    print(f'REF-FREE (group median of the arc-relative deviation removed; groups >= {a.min_group}: {n_groups} used, {n_small} too small) — beside a size-matched iid-Gaussian NULL')
    for name in ('e1', 'e2', 'wl'):
        print(stats_line(name, *rf[name]))
        rm = sorted(raw[name][0]); fm = sorted(rf[name][0]); nm = sorted(nf[name][0])
        if rm and fm and nm:
            r_real = q(fm, .5) / q(rm, .5); r_null = q(nm, .5) / q(rm, .5)
            print(f'    {name}: ref-free/raw MAD p50 ratio REAL {r_real:.3f} vs NULL {r_null:.3f} -> gap {r_real - r_null:+.3f} (negative = the reference carried genuine common wander beyond the estimator shrinkage)')

if __name__ == '__main__':
    sys.exit(main())
