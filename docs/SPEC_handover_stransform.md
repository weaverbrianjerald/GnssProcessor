# SPEC — carry S-transform at datum handover (retain, don't discard)

**Status**: SPEC'd 2026-08-04. Build in a fresh session.
**Origin**: NGS LASER Guide v2.7 slides 5-86..5-88 (PDF pp. 398–400,
`\\depot\cce_projects\gcap\Software\LASER\`): any minimally-constrained solution
transforms to any other **exactly** by `S := I_m − Eᵀ(KEᵀ)⁻¹K`, applied to mean AND
dispersion, with only an l×l inverse (l = datum defect). Psychas/Khodabandeh (2024) for
the PPP-RTK datum-continuity framing.
**Memory context**: `project_network_ssystem_dia_assessment` (catalog item 3 + ranked
plan 3 — this card), `project_laser_theory_mapping`,
`project_network_falsified_approaches` (read the ensemble-datum entry BEFORE the
covariance work — its "epoch-2 geometric divergence" signature is this card's falsifier),
`project_fast_clock_card` (emitted clock ~98% churn), `project_agu26_abstract_campaign`
(churn, not accuracy, is the blocker).
**Scope discipline**: default-OFF, OFF byte-identical, **FINGERPRINTED** (solve-affecting
— it changes which priors fold; fresh convergence only, never flip mid-chain).

---

## 1. Why — the handover free-fall is the discontinuity generator

Today a clock-pivot handover is answered by **skipping the fold** of every affected
carried prior for one epoch: `handover_affects` is mirrored at five sites
(`src/core/app/NetworkRunContext.cpp:1375`/`:1582`, `src/core/filter/NetworkClockQc.cpp:92`,
`src/core/filter/NetworkSolveGlue.cpp:380`, `src/core/filter/NetworkStashStatus.cpp:85`).
`prev_carry` is re-stashed every epoch (`NetworkRunContext.cpp:2051`), so the skip is not
a persistent loss — it is one epoch of **free fall**: stiff states (PB at q≈1e-12, CB,
clock levels) solve data-only exactly once, re-level, and re-stash in the new datum.
That one-epoch re-level IS the product discontinuity: 91 % of emitted PB discontinuities
are ref-path re-referencing (AGU-26 amendment 2), and the emitted clock is ~98 % churn
(FC-1). The information in the carried prior was **valid** — it was merely expressed in
the old datum. LASER 5-86 says the re-expression is exact and cheap.

The S-transform closes the hole: at the handover edge, re-express the carried priors in
the new datum, then fold them normally. No free fall, no re-level, the stiff carry
survives the edge. This is the same discipline SBC's continuity RHS (`vᵀx_prev`,
`NetworkSbasis::completeSbasisDimension`) already applies to deficient directions —
there: hold a direction at its carried level; here: re-express carried levels when the
realized datum moves.

**What this card is NOT** (falsified — do not re-litigate): a consumer fix via datum
choice at emit. Ensemble PB datum, clock s2z, broadcast-anchored were all A/B'd dead
(`project_network_falsified_approaches`) — common-mode is consumer-absorbable. This card
is about the PRODUCER-INTERNAL carry surviving the edge; the wire still steps by the
datum delta and the existing disc channels still describe it.

## 2. The transform, specialized to a pin swap

The clock datum per constellation c is a Dirichlet pin-to-ZERO on the pivot satellite
(`pin_y = −piv_x`; note the design-doc drift warning in the DIA catalog — the tree pins
to zero, not to the float value). A handover p→q within c swaps the pin row. The LASER
5-86 transform for that swap reduces to a rank-1 update along the gauge null direction:

```
c₀   = x_q_pre / v_c[q]                       the new pivot's pre-transform carried level
x   ←  x − v_c · c₀                           every carried mean with a component in v_c
var ←  var + v_c[i]² · var_q                  diagonal-compatible dispersion update
```

where `v_c` is the **structural** null direction of the per-constellation clock gauge —
obtained from the same structural gauge derivation `NetworkSbasis` owns, NOT hand-written
and NOT an eigenvector (SBC lesson: same-scale eigenvectors mix; the structural
membership is exact and carries per-sat identity). After the update the new pivot's
carried level is exactly 0 = its pinned value, and every other carried mean is what a
fresh solve under the new pin would have produced (unit test 1 proves this).

The dispersion line is the honest diagonal approximation of `S·D·Sᵀ`: exact per-column
variance of `x_i − v_i·c₀` is `var_i + v_i²·var_q − 2v_i·cov(i,q)`. Dropping the cross
term is **conservative** for the dominant common-mode case (clocks positively correlated
⇒ cov(i,q) > 0 ⇒ omitting −2cov over-states variance) — the same
"conservative-if-omitted" property that was load-bearing in the ensemble fix. The full
`S·D·Sᵀ` on the carry blob is explicitly deferred (§8): the diagonal blob cannot
represent it, and the ensemble archive documents what mishandling that path does.

## 3. Edge classes

| class | detection (exists today) | v1 action |
|---|---|---|
| A `adjacent` | `datum_changed` = `pivotHandoverSignalled` (`NetworkSolveGlue.cpp:373`) + `changed_consts`; arm derived at `NetworkSolPrep.cpp:591-614` | transform |
| B `vanish_return` | same site — `prev_pivot` lacks the key, old pivot from the **persistent** `last_pivot` map (V1/V2 landed; serialized at `NetworkStateSnapshot.cpp:131/303`) | transform from the last-known realization; win bounded by surviving-prior count (counter — priors past max-gap aged out and the transform is a counted no-op) |
| C PB-anchor handover | prioritized-anchor election change | **shadow only** (INC-2): compute and event the would-be coefficient, apply nothing. The PB gauge direction spans PB and ambiguity columns — bigger surface, measured first. |

## 4. Design

### INC-0: the kernel + seam

New seam `src/core/filter/NetworkHandoverStransform.cpp` (+ manual `CMakeLists.txt`
entry; header `include/filter/NetworkHandoverStransform.hpp`), free function over
`(NetworkRunContext& ctx, const GnssConfig&, …)` per the seam contract, called from the
`NetworkSolveGlue` site where `datum_changed` is derived. It mutates the carried priors
in `ctx.carry` (shared `prev_carry` values; any pl/local key the structural direction
touches — membership is data-driven from `v_c`, not assumed) BEFORE the epoch's folds.

### INC-1: release the skips — the load-bearing change

On an APPLIED transform the five `handover_affects` skip predicates must NOT fire for
the transformed keys: the prior is now valid in the new datum and must fold. Gate each
lambda on the applied set; count `sts_rows_retained` (rows folded that the baseline
would have skipped). Invariant #4: this counter is the card's ON-path telemetry — a run
with transforms applied and `sts_rows_retained = 0` means the release is mis-wired no
matter how clean the log looks.

### INC-2: both edges are events (invariant #7)

Extend the existing PIVOT event row (`NetworkSolPrep.cpp:596-613`) with
`applied|shadow|skipped` and the coefficient magnitude `|c₀|`; per-sat identity for
every transformed key above a trace floor. Counters: `sts_applied_{adjacent,vanish}`,
`sts_shadow_pb`, `sts_rows_retained`, `sts_noop_vanish` (no surviving priors), reported
by `NetworkTeardown`. **Cross-epoch reference holders must be audited at build time**:
any monitor comparing values across the edge (`NetworkClockQc` jump test — it already
iterates `prev_pivot` at `:489`; CB disc jump baselines; relevel baselines) must either
receive the same transform or classify the edge as FRAME. The prior-residual census's
FRAME ladder is the audit instrument — after this card, a transformed handover edge
should show `|t|` collapse toward CLEAN on the affected families (that is gate §7.3).

### INC-3: exactness self-check (cheap, always on when armed)

After the transform, the new pivot's carried level must be exactly 0 and re-running the
coefficient derivation must return c₀ = 0 (idempotence). Assert-with-event, never throw.

## 5. Cost

Rank-1 over the carried key set, once per handover edge (not per epoch). Handover rate
on the golden lineage is ~10⁻²/epoch·constellation. Budget: unmeasurable (<0.1 ms/edge);
if it measures above 1 ms/edge something is wrong with the implementation.

## 6. Config

| knob | type | default | note |
|---|---|---|---|
| `network.handover_stransform` | bool | `false` | master arm; **FINGERPRINTED** |
| `network.handover_stransform_scope` | string | `"clock"` | v1 accepts only `"clock"` (classes A+B); loader ERROR on anything else |
| `network.handover_stransform_pb_shadow` | bool | `false` | class C measure-only shadow |
| `network.handover_var_bump` | string | `"auto"` | `"auto"` = v_i²·var_q per column; or a fixed m² floor added to transformed columns |

Loader: `loadNetworkCarrySection_`, with the standard requires/mutex validation. Never
legal on a warm resume whose seed lacked it (conditional-fingerprint discipline, the
`cb_excursion_reinit` precedent).

## 7. Field validation — known-answer gates

Controls: FT63 leg with known handovers + the golden 3 h/305 lineage. Release only,
smoke first, seamless lineages (a warm-resume seam erases the very events under study).

1. **OFF byte-identity** (structural + same-build A/B).
2. **Live wiring**: `sts_applied > 0` AND `sts_rows_retained > 0` (invariant #4).
3. **The census cross-check**: on transformed edges the FRAME-label `|t|` ladder for
   `cb`/`pb` collapses toward the CLEAN ladder (the prior no longer disagrees with the
   post-edge solve). This is the cleanest single number the card owes.
4. **Product churn**: PB discontinuity rate and per-sat clock re-level count across
   handover edges, ON vs OFF. The 91 % ref-path-slip population is the target; expect a
   measurable cut in the handover-attributed slice specifically (attribute via the PIVOT
   events — do not claim the slip-attributed slice).
5. **Grade datum-invariant quantities only** (invariant #5): total_IF, DD steps, a00
   band. The raw wire steps by the datum delta BY DESIGN — a raw-wire comparison is a
   confound, not a grade.
6. **The falsifier watch**: the ensemble archive's epoch-2 signature — de-medianed
   states matching at the edge epoch then diverging geometrically (~7×/epoch) — means
   the covariance path is wrong (the bump under-states a cross term somewhere). Halt and
   report; do not tune past it.

## 8. Deferral triage (invariant #3)

| deferred | field-blocking? | evidence |
|---|---|---|
| Full `S·D·Sᵀ` on the carry blob | **INERT for v1** — the diagonal bump is conservative for the common-mode-dominated case (§2) and gate 6 watches the failure signature | conservatism argument + gate 6; full form blocked on a full-cov carry representation (`project_network_fullcov_carry`) |
| Class C apply (PB anchor) | **INERT** — shadow measures the coefficient population first; applying blind to a direction that spans ambiguity columns is the invariant-#1 shape | shadow counters are the successor card's design input |
| Consumer-side transform | **INERT** | producer validation first |
| Event-driven per-const disc + vanish-return persistent map | already landed (V1/V2) — this card CONSUMES them | tree-verified at `NetworkSolPrep.cpp:591-614` |

## 9. Standing-invariant pre-flight

1. The transform's whole purpose is invariant #1: the stiff carried prior gets a correct
   event response (re-expression) instead of a silent free-fall re-level.
2. Release-only field runs. 3. Deferrals triaged above with evidence.
4. `sts_*` counters + run-end line distinguish "armed, no handovers" from "never ran".
5. Gates grade total_IF / DD / census ladders only. 6. Default-OFF, byte-identical,
   same-build A/B. 7. Both edges evented with identities (INC-2); the census FRAME
   ladder measures what the re-coupling edge moved, before and after.

## 10. What this card explicitly does NOT do

- It does not change the emitted datum, the wire, or the disc-channel contract.
- It does not touch pivot election, SBC, PA, or the S-basis gate.
- It does not transform covariance beyond the diagonal bump in v1.
- It does not apply to PB-anchor handovers in v1 (shadow only).
- It is not a consumer fix and must not be graded as one (§7.5).
