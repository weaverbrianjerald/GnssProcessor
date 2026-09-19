# GnssProcessor — technical overview

> This is the long-form description of the engine: architecture, capabilities, build
> targets, configuration sections, data layout. It was the repository README until
> 2026-09-19, when the root [README](../README.md) became a short entry page.
> New contributors: start at [onboarding/README.md](onboarding/README.md) instead.

A **C++17 GNSS post-processing and real-time engine** for high-precision positioning. It implements Precise Point Positioning (PPP),
uncombined **PPP-RTK**, double-differenced baseline processing, and a minimum-constraint **S-system network producer/consumer** — with
integer ambiguity resolution (MLAMBDA), optional IMU tight coupling, and a backward RTS smoother. It runs equally as a batch
post-processor over RINEX + IGS products or as a live engine over NTRIP / RTCM3 / Septentrio SBF streams.

> **Status:** actively developed research & engineering codebase. `main` is the integration branch. A GoogleTest suite of about 2,080 cases
> across ~280 suites guards the numerics (`gnss_tests.exe --gtest_list_tests` gives the live count).

[Highlights](#highlights) · [Processing modes](#processing-modes) · [Architecture](#architecture) · [Quick start](#quick-start) ·
[Building](#building) · [Configuration](#configuration) · [Data & products](#data--products) ·
[Repository layout](#repository-layout) · [Capabilities in depth](#capabilities-in-depth) ·
[Real-time streaming](#real-time-streaming) · [PPP-RTK network mode](#ppp-rtk-network-mode) · [Testing](#testing) ·
[Tooling & Python](#tooling--python) · [Documentation](#documentation) · [License](#license)

## Highlights

- **Uncombined and ionosphere-free** measurement models over a variable-dimension Extended Kalman Filter; `StateManager` owns the
  layout dynamically (position, clocks, ZWD, per-satellite ionosphere, ambiguities, code/phase biases, system-time offsets).
- **Integer ambiguity resolution** via LAMBDA / MLAMBDA — float, continuous, instantaneous and fix-and-hold modes, WL-only
  (MW-resolved widelane) fixing, and FFR acceptance.
- **Multi-GNSS**: GPS, Galileo, BeiDou, GLONASS (per-FCN code inter-frequency bias).
- **Full physical modeling**: troposphere, ionosphere (incl. higher-order), ANTEX PCO/PCV, site displacement, relativity, phase
  wind-up, attitude/eclipse — detailed below.
- **Real-time engine** over NTRIP v2 (HTTP/HTTPS/TLS), raw TCP, or file replay — RTCM3 MSM4/5/7, broadcast ephemeris, station
  coordinates, IGS-SSR orbit/clock/bias corrections, plus native Septentrio SBF.
- **PPP-RTK network producer**: a joint multi-station estimator that solves satellite clocks and code/phase biases on an
  arrowhead-Schur normal-equation kernel under an explicit S-system datum, emits an IGS-SSR-convention product stream, and closes
  the loop through a held-out consumer station.
- **Backward RTS smoother**, IMU/INS tight coupling, and a pluggable cycle-slip detection/repair stack (Melbourne-Wübbena,
  geometry-free, GRAPHIC, EWL, TurboEdit).

## Processing modes

Two orthogonal config axes: `app_mode` (post-processing vs. real-time) and, within post-processing, `processing.mode`.

| `processing.mode` | What it does |
| --- | --- |
| `PPP` | Single receiver, ionosphere-free, external precise orbit/clock products. |
| `PPP-RTK` | Single receiver, **uncombined**, between-satellite single-difference, phase clock + per-satellite ionosphere, optional AR. |
| `Baseline-Single` | Double-differenced single baseline (one rover + one reference station). |
| `Baseline-Multi` | Double-differenced star topology (N rovers against one master reference). |
| `ppp-rtk-network` | **Network producer** — minimum-constraint S-system joint multi-station estimator emitting satellite clock + code/phase bias (+ atmosphere). |
| `ppp-rtk-user` | **Network consumer** — uncombined PPP-RTK that ingests the network SSR products and cross-checks the S-basis. |

`app_mode: "real_time"` runs the streaming `RealTimeEngine` instead, selected further by `realtime.input_type`
(`file` \| `ntrip` \| `tcp`) and `realtime.input_format` (`rtcm` \| `sbf`).

## Architecture

A single `Application` driver, configured by one JSON file, dispatches to one of four run methods. Every input path — batch
files/products or live streams — is normalized through the ingest layer before feeding the same **per-epoch estimation core**; the
PPP-RTK network producer runs as a parallel path emitting an IGS-SSR product stream, which the `ppp-rtk-user` consumer reads back in
(the dashed edge). Colors group the layers: **config**, **ingest**, **estimation core**, **network**, **outputs**.

```mermaid
flowchart TB
    CFG["GnssConfig — config/*.json<br/>app_mode + processing.mode"]
    subgraph IN["Inputs"]
      direction LR
      subgraph FILES["Files and products — post-processing"]
        RNX["RINEX obs / nav"]
        PRD["SP3 · CLK · BIA · ERP<br/>ATX · IONEX · ocean-load"]
      end
      subgraph STREAM["Streams — real-time"]
        NTRIP["NTRIP / TCP / file"]
        RTCM["RTCM3 MSM4/5/7 · SSR · nav"]
        SBF["Septentrio SBF"]
      end
    end
    subgraph INGEST["Resolve and ingest"]
      PR["PathResolver → ProductResolver<br/>DataDownloader — CDDIS"]
      PARSE["RinexParser · MultiRinexSync<br/>ProductParser · RinexNavParser"]
      RTE["RealTimeEngine<br/>RtcmParser · SbfParser<br/>SsrCorrectionStore · LiveEphemerisStore"]
    end
    APP{{"Application driver<br/>run · runBaseline · runNetwork · runRealTime"}}
    subgraph CORE["Per-epoch estimation core"]
      MM["MeasurementModel / Builder"]
      DIFF["Differencing — BSSD (PPP-RTK) · DD (baseline)<br/>PivotManager · DdPivotManager"]
      MODEL["PppModel + GnssCorrections<br/>tropo · iono · tides · antenna · relativity · windup"]
      STO["StochasticModel — weighting"]
      KF["KalmanFilter predict/update<br/>over StateManager"]
      AR["AmbiguityResolver — MLAMBDA"]
      SLIP["CycleSlipManager<br/>MW · GF · GRAPHIC · EWL · TurboEdit"]
      AID["ImuIntegrator · AidingSourceManager"]
      SM["Smoother — RTS, post-only"]
      MM --> DIFF --> MODEL --> STO --> KF --> AR
      SLIP --> MM
      AID --> KF
      KF --> SM
    end
    subgraph NET["PPP-RTK network producer"]
      NE["NetworkEstimator<br/>NetworkNormalEquations — arrowhead Schur<br/>NetworkSbasis — S-system datum"]
      EMIT["NetworkProductEmitter → NetworkProductIO<br/>IGS-SSR product stream"]
      NE --> EMIT
    end
    subgraph OUT["Outputs"]
      SOL["Position / state solution"]
      LOG["LogManager · TraceLogger<br/>CSV traces + spdlog"]
      VIZ["pyviz toolkit · Python module"]
    end
    CFG --> APP
    FILES --> PR --> PARSE --> APP
    STREAM --> RTE --> APP
    APP --> MM
    APP --> NE
    KF --> SOL
    NE --> SOL
    EMIT -. consumed by ppp-rtk-user .-> PARSE
    KF --> LOG
    NE --> LOG
    LOG --> VIZ
    SOL --> VIZ
    classDef cfg fill:#fef3c7,stroke:#d97706,color:#111;
    classDef ingest fill:#dbeafe,stroke:#2563eb,color:#111;
    classDef core fill:#dcfce7,stroke:#16a34a,color:#111;
    classDef net fill:#f3e8ff,stroke:#9333ea,color:#111;
    classDef out fill:#ffe4e6,stroke:#e11d48,color:#111;
    class CFG cfg;
    class PR,PARSE,RTE ingest;
    class MM,DIFF,MODEL,STO,KF,AR,SLIP,AID,SM core;
    class NE,EMIT net;
    class SOL,LOG,VIZ out;
```

## Quick start

```powershell
scripts\vs2026_build.ps1 -Config x64-Release          # every data run uses Release (see "Building")
python scripts/bootstrap_internal_data.py             # populate bundled fixtures once per clone
build\x64-Release\gnss_processor.exe config/ppp_rtk_internal.json
```

> **Never run `gnss_processor` from the Debug tree.** Debug's unoptimized Eigen is 50–100× slower — a Release run of minutes becomes
> hours. Debug is for compile checks and `gnss_tests` only. Run with no argument and it defaults to `config/ppp_rtk_internal.json`;
> every file in [`config/`](../config/) is a complete, standalone scenario — there is no config layering.

## Building

**Toolchain:** MSVC (Visual Studio **2026** Enterprise, v18) + CMake + Ninja, targeting C++17 — MSVC 19.51.36256 / toolset
14.51.36231. VS 2022 is retired. The C language is also required: the vendored IAU SOFA library (`third_party/sofa/`) is ANSI C,
built as a separate warning-suppressed static lib. **Dependencies** via [vcpkg](https://github.com/microsoft/vcpkg): Eigen3, spdlog,
pybind11, GoogleTest, netCDF, libcurl, zlib, and OpenMP (optional; enables Eigen dense-kernel threading). Python 3.13 drives the
pybind11 module.

```powershell
scripts\vs2026_build.ps1 -Config x64-Debug -Target gnss_core   # fast compile check
scripts\vs2026_build.ps1 -Config x64-Debug                     # full Debug + tests
scripts\vs2026_build.ps1 -Config x64-Release                   # every data run
scripts\vs2026_build.ps1 -Config x64-Debug -Reconfigure        # after a toolchain change
```

Three traps the wrapper handles that a bare `cmake --build` does not: the default shell does not source `vcvars64.bat` (naive builds
fail with `Cannot open include file: 'memory'`, and the file moved to `…\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\` —
`18` is the install *major-version* dir, not a year); **two CMakes are on PATH** and vcvars does not override the standalone 3.18.3,
so the wrapper pins VS 2026's bundled 4.3.1 by absolute path; and `set "PATH=…;%PATH%"` inside a `cmd /c "vcvars && …"` chain
silently destroys the compiler entries, because `cmd` expands `%PATH%` before vcvars runs. Build trees `build/x64-Debug` and
`build/x64-Release` are git-ignored and disposable. Full migration notes: [`CLAUDE.md`](../CLAUDE.md).

**Targets** (explicit source lists in [`CMakeLists.txt`](../CMakeLists.txt) — no globbing except vendored SOFA; add `--target gnss_core`
for fast library-only iteration; `--target docs` builds Doxygen when Doxygen and, optionally, Graphviz `dot` are on `PATH`):

| Target | Kind | Purpose |
| --- | --- | --- |
| `gnss_core` | static lib | The entire engine. |
| `gnss_processor` | executable | Main driver (`gnss_processor.exe <config.json>`). |
| `gnss_tests` | executable | GoogleTest unit suite. |
| `gnss_processor_module` | `.pyd` | pybind11 Python module (copied to `python/gnss_processor.pyd`). |
| `gnss_simulator` | executable | Multi-GNSS RINEX scenario simulator + truth verifier. |
| `gnss_rtcm_diag` | executable | RTCM3 capture inspector (message histogram, coords, SSR, MSM7). |
| `gnss_sbf_diag` | executable | Septentrio SBF block/observation inspector. |
| `gnss_meas3_xcheck` | executable | SBF Meas3Ranges vs. MeasEpoch cross-check. |
| `gnss_atx_diag` | executable | ANTEX PCO/PCV coverage inspector. |

## Configuration

One JSON file drives a run. Top-level scalars `app_mode` and `data_source` select the execution axis and file provenance; the rest is
grouped into sections:

| Section | Governs |
| --- | --- |
| `processing` | Global loop knobs: `mode`, `active_systems`, `elevation_mask_deg`, `signal_priorities` (per-constellation tracking-code priorities — **required** to make GLONASS/BeiDou non-inert), interpolation, threading. |
| `estimation` | State-vector toggles: velocity, per-satellite code-bias estimation, between-satellite SD + sticky-pivot elevation, `ambiguity_resolution` (MLAMBDA), adaptive iono process noise. |
| `filter` | Kalman tuning: `process_noise` (spectral densities), `initial_std` (a-priori **standard deviations**, not variances), `measurement_noise`, `stochastic_model`. |
| `model_corrections` | Per-effect `enable_*` physical-model switches (troposphere, tides, PCO/PCV, relativity, phase wind-up, higher-order iono, …). |
| `baseline` | DD-mode knobs: `use_iono_free`, `estimate_differential_tropo`, `enable_pivot_handover`, `signal_aliasing`, plus the `baseline_fixture` preset selector. |
| `network` | Producer/consumer: `clock_datum_station`, held-fixed `stations[]`, `product_output_path`, and the `closed_loop` consumer block. |
| `realtime` | Streaming input: `input_type`/`input_format`, NTRIP/TCP connection, and independent `corrections` (SSR) and `ephemeris` streams. |
| `data_download` | Production-mode fetch settings (CDDIS source/type, year/doy/station) and ATX auto-refresh. |
| `logging` | Per-channel spdlog + CSV-trace `enable_*` flags and per-logger severity. |

Shipped configs use `//` line comments liberally to document and stash alternative values. See [`docs/CONFIG.md`](CONFIG.md) for
the "which knob does what" narrative and the field-level Doxygen in [`include/config/GnssConfig.hpp`](../include/config/GnssConfig.hpp)
for the exhaustive reference. **Selected scenario presets** in [`config/`](../config/):

| File | Scenario |
| --- | --- |
| `ppp_internal.json` / `ppp_prod.json` | PPP (bundled fixtures / production download). |
| `ppp_rtk_internal.json` / `ppp_rtk_internal_AR.json` | Single-station PPP-RTK (float / with ambiguity resolution). |
| `baseline_single.json` / `baseline_multi.json` | Double-differenced baseline. |
| `ppp_rtk_network_internal.json` (+ codebias / glonass / global variants) | Network producer + closed-loop consumer. |
| `realtime_ppp_rtk.json` / `realtime_igs_ntrip.json` | Live PPP-RTK over NTRIP (IGS-RTS SSR + broadcast nav). |
| `sbf_replay.json` | Septentrio SBF file replay. |

## Data & products

All input-file location goes through one class — `PathResolver` — driven by the top-level `data_source` field. Both modes share the
same **date-first on-disk layout**:

```
<root>/
  <YYYY-DOY>/                 e.g. 2024-129
    products/                 SP3  CLK  BIA  ERP  *_MN.rnx (nav)
    obs/<station>/            RINEX observations (Hatanaka-decompressed)
  static/                     atx/igs20.atx   nc/EOT20_load.nc
  cache/<YYYY-DOY>/           raw .gz / .crx intermediates (production only)
```

- **`internal`** (default) — turnkey, no network. `PathResolver` globs the bundled `data/` tree (a CMake-baked project-root path) and
  matches files by station and date. Used by the tests and every demo run.
- **`production`** — `PathResolver` delegates to `ProductResolver`, which builds IGS long-name URLs and downloads missing products
  from CDDIS (`.netrc` HTTP Basic auth), gunzips, and Hatanaka-decompresses (`crx2rnx`) as needed.

Parsed formats: RINEX 2/3 observations, RINEX 3/4 navigation, SP3 orbits, RINEX/CLK clocks, Bias-SINEX / OSB, ERP, IGS ANTEX,
IONEX/GIM, and NetCDF ocean-loading grids. `data/` is `.gitignore`d except for README shells — populate it once after cloning with
`python scripts/bootstrap_internal_data.py` (add `--no-eot20` to skip the 280 MB ocean-tide file).

## Repository layout

```
include/            Public headers, mirroring src/core/ (heavy Doxygen; always include with the subdir prefix, e.g. "filter/KalmanFilter.hpp").
  app/              Application driver (post / baseline / real-time / network entry points).
  atmosphere/       Troposphere (GMF/VMF3/GPT3), Ionosphere (Klobuchar/IONEX/SH/higher-order).
  common/           Cross-cutting types: constants, GNSS/PPP types, state, observations.
  config/           GnssConfig + enums + JSON loader helpers; primary config in src/core/config.
  corrections/      GnssCorrections, ocean-tide model.
  filter/           KalmanFilter, StateManager, Smoother (RTS), AmbiguityResolver (MLAMBDA), PivotManager / DdPivotManager, ImuIntegrator, NetworkEstimator + S-basis.
  measurement/      MeasurementModel/Builder, StateInitializer, BSSD/BRSD transforms.
  model/            PppModel (uncombined PPP-RTK), StochasticModel, Antenna, attitude, geometry.
  products/         Path/Product resolvers, DataDownloader, SSR store, network product I/O, live ephemeris store, RINEX nav parser.
  realtime/         NtripClient (TLS), TcpClient, RtcmStreamReader, RealTimeEngine.
  rinex/ rtcm/ sbf/ RINEX parsing + sync; RTCM3 (MSM/SSR/nav) decoders; Septentrio SBF.
  slip/             CycleSlipManager + MW/GF/GRAPHIC/EWL/TurboEdit detectors, LLI bridge.
  aiding/           IAidingSource abstraction (IMU, ZUPT) + registry/manager.
src/core/           Implementation, mirroring include/ one-to-one.
src/tools/ src/sim/ Standalone diagnostic tools and the scenario simulator.
src/bindings/       pybind11 module.
tests/              GoogleTest suite (~2,080 cases / ~280 suites).  tests/sim/ is a SEPARATE target, gnss_simulator_selftest (80 cases), not linked in.
scripts/            Python utilities (bootstrap, ATX refresh, layout migration, obs fetch) plus the vs2026_build.ps1 build wrapper.
pyviz/              Python visualization toolkit (traces, maps, reports).
third_party/sofa/   Vendored IAU SOFA (ANSI C) for the CIO frame transform.
data/               Local cache for internal-mode fixtures (git-ignored; see "Data & products").
docs/               Design notes, config reference, operational runbooks — start at docs/INDEX.md.  docs/attic/ holds retired records.
```

## Capabilities in depth

Detail behind the [Highlights](#highlights) bullets — the mechanisms, not a second list of features.

**Estimation core.** `StateManager` is the single source of truth for key→index layout, with optional IEKF geometry
relinearization. The backward RTS smoother is post-processing only and its segments break at ambiguity resets. Integer fixing is
LAMBDA / MLAMBDA integer least-squares (Teunissen 1995; Chang et al. 2005) with a ratio test, convergence guards, NL/WL basis,
WL-only mode and fixed-failure-rate acceptance. PPP-RTK uses a between-satellite single-difference with a sticky pivot + S-transform
handover; DD baselines use a dedicated DD pivot manager. GNSS/INS coupling is IMU strapdown ECEF mechanization with
accelerometer/gyro biases as filter states, behind an extensible aiding-source abstraction (IMU, ZUPT).

**Modeling.** Troposphere: hydrostatic + estimated wet ZWD, GMF / VMF3 / GPT3 mapping. Ionosphere: per-satellite slant states,
Klobuchar, IONEX/GIM, spherical-harmonic VTEC, and 2nd/3rd-order terms with an IGRF magnetic-field model. Antenna: satellite and
receiver PCO/PCV from IGS ANTEX with cross-system fallback. Site displacement: solid-earth tide, ocean-tide loading (EOT20), pole
tide, atmospheric loading; plus relativity (Shapiro + periodic), phase wind-up, and satellite attitude/eclipse.

**Quality control.** Innovation/residual outlier screening, RAIM-style network fault isolation, per-satellite clock monitoring, and
residual-history tracking.

## Real-time streaming

`RealTimeEngine` consumes up to three concurrent streams — a primary observation stream plus optional independent **corrections**
(SSR) and **ephemeris** streams — each configurable as file replay, NTRIP, or raw TCP. The **NTRIP v2 client** runs over HTTP or
HTTPS/TLS (libcurl) with GGA upload for VRS mountpoints and jittered reconnect. **RTCM3 decoding** covers MSM4/5/7 multi-GNSS
observations, broadcast ephemeris, reference-station coordinates (1005/1006/1033/1230), and IGS-SSR orbit / clock / code+phase-bias
corrections; **Septentrio SBF** covers MeasEpoch and Meas3Ranges with navigation subframe assembly. Observation↔SSR time-sync
buffering and a live ephemeris store keep the correction and observation clocks aligned.

## PPP-RTK network mode

The `ppp-rtk-network` producer estimates **shared satellite parameters** (clock, code bias, phase bias, and optionally atmosphere)
jointly across a set of held-fixed stations, on an **arrowhead / Schur** normal-equation kernel (`NetworkNormalEquations`) with a
semantic layer (`NetworkEstimator`) that gives each shared column a (satellite, parameter, signal) meaning. It is solved under an
explicit **minimum-constraint S-system datum** (`NetworkSbasis`) that pins exactly the gauge freedoms (overall clock,
per-constellation phase-bias↔ambiguity, ISB), then serialized as an IGS-SSR-convention product stream (`NetworkProductEmitter` /
`NetworkProductIO`) and consumed closed-loop by a held-out station routed through the user-side PPP-RTK + AR pipeline for A/B
validation.

## Testing

```powershell
scripts\vs2026_build.ps1 -Config x64-Debug -Target gnss_tests
build\x64-Debug\gnss_tests.exe          # run the exe directly; ctest only registers gnss_simulator_selftest
```

**About 2,080 GoogleTest cases across ~280 suites** (~90 s for a full Debug run) cover the math core, site displacement,
atmosphere/antenna models, the filter core, transforms/estimation, RTCM & SBF decoders, ILS/MLAMBDA, the state-key schema, the
network estimator + normal equations + invariant-carry driver + product I/O, cycle-slip/TurboEdit, the RTS smoother, aiding sources,
and real-time seams. One case is fixture-gated on `bootstrap_internal_data.py`. `tests/sim/` is a **separate** target,
`gnss_simulator_selftest` (80 cases), not linked into `gnss_tests`. The suite guards structure and non-regression; it does **not**
validate end-to-end estimation accuracy. [`docs/test_suite.md`](test_suite.md) documents the separate real-data truth-graded
harness, not this unit suite. (Counts re-verified 2026-08-12; the earlier "~550 cases across 33 files" figure quoted here is
superseded.)

> New behavior is developed default-OFF and byte-identical where possible, so the suite can assert non-regression against prior runs.

## Tooling & Python

- **Diagnostic executables** — `gnss_rtcm_diag`, `gnss_sbf_diag`, `gnss_meas3_xcheck`, `gnss_atx_diag` (see the target table above)
  validate captures and coverage before a live run.
- **Simulator** — `gnss_simulator` synthesizes multi-GNSS RINEX-3.04 observations with a truth answer key and verifies the processor
  recovers the injected truth.
- **Python module** — `gnss_processor_module` (pybind11) is copied to `python/gnss_processor.pyd` for scripted driving and
  post-analysis.
- **`pyviz/`** — a backend-agnostic visualization toolkit (state/ambiguity/residual traces, station maps, network viewers, HTML
  reports).
- **`scripts/`** — `bootstrap_internal_data.py` (fixtures), `refresh_atx.py` (ANTEX), `migrate_data_layout.py`
  (legacy→date-first), `fetch_global_obs.py`, and others.

## Documentation

**Start at [`docs/INDEX.md`](INDEX.md)** — the single entry point for the `docs/` tree ([`docs/README.md`](README.md) is the older generated index it superseded).
Entry points worth naming:

- [`CLAUDE.md`](../CLAUDE.md) — build/layout/conventions overview and the standing invariants.
- [`docs/CONFIG.md`](CONFIG.md) — configuration reference.
- [`docs/CONFIG_promotion_ledger.md`](CONFIG_promotion_ledger.md) — where knob promotion state actually lives (validated knobs
  stay default-OFF; the ledger is the record).
- [`docs/test_suite.md`](test_suite.md) — the real-data truth-graded harness.
- [`docs/rt_service_conventions.md`](rt_service_conventions.md) / [`docs/rt_service_operations.md`](rt_service_operations.md)
  — real-time service runbooks.
- [`docs/network_ambiguity_resolution_design.md`](network_ambiguity_resolution_design.md) and
  [`docs/SPEC_sbasis_realization_contract.md`](SPEC_sbasis_realization_contract.md) — representative subsystem deep-dives.
- Doxygen (`--target docs`) — API reference from the in-source comments.

## License

No `LICENSE` file is currently present; the code is under the exclusive copyright of the repository author (all rights reserved)
until one is added. Contact the author before redistributing or reusing.
