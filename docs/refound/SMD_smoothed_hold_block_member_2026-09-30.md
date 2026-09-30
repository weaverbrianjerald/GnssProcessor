# SMD: the smoothed hold's phase-visible member on a (c, p_1, p_2) block clone

**STATUS**

- **id:** SMD
- **topic:** network-vehicle
- **status:** open
- **summary:** BUILT 2026-09-30, default off, NOT armed (sections 7 and 8). U0 (10 cases) and G0 pass. P1 FAILS: the level part of the passed motion is zero at every clone age on both beds (the phase biases are random constants of the model, so the smoother reads every level move as a revision; card SMH's clock member was already level-blind) and the phase-visible part carries it all (field p90 9 to 25 mm, r_D 1.5 to 2.2 after the first hour; sim p90 7 to 12 mm at r_D 0.3 to 0.5, the estimator's own wander where the truth has none). P2 FAILS: the census keeps the hour-1 gain (all-site -0.070 m) and the C1W loss from hour 3 (+0.027, +0.053, +0.048 m), the block being card SMH's member with a shorter life. P3 holds. The live consumer keeps its gain (phase-visible). The attribution check found a defect of card SMH's bridge (the epoch's clock step dropped at every switch, up to 1.6 m), fixed. What follows is card PRM's q rule on the phase-visible part, against the sim's floor. Pre-registered 2026-09-30 (the outside review's fourth note, after card SMH section 10). The smoothed member of card LAH's hold family on the ionosphere-free phase-visible clock D = c - sum_j w_j p_j: the hold passes Phi_D = D(t|t) - D(t0|t) instead of the clock's Phi, so a level walk or revision (c and every phase bias together) cancels exactly, a clock walk passes at 1 - rho_D, and a phase-datum re-realization is hidden because the clone retires at every declared step of its phase biases. D(t0|t) is carried as one coupling row on the (c, p_1, p_2) block, the band pair fixed at birth. Both members' rows are carried whenever the smoothed hold is armed, so every run reports the split of the passed motion into its level part and its phase-visible part. Predictions written before the build: on the field the level part carries the centimetres and grows with age while the phase-visible part is small and consistent; the census's hour-3 loss disappears and its hour-1 gain stays; on the sim the two members differ by the sim's small level wander.
- **source:** [SMH](SMH_smoothed_hold_clone_2026-09-29.md) sections 9 to 11; [PVN](PVN_phase_visible_nees_2026-09-29.md) sections 1 and 6; [LAH](LAH_level_aware_hold_2026-09-29.md) section 2; [CTU](CTU_coupling_time_update_2026-09-28.md) section 4 (the exact rule and its cross term); [PRM](PRM_parameter_registry_2026-09-29.md) (the q rule the split feeds); the outside review's fourth note (2026-09-30)
- **knobs / posture:** `network.emit_pb_hold_smoothed_block` (default off; requires `network.emit_pb_hold_smoothed` and everything that knob requires); S line `pbhold=4`; emit-side only, the solve byte-identical; with it off the smoothed hold's wire is card SMH's clock member unchanged; no golden, ledger or posture change
- **dates in the text:** 2026-09-30
- **hashes / run dirs:** exe `gnss_processor_smd1.exe` `ac1916faa9e296ae` (G0, the defective bridge), `gnss_processor_smd2.exe` `943bf2d51913cf5e` (the bridge fixed), `gnss_processor_smd3.exe` `51635d166c00033f` (the D-step counter; products identical to smd2); arms `runs/_rap_smd1_{ex,blk}_{s,f}`, `runs/_rap_smd1_ex6_f`, `runs/_rap_smd1_blk6_{f,s}`, `runs/_rap_smd2_{ex,blk}_{s,f}`, `runs/_rap_smd2_{ex6,blk6}_{f,s}`, `runs/_rap_smd3_blk6_{f,s}`; census keys `smd1b6`, `smd1bs` (defective wire), `smd2c6`, `smd2b6`, `smd2cs`, `smd2bs`; session scripts `smd_split.py`, `smd_ctlwire.py`, `census6h.py`, `simcensus6h.py`, `smh_g0.py`

## 1. The member

Three kinds of motion reach a satellite's clock and phase biases: a clock walk (c moves, the phase
biases do not), a level walk (c and every phase bias move together, which the phase does not see)
and a phase-datum re-realization (the phase biases move, c does not). The wire's phase side should
pass the first, never the second, and hide the third. Card SMH's clock member passes the clock's
smoothed increment, which cannot separate the first two (both move c); its field census lost from
hour 3 at the C1W sites by exactly its passed motion (SMH section 10), which grows with the clone's
age at about twice its stated walk (SMH section 11).

