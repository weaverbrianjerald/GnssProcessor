# FXA: the field's excess phase-visible motion: steps at events, oversized updates, or a drift

**STATUS**

- **id:** FXA
- **topic:** network-vehicle
- **status:** closed
- **summary:** READ 2026-09-30 (sections 4 and 5): A1, A2 and A3 hold. The sim's smoothed generation-start value is a martingale (white increments at the stated size); the field's increments are smaller than stated but persistently correlated (lag-1 +0.35, +0.12 at 30 minutes; revision NEES at 1 h 24 times the increment NIS), and the event windows hold only their base-rate share. The drift is stationary, smooth and COMMON TO THE CONSTELLATION: with each epoch's median step removed, the field's per-satellite motion (7.6 mm at 30 minutes) is no larger than the sim's. Reading: consistent with a clock-datum drift (the pivot's a priori clock, not yet verified), harmless when passed commonly, made between-satellite by the smoothed hold's per-clone walk share; the next card replays card SMD's member with the common step removed (section 6: that mechanism falsified by card SMC, the walk share does not carry the drift). Pre-registered 2026-09-30 (lever 1 after cards REV and WSX). On the field, block clones born after convergence are revised 2 to 4 times beyond their own stated variance (card REV's revision NEES 4 to 18, the sim 0.2 to 1.6), and the walk share passes r_D 1.5 to 2.2 against the sim's 0.3 to 0.5 (card SMD); both beds carry about 60 stations, so the cross-station over-statement of card WSX, which would hit both alike, is not the whole story. For a consistent model the smoothed generation-start value D(t0|t) is a martingale in the data: its revision increments are uncorrelated and their variance is the clone's stated variance fall. The card tests, on card REV's 6 h traces with no run and no build, which of three causes carries the field's excess: steps at declared events, increments larger than stated, or a drift (positively correlated increments, a time-correlated error the white model books as revision). Prediction: a drift.
- **source:** [REV](REV_revision_member_replay_2026-09-30.md) sections 6 and 7 (the revision NEES, the trace columns); [WSX](WSX_within_station_cross_terms_2026-09-30.md) sections 6 to 8; [SMD](SMD_smoothed_hold_block_member_2026-09-30.md) section 8 (r_D); [PVN](PVN_phase_visible_nees_2026-09-29.md) (the field NIS 0.05); [QPV](QPV_anchor_q_phase_visible_2026-09-30.md) section 6 (a larger walk does not serve the consumer)
- **knobs / posture:** none; the traces of `runs/_rap_rev1_blk6_{f,s}` (card REV's instrument build, card SMD's exact-update posture, 6 h)
- **dates in the text:** 2026-09-30
- **hashes / run dirs:** traces of `runs/_rap_rev1_blk6_{f,s}` (exe `gnss_processor_rev1.exe` `a59231c8802a14f8`); grader `scripts/clock_level/fxa_attrib.py`

## 1. The test

Per block clone, the trace row at epoch t carries R(t) = D(t0|t) - D(t0|t0) and the clone's stated
variance V(t) = Var(D(t0) | data to t) (card REV section 6). Under the model, the increments
dR(t) = R(t) - R(t-1) are uncorrelated with variance V(t-1) - V(t). Three readings of a clone
population, on consecutive rows only (a gap breaks the chain):

- **The increment NIS**, the mean of dR^2 / (V(t-1) - V(t)) over rows with a positive fall. Near 1
  for a consistent model; above 1 means the increments are larger than stated.
- **The persistence**, the lag-1 autocorrelation of the normalized increments
  z = dR / sqrt(V(t-1) - V(t)) within clones, and the ratio of the revision NEES at one hour of age
  to the mean increment NIS over the same rows (above 1 only through correlation between
  increments).
- **The event share**, the share of the sum of z^2 that falls within 2 epochs of a declared event
  on the clone's satellite at any station, against the share of rows those windows hold (the base
  rate). Event families: slips and resumes (`lli_slip`, `if_slip`, `mw_wl_slip`, `reentry_relevel`,
  `arc_resume`, `sig_resume`, `attrib_arc`), the second code channel (`companion_open`,
  `companion_close`), datum and anchor events (`anchor_handover`, `handover_continuous`,
  `handover_cold`, `anchor_slip_handover`, `sts_applied`, `sts_cold_declared`, `refsat_reset`),
  clock coupling and seeds (`cta_decouple`, `cta_recouple`, `cta_disc_landed`, `seed_clock`,
  `seed_code_bias`, `seed_held`, `readmit`, `reseed_disc_landed`), columns (`col_added`,
  `col_removed`) and emission (`retro_bump`, `gross_los_emit_disc`, `gen_start`).

The populations: clones born after the first hour of run time ("later", where the field's excess
sits) and clones born in it ("ignition"), each bed, one row per satellite and epoch.

## 2. Predictions (written before the read)

- **A1 (the sim is a martingale).** Later clones on the sim: increment NIS within 0.5 to 2,
  lag-1 autocorrelation of z within +-0.1.
