#!/usr/bin/env python3
"""SPEC_s2z_realization_ab Phase-2 grading — P1(a,c) + P2 (wire damage).

All statistics datum-invariant (standing invariant #5 is doubly binding: the
two arms' raw frames differ by construction).  Grades ONE leg directory and,
optionally, prints the same statistics for a reference leg for side-by-side
reading — never a raw-frame difference across realizations.

  P1(a)  constraint check: per-epoch clock_median_m inter-epoch delta at each
         onset (network_epoch_status.csv).  Under s2z the solved sat-clock
         median cannot jump at 334020 (the Sigma row holds); a jump means the
         arm is misconfigured -> STOP.
  P1(c)  sigma0_pf_code jump at each onset vs its own pre-onset baseline
         (network_epoch_status.csv, robust z).
  P2     between-satellite total-IF deviation-from-median level bands
         (network_solution.csv): per-sat EOD offset vs the pre-20:47 quiet
         band, worst-sat and G01 explicit, out-of-band satellite count
         (H1 predicts ~0; H2 predicts a distributed multi-sat shift), plus
         per-(signal) CB channel-sum trajectories (hourly medians).

Usage:
    python scripts/s2z_p2_grade.py --legdir runs/s2z_ab/leg01
        [--events 328890 334020] [--quiet-band 330300 333990] [--pivot G01]
"""
import argparse
import csv
import os
import statistics
import sys
from collections import defaultdict

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

DAY_ANCHOR_TOW = 259200.0
EPOCH_S = 30.0
C1 = 2.545728
C2 = 1.545728
L1_CODES = ("C1W", "C1C", "C1X", "C1P")
L2_CODES = ("C2W", "C2C", "C2X", "C2P", "C2L", "C2S")


def hms(tow):
    s = int(round(tow - DAY_ANCHOR_TOW))
    return f"{s // 3600:02d}:{(s % 3600) // 60:02d}:{s % 60:02d}"


def robust(vals):
    if not vals:
        return 0.0, 0.0
    med = statistics.median(vals)
    mad = statistics.median([abs(v - med) for v in vals])
    return med, 1.4826 * mad


def load_solution(path):
    """network_solution.csv -> (ordered [(tow, {sat: total_if})], {tow: {sig: sum_cb}})."""
    clk = defaultdict(dict)
    cb = defaultdict(lambda: defaultdict(dict))
    with open(path, newline="") as fh:
        for r in csv.DictReader(fh):
            try:
                tow, sat, kind, val = (float(r["tow"]), r["sat"], r["kind"],
                                       float(r["value_m"]))
            except (ValueError, KeyError, TypeError):
                continue
            if kind == "CLK":
                clk[tow][sat] = val
            elif kind == "CB":
                cb[tow][sat][r.get("signal", "")] = val
    epochs = []
    chan_sum = {}
    for tow in sorted(clk):
        tot = {}
        for sat, c in clk[tow].items():
            sigs = cb[tow].get(sat, {})
            l1 = next((sigs[s] for s in L1_CODES if s in sigs), None)
            l2 = next((sigs[s] for s in L2_CODES if s in sigs), None)
            if l1 is not None and l2 is not None:
                tot[sat] = c + C1 * l1 - C2 * l2
        if len(tot) >= 8:
            epochs.append((tow, tot))
        sums = defaultdict(float)
        for sat, sigs in cb[tow].items():
            for sig, v in sigs.items():
                sums[sig] += v
        chan_sum[tow] = dict(sums)
    return epochs, chan_sum


def load_epoch_status(path):
    rows = []
    with open(path, newline="") as fh:
        for r in csv.DictReader(fh):
            try:
                rows.append((float(r["tow"]), r))
            except (ValueError, KeyError, TypeError):
                continue
    return rows


