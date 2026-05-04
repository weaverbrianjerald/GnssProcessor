#!/usr/bin/env python3
"""run_batch.py

Batch-run @c gnss_processor.exe across a grid of (station, date) cells.

Each cell gets:
  - A generated per-run JSON config (template + station/date overrides).
  - An isolated working directory so the exe's @c logs/ tree and CSV
    traces don't collide with other parallel runs.
  - A captured stdout/stderr log for post-mortem.

============================================================
Concurrency model
============================================================

Days are processed strictly in order.  Within each day:
  1. Station[0] runs SERIALLY.  This is the only path that may need
     to download products from CDDIS — letting it complete first
     guarantees the products are on disk before any siblings start.
  2. Stations[1:] run in PARALLEL via a process pool.  By this point
     the on-disk product cache is warm; every parallel worker just
     reads from there.

Without the serial first-of-day, two parallel processes can race on
the same partially-written .gz/.crx file in @c data_root/cache/, and
@c DataDownloader.download() will mistake a partial file for a
"cached" one (its existence check happens before any size or hash
verification).  The serial first-station avoids that without needing
file locks in the C++ side.

============================================================
Output layout
============================================================

::

    <output_root>/
      summary.json
      <YYYY-DOY>/
        <STATION>/
          run_config.json     # the generated config that ran
          stdout.log
          stderr.log
          logs/               # whatever the exe wrote (state-trace CSVs, spdlog files)
          .success            # zero-byte sentinel; presence = idempotent skip on re-run

============================================================
Usage
============================================================

::

    # 7-day window for 3 stations off the kinematic prod template
    python scripts/run_batch.py \\
        --stations GCGO,JPLM,AMC4 \\
        --year 2025 --start-doy 200 --end-doy 206 \\
        --template config/ppp_rtk_kinematic_multi_prod.json \\
        --output-root batch_results/week_2025_200

    # Resume a partially-completed batch (re-running is idempotent —
    # cells with .success markers are skipped).
    python scripts/run_batch.py ... --output-root batch_results/week_2025_200

    # Force re-run of every cell.
    python scripts/run_batch.py ... --force
"""

from __future__ import annotations

import argparse
import concurrent.futures
import copy
import json
import os
import re
import subprocess
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path

# ---------------------------------------------------------------------
# JSONC support
# ---------------------------------------------------------------------
# The C++ side parses configs with nlohmann::json's ignore_comments
# mode, so the in-tree configs use // and /* */ liberally.  Python's
# stdlib json doesn't grok comments, so we strip them before parsing.
# The regex alternates between matching string literals (which we
# preserve verbatim — comments don't apply inside strings) and matching
# comments (which we drop).  This is sufficient for the small number
# of configs we deal with; if we ever need full JSON5, swap to the
# json5 PyPI package.
_JSONC_RE = re.compile(
    r'(?P<string>"(?:[^"\\]|\\.)*")|(?P<linec>//[^\n]*)|(?P<blockc>/\*.*?\*/)',
    flags=re.DOTALL,
)

def _strip_jsonc(text: str) -> str:
    def _sub(m):
        return m.group("string") if m.group("string") else ""
    return _JSONC_RE.sub(_sub, text)

def load_jsonc(path: Path) -> dict:
    return json.loads(_strip_jsonc(path.read_text(encoding="utf-8-sig")))

# ---------------------------------------------------------------------
# Cell description
# ---------------------------------------------------------------------

@dataclass
class Cell:
    """One (station, year, doy) batch cell."""
    station: str
    country: str
    year: int
    doy: int
    run_dir: Path
    config_path: Path

    @property
    def label(self) -> str:
        return f"{self.year}-{self.doy:03d}/{self.station}"

@dataclass
class CellResult:
    station: str
    year: int
    doy: int
    success: bool
    elapsed_s: float
    return_code: int
    run_dir: str
    skipped: bool = False

