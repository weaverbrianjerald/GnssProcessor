#!/usr/bin/env python3
"""Run one decomposition golden-matrix arm into a fresh run directory.

Companion to compare_network_run.py (see docs/network_decomposition_plan.md §9).
Creates <run_dir>, executes the processor with CWD=<run_dir> so the logs/ tree and any
relative-path artifacts (e.g. the ARM-8 state snapshot) land inside it, tees stdout+stderr to
run.log, and writes wall.txt as "exit=N" — the two file names the harness maskspec expects.

Usage:
    python scripts/run_decomp_arm.py <exe> <config.json> <run_dir> [--force]

Exit code mirrors the processor's exit code (so a failed run fails the capture step loudly).
"""

import argparse
import pathlib
import shutil
import subprocess
import sys


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("exe", help="gnss_processor executable (use the renamed copy, Release only)")
    p.add_argument("config", help="arm config JSON (absolute path recommended)")
    p.add_argument("run_dir", help="fresh run directory (created; must not exist unless --force)")
    p.add_argument("--force", action="store_true",
                   help="delete and recreate run_dir if it already exists")
    p.add_argument("--seed", nargs=2, action="append", metavar=("SRC", "DEST_REL"),
                   default=[],
                   help="copy SRC into the fresh run_dir at relative path DEST_REL before "
                        "launching (repeatable) — e.g. the ARM-8 resume leg seeds the golden "
                        "leg-A state snapshot at logs/network_state_snapshot.json")
    args = p.parse_args()

    exe = pathlib.Path(args.exe).resolve()
    cfg = pathlib.Path(args.config).resolve()
    run_dir = pathlib.Path(args.run_dir).resolve()
    if not exe.is_file():
        print(f"ERROR: exe not found: {exe}", file=sys.stderr)
        return 2
    if not cfg.is_file():
        print(f"ERROR: config not found: {cfg}", file=sys.stderr)
        return 2
    if run_dir.exists():
        if not args.force:
            print(f"ERROR: run_dir exists (use --force to replace): {run_dir}", file=sys.stderr)
            return 2
        shutil.rmtree(run_dir)
    run_dir.mkdir(parents=True)
    for src, dest_rel in args.seed:
        src_p = pathlib.Path(src).resolve()
        if not src_p.is_file():
            print(f"ERROR: seed file not found: {src_p}", file=sys.stderr)
            return 2
        dest_p = run_dir / dest_rel
        dest_p.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src_p, dest_p)

    log_path = run_dir / "run.log"
    with open(log_path, "wb") as log:
        proc = subprocess.run([str(exe), str(cfg)], cwd=str(run_dir),
                              stdout=log, stderr=subprocess.STDOUT)
    (run_dir / "wall.txt").write_text(f"exit={proc.returncode}\n", encoding="ascii")
    print(f"{cfg.name} -> {run_dir}  exit={proc.returncode}")
    return proc.returncode


if __name__ == "__main__":
    sys.exit(main())
