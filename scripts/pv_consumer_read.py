#!/usr/bin/env python3
"""Card PV consumer read (SPEC_event_harness s24.11, the s24.3 instrument): the held-out closed-loop
consumer's error against the roster position it already differences against, per arm.

    python scripts/pv_consumer_read.py [--runs C:/GnssProcessor/runs] [--arms a0,pva,pvb]
        [--prefix _cm_] [--marks 30,60,90,120] [--tail-min 30] [--json out.json]

Per arm it prints, straight off `<armdir>/logs/closed_loop_trace.csv`:
  * 3D / E / N / U at 30, 60, 90 and 120 min AFTER THE CONSUMER'S OWN FIRST EPOCH (not after the
    producer's — a consumer that starts late would otherwise be graded on a shifted clock).  The
    mark is the trace row nearest that time; the actual offset is printed beside it, and a mark
    further than half an epoch off is flagged.
  * the p50 (and p90 / max) of 3D over the last `--tail-min` minutes of the trace.
  * consumer RESETS.  This lineage has no `rt_engine.log` and `closed_loop_trace.csv` has no reset
    column (its columns are week,tow,n_sats,ar_ran,ar_success,num_fixed,num_candidates,ratio,
    pos_err_3d_m,east_m,north_m,up_m,ar_gate,p_succ), so the count is taken from the producer's own
    voice in stdout.txt — the consumer 'initialized' line and any reset/re-init/restart line — and a
    STEP proxy (|d 3D| > --step-m between consecutive epochs) is reported beside it, labelled as a
    proxy.  A zero reset count that rests only on the absence of a channel says so.
  * the sha256 of the arm's products.txt, and the row / epoch counts.

NO BAR (s24.1): the cold 2 h consumer number is reported, never graded — s24.3 has it at metres on
every network product, and the point of the card is the producer-side (i)-(iii) bars.
"""
import argparse
import csv
import hashlib
import json
import os
import re
import sys

import numpy as np

EPOCH_S = 30.0


