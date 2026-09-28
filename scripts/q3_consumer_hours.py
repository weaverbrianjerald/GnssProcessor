#!/usr/bin/env python3
"""Hourly level bands of the in-process closed-loop consumer, for Card Q3 A/B arms.

Grades ``logs/closed_loop_trace.csv`` (the live WTZZ holdout) as hourly medians,
p95, worst 3D, plus the AR fix tallies.

Hourly level bands are graded instead of a slope because a warm-resumed or
cold-started filter's error is not monotone: ep20 is a local minimum about 30 %
below the true floor, so a line fitted through the early window reads the
climb-back as drift (``project_day129_pivot_cb_ratchet``, "Judge hourly LEVEL
bands, never slopes"). The first hour is ignition on a cold lineage and is
reported but never used for a verdict on its own.

Usage:
    python scripts/q3_consumer_hours.py <run_dir_or_trace.csv> [...]
"""
import os
import statistics as st
import sys


def load(path):
    if os.path.isdir(path):
        for cand in ("closed_loop_trace.csv", os.path.join("logs", "closed_loop_trace.csv")):
            p = os.path.join(path, cand)
            if os.path.exists(p):
                path = p
                break
    rows = []
    with open(path) as f:
        head = f.readline().rstrip("\n").split(",")
        ix = {k: i for i, k in enumerate(head)}
        for line in f:
            p = line.rstrip("\n").split(",")
            if len(p) < len(head):
                continue
            try:
                rows.append((float(p[ix["tow"]]), float(p[ix["pos_err_3d_m"]]),
                             int(p[ix["ar_ran"]]), int(p[ix["num_fixed"]])))
            except ValueError:
                continue
    return path, rows


def main():
    for arg in sys.argv[1:]:
        path, rows = load(arg)
        if not rows:
            print(f"{path}: NO ROWS")
            continue
        t0 = rows[0][0]
        by_hour = {}
        for tow, err, ar, fixed in rows:
            by_hour.setdefault(int((tow - t0) // 3600), []).append((err, ar, fixed))
        print("=" * 78)
        print(f"{path}: {len(rows)} epochs")
        print("  hour     n     med      p95     worst    >20m   ar_ran  fixed")
        tot_ar = tot_fix = 0
        for h in sorted(by_hour):
            v = by_hour[h]
            e = sorted(x[0] for x in v)
            ar = sum(x[1] for x in v)
            fx = sum(1 for x in v if x[2] > 0)
            tot_ar += ar
            tot_fix += fx
            print(f"  {h:>4} {len(e):>5} {st.median(e):>7.2f} "
                  f"{e[int(0.95 * (len(e) - 1))]:>8.2f} {e[-1]:>8.2f} "
                  f"{sum(1 for x in e if x > 20):>6} {ar:>8} {fx:>6}")
        e = sorted(x[0] for x in (y for v in by_hour.values() for y in v))
        print(f"   ALL {len(e):>5} {st.median(e):>7.2f} "
              f"{e[int(0.95 * (len(e) - 1))]:>8.2f} {e[-1]:>8.2f} "
              f"{sum(1 for x in e if x > 20):>6} {tot_ar:>8} {tot_fix:>6}")
        # Standing invariant: hour 0 is ignition on a cold lineage.  Report the
        # settled band separately so a verdict is never taken off the transient.
        settled = [x[0] for h, v in by_hour.items() if h >= 1 for x in v]
        if settled:
            s = sorted(settled)
            print(f"  SETTLED (hour >= 1): n {len(s)} med {st.median(s):.2f} "
                  f"p95 {s[int(0.95 * (len(s) - 1))]:.2f} worst {s[-1]:.2f} "
                  f">20m {sum(1 for x in s if x > 20)}")


if __name__ == "__main__":
    main()
