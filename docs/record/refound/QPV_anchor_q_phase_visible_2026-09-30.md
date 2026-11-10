# QPV: the anchor's clock q from the phase-visible passed motion

**STATUS**

- **id:** QPV
- **topic:** network-vehicle
- **status:** closed
- **summary:** Q1 FAILS: the rule derives 3.1 times card Q3's floor (9.1e-8 m2/s); at the nearest rung the C1W loss stays and the producer pays, so the anchor's q stays at the floor.
- **source:** [SMD](SMD_smoothed_hold_block_member_2026-09-30.md) sections 1 and 8; [SMH](SMH_smoothed_hold_clone_2026-09-29.md) sections 4, 9 to 12; [PRM](PRM_parameter_registry_2026-09-29.md) sections 4 and 12 (the q rule); card Q3 (`docs/SPEC_clock_temporal_anchor.md` sections 7 and 8: the carrier-phase structure function and the q ladder); the outside review's fourth note (2026-09-30)
- **knobs / posture:** `network.clock_anchor_q` (2.9e-8 m2/s on the exact-update arms; the loader default 1e-4 is card Q3's pre-floor value) on arms only; `network.emit_pb_hold_smoothed` armed on the arms as the instrument; no default, golden, ledger or posture change
- **dates in the text:** 2026-09-30
- **hashes / run dirs:** exe `gnss_processor_smd3.exe` `51635d166c00033f`; arms `runs/_rap_qpv_{q025,q05,q2,q4}_{f,s}` with `runs/_rap_smd2_ex6_{f,s}`; rebuilt wires `runs/_rpl_qpv_q4lah_{f,s}`, `runs/_rpl_qpv_a{025,05,075}_{f,s}`; census keys `q4lah`, `q4smh`, `q4lahs`, `q4smhs`, `qa025`, `qa05`, `qa075` and their `s` twins

**Outcome.** Question: can the anchor's clock q be derived from the smoothed hold's phase-visible passed motion? Q2 FAILS at the lowest rung (sim r 1.21); Q3 holds (the C1W loss of hours 3 to 5 stays at +0.014 to +0.022 m). In the shrinkage read the C1W hour-1 gain and the later loss scale together. Of section 6's two levers, the member was FALSIFIED by card [REV](REV_revision_member_replay_2026-09-30.md) and the wander at its source remains as lever 1 (card REV section 7); card [SMC](SMC_smoothed_hold_common_step_removed_2026-10-01.md) section 5 closed the hold family on this estimator. Card [SFR](SFR_sim_fixture_coloured_phase_2026-10-01.md) supersedes the sim numbers.

## 1. Why

Card SMD split the smoothed hold's passed motion into a level part and a phase-visible part: the
level part is zero on both beds (the satellite phase biases are random constants of the model),
and the phase-visible part carries it all. On the sim, whose truth has no phase-visible motion,
it reaches p90 7 to 12 mm at r_D 0.3 to 0.5 after the first hour of the clone's age; on the field
about twice that at r_D 1.5 to 2.2 (SMD section 8). The outside review's decision 2 was to derive
the clock's q from the phase-visible part once the split exists; with the level part empty the
whole of r is the input. The anchor's q, 2.9e-8 m2/s, is card Q3's walk fit to the structure
function of the carrier-phase time differences over 300 to 2400 s, itself a phase-visible
measurement, so the derivation has an independent comparison.

Two cautions from SMD's tables: the passed motion saturates with age (field p90 about 2.5 cm from
the second hour on, sim about 1.2 cm), so the phase-visible error is not a random walk at one q
over hours; and r_D falls with age on both beds for the same reason. The rule below therefore
reads the ages where the bins are populated (1 to 3 h) and reports the age dependence beside it.

## 2. The rule

For each rung q of the ladder, r_f(q) and r_s(q) are the mean of r_D = Phi_D^2 / (W_D - V_D) over
the smoothed hold's clones of clone age 1 to 3 h (one row per satellite and epoch, card SMD's
split read on the clock clone, whose passed motion equals the block's), on the field and on the
sim. The sim's truth has no phase-visible motion, so r_s(q) is the estimator's floor at that q.
The field's excess x(q) = r_f(q) - r_s(q) is the passed motion the floor does not explain, in
units of the stated walk. The derived q solves x(q) = 1 (the excess equals the stated walk),
interpolated linearly in log q between the bracketing rungs. If x stays above 1 or below 1 across
the ladder, the derived q is reported as outside it, with the direction.

## 3. The ladder and the reads

- **Rungs:** `network.clock_anchor_q` = 7.25e-9, 1.45e-8, 2.9e-8, 5.8e-8, 1.16e-7 m2/s (a quarter of the floor to four
  times it), 6 h on both beds, card SMH's smoothed hold armed (the instrument; the
  block clones ride along), the exact-update posture of cards CTU to SMD, the exe of commit
  `fa3c96c`. The 2.9e-8 rung is card SMD's `runs/_rap_smd2_ex6_{f,s}`.
