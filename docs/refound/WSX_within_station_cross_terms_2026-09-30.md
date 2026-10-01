# WSX: the exact time update's within-station cross covariance, re-tested on the current posture

**STATUS**

- **id:** WSX
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-09-30 (lever 1 after card REV: the phase-visible wander at its source, the first step of the joint-covariance sequence named in the outside review of cards CTU and CTU2). Card CTU's exact time update adds each carried row's own inflation M_i' B M_i and drops the covariance M_i' B M_k between two rows of one station; a first build that added it drifted the sim level (0.97 m at epoch 240, CTU section 13.4 (c)) and was removed before commit. That arm predates four changes of the posture (the clean sim start, `network.rank_gate_tol` 1e-11, the receiver code bias in the block, `network.dense_fold_kappa` 1e-12). The term is re-built behind `network.carry_coupling_exact_cross` (default off) in the station's whitened local block, which the float vehicle keeps live; the unit gate gains the shipped form beside the removed one against the exact batch. Predictions: in the unit network the shipped form states the level tighter than the form with the cross terms and both stay within a factor 2 of the batch; on the current posture the cross terms do not drift the level; they leave the level and phase-visible innovation instruments within 0.1.
- **source:** [CTU](CTU_coupling_time_update_2026-09-28.md) sections 4, 11 and 13.4 (c); [CTU2](CTU2_level_recursion_followup_2026-09-29.md) (the clean start, the receiver code bias in the block, the rank gate); [PVN](PVN_phase_visible_nees_2026-09-29.md); [REV](REV_revision_member_replay_2026-09-30.md) sections 6 and 7 (the revision NEES); [PRM](PRM_parameter_registry_2026-09-29.md) (the sequence after CTU2); the outside review of cards CTU and CTU2 (2026-09-29: the within-station terms re-tested first, then the local block, then the augmented previous border)
- **knobs / posture:** `network.carry_coupling_exact_cross` (new, default off; requires `network.carry_coupling_exact_update` and the whitened local block); arms on card REV's exact-update posture (`runs/_rap_rev1_blk6_{f,s}`, 2 h, the sim from 00:30:15); nothing armed, no golden or ledger change
- **dates in the text:** 2026-09-30
- **hashes / run dirs:** to be filled at the runs (section 6)

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
