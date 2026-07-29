#!/usr/bin/env python3
"""SPEC_day129_frame_guards Phase-1 trigger verdict.

Reads one instrumented leg's traces and answers the three falsifiable questions the
spec poses about an `a00` collapse onset (default: tow 286320 = 07:32:00 on
DOY-129):

  Q1a  does lambda_min of the reduced shared normals CRATER at onset, and does its
       eigenvector SPAN the clock and SH-VTEC families (the joint near-null slide of
       REPORT section 3.2)?
  Q1b  does the D5 re-tap / chord-birth count SPIKE at onset?
  Q1c  did a deweight ENGAGE/RELEASE wave land at onset?

Method: each channel is standardized against its own pre-onset baseline window with a
ROBUST z (median + 1.4826*MAD), because these series are heavy-tailed and a mean/sd
z would be dominated by whatever the tail already contains.  A channel is called
CONFIRMED only if the onset epoch exceeds `--k` robust sigma AND exceeds the largest
value anywhere in the baseline window -- the second test is what stops a merely
noisy channel reading as a trigger.

Any one channel confirmed closes Q1.  All quiet is reported as such: the standing
invariant is counters, not absence of crashes.

Usage:
  python scripts/p1_trigger_verdict.py --legdir runs/p1_leg [--onset 286320]
"""

import argparse
import csv
import os
import statistics
import sys

CHANNELS_STATUS = [
    ("n_d5_flags",      "D5 innovation flags",       "up"),
    ("n_d5_retaps",     "D5 fresh re-taps",          "up"),
    ("n_chord_births",  "chord births",              "up"),
    ("n_slip_refloats", "slip refloats",             "up"),
    ("n_dw_engaged",    "deweight engaged count",    "both"),
    ("n_dw_zero",       "deweight w=0 count",        "both"),
    ("dw_mean_w",       "deweight mean w",           "both"),
    ("sigma0_pf_code",  "post-fit sigma0 (code)",    "up"),
    ("worst_dev_m",     "worst clock deviation",     "up"),
    ("clock_std_m",     "clock spread (std)",        "up"),
]
# NOTE ev_min is the smallest eigenvalue of the PRE-datum reduced normals, so it
# legitimately includes the DECLARED gauge freedoms.  It is judged here as a
# RATIO to the rank gate's own absolute cutoff when that column is present (runs
# from 2026-07-26 onward); older files fall back to the raw eigenvalue.
CHANNELS_SPECTRUM = [
    ("ev_min",   "lambda_min of N_ss", "down"),
    ("ev_over_cutoff", "lambda_min / rank-gate cutoff", "down"),
    ("cond",     "condition number",   "up"),
    ("mass_clk", "smallest-mode mass on CLOCKS", "both"),
    ("mass_sh",  "smallest-mode mass on SH-VTEC", "both"),
    ("mass_pb",  "smallest-mode mass on PHASE BIAS", "both"),
]


def load(path):
    """Read a trace CSV, DROPPING any short/truncated row.

    These files are read while the producer is still appending to them, so the
    final line is routinely a partially-flushed fragment; csv.DictReader turns
    that into a row whose tail fields are None, which would otherwise crash the
    float() below or, worse, be silently treated as data.  Rows with fewer fields
    than the header are dropped and counted.
    """
    if not os.path.exists(path):
        return []
    with open(path, newline="") as fh:
        rd = csv.reader(fh)
        try:
            header = next(rd)
        except StopIteration:
            return []
        n, rows, short = len(header), [], 0
        for rec in rd:
            if len(rec) < n:
                short += 1
                continue
            rows.append(dict(zip(header, rec)))
    if short:
        print(f"  note: dropped {short} truncated row(s) from {os.path.basename(path)} "
              f"(file is being appended to)")
    return rows


def series(rows, col):
    out = []
    for r in rows:
        v = r.get(col, "")
        if v in ("", None):
            continue
        try:
            out.append((float(r["tow"]), float(v)))
        except (ValueError, KeyError):
            continue
    return out


def robust_z(baseline, value):
    """(value - median) / (1.4826*MAD); 0 when the baseline has no spread."""
    if not baseline:
        return 0.0, 0.0, 0.0
    med = statistics.median(baseline)
    mad = statistics.median([abs(x - med) for x in baseline])
    scale = 1.4826 * mad
    if scale <= 0.0:
        # Degenerate (constant) baseline: any departure is infinitely significant,
        # but say so honestly rather than dividing by zero.
        return (float("inf") if value != med else 0.0), med, 0.0
    return (value - med) / scale, med, scale


