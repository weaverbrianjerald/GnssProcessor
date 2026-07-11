# CARD 1c — Numerical trace: why don't coupling rows contract σ on surviving arcs?

*Fable-specced 2026-07-10 (end of session). Full context: memory
`project_network_ddfrac_rootcause_shvtec` (read the TOP entries). Field facts: arcs
survive 16-43 consecutive epochs; dep_var≈0 (clock_model OFF in the 50-sta lineage ⇒
M_w empty ⇒ Card-1b joint whitening retired as a no-op, Opus A/B-proven); yet DD formal
σ stays flat at var_eff≈5.6 cyc and values random-walk 6-7 cyc/epoch — the
carry_conditional_coupling rows contribute ~zero information where 1/√k contraction is
expected. Settle WHY with a unit-test-scale trace. NO long runs, NO fixes before
reporting numbers.

## Build (test-only; extend the ConditionalCarryRecursiveEqualsBatch harness)

2 stations; shared border = 1 clock-like column (large per-epoch q, stashed) + 1
bias-like column (small q, stashed); locals = 1 ambiguity + 1 iono per station; phase
row (amb+iono+clock, σ 0.002 m) + code row (iono+clock, σ 0.3 m) per station per epoch;
8 epochs, no datum changes, zero ambiguity process noise. Drive the PRODUCTION path each
epoch: solve → harvest (conditionalLocalVarianceDiag + regression row) → inject coupling
rows next epoch. PRINT per epoch per ambiguity: harvested q_cond, injected row σ², M_r
coefficients, posterior conditional variance (the jointConditionalCovariance /
dd_sigma path), posterior value error vs truth.

## Interpret — three branches; report which, with the table

(a) σ contracts ~1/√k in the plain synthetic → the field flatness comes from an
    ingredient the synthetic lacks. Add ONE: an SH-like carried shared column both
    ionos are pinned to (finite q + representativeness σ) = correlated per-epoch iono
    error. Re-run. If contraction stops → CONFIRMED: correlated SH-carried iono error is
    the floor (physics, not a bug) — report and stop. Consequence: producer needs
    longer accumulation or structurally stronger iono; Card 3 (Eq-34 datum) proceeds in
    parallel since product integerness is independent.
(b) σ does NOT contract even in the plain synthetic → fixed-point defect in the
    harvest→inject loop (q_cond_k reproducing posterior_{k-1} instead of accumulating,
    or rows not entering N_rr). Identify the exact file:line — report, do not fix yet.
(c) Rows never injected for these ambiguities (gating) → report the gate condition.

## Deliverable

Per-epoch table + one-paragraph verdict naming (a)/(b)/(c) + file:line for (b)/(c).
Suite stays green (test-only change); fresh Debug build. Verification (Fable, next
session): reproduce the table, check the branch logic, then spec the follow-up card.

## RESULT (2026-07-10, Opus): BRANCH (a) — the carry is mechanically CORRECT; it DOES contract σ ~1/√k

Test `NetworkEstimatorTest.ConditionalCarryContractionTrace` (tests/test_network_estimator.cpp,
~line 1174) drives the production harvest→inject path for 8 epochs on a surviving arc and prints
two σ's: the single-ambiguity conditional σ AND the between-station DD σ N_{1,1}−N_{0,1} (the
field's dd_sigma contrast, which cancels the shared clock/bias).  Fresh Debug build; suite **604
pass / 1 skip** (test-only, +1 test).

**Plain synthetic (per-epoch, tracked arc N_{1,1}):**

| ep | q_cond(N11) | M_r          | single σ | **DD σ** | DD val err |
|----|-------------|--------------|----------|----------|-----------|
| 0  | 9.000e-2    | clk1 −2, pb1 +1 | 2.256 | 0.4243 | +0.79 |
| 1  | 4.500e-2    | −2, +1       | 2.247    | 0.3000   | −0.16 |
| 3  | 2.250e-2    | −2, +1       | 2.244    | 0.2121   | +0.03 |
| 7  | 1.125e-2    | −2, +1       | 2.245    | **0.1500** | −0.08 |

