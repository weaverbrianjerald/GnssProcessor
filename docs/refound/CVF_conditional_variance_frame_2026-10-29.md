# CVF: the border-known variance read directly, and one datum-frame covariance per solve

**STATUS**

- **id:** CVF
- **topic:** perf
- **status:** open
- **summary:** BUILT and GATED 2026-10-29, not merged (branch `perf/cvf`, uncommitted). Second build: G2 24 of 24 IDENTICAL, real time identical apart from wall-clock text; first build: user side 16 of 16, the field golden loop 9.5 and 10.5 % shorter over the whole run (bar 5 %) and 12.8 and 13.5 % shorter while both members of a pair run, the sim loop 4.2 to 6.8 %, products byte-identical in every pair. The first build moved the per-epoch consumer call count of the timing lines (section 5.3); the timer placement was corrected. G5 passes (section 5.8: the untouched sections move inside the spread of a same-executable pair). Owed: the merge. Pre-registered 2026-10-29. Optimization 2 of card NETCOST (section 5), its own card with its own gate as that card's Owed list requires. Two exact changes to the covariance consumers of the network estimator: (1) `pureConditionalLocalVarianceDiag` returns diag(N_rr⁻¹) without the product with an all-zero n_s x n_s matrix (212 ms per field epoch of 2 127 ms in card NETCOST section 3); (2) the datum-frame shared covariance (the last solve's `shared_cov_` with the datum-pinned and phase-gauge columns zeroed) is built once per solve and read by every consumer that passes the solve's datum, in place of one 1.4 MB copy per consumer call (75 ms per field epoch in `conditionalLocalVarianceDiag` alone). No key: the change is exact by construction and is gated as byte-identical; a gate that is not identical sends it behind a default-off key or drops it.
- **source:** [NETCOST](NETCOST_gps_galileo_cost_attribution_2026-10-18.md) sections 3, 4, 5 and 12.5 (the sub-section timings behind the estimate); [NETCOSTB](NETCOSTB_gps_galileo_cost_continued_2026-10-18.md) section 5 (the attribution reproduced)
- **knobs / posture:** none; no source change to configuration, goldens or ledger
- **dates in the text:** 2026-10-29
- **hashes / run dirs:** branch `perf/cvf` from main `c37d4d6`; worktree `D:/GnssProcessorSupport/worktrees/cvf`; executables under `D:/GnssProcessorSupport/gates/exe/x64-Release/`: control `gnss_processor_cvfctl.exe` (sha256[:16] `9472e3f139a816e4`), first build `gnss_processor_cvf.exe` (`a694972c2479b8ea`), second build `gnss_processor_cvf2.exe` (`1fd1e49a5559b83b`); gate records `D:/GnssProcessorSupport/gates/cvf/`; G5 sides `runs_nom/_rap_g5_{cvfctl,cvf,cvf2}_*`; timing arms `runs_nom/_rap_nc_cvf{A,B}{1..4}_*`; real time `C:/GnssProcessor/runs/rtseam_goldens/{cvfctl,cvf,cvf2}_{dr,cl}`; user side `C:/GnssProcessor/runs/_uom_{cvfctl,cvf}`
- **gates (frozen at pre-registration):** section 4, written before any run of the changed executable

**Outcome.** Question: can the network estimator's two costliest covariance-consumer overheads (a product with an
all-zero matrix and one shared-covariance copy per call) go without moving an output bit? Result: yes. The second
build is identical on the 23-arm gate (24 of 24) and in real time apart from wall-clock text, and the first build on
the user side (16 of 16). The field golden loop is 9.5 to 10.5 % shorter over the whole run and 12.8 to 13.5 % while
both runs of a pair are live (the sim 4.2 to 6.8 %), against a bar of 5 %. The first build moved a timing call count,
since corrected (section 5.3). Standing: every gate passes; not merged (section 5.8).

## 1. Question

Card NETCOST attributed the GPS + Galileo loop of the field golden (2 127 ms per epoch under load) and estimated three
exact optimizations. The first, the sparse station normals, is built and armed (`network.sparse_station_normals`,
13.3 to 13.8 % of the field loop). The second is this card: can the two covariance-consumer costs below be removed
without moving one output bit, and what fraction of the loop do they return?

