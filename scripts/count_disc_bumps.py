#!/usr/bin/env python
"""Per-epoch discontinuity-bump counter from a run's logs/network_solution.csv.

Usage:  python scripts/count_disc_bumps.py <run_dir> [<run_dir2> ...]

For each run dir, prints per-epoch disc bumps split by kind (PB/CB/...) plus the
run total — the wire-side PB-churn metric used in the Jul-2026 dev300s campaign
(baseline: hand-tuned v1 arm ~10 PB/ep ref-path-only at 300 s; pre-fix ~180/ep).
"""
import csv
import collections
import sys
import os


def grade(run_dir: str) -> None:
    path = os.path.join(run_dir, "logs", "network_solution.csv")
    if not os.path.isfile(path):
        print(f"{run_dir}: MISSING {path}")
        return
    prev = {}
    bumps = collections.OrderedDict()
    keys_per_epoch = collections.Counter()
    pb_keys = collections.Counter()
    with open(path) as f:
        for row in csv.DictReader(f):
            tow = float(row["tow"])
            kind = row["kind"]
            k = (row["sat"], kind, row["signal"])
            dc = int(row["disc_counter"]) if row["disc_counter"] else 0
            bumps.setdefault(tow, collections.Counter())
            keys_per_epoch[tow] += 1
            if kind == "PB":
                pb_keys[tow] += 1
            if k in prev and dc > prev[k]:
                bumps[tow][kind] += dc - prev[k]
            prev[k] = dc
    print(f"== {run_dir}")
    print("tow       | keys (PB) | disc bumps by kind")
    total = collections.Counter()
    for tow, c in bumps.items():
        print(f"{tow:10.0f} | {keys_per_epoch[tow]:4d} ({pb_keys[tow]:3d}) | "
              f"{dict(c) if c else '-'}")
        total.update(c)
    n_trans = max(1, len(bumps) - 1)
    print(f"TOTAL: {dict(total)}  |  PB/epoch = {total.get('PB', 0) / n_trans:.1f} "
          f"over {n_trans} transitions")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for d in sys.argv[1:]:
        grade(d)
