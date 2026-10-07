# SCL2: the clock level's residue under the clone at long horizons

**STATUS**

- **id:** SCL2
- **topic:** network-vehicle
- **status:** open
- **summary:** BUILT, NOT ARMED: the residue is B1's per-station harvest compounded over the horizon; X2's shared history cuts the hour-6 de-commoned level from 0.042 to 0.017 m (broadcast bed); continued in card SCL2B.
- **source:** [SCL](SCL_self_contained_clock_level_2026-10-03.md) sections 3.7, 4.1, 5, 9 and 10; [CTU](CTU_coupling_time_update_2026-09-28.md) section 4; [CTU2](CTU2_level_recursion_followup_2026-09-29.md) section 4 (the level instrument); [AIL](AIL_ambiguity_information_loss_2026-10-01.md) (the joint carry as a design question); [WSX](WSX_within_station_cross_terms_2026-09-30.md); [PRM](PRM_parameter_registry_2026-09-29.md) section 4.2 (the route field); [SFR](SFR_sim_fixture_coloured_phase_2026-10-01.md); card CLK-R (the emitted clock's frame hold)
- **knobs / posture:** proposed, default off, one of: X1 `network.carry_station_locals_dense` (the dense border route for the carried station-local kinds) or X2 `network.carry_coupling_shared_history` with `network.carry_coupling_shared_history_rank` (requires B1). Decision (section 9): X2 at k = 1 (rank default 1); X1 not built; X2 landed default off with card SCL2B. The arms use card SCL's knobs as it left them (`network.carry_coupling_clock_clone`, `network.handover_stransform_dense`, `network.clock_anchor_datum_keep`, the smoothed hold, `network.clock_anchor_q` 5e-6, `files.internal_data_root` for the bed). Nothing armed; no golden, ledger or posture change
- **dates in the text:** 2026-10-05, 2026-10-06, 2026-10-07
- **hashes / run dirs:** exe `gnss_processor_lres.exe` (a copy of main `5b5cb45`'s `gnss_processor_scl.exe`, sha256[:16] `8a1aa9255f3321d8`); arms `runs/_rap_scl2d_*` (16; `d1` byte-identical to card SCL's `runs/_rap_scl16_on6_s`); data roots `runs/_scl2_data_rw` (walk file sha256[:16] `39381c6f62a87a33`), `runs/_scl2_data_rww` (`7fc563fa6368d9d2`), `runs/_scl2_data_rws` (`c7ad1cd64a67f0f8`); card SCL's arms and data roots as cited; unit-network probe `tests/test_scl_unit_probe.cpp` in the session worktree at `5b5cb45` (uncommitted, its patch in the session scratchpad)

**Outcome.** Question: what holds the clock level under card SCL's clone (B1) at a broadcast-grade q. The between-satellite level freezes on both beds at every q while its statement keeps shrinking; the unit network reproduces it (24 stations, epoch 2400: B1's form 21 mm, the batch 5.7 mm). U2 chose X2 at k = 1: G1's hour-6 and statement bars PASS; its ratio clause (the code's realization, section 12), both censuses and G7's consumer FAIL. Card [SCL2B](SCL2B_shared_history_with_the_hold_2026-10-07.md) projected card SMH's hold rows (X2b): the sim census and the field census median pass, G4's site count still FAILS; X2 with X2b landed default off, nothing armed.

## 1. The question

Card SCL's clock clone (B1, `network.carry_coupling_clock_clone`) removed the level lock that card
CTU's per-row time update causes at a broadcast-grade clock walk under the producer's per-satellite
phase anchors (card SCL section 4.1). A residue remained: on the sim the level settles near 4 cm and
does not converge (card SCL section 9: 6 h de-commoned clock error 0.049 0.043 0.044 0.040 0.039
0.042 m by hour, broadcast bed, q 5e-6), and at the golden q B1 costs 5 mm (card SCL gate G6). Card
SCL named the per-station harvest's second-order term as the leading candidate without showing it.

The card's question: what holds the level, and what recursion lets it converge at the code-only
rate with a clock that walks at the broadcast grade, on any a priori.

## 2. What card SCL left, and the right target

The self-contained producer has no precise a priori, so the level's information is the code's.
On the sim the COD a priori is the truth (the simulator's clocks are the COD CLK), so the golden-q
COD control (0.015 m in hour 6) is helped by a perfect a priori. The target is therefore the
code-only bound 1/I_L (section 3.1), which the unit network shows the exact recursion reaches at the
broadcast q (section 3.6), and a stated level consistent with the error.

## 3. Fresh evidence (2026-10-05 and 06)

Sim, 6 h or 2 h from 00:30:15 (`network.start_offset_hours` 0.504167), from
`config/golden/golden_sim_refound.json` with `scripts/rapid_arm.py`, on main's exe (header). Every arm
emitted every epoch with stderr empty; B1's counters were non-zero on every B1 arm (29.4 clones per
epoch, 0.67 M rows with clone terms at 6 h, 0 fold fallbacks, 0 gate-view failures, no INERT
warning); B2 recorded 0 dense edges (G02 is the pivot all day on these beds). `d1` is byte-identical
to card SCL's `runs/_rap_scl16_on6_s` (products, closed-loop trace, epoch status), so B2 and the
keep are inert on the bed and main's exe equals the worktree's there.

**The beds.** Card SCL's `runs/_scl_data_bc` (the broadcast clock) and `runs/_scl_data_cod` (COD, G01
dropped), and three new roots built by `scripts/clock_level/scl2_rw_bed.py` from the COD root: every
GPS AS record plus a seeded random walk at exactly q = 5e-6 m2/s per satellite, continuous over the
three days (`_scl2_data_rw`); the same plus white noise of 8.2 mm per record (`_scl2_data_rww`,
matching the broadcast error's pooled 30 s random-walk equivalent, 1.62e-5 m2/s); the same walk plus
per-satellite white noise matching each broadcast satellite's own 1-epoch increment (`_scl2_data_rws`:
10 to 29 mm on G02 G05 G07 G12 G13 G15 G16 G17 G19 G20 G21 G22 G29 G31, 0 on the other 18). The
broadcast error's hour-scale motion is smaller than the matched walk's (pooled de-commoned
random-walk equivalent 3.3e-6 / 2.0e-6 / 2.4e-6 m2/s at 15 min / 1 h / 2 h against 7.1e-6 / 6.0e-6 /
6.1e-6), so the walked beds carry two to three times the broadcast's hour-scale variance.

**3.1 What the level grades read.** `scl_grade.py` and `levelnees.py` read the level from the wire:
e_s = C_s + ds_s + w_s minus the same for the pivot, with C the emitted clock record, ds the
IF(C1C,C2W) truth OSB share and w the a priori error. The smoothed hold edits only phase-bias records
(`NetworkPbHold.cpp`, `NetworkPbHoldClone.cpp`: no write to a clock value, no estimator call), and
the C records are identical between the two holds on the base exe (`runs/_rap_scl1_c_s` against
`_scl1_cs_s`). The bound 1/I_L (`network_level_bound.csv` `inv_info_m2`) accumulates the code-only
information without decay (`NetworkClockLevel.cpp:1166-1171`): its sd is 45.3 / 32.0 / 22.7 / 18.6
/ 16.0 / 14.4 / 13.1 mm at epochs 60 / 120 / 240 / 360 / 480 / 600 / 720, identical on every
60-station arm, so e^2/(1/I_L) against a flat error grows by itself. P_L (`p_level_m2`) is not the
producer's statement either: it carries the phase biases' geometry-free direction (median sd 21 cm
at epoch 239). The statement is the prior clock-contrast variance of `network_level_innov.csv`
(`p_prior_m2`) less the walk q x 30 s. `scripts/clock_level/scl2_level.py` splits the pivot-frame
error per epoch into a common part (the mean over the non-pivot satellites, minus the pivot's own
level) and the de-commoned rms about it.

