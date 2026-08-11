"""Card Q3 — floor-vs-walk split of the phase-measured (TDCP) satellite clock
correction, from the FC-1 shadow trace (network_fast_clock.csv).

The per-tick TDCP change telescopes: the accumulated change over lag L is the
direct endpoint phase difference (interior measurement noise cancels), so a
structure function over lags separates a MEASUREMENT FLOOR (flat in L) from a
TRUE RANDOM WALK (grows as sqrt(L)).  The walk part is the physics the anchor's
q should model; the floor is what every per-sample "physical change" statistic
(FC-1's 1.7-2 cm hour table, the 0.30 cm TDCP column) actually measured.

First run (2026-08-10, runs/cta3_ctl, 32 sats, 3 h, common-mode removed):
  floor SF(30-120 s) = 0.27 cm flat; walk fit 300->2400 s:
  q_phys ~ 2.9e-8 m^2/s  ->  0.9 mm / 30 s  (fractional ~1e-13)
i.e. the clock corrections beyond the COD a-priori are quasi-static at the
mm-per-epoch level -- SPEC_clock_temporal_anchor SS7.

Usage: python scripts/q3_clock_structure_function.py <run_dir_or_csv>
       [--lags 30,60,120,300,600,1200,2400] [--fit-from 300] [--min-pairs 200]
"""
import argparse
import collections
import csv
import math
import os
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run", help="run dir containing network_fast_clock.csv, or the csv path")
    ap.add_argument("--lags", default="30,60,120,300,600,1200,2400")
    ap.add_argument("--fit-from", type=float, default=300.0,
                    help="lower lag (s) of the walk fit; below it the floor dominates")
    ap.add_argument("--min-pairs", type=int, default=200)
    a = ap.parse_args()
    path = a.run
    if os.path.isdir(path):
        cand = [os.path.join(path, "network_fast_clock.csv"),
                os.path.join(path, "logs", "network_fast_clock.csv")]
        path = next((c for c in cand if os.path.exists(c)), cand[0])

    series = collections.defaultdict(dict)   # sat -> {tow: cumulative TDCP (m)}
    cum = collections.defaultdict(float)
    with open(path) as f:
        r = csv.reader(f)
        hdr = next(r)
        ix = {c: i for i, c in enumerate(hdr)}
        for row in r:
            sat = row[ix["sat"]]
            if not sat:
                continue          # T summary rows
            dc = row[ix["dc_m"]]
            if not dc:
                continue
            cum[sat] += float(dc)
            series[sat][round(float(row[ix["tow"]]))] = cum[sat]

    # Common-mode removal per epoch (median over sats present): the ensemble
    # gauge is CLK-R's channel, not this card's.
    tows = sorted({t for s in series.values() for t in s})
    med = {}
    for t in tows:
        v = sorted(s[t] for s in series.values() if t in s)
        if len(v) >= 8:
            med[t] = v[len(v) // 2]
    for s in series.values():
        for t in list(s):
            if t in med:
                s[t] -= med[t]
            else:
                del s[t]

    print(f"{path}: {len(series)} sats, {len(tows)} epochs")
    print(" lag L[s]   SF robust [cm]   SF/sqrt(L) [cm/sqrt(s)]")
    res = {}
    for L in (int(x) for x in a.lags.split(",")):
        d = []
        for s in series.values():
            for t, v in s.items():
                if t + L in s:
                    d.append(abs(s[t + L] - v))
        if len(d) < a.min_pairs:
            print(f" {L:6d}    (only {len(d)} pairs -- skipped)")
            continue
        d.sort()
        sf = 1.4826 * d[len(d) // 2]
        res[L] = sf
        print(f" {L:6d}    {sf*100:8.3f}         {sf*100/math.sqrt(L):.4f}")

    lags = sorted(res)
    fit = [L for L in lags if L >= a.fit_from]
    if len(fit) >= 2 and lags:
        L0, L1 = fit[0], fit[-1]
        q = (res[L1] ** 2 - res[L0] ** 2) / (L1 - L0)
        floor = res[lags[0]]
        print(f"\nfloor SF({lags[0]} s) = {floor*100:.2f} cm (flat part -- NOT the walk)")
        print(f"walk fit {L0}->{L1} s:  q_phys = {q:.2e} m^2/s"
              f"  ->  sqrt(q*30 s) = {math.sqrt(max(q, 0.0)*30)*1000:.2f} mm / 30 s")
        return 0
    print("insufficient lags for the walk fit", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
