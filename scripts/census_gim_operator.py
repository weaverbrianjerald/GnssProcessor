#!/usr/bin/env python3
"""LV-6 gate G2: censusOperator{GIM}, the sampling-matched GIM reference.

`docs/SPEC_level_authority_internal.md` §4 G2 / §5.2.  The gate is

    settled  L_cs(t) - censusOperator{GIM}(t)  stable within +/- 2 TECU
    and the offset in the raw-realization window  -10 .. -20 vertical TECU
    (outside -25 .. +5 falsifies the C4 confound model: stop rule, no fold card)

Why the reference is not the planetary mean.  The §5.2 adversary pass rejected
the naive "fleet median vs GIM global mean" grade: the network sees roughly one
hemisphere's footprint, so that difference carries the roster's geography
(several TECU, diurnal) on top of the level.  The reference is therefore the
GIM pushed through the same operator the census applies to its own observable:
same stations, same pierce points, same weights, same two-tier reduction.  This
script is that operator.  It also prints the planetary mean, to show how large
the sampling term it removes is.

Sampling comes from the run, not from a re-derivation.  With
`network.level_authority_los_dump` armed the census publishes every LOS it
reduced (station, satellite, IPP lat/lon, thin-shell mapping, information
weight), and the GIM is evaluated at those pierce points with those weights.  A
re-derived "all satellites above the mask" set would be a different sample (the
census keeps only mature CS-GF states) and would reintroduce, on a smaller
scale, the sampling error §5.2 rejected.  Without the dump this script refuses
to grade rather than approximate.

Units.  The census's `v_tecu` is already vertical (it divides the slant value
by the mapping), and an IONEX map is vertical TEC, so the two sides are
directly comparable and no mapping is applied to the reference.  The mapping
column is carried in the dump for provenance and for the self-test.

Usage
  python scripts/census_gim_operator.py <run_dir> [<run_dir> ...]
      [--ionex PATH] [--settled-from 7200] [--min-los 4]
  python scripts/census_gim_operator.py --selftest

Exit status: 0 = G2 pass, 1 = G2 fail, 2 = stop rule (offset outside -25 .. +5);
over several run dirs, the largest.

Caveats
  * GIM is a product with a few-TECU error of its own and relies on data-free
    ocean/southern interpolation; this is a level comparison, not truth.
    G1 (sim) is the only truth-graded venue.  GIM stays diagnostics-only
    (standing directive).
"""
import argparse
import csv
import glob
import math
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gim_global_mean_series import load_ionex, global_mean  # noqa: E402

EARTH_ROT_DEG_PER_SEC = 360.0 / 86400.0


def sample_map(grid, lat, lon):
    """Bilinear VTEC (TECU) from one IONEX map at (lat, lon) in degrees."""
    lats = sorted(grid)
    if not lats:
        return float("nan")
    lat = min(max(lat, lats[0]), lats[-1])
    i = 0
    for k in range(len(lats) - 1):
        if lats[k] <= lat <= lats[k + 1] or lats[k] >= lat >= lats[k + 1]:
            i = k
            break
    else:
        i = len(lats) - 2
    la0, la1 = lats[i], lats[i + 1]
    f = 0.0 if la1 == la0 else (lat - la0) / (la1 - la0)
    lon = (lon + 180.0) % 360.0 - 180.0
    out = 0.0
    for la, w in ((la0, 1.0 - f), (la1, f)):
        if w == 0.0:
            continue
        lon1, dlon, row = grid[la]
        idx = (lon - lon1) / dlon
        j = int(math.floor(idx))
        j = min(max(j, 0), len(row) - 2)
        fr = min(max(idx - j, 0.0), 1.0)
        out += w * ((1.0 - fr) * row[j] + fr * row[j + 1])
    return out


def gim_at(maps, sod, lat, lon):
    """Sun-fixed interpolated GIM vertical TECU, matching the C++ reader.

    GlobalIonosphereMap::interpolateVtec rotates the query longitude by the
    Earth's rotation between each bracketing map epoch and the query time, then
    blends linearly in time.  Skipping that shift costs ~0.2 TECU.
    """
    if not maps:
        return float("nan")
    i1 = 0
    while i1 + 1 < len(maps) and maps[i1 + 1][0] <= sod:
        i1 += 1
    i2 = min(i1 + 1, len(maps) - 1)
    t1, g1 = maps[i1]
    t2, g2 = maps[i2]
    v1 = sample_map(g1, lat, lon + (sod - t1) * EARTH_ROT_DEG_PER_SEC)
    v2 = sample_map(g2, lat, lon + (sod - t2) * EARTH_ROT_DEG_PER_SEC)
    w = 0.0 if t2 <= t1 else min(max((sod - t1) / (t2 - t1), 0.0), 1.0)
    return (1.0 - w) * v1 + w * v2