**3.2 The residue over 6 h** (rms by hour, m; q 5e-6 and the smoothed hold unless stated;
`scl2_level.py decomp`):

| arm | bed, B1 | pivot frame | common | de-commoned |
| --- | --- | --- | --- | --- |
| `scl2d_d1` (= `scl16_on6`) | broadcast, on | 0.109 0.063 0.057 0.050 0.044 0.047 | 0.078 0.047 0.035 0.030 0.021 0.021 | 0.077 **0.042 0.044 0.040 0.039 0.042** |
| `scl16_off6` | broadcast, off | 0.140 0.110 0.107 0.105 0.104 0.104 | 0.082 0.030 0.018 0.021 0.028 0.034 | 0.113 0.105 0.105 0.102 0.100 0.099 |
| `scl2d_d2` | COD, on | 0.106 0.046 0.049 0.046 0.045 0.045 | 0.073 0.017 0.009 0.007 0.005 0.004 | 0.076 **0.043 0.048 0.045 0.044 0.045** |
| `scl2d_d2off` | COD, off | 0.117 0.066 0.067 0.065 0.062 0.061 | 0.074 0.010 0.013 0.013 0.016 0.019 | 0.090 0.065 0.066 0.064 0.060 0.057 |
| `scl2d_d3` | walk, on | 0.106 0.070 0.075 0.067 0.066 0.067 | 0.073 0.051 0.047 0.038 0.038 0.035 | 0.077 0.048 0.059 0.056 0.054 0.056 |
| `scl2d_d3off` | walk, off | 0.137 0.105 0.104 0.095 0.090 0.083 | 0.083 0.044 0.035 0.020 0.015 0.019 | 0.109 0.096 0.097 0.093 0.089 0.081 |
| `scl2d_d4` | walk + white 8.2 mm, on | 0.106 0.070 0.075 0.066 0.065 0.065 | 0.074 0.052 0.049 0.039 0.039 0.036 | 0.077 0.047 0.057 0.053 0.052 0.054 |
| `scl2d_d4s` | walk + per-satellite white, on | 0.107 0.071 0.076 0.068 0.066 0.067 | 0.074 0.051 0.048 0.038 0.037 0.034 | 0.077 0.048 0.059 0.056 0.055 0.058 |
| `scl16_c6` | COD, golden q, phase-clock hold | 0.104 0.028 0.028 0.026 0.027 0.027 | 0.072 0.014 0.020 0.021 0.023 0.022 | 0.076 0.025 0.019 0.015 0.014 0.015 |

The common part on the broadcast bed is the pinned pivot's own a priori walk: its per-epoch step
(13.8 mm rms; 1.2 mm on the COD golden) correlates -0.86 with G02's broadcast-error step and +0.03
with the constellation median's, as card CLK-R's frame hold predicts. It converges (0.047 to 0.021).
Card SCL's G1 single-epoch reads on that bed include it (the epoch-120 figure 0.072 m against a
de-commoned 0.040).

**3.3 One-change arms, 2 h** (hour 2, m; e2 = median e^2/(1/I_L) at epoch 239):

| arm | bed, q, B1 | pivot frame | de-commoned | e2 | PVN NIS | clone sd p50 / max (mm) |
| --- | --- | --- | --- | --- | --- | --- |
| `scl2d_d5q1e6` | broadcast, 1e-6, on | 0.074 | 0.049 | 4.1 | 3.59 | 3.2 / 5.5 |
| `scl2d_d5q2e6` | broadcast, 2e-6, on | 0.070 | 0.045 | 4.9 | 2.46 | 3.8 / 7.7 |
| `scl14_on` | broadcast, 5e-6, on | 0.063 | 0.042 | 3.6 | | 4.3 / 12.2 |
| `scl2d_d5q1e5` | broadcast, 1e-5, on | 0.055 | 0.041 | 2.3 | 0.77 | 4.9 / 17.3 |
| `scl2d_d5q2e5` | broadcast, 2e-5, on | 0.050 | 0.042 | 1.8 | 0.45 | 5.3 / 24.3 |
| `scl2d_d7q1e6` .. `d7q2e5` | COD, 1e-6 .. 2e-5, on | 0.044 to 0.047 | 0.043 (all four) | 1.3 to 1.6 | 0.06 to 0.15 | 3.2 .. 5.3 / 5.5 .. 24.3 |
| `scl15_cq5` | COD, 5e-6, on (phase-clock hold) | 0.046 | 0.043 | 1.3 | | 4.4 / 12.2 |
| `scl6_codq5` | COD, 5e-6, off | 0.066 | 0.065 | | | |
| `scl2d_d6` | broadcast, 5e-6, on, 31 stations | 0.060 | 0.056 | 0.7 | 1.38 | 5.6 / 12.2 |
| `scl3_h30q5` | broadcast, 5e-6, off, 31 stations | 0.114 | 0.106 | 4.0 | 1.33 | |
| `scl15_g` / `scl1_c` | COD, golden q, on / off (phase-clock hold) | 0.033 / 0.028 | 0.030 / 0.025 | | | 0.9 / 0.9 (on) |

With B1 the de-commoned level is flat in q on the COD bed while the clone's posterior sd and the
harvest's added variance (13.9 / 18.0 / 22.9 / 25.0 / 26.8 m2 over the run for q 1e-6 to 2e-5)
double; on the broadcast bed a larger q moves the pivot-frame figure through the common part only.
With 31 stations the residue in metres grows with the bound (36.9 against 26.3 mm in hour 2): the
ratio of the de-commoned rms to the bound sd is 1.5 against 1.6.

**3.4 The statement against the truth** (`scl2_level.py stated`; de-commoned rms actual, median
stated sd, mean NEES, per 30-epoch window):

| window | `d1` broadcast, B1 | `d2` COD, B1 | `scl16_c6` COD, golden q |
| --- | --- | --- | --- |
| 91-120 | 39.2 / 36.4 mm, 1.17 | 41.6 / 36.5, 1.31 | 33.0 / 39.2, 0.71 |
| 211-240 | 42.6 / 23.5, 3.58 | 42.7 / 23.5, 3.43 | 22.2 / 25.7, 0.72 |
| 691-720 | 42.4 / 12.9, 12.40 | 43.2 / 12.7, 13.37 | 14.7 / 13.4, 1.24 |

