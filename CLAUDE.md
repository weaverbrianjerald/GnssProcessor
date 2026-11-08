# GnssProcessor

C++17 GNSS post-processor and real-time engine. Supports PPP and PPP-RTK (uncombined), optional IMU integration, integer ambiguity resolution (MLAMBDA), and a backward RTS smoother. Scenario configs: `config/*.json` (one file = one run; `config/ppp_internal.json` is the PPP starting point). New-contributor onboarding: `docs/onboarding/README.md` + `CONTRIBUTING.md`; worked example on the unmerged reference branch `example/stochastic-model`.

## Build

**Visual Studio 2026 Enterprise (v18) + CMake + Ninja. VS 2022 is RETIRED —
standing user directive, 2026-08-12. Do not invoke the 2022 toolchain even
though it is still installed on disk.**

Use the wrapper; it encodes the toolchain traps (vcvars64 path, two CMakes, `%PATH%` trap, logger defect), recorded in `docs/onboarding/build_toolchain.md`. A bare `cmake --build` fails; `vswhere.exe not recognized` is benign noise.

```powershell
C:\GnssProcessor\scripts\vs2026_build.ps1 -Config x64-Debug -Target gnss_core   # fast compile check
C:\GnssProcessor\scripts\vs2026_build.ps1 -Config x64-Debug                     # full Debug + tests
C:\GnssProcessor\scripts\vs2026_build.ps1 -Config x64-Release                   # every data run
C:\GnssProcessor\scripts\vs2026_build.ps1 -Config x64-Debug -Reconfigure        # after a toolchain change
```

Targets: `gnss_core` (static lib), `gnss_processor` (exe), `gnss_tests` (exe),
`gnss_processor_module` (pybind11 `.pyd`), plus `gnss_simulator_selftest`.

Build tree: `build/x64-Debug` and `build/x64-Release` — both are **git-ignored
and disposable**; regenerating from scratch is one wrapper call (~180 s Debug).

**Disk placement (owner request, 2026-10-13: keep C: free, target D: for builds and runs).** D: is USB-attached and unavailable while disconnected.
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
- Claude session scratchpads (`%TEMP%\claude\...`) move to D: only if the owner sets the documented `CLAUDE_CODE_TMPDIR` variable (new sessions only).

**NEVER run `gnss_processor` on data from the Debug tree** (standing user
directive). Debug's unoptimized Eigen is 50–100× slower — a Release run of
minutes becomes hours. Debug is for compile checks and `gnss_tests` only;
every data run (smoke, field grade, A/B, timing) uses
`build/x64-Release/gnss_processor.exe`. When another session may rebuild
concurrently, run a renamed copy (e.g. `gnss_processor_fable_run.exe`) so the
linker never hits a locked exe.

## Layout