The phase-visible member keeps the hold family's form (card LAH section 2):

(C - P_j)(t) = (c - p_j)(t0|t0) + Phi_D(t),   Phi_D(t) = D(t|t) - D(t0|t),

with D = c - sum_j w_j p_j over one phase bias per band, w the minimum-norm weights with
sum w = 1 and sum w lambda^2 = 0 (GPS L1 / L2: 2.5457 / -1.5457; `clockAnchorPhaseClockWeights`).
It is written as card LAH's transfer minus Phi_D accumulated frame-free, card SMH section 10's
frame, so the phase-clock member (Phi = 0), the clock member and this one differ only by what
they pass. On the ionosphere-free combination the wire is C - sum_j w_j P_j = D(t0|t0) + Phi_D:
the estimator's current ionosphere-free phase bias plus the smoother's revision of D(t0). Per
band, the hold keeps the generation-start difference between the bands, as every member does.

- A level revision or a level walk moves c and every p_j by the same amount, and sum w = 1: D
  does not move, Phi_D does not move.
- A clock walk moves D; the fixed-point smoother splits it between the revision of D(t0) and the
  walk by rho_D = P_D / (P_D + sum q_D), so a fraction 1 - rho_D passes.
- A re-realization of a phase bias in the functional moves D(t|t). It is hidden only if D(t0|t)
  follows the same step (section 3). A re-realization of a phase bias outside the functional
  does not reach Phi_D at all.

## 2. The clone

D(t0|t) is carried as one row in card SMH's coupling-row convention, born at a stash with the
terms {c: M = -1, p_a: M = +w_a, p_b: M = +w_b} (so x0 = c - w_a p_a - w_b p_b), the value
D at the stash and sigma^2 = 0. Card CTU's exact rule is linear in the row's coefficients, so this
row is the functional of the (c, p_a, p_b) block of three clones: its value is the combination of
theirs, and its time-update variance is the block's with every cross term,
sum_{a,b} g_a g_b M_a' B M_b (`couplingExactCross`). The value update, the re-harvest, the
pinned-pivot rule and the death on a term that would leave the exact route are card SMH's,
applied to the three keys of the functional (any of them outside the dense block kills the
clone, as the clock alone does now). The passed motion is accumulated frame-free (SMH section 10):
each epoch the functional's step against the prior means the solve folded, minus the revision of
x0.

