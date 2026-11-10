# SMC: the smoothed hold's walk share with the constellation-common step removed

**STATUS**

- **id:** SMC
- **topic:** network-vehicle
- **status:** closed
- **summary:** FALSIFIED 2026-10-01: removing the constellation-common step moves the wire by at most 3 mm and the census not at all; the hold family is closed on this estimator.
- **source:** card FXA's amended decision; [FXA](FXA_field_excess_phase_visible_motion_2026-09-30.md) sections 4 and 5; [SMD](SMD_smoothed_hold_block_member_2026-09-30.md) sections 3 and 8; [REV](REV_revision_member_replay_2026-09-30.md) section 6 (the satellite-consistent bridge, the census tools); [LAH](LAH_level_aware_hold_2026-09-29.md) (the median clock step its transfer removes); [QPV](QPV_anchor_q_phase_visible_2026-09-30.md) section 6 (the gain and the loss scale together with the fraction of Phi_D written)
- **knobs / posture:** none; offline wires from `runs/_rap_rev1_blk6_{f,s}` over `runs/_rap_lahk12_6_{f,s}`; nothing armed
- **dates in the text:** 2026-10-01
- **hashes / run dirs:** wires `runs/_rpl_smc_phidc_{f,s}` (rule `phidc`, satellite-consistent bridge); census keys `smcdc`, `smcdcs` on `gnss_processor_rev1.exe` `a59231c8802a14f8`

**Outcome.** The card asked whether card FXA's constellation-common drift, made between-satellite by the clones' births, is what card [SMD](SMD_smoothed_hold_block_member_2026-09-30.md)'s member passes to the C1W sites from hour 3. It is not: the smoother books the drift as revision, so the late between-satellite spread stays 25.2 against 25.1 mm on the field (bar: half). C1 FAILS (C1W +0.027, +0.052, +0.047 m in hours 3 to 5), C2 and C3 hold; the phase-clock hold stays the wire. Card REV's field revision NEES of 4 to 18 read the drift. Card [PAC](PAC_common_drift_pivot_clock_2026-10-01.md) traced the drift to the pinned pivot's level; card [SFR](SFR_sim_fixture_coloured_phase_2026-10-01.md) supersedes the sim NEES.

## 1. The member

Per satellite (its L2W key's block clone, the satellite-consistent bridge of card REV section 6,
applied to every key of the satellite), the walk share's increment at epoch t is
dPhi_s(t) = Phi_D,s(t) - Phi_D,s(t-1) (from the clone's birth, Phi_D = 0). The constellation-common
step is c(t), the median of dPhi_s(t) over the satellites with a live clone and an increment at t
(at least four). The member writes

P = P_LAH - (frozen + Phi^dc),   Phi^dc_s(t) = sum over the clone's epochs up to t of (dPhi_s - c),

with the frozen sum of earlier clones of the generation kept as card SMD's bridge keeps it. Removing
c from every satellite alike is what card LAH's transfer does with the median clock step; a common
part left in the wire is absorbed by a float consumer's receiver clock either way, so the member
differs from card SMD's only by the between-satellite share that the clones' different births
gave the common drift.

## 2. Predictions (written before the read)

- **C1 (the loss from hour 3).** Field C1W median paired change against the phase-clock hold in
  each of hours 3 to 5 at most +0.010 m (card SMD on the same rebuild +0.027, +0.053, +0.047), with
  at most 4 of 10 sites worse at 1 cm.
- **C2 (the hour-1 gain).** Field all-site hour-1 change at most -0.035 m (card SMD -0.070).
- **C3 (the sim).** Sim change in each of hours 3 to 5 no worse than card SMD's on the same rebuild
  plus 0.002 m (card SMD +0.009, +0.012, +0.009).
- **The wire.** On the field the common step removes most of the walk share's between-satellite
  spread late in the run: the median over epochs of the cross-satellite standard deviation of
  Phi^dc in hours 3 to 6 at most half that of Phi_D.

**The decision rule.** C1 and C2 hold: the C1W loss is the common drift made differential, and the
member (a smoothed hold with the common step removed, or card SMD's member with its walk share
de-commoned at the hold) is owed a build with a 2 h G0 / G1 and the 6 h censuses for the author.
C1 fails: the loss is per-satellite motion, as card QPV's scaling read suggested, and the hold
family is closed on this estimator; the common drift's source (the pivot's a priori clock) is read
next on its own. C2 fails with C1 holding: the hour-1 gain was the common drift too, and the
member's value is the loss it removes, graded against the phase-clock hold alone.

