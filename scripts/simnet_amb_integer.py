#!/usr/bin/env python3
"""SYNTHETIC MINIMUM NETWORK — the ambiguity-INTEGER grader
(docs/HANDOFF_slant_triple_next.md §3; SPEC_synthetic_minimum_network §13).

The first absolute integer test the network has ever had.  The field can only
measure the FRACTIONAL part of a DD ambiguity (med|frac| 0.248 says "no integer
structure" but cannot say why); on the sim fixture the true integers are known
per (station, satellite, signal), so the graded quantity is

    e = dd_cyc_est − DD_true          (cycles, FULL value — not the fractional part)

with  DD_true = [N(a,i) − N(a,ref)] − [N(b,i) − N(b,ref)]  (+ cumulative
truth_slips deltas up to the epoch), formed in PYTHON INTS before any float
touches it — the integers are km-scale by construction (N ∈ [1e5, 1e6): the sim
draws them unguessable for the MW/NL exercise), so float differencing would
grade cancellation error, not the estimator.

Lane combinations are formed from matched per-band DDs on the SAME
(sat, ref, sta_a, sta_b) quadruple in cycle-difference form:

    e_WL(1,2)  = e_L1C − e_L2W        N_WL  = N1 − N2  (integer by construction)
    e_EWL(2,5) = e_L2W − e_L5Q        N_EWL = N2 − N5  (integer by construction)

Iono sensitivity per metre of L1 DD-iono error in these units: L1 5.26 cyc,
WL 1.49 cyc, EWL 0.29 cyc — an 18× ladder, which is the cascade the third band
opens.  The pre-registered expected shape (handoff §3.3): EWL near-integer, WL
plausible, NL not — the value is WHICH RUNG BREAKS.

Reads:  <run>/logs/network_dd_fractional.csv   (trace_dd_fractional, stride 1)
        <fixture>/<station>/truth_ambiguities.csv   (satellite,signal,N_int,wavelength_m)
        <fixture>/<station>/truth_slips.csv         (week,tow_s,satellite,signal,delta_cycles,…)

Usage:
    python scripts/simnet_amb_integer.py runs/simnet60_tri --run runs/simnet60_tri_slant_1h_ddint/run
    python scripts/simnet_amb_integer.py --selftest
"""

import argparse
import bisect
import csv
import os
import sys
from collections import defaultdict


def load_truth(fixture_dir, stations):
    """Per-station truth: N[(sta, sat, sig)] -> int, and per-key sorted slip
    lists [(tow, cum_delta_after)] for cumulative lookup."""
    n_int = {}
    slips = defaultdict(list)          # (sta, sat, sig) -> [(tow, delta_int), ...]
    for sta in sorted(stations):
        amb_path = os.path.join(fixture_dir, sta, "truth_ambiguities.csv")
        if not os.path.isfile(amb_path):
            raise SystemExit(f"missing truth file: {amb_path}")
        with open(amb_path, newline="") as f:
            for row in csv.DictReader(f):
                n_int[(sta, row["satellite"], row["signal"])] = int(row["N_int"])
        slip_path = os.path.join(fixture_dir, sta, "truth_slips.csv")
        if os.path.isfile(slip_path):
            with open(slip_path, newline="") as f:
                for row in csv.DictReader(f):
                    slips[(sta, row["satellite"], row["signal"])].append(
                        (float(row["tow_s"]), int(round(float(row["delta_cycles"])))))
    for k in slips:
        slips[k].sort()
    return n_int, slips


def n_at(n_int, slips, sta, sat, sig, tow):
    """True integer at epoch `tow`: pre-slip N_int + Σ deltas with tow_s <= tow.
    Returns None if the (sta, sat, sig) has no truth row (should not happen)."""
    base = n_int.get((sta, sat, sig))
    if base is None:
        return None
    ss = slips.get((sta, sat, sig))
    if ss:
        tows = [t for t, _ in ss]
        k = bisect.bisect_right(tows, tow)
        base += sum(d for _, d in ss[:k])
    return base


