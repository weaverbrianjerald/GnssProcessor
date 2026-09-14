# queue 1(b) — the handover S-transform spec — the transform, its rank-1 specialization to a pin swap, INC-0..3 and the gates

**STATUS**

- **id:** queue 1(b)
- **topic:** network-vehicle
- **status:** closed
- **summary:** the handover S-transform spec — the transform, its rank-1 specialization to a pin swap, INC-0..3 and the gates
- **dates in the text:** 2026-08-04 → 2026-08-15
- **knobs / posture:** `network.handover_stransform`, `network.handover_stransform_scope`, `network.handover_stransform_pb_shadow`, `network.handover_var_bump`
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** —
- **source:** `docs/archive/SPEC_pivot_visibility_gauge_defect.md` — 8, 8.1, 8.2, 8.3, 8.4, 8.5, 8.6, 8.7, 8.8, 8.9

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_pivot_visibility_gauge_defect.md -->
## 8. The handover S-transform — QUEUE item 1(b), SPEC'd 2026-08-04, **UNBUILT / OPEN**

*Absorbed 2026-08-15 from `docs/SPEC_handover_stransform.md` (now `docs/attic/`). **Status:
OPEN.** Verified against the tree at absorption: no `handover_stransform`,
`handover_stransform_scope`, `handover_stransform_pb_shadow` or `handover_var_bump` symbol exists
anywhere in `src/` or `include/`. It is live as item **1(b)** of
`docs/QUEUE_principled_replacements.md` and the merge does not retire it. (`docs/README.md` lists
the absorbed card's status as CLOSED — that is wrong; fix at the next index regeneration.)
Memory context of the absorbed card, preserved: `project_network_ssystem_dia_assessment`
(**catalog item 3 + ranked plan 3 — this card**), `project_laser_theory_mapping`,
`project_network_falsified_approaches`, `project_fast_clock_card` (emitted clock ~98 % churn),
`project_agu26_abstract_campaign` (churn, not accuracy, is the blocker). Section mapping for old
citations: the absorbed card's §7 gate list is §8.7 here — its **§7.3** is §8.7 gate 3 (the census
`|t|` cross-check) and its **§7.5** is §8.7 gate 5 / the closing non-goal ("not a consumer fix and
must not be graded as one").*

> ### ⚠ §8 and §5.07/§5.08 are DIFFERENT GAUGES — do not blur them
> **§8 (item 1(b)) TRANSFORMS the CLOCK-PIVOT carry**: the carried priors that are today
> *skipped* for one epoch at five `handover_affects` sites when the per-constellation clock pivot
> changes. **§5.07/§5.08 (item 1(d), `phase_anchor_carry_reref`, LANDED) MARGINALIZES the
> PB-ANCHOR carry**: the phase-bias datum re-realized on the design matrix while the carry stayed
> in the retired frame. Same template — *the carry at a datum edge* — different gauge, different
> detection site, different knob. **1(d)'s validation does NOT close 1(b).**

### 8.1 The transform, and its rank-1 specialization to a pin swap

**Origin.** NGS LASER Guide v2.7 slides 5-86..5-88 (PDF pp. 398–400,
`\\depot\cce_projects\gcap\Software\LASER\`): any minimally-constrained solution transforms to any
other **exactly** by `S := I_m − Eᵀ(KEᵀ)⁻¹K`, applied to mean **AND** dispersion, with only an
l×l inverse (l = the datum defect). Psychas/Khodabandeh (2024) supplies the PPP-RTK
datum-continuity framing.

The clock datum per constellation c is a Dirichlet pin-to-**ZERO** on the pivot satellite
(`pin_y = −piv_x`; note the design-doc drift warning in the DIA catalog — the tree pins to zero,
not to the float value). A handover p→q within c swaps the pin row, and the LASER transform
reduces to a rank-1 update along the gauge null direction:

```
c₀   = x_q_pre / v_c[q]        the new pivot's pre-transform carried level
x   ←  x − v_c · c₀            every carried mean with a component in v_c
var ←  var + v_c[i]² · var_q   diagonal-compatible dispersion update
```

Two load-bearing rules:

1. **`v_c` MUST be the STRUCTURAL null direction `NetworkSbasis` owns** — never hand-written and
   never an eigenvector (SBC lesson: same-scale eigenvectors mix; structural membership is exact
   and carries per-sat identity). After the update the new pivot's carried level is exactly 0 = its
   pinned value, and every other carried mean is what a fresh solve under the new pin would have
   produced.
2. **Dropping the cross term is CONSERVATIVE.** The exact per-column variance of `x_i − v_i·c₀` is
   `var_i + v_i²·var_q − 2v_i·cov(i,q)`. For the dominant common-mode case clocks are positively
   correlated ⇒ `cov(i,q) > 0` ⇒ omitting `−2v_i·cov` **over**-states variance — the same
   "conservative-if-omitted" property that was load-bearing in the ensemble fix. Full `S·D·Sᵀ` on
   the carry blob is deferred (§8.8).

### 8.2 Why — the clock-side handover free-fall is the discontinuity generator

Today a clock-pivot handover is answered by **skipping the fold of every affected carried prior for
one epoch**. `handover_affects` is mirrored at **five** sites:
`src/core/app/NetworkRunContext.cpp:1375`, `NetworkRunContext.cpp:1582`,
`src/core/filter/NetworkClockQc.cpp:92`, `src/core/filter/NetworkSolveGlue.cpp:380`,
`src/core/filter/NetworkStashStatus.cpp:85`. `prev_carry` is re-stashed every epoch
(`NetworkRunContext.cpp:2051`), so the skip is not a persistent loss — it is **one epoch of free
fall**: stiff states (PB at q ≈ 1e-12, CB, clock levels) solve data-only exactly once, re-level,
and re-stash in the new datum. **That one-epoch re-level IS the product discontinuity.**

Motivating quantification: **91 % of emitted PB discontinuities are ref-path re-referencing**
(CLAUDE.md AGU-26 amendment 2) and the emitted clock is **~98 % churn** (FC-1). The information in
the carried prior was **valid** — merely expressed in the old datum, and LASER 5-86 says the
re-expression is exact and cheap. Same discipline SBC's continuity RHS (`vᵀx_prev`,
`NetworkSbasis::completeSbasisDimension`) already applies to deficient directions; there: hold a
direction at its carried level, here: re-express carried levels when the realized datum moves.

**What this card is NOT (FALSIFIED — do not re-litigate):** a consumer fix via datum choice at
emit. Ensemble PB datum, clock s2z and broadcast-anchored were **all A/B'd dead**
(`project_network_falsified_approaches`) — common-mode is consumer-absorbable. §8 is about the
PRODUCER-INTERNAL carry surviving the edge; the wire still steps by the datum delta **by design**
and the existing disc channels still describe it.

### 8.3 Edge classes

| class | detection (exists today) | v1 action |
|---|---|---|
| **A `adjacent`** | `datum_changed` = `pivotHandoverSignalled` (`NetworkSolveGlue.cpp:373`) + `changed_consts`; arm derived at `NetworkSolPrep.cpp:591-614` | **transform** |
| **B `vanish_return`** | same site — `prev_pivot` lacks the key; old pivot from the **persistent** `last_pivot` map (V1/V2 landed, serialized at `NetworkStateSnapshot.cpp:131`/`:303`) | **transform from the last-known realization**; win bounded by surviving-prior count (counter: priors past max-gap aged out ⇒ a counted no-op) |
| **C PB-anchor handover** | prioritized-anchor election change | **SHADOW ONLY** (INC-2) — compute and event the would-be coefficient, apply nothing. That gauge direction spans **PB and ambiguity columns**: bigger surface, measure first |

### 8.4 Design — INC-0..3

**INC-0: the kernel + seam.** New seam `src/core/filter/NetworkHandoverStransform.cpp` (+ manual
`CMakeLists.txt` entry; header `include/filter/NetworkHandoverStransform.hpp`), free function over
`(NetworkRunContext& ctx, const GnssConfig&, …)` per the seam contract, called from the
`NetworkSolveGlue` site where `datum_changed` is derived. It mutates the carried priors in
`ctx.carry` (shared `prev_carry` values; any pl/local key the structural direction touches —
membership is data-driven from `v_c`, not assumed) **BEFORE** the epoch's folds.

**INC-1: release the skips — the load-bearing change.** On an APPLIED transform the five
`handover_affects` skip predicates must **NOT** fire for the transformed keys: the prior is now
valid in the new datum and must fold. Gate each lambda on the applied set; count
`sts_rows_retained` (rows folded that the baseline would have skipped). **Invariant #4: this
counter is the card's ON-path telemetry — a run with transforms applied and
`sts_rows_retained = 0` means the release is mis-wired no matter how clean the log looks.**

**INC-2: both edges are events (invariant #7).** Extend the existing PIVOT event row
(`NetworkSolPrep.cpp:596-613`) with `applied|shadow|skipped` and the coefficient magnitude `|c₀|`;
per-sat identity for every transformed key above a trace floor. Counters
`sts_applied_{adjacent,vanish}`, `sts_shadow_pb`, `sts_rows_retained`, `sts_noop_vanish`, reported
by `NetworkTeardown`. **Cross-epoch reference holders must be audited at build time**: any monitor
comparing values across the edge (the `NetworkClockQc` jump test — it already iterates `prev_pivot`
at `:489`; CB disc jump baselines; relevel baselines) must either receive the same transform or
classify the edge as **FRAME**. The prior-residual census's FRAME ladder is the audit instrument —
after this card a transformed handover edge should show `|t|` **collapse toward CLEAN** on the
affected families, the cleanest single number the card owes (gate 3).

**INC-3: exactness self-check** (cheap, always on when armed). After the transform the new pivot's
carried level must be **exactly 0** and re-deriving the coefficient must return **c₀ = 0**
(idempotence). Assert-with-event, never throw.

### 8.5 Config — four knobs, all default-OFF

| knob | type | default | class / note |
|---|---|---|---|
| `network.handover_stransform` | bool | `false` | master arm; **FINGERPRINTED** — solve-affecting (it changes which priors fold), fresh convergence only, **never flip mid-chain** (the `cb_excursion_reinit` conditional-fingerprint precedent) |
| `network.handover_stransform_scope` | string | `"clock"` | v1 accepts only `"clock"` (classes A+B); loader **ERROR** on anything else |
| `network.handover_stransform_pb_shadow` | bool | `false` | class C measure-only shadow |
| `network.handover_var_bump` | string | `"auto"` | `"auto"` = `v_i²·var_q` per column; or a fixed m² floor added to transformed columns |

Loader site: `loadNetworkCarrySection_`, with the standard requires/mutex validation. **Never legal
on a warm resume whose seed lacked it.**

### 8.6 Cost

Rank-1 over the carried key set, once per handover **EDGE** — not per epoch. Handover rate on the
golden lineage is ~10⁻²/epoch·constellation. Budget: unmeasurable (**< 0.1 ms/edge**); **above
1 ms/edge means the implementation is wrong**.

### 8.7 Field validation — known-answer gates

Controls: an FT63 leg with known handovers + the golden 3 h / 305 lineage. Release only, smoke
first, **seamless lineages** (a warm-resume seam erases the very events under study).

1. **OFF byte-identity** (structural + same-build A/B).
2. **Live wiring**: `sts_applied > 0` **AND** `sts_rows_retained > 0` (invariant #4).
3. **The census cross-check** *(the absorbed card's §7.3)*: on transformed edges the FRAME-label
   `|t|` ladder for `cb`/`pb` collapses toward the CLEAN ladder.
4. **Product churn**: PB discontinuity rate and per-sat clock re-level count across handover edges,
   ON vs OFF. The 91 % ref-path-slip population is the target; expect a measurable cut in the
   **handover-attributed slice specifically** (attribute via the PIVOT events — do not claim the
   slip-attributed slice).
5. **Grade datum-invariant quantities only** (invariant #5): total_IF, DD steps, a00 band. The raw
   wire steps by the datum delta BY DESIGN — a raw-wire comparison is a confound, not a grade.
6. **⚠ THE FALSIFIER WATCH.** The ensemble archive's **epoch-2 signature** — de-medianed states
   matching at the edge epoch then diverging geometrically (~7×/epoch) — means the covariance path
   is wrong (the bump under-states a cross term somewhere). **Halt and report; do NOT tune past it.**

### 8.8 Deferral triage (invariant #3) and non-goals

| deferred | field-blocking? | evidence |
|---|---|---|
| Full `S·D·Sᵀ` on the carry blob | **INERT for v1** — the diagonal bump is conservative for the common-mode-dominated case (§8.1) and gate 6 watches the failure signature | conservatism argument + gate 6; the full form is blocked on a full-cov carry representation (`project_network_fullcov_carry`) |
| Class C apply (PB anchor) | **INERT** — shadow measures the coefficient population first; applying blind to a direction spanning ambiguity columns is the invariant-#1 shape | shadow counters are the successor card's design input |
| Consumer-side transform | **INERT** | producer validation first |
| Event-driven per-const disc + vanish-return persistent map | already landed (V1/V2) — this card CONSUMES them | tree-verified at `NetworkSolPrep.cpp:591-614` |

**What §8 explicitly does NOT do:** it does not change the emitted datum, the wire, or the
disc-channel contract; it does not touch pivot election, SBC, PA or the S-basis gate; it does not
transform covariance beyond the diagonal bump in v1; it does not apply to PB-anchor handovers in
v1 (shadow only); and **it is not a consumer fix and must not be graded as one** *(the absorbed
card's §7.5 = gate 5)*.

**Standing-invariant pre-flight.** (1) The transform's whole purpose is invariant #1 — the stiff
carried prior gets a correct event response (re-expression) instead of a silent free-fall re-level.
(2) Release-only. (3) Deferrals triaged above. (4) `sts_*` counters distinguish "armed, no
handovers" from "never ran". (5) Datum-invariant grading only. (6) Default-OFF, byte-identical,
same-build A/B. (7) Both edges evented with identities (INC-2); the census FRAME ladder measures
what the re-coupling edge moved.

### 8.9 Relationship to 1(d) — what 1(b) still owes

1(d) **marginalizes** the PB-anchor carry and is measured sufficient there (§5.08.3); §8
**transforms** the clock-pivot carry instead of deleting it, on an edge 1(d) never touches.
**"Transform > delete" remains UNMEASURED** (§5.07.6 / §5.08.5) — build the transform when a
bigger fixture shows the one-epoch refloat cost, or for §8's own clock-pivot edge.

