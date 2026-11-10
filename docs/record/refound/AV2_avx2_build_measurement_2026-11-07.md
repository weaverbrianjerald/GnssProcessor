# AV2: what an AVX2 build of the network producer returns, and what it costs in reproducibility

**STATUS**

- **id:** AV2
- **topic:** perf
- **status:** closed
- **summary:** Pre-registered 2026-11-07 (`3ec110d`), closed the same day; the decision is the author's. An `/arch:AVX2` Release build of the network producer against the SSE2 build of one commit (`8535d3b`), on the two goldens, 2 h, controlled sequences under 12 to 16 logical processors of foreign load. As the build stands the loop is 8.5 to 8.8 % shorter on the field golden and not measurably shorter on the sim golden. The Schur reduce and the coupling inverse turn serial because their checks read the SSE2 tile (counters: 0 of 240 reduces split). With the two column-split checks taught (scratch, identical to the AVX2 build as it stands, every split taken) the loop is 17.8 % (field) and 15.7 % (sim) shorter, and 20.8 % and 13.8 % with the three keys of card PES section 7. The sparse normals give back nothing, because the dense AVX2 product is faster. Reproducibility: every same-executable pair identical, 12 threads identical to 20; the SSE2 wires identical to the baseline pair; the AVX2 outputs move by about card NSP's neutral spread (field census at most 0.010 mm, sim census 0.000 mm from 00:30:15, held-out consumers within 0.05 mm by hour, no decision changed). Unit suite under AVX2: 10 of 3 839 fail (six pin the SSE2 tile, three tolerances at 1e-8, one hold-clone variance off by 2.1 %, unexplained). Recommended: AVX2 with the separate step (teach the two checks, a counter for the sparse formation, the four numerical tests settled), then a new baseline pair; not the build as it stands. Nothing armed, nothing merged. The separate step was built the same day (section 6, branch `perf/avx2-teach`, not merged): both checks accept the AVX2 tile and take every split at output identical to the untaught AVX2 build, the SSE2 build is identical to before, the sparse formation reports formed sparse and formed dense at run end, and the four numerical tests are settled with no production defect (two test references sat at their own rounding limit; the hold clone agrees with an exact reference to 3e-15 or better). The full unit suite passes in both Release trees and in Debug. Switching the default build and the new baseline pair remain the author's decision. Section 7 (2026-11-07, the author's decision): AVX2 is the default build (`GNSS_ARCH`), and the baseline pair `_rf0hv18` is made on it; every user moves within 0.10 mm of the SSE2 build of the same keys.
- **source:** [PES](../../refound/PES_parallel_epoch_stages_2026-10-29.md) section 7.7 (the follow-up "the instruction set"); [NSP](NSP_census_neutral_spread_2026-10-29.md) (the census's spread under a neutral change of the wire, and `scripts/neutral_spread.py`); [BOF](BOF_built_off_follow_through_2026-10-28.md) section 6 (the baseline pair).
- **knobs / posture:** no key added and none armed. The build flag is a separate tree, `scripts/wt_build.ps1 -Arch AVX2` (`build/x64-Release-avx2`); `CMakeLists.txt` and the default build are unchanged. The arms read the three stage keys of card PES section 7 (`network.parallel_carry_harvest`, `parallel_iono_traces`, `parallel_coupling_prepare`), unarmed, only in the arms named `all`.
- **dates in the text:** 2026-11-07
- **hashes / run dirs:** branch `perf/avx2-measure` from `perf/pes-next` `8535d3b` (main adds docs only since the merge base `284ec29`), worktree `D:/GnssProcessorSupport/worktrees/avx2`; runs `D:/GnssProcessorSupport/runs_avx2/`; gate records `D:/GnssProcessorSupport/gates/avx2/`. Executables (section 5.1): SSE2 `50023c016c09030a`, AVX2 `db6370b62c359add`, scratch T `581e457ba45a9b6c`, scratch X `411513bfc40fb14c`. Section 6: branch `perf/avx2-teach` from `f53139a`, worktree `D:/GnssProcessorSupport/worktrees/avx2teach`, gate records `D:/GnssProcessorSupport/gates/avx2teach/`, arms `runs_avx2/_rap_avt_*`; executables SSE2 `78887f2b807582d1`, AVX2 `20609d73f38cd414`.
- **gates (frozen at pre-registration):** section 3, written before any run of either executable.

**Outcome.** Question: what does an AVX2 build of the network producer return in wall time on the two goldens, and what does it cost in reproducibility? As the build stands, the loop is 8.5 to 8.8 % shorter on the field golden and no shorter on the sim golden, because the Schur reduce and the coupling inverse decline to serial paths. With their two checks taught, the loop is 15.7 to 20.8 % shorter at identical output. The outputs move by about a neutral perturbation (census at most 0.010 mm, no decision changed), and each build reproduces itself exactly. Standing: closed; the separate step is built (section 6, unmerged). Recommended: AVX2 with that step and a new baseline pair; nothing armed.

## 1. Question

`CMakeLists.txt` sets no `/arch`, so every dense kernel runs on SSE2 doubles. The sparse station normals, the column-split Schur reduce and the split coupling inverse depend on the SSE2 tile geometry of Eigen's blocked kernel:

- `gemmOrderOf` (`src/core/network/solve/NormalEquations.cpp`) returns false on any build with `__AVX2__` or `EIGEN_VECTORIZE_FMA`, so `formStationNormalsSparse` declines and each station's normal blocks take the dense product;
- `schurColumnSplitExact` (same file) and `inverseSplitExact` (`src/core/network/carry/CouplingExact.cpp`) return false unless `gebp_traits<double, double>` has `mr == 4` and `nr == 4`, so the reduce runs its serial loop and the coupling inverse the whole solve.

Three questions follow:

1. How much shorter is the loop on an AVX2 build as it stands, with those three paths declined?
2. How much of the gain do the three fallbacks give back?
3. How far do the outputs move, and is an AVX2 build still reproducible against itself? An AVX2 build uses fused multiply-add and other kernel tiles, so it is expected not to be byte-identical to SSE2. Replacing the build would mean a new baseline pair, and every older reference and every byte comparison with an older arm would stop applying.

## 2. Method

**Builds.** One source tree, `8535d3b`, two Release trees in the D: worktree: `build/x64-Release` (the default configure line) and `build/x64-Release-avx2` (the same line plus `/arch:AVX2` through `CMAKE_CXX_FLAGS_INIT` and `CMAKE_C_FLAGS_INIT`). Each executable is copied once to a unique name beside its DLLs; the card records its sha256. Whether the flag reached the kernels is checked, not assumed (gate B0).

**Smoke.** A 10-epoch arm of each golden on each executable before any long run.

**Timing.** The same-commit pair, one run at a time, above-normal priority, `scripts/cost_attrib/run_timed.ps1` recording the machine load per run. Field and sim goldens, 2 h, the sim from 00:30:15 (`start_offset_hours` 0.504167), as in card PES section 7:

| sequence | posture | order on each golden |
| --- | --- | --- |
| `t1` | the golden as it is (the three armed stages of card PES) | SSE2, AVX2, AVX2, SSE2 |
| `t2` | `all`: the golden plus the three keys of card PES section 7 | SSE2, AVX2, AVX2, SSE2 |

**Output movement.** 6 h wires of each golden on each executable from the run start (the form of the baseline pair `runs/_rf0hv17_{field,sim}6`), then consumer-only censuses of those wires:

| instrument | tool | what is compared |
| --- | --- | --- |
| held-out consumer by hour (WTZZ00DEU, SZZ000SIM) | `scripts/refound_grade.py` | AVX2, SSE2, baseline pair |
| the wire against the reference by hour (on the sim the reference is truth) | `scripts/refound_grade.py` | AVX2, SSE2, baseline pair |
| between-satellite wire and the event sets, hours 2 to 6 | `scripts/neutral_spread.py wire` | AVX2 against SSE2 |
| field census, 16 sites | `scripts/clock_level/gcensus.py` | AVX2 wire with AVX2 consumer, against SSE2 wire with SSE2 consumer |
| sim census, 16 in-network stations | `scripts/clock_level/simcensus6h.py`, `SIMCENSUS_BASE` = the wire's arm | the same pairing |

## 3. Gates (frozen)

- **B0, the build is what it says.** The compile lines of `build/x64-Release-avx2` carry `/arch:AVX2` and those of `build/x64-Release` do not; a scratch translation unit compiled with each flag prints Eigen's `SimdInstructionSetsInUse()`, `gebp_traits<double, double>::mr` and `nr`; the AVX2 executable holds `vfmadd` instructions and the SSE2 one none.
- **T, cost.** Per arm: `Network[timing]` ms per epoch, the D0 and D0b section timers, CPU seconds and the machine load. A gain inside the spread of two arms of one executable in the same sequence is reported as no gain (the rule of card PES G5).
- **F, the fallbacks.** The run-end lines of card PES read on the AVX2 arms (`Schur reduce by columns: split, declined`; `coupling preparation: split, declined`), and the sections that hold the three paths (station normals in the assembly and `est.addStation`, `reduce`, `pre-asm`) against SSE2. A decline that does not show in the counters is a finding against the reading above, not a detail.
- **R1, reproducible against itself.** Within each sequence, the two arms of one executable by `scripts/prm_ab.py`'s `compare_dirs` (`products.txt` and every CSV under `logs/`, wall-clock columns dropped). Plus one AVX2 arm of each golden at `network.parallel_max_threads` 12 against the pinned 20. Bar: identical, both beds. A difference here is a blocker: an AVX2 peel count that follows the heap address would show here, and it is the failure this gate exists to catch.
- **R2, against SSE2.** The first differing epoch and record, the between-satellite move (p50, p95, max in mm, hours 2 to 6) for clock, code bias and phase bias, and the event rows present in one run only. Not a pass or fail: this is the measurement.
- **R3, the commit against the baseline.** The 6 h SSE2 wire of `8535d3b` against `runs/_rf0hv17_{field,sim}6` (`products.txt`, the armed arm differs from the baseline's only by the three stage keys, identical by card PES). Expected identical; a difference is attributed before any AVX2 reading is compared with the baseline.
- **M1, the consumer by hour.** Hours 2 to 6 are the settled reading; hour 1 is ignition.
- **M2, the census.** Per site the mean of the hourly p50 over hours 2 to 6, paired against SSE2. Two lines of reading, both frozen: more than 1 cm worse fails the "no site worse" clause; a change above the neutral spread of card NSP (0.19 mm, field, on an older executable) is reported by site as a response to the build and not as noise.
- **U, the unit suite.** `gnss_tests.exe` built in both Release trees: failures per build, named. A test that pins a kernel's tile order fails by design under AVX2 and is listed as such.
- **Recommendation rule, fixed now.** Adoption (a new baseline pair, at the author's decision) is recommended only when R1 passes on both beds, M1 shows no settled hour worse by more than 1 cm on either bed, M2 has no site past the 1 cm clause, R2 shows no new class of event, and the loop falls by at least 10 % on both beds beyond the spread of same-executable arms. A gain below 10 % as the build stands, with a larger figure available after teaching the checks, is reported as such with the separate step as its condition.

## 4. The separate step: teaching the checks

The first reading is the AVX2 build as it stands, with the three paths declined. How much of the gain the fallbacks give back is read from gate F. If a measured figure is needed to size it, a scratch executable (patch kept in the gate records, not on this branch, no key) separates the two kinds of check:

- `schurColumnSplitExact` and `inverseSplitExact` depend on the column tile (`nr`) and on panel cuts at tile multiples, so an AVX2 geometry may be accepted by the same argument. The bar for that scratch build is identity with the AVX2 build as it stands, on both goldens.
- `gemmOrderOf` describes the sum order of the SSE2 kernel. Under fused multiply-add the order differs, so a scratch arm that forces the sparse formation is a timing reading only: its output differs from the dense AVX2 kernel by rounding, which is graded with the instruments of gate R2.

Teaching the checks on the branch, with unit tests, is not part of this card.

## 5. Results

Appended 2026-11-07 after every run; sections 1 to 4 are as committed at pre-registration (`3ec110d`).

### 5.1 The builds (gate B0)

| executable | tree | sha256[:16] |
| --- | --- | --- |
| `gnss_processor_av2_sse2.exe` | `build/x64-Release` | `50023c016c09030a` |
| `gnss_processor_av2_avx2.exe` | `build/x64-Release-avx2` | `db6370b62c359add` |
| `gnss_processor_av2_teach.exe` (scratch, section 5.3) | `build/x64-Release-avx2`, patch `gates/avx2/teach_split.patch` | `581e457ba45a9b6c` |
| `gnss_processor_av2_teachx.exe` (scratch, section 5.3) | the same, patch `gates/avx2/teach_split_sparse.patch` | `411513bfc40fb14c` |

B0 passes. Every compile statement of `build/x64-Release-avx2/build.ninja` carries `/arch:AVX2`, and none of `build/x64-Release` does. The probe `gates/avx2/b0_geometry.cpp`, compiled with each flag:

| flag | Eigen instruction sets | packet (doubles) | `mr` | `nr` | fused multiply-add | alignment | depth panel `kc` of a 420 x 420 x 420 product |
| --- | --- | --- | --- | --- | --- | --- | --- |
| none (SSE2) | SSE, SSE2 | 2 | 4 | 4 | no | 16 | 420 |
| `/arch:AVX2` | AVX2, FMA | 4 | 12 | 4 | yes | 32 | 216 |
| `/arch:AVX512` (probe only, not built) | AVX512, FMA | 8 | 24 | 4 | yes | 64 | 420 |

The disassembly of the SSE2 executable holds no fused multiply-add instruction; the AVX2 one holds 3 062 (56 805 instruction lines on YMM registers against 6 355); the count was not attributed by function. Both 10-epoch smokes ran to exit 0 on both goldens; the AVX2 wire differs from the SSE2 one from the first epoch.

### 5.2 Cost as the build stands (gate T)

Controlled sequences, one run at a time, above-normal priority; the machine carried 12 to 16 logical processors of foreign load in every run (`other_procs_mean`, read per run in `runs_avx2/av2_runs.txt`). ms per epoch of the loop; spread is the gap between the two arms of one executable in the sequence.

| sequence | golden | SSE2 (arms) | AVX2 (arms) | AVX2 against SSE2 | spread SSE2, AVX2 | CPU s user, SSE2 to AVX2 |
| --- | --- | --- | --- | --- | --- | --- |
| `t1`, golden | field | 947.4 (940.7, 954.1) | 864.1 (859.1, 869.1) | -8.8 % | 1.4 %, 1.2 % | 790 to 625 |
| `t1`, golden | sim | 569.0 (573.6, 564.3) | 559.8 (572.1, 547.5) | -1.6 %, no gain | 1.6 %, 4.4 % | 531 to 455 |
| `t2`, `all` | field | 776.2 (776.6, 775.8) | 751.5 (748.8, 754.2) | -3.2 % | 0.1 %, 0.7 % | 878 to 702 |
| `t2`, `all` | sim | 492.9 (477.5, 508.3) | 487.0 (488.8, 485.2) | -1.2 %, no gain | 6.2 %, 0.7 % | 563 to 472 |

The work falls by 14 to 21 % of user CPU on every arm, but the wall falls by much less, because the reduce turns serial (section 5.3). On the sim golden the gain is inside the spread of same-executable arms, so it is reported as no gain.

### 5.3 The fallbacks, and the separate step (gate F, section 4)

The run-end counters of the AVX2 arms show the declines: `Schur reduce by columns: 0 reduce(s) split, 240 declined` (field; 241 on the sim) and, in the `all` arms, `coupling preparation: 0 inverse(s) split, 239 declined`. The station normals are still formed in parallel (13 919 forms), but densely: the sparse formation has no run-end counter (a gap against standing invariant 4), so its decline is read from the code path and from the scratch arm X below.

The scratch executables of section 4, in one sequence with the two builds (`t3`: SSE2, AVX2, T, X, X, T, AVX2, SSE2 on each golden; `t4`: SSE2, T, X, X, T, SSE2 with the three keys). T teaches `schurColumnSplitExact` and `inverseSplitExact` the AVX2 tile (`nr` 4, row panels cut at multiples of `mr`); X adds the sparse normals forced under AVX2 (a plain ascending sum, not the kernel's order).

| sequence | golden | SSE2 | AVX2 as it stands | T: split checks taught | X: T plus sparse normals | spread (largest pair) |
| --- | --- | --- | --- | --- | --- | --- |
| `t3`, golden | field | 901.8 | 824.9 (-8.5 %) | 741.5 (-17.8 %) | 755.0 (-16.3 %) | 4.1 % |
| `t3`, golden | sim | 519.3 | 499.2 (-3.9 %, no gain) | 437.6 (-15.7 %) | 445.1 (-14.3 %) | 8.7 % (AVX2), 5.5 % (SSE2) |
| `t4`, `all` | field | 716.0 | (`t2`: -3.2 %) | 566.9 (-20.8 %) | 593.0 (-17.2 %) | 3.4 % |
| `t4`, `all` | sim | 451.3 | (`t2`: -1.2 %) | 389.0 (-13.8 %) | 394.3 (-12.6 %) | 6.4 % |

T is IDENTICAL to the AVX2 build as it stands on both goldens, in both postures (`t3` and `t4` T arms against the `t3` and `t2` AVX2 arms, every file), with every reduce split (240 of 240 field, 241 of 241 sim) and every inverse split (239 of 239). The argument of card PES holds under the AVX2 tile: a column's arithmetic depends on its 4-column tile and the panels, not on the block that holds it.

Per section (ms per epoch, `t3`, golden as it is; `t4` for the coupling preparation):

| section | field SSE2 | field AVX2 | field T | field X | sim SSE2 | sim AVX2 | sim T | sim X |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Schur reduce (`D0 reduce`) | 36.8 | 124.7 | 22.1 | 22.7 | 15.1 | 62.3 | 8.5 | 8.4 |
| station normals (`D0 est.addStation`) | 51.3 | 38.0 | 37.9 | 51.8 | 25.4 | 17.0 | 16.7 | 27.1 |
| assembly (`D0 assembly-wall`) | 96.7 | 75.9 | 76.2 | 91.0 | 54.4 | 43.3 | 42.3 | 53.0 |
| covariance inverse (`D0 cov-inverse`) | 34.1 | 17.0 | 17.3 | 17.5 | 9.7 | 5.1 | 4.9 | 4.7 |
| covariance consumers | 119.5 | 83.8 | 84.3 | 86.1 | 86.5 | 60.3 | 60.4 | 60.1 |
| coupling preparation (`pre-asm`, `all`) | 35.6 (`t4`) | 50.1 (`t2`; SSE2 40.2) | 31.3 | 32.0 | 22.6 | 28.7 (`t2`; SSE2 26.5) | 20.7 | 20.2 |

What the three fallbacks give back:

- **The column-split reduce: most of the gain.** Serial under AVX2, the reduce costs 83 ms per field epoch (9.2 % of the SSE2 loop) and 62 ms per sim epoch (11.9 %). Taught, it is 1.7 (field) to 1.8 (sim) times faster than SSE2's split. On the golden as it is, the AVX2 gain is 77 ms (field) and 20 ms (sim) as the build stands and 160 and 82 ms with T.
- **The split coupling inverse.** With the three keys of card PES section 7, the serial inverse makes the preparation slower than SSE2 (50.1 against 40.2 ms, field); taught, it is 31.3 against 35.6. As the build stands the `all` posture gains 3.2 % (field); with T, 20.8 %.
- **The sparse station normals: nothing.** The dense AVX2 product is faster than the sparse formation: forcing the sparse path (X) costs 13.9 ms (field) and 10.4 ms (sim) per epoch in the station normals against T. `gemmOrderOf` declining under AVX2 costs nothing; teaching it the fused kernel's order is not worth building.

The X wire differs from the AVX2 wire at the same scale as AVX2 from SSE2 (2 h field: between-satellite clock move p95 0.007 mm, phase bias p95 0.04 mm, 120 decision rows in one run only, all `PRIORRES`), and the X pairs are identical among themselves.

### 5.4 Reproducibility against itself and against the baseline (gates R1, R3)

- **R1 passes.** Every same-executable pair is IDENTICAL on both goldens: SSE2 and AVX2 in `t1` and `t2`, SSE2, AVX2, T and X in `t3` and `t4`, and the SSE2 arms of `t1` against those of `t3` (36 files each, `gates/avx2/av2_cmp.py`). The AVX2 build at `network.parallel_max_threads` 12 against the pinned 20 is identical in every file but `logs/network_run_summary.csv`, which records the thread count. Seven AVX2-family processes of each golden as it is (two AVX2 in `t1`, two AVX2 and two T in `t3`, one at 12 threads) and four in the `all` posture gave one output per golden and posture: no output followed the heap address.
- **R3 passes.** The 6 h SSE2 wires of `8535d3b` are IDENTICAL to the baseline pair `runs/_rf0hv17_{field,sim}6` in `products.txt` and every compared CSV (`gates/avx2/r3_field.log`, `r3_sim.log`). Every AVX2 movement below is therefore also a movement against the baseline pair.

### 5.5 How far the outputs move (gates R2, M1, M2)

6 h wires `_rap_av2_w6_{sse,avx}_f_ge` (field), `_rap_av2_w6_{sse,avx}_S_ge` (sim from the run start, the baseline's form) and `_rap_av2_w6o_{sse,avx}_s_ge` (sim from 00:30:15, outside the fixture's start artifact).

**R2, the wire and the decisions** (`scripts/neutral_spread.py wire` and `gates/avx2/av2_wire.py`; between-satellite move over hours 2 to 6, mm):

| wire | first difference | clock p50 / p95 / max | code bias p95 / max | phase bias p50 / p95 / max | decision rows in one run only |
| --- | --- | --- | --- | --- | --- |
| field, AVX2 against SSE2 | 00:00:00 | 0.002 / 0.006 / 0.020 | 0.002 / 0.032 | 0.005 / 0.208 / 2.36 | 493, all `PRIORRES` (measure-only) |
| sim from the start | 00:00:30 | 0.000 / 0.001 / 0.002 | 0.000 / 0.000 | 0.003 / 0.014 / 0.30 | 46, all `PRIORRES` |
| sim from 00:30:15 | 00:31:00 | 0.000 / 0.000 / 0.000 | 0.000 / 0.000 | 0.002 / 0.012 / 0.12 | 36, all `PRIORRES` |
| for scale: card NSP's neutral re-rounded roster, field DOY 129, six arms | 00:00:00 | 0.009 to 0.015 / 0.031 to 0.050 / 0.1 | | p95 0.16 to 0.27 | 401 to 492 |

No record is present in one wire only, and no event class appears or disappears; every differing decision row is the measure-only prior-residual census. The largest phase-bias moves are steps captured at one epoch and then held: G12 L1W and L2W move by 1.42 and 2.35 mm at 02:22:00 and keep that offset to the end, E02 L5X by 1.88 mm at 04:16:30. The held value takes the rounding difference of the float state at the capture epoch. The AVX2 build moves the wire by about as much as card NSP's neutral arms: less in the clock, the same in the phase bias.

**M1, the held-out consumer by hour** (hourly p50 of the 3D error, mm; `gates/avx2/av2_consumer.py`; the trace resolves 0.1 mm):

| consumer | build | h1 | h2 | h3 | h4 | h5 | h6 | largest per-epoch change |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| WTZZ00DEU | SSE2 (= baseline) | 58.8 | 12.3 | 23.05 | 12.25 | 16.5 | 12.0 | |
| WTZZ00DEU | AVX2 | 58.8 | 12.3 | 23.05 | 12.25 | 16.5 | 11.95 | 0.2 (h1), 0.1 after |
| SZZ000SIM from the start | SSE2 (= baseline) | 136.85 | 72.3 | 30.75 | 23.95 | 25.7 | 18.2 | |
| SZZ000SIM from the start | AVX2 | 136.85 | 72.3 | 30.8 | 23.95 | 25.7 | 18.2 | 0.6 (h1), 0.1 after |
| SZZ000SIM from 00:30:15 | SSE2 | 99.45 | 32.6 | 39.8 | 16.0 | 18.0 | 15.15 | |
| SZZ000SIM from 00:30:15 | AVX2 | identical | | | | | | 0.1 (719 of 720 rows identical) |

The two re-founding instruments (`scripts/refound_grade.py`) agree at their printed precision except the third decimal of a metre in a few wire-against-reference rows.

**M2, the census** (each build's wire with the same build's consumer; per site the mean hourly p50 over hours 2 to 6; `gates/avx2/census_fine.py`):

| census | consumers | largest change, hours 2 to 6 | paired median | sites past the 1 cm clause | largest per-epoch change |
| --- | --- | --- | --- | --- | --- |
| field, DOY 129 | WTZZ00DEU and 16 sites | 0.010 mm (WTZZ00DEU -0.010; QUIN, ARHT, BAKE +0.010) | 0.000 mm | 0 | 0.5 mm (CHTI) |
| sim from 00:30:15 | 16 stations | 0.000 mm | 0.000 mm | 0 | 0.1 mm |
| sim from the start | 16 stations | 0.44 mm (S25, better) | 0.000 mm | 0 | 143 mm (S25, hour 1) |

The field census sits 19 times below card NSP's neutral spread (0.19 mm). The one exception is S25 in the replay from the run start, inside the sim fixture's start artifact: the consumer starts 11.3 m off and its hour-1 p50 is 1.87 m on both builds. There AVX2 moves hour 1 by -30.7 mm and hours 2 to 6 by -0.44 mm (hour 2 -1.9 mm, hours 3 to 6 within 0.4 mm). The crossed replays (`gates/avx2/s25_cross.py`) separate the two halves: the AVX2 wire with the SSE2 consumer, and the SSE2 wire with the AVX2 consumer, each move S25 by at most 0.2 mm in any epoch. Only the two together make the 143 mm, which marks an ill-conditioned convergence from metres rather than a response of a settled solution. From 00:30:15, the grading start the project prescribes for the sim bed, S25 does not move (716 of 720 rows identical).

### 5.6 The unit suite (gate U)

Release `gnss_tests.exe` of the same commit in each tree (`gates/avx2/tests_sse2.log`, `tests_avx2.log`, the messages in `tests_avx2_failures.txt`):

| build | tests | passed | skipped (fixture-gated in a worktree) | failed |
| --- | --- | --- | --- | --- |
| SSE2 | 3 839 in 526 suites | 3 806 | 33 | 0 |
| AVX2 | 3 839 in 526 suites | 3 796 | 33 | 10 |

The ten AVX2 failures, by kind:

- **Six pin the SSE2 tile, by design.** `NeqSparseStationNormalsTest` (three cases: the sparse formation is refused on every shape, 0 accepted of 2 000), `NeqParallelStagesTest.ColumnSplitReduceMatchesTheSerialReduceBitForBit`, `LdltInverseColumnsTest.MatchesTheWholeSolveBitForBit` and `CouplingExactFactorSplitTest.HoldsTheSameInverseWithTheSplit` require the split or the sparse path and fail on the decline, not on a wrong number.
- **Three meet a tolerance at the conditioning limit.** `CouplingExactUpdateTest` (`ExactRecursionEqualsTheBatch`, `DecoupledAndDepartedColumnsEqualTheBatch`, `BroadcastGradeWalkLocksThePerRowLevelWithRotatedAnchors`): the exact recursion against the batch solve differs by 1.09e-8 to 1.31e-8 against a bar of 1e-8 that the SSE2 rounding meets.
- **One is not explained here.** `PbHoldSmoothedTest.AReinitOfTheClonesOwnClockKeepsItUnbiased` compares the hold clone's increment variance with an augmented-filter reference at 1e-6 relative after a re-initialization variance of 1e7; under AVX2 the two read 1.3340e-4 and 1.3059e-4 (2.1 %). Which side moved, the clone or the test's reference (a cancellation against the 1e7 re-initialization is the candidate), is not determined. The clone feeds the phase-bias hold that both goldens arm, so this is settled before any adoption.

### 5.7 Not measured

- A quiet machine: every timing carries 12 to 16 logical processors of foreign load, so the ratios are the reading and the absolute times sit above quiet figures.
- The 305-station posture, the real-time engine, the user-side PPP path and the Python module under AVX2.
- Another processor model. Eigen sizes its panels from the cache sizes it reads at run time, and the column-split checks read those panels, under either build.
- An AVX-512 build (the probe of section 5.1 only: a 24-row tile).

### 5.8 Recommendation

The frozen rule of section 3, clause by clause:

| clause | reading | met |
| --- | --- | --- |
| R1, reproducible against itself, both beds | every same-executable pair identical; 12 threads identical to 20 | yes |
| M1, no settled hour worse by more than 1 cm | the largest hourly change is 0.05 mm (half the trace's 0.1 mm resolution) | yes |
| M2, no site past the 1 cm clause | 0 of 17 (field) and 0 of 16 (sim); largest 0.010 mm field, 0.000 mm sim from 00:30:15 | yes |
| R2, no new class of event | no class appears or disappears; every differing row is the measure-only prior-residual census | yes |
| loop at least 10 % shorter on both beds, beyond the spread | as the build stands: field -8.5 to -8.8 % (golden), -3.2 % (`all`); sim -1.6 to -3.9 %, inside the spread | **no** |
| the same with the separate step (scratch T) | field -17.8 % (golden), -20.8 % (`all`); sim -15.7 %, -13.8 %; spreads at most 6.4 % | yes |

**The AVX2 build as it stands is not recommended:** it misses the 10 % bar on both beds, because the reduce and the coupling inverse turn serial. **AVX2 is recommended with the separate step as its condition.** The cost in reproducibility is one of bookkeeping, not of results. The outputs move by about as much as card NSP's neutral arms (field census 0.010 mm, sim census 0.000 mm from 00:30:15, held-out consumers within one 0.1 mm trace quantum, no decision changed). An AVX2 build reproduces itself exactly. What it costs is the baseline pair, re-established on the AVX2 build, and every byte comparison with an earlier arm, which stops applying across the change. The steps, in order, each the author's to approve:

1. **Teach the two column-split checks** on a branch of their own: `schurColumnSplitExact` and `inverseSplitExact` accept `nr` 4 with row panels cut at multiples of `mr` (the scratch patch `gates/avx2/teach_split.patch` is three lines). Extend the unit tests that require the split to the AVX2 tile, with a test tree built under each flag. T is identical to the AVX2 build as it stands on both goldens, so this step moves no output.
2. **Leave `gemmOrderOf` declining under AVX2**, and give the sparse formation a run-end counter (formed sparse, formed dense) so the decline shows as telemetry. Under AVX2 the dense product is faster than the sparse formation (section 5.3), so the three `NeqSparseStationNormalsTest` cases become SSE2-only.
3. **Settle the four numerical tests** of section 5.6: the three recursion-against-batch tolerances (1e-8 against a measured 1.3e-8) and, before any adoption, which side of `PbHoldSmoothedTest.AReinitOfTheClonesOwnClockKeepsItUnbiased` moves by 2.1 %.
4. **Switch the default build** (`/arch:AVX2` in `CMakeLists.txt`), which also requires AVX2 on every machine that runs the producer. Then re-establish the baseline pair on it.

## 6. The separate step, built (2026-11-07)

Appended 2026-11-07. Steps 1 to 3 of section 5.8, on branch `perf/avx2-teach` from `f53139a` (the code of `8535d3b`), worktree `D:/GnssProcessorSupport/worktrees/avx2teach`, gate records `D:/GnssProcessorSupport/gates/avx2teach/`, arms `runs_avx2/_rap_avt_*`. Not merged, nothing armed; `CMakeLists.txt` and the default build are unchanged. Step 4 (the default build and a new baseline pair) remains the author's decision.

### 6.1 What changed

- **The two column-split checks.** `schurColumnSplitExact` (`src/core/network/solve/NormalEquations.cpp`) and `inverseSplitExact` (`src/core/network/carry/CouplingExact.cpp`) accept any tile with `nr` 4, with row panels cut at multiples of `mr` and column panels at multiples of `nr`, instead of requiring `mr` 4. On SSE2 (`mr` 4) the checks are the same conditions as before. The Doxygen of each states the two accepted tiles (SSE2: `mr` 4; AVX2 with fused multiply-add: `mr` 12) and the argument: an element's arithmetic depends on its row tile, its column tile and the depth panels; panels start at multiples of their size, so with `mc` a multiple of `mr` and `nc` a multiple of `nr` every tile but the global remainder is whole in the whole product and in each block. For the triangular solve the strips of max(`mr`, `nr`) rows, the depth panels and the row panels are properties of the rows; only the column tile depends on the block, and the blocks start at multiples of 4.
- **The sparse formation's build property and counter.** `NetworkNormalEquations::sparseFormationOnThisBuild()` is the one owner of the fact that the formation reproduces the SSE2 kernel only (`gemmOrderOf` reads it). `StationFormationCounts` (formed sparse, formed dense) is counted by `addStationObservations` and by `commitStation`, once per station that reaches the formation, and nothing while `network.sparse_station_normals` is off. The run end prints, beside the PES lines in `src/core/network/runend/RunTimingRunEnd.cpp`, `Network station normals: N formed sparse, M formed dense after the sparse formation declined the shape`, with a note on a build with fused multiply-add. Telemetry only: no key, no ledger row, no CSV column.
- **Tests.** `tests/network/test_neq_parallel_stages.cpp`: the column-split reduce over shared sizes 64 to 75 (every residue modulo 12, so modulo 4), 418 and 421, with 5 and 49 locals and 3 and 20 threads, each required to take the split; the formed-and-committed kernel counts the same formations as the serial add. `tests/network/test_harvest_coupling_parallel.cpp`: the inverse over orders 120 to 123, 128 to 131 and 256 to 259 (every residue modulo 12) required to split, beside the small orders and 421. `tests/network/test_neq_sparse_station_normals.cpp`: the three cases that require an accepted shape skip on a build where `sparseFormationOnThisBuild()` is false; a new case, `TheFormationCountsShowEveryDecline`, runs under both flags. The two numerical tests of section 6.4.

### 6.2 The panels each build cuts

Eigen's blocking for the shapes of the tests and of the goldens, from a probe compiled with each flag (`gates/avx2teach/probe/panels.cpp`, output `panels_output.txt`; this machine's cache sizes):

| product | SSE2 (`mr` 4, `nr` 4) | AVX2 (`mr` 12, `nr` 4) |
| --- | --- | --- |
| Schur reduce, 418 or 421 shared columns, 49 locals (the goldens' shape) | one depth panel; columns cut at `nc` 212 | one depth panel; rows cut at `mc` 216 (18 tiles) |
| Schur reduce, 64 to 75 shared, 5 locals | rows cut at 36 or 40, or none | rows cut at 36 or 48, or none |
| Schur reduce, 64 to 75 shared, 49 locals | columns cut at 16, 12 or 8 | columns cut at 16 to 4 |
| coupling inverse (triangular solve), order 421 | `kc` 112, `mc` 421 | `kc` 56, `mc` 421 |
| coupling inverse, orders 49 to 51 | declined (a block gets another `mc`) | the same |

At the goldens' shape the AVX2 reduce rests on the row-panel clause that the teaching added, the SSE2 one on the column-panel clause; both clauses are exercised by the unit tests on either build.

### 6.3 Gates

| gate | reading | met |
| --- | --- | --- |
| SSE2 identical to before, both 2 h goldens | final executable `78887f2b807582d1`: `_rap_avt_sseb_{f,s}_ge` against card AV2's `_rap_av2_t3a_sse_{f,s}_ge` and `_rap_av2_t1a_sse_{f,s}_ge`; an earlier build of this branch (differing in one log string) also in the `all` posture, `_rap_avt_sse_{f,s}_all` against `_rap_av2_t2a_sse_{f,s}_all`; IDENTICAL, 35 files each (`prm_ab.py` `compare_dirs`, `gates/avx2teach/cmp_sse.log`, `cmp_sseb.log`) | yes |
| AVX2 splits every reduce and inverse | `_rap_avt_avx_{f,s}_{ge,all}`: Schur reduce 240 of 240 split (field), 241 of 241 (sim); coupling inverse 239 of 239 (field), 240 of 240 (sim) | yes |
| AVX2 identical to the untaught AVX2 build, both goldens | `_rap_avt_avx_{f,s}_ge` against `_rap_av2_t3a_avx_{f,s}_ge`, `_rap_avt_avx_{f,s}_all` against `_rap_av2_t2a_avx_{f,s}_all`, and the golden arms against scratch T (`_rap_av2_t3a_tch_{f,s}_ge`): IDENTICAL, 35 files each (`cmp_avx.log`) | yes |
| full unit suite, both Release trees and Debug | SSE2 Release: 3 840 tests, 3 807 passed, 33 skipped (fixture-gated in a worktree), 0 failed; AVX2 Release: 3 804 passed, 36 skipped (the same 33 and the three SSE2-only cases), 0 failed; Debug (SSE2): 3 807 passed, 33 skipped, 0 failed, run in four gtest shards (`tests_sse2.log`, `tests_avx2.log`, `tests_debug_shard_0.log` to `_3.log`) | yes |
| `nom_epoch_lint.py`, `docs_index_check.py` | PASS | yes |

The new counter on the arms: SSE2 13 919 formed sparse and 0 dense (field), 14 460 and 0 (sim); AVX2 0 sparse and 13 919 dense, 0 and 14 460, the decline by design.

An incidental timing reading, not a gate and not a controlled sequence (single arms, with builds and unit suites sharing the machine during most of them; the controlled figures are section 5.3): ms per epoch of the loop, SSE2 against AVX2, field golden 899.9 (two arms) against 752.4 (-16.4 %), sim golden 543.4 against 474.0 (-12.8 %); `all` posture field 757.5 against 618.9 (-18.3 %), sim 478.9 against 392.2 (-18.1 %).

### 6.4 The four numerical tests

**`PbHoldSmoothedTest.AReinitOfTheClonesOwnClockKeepsItUnbiased`: the test's reference moved, not the hold clone.** The reference filters (5 states, and 6 with the augmented c(t0)) took the re-initialization epoch as a Joseph update with a stand-in prior variance of 1e7. Recomputed beside them in information form, the exact q to infinity limit of the exact rule's Reinit route (the re-initialized column carries no prior information, so no large number enters), at the reinit epoch (`gates/avx2teach/probe/diag_sse2.txt`, `diag_avx2.txt`):

| quantity at epoch 9, reinit of G01's clock | SSE2 | AVX2 |
| --- | --- | --- |
| Joseph 5-state against Joseph 6-state, max abs over the shared block | 0 (bit-identical rounding) | 2.8e-6 |
| Joseph against the exact form, max abs | 3.4e-5 (both filters) | 3.5e-5 and 3.2e-5 |
| exact 5-state against exact 6-state | 8.7e-17 | 4.3e-16 |
| P(0,0): Joseph, exact | 4.8752e-3, 4.8409e-3 | 4.8757e-3, 4.8409e-3 |
| increment variance, clone side and reference side, Joseph | 1.32846e-4, 1.32846e-4 | 1.33402e-4, 1.30593e-4 (the reported 2.1 %) |
| the same, exact reference | 9.85576e-5, 9.85576e-5 | 9.85576e-5, 9.85576e-5 |
| the clone's own variance `st.var` | 4.8585499593e-5 | 4.8585499593e-5 |
| worst relative mismatch over the run (value absolute, variance, increment), exact reference | 1.2e-15, 7.7e-17, 6.0e-18 | 3.1e-15, 4.3e-16, 9.7e-18 |

The Joseph reference missed the posterior of a re-initialized clock by 0.7 % and its increment variance by 35 %. On SSE2 the two reference filters rounded identically (their shared block bit for bit), so the same error stood on both sides of the comparison and the case passed; under AVX2 the two filters round differently and the error shows. The production clone is unchanged between the builds and agrees with the exact reference to 3e-15 or better. Fix (test only): the reference takes the reinit epoch in information form, and both reinit cases are now held to the 1e-8 of the plain case instead of 1e-6. No production defect.

**The three `CouplingExactUpdateTest` recursion-against-batch cases: the batch reference sits at its conditioning limit.** Measured on both builds with a temporary diagnostic (removed): the batch information matrix has a condition number of 2.0e7 to 2.1e7 (a code-determined level under millimetre phase); a relative perturbation of 4 eps on each entry moves the batch's border values by up to 1.4e-8 m (SSE2) and 1.2e-8 m (AVX2); the worst recursion-against-batch gap is 4.8e-9 to 1.0e-8 m on SSE2 (already at 99.8 % of the old 1e-8 bar) and 1.0e-8 to 1.3e-8 m on AVX2, each time on an L2 phase bias whose batch sigma is 0.19 m. Key by key between the builds (the default scenario), the production recursion's border values move by at most 2.7e-9 m and the test's batch values by up to 1.7e-8 m: the reference carries the rounding, not the recursion. Fix (test only): border values are held to 1e-7 m, the bar the same function already used for the ambiguities for the same reason (5e-7 of those columns' sigma); every variance and covariance stays at 1e-8 relative and meets it on both builds. No production defect.

### 6.5 Standing

The conditions section 5.8 set for an AVX2 build are met on this branch: the checks are taught with tests that run under either flag, the sparse formation reports its declines, and the four numerical tests are settled. What remains is step 4 of section 5.8, the author's: switching the default build (`/arch:AVX2`, which requires AVX2 on every machine that runs the producer) and re-establishing the baseline pair on it.

## 7. The default build and the baseline pair (2026-11-07)

Appended 2026-11-07. Step 4 of section 5.8, at the author's decision ("AVX2 is good to enable, I don't care about slightly moving the results if we gain performance improvement"). Branch `int/avx2-default` from main `e5513fd`: merge of `perf/avx2-teach` (`dbd0bfa`), the build switch and the D6 regime (`2b90874`), `/bigobj` for one test (`a9b810c`). Gate records and graders `D:/GnssProcessorSupport/gates/avx2def/`, runs `D:/GnssProcessorSupport/runs_avx2def/`.

**The switch.** `CMakeLists.txt` gains the cache variable `GNSS_ARCH` (default `AVX2`, accepted `SSE2`), which adds `/arch:AVX2` to every translation unit; `scripts/vs2026_build.ps1` builds AVX2 with no change. `scripts/wt_build.ps1 -Arch SSE2` configures a separate tree `build/<Config>-sse2` with the earlier instruction set (the option `-Arch AVX2` of section 6 is gone: AVX2 is the default tree). Under AVX2 the Debug build of `tests/test_rhs_attrib_local.cpp` crosses the object file section limit (C1128), so it takes `/bigobj` like its two siblings.

**Gates.** Release: 4 123 cases in 547 suites, 4 087 passed, 36 skipped (fixture-gated, worktree), 0 failed; `gnss_simulator_selftest` 99 of 99. Golden, posture, docs, epoch-lint and N4 verifiers pass. Exe `gnss_processor_avx2def.exe`, sha256[:16] 6c0162d829454319.

**Baseline pair `_rf0hv18`** (6 h, the three goldens of this branch, all 720 epochs emitted, none withheld). Against the SSE2 build of the same keys (the 2026-11-07 health-check arms `runs_int1107/_rap_i7_{f,s,b}_arm`), the products differ and the users do not move:

| bed | in-loop user, hourly median 3D (m), AVX2 | largest change against SSE2 |
| --- | --- | --- |
| field DOY 129, WTZZ00DEU | 0.0586 0.0119 0.0237 0.0125 0.0166 0.0122 | 0.00 mm every hour |
| sim, SZZ000SIM | 0.1967 0.1087 0.0277 0.0195 0.0290 0.0183 | +0.05 mm (hour 2) |
| BeiDou lineage, WTZZ00DEU | 0.0344 0.0142 0.0206 0.0198 0.0203 0.0160 | 0.00 mm every hour |

Field census (key `c18f`, 16 sites plus WTZZ00DEU, hours 2 to 6): median change 0.000 mm, largest site mean -0.01 mm (BADG), largest single hour -0.05 mm. Sim census (key `s18`, 16 stations): median 0.000 mm, largest site mean +0.03 mm (S37), largest single hour +0.10 mm. All inside card NSP's neutral spread (0.19 mm). Wall time on a machine shared with two builds: field 1 534 s, sim 1 368 s, BeiDou 2 016 s; the controlled timing is section 6's.

**Standing.** AVX2 is the default build; `_rf0hv18` is the baseline, and comparisons are made within one build. Every machine that runs the producer needs AVX2.
