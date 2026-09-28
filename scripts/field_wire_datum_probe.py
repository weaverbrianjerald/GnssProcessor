#!/usr/bin/env python3
"""Field wire: datum-station attribution probe (SPEC_synthetic_minimum_network §9.9).

The §9.8 sim mechanism: under the legacy pivot emit, the wire's fleet-common
clock level w(t) carries the datum receiver's own clock-estimation error 1:1
(the datum direction has zero redundancy: only the datum station constrains
it, and every other receiver's free clock re-zeros it). The receiver-side
driver of that error is the station-common part of its code error (multipath
plus noise).

This probe tests the mechanism on a retained field run without re-processing:

  w(t)        = per-epoch median over satellites of the emitted C records
                (products.txt), increments formed per-satellite then medianed,
                pairs across datum_changed epochs dropped.
  mp_i(t)     = per-station iono-free multipath proxy from network_slip_raw.csv:
                l1c1 + (2/(gamma-1))*gf  (gamma = (f1/f2)^2; iono cancels, arc
                constants drop in increments, d(mp_proxy) ~= -d(mp_code)),
                LLI-flagged rows excluded, increments per-satellite -> median.

Then corr( dw, dmp_i ) per station i. Prediction if the field wander is
datum-owned like the sim: the datum station is the top-|corr| outlier with
positive sign (dw = -dm and dmp = -dm); control stations' code multipath is
absorbed by their own free receiver clocks and should sit at the noise floor.
A flat table instead says the field wander is not receiver-code-driven
(shared state or product side), which is equally a result.

Usage:
  python scripts/field_wire_datum_probe.py <run_dir> [--datum MBAR00UGA]
         [--settle-hours 2] [--min-sats 6] [--csv out.csv]
  <run_dir> must hold products.txt and logs/network_slip_raw.csv.
"""

import argparse
import math
import os
import sys

GAMMA_L1L2 = (1575.42 / 1227.60) ** 2   # GPS L1/L2; L2 pair dominates this lineage
K_GF = 2.0 / (GAMMA_L1L2 - 1.0)


def median(xs):
    s = sorted(xs)
    n = len(s)
    if n == 0:
        return float("nan")
    return s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])


def qtow(t):
    """Quantize a TOW to the nearest second for cross-file joins: the product
    emitter's E rows carry accumulated float dust (e.g. 302370.00000190735)
    while the trace CSVs print %.3f, so exact float keys never meet. The epoch
    grid is 30 s, so nearest-second is unambiguous."""
    return float(round(t))


def parse_wire(path):
    """products.txt -> ordered [(tow, datum_changed, {sat: c0})]."""
    epochs = []
    tow = None
    changed = 0
    vals = {}

    def commit():
        if tow is not None and vals:
            epochs.append((tow, changed, dict(vals)))

    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("E,"):
                commit()
                p = line.strip().split(",")
                try:
                    tow = qtow(float(p[2]))
                    changed = int(p[6]) if len(p) > 6 and p[6] and "=" not in p[6] else 0
                except (IndexError, ValueError):
                    tow = None
                    changed = 0
                vals = {}
            elif line.startswith("C,") and tow is not None:
                p = line.strip().split(",")
                if len(p) >= 3 and "=" not in p[2]:
                    try:
                        v = float(p[2])
                    except ValueError:
                        continue
                    if math.isfinite(v):
                        vals[p[1]] = v
    commit()
    return epochs


def parse_mp(path):
    """network_slip_raw.csv -> {station: {tow: {sat: mp_proxy}}} (dual, LLI-clean)."""
    out = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        header = f.readline().strip().split(",")
        idx = {name: i for i, name in enumerate(header)}
        need = ["tow", "station", "sat", "gf_m", "l1_c1_m", "lli1", "lli2", "sig2"]
        for n in need:
            if n not in idx:
                sys.exit(f"FATAL: column '{n}' missing from {path}")
        it, ist, isa = idx["tow"], idx["station"], idx["sat"]
        ig, il, i1, i2, is2 = idx["gf_m"], idx["l1_c1_m"], idx["lli1"], idx["lli2"], idx["sig2"]
        for line in f:
            p = line.rstrip("\n").split(",")
            if len(p) <= max(ig, il, i1, i2):
                continue
            if not p[is2]:               # single-band row: no GF -> no iono-null proxy
                continue
            if p[i1] != "0" or p[i2] != "0":   # LLI-flagged: arc break, constants move
                continue
            try:
                tow = qtow(float(p[it]))
                gf = float(p[ig])
                l1c1 = float(p[il])
            except ValueError:
                continue
            if not (math.isfinite(gf) and math.isfinite(l1c1)):
                continue
            out.setdefault(p[ist], {}).setdefault(tow, {})[p[isa]] = l1c1 + K_GF * gf
    return out


def increments(series_by_tow, pairs, min_sats):
    """{tow:{sat:v}} + [(t0,t1)] -> {t1: median over common sats of v(t1)-v(t0)}."""
    out = {}
    for t0, t1 in pairs:
        a = series_by_tow.get(t0)
        b = series_by_tow.get(t1)
        if not a or not b:
            continue
        ds = [b[s] - a[s] for s in b if s in a]
        if len(ds) >= min_sats:
            out[t1] = median(ds)
    return out


