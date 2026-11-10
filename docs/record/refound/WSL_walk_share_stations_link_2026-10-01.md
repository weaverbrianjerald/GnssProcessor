# WSL: the field's larger walk share against the stations per satellite

**STATUS**

- **id:** WSL
- **topic:** network-vehicle
- **status:** closed
- **summary:** FALSIFIED 2026-10-01: the field's larger walk share is not card WSX's carry defect seen through the stations per satellite; at a matched station count it is still 1.50 times the sim's.
- **source:** [WSX](WSX_within_station_cross_terms_2026-09-30.md) sections 6 to 8; [SMC](SMC_smoothed_hold_common_step_removed_2026-10-01.md) sections 4 and 5; [FXA](FXA_field_excess_phase_visible_motion_2026-09-30.md); [SMD](SMD_smoothed_hold_block_member_2026-09-30.md) section 8; [PVN](PVN_phase_visible_nees_2026-09-29.md) (the stated contrast variance with every cross term); [REV](REV_revision_member_replay_2026-09-30.md) section 6 (the trace columns); the outside review's note of 2026-10-01
- **knobs / posture:** none; the traces of `runs/_rap_rev1_blk6_{f,s}` (card REV's instrument build, card SMD's exact-update posture, 6 h) and the unit network of `tests/test_coupling_exact_update.cpp`; nothing armed
- **dates in the text:** 2026-10-01
- **hashes / run dirs:** traces of `runs/_rap_rev1_blk6_{f,s}`; grader `scripts/clock_level/wsl_link.py`; the unit network at 12 and 15 stations (by hand, `tests/test_coupling_exact_update.cpp` restored to 3)

**Outcome.** The card asked whether card [WSX](WSX_within_station_cross_terms_2026-09-30.md)'s carry over-statement, which grows with the stations observing a satellite, explains the field's larger walk share. The field's satellites are seen by fewer stations (median 12 against 15), the stated contrast does not tighten with the count (pooled slopes +0.34 and +0.41, bar -0.5), and the unit network predicts the opposite direction (0.91): L1, L3 and L4 FAIL, L2 is not shown. The augmented previous border stays owed for consistency, off the critical path; nothing is armed. Card [FWS](FWS_field_walk_share_sources_2026-10-01.md) then traced the larger walk share to persistent steps: the stations' time-correlated phase errors and the fractional wind-up staircase.

## 1. The quantities

Per block clone, from its L2W key (one per satellite): the realized walk share |Phi_D| at clone
age 1 h (the row nearest 3600 s within 60 s); the structural walk share f = W / (V0 + W), with V0
the stated variance of D at birth (`b_vd0b_m2`) and W the walk the fold added along D up to that row
(`b_wd_m2`); and n, the mean over the clone's rows up to that age of the number of stations carrying
an ambiguity of the satellite on L2W at that epoch (`network_amb_state.csv`). Per satellite and
epoch, card PVN's stated contrast standard deviation (`sqrt_sp_m` of `network_phase_visible.csv`, the
constellation contrast with every cross term) against the same epoch's station count.

The rival explanation is the clone's age (W grows with it). Holding the age fixed at 1 h removes it
from the clone comparison; the per-epoch contrast read holds the run hour fixed (hours 2 to 6) and
compares across station counts within an hour.

## 2. Predictions (written before the read)

- **L1 (the stated contrast tightens with the stations).** On both beds, within each run hour from
  the second on, the stated contrast sd falls with the station count: the slope of log sd on log n
  below -0.5 (faster than independent stations would give).
- **L2 (the walk share follows the stations).** On both beds, the realized walk share at clone age
  1 h rises with n: the slope of log |Phi_D| on log n above 0 at the clone level, and the structural
  share f rises with n likewise.
- **L3 (the beds differ through the stations).** The field's station counts per satellite are shifted
  above the sim's (median n higher on the field), and at matched n (bins of n holding clones on both
  beds) the two beds' walk shares agree within a factor 1.5, where unmatched they differ by the
  measured 2.8.
