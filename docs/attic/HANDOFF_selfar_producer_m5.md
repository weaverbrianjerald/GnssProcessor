# M5 — Self-AR producer: ambiguity-fixed network corrections

*Fable-specced 2026-07-11 at SCOPE level — interfaces bind to Card 4's z-store
(docs/HANDOFF_card4_invariant_carry.md), so implementation waits for M1.
Foundations: TK-2015 (proper PPP-RTK estimators; frac-of-float improper),
Psychas 2024b (frac operator delivers non-Gaussian biases — distributional
strike), Zhang 2021 (m−2 partial fixing). Supersedes the sketch in memory
`network-ar-producer-scope`. Acceptance harness ALREADY BUILT:
tests/test_eq34_consumer_integer.cpp.*

## Why (theory mandate, not just precision)

TK-2015 §5: the four proper correction estimators are GF/GB × float/fixed.
Our current emission is GB-FLOAT — proper as-is. The FIXED estimators buy
1/√n-class precision (GF) and, critically, make any fractional-bias-style
derivation legitimate (frac is proper ONLY on fixed solutions). Producer AR
also collapses correction reconvergence after satellite events — the Zhang
2021 combined-correction findings quantify what fixing recovers.

## Design (three stages, each gated)

- **S1. Float DD source = Card 4 z-store.** ẑ, Pᶻ per (const,σ) component IS
  the float DD solution MLAMBDA needs — no reconstruction pass, no extra
  covariance assembly. (This dependency is why M5 waits for M1.)
- **S2. Partial fixing policy.** Decorrelate (Z-transform) then fix the
  best-determined subset: success-rate-ordered bootstrapping with FFR gate
  (memory `ar-validation-criterion`: FFR both tiers), cap per Zhang 2021
  (fix ≤ m−2 per component/frequency class; GLONASS-specific caps deferred
  with L1). Never fix through a Def-1 innovation alarm (slip suspects).
- **S3. Constrain + re-emit.** Fixed subset re-enters the epoch solve as hard
  z-priors (integer targets, 1e10 — the Card 4 z-prior mechanism reused);
  biases re-emitted from the constrained solve are the AMBIGUITY-FIXED
  corrections. Disc/jump handling unchanged (fixing steps biases by exact
  integers by construction; emit-rebase absorbs them).
- Fallback duty: on FFR gate failure emit the float solution (never withhold
  for AR reasons); flag per-epoch `arfix=<n_fixed>/<n_cand>` on the S line
  (optional token, diagnostics).

## Config

```
network.producer_ar               bool  false  master; requires carry_invariant (Card 4)
network.producer_ar_ffr           double 0.999 fixed-failure-rate gate (both tiers)
network.producer_ar_max_frac      double 0.8   cap: fixed/candidates per component
```

## Validation

1. The EXISTING harness closes the loop: synthesize→consume is proven; add
   the producer leg — run the network on the synthetic-truth fixture class,
   assert the producer's fixed corrections reproduce the known integer frame
   (extends tests/test_eq34_consumer_integer.cpp fixture family).
2. Field: 3h golden + Card 4 armed — consumer A/B float-corrections vs
   fixed-corrections arms: consumer TTFF/ratio distributions improve; no
   sigma_gate regression. Wire steps at fixing events ∈ λℤ (grader I3).
3. Guard test: FFR gate forced-fail path emits float (no withhold, counter
   increments).
