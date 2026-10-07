# SCL2: the clock level's residue under the clone (the per-station harvest at long horizons)

**STATUS**

- **id:** SCL2
- **topic:** network-vehicle
- **status:** open
- **summary:** PRE-REGISTERED 2026-10-06 (card SCL's owed level residue). With card SCL's clock clone (B1) at a broadcast-grade anchor q, the satellite clock level stops converging near 4 cm on the sim. Fresh evidence (section 3): the residue is the between-satellite (de-commoned) level, 0.042 to 0.048 m in hours 2 to 6 on the COD bed and on the broadcast bed alike and at every q from 1e-6 to 2e-5 m2/s. It forms in hours 1 and 2 as a per-satellite pattern and then holds: code averaging continues at about 8 % of the 1/n rate. The producer's stated level keeps shrinking at the code-only rate, so the level NEES grows from 1.2 to 12 over 6 h. The pivot-frame figures card SCL graded on the broadcast bed also carry the pinned pivot's own broadcast walk, through card CLK-R's frame hold. The unit network at production scale reproduces the residue, q-flat and independent of the truth. B1's per-station harvest (mode 5) plateaus after 120 to 240 epochs, while the exact recursion (mode 4, the batch) keeps converging: at 24 stations, 21 mm against 5.7 mm at epoch 2400. The term card SCL named is the mechanism, but its size compounds with the horizon. Build (section 5, default off, one of two forms chosen by gate U2): X2, the clone's shared conditional carried as a bounded set of history columns, or X1, the exact joint carry of every carried station-local column on the dense border route. Gates in section 6.
- **source:** [SCL](SCL_self_contained_clock_level_2026-10-03.md) sections 3.7, 4.1, 5, 9 and 10; [CTU](CTU_coupling_time_update_2026-09-28.md) section 4; [CTU2](CTU2_level_recursion_followup_2026-09-29.md) section 4 (the level instrument); [AIL](AIL_ambiguity_information_loss_2026-10-01.md) (the joint carry as a design question); [WSX](WSX_within_station_cross_terms_2026-09-30.md); [PRM](PRM_parameter_registry_2026-09-29.md) section 4.2 (the route field); [SFR](SFR_sim_fixture_coloured_phase_2026-10-01.md); card CLK-R (the emitted clock's frame hold)
- **knobs / posture:** proposed, default off, one of: X1 `network.carry_station_locals_dense` (the dense border route for the carried station-local kinds) or X2 `network.carry_coupling_shared_history` with `network.carry_coupling_shared_history_rank` (requires B1). The arms use card SCL's knobs as it left them (`network.carry_coupling_clock_clone`, `network.handover_stransform_dense`, `network.clock_anchor_datum_keep`, the smoothed hold, `network.clock_anchor_q` 5e-6, `files.internal_data_root` for the bed). Nothing armed; no golden, ledger or posture change
- **dates in the text:** 2026-10-05, 2026-10-06
- **hashes / run dirs:** exe `gnss_processor_lres.exe` (a copy of main `5b5cb45`'s `gnss_processor_scl.exe`, sha256[:16] `8a1aa9255f3321d8`); arms `runs/_rap_scl2d_*` (16; `d1` byte-identical to card SCL's `runs/_rap_scl16_on6_s`); data roots `runs/_scl2_data_rw` (walk file sha256[:16] `39381c6f62a87a33`), `runs/_scl2_data_rww` (`7fc563fa6368d9d2`), `runs/_scl2_data_rws` (`c7ad1cd64a67f0f8`); card SCL's arms and data roots as cited; unit-network probe `tests/test_scl_unit_probe.cpp` in the session worktree at `5b5cb45` (uncommitted, its patch in the session scratchpad)

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