- **A2 (the field drifts).** Later clones on the field: lag-1 autocorrelation of z above 0.3 and
  above the sim's by at least 0.2; the revision NEES at one hour of age at least twice the mean
  increment NIS of the same rows (the excess is persistence, not size); the increment NIS itself
  at most 2 (card PVN's field updates are smaller than stated).
- **A3 (not the events).** On the field the event windows hold at most 1.5 times their base-rate
  share of the sum of z^2.

**The decision rule.** A2 and A3 hold: the field's excess is a time-correlated error that the white
walk cannot carry; the next card models it (its correlation time and amplitude measured from the
same increments, a Gauss-Markov component of the satellite's phase-visible clock instead of a
larger walk, which card QPV found the consumer does not want), graded against the census's C1W loss
from hour 3. A3 fails: the event families that carry the excess are the next card's target, their
responses checked against standing invariants 1 and 7. A2 fails with the increment NIS above 2: the
field's increments are larger than stated, a variance-model question (the per-epoch noise along D).
A1 fails: the instrument or the clone is suspect and nothing is concluded from the field.

## 3. Order

The grader (`scripts/clock_level/fxa_attrib.py`), the read on both beds, the verdicts. No run, no
build, nothing armed.

## 4. The read (2026-09-30)

`scripts/clock_level/fxa_attrib.py` on `runs/_rap_rev1_blk6_{s,f}` (card REV's 6 h traces, the L2W key
of every satellite, one row per satellite and epoch, increments between rows 30 s apart; no
increment lacked a positive variance fall).

| | sim ignition | sim later | field ignition | field later |
| --- | --- | --- | --- | --- |
| clones / increments | 40 / 11 643 | 80 / 10 519 | 47 / 10 060 | 115 / 11 958 |
| increment NIS | 1.02 | 1.13 | 0.39 | 0.52 |
| lag correlation of z at 1 / 5 / 20 / 60 epochs | +0.04 / -0.01 / +0.01 / -0.01 | +0.08 / +0.11 / +0.05 / -0.06 | +0.47 / +0.37 / +0.34 / +0.27 | +0.35 / +0.21 / +0.16 / +0.12 |
| revision NEES at clone age 1 h / increment NIS of the same rows | 0.74 / 1.10 | 1.42 / 1.20 | 1.21 / 0.32 | 10.28 / 0.43 |
| persistence R^2 / sum dR^2 at 1 h, p50 | 0.27 | 0.58 | 0.77 | 20.3 |
| event windows: share of sum z^2 / share of rows | 1.6 / 1.4 % | 2.9 / 3.4 % | 26.1 / 24.6 % | 23.9 / 22.2 % |

**A1 holds**: on the sim the later clones' increments are white (lag-1 +0.08) at the stated size
(NIS 1.13); the smoother is a martingale there. **A2 holds**: on the field the later clones'
increments are individually smaller than stated (NIS 0.52) and persistently correlated (+0.35 at one
epoch, still +0.12 at 30 minutes, the sim's +0.08 / -0.06); the revision NEES at one hour is 24 times
the increment NIS of the same rows. **A3 holds**: the event windows hold their base-rate share (field
enrichment 1.08; by family 0.48 to 1.18, emission 4.05 on 0.6 % of the rows).

**The drift's shape** (descriptive, the next card's input): the median-based rms of the frame-free
motion of D over a lag, for converged rows (ignition clones past 1.5 h of age, later clones past
30 minutes):

| lag | 2.5 min | 10 min | 30 min | 60 min |
| --- | --- | --- | --- | --- |
| sim | 3.5 mm | 6.3 mm | 11.8 mm | 16.7 mm |
| sim, each epoch's common step removed | 2.9 mm | 5.3 mm | 8.7 mm | 14.8 mm |
| field | 4.7 mm | 17.4 mm | 56.0 mm | 98.1 mm |
| field, each epoch's common step removed | 2.0 mm | 4.4 mm | 7.6 mm | 11.1 mm |
| the stated walk | 2.1 mm | 4.1 mm | 7.1 mm | 10.0 mm |

The field's drift is stationary (the same in a reborn clone's first half hour, after it, and in
ignition clones long converged: 26, 73 and 109 mm at 10, 30 and 60 minutes) and smooth (the rms grows
about as the lag to the power 1.2 between 10 and 60 minutes, saturating beyond an hour). And it is
**common to the constellation**: with each epoch's median step over the satellites removed, what is
left per satellite (7.6 mm at 30 minutes, 11.1 mm at an hour) is no larger than the sim's. The field's
excess is not per-satellite wander of the estimator; it is a constellation-common drift of the
phase-visible clock.

## 5. Reading and decision (2026-09-30)

A constellation-common motion of D is what the clock datum produces when the pinned pivot's a priori
clock moves: every other satellite's clock is realized against the pivot's, so the pivot's a priori
error appears with the opposite sign on all of them. On the sim the a priori clock is the truth and
there is nothing to absorb, which matches its white increments. A float consumer absorbs a common
mode in its receiver clock, so this drift is harmless on a wire that passes it to every satellite
alike. A wire that passes it per satellite from different starting epochs turns it into a
between-satellite difference, which the consumer cannot absorb. The smoothed hold does exactly that:
its walk share Phi_D accumulates from each clone's own birth, and births are spread over the run by
retirements (card SMD section 3), while the phase-clock hold transfers the clock with its
constellation median step removed (card LAH). That is a candidate mechanism for the census's C1W loss
from hour 3, which no hold, q or covariance card has removed so far.

**The decision rule, amended.** A2 and A3 hold, and the rule's branch was a model of the drift as a
Gauss-Markov component of each satellite's phase-visible clock. The common-mode reading rules that
form out: a per-satellite model would let every clock track a drift that belongs to the datum. The
next card instead replays card SMD's member with the constellation-common step removed from the walk
share (offline, the rebuilt-wire trick, both beds), predicting that the C1W loss from hour 3
shrinks while the hour-1 gain stays. Whether the common drift is the pivot's a priori clock (a
pivot-election question) is a separate read. Nothing is armed.

## 6. Annotation (2026-10-01)

The candidate mechanism of section 5 is falsified by card SMC: the smoother books the common drift
as revision, not as walk share, so card SMD's member does not carry it and removing it changes
neither the wire's between-satellite spread nor the census. The common drift itself stands, and so
does the reading that it is harmless to a float consumer on a wire that passes it commonly; card
REV's field revision NEES of 4 to 18 was this drift (card REV section 8).
