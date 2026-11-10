# SMH: the smoothed phase-bias hold on a carried clock clone

**STATUS**

- **id:** SMH
- **topic:** network-vehicle
- **status:** closed
- **summary:** BUILT, NOT ARMED: the field census gains in hour 1 and loses at the C1W sites from hour 3 (median +1.5 to +4.3 cm); the phase-clock hold stays the wire.
- **source:** [LAH](LAH_level_aware_hold_2026-09-29.md) sections 2, 7 and 10; [PVN](PVN_phase_visible_nees_2026-09-29.md) section 6; [CTU](CTU_coupling_time_update_2026-09-28.md) sections 4, 9.1 and 13.1; [CTU2](CTU2_level_recursion_followup_2026-09-29.md) sections 8 and 10; [PBH](PBH_phase_bias_hold_2026-09-19.md); the outside review's two notes of 2026-09-29 (the author's session): the clone's datum rule and the sharpened gates
- **knobs / posture:** `network.emit_pb_hold_smoothed` (default off; requires `network.emit_pb_hold`, `network.carry_coupling_exact_update`, `network.raim_within_epoch` (its fold capture, from section 8) and `network.clock_temporal_anchor` without `network.clock_model` (section 9), all refused at load time on resolved values; excludes `network.emit_pb_hold_phase_clock`, whose transfer it carries as its fallback; refuses what LAH refuses); S line `pbhold=3`; emit-side only, the solve byte-identical; section 9 moved the config default of `network.dense_fold_kappa` (card CTU2) from 1e-6 to 1e-12, byte-identical on the golden posture (2 h, both beds); no golden, ledger or posture change
- **dates in the text:** 2026-09-29, 2026-09-30
- **hashes / run dirs:** section 12 exe `gnss_processor_smd2.exe` `943bf2d51913cf5e`, arms `runs/_rap_smd2_ex_{s,f}`, `runs/_rap_smd2_ex6_{f,s}`, census keys `smd2c6`, `smd2cs`; section 11 arms `runs/_rap_smh5_ex6_s`, `runs/_rap_lahk12_6_s` (exe `gnss_processor_smh5.exe`), sim census keys `lahk12s`, `smh5s` (tags `runs/_rap_smc6_*`); exe `gnss_processor_smh1.exe` `3338a185a7c104c7` (section 7), `gnss_processor_smh2.exe` `bf8af85637d573de` (section 8), `gnss_processor_smh3.exe` `095a55612d6ffed8` and `gnss_processor_smh4.exe` `9e33142f0b352c5c` (section 9), `gnss_processor_smh5.exe` `4b4b9b468d590c47` (section 10); section 10 arms `runs/_rap_smh5_{off6,off,ex,k6}_{s,f}`, `runs/_rap_smh5_ex6_f`, `runs/_rap_lahk12_6_f`, `runs/_rap_g3{a,b}_smh5`, previews `runs/_rpl_lahphi_*`, census keys `smh5P`, `lahk12P`, `smh5k6`, `lahk12k6`; section 9 arms `runs/_rap_smh3_{off,ex,k12,k12lah}_{s,f}`, `runs/_rap_smh4_ex_{s,f}`, `runs/_rap_gk6_*`, `runs/_rap_gk12_*`, `runs/_rap_gk_d4_*`, G3 `runs/_rap_g3a_{smh,lah,nohold}`, `runs/_rap_g3b_{smh,nohold}` on the product `runs/_g3_data`, replays `runs/_rpl_smh3k12_*`, `runs/_rpl_smh3ex_*`; arms `runs/_rap_smh_{off,ex,lah}_{s,f}`, `runs/_rap_smh2_ex_{s,f}`; replays `runs/_rpl_smh_*`

**Outcome.** Question: can a hold passing the clock's smoothed motion match card LAH's phase-clock hold at the consumer while passing a modelled walk? G1 holds at the historical fold bound. On G3 the member passes 97 % of an injected walk, but the pre-registered f rho_D prediction FAILS (0.41 predicted, 0.026 measured); card [SFR](SFR_sim_fixture_coloured_phase_2026-10-01.md) supersedes the sim G2 and G3. Card [SMD](SMD_smoothed_hold_block_member_2026-09-30.md) found the passed motion wholly phase-visible; card [SMC](SMC_smoothed_hold_common_step_removed_2026-10-01.md) closed the hold family on this estimator. Cards [SCL](SCL_self_contained_clock_level_2026-10-03.md) and [NBP](NBP_native_broadcast_clock_posture_2026-10-07.md) reuse the member with a broadcast a priori clock.

## 1. The member

Card LAH section 2: every hold with C(t) = c(t|t) and (C - P_j)(t) = (c - p_j)(t0|t0) + Phi(t) is
level-blind. The smoothed member takes Phi(t) = c(t|t) - c(t0|t), the fixed-point smoothed clock
motion since t0. Its phase side keeps the generation-start phase-visible clock plus the motion the
model attributes to the clock walk; a revision of c(t0) (the level, under the exact update)
reaches P_j, so it stays off the phase side. LAH rejected it on a premise card PVN refuted (the
phase-visible clock D was taken as stated to millimetres): with the stated sd of D in centimetres
to decimetres the smoother's regression of D(t0) on D(t), P_D / (P_D + sum q), stays near 1 for
hours, so on these beds the member should equal the phase-clock hold (Phi = 0), while it passes a
modelled walk the phase-clock hold would hide.

## 2. The clone

c(t0|t) is carried as a row with no observation rows, in the coupling-row convention
x0 = value - sum_j M_j (x_j - s_prev_j) with residual variance sigma2:

- **Birth.** The generation starts at emit t0; the border is stashed after emit in the same epoch
  (`networkStashStatus`, after `networkEmitStore`), so the clone is materialized at that stash:
  one term on the satellite's clock with M = -1, s_prev and p_prev from the stash, value = the
  stashed clock, sigma2 = 0, stamp = the stash time. c(t0|t0) is that value. A clone stamped at
  any other stash is a different hold.
- **Time update.** Each later epoch, before emit, the row takes card CTU's exact route over the
  epoch's inputs (`couplingExactRow` on `cxu_epoch`): S terms updated by the one rule, D terms
  (departed, reinit, re-referenced) re-attributed through P, the clock-family deficit to the
  stash's pinned pivot, the seam's off-diagonal noise through `q_off`; terms on keys outside the
  dense block take the fold's own kappa (exact for a diagonal key).
- **Value.** The row has no observations, so the measurement update leaves the conditional on
  the border unchanged and the value moves only by the border's revision:
  x0(t|t) = x0(t-1|t-1) - sum_S m_s [x_s(t|t) - prior_s(t)], with prior_s the prior mean the
  solve used for that column (the stash value after any in-place frame transform). A pinned
  column and the deficit's pivot contribute no revision. A declared frame step (a pivot handover,
  an a priori re-level, a constellation common mode) therefore never steps the clone and lands on
  C alone; the one frame step that is a revision, card CLV's discrete re-base, is refused.
