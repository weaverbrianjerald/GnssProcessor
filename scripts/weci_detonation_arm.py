#!/usr/bin/env python3
"""Warm-start arms over the 19:26:00 SH detonation from the guards_cont history.

Derives an arm config from the anchor's OWN leg.json (fingerprint match by
construction) and a chosen history snapshot runs/guards_cont/seed/snap.txt.epNNNN:

  --arm null   stock resume (nothing changed) — the control that must REPRODUCE
               the detonation through the resume seam before any treated arm is
               readable (the 19:00-seam lesson: guards-armed resumes can mute it).
  --arm weci   same + network.raim_we_code_iono = true — the first ON run of the
               within-epoch code/iono screening, meeting the detonation live.

Anchor default ep0721 = 19:00:00 (tow 327600): ~52 epochs of post-resume settle
before the detonation at ep 773 (tow 329160).  Window default 2700 s
(19:00:30 .. 19:45:00) covers the detonation + the 19:34 spike + decay onset.

chain_make_leg discipline: the arm COPIES the history snapshot into its own dir
(reads AND writes its own file), half-epoch back-off on the offset, require=true.

Usage:
    python scripts/weci_detonation_arm.py --arm null [--anchor-ep 721] [--dur 2700]
    python scripts/weci_detonation_arm.py --arm weci
"""
import argparse
import json
import os
import shutil

ANCHOR_ROOT = r"C:\GnssProcessor\runs\guards_cont"
BASE = os.path.join(ANCHOR_ROOT, "leg.json")
OUT_ROOT = r"C:\GnssProcessor\runs\weci_det"
DAY_ANCHOR_S = 1399161600.0
HALF_EPOCH_BACKOFF_S = 15.0


def read_snapshot_header(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if line.startswith("M,"):
                f = line.rstrip("\n").split(",")
                return int(f[1]), float(f[2]), f[5]
    raise SystemExit(f"FATAL: {path} has no M header")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arm", choices=("null", "weci"), required=True)
    ap.add_argument("--anchor-ep", type=int, default=721,
                    help="guards_cont history epoch to resume FROM (721 = 19:00:00)")
    ap.add_argument("--dur", type=float, default=2700.0,
                    help="processing.max_duration_s from the first resumed epoch")
    args = ap.parse_args()

    snap_src = os.path.join(ANCHOR_ROOT, "seed", f"snap.txt.ep{args.anchor_ep:04d}")
    if not os.path.isfile(snap_src):
        raise SystemExit(f"FATAL: {snap_src} does not exist (anchor incomplete?)")
    week, tow, fp = read_snapshot_header(snap_src)
    exp_tow = 306000.0 + 30.0 * (args.anchor_ep - 1)
    if abs(tow - exp_tow) > 1.0:
        raise SystemExit(f"FATAL: ep{args.anchor_ep:04d} header tow {tow} != expected "
                         f"{exp_tow} — history numbering assumption broken, STOP")

    armdir = os.path.join(OUT_ROOT, args.arm)
    os.makedirs(os.path.join(armdir, "logs"), exist_ok=True)
    arm_snap = os.path.join(armdir, "snap.txt")
    shutil.copyfile(snap_src, arm_snap)

    cfg = json.load(open(BASE, encoding="utf-8"))
    net = cfg["network"]
    cfg["processing"]["max_duration_s"] = args.dur
    net["start_offset_hours"] = (week * 604800.0 + tow - DAY_ANCHOR_S
                                 + HALF_EPOCH_BACKOFF_S) / 3600.0
    net["state_snapshot_path"] = arm_snap.replace("\\", "/")
    net["state_snapshot_require"] = True
    net["state_snapshot_keep_history"] = False
    net["product_output_path"] = os.path.join(armdir, "products.txt").replace("\\", "/")
    if args.arm == "weci":
        net["raim_we_code_iono"] = True     # triggers: code 10.0 / iono 12.0 defaults
    out = os.path.join(armdir, "leg.json")
    with open(out, "w", encoding="utf-8") as fh:
        json.dump(cfg, fh, indent=2)
    print(f"arm {args.arm}: {out}")
    print(f"  anchor      ep{args.anchor_ep:04d} tow {tow:.0f} fp {fp}")
    print(f"  window      first ep tow {tow + 30.0:.0f} .. +{args.dur:.0f}s "
          f"(detonation tow 329160 = {(329160 - tow) / 30.0:+.0f} ep)")
    print(f"  treatment   raim_we_code_iono = {args.arm == 'weci'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
