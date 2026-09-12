# GnssProcessor

C++17 GNSS post-processor and real-time engine. Supports PPP and PPP-RTK (uncombined), optional IMU integration, integer ambiguity resolution (MLAMBDA), and a backward RTS smoother. Primary config: `src/core/config/gnss_config.json`.

## Build

**Visual Studio 2026 Enterprise (v18) + CMake + Ninja. VS 2022 is RETIRED —
standing user directive, 2026-08-12. Do not invoke the 2022 toolchain even
though it is still installed on disk.**

Use the wrapper — it encodes every trap below:

```powershell
C:\GnssProcessor\scripts\vs2026_build.ps1 -Config x64-Debug -Target gnss_core   # fast compile check
C:\GnssProcessor\scripts\vs2026_build.ps1 -Config x64-Debug                     # full Debug + tests
C:\GnssProcessor\scripts\vs2026_build.ps1 -Config x64-Release                   # every data run
C:\GnssProcessor\scripts\vs2026_build.ps1 -Config x64-Debug -Reconfigure        # after a toolchain change
```

Migration facts, all verified 2026-08-12 (fresh configure + 538/538 build +
full unit suite under the new toolchain):

- **vcvars64.bat moved.** `...\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\`
  — `18` is the install *major-version* dir, not a year. The user's default
  shell still does NOT source it, so a naive `cmake --build` fails with
  "Cannot open include file: 'memory'".
- **Toolchain: MSVC 19.51.36256 / toolset 14.51.36231.** The existing
  `C:\vcpkg` tree is v143-built and links against it cleanly (Eigen, spdlog,
  gtest 1.17, netCDF, CURL, ZLIB, pybind11 2.13.6) — no vcpkg rebuild needed.
- **⚠ Two CMakes on this box.** `cmake` on PATH is the standalone **3.18.3**
  (`C:\Program Files\CMake`) and vcvars does NOT override it; VS 2026 bundles
  **4.3.1** at `...\18\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\`.
  The wrapper pins the bundled 4.3.1 by absolute path so a CLI configure and
  an IDE configure agree. CMake 4 hard-errors on `cmake_minimum_required` < 3.5;
  the lowest in our dependency closure is exactly 3.5 (and only in a doc file
  plus a vcpkg-internal port script, neither consumed by `find_package`), so
  the project configures clean.
- **⚠ Never `set "PATH=...;%PATH%"` inside the `cmd /c "vcvars && ..."` chain.**
  `cmd` expands `%PATH%` when it parses the whole `a && b` line — *before*
  vcvars runs — so the assignment writes back the pre-vcvars PATH and silently
  destroys the compiler entries, giving "No CMAKE_CXX_COMPILER could be found"
  on a machine where the compiler is fine. Ninja needs no pinning: vcvars
  already puts VS 2026's own ninja (1.13.2) on PATH.
- **⚠ The migration surfaced one real code defect — the cached-static logger
  accessor.** `RinexNavParser.cpp`'s `navLog()` held
  `static shared_ptr lg = spdlog::get(...) ? ... : spdlog::default_logger();`
  and returned `lg.get()`. Under VS 2026 that faulted (0xC0000005) on the only
  two parser paths that log, so the suite showed exactly two failures
  (`RinexNavParser.RejectsPreRinex3`, `MissingEndOfHeaderReturnsEmpty`).
  Causality was confirmed both ways (revert reproduces, fix clears), and a
  standalone probe in the identical Debug/`SPDLOG_SHARED_LIB` flavour showed
  `spdlog::default_logger()` itself returns a **valid** logger — so this is NOT
  a vcpkg ABI problem; explicit loggers and fmt formatting cross the v143 DLL
  boundary fine. **The pattern to avoid is caching a raw pointer out of a
  by-value `shared_ptr` (worse, out of a ternary of two DLL-returned
  temporaries) in a function-local `static`.** Resolve per call and return the
  `shared_ptr`; `spdlog::default_logger()` is nullable in principle because
  `spdlog::drop_all()` clears it, and `LogManager` calls that on both
  re-initialize and shutdown. `tailLog()` in `NetworkProductIO.cpp` had the
  same nullable shape with three unguarded `->` uses and was hardened too.
  Whether VS 2022 also faulted on the *current* source is unverified — the
  archived pre-migration exe is a different source revision, and current
  source was not rebuilt with 2022 per the retirement directive.
- `vswhere.exe not recognized` from the cmd subshell remains benign noise.

Targets: `gnss_core` (static lib), `gnss_processor` (exe), `gnss_tests` (exe),
`gnss_processor_module` (pybind11 `.pyd`), plus `gnss_simulator_selftest`.

Build tree: `build/x64-Debug` and `build/x64-Release` — both are **git-ignored
and disposable**; regenerating from scratch is one wrapper call (~180 s Debug).

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

tests/                GoogleTest unit suite (1540 cases / 215 suites across 104
                      files) linked into `gnss_tests.exe`.  tests/sim/ is a
                      SEPARATE target, `gnss_simulator_selftest` (80 cases).
                      Note: docs/test_suite.md documents the real-data
                      truth-graded harness, NOT this unit suite.
logs/                 Runtime CSV traces + spdlog files (under build tree)
runs/                 Campaign run outputs.  ⚠️ ARCHIVED OFF C: on 2026-08-12
                      to D:\GnssProcessor_archive\2026-08-12\runs\ (619 dirs,
                      40.6 GB) along with the pre-VS2026 build/ trees, to
                      reclaim disk.  Both are git-ignored.  Reports that cite
                      a `runs/<name>` or `build/x64-Release/run_<name>` path
                      (e.g. CONFIG_promotion_ledger.md §A1,
                      CLOCK_HADAMARD_CALIB_2026-07-14.md) resolve under the
                      archive, NOT in the working tree.  Per user: those
                      results predate the recent network work and are not
                      trusted — regenerate rather than re-grade them.

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

**Git commits — NO Claude attribution (standing user directive).** Commit messages
carry subject + body only. Do **not** append `Co-Authored-By: Claude <model>
<noreply@anthropic.com>` in any model/version variant, and no "Generated with
Claude Code" line. This overrides any harness default that says to add one.
Co-author trailers for human collaborators are fine.

This is enforced mechanically by `.githooks/commit-msg`, which strips the trailer
rather than rejecting the commit (a rejection just gets retried by an agent that
does not know the rule). **The hook needs one command per clone** — git cannot
auto-install hooks, by design:

```bash
git config core.hooksPath .githooks
```

Without that, the hook is inert and the trailer lands silently. It did: 44 of the
63 commits in the 2026-07-29 `master`→`main` merge carry it, from sessions that
never saw the directive when it lived only in an agent memory file. Those are
published and merged — do not rewrite them; stripping would mean force-pushing
over merged history. Note `core.hooksPath` *replaces* `.git/hooks` wholesale, so
any future hook belongs in `.githooks/` or it will not run.

When auditing for the trailer, grep for `co-authored-by` — a `grep -i claude`
over commit messages false-positives on the literal filename `CLAUDE.md`.

**Commit convention.** Concurrent sessions are normal here, so a tree often holds
several independent workstreams. The established pattern is one commit with a
compound subject (`<areas>: land concurrent workstreams (<item>, <item>, …)`) and
a body sectioned per workstream — see `9a2f289`, `dee2824`, `18b4502`. Read the
other sessions' diffs before describing them, and confirm the tree builds first.

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
   Jul 2026), **and the response's own RE-COUPLING edge: a refloat that
   re-levels from ONE epoch of code CREATES a fresh zero-q value seeded with
   station-common iono error — at a realistic slip rate the level channels
   random-walk metres over hours with every internal instrument silent
   (60-sta truth campaign, Aug 2026; `docs/SPEC_slip_relevel_walk.md`).**
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
7. **Any mechanism that changes which information flows into which states —
   gates, deweights, admissions, coverage/rank drops, datum handovers, holds,
   withhold eras — is an EVENT at BOTH edges, with counters and identities.
   The DECOUPLING edge must bound what the decoupled states can accumulate;
   the RE-COUPLING edge must re-validate or re-seed what it reconnects.
   "The weight ramps smoothly" is not an exemption — era-2's valve ramped at
   the configured rate limit and discharged anyway.**
   Field-demonstrated: the FT63 leg-19 topology ledger
   (`network.trace_topology`) recovered 13 re-coupling edges that were
   previously silent, including the three stations (SAVO/OAFA/POVE) whose
   SH-tie weight went 0.25 → 0.5 at TOW 329190 one epoch after the era-2
   discharge. Every one of them had been invisible because a station with zero
   tie rows dropped out of the baseline map and its return read as a *first
   sighting* rather than a re-coupling — the exact failure this invariant
   names. The open valve inventory (which valves still have a bare edge) lives
   in `docs/SPEC_prefit_census_observability.md` §13.

## Standard armed set — every NETWORK run uses these (directive, Jul-28)

**⚠ THE GOLDEN CONFIGS ARE THIS SET MADE EXECUTABLE (2026-09-05).** Do not re-derive the
armed set from this prose or from an old arm — start every new network arm from a golden:

```
config/golden/golden_sim_tri_w6.json       # sim truth bed: GPS+Gal triple, W6+GF datum, free slant iono,
                                           #   phase_leveling + F3 station block, r2q3 anchor, sticky pivot
