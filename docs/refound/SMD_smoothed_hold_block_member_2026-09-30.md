# SMD: the smoothed hold's phase-visible member on a (c, p_1, p_2) block clone

**STATUS**

- **id:** SMD
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-09-30 (the outside review's fourth note, after card SMH section 10). The smoothed member of card LAH's hold family on the ionosphere-free phase-visible clock D = c - sum_j w_j p_j: the hold passes Phi_D = D(t|t) - D(t0|t) instead of the clock's Phi, so a level walk or revision (c and every phase bias together) cancels exactly, a clock walk passes at 1 - rho_D, and a phase-datum re-realization is hidden because the clone retires at every declared step of its phase biases. D(t0|t) is carried as one coupling row on the (c, p_1, p_2) block, the band pair fixed at birth. Both members' rows are carried whenever the smoothed hold is armed, so every run reports the split of the passed motion into its level part and its phase-visible part. Predictions written before the build: on the field the level part carries the centimetres and grows with age while the phase-visible part is small and consistent; the census's hour-3 loss disappears and its hour-1 gain stays; on the sim the two members differ by the sim's small level wander.
- **source:** [SMH](SMH_smoothed_hold_clone_2026-09-29.md) sections 9 to 11; [PVN](PVN_phase_visible_nees_2026-09-29.md) sections 1 and 6; [LAH](LAH_level_aware_hold_2026-09-29.md) section 2; [CTU](CTU_coupling_time_update_2026-09-28.md) section 4 (the exact rule and its cross term); [PRM](PRM_parameter_registry_2026-09-29.md) (the q rule the split feeds); the outside review's fourth note (2026-09-30)
- **knobs / posture:** `network.emit_pb_hold_smoothed_block` (default off; requires `network.emit_pb_hold_smoothed` and everything that knob requires); S line `pbhold=4`; emit-side only, the solve byte-identical; with it off the smoothed hold's wire is card SMH's clock member unchanged; no golden, ledger or posture change
- **dates in the text:** 2026-09-30
- **hashes / run dirs:** none yet

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
