# HANDOFF — Card-4 invariant-carry driver: perf restructure (the P0 pivot)

*Written 2026-07-14 (Fable), superseding the per-LOS focus of
[HANDOFF_perlos_parallel_sweeps.md](HANDOFF_perlos_parallel_sweeps.md) after its
own INC-P0 sub-timers attributed **~79% of "other" to the invariant-carry DD
driver** (dense K≈7834 DD covariance) — not the per-LOS sweeps (14.5%,
cov-consumers, the separate D1 card). P1/P2 of that handoff are PARKED pending
a post-pivot re-measure; its P0 instrumentation stays. This is the P0-first
discipline working as designed: the driver's fold/read-back was unbracketed
inside "other" and my §2 ranking missed it.*

## 0. Reconcile the finding with the design FIRST (INC-C0)

The driver header (`include/filter/NetworkInvariantCarryDriver.hpp`) claims:
`Pz` is **dense per (const,signal) COMPONENT** (`ZComponent::Pz`), chord death
**= marginalization** (`ZComponent::killChord`, "the covariance sub-block"),
counter `chords_marginalized` exists. "K≈7834 never marginalized" therefore
means one of:

(a) components merged into a few network-spanning giants (305-sta graph: K_c ≈
    #arcs − #nodes + 1 ≈ 2,600–3,900 per signal — two GPS signals ≈ 7,800 live
    chords, all genuinely alive for hours ⇒ marginalization is working and
    simply has nothing to kill), or
(b) the killChord path doesn't fire when it should (dead arcs retained), or
(c) both.

INC-C0 deliverable (extend the P0 sub-timers + counters, log-only):
- per-component census each emit epoch: K_c live, chords_marginalized,
  chords_tapped, K growth over the run;
- per-OP timing split inside the driver: (i) basis projection / unimodular T,
  (ii) time update (+B), (iii) the fold M=(Pz+B)⁻¹ + GMGᵀ
  (`foldReducedDdPriors`), (iv) posterior read-back (mean + the K_c×K_c
  covariance install), (v) D5 gate work;
- run at BOTH tiers: `_net305_3card_dev300s_gps_d8_em10_smoke.json` (fast
  harness; note D5 re-taps ~463/ep churn K there) and one 30 s 30m config
  (the golden-relevant cost).

Everything below is chosen by C0's per-op ranking — the same rule that caught
my wrong ranking applies to this list.

## 1. INC-C1 — exact quick wins (no representation change)

- **If (b):** fix the kill path; deletion of a departed chord's row/col in
  COVARIANCE form is exact marginalization; a returning arc is re-tapped fresh
  (C3c first-sighting semantics), so retention has no functional role. If
  deleting a dead chord changes ANY output, that is a latent bug to surface,
  not a tolerance to accept.
- **Growth bound:** whatever the cause, add a run-end counter line for max K_c
  and assert (warn) on monotone unbounded growth — this is also a long-run
  (12 h) robustness issue, not just wall-clock.
- **Avoid dense materialization where an op doesn't need it** (e.g. if the
  posterior install writes a full K_c² block that the next epoch only reads
  through (Pz+B)⁻¹, consider computing the fold from the factor the kernel
  already holds — but only if C0 shows the install itself is a top op).

Verification: these are exact/no-op-class changes — demand byte-identity on
emitted products + counters (same build, both tiers).

## 2. INC-C2 — the structural fix: factored Pz (border-rank + station blocks)

The physics: the solve covariance Σ is arrowhead, so the DD posterior
covariance is EXACTLY

    Pz = A + U S Uᵀ

with A block-diagonal (per-station conditional blocks, the (N_rr⁻¹) terms) and
U = K_c × n_s the border-propagation rows (m_c = N_sr N_rr⁻¹ e_c per chord),
S = shared free covariance. This identity is already implemented elsewhere in
the tree — `NetworkNormalEquations::jointConditionalCovariance` computes
exactly these two terms per entry. Dense K_c² is a representation choice.

Restructure the z-store to CARRY the factored form:
- **Storage:** A (block-sparse by station), U (K_c × n_s), S (n_s × n_s, one
  per epoch, shared across components).
- **Time update:** +B (diagonal q_z·dt) → A+B keeps the form.
- **Fold:** M = (Pz+B)⁻¹ by Woodbury on (A+B) + U S Uᵀ:
  O(K_c·n_s² + n_s³) instead of O(K_c³). At K_c≈3,900, n_s≈534 that is a
  ~50–100× flop cut on the fold.
- **Marginalization:** delete rows of U + entries/blocks of A — trivial.
- **The one messy part — unimodular T** (chord re-basis on tree-edge death):
  A → T A Tᵀ is no longer station-block-diagonal in general. Constraints that
  keep this tractable: T is component-local and touches few chords per event;
  allow bounded fill-in in A (block-sparse, not strictly block-diagonal), or
  re-tap the affected chords when fill-in exceeds a bound (a re-tap is the
  existing, S-legal reset primitive). SPEC THIS PRECISELY BEFORE CODING —
  this is the covariance-contract seam class (same class as D1) and gets a
  FABLE review gate on the algebra before implementation.

**Verification contract change — state it, don't blur it:** C2 is exact
algebra with different float op order → byte-identity is NOT the standard.
The standard is the Card-4 battery + field grade:
- unit: filter==batch equivalence (existing phase-B gate machinery),
  basis-invariance (gate 4), unimodular-T exactness, re-tap/marginalize
  lifecycle — all re-run against the factored store at ≤1e-8 class tolerances
  vs the dense reference implementation (keep the dense path compiled behind
  the toggle as the oracle);
- toggle default-OFF (`network.carry_invariant_factored`), OFF = dense path
  byte-identical;
- field: dd-frac window grade + D5/PB/re-base counters ON-vs-OFF within noise
  on a 30 s 30m run; wall-clock table from the C0 timers.

## 3. Out of scope / sequencing

- Per-component independence is already the design (components are separate
  Pz blocks) — do NOT additionally approximate across stations (dropping the
  U S Uᵀ coupling is NOT licensed; it would break the basis-invariance
  property the unit gates protect).
- The per-LOS sweeps (old P1/P2) and cov-consumers (D1) stay parked until a
  post-C2 re-measure re-ranks them.
- 300 s tier note: D5 re-tap churn (~463/ep there) directly stresses chord
  lifecycle — the tier is the cheap perf harness, but all GRADED verdicts at
  30 s (tier rules, memory `project_dev300s_tier`).
- Expected payoff envelope (sanity, not a promise): driver ≈ 34 s/ep of the
  ~43 s "other" at the dev tier → C1+C2 target ≤ 5–8 s/ep, i.e. epoch ~52 →
  ~20 s at 305 stations; a 3 h 30 s golden run drops from ~5 h toward ~2 h.

## 4. Pointers

`include/filter/NetworkInvariantCarryDriver.hpp` (ZComponent::Pz dense-per-
component, killChord, Counters), `NetworkNormalEquations::{foldReducedDdPriors,
ddPriorPosteriorMean, jointConditionalCovariance}` (the factored-form identity
lives here), `docs/HANDOFF_card4_invariant_carry.md` + phase-B/C handoffs (the
unit-gate battery to re-run), memory `project_m1_card4_phaseC` (fold algebra,
M=(Pz+B)⁻¹), `project_m1_card4_d5_innovation` (re-tap semantics),
`project_dev300s_tier` (tier rules + the P0 measurement chain that led here).
