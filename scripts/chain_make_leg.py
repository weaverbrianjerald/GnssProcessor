#!/usr/bin/env python3
"""Generate one leg config for the DOY-129 continuous/chained network run.

Derives ``network.start_offset_hours`` from a snapshot's ``M`` line using the
self-correcting recurrence, so a leg always resumes on the epoch immediately
after the last one the snapshot actually recorded -- whether the previous run
finished cleanly or was killed mid-leg.

    start_offset_hours = (snap_week*604800 + snap_tow - DAY_ANCHOR + 15) / 3600

The +15 s is a half-epoch back-off: epoch elapsed times carry up to ~2e-05 s of
double-precision residue (TimeUtils builds them through a Julian-date divide),
and the fast-forward gate at ApplicationNetwork.cpp:1011 is a strict ``<``.
Landing 15 s below the target epoch gives ~6 orders of magnitude of margin, and
is nowhere near the previous epoch 30 s back.

Only fingerprint-safe keys are touched (verified against the fingerprint
construction at NetworkRunContext.cpp:172-305).  The station roster and its
order, carry_invariant, state_snapshot_carry*, estimate_iono_sh, every q/floor,
apriori_source, epoch_decimation_n and the datum station are all IN the
fingerprint -- changing any of them hard-aborts every subsequent leg.

Usage:
    python scripts/chain_make_leg.py --snapshot <in.txt> --legdir <dir> \
        [--base config/test_suite/_night305_resume.json] [--max-duration 86400]
"""
import argparse
import json
import os
import shutil
import sys

# First synced epoch of DOY-129 = 2024-05-08 00:00:00 GPS, in continuous GPS
# seconds (week 2313, tow 259200).  This is the anchor the epoch loop latches
# on -- it is NOT restored from the snapshot, so it is always the file start.
DAY_ANCHOR_S = 1399161600.0
EPOCH_INTERVAL_S = 30.0
HALF_EPOCH_BACKOFF_S = 15.0


def read_snapshot_header(path):
    """Return (week, tow, n_emitted, fingerprint) from a snapshot's M/N lines."""
    week = tow = n_emitted = None
    fingerprint = None
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if line.startswith("M,"):
                f = line.rstrip("\n").split(",")
                week, tow, fingerprint = int(f[1]), float(f[2]), f[5]
            elif line.startswith("N,"):
                n_emitted = int(line.split(",")[1])
                break
    if week is None or tow is None or n_emitted is None:
        raise SystemExit(f"FATAL: {path} has no readable M/N header")
    return week, tow, n_emitted, fingerprint


def start_offset_hours(week, tow):
    """Self-correcting recurrence -> offset that lands on snapshot_epoch + 1."""
    continuous_s = week * 604800.0 + tow
    return (continuous_s - DAY_ANCHOR_S + HALF_EPOCH_BACKOFF_S) / 3600.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--snapshot", required=True, help="snapshot to resume FROM")
    ap.add_argument("--legdir", required=True, help="leg working directory")
    ap.add_argument("--base", default="config/test_suite/_night305_resume.json")
    ap.add_argument("--max-duration", type=float, default=86400.0,
                    help="processing.max_duration_s; 86400 runs to end of data")
    ap.add_argument("--frame-guards", action="store_true",
                    help="arm the SPEC_day129_frame_guards Phase-1 trigger "
                         "instrumentation (trace_sh_spectrum + trace_postfit_sigma0 "
                         "+ trace_dd_fractional).  All three are TRACE-ONLY and "
                         "absent from the resume fingerprint, so they may be armed "
                         "on a leg resuming an untraced snapshot.")
    ap.add_argument("--sh-spectrum-stride", type=int, default=10,
                    help="network.trace_sh_spectrum_stride (mode-detail cadence)")
    ap.add_argument("--dd-frac-max-pairs", type=int, default=500,
                    help="network.trace_dd_fractional_max_pairs (bounds the "
                         "dominant 305-station tracer cost)")
    args = ap.parse_args()

    week, tow, n_emitted, fingerprint = read_snapshot_header(args.snapshot)
    offset_h = start_offset_hours(week, tow)

    legdir = os.path.abspath(args.legdir)
    os.makedirs(legdir, exist_ok=True)
    os.makedirs(os.path.join(legdir, "logs"), exist_ok=True)

    # The leg reads AND writes its OWN snapshot -- never the file it was seeded
    # from, so a killed leg can never destroy the state it resumed from.
    leg_snap = os.path.join(legdir, "snap.txt")
    if os.path.abspath(args.snapshot) != leg_snap:
        shutil.copyfile(args.snapshot, leg_snap)

    with open(args.base, "r", encoding="utf-8") as fh:
        cfg = json.load(fh)

    cfg["processing"]["max_duration_s"] = args.max_duration
    net = cfg["network"]
    net["start_offset_hours"] = offset_h
    net["state_snapshot_path"] = leg_snap.replace("\\", "/")
    net["state_snapshot_require"] = True          # never silently cold-start
    net["state_snapshot_keep_history"] = False    # 0.55 MB x every epoch otherwise
    # B5: no product stream was being written at all -- "" in both stock configs.
    net["product_output_path"] = os.path.join(legdir, "products.txt").replace("\\", "/")
    # Observability (all fingerprint-safe, all default-off).
    net["write_run_manifest"] = True
    net["trace_epoch_status"] = True
    net["trace_events"] = True
    net["trace_flag_rates"] = True
    net["health_interval_s"] = 300.0
    # SPEC_day129_frame_guards Phase 1.  Every key below is verified ABSENT from
    # the fingerprint construction (NetworkRunContext.cpp:172-307), so arming them
    # on a resume against the untraced 6 h seed is legal.
    if args.frame_guards:
        net["trace_sh_spectrum"] = True
        net["trace_sh_spectrum_stride"] = args.sh_spectrum_stride
        net["trace_postfit_sigma0"] = True
        net["trace_postfit_keyed"] = True            # §4.3 RHS localization (STA/SAT/SIG pools)
        net["trace_dd_fractional"] = True            # P1-5 (closes handoff Q6 measurability)
        net["trace_dd_fractional_max_pairs"] = args.dd_frac_max_pairs
        net["trace_dd_fractional_epoch_stride"] = 1

    cfg_path = os.path.join(legdir, "leg.json")
    with open(cfg_path, "w", encoding="utf-8") as fh:
        json.dump(cfg, fh, indent=2)

    first_tow = tow + EPOCH_INTERVAL_S
    print(f"legdir            {legdir}")
    print(f"seed snapshot     {args.snapshot}")
    print(f"  week/tow        {week} / {tow!r}")
    print(f"  n_emitted       {n_emitted}")
    print(f"  fingerprint     {fingerprint}")
    print(f"start_offset_hours {offset_h!r}")
    print(f"max_duration_s    {args.max_duration}")
    print(f"expected first ep tow {first_tow!r}  (snapshot age must log as 30.0 s)")
    if args.frame_guards:
        print("frame-guards      ON (trace_sh_spectrum stride "
              f"{args.sh_spectrum_stride}, trace_postfit_sigma0, "
              f"trace_dd_fractional max_pairs {args.dd_frac_max_pairs})")
    print(f"config            {cfg_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