| cost (card NETCOST section 12.5, field, GPS + Galileo, ms per epoch) | what it is |
| --- | --- |
| `pureConditionalLocalVarianceDiag` 206.6 | per requested local column, `m_cᵀ · 0 · m_c` with an n_s x n_s zero matrix (n_s about 418), plus the zero matrix's own allocation and fill per call |
| `conditionalLocalVarianceDiag`, copy of `shared_cov_` 66.6 | one copy of the 1.4 MB shared covariance per station and per call, then the same pinned and gauge columns zeroed again |

The second cost is not specific to `conditionalLocalVarianceDiag`: `conditionalLocalCovariance`,
`jointConditionalCovariance`, `blockConditionalCovariances`, `rowRedundancy` (the RAIM redundancy, per station) and
`datumFrameSharedCov` each build the same frame with the same seven lines. Every caller in the network producer passes
`plan.shared_datum`, the datum of the solve.

## 2. The two changes, and why each is exact

**Change 1: the border-known variance.** `reconstructStationCovarianceDiag(station, cols, cov, false)` returns, per
column c, `var = n_c(c) + m_cᵀ (cov m_c)` with `n_c = N_rr⁻¹ e_c` and `m_c = N_sr n_c`. With `cov` the zero matrix,
every entry of `cov m_c` is a sum of `0 · m_c(j)`: exactly ±0 when `m_c` is finite, and NaN when any `m_c(j)` is
infinite or NaN. The dot product is then ±0 or NaN. Adding ±0 to a nonzero `var` returns `var` bit for bit; adding it to
a zero `var` can change the sign of the zero. The new kernel function returns `n_c(c)` directly and takes the
original zero-matrix product, bit for bit the old arithmetic, in exactly the two cases where the result could differ:
`var == 0` and a non-finite `m_c`. `m_c` is still formed (n_s x n_local per column, about 4 % of the n_s x n_s
product it replaces) because its finiteness decides the case.

**Change 2: one datum-frame covariance per solve.** The frame is a pure function of three inputs: `shared_cov_`, the
pinned columns of the datum, and `phase_sumzero_gauges_`. `solve()` builds it once from its own datum, right after it
fills `shared_cov_`, and records the pinned columns. A consumer whose datum has the same pinned columns, in the same
order, reads the stored frame; any other datum builds a local frame by the same function, as today. The stored frame
is invalidated at the start of `solve()` (so a solve that throws leaves none), by `reset()`, and by
`setPhaseSumZeroGauges()`, the only writers of the three inputs. Zeroing a row or column writes +0.0 whatever the
order, so the stored frame and a per-call build are the same bytes. The stored frame is written only by the
non-const `solve()`, so concurrent const readers (the parallel station paths) read immutable data.

Both changes keep the `CovTimer` accounting: the per-solve build is timed as a covariance consumer, so the run-end
`cov-consumers` section still holds the frame's cost.

## 3. What is not changed

The kernel's border-inclusive arithmetic, the carry-aware correction, the RAIM, census and harvest logic, every
configuration key, the goldens and the ledger. The sparse station normals (change 1 of card NETCOST) and change 3 of
card NETCOST (the reduce subtract, the redundancy census product and the joint conditional covariance) are untouched;
change 3 stays owed on card NETCOST.

## 4. Gates (frozen 2026-10-29, before any run of the changed executable)

The control is the Release executable of main `c37d4d6` built in the card's own worktree before the change
(same build flavour and tree as the candidate).

- **G0 (unit).** New cases: change 1 against the old zero-matrix form, bit for bit (`std::memcmp` on the doubles), on
  random stations including a zero diagonal entry and an infinite and a NaN regression entry; change 2 against a
  per-call build, bit for bit, for the consumer families, with the stored frame invalidated by `reset()`,
  `setPhaseSumZeroGauges()` and a datum whose pinned columns differ.
- **G1 (suite).** The full Debug `gnss_tests` passes (fixture-gated skips aside).
- **G2 (identity, network producer).** The 23-arm G5 gate, candidate against control, run as a same-era pair
  (`gates/g7/gate_pair.sh`): 24 of 24 IDENTICAL.
- **G3 (identity, user side and real time).** `uom_gate.py` quick set 16 of 16 IDENTICAL; the real-time arms identical
  apart from wall-clock text.