def level_authority_reduce(sta_rows, min_los):
    """Transliteration of levelAuthorityReduce in include/filter/NetworkRobustStats.hpp.

    Tier 1: per-station information-weighted mean (stations with fewer than
    min_los rows or a non-positive weight sum are skipped).  Tier 2: fleet
    median; disp is 1.4826 * MAD of the station means.  Returns
    (level, disp, n_sta, n_los), or None when no station qualifies, never 0.0
    ("unavailable is not 0").
    """
    means, n_los = [], 0
    for rows in sta_rows.values():
        if len(rows) < min_los:
            continue
        sw = sum(w for _v, w in rows)
        if not sw > 0.0:
            continue
        means.append(sum(w * v for v, w in rows) / sw)
        n_los += len(rows)
    if not means:
        return None
    med = statistics.median(means)
    mad = statistics.median([abs(m - med) for m in means])
    return med, 1.4826 * mad, len(means), n_los


def tow_to_sod(tow):
    """GPS TOW -> seconds of day, matching the IONEX day the run sits in."""
    return tow % 86400.0


def operator_series(los_csv, maps, min_los):
    per = {}
    for r in csv.DictReader(open(los_csv, newline="")):
        tow = round(float(r["tow"]), 3)
        per.setdefault(tow, {}).setdefault(r["station"], []).append(
            (gim_at(maps, tow_to_sod(tow), float(r["ipp_lat_deg"]),
                    float(r["ipp_lon_deg"])), float(r["weight"])))
    out = {}
    for tow, sta in per.items():
        red = level_authority_reduce(sta, min_los)
        if red:
            out[tow] = red[0]
    return out


def census_series(census_csv):
    fleet = {}
    for r in csv.DictReader(open(census_csv, newline="")):
        if r["kind"] == "FLEET":
            fleet[round(float(r["tow"]), 3)] = float(r["level_tecu"])
    return fleet


def grade(run_dir, ionex, settled_from, min_los):
    logs = os.path.join(run_dir, "logs")
    census_csv = os.path.join(logs, "network_level_authority.csv")
    los_csv = os.path.join(logs, "network_level_authority_los.csv")
    if not os.path.isfile(census_csv):
        sys.exit("no census trace: " + census_csv)
    if not os.path.isfile(los_csv):
        sys.exit("no LOS dump: " + los_csv + "\n"
                 "G2's reference must be sampling-matched (SPEC §5.2).  Re-run with "
                 "network.level_authority_los_dump: true; this script refuses to "
                 "approximate the sampling.")
    maps = load_ionex(ionex)
    fleet = census_series(census_csv)
    ref = operator_series(los_csv, maps, min_los)
    tows = sorted(set(fleet) & set(ref))
    if not tows:
        sys.exit("no joint epochs between census and reference")
    t0 = tows[0]
    settled = [t for t in tows if t - t0 >= settled_from]
    if not settled:
        sys.exit("no settled epochs: the arm is shorter than --settled-from")
    d = [fleet[t] - ref[t] for t in settled]
    mean_d = sum(d) / len(d)
    pmean = statistics.median(
        [global_mean(g) for _s, g in maps]) if maps else float("nan")
    print("run                    %s" % run_dir)
    print("epochs                 %d total, %d settled (from +%.0f s)"
          % (len(tows), len(settled), settled_from))
    print("L_cs                   mean %+.3f  sd %.3f TECU"
          % (sum(fleet[t] for t in settled) / len(settled),
             statistics.pstdev([fleet[t] for t in settled])))
    print("censusOperator{GIM}    mean %+.3f  sd %.3f TECU"
          % (sum(ref[t] for t in settled) / len(settled),
             statistics.pstdev([ref[t] for t in settled])))
    print("GIM planetary mean     %+.3f TECU   (NOT the reference — shown so the "
          "sampling term it removes is visible)" % pmean)
    print("offset L_cs - operator mean %+.3f  sd %.3f  min %+.3f  max %+.3f  p2p %.3f TECU"
          % (mean_d, statistics.pstdev(d), min(d), max(d), max(d) - min(d)))
    stable = (max(d) - min(d)) <= 4.0        # "stable within +/- 2 TECU"
    inwin = -20.0 <= mean_d <= -10.0
    falsified = not (-25.0 <= mean_d <= 5.0)
    print("G2 stability           %s   [+/- 2 TECU => p2p <= 4.000]"
          % ("PASS" if stable else "FAIL"))
    print("G2 offset window       %s   [-10 .. -20 vertical TECU]"
          % ("PASS" if inwin else "FAIL"))
    if falsified:
        print("G2 VERDICT             STOP-RULE  (offset outside -25..+5 — the C4 "
              "confound model is falsified; record, no fold card)")
        return 2
    print("G2 VERDICT             %s" % ("PASS" if (stable and inwin) else "FAIL"))
    return 0 if (stable and inwin) else 1


