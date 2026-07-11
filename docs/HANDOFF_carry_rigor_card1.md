# CARD 1 — Rigorous between-epoch carry: conditional local carry with shared-coupling rows

*Fable-specced, Opus-executed (2026-07-10). Do NOT deviate from this design; where a step
is ambiguous, stop and report rather than improvise. Source evidence: memory
`project_network_ddfrac_rootcause_shvtec` (T6/T6b: floors÷10 ⇒ 12 h divergence at ANY
iono-pin strength — the carry incest is the gating defect) and
`project_opus_ssystem_audit_jul9` (time-linking finding [0]).*

## The defect (what you are fixing)

Each epoch, TWO marginals of the SAME joint posterior are re-injected next epoch as
INDEPENDENT priors:
1. the shared-border marginal (D2 diagonal or C1 dense stash → addSharedPrior/Dense), and
2. each station's local ambiguity/ZWD values with their CONDITIONAL variances
   (`est.conditionalLocalVarianceDiag`, harvested per station) injected as MARGINAL
   information via `addStationPrior` (a diagonal row on the local column only).

The local prior therefore re-asserts information about the shared border that the border
carry already delivered (the local value encodes the border through the reduction), and
the injection treats a CONDITIONAL variance as MARGINAL — both double-counts. Compounding
over epochs ⇒ σ collapse (blinded only by the B4 floors) ⇒ T6/T6b divergence when floors
drop. Formal statement: the exact recursion must carry the joint (border + cross + local)
— Tienstra estimation-in-phases / Hou-Zhang Eqs 18-24.

## The fix (exact design — no alternatives)

Carry the local ambiguity conditionally and inject it WITH its regression on the shared
border, as a pseudo-observation ROW spanning (A_s | A_r):

For carried local column r (ambiguity or ZWD) of station i, at harvest time compute the
regression row
```
M_r = (N_rr⁻¹ e_r)ᵀ N_rs        (1 × n_shared)
```
using the station's CACHED LDLᵀ factor (`st.Nrr_ldlt.solve(e_r)`, one backsolve) and the
station's `N_sr`ᵀ — both already live in NetworkNormalEquations::Station. Stash per
carried column: value `r̂` (absolute, as today), conditional variance `q_cond` (from
`conditionalLocalVarianceDiag`, as today), the row `M_r`, and the shared-column KEYS
(identity strings) for every non-negligible entry of `M_r` (|M_rj| > 1e-8 · max|M_r| —
threshold to sparsify; keep the threshold a named constant).

Next epoch, instead of `addStationPrior` on the local column alone, append ONE
pseudo-observation row into the station's assembly (the same A_s/A_r append pattern
appendShIonoRows uses):
```
row:  δr  −  Σ_j M_rj · δs_j   =   r̂_prev − r_lin + Σ_j M_rj · (s_lin,j − ŝ_prev,j)
σ²:   q_cond + q_process·dt    (q_process = existing phase_leveling q / zwd q)
```
where `r_lin` is this epoch's local linearization value and `s_lin,j` / `ŝ_prev,j` are
this epoch's linearization and last epoch's solved values of shared column j — i.e. the
prior says "r moves WITH the border per M_r", which is exactly the conditional
distribution r | s. Columns of `M_r` whose shared key is ABSENT this epoch: drop the term
and add `M_rj² · P_prev,jj` (from the stashed border variance) to the row σ² (marginalize
the departed column, do not fabricate it).

The shared-border marginal carry (C1 dense — REQUIRED on for this card's validation)
stays exactly as is: with the local rows now conditional, the border information flows
ONLY through the border carry — the double-count is gone by construction.

What does NOT change: the kernel (`reduce`, folds, datums), the harvest of the border
stash, RAIM/QC, the emit path, any threshold. The ZWD carry gets the same treatment (it
is one more carried local column).

## Config

`network.carry_conditional_coupling` (bool, default **false** = byte-identical). When
true: the local carry injection switches to coupling rows for ALL carried local columns
(phase-leveling ambiguities + ZWD); plain `addStationPrior` path is not executed for
them. Require (hard error at load) `shared_carry_full_cov = true` when armed — the
conditional design is only exact against a dense border carry. Add the toggle to the
snapshot fingerprint (estimation-relevant, mirror `shdevtau=`), GnssConfig getter with
Doxygen, `loadNetworkSection_` parse + summary log line, per CLAUDE.md conventions.

## Code anchors (re-grep every one; lines drift)

- Local carry stash + injection: `local_carry`, `addStationPrior` call
  (ApplicationNetwork.cpp — injection "BEFORE reduce" comment), harvest via
  `conditionalLocalVarianceDiag` near the phase-leveling harvest.
- Row-append pattern to copy: `appendShIonoRows` (A_s/A_r conservativeResize + row fill).
- Kernel accessors needed: `Station::Nrr_ldlt`, `Station::N_sr` — if not publicly
  reachable, add a `const` accessor `NetworkNormalEquations::stationRegressionRow(station,
  local_col)` returning `M_r` (10 lines, kernel-read-only) rather than exposing members.
- Border stash: the C1 dense carry (`shared_carry_full_cov`, CarryPrior dense block) —
  read-only for this card.
- ZWD carry: `zwd_carry` map.

## Validation (in this order; a failure at any step = stop and report)

1. **Unit test (the correctness gate): recursive == batch.** Synthetic 2 stations ×
   2 epochs, static truth, no datum change, zero process noise: solve epochs jointly
   (stacked batch) vs sequentially with {C1 dense border carry + conditional coupling
   rows}. Assert shared AND local posteriors match to 1e-9 (mirror the existing
   `DenseCarryRecursiveEqualsBatch` test — extend it, same file). This test FAILING with
   the old diagonal carry and PASSING with the new rows is the whole point of the card.
2. Suite ≥ 596 pass / 1 skip; byte-identity OFF: rerun
   `config/test_suite/_t4_gg_treated_smoke.json`, SHA of network_solution.csv ==
   `69C20D38E22B5EBE`.
3. **The falsifier (armed):** copy `_t6b_gg_treated_lowfloor_12h.json` + the new toggle
   (+ `shared_carry_full_cov: true`) → 12 h run (~10 min wall). SUCCESS =
   NO divergence (a00 stays ~40±5 TECU all 12 h), formal dd_σ rides the low floor
   (~0.034 cyc) with DD step med ≈ σ·√2 (NOT 12 cyc/ep), σ-calib within ~2× of 1.
   Grade: `python <fable-scratch>/grade_t5.py <run> 3600` (grader spec in memory; if the
   scratchpad is gone, the memory documents the metrics — |frac| stats, R, med dd_σ, med
   step, LOS>3σ, a00).
4. Report the numbers for BOTH the 1 h and 3 h tails, including the |frac| deciles —
   whether fractions begin clustering is the first honest reading of that instrument, and
   EITHER outcome is a finding. Do not tune anything to chase clustering.

## Explicitly out of scope (do not touch)

Code-bias gauge/datum timing (card 2), Eq-34 integer datum (card 3), floors' default
values, RAIM thresholds, the T5 GM-deviation code (leave as is, default-off), kernel
reduction/fold logic.
