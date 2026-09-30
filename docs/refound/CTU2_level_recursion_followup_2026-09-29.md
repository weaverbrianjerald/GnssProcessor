# CTU2: the level recursion's follow-up (the fixture artifact, the rx-CB double count, the level instrument, the phase-space guard, the seam)

**STATUS**

- **id:** CTU2
- **topic:** network-vehicle
- **status:** open
- **summary:** 2026-09-29, the implementing session's follow-up of card CTU section 13.5, nothing armed. The sim ignition damage is a FIXTURE artifact (the 24 h sim fixture's first epochs carry the simulator's orbit extrapolation; from 00:30:15 the control converges to 0.14 m and freezes, section 1). The diagonal receiver code-bias carry folds a conditional variance as an absolute prior and doubles the border information every epoch; `network.carry_rx_code_bias_in_block` folds it inside the station block instead (section 2). The dense fold's conditioning bound is a knob and `rank_gate_tol` 1e-11 separates weak directions from the gauge (section 3). The monitor carries a truth-free level instrument (split-half NEES; section 4). The anchor guard has a level- and ionosphere-blind phase statistic (section 5). The field's first-fold jump was a float-vehicle drop that did not decouple an arc at birth strength (`network.amb_float_drop_decouple_always`, section 6). Clean-start sim with the exact update: G1 and G2 PASS (0.03 m at 2 h, NIS 1.07); field G4 0.164 m against 0.15, G1 trips (4 against 2, with a one-phase-bias-per-band functional) and G8 cost FAIL; G5 FAIL (a switch-on from a control snapshot keeps the level, held by the restored ambiguities, section 8); the consumer is worse with the exact update under every hold setting, so the hold needs a level-aware form before G6 and G7 (section 9). 6 h arms (section 10): a pivot handover orphaned the deficit of the departed pivot (30 withheld epochs); the deficit is now spread over the present clocks (0 withheld); sim IF code spread 0.04 to 0.05 m every hour (control 0.11 to 0.17), field 0.14 to 0.24 m (control 0.19 to 0.33) outside one satellite; sim consumer with the hold ends below the control (0.036 against 0.068 m in hour 6) after three bad hours, field consumer 14 m in hour 1.
- **source:** [CTU](CTU_coupling_time_update_2026-09-28.md) sections 9, 10, 13.4 and 13.5; [CLV](CLV_clock_level_2026-09-22.md) sections 1 and 3; [RF-1 design](RF-1_float_ambiguities_design.md) sections 1.3 to 1.5; `docs/SPEC_rx_cb_carry.md`; the Q3 design (`docs/cards/clock/Q3-ANCHOR-DESIGN.md`)
- **knobs / posture:** all default off and byte-identical off: `network.carry_rx_code_bias_in_block`, `network.dense_fold_kappa` (default 1e-6), `network.clock_level_innov_window_ep` (instrument window of `network.clock_level_monitor`), `network.clock_anchor_guard_space` (`clock` | `phase_shadow` | `phase`) with `network.clock_anchor_phase_sigma_floor_m`, `network.amb_float_drop_decouple_always`, `network.clock_level_restore_raise` with `network.clock_level_restore_credit_ep`; the arms also use the existing `network.rank_gate_tol` 1e-11 and `network.start_offset_hours` 0.504167; no golden, ledger or posture change
- **dates in the text:** 2026-09-28, 2026-09-29
- **hashes / run dirs:** final exe `gnss_processor_ctu15.exe` sha256 prefix `83d515d49fa884da` (`_ctu14` `3066c89a23110c54`, `_ctu13` `aa5ebde2834ceb8e`; graded arms also on `_ctu8` `9534061c1420a82c`, `_ctu10` `641f766b5bbff093`, `_ctu12` `8194c84890edcaa9`); ignition arms `runs/_rap_ig_*`; clean-start arms `runs/_rap_i3_*`, `_rap_i4_*`, `_rap_i5_*`; field and numerical arms `runs/_rap_z_*`, `_rap_z6_*`, `_rap_z7_*`, `_rap_z8_*`; identity `runs/_rap_z13_*`; one-per-band guard `runs/_rap_i6_*`; 6 h `runs/_rap_s6_*`, `runs/_rap_s7_*`; G5 seed `runs/evseed_ctu_g5d`, legs `runs/evleg_ctu_g5d_{ctl,ex,exnr,exq,exq1,exq10}`; session scratchpad graders `ctugrade.py`, `ign.py`, `ifwin.py`, `iflevel_code2.py`, `guardladder.py`, `wf/lvl/lvl3.py`, and the read-only analyses under `wf/`

