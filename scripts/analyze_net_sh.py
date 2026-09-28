#!/usr/bin/env python3
"""Analyze a 305-station global SH-VTEC network run for field divergence.

Reads logs/network_vtec_sh.csv (week,tow,n,m,trig,value_tecu,std_tecu) and
logs/network_trace.csv (...,declared_dim,gate_ok,solved,rms_clock_m,max_clock_m,...)
and reports the divergence diagnostics from project_global_sh_iono.md:

  - a00 (mean VTEC) trajectory                 -> should be O(10-40) TECU, stable
  - max |coeff| and which (n,m,trig)           -> blow-up => decomposition degenerate
  - per-degree RMS power spectrum              -> Kaula-like decay vs flat/growing
  - synthesized global VTEC min/max + subsolar -> physical field (0..~80 TECU)
  - clock-Delta rms/max per epoch              -> cold-start clock blow-up
  - gate_ok / solved / declared_dim            -> estimability gate health

Usage: python analyze_net_sh.py <logs_dir> [label]
"""
import csv
import math
import sys
from collections import defaultdict
from pathlib import Path


def norm_table(N):
    nrm = [[0.0] * (n + 1) for n in range(N + 1)]
    for n in range(N + 1):
        for m in range(n + 1):
            ratio = 1.0
            for k in range(n - m + 1, n + m + 1):
                ratio /= k
            nrm[n][m] = math.sqrt((1.0 if m == 0 else 2.0) * (2 * n + 1) * ratio)
    return nrm


def legendre(N, x, nrm):
    s = math.sqrt(max(0.0, 1.0 - x * x))
    P = [[0.0] * (n + 1) for n in range(N + 1)]
    P[0][0] = 1.0
    for m in range(1, N + 1):
        P[m][m] = (2.0 * m - 1.0) * s * P[m - 1][m - 1]
    for m in range(0, N + 1):
        if m + 1 <= N:
            P[m + 1][m] = (2.0 * m + 1.0) * x * P[m][m]
        for n in range(m + 2, N + 1):
            P[n][m] = ((2.0 * n - 1.0) * x * P[n - 1][m]
                       - (n + m - 1.0) * P[n - 2][m]) / (n - m)
    for n in range(N + 1):
        for m in range(n + 1):
            P[n][m] *= nrm[n][m]
    return P


def synth_vtec(coeffs, N, nrm, lat, lon_sf):
    """coeffs: dict[(n,m,trig)] -> value; returns VTEC at (lat,lon_sf) in rad."""
    P = legendre(N, math.sin(lat), nrm)
    v = 0.0
    for (n, m, trig), c in coeffs.items():
        pb = P[n][m]
        v += c * (pb * math.cos(m * lon_sf) if trig == 'C' else pb * math.sin(m * lon_sf))
    return v


