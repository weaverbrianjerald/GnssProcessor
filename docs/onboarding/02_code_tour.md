# 2 · Code tour — follow one epoch through the PPP path

**Goal:** know where things live, so that when you are given a task you can find the
three or four files it touches. You do not need to understand every file — nobody
starts that way.

This tour covers the **single-receiver path**: PPP and PPP-RTK for one station, in
post-processing and in real time. That is where you will work. The multi-station
*network producer* is a separate research program; [skip it for now](#what-to-skip).

---

## The big picture

One JSON file describes a run. One driver class, `Application`, reads it and runs one
of four pipelines. You care about two of them:

```
gnss_processor.exe config\ppp_internal.json
        │
        ▼
   main()  ──►  Application::run()                      src/core/app/Application.cpp
                    │
                    ├─ app_mode = "post"       ──►  the epoch loop in run()          ◄── you, mostly
                    ├─ app_mode = "real_time"  ──►  runRealTime() → RealTimeEngine   ◄── you, later
                    ├─ mode = Baseline-*       ──►  runBaseline()
                    └─ mode = ppp-rtk-network  ──►  runNetwork()                     (skip)
```

Both of your pipelines share the same building blocks — the same observation model,
the same Kalman filter, the same slip detectors. Real time differs in *where the data
comes from* and in *what it must do when corrections arrive late or jump*.

## Before the first epoch

`Application::run()` sets the stage, in this order:

| Step | Function | What it does |
| --- | --- | --- |
| 1 | `initLoggingAndConfig()` | Reads the JSON into a `GnssConfig` object. Every setting in the code comes from a `GnssConfig` getter. |
| 2 | `resolveData()` | Finds the input files. **All** file finding goes through one class, `PathResolver`. With `data_source: "internal"` it looks in `data\`; with `"production"` it downloads from CDDIS. |
| 3 | `loadInputData()` | Parses the RINEX observations (`RinexParser`), then the products — orbits, clocks, biases, antenna file, ERP (`ProductParser`, `ProductCache`). |
| 4 | `initializePosition()` | Seeds the position from the RINEX header. |
| 5 | constructors | Builds `StateManager`, `PppModel`, `MeasurementModel`, `KalmanFilter`, `CycleSlipManager`, optionally `AmbiguityResolver`, and `TraceLogger` (the CSV writers). |

## One epoch

This is the heart of the program: the loop body in `Application::run()`. Read it with
this table next to you.

| # | Step | Function | In plain words |
| --- | --- | --- | --- |
| 1 | collect | `collectEpochObservations()` | Gather all observations with the same time tag. |
| 2 | **model** | `PppModel::computeModels()` | For every satellite: position and clock from the products, geometry, all corrections (troposphere, tides, antenna, relativity, wind-up, biases), signal selection — and the **variance** of each observation, from `StochasticModelWeight::computeVariance()`. The result is a list of `PppObservation`. |
| 3 | seed | `handleWlsSeeding()` | A quick least-squares fix to seed the receiver clock. |
| 4 | **predict** | `KalmanFilter::predict()` | Move the state forward in time and add process noise. |
| 5 | **slips** | `detectCycleSlips()` → `CycleSlipManager::detectAndRepair()` | Find carrier-phase cycle slips; reset or repair the affected ambiguities. |
| 6 | **update** | `runFilterUpdateLoop()` → `MeasurementModel::computeMeasurementUpdate()` → `KalmanFilter::update()` | Build the design matrix **H**, the residuals **z** and the noise matrix **R**; run the Kalman update. May repeat if the filter rejects observations. |
| 7 | fix | `AmbiguityResolver::resolve()` | Integer ambiguity resolution (MLAMBDA), if enabled. |
| 8 | write | `writeEpochTraces()` | One row per epoch into each CSV trace. |

After the last epoch, the optional backward smoother (`Smoother::run()`) runs, and the
program prints how long it took.

> **A design fact worth remembering.** Observation variances are decided in step 2,
> inside `PppModel` — *not* in the Kalman filter. `MeasurementModel` only copies them
> into **R**. If you change weighting, you work in `model/`, not in `filter/`. The
> [worked example](03_worked_example.md) does exactly this.

## The state vector

The filter's state (position, clock, troposphere, then a changing set of ambiguities,
ionosphere delays, biases) grows and shrinks as satellites rise and set. **`StateManager`
owns that layout.** Nothing else keeps its own index into the state; everything asks
`StateManager`. State names ("keys") are built and parsed only by the helpers in
`filter/StateKeySchema.hpp`.

## What a run writes

Everything goes to a folder named **`logs\`, created in the directory you ran from**
(it is not configurable). Run from the repository root and keep it that way.

| File | Contents |
| --- | --- |
| `logs\state_trace.csv` | **The solution**: time, position, clock, troposphere and their sigmas, one row per epoch. |
| `logs\measurement_residuals.csv` | Post-fit residuals per satellite and signal — the first place to look when something is wrong. |
| `logs\satellite_status_trace.csv`, `dop_trace.csv`, `ambiguity_trace.csv`, `ionosphere_trace.csv`, … | One CSV per topic. |
| `logs\<name>.log` | Text logs, one per subsystem (`main_log.log`, `kalman_filter.log`, `ppp_model.log`, …). |

Each CSV and log is switched on in the `logging` block of the config. A log needs
**two** settings to be useful: `enable_<name>: true` **and** a level in
`logging.levels` that is not `"warn"`.

`logs\` is overwritten by the next run. To keep a run, rename the folder:
`move logs logs_baseline`.

Plot a run:

```powershell
python pyviz\run_report.py logs          # writes an HTML report with the standard plots
```

## The config file

Open `config\ppp_internal.json`. The fields you will touch first:

| To change… | Edit |
| --- | --- |
| which station / day | `files.observation_path` (a RINEX file under `data\<YYYY-DOY>\obs\<station>\`) |
| constellations | `processing.active_systems` |
| elevation mask | `processing.elevation_mask_deg` |
| observation noise and weighting | `filter.measurement_noise`, `filter.stochastic_model` |
| process noise (how fast states may change) | `filter.process_noise` |
| which physical corrections are on | `model_corrections.*` |
| ambiguity resolution | `estimation.ambiguity_resolution.mode` |
| which traces and logs are written | `logging.*` |

Ready-made configs for your path, all in `config\`:

| File | Scenario |
| --- | --- |
| `ppp_internal.json` | PPP, ionosphere-free, bundled data |
| `ppp_rtk_internal.json` · `ppp_rtk_internal_AR.json` | uncombined PPP-RTK, float · with ambiguity resolution |
| `ppp_prod.json` · `ppp_rtk_prod.json` | same, but downloading products for any station/day |
| `sbf_replay.json` | real-time engine replaying a recorded Septentrio file — real time without a network connection |
| `realtime_igs_ntrip.json` · `realtime_ppp_rtk.json` | live real time over NTRIP with IGS corrections |

Everything else in `config\` belongs to the network program. (`config\README.md` is
out of date — trust the table above.)

## The real-time path

`Application::runRealTime()` opens the input streams and feeds frames to
`RealTimeEngine`. For each epoch, `RealTimeEngine::processEpoch()` makes a fixed
sequence of calls. Each group lives in its own file, so you always know where new code
belongs:

| Order | File (`src/core/realtime/`) | Responsibility |
| --- | --- | --- |
| 1 | `RealTimeEngineAdmit.cpp` | May this epoch run yet? (corrections arrived? data gap?) — the only place allowed to skip an epoch |
| 2 | — | `PppModel::computeModels()` — the *same* modelling as post-processing |
| 3 | `RealTimeEngineSsrDiag.cpp` | Observability of the correction stream |
| 4 | `RealTimeEnginePrepare.cpp` | Condition the measurement set before the predict |
| 5 | `RealTimeEngineSlipQc.cpp` | Cycle slips |
| 6 | `RealTimeEngineSsrBreaks.cpp` | React when the correction provider jumps or resets |
| 7 | `RealTimeEngineEstimate.cpp` | Kalman update, constraints, ambiguity resolution, traces |

Decoders for the input formats are in `src/core/rtcm/` (RTCM3 observations and IGS-SSR
corrections) and `src/core/sbf/` (Septentrio). Start with `sbf_replay.json`: it runs
the whole real-time engine from a file, so it is repeatable and you can debug it.

## Reading list

Read the **headers** (`include/…`), not the `.cpp` files — the headers carry the
documentation. In this order:

| # | Header | Why |
| --- | --- | --- |
| 1 | `app/Application.hpp` | the driver; every per-epoch step is declared here |
| 2 | `common/PppObservation.hpp` | the per-satellite record that flows through the whole epoch |
| 3 | `common/PppState.hpp` | the receiver state |
| 4 | `model/PppModel.hpp` | observation modelling and corrections |
| 5 | `model/StochasticModel.hpp` | observation weighting — small, and the subject of the worked example |
| 6 | `filter/StateManager.hpp` | the state-vector layout |
| 7 | `measurement/MeasurementModel.hpp` | builds H, z, R |
| 8 | `filter/KalmanFilter.hpp` | predict / update |
| 9 | `slip/CycleSlipManager.hpp` | cycle-slip detection and repair |
| 10 | `filter/AmbiguityResolver.hpp` | integer ambiguity resolution |
| 11 | `realtime/RealTimeEngine.hpp` | the real-time epoch |

The **math** behind each of these is in the LaTeX reference, `docs\report\main.pdf`.
Read the matching chapter together with the header: observation equations with
`PppModel`, the Kalman chapter with `KalmanFilter`, and so on.

## What to skip

Until you are asked to work on it, ignore everything belonging to the network
producer: files named `Network*` (mostly in `include/filter/` and `src/core/filter/`),
`ApplicationNetwork*.cpp`, `config\archive\`, `config\golden\`, `config\test_suite\`,
`docs\refound\`, `docs\cards\`, `docs\archive\`, and most of `scripts\`. That is about
two thirds of the repository. It is a careful research record, not clutter — it is just
not your starting point.

Next: [3 · Worked example](03_worked_example.md).
