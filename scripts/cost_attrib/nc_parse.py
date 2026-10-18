#!/usr/bin/env python3
"""Parse one run's stdout.txt into nc.json: the loop wall, the D0 sections, the D0b blocks.

    python nc_parse.py <run dir>            (writes <run dir>/nc.json, prints it)

Reads the 'Network[timing]' line and the 'Network (D0)' / 'Network (D0b)' lines the run end prints
(trace_section_timers, armed in both goldens).  Values are milliseconds over the whole run.
"""
import json
import os
import re
import sys


def parse(run):
    out = {"run": os.path.basename(run.rstrip("/\\")), "d0": {}, "d0b": {}}
    txt = open(os.path.join(run, "stdout.txt"), encoding="utf-8", errors="replace").read().splitlines()
    for ln in txt:
        m = re.search(r"Network\[timing\]: epoch-loop wall ([\d.]+) ms over (\d+) epochs = ([\d.]+) ms/epoch \| (\d+) stations", ln)
        if m:
            out["loop_ms"], out["epochs"], out["ms_epoch"], out["stations"] = float(m[1]), int(m[2]), float(m[3]), int(m[4])
        m = re.search(r"Network \(D0\)\s+(assembly-wall|reduce|solve|cov-inverse|cov-consumers|other|est\.addStation)\s+(\d+) ms", ln)
        if m:
            d = {"ms": float(m[2])}
            c = re.search(r"\[(\d+) (recomputes|solves|calls|adds)", ln)
            if c:
                d["calls"] = int(c[1])
            out["d0"][m[1]] = d
        m = re.search(r"Network \(D0b\)\s+(\S+)\s+net\s+(\d+) ms.*?\| wall\s+(\d+) ms \| cov-consumers\s+(\d+) ms \((\d+) calls\)", ln)
        if m:
            out["d0b"][m[1]] = {"net": float(m[2]), "wall": float(m[3]), "cov_ms": float(m[4]), "cov_calls": int(m[5])}
        m = re.search(r"Network \(D0b\)\s+unattributed\s+net\s+(\d+) ms", ln)
        if m:
            out["d0b"]["unattributed"] = {"net": float(m[1]), "wall": float(m[1]), "cov_ms": 0.0, "cov_calls": 0}
    tp = os.path.join(run, "timing.txt")
    if os.path.exists(tp):
        out["timing"] = dict(kv.split("=") for kv in open(tp).read().split())
    json.dump(out, open(os.path.join(run, "nc.json"), "w"), indent=1)
    return out


if __name__ == "__main__":
    print(json.dumps(parse(sys.argv[1])))