```
include/        All public headers, organized into subdirs mirroring src/core/ — heavy Doxygen is the norm.  Always
                include with the subdir prefix: `#include "filter/KalmanFilter.hpp"` not `#include "KalmanFilter.hpp"`.
                CMake adds `include/` to the include path; the subdir is part of the include string.
  app/          Application.hpp (post-processing + real-time entry points; impls split across Application.cpp /
                ApplicationBaseline.cpp / ApplicationRealtime.cpp)
  atmosphere/   Ionosphere, Troposphere
  common/       Truly cross-cutting types and identifiers only: Constants, Namespaces, GnssTypes, PppTypes, PppState,
                PppObservation, EpochMeasurement, PppUtils, SignalBand
  config/       GnssConfig + GnssConfigTypes (the config enums + POD sub-structs, relocated out of the GnssConfig.hpp
                header; GnssConfig.hpp includes it) + GnssConfigJsonHelpers (loader split into per-section helpers in
                src/core/config/GnssConfigLoad.cpp)
  corrections/  GnssCorrections, OceanTideModel
  filter/       KalmanFilter, StateManager, StateKeySchema, Smoother, AmbiguityResolver, ImuIntegrator, PivotManager
                (PPP-RTK SD), DdPivotManager (baseline DD)
  logging/      LogManager, TraceLogger, KalmanFilterLogger, WlsLogger
  math/         Coordinates, TimeUtils, Interpolate, WlsEstimator
  measurement/  MeasurementModel, StateInitializer, BssdTransform, BrsdTransform (BSSD: PPP-RTK SD via
                BssdTransform::apply, baseline DD via BssdTransform::differenceBrsd composed on BrsdTransform output)
  model/        PppModel (also hosts uncombined PPP-RTK, composed with measurement/BssdTransform), StochasticModel,
                Antenna, Planetary, ModelGeometry
  products/     ProductManager, ProductCache, ProductParser, PathResolver, ProductResolver, ProductTypes, DataDownloader
  realtime/     NtripClient, RtcmStreamReader, RealTimeEngine
  rinex/        RinexParser, MultiRinexSync
  rtcm/         RtcmParser
  slip/         CycleSlipManager + detectors: MwSlipDetector, GraphicSlipDetector (active); IfPhaseSlipDetector (active
                on the NETWORK path — network.cycle_slip.if_phase_td; STAGED for user/kinematic); GfSlipDetector (STAGED
                — uncompiled, not wired in; see its header banner); IonoVariabilityMonitor
  network/      The network producer, one folder per concern, mirrored under src/core/network/ (tests: tests/network/).
                File basenames drop the Network prefix (class NetworkEstimator is in network/solve/Estimator.hpp);
                include with the full prefix: `#include "network/qc/Raim.hpp"`.  The wire I/O, emitter and state
                snapshot (products/NetworkProductIO, NetworkProductEmitter, NetworkStateSnapshot) stay in products/:
                consumers and the real-time engine include them.
    driver/     ApplicationNetwork*, Run, Epoch (+ admission, observers), Knobs, run-type headers, stream front end
    state/      RunContext, Entities, RunFields, snapshot write, warm resume, border stash and prior, carry sites and
                disposition, ArcLedger
    assembly/   station build, split, anchors and carry; contributions; parameter inventory and registry; responses;
                admissions; band scopes; SignalRoster
    carry/      the carried families (leveling, phase colour, slant iono, rx clock, rx code bias, ZWD), harvest,
                install, invariant-carry driver, DD prior, injector, coupling, shared history, carry VCE
    solve/      solve glue and stations, estimator, normal equations, rank gate, datum (clock, code bias, phase, chain,
                S-basis, anchors)
    qc/         RAIM (+ within-epoch), clock QC, gross-LOS, prefit and redundancy census, Baarda, congruence, MDB,
                relevel
    slip/       slip QC and funnel, oracle and pin, gap-resume census, raw observables, common-band scope, the PASS A
                scan and drain stages
    ar/         arc graph and life, cycle basis, lattice, suspect, fixed cycles, producer AR, SD-EWL (rows and row
                stages), float vehicle, z-store
    emit/       solution prep, emit and store, ledger emit, broadcast wire, PB hold, emit rebase
    diag/       trace sweeps and diagnostics, run traces, station block
    runend/     the run-end report table, its walker and the driver-level reports

src/core/       Implementation files mirror the include/ subdir layout one-to-one (app/, common/, config/, logging/,
                math/, products/, slip/, network/, etc.).  User-edited scenario configs live in the top-level config/
                dir, not here.

tests/          GoogleTest unit suite in `gnss_tests.exe` (network tests in tests/network/, shared harness in
                tests/support/); tests/sim/ is a SEPARATE target, `gnss_simulator_selftest`. docs/test_suite.md
                documents the real-data truth-graded harness, NOT this unit suite.
logs/           Runtime CSV traces + spdlog files.  Hard-coded `./logs` relative to the process CWD
                (`Application::log_dir_`); git-ignored, recreated by the next run.
runs/           Campaign run outputs (the git-ignored default output dir). Finished runs live on
                D:\GnssProcessorSupport\runs\ under the same names, so a cited `runs/<name>` resolves there
                (D:\GnssProcessorSupport\MANIFEST.md maps every move). Runs whose newest file predates 2026-09-02
                were deleted on 2026-11-02, with the old archive D:\GnssProcessor_archive\: a run cited from
                before September is regenerated, not looked up (history: docs/onboarding/build_toolchain.md).

