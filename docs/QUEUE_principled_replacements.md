# QUEUE — principled replacements for symptom-level patches (cross-family index)

2026-08-14.  Written from the review session for the pivot-gauge handover card
(`docs/SPEC_pivot_visibility_gauge_defect.md` §5.1; implementation committed
`ca19d0d`).  ⚠ **The validation arms have LANDED** (`353f99f`, `58f650c`) and the
handover alone does NOT close the defect: the prioritized union already suppresses
the legacy deletion (1 col_removed vs 6), and the prioritized arm's residual churn
is a THIRD root with three hypotheses falsified.  Item 1's sub-cards have been
re-scoped against those findings, a new item 0 added, and the build order amended
— see below.  Detail: `SPEC_pivot_visibility_gauge_defect.md` §5.0–§5.06, memory
`project_pivot_visibility_gauge_defect`.
The question it answers: **which standing "bandaid" patches — symptom filters,
hand tunings, avoidance rituals, binary backstops — have a known theoretically
correct replacement, and in what order should we build them.**

**Relationship to the existing queues (do not reconcile by guessing):**

- `docs/HANDOFF_edge_response_queue.md` (2026-08-05) ranks the BARE frame/coupling
  EDGES by measured damage (5 edges).  This doc ranks ACROSS families (edges +
  test statistics + numerics), post-dates the pivot-gauge card, and re-scores
  readiness accordingly.  Where they overlap, the edge queue carries the
  measurement detail; this doc carries only the ranking.  ⚠ Its
  session-constraints block encodes a phase-specific run budget (the 2026-08-05
  "ultra-fast turnaround" directive) — do not inherit it blindly.
- `docs/SPEC_level_information_budget.md` §27 ranks LEVEL next-actions and
  `docs/CONTRACT_level_authority.md` (LV-3) declares the level frame.  Both stand.
- The open valve inventory (valves with a bare edge) is
  `docs/SPEC_prefit_census_observability.md` §13 (= edge 5 of the edge queue).

Ranking = measured payoff × readiness, scored 2026-08-14.  Every item names its
sim-bench validation shape — the synthetic network (truth-graded, injectable) is
the venue for all of these; several are gradable there in ways the field never
allowed.

**BUILD ORDER (amended 2026-08-14 after the item-1 validation arms landed).**
Item numbers below are STABLE — they are referenced by section and by other docs,
so the re-ranking is expressed here rather than by renumbering:

> **0 → 2 → 4 → 1(b) → 5 → 7 → 6**, with **1(a) demoted** and **3 held**.

Rationale, applying this doc's own "measured payoff × readiness" rule to the
arms that have since been measured (`SPEC_pivot_visibility_gauge_defect.md`
§5.0–§5.06):

- **0 is new and ranks first**: one knob, already built, measured 1.9× on the
  code-bias channel. Zero build cost.
- **2 outranks 1**: datum election is measured 5.9× on the datum-free C contrast
  with a 40 % T3 cut, and its remaining work is plumbing. Item 1(a) is measured
  **null** on the channel it targets.
- **3 is HELD, not scheduled**: four independent phase-bias-gauge interventions
  are now null on the clock channel (see "Blocked on root cause"). Do not build
  more gauge work against it until the third root is named. 1(b) survives because
  it acts on a *different* edge (the clock datum handover, not the PB gauge).

---

## 0 — Arm the pin bridge (one knob, already built, already measured)

**Bandaid:** none — this is a validated mechanism sitting unarmed.  It is listed
first because by this doc's own ranking rule nothing else comes close on
payoff × readiness.

`network.phase_datum_time_linked`.  `NetworkAnchorHygiene.cpp:231`'s `want_bridge`
is reached only when `handover_pending`, and its `min_arc_ep <= 0` disjunct is the
DEFAULT — so arming the knob bridges **every** handover: the key holds its shared
phase bias at `pb_level_prev` through a soft row (σ = 0.05 m) with **no station
pin at all**.  The gauge moves to the SHARED side, station-independently.  It is
reachable from the prioritized anchor election today, not eq34-only.

**Measured** (24-sta arm, single knob vs `_simnet24_inj_null_pdp.json`):
27 bridges opened, 453 row-folds, **0 reattaches performed** — the gauge never
moved to another station.  **B rms 1.573 → 0.813 m (1.9×)**, datum-free C
contrast 18.655 → 17.214 (7.7 %).  It does NOT reduce the clock re-realizations
(7 → 8 episodes, Σ|dev| 160.5 → 165.3 m) — see item 3 and the blocked section.

