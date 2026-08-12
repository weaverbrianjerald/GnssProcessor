#!/usr/bin/env python3
"""FIELD — cross-station iono-field agreement from raw GF (SPEC §9.10).

Two stations tracking the SAME satellite sample nearby columns of the same
ionosphere (IPP separation ~ their baseline for MEO geometry).  If the field
the network co-estimates is coherent at those scales, their vertical-mapped
GF RATES must agree; disagreement measures the station-local content (phase
multipath, mapping error at low elevation) that the iono states — and through
the level exchanges, the clock/CB commons — are forced to absorb.

Method (raw observables only — no estimator output, so this is an INPUT
property, not a fit property): per (station,sat), LLI-clean dual rows from
network_slip_raw.csv; 300 s GF increments mapped to vertical by the thin-shell
obliquity 1/M(el), h = 450 km; per station-pair-satellite series with >= 1 h of
joint epochs: Pearson corr + relative rms of the difference.  Aggregated by
baseline bin x elevation class (both high vs one-low/one-high — the user's
"one high elevation and one low elevation ~1000 km apart" case).

Usage:
  python scripts/field_iono_pair_agreement.py <run_dir> <stations_config.json>
         [--settle-hours 2] [--lag-epochs 10] [--min-joint 120]
"""

import argparse
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from field_wire_datum_probe import qtow, median  # noqa: E402

RE_KM = 6371.0
H_KM = 450.0


def obliquity(el_deg):
    c = RE_KM * math.cos(math.radians(el_deg)) / (RE_KM + H_KM)
    return 1.0 / math.sqrt(max(1e-6, 1.0 - c * c))


def parse_gf(path):
    """-> {station: {sat: {tow: (gf_m, el)}}} (dual, LLI-clean)."""
    out = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        header = f.readline().strip().split(",")
        idx = {n: i for i, n in enumerate(header)}
        it, ist, isa = idx["tow"], idx["station"], idx["sat"]
        ig, ie, i1, i2, is2 = (idx["gf_m"], idx["elev_deg"], idx["lli1"],
                               idx["lli2"], idx["sig2"])
        for line in f:
            p = line.rstrip("\n").split(",")
            if len(p) <= max(ig, ie) or not p[is2] or p[i1] != "0" or p[i2] != "0":
                continue
            try:
                tow = qtow(float(p[it]))
                gf = float(p[ig])
                el = float(p[ie])
            except ValueError:
                continue
            if math.isfinite(gf) and math.isfinite(el):
                out.setdefault(p[ist], {}).setdefault(p[isa], {})[tow] = (gf, el)
    return out


def vrate_series(sat_map, lag_s, cut):
    """{tow:(gf,el)} -> {tow: (dV, el)}: lagged GF increment mapped to vertical."""
    out = {}
    for t, (gf, el) in sat_map.items():
        if t < cut:
            continue
        prev = sat_map.get(t - lag_s)
        if prev is None:
            continue
        gf0, el0 = prev
        out[t] = ((gf - gf0) / obliquity(0.5 * (el + el0)), 0.5 * (el + el0))
    return out


def pearson(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxy = sum((a - mx) * (b - my) for a, b in zip(xs, ys))
    sxx = sum((a - mx) ** 2 for a in xs)
    syy = sum((b - my) ** 2 for b in ys)
    if sxx <= 0 or syy <= 0:
        return float("nan")
    return sxy / math.sqrt(sxx * syy)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir")
    ap.add_argument("stations_config")
    ap.add_argument("--settle-hours", type=float, default=2.0)
    ap.add_argument("--lag-epochs", type=int, default=10)
    ap.add_argument("--min-joint", type=int, default=120)
    args = ap.parse_args()

    cfg = json.load(open(args.stations_config))
    ecef = {s["name"]: s["position_ecef"] for s in cfg["network"]["stations"]
            if s.get("position_ecef")}

    gf = parse_gf(os.path.join(args.run_dir, "logs", "network_slip_raw.csv"))
    tows = [t for sta in gf.values() for sm in sta.values() for t in sm]
    t0 = min(tows)
    cut = t0 + args.settle_hours * 3600.0
    lag_s = args.lag_epochs * 30.0

    stations = sorted(set(gf) & set(ecef))
    pairs = []
    for i, a in enumerate(stations):
        for b in stations[i + 1:]:
            d = math.dist(ecef[a], ecef[b]) / 1000.0
            if 300.0 <= d <= 2000.0:
                pairs.append((a, b, d))

    # bins: (baseline_lo, baseline_hi) x elevation class
    bins = {}
    n_series = 0
    for a, b, dkm in pairs:
        common = set(gf[a]) & set(gf[b])
        for sat in common:
            va = vrate_series(gf[a][sat], lag_s, cut)
            vb = vrate_series(gf[b][sat], lag_s, cut)
            joint = sorted(set(va) & set(vb))
            if len(joint) < args.min_joint:
                continue
            xa = [va[t][0] for t in joint]
            xb = [vb[t][0] for t in joint]
            ea = median([va[t][1] for t in joint])
            eb = median([vb[t][1] for t in joint])
            r = pearson(xa, xb)
            if math.isnan(r):
                continue
            dd = [x - y for x, y in zip(xa, xb)]
            rms_d = math.sqrt(sum(x * x for x in dd) / len(dd))
            rms_p = math.sqrt(sum(x * x for x in xa + xb) / (2 * len(xa)))
            rel = rms_d / rms_p if rms_p > 0 else float("nan")
            lo, hi = min(ea, eb), max(ea, eb)
            elcls = ("low-high" if lo < 25.0 <= 40.0 <= hi else
                     "both-high" if lo >= 40.0 else "mid")
            bb = "300-700km" if dkm < 700.0 else ("700-1200km" if dkm < 1200.0
                                                  else "1200-2000km")
            bins.setdefault((bb, elcls), []).append((r, rel))
            n_series += 1

    print(f"-- iono-field pair agreement: {len(pairs)} pairs in [300,2000] km, "
          f"{n_series} (pair,sat) series, lag {lag_s:.0f} s, settled "
          f"{args.settle_hours:.0f}h+ --")
    print(f"   {'baseline':<13}{'elev class':<11}{'series':>7}"
          f"{'med corr':>10}{'med rel-rms':>13}")
    for key in sorted(bins):
        vals = bins[key]
        rs = sorted(v[0] for v in vals)
        rels = sorted(v[1] for v in vals)
        print(f"   {key[0]:<13}{key[1]:<11}{len(vals):>7}"
              f"{median(rs):>10.3f}{median(rels):>13.3f}")
    print("   (rel-rms 0 = identical fields, sqrt(2) = uncorrelated; the "
          "low-high class is the thin-shell mapping's weak regime)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
