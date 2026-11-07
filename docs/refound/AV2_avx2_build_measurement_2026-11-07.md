# AV2: what an AVX2 build of the network producer returns, and what it costs in reproducibility

**STATUS**

- **id:** AV2
- **topic:** perf
- **status:** open
- **summary:** Pre-registered 2026-11-07, before any run. Measures an `/arch:AVX2` Release build of the network producer against the SSE2 Release build of one commit, on the two goldens: the loop and the section timers, how much the three SSE2 geometry checks give back, and how far the outputs move. Nothing armed, nothing merged; the decision is the author's.
- **source:** [PES](PES_parallel_epoch_stages_2026-10-29.md) section 7.7 (the follow-up "the instruction set"); [NSP](NSP_census_neutral_spread_2026-10-29.md) (the census's spread under a neutral change of the wire, and `scripts/neutral_spread.py`); [BOF](BOF_built_off_follow_through_2026-10-28.md) section 6 (the baseline pair).
- **knobs / posture:** no key added and none armed. The build flag is a separate tree, `scripts/wt_build.ps1 -Arch AVX2` (`build/x64-Release-avx2`); `CMakeLists.txt` and the default build are unchanged. The arms read the three stage keys of card PES section 7 (`network.parallel_carry_harvest`, `parallel_iono_traces`, `parallel_coupling_prepare`), unarmed, only in the arms named `all`.
- **dates in the text:** 2026-11-07
- **hashes / run dirs:** branch `perf/avx2-measure` from `perf/pes-next` `8535d3b` (main adds docs only since the merge base `284ec29`), worktree `D:/GnssProcessorSupport/worktrees/avx2`; runs `D:/GnssProcessorSupport/runs_avx2/`; gate records `D:/GnssProcessorSupport/gates/avx2/`.
- **gates (frozen at pre-registration):** section 3, written before any run of either executable.

**Outcome.** Question: what does an AVX2 build of the network producer return in wall time on the two goldens, and what does it cost in reproducibility? Pre-registered 2026-11-07, no result yet. The card compares two Release builds of one commit in controlled sequences, reads how much the checks that decline under AVX2 give back, and grades the output movement on the held-out consumer by hour, the census, the between-satellite wire and the event sets, against the baseline pair and the neutral spread of card NSP. Standing: open, nothing armed.

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

None yet.