def stats(errs):
    """(n, median e, median |e|, p90 |e|, %|e|<=0.10, %|e|<=0.25, %round(e)==0)"""
    if not errs:
        return None
    s = sorted(errs)
    a = sorted(abs(e) for e in errs)
    n = len(errs)

    def q(v, p):
        i = min(n - 1, max(0, int(round(p * (n - 1)))))
        return v[i]

    within = lambda t: 100.0 * sum(1 for e in a if e <= t) / n
    int0 = 100.0 * sum(1 for e in errs if round(e) == 0) / n
    return (n, q(s, 0.5), q(a, 0.5), q(a, 0.9), within(0.10), within(0.25), int0)


def fmt_row(label, st):
    if st is None:
        return f"  {label:<12} (no rows)"
    n, med, meda, p90, w10, w25, i0 = st
    return (f"  {label:<12} n={n:<7d} med(e)={med:+8.3f}  med|e|={meda:7.3f}  "
            f"p90|e|={p90:8.3f}  <=0.10cyc {w10:5.1f}%  <=0.25cyc {w25:5.1f}%  "
            f"round(e)=0 {i0:5.1f}%")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("fixture_dir", nargs="?")
    ap.add_argument("--run", required=False, help="producer run dir (its logs/ holds network_dd_fractional.csv)")
    ap.add_argument("--last-frac", type=float, default=0.25,
                    help="also report the LAST this fraction of epochs (default 0.25)")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if not args.fixture_dir or not args.run:
        ap.error("fixture_dir and --run are required (or --selftest)")

    dd_path = os.path.join(args.run, "logs", "network_dd_fractional.csv")
    if not os.path.isfile(dd_path):
        raise SystemExit(f"missing {dd_path} — arm network.trace_dd_fractional")

    rows = []
    with open(dd_path, newline="") as f:
        for row in csv.DictReader(f):
            rows.append(row)
    if not rows:
        raise SystemExit("DD trace is empty")

    stations = {r["sta_a"] for r in rows} | {r["sta_b"] for r in rows}
    n_int, slips = load_truth(args.fixture_dir, stations)
    n_slip_events = sum(len(v) for v in slips.values())

    # --- per-band errors --------------------------------------------------
    tows = sorted({float(r["tow"]) for r in rows})
    t_last = tows[int(len(tows) * (1.0 - args.last_frac))] if len(tows) > 1 else tows[0]
    band_all = defaultdict(list)
    band_last = defaultdict(list)
    # (tow, sat, ref, sta_a, sta_b) -> {signal: e}  for the lane matching
    quad = defaultdict(dict)
    n_unmatched = 0
    for r in rows:
        tow = float(r["tow"])
        sat, ref = r["sat_i"], r["ref_sat"]
        sa, sb = r["sta_a"], r["sta_b"]
        sig = r["signal"]
        legs = [n_at(n_int, slips, sa, sat, sig, tow),
                n_at(n_int, slips, sa, ref, sig, tow),
                n_at(n_int, slips, sb, sat, sig, tow),
                n_at(n_int, slips, sb, ref, sig, tow)]
        if any(v is None for v in legs):
            n_unmatched += 1
            continue
        dd_true = (legs[0] - legs[1]) - (legs[2] - legs[3])     # python ints
        e = float(r["dd_cyc"]) - dd_true
        # sanity: our e and the trace's own frac agree modulo integers
        frac_trace = float(r["frac_cyc"])
        if abs((e - round(e)) - frac_trace) > 2e-3 and abs(abs(e - round(e)) - abs(frac_trace)) > 2e-3:
            # tolerate the .4f rounding and the round-half seam; a real mismatch
            # means the truth join is wrong for this row
            n_unmatched += 1
            continue
        band_all[sig].append(e)
        if tow >= t_last:
            band_last[sig].append(e)
        quad[(tow, sat, ref, sa, sb)][sig] = e

    # --- lane combinations on matched quadruples --------------------------
    lanes = {"EWL(2,5)": ("L2W", "L5Q"), "WL(1,2)": ("L1C", "L2W")}
    lane_all = defaultdict(list)
    lane_last = defaultdict(list)
    for (tow, *_), sigs in quad.items():
        for lane, (s1, s2) in lanes.items():
            if s1 in sigs and s2 in sigs:
                e = sigs[s1] - sigs[s2]
                lane_all[lane].append(e)
                if tow >= t_last:
                    lane_last[lane].append(e)

    print("=" * 78)
    print("AMBIGUITY-INTEGER GRADE — DD estimates vs KNOWN true integers (cycles)")
    print("=" * 78)
    print(f"  run       : {args.run}")
    print(f"  rows      : {len(rows)} traced, {n_unmatched} dropped (no truth / frac mismatch)")
    print(f"  epochs    : {len(tows)}  (last-window cut at tow >= {t_last:.0f}, "
          f"= final {args.last_frac:.0%})")
    print(f"  slips     : {n_slip_events} truth slip event(s) applied "
          f"({'quiet fixture' if n_slip_events == 0 else 'CUMULATIVE deltas active'})")
    print()
    print("-- per band (NL-class: the raw uncombined DD against its own integer) --")
    for sig in sorted(band_all):
        print(fmt_row(sig, stats(band_all[sig])))
    print(f"-- same, LAST {args.last_frac:.0%} of epochs only --")
    for sig in sorted(band_last):
        print(fmt_row(sig, stats(band_last[sig])))
    print()
    print("-- lane combinations (matched quadruples, cycle-difference form) --")
    print("   iono sensitivity per metre of L1 DD-iono error: L1-band 5.26 cyc, "
          "WL(1,2) 1.49, EWL(2,5) 0.29")
    for lane in lanes:
        print(fmt_row(lane, stats(lane_all[lane])))
    print(f"-- same, LAST {args.last_frac:.0%} of epochs only --")
    for lane in lanes:
        print(fmt_row(lane, stats(lane_last[lane])))
    print()
    print("  READING GUIDE: 'round(e)=0' is the absolute question (near ITS OWN")
    print("  integer); 'med|e|' bounds the whole error including integer offsets —")
    print("  a fractional-part test can never separate the two, which is the point.")
    print("=" * 78)
    return 0


