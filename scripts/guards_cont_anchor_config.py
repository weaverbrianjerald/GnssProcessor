#!/usr/bin/env python3
"""Continuous GUARDS-ARMED run 13:00 -> 21:29:30 with full snapshot history.

Follow-on to runs/raim_cold_event (Jul-28), which proved the 20:47:00 epoch
carries NO input insult: from a cold state 35 epochs old, every statistic is
flat at tow 334020 (sigma0_pf_code 3.91, slant mean 8.3 m, GOLD00USA 4.62,
within-epoch RAIM live with n_adapt = 0, zero trips/withholds).  The event
therefore requires the ACCUMULATED filter state of a long-running arm — it
expressed on 7.8 h (s2z) and 14.8 h (reference) continuous lineages, and on
nothing shorter or seamed.

This run gives, in one 9 h pass:
  (a) the owed frame-guards ON-path validation on a lineage that can express
      the disease (continuous, no 19:00 seam, guards armed throughout);
  (b) state_snapshot_keep_history = true -> EVERY epoch's snapshot survives
      (~0.55 MB x 1020 ep ~ 560 MB), so any pre-event epoch becomes a
      warm-start anchor for ~30-min treatment iterations afterwards (the
      rapid-iteration harness the Jul-28 RAIM directive asks for).

Derived from _day129_guards_conv.json (fingerprinted guard set in the seed by
construction, so warm resumes from ANY history snapshot carry it).
"""
import json
import os

import sys
_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
# Resolve any "extends" chain the base may grow.  These bases are self-contained
# today, so this is byte-identical now; it is here so the script does not silently
# read a four-key DELTA as a whole config the day someone rebases it (the failure
# scripts/config_posture_check.py shipped with).
from gnss_config_chain import load_config_flat  # noqa: E402

BASE = r"C:\GnssProcessor\config\test_suite\_day129_guards_conv.json"
RUNDIR = r"C:\GnssProcessor\runs\guards_cont"

cfg = load_config_flat(BASE)
net = cfg["network"]

cfg["processing"]["max_duration_s"] = 30600.0           # 13:00:00 .. 21:29:30 (1020 ep)
# Paths MUST live under <RunRoot>/seed/ — the supervisor's checkpoint watchdog
# watches <RunRoot>/seed/snap.txt and kills any segment that has not written it
# within 30 min (measured the hard way: the first launch checkpointed happily
# to <RunRoot>/snap.txt and was watchdog-killed at +30 min, healthy, ep 35).
SEED = RUNDIR + r"\seed"
net["state_snapshot_path"] = (SEED + "/snap.txt").replace("\\", "/")
net["state_snapshot_require"] = False
net["state_snapshot_keep_history"] = True               # the whole point
net["product_output_path"] = (SEED + "/products.txt").replace("\\", "/")
net["trace_postfit_sigma0"] = True
net["trace_postfit_keyed"] = True

os.makedirs(os.path.join(SEED, "logs"), exist_ok=True)
out = os.path.join(RUNDIR, "leg.json")
with open(out, "w", encoding="utf-8") as fh:
    json.dump(cfg, fh, indent=2)
print(f"wrote {out}")
print("window 13:00:00 -> 21:29:30 COLD (1020 ep), guards armed, snapshot history ON")
print("events in window: 19:21:30 (tow 328890), 19:34 disturbance, 20:47:00 (tow 334020)")
