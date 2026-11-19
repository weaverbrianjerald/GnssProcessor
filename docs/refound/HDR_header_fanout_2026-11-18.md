# HDR: rebuild cost of the widely included headers (2026-11-18)

**STATUS**

- **id:** HDR
- **topic:** build-infra
- **status:** open
- **summary:** A measurement of the include fan-out of `config/GnssConfig.hpp`, `config/GnssConfigTypes.hpp`, `network/state/RunContext.hpp`, `network/state/Entities.hpp` and `network/driver/RunTypes.hpp`, with a split design and an include-only step list; no build, engine run or test run was made. The rebuild counts of today's tree are include-graph counts that the check reproduced within one unit; every count after a proposed change is a simulation on the include graph, not a build, and every compile-second figure is unconfirmed. The check corrected where three config structs live (the ring-fix, closed-loop and slip-QC structs are members of `NetworkConfig`), one table row and one decision that rested on it, and the cost of the counter-leaf step.
- **source:** plan package 13 (measurement and design), the rebuild cost of the widely included headers; the draft read commit `03dfcbc` of branch `chain/audit` (committed files only); the check read `7fb0737` for network code and main `5ddc422` for user-engine code, with `03dfcbc` through `git show`. Written by an agent team in the night of 2026-11-18 and checked against the code by an independent agent; the corrections are the last section.

**Outcome.** Two separate costs exist. Config headers: an edit rebuilds about 557 to 567 of about 721 translation units (264 to 266 of about 334 in `gnss_core`), because `GnssConfig` holds every concern struct by value with inline getters; Run-state headers: `RunContext.hpp` pulls in 216 headers, so an edit to almost any network header rebuilds about 217 units. The order stands: counter leaves first (a simulated gain; tally types move with their counters, and nested `NetworkSbasis` types block a verbatim move), then the network configuration behind an indirection after an author decision.

Read at commit `03dfcbc`. Every `path:line` below is at that commit. Files with uncommitted edits in the worktree (among them both config headers, `RunContext.hpp`, `Entities.hpp`, `network/ar/RingFix.hpp` and `CMakeLists.txt`) were read with `git show 03dfcbc:<path>`. No build, engine run or test run was made; compile seconds come from an existing log, and every saving after a change is a simulation on the include graph.

## 1. Method and its limits

- Graph: quoted `#include` lines over `include/`, `src/` and `tests/`, resolved against `include/` and reverse-closed to translation units (TUs). Angle-bracket includes, conditional includes and macros are not in the graph.
- [corrected 2026-11-19: see Corrections, item 11] Totals: 721 TUs (334 in `gnss_core`, 370 test files, plus `main`, the simulator and tools). Two independent graphs agree to within 7 TUs on every closure; the difference is how `main`, the simulator and tools were counted (inferred).
- Compile seconds: latest entry per object in `D:/GnssProcessorSupport/worktrees/chain/build/x64-Release/.ninja_log`, recorded under parallel load and possibly mixing builds. They rank TUs; they are not absolute costs. The review stage did not check them: every figure in seconds is marked unconfirmed.
- No precompiled header, no unity build and no `/GL` or LTCG: `CMakeLists.txt` has no `target_precompile_headers` or `UNITY` setting, and lines 45-46 add only `/O2 /Zi`. `gnss_core` is a static library (`CMakeLists.txt:65`).

## 2. Include closure

| Header | Lines | Size | Direct includers | TUs rebuilt by one edit | of which in `gnss_core` | Core compile-s rebuilt (unconfirmed) |
|---|---|---|---|---|---|---|
| `config/GnssConfig.hpp` | 10 586 | 757 KB | [corrected 2026-11-19: see Corrections, item 7] 395 files (49 headers, 342 TUs) | 557 | 264 of 334 | 10 381 of 12 376 (84 %) |
| `config/GnssConfigTypes.hpp` | 6 130 | 484 KB | 35 files (5 headers) | 567 | 266 | 10 403 (84 %) |
| `network/state/RunContext.hpp` | 3 009 | | 216 files (3 headers) | 217 | 141 | 8 063 (65 %) |
| `network/state/Entities.hpp` | 2 542 | | 53 files | 268 | 148 | 8 271 |
| `network/driver/RunTypes.hpp` | 2 351 | | 180 files (60 headers) | 294 | 151 | 8 369 |

Confirmed by the review stage: every direct-includer count, and the 342 TUs that include `GnssConfig.hpp` directly (140 under `src`, 202 under `tests`). The RunTypes header count is 60, not the 66 one stage reported.

### 2.1 Why the config headers reach so far