Card CTU section 13.5 items 1 to 4 in that order: the ignition damage, the anchor guard and the level
direction, a truth-free level instrument, then G2, G4 and G5 again. Arms are 2 h (15 or 30 min for
the ignition reads) from `config/golden/golden_{sim,field}_refound.json` with `scripts/rapid_arm.py`
(G5 with `scripts/event_leg.py`). Exes `gnss_processor_ctu5.exe` to `_ctu13.exe`; the final one is
`_ctu13` (sha256 prefix `aa5ebde2834ceb8e`); the arms graded below ran on `_ctu8` (`9534061c1420a82c`),
`_ctu10` (`641f766b5bbff093`), `_ctu12` (`8194c84890edcaa9`) and `_ctu13`, and every later exe only
adds default-off code to the earlier ones. Session graders: `ctugrade.py` (one-screen grade),
`ign.py` (per-epoch level), `ifwin.py` (windowed IF code level), `iflevel_code2.py` (the field
against CODE), `guardladder.py` (the guard's phase ladder), `wf/lvl/lvl3.py` (the level instrument
offline).

## 1. The sim bed's ignition damage is a fixture artifact

The 24 h sim fixture (`runs/simnet60_quiet_wu93_24h`) starts at 00:00:30, and its simulator
extrapolated the satellite positions outside its SP3 window for the first epochs: each station's
`logs/geometry.log` carries about 1 600 warnings "Position extrapolation ... outside SP3 window
[259500.0, 262500.0]" over transmit times 259229.9 to 259349.9 s (270 to 150 s outside). The first
epochs' observations are inconsistent with the producer's orbits: the post-fit code residuals over
their weights read 2.99 (C1C) and 1.82 (C2W) at epoch 1 and fall to 1.0 by epoch 8, the station
ZWD spread is 2.96 m at epoch 1, and the phase-anchor stations' ZWD runs to -24 to -48 m by epochs 7
to 9. The same posture on the 12:00 fixtures (`_camp60wu_quiet12h_r2q3` against
`_camp60wu_quiet24h_r2q3`) converges from epoch 1. The decisive arm starts the golden at 00:30:15
(`network.start_offset_hours` 0.504167, inside the window, with the half-epoch back-off):

| arm (sim) | datum-free level, epochs 1 to 8 (m) | at 16 / 60 / 120 / 240 (m) | ZWD spread, epoch 1 / 9 (m) |
| --- | --- | --- | --- |
| golden from 00:00:30 (`_rap_ig_base`, `_rap_y_g0_s`) | 0.69 0.69 1.00 1.30 1.50 1.30 1.19 1.18 | 1.17 / 1.14 / 1.14 / 1.15 | 2.96 / 4.36 |
| golden from 00:30:15 (`_rap_i3_g0_s`) | 0.42 0.32 0.25 0.22 0.20 0.20 0.17 0.15 | 0.14 / 0.14 / 0.14 / 0.14 | 1.14 / 0.54 |

The control's freeze (card 1d) remains, at the level the first epochs reach (0.14 m). The G2 bars
at epochs 16 and 60 of CTU 13.3 and the reading CTU 13.4 (e) measured the artifact. The permanent fix is a
fixture regenerated with the previous day's orbit and clock files visible to the simulator (the
sim baselines then move); until then sim level grades start inside the window. The field has no
such start: its control's level innovation (4) is 0.2 to 0.4 m per epoch from epoch 1.

## 2. The receiver code-bias carry counted the border twice

One inconsistency of the first epochs is independent of the artifact. The diagonal receiver
code-bias carry (`network.carry_rx_code_bias`, armed on both goldens) folds each station's code-bias
column as an absolute prior at its pure conditional variance (N_rr^-1)_cc, the variance given the
shared border, so every epoch it states the border along the column's regression at that precision
on top of the dense border carry that already holds the same information. On the sim (15 min arms
`_rap_ig_*`), the monitor's projection of the border block along each satellite's level direction
reads 188, 63, 27, 13, 6.3 m^2 over epochs 1 to 5 with the carry (information 1, 3, 7, 15, 30, that
is I_k = 2 I_(k-1) + 1) and 188, 94, 63, 47, 38 m^2 without it (188/k); the weak eigenvalues of the
reduced shared normals double every epoch with it (1.87e-4, 3.73e-4, 7.39e-4, 1.45e-3, 2.81e-3) and
stay at the one-epoch value without it. The same fold re-applies the receiver DCB datum (sigma 10 m)
on the carried column every epoch, so the datum accumulates (10/sqrt(k) m on a column the data do
not reach).