def _selftest():
    # 1. reduction algebra, mirroring tests/test_level_authority.cpp
    r = level_authority_reduce({"a": [(10.0, 1.0), (20.0, 1.0)],
                                "b": [(30.0, 1.0), (30.0, 1.0)],
                                "c": [(100.0, 1.0), (100.0, 1.0)]}, 2)
    assert r and abs(r[0] - 30.0) < 1e-12, r
    r2 = level_authority_reduce({"a": [(0.0, 1.0), (10.0, 9.0)]}, 2)
    assert abs(r2[0] - 9.0) < 1e-12, r2          # information weighting
    assert level_authority_reduce({"a": [(1.0, 1.0)]}, 4) is None   # min_los screen
    assert level_authority_reduce({}, 4) is None                    # "not 0"
    print("reduction: weighted-mean-then-median, info weights, min_los, "
          "unavailable-is-None all OK")

    # 2. sampler: a constant field returns that constant at any point
    grid = {la: (-180.0, 5.0, [7.5] * 73) for la in (-10.0, -5.0, 0.0, 5.0, 10.0)}
    for lat, lon in ((0.0, 0.0), (3.3, -177.0), (-8.0, 179.9)):
        assert abs(sample_map(grid, lat, lon) - 7.5) < 1e-9, (lat, lon)
    # a query exactly on a node returns the node value
    g2 = {0.0: (-180.0, 180.0, [1.0, 2.0, 3.0]), 5.0: (-180.0, 180.0, [1.0, 2.0, 3.0])}
    assert abs(sample_map(g2, 0.0, 0.0) - 2.0) < 1e-9, sample_map(g2, 0.0, 0.0)
    print("sampler: constant-field invariance and node exactness OK")

    # 3. co-rotation is zero at a map epoch, and 15 deg/h away from it
    maps = [(0, grid), (3600, grid)]
    assert abs(gim_at(maps, 0, 0.0, 0.0) - 7.5) < 1e-9
    assert abs(EARTH_ROT_DEG_PER_SEC * 3600.0 - 15.0) < 1e-12
    print("co-rotation: 15.000 deg/h, no shift at a map epoch, OK")

    # 4. the reference is vertical: no mapping is applied anywhere (this check
    #    scans this file's own source, so no text in it may contain the
    #    multiply or divide by the mapping column)
    src = open(os.path.abspath(__file__), encoding="utf-8").read()
    assert "/ float(r[\"mapping\"])" not in src and "* float(r[\"mapping\"])" not in src
    print("units: the operator never applies the mapping column (both sides vertical) OK")
    print("SELFTEST PASS")
    return 0


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run_dir", nargs="*")
    ap.add_argument("--ionex", default=None,
                    help="IONEX path (default: the newest *GIM.INX under data/)")
    ap.add_argument("--settled-from", type=float, default=7200.0,
                    help="discard this many seconds from the first epoch (default 7200 "
                         "= the level channel's ~2 h convergence budget)")
    ap.add_argument("--min-los", type=int, default=4,
                    help="match network.level_authority_min_los (default 4)")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        sys.exit(_selftest())
    if not a.run_dir:
        ap.error("give at least one run_dir (or --selftest)")
    ionex = a.ionex
    if ionex is None:
        cand = sorted(glob.glob(os.path.join("data", "*", "products", "*GIM.INX")))
        if not cand:
            ap.error("no IONEX found under data/*/products/*GIM.INX; pass --ionex")
        ionex = cand[-1]
    rc = 0
    for rd in a.run_dir:
        rc = max(rc, grade(rd, ionex, a.settled_from, a.min_los))
        print()
    sys.exit(rc)
