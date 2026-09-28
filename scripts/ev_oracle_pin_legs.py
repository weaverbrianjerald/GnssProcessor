#!/usr/bin/env python3
"""Oracle-pin driver: the leg matrix for docs/SPEC_oracle_pin.md.

The card (SPEC_event_harness.md §17): hold some or all of the producer's float
ambiguities at the fixture's true integers and grade the remaining parameters
against truth. With the ambiguity/ionosphere degeneracy removed, the residual
error budget is the model's own; the partial columns are the coupling map,
showing which parameter absorbs the error that a pinned band or station no
longer carries.

Nine legs, all 60 epochs off the same snapshot, pin at +5:

    quiet seed (runs/evseed_quiet6h ep0665, fingerprint 6a3531a1afb2b40b)
      opin_null_base   base exe, no event     G1 reference
      opin_null        on   exe, no event     G1 (must be byte-identical) + the table's `none`
      opin_all         scope: all             the ceiling
      opin_l12         scope: band:L1C+L2W    L5Q free
      opin_l5          scope: band:L5Q        L1C/L2W free
      opin_s21         scope: station:S21000SIM
      opin_g08         scope: sat:G08
    slipped seed (runs/evseed_slipped6h ep0665)
      opin_slip_null   no event               G4 reference
      opin_slip_all    scope: all             G4

`oracle_truth_root` is armed on every leg, the nulls included, so the only
difference between a null and an arm is the event itself. The root is inert
without an `oracle_pin` event (and without `cycle_slip.oracle_relevel`), which
is exactly what gate G1 proves.

    python scripts/ev_oracle_pin_legs.py make
    python scripts/ev_oracle_pin_legs.py run opin_null_base opin_null
    python scripts/ev_oracle_pin_legs.py list
    python scripts/ev_oracle_pin_legs.py status

Concurrency is capped at two (this card's directive: another session may be
running a storm arm in the third slot), and completion is read from each leg's
`exitcode.txt`, never from the process list (memory
`feedback_detached_run_launchers`: a subagent's children can be reaped out from
under the launcher, and a `TaskStop` leaves the tree alive).
"""
import argparse
import json
import os
import subprocess
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(_HERE)

BASE_EXE = os.path.join(REPO, "build", "x64-Release", "gnss_processor_opin_base.exe")
PIN_EXE = os.path.join(REPO, "build", "x64-Release", "gnss_processor_opin.exe")
# The post-rebase exe. `opin_all_rb` is the rebase smoke leg: the leg derives from
# the frozen seed config, so the posture is unchanged by main's golden edits, and
# the run-end clause must reproduce `opin_all`'s counts exactly.
RB_EXE = os.path.join(REPO, "build", "x64-Release", "gnss_processor_opin_rb.exe")

QUIET_SEED = "C:/GnssProcessor/.claude/worktrees/evharness/runs/evseed_quiet6h"
QUIET_FIXTURE = "C:/GnssProcessor/runs/simnet60_quiet_wu93_24h"
SLIP_SEED = "C:/GnssProcessor/runs/evseed_slipped6h"
SLIP_FIXTURE = "C:/GnssProcessor/runs/simnet60_campaign_wu93"

SNAP_EP = 665
EPOCHS = 60
AT_EP = 5              # pre-registered in the §17 stub
MAX_CONCURRENT = 2

# (name, seed, fixture, exe, scope | None, one-line purpose)
LEGS = [
    ("opin_null_base", QUIET_SEED, QUIET_FIXTURE, BASE_EXE, None,
     "G1 reference — the UNMODIFIED exe, truth root armed, no event"),
    ("opin_null", QUIET_SEED, QUIET_FIXTURE, PIN_EXE, None,
     "G1 (byte-identical to the base) + the table's `none` column"),
    ("opin_all", QUIET_SEED, QUIET_FIXTURE, PIN_EXE, "all",
     "THE CEILING — every carried non-anchor arc held at truth"),
    ("opin_l12", QUIET_SEED, QUIET_FIXTURE, PIN_EXE, "band:L1C+L2W",
     "L5Q left free — where does its former error land?"),
    ("opin_l5", QUIET_SEED, QUIET_FIXTURE, PIN_EXE, "band:L5Q",
     "L1C/L2W left free — the complement of opin_l12"),
    ("opin_s21", QUIET_SEED, QUIET_FIXTURE, PIN_EXE, "station:S21000SIM",
     "one station — does ITS ZWD/clock improve while its neighbours do not?"),
    ("opin_g08", QUIET_SEED, QUIET_FIXTURE, PIN_EXE, "sat:G08",
     "one satellite — does the pin move its clock / phase bias only?"),
    ("opin_slip_null", SLIP_SEED, SLIP_FIXTURE, PIN_EXE, None,
     "G4 reference on the SLIPPED bed"),
    ("opin_slip_all", SLIP_SEED, SLIP_FIXTURE, PIN_EXE, "all",
     "G4 — the pin must FOLLOW N(t) through every truth slip on a pinned arc"),
    ("opin_all_rb", QUIET_SEED, QUIET_FIXTURE, RB_EXE, "all",
     "REBASE SMOKE — `all` on the post-rebase exe; the clause must reproduce opin_all"),
]


