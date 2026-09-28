#!/usr/bin/env python3
"""Card ALC - read the consumer ambiguity level check's own telemetry.

    python scripts/alc_level_read.py <run_dir> [<run_dir> ...] [--hour-bins 6]

Trace-only: launches nothing, writes nothing but stdout.

Reads the `AMB_LEVEL` rows of `logs/network_events.csv` (invariant #7's two
edges: `trip` is the decoupling test firing, `refloat` is the response) and the
`refloat_level` kind in `ev_pb` of `logs/closed_loop_sat_trace.csv`, and reports:

  * trips and refloats per hour (binned on the producer's own tow) and per
    satellite, split by which test fired (wl / if);
  * the distribution of |state - level| / sigma_level at the trips, the
    quantity the k bar is applied to, so a census that sits just above k is a
    different finding from one that sits at 20 k;
  * the trips inside a named window (--tow-min / --tow-max), to check whether
    the check fires at the tow of an accidental reset it is meant to catch.

The counter closure (checks / trips / refloats) is on the run's own summary
line; this script is the per-hour and per-satellite breakdown that line cannot
carry.
"""
from __future__ import annotations

import argparse
import csv
import os
import sys
from collections import Counter, defaultdict


def load(run_dir):
    rows = []
    p = os.path.join(run_dir, "logs", "network_events.csv")
    if not os.path.exists(p):
        return rows
    with open(p, encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(fh):
            if r.get("tier") != "AMB_LEVEL":
                continue
            try:
                tow = float(r.get("tow") or "nan")
            except ValueError:
                tow = float("nan")

            def num(k):
                v = r.get(k)
                try:
                    return float(v) if v not in (None, "") else None
                except ValueError:
                    return None

            rows.append({
                "tow": tow,
                "kind": r.get("event") or "",
                "sat": r.get("sat") or "",
                "test": r.get("action") or "",
                "diff": num("statistic"),
                "sigma": num("threshold"),
                "detail": r.get("detail") or "",
            })
    return rows


def cst_refloats(run_dir):
    """(sat, signal) cell-epochs whose CST ev_pb carries `refloat_level`."""
    p = os.path.join(run_dir, "logs", "closed_loop_sat_trace.csv")
    n, sats = 0, Counter()
    if not os.path.exists(p):
        return n, sats
    with open(p, encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(fh):
            if "refloat_level" in (r.get("ev_pb") or ""):
                n += 1
                sats[r.get("sat") or "?"] += 1
    return n, sats


def q(vals, pct):
    if not vals:
        return float("nan")
    s = sorted(vals)
    i = (len(s) - 1) * pct / 100.0
    lo, hi = int(i), min(int(i) + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (i - lo)


def report(run_dir, args):
    rows = load(run_dir)
    print("== %s : %d AMB_LEVEL rows" % (run_dir, len(rows)))
    if not rows:
        print("   (none - the knob was OFF, or no window ever filled)")
        return

    trips = [r for r in rows if r["kind"] == "trip"]
    refl = [r for r in rows if r["kind"] == "refloat"]
    t0 = min(r["tow"] for r in rows)
    print("   trips %d (wl %d / if %d) | refloats %d | tow %.1f .. %.1f"
          % (len(trips), sum(1 for r in trips if r["test"] == "wl"),
             sum(1 for r in trips if r["test"] == "if"), len(refl),
             min(r["tow"] for r in rows), max(r["tow"] for r in rows)))

    # Per hour, binned on the run's own first emitted epoch.
    per_h = defaultdict(Counter)
    for r in rows:
        h = int((r["tow"] - args.t0_ref) // 3600) + 1 if args.t0_ref is not None \
            else int((r["tow"] - t0) // 3600) + 1
        per_h[h][r["kind"] + ":" + r["test"]] += 1
        per_h[h][r["kind"]] += 1
    print("   per hour: " + "  ".join(
        "h%d trip %d (wl %d/if %d) refl %d" % (h, per_h[h]["trip"],
                                               per_h[h]["trip:wl"], per_h[h]["trip:if"],
                                               per_h[h]["refloat"])
        for h in sorted(per_h)))

    per_s = defaultdict(Counter)
    for r in rows:
        per_s[r["sat"]][r["kind"]] += 1
        if r["kind"] == "trip":
            per_s[r["sat"]][r["test"]] += 1
    print("   per satellite: " + "  ".join(
        "%s %d(wl %d/if %d)" % (s, per_s[s]["trip"], per_s[s]["wl"], per_s[s]["if"])
        for s in sorted(per_s)))

    ratios = [r["diff"] / r["sigma"] for r in trips
              if r["diff"] is not None and r["sigma"] not in (None, 0.0)]
    if ratios:
        print("   |state-level| / sigma at the trips: n %d  min %.2f  p25 %.2f  p50 %.2f  "
              "p90 %.2f  max %.2f" % (len(ratios), min(ratios), q(ratios, 25),
                                      q(ratios, 50), q(ratios, 90), max(ratios)))
    for tst in ("wl", "if"):
        d = [r["diff"] for r in trips if r["test"] == tst and r["diff"] is not None]
        if d:
            unit = "cyc" if tst == "wl" else "m"
            print("   |state-level| at the %s trips (%s): n %d  p50 %.4f  p90 %.4f  max %.4f"
                  % (tst.upper(), unit, len(d), q(d, 50), q(d, 90), max(d)))

    if args.tow_min is not None and args.tow_max is not None:
        w = [r for r in rows if args.tow_min <= r["tow"] <= args.tow_max]
        wt = [r for r in w if r["kind"] == "trip"]
        print("   WINDOW [%.0f, %.0f]: %d rows, trips %d (wl %d / if %d), refloats %d"
              % (args.tow_min, args.tow_max, len(w), len(wt),
                 sum(1 for r in wt if r["test"] == "wl"),
                 sum(1 for r in wt if r["test"] == "if"),
                 sum(1 for r in w if r["kind"] == "refloat")))
        for r in sorted(w, key=lambda r: r["tow"])[:args.top]:
            print("      %.1f %-8s %-4s %-3s diff %.4f sigma %.4f | %s"
                  % (r["tow"], r["kind"], r["sat"], r["test"],
                     r["diff"] if r["diff"] is not None else float("nan"),
                     r["sigma"] if r["sigma"] is not None else float("nan"),
                     r["detail"][:110]))

    n_cst, cst_by_sat = cst_refloats(run_dir)
    print("   CST cross-check: %d cell-epochs carry `refloat_level` (expect 2 x refloats "
          "= %d); %s" % (n_cst, 2 * len(refl),
                         "AGREE" if n_cst == 2 * len(refl) else "MISMATCH"))
    if cst_by_sat:
        print("      by satellite: " + "  ".join("%s %d" % (s, n)
                                                 for s, n in sorted(cst_by_sat.items())))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir", nargs="+")
    ap.add_argument("--tow-min", type=float, default=None)
    ap.add_argument("--tow-max", type=float, default=None)
    ap.add_argument("--t0-ref", type=float, default=None,
                    help="absolute tow of hour-1 start (default: the first AMB_LEVEL row)")
    ap.add_argument("--top", type=int, default=30)
    args = ap.parse_args()
    for d in args.run_dir:
        report(d, args)


if __name__ == "__main__":
    main()
