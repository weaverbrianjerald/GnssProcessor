# Standard test run — real-data regression / A-B suite

A frozen, real-data harness that runs `gnss_processor` across a matrix of
configs (PPP / PPP-RTK, GPS+Galileo, static / kinematic, feature toggles on/off)
on a handful of bundled IGS stations, and grades each run's position against an
**independent truth coordinate**. Two jobs:

1. **Absolute accuracy** — how close is the solution to the published ITRF
   coordinate? (validates the whole physics chain).
2. **A/B config diff** — turn feature X on vs off and see the position move, in
   cm, vs truth. This is the "confirm new and future configs quickly" lever.

There is no numerical unit-test suite for the estimator, so this is the primary
regression guard for numerical changes.

## Truth reference: IGS SINEX/SSC primary, NGS SIF fallback

**Decision: grade against an IGS/ITRF SINEX (SSC) as the primary truth, with the
NGS SIF as a fallback + cross-check, both behind one `TruthSource` interface.**

Why SINEX over SIF, and why truth must live *outside* the run config:

- **Independence.** `receiver.reference_position_ecef` is consumed by the engine
  to seed the state position (`PppModel.cpp` — "intended for fixed-station
  residual analysis with zero position process noise"). A static run with zero
  position process noise barely moves off its seed, so grading against that seed
  is circular — it would report ≈0 error regardless of whether the physics is
  right. The SIF is also consumed by the engine (a-priori / OTL / QC), so it is
  partially circular too. An IGS SSC is consumed **nowhere** in the engine — a
  frame or site-displacement bug cannot hide inside the grader.
- **Coverage from one file.** A single ITRF2020/IGS20 SSC carries position +
  velocity for every IGS station globally; velocity propagation handles any
  date. SIF covers whatever NGS published.
- **Trivial to parse.** Only the `SOLUTION/ESTIMATE` `STA*`/`VEL*` rows are
  needed (~150 lines of Python), and it stays out of C++ entirely.

The truth lives in the **Python harness**, never in the engine config — see
`gnssviz.eval`.

### Frame / epoch caveats (where sub-cm comparisons live or die)

- **Frame consistency.** The truth datum (ITRF2020 / IGS20) must match the frame
  of the precise products the run used. The bundled `COD0MGXFIN` finals are
  IGS20-aligned ⇒ compare against an ITRF2020/IGS20 SSC. (Real-time SSR uses the
  provider's datum — out of scope for the frozen post-processing set.)
- **Velocity propagation is mandatory.** SSC epoch (2015.0) → run epoch (2024.35)
  is ~9 yr; station velocities give several cm. Done in `SinexTruth` / `SifTruth`.
- **Tide convention.** ITRF/IGS coordinates are conventional tide-free. Over a
  24 h static arc the diurnal/semidiurnal tides average out *only if modeled*; a
  wrong solid-tide / OTL / pole-tide model shows up as a vertical bias or
  sinusoid. The suite therefore doubles as a regression guard on the
  site-displacement models.
- **Secular vs actual.** SINEX gives the secular (regularized) position; SIF with
  `with_periodic=True` adds the empirical seasonal term (the *actual* position at
  epoch, which is what a daily static PPP estimates). They differ by the seasonal
  displacement (mm–cm). For A/B diffing on a fixed date this cancels; for
  absolute accuracy it does not.
- **Marker/ARP reduction.** The SSC coordinate is the monument reference; the
  solution is the phase center. The engine must reduce phase-center → marker via
  the RINEX antenna height + `igs20.atx` PCO, or a constant vertical bias
  appears.

## Architecture

```
data/<YYYY-DOY>/{products,obs/<station>}   bundled fixtures (internal mode)
data/static/sinex/*.snx                     frozen SSC truth (primary)   <- drop-in
data/static/sif/*_SIF.json                  bundled SIF truth (fallback)
config/test_suite/ppp_static.jsonc          A-cell base config (genuinely static)
        │
scripts/run_test_slice.py                   runs the (station × cell) matrix
   → per cell: deep-copy base + override patch → run gnss_processor.exe
   → read logs/state_trace.csv (week,tow_s,x_m,y_m,z_m)
        │
gnssviz.eval.truth   SinexTruth | SifTruth | CompositeTruth  (independent grader)
gnssviz.eval.metrics evaluate_state_trace → ENU error, bias/RMS, convergence
        │
   scorecard table + A/B deltas + slice_metrics.json
```

Reuses existing infrastructure:
[`scripts/run_batch.py`](../scripts/run_batch.py) (subprocess + JSONC + override
pattern), [`gnssviz.geodesy`](../pyviz/gnssviz/geodesy.py) /
[`gnssviz.io`](../pyviz/gnssviz/io.py) (ECEF↔ENU and GPS-time, so ENU sign
conventions match the `state` viewer). This is pyviz's own roadmap item
("truth-referenced accuracy → ENU error / convergence / RMS + run-comparison
scorecard").

### Components

| File | Role |
|------|------|
| [`pyviz/gnssviz/eval/truth.py`](../pyviz/gnssviz/eval/truth.py) | `TruthSource` ABC + `SinexTruth` (primary; solution-aware SINEX/SSC), `SifTruth` (faithful port of `resolveStationCoordinate`), `CompositeTruth`, `make_default_truth` |
| [`pyviz/gnssviz/eval/fetch.py`](../pyviz/gnssviz/eval/fetch.py) | `ensure_itrf_ssc` — auto-download the no-auth ITRF2020 IGS SSC (2.4 MB) into `data/static/sinex/`; `find_sinex_files` |
| [`pyviz/gnssviz/eval/metrics.py`](../pyviz/gnssviz/eval/metrics.py) | `evaluate_state_trace` → `PositionMetrics` (ENU bias/RMS/scatter over a convergence window, time-to-threshold) |
| [`pyviz/gnssviz/eval/regression.py`](../pyviz/gnssviz/eval/regression.py) | `cross_check_truth` (SINEX vs SIF) + `compare_metrics` (golden-metrics regression gate) |
| [`pyviz/gnssviz/eval/report.py`](../pyviz/gnssviz/eval/report.py) | `render_suite_report` — full HTML report: scorecard + regression-vs-golden (gate banner, current/golden + Δ-with-tolerance-band figures) + per-cell convergence curves auto-rebuilt from each `state_trace.csv`; `render_scorecard_html` = scorecard-only |
| [`config/test_suite/ppp_static.jsonc`](../config/test_suite/ppp_static.jsonc) · [`ppp_rtk_static.jsonc`](../config/test_suite/ppp_rtk_static.jsonc) | base configs: genuinely static PPP / PPP-RTK (`position_q=0`), internal data |
| [`config/test_suite/manifest.json`](../config/test_suite/manifest.json) | the matrix: stations × cells (base + override patch), truth/eval params, tolerances |
| [`scripts/run_test_suite.py`](../scripts/run_test_suite.py) | **manifest-driven runner**: fetch SINEX → cross-check → run matrix (parallel) → grade → scorecard → golden gate |
| [`scripts/run_test_slice.py`](../scripts/run_test_slice.py) | the original phase-1 thin slice (one station, one A/B); superseded by `run_test_suite.py` |
| [`scripts/fetch_sinex_subset.py`](../scripts/fetch_sinex_subset.py) | freeze a station subset out of a big IGS/ITRF SSC (optional) |
| `pyviz/tests/test_eval_*.py` | unit tests (truth, metrics, regression, report) — 40 cases |

## Static vs kinematic

Not a mode enum — just process-noise knobs (`filter.process_noise`):

- **Static:** `position_q = 0` + `estimate_velocity = false` ⇒ position is one
  constant state. (Note `config/ppp_internal.json` ships `position_q = 1e2`, a
  loose position random walk — *not* static; `ppp_static.jsonc` fixes this.)
- **Kinematic:** `estimate_velocity = true` + `acceleration_psd_q ≈ 1e0–1e2`.

A *static* IGS station processed kinematically uses the **same** SSC truth — the
metric is per-epoch ENU scatter (the kinematic noise floor) + time-to-converge,
not error vs a separate trajectory. That is the standard way to validate
kinematic PPP without a moving-platform reference (which we don't have).

## A/B and regression gating

- **A/B:** two matrix cells (feature on/off) differenced vs the base's reference
  (empty-overrides) cell; the SSC truth makes the delta "better/worse", not just
  "different".
- **Regression gate:** `run_test_suite.py --golden <golden.json>` diffs the
  current scorecard against a stored golden and **exits non-zero** if any cell's
  ENU bias or 3D-RMS moved beyond tolerance, *or* a previously-passing cell now
  fails/diverges/is missing, *or* the golden file is absent. A diverged run
  (all-NaN metrics) is graded `ok=False` and trips the gate rather than sneaking
  through. Seed/update with `--update-golden <path>`.

## Visual report

Every `run_test_suite.py` run also writes `<output_root>/report.html` (disable
with `--no-report`) — a standalone, self-contained page with three sections:

1. **Scorecard** — the text scorecard (cross-check + per-cell ENU bias/RMS/
   convergence + A/B deltas) plus per-cell 3D-RMS and Up-bias bar figures.
2. **Regression vs golden** (only with `--golden`) — a coloured **PASS/FAIL gate
   banner**, the per-cell delta table, and figures: current-vs-golden 3D-RMS, and
   Δ3D-RMS / ΔUp-bias per cell drawn against the ±tolerance band so a cell that
   crosses the gate is obvious (out-of-band points reddened).
3. **Convergence** — auto-generated 3D-error-vs-time curves rebuilt from every
   cell's `state_trace.csv` (one panel per station, one line per cell, threshold
   + warmup markers). This is the "auto-figure generation" applied to the suite.

`--per-cell-reports` additionally runs the full per-run viewer suite
(`run_report.py`) for each cell, writing `logs/gnss_report.html`; the suite
report then hyperlinks each cell to its detailed per-run figures. The report can
also be (re)built standalone from a saved `suite_metrics.json`:

```powershell
$env:PYTHONPATH = "C:\GnssProcessor\pyviz"
& $py -m gnssviz.eval.report C:\GnssProcessor\test_suite_results\matrix\suite_metrics.json `
      --golden C:\GnssProcessor\test_suite_results\golden.json
```

## Phasing

- **Phase 1 — DONE:** `gnssviz.eval` truth sources + metrics (unit-tested),
  static PPP config, A/B slice runner, JPLM + solid-tide A/B, graded vs SIF.
- **Phase 2 — DONE:** SINEX is live primary truth via `ensure_itrf_ssc`
  (auto-downloads the no-auth ITRF2020 IGS SSC, 2.4 MB, solution-aware parser),
  SINEX-vs-secular-SIF cross-check, golden-metrics regression gate (with
  divergence/failure handling), pyviz scorecard HTML.
- **Phase 3 — DONE:** `config/test_suite/manifest.json` matrix driven by
  `run_test_suite.py` — JPLM/CIT1/GCGO × {PPP, PPP-RTK, GPS-only vs GPS+GAL,
  static/kinematic, AR on/off, BSSD on/off, smoother on/off}, run in parallel on
  bundled data and graded vs truth.
- **Later:** grade the smoother cell against `smoother_trace.csv` (it currently
  grades the forward `state_trace`, so it reads identical to the static cell);
  a CDDIS-auth path for the operational weekly SINEX; more dates/stations.

## Findings surfaced while building

- **BaselineFixtures.hpp `IGS_Pasadena` CIT100USA reference ECEF was wrong** by
  ~1.7 km (`{-2492006.9389, -4659170.3174, 3559273.6829}` → h = −693 m, ~1.7 km
  from Caltech; the SIF value lands exactly on Caltech, h = 215 m). Since
  `baseline_single.json` holds CIT100USA *fixed* as the DD reference, baseline
  runs on that preset inherited a ~1.7 km absolute error. **FIXED**: replaced
  with the SIF/ITRF2020 value propagated to the obs epoch,
  `{-2491490.7288, -4660802.8538, 3559129.1818}` (ITRF2020 @ 2024.35). Verified:
  `baseline_single.json` on bundled CIT1+JPLM data now puts the rover at
  `(-2493305.029, -4655214.853, 3565497.712)` — **8.5 cm** from JPLM truth
  (E −0.5, N −1.8, U +8.3 cm), vs ~1.7 km off before. NARY (osu_campus, h=47 m,
  Corvallis) and the JPLM rover (h=424 m, JPL) were checked and are fine.
- **P38900USA** has its only SIF occupation flagged `DO_NOT_USE` ⇒ no SIF truth;
  use SINEX for P389 or exclude it.

## Results on the bundled matrix (2024-129)

12 cells, all graded vs the auto-downloaded ITRF2020 IGS SSC. Representative
(cm, ENU error vs truth):

| cell | E | N | U | 3D-RMS |
|------|---|---|---|--------|
| JPLM ppp_static | +1.4 | +0.1 | +5.7 | 6.4 |
| JPLM ppp_gps_only | −0.4 | −0.1 | +2.9 | 4.4 |
| JPLM ppp_kinematic | −0.4 | +1.8 | +8.0 | 12.4 |
| JPLM ppprtk_float | +1.5 | +0.1 | +5.2 | 5.8 |
| JPLM ppprtk_ar | +1.5 | −0.0 | +5.4 | 6.0 |
| CIT1 ppp_static | −1.6 | −2.5 | −4.8 | 7.1 |
| GCGO ppp_static | +2.4 | −5.7 | +3.3 | 8.1 |

A/B highlights: solid-tide off → JPLM dU **−13.9 cm**, CIT1 dU **−7.6 cm**;
AR vs float dU +0.17 cm (AR mainly speeds convergence on a converged static
solution). Truth cross-check: JPLM 0.5 cm, CIT1 1.4 cm agreement; **GCGO flagged
at 5.45 cm** (a newer station extrapolated past the SSC span — base ITRF2020 vs
SIF's 2023 update genuinely differ), so its absolute bias is treated as truth
ambiguity, not silently trusted.

## Running

```powershell
$py = "C:\GnssProcessor\venv\Scripts\python.exe"

# unit tests (truth / metrics / regression / report)
& $py -m pytest C:\GnssProcessor\pyviz\tests\test_eval_truth.py `
  C:\GnssProcessor\pyviz\tests\test_eval_metrics.py `
  C:\GnssProcessor\pyviz\tests\test_eval_regression.py `
  C:\GnssProcessor\pyviz\tests\test_eval_report.py -q

# the full matrix (auto-fetches SINEX, runs all cells, grades, scorecards)
& $py C:\GnssProcessor\scripts\run_test_suite.py --workers 3

# regression gate against a stored golden (exit non-zero on any regression)
& $py C:\GnssProcessor\scripts\run_test_suite.py --golden C:\GnssProcessor\test_suite_results\golden.json
& $py C:\GnssProcessor\scripts\run_test_suite.py --no-run --update-golden C:\GnssProcessor\test_suite_results\golden.json

# the gate run also auto-writes test_suite_results/matrix/report.html (scorecard +
# regression-vs-golden + convergence); add --per-cell-reports to also link each
# cell's full per-run gnss_report.html.  Rebuild it standalone from a saved scorecard:
$env:PYTHONPATH = "C:\GnssProcessor\pyviz"
& $py -m gnssviz.eval.report C:\GnssProcessor\test_suite_results\matrix\suite_metrics.json `
      --golden C:\GnssProcessor\test_suite_results\golden.json
```

Run outputs land under `test_suite_results/` (worth a `.gitignore` entry).
