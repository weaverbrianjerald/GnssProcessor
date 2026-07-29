# GnssProcessor

C++17 GNSS post-processor and real-time engine. Supports PPP and PPP-RTK (uncombined), optional IMU integration, integer ambiguity resolution (MLAMBDA), and a backward RTS smoother. Primary config: `src/core/config/gnss_config.json`.

## Build

MSVC (Visual Studio 2022 Enterprise) + CMake + Ninja. The user's default shell does NOT source `vcvars64.bat`, so naive `cmake --build` fails with "Cannot open include file: 'memory'". Use this pattern via the PowerShell tool:

```powershell
$vcvars = "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vcvars`" >nul && cmake --build C:\GnssProcessor\build\x64-Debug 2>&1"
```

Targets: `gnss_core` (static lib), `gnss_processor` (exe), `gnss_tests` (exe), `gnss_processor_module` (pybind11 `.pyd`). Add `--target gnss_core` for library-only checks during iteration.

Build tree: `build/x64-Debug` and `build/x64-Release`.

**NEVER run `gnss_processor` on data from the Debug tree** (standing user
directive). Debug's unoptimized Eigen is 50–100× slower — a Release run of
minutes becomes hours. Debug is for compile checks and `gnss_tests` only;
every data run (smoke, field grade, A/B, timing) uses
`build/x64-Release/gnss_processor.exe`. When another session may rebuild
concurrently, run a renamed copy (e.g. `gnss_processor_fable_run.exe`) so the
linker never hits a locked exe.

## Layout

```
include/              All public headers, organized into subdirs mirroring
                      src/core/ — heavy Doxygen is the norm.  Always include
                      with the subdir prefix: `#include "filter/KalmanFilter.hpp"`
                      not `#include "KalmanFilter.hpp"`.  CMake adds `include/`
                      to the include path; the subdir is part of the include
                      string.
  app/                Application.hpp (post-processing + real-time entry points;
                      impls split across Application.cpp / ApplicationBaseline.cpp /
                      ApplicationRealtime.cpp)
  atmosphere/         Ionosphere, Troposphere
  common/             Truly cross-cutting types and identifiers only:
                      Constants, Namespaces, GnssTypes, PppTypes, PppState,
                      PppObservation, EpochMeasurement, PppUtils, SignalBand
  config/             GnssConfig + GnssConfigTypes (the config enums + POD
                      sub-structs, relocated out of the GnssConfig.hpp header;
                      GnssConfig.hpp includes it) + GnssConfigJsonHelpers
                      (loader split into per-section helpers in
                      src/core/config/GnssConfigLoad.cpp)
  corrections/        GnssCorrections, OceanTideModel
  filter/             KalmanFilter, StateManager, StateKeySchema, Smoother,
                      AmbiguityResolver, ImuIntegrator, PivotManager (PPP-RTK SD),
                      DdPivotManager (baseline DD)
  logging/            LogManager, TraceLogger, KalmanFilterLogger, WlsLogger
  math/               Coordinates, TimeUtils, Interpolate, WlsEstimator
  measurement/        MeasurementModel, StateInitializer, BssdTransform, BrsdTransform
                      (BSSD: PPP-RTK SD via BssdTransform::apply, baseline DD via
                       BssdTransform::differenceBrsd composed on BrsdTransform output)
  model/              PppModel (also hosts uncombined PPP-RTK, composed with
                      measurement/BssdTransform), StochasticModel, Antenna,
                      Planetary, ModelGeometry
  products/           ProductManager, ProductCache, ProductParser, PathResolver,
                      ProductResolver, ProductTypes, DataDownloader
  realtime/           NtripClient, RtcmStreamReader, RealTimeEngine
  rinex/              RinexParser, MultiRinexSync
  rtcm/               RtcmParser
  slip/               CycleSlipManager + detectors: MwSlipDetector,
                      GraphicSlipDetector (active); IfPhaseSlipDetector (active
                      on the NETWORK path — network.cycle_slip.if_phase_td;
                      STAGED for user/kinematic); GfSlipDetector (STAGED —
                      uncompiled, not wired in; see its header banner);
                      IonoVariabilityMonitor

