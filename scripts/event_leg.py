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
``readmit_seed_horizon_s`` + (since 2026-09-11) ``zwd_seam_reseed``, without which
the ZWD channel restarts at the seam (0.06-0.08 m truth rms for the whole leg
against the seed's converged 0.008 m; ``--no-zwd-seam-reseed`` for old-leg A/Bs).  Discard the first ~5 epochs after the resume; the
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


def start_offset_hours(week, tow, anchor_s=None):
    """Offset so the first PROCESSED epoch is the snapshot's tow + 30 s.

    anchor_s = the fixture's first observation epoch in continuous GPS seconds
    (see fixture_first_obs_s); the half-epoch back-off keeps the strict `<` gate
    off the double-residue boundary."""
    if anchor_s is None:
        anchor_s = DAY_ANCHOR_S
    return (week * 604800.0 + tow - anchor_s + HALF_EPOCH_BACKOFF_S) / 3600.0
def fixture_first_obs_s(cfg):
    """Continuous GPS seconds of the fixture's FIRST observation epoch.

    The producer's fast-forward gate (ApplicationNetwork.cpp, `start_offset_hours`)
    anchors on the first SYNCED epoch of the observation files, not on the GPS day
    start.  Read `TIME OF FIRST OBS` from the first station's RINEX header; fall back
    to DAY_ANCHOR_S (with a loud warning) only when the header cannot be read.
    Caught 2026-09-11: the campaign fixture starts at 12:00, so every leg on it was
    offset 12 h and refused; the quiet fixture starts at 00:00:30, so its legs
    resumed ONE EPOCH LATE (snapshot + 60 s) without anyone noticing.
    """
    import datetime as _dt
    try:
        st = cfg.get("network", {}).get("stations", [])
        path = st[0]["rinex_path"]
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            for _ in range(200):
                line = fh.readline()
                if not line:
                    break
                if "TIME OF FIRST OBS" in line:
                    f = line.split()
                    y, mo, d, h, mi = (int(x) for x in f[:5])
                    s = float(f[5])
                    t = _dt.datetime(y, mo, d, h, mi) - _dt.datetime(1980, 1, 6)
                    return t.total_seconds() + s
                if "END OF HEADER" in line:
                    break
    except Exception as e:  # noqa: BLE001 - any failure falls back loudly
        print(f"WARNING: cannot read TIME OF FIRST OBS ({e}); anchoring on the GPS day start")
    print("WARNING: anchoring the resume offset on DAY_ANCHOR_S (day start) — only right for a "
          "fixture that starts at 00:00:00")
    return DAY_ANCHOR_S



def _flatten(base):
    try:
        cfg, chain = load_config(base)
    except ConfigChainError as e:
        raise SystemExit(f"cannot read base {base}: {e}")
    return cfg, chain


def _deep_merge(dst, src):
    """Recursive dict merge (same semantics as scripts/ev_dose_legs.py:deep_merge).

    A dict value merges INTO an existing dict; anything else REPLACES.  Arrays
    therefore replace wholesale — the documented config-inheritance rule.
    """
    for k, v in src.items():
        if isinstance(v, dict) and isinstance(dst.get(k), dict):
            _deep_merge(dst[k], v)
        else:
            dst[k] = v


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
    # RS-2 (SPEC_event_harness §21.4): also capture the invariant DD carry's
    # LINEARIZATION frame (one ZL row per block).  The serialized chord mean is the
    # CORRECTION-frame DD (λN − Σ c·x_lin) and a resumed run re-seeds every station
    # ambiguity x_lin from code, so without this row every restored chord folds wrong
    # by Σ c·Δx_lin.  Write-side only: products and traces are byte-identical, and a
    # reader that finds no ZL row behaves exactly as before, so it is not fingerprinted.
    net["state_snapshot_carry_lin"] = True
    # RS-3 (SPEC_event_harness §21.7): also capture the two DATUM memories - the eq34
    # chain tree and the time-linked code-bias datum - so a resume leg continues both
    # datums instead of re-electing / re-seeding them (110 -> 8 rerefs, 30 -> 0 CB discs).
    net["state_snapshot_datum_capture"] = True
    # RS-5 (SPEC_event_harness §21.10): also capture the EMIT layer - the eq34 emit
    # re-base frame and the PB emission filter state - so a resume leg's wire continues
    # the writer's phase-bias levels (17.7 m p50 -> 0.000 m at the first epoch).
    net["state_snapshot_emit_capture"] = True
    net["state_snapshot_require"] = False          # the seed IS the cold start
    net["product_output_path"] = _fwd(os.path.join(rundir, "products.txt"))
    net["write_run_manifest"] = True
    net["trace_dd_fractional"] = True
    net["trace_dd_fractional_epoch_stride"] = 1
    net["trace_events"] = True
    net["trace_epoch_status"] = True
    cfg.setdefault("logging", {})["enable_rt_engine"] = True

    # --extra: deep-merged LAST so it can override anything this function set.
    # write-seed inherits the GOLDEN's config, which predates any knob added
    # after the golden was pinned — arming those (trace-class ones especially,
    # e.g. logging.enable_mw_slip_detector, which defaults FALSE once a logging
    # section exists) is exactly what --extra is for.  Recorded verbatim in
    # seed_meta.json so the seed is self-describing, exactly as leg_meta.json
    # records make-leg's.
    extra = {}
    if getattr(args, "extra", ""):
        try:
            extra = json.loads(args.extra)
        except ValueError as e:
            raise SystemExit(f"FATAL: --extra is not valid JSON: {e}")
        if not isinstance(extra, dict):
            raise SystemExit("FATAL: --extra must be a JSON object")
        _deep_merge(cfg, extra)

    cfg_path = _write(cfg, os.path.join(rundir, "seed.json"))
    meta = {
        "base": args.base, "hours": args.hours,
        "interval_ep": args.interval_ep, "extra": extra, "exe": args.exe,
    }
    with open(os.path.join(rundir, "seed_meta.json"), "w", encoding="utf-8") as fh:
        json.dump(meta, fh, indent=2)
    n_ep = int(args.hours * 3600.0 / EPOCH_INTERVAL_S)
    print(f"seed rundir       {rundir}")
    print(f"  base chain      {args.base}"
          + (" <- " + " <- ".join(os.path.basename(c) for c in reversed(chain))
             if chain else ""))
    print(f"  hours           {args.hours}  (~{n_ep} epochs)")
    print(f"  snapshots       every {args.interval_ep} emitted epochs -> "
          f"{os.path.basename(snap)}.epNNNN")
    if extra:
        print(f"  EXTRA           {json.dumps(extra)}")
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

    cfg, _ = _flatten(seed_cfg_path)
    anchor_s = fixture_first_obs_s(cfg)
    offset_h = start_offset_hours(week, tow, anchor_s)
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
    # RS-2 (SPEC_event_harness §21.4): re-base the restored invariant DD branch onto THIS
    # run's linearization at the first post-resume DD prior build (ẑ += W·zl_writer −
    # zl_now).  Inert against a seed written without state_snapshot_carry_lin — the
    # producer says so loudly rather than silently keeping the writer's frame.  Same
    # forward-only resume-response exemption class as the two restores above.
    net["resume_carry_rebase"] = True
    # RS-2b (SPEC_event_harness §21.7): hold the restored branch through the first
    # post-resume epoch — the D5 gate is diagnostic-only for K = 1 epoch, so the cold
    # station layer's one-epoch disagreement (|v| ~11 m on 301 of 701 chords) does not
    # erase a branch the data agrees with from +2 on (rebirths at +2: 305 -> 7).  MW /
    # LLI / IF-TD drains stay live.  Inert without a restored blob.
    net["resume_d5_warmup_ep"] = 1
    # RS-3 (SPEC_event_harness §21.7): restore the eq34 tree maps and the code-bias
    # datum memory at the seam.  Inert (logged) against a seed written without
    # state_snapshot_datum_capture.
    net["resume_datum_restore"] = True
    # RS-4 (SPEC_event_harness §21.10): declare a phase-bias LEVEL re-realization at the
    # first post-resume emit (datum_changed for the constellation) — the CLK-R seam mark
    # is suppressed once the held frame is restored, so without this the seam is silent.
    net["resume_pb_seam_mark"] = True
    # RS-5: restore both emit engines at the seam.  Inert (logged) against a seed
    # written without state_snapshot_emit_capture.
    net["resume_emit_restore"] = True
    # Ledger row network.zwd_seam_reseed (RECOMMEND_SCOPED, regime warm +
    # phase_leveling, "arm on the RESUME leg"): without it the ZWD channel
    # RESTARTS at the seam.  Measured 2026-09-11 on evseed_quiet6h ep0665: the
    # null leg's ZWD truth rms sits at 0.061-0.081 m for the whole 60-epoch leg
    # against the seed's own converged 0.0076 m; armed, it reads 0.008-0.012 m
    # from +1 on.  Every leg generated before this date graded ZWD (and the
    # consumer vertical it feeds) on that restart.  --no-zwd-seam-reseed
    # reproduces the old posture for a like-for-like A/B against an old leg.
    if not getattr(args, "no_zwd_seam_reseed", False):
        net["zwd_seam_reseed"] = True
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

    # --extra: deep-merged LAST so it can override anything this function set.
    # make-leg inherits the SEED's config, which predates any knob added after
    # the seed was written — arming those is exactly what --extra is for.
    # Recorded verbatim in leg_meta.json so the leg is self-describing.
    extra = {}
    if getattr(args, "extra", ""):
        try:
            extra = json.loads(args.extra)
        except ValueError as e:
            raise SystemExit(f"FATAL: --extra is not valid JSON: {e}")
        if not isinstance(extra, dict):
            raise SystemExit("FATAL: --extra must be a JSON object")
        _deep_merge(cfg, extra)

    cfg_path = _write(cfg, os.path.join(legdir, "leg.json"))
    meta = {
        "legdir": legdir, "seed_run": seed_run, "snapshot": snap_src,
        "week": week, "snapshot_tow": tow, "n_emitted": n_emitted,
        "fingerprint": fingerprint, "start_offset_hours": offset_h,
        "fixture_first_obs_s": anchor_s,
        "epochs": args.epochs, "first_tow": first_tow,
        "event_tow": (ev_tow if event_json is not None else None),
        "at_ep": args.at_ep,
        "event": (net.get("inject", {}).get("events", [None])[0]
                  if event_json is not None else None),
        "max_duration_s": proc["max_duration_s"],
        "extra": extra,
    }
    with open(os.path.join(legdir, "leg_meta.json"), "w", encoding="utf-8") as fh:
        json.dump(meta, fh, indent=2)

    print(f"legdir            {legdir}")
    print(f"  snapshot        {os.path.basename(snap_src)}  "
          f"week/tow {week}/{tow!r}  n_emitted {n_emitted}")
    print(f"  fingerprint     {fingerprint}")
    print(f"  start_offset_h  {offset_h!r}   first epoch tow {first_tow!r}   "
          f"(anchored on the fixture's first obs, +{week * 604800.0 + tow + EPOCH_INTERVAL_S - anchor_s:.0f} s)")
    print(f"  epochs          {args.epochs}   max_duration_s {proc['max_duration_s']}")
    if event_json is not None:
        print(f"  EVENT           {json.dumps(meta['event'])}")
        print(f"  event tow       {ev_tow!r}  (= first + {args.at_ep} ep)")
    else:
        print("  EVENT           none (NULL control leg)")
    if extra:
        print(f"  EXTRA           {json.dumps(extra)}")
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

    ⚠ THE SNAPSHOT-OVERWRITE TRAP (SPEC_warm_resume_event_card §0.4).  A leg
    WRITES its own snapshot at its first emitted epoch —
    `state_snapshot_interval_s` does not suppress that first write — so after one
    run `seed.txt` holds the leg's OWN second epoch, and relaunching the exe on
    the same `leg.json` resumes to an epoch the snapshot already contains
    (`warmResumeAllowed`'s non-positive-age refusal).

    Since SPEC_event_harness §11.1 that refusal is a CLEAN refusal: one error line
    naming the reason and pointing here, and exit code 1.  Before that fix it was
    a `COLD START` warning followed by a 0xC0000409 fast-fail with the reason
    printed nowhere, which is how a relaunched leg could read as a cold one.
    Either way the leg does not run: never invoke the exe on an existing
    `leg.json`.  Use this, which restores the pristine copy first.
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
    s.add_argument("--extra", default="",
                   help="JSON object deep-merged into seed.json AFTER every "
                        "harness field (arms knobs the base golden predates)")
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
        p.add_argument("--no-zwd-seam-reseed", dest="no_zwd_seam_reseed",
                       action="store_true",
                       help="do NOT arm network.zwd_seam_reseed on the leg (the "
                            "pre-2026-09-11 posture: the ZWD channel restarts at "
                            "the seam; only for a like-for-like A/B against an "
                            "old leg)")
        p.add_argument("--extra", default="",
                       help="JSON object deep-merged into leg.json AFTER every "
                            "harness field (arms knobs the seed's config predates)")
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
