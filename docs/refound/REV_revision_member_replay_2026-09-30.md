# REV: the revision member of the hold family, replayed from card SMD's block arms

**STATUS**

- **id:** REV
- **topic:** network-vehicle
- **status:** closed
- **summary:** FALSIFIED 2026-09-30 (sections 6 and 7): R1 FAILS (field all-site hour-1 change +1.225 m against the phase-clock hold, sim +1.322 m; the bars -0.080 and -0.005 m), R2 holds, R3 FAILS, E holds. The ignition convergence is nearly all revision (10 to 50 times the walk share in p90 at every clone age), and passing it reproduces the plain hold's hour 1; a float consumer absorbs any constant phase-side value and pays for every change, so a correction of the generation-start value costs in proportion to its size, and no freeze of the ladder (k = 1, 10, 100) rescues it. The revision member is not built and the declared-record alternative loses its premise; what remains is lever 1 (the phase-visible wander at its source). A new field instrument: the revision NEES of the clones born after convergence, field 4 to 18 against the sim's 0.2 to 1.6. Nothing armed. Pre-registered 2026-09-30 (the outside review's note after card QPV, on the lever QPV section 6 names last: a member that passes the generation-start revision of an ignition clone and then holds). The fourth member of the hold family on the ionosphere-free phase-visible clock D: the wire's phase side at D(t0|t), the smoothed generation-start value, so the hold passes the revision R = D(t0|t) - D(t0|t0) where cards SMH and SMD pass the walk share Phi_D = D(t|t) - D(t0|t); frozen per clone at the first epoch the clone's own stated variance of D(t0) falls by less than the stated walk adds along D. Replayed offline from card SMD's 6 h block arms on both beds by the rebuilt-wire trick (card SMD section 7); no member is built. The existing traces carry neither R nor the clone's variance, so the clone trace gains five columns (an instrument, the products byte-identical) and the two block arms are re-run. Predictions: the hour-1 gain above card SMD's; unfrozen, more late wander than card SMD; frozen, the phase-clock hold from the freeze on. A model of the freeze's timing, written beside them, expects the freeze late on the sim and not within 6 h on the field.
- **source:** [SMD](SMD_smoothed_hold_block_member_2026-09-30.md) sections 1, 2, 7 and 8; [QPV](QPV_anchor_q_phase_visible_2026-09-30.md) section 6; [SMH](SMH_smoothed_hold_clone_2026-09-29.md) sections 9, 10 and 12 (the lifetime and the bridge); [PVN](PVN_phase_visible_nees_2026-09-29.md) section 5 (the stated variance of D); [LAH](LAH_level_aware_hold_2026-09-29.md) section 2 (the family's form); the outside review's note of 2026-09-30 (night)
- **knobs / posture:** none new; card SMD's exact-update posture with `network.emit_pb_hold_smoothed` and `network.emit_pb_hold_smoothed_block` (the block arms `_rap_smd3_blk6_{f,s}`), re-run on the instrument build; the wires are rebuilt offline from the phase-clock hold's wire of the same solve. Nothing armed; the phase-clock hold stays the reference wire; no phase-bias process noise
- **dates in the text:** 2026-09-30
- **hashes / run dirs:** exe `gnss_processor_rev1.exe` `a59231c8802a14f8` (the instrument); arms `runs/_rap_rev1_blk6_{f,s}`; wires `runs/_rpl_rev_{phi,rev,revk1,revk10,revk100,both}_{f,s}`; census keys `revlah`, `revphi`, `revu`, `revk1`, `revk10`, `revk100`, `revboth` on the field and the same with `s` on the sim, `revphisw`, `revusw`, `revk1sw` (the satellite-consistent bridge); scripts `scripts/clock_level/rev_wire.py`, `rev_grade.py`

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

## 6. The replay (2026-09-30)

Exe `gnss_processor_rev1.exe` (sha256 prefix `a59231c8802a14f8`, the instrument on commit
`151ec08`); arms `runs/_rap_rev1_blk6_{f,s}` (card SMD's `_rap_smd3_blk6_{f,s}` re-run, only the
output path changed); rebuilt wires `runs/_rpl_rev_{phi,rev,revk1,revk10,revk100,both}_{f,s}`;
census keys `revlah`, `revphi`, `revu`, `revk1`, `revk10`, `revk100`, `revboth` and their `s`
twins on the sim; scripts `scripts/clock_level/rev_wire.py` (the wires and the trace reads) and
`scripts/clock_level/rev_grade.py` (the bars).

**The instrument, U0 and G0.** Six columns, not the five of section 3: a block clone's first trace
row is one epoch after its birth, so the first update's freeze test needs V at birth, which the
sixth column (`b_vd0b_m2`) carries. U0: the augmented-filter case checks the helper's V against the
filter's variance of D(t0) to 1e-8 at every epoch, V at birth to 1e-12, and V non-increasing
(`PbHoldBlockTest` 10 of 10; the full Debug suite 2732 passed, 1 skipped). G0 against
`_rap_smd3_blk6_{f,s}`: every product record identical (field 62 322 P records, sim 45 060), the
consumer trace identical, and the 24 earlier trace columns identical row by row (60 928 and 44 324
rows). Phi_D + R = S_D to 1e-6 m (the trace's resolution) on every row; 1 row gap in 22 431 clone
steps on the field and none on the sim; the traced dW equals the increment of W to 7e-11 m2.

**The attribution and the consumer identity.** The rebuilt Phi_D wire reproduces the block run's
own wire to 1.7e-6 m on both beds (every P record). The phase-clock hold's census on the instrument
exe (`revlah`, `revlahs`) is byte-identical to `lahk12k6` and `lahk12s` at all 17 field and 16 sim
consumers, so every paired change below is the wire's alone.

**The trace: R against Phi_D by clone age** (one row per satellite and epoch, mm):

| clone age (h) | 0-1 | 1-2 | 2-3 | 3-4 | 4-5 |
| --- | --- | --- | --- | --- | --- |
| field \|R\| p50 / p90 | 20.6 / 324.8 | 133.4 / 396.8 | 207.9 / 465.6 | 172.2 / 326.2 | 104.0 / 214.2 |
| field \|Phi_D\| p50 / p90 | 2.4 / 9.4 | 7.1 / 23.7 | 12.7 / 24.8 | 8.1 / 25.3 | 8.2 / 20.1 |
| sim \|R\| p50 / p90 | 9.9 / 316.8 | 43.9 / 403.5 | 216.0 / 570.4 | 267.9 / 598.8 | 329.2 / 1205.8 |
| sim \|Phi_D\| p50 / p90 | 1.9 / 7.2 | 4.0 / 10.0 | 4.3 / 12.4 | 4.6 / 11.9 | 3.6 / 7.9 |

R1's and R2's trace bars hold by an order of magnitude: the revision is 10 to 50 times the walk
share in p90 at every age. The Phi_D rows reproduce card SMD's table. The largest revisions are the
ignition clones: on the field 9 clones born at 0.03 h of run time with a stated sd of D at birth
of 25 m (G22 L1C revised by 52 m within 5 minutes of age, 34 m at its death), on the sim 0.4 to
0.8 m. Removing each epoch's median revision over the live clones leaves the same order (sim
p90 0.3 to 1.0 m at ages 2 to 4 h), so the revision is per satellite, not a common mode.

The revision against its own variance, R^2 / (V0 - V) (a consistent smoother holds it near 1),
mean by clone age 0 to 3 h:

| | ignition clones (born in run hour 1) | later clones |
| --- | --- | --- |
| sim | 0.68, 0.85, 0.88, 0.95 | 1.36, 1.57, 0.33, 0.17 |
| field | 0.76, 2.07, 2.09, 1.51 | 4.26, 13.48, 17.85, 16.70 |

On the sim the smoother's revision is consistent with the variance it states. On the field the
clones born after convergence are revised 2 to 4 times beyond their stated variance (in sd): the
smoothing-axis face of card QPV's field excess (r_D 1.5 to 2.2 against the sim's 0.3 to 0.5), a
real phase-visible motion the walk model cannot carry, booked as revision.

**E holds.** At k = 1 the field freezes 68 of 170 clones and the sim 79 of 120, every one by a
plateau (no rise). No row is frozen before run hour 5 on the field (share of rows frozen by run
hour 0, 0, 0, 0, 0.37, 0.79; hours 3 to 5: 0.12) or before run hour 4 on the sim (0, 0, 0, 0.25,
0.86, 0.91, 0.90); 46 of the field's 49 and 29 of the sim's 40 ignition clones never freeze. The
clones that freeze early in their life are late-born ones on a converged D (age at the freeze p50
0.07 h). The model's scaling with run time holds; its field number did not: the filtered sd of D
on this posture at hour 2 is 54 mm on the field and 39 mm on the sim (p50), not card PVN's 11 to
14 cm of an earlier arm, which puts t* near 7.5 and 5.3 h against the observed onsets in hours 5
and 4. The ladder freezes earlier: k = 10 freezes 0.87 of the field's rows in hours 3 to 5, k =
100 0.96 (the ignition clones' median freeze at 0.80 h of run time). Re-coupling edges (a birth
after a frozen clone of the key) at k = 1: 39 field, 49 sim, each passing p50 2.4 and 4.3 mm
before its own freeze.

**The censuses.** Median paired change against the phase-clock hold (better / worse at 1 cm),
field all 16 sites:

| member | h1 | h2 | h3 | h4 | h5 | h6 |
| --- | --- | --- | --- | --- | --- | --- |
| card SMD (Phi_D, rebuilt) | -0.070 (11/3) | +0.011 (5/8) | +0.019 (4/9) | +0.022 (5/9) | +0.011 (6/9) | +0.005 (5/7) |
| revision, unfrozen | +1.225 (4/12) | +0.720 (0/16) | +0.310 (0/15) | +0.218 (1/14) | +0.150 (3/13) | +0.127 (1/14) |
| revision, k = 1 | +1.225 (4/12) | +0.720 (0/16) | +0.310 (0/15) | +0.218 (1/14) | +0.151 (3/13) | +0.125 (1/14) |
| revision, k = 10 | +1.225 (4/12) | +0.714 (0/16) | +0.373 (1/15) | +0.238 (1/14) | +0.155 (2/14) | +0.070 (2/14) |
| revision, k = 100 | +1.225 (4/12) | +0.708 (1/15) | +0.243 (0/15) | +0.178 (1/14) | +0.070 (1/15) | +0.039 (1/13) |
| both (Phi_D + R) | +1.200 (4/12) | +0.737 (0/16) | +0.390 (2/14) | +0.205 (1/14) | +0.123 (3/13) | +0.084 (4/12) |

The C1W sites (10): card SMD -0.068, +0.011, +0.027, +0.053, +0.048, +0.005; the unfrozen revision
+0.526, +0.668, +0.359, +0.364, +0.167, +0.127; k = 100 +0.526, +0.637, +0.198, +0.252, +0.069,
+0.036. The phase-clock hold's level by hour is 0.603 0.166 0.119 0.081 0.101 0.077 m; the unfrozen
revision's 2.825 1.121 0.438 0.366 0.228 0.230. WTZZ00DEU: phase-clock hold 0.108 0.024 0.016 0.020
0.030 0.026 m, card SMD 0.109 0.022 0.010 0.014 0.022 0.019, the unfrozen revision 18.327 3.631
0.476 0.295 0.111 0.131, k = 100 18.327 3.562 0.278 0.201 0.086 0.070.

The sim, 16 stations (the census bins from 00:00 as in cards SMD and QPV, so its hour 1 is the
run's first 29 minutes, the consumers starting at 00:31):

| member | h1 | h2 | h3 | h4 | h5 | h6 |
| --- | --- | --- | --- | --- | --- | --- |
| card SMD (Phi_D, rebuilt) | -0.000 (5/5) | +0.002 (4/7) | +0.009 (1/8) | +0.012 (3/9) | +0.009 (0/6) | +0.004 (1/4) |
| revision, unfrozen | +1.322 (0/16) | +0.569 (2/14) | +0.147 (1/15) | +0.074 (1/13) | +0.032 (0/14) | +0.015 (0/11) |
| revision, k = 1 | +1.322 (0/16) | +0.569 (2/14) | +0.147 (1/15) | +0.074 (1/13) | +0.034 (0/15) | +0.021 (1/12) |
| revision, k = 100 | +1.322 (0/16) | +0.597 (1/14) | +0.110 (1/14) | +0.024 (0/12) | +0.016 (0/9) | +0.002 (0/4) |
| both (Phi_D + R) | +1.303 (0/16) | +0.532 (1/15) | +0.156 (1/15) | +0.071 (1/14) | +0.044 (0/16) | +0.021 (0/12) |

**The verdicts.** **R1 FAILS**: the field all-site hour-1 change is +1.225 m against the bar
-0.080 m, the C1W sites +0.526 against -0.078, the sim +1.322 against -0.005; 12 of 16 field sites
and all 16 sim stations are worse in hour 1. **R2 holds** on the trace and the census (the
unfrozen member is worse than card SMD in every hour from the second on, both beds). **R3 FAILS**:
at k = 1 the C1W sites read +0.359, +0.364, +0.167 m in hours 3 to 5, the sim +0.147 to +0.021 m in
hours 3 to 6, and the hour-1 gain is a loss. **E holds** on both beds.

**The verification.** Two independent checks, each with its own code. A re-grade from the raw
consumer traces reproduces every census number above to 0.000 m and the consumer identity on all
33 consumers. A recompute of every P record of every rebuilt wire from the phase-clock hold's wire
and the clone trace matches to 0.0 on both beds; the sign of R agrees with the solve's own
posterior-minus-prior step of D (correlation 0.99999 at clone ages up to 15 minutes, where the
walk share is millimetres). The recompute found one property of the rebuild, shared with card
SMD's own wire: the bridge's frozen sum is kept per key, and on 8 field satellites the L1W key's
clone history differs from the L2W key's (a later birth, a separate generation or pair), so an
L1W + L2W user's ionosphere-free phase side mixes two frozen sums (up to 0.52 m apart under the
revision, 3.5 cm under card SMD's Phi_D). Rebuilt with every key of a satellite taking its L2W
key's value (`rev_wire.py ... satwise`, keys `revphisw`, `revusw`, `revk1sw`), the field census
moves by at most 0.001 m in any hour and family; no verdict depends on it. For card SMD's member
it remains a property to fix before any arming: the two keys a consumer combines should carry one
frozen sum.

## 7. Reading (2026-09-30)

The premise of R1 holds and its inference does not. The ignition convergence is nearly all
revision, as the review said: the revision is 10 to 50 times the walk share at every age, and
largest for the clones born at ignition. Passing it is what destroys the consumer: the member
reproduces the plain hold's hour 1 (card LAH: 2.27 m; here a site median of 2.83 m, and 2.50 m for
the member that passes both shares). The account in section 1, that the wire's phase-side value
error reaches a converging consumer's position so that tracking the producer's convergence
helps, is falsified. A float consumer's ambiguities absorb any constant phase-side value, however
far from the truth; what reaches its position is the wire's change. A correction of the
generation-start value is a change with no true motion behind it, so it costs in proportion to
its size, at every hour, and at ignition it is metres. A freeze does not rescue the member: what
passed before the freeze stays a change the consumer had to absorb, and the earliest freeze of the
ladder (k = 100, 96 % of the field's rows in hours 3 to 5 frozen) still costs +0.07 to +0.24 m in
hours 3 to 5.

The only share of the family that can serve a float consumer is the one that tracks true motion:
the walk share, to the extent that it is a real motion of the a priori clock (card SMD's field
hour-1 gain) and not the estimator's wander (its loss from hour 3 on the field, and its small cost
at every hour on the sim, whose truth has no phase-visible motion). That bounds the hold family on
this estimator: the phase-clock hold passes nothing, the walk share's value is capped by the
wander, and the revision is harmful in full.

**The decision (the pre-registered rule).** R1 fails: the revision member is not built, and the
declared-record alternative (the revision and its variance emitted for a consumer to apply while
its own ambiguities converge) loses its premise with it, since every consumer of these censuses is
converging in hour 1 and pays most there. What remains is lever 1, the phase-visible wander at its
source (the estimator's joint covariance), which now has a second truth-free field instrument
beside card QPV's r_D: the revision NEES of the clones born after convergence (field 4 to 18, sim
0.2 to 1.6). Nothing is armed, no default moves, and the phase-clock hold stays the reference wire.
