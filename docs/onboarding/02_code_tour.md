# 2 · Code tour: one epoch through the PPP path

**Goal:** know where things are, so that the three or four files a task touches can be
found quickly. Understanding every file is not required.

The tour covers the **single-receiver path**: PPP and PPP-RTK for one station, in
post-processing and in real time, and briefly the two-station baseline. The
multi-station network producer is a separate research program and is
[out of scope here](#what-to-skip).

The single-receiver code was reorganized in October 2026 (card UOM,
`docs\refound\UOM_user_object_model_2026-10-13.md`): every long function became a short
driver over named stages, each stage in the file of its concern. This page follows the
drivers. A function a step names is the place to read; its file is given beside it.

---

## Overview

One JSON file describes a run. One driver class, `Application`, reads it and runs one
of four pipelines. Two of them matter here:

```
gnss_processor.exe config\ppp_internal.json
        │
        ▼
   main()  ──►  Application::run()                      src\core\app\Application.cpp
                    │   reads the config, then dispatches on the mode
                    ├─ app_mode = "real_time"  ──►  runRealTime() → RealTimeEngine   ◄── second working area
                    ├─ mode = Baseline-*       ──►  runBaseline()
                    ├─ mode = ppp-rtk-network  ──►  runNetwork()                     (skip)
                    └─ otherwise (PPP, PPP-RTK)──►  runPpp()                         ◄── main working area
```

Both PPP pipelines use the same components: `PppModel` (the observation model),
`MeasurementModel` (rows, Jacobian, variances), `KalmanFilter`, `StateManager` (the state
layout), the MW and TurboEdit slip detectors, and the rebuild helpers of
`src\core\app\EpochUpdateSupport.cpp`. They do not run one epoch the same way: real time
has its own slip orchestration, update loop and ambiguity-resolution setup.
[Where real time differs](#where-real-time-differs) lists the differences.

## Before the first epoch

`Application::run()` reads the configuration (`initLoggingAndConfig()`: the JSON into a
`GnssConfig`; every setting in the code comes from a `GnssConfig` getter), sets the
Eigen thread count, and dispatches. `Application::runPpp()` (`Application.cpp`, 118
lines) then prepares the run in this order:

| Step | Function (file) | Purpose |
| --- | --- | --- |
| 1 | `resolveData()` (`Application.cpp`) | Locates the input files. All file location goes through one class, `PathResolver`. With `data_source: "internal"` it searches `data\`; with `"production"` it downloads from CDDIS. |
| 2 | `loadInputData()` (`Application.cpp`) | Parses the RINEX observations (`RinexParser`), then the products: orbits, clocks, biases, antenna file, ERP (`ProductParser`, `ProductCache`). |
| 3 | `StateManager` constructor (`filter\StateManagerLayout.cpp`), `logFamiliesAsArmed()` | Lays out the fixed core of the state (position, clock, troposphere, and the optional blocks) and prints the run-start table [state families as armed](#the-tables-a-run-prints). |
| 4 | `initializePosition()` (`app\PppSetup.cpp`) | Seeds the position from the RINEX header, or from the station information file when enabled. |
| 5 | `buildPppComponents_()` (`PppSetup.cpp`) | Builds `PppModel`, `MeasurementModel`, `KalmanFilter`, `WlsEstimator` and `CycleSlipManager`, in that order. |
| 6 | `buildGlobalIonosphereMap_()`, `buildIonoVariabilityMonitor_()`, `buildNeqAccumulator_()` | The optional components: the GIM, the adaptive ionosphere process noise, the normal-equation export. |
| 7 | `wireCycleSlipManager_()` | Hands the slip layer the biases and the sampling interval, and gives the Kalman filter the MW scores for its outlier tiebreak. |
| 8 | `enablePppTraceStreams_()`, `buildAidingSources_()` | The module traces (MW, TurboEdit, wind-up); the IMU and the other aiding sources. |
| 9 | `initializePppState_()` | `PppModel::initializeState`, then the station-information position re-applied. |
| 10 | `buildAmbiguityResolver_()`, `buildTraceLogger_()` | MLAMBDA when `estimation.ambiguity_resolution.mode` is above 0; the CSV writers, last. |

Steps 4 to 10 are private members of `Application` defined in
`src\core\app\PppSetup.cpp` and declared in `include\app\Application.hpp` in this order.

## One epoch

The epoch loop is the core of the program: about 70 lines of `runPpp()`, one named stage
per line. The values the stages hand to one another (the observations, the time step,
the modeled observations, the slip result, the accepted measurement) are the members of
one `PppEpochWork` (`include\app\PppPipelineWork.hpp`), built empty at the top of each
epoch. The stages live in three files of `src\core\app\`: `PppEpochSteps.cpp` (up to the
slips), `PppEpochUpdate.cpp` (the update and ambiguity resolution) and
`PppEpochRecords.cpp` (the smoother snapshots and the outputs).

| # | Stage | Function (file) | Description |
| --- | --- | --- | --- |
| 1 | collect | `collectEpochObservations()` (`PppEpochSteps.cpp`) | Gathers the observations with the same time tag, of the active systems. |
| 2 | gates | `skipDecimatedEpoch_()`, `reachedMaxDuration_()` | Decimation (`processing.gnss_rate_s`) and the duration cutoff (`processing.max_duration_s`). |
| 3 | time step | `stepEpochClock_()` | The epoch stamp, the time step `dt` and the data-gap test. |
| 4 | sync | `syncPppState_()` | Copies the state vector into `ppp_state_`, the receiver state the geometry reads. |
| 5 | **model** | `computeEpochModels_()` → `PppModel::computeModels()` (`model\PppModel.cpp`) | Per satellite: position and clock from the products, geometry, all corrections (troposphere, tides, antenna, relativity, wind-up, biases), signal selection, and the **variance** of each observation from `StochasticModelWeight::computeVariance()`. The result is a list of `PppObservation`, and a drop reason for each satellite left out. |
| 6 | clock seed | `handleWlsSeeding()` | A least-squares fix that seeds the receiver clock (no receiver clock under BSSD). |
| 7 | gap policy | `handleGap()` | On a data gap only, by dynamics mode: zeroes the clock (and, without an IMU, the position) cross-covariances to the satellite states; after a long static gap or any kinematic gap without an IMU it also prunes the ionosphere, ambiguity and satellite-bias states, clears the soft-LLI bridge and drops the BSSD pivots. |
| 8 | Q overrides | `refreshSubstitutedAtxQ_()`, `refreshAdaptiveIonoQ_()` | Per-state process noise for the next predict: ambiguities whose antenna correction was frequency-substituted, and the adaptive ionosphere. Both inert unless armed. |
| 9 | **predict** | `predictEpoch_()` → `KalmanFilter::predict()` (`filter\KalmanFilter.cpp`) | Propagates the state; the process noise comes from `StateManager::getProcessNoiseMatrix()` (`filter\StateManagerProcessNoise.cpp`). |
| 10 | smoother prior | `capturePriorSnapshot()` (`PppEpochRecords.cpp`) | The a-priori state for the backward smoother, then `ppp_state_` re-synced. |
| 11 | **slips** | `detectCycleSlips()` → `CycleSlipManager::detectAndRepair()` (`slip\CycleSlipManager.cpp`); `foldSlipRepairsIntoPrior_()` | Detects carrier-phase cycle slips, then resets or integer-repairs the affected ambiguities; integer repairs are folded into the smoother's a-priori. |
| 12 | **update** | `runFilterUpdateLoop()` (`PppEpochUpdate.cpp`) → `MeasurementModel::computeMeasurementUpdate()` → `KalmanFilter::update()` | Builds the design matrix **H**, the innovations **z** and the noise matrix **R**, then runs the Kalman update. Repeats while the filter asks for a rebuild (a row excluded or an ambiguity reset); each rebuild passes the filter's resets on to the slip detectors and re-inflates the reset satellites' states. |
| 13 | relinearize | `runGeometryRelinearization()` | Optional (`relinearize_geometry`): an iterated update from the filter's stored prior. |
| 14 | resets merged | `mergeEpochResets_()` | The filter's and the slip layer's resets of the epoch, for the smoother. |
| 15 | GIM | `applyGimIonoUpdate()` | Optional: the GIM as a pseudo-observation on the slant-ionosphere states. |
| 16 | smoother posterior | `capturePosteriorSnapshot()`, `accumulateNormalEquations_()`, `recordBridgeSnapshot_()` | The float solution recorded for the smoother (before AR); the optional NEQ export; the soft-LLI bridge's last-good snapshot. |
| 17 | **fix** | `resolveEpochAmbiguities_()` → `AmbiguityResolver::resolve()` | Integer ambiguity resolution (MLAMBDA), if enabled, after the antenna-correction gates; the fixed state is written and held. |
| 18 | write | `logEpochSummary()`, `writeEpochTraces()` (`PppEpochRecords.cpp`) | The epoch summary line and one row per CSV trace. The traces read the state after AR; the summary line reads `ppp_state_`, which is not re-synced after AR, so it prints the float position (card UOM defect P-D4). |

Three steps do more than their names say:

- **`computeMeasurementUpdate()` changes the state layout.** Before it builds a row it
  seeds the first-epoch inter-system biases, phase clock and receiver code biases
  (values from `Seeds.cpp`, applied in `MeasurementModelSeeds.cpp`, latched by
  `StateInitializer`), runs the BSSD pivot handover, births and prunes the dynamic states
  of the epoch (`ensureRequiredStates()`), and freezes the pivot covariances.
- **`KalmanFilter::update()` acts on what it finds.** Its post-fit screen may exclude a
  row, reset an ambiguity, queue an **R** inflation for the next pass, request a BSSD
  pivot handover, or reset the whole filter. The update loop drains the requests
  (`forwardKfPivotHandovers()`, `src\core\app\EpochUpdateSupport.cpp`) and rebuilds.
- **`KalmanFilter::beginEpoch()`**, called once per epoch before the update loop, gates
  the concise filter log and also clears the pending Huber **R** inflations. Only this
  driver calls it (card UOM defect K-D1).

## After the last epoch

The run end is a table. `Application::reportPppRunEnd_()` (`src\core\app\PppRunEnd.cpp`)
walks `kPppRunEndReports`, one row per report, in print order:

| Report | Armed by | Where it prints |
| --- | --- | --- |
| wind-up census | `estimation.trace_windup_census` | `ppp_model` log |
| RTS smoother and its trace (`Smoother::run()`) | `processing.run_smoother` | `smoother` log, `smoother_trace.csv` |
| normal-equation export | `processing.enable_neq_export` | `.neq` and `.asc` files, named in `main_log` |
| processing time | always | `main_log` |
| state re-inflations (card OWD) | always | `main_log` |
| Kalman filter timings and [QC counters](#the-tables-a-run-prints) | always | `main_log` |
| [cycle-slip counters](#the-tables-a-run-prints) | always | `cycle_slip_manager` log |

The optional HTML report (`pyviz\run_report.py`) runs after the table.

> **Design fact.** The base variance of each observation is set in step 5, inside
> `PppModel`, by the stochastic model. `rowVariance()`
> (`src\core\measurement\MeasurementModelVariance.cpp`) starts from it and adds terms
> only for a PPP-RTK user of network corrections (the correction URA, clock staleness,
> phase-bias sigma) and the optional code downweight for signals without a `.BIA`
> correction; when armed, `KalmanFilter::update()` then inflates **R** for persistent
> phase biases and for the Huber targets of the previous pass. A change to the weighting
> model is
> therefore made in `model\`, not in `filter\`. The
> [worked example](03_worked_example.md) is such a change.

## Where to start reading each component

### The Kalman filter

`include\filter\KalmanFilter.hpp` first. The class is split by concern over five files
in `src\core\filter\`:

| File | Content |
| --- | --- |
| `KalmanFilter.cpp` | The driver: construction, `predict()`, `predictAided()`, `update()` (392 lines: R and its inflations, the gain, the Joseph covariance update, the post-fit residuals, then the screen), `updateFromPrior()` (the iterated step), `beginEpoch()`, `endEpoch()`, the run-end timings and counters. |
| `KalmanKernels.cpp` | The algebra: the sparse **H** products, the aided-predict splice and the dense Joseph-form update. Stateless. |
| `KalmanResidualQc.cpp` | The statistical QC kernel: redundancy, w-test, MDB, variance factor. Pure. |
| `KalmanPostFitQc.cpp` | The post-fit outlier screen. `validatePostFitResiduals()` is a 33-line driver over stages S0 to S9 that share one `PostFitQcWork`. |
| `KalmanOutlierResponse.cpp` | The responses the screen calls: the ambiguity reset behind a phase outlier, the code-row exclusion, the complete reset. |

`include\filter\KalmanPostFitQc.hpp` lists the screen's stages and its four exits in
order: the residual report, the statistical QC, the group detectors (coherent pivot
offsets, then one non-pivot satellite slipping on several bands), the Huber pass, the
gate row (sigma threshold or w-test), the dispatch (reset a phase row, exclude a code
row, with the MW tiebreak), the complete-reset backstop and the clean commit. The four
coherent-offset pivot detectors (phase per signal, phase across bands, code per signal,
code across bands) are the four rows of one table, `kalman::kPivotDetectors`, in the same
header. Every QC event increments one member of `KalmanQcCounters`
(`include\filter\KalmanQcCounters.hpp`), printed at run end.

### The measurement model

`include\measurement\MeasurementModel.hpp` first; it holds the table "the state
pre-pass, in call order" (each state category's birth function, gate, seed and initial
variance). The class is split over six files in `src\core\measurement\`, and its row is
built by the observation equation's files beside them:

| File | Content |
| --- | --- |
| `MeasurementModel.cpp` | `computeMeasurementUpdate()`: the order of the epoch's steps (first-epoch seeds, pivot handover, the state pre-pass, pivot freeze) and the row loop. |
| `MeasurementModelStates.cpp` | The state pre-pass: `ensureRequiredStates()`, a 30-line driver over 13 members called in a fixed order (the keep sets, slant ionosphere, phase colour, the network's cold clock seed, the BSSD pivot seeds, per-band or NL/WL ambiguities, then, after the ambiguity prune, the AR eligibility marks and the receiver and satellite code biases). The order is a dependency chain. |
| `MeasurementModelSeeds.cpp` | The first-epoch seeding of the single-receiver state and the ambiguity seed: `initializeInterSystemBiases()`, `initializePhaseClockOffset()`, `initializeCodeBiases()`, `preseedColdReceiverClock()` and `ambiguitySeed()`. Each asks `Seeds.cpp` for the values and writes them to the `StateManager`. |
| `Seeds.cpp` | The seeds as pure functions of the observation equation (`include\measurement\Seeds.hpp`): the residual a seed is formed from (the equation less the terms already known, two masks), and the seeds of the inter-system biases, cold clock, phase clock, receiver code biases, slant ionosphere and ambiguities. They read a lookup and return values; nothing is written. |
| `MeasurementModelRow.cpp` | One row: `buildJacobianRow()`, which builds the term list of the observation equation over the state (`ObservationEquation.cpp`, `UserParamView.cpp`), sums it for the predicted observable and scatters the partials; the Doppler row; and the definitions of the free functions `isUsableMeasurement()` and the product coverage checks (declared in `include\measurement\RowRules.hpp`). |
| `ObservationEquation.cpp` | The observation equation of a code or phase row as a list of terms in the order of the sum: one function per effect (clock, troposphere, ionosphere, biases, ambiguity), `evaluate()` for the predicted observable and `scatterDense()` for the Jacobian row. Parameters are named by `ParamRef` (`include\common\ParamRef.hpp`) and found through a lookup the caller supplies. |
| `UserParamView.cpp` | The single-receiver lookup: where a `ParamRef` sits in a `StateManager` and a state vector, and the one place a typed identity becomes a user-side state key (`userKey()`). |
| `MeasurementModelRowReference.cpp`, `ObservationEquationCheck.cpp` | Temporary. The row builder as written before the term list, and the counters of `processing.obs_equation_check`, which builds every row both ways and compares the bits. |
| `MeasurementModelVariance.cpp` | `rowVariance()`, a free function declared in `RowRules.hpp`: the variance of one row, its terms in the order the banner lists. |
| `MeasurementModelDd.cpp` | The baseline double difference: `buildDdMeasurement()`, a 28-line driver over stages (`ddRunStages()`); the three stages that write filter state are named in it. |
| `MeasurementModelNetwork.cpp` | The part only the network producer reaches. Skip. |

### The state families

`StateManager` owns the state vector and covariance. The fixed core (position, clock,
troposphere, and the optional velocity, gradients, IMU and inter-system offsets) is laid
out by the constructor (`src\core\filter\StateManagerLayout.cpp`, four named steps). The
rest is nine **dynamic families**, one key-to-index map each: ambiguities, slant
ionosphere and its rate, receiver code biases, satellite code biases, and four that only
network stations use (satellite phase bias, receiver phase bias, phase colour, satellite
clock).

`include\filter\StateFamily.hpp` declares them once: `kStateFamilies`, one row per
family with its name, label prefix, RT snapshot tag, re-inflation counter, gap response,
the rule that names a key's satellite, its process-noise rule and field, the
configuration that arms its birth, and the source of its initial variance. The state
size, the index shift after a removal, the state labels, the RT snapshot, the gap
decoupling and the run-start table are loops over this table. Process noise is built by
`StateManager::getProcessNoiseMatrix()` (`StateManagerProcessNoise.cpp`), a driver with
one function per block of **Q**.

### The slip layer

`include\slip\CycleSlipManager.hpp` first. `CycleSlipManager::detectAndRepair()`
(`src\core\slip\CycleSlipManager.cpp`) is a 32-line driver over sections S0 to S17 that
share one `SlipEpochWork` (`include\slip\CycleSlipEpochWork.hpp`). S0 and S17, the
epoch's frame, are beside the driver; the others live by concern:

| Sections | File | Content |
| --- | --- | --- |
| S1 to S8 | `CycleSlipDetection.cpp` | The soft-LLI gap holds; the detectors run (GRAPHIC and EWL, then MW and the receiver's loss of lock); TurboEdit and the soft-LLI bridge resolve; the MW results are grouped per satellite. |
| S9, S12, S13 | `CycleSlipPivot.cpp` | The BSSD pivot frame: which pivots slipped, the frame repaired in place, the forced handover. |
| S11, S14 | `CycleSlipAdjudication.cpp` | One satellite's MW mandates: `adjudicateSatellite()` returns a `SatelliteSlipVerdict` (`include\slip\CycleSlipVerdict.hpp`) and writes no state; `handleSatelliteMandates()` applies it through one action (skip, repair the culprits, suppress, or full reset). |
| S15, S16 | `CycleSlipNonMwPasses.cpp` | The evidence that is not an MW mandate: TurboEdit's geometry-free-only resolutions, the auxiliary detectors' resets. |
| | `CycleSlipRepair.cpp` | The two state primitives every response uses: the BSSD-aware ambiguity reset and the covariance-preserving integer repair. |

The detectors themselves follow the same pattern. `MwSlipDetector` has two drivers in
`MwSlipDetector.cpp`, `detectLargeMwResets()` (one epoch, steps E0 to E3) and
`processMwSample()` (one sample, steps S1 to S12), listed in
`include\slip\MwDetectorWork.hpp`; `include\slip\MwSlipDetector.hpp` names the file of
every other body. `TurboEditResolver::resolve()` (`TurboEditResolver.cpp`) is a driver
over steps R0 to R9. The manager's MW, GRAPHIC and EWL detectors are built by
`SlipDetectorRegistry::createEnabled()`. `IfPhaseSlipDetector` is built only by the
network producer; its user-path branches are staged in `IfPhaseStagedUserPath.cpp`,
which no configuration reaches.

## The tables a run prints

Three tables state what a run estimates and what its quality control did. The excerpts
below are from a 10-minute run of `ppp_internal.json` (ionosphere-free PPP) with every
module log enabled at level `info`, with the leading time stamps removed and long lines
cut (`...`).

**State families as armed**, at run start, in `logs\state_manager.log`
(`logging.enable_state_manager`, level `info`; `ppp_internal.json` sets both). One line
per family: whether this configuration arms its birth, and why; its label and snapshot
tag; its initial standard deviation and the key it comes from; its process noise; the
events wired to it.

```
State families as armed (born: the configuration arms the family's birth site; the core layout is logged above):
  slant ionosphere:     born no (processing.uncombined_enabled); label IONO:; snapshot ION; initial std filter.initial_std.ionosphere = 5; ...
  ...
  ambiguity:            born yes (every processed phase row); label AMB:; snapshot AMB; initial std GnssConfig::getAmbiguityStd() = 1000 (no JSON key; ...
  ...
  satellite code bias:  born yes (estimation.estimate_sat_biases); label SATB:; snapshot SATB; initial std filter.initial_std.sat_bias = 10; Q random walk, ...
  ...
```

**KF QC event counters**, at run end, in `logs\main_log.log`, after the KF timing
summary: one line per event of the post-fit screen. A QC branch that never acts reads 0.

```
KF QC event counters (post-fit screen and its responses):
  pivot group slips (phase)      : 0
  pivot code disturbances        : 0
  non-pivot multi-band slips     : 0
  Huber R targets queued         : 0
  data-snooping rejections       : 0
  phase rows reset               : 4
  ...
```

**Cycle-slip counters**, at run end, in `logs\cycle_slip_manager.log`
(`logging.enable_cycle_slip_manager`, level `info`; of the ready-made post-processing
configs only `ppp_rtk_internal_AR.json` sets both): detections per built detector,
mandates, repairs, resets, suppressions and pivot actions, each label with its unit.

```
Cycle-slip counters at run end (post-processing, 20 epochs):
  detections, mw (result entries)                          8
  MW mandates (ambiguity keys)                             8
  integer repairs (covariance kept)                        0
  ambiguity resets (BSSD-aware calls)                      8
  full satellite resets (satellites)                       6
  ...
```

The state-families table is printed by the PPP and baseline drivers, the counters by the
PPP driver (and the KF counters by the baseline driver); the real-time engine prints none
of them.

## The state vector

The filter state (position, clock, troposphere, then a varying set of ambiguities,
ionosphere delays and biases) grows and shrinks as satellites rise and set.
**`StateManager` owns that layout.** No other class keeps its own index into the state;
all access goes through `StateManager`. State names ("keys") have one format each,
defined by the helpers in `filter\StateKeySchema.hpp`; new code builds and parses keys
only through them. Older code on the single-receiver path still formats some keys by
hand (`sat + "_" + signal`); card UOM step U8 moves those onto the helpers and the typed
identities of `common\GnssIdentity.hpp`.

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

`Application::runRealTime()` (`src\core\app\ApplicationRealtime.cpp`) opens the input
streams and feeds frames to `RealTimeEngine`. For each epoch,
`RealTimeEngine::processEpoch()` (`src\core\realtime\RealTimeEngine.cpp`) makes a fixed
sequence of calls. Each group of calls is in its own file, which also determines where
new code goes:

| Order | File (`src\core\realtime\`) | Calls |
| --- | --- | --- |
| 1 | `RealTimeEngineStateSnap.cpp` | `applyStagedStateSnapshot_()`: a staged warm restart, if any |
| 2 | `RealTimeEngineAdmit.cpp` | Admission: the backlog and synchronization deferral, the duration budget, the time step and data-gap test |
| 3 | `RealTimeEngine.cpp` (the driver) | The `ppp_state_` sync, the active-system filter, the producer's declared signal roster (`refreshDeclaredRoster_()`, in `RealTimeEngineSsrBreaks.cpp`), then `PppModel::computeModels()`, the same modelling as post-processing |
| 4 | `RealTimeEngineAdmit.cpp` | The corrections-ready startup gate |
| 5 | `RealTimeEngineSsrDiag.cpp` | Observability of the correction stream |
| 6 | `RealTimeEnginePrepare.cpp` | Conditioning of the measurement set before the predict (broadcast-grade drop, WLS clock seed, BSSD re-gauge) |
| 7 | `RealTimeEngine.cpp` (the driver) | `KalmanFilter::predict()` or `predictAided()` |
| 8 | `RealTimeEngineSlipQc.cpp` | Cycle slips |
| 9 | `RealTimeEngineSsrBreaks.cpp` | Reaction to jumps or resets of the correction provider |
| 10 | `RealTimeEngineEstimate.cpp` | Kalman update loop, constraints, ambiguity resolution, traces |
| 11 | `RealTimeEngineSsrDiag.cpp` | The per-satellite consumer readout |
| 12 | `RealTimeEngineStateSnap.cpp` | `writeStateSnapshot_()`: the filter-state snapshot, when configured |

An epoch ends early only by returning false: from an admission gate (rows 2 and 4), or
in the driver itself on an empty epoch, a failed state sync, an epoch with no
observation of an active system, or a failed `computeModels()`.

Decoders for the input formats are in `src\core\rtcm\` (RTCM3 observations and IGS-SSR
corrections) and `src\core\sbf\` (Septentrio). `sbf_replay.json` is the recommended
starting point: it runs the complete real-time engine from a file, so runs are
repeatable and can be debugged.

### Where real time differs

| Concern | Post-processing (`runPpp`) | Real time (`processEpoch`) |
| --- | --- | --- |
| Cycle slips | `CycleSlipManager::detectAndRepair()`: MW, TurboEdit, the soft-LLI bridge, GRAPHIC and EWL, with the verdict and action of `CycleSlipAdjudication.cpp` | `detectAndRepairSlips()` (`RealTimeEngineSlipQc.cpp`): MW, TurboEdit and the soft-LLI bridge, its own orchestration; no `CycleSlipManager` |
| MW tiebreak in the filter's outlier dispatch | wired by `wireCycleSlipManager_()` | not wired |
| Pending Huber **R** inflations | cleared every epoch by `KalmanFilter::beginEpoch()` | never cleared (card UOM defect K-D1) |
| Data gap | `handleGap()`, by dynamics mode | only the WLS clock re-seed (`seedClockFromWls()`) |
| Ambiguity-resolution setup | `buildAmbiguityResolver_()` (`PppSetup.cpp`), 20 fields | `RealTimeEngineInit.cpp`, 18 fields: no partial AR, no arc-maturity gate |
| ATX-substituted ambiguity process noise | `refreshSubstitutedAtxQ_()` every epoch | only the birth variance, in `MeasurementModel` |
| Run-end counters | KF QC counters and slip counters | none |
| Shared | `computeRebuildPassBudget()`, `forwardKfPivotHandovers()`, `reinflateResetSatStates()` (`src\core\app\EpochUpdateSupport.cpp`) | the same three |

## The baseline path

`Application::runBaseline()` (`src\core\app\ApplicationBaseline.cpp`) processes a
reference station and one or more rovers by double differences. It is a driver over
three levels of stages: the run (data, products, the reference station), one rover
(`processBaselineRover_()`: its a-priori position, filter, AR and traces), and one epoch
of one rover (`processBaselineEpoch_()`, 29 lines: the duration cutoff, station presence,
the data-gap reset, the MW pre-filter, the models, the update loop, AR and the traces).
Each level's shared values are one struct (`BaselineRunWork`, `BaselineRoverWork`,
`BaselineEpochWork`). The rows come from `MeasurementModel::buildDdMeasurement()`; the MW
detectors are built directly, without `CycleSlipManager`.

## Where new user-side code goes

Each kind of change has one place. The driver functions stay lists of calls.

- **A new dynamic state family**: an enumerator of `StateFamily`, `kStateFamilyCount`
  and one row of `kStateFamilies` (`include\filter\StateFamily.hpp`), its key-to-index map in
  `StateManager` with its case in `indicesOf_()`, and its key format in
  `StateKeySchema.hpp`. Its process noise is one call in `getProcessNoiseMatrix()`
  (`StateManagerProcessNoise.cpp`); a new birth gate is a `BirthGate` enumerator with its
  two cases in `StateManagerFamilies.cpp`. Its birth is a member of the state pre-pass,
  its column a term of the observation equation (both below). The state size, labels,
  snapshot, gap decoupling and run-start table follow from the row.
- **A new fixed core state**: the constructor's steps in `StateManagerLayout.cpp`
  (`layoutCore_()`, `seedCoreVariances_()`, `logLayout_()`) and its block of **Q**.
- **A new state birth, seed or prune in the pre-pass**: a member in
  `MeasurementModelStates.cpp`, called by `ensureRequiredStates()` at its place in the
  dependency order, and a row of the pre-pass table in `MeasurementModel.hpp`.
- **A new term of the observation equation**: an `ObsEffect` and its function in
  `observationEquation()` (`ObservationEquation.cpp`), at its place in the order of the sum;
  the column is named by a `ParamRef` (`include\common\ParamRef.hpp`) and found by
  `UserParamView::find()`; a rule for whether a row enters: `isUsableMeasurement()`. Whether
  a seed knows the term: the masks `kLegacySeedMask` (`Seeds.hpp`).
- **A new variance term**: `rowVariance()` (`MeasurementModelVariance.cpp`), at its place
  in the composition; a new weighting model: `model\StochasticModel`
  ([worked example](03_worked_example.md)).
- **A new baseline double-difference stage**: a stage of `ddRunStages()`
  (`MeasurementModelDd.cpp`) over `DdBuild`; a stage that writes filter state is named as
  such in the driver.
- **A new post-fit QC stage**: a private stage of `validatePostFitResiduals()` in
  `KalmanPostFitQc.cpp`, over `PostFitQcWork`, and a line of the stage list in
  `KalmanPostFitQc.hpp`. Its event is a member of `KalmanQcCounters` and a row of
  `kKalmanQcCounterRows`.
- **A new coherent-offset pivot detector**: a row of `kalman::kPivotDetectors`
  (`KalmanPostFitQc.hpp`); a new option is a field of `PivotDetectorSpec`.
- **A new response to an outlier**: `KalmanOutlierResponse.cpp`, called from the
  dispatch stage. Algebra: `KalmanKernels.cpp`; the statistical kernel:
  `KalmanResidualQc.cpp`.
- **A new slip response**: a `SatelliteSlipAction` and the fields it needs in
  `SatelliteSlipVerdict` (`CycleSlipVerdict.hpp`), decided in `adjudicateSatellite()` and
  applied by its action in `handleSatelliteMandates()` (`CycleSlipAdjudication.cpp`).
  State edits go through `CycleSlipRepair.cpp`; each response counts in
  `CycleSlipCounters` (`include\slip\CycleSlipCounters.hpp`) and its row in
  `cycleSlipCounterLines()`.
- **A new section of the slip epoch**: a member called by `detectAndRepair()`, in the
  section file of its concern, over `SlipEpochWork`, and a line of the section table in
  `CycleSlipManager.hpp`.
- **A new slip detector**: the steps in `include\slip\SlipDetectorRegistry.hpp`
  (implement `ISlipDetector` with a new `SlipDetectorKind`, a creator in
  `SlipDetectorRegistry.cpp`, a toggle and the selection list of `createEnabled()`, the
  `.cpp` in `CMakeLists.txt`). Whether a declared detector table replaces the interface is
  an open decision (card UOM, section 7). A step of an existing detector goes into its
  driver: the MW steps are listed in `MwDetectorWork.hpp`, the TurboEdit steps in the
  step table of `TurboEditResolver.hpp`.
- **A new PPP epoch step**: a private stage of `Application`, declared in
  `Application.hpp` in pipeline order, defined in the `PppEpoch*.cpp` file of its concern
  and called in the spine of `runPpp()`. A value it hands to later stages is a member of
  `PppEpochWork`; a value that must survive to the next epoch is an `Application` member.
- **A new setup component**: a build or wire stage in `PppSetup.cpp`, called in
  `runPpp()`'s setup list.
- **A new run-end readout**: a report member in `PppRunEnd.cpp`, declared in
  `Application.hpp`, and one row of `kPppRunEndReports`.
- **A new baseline step**: the stage of its level in `ApplicationBaseline.cpp` (run,
  rover or epoch), its values in that level's struct.
- **Logic every driver needs in the update loop**: a free function in
  `EpochUpdateSupport.cpp`, called by each driver, so the copies cannot drift.
- **A real-time step**: the matching `RealTimeEngine*.cpp` seam (table above).

## Reading list

The **headers** (`include\…`) carry the documentation and are read before the `.cpp`
files, in this order:

| # | Header | Content |
| --- | --- | --- |
| 1 | `app\Application.hpp` | The driver; its class comment lists the PPP stages by file, and every stage is declared in pipeline order |
| 2 | `app\PppPipelineWork.hpp` | The values one epoch's stages share, grouped by the stage that writes them |
| 3 | `common\PppObservation.hpp` | The per-satellite record that passes through the whole epoch |
| 4 | `common\PppState.hpp` | The receiver state |
| 5 | `model\PppModel.hpp` | Observation modelling and corrections |
| 6 | `model\StochasticModel.hpp` | Observation weighting; small, and the subject of the worked example |
| 7 | `filter\StateManager.hpp`, `filter\StateFamily.hpp` | The state-vector layout and the family table |
| 8 | `filter\StateKeySchema.hpp` | The format of every state key |
| 9 | `measurement\MeasurementModel.hpp` | Construction of H, z, R; the state pre-pass table |
| 10 | `filter\KalmanFilter.hpp`, `filter\KalmanPostFitQc.hpp` | Predict and update; the post-fit screen's stages and the pivot detectors |
| 11 | `slip\CycleSlipManager.hpp`, `slip\CycleSlipVerdict.hpp` | Post-processing cycle-slip detection and repair; the per-satellite verdict |
| 12 | `app\EpochUpdateSupport.hpp` | The update-loop helpers the drivers share |
| 13 | `filter\AmbiguityResolver.hpp` | Integer ambiguity resolution |
| 14 | `realtime\RealTimeEngine.hpp` | The real-time epoch |

Some single-receiver classes also carry code only the network producer reaches:
`MeasurementModelNetwork.cpp` and the network-only surfaces of `MeasurementModel.hpp` and
`MwSlipDetector.hpp` (each behind a banner), the four network families of `StateManager`
(named in `StateFamily.hpp`), and `IfPhaseSlipDetector` (its header note).

The **math** behind each header is in the LaTeX reference, `docs\report\main.pdf`.
Each header is best read with its chapter: the observation equations with `PppModel`,
the Kalman chapter with `KalmanFilter`, and so on.

## What to skip

Everything that belongs to the network producer is out of scope until a task requires
it: `include\network\`, `src\core\network\` and `tests\network\` (the network producer),
`config\archive\`, `config\golden\`, `config\test_suite\`,
`docs\refound\`, `docs\cards\`, `docs\archive\`, and most of `scripts\`. That is about
two thirds of the repository. It is a careful research record, not clutter, but it is
not the starting point. When a task requires it, [7 · Network tour](07_network_tour.md)
reads one network run from its tables and walks its eight stages.

Next: [3 · Worked example](03_worked_example.md).
