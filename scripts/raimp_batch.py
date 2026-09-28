#!/usr/bin/env python3
"""RAIM POWER leg batch driver (docs/SPEC_raim_pool_rehost.md §7.2 + the P0' extension).

    python scripts/raimp_batch.py [--extension] [--only name,name] [--dry-run]
                                  [--status-json PATH]

Legs from the v8 field seed (`runs/evseed_field6h_v8` ep0604, 60 epochs), run
sequentially under the box-wide producer slot lock (`scripts/ev_slot.py`), one
producer at a time, from C:/GnssProcessor with the main session's harness
(`scripts/event_leg.py`) into C:/GnssProcessor/runs/evleg_<name>/.

The RAIM-only slip shape (§7.2): equal cycles (n, n, n) on L1C / L2W / L5X of
G24 at GOPE00CZE at the event epoch, no LLI. Every MW pair is identically
zero, the (1,2) IF-TD step is 0.107·n m against the chain's bar, GF is not
armed.  ``--event`` carries the L1C slip (the harness records it as the leg's
event); ``--extra`` carries the full ``network.inject`` block with all three
slips plus the knob(s) under test.  The extra's events must carry ``tow``
explicitly: the harness fills ``tow`` on the ``--event`` object only, and the
extra's array replaces the harness's (config-inheritance rule: arrays replace).

The ten pre-registered legs (event at ``--at-ep 10`` = leg epoch 11, tow
277620.00000810623), in order:
  raimp_null_screen                (no event, raim_carry_invariant: true)
  for n in 1, 2, 4:
    raimp_n<n>_off                 (inject only, the seed posture; loader inert line)
    raimp_n<n>_screen              (inject + raim_carry_invariant: true)
    raimp_n<n>_act                 (inject + raim_carry_invariant + _refloat: true)

The P0' extension (2026-09-13): the (1,2) IF-TD bar on this posture is
its physical floor λ_IF(1,1) = 0.106953 m, exactly the step an equal-count
n = 1 slip adds to the chain, so n = 1 is caught or missed by the sign of the
chain's own noise at the event epoch, a coin flip per epoch, not a blind
shape.  ``--extension`` appends n = 1 at three more event epochs from the same
snapshot, off and act arms (a screen-only leg is byte-identical to off unless
it flags):
  for at_ep in 14, 18, 22:   raimp_n1e<at_ep>_off, raimp_n1e<at_ep>_act

Per leg the driver records the harness sha256 before the launch (the batch is
only valid if it never moves: SPEC_event_harness §21.21, "harness defaults are
shared state"), claims a slot (``claim raimp --max-wait 540``; exit 3 = call
again), runs the harness with ``--run`` (blocks until the exe exits), releases
the slot once ``exitcode.txt`` exists, and appends the leg's wall time, rc and
harness sha to the status JSON.  A leg whose ``exitcode.txt`` already reads 0 is
skipped, so a relaunch after an interruption resumes rather than re-runs.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import time

REPO = "C:/GnssProcessor"
EXE = f"{REPO}/build/x64-Release/gnss_processor_raimp.exe"
EXE_SHA = "af418a9ff412626fd3c585f0a0910633aa4057d5f2c74a5b05672f1451c4813f"
HARNESS = f"{REPO}/scripts/event_leg.py"
HARNESS_SHA16 = "e6071f43"
SLOT = f"{REPO}/scripts/ev_slot.py"
SEED = "runs/evseed_field6h_v8"
SNAP_EP = 604
SNAP_TOW = 277290.00000810623          # the ep0604 snapshot's M-line tow
EPOCHS = 60
STATION, SAT = "GOPE00CZE", "G24"
BANDS = ("L1C", "L2W", "L5X")
EXT_AT_EPS = (14, 18, 22)


def event_tow(at_ep):
    """The harness's own arithmetic: first processed epoch = snapshot + 30 s."""
    return SNAP_TOW + 30.0 + at_ep * 30.0


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def slip(sig, n, tow):
    return {"kind": "slip", "station": STATION, "sat": SAT, "signal": sig,
            "delta_cycles": n, "tow": tow}


def legs(extension=False):
    """[(name, at_ep, event_json_or_None, extra_dict)] in run order."""
    out = [("raimp_null_screen", 10, None, {"network": {"raim_carry_invariant": True}})]
    for n in (1, 2, 4):
        tow = event_tow(10)
        inject = {"enable": True, "events": [slip(s, n, tow) for s in BANDS]}
        ev = slip("L1C", n, tow)
        out.append((f"raimp_n{n}_off", 10, ev, {"network": {"inject": inject}}))
        out.append((f"raimp_n{n}_screen", 10, ev,
                    {"network": {"inject": inject, "raim_carry_invariant": True}}))
        out.append((f"raimp_n{n}_act", 10, ev,
                    {"network": {"inject": inject, "raim_carry_invariant": True,
                                 "raim_carry_invariant_refloat": True}}))
    if extension:
        for at_ep in EXT_AT_EPS:
            tow = event_tow(at_ep)
            inject = {"enable": True, "events": [slip(s, 1, tow) for s in BANDS]}
            ev = slip("L1C", 1, tow)
            out.append((f"raimp_n1e{at_ep}_off", at_ep, ev, {"network": {"inject": inject}}))
            out.append((f"raimp_n1e{at_ep}_act", at_ep, ev,
                        {"network": {"inject": inject, "raim_carry_invariant": True,
                                     "raim_carry_invariant_refloat": True}}))
    return out


