#!/usr/bin/env python3
"""Select a globally-distributed subset of IGS stations from a CDDIS day listing.

Input: a CDDIS daily-directory listing (``curl '<dir>*?list'``) for a given day,
which names every RINEX-3 obs file actually present that day.  Keeps the
stations that are also in the ITRF2020 IGS SSC (so a held-fixed ECEF is
available, mm-precise) and farthest-point-samples N of them for even global
coverage: the geometry the network needs to observe satellite clocks and
biases well.

Emits ``selected.json``: ``[{long, code, ecef:[x,y,z]}, ...]`` for the fetch and
config-generation steps.  Reuses the test-suite SinexTruth resolver, loaded
standalone because the gnssviz package init needs pandas.
"""
from __future__ import annotations

import importlib.util
import json
import re
import sys
from pathlib import Path

import numpy as np

_REPO = Path(__file__).resolve().parent.parent


def load_sinex(ssc: str):
    spec = importlib.util.spec_from_file_location(
        "snxtruth", _REPO / "pyviz" / "gnssviz" / "eval" / "truth.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["snxtruth"] = mod
    spec.loader.exec_module(mod)
    return mod.SinexTruth(ssc)


def main() -> int:
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("listing", help="CDDIS '*?list' output for the day.")
    ap.add_argument("-n", "--num", type=int, default=50)
    ap.add_argument("--rate", default="30S")
    ap.add_argument("--year", type=float, default=2024.3497, help="decimal year for velocity propagation")
    ap.add_argument("--ssc", default=str(_REPO / "data" / "static" / "sinex" / "ITRF2020-IGS-TRF.SSC"))
    ap.add_argument("--out", default=str(_REPO / "config" / "test_suite" / "global_network_stations.json"))
    ap.add_argument("--keep", nargs="*", default=[], help="station 4-char codes to force-include")
    args = ap.parse_args()

    truth = load_sinex(args.ssc)
    text = Path(args.listing).read_text()
    pat = re.compile(r'([A-Z0-9]{9})_R_\d{11}_01D_' + re.escape(args.rate) + r'_MO\.crx\.gz')
    longs = sorted(set(pat.findall(text)))

    cands = []
    for ln in longs:
        tc = truth.resolve(ln[:4], args.year)
        if tc is None:
            continue
        cands.append((ln, ln[:4], np.array(tc.ecef, dtype=float)))
    print(f"{len(longs)} listed @ {args.rate}, {len(cands)} also in SSC", file=sys.stderr)
    if not cands:
        print("no candidates", file=sys.stderr)
        return 1

    by_code = {c[1]: c for c in cands}
    sel = [by_code[k] for k in args.keep if k in by_code]
    selset = {s[0] for s in sel}
    if not sel:                                  # seed at a pole for spread
        cands.sort(key=lambda c: -abs(c[2][2]))
        sel = [cands[0]]
        selset = {cands[0][0]}

    # Greedy farthest-point sampling (max-min ECEF distance) for global coverage.
    while len(sel) < min(args.num, len(cands)):
        best, bestd = None, -1.0
        for c in cands:
            if c[0] in selset:
                continue
            dmin = min(float(np.linalg.norm(c[2] - s[2])) for s in sel)
            if dmin > bestd:
                bestd, best = dmin, c
        if best is None:
            break
        sel.append(best)
        selset.add(best[0])

    out = [{"long": ln, "code": code, "ecef": [round(float(v), 4) for v in ecef]}
           for ln, code, ecef in sel]
    Path(args.out).write_text(json.dumps(out, indent=2))
    print(f"selected {len(out)} stations -> {args.out}", file=sys.stderr)
    for o in out:
        print(o["long"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
