#!/usr/bin/env python3
"""Per-LOS ionosphere interaction grade — the SPEC_t3_null_derivation §7.8 decomposition,
promoted from the scratch join that produced the 0.764 m number (QUEUE item 8).

Joins the producer's network_ionosphere.csv (column `iono_total_m` = x_lin + x_r —
the ONLY gradable column; `iono_m` is the seed-less correction, ~11% of the quantity)
against each station's truth_atmo.csv `I_L1_m` (already SLANT — never multiply by
obliquity), then removes the datum-class lumps per standing invariant #5:

    e(sta, sat, t) = mu + a(sta) + b(sat) + r(sta, sat, t)

a(sta) is rx-DCB-class, b(sat) is sat-DCB-class, and only the INTERACTION r is a
defect channel.  Fitted by alternating means over the settled window (default:
tow >= tow0 + 1800 s — the level channel's ignition budget; a shorter window grades
convergence, not the null).

Traps defended (each produced a wrong answer once):
  * join on integer-quantized tow — the trace carries %.3f, product rows carry float dust
  * settled window only; report the window in the same sentence as the number
  * blank/absent means did-not-run, never 0
  * per-station truth lives in <fixture>/<STA>/truth_atmo.csv (files, not a station column)

Usage:
  python scripts/simnet_iono_interaction.py <run_dir> <fixture_dir> [--settle-s 1800] [--json out.json]

<run_dir> needs logs/network_ionosphere.csv (arm network.trace_ionosphere).
"""
import argparse
import csv
import json
import math
import os
import sys
from collections import defaultdict

# GPS L1/L2 GF factor: P1 - P2 = I1*(1 - mu) + (d1 - d2), mu = (f1/f2)^2.
# d_GF = (d1 - d2)/(1 - mu) is the receiver bias the L1 iono absorbs.  The SIGN
# is measured by the reported slope, never asserted (two conventions bit once).
F1, F2 = 1575.42e6, 1227.60e6
MU12 = (F1 / F2) ** 2


def load_rx_gf_truth(fixture_dir, c1="C1C", c2="C2W"):
    """Per-station receiver GF bias (L1 metres) from truth_rx_bias.csv, demeaned
    over the fleet.  Contract §B.4's d-bar_r,GF is the fleet-mean functional; the
    per-station DEVIATION from it is what the rx-DCB states should hold, so the
    fitted iono lump a(sta) tracking this deviation (report the measured slope)
    grades the realized Table-8 split."""
    gf = {}
    for name in sorted(os.listdir(fixture_dir)):
        p = os.path.join(fixture_dir, name, "truth_rx_bias.csv")
        if not os.path.isfile(p):
            continue
        d = {}
        with open(p, newline="") as f:
            for row in csv.DictReader(f):
                if row.get("kind") == "code":
                    d[row["code"]] = float(row["value_m"])
        if c1 in d and c2 in d:
            gf[name] = (d[c1] - d[c2]) / (1.0 - MU12)
    if not gf:
        return {}
    mean = sum(gf.values()) / len(gf)
    return {s: v - mean for s, v in gf.items()}


def load_trace(path):
    rows = []
    with open(path, newline="") as f:
        r = csv.DictReader(f)
        if "iono_total_m" not in (r.fieldnames or []):
            sys.exit(f"ERROR: {path} has no iono_total_m column — pre-fix trace or wrong file "
                     f"(columns: {r.fieldnames}). Grade iono_total_m, never iono_m.")
        for row in r:
            try:
                tow = int(round(float(row["tow"])))
                v = float(row["iono_total_m"])
            except (ValueError, KeyError):
                continue
            rows.append((tow, row["station"], row["sat"], v))
    return rows


def load_truth(fixture_dir):
    truth = {}
    stations = 0
    for name in sorted(os.listdir(fixture_dir)):
        p = os.path.join(fixture_dir, name, "truth_atmo.csv")
        if not os.path.isfile(p):
            continue
        stations += 1
        with open(p, newline="") as f:
            r = csv.DictReader(f)
            if "I_L1_m" not in (r.fieldnames or []):
                sys.exit(f"ERROR: {p} has no I_L1_m column ({r.fieldnames})")
            for row in r:
                try:
                    tow = int(round(float(row["tow_s"])))
                    truth[(tow, name, row["satellite"])] = float(row["I_L1_m"])
                except (ValueError, KeyError):
                    continue
    if stations == 0:
        sys.exit(f"ERROR: no <station>/truth_atmo.csv under {fixture_dir}")
    return truth


