# URS: the user resolver split into named stages, at identical output (2026-11-19)

**STATUS**

- **id:** URS
- **topic:** user-ar
- **status:** open
- **summary:** `AmbiguityResolver.cpp` of the user engine split into named stage functions at identical output, step by step, then the reference-switching build. Steps 1 and 2 (stages S1 and S9 as named functions) built on branch `user/resolver-stages-2026-11-19` (fb44bc6, af91bf5): unit log 325 of 325, the UTQI fixing replay identical to its control in 32 of 37 files and the other 5 differing only in path, wall time and backlog lines; merged into `int/2026-11-19` on 2026-11-20 (e02616a) and in main. Steps 3 to 6 and the reference switching are drafted and reviewed, not built: paused, to resume on top of card UFQ (one writer per file).
- **source:** package `user-resolver-split` of the day program of 2026-11-19 (`D:/GnssProcessorSupport/gates/day_program_2026-11-19/packages/user-resolver-split/`: `CHECKPOINT_1.md`, `CHECKPOINT_2.md`; tools and night-run notes in `D:/GnssProcessorSupport/gates/day_program_2026-11-19/results/user_resolver_split/`); the managing session's checks of 2026-11-19.
- **filed:** 2026-11-21, after the fact, from the package named under source.

**Outcome.** The first two steps of the split are in main and move no output: stages S1 and S9 of the user resolver are named functions, the unit suite of the resolver passes 325 of 325, and a fixing user replay equals its control in every file except paths, wall time and backlog lines. The remaining four steps and the reference-switching build are drafted and independently reviewed but not built; they wait for the user-side work of card UFQ, which owns `AmbiguityResolver.*` now, and resume on top of it.

## Filing note (2026-11-21)

The sections below are the package's two checkpoints, unchanged apart from heading levels.

## CHECKPOINT_1.md

### user-resolver-split, checkpoint 1 (2026-11-19, 11:50)

1. The control engine and the identity method are fixed and shown repeatable on a simulator user and an observed user.
2. Step 1 (compile only) is applied and its Debug build links; its unit-test run waits in the slot line, so it is not committed yet.
3. Steps 2 to 6 and the reference-switching build are drafted and independently reviewed; none is built.

#### Where

- Worktree `D:\GnssProcessorSupport\worktrees\ar_resolver`, branch `user/resolver-stages-2026-11-19`, cut from
  `int/2026-11-19` at cf983dc (verified by the managing session: four 2 h identity runs, full unit suite clean).
