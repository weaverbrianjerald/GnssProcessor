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
PolarScatter`; `Axes` supports `projection='polar'`, dual y-axis, log scale,
per-axes colormaps, frameless text. Viewers never import a plotting library —
they emit specs; a backend interprets them. The plotly backend renders the same
specs with **zero viewer changes** — that is what the abstraction buys.

## Viewers (21 ported)

| Group | Viewer (CSV) |
|-------|--------------|
| State / position | `state` (state_trace) · `wls` (wls_trace) · `baseline` (baseline_trace) |
| Measurement | `measurement_residuals`, `residual_strip`, `plot_satellite_signals` (measurement_residuals) · `dops` (dop_trace) · `satellite_status`, `satellite_strip` (satellite_status_trace) |
| State (per-sat) | `ambiguities`, `ambiguity_locktime` (ambiguity_trace) · `ionosphere`, `iono_strip` (ionosphere_trace) · `sat_biases` (sat_bias_trace) |
| Cycle-slip | `mwwl_trace` (mwwl_trace) · `turbo_edit_trace` (turbo_edit_trace) · `slip_gate_strip`, `slip_conditions` (mwwl/turbo) · `graphic_trace`, `gf_trace`, `if_slip_trace` (staged detectors) |

Each viewer auto-skips when its trace CSV is missing/empty (header-only too), so
one invocation works unchanged across PPP / PPP-RTK / baseline / replay runs.

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
```

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