def robust_trim(pairs_xy, k=8.0):
    """Drop joint samples where either coordinate is a >k*MAD outlier."""
    xs = [x for x, _ in pairs_xy]
    ys = [y for _, y in pairs_xy]
    mx, my = median(xs), median(ys)
    madx = median([abs(x - mx) for x in xs]) or 1e-12
    mady = median([abs(y - my) for y in ys]) or 1e-12
    return [(x, y) for x, y in pairs_xy
            if abs(x - mx) <= k * 1.4826 * madx and abs(y - my) <= k * 1.4826 * mady]


def corr_slope(pairs_xy):
    n = len(pairs_xy)
    if n < 30:
        return float("nan"), float("nan"), n
    xs = [x for x, _ in pairs_xy]
    ys = [y for _, y in pairs_xy]
    mx = sum(xs) / n
    my = sum(ys) / n
    sxy = sum((x - mx) * (y - my) for x, y in pairs_xy)
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    if sxx <= 0 or syy <= 0:
        return float("nan"), float("nan"), n
    return sxy / math.sqrt(sxx * syy), sxy / sxx, n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir")
    ap.add_argument("--datum", default="MBAR00UGA")
    ap.add_argument("--settle-hours", type=float, default=2.0)
    ap.add_argument("--min-sats", type=int, default=6)
    ap.add_argument("--max-dt", type=float, default=60.0)
    ap.add_argument("--csv", default=None)
    args = ap.parse_args()

    wire_path = os.path.join(args.run_dir, "products.txt")
    mp_path = os.path.join(args.run_dir, "logs", "network_slip_raw.csv")
    epochs = parse_wire(wire_path)
    if len(epochs) < 100:
        sys.exit(f"FATAL: only {len(epochs)} wire epochs in {wire_path}")
    t_start = epochs[0][0]
    t_end = epochs[-1][0]
    cut = t_start + args.settle_hours * 3600.0

    # Increment pairs: consecutive emitted epochs, settled, close in time, and the
    # later epoch not datum_changed (a pivot handover steps every C together and
    # the median would read it as wander).
    wire_by_tow = {t: v for t, _, v in epochs}
    pairs = []
    n_datum_changed = 0
    for (t0, _, _), (t1, ch1, _) in zip(epochs, epochs[1:]):
        if ch1:
            n_datum_changed += 1
            continue
        if t1 - t0 <= args.max_dt and t0 >= cut:
            pairs.append((t0, t1))
    dw = increments(wire_by_tow, pairs, args.min_sats)

    # Field wander magnitude for the report: settled level series, longest
    # unchanged-datum segment, demeaned std.
    seg, best = [], []
    for t, ch, v in epochs:
        if t < cut:
            continue
        if ch:
            if len(seg) > len(best):
                best = seg
            seg = []
        vals = list(v.values())
        if vals:
            seg.append(median(vals))
    if len(seg) > len(best):
        best = seg
    if len(best) >= 10:
        m = sum(best) / len(best)
        sd = math.sqrt(sum((x - m) ** 2 for x in best) / len(best))
        print(f"-- wire common level: {len(epochs)} epochs, {n_datum_changed} datum-changed; "
              f"longest settled segment {len(best)} ep, demeaned std = {sd:.3f} m --")

    mp = parse_mp(mp_path)
    rows = []
    for sta, series in sorted(mp.items()):
        dm = increments(series, pairs, args.min_sats)
        joint = [(dw[t], dm[t]) for t in dw if t in dm]
        joint = robust_trim(joint)
        r, b, n = corr_slope(joint)
        if not math.isnan(r):
            rows.append((abs(r), r, b, n, sta))
    rows.sort(reverse=True)

    print(f"-- corr( d(wire common), d(mp_proxy common at station) ), settled "
          f"{args.settle_hours:.0f}h+, {len(rows)} stations --")
    print(f"   {'rank':<5}{'station':<12}{'corr':>8}{'slope':>8}{'n':>6}")
    datum_rank = None
    for rank, (_, r, b, n, sta) in enumerate(rows, 1):
        mark = "  <== DATUM" if sta == args.datum else ""
        if rank <= 10 or sta == args.datum:
            print(f"   {rank:<5}{sta:<12}{r:>+8.3f}{b:>+8.3f}{n:>6}{mark}")
        if sta == args.datum:
            datum_rank = rank
    ctrl = [abs_r for abs_r, _, _, _, sta in rows if sta != args.datum]
    if ctrl:
        print(f"   controls: median |corr| = {median(ctrl):.3f}, "
              f"max |corr| = {max(ctrl):.3f}")
    if datum_rank is None:
        print(f"   WARNING: datum station '{args.datum}' absent from the mp trace")
    else:
        print(f"   verdict: datum station rank {datum_rank} of {len(rows)} by |corr|"
              + ("  => DATUM-OWNED signature" if datum_rank == 1 else
                 "  => NOT the top driver — wander not (only) receiver-code-driven"))

    if args.csv:
        with open(args.csv, "w") as f:
            f.write("rank,station,corr,slope,n\n")
            for rank, (_, r, b, n, sta) in enumerate(rows, 1):
                f.write(f"{rank},{sta},{r:.4f},{b:.4f},{n}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