- `GnssConfig.hpp:4` includes `GnssConfigTypes.hpp`, and `GnssConfig.hpp:6` includes `logging/LogManager.hpp`, which brings `spdlog.h` and two sink headers to every includer. `GnssConfigTypes.hpp:13` includes the same header for `LogLevel`, used once (`:6127`).
- 215 TUs reach `GnssConfig.hpp` only through other headers. The widest carriers: `rinex/RinexParser.hpp` (422 TUs), `products/ProductParser.hpp` (401), `atmosphere/Troposphere.hpp` (384), `model/ModelGeometry.hpp` (380), `model/Antenna.hpp` (379), `model/StochasticModel.hpp` and `atmosphere/Ionosphere.hpp` (377), `model/PppModel.hpp` (375), `slip/MwSlipDetector.hpp` (359). The network headers inherit it through `model/PppModel.hpp` (`RunTypes.hpp`, `Entities.hpp:78`) and through `rinex/MultiRinexSync.hpp`.
- Of the 49 headers that include `GnssConfig.hpp`: 35 use only `const GnssConfig&` (for example `RinexParser.hpp:10`, used at `:166` and `:405`); 9 use only names from `GnssConfigTypes.hpp`; 5 need the complete class (`app/Application.hpp:1464`, `network/driver/SetupResult.hpp:169`, `rinex/MultiRinexSync.hpp:206` and `sim/EphemerisProvider.hpp:538` hold it by value; `realtime/RealTimeEngine.hpp` calls getters inline at `:436`, `:576`, `:716`). Confirmed by the review stage.
- The large rebuild is required for correctness, not a timestamp effect: `GnssConfig` holds every concern struct by value (`GnssConfig.hpp:10550-10583`, `network_config_` at `:10559`) and its 554 getters are inline, so an added member moves offsets compiled into every caller. Only comment edits and private-declaration edits rebuild without need.
- [corrected 2026-11-19: see Corrections, item 10] Network content is about 74 % of each header: `NetworkConfig` alone is `GnssConfigTypes.hpp:2088-3970` (186 KB, about 238 members); 341 of the 554 getters are network getters, 283 of them simply `return network_X_;`, over 308 distinct `network_*_` members. Network keys therefore have two homes: a `GnssConfig` member with a getter, and a `NetworkConfig` member read through `getNetworkConfig()` (`GnssConfig.hpp:9452`). No name collides between the two lists (review stage), so a merge has no clash.

### 2.2 Why the run-state headers reach so far

- [corrected 2026-11-19: see Corrections, item 6] `RunContext.hpp:31-130` holds 100 project includes that close over 216 headers (about 6.5 MB of header text before Eigen and the standard library). Lines 77-130 (54 includes) are commented as needed for one `XxxCounters` base each, and `NetworkServiceCounters` (`:2467-2557`) inherits about 95 such bases.
- [corrected 2026-11-19: see Corrections, item 5] Because of that closure, 123 of the 170 network headers reach at least 130 core TUs: editing `network/qc/Raim.hpp` or `network/ar/RingFix.hpp` rebuilds 141 core TUs.
- [corrected 2026-11-19: see Corrections, item 4] `RunContext.hpp:59`, `:62`, `:63`, `:64` include whole headers only for members held by `std::unique_ptr` (`:2306`, `:1716`, `:1726`, `:1732`). `RingFix.hpp` was edited 29 times in three weeks; the include at `:64` exists for `std::unique_ptr<NetworkRingFix> ring_fix` alone.
- Counters-only includes bring heavy chains: `RunContext.hpp:83` reaches `realtime/RealTimeEngine.hpp` (1 660 lines) through `ar/SdEwlAr.hpp` and `driver/SetupResult.hpp`; `RunContext.hpp:126` reaches `rinex/RinexParser.hpp` through `ar/ProducerAr.hpp:56`.
- Includers use little of it: per TU a median of 3 distinct knobs and 4 counters; 9 includers use no context member at all and include it as an umbrella (textual count, may undercount aliased bindings).
- `Entities.hpp:76` includes `RunTypes.hpp` for seven value records; `Entities.hpp:78` includes `model/PppModel.hpp` only for `PppModel::CanonicalSignal` (`:793`), which, being nested, cannot be forward-declared. Its declaration tables (about `:2451-2498`) serve 4 TUs.
- `RunTypes.hpp` holds six groups (station roster, carried-prior records, relevel state, anchor handover, clock oscillator and anchor, epoch workspace and status). 12 of its 60 including headers use none of its types in code; `RunTypes.hpp:342` `rowContextOf(const GnssConfig&, ...)` is inline. Even the config loader reaches it (`GnssConfigLoadNetwork.cpp:51` through `qc/PrefitCensus.hpp:100`).
- Compile time follows `RunContext.hpp`, not the config headers (unconfirmed seconds): the 141 core TUs that reach it take a median of 55 s each; the 122 that reach `GnssConfig.hpp` but not `RunContext.hpp` take a median of 16 s.

## 3. Rebuild share of the common edits