config/golden/golden_field_gps_dual.json   # field DOY-129 63-sta GPS L1/L2/L5, SH deg-15, W6 OSB,
                                           #   Card-4 carry_invariant + PD-safe fold + D5 v3, WTZZ consumer
python scripts/make_golden_config.py --verify   # run at session start and after ANY ledger change
```

They are GENERATED (`scripts/make_golden_config.py` from `config/golden/golden_manifest.json`
+ `docs/CONFIG_promotion_ledger.json`), never hand-edited: the generator flattens the base
lineage, pins the manifest's knobs, and FAILS if any ledger RECOMMEND_ON/SCOPED knob that is
in regime for the golden is unarmed without a documented exception — so a new validated card
that the goldens do not carry breaks the verify, which is the promotion step this project
lacked (`docs/CONFIG_promotion_ledger.md` §A4).  An arm = a copy of a golden with ONLY the
knob under test and the run paths changed.  If you find yourself chasing a problem the
ledger already resolved, the golden was not the base — check `git diff` of your arm against
its golden first.

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
- **Observability, prefit tier** (promoted Aug-1 on the FT63 leg-19 gates —
  all three trace-class and fingerprint-SAFE, so legal on a warm resume):
  `trace_prefit_census` (the ONLY always-on PRE-solve screen: postfit σ̂₀
  launders a parked cell through the free ambiguities, and `raim_within_epoch`
  is carry_warm-gated and inert without leveling; 7.6 ms/epoch at 63 sta,
  solve byte-identical), `trace_topology` (invariant #7's two edges), and
  `rhs_attrib_on_trip` (the next discharge attributes itself — reproduced the
  era-2 attribution with no armed TOW).  The FUSE
  (`network.prefit_fuse_sigma`) is deliberately NOT in this set: it is
  solve-affecting and stays a validated default-OFF guard until total_IF
  grading and a second event.
- **Parallel**: `parallel_carry: true` / `parallel_reduce: false` (mutex).
- **Lane layer**: OFF **on large networks only** (~300 sta); at ~60 sta it is a
  small net LOSS. Scale-dependent — see below.

### Amendments from the AGU-26 campaign (2026-08-04) — measured, not inferred

**1. The MW/WL lane layer is OFF on LARGE networks — the win is scale-dependent
and it is NOT a blanket default.**
`applyLaneMeasurement` is **84.7 % of total runtime at 305 stations** — 232 ms
per row of dense O(dim²) downdate on a **2K×2K joint block** (max K_c 3866, so
7732² = 478 MB touched per row, twice, because the per-row re-symmetrize
allocates a second copy). Settled 305-sta GPS epoch:

| arm | s/epoch | note |
|---|---|---|
| `lane_carry.enable: true` (the old default) | **310–325** | 3 h of data = 33 h wall |
| + `fold_blocked` + `parallel_components` | **115** | 2.8×; fold_blocked is NOT bit-identical |
| `lane_carry.enable: false` | **26.7** | **12×**; 3 h of data = 2.7 h wall |

Turning it off also shrinks the largest carried component **3866 → 2359** (the
pair-block both merges the two bands into one component and doubles its
dimension). It buys nothing measurable on DOY-129: AR fix rate is **0 %** and
med|frac_cyc| is **uniform 0.25** with the layer ON, i.e. the lane layer costs
85 % of the run to support integer fixing that is not happening. Consistent with
the standing "MW/WL as conditioning — do NOT arm; net-harmful" verdict.

**~~It does NOT transfer to ~60 stations~~ — SUPERSEDED 2026-08-04. Lane OFF
wins at 63 stations too, once the window is settled.** The earlier reading
(30-epoch arms `runs/agu63_nolane_30ep`: dd-build 895 → 241 ms but `install-post`
108 → 291 and `solve-glue` 233 → 308, TOTAL 2084 → 2207 ms = a small net LOSS)
was measured **entirely inside ignition** — the same objection this section
already raises against 30-epoch arms below. Re-measured on matched 1 h arms
(`_golden63_gps_1h_laneoff` / `_laneon`, 121 epochs, median over the 101 settled
epochs, single knob differing):

| section (median, settled) | lane OFF | lane ON | ON/OFF |
|---|---|---|---|
| **total epoch** | **2266.8 ms** | **2940.1 ms** | **1.30×** |
| dd-build | 173.3 | 1150.0 | 6.64× |
| install-post | 706.9 | 526.8 | 0.75× |
| solve-glue | 363.6 | 386.1 | 1.06× |
| assembly / reduce / cov-consumers | 250.6 / 134.2 / 163.6 | 252.7 / 93.3 / 163.8 | flat |

It attributes exactly: +977 ms dd-build, −180 ms install-post, net +673 ms =
the observed median gap. Both component DIRECTIONS matched the old 30-epoch
reading and the install-post penalty is nearly identical (+183 then, +180 now);
what flipped the total is the much larger settled dd-build saving (−977 vs −654).
**Lane OFF is now the speed call at BOTH scales** — the old "decide it on speed
only above ~100 stations; below that decide it on science" no longer applies,
though science still points the same way.

**⚠️ It changes carry RETENTION — now measured (2026-08-04), and the answer cuts
both ways.** Same 1 h arms. Whole-block marginalizations: lane ON = **0**, lane
OFF = **2549** over 121 epochs (reproducing the ON=0 / OFF=many pattern; the
earlier figures were ON = 0 over 1440 ep, OFF = 735 over 31 ep). Lane ON ends
with **726 carried DDs from 2113 births** vs OFF's **498 from 4312** — 46 % more
retained carry from half the births, and max K_c 460 → 708 (the pair-block
dimension doubling). Without pair-joining, a per-band component whose chords all
refloat is discarded wholesale, where the joint block survived if either band did.

**But the two σ̂₀ families split, and that is the finding.** Prior side gets
QUIETER with lane ON (prires `cb` σ̂₀ 2.3372 → 2.0233, `rxcb` 1.5249 → 1.1417,
global prior-only σ̂₀ 2.1051 → 1.8463, rows > 4σ̂₀ 717 → 223) while the post-fit
DATA side gets ~2.3× LOUDER on two independent instruments (block-Baarda G σ̂₀
6.372 → 15.51; station-congruence σ̂₀ 17.69 → 40.53). The prires drop is not a
moved-threshold artifact — lane ON's σ̂₀ is *lower*, so its 4σ̂₀ bar is tighter,
and it still flags 3.2× fewer rows. **Interpretation (consistent with the data,
not independently proven): that split is what OVER-RETENTION looks like — the
lane keeps state the data does not support, the carry stays self-consistent
because it is stiff, and the observation residuals pay for it.** Consistent with
the standing "MW/WL as conditioning — do NOT arm; net-harmful" verdict, now with
a visible mechanism. **Still ungraded: total_IF** — 1 h cannot settle the LEVEL
channel, so grade that on a ≥2 h run before quoting it.

*A/B sanity gate worth reusing:* the block-Baarda block-size histogram and the
datum-graph census came out **bit-identical** across the two arms (7259 blocks,
`b1:17 b2:25 b3:15 b4:3672 b5:2 b6:3528`; 67518 edges, 438 tier-2). Both key off
observation / S-basis structure rather than carry structure, so a lane A/B that
moves them has a second variable loose.

**2. Do NOT arm `emit_pb_filter_gate_v2` on this lineage.** Tested Aug-4
(`runs/agu63_pbv2`, 12 h): solve byte-identical (CLK and CB max |Δ| = 0.0, so
invariant #6 passes), but the effect is null-to-negative — ref-path-slip resets
unchanged at 538, innovation resets **0 → 31**, PB discontinuity rate 1/128 min
→ 1/124 min, consumer unchanged at 0 % fix. Its advertised win ("innovation
resets 5707 → 32") has an **empty target population** here because the inherited
`emit_pb_filter_jump_k: 8.0` already suppressed them. **91 % of emitted
phase-bias discontinuities are ref-path SLIP re-referencing**, which no emission
filter touches.

**3. `emit_pb_filter_jump_k: 8.0` in the FT63 lineage is the RETIRED v1 hand
tuning** — the code default is 4.0.

**4. `trace_ionosphere` is a DIAGNOSTIC, and `network_ionosphere.csv` is
frame-broken.** It emits `x_r` alone while the per-LOS iono state is seeded at
allocation, so it is a CORRECTION, not absolute slant ionosphere (medians 9.9
TECU against the field's own 30–33 TECU a00, 37 % negative). For anything that
needs the estimated ionosphere, synthesize `network_vtec_sh.csv` —
`scripts/sh_vtec.py` (ported basis + `--selftest`).

**5. Candidates that still need their first validation run** (do not arm blind):
`network.clock_datum_broadcast_anchored` — `declared_dimension = 0` with "no
pivot, no handover seam", and the clock is the worst re-realizer measured
(**13 product-wide jumps** post-convergence over 12 h vs 2 for code bias and 0
for phase bias); and `network.emit_clock_disc: true` — without it the
per-satellite clock disc counter is never populated, so all 13 of those jumps
are **silent to the consumer**.  `network.clock_datum_sum_to_zero` got its
first validation 2026-08-11 on the synthetic 50-sta fixture
(SPEC_synthetic_minimum_network §9.8): PASS — wire mean pinned exactly, wander
0.193 → 0.036 m at a fixed polar datum, availability unchanged, set-change
re-referencing auto-marked.  Sim-only; the field arm is still owed, and the
same matrix showed the bigger consumer lever is PIVOT ELECTION (the datum-free
contrast moves 2.2× with the pivot's geometry — `clock_datum_station` is a
product-quality decision, not a label).

**6. NEVER grade the ionosphere LEVEL against a single GIM number, and never on a
1 h window** (2026-08-05, `docs/SPEC_level_information_budget.md` §14).  Two
independent traps, both paid for:

- The GIM's own **global mean VTEC moves through the day** — measured on DOY-129:
  32.0 TECU at 00 UT, 36.9 at 08, 33.9 at 24.  Every level verdict on the EXT-CB
  ladder before this was quoted against "GIM 32.24", the 00:00 map alone, so a run
  tracking the ionosphere reads as drifting and vice versa.  Use
  `python scripts/gim_global_mean_series.py <run_dir> …`, which reports
  `a₀₀(t) − GIM_mean(t)` hourly (a sun-fixed a₀₀ **is** the global mean: Ȳ₀₀ ≡ 1).
- **DEFECT 2 is about half an ignition transient.**  Graded time-matched, the
  canonical EXT-CB posture starts −15.8 TECU (what every 20-epoch and 1 h arm
  measured) and from hour 2 wanders in a −4…−15 band, mean ≈ −9.  The level
  channel's convergence budget is ~2 h; any level claim from a shorter window is
  measuring ignition.  The LV-1/2/4/5 stack inverts the trade — best ignition on
  the ladder (+4) and then a ramp to +15…+21 — so **it is NOT armed for settled
  work**; its cards remain valid fixes to the defects they name.
- Corollary, measured over four 12 h postures (§15): **the rx-CB carry's rolling
  common-mode memory is what TRACKS the level**, and the ±6 TECU breathing is its
  price.  Freezing that memory (de-mean) drifts +20; loosening it (`rx_cb_carry_q`
  1e-9 → 1e-6) drifts +25 — the frozen pin was RESISTING the drift; removing the
  carry entirely collapses at −9 TECU/epoch.  Do not "fix" the level by tuning the
  carry's memory model in either direction; it needs a per-epoch data-derived
  authority instead.

Grading note: the response doctrine is adapt-and-EMIT within the detected
epoch; a withhold is the backstop and is graded as an adaptation failure
unless the epoch is provably unrecoverable.  (The SH gate currently has no
release/degrade policy — a sick-field era withholds the wire for its full
duration, 95+ min measured on DOY-129; open policy item.)

### Amendments from the wu93 night (2026-08-21/22) — ledger §A3, measured

**1. The carrier-phase wind-up frame is `nominal-wu93`** (the sign fix,
`4f49fa2` + consumer-gap closure `0d25aa0`).  Every pre-fix product, golden
byte-cert hash, and state snapshot is an **expected-fail baseline** —
re-capture, never re-grade.  **A sim fixture and a producer must sit on the
SAME side of the fix** (`runs/simnet60_{campaign,quiet}_wu93` are the current
fixtures): the mixed pairing injects 2φ_wu into every phase obs and is
INVISIBLE to σ̂₀ at short horizons (ambiguity-absorbed) — enforce by
provenance, not by instruments (SPEC_phase_windup_sign §9.9).

**2. Slipped sim postures arm `cycle_slip.relevel_free_epoch`** (walk dead,
720/720, quiet-twin ZWD p50 2.5 cm — the campaign template now pins this).
**`relevel_retap_statistical` and `relevel_retap_corroborate` are KEEP_OFF —
both FALSIFIED on the lane-integer instrument** (settled GPS EWL at-own-true-
integer: refloat-always 70.2 %, corroborated keep 59.6 %, keep-always 54.0 %;
the raw MW/GF evidence refuses only 1.5 % because T4 catches sub-threshold
DRIFTS, not steps — SPEC_lane_slip_response).  The lane-integer slip cost is
**100 % response-inflicted** (response-virgin chords: 98.9–100 % at their own
true integer); the open build is the LANE-PRESERVING refloat (EWL-MW snap,
σ ≈ 0.05 cyc) + band-scoped refloat.
⚠ **UPDATE 2026-08-22 (SPEC_lane_slip_response §7.2): the SEED-SITE snap is
DELETED and must not be rebuilt.**  Its only write was an `x_lin` shift on a
local ambiguity column — a proven EXACT solve no-op on the network producer, so
all three iterations were debugging a mechanism that could not actuate (the
carried prior's RHS `η = Λ·(value_abs − x_lin)` absorbs the shift, and the
graded instrument `network_dd_fractional.csv` forms `x_lin + x_r`).  Its `d`
degenerated to `−Δe_b/λ_b`: one band's frozen-frame model-error change over a
SINGLE-band wavelength, 3.53× the lane coefficient — which is the whole 15.9-cyc
"structural discrepancy".  **The lane lives in `LocalCarry::value_abs`** (the
absolute estimate `x_lin + x_r`, where the frozen frame is structurally absent);
rung 3 v3 measures it there, OBSERVATORY-FIRST (measure-only, byte-identical on
every solve artifact — SPEC §7.3).  `network.cycle_slip.relevel_lane_snap` is
RE-POINTED at that observatory behind two interlocks (`phase_leveling`, and
`relevel_free_epoch` on its SETTLED value; dies under `carry_invariant`).

**3. `estimation.accumulate_phase_windup` is an OPEN default card** — every
field lineage silently runs FRACTIONAL (wrapping) mode (±1-cycle staircase,
MW-blind; the loader now warns).  Do not flip the loader default without the
migration card; arm it explicitly on fresh-convergence lineages.
⚠ **UPDATE 2026-08-22 (SPEC_phase_windup_sign §10): the flip is BLOCKED on an
inverted horizon, not just on evidence.**  The accumulator's continuity horizon
is 300 s while the FT63/EXT63/FLD63 chain runs `phase_leveling_max_gap_s: 900`,
so a gap in **(300, 900] s** restarts the unwrap — an exact integer-cycle step
on every signal of that satellite — while the carried ambiguity and the zero-q
leveling level SURVIVE and re-inject at their pre-gap values.  Invariant #1 with
no wired response, invariant #7 with both edges silent, and MW-blind (the step
is band-common, ΔWL = 0).  The accumulator itself is safe as KEYED (one
`GnssCorrections` per station makes the satellite-only key injective — verified;
the simulator's shared-provider fan-out is the exception and already pays a
process-per-station cost).  **Arm `estimation.trace_windup_census` on a
fractional field lineage first** — DONE 2026-08-22 (`_fld63_wucensus4h`, 4 h,
61 sta): the staircase is **51 in-horizon one-cycle steps (~306/day network-wide,
2.15e-4 per sat-epoch — §7's GCGO rate once cadence-normalised)**, and
`gap_wu_to_carry` = **20** (~120/day) — NONZERO, so the wired restart response is
a BLOCKING prerequisite.  ⚠ Sharper than §9.5's algebra implies: accumulation does
not zero the defect, it trades ~306 fractional wraps/day for ~120 UNCOMPENSATED
restarts/day — a ~2.5× reduction that moves the problem to an uninstrumented
channel.  Build the response, not the flip.

**5. THE RE-ADMISSION EDGE (2026-09-05/06, SPEC_amb_iono §6.10/§6.11, SPEC_synthetic
§18.13/§18.14).**  A satellite's eclipse return was one edge behind three open items.
`network.readmit_seed_horizon_s: 3900` (clock AND code-bias seeds — the clock-only
variant is FALSIFIED, the tails return) is VALIDATED and pinned on the sim golden: T3
clock tails 2–4× smaller, consumer post-eclipse excursion 2–3× smaller, DD unchanged.
`readmit_coupling_marg_ep`, `phase_datum_handover_max_var_ratio` and BOTH F3 age-gate
responses are KEEP OFF (the post-eclipse DD episodes are NEITHER the returned columns
NOR the newborn legs; the ratio's first-stash denominator is biased by pin tenure; ANY
diagonal leveling row for a subset of legs re-poisons the station).  The warm-restart
consumer residual was the one-epoch DIAGONAL border fold at the seam: `resume_frame_
restore` + `resume_coupling_restore` on a QK/QP-bearing snapshot (leg E) gives a warm
restart whose consumer converges like a cold consumer on the continuous wire — ⚠ grade
every seam card with a COLD-CONSUMER REPLAY (`_seamctl_replay_*`, `splice_wire.py`),
never the seam-epoch diff alone: it cannot see an integrated, unsignalled drift.

**6. THE DD TRACE HAS ONE REFERENCE SATELLITE, AND IT IS IN EVERY CHORD (2026-09-06,
SPEC_amb_iono §6.12).**  `network_dd_fractional.csv` differences every satellite against the
lexically-lowest one present at ≥ 2 stations (G01 / E02 all day on the sim); its own
between-station error is common to every chord, and its eclipse return re-references the
whole trace to newborn legs — the "post-eclipse network-wide DD episode" (as-traced 0.52–0.59
cyc) was that; ref-free the DD is flat 0.07–0.11 cyc all day.  ⚠ Every DD-integer number
quoted before 2026-09-06 is as-traced.  Grade with `scripts/simnet_dd_reref.py` or
`simnet_amb_integer.py --ref-free`.  Rule: any grade built on a single reference element
reports that element's share or differences it away.
⚠ **UPDATE 2026-09-06 (§6.13): the FIELD is NOT this artifact — the DOY-129 uniform-0.25
histogram stands on its own.**  Truth-free instrument `scripts/dd_frac_reref.py` (the field has
no truth, so it grades `frac_cyc` on the circle against a Monte-Carlo null).  It FIRES on the sim
base — R 0.610 vs null 0.430, ref-free gap −0.047, and its 0.079 reproduces the truth-graded
0.089 — and is SILENT on `fld63_stk_on`: R 0.468 vs null 0.473, gap +0.008 on every signal.  The
field reference also rotates on its own (8–11 switches, 3–9 satellites) with no post-switch
degradation.  What the field DOES inherit is the COVERAGE half (chords exist only for pairs that
both see the reference), so per-station field splits off this trace still compare rotating
populations.  `network.trace_dd_fractional_ref` = `lowest` (default) | `oldest` is BUILT and
UNVALIDATED — goldens stay on `lowest`; an `oldest` trace is quieter, never reference-FREE.
⚠ **TWO NULL TRAPS in any wrapped-fraction grade** (both cost a false finding here before they
were caught): subtracting a group's own circular median shrinks |residual| ~42 % on pure uniform
noise, so quote the GAP to a size-matched null, never the raw value; and a triangle-closure test
is confounded because a DD is per-station additive BY CONSTRUCTION — its null must carry the DD
structure with no reference term (field closure 0.182 reads as structure against 0.250 and as
NOTHING against the correct 0.172).  ⚠ A truth-free wrapped grade is only meaningful where the
underlying errors are SUB-CYCLE: two arms picked as controls had 2–20-cycle DD errors, wrapped to
uniform, and looked like a clean refutation of §6.12.

**4. `network.trace_station_residuals` writes ZERO rows on `carry_invariant`
chains** — the writer rides the leveling RAIM pool, structurally empty there
(the known pool re-host gap).  Use the prires / block-Baarda / congruence
run-end counters as the σ̂₀ instrument set on those lineages.

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

- **Test suite.** `gnss_tests.exe` runs 1975 GoogleTest cases across 269 suites in 130 files (~120 s, full Debug run under VS 2026, measured 1974 passed + 1 skip on 2026-09-12 after the RS-2 / RS-2b / reference-arc cards — the earlier 1540/215/104 figures predate them; one fixture-gated skip — `FixtureSmokeTest.Sp3HeaderTimeConversionMatches`, which needs `bootstrap_internal_data.py` to have run) — math core, site displacement, atmosphere models, antenna/ATX, filter core, transforms/estimation, RTCM/SBF decoders, ILS/MLAMBDA, state-key schema, the network estimator + normal equations + invariant-carry driver + product I/O, cycle-slip/TurboEdit, the RTS smoother, aiding, and real-time seams. `tests/sim/` is a **separate** target, `gnss_simulator_selftest` (80 cases), not linked into `gnss_tests`. The suite guards structure and non-regression (new work is developed default-OFF / byte-identical). It does NOT validate end-to-end estimation accuracy — confidence for numerical changes still comes from a clean build + field runs graded on datum-invariant quantities (see Standing invariants) + trace inspection.
  - Suite hygiene: every suite name should be owned by exactly one file — keep it that way, and prefer a new file over widening an existing one (the zero-cross-file-overlap property was re-verified at 215 suites on 2026-08-22, tests/ + tests/sim/ both scanned). At 51.5k test LOC against 189.2k production LOC the suite is *thin* (0.27 ratio, ~1 case per 139 production lines); the gap is coverage, not bloat. There is no dedicated doc for this suite — `docs/test_suite.md` is the real-data harness.
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

`src/core/app/ApplicationNetwork.cpp` (`runNetwork`) was decomposed (INC-0..12) from ~14.7k lines to ~1.6k lines of thin epoch-loop orchestration at INC-12 (**2,359 lines as of 2026-08-15** — accreted ~750 since the cut; re-measure before quoting); the per-epoch body is a sequence of `subBegin; networkX_(...); subEnd;` calls into single-concern seams. **Keep it thin** — new network-producer code goes in the seam matching its concern, NOT inline in the driver:

- **New per-epoch estimation/QC step** → the matching `filter/Network*` seam (or a new one) called from the loop. Datum/solve → `NetworkSbasis` / `NetworkChainDatum` / `NetworkPhaseDatum` / `NetworkSolveGlue` / `NetworkDdPriorBuild` / `NetworkInstallCarry` / `NetworkProducerAr`; post-fit QC → `NetworkRaim` / `NetworkClockQc` / `NetworkRelevel`; slip → `slip/NetworkSlipQc`; coupling → `NetworkCoupling`.
- **New emit/store behavior** → `NetworkSolPrep` (pre-emit prep) or `NetworkEmitStore` (emit / product-write / closed-loop consume).
- **New diagnostic/trace sweep** → `NetworkIonoDiag` / `NetworkDdFrac` / `NetworkShDevVtec` / `NetworkShHealth` / `NetworkLeveling` / `NetworkStashStatus`, or a new diag seam. Trace-stream ownership + run-summary/manifest writers live in `logging/NetworkRunTraces` (NOT the driver).
- **New run-end telemetry / readout** → `filter/NetworkTeardown` (the post-loop run-end tail).
- **New carried/persistent state, counters, or config snapshots** → `app/NetworkRunContext` (`NetworkCarryState` / `NetworkServiceCounters` / `NetworkKnobs`), aliased in the driver and passed to the seams — do NOT add a bare function-local (that is precisely what INC-12 spent a pass deleting: 191 dead aliases left behind by relocated blocks).
- **New run-loop-local helper type a seam must name** → `filter/NetworkRunTypes` (general) or `filter/NetworkWlRunTypes` (WL / section-timing).
- **Setup** (validation / sinks / consumer) → `app/ApplicationNetworkSetup`; **assembly** (PASS A/B) → `app/ApplicationNetworkAssembly`.

Seam contract: a free function over `(NetworkRunContext& ctx, const GnssConfig& config_, log, NetworkRunTraces&, …)` opening with an alias preamble that reconstructs the driver's `ctx.*` / `setup.*` aliases so the body reads the original names; it owns NO state (every carry object is a reference into `ctx`). The driver keeps only the `subBegin/subEnd` brackets and the call. New `.cpp` seams need a manual `CMakeLists.txt` entry (no globbing).

### Real-time engine — where new code goes (anti-regrowth decision-tree)

`src/core/realtime/RealTimeEngine.cpp` got the same treatment (2,366 → 625 lines;
`processEpoch` 1,438 → 135, `initialize` 343 → 19, `feedRtcmFrame` 256 → 78),
cut **before** the RTF-4..9 service queue lands more accretion. The driver is now
thin ordered orchestration. **Keep it thin** — new code goes in the seam matching
its concern:

- **New per-epoch admission / deferral rule** → `RealTimeEngineAdmit.cpp`. These
  are the ONLY seams that may end an epoch: `bool`, false ⇒ `processEpoch`
  returns false. Everything downstream is unconditionally straight-line.
- **New consumer-side observability** → `RealTimeEngineSsrDiag.cpp`.
- **New measurement-set conditioning** (drop/seed/re-gauge, pre-predict) →
  `RealTimeEnginePrepare.cpp`. Note `seedClockFromWls` and `regaugeBssd` are
  mutually exclusive (`hasClock()` vs not) — a matrix that arms only one leaves
  the other uncertified.
- **New slip detection / repair** → `RealTimeEngineSlipQc.cpp`.
- **New "the producer moved its datum" reaction** → `RealTimeEngineSsrBreaks.cpp`.
  Order is load-bearing: `reactToGenerationChange` applies the union globally and
  must precede the per-signal handlers.
- **New update / constraint / AR / trace step** → `RealTimeEngineEstimate.cpp`.
  Pseudo-observation constraints belong after the update and before AR.
- **New construction step** → `RealTimeEngineInit.cpp` (the order there is a real
  dependency chain — `seedInitialState` must follow `initProcessingComponents`).
- **New RTCM ingest step** → `RealTimeEngineIngest.cpp`. The obs-buffering loop
  and ephemeris push stay in the driver on purpose: they call the file-local
  anon-namespace helpers `isSystemActive` / `refreshGlonassSlots`.

Seam contract: a **private member function** (not a free function — every
persistent value is already a `RealTimeEngine` member, so no `NetworkRunContext`
equivalent is needed) opening with an alias preamble reconstructing the driver's
local names. Cross-seam per-epoch values travel in `RtEpochContext`, which owns
nothing that outlives the epoch — **a value that must survive to the next epoch
is a member, and putting it in the context silently cold-starts it every epoch.**

Gate: `scripts/rt_seamcut.py` (generator, one canonical seam table),
`scripts/rt_seam_verify.py` (proves each relocated body character-exact against
the frozen pre-cut source — 30/30 verbatim), `scripts/rt_seam_gate.ps1` +
`scripts/rt_seam_compare.py` (4-arm same-build byte-identity A/B, all PASS).
Re-run the verifier after touching a seam body; see `docs/rt_seam_decomposition.md`
for the coverage the A/B does and does NOT certify.
