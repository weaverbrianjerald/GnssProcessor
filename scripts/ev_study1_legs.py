#!/usr/bin/env python3
"""EVENT HARNESS Study 1 driver — make + run the erase-vs-refloat leg matrix.

One leg per (arc, mode).  Every leg resumes from the SAME snapshot as the
validation legs (``runs/evseed_quiet6h`` ep0665) with the same config except
``network.inject``, so ``scripts/event_grade.py`` can difference any of them
against the single NULL leg.

    python scripts/ev_study1_legs.py make            # write every leg.json
    python scripts/ev_study1_legs.py run  <name> ... # run these legs (<=3 at a time)
    python scripts/ev_study1_legs.py list

Concurrency is capped at 3 (the standing directive for this box).
"""
import argparse
import json
import os
import subprocess
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(_HERE)
EXE = os.path.join(REPO, "build", "x64-Release", "gnss_processor_study1b.exe")
SEED = "runs/evseed_quiet6h"
SNAP_EP = 665         # the validation legs' snapshot (state_snapshot.txt.ep0665, tow 279150)
EPOCHS = 60
AT_EP = 10

# (leg name, station, satellite, arc_erase mode | None for the null)
# A mode may carry a BAND SCOPE as "<mode>@<L..;L..>" (rung 2): the scoped legs
# free/erase only those signals, reproducing the 2-of-3 scope a real IF-TD
# detector refloat used on the validation slip leg.
ARC1 = ("S21000SIM", "G10")     # the validation arc (mean elevation 14.2 deg)
ARC2 = ("S50000SIM", "G31")     # different station, HIGH elevation (mean 81.3 deg)
ARC3 = ("S40000SIM", "G31")     # arc2's satellite at a LOW-elevation station (13.8 deg)

LEGS = [
    # byte-identity gate: a NULL leg on the new exe must reproduce evleg_null
    ("s1_nullgate", None, None, None),
    # arc 1 — the four diagnostic modes (erase / slip / null reuse the validation legs)
    ("s1_free",     ARC1[0], ARC1[1], "free_epoch"),
    ("s1_keepiono", ARC1[0], ARC1[1], "erase_keep_iono"),
    ("s1_noreseed", ARC1[0], ARC1[1], "erase_no_reseed"),
    ("s1_nofunnel", ARC1[0], ARC1[1], "erase_no_funnel"),
    # arc 2 — the decisive contrast on a different station
    ("s2_erase",    ARC2[0], ARC2[1], "erase"),
    ("s2_free",     ARC2[0], ARC2[1], "free_epoch"),
    ("s2_noreseed", ARC2[0], ARC2[1], "erase_no_reseed"),
    # arc 3 — the decisive contrast on a LOW-elevation arc
    ("s3_erase",    ARC3[0], ARC3[1], "erase"),
    ("s3_free",     ARC3[0], ARC3[1], "free_epoch"),
    ("s3_noreseed", ARC3[0], ARC3[1], "erase_no_reseed"),
    # rung 2 — BAND SCOPE: the same actions on 2 of 3 bands (the slip leg's scope)
    ("s1_free_l12",  ARC1[0], ARC1[1], "free_epoch@L1C;L2W"),
    ("s1_erase_l12", ARC1[0], ARC1[1], "erase@L1C;L2W"),
    ("s2_free_l12",  ARC2[0], ARC2[1], "free_epoch@L1C;L2W"),
]


def spec(name):
    for row in LEGS:
        if row[0] == name:
            return row
    raise SystemExit(f"unknown leg {name}; see `list`")


def cmd_make(args):
    for name, sta, sat, mode in LEGS:
        if args.only and name not in args.only:
            continue
        cmd = [sys.executable, os.path.join(_HERE, "event_leg.py")]
        if mode is None:
            cmd += ["null"]
        else:
            m, _, bands = mode.partition("@")
            ev = {"kind": "arc_erase", "station": sta, "sat": sat, "mode": m}
            if bands:
                ev["signal"] = bands
            cmd += ["make-leg", "--event", json.dumps(ev)]
        cmd += ["--seed-run", SEED, "--snapshot-ep", str(SNAP_EP),
                "--epochs", str(EPOCHS), "--at-ep", str(AT_EP),
                "--name", name, "--exe", EXE]
        rc = subprocess.call(cmd)
        if rc:
            raise SystemExit(f"make-leg failed for {name} (rc {rc})")
    return 0


def cmd_run(args):
    names = args.names
    if len(names) > 3:
        raise SystemExit("at most 3 concurrent legs (standing directive)")
    procs = []
    for name in names:
        spec(name)
        legdir = os.path.join(REPO, "runs", f"evleg_{name}")
        # rerun re-seeds from the pristine .epNNNN first (the §6b overwrite trap)
        cmd = [sys.executable, os.path.join(_HERE, "event_leg.py"), "rerun",
               legdir, "--exe", EXE]
        print(f"launch {name}")
        procs.append((name, time.time(),
                      subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT)))
    bad = 0
    for name, t0, p in procs:
        out, _ = p.communicate()
        dt = time.time() - t0
        tail = out.decode("utf-8", "replace").strip().splitlines()[-1:]
        print(f"  {name:14s} rc={p.returncode} {dt:6.1f}s   {' '.join(tail)}")
        if p.returncode:
            bad += 1
    return 1 if bad else 0


def cmd_list(args):
    for name, sta, sat, mode in LEGS:
        print(f"{name:14s} {sta or '-':11s} {sat or '-':4s} {mode or 'NULL'}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("make")
    m.add_argument("--only", nargs="*")
    m.set_defaults(func=cmd_make)
    r = sub.add_parser("run")
    r.add_argument("names", nargs="+")
    r.set_defaults(func=cmd_run)
    l = sub.add_parser("list")
    l.set_defaults(func=cmd_list)
    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
