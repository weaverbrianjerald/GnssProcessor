# CLV: the satellite clock level, frozen at ignition by the carry

**STATUS**

- **id:** CLV
- **topic:** network-vehicle
- **status:** open
- **summary:** Build card, pre-registered 2026-09-22 by the author's decision to fix the root card GAL exposed before continuing. Every satellite's clock level on the wire is set by the single-epoch code solves of the first epochs and frozen by the carry from its first harvest on (datum-free error against truth 1.12 m rms across satellites on the GPS-only sim baseline, constant for six hours; per-satellite ionosphere-free code residual medians of -2.6 to +1.9 m, 71 percent of the error absorbed by the free slant ionosphere of every station). Twenty-two config-only arms locate the lock in the carried priors as a whole, not in any one knob: the Q3 anchor, its floor and walk, the held basis, the phase-bias floors, the emit hold, the birth floors and the in-block carries leave the level untouched; a walking phase bias moves it halfway at a consumer cost; only the free-ambiguity era (the harvest delayed to epoch 76) converges it at the code's rate, 1.30 to 0.16 m, at an ignition cost of 2.1 m. Design: the border carry inflated along each satellite's LEVEL direction (its clock up, its phase biases down, the phase rows invariant) by the code-only level variance, which the accumulated ionosphere-free code information shrinks like a constant's, so the level converges as in PPP while the clock walk stays anchored and the phase side of the wire does not move.
- **source:** [GAL](GAL_gps_galileo_dual_2026-09-22.md) section 6.1 (the finding and the first arms); [RF-1_float_ambiguities_design](RF-1_float_ambiguities_design.md) sections 1.3 and 1.4 (the carried priors count the level as phase-precise; "the level freezes at K_eff epochs"); [RF-IGN](RF-IGN_ignition_born_levels_2026-09-15.md) (the same lever on the field golden); [RF-2](RF-2_held_basis_2026-09-15.md) (the clock-level ramp read R-CL, never done); [Q3 design](../cards/clock/Q3-ANCHOR-DESIGN.md) and [Q3 results](../cards/clock/Q3-ANCHOR-RESULTS.md) (the anchor binds the walk, not the level)
- **knobs / posture:** new `network.clock_level_carry` (bool, default false, byte-identical off) with `network.clock_level_sigma_floor_m` (default 0.01) and `network.clock_level_horizon_s` (default 7200); on the RF-0hv working bases at main `68915a0` (GPS only), then card GAL's Galileo set on top
- **dates in the text:** 2026-09-22
- **hashes / run dirs:** baseline pair `runs/_rf0hv10_{field,sim}6`; the ladder `runs/_rap_cil_e2` to `_e23` (2 h sim); arms `runs/_rap_clv_*`; exe `gnss_processor_clv.exe`

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
