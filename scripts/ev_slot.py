#!/usr/bin/env python3
"""Box-wide PRODUCER SLOT locks for the event harness (standing cap: 2 concurrent gnss_processor runs).

    python scripts/ev_slot.py claim <worker> [--max-wait S]   # blocks until a slot is free (default 540 s), prints the lock path;
                                                            # exit 3 = no slot within the wait: just call it again
    python scripts/ev_slot.py release <lockpath>  # after the run's exitcode.txt appeared
    python scripts/ev_slot.py status

Why: several worker sessions launch producers concurrently, each polling the process list before a
launch; two of them can read "1 running" in the same second and launch together (3 on the box).  A
lock file created atomically (O_CREAT|O_EXCL) is a real mutex.  Two slots, `runs/_slots/slot{1,2}.lock`,
content = worker name, pid, ISO time.  A lock older than STALE_H hours is assumed abandoned (a dead
worker) and is broken with a printed warning.  `claim` ALSO refuses to proceed while >= 2
gnss_processor*.exe are running (another session that does not use the locks), so the lock is
necessary but not sufficient — the process count stays the backstop.
"""
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SLOT_DIR = os.path.join(ROOT, "runs", "_slots")
N_SLOTS = 2
STALE_H = 4.0
POLL_S = 15


def _running():
    """Count gnss_processor*.exe processes (tasklist; 0 on any failure)."""
    try:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq gnss_processor*"],
                             capture_output=True, text=True, timeout=30).stdout
    except Exception:
        return 0
    return sum(1 for ln in out.splitlines() if ln.lower().startswith("gnss_processor"))


def _lock_path(i):
    return os.path.join(SLOT_DIR, f"slot{i}.lock")


def _try_claim(i, who):
    p = _lock_path(i)
    try:
        fd = os.open(p, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        try:
            age_h = (time.time() - os.path.getmtime(p)) / 3600.0
        except OSError:
            return None
        if age_h > STALE_H:
            print(f"WARNING: breaking stale lock {p} ({age_h:.1f} h old)", flush=True)
            try:
                os.remove(p)
            except OSError:
                return None
            return _try_claim(i, who)
        return None
    with os.fdopen(fd, "w") as fh:
        fh.write(f"{who}\n{os.getpid()}\n{time.strftime('%Y-%m-%dT%H:%M:%S')}\n")
    return p


def claim(who, max_wait=540):
    os.makedirs(SLOT_DIR, exist_ok=True)
    waited = 0
    while True:
        if _running() < N_SLOTS:
            for i in range(1, N_SLOTS + 1):
                p = _try_claim(i, who)
                if p:
                    print(p, flush=True)
                    return 0
        if waited >= max_wait:
            print(f"# {who}: no producer slot within {max_wait} s - call claim again", file=sys.stderr, flush=True)
            return 3
        time.sleep(POLL_S)
        waited += POLL_S


def release(p):
    if os.path.isfile(p):
        os.remove(p)
        print(f"released {p}")
        return 0
    print(f"no such lock {p}", file=sys.stderr)
    return 1


def status():
    os.makedirs(SLOT_DIR, exist_ok=True)
    for i in range(1, N_SLOTS + 1):
        p = _lock_path(i)
        if os.path.isfile(p):
            with open(p) as fh:
                who = fh.read().split()
            age = (time.time() - os.path.getmtime(p)) / 60.0
            print(f"slot{i}: HELD by {who[0] if who else '?'} for {age:.0f} min")
        else:
            print(f"slot{i}: free")
    print(f"gnss_processor*.exe running: {_running()}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    if cmd == "claim" and len(sys.argv) >= 3:
        mw = 540
        if "--max-wait" in sys.argv:
            mw = int(sys.argv[sys.argv.index("--max-wait") + 1])
        sys.exit(claim(sys.argv[2], mw))
    if cmd == "release" and len(sys.argv) == 3:
        sys.exit(release(sys.argv[2]))
    if cmd == "status":
        sys.exit(status())
    sys.exit(__doc__)