def harness_cmd(name, at_ep, ev, extra):
    sub = "null" if ev is None else "make-leg"
    cmd = [sys.executable, HARNESS, sub, "--seed-run", SEED, "--snapshot-ep",
           str(SNAP_EP), "--epochs", str(EPOCHS), "--at-ep", str(at_ep), "--name", name,
           "--exe", EXE, "--run", "--extra", json.dumps(extra)]
    if ev is not None:
        cmd[3:3] = ["--event", json.dumps(ev)]
    return cmd


def claim():
    """Block until a producer slot is ours; return the lock path."""
    while True:
        p = subprocess.run([sys.executable, SLOT, "claim", "raimp", "--max-wait", "540"],
                           capture_output=True, text=True, cwd=REPO)
        if p.returncode == 0:
            lines = [ln.strip() for ln in p.stdout.splitlines() if ln.strip()]
            lock = next((ln for ln in reversed(lines) if ln.lower().endswith(".lock")), None)
            if lock:
                return lock
            print(f"claim exit 0 but no lock path in stdout: {p.stdout!r}", flush=True)
        elif p.returncode != 3:
            print(f"claim rc {p.returncode}: {p.stdout!r} {p.stderr!r}", flush=True)
        print(f"[{time.strftime('%H:%M:%S')}] no slot yet — calling claim again", flush=True)
        time.sleep(5)


def release(lock):
    p = subprocess.run([sys.executable, SLOT, "release", lock],
                       capture_output=True, text=True, cwd=REPO)
    print(f"release rc {p.returncode}: {p.stdout.strip()} {p.stderr.strip()}", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--extension", action="store_true",
                    help="append the P0' legs (n = 1 at --at-ep 14 / 18 / 22, off + act)")
    ap.add_argument("--only", default="", help="comma-separated leg names to run")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--status-json", default=f"{REPO}/runs/_raimp_batch_status.json")
    a = ap.parse_args()

    status = {"started": time.strftime("%Y-%m-%dT%H:%M:%S"), "exe": EXE,
              "extension": a.extension, "legs": [], "done": False}
    exe_sha = sha256(EXE)
    status["exe_sha256"] = exe_sha
    if exe_sha != EXE_SHA:
        raise SystemExit(f"FATAL: {EXE} sha256 {exe_sha} != certified {EXE_SHA}")
    status["harness_sha256_start"] = sha256(HARNESS)
    print(f"exe {EXE} sha256 {exe_sha}", flush=True)
    print(f"harness {HARNESS} sha256 {status['harness_sha256_start']}", flush=True)
    if not status["harness_sha256_start"].startswith(HARNESS_SHA16):
        print(f"WARNING: harness sha does not start with {HARNESS_SHA16} — record it", flush=True)

    def dump():
        with open(a.status_json, "w", encoding="utf-8") as fh:
            json.dump(status, fh, indent=1)

    only = {s.strip() for s in a.only.split(",") if s.strip()}
    for name, at_ep, ev, extra in legs(a.extension):
        if only and name not in only:
            continue
        legdir = f"{REPO}/runs/evleg_{name}"
        ec = os.path.join(legdir, "exitcode.txt")
        if os.path.isfile(ec) and open(ec).read().split()[:1] == ["0"]:
            print(f"SKIP {name}: exitcode.txt already 0", flush=True)
            status["legs"].append({"name": name, "skipped": True})
            dump()
            continue
        cmd = harness_cmd(name, at_ep, ev, extra)
        rec = {"name": name, "at_ep": at_ep, "event_tow": (event_tow(at_ep) if ev else None),
               "cmd": cmd, "harness_sha256_before": sha256(HARNESS)}
        print(f"\n=== {name} ===\n  harness sha {rec['harness_sha256_before'][:16]}\n  "
              + " ".join(json.dumps(c) if c.startswith("{") else c for c in cmd), flush=True)
        if a.dry_run:
            status["legs"].append(rec)
            continue
        lock = claim()
        rec["lock"] = lock
        rec["t_start"] = time.strftime("%Y-%m-%dT%H:%M:%S")
        t0 = time.time()
        print(f"  slot {lock} claimed at {rec['t_start']}", flush=True)
        try:
            p = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True,
                               encoding="utf-8", errors="replace")
            rec["harness_rc"] = p.returncode
            rec["harness_stdout"] = p.stdout[-4000:]
            rec["harness_stderr"] = p.stderr[-4000:]
            # the harness writes exitcode.txt before returning; wait briefly regardless
            for _ in range(60):
                if os.path.isfile(ec):
                    break
                time.sleep(1)
        finally:
            release(lock)
        rec["t_end"] = time.strftime("%Y-%m-%dT%H:%M:%S")
        rec["wall_s"] = round(time.time() - t0, 1)
        rec["exitcode_txt"] = open(ec).read().split() if os.path.isfile(ec) else None
        rec["harness_sha256_after"] = sha256(HARNESS)
        print(f"  done rc {rec['harness_rc']} exitcode.txt {rec['exitcode_txt']} "
              f"wall {rec['wall_s']} s", flush=True)
        print("  " + p.stdout.strip().replace("\n", "\n  ")[-1500:], flush=True)
        status["legs"].append(rec)
        dump()

    status["harness_sha256_end"] = sha256(HARNESS)
    status["done"] = True
    status["finished"] = time.strftime("%Y-%m-%dT%H:%M:%S")
    dump()
    print(f"\nBATCH DONE harness sha end {status['harness_sha256_end']}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
