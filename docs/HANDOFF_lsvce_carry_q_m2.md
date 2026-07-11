# M2 — LS-VCE estimation of carry process noise (replacing hand-set floors)

*Fable-specced 2026-07-11. Foundation: Teunissen & Amiri-Simkooei 2008 (J Geod
82:65–82, LS-VCE). Motivation: the `_golden_carrylive_tu_12h` divergence at
content-hour 3.9 (docs/GOLDEN_carrylive_baseline.md "Open question") — leading
hypothesis is that the hand-set phase-leveling variance floor
(`phase_leveling_var_floor_m2`, 4e-5) sets the arming time and over-weights
carried rows after ~4 h. M3 (the 12h 4e-4 vs 1e-5 discriminator pair) tests
the premise; this card replaces the hand floor with a measured q.*

## Theory mapping (LS-VCE → our carry rows)

Carry rows are pseudo-observations with covariance `var_carried + q·Δt` — q
enters the observables' covariance **linearly through a known cofactor**
(1 on carry rows scaled by Δt, 0 elsewhere): exactly the LS-VCE model
Qyy = Q0 + Σ σk·Qk. Components to estimate, in priority order:

1. `q_pl` — phase-leveling ambiguity-carry process noise (the floor's
   replacement; physically ≈0 + un-modeled leveling error).
2. `q_pb`, `q_cb` — D2 shared-carry PB/code-bias q's (current floors 3e-7 /
   3e-5 m² are hand-set).
3. (defer) `q_clock` — the oscillator model has its own structure; out of
   scope v1.

Estimator (their Corollary 4, residual form — no misclosure basis needed):
  n_kl = tr(Qk Wy P⊥A Ql Wy P⊥A),  r_k = êᵀWy Qk Wy ê − tr(Qk Wy P⊥A Q0 Wy P⊥A)
with ê the per-epoch LS residuals and Wy = Qyy⁻¹ (minimum-variance weight,
iterate). Because Qk is nonzero ONLY on that component's carry rows, the
traces collapse to carry-row blocks of the residual projector — cheap even at
network scale (needs the carry rows' residuals + their formal variances +
cross-redundancy numbers, all available from the epoch solve; verify the
reduced/border solve exposes per-row redundancy contributions — if only
block-diagonal redundancy is cheap, use the block approximation and document
the bias).

## Design

- **Sequential accumulator** (their §3.2): per epoch compute the increment
  (n_kl, r_k) from carry-row residuals; fold with the running estimate as
  a-priori (σ0 = previous σ̂, W0 = its information). One σ̂ per component per
  constellation (start pooled; split per-σ only if residuals demand).
- **Positivity**: σk = exp(sk) reparametrization (their Eq. 32) — Gauss-Newton
  on sk; or simple clamping in v1 with a counter (report which bound binds).
- **Clean-epoch gating**: exclude epochs/rows flagged by slip detectors,
  refloat events, datum re-references (Card 3 counters / Card 4 tree swaps),
  RAIM exclusions — slip contamination biases êᵀQkê upward (the estimator
  would inflate q to explain re-levels). Gate = the same event sources the
  carry hygiene already tracks.
- **Separability check** (their §3.3): monitor the VCE normal matrix N's
  condition — q_pl vs σ0 (phase noise) vs q_pb near-collinearity is the known
  risk; if N is near-singular, freeze the weak component at prior.
- **Application mode** (two-stage, keep decoupled):
  - v1 = MONITOR: run the estimator, log σ̂ ± its STD per hour; compare to the
    hand floors. No feedback into the solve. This alone answers the hr-3.9
    question quantitatively alongside M3.
  - v2 = APPLY: replace floor/q constants with the running σ̂ (with a slew
    limiter and floor/ceiling guards). Only after v1 tracks sanely on the 3h
    and 12h golden pairs.
- Cross-validation instrument: Card 4's D5 Lemma-1 parallel actual-covariance
  (when built) measures the SAME misspecification independently — the two
  should agree.

## Config

```
network.carry_vce                 bool   false  master (v1 monitor mode)
network.carry_vce_apply           bool   false  v2: feed σ̂ back into the solve (requires carry_vce)
network.carry_vce_components      string "pl"   comma list: pl,pb,cb
network.carry_vce_window_epochs   int    60     accumulator half-life (exponential forgetting)
```
OFF byte-identical (no accumulator, no logging). `carry_vce_apply` without
`carry_vce` → load error.

## Tests / validation

1. Unit: synthetic 2-component fixture with known (q, σ0) — estimator
   recovers both within formal STD; exp-reparam never yields q<0.
2. Unit: contaminated fixture (injected level shift) — gated accumulator
   unbiased, ungated biased (documents WHY the gate exists).
3. Field v1: 3h golden — σ̂_pl vs the 4e-5 floor; 12h pair (M3 arms) — σ̂_pl
   trajectory before/at the cliff. ACCEPTANCE: σ̂ explains the M3 outcome
   (predicts which arm diverges).
4. Field v2: 12h golden with apply ON — no hr-3.9 cliff, dd-step ≤ tu-arm
   baseline.
