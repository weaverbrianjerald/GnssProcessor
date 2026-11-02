# PES: the sequential stages of the network epoch, run in parallel at identical output

**STATUS**

- **id:** PES
- **topic:** perf
- **status:** open
- **summary:** ARMED 2026-10-29 at the author's decision (section 5), merged into main 2026-11-02 (branch `perf/pes`, section 6). Three serial stages of the network epoch run in parallel over stations behind keys the loader leaves off (ledger ARMED, pinned true in both goldens): `network.parallel_station_normals` (the station normal blocks formed ahead of the serial scatter, committed in station order), `network.parallel_schur_columns` (the Schur reduce split by border columns, every element summed over the stations in serial order; no per-thread partial sum) and `network.parallel_redundancy_census` (the census's per-station kernels ahead of its serial walk). Key on against off, one build, 2 h: IDENTICAL on both goldens for each key and for the three together; off IDENTICAL to main; full Debug suite 3 591 passed, 0 failed; the lints pass. On main with card CVF the loop is 27.9 % shorter on the field golden, 20.3 % on the sim golden and 39.7 % on a 305-station field arm (30 min), at identical output. Armed: all three; the baseline pair `runs/_rf0hv17_{field,sim}6` stands (section 5).
- **source:** [NETCOST](NETCOST_gps_galileo_cost_attribution_2026-10-18.md) sections 2 and 3 (the section attribution under load); [CVF](CVF_conditional_variance_frame_2026-10-29.md) (the covariance-consumer overheads; merged into main during this card, section 4.3); the auto-memory records of the closed perf doors (`parallel_reduce` deleted by card RKD: per-thread partial sums regroup the Schur subtraction and the network's conditioning amplifies the ULP difference over the carried epochs).
- **knobs / posture:** new `network.parallel_station_normals`, `network.parallel_schur_columns`, `network.parallel_redundancy_census`: bool, loader default false, ledger ARMED and pinned true in both goldens since 2026-10-29 (section 5); nothing else changed.
- **dates in the text:** 2026-10-29
- **hashes / run dirs:** branch `perf/pes` from main `a78ba68` (gates G1 to G4), main merged in at `b09c353` (G5); executables `gnss_processor_pes_c3.exe` (`64d24097d86f7bef`), `gnss_processor_pes_c4.exe` (`0845299169e2b0bd`), main `gnss_processor_pes_base.exe` (`66333b5e42827350`); worktree `D:/GnssProcessorSupport/worktrees/pes`; executables under its `build/x64-Release/`; runs `D:/GnssProcessorSupport/runs_pes/`; gate records `D:/GnssProcessorSupport/gates/pes/`.
- **gates (frozen at pre-registration):** section 3, written before any run of a changed executable.

**Outcome.** Question: which serial stages of the network epoch split by station, and how much of the loop do they
return when run in parallel without moving an output bit? Result: the station normal formation, the Schur reduce and
the redundancy census (37 % of the field epoch) run 2.0 to 4.8 times faster in parallel. Every key-on arm is identical
to key off, and key off to main. The golden loop is 27.9 % shorter (field) and 20.3 % (sim), and 39.7 % at 305
stations (bars: identical, a gain on both beds: met). Standing: ARMED on both goldens at the author's decision,
loader default off, merged into main 2026-11-02 (section 6).

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
- A settled 305-station pair (2 h or more) for the scaling figure.

## 6. 2026-11-02: merged into main

Merged in the integration branch `int/2026-11-02` (merge `8d2a481`), with cards NOM5, RPC, RKT, SMX, SCK, SCR, ARS, CRS, SCB and PES and the NTRIP credentials loader, then fast-forwarded into main. Gate against the Release build of main `aed227f` (`gnss_processor_closeint.exe`, sha256[:16] `3c8bc25102a7cad7`; candidate `gnss_processor_int1102.exe`, `7f4bd4729069be0e`): both 2 h goldens, `products.txt` and every CSV identical (the wall-clock columns dropped); the 32 user-side arms of `scripts/uom_gate.py`, every CSV identical and the `Toggles:` line the one log difference; the four real-time arms of `scripts/rt_seam_gate.ps1`, 34 of 34 CSVs identical; Debug `gnss_tests` 3833 tests in 523 suites, 3800 passed, 33 skipped, 0 failed; `gnss_simulator_selftest` 99 of 99. The log text that moves is declared: the `Toggles:` line gains `seed_full=off` and the parameter tables gain the inert `tropo_gradient` rows (card NOM5), the PES arming line and run-end counters are new, and the real-time arm `cl` prints one retired-key warning (`network.serialize_sbasis_contract`, set by `config/ppp_rtk_network_brdc_rts.json`; card RKT). Records: `D:/GnssProcessorSupport/worktrees/int1102/runs/_int1102_gate_records/`. The goldens arm the three stages from this merge on. Wall time of the 2 h goldens, reference against candidate on a shared and loaded machine (not a timing grade): field 411 s to 287 s, sim 263 s to 202 s. The items of the Owed list stand.
