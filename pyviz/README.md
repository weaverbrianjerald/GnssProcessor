# pyviz — Python port of the GnssProcessor visualization suite

A backend-agnostic Python re-implementation of the MATLAB `matlab/` trace
viewers. It reads the same `*_trace.csv` logs a processor run emits and produces
the same figures, with an offline **HTML summary report**, an **interactive
(plotly) report**, and a **real-time live dashboard**.

## Architecture (backend-agnostic)

```
  data layer        io · geodesy · colormaps · trace_paths · identifiers · palettes
  (pure pandas/numpy, no plotting)
        │  prepared DataFrames + derived quantities
        ▼
  viewers           viewers/*.py  →  build  figures.Figure  specs
  (GNSS semantics, no plotting library)
        │  declarative scene graph (pure data)
        ▼
  backends          backends/matplotlib_backend.py   (static PNG / report)
  (render Figure → output)        backends/plotly_backend.py   (interactive / real-time)
        │
        ▼
  report.py (offline HTML)   ·   realtime.py (live HTTP dashboard)
```

The load-bearing boundary is [figures.py](gnssviz/figures.py): a declarative
scene graph (`Figure → Axes → traces`). Trace kinds: `Line, Scatter, Band,
StackedArea, HLine, VLine, TextBox, Stem, Raster, RgbRaster, Markers,
PolarScatter, Mesh, Polygon, RgbImage` (a georeferenced truecolor image pinned to
a data-coordinate `extent` — e.g. the satellite basemap behind the exam map);
`Axes` supports `projection='polar'` and `'geo'`
(equirectangular world map with a vendored Natural-Earth coastline), dual y-axis,
log scale, per-axes colormaps, coloured `Scatter` (per-point `c` through the axes
cmap), frameless text. An [`Animation`](gnssviz/animation.py) is just a list of
`Figure` frames the backend renders to a looping GIF (the ionosphere movie).
Viewers never import a plotting library — they emit specs; a backend interprets
them. The plotly backend renders the same specs with **zero viewer changes** —
that is what the abstraction buys.

## Viewers (21 ported + 5 network)

| Group | Viewer (CSV) |
|-------|--------------|
| State / position | `state` (state_trace) · `wls` (wls_trace) · `baseline` (baseline_trace) |
| Measurement | `measurement_residuals`, `residual_strip`†, `plot_satellite_signals` (measurement_residuals) · `dops` (dop_trace) · `satellite_status`, `satellite_strip` (satellite_status_trace) |
| State (per-sat) | `ambiguities`, `ambiguity_locktime` (ambiguity_trace) · `ionosphere`, `iono_strip` (ionosphere_trace) · `sat_biases` (sat_bias_trace) |
| Cycle-slip | `mwwl_trace` (mwwl_trace) · `turbo_edit_trace` (turbo_edit_trace) · `slip_gate_strip`, `slip_conditions` (mwwl/turbo) · `graphic_trace`, `gf_trace`, `if_slip_trace` (staged detectors) |
| **Network** (PPP-RTK NETWORK producer) | `network_map` (network_stations.csv / config) · `network_health` (network_trace) · `network_products` (network_solution) · `network_iono_map` (network_ionosphere) · `network_closed_loop` (closed_loop_trace) |
| **Exam / simulator truth** (`--truth-dir`) | `exam_scorecard` (verify_report + scenario_summary + truth_scalars) · `exam_position_truth` (truth_position + state_trace) · `exam_truth_lines` (truth_scalars + state_trace) · `exam_track` (truth_position + state_trace) · `exam_map` (truth_position; **satellite-imagery** basemap, coastline fallback) · `exam_visibility` (truth_visibility) · `exam_iono_truth` (truth_atmo) · `exam_skyplot_slips` (truth_atmo + truth_slips) · `exam_slip_timeline` (truth_slips) |
| **Surveyed reference** (`--run-config` / `--ref-ecef`) | `rt_scorecard` (state_trace vs `receiver.reference_position_ecef`) |

Each viewer auto-skips when its trace CSV is missing/empty (header-only too), so
one invocation works unchanged across PPP / PPP-RTK / baseline / replay runs.

The **exam / simulator-truth** viewers compare the real solution against the
[GnssSimulator](../../GnssSimulator) answer key (`truth_*.csv` +
`scenario_summary.json`).  They light up only when `--truth-dir <dir>` is given
(a plain processor run auto-skips every one of them, so its report is unchanged),
and each guards its optional columns/files, so a legacy partial-truth dir renders
what it can and skips the rest.

