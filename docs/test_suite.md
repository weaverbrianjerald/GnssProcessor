# Standard test run — real-data regression / A-B suite

A frozen real-data harness: `gnss_processor` over a matrix of configs (PPP / PPP-RTK,
GPS+Galileo, static / kinematic, feature toggles on/off) on bundled IGS stations, each run
graded against an **independent truth coordinate**. Two jobs: **absolute accuracy** (how
close to the published ITRF coordinate — validates the whole physics chain) and **A/B
config diff** (feature on vs off, in cm vs truth). There is no numerical unit-test suite
for the estimator, so this is the primary regression guard for numerical changes.

## Truth reference: IGS SINEX/SSC primary, NGS SIF fallback

**Decision: IGS/ITRF SINEX (SSC) is primary truth, NGS SIF the fallback + cross-check,
both behind one `TruthSource` interface.** Truth lives in the Python harness
(`gnssviz.eval`), never in the engine config. Why:

- **Independence.** `receiver.reference_position_ecef` seeds the state position
  (`PppModel.cpp` — "intended for fixed-station residual analysis with zero position
  process noise"), and a static run with zero position process noise barely moves off its
  seed, so grading against that seed reports ≈0 error regardless of whether the physics is
  right. The SIF is also engine-consumed (a-priori / OTL / QC) ⇒ partially circular. An IGS
  SSC is consumed **nowhere** in the engine — a frame or site-displacement bug cannot hide
  inside the grader.
- **Coverage from one file.** One ITRF2020/IGS20 SSC carries position + velocity for every
  IGS station globally; velocity propagation handles any date. SIF covers whatever NGS
  published.
- **Trivial to parse.** Only the `SOLUTION/ESTIMATE` `STA*`/`VEL*` rows (~150 lines of
  Python); stays out of C++ entirely.

### Frame / epoch caveats (where sub-cm comparisons live or die)

- **Frame consistency.** Truth datum must match the products' frame — bundled `COD0MGXFIN`
  finals are IGS20-aligned ⇒ compare against an ITRF2020/IGS20 SSC. (Real-time SSR uses the
  provider's datum; out of scope for the frozen post-processing set.)
- **Velocity propagation is mandatory.** SSC epoch 2015.0 → run epoch 2024.35 is ~9 yr =
  several cm. Done in `SinexTruth` / `SifTruth`.
- **Tide convention.** ITRF/IGS coordinates are conventional tide-free; over a 24 h static
  arc diurnal/semidiurnal tides average out *only if modeled*, so a wrong solid-tide / OTL /
  pole-tide model shows as a vertical bias or sinusoid — the suite doubles as a
  site-displacement regression guard.
- **Secular vs actual.** SINEX gives the secular (regularized) position; SIF with
  `with_periodic=True` adds the empirical seasonal term (the *actual* position at epoch,
  which is what a daily static PPP estimates). Cancels for A/B on a fixed date; does not
  cancel for absolute accuracy.
- **Marker/ARP reduction.** SSC = monument reference, solution = phase center. The engine
  must reduce phase-center → marker via the RINEX antenna height + `igs20.atx` PCO, or a
  constant vertical bias appears.

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

Reuses [`scripts/run_batch.py`](../scripts/run_batch.py) (subprocess + JSONC + override
pattern) and [`gnssviz.geodesy`](../pyviz/gnssviz/geodesy.py) /
[`gnssviz.io`](../pyviz/gnssviz/io.py) (ECEF↔ENU and GPS-time, so ENU sign conventions match
the `state` viewer) — pyviz's own roadmap item.

### Components

| File | Role |
|------|------|
| [`pyviz/gnssviz/eval/truth.py`](../pyviz/gnssviz/eval/truth.py) | `TruthSource` ABC + `SinexTruth` (primary; solution-aware SINEX/SSC), `SifTruth` (faithful port of `resolveStationCoordinate`), `CompositeTruth`, `make_default_truth` |
| [`pyviz/gnssviz/eval/fetch.py`](../pyviz/gnssviz/eval/fetch.py) | `ensure_itrf_ssc` — auto-download the no-auth ITRF2020 IGS SSC (2.4 MB) into `data/static/sinex/`; `find_sinex_files` |
| [`pyviz/gnssviz/eval/metrics.py`](../pyviz/gnssviz/eval/metrics.py) | `evaluate_state_trace` → `PositionMetrics` (ENU bias/RMS/scatter over a convergence window, time-to-threshold) |
| [`pyviz/gnssviz/eval/regression.py`](../pyviz/gnssviz/eval/regression.py) | `cross_check_truth` (SINEX vs SIF) + `compare_metrics` (golden-metrics regression gate) |
| [`pyviz/gnssviz/eval/report.py`](../pyviz/gnssviz/eval/report.py) | `render_suite_report` — scorecard + regression-vs-golden (gate banner, current/golden + Δ-with-tolerance-band figures) + per-cell convergence curves rebuilt from each `state_trace.csv`; `render_scorecard_html` = scorecard-only |
| [`config/test_suite/ppp_static.jsonc`](../config/test_suite/ppp_static.jsonc) · [`ppp_rtk_static.jsonc`](../config/test_suite/ppp_rtk_static.jsonc) | base configs: genuinely static PPP / PPP-RTK (`position_q=0`), internal data |
| [`config/test_suite/manifest.json`](../config/test_suite/manifest.json) | the matrix: stations × cells (base + override patch), truth/eval params, tolerances |
| [`scripts/run_test_suite.py`](../scripts/run_test_suite.py) | **manifest-driven runner**: fetch SINEX → cross-check → run matrix (parallel) → grade → scorecard → golden gate |
| [`scripts/run_test_slice.py`](../scripts/run_test_slice.py) | the original phase-1 thin slice (one station, one A/B); superseded by `run_test_suite.py` |
| [`scripts/fetch_sinex_subset.py`](../scripts/fetch_sinex_subset.py) | freeze a station subset out of a big IGS/ITRF SSC (optional) |
| `pyviz/tests/test_eval_*.py` | unit tests (truth, metrics, regression, report) — 40 cases |

## Static vs kinematic

Not a mode enum — just `filter.process_noise` knobs.

- **Static:** `position_q = 0` + `estimate_velocity = false` ⇒ position is one constant
  state. (⚠ `config/ppp_internal.json` ships `position_q = 1e2`, a loose random walk —
  *not* static; `ppp_static.jsonc` fixes this.)
- **Kinematic:** `estimate_velocity = true` + `acceleration_psd_q ≈ 1e0–1e2`.

A *static* IGS station processed kinematically uses the **same** SSC truth — the metric is
per-epoch ENU scatter (the kinematic noise floor) + time-to-converge, not error vs a
separate trajectory. That is the standard way to validate kinematic PPP without a
moving-platform reference (which we don't have).

## A/B and regression gating

- **A/B:** two matrix cells (feature on/off) differenced vs the base's reference
  (empty-overrides) cell; the SSC truth makes the delta "better/worse", not just "different".
- **Regression gate:** `run_test_suite.py --golden <golden.json>` diffs the current
  scorecard against a stored golden and **exits non-zero** if any cell's ENU bias or 3D-RMS
  moved beyond tolerance, *or* a previously-passing cell now fails/diverges/is missing, *or*
  the golden file is absent. A diverged run (all-NaN metrics) grades `ok=False` and trips the
  gate rather than sneaking through. Seed/update with `--update-golden <path>`.

## Visual report

Every `run_test_suite.py` run writes `<output_root>/report.html` (disable with
`--no-report`) — standalone, self-contained, three sections:

1. **Scorecard** — text scorecard (cross-check + per-cell ENU bias/RMS/convergence + A/B
   deltas) plus per-cell 3D-RMS and Up-bias bar figures.
2. **Regression vs golden** (only with `--golden`) — coloured **PASS/FAIL gate banner**, the
   per-cell delta table, and figures: current-vs-golden 3D-RMS, and Δ3D-RMS / ΔUp-bias per
   cell against the ±tolerance band (out-of-band points reddened).
3. **Convergence** — 3D-error-vs-time curves rebuilt from every cell's `state_trace.csv`
   (one panel per station, one line per cell, threshold + warmup markers).

`--per-cell-reports` additionally runs the full per-run viewer suite (`run_report.py`) per
cell, writing `logs/gnss_report.html` and hyperlinking each cell to it. The report can be
rebuilt standalone from a saved `suite_metrics.json` — command under **Running**.

## Phasing

**Phases 1-3 are all DONE** — truth sources + metrics, live SINEX primary truth with the
SINEX-vs-SIF cross-check, the golden regression gate, and the manifest-driven station × cell
matrix are built and running. **Still open:** grade the smoother cell against
`smoother_trace.csv` (it currently grades the forward `state_trace`, so it reads identical to
the static cell); a CDDIS-auth path for the operational weekly SINEX; more dates/stations.

## Findings surfaced while building

- **`BaselineFixtures.hpp` `IGS_Pasadena` CIT100USA reference ECEF was wrong by ~1.7 km** —
  `{-2492006.9389, -4659170.3174, 3559273.6829}` gives h = −693 m, ~1.7 km from Caltech (the
  SIF value lands exactly on Caltech, h = 215 m). `baseline_single.json` holds CIT100USA
  *fixed* as the DD reference, so baseline runs on that preset inherited the error.
  **FIXED**: replaced with the SIF/ITRF2020 value propagated to the obs epoch,
  `{-2491490.7288, -4660802.8538, 3559129.1818}` (ITRF2020 @ 2024.35). Verified —
  `baseline_single.json` on bundled CIT1+JPLM data now puts the rover at
  `(-2493305.029, -4655214.853, 3565497.712)`, **8.5 cm** from JPLM truth (E −0.5, N −1.8,
  U +8.3 cm) vs ~1.7 km before. NARY (osu_campus, h=47 m, Corvallis) and the JPLM rover
  (h=424 m, JPL) were checked and are fine.
- **P38900USA**'s only SIF occupation is flagged `DO_NOT_USE` ⇒ no SIF truth; use SINEX for
  P389 or exclude it.

## Results on the bundled matrix (2024-129)

12 cells, all graded vs the auto-downloaded ITRF2020 IGS SSC. Representative (cm, ENU error
vs truth).

⚠ **Build provenance: produced BEFORE the VS-2026 toolchain migration of 2026-08-12** (last
doc commit 2026-06-28), and standing invariant #6 states verification integers are
build-config-sensitive at the ppm level. The cm-level ENU values are very unlikely to move
materially, but **re-baseline `golden.json` under the current toolchain before quoting this
table as a GATE** rather than as a record.

| cell | E | N | U | 3D-RMS |
|------|---|---|---|--------|
| JPLM ppp_static | +1.4 | +0.1 | +5.7 | 6.4 |
| JPLM ppp_gps_only | −0.4 | −0.1 | +2.9 | 4.4 |
| JPLM ppp_kinematic | −0.4 | +1.8 | +8.0 | 12.4 |
| JPLM ppprtk_float | +1.5 | +0.1 | +5.2 | 5.8 |
| JPLM ppprtk_ar | +1.5 | −0.0 | +5.4 | 6.0 |
| CIT1 ppp_static | −1.6 | −2.5 | −4.8 | 7.1 |
| GCGO ppp_static | +2.4 | −5.7 | +3.3 | 8.1 |

A/B highlights: solid-tide off → JPLM dU **−13.9 cm**, CIT1 dU **−7.6 cm**; AR vs float
dU +0.17 cm (AR mainly speeds convergence on a converged static solution). Truth cross-check:
JPLM 0.5 cm, CIT1 1.4 cm agreement; **GCGO flagged at 5.45 cm** (a newer station extrapolated
past the SSC span — base ITRF2020 vs SIF's 2023 update genuinely differ), so its absolute
bias is treated as truth ambiguity, not silently trusted.

## Suite catalog (declarative suites)

A **suite** declares a slice of config space anchored to one base; `gnssviz.eval.suites`
(`expand_manifest_suites`, wired into `run_test_suite.py`) expands it into ordinary cells +
matrix rows, so the matrix no longer has to be hand-listed cell by cell.

```jsonc
"suites": [{
  "name": "sys_ppp", "base": "ppp", "stations": ["JPLM","GCGO"],
  "design": "full",                                   // full | ofat | pairwise
  "pinned": { "processing.frequency_mode": "all" },   // held constant across the suite
  "axes": {                                           // each level is an OVERRIDE PATCH
    "sys": { "G":  {"processing.active_systems": ["GPS"]},
             "GE": {"processing.active_systems": ["GPS","Galileo"]} },
    "dyn": { "static":    {"estimation.estimate_velocity": false},
             "kinematic": {"estimation.estimate_velocity": true,
                           "filter.process_noise.acceleration_psd_q": 1.0} }
  }
}]
```

A cell's override is the ordered merge `pinned + axis[0] patch + axis[1] patch + …` (later
wins); label defaults to `name_level1_level2` (override with `label_template`). The
**design** is the lever that tames the explosion:

| design | runs | catches | use for |
|--------|------|---------|---------|
| `full` | ∏ \|axis\| | the interaction between the (few) axes | a 1–2 axis grid you want to read in full |
| `ofat` | 1 + Σ(\|axis\|−1) — **linear** | main effects | sensitivity sweeps over many independent knobs |
| `pairwise` | ~tens (2-wise covering array) | all 2-way interactions | many axes, suspected coupling, full product too big |

**The "≤ 2 big variables per suite" rule** keeps every scorecard delta unambiguously
attributable: pin everything not under test, vary at most one or two axes. Run several
focused slices rather than the full product of dozens of variables.

### Tiers (run frequency)

| manifest | tier | contents |
|----------|------|----------|
| `manifest_smoke.json` | gate (every change) | PPP + PPP-RTK static, JPLM — the regression anchor |
| `manifest_suites.json` | system (per feature) | `sys_ppp`/`sys_ppprtk` = constellation × dynamics (full) + `tropo` |
| `manifest_sensitivity.json` | sensitivity (on demand) | `site_disp` ofat — the +5 cm-Up decomposition |
| `manifest_interaction.json` | interaction (on demand) | `ar_bssd` (full) + `robust` (pairwise, 8 corners → 4 cells) |
| `manifest_realtime.json` | real-time (on demand) | RT engine via SBF file replay + precise products, graded vs a frozen site coordinate |

Adding a suite: drop a `suites` entry into the matching tier's manifest (or a new manifest),
confirm every axis-value override key is **live in code** first (this codebase has
silently-dead toggles), then run. Each tier writes its own `report.html` and `golden.json`
under `test_suite_results/<tier>/`.

## Real-time replay tier

The post-processing tiers never exercise the **real-time engine** (`RealTimeEngine` →
SBF/RTCM decode → `processEpoch`). The RT tier closes that gap by reusing the *same* runner
and grader, unchanged, on a deterministic **file replay**:

```
config/test_suite/rt_replay_covl.jsonc   RT base: app_mode=real_time, input_type=file,
                                          input_format=sbf, precise products, static
data/sbf/covl1452.26_                     frozen SBF capture (1 h, Corvallis AsteRx-U)
data/static/sites/covl.json               frozen site truth  (SiteTruth)   <- independent
config/test_suite/manifest_realtime.json  COVL × rt_static, truth = SiteTruth
```

Three things make it work with almost no new harness code:

1. **Deterministic file replay already exists.** `input_type:"file"` + `input_format:"sbf"`
   drives the RT engine reproducibly off a frozen dump — no live caster. EOF →
   `engine.flush()` → clean finite run.
2. **The RT engine writes the same gradeable trace.** `state_trace.csv` has the identical
   `week,tow_s,x_m,y_m,z_m,…` header, so `evaluate_state_trace` grades it unchanged.
   `run_test_suite.py` detects an RT cell by the base config's `app_mode == "real_time"` and
   injects `realtime.input_path` (the station's `input_path` field) instead of
   `files.observation_path`; everything downstream is identical.
3. **Independent truth for a non-IGS receiver.** The Corvallis rooftop AsteRx-U is absent
   from any SINEX/SSC or NGS SIF, so its truth coordinate is frozen in a small **site
   database** (`data/static/sites/*.json`) read by `SiteTruth` — consumed *nowhere* in the
   engine, the same independence guarantee `SinexTruth` gives. Add it to a manifest's
   `truth.site_paths`.

### Precise products, not a live SSR stream — and why

Freezing a live SSR window (provider datum vs ITRF, recording the stream) was the plan's
hard part. This tier sidesteps it: the replay runs against **COD MGX final products**
(`data_source:"production"`, staged under `data_download.data_root`), i.e. PPP through the RT
*code path*, not PPP-RTK with live SSR. That gives a reproducible, network-free cm-grade RT
cell today; a true frozen-SSR PPP-RTK cell is a later addition (needs a recorded SSR dump +
frame-datum handling).

### Bootstrap seed and independence

This Meas3 capture carries no PVTGeodetic block and BSSD has no WLS cold-start, so the filter
needs *some* a-priori to lift satellites above the mask (a missing seed pins it at the
Earth-centred pole fallback forever). The base config seeds a **coarse (~10 m) approximate**
position — *not* the cm-level truth. The grade stays independent because the position is
**measurement-driven**: the filter pulls it several metres off the seed to a point fixed by
the observations + precise products (verified: epoch-1 jumps ~4 m toward the monument),
exactly as post-processing seeds from the RINEX APPROX POSITION (also ~m-level) and grades vs
an independent SSC. Run STATIC (`position_q = 0`) for a sharp converged coordinate.

### Stream-level invariants the position grade cannot see

Position grading is blind to defects that still leave a *plausible* position — an old
observation consuming a future-stamped SSR correction (the obs↔SSR backlog hole), or an
ephemeris extrapolated far past its fit interval (the BSSD epoch-blowup / RT
model-degradation root). Those are pinned by focused C++ gtests in
[`tests/test_realtime_seams.cpp`](../tests/test_realtime_seams.cpp)
(`RealtimeSsrFreshnessTest`, `RealtimeEphStalenessTest`), complementing the existing
RTCM/SSR/eph decode tests (`SsrCorrectionStoreTest`, `GlonassTodToGps`,
`LiveEphemerisStoreTest`, …) in `tests/test_rtcm.cpp`.

### Prerequisites

The SBF fixture (`data/sbf/covl1452.26_`) and the COD MGX final products for the fixture date
(2026-145) staged under `data_download.data_root` (e.g.
`C:/GnssProcessorSupport/products/2026-145/products/`). With both present the run is offline;
absent them the cell is skipped (fixture) or hits the network (products).

```powershell
$py = "C:\GnssProcessor\venv\Scripts\python.exe"
& $py C:\GnssProcessor\scripts\run_test_suite.py `
      --manifest C:\GnssProcessor\config\test_suite\manifest_realtime.json --workers 1
```

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
