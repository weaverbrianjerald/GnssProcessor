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
                      GraphicSlipDetector (active); IfPhaseSlipDetector,
                      GfSlipDetector (STAGED — uncompiled, not wired in; see
                      their header banners); IonoVariabilityMonitor

src/core/             Implementation files mirror the include/ subdir layout
                      one-to-one (app/, common/, config/, logging/, math/,
                      products/, slip/, etc.).  src/core/config/ also holds
                      gnss_config.json — the primary user-edited config file.

tests/                GoogleTest unit suite (~550 cases across 33 files) linked into
                      `gnss_tests.exe`; see docs/test_suite.md
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

**Migrating from the old flat layout.**  Pre-refactor data trees had
`<root>/products/{orbit,clock,dcb,erp,brdc,rinex}/{zip,unzip}/` plus
`products/{atx,nc}/` and `observations/`.  Run
`python scripts/migrate_data_layout.py <root>` to relocate everything
in-place (`--dry-run` previews).  Idempotent and safe on the bundled
`data/`, an ad-hoc project-tree production cache, or a user-owned
root like `C:/GnssProcessorSupport/products/`.  User-managed dirs
outside the canonical scan paths (e.g. a hand-curated
`data/rinex/debug/`) are preserved.

**CDDIS authentication.** Production-mode downloads use HTTP Basic auth
via `.netrc`. `DataDownloader` resolves the file at startup in this
order: `data_download.netrc_path` (if set) → `~/.netrc` → `~/_netrc`
(Windows convention). If none is found a warning is logged at startup;
HTTP 401 / 403 responses include the resolved-path and a hint pointing
at the CDDIS netrc-creation guide.

**crx2rnx (Hatanaka decompression).** The Python data scripts
(`bootstrap_internal_data.py`, `fetch_global_obs.py`) decompress via
the **`hatanaka`** library (pinned in `requirements.txt`; bundles
RNXCMP 4.x, so RINEX 2/3/**4** all work — the old standalone crx2rnx
4.0.x rejects RINEX 4 with "format version not valid" and leaves an
82-byte stub). `bootstrap`'s `crx_to_rnx` keeps the external crx2rnx
subprocess as a legacy fallback when hatanaka isn't installed.

The C++ production path (`ProductResolver` → `DataDownloader`) still
shells out: RNXCMP's `crx2rnx` is invoked with stderr+stdout captured
(via `_popen` / `popen`); failures are logged at error level with the
exit code and the first ~10 lines of output. Resolution order for the
executable: `data_download.crx2rnx_path` → well-known install dirs
(incl. `C:/GnssProcessorSupport/exe/crx2rnx.exe`, upgraded in-place to
RNXCMP 4.1.0 for RINEX-4 support; the 2015 binary is kept beside it as
`crx2rnx_2015_rnxcmp.exe.bak`) → `where crx2rnx.exe` (Windows) /
`which crx2rnx` (Unix). When a `.crx.gz` download succeeds but
conversion fails, ProductResolver hard-errors rather than masking the
problem with a `.rnx.gz` fallback.

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

- **Test suite.** `gnss_tests.exe` runs a substantial GoogleTest suite (~550 cases across 33 files; see docs/test_suite.md) — filter core, transforms/estimation, RTCM/SBF decoders, ILS/MLAMBDA, state-key schema, the network estimator + normal equations + product I/O, cycle-slip/TurboEdit, the RTS smoother, aiding, and real-time seams. It guards structure and non-regression (new work is developed default-OFF / byte-identical). It does NOT validate end-to-end estimation accuracy — confidence for numerical changes still comes from a clean build + user runs on real data + trace inspection.
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
