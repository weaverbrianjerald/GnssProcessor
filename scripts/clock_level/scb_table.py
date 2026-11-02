#!/usr/bin/env python3
"""scb_table.py (card SCB): one Markdown table from the JSON outputs of simnet_clock_truth_grade.py.

    python scripts/clock_level/scb_table.py <grade.json> [<grade.json> ...] [--label tag=Label ...]

Each JSON holds a list of run results (one per --run of the grader). Rows: one per run,
columns: level rms by hour (C1a), phase-visible rms by hour (C1b), per-satellite mean rms over
hours 2 to 6 (C1c), the rate and truth-mean correlations (C1d, clock fixture only), and the
consumer's hourly 3D p50 (C2). Metres, three decimals.
"""
import argparse
import json
import os


def fmt(v, nd=3):
    if v is None or v != v:
        return "-"
    return f"{v:.{nd}f}"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("json", nargs="+")
    ap.add_argument("--label", action="append", default=[], help="tag=Label for the run column")
    a = ap.parse_args()
    labels = dict(kv.split("=", 1) for kv in a.label)
    rows = []
    for p in a.json:
        for res in json.load(open(p)):
            tag = os.path.basename(res["run"].rstrip("/\\")).replace("_rap_", "")
            rows.append((labels.get(tag, tag), res))
    print("| arm | level rms by hour (m) | phase-visible rms by hour (m) | per-sat mean h2-6 rms (m) | corr(mean, rate) / corr(mean, truth mean) | consumer 3D p50 by hour (m) |")
    print("| --- | --- | --- | --- | --- | --- |")
    for label, res in rows:
        lev = " ".join(fmt(h["level_rms_m"]) for h in res["hours"])
        pv = " ".join(fmt(h["phase_visible_rms_m"]) for h in res["hours"])
        pm = fmt(res["persat_mean"]["rms_m"]) + f" ({res['persat_mean']['n_sat']} sats)"
        rc = res.get("rate_corr")
        corr = (f"{rc['corr_mean_vs_rate']:+.2f} / {rc['corr_mean_vs_truth_mean']:+.2f}" if rc
                and rc["corr_mean_vs_rate"] == rc["corr_mean_vs_rate"] else "-")
        cons = " ".join(fmt(c) for c in res["consumer_p50_m"]) if res["consumer_p50_m"] else "-"
        print(f"| {label} | {lev} | {pv} | {pm} | {corr} | {cons} |")


if __name__ == "__main__":
    main()
