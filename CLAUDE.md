# GnssProcessor

C++17 GNSS post-processor and real-time engine. Supports PPP and PPP-RTK (uncombined), optional IMU integration, integer ambiguity resolution (MLAMBDA), and a backward RTS smoother. Scenario configs: `config/*.json` (one file = one run; `config/ppp_internal.json` is the PPP starting point). New-contributor onboarding: `docs/onboarding/README.md` + `CONTRIBUTING.md`; worked example on the unmerged reference branch `example/stochastic-model`.

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

**Disk placement (owner request, 2026-10-13: keep C: free, target D: for builds
and runs).** C: is a 1 TB internal NVMe that runs short; D: is a 4 TB NVMe SSD
in a USB enclosure on a 5 Gbps port (about 0.4 GB/s; Release builds on D:
timed within the range of C: worktree builds, being CPU-bound; anything on D:
is unavailable while it is disconnected).
- **Worktrees and their builds go on D:**, as
  `git worktree add D:\GnssProcessorSupport\worktrees\<name>`, built with
  `scripts\wt_build.ps1` from inside that worktree (it roots the build tree at
  its own checkout). `scripts\vs2026_build.ps1` hard-codes `C:\GnssProcessor`
  and rebuilds the MAIN tree from anywhere. A worktree of this repo still loads
  the `C--GnssProcessor` auto-memory (memory keys on the git root).
- **Rapid-arm runs go on D:** with `RAPID_RUNS_ROOT` set, e.g.
  `D:/GnssProcessorSupport/runs_nom` (`scripts/rapid_arm.py`, and
  `scripts/nom_td_gate.py` resolves bare run names there too). Inputs still
  come from C: (`data/`, the sim fixture `runs/simnet60_quiet_col_24h`).
- **Finished runs move to `D:\GnssProcessorSupport\runs\<same name>`**
  (MANIFEST addenda). Folders that configs read stay on C:, or move with a
  junction left at the old path. Never `robocopy /MOVE` without `/XJ`, and
  never `scripts/run_decomp_arm.py --force` on a junctioned run dir (its
  `resolve()` + `rmtree` deletes the D: target).
- Claude session scratchpads (`%TEMP%\claude\...`, where session worktrees
  used to accumulate 11 GB builds) move to D: only if the owner sets the
  documented `CLAUDE_CODE_TMPDIR` variable (user environment or user
  settings `env`; new sessions only).

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
                      products/, slip/, etc.).  User-edited scenario configs
                      live in the top-level config/ dir, not here.

tests/                GoogleTest unit suite (1540 cases / 215 suites across 104
                      files) linked into `gnss_tests.exe`.  tests/sim/ is a
                      SEPARATE target, `gnss_simulator_selftest` (80 cases).
                      Note: docs/test_suite.md documents the real-data
                      truth-graded harness, NOT this unit suite.
logs/                 Runtime CSV traces + spdlog files.  Hard-coded `./logs`
                      relative to the process CWD (`Application::log_dir_`);
                      git-ignored, recreated by the next run.
runs/                 Campaign run outputs.  ⚠️ RELOCATED OFF C: AGAIN on
                      2026-09-19 (the repo-root cleanup for onboarding): the
                      741 run dirs then in the tree (149 GB, incl. every RF-* /
                      ALC / PBT run cited by `docs/refound/`) are at
                      D:\GnssProcessorSupport\runs\ (same names), so a cited
                      `runs/<name>` resolves there.  `reference_software/`
                      (M-PAGES, POINT, cssrlib), `val_out/`,
                      `test_suite_results/`, `out/`, `batch_results*/` and the
                      stray root files moved with them;
                      D:\GnssProcessorSupport\MANIFEST.md maps every old path.
                      Again on 2026-10-06: 4,719 finished run dirs (266 GB)
                      moved to the same place (MANIFEST addendum); the live
                      fixtures, baselines and the 2026-10-03+ working set
                      stayed on C:.  Again on 2026-10-13: 441 entries older
                      than a week (35 GB) moved there too, same rule, plus
                      every FCF / AGT arm (280 dirs, 60 GB; cards closed),
                      plus the runs of every other closed card (1,095 dirs,
                      65 GB) (MANIFEST addendum).  `runs/` is still the git-ignored default output dir; a
                      new campaign recreates it.  Earlier: ARCHIVED on 2026-08-12
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

## Documentation — the topic tree (standing user directive, 2026-09-14)

