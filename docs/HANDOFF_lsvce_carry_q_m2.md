# M2 — LS-VCE estimation of carry process noise (calibration/honesty instrument)

*Fable-specced 2026-07-11; REFRAMED same day after M3 falsified the original
motivation. Foundation: Teunissen & Amiri-Simkooei 2008 (J Geod 82:65–82,
LS-VCE).*

*STATUS OF THE ORIGINAL PREMISE: FALSIFIED. The hr-4 divergence of
`_golden_carrylive_tu_12h` is NOT floor-driven: the M3 three-point ladder
(4e-4 / 4e-5 / 1e-5 — a 40× swing, knob demonstrably active via monotone
clean-window formal dd_σ) shows all arms clean hr 0–3 and ALL cliffing at
content-hr 4 with the same signature. The cliff is EVENT-DRIVEN (the 04:00
UTC PB-set datum reshuffle → silent re-level → stale coupling rows integrate;
memory `card1e-gauge-amputation`), and its cure is the event response /
datum-invariant carry (Card 4), not any carry q.*

*SURVIVING PURPOSE (demoted priority): principled calibration of the carry
stochastic model — replace hand floors with measured σ̂ for HONESTY (formal σ
that means something; the M3 ladder itself showed formal dd_σ varying 14%
across floors while actual behavior was identical — i.e., today's formal
numbers are floor artifacts). Serves as the independent cross-check of Card
4's Lemma-1 actual-covariance instrument, and as general floor hygiene.
It is NOT expected to move any divergence.*

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
3. Field v1: 3h golden — σ̂_pl vs the 4e-5 floor (is the hand floor honest?);
   ACCEPTANCE: formal dd_σ under σ̂-weighting matches empirical DD-step
   statistics (calibration ratio → ~1× from today's floor-dependent 14%
   spread; the M3 ladder documents the artifact).
4. Field v2 (apply ON): 3h golden dd-step and withhold counts not degraded vs
   the hand-floor baseline, with honest formal σ. NOTE: no divergence-related
   acceptance — the hr-4 cliff is event-driven and out of this card's scope.
