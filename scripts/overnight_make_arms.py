#!/usr/bin/env python
"""Generate the overnight follow-on arm configs for the FT63 convergence lineage.

Every arm is derived from the validated `_ft63_24h_conv.json` posture so the
only variable in each is the thing being tested.  Run AFTER `runs/ft63_24h_conv`
finishes -- the two resume arms read that run's hourly warm-start entries.

    python scripts/overnight_make_arms.py            # all arms
    python scripts/overnight_make_arms.py --resume-only

Arms
----
ft63_warm12 / ft63_cold12
    Warm-start convergence.  Same 1 h window, same config, one seeded from the
    hour-12 snapshot and one cold.  `state_snapshot_require` is armed on the
    warm arm so a fingerprint/age failure ABORTS instead of silently
    cold-starting (standing invariant #4 -- a silent cold start reads as a
    clean resume).
ft63_l2off
    Dual-layer ionosphere.  layer2_max_degree 4 -> 0 (single 450 km shell),
    3 h cold; the control is the first 3 h of the 24h run.  Cold by necessity:
    layer2_max_degree is inside the warm-resume fingerprint.
ft63_gg12h
    Multi-GNSS.  GPS+Galileo, all/all, 12 h (covers era-1 at 07:32).  Explicit
    Galileo `signal_priorities` are mandatory -- without them the multi-GNSS
    drop is silent.  Also the first exercise of the admission screen on an
    all/all config, the regime where the median-over-observables inversion
    lived before it was fixed.
net305_screen
    Scale.  The full 305-station roster, GPS-only, 20 epochs: does the screen
    stay BSHM-only at 5x the roster, and does ignition survive?  NOT a
    convergence run -- 305 sta costs ~3-4 min/epoch, so a day is ~1 week.
"""
import argparse
import json
import pathlib
import shutil
import sys

ROOT = pathlib.Path("C:/GnssProcessor")
CFG = ROOT / "config/test_suite"
RUNS = ROOT / "runs"
PARENT_RUN = RUNS / "ft63_24h_conv"
BASE_CFG = CFG / "_ft63_24h_conv.json"

HALF_EPOCH_H = 15.0 / 3600.0   # +15 s: the fast-forward gate is a strict <


def base():
    return json.loads(BASE_CFG.read_text())


def write(name, cfg, snapshot=True):
    """Write one arm config, pointing its snapshot at its own run dir."""
    n = cfg["network"]
    if snapshot:
        n["state_snapshot_path"] = f"C:/GnssProcessor/runs/{name}/state_snapshot.bin"
    else:
        # An empty path is OFF; clear its riders too so the loader is not asked
        # to honour a carry/history request with nowhere to write it.
        n["state_snapshot_path"] = ""
        n["state_snapshot_carry"] = False
        n["state_snapshot_keep_history"] = False
    (CFG / f"_{name}.json").write_text(json.dumps(cfg, indent=1) + "\n")
    print(f"  _{name}.json".ljust(36)
          + f"{len(n['stations']):3d} sta  {cfg['processing']['max_duration_s'] / 3600:5.2f} h"
          + f"  sys={','.join(cfg['processing']['active_systems'])}"
          + (f"  offset={n['start_offset_hours']:.6f} h" if n.get("start_offset_hours") else ""))


def emitted_tows():
    """TOW of each EMITTED epoch of the parent run, in order.

    The history snapshots are named `.ep<n_emitted>`, so index i (1-based) of
    this list is the epoch that snapshot describes.  Deriving it from the
    status CSV rather than assuming uniform 30 s spacing keeps the mapping
    correct if the parent run ever withholds an epoch.
    """
    import csv
    p = PARENT_RUN / "logs/network_epoch_status.csv"
    with p.open() as f:
        return [float(r["tow"]) for r in csv.DictReader(f)
                if r["disposition"] == "emitted"]