- **Re-harvest.** At the stash the row is re-expressed on the new stash keys (s_prev and p_prev
  from the stash, the pinned pivot's term kept at p_prev = 0), and its stamp set to the stash
  time.
- **No fallback.** A clone that would take card 1d's route (an unfactored epoch, a stale stamp,
  its own clock term outside the dense block) is destroyed, never shrunk (the scalar kappa on
  the clock is 3.3e-4). Its key reverts to the phase-clock transfer of card LAH for the rest of
  the generation, continuous at the switch, counted.
- **Housekeeping.** None of the ambiguity responses apply (the elapsed-time prune, the declared
  handover shift of float ambiguities, the re-reference erase, slip and refloat), so the clone
  lives in its own carry site (`ctx.carry`), not in a station's local carry.
- **Keying.** One clone per hold key (satellite, signal), lifetime equal to the key's
  generation; keys of one satellite that start together carry identical rows.

The row form is exactly the first stage of a clone column in the estimator (a zero-noise column
whose only information is its prior correlation with the border): both condition c(t0) on the
border and nothing else. It needs no column, no rank-gate mask and no fold clause. The augmented
previous-border block of the joint-covariance work needs clone columns with tie rows in the solve;
what it shares with this card is the exact-update row code and the carry site, not the vehicle.

## 3. The hold

`pbhold=3`. At a generation start the key queues its clone's birth. At a held emission with a
live, updated clone the transfer is x0(t|t) - c(t0|t0) (the clone's revision since birth), on
top of the held value; with the clone dead the key carries card LAH's transfer from the switch.
LAH's common-mode median and coupled-mark transfer are not used by this member while its clone
lives. The consumer is unchanged (a held P is a held P); the reader recognizes `pbhold=3` and the
contract check compares it.

## 4. Instruments

Per clone at each held emission, in the solve frame:

- Phi = c(t|t) - x0(t|t), the motion the phase side passes; the transfer x0(t|t) - c(t0|t0);
  LAH's transfer computed in the same pass, and their difference.
- W = sum of the fold inflations q applied to the parent clock since birth (the anchor's floor,
  loose epochs and the seam included).
- V_post = sigma2 + (e_c + m)' P(t|t) (e_c + m), the posterior variance of the increment
  c(t) - c(t0), from the row and the solve's covariance.
- r = Phi^2 / (W - V_post): the passed motion against its stated variance. Under a consistent
  covariance r averages 1; it reads the actual-to-stated variance ratio of D on the smoothing
  axis (card PVN on another axis).

Run-end: births, deaths by cause, reverts, the mean r per constellation and hour, the rms of the
transfer and of its difference to LAH's. Trace `logs/network_pb_hold_clone.csv`.

## 5. Gates

- **U0.** Unit gates: the clone row's value against a batch fixed-point smoother (the augmented
  state [x(t), c(t0)] of a small dense walk model) at every epoch to 1e-9, with a reinit and a
  departure of the parent; a frame step of the prior means leaves the value unchanged; the
  revert is continuous; the loader's requirements; the wire contract. Full suite.
- **G0.** Knob off: products and consumer traces identical to the pre-card build, both beds.
  Knob on: the solve identical (every record but P, and P only on held keys).
- **G1 (consumer).** 2 h on both beds, the exact update on: consumer 3D p50 by hour equal to the
  phase-clock hold's within millimetres. Prediction: equality, because the regression stays near
  1 with D stated at decimetres against a 2 h walk of about 2e-4 m2. A difference is attributed
  through the instruments (Phi, the reverts, the per-key transfer difference), not declared a
  win. The census follows only if G1 holds.
- **G2 (the pass, measured).** r per constellation and hour on both beds, recorded as a baseline
  the covariance work must move to 1, not as a pass or fail. Prediction: about 0.02 on the sim
  (the stated D variance far above the wander).
- **G3 (adaptation).** The sim with a known per-satellite random walk added to the a priori clock
  product, read by the producer and the consumer alike; the truth grade subtracts the injected
  walk. Arms: (A) the anchor's q raised to the injected walk; (B) the walk with the golden q;
  (C) no walk. Prediction for A: the smoothed hold hides a fraction f rho_D of the injected
  walk's phase-visible motion, with rho_D = P_D / (P_D + sum q) from the clone's own covariance
  and f the clock's share of the phase-visible direction (about one half): a walk of 0.036 m2 at
  2 h against today's stated D of 0.04 m2 hides about a quarter, a consistent D near 3 cm about
  one percent. A passes when the measured hidden fraction equals the predicted one; the absolute
  consumer number is the joint-covariance work's target, not this card's. B identifies the
  instruments that would catch a wrong q in the field (the level NEES and card PVN's NIS) on a
  known case. The phase-clock hold is run on A for comparison (it hides the whole walk).

## 6. Order

Build and U0, G0, G1 and G2 on both beds, then the perturbed a priori product and G3. 2 h checks
(the sim from 00:30:15); 6 h only for an adoption proposal.

## 7. Build, U0, G0, G1 and the G2 baseline (2026-09-29)

**Build.** Seam `src/core/filter/NetworkPbHoldClone.cpp`: the pure `pbHoldCloneAdvance` (the exact
route of section 2 and the revision of the value) and `pbHoldCloneRebase` (the re-harvest), called
by `networkPbHoldCloneUpdate` right before `networkEmitStore` and `networkPbHoldCloneHarvest` right
after `networkStashStatus`; the mode in `networkPbHold` (queue at a generation start, the clone's
transfer on a held emission, the revert to the phase-clock transfer, continuous at the switch);
knob, loader interlocks, S line `pbhold=3`, trace `logs/network_pb_hold_clone.csv`, run-end line
`pb hold, smoothed (SMH)`. The border is stashed after emit, so the birth is declared at emit and
materialized at the same epoch's stash (section 2's birth rule). An S term carries its stash
variance until the re-harvest, so the posterior variance of the increment includes it (the first
build left it out; the unit gate caught it).

**U0.** `tests/test_pb_hold_smoothed.cpp`, suite `PbHoldSmoothedTest`, 9 cases: the clone's value,
its variance and the increment's posterior variance equal an augmented Kalman filter carrying
c(t0) as a zero-noise state (the fixed-point smoother) at every epoch of a 40-epoch dense walk
model to 1e-8, and to 1e-6 relative with a reinit of a column the clone has spread onto and with a
reinit of the clone's own clock (the Reinit route re-attributes through P); a frame step of the
prior means leaves the value unchanged and a pinned term adds no revision; the three deaths; the
re-harvest; the hold's transfer and its continuous revert; the loader; the S line. Full Debug
suite 2 717 tests, 2 716 passed, 1 fixture-gated skip.

