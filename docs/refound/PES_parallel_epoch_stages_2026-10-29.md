# PES: the sequential stages of the network epoch, run in parallel at identical output

**STATUS**

- **id:** PES
- **topic:** perf
- **status:** open
- **summary:** ARMED 2026-10-29 at the author's decision (section 5), merged into main 2026-11-02 (branch `perf/pes`, section 6). Three serial stages of the network epoch run in parallel over stations behind keys the loader leaves off (ledger ARMED, pinned true in both goldens): `network.parallel_station_normals` (the station normal blocks formed ahead of the serial scatter, committed in station order), `network.parallel_schur_columns` (the Schur reduce split by border columns, every element summed over the stations in serial order; no per-thread partial sum) and `network.parallel_redundancy_census` (the census's per-station kernels ahead of its serial walk). Key on against off, one build, 2 h: IDENTICAL on both goldens for each key and for the three together; off IDENTICAL to main; full Debug suite 3 591 passed, 0 failed; the lints pass. On main with card CVF the loop is 27.9 % shorter on the field golden, 20.3 % on the sim golden and 39.7 % on a 305-station field arm (30 min), at identical output. Armed: all three; the baseline pair `runs/_rf0hv17_{field,sim}6` stands (section 5). 2026-11-06 (section 7, branch `perf/pes-next`, NOT merged, NOT armed): three more stages behind keys the loader leaves off, ledger UNVALIDATED: `network.parallel_carry_harvest`, `network.parallel_iono_traces`, `network.parallel_coupling_prepare`; PASS A's serial remainder rejected with its timing. One OpenMP team size in every region (a change of size rebuilds the team, about 9 ms of wall each: PASS A alone returns 36 of the 66 ms section 4.3 left open) and a cheaper coupling row reach the armed posture without a key. Each key alone and the three together IDENTICAL to off on both goldens, off IDENTICAL to main; on the final build the three keys shorten the golden loop by 16.1 % (field) and 13.3 % (sim) against off; the whole change is 28.9 % (field) and 25.7 % (sim) shorter than main in one matched sequence, and all six stages 48.7 % shorter than none at 305 stations over 2 h, on a machine carrying 11 to 18 logical processors of foreign load. Recommended: arm the three keys (the author's decision). 2026-11-07 (section 8, same branch, NOT armed): a fourth key, `network.sbasis_values_first`: the S-basis completion decides on the eigenvalues alone and computes eigenvectors only on an epoch that needs them, and the rank gate reads those eigenvalues; identical alone and with the other three on both goldens and at 305 stations, off identical to main. Main against all four keys in a matched sequence: -41.5 % (field), -34.8 % (sim). Recommended: arm all four. ARMED 2026-11-07 at the author's decision (section 9): pinned in all three goldens, main's commits merged into the branch; the baseline pair stands.
- **source:** [NETCOST](../record/refound/NETCOST_gps_galileo_cost_attribution_2026-10-18.md) sections 2 and 3 (the section attribution under load); [CVF](../record/refound/CVF_conditional_variance_frame_2026-10-29.md) (the covariance-consumer overheads; merged into main during this card, section 4.3); the auto-memory records of the closed perf doors (`parallel_reduce` deleted by card RKD: per-thread partial sums regroup the Schur subtraction and the network's conditioning amplifies the ULP difference over the carried epochs).
- **knobs / posture:** new `network.parallel_station_normals`, `network.parallel_schur_columns`, `network.parallel_redundancy_census`: bool, loader default false, ledger ARMED and pinned true in both goldens since 2026-10-29 (section 5); nothing else changed. New 2026-11-06 on branch `perf/pes-next`: `network.parallel_carry_harvest`, `network.parallel_iono_traces`, `network.parallel_coupling_prepare`: bool, loader default false, ledger UNVALIDATED, in no golden. New 2026-11-07, same branch: `network.sbasis_values_first`, the same. All four ARMED 2026-11-07 (section 9): ledger ARMED, pinned true in the sim and field goldens and through its base in the BeiDou golden; loader default false.
- **dates in the text:** 2026-10-29, 2026-11-02, 2026-11-06, 2026-11-07
- **hashes / run dirs:** branch `perf/pes` from main `a78ba68` (gates G1 to G4), main merged in at `b09c353` (G5); executables `gnss_processor_pes_c3.exe` (`64d24097d86f7bef`), `gnss_processor_pes_c4.exe` (`0845299169e2b0bd`), main `gnss_processor_pes_base.exe` (`66333b5e42827350`); worktree `D:/GnssProcessorSupport/worktrees/pes`; executables under its `build/x64-Release/`; runs `D:/GnssProcessorSupport/runs_pes/`; gate records `D:/GnssProcessorSupport/gates/pes/`. Section 7: branch `perf/pes-next` from main `284ec29`, worktree `D:/GnssProcessorSupport/worktrees/pes2`, runs `D:/GnssProcessorSupport/runs_pes2/`, gate records `D:/GnssProcessorSupport/gates/pes2/`.
- **gates (frozen at pre-registration):** section 3, written before any run of a changed executable.

**Outcome.** Question: which serial stages of the network epoch split by station, and how much of the loop do they
return when run in parallel without moving an output bit? Result: the station normal formation, the Schur reduce and
the redundancy census (37 % of the field epoch) run 2.0 to 4.8 times faster in parallel. Every key-on arm is identical
to key off, and key off to main. The golden loop is 27.9 % shorter (field) and 20.3 % (sim), and 39.7 % at 305
stations (bars: identical, a gain on both beds: met). Standing: ARMED on both goldens at the author's decision,
loader default off, merged into main 2026-11-02 (section 6). Continued 2026-11-06 (section 7): the carry harvest, the iono traces and the coupling inverse also run in parallel at identical output, PASS A is rejected on its timing, and one OpenMP team size removes about 9 ms of wall per team change. The golden loop is 28.9 % (field) and 25.7 % (sim) shorter than main, and all six stages 48.7 % shorter than none at 305 stations. Standing: the three new keys unarmed on branch `perf/pes-next`, not merged. Continued 2026-11-07 (section 8): the
S-basis completion now decides on eigenvalues alone and the rank gate reads them, at identical output; with all four keys
the golden loop is 41.5 % (field) and 34.8 % (sim) shorter than main. Armed in all three goldens 2026-11-07 (section 9).

## 1. Question

Card NETCOST attributed the GPS + Galileo epoch by section on a loaded machine. On a quiet machine (2026-10-29, one
unrelated real-time process running, 10 % load) the 2 h field golden arm on main `a78ba68` spends 1 270 ms per epoch
and keeps 4.2 of 20 logical processors busy, and the sim golden 659 ms per epoch at 5.8. The parallel regions that
exist (the PASS A model build, the PASS B station build, the carry fold, the threaded carry algebra) are short; the
rest of the epoch runs on one thread. This card ranks the serial stages by wall time, decides for each whether its
work is independent per station, per constellation or per satellite, and moves the largest independent ones into
parallel regions without moving an output bit.

## 2. Method

1. **Profile.** Two 2 h arms (field golden; sim golden from 00:30:15) on the Release build of main, one at a time,
   at above-normal priority (`scripts/cost_attrib/abba.sh`, `run_timed.ps1`): the run-end section timers the goldens
   arm (`network.trace_section_timers`), and a second pair on a scratch build that times every seam of the epoch
   spine, every observer row, every solve stage and harvest step, and every covariance consumer by its caller (a
   hierarchical wall-clock accumulator on the main thread; the patch is kept in the gate records and is not part of
   the branch).
2. **Rank and classify** each serial stage: its wall time per epoch, and whether its work splits into independent
   per-station (or per-satellite) pieces whose results are combined in a fixed order.
3. **Build** the largest independent stages as parallel regions under the contract: each iteration writes only its
   own slot; the shared result is combined serially in the order the serial code uses today, so every floating-point
   sum keeps its association; no log, trace or entity-table write inside a parallel region (the V12 lint); an
   exception inside the region is caught per iteration and rethrown at the serial point where the serial code would
   have thrown it. A per-thread partial sum (the `parallel_reduce` failure) is excluded by construction.
4. **Gate** each change (section 3) and measure its wall-time delta on 2 h arms of both goldens.

## 3. Gates (frozen)

- **G1, identity.** One Release build; each golden (field 2 h; sim 2 h from 00:30:15) run with the new key off and
  with it on, everything else the golden: `products.txt` (comment lines dropped), `logs/closed_loop_trace.csv`,
  `logs/network_epoch_status.csv` and every other CSV under `logs/` that both write (wall-clock columns dropped), by
  `scripts/prm_ab.py`'s `compare_dirs`. Bar: every file identical, on both beds. A difference sends the change back
  or drops it; it is never explained away.
- **G2, off is main.** The changed build with every new key off against the build of main `a78ba68`, both goldens,
  the same files. Bar: identical (standing invariant 6).
- **G3, unit suite.** The full Debug `gnss_tests.exe` and the new unit tests of each change (bit-for-bit against the
  serial arithmetic, `memcmp`). Bar: no failure.
- **G4, lint.** `python scripts/nom_epoch_lint.py` passes.
- **G5, cost.** The epoch-loop wall (`Network[timing]`) of the key-on and key-off arms of G1, same build, one run
  at a time on the same machine state. Reported per golden with the machine load of each run; a change whose gain is
  inside the run-to-run spread of two key-off runs is reported as no gain.
- **Recommendation.** Arming is the author's decision. The card recommends a key when G1 to G4 pass and G5 shows a
  gain on both beds.

## 4. Results

### 4.1 The profile (2026-10-29)

Release build of main `a78ba68` with the scratch timer, 2 h arms, above-normal priority. Field golden: 1 206 ms per
epoch at 10 to 30 % machine load (`runs_pes/_rap_nc_prof2_f_ge_1`; the unpatched build read 1 270 ms in
`_rap_nc_prof_f_ge_1`, so the timer's overhead is within the run-to-run spread). Sim golden: the scratch run met
another session's build (802 ms per epoch at 75 % load, `_rap_nc_prof2_s_ge_2`), so its column is read for shares
and ranking only; the unpatched sim run at a quiet moment read 659 ms per epoch (`_rap_nc_prof_s_ge_2`). Both runs
keep 4.2 to 5.8 of 20 logical processors busy on average, and 26 to 35 % of their CPU time is kernel time.

The serial stages, ranked by the field column (ms per epoch, the path in the epoch spine in brackets):

| stage | field | sim (loaded) | independent per | decision |
| --- | --- | --- | --- | --- |
| station normals in the serial scatter (`networkPassB_` / `networkAssemblyConsume`, `est.addStation`) | 203 | 93 | station; the border sum is elementwise, in station order | built: `network.parallel_station_normals` |
| border-known variance of the handover ledger (`leveling` / `networkHarvestHandoverLedger`) | 139 | 46 | station | not built: card CVF removes the product with an all-zero matrix (about 90 % of it) |
| redundancy census (`frame-diag` / `networkRedundancyCensus`): block cofactors 79, row redundancy 44 | 125 | 72 | station (pure kernel reads; the walk's ladders, events and traces are serial) | built: `network.parallel_redundancy_census` |
| Schur reduce (`solve-glue` / `networkSbasisCompletion` / `reduce`) | 120 | 72 | station terms summed into one border, in station order | built: `network.parallel_schur_columns` (split by columns, not by stations) |
| iono traces (`iono-diag`): conditional variance 78, of which the covariance copy about 48 | 87 | 23 | station | not built: after CVF the remaining 30 ms is the next candidate |
| ambiguity harvest (`leveling` / `networkHarvestAmbiguityStation`) | 84 | 68 | station, with arc-table writes interleaved | not built: needs a compute/write split of the harvest |
| PASS A slip scans (`slip-scans`) | 55 | 39 | station (the model build is already parallel) | not examined further |
| coupling exact prepare (`pre-asm`) | 45 | 11 | | not examined |
| PASS B station build (already parallel) | 37 | 21 | | |
| post-gate dense prior fold (`networkPostGatePriors`) | 28 | 9 | shared border | serial by nature |
| RHS attribution trip, post-fit sigma0 (frame-diag observers) | 40 | 36 | | not examined |
| DD fractional trace, joint covariance (`dd-frac`) | 23 | 29 | station pair | not examined |
| primary solve and rank gate | 38 | 13 | the shared system | serial by nature |

The three built stages are 448 ms of the field epoch (37 %) and 237 ms of the loaded sim epoch (37 %).

**Why the reduce is split by columns.** The deleted `parallel_reduce` gave each thread a range of stations and
summed the per-thread partial matrices afterwards: every border element was then summed in a different grouping,
ULP-different from the serial loop, and the network's conditioning carried the difference forward. Here every
thread owns a block of border columns and subtracts every station's term from it, in ascending station order, as
the same fused product-subtraction the serial loop makes. Each element therefore receives the same operations in
the same order. What remains is whether Eigen's kernel sums an element of a block product as it sums that element
of the whole product. It does when both take the blocked kernel in one depth panel and their row and column panels
are cut at multiples of the kernel's 4-row and 4-column tiles. The check reads that from Eigen's own blocking
heuristic and otherwise declines to the serial loop. A first build required one column panel as well. That
declined on every golden epoch (240 of 240), because at about 418 shared columns Eigen's whole product already
cuts its columns into two panels. Panels cut at tile multiples keep every element in its tile, so the second build
drops that requirement. The unit test requires the split to run on the network's shape and on every residue
of the shared size modulo 4.

### 4.2 Identity and the other gates (G1 to G4), on main `a78ba68`

Executables under `D:/GnssProcessorSupport/worktrees/pes/build/x64-Release/`: candidate `gnss_processor_pes_c3.exe`
(sha256[:16] `64d24097d86f7bef`, branch `perf/pes` before the merge of main); main `a78ba68` built in its own worktree,
`D:/GnssProcessorSupport/worktrees/pes_base/build/x64-Release/gnss_processor_pes_base.exe` (`66333b5e42827350`). Arms
`runs_pes/_rap_pes_g3_{f,s}_{off,all,sn,sc,rc}` (2 h; `sn`, `sc`, `rc` one key each, `all` the three) and
`runs_pes/_rap_pes_g2_{f,s}_off` (main). Comparison: `gates/pes/pes_cmp.py` (`scripts/prm_ab.py`'s `compare_dirs`, all
CSVs), records `gates/pes/g3_cmp_part{1,2}.log`, `g2_cmp.log`.

| gate | field golden | sim golden |
| --- | --- | --- |
| G1, all three keys on against off | IDENTICAL (36 files) | IDENTICAL (36 files) |
| G1, `parallel_station_normals` alone | IDENTICAL | IDENTICAL |
| G1, `parallel_schur_columns` alone | IDENTICAL | IDENTICAL |
| G1, `parallel_redundancy_census` alone | IDENTICAL | IDENTICAL |
| G2, every key off against main | IDENTICAL | IDENTICAL |

The on-path counters of the `all` arms (invariant 4): station normals 13 919 (field) and 14 460 (sim) formed in parallel,
0 serially, all committed; Schur reduce 240 of 240 and 241 of 241 split, 0 declined; redundancy census 1 392 of 1 392
and 1 500 of 1 500 swept station-epochs computed in parallel (the census sweeps every station on its stride epochs, so
its cost lands on about one epoch in ten). A first candidate (`c2`) declined the Schur split on every epoch (section
4.1) and its field `all` arm was identical too; it is not counted above.

- **G3.** Full Debug `gnss_tests.exe` on the candidate tree: 3 624 tests, 3 591 passed, 33 skipped (fixture-gated: SIF,
  GPT3 grid, bundled products), 0 failed (`gates/pes/g3_unit_suite.log`). The 6 new cases of
  `tests/network/test_neq_parallel_stages.cpp` compare against the serial arithmetic with `memcmp`: form and commit
  (dense, sparse, with and without a prior), a rejected station (message, null-direction record, no trace in the
  border, no second commit), and the column-split reduce over every residue of the shared size modulo 4, a border Eigen
  cuts into two column panels, 3 and 20 threads, with the split required to run on each of those shapes and on the
  network's own.
- **G4.** `python scripts/nom_epoch_lint.py` PASS (V12: 94 binding functions, 94 rows, 0 unbound writes);
  `scripts/counters_split_check.py`, `scripts/nom_n4_verify.py --whole`, `scripts/make_golden_config.py --verify`,
  `scripts/config_posture_check.py --verify-loader` (0 coverage gaps), `scripts/ledger_render.py --check` and
  `scripts/docs_index_check.py` pass.

### 4.3 Cost (G5), on main with card CVF

Card CVF merged into main while this card ran (`1527cf9`). The branch merged main (`b09c353`, conflicts only in the
lists of `CMakeLists.txt`, `INDEX.md` and `CARDS.md`), and the cost was measured on that build, the one the keys would
be armed on: `gnss_processor_pes_c4.exe` (sha256[:16] `0845299169e2b0bd`). The 184 unit cases of the solve kernel, card
CVF, the block-Baarda, congruence and redundancy censuses and the estimator pass on it in Debug
(`gates/pes/m4_unit_targeted.log`). Arms `runs_pes/_rap_pes_m4{a,b}_{f,s}_{off,all}`, run one at a time in the order
off, all, all, off on each golden, at above-normal priority, with 0.9 to 1.2 logical processors of foreign load; every
off and all pair is again IDENTICAL (`gates/pes/m4_cmp.log`).

| golden (2 h) | keys off, ms per epoch | all three on | change | busy logical processors |
| --- | --- | --- | --- | --- |
| field (58 stations) | 980.1, 989.5 | 705.4, 714.2 | -27.9 % | 5.3 to 8.9 |
| sim (60 stations, from 00:30:15) | 584.2, 586.4 | 468.5, 464.6 | -20.3 % | 6.6 to 9.5 |
| field, 305 stations (`runs_nom/bof_bases/golden_field_305.json`, 30 min) | 3 952 | 2 384 | -39.7 % | 2.7 to 5.4 |

The 305-station pair is one pair over the first 30 minutes (ignition, about 40 % of the settled cost card BOF read);
its key-on arm ran with more foreign load (2.3 against 0.9 logical processors). It is identical too (18 161 station
forms in parallel, 60 of 60 reduces split, 1 805 census station-epochs).

Per stage (section timers, ms per epoch; field the mean of the two arms of each side, sim one arm each):

| stage | key | field off | field on | sim off | sim on |
| --- | --- | --- | --- | --- | --- |
| station normals (`est.addStation`) | `parallel_station_normals` | 198.3 | 47.9 (4.1x) | 116.2 | 24.3 (4.8x) |
| Schur reduce | `parallel_schur_columns` | 119.1 | 28.9 (4.1x) | 60.6 | 13.3 (4.6x) |
| frame diagnostics (the census and the other observers of the slot) | `parallel_redundancy_census` | 161.6 | 60.7 (2.7x) | 95.0 | 48.7 (2.0x) |

On the field bed the three stages return 341 ms per epoch and the loop 275 ms: the other sections are together about
66 ms per epoch longer with the regions on. This is not attributed here; memory traffic and the allocator's kernel time
(already 26 to 35 % of the process time) are candidates.

### 4.4 Recommendation

All three keys pass G1 to G5 on both goldens. Each is byte-identical to its serial path by construction and at any
thread count, so, unlike `parallel_carry`, a reference made with them on holds at another thread count. Arming
changes no output, so no new baseline pair is needed. **Recommended: arm all three on both goldens** (the author's
decision: the ledger rows go from UNVALIDATED to ARMED with the golden pins). Together they shorten the golden loop
by 28 % (field) and 20 % (sim) on top of card CVF, and by 40 % at 305 stations, where the serial stages weigh most.

## 5. Arming (2026-10-29)

The author's decision on reading section 4.4, 2026-10-29: arm all three.

- The three ledger rows move from UNVALIDATED to ARMED (`golden` true), as `network.sparse_station_normals` did (card
  NETCOST). They are not posture rows: a run that leaves them off loses wall time and nothing else.
- The manifest pins the three keys in both goldens beside `network.sparse_station_normals`, with a dated comment and
  the armed list of both descriptions extended; `scripts/make_golden_config.py` regenerated both. `--verify` passes
  (78 recommended knobs armed in regime on the sim golden, 82 on the field golden, the documented exceptions
  unchanged), the ledger view is re-rendered, and `config_posture_check.py --verify-loader` reports 0 coverage gaps.
- The loader default stays false (the standing rule: a flipped default invalidates snapshot fingerprints).
- Each regenerated golden differs from its G5 arm `_rap_pes_m4a_{f,s}_all` only in the run length (6 h against 2 h),
  the product path and, on the sim, the start offset every sim arm adds, so sections 4.2 and 4.3 hold for the goldens
  as generated.
- Provenance hashes. The generator records a sha256 over the bytes of each base and of the ledger. This worktree checks
  files out with CRLF line endings (`core.autocrlf` true), so a regeneration here moves the recorded base hash with no
  content change. The goldens were regenerated from the committed LF bytes: both base hashes are unchanged and the
  ledger hash is that of the committed ledger. The previous goldens' ledger hash, `6edf48e13b59d501`, is that of a
  CRLF working copy of the ledger blob `e9d27ef12bacc0fa`.

**Smoke.** Each regenerated golden for 0.1 h (12 epochs on the field golden, 13 on the sim golden from 00:30:15)
against the same golden with the three keys set false, one run at a time on `gnss_processor_pes_c4.exe` (arms
`runs_pes/_rap_pes_arm_{f,s}_{armed,off}`, record `gates/pes/arm_smoke.log`): both pairs IDENTICAL (`products.txt`
without comment lines, `closed_loop_trace.csv`, `network_epoch_status.csv` and every other CSV both wrote). The
run-end counters show each stage active on the goldens as generated: on the field golden 696 station forms in parallel
and 0 serially over 12 epochs, 12 of 12 reduces split and 116 of 116 census station-epochs ahead of the walk; on the
sim golden 780, 0, 13 of 13 and 120 of 120 over 13 epochs. The loop over these ignition epochs, keys off to on: field
1 029 to 700 ms per epoch, sim 597 to 453 (a smoke figure; section 4.3 holds the cost).

**Standing.** ARMED on both goldens on branch `perf/pes`, not merged. Arming moves no output bit (section 4.2), so the
posture's baseline pair `runs/_rf0hv17_{field,sim}6` stands and no new pair is run; a reference made with the keys on
holds at any thread count (section 4.4).

## Owed

- The arming decision (section 4.4) and the merge of `perf/pes`. The arming: done 2026-10-29 (section 5); the merge
  remains.
- The next serial stages of section 4.1, each with its own key and the same contract: the ambiguity harvest (84 ms per
  field epoch; its arc-table writes need a compute and write split), the iono traces' conditional variances (about 30
  ms after card CVF), the PASS A slip scans (55 ms), the coupling preparation (45 ms) and the RHS attribution trip. The
  within-epoch re-solve still adds its stations serially (`RaimWithinEpoch`, event epochs only) and can use
  `formStations`.
- The 66 ms per field epoch that the other sections grow with the regions on (section 4.3).
- A settled 305-station pair (2 h or more) for the scaling figure. Done 2026-11-06 (section 7.6). The next serial stages
  and the 66 ms above: section 7. The RHS attribution trip and the within-epoch re-solve stay owed.

## 6. 2026-11-02: merged into main

Merged in the integration branch `int/2026-11-02` (merge `8d2a481`), with cards NOM5, RPC, RKT, SMX, SCK, SCR, ARS, CRS, SCB and PES and the NTRIP credentials loader, then fast-forwarded into main. Gate against the Release build of main `aed227f` (`gnss_processor_closeint.exe`, sha256[:16] `3c8bc25102a7cad7`; candidate `gnss_processor_int1102.exe`, `7f4bd4729069be0e`): both 2 h goldens, `products.txt` and every CSV identical (the wall-clock columns dropped); the 32 user-side arms of `scripts/uom_gate.py`, every CSV identical and the `Toggles:` line the one log difference; the four real-time arms of `scripts/rt_seam_gate.ps1`, 34 of 34 CSVs identical; Debug `gnss_tests` 3833 tests in 523 suites, 3800 passed, 33 skipped, 0 failed; `gnss_simulator_selftest` 99 of 99. The log text that moves is declared: the `Toggles:` line gains `seed_full=off` and the parameter tables gain the inert `tropo_gradient` rows (card NOM5), the PES arming line and run-end counters are new, and the real-time arm `cl` prints one retired-key warning (`network.serialize_sbasis_contract`, set by `config/ppp_rtk_network_brdc_rts.json`; card RKT). Records: `D:/GnssProcessorSupport/worktrees/int1102/runs/_int1102_gate_records/`. The goldens arm the three stages from this merge on. Wall time of the 2 h goldens, reference against candidate on a shared and loaded machine (not a timing grade): field 411 s to 287 s, sim 263 s to 202 s. The items of the Owed list stand.

## 7. 2026-11-06: the next stages (branch `perf/pes-next`, not merged, not armed)

Branch `perf/pes-next` from main `284ec29`, worktree `D:/GnssProcessorSupport/worktrees/pes2`. Executables under its
`build/x64-Release/`: main `gnss_processor_pes2_base.exe` (sha256[:16] `55b166f424bfc86c`), scratch profiler
`gnss_processor_pes2_prof.exe` (`467bb586775b5c1e`, patch `gates/pes2/scratchprof2.patch`, not part of the branch),
candidate `gnss_processor_pes2_c4.exe` (`0af5b6e4d3859e51`). Runs `D:/GnssProcessorSupport/runs_pes2/`, gate records
`D:/GnssProcessorSupport/gates/pes2/`. The machine carried about ten network arms and several builds of other sessions
throughout (12 to 18 of 20 logical processors of foreign load, read per run), so every timing below is a controlled
sequence on above-normal priority, one run of this card at a time, and absolute times are above the quiet figures of
section 4.3.

### 7.1 The profile on main

The scratch timer of section 2 with finer scopes, 2 h arms `_rap_pes2_prof_{f,s}_ge`, read for shares: field loop 1 046.7
ms per epoch, sim 598.8. Per processed epoch (the timer's own ms/ep column divides the sim by its 303 synced epochs):

| stage | field ms | field % | sim ms | sim % | decision |
| --- | --- | --- | --- | --- | --- |
| ambiguity harvest: regressions and coupling rows (`networkHarvestColumn`) | 132.2 | 12.6 | 76.8 | 12.8 | built |
| of which the regression (`stationRegression`) | 25.0 | 2.4 | 13.9 | 2.3 | |
| ambiguity harvest: stash into the arc table | 21.2 | 2.0 | 14.2 | 2.4 | serial: the writes |
| ambiguity harvest: monitor feeds | 5.7 | 0.5 | 3.7 | 0.6 | serial: the writes |
| handover ledger | 20.2 | 1.9 | 15.7 | 2.6 | built, with the harvest |
| receiver code bias harvest | 17.9 | 1.7 | 8.2 | 1.4 | its columns built, with the harvest |
| iono traces: receiver states, ZWD, slant iono | 53.0 | 5.1 | 24.3 | 4.1 | built |
| coupling preparation: all | 47.4 | 4.5 | 13.6 | 2.3 | inverse built |
| of which factor and inverse | 42.2 | 4.0 | 12.2 | 2.0 | |
| PASS A slip scans: all | 76.1 | 7.3 | 62.6 | 10.5 | see below |
| of which the station scan (already parallel) | 44.0 | 4.2 | 32.5 | 5.4 | already parallel |
| of which raw observables | 15.4 | 1.5 | 11.9 | 2.0 | rejected |
| of which the IF-TD drain | 11.1 | 1.1 | 13.8 | 2.3 | rejected |
| of which second code, common band, MW drain | 4.2 | 0.4 | 3.0 | 0.5 | rejected |

**The harvest's cost was not the solve.** Of the 132 ms, the regression is 25; the other 107 are the coupling row: a
`std::map` lookup per border column (about 420 per carried column, about 1 430 carried columns per epoch) and a vector
grown by repeated reallocation of 112-byte elements. The row now reads a table by column and reserves its counted size,
which keeps the same terms in the same order (unit test against the map walk).

**PASS A is rejected as a stage.** Its largest piece is already parallel. The serial remainder is 31 ms on the field and
30 ms on the sim. The IF-TD drain pools over every station, takes the medians and then writes in station order, so it
has no station-independent part of any size. The raw observables split by station only with the roster lookups' counters,
the mismatch counter, the resumption lists and the stepped set made per-station and folded afterwards, for a return of
about 10 ms (1 to 1.5 % of the loop). No key was added.

### 7.2 What was built

Three keys, loader default off, ledger UNVALIDATED, each with a run-end counter line (invariant 4), each acting only with
`parallel_stations` and Eigen pinned to one thread:

- **`network.parallel_carry_harvest`.** The harvest's layouts are read first, then every carried column's regression and
  coupling row is computed into its own slot in parallel; the walk reads each slot once through `networkHarvestColumn`
  and keeps every write to the arc table, counters, traces and marks, in its order. The handover ledger's two kernels are
  computed the same way. A column left unset, or asked for in another form, is computed by the walk. A clone regression is
  not taken ahead: its row step writes the border.
- **`network.parallel_iono_traces`.** Each station's rows of the slant iono, ZWD, gradient and receiver-state traces are a
  function of the solved kernel, formed into the station's own buffer, serially or in parallel; the buffers are written in
  station order. The back-substitutions, the conditional variances and the formatting all run in the region.
- **`network.parallel_coupling_prepare`.** The inverse of the exact update's preparation (`LDLT::solve` on the identity) is
  split over column blocks whose width is a multiple of 4, on the reasoning of section 4.1: a column's arithmetic in the
  blocked triangular solve depends on the depth and row panels and on its 4-column tile, not on the block that holds it.
  `ldltInverseColumns` takes the split only where Eigen's heuristic gives the blocks the panels of the whole solve. The
  factorization stays serial.

Not keyed, identical by construction and gated with the rest: the coupling row's table and reservation above, and one team
size in every region (section 7.3).

### 7.3 One team size: the owed 66 ms

Section 4.3 left 66 ms per field epoch unattributed: sections other than the three stages grew with the regions on. The
first candidate of this card (`c2`) showed the coupling key returning nothing on the field and costing about 10 ms on the
sim, with the block count (18 or 19) as its team size (arms `_rap_pes2_m1*`, `_rap_pes2_g1_*`; all three keys together
read -9.8 % on the field and -10.3 % on the sim on that candidate). A micro-benchmark on this toolchain (`gates/pes2/ompbench`, MSVC
`vcomp`, 60 iterations of about 0.5 ms per region, above-normal priority, the same load) gives the reason:

| team sizes of successive regions | wall per region | kernel CPU per region |
| --- | --- | --- |
| 20, 20, 20, ... | 2.6 ms | 5.7 ms |
| 20, 18, 20, 18, ... | 11.3 to 12.0 ms | 46.8 to 51.9 ms |

Each change of team size rebuilds the team: about 9 ms of wall and 45 ms of kernel CPU. At normal priority on the same
loaded machine the two rows read 10.8 and 37.1 ms of wall. The armed Schur reduce changed size twice per epoch (20 for
its solves, the block count 17 to 19 for its columns, 20 again in the next region), and the cost landed in the reduce and
in the regions after it. From main to the candidate with the new keys off, the reduce's own section falls from 74 to 35
ms (field) and 33 to 14 ms (sim) and PASS A's from 87 to 51 ms and 74 to 46 ms, with no change to the work of either
stage. PASS A alone returns 36 of the 66 ms that section 4.3 could not attribute. Every region of the card now uses the
run's thread count; a block count below it leaves threads idle in the region. Which thread runs a block does not enter
its arithmetic.

### 7.4 Gates on the final candidate

Arms `_rap_pes2_m3{a,b}_{f,s}_*` (2 h), main `_rap_pes2_m2a_base_{f,s}_off`; comparison `gates/pes2/pes2_cmp.py`
(`prm_ab.py`'s `compare_dirs`, `products.txt` and every CSV), record `gates/pes2/final_cmp.log`.

| gate | field golden | sim golden |
| --- | --- | --- |
| G1, `parallel_carry_harvest` alone against off | IDENTICAL (36 files) | IDENTICAL (36 files) |
| G1, `parallel_iono_traces` alone against off | IDENTICAL | IDENTICAL |
| G1, `parallel_coupling_prepare` alone against off | IDENTICAL | IDENTICAL |
| G1, the three together against off | IDENTICAL | IDENTICAL |
| G2, every new key off against main `284ec29` | IDENTICAL | IDENTICAL |
| the two off arms of one executable | IDENTICAL | IDENTICAL |

On-path counters of the `hv_io_cx` arms: field 381 533 carried columns computed ahead and 381 533 read, 13 919 ledger
stations, 13 919 station-epochs of trace rows, 239 of 239 inverses split, 0 declined; sim 416 410 and 416 410, 14 460,
14 460, 240 of 240, 0 declined. The earlier candidates `c2` and `c3` were identical on the same gates.

- **G3.** Full Debug `gnss_tests.exe` on the final tree: 3 839 tests in 526 suites, 3 806 passed, 33 skipped
  (fixture-gated), 0 failed (`gates/pes2/g3_unit_suite_final.log`). The 6 new cases of
  `tests/network/test_harvest_coupling_parallel.cpp` compare with `memcmp` against the serial arithmetic: the inverse over
  column blocks against the whole solve on every residue of the order modulo 4 and a network order, with the split required
  from order 256 up; the decline with Eigen threaded and inside a region; the exact update's factor; the coupling row against
  the map walk it replaced; the columns ahead inside a parallel region; a column the kernel refuses stays for the walk.
- **G4.** `nom_epoch_lint.py` PASS (V12: 94 binding functions, 94 rows, 0 unbound writes), `counters_split_check.py`,
  `nom_n4_verify.py --whole`, `make_golden_config.py --verify`, `config_posture_check.py --verify-loader` (0 coverage gaps),
  `ledger_render.py --check` and `docs_index_check.py` pass. `nom_hv1_verify.py` fails on main itself (stale paths from
  before the header move) and is not a live gate; the harvest's order constraints are kept by hand and stated in the header.

### 7.5 Cost (G5)

Controlled sequences, one run at a time, above-normal priority, 12 to 15 logical processors of foreign load. `off` is the
golden as it is on the candidate; `all` adds the three keys.

| golden (2 h, ms per epoch) | main | off | all | off against main | all against off | all against main |
| --- | --- | --- | --- | --- | --- | --- |
| field, sequence `m2` | 1 062.1, 1 011.9 | 877.2, 848.8 | 732.4, 742.7 | -16.8 % | -14.5 % | -28.9 % |
| sim, sequence `m2` | 609.0, 633.7 | 558.7, 525.8 | 466.1, 457.5 | -12.7 % | -14.8 % | -25.7 % |
| field, sequence `m4` | | 866.9, 855.3 | 719.8, 725.5 | | -16.1 % | |
| sim, sequence `m4` | | 511.8, 501.5 | 438.5, 439.6 | | -13.3 % | |

Sequence `m2` runs main, off, all, all, off, main on each golden (candidate `c3`, which differs from the final build only in the
team size of a network under 20 stations); `m4` runs off, all, all, off on the final build. Each pair is identical again.

Per key, one sequence off, each key alone, all, off (arms `m3`; off is the mean of the two; ms per epoch):

| golden | off | `parallel_carry_harvest` | `parallel_iono_traces` | `parallel_coupling_prepare` | all three |
| --- | --- | --- | --- | --- | --- |
| field | 872.7, 848.3 | 812.8 (-5.5 %) | 817.3 (-5.0 %) | 838.7 (-2.5 %) | 727.8 (-15.4 %) |
| sim | 510.6, 527.5 | 475.1 (-8.5 %) | 492.5 (-5.1 %) | 499.6 (-3.8 %) | 473.0 (-8.9 %) |

The spread of two off arms is 2.8 % (field) and 3.3 % (sim). The sim's `all` arm of this sequence met a busier moment than
its keys alone suggest (its PASS A and frame diagnostics sections read 7 to 12 % above the other arms); the matched sequences
above are the reading for the three together.

Per stage (section timers, ms per epoch; main from `m2`, off and on from `m4`):

| section | key | field main | field off | field on | sim main | sim off | sim on |
| --- | --- | --- | --- | --- | --- | --- | --- |
| leveling (the harvest) | `parallel_carry_harvest` | 188 | 103 | 53 (1.9x) | 132 | 75 | 45 (1.7x) |
| iono traces | `parallel_iono_traces` | 52 | 45 | 3.6 (12.5x) | 26 | 20 | 2.1 (9.5x) |
| pre-assembly (the coupling preparation) | `parallel_coupling_prepare` | 65 | 63 | 36 (1.8x) | 29 | 29 | 22 (1.3x) |
| Schur reduce | none: one team size | 74 | 35 | 32 | 33 | 14 | 14 |
| PASS A slip scans | none: one team size | 87 | 51 | 50 | 74 | 46 | 47 |

Processor time rises while the wall falls: on the field `off` reads 1 280 s of CPU (484 s kernel) and `all` 1 560 s (697 s
kernel). The work is the same, so the extra is the runtime's overhead, most of it kernel time; it is read here as the idle
workers waiting after each added region and is not attributed further. It does not lengthen a run, but it takes processors
from other sessions on a shared machine.

### 7.6 The settled 305-station pair

Base `runs_nom/bof_bases/golden_field_305.json` (card BOF; written before this card, so it arms none of the stages), 2 h, 240
epochs, 303 stations in the solve, 433 shared columns, final build, one run at a time in the order all six, none, three,
above-normal priority, 15 to 18 logical processors of foreign load. Arms `_rap_pes2_x1_x_{off,on3,on3_hv_io_cx}`.

| stages on | ms per epoch | hour 1 | hour 2 | against none | CPU s (kernel) |
| --- | --- | --- | --- | --- | --- |
| none | 7 539 | 7 259 | 7 630 | | 3 711 (458) |
| the three armed | 5 037 | 4 164 | 5 705 | -33.2 %, hour 2 -25.2 % | 4 527 (765) |
| all six | 3 866 | 3 339 | 4 189 | -48.7 %, hour 2 -45.1 % | 5 043 (1 210) |

All three arms are identical (36 files per pair, `gates/pes2/x305_cmp.log`), with 72 621 station forms in parallel, 240 of 240
reduces split, 239 of 239 inverses split, 2 056 436 carried columns computed ahead and read, and 7 215 census station-epochs.
The absolute times sit above the 30-minute pair of section 4.3 (3 952 and 2 384 ms on a quiet machine) because the foreign load
takes processors, so the ratios are the reading, one run each. The three-stage arm's second hour reads 37 % above its first,
against 5 % for no stage and 25 % for all six, so its figure may carry a busier machine. Against the three armed stages, the
three new keys return -23.3 % (hour 2 -26.6 %). They weigh more here than on the 58-station goldens because the harvest and the
traces scale with the station count: leveling and the iono traces are 33 % of the three-stage loop at 305 stations and 17 % at
58 stations. The iono traces fall from 244 to 17 ms per epoch and leveling from 1 413 to 849 ms.

### 7.7 Recommendation and follow-ups

All three keys pass G1 to G5 on both goldens. **Recommended: arm all three** (the author's decision: the ledger rows move from
UNVALIDATED to ARMED with the golden pins, as in section 5). The unkeyed changes reach the goldens with the merge, at
identical output.

Follow-ups this card found and did not build:

- **The S-basis completion's eigenvectors.** `completeSbasisDimension` computes eigenvectors on every epoch: 115 ms on the
  field (11 % of the loop in the profile of section 7.1), 36 ms on the sim. Both 2 h goldens fold 0 completion rows, so the
  vectors are read on no epoch. A scratch check (`gates/pes2/ompbench/eigbench.cpp`, order 420) reads 175 ms with vectors
  against 37 ms for eigenvalues only, with the eigenvalues bit-identical. Eigenvalues first, vectors only when the count
  exceeds the declared dimension, would return about 8 % (field) and 4 % (sim) at identical output. The rank gate then
  decomposes the same matrix again (29 ms) when no row was added.
- **The instruction set.** `CMakeLists.txt` sets no `/arch`, so the dense kernels run on SSE2. An AVX2 build is not
  byte-identical (fused multiply-add and other kernel tiles) and needs a new baseline pair: the author's decision.
- **The waiting workers.** `OMP_WAIT_POLICY` and `parallel_max_threads` at the physical core count would lower the idle
  processor time of section 7.5; neither is measured on a golden arm here.

## 8. 2026-11-07: the S-basis completion on eigenvalues first (branch `perf/pes-next`, not merged, not armed)

The first follow-up of section 7.7, built at the author's word. Final executable `gnss_processor_pes2_c5.exe` (sha256[:16]
`014100c019242d4d`), arms `_rap_pes2_{m5,m6}*` and `_rap_pes2_x2_*`, records under `gates/pes2/`. Section 7's keys are in this
build too, so its gates are repeated here on the build a merge would carry.

### 8.1 What was built

`network.sbasis_values_first`, loader default off, ledger UNVALIDATED, with a run-end counter line. `completeSbasisDimension`
decides on the eigenvalues alone and computes the eigenvectors only on an epoch whose near-null count exceeds the declared
dimension. On an epoch with nothing folded the rank gate judges the same matrix, so it reads those eigenvalues instead of
decomposing again (`EpochWorkspace::sbc_spectrum`); a heal's re-gate decomposes as before. An epoch that needs the vectors
pays one extra eigenvalue decomposition.

Output cannot move through this key if the eigenvalues do not: the tridiagonal reduction and the QL sweeps do the same
arithmetic on the diagonal and subdiagonal with or without the vectors, and the new unit case pins that against Eigen with
`memcmp`. The completion's rows are computed from the same full decomposition as before.

### 8.2 Gates

Arms `_rap_pes2_m5a_{f,s}_*` (2 h) against each bed's off arm, main from `_rap_pes2_m2a_base_{f,s}_off`; record
`gates/pes2/c5_cmp.log`.

| gate | field golden | sim golden |
| --- | --- | --- |
| G1, `sbasis_values_first` alone against off | IDENTICAL (36 files) | IDENTICAL (36 files) |
| G1, `parallel_carry_harvest` alone against off | IDENTICAL | IDENTICAL |
| G1, `parallel_iono_traces` alone against off | IDENTICAL | IDENTICAL |
| G1, `parallel_coupling_prepare` alone against off | IDENTICAL | IDENTICAL |
| G1, those three together against off | IDENTICAL | IDENTICAL |
| G1, all four against off | IDENTICAL | IDENTICAL |
| G2, every new key off against main | IDENTICAL | IDENTICAL |

Main has gained 14 commits since `284ec29`, none touching `src/`, `include/`, `config/` or `CMakeLists.txt`, so main's source is
the reference's. On-path counters of the all-four arms: field 240 epochs decided on the eigenvalues alone, 0 computed the
vectors, 240 gate evaluations read the eigenvalues; sim 241, 0 and 241. The goldens therefore never run the vector path in 2 h,
and the 305-station bed does (section 8.4): it folds rows on 67 of 240 epochs, and those arms are identical too. The unit cases
run the path on every outcome: nothing to complete, over-specified, extras to fold, an unseparated extra skipped and a burst
over the cap, requiring the vectors only on the outcomes that read them.

- **G3.** Full Debug `gnss_tests.exe`: 3 842 tests in 527 suites, 3 809 passed, 33 skipped (fixture-gated), 0 failed
  (`gates/pes2/g3_unit_suite_c5.log`). New: `tests/network/test_sbasis_values_first.cpp`, 3 cases.
- **G4.** `nom_epoch_lint.py`, `counters_split_check.py`, `nom_n4_verify.py --whole`, `make_golden_config.py --verify`,
  `config_posture_check.py --verify-loader` (0 coverage gaps), `ledger_render.py --check` and `docs_index_check.py` pass
  (`gates/pes2/lints_c5.log`).

### 8.3 Cost (G5)

Controlled sequences on the final build, one run at a time, above-normal priority, 12 to 16 logical processors of foreign
load. Sequence `m7` is the headline: main, all four keys, all four, main, on each golden.

| golden (2 h, ms per epoch) | main | all four keys | change |
| --- | --- | --- | --- |
| field | 1 135.8, 1 108.7 | 653.8, 658.5 | -41.5 % |
| sim | 668.6, 682.8 | 438.6, 442.5 | -34.8 % |

Main against all four is identical (`products.txt` and every CSV). With the three keys of section 7, the same comparison read
-28.9 % (field) and -25.7 % (sim).

The key alone, sequence `m6` (off, the key, all four, all four, the key, off) and the first half of `m5`:

| golden | off | `sbasis_values_first` alone | all four | key against off |
| --- | --- | --- | --- | --- |
| field, `m6` | 1 014.7, 921.6 | 834.7, 796.9 | 639.4, 640.4 | -15.7 % |
| sim, `m6` | 517.8, 568.5 | 483.2, 490.5 | 467.6, 465.6 | -10.4 % |
| field, `m5` | 898.9, 909.8 | 778.8, 798.0 | | -12.8 % |

The off arms of `m6` differ by about 10 % on each bed (one of each met a busier machine), so the key alone reads -13 to -16 %
on the field and -7 to -10 % on the sim. The section that holds it is the steadier reading: the solve section falls from 332 to
202 ms per epoch on the field and from 105 to 67 ms on the sim (`m6`). The completion and the gate decompose a matrix of about
415 columns on the field and about 280 on the sim. The cost of the decomposition grows with the cube of the order, so the field
gains more.

### 8.4 305 stations

Arms `_rap_pes2_x2_x_{on3_hv_io_cx,on3_hv_io_cx_sv}`, 2 h, one run each, one after the other, 15 to 16 logical processors of foreign
load: six stages 4 020 ms per epoch, all seven 3 995 ms. On the seven-stage arm 173 epochs were decided on the eigenvalues
alone, 67 computed the vectors, and 173 gate evaluations read the eigenvalues. The solve section falls from 858 to 809 ms, and the loop's
difference is inside the spread of the other sections. The completion needs the vectors on 67 of 240 epochs here and decides on
the eigenvalues alone on the other 173. The key therefore returns most on the goldens' shape, where the vectors are never read,
and little at 305 stations, where the harvest and the reduce dominate.

### 8.5 Recommendation

All four keys pass G1 to G5 on both goldens. **Recommended: arm all four** (the author's decision, as in section 7.7).

What remains of the follow-ups of section 7.7: the instruction set is being measured in its own session; the waiting workers are
not measured.

## 9. 2026-11-07: armed

The author's decision on reading section 8.5, 2026-11-07: arm all four keys and merge the branch.

- The four ledger rows move from UNVALIDATED to ARMED (`golden` true). They are not posture rows: a run that leaves them off
  loses wall time and nothing else.
- The manifest pins the four keys in the sim and field goldens beside the three of section 5, with the descriptions extended
  and a dated comment; the BeiDou lineage takes them through its base. `scripts/make_golden_config.py --only` regenerated
  the three goldens in manifest order, and `--verify` passes (78 recommended knobs armed in regime on the sim golden, 82 on the
  field golden, 76 on the BeiDou golden, the documented exceptions unchanged). Each golden's diff is the four keys and its
  provenance lines (date, ledger hash, pins); the base hashes are unchanged, since the generator now reads CRLF as LF.
  `config_posture_check.py --verify-loader` reports 0 coverage gaps.
- The loader default stays false (a flipped default invalidates snapshot fingerprints).
- Main's 14 commits since `284ec29` (docs and scripts only) are merged into the branch (`f8ea40a`; the generated ledger view
  conflicted and was re-rendered from the merged JSON), so main reaches this posture by a fast-forward.

**Smoke.** Each regenerated golden for 0.1 h against the same golden with the four keys set false, one run at a time on the
final executable (arms `runs_pes2/_rap_pes2_arm_{f,s,b}_{armed,off}`, record `gates/pes2/arm_smoke_cmp.log`): field, sim (from
00:30:15) and BeiDou pairs IDENTICAL (36 files each). The run-end counters show every stage active on the goldens as
generated, and none on the off arms:

| golden | columns ahead, read | trace station-epochs | inverses split | epochs on eigenvalues alone, gate readings |
| --- | --- | --- | --- | --- |
| field (12 epochs) | 9 768, 9 768 | 696 | 11 of 11 | 12, 12 |
| sim (13 epochs) | 12 530, 12 530 | 780 | 12 of 12 | 13, 13 |
| BeiDou (12 epochs) | 15 397, 15 397 | 696 | 11 of 11 | 12, 12 |

The loop over these ignition epochs, keys off to on: field 866 to 612 ms per epoch, sim 510 to 396, BeiDou 2 406 to 1 663 (a
smoke figure, with 18 logical processors of foreign load; sections 7.5 and 8.3 hold the cost).

**Standing.** ARMED on all three goldens. Arming moves no output bit (sections 7.4, 8.2 and the smoke above), so the baseline
pair `runs/_rf0hv17_{field,sim}6` and the BeiDou baseline arm stand and no new pair is run.
