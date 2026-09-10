#!/usr/bin/env python3
"""EVENT HARNESS — snapshot seed writer + single-event resume-leg generator.

The point of the harness is stated by the directive that motivated it: *run the
network to convergence ONCE, then inject one simulated event and continue a few
epochs to study the effects — minutes, not day-long runs.*

Three subcommands:

``write-seed``
    Derive a snapshot-WRITER config from a golden (default the sim golden).  The
    seed run is the only expensive thing in the harness: it converges the network
    and drops a snapshot every ``--interval-ep`` epochs into
    ``runs/evseed_<name>/state_snapshot.txt.epNNNN`` (history keyed on
    ``n_emitted`` — ``NetworkRunContext.cpp:2045-2047``).

``make-leg``
    Copy ONE ``.epNNNN`` snapshot into ``runs/evleg_<name>/seed.txt`` (never the
    seed's live file — a killed leg must not be able to destroy the state it
    resumed from), derive the resume config from the seed config, and arm exactly
    one ``network.inject`` event at ``snapshot_tow + at_ep*30`` s.

``null``
    The same leg with ``network.inject`` absent.  Every event leg is graded
    against its null so the resume seam transient cancels.

Resume mechanics reused verbatim from ``scripts/chain_make_leg.py``: the leg's
``network.start_offset_hours`` is the self-correcting recurrence

    start_offset_hours = (week*604800 + tow - DAY_ANCHOR + 15) / 3600

with the +15 s half-epoch back-off (epoch elapsed times carry double-precision
residue and the fast-forward gate at ``ApplicationNetwork.cpp:1298-1302`` is a
strict ``<``).  ``processing.max_duration_s`` is anchored on the first PROCESSED
epoch (the offset gate returns before the duration gate is reached,
``ApplicationNetwork.cpp:1350-1360``), so a leg's duration is relative to the
resume point, not to the file start.

The warm-restart posture is the one validated as leg E of the seam card
(``SPEC_synthetic_minimum_network.md`` §18.14): writer armed with
``state_snapshot_frame_capture`` + ``state_snapshot_coupling_capture`` (QK/QP),
reader armed with ``resume_frame_restore`` + ``resume_coupling_restore`` +
``readmit_seed_horizon_s``.  Discard the first ~5 epochs after the resume; the
default ``--at-ep 10`` puts the event safely past that transient.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
from gnss_config_chain import load_config, ConfigChainError  # noqa: E402

REPO = os.path.dirname(_HERE)

# First synced epoch of DOY-129 = 2024-05-08 00:00:00 GPS (week 2313, tow
# 259200) in continuous GPS seconds.  Identical anchor to chain_make_leg.py.
DAY_ANCHOR_S = 1399161600.0
EPOCH_INTERVAL_S = 30.0
HALF_EPOCH_BACKOFF_S = 15.0

DEFAULT_GOLDEN = os.path.join(REPO, "config", "golden", "golden_sim_tri_w6.json")
DEFAULT_EXE = os.path.join(REPO, "build", "x64-Release", "gnss_processor.exe")


# ---------------------------------------------------------------- utilities
def _fwd(p):
    return os.path.abspath(p).replace("\\", "/")


def read_snapshot_header(path):
    """Return (week, tow, n_emitted, fingerprint) from a snapshot's M/N lines."""
    week = tow = n_emitted = None
    fingerprint = None
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if line.startswith("M,"):
                f = line.rstrip("\n").split(",")
                week, tow, fingerprint = int(f[1]), float(f[2]), f[5]
            elif line.startswith("N,"):
                n_emitted = int(line.split(",")[1])
                break
    if week is None or tow is None or n_emitted is None:
        raise SystemExit(f"FATAL: {path} has no readable M/N header")
    return week, tow, n_emitted, fingerprint


def start_offset_hours(week, tow):
    return (week * 604800.0 + tow - DAY_ANCHOR_S + HALF_EPOCH_BACKOFF_S) / 3600.0