**`docs/INDEX.md` is the single entry point, and `docs/refound/` is the current
program.** The author's directive: *"We need a method to organize our whole project,
rather than simply appending to these massive files."* On 2026-09-14, 18 of the 19
`docs/*.md` over 60 KB were split into a topic tree — verbatim, hash-verified, with the
originals frozen. The nineteenth, `CONFIG_promotion_ledger.md`, is now a generated view
of the ledger JSON (see the ⚠ below).

```
docs/INDEX.md                    entry point: topics, cards, the rules
docs/refound/                    the current program (RF-0..RF-3) — not split, not moved
docs/cards/<topic>/<CARD-ID>.md  one card per file: STATUS block + verbatim text
docs/topics/<topic>.md           a short living page per topic (17 topics)
docs/decisions/                  posture rulings whose text is self-contained
docs/status/OPEN_ITEMS.md        everything owed, with its evidence pointer
docs/archive/<monolith>.md       the pre-split originals, byte-for-byte, FROZEN
docs/<monolith>.md               a routing stub: section -> card, so an existing
                                 citation ("SPEC_event_harness §24.10") still resolves
```

The rules — **all seven are enforced or checked**:

1. **One card = one file** under `docs/cards/<topic>/`. Pre-register it as a file.
2. **Results are appended to the CARD's file only** — never to a topic page, never to
   `docs/archive/`, never to a monolith.
3. **The INDEX line is updated twice:** at pre-registration, and at close.
4. **No non-archive file over ~60 KB.** Split by topic as one approaches it. The one
   exemption is the generated ledger view `CONFIG_promotion_ledger.md`, named in
   `SIZE_EXEMPT` in `scripts/docs_index_check.py`.
5. **Every card file starts with the STATUS block** (`id`, `topic`, `status`, `summary`,
   `source`, plus the knobs / hashes / run dirs / dates). `status` ∈ {`open`, `closed`,
   `parked`, `superseded`, `reference`}; PROMOTED / KEEP_OFF / FALSIFIED belong in the
   summary and the text. An **Outcome** paragraph follows the block (2026-10-07): question,
   decisive result, verdict (missed bars included), current standing with a link, about 90
   words. The header (title, STATUS, Outcome) is revised as results land; the body never is.
6. **The monoliths are frozen.** `docs/archive/` is read-only; the old path is a stub.
7. **`python scripts/docs_index_check.py` must pass before a docs commit** (reachability
   from INDEX, dangling links, STATUS blocks, the 60 KB bar, the archive banners, the
   generated ledger view).

**Never rewrite, shorten or reflow existing section text** — the docs are the scientific
record; a superseded result is annotated, not deleted. A card's history sits below its
`<!-- BEGIN VERBATIM: … -->` marker; append new results under a dated heading *below* it,
and do not "fix" the links inside it (they were written against the old location and are
deliberately not link-checked). `python scripts/docs_split.py --verify` proves the
existing split is byte-exact (results appended below a body are reported, not failed);
`--build` renders missing cards from `scripts/docs_map/*.json` and never re-renders an
existing card, whose header is maintained by hand.

⚠ **The config ledger is `docs/CONFIG_promotion_ledger.json`: one row, one verdict, per
config key the network loader reads** (rebuilt 2026-10-13; verdicts RECOMMEND_ON /
RECOMMEND_SCOPED / ARMED / UNVALIDATED / KEEP_OFF / REMOVE / PARAM / TRACE / CORE).
`make_golden_config.py --verify` and `config_posture_check.py` read it;
`docs/CONFIG_promotion_ledger.md` is its view (a hand-written head, then tables rendered by
`scripts/ledger_render.py`). A ledger change is a JSON edit, a re-render and the two
verifies; a new knob gets its row in the same commit. The full-prose ledger before
2026-10-13 is in git (`git show 8326da3:docs/CONFIG_promotion_ledger.md`).

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

**Organization (standing user directive, 2026-10-08).** "The codebase should be so
incredibly organized that I can read it and follow easily, and anyone new can immediately
begin contributing." Every change is judged also on whether a reader can follow it: one
owner per concept, typed identities, declared properties, run-start tables a human reads,
short functions with a linear story. On refactors, a clear structure outranks byte identity:
the same-build golden comparison still runs, and any output move is explained before it
lands. The network producer is being reorganized under card NOM
(`docs/refound/NOM_network_object_model_2026-10-08.md`): new network code takes its
identities from `common/GnssIdentity.hpp` (constellation traits, `SatId`, `SignalId`,
`LosId`, `ArcId`; no new `sat[0]` / `sig[1]` parsing), and every station state column has a
declared disposition in `filter/NetworkParamInventory.hpp` (the "parameters as armed" table
at run start). Plain value types and tables; no behaviour classes, no virtual dispatch.