With B1 the stated sd follows the code-only bound to within 4 % while the error stops shrinking near
epoch 120, so the over-statement grows with time (card SCL's "about 2 times too tight in standard
deviation" is the epoch-239 value of a growing ratio). The level-innovation monitor's truth-free
windowed level NEES (card CTU2 section 4, the run-end line, 60-epoch windows 6 to 12) sees it on the
sim: `d1` 3.15 8.86 5.52 6.29 -1.64 3.49 3.44 (median 3.49), `d2` 3.43 9.13 5.28 9.28 1.51 4.82 6.10
(5.28), `scl16_c6` 1.37 3.70 1.60 -0.00 -7.66 -4.28 -0.06 (-0.00), `scl16_off6` 18 to 36. On the
field it reads large on every posture, the golden one included (`runs/_rap_scl16_c6_f` 15.37 -6.38
-14.97 15.80 19.90 59.22 63.11; with B1 on the broadcast bed `_scl16_on6_f` 10.89 -3.29 -8.02 12.26
13.15 35.63 43.25): not a usable absolute bar there (section 7).

**3.5 Its shape** (broadcast bed with B1, 6 h, unless stated). Over epochs 121 to 720 the rms of the
per-satellite time means is 38.7 mm against a within-satellite sd of 14.3 mm (COD golden 12.9 /
12.3). The autocorrelation of the de-commoned error at lags of 1 / 2 / 3 h is 0.92 / 0.83 / 0.77
(golden 0.58 / 0.31 / 0.22). The correlation across satellites of each window's mean with the final
pattern (epochs 481 to 720) is 0.30 at epoch 60, 0.54 at 120, 0.86 at 210 and 0.97 from 420 on. The
pattern of hour 2 correlates 0.79 to 0.86 with hour 6 on every B1 arm (golden 0.19). The share of
the satellites' window-mean errors that the next 60 epochs remove (-sum(Δe e) / sum(e^2) over the
satellites) is 0.35 at
epoch 90, then 0.00 to 0.11 from 150 to 630 (mean 0.011 over 210 to 630, against the 1/n rate's
0.14 and the golden's 0.05 to 0.36): a partial lock that sets in at epochs 120 to 150. The code keeps
seeing the offset (the window-mean level innovation correlates +0.44 to +0.76 with the truth window
mean in every window to 6 h; about 0 on the golden after epoch 180).

The pattern is shared: across the a priori (de-commoned, epochs 61 to 240, correlation 0.93 between
`d1` and `d2`, both 43 mm), across q (hour 2, 0.95 to 1.00 among the COD arms of q 1e-6 to 2e-5),
and with the walked beds (hour 6, 0.94 between `d2` and `d3`, `d3` about 1.34 times `d2` plus 2.2
cm independent). The 31-station arm carries a different pattern (0.31 with `d2`), as an ignition draw
over a different station set would. The residue is broad with a heavy head (G07 22 % of the summed
squares, -103 mm flat from epoch 105, the top 8 satellites 67 %) and has no predictor among the
satellite's station count (14 to 16 here), its 1/I_L, its phase-datum events, arc edges, IF-TD
flags or its broadcast error's 1-epoch or 120-epoch motion (Spearman |rho| at most 0.18, 30
satellites). Phase-datum event epochs carry 0.5 % of the squared level steps (`scl_lvl_events.py`).

**What it is not.** Not the a priori (the de-commoned residue is the same on COD and broadcast).
Not a white part of the a priori error (`d4` and `d4s` equal `d3` within 4 mm at every epoch,
pattern correlation 1.00). Not the true walk at the broadcast's size (`d1` equals `d2`); a walk two
to three times the broadcast's at hour scales adds about 3 cm in quadrature (`d3` 0.054 to 0.059
against `d2` 0.044 to 0.048). Not the anchor q between 1e-6 and 2e-5. Not the station count in units
of the bound.

**3.6 The unit network at production scale** (the fixture of `tests/test_coupling_exact_update.cpp`
with the station count R, satellite count S and epoch count K as runtime parameters, rotated anchors
(satellite s held at station s mod R) unless stated; Release build in the session worktree at
`5b5cb45`; Monte Carlo with 1000 seeds per cell, 300 to 500 at R 60; batch = mode 4 to 1.3e-9 in
relative variance at R 16 and 24; batch NEES 0.86 to 1.07 in every matched cell; K 60 reproduces card
SCL section 4.1 to four digits).