def resume_arms(target_h=12.0):
    tows = emitted_tows()
    if not tows:
        sys.exit("parent run has no emitted epochs yet")
    t0 = tows[0]

    snaps = {}
    for p in PARENT_RUN.glob("state_snapshot.bin.ep*"):
        try:
            snaps[int(p.name.rsplit(".ep", 1)[1])] = p
        except ValueError:
            pass
    if not snaps:
        sys.exit(f"no .ep history snapshots in {PARENT_RUN}")

    # snapshot .epN describes the Nth emitted epoch => tows[N-1]
    def snap_tow(idx):
        return tows[idx - 1] if 1 <= idx <= len(tows) else None

    cand = [(i, snap_tow(i)) for i in sorted(snaps) if snap_tow(i) is not None]
    idx, tsnap = min(cand, key=lambda it: abs((it[1] - t0) - target_h * 3600.0))
    hours = (tsnap - t0) / 3600.0
    print(f"  seed: {snaps[idx].name}  ep{idx}  TOW {tsnap:.0f}  = {hours:.3f} h into the run")

    # Resume begins at the epoch AFTER the snapshot: offset past it by a half
    # epoch so the strict-< fast-forward gate cannot re-consume the snapped one.
    offset = hours + HALF_EPOCH_H

    for name, warm in (("ft63_warm12", True), ("ft63_cold12", False)):
        c = base()
        c["processing"]["max_duration_s"] = 3600.0
        c["network"]["max_duration_s"] = 3600.0
        c["network"]["start_offset_hours"] = offset
        if warm:
            c["network"]["state_snapshot_require"] = True
            write(name, c, snapshot=True)
            d = RUNS / name
            d.mkdir(parents=True, exist_ok=True)
            shutil.copy2(snaps[idx], d / "state_snapshot.bin")
            print(f"      seeded {d / 'state_snapshot.bin'}")
        else:
            # cold twin: no snapshot to read, and none written (nothing to
            # resume from would make the comparison ambiguous)
            write(name, c, snapshot=False)
    return idx, tsnap


def cold_arms():
    # --- dual-layer ionosphere OFF -----------------------------------------
    for name, dur in (("ft63_l2off_smoke", 300.0), ("ft63_l2off", 3 * 3600.0)):
        c = base()
        c["processing"]["max_duration_s"] = dur
        c["network"]["max_duration_s"] = dur
        c["estimation"]["global_vtec_sh"]["layer2_max_degree"] = 0
        write(name, c)

    # --- GPS + Galileo ------------------------------------------------------
    for name, dur in (("ft63_gg_smoke", 300.0), ("ft63_gg12h", 12 * 3600.0)):
        c = base()
        c["processing"]["max_duration_s"] = dur
        c["network"]["max_duration_s"] = dur
        c["processing"]["active_systems"] = ["GPS", "Galileo"]
        c["processing"]["frequency_mode"] = "all"
        c["processing"]["channel_mode"] = "all"
        c["processing"]["signal_priorities"] = {"GPS": "WCSLXPYMIQ",
                                                "Galileo": "CBXIQ"}
        write(name, c)

    # --- 305 stations -------------------------------------------------------
    src = json.loads((CFG / "_maxnet_305_gpsgal_d15_24h.json").read_text())
    c = base()
    c["processing"]["max_duration_s"] = 600.0
    c["network"]["max_duration_s"] = 600.0
    c["network"]["stations"] = src["network"]["stations"]
    # The promotion ledger's own cautionary example: validated at 50 sta,
    # NET-HARMFUL at 305 (short-baseline wrong-integer 0.0% -> 12.5%).
    c["network"]["mw_wl"]["bridge_sample_debias"] = False
    c["network"]["state_snapshot_interval_s"] = 300.0
    write("net305_screen", c)
    assert any(s["name"] == c["network"]["clock_datum_station"]
               for s in c["network"]["stations"]), "clock datum station not in 305 roster"


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--resume-only", action="store_true")
    ap.add_argument("--cold-only", action="store_true")
    ap.add_argument("--target-hours", type=float, default=12.0)
    a = ap.parse_args()
    if not a.cold_only:
        resume_arms(a.target_hours)
    if not a.resume_only:
        cold_arms()
