#!/usr/bin/env python3
"""Build MW/WL-cascade CONDITIONING arms on the DOY-129 305-station shape.

Question: can the EWL->WL cascade stiffen the code/CB/iono system against the
SH-detonation disease?  Three arms, all derived from the guards_cont anchor's OWN
leg.json so the estimator shape (roster, guards, carry, SH) is identical:

  ctl   cascade OFF                      -- the reference shape
  lane  ewl_enable + lane_carry with bridge_meas=FALSE.
        ⚠️ This is NOT an iono-free arm, and no iono-free arm is constructible:
        (1) network.lane_carry.enable REQUIRES network.mw_wl.ewl_enable
            (GnssConfigLoadNetwork.cpp:2714-2720, fail-loud `return false`), and
        (2) bridge_meas only suppresses the raw-GF lane INCREMENT; the WL
            DECISION still comes from the phase bridge, because the walk is
            gated on `(wl_inject_on || lane_fix_on || lane_bridge_on)
            && !ewl_fixed.empty()` (NetworkMwWlGlue.cpp:439) and lane_fix_on is
            `lane_on && lane_joint` (ApplicationNetwork.cpp:580).
        So this arm isolates the lane-increment cost, not the iono coupling.
  casc  the shipped cascade: ewl_enable + uwl_enable (GPS EWL(2,5) lam 5.861 m)
        + lane_carry with the phase BRIDGE increments on.

Cold by default (no snapshot): these are engagement/cost probes, not lineage runs.
The anchor's snapshot path is always redirected into the arm dir so a run can never
write into runs/guards_cont.

    python scripts/mwwl_cond_arm.py --arm casc --at 19:00:00 --eps 15
"""
import argparse
import copy
import json
import os

ANCHOR = r"C:\GnssProcessor\runs\guards_cont\leg.json"
OUT_ROOT = r"C:\GnssProcessor\runs\mwwl_cond"


def hms(s):
    h, m, sec = s.split(":")
    return int(h) * 3600 + int(m) * 60 + float(sec)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arm", choices=("ctl", "lane", "casc", "geom"), required=True)
    ap.add_argument("--at", default="19:00:00", help="first epoch (UTC time of day)")
    ap.add_argument("--eps", type=int, default=15)
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--tag", default="")
    ap.add_argument("--min-arc", type=int, default=3,
                    help="geom arm only: mw_wl.min_arc_epochs. The F6 dump is ONE-SHOT on the "
                         "first arming epoch, so a low value fires early with only a handful "
                         "of mature arcs (tiny sample). Use 10 + >=14 epochs for a real "
                         "cycle-geometry population.")
    a = ap.parse_args()

    cfg = json.load(open(ANCHOR, encoding="utf-8"))
    net = cfg["network"]
    name = a.arm + (("_" + a.tag) if a.tag else "")
    armdir = os.path.join(OUT_ROOT, name)
    os.makedirs(os.path.join(armdir, "logs"), exist_ok=True)

    # --- window: half-epoch back-off (the 13:00 double-residue lesson applies cold too)
    net["start_offset_hours"] = (hms(a.at) - 15.0) / 3600.0
    cfg["processing"]["max_duration_s"] = 30.0 * (a.eps - 1)
    cfg["processing"]["eigen_threads"] = a.threads

    # --- never touch the anchor: cold, own product + snapshot paths
    net.pop("state_snapshot_carry", None)
    net["state_snapshot_path"] = os.path.join(armdir, "snap.txt").replace("\\", "/")
    net["state_snapshot_require"] = False
    net["state_snapshot_keep_history"] = False
    net["product_output_path"] = os.path.join(armdir, "products.txt").replace("\\", "/")

    # --- cascade telemetry (inert unless a lane/inject path is armed)
    mw = net.setdefault("mw_wl", {})
    lc = net.setdefault("lane_carry", {})
    mw["lane_trace"] = True

    if a.arm == "lane":
        mw["ewl_enable"] = True       # MANDATORY under lane_carry (loader hard requirement)
        mw["bridge_sample_debias"] = True
        lc["enable"] = True
        lc["lane_meas"] = True        # per-epoch MW(1,2) lane increments  (iono-free)
        lc["bridge_meas"] = False     # suppresses the raw-GF INCREMENT only, not the decision
    elif a.arm == "casc":
        mw["ewl_enable"] = True       # GPS EWL(2,5), lam 5.861 m, third band UNCARRIED
        mw["uwl_enable"] = True       # arms the per-constellation lane-tier table
        mw["bridge_sample_debias"] = True   # 60-ep age-bias fix (per-sample de-bias)
        # Galileo-only levers stay off on a GPS-only roster
        mw["uwl_mid_enable"] = False
        mw["uwl_dual_top"] = False
        mw["uwl_family_pairs"] = False
        mw["uwl_cold_fix"] = False    # FIELD-BLOCKING until the force-fire A/B (invariant #1)
        lc["enable"] = True
        lc["lane_meas"] = True
        lc["bridge_meas"] = True
    elif a.arm == "geom":
        # CYCLE-GEOMETRY PROBE.  The lane trace records only the CHORD (station, sat); the
        # fundamental cycle closes through the eq34 tree, so the effective DD baseline is not
        # observable from it.  The F6 dump carries `est_station` for EVERY arc of a cycle
        # (F6Arc, NetworkWlRunTypes.hpp), which is exactly the cycle's station membership.
        # f6_dump is gated on mw_wl.enable (ApplicationNetwork.cpp:653), the WL-1 float layer —
        # a DIAGNOSTIC shape that double-drives the WL columns, so this arm is for geometry
        # only and must never be used to grade a treatment.
        mw["enable"] = True
        mw["f6_dump"] = True
        mw["ewl_enable"] = True
        # The one-shot dump fires on the FIRST epoch the WL-1 layer arms, which needs an arc
        # at n >= min_arc_epochs (default 10).  Cycle GEOMETRY (which stations a fundamental
        # cycle spans) is a property of the eq34 tree, not of the MW maturity threshold, so
        # lowering it just makes the probe cheap.  Never use this value to grade a lane.
        mw["min_arc_epochs"] = a.min_arc
        # Without cold_clock_seed a cold 305-station start leaves O(10^5) m of receiver-clock
        # error in the ambiguity SEEDS (measured here: x_lin1 = 269 km at station 1), which
        # the lane_max_abs_xlin_m quarantine then rejects — the WL layer arms with ~8 of ~2317
        # cycles.  The seeder is the documented cure ([[project_network_coldseed_clock_leak]]);
        # arm it so the geometry probe sees a representative cycle population.
        net["cold_clock_seed"] = True

    out = os.path.join(armdir, "leg.json")
    with open(out, "w", encoding="utf-8") as fh:
        json.dump(cfg, fh, indent=2)
    print(f"arm {name}: {out}")
    print(f"  window   {a.at} + {a.eps} ep ({cfg['processing']['max_duration_s']:.0f} s), "
          f"offset_h {net['start_offset_hours']:.9f}, threads {a.threads}")
    print(f"  mw_wl    {json.dumps(mw)}")
    print(f"  lane     {json.dumps(lc)}")
    print(f"  outputs  {armdir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