### Network-level visualization

The network viewers visualize a **PPP-RTK NETWORK** run (the multi-station
satellite-product producer):

- **`network_map`** — every station on a world map: estimating peers, the clock
  datum (★), and the held-out closed-loop user (▲), with great-circle baselines
  from the datum. Positions come from the producer's `network_stations.csv`
  roster (preferred) or, with `--network-config <cfg.json>`, straight from the
  run config (so it renders **without a run**).
- **`network_iono_map`** — the network's *own* estimated ionosphere as
  **ionospheric pierce points** coloured by VTEC: a coverage map, a single-epoch
  snapshot over the CODE IONEX field (found under `--data-root`), and an animated
  **GIF** stepping through epochs.
- **`network_health`** — per-epoch estimability & convergence (stations/clocks/
  phase-biases present, clock-Δ RMS, gate-withheld epochs).
- **`network_products`** — the emitted satellite clock / phase-bias / code-bias
  time series, one line per satellite, with discontinuity rings.
- **`network_closed_loop`** — the held-out user's 3D/ENU error and integer-fix
  rate over time.

`network_iono_map` needs the producer to emit the per-station slant iono — enable
it in the network config: `"network": { "trace_ionosphere": true }` (the cheap
`network_stations.csv` roster is on by default via `"trace_stations": true`).

† `residual_strip` is **off by default** in the report (heavy per-signal rasters,
redundant with the `measurement_residuals` line plots); enable it with `--only
residual_strip` or `--all`. `measurement_residuals` likewise renders only the raw
Code/Phase residual series by default — its **standardized**-residual panels are
off (see the `partial(..., show_standardized=False)` in its viewer module).

**Deferred** (separate concerns, easy to add): `view_lcf` (NGS LASER constraint
files — its own input format/workstream) and the **batch lane**
(`runBatchPlots` / `plotBatchSummary` / `plotCellTimeseries` — a `summary.json`
batch-results contract, not the trace-log lane). The `gf`/`if_slip` staged
detectors are uncompiled in the C++ engine, so their CSVs are rarely produced.

## Install

Uses the project `venv` (Python 3.13).

```powershell
C:\GnssProcessor\venv\Scripts\python.exe -m pip install -r C:\GnssProcessor\pyviz\requirements.txt
```

Dependencies: numpy, pandas, matplotlib (static), plotly (interactive/real-time);
`kaleido` is optional (only for exporting plotly figures to PNG).

## Usage

### Offline report

```powershell
cd C:\GnssProcessor\pyviz
..\venv\Scripts\python.exe run_report.py --open                  # static PNG report
..\venv\Scripts\python.exe run_report.py --backend plotly -o r.html   # interactive report
..\venv\Scripts\python.exe run_report.py D:\run\logs --only state dops
..\venv\Scripts\python.exe run_report.py --all                   # incl. off-by-default viewers

# Network run: feed the config (station positions/roles) + data root (IONEX backdrop)
..\venv\Scripts\python.exe run_report.py <netlogs> --network-config config\ppp_rtk_network_internal.json --data-root .\data
```

### Exam dashboard (GnssSimulator answer key)

```powershell
# Truth-referenced dashboard: overlay the simulator's answer key on the solution.
..\venv\Scripts\python.exe run_report.py <run_logs> --truth-dir <truth_dir> `
    -o dashboard.html --backend plotly --plotly-js inline