| # | Edit kind (commits in 3 weeks) | Needed | Today | After S1 (forward declarations, type headers) | After S4/C (network config boxed, network keys moved) | After E (counter leaves) |
|---|---|---|---|---|---|---|
| 1 | Ring-fix key, a member of `NetworkRingFixConfig` (23) | at most 28 TUs | 567 | 567 | about 257 (112 core, 145 tests); at most 28 only if the ring-fix struct gets its own indirection (inferred) | no change |
| 2 | Network key as `GnssConfig` member plus getter (18) | loader, `Knobs.cpp`, one test | 557 | about 519 | becomes kind 3 once the convention changes: about 257; 163 core if `NetworkKnobs` also changes | no change |
| 3 | Network key as `NetworkConfig` member, no getter (about 10) | 2 to 22 TUs | 567 | 567 | about 257 | no change |
| 4 | Comment, Doxygen or link edit only (about 10) | 0 | 557 to 567 | 519 to 567 | the includers of the edited concern header | no change |
| 5 | User-side key as `GnssConfig` member plus getter (6) | 2 or 3 TUs | 557 | about 519 | 557 (not covered: only network config is boxed) | no change |
| 6 | [corrected 2026-11-19: see Corrections, item 1] User or QC key in a sub-struct other than `NetworkConfig` (about 7: closed-loop 3, slip QC 2, real time 2, cycle slip 1) | 3 to 5 TUs | 567 | 567 | 567 unless that struct is also boxed; then its concern header's includers (inferred) | no change |
| 7 | `GnssConfig` getter forwarding a `NetworkConfig` member (2) | 3 TUs | 567 | 567 | getter removed, becomes kind 3 | no change |
| 8 | Default or meaning changed in place (3 or 4) | loader and readers | 567 | 567 | about 257 for network keys (in-class initializer stays in the header) | no change |
| 9 | New enum class or enum value for a mode key (3) | loader, reader, label function | 567 | 567 | includers of the concern header that holds the enum (inferred) | no change |
| 10 | Private helper or posture declaration in `GnssConfig` (3) | 1 TU | 557 | about 519 | 1 TU only after the loader conversion of S5 | no change |
| 11 | Substantive edit to a network subsystem header (at least 27 of 49 run-state commits not touching config) | its includers | 217 (141 core) | no change | no change | `Raim.hpp` 12 core, `RingFix.hpp` 9 core; mean network-header edit 6 094 to 2 815 core-s (unconfirmed) |
| 12 | Knob or counter added to `RunContext.hpp` (33 of 35 commits add a knob) | the reading seam | 217 | no change | no change | about 217: the floor while every seam reads the run context |
| 13 | Row member added in `Entities.hpp` or `RunTypes.hpp` (14 and 17 commits) | the reading seams | 268 / 294 | no change | no change | lower only as includers move to narrow headers; 151 core for `RunTypes.hpp` while 180 files include it directly |

Two facts govern the order:

- 29 of the 35 `RunContext.hpp` commits also edited a config header. For the usual new-key commit the 217 run-state TUs are already inside the 557 to 567 config set, so step E does nothing for kinds 1 to 3 until the config side is solved.
- Under the current design the 71 config commits cost about 40 000 TU compiles against a few hundred needed (inferred, one rebuild per commit; working sessions rebuild more often).

## 4. Split design chosen

### 4.1 Chosen, in this order