Switching the carry off is not a fix: it disarms the second code channel (card SCC), whose receiver
inter-signal columns ride that carry, and on the field the code side against CODE then worsens
(C1C user 0.29 to 0.47 m rms across satellites in hour 2, `_rap_z_nrx_f`). The consistent fix is
`network.carry_rx_code_bias_in_block` (RXCB-IN-BLOCK, default off, fingerprinted): the receiver
code-bias columns are harvested with their regression on the border and their cross covariances to
the station's other carried columns, and folded as rows of the station's jointly whitened local
block (the receiver-clock and ZWD in-block route). There is no diagonal fallback (before the
leveling carry is warm, and on a non-SPD block, the columns carry nothing, counted); the event
response is a frame-invariant step between two harvested absolutes (the diagonal route's inject
screen |value_abs - x_lin| withheld 4 of 60 sim stations for the whole run, because x_lin is a run
constant); a warm resume's restored diagonal entries stay out of the block until re-harvested.
Measured: the level projection returns to 188/k (`_rap_ig_rxb_mon`); 22 832 columns fold in-block
over 2 h on the field with 0 failures and 419 missing cross terms out of the block pairs; the field's
code side against CODE is kept (C1C 0.33 and C1W 0.33 m in hour 2, against the control's 0.29 and
0.39).

## 3. Two numerical guards that assumed an inconsistent level

With the level's variance honest (metres early, shrinking with the code) and the phase-visible
combinations phase-precise, two guards built around the old carry fire:

- The dense fold's conditioning test (`NetworkEstimator::denseFoldTerms_`: the correlation-scaled
  block is rejected when its smallest LDLT pivot is below 1e-6 of the largest) rejects the honest
  block at the anchor's engagement epoch, where a clock and its phase biases correlate to within
  about 1e-8 of one, and the fold falls back to the diagonal carry for that epoch (1 or 2 per run).
  The bound is now a knob, `network.dense_fold_kappa` (default 1e-6, byte-identical). On the
  00:00:30 start, admitting the block (1e-12) made the level worse (0.62 m at epoch 60 against 0.09):
  the one-epoch diagonal fallback had been releasing the artifact's level. The default is kept.