def main():
    logs = Path(sys.argv[1])
    label = sys.argv[2] if len(sys.argv) > 2 else logs.name
    sh_path = logs / "network_vtec_sh.csv"
    tr_path = logs / "network_trace.csv"

    # ---- SH coefficients per epoch -------------------------------------------
    by_epoch = defaultdict(dict)   # tow -> {(n,m,trig): value}
    std_by_epoch = defaultdict(dict)
    Nmax = 0
    if sh_path.exists():
        with sh_path.open() as f:
            for row in csv.DictReader(f):
                # Skip torn rows.  A trace file is buffered, so the final line of a run
                # still in flight, or one that was killed or WER-zombied (a realistic
                # end state for a multi-hour run), can be a partial write with missing
                # trailing fields.  csv.DictReader fills a missing field with None, and
                # float(None) raises TypeError rather than ValueError, so the except
                # clauses below must catch both.
                if row.get("std_tecu") is None or row.get("tow") is None:
                    continue
                try:
                    tow = float(row["tow"])
                    n, m = int(row["n"]), int(row["m"])
                except (ValueError, TypeError):
                    continue
                key = (n, m, row["trig"])
                try:
                    val = float(row["value_tecu"])
                    sd = float(row["std_tecu"])
                except (ValueError, TypeError):
                    val, sd = float("nan"), float("nan")
                by_epoch[tow][key] = val
                std_by_epoch[tow][key] = sd
                Nmax = max(Nmax, n)
    epochs = sorted(by_epoch)
    nrm = norm_table(Nmax) if Nmax else None

    print(f"\n================ {label} ================")
    print(f"SH trace: {sh_path}")
    print(f"  degree N = {Nmax}, coeffs/epoch = {(Nmax+1)**2 if Nmax else 0}, "
          f"epochs logged = {len(epochs)}")
    if not epochs:
        print("  (no SH rows)")
    else:
        # subsolar (lat=0, lon_sf=0) and a few sample lats at noon
        hdr = (f"  {'tow':>8} {'a00':>9} {'max|c|':>10} {'argmax':>10} "
               f"{'L2coef':>9} {'VTECsub':>8} {'VTECmin':>9} {'VTECmax':>9} {'nan':>4}")
        print(hdr)
        for tow in epochs:
            c = by_epoch[tow]
            a00 = c.get((0, 0, 'C'), float('nan'))
            finite = {k: v for k, v in c.items() if math.isfinite(v)}
            n_nan = len(c) - len(finite)
            if finite:
                amax_k = max(finite, key=lambda k: abs(finite[k]))
                amax = finite[amax_k]
                l2 = math.sqrt(sum(v * v for v in finite.values()))
            else:
                amax_k, amax, l2 = (0, 0, 'C'), float('nan'), float('nan')
            # global VTEC range over a coarse sun-fixed grid
            vsub = synth_vtec(finite, Nmax, nrm, 0.0, 0.0) if finite else float('nan')
            vmin, vmax = float('inf'), float('-inf')
            if finite:
                for ilat in range(-80, 81, 20):
                    for ilon in range(-180, 181, 30):
                        vv = synth_vtec(finite, Nmax, nrm,
                                        math.radians(ilat), math.radians(ilon))
                        vmin = min(vmin, vv); vmax = max(vmax, vv)
            print(f"  {tow:8.0f} {a00:9.2f} {abs(amax):10.2f} "
                  f"{amax_k[0]:>2}/{amax_k[1]:<2}{amax_k[2]:<2} {l2:9.2f} "
                  f"{vsub:8.1f} {vmin:9.1f} {vmax:9.1f} {n_nan:4d}")

        # per-degree RMS power spectrum at the last epoch
        last = epochs[-1]
        deg_pow = defaultdict(list)
        for (n, m, trig), v in by_epoch[last].items():
            if math.isfinite(v):
                deg_pow[n].append(v)
        print(f"\n  per-degree RMS power @ last epoch tow={last:.0f} "
              f"(sqrt(mean c^2) over the 2n+1 coeffs):")
        for n in range(Nmax + 1):
            vals = deg_pow.get(n, [])
            rms = math.sqrt(sum(v * v for v in vals) / len(vals)) if vals else 0.0
            bar = '#' * min(60, int(rms))
            print(f"    n={n:2d}: rms={rms:8.2f} TECU  {bar}")

    # ---- clock / gate health -------------------------------------------------
    print(f"\nNetwork trace: {tr_path}")
    if tr_path.exists():
        with tr_path.open() as f:
            rows = list(csv.DictReader(f))
        print(f"  {'tow':>8} {'n_pres':>6} {'dim':>4} {'gate':>4} {'solv':>4} "
              f"{'rms_clk_m':>11} {'max_clk_m':>11}")
        for r in rows:
            try:
                print(f"  {float(r['tow']):8.0f} {r['n_present']:>6} "
                      f"{r['declared_dim']:>4} {r['gate_ok']:>4} {r['solved']:>4} "
                      f"{float(r['rms_clock_m']):11.2f} {float(r['max_clock_m']):11.2f}")
            except (KeyError, ValueError):
                print("  " + ",".join(r.values()))
    else:
        print("  (missing)")


if __name__ == "__main__":
    main()
