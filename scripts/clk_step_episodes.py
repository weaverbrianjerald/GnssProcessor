#!/usr/bin/env python3
"""CLK_STEP episode census over network_events.csv.

The per-mark count is a bad discriminator (SPEC_pivot_visibility_gauge_defect
§5.0.1: 9 of 16 marks were ONE satellite crossing the 1.5 m bar on nine
consecutive epochs), so re-realizations are graded on EPISODES — contiguous
mark-runs per satellite (marks on consecutive epochs merge; a gap > one epoch
interval starts a new episode).  Reports per-episode onset / length / Σ|dev| /
max|dev| and run totals, plus the §5.07.4-defect-4 caveat: this census is
contaminated at both ends (a sub-threshold ramp has ZERO marks; a recovering
step is not a re-realization at all), so treat it as the CHANNEL the SPEC
grades, not as ground truth.

    python scripts/clk_step_episodes.py runs/pareref_off/logs [more ...]
    python scripts/clk_step_episodes.py --interval-s 30 runs/a/logs runs/b/logs

Reads <dir>/network_events.csv (or the dir itself if it contains the file).
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path


def find_events(d: Path) -> Path:
    for cand in (d, d / "logs"):
        p = cand / "network_events.csv"
        if p.is_file():
            return p
    sys.exit(f"ERROR: no network_events.csv under {d}")


def census(path: Path, interval_s: float):
    marks = []  # (tow, sat, dev)
    with path.open(newline="", encoding="utf-8", errors="replace") as f:
        for row in csv.reader(f):
            if len(row) < 7 or row[2] != "CLK_STEP":
                continue
            try:
                marks.append((float(row[1]), row[4], float(row[6])))
            except ValueError:
                continue
    marks.sort()
    episodes = {}  # sat -> list of [onset, last_tow, n, sum_abs, max_abs]
    for tow, sat, dev in marks:
        eps = episodes.setdefault(sat, [])
        if eps and tow - eps[-1][1] <= interval_s + 1e-6:
            e = eps[-1]
            e[1] = tow
            e[2] += 1
            e[3] += abs(dev)
            e[4] = max(e[4], abs(dev))
        else:
            eps.append([tow, tow, 1, abs(dev), abs(dev)])
    return marks, episodes


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("runs", nargs="+", type=Path)
    ap.add_argument("--interval-s", type=float, default=30.0,
                    help="epoch interval (s); marks this far apart are contiguous")
    args = ap.parse_args()

    for d in args.runs:
        marks, episodes = census(find_events(d), args.interval_s)
        n_ep = sum(len(v) for v in episodes.values())
        tot = sum(e[3] for v in episodes.values() for e in v)
        mx = max((e[4] for v in episodes.values() for e in v), default=0.0)
        print(f"\n=== {d} ===")
        print(f"marks {len(marks)}  episodes {n_ep}  "
              f"sum|dev| {tot:.3f} m  max|dev| {mx:.3f} m")
        for sat in sorted(episodes):
            for e in episodes[sat]:
                print(f"  {sat}  onset {e[0]:.0f}  epochs {e[2]:2d}  "
                      f"sum|dev| {e[3]:7.3f}  max {e[4]:7.3f}")


if __name__ == "__main__":
    main()