def fit_two_way(err):
    """err: dict (sta,sat)->list of e.  Returns mu, a[sta], b[sat] by alternating means."""
    mu = sum(v for vs in err.values() for v in vs) / sum(len(vs) for vs in err.values())
    a = defaultdict(float)
    b = defaultdict(float)
    for _ in range(25):
        # a-step
        acc = defaultdict(lambda: [0.0, 0])
        for (sta, sat), vs in err.items():
            for v in vs:
                acc[sta][0] += v - mu - b[sat]
                acc[sta][1] += 1
        for sta, (s, n) in acc.items():
            a[sta] = s / n
        # b-step
        acc = defaultdict(lambda: [0.0, 0])
        for (sta, sat), vs in err.items():
            for v in vs:
                acc[sat][0] += v - mu - a[sta]
                acc[sat][1] += 1
        for sat, (s, n) in acc.items():
            b[sat] = s / n
        # re-center (a and b sum-to-zero; push means into mu)
        ma = sum(a.values()) / len(a)
        mb = sum(b.values()) / len(b)
        for sta in a:
            a[sta] -= ma
        for sat in b:
            b[sat] -= mb
        mu += ma + mb
    return mu, a, b


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir")
    ap.add_argument("fixture_dir")
    ap.add_argument("--settle-s", type=float, default=1800.0)
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    trace_path = os.path.join(args.run_dir, "logs", "network_ionosphere.csv")
    if not os.path.isfile(trace_path):
        trace_path = os.path.join(args.run_dir, "network_ionosphere.csv")
    if not os.path.isfile(trace_path):
        sys.exit(f"ERROR: no network_ionosphere.csv under {args.run_dir} (arm network.trace_ionosphere)")

    trace = load_trace(trace_path)
    if not trace:
        sys.exit("ERROR: trace is empty — did-not-run, not zero")
    truth = load_truth(args.fixture_dir)

    tow0 = min(t for t, _, _, _ in trace)
    tow_hi = max(t for t, _, _, _ in trace)
    settle = tow0 + args.settle_s

    err = defaultdict(list)
    n_join = n_miss = 0
    for tow, sta, sat, v in trace:
        if tow < settle:
            continue
        key = (tow, sta, sat)
        if key not in truth:
            n_miss += 1
            continue
        err[(sta, sat)].append(v - truth[key])
        n_join += 1

    if n_join == 0:
        sys.exit(f"ERROR: zero settled joins (settle tow>={settle:.0f}, trace {tow0}-{tow_hi}, "
                 f"{n_miss} unmatched) — window or fixture mismatch")

    mu, a, b = fit_two_way(err)

    r_all = []
    per_sta = defaultdict(list)
    for (sta, sat), vs in err.items():
        for v in vs:
            r = v - mu - a[sta] - b[sat]
            r_all.append(r)
            per_sta[sta].append(r)

    rms = math.sqrt(sum(x * x for x in r_all) / len(r_all))
    sta_rms = {s: math.sqrt(sum(x * x for x in v) / len(v)) for s, v in per_sta.items()}
    lo_sta = min(sta_rms, key=sta_rms.get)
    hi_sta = max(sta_rms, key=sta_rms.get)

    out = {
        "run_dir": args.run_dir,
        "settled_window_tow": [settle, tow_hi],
        "n_settled_triples": n_join,
        "n_unmatched": n_miss,
        "mu_m": round(mu, 4),
        "a_sta_range_m": [round(min(a.values()), 4), round(max(a.values()), 4)],
        "b_sat_range_m": [round(min(b.values()), 4), round(max(b.values()), 4)],
        "interaction_rms_m": round(rms, 4),
        "interaction_rms_by_station_m": {s: round(v, 4) for s, v in sorted(sta_rms.items())},
        "station_min": [lo_sta, round(sta_rms[lo_sta], 4)],
        "station_max": [hi_sta, round(sta_rms[hi_sta], 4)],
    }
    print(f"settled triples {n_join} (tow {settle:.0f}..{tow_hi}, {n_miss} unmatched)")
    print(f"mu {mu:+.3f} m | a(sta) [{out['a_sta_range_m'][0]:+.3f}, {out['a_sta_range_m'][1]:+.3f}] "
          f"| b(sat) [{out['b_sat_range_m'][0]:+.3f}, {out['b_sat_range_m'][1]:+.3f}]")
    print(f"INTERACTION rms {rms:.4f} m  (by station: {sta_rms[lo_sta]:.3f} [{lo_sta}] "
          f".. {sta_rms[hi_sta]:.3f} [{hi_sta}])")

    # Table-8 split check (contract §B.4 (b), REPORT form): does the fitted iono
    # station lump a(sta) track the demeaned truth rx GF bias?  A clean Table-8
    # realization keeps the per-station GF deviation in the rx-DCB states, so
    # slope ~0 with small a(sta) is CLEAN, |slope| ~1 with a(sta) ~ the GF truth
    # means the iono absorbed the per-station split; anything else is the level
    # exchange wandering.  The slope is MEASURED, never asserted (sign rule).
    gf = load_rx_gf_truth(args.fixture_dir)
    common = sorted(set(gf) & set(a))
    if len(common) >= 3:
        ax = [gf[s] for s in common]
        ay = [a[s] for s in common]
        mx = sum(ax) / len(ax)
        my = sum(ay) / len(ay)
        sxx = sum((x - mx) ** 2 for x in ax)
        sxy = sum((x - mx) * (y - my) for x, y in zip(ax, ay))
        slope = sxy / sxx if sxx > 0 else float("nan")
        sy = math.sqrt(sum((y - my) ** 2 for y in ay) / len(ay))
        sx = math.sqrt(sxx / len(ax))
        corr = (sxy / len(ax)) / (sx * sy) if sx > 0 and sy > 0 else float("nan")
        out["table8_split"] = {
            "n_stations": len(common),
            "slope_a_vs_rxgf": round(slope, 3),
            "corr": round(corr, 3),
            "rx_gf_truth_demeaned_range_m": [round(min(ax), 3), round(max(ax), 3)],
        }
        print(f"Table-8 split: a(sta) vs demeaned truth rx-GF over {len(common)} stations — "
              f"slope {slope:+.3f}, corr {corr:+.3f} "
              f"(GF truth range [{min(ax):+.3f}, {max(ax):+.3f}] m)")
    if args.json:
        with open(args.json, "w") as f:
            json.dump(out, f, indent=1)
        print(f"wrote {args.json}")


if __name__ == "__main__":
    main()
