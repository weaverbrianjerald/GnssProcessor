#!/usr/bin/env python3
"""run_test_slice.py: thin slice of the "standard test run" harness.

Runs ``gnss_processor.exe`` over a small matrix of (station x config-cell) on the
bundled internal data, then grades each run's ``state_trace.csv`` position
against an independent truth coordinate (IGS SINEX/SSC primary, NGS SIF
fallback; see :mod:`gnssviz.eval`). Prints an ENU bias / RMS / convergence
scorecard and the A-vs-B deltas, and writes ``slice_metrics.json``.

Design notes
------------
* Truth comes from :func:`gnssviz.eval.make_default_truth`, not from any field
  in the run config. The engine seeds its a-priori from the RINEX marker /
  ``receiver.reference_position_ecef``; reusing that as truth would be circular
  (a static run with zero position process noise barely moves off its seed), so
  the grader is deliberately decoupled.
* Each cell runs in its own dir so the exe's ``logs/`` trees don't collide.
* This is the phase-1 slice: one station (JPLM), one A/B toggle. The STATIONS /
  CELLS tables below are the seam the phase-3 full matrix grows from.

Usage
-----
::

    C:\\GnssProcessor\\venv\\Scripts\\python.exe scripts\\run_test_slice.py
    ...\\python.exe scripts\\run_test_slice.py --exe build/x64-Debug/gnss_processor.exe
    ...\\python.exe scripts\\run_test_slice.py --force        # ignore .success markers
"""

from __future__ import annotations

import argparse
import copy
import json
import re
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "pyviz"))   # make gnssviz importable

from gnssviz.eval import (                 # noqa: E402
    doy_to_decimal_year,
    evaluate_state_trace,
    make_default_truth,
)

# ---------------------------------------------------------------------------
# Slice definition  (the phase-3 full matrix grows from these two tables)
# ---------------------------------------------------------------------------

YEAR, DOY = 2024, 129
DATE_DIR = f"{YEAR}-{DOY:03d}"

# station id (for the truth lookup) -> obs file + constellations to enable.
STATIONS = [
    {
        "name": "JPLM",
        "truth_key": "JPLM00USA",
        "obs": REPO / "data" / DATE_DIR / "obs" / "jplm00usa"
               / "JPLM00USA_R_20241290000_01D_30S_MO.rnx",
        "active_systems": ["GPS", "Galileo"],
    },
    # CIT1 is RINEX-2 (GPS-centric) and bundled too; kept for the wider matrix.
    # Note: its BaselineFixtures.hpp reference ECEF is wrong (~1.7 km / h=-693 m);
    # the truth here comes from SIF, not that fixture, so this is unaffected.
    # {
    #     "name": "CIT1", "truth_key": "CIT100USA",
    #     "obs": REPO / "data" / DATE_DIR / "obs" / "cit1" / "cit11290.24o",
    #     "active_systems": ["GPS"],
    # },
]

# A/B config cells: (label, {dot.path: value, ...}) override patches on the base.
CELLS = [
    ("baseline", {}),
    # Regression injection: disabling solid-earth tide should move the position
    # (especially vertical) vs truth, and the suite must resolve that as a position diff.
    ("no_solid_tide", {"model_corrections.enable_solid_earth_tide": False}),
]

# Truth sources: SINEX/SSC primary (any *.snx dropped into data/static/sinex/),
# SIF fallback (the bundled NGS files).  Composite degrades to SIF until an SSC
# is present.
SINEX_DIR = REPO / "data" / "static" / "sinex"
SIF_PATHS = [
    REPO / "data" / "static" / "sif" / "igs20_SIF.json",
    REPO / "data" / "static" / "sif" / "ncn20_SIF.json",
]

BASE_CONFIG = REPO / "config" / "test_suite" / "ppp_static.jsonc"


# ---------------------------------------------------------------------------
# JSONC + dot-path override helpers
# ---------------------------------------------------------------------------

