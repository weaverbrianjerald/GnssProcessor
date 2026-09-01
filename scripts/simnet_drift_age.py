#!/usr/bin/env python3
"""simnet_drift_age.py -- STABILITY target 1: settled late drift vs carry age.

The 12 h quiet flagship (_camp60wu_quiet12h_r2q3) rises h7->h11 in settled
consumer 3D (0.107 -> 0.179 rms) while young-carry resumed legs beat it
(0.088-0.134).  Named suspect: carry aging / accumulated frame tension.  This
instrument decomposes a (12 h or 24 h) quiet-bed run into the channels that
could age, WITHOUT presupposing the mechanism:

  A. hourly consumer 3D / up (closed_loop_trace.csv) -- the verdict channel.
  B. hourly truth-graded producer channels (simnet_grade_series.csv):
     clk residual/contrast, cb, rx-dcb contrast, ZWD, phase med|frac|.
  C. hourly event-census rates (network_events.csv) -- frame tension as events.
  D. DD ambiguity truth error |e| stratified by CHORD AGE (network_dd_
     fractional.csv joined against fixture truth, the simnet_amb_integer join
     verbatim).  Age = epochs since the (signal, sat, sta_a, sta_b) chord was
     first traced (re-birth after a >1-epoch gap).  The within-hour young/old
     contrast controls for anything time-varying that is NOT age.
  E. per-station ZWD late drift (which stations pay, late-hour minus h_ref).

Join gate (same discipline as simnet_plateau_decompose): on the published
flagship this script must reproduce the campaign hourly consumer values
(h7 rms 0.107, h11 0.179) and grade.json's settled consumer rms before any
new number is trusted.

Usage:
  python scripts/simnet_drift_age.py <fixture_dir> --run <run_dir>
                                     [--settle-h 7] [--json out.json]
"""
from __future__ import annotations

import argparse
import bisect
import csv
import json
import math
import os
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from simnet_amb_integer import load_truth, n_at  # noqa: E402

EP = 30.0
AGE_BUCKETS = [(0, 10), (11, 60), (61, 120), (121, 240), (241, 480),
               (481, 960), (961, 10**9)]
YOUNG_MAX_EP = 120       # "young" chord: <= 1 h old
OLD_MIN_EP = 481         # "old" chord:   > 4 h old


def rms(v):
    return math.sqrt(sum(x * x for x in v) / len(v)) if v else float("nan")