def spec(name):
    for row in LEGS:
        if row[0] == name:
            return row
    raise SystemExit(f"unknown leg {name}; see `list`")


def legdir(name):
    return os.path.join(REPO, "runs", f"evleg_{name}")


def _extra(fixture):
    """`--extra` deep-merges last, so it is how a leg arms a knob the seed's
    config predates. Armed on every leg, nulls included: the only difference
    between a null and an arm must be the event."""
    return json.dumps({"network": {"cycle_slip": {"oracle_truth_root": fixture}}})


def cmd_make(args):
    for name, seed, fixture, exe, scope, _why in LEGS:
        if args.only and name not in args.only:
            continue
        cmd = [sys.executable, os.path.join(_HERE, "event_leg.py")]
        if scope is None:
            cmd += ["null"]
        else:
            cmd += ["make-leg", "--event",
                    json.dumps({"kind": "oracle_pin", "scope": scope,
                                "sigma_m": 1.0e-4})]
        cmd += ["--seed-run", seed, "--snapshot-ep", str(SNAP_EP),
                "--epochs", str(EPOCHS), "--at-ep", str(AT_EP),
                "--name", name, "--exe", exe, "--extra", _extra(fixture)]
        rc = subprocess.call(cmd)
        if rc:
            raise SystemExit(f"make-leg failed for {name} (rc {rc})")
        # Record the exe this leg is for, so `run` cannot silently use the wrong
        # one (the G1 pair differs only in the binary).
        meta_path = os.path.join(legdir(name), "leg_meta.json")
        with open(meta_path, encoding="utf-8") as fh:
            meta = json.load(fh)
        meta["opin_exe"] = exe
        meta["opin_scope"] = scope
        meta["opin_fixture"] = fixture
        with open(meta_path, "w", encoding="utf-8") as fh:
            json.dump(meta, fh, indent=2)
        print(f"  {name:16s} exe={os.path.basename(exe)} scope={scope}")
    return 0


def _exitcode(name):
    """(rc, wall_s) from the leg's exitcode.txt, or (None, None)."""
    p = os.path.join(legdir(name), "exitcode.txt")
    if not os.path.isfile(p):
        return None, None
    try:
        with open(p, encoding="utf-8") as fh:
            lines = fh.read().split()
        return int(lines[0]), float(lines[1])
    except (ValueError, IndexError):
        return None, None


def cmd_run(args):
    names = args.names
    if len(names) > MAX_CONCURRENT:
        raise SystemExit(f"at most {MAX_CONCURRENT} concurrent legs "
                         "(this card's directive — another session may hold the third slot)")
    procs = []
    for name in names:
        _n, _s, _f, exe, _sc, _w = spec(name)
        d = legdir(name)
        if not os.path.isfile(os.path.join(d, "leg.json")):
            raise SystemExit(f"{name}: no leg.json — run `make` first")
        # event_leg.py `rerun` restores the pristine snapshot first: a leg writes
        # its own snapshot at its first emitted epoch, so relaunching the exe on
        # an existing leg.json resumes into an epoch the snapshot already holds
        # and is cleanly refused (SPEC_event_harness §11.1).
        for stale in ("exitcode.txt",):
            p = os.path.join(d, stale)
            if os.path.isfile(p):
                os.remove(p)
        cmd = [sys.executable, os.path.join(_HERE, "event_leg.py"), "rerun", d,
               "--exe", exe]
        print(f"launch {name:16s} exe={os.path.basename(exe)}")
        procs.append((name, time.time(),
                      subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT)))
    bad = 0
    for name, t0, p in procs:
        p.communicate()
        rc, wall = _exitcode(name)          # the file, never p.returncode
        print(f"  {name:16s} exitcode.txt rc={rc} wall={wall}s "
              f"(launcher {time.time() - t0:.1f}s)")
        if rc != 0:
            bad += 1
    return 1 if bad else 0


def oracle_pin_clause(name):
    """The run-end oracle-pin line from a leg's stdout, verbatim (or None).

    Standing invariant #4: this line, not the absence of a crash, is what says
    whether the arm actuated.  It is multi-line in the log (spdlog wraps nothing,
    but the message itself contains no newline), so one match is the whole line.
    """
    p = os.path.join(legdir(name), "stdout.txt")
    if not os.path.isfile(p):
        return None
    with open(p, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if "Network (ORACLE PIN):" in line and "active epoch(s)" in line:
                return line.rstrip("\n")
    return None


def cmd_status(args):
    for name, _seed, _fx, exe, scope, _why in LEGS:
        rc, wall = _exitcode(name)
        state = "not run" if rc is None else ("OK" if rc == 0 else f"FAIL rc={rc}")
        print(f"{name:16s} {state:10s} wall={wall}  exe={os.path.basename(exe)} "
              f"scope={scope}")
        cl = oracle_pin_clause(name)
        if cl:
            print(f"    {cl.strip()}")
    return 0


def cmd_list(args):
    for name, seed, fixture, exe, scope, why in LEGS:
        print(f"{name:16s} seed={os.path.basename(seed):16s} "
              f"exe={os.path.basename(exe):28s} scope={scope}\n"
              f"                 {why}")
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
    s = sub.add_parser("status")
    s.set_defaults(func=cmd_status)
    l = sub.add_parser("list")
    l.set_defaults(func=cmd_list)
    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
