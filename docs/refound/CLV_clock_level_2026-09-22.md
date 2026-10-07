# CLV: the satellite clock level, frozen at ignition by the carry

**STATUS**

- **id:** CLV
- **topic:** network-vehicle
- **status:** superseded
- **summary:** SUPERSEDED by card CTU's consistent coupling recursion: the out-of-solve level re-base (`network.clock_level_carry`, built `e47b267`, NOT ARMED) fixed the sim level (datum-free 1.15 to 0.07 m), not the field.
- **source:** [GAL](GAL_gps_galileo_dual_2026-09-22.md) section 6.1 (the finding and the first arms); [RF-1_float_ambiguities_design](RF-1_float_ambiguities_design.md) sections 1.3 and 1.4 (the carried priors count the level as phase-precise; "the level freezes at K_eff epochs"); [RF-IGN](RF-IGN_ignition_born_levels_2026-09-15.md) (the same lever on the field golden); [RF-2](RF-2_held_basis_2026-09-15.md) (the clock-level ramp read R-CL, never done); [Q3 design](../cards/clock/Q3-ANCHOR-DESIGN.md) and [Q3 results](../cards/clock/Q3-ANCHOR-RESULTS.md) (the anchor binds the walk, not the level)
- **knobs / posture:** new `network.clock_level_carry` (bool, default false, byte-identical off) with `network.clock_level_sigma_floor_m` (default 0.01) and `network.clock_level_horizon_s` (default 7200); the return plan (section 7): `network.clock_level_pin_pair_read`, `network.clock_level_free_cb_follow` (off, if, full), `network.clock_level_datum_pause`, `network.clock_level_confirm_epochs`, `network.clock_level_pivot_ref`, every default the built card; on the RF-0hv working bases at main `68915a0` (GPS only), then card GAL's Galileo set on top
- **dates in the text:** 2026-09-22 (pre-registered by the author's decision to fix the root card GAL exposed before continuing; built), 2026-09-25
- **hashes / run dirs:** batch 1 (section 8): exe `gnss_processor_clvrp1.exe` sha256 prefix `a2060462f5ba3ec2` (worktree build on `335f640`), arms `runs/_rap_clvrp1_*`, census replays `runs/_rap_msc_<site>_rp1<arm>_d`; baseline pair `runs/_rf0hv10_{field,sim}6`; the ladder `runs/_rap_cil_e2` to `_e23` (2 h sim); arms `runs/_rap_clv_*`; exe `gnss_processor_clv.exe` sha256 `289404cbbbbad82d2f6eb4c744c3250737eae8574376f05143bd1380a9416b77`; build `e47b267`; gate arms `runs/_rap_clv_g0f_sim2`, `_g0f_field2`, `_g1f_sim2`, `_g2_sim6`, `_g3_field6`, `_gal_sim6`, `_galx_sim6`, `_gal_field6` (card GAL's bases `runs/_rap_gal_g2b_sim6`, `_g3x2_sim6`, `_g5s_field6`); census keys `clv` against `rf3gl2nb` and `clvgal` against `gals` (replays `runs/_rap_msc_<site>_{clv,clvgal}_d`); the in-solve variants `runs/_rap_clv_v3*_sim2`, `_v4*_sim2`, `_f4_field6` and `_f6_field6` to `_f9_field6`

**Outcome.** Question: can the ignition-frozen level converge at the code's rate with the phase side held. The re-base passes every G1 and G2 sim truth bar, misses three other G2 bars, and fails the field G3 (WTZZ00DEU hour 1 0.791 against 0.560 m), the Galileo field and the split emulation: the free same-band code-bias families absorbed the level. Batch 1 was NOT ARMED: every variant lost hour 1 at a cold consumer. The author chose a consistent recursion ([CLV2](CLV2_clock_level_batch2_2026-09-25.md) section 6.8): [CTU](CTU_coupling_time_update_2026-09-28.md) retired this corrector, and [EXL](EXL_exact_update_lah_promotion_2026-10-01.md) armed the exact update.

## 1. What is measured (GPS-only sim baseline `runs/_rf0hv10_sim6`, where the a priori clock is the truth)

The wire's per-satellite clock offset has an rms across satellites of 1.29 m at epoch 1, 1.50 at
epoch 4 and 1.45 at epoch 480, constant from epoch 4 on. Part of it is the network's datum: the
ionosphere-free combination of C1C and C2W is pinned at zero per satellite while the product's
clock datum is P1/P2, so the offsets correlate -0.67 with the product's own IF(C1C, C2W) bias
per satellite. Removing that share (the datum-free error, `scratchpad/clklevel.py --datumfree`):
0.69 m rms at epoch 1, 1.30 at epoch 4, 1.18 at epoch 8, 1.12 from epoch 60 to 720. The
single-epoch code solve is 0.7 m noisy, the first harvests of the carry double the error, and
from then on it never moves. The per-satellite median ionosphere-free code residual over hours
2 to 6 runs from -2.64 to +1.89 m (sd 1.01 m across 32 satellites), hour after hour; the C1C and
C2W residual medians are +0.29 and -0.17 times the clock offset, which is the pattern of a
wrong clock level of which the free slant ionosphere absorbs 71 percent per line of sight. The
phase is consistent throughout: the ambiguities absorb a per-satellite constant, so the float
consumer, the within-arc wire instrument and every card since RF-0 were blind to it. The field
baseline shows the same constancy (0.85 m rms from the COD clock per satellite, code residual
medians -2.29 to +0.53 m). The clock, the code biases and the ionosphere levels on the wire are
therefore wrong per satellite at the metre level for any code-based user, and card GAL's split
emulation, its consumer inter-system bias and E12's 749 gross C1C rows on the field are the
places it surfaced.

## 2. The ladder (2 h sim arms on the base, `runs/_rap_cil_*`; clock offset rms across satellites with the per-epoch median removed at epochs 4 / 16 / 60 / 240; the datum-free error at epoch 240; the sd across satellites of the per-satellite median IF code residual in hour 2; the consumer 3D p50 by hour and its count below 0.1 m of 240; the clock-alone within-arc rms over the 2 h)

| arm | set | level 4 / 16 / 60 / 240 | datum-free 240 | resid sd | consumer | walk |
| --- | --- | --- | --- | --- | --- | --- |
| base (`_rap_gal_g0_sim2c`) | | 1.47 / 1.48 / 1.45 / 1.45 | 1.15 | 0.97 | 0.601 0.104 0.028 (53) | 0.121 |
| e2 | `clock_temporal_anchor: false` | 1.47 / 1.65 / 1.56 / 1.52 | 1.40 | 1.13 | 1.025 0.201 0.117 (0) | 0.112 |
| e3 | `clock_anchor_var_floor_m2: 1.0` | 1.47 / 1.63 / 1.53 / 1.48 | 1.39 | 1.09 | 1.186 0.299 0.170 (0) | 0.145 |
| e4 | `clock_anchor_q: 1e-3` | as e2 | 1.40 | 1.13 | as e2 | 0.112 |
| e5 | `phase_leveling_young_inflate_var_m2: 1.0` | 1.47 / 1.45 / 1.36 / 1.34 | | 0.86 | 0.673 0.135 0.024 (24) | 0.118 |
| e6 | `phase_leveling_var_floor_m2: 0.01` | 1.47 / 1.48 / 1.45 / 1.44 | | 0.95 | 0.635 0.169 0.083 (13) | 0.121 |
| e7 | `phase_leveling_q_amb: 1e-5` | 1.47 / 1.48 / 1.44 / 1.41 | | 0.91 | 0.608 0.228 0.143 (11) | 0.120 |
| e8, e11, e11b, e12, e13, e14 | `phase_leveling_local_block`, `amb_float_birth_var_m2` 1 and 4, `amb_float_q_m2s 1e-4`, `carry_iono_in_block`, the three in-block knobs | byte-identical to the base | | | | |
| e9 | `filter.process_noise.ambiguity_q: 1e-4` | as base | | 0.97 | 10.504 11.214 6.863 (0) | |
| **e10** | **`phase_leveling_warmup_epochs: 76`** | **1.47 / 1.14 / 0.80 / 0.81** | **0.16** | **0.12** | 2.107 0.610 0.393 (0) | 0.176 |
| e16 | `phase_datum_held_basis: false` | as base | | 0.97 | 0.610 0.131 0.058 (35) | 0.121 |
| e17 | `filter.process_noise.sat_phase_bias_q: 1e-4` | 1.10 / 0.97 / 0.97 / 0.97 | 0.64 | 0.49 | 0.602 0.123 0.068 (55) | 0.078 |
| e18 | `clock_anchor_gauge_free: true` | 1.47 / 1.45 / 1.43 / 1.39 | | 1.03 | 0.607 0.108 0.028 (67) | 0.120 |
| e19, e20 | `shared_carry_var_floor_pb_m2` 0.01 and 0.0025 | as base | | 0.97 | 0.601 0.100 0.028 (61); 0.601 0.095 0.026 (66) | 0.121 |
| e21 | `emit_pb_hold: false` | as base | | 0.97 | 1.951 0.827 0.501 (0) | 0.121 |
| e22 | e17 + e2 | 1.10 / 1.24 / 1.15 / 0.88 | 0.56 | 0.46 | 1.071 0.964 0.483 (0) | 0.155 |
| e23 | e17 + e3 | 1.10 / 1.32 / 1.19 / 0.85 | 0.47 | 0.38 | 1.064 0.590 0.492 (0) | 0.206 |

The reading. The level converges at the code's rate only while the ambiguities solve free (e10:
1.30 to 0.16 m over 76 epochs, then frozen where the carry began). Every carried prior locks it
once harvested, and no single one of them is the lock: loosening the clock alone does nothing
(e2, e3, e4), loosening the phase bias alone moves it halfway (e17), loosening both moves it
slowly at the price of the consumer and the wire's walk (e22, e23), and the local ambiguity
carries hold the rest. The lock is what RF-1 section 1.3 named: the carried priors, taken
together, state the level at phase precision although only the code rows carry level
information. Delaying the harvest is not a fix (2.1 m in hour 1, 0.61 in hour 2, the walk 0.12
to 0.18 m): the phase-consistent part of the carry must start at epoch 6 as it does today while
the LEVEL stays at its code-only uncertainty.

## 3. The design

For satellite s the level direction is u_s: +1 on its clock column, -1 on each of its phase-bias
columns. A shift along u_s leaves every phase row invariant (clock and phase bias enter a phase
row with the same coefficient) and moves every code row of s by the shift, so the level is a
code-only quantity and its uncertainty is the code-only variance V_L^s(t): one over the
accumulated ionosphere-free code information of s (the sum over its full-weight code rows of
1 / sigma_IF,row^2, sigma_IF,row from the row's own code sigma and the ionosphere-free
coefficients), decayed with the horizon `clock_level_horizon_s` and floored at
`clock_level_sigma_floor_m`^2. `network.clock_level_carry` inflates the dense border carry of s
by V_L^s(t) u_s u_s^T before the border prior is folded (after the Q3 anchor's own fold, so
the anchor keeps binding the walk, the direction u_s excepted, and its guard reads the inflated
prior). Nothing else changes: the local ambiguity coupling rows are invariant to u_s by
construction (their regression on the border carries equal coefficients on the satellite's clock
and phase-bias columns; a unit test asserts it), the phase side of the wire (clock plus phase
bias) does not move, the code side (clock plus code bias) converges as 1/sqrt(N) like a PPP
level, and the per-satellite ionosphere levels of every station converge with it.

## 4. The gates (2 h sim for structure, 6 h for the truth grades; then the field; then card GAL's arms on the fixed posture)

| gate | arm | bar |
| --- | --- | --- |
| G0 | `clv_g0_sim2`, `clv_g0_field2` (knob absent) | the three graded files identical to `runs/_rf0hv10_{field,sim}6` over the window |
| G1 | `clv_g1_sim2` (knob on, 2 h) | datum-free clock error rms across satellites at epochs 16 / 60 / 240 at or below 0.60 / 0.30 / 0.15 m (base 1.17 / 1.14 / 1.15); the IF code residual median spread in hour 2 at or below 0.10 m (base 0.97); the consumer not worse than 0.601 0.104 0.028 by more than 0.010 m in any hour; `declared_dim == n_below`; the run-end line naming V_L per satellite (min, median, max) and the epochs the guard tripped |
| G2 | `clv_g2_sim6` (6 h) | datum-free error at epochs 240 / 720 at or below 0.15 / 0.08 m; residual spread in hours 2 to 6 at or below 0.08 m; consumer not worse than 0.598 0.103 0.096 0.056 0.055 0.049 by 0.010 m in any hour and the count not below 448; the phase-side wire (D_phase within-arc, `wire_vs_code_census.py`) not worse than the base; the clock-alone within-arc rms reported (it may carry the level's motion); the mean Up per hour against truth reported; gross rows not above 595 / 1 |
| G3 | `clv_g3_field6` (6 h) | the per-satellite IF code residual median spread at or below 0.10 m (base 0.50); WTZZ00DEU not worse than 0.560 0.059 0.028 0.030 0.039 0.048 by 0.010 m, the count not below 611; the phase-side wire not worse than the base; the census key `clv` against `rf3gl2nb` not worse (median count, hours 2 to 6, hour 1) |
| G4 | card GAL's `g2b_sim6`, `g3x2_sim6` and `g5_field6` sets on the knob (`clv_gal_*`) | the consumer inter-system bias within 0.10 m of 8.000 m less the datum from hour 2 (GAL bar 0.05 failed at 0.5 to 0.95); the split emulation's C1X minus C1C recovery rms at or below 0.05 m from hour 3 (0.87 before; the C5X assertion of 0.048 m sd stays); E12's C1C gross rows on the field below 100 (749); WTZZ00DEU and the census not worse than GAL's |
| G5 | cost | `loop_ms` on against off |

## 5. Owed alongside (from card GAL, to be taken in this or the next pass)

The replay reader's clock-discontinuity channel gated on `serialize_sbasis_contract`; the
consumer's inter-system bias reset that can shrink the variance; the group-delay latch on the
first navigation record; the sim fixture without E05, E11 and E12; the Galileo yaw law.

## 6. Result (2026-09-22)

Build worker result. Every number is read from the file named, under the run directory named;
the graders are read-only readers of those files (`scripts/wire_vs_code_census.py`,
`scripts/gal_grade.py`, and the session graders `scratchpad/clklevel.py`, `wtzz.py`, `gross.py`,
`gross_sys.py`, `gal_wire.py`, `hash3.py`, `g3_recovery.py` and `clv_*.py`). Build `e47b267`
(the file and line references below are at that commit); exe
`build/x64-Release/gnss_processor_clv.exe`,
sha256 `289404cbbbbad82d2f6eb4c744c3250737eae8574376f05143bd1380a9416b77`;
every gate run started after the exe was copied. Nothing is armed: the bases, the goldens and
the ledger are untouched.

**6.1 What was built against what was pre-registered.**

- R1, the level direction. In this codebase the phase bias enters a phase row at +1
  (`MeasurementModel.cpp:1549`), the satellite clock enters code and phase rows at -1 (`:1591`)
  and the satellite code bias enters code rows only, at +1 (`:1527`). The phase-blind direction
  is therefore +1 on the clock and +1 on every phase bias of the satellite; section 3 wrote -1
  on the phase biases, the sign inverted for these conventions. Test
  `LevelDirectionIsBlindToPhaseAndSeenByCode`.
- R2, the code-only information: built as the information of a two-way fit, not a raw row sum.
  Per kept epoch, one ionosphere-free pair per (station, satellite) from the heaviest
  full-weight (w at or above 1e-2) data code row on each of the constellation's two pin bands
  (`NetworkClockLevel.cpp:363-410`) is fitted with one level effect per satellite and one offset
  per (station, constellation), which takes what the receiver clocks absorbed; the effects sum
  to zero per constellation (`clockLevelTwoWay`, `:106`). Each effect and its information feed a
  scalar filter per satellite (`clockLevelFilterUpdate`, `:193-216`): I_L decays with
  `clock_level_horizon_s` (`:70`) and is capped at 1 / floor^2, V_L = max(1 / I_L, floor^2),
  the ignition variance while unarmed (`:77`). The filter arms when the Q3 anchor holds the
  clock and restarts on a 4-sigma break. The state is `NetworkCarryState::clk_level`
  (`NetworkRunContext.hpp:775`).
- R3, the inflation: built, measured, REPLACED (the in-solve code is not in the build).
  V_L u_s u_s^T added to the border carry moved the sim level (`runs/_rap_clv_v3g_sim2`:
  datum-free 0.42 / 0.10 / 0.07 m at epochs 16 / 60 / 240, consumer 0.621 0.066 0.072 m against
  0.601 0.104 0.028) but on the field it let station events into the satellite clocks. On
  `runs/_rap_clv_f4_field6` (`stdout.txt`) the anchor refloated six satellites at once at TOW
  259410 (G09, G13, G14, G20, G22, G30), the guard tripped 54 times (23 NIS) against 32 (1 NIS)
  on the base, and WTZZ00DEU read 0.450 0.052 0.025 0.639 0.910 0.244 m by hour with 238 epochs
  below 0.1 m (`logs/closed_loop_trace.csv`) against 0.560 0.059 0.028 0.030 0.039 0.048 and
  611. A level variance added every epoch makes the level a random walk inside the solve, and
  the station clocks and the ionosphere trade against it. The replacement leaves the solve
  untouched: after each kept epoch (after the stash, `ApplicationNetwork.cpp:2965`) the filter's
  update d_s, de-meaned per constellation over the updated satellites
  (`NetworkClockLevel.cpp:456-470`), moves the carry along u_s: the carried clock and every
  carried phase bias of s, every coupling term that references them (the `s_prev` of the local
  ambiguity, ZWD, ionosphere, hold, receiver clock, receiver phase-bias and receiver code-bias
  carries), the clock-jump, phase-bias level and pin-bridge baselines, and the emit-side
  phase-bias hold through a per-signal transfer (`clockLevelShiftCarry`, `:218-307`).
- R4, the coupling rows. On this posture they are not invariant to u_s: the Card-1d kappa time
  update shrinks a clock term far more than a phase-bias term (6.4, item 1), so a carried
  ambiguity row states the level. The re-base moves every coupling `s_prev` with the level, so
  each row keeps its residual (tests `CouplingRowsKeepTheirResidualUnderTheReBase`,
  `TimeUpdateTurnsAnAmbiguityRowIntoALevelStatement`).
- R5, the wire. The held phase-bias emission follows the shift (`NetworkPbHold.cpp:111-170`):
  the clock record and every phase-bias record of s move by the same step, the phase side
  (minus the clock plus the phase bias) does not move, and the code side converges. The step is
  declared on the wire as the C-record token `lvl=<m>`, written only when nonzero so an unarmed
  wire is byte-identical (writer `NetworkProductIO.cpp:797`, legend `:679`, reader `:217`),
  stamped per satellite at the emit (`networkClockLevelDeclare`, `NetworkClockLevel.cpp:309`,
  called at `NetworkEmitStore.cpp:317`) and removed from the emitter's phase-bias value-jump
  test (`NetworkProductEmitter.cpp:572-577`) and from the CLK-R frame vote and step tests
  (`NetworkClockEmitRebase.cpp:46`, `:93`, `:113`): one code path for the producer and a
  replay. Unit test `LevelStepRoundTripsTheWireAndKeepsTheDeclarations`: an emitter fed the
  level steps directly and an emitter fed the written-then-read records give identical clock,
  phase-bias and code-bias discontinuity counters (L1C 0, L2W 1, 4 jump tests excluded by the
  declaration); the same records with the declaration withheld declare L1C 2 and L2W 3; `lvl=`
  is on exactly the 2 C records with a nonzero step. On the runs (`stdout.txt`), jump tests over
  the bar only because of the declared step: 660 (G1), 1363 (G2), 601 (G3), 8120 (G4 field). The
  WTZZ00DEU replays of the G3 and G4 field wires (`runs/_rap_msc_wtzz_clv_d`,
  `runs/_rap_msc_wtzz_clvgal_d`) equal the in-process consumer in 720 of 720 rows of
  `logs/closed_loop_trace.csv`.
- R6, telemetry: the run-end V_L line per constellation, the re-base line and the wire line
  (`NetworkClockLevel.cpp:502-554`); `logs/network_clock_level.csv` (week, tow, sat,
  v_level_m2, i_level, n_rows_epoch, e_level_m, d_level_m, event) under `network.trace_events`
  (`NetworkRunTraces.cpp:486-492`); the manifest fields (`:1714`).
- R7, tests: `tests/test_clock_level_carry.cpp`, suite `ClockLevelCarryTest`, 9 cases, all
  passing; the rank-1 inflation case does not apply to the re-base, and the knob-off byte
  identity is certified by G0. `gnss_tests`: 2609 ran in 327 suites, 2608 passed, 1 known skip
  (`FixtureSmokeTest.Sp3HeaderTimeConversionMatches`).
- R8, knobs: `network.clock_level_carry` (false), `network.clock_level_sigma_floor_m` (0.01),
  `network.clock_level_horizon_s` (7200); getters with Doxygen (`GnssConfig.hpp:5219-5232`),
  loader (`GnssConfigLoadNetwork.cpp:8986-8993`) with a post-arming check (`:11044`: resolved
  OFF under `network.clock_model`, a warning without the Q3 anchor), fingerprinted
  (`NetworkRunContext.cpp:1009`); `config_posture_check.py --verify-loader` 0 coverage gaps;
  `make_golden_config.py --verify` PASS (61 armed, 7 documented exceptions, both goldens).

**6.2 The gates.**

G0 (2 h, knob absent; `hash3.py` over `products.txt` with comments stripped,
`logs/closed_loop_trace.csv` and `logs/network_epoch_status.csv`, hash / lines hashed): PASS,
identical over the window. `runs/_rap_clv_g0f_sim2` and `runs/_rf0hv10_sim6`:
bc6b8905a878b049 / 45821, 8946f461860cfbbb / 241, 9ee045c023d603f8 / 241;
`runs/_rap_clv_g0f_field2` and `runs/_rf0hv10_field6`: d593befbe8be160a / 59194,
c3490c2bd2a745f9 / 241, f74d7d5b7a584fef / 241.

G1, `runs/_rap_clv_g1f_sim2` against `runs/_rap_clv_g0f_sim2` (2 h):

| bar | CLV | base | verdict |
| --- | --- | --- | --- |
| datum-free clock error rms across satellites at epochs 16 / 60 / 240 at or below 0.60 / 0.30 / 0.15 m (`products.txt`, `clklevel.py --datumfree`) | 0.43 / 0.13 / 0.07 | 1.17 / 1.14 / 1.15 | PASS |
| IF code-residual median spread in hour 2 at or below 0.10 m (`logs/network_pf_rows.csv`) | 0.07 | 0.97 | PASS |
| consumer 3D p50 by hour not worse than the base by more than 0.010 m in any hour (`logs/closed_loop_trace.csv`) | 0.365 0.088 0.031 (93 epochs below 0.1 m) | 0.601 0.104 0.028 (53) | PASS |
| `declared_dim == n_below` (`logs/network_estimability.csv`) | 6 of 6 EPOCH rows (the trace budget is 6 epochs) | | PASS |
| Q3 guard trips (`stdout.txt`) | 0 | 1 (1 NIS) | read |

The V_L line (`stdout.txt`): "G: V_L over 31 armed satellite(s) at their last update min
7.790e-04 / median 8.301e-04 / max 6.699e-03 m^2 (sigma 0.0279 / 0.0288 / 0.0818 m); 0 at the
floor (0.010 m); 1 satellite(s) not armed at run end". The re-base: 7232 updates (31 arms, 9
breaks), 364 not armed, mean shift 0.0194 m, largest 2.8677 m, 2 777 109 carried references
moved. V_L per satellite (`logs/network_clock_level.csv`, seconds after the start): G05 arms at
150 s at 1.263e-01 m2 (effect -0.865, shift +0.882 m), then 6.33e-02 at 180 s, 3.17e-02 at 240,
8.20e-03 at 600, 2.29e-03 at 1920 and 7.89e-04 at 7200; G16 arms at 150 s at 1.177e-01 (effect
-2.161, shift +2.178 m), 7.74e-03 at 600 s, 7.79e-04 at 7200; G03 arms at 150 s at 1.323e-01,
breaks at 240, 360, 390 and 480 s (effects +2.116, -1.879, +2.335, +2.416 m), 2.99e-02 at
600 s, 8.49e-04 at 7200.

G2, `runs/_rap_clv_g2_sim6` against `runs/_rf0hv10_sim6` (6 h):

| bar | CLV | base | verdict |
| --- | --- | --- | --- |
| datum-free error at epochs 240 / 720 at or below 0.15 / 0.08 m (`products.txt`) | 0.07 / 0.05 | 1.15 / 1.12 | PASS |
| IF code-residual spread, hours 2 to 6, at or below 0.08 m (`logs/network_pf_rows.csv`) | 0.07 0.07 0.08 0.07 0.06 | 0.97 1.00 1.04 1.02 1.03 | PASS |
| consumer by hour, base + 0.010 m (`logs/closed_loop_trace.csv`) | 0.365 0.088 0.066 0.051 0.061 0.051 | 0.601 0.104 0.096 0.056 0.055 0.049 | PASS (largest excess +0.006, hour 5) |
| epochs below 0.1 m, at least 448 | 519 | 448 | PASS |
| phase-side wire D_phase, within-arc rms, hours 2 to 6, per-epoch median common mode (`products.txt`, `wire_vs_code_census.py`) | 0.328 0.260 0.283 0.295 0.892 | 0.608 0.424 0.464 0.462 0.798 | FAIL (hour 6 +0.094) |
| the same, diffmedian common mode | 0.011 0.011 0.011 0.011 0.012 | 0.010 0.009 0.009 0.009 0.010 | FAIL (+0.001 to +0.002) |
| clock-alone within-arc rms, hours 2 to 6, all / tracked (`gal_wire.py`) | 0.0448 / 0.0339 | 0.0040 / 0.0030 | reported (carries the level's motion) |
| mean Up against truth by hour (`logs/closed_loop_trace.csv`) | -0.004 +0.047 +0.058 +0.035 +0.051 +0.034 | -0.560 +0.080 +0.087 +0.047 +0.046 +0.036 | reported |
| full-weight gross code rows above 3 m / 10 m, at most 595 / 1 (`logs/network_pf_rows.csv`, `gross.py`) | 521 / 3 | 595 / 1 | FAIL on 10 m |
| Q3 trips (`stdout.txt`) | 31 (0 NIS, 31 datum) | 32 (1 NIS, 31 datum) | read |

The V_L line: "G: ... min 1.030e-03 / median 1.101e-03 / max 8.460e-03 m^2"; the re-base: 21618
updates (62 arms, 18 breaks), 937 not armed, mean shift 0.0125 m, largest 2.8677 m; the wire:
43716 held phase-bias emissions moved.

G3, `runs/_rap_clv_g3_field6` (with `network.serialize_sbasis_contract`) against
`runs/_rf0hv10_field6` (6 h):

| bar | CLV | base | verdict |
| --- | --- | --- | --- |
| IF code-residual spread in the last hour at or below 0.10 m (`logs/network_pf_rows.csv`) | 0.24 (by hour 0.15 0.17 0.23 0.15 0.14 0.24) | 0.50 (0.19 0.27 0.32 0.28 0.29 0.50) | FAIL |
| WTZZ00DEU by hour, base + 0.010 m (`logs/closed_loop_trace.csv`) | 0.791 0.051 0.026 0.028 0.037 0.047 | 0.560 0.059 0.028 0.030 0.039 0.048 | FAIL in hour 1 (+0.231); hours 2 to 6 PASS |
| WTZZ00DEU epochs below 0.1 m, at least 611 | 580 | 611 | FAIL |
| WTZZ00DEU mean Up by hour | +1.180 +0.051 +0.005 -0.024 -0.032 -0.044 | +0.794 +0.014 -0.004 -0.024 -0.034 -0.046 | reported |
| phase-side wire, hours 2 to 6, per-epoch median (`products.txt`, `wire_vs_code_census.py`) | 1.104 0.908 0.993 1.538 1.388 | 1.115 0.930 1.009 1.579 1.440 | PASS |
| the same, diffmedian | 0.011 0.008 0.008 0.020 0.015 (10 steps above 0.25 m in hour 5) | 0.009 0.006 0.007 0.010 0.014 | FAIL |
| clock-alone within-arc rms, diffmedian, hours 2 to 6 | 0.070 0.051 0.056 0.095 0.154 | 0.009 0.007 0.006 0.010 0.015 | reported |
| gross code rows above 3 m / 10 m (`gross.py`) | 216 / 32 | 213 / 32 | read |
| Q3 trips (`stdout.txt`); anchor decouples by hour (`logs/network_events.csv`) | 57 (26 NIS, 31 datum); 0 1 0 0 56 0 | 32 (1 NIS, 31 datum); 0 1 0 0 31 0 | read |
| replay identity at WTZZ00DEU (`runs/_rap_msc_wtzz_clv_d`) | 720 of 720 rows | | PASS |
| census `clv` against `rf3gl2nb`: median count at least 348, hours 2 to 6 and hour 1 not worse at more sites than better | below | | FAIL |

The re-base: 21514 updates (88 arms, 0 breaks), 164 epochs with too few pairs, 26 skipped at an
anchor decouple, 851 not armed, mean shift 0.0040 m, largest 1.2593 m; the wire: 60840 held
emissions moved. The census, verbatim (`runs/_msc/scripts/msc_replay_set.py clv
_rap_clv_g3_field6 _rf0hv10_field6`, `msc_pair.py rf3gl2nb:d clv:d`):

```
wire rf3gl2nb:d n=16 | median p50 by hour: 0.670 0.169 0.104 0.077 0.092 0.070 | median count 348.0
wire clv:d    n=16 | median p50 by hour: 1.389 0.280 0.109 0.136 0.107 0.068 | median count 235.0
   vs rf3gl2nb:d: count improve 8 worsen 8 (>+36: 5, <-36: 6) median -6.0 | mean p50 hours 2-6: better(>1 cm) 6 worse 7 median +0.002 m | hour 1: better(>5 cm) 4 worse 10 median +0.479 m
   per site count (A -> B): CHTI 499>547 SCRZ 398>362 ANMG 204>256 QUIN 583>256 HARB 358>213 POHN 0>4 DAV1 339>246 BADG 185>202 STPM 581>512 ASCG 180>224 CEDU 357>406 KOKB 125>109 UTQI 13>29 ARHT 562>0 WUH2 69>16 BAKE 504>619
```

G4, card GAL's sets on the knob (6 h each):

| arm (base) | bar | CLV | base | verdict |
| --- | --- | --- | --- | --- |
| `runs/_rap_clv_gal_sim6` (`_rap_gal_g2b_sim6`) | consumer inter-system bias minus the truth-and-datum prediction within 0.10 m from hour 2, median by hour, hours 2 to 6 (`logs/closed_loop_isb.csv`, `products.txt`, the fixture truth; `gal_grade.py --sections isbtruth`) | +0.053 +0.029 -0.005 -0.012 +0.003 (p90 of the absolute difference 0.107 0.054 0.038 0.032 0.020) | +0.951 +0.948 +0.811 +0.670 +0.491 | PASS |
| same | datum-free clock error rms per constellation at epochs 16 / 60 / 240 / 480 / 720 (`products.txt`, `truth_bias.csv`; `clv_df_sys.py`) | GPS 0.462 0.199 0.167 0.163 0.048; Galileo 0.326 0.136 0.082 0.272 0.019 (without the pivots G01 and E02: GPS 0.421 0.109 0.030 0.030 0.049; Galileo 0.327 0.123 0.036 0.272 0.019) | GPS 1.188 1.136 1.130 1.101 1.082; Galileo 1.001 0.971 0.951 0.943 0.886 | reported |
| same | consumer by hour (`logs/closed_loop_trace.csv`); mean Up | 0.656 0.057 0.050 0.032 0.037 0.025 (588); -0.853 -0.013 +0.044 -0.016 -0.022 -0.005 | 0.519 0.061 0.037 0.032 0.035 0.026 (587); -0.434 -0.040 +0.015 -0.028 -0.021 -0.006 | read |
| same | gross code rows above 3 m / 10 m, GPS; Galileo (`gal_grade.py`) | 560 / 2; 334 / 0 | 645 / 0; 399 / 0 | read |
| `runs/_rap_clv_galx_sim6` (`_rap_gal_g3x2_sim6`; the same RINEX ops, the rewritten files byte-identical) | C1X minus C1C recovery rms by hour, at or below 0.05 m from hour 3 (`products.txt`, `rapid.json`; `g3_recovery.py`) | 0.648 0.385 0.314 0.272 0.235 0.167 | 0.881 0.868 0.869 0.876 0.866 0.596 | FAIL |
| same | C5X minus C5Q recovery rms by hour | 0.030 0.029 0.029 0.029 0.029 0.030 | 0.029 0.028 0.028 0.027 0.027 0.028 | read |
| same | inter-system bias minus the prediction, hours 2 to 6 | +0.096 +0.039 -0.017 -0.032 -0.025 | +0.353 +0.320 +0.294 +0.200 +0.026 | read |
| same | datum-free rms at epoch 240, GPS / Galileo | 0.069 / 0.436 | 1.053 / 1.994 | reported |
| same | consumer by hour | 0.221 0.059 0.044 0.034 0.041 0.028 (623) | 0.299 0.063 0.045 0.035 0.042 0.029 (599) | read |
| `runs/_rap_clv_gal_field6` (`_rap_gal_g5s_field6`, both with the S contract) | E12 C1C full-weight code rows above 3 m below 100 (`logs/network_pf_rows.csv`; `clv_gross_sat.py`) | 701 (E12 C5Q 739) | 715 (E12 C5Q 80) | FAIL |
| same | gross rows above 3 m / 10 m, Galileo; GPS (`gross_sys.py`) | 1635 / 14; 167 / 32 | 958 / 14; 168 / 32 | read |
| same | WTZZ00DEU by hour not worse than GAL's (`logs/closed_loop_trace.csv`) | 0.260 0.042 0.025 0.029 0.029 0.021 | 0.197 0.063 0.032 0.006 0.010 0.009 | FAIL in hours 1, 4, 5, 6 (+0.063, +0.023, +0.019, +0.012) |
| same | WTZZ00DEU epochs below 0.1 m; mean Up by hour | 648; -0.065 +0.030 +0.020 -0.024 -0.027 -0.018 | 623; -0.403 +0.013 +0.021 -0.003 -0.006 -0.006 | count PASS; a standing -0.02 m Up in hours 4 to 6 |
| same | Q3 trips (`stdout.txt`); anchor decouples by hour (`logs/network_events.csv`) | 91 (36 NIS); 4 1 0 26 60 0 | 60 (5 NIS); 4 1 0 24 31 0 | read |
| same | replay identity at WTZZ00DEU (`runs/_rap_msc_wtzz_clvgal_d`) | 720 of 720 rows | | PASS |
| same | census `clvgal` not worse than GAL's `gals` (median count 434.5) | 362.5 | 434.5 | FAIL |

The Galileo field re-base (`stdout.txt`): 38086 updates (146 arms, 77 breaks, every break on
E12), 162 epochs with too few pairs, 36 skipped at an anchor decouple, 1769 not armed, mean
shift 0.0148 m, largest 3.3775 m; the wire: 125436 held emissions moved. The census, verbatim
(`msc_replay_set.py clvgal _rap_clv_gal_field6 _rap_clv_gal_field6`, `msc_pair.py gals:d
clvgal:d`):

```
wire gals:d   n=16 | median p50 by hour: 0.519 0.104 0.054 0.058 0.050 0.045 | median count 434.5
wire clvgal:d n=16 | median p50 by hour: 0.667 0.127 0.068 0.099 0.082 0.051 | median count 362.5
   vs gals:d: count improve 3 worsen 13 (>+36: 1, <-36: 6) median -27.5 | mean p50 hours 2-6: better(>1 cm) 3 worse 5 median +0.003 m | hour 1: better(>5 cm) 6 worse 10 median +0.103 m
   per site count (A -> B): CHTI 568>540 SCRZ 348>364 ANMG 394>361 QUIN 594>579 HARB 493>357 POHN 323>210 DAV1 441>420 BADG 286>325 STPM 677>619 ASCG 428>391 CEDU 551>550 KOKB 295>247 UTQI 108>133 ARHT 487>23 WUH2 103>76 BAKE 649>645
```

G5, cost (`logs/network_run_manifest.json`, `loop_ms`): sim 2 h 58448.7 against 58059.3 ms
(`runs/_rap_clv_g1f_sim2` against `runs/_rap_clv_g0f_sim2`, run side by side: +389 ms, +0.67
percent); field 6 h 231576.6 against 228805.8 ms (`runs/_rap_clv_g3_field6` against
`runs/_rf0hv10_field6`, not load-matched: +2771 ms, +1.2 percent). The Galileo arms ran under a
different load from card GAL's bases and are not compared.

**6.3 The mechanism read: the free same-band code-bias families.**

Under the RF-0d pin rules (card GAL section 3.2) a satellite's ionosphere-free and
geometry-free pins sit on one pair (GPS C1C and C2W; Galileo C1C and C5Q, with the cross-band
extra C5X tied to C1C through its geometry-free pin), and a same-band extra code (GPS C1W,
Galileo C1X) keeps a free, carried bias column. At ignition that column absorbed the frozen
level exactly as the clock did (G05's C1W bias sigma 0.328 m at the first epoch and 0.011 m at
TOW 270000, `runs/_rap_clv_g3_field6/logs/network_sat_bias_states.csv`). The re-base moves the
clock and the phase biases, not the free code biases, and the two-way fit takes band 1 from the
heavier of the two band-1 rows.

- The free column does not follow. Arm against base per GPS satellite and epoch, per-epoch
  median removed (`products.txt` of `runs/_rap_clv_g3_field6` and `runs/_rf0hv10_field6`;
  `clv_c1w_follow.py`): the change of (B_C1W - B_C1C) regressed on the change of the clock
  record has slope 0.180 0.226 0.246 0.243 0.209 0.156 by hour (1 would be a full follow, 0
  none), so a C1W user's code side moves by 0.106 0.146 0.157 0.172 0.211 0.277 m rms by hour
  against the base, where the level re-base intended none.
- The fit reads the level through the free column. At 40 of the 58 field stations both C1C and
  C1W carry full-weight rows (first 30 minutes); where both exist the C1C row often carries the
  second code channel's deweight (MBAR00UGA on G05 at the first epoch: w 0.0101 against
  1.005). In hour 2, of 57 280 (station, satellite, epoch) cells with a full-weight band-1 and
  band-2 GPS code row, band 1 was read through C1W at 18 081 (31.6 percent): 16 901 where C1W
  is the heavier (44 percent of the 38 029 dual-tracking cells) and 1 180 with C1W alone
  (`runs/_rap_clv_g3_field6/logs/network_pf_rows.csv`).
- The census splits by the consumer's latched band-1 code (`runs/_msc/site_list.json`; the
  replays' `logs/closed_loop_trace.csv`, `clv` against `rf3gl2nb`): the 6 C1C consumers count
  better at 4 sites and worse at 2 (sum +88), hours 2 to 6 median change -0.018 m, hour 1
  +0.118 m; the 10 C1W consumers better at 4 and worse at 6 (sum -1044, ARHT00ATA 562 to 0,
  QUIN00USA 583 to 256), hours 2 to 6 +0.041 m, hour 1 +0.581 m. On the Galileo wire
  (`clvgal` against `gals`) the C1C consumers count better at 1 and worse at 5 (sum -141) and
  the C1W consumers better at 2 and worse at 8 (sum -764).
- The split emulation is the same mechanism on Galileo's free C1X. The recovery is no longer
  frozen but converges at the rate the free column follows: 0.648 0.385 0.314 0.272 0.235
  0.167 m by hour, the per-satellite range over hours 2 to 6 up to 0.772 m (E30) against at
  most 0.087 m among the base's worst five (`runs/_rap_clv_galx_sim6`, `g3_recovery.py`); the
  tied C5X recovers throughout (0.029 to 0.030 m).
- E12 on the Galileo field is the mechanism at 8 m. On card GAL's base
  (`runs/_rap_gal_g5s_field6/logs/network_pf_rows.csv`, full-weight rows, `clv_e12_family.py`)
  E12's pinned pilot family reads C1C -2.71 m (18 stations) and C5Q +1.56 m (17 stations),
  ionosphere-free median -8.16 m, while its combined family (C1X free, C5X tied) reads C1X
  +0.05 m and C5X -0.03 m (21 stations), ionosphere-free +0.15 m: the free C1X bias, with the
  per-line-of-sight slant ionosphere taking band 5, absorbed an 8 m level error that the pinned
  family carries in its residuals (the per-band signs are the level's signature with the free
  slant ionosphere absorbing part of it, as in GAL section 6.1). With the card, the fit
  mixes both families and E12's filter enters a limit cycle from TOW 265440: 77 breaks, 43 at a
  median effect of -2.47 m with a median of 16 pairs and 34 at +2.79 m with 10.5 pairs, 185.8 m
  of shifts in all (`runs/_rap_clv_gal_field6/logs/network_clock_level.csv`); the pilot
  ionosphere-free median only moves to -6.19 m and the combined family's to +0.76 m, E12's C1C
  rows above 3 m stay at 701 (715) and its C5Q rows rise from 80 to 739. The G4 E12 bar is
  therefore an R-A bar.

The hour-1 losses at WTZZ00DEU and in the censuses are not split between this mechanism and the
gain-1 arming of 6.4, item 2: both act in hour 1.

**6.4 Seen and not fixed.**

1. The lock itself. `CouplingRowBuilder::couplingQ` returns the legacy satellite-clock process
   noise for a clock term (`NetworkCoupling.cpp:60-62`; the kappa = p / (p + q dt) shrink at
   `:215-222` and `:313`), `filter.process_noise.sat_clock_q` = 1e-3 m2/s taken at
   `NetworkRunContext.cpp:51`, while the dense fold binds the same clock at the Q3 anchor's
   2.9e-8 m2/s (the anchor replaces the clock's folded variance,
   `NetworkRunContext.cpp:3666-3671`; the time update's own contract,
   `network.carry_coupling_time_update` at `GnssConfig.hpp:8486`, armed on both baselines, is
   the dense fold's per-kind q ladder). With the clock's carried variance at the anchor floor
   (1e-5 m2) and a 30 s gap, kappa is about 3e-4 for a clock term and about 1 for a phase-bias
   term (`sat_phase_bias_q` 1e-12), so a carried ambiguity row keeps its phase-bias regression,
   drops its clock regression and states the level at phase precision (test
   `TimeUpdateTurnsAnAmbiguityRowIntoALevelStatement`). Solve-wide; its own card.
2. The restart gain of 1 at arm and at a break (`NetworkClockLevel.cpp:214`): one epoch's effect
   becomes a level step (E25 on the Galileo sim at TOW 273570: effect +1.321 m, step -1.255 m;
   G03's alternating breaks in G1). After the field's clock-frame re-reference at TOW 276150
   (31 datum decouples) all 30 GPS satellites re-arm at gain 1 at TOW 276270 (shifts up to
   0.674 m), 7, 9, 7, 1 and 1 of them trip on NIS at TOW 276300, 276450, 276600, 276750 and
   276900 and re-arm at gain 1 after each (shifts up to 1.259 m): 25 decouples above the base in
   hour 5 (`runs/_rap_clv_g3_field6/logs/network_events.csv`, `logs/network_clock_level.csv`).
3. The clock pivot is never re-based: the filter runs only while the Q3 anchor holds a clock
   (`NetworkClockLevel.cpp:437`), the anchor does not hold the pivot, and the shifts are
   de-meaned over the updated satellites (`:456-470`), so the pivot keeps its ignition relation
   to the rest: G01 at -0.913 m at epoch 240 on the Galileo sim, -0.33 m on the GPS-only G2
   (`products.txt`, `clv_df_sat.py`).
4. Consumer fragility: ARHT00ATA diverges under a decimetre-level change of its code side
   (hour-1 p50 460 m, count 562 to 0 on `clv`; 487 to 23 on `clvgal`;
   `runs/_rap_msc_arht_clv_d/logs/closed_loop_trace.csv`). Consumer-side.
5. `declared_dim == n_below` is checked at 6 epochs, all in ignition (the trace budget,
   `network.trace_estimability_budget` default 6; `NetworkSolveGlue.cpp:751-781`).
6. The ephemeris-cutover re-level moves only the `local_carry` and `zwd_carry` coupling terms
   (`NetworkRelevel.cpp:115-124`), while the ionosphere, hold, receiver clock, receiver
   phase-bias and receiver code-bias carries also hold clock references (the set
   `clockLevelShiftCarry` moves, `NetworkClockLevel.cpp:249-256`). Not measured on an armed
   posture.
7. Before arming the level is the single-epoch code solve's: datum-free 0.69 m at epoch 1 and
   1.30 at epoch 4 before the filters arm at about 150 s (`runs/_rap_clv_g1f_sim2`).
8. Not attributed: G2's phase-side wire (+0.001 to +0.002 m on the diffmedian estimator, hour 6
   +0.094 m on the per-epoch median) and its 3 gross rows above 10 m.

**6.5 Verdict: NOT ARMED, SEQUENCED.**

The level is fixed on the sim (every G1 and G2 truth bar; the inter-system bias on both Galileo
sim arms) and not on the field or the split emulation, for one read mechanism (6.3). The card
stays open. Return plan, one change per step, each graded on 2 h arms first (the first two
hours of this build's 6 h runs are the reference, the solution being causal) and on 6 h only
for adoption:

1. R-A, the free same-band families. Read the level only from ionosphere-free pairs of the
   pinned pair (GPS C1C and C2W; Galileo C1C and C5Q), and move every free same-band extra
   code-bias column of the satellite (GPS C1W, Galileo C1X) by the level shift in
   `clockLevelShiftCarry`. 2 h checks: on `_rf0hv10_field6` with the knob, the C1W follow
   slope (0.180 and 0.226 in hours 1 and 2, target near 1), the IF code-residual spread (0.15
   and 0.17 m; base 0.19 and 0.27) and WTZZ00DEU hours 1 and 2 (0.791 and 0.051 m; base 0.560
   and 0.059); the split emulation's C1X minus C1C recovery (0.648 and 0.385 m); on the Galileo
   field set, E12's pilot ionosphere-free median (-8.86 m; base -8.44), its C1C rows above 3 m
   (128; base 79) and its breaks (2); the 16 census replays graded on hours 1 and 2, split by
   the latched band-1 code.
2. R-B, the arm and the break. Arm, and re-arm after an anchor decouple, from the prior's
   information instead of a gain of 1, and break only on an innovation that persists over
   consecutive epochs. 2 h checks: G1's datum-free error at epochs 16 and 60 (0.43 and 0.13 m)
   and its break count (9); WTZZ00DEU hour 1. The post-re-reference cycle lies outside a 2 h
   window (TOW 276150), so it is graded on an event-harness window around that epoch: anchor
   decouples in the following hour (56 on this build against 31).
3. R-C, the clock pivot. Reference each constellation's shifts to its clock pivot instead of
   the mean of the updated satellites. 2 h checks: on the GPS and Galileo sim set, the
   per-constellation datum-free rms with the pivot included (GPS 0.199 m at epoch 60 with G01
   at -0.950 m, 0.109 m without it) and the consumer inter-system bias against the prediction.

Then G2, G3 and G4 at 6 h with the censuses `clv` and `clvgal` for adoption. The coupling kappa
of 6.4, item 1, is the root of the lock and stays a separate card.

## 7. Return plan, batch 1 (pre-registered 2026-09-25, before any arm was graded)

**7.1 The share a free same-band column follows (a correction to R-A as written in 6.5).**
R-A moves a free same-band extra by the whole level shift d. Consider a station that reads
band 1 through the free extra k (GPS C1W, Galileo C1X) and band 2 through the pinned second
signal (GPS C2W; Galileo C5X, tied to C1C by its geometry-free pin, section 6.3). An ignition
level error e enters its code rows as -e + x_k + I and -e + mu2 I, with mu2 = (f1/f2)^2. The
fit absorbs both with I = e / mu2 and x_k = e (1 - 1/mu2): the free column holds 0.393 e on
GPS L1/L2 and 0.442 e on Galileo E1/E5a, not e, and the ionosphere-free combination of that
station's pair carries no level error. A consumer reading that pair is therefore on a
consistent wire before any re-base. A re-base that moves the clock by d and the extra by x
changes that consumer's ionosphere-free code by -d + a1 x, a1 = f1^2 / (f1^2 - f2^2) = 2.546:
zero for x = d (1 - (f2/f1)^2), +1.55 d for the whole-shift follow of 6.5, and -0.49 d for the
partial follow the built card measured (slope 0.18 to 0.25, section 6.3). The last matches the
census split of 6.3 (the C1W consumers lose, the C1C consumers gain). The pin-pair read of R-A
stands: at a station reading C1W and C2W the ionosphere-free pair is blind to the level and
dilutes the effect.

**7.2 Built (every knob inert unless `network.clock_level_carry`; every default reproduces the
built card).** Worktree build on main `335f640`, exe `gnss_processor_clvrp1.exe`, sha256 prefix
`a2060462f5ba3ec2`.

- `network.clock_level_pin_pair_read` (R-A): the ionosphere-free pairs of the two-way fit come
  only from the rows of the satellite's realized pin pair (`this_epoch_if_pin_real`); a column
  outside it is counted and left out.
- `network.clock_level_free_cb_follow` (R-A): `off`, `if` or `full`. A free same-band extra (a
  code on the band of the pin pair's first signal that is not in the pair) moves by
  d (1 - (f2/f1)^2) (`if`) or by d (`full`): its carried value, the `s_prev` of every coupling
  term on it, its time-linked datum reference (`code_bias_prev_`), and a declaration on its
  next B record (`lvl=`), which the emitter's code-bias jump test removes in the producer and
  in a replay.
- `network.clock_level_datum_pause` (R-B): at a datum change every armed filter is marked; a
  marked satellite whose anchor is re-validating is skipped with its information kept.
- `network.clock_level_confirm_epochs` N (R-B): an arm, or a break, needs N consecutive epochs
  (a break: the same sign) and restarts at their information-weighted mean effect with their
  summed information; an epoch waiting for confirmation applies no shift.
- `network.clock_level_pivot_ref` (R-C): the pivot's filter runs; every other satellite of the
  constellation shifts by its update minus the pivot's, the pivot by nothing; without a pivot
  update the de-mean is used and counted.

**7.3 The batch (2 h arms, tags `clvrp1_*`, run dirs `runs/_rap_clvrp1_*`; bases: field
`_rf0hv10_field6`, sim `_rf0hv10_sim6`, split emulation `_rap_gal_g3x2_sim6`, Galileo field
`_rap_gal_g5s_field6`, Galileo sim `_rap_gal_g2b_sim6`).**

| tag | base | change (with `clock_level_carry` unless stated) | question |
| --- | --- | --- | --- |
| `f_g0`, `s_g0` | field, sim | nothing | the build's knob-absent identity |
| `f_clv`, `s_clv`, `x_clv`, `fg_clv`, `sg_clv` | all five | the built card | reference on this build |
| `f_pp` | field | pin-pair read | the dilution alone |
| `f_ppif`, `x_ppif`, `fg_ppif` | field, split, Galileo field | pin-pair read, follow `if` | R-A as corrected |
| `f_ppfull`, `x_ppfull` | field, split | pin-pair read, follow `full` | R-A as written |
| `f_cf`, `s_cf` | field, sim | confirm 3 | R-B's arm and break |
| `s_pv`, `sg_pv` | sim, Galileo sim | pivot reference | R-C |
| `fw_clv`, `fw_dp` | field, 3.75 to 5.75 h | the built card; datum pause | R-B across the G01 handover (TOW 276150) |
| `f_all`, `s_all`, `x_all`, `fg_all`, `sg_all`, `fw_all` | all | every sub-knob, follow `if`, confirm 3 | the candidate |

**7.4 Frozen reading.**

| bar | arms | pass |
| --- | --- | --- |
| B0 | `f_g0`, `s_g0` | `products.txt` (comments stripped), `closed_loop_trace.csv`, `network_epoch_status.csv` identical to `_rf0hv10_{field,sim}6` over the window; the `*_clv` arms identical to the built card's 6 h arms over the window, or else the batch's own `*_clv` arms are the references |
| B1 | `f_ppif` | WTZZ00DEU hours 1 and 2 at most 0.570 and 0.069 m; the IF(C1C, C2W) residual spread in hour 2 at most 0.10 m (built card 0.17, baseline 0.27); the C1W follow slope against `f_g0` within 0.05 of 0.393 in hour 2; on the 16-site census replayed on the 2 h wire, paired against `rf3gl2nb` per site: the median change of the hour-2 p50 at the C1W sites at most +0.010 m, and at the C1C sites at most 0 |
| B2 | `x_ppif` | the C1X minus C1C recovery rms in hour 2 at most 0.10 m (built card 0.385, base 0.868); the C5X minus C5Q recovery not above 0.035 m |
| B3 | `fg_ppif` | WTZZ00DEU hours 1 and 2 not worse than card GAL's base (0.197 and 0.063 m) by more than 0.010 m; E12's breaks and pilot ionosphere-free median reported |
| B4 | `f_cf`, `s_cf` | the sim datum-free error at epochs 16, 60 and 240 not above the built card's (0.43, 0.13, 0.07 m) by more than 0.05 m; fewer breaks; WTZZ00DEU hour 1 not worse than `f_clv` |
| B5 | `fw_dp` | Q3 anchor decouples in the hour after TOW 276150 at most 34 (the base's 31 plus 3; built card 56) |
| B6 | `s_pv`, `sg_pv` | the GPS datum-free rms at epoch 240 with the pivot at most 0.03 m above the rms without it; on `sg_pv`, the consumer inter-system bias within 0.10 m of the prediction from hour 2 |
| B7 | `*_all` | reported; if B1 to B6 pass, `*_all` is the candidate for the 6 h gates G2 to G4 and the censuses |

A bar missed is a result, not a stop: the next batch changes one thing against it.

## 8. Batch 1 result (2026-09-25)

Every number is read from the run directory named (`runs/_rap_clvrp1_<tag>`). Graders (read-only):
the card's `clklevel.py`, `g3_recovery.py`, `clv_c1w_follow.py`, `clv_df_sys.py`,
`clv_e12_family.py` and `gross_sys.py`; `scripts/gal_grade.py`;
`scripts/wire_vs_code_census.py` with the fix of 8.5; and the session graders `ident2h.py`
(windowed identity on the post-OWD rules: records only, `n_sats` dropped), `census2h.py` (the
16 census sites replayed for 2 h on a wire, paired against `rf3gl2nb`), `codeside.py`,
`phaseside.py` and `iflevel_code.py` (the ionosphere-free code side against the CODE products).

**8.1 The bars.**

| bar | reading | verdict |
| --- | --- | --- |
| B0 | `f_g0` and `s_g0` identical to `_rap_owd_field6f` and `_rap_owd_sim6f` over the window (records, closed loop, epoch status); against `_rf0hv10_{field,sim}6` the records are identical and the consumer differs by 0.1 mm (card OWD O2); the `f_clv` records identical to `_rap_clv_g3_field6` | PASS |
| B1 | `f_ppif`: WTZZ00DEU 0.966 / 0.056 m (bars 0.570 / 0.069); IF(C1C, C2W) residual spread 0.14 / 0.15 m (bar 0.10); C1W follow slope 0.386 / 0.392 (bar 0.393 +/- 0.05); census change of the hour-2 p50, C1W sites +0.158 m (bar +0.010), C1C sites -0.011 m (bar 0) | FAIL (the slope and the C1C sites pass) |
| B2 | `x_ppif`: C1X minus C1C recovery 0.359 / 0.080 m (bar 0.10 in hour 2; built card 0.648 / 0.385; `x_ppfull` 0.489 / 0.279); C5X minus C5Q 0.030 / 0.030 m | PASS |
| B3 | `fg_ppif`: WTZZ00DEU 0.386 / 0.054 m (bars 0.207 / 0.073); E12 pilot IF median -7.63 m (built card on this build -8.69), E12 C1C median -0.09 m (-1.53); Galileo code rows above 3 m 553 (284) | FAIL in hour 1 |
| B4 | `s_cf`: datum-free 0.45 / 0.12 / 0.03 m at epochs 16 / 60 / 240 and no break, but the sim consumer 0.941 / 0.199 m against `s_clv` 0.358 / 0.088, with 3 NIS anchor trips right after the delayed arm (TOW 259410 and 259440; none on `s_clv`); `f_cf` WTZZ00DEU hour 1 0.925 m against 0.791 | FAIL |
| B5 | `fw_dp`: 31 anchor decouples in the hour after TOW 276150, all datum-class (`fw_clv` 40: 31 datum, 9 NIS) | PASS |
| B6 | datum-free at epoch 240 with the pivot: `s_pv` 0.03 m (`s_clv` 0.07), `sg_pv` GPS 0.033 and Galileo 0.036 m (`sg_clv` 0.167 and 0.082); consumer inter-system bias minus the prediction +0.494 / +0.443 m in hours 2 / 3 (`sg_clv` +0.013 / -0.000) | the datum part PASS, the inter-system bias FAIL |

Consumer 3D p50 by hour (m), 2 h: field WTZZ00DEU `g0` 0.560 0.059, `clv` 0.791 0.051, `pp`
1.011 0.056, `ppif` 0.966 0.056, `ppfull` 0.872 0.054, `cf` 0.925 0.078, `all` 1.017 0.093; sim
`g0` 0.598 0.103, `clv` 0.358 0.088, `cf` 0.941 0.199, `pv` 0.209 0.109, `all` 0.941 0.203;
split emulation `x_clv` 0.220 0.059, `x_ppif` 0.516 0.071, `x_ppfull` 0.316 0.089, `x_all`
0.379 0.050; Galileo sim `sg_clv` 0.652 0.056, `sg_pv` 0.510 0.040, `sg_all` 0.378 0.089;
Galileo field `fg_clv` 0.260 0.042, `fg_ppif` 0.386 0.054, `fg_all` 0.286 0.045.

The field census (median change of the hour-1 / hour-2 p50 against `rf3gl2nb`, 10 C1W and 6
C1C sites; the replay of `f_g0` equals `rf3gl2nb` at every site):

| wire | C1W sites | C1C sites | median p50 by hour |
| --- | --- | --- | --- |
| `f_clv` | +0.581 / +0.075 | +0.118 / -0.034 | 1.389 / 0.280 |
| `f_pp` | +0.477 / +0.174 | +0.048 / -0.014 | 1.533 / 0.284 |
| `f_ppif` | +0.412 / +0.158 | +0.059 / -0.011 | 1.452 / 0.288 |
| `f_ppfull` | +0.333 / +0.150 | +0.083 / -0.005 | 1.297 / 0.341 |
| `f_cf` | +0.390 / +0.044 | +0.072 / +0.006 | 1.298 / 0.234 |
| `f_all` | +0.420 / +0.029 | +0.264 / +0.002 | 1.329 / 0.204 |
| `rf3gl2nb` | | | 0.670 / 0.169 |

**8.2 The follow share is right where the truth is known.** On the split emulation the corrected
share recovers the injected C1X minus C1C constants to 0.080 m in hour 2 (B2) where the
whole-shift follow leaves 0.279 m, and the Galileo datum-free error at epoch 240 falls to 0.062 m
(built card 0.436, whole shift 0.083). On the field wire the share does what section 7.1 derives:
a C1W user's ionosphere-free code side moves 0.021 / 0.020 m rms across satellites against `f_g0`
(`codeside.py`), against 0.079 / 0.087 m for the built card and 0.160 / 0.160 m for the whole
shift; the measured follow slope is 0.386 / 0.392.

**8.3 The field against the CODE products: the C1W family was never consistent.** The
ionosphere-free code error a user sees per satellite, network minus CODE (the wire clock is the
residual against the CODE a-priori, so X = -c0 + a1 (B1 - OSB1) - a2 (B2 - OSB2)), per-epoch
median removed, rms across satellites of the hourly per-satellite mean (`iflevel_code.py`):

| wire | C1C user, hours 1 / 2 | C1W user, hours 1 / 2 |
| --- | --- | --- |
| `f_g0` (the baseline) | 0.290 / 0.293 | 0.360 / 0.392 |
| `f_clv` | 0.223 / 0.203 | 0.336 / 0.373 |
| `f_pp` | 0.191 / 0.189 | 0.332 / 0.375 |
| `f_ppif` | 0.191 / 0.189 | 0.359 / 0.398 |
| `f_ppfull` | 0.190 / 0.188 | 0.436 / 0.470 |
| `f_all` | 0.193 / 0.185 | 0.371 / 0.418 |

The level re-base is a correction on an independent reference for a C1C user (0.29 to 0.19 m).
A C1W user's code side is 0.36 to 0.39 m off CODE on the baseline wire, more than a C1C user's:
the free C1W family carries its own frozen error. Section 7.1 assumed that the free column
absorbed exactly the level's share, so that its user was consistent; that holds on the sim, where
the free family is determined by the level alone, and is refuted on the field. The corrected
share preserves the C1W family's error; the whole shift adds to it.

**8.4 Why every variant loses hour 1 at a cold consumer (a reading consistent with the data, not
independently proven).** The wire's phase side moves against `f_g0` by 0.08 to 0.11 m rms per
satellite, mostly a per-satellite constant (0.02 m after removing each satellite's mean, which a
float user absorbs), with its steps concentrated at the arming epochs (TOW 259350 to 259500,
summed 1.0 to 1.75 m over the satellites in one epoch, `phaseside.py`); the code side moves at
the same epochs by the level itself (up to 2.8 m on the sim). A consumer that started at the
producer's ignition has put part of the old code level into its slant ionosphere (71 percent,
section 1) and re-converges; the consumer does not read `lvl=`. The loss appears where the wire
ends closer to the truth (`x_ppif`: recovery 0.080 m, consumer hour 1 0.516 against `x_clv`
0.220), and in single-site trajectories (QUIN00USA converges by epoch 40 on the reference and
sits at 3 to 4 m until epoch 100 on every variant).

**8.5 Tooling.** `scripts/wire_vs_code_census.py` read field 5 of a B record as the positional
code disc counter and stopped on a key=value token (the B-record `lvl=`); it now skips a field
holding `=`, the C++ reader's rule.

**8.6 Verdict: NOT ARMED; batch 2 (one change each, 2 h).**

1. R-A2, the free family's own level: re-base each free same-band column from the same-band
   difference (C1W minus C1C, C1X minus C1C) at stations that track both, a two-way fit of those
   differences (satellite effect, per-station offset), instead of the clock's share alone. Check:
   a C1W user's code side against CODE (0.36 / 0.39 m) and the census C1W sites.
2. The consumer's response to a declared level step: on `lvl=` the consumer re-floats that
   satellite's slant ionosphere, the state that absorbed the old code level. Check: `x_ppif`'s
   consumer hour 1 (0.516 m) and the census hour 1.
3. R-C with the pivot-referenced common shift declared as a constellation clock re-reference, so
   a consumer re-inflates its inter-system bias as it does at a pivot handover. Check: `sg_pv`'s
   inter-system bias within 0.10 m.
4. R-B: the datum pause is kept; the confirmation arm is dropped until the anchor trips after
   its delayed arm are read.
