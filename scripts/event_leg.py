#!/usr/bin/env python3
"""Event harness: snapshot seed writer and single-event resume-leg generator.

The harness converges the network once, then injects one simulated event and
continues for a few epochs to study its effects, so an event study takes minutes
instead of a day-long run.

Four subcommands:

``write-seed``
    Derive a snapshot-writer config from a golden (default
    ``config/golden/golden_sim_tri_w6.json``).  The seed run is the only expensive
    step of the harness: it converges the network and writes a snapshot every
    ``--interval-ep`` emitted epochs to
    ``runs/evseed_<name>/state_snapshot.txt.epNNNN`` (the history suffix is
    ``n_emitted``; ``networkWriteStateSnapshot`` in ``StateSnapshotWrite.cpp``).

``make-leg``
    Copy one ``.epNNNN`` snapshot into the leg dir twice: ``seed.txt``, the
    pristine record the exe is never pointed at, and ``state_snapshot.txt``, which
    ``network.state_snapshot_path`` names and the run may overwrite.  Then derive
    the resume config from the seed config and arm one ``network.inject`` event
    at ``snapshot_tow + (at_ep + 1)*30`` s (unless the event already carries a
    ``tow``).  The seed run's live file is never used, so a killed leg cannot
    destroy the state it resumed from.

    With two copies the exe never reads and writes the same file.  This matters
    because the producer's first snapshot write is unconditional (in
    ``StateSnapshotWrite.cpp`` the write fires while ``last_snapshot_write_s <
    0.0``, whatever ``state_snapshot_interval_s`` says): a single file would hold
    the leg's own second epoch after one run, and any relaunch on the same
    ``leg.json`` would be refused.  ``seed.txt``'s sha256 goes into
    ``leg_meta.json`` as ``snapshot_sha256``; only ``state_snapshot.txt`` is
    disposable, and ``rerun`` restores only that one.

``null``
    The same leg with ``network.inject`` absent.  Every event leg is graded
    against its null so the resume seam transient cancels.

``rerun``
    Restore a leg's disposable snapshot(s) from the verified pristine copy and run
    the leg again.

The consumer's warm start (card CC (B), ``SPEC_event_harness.md`` §24.4).  By
default ``write-seed`` arms ``network.closed_loop.state_snapshot``, so the producer
writes the closed-loop consumer's own filter-state snapshot beside every network
history copy: ``state_snapshot.txt.consumer.epNNNN``, same suffix, same tow (the
producer refuses the write when the consumer did not process that tow).
``make-leg`` and ``null`` copy it into the leg the same way as the network one
(``seed_consumer.txt`` pristine, ``consumer_snapshot.txt`` disposable), point
``realtime.state_snapshot_path`` at the disposable copy and park the consumer's own
write cadence (``realtime.state_snapshot_interval_s`` 1e9; its first write is
unconditional and lands on the disposable copy), so the leg's consumer resumes at
the seam instead of cold-starting; ``rerun`` restores that copy too.  A seed
without the file (any seed before v9) leaves the leg's consumer cold, and the leg
summary's ``consumer`` line says which case applies.  This affects consumer memory
only: products and the producer's snapshot are byte-identical with and without it.

The resume mechanics are those of ``scripts/chain_make_leg.py``: the leg's
``network.start_offset_hours`` is the self-correcting recurrence

    start_offset_hours = (week*604800 + tow - DAY_ANCHOR + 15) / 3600

with the +15 s half-epoch back-off (epoch elapsed times carry double-precision
residue and the fast-forward gate for ``network.start_offset_hours`` in
``ApplicationNetwork.cpp`` is a strict ``<``).  DAY_ANCHOR is the fixture's first
observation epoch (``fixture_first_obs_s``).  ``processing.max_duration_s`` is
anchored on the first processed epoch (the offset gate returns before the duration
gate is reached, in ``ApplicationNetwork.cpp``), so a leg's duration is relative to
the resume point, not to the file start.

The warm-restart posture is the one validated as leg E of the seam card
(``SPEC_synthetic_minimum_network.md`` §18.14): writer armed with
``state_snapshot_frame_capture`` + ``state_snapshot_coupling_capture`` (QK/QP),
reader armed with ``resume_frame_restore`` + ``resume_coupling_restore`` (only when
the seed lineage carries its loader interlocks) + ``readmit_seed_horizon_s`` +
``zwd_seam_reseed`` (only when the lineage carries the ZWD; RS-10).  Without the
reseed on such a lineage the ZWD channel restarts at the seam (0.06-0.08 m truth
rms for the whole leg against the seed's converged 0.008 m); ``--no-zwd-seam-reseed``
leaves the knob at the seed config's value, for A/Bs against old legs.  Discard the first ~5 epochs after the
resume; the default ``--at-ep 10`` puts the event past that transient.
"""
import argparse
import hashlib
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
# 259200) in continuous GPS seconds; the same anchor as chain_make_leg.py.
DAY_ANCHOR_S = 1399161600.0
EPOCH_INTERVAL_S = 30.0
HALF_EPOCH_BACKOFF_S = 15.0

