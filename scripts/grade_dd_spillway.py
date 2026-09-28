"""Spillway forensics on network_dd_fractional.csv (root-cause kit, 2026-08-02).

Four analyses on the traced between-station DDs (all datum-invariant, standing
invariant #5). Run as:

    python scripts/grade_dd_spillway.py <run_dir> [tow_min]

1. Age bins:   med|step|, med dd_sigma_cyc, med|frac| conditioned on key age.
               dd_sigma_cyc is carry-blind: jointConditionalCovariance carries
               no dd-prior term, only the mean path has the kappa correction,
               so a flat sigma does not mean no accumulation.
2. Structure:  D(k) = med|dd(t+k)-dd(t)|^2 on mature series (age>=25).
               Converged looks flat at sqrt(2)*noise; a random walk grows as
               k^0.5. Measured 2026-08-02: k^0.75 with a 30-60 min knee at
               35-40 cyc, consistent with a persistent drift rather than noise
               or convergence.
3. Cross-band: same-epoch step ratio in meters between bands, ref-free (the
               cross-satellite difference cancels each signal pool's lexical
               reference satellite). Reference ratios:
               common-cycle (dN1=dN2): lambda2/lambda1 = 1.2833 (L1/L2)
               raw iono in observable : (f1/f2)^2     = 1.6469
               geometric (datum/clock): 1.0
               Measured 1.275-1.291, matching the common-cycle ratio: the
               iono-representation residual projects onto the ambiguity pair
               along the widelane-preserving direction, at about 6.18 cyc per
               meter (c = delta*(l1 + a2*l2)/(l1^2 + l2^2), the projection of
               the iono direction onto the common-cycle direction).
4. Pair rank:  med mature step per station pair. Measured: the ranking follows
               iono-gradient geometry (equatorial-anomaly / cross-sector pairs
               at 5-8 cyc/ep; same-sector or high-latitude pairs at 0.6-1.4),
               consistent with sky structure that a degree-8 spherical-harmonic
               surface cannot follow, rather than a self-generated gauge wander.
"""
import csv
import math
import os
import sys
from collections import defaultdict

EP_S = 30.0
MAX_GAP_EP = 2
BINS = [(1, 3), (3, 6), (6, 12), (12, 25), (25, 50), (50, 100), (100, 1 << 30)]
LAGS = [1, 2, 4, 8, 16, 32, 64, 128, 256]


def med(xs):
    if not xs:
        return float('nan')
    s = sorted(xs)
    n = len(s)
    return s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])


def load(path):
    series = defaultdict(list)
    with open(path, newline='') as f:
        for row in csv.DictReader(f):
            if float(row['lambda_m']) <= 0:
                continue
            key = (row['cons'], row['signal'], row['sat_i'], row['ref_sat'],
                   row['sta_a'], row['sta_b'])
            series[key].append((float(row['tow']), float(row['dd_cyc']),
                                float(row['dd_sigma_cyc']),
                                abs(float(row['frac_cyc'])), float(row['dd_m'])))
    for rows in series.values():
        rows.sort()
    return series


def segments(rows):
    seg = []
    for r in rows:
        if seg and r[0] - seg[-1][0] > MAX_GAP_EP * EP_S:
            yield seg
            seg = []
        seg.append(r)
    if seg:
        yield seg


def age_bins(series, tow_min):
    stepb, sigb, fracb = defaultdict(list), defaultdict(list), defaultdict(list)

    def binof(age):
        for lo, hi in BINS:
            if lo <= age < hi:
                return (lo, hi)
        return None

    for rows in series.values():
        for seg in segments(rows):
            for i, (tow, dd, sig, frac, _m) in enumerate(seg):
                if tow_min is not None and tow < tow_min:
                    continue
                b = binof(i)
                if b is None or i < 1:
                    continue
                if sig > 0:
                    sigb[b].append(sig)
                fracb[b].append(frac)
                ptow, pdd = seg[i - 1][0], seg[i - 1][1]
                if tow - ptow == EP_S:
                    stepb[b].append(abs(dd - pdd))
    print("age bin (ep) |      n | med|dDD| cyc | med sigma cyc | med|frac|")
    for b in BINS:
        lab = f"{b[0]}-{b[1] if b[1] < (1 << 30) else 'inf'}"
        print(f"{lab:>12} | {len(stepb[b]):>6} | {med(stepb[b]):>12.3f} |"
              f" {med(sigb[b]):>13.3f} | {med(fracb[b]):>9.3f}")


