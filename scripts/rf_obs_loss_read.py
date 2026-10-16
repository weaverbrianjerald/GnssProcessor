#!/usr/bin/env python3
"""RF-NS-b: the vehicle observation-loss read.

Why this exists
---------------
`network_pf_keyed.csv` / `network_pf_rows.csv` are written only for stations that
survived `NetworkNormalEquations::addStation` (the per-station PD test,
`src/core/filter/NormalEquations.cpp:476-484`); every trace that keys off
`est_index_of[i] >= 0` inherits that filter.  Reading a satellite's post-fit row
count as "the satellite left the network" therefore confounds

  (1) the satellite not being tracked (an observation fact), with
  (2) its tracking stations being rejected by the local-block PD test
      (a solve fact, and the one the float vehicle moves).

This grader separates them:

  tracked   := stations with a row for (sat, epoch) in network_if_slip.csv.
               That trace is written from the raw observation stream and is
               byte-identical across the RF-0g golden and the RF-1/2b/2d vehicle,
               so it is the coverage truth for both arms.
  dropped   := stations named by "addStation rejected ... dropping it this epoch"
               in main_log*.log at that epoch.
  in_solve  := tracked - dropped  (cross-checked against the distinct stations
               actually carrying a post-fit code row for that satellite in
               network_pf_rows.csv on emitted epochs).

Usage
-----
    python scripts/rf_obs_loss_read.py <golden_run> <vehicle_run> --sat G07 \
        [--tow-min 273900] [--tow-max 276400] [--events] [--stride 1]

Every number it prints is a count of trace rows; it opens no build artifact.
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from collections import defaultdict
from pathlib import Path

DROP_RE = re.compile(
    r"\[2313,\s*([0-9.]+)\s*\(.*?station '([A-Z0-9]+)' addStation rejected"
)


def tow_key(s: str) -> int:
    return int(float(s))


def tracked_by_epoch(run: Path, sat: str, lo: float, hi: float):
    """Stations with an observation row for `sat`, per epoch, from the slip trace."""
    out = defaultdict(set)
    path = run / "logs" / "network_if_slip.csv"
    with path.open(newline="") as fh:
        rd = csv.reader(fh)
        next(rd, None)
        for row in rd:
            if len(row) < 4 or row[3] != sat:
                continue
            t = float(row[1])
            if lo <= t <= hi:
                out[tow_key(row[1])].add(row[2])
    return out


def dropped_by_epoch(run: Path, lo: float, hi: float):
    """Stations the addStation PD test rejected, per epoch, from the rotated logs."""
    out = defaultdict(set)
    logs = sorted((run / "logs").glob("main_log*.log"))
    for lg in logs:
        with lg.open(errors="replace") as fh:
            for line in fh:
                if "addStation rejected" not in line:
                    continue
                m = DROP_RE.search(line)
                if not m:
                    continue
                t = float(m.group(1))
                if lo <= t <= hi:
                    out[tow_key(m.group(1))].add(m.group(2))
    return out


def pf_code_stations(run: Path, sat: str, lo: float, hi: float):
    """Distinct stations with a post-fit code row for `sat`, per emitted epoch."""
    out = defaultdict(set)
    path = run / "logs" / "network_pf_rows.csv"
    with path.open(newline="") as fh:
        rd = csv.reader(fh)
        next(rd, None)
        for row in rd:
            if len(row) < 6 or row[3] != sat or row[5] != "code":
                continue
            t = float(row[1])
            if lo <= t <= hi:
                out[tow_key(row[1])].add(row[2])
    return out


def dispositions(run: Path, lo: float, hi: float):
    out = {}
    path = run / "logs" / "network_epoch_status.csv"
    with path.open(newline="") as fh:
        rd = csv.reader(fh)
        next(rd, None)
        for row in rd:
            if len(row) < 3:
                continue
            t = float(row[1])
            if lo <= t <= hi:
                out[tow_key(row[1])] = row[2]
    return out


def events(run: Path, sat: str, lo: float, hi: float):
    out = []
    path = run / "logs" / "network_events.csv"
    with path.open(newline="") as fh:
        rd = csv.reader(fh)
        next(rd, None)
        for row in rd:
            if len(row) < 10:
                continue
            t = float(row[1])
            if not (lo <= t <= hi):
                continue
            if row[4] == sat or f":{sat}:" in row[9] or f"={sat}" in row[9]:
                out.append(row)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("golden")
    ap.add_argument("vehicle")
    ap.add_argument("--sat", required=True)
    ap.add_argument("--tow-min", type=float, default=0.0)
    ap.add_argument("--tow-max", type=float, default=1e12)
    ap.add_argument("--stride", type=int, default=1, help="print every Nth epoch")
    ap.add_argument("--events", action="store_true")
    a = ap.parse_args()

    g, v = Path(a.golden), Path(a.vehicle)
    lo, hi = a.tow_min, a.tow_max

    trk_g = tracked_by_epoch(g, a.sat, lo, hi)
    trk_v = tracked_by_epoch(v, a.sat, lo, hi)
    drop_g = dropped_by_epoch(g, lo, hi)
    drop_v = dropped_by_epoch(v, lo, hi)
    pf_g = pf_code_stations(g, a.sat, lo, hi)
    pf_v = pf_code_stations(v, a.sat, lo, hi)
    dsp_g = dispositions(g, lo, hi)
    dsp_v = dispositions(v, lo, hi)

    print(f"# sat={a.sat}  golden={g.name}  vehicle={v.name}")
    print("# tracked = stations with an observation row (network_if_slip.csv)")
    print("# ndrop   = stations the addStation PD test rejected THIS epoch (all sats)")
    print("# in_solve= tracked - dropped;  pf = distinct stations with a post-fit CODE row")
    print("tow        trk  | G:ndrop in_solve pf  disp     | V:ndrop in_solve pf  disp")
    epochs = sorted(set(trk_g) | set(trk_v) | set(dsp_g) | set(dsp_v))
    for i, t in enumerate(epochs):
        if i % a.stride:
            continue
        tg, tv = trk_g.get(t, set()), trk_v.get(t, set())
        isg = len(tg - drop_g.get(t, set()))
        isv = len(tv - drop_v.get(t, set()))
        print(
            f"{t:<10d} {len(tg):>3}/{len(tv):<3}| "
            f"{len(drop_g.get(t, set())):>6} {isg:>8} {len(pf_g.get(t, set())):>3} "
            f"{dsp_g.get(t, '-'):<8} | "
            f"{len(drop_v.get(t, set())):>6} {isv:>8} {len(pf_v.get(t, set())):>3} "
            f"{dsp_v.get(t, '-'):<8}"
        )

    if a.events:
        for name, run in (("GOLDEN", g), ("VEHICLE", v)):
            print(f"\n## {name} events on {a.sat}")
            for row in events(run, a.sat, lo, hi):
                print(f"  {row[1]:>12} {row[2]:<14} {row[3]:<20} "
                      f"{row[4]:<4} {row[5]:<12} {row[9][:90]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
