# SMC: the smoothed hold's walk share with the constellation-common step removed

**STATUS**

- **id:** SMC
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-10-01 (card FXA's amended decision). Card FXA found the field's excess phase-visible motion to be a drift common to the constellation (with each epoch's median step over the satellites removed, the field's per-satellite motion is the sim's size), which a float consumer absorbs when it reaches every satellite alike. Card SMD's member passes its walk share Phi_D per block clone from each clone's own birth, and births are spread over the run by retirements, so a common drift reaches the wire as a between-satellite difference. The card replays card SMD's member offline with each epoch's constellation-common step removed from the increments of Phi_D (the rebuilt-wire trick on card REV's 6 h traces, both beds) and grades it on the censuses. Predictions: the C1W loss from hour 3 disappears; the hour-1 gain keeps at least half its size; the sim does not get worse.
- **source:** [FXA](FXA_field_excess_phase_visible_motion_2026-09-30.md) sections 4 and 5; [SMD](SMD_smoothed_hold_block_member_2026-09-30.md) sections 3 and 8; [REV](REV_revision_member_replay_2026-09-30.md) section 6 (the satellite-consistent bridge, the census tools); [LAH](LAH_level_aware_hold_2026-09-29.md) (the median clock step its transfer removes); [QPV](QPV_anchor_q_phase_visible_2026-09-30.md) section 6 (the gain and the loss scale together with the fraction of Phi_D written)
- **knobs / posture:** none; offline wires from `runs/_rap_rev1_blk6_{f,s}` over `runs/_rap_lahk12_6_{f,s}`; nothing armed
- **dates in the text:** 2026-10-01
- **hashes / run dirs:** to be filled at the read (section 4)

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