DEFAULT_GOLDEN = os.path.join(REPO, "config", "golden", "golden_sim_tri_w6.json")
DEFAULT_EXE = os.path.join(REPO, "build", "x64-Release", "gnss_processor.exe")


# ---------------------------------------------------------------- utilities
def _fwd(p):
    return os.path.abspath(p).replace("\\", "/")


def _sha256(path):
    """sha256 of a file, streamed (a field snapshot runs to ~100 MB)."""
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


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


def read_rt_snapshot_header(path):
    """Return (week, tow) from an RT (consumer) snapshot's M line.

    Format (include/realtime/RtStateSnapshot.hpp):
    ``M,week,tow,wall_utc,fingerprint_hex,core_size,state_size``."""
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if line.startswith("M,"):
                f = line.rstrip("\n").split(",")
                return int(f[1]), float(f[2])
    raise SystemExit(f"FATAL: {path} has no readable M header (not an RT state snapshot?)")


def start_offset_hours(week, tow, anchor_s=None):
    """Offset so the first processed epoch is the snapshot's tow + 30 s.

    anchor_s = the fixture's first observation epoch in continuous GPS seconds
    (see fixture_first_obs_s); the half-epoch back-off keeps the strict `<` gate
    off the double-residue boundary."""
    if anchor_s is None:
        anchor_s = DAY_ANCHOR_S
    return (week * 604800.0 + tow - anchor_s + HALF_EPOCH_BACKOFF_S) / 3600.0