def _flatten(base):
    try:
        cfg, chain = load_config(base)
    except ConfigChainError as e:
        raise SystemExit(f"cannot read base {base}: {e}")
    return cfg, chain


def _write(cfg, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(cfg, fh, indent=2)
    return path


def _run(exe, cfg_path, rundir):
    """Run the producer with cwd=rundir so ./logs/ cannot collide with another run."""
    os.makedirs(os.path.join(rundir, "logs"), exist_ok=True)
    t0 = time.time()
    with open(os.path.join(rundir, "stdout.txt"), "w", encoding="utf-8",
              errors="replace") as out:
        rc = subprocess.call([exe, cfg_path], cwd=rundir, stdout=out,
                             stderr=subprocess.STDOUT)
    dt = time.time() - t0
    with open(os.path.join(rundir, "exitcode.txt"), "w", encoding="utf-8") as fh:
        fh.write(f"{rc}\n{dt:.1f}\n")
    print(f"exit {rc}  ({dt:.1f}s)  {rundir}")
    return rc


# ------------------------------------------------------------- write-seed
def cmd_write_seed(args):
    name = args.name
    rundir = os.path.join(REPO, "runs", f"evseed_{name}")
    os.makedirs(rundir, exist_ok=True)
    cfg, chain = _flatten(args.base)

    snap = os.path.join(rundir, "state_snapshot.txt")
    net = cfg.setdefault("network", {})
    proc = cfg.setdefault("processing", {})

    proc["max_duration_s"] = args.hours * 3600.0
    net["start_offset_hours"] = 0.0
    net["state_snapshot_path"] = _fwd(snap)
    net["state_snapshot_interval_s"] = args.interval_ep * EPOCH_INTERVAL_S
    net["state_snapshot_keep_history"] = True
    net["state_snapshot_frame_capture"] = True
    net["state_snapshot_coupling_capture"] = True
    net["state_snapshot_require"] = False          # the seed IS the cold start
    net["product_output_path"] = _fwd(os.path.join(rundir, "products.txt"))
    net["write_run_manifest"] = True
    net["trace_dd_fractional"] = True
    net["trace_dd_fractional_epoch_stride"] = 1
    net["trace_events"] = True
    net["trace_epoch_status"] = True
    cfg.setdefault("logging", {})["enable_rt_engine"] = True

    cfg_path = _write(cfg, os.path.join(rundir, "seed.json"))
    n_ep = int(args.hours * 3600.0 / EPOCH_INTERVAL_S)
    print(f"seed rundir       {rundir}")
    print(f"  base chain      {args.base}"
          + (" <- " + " <- ".join(os.path.basename(c) for c in reversed(chain))
             if chain else ""))
    print(f"  hours           {args.hours}  (~{n_ep} epochs)")
    print(f"  snapshots       every {args.interval_ep} emitted epochs -> "
          f"{os.path.basename(snap)}.epNNNN")
    print(f"  config          {cfg_path}")
    print(f"RUN: pushd {rundir} && \"{args.exe}\" \"{cfg_path}\"")
    if args.run:
        return _run(args.exe, cfg_path, rundir)
    return 0


# ---------------------------------------------------------------- make-leg
def _leg_common(args, event_json):
    seed_run = args.seed_run
    if not os.path.isabs(seed_run):
        seed_run = os.path.join(REPO, seed_run)
    seed_cfg_path = os.path.join(seed_run, "seed.json")
    if not os.path.isfile(seed_cfg_path):
        raise SystemExit(f"FATAL: no seed.json in {seed_run} (run write-seed first)")
    snap_src = os.path.join(seed_run, f"state_snapshot.txt.ep{args.snapshot_ep:04d}")
    if not os.path.isfile(snap_src):
        avail = sorted(f for f in os.listdir(seed_run)
                       if f.startswith("state_snapshot.txt.ep"))
        raise SystemExit(f"FATAL: {snap_src} missing.  available: {avail}")

    legdir = os.path.join(REPO, "runs", f"evleg_{args.name}")
    os.makedirs(os.path.join(legdir, "logs"), exist_ok=True)
    leg_snap = os.path.join(legdir, "seed.txt")
    shutil.copyfile(snap_src, leg_snap)

    week, tow, n_emitted, fingerprint = read_snapshot_header(leg_snap)
    offset_h = start_offset_hours(week, tow)

    cfg, _ = _flatten(seed_cfg_path)
    net = cfg.setdefault("network", {})
    proc = cfg.setdefault("processing", {})

    proc["max_duration_s"] = EPOCH_INTERVAL_S * (args.epochs + 1)
    net["start_offset_hours"] = offset_h
    net["state_snapshot_path"] = _fwd(leg_snap)
    net["state_snapshot_require"] = True
    net["state_snapshot_keep_history"] = False
    net["state_snapshot_interval_s"] = 1.0e9      # never overwrite the leg's seed
    net["resume_frame_restore"] = True
    net["resume_coupling_restore"] = True
    net["product_output_path"] = _fwd(os.path.join(legdir, "products.txt"))
    net["write_run_manifest"] = True
    net["trace_dd_fractional"] = True
    net["trace_dd_fractional_epoch_stride"] = 1
    net["trace_events"] = True
    net["trace_epoch_status"] = True
    lg = cfg.setdefault("logging", {})
    lg["enable_ambiguity_trace"] = True
    lg["enable_rt_engine"] = True

    # Event tow: the snapshot's tow is the LAST epoch it recorded, so the leg's
    # first processed epoch is tow + 30 s.  --at-ep is counted from that first
    # epoch, i.e. at-ep 10 == the 11th epoch of the leg, well past the ~5-epoch
    # resume transient.
    first_tow = tow + EPOCH_INTERVAL_S
    ev_tow = first_tow + args.at_ep * EPOCH_INTERVAL_S
    if event_json is not None:
        ev = dict(event_json)
        ev.setdefault("tow", ev_tow)
        net["inject"] = {"enable": True, "events": [ev]}
    else:
        net.pop("inject", None)

    cfg_path = _write(cfg, os.path.join(legdir, "leg.json"))
    meta = {
        "legdir": legdir, "seed_run": seed_run, "snapshot": snap_src,
        "week": week, "snapshot_tow": tow, "n_emitted": n_emitted,
        "fingerprint": fingerprint, "start_offset_hours": offset_h,
        "epochs": args.epochs, "first_tow": first_tow,
        "event_tow": (ev_tow if event_json is not None else None),
        "at_ep": args.at_ep,
        "event": (net.get("inject", {}).get("events", [None])[0]
                  if event_json is not None else None),
        "max_duration_s": proc["max_duration_s"],
    }
    with open(os.path.join(legdir, "leg_meta.json"), "w", encoding="utf-8") as fh:
        json.dump(meta, fh, indent=2)

    print(f"legdir            {legdir}")
    print(f"  snapshot        {os.path.basename(snap_src)}  "
          f"week/tow {week}/{tow!r}  n_emitted {n_emitted}")
    print(f"  fingerprint     {fingerprint}")
    print(f"  start_offset_h  {offset_h!r}   first epoch tow {first_tow!r}")
    print(f"  epochs          {args.epochs}   max_duration_s {proc['max_duration_s']}")
    if event_json is not None:
        print(f"  EVENT           {json.dumps(meta['event'])}")
        print(f"  event tow       {ev_tow!r}  (= first + {args.at_ep} ep)")
    else:
        print("  EVENT           none (NULL control leg)")
    print(f"  config          {cfg_path}")
    print(f"RUN: pushd {legdir} && \"{args.exe}\" \"{cfg_path}\"")
    if args.run:
        return _run(args.exe, cfg_path, legdir)
    return 0


def cmd_make_leg(args):
    try:
        ev = json.loads(args.event)
    except ValueError as e:
        raise SystemExit(f"FATAL: --event is not valid JSON: {e}")
    if not isinstance(ev, dict) or "kind" not in ev:
        raise SystemExit("FATAL: --event must be a JSON object with a 'kind' field")
    return _leg_common(args, ev)


def cmd_null(args):
    return _leg_common(args, None)



# ------------------------------------------------------------------- rerun
def cmd_rerun(args):
    """Re-seed a leg from its recorded snapshot and run it again.

    ⚠ THE SNAPSHOT-OVERWRITE TRAP (SPEC_warm_resume_event_card §0.4), and it is
    NOT closed for this refusal reason.  A leg WRITES its own snapshot at its
    first emitted epoch — `state_snapshot_interval_s` does not suppress that
    first write — so after one run `seed.txt` holds the leg's OWN second epoch.
    Relaunching the exe on the same `leg.json` then hits
    `warmResumeAllowed`'s non-positive-age refusal, and that particular branch
    does NOT go through the HB-1 hard-error hook: `state_snapshot_require: true`
    warns and falls through to a COLD START (which then died 0xC0000409 when this
    was found, 2026-09-10).  So: never invoke the exe on an existing leg.json.
    Use this, which restores the pristine copy first.
    """
    legdir = args.legdir
    if not os.path.isabs(legdir):
        legdir = os.path.join(REPO, legdir)
    meta_path = os.path.join(legdir, "leg_meta.json")
    if not os.path.isfile(meta_path):
        raise SystemExit(f"FATAL: no leg_meta.json in {legdir}")
    with open(meta_path, encoding="utf-8") as fh:
        meta = json.load(fh)
    src = meta["snapshot"]
    if not os.path.isfile(src):
        raise SystemExit(f"FATAL: the leg's source snapshot {src} is gone")
    shutil.copyfile(src, os.path.join(legdir, "seed.txt"))
    cfg_path = os.path.join(legdir, "leg.json")
    print(f"re-seeded {legdir}/seed.txt from {os.path.basename(src)}")
    return _run(args.exe, cfg_path, legdir)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("write-seed", help="derive + optionally run a snapshot-writer config")
    s.add_argument("--name", required=True)
    s.add_argument("--base", default=DEFAULT_GOLDEN)
    s.add_argument("--hours", type=float, default=6.0)
    s.add_argument("--interval-ep", type=int, default=60,
                   help="snapshot cadence in EMITTED epochs (default 60 = 1800 s)")
    s.add_argument("--exe", default=DEFAULT_EXE)
    s.add_argument("--run", action="store_true")
    s.set_defaults(func=cmd_write_seed)

    for nm, fn, need_ev in (("make-leg", cmd_make_leg, True), ("null", cmd_null, False)):
        p = sub.add_parser(nm, help=("one-event resume leg" if need_ev
                                     else "the no-event control leg"))
        p.add_argument("--seed-run", required=True)
        p.add_argument("--snapshot-ep", type=int, required=True)
        p.add_argument("--epochs", type=int, default=60)
        p.add_argument("--name", required=True)
        p.add_argument("--at-ep", type=int, default=10,
                       help="event fires this many epochs after the leg's first "
                            "epoch (default 10 — past the ~5-epoch resume transient)")
        p.add_argument("--exe", default=DEFAULT_EXE)
        p.add_argument("--run", action="store_true")
        if need_ev:
            p.add_argument("--event", required=True,
                           help="one event object as JSON; 'tow' is filled in from "
                                "the snapshot + --at-ep if absent")
        p.set_defaults(func=fn)

    r = sub.add_parser("rerun", help="re-seed a leg from its snapshot and run it again")
    r.add_argument("legdir")
    r.add_argument("--exe", default=DEFAULT_EXE)
    r.set_defaults(func=cmd_rerun)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