**The pair is fixed at birth.** On the field L1C and L1W are separate estimator states fed by
disjoint station sets (each station latches one L1 signal), with datums hundreds of metres apart
(card PVN section 5); card PVN's instrument re-chooses the best-determined signal of a band every
epoch (64 changes in 2 h on the field's exact-update arm), which a clone would read as a step of
w_1 (p_L1C - p_L1W). At birth the clone takes, per band, the satellite's stashed phase bias with
the smallest stashed variance (PVN's rule read once) and keeps it for its life. Every key of the
satellite born at one stash takes the same pair: the L2W key has no L1 partner of its own, and one
functional per satellite gives every consumer, C1C or C1W, the same passed motion on its
ionosphere-free combination.

**Both members are carried.** Whenever `network.emit_pb_hold_smoothed` is armed, each hold key
carries card SMH's clock clone and a block clone, in two carry maps with their own births and
deaths; `network.emit_pb_hold_smoothed_block` selects the block's D row for the wire. The block
clone carries two rows on one lifetime, its own clock row and the D row, so the split is always
read on a shared birth: the level part Phi_c - Phi_D = sum_j w_j Phi_pj and the phase-visible part
Phi_D. The block's clock row also gives the control for the block's shorter lifetime (section 3):
a preview wire written with the block's Phi_c in place of Phi_D (card SMH section 10's replay)
separates what the functional removes from what the retirements remove.

## 3. The dependence: the phase datum's steps

The member depends on one clause: its phase-bias terms follow the phase datum's steps. On the
golden posture (the anchored election with the held basis and its cold release, the float
vehicle, `phase_anchor_carry_reref`) no phase-bias mean is ever transformed in place with a
declared value (the float vehicle's declared S-transform, the eq34 shift and card CLV's re-base
are all unarmed; the clock-pivot S-transform moves clocks only). A phase bias steps in three ways,
all declared, none with a value:

- A *re-reference* (`phase_anchor_reref`: a cold handover, an incumbent-less pin, a coverage-gate
  drop). The fold withholds the key's prior for that epoch, so no frame-free step exists for the
  key. A cold handover is marked on the wire at the same epoch (a retro bump; the declaration's
  `marked=1`).
- A *new anchor tenure* of the family (`phase_basis`, elected this epoch). A continuous handover
  keeps the key folded at its old mean and pins the successor at its ledger value, whose
  conditional variance may reach 0.01 m2, so the family can step by up to about 0.1 m with the
  step's value undeclared.
- An *emitter mark* on the satellite's phase-bias channel. Under the golden's per-satellite mark
  scope the marks are a superset: a clock-only or contributor-set mark (51 of the 61 retro bumps
  of the 6 h field run are the gross line-of-sight screen's) bumps the phase channel too.

**The rule.** The D row follows a step by retiring at it: at a re-reference or a new tenure of
either pair key (both known before the emit) and at an emitter mark on the satellite's phase-bias
channel (known at the hold), the block clone dies, the key bridges on the phase-clock transfer
from its last value for that epoch (the Phi_D the clone had reached stays in the key's transfer,
as at a withdrawal, SMH section 9), and the birth is queued for the same epoch's stash, on the new
frame. A pair key with no column or no folded prior (a departure, a skipped fold) retires the
clone the same way. The 6 h field run of card SMH (`_rap_smh5_ex6_f`) has 157 continuous and 12
cold handovers, 13 gate drops, 139 re-referenced family-epochs at the run end (so about 114
incumbent-less pins, most on a key without a stashed row) and 61 retro bumps, so the block lives
shorter than the clock clone; the lifetime control of section 2 measures what that costs.

**The counters.** Steps followed, by cause (a re-reference, a new tenure, cold or continuous, a
mark); re-references whose satellite carried no emitter mark that epoch; and steps unmarked: a
pair phase bias whose step (its posterior minus its folded prior mean) exceeds the emitter's own
jump bar (`discThresholdM`, 0.25 cycle) on a live block clone, which the clone passes as motion,
with the largest. The last is the clause's failure count.

## 4. Instruments

Per smoothed held emission (the trace `network_pb_hold_clone.csv`, extended): the member writing
the wire, Phi_c, Phi_D, the level part on a shared birth, the pair, W_D (the fold inflation along
D, g'Qg per epoch with the seam's off-diagonal noise), V_D (the posterior variance of the
increment) and r_D = Phi_D^2 / (W_D - V_D). Run end: births deferred for want of a pair, deaths by
cause, the step counters, |Phi_D| and the level part's p50 / p90, and r_D by hour.

## 5. Gates and predictions (written before the build)

- **U0.** The D row against an augmented filter carrying D(t0) as a zero-noise state (value and
  variance at every epoch to 1e-8); the D row equals the combination of the three single-key rows,
  its variance the block's through `couplingExactCross`; a level step of c and both phase biases
  passes nothing and a clock step passes; the retirement at a re-reference, at a new tenure and
  at a mark, the bridge continuous, the rebirth on the new frame; the pair fixed at birth; the
  loader and `pbhold=4`; the full suite.
- **G0.** `network.emit_pb_hold_smoothed_block` off: products identical to card SMH section 10's
  clock member (`runs/_rap_smh5_ex_{s,f}`), both beds, 2 h. On: the solve identical (every record
  but P).
- **P1 (the field's split, 6 h).** The level part carries the centimetres and grows with age; the
  phase-visible part is small and consistent. Bars, on the block clones' own ages (one row per
  satellite and epoch, every clone-age hour from the second on that holds at least 500 rows): the
  level part's |Phi_c - Phi_D| p90 rising with age and at least twice |Phi_D| p90; at least 2 cm in
  the oldest such hour; the mean of r_D after the first hour of age within 0.3 to 3. The block
  lives shorter than the clock clone (section 3), so the oldest hours may be empty; the bars read
  the hours that exist.
- **P2 (the field census, 6 h, the 16 sites of card SMH section 10, against the phase-clock hold).**
  The hour-3 loss disappears and the hour-1 gain stays. Bars: the C1W median paired change at most
  +0.005 m in each of hours 3 to 5, with at most 3 of 10 sites worse at 1 cm; the all-site median
  paired change in hour 1 at most -0.03 m. The lifetime control (the block's Phi_c on the same
  retirements) is graded beside it: the functional is credited with the part of the change the
  control does not reproduce. The live consumer (WTZZ00DEU) is read against both members: its
  gain survives if it was phase-visible and goes if it was a compensated local error.
- **P3 (the sim).** The two members differ by the sim's level wander, which is small. Bars: sim
  |Phi_c - Phi_D| p90 at most 1 cm at every clone age; SZZ000SIM and the 16-station sim census of
  card SMH section 11 within 0.005 m of the clock member in every hour after hour 1.
- **The decision rule.** If P1 fails with the phase-visible part carrying the centimetres, the sim
  decides: a sim |Phi_D| p90 at most 5 mm (the sim's truth has no phase-visible motion, its a
  priori clock being the truth) makes the field's Phi_D the a priori clock's error, which the
  member passes by design (card PBH read 3 measured the field clock wandering 2.7 cm within an
  arc); a sim Phi_D of centimetres makes it a phase-side drift of the estimator.
- **What follows.** The anchor's q is derived from the phase-visible part, and the level part is
  read against the code-bias and receiver code-bias carries (card PRM's q rule, with the split as
  its input).

## 6. Order

Build and U0; G0 at 2 h on both beds; the 6 h field arm with its split (P1) and census (P2); the
6 h sim arm with SZZ000SIM and the sim census (P3). Arming is the author's decision.

## 7. Build, U0, G0 and a defect of the bridge (2026-09-30)

**Build.** `NetworkPbHoldClone.{hpp,cpp}`: the row advance generalized to a functional
(`pbHoldCloneAdvanceRow`, card SMH's clock row being the clock alone with g = 1, its arithmetic
unchanged), `pbHoldClonePickPair` (per band the phase bias with the smallest stashed variance,
the weights of `clockAnchorPhaseClockWeights`), the block clones in `ctx.carry.smh_block` with
their own pending births, the engagement test shared with card SMH, the follow pass before the
exact route (a re-reference in `phase_anchor_reref` or `eq34_reref`, a new tenure in
`phase_basis`) and `networkPbHoldBlockAtHold` at the hold (the emitter's phase-channel mark read
through `pendingMarkChannels`, the unmarked steps, the split). Deaths `followed` and `pair_lost`
queue the birth again, like a withdrawal. `network.emit_pb_hold_smoothed_block` (loader refusal
without the smoothed hold), `pbhold=4` on the S line (read back as the smoothed hold and its
block member), the trace `network_pb_hold_clone.csv` extended by the writing member and the
block's columns, and a run-end line. The emitter's `discThresholdM` became public for the
unmarked-step test.

**U0.** 10 cases in `tests/test_pb_hold_block.cpp`: the phase-visible row against an augmented
filter carrying D(t0) with a level walk in the truth (value, variance and the increment's
variance to 1e-8 at every epoch, and g'Qg as the fold's inflation); the row as the functional of
the three single-key rows, its variance the block's through `couplingExactCross` and its
inflation with the seam's cross term; the pair (per band, ties, one band refused, the weights
blind to the level and the ionosphere); the retirement at a re-reference and at a new tenure; the
birth with its pair from the stash (one band waits); the wire taking Phi_D and a mark retiring the
block continuously; the switch to the bridge keeping the epoch's clock step (section 7's
defect); the clock member ignoring the block on the wire; the loader; the contract. Full Debug
suite 2731 passed, 1 skipped.

