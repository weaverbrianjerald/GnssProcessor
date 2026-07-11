# CARD 2 — Code-bias datum temporal discipline: time-linked gauge targets

*Fable-specced, Opus-executed (2026-07-10). Execute ONLY after Card 1
(docs/HANDOFF_carry_rigor_card1.md) has landed and its falsifier passed. Do not deviate;
where ambiguous, stop and report. Source: memory `project_opus_ssystem_audit_jul9`
(defects 6-7: temporal double-datum, IF-pin re-selection), codebias-rank audit finding
[0]-[1]; theory: Odijk-2016 §4.1 set-dependence warning, Hou-Zhang two-transformation,
theory-doc §7 item 10 (no silent re-datums).*

## The defect

Every epoch the driver re-imposes the code-bias datum over the CURRENT satellite set at
weight 1e3 (grep `kCodeBiasGaugeWeight` in ApplicationNetwork.cpp): one IF-pin row per
satellite (fixes the code-clock split) + one sum-to-zero row per (constellation,signal)
(fixes the channel level). The same shared columns carry the near-zero-q D2/C1 prior.
On any rise/set the s2z TARGET shifts by (departed bias)/m′, the stiff carry resists,
and the level RELAXES over many epochs toward the new target — a hysteretic, unsignalled
common-mode wander in the emitted OSBs that leaks into the emitted clock through the
(equally soft) IF pin. The disc counter fires once AT the set change while the value
drifts AFTERWARD — the worst ordering for a consumer holding ambiguities. This is the
mechanism behind the documented ~0.9 m code-clock wander.

## The fix (exact design — time-linked datum, "pin the change, not the level")

Toggle `network.code_datum_time_linked` (default **false** = byte-identical). When on,
the SAME number of datum rows is imposed per epoch (rank unchanged — the null-space gate
must still pass), but their TARGETS reference the previous epoch's solved values instead
of zero:

1. **Channel level (per constellation,signal):** the sum-to-zero row spans the present
   satellites that ALSO have a previous-epoch solved value ("veterans"), with RHS
   `Σ_veterans b̂_s,k−1` instead of 0 — i.e. the row pins the CHANGE of the veteran mean
   to zero. Membership changes no longer move the target (each veteran is referenced to
   itself); new satellites take their level from the network + the veteran row. A channel
   with NO veterans (cold start, warm-resume miss) seeds at Σ = 0 exactly as today — and
   that seed is a REAL re-reference: bump the disc counters of the channel's satellites
   AT that epoch (markSatDiscontinuity), so value step and disc coincide.
2. **Code-clock split (per satellite):** the IF-pin row's RHS becomes the satellite's
   previous-epoch solved `μ_IF·b̂_s,k−1` instead of 0 (seed 0 at first appearance, disc
   bump on any later re-seed). Note the existing opt-in `clock_signal_basis_hold`
   overlaps in intent — when `code_datum_time_linked` is on, it subsumes that behavior
   for the pin VALUE; do not run both mechanisms on the same row (assert/warn at load if
   both armed).
3. **Deterministic pair selection:** the IF-pin pair must be a deterministic function of
   (sat, signal-set): stable sort by (band digit, full signal name). A pair CHANGE for a
   satellite (channel appears/disappears) is a re-seed of its split → disc bump at that
   epoch. (A spawned chip "Make IF-pin pair selection deterministic + signal re-latch
   disc" may already have landed parts of this — grep for a stable-sort comparator at the
   pair-selection site first; implement only what is missing.)
4. Previous-value bookkeeping: one map per (const,signal) → per-sat b̂_prev, refreshed at
   harvest from the solved shared vector; entries dropped when a satellite leaves (its
   return = new seed for IT, veterans unaffected); NOT persisted in the snapshot (a warm
   resume re-seeds with disc bumps — correct and signalled). Add the toggle to the
   snapshot fingerprint (estimation-relevant).

Why this is the right datum: the gauge still removes exactly the same 1+m DOF per
channel (level + per-sat split) — but as a TIME datum (continuity) rather than a
per-epoch set-mean datum, which is the Odijk-consistent cure for CC-S set-dependence and
eliminates the gauge-vs-carry weight fight entirely (the target now agrees with the
carry by construction; no relaxation dynamics exist).

## Code anchors (re-grep; lines drift)

Gauge construction: `kCodeBiasGaugeWeight`, `addSharedConstraintRows`, the per-sat IF-pin
+ per-(const,signal) s2z block in ApplicationNetwork.cpp (audit-era ~4152-4292). Pair
selection sort (~4196-4211 audit-era). Disc: `markSatDiscontinuity` (emitter). Previous
shared values: the solved x_s + shared key layout (same identity strings the D2 carry
uses). Null-space gate: the reduced-normals estimability check that must keep passing.

## Validation (in order; stop on failure)

1. **Unit test — set-change continuity:** synthetic 2 epochs, one channel, satellite
   leaves between epochs. OLD (toggle off): surviving biases step (level shifts by
   departed/m′). NEW: surviving biases continuous to solver precision, no step, no drift;
   null-space gate rank identical. Second test: forced cold re-seed bumps the channel's
   disc counters at the SAME epoch as the value step.
2. Suite green (≥ Card-1 count); byte-identity OFF: `_t4_gg_treated_smoke.json` SHA of
   network_solution.csv unchanged vs the current tree's OFF baseline (69C20D38E22B5EBE
   if the Card-1 OFF path held it; otherwise regenerate the baseline FIRST on the
   pre-Card-2 tree and compare against that).
3. **Falsifier — the hysteretic wander:** `_t4_gg_treated_3h.json` A/B (toggle off vs
   on, full floors). Metric (small python, spec exact): from the emitted products, per
   (const,signal) channel compute L_cs(t) = mean over present sats of the code-bias
   value; report (a) max |ΔL| between consecutive epochs NOT accompanied by a disc bump,
   (b) total |L(end) − L(start)| over the final 2 h, (c) alignment: every |ΔL| > 0.05 m
   must coincide with a disc bump. SUCCESS = (a) drops to noise (< ~2 cm vs the baseline
   arm's post-set-change relaxation tails), (c) holds exactly; ALSO the emitted-clock
   per-sat temporal std must not degrade.
4. **Joint falsifier with Card 1:** rerun the Card-1 12 h low-floor config with BOTH
   toggles on — no divergence, and the code-bias block's contribution to a00/clock
   wander visibly reduced vs the Card-1-only arm. Report numbers for both tails; either
   outcome is a finding — do not tune.

## Out of scope

Eq-34 phase datum (Card 3 — specced after Cards 1-2 validate), GLONASS per-slot IFBs,
wire-format changes, emitted-σ changes, floors' values, the T5 GM-deviation code.
