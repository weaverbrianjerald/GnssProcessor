#!/usr/bin/env python3
"""Card IRD section 12 (IRD-0, gate 2 reading), promoted at IRD-0r (section 13.5, item 7): the
golden wire's E_L1 satellite pairs split by whether both satellites' L1C keys share a current
phase-datum anchor station (logs/network_phase_anchor.csv) at the epoch.

Per hour from the start TOW: n and p50 |wrap(E_L1^s - E_L1^p)| of the same-anchor and the
different-anchor pairs (scripts/simnet_wire_integer_consistency.py owns E_b).  A one-off
diagnostic, not a graded instrument.

Usage:
    python scripts/ird0_anchor_split.py <run_dir> <fixture_dir> <start_tow>
"""
import argparse
import csv
import os
import sys
from collections import defaultdict

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import simnet_wire_integer_consistency as W  # noqa: E402  (the wire algebra's one owner)


def anchor_split(run, fixture, t0):
    """{hour: {"same" | "diff": [|wrap D_L1|]}} over the pairs whose L1C keys have an anchor."""
    osb, sc, eps, _ = W.dual_load(fixture, run)
    anc = defaultdict(dict)
    with open(os.path.join(run, "logs", "network_phase_anchor.csv"), encoding="utf-8") as fh:
        for r in csv.DictReader(fh):
            if r["signal"] == "L1C":
                anc[round(float(r["tow"]), 3)][r["sat"]] = r["anchor_sta"]
    acc = defaultdict(lambda: defaultdict(list))
    for ep in eps:
        if ep["tow"] < t0:
            continue
        h = int((ep["tow"] - t0) // 3600)
        v = W.dual_sat_values(ep, osb, sc, ("L1C", "L2W"))
        a = anc.get(ep["tow"], {})
        s = sorted(x for x in v if x in a and x[0] == "G")
        for i in range(len(s)):
            for j in range(i + 1, len(s)):
                k = "same" if a[s[i]] == a[s[j]] else "diff"
                acc[h][k].append(abs(W.wrapf(v[s[i]]["E1"] - v[s[j]]["E1"])))
    return acc


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run")
    ap.add_argument("fixture")
    ap.add_argument("start_tow", type=float)
    a = ap.parse_args()
    acc = anchor_split(os.path.abspath(a.run), os.path.abspath(a.fixture), a.start_tow)
    for h in sorted(acc):
        print("h%d " % h + "  ".join("%s-anchor n %d E_L1 p50 %.3f"
                                     % (k, len(acc[h][k]), np.median(acc[h][k]))
                                     for k in ("same", "diff") if acc[h][k]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