**G0** (exe `gnss_processor_smd1.exe`, sha256 prefix `ac1916faa9e296ae`). The block knob off: the
products and the consumer trace byte-identical to card SMH section 10's clock member
(`_rap_smd1_ex_{s,f}` against `_rap_smh5_ex_{s,f}` at 2 h, `_rap_smd1_ex6_f` against
`_rap_smh5_ex6_f` at 6 h). On: every record but P identical, P in value only, the S line
`pbhold=4` (`_rap_smd1_blk_{s,f}`).

**A defect of card SMH's bridge, found by the attribution check.** Rebuilding the block wire from
the phase-clock hold's (P = P_LAH - frozen - Phi_D, the frozen sum being the Phi each earlier
clone of the key reached) missed the block's own wire by up to 1.6 m on the field. At the epoch a
clone died, the hold re-referenced card LAH's transfer on the transfer it had last written, which
dropped that epoch's LAH step: C - P stepped by the satellite's de-commoned clock step at every
switch to the bridge: 6.3 cm at G15's cold handover on the sim, and 1.6 m on the field where an
emitter mark (a 1.62 m per-satellite re-realization of G07's emitted clock, `CLK_STEP`) retired
G07's block. The fix
folds the passed motion the clone reached into LAH's reference, so the epoch's step is
transferred as LAH transfers it; only a key that never had an LAH reference still takes it from
the last transfer. After it (exe `gnss_processor_smd2.exe`, `943bf2d51913cf5e`) the rebuilt wires
match the block wires to 2e-6 m on both beds (the trace's resolution) and the clock member's to
1e-6 m. Card SMH's clock member is unchanged at 2 h on both beds (no bridge in 2 h,
`_rap_smd2_ex_{s,f}`) and moves at its 6 h field bridges by at most 5.3 mm (12 542 P records,
`_rap_smd2_ex6_f`); its census re-graded on the fixed wire is section 10's to a millimetre except
hour 6 (median 0.092 against 0.088 m, the C1W change +0.005 against +0.006), and the live
consumer's hour 6 improves (0.023 against 0.029 m). The block member's two censuses on the
defective wire are recorded in section 8 for completeness.

**The retirements and the dependence's counters** (6 h, `_rap_smd3_blk6_{f,s}`, exe
`gnss_processor_smd3.exe` `51635d166c00033f`, products byte-identical to `smd2`'s). Steps followed:
field 26 re-references, 228 new anchor tenures and 159 emitter marks (sim 10, 94 and 0); 9
re-references on the field carried no emitter mark (sim 0), which the hold hides by the
retirement. The unmarked-step count as pre-registered (a pair phase bias's own step against the
emitter's bar) read the level updates, which move each phase bias with the clock (5 564 on the
field, the largest 14.5 m); a key's phase-visible step c - p_j still carries the ionosphere
direction (5 820). It now reads the step of D itself against the pair's smallest bar: field 1 570
per held key, 1 542 of them in the first hour (D's convergence at ignition, the largest 24 m),
then 8, 14, 3, 0 and 3 by hour; sim 582, all in the first hour. After the first hour no live
clone's Phi_D moved by more than 2 cm in one epoch on the field: those steps were revisions of
D(t0), not passed motion. The clause the member depends on holds after ignition on both beds.

## 8. The split, the censuses and the reading (2026-09-30)

**P1 FAILS: the level part is zero.** The block clones' split by their own age, one row per
satellite and epoch (6 h, `_rap_smd2_blk6_{f,s}`); level part |Phi_c - Phi_D| and Phi_D p50 / p90 in
mm, and the mean of r_D:

| clone age (h) | 0-1 | 1-2 | 2-3 | 3-4 | 4-5 |
| --- | --- | --- | --- | --- | --- |
| field rows | 13 089 | 5 459 | 2 455 | 842 | 173 |
| field level part | 0.0 / 0.0 | 0.0 / 0.0 | 0.0 / 0.0 | 0.0 / 0.0 | 0.0 / 0.1 |
| field Phi_D | 2.4 / 9.4 | 7.1 / 23.7 | 12.7 / 24.8 | 8.1 / 25.3 | 8.2 / 20.1 |
| field r_D | 6.43 | 2.17 | 1.46 | 0.75 | 0.33 |
| sim rows | 11 501 | 5 978 | 2 835 | 1 665 | 183 |
| sim level part | 0.0 / 0.0 | 0.0 / 0.0 | 0.0 / 0.0 | 0.0 / 0.0 | 0.0 / 0.0 |
| sim Phi_D | 1.9 / 7.2 | 4.0 / 10.0 | 4.3 / 12.4 | 4.6 / 11.9 | 3.6 / 7.9 |
| sim r_D | 1.08 | 0.49 | 0.36 | 0.27 | 0.08 |

The block's clock row and its phase-visible row pass the same motion to a tenth of a millimetre.
The reason is the estimator's model, not the data: the satellite phase biases are random
constants (`sat_phase_bias_q` 1e-12 m2/s), so their fixed-point smoothed value at t0 is their
filtered value at t, Phi_pj = 0, and Phi_D = Phi_c - sum_j w_j Phi_pj = Phi_c. A move of the
level (the clock and every phase bias together) is read by the smoother as a revision of the
constants, and the phase-precise correlation between the clock and its phase biases carries the
same revision to c(t0|t). The model has no level walk, so the smoother cannot pass one: card
SMH's clock member was already blind to the level, and section 11 of card SMH (the passed motion
read as mostly level) does not hold for this estimator. What both members pass is the motion of
the phase-visible clock D.

**The decision rule: the sim's phase-visible part is not near zero.** The sim's truth has no
phase-visible motion (its a priori clock is the truth, its phase biases constant), and its Phi_D
reaches p90 7 to 12 mm at r_D 0.3 to 0.5 after the first hour: above the 5 mm bar, so the passed
motion includes the estimator's own phase-visible wander (card PVN measured the sim's D wandering
2 to 5 cm within an arc), passed at the anchor's stated walk. The field's Phi_D is about twice the
sim's (p90 9 to 25 mm) at r_D 1.5 to 2.2 in the second and third hour of age: the sim's floor and
more, consistent with a real error of the a priori clock on top (card PBH read 3) but not
separated from it by this card.