- **Producer reads at every rung** (the q is solve-affecting): card CTU2's datum-free level at
  epochs 16 / 60 / 120 / 240 (sim), the IF code-residual spread in hour 2, card Q3's trips and
  in-epoch re-solves, the RAIM re-floats, card PVN's NIS and within-arc NEES, and the
  prior-residual census's clock and code-bias families (card Q3's tension sink).
- **The census at the derived rung** (the rung nearest the derived q if it falls between rungs):
  the 16 field sites plus WTZZ00DEU and the 16-station sim census, for the smoothed hold and the
  phase-clock hold of the same solve (a phase-clock arm at that rung), against the phase-clock
  hold at 2.9e-8.
- **The shrinkage read (emit-side, at 2.9e-8):** wires P = P_LAH - alpha (frozen + Phi) rebuilt
  offline from the smoothed hold's trace (card SMD's attribution tool) for alpha = 0.25, 0.5 and
  0.75, and the same censuses. alpha = 0 is the phase-clock hold and alpha = 1 the smoothed hold,
  so the census's best alpha by hour says how much of the passed motion serves the consumer,
  without changing the solve.

## 4. Predictions (written before the runs)

- **Q1.** The derived q is within a factor 2 of 2.9e-8 m2/s, because the passed motion and card
  Q3's carrier-phase structure function read the same phase-visible clock.
- **Q2.** The sim's r_D after the first hour of age stays below 1 at every rung (its truth has no
  phase-visible walk), and the field's excess x(q) falls as q rises.
- **Q3.** No rung at or above the floor removes the census's C1W loss of hours 3 to 5 (median
  paired change at most +0.005 m): a larger stated walk passes more. If the loss goes only below
  the floor, the consumer is served by passing less than the phase-visible motion supports, which
  is a suppression of the estimator's wander, not a statement about the a priori clock.
- **Q4.** The shrinkage read's best alpha falls with the hour: near 1 in hour 1 (the hour-1 gain)
  and near 0 from hour 3 on the field; near 0 from hour 2 on the sim.

## 5. What would follow