- **G4 (gain).** Field and sim golden 2 h, candidate against control, members of each pair run at the same time with
  the launch order swapped in the second pair (`scripts/cost_attrib/abba.sh`). Bar: the field loop at least 5 %
  shorter (card NETCOST's estimate is 12 to 13 %); the sim loop reported; the products of each pair byte-identical
  (records, `^#` stripped); the foreign load stated.
- **G5 (attribution).** The run-end `cov-consumers` section falls by at least half of the 206.6 + 66.6 ms that section
  12.5 of card NETCOST attributes to the two costs on the field bed, scaled to the loop of the pair; every other
  section moves within its repeat spread.

A G2 or G3 difference is read before anything else: if it traces to this change, the change goes behind a default-off
key or is dropped, and the card says which.

## 5. Results, 2026-10-29

### 5.1 The build

Branch `perf/cvf` (uncommitted at the time of writing), worktree `D:/GnssProcessorSupport/worktrees/cvf`. Control:
the Release executable of main `c37d4d6` built in that worktree before the change,
`D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_cvfctl.exe` (sha256[:16] `9472e3f139a816e4`).
Candidate: the same tree with the change, `gnss_processor_cvf.exe` (sha256[:16] `a694972c2479b8ea`).

The change as built:

- `NetworkNormalEquations::stationConditionalVarianceDiag` (new): one local solve and one `N_sr n_c` product per
  column; the zero-matrix product is taken only when the variance is zero or non-finite or `m_c` is non-finite.
  `NetworkEstimator::pureConditionalLocalVarianceDiag` forwards to it and keeps the old contract (a throw unless a
  solve has sized `shared_cov_`).
- `NetworkEstimator::zeroDatumFrame_` (new) is the one definition of the frame; `datumFrame_` (new) returns the frame
  stored by `solve()` when the datum pins the same columns in the same order, else builds one into a caller's scratch
  matrix. `conditionalLocalCovariance`, `conditionalLocalVarianceDiag`, `jointConditionalCovariance`,
  `blockConditionalCovariances`, `rowRedundancy`, `datumFrameSharedCov`, `ddPriorPosteriorCovariance` and
  `ddPriorInnovation` read it; seven copies of the copy-and-zero block are gone. The carry harvest's own copy of the
  same frame (`LocalCarryHarvest.cpp`, the shared border's `cov_df`) now calls `datumFrameSharedCov`.
- Declared diagnostic change: the per-solve build is timed by `CovTimer`, so the `cov-consumers` call count of the
  run-end `Network (D0)` lines rises by one per solve, and the harvest's frame copy moves from the harvest's time into
  `cov-consumers`. The gate drops the `Network (D0)` lines (wall-clock), so neither is visible to it.

### 5.2 G0 (unit) and G1 (suite): pass

`tests/network/test_network_conditional_variance_frame.cpp`, suite `ConditionalVarianceFrameTest`, 8 cases, compared
with `std::memcmp` on the doubles:

- The border-known variance against the zero-matrix form on six random stations (both signs of the regression rows,
  subsets out of order and repeated), with no shared columns, and with an infinite, a negative infinite and a NaN
  regression entry. The NaN case reaches the readback (the test asserts that at least one does, and that the result
  holds a NaN).
- A zero variance cannot arise: the kernel refuses a local block with an infinite weight (its positive-definiteness
  test), so the zero branch of the shortcut is defensive. The case passes on the refusal and records it.
- The stored frame against a frame built by hand from `sharedCovariance()` and fed to the kernel through `normals()`,
  for `datumFrameSharedCov`, `conditionalLocalVarianceDiag`, `conditionalLocalCovariance`,
  `pureConditionalLocalVarianceDiag`, `jointConditionalCovariance` and `blockConditionalCovariances`; a datum with
  another pinned column and a datum listing the solve's pin twice (both must build their own frame; the test first
  asserts the two hand frames differ); gauges set after the solve (the test asserts the gauge changes the variances,
  so a stale frame would fail it); two epochs through `reset()`.

The full Debug suite of the worktree (`gates/cvf/dbg_suite.log`): 3626 tests in 491 suites, 3593 passed, 33 skipped,
0 failed. The 33 skips are the data and fixture gates (SIF, station information, the GPT3 grid, the bundled fixtures;
the worktree's `data/` is empty).

Checks on the tree: `nom_epoch_lint.py` PASS, `nom_n4_verify.py --whole` PASS, `make_golden_config.py --verify`
PASS, `docs_index_check.py` OK.

### 5.3 G2, first build: 3 of 24, the consumer call count

The 23-arm gate of the first build (`gates/g7/gate_pair.sh cvfctl ... cvf ...`, summary `gates/cvf/summary_cvf.txt`,
detail `gates/g5/gate_cvf/`) read 3 of 24 IDENTICAL (`zero`, `gate` and the snapshot bytes). In the other 21 arms
`products.txt`, every trace CSV, the run manifest, the snapshot lines and the exit code are IDENTICAL; what differs is
the whole-log comparison of `stdout.txt` and `main_log.log`, and there only the call count of the per-epoch
`Network (D0) TOW` lines, whose milliseconds the gate masks and whose count it keeps
(`cov-consumers <MS> (640 calls)` against `(641 calls)` at TOW 259200 of `f2`). With the count masked, 0 lines
differ in either file of any arm (`gates/cvf/g2_recheck.txt`).

So section 5.1's statement that the gate drops the `Network (D0)` lines was wrong: it drops the run-end table, not
the per-epoch lines. The count moved because the first build counted the per-solve frame build as a consumer call
(`CovTimer`), and the harvest's frame copy became a counted `datumFrameSharedCov` call. The rule of section 4 for a
difference traced to the change is a default-off key or a drop; the difference here is the change's own timer
placement, so the placement was corrected instead, and the count is again a count of consumer calls:

- the per-solve build is not timed by `CovTimer` (its one copy per solve falls in the caller's section);
- the carry harvest's own frame copy is restored to the code of main (`LocalCarryHarvest.cpp` is unchanged by the
  card), so the de-duplication of section 5.1 does not reach it;
- `ddPriorPosteriorCovariance` and `ddPriorInnovation` time themselves, one call each, as they were counted when they
  went through `datumFrameSharedCov`.

The second build is gated in section 5.6.

### 5.4 G3: pass

- User side (`uom_gate.py` quick set, sides `cvfctl` and `cvf`): 16 of 16 IDENTICAL (`gates/cvf/uom_cmp_cvf.log`).
- Real time (`gates/cvf/rt.sh`): `sbf`, `sbf_bssd`, `sbf_te` PASS (`rt_cmp_dr.log`). The `cl` arm reads FAIL in the
  comparator on one line of `console.log` and `main_log.log`, the ARC LEDGER cost line (`build 47.0 ms ... audit 77.9
  ms` against `46.4 ms ... 76.6 ms`), a wall-clock line the brief named; every other file is identical
  (`rt_cl_fulldiff.txt`, `rt_cl_ledger.txt`).

### 5.5 G4 and G5: the gain

Four 2 h golden pairs, control and candidate at the same time, launch order swapped in the second pair of each bed
(`scripts/cost_attrib/abba.sh`, labels `cvfA1` to `cvfB4`, tables `gates/cvf/nc_table_pair{1..4}.txt`); the
products of every pair are byte-identical with `#` lines stripped (113 888 records on the field bed, 80 690 on the
sim bed). Other load 3.9 to 5.7 logical processors including the partner (`timing.txt`).

| pair | bed, first launched | loop ms per epoch, control to candidate | whole run | epochs 30 to 200 |
| --- | --- | --- | --- | --- |
| 1 | field, control | 1 612 to 1 442 | -10.5 % | 1 672 to 1 447, -13.5 % |
| 2 | field, candidate | 1 607 to 1 454 | -9.5 % | 1 682 to 1 467, -12.8 % |
| 3 | sim, control | 885 to 839 | -5.2 % | 887 to 833, -6.1 % |
| 4 | sim, candidate | 874 to 837 | -4.2 % | 892 to 832, -6.8 % |

**G4 passes:** the field loop is 9.5 and 10.5 % shorter over the whole run, against the bar of 5 %.

**The whole-run figure understates the gain.** The candidate finishes first (field wall 386 and 392 s against 426
s), so the control runs its last tens of epochs alone, while the candidate always has a partner. The per-epoch
`Network (D0)` lines show it (`gates/cvf/d0_window.py`, output `d0_window.txt`): over the last 25 field epochs
the control's reduce reads 120 to 126 ms against 205 when both run. Over epochs 30 to 200, when both runs are
certainly concurrent, the field loop is 12.8 and 13.5 % shorter (card NETCOST's estimate: 12 to 13 %), the sim loop
6.1 and 6.8 %.

**G5, first clause, passes:** `cov-consumers` falls from 349.0 to 125.7 and from 343.9 to 124.9 ms per epoch on the
field bed (223 and 219 ms, against the bar of half of 273 ms) and from 148.9 to 88.0 and 144.4 to 87.7 on the sim bed.
**G5, second clause, is not met as frozen, and its reading is open.** Over the whole run, untouched sections read
higher on the candidate in both launch orders (frame-diag +11 to +16, leveling +11 to +17, reduce +10 to +15,
solve-glue +8 to +10 ms), which is the end-of-run asymmetry above. Over epochs 30 to 200 the assembly wall and the
reduce move by -2.4 to +5.5 ms, but the unattributed `other` of the per-epoch line (which holds frame-diag, leveling
and solve-glue) still reads +12 to +15 ms on the field bed in both orders (+2 to +6 on the sim bed). With one run per
side per pair there is no repeat spread to set it against; a candidate-against-candidate pair would supply one.

### 5.6 The second build: G2 pass, real time pass

The second build (`gnss_processor_cvf2.exe`, sha256[:16] `1fd1e49a5559b83b`; the timer placement of section 5.3 and
nothing else) was gated against the control side of section 5.3 (`gates/cvf/rerun2.sh`, `rerun2.log`):

- **G2:** 24 of 24 IDENTICAL, the call counts of the per-epoch timing lines included (`gates/cvf/summary_cvf2.txt`,
  detail `gates/g5/gate_cvf2/`).
- **Real time:** `sbf`, `sbf_bssd`, `sbf_te` PASS (`rt_cmp2_dr.log`). `cl`: the comparator reads FAIL on
  `console.log` line 807 and `main_log.log` line 650; with seconds and milliseconds masked, `main_log.log` of
  `cvfctl_cl` and `cvf2_cl` differ in 0 lines (the differing lines are RINEX ingestion seconds, the epoch-loop wall,
  the round-trip self-check seconds and the ARC LEDGER cost line).
- **Unit tests:** the Debug build of the second tree passes the 113 cases of the estimator, normal-equation,
  carry-aware, sparse-normal, detached-fold and `ConditionalVarianceFrameTest` suites; the full suite of section 5.2
  ran on the first build.
- **Not rerun on the second build:** the user side (no user-side path reaches the network estimator) and the timing
  pairs. The second build does the same work as the first (the harvest's one frame copy per epoch is made inline
  again instead of through `datumFrameSharedCov`; only where the timers sit moved), so section 5.5 stands for it.

### 5.7 Reading

| gate | result |
| --- | --- |
| G0 unit | pass (8 cases, bit for bit; the zero-variance branch is defensive, the kernel refuses its input) |
| G1 suite | pass (3 593 passed, 33 data-gated skips, 0 failed) |
| G2 identity, producer | first build 3 of 24 (timing call count only); second build 24 of 24 |
| G3 identity, user side and real time | pass (16 of 16; real time wall-clock text only, both builds) |
| G4 gain | pass: field -9.5 and -10.5 % whole run, -12.8 and -13.5 % with both runs live; sim -4.2 to -6.8 % |
| G5 attribution | pass: `cov-consumers` -219 to -223 ms per field epoch; the rest inside the same-executable spread (section 5.8) |

Owed before closing the card: the merge (the author's call; the branch is uncommitted). Card NETCOST's
optimization 3 (the reduce subtract, the redundancy census product, the joint conditional covariance) is untouched and
stays owed there.

### 5.8 G5's second clause: the same-executable spread

Two further field golden 2 h pairs, each of one executable with both members at the same time (`abba.sh` entry
`f:ge+f:ge`; labels `cvfC`, the control, and `cvfD`, the second build; other load 4.9 and 5.4 logical processors;
script `gates/cvf/d0_spread.py`, output `d0_spread.txt`). Member 2 minus member 1 over epochs 30 to 200, ms per epoch:

| section | control pair | second-build pair |
| --- | --- | --- |
| loop total | +5.6 | -15.2 |
| assembly wall | +29.5 | -8.1 |
| reduce | +20.3 | -4.5 |
| cov-consumers | -4.5 | -0.4 |
| other | -36.7 | -3.1 |

Two members of one executable, run together, differ by up to 37 ms in `other` and 30 ms in the assembly wall, so the
+12 to +15 ms of `other` and the -2.4 to +5.5 ms of the assembly wall and the reduce between candidate and control
(section 5.5) are inside the repeat spread: **G5's second clause passes.** The loops of these pairs (control 1 594 and
1 600, second build 1 395 and 1 380 ms per epoch, at slightly different loads) agree with section 5.5. `products.txt`
with `#` lines stripped hashes to the same value (sha256[:16] `f7bc1445343068d1`) in all four runs and in `cvfA1`.