```

| Flag | Meaning |
|------|---------|
| `--truth-dir <dir>` | Bind the GnssSimulator answer key (`truth_*.csv` + `scenario_summary.json`). Lights up the `exam_*` viewers, flips the report title to *GnssSimulator exam run report*, and turns the standardized-residual panels on. Without it every exam viewer auto-skips (a plain report is unchanged). |
| `--verify-report <json>` | `verify_report.json` for the coloured PASS/FAIL gate banner + scorecard table. Default: auto-detect `<run_logs>/verify_report.json` (written by `gnss_simulator verify`). |
| `--plotly-js {cdn,inline}` | Only for `--backend plotly`. `cdn` (default) loads plotly.js from the CDN (needs network on first open); `inline` embeds it (~4 MB, fully **offline** — the grader-archive mode). |
| `--std-residuals` / `--no-std-residuals` | Force the standardized-residual panels on/off (default: on whenever `--truth-dir` is given). |

The simulator's `all` one-shot and `harness/gnss_verify_loop.ps1` invoke this
automatically after `verify` (`--backend plotly --plotly-js inline`), writing
`<run_logs>/dashboard.html`.  The default **matplotlib** backend is offline by
construction (base64-PNG); use it for a static grader archive.

### Simulated-network dashboard (simnet truth grade)

The network analogue of the exam dashboard: the synthetic minimum-network
fixture (`docs/SPEC_synthetic_minimum_network.md`) injects truth, and
`scripts/simnet_grade.py` transforms it into the producer's realized S-basis.
One wrapper chains grade → report:

```powershell
cd C:\GnssProcessor
venv\Scripts\python.exe scripts\simnet_report.py runs\simnet60_quiet_wu93 --run runs\camp60wu_quiet
# -> <run>\simnet_dashboard.html  (offline, self-contained)
```

The wrapper writes `simnet_grade.{json,txt}` + `simnet_grade_series.csv` into
the run's `logs/`, and the `simnet_*` viewers key on them (auto-skip
everywhere else): `simnet_scorecard` (the grader's verdict + verbatim
readout), `simnet_theory` (uncombined model / rank defect, the run's realized
S-basis wire contracts, truth-projection grading — mathtext, offline), and
`simnet_truth_series` (per-epoch truth error vs published σ — the calibration
exhibit — plus per-station ZWD, rx DCB contrast/level, phase SD fractional).
The series is produced by the grader's own transform code path (`--series`),
so the panels cannot drift from the verdict; the settle boundary is drawn on
every axis and everything left of it is shown, never graded.

### Real-time / rooftop report (surveyed reference)

A field run has no simulator answer key, but a **static receiver on a surveyed
mark** does have a truth point: the config's `receiver.reference_position_ecef`.
Bind the run's own config and the `rt_scorecard` viewer grades the solution
against it — the field analogue of the exam dashboard.

```powershell
# Replay the archived SBF capture through the real-time engine, then report it.
cd C:\GnssProcessor\build\x64-Release\run_rt_workshop
..\gnss_processor.exe C:\GnssProcessor\config\sbf_replay_covl145.json

cd C:\GnssProcessor\pyviz
..\venv\Scripts\python.exe run_report.py <run>\logs `
    --run-config C:\GnssProcessor\config\sbf_replay_covl145.json `
    -o <run>\rt_report.html --backend plotly --plotly-js inline
```

| Flag | Meaning |
|------|---------|
| `--run-config <cfg.json>` | This run's processor config (`//` and `/* */` comments tolerated). Supplies `receiver.reference_position_ecef` to `rt_scorecard`, adds the summary banner + metric chips, and — for `app_mode: "real_time"` — retitles the report *GnssProcessor real-time run report*. |
| `--ref-ecef X Y Z` | Surveyed reference ECEF [m] directly; **wins** over the config block (same explicit-beats-auto rule as the C++ path fields). |

Without either flag `rt_scorecard` auto-skips and the banner is empty, so every
existing report is byte-unchanged. The viewer draws: truth-referenced E/N/U + 3D
error, a log-scale convergence panel against the `3·σ3D` envelope, the
post-convergence horizontal scatter (time-coloured, 50 %/95 % circles) and the
steady-state error CDFs.

**Convergence is suffix-stable**: converged-at-`thr` means the 3D error stays
`≤ thr` from that epoch *to the end of the run*, so a dip that later diverges
does not count. The ladder is 1.0 / 0.5 / 0.2 / 0.1 m and steady-state
statistics run from the coarsest achieved rung (the summary names which, and
says so explicitly when even 1.0 m was never held).

An RT report drops the per-(sat,signal) slip/residual internals by default
(`RT_DASHBOARD_OFF` in `run_report.py`) — at 1 Hz those traces are tens of MB
each and an interactive report embedding them exceeded 70 MB. `--all`, or
`--only <name>`, brings any of them back.

### Automatic report at end of a run

The C++ processor can generate the report itself when a post-processing run
finishes. Set in the config `processing` block:

```jsonc
"auto_report": true,              // generate <logs>/gnss_report.html at end of run
"report_backend": "matplotlib",   // "matplotlib" (static) | "plotly" (interactive)
"report_async": false,            // true = launch detached (run returns without waiting)
"report_viewers": [],             // curated subset for a fast report ([] = all). e.g.:
//   ["state","dops","satellite_status","measurement_residuals","ionosphere","ambiguities"]
// "report_python": "C:/path/to/python.exe"   // optional; default = <root>/venv
```