def selftest():
    """Exercise the integer join + slip accumulation + lane matching on a
    hand-built mini-case."""
    import tempfile
    ok = True
    with tempfile.TemporaryDirectory() as td:
        for sta, base in (("SAAA", 100000), ("SBBB", 200000)):
            os.makedirs(os.path.join(td, sta))
            with open(os.path.join(td, sta, "truth_ambiguities.csv"), "w") as f:
                f.write("satellite,signal,N_int,wavelength_m\n")
                for sat, off in (("G01", 0), ("G02", 7)):
                    for sig, boff in (("L1C", 0), ("L2W", 3)):
                        f.write(f"{sat},{sig},{base + off + boff},0.19\n")
            with open(os.path.join(td, sta, "truth_slips.csv"), "w") as f:
                f.write("week,tow_s,satellite,signal,delta_cycles,flagged,elev_deg\n")
                if sta == "SBBB":
                    f.write("2313,150.0,G02,L1C,5,1,45.0\n")
        n_int, slips = load_truth(td, {"SAAA", "SBBB"})
        # before the slip: DD_true = [(A,G02)-(A,G01)] - [(B,G02)-(B,G01)] = 7-7 = 0
        legs = lambda tow, sig: ((n_at(n_int, slips, "SAAA", "G02", sig, tow)
                                  - n_at(n_int, slips, "SAAA", "G01", sig, tow))
                                 - (n_at(n_int, slips, "SBBB", "G02", sig, tow)
                                    - n_at(n_int, slips, "SBBB", "G01", sig, tow)))
        ok &= legs(100.0, "L1C") == 0
        # after the slip on (SBBB, G02, L1C): DD_true = 0 - 5 = -5
        ok &= legs(200.0, "L1C") == -5
        ok &= legs(200.0, "L2W") == 0          # other band untouched
        # stats: round(e)==0 share
        st = stats([0.02, -0.10, 0.90, 1.04])
        ok &= st[0] == 4 and abs(st[6] - 50.0) < 1e-9
    print("selftest:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