src/core/             Implementation files mirror the include/ subdir layout
                      one-to-one (app/, common/, config/, logging/, math/,
                      products/, slip/, etc.).  src/core/config/ also holds
                      gnss_config.json — the primary user-edited config file.

tests/                GoogleTest unit suite (904 cases / 138 suites across 55
                      files) linked into `gnss_tests.exe`.  tests/sim/ is a
                      SEPARATE target, `gnss_simulator_selftest` (35 cases).
                      Note: docs/test_suite.md documents the real-data
                      truth-graded harness, NOT this unit suite.
logs/                 Runtime CSV traces + spdlog files (under build tree)

scripts/              One-off Python utilities.  Most notably
                      bootstrap_internal_data.py, which populates
                      data/ for internal-mode runs (see "File management"
                      below).
data/                 Local cache for internal-mode test fixtures.
                      Empty in git (.gitignored except for README.md
                      shells); populated by the bootstrap script.
                      Layout matches what ProductResolver creates in
                      production: products/{orbit,clock,dcb,erp,brdc,
                      rinex}/{zip,unzip}/, products/{atx,nc}/,
                      observations/{absolute,baseline/...}/.
```

Anything that genuinely belongs to *every* TU lives in `common/`; everything else
got pulled into a category dir.  When adding a new file, prefer the most
specific subdir that fits — resist the urge to drop new headers into `common/`.

## Conventions

**Config toggles.** New feature flags follow:
```cpp
bool enable_X_{false};                                         // in GnssConfig private members
bool enableX() const { return enable_X_; }                     // getter (doxygen expected)
enable_X_ = jval<bool>(json, "section.enable_x", "enable_x", false);  // JSON parse
```
JSON section is `processing` (loop-level toggles) or `estimation` (state-vector toggles). Also add to `isLoggerEnabled` and the summary toggle-log line if it's user-visible. `run_smoother` and `gnss_rate_s` are recent examples.

**CMake.** Explicit source list in `CMakeLists.txt` — no globbing. New `.cpp` files must be added manually or the linker will surface mysterious undefined-reference errors.

**Doxygen.** Public API in headers gets full Doxygen (`@brief`, `@param`, `@return`). The existing style is unusually thorough — match it, don't dilute it.

**State-vector discipline.** `StateManager` is the single source of truth for state layout and dynamic key→index maps (ambiguities, per-sat iono, code biases, sat biases, system time offsets). `KalmanFilter` operates on state owned by `StateManager`. Don't add sibling classes that own parallel state; extend `StateManager`.

## Standing invariants — pre-flight EVERY new estimation card against these

Mechanism-independent lessons paid for in field time. Check them at SPEC time
and again at the validation gate; "my mechanism is different" is not an
exemption (each was re-learned on a "different" mechanism).

1. **Any zero-q / stiff time-linked quantity MUST have a wired, tested
   slip/jump/event response** (erase, refloat, or re-tap — mechanism-specific)
   before an ON field run. A stiff prior carrying a stale value past a jump
   does not degrade gracefully — it warps the whole solution (iono/clock
   absorb the tension). Instances: phase-leveling zero-q carry (needs slip
   detector), Card-4 invariant DD carry (slip hole → 10.9 cyc/ep field FAIL,
   Jul 2026).
2. **Field runs are Release-only** (see Build). Debug perf numbers are
   artifacts; never profile or grade in Debug.
3. **Every deferral in a card is triaged field-blocking vs field-inert, with
   DATA for "inert"** (e.g. I2 measured 0 gap events ⇒ prediction-only chords
   deferrable). A field-blocking deferral means the toggle is NOT
   field-runnable and the report must say so.
4. **Run-end counters are the ON-path telemetry.** "0 driven / 0 folded" means
   the wiring is inert no matter how clean the log looks. Check counters, not
   absence of crashes.
5. **Grade only datum-invariant / SD quantities** (Psychas single-realization
   mandate); raw wire steps confound benign frame drift with real defects.
6. **New work is default-OFF with structural byte-identity**; shared-path
   edits require same-build golden comparison (verification integers are
   build-config-sensitive at the ppm level — never compare Debug vs Release).

## Standard armed set — every NETWORK run uses these (directive, Jul-28)

"Unguarded" arms are over (the diagnostic cards that needed them are closed).
Loader DEFAULTS stay off (flipping them invalidates stored-snapshot
fingerprints — a migration card, not a quick edit); the rule is that every new
network CONFIG derives from a base that arms this set explicitly
(`scripts/p2_make_guard_configs.py` is the current template):

- **QC, fingerprint-SAFE** (legal to arm even on a warm resume): `raim`,
  `raim_attribution`, `raim_within_epoch`, `raim_withhold_sigma0: 6.0` (A4),
  `clock_runaway_check`, `sh_health_gate`.  `raim_we_code_iono` (WE-CI — the
  code+iono within-epoch pools) joins this list once its first ON validation
  passes.  Known gap: the within-epoch tier is `carry_warm`-gated, so it is
  inert without phase_leveling (decouple is a sanctioned open fix).
- **Fingerprinted — fresh convergence only**: `phase_leveling`, `emit_ura`,
  `code_bias_disc_jump_test` (+ explicit `cycle_slip.mw_detect` with
  leveling).  `cb_excursion_reinit` is conditional-fingerprinted: seed and
  legs must ALL carry it; never flip it mid-chain.
- **Cold-start hygiene**: `cold_clock_seed` (the 256 km x_lin cold-start
  leak fix — mandatory for MW-WL work), half-epoch back-off on every
  `start_offset_hours` (bare hour values sit on the double-residue boundary),
  and SEAMLESS (continuous) lineages for event studies — a 19:00-style
  mid-lineage warm-resume seam measurably erases the events under guards.
- **Observability**: `write_run_manifest`, `trace_epoch_status`,
  `trace_events`, `trace_flag_rates`, `health_interval_s: 300`,
  `emit_epoch_status_record`, `trace_postfit_sigma0`, `trace_postfit_keyed`.
- **Parallel**: `parallel_carry: true` / `parallel_reduce: false` (mutex).

Grading note: the response doctrine is adapt-and-EMIT within the detected
epoch; a withhold is the backstop and is graded as an adaptation failure
unless the epoch is provably unrecoverable.  (The SH gate currently has no
release/degrade policy — a sick-field era withholds the wire for its full
duration, 95+ min measured on DOY-129; open policy item.)

## File management (RINEX, products, downloads)

Input-file location goes through one class — `PathResolver` — driven
by a single top-level config field, `data_source`.  Both modes share
the same on-disk layout (date-first); they differ only in how files
get there (bundled fixtures vs automatic downloads).

**Unified layout** under any data root:
```
<root>/
  <YYYY-DOY>/                      e.g. "2024-129"
    products/                      ready *.SP3 *.CLK *.BIA *.ERP *_MN.rnx
    obs/<station>/                 ready *.rnx (Hatanaka converted via crx2rnx)
  static/
    atx/igs20.atx
    nc/EOT20_load.nc
  cache/<YYYY-DOY>/                raw .gz / .crx (production-mode only)