A derived q that agrees with the floor (Q1) leaves the anchor's q where it is and places the
hold's loss in the estimator's phase-visible wander, which no q removes; the lever is then the
wander itself (the float ambiguities' datum behind D, card PVN) or a hold that passes the motion
only while it is young (Q4's alpha by hour). A derived q far from the floor revises card Q3's
calibration for the exact-update posture, and goes to card PRM's registry with the product it
was derived for. Arming anything is the author's decision.

## 6. The ladder, the census and the shrinkage read (2026-09-30)

Exe `gnss_processor_smd3.exe` (`51635d166c00033f`, the code of commit `fa3c96c`); arms
`runs/_rap_qpv_{q025,q05,q2,q4}_{f,s}` and card SMD's `runs/_rap_smd2_ex6_{f,s}` at 2.9e-8, 6 h,
card SMH's smoothed hold armed. Session scripts `qpv_ladder.py` (the rule), `ctugrade.py`,
`pvnees.py`, `smd_ctlwire.py` (the rebuilt wires), `census6h.py`, `simcensus6h.py`.

| `clock_anchor_q` (m2/s) | 7.25e-9 | 1.45e-8 | 2.9e-8 | 5.8e-8 | 1.16e-7 |
| --- | --- | --- | --- | --- | --- |
| field r, clone age 1 to 3 h | 4.71 | 2.86 | 1.95 | 1.50 | 1.09 |
| sim r, clone age 1 to 3 h | 1.21 | 0.85 | 0.56 | 0.34 | 0.18 |
| excess x = r_f - r_s | 3.50 | 2.02 | 1.40 | 1.16 | 0.91 |
| field / sim \|Phi\| p90 at age 2 to 3 h (mm) | 22 / 9 | 25 / 12 | 29 / 14 | 35 / 16 | 40 / 14 |
| field IF code spread, hour 2 (m) | 0.164 | 0.164 | 0.164 | 0.175 | 0.193 |
| field anchor trips (besides the 31 of the handover) | 0 | 0 | 0 | 8 (phase) | 5 (phase) |
| field RAIM re-floats (epochs / ambiguities) | 25 / 61 | 23 / 58 | 23 / 59 | 24 / 57 | 27 / 68 |
| PVN NIS, field / sim | 0.42 / 1.08 | 0.33 / 1.07 | 0.30 / 1.05 | 0.26 / 1.01 | 0.21 / 0.93 |
| in-run consumer, smoothed, field hours 2 / 3 (m) | 0.019 / 0.021 | 0.018 / 0.012 | 0.022 / 0.010 | 0.037 / 0.016 | 0.046 / 0.034 |
| in-run consumer, smoothed, sim hours 3 / 4 (m) | 0.059 / 0.041 | 0.057 / 0.041 | 0.064 / 0.044 | 0.094 / 0.056 | 0.115 / 0.087 |

The sim's datum-free level (0.10 / 0.06 / 0.04 / 0.03 m at epochs 16 / 60 / 120 / 240), its IF code
spread (0.047 to 0.049 m) and its trips (the 31 of the handover) are the same at every rung.

**The rule: the derived q is 9.1e-8 m2/s, 3.1 times the floor. Q1 FAILS** (the bar was a factor
2). **Q2 FAILS at the lowest rung**: the sim's r over ages 1 to 3 h is 1.21 at a quarter of the
floor (the estimator's wander exceeds a stated walk that small); from half the floor up it stays
below 1, and the field's excess falls with q as predicted.

**The census at the derived rung** (1.16e-7, the rung nearest 9.1e-8; the phase-clock hold's
wire of the same solve rebuilt from the smoothed run's trace, which reproduces the 2.9e-8
phase-clock run to 1e-6 m; keys `q4lah`, `q4smh`, `q4lahs`, `q4smhs`). The phase-clock hold is
the same at both q (field paired change within 0.003 m in every hour; sim 0.000). The smoothed
hold against its own phase-clock hold, median paired change (better / worse at 1 cm):

| | h1 | h2 | h3 | h4 | h5 | h6 |
| --- | --- | --- | --- | --- | --- | --- |
| field C1W (10) | -0.091 (6 / 3) | +0.012 (3 / 6) | +0.015 (3 / 7) | +0.014 (2 / 5) | +0.022 (3 / 6) | -0.002 (2 / 3) |
| field C1C (6) | -0.022 (3 / 3) | +0.020 (2 / 3) | +0.027 (2 / 4) | -0.006 (2 / 2) | -0.019 (4 / 2) | -0.006 (3 / 3) |
| sim (16) | +0.004 (5 / 7) | +0.017 (5 / 9) | +0.014 (2 / 9) | +0.017 (2 / 11) | +0.007 (0 / 6) | +0.001 (0 / 2) |

WTZZ00DEU 0.102 0.046 0.034 0.019 0.027 0.034 m against 0.110 0.025 0.016 0.020 0.030 0.026.
**Q3 holds**: the C1W loss of hours 3 to 5 stays at the derived q (+0.014 to +0.022 m), hour 2
and the C1C sites get worse than at the floor, the sim's loss doubles, and the producer pays
(the field's code spread +0.03 m in hour 2, phase-statistic trips, PVN's NIS further from 1).

**The shrinkage read** (2.9e-8, P = P_LAH - alpha (frozen + Phi), keys `qa025`, `qa05`, `qa075`,
the `s` keys on the sim), median paired change against the phase-clock hold:

| alpha | field C1W h1 | C1W h3 / h4 / h5 | C1C h4 / h5 | WTZZ00DEU h3 / h4 / h5 (m) | sim h3 / h4 |
| --- | --- | --- | --- | --- | --- |
| 0 (phase-clock hold) | 0 | 0 / 0 / 0 | 0 / 0 | 0.016 / 0.020 / 0.030 | 0 / 0 |
| 0.25 | -0.024 | +0.003 / +0.010 / +0.004 | -0.007 / -0.012 | 0.013 / 0.014 / 0.027 | -0.000 / +0.001 |
| 0.5 | -0.039 | +0.006 / +0.018 / +0.011 | -0.009 / -0.022 | 0.011 / 0.011 / 0.024 | +0.002 / +0.003 |
| 0.75 | -0.053 | +0.011 / +0.029 / +0.019 | -0.010 / -0.026 | 0.009 / 0.013 / 0.023 | +0.003 / +0.007 |
| 1 (smoothed hold) | -0.068 | +0.015 / +0.043 / +0.025 | -0.010 / -0.017 | 0.010 / 0.016 / 0.022 | +0.008 / +0.011 |

The C1W sites' hour-1 gain and their loss from hour 3 both scale with alpha, nearly linearly: no
alpha keeps the one without the other. **Q4 holds for the C1W sites and the sim** (the best alpha
near 1 in hour 1 and near 0 from hour 3) and not for the C1C sites and the live consumer, which
prefer alpha 0.5 to 0.75 in hours 3 to 5.

**Reading.** The rule asks the stated walk to equal the field's passed motion net of the sim's
floor, and lands at three times card Q3's carrier-phase floor; at that q the hold passes more
motion and the consumers that lost lose as much or more. The field's excess over the sim's floor
is therefore not a clean walk of the a priori clock that a larger q would pass to the consumer's
benefit. That is consistent with the field's estimator wandering more than the sim's, which the
rule's subtraction assumes away (the passed motion grows with q on both beds, and the field's
p90 at clone age 2 to 3 h is two to three times the sim's at every rung); this card does not
separate the two. The anchor's q stays at the floor, which the census at the derived q and the
producer reads both support.

What the hold passes serves the consumer at ignition (hour 1) and costs the C1W sites after
convergence, in proportion; the hour of the run, not the q, separates the two. The levers that
remain are the phase-visible wander itself (card PVN's D, limited by the float ambiguities'
datum) and a member that passes the generation-start revision of an ignition clone and then
holds (the hour-1 gain without the later wander), both for the author to choose. Nothing is
armed; no default moves.
