# 2 · Code tour: one epoch through the PPP path

**Goal:** know where things are, so that the three or four files a task touches can be
found quickly. Understanding every file is not required.

The tour covers the **single-receiver path**: PPP and PPP-RTK for one station, in
post-processing and in real time. The multi-station network producer is a separate
research program and is [out of scope here](#what-to-skip).

---

## Overview

One JSON file describes a run. One driver class, `Application`, reads it and runs one
of four pipelines. Two of them matter here:

```
gnss_processor.exe config\ppp_internal.json
        │
        ▼
   main()  ──►  Application::run()                      src/core/app/Application.cpp
                    │
                    ├─ app_mode = "post"       ──►  the epoch loop in run()          ◄── main working area
                    ├─ app_mode = "real_time"  ──►  runRealTime() → RealTimeEngine   ◄── second working area
                    ├─ mode = Baseline-*       ──►  runBaseline()
                    └─ mode = ppp-rtk-network  ──►  runNetwork()                     (skip)
```

Both pipelines share the same building blocks: the same observation model, the same
Kalman filter, the same slip detectors. Real time differs in the data source and in
the handling of corrections that arrive late or jump.

## Before the first epoch

`Application::run()` prepares the run in this order:

| Step | Function | Purpose |
| --- | --- | --- |
| 1 | `initLoggingAndConfig()` | Reads the JSON into a `GnssConfig` object. Every setting in the code comes from a `GnssConfig` getter. |
| 2 | `resolveData()` | Locates the input files. All file location goes through one class, `PathResolver`. With `data_source: "internal"` it searches `data\`; with `"production"` it downloads from CDDIS. |
| 3 | `loadInputData()` | Parses the RINEX observations (`RinexParser`), then the products: orbits, clocks, biases, antenna file, ERP (`ProductParser`, `ProductCache`). |
| 4 | `initializePosition()` | Seeds the position from the RINEX header. |
| 5 | constructors | Builds `StateManager`, `PppModel`, `MeasurementModel`, `KalmanFilter`, `CycleSlipManager`, optionally `AmbiguityResolver`, and `TraceLogger` (the CSV writers). |

## One epoch

The loop body in `Application::run()` is the core of the program. The table follows it
step by step.

| # | Step | Function | Description |
| --- | --- | --- | --- |
| 1 | collect | `collectEpochObservations()` | Gathers all observations with the same time tag. |
| 2 | **model** | `PppModel::computeModels()` | Per satellite: position and clock from the products, geometry, all corrections (troposphere, tides, antenna, relativity, wind-up, biases), signal selection, and the **variance** of each observation from `StochasticModelWeight::computeVariance()`. The result is a list of `PppObservation`. |
| 3 | seed | `handleWlsSeeding()` | A least-squares fix that seeds the receiver clock. |
| 4 | **predict** | `KalmanFilter::predict()` | Propagates the state in time and adds process noise. |
| 5 | **slips** | `detectCycleSlips()` → `CycleSlipManager::detectAndRepair()` | Detects carrier-phase cycle slips; resets or repairs the affected ambiguities. |
| 6 | **update** | `runFilterUpdateLoop()` → `MeasurementModel::computeMeasurementUpdate()` → `KalmanFilter::update()` | Builds the design matrix **H**, the residuals **z** and the noise matrix **R**, then runs the Kalman update. Repeats if the filter rejects observations. |
| 7 | fix | `AmbiguityResolver::resolve()` | Integer ambiguity resolution (MLAMBDA), if enabled. |
| 8 | write | `writeEpochTraces()` | One row per epoch into each CSV trace. |

After the last epoch the optional backward smoother (`Smoother::run()`) runs, and the
elapsed time is printed.

> **Design fact.** Observation variances are set in step 2, inside `PppModel`, not in
> the Kalman filter. `MeasurementModel` only copies them into **R**. Changes to
> weighting are therefore made in `model/`, not in `filter/`. The
> [worked example](03_worked_example.md) is such a change.

## The state vector

The filter state (position, clock, troposphere, then a varying set of ambiguities,
ionosphere delays and biases) grows and shrinks as satellites rise and set.
**`StateManager` owns that layout.** No other class keeps its own index into the state;
all access goes through `StateManager`. State names ("keys") are built and parsed only
by the helpers in `filter/StateKeySchema.hpp`.

## Run output

All output goes to a folder named **`logs\`, created in the directory the program was
started from**. The location is not configurable. Runs are started from the repository
root.

| File | Contents |
| --- | --- |
| `logs\state_trace.csv` | **The solution**: time, position, clock, troposphere and their sigmas, one row per epoch. |
| `logs\measurement_residuals.csv` | Post-fit residuals per satellite and signal. The first file to inspect when a run looks wrong. |
| `logs\satellite_status_trace.csv`, `dop_trace.csv`, `ambiguity_trace.csv`, `ionosphere_trace.csv`, … | One CSV per topic. |
| `logs\<name>.log` | Text logs, one per subsystem (`main_log.log`, `kalman_filter.log`, `ppp_model.log`, …). |

Each CSV and log is switched on in the `logging` block of the config. A log needs
**two** settings to contain anything: `enable_<name>: true` **and** a level in
`logging.levels` other than `"warn"`.

`logs\` is overwritten by the next run. A run is kept by renaming the folder:
`move logs logs_baseline`.

Plotting a run:

```powershell
python pyviz\run_report.py logs          # writes an HTML report with the standard plots
```

## The config file

`config\ppp_internal.json` is the reference example. The fields changed most often:

| Setting | Field |
| --- | --- |
| Station and day | `files.observation_path` (a RINEX file under `data\<YYYY-DOY>\obs\<station>\`) |
| Constellations | `processing.active_systems` |
| Elevation mask | `processing.elevation_mask_deg` |
| Observation noise and weighting | `filter.measurement_noise`, `filter.stochastic_model` |
| Process noise (allowed rate of change of the states) | `filter.process_noise` |
| Physical corrections | `model_corrections.*` |
| Ambiguity resolution | `estimation.ambiguity_resolution.mode` |
| Traces and logs | `logging.*` |

Ready-made configs for this path, all in `config\`:

| File | Scenario |
| --- | --- |
| `ppp_internal.json` | PPP, ionosphere-free, bundled data |
| `ppp_rtk_internal.json` · `ppp_rtk_internal_AR.json` | Uncombined PPP-RTK, float · with ambiguity resolution |
| `ppp_prod.json` · `ppp_rtk_prod.json` | The same, with products downloaded for any station and day |
| `sbf_replay.json` | Real-time engine replaying a recorded Septentrio file: real time without a network connection |
| `realtime_igs_ntrip.json` · `realtime_ppp_rtk.json` | Live real time over NTRIP with IGS corrections |

Everything else in `config\` belongs to the network program. `config\README.md` is out
of date; the table above is current.

## The real-time path

`Application::runRealTime()` opens the input streams and feeds frames to
`RealTimeEngine`. For each epoch, `RealTimeEngine::processEpoch()` makes a fixed
sequence of calls. Each group of calls is in its own file, which also determines where
new code goes:

| Order | File (`src/core/realtime/`) | Responsibility |
| --- | --- | --- |
| 1 | `RealTimeEngineAdmit.cpp` | Admission: have corrections arrived, is there a data gap? The only place allowed to skip an epoch. |
| 2 | (in `RealTimeEngine.cpp`) | `PppModel::computeModels()`, the same modelling as post-processing |
| 3 | `RealTimeEngineSsrDiag.cpp` | Observability of the correction stream |
| 4 | `RealTimeEnginePrepare.cpp` | Conditioning of the measurement set before the predict |
| 5 | `RealTimeEngineSlipQc.cpp` | Cycle slips |
| 6 | `RealTimeEngineSsrBreaks.cpp` | Reaction to jumps or resets of the correction provider |
| 7 | `RealTimeEngineEstimate.cpp` | Kalman update, constraints, ambiguity resolution, traces |

Decoders for the input formats are in `src/core/rtcm/` (RTCM3 observations and IGS-SSR
corrections) and `src/core/sbf/` (Septentrio). `sbf_replay.json` is the recommended
starting point: it runs the complete real-time engine from a file, so runs are
repeatable and can be debugged.

## Reading list

The **headers** (`include/…`) carry the documentation and are read before the `.cpp`
files, in this order:

| # | Header | Content |
| --- | --- | --- |
| 1 | `app/Application.hpp` | The driver; every per-epoch step is declared here |
| 2 | `common/PppObservation.hpp` | The per-satellite record that passes through the whole epoch |
| 3 | `common/PppState.hpp` | The receiver state |
| 4 | `model/PppModel.hpp` | Observation modelling and corrections |
| 5 | `model/StochasticModel.hpp` | Observation weighting; small, and the subject of the worked example |
| 6 | `filter/StateManager.hpp` | The state-vector layout |
| 7 | `measurement/MeasurementModel.hpp` | Construction of H, z, R |
| 8 | `filter/KalmanFilter.hpp` | Predict and update |
| 9 | `slip/CycleSlipManager.hpp` | Cycle-slip detection and repair |
| 10 | `filter/AmbiguityResolver.hpp` | Integer ambiguity resolution |
| 11 | `realtime/RealTimeEngine.hpp` | The real-time epoch |

The **math** behind each header is in the LaTeX reference, `docs\report\main.pdf`.
Each header is best read with its chapter: the observation equations with `PppModel`,
the Kalman chapter with `KalmanFilter`, and so on.

## What to skip

Everything that belongs to the network producer is out of scope until a task requires
it: files named `Network*` (mostly in `include/filter/` and `src/core/filter/`),
`ApplicationNetwork*.cpp`, `config\archive\`, `config\golden\`, `config\test_suite\`,
`docs\refound\`, `docs\cards\`, `docs\archive\`, and most of `scripts\`. That is about
two thirds of the repository. It is a careful research record, not clutter, but it is
not the starting point.

Next: [3 · Worked example](03_worked_example.md).
