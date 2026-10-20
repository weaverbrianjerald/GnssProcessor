#!/usr/bin/env python3
"""Per-constellation network satellite-clock RMS, epoch by epoch.

Reads a network producer run's logs/ and reports, for each epoch, the
per-constellation RMS and max |clock| of the emitted satellite clocks: the
diagnostic for watching BeiDou (system 'C') clock contamination or divergence
in a GPS+Galileo+BeiDou PPP-RTK network.

Usage: python analyze_network_clock_rms.py <run_dir>
  <run_dir> contains logs/network_solution.csv
"""
import csv
import math
import sys
from collections import defaultdict

SYS_NAME = {"G": "GPS", "E": "Galileo", "C": "BeiDou", "R": "GLONASS", "J": "QZSS"}


def rms(vals):
    return math.sqrt(sum(v * v for v in vals) / len(vals)) if vals else 0.0


def main(run_dir):
    soln = f"{run_dir}/logs/network_solution.csv"
    # epoch (week,tow) -> system char -> list of clock values (m)
    per = defaultdict(lambda: defaultdict(list))
    epochs = []
    seen = set()
    with open(soln, newline="") as f:
        for row in csv.DictReader(f):
            if row.get("kind") != "CLK":
                continue
            sat = row["sat"]
            sysc = sat[0]
            key = (int(row["week"]), float(row["tow"]))
            if key not in seen:
                seen.add(key)
                epochs.append(key)
            try:
                per[key][sysc].append(float(row["value_m"]))
            except ValueError:
                continue

    epochs.sort()
    systems = sorted({s for k in per for s in per[k]},
                     key=lambda c: ("GECRJ".index(c) if c in "GECRJ" else 9))
    hdr = ["epoch", "tow"]
    for s in systems:
        nm = SYS_NAME.get(s, s)
        hdr += [f"{nm}_n", f"{nm}_rms_m", f"{nm}_max_m"]
    print(",".join(hdr))

    # track peak per system for the summary
    peak = defaultdict(float)
    for i, key in enumerate(epochs):
        row = [str(i), f"{key[1]:.1f}"]
        for s in systems:
            vals = per[key].get(s, [])
            r = rms(vals)
            mx = max((abs(v) for v in vals), default=0.0)
            peak[s] = max(peak[s], mx)
            row += [str(len(vals)), f"{r:.4f}", f"{mx:.4f}"]
        print(",".join(row))

    print("\n=== peak |clock| over run (m) ===")
    for s in systems:
        print(f"  {SYS_NAME.get(s, s):8s}: peak |clk| = {peak[s]:12.3f} m   "
              f"({'DIVERGED' if peak[s] > 1e3 else 'BLOWING UP' if peak[s] > 100 else 'ok'})")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