`q_cond` = **exactly 0.09/k** (1/k accumulation).  **DD σ ratio σ(0)/σ(7) = 2.83 = √8 EXACTLY**
(1/√k).  Value error converges, no bias.  The single-ambiguity σ is FLAT (ratio 1.01) only because
it keeps the shared-clock (clk1, M_r=−2) propagation the DD cancels.

**Verdict — BRANCH (a).**  The conditional-coupling carry is not defective: the harvested
conditional variance contracts exactly 1/k and the between-station DD σ (what the field's
`dd_sigma_cyc` actually is) contracts exactly 1/√k on an 8-epoch surviving arc.  The coupling rows
enter N_rr and accumulate perfectly — this RULES OUT branch (b) [no fixed-point defect] and branch
(c) [rows are injected; M_r nonzero].  So the field's FLAT σ is NOT a coupling-row accumulation
bug; it comes from an ingredient the plain synthetic lacks.

**Caution — the card's branch-(a) probe prediction is WRONG in an instructive way.**  The card
predicted adding a correlated SH-carried iono column would STOP the contraction (floor σ).  The
probe (`correlated_iono=1`: SH mapped DIFFERENTLY per station so it survives the DD, carried loose)
did NOT stop the formal-σ contraction (DD ratio still 2.70 ≈ √8) — instead it produced **σ-INCEST**:
the formal DD σ kept contracting (0.303→0.112 m) while the DD VALUE error blew up to **±5 m**
(formal σ ≈ 30–50× overconfident).  That is exactly the σ-incest of
`project_network_ddfrac_rootcause_shvtec` (white-reapplied SH against a quasi-static systematic):
correlated iono error makes the estimate CONFIDENT-BUT-WRONG, it does NOT produce a flat σ floor.

**Therefore the field's flat ~5.6-cyc formal σ is neither a carry defect nor plain correlated-iono
flooring.**  Given the carry provably accumulates, the flat formal σ on "surviving" arcs most likely
means the carry is repeatedly RESET — RAIM-attrib refloat churn (2→1063 arcs/ep in the memory),
pending-reseed, or the phase-leveling warmup/convergence gate drops the coupling row — so the
effective accumulation window is ≈1 epoch and q_cond never leaves the data-only level.  (Or the
flat median is an ensemble-of-freshly-(re)floated-arcs artifact.)  **Next test: instrument the
field carry LIFETIME — how many consecutive epochs a given (station,sat) arc keeps its coupling row
before a refloat/reseed resets it** (ApplicationNetwork.cpp refloat/`isAmbiguityPendingReseed`/
`clearPendingReseed` sites), on a real short run — NOT another unit test.

---

## VERIFICATION (2026-07-10 evening, Fable) — branch call OVERTURNED: it is **(b)**, mechanism identified, fix validated batch-exact

Opus's table is reproduced to the digit (both variants, current exe) and every number in it is
real.  The branch-(a) reading does not survive verification; the trail of evidence
(3-agent adversarial workflow + falsifier experiments, scratchpad `card1c_replicate.py` /
`card1c_falsify.py`):

**The defect.** The coupling row `r + M_rᵀs = r̂_prev + M_rᵀŝ_prev` @ weight `1/cond_var` is an
exact pseudo-obs of `u = r + M_rᵀs` **at the harvest epoch** (`Var(u|D_k) = cond_var`, u ⊥ s by
construction).  Between epochs the border moves: `s⁺ = s + w`, `w~N(0,Q)` (the same Q the D2/C1
fold applies at ApplicationNetwork.cpp:5646/5703), so `u⁺ = u + M_rᵀw` — the row variance must
inflate by `M_rᵀQM_r` and exactness requires the shrinkage `M' = (P+Q)⁻¹P·M_r`.  Production
applies NEITHER to layout-present columns (buildCplRow, ApplicationNetwork.cpp:4413-4431 scalar /
4375-4411 joint; `dep_var` fires only for layout-DEPARTED columns).  Each row is therefore
inconsistent by `M_rᵀQM_r` — T7 field magnitude: `M_clk≈−2`, `q_clk·dt = 0.01·60 = 0.6 m²/ep`
⇒ σ≈1.1 m ≈ **5.8 L1 cyc/epoch** told at a 4e-5 m²-floored σ (~1.5e4× weight overstatement),
matching the observed 6-7 cyc/epoch DD random walk.  The 1/k q_cond contraction is the recursion
correctly accumulating FICTITIOUS information — rows get stiffer AND staler (positive feedback).

