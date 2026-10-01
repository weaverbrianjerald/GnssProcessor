# WSX: the exact time update's within-station cross covariance, re-tested on the current posture

**STATUS**

- **id:** WSX
- **topic:** network-vehicle
- **status:** closed
- **summary:** KEEP_OFF 2026-09-30 (sections 6 and 7): on the current posture the within-station term is harmless and almost invisible (G0 to G5 pass on 2 h arms of both beds: no drift, the CTU section 13.4 (c) drift belonged to the earlier posture; census paired change at most +0.006 m; satellite-side instruments within a few hundredths); X1's ordering and X4's code-bias half fail. The unit network's size study is the finding: the shipped per-row form keeps the level with the exact batch but over-states a between-satellite phase-visible contrast, 0.885 / 0.661 / 0.336 of the exact variance at 8 / 16 / 48 stations (the dropped cross-station half), and the within-station half makes it worse at every size. Next: a measure-only contrast instrument on the full network, then the augmented previous border. Nothing armed. Pre-registered 2026-09-30 (lever 1 after card REV: the phase-visible wander at its source, the first step of the joint-covariance sequence named in the outside review of cards CTU and CTU2). Card CTU's exact time update adds each carried row's own inflation M_i' B M_i and drops the covariance M_i' B M_k between two rows of one station; a first build that added it drifted the sim level (0.97 m at epoch 240, CTU section 13.4 (c)) and was removed before commit. That arm predates four changes of the posture (the clean sim start, `network.rank_gate_tol` 1e-11, the receiver code bias in the block, `network.dense_fold_kappa` 1e-12). The term is re-built behind `network.carry_coupling_exact_cross` (default off) in the station's whitened local block, which the float vehicle keeps live; the unit gate gains the shipped form beside the removed one against the exact batch. Predictions: in the unit network the shipped form states the level tighter than the form with the cross terms and both stay within a factor 2 of the batch; on the current posture the cross terms do not drift the level; they leave the level and phase-visible innovation instruments within 0.1.
- **source:** [CTU](CTU_coupling_time_update_2026-09-28.md) sections 4, 11 and 13.4 (c); [CTU2](CTU2_level_recursion_followup_2026-09-29.md) (the clean start, the receiver code bias in the block, the rank gate); [PVN](PVN_phase_visible_nees_2026-09-29.md); [REV](REV_revision_member_replay_2026-09-30.md) sections 6 and 7 (the revision NEES); [PRM](PRM_parameter_registry_2026-09-29.md) (the sequence after CTU2); the outside review of cards CTU and CTU2 (2026-09-29: the within-station terms re-tested first, then the local block, then the augmented previous border)
- **knobs / posture:** `network.carry_coupling_exact_cross` (new, default off; requires `network.carry_coupling_exact_update` and the whitened local block); arms on card REV's exact-update posture (`runs/_rap_rev1_blk6_{f,s}`, 2 h, the sim from 00:30:15); nothing armed, no golden or ledger change
- **dates in the text:** 2026-09-30
- **hashes / run dirs:** exe `gnss_processor_wsx1.exe` `e3d6c4955054e1f9`; arms `runs/_rap_wsx_{ctl,on}_{f,s}` (2 h); rebuilt phase-clock wires `runs/_rpl_wsx_lah_{ctl,on}_{f,s}`; census keys `wsxc`, `wsxn`, `wsxcs`, `wsxns`; unit cases `CouplingExactUpdateTest.WithinStationCrossTermsAgainstTheBatch`, `WsxWithinStationCrossTest`

## 1. Why this term, and what it can and cannot show

A carried row of station r, l_i = r_i + M_i' s + e_i, states the station's ambiguity against the
shared border s at the previous epoch. Card CTU's exact time update re-expresses it on the moved
border, M_new = (P + Q)^-1 P M, with the added error M' B M, B = P - P_{all,S} (P_SS + Q_S)^-1
P_{S,all} (CTU section 4). Two rows i and k share that error through M_i' B M_k. The shipped form keeps
the diagonal and drops the rest:

- **Within a station** (this card): two rows of one station involve different satellites; they
  share the time-update error through the regression's spread (M_new picks up every border key
  correlated with the row's own) and through the block's receiver code-bias rows, whose
  coefficients reach every satellite of the station.