```

- **`data_source: "internal"`** (default).  PathResolver globs the
  bundled `data/` tree at `getInternalDataRoot()` (a CMake-baked
  absolute path; defaults to `<project_root>/data`, overridable via
  `files.internal_data_root` in JSON).  No network access.  Forces
  `data_download.enabled = false` regardless of what the user wrote.
  Suitable for unit tests, demos, and turnkey runs against the
  bundled fixtures.

- **`data_source: "production"`**.  PathResolver delegates to
  `ProductResolver`, which builds IGS long-name URLs from
  `data_download.{year, doy, station, product_source, product_type}`
  and downloads any missing products from CDDIS into
  `data_download.data_root`.  Same layout — a directory populated by
  internal-mode bootstrap is a perfectly valid production cache and
  vice versa.

**Date selection (internal mode).**  PathResolver picks a date from,
in order: (1) the YYYYDOY embedded in `files.observation_path` /
`baseline.{reference,rovers}.rinex_path` if pinned, (2)
`data_download.{year, doy}` (re-used as a generic date hint even
under `data_source = internal`), (3) the earliest `<YYYY-DOY>/`
subdir under `<root>` (lex sort).  Step (3) makes a single-fixture
data root work without any config knobs at all.

**Migrating from the old flat layout.**  For pre-refactor data trees
(`<root>/products/{orbit,clock,...}/{zip,unzip}/` + `observations/`):
`python scripts/migrate_data_layout.py <root>` relocates in-place
(`--dry-run` previews; idempotent; preserves user-managed dirs outside
the canonical scan paths).

**CDDIS authentication.** Production-mode downloads use HTTP Basic auth
via `.netrc`. `DataDownloader` resolves the file at startup in this
order: `data_download.netrc_path` (if set) → `~/.netrc` → `~/_netrc`
(Windows convention). If none is found a warning is logged at startup;
HTTP 401 / 403 responses include the resolved-path and a hint pointing
at the CDDIS netrc-creation guide.

**crx2rnx (Hatanaka decompression).** Python data scripts decompress
via the **`hatanaka`** library (pinned in `requirements.txt`; handles
RINEX 2/3/**4** — standalone crx2rnx 4.0.x rejects RINEX 4), with the
external exe as legacy fallback. The C++ production path shells out to
RNXCMP `crx2rnx` (output captured; failures logged with exit code);
exe resolution: `data_download.crx2rnx_path` → well-known dirs (incl.
`C:/GnssProcessorSupport/exe/crx2rnx.exe`, RNXCMP 4.1.0) → PATH. A
successful `.crx.gz` download whose conversion fails is a hard error,
never masked by a `.rnx.gz` fallback.

Either mode produces a `PathResolver::ResolvedPaths` struct that the
`Application` driver merges into `GnssConfig` via the
`setObservationPath` / `setNavigationPath` / `setProductPaths` /
`setBaselineReferenceRinexPath` / `setBaselineRoverRinexPath`
setters.  Path resolution runs once per process via
`Application::resolveData()` — for PPP / PPP-RTK from `run()`, for
baseline modes from `runBaseline()` (which dispatches before the
generic resolveData call site).

**Path precedence within a mode.** Explicit paths in the JSON config
always win over auto-discovery.  An empty string in
`files.observation_path`, `files.navigation_path`,
`baseline.reference.rinex_path`, or any rover's `rinex_path` is the
signal that PathResolver should fill it in.  This lets a config
override one specific input while letting everything else
auto-resolve.

**Internal-mode bootstrap.** `data/` is `.gitignore`d except for
README shells.  Populate it once after a fresh clone:
```
python scripts/bootstrap_internal_data.py            # full set
python scripts/bootstrap_internal_data.py --no-eot20 # skip the 280 MB ocean tide file
python scripts/bootstrap_internal_data.py --crx2rnx C:/path/to/crx2rnx.exe
```
The script writes the unified date-first layout: products land in
`data/<YYYY-DOY>/products/`, observations in
`data/<YYYY-DOY>/obs/<station>/`, raw `.gz` / `.crx` intermediates in
`data/cache/<YYYY-DOY>/`, and `igs20.atx` / `EOT20_load.nc` under
`data/static/`.  PathResolver picks files up by station-name match
within the date subtree.  Local research data (e.g. the OSU baseline
fixtures) is not on a public archive and the script prints a "please
supply manually" message at the end with the destination paths.

**Baseline fixture presets.** The top-level config field
`baseline_fixture` selects a named bundle of (reference station,
reference ECEF, rover station, year/DOY) from
`include/config/BaselineFixtures.hpp`:

- `IGS_Pasadena` (default) — Caltech (CIT100USA) vs JPL (JPLM00USA),
  ~10 km, 2024-DOY129.  Both stations on CDDIS daily archive;
  bootstrap fetches them automatically.
- `OSU_Campus` — Local OSU survey (NARY ref + 233 rover), ~340 m,
  2023-DOY321.  Not on any public archive; user supplies the obs
  files manually under `data/2023-321/obs/<station>/`.
- `Custom` — disable presets entirely; every baseline field must be
  set explicitly in the JSON config.

The preset only fills fields the user left blank in the
`baseline.{reference,rover}` JSON block — explicit values always
win.  The preset's year/doy also seeds `data_download.{year,doy}`
when the user hasn't typed their own.  Adding a preset:
register it in `BaselineFixtures.hpp`, then add matching
`FIXTURE_OBSERVATIONS` entries in the bootstrap script.

**`.gz` cleanup.**  `DataDownloader::downloadAndDecompress` removes
the `.gz` after a successful gunzip by default, halving the cache
footprint.  Set `data_download.keep_zip_after_unzip = true` in the
JSON to retain the compressed copy (for byte-exact archival).

## Known state

- **Test suite.** `gnss_tests.exe` runs 904 GoogleTest cases across 138 suites in 55 files (~76 s, full Debug run; one fixture-gated skip — `FixtureSmokeTest.Sp3HeaderTimeConversionMatches`, which needs `bootstrap_internal_data.py` to have run) — math core, site displacement, atmosphere models, antenna/ATX, filter core, transforms/estimation, RTCM/SBF decoders, ILS/MLAMBDA, state-key schema, the network estimator + normal equations + invariant-carry driver + product I/O, cycle-slip/TurboEdit, the RTS smoother, aiding, and real-time seams. `tests/sim/` is a **separate** target, `gnss_simulator_selftest` (35 cases), not linked into `gnss_tests`. The suite guards structure and non-regression (new work is developed default-OFF / byte-identical). It does NOT validate end-to-end estimation accuracy — confidence for numerical changes still comes from a clean build + field runs graded on datum-invariant quantities (see Standing invariants) + trace inspection.
  - Suite hygiene: every suite name is owned by exactly one file (138 suites, zero cross-file overlap) — keep it that way, and prefer a new file over widening an existing one. At 29.5k test LOC against 136.9k production LOC the suite is *thin* (0.22 ratio, ~1 case per 151 production lines); the gap is coverage, not bloat. There is no dedicated doc for this suite — `docs/test_suite.md` is the real-data harness.
- **Pre-existing C4828 warnings** (illegal characters at codepage 65001) are from older mixed-encoding files and unrelated to new work. Ignore unless the user asks to clean them up.
- **`vswhere.exe not recognized`** messages from cmd subshells are benign noise when using the vcvars64 pattern above — the build proceeds.
- **Real-time mode** uses a separate path (`Application::runRealTime` → `RealTimeEngine`). Post-processing-only features (e.g. smoother, `gnss_rate_s`) live in `Application::run` and should guard against `AppMode::RealTime`.

## Entry points worth knowing

- `src/core/app/Application.cpp` — `run()` is the post-processing pipeline, `runRealTime()` the streaming one. The per-epoch loop is the place to wire in new predict/update-phase logic. Mode-specific impls in `ApplicationBaseline.cpp` and `ApplicationRealtime.cpp`.
- `src/core/config/GnssConfigLoad.cpp` — `loadFromJson()` is a thin orchestrator; each top-level JSON section has its own `loadXxxSection_` private helper. New toggles slot into the matching section helper. Shared JSON-reading primitives (`jval`, `jsection`, `resolvePath`) live in `include/config/GnssConfigJsonHelpers.hpp`.
- `include/products/PathResolver.hpp` — single mode-aware locator for every input file. Internal mode globs `data/`; production mode delegates to `ProductResolver`. New file types (e.g. an SSR stream cache) plug in here, not into the call sites in `Application` / `ApplicationBaseline`.
- `include/filter/KalmanFilter.hpp` — predict / update / IMU-aided predict. Variable-dimension state is handled via `StateManager` key maps, not via fixed indices.
- `include/filter/StateKeySchema.hpp` — single source of truth for every dynamic state-vector key format (PPP ambiguity, DD ambiguity, per-sat iono, receiver code bias, sat bias, system time offset, BSSD pivot seed). All key construction and parsing should go through its `make*` / `parse*` helpers.
- `include/filter/Smoother.hpp` — RTS smoother operates on float ambiguities; segment boundaries at ambiguity resets. Smoothed position is degenerate (constant across epochs) in static PPP by construction — that's expected.

### Network producer — where new code goes (anti-regrowth decision-tree)

`src/core/app/ApplicationNetwork.cpp` (`runNetwork`) was decomposed (INC-0..12) from ~14.7k lines to ~1.6k lines of thin epoch-loop orchestration; the per-epoch body is a sequence of `subBegin; networkX_(...); subEnd;` calls into single-concern seams. **Keep it thin** — new network-producer code goes in the seam matching its concern, NOT inline in the driver:

- **New per-epoch estimation/QC step** → the matching `filter/Network*` seam (or a new one) called from the loop. Datum/solve → `NetworkSbasis` / `NetworkChainDatum` / `NetworkPhaseDatum` / `NetworkSolveGlue` / `NetworkDdPriorBuild` / `NetworkInstallCarry` / `NetworkProducerAr`; post-fit QC → `NetworkRaim` / `NetworkClockQc` / `NetworkRelevel`; slip → `slip/NetworkSlipQc`; coupling → `NetworkCoupling`.
- **New emit/store behavior** → `NetworkSolPrep` (pre-emit prep) or `NetworkEmitStore` (emit / product-write / closed-loop consume).
- **New diagnostic/trace sweep** → `NetworkIonoDiag` / `NetworkDdFrac` / `NetworkShDevVtec` / `NetworkShHealth` / `NetworkLeveling` / `NetworkStashStatus`, or a new diag seam. Trace-stream ownership + run-summary/manifest writers live in `logging/NetworkRunTraces` (NOT the driver).
- **New run-end telemetry / readout** → `filter/NetworkTeardown` (the post-loop run-end tail).
- **New carried/persistent state, counters, or config snapshots** → `app/NetworkRunContext` (`NetworkCarryState` / `NetworkServiceCounters` / `NetworkKnobs`), aliased in the driver and passed to the seams — do NOT add a bare function-local (that is precisely what INC-12 spent a pass deleting: 191 dead aliases left behind by relocated blocks).
- **New run-loop-local helper type a seam must name** → `filter/NetworkRunTypes` (general) or `filter/NetworkWlRunTypes` (WL / section-timing).
- **Setup** (validation / sinks / consumer) → `app/ApplicationNetworkSetup`; **assembly** (PASS A/B) → `app/ApplicationNetworkAssembly`.

Seam contract: a free function over `(NetworkRunContext& ctx, const GnssConfig& config_, log, NetworkRunTraces&, …)` opening with an alias preamble that reconstructs the driver's `ctx.*` / `setup.*` aliases so the body reads the original names; it owns NO state (every carry object is a reference into `ctx`). The driver keeps only the `subBegin/subEnd` brackets and the call. New `.cpp` seams need a manual `CMakeLists.txt` entry (no globbing).