Stated level variance over the batch's, worst non-pivot satellite, mode 3 (the shipped per-row
form) / mode 5 (B1's per-station harvest), q 5e-6:

| S | R | epoch 60 | epoch 240 | epoch 600 |
| --- | --- | --- | --- | --- |
| 3 | 8 | 0.2779 / 0.9309 | 0.2879 / 0.2880 | 0.3091 / 0.3090 |
| 6 | 16 | 0.2770 / 0.9166 | 0.2881 / 0.2882 | 0.3099 / 0.3098 |
| 6 | 24 | 0.2046 / 0.9095 | 0.2174 / 0.2176 | 0.2420 / 0.2420 |
| 6 | 60 | 0.0963 / 0.8986 | 0.1115 / 0.1117 | 0.1406 / 0.1406 |

From epoch 240 mode 5 states the level exactly as tightly as mode 3; at q 1e-6 and 2e-5 the epoch-240
and 600 columns match these within 0.01 for R of 8 or more (q-flat, as in production).

The level against the truth, R 24, S 6, q 5e-6 (rms over seeds and satellites, mm):

| epoch | 60 | 120 | 240 | 600 | 1200 | 2400 |
| --- | --- | --- | --- | --- | --- | --- |
| batch (= mode 4) | 32.4 | 23.5 | 16.5 | 11.0 | 7.7 | 5.7 |
| mode 5 | 32.7 | 24.2 | 21.8 | 21.1 | 20.9 | 20.6 |
| mode 5 MSE over the batch's | 1.02 | 1.06 | 1.76 | 3.66 | 7.44 | 12.9 |

At epoch 600 mode 5's MSE over the batch's is 1.56 / 3.24 / 3.88 / 3.58 / 3.93 at R 3 / 8 / 16 / 24 /
60 (S 6), and its NEES 2.05 / 6.96 / 12.3 / 15.7 / 28.6: the error saturates with the station count,
the over-statement keeps growing. Mode 3 locks at once (105 mm at R 24 and 93 mm at R 60, S 12;
production without B1 0.100 to 0.116 m).

Truth against the model, R 24, S 6, filter q 5e-6, epoch 600 (NEES batch / mode 5, mode 5 MSE over
the batch's): truth walk 0, 1.06 / 15.0 (3.42); matched 5e-6, 1.06 / 15.7 (3.58); plus 6 mm white per
epoch, 1.06 / 15.7 (3.57); plus 12 mm, 1.07 / 15.7 (3.56). The batch is consistent whatever the
truth does (clock motion is phase-visible and never reaches the level), and its level variance is
within 0 to 6 % of the no-walk filter's at epoch 600 (22 % at 2400): the code-only bound is
reachable at the broadcast q. Mode 5's residue is its own recursion's.

The datum decides it: with station 0 holding every satellite's ambiguity datum (the fixture's
original layout), modes 3 and 5 stay with the batch (stated ratio at least 0.998, MSE over the batch
at most 1.14, R 8 / 16 / 24, S 6, both q, K 600). Under rotated anchors the exact recursion's carried
ambiguities, conditional on the border, correlate across stations up to 0.55 (R 8, S 6, q 5e-6,
epoch 600; 0.26 at the golden q; 0.0005 with the station-0 datum), while each epoch's dropped piece
is at most 0.005 by epoch 600 (0.13 at R 3, epoch 60): the pieces compound. The largest eigenvalues
of that cross-station correlation are 0.82, 2.54 and 4.36 at epochs 60, 240 and 600; at epoch 600 the
top sixteen are 4.36 (four times), 3.74, 2.87, 1.73 (four times), 1.54, 0.51 (four times), 0.49,
eleven of them above 1 for five non-pivot satellites. The clone's conditional sd at q 5e-6 is 5.3 /
3.8 / 2.8 / 2.4 / 1.6 mm at R 3 / 8 / 16 / 24 / 60 (production p50 4.4 mm). At the golden q the
shipped form and mode 5 also plateau at long horizons under rotated anchors (MSE over the batch at
R 24, epoch 600: 7.8 for mode 3, 8.36 for mode 5).

The fixture is idealized: every station sees every satellite throughout, with no set changes,
resets, handovers or a priori, and white noise only. Its plateau (14 to 22 mm at R 24 to 60) is a
half to a third of production's de-commoned 42 to 48 mm.

## 4. The reading

**The residue is B1's per-station harvest, compounded over the horizon.** At each harvest B1 regresses
every station's clone coefficients onto the border and adds the clone's conditional variance to each
row on its own (`NetworkClockClone.cpp:627-675`, `NetworkEstimator.cpp:852-905`), dropping the
cross-station covariance the shared clone leaves. One epoch's dropped piece is small (the clone's
sd, 4.4 mm; 0.005 in correlation by epoch 600 in the fixture), but the exact recursion accumulates
those pieces into a cross-station correlation of the carried ambiguities (0.55), and the per-station
form keeps none of it. From 120 to 240 epochs on, it states the level as tightly as the shipped form,
code averaging nearly stops, and the error freezes at the pattern the first two hours drew, while the
statement keeps shrinking at the code-only rate. Card SCL section 4.1 measured the same term at 60
epochs (variance ratio 0.92) and read it as small: the defect is in the horizon. It is q-flat
because the compounding saturates, in the fixture and in production alike, which answers the
objection the q ladder raised (section 3.3: the clone's variance doubles, the residue does not move).

**The rest of production's excess is unattributed.** The fixture accounts for a half to a third of
the production residue in metres. Candidates outside the fixture: satellite set changes and arc
births and ends (card CTU's per-row routes for decoupled, re-referenced, re-initialized and departed
border columns also drop the cross-row term, q-independently; `NetworkCouplingExact.cpp:88-110`,
`NetworkCoupling.cpp:647-682`), partial visibility (rows dense over a station's satellites through
the receiver clock, 29 clone terms per row), and the re-based fixture's coloured phase (card SFR)
under a loose clock. Section 7 reads two of them before the build.

**The cure is the exact recursion, kept across stations.** Mode 4 is exact by construction: it
conditions every carried ambiguity on the new border jointly. Two production forms carry it:

- X1, the joint carry: every carried station-local column (the float ambiguities, the carried
  zenith delay and the receiver code bias) on the dense border route (card PRM's `DenseBorder`), so
  the stashed block holds the cross-station covariance and the clock's time update is P + Q on one
  block. Exact; no clone needed; its cost grows as the cube of about 1 200 carried columns at 60
  stations, and it does not scale to the 305-station network.
- X2, the shared history: B1 kept, and at the harvest the clone's shared conditional (the rows'
  coefficients on the clone residual, scaled by its posterior square root) is kept as a bounded set
  of constant history columns in the border instead of being marginalized per station. Each epoch the
  previous history and the new residual are compressed to their leading singular directions across
  stations, up to a cap of k columns per cloned clock; the remainder is marginalized per station as
  B1 does today, counted as dropped mass. Exact when the shared term's rank is within the cap. The
  fixture's spectrum (eleven eigenvalues above 1 for five satellites at epoch 600) suggests about two
  per satellite; U2 measures it. Cost of order the carried rows times (k S)^2 per epoch.

**Rejected here, with the reason.** A conservative scale on the per-station added variance (a
covariance-intersection bound): all 60 stations share every clone through the receiver clock, so the
consistent factor is about 60 per row, which would remove the phase-visible precision of every
carried ambiguity (card AIL). A fixed-lag window of clones: at the window's end the dropped piece is
again the conditional of a one-step increment given its successor, of the same size (derived, not
measured); it delays the loss. A datum hub: exact only when one station anchors every satellite
(section 3.6), which partial visibility forbids; it stays a read (section 7).

**For card SCL's record.** Its G1 pivot-frame bars on the broadcast bed mix in the frame hold's
jitter (section 3.2); its "stated about 2 times too tight" is the epoch-239 value of a ratio that
grows to 3.3 at 6 h (section 3.4); the instruments for the level are the de-commoned error against
the code-only bound and the stated NEES.

## 5. The build (default off; one form, chosen by gate U2)

**U-stage first, no production code.** The fixture gains mode 6 (X2 at a cap of k history columns per
cloned clock, the compression and the per-station remainder as the production harvest would do them)
beside modes 3, 4 and 5; X1 is mode 4.

**X1, `network.carry_station_locals_dense`** (requires `network.carry_coupling_exact_update` and
`network.shared_carry_full_cov`; makes B1 unnecessary and refuses it). The registry's route for the
Ambiguity, Zwd and RxCodeBias kinds becomes `DenseBorder` while the knob is on: a carried instance
is a shared column from its birth (at the birth variance, independent) to its end (marginalized from
the stashed block, exact); a slip, refloat or reset marginalizes and re-births it; the phase datum's
anchor ambiguity is held on its column; an anchor re-election and a clock-pivot handover are
S-transforms of the one block (card SCL's B2 kernel extended over the block). The per-station
reduction still eliminates each epoch's uncarried locals (receiver clock, slant ionosphere). Counters:
columns on the route per epoch, births, ends, re-births, transforms, factor failures, the block's
dimension and fold time.

**X2, `network.carry_coupling_shared_history`** with `network.carry_coupling_shared_history_rank`
(k, default 2; requires B1). After the solve, the clone's conditional residual and the previous
history are re-expressed as at most k history columns per cloned clock: constant border columns
(no walk), registered after every other column like B1's clones, never emitted, folded by identity,
read by the gate view as nuisance (marginalized), rows' coefficients on them kept by the harvest. The
compression keeps the leading singular directions of the rows' shared conditional term; the
remainder goes to each station's rows as B1 does today. Events: a history column referenced by no
row is dropped (counted); the clock-pivot handover re-expresses the history with the block (B2);
a satellite's departure keeps its history columns while any row references them. Counters: history
columns per epoch, rows referencing them, the kept and dropped mass per epoch (run-end p50 and max),
compression failures, an INERT warning when no row references a history column.

**5.1 Sites** (to be mapped read-only at the build, as card SCL section 5.1 was): the registry
(`NetworkParamRegistry.cpp`, the Ambiguity descriptor at about 164 to 186), column registration in
`ApplicationNetworkAssembly.cpp` (B1's site), the dense fold (`NetworkRunContext.cpp`,
`NetworkEstimator::addSharedPriorDense`, `NetworkClockClone.cpp:294-332`), the harvest
(`networkHarvestLocalCarries`, `NetworkClockClone.cpp:604-675`), the gate view
(`NetworkClockClone.cpp:113-177`), the handover kernel (`NetworkHandoverStransform.cpp`), the
snapshot (fresh convergence only: fingerprinted), the teardown.

## 6. Gates (frozen before the build)

One Release exe built from main grades every arm, and the controls are rerun on it with the new knob
off (invariant 6). Arms are 2 h, then 6 h for G1, G4, G5, G6 and G8.

| gate | bar |
| --- | --- |
| U0 | Full Debug suite with no new failure; `make_golden_config.py --verify` PASS; `config_posture_check.py --verify-loader` 0 gaps; `docs_index_check.py` passes. |
| U1, the prediction (committed before any production code) | The fixture at R 8, S 3, rotated anchors, q 5e-6, K 240: mode 5's level variance ratio below 0.5 (0.2880 measured) while mode 4 equals the batch to 1e-7; the golden-q line at K 600 printed (mode 3 0.4160, mode 5 0.2813 measured), not asserted. |
| U2, the decision (fixture, Release Monte Carlo as in section 3.6) | For mode 6 at k = 1, 2 and 4: at R 24 and 60, S 6, q 5e-6 and 2.9e-8, epochs 600 and 2400 (R 60 to 600 if the runtime forbids more): level MSE over the batch's at most 1.2, mean NEES in [0.8, 1.25], and with the station-0 datum no worse than mode 5. Decision: X2 at the smallest passing k; if no k up to 4 passes, X1 under G8's X1 bar. The reading and the decision are written into this card, dated, before production code. |
| U3, the production form | X2: the compression equals the uncompressed shared term to 1e-9 when its rank is within the cap; a history column is registered after every other column, labelled, folded by identity and never emitted. X1: an ambiguity on the dense route equals the conditional-row route in value and variance where the per-row form is exact (one station, no clock walk), to 1e-9. Knob off bitwise. |
| G0 | Knob off: 2 h, both goldens and both broadcast-bed arms (sim, field), byte-identical to the same exe's controls (products without comment lines, `closed_loop_trace.csv`, `network_epoch_status.csv`). |
| ON | The section 5 counters non-zero where they must be; 0 factor or compression failures; no INERT warning. |
| G1, the level (sim truth, 6 h, q 5e-6, smoothed hold, B1 / B2 / keep as card SCL left them for X2) | On the COD bed (control `scl2d_d2`) and the broadcast bed (control `scl2d_d1`): de-commoned level (`scl2_level.py decomp`) in hour 6 at most 0.020 m on both (controls 0.045 / 0.042; golden-q COD 0.015; bound sd 13 mm at epoch 720) and at most 0.6 times its hour-2 value (converging); the statement (`scl2_level.py stated`) mean NEES in [0.5, 2.0] in windows 211-240 and 691-720 (controls 3.43 / 13.37 and 3.58 / 12.40). |
| G2, the level (truth-free) | The level-innovation monitor's windowed level NEES, median over windows 6 to 12, at most 2.0 on both sim beds (controls 5.28 COD, 3.49 broadcast; golden -0.00). The field's windows are reported against B1 alone, without a bar (section 3.4). |
| G3, the golden posture | Golden + the build (COD, golden q, phase-clock hold), 2 h: WTZZ00DEU and SZZ000SIM within 0.005 m of the control in every hour; sim de-commoned level in hour 2 at most 0.027 m (control 0.025; B1 alone 0.030). |
| G4, census (field, 6 h, broadcast bed) | Card SCL's G4 bar against COD with the phase-clock hold: median paired change of the site p50 over hours 2 to 6 at most +0.010 m and at most 3 sites worse by more than 0.02 m (B1 alone +0.003 m, 5 sites); median no worse than B1 alone. |
| G5, census (sim, 6 h) | Card SCL's G5 bar against the COD control (B1 alone +0.005 m, 2 worse). |
| G6, the handover | 6 h on the golden root with G01 kept (clock-pivot handover at 04:42:30) in the SCL regime (q 5e-6, smoothed hold, B1 / B2 / keep for X2) with the build: 0 failed solves, 0 datum decouples at the edge, the de-commoned level in the 10 epochs after the edge within the run's p95. |
| G7, the phase side | Card PVN's NIS mean on the broadcast sim and field arms in [0.7, 1.6] (B1 alone 1.56 sim, 1.47 field); WTZZ00DEU and SZZ000SIM on the broadcast bed no worse than B1 alone by more than 0.005 m in hours 2 to 6. |
| G8, cost | 6 h loop time on the same exe, one job at a time: X2 within 15 % of B1 alone; X1 within 100 % at 60 stations, with its time at 31 stations and the cubic extrapolation to 305 stations reported. |

Arming is the author's decision after G4 and G5.

## 7. Order, stop rules, and what stays owed

1. Reads before any code (recorded here, no bars): R1, card SFR's coloured phase, one 2 h arm of the
   COD bed at q 5e-6 with B1, B2 and the keep on the white-phase fixture `runs/simnet60_quiet_wu93_24h`
   (pre-SFR, superseded for every other purpose), against `scl2d_d7` / `scl15_cq5`; R2, the datum's
   lever: the production anchor layout (distinct anchor stations per epoch, from the phase-datum
   trace) and the fixture with anchors at s mod m for m = 1, 2, 3 and R.
2. U1 committed; U2; the decision dated in this card.
3. The chosen build; U0, U3, G0, ON.
4. G1 to G3 and G7 on 2 h, then 6 h; G6.
5. G4 and G5; G8.

Stop rules. If U2 passes no k and X1's cost model (U2's timing of mode 4 scaled to 60 and 305
stations) exceeds G8's X1 bar, the card records the reading and stops before production code. If G1
fails with U2 passing, the remaining excess is attributed (R1, R2, the per-row routes of section 4)
before any further build.

Owed outside this card: the native broadcast-clock posture (card SCL section 5); a per-satellite q
from the phase-visible NIS; card SMH's orphaned deficits at a dense edge (card SCL section 10); a 24 h
sim read of the golden posture's level (the fixture plateaus at the golden q at long horizons, while
the 6 h production golden reaches 15 mm against a 13 mm bound); a field level instrument that
separates a lock from persistent code errors (the windowed NEES reads 15 to 63 on the golden
posture); card SCL's census tail sites.

