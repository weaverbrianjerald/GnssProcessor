# PES: the sequential stages of the network epoch, run in parallel at identical output

**STATUS**

- **id:** PES
- **topic:** perf
- **status:** open
- **summary:** Pre-registered 2026-10-29. The GPS + Galileo golden epoch keeps on average 4.2 (field) and 5.8 (sim) of the machine's 20 logical processors busy: most of its wall time is serial work on the main thread. Question: which sequential stages of `networkProcessEpoch_` are independent per station (or per constellation, per satellite), and how much of the loop do they return when they run in parallel under the deterministic contract of `network/assembly/StationBuild.hpp` (per-index slots, no shared write inside a parallel region, every reduction in the serial order it has today)? Each change sits behind its own default-off key, with a ledger row, and is gated as byte-identical with the key on against off in one build.
- **source:** [NETCOST](NETCOST_gps_galileo_cost_attribution_2026-10-18.md) sections 2 and 3 (the section attribution under load); card CVF (the covariance-consumer overheads; on branch `perf/cvf`, not merged, so not linked here); the auto-memory records of the closed perf doors (`parallel_reduce` deleted by card RKD: per-thread partial sums regroup the Schur subtraction and the network's conditioning amplifies the ULP difference over the carried epochs).
- **knobs / posture:** new keys, each default false, ledger UNVALIDATED, unarmed in both goldens (section 4 names them as they land).
- **dates in the text:** 2026-10-29
- **hashes / run dirs:** branch `perf/pes` from main `a78ba68`; worktree `D:/GnssProcessorSupport/worktrees/pes`; executables under its `build/x64-Release/`; runs `D:/GnssProcessorSupport/runs_pes/`; gate records `D:/GnssProcessorSupport/gates/pes/`.
- **gates (frozen at pre-registration):** section 3, written before any run of a changed executable.

**Outcome.** Pending.

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