- Results and tools: `D:\GnssProcessorSupport\gates\day_program_2026-11-19\results\user_resolver_split\`
  (`tools\make_users.py`, `tools\run_users.py`, `tools\cmp_runs.py`, `NIGHT_RUNS.md`).

#### Control engine (step 0)

| Item | Value |
|---|---|
| Source | cf983dc, unmodified, `scripts\wt_build.ps1 -Config x64-Release -Target gnss_processor` (AVX2, MSVC 14.51) |
| Frozen copy | `build\x64-Release\gnss_processor_rs0.exe` and `D:\GnssProcessorSupport\gates\exe\x64-Release\gnss_processor_rs0.exe` |
| sha256 | 01ee83522084308a97c52ffa7af28554a4c928a093d73ae308c62288407d67d1 |

#### Replays (the three users of every step)

Each folder is a copy of the source user's `arm.json` with only `network.product_output_path` moved into the
folder (and, for the observed users, `processing.max_duration_s` 14400). Root `D:\GnssProcessorSupport\runs_nom`.

| User | Source folder | Control folder | Length | Wall time | What it exercises |
|---|---|---|---|---|---|
| simulator, partial fixing on | `_rap_irdu_u_nrf7_hf_g6_fix_s40_s30` | `_rap_rs_c0_irdu_u_nrf7_hf_g6_fix_s40_s30` | 2 h | 70 s | 43 fixes of which 21 partial ("[PAR]" lines 91), 440 refused partial searches, 56 partner refloats |
| UTQI fixing | `_rap_nw_obsadx_146_utqi` | `_rap_rs_c0h4_nw_obsadx_146_utqi` | first 4 h | 1 497 s | 8 full fixes (first at 02:28), 56 ratio refusals, 162 upper-bound refusals, 3 607 wide-lane branch decisions, 2 791 ARCAGE lines |
| UTQI float | `_rap_nw_obsadf_146_utqi` | `_rap_rs_c0h4_nw_obsadf_146_utqi` | first 4 h | running at 11:50 | the float reference user |

Why 4 h and not 24 h: on the shared machine a 24 h observed replay ran at about 4 min per data-hour (about 100 min
per run); UTQI's first fix is at 02:28, and the record's own proof specification for identity steps is 2 h (UFA
section 5). The 24 h identity of all 42 users is the night set A of `NIGHT_RUNS.md`.

No observed user releases a hold on DOY 146 (0 HOLDREL lines at UTQI, QUIN, ASCG and WTZR; the user-holds team counts
0 in all 105 fixing user-days), so the release branch is exercised by the unit suites, not by replays.

#### Identity method

`cmp_runs.py <control> <candidate>`: byte comparison of `stdout.txt` and every file of `logs/` (31 files for the
simulator user, 33 for UTQI). Three masks, nothing else:

1. the run folder's own name (it appears in the "Configuration loaded from" line);
2. the wall-clock lag of the "obs backlog ... behind wall-clock GPS" warning;
3. the elapsed time of the "RINEX ingestion: parsed N stations in T s" line.

The resolver's log lines carry epoch time, not wall-clock time, so they need no mask.

Repeatability of the control:

| Pair | Verdict |
|---|---|
| `_rap_rs_c0b_irdu_u_nrf7_hf_g6_fix_s40_s30` vs `_rap_rs_c0_...` | IDENTICAL, 31 files, 48 masked lines |
| `_rap_rs_c0bh4_nw_obsadx_146_utqi` vs `_rap_rs_c0h4_...` | IDENTICAL, 33 files, 36 masked lines (20 Eigen threads) |

The control does not equal the old folders (made by the rscb6 build): `gnss_config.log` and `stdout.txt` differ in
length from the configuration lines on. As the brief says, those folders are never compared.

#### Step 1 (compile only), applied, not yet committed

- `include/filter/AmbiguityResolverStages.hpp` (new): `kWlcStages` (S1 to S9 with reads, writes and the audit rules
  W1 to W18 each owns) and `HoldCheck` (ok, contradicted, unusable group, key miss, bad index).
- `include/filter/AmbiguityResolver.hpp`: the stage types (WlcSignal, WlcGroup, HoldCheckRecord, WlcWideLane,
  NlCandidate moved from a local struct, WlcEpoch); QbCond, qbCondition and rebuildQbCycles moved out of the
  anonymous namespace; friend `ArWlcStageTestAccess`.
- `src/core/filter/AmbiguityResolver.cpp`: the two moves above, no executed line changed.
- New suites: `ArQbCondition` (`tests/test_ar_qb_condition.cpp`), `ArWlcStageTable` (`tests/test_ar_wlc_stage_table.cpp`).
- Debug build of `gnss_tests`: linked, no error (log `D:\GnssProcessorSupport\worktrees\ar_resolver_dbg1.log`).
- Unit run by filter `Ar*:Ambiguity*:Kalman*:Mlambda*:NlSearchRow*:WlFfr*:WlOnly*`: queued (15th in the slot line
  at 11:36); result to `results\user_resolver_split\step1_tests.log`.

#### Drafted, reviewed, not built

| Step | Content | Review |
|---|---|---|
| 2 | S1 groupSatellitePairs_, S9 reportFix_; suites ArStageGroupPairs, ArStageReportFix | none of the engine output changes; one fix applied (the logger is fetched once per call as before; a fetch per stage would add console lines with an uninitialised log manager) |
| 3 | S2 testHeldPairs_ (with the in-memory outcome record), S3 decideWideLane_, S5 settleContradictions_; suites ArStageHeldPairs, ArStageWideLane | as step 2 |
| 4 | S4 gateCandidate_, S6 buildSearchProblem_; dead consumed_dd and an unreachable test removed; suite ArStageSearchProblem | none |
| 5 | S7 searchAndAccept_, S8 writeBack_ (full and partial write-backs merged, "[PAR]" kept); suite ArStageAcceptWrite | none (groups are disjoint, so the per-candidate rows equal the old per-index vector) |
| 6 | resolveCore_ as a walk of kArGates (C4 to C12), per-band body moved verbatim to resolvePerBand_, refreshArcAges_ on stepKeyGenerations and partnerRefloatSatellites; suite ArStageGates | none; two documentation points fixed (doc block place, compile-time check of the table length) |

After step 5, `resolveWlConstrained` is 47 lines (from 805).

#### Machine

From 10:10 to 10:54 every slot was held by fresh full builds of other teams; the step-1 Debug build of this fresh
tree took 38 min. Each further step needs one Debug build, one unit run, one Release build (incremental, but the
header is included by `Application.hpp`, so about a hundred units recompile) and three replays (1, 13 to 25, and 13
to 25 min). At priority 5 behind the current line, steps 2 to 6 will not all land today.

#### Addendum 12:15: step 1 committed and tested

- Commit fb44bc6 on `user/resolver-stages-2026-11-19` (step 1, compile only).
- Unit run on the step-1 Debug binary (frozen copy `build\x64-Debug\gnss_tests_step1.exe`), filter
  `Ar*:Ambiguity*:Kalman*:Mlambda*:NlSearchRow*:WlFfr*:WlOnly*`: 313 tests from 36 suites, all passed, including the
  new ArQbCondition (10) and ArWlcStageTable (5). Log: `results\user_resolver_split\step1_tests.log`.
- Step 2 is applied (not committed); its Release build, Debug build and unit run are one queued ticket.


## CHECKPOINT_2.md

### user-resolver-split, checkpoint 2 (2026-11-19, 15:40)

1. Step 2 is committed (af91bf5): stages S1 (grouping of pairs) and S9 (fix report) are named functions; output identical.
2. Unit run 325 of 325 passed (12 new tests); three replays byte-identical to the control engine.
3. Stopped here under the budget hold; steps 3 to 6 and the reference-switching build wait for release.

#### What changed

- `groupSatellitePairs_` (S1): DD pairs grouped by satellite pair, each signal's frequency and wavelength, the
  primary and extra split computed once per group; sets epoch time, log flag and logger (fetched once per call, as
  before).
- `reportFix_` (S9): the summary and QBCOND lines of a full fix, a partial fix and a refused partial search; text
  unchanged, the refused line keeps its literal zeros.
- New suites `ArStageGroupPairs` and `ArStageReportFix`; shared test access `tests/support/ArWlcStageAccess.hpp`.
- Two test-side fixes found by the first build: the access header named a private type (now the friend's alias),
  and the report test now strips the CR of the CR LF line end of the captured log.

#### Numbers

| Item | Value |
|---|---|
| Unit run (filter `Ar*:Ambiguity*:Kalman*:Mlambda*:NlSearchRow*:WlFfr*:WlOnly*`) | 325 of 325 passed, 38 suites (log `results\user_resolver_split\step2c_tests.log`) |
| Step-1 unit run (for the record) | 313 of 313 passed (`step1_tests.log`) |
| Step-2 engine | `build\x64-Release\gnss_processor_rs2.exe`, sha256 165d1ccac57899d17d2cdfbe694813b9c2e6657410c74783de246c224aa45b30 |
| Control engine | `gnss_processor_rs0.exe` (cf983dc unmodified), sha256 01ee8352...07d67d1 |

| Replay (runs_nom) | Control | Verdict |
|---|---|---|
| `_rap_rs_s2_irdu_u_nrf7_hf_g6_fix_s40_s30` (sim, 2 h, 43 fixes) | `_rap_rs_c0_...` | IDENTICAL, 31 files, 48 lines masked |
| `_rap_rs_s2h4_nw_obsadx_146_utqi` (first 4 h, 8 fixes) | `_rap_rs_c0h4_...` | IDENTICAL, 33 files, 36 lines masked |
| `_rap_rs_s2h4_nw_obsadf_146_utqi` (first 4 h, float) | `_rap_rs_c0h4_...` | IDENTICAL, 32 files, 36 lines masked |

The three replays took 26 s, 36 s and 40 s in one slot (the machine was free; the controls took up to 1 497 s under load).

#### Owed

- Steps 3 to 6 (drafted and reviewed, not applied), each with its build, unit run and three replays.
- The reference-switching build, its tests and replays (simulator users, then MRC1, SEYG, WTZR DOY 146).
- The night-run note `results\user_resolver_split\NIGHT_RUNS.md` is written; nothing in it is run.