## 8. Tools

In `scripts/clock_level/` with this card: `scl2_level.py` (`series`, `decomp`, `stated`: the level
over a whole run, its split into the pivot-common and de-commoned parts, and the statement against
the truth; reproduces the section 3 tables) and `scl2_rw_bed.py` (the matched-walk beds; rebuilds
`runs/_scl2_data_rw` and `_rww` byte-identically: `--q 5e-6` with the default seeds, `--white 0.0082`
for the second). The section 3.5 statistics (persistence, decay, predictors, the common part's
correlation with the pivot's step) and the fixture probe are in the session scratchpad; the probe's
cases needed by U1 and U2 move into `tests/test_coupling_exact_update.cpp` with U1.

## 9. The U-stage read and the decision (2026-10-06)

**R1, card SFR's coloured phase** (arms `runs/_rap_scl2r_*`, the three `r1` arms' RINEX re-pointed
to the pre-SFR fixture `runs/simnet60_quiet_wu93_24h`, 61 paths, the original kept as
`arm.json.col_orig`; COD bed). With B1 at q 5e-6 the de-commoned level in hours 3 to 6 reads 0.051
0.049 0.048 0.045 m on that fixture against 0.048 0.045 0.044 0.045 on the coloured one (`d2`), and
it freezes the same way (statement NEES 14.04 against 13.37 in window 691-720); the golden posture
reads 0.020 against 0.015 m in hour 6. The pre-SFR fixture is not a colour-free twin (3.0 mm white
phase and 2.0 mm per-line Gauss-Markov multipath, against 0.5 mm white and a 4.4 mm band-common
colour) and each fixture is one noise draw: the colour is not the bulk of the plateau; a share of a
few millimetres is not excluded. On that fixture the truth-free windowed level NEES reads 2.19 for a
level whose truth NEES is 14: G2's bar is read on the goldens' fixture only, as written.

**R2, the datum's lever.** `network_phase_anchor.csv` (one row per epoch, signal and satellite, the
station holding the satellite's phase-datum anchor; `NetworkDdFrac.cpp:82-102`): 9 to 12 distinct
anchor stations per epoch for the 30 GPS satellites, one station holding 5 to 10, 6 to 13
re-elections per hour, S30000SIM holding 24 to 25 % of the anchor-epochs, median tenure about 230
epochs. Satellite pairs sharing an anchor station show smaller level differences on three of the
four q-5e-6 arms (rank partial correlation given co-visibility, p 0.001 to 0.151), also without B1,
and not at the golden q. The fixture's lever (R 24, S 6, q 5e-6, K 600, 200 seeds; anchors at s
mod m), mode 5 MSE over the batch's: 1.07 / 1.57 / 2.05 / 3.75 for m = 1 / 2 / 3 / 24 on the static
geometry and 2.93 / 2.76 / 2.06 / 4.29 on the time-varying one (below). The station-0 datum's
protection is a property of the static geometry; a datum hub is not a cure. Mode 6 at k = 1 reads
1.0000 in all eight cells.

**Mode 6, X2's fixture form** (the session probe, dense and arrow-head). With no cap it equals mode 4
and the batch: 5.5e-9 in variance ratio and 3.1e-8 sigma in value on the static geometry, 9.3e-10 and
2.0e-8 on the time-varying one; the self-check D0 = Cov(N | s, Y) is block diagonal to a cross-station
correlation of 1.8e-9.