**G0.** Exe `gnss_processor_smh1.exe` sha256 prefix `3338a185a7c104c7`, arms
`runs/_rap_smh_{off,ex,lah}_{s,f}` from the configs of card PVN's exact-update arms (2 h, the sim
from 00:30:15). Knob off: `products.txt` and `closed_loop_trace.csv` identical to
`_rap_pv_ex_{s,f}` (card PVN's exe), both beds. Knob on against off: every C, B, U, H, E and R
record identical; P records differ in value only, with keys and generations equal (14 574 sim,
20 390 field); the S line reads `pbhold=3`.

**G1.** Consumer 3D p50 by hour (m), the exact update on:

| wire | sim | field |
| --- | --- | --- |
| plain hold (`_rap_smh_off`) | 1.49 / 0.78 / 0.57 | 14.00 / 3.45 |
| smoothed hold (`_rap_smh_ex`) | 1.41 / 0.24 / 0.26 | 0.60 / 0.18 |
| phase-clock hold (`_rap_smh_lah`) | 0.34 / 0.16 / 0.13 | 0.12 / 0.02 |

Not equal: G1 fails, and the difference is attributed. Instruments: 66 (sim) and 91 (field)
clones born, no death, no revert; |Phi| p50 5.0 and 2.5 cm, p90 14 and 9 cm; the transfer differs
from the phase-clock transfer by 12 and 28 cm p50, common mode included.

The fold inflation W of the parent clock is the anchor's q (9e-7 m2 per epoch) at every epoch but
the Q3 anchor's LOOSE ones, where the clock folds at the legacy rate (0.03 m2 per epoch). Every
satellite is born LOOSE, so every clone born at the run's first emission spans its first four
epochs at that rate (W = 0.03, 0.06, 0.09, 0.12 m2 at ages 30 to 120 s, then 9e-7 m2 per epoch): the
model states the clock free at 17 cm per epoch there, and
the smoother passes the estimator's phase-visible motion across them. Split by W (clones older than
30 min):

| clones | sim \|Phi\| p50 / p90 | field \|Phi\| p50 / p90 | r mean sim / field |
| --- | --- | --- | --- |
| spanning LOOSE epochs (W > 0.01 m2) | 5.1 / 14.0 cm | 2.5 / 7.7 cm | 0.086 / 0.104 |
| not spanning them | 0.0 / 0.2 mm | 0.1 / 6.6 mm | 0.67 / 0.59 |

For the clones that do not span LOOSE epochs, section 5's prediction holds: the smoother passes
nothing. Offline replays of the SMH wire (`runs/_rpl_smh_*`, the session scratchpad's
`smh_rewrite.py`; the unmodified wire's replay reproduces the live consumer's p50):

| replay | sim | field |
| --- | --- | --- |
| the constellation common mode of the transfer removed | consumer within 1.5 mm of the unmodified | within 4.9 mm |
| the transfer in the emitted frame, P = held + [C(t) - C(t0)] - Phi | 1.41 / 0.24 / 0.25 | 0.60 / 0.18 |
| as above, Phi zeroed for the clones spanning LOOSE epochs | 0.34 / 0.16 / 0.12 | 0.12 / 0.02 |

So the whole difference to the phase-clock hold is the motion the smoother passes across the
anchor's LOOSE epochs. The consumer absorbs a common phase-bias motion, and the emitted clock equals
the solve clock up to a per-epoch common mode (their de-commoned difference within a generation is
0 on both beds), so neither the common mode nor the frame separates the two. On the sim, whose clock
truth has no jumps, the motion passed is the estimator's phase-visible settling after ignition.

**G2 (baseline).** r = Phi^2 / (W - V_post) mean 0.123 (sim) and 0.134 (field) over all smoothed
emissions; 0.67 and 0.59 for the clones that do not span LOOSE epochs, against section 5's
prediction of about 0.02.

**Reading.** The member does what the model states; the model's LOOSE epochs are the difference.
Two responses, not yet chosen: (1) a LOOSE epoch of the parent as a decoupling edge of the clone
(its birth deferred until the parent's anchor holds, the phase-clock transfer until then, continuous
at the switch), which the last replay row says reproduces the phase-clock hold on both beds; (2) the
anchor's LOOSE fold rate (the legacy q, 1e-3 m2/s, at every satellite's birth and re-admission) as
the estimator's own statement to revisit, outside this card. G3 waits for the choice: its arms
would span the same LOOSE epochs.

## 8. The review and the fixed build (2026-09-29)

**The review.** An adversarial read of the build (four lenses, each finding checked by a skeptic)
confirmed four defects and refuted three claims.

- Card Q3's in-epoch refloat withdraws a tripped clock's carried prior and re-solves; the clone
  still routed that clock through S at the anchor's q and read its jump as a revision, so a
  declared clock step would have reached the held phase bias.
- At a clock-pivot handover whose constellation carry the fold skipped (the S-transform a no-op
  for want of a carried level of the new pivot, the transform off, or an unscoped handover), the
  clone read the old frame's means as the prior and stepped with the re-realization.
- Both came from reading the epoch's S set and the stash's means instead of what the final solve
  folded. The fix reads the fold's capture (`ws.cap_dense_keys`, which the refloat trims, and
  `ws.cap_priors`): an S key the solve did not fold takes the Reinit route
  (`pbHoldCloneRestrictEpoch`, unit-tested against the Reinit reference), the prior means are the
  capture's, a clone whose row loses every dense term dies (cause `unlinked`), and the mode now
  requires `network.raim_within_epoch`.
- A coupled clock and phase-bias mark on a key written by its clone was counted as transferred by
  the phase-clock rule; it now goes to the not-transferred census.
- The hold's run-end lines, SMH's like PBH's and LAH's, print only with the closed-loop summary;
  kept as they are.

The refuted claims: the revert's continuity (twice; it is the rule of section 2), and two readings
of the inflation and of the refloat that the code does not support.

**The fixed build.** Exe `gnss_processor_smh2.exe` sha256 prefix `bf8af85637d573de`; full Debug
suite 2 718 tests, 2 717 passed, 1 fixture-gated skip; U0 10 cases; G0 as in section 7 (every non-P
record identical, P in value only), arms `runs/_rap_smh2_ex_{s,f}`.

The capture shows what the first build ignored: at the first epoch after the anchor's LOOSE epochs
the dense carried-covariance fold is not positive definite after its ridge retry and falls back to
the diagonal carry (sim TOW 261210 and 261570, field 259350 and 259440; the same epochs in the
knob-off arms and in card PVN's exact arm, the one-epoch fallback card CTU2 section 3 kept the
default bound for). With no cross-covariance carried, every clone born at ignition is unlinked
there (62 of 66 on the sim, 86 of 91 on the field) and its key reverts to the phase-clock transfer,
continuing from the transfer the clone had reached, which is the motion passed across the LOOSE
epochs.

