#!/usr/bin/env python3
"""Generate the SPEC_s2z_realization_ab arm config (sibling of p2_make_guard_configs.py).

Derives config/test_suite/_day129_s2z_conv.json from _night305_conv.json by
setting only the treatment (network.clock_datum_sum_to_zero) plus the
fingerprint-safe instrumentation set, anchored on the 13:00 GPS lineage
(half-epoch back-off). Guards stay off by decision D-C: this is an
observation run of the 20:47:00 excursion under the sum-to-zero clock gauge,
deliberately matching the reference run's unguarded posture (same diagnostic
class as runs/day129_cont and runs/p1_leg) so the landing site is not masked
by a reinit. Not a field posture.

Deviation from the spec's seed+leg shape, deliberate (Jul-28): one continuous
segment 13:00:00 to 23:59:30 (max_duration_s 39600, 1320 epochs), no designed
19:00 seam. The guards chain measured the seam as materially non-inert on
this exact lineage: its leg's first-epoch diagonal-only resume blew a00 to
1410 TECU, the SH gate then withheld 19:00 to 20:59:30 continuously, and
neither evening excursion expressed (clock-median delta at 334020 was +0.31 m
vs the reference's 4581.9 m). The reference run this A/B grades against is
continuous through both events; a continuous arm also shrinks the spec's own
carry-age confound (7.8 h at 20:47 instead of 1.8 h, tau = 3.79 h). Cost: the
leg-only --frame-guards traces (sh_spectrum, dd_fractional) are dropped, both
explicitly marked skippable by the spec; the landing-site instruments
(trace_postfit_keyed/sigma0, SH health) ride the whole run from this config.

The s2z knob is conditional-appended to the resume fingerprint
(NetworkSnapshotFingerprint.cpp: "clkdatum:s2z"), which is why this arm needs its own
cold 6 h seed: no existing snapshot can be resumed.

Phase-0 loader-sweep guarantees (GnssConfigLoadNetwork.cpp:2283-2312, :2424-2450):
the derived config must trip none of the s2z warn/mutex sites. This script
asserts the base config leaves the coupled knobs absent rather than trusting
it silently: if the base ever grows one, generation fails loud instead of
producing a config whose loader sweep is dirty.

Usage:
    python scripts/s2z_make_config.py [--smoke]
      --smoke additionally writes runs/s2z_ab/smoke/leg.json, the same config
      with max_duration_s = 300 (10 epochs 13:00:00..13:04:30) and smoke-owned
      snapshot/product paths, per the staged-cadence directive.
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
OUT = r"C:\GnssProcessor\config\test_suite\_day129_s2z_conv.json"
RUNROOT = r"C:\GnssProcessor\runs\s2z_ab"

# Loader warn/mutex sites for s2z (Phase-0 item 1). A pivot pin, pivot
# stickiness/handover signalling, or the broadcast-anchored gauge in the same
# config would be warned inert or hard-rejected; emit_clock_disc would trigger
# the T3c auto-arm (clock_disc_on_set_change), changing the emitted stream.
S2Z_COUPLED_MUST_BE_ABSENT = [
    "clock_datum_satellite",           # warned ignored under s2z (:2290)
    "sticky_pivot",                    # warned inert under s2z (:2298)
    "pivot_vanish_return_signal",      # warned inert under s2z (:2298)
    "clock_datum_broadcast_anchored",  # mutually exclusive: loader error (:2307)
    "emit_clock_disc",                 # T3c auto-arm would flip clock_disc_on_set_change (:2427)
    "clock_disc_on_set_change",        # requires emit_clock_disc; not part of this arm
]

# Decision D-C: the guard set stays OFF (observation run).  Assert the base
# does not carry any of them so the derived config's unguarded posture is the
# reference run's posture by construction, not by luck.
GUARDS_MUST_BE_ABSENT = [
    "phase_leveling", "emit_ura", "code_bias_disc_jump_test",
    "raim", "raim_attribution", "raim_within_epoch", "clock_runaway_check",
    "cb_excursion_reinit", "sh_health_gate",
]


def build(seed_dir, max_duration_s):
    cfg = load_config_flat(BASE)
    net = cfg["network"]

    for key in S2Z_COUPLED_MUST_BE_ABSENT + GUARDS_MUST_BE_ABSENT:
        if key in net:
            raise SystemExit(
                f"FATAL: base config carries network.{key} = {net[key]!r} — "
                f"the s2z arm's loader sweep / unguarded posture would be dirty. "
                f"Re-derive the delta by hand.")

    cfg["processing"]["max_duration_s"] = float(max_duration_s)
    # --- seed anchoring, exactly the guards-seed shape -------------------------
    # Half-epoch back-off: a bare 13.0 h sits ON the double-residue boundary and
    # the strict `<` skips the 13:00:00 epoch (measured on the guards smoke).
    net["start_offset_hours"] = 13.0 - 15.0 / 3600.0
    net["state_snapshot_path"] = (seed_dir + "/snap.txt").replace("\\", "/")
    net["state_snapshot_require"] = False          # the seed is the cold start
    net["state_snapshot_keep_history"] = False
    net["product_output_path"] = (seed_dir + "/products.txt").replace("\\", "/")
    # --- the treatment -----------------------------------------------------
    net["clock_datum_sum_to_zero"] = True
    # --- instrumentation (all fingerprint-SAFE; sh_spectrum / dd_fractional
    #     ride the leg only, via chain_make_leg.py --frame-guards) --------------
    net["write_run_manifest"] = True
    net["trace_epoch_status"] = True
    net["trace_events"] = True
    net["trace_flag_rates"] = True
    net["health_interval_s"] = 300.0
    net["emit_epoch_status_record"] = True
    net["trace_postfit_sigma0"] = True
    net["trace_postfit_keyed"] = True              # the landing-site instrument
    return cfg


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--smoke", action="store_true",
                    help="also write the 10-epoch smoke variant")
    args = ap.parse_args()

    seed_dir = os.path.join(RUNROOT, "seed")
    os.makedirs(os.path.join(seed_dir, "logs"), exist_ok=True)
    cfg = build(seed_dir, 39600.0)
    with open(OUT, "w", encoding="utf-8") as fh:
        json.dump(cfg, fh, indent=2)
    print(f"seed config   {OUT}")
    print(f"  treatment   network.clock_datum_sum_to_zero = true (guards OFF, D-C)")
    print(f"  window      13:00:00 -> 23:59:30 GPS (1320 epochs), cold start, NO 19:00 seam")
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
