#!/usr/bin/env python3
"""field_lane_frac.py: truth-free lane-fraction grade for field (and sim) runs.

The per-band DD fraction is iono-limited and uniform on every field lineage
(med|frac| 0.25; project_network_ddfrac_rootcause).  The lane fraction is a
different object: on matched (tow, sat, ref, sta_a, sta_b) quadruples,

    f_lane = wrap( dd_cyc(band_a) - dd_cyc(band_b) )      in [-0.5, 0.5)

is computable with no truth (the true lane DD is an integer, so its fraction
is identically 0; any spread in f_lane is pure estimation error), and it is
exactly the quantity the rung-3 carry-domain lane work targets.  Reads
<run>/logs/network_dd_fractional.csv (network.trace_dd_fractional).

This script was originally written for the seed-site snap that relevel_lane_snap
drove.  That mechanism was deleted on 2026-08-22 as a proven solve no-op
(SPEC_lane_slip_response 7.2), and the knob now arms a measure-only carry-domain
observatory (7.3), so an observatory arm and its control return the same
numbers here: that agreement is the observatory's wiring check, not a null
result.  This script grades the lane an apply leg would move, if one is ever
built.

Reports med|f|, P(|f|<0.10), P(|f|<0.25), and the circular resultant R per
lane pair, over the full run and the settled last quarter.  Statistics
discipline (O2 card): rows repeat per key, so only medians and rates are
reported, with the per-key collapsed view printed beside the row view.

Usage:
    python scripts/field_lane_frac.py runs/fld63_lanesnap4h [--last-frac 0.25]
"""

from __future__ import annotations

import argparse
import csv
import math
import os
import sys
from collections import defaultdict

LANES = [
    ("WL(1,2) G", "L1C", "L2W"),
    ("EWL(2,5) G", "L2W", "L5Q"),
    ("WL(1,5) E", "L1C", "L5Q"),
    ("EWL(5,7) E", "L5Q", "L7Q"),
]


def wrap(x: float) -> float:
    return x - round(x)


def stats(vals):
    if not vals:
        return None
    a = sorted(abs(v) for v in vals)
    n = len(a)
    med = a[n // 2]
    p10 = sum(1 for v in a if v < 0.10) / n * 100.0
    p25 = sum(1 for v in a if v < 0.25) / n * 100.0
    c = sum(math.cos(2 * math.pi * v) for v in vals) / n
    s = sum(math.sin(2 * math.pi * v) for v in vals) / n
    return n, med, p10, p25, math.hypot(c, s)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir")
    ap.add_argument("--last-frac", type=float, default=0.25)
    args = ap.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

    path = os.path.join(args.run_dir, "logs", "network_dd_fractional.csv")
    quad = defaultdict(dict)      # (tow, sat, ref, sa, sb) -> {signal: dd_cyc}
    tows = set()
    for r in csv.DictReader(open(path, newline="")):
        try:
            tow = float(r["tow"])
            quad[(tow, r["sat_i"], r["ref_sat"], r["sta_a"], r["sta_b"])][r["signal"]] = \
                float(r["dd_cyc"])
            tows.add(tow)
        except (TypeError, ValueError, KeyError):
            continue
    tows = sorted(tows)
    t_cut = tows[int(len(tows) * (1.0 - args.last_frac))] if len(tows) > 1 else 0.0
    print(f"== {args.run_dir}  ({len(quad)} quadruple-epochs, {len(tows)} epochs; "
          f"settled cut tow >= {t_cut:.0f})")

    for label, b1, b2 in LANES:
        rows_all, rows_set = [], []
        by_key_all = defaultdict(list)
        for (tow, sat, ref, sa, sb), sigs in quad.items():
            if b1 not in sigs or b2 not in sigs:
                continue
            f = wrap(sigs[b1] - sigs[b2])
            rows_all.append(f)
            by_key_all[(sat, ref, sa, sb)].append(f)
            if tow >= t_cut:
                rows_set.append(f)
        if not rows_all:
            continue
        # per-key collapsed view: one median fraction per quadruple key
        key_meds = []
        for k, v in by_key_all.items():
            v_sorted = sorted(v)
            key_meds.append(v_sorted[len(v_sorted) // 2])
        for tag, vals in (("all rows", rows_all), ("settled ", rows_set),
                          (f"per-key ({len(key_meds)})", key_meds)):
            st = stats(vals)
            if st:
                n, med, p10, p25, R = st
                print(f"  {label:<11} {tag:<16} n={n:8d}  med|f|={med:6.3f}  "
                      f"<0.10 {p10:5.1f}%  <0.25 {p25:5.1f}%  R={R:.3f}")
        print()


if __name__ == "__main__":
    main()