| consumer p50 by hour (m) | sim | field |
| --- | --- | --- |
| smoothed hold, fixed build (`_rap_smh2_ex`) | 1.79 / 0.21 / 0.13 | 0.11 / 0.02 |
| phase-clock hold (`_rap_smh_lah`) | 0.34 / 0.16 / 0.13 | 0.12 / 0.02 |

G2 over the clones that survive: r mean 0.74 (sim) and 0.52 (field).

**Reading.** On these beds the smoothed member lives only for the generations born after
ignition, and the smoothed hold is the phase-clock hold everywhere else, plus the LOOSE-era motion
frozen in by the revert. What the member exposes are two statements of the estimator: the Q3
anchor's LOOSE fold at the legacy rate (0.03 m2 per epoch at every satellite's birth), which the
smoother reads as licence to pass the phase-visible settling, and the dense fold's non-positive
block at the epoch the anchor leaves LOOSE, which unlinks every clone. Open, for the author: a LOOSE
epoch of the parent as a decoupling edge of the clone (the birth deferred until the anchor holds,
which the replay of section 7 says reproduces the phase-clock hold); the anchor's LOOSE rate
itself; the non-positive block at the LOOSE exit. G3 waits on that choice.

## 9. The anchor's engagement, the fold bound and G3 (2026-09-29)

**The outside review's third note** (the author's session, after section 8), and its disposition:

1. The clone's lifetime is the anchor's engagement. A LOOSE clock folds at the legacy rate, which
   is a datum device (the clock re-solved nearly fresh while it finds the a priori's frame), not a
   statement about motion, and a smoother reads q literally as motion. So the clone is born at
   engagement and dies at withdrawal, with the phase-clock transfer bridging in between: one rule
   for the run start, every re-admission and the jump withdrawals. Counters: births deferred and
   for how long, clones killed by a withdrawal, bridged emissions. Built (below).
2. The anchor's LOOSE rate belongs to card PRM: the principled replacement is a birth variance
   with the physical q from the first epoch, the engagement test kept as the event it is, and a q
   rule that is not a motion model says so in the registry. Recorded there (PRM section 12).
3. The fold's conditioning bar (card CTU2 section 3) rejects an honestly conditioned block at
   engagement; one knob, one clean-start arm, graded on the level gates, card PVN and the
   survival of the clones born at start-up; the default moves if the clean start is unchanged or
   better. Run (below); the default moved.
4. G2's predicted 0.02 used the control arm's stated D (21 cm); the hold runs on the exact arm,
   where D tightens to about 3 cm by 2 h, so the expected reading is card PVN's within-arc NEES
   (about 0.3) blended with the level's 2 to 3. It stays a baseline; it becomes two instruments
   only when a satellite's phase biases are cloned with its clock as a small block with the cross
   terms kept (later, not this card).
5. The dependency on the within-epoch tier is a load-time refusal, not a runtime degradation. It
   was one since section 8; it now reads the resolved value (the tier can be armed by
   `network.raim_withhold_sigma0` or `network.raim_we_code_iono`), and the fold capture has no
   runtime condition beyond the knob (`within_epoch_armed = config_.networkRaimWithinEpoch()`).
6. The bridge keeps card LAH's common-mode median: the phase-clock transfer needs it, the
   smoothed member does not. It does (unit-tested below).

**Build.** Exe `gnss_processor_smh3.exe` sha256 prefix `095a55612d6ffed8`, then
`gnss_processor_smh4.exe` `9e33142f0b352c5c` (the fold default below).

- Update (`networkPbHoldCloneUpdate`), before the exact route: a live clone whose parent clock
  card Q3's guard decoupled this epoch (`clk_anchor.tripped`: a trip, a gap, a retired frame)
  dies `withdrawn`; one whose parent was not folded at the anchored variance this epoch
  (`folded_anchored` false: LOOSE, or no clock prior folded) dies `disengaged`. Both queue the
  birth again for the same generation. A clone on the constellation's pinned clock is exempt from
  the second (no anchor fold; its transfer is zero).
- Harvest (`networkPbHoldCloneHarvest`): a pending birth is taken only at a stash that holds the
  parent's clock while the anchor holds it (anchored after this epoch's guard, not tripped in it);
  otherwise it waits, counted by cause (the anchor, a stashed clock) with the wait in epochs. A
  pending birth whose generation has ended is dropped.