- **L4 (the unit network predicts the size).** Card WSX's unit network at the beds' median station
  counts gives a ratio of the over-statements (sim's smallest generalized eigenvalue over the
  field's) within a factor 1.5 of the measured walk-share ratio 2.8.

**The decision rule.** L2, L3 and L4 hold: the field's extra walk share is card WSX's carry defect
seen through the hold, the consumer's loss from hour 3 is its manifestation, and the augmented
previous border is the critical path, its gate the truth-side contrast NEES on the sim (owed, a
grader over card PVN's stated contrast variance with the sim's datum) and the walk-share ratio it
should remove. L3 fails (the field's satellites are not seen by more stations, or matching n leaves
the beds apart): the field's extra walk share is something at the field's stations themselves, the
augmented border stays owed for the estimator's consistency and moves behind that investigation.

## 3. Order

The grader (`scripts/clock_level/wsl_link.py`), the read on both beds, the unit network at the two
profiles, the verdicts. No run, no build, nothing armed.

## 4. The read (2026-10-01)

`scripts/clock_level/wsl_link.py runs/_rap_rev1_blk6_f runs/_rap_rev1_blk6_s` (station counts from
`network_amb_state.csv`, L2W; the stated contrast from `network_phase_visible.csv`); the unit network
of card WSX at 12 and 15 stations (3 satellites, `runRecursion` mode 3).

| | field | sim |
| --- | --- | --- |
| stations carrying the satellite on L2W, p10 / p50 / p90 per satellite-epoch | 7 / 12 / 25 | 13 / 15 / 16 |
| L1: slope of log stated contrast sd on log n, run hours 2 to 6 | -0.01, +0.03, +0.52, +0.78, +0.48 (pooled +0.34 +- 0.01) | -0.65, +0.20, -0.12, +1.09, +1.45 (pooled +0.41 +- 0.05) |
| clones at age 1 h; their n-bar p10 / p50 / p90 | 71; 7.3 / 11.4 / 25.1 | 72; 13.4 / 14.5 / 15.4 |
| L2: slope of log \|Phi_D\| on log n-bar; of log f | +0.73 +- 0.44; -0.74 +- 0.90 | -0.74 +- 3.38; +3.79 +- 7.09 (n-bar too narrow) |
| median \|Phi_D\| at age 1 h | 5.2 mm | 3.3 mm |

L3: the unmatched ratio of the median walk share at age 1 h is 1.55 (the clone-level counterpart of
card SMC's late spread ratio 2.8); in the one bin that holds both beds (n-bar 12 to 16) it is 1.50
(17 field clones, 71 sim). L4: the unit network's smallest generalized eigenvalue is 0.744 at 12
stations and 0.679 at 15, so it predicts the field's walk share at 0.91 of the sim's.

**L1 FAILS** (the stated contrast does not tighten with the station count on either bed; the slopes
are near zero or positive). **L2 is not shown** (+0.73 +- 0.44 on the field, the structural share
flat, the sim's range too narrow to test). **L3 FAILS** on both clauses: the field's satellites are
not seen by more stations (median 12 against 15; the field's spread is wider), and matching the
station count leaves the beds 1.50 apart. **L4 FAILS**: the unit network predicts the opposite
direction.

## 5. Reading and decision (2026-10-01)

The field's larger walk share is not card WSX's carry defect seen through the stations per
satellite. The clustered roster gives the field's satellites a wider spread of station counts but a
lower median than the sim's evenly spread network, the stated contrast does not tighten with the
count, and the beds stay 1.5 apart at a matched count. The unit network's dependence on the station
count stands (card WSX section 6), but on these beds it would make the sim's contrasts the more
over-stated.

**The decision (the pre-registered rule, the L3 branch).** The field's extra walk share is something
at the field's stations themselves, not the carry's station-count arithmetic. The augmented previous
border stays owed for the estimator's consistency (card WSX's over-statement is real in the unit
network) and moves behind that investigation; it is not the critical path for the consumer's loss
from hour 3. Card PAC found one field-specific contribution to the phase-visible frame (the
unhealthy pivot's slow level), which is common-mode and does not reach the walk share; what makes the
field's per-satellite walk share larger remains open. Nothing is armed.