**E. Counter and state leaves for `RunContext.hpp` (first).**
- [corrected 2026-11-19: see Corrections, item 2] Each subsystem's `XxxCounters` struct moves verbatim into `<X>Counters.hpp`, on the pattern of the eight that exist (`StateSnapshotCounters.hpp`, `BorderCounters.hpp`, `ChainFrameCounters.hpp`, `PassASlipScanCounters.hpp`, `WarmResumeCounters.hpp`, `HandoverStransformCounters.hpp`, `filter/KalmanQcCounters.hpp`, `slip/CycleSlipCounters.hpp`). Types held by value (for example `SharedHistoryState`, `ArLatticeState`, `SignalRoster`, `ArcLedger`) move into `<X>State.hpp`. Function declarations stay in the subsystem header.
- The four `unique_ptr` members (`NetworkSlipOracle`, `NetworkOraclePin`, `NetworkRingFix`, `NetworkAttribFrame`) are forward-declared; `NetworkCarryState` gets an out-of-line destructor and move operations. The `RhsAttrib.hpp` include stays while `RhsAttribCounters` is a base.
- `NetworkKnobs` (`RunContext.hpp:169-1213`) moves to `include/network/driver/Knobs.hpp`, beside the existing `src/core/network/driver/Knobs.cpp` (one stage proposed `network/state/`; the driver folder matches the existing source). This helps parsing only: `NetworkRunContext` holds `const NetworkKnobs knobs` by value (`:2587`).
- The snapshot, resume and injection free functions (`:2647-3009`, 29 callers) move to their own header.
- Reasons: largest simulated gain (123 to 35 network headers reaching at least 130 core TUs); mechanical; layout-neutral; one owner per concept (a subsystem's counters live beside it, readable on their own); precedent exists (`a65e4c7`).

**S1. Forward declarations and type headers for the config (low gain, low risk).**
- [corrected 2026-11-19: see Corrections, item 9] `include/config/GnssConfigFwd.hpp` holds `class GnssConfig;`. It replaces the include in the 35 reference-only headers and consolidates the 68 ad hoc `class GnssConfig;` declarations already present (none spells `struct`). Simulated: 557 to 519 TUs.
- The 9 type-only headers include the concern header they need (step S3 below). Simulated with both: 438 TUs; 387 if `MultiRinexSync.hpp`, `RealTimeEngine.hpp` and `EphemerisProvider.hpp` also stop holding or calling the class inline. The floor is the 342 direct TU includers.
- Reason: cheap and readable; it does not by itself help any struct edit (kinds 1, 3, 6 to 9), because `GnssConfig.hpp` still includes every struct to hold it.

**S3. `GnssConfigTypes.hpp` split by concern, umbrella kept.**
- Headers: `config/NetworkConfig.hpp`, `config/RingFixConfig.hpp` (a third of all edits), `config/RealTimeConfig.hpp`, `config/StochasticConfig.hpp`, `config/SlipConfig.hpp`, `config/ModelConfig.hpp`, `config/RunLayout.hpp`, `config/SignalScope.hpp`, `config/LoggingConfig.hpp`. `GnssConfigTypes.hpp` becomes an umbrella that includes them all, so no includer changes at once.
- Text moves byte for byte; a concatenation diff against the old file proves it.
- Reason: it is the precondition of S4/C and of every gain for kinds 1, 6 and 9.

**S4/C. Network configuration behind an indirection (after the author's decision).**
- `GnssConfig` holds `NetworkConfig` in a deep-copying box; `getNetworkConfig()` and the getters that read `network_config_` (24 uses counted by the review stage; one stage counted 28) are defined in `GnssConfig.cpp`; `GnssConfig.hpp` needs only a forward declaration. Copy, move and destructor of `GnssConfig` are `= default` out of line. The inline `NetworkRunContext` constructor that reads `nc.trace_estimability_budget` (`RunContext.hpp:2569-2577`) moves to a `.cpp`.
- Phase 2: the 308 `network_*_` members move into `NetworkConfig`, their 283 trivial getters are deleted, and network keys are read through `getNetworkConfig()`.
- Reason: the only option that cuts kinds 1 to 3, 7 and 8 (simulated: 84 % to 46 % of core compile-s, unconfirmed; 69 % when `NetworkKnobs` also changes). It also removes the two homes of a network key (one owner per concept).
- Extension for kind 6: the same box for the closed-loop, slip QC and real-time structs. Not designed here; listed as a decision.

**S5. Private loader declarations out of `GnssConfig.hpp` (for kind 10).**
- The posture readout types and the `loadNetwork*_`, `validateNetwork*_`, `logNetworkSummary_` declarations (`GnssConfig.hpp:9875-10006`) are private member functions; they cannot leave the class by a move. They become static members of a `GnssConfigLoader` struct in an internal header under `src/core/config/`, with one `friend struct GnssConfigLoader;` in the class. This is a refactor, not an include-only step (review stage correction).

**F. `RunTypes.hpp` and `Entities.hpp` (last, file by file).**
- `network/state/CarryRecords.hpp` takes the seven records `Entities.hpp` uses and `kCarryDispositions` (`RunTypes.hpp:404`); `Entities.hpp:76` then drops `RunTypes.hpp`. `StationAssembly` (`:1288-1674`, 119 files) and `RelevelEpochState` (51 files) split out first, since neither `RunContext.hpp` nor `Entities.hpp` uses them. `rowContextOf` (`:342`) moves to a `.cpp`. The declaration tables and snapshot hooks of `Entities.hpp` move to `network/state/EntityDecl.hpp` (extern declarations only; definitions stay in `Entities.cpp` and `RunFields.cpp`). `PppModel::CanonicalSignal` moves to a namespace-scope leaf with an alias kept inside `PppModel` (feasibility inferred).
- Reason: gains arrive only as includers move to narrow headers, so it is slow and is scheduled last.

### 4.2 Rejected

- **D. Moving every inline getter out of `GnssConfig.hpp` on its own.** The member layout stays in the header, so the rebuild set does not shrink; about 8 700 of the 10 586 lines are comments, so parse savings are small; without LTCG, 554 trivial getters become cross-TU calls. Kept only for the network getters inside C.
- **Precompiled headers.** They do not shrink the rebuild set of a header that is edited often, and these five are the most edited.
- **Moving `LogLevel` alone to stop spdlog reaching config includers.** Insufficient: `GnssConfig.hpp:6` includes `LogManager.hpp` itself and its private declarations take `std::shared_ptr<spdlog::logger>`. A small `logging/LogLevel.hpp` plus a forward declaration of `spdlog::logger` (or the S5 loader move) is required (review stage correction).

## 5. Include-only step list

[corrected 2026-11-19: see Corrections, item 8] Every step is one commit and is checked by a compile of `gnss_core` from inside the worktree with `scripts/wt_build.ps1 -Config x64-Debug -Target gnss_core` (not `scripts/vs2026_build.ps1`, which rebuilds the main checkout), then a compile only of `gnss_tests`, `gnss_processor_module` and `gnss_simulator_selftest`, which include the same headers. Nothing is run. A TU that loses a type names itself with C2027 and gets the include it used.

| Step | Change | Kind | Waits for |
|---|---|---|---|
| 1 | Add `config/GnssConfigFwd.hpp`; switch the 35 reference-only headers and the 68 ad hoc declarations | include-only | nothing in the paused list touches these headers; may go first |
| 2 | Split `GnssConfigTypes.hpp` into the nine concern headers, umbrella kept; concatenation diff proves the text | verbatim move | the configuration work to commit (both config headers are uncommitted) |
| 3 | Switch the 9 type-only headers to their concern header | include-only | step 2 |
| 4 | `GnssConfigLoadNetwork.cpp` stops including `qc/PrefitCensus.hpp` for two parse functions (the functions move to a small parse header) | include-only plus one declaration move | the configuration work |
| 5 | [corrected 2026-11-19: see Corrections, item 3] Counter leaves, one subsystem per commit, the most edited first (ring fixing, RAIM, chain datum and frame, product emitter) | verbatim move | the chain cleanup (ring fixing, chain and emitter headers are in its scope and in the paused list) |
| 6 | Forward-declare the four `unique_ptr` members; out-of-line special members of `NetworkCarryState` | include-only plus out-of-line definitions | step 5; `RunContext.hpp` and `RingFix.hpp` committed |
| 7 | `NetworkKnobs` to `network/driver/Knobs.hpp`; snapshot and resume free functions to their own header; `NetworkRunContext` constructor out of line | verbatim move | step 6; `Knobs.cpp` and `Run.cpp` committed |
| 8 | `RunTypes.hpp` concern headers, `CarryRecords.hpp`, `EntityDecl.hpp`, `rowContextOf` out of line; remove the include from the 12 headers that use no `RunTypes.hpp` type | verbatim move, include-only | step 7; `Entities.hpp` committed |
| 9 | `CanonicalSignal` leaf; `Entities.hpp:78` drops `PppModel.hpp` | type move | step 8 and a lookup check (risk 6) |
| 10 | Network configuration box (S4/C phase 1), then phase 2 member migration, then S5 | refactor, layout change | the author's decision on the key convention; the configuration work committed |

A new `.cpp` (for a moved constructor or the loader struct) must be added by hand to the explicit source list in `CMakeLists.txt`, or the link fails. New headers need no entry.

Runs needed later, none requested now: a full Debug build with `gnss_tests` after steps 4, 9 and 10; a full Release build; a per-object symbol list comparison (`dumpbin /symbols`) before and after each include-removal step; the same-build output comparison on the three reference configurations (GPS and Galileo on observed data, the simulated network, the BeiDou lineage; AVX2 build against AVX2 build) after step 7 and after step 10; one 10-epoch smoke and one 2 h comparison against the baseline pair after step 10.

## 6. What must wait

- **The paused step.** Its uncommitted files include both config headers, `RunContext.hpp`, `Entities.hpp`, `RingFix.hpp`, `NetworkProductEmitter.hpp`, `NetworkStateSnapshot.hpp`, `Knobs.cpp`, `Run.cpp` and `CMakeLists.txt`. Every step except step 1 edits at least one of them; doing them first would conflict with the paused work.
- **The chain cleanup** (defects P1 to P18 of the chain audit). It edits the ring-fixing, chain and emitter headers that step 5 splits; moving their counters first would rebase every one of its commits (inferred from the file list, not from the defect texts).
- **The configuration work** (new keys, ledger rows, the convention decision). Steps 2 to 4 and 10 rewrite the files where keys are added; step 10 changes the CLAUDE.md toggle convention itself.

## 7. Risks

1. **Silent overload change on include removal.** A TU that loses a header can still compile if a weaker overload stays visible. Steps 5 and 8 remove the most includes. Guard: the per-object symbol comparison (inferred method).
2. **Initialization order of `NetworkRunContext`.** Knobs, then params (built from knobs), then the parameter inventory (`RunContext.hpp:2569-2599`). Declaration order must not change in step 7. Guard: enable MSVC warning C5038 for `gnss_core`.
3. **Copy semantics of the box (step 10).** `GnssConfig` is copied (`deriveClosedLoopConsumerConfig`, `GnssConfig.hpp:146`; `SetupResult.hpp:169`). A `std::unique_ptr` deletes copy; a hand-written copy constructor silently drops any member added later; a moved-from box read through `getNetworkConfig()` either crashes or, if it returns a default, silently runs a default network configuration. Required: a deep-copying box, special members `= default` out of line, no default fallback.
4. **Moved enum machinery.** `GnssConfigTypes.hpp` has four inline functions (`:4412`, `:4627`, `:4668`, `:4696`), a class-scope static (`:1713`), and `kNetworkQClassCount` (`:1582`) and `kNetworkQClassTable` (`:1611`) tied to the "append only" enum at `:1567`. These three move together with `NetworkProcessNoise` (`:1671`). An opaque enum declaration must repeat its base (`enum class NetworkQClass : std::size_t;`).
5. **Out-of-line special members** of `NetworkCarryState` (step 6): declaring only the destructor suppresses the implicit move; this fails at compile time (unique_ptr is not copyable), so it is not silent.
6. **Hoisting `PppModel::CanonicalSignal`** changes argument-dependent lookup: functions found through the class scope stop being found, which may be an error or a silent change of overload (inferred).
7. **Static initialization.** `kEntityFieldTables` (`Entities.cpp:1238`) points at the `kXxxFields` tables (constant initialization); `kRunFields` (`RunFields.cpp:20`) is read by no initializer in `Entities.cpp`. Moving extern declarations is safe; moving definitions between `.cpp` files is not done.
8. **Object identity.** No `memcpy`, `sizeof` or `offsetof` is applied to the moved structs (review stage grep, including `SnapshotFingerprint.cpp`), so snapshot fingerprints cannot move. Code that uses `__LINE__` changes constants, so objects are not byte-identical after a move; compare disassembly ignoring line constants (inferred).
9. **Regrowth.** A leaf header that later includes its subsystem header undoes the gain. A check in `scripts/nom_epoch_lint.py` style (leaf headers include only leaves) would hold it (inferred, not designed).
10. **Cost of out-of-line getters.** Without LTCG they cannot inline. Seams already bind `const NetworkConfig& nc` once (`RunContext.hpp:2569`), so the cost should be negligible (inferred).

## 8. Disagreements between the stages, as settled

| Claim | Stage | Review verdict | Used here |
|---|---|---|---|
| 21 reference-only and 19 type-only config includers | design | refuted: 21 + 19 + 5 does not make 49; grep gives 35 / 9 / 5 | 35 / 9 / 5 |
| "the one inline free function" in `GnssConfigTypes.hpp` | design | refuted: four inline functions, a static and two constexpr tables | risk 4 |
| Moving `LogLevel` stops spdlog reaching config includers | measure (config) | refuted: `GnssConfig.hpp:6` includes it too | section 4.2 |
| Validate and log declarations move to an internal header | measure (config) | refuted as a move: private members, need a friend loader | S5 |
| RunTypes has 66 including headers | measure (state) | 60 | 60 |
| Seven `/bigobj` exceptions caused by the run-context headers | design | partly: 13 settings from `CMakeLists.txt:418`; several attributed to Eigen (`CMakeLists.txt:408-410`) | not used as evidence |
| `Knobs.hpp` in `network/state/` or `network/driver/` | the two stages | neither exists; driver matches `Knobs.cpp` | `network/driver/Knobs.hpp` |
| 28 getters read `network_config_` | design | 24 uses plus `getNetworkConfig()`; exact figure open | "24 to 28" |
| Forward declarations save 2 % (core compile-s) or 7 % (all TUs) | both | consistent: different measures | both quoted |
| Compile-second medians 55 s and 16 s | design | not checked | marked unconfirmed |
| A ring-fix key would rebuild at most 28 TUs after S4 | measure (config) | not addressed | holds only with a separate indirection for the ring-fix struct; about 257 under C as designed (inferred) |

## Decisions for the author

1. **Where a network key lives.** Adopt "a member of `NetworkConfig`, read through `getNetworkConfig()`" in place of the CLAUDE.md private-member-plus-getter convention for network keys. Without it every new key adds a getter to `GnssConfig.hpp` and step 10 saves nothing.
2. [corrected 2026-11-19: see Corrections, item 1] **Box the other concern structs too** (closed-loop, slip QC, real time, and the ring-fix struct on its own), so that edit kinds 6 and 1 fall to tens of TUs instead of about 257 or 567.
3. **Whether to start with step 1 now**, before the paused step lands; it touches no file in the paused list and saves about 7 % of TUs per config edit.
4. **Order relative to the chain cleanup:** counter leaves (step 5) after the cleanup commits, or the cleanup rebased onto them.
5. **A lint rule against regrowth** of leaf headers.

## Not checked

- Wall-clock compile cost: no build was made; seconds come from one parallel-load log and were not reviewed.
- Every saving after a change is a simulation on the include graph; none was re-run by the review stage.
- Reader counts are grep counts and include comment mentions (upper bounds); `ctx.` member-use counts may miss aliased bindings.
- Whether inline code in the subsystem headers needs the complete `RunContext.hpp` types, which decides whether step 5 reaches 12 core TUs or fewer.
- Whether the 9 includers that use no context member and the 12 headers that use no `RunTypes.hpp` type compile without the include.
- [corrected 2026-11-19: see Corrections, item 1] Whether `NetworkRingFixConfig`, `SlipQcConfig` and `ClosedLoopConfig` are held inside `NetworkConfig` or directly by `GnssConfig`, which sets the post-C rebuild of kinds 1 and 6.
- How many of the 342 direct `GnssConfig.hpp` includers read only 1 to 3 getters.
- Whether a typical new key also edits `RunContext.hpp`, `RingFix.hpp` or `Entities.hpp` beyond the 29 of 35 measured.
- Whether `CanonicalSignal` can move out of `PppModel` without a lookup change.
- The working-copy edits of the paused step were deliberately not read.

## 2026-11-19: corrections from the check against the code

Trees: `03dfcbc` read with `git show` from the read-only checkout of `7fb0737`; `7fb0737` (one commit later: adds `ChainMemory.hpp`/`.cpp`, one test and 10 lines in `RunContext.hpp`); main at `5ddc422`. The check counted on the include graph with the same definitions as the draft; no build was made.

1. **Where three config structs live** (section 3 row 6, decision 2, the "Not checked" item on `NetworkRingFixConfig`, `SlipQcConfig`, `ClosedLoopConfig`). The document treats the closed-loop and slip-QC keys as members of a sub-struct other than `NetworkConfig` and leaves open where the ring-fix, slip-QC and closed-loop structs are held. The code: all three are members of `NetworkConfig` (`GnssConfigTypes.hpp:2094`, `:2096`, `:2111`, at `03dfcbc` and `7fb0737`); only `RealTimeConfig` and `CycleSlipConfig` are held directly by `GnssConfig` (`GnssConfig.hpp:10532`, `:10556`, at `03dfcbc`). Row 6 therefore splits: closed-loop (3) and slip QC (2) behave as kind 3 after S4/C (about 257 TUs); real time (2) and cycle slip (1) stay at 567 unless boxed. Decision 2 in corrected form: box the real-time and cycle-slip structs, and give the ring-fix, closed-loop and slip-QC structs their own indirection inside `NetworkConfig`, so that kinds 6 and 1 fall to tens of TUs instead of about 257 (ring-fix, closed-loop, slip QC) or 567 (real time, cycle slip). The "Not checked" item is settled.
2. **Step E is not a pure verbatim move of the counter structs** (section 4.1 E). Every type a counter struct holds by value moves with it (for example the four RAIM tallies of `Raim.hpp`, `StationSolveTrack`, `NetworkWireFillCounts`, the `PrefitCensus` tallies). `SbasisCounters` and `CodeBiasDatumCounters` hold maps of types nested in class `NetworkSbasis` (`Sbasis.hpp:246`, `:337`, `:440`, `:492`, `:693`), which can neither be forward-declared nor moved verbatim: they are hoisted with aliases (a lookup change of the risk-6 kind) or that leaf includes `Sbasis.hpp`.
3. **Ring fixing in step 5** (section 5, step 5, "the most edited first (ring fixing, ...)"). `NetworkRingFix` contributes no base to `NetworkServiceCounters`; its gain comes from step 6, so "ring fixing" drops from step 5.
4. **`RunContext.hpp:59`** (section 2.2). The document says `:59`, `:62`, `:63`, `:64` include whole headers only for `unique_ptr` members. `:62`, `:63`, `:64` do; `:59` (`RhsAttrib.hpp`) also supplies the base `RhsAttribCounters` (`RunContext.hpp:2556` at `03dfcbc`, `:2566` at `7fb0737`), so forward-declaring `NetworkAttribFrame` gains nothing while that base stays. Section 4.1 E already keeps that include.
5. **`RingFix.hpp` rebuild** (section 2.2). An edit to `Raim.hpp` rebuilds 141 core TUs (confirmed); `RingFix.hpp` rebuilds 143 at `03dfcbc` (144 at `7fb0737`), not 141.
6. **Bases of `NetworkServiceCounters`** (section 2.2). 89 bases, not "about 95" (`RunContext.hpp:2467-2556` at `03dfcbc`, `:2477-2566` at `7fb0737`). Of the 54 includes at `:77-130`, 6 are already leaf counter headers (`StateSnapshot`, `HandoverStransform`, `Border`, `ChainFrame`, `WarmResume`, `PassASlipScan` counters).
7. **Direct includers of `GnssConfig.hpp`** (section 2 table). 395 files = 53 headers (49 under `include/`, plus `tests/network/ConfigLoadCapture.hpp`, `tests/support/ModelStack.hpp`, `tests/support/NetworkTestRun.hpp`, `tests/support/StationBirthFixture.hpp`) and 342 TUs; 49 + 342 does not make 395.
8. **Compile errors after an include removal** (section 5). A TU that loses a declaration fails with C2027, C2065, C3861 or C2039, not only C2027.
9. **Ad hoc declarations** (section 4.1 S1). 70 `class GnssConfig;` declarations in 68 files, none spelled `struct` (`git grep` at `03dfcbc`), not 68 declarations.
10. **Trivial network getters** (section 2.1). 308 `network_*_` members confirmed; the 283 trivial getters were not reproduced: 307 one-line `return network_X_;` getters exist, excluding `getNetworkConfig` (`03dfcbc`). The counting rule that gives 283 is not stated.
11. **Translation-unit totals** (section 1). The committed `CMakeLists.txt` at `03dfcbc` lists 720 distinct TUs (333 core, 368 in `gnss_tests`, 4 in the simulator self-test); at `7fb0737` it lists 722 (334, 369, 4). The draft's 334 core matches only the later tree (the uncommitted list), against its "committed files only". Rebuild counts at `03dfcbc` are one lower accordingly (556, and 263 of 333 core, for `GnssConfig.hpp`; 566 and 265 for `GnssConfigTypes.hpp`).

The manager's note at the foot of the draft was removed before filing.

### Line numbers that moved

- `unique_ptr` members of `RunContext.hpp`: `:2306`, `:1716`, `:1726`, `:1732` at `03dfcbc` are `:2316`, `:1726`, `:1736`, `:1742` at `7fb0737`.
- `NetworkServiceCounters` bases: `RunContext.hpp:2467-2556` at `03dfcbc` (the draft says `:2467-2557`), `:2477-2566` at `7fb0737`.
- The by-value struct block of `GnssConfig` starts at `GnssConfig.hpp:10532` (`cycle_slip_`), earlier than the `:10550` the draft gives.
- Counts at `7fb0737`: `GnssConfig.hpp` 396 direct includers; `RunContext.hpp` 218 includers, 219 TUs rebuilt, 142 core; 123 of 171 network headers reach at least 130 core TUs; `RingFix.hpp` 144 core.
- `PppModel::CanonicalSignal`: `PppModel.hpp:74`, `:557` at `7fb0737`.

### What stands

The Outcome's counts (557 to 567, 264 of 334, 216 headers, 217 units) reproduce within one unit on the include graph. The two-costs diagnosis and the order (counter leaves first, then the configuration indirection after the author's decision) stand. The figures after a change are simulations on the include graph, as the draft says; the check's own simulation of step E gives 123 to about 30 network headers reaching at least 130 core TUs (the draft says 35), `Raim.hpp` 12 core and `RingFix.hpp` 10 core (the draft says 9), at `7fb0737`, assuming leaves include nothing and `RhsAttrib.hpp` stays. Step E, as a verbatim move of type definitions, keeps every class layout and the base order of `NetworkServiceCounters` (written in `RunContext.hpp`, not moved), so it cannot change arithmetic by itself; behaviour can move only through removed transitive includes (overload, argument-dependent lookup, template specialization), which risk 1 and the symbol comparison cover. No header in the closure has a namespace-scope `using namespace`, a project `#define` or a dynamically initialized global. What the draft does not see is cost, not risk (items 2 and 3).

Decisions put to the author after correction:

1. Where a network key lives: stands.
2. Boxing the other concern structs: stands only in the corrected form of item 1 (box the real-time and cycle-slip structs; an indirection inside `NetworkConfig` for the ring-fix, closed-loop and slip-QC structs).
3. Whether to start with step 1 now: stands.
4. Order of the counter leaves relative to the chain cleanup: stands.
5. A lint rule against regrowth of leaf headers: stands.

### Left unconfirmed

- Compile seconds (every figure already marked unconfirmed in the draft).
- The simulated counts after S1 and S4/C (519, 438, 387, about 257, 163, 46 %, 69 %).
- The 35 / 9 / 5 classification of the 49 header includers: the check's grep gives 36 naming `GnssConfig&` or `GnssConfig*`, 3 naming only `GnssConfigTypes.hpp` types, and 5 naming neither (`BlockBaarda`, `Congruence`, `RedundancyCensus`, `RhsAttrib`, `DatumGraph`).
- The "74 %" network share; the per-TU median of 3 knobs and 4 counters; the 12 headers that use no `RunTypes.hpp` type; the 9 umbrella includers.
- The hunk classification behind the edit-kind commit counts of section 3 rows 1 to 10 (the totals 35, 71, 29 of 35, 14 and 17 are confirmed).
- The carriers of section 2.1 other than `RinexParser.hpp` (422), `ProductParser.hpp` (401) and `PppModel.hpp` (375), which are confirmed.