- Hold (`networkPbHold`): a key with no live clone of its generation bridges on LAH's transfer,
  median included. A clone taking over writes its transfer on top of the transfer the key had
  reached (`smh_base`); leaving a clone, LAH continues from the clone's last transfer. A clone
  dead of any other cause (section 2's list, `unlinked`) reverts the key for the rest of the
  generation, as before. Event `PB_HOLD smh_bridge` at a switch to the bridge.
- Loader: both refusals at the end of the network section on resolved values; the mode also
  requires `network.clock_temporal_anchor` without `network.clock_model` (which disarms it).
- Trace `network_pb_hold_clone.csv` gains `base_m`, and `pd0_m2`, `f0` (the stated variance of
  c - p_j at birth and the clock's share of it; see G3 for why they are not the prediction's
  inputs).
- Run-end: a second SMH line (deferred births and the wait p50 / max, re-births, the two waits,
  the dropped, the withdrawn and disengaged deaths, switches to the bridge, bridged emissions).

**U0.** 13 cases (4 new): the bridge keeps the median and a re-born clone continues its
transfer; a withdrawn or LOOSE parent kills the clone and queues the birth (the engaged and the
pinned ones go on to the exact route); a birth waits for the anchor and for a stashed clock (and
a withdrawal in the stash's own epoch defers it); the loader refuses the mode without the anchor
and accepts the within-epoch tier armed by the withhold ceiling. Full Debug suite (with the fold
default below) 2 721 tests, 2 720 passed, 1 fixture-gated skip.

**G0.** Knob off: `products.txt` and `closed_loop_trace.csv` identical to section 7's off arms
(`_rap_smh3_off_{s,f}` against `_rap_smh_off_{s,f}`). Knob on: every record but P identical, P
in value only (keys and generations equal), both beds.

**Option 1 at the historical bound (1e-6), `_rap_smh3_ex_{s,f}`.** Every birth at the run start
waits 4 epochs (62 sim, 86 field), and the clone is born at the stash of the guard's re-coupling
epoch. The next epoch is the dense fold's diagonal fallback (section 8), and every one of them
dies `unlinked` before its first smoothed emission, so its key carries LAH's transfer unchanged:
no LOOSE-era motion is frozen in any more. 0 withdrawn, 0 disengaged.

| consumer p50 by hour (m) | sim | field |
| --- | --- | --- |
| smoothed hold, section 8 | 1.79 / 0.21 | 0.11 / 0.02 |
| smoothed hold, engagement rule, 1e-6 | 0.348 / 0.169 | 0.116 / 0.025 |
| phase-clock hold (`_rap_smh_lah`) | 0.340 / 0.160 | 0.116 / 0.025 |

The field is equal. The sim's 8 to 9 mm is two clones of second generations born with the anchor
engaged (G20 and G12): the replay with LAH's P spliced onto those two keys gives LAH's consumer
exactly (0.340 / 0.160, `runs/_rpl_smh3ex_g20_s`), and the splice onto the pinned clock's clone
(G01, whose transfer differs from LAH's by up to 13 cm) changes nothing (`_rpl_smh3ex_g01_s`).

**The fold bound, `_rap_smh3_k12_{s,f}` (`network.dense_fold_kappa` 1e-12) against
`_rap_smh3_ex_{s,f}`.** The solve changes at the engagement epoch only (no diagonal fallback: 2
events to 0 per bed).

| | sim, 1e-6 | sim, 1e-12 | field, 1e-6 | field, 1e-12 |
| --- | --- | --- | --- | --- |
| datum-free level at epochs 16 / 60 / 120 / 240 (m) | 0.10 / 0.07 / 0.05 / 0.03 | 0.10 / 0.06 / 0.04 / 0.03 | | |
| IF code-residual spread, hour 2 (m) | 0.052 | 0.048 | 0.164 | 0.164 |
| anchor trips | 0 | 0 | 4 | 0 |
| RAIM-attribution re-floats (epochs / ambiguities) | 0 / 0 | 0 / 0 | 9 / 27 | 7 / 23 |
| PVN NIS mean | 1.03 | 1.04 | 0.10 | 0.05 |
| PVN within-arc NEES | 0.3 | 0.4 | 0.2 | 0.0 |
| PVN rms scatter, hour 0 / 1 (m) | 0.056 / 0.007 | 0.036 / 0.007 | 0.115 / 0.016 | 0.140 / 0.014 |
| clones born at start-up alive at 2 h | 0 of 62 | 62 of 62 | 0 of 86 | 86 of 86 |

Unchanged or better on the level gates and on PVN (the field's hour-0 scatter excepted, 2.5 cm,
with the stated sd doubling in that hour); the start-up clones survive. On the golden posture the
two bounds are byte-identical (products and consumer, 2 h, both beds, `runs/_rap_gk6_*` against
`runs/_rap_gk12_*`: the golden's block never reaches the 1e-6 bar). CTU2's 1e-12 trial that made
the level worse ran on the 00:00:30 start, where the fallback released the artifact's level; on
the clean start it does not decide. **The config default moved to 1e-12** (the loader and
`GnssConfig`; the estimator's class default stays 1e-6, and 1e-6 in a config restores the
historical behaviour). Checked on the new exe: the golden 2 h arms identical to the 1e-6 arms
(`runs/_rap_gk_d4_*`), and the smoothed-hold arms without the knob identical to the explicit
1e-12 arms (`runs/_rap_smh4_ex_*`).

**G1 at the new default** (the phase-clock hold on the same solve, `_rap_smh3_k12lah_{s,f}`):

| consumer p50 by hour (m) | sim | field |
| --- | --- | --- |
| smoothed hold | 0.307 / 0.156 | 0.108 / 0.023 |
| phase-clock hold | 0.339 / 0.160 | 0.108 / 0.024 |

Field equal within a millimetre. Sim: 3.2 cm in hour 0, 4 mm in hour 1, in the smoothed hold's
favour; a finding, attributed. Replays of the sim wire (`runs/_rpl_smh3k12_*`): unchanged 0.307 /
0.156 (the live consumer); P rebuilt in the emitted frame with Phi kept 0.305 / 0.157; with Phi
zeroed for every clone 0.339 / 0.160, the phase-clock hold exactly. So the whole difference is
the motion the clones pass, and every clone is now born with the anchor engaged: it is the
estimator's phase-visible settling after engagement, which section 5 predicted the smoother would
not pass. |Phi| p50 2.5 mm, p90 1.0 cm (sim), 4.5 mm and 1.5 cm (field). The consumer says the
passed settling moves the sim's wire towards the truth; the phase-bias truth is defined only up
to the ambiguity datum, so that is inferred from the consumer, not graded. Every start-up key
bridges for 4 emissions (248 sim, 344 field); 6 clones per bed die `disengaged` late in the run
(their clocks leave; the re-births wait on a stashed clock).

**G2 at the new default.** r mean 1.35 on the sim (hour 0 1.87, hour 1 0.83), consistent with
item 4's blend; 10.4 on the field (hour 0 18.7, hour 1 2.06). The field's reading has the
signature of G3's arm B below (a real clock motion against the anchor's q): on the field the a
priori clock's real error is not a constant, and on the phase-visible axis it moves faster than
2.9e-8 m2/s states. Baseline, not graded.

**G3.** The a priori product `runs/_g3_data/2024-129/products/COD0MGXFIN_20241290000_01D_30S_CLK.CLK`:
every GPS clock record from TOW 261000 (00:30:00) moved by w_s(t)/c, w_s an independent random
walk per satellite with q = 5e-6 m2/s (0.036 m2 over 2 h; seed 129; the walk in
`runs/_g3_data/walk_q5e-6.csv`; 90 208 records changed, each equal to the walk within 1 mm, no
other byte changed); the other products copied, `static/` a junction to `data/static`. Producer
and consumer read it through `files.internal_data_root`. Grade: on the wire D_w = C - P (L1C) and
the truth D = -w, both de-commoned per epoch across the GPS satellites (the consumer absorbs a
common mode), increments from the clone's birth; hidden = 1 - the pooled slope of the wire's
increment on the truth's. Arms (sim, 2 h, the clean start, the new fold default):

| arm | hidden fraction | consumer p50 by hour (m) | r mean | PVN NIS mean |
| --- | --- | --- | --- | --- |
| A: walk, anchor q 5.029e-6 (`_rap_g3a_smh`) | 0.026 (clone segments; 0.060 over every held emission) | 0.627 / 0.066 | 1.55 | 1.02 |
| A with the phase-clock hold (`_rap_g3a_lah`) | 1.008 | 1.305 / 0.763 | | |
| B: walk, golden q (`_rap_g3b_smh`) | 0.455 | 0.951 / 0.713 | 130 (h0 61, h1 199) | 3.22 (h1 4.92, 14 % above 6.63) |
| C: no walk (`_rap_smh3_k12_s`) | | 0.307 / 0.156 | 1.35 | 1.04 |

The adaptation is real: with the anchor's q matched to the walk the smoothed hold passes 97 % of
the walk's phase-visible motion and the consumer converges (hour 1 0.066 m), while the phase-clock
hold hides all of it (1.008) and the consumer stays at 0.76 m. With the golden q (B) it hides 46 %
and the consumer stays at 0.71 m. B's wrong q is caught by two instruments: the smoothed hold's r
(130 against 1.5) and card PVN's NIS (3.2 against 1.0). The anchor guard does not trip (0
decouplings on A and B).

The pre-registered prediction does not hold on A. With rho_D from the stated phase-visible
variance at birth (card PVN's functional, sqrt_sp 0.34 m at the engagement epoch) and f = 1/2, it
is 0.41 on A (rho_D 0.81) and 0.50 on B (rho_D 1.00); measured 0.026 and 0.455 (`g3pred.py`).

The reading. On A the clock takes the walk (the anchor's q matches it) and the smoother passes
it: the carrier phase observes D's epoch-to-epoch increments to millimetres while D's absolute,
tied to the float ambiguities, converges slowly, so the smoother revises the absolute's
convergence onto c(t_b) and passes the walk. rho_D over the absolute D is not the smoother's gain
on a walk whose increments are observed. On B the anchor's q cannot follow the walk and the
estimator's clock does not take it: the de-commoned wire clock follows 0.29 of the walk against
0.55 on A, and B's held phase bias does not move (`g3track.py`; C and P de-commoned separately,
so these slopes are indicative, not a decomposition). B's hidden share is therefore mostly the
estimator's, which the hold passes on as it is, and B's agreement with f rho_D at f = 1/2 does not
confirm the formula: the formula mixes the estimator's split of a phase-visible increment with
the smoother's gain. The hold-off arms (`_rap_g3{a,b}_nohold`) do not separate the two, because
the unheld phase bias moves several times the walk (its re-references, which is why the hold
exists). The trace's `pd0_m2` and `f0` (c - p_j at birth) carry the float-ambiguity datum (p50
48 m2, f0 0.001) and are not the prediction's inputs. The clock-plus-phase-bias clone block of
item 4 would measure the split directly; it is the instrument this prediction needs.

**Open.**

- The mixed state. The smoothed transfer is the revision in the solve frame; the phase-clock
  transfer is de-commoned by the median in the emitted frame. With every key on one member the
  two differ by a common mode, which the consumer absorbs; with some keys on each (the bridge,
  the reverts), a smoothed key and its bridged neighbours differ by the constellation's median
  solve-frame clock motion since the clone's birth. At the new default the mixed state is the 4
  bridge emissions at birth and the late disengagements; not changed.
- The census (2 h, then the 6 h adoption pair) is owed before any arming.
- G3 with the phase-bias block, when it is built.

Scratchpad graders (the session's): `g3_walk.py` (the product and its check), `g3grade.py` (the
hidden fraction, the clock error with the walk removed), `g3pred.py` (rho_D from card PVN's
trace), `g3track.py` (the clock's and the held phase bias's share of the walk), `smh_rewrite.py` (rules
`same`, `emit`, `emit0`, `emitz`), `splicekey.py`, `pkeydiff.py`.

## 10. One frame for the hold family (2026-09-29)

**The defect.** Section 9's open item, measured. The smoothed transfer was the revision of the
clone in the solve frame; the phase-clock transfer is the emitted clock de-commoned by card LAH's
median M_k. With C = x + g (the emitted clock is the solve clock up to a per-epoch common mode g)
and M_k's step the median over the constellation's satellites of the emitted-clock step, the
phase-visible wire moves by dM_k on a bridged key and by dPhi + dg on a smoothed one. With every
key on one member that is a common mode; with some keys on each, a smoothed key and its bridged
neighbours drift apart by the median of the solve-frame clock steps since the clone's birth. At the
historical bound the mixed state is the rule (every start-up clone dies unlinked), and it was the
whole of section 9's 8 mm on the sim: a preview replay of LAH's wire with P = P_LAH - Phi on every
live clone's emissions gives LAH's consumer exactly (0.340 / 0.160, `runs/_rpl_lahphi_ex_s`),
and at the new default it gives the smoothed arm's (0.305 / 0.157 against 0.307 / 0.156,
`runs/_rpl_lahphi_k12_s`).

**The rule.** The hold writes card LAH's transfer minus the clone's passed motion:
P_j(t) = held_j + T_LAH(t) - Phi(t). The members of the hold family then share LAH's frame and
its common-mode median, and differ only by Phi (the phase-clock member is Phi = 0). Phi is
accumulated frame-free per clone: each epoch it adds the parent clock's step net of declared frame
steps (the posterior minus the prior mean the solve folded, which an in-place frame transform has
already moved) minus the revision of x0. Without a frame step it equals section 4's
c(t|t) - x0(t|t). A clone on the pinned clock has Phi = 0 and writes LAH's transfer exactly.
Consequences: the clone's value and its reference no longer enter the transfer (they feed Phi and
the instruments); a per-satellite declared step (an a priori re-level) now follows LAH's rule (the
held phase bias moves with the clock) instead of landing on C alone, which on these beds does not
occur (a precise a priori; CLV's re-base refused); a switch between the clone and the bridge is
continuous with no stored base (LAH's reference carries it), so `smh_base` is gone and the key
keeps `smh_lah_last` (LAH's transfer reused on an epoch without the satellite's clock). A clone
whose parent has no folded prior mean dies `parent_outside` (the engagement test leaves no such
case under the anchor).

**Gates.** Exe `gnss_processor_smh5.exe` sha256 prefix `4b4b9b468d590c47`.

- U0: 14 cases (a new one: with no passed motion the smoothed key writes LAH's transfer, median
  included, while its neighbours bridge; the two hold tests now set Phi). Full Debug suite 2 722
  tests, 2 721 passed, 1 fixture-gated skip.
- G0: knob off byte-identical to section 9's off arms (`_rap_smh5_off6_{s,f}` against
  `_rap_smh3_off_{s,f}`, 1e-6); knob on against off at the new default, every record but P
  identical, P in value only (`_rap_smh5_{off,ex}_{s,f}`).
- Phi: the frame-free accumulation equals section 9's solve-frame Phi on every row of both beds
  (14 806 sim, 20 524 field; the trace's 1e-6 m resolution). No clock-pivot handover occurred in
  these 2 h (the field's 119 handover events are phase-anchor ones), so the frame-step path is
  exercised by construction, not by these arms.

**G1.**

| consumer p50 by hour (m) | sim | field |
| --- | --- | --- |
| smoothed hold, 1e-6 (`_rap_smh5_k6`) | 0.340 / 0.160 | 0.116 / 0.025 |
| phase-clock hold, 1e-6 (`_rap_smh_lah`) | 0.340 / 0.160 | 0.116 / 0.025 |
| smoothed hold, 1e-12 (`_rap_smh5_ex`) | 0.305 / 0.157 | 0.108 / 0.022 |
| phase-clock hold, 1e-12 (`_rap_smh3_k12lah`) | 0.339 / 0.160 | 0.108 / 0.024 |

At the historical bound, where every start-up clone dies at engagement, the smoothed hold is the
phase-clock hold on both beds: G1's prediction holds. At the new default the difference is the
passed motion of the engaged clones (section 9's replay), now free of the frame term.

**G3 again** (`_rap_g3{a,b}_smh5`): hidden 0.038 on arm A's clone segments (0.026 without the
pinned clock's clone, whose transfer is now LAH's) and 0.465 on arm B; consumers 0.628 / 0.066 and
0.954 / 0.712, as in section 9.

**Census, 2 h** (`census2h.py`, PBF on, the 16 field sites; the wires `_rap_smh5_ex_f` and the
phase-clock hold on the same solve `_rap_smh3_k12lah_f`; keys `smh5P`, `lahk12P`): median of the
site p50 hours 1 / 2, 0.606 / 0.163 against 0.603 / 0.166. The medians agree, the sites do not.
Hour 1 improves at most sites (the paired change median -0.068 m on the 10 C1W sites, 6 better and
2 worse at 5 cm; -0.075 m on the 6 C1C sites, 4 better, 0 worse). Hour 2 splits: CEDU 0.294 to
0.171, DAV1 0.136 to 0.097, KOKB 1.475 to 1.361, BAKE 0.104 to 0.074 better; CHTI 0.059 to 0.095,
ANMG 0.060 to 0.115, BADG 0.049 to 0.093, STPM 0.045 to 0.085, WUH2 0.082 to 0.156, POHN 0.495 to
0.630 worse (paired median +0.006 C1W, +0.017 C1C). On the field the passed motion grows with the
clone's age (|Phi| p50 2.5 mm in hour 1, 6.9 mm in hour 2, p90 1.9 cm; G10 7.4 cm, G02, G07 and G08
about 3 cm) against a stated walk of 1.4 cm sd over 2 h (W 2e-4 m2), which is G2's field reading
(r 18.7 then 2.1): the smoothed hold passes centimetre-level phase-visible motion on the field that
the phase-clock hold freezes. Whether that is the a priori clock's real error (card PBH read 3: the
field clock wanders 2.7 cm within an arc) or the estimator's, the census does not say; the good
sites that lose in hour 2 say part of it is not what the consumer needs.

**Census, 6 h** (`census6h.py`, PBF on; the fixed member and the phase-clock hold on the new
default, wires `_rap_smh5_ex6_f` and `_rap_lahk12_6_f`, keys `smh5k6`, `lahk12k6`; the control
with the plain hold from card LAH, key `ctl`). Median of the site p50 by hour (m):

| wire | h1 | h2 | h3 | h4 | h5 | h6 |
| --- | --- | --- | --- | --- | --- | --- |
| phase-clock hold | 0.603 | 0.166 | 0.119 | 0.081 | 0.101 | 0.077 |
| smoothed hold | 0.606 | 0.163 | 0.106 | 0.102 | 0.099 | 0.088 |
| control, plain hold | 0.666 | 0.169 | 0.104 | 0.077 | 0.092 | 0.070 |

Paired change against the phase-clock hold, median (better / worse at 1 cm): the 10 C1W sites
-0.068 (8 / 2), +0.006 (3 / 5), +0.015 (2 / 7), +0.043 (2 / 8), +0.026 (2 / 8), +0.006 (2 / 5); the 6
C1C sites -0.075 (4 / 1), +0.017 (2 / 3), +0.009 (2 / 3), -0.010 (3 / 1), -0.017 (3 / 2), -0.001
(3 / 3). The largest losses: KOKB hour 3 0.13 to 0.25 and hour 4 0.07 to 0.18, POHN hours 2 to 6,
ARHT hour 4 0.14 to 0.26, BAKE hour 5 0.02 to 0.10; CEDU gains in hours 2 to 5. The live consumer
(WTZZ): 0.108 0.022 0.010 0.016 0.023 0.029 against 0.108 0.024 0.016 0.020 0.030 0.026.

Attribution: the two wires differ by the passed motion and nothing else. On every held emission
P_smoothed = P_phase-clock - Phi - (the Phi each earlier clone of the key had reached when a
withdrawal ended it, kept for continuity); the residual is constant within each of the 197
segments to 5e-7 m. The 6 h field run has 92 withdrawals (86 switches to the bridge, 92
re-births), the 2 h runs none.

**Reading.** The census does not support the smoothed member on the field beyond hour 1: it gains
at ignition (hour 1, 12 of 16 sites better) and loses from hour 3 at the C1W sites (median +1.5 to
+4.3 cm, 7 or 8 of 10 worse), while the live consumer gains throughout. On the field the passed
motion exceeds its stated walk (r 18.7 in hour 1, 2.1 in hour 2) and grows with the clone's age.
Whether it is the a priori clock's real error or the estimator's drift into the clock needs a truth
the field does not have; on the sim, G3's arm A shows the member passing a real walk when the
anchor's q states it, and arm B shows what an understated q does to the estimator. The member
stays default off and not armed; the phase-clock hold (card LAH) stays the hold of the
exact-update posture. Open, for the author: the anchor's q derived from the field's own passed
motion (card PRM's q rule, now with a graded instrument, r); the clock-plus-phase-bias block for
G2's split; the census at the C1W sites.

Session scratchpad: `lahphi.py` (the preview wire), `census2h.py`, `census6h.py`, `g3grade.py`,
`g3pred.py`, `g3track.py`.

## 11. What the smoothed member passes, and the block member (2026-09-30)

**The reading (the outside review's fourth note).** Three kinds of motion reach a satellite's clock
and phase biases: a clock walk (c moves, the phase biases do not), a level walk (c and every phase
bias move together, which the phase does not see) and a phase-datum re-realization (the phase
biases move, c does not). The wire's phase side should pass the first, never the second, and hide
the third. The clock member passes the clock's smoothed increment Phi = c(t|t) - c(t0|t), and no
function of the clock's increment alone separates a clock walk from a level walk, since both move
c. The smoother splits the level's motion between the revision of c(t0) and the walk by
P_L / (P_L + sum q_L): with the level stated at 2 to 3 cm after two hours and the level's walk
noise at a third of the clock's q per epoch, about two thirds of the level's motion reads as walk
by hour three, is not revised at t0 and reaches the phase side. This predicts section 10's field
reading, growth with the clone's age and r well above the anchor's q, and the largest losses at
the most remote sites, whose arcs are longest (section 10's largest were at KOKB, POHN, ARHT and
BAKE; their remoteness is not graded here). The field's level moves by centimetres over hours
through the code side (the receiver code-bias carry's memory, the ionosphere's breathing); the sim
has no such driver. The live consumer's gain throughout is not explained by this reading.

**Two counters, read before the build.**

- *The withdrawals.* The 92 withdrawals of the 6 h field run (`_rap_smh5_ex6_f`) are one event:
  the clock-pivot handover G01 to G02 at 04:42:30 (TOW 276150, `datum_bump`, the S-transform
  applied, c0 = -1.588 m), which card Q3's anchor answers as a retired clock frame
  (`cta_decouple reason=datum`) on all 31 clocks at once: card Q3 reads 31 trips, all of them datum, and none of its step tests tripped in 6 h.
  Every clone died at that epoch, its passed motion stayed in its key's transfer (section 10's
  attribution), and the re-births 4 epochs later restarted at zero age. The sim has the same
  handover at the same epoch (c0 = -1.906 m, 62 withdrawals).
- *The passed motion by the clone's age* (one row per satellite and epoch; |Phi| p50 / p90 in mm,
  and the mean of r = Phi^2 / (W - V_post)):

  | clone age (h) | 0-1 | 1-2 | 2-3 | 3-4 | 4-5 |
  | --- | --- | --- | --- | --- | --- |
  | field \|Phi\| | 2.9 / 11.7 | 7.4 / 20.4 | 10.0 / 28.9 | 12.4 / 52.3 | 15.2 / 62.8 |
  | field r | 11.1 | 2.0 | 1.9 | 2.3 | 2.5 |
  | sim \|Phi\| | 2.1 / 7.8 | 4.0 / 10.4 | 5.2 / 13.9 | 6.8 / 13.8 | 7.1 / 15.0 |
  | sim r | 1.33 | 0.59 | 0.49 | 0.35 | 0.30 |

  By hour of the day the field's p50 is 2.5, 6.9, 9.5, 12.2, 10.0 and 4.5 mm; the last hour follows
  the handover's reset. On the field the passed motion grows with age and stays at about twice its
  stated walk; on the sim, whose a priori clock is the truth (no clock walk to pass), it grows by
  millimetres and stays inside its stated walk.
- *The sim at 6 h* (`_rap_smh5_ex6_s` against `_rap_lahk12_6_s`, the same solve; exe
  `gnss_processor_smh5.exe`). The held-out consumer SZZ000SIM, p50 by hour (m):

  | wire | h1 | h2 | h3 | h4 | h5 | h6 |
  | --- | --- | --- | --- | --- | --- | --- |
  | phase-clock hold | 0.339 | 0.160 | 0.057 | 0.039 | 0.038 | 0.060 |
  | smoothed hold (clock member) | 0.305 | 0.157 | 0.064 | 0.044 | 0.043 | 0.038 |

  The sim has one held-out consumer, so a census of 16 in-network stations (S01, S05, ..., S57
  and S60; consumer-only replays of the two wires on the sim golden's consumer with PBF on, keys
  `lahk12s`, `smh5s`, tags `smc6_*`, session script `simcensus6h.py`) reads the paired change.
  These consumers are in the network, so the absolute values are optimistic; the change is what
  is read. Median of the station p50 by hour (m), and the median paired change (better / worse
  at 1 cm):

  | wire | h1 | h2 | h3 | h4 | h5 | h6 |
  | --- | --- | --- | --- | --- | --- | --- |
  | phase-clock hold | 0.484 | 0.129 | 0.059 | 0.054 | 0.041 | 0.044 |
  | smoothed hold (clock member) | 0.442 | 0.133 | 0.075 | 0.067 | 0.049 | 0.047 |
  | change | -0.000 (5 / 5) | +0.004 (4 / 7) | +0.008 (1 / 8) | +0.011 (3 / 9) | +0.006 (0 / 7) | +0.002 (2 / 3) |

  From hour 3 the sim's change is small and of the field's sign (the field's C1W sites +0.015,
  +0.043, +0.026 m in hours 3 to 5).

**The outside review's decisions on section 10.**

- *The fold bound stays at 1e-12.* The clean-start readings of section 9 (the level gates, card
  PVN, every start-up clone surviving) are unchanged or better (card
  [CTU2](CTU2_level_recursion_followup_2026-09-29.md) section 12); the exact factor's own bar is
  1e-14 (`couplingExactFactor`); the historical 1e-6 was calibrated on an inconsistent level; the
  regression of CTU2 section 3 was read on the 00:00:30 start, which is not graded. The goldens
  are byte-identical at both bounds, so there is no ledger consequence.
- *The anchor's q is not derived from r as it stands.* Section 10's passed motion is mostly level
  under the reading above, so a clock q derived from it would loosen the clock toward the code
  side's drift and enlarge the leak. The clock's q is derived from the phase-visible part once the
  split exists, and the level part informs the code-bias and receiver code-bias carries, where
  that drift belongs: card [PRM](PRM_parameter_registry_2026-09-29.md)'s q rule, with the split
  as its input.
- *The member that separates the two walks is built as a member, not as an instrument*: the
  smoothed hold on the ionosphere-free phase-visible clock D = c - sum_j w_j p_j, whose clone is
  the (c, p_1, p_2) block. Pre-registered as its own card,
  [SMD](SMD_smoothed_hold_block_member_2026-09-30.md) (this file is near the size bar).

This card's member stays default off and not armed; the phase-clock hold stays the exact
posture's hold. Session scripts: `simcensus6h.py`.

## 12. The bridge keeps the epoch's clock step (2026-09-30)

Card [SMD](SMD_smoothed_hold_block_member_2026-09-30.md) section 7 found, through its attribution
check, that the switch to the bridge (section 9) re-referenced card LAH's transfer on the transfer
the key had last written, which dropped LAH's step of that epoch: C - P stepped by the satellite's
de-commoned clock step at every withdrawal, disengagement or revert. The fix folds the passed
motion the clone reached into LAH's reference, so the epoch's step is transferred as LAH
transfers it (exe `gnss_processor_smd2.exe`). The member is unchanged at 2 h on both beds (no
switch in 2 h, `runs/_rap_smd2_ex_{s,f}` byte-identical to `runs/_rap_smh5_ex_{s,f}`); on the 6 h
field wire 12 542 P records move by at most 5.3 mm (the 92 bridges of the 04:42:30 handover,
`runs/_rap_smd2_ex6_f`). The census re-graded on the fixed wire (key `smd2c6`) is section 10's to
a millimetre except hour 6 (median of the site p50 0.092 against 0.088 m; C1W change +0.005
against +0.006 m), and the live consumer's hour 6 improves (0.023 against 0.029 m). Section 10's
attribution (a residual constant within each segment) held because a dropped step is constant
within its segment; the wire rebuilt from LAH's with the frozen passed motion now matches the
member's to 1e-6 m. Card SMD section 8 also finds the passed motion wholly phase-visible (the
level is a constant of the estimator's model), which revises section 11's reading.
