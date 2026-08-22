#!/usr/bin/env python3
"""windup_ab_slice.py -- the SPEC_phase_windup_sign.md §8 item 1 decisive arm.

Real-data A/B the sim structurally cannot provide (the sim injects wind-up via
the same ``computePhaseWindUp`` it models with, so injected and modelled errors
cancel identically).  Four arms over bundled 2024-129 stations, graded against
independent SINEX/SIF truth:

    shipped_on   pre-fix exe,  enable_phase_windup: true   (the defect, live)
    shipped_off  pre-fix exe,  enable_phase_windup: false  (control)
    fixed_on     wu93 exe,     enable_phase_windup: true   (the fix, live)
    fixed_off    wu93 exe,     enable_phase_windup: false  (control twin)

Pre-registered expectations if the sign was inverted (SPEC §8):
  * shipped_on must be WORSE than shipped_off (modelling -phi is worse than
    not modelling at all);
  * fixed_on must be BETTER than fixed_off (modelling +phi finally helps);
  * shipped_off ~= fixed_off (the two binaries differ only in the wind-up
    construction + product-frame plumbing, none of which is live with the
    correction disabled on the user PPP path) -- this pair is the same-build
    sanity gate that isolates the sign as the only mover.

Usage::

    venv\\Scripts\\python.exe scripts\\windup_ab_slice.py \
        --shipped-exe build/x64-Release/gnss_processor_shipped.exe \
        --fixed-exe   build/x64-Release/gnss_processor_wu93.exe
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
sys.path.insert(0, str(REPO / "pyviz"))

from gnssviz.eval import (  # noqa: E402
    doy_to_decimal_year,
    evaluate_state_trace,
    make_default_truth,
)

YEAR, DOY = 2024, 129
DATE_DIR = f"{YEAR}-{DOY:03d}"

STATIONS = [
    {"name": "JPLM", "truth_key": "JPLM00USA", "dir": "jplm00usa"},
    {"name": "ALGO", "truth_key": "ALGO00CAN", "dir": "algo00can"},
    {"name": "ALIC", "truth_key": "ALIC00AUS", "dir": "alic00aus"},
    {"name": "ABPO", "truth_key": "ABPO00MDG", "dir": "abpo00mdg"},
    {"name": "ZIMM", "truth_key": "ZIMM00CHE", "dir": "zimm00che"},
]

SINEX_DIR = REPO / "data" / "static" / "sinex"
SIF_PATHS = [
    REPO / "data" / "static" / "sif" / "igs20_SIF.json",
    REPO / "data" / "static" / "sif" / "ncn20_SIF.json",
]
BASE_CONFIG = REPO / "config" / "test_suite" / "ppp_static.jsonc"

_JSONC_RE = re.compile(
    r'(?P<string>"(?:[^"\\]|\\.)*")|(?P<linec>//[^\n]*)|(?P<blockc>/\*.*?\*/)',
    flags=re.DOTALL,
)


def load_jsonc(path: Path) -> dict:
    def _sub(m):
        return m.group("string") if m.group("string") else ""
    return json.loads(_JSONC_RE.sub(_sub, path.read_text(encoding="utf-8-sig")))


def build_cfg(base: dict, station: dict, windup_on: bool) -> dict:
    cfg = copy.deepcopy(base)
    cfg["data_source"] = "internal"
    files = cfg.setdefault("files", {})
    obs = (REPO / "data" / DATE_DIR / "obs" / station["dir"]
           / f"{station['truth_key']}_R_{YEAR}{DOY:03d}0000_01D_30S_MO.rnx")
    files["observation_path"] = str(obs)
    files["internal_data_root"] = str(REPO / "data")
    cfg.setdefault("data_download", {}).update(
        {"enabled": False, "station": station["name"], "year": YEAR, "doy": DOY})
    cfg.setdefault("model_corrections", {})["enable_phase_windup"] = windup_on
    return cfg


def run_one(exe: Path, cfg: dict, run_dir: Path, timeout_s: int, force: bool) -> dict:
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


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--shipped-exe", type=Path, required=True)
    ap.add_argument("--fixed-exe", type=Path, required=True)
    ap.add_argument("--output-root", type=Path,
                    default=REPO / "test_suite_results" / "windup_ab")
    ap.add_argument("--warmup-s", type=float, default=1800.0)
    ap.add_argument("--threshold-m", type=float, default=0.10)
    ap.add_argument("--timeout", type=int, default=2400)
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args(argv)

    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

    for exe in (args.shipped_exe, args.fixed_exe):
        if not exe.is_file():
            print(f"error: exe not found: {exe}", file=sys.stderr)
            return 2

    cells = [
        ("shipped_on", args.shipped_exe, True),
        ("shipped_off", args.shipped_exe, False),
        ("fixed_on", args.fixed_exe, True),
        ("fixed_off", args.fixed_exe, False),
    ]

    base = load_jsonc(BASE_CONFIG)
    epoch = doy_to_decimal_year(YEAR, DOY)
    truth = make_default_truth(
        sinex_paths=sorted(SINEX_DIR.glob("*.snx")) + sorted(SINEX_DIR.glob("*.SNX"))
        + sorted(SINEX_DIR.glob("*.SSC")),
        sif_paths=SIF_PATHS,
    )

    print(f"== Wind-up sign A/B ==  {DATE_DIR}  epoch={epoch:.4f}")
    print(f"   shipped: {args.shipped_exe}")
    print(f"   fixed:   {args.fixed_exe}")
    print()

    rows = []
    for station in STATIONS:
        tc = truth.resolve(station["truth_key"], epoch)
        if tc is None:
            print(f"[truth] {station['name']}: NO TRUTH -- skipping")
            continue
        print(f"[truth] {station['name']} <- {tc.source}  frame={tc.frame or '?'}")
        for label, exe, windup_on in cells:
            run_dir = args.output_root / station["name"] / label
            cfg = build_cfg(base, station, windup_on)
            info = run_one(exe, cfg, run_dir, args.timeout, args.force)
            tag = "skip" if info["skipped"] else ("ok" if info["returncode"] == 0 else "FAIL")
            print(f"[run  ] {station['name']}/{label:<12} {tag:>4}  {info['elapsed_s']:6.1f}s")
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

    print("\n" + "=" * 100)
    print("SCORECARD  (ENU vs independent truth)")
    print("=" * 100)
    metrics_out = []
    for sta, label, m, info in rows:
        if m is None:
            print(f"  {sta}/{label:<12}  -- no metrics --")
            continue
        print(f"  {sta}/{label:<12} {m.one_line()}")
        metrics_out.append({"station": sta, "cell": label,
                            "elapsed_s": round(info["elapsed_s"], 1), **m.to_dict()})

    print("\n" + "-" * 100)
    print("PER-STATION 3D-RMS (cm) BY ARM   [pre-registered: shipped_on > shipped_off ~= fixed_off > fixed_on]")
    print("-" * 100)
    by_sta = {}
    for sta, label, m, _ in rows:
        if m is not None:
            by_sta.setdefault(sta, {})[label] = m
    hdr = f"  {'station':<8}" + "".join(f"{c[0]:>13}" for c in cells)
    print(hdr)
    for sta, cm in by_sta.items():
        line = f"  {sta:<8}"
        for label, _, _ in cells:
            m = cm.get(label)
            line += f"{m.rms_3d * 100:13.2f}" if m else f"{'--':>13}"
        print(line)

    out_path = args.output_root / "windup_ab_metrics.json"
    args.output_root.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps({
        "date": DATE_DIR, "epoch_decimal_year": epoch,
        "warmup_s": args.warmup_s,
        "shipped_exe": str(args.shipped_exe), "fixed_exe": str(args.fixed_exe),
        "metrics": metrics_out,
    }, indent=2), encoding="utf-8")
    print(f"\nWrote {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
