#!/usr/bin/env python3
"""Synthetic network: fleet-common C-record wander extractor / A-B comparator.

docs/SPEC_synthetic_minimum_network.md §9.8 (the datum-wander arm).

On the quiet fixture the a-priori CLK product is the truth (§4.2), so each
epoch's C record is pure estimator error, and its fleet-common component (the
robust median over satellites) is, up to a constant, minus the realized datum
receiver clock error:

    C(s,t) = -dt_datum_err(t) + const_s + eps_s(t)
    common(t) := median_s C(s,t) ~= -dt_datum_err(t) + median_s const_s

This is exactly the ~0.9-1.3 m rms channel §9.7.1/§9.7.9 measured (the
pivot-relative common level that does not converge with n and is slip-
independent).  The A/B question this tool answers: run the same data under two
different `network.clock_datum_station` choices and correlate the two settled,
demeaned common(t) series:

    P1 (wander owned by the datum station: its code multipath, noise and ZWD
        error filtering through the datum): corr ~ 0, std(A-B) ~ sqrt(2)*std
    P2 (wander in shared states, iono/CB common leakage every datum sees):
        corr ~ 1, std(A-B) << std

Parsing rules follow the SPEC §8 traps: join on TOW, never row index; stop the
positional parse at the first '=' token; a withheld epoch simply has no E
block.  The constant offset (A_c, d_IF medians) is not wander; all statistics
are on demeaned settled series.

Usage:
  python scripts/simnet_common_mode.py <productsA.txt> [<productsB.txt>]
         [--settle-frac 0.5] [--csv out.csv]
"""

import argparse
import math
import sys


def parse_common_series(path):
    """products.txt -> {tow: median-over-sats C value} (per emitted epoch)."""
    series = {}
    tow = None
    vals = []

    def commit():
        if tow is not None and vals:
            s = sorted(vals)
            n = len(s)
            med = s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])
            series[tow] = med

    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("E,"):
                commit()
                parts = line.strip().split(",")
                try:
                    tow = float(parts[2])
                except (IndexError, ValueError):
                    tow = None
                vals = []
            elif line.startswith("C,") and tow is not None:
                parts = line.strip().split(",")
                # positional stop at the first key=value token (SPEC §8 trap 4);
                # clock_c0_m is field 2, always before any '=' token.
                if len(parts) >= 3 and "=" not in parts[2]:
                    try:
                        v = float(parts[2])
                    except ValueError:
                        continue
                    if math.isfinite(v):
                        vals.append(v)
    commit()
    return series


def settled(series, settle_frac):
    if not series:
        return {}
    tows = sorted(series)
    t0, t1 = tows[0], tows[-1]
    cut = t0 + settle_frac * (t1 - t0)
    return {t: series[t] for t in tows if t >= cut}


def stats(xs):
    n = len(xs)
    if n < 2:
        return 0.0, 0.0
    m = sum(xs) / n
    var = sum((x - m) ** 2 for x in xs) / n
    return m, math.sqrt(var)


def pearson(xs, ys):
    n = len(xs)
    if n < 3:
        return float("nan")
    mx = sum(xs) / n
    my = sum(ys) / n
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    if sxx <= 0.0 or syy <= 0.0:
        return float("nan")
    return sxy / math.sqrt(sxx * syy)


def describe(name, series, settle_frac):
    st = settled(series, settle_frac)
    xs = [st[t] for t in sorted(st)]
    m, sd = stats(xs)
    rng = (max(xs) - min(xs)) if xs else 0.0
    print(f"  {name}: {len(series)} epochs ({len(st)} settled)  "
          f"mean={m:+.3f} m  std={sd:.3f} m  range={rng:.3f} m")
    return st


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("products_a")
    ap.add_argument("products_b", nargs="?", default=None)
    ap.add_argument("--settle-frac", type=float, default=0.5)
    ap.add_argument("--csv", default=None,
                    help="write the joined settled series (tow,common_a[,common_b])")
    args = ap.parse_args()

    print("-- fleet-common C series (median over sats; quiet fixture => pure error) --")
    sa = describe("A", parse_common_series(args.products_a), args.settle_frac)
    if args.products_b is None:
        if args.csv:
            with open(args.csv, "w") as f:
                f.write("tow,common_a\n")
                for t in sorted(sa):
                    f.write(f"{t:.3f},{sa[t]:.6f}\n")
        return 0

    sb = describe("B", parse_common_series(args.products_b), args.settle_frac)
    common_tows = sorted(set(sa) & set(sb))
    if len(common_tows) < 10:
        print(f"FATAL: only {len(common_tows)} joint settled epochs")
        return 1
    xa = [sa[t] for t in common_tows]
    xb = [sb[t] for t in common_tows]
    ma, sda = stats(xa)
    mb, sdb = stats(xb)
    da = [x - ma for x in xa]
    db = [x - mb for x in xb]
    diff = [a - b for a, b in zip(da, db)]
    _, sdd = stats(diff)
    r = pearson(da, db)
    fda = [da[i + 1] - da[i] for i in range(len(da) - 1)]
    fdb = [db[i + 1] - db[i] for i in range(len(db) - 1)]
    rfd = pearson(fda, fdb)

    print(f"-- joint settled epochs: {len(common_tows)} --")
    print(f"  std(A)={sda:.3f} m   std(B)={sdb:.3f} m   std(A-B)={sdd:.3f} m   "
          f"sqrt2*geomean(std)={math.sqrt(2.0)*math.sqrt(sda*sdb):.3f} m")
    print(f"  corr(A,B) demeaned      = {r:+.3f}")
    print(f"  corr(dA,dB) first-diff  = {rfd:+.3f}")
    verdict = ("P1: DATUM-STATION-OWNED (series decorrelate)" if abs(r) < 0.35 else
               "P2: SHARED-STATE (series move together)" if r > 0.75 else
               "MIXED — decompose further before concluding")
    print(f"  verdict: {verdict}")

    if args.csv:
        with open(args.csv, "w") as f:
            f.write("tow,common_a,common_b\n")
            for t in common_tows:
                f.write(f"{t:.3f},{sa[t]:.6f},{sb[t]:.6f}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