- The rank gate's relative cutoff (`rank_gate_tol` x lambda_max, tol 1e-9) rises with the
  phase-precise information, and weak but real directions (L2W phase-bias common modes over a phase
  anchor's group, sigma of tens of metres in the first quarter hour) fall under it: 67 to 314
  S-basis completions per 2 h, and on the field with the exact update 11 to 13 "null" directions at
  epochs 13 to 16, above the completion cap of 6, so 4 epochs withheld. `network.rank_gate_tol`
  1e-11 (an existing knob) separates them: the declared clock gauge sits at 1e-11 to 1e-10 absolute
  (numerical zero at lambda_max 1e5 to 3e6), the weak directions at 1e-4 to 1e-3. With it, 0
  completions and 0 withheld epochs on both beds; the sim level converges slightly slower in the
  first minutes (0.10 against 0.09 m at epoch 16 on the clean start) and reaches the same 0.03 m.

## 4. The level innovation instrument (CTU 13.5 item 3)

Built into the monitor (`network.clock_level_monitor`, trace class; window
`network.clock_level_innov_window_ep`, default 60). The ionosphere-free code sees a satellite's level
through its clock alone (the code bias's IF combination is the datum) and the slant ionosphere
cannot take any of it (A(1 - k') - B(1 - g k') = 1 because A - B g = 0). Per epoch, the post-fit
two-way effect of the IF code pairs (the re-base's pair build and fit, which now also returns the
effects' unit-weight contrast covariance and s0^2) minus the constellation contrast of this epoch's
clock update is the prefit innovation of the level, nu_s = (z_s - mean z) - (dc_s - mean dc); its
predicted variance is S = u'P-u + s0^2 Q (the previous stash's clock block along the contrast plus
the walk, and the fit's variance), and NIS = nu^2/S. Per window, the mean innovation per satellite,
T = nubar^2 / var(nubar), and the truth-free level NEES from two independent station halves (even
and odd index), NEES_tf = sum_s nubar_A nubar_B / sum_s Pbar_s, unbiased for the error's square
whatever the code noise. Traces `logs/network_level_innov.csv` and
`logs/network_level_innov_win.csv`, and a run-end line. Offline on card CTU's section 13 arms the same
statistic recovers the truth-graded error (correlation 0.96 to 1.00, slope 0.90 to 1.01;
`wf/lvl/lvl3.py`).

| arm | NIS mean after epoch 15 | window NEES_tf | rms mean innovation per window (m) | truth (sim) |
| --- | --- | --- | --- | --- |
| sim control, 00:30:15 (`_rap_i3_g0_s`) | 1.20 | 1.4 / 3 617 / 3 695 / 2 901 | 0.14 / 0.15 / 0.17 / 0.16 | 0.14 |
| sim exact + 2 + 3 + 5, 00:30:15 (`_rap_i5_ex_s`) | 1.07 | -0.05 / 2.2 / 2.7 / -2.5 | 0.06 / 0.08 / 0.09 / 0.06 | 0.10 to 0.03 |
| sim control, 00:00:30 (`_rap_z7_mon_s`) | 8.29 | 81 / 2.1e5 / 1.8e5 / 1.8e5 | 0.96 / 1.06 / 1.06 / 1.06 | 1.08 |
| field control (`_rap_z7_mon_f`) | 2.99 | 0.05 / 4 602 / 954 / 11 965 | 0.18 / 0.25 / 0.28 / 0.30 | |
| field exact + 2 + 3 + 5 + 7 (`_rap_i5_ex_f`) | 2.80 | 6.5 / 5.2 / -3.7 / 1.8 | 0.50 / 0.16 / 0.15 / 0.16 | |

This corrects the reading of CTU 13.4 (f) on one point. The ratio P_L / (1/I_L) fails mainly because
P_L is a pivot-frame projection over the clock and every phase bias (dominated by the phase biases'
variance early on) compared with a single satellite's IF information, and because a variance ratio
cannot see a wrong mean. The "more than IF information" argument holds only where the ambiguities
are mature (8 measures about 15 times the IF information per epoch through restored mature
ambiguities; a cold start is IF-efficient). The field's split-half noise ratio is 12 to 51 against
0.6 to 2 on the sim: the field's code errors are not white per station (receiver-group code-bias
differences), so its T and NIS overstate, while NEES_tf, a cross product, is not affected.

## 5. The anchor guard in the phase-visible complement (CTU 13.5 item 2)

`network.clock_anchor_guard_space` = `clock` (default, byte-identical) | `phase_shadow` | `phase`,
with `network.clock_anchor_phase_sigma_floor_m` (default 0.02). The statistic is the
ionosphere-free phase clock d = c - sum_j w_j p_j with sum w = 1 (blind to the level) and
sum w lambda_j^2 = 0 (blind to the satellite's ionosphere direction): GPS L1/L2 w = 2.5457 and
-1.5457; a shared band splits its weight evenly (L1C, L1W, L2W: 1.2729, 1.2729, -1.5457). The
innovation is taken against the folded prior means of the dense capture, the variance is P- - P+
of the contrast (the folded block and the posterior along the functional, projected on the
constellation contrast) floored, and the constellation median is removed. The absolute backstop
stays on the clock. `phase_shadow` computes, ladders and traces the statistic
(`logs/network_anchor_guard.csv`) while the clock statistic decides; the solve is byte-identical
(`_rap_z8_csh_{s,f}`).

Shadow ladder, anchored rows from epoch 30 (`guardladder.py`): on the clean-start sim with the exact
update |e_phi| is 2.2 mm p50, 11 mm p90, 40 mm p99, and no row passes k = 6 at the 2 cm floor, while
the clock statistic trips 4 times, all level motion (G20 converging after its readmission at 1 to
2 cm per epoch against the 1.8 mm floor). In phase mode: 0 trips on the clean-start sim
(`_rap_i5_ex_s`), level and code spread unchanged. On the field (`_rap_i5_ex_f`) 1 absolute trip and
10 phase trips: 9 of them at TOW 260040, where the IF phase clocks of G07, G08, G09, G13, G14, G15,
G18, G22 and G30 step by 0.48 to 1.54 m while their clocks do not move, at a cold phase-anchor
handover (`STRANSFORM sts_cold_declared`) beside an anchor station's gap re-entry (MAJU00MHL): a
phase-bias frame event the statistic sees correctly and the response (a clock decouple) does not fit.
The anchors of those satellites did not change at that epoch; the satellites carrying a third phase
bias (L1W, anchored in the sparse L1W family) had it in the minimum-norm functional. The functional now
keeps one phase bias per band, the best-determined one (smallest folded variance), which is equally
blind to the level and the ionosphere: on the field (`_rap_i6_ex_f`, exe `_ctu14`) the trips fall from
11 to 4 (1 absolute at epoch 9, 3 phase trips at 260040 where G13, G15 and G18's phase-visible clocks
step 2.1 m with their clocks steady), the sim is unchanged (one signal per band already). Next for the
guard: a phase-visible step with a steady clock is a phase-bias event, and its response should be the
phase bias's (a declared step), not a clock decouple.

## 6. The field's first-fold jump: a drop that did not decouple

With the exact update the field's level jumped by metres at the first ambiguity fold (epoch 8, TOW
259410: G09 +4.25, G14 -6.20, G22 -3.49, G30 -4.56, G13 -2.66 m, the satellites of BSHM00ISR), and
G07 ran away to -15 m. Cause: BSHM00ISR's G07 L2W arc was born at 259380 and slipped at 259410 (the
IF-slip detector sees 60 m, the free-epoch latch fires); the float vehicle's drop
(`ambFloatDrop`, `NetworkAmbFloat.cpp`) restores max(var_birth, var_first, floors) and, only when the
variance moves, clears the arc's border regression and block row. An arc that slips at age 1 is
already at birth strength, so the drop returned early and the arc kept folding its pre-slip value
with full cross terms and a full regression. The exact update keeps the station's ionosphere-free
ties phase-tight, so the stale value went into the satellite clocks; card 1d's clock inflation had
loosened those ties to decimetres and hidden the missing response. The prior-residual census
labelled the arc RESPONDED with folded=0 and did not see it. Fix: `network.amb_float_drop_decouple_always`
(default off, fingerprinted): the drop clears the regression and block row on every call (12 drops
per 2 h on the field, 0 on the clean-start sim). With it the epoch-8 jump and the runaway are gone,
the field's IF code spread in hour 2 is 0.167 m (control 0.27), the code side against CODE is C1C
0.38 / 0.20 m and C1W 0.33 / 0.31 m in hours 1 / 2 (control 0.29 / 0.29 and 0.36 / 0.39), and the
level instrument reads NEES_tf 6.5 / 5.2 / -3.7 / 1.8 per window.

## 7. The gates again

Candidate posture: the golden plus `carry_coupling_exact_update`, `carry_rx_code_bias_in_block`,
`rank_gate_tol` 1e-11, `clock_anchor_guard_space` `phase`, `amb_float_drop_decouple_always` and the
monitor; the sim from 00:30:15.

| gate | arm | reading | verdict |
| --- | --- | --- | --- |
| U0 | the full Debug suite | 2 690 tests in 342 suites: 2 689 passed, 1 fixture-gated skip (`FixtureSmokeTest.Sp3HeaderTimeConversionMatches`); `CtuLevelGuardTest` 9 of 9, `CouplingExactUpdateTest` 8 of 8; `docs_index_check.py` and `make_golden_config.py --verify` pass | PASS |
| G0 | knob-off and trace-only arms on every exe that touched a shared path (`_rap_z_g0_s`, `_rap_z8_g0_{s,f}`, `_rap_z8_csh_{s,f}`, `_rap_i5_g0_s`, `_rap_z13_*`) | products, closed-loop trace and epoch status identical to `_rap_y_g0_{s,f}` | PASS |
| G1 | sim `_rap_i5_ex_s` (control `_rap_i3_g0_s`) | floor folds 0 % (63.5); trips 0 (0); re-floats 0 (0); completions 0; every kept epoch exact | PASS |
| G1 | field `_rap_i6_ex_f` (control `_rap_z8_g0_f`) | floor folds 0 % (44.3); trips 4 (1): 1 absolute at epoch 9, 3 at one phase-bias event (5); re-floats 9 epochs / 27 ambiguities (16 / 28); completions 0 (19) | FAIL on the trips (4 against a bar of 2) |
| G2 | sim `_rap_i5_ex_s` | level 0.10 / 0.07 / 0.03 m at epochs 16 / 60 / 240 (bars 0.25 / 0.15 / 0.08; control 0.14 frozen); IF code spread hour 2 0.052 m (bar 0.10; control 0.14); NIS 1.07, window NEES_tf about 2 to 3 | PASS (clean start) |
| G3 | the ratio | superseded by the instrument, 4 | not a gate |
| G4 | field `_rap_i6_ex_f` | IF code spread hour 2 0.164 m (bar 0.15; control 0.27; CLV re-base 0.17); anchor as G1 | FAIL by 1.4 cm |
| G5 | 8 | seam mechanics in place; the level after a switch-on from a control snapshot stays near the control's (0.14 to 0.11 m over the hour) | FAIL |
| G6 / G7 | 9 | | not run as gates |
| G8 | field | mean epoch time 583 ms against 481 (+21 %; the monitor's three two-way fits, the phase statistic and the larger blocks); sim wall +4.5 % | FAIL on the field |

## 8. G5, the switch-on seam, and U0

G5 needed three pieces the seam did not have (all default off): the switch-on direction is admitted
by a fingerprint that strips the card's own tokens (a snapshot written without them resumes into a run
that arms them, never the reverse); the exact update's stash stamp and pivot map are rebuilt from the
restored dense border; the monitor's contrast-frame level information travels in the snapshot (`LB`
rows) and `network.clock_level_restore_raise` turns it into this epoch's process noise along each
satellite's level direction (V u u' over its clock and phase-bias rows, in the dense fold and in the
coupling rows' exact update, not a raise of the stashed block), with
`network.clock_level_restore_credit_ep` setting how many epochs of code information the inherited level
is credited with. Seed `runs/evseed_ctu_g5d` (the golden from 00:30:15 with the monitor, snapshot at
epoch 122), legs of 118 epochs:

