# QPR — the charter — why this queue exists, how an item enters and leaves it, and the rank order

**STATUS**

- **id:** QPR
- **topic:** queues
- **status:** open
- **summary:** the charter — why this queue exists, how an item enters and leaves it, and the rank order
- **dates in the text:** 2026-08-05 → 2026-08-21
- **knobs / posture:** `network.cycle_slip.relevel_free_epoch`
- **hashes named in the text** (commit / exe / byte-cert): `ca19d0d`, `353f99f`, `58f650c`
- **run dirs named in the text:** `runs/inj_weab_`, `runs/pareref_on`
- **source:** `docs/archive/QUEUE_principled_replacements.md` — (preamble)

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: QUEUE_principled_replacements.md -->
# QUEUE — principled replacements for symptom-level patches (cross-family index)

2026-08-14. Which standing "bandaid" patches — symptom filters, hand tunings, avoidance rituals,
binary backstops — have a known theoretically correct replacement, and in what order to build them.
Written from the pivot-gauge handover review (`docs/SPEC_pivot_visibility_gauge_defect.md` §5.1,
implementation `ca19d0d`). ⚠ **The validation arms LANDED** (`353f99f`, `58f650c`) and the handover
alone does NOT close the defect: the prioritized union already suppresses the legacy deletion (1
col_removed vs 6) and the prioritized arm's residual churn is a THIRD root with three hypotheses
falsified — detail in `SPEC_pivot_visibility_gauge_defect.md` §5.0–§5.08, memory
`project_pivot_visibility_gauge_defect`.

**Relationship to the existing queues (do not reconcile by guessing):**

- `docs/HANDOFF_edge_response_queue.md` (2026-08-05) ranks the BARE frame/coupling EDGES by
  measured damage (5 edges). This doc ranks ACROSS families (edges + test statistics + numerics),
  post-dates the pivot-gauge card, and re-scores readiness. Where they overlap, the edge queue
  carries the measurement detail; this doc carries only the ranking. ⚠ Its session-constraints
  block encodes a phase-specific run budget (the 2026-08-05 "ultra-fast turnaround" directive) —
  do not inherit it blindly.
- `docs/SPEC_level_information_budget.md` §27 ranks LEVEL next-actions and §30 (LV-3) declares the
  level frame. Both stand.
- The open valve inventory (valves with a bare edge) is
  `docs/SPEC_prefit_census_observability.md` §13 (= edge 5 of the edge queue).

Ranking = measured payoff × readiness, scored 2026-08-14. Every item names its sim-bench validation
shape — the synthetic network (truth-graded, injectable) is the venue for all of these.

**BUILD ORDER — CURRENT (2026-08-14, fourth ranking).** Item numbers are STABLE — cited by section
from other docs and from `include/config/GnssConfig.hpp:3469` and `scripts/t3_null_fit.py`, so a
re-ranking is expressed HERE rather than by renumbering.

