#!/usr/bin/env python3
"""Gate: the production ocean tide loading model against the IERS HARDISP reference.

For each station the script
  1. takes the SIF ocean tide loading block (the same JSON the production loader reads),
  2. evaluates NGS M-PAGES v1.2 HARDISP (hardisp_port.py, 342 constituents, spline
     admittance) over one UTC day at a regular step, feeding the SIF columns to the
     routine exactly as M-PAGES does (UP, EAST, NORTH to its up, west, south rows;
     OceanTideLoad::computeOtlDisplacement then returns ENU = (-west, -south, up)),
  3. runs gnss_otl_diag on the same epochs for three variants of the production model:
        legacy  the SIF reading of processing.enable_station_info (east/north columns read
                as true east/north, phases + 180 degrees), nodal corrections off
        blq     otl_from_sif mapping (columns read as the BLQ west/south rows), nodal off
        blq+nod the same with otl_nodal_corrections (nodal factors, UTC Greenwich angle)
  4. reports, per variant and component, the maximum and rms difference to HARDISP in mm
     together with the signal's own peak.

    python -I otl_gate.py --exe <gnss_otl_diag.exe> [--date 2024-129] [--step 120]
                          [--stations KOKB00USA BRST00FRA WTZZ00DEU] [--json out.json]
"""
import argparse
import csv
import json
import math
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hardisp_port as hp          # noqa: E402

WORK = "D:/GnssProcessorSupport/gates/otl"
SIF_FILES = ["C:/GnssProcessor/data/static/sif/igs20_SIF.json",
             "C:/GnssProcessor/data/static/sif/ncn20_SIF.json"]
NAMES = ["M2", "S2", "N2", "K2", "K1", "O1", "P1", "Q1", "Mf", "Mm", "Ssa"]


def sif_record(key):
    for path in SIF_FILES:
        for s in json.load(open(path))["STATIONS"]:
            if s["ID"] == key and s.get("OCEAN_TIDE_LOADING"):
                return s
    raise KeyError(key)


def hardisp_columns(rec, shift_horizontal):
    """SIF columns -> HARDISP [11][3] amp and phase, the M-PAGES way (shift 0), or with the
    production legacy reading (shift 180 on both horizontal phases)."""
    otl = rec["OCEAN_TIDE_LOADING"]
    amp, phs = [], []
    for n in NAMES:
        c = otl[n if n in otl else n.upper()]
        amp.append([c["UP_AMPLITUDE"], c["EAST_AMPLITUDE"], c["NORTH_AMPLITUDE"]])
        phs.append([c["UP_PHASE"], c["EAST_PHASE"] + shift_horizontal, c["NORTH_PHASE"] + shift_horizontal])
    return amp, phs


def run_series(exe, key, date, step, extra):
    out = os.path.join(WORK, f"series_{key}_{'_'.join(x.strip('-') for x in extra)}.csv")
    cmd = [exe, "series"] + sum([["--sif", p] for p in SIF_FILES], []) +           ["--station", key, "--date", date, "--hours", "24", "--step", str(step), "--out", out] + extra
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(r.stderr[-400:])
    return list(csv.DictReader(open(out)))


def stats(a, b):
    d = [x - y for x, y in zip(a, b)]
    return (max(abs(v) for v in d) * 1000.0, math.sqrt(sum(v * v for v in d) / len(d)) * 1000.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--date", default="2024-129")
    ap.add_argument("--step", type=int, default=120)
    ap.add_argument("--stations", nargs="*", default=["KOKB00USA", "BRST00FRA", "WTZZ00DEU"])
    ap.add_argument("--json", default=None)
    a = ap.parse_args()
    year, doy = (int(v) for v in a.date.split("-"))
    h = hp.Hardisp()
    variants = [("legacy", ["--mapping", "legacy"]),
                ("blq", ["--mapping", "blq"]),
                ("blq+nodal", ["--mapping", "blq", "--nodal"])]
    out = {}
    for key in a.stations:
        rec = sif_record(key)
        amp_m, phs_m = hardisp_columns(rec, 0.0)         # the M-PAGES reading
        amp_l, phs_l = hardisp_columns(rec, 180.0)       # the legacy reading
        series = {name: run_series(a.exe, key, a.date, a.step, extra) for name, extra in variants}
        n = len(series["blq"])
        ref = {"E": [], "N": [], "U": []}
        ref_legacy = {"E": [], "N": [], "U": []}
        for i in range(n):
            sod = i * a.step
            hh, mm, ss = sod // 3600, (sod // 60) % 60, sod % 60
            u, s, w = h.evaluate(year, doy, hh, mm, ss, amp_m, phs_m)
            ref["E"].append(-w); ref["N"].append(-s); ref["U"].append(u)
            u2, s2, w2 = h.evaluate(year, doy, hh, mm, ss, amp_l, phs_l)
            ref_legacy["E"].append(-w2); ref_legacy["N"].append(-s2); ref_legacy["U"].append(u2)
        peak = {c: max(abs(v) for v in ref[c]) * 1000.0 for c in "ENU"}
        print(f"\n{key}  ({rec['ID']}), {a.date} UTC, step {a.step} s, {n} epochs; "
              f"HARDISP peak E {peak['E']:.2f} N {peak['N']:.2f} U {peak['U']:.2f} mm")
        print("  production variant vs HARDISP        max |diff| mm (rms)")
        print("                                       East            North           Up")
        res = {}
        for name, _ in variants:
            col = {"E": [float(r["e_m"]) for r in series[name]],
                   "N": [float(r["n_m"]) for r in series[name]],
                   "U": [float(r["u_m"]) for r in series[name]]}
            refc = ref_legacy if name == "legacy" else ref
            label = "vs HARDISP, legacy reading" if name == "legacy" else "vs HARDISP, M-PAGES reading"
            st = {c: stats(col[c], refc[c]) for c in "ENU"}
            res[name] = {c: {"max_mm": st[c][0], "rms_mm": st[c][1]} for c in "ENU"}
            print(f"  {name:10s} {label:29s}" + "  ".join(f"{st[c][0]:6.3f} ({st[c][1]:5.3f})" for c in "ENU"))
            if name == "legacy":
                st2 = {c: stats(col[c], ref[c]) for c in "ENU"}
                res["legacy_vs_mpages"] = {c: {"max_mm": st2[c][0], "rms_mm": st2[c][1]} for c in "ENU"}
                print(f"  {name:10s} {'vs HARDISP, M-PAGES reading':29s}" +
                      "  ".join(f"{st2[c][0]:6.3f} ({st2[c][1]:5.3f})" for c in "ENU"))
        out[key] = {"peak_mm": peak, "diff": res, "epochs": n, "step_s": a.step, "date": a.date}
    if a.json:
        json.dump(out, open(a.json, "w"), indent=1)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