| leg | level after the seam, leg epochs 1 / 11 / 31 / 61 / 120 (m) | window NEES_tf |
| --- | --- | --- |
| control (`evleg_ctu_g5d_ctl`) | 0.139 / 0.139 / 0.138 / 0.138 / 0.165 | |
| exact + fixes, no level noise (`_exnr`) | 0.139 / 0.139 / 0.138 / 0.138 / 0.139 | 1 947 / 1 430 |
| exact + fixes, the stashed block raised (`_ex`, an intermediate build) | 0.141 / 0.141 / 0.138 / 0.136 / 0.137 | 98 / 179 |
| exact + fixes, level noise, full credit (`_exq`) | 0.140 / 0.139 / 0.132 / 0.127 / 0.120 | 38 / 55 |
| the same, credit 1 epoch (`_exq1`, V = 0.15 m^2 per satellite) | 0.162 / 0.140 / 0.130 / 0.123 / 0.113 | |
| cold start, same posture, epochs 122 to 240 (`_rap_i3_ex_s`) | 0.045 to 0.031 | |

The level stays near the control's. The instrument shows why: the first leg epoch after a level
noise of 0.15 m^2 hands the level about 90 m^-2 of information, about 15 times what the IF code
supplies, and the level barely moves. The extra information comes through the restored carried
ambiguities and the slant ionosphere they tie: in the control lineage they absorbed the frozen level
error (71 percent of it per line of sight, card CLV section 1), so the single-frequency codes read
through them agree with the frozen level. A switch-on seam therefore also needs an ambiguity-side
response (the carried ambiguities loosened along the direction that couples them to the level, or
re-floated), which is not built.