**Remaining work:** a field arm, and a decision on the σ and `bridge_max_ep`
defaults (0.05 m / 20 ep were never tuned for this use).  Config:
`config/test_suite/_simnet24_inj_bridge.json`.

---

## 1 — Complete the S-transform re-reference family

**Bandaids today:** the PB emission-continuity filter — v1's `emit_pb_filter_jump_k:
8.0` is a retired hand tuning, v2 measured null-to-negative (`runs/agu63_pbv2`) —
filters the WIRE while **91 % of emitted PB discontinuities are ref-path slip
re-referencing** (AGU-26 amendment 2) that no emission filter can reach; and the
one-epoch fold-skip "free fall" at clock-pivot handover (`handover_affects`
mirrored across **4 files / 14 occurrences** — `NetworkRunContext.cpp` 5,
`NetworkClockQc.cpp` 5, `NetworkStashStatus.cpp` 3, `NetworkSolveGlue.cpp` 1;
re-counted 2026-08-14, the earlier "five sites" was stale and the number scopes
1(b) — see `docs/SPEC_handover_stransform.md` §1).

**Principled replacement:** an S-transform at every re-reference edge, in three
sub-cards:

- **(a) Slip-triggered continuous handover in the prioritized PB election.**
  ⚠ **DEMOTED and RE-SCOPED 2026-08-14.**  When the pin-holder's arc slips, elect
  an unslipped peer and hand over at the ledger target — the mechanism half of the
  review's slip-hole finding (the election currently receives NO slip input;
  `carry_slipped_ambs` at `ApplicationNetwork.cpp:1441` already has the ledger's
  exact `pair<int,string>` key type, and the eq34 election already takes it).
  Machinery (ledger, counters, PBGAUGE trace) landed `ca19d0d`.

  **The premise changed.**  With item 0 armed the pin bridge INTERCEPTS every
  handover before the ledger is ever consulted (measured: 27 bridges, **0
  reattaches performed**), so the two mechanisms compete for the same edge and the
  bridge already measures better.  The handover itself measured null on its target
  channel: C contrast 18.655 → 18.604, episodes 7 → 7, and every episode merely
  DEFERRED by exactly one epoch — the signature of a one-epoch-stale ledger
  (§5.0.1).  The remaining question is therefore not "build it" but the much
  smaller **"does a slip-triggered handover add anything over bridging?"** — which
  the fault fixture can answer directly, since a slip on the pin-holder is exactly
  the case where `prior_fresh` may fail and the bridge declines to open.  Answer
  that before writing any election code.
- **(b) `docs/SPEC_handover_stransform.md`** — SPEC'd 2026-08-04, unbuilt:
  re-express carried priors at the clock-datum handover instead of the free-fall
  skip; exact and cheap per LASER (S := I − Eᵀ(KEᵀ)⁻¹K on mean AND dispersion).
- **(c) eq34 chain-datum handover continuity** — named "a separate card" by the
  loader guard in `GnssConfigLoadNetwork.cpp`; unwritten.  This is the FIELD
  posture's instance (the GG-Q lineage's 2 avoidable unmarked G01 events).

**Sim validation:** (a) fault fixture `_simnet24_inj_fault` with a slip injected
at the pin-holder / next-ranked candidate on a known handover epoch; grade
PBGAUGE continuous/cold counters, the emitted PB disc census, and the datum-free
C contrast.  (b)/(c) per their specs, eq34 on the sim tri-slant posture.

## 2 — Datum-station election by criterion, not luck

**Bandaid:** the field posture dodges the pivot-gauge defect only because
MBAR00UGA was hand-picked (equatorial, loses almost nothing) — §4.2 of the pivot
spec calls it "the §5.1 remedy arrived at by luck"; the 300-sta sim arm's default
datum carries 8/8 avoidable events; `simnet_make_producer_config.py` warns about
the POLAR fallback only.

**Principled replacement:** election ranked on satellite-visibility continuity
(`scripts/datum_gauge_audit.py --fixture` already ranks rosters), equatorial
preference among ties (§9.8: 8× wander from a polar pivot), formalized in the
config generator + a field-config preflight; `--run` as a harness gate (exit 1
on any avoidable removal).

**Sim validation:** already done — the three-arm prediction test measured 5.9×
C-contrast and a 40 % T3 cut for clean election.  Remaining work is plumbing.

## 3 — Clock: from per-event patches to a re-realization-free posture (= edge 2)

**Bandaids:** CLK-R emission re-base (correct triage — census 613→1 — but applied
at the emission boundary while the estimator underneath still re-realizes; the
clock is the worst re-realizer, 13 product-wide jumps/12 h vs 2 CB / 0 PB) and
unarmed markers on field lineages (`emit_clock_disc` unset ⇒ all 13 silent).

**Principled replacement — with the falsification honored:** datum-choice-at-emit
is NOT the consumer fix.  Ensemble PB datum, clock sum-to-zero, and
broadcast-anchored were all A/B'd dead as consumer-visible improvements —
common-mode is consumer-absorbable (`docs/SPEC_handover_stransform.md` "what this
card is NOT"; `project_network_falsified_approaches`).  The principled path is
producer-internal: the carry S-transform at the clock handover (item 1b), pivot
ELECTION as a product-quality decision (datum-free contrast moves 2.2× with the
pivot's geometry, §9.8), sum-to-zero for wire-frame wander only (sim PASS
0.193 → 0.036 m; field arm still owed), and always-armed marking
(`emit_clock_disc` + `clock_disc_jump_test`) so whatever remains is at least
declared to the consumer.

⚠ **HELD 2026-08-14 — do not build this as more gauge work.**  FOUR independent
phase-bias-gauge interventions are now measured null on the clock channel (datum
election on the prioritized path, handover-continuous, the pin bridge, the
coverage gate); see the blocked section.  Of the list above, only **always-armed
marking** and **1(b)** survive that evidence unqualified — marking because it is
declaration rather than estimation, and 1(b) because it acts on the CLOCK datum
handover, a different edge from the PB gauge those four all touched.  Pivot
election and sum-to-zero stay listed but inherit the caveat.

**Sim validation:** the §9.8 synthetic 50-sta matrix is the precedent; edge 2 of
the edge queue holds the candidate list.

## 4 — T3-class test statistics: correct nulls, not recalibrated constants

**Bandaid:** a Gaussian 6σ quantile under a statistic that is a max-over-rows
normalized by a median-over-rows.  Measured implied α: 3.78e7× nominal on the
quiet 24-sta fixture, 2.7e7× on the quiet 300-sta fixture; excess kurtosis 4391;
the implied-scale-vs-quantile curve rises monotonically (1.83 → 313) ⇒ the
residue is SHAPE, not scale, and a 4.5 % floor survives datum election (pivot
spec §6).  Sibling in class: the rx-CB admission gate's flat 26.7–28 % trip rate.

**Principled replacement:** derive the null of the statistic actually computed —
extreme-value / order-statistics of correlated standardized residuals — or
replace it with per-row w-tests under FDR control.  The constraint-aware σ̂₀
card (`docs/SPEC_sigma0_constraint_aware.md`) is the in-house proof this
derivation pays.

**Sim validation:** the truth-clean fixtures ARE the null-calibration
instrument: fit the corrected quantile on the 24-sta null arm, verify implied
α ≈ nominal on an independent null, then check the field flag rate.

⚠ **The verification arm must be SAME-REGIME, different-seed — NOT the 60-sta
fixture.**  `runs/simnet60_tri` is LEGACY-N (N ∈ [1e5,1e6)) and a different
posture, while the 24-sta null is field-N; the regime alone moves the DD channel
by 24 points (`project_sim_field_n_regime`).  Fitting on one and verifying on the
other would conflate REGIME with STATISTIC, and could pass or fail for reasons
that have nothing to do with the quantile.  Generate the verification fixture
from the same `config/sim/simnet24_inject.json` with a different roster salt —
the roster is deterministic in N, so only the noise/integer realization moves.

**⚠ NOT in this class:** MW slip thresholds — empirical by standing decision
(data-rate-dependent); do not "theorize" them.

## 5 — The level: enforce the written contract (= LV-3 + edge 3)

**Status:** further along than it looks — `docs/CONTRACT_level_authority.md`
already declares per-posture ownership and measures the accident (rx-CB carry
~100 info/epoch on the level direction vs the designed authorities' 0.25/0.01);
LV-2's demean removes the carry's net force.  The card's own verdict stands:
freeze ⇒ +20 TECU, loosen ⇒ +25, remove ⇒ −9/epoch collapse — the carry is the
wrong vehicle in every tuning direction.

**Remaining:** a per-epoch data-derived authority for postures where the
designed owners are weak — INTERNAL data only (GIM stays diagnostics-only,
standing directive).

**Sim validation:** the one venue where the LEVEL grades against truth directly
(the sim provides truth iono; the field never could).  ⚠ Level claims need the
≥2 h convergence budget — 1 h windows are rank-only.

## 6 — Retire the half-epoch back-off ritual

**Bandaid:** every `start_offset_hours` is backed off half an epoch because bare
hour values sit on a double-residue boundary (CLAUDE.md "cold-start hygiene") —
a floating-point bug institutionalized as ritual.

**Principled replacement:** integer week/TOW arithmetic or round-to-nearest-epoch
semantics at the epoch seek; then delete the convention from CLAUDE.md.  Bundle
the recorded leap-second lookup defect (memory `project_point_codebase`,
`TimeUtils.cpp` leap table) into the same hygiene card.

**Sim validation:** unit tests + a bare-hour config sweep on the 24-sta fixture
that must produce epoch sets identical to the backed-off twins.

## 7 — SH health gate release policy (= edge 4)

**Bandaid:** a binary withhold with no release/degrade path — a sick-field era
withholds the wire for its full duration (95+ min measured, DOY-129), and the
doctrine itself grades a withhold as an adaptation failure.

**Principled replacement:** attribute-adapt-emit within the detected epoch, with
the release decision evaluated against the ADAPTED solution — the
degrade-during-withhold DEADLOCK is already documented
(`docs/SPEC_day129_gate_policy.md`); the fix must not re-test against the
withheld state.

**Sim validation:** needs an injected sick-field / storm mode in the injection
bench — a bench extension, owed before this card can be graded.

---

## Blocked on root cause — do not build the "correct" fix yet

- **⚠ NEW 2026-08-14 — the clock re-realization churn itself (item 3).**  FOUR
  independent interventions on the phase-bias gauge left it untouched on the
  24-sta arm: datum election on the prioritized path,
  `phase_datum_handover_continuous`, the pin bridge, and the
  `phase_datum_min_stations` coverage gate (that last was byte-identical at 1/2/3
  — the gate never binds at this density).  Every arm holds at **7–8 episodes and
  Σ|dev| ≈ 160 m**.  Three candidate roots were tested and falsified: anchor
  handovers (compensated, no effect), column-set changes (0/15 correlate), and the
  CLOCK datum station's own visibility (1/6 — the mechanism that DOES explain the
  legacy PB defect).  Four nulls plus three falsifications is strong evidence this
  is **not a gauge problem at all**.  Name the root before building item 3.  Live
  lead: the episodes behave like threshold crossings of a slowly-evolving
  quantity — arming the handover knob shifted every one of the seven by exactly
  one epoch.

- **Trio-runaway (settled triple-freq divergence):** the INC-1 FREEZE is a
  bandaid that measured as ~35 % contributor, NOT root (runaway survives
  1720 → 1132 cyc).  Mechanism card first (suspects: tree re-hosts refolding
  invariant carry; chord-carry × slant-iono feedback).  FLOAT stays the track.
- **D5 born-chord gating:** the lane spectrometer verdict is that chord churn IS
  the iono representation.  Replacing D5 means re-deciding the representation
  (slant vs SH) — a science program, not a patch swap.  Keep D5 until then.

## Leave alone — falsified or decided; do not re-litigate

- ZOH clock dynamics (C1: the rate state measured net-harmful; ZOH is MMSE here).
- MW slip thresholds empirical (standing user decision).
- Lane layer OFF (measured at both 63 and 305 stations, settled windows).
- Era-2 adaptive iono q (falsified).
- Datum-choice-at-emit as a consumer fix — ensemble PB, clock s2z,
  broadcast-anchored (falsified; see item 3).
- Reset-over-repair and withhold-as-backstop doctrines — principled; the gap is
  item 7's release policy, not the doctrines.

## Maintenance

Strike items as their cards land; this doc indexes, the SPECs carry the detail.
When item 1a/1c land, update `docs/SPEC_pivot_visibility_gauge_defect.md` §5.1
and the memory card `project_pivot_visibility_gauge_defect` in the same commit.
Item numbers are STABLE — express a re-ranking in the BUILD ORDER block at the
top rather than renumbering, so cross-references from other docs keep resolving.
