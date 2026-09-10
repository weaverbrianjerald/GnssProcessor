#!/usr/bin/env python3
"""EVENT HARNESS Study 1 — the erase-vs-refloat summary table.

Runs ``scripts/event_grade.py --json`` for every leg of the matrix against the
single NULL leg, then reduces each leg to one row:

    peak |e| (cyc) | |e| at +14 / +30 / +45 | floor mean +14..+45 |
    ZWD peak (mm)  | iono error for the target sat at +45 (m) |
    T4/T3 re-flags on the arc within 45 ep | consumer 3D peak (m)

``lvl`` is the injected arc's chord-median DD level error, EVENT MINUS NULL
(the grader's ``lvl_diff``) — the DD trace's single reference satellite cancels
in the difference wherever the two legs share it, which they do on a quiet bed.

The iono column is read straight from ``logs/network_ionosphere.csv``
(``iono_total_m`` = x_lin + x_r) as event − null for the injected (station,
satellite), median over the LOSs of that satellite at that station.  Absolute
slant iono from that trace is frame-broken (CLAUDE.md), but a same-snapshot
event − null difference is not: both legs seeded the state identically.
"""
import argparse
import csv
import json
import os
import statistics
import subprocess
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(_HERE)
FIXTURE = "C:/GnssProcessor/runs/simnet60_quiet_wu93_24h"
EPOCH_S = 30.0


def grade(legdir, null, out_json):
    cmd = [sys.executable, os.path.join(_HERE, "event_grade.py"), legdir,
           "--null", null, "--fixture", FIXTURE, "--pre", "10", "--post", "50",
           "--json", out_json]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode:
        print(r.stdout.decode("utf-8", "replace"))
        raise SystemExit(f"grade failed for {legdir}")
    with open(out_json, encoding="utf-8") as f:
        return json.load(f)


def iono_series(legdir, sta, sat):
    """tow -> median iono_total_m over that (station, satellite)'s rows."""
    p = os.path.join(legdir, "logs", "network_ionosphere.csv")
    if not os.path.isfile(p):
        return {}
    acc = {}
    with open(p, newline="") as f:
        for r in csv.DictReader(f):
            if r["station"] != sta or r["sat"] != sat:
                continue
            acc.setdefault(float(r["tow"]), []).append(float(r["iono_total_m"]))
    return {t: statistics.median(v) for t, v in acc.items()}


def qc_reflags(res, lo=1, hi=45):
    """T3/T4 rows on the arc AFTER the injected event, event leg only."""
    n = 0
    tiers = []
    for k, rows in (res.get("qc") or {}).items():
        ki = int(k)
        if ki < lo or ki > hi:
            continue
        for row in rows:
            if row.startswith("T4") or row.startswith("T3"):
                n += 1
                tiers.append(f"{row}@{ki:+d}")
    return n, tiers


def f(v, d=3):
    return "-" if v is None else f"{v:.{d}f}"