# ---------------------------------------------------------------------
# Config templating
# ---------------------------------------------------------------------

def make_run_config(template: dict, station: str, country: str,
                    year: int, doy: int) -> dict:
    """Apply station/date overrides to a deep-copied template config.

    Forces production mode + obs download because that's the standard
    batch-processing flow; flips off any pinned @c observation_path
    so PathResolver auto-discovers the downloaded RINEX.
    """
    cfg = copy.deepcopy(template)

    cfg["data_source"] = "production"
    cfg.setdefault("data_download", {})
    dl = cfg["data_download"]
    dl["enabled"] = True
    dl["station"] = station
    dl["station_country"] = country
    dl["year"] = year
    dl["doy"] = doy
    dl.setdefault("download_rinex", True)
    dl.setdefault("download_products", True)

    # An explicit observation_path / navigation_path in the template
    # would override PathResolver's auto-discovery and pin EVERY cell
    # to the same file — almost certainly not what a batch wants.
    files = cfg.setdefault("files", {})
    files.pop("observation_path", None)
    files.pop("navigation_path", None)

    # Force the minimal logging set.  Per-epoch loggers (Kalman,
    # measurement model, slip detectors, ambiguity/iono/residual/WLS
    # traces) write a few hundred lines per epoch; on a 24-hour 30-s
    # daily file that's ~1M log lines per cell, dominating wall-clock
    # for batch runs even on Release builds.  GnssConfig defaults
    # missing logger keys to false, so this two-key block is enough
    # to guarantee the rest stay off — no need to enumerate every
    # toggle.
    cfg["logging"] = {
        "enable_main_log":   True,
        "enable_state_trace": True,
        "enable_residual_trace": True,
    }

    return cfg

# ---------------------------------------------------------------------
# Cell execution
# ---------------------------------------------------------------------

def write_run_inputs(cell: Cell, cfg: dict) -> None:
    cell.run_dir.mkdir(parents=True, exist_ok=True)
    with open(cell.config_path, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2)

def run_cell(exe: str, cell_dict: dict, timeout_s: int) -> dict:
    """Pool-friendly wrapper: takes plain dicts, returns a plain dict.

    @c ProcessPoolExecutor pickles arguments across processes; the
    @c Cell dataclass with a @c Path field round-trips fine but plain
    dicts are simpler and don't require the worker to import this
    module's types.
    """
    cell = Cell(
        station=cell_dict["station"],
        country=cell_dict["country"],
        year=cell_dict["year"],
        doy=cell_dict["doy"],
        run_dir=Path(cell_dict["run_dir"]),
        config_path=Path(cell_dict["config_path"]),
    )
    success_marker = cell.run_dir / ".success"

    t0 = time.time()
    try:
        proc = subprocess.run(
            [exe, str(cell.config_path)],
            cwd=str(cell.run_dir),
            capture_output=True,
            text=True,
            timeout=timeout_s,
        )
        elapsed = time.time() - t0
        (cell.run_dir / "stdout.log").write_text(proc.stdout, encoding="utf-8")
        (cell.run_dir / "stderr.log").write_text(proc.stderr, encoding="utf-8")
        success = proc.returncode == 0
        if success:
            success_marker.touch()
        return asdict(CellResult(
            station=cell.station, year=cell.year, doy=cell.doy,
            success=success, elapsed_s=elapsed,
            return_code=proc.returncode, run_dir=str(cell.run_dir),
        ))
    except subprocess.TimeoutExpired as e:
        elapsed = time.time() - t0
        (cell.run_dir / "stdout.log").write_text(e.stdout or "", encoding="utf-8")
        (cell.run_dir / "stderr.log").write_text(
            (e.stderr or "") + f"\n[run_batch] timed out after {timeout_s}s\n",
            encoding="utf-8")
        return asdict(CellResult(
            station=cell.station, year=cell.year, doy=cell.doy,
            success=False, elapsed_s=elapsed,
            return_code=-1, run_dir=str(cell.run_dir),
        ))