## 3. Order

The wire rule (`rev_wire.py`, rule `phidc` on the satellite-consistent bridge), the wire read, the
censuses (field 16 sites plus WTZZ00DEU, sim 16 stations, on `gnss_processor_rev1.exe`, paired
against `revlah` and card SMD's satellite-consistent rebuild `revphisw`), the verdicts.

## 4. The read (2026-10-01)

Wires `runs/_rpl_smc_phidc_{f,s}` (`rev_wire.py ... phidc satwise` over `runs/_rap_lahk12_6_{f,s}` and
the traces of `runs/_rap_rev1_blk6_{f,s}`); census keys `smcdc` (field) and `smcdcs` (sim) on
`gnss_processor_rev1.exe`, against `revlah` / `revlahs` and card SMD's rebuild `revphisw` / `revphis`.

**The wire read FAILS, and decides the mechanism.** Removing each epoch's common step leaves the
walk share's between-satellite spread where it was: median over the epochs of run hours 3 to 6,
25.2 against 25.1 mm on the field and 9.0 against 8.9 mm on the sim (bar: half). The wire moves by
at most 3 mm against card SMD's (p90 1.7 mm field, 2.2 mm sim). The constellation-common drift of
card FXA is not in the walk share: the smoother books it as revision (the model's walk is far
smaller than the drift), so the clones' different births cannot turn it into a between-satellite
difference through Phi_D.

**The censuses**, median paired change against the phase-clock hold (better / worse at 1 cm):

| | h1 | h2 | h3 | h4 | h5 | h6 |
| --- | --- | --- | --- | --- | --- | --- |
| field all, card SMD rebuild | -0.070 (11/3) | +0.011 (5/8) | +0.019 (4/9) | +0.022 (5/9) | +0.011 (6/9) | +0.005 (5/7) |
| field all, this member | -0.069 (11/3) | +0.011 (5/9) | +0.019 (4/9) | +0.022 (5/9) | +0.011 (6/9) | +0.005 (5/6) |
| field C1W, this member | -0.067 (7/2) | +0.011 (3/6) | +0.027 (2/6) | +0.052 (2/8) | +0.047 (2/7) | +0.005 (2/3) |
| sim, card SMD | -0.000 (5/5) | +0.002 (4/7) | +0.009 (1/8) | +0.012 (3/9) | +0.009 (0/6) | +0.004 (1/4) |
| sim, this member | -0.000 (5/5) | +0.002 (4/7) | +0.009 (1/8) | +0.012 (3/9) | +0.009 (0/6) | +0.004 (1/4) |

**C1 FAILS** (C1W +0.027, +0.052, +0.047 m in hours 3 to 5); **C2 holds** (all-site hour 1
-0.069 m); **C3 holds** (the sim equals card SMD's).

## 5. Reading and decision (2026-10-01)

Card FXA's candidate mechanism is falsified at the wire: the common drift lives in the revision,
not in the walk share card SMD's member passes, so removing it changes nothing a consumer sees. What
the member passes is per-satellite: its between-satellite spread late in the run is 25 mm on the
field against 9 mm on the sim, although the total per-satellite motion of D (walk share plus
revision, the common step removed) is the sim's size on the field (card FXA section 4). The field's
smoother assigns a larger part of the same per-satellite motion to the walk, and that part is what
the C1W sites pay for from hour 3, in proportion to the fraction written (card QPV's shrinkage read).

A correction follows for card REV's field instrument. The revision carries the common drift, so
the later clones' revision NEES of 4 to 18 (card REV section 6) read it, not a per-satellite
inconsistency: with each epoch's common revision step removed, the field's later clones read 0.92,
1.19, 0.77 and 0.04 at clone ages 0 to 3 h and the sim's 0.68, 0.60, 0.61 and 0.11 (ignition clones
0.43 to 0.68 and 0.41 to 0.56). Per satellite, the smoother is consistent on both beds (standing
invariant 5: grade the datum-invariant quantity). Card QPV's r_D is formed from the walk share,
which carries no common step, and stands.

**The decision (the pre-registered rule).** C1 fails: the loss is per-satellite motion, the hold
family is closed on this estimator, and the phase-clock hold stays the wire. The common drift's
source (the pivot's a priori clock, a pivot-election question) is a separate read; it does not reach
a float consumer through any member of the family, since the phase-clock hold removes the median
clock step and the walk share does not carry it. Nothing is armed.