MATRIX = [
    # label,           legdir,              station,     sat
    ("arc1 slip 10cyc", "evleg_slip",       "S21000SIM", "G10"),
    ("arc1 erase",      "evleg_erase",      "S21000SIM", "G10"),
    ("arc1 keep_iono",  "evleg_s1_keepiono", "S21000SIM", "G10"),
    ("arc1 no_reseed",  "evleg_s1_noreseed", "S21000SIM", "G10"),
    ("arc1 no_funnel",  "evleg_s1_nofunnel", "S21000SIM", "G10"),
    ("arc1 free_epoch", "evleg_s1_free",    "S21000SIM", "G10"),
    ("arc2 erase",      "evleg_s2_erase",   "S50000SIM", "G31"),
    ("arc2 no_reseed",  "evleg_s2_noreseed", "S50000SIM", "G31"),
    ("arc2 free_epoch", "evleg_s2_free",    "S50000SIM", "G31"),
    ("arc3 erase",      "evleg_s3_erase",   "S40000SIM", "G31"),
    ("arc3 no_reseed",  "evleg_s3_noreseed", "S40000SIM", "G31"),
    ("arc3 free_epoch", "evleg_s3_free",    "S40000SIM", "G31"),
    # rung 2 — the SAME actions restricted to 2 of the 3 bands
    ("arc1 erase L1L2", "evleg_s1_erase_l12", "S21000SIM", "G10"),
    ("arc1 free  L1L2", "evleg_s1_free_l12", "S21000SIM", "G10"),
    ("arc2 free  L1L2", "evleg_s2_free_l12", "S50000SIM", "G31"),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--null", default="runs/evleg_null")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()
    null = os.path.join(REPO, args.null) if not os.path.isabs(args.null) else args.null
    scratch = os.path.join(REPO, "runs", "_study1_json")
    os.makedirs(scratch, exist_ok=True)

    hdr = (f"{'leg':17s} {'peak|e|':>8s} {'@ep':>4s} {'+14':>7s} {'+30':>7s} "
           f"{'+45':>7s} {'floor':>7s} {'ZWDpk_mm':>9s} {'dIono+45':>9s} "
           f"{'T4/T3':>6s} {'cons_pk':>8s}")
    print(hdr)
    print("-" * len(hdr))
    rows = []
    for label, leg, sta, sat in MATRIX:
        legdir = os.path.join(REPO, "runs", leg)
        res = grade(legdir, null, os.path.join(scratch, leg + ".json"))
        d = {int(k): v for k, v in (res.get("lvl_diff") or {}).items()
             if v is not None}
        zw = {int(k): v for k, v in (res.get("zwd_diff") or {}).items()
              if v is not None}
        cn = {int(k): v for k, v in (res.get("cons_diff") or {}).items()
              if v is not None}
        pk_k = max(d, key=lambda k: abs(d[k])) if d else None
        floor = [abs(d[k]) for k in d if 14 <= k <= 45]
        zpk_k = max(zw, key=lambda k: abs(zw[k])) if zw else None
        cpk_k = max(cn, key=lambda k: abs(cn[k])) if cn else None
        t0 = res["t0"] if "t0" in res else None
        if t0 is None:
            with open(os.path.join(legdir, "leg_meta.json"), encoding="utf-8") as fh:
                t0 = json.load(fh)["event_tow"]
        ie, inl = iono_series(legdir, sta, sat), iono_series(null, sta, sat)
        t45 = t0 + 45 * EPOCH_S
        di = None
        for t in ie:
            if abs(t - t45) < 0.5 and t in inl:
                di = ie[t] - inl[t]
        nq, tiers = qc_reflags(res)
        rows.append(dict(label=label, leg=leg, peak=(d.get(pk_k) if pk_k is not None else None),
                         peak_ep=pk_k, e14=d.get(14), e30=d.get(30), e45=d.get(45),
                         floor=(sum(floor) / len(floor) if floor else None),
                         zwd_pk=(zw.get(zpk_k) if zpk_k is not None else None),
                         zwd_pk_ep=zpk_k, iono45=di, reflags=nq, tiers=tiers,
                         cons_pk=(cn.get(cpk_k) if cpk_k is not None else None),
                         cons_pk_ep=cpk_k))
        r = rows[-1]
        print(f"{label:17s} {f(abs(r['peak']) if r['peak'] is not None else None):>8s} "
              f"{(f'{pk_k:+d}' if pk_k is not None else '-'):>4s} "
              f"{f(abs(r['e14']) if r['e14'] is not None else None):>7s} "
              f"{f(abs(r['e30']) if r['e30'] is not None else None):>7s} "
              f"{f(abs(r['e45']) if r['e45'] is not None else None):>7s} "
              f"{f(r['floor']):>7s} "
              f"{f((r['zwd_pk'] or 0) * 1000.0, 1):>9s} "
              f"{f(r['iono45'], 4):>9s} {r['reflags']:>6d} "
              f"{f(r['cons_pk']):>8s}")
    out = args.out or os.path.join(scratch, "study1_table.json")
    with open(out, "w", encoding="utf-8") as fh:
        json.dump(rows, fh, indent=2)
    print(f"\njson: {out}")
    for r in rows:
        if r["tiers"]:
            print(f"  {r['label']:17s} re-flags: {', '.join(r['tiers'][:8])}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