- **Across stations** (not this card): two stations' rows of one satellite share that
  satellite's clock walk almost wholly. This is the larger part of what the shipped form drops,
  and only the augmented previous border (the previous epoch's border carried as states, the
  walk applied by the filter) carries it exactly.

The off-diagonal of the carried rows' covariance is not empty today: on this posture the float
vehicle keeps the station's whitened local block live (`k.pl_local_block` resolves on under
`ambiguity_vehicle: float_kf`), and every exact row is a row of a block whose off-diagonal holds
the harvested conditional covariances (N_rr^-1). The time-update term is the one missing there.

A partial covariance is an approximation of its own: adding the within-station half while the
cross-station half stays out need not move the recursion toward the batch. The unit gate measures
that on a network small enough to run the exact batch.

Two defects of the record found while scoping the card are fixed with it: the unit gate's
"producer's form" (`runRecursion` mode 1) is the removed variant with the cross terms, so the
shipped form has no recursion test; and the loader's log of the exact update still announces the
removed term ("and between a station's rows in the local block").

## 2. The build

- `network.carry_coupling_exact_cross` (processing toggle in the `network` section, default
  off). The loader refuses it without `network.carry_coupling_exact_update`, with
  `network.carry_coupling_joint_whiten` (card 1b's path), and warns when the local block is not
  live (then the knob is inert, counted).
- In `CouplingRowBuilder::buildLocalBlock`, after the harvested cross covariances and before the
  young-arc congruence (so the congruence acts on the whole matrix and keeps every correlation):
  for every pair of rows that both took the exact route, Sigma(i, k) += couplingExactCross(xr_i,
  xr_k), symmetric; a row on card 1d's route or on an unbuilt epoch adds nothing. The term is not
  scaled by the variance-component factor (the diagonal's own time-update addend is not). The
  per-row result of `couplingExactRow`, discarded today, is kept for the pair loop.
- Counters (run end): blocks and pairs that took the term, pairs skipped for a non-exact member,
  the added correlation |rho_ik| p50 / p90 / max split into ambiguity pairs and pairs with a
  receiver code-bias row, and the block PD fallbacks (`pl_blk_fallback`, which falls back to
  per-column rows without any cross term).
- The loader log names the cross term only when the knob is on.

## 3. Gates

- **U0.** The recursion unit gate gains mode 3, the shipped form (one block per station, the
  harvested within-station covariance kept, the time-update addend on the diagonal only), beside
  mode 1 (the same with the within-station time-update cross terms, the knob's form); both against
  the exact batch, the level variance ratio and error per satellite printed. A builder case: two
  exact rows of one station get Sigma(i, k) equal to `couplingExactCross`, a card 1d row none,
  the knob off leaves Sigma byte-identical. The loader. The full Debug suite.
- **G0.** Knob off: the products, the consumer trace and the clone trace identical to the first
  2 h of `_rap_rev1_blk6_{f,s}` (`ident2h.py`), both beds.
- **G1 (the mechanism).** Pairs taken above zero on both beds; no rise of the PD fallbacks;
  card CTU's negative-inflation counter stays at 0.
- **G2 (no drift, sim).** The datum-free level (`ctugrade.py`) within 0.05 m of the control at
  epochs 60, 120 and 240 and not rising by more than 0.03 m from epoch 120 to 240; the IF code
  spread in hour 2 within 0.01 m of the control; anchor trips at most the control's plus 1;
  RAIM re-floats at most 1.5 times the control's plus 2; S-basis completions at most twice the
  control's plus 10.
- **G3 (consistency, both beds).** The level innovation NIS mean and the split-half NEES_tf, card
  PVN's NIS and within-arc NEES, the prior-residual census sigma0 by family (its pl dispersion is
  a mirror that excludes the time-update addends, so it moves only through the residuals), card
  SMD's r_D and card REV's revision NEES by birth class.
- **G4 (field).** The IF code spread in hour 2 within 0.01 m of the control; trips at most the
  control's plus 2.
- **G5 (the consumer, only if G2 passes).** The 16-site field census at 2 h (`census2h.py`) on
  the phase-clock hold's wire of each arm, rebuilt from its block trace (`smd_ctlwire.py` with
  alpha -1), against the control's: no hour with a median paired change above +0.010 m.

## 4. Predictions (written before the build)

- **X1 (the unit network).** The shipped form (mode 3) states the level tighter than mode 1 for
  every satellite (a smaller variance ratio to the batch), and both stay within the existing bar
  (variance ratio above 0.5, level error below half the batch sigma).
- **X2 (no drift).** With the four later changes in place the cross terms do not drift the sim
  level: G2 passes.
- **X3 (the instruments).** The within-station terms act on station-common directions, which the
  satellite-side instruments do not read: the level innovation NIS mean and card PVN's NIS each
  move by less than 0.1 on both beds, and the pl and pb sigma0 by less than 0.02.
- **X4 (the size).** The added correlations are small between ambiguity rows (|rho| p90 below 0.1)
  and larger in pairs with a receiver code-bias row (p90 above 0.2).

**The decision rule.** G2 fails: the within-station half alone is not a step toward the batch on
this posture; the knob stays off and the joint-covariance sequence goes to the augmented previous
border, which carries the walk's covariance within and across stations at once. G2 passes and X3
holds: the term is harmless and nearly invisible, the dropped covariance that matters is the
cross-station half, and the next step is the same. G2 passes and an instrument moves by more than
X3 allows toward consistency: a 6 h pair and the 6 h censuses for the author's decision before the
augmented border.

## 5. Order

The unit gate first (X1 decides nothing on its own, but it is the only place the batch is
available), then the build, U0, G0 and the 2 h arms on both beds (control and knob on, one exe),
then G5 if G2 passes.

## 6. Results (2026-09-30)

Exe `gnss_processor_wsx1.exe` (sha256 prefix `e3d6c4955054e1f9`); arms `runs/_rap_wsx_{ctl,on}_{f,s}`
(2 h, card REV's block arms with only the knob changed); the phase-clock hold's wires rebuilt from
each arm's block trace, `runs/_rpl_wsx_lah_{ctl,on}_{f,s}` (the control's rebuild equals the
phase-clock hold's own run to 1.2e-6 m on both beds); census keys `wsxc`, `wsxn` (field, 2 h, PBF
on) and `wsxcs`, `wsxns` (sim, hours 1 and 2 read).

**U0.** `runRecursion` gains mode 3, the shipped form; `CouplingExactUpdateTest.
WithinStationCrossTermsAgainstTheBatch` reads both forms against the exact batch;
`WsxWithinStationCrossTest` (3 cases: the term between exact rows only, off bitwise, the loader).
Full Debug suite 2736 passed, 1 skipped.

**X1 (the unit network, 3 stations): the ordering FAILS, the bar holds.** The level's variance
ratio to the batch is 1.0000 with the within-station terms and 1.0001 shipped (equal to 1e-4, not
ordered as predicted); both errors stay below 0.02 batch sigma. Over the whole border the shipped
form states no direction tighter than the batch (generalized eigenvalues 1.000 to 1.163), while
the within-station half alone states one 5.5 % tighter (0.9445), a between-satellite contrast of
the phase-visible clock D (loadings clock : L1 : L2 phase bias 308 : -780 : +473, opposite for the
two satellites).

**The unit network by size** (the station count raised by hand; the exact recursion meets the
batch to 1e-8 relative at every size): the smallest generalized eigenvalue of the recursion's
border covariance against the batch's, always along a between-satellite phase-visible contrast,
and the level's variance ratio:

| stations | 3 | 8 | 16 | 48 |
| --- | --- | --- | --- | --- |
| shipped form, smallest eigenvalue | 1.000 | 0.885 | 0.661 | 0.336 |
| with the within-station half | 0.944 | 0.693 | 0.518 | 0.322 |
| level variance ratio (shipped) | 1.000 | 1.000 | 0.999 | 0.996 |

**G0** passes on both beds: products (59 192 and 45 428 records), consumer trace, epoch status and
clone trace identical to the first 2 h of `_rap_rev1_blk6_{f,s}`. **G1** passes: 1 810 682 pairs
over 13 922 blocks on the sim and 1 847 929 over 13 474 on the field took the term (0 and 814
skipped for a row off the exact route), no PD fallback, no negative inflation.

**X4 holds for the ambiguity pairs and FAILS for the code-bias pairs.** |rho| of the added term
against the pair's diagonal is below 0.01 for 99.97 % (sim) and 99.80 % (field) of the ambiguity
pairs and for 99.92 % and 99.81 % of the pairs with a receiver code-bias row: those are not
larger. Each class has a thin tail at |rho| >= 0.8 (sim 172 and 28 pairs, field 105 and 15, max
0.99).

**G2 passes (X2 holds): no drift.** Sim datum-free level at epochs 16 / 60 / 120 / 240: 0.10 / 0.06
/ 0.04 / 0.04 m against the control's 0.10 / 0.06 / 0.04 / 0.03; IF code spread in hour 2 0.056
against 0.048 m (+0.008, bar 0.01); trips, RAIM re-floats and S-basis completions 0 on both arms.
**G4 passes**: field IF code spread in hour 2 0.165 against 0.164 m, trips 0 and 0, re-floats 8 / 25
against 7 / 23 (epochs / ambiguities).

**G3 (X3 holds)**, control against knob on:

| | sim | field |
| --- | --- | --- |
| level innovation NIS mean | 1.070 -> 1.074 | 1.833 -> 1.832 |
| PVN NIS mean | 1.040 -> 1.062 | 0.046 -> 0.044 |
| PVN within-arc NEES by hour | 0.4, 0.1 -> 0.4, 0.1 | 0.0, 0.0 -> 0.1, 0.0 |
| PVN within-arc scatter, hours 1 / 2 (cm) | 3.60 / 0.69 -> 3.56 / 0.84 | (arc populations differ: 95 and 88 arcs) |
| stated sd of D, p50 (mm) | 39.1 -> 37.1 | 152 -> 153 |
| prior-residual sigma0 pl / pb | 0.184 / 0.107 -> 0.179 / 0.107 | 0.147 / 0.163 -> 0.129 / 0.162 |
| card SMD r_D, hours 0 / 1 | 1.84 / 0.71 -> 1.25 / 0.46 | 18.1 / 1.64 -> 17.4 / 1.20 |
| \|Phi_D\| p90 at clone age 0 / 1 h (mm) | 8.0 / 11.0 -> 5.4 / 7.0 | 9.8 / 17.0 -> 9.1 / 15.0 |
| revision NEES, ignition clones, age 0 / 1 h | 0.68 / 0.86 -> 0.68 / 0.75 | 0.76 / 1.10 -> 1.15 / 1.50 |
| revision NEES, later clones, age 0 | 1.76 -> 1.62 | 4.09 -> 4.97 |

The smaller walk share on the sim is the smoother re-apportioning under a slightly tighter stated D,
not less wander: the within-arc scatter of D itself does not fall.

**G5 passes.** On the phase-clock hold's wire, field census against the control (median paired
change, hours 1 / 2): C1W +0.001 / +0.000 m, C1C +0.006 / -0.000 m; sim 0.000 in both hours.

