#!/usr/bin/env python3
"""Build the validation arm configs for docs/SPEC_prefit_census_observability.md.

Every arm derives from the standing FT63 leg-19 replay config
(``config/test_suite/_ft63_leg19_rhsat.json``) so the estimation configuration
is identical across arms and only this card's knobs differ.  The §12 interaction
discipline is enforced here: no arm carries a deweight-release-hygiene knob, and
the fuse arm is the only one that touches the solve.

Each arm gets its own run directory and its own copy of the seed snapshot,
because the producer overwrites ``state_snapshot_path`` at its first hour
boundary: sharing one file between arms would silently re-seed the later arms
from the earlier arm's output.

Usage::

    python scripts/pc_make_arms.py                 # write configs + seed run dirs
    python scripts/pc_make_arms.py --list          # just print the plan
"""
from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "config" / "test_suite" / "_ft63_leg19_rhsat.json"
SNAP_DIR = ROOT / "runs" / "ft63_24h_allon" / "snap_archive"

# (arm tag, snapshot index, start_offset_hours, network-knob overrides)
# ``None`` as a value removes the key inherited from the base config.
ARMS = [
    # Control for the off byte-identity check: same build, no card knobs at all.
    ("pcbase", 20, 19.1125, {"rhs_attrib_tow": None, "rhs_attrib_n_ep": None}),
    # Hoist regression: byte-identical attribution against runs/ft63_leg19_rhsat (INC-0).
    ("rhsat2", 20, 19.1125, {}),
    # Census, plus the negative clock-step gate at TOW 329340, URUM00CHN (INC-1).
    ("census", 20, 19.1125, {
        "rhs_attrib_tow": None, "rhs_attrib_n_ep": None,
        "trace_prefit_census": True,
        "prefit_event_sigma": 1000.0,
        "prefit_event_holdoff_ep": 20,
    }),
    # Topology ledger (INC-4).
    ("topo", 20, 19.1125, {
        "rhs_attrib_tow": None, "rhs_attrib_n_ep": None,
        "trace_topology": True,
    }),
    # Flight recorder; the hand-picked window is disarmed on purpose (INC-5).
    ("trip", 20, 19.1125, {
        "rhs_attrib_tow": None, "rhs_attrib_n_ep": None,
        "rhs_attrib_on_trip": True,
        "rhs_attrib_on_trip_prev": True,
    }),
    # Structural fuse, the one solve-affecting arm, graded on its own leg (INC-2).
    # The census rides along so the events name what the fuse cut.
    ("fuse", 20, 19.1125, {
        "rhs_attrib_tow": None, "rhs_attrib_n_ep": None,
        "trace_prefit_census": True,
        "prefit_fuse_sigma": 1000.0,
        "prefit_event_holdoff_ep": 20,
    }),
    # Seed-birth tripwire on the leg that contains G07@BSHM's ~18:34 arc birth
    # (INC-3).  The snapshot index is not the hour: the archive's snap_N is
    # written about 30 s before offset (N-1).1125, which is why the main leg
    # pairs snap_20 with 19.1125.  Pairing snap_18 with 18.1125, as the spec's
    # §12.6 text does, makes the snapshot 3690 s stale and, with
    # state_snapshot_require on, hard-aborts the leg.
    ("birth18", 19, 18.1125, {
        "rhs_attrib_tow": None, "rhs_attrib_n_ep": None,
        "trace_prefit_census": True,
        "seed_birth_check_sigma": 100.0,
        "prefit_event_holdoff_ep": 20,
        "max_duration_s": 3600.0,
    }),
]


def snapshot_for(idx: int) -> Path:
    hits = sorted(SNAP_DIR.glob(f"snap_{idx:02d}_*.bin"))
    if not hits:
        raise SystemExit(f"no snapshot snap_{idx:02d}_*.bin under {SNAP_DIR}")
    return hits[0]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true", help="print the plan and exit")
    args = ap.parse_args()

    base = json.loads(BASE.read_text(encoding="utf-8"))
    for tag, snap_idx, offset, overrides in ARMS:
        run_dir = ROOT / "runs" / f"ft63_leg19_pc_{tag}"
        cfg_path = ROOT / "config" / "test_suite" / f"_ft63_leg19_pc_{tag}.json"
        seed = snapshot_for(snap_idx)
        if args.list:
            print(f"{tag:8s} snap_{snap_idx:02d} offset={offset} -> {run_dir.name}")
            continue

        cfg = json.loads(json.dumps(base))          # deep copy
        net = cfg["network"]
        net["start_offset_hours"] = offset
        net["state_snapshot_path"] = str(run_dir / "state_snapshot.bin").replace("\\", "/")
        for k, v in overrides.items():
            if v is None:
                net.pop(k, None)
            else:
                net[k] = v
        cfg_path.write_text(json.dumps(cfg, indent=1) + "\n", encoding="utf-8")

        run_dir.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(seed, run_dir / "state_snapshot.bin")
        print(f"wrote {cfg_path.name}  (seed {seed.name} -> {run_dir.name})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