def sha256(path):
    if not os.path.isfile(path):
        return None
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_trace(path):
    """-> list of dicts with tow / err3d / e / n / u, in file order, numeric rows only."""
    rows = []
    if not os.path.isfile(path):
        return rows
    with open(path, newline="", encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(fh):
            try:
                rows.append(dict(tow=float(r["tow"]),
                                 err3d=float(r["pos_err_3d_m"]),
                                 e=float(r["east_m"]), n=float(r["north_m"]), u=float(r["up_m"]),
                                 gate=(r.get("ar_gate") or "").strip(),
                                 fixed=int(float(r.get("num_fixed") or 0))))
            except (TypeError, ValueError, KeyError):
                continue
    return rows


def resets_from_stdout(path):
    """(init_lines, reset_lines) from the producer's own voice — the only reset channel this lineage has."""
    init, rst = [], []
    if not os.path.isfile(path):
        return init, rst
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            # [gnss_config] lines are the loader's DESCRIPTIONS of what a knob does ("…no consumer
            # reset…"), not events — counting them gave a0 two phantom resets.
            if "[gnss_config]" in line:
                continue
            if not re.search(r"closed.loop|consumer", line, re.I):
                continue
            if re.search(r"initiali[sz]ed", line, re.I):
                init.append(line.strip()[-160:])
            elif re.search(r"\breset|re-?init|restart|re-?seed|diverg", line, re.I):
                rst.append(line.strip()[-160:])
    return init, rst


def grade(arm, armdir, marks, tail_min, step_m):
    tr = read_trace(os.path.join(armdir, "logs", "closed_loop_trace.csv"))
    R = dict(arm=arm, dir=armdir, rows=len(tr),
             products_sha256=sha256(os.path.join(armdir, "products.txt")))
    if not tr:
        R["error"] = "no closed_loop_trace.csv rows"
        return R
    t0 = tr[0]["tow"]
    R["tow_first"] = t0
    R["tow_last"] = tr[-1]["tow"]
    R["span_min"] = (tr[-1]["tow"] - t0) / 60.0
    tows = np.array([r["tow"] for r in tr])

    R["marks"] = []
    for m in marks:
        want = t0 + 60.0 * m
        i = int(np.argmin(np.abs(tows - want)))
        off = tows[i] - want
        R["marks"].append(dict(min=m, tow=tr[i]["tow"], offset_s=off,
                               beyond_half_epoch=bool(abs(off) > EPOCH_S / 2.0),
                               err3d=tr[i]["err3d"], e=tr[i]["e"], n=tr[i]["n"], u=tr[i]["u"]))

    tail = [r["err3d"] for r in tr if r["tow"] >= tr[-1]["tow"] - 60.0 * tail_min + 1e-6]
    if tail:
        a = np.array(tail)
        R["tail"] = dict(min=tail_min, n=len(tail), p50=float(np.median(a)),
                         p90=float(np.percentile(a, 90)), max=float(a.max()))

    d = np.abs(np.diff(np.array([r["err3d"] for r in tr])))
    R["step_proxy"] = dict(threshold_m=step_m, n=int((d > step_m).sum()),
                           max_step_m=float(d.max()) if d.size else None)
    init, rst = resets_from_stdout(os.path.join(armdir, "stdout.txt"))
    R["consumer_init_lines"] = init
    R["consumer_reset_lines"] = rst
    R["resets"] = len(rst)
    R["resets_channel"] = ("producer stdout (no rt_engine.log and no reset column in "
                           "closed_loop_trace.csv on this lineage)")
    R["fix_epochs"] = int(sum(1 for r in tr if r["fixed"] > 0))
    gates = {}
    for r in tr:
        # collapse sigma_gate(0.0912m) etc. to the gate NAME — the per-epoch threshold makes every
        # row its own key otherwise, and the useful read is which gate held, not at what value
        g = re.sub(r"\(.*\)", "", r["gate"]) or "(none)"
        gates[g] = gates.get(g, 0) + 1
    R["ar_gates"] = gates
    return R


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runs", default="C:/GnssProcessor/runs")
    ap.add_argument("--arms", default="a0,pva,pvb")
    ap.add_argument("--prefix", default="_cm_")
    ap.add_argument("--marks", default="30,60,90,120")
    ap.add_argument("--tail-min", type=float, default=30.0)
    ap.add_argument("--step-m", type=float, default=1.0,
                    help="|d 3D| between consecutive epochs counted as a STEP (reset proxy)")
    ap.add_argument("--json", default="")
    a = ap.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    marks = [float(x) for x in a.marks.split(",") if x]

    out = []
    for arm in [s for s in a.arms.split(",") if s]:
        armdir = os.path.join(a.runs, f"{a.prefix}{arm}")
        if not os.path.isdir(armdir):
            print(f"!! {armdir} missing - skipped")
            continue
        out.append(grade(arm, armdir, marks, a.tail_min, a.step_m))

    hdr = f"{'arm':<6}" + "".join(f"{('3D@%g' % m):>10}" for m in marks) + \
          f"{'p50 tail':>10}{'p90 tail':>10}{'resets':>8}{'steps':>7}{'rows':>6}"
    print("\nCLOSED-LOOP CONSUMER (WTZZ00DEU held out) - error vs the roster position, metres")
    print("=" * len(hdr))
    print(hdr)
    print("-" * len(hdr))
    for R in out:
        if R.get("error"):
            print(f"{R['arm']:<6}  {R['error']}")
            continue
        cells = "".join(f"{m['err3d']:>10.3f}" for m in R["marks"])
        t = R.get("tail") or {}
        print(f"{R['arm']:<6}{cells}{t.get('p50', float('nan')):>10.3f}"
              f"{t.get('p90', float('nan')):>10.3f}{R['resets']:>8}"
              f"{R['step_proxy']['n']:>7}{R['rows']:>6}")
    print("-" * len(hdr))
    print(f"p50/p90 tail = last {a.tail_min:g} min of the trace.  resets = producer-stdout lines "
          f"({out[0]['resets_channel'] if out else ''}).")
    print(f"steps = |d 3D| > {a.step_m:g} m between consecutive epochs - a PROXY, not a reset count.")
    print("NO BAR on these numbers (s24.1): reported, not graded.\n")

    for R in out:
        if R.get("error"):
            continue
        print(f"--- {R['arm']}  ({R['dir']})")
        print(f"    rows {R['rows']}  tow {R['tow_first']:.0f}..{R['tow_last']:.0f}  "
              f"span {R['span_min']:.1f} min   products.txt sha256 {R['products_sha256']}")
        for m in R["marks"]:
            flag = "  <-- NEAREST ROW IS > half an epoch off the mark" if m["beyond_half_epoch"] else ""
            print(f"    {m['min']:>5.0f} min (tow {m['tow']:.0f}, {m['offset_s']:+.0f} s): "
                  f"3D {m['err3d']:8.3f}   E {m['e']:8.3f}  N {m['n']:8.3f}  U {m['u']:8.3f}{flag}")
        t = R.get("tail") or {}
        if t:
            print(f"    last {t['min']:g} min (n={t['n']}): 3D p50 {t['p50']:.3f}  p90 {t['p90']:.3f}  "
                  f"max {t['max']:.3f}")
        print(f"    AR: {R['fix_epochs']} epoch(s) with a fix; gates {R['ar_gates']}")
        print(f"    consumer init lines: {len(R['consumer_init_lines'])}; reset lines: {R['resets']}")
        for line in R["consumer_reset_lines"][:5]:
            print(f"       ! {line}")
        print()

    if a.json:
        with open(a.json, "w", encoding="utf-8") as fh:
            json.dump(out, fh, indent=1, default=float)
        print(f"json -> {a.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
