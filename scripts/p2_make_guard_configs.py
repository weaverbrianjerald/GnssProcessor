#!/usr/bin/env python3
"""Generate the SPEC_day129_frame_guards Phase-2 armed configs.

Derives config/test_suite/_day129_guards_conv.json from _night305_conv.json by
adding the arm-together guard set (handoff §7 / spec §2.2) and re-anchoring the
6 h convergence seed at 13:00 GPS so the follow-on leg (19:00 -> 24:00, via
scripts/chain_make_leg.py --base <this file> --frame-guards) covers both evening
excursions (19:21:30 tow ~328890 and 20:47:00 tow 334020) plus the end-of-day
re-track tail.

Anchor arithmetic (ApplicationNetwork.cpp): network.start_offset_hours skips
epochs before the processing.max_duration_s anchor latches, so the max-duration
anchor lands on the first processed epoch (13:00:00) and max_duration_s = 21600
ends the seed at 18:59:30 (720 epochs), matching the 00:00-anchored night305
seed shape.

The delta over _night305_conv.json, and nothing else:
  fingerprinted arms  : phase_leveling, emit_ura, code_bias_disc_jump_test
                        (the reason a fresh convergence is needed at all)
  fingerprint-safe    : raim, raim_attribution, raim_within_epoch,
                        raim_withhold_sigma0 = 6.0 (A4 ceiling, 2x the 3.0
                        trigger per the service-bundle design guidance),
                        clock_runaway_check
  Phase 2.1           : cb_excursion_reinit (threshold 50 m default; pivot-only
                        D1), conditional-fingerprinted, so seed and legs carry it
  Phase 2.2           : sh_health_gate (a00 band [2,150] TECU from the SH
                        section defaults, min_epochs 1, rms ceiling off)
  slip detector       : cycle_slip.mw_detect armed explicitly (phase_leveling
                        zero-q carry requires a wired slip response; the loader
                        would force it anyway, but explicit beats implicit)
  observability       : write_run_manifest, trace_epoch_status, trace_events,
                        trace_flag_rates, health_interval_s = 300
  seed anchoring      : start_offset_hours = 13.0, max_duration_s = 21600,
                        state_snapshot_path -> runs/day129_guards/seed/snap.txt,
                        product_output_path -> runs/day129_guards/seed/products.txt

Usage:
    python scripts/p2_make_guard_configs.py [--smoke]
      --smoke additionally writes runs/day129_guards/smoke/leg.json: the same
      config with max_duration_s = 300 (10 epochs 13:00:00..13:04:30) and
      smoke-owned snapshot/product paths, per the staged-cadence directive.
"""
import argparse
import json
import os

import sys
_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
# Resolve any "extends" chain the base may grow. These bases are self-contained
# today, so this is byte-identical now; it is here so the script does not silently
# read a four-key delta as a whole config the day someone rebases it (the failure
# scripts/config_posture_check.py shipped with).
from gnss_config_chain import load_config_flat  # noqa: E402

BASE = r"C:\GnssProcessor\config\test_suite\_night305_conv.json"
OUT = r"C:\GnssProcessor\config\test_suite\_day129_guards_conv.json"
RUNROOT = r"C:\GnssProcessor\runs\day129_guards"