U0: the suite with the new file `tests/test_ctu_level_guard.cpp` (suite `CtuLevelGuardTest`, 9
cases: the phase-clock weights, their level and ionosphere blindness, the two-way fit's contrast
covariance against a brute-force inverse, s0^2, the seam level noise leaving a level-blind row
unchanged and releasing a level-stating row, the drop's decouple at birth strength).
`tests/test_block_baarda.cpp` now compiles with `/bigobj` (the new header members pushed it over the
Debug section limit).

## 9. Disposition

Nothing armed; no golden, ledger or posture change. The sim result is the one card CTU section 2 predicted
once the fixture artifact is set aside: with the exact update, the receiver code-bias carry in the
block and the rank tolerance, the level converges at the code's rate (0.03 m at 2 h against the
control's frozen 0.14 m) and reads consistent on the truth-free instrument (NIS 1.07). On the field,
the same posture plus the drop fix brings the level within 1.7 cm of the G4 bar and near consistency
(NEES_tf 2 to 7). What stands between it and arming, in order:

1. The consumer. Cold-consumer p50 by hour, not graded: sim control 0.97 / 0.16 / 0.11, exact with
   the hold 1.49 / 0.78 / 0.57, exact with the hold off 0.87 / 0.38 / 0.44, control with the hold off
   0.68 / 0.40 / 0.51 m; field control 0.56 / 0.06, exact with the hold 0 / 3.5, with the hold off
   10.7 / 1.6 m. The hold freezes the phase bias while the level moves the clock and phase bias
   together, and without the hold the phase-bias wander (CTU 13.4 (h)) is on the wire. The hold needs a
   level-aware form (pass the level motion, hold the wander) before G6 and G7. The separation is the
   design question. Transferring each epoch's clock change to the held phase biases (the level motion
   read as Delta c) freezes the wire's phase-visible clock, which on this bed is noise (the sim's a
   priori clock is the truth) but on the field drops the network's real phase-visible corrections to
   the a priori clocks (the anchor walk, about 1 mm per epoch). Transferring the IF combination of the
   phase-bias change (sum_j w_j Delta p_j, the change of c minus that of the IF phase clock) passes the
   level exactly and holds the GF wander, but passes the IF wander too, and the guard's shadow ladder
   shows the IF phase clock steps 2 to 13 mm per epoch (card CTU 13.4 (h)). The separation that follows
   the model is the level coordinate of each epoch's update in the information metric (the component
   along u_s orthogonal, in P+^-1, to the phase-visible directions), which moves only with the code.