def structure(series, tow_min, min_age=25):
    dsq = {k: [] for k in LAGS}
    for rows in series.values():
        for seg in segments(rows):
            if len(seg) < min_age + 2:
                continue
            towix = {round(t / EP_S): d for t, d, *_ in seg[min_age:]}
            for i in sorted(towix):
                if tow_min is not None and i * EP_S < tow_min:
                    continue
                for k in LAGS:
                    if i + k in towix:
                        dsq[k].append((towix[i + k] - towix[i]) ** 2)
    print("lag ep |  min |       n | sqrt(medD) cyc |     m")
    for k in LAGS:
        if not dsq[k]:
            continue
        r = math.sqrt(med(dsq[k]))
        print(f"{k:>6} | {k * 0.5:>4.0f} | {len(dsq[k]):>7} | {r:>14.2f} |"
              f" {r * 0.1903:>5.2f}")


def cross_band(series):
    cells = defaultdict(lambda: defaultdict(dict))
    for (cons, sig, sat, _ref, a, b), rows in series.items():
        for tow, _dd, _s, _f, dd_m in rows:
            cells[sig][(round(tow / EP_S), a, b)][sat] = dd_m
    steps = defaultdict(dict)
    for sig, bycell in cells.items():
        for (ix, a, b), sats in bycell.items():
            nxt = bycell.get((ix + 1, a, b))
            if not nxt:
                continue
            common = sorted(s for s in sats if s in nxt)
            for i in range(len(common)):
                for j in range(i + 1, len(common)):
                    si, sj = common[i], common[j]
                    steps[sig][(si, sj, a, b, ix)] = \
                        (nxt[si] - nxt[sj]) - (sats[si] - sats[sj])
    base = steps.get('L1C', {})
    for sig in sorted(steps):
        if sig == 'L1C':
            continue
        ratios, d1s, d2s = [], [], []
        for k, d1 in base.items():
            if k in steps[sig] and abs(d1) > 0.10:
                ratios.append(steps[sig][k] / d1)
                d1s.append(d1)
                d2s.append(steps[sig][k])
        if len(ratios) < 30:
            continue
        s_reg = sum(x * y for x, y in zip(d1s, d2s)) / sum(x * x for x in d1s)
        print(f"L1C vs {sig}: n={len(ratios)}  med ratio={med(ratios):.3f}  "
              f"reg={s_reg:.3f}   [cc L2W 1.2833 / L5Q 1.3391 | iono 1.6469 /"
              f" 1.7933 | geo 1.0]")


def pair_rank(series, min_age=25, top=8):
    pair_steps = defaultdict(list)
    for (c, s, sat, ref, a, b), rows in series.items():
        for seg in segments(rows):
            for i in range(min_age + 1, len(seg)):
                if seg[i][0] - seg[i - 1][0] == EP_S:
                    pair_steps[(a, b)].append(abs(seg[i][1] - seg[i - 1][1]))
    rank = sorted(((med(v), len(v), a, b)
                   for (a, b), v in pair_steps.items() if len(v) >= 50),
                  reverse=True)
    print("worst mature walkers:")
    for m, n, a, b in rank[:top]:
        print(f"  {m:6.2f} cyc/ep  n={n:<5} {a} - {b}")
    print("quietest:")
    for m, n, a, b in rank[-top:]:
        print(f"  {m:6.2f} cyc/ep  n={n:<5} {a} - {b}")


if __name__ == '__main__':
    run_dir = sys.argv[1]
    tow_min = float(sys.argv[2]) if len(sys.argv) > 2 else None
    path = os.path.join(run_dir, 'logs', 'network_dd_fractional.csv')
    series = load(path)
    print(f"== {run_dir}  keys={len(series)}  tow_min={tow_min} ==")
    print("\n-- 1. age bins --")
    age_bins(series, tow_min)
    print("\n-- 2. structure function (mature, age>=25) --")
    structure(series, tow_min)
    print("\n-- 3. cross-band step ratio (ref-free) --")
    cross_band(series)
    print("\n-- 4. station-pair ranking --")
    pair_rank(series)
