# eq34 D4: the carry S-transform re-bases `coupling[].s_prev`

**STATUS**

- **id:** eq34 D4
- **topic:** network-vehicle
- **status:** closed
- **summary:** FIXED 2026-08-17: the eq34 D4 carry shift now moves the shifted columns' phase-bias coupling `s_prev` with the carried value, so a known frame step no longer enters a coupling row as bias.
- **dates in the text:** 2026-08-15 (filed) → 2026-08-17 (fixed)
- **knobs / posture:** none new; the defect fires only when the eq34 shift door (`eq34_shift_on`, needs `!carry_invariant`) and `carry_conditional_coupling` are armed together, in-tree only in `config/archive/_dcmp_50sta_10ep_eq34x.json`
- **hashes named in the text** (commit / exe / byte-cert): none
- **run dirs named in the text:** none
- **source:** `docs/archive/SPEC_card1_coupling_carry.md` §14

**Outcome.** The D4 shift moved `lc.value_abs` and `cp.value` but left `coupling[].s_prev` (the same number for the same identity) in the old frame, so a coupling row's two carried references moved inconsistently. The fix, `rebaseCouplingPhaseBiasSprev`, applies the same `+s` to every `PhaseBias` coupling term on a shifted (sat, signal) in the local, rpb and zwd carries, counted by `eq34_cpl_sprev_total`. The defect was latent but live; no live lineage armed the firing set. [PRM](../../refound/PRM_parameter_registry_2026-09-29.md) section 16.4 later replaced the helper with the shared `phaseBiasShiftOf` rule, byte-identical; the eq34 shift is inert on the current goldens (section 16.3).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_card1_coupling_carry.md -->
## 14. FIXED — the eq34 D4 S-transform now re-bases `coupling[].s_prev` (filed 2026-08-15, fixed 2026-08-17)

**The defect as filed.**  `CoupledShared::s_prev` and `CarryPrior::value` are **literally
the same number** for the same identity — both are assigned `x_s(j)` at harvest.  The D4
carry S-transform in `NetworkChainDatum.cpp` shifted `lc.value_abs` and `cp.value`, and
left `lc.coupling[].s_prev` untouched.  A coupling row's RHS is
`y = (value_abs − x_lin) + Σ_j M_j·ŝ_prev,j`, so under a known frame step the row's two
carried references moved **inconsistently** and the block injected precisely the frame
step as row bias.

For a gauge-invariant row the correct net y-change under a KNOWN step is the exact
cancellation of those two moves.  (Not identically zero in general: eq34 realizes its
datum with 1e10-weighted pin **rows** rather than Dirichlet elimination, so the residual
is a bounded pin-leverage term — which is itself a free diagnostic of how much `M`
truncation has eaten.)

It was **latent-and-LIVE, not latent-and-harmless**: at the population site the erase and
shift sets are mutually exclusive by construction —
`if (!k) eq34_reref.insert({sat, grp_sig}); else if (s != 0.0) shift_sat[{sat, grp_sig}] = s;`
— so the shifted keys are exactly the ones whose carries **survive** the erase.  What kept
it dormant is only that the shift door (`eq34_shift_on`, which needs `!carry_invariant`)
is armed together with `carry_conditional_coupling` in essentially no live config;
`config/archive/_dcmp_50sta_10ep_eq34x.json` does arm the full firing set.

**The fix (2026-08-17)** is the one the filing prescribed, in the Card-6 precedent's shape
(`NetworkRelevel.cpp` eph cutover: *the row's functional `u = r + Mᵀs` is frame-consistent
only if ALL carried references move together*):

- `rebaseCouplingPhaseBiasSprev` (`filter/NetworkCoupling.{hpp,cpp}`): applies the SAME
  `+s` the `prev_carry` walk applies to `cp.value` to every `PhaseBias`-kind coupling term
  whose (sat, signal) is in `shift_sat`, with the same zero / non-finite guards.  Nothing
  else moves — `coeff` and `p_prev` are frame-invariant.
- Called in the D4 shift block over **all three** coupling-bearing stashes —
  `local_carry`, `rpb_carry`, `zwd_carry` (the same trio precedent Card-6 walks for clock
  columns) — hoisted **out** of the per-carry `d != 0.0` guard: a carry whose own (sat,σ)
  did not move can still hold coupling terms on other satellites' shifted columns.
- Receiver-side shift maps (`shift_rxg`, `shift_rpb`) need no coupling image:
  `SharedParamKind` spans satellite-side + SH columns only.
- Not erased and not marked departed, exactly as filed: erasing discards the information
  the shift exists to preserve, and departing over-marginalizes a known, exactly-computed
  step.
- Counter `eq34_cpl_sprev_total` (invariant #4), reported in the run-end Card 3/eq34
  line — on an eq34+coupling lineage **0 re-based alongside nonzero v2-shifts is the
  wiring-inert alarm**, not a clean bill.
- Unit tests `NetworkCouplingTest.Eq34Rebase*`: kind/sat/signal selectivity, the exact
  `+s` sign, coeff/p_prev untouched, zero/NaN guards, empty-input no-ops.

⚠ **A/B methodology (same class as §13.6's replay exception):** the change is
solve-affecting on any lineage arming the eq34 shift door together with
`carry_conditional_coupling`, and **no fingerprint token discriminates old from new
behaviour** — an A/B spanning this commit on such a lineage is not a single-variable
comparison; re-baseline it.  In-tree only the archive config above arms the full firing
set, so no live lineage is invalidated.  With coupling off the stashes are empty and the
walk is structurally byte-identical.  The §13.8 ladder rungs remain valid A/Bs — the one
eq34 rung (`fld50`) carried this defect in BOTH arms, so the 1f/1g comparison was still
single-variable — but their absolute numbers will not reproduce on a post-fix build;
re-baseline before quoting any of them against a new run.