def build(seed_dir, max_duration_s):
    cfg = load_config_flat(BASE)
    cfg["processing"]["max_duration_s"] = float(max_duration_s)
    net = cfg["network"]
    # --- seed anchoring (offset skip runs before the max-duration anchor latch) --
    # Half-epoch back-off (the chain_make_leg discipline): a bare 13.0 h sits on
    # the double-residue boundary, and the strict `<` skipped the 13:00:00 epoch
    # in the smoke (first processed 13:00:30). 13 h minus 15 s starts at 13:00:00
    # exactly, and the 21600 s max-duration then ends the seed at 18:59:30.
    net["start_offset_hours"] = 13.0 - 15.0 / 3600.0
    net["state_snapshot_path"] = (seed_dir + "/snap.txt").replace("\\", "/")
    net["state_snapshot_require"] = False          # the seed is the cold start
    net["state_snapshot_keep_history"] = False
    net["product_output_path"] = (seed_dir + "/products.txt").replace("\\", "/")
    # --- fingerprinted arm-together set (needs this fresh convergence) ---------
    net["phase_leveling"] = True
    net["emit_ura"] = True
    net["code_bias_disc_jump_test"] = True
    # --- fingerprint-SAFE guards ----------------------------------------------
    net["raim"] = True
    net["raim_attribution"] = True
    net["raim_within_epoch"] = True                # sigma0_adapt provider (A4 + T2)
    net["raim_withhold_sigma0"] = 6.0              # withhold ceiling (A4) = 2x raim_sigma0_trigger (3.0)
    net["clock_runaway_check"] = True
    # --- Phase 2.1 (conditional-fingerprinted: seed and legs both carry it) ----
    net["cb_excursion_reinit"] = True              # threshold 50 m default; pivot-only (D1)
    # --- Phase 2.2 -------------------------------------------------------------
    net["sh_health_gate"] = True                   # a00 band [2,150] TECU; min_epochs 1
    # --- slip response for the zero-q leveling carry (explicit, not auto) ------
    net.setdefault("cycle_slip", {})["mw_detect"] = True
    # --- observability (fingerprint-SAFE) --------------------------------------
    net["write_run_manifest"] = True
    net["trace_epoch_status"] = True
    net["trace_events"] = True
    net["trace_flag_rates"] = True
    net["health_interval_s"] = 300.0
    # On the wire, a withheld epoch (A4 / SH gate) must be distinguishable from a
    # dead producer; without this, the smoke run showed 0 H heartbeat lines (RTA-10).
    net["emit_epoch_status_record"] = True
    # --- observability, prefit tier (SPEC_prefit_census_observability.md §9;
    #     promoted 2026-08-01 after the FT63 leg-19 gates). All three are
    #     trace-class and fingerprint-safe, so they are legal on a warm resume.
    #     The census is the only always-on pre-solve screen the producer has:
    #     postfit sigma0 lets a parked cell pass unnoticed through the free
    #     ambiguities, and raim_within_epoch is carry_warm-gated and inert without
    #     leveling. The topology ledger is what makes standing invariant #7's two
    #     edges visible at all. The recorder makes the next discharge attribute
    #     itself. The fuse (network.prefit_fuse_sigma) is deliberately not here:
    #     it is solve-affecting and stays a default-off guard (user directive).
    net["trace_prefit_census"] = True
    net["trace_topology"] = True
    net["rhs_attrib_on_trip"] = True
    return cfg


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--smoke", action="store_true",
                    help="also write the 10-epoch smoke variant")
    args = ap.parse_args()

    seed_dir = os.path.join(RUNROOT, "seed")
    os.makedirs(os.path.join(seed_dir, "logs"), exist_ok=True)
    cfg = build(seed_dir, 21600.0)
    with open(OUT, "w", encoding="utf-8") as fh:
        json.dump(cfg, fh, indent=2)
    print(f"seed config   {OUT}")
    print(f"  window      13:00:00 -> 18:59:30 GPS (720 epochs), cold start")
    print(f"  snapshot    {cfg['network']['state_snapshot_path']}")

    if args.smoke:
        smoke_dir = os.path.join(RUNROOT, "smoke")
        os.makedirs(os.path.join(smoke_dir, "logs"), exist_ok=True)
        scfg = build(smoke_dir, 300.0)
        spath = os.path.join(smoke_dir, "leg.json")
        with open(spath, "w", encoding="utf-8") as fh:
            json.dump(scfg, fh, indent=2)
        print(f"smoke config  {spath}  (13:00:00..13:04:30, 10 epochs)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
