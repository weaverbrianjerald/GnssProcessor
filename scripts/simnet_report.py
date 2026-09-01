#!/usr/bin/env python3
"""One-command HTML dashboard for a SIMULATED NETWORK run.

Chains the two halves that already exist, in the order the artifacts flow:

  1. ``scripts/simnet_grade.py``  — the truth grader (SPEC_synthetic_minimum_network
     §4), writing its verdict INTO the run's log dir so the report can bind it:
         logs/simnet_grade.json         machine-readable channel grades
         logs/simnet_grade.txt          the full console readout (verbatim caveats)
         logs/simnet_grade_series.csv   per-epoch truth-error series (--series)
  2. ``pyviz/run_report.py``      — the standard offline HTML report; the simnet
     viewers (scorecard / theory / truth series) light up on the artifacts above,
     and the ordinary network viewers cover the run's own traces.

Usage:
    python scripts/simnet_report.py <fixture_dir> --run <run_dir> [-o out.html]
    python scripts/simnet_report.py runs/simnet60_campaign_wu93 --run runs/camp60wu_f5c

The output defaults to ``<run_dir>/simnet_dashboard.html`` — one self-contained
offline file (matplotlib figures base64-embedded), the same artifact shape as the
GnssSimulator exam dashboard.

``--skip-grade`` reuses grade artifacts already in the log dir (e.g. to iterate
on report composition without re-grading a long run).
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The grader's readout carries σ/⚠/− etc.; a cp1252 Windows console must not be
# able to kill the pipeline over a glyph.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, OSError):
        pass


def _python():
    """The interpreter pyviz needs (pandas/matplotlib): venv first, then us."""
    venv = os.path.join(ROOT, "venv", "Scripts", "python.exe")
    return venv if os.path.exists(venv) else sys.executable


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("fixture_dir", help="simnet fixture dir (network_fixture.json + truth)")
    ap.add_argument("--run", default=None,
                    help="producer run dir holding logs/ (default <fixture>/run)")
    ap.add_argument("--products", default=None,
                    help="products.txt (default <run>/products.txt, else <fixture>/products.txt)")
    ap.add_argument("-o", "--output", default=None,
                    help="output HTML (default <run>/simnet_dashboard.html)")
    ap.add_argument("--settle-frac", type=float, default=None,
                    help="passed through to the grader (its default: 0.5)")
    ap.add_argument("--gf-datum", action="store_true",
                    help="passed through to the grader (slant posture; usually auto-detected)")
    ap.add_argument("--backend", default="matplotlib",
                    help="report backend (matplotlib = offline PNGs, plotly = interactive)")
    ap.add_argument("--title", default=None,
                    help="report title (default derived from the run dir name)")
    ap.add_argument("--skip-grade", action="store_true",
                    help="reuse existing simnet_grade.* artifacts in the log dir")
    ap.add_argument("--all", action="store_true",
                    help="also run default-off viewers (passed to run_report)")
    ap.add_argument("--open", action="store_true",
                    help="open the finished report in the default browser")
    args = ap.parse_args()

    fx = os.path.abspath(args.fixture_dir)
    run_dir = os.path.abspath(args.run or os.path.join(fx, "run"))
    logs = os.path.join(run_dir, "logs")
    if not os.path.isdir(logs):
        print(f"FAIL: no logs/ under {run_dir}")
        return 2
    products = args.products or os.path.join(run_dir, "products.txt")
    if not os.path.exists(products):
        products = os.path.join(fx, "products.txt")
    py = _python()

    # ---- 1. grade (verdict + series into the log dir, console tee'd to .txt) ----
    if not args.skip_grade:
        cmd = [py, os.path.join(ROOT, "scripts", "simnet_grade.py"), fx,
               "--run", run_dir, "--products", products,
               "--json", os.path.join(logs, "simnet_grade.json"),
               "--series", os.path.join(logs, "simnet_grade_series.csv")]
        if args.settle_frac is not None:
            cmd += ["--settle-frac", str(args.settle_frac)]
        if args.gf_datum:
            cmd += ["--gf-datum"]
        print("== simnet_report: grading ==", flush=True)
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              encoding="utf-8", errors="replace")
        sys.stdout.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        with open(os.path.join(logs, "simnet_grade.txt"), "w", encoding="utf-8") as fh:
            fh.write(proc.stdout)
        if proc.returncode not in (0, None):
            print(f"FAIL: grader exited {proc.returncode} — not building a report "
                  "on an ungraded run (pass --skip-grade to override with stale artifacts)")
            return proc.returncode
    else:
        need = os.path.join(logs, "simnet_grade.json")
        if not os.path.exists(need):
            print(f"FAIL: --skip-grade but no {need}")
            return 2

    # ---- 2. report (simnet viewers light up on the artifacts just written) ----
    out = os.path.abspath(args.output or os.path.join(run_dir, "simnet_dashboard.html"))
    title = args.title or f"Simulated network run report — {os.path.basename(run_dir)}"
    cmd = [py, os.path.join(ROOT, "pyviz", "run_report.py"), logs,
           "-o", out, "--backend", args.backend, "--title", title]
    if args.all:
        cmd += ["--all"]
    if args.open:
        cmd += ["--open"]
    print("\n== simnet_report: rendering ==", flush=True)
    rc = subprocess.run(cmd).returncode
    if rc == 0:
        print(f"\nsimnet dashboard -> {out}")
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