def fixture_first_obs_s(cfg):
    """Continuous GPS seconds of the fixture's first observation epoch.

    The producer's fast-forward gate (ApplicationNetwork.cpp, `start_offset_hours`)
    anchors on the first synced epoch of the observation files, not on the GPS day
    start.  This reads `TIME OF FIRST OBS` from the first station's RINEX header and
    falls back to DAY_ANCHOR_S, with a warning, only when the header cannot be read.
    A day-start anchor is wrong for any fixture that does not start at 00:00:00: a
    12:00 start offsets every leg by 12 h (the resume is refused), and a 00:00:30
    start resumes one epoch late (snapshot + 60 s).
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

    A dict value merges into an existing dict; anything else replaces.  Arrays
    therefore replace wholesale, which is the documented config-inheritance rule.
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
    # Also capture the linearization frame of the invariant DD carry (one ZL row per
    # block).  The serialized chord mean is the correction-frame DD (λN − Σ c·x_lin),
    # and a resumed run re-seeds every station ambiguity x_lin from code, so without
    # this row every restored chord folds wrong by Σ c·Δx_lin.  Write-side only:
    # products and traces are byte-identical, and a reader that finds no ZL row
    # behaves as before, so the knob is not fingerprinted.
    # (RS-2, SPEC_event_harness §21.4)
    net["state_snapshot_carry_lin"] = True
    # Also capture the two datum memories (the eq34 chain tree and the time-linked
    # code-bias datum), so a resume leg continues both datums instead of re-electing
    # and re-seeding them (110 -> 8 rerefs, 30 -> 0 CB discs).
    # (RS-3, SPEC_event_harness §21.7)
    net["state_snapshot_datum_capture"] = True
    # Also capture the emit layer (the eq34 emit re-base frame and the PB emission
    # filter state), so a resume leg's wire continues the writer's phase-bias levels
    # (17.7 m p50 -> 0.000 m at the first epoch).  (RS-5, SPEC_event_harness §21.10)
    net["state_snapshot_emit_capture"] = True
    # Also capture the station layer's memory: the rx-CB carry and the T5 per-LOS
    # iono deviation carry (RC/SD rows).  (RS-6a, SPEC_event_harness §21.13)
    net["state_snapshot_station_capture"] = True
    # Also capture the rx-CB carry's code-bias linearization point (RL rows beside
    # RC), the frame the inject gate judges the carry against.
    # (RS-9, SPEC_event_harness §21.18)
    net["state_snapshot_rxcb_lin_capture"] = True
    # Also capture every station's canonical-signal map (CN rows), the per-station
    # latch that a resume otherwise re-decides (on 23 of 61 stations).
    # (RS-8, SPEC_event_harness §21.16)
    net["state_snapshot_signal_capture"] = True
    # Also write the closed-loop consumer's RtStateSnapshot beside every network
    # history copy (state_snapshot.txt.consumer.epNNNN, same suffix, same tow), so a
    # leg's consumer can resume at the seam instead of cold-starting.  Consumer
    # memory only: the producer's products and its own snapshot are byte-identical
    # with it (the v9 field seed hashes v8's products).  Inert without a closed_loop
    # block.  (Card CC (B), SPEC_event_harness §24.4)
    net.setdefault("closed_loop", {})["state_snapshot"] = True
    net["state_snapshot_require"] = False          # the seed is the cold start
    net["product_output_path"] = _fwd(os.path.join(rundir, "products.txt"))
    net["write_run_manifest"] = True
    net["trace_dd_fractional"] = True
    net["trace_dd_fractional_epoch_stride"] = 1
    net["trace_events"] = True
    net["trace_epoch_status"] = True
    cfg.setdefault("logging", {})["enable_rt_engine"] = True

    # --extra is deep-merged last, so it can override anything this function set.
    # write-seed inherits the golden's config, which predates any knob added after
    # the golden was pinned; --extra arms those (trace-class ones in particular,
    # e.g. logging.enable_mw_slip_detector, which defaults to false once a logging
    # section exists).  It is recorded verbatim in seed_meta.json so the seed is
    # self-describing, as leg_meta.json does for make-leg.
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
        # The harness default that makes the consumer snapshots exist (Card CC (B)).
        "harness_consumer_snapshot": bool(net.get("closed_loop", {}).get("state_snapshot", False)),
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
    # Two copies, so the exe never reads and writes the same file.  The producer's
    # first snapshot write is unconditional (StateSnapshotWrite.cpp fires it while
    # `last_snapshot_write_s < 0.0`, whatever state_snapshot_interval_s says), so
    # the file `state_snapshot_path` names holds the leg's own second epoch as soon
    # as the run emits, and a relaunch on the same leg.json would resume from a
    # snapshot that already contains the target epoch (warmResumeAllowed's
    # non-positive-age refusal).  The exe is pointed at the disposable copy and the
    # pristine one stays out of its reach; `rerun` then restores only
    # state_snapshot.txt.
    pristine = os.path.join(legdir, "seed.txt")            # never handed to the exe
    leg_snap = os.path.join(legdir, "state_snapshot.txt")  # the exe may overwrite it
    shutil.copyfile(snap_src, pristine)
    shutil.copyfile(snap_src, leg_snap)
    snap_sha = _sha256(pristine)

    week, tow, n_emitted, fingerprint = read_snapshot_header(pristine)

    # The closed-loop consumer's snapshot beside the network copy, when the seed has
    # one (v9 and later).  Same two-copy layout, for the same reason: the exe's own
    # first cadence write is unconditional and has to land on the disposable copy.
    # (Card CC (B))
    cons_src = os.path.join(seed_run, f"state_snapshot.txt.consumer.ep{args.snapshot_ep:04d}")
    cons_pristine = cons_leg = cons_sha = cons_tow = None
    if os.path.isfile(cons_src):
        cons_pristine = os.path.join(legdir, "seed_consumer.txt")      # never handed to the exe
        cons_leg = os.path.join(legdir, "consumer_snapshot.txt")       # the exe may overwrite it
        shutil.copyfile(cons_src, cons_pristine)
        shutil.copyfile(cons_src, cons_leg)
        cons_sha = _sha256(cons_pristine)
        _cw, cons_tow = read_rt_snapshot_header(cons_pristine)
        if abs(cons_tow - tow) > 1e-3:
            print(f"WARNING: consumer snapshot tow {cons_tow!r} != network snapshot tow {tow!r} "
                  "(the producer should have refused this write) - the consumer resume may be "
                  "refused by its age gate")

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
    # Suppresses every write but the first, which is unconditional; that one lands
    # on the disposable copy above, never on seed.txt.
    net["state_snapshot_interval_s"] = 1.0e9
    net["resume_frame_restore"] = True
    # Coupling restore (§18.12, Card 1).  The loader throws on it unless the lineage
    # also carries the Card-1f interlocks (GnssConfigLoadNetwork.cpp, the three
    # throws under `if (network_resume_coupling_restore_)`): resume_frame_restore
    # (set just above, always satisfied here), carry_conditional_coupling and
    # carry_coupling_gauge_project.  The sim
    # golden carries 1f and the field golden does not, so an unconditional arm
    # would make every field leg fail to load.  The same keys are read off the
    # flattened seed config and the restore is armed only when they hold; --extra
    # still overrides in either direction.  The other resume_* defaults stay
    # unconditional on purpose: against a seed written without their rows they are
    # inert and log it, whereas this one is a hard config error.
    cpl_missing = []
    if not net.get("carry_conditional_coupling", False):
        cpl_missing.append("network.carry_conditional_coupling")
    if not net.get("carry_coupling_gauge_project", False):
        cpl_missing.append("network.carry_coupling_gauge_project (Card 1f)")
    net["resume_coupling_restore"] = not cpl_missing
    # Hold the restored branch through the first post-resume epoch: the D5 gate is
    # diagnostic-only for K = 1 epoch, so the cold station layer's one-epoch
    # disagreement (|v| ~11 m on 301 of 701 chords) does not erase a branch the data
    # agrees with from +2 on (rebirths at +2: 305 -> 7).  MW / LLI / IF-TD drains
    # stay live.  Inert without a restored blob.  (RS-2b, SPEC_event_harness §21.7)
    net["resume_d5_warmup_ep"] = 1
    # Restore the eq34 tree maps and the code-bias datum memory at the seam.  Inert
    # (logged) against a seed written without state_snapshot_datum_capture.
    # (RS-3, SPEC_event_harness §21.7)
    net["resume_datum_restore"] = True
    # Declare a phase-bias level re-realization at the first post-resume emit
    # (datum_changed for the constellation).  The CLK-R seam mark is suppressed once
    # the held frame is restored, so without this the seam is not declared.
    # (RS-4, SPEC_event_harness §21.10)
    net["resume_pb_seam_mark"] = True
    # Restore both emit engines at the seam.  Inert (logged) against a seed written
    # without state_snapshot_emit_capture.  (RS-5)
    net["resume_emit_restore"] = True
    # Restore both station-layer memories at the seam (the a00 seam transient
    # +4.6 -> +0.4 TECU).  Inert (logged) against a seed written without the RC/SD
    # rows.  (RS-6a)
    net["resume_station_restore"] = True
    # Restore the rx-CB carry's code-bias linearization point at the seam (withheld
    # share 26 % -> 10 %, against the seed's 11 %).  Inert (logged) against a seed
    # written without RL rows.  (RS-9)
    net["resume_rxcb_lin_restore"] = True
    # Restore the canonical-signal latch at the seam (chord births at +1: 64 -> 1).
    # Inert (logged) against a seed written without CN rows.  (RS-8)
    net["resume_signal_canon_restore"] = True
    # Ledger row network.zwd_seam_reseed (RECOMMEND_SCOPED, regime warm +
    # phase_leveling, "arm on the RESUME leg"): without it the ZWD channel restarts
    # at the seam.  On evseed_quiet6h ep0665 the null leg's ZWD truth rms sat at
    # 0.061-0.081 m for the whole 60-epoch leg against the seed's converged
    # 0.0076 m; armed, it reads 0.008-0.012 m from +1 on.  Legs generated before
    # 2026-09-11 graded ZWD (and the consumer vertical it feeds) on that restart.
    # --no-zwd-seam-reseed leaves the knob at the seed config's value, for a
    # like-for-like A/B against an old leg.
    # The reseed is only right where the continuous run carries the ZWD
    # (phase_leveling_local_block or carry_zwd).  On a lineage where the ZWD is a
    # per-epoch white sink (the field golden: no F3 block, no carry_zwd) the Z rows
    # are exact, but the B9 prior at q 1e-9 is ~4000x too stiff for a channel whose
    # own epoch step is 0.11 m p50 (zwd prior sigma0 11, refused), and from +2 the
    # harvest turns it into the FALSIFIED diagonal ZWD carry for 480 epochs (ZWD
    # leg-seed 0.29 -> 1.34 m over the hour, satellite-clock contrast p95 10.3 vs
    # 5.4).  It is therefore armed only where the lineage carries the ZWD; --extra
    # still overrides.  (RS-10, SPEC_event_harness §21.21)
    zwd_carried = bool(net.get("phase_leveling_local_block", False)) or bool(net.get("carry_zwd", False))
    if getattr(args, "no_zwd_seam_reseed", False):
        pass
    elif zwd_carried:
        net["zwd_seam_reseed"] = True
    else:
        net["zwd_seam_reseed"] = False
    net["product_output_path"] = _fwd(os.path.join(legdir, "products.txt"))
    net["write_run_manifest"] = True
    net["trace_dd_fractional"] = True
    net["trace_dd_fractional_epoch_stride"] = 1
    net["trace_events"] = True
    net["trace_epoch_status"] = True
    lg = cfg.setdefault("logging", {})
    lg["enable_ambiguity_trace"] = True
    lg["enable_rt_engine"] = True
    # Point the leg's consumer at the disposable consumer snapshot.  The engine
    # stages it at initialize and applies it at its first processed epoch (age 30 s
    # against the 300 s horizon; the fingerprint is the seed consumer's own config
    # facts).  Interval 1e9 parks every cadence write but the first unconditional
    # one, which lands on the disposable copy.  A seed without the file leaves the
    # realtime block untouched, and the consumer cold-starts.  (Card CC (B))
    if cons_leg is not None:
        rt = cfg.setdefault("realtime", {})
        rt["state_snapshot_path"] = _fwd(cons_leg)
        rt["state_snapshot_interval_s"] = 1.0e9
        # The loader's default restore scope "core" restores only the five core states
        # and re-creates the dynamic ones, so the leg's consumer is effectively cold
        # (14.4 m at k = 1, the cold trajectory to the digit).  "full" restores every
        # state, and the seam refloat has to be off (on, it re-opens every ambiguity
        # and is worse than cold at k = 11).  full + refloat off resumes 0.63 m from
        # the continuous consumer at k = 1.  (SPEC_event_harness §24.4 G2)
        rt["state_snapshot_restore_scope"] = "full"
        rt["state_snapshot_seam_refloat"] = False

    # Event tow: the snapshot's tow is the last epoch it recorded, so the leg's
    # first processed epoch is tow + 30 s.  --at-ep is counted from that first
    # epoch (at-ep 10 is the 11th epoch of the leg, past the ~5-epoch resume
    # transient).  An event that already carries a tow keeps it.
    first_tow = tow + EPOCH_INTERVAL_S
    ev_tow = first_tow + args.at_ep * EPOCH_INTERVAL_S
    if event_json is not None:
        ev = dict(event_json)
        ev.setdefault("tow", ev_tow)
        net["inject"] = {"enable": True, "events": [ev]}
    else:
        net.pop("inject", None)

    # --extra is deep-merged last, so it can override anything this function set.
    # make-leg inherits the seed's config, which predates any knob added after the
    # seed was written; --extra arms those.  It is recorded verbatim in
    # leg_meta.json so the leg is self-describing.
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
        "snapshot_sha256": snap_sha, "leg_snapshot": _fwd(leg_snap),
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
        # The consumer's warm start (None/False = no consumer snapshot in the seed,
        # and the leg's consumer cold-starts, as on any pre-v9 seed).  (Card CC (B))
        "harness_consumer_snapshot": cons_leg is not None,
        "consumer_snapshot": (cons_src if cons_leg is not None else None),
        "consumer_snapshot_sha256": cons_sha,
        "leg_consumer_snapshot": (_fwd(cons_leg) if cons_leg is not None else None),
        "consumer_snapshot_tow": cons_tow,
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
    if cpl_missing:
        forced = bool(net.get("resume_coupling_restore", False))
        print("  coupling        resume_coupling_restore "
              + ("ON (forced by --extra; the loader will THROW)" if forced else "OFF")
              + " - the seed's lineage does not carry "
              + ", ".join(cpl_missing))
    if cons_leg is not None:
        print(f"  consumer        {os.path.basename(cons_src)}  tow {cons_tow!r}"
              f" ({'== network' if abs(cons_tow - tow) <= 1e-3 else '!= NETWORK'})"
              f" -> realtime.state_snapshot_path = {os.path.basename(cons_leg)}"
              " (WARM start, scope full, seam refloat OFF; cadence parked at 1e9 s; sha256 "
              f"{cons_sha[:16]}...)")
    else:
        print("  consumer        no state_snapshot.txt.consumer.ep"
              f"{args.snapshot_ep:04d} in the seed -> the leg's consumer COLD-starts "
              "(pre-CC seed)")
    print(f"  config          {cfg_path}")
    print(f"RUN: pushd {legdir} && \"{args.exe}\" \"{cfg_path}\"")
    if args.run:
        return _run(args.exe, cfg_path, legdir)
    return 0
    if not getattr(args, "no_zwd_seam_reseed", False) and not zwd_carried:
        print("  zwd             zwd_seam_reseed OFF - the seed's lineage carries no ZWD memory "
              "(neither phase_leveling_local_block nor carry_zwd); RS-10")



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
    """Restore a leg's resume snapshot from a pristine copy and run it again.

    make-leg lays down two copies of the seed snapshot and points the exe only at
    the disposable `state_snapshot.txt`, so the exe never reads and writes the same
    file and `seed.txt` stays pristine for the life of the leg; this command
    restores only the copy the run consumed.

    A leg writes its own snapshot at its first emitted epoch
    (`state_snapshot_interval_s` does not suppress that first write), so after one
    run the exe-facing file holds the leg's own second epoch, and a relaunch on the
    same `leg.json` without a restore resumes to an epoch the snapshot already
    contains.  `warmResumeAllowed` refuses that with a one-line message and exit 1
    (SPEC_event_harness §11.1).  Do not invoke the exe on an existing `leg.json` by
    hand; use this command.

    Sources, in order: `leg_meta.json`'s `snapshot` (the seed run's own `.epNNNN`),
    else the leg's `seed.txt`.  Each is verified against the recorded
    `snapshot_sha256`, and the command refuses if neither pristine copy verifies.
    With no recorded sha256 (a leg from before the two-copy layout) only the seed
    run's `.epNNNN` is accepted, unverified.  `seed.txt` is never written, except
    when it is itself the exe-facing file.  The restore target is read from the
    leg's own `leg.json` (`network.state_snapshot_path`), so a leg generated before
    the split, whose exe-facing file is `seed.txt`, still reruns, with a warning.
    The consumer snapshot, when the leg has one, is restored the same way.
    """
    legdir = args.legdir
    if not os.path.isabs(legdir):
        legdir = os.path.join(REPO, legdir)
    meta_path = os.path.join(legdir, "leg_meta.json")
    if not os.path.isfile(meta_path):
        raise SystemExit(f"FATAL: no leg_meta.json in {legdir}")
    with open(meta_path, encoding="utf-8") as fh:
        meta = json.load(fh)
    cfg_path = os.path.join(legdir, "leg.json")
    if not os.path.isfile(cfg_path):
        raise SystemExit(f"FATAL: no leg.json in {legdir}")
    with open(cfg_path, encoding="utf-8") as fh:
        leg_cfg = json.load(fh)

    # Restore whatever file the leg's own config points the exe at.
    dst = (leg_cfg.get("network", {}).get("state_snapshot_path")
           or os.path.join(legdir, "state_snapshot.txt"))
    dst = os.path.abspath(dst)
    pristine = os.path.join(legdir, "seed.txt")
    if os.path.abspath(pristine) == dst:
        print("WARNING: this leg predates the two-copy layout - its exe-facing "
              "snapshot IS seed.txt, so the restore overwrites the pristine copy. "
              "Re-generate the leg with make-leg / null to get the split.")

    want = meta.get("snapshot_sha256")
    tried, use = [], None
    for cand in (meta.get("snapshot"), pristine):
        if not cand or not os.path.isfile(cand):
            tried.append(f"{cand}: missing")
            continue
        if want is None:
            if os.path.abspath(cand) == os.path.abspath(pristine):
                tried.append(f"{cand}: no snapshot_sha256 in leg_meta.json and this "
                             "leg predates the two-copy split, so its seed.txt "
                             "cannot be told from the run's own overwrite")
                continue
            use = cand            # leg without a recorded sha256: the seed run's .epNNNN only
            break
        got = _sha256(cand)
        if got == want:
            use = cand
            break
        tried.append(f"{cand}: sha256 {got[:16]}... != recorded {want[:16]}...")
    if use is None:
        raise SystemExit("FATAL: no verified pristine copy of this leg's snapshot:\n  "
                         + "\n  ".join(tried)
                         + "\n(re-generate the leg with make-leg / null)")

    if os.path.abspath(use) != dst:
        shutil.copyfile(use, dst)
    print(f"re-seeded {dst} from {use}"
          + ("" if want is None else f"  (sha256 {want[:16]}... verified)"))

    # The consumer's disposable snapshot, restored the same way (the leg's consumer
    # overwrote it with its own first unconditional cadence write).  Sources:
    # leg_meta's `consumer_snapshot` (the seed's .consumer.epNNNN), else the leg's
    # seed_consumer.txt; verified against `consumer_snapshot_sha256` when one is
    # recorded.  (Card CC (B))
    cdst = (leg_cfg.get("realtime", {}).get("state_snapshot_path")
            or meta.get("leg_consumer_snapshot"))
    if meta.get("harness_consumer_snapshot") and cdst:
        cdst = os.path.abspath(cdst)
        cwant = meta.get("consumer_snapshot_sha256")
        ctried, cuse = [], None
        for cand in (meta.get("consumer_snapshot"), os.path.join(legdir, "seed_consumer.txt")):
            if not cand or not os.path.isfile(cand):
                ctried.append(f"{cand}: missing")
                continue
            got = _sha256(cand)
            if cwant is None or got == cwant:
                cuse = cand
                break
            ctried.append(f"{cand}: sha256 {got[:16]}... != recorded {cwant[:16]}...")
        if cuse is None:
            raise SystemExit("FATAL: no verified pristine copy of this leg's CONSUMER snapshot:\n  "
                             + "\n  ".join(ctried)
                             + "\n(re-generate the leg with make-leg / null)")
        if os.path.abspath(cuse) != cdst:
            shutil.copyfile(cuse, cdst)
        print(f"re-seeded consumer {cdst} from {cuse}"
              + ("" if cwant is None else f"  (sha256 {cwant[:16]}... verified)"))
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