> ### **9 → 8(b) → 4(R2/R4) → 2 → 1(b) → 5 → 7 → 6**  *(fifth ranking, 2026-08-20)*
> with **8(a) EXECUTED 2026-08-20 (verdict: PERSISTS — see §8)**, **4(R1) HELD**,
> **3 dissolved into 1(d)**, **0 downgraded**, **1(a) demoted**, and
> **R0 + 1(d) already EXECUTED, LANDED and VALIDATED** (2026-08-14).
>
> **Item 8(b) — EXECUTED 2026-08-20/21 (verdict: the carry family is FALSIFIED
> at all four points — solve-fed × {relevel, always} and CS-GF code-anchored ×
> {relevel, always} — with the wiring proven live; the EVENT channel is
> exonerated and the plateau is the per-epoch exchange equilibrium under
> mid-arc level lock-in.  `SPEC_iono_carry_8b.md` §6.5/§6.6; execution entry
> in §8(b) below.  Everything default-OFF, byte-certified.  The next 8-family
> card was the O2 / carry_invariant-lineage accumulator — **DERIVED AND CLOSED
> UNBUILT 2026-08-21, `docs/SPEC_o2_carry_invariant_accumulator.md`, never
> ranked.  Three independent sufficient reasons: the channel already exists
> (mech-B, τ = 900 s, armed on the whole pdfix lineage, P3 measured its removal
> null); two of the three iono-error subspaces are EXACT null directions of the
> data rows (gauge, not estimable — they belong to item 5 / LV-6); and the third
> is floored at σ_eff 0.35–0.45 cyc, invariant across five representation
> interventions × two q_z values × two lane arms.  The 8-family is CLOSED.**
> Its one forward lead is not an 8-family question: the measured cross-band
> residual direction is **common-cycle, not ionospheric** (the iono direction has
> never been observed), matching the truth-graded equal-in-cycles component of
> `SPEC_synthetic_minimum_network.md` §13.6 with phase-windup as the named
> candidate.**
>
> **Item 9 — EXECUTED 2026-08-20 (same session): the walk is DEAD under
> `network.cycle_slip.relevel_free_epoch` (+ `relevel_delta_bridge`), built,
> suite-green, byte-certified, validated on the slipped fixture (ZWD hourly rms
> FLAT 2.3–3.1 m vs 2.6→6.5 √t; `SPEC_slip_relevel_walk.md` §8).  The deeper
> code fact: the pending-reseed mark was CONSUMED by PASS-B before the inject
> read it — the leveling slip refloat had been structurally inert all along.
> Residues: the stationary ~2.3–3.1 m plateau (→ 8(b)), bridge v2 cross-epoch
> pairing (3 959 unpaired MW candidates), retap truth-neutral (default-OFF),
> field arm owed.  Original filing below.**
>
> **Item 9 was NEW and ranked FIRST — the slip-RE-LEVEL level walk**
> (`docs/SPEC_slip_relevel_walk.md`): on the first long/multi-GNSS/realistic-slip
> truth fixture, every refloat re-levels its arc from ONE epoch of code and freezes
> the result (zero-q), injecting station-common metre-class iono error into the
> leveling datum; the ZWD/clock exchange integrates the steps into a √t walk
> (6 h → ~6.5 m ZWD rms, clock contrast 14 m) with EVERY internal instrument
> silent.  Isolated by a six-arm ladder: iono posture / arc persistence /
> coupling stack / Galileo all exonerated; the QUIET twin converges (0.15 m).
> The T4 arc-refloat storm (21 k arcs / 6 h) is a co-symptom riding the same
> iono-level error — halving it moves nothing, so 4(R1)-class statistic work
> stays SECOND to fixing the re-level edge.  Fix options §6 of the SPEC
> (predicted-phase re-level / validated-delta carry repair / post-re-level vet
> window / at minimum the injection counter).  This is the measured reason a
> "robust network run" does not yet survive a realistic slip environment.

Why this order — the measurement that earned each call lives in the item's own section:

- **8 is NEW and ranks FIRST.** Item 4's investigation ended by measuring that the T3 false-alarm
  floor, the per-station residual heterogeneity, the L1C excess AND the integer-resolution failure
  are **ONE defect: per-LOS ionosphere estimation error** (`SPEC_t3_null_derivation.md` §7.8-§7.9).
  **8(a) is one knob on an existing arm** and decides the scope of everything else.
- **4(R1) — the stratified per-station variance component — is HELD, not scheduled**: it would
  absorb a measured 0.11-2.55 m per-station ionosphere error into the noise model, i.e. *bury the
  root*. **4(R2) (declare the station factor as its own statistic) and 4(R4) (population hygiene)
  survive and stay cheap**; 4(R3) (FDR) is correct but second-order.
- **5 (the level) slips behind 8 and re-scopes** — same ionospheric field, smaller term (overall
  level μ 0.328 m vs the 0.764 m rms datum-free interaction).
- **2 outranks 1 but slips behind 4/8**: datum election is measured 5.9× on the datum-free C
  contrast with a 40 % T3 cut and its remaining work is plumbing — but electing a better datum
  reduces *exposure* to an edge that 1(d) *fixes*.
- **1(a) is DEMOTED** — measured **null** on the channel it targets. **3 is DISSOLVED into 1(d)**:
  the root was never clock work, it is phase-bias-datum CARRY work
  (`SPEC_pivot_visibility_gauge_defect.md` §5.07); item 3 retains only always-armed marking and
  1(b). **0 is DOWNGRADED** — a measured net LOSS on the clock channel (its 1.9× code-bias win
  stands; see §0).
- **The D5 / "re-deciding the representation" hold is substantially lifted** — it was held as "a
  science program, not a patch swap" only because the representation error had never been
  quantified. It now is, with a truth-graded instrument.
- ⚠ **`clock_monitor_couple_raim` is NOT the fix** despite naming the right coupling: **85.5 % of
  Σ|dev| is emitted before the stash it acts through.**
- **2, 1(b), 7, 6 are unchanged in kind** and simply move down.

*Provenance (superseded orders, kept so a stale citation can be placed):* (1) `0 → 2 → 4 → 1(b) →
5 → 7 → 6`; (2) after the third root was named, `R0(telemetry) → 1(d) → regenerate the null arm →
4 → 2 → 1(b) → 5 → 7 → 6` (A/B `runs/inj_weab_{on,off}`, configs `_simnet24_inj_weab_{on,off}.json`);
(3) after R0 and 1(d) landed and item 4 was derived, `4 → 2 → 1(b) → 5 → 7 → 6`. The "regenerate the
null arm" step is satisfied by **`runs/pareref_on`**, and the pre-fit gate passed on it: σ̂₀_first at
the assessed handover epochs reads **1.000–1.323** against a run median of 1.247
(OFF arm 1.37–13.27 against 1.83), σ̂₀_first ≡ σ̂₀ at every one ⇒ a clean null instrument.

---

