# REV: the revision member of the hold family, replayed from card SMD's block arms

**STATUS**

- **id:** REV
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-09-30 (the outside review's note after card QPV, on the lever QPV section 6 names last: a member that passes the generation-start revision of an ignition clone and then holds). The fourth member of the hold family on the ionosphere-free phase-visible clock D: the wire's phase side at D(t0|t), the smoothed generation-start value, so the hold passes the revision R = D(t0|t) - D(t0|t0) where cards SMH and SMD pass the walk share Phi_D = D(t|t) - D(t0|t); frozen per clone at the first epoch the clone's own stated variance of D(t0) falls by less than the stated walk adds along D. Replayed offline from card SMD's 6 h block arms on both beds by the rebuilt-wire trick (card SMD section 7); no member is built. The existing traces carry neither R nor the clone's variance, so the clone trace gains five columns (an instrument, the products byte-identical) and the two block arms are re-run. Predictions: the hour-1 gain above card SMD's; unfrozen, more late wander than card SMD; frozen, the phase-clock hold from the freeze on. A model of the freeze's timing, written beside them, expects the freeze late on the sim and not within 6 h on the field.
- **source:** [SMD](SMD_smoothed_hold_block_member_2026-09-30.md) sections 1, 2, 7 and 8; [QPV](QPV_anchor_q_phase_visible_2026-09-30.md) section 6; [SMH](SMH_smoothed_hold_clone_2026-09-29.md) sections 9, 10 and 12 (the lifetime and the bridge); [PVN](PVN_phase_visible_nees_2026-09-29.md) section 5 (the stated variance of D); [LAH](LAH_level_aware_hold_2026-09-29.md) section 2 (the family's form); the outside review's note of 2026-09-30 (night)
- **knobs / posture:** none new; card SMD's exact-update posture with `network.emit_pb_hold_smoothed` and `network.emit_pb_hold_smoothed_block` (the block arms `_rap_smd3_blk6_{f,s}`), re-run on the instrument build; the wires are rebuilt offline from the phase-clock hold's wire of the same solve. Nothing armed; the phase-clock hold stays the reference wire; no phase-bias process noise
- **dates in the text:** 2026-09-30
- **hashes / run dirs:** to be filled at the runs (section 6)

## 1. The member

Within one block clone's life (card SMD section 2, frame-free), the phase-visible clock's motion
since the clone's birth splits into two shares:

D(t|t) - D(t0|t0) = Phi_D(t) + R(t),   Phi_D = D(t|t) - D(t0|t),   R = D(t0|t) - D(t0|t0).

Phi_D is the walk share, the motion the smoother attributes to D after t0. R is the revision
share, the smoother's correction of the generation-start value itself. The hold family on D
(card LAH section 2, (C - P_j)(t) = (c - p_j)(t0|t0) + X(t)) has one member per subset:

| member | passes X | ionosphere-free wire phase side |
| --- | --- | --- |
| the phase-clock hold (card LAH) | 0 | D(t0\|t0) |
| the smoothed hold (cards SMH, SMD) | Phi_D | D(t0\|t0) + Phi_D |
| the revision member (this card) | R | D(t0\|t) |
| both | Phi_D + R | D(t\|t), every declared frame step still hidden by the retirements |

Card SMD found the smoothed hold level-blind on this estimator and its passed motion wholly
phase-visible. Card QPV's shrinkage read found the hour-1 gain and the loss from hour 3 scaling
together with the fraction of Phi written, so no fraction of the walk share keeps the one without
the other, and the hour of the run separates them. The review's account of that: while a
consumer's own ambiguities are converging, the wire's phase-side value error reaches its
position, so tracking the producer's convergence helps; once they have converged, only the phase
side's variation matters, and passing wander hurts. R is a correction of a value, Phi_D a motion.
The revision member passes the value correction and, frozen once the value has converged, is the
phase-clock hold plus one constant per key, which a converged consumer's ambiguities absorb.

Per band the member keeps the generation-start difference between the bands, as every member
does: C - P_j = (c - p_j)(t0|t0) + R(t). On the wire, P = P_LAH - (frozen + R), with frozen the
sum of the R each earlier block clone of the key reached in the same generation (card SMD's
bridge, card SMH section 12): a clone's death keeps what it reached in the key's transfer, and a
reborn clone starts from R = 0 on the new frame.

R is frame-free by construction: the clone's value x0 moves only by the smoother's revisions
(the sum of its coefficients times the posterior minus the prior mean the solve folded), and a
declared frame step never steps it (`NetworkPbHoldClone.hpp`). Each epoch the clone's
frame-free step of D splits into the revision and the passed motion, so Phi_D + R = S_D, the
cumulative frame-free step of D since birth, row by row.

## 2. The freeze

Per block clone, V(t) = Var(D(t0) | data to t) = sigma^2 + m' Sigma(t|t) m, the D row's residual
variance plus its coefficients on the epoch's border (the quantity card SMD's U0 checks against an
augmented filter carrying D(t0) as a zero-noise state), and dW(t) = g' Q g, the stated walk along
D that the epoch's fold adds (card SMD's W_D increment). **The clone freezes at the first update
after its birth with V(t-1) - V(t) < dW(t)**: its information gain on D(t0) per epoch below the
walk rate. Between two consecutive rows of a key with epochs missing in between, both sides are
taken over the same span. From the freeze on, the clone's R is held at the value it reached at
the freeze epoch; at the clone's death the held value enters the key's frozen sum like any reached
value; a reborn clone starts unfrozen and freezes by its own variance.