## 7. Reading and decision (2026-09-30)

On the current posture the within-station half is harmless and almost invisible: the drift of CTU
section 13.4 (c) does not recur (it belonged to the earlier posture), the consumer does not move,
and the satellite-side instruments move by a few hundredths. It is also not a step toward the
batch: in the unit network it tightens a between-satellite phase-visible contrast beyond the exact
recursion at every size. **The knob stays off (KEEP_OFF).**

The card's main finding is the unit network's size study. The shipped per-row form keeps the level
with the batch at every size but over-states a between-satellite phase-visible contrast, more as the
stations per satellite grow: 0.885, 0.661 and 0.336 of the exact variance at 8, 16 and 48 stations.
The cause is the dropped cross-station half of the time-update covariance, every station's row of a
satellite carrying that satellite's one walk. The direction is the one a float consumer cannot
absorb (a contrast between satellites, not a common mode), and at the field's station counts the
over-statement is about a factor of 3 in variance. The instruments graded here read single
satellites (card PVN's D, card REV's revision per clone) or the level, not this contrast, so the
full network's size of it is not yet measured.

**What follows (the decision rule's branch, with the size study as its target).** The augmented
previous border: the previous epoch's border carried as states, the walk applied by the filter,
exact by construction (the unit network's mode 0). Before that build, a measure-only instrument of
the between-satellite phase-visible contrast on the full network (its stated variance from the
shared border's posterior against its within-arc scatter on the sim, whose truth has none), with the
unit network's prediction (a contrast NEES above 1 on the 60-station sim) pre-registered. Nothing is
armed and no default moves.