scripts/        One-off Python utilities (bootstrap_internal_data.py populates data/ for internal-mode runs).
data/           Local cache for internal-mode test fixtures. Empty in git (.gitignored except for README.md shells);
                populated by the bootstrap script in the date-first layout: data/<YYYY-DOY>/{products,obs/<station>}/,
                data/static/{atx,nc}/, data/cache/<YYYY-DOY>/ (see "File management" below).
```

Anything that genuinely belongs to *every* TU lives in `common/`; everything else
got pulled into a category dir.  When adding a new file, prefer the most
specific subdir that fits — resist the urge to drop new headers into `common/`.

## Documentation — the topic tree (standing user directive, 2026-09-14)

**`docs/INDEX.md` is the single entry point, and `docs/refound/` is the current program.** The author's directive: *"We need a method to organize our whole project, rather than simply appending to these massive files."*

The rules — **all seven are enforced or checked**:

1. **One card = one file** under `docs/cards/<topic>/`. Pre-register it as a file.
2. **Results are appended to the CARD's file only** — never to a topic page, never to
   `docs/archive/`, never to a monolith.
3. **The INDEX line is updated twice:** at pre-registration, and at close.
4. **No non-archive file over ~60 KB.** Split by topic as one approaches it. The one
   exemption is the generated ledger view `CONFIG_promotion_ledger.md`, named in
   `SIZE_EXEMPT` in `scripts/docs_index_check.py`.
5. **Every card file starts with the STATUS block** (`id`, `topic`, `status`, `summary`, `source`) and an **Outcome** paragraph (about 90 words); `status` ∈ {`open`, `closed`, `parked`, `superseded`, `reference`}. The header is revised as results land; the body never is (field details: `docs/INDEX.md`, rule 5).
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

⚠ **The config ledger is `docs/CONFIG_promotion_ledger.json`** (one verdict per config key; `docs/topics/config-ledger.md`). A ledger change is a JSON edit, a re-render and the two verifies; a new knob gets its row in the same commit.

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
declared disposition in `network/assembly/ParamInventory.hpp` (the "parameters as armed" table
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
config/golden/golden_field_bds_refound.json # third lineage: the field golden + BeiDou B1I/B3I (card BDG)
python scripts/make_golden_config.py --verify   # at session start and after ANY ledger change
```

Posture, one on two beds: GPS L1/L2 + Galileo E1/E5a, dual frequency; self-contained (satellite
GF code bias pinned at broadcast T_GD, no GIM, no Kaula prior); free per-LOS slant ionosphere (no
SH); float vehicle (`ambiguity_vehicle: float_kf` + `phase_datum_held_basis`) on the
phase-leveling carry with gauge projection, the prioritized phase datum and the exact coupling
update; Q3 clock anchor and clock step test; phase-bias hold; gross-LOS screen with its 12 m cold
bound; accumulated wind-up with restart refloat. Field golden only: ocean tide loading from the SIF
registry and the GPS IIF / IIR noon-turn attitude (the sim fixture carries neither). No lanes, F3,
SH constraint or seams.