**P2 FAILS: the hour-3 loss stays.** Census of card SMH section 10 (16 sites plus WTZZ00DEU, PBF
on), on the fixed build (`smd2c6` the clock member, `smd2b6` the block member), median of the
site p50 by hour (m) and the median paired change against the phase-clock hold (better / worse at
1 cm):

| wire | h1 | h2 | h3 | h4 | h5 | h6 |
| --- | --- | --- | --- | --- | --- | --- |
| phase-clock hold (`lahk12k6`) | 0.603 | 0.166 | 0.119 | 0.081 | 0.101 | 0.077 |
| clock member (`smd2c6`) | 0.606 | 0.163 | 0.106 | 0.102 | 0.099 | 0.092 |
| block member (`smd2b6`) | 0.605 | 0.155 | 0.113 | 0.104 | 0.098 | 0.081 |
| block, C1W change | -0.068 (7 / 2) | +0.011 (3 / 5) | +0.027 (2 / 6) | +0.053 (2 / 8) | +0.048 (2 / 7) | +0.005 (2 / 4) |
| block, C1C change | -0.075 (4 / 1) | +0.015 (2 / 3) | +0.004 (2 / 3) | -0.011 (3 / 1) | -0.026 (4 / 2) | -0.006 (3 / 3) |

The hour-1 gain stays (all-site median -0.070 m, 11 better and 3 worse); the C1W loss of hours 3 to
5 does not disappear (bar +0.005 m), and is a little larger than the clock member's (+0.015,
+0.043, +0.025 m). The lifetime control needs no separate wire: the block's clock row passes Phi_D
to 0.1 mm, so the control is the member, and the whole difference to the clock member is the
block's shorter life (614 births against 198 in the 6 h field run, 503 of them within a
generation; 16 % of the block's rows are older than two hours against 43 % of the clock
clone's). On the defective wire (`smd1b6`, `smd1bs`) the block member read hour 1 -0.057 and hours
3 to 5 +0.028, +0.044, +0.041 m at the C1W sites.