# ---------------------------------------------------------------------
# Orchestration
# ---------------------------------------------------------------------

def build_cell_grid(stations: list[str], country: str, year: int,
                    start_doy: int, end_doy: int,
                    output_root: Path) -> list[list[Cell]]:
    """Return a list-of-lists indexed by date, each inner list one
    cell per station.  Date order is preserved so the day-by-day
    serial-first pattern can dispatch each row in turn."""
    grid: list[list[Cell]] = []
    for doy in range(start_doy, end_doy + 1):
        date_label = f"{year:04d}-{doy:03d}"
        row: list[Cell] = []
        for station in stations:
            run_dir = output_root / date_label / station.upper()
            row.append(Cell(
                station=station,
                country=country,
                year=year,
                doy=doy,
                run_dir=run_dir,
                config_path=run_dir / "run_config.json",
            ))
        grid.append(row)
    return grid

def already_done(cell: Cell) -> bool:
    return (cell.run_dir / ".success").exists()

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n", 1)[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--stations", required=True,
                    help="Comma-separated 4- or 9-char station IDs (e.g. GCGO,JPLM,AMC4).")
    ap.add_argument("--country", default="USA",
                    help="3-char country code for IGS long-name expansion. Default: USA")
    ap.add_argument("--year", type=int, required=True)
    ap.add_argument("--start-doy", type=int, required=True)
    ap.add_argument("--end-doy", type=int, required=True,
                    help="Inclusive.  Single day: --start-doy 200 --end-doy 200.")
    ap.add_argument("--template", type=Path, required=True,
                    help="Template config; the script overrides station/date/data_source fields.")
    ap.add_argument("--output-root", type=Path, required=True)
    ap.add_argument("--exe", default=str(Path("build") / "x64-Release" / "gnss_processor.exe"),
                    help="Path to gnss_processor executable.  Default Release because "
                         "Debug is 5-10x slower on Eigen-heavy filter math — that compounds "
                         "fast across a many-cell batch.  Default: %(default)s")
    ap.add_argument("--max-workers", type=int, default=max(1, os.cpu_count() // 2),
                    help="Parallel station workers per day (after the serial first-station). "
                         "Default: %(default)s")
    ap.add_argument("--timeout", type=int, default=3600,
                    help="Per-cell wall-clock timeout in seconds. Default: %(default)s")
    ap.add_argument("--force", action="store_true",
                    help="Re-run cells even if their .success marker exists.")
    args = ap.parse_args(argv)

    stations = [s.strip() for s in args.stations.split(",") if s.strip()]
    if not stations:
        print("error: --stations resolved to an empty list", file=sys.stderr)
        return 2
    if args.end_doy < args.start_doy:
        print("error: --end-doy < --start-doy", file=sys.stderr)
        return 2
    if not args.template.is_file():
        print(f"error: template config not found: {args.template}", file=sys.stderr)
        return 2

    exe_path = Path(args.exe).resolve()
    if not exe_path.is_file():
        print(f"error: executable not found: {exe_path}", file=sys.stderr)
        return 2

    template = load_jsonc(args.template)
    output_root = args.output_root.resolve()
    output_root.mkdir(parents=True, exist_ok=True)

    grid = build_cell_grid(
        stations=stations, country=args.country, year=args.year,
        start_doy=args.start_doy, end_doy=args.end_doy,
        output_root=output_root,
    )

    n_cells = sum(len(row) for row in grid)
    print(f"Batch: {n_cells} cells = {len(grid)} dates × {len(stations)} stations")
    print(f"  template:    {args.template}")
    print(f"  output_root: {output_root}")
    print(f"  exe:         {exe_path}")
    print(f"  workers:     {args.max_workers} (per day, after serial first-station)")
    print()

    all_results: list[dict] = []
    t_batch = time.time()

    for row in grid:
        date_label = f"{row[0].year:04d}-{row[0].doy:03d}"
        # Stage configs for every cell in this row up front so each
        # worker just executes its own.
        for cell in row:
            cfg = make_run_config(template, cell.station, cell.country,
                                  cell.year, cell.doy)
            write_run_inputs(cell, cfg)

        # --- Serial first-of-day (ensures product cache is warm) ---
        first = row[0]
        if not args.force and already_done(first):
            print(f"[skip] {first.label}  (already-success)")
            all_results.append(asdict(CellResult(
                station=first.station, year=first.year, doy=first.doy,
                success=True, elapsed_s=0.0, return_code=0,
                run_dir=str(first.run_dir), skipped=True,
            )))
        else:
            print(f"[run ] {first.label}  (serial; warms product cache for day)")
            cell_dict = {
                "station": first.station, "country": first.country,
                "year": first.year, "doy": first.doy,
                "run_dir": str(first.run_dir),
                "config_path": str(first.config_path),
            }
            r = run_cell(str(exe_path), cell_dict, args.timeout)
            all_results.append(r)
            status = "ok " if r["success"] else "FAIL"
            print(f"  -> {status}  {r['elapsed_s']:6.1f}s")

        # --- Parallel rest-of-day -----------------------------------
        rest = row[1:]
        pending = []
        for cell in rest:
            if not args.force and already_done(cell):
                print(f"[skip] {cell.label}  (already-success)")
                all_results.append(asdict(CellResult(
                    station=cell.station, year=cell.year, doy=cell.doy,
                    success=True, elapsed_s=0.0, return_code=0,
                    run_dir=str(cell.run_dir), skipped=True,
                )))
                continue
            pending.append({
                "station": cell.station, "country": cell.country,
                "year": cell.year, "doy": cell.doy,
                "run_dir": str(cell.run_dir),
                "config_path": str(cell.config_path),
            })
        if pending:
            print(f"[pool] {date_label}: dispatching {len(pending)} cells "
                  f"to {args.max_workers} workers")
            with concurrent.futures.ProcessPoolExecutor(
                    max_workers=args.max_workers) as ex:
                futures = {
                    ex.submit(run_cell, str(exe_path), p, args.timeout): p
                    for p in pending
                }
                for fut in concurrent.futures.as_completed(futures):
                    r = fut.result()
                    all_results.append(r)
                    status = "ok " if r["success"] else "FAIL"
                    print(f"  -> {status}  {r['year']}-{r['doy']:03d}/{r['station']:<10} "
                          f"{r['elapsed_s']:6.1f}s")

    # ---- Summary ---------------------------------------------------
    wall = time.time() - t_batch
    n_pass = sum(1 for r in all_results if r["success"] and not r.get("skipped"))
    n_skip = sum(1 for r in all_results if r.get("skipped"))
    n_fail = sum(1 for r in all_results if not r["success"])

    summary = {
        "batch_args": vars(args) | {
            "exe": str(exe_path),
            "output_root": str(output_root),
            "template": str(args.template),
        },
        "wall_seconds": round(wall, 2),
        "counts": { "pass": n_pass, "fail": n_fail, "skipped": n_skip,
                    "total": len(all_results) },
        # Sort for human readability AND stable diffs across re-runs.
        "results": sorted(all_results,
                          key=lambda r: (r["year"], r["doy"], r["station"])),
    }
    summary_path = output_root / "summary.json"
    with open(summary_path, "w", encoding="utf-8") as f:
        # default=str so any stray Path slipped past asdict() serializes.
        json.dump(summary, f, indent=2, default=str)

    print()
    print(f"Done in {wall:.1f}s.  pass={n_pass}  fail={n_fail}  skipped={n_skip}")
    print(f"Summary: {summary_path}")
    return 0 if n_fail == 0 else 1

if __name__ == "__main__":
    sys.exit(main())
