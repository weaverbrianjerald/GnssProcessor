#!/usr/bin/env python3
"""Cold-start guards-armed window over the 20:47:00 excursion (rapid-iteration probe).

Derives runs/raim_cold_event/leg.json from the Phase-2 guards config
(_day129_guards_conv.json, full armed set: phase_leveling, within-epoch RAIM
plus A4 ceiling, cb_excursion guard, sh_health_gate, emit_ura,
code_bias_disc_jump_test) and re-anchors it cold at 20:30:00 for 80 epochs
(20:30:00..21:09:30; the excursion epoch tow 334020 lands at epoch ~35).

Purpose (user directive Jul-28): the armed residual-domain tiers must meet the
real 96 km epoch without a pedigreed state, and a cold start answers
state-independence of the trigger as a side effect.  Adds the landing-site
instruments plus trace_station_residuals for row-level attribution of the
GOLD00USA concentration.

Cold start implies no resume, so the fingerprint constrains nothing here.
"""
import json
import os

import sys
_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
# Resolve any "extends" chain the base may grow.  These bases are self-contained
# today, so this is byte-identical now; it is here so the script does not silently
# read a four-key delta as a whole config the day someone rebases it, the failure
# scripts/config_posture_check.py shipped with.
from gnss_config_chain import load_config_flat  # noqa: E402

BASE = r"C:\GnssProcessor\config\test_suite\_day129_guards_conv.json"
RUNDIR = r"C:\GnssProcessor\runs\raim_cold_event"

cfg = load_config_flat(BASE)
net = cfg["network"]

cfg["processing"]["max_duration_s"] = 2400.0            # 80 epochs
net["start_offset_hours"] = 20.5 - 15.0 / 3600.0        # 20:30:00 with back-off
net["state_snapshot_path"] = (RUNDIR + "/snap.txt").replace("\\", "/")
net["state_snapshot_require"] = False
net["state_snapshot_keep_history"] = False
net["product_output_path"] = (RUNDIR + "/products.txt").replace("\\", "/")
# Landing-site + attribution instruments (all trace-only).
net["trace_postfit_sigma0"] = True
net["trace_postfit_keyed"] = True
net["trace_station_residuals"] = True

os.makedirs(os.path.join(RUNDIR, "logs"), exist_ok=True)
out = os.path.join(RUNDIR, "leg.json")
with open(out, "w", encoding="utf-8") as fh:
    json.dump(cfg, fh, indent=2)
print(f"wrote {out}")
print("window 20:30:00 -> 21:09:30 (80 ep, cold), event tow 334020 at ep ~35")
print("armed: phase_leveling, raim(+within_epoch, A4=6.0), cb_excursion, "
      "sh_health_gate, emit_ura, cb_disc_jump_test, clock_runaway_check")
