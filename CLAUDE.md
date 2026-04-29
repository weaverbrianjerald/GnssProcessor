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
  config/             GnssConfig + GnssConfigJsonHelpers (loader split into
                      per-section helpers in src/core/config/GnssConfigLoad.cpp)
  corrections/        GnssCorrections, OceanTideModel
  filter/             KalmanFilter, StateManager, StateKeySchema, Smoother,
                      AmbiguityResolver, ImuIntegrator, PivotManager (PPP-RTK SD),
                      DdPivotManager (baseline DD)
  logging/            LogManager, TraceLogger, KalmanFilterLogger, WlsLogger
  math/               Coordinates, TimeUtils, Interpolate, WlsEstimator
  measurement/        MeasurementModel, StateInitializer, BssdTransform, BrsdTransform
                      (BSSD: PPP-RTK SD via BssdTransform::apply, baseline DD via
                       BssdTransform::differenceBrsd composed on BrsdTransform output)
  model/              PppModel, PppRtkModel, StochasticModel, Antenna, Planetary,
                      ModelGeometry
  products/           ProductManager, ProductCache, ProductParser, PathResolver,
                      ProductResolver, ProductTypes, DataDownloader
  realtime/           NtripClient, RtcmStreamReader, RealTimeEngine
  rinex/              RinexParser, MultiRinexSync
  rtcm/               RtcmParser
  slip/               CycleSlipManager + detectors: MwSlipDetector,
                      IfPhaseSlipDetector, GfSlipDetector, EwlSlipDetector,
                      GraphicSlipDetector
  sinex/, ssystem/    SinexParser, SSystem (currently disabled in CMakeLists)

src/core/             Implementation files mirror the include/ subdir layout
                      one-to-one (app/, common/, config/, logging/, math/,
                      products/, slip/, etc.).  src/core/config/ also holds
                      gnss_config.json — the primary user-edited config file.

tests/                Empty — `gnss_tests.exe` builds but registers 0 test cases
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
by a single top-level config field, `data_source`:

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
  `data_download.data_root`.  Same on-disk layout as internal mode.

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

**Internal-mode date hint.** When the bundled `data/` contains files
for multiple dates (e.g. both 2023-321 and 2024-129), PathResolver
extracts a YYYYDOY hint from the chosen observation file and uses it
to prefer the matching `_MN.` broadcast nav.  See
`extractYyyyDoy()` in `src/core/products/PathResolver.cpp`.

**Internal-mode bootstrap.** `data/` is `.gitignore`d except for
README shells.  Populate it once after a fresh clone:
```
python scripts/bootstrap_internal_data.py            # full set
python scripts/bootstrap_internal_data.py --no-eot20 # skip the 280 MB ocean tide file
python scripts/bootstrap_internal_data.py --crx2rnx C:/path/to/crx2rnx.exe
```
The script downloads SP3 / CLK / BIA / ERP / BRDC for each date in
its `FIXTURE_DATES` list (CDDIS, requires `.netrc`), `igs20.atx`
from `files.igs.org` (no auth), and `EOT20_load.nc` from DGFI-TUM.
For CDDIS-hosted observation files (Hatanaka `.crx.gz`) the script
locates `crx2rnx` (RNXCMP package) on PATH or via the `--crx2rnx`
flag, runs the conversion, and stages the resulting `.rnx` under
`data/observations/<target_subdir>/`.  Local research data (e.g.
the OSU baseline fixtures) is not on a public archive and the
script prints a "please supply manually" message at the end with
the destination paths.

**Baseline fixture presets.** The top-level config field
`baseline_fixture` selects a named bundle of (reference station,
reference ECEF, rover station, year/DOY) from
`include/config/BaselineFixtures.hpp`:

- `IGS_Pasadena` (default) — Caltech (CIT100USA) vs JPL (JPLM00USA),
  ~10 km, 2024-DOY129.  Both stations on CDDIS daily archive;
  bootstrap fetches them automatically.
- `OSU_Campus` — Local OSU survey (NARY ref + 233 rover), ~340 m,
  2023-DOY321.  Not on any public archive; user supplies the obs
  files manually under `data/observations/baseline/short/static/`.
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

- **No test suite.** `gnss_tests.exe` has zero registered tests. Verification for numerical changes = clean build + user runs on real data + trace inspection. Do not claim regression confidence from the test binary.
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