def p50(v):
    return sorted(v)[len(v) // 2] if v else float("nan")


def p90(v):
    s = sorted(v)
    return s[min(len(s) - 1, int(round(0.9 * (len(s) - 1))))] if s else float("nan")


def hour_of(tow, t0):
    return int((tow - t0) // 3600.0)


# --------------------------------------------------------------------------- #
# A. consumer hourly
# --------------------------------------------------------------------------- #
def consumer_hourly(run_dir):
    p = os.path.join(run_dir, "logs", "closed_loop_trace.csv")
    per = defaultdict(lambda: ([], []))   # h -> (e3d, up)
    t0 = None
    with open(p, newline="") as f:
        for r in csv.DictReader(f):
            s = (r.get("pos_err_3d_m") or "").strip()
            if not s:
                continue
            tow = float(r["tow"])
            if t0 is None:
                t0 = tow
            h = hour_of(tow, t0)
            per[h][0].append(float(s))
            per[h][1].append(float(r["up_m"]))
    return {h: {"e3d_rms": rms(a), "e3d_p50": p50([abs(x) for x in a]),
                "up_rms": rms(b), "n": len(a)}
            for h, (a, b) in sorted(per.items())}, t0


# --------------------------------------------------------------------------- #
# B. graded channel hourly (long-format series)
# --------------------------------------------------------------------------- #
def series_hourly(run_dir):
    p = os.path.join(run_dir, "logs", "simnet_grade_series.csv")
    if not os.path.exists(p):
        return {}
    want_rms = {"clk_rms", "clk_contrast_rms", "cb_rms", "rxdcb_contrast_rms",
                "zwd_e"}
    want_med = {"phase_frac_med"}
    acc = defaultdict(lambda: defaultdict(list))   # chan -> h -> values
    t0 = None
    with open(p, newline="") as f:
        for r in csv.DictReader(f):
            ch = r.get("channel", "")
            if ch not in want_rms and ch not in want_med:
                continue
            try:
                tow = float(r["tow"]); v = float(r["value"])
            except (ValueError, KeyError):
                continue
            if t0 is None:
                t0 = tow
            acc[ch][hour_of(tow, t0)].append(v)
    out = {}
    for ch, hs in acc.items():
        agg = rms if ch in want_rms else (lambda v: p50([abs(x) for x in v]))
        out[ch] = {h: agg(vs) for h, vs in sorted(hs.items())}
    return out


# --------------------------------------------------------------------------- #
# C. event census hourly
# --------------------------------------------------------------------------- #
def events_hourly(run_dir, t0):
    p = os.path.join(run_dir, "logs", "network_events.csv")
    out = defaultdict(lambda: defaultdict(int))    # kind -> h -> count
    if not os.path.exists(p) or t0 is None:
        return out
    with open(p, newline="") as f:
        for r in csv.DictReader(f):
            k = r.get("event") or r.get("kind") or "NA"
            try:
                tow = float(r["tow"])
            except (ValueError, KeyError):
                continue
            out[k][hour_of(tow, t0)] += 1
    return out


# --------------------------------------------------------------------------- #
# D. DD truth error vs chord age (streamed -- the file can be ~3/4 GB)
# --------------------------------------------------------------------------- #
def dd_age(run_dir, fixture_dir, t0, settle_h):
    p = os.path.join(run_dir, "logs", "network_dd_fractional.csv")
    if not os.path.isfile(p):
        return None
    # Station set from the FIXTURE, not from a file prefix: the trace's early
    # epochs cover only a pivot-adjacent subset (measured: 21 of 56 stations in
    # the first 200k rows of the 12 h flagship), so a learned prefix silently
    # drops the majority of rows as "unknown station".
    stations = {d for d in os.listdir(fixture_dir)
                if os.path.isfile(os.path.join(fixture_dir, d,
                                               "truth_ambiguities.csv"))}
    n_int, slips = load_truth(fixture_dir, stations)

    birth = {}          # (sig, sat, sa, sb) -> birth tow
    last_seen = {}      # (sig, sat, sa, sb) -> last tow
    buckets = {b: [] for b in AGE_BUCKETS}          # settled rows only
    hour_age = defaultdict(list)                    # h -> ages (all rows)
    hour_young = defaultdict(list)                  # h -> |e| young
    hour_old = defaultdict(list)                    # h -> |e| old
    n_rows = n_drop = n_newsta = 0

    with open(p, newline="") as f:
        for r in csv.DictReader(f):
            tow = float(r["tow"])
            sig = r["signal"]; sat = r["sat_i"]; ref = r["ref_sat"]
            sa = r["sta_a"]; sb = r["sta_b"]
            key = (sig, sat, sa, sb)
            prev = last_seen.get(key)
            if prev is None or tow - prev > EP * 1.5:
                birth[key] = tow
            last_seen[key] = tow
            age_ep = (tow - birth[key]) / EP
            h = hour_of(tow, t0)
            hour_age[h].append(age_ep)
            if sa not in stations or sb not in stations:
                n_newsta += 1
                continue
            legs = [n_at(n_int, slips, sa, sat, sig, tow),
                    n_at(n_int, slips, sa, ref, sig, tow),
                    n_at(n_int, slips, sb, sat, sig, tow),
                    n_at(n_int, slips, sb, ref, sig, tow)]
            if any(v is None for v in legs):
                n_drop += 1
                continue
            dd_true = (legs[0] - legs[1]) - (legs[2] - legs[3])
            e = float(r["dd_cyc"]) - dd_true
            frac_trace = float(r["frac_cyc"])
            if (abs((e - round(e)) - frac_trace) > 2e-3 and
                    abs(abs(e - round(e)) - abs(frac_trace)) > 2e-3):
                n_drop += 1
                continue
            n_rows += 1
            ae = abs(e)
            if h >= settle_h:
                for (lo, hi) in AGE_BUCKETS:
                    if lo <= age_ep <= hi:
                        buckets[(lo, hi)].append(ae)
                        break
            if age_ep <= YOUNG_MAX_EP:
                hour_young[h].append(ae)
            elif age_ep >= OLD_MIN_EP:
                hour_old[h].append(ae)
    return {"buckets": buckets, "hour_age": hour_age,
            "hour_young": hour_young, "hour_old": hour_old,
            "n_rows": n_rows, "n_drop": n_drop, "n_newsta": n_newsta}


# --------------------------------------------------------------------------- #
# E. per-station ZWD late drift
# --------------------------------------------------------------------------- #
def zwd_stations(run_dir, settle_h):
    p = os.path.join(run_dir, "logs", "simnet_grade_series.csv")
    acc = defaultdict(lambda: defaultdict(list))   # sta -> h -> |zwd_e|
    t0 = None
    if not os.path.exists(p):
        return {}
    with open(p, newline="") as f:
        for r in csv.DictReader(f):
            if r.get("channel") != "zwd_e":
                continue
            tow = float(r["tow"])
            if t0 is None:
                t0 = tow
            acc[r.get("id", "?")][hour_of(tow, t0)].append(abs(float(r["value"])))
    return {sta: {h: rms(v) for h, v in sorted(hs.items())}
            for sta, hs in acc.items()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("fixture_dir")
    ap.add_argument("--run", required=True)
    ap.add_argument("--settle-h", type=int, default=7,
                    help="first settled hour for the age buckets (default 7)")
    ap.add_argument("--json", default=None)
    ap.add_argument("--skip-dd", action="store_true",
                    help="skip the (slow) DD age join")
    args = ap.parse_args()

    for st in (sys.stdout, sys.stderr):
        try:
            st.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, OSError):
            pass

    cons, t0 = consumer_hourly(args.run)
    ser = series_hourly(args.run)
    evs = events_hourly(args.run, t0)
    hours = sorted(cons)

    print("=" * 110)
    print(f"STABILITY drift-vs-age readout — run {args.run}")
    print(f"  arc t0 tow {t0:.0f}; {len(hours)} hours; settle-h {args.settle_h}")
    print("=" * 110)

    print("\n-- A/B. hourly channels (consumer + graded producer channels) --")
    chans = ["clk_rms", "clk_contrast_rms", "cb_rms", "rxdcb_contrast_rms",
             "zwd_e", "phase_frac_med"]
    hdr = f"  {'h':>3} {'con3D rms':>10} {'con3D p50':>10} {'up rms':>8}"
    hdr += "".join(f"{c:>14}" for c in chans)
    print(hdr)
    for h in hours:
        c = cons[h]
        line = (f"  {h:>3} {c['e3d_rms']:>10.3f} {c['e3d_p50']:>10.3f} "
                f"{c['up_rms']:>8.3f}")
        for ch in chans:
            v = ser.get(ch, {}).get(h, float("nan"))
            line += f"{v:>14.4f}"
        print(line)

    print("\n-- C. hourly event census (counts) --")
    kinds = sorted(evs, key=lambda k: -sum(evs[k].values()))
    print(f"  {'kind':<26}" + "".join(f"{h:>6}" for h in hours))
    for k in kinds:
        print(f"  {k:<26}" + "".join(f"{evs[k].get(h, 0):>6}" for h in hours))

    dd = None
    if not args.skip_dd:
        dd = dd_age(args.run, args.fixture_dir, t0, args.settle_h)
    if dd:
        print(f"\n-- D. DD truth error vs CHORD AGE (settled rows, h>={args.settle_h}; "
              f"{dd['n_rows']} rows joined, {dd['n_drop']} dropped, "
              f"{dd['n_newsta']} late-station) --")
        for (lo, hi) in AGE_BUCKETS:
            v = dd["buckets"][(lo, hi)]
            tag = f"{lo}-{hi if hi < 10**9 else 'inf'} ep"
            if not v:
                print(f"  age {tag:>12}: (no rows)")
                continue
            at_int = 100.0 * sum(1 for x in v if x <= 0.5 and round(x) == 0) / len(v)
            print(f"  age {tag:>12}: n={len(v):>8} med|e|={p50(v):7.3f} "
                  f"p90|e|={p90(v):8.3f}  round(e)=0 {at_int:5.1f}%")
        print("\n  per-hour chord-age population + young/old |e| contrast")
        print(f"  {'h':>3} {'age p50':>8} {'age p90':>8} "
              f"{'med|e| yng':>11} {'med|e| old':>11} {'n yng':>7} {'n old':>7}")
        for h in hours:
            ages = dd["hour_age"].get(h, [])
            y = dd["hour_young"].get(h, [])
            o = dd["hour_old"].get(h, [])
            print(f"  {h:>3} {p50(ages):>8.0f} {p90(ages):>8.0f} "
                  f"{p50(y):>11.3f} {p50(o):>11.3f} {len(y):>7} {len(o):>7}")

    zw = zwd_stations(args.run, args.settle_h)
    if zw:
        last_h = max(hours)
        # skip the final hour if it is the product-day-edge hour (partial/tainted)
        ref_h = min(args.settle_h, last_h)
        drift = []
        for sta, hs in zw.items():
            if ref_h in hs and last_h in hs:
                drift.append((hs[last_h] - hs[ref_h], sta, hs[ref_h], hs[last_h]))
        drift.sort(reverse=True)
        print(f"\n-- E. per-station ZWD |e| rms drift  h{ref_h} -> h{last_h} "
              f"(top 8 growers) --")
        for d, sta, a, b in drift[:8]:
            print(f"  {sta}: {a:7.4f} -> {b:7.4f}  (+{d:7.4f})")
        net = [d for d, *_ in drift]
        print(f"  network: median drift {p50(net):+.4f} m over {len(net)} stations")

    if args.json:
        out = {"consumer_hourly": cons, "series_hourly": ser,
               "events_hourly": {k: dict(v) for k, v in evs.items()},
               "t0": t0}
        if dd:
            out["dd_age_buckets"] = {
                f"{lo}-{hi}": {"n": len(v), "med": p50(v), "p90": p90(v)}
                for (lo, hi), v in dd["buckets"].items()}
            out["dd_hour"] = {
                h: {"age_p50": p50(dd["hour_age"].get(h, [])),
                    "young_med": p50(dd["hour_young"].get(h, [])),
                    "old_med": p50(dd["hour_old"].get(h, []))}
                for h in hours}
        if zw:
            out["zwd_stations"] = zw
        with open(args.json, "w") as f:
            json.dump(out, f, indent=1, default=lambda x: None if x != x else x)
        print(f"\nwrote {args.json}")


if __name__ == "__main__":
    main()
