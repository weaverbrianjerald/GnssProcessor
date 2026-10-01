# WSL: the field's larger walk share against the stations per satellite (card WSX's defect seen through the hold)

**STATUS**

- **id:** WSL
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-10-01 (the outside review's note after cards WSX, FXA and SMC). Card SMC found that what card SMD's member passes is per-satellite walk share, with a between-satellite spread of 25 mm on the field against 9 mm on the sim, although the total per-satellite motion of D is the sim's size on the field. Card WSX's unit network shows the shipped carry stating between-satellite phase-visible contrasts too tightly, more so the more stations see a satellite (0.885 / 0.661 / 0.336 of the exact variance at 8 / 16 / 48 stations). The share of a satellite's motion the smoother books as walk is about sum q / (stated variance + sum q), so a tighter stated variance gives a larger walk share for the same q. The review's prediction: the field's clustered roster sees its satellites with more stations, and the field's larger walk share is card WSX's defect showing through the hold. Tested from existing traces and the unit network, before any build: the walk share against the stations observing the satellite, with clone age as the rival explanation, on both beds; the unit network at the beds' station profiles. If the link holds, the augmented previous border is the exact fix for a measured defect in the quantity a consumer uses, and the critical path.
- **source:** [WSX](WSX_within_station_cross_terms_2026-09-30.md) sections 6 to 8; [SMC](SMC_smoothed_hold_common_step_removed_2026-10-01.md) sections 4 and 5; [FXA](FXA_field_excess_phase_visible_motion_2026-09-30.md); [SMD](SMD_smoothed_hold_block_member_2026-09-30.md) section 8; [PVN](PVN_phase_visible_nees_2026-09-29.md) (the stated contrast variance with every cross term); [REV](REV_revision_member_replay_2026-09-30.md) section 6 (the trace columns); the outside review's note of 2026-10-01
- **knobs / posture:** none; the traces of `runs/_rap_rev1_blk6_{f,s}` (card REV's instrument build, card SMD's exact-update posture, 6 h) and the unit network of `tests/test_coupling_exact_update.cpp`; nothing armed
- **dates in the text:** 2026-10-01
- **hashes / run dirs:** to be filled at the read (section 5)

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