2. The guard's phase statistic at phase-bias frame events (5).
3. The ambiguity-side seam response (8), then G5 again.
4. The fixture regenerated (1), then the sim baselines and the G2 bars from epoch 1.
5. G8: the monitor's per-epoch cost on the field.

## 10. The 6 h arms and the pivot-handover orphans (2026-09-29)

Arms of 6 h on the candidate posture of section 7 (the sim from 00:30:15), with and without
`network.closed_loop.emit_pb_hold`, against the golden controls `runs/_rap_s6_ctl_{s,f}` (hold on)
and `runs/_rap_s7_h0_s` (hold off).

**The orphans.** The first 6 h exact arms (`runs/_rap_s6_ex_{s,f}`, `runs/_rap_s6_exh0_{s,f}`, exe
`_ctu13`) withheld 30 epochs after the clock pivot handover at TOW 276150 (G01 to G02, about 4.2 h
in): the rows whose clock-family deficit belonged to the departed pivot had no column to take it,
28 192 row terms were marginalized outside the block ("ORPHAN" on the CTU line), and the product
gate withheld the wire until the stash was rebuilt. The deficit is a gauge quantity: any
distribution over the constellation's present clocks that sums to it leaves the estimable part of
the row unchanged. When the pivot has left, `exactRowIngredients` now spreads the deficit evenly over
the present clocks of that constellation (and counts an orphan only when none is present). Exe
`gnss_processor_ctu15.exe` (sha256 prefix `83d515d49fa884da`; also carries the one-phase-bias-per-band
functional of section 5, which `_ctu14`, `3066c89a23110c54`, introduced): 962 row terms spread on
the sim and 897 on the field, 0 orphans, 720 of 720 epochs emitted on every arm, 0 withheld. The 31
anchor decouples at that epoch (`reason=datum`, followed by `cta_disc_landed`) appear on every arm
including the controls; they are the anchor's designed response to a datum handover, not guard trips.

**The level, truth-free.** Per-satellite median IF(C1C, C2W) post-fit code residual per hour, rms
across satellites after removing the hour's median (`ifwin.py --win 3600`), in metres:

| arm | h1 | h2 | h3 | h4 | h5 | h6 |
| --- | --- | --- | --- | --- | --- | --- |
| sim exact `_rap_s7_ex_s` | 0.044 | 0.052 | 0.043 | 0.044 | 0.054 | 0.037 |
| sim control `_rap_s6_ctl_s` | 0.114 | 0.144 | 0.129 | 0.138 | 0.118 | 0.168 |
| field exact `_rap_s7_ex_f` | 0.240 | 0.165 | 0.210 | 0.136 | 0.170 | 0.333 |
| field control `_rap_s6_ctl_f` | 0.194 | 0.275 | 0.327 | 0.288 | 0.296 | 0.502 |