**A correction to section 4, and an extension of U2.** The "eleven eigenvalues above 1" of section 3.6
are those of the off-block correlation matrix, not the rank of the shared factor. On the static
geometry the carried ambiguities depend on the past clocks only through their sum, so the shared
term has rank nclk exactly and one history column per clock is exact by construction: U2's grid as
pre-registered cannot separate k. The worker added a time-varying geometry, not pre-registered: every
row of satellite s at station r scaled by 1 / sin(e) with e sweeping 15 to 75 degrees on a 12 h or 4 h
cycle, or drawn independently per epoch as a stress bound. The uncapped history then grows to 13 to 40
columns and the cap truncates.

**U2** (Release probe, 200 seeds per cell at every R, S 6 unless stated, rotated anchors, matched
truth; MSE over the batch's / mean NEES; k = 1, 2 and 4 agree to four digits wherever run together):

| geometry, cell | epoch | batch NEES | mode 5 | mode 6, k = 1 | dropped mass p50 / max |
| --- | --- | --- | --- | --- | --- |
| static, R 24, q 5e-6 | 600 / 2400 | 1.048 / 1.018 | 3.75 / 16.2; 12.4 / 36.6 | 1.0000 / 1.048; 1.0000 / 1.018 | 5e-17 / 3e-16 |
| static, R 60, q 5e-6 | 600 / 2400 | 0.989 / 0.981 | 4.30 / 30.3; 14.9 / 55.9 | 1.0000 / 0.989; 1.0000 / 0.981 | 4e-17 / 3e-16 |
| static, R 24, q 2.9e-8 | 600 / 2400 | 1.058 / 0.967 | 8.78 / 43.6; 35.7 / 139 | 1.0000 / 1.058; 1.0000 / 0.967 | 2e-16 / 1e-15 |
| static, R 60, q 2.9e-8 | 600 / 2400 | 0.998 / 0.982 | 10.3 / 92.1; 38.9 / 227 | 1.0000 / 0.998; 1.0000 / 0.982 | 2e-16 / 1e-15 |
| static, R 24, station-0 datum, q 5e-6 | 600 | 1.048 | 1.072 / 1.126 | 1.0000 / 1.048 | |
| 12 h sweep, R 24, q 5e-6 | 600 / 2400 | | 4.29 / 19.7; 15.5 / 45.8 | 1.0000 / 1.053; 0.9999 / 1.003 | 5e-7 / 5e-6 |
| 12 h sweep, R 24, q 2.9e-8 | 600 / 2400 | | 9.02 / 48.8; 40.2 / 170 | 1.0000 / 1.065; 1.0000 / 0.959 | 2e-6 / 2e-5 |
| 4 h sweep, R 24 / R 60, q 5e-6 | 2400 / 600 | | 15.4 / 46.0; 4.97 / 37.3 | 1.0001 / 0.990; 1.0000 / 1.020 | up to 9e-6 / 4e-5 |
| independent per epoch, R 24 and 60, q 5e-6 | 600 and 2400 | | 4.28 / 17.0 to 15.9 / 56.9 | 1.0000 / 0.974 to 0.990 | 2e-7 / 5e-2 |
| independent per epoch, S 12, R 24 / 60, q 5e-6 | 600 | 1.070 / 1.092 | 4.11 / 12.0; 4.32 / 22.3 | 1.0000 / 1.070; 1.0000 / 1.092 | 3e-6 / 4e-2 |

**Decision (section 6, U2's rule): X2 at k = 1.** `network.carry_coupling_shared_history_rank`
defaults to 1 (section 5 said 2; the decision replaces it). X1 is not built: it was the fallback, and
the read-only map scales it to about 15 to 20 s per epoch at 60 stations against 0.82 s today.

**What U2 does not cover.** The fixture has no arc births or ends, set changes, anchor re-elections,
partial visibility or card CTU's per-row decouple and re-reference routes; at those events the shared
term's rank enters in jumps. The ON counters' dropped mass per epoch (run-end p50 and max) is read at
arc edges: if production's is far above the fixture's (p50 at most 4e-5), k = 2 is the documented next
setting, graded by the same gates. The read-only map projects X2 at k = 1 at about 30 more border
columns (179 to about 209) and +10 % loop time plus 2 to 4 % for the new algebra, near G8's 15 % bar;
the lever, if G8 fails, is an X2-gated assembly that skips a history column's zero rows.

**U1** is committed as `tests/test_coupling_long_horizon.cpp` (suite `CouplingLongHorizonTest`; section 8
named `test_coupling_exact_update.cpp`, and a new file follows the suite-hygiene rule): (a) mode 5's
worst stated level variance over the batch's 0.2880 (below 0.5), mode 4 equal to the batch to 1.4e-10
in variance and 1.6e-9 sigma, the golden-q line printed (mode 3 0.4160, mode 5 0.2813); (b) mode 6 with
no cap equal to the batch on both geometries and both q; (c) mode 6 at k = 1 on the time-varying
geometry, stated ratio 1.0000 (above 0.95); (d) the uncapped history width nclk on the static geometry
and above it on the time-varying one, so that a static pass is never read as evidence for the cap.
17.7 s in Debug; the worktree's full Debug suite 2801 passed, 17 data-fixture skips, 0 failed.

## 10. X2 built and graded (2026-10-06)

**The build** (session worktree at `2b917a9`, uncommitted at the time of this section; exe
`gnss_processor_scl2x.exe`, sha256[:16] `da3114428f2bcc5e`). New seam `filter/NetworkSharedHistory.{hpp,cpp}`
following the map: history columns keyed `HIST|j` after B1's clones, never emitted or stashed, prior N(0, 1);
B1's regression widened to Y = [clones; history]; the harvest stores each row's Y coefficients and, at its
end, compresses them across every carried row of the network (Cholesky of Sigma_Y|s, eigen-decomposition of the
Gram, the kept directions as the next epoch's history coefficients, the remainder per station); the row builder
emits history terms on card CTU's exact route and marginalizes them on a stale generation or card 1d's route;
the snapshot writes the marginal variance; an armed-only fingerprint token; the run-end line and an INERT
warning. Tests `tests/test_network_shared_history.cpp` (suite `NetworkSharedHistoryTest`, 7 cases: U3). Card
SMH's clone rows are not in the compression (each marginalizes its share per row, as under B1).

**U0, U3, G0, ON.** Debug suite 2825 tests, 2808 passed, 17 data-fixture skips, 0 failed; the golden verify
PASS; the posture check 0 gaps. Knob off byte-identical to the base exe on the 2 h goldens and the broadcast
bed's SCL regime, both beds (`runs/_rap_scl2x_g0*`; one pair re-run on the base exe by the manager,
`runs/_rap_scl2v_g0bs_lres`, identical to both). ON: 29.4 history columns per epoch at k = 1, 0 compression
failures, 0 rows lost, no INERT warning.

**Production's shared term is full rank.** On every arm the rank equals Y's width (58.8 at k = 1, 88.2 at k = 2),
so the cap truncates on 712 of 713 epochs: remainder share p50 7.3e-3 at k = 1 and 1.6e-3 at k = 2 (broadcast;
COD 4.5e-3 and 8.8e-4), far above the fixture's 4e-5, so k = 2 was graded with k = 1 (section 9).

**Gates** (arms `runs/_rap_scl2g_*`; controls the B1-alone runs, same-exe by G0, plus the X2-off re-runs
`scl2g_b6off_f`, `scl2g_h6off_f`, `scl2g_hk0_s`, identical to the old controls):

| gate | reading, k = 1 | k = 2 | verdict |
| --- | --- | --- | --- |
| G1, broadcast | de-commoned level by hour 0.076 0.024 0.021 0.017 0.017 0.017 m (B1 0.077 0.042 0.044 0.040 0.039 0.042); hour 6 over hour 2 0.72; NEES 0.66 / 1.17 | 0.076 0.024 0.020 0.017 0.016 0.017; 0.70; 0.66 / 1.13 | hour-6 bar PASS, ratio FAIL, NEES PASS |
| G1, COD | 0.075 0.024 0.022 0.018 0.017 0.019 (B1 0.076 0.043 0.048 0.045 0.044 0.045); 0.77; 0.67 / 1.15 | 0.75 ratio; 0.67 / 1.13 | hour-6 bar PASS, ratio FAIL, NEES PASS |
| G2 | windowed level NEES median 0.32 broadcast, 0.58 COD (B1 3.49, 5.28) | 0.42, 0.49 | PASS |
| G3 | SZZ000SIM within 0.3 mm, WTZZ00DEU within 0.3 mm of the control in every hour; sim level hour 2 0.0265 m | 0.0262 | PASS |
| G4, field census 6 h | median +0.0093 m against `s16c`, 7 sites worse by more than 2 cm; against B1 alone (`s16on`) +0.0112 | +0.0108, 5 worse; +0.0093 | FAIL |
| G5, sim census 6 h | +0.0120 m, 5 stations worse | +0.0120, 4 worse | FAIL |
| G6, handover | 720 of 720 emitted, 0 datum decouples (31 kept), sim level after the edge 0.024 against the run's p95 0.029 | 0.023 against 0.029 | PASS |
| G7 | PVN NIS 1.55 sim, 1.46 field; SZZ000SIM +0.078 +0.086 +0.005 +0.034 +0.008 m against B1 alone in hours 2 to 6, WTZZ00DEU +0.012 -0.001 +0.019 +0.026 +0.022 | NIS the same; SZZ up to +0.126, WTZZ up to +0.030 | NIS PASS, consumer FAIL |
| G8, cost | loop time 1.106 of B1 alone (one job at a time, two interleaved repeats, spread below 1.5 %) | 1.222 | k = 1 PASS, k = 2 FAIL |

The census reads were proven first: card SCL's B1-alone figures reproduce exactly (field +0.0034 m, 5 worse, 4
better, 16 sites without WTZZ00DEU; sim +0.0049 m, 2 worse, with run-hour bins from the replays' first epoch).

**Reading.**

1. The level: X2 removes the lock of the first hours. The de-commoned level follows the code-only bound through
   hour 4 (24.1 / 20.8 / 17.2 mm against 26.7 / 20.5 / 17.2) and then flattens near 17 mm while the bound
   falls to 13.7: a later partial lock (the hour-4 pattern correlates 0.77 to 0.82 with hour 6; hour 2 with hour
   6 only 0.04 to 0.08, against 0.79 / 0.86 under B1). The rank cap does not cause it: k = 2 cuts the remainder
   fourfold and moves the level by at most 0.5 mm.
2. A regression G1 does not grade, found in the manager's re-grade: the pivot-common part (the mean over the
   non-pivot satellites of the pivot-frame error, minus the pinned pivot's own level) grows under X2 from hour 2
   on both beds, 0.017 0.026 0.028 0.034 0.038 m (broadcast) and 0.018 0.025 0.023 0.030 0.031 m (COD), where B1
   alone converges (0.047 to 0.021 broadcast, 0.017 to 0.004 COD). The estimated satellites drift together
   against the pinned pivot by 3 to 4 cm: a between-satellite error on the pivot that the de-commoned metric
   hides. Candidate: the clock-family deficit rule under X2 (the history's share moved onto the clocks is
   restored on the pinned clock), which the fixture, pinning its pivot directly, does not exercise.
3. The consumer: hybrid wires put the whole loss on the wire's phase side (SZZ000SIM: X2's code side with B1's
   phase side +0.0014 to +0.0066 m by hour; B1's code side with X2's phase side +0.173 +0.044 +0.006 +0.026
   +0.026 +0.023, equal to the full X2 replay; WTZZ00DEU likewise), and under the phase-clock hold the consumer
   does not move (G3). Card SMH's smoothed hold forms the walk share from its clone rows, which marginalize their
   share of the shared residual per row while the coupling rows now keep theirs in the history: the hold's
   past-clock estimate misses the revision the history carries. The same pattern as card SCL section 9 (B1
   before card SMH was made consistent with it).

Section 7's stop rule applies (G1 fails with U2 passing): the late partial lock, the pivot-common drift and the
smoothed hold's consistency with the history are attributed before any further build.

## 11. The three readings attributed, and the pre-registered amendment (2026-10-06)

Arms `runs/_rap_scl2a_*` on the X2 exe; the fixtures in the session scratchpad (`scl2a/test_scl_t1_probe.cpp`,
and `tests/test_scl_event_probe.cpp` in a second worktree at `13c11d3`). No production code changed.

**Reading 3, the consumer: card SMH's per-row form under X2 (attributed).** Card SMH's clone rows are the only
rows in the network still on B1's per-row form under X2: they move their clone coefficients onto the free border
and add their conditional variance to their own (0.23 m2 over the run, as under B1), while the coupling rows keep
their shared term in the history (their per-row variance added: 22.88 m2 under B1, 0 under X2). On the COD sim bed,
where the true walk share is 0, the hold's past-clock estimate c(t0 | t) is better than the current clock and
correlated 0.97 with it under B1; under X2 the current clock improves 2.5-fold, the past clock is worse than it in
every hour (22 to 27 mm against 17 to 24) and their correlation falls to 0.65 to 0.84, so the walk share's error
grows (1.43 to 1.69 cm de-commoned, against 1.10 to 1.18 under B1). On four bed pairs the X2-minus-B1 change of the
emitted phase-visible clock's motion within a hold generation equals the change of the walk share to 0.1 mm. In the
fixture (the hold's row as card SMH's fixed-point smoother target; R 8 and 24, S 6, q 5e-6 and 2.9e-8, static, swept
and per-epoch weights, 200 seeds), the walk share's MSE over the batch's fixed-point smoother at epochs 240 / 600:
the per-row form under X2 16 to 43 (B1's per-row form 1.4 to 14); the target inside the compression 0.998 to 1.015;
the target projected onto the coupling rows' kept directions ("form p", the compression unchanged) 0.998 to 1.014.
Production pairs: under the phase-clock hold (COD bed, q 5e-6) X2 moves neither consumer (SZZ000SIM within 0.9 mm,
WTZZ00DEU within 0.1 mm by hour; the 16-station sim census median +0.0000 m, every station within 1 mm); without a
hold both consumers sit at 0.3 to 1.2 m and the pair is noise.

**Reading 2, the pivot-common part: not a defect of X2.** In the pivot frame the common part is minus the pinned
pivot's own level error. Under X2 that level follows its code, as every other satellite's does; on this fixture G02's
ionosphere-free code averages about -4 cm against the truth over hours 2 to 6 (in both arms, and as a non-pivot
satellite in the golden-root arms), and B1 held an hour-1 value that the code contradicts by 5 to 7 cm. Predicted
before grading from each pivot's code residual and confirmed with the pivot rotated (`network.clock_datum_satellite`
G14 and G19, arms `runs/_rap_scl2a_t2_p*`): over 15 pivot-hours X2's common part follows the prediction (correlation
0.93, residual rms 0.010 m), B1's does not (-0.49, 0.036 m); with G19 as the pivot B1 shows a 2.7 cm common part and
X2 does not; over the three pivots both arms average 0.018 m. The clock-family deficit identity holds exactly under
X2 (history columns are linear in the gauge-invariant clone residuals, so their family weight is 0), and with the
pivot pinned by Dirichlet elimination the deficit cannot move the solve. The level graded over all satellites
including the pivot: X2 0.024 0.022 0.019 0.018 0.019 m in hours 2 to 6, B1 0.042 0.047 0.045 0.044 0.045. The
de-commoned metric of section 3.1 leaves the pivot ungraded; it is reported from here on with the all-satellite
figure beside it (amendment A2 below).

