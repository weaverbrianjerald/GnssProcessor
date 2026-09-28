#!/usr/bin/env python3
"""Generate a warm-resume branch leg of the 24 h quiet bed from its .ep snapshot library.

STABILITY campaign (SPEC_synthetic_minimum_network §18.7 / §18.9): the continuous
run `runs/camp60wu_quiet24h_r2q3` keeps a per-epoch snapshot history
(`state_snapshot.bin.ep<NNNN>`, one per emitted epoch); any hour can be resumed
as a leg that shares the continuous run's eclipse schedule, so a leg's h12-h24
grades against the same hours of the continuous run.  This script reproduces
the leg-A recipe (`config/test_suite/_camp60wu_q12warm_szv.json`) with an
arbitrary knob set, so an ON/OFF pair differs in exactly the knobs named:

  1. copy `<base_run>/state_snapshot.bin.ep<N>` to `runs/<leg>/state_snapshot.snap`
     (a resume overwrites the snapshot it read, so a leg must never point directly
     at the library file);
  2. write `config/test_suite/_<leg>.json` as the leg-A config with
       network.state_snapshot_path -> runs/<leg>/state_snapshot.snap
       network.state_snapshot_require true, keep_history false
       network.product_output_path  -> runs/<leg>/products.txt
       network.start_offset_hours   -> (hour - 15 s)/3600 (half-epoch back-off)
     plus every `--set section.key=value` (JSON-parsed value; dotted path; arrays
     replace rather than merge).

Rails (CLAUDE.md): a fingerprinted knob needs a fresh convergence, so a leg can only
flip knobs outside the warm-resume fingerprint (forward-only response policies); the
loader enforces this via state_snapshot_require (a mismatch is a hard error, not a
silent cold start).

Usage:
  python scripts/simnet_make_seam_leg.py --leg camp60wu_q12warm_xyz --ep 1440 \
      --set network.some_knob=true --note "single mechanism: ..."
  python scripts/simnet_make_seam_leg.py --leg camp60wu_q06warm_ctl --ep 720 --hour 6
Then run from the leg's own CWD under a hash-verified renamed Release exe:
  cd runs/<leg> && ../../build/x64-Release/gnss_processor_<tag>.exe ../../config/test_suite/_<leg>.json > console.txt 2>&1
"""
from __future__ import annotations
import argparse
import json
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEMPLATE = os.path.join(ROOT, "config", "test_suite", "_camp60wu_q12warm_szv.json")
BASE_RUN = os.path.join(ROOT, "runs", "camp60wu_quiet24h_r2q3")
EPOCH_S = 30.0
T0_TOW = 259230.0   # first epoch of the 24 h bed (00:00:30 DOY-129, GPS week 2313)


def set_path(cfg: dict, dotted: str, value) -> None:
    keys = dotted.split(".")
    d = cfg
    for k in keys[:-1]:
        d = d.setdefault(k, {})
    d[keys[-1]] = value


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--leg", required=True, help="leg name (run dir runs/<leg>, config _<leg>.json)")
    ap.add_argument("--ep", type=int, required=True, help="snapshot index N of state_snapshot.bin.ep<N> (1440 = h12)")
    ap.add_argument("--hour", type=float, default=None,
                    help="resume hour; default = ep * 30 s / 3600 (the snapshot at ep N is written at epoch N)")
    ap.add_argument("--base-run", default=BASE_RUN)
    ap.add_argument("--template", default=TEMPLATE)
    ap.add_argument("--set", action="append", default=[], metavar="section.key=JSON",
                    help="knob override, JSON-parsed value (true/false/1e-9/\"str\"/[..])")
    ap.add_argument("--note", default="", help="one-line purpose recorded in the config _comment")
    ap.add_argument("--force", action="store_true", help="overwrite an existing leg dir/config")
    a = ap.parse_args()

    ep_file = os.path.join(a.base_run, f"state_snapshot.bin.ep{a.ep}")
    if not os.path.isfile(ep_file):
        print(f"ERROR: snapshot {ep_file} not found", file=sys.stderr)
        return 2
    leg_dir = os.path.join(ROOT, "runs", a.leg)
    cfg_path = os.path.join(ROOT, "config", "test_suite", f"_{a.leg}.json")
    if (os.path.exists(leg_dir) or os.path.exists(cfg_path)) and not a.force:
        print(f"ERROR: {leg_dir} or {cfg_path} exists (use --force)", file=sys.stderr)
        return 2
    os.makedirs(leg_dir, exist_ok=True)
    snap_dst = os.path.join(leg_dir, "state_snapshot.snap")
    shutil.copyfile(ep_file, snap_dst)

    hour = a.hour if a.hour is not None else a.ep * EPOCH_S / 3600.0
    start_offset_h = (hour * 3600.0 - EPOCH_S / 2.0) / 3600.0
    first_tow = T0_TOW + hour * 3600.0

    with open(a.template, "r", encoding="utf-8") as f:
        cfg = json.load(f)
    n = cfg["network"]
    n["state_snapshot_path"] = snap_dst.replace("\\", "/")
    n["state_snapshot_require"] = True
    n["state_snapshot_keep_history"] = False
    n["product_output_path"] = os.path.join(leg_dir, "products.txt").replace("\\", "/")
    n["start_offset_hours"] = round(start_offset_h, 7)
    sets = []
    for s in a.set:
        if "=" not in s:
            print(f"ERROR: --set needs section.key=value, got {s!r}", file=sys.stderr)
            return 2
        k, v = s.split("=", 1)
        try:
            val = json.loads(v)
        except json.JSONDecodeError:
            val = v
        set_path(cfg, k, val)
        sets.append(f"{k}={json.dumps(val)}")
    cfg["_comment"] = [
        f"STABILITY branch leg {a.leg}: warm resume at h{hour:g} of the 24 h bed from",
        f"{os.path.relpath(ep_file, ROOT)} (copied to runs/{a.leg}/state_snapshot.snap; a resume overwrites it),",
        f"start_offset_hours {n['start_offset_hours']} (h{hour:g} - 15 s half-epoch back-off; first processed epoch tow {first_tow:.0f}),",
        "state_snapshot_require + zwd_seam_reseed inherited from the leg-A recipe (_camp60wu_q12warm_szv).",
        "Knob overrides vs leg A: " + (", ".join(sets) if sets else "NONE (control)"),
    ] + ([a.note] if a.note else []) + [
        "Generated by scripts/simnet_make_seam_leg.py — grade h-window against the continuous run's SAME hours.",
    ]
    with open(cfg_path, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2)
    print(f"leg {a.leg}: snapshot ep{a.ep} -> {snap_dst}")
    print(f"config {cfg_path}: start_offset_hours {n['start_offset_hours']} (first epoch tow {first_tow:.0f}); overrides: {sets or 'none'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