_JSONC_RE = re.compile(
    r'(?P<string>"(?:[^"\\]|\\.)*")|(?P<linec>//[^\n]*)|(?P<blockc>/\*.*?\*/)',
    flags=re.DOTALL,
)


def load_jsonc(path: Path) -> dict:
    def _sub(m):
        return m.group("string") if m.group("string") else ""
    return json.loads(_JSONC_RE.sub(_sub, path.read_text(encoding="utf-8-sig")))


def set_dotpath(cfg: dict, dotted: str, value) -> None:
    keys = dotted.split(".")
    node = cfg
    for k in keys[:-1]:
        node = node.setdefault(k, {})
    node[keys[-1]] = value


# ---------------------------------------------------------------------------
# Run one cell
# ---------------------------------------------------------------------------

def build_cell_config(base: dict, station: dict, overrides: dict) -> dict:
    cfg = copy.deepcopy(base)
    cfg["data_source"] = "internal"
    files = cfg.setdefault("files", {})
    files["observation_path"] = str(station["obs"])
    files["internal_data_root"] = str(REPO / "data")
    cfg.setdefault("processing", {})["active_systems"] = list(station["active_systems"])
    dl = cfg.setdefault("data_download", {})
    dl.update({"enabled": False, "station": station["name"],
               "year": YEAR, "doy": DOY})
    for dotted, value in overrides.items():
        set_dotpath(cfg, dotted, value)
    return cfg