The live consumer keeps its gain: WTZZ00DEU 0.109 0.022 0.010 0.014 0.022 0.019 m against the
phase-clock hold's 0.108 0.024 0.016 0.020 0.030 0.026 (the clock member 0.108 0.022 0.010 0.016
0.022 0.023). By the pre-registration its gain was phase-visible.

**P3 holds.** The two members differ by nothing along the level (sim level part 0.0 mm at every
age); the 16-station sim census of the block member is within 5 mm of the clock member's in every
hour (0.442 0.130 0.075 0.064 0.051 0.048 against 0.442 0.133 0.075 0.067 0.049 0.046 m; paired
change against the phase-clock hold -0.000, +0.002, +0.009, +0.012, +0.009, +0.004); SZZ000SIM
alone differs by 1.1 cm in hour 6 (0.048 against 0.037 m), a retirement's timing on one consumer.

**Reading.** On this estimator the smoothed hold passes only phase-visible motion, whichever
member is armed: the level is a constant of the model and every level move is a revision. The
field's loss from hour 3 is therefore not a level leak; it is phase-visible motion passed at the
anchor's stated walk, of which the sim shows a floor of estimator wander (about 1 cm p90) that the
consumer pays for (+0.8 to +1.2 cm in hours 3 to 5 on the sim census), and the field about twice as
much. The block member is not armed; it is card SMH's member with a shorter life on this model.
What follows is card PRM's q rule with the split as its input, now read as all phase-visible: the
anchor's q set against the sim's floor (the estimator's own wander, which a matched q would pass
as motion) and the field's excess over it (the candidate a priori error), not against r as a
whole. The phase biases' own walk (a model with a level walk) is a separate estimator question,
not a hold one.
