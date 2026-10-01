# FXA: the field's excess phase-visible motion: steps at events, oversized updates, or a drift

**STATUS**

- **id:** FXA
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-09-30 (lever 1 after cards REV and WSX). On the field, block clones born after convergence are revised 2 to 4 times beyond their own stated variance (card REV's revision NEES 4 to 18, the sim 0.2 to 1.6), and the walk share passes r_D 1.5 to 2.2 against the sim's 0.3 to 0.5 (card SMD); both beds carry about 60 stations, so the cross-station over-statement of card WSX, which would hit both alike, is not the whole story. For a consistent model the smoothed generation-start value D(t0|t) is a martingale in the data: its revision increments are uncorrelated and their variance is the clone's stated variance fall. The card tests, on card REV's 6 h traces with no run and no build, which of three causes carries the field's excess: steps at declared events, increments larger than stated, or a drift (positively correlated increments, a time-correlated error the white model books as revision). Prediction: a drift.
- **source:** [REV](REV_revision_member_replay_2026-09-30.md) sections 6 and 7 (the revision NEES, the trace columns); [WSX](WSX_within_station_cross_terms_2026-09-30.md) sections 6 to 8; [SMD](SMD_smoothed_hold_block_member_2026-09-30.md) section 8 (r_D); [PVN](PVN_phase_visible_nees_2026-09-29.md) (the field NIS 0.05); [QPV](QPV_anchor_q_phase_visible_2026-09-30.md) section 6 (a larger walk does not serve the consumer)
- **knobs / posture:** none; the traces of `runs/_rap_rev1_blk6_{f,s}` (card REV's instrument build, card SMD's exact-update posture, 6 h)
- **dates in the text:** 2026-09-30
- **hashes / run dirs:** to be filled at the read (section 5)

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