**Why this fixture said (a).**  (i) q_clk was changed 1e4 → 1e-3 ("a truly large-q free clock
would diverge here") — the divergence WAS the branch-(b) evidence: the NumPy exact batch on the
SAME large-q fixture is consistent (|err|≤1.9σ), so the fixture was never ill-posed; only the
production carry formula corrupts it.  With q≈0 there is nothing stale to poison — the fix reduces
to a no-op (κ = P/(P+q) ≈ 1) and the recursion is genuinely near-exact.  (ii) The clean DD is an
exact-symmetry artifact: both stations' harvested M_r are identical, so the poison cancels
term-by-term in the between-station DD.  Falsifier E2 (symmetric, q=1e4): single-amb value walks
−159 cyc @8ep / −658 @24ep while the DD stays ≡ batch to 4 digits.  E3 (mild asymmetry — station-1
code σ 0.45 + one dropped code row on odd epochs): the DD leak switches on immediately, err/σ 81×
@ep2, **250× @ep11**; the time-update fix holds ≤1.6σ.  Fifty heterogeneous field stations are
maximally asymmetric — every field DD leaks.

**The correlated-iono probe adjudicates AGAINST the σ-incest attribution.**  E4 reproduces the
probe exactly (DD σ 0.303→0.112 contracting-while-wrong, err/σ to 32.5× @24ep — the "30-50×"
band).  The matrix time-update fix **cures it**: err/σ ≤ 1.23 @8ep / ≤ 1.1 @24ep, and the exact
batch under the SAME mismatched SH model is honest (−0.11σ/+0.22σ).  The ±5 m was the stale SH
coupling row (M_sh frozen at −0.50/−0.70 vs a border column moving q=5/ep) leaking through the
1.0-vs-1.4 mapping asymmetry — this card's own probe demonstrating the branch-(b) defect.  The
honest post-fix DD σ contracts with no meter-scale floor (0.303→0.148 @8ep → 0.121 @24ep vs batch
0.107/0.062).

**The reset/churn hypothesis fails the field data** (T7 12h forensics, 1822 arcs ≥15 ep):
per-arc steps FLAT with arc age (p50 5.5-7.8 cyc across age buckets 1-60; resets would give
independent draws, and honest ones at data-σ), values TREND (median 106-cyc drift/arc ≈ 20
formal σ; drift/(step·√n) = 2.57 — a random walk, which requires carried memory), steps GROW with
run time (p50 2.3→11 cyc over 12 h — matches rows stiffening as q_cond contracts), and the blow-up
onset is bracketed by the two "C1 dense fold non-PD" warnings (TOW 276420/287580) — the carried
covariance turning inconsistent.  σ p50 5.06 cyc (G 4.60/E 5.64), per-arc σ-vs-age log-log slope
−0.10 (flat), NOT floor-valued.  Step/σ p50 1.28 but p90 10.9 (heavy tail ≫ honest √2).

**Verdict: (b).**  file:line = ApplicationNetwork.cpp:4413-4431 (+4375-4411 joint path): missing
conditional time update (κ-shrinkage + MᵀQM inflation) for layout-present border columns; the
existing departed-column `dep_var` is the q→∞ limit of the correct rule.  The fix is specced as
**Card 1d — `docs/HANDOFF_carry_timeupdate_card1d.md`** (scalar per-column v1, matrix/joint v2
reusing the Card-1b whitening plumbing; NumPy-validated: matrix form == exact batch to 4 dp,
scalar form consistent-but-conservative).  Do NOT build the carry-lifetime instrumentation as the
next step — refloat churn is real but is not what flattens σ on surviving arcs.
