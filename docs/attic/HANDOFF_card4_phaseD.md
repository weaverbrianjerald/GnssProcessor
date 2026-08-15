# CARD 4 phase D — emit-layer re-basing + gate 5 (Opus builds, Fable gates)

*Fable-specced 2026-07-12 after the C3c gate PASS (@967def1). Parent: spec D4/D5
(docs/SPEC_card4_invariant_carry.md); lineage in memory
`project-m1-card4-phasec3c`. Field config decision: **carry_invariant_qz=1e-4**
(beats q_z=0 post-slip-fix: 1.50 vs 2.43 cyc/ep; a00 22.1±3.1). The 6h/12h
hr-4 cliff-immunity runs are in flight (Fable); results land in memory —
phase D does NOT gate on them.*

## Current falsifier state (3h eq34 lineage, Release, reproduced by Fable)

| arm | DD step | a00 tail | calib |
|---|---|---|---|
| control (legacy eq34) | 8.29 | −96±110 | 22.3× (σ-incest) |
| carry ON, q_z=0 | 2.433 | +11.4±13.1 | 0.82 |
| carry ON, q_z=1e-4 | **1.498** | +22.1±3.1 | 0.49 |

Residual gap to golden 0.085 = honest shared-param σ-floor + genuine new-arc
births (separate axes, NOT phase-D scope). The consumer A/B below is the
arbiter of whether that gap matters.

## Scope

1. **Wire `carry_invariant_emit_rebase`** (parsed since C3b, currently inert)
   per spec D4: after each accepted solve, re-base the emitted per-(sat,σ)
   phase biases onto the previous EMISSION frame — subtract the integer×λ and,
   per (const,σ) component, the common fractional δ-shift that minimizes the
   step against the last emitted value. Outputs only (zero filter impact; user
   DDs invariant). The emitter's existing 0.05 m jump test REMAINS the disc
   authority — steps should survive only at genuine events (slips on
   emission-reference paths, component splits), never at datum churn.
   Component death/rebirth: a component absent from the previous emission
   re-bases against nothing (fresh frame, disc fires once — correct).
2. **Gate 5 (EmitRebaseInteger):** a forced tree change through the PRODUCTION
   driver + emitter seam ⇒ emitted per-key steps exactly 0 after re-basing and
   the disc counter silent; NEGATIVE control = re-base disabled ⇒ integer-class
   (≥1 λ) steps visible on the same assertion (numerical fail, not a throw).
   A slip on the emission-reference path ⇒ step survives + disc fires
   (the re-base must NOT launder genuine events — assert one).
3. **Counters:** emitted re-base integer total + common-fractional shift per
   component in the run-end Card-4 line (phase-D telemetry for the consumer A/B).

Out of scope: honesty instrument (Lemma-1), prediction-only conditioning
(field-inert, data-confirmed), GLONASS IE-FDMA, the 0.085-gap election levers.

## After the gate (Fable runs)
Consumer A/B with network iono supply: producer = eq34 + carry_invariant
(q_z=1e-4) + emit_rebase; staged consumer configs (_clAiono_pivot_3h /
_clBiono_eq34_3h lineage). Acceptance: MLAMBDA opens (ratio > 0) on the Card-4
arm; consumer vertical does not regress vs the M4 bias-only baseline.

## Gate (Fable)
Gate 5 per §2 incl. both negative controls; OFF byte-identity structural
(emit_rebase=true is the default when carry_invariant is armed — the OFF path
is carry_invariant=false, which must remain byte-identical); suite green
(674/1 baseline); deferral triage per Standing invariant #3.