- **The BeiDou golden is a separate lineage** (card BDG, 2026-11-02: the author's course B of card BDSB): generated
  from the field golden's output plus `processing.active_systems` with BeiDou, its B1I / B3I priorities,
  `network.second_band_admission` and `network.n3c_wire_per_signal`. The GPS + Galileo goldens stay the primary posture
  and the base of every arm; the sim bed has no BeiDou. Its baseline is card BDSB's arm `runs_nom/_rap_bdsc_c6`.
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
  `emit_clock_disc`, the CB jump test with `mw_detect`, `cold_clock_seed`, the estimable block-Baarda
  census (`block_baarda_estimable`, the one check that reports code multipath) and the readmit-seed
  mirrors; `parallel_carry` true, with the seven identical-output stages of card PES
  (`parallel_station_normals`, `parallel_schur_columns`, `parallel_redundancy_census`, `parallel_carry_harvest`,
  `parallel_iono_traces`, `parallel_coupling_prepare`, `sbasis_values_first`).
  Response doctrine: adapt and EMIT within the detected epoch; a
  withhold is the backstop, graded as an adaptation failure unless the epoch is provably
  unrecoverable. Not armed: `prefit_fuse_sigma`. Deleted from the code on 2026-10-14 (card RKD,
  `docs/refound/RKD_retired_key_deletion_2026-10-14.md`): `sh_health_gate`, the old lane layer
  (`mw_wl.*`, `lane_carry.*`; lanes return only as card AB3's new build) and `emit_pb_filter*`.

**Baseline: card BOF (2026-10-28, `docs/refound/BOF_built_off_follow_through_2026-10-28.md`), after card MRF, on card GAD's Galileo posture (2026-10-13).** Pair `runs/_rf0hv17_{field,sim}6` (exe `gnss_processor_bofarm.exe`, sha256[:16] 91143c81de9a9547): field WTZZ00DEU 0.059 0.012 0.023 0.012 0.017 0.012 m by hour; sim products identical to `_rf0hv16_sim6`. The armed keys of cards GAL2, GFC, MRF and BOF, the earlier pairs (`_rf0hv16`, `_rf0hv15`), the census against GPS only and the open items: the cards, and the paragraph at full length in `docs/onboarding/claude_md_history.md`. ⚠ Every golden arm is GPS + Galileo and 2 to 6 times slower. Constellation-blind graders pool the systems: `rapid_arm.py score` counts E5a as band 5, `ftc_step_attrib.py` R3 needs `--systems G,E`, and `census6h.py` / `simcensus6h.py` default to GPS-only bases (use `gcensus.py`, or `SIMCENSUS_BASE` = the wire's arm).

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

Retired network rules (Card-4 `carry_invariant`, lanes, SH and GIM ionosphere level, pre-golden armed set, clock-datum candidates, wind-up accumulation, re-admission edge): records in `docs/onboarding/claude_md_history.md`; pre-2026-10-13 text: `git show 8326da3:CLAUDE.md`.

## File management (RINEX, products, downloads)

Input-file location goes through one class, `PathResolver` (`include/products/PathResolver.hpp`; new file types plug in there, not into the call sites), driven by the top-level config field `data_source`: `"internal"` globs the bundled `data/` tree with no network access, `"production"` delegates to `ProductResolver`, which downloads missing products from CDDIS. Both modes share one date-first layout (`<root>/<YYYY-DOY>/{products,obs/<station>}`, `static/{atx,nc}`, `cache/<YYYY-DOY>/`).

**Date selection (internal mode)**, in order: (1) the YYYYDOY embedded in `files.observation_path` / `baseline.{reference,rovers}.rinex_path` if pinned, (2) `data_download.{year, doy}`, (3) the earliest `<YYYY-DOY>/` subdir under `<root>`. **Path precedence:** explicit JSON paths always win; an empty string in `files.observation_path`, `files.navigation_path`, `baseline.reference.rinex_path` or any rover's `rinex_path` is the signal that PathResolver fills it in. Layout tree, CDDIS authentication, crx2rnx, baseline fixture presets and `.gz` cleanup: `docs/onboarding/data_files.md`.

## Known state

- **Test suite.** Debug `gnss_tests.exe`: 3,842 cases in 527 suites, 3,809 passed, 33 skipped (fixture-gated), 0 failed (measured 2026-11-07, card PES section 8); `gnss_simulator_selftest` 99 of 99 (measured 2026-11-02, card BDG gate G5); re-measure with the Debug build. It guards structure and non-regression (new work is developed default-OFF / byte-identical), NOT end-to-end estimation accuracy: confidence for numerical changes comes from a clean build + field runs graded on datum-invariant quantities + trace inspection. Every suite name is owned by exactly one file; prefer a new file over widening one.

## Entry points worth knowing

`Application::run()` (`src/core/app/Application.cpp`) is the post-processing pipeline and its per-epoch loop is where new predict/update logic is wired; `runRealTime()` is the streaming path (`RealTimeEngine`). Post-processing-only features (e.g. smoother, `gnss_rate_s`) live in `run()` and should guard against `AppMode::RealTime`. New toggles slot into the matching `loadXxxSection_` helper of `src/core/config/GnssConfigLoad.cpp`. Dynamic state-vector keys are built and parsed only through the `make*` / `parse*` helpers of `include/filter/StateKeySchema.hpp`. The RTS smoother's position is degenerate (constant across epochs) in static PPP by construction.

### Network producer — where new code goes (anti-regrowth decision-tree)

`runNetwork` holds setup and the epoch source; one epoch is `Application::networkProcessEpoch_`, a spine of seam calls (`network/driver/ApplicationNetworkEpoch.cpp`); `NetworkRun` (`driver/Run.hpp`) owns the engines and groups, `NetworkEpoch` (`driver/Epoch.hpp`) the per-epoch values grouped by the writing stage. **Keep it thin**: new network-producer code goes in the seam matching its concern, NOT inline in the driver. Paths below are under `network/`; the full rows are in `docs/onboarding/where_new_code_goes.md` (read it before adding network code).

- **Per-epoch estimation/QC step** → the matching seam (or a new one) in its concern folder (Layout), called from the loop; graph components, spanning forests, cycle bases and integer maps → `ar/ArcGraph` only (no new union-find or tree walk elsewhere).
- **Slip-funnel producer or reader** → `slip/SlipFunnel`.
- **Inside the solve** (`networkSolveGlue`, 14 stages; order constraints in the banner of `solve/SolveGlue.hpp`) → `networkPreGatePriors` / `networkPostGatePriors`, `qc/RaimWithinEpoch`; a new way to end the epoch is a `SolveSkip` row of `kSolveSkips`, never an inline return; a stage reaches `ws`, `est_index_of` and `dd_build` only through its parameters, never through `ep`.
- **Which signal stands for a band, which arcs a (station, satellite) has** → the roster (`signalRosterModelledOr`) and `state/ArcLedger`, never a new rule; a new band-role or signal-choice site gets a `RosterN3bSite` counter first.
- **Emit/store** → `emit/SolPrep` or `EmitStore`. **Diagnostic/trace sweep** → `diag/`; trace streams and manifest writers: `diag/RunTraces`, not the driver.
- **Measure-only instrument** (an observer `void networkX(NetworkRun&, const NetworkEpoch&)` writing only its own counters, trace rows, events and manifest records) → one row of `kNetworkEpochObservers` (`driver/EpochObservers.hpp`); never a direct spine call.
- **Run-end telemetry** → the subsystem's own `networkXxxRunEnd`, one row of `kNetworkRunEndReports` (`runend/RunEnd.hpp`); a new line goes in its subsystem's report, never in `runend/Teardown.cpp`.
- **Cross-epoch state** → an entity field with a `FieldDecl` row (`state/Entities.hpp`: station, constellation, line of sight, satellite signal, satellite, arc), else a `NetworkCarryState` member with a `net::kRunFields` row; never an undeclared member, a bare function-local or an entity-keyed `NetworkCarryState` member (`python scripts/nom_n4_verify.py`). Counters and latches nothing in the solve reads → a base of `NetworkServiceCounters`; a warm-resume hand-off → `NetworkCarryState::resume`; config snapshots → `NetworkKnobs`.
- **Station parameter** (a column of every station's local block) → its identity in `common/ParamRef.hpp`, its slot in `state/StationParams`, its term in `measurement/ObservationEquation.cpp` (one equation for the producer, the consumer and the seeds), then the declared treatment and a network key with its ledger row; the recipe is `docs/onboarding/08_adding_a_network_parameter.md`. A station's rows are sparse (`StationDataRows`): never a dense matrix of rows by slots on the network.
- **Per-epoch value a later stage reads** → `NetworkEpoch`, in the writing stage's group; a per-epoch event → `EpochEvents` (`ep.events`); what survives the epoch is run state.
- **Entity-table access** → bind the table in the seam's opening block; each non-const binding has its `net::kEntityWriters` row (`python scripts/nom_epoch_lint.py`, check V12).
- **Setup step** → a builder `networkX(NetworkRun&[, kept...])` in the consuming subsystem's file, called once from `runNetwork`, never inline there (validation, sinks, consumer: `driver/ApplicationNetworkSetup`; assembly: `driver/ApplicationNetworkAssembly`; helper types a seam must name: `driver/RunTypes`, `WlRunTypes`).
- **PASS A scan step** → `passA*` in `slip/PassASlipScan`, a serial drain → `passA*Stage` in `slip/PassASlipDrains`, never in the orchestrator. **Station row or carried-prior family** (PASS B) → one row of `kStationBuildSteps` / `kStationCarrySteps` (`assembly/StationBuild.hpp`); a step never takes a writable `NetworkRun&` inside the parallel produce.
- **Harvest of a carried kind** → `networkHarvestX` in the kind's family file, called from `networkHarvestLocalCarries` (`carry/LocalCarryHarvest.hpp`).

Seam contract: a per-epoch seam is a free function `R networkX(NetworkRun& run, const NetworkEpoch& ep[, kept...][, outputs...])` that owns no state, opens with one binding per name it uses (reads through `ep`, writes through output parameters marked `// written`), never spells `ep.` below the bindings, and keeps its arming gate as its own first statement. A new or changed epoch writer is a row of `tests/network/test_network_epoch_contract.cpp`; run `python scripts/nom_epoch_lint.py` before committing a change to a seam signature or to `ApplicationNetworkEpoch.cpp`.

### Real-time engine — where new code goes (anti-regrowth decision-tree)

`src/core/realtime/RealTimeEngine.cpp` is thin ordered orchestration. **Keep it thin**: new code goes in the seam matching its concern (full text: `docs/onboarding/where_new_code_goes.md`):

- **Per-epoch admission / deferral rule** → `RealTimeEngineAdmit.cpp`: the ONLY seams that may end an epoch (`bool`, false ⇒ `processEpoch` returns false); everything downstream is unconditionally straight-line.
- **Consumer-side observability** → `RealTimeEngineSsrDiag.cpp`. **Slip detection / repair** → `RealTimeEngineSlipQc.cpp`. **Construction step** → `RealTimeEngineInit.cpp` (`seedInitialState` must follow `initProcessingComponents`). **RTCM ingest step** → `RealTimeEngineIngest.cpp` (the obs-buffering loop and ephemeris push stay in the driver on purpose).
- **Measurement-set conditioning** (drop/seed/re-gauge, pre-predict) → `RealTimeEnginePrepare.cpp`; `seedClockFromWls` and `regaugeBssd` are mutually exclusive, so a matrix that arms only one leaves the other uncertified.
- **"The producer moved its datum" reaction** → `RealTimeEngineSsrBreaks.cpp`; order is load-bearing: `reactToGenerationChange` applies the union globally and must precede the per-signal handlers.
- **Update / constraint / AR / trace step** → `RealTimeEngineEstimate.cpp`; pseudo-observation constraints belong after the update and before AR.
- **Pre-update row screen or exclusion** → `RealTimeEngineScreen.cpp` (arithmetic and counters in `realtime/InnovationScreen`); an exclusion is an event at both edges (standing invariant 7): the held-out key is re-tested every epoch, and a persistent phase row that cannot heal is re-floated.

Seam contract: a **private member function** opening with an alias preamble reconstructing the driver's local names; cross-seam per-epoch values travel in `RtEpochContext`, which owns nothing that outlives the epoch: **a value that must survive to the next epoch is a member, and putting it in the context silently cold-starts it every epoch.** Gate: `scripts/rt_seam_gate.ps1` + `scripts/rt_seam_compare.py` (4-arm same-build byte-identity A/B) and a test of its own for a reaction the arms do not drive (`docs/rt_seam_decomposition.md`).