def report(title, rows, channels, onset, base_span, k):
    print(f"\n=== {title} ===")
    if not rows:
        print("  (file absent or empty)")
        return []
    verdicts = []
    print(f"  {'channel':<34} {'onset':>12} {'base med':>12} {'robust z':>10} "
          f"{'base max':>12}  verdict")
    for col, label, direction in channels:
        s = series(rows, col)
        if not s:
            print(f"  {label:<34} {'--':>12}   (column absent / empty)")
            continue
        base = [v for t, v in s if onset - base_span <= t < onset]
        at = [v for t, v in s if abs(t - onset) < 1.0]
        if not base or not at:
            print(f"  {label:<34} {'--':>12}   (no baseline or no onset epoch in this leg)")
            continue
        v = at[0]
        z, med, scale = robust_z(base, v)
        bmax, bmin = max(base), min(base)
        exceeds = (v > bmax) if direction == "up" else \
                  (v < bmin) if direction == "down" else (v > bmax or v < bmin)
        hit = abs(z) >= k and exceeds
        verdicts.append((label, hit, z, v, med))
        print(f"  {label:<34} {v:12.4f} {med:12.4f} {z:10.2f} {bmax:12.4f}  "
              f"{'*** CONFIRMED ***' if hit else 'quiet'}")
    return verdicts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--legdir", required=True)
    ap.add_argument("--onset", type=float, default=286320.0,
                    help="collapse onset tow (default 286320 = 07:32:00 DOY-129)")
    ap.add_argument("--baseline-span", type=float, default=1800.0,
                    help="seconds of pre-onset data used as the baseline")
    ap.add_argument("--k", type=float, default=4.0,
                    help="robust-sigma threshold for CONFIRMED")
    args = ap.parse_args()

    logs = os.path.join(args.legdir, "logs")
    st = load(os.path.join(logs, "network_epoch_status.csv"))
    sp = load(os.path.join(logs, "network_sh_spectrum.csv"))
    ev = load(os.path.join(logs, "network_events.csv"))

    print(f"leg      {args.legdir}")
    print(f"onset    tow {args.onset:.0f}")
    print(f"baseline {args.baseline_span:.0f} s before onset, robust z, k={args.k}")
    print(f"rows     epoch_status {len(st)}, sh_spectrum {len(sp)}, events {len(ev)}")

    v1 = report("Q1b/Q1c — per-epoch carry churn, deweight state, post-fit sigma0",
                st, CHANNELS_STATUS, args.onset, args.baseline_span, args.k)
    # Derived channel: lambda_min relative to the gate's own cutoff (see the note
    # on CHANNELS_SPECTRUM).  Skipped for pre-cutoff-column files.
    for r in sp:
        try:
            cut = float(r.get("cutoff", "") or "nan")
            r["ev_over_cutoff"] = repr(float(r["ev_min"]) / cut) if cut > 0 else ""
        except (ValueError, KeyError, ZeroDivisionError):
            r["ev_over_cutoff"] = ""
    v2 = report("Q1a — reduced shared-normal spectrum",
                sp, CHANNELS_SPECTRUM, args.onset, args.baseline_span, args.k)

    # Deweight transition wave: count IONO_DW rows in the onset epoch and just before.
    print("\n=== Q1c — IONO_DW engage/release transitions near onset ===")
    if ev:
        win = {}
        for r in ev:
            if r.get("tier") != "IONO_DW":
                continue
            try:
                t = float(r["tow"])
            except (ValueError, KeyError):
                continue
            if args.onset - args.baseline_span <= t <= args.onset + 300.0:
                win.setdefault(t, []).append(r.get("event", "?"))
        if not win:
            print("  no IONO_DW transitions in the window")
        else:
            for t in sorted(win):
                eng = sum(1 for e in win[t] if e == "engage")
                rel = sum(1 for e in win[t] if e == "release")
                flag = "  <== ONSET" if abs(t - args.onset) < 1.0 else ""
                print(f"  tow {t:9.0f}  engage {eng:3d}  release {rel:3d}{flag}")
    else:
        print("  network_events.csv absent or empty")

    confirmed = [lbl for lbl, hit, *_ in (v1 + v2) if hit]
    print("\n=== VERDICT ===")
    if confirmed:
        print("  Q1 CLOSED — trigger channel(s) confirmed at onset:")
        for c in confirmed:
            print(f"    - {c}")
    else:
        print("  ALL QUIET at onset in every instrumented channel.")
        print("  Report this honestly: no channel here identifies the trigger.")
        print("  It does NOT mean nothing happened - it means the trigger is not in")
        print("  {carry churn, deweight state, shared-normal spectrum, post-fit sigma0},")
        print("  and the next instrumentation pass must look elsewhere.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