The field's hour 6 on both arms is one satellite: G01, seen by few stations in that hour (204
IF rows), sits at -1.55 m (exact) and -2.29 m (control); without it the hour reads 0.189 and 0.300 m.
The monitor's instrument agrees. Sim exact: NIS mean 1.090, window NEES_tf from -10.2 to 3.0 over
the eleven windows, split-half noise ratio 1.05 to 1.81; sim control: window NEES_tf 15 to 3 695.
Field exact: NIS mean 2.25, window NEES_tf -9.3 to 24.5 and 71.9 in the last window (G01); field
control: 61 to 15 898. On the field the split-half noise ratio is 11 to 51 on both arms: the two
station halves disagree far beyond their code noise, so the field's IF code carries per-station
structure (multipath, receiver-side code terms not in the model) that no level estimate removes,
and the instrument's floor there is that structure, not the level.

**The consumer.** Cold-consumer 3D p50 by hour (m):

| arm | h1 | h2 | h3 | h4 | h5 | h6 |
| --- | --- | --- | --- | --- | --- | --- |
| sim exact, hold on `_rap_s7_ex_s` | 1.491 | 0.780 | 0.410 | 0.122 | 0.053 | 0.036 |
| sim exact, hold off `_rap_s7_exh0_s` | 0.865 | 0.379 | 0.298 | 0.306 | 0.228 | 0.202 |
| sim control, hold on `_rap_s6_ctl_s` | 0.968 | 0.161 | 0.056 | 0.067 | 0.057 | 0.068 |
| sim control, hold off `_rap_s7_h0_s` | 0.682 | 0.396 | 0.359 | 0.459 | 0.233 | 0.142 |
| field exact, hold on `_rap_s7_ex_f` | 14.001 | 3.448 | 0.897 | 0.345 | 0.272 | 0.132 |
| field exact, hold off `_rap_s7_exh0_f` | 6.369 | 1.311 | 0.515 | 0.337 | 0.356 | 0.376 |
| field control, hold on `_rap_s6_ctl_f` | 0.560 | 0.059 | 0.028 | 0.030 | 0.039 | 0.048 |

With the hold, the sim exact arm's consumer reaches the control's accuracy in hour 5 and ends below it
(0.036 against 0.068 m in hour 6), consistent with a wire whose level has converged; the first three
hours are the hold freezing the level motion into the phase side (section 9, item 1). On the field the
same mechanism costs 14 m in hour 1 and is still 0.13 m in hour 6 against the control's 0.05 m. Without
the hold the phase-bias wander is on the wire on both beds. Section 9's order stands; the level-aware
hold is the item that blocks G6 and G7, and the field is where it is graded.

## 11. The level-aware hold (2026-09-29)

Section 9 item 1 is card [LAH](LAH_level_aware_hold_2026-09-29.md): the phase-clock hold
(`network.emit_pb_hold_phase_clock`), built and gated, not armed. With the exact update its 6 h
census reads 0.59 / 0.17 / 0.12 / 0.08 / 0.10 / 0.08 m by hour against 2.27 / 0.75 / 0.57 / 0.38 /
0.26 / 0.20 under the plain hold and the control's 0.67 / 0.17 / 0.10 / 0.08 / 0.09 / 0.07.

## 12. The fold bound's default (2026-09-29)

Section 3 kept `network.dense_fold_kappa` at 1e-6 after a trial on the 00:00:30 start. Card
[SMH](SMH_smoothed_hold_clone_2026-09-29.md) section 9 re-ran it on the clean start (the exact
update, 2 h, both beds): the datum-free level at epochs 16 / 60 / 120 / 240 0.10 / 0.06 / 0.04 /
0.03 m against 0.10 / 0.07 / 0.05 / 0.03 (sim), the IF code-residual spread in hour 2 0.048
against 0.052 m (sim) and 0.164 against 0.164 m (field), anchor trips 0 against 4 and re-floats 7
/ 23 against 9 / 27 (field), card PVN's NIS 1.04 against 1.03 (sim) and 0.05 against 0.10
(field) and its within-arc NEES 0.4 against 0.3 (sim) and 0.0 against 0.2 (field), and the
engagement epoch's diagonal fallback gone (2 to 0 per bed). On the golden posture the two bounds
are byte-identical (products and consumer, 2 h, both beds). The config default moved to 1e-12
(the loader and `GnssConfig`; the estimator's class default stays 1e-6; 1e-6 in a config
restores the historical behaviour).
