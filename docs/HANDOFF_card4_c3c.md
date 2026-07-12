# CARD 4 phase C3c — slip integration for the invariant carry (Opus builds, Fable gates)

*Fable-specced 2026-07-12 from the field-falsifier FAIL. Parent: spec D1
(docs/HANDOFF_card4_invariant_carry.md), C3b handoff (docs/HANDOFF_card4_c3b.md),
evidence in memory `project-m1-card4-phasec3b`. Build on HEAD (85a9587+).
Pre-flight against CLAUDE.md "Standing invariants" — #1 IS this card.*

## Why (the falsifier evidence — 3h eq34 lineage, Release, grade_ddfrac 1h tail)

| arm | DD step med | a00 tail | dd_σ |
|---|---|---|---|
| eq34 control | 8.29 cyc/ep | −96±110 TECU | 0.263 |
| carry_invariant ON (q_z=0) | **10.88 — WORSE** | **+498±392 diverged** | 2.09 (8×) |
| qz probe (q_z=1e-4) | **5.98 — beats control** | 107±112 physical | 2.16 |

Mechanism PROVEN by the probe ladder: stale-stiff-prior poisoning. The driver
has **zero slip handling** (spec D1's delete+re-tap was never built): a real
cycle slip shifts the true DD by integer n while the q_z=0 carry pins the
pre-slip value with accumulated sub-cycle stiffness → stiff wrong prior →
solve warps (iono absorbs the tension). The forgetful arm BEATING the control
proves correctly-projected carry already adds value — fix the event response
and the stiff carry should win decisively.

## Scope

1. **Slip feed → driver.** Per epoch, BEFORE `buildDdPriors`, hand the driver
   the set of slipped arcs (station, sat, σ) from the SAME trigger points that
   ERASE the phase-leveling local carry today (MW/Graphic detections, LLI,
   refloat/reset events — the network loop already localizes these; under
   carry_invariant the local-carry inject is bypassed but its slip triggers
   remain the authoritative event source). New driver API, e.g.
   `markArcSlipped(rx_vertex, sat_vertex)` accumulated into beginEpoch state.
2. **Projection semantics (exact, in the projection-W architecture):**
   - An old carried chord whose fundamental cycle CONTAINS a slipped arc is
     POISONED → drop it (marginalization = row/col delete, exact) before
     computing W.
   - A slipped arc is EXCLUDED from the old-arc membership set for W, so any
     new cycle whose support includes it cannot be projected → fresh
     (diffuse → data-only tap-off), exactly the v1 birth path.
   - This subsumes spec D1's "pivot first, then refloat" (that phrasing was
     for the phase-B primitive store; with projection-W, exclusion + drop is
     the equivalent exact operation).
3. **RAIM interaction:** a phase row RAIM-rejected mid-epoch flips its chord
   to unmeasurable for the REfold (re-derive measurability in the rebuild —
   the C3b handoff's H13 rule already requires refolding from unchanged
   predicted state; this adds the culprit-withhold).
4. **Counters:** `slip_kills` (chords dropped by slip) + `chords_dropped`
   (the ~1500 currently-silent gap drops — make them visible; today's
   run-end line shows 2534 births / 7 marginalizations / 1022 alive, which
   doesn't balance) in the Card-4 run-end line.
5. **Unit gates (production driver, not a reimplementation):**
   - Inject an integer jump on a CHORD arc mid-sequence with the slip
     signaled → post-slip estimable DD matches a no-carry reference at the
     slip epoch (uncontaminated) and re-converges after; carried entries not
     touching the slipped arc keep their accumulated variance (no collateral
     erasure).
   - Same for a TREE arc (all cycles through it poisoned → dropped; others
     survive).
   - NEGATIVE control: same jump with the slip NOT signaled → contamination
     visible (>1e-3-class deviation on the estimable DD) — proves the wiring
     is load-bearing, not decorative.
6. **Falsifier re-run** (configs staged: `_eq34_only_3h`, `_eq34_carryinv_3h`,
   `_eq34_carryinv_qz_3h`; ~5 min each in Release): acceptance = the ON arm
   enters the **0.085 cyc/ep golden class** with a00 physical and counters
   sane (tree swaps ≈ arc-death rate, slip_kills ≈ slip rate, zero refloat
   storms). Interim sanity: it must at minimum beat BOTH 8.29 (control) and
   5.98 (forgetful probe) stiff — if it lands between, the event feed is
   incomplete (hunt for unsignaled event classes: state resets, arc re-keys).

## Out of scope (unchanged deferrals)
Prediction-only conditioning (Fable §1 recipe in the C3b handoff), emit-layer
re-basing + gate 5 (phase D), honesty instrument, GLONASS IE-FDMA.

## Gate (Fable)
Slip unit gates per §5 incl. the negative control; falsifier table per §6
reproduced by Fable same-build; OFF byte-identity structural review (slip feed
must be gated `carry_invariant`-only); suite green (673+ baseline); deferral
list re-triaged field-blocking vs field-inert per Standing invariant #3.
