#!/usr/bin/env python3
"""Per-epoch cost of the named sub-sections of one instrumented GPS + Galileo run against one GPS-only run.

    python scripts/cost_attrib/instr_summary.py <GE run dir> <G run dir>

The runs are made by an executable built with a scratch timing patch (named accumulators in the station-add,
covariance-consumer, reduce and observer code; the patch is not in the tree).  Each run writes
logs/nc_instr.txt (the named wall-clock accumulators) and, parsed by nc_parse.py, nc.json.  Prints the mean
dimensions of a station's normal equations (rows m, shared columns n_s, local columns n_r) and, per
sub-section, ms per epoch for each run, the ratio, the calls per epoch and the microseconds per call.  Inside
the parallel produce the accumulators are thread time, not wall; the sub-sections listed here run on the
solve thread.
"""
import json
import os
import re
import sys


def load(run):
    d = {}
    for ln in open(os.path.join(run, "logs", "nc_instr.txt"), encoding="utf-8"):
        m = re.match(r"(\S+)\s+([\d.]+) ms\s+(\d+) calls\s+([\d.]+) us/call\s+raw_mean ([\d.]+)", ln)
        if m:
            d[m[1]] = (float(m[2]), int(m[3]), float(m[4]), float(m[5]))
    return d


def main():
    ge_dir, g_dir = sys.argv[1], sys.argv[2]
    a, b = load(ge_dir), load(g_dir)
    ea = json.load(open(os.path.join(ge_dir, "nc.json")))["epochs"]
    eb = json.load(open(os.path.join(g_dir, "nc.json")))["epochs"]
    print(f"epochs {ea} (GPS + Galileo) and {eb} (GPS only)")
    for k in sorted(a):
        if k.startswith("asm.dim") and k in b:
            print(f"  {k:<24} G {b[k][3]:8.1f}   GE {a[k][3]:8.1f}")
    for k in sorted(a):
        if k.startswith("asm.dim") or k not in b:
            continue
        ra = a[k][0] / ea
        rb = b[k][0] / eb
        print(f"  {k:<44} G {rb:8.1f}  GE {ra:8.1f} ms/epoch  ratio {ra / rb if rb > 0 else 0:6.2f}"
              f"   calls/ep G {b[k][1] / eb:6.0f} GE {a[k][1] / ea:6.0f}  us/call G {b[k][2]:9.1f} GE {a[k][2]:9.1f}")


if __name__ == "__main__":
    main()