**Documentation prose style (standing user directive, 2026-09-19).** New or edited
Markdown (README, CONTRIBUTING, `docs/onboarding/`, new cards, summaries) uses an
impersonal, concise, technical register. No second-person address ("changes to X are
made in Y", not "you change X"); imperatives only inside numbered procedures. No em
dashes: use a period, colon, comma or parentheses. No personal names as points of
contact ("a maintainer"). Claims are verified against the code or a run before they
are written. This does NOT license rewording the existing scientific record
(`docs/cards/`, `docs/archive/`, `docs/refound/`, this file's older sections). AI
assistants are permitted on this project under the rules in `CONTRIBUTING.md`
(section "AI coding assistants"); licensing and credit are in `LICENSE` and
`CITATION.cff` (interim OSU notice, credit to Brian Weaver, to be replaced by a
journal citation).

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

## Standing invariants: pre-flight every new estimation card against these

Mechanism-independent lessons paid for in field time. Check them at SPEC time and again at the
validation gate; "my mechanism is different" is not an exemption.

1. **Every zero-q or stiff time-linked quantity has a wired, tested slip/jump/event response**
   (drop, refloat or re-seed) before an ON field run. A stiff prior carried past a jump warps the
   whole solution (ionosphere and clock absorb the tension). The response has its own re-coupling
   edge: re-seeding from one epoch of code creates a fresh stiff value carrying that epoch's error,
   and at a realistic slip rate the levels random-walk metres with every instrument silent
   (`docs/SPEC_slip_relevel_walk.md`; the float vehicle of card RF-1 drops the prior instead).
2. **Data runs are Release only** (see Build). Debug timings and grades are artifacts.
3. **Every deferral is triaged field-blocking or field-inert, with data for "inert".** A
   field-blocking deferral makes the toggle not field-runnable, and the card says so.
4. **Run-end counters are the ON-path telemetry.** "0 driven / 0 folded" means inert wiring,
   however clean the log.
5. **Grade only datum-invariant or SD quantities** (single-realization mandate); raw wire steps mix
   benign frame drift with defects.
6. **New work is default-OFF and byte-identical when off**; a shared-path edit gets a same-build
   golden comparison (never Debug against Release: verification integers move at the ppm level).
   A refactor may move output when the move is explained before it lands (Conventions).
7. **A mechanism that changes which information reaches which states (gates, deweights,
   admissions, coverage or rank drops, datum handovers, holds, withholds) is an EVENT at both
   edges, with counters and identities.** The decoupling edge bounds what the decoupled states can
   accumulate; the re-coupling edge re-validates or re-seeds what it reconnects. A smooth weight
   ramp is no exemption. Instrument: `network.trace_topology` (a returning station is a
   re-coupling, never a first sighting); the valve inventory is
   `docs/SPEC_prefit_census_observability.md` §13.

## Network runs: start from a golden

The goldens ARE the posture of the re-founding (`docs/refound/NETWORK_REFOUNDING_2026-09-14.md`;
card list `docs/refound/CARDS.md`). Every network arm is a copy of one:

```
config/golden/golden_field_refound.json   # field DOY-129, 58 sta, WTZZ00DEU held-out consumer
config/golden/golden_sim_refound.json     # sim truth bed, 60 sta (runs/simnet60_quiet_col_24h), SZZ000SIM
python scripts/make_golden_config.py --verify   # at session start and after ANY ledger change
```

Posture, one on two beds: GPS L1/L2 + Galileo E1/E5a, dual frequency; self-contained (satellite
GF code bias pinned at broadcast T_GD, no GIM, no Kaula prior); free per-LOS slant ionosphere (no
SH); float vehicle (`ambiguity_vehicle: float_kf` + `phase_datum_held_basis`) on the
phase-leveling carry with gauge projection, the prioritized phase datum and the exact coupling
update; Q3 clock anchor and clock step test; phase-bias hold; gross-LOS screen with its 12 m cold
bound; accumulated wind-up with restart refloat. No lanes, F3, SH constraint or seams.

- **Generated, never hand-edited.** `make_golden_config.py` builds them from
  `config/golden/golden_manifest.json` and `docs/CONFIG_promotion_ledger.json`, and FAILS when an
  in-regime RECOMMEND_ON/SCOPED knob is unarmed without a documented exception. A knob is armed only
  by the author's decision, through a ledger row and the generator. Loader defaults stay off
  (flipping one invalidates snapshot fingerprints: a migration card).
- **An arm** = a golden copy with ONLY the knob under test and the run paths changed
  (`scripts/rapid_arm.py`). A problem the ledger already resolved means the golden was not the base:
  `git diff` the arm against it. `golden_sim_tri_w6.json` and `golden_field_gps_dual.json` are
  RETIRED (frozen for archived configs and old scripts), never a base.
- **Grading.** A delta against the baseline pair and a same-exe control: the held-out consumer by
  hour plus the 16-site census; never adopt on one instrument. Development checks on 2 h arms; 6 h
  for adoption, census and baselines. Hour 1 is ignition: settled claims come from hours 2 to 6.
- **Fresh convergence only.** Fingerprinted knobs (EXL, GFC and others) never resume from a
  snapshot written without them (`network.ctu_switch_on_resume` stays off; that seam failed CTU G5).
- **Sim start.** The sim golden starts inside the fixture's start artifact: grade sim level,
  ignition and hour 1 from 00:30:15 (`start_offset_hours` 0.504167). Every `start_offset_hours`
  keeps the half-epoch back-off (bare hours sit on a double-residue boundary).
- **Conventions shared end to end.** Never compare numbers across sim fixtures (the pre-SFR
  `simnet60_quiet_wu93_24h` numbers are superseded). Producer, consumers and sim fixture share one
  wind-up convention (accumulated, `nominal-wu93` frame): a mismatch is ambiguity-absorbed and
  invisible to σ̂₀.
- **Interim hold.** `emit_pb_hold_phase_clock` is valid only with a precise a priori clock product
  (card LAH) until the self-contained clock level lands (cards SCL, NBP).
- **Armed QC and observability** (in both goldens; verdicts in the ledger): RAIM with attribution,
  within-epoch tier and `raim_withhold_sigma0` 6.0, clock runaway check, prefit census, topology,
  RHS attribution on trip, run manifest and epoch status, postfit traces, `emit_ura`,
  `emit_clock_disc`, the CB jump test with `mw_detect`, `cold_clock_seed`; `parallel_carry` true.
  Response doctrine: adapt and EMIT within the detected epoch; a
  withhold is the backstop, graded as an adaptation failure unless the epoch is provably
  unrecoverable. Not armed: `prefit_fuse_sigma`. Deleted from the code on 2026-10-14 (card RKD,
  `docs/refound/RKD_retired_key_deletion_2026-10-14.md`): `sh_health_gate`, the old lane layer
  (`mw_wl.*`, `lane_carry.*`; lanes return only as card AB3's new build) and `emit_pb_filter*`.

**Baseline: card GAD (2026-10-13, `docs/refound/GAD_galileo_adoption_2026-10-13.md`).** The
Galileo package is armed: `processing.active_systems` `["GPS","Galileo"]` with E1/E5a in both code
families (`["C1C","C1X","L1C","L1X","C5Q","C5X","L5Q","L5X"]`, `frequency_mode` dual),
`estimation.clock_disc_isb_reinflate_std` 10 and
`model_corrections.enable_receiver_cross_system_atx_fallback` (card GAL2),
`network.code_datum_family_difference` with `code_family_birth_prior_sigma_m` 0.10 (card GFC;
`code_datum_pair_continuity` KEEP_OFF). Pair `runs/_rf0hv15_{field,sim}6` (exe
`gnss_processor_galp2.exe`): field WTZZ00DEU 0.058 0.011 0.018 0.010 0.015 0.011 m by hour; sim
SZZ000SIM 0.137 0.072 0.031 0.024 0.026 0.018 m from the run start. Census against GPS only,
median site in hours 2 to 6: field -0.020 m (10 better, 2 worse: DAV100ATA, HARB00ZAF), sim
-0.030 m; GPS-only users of the wire unchanged at the median (ARHT00ATA +0.20 m). ⚠ Every golden
arm is GPS + Galileo and 2 to 6 times slower. Constellation-blind graders pool the systems:
`rapid_arm.py score` counts E5a as band 5, `ftc_step_attrib.py` R3 needs `--systems G,E`, and
`census6h.py` / `simcensus6h.py` default to GPS-only bases (use `gcensus.py`, or `SIMCENSUS_BASE` =
the wire's arm). Open: DAV100ATA, the GFC hour-2 clauses, SEYG00SYC's E11 / E12 C5Q rows, nominal
yaw steering only, E14 / E18 broadcast unhealthy and kept (GAD section 4).

### Grading traps that still apply

- **The DD trace has one reference satellite in every chord.** `network_dd_fractional.csv`
  differences against the lexically lowest satellite seen at two or more stations; its error and
  its returns enter every chord. Grade ref-free (`scripts/simnet_dd_reref.py`,
  `simnet_amb_integer.py --ref-free`; the field, without truth, `scripts/dd_frac_reref.py` against
  its null). A grade built on one reference element reports that element's share or differences it
  away. The goldens stay on `trace_dd_fractional_ref: lowest` (manifest exception, continuity).
- **Wrapped-fraction nulls.** Subtracting a group's own circular median shrinks |residual| about
  42 % on uniform noise: quote the gap to a size-matched null. A triangle-closure null carries the
  DD structure. A truth-free wrapped grade means something only for sub-cycle errors
  (`docs/cards/sim-bed/DD-TRACE-REF-SAT.md`).
- **`network_ionosphere.csv`:** grade `iono_total_m` (= `x_lin + x_r`, absolute slant); `iono_m`
  is the correction `x_r` to the allocation seed.
- **An `x_lin` shift on a carried column is an exact solve no-op** (the carried RHS absorbs it): a
  mechanism whose only write is `x_lin` cannot actuate (`docs/cards/slip/LANE-SNAP-RUNG3.md`).
- **Seams (an add-back):** grade with a cold-consumer replay (`scripts/splice_wire.py`), never the
  seam-epoch diff alone; it cannot see an integrated, unsignalled drift.

### Retired network rules (full pre-2026-10-13 text: `git show 8326da3:CLAUDE.md`)

- Card-4 `carry_invariant` lineage (`emit_pb_filter_gate_v2`, `emit_pb_filter_jump_k` 8.0,
  `raim_carry_invariant`, the `trace_station_residuals` zero-row trap): float vehicle, cards RF-1,
  RF-2b, GRB; ledger rows (the `emit_pb_filter*` rows left with their keys, card RKD:
  `git show 4bdd693:docs/CONFIG_promotion_ledger.json`).
- MW/WL lane layer cost and retention (AGU-26, 63 and 305 stations): card AB3; the `lane_carry.*`
  and `mw_wl.*` keys were deleted by card RKD (rows: `git show 4bdd693:docs/CONFIG_promotion_ledger.json`).
- Lane slip responses (`relevel_retap_*`, `relevel_lane_snap`, lane-preserving refloat):
  `docs/cards/slip/`; the keys were deleted by card RKD (rows at `4bdd693`, as above).
- SH and GIM ionosphere level (GIM mean series, DEFECT 2, rx-CB carry memory, SH gate release):
  `docs/cards/ionosphere/LV-CORRECTION-AND-12H.md`, card AB1.
- Pre-golden armed set (the Jul-28 guard list, `scripts/p2_make_guard_configs.py`,
  `cb_excursion_reinit` chains, `raim_we_code_iono`): the goldens and the ledger.
- Clock-datum candidates (`clock_datum_broadcast_anchored`, `clock_datum_sum_to_zero`, pivot
  election): ledger rows; pivot election is a queued build (`docs/status/OPEN_ITEMS.md`).
- Wind-up accumulation (fractional census, inverted horizon, restart response): card WUP,
  `docs/cards/slip/WINDUP-FRACTIONAL-CENSUS.md`.
- Re-admission edge (`readmit_seed_horizon_s` 3900 pinned; its KEEP_OFF siblings):
  `docs/cards/network-vehicle/READMIT-EDGE.md`.
- Golden re-bases 2026-09-15 to 2026-10-01 (RF-3-prep, GRB, RF-3, PBF, WUP, SFR, EXL; baselines
  `_rf0hv9` to `_rf0hv14`): the cards in `docs/refound/CARDS.md`.

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

- **Test suite.** `gnss_tests.exe` runs 2083 GoogleTest cases across 282 suites in 140 files (85 s on an idle box, ~205 s under load; full Debug run under VS 2026, measured 2082 passed + 1 skip on 2026-09-14 after card CC (the consumer's declared-step carry + warm start); the 2073/281/139 figure of 2026-09-13 covered the RS-7..RS-10b warm-resume cards, REF-FAN, the D5-v5 field card and the RAIM-pool re-host gates — the earlier 1540/215/104 and 2013/275/135 figures predate them; one fixture-gated skip — `FixtureSmokeTest.Sp3HeaderTimeConversionMatches`, which needs `bootstrap_internal_data.py` to have run) — math core, site displacement, atmosphere models, antenna/ATX, filter core, transforms/estimation, RTCM/SBF decoders, ILS/MLAMBDA, state-key schema, the network estimator + normal equations + invariant-carry driver + product I/O, cycle-slip/TurboEdit, the RTS smoother, aiding, and real-time seams. `tests/sim/` is a **separate** target, `gnss_simulator_selftest` (80 cases), not linked into `gnss_tests`. The suite guards structure and non-regression (new work is developed default-OFF / byte-identical). It does NOT validate end-to-end estimation accuracy — confidence for numerical changes still comes from a clean build + field runs graded on datum-invariant quantities (see Standing invariants) + trace inspection.
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

`src/core/app/ApplicationNetwork.cpp` (`runNetwork`) holds setup and the epoch source (351 lines after card NOM3 step EP4, 2026-10-15; `runNetwork` is 153 of them: the setup calls, an ordered list of builders, and the epoch source). One processed epoch is `Application::networkProcessEpoch_(NetworkRun&, const SyncedEpoch&)` in `src/core/app/ApplicationNetworkEpoch.cpp` (624 lines after card NOM3 steps EP2 and EP3, 2026-10-15): a spine of seam calls inside `networkSubBegin(run, SB_X)` / `networkSubEnd(run, SB_X)` brackets, opened by the admission (`app/NetworkEpochAdmission`: `networkFastForward` before the injector, `networkAdmitEpoch` after the roster, one `EpochAdmission` verdict, Process, Skip or Stop). `NetworkRun` (`app/NetworkRun.hpp`) owns the run's engines and groups; `NetworkEpoch` (`app/NetworkEpoch.hpp`) holds the per-epoch values, grouped by the stage that writes them. **Keep it thin** — new network-producer code goes in the seam matching its concern, NOT inline in the driver:

- **New per-epoch estimation/QC step** → the matching `filter/Network*` seam (or a new one) called from the loop. Datum/solve → `NetworkSbasis` / `NetworkChainDatum` / `NetworkPhaseDatum` (the epoch's election: `NetworkPhaseAnchorEpoch`) / `NetworkSolveGlue` / `NetworkDdPriorBuild` / `NetworkInstallCarry` / `NetworkProducerAr`; post-fit QC → `NetworkRaim` / `NetworkClockQc` / `NetworkRelevel`; slip → `slip/NetworkSlipQc` (the arc-resumption census: `slip/NetworkGapResumeCensus`); coupling → `NetworkCoupling`. Observation-graph components, spanning forests, cycle bases, the integer map between bases and fixed-cycle lattices → `filter/NetworkArcGraph` (card AGT; no new union-find or tree walk elsewhere); a per-band arc life stamp → `filter/NetworkArcLife`; fixed integer cycles folded into a detached held solve → `filter/NetworkFixedCycles` (card FCF, default off, measure-only; its algebra in `NetworkNormalEquationsDetached.cpp`).
- **New slip-funnel producer or reader** (the epoch's set of slipped (station, ambiguity) arcs that the carried quantities' slip responses read) → `filter/NetworkSlipFunnel`: its header lists the producers and readers and the standing invariants 1 and 7 the funnel serves (what is enforced, what is named and not). A producer takes the funnel as an output parameter and joins that list and the `funnel` row of `app/NetworkEpoch.hpp`; a reader binds it const through `ep`.
- **Inside the solve** (`networkSolveGlue`, card NOM3 step SG) → the solve is a fixed list of 14 stages; the table, its homes and its order constraints are the banner of `filter/NetworkSolveGlue.hpp`. A new pre-gate or post-gate fold goes into `networkPreGatePriors` / `networkPostGatePriors` at its place in that order; a new within-epoch (T2) response class into `filter/NetworkRaimWithinEpoch`; a new way to end the epoch is a `SolveSkip` row of `kSolveSkips` (never an inline return with its own bookkeeping). Inside the solve a stage reaches `ws`, `est_index_of` and `dd_build` only through its parameters, never through `ep` (one access path, card WCF). The stage text is generated (`python scripts/nom_sg_cut.py --check`) and verified (`python scripts/nom_sg_verify.py`, `--controls`).
- **Which signal stands for a band, and which arcs a (station, satellite) has** → ask the roster and the arc ledger, never a new rule: `signalRosterModelledOr` (`app/SignalRosterSites.cpp`; the roster's Modelled role, the latch only at a constellation's first epoch) and `filter/ArcLedger` (the wind-up restart's carry targets are named by it). A new band-role or signal-choice site gets a `RosterN3bSite` counter first and reads the roster only at 0 on every arm. The open choices (pin pair, channel family, canonical pair, the signal for a band) are the decision list `scripts/nom3_designs/N3b_decisions.md` (card NOM step N3b).
- **New emit/store behavior** → `NetworkSolPrep` (pre-emit prep) or `NetworkEmitStore` (emit / product-write / closed-loop consume).
- **New diagnostic/trace sweep** → `NetworkIonoDiag` / `NetworkDdFrac` / `NetworkShDevVtec` / `NetworkShHealth` / `NetworkLeveling` / `NetworkStashStatus`, or a new diag seam. Trace-stream ownership + run-summary/manifest writers live in `logging/NetworkRunTraces` (NOT the driver).
- **New measure-only per-epoch instrument** (an observer: `void networkX(NetworkRun&, const NetworkEpoch&)` that writes only its own counters, trace rows, events and manifest records (and the epoch-status values `networkStashStatus` reads, named in the header's FrameDiag note), gated on its first line after its bindings) → one row of `kNetworkEpochObservers` (`filter/NetworkEpochObservers.hpp`, whose header states the six slots, why each observer sits at its slot, and the order constraints within a slot; `tests/test_network_epoch_observers.cpp` pins every row), in the slot where its inputs are final. The spine walks a slot with `networkObserveEpoch(run, ep, NetworkObserverSlot::X)`; never a new direct spine call (card NOM3 step EP3, 2026-10-15: 21 rows in six slots; `ApplicationNetworkEpoch.cpp` 624 lines; the observers' three remaining driver gates moved into their seams, so the RAIM gate is the one `if` left around a spine call).
- **New run-end telemetry / readout** → the subsystem's own run-end report, `int networkXxxRunEnd(const NetworkRunEndView&)` in the subsystem's `.cpp` (or a `<Subsystem>RunEnd.cpp` beside it when its state lives in one of the large files), registered as one row of `kNetworkRunEndReports` (`filter/NetworkRunEnd.hpp`, whose header states the order constraints). `filter/NetworkTeardown` only walks the table (card NOM step TD1b, 2026-10-09: the 7,277-line run-end function became 107 owned reports). Never add lines to `NetworkTeardown.cpp`.
  One report per subsystem (card NOM2 step TD2): a new line goes in its subsystem's report, or in a new `static` section of it listed by `networkRunEndSections`; a subsystem with no row yet gets one row. The walk prints a `Network[run-end] <row name>` heading ahead of each row that prints.
- **New carried/persistent state, counters, or config snapshots** → `app/NetworkRunContext` (`NetworkCarryState` / `NetworkServiceCounters` / `NetworkKnobs`), aliased in the driver and passed to the seams — do NOT add a bare function-local (that is precisely what INC-12 spent a pass deleting: 191 dead aliases left behind by relocated blocks).
- **New cross-epoch per-station state** → the Station entity (`app/NetworkEntities.hpp`: a field of `net::StationState`, or of `net::StationSignalState` for state keyed below the station), with a `FieldDecl` row in `kStationFields` (`src/core/app/NetworkEntities.cpp`) declaring its persistence; never a new station-keyed member of `NetworkCarryState` (card NOM step N4a). Access goes through the free functions of that header (`net::get` / `put` / `drop` / `forEach`, `byStationName` for a name-keyed walk); `python scripts/nom_n4_verify.py --entity station` checks the moves.
- **New per-epoch value a later stage reads** → a member of `NetworkEpoch` in the group of the stage that writes it, with that group's writer list in the header updated; a value that must survive the epoch is run state (`NetworkRunContext`), never an epoch member.
- **New run-loop-local helper type a seam must name** → `filter/NetworkRunTypes` (general) or `filter/NetworkWlRunTypes` (section timing).
- **Setup** (validation / sinks / consumer) → `app/ApplicationNetworkSetup`; **assembly** (PASS A/B) → `app/ApplicationNetworkAssembly`.
- **New station row or carried-prior family (PASS B's parallel produce)** → the family's file under `filter/` (`NetworkStationSplit`, `NetworkPhaseDatum`, `NetworkStationAnchors`, `NetworkIonoConstraintRows`, `NetworkSdEwlAr`, `NetworkRaim`, `NetworkStationCarry`, `NetworkLevelingCarry`, `NetworkSlantIonoCarry`, `NetworkRxClockCarry`, `NetworkRxCodeBiasCarry`, `NetworkPhaseColourCarry`, `NetworkZwdCarry`, `NetworkOraclePin`, `NetworkCoupling`, `NetworkCongruence`), as one row of `kStationBuildSteps` or `kStationCarrySteps` (`filter/NetworkStationBuild.hpp`, card NOM3 step PB1b; its banner states the parallel contract and the orders a step may not change, `tests/test_network_station_build.cpp` pins them). A step takes `StationBuild&` (the station and its slot writable, the run and every other station through const references) or the carried-prior frame `StationCarryFrame&`; never a writable `NetworkRun&` inside the parallel produce, and never a write through a pointer member of the view other than the station's own. `ApplicationNetworkAssembly.cpp` keeps PASS A, the prefilter, the column registration and the `networkPassB_` orchestrator (1,671 lines after step PB1b, 2026-10-15; the six carry families' harvest halves are step HV1's).
- **New harvest of a carried kind** (the post-solve read of a station-local carry, its stash and its prune) → a function `void networkHarvestX(NetworkRun& run, const NetworkEpoch& ep[, HarvestBorder&, the layout])` in the harvest half of the kind's family file (the files of its PASS B steps; the AIL trace in `filter/NetworkAmbStateTrace`, the eq34 receiver phase bias in `filter/NetworkChainDatum`, the post-resume census in `app/NetworkWarmResume`), called in its place from the orchestrator `networkHarvestLocalCarries(run, ep)` (`app/NetworkLocalCarryHarvest.hpp`: the table of contents and the seven order constraints; card NOM3 step HV1, 2026-10-15: the 1,871-line function became an orchestrator and 21 functions, the spine file 390 lines). A carried column's regression goes through `networkHarvestColumn`; a row of the F3 block goes to `StationHarvestBlock::segmentBlock(BlockSegment::X)` at `rowOf(X)`, before `networkHarvestBorderEnd`. Never a new per-station hand-forward vector or a bare alias of the harvest's former 17 parameters.
- **New setup step** (an engine's or the run's configuration before the first epoch) → a builder `void networkX(NetworkRun& run[, kept...])` in the file of the subsystem that consumes what it builds (a `<Concern>Setup.cpp` beside it when that file is large or shared), Doxygen at its declaration, called once from `runNetwork` in its place in the order (card NOM3 step EP4; `scripts/nom3_designs/EP4_design.md` section 2 states the order constraints). Never inline in `runNetwork`.

Seam contract (card NOM3 step EP1): a per-epoch seam is a free function (or one of the five `Application` members) `R networkX(NetworkRun& run, const NetworkEpoch& ep[, kept...][, outputs...])`. It owns no state. It opens with one binding per name it uses (reads through `ep`, writes through its output parameters, marked `// written`) and below the bindings never spells `ep.`. An output is an epoch group the seam is listed as a writer of in `app/NetworkEpoch.hpp`; after the solve the only outputs are `ep.solve.handoff` and `ep.prep.sol`. A measure-only seam has the observer form `void(NetworkRun&, const NetworkEpoch&)`; a seam with another caller (a test, another seam) keeps its explicit form beside a one-line epoch-form adapter. A seam's arming gate is its own first statement after its bindings, never an `if` around its spine call (card NOM3 step EP2; the four still in the driver belong to step SG's files). A unit test calls the epoch form through `tests/support/NetworkTestRun.hpp`, never an explicit-parameter overload kept for it. A new or changed epoch writer is a row of `tests/test_network_epoch_contract.cpp`; run `python scripts/nom_epoch_lint.py` before committing a change to a seam signature or to `ApplicationNetworkEpoch.cpp`. New `.cpp` seams need a manual `CMakeLists.txt` entry (no globbing).

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