Both edges are events. The freeze (the wire decouples from the revision) is counted by cause: a
plateau (0 <= V(t-1) - V(t) < dW) or a rise (V(t) > V(t-1), which the smoother does not do in
exact arithmetic, so a rise is a time update or a re-attribution, read separately), with the age,
V and |R| at the freeze. The re-coupling edge is a birth after a frozen clone of the same key and
generation; it is counted with the |R| the reborn clone reaches before its own freeze.

**A model of the timing, written before the runs.** If V tracks the filtered variance of D and
that falls as c/t of run time (information on D accruing at a constant rate through the float
ambiguities' convergence), then V(t-1) - V(t) is about V/t per epoch and the freeze arrives at a
run time t* = (c / dW)^(1/2), whatever the clone's age: a clone born late on a converged D shares
the network's information rate. With card PVN's stated sd of D at hour 2 on the exact posture
(rms over arcs: sim 2.9 cm, field 11 cm; p50 4.4 and 14 cm) and dW = 2.9e-8 m2/s x 30 s =
8.7e-7 m2, t* is about 4 to 6 h on the sim and 15 to 19 h on the field. Card PVN's sim collapse
from 28 to 2.9 cm between hours 1 and 2 is faster than 1/t, so the sim's plateau may come earlier
than this; the replay measures it.

The ladder of the freeze's scale (reads, not predictions): the same rule at V(t-1) - V(t) <
k dW for k = 10 and 100, which freezes earlier. They say what an earlier freeze from the same
estimator-internal signal would deliver, as card QPV's alpha ladder did for the fraction.

## 3. The replay

- **The instrument.** The block columns of `logs/network_pb_hold_clone.csv` gain five fields,
  written when the block row is live: `b_revd_m` (R), `b_sd_m` (S_D), `b_vd0_m2` (V),
  `b_pd_m2` (the filtered variance of D, g' Sigma g) and `b_dwd_m2` (dW this epoch, 0 with a key
  of the functional outside S). V comes from a pure helper that the augmented-filter case of card
  SMD's U0 also checks. Emit-side and trace-only.
- **U0.** The augmented-filter case extended: the helper's V equals the augmented filter's
  variance of D(t0) to 1e-8 at every epoch. The full Debug suite.
- **G0.** The instrument build re-runs `_rap_smd3_blk6_{f,s}` (6 h, the same arm): products and
  the consumer trace byte-identical, and every pre-existing column of the clone trace identical.
  Per row, |Phi_D + R - S_D| at the trace's resolution.
- **The wires.** `scripts/clock_level/rev_wire.py` rebuilds P = P_LAH - (frozen + X) on every
  held key over the phase-clock hold's wire of the same solve (`runs/_rap_lahk12_6_{f,s}`), for X
  = Phi_D (the attribution check: the block run's own wire to 1e-5 m), R unfrozen, R frozen at
  k = 1 (the member), 10 and 100, and S_D (both).
- **The censuses.** Card SMD's: the 16 field sites plus WTZZ00DEU (`census6h.py`), the 16 sim
  stations (`simcensus6h.py`), on a renamed copy of the instrument build. The phase-clock hold's
  census is re-run on that copy as the consumer identity check (closed-loop traces byte-identical
  to `lahk12k6` and `lahk12s`) and is the reference of every paired change. The rebuilt Phi_D wire
  gets its own census, so card SMD's member is compared on the same rebuild and the same exe.

## 4. Predictions (written before the runs)

- **R1 (hour 1: the revision is the ignition gain).** Early, the smoother reads nearly all of
  the ignition convergence as revision (rho_D near 1) and card SMD passed only the walk share, so
  the unfrozen member's hour-1 gain exceeds card SMD's. Bars: the all-site median paired change
  against the phase-clock hold in hour 1 at most -0.080 m (card SMD's block member -0.070 m) and the
  C1W median at most -0.078 m (card SMD -0.068 m); on the sim, at most -0.005 m (card SMD -0.000 m).
  On the trace, at clone age 0 to 1 h, |R| p90 at least 3 times |Phi_D| p90 on both beds.
- **R2 (unfrozen, late: it passes more wander than card SMD).** The revision share stays large at
  hours of age (rho_D near 0.9 on the field at hour 3), so the unfrozen member passes more of the
  late wander than card SMD. Bars: |R| p90 above |Phi_D| p90 at every clone-age hour from the second
  on that holds at least 500 rows, on both beds; the C1W median paired change above card SMD's in at
  least 2 of hours 3 to 5 (card SMD on this rebuild, expected near +0.027, +0.053, +0.048 m); on the
  sim, above card SMD's in at least 2 of hours 3 to 5.
- **R3 (frozen: the phase-clock hold from the freeze on).** Under the freeze at k = 1, the C1W
  median paired change in each of hours 3 to 5 within +-0.005 m with at most 3 of 10 sites worse
  at 1 cm (card SMD's P2 bar), the sim's hours 3 to 6 within +-0.005 m, and the hour-1 gain kept
  (all-site at most -0.03 m).
- **E (the freeze's timing, section 2's model).** On the field fewer than half of the block rows in
  hours 3 to 5 of run time are frozen at k = 1; on the sim the median run time of the first freeze
  of the ignition clones (born in the first hour) is after hour 3. If E holds on the field, R3 is
  decided there by the timing of the freeze, not by the member's form.

**The decision rule.** R1 and R3 hold: the member is worth a build (the block clone exists; the
wire takes R and a freeze edge), its first gate the 2 h field census against this replay. R1 holds
and R3 fails because the freeze does not engage within the run: the freeze signal is the open item,
and the k ladder says whether the same signal at a larger scale serves; a scaled rule is
pre-registered on its own before any build. R1 fails: the revision is not what serves the consumer
at ignition, and the declared-record alternative (the revision and its variance emitted as a
record that a consumer applies while its own ambiguities converge) loses its premise with it.

## 5. Order

The instrument, U0 and G0; the two 6 h block arms; the wires and the attribution check; the trace
reads (R against Phi_D by clone age, the freeze's timing for E); the censuses; the grade. Nothing
is armed and no default moves.
