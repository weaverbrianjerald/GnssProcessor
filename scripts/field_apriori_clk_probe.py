#!/usr/bin/env python3
"""Field wire: a-priori CLK common-drift probe (SPEC_synthetic_minimum_network §9.9.1).

§9.9 eliminated the receiver-code driver of the field wire's common-level
wander (1.735 m).  The next candidate is the a-priori frame itself: the C
records are corrections about the resolved CLK product, so

    w(t) = fleet-common(C) = [estimator common realization] − [product common]

and a fleet-common drift of the product enters w(t) directly with slope −1,
trackable by no station observable (exactly the §9.9 signature).  This probe
computes the product's own fleet-common series A(t) = c · median_s dt_s(t)
from the cached RINEX CLK and correlates it with w(t) on the joint epochs.

  corr(w, A) ≈ −1 (slope ≈ −1)  => the wander is the a-priori frame moving
                                    (product-side; datum-invariant consumers
                                    never see it, per the Psychas mandate)
  corr ≈ 0                      => the wander is estimator-internal (the
                                    CB/iono common exchanges, the level
                                    channel campaign target)

Both the demeaned level correlation (within the longest datum-stable settled
segment) and the 30 s increment correlation are reported; a linear trend is
also removed from both series as a variant (a linear timescale drift is
conventional and absorbable).

Usage:
  python scripts/field_apriori_clk_probe.py <run_dir> <clk_file> [<clk_file2> ...]
         [--settle-hours 2] [--week 2313] [--dow 3]
"""

import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from field_wire_datum_probe import parse_wire, median, qtow  # noqa: E402

C_LIGHT = 299792458.0


def parse_clk(paths, dow):
    """RINEX CLK AS records -> {tow: {sat: bias_m}} (GPS only)."""
    out = {}
    for path in paths:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            in_header = True
            for line in f:
                if in_header:
                    if "END OF HEADER" in line:
                        in_header = False
                    continue
                if not line.startswith("AS "):
                    continue
                p = line.split()
                # AS <sat> yyyy mm dd hh mi sec n bias [sigma]
                if len(p) < 10 or not p[1].startswith("G"):
                    continue
                try:
                    hh, mi = int(p[5]), int(p[6])
                    sec = float(p[7])
                    dd = int(p[4])
                    bias = float(p[9])
                except ValueError:
                    continue
                # Day-of-week offset from the file's own calendar date. The
                # caller anchors day `dow` (DOY 129 = 2024-05-08 = Wednesday = 3);
                # neighbouring-day files land one day earlier or later.
                tow = (dow + (dd - 8)) * 86400.0 + hh * 3600.0 + mi * 60.0 + sec
                out.setdefault(qtow(tow), {})[p[1]] = bias * C_LIGHT
    return out


def stats(xs):
    n = len(xs)
    m = sum(xs) / n
    return m, math.sqrt(sum((x - m) ** 2 for x in xs) / n)


def pearson(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxy = sum((a - mx) * (b - my) for a, b in zip(xs, ys))
    sxx = sum((a - mx) ** 2 for a in xs)
    syy = sum((b - my) ** 2 for b in ys)
    if sxx <= 0 or syy <= 0:
        return float("nan"), float("nan")
    return sxy / math.sqrt(sxx * syy), sxy / sxx


def detrend(ts, xs):
    n = len(ts)
    mt, mx = sum(ts) / n, sum(xs) / n
    stt = sum((t - mt) ** 2 for t in ts)
    b = sum((t - mt) * (x - mx) for t, x in zip(ts, xs)) / stt if stt > 0 else 0.0
    return [x - (mx + b * (t - mt)) for t, x in zip(ts, xs)], b


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir")
    ap.add_argument("clk", nargs="+")
    ap.add_argument("--settle-hours", type=float, default=2.0)
    ap.add_argument("--dow", type=int, default=3, help="day-of-week of the data day (DOY129=Wed=3)")
    args = ap.parse_args()

    epochs = parse_wire(os.path.join(args.run_dir, "products.txt"))
    cut = epochs[0][0] + args.settle_hours * 3600.0

    # Longest settled datum-stable segment of the wire common level.
    seg, best = [], []
    for t, ch, v in epochs:
        if t < cut:
            continue
        if ch:
            if len(seg) > len(best):
                best = seg
            seg = []
            continue
        vals = list(v.values())
        if vals:
            seg.append((t, median(vals)))
    if len(seg) > len(best):
        best = seg

    clk = parse_clk(args.clk, args.dow)
    joint = [(t, w, median(list(clk[t].values()))) for t, w in best if t in clk and clk[t]]
    if len(joint) < 50:
        sys.exit(f"FATAL: only {len(joint)} joint epochs (wire segment {len(best)}, clk {len(clk)})")

    ts = [t for t, _, _ in joint]
    ws = [w for _, w, _ in joint]
    as_ = [a for _, _, a in joint]
    mw, sw = stats(ws)
    ma, sa = stats(as_)
    print(f"-- joint settled datum-stable epochs: {len(joint)} "
          f"({(ts[-1]-ts[0])/3600.0:.1f} h) --")
    print(f"  wire common w(t): std = {sw:.3f} m")
    print(f"  a-priori CLK fleet-common A(t): std = {sa:.3f} m "
          f"(range {max(as_)-min(as_):.3f} m)")
    r_lvl, b_lvl = pearson(ws, as_)
    print(f"  LEVEL   corr(w, A) = {r_lvl:+.3f}   slope = {b_lvl:+.3f}")
    wd, bw = detrend(ts, ws)
    ad, ba = detrend(ts, as_)
    _, swd = stats(wd)
    _, sad = stats(ad)
    r_dt, b_dt = pearson(wd, ad)
    print(f"  DETREND w: trend {bw*3600:+.3f} m/h, resid std {swd:.3f} m; "
          f"A: trend {ba*3600:+.3f} m/h, resid std {sad:.3f} m")
    print(f"  DETREND corr(w, A) = {r_dt:+.3f}   slope = {b_dt:+.3f}")
    dws = [ws[i + 1] - ws[i] for i in range(len(ws) - 1)]
    das = [as_[i + 1] - as_[i] for i in range(len(as_) - 1)]
    r_inc, b_inc = pearson(dws, das)
    print(f"  INCR    corr(dw, dA) = {r_inc:+.3f}   slope = {b_inc:+.3f}")
    verdict = ("PRODUCT-FRAME-DRIVEN (w tracks -A)" if r_lvl < -0.75 and b_lvl < -0.5 else
               "NOT product-frame-driven — estimator-internal (level channel)"
               if abs(r_lvl) < 0.35 else "MIXED — decompose further")
    print(f"  verdict: {verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
