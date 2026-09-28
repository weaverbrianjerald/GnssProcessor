#!/usr/bin/env python3
"""Event Harness Study 3 driver: the band-scoped response leg matrix (§Z).

Study 1 measured that the persistent ~0.45-cycle level floor an erase or a
whole-satellite refloat leaves is bought by the band scope and nothing else on
the erase path: restrict either to 2 of 3 bands and the floor becomes ~0.05.
This matrix grades the production knobs that make that the default response:

    network.raim_erase_band_scope        = min_keep_one
    network.cycle_slip.refloat_band_scope = implicated

Every leg resumes from the same snapshot as the §6/§9 validation legs
(``runs/evseed_quiet6h`` ep0665, 61 epochs, event at +10) and is graded against
the same null, so the warm-resume seam transient cancels. Legs that differ only
in the two knobs are direct A/Bs; the off arms are the §6/§9 legs already on
disk (``evleg_null``, ``evleg_erase``, ``evleg_s2_erase``, ``evleg_s3_erase``,
``evleg_slip``): nothing is re-run that already exists at the same settings.

    python scripts/ev_study3_legs.py make
    python scripts/ev_study3_legs.py run z_a1erase_on z_a2erase_on z_a3erase_on
    python scripts/ev_study3_legs.py list

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
EXE = os.path.join(REPO, "build", "x64-Release", "gnss_processor_bscope.exe")
SEED = "runs/evseed_quiet6h"
SNAP_EP = 665
EPOCHS = 60
AT_EP = 10

ARC1 = ("S21000SIM", "G10")     # the validation arc (mean elevation 14.2 deg)
ARC2 = ("S50000SIM", "G31")     # different station, high elevation (81.3 deg)
ARC3 = ("S40000SIM", "G31")     # arc2's satellite at a LOW station (13.8 deg)

# The three bands of a GPS triple arc on this posture, and the cycle counts the
# card names for the all-band injected slip. LLI is raised on all three because
# that is the only detector that implicates the whole arc: the IF-TD pair and MW
# both act on (L1C, L2W) and would leave L5Q pinned by their natural scope, i.e.
# they never reach the all-band fallback this leg exists to price. A receiver
# loss of lock on the whole satellite is exactly the physical case.
TRIPLE = [("L1C", 10.0), ("L2W", 8.0), ("L5Q", 6.0)]


def _slip(sta, sat, sig, dn, lli=True):
    ev = {"kind": "slip", "station": sta, "sat": sat, "signal": sig,
          "delta_cycles": dn}
    if lli:
        ev["flag_lli"] = True
    return ev


def _erase(sta, sat):
    return {"kind": "arc_erase", "station": sta, "sat": sat}


# (name, events | None, band-scope knobs on?, one-line purpose)
LEGS = [
    ("z_null_on", None, True,
     "null with both knobs armed — they must be INERT on a quiet station"),
    ("z_a1erase_on", [_erase(*ARC1)], True,
     "T4 path, arc 1 (14.2 deg): the 0.445 floor vs evleg_erase"),
    ("z_a2erase_on", [_erase(*ARC2)], True,
     "T4 path, arc 2 (81.3 deg): vs evleg_s2_erase (floor 0.130)"),
    ("z_a3erase_on", [_erase(*ARC3)], True,
     "T4 path, arc 3 (13.8 deg): vs evleg_s3_erase (floor 0.375)"),
    ("z_slip3_off", [_slip(*ARC1, s, d) for s, d in TRIPLE], False,
     "ALL-BAND injected slip, knobs off — the control for the fallback"),
    ("z_slip3_on", [_slip(*ARC1, s, d) for s, d in TRIPLE], True,
     "ALL-BAND injected slip, knobs on — what the kept band COSTS"),
    # flag_lli is left out here on purpose: `runs/evleg_slip` (the §6(2) control
    # this leg is graded against) does not set it, and a control that differed in
    # two variables would grade the LLI tier, not the band scope.
    ("z_slip1_on", [_slip(*ARC1, "L1C", 10.0, lli=False)], True,
     "single-band slip control — the knobs must not perturb evleg_slip"),
]


def spec(name):
    for row in LEGS:
        if row[0] == name:
            return row
    raise SystemExit(f"unknown leg {name}; see `list`")


def _patch(legdir, events, bscope):
    """Post-process the generated leg.json: arm the knobs and (for a multi-event
    leg) replace the single-event schedule event_leg.py wrote.

    event_leg.py takes one --event; the all-band slip needs three at the same
    tow. The tow it derived is recorded in leg_meta.json, so the multi-event
    schedule is written against the same epoch the single-event one would have
    fired on, so the legs stay directly comparable.
    """
    cfg_path = os.path.join(legdir, "leg.json")
    meta_path = os.path.join(legdir, "leg_meta.json")
    with open(cfg_path, encoding="utf-8") as fh:
        cfg = json.load(fh)
    with open(meta_path, encoding="utf-8") as fh:
        meta = json.load(fh)
    net = cfg["network"]
    if bscope:
        net["raim_erase_band_scope"] = "min_keep_one"
        net.setdefault("cycle_slip", {})["refloat_band_scope"] = "implicated"
    if events:
        tow = meta["event_tow"]
        evs = [dict(e, tow=tow) for e in events]
        net["inject"] = {"enable": True, "events": evs}
        meta["event"] = evs
    with open(cfg_path, "w", encoding="utf-8") as fh:
        json.dump(cfg, fh, indent=2)
    with open(meta_path, "w", encoding="utf-8") as fh:
        json.dump(meta, fh, indent=2)


def cmd_make(args):
    for name, events, bscope, _why in LEGS:
        if args.only and name not in args.only:
            continue
        cmd = [sys.executable, os.path.join(_HERE, "event_leg.py")]
        if events is None:
            cmd += ["null"]
        else:
            # The first event carries the schedule; _patch rewrites the array for
            # a multi-event leg (and is a no-op rewrite for a single one).
            cmd += ["make-leg", "--event", json.dumps(events[0])]
        cmd += ["--seed-run", SEED, "--snapshot-ep", str(SNAP_EP),
                "--epochs", str(EPOCHS), "--at-ep", str(AT_EP),
                "--name", name, "--exe", EXE]
        rc = subprocess.call(cmd)
        if rc:
            raise SystemExit(f"make-leg failed for {name} (rc {rc})")
        _patch(os.path.join(REPO, "runs", f"evleg_{name}"), events, bscope)
        print(f"  patched {name}: bscope={'on' if bscope else 'off'} "
              f"events={0 if events is None else len(events)}")
    return 0


def cmd_run(args):
    names = args.names
    if len(names) > 3:
        raise SystemExit("at most 3 concurrent legs (standing directive)")
    procs = []
    for name in names:
        spec(name)
        legdir = os.path.join(REPO, "runs", f"evleg_{name}")
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
    for name, events, bscope, why in LEGS:
        print(f"{name:14s} bscope={'ON ' if bscope else 'OFF'} "
              f"events={0 if events is None else len(events)}  {why}")
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