def run_one(exe: Path, cfg: dict, run_dir: Path, timeout_s: int,
            force: bool) -> dict:
    run_dir.mkdir(parents=True, exist_ok=True)
    marker = run_dir / ".success"
    cfg_path = run_dir / "run_config.json"
    cfg_path.write_text(json.dumps(cfg, indent=2), encoding="utf-8")

    if marker.exists() and not force:
        return {"skipped": True, "returncode": 0, "elapsed_s": 0.0}

    t0 = time.time()
    proc = subprocess.run([str(exe), str(cfg_path)], cwd=str(run_dir),
                          capture_output=True, text=True, timeout=timeout_s)
    elapsed = time.time() - t0
    (run_dir / "stdout.log").write_text(proc.stdout or "", encoding="utf-8")
    (run_dir / "stderr.log").write_text(proc.stderr or "", encoding="utf-8")
    if proc.returncode == 0:
        marker.touch()
    return {"skipped": False, "returncode": proc.returncode, "elapsed_s": elapsed}


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--exe", type=Path,
                    default=REPO / "build" / "x64-Release" / "gnss_processor.exe")
    ap.add_argument("--output-root", type=Path,
                    default=REPO / "test_suite_results" / "slice")
    ap.add_argument("--warmup-s", type=float, default=1800.0,
                    help="Exclude this many seconds of convergence transient. "
                         "Default: %(default)s")
    ap.add_argument("--threshold-m", type=float, default=0.10)
    ap.add_argument("--timeout", type=int, default=1800)
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args(argv)

    # Windows consoles / piped stdout default to cp1252; force UTF-8 so the
    # scorecard never dies on a stray non-ASCII glyph.
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

    if not args.exe.is_file():
        print(f"error: exe not found: {args.exe}", file=sys.stderr)
        return 2
    if not BASE_CONFIG.is_file():
        print(f"error: base config not found: {BASE_CONFIG}", file=sys.stderr)
        return 2

    base = load_jsonc(BASE_CONFIG)
    epoch = doy_to_decimal_year(YEAR, DOY)
    truth = make_default_truth(
        sinex_paths=sorted(SINEX_DIR.glob("*.snx")) + sorted(SINEX_DIR.glob("*.SNX")),
        sif_paths=SIF_PATHS,
    )

    print(f"== Test-suite slice ==  {DATE_DIR}  epoch={epoch:.4f}")
    print(f"   exe:     {args.exe}")
    print(f"   truth:   SINEX({len(list(SINEX_DIR.glob('*.snx')) + list(SINEX_DIR.glob('*.SNX')))} files) "
          f"-> SIF fallback")
    print(f"   results: {args.output_root}")
    print()

    rows = []          # (station, cell, PositionMetrics | None, run_info)
    for station in STATIONS:
        tc = truth.resolve(station["truth_key"], epoch)
        if tc is None:
            print(f"[truth] {station['name']}: NO TRUTH for {station['truth_key']} "
                  f"-- skipping station")
            continue
        if not station["obs"].is_file():
            print(f"[data ] {station['name']}: obs file missing: {station['obs']} "
                  f"-- skipping station")
            continue
        print(f"[truth] {station['name']} <- {tc.source}  frame={tc.frame or '?'}  "
              f"ecef=({tc.ecef[0]:.3f},{tc.ecef[1]:.3f},{tc.ecef[2]:.3f})")

        for label, overrides in CELLS:
            run_dir = args.output_root / station["name"] / label
            cfg = build_cell_config(base, station, overrides)
            info = run_one(args.exe, cfg, run_dir, args.timeout, args.force)
            tag = "skip" if info["skipped"] else ("ok" if info["returncode"] == 0 else "FAIL")
            print(f"[run  ] {station['name']}/{label:<14} {tag:>4}  "
                  f"{info['elapsed_s']:6.1f}s")
            if info["returncode"] != 0:
                rows.append((station["name"], label, None, info))
                continue
            trace = run_dir / "logs" / "state_trace.csv"
            if not trace.is_file():
                print(f"         (no state_trace.csv at {trace})")
                rows.append((station["name"], label, None, info))
                continue
            m = evaluate_state_trace(
                str(trace), tc.ecef, station=station["name"],
                source=tc.source, frame=tc.frame,
                warmup_s=args.warmup_s, threshold_m=args.threshold_m)
            rows.append((station["name"], label, m, info))

    # ---- Scorecard ----
    print("\n" + "=" * 96)
    print("SCORECARD  (ENU error vs independent truth; bias in cm, RMS in cm)")
    print("=" * 96)
    metrics_out = []
    for sta, label, m, info in rows:
        if m is None:
            print(f"  {sta}/{label:<14}  -- no metrics (run failed / no trace) --")
            continue
        print(f"  {label:<14} {m.one_line()}")
        metrics_out.append({"station": sta, "cell": label,
                            "returncode": info["returncode"],
                            "elapsed_s": round(info["elapsed_s"], 1),
                            **m.to_dict()})

    # ---- A/B deltas (per station, vs the 'baseline' cell) ----
    print("\n" + "-" * 96)
    print("A/B DELTAS vs 'baseline' cell  (delta in cm; +U = cell sits higher than baseline)")
    print("-" * 96)
    by_station = {}
    for sta, label, m, _ in rows:
        if m is not None:
            by_station.setdefault(sta, {})[label] = m
    for sta, cells in by_station.items():
        base_m = cells.get("baseline")
        if base_m is None:
            continue
        for label, m in cells.items():
            if label == "baseline":
                continue
            dE = (m.bias_enu[0] - base_m.bias_enu[0]) * 100
            dN = (m.bias_enu[1] - base_m.bias_enu[1]) * 100
            dU = (m.bias_enu[2] - base_m.bias_enu[2]) * 100
            d3 = (m.rms_3d - base_m.rms_3d) * 100
            print(f"  {sta}  {label:<14} dBiasE{dE:+7.2f} dBiasN{dN:+7.2f} "
                  f"dBiasU{dU:+7.2f}   d3D-RMS{d3:+7.2f}")

    out_path = args.output_root / "slice_metrics.json"
    args.output_root.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps({
        "date": DATE_DIR, "epoch_decimal_year": epoch,
        "warmup_s": args.warmup_s, "threshold_m": args.threshold_m,
        "metrics": metrics_out,
    }, indent=2), encoding="utf-8")
    print(f"\nWrote {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