def grade(legdir, events, quiet_band, pivot):
    logs = os.path.join(legdir, "logs")
    print(f"=== s2z A/B P1(a,c) + P2 — {legdir} ===\n")

    # ---- P1(a): clock-median continuity at each onset ----------------------
    es = load_epoch_status(os.path.join(logs, "network_epoch_status.csv"))
    med_of = {t: float(r["clock_median_m"]) for t, r in es if r.get("clock_median_m")}
    towlist = sorted(med_of)
    deltas = [abs(med_of[b] - med_of[a]) for a, b in zip(towlist, towlist[1:])]
    dmed, dsig = robust(deltas)
    print("--- P1(a): clock_median_m inter-epoch continuity ---")
    print(f"  leg-wide |d(clock_median)|: median {dmed:.4f} m, robust sigma {dsig:.4f} m")
    for onset in events:
        prev = onset - EPOCH_S
        if onset in med_of and prev in med_of:
            d = med_of[onset] - med_of[prev]
            flag = "OK (Sigma row held)" if abs(d) < max(0.5, 10 * dsig) else \
                   "*** JUMPED — misconfigured arm? STOP ***"
            print(f"  {hms(onset)}: median {med_of[prev]:+.3f} -> {med_of[onset]:+.3f} "
                  f"(delta {d:+.4f} m)  {flag}")
        else:
            print(f"  {hms(onset)}: epoch(s) missing from status trace")
    print()

    # ---- P1(a2): the EXACT constraint — per-epoch mean of solved CLK -------
    # Sigma dtks = 0 per constellation constrains the MEAN exactly (the median
    # only through symmetry).  Under s2z this must sit at ~0 every epoch
    # (soft-constraint residual, mm-level); under the pin it is the free
    # common-mode.  The definitive "this arm really runs s2z" check.
    sol_path0 = os.path.join(logs, "network_solution.csv")
    clk_mean = {}
    if os.path.isfile(sol_path0):
        per_tow = defaultdict(list)
        with open(sol_path0, newline="") as fh:
            for r in csv.DictReader(fh):
                if r.get("kind") == "CLK":
                    try:
                        per_tow[float(r["tow"])].append(float(r["value_m"]))
                    except (ValueError, TypeError):
                        continue
        clk_mean = {t: sum(v) / len(v) for t, v in per_tow.items() if v}
    if clk_mean:
        absmeans = sorted(abs(v) for v in clk_mean.values())
        q50 = absmeans[len(absmeans) // 2]
        qmax = absmeans[-1]
        print("--- P1(a2): per-epoch mean of solved sat clocks (exact Sigma check) ---")
        print(f"  |mean CLK|: median {q50:.6f} m, max {qmax:.6f} m over "
              f"{len(clk_mean)} epochs  "
              + ("(~0: s2z constraint HELD)" if qmax < 0.01 else
                 "(NON-ZERO: pin-frame common mode / constraint NOT held)"))
        for onset in events:
            if onset in clk_mean and (onset - EPOCH_S) in clk_mean:
                print(f"  {hms(onset)}: mean {clk_mean[onset - EPOCH_S]:+.6f} -> "
                      f"{clk_mean[onset]:+.6f} m")
        print()

    # ---- P1(c): sigma0_pf_code at each onset -------------------------------
    s0 = {t: float(r["sigma0_pf_code"]) for t, r in es
          if r.get("sigma0_pf_code") not in (None, "")}
    print("--- P1(c): sigma0_pf_code onset jump (own-baseline robust z) ---")
    for onset in events:
        base = [v for t, v in s0.items() if t < onset - EPOCH_S]
        if onset not in s0 or len(base) < 10:
            print(f"  {hms(onset)}: insufficient data")
            continue
        bmed, bsig = robust(base)
        z = (s0[onset] - bmed) / bsig if bsig > 1e-9 else float("nan")
        post = [s0.get(onset + k * EPOCH_S) for k in range(1, 6)]
        post_txt = " ".join(f"{v:.2f}" for v in post if v is not None)
        print(f"  {hms(onset)}: {bmed:.2f} -> {s0[onset]:.2f} (z={z:.0f}); next 5 ep: {post_txt}")
    print()

    # ---- P2: between-sat total-IF level bands ------------------------------
    sol_path = os.path.join(logs, "network_solution.csv")
    epochs, chan_sum = load_solution(sol_path)
    if not epochs:
        print("P2: network_solution.csv empty — cannot grade")
        return
    dev = defaultdict(dict)          # sat -> {tow: dev}
    for tow, tot in epochs:
        med = statistics.median(tot.values())
        for sat, v in tot.items():
            dev[sat][tow] = v - med
    qlo, qhi = quiet_band
    eod = epochs[-1][0]
    print(f"--- P2: per-sat EOD offset vs quiet band [{hms(qlo)}..{hms(qhi)}] "
          f"(EOD = {hms(eod)}) ---")
    rows = []
    for sat, sv in sorted(dev.items()):
        band = [v for t, v in sv.items() if qlo <= t <= qhi]
        if len(band) < 10 or eod not in sv:
            continue
        bmed, bsig = robust(band)
        off = sv[eod] - bmed
        thr = max(2.0, 3.0 * bsig)
        rows.append((abs(off), sat, off, bmed, bsig, thr, abs(off) > thr))
    rows.sort(reverse=True)
    n_out = sum(1 for r in rows if r[6])
    print(f"  {len(rows)} sats graded; {n_out} outside max(2 m, 3 sigma) of own quiet band")
    print(f"  {'sat':>4s} {'EODoff_m':>9s} {'band_med':>9s} {'band_sig':>9s}  flag")
    shown = set()
    for absoff, sat, off, bmed, bsig, thr, out in rows[:8]:
        print(f"  {sat:>4s} {off:+9.2f} {bmed:+9.2f} {bsig:9.2f}  {'OUT' if out else 'in'}")
        shown.add(sat)
    if pivot not in shown:
        for absoff, sat, off, bmed, bsig, thr, out in rows:
            if sat == pivot:
                print(f"  {sat:>4s} {off:+9.2f} {bmed:+9.2f} {bsig:9.2f}  "
                      f"{'OUT' if out else 'in'}  (reference-arm concentration sat)")
    # between-sat spread trajectory, hourly
    print("\n  hourly between-sat spread (robust sigma of devs) + worst sat:")
    by_hour = defaultdict(list)
    for tow, tot in epochs:
        by_hour[int((tow - DAY_ANCHOR_TOW) // 3600)].append((tow, tot))
    for h in sorted(by_hour):
        vals = []
        worst = ("", 0.0)
        for tow, tot in by_hour[h]:
            med = statistics.median(tot.values())
            for sat, v in tot.items():
                d = v - med
                vals.append(d)
                if abs(d) > abs(worst[1]):
                    worst = (sat, d)
        _, sig = robust(vals)
        print(f"    hr {h:02d}: sigma {sig:7.2f} m   worst {worst[0]} {worst[1]:+8.2f} m")

    # ---- P2: CB channel sums ----------------------------------------------
    print("\n--- P2: CB channel-sum trajectories (hourly median of Sigma_sats CB) ---")
    sigs = sorted({s for sums in chan_sum.values() for s in sums})
    hours = sorted({int((t - DAY_ANCHOR_TOW) // 3600) for t in chan_sum})
    print("    hr   " + "".join(f"{s:>12s}" for s in sigs))
    for h in hours:
        cells = []
        for s in sigs:
            vals = [sums[s] for t, sums in chan_sum.items()
                    if int((t - DAY_ANCHOR_TOW) // 3600) == h and s in sums]
            cells.append(f"{statistics.median(vals):12.3f}" if vals else f"{'-':>12s}")
        print(f"    {h:02d}  " + "".join(cells))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--legdir", required=True)
    ap.add_argument("--events", nargs="*", type=float, default=[328890.0, 334020.0])
    ap.add_argument("--quiet-band", nargs=2, type=float, default=[330300.0, 333990.0],
                    help="pre-20:47 band excluding the 19:21 transient "
                         "(default 19:45:00..20:46:30)")
    ap.add_argument("--pivot", default="G01",
                    help="reference-arm concentration satellite to always report")
    args = ap.parse_args()
    grade(os.path.abspath(args.legdir), args.events, tuple(args.quiet_band), args.pivot)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