`Application::generateReport_()` flushes the traces and shells out to
`run_report.py` against the run's log folder (post-processing **and** baseline
modes). It's non-fatal: if pyviz/python isn't set up, a warning is logged and
the run still succeeds. (Real-time mode is unaffected — use `live_view.py`
there.)

### Real-time live dashboard

```powershell
..\venv\Scripts\python.exe live_view.py                  # http://127.0.0.1:8765 (plotly)
..\venv\Scripts\python.exe live_view.py D:\run\logs --port 9000 --interval 5
..\venv\Scripts\python.exe live_view.py --all            # every viewer (heavier)
```

Point it at the log folder of a *running* processor (post-processing writing
growing traces, or the real-time engine). The page polls a cheap `/status`
token (newest trace mtime); when it changes it fetches `/update` and patches
each figure **in place** (Plotly `react` / image swap) — no full-page reload,
so zoom/pan survive and there's no flicker. Two efficiency layers keep it cheap
even with large traces: **per-viewer change detection** (a viewer is only
re-rendered when one of *its* CSVs changes, so a 20 MB `mwwl_trace.csv` update
never re-renders the state/DOP panels) and token-gated polling (nothing renders
while idle). Default live set is a curated, fast subset (excludes the multi-MB
slip traces); `--only` / `--all` override.

### Library API

```python
from gnssviz import TraceContext, run_viewers
from gnssviz.backends import get_backend
from gnssviz.report import write_html_report

ctx = TraceContext(r"C:\GnssProcessor\logs")
out = run_viewers(ctx)                         # Figure specs, backend-independent
write_html_report("report.html",
                  [(v.title, r) for (v, r) in out.ran],
                  get_backend("plotly"),       # or "matplotlib"
                  log_dir=ctx.log_dir, skipped=out.skipped)
```

## Adding a viewer

1. `gnssviz/viewers/<name>.py` with `build_<name>(ctx) -> ViewerResult` that
   loads its CSV (`ctx.path(...)`), prepares data via the `io`/`geodesy`/
   `identifiers`/`palettes` helpers, and returns `figures.Figure` specs.
2. Expose `viewer = Viewer(name, title, required_csvs, build_<name>)`.
3. Register it in `gnssviz/viewers/__init__.py` (`REGISTRY`).
4. A new visual primitive goes in `figures.py` + both backends — the only place
   a plotting library is touched.

See [PORTING_PLAN.md](PORTING_PLAN.md) for the per-file MATLAB catalog.

## Vendored map assets (offline)

The geo viewers keep the reports **fully offline** by *vendoring* their
backdrops under `gnssviz/assets/` (no cartopy/contextily/tile-server dependency —
the numpy/pandas/matplotlib-only philosophy):

- **Coastline** — `coastline_110m.json` (Natural Earth 110 m, public domain),
  loaded by [`coastline.py`](gnssviz/coastline.py); the `geo` projection and the
  `exam_map` world panel draw it as an outline / filled land.
- **Satellite basemap** — `basemap_esri_fiji.jpg` + `.json` sidecar, loaded by
  [`basemap.py`](gnssviz/basemap.py) and drawn as an `RgbImage` behind the
  `exam_map` regional track (Esri "World Imagery", EPSG:4326, attribution
  *Esri, Maxar, Earthstar Geographics* rendered on the map). Regenerate with
  `python scripts/fetch_satellite.py` (fetches + stitches the two dateline
  halves once; the committed asset is what the report embeds). Both loaders
  degrade gracefully — a missing asset falls back (coastline → graticule;
  imagery → filled coastline basemap).

## Tests

```powershell
cd C:\GnssProcessor\pyviz
..\venv\Scripts\python.exe -m pytest -q
```

Covers the pure data layer (geodesy, GPS-time, colormaps, strip percentile /
PRN-sort / caxis, state stats), the real-time change-token + live HTML, and an
end-to-end smoke test that renders every applicable viewer against `logs/` and
asserts the report assembles.

## Roadmap

- Run scorecard + truth-referenced accuracy (config truth ECEF → true ENU error,
  convergence time, RMS, CDF) and a metrics panel at the top of the report.
- Run-comparison / overlay mode (load N runs, overlay key series + scorecard diff).
- Row-level incremental tailing of huge traces (the live path already avoids
  re-rendering unchanged viewers; this would also avoid re-reading them).
- Port `view_lcf` and the batch lane if needed.
