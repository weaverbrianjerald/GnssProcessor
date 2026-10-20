#!/usr/bin/env python3
"""Read the block-Baarda events of a run against the other tiers' events at the same cells.

A BLOCKBAARDA event names one (station, satellite) line of sight at one epoch.  This tool
asks what else the producer said about that cell around that time: the RAIM trips, the
slip detectors, the redundancy census's cross-flags, the MDB, the prior-residual census,
the gross-LOS screen and the admission, readmission and hold events.  A block event with
no companion is the population only the block test sees.

Each tier's hit rate at the flagged cells is set against the rate at the same cells a
fixed time later (the control; earlier when the run ends first), so a tier that fires on
every cell all the time does not look like an explanation.  The flagged blocks are also
grouped by epoch and satellite, and by station: a fault of the satellite or the station
shows as many cells flagged together.

  block_baarda_join.py <run_dir> [--window-s 90] [--control-s 1800] [--top 12]

Reads <run_dir>/logs/network_events.csv and network_raim.csv.  Needs trace_events.
"""
import argparse
import csv
import os
import re
import sys
from collections import Counter, defaultdict

#: Tiers read against the block events, in reading order.  A tier name is the `tier` column.
TIERS = [
    ("RAIM", "network_raim.csv spiked rows"),
    ("T0_mw_detect", "MW/LLI slip detector"),
    ("T1_if_slip", "IF-phase slip detector"),
    ("T0_gap", "gap and arc resume"),
    ("T4_attrib", "slip attribution"),
    ("T0_windup", "wind-up restart"),
    ("REDUNDANCY", "census cross-flag"),
    ("MDB", "reliability exceedance"),
    ("PRIORRES", "prior-residual census"),
    ("GROSSLOS", "gross-LOS screen"),
    ("ADMISSION", "measurement admission"),
    ("READMIT", "readmission"),
    ("PB_HOLD", "phase-bias hold"),
    ("T0_band_scope", "band scope"),
]


def parse_detail(text):
    out = {}
    for part in text.split(";"):
        if "=" in part:
            k, v = part.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def load(run):
    logs = os.path.join(run, "logs")
    blocks = []                               # (tow, station, sat, t, detail)
    cells = defaultdict(lambda: defaultdict(list))   # tier -> (station, sat) -> [tow]
    path = os.path.join(logs, "network_events.csv")
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        rd = csv.reader(f)
        next(rd)
        for row in rd:
            if len(row) < 10:
                continue
            tow, tier, event, sat, sta = float(row[1]), row[2], row[3], row[4], row[5]
            if tier == "BLOCKBAARDA":
                try:
                    t = float(row[6])
                except ValueError:
                    t = float("nan")
                blocks.append((tow, sta, sat, t, row[9], event))
            else:
                cells[tier][(sta, sat)].append(tow)
    raim = os.path.join(logs, "network_raim.csv")
    if os.path.exists(raim):
        with open(raim, newline="", encoding="utf-8", errors="replace") as f:
            for r in csv.DictReader(f):
                if r.get("spiked") == "1":
                    cells["RAIM"][(r["station"], r["sat"])].append(float(r["tow"]))
    return blocks, cells


def near(tows, t0, win):
    return any(abs(t - t0) <= win for t in tows)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run")
    ap.add_argument("--window-s", type=float, default=90.0)
    ap.add_argument("--control-s", type=float, default=1800.0)
    ap.add_argument("--top", type=int, default=12)
    a = ap.parse_args()

    blocks, cells = load(a.run)
    if not blocks:
        print("no BLOCKBAARDA event in", a.run, "(none flagged, or trace_events off)")
        return 0
    n = len(blocks)
    only = sum(1 for b in blocks if b[5] == "block_only_outlier")
    t_end = max([b[0] for b in blocks] + [t for c in cells.values() for ts in c.values() for t in ts])
    print(f"{n} block event(s), {only} block-only; {len({(b[1], b[2]) for b in blocks})} distinct cells; "
          f"window +-{a.window_s:g} s, control +-{a.control_s:g} s")
    dofs = Counter(parse_detail(b[4]).get("dof", "-") for b in blocks)
    print("directions tested (dof) of the flagged blocks:", dict(sorted(dofs.items())))
    labels = Counter(parse_detail(b[4]).get("label", "?") for b in blocks)
    print("labels:", dict(labels))

    print(f"\n{'tier':<16}{'hit':>6}{'rate':>8}{'control':>9}{'lift':>7}   what")
    explained = [False] * n
    for tier, what in TIERS:
        c = cells.get(tier, {})
        hit = ctl = 0
        for i, (tow, sta, sat, t, _d, _e) in enumerate(blocks):
            tows = c.get((sta, sat), [])
            if near(tows, tow, a.window_s):
                hit += 1
                explained[i] = True
            shifted = tow + a.control_s if tow + a.control_s <= t_end else tow - a.control_s
            if near(tows, shifted, a.window_s):
                ctl += 1
        lift = (hit / ctl) if ctl else float("inf") if hit else 0.0
        print(f"{tier:<16}{hit:>6}{hit / n:>8.3f}{ctl:>9}{lift:>7.2f}   {what}")
    by_epoch_sat = Counter((b[0], b[2]) for b in blocks)
    wide = [(k, c) for k, c in by_epoch_sat.items() if c >= 3]
    print(f"\nepoch and satellite groups with 3 or more stations flagged together: {len(wide)}; "
          f"cells in them: {sum(c for _, c in wide)} of {n}")
    for (tow, sat), c in sorted(wide, key=lambda kc: -kc[1])[:a.top]:
        print(f"  tow {tow:9.1f}  {sat:<5} {c:3d} stations")
    by_sta = Counter(b[1] for b in blocks)
    print("stations with the most flagged cells:", ", ".join(f"{k} {v}" for k, v in by_sta.most_common(8)))
    unexplained = [b for b, e in zip(blocks, explained) if not e]
    print(f"\nblock events with none of the tiers above at the cell: {len(unexplained)} of {n}")
    for tow, sta, sat, t, d, ev in sorted(unexplained, key=lambda b: -b[3])[:a.top]:
        dd = parse_detail(d)
        print(f"  tow {tow:9.1f}  {sta:<10}{sat:<5} t_B {t:8.3g}  row_t {dd.get('row_t', '?'):>8}  "
              f"dof {dd.get('dof', '-')}/{dd.get('b', '?')}  {ev}  corrob {dd.get('corrob', '?')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