**Reading 1, the late flattening: mostly not a lock, and not X2.** An exact filter's error at hour 6 correlates with
its hour-4 error at about the ratio of their standard deviations (0.79 on the production bound; 0.70 to 0.77 in
the fixture's batch); X2 reads 0.81 to 0.83, B1 0.95 to 0.97. The excess over the bound sits in three satellites:
the top three carry 44 % (broadcast) and 54 % (COD) of the hour-6 mean square. G26 is a bound artifact: readmitted
at epoch 644, it keeps its pre-park code epochs in the undecayed 1/I_L while the filter restarts correctly (stated sd
59 mm, NEES 0.53). G20 and G12 hold about -43 and -30 mm from about two hours after their readmissions (epochs 63
and 223) in every posture: B1 alone, X2 at k = 1 and 2, both beds, and the golden-root arms (G12 differs only under
the golden-root B1). Two artificial outages through the same production path recover normally, and turning the
readmission seed off does not free them; their cause is open. Without those three satellites the de-commoned level
tracks the bound on all four X2 arms (hour 6: 13.9 / 13.3 / 13.6 / 13.2 mm against 13.7; mean z^2 0.93 to 1.02;
hour 6 over hour 2 0.555 to 0.586). The fixture with events (arc births and ends at 479 rises, outages with the
phase bias re-realized, 24 anchor moves with an exact S-transform; R 24, S 6, 2400 epochs, 200 seeds): X2 without a
cap stays exact through all of them; k = 2 stays within 0.12 % of the batch; k = 1 degrades (MSE over the batch's
1.24 to 3.15) only when a retired border column enters Y (an outage's return, the transform's old phase bias),
which production does not do in this form (it pins at the anchor's estimate, about 1 % after 20 h) and which would
show as a k = 1 against k = 2 difference that production does not have (at most 0.5 mm). G1's ratio clause stays
FAILED as pre-registered; this reading attributes it.

**Amendment A1, the build X2b (pre-registered here, before any code): card SMH's rows in the shared history, form
p.** When card SMH's smoothed hold is armed with X2, each hold row's Y coefficients r_T (its terms on the clones and
the current history) are projected onto the coupling rows' kept directions at the harvest's end: with F_T = r_T' L,
the row keeps F_T V_kt as its history coefficients (emitted with the history columns at the next epoch, as a
coupling row's are) and adds ||F_T V_rest||^2 to its own variance; the compression itself is unchanged. No new knob:
it is part of X2 whenever the smoothed hold is armed (X2 has not been armed anywhere). Counters: hold rows
projected, their kept and remainder shares (run-end p50 and max), hold rows marginalized on a stale generation.

Gates for X2b (the section 6 bars where they exist, unchanged):

| gate | bar |
| --- | --- |
| U4 (fixture, committed in `tests/test_coupling_long_horizon.cpp`) | The hold row's walk share in form p within 2 % of the batch's fixed-point smoother at k = 1 (R 8, S 6, q 5e-6, K 240, static and swept weights), and the per-row form at least 5 times the batch's there (printed with its value). |
| U5 (production unit) | A hold row's projection: history coefficients F_T V_kt and remainder variance ||F_T V_rest||^2 to 1e-12 for given directions; a stale generation marginalizes; X2 off and the smoothed hold off bitwise unchanged. |
| G0 | X2 off byte-identical to the base exe on the four 2 h arms of section 10. |
| ON | Hold rows projected above 0 per epoch, 0 projection failures, the counters above. |
| G1, G2, G3, G6, G8 | Re-run on the X2b exe with the section 6 bars; G1's hour-6 and statement bars must still pass on both beds; G8 within 15 % of B1 alone. |
| G4, G5, G7 | Re-run on the X2b exe with the section 6 bars unchanged (field census: median at most +0.010 m over hours 2 to 6 against `s16c`, at most 3 sites worse by more than 0.02 m, median no worse than B1 alone; sim census likewise against `s16cs`; PVN NIS in [0.7, 1.6]; SZZ000SIM and WTZZ00DEU no worse than B1 alone by more than 0.005 m in hours 2 to 6). |

**Amendment A2, the grade (reporting only, no bar changes).** G1 is reported also over all satellites including the
pivot, and with the pivot rotated once (G14) on the COD bed; the per-satellite table names any satellite readmitted
during the run, whose bound keeps its pre-park epochs.

Owed outside X2b: the frozen G20 and G12 (common to every posture, cause open); the bound's restart at a readmission
(`network_level_bound.csv` keeps the pre-park information); the G07 and G15 excursions under X2 (2.4 sigma,
reproduced in every X2 posture, absent under B1).

## 12. G20, G12, G07 and G15: the level follows the code's realization (2026-10-07)

The fixture has no per-satellite code bias: rebuilt from the simulator's observation equations, the
ionosphere-free code noise correlates 0.998 per row with the producer's post-fit code residual, is white (lag-1
autocorrelation 0.004) and matches the producer's weights (w var(n) about 9 in every elevation bin). A prediction
written before grading (`scl2f/predictions_ck1_preregistered.txt`, a least-squares level per satellite segment
that follows the code noise's running mean and reads no truth level) matches X2's level over hours 3 to 6 on every
satellite, the pivot included: correlation 0.91 / 0.92 (COD, k = 1 / 2), 0.94 with the readmission seed off,
residual 8 to 12 mm rms (z^2 0.12 to 0.32); B1 0.32 to 0.42 (about 40 mm). G20 is a 2.9 to 3.6 sigma draw of its
own code and its neighbours' through the shared station levels (-28.5 and -22.5 mm of its hour-6 -51 mm), G12
about 2 sigma. Causal test (arms `runs/_rap_scl2f_flip_{x,b}`, the COD bed, the RINEX of G20, G12, G07 and G15 with
the ionosphere-free code noise mirrored, `runs/_scl2f_flip_rinex/`, predictions written first): X2's level moves by
the predicted amount (correlation 0.994, slope 0.87, residual 8.9 mm over 16 satellite-hours; G20 turns to +24 +22
+19 +12 mm); B1 does not follow (G07 flat while its code moves 43 to 80 mm). The readmission seed lags the code (slope
0.27 to 0.62 against 0.92 without it). The parks are the producer's eclipse exclusion (G20 12 min, G12 52, G25 55);
the simulator's yaw model is the processor's nominal one and its code has no eclipse term.

Reading: the frozen satellites and the X2-only excursions are the estimator following the data's realization, not a
lock and not a defect of the readmission path. G1's ratio clause measures that draw: a level that follows its code
exactly reads 0.90 for hour 6 over hour 2 on this realization, and the mirrored data move the estimator's ratio from
0.79 to 0.70. The level grade that separates a lock from code-consistency is d_e minus the code-following
prediction, with z^2 against the code-only bound; it is the instrument for the next level card. Owed: whether the
readmission seed's pre-park prior (variance 1.2e-2 to 1.8e-2 m^2) is consistent with a 12 to 55 min gap.

## 13. Continued in card SCL2B (2026-10-07)

Amendment A1's build (X2b), its grade against the section 11 table and the landing of X2 with X2b are recorded in
[SCL2B](SCL2B_shared_history_with_the_hold_2026-10-07.md), this file being near the 60 KB bar.
