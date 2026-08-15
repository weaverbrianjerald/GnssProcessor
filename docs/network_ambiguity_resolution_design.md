# Producer-side Network Ambiguity Resolution (integer-leveled satellite phase biases)

**Status (2026-08-15):** scope / implementation plan. **Phase 0 LANDED** — `include/filter/IlsCore.hpp` exists
and cites §5 for the network gate defaults. **Phases 1–5 are NOT built; the standing verdict is DO-NOT-BUILD**
(memory `project_network_ar_producer_scope`: "Network AR producer (M5) — DO NOT BUILD"). What stays live: the
theory (§2), the code-cited gate table (§5) and the risk register (§7, cited by
`tests/test_network_estimator.cpp:1497`). Drafted 2026-06-28 for B. Weaver. **Companion memories:**
`project_ar_validation_criterion`, `project_ppp_rtk_network`, `project_network_ssystem_dia_assessment` /
`project_ssystem_theory_reference` (the previously-cited `project_ssystem_conformance.md` does not exist — the
doc equivalent is `docs/SPEC_sbasis_realization_contract.md §F`).

> ⚠️ Line numbers here are *indicative only* — `ApplicationNetwork.cpp` is under active
> concurrent edit.  Every integration point is anchored on a **symbol**; re-grep it.

---

## 1. Goal and one-paragraph summary

Today the producer (`runNetwork` → `NetworkEstimator`) emits **float** satellite phase biases: the estimable
bias absorbs the *pivot receiver's* ambiguity lump as a real-valued offset (Odijk Eq. 4), so the bias carries
non-integer cycles. Producer-side AR would make the emitted bias **integer-leveled** — integer part referred
to a stable network datum, fractional part the true satellite phase UPD — so a held-out user sees a tighter,
integer-consistent bias surface: higher fix rate, faster TTFF, lower converged RMS.

**Low-invention feature — the two hardest prerequisites already exist:**

1. **Network ambiguities are NOT Schur-eliminated.** They live in each station's local block `A_r`,
   recoverable after the float solve via `NetworkNormalEquations::reconstructStation(station, x_s)`, with the
   cached LDLᵀ factor available for the conditional covariance. *Make-or-break enabler — confirmed present.*
2. **The MLAMBDA + FFR core is already decoupled from `StateManager`** — `AmbiguityResolver::{mlambda,
   ldlDecompose, reduction, search, computeBootstrapSuccessRate, selectFixableSubset_}` operate purely on `(â
   cycles, Q cycles)` and are reusable verbatim; only a thin pure entry point needs exposing.

Genuinely new: (a) reconstruct the DD float block with the *correct* conditional covariance, (b) form
between-station DDs and call the existing ILS core, (c) fold the fixed integers back as pins and re-solve.
Emission, discontinuity signalling and closed-loop validation are all reused.

---

## 2. Theory — what "integer-leveled" means in this S-basis

The network uses an **explicit minimum-constraint S-basis** — the one place in the codebase that does. Datum:

- **Shared:** one satellite-clock pin per constellation (`NetworkSbasis::buildSharedDatum`, hard pin at 0).
- **Local:** tight-prior pins (`w = 1e10`) on the **pivot receiver's** ambiguities, driving each to its
  *absolute float value* (`innovation = −current_ambiguity`). They eliminate in the Schur reduction, leaving
  `b̃ₛʲ = bₛʲ + λ·N_pivotʲ` — Odijk Eq. 4. Because `N_pivotʲ` is pinned to a **real** number, the product is
  float.

The integer-estimable quantities are **between-station, between-satellite double differences**:

```
∇ΔN = (N_r^j − N_r^p) − (N_1^j − N_1^p)        station r vs pivot station 1,
                                                sat j vs pivot sat p
```

They are integer because all four ambiguities ride the same code channel, so receiver and satellite phase
biases cancel. (Undifferenced ambiguities are *not* integer — they absorb both biases. This is why the float
product is correct-but-loose, not wrong.)

**The integer-leveling move is small:** change the pivot-receiver pins from *pin to float value* to *pin to
the nearest integer*, and add the fixed DDs as integer constraints. Then `N_pivotʲ` is an integer on a defined
datum, `b̃ₛʲ`'s integer part is referred to that datum and its fractional part is the satellite UPD — the
product's *meaning* to the consumer is unchanged; only its tightness and datum stability improve.

**Datum choice for the integer part:** reference station = existing `clock_datum_station`; reference satellite
per constellation = existing `Plan::clock_pivot`. Set the (pivot-station, pivot-sat) undifferenced ambiguity
per signal to an integer (round-of-float, or 0); all others follow from the fixed DDs relative to it.

### Constellation scope

- **GPS + Galileo only** (V1 network scope).
- **GLONASS stays float, always.** FDMA per-slot IFB does not cancel in between-station SD without per-slot
  IFB calibration. Reuse the existing exclusion — `MeasurementModel::non_integer_capable_dd_ambiguity_keys_`
  and `markAmbiguityUnresolvable()`; do not invent a new gate.

---

## 3. Where it hooks in (`runNetwork` epoch loop)

The float solve sequence today (symbol-anchored):

```
PASS A   ensureNetworkSharedStates           (allocate shared clock/phase-bias cols)
PASS B   splitStation → A_s / A_r            (+ pivot ambiguity pins, w=1e10)
         est.addStation(...)                 (scatter into arrowhead N_ss/N_sr/N_rr)
         NetworkSbasis::buildSharedDatum      (clock pins)
         assertNullSpaceRank                  (W5 observability gate)
         est.addSharedPrior                   (D2 carry-forward)
  ┌───── est.solve(plan.shared_datum)         (Dirichlet pins → x_s, cache shared_cov_)
  │  ▼  ◄────────── NEW: network AR step inserts HERE ──────────►
  └───── est.toSolutionEpoch(x_s, ...)        (x_s → NetworkSolutionEpoch)
         emitter.emitToStore(sol, store)      (→ SsrCorrectionStore, P-lines)
         stash prev_carry                      (D2)
```

**The AR step runs strictly between `solve()` and `toSolutionEpoch()`**: reconstruct ambiguities, fix DDs,
re-pin to integers, re-solve, hand the integer-consistent `x_s` on.

---

## 4. Implementation phases

### Phase 0 — Extract a pure ILS entry point  *(refactor, no behavior change)*

Expose the already-generic core so the user path and the network path call one implementation:

```cpp
// include/filter/IlsCore.hpp  (new; or static on AmbiguityResolver)
struct IlsResult {
    Eigen::VectorXd afix;      // fixed integers (cycles)
    std::vector<int> kept;     // indices kept (== all, unless partial AR)
    double ratio   = 0.0;      // s2/s1
    double p_succ  = 0.0;      // bootstrap success rate (FFR)
    bool   ok      = false;    // passed all gates
};

// Pure: no StateManager, no Kalman. Wraps mlambda + computeBootstrapSuccessRate
// + ratio/upper-bound + per-pair frac + optional partial-AR backward elimination.
IlsResult ilsFix(const Eigen::VectorXd& a_hat_cycles,
                 const Eigen::MatrixXd& Q_cycles,
                 const IlsGateConfig&   gates);
```

`IlsGateConfig` = `{ratio_threshold, ratio_upper_bound, min_success_rate, nl_max_per_pair_frac,
enable_partial_ar}`. Re-point `AmbiguityResolver::resolve` / `resolveDd` / `resolveWlConstrained` at `ilsFix`
internally (their StateManager wrappers stay). Independently unit-testable (CLAUDE.md carries the current case
count). **LANDED** — see the status line.

### Phase 1 — Reconstruct DD float block with the correct conditional covariance

After `solve()`, call `reconstructStation(i, x_s)` per non-pivot station for `x_r` (incl. float ambiguities).
**The covariance is the subtle part and the #1 correctness risk:** `shared_cov_` comes from the
*datum-constrained* reduced normals, so pinned columns carry an artificial `1/pin_scale` variance, not a true
posterior. The ambiguities must be conditioned on the pinned shared params as **KNOWN constants** — the naïve
`Cov(x_r) = N_rr⁻¹ + N_rr⁻¹ N_rs Cov(x_s) N_sr N_rr⁻¹` is wrong if it pulls in pinned-column variance. Correct
recipe, using only the **free** shared block:

```
Cov(x_r | x_s^free) = N_rr⁻¹ + N_rr⁻¹ N_rs · Cov_free(x_s) · N_sr N_rr⁻¹
```

`Cov_free` zeroes the rows/cols of datum-pinned columns (they are exact). Reuse the cached LDLᵀ factor of
`N_rr`; do not refactor `N_rr`. Difference to DD covariance the standard way (`Qb(i,j) = P(i,i) − P(i,ref) −
P(ref,i) + P(ref,ref)` across station/sat).

**Validation gate (free):** reconstruct the *pivot* station's pinned ambiguities; they must equal their pins
to ~machine precision. If not, the float solve is suspect — skip AR this epoch and emit float (fail-open).
Also owed: an epoch-boundary self-consistency check plus a unit test on the internal 3-station fixture.

### Phase 2 — Form DDs, run WL→NL cascade, fix

1. Pick pivot station (`clock_datum_station`) and per-constellation pivot sat (`Plan::clock_pivot`) — reuse,
   do not re-derive.
2. Form between-station DDs vs the pivot station; build the spanning-tree set (one independent DD per extra
   station per extra sat).
3. **Exclude** GLONASS and code-channel-mismatch DDs via `non_integer_capable_dd_ambiguity_keys_`.
4. **WL first (recommended):** MW DD widelanes from per-station MW arcs, fixed geometry-free. *Caveat:* the
   network has no per-station MW aggregation today (§6). For a first cut the **NL-direct LAMBDA** path is
   acceptable on the strong network geometry; add the cascade second.
5. Call `ilsFix(â_DD_cycles, Q_DD_cycles, gates)` with **strict** gates (§5). Use partial AR — fix the
   decisively-fixable subset, never force a full-set fix.

### Phase 3 — Re-pin to integers and re-solve

Reuse the existing pivot-pin machinery (the synthetic tight-prior rows appended to `A_r` in `splitStation`):
switch the pivot-station ambiguity pins from float target to integer target; append one tight-prior row per
fixed DD (same `w = 1e10`, target = the fixed integer); re-reduce + re-solve (cheap — the per-station `N_rr`
LDLᵀ factors are cached); `toSolutionEpoch(x_s_fixed, …)` then yields integer-leveled biases.

- **Hard guard:** if `datum_changed` (pivot sat handover this epoch), do NOT carry or apply previous-epoch
  integers — re-fix from scratch or emit float. **A handover invalidates the integer datum.**
- **Pivot exclusion:** never let AR fix a *pivot-receiver* ambiguity inconsistent with the datum integer —
  that breaks Odijk Eq. 4 and jumps the bias. **The pivot ambiguities define the datum; they are set, not
  searched.**

### Phase 4 — Product semantics and consumer frame

**Recommended (V1): no new serialization field.** Emit the (now integer-consistent) total bias in metres as
today — the consumer's own AR absorbs the integer part natively against a stable surface, avoiding a
`NetworkProductIO` format bump. Two changes: set `signal_integer = true` (and `wide_lane_integer` if WL fixed)
on fixed (sat, signal) entries; and **bump `discontinuity_counter` on any integer-datum change** (pivot
re-reference, or a fixed integer that flips) even when `bias_m` is smooth, via
`NetworkProductEmitter::markDatumChange()`, so the consumer's `StateManager::noteSsrDiscontinuity` →
`resetAmbiguity` fires cleanly. *Widen the counter to ≥ 32-bit to avoid the 2-bit IGS-SSR aliasing wrap.*

**Consumer MUST run `consumer_undifferenced = true`.** Integer-leveled bias + BSSD is a frame conflict (BSSD
keeps the ambiguity definition receiver-relative); the undifferenced user model applies the bias per-band and
dissolves the `.BIA` ↔ network frame conflict. Add a **config validation gate**: consumer estimating sat phase
biases against a network-AR product and NOT undifferenced ⇒ hard error.

**Sign convention:** `kSsrPhaseBiasSign = +1.0` (bias added to clean model), identical producer and consumer.
Diagnostic: a fractional-residual histogram — post-AR float fractions clustering near 0.5 instead of 0 means
the sign is flipped.

### Phase 5 — Validation (the closed-loop harness is the KPI)

The held-out closed-loop consumer (`cl_*` counters, `closed_loop_trace.csv`) is the built-in A/B rig: **A** =
producer float-only, **B** = producer integer-leveled, both graded on consumer AR fix rate / TTFF / converged
3D RMS, with `consume_network_clock = false` to isolate bias quality (precise clock + network phase bias
only). Expected: B better on all three. **Add `time_to_first_fix` and per-epoch `num_candidates` to the trace
if absent**, and run BOTH fixtures — `IGS_Pasadena` and the internal 3-station one.

**Metric-inflation caveat (log it):** once producer AR runs, the consumer fix rate measures the *fixed*
product, not the float one. Keep the float-producer A run as the reference and say so in the summary line.

---

## 5. Validation criterion (ties to the FFR decision)

Per `project_ar_validation_criterion`: gate with the **fixed-failure-rate (bootstrap success rate)**, not
ADOP. Stricter on the network than on the user, because a wrong network fix poisons every downstream user:

| gate | user default | **network default (proposed)** |
|------|--------------|-------------------------------|
| `min_success_rate` (FFR) | 0.999 | **0.9999** (1e-4 target) |
| `ratio_threshold` | 3.0 | 3.0 (backstop) |
| `ratio_upper_bound` | 1e4 | 1e4 |
| `nl_max_per_pair_frac` | 0.15 cyc | **0.10 cyc** |
| `enable_partial_ar` | false | **true** (fix the fixable subset) |

ADOP stays a **diagnostic / feasibility** scalar (per-sat bias estimability, geometry adequacy, logged QC) —
not a gate. Do not build the Verhagen–Teunissen FF-RT critical-value machinery; the bootstrap gate already
controls failure rate.

New config section (mirrors `estimation.ambiguity_resolution`), wired via a `loadNetworkSection_` helper in
`GnssConfigLoad.cpp` (the `jval` pattern); `network.ambiguity_resolution.enable` defaults false so the float
product is bit-identical until turned on:

```jsonc
"network": {
  "ambiguity_resolution": {
    "enable": false,            // master switch (default OFF — float product unchanged)
    "min_success_rate": 0.9999,
    "ratio_threshold": 3.0,
    "ratio_upper_bound": 1e4,
    "nl_max_per_pair_frac": 0.10,
    "enable_partial_ar": true,
    "wl_cascade": false,        // Phase 2 step 4 — off until per-station MW aggregation lands
    "min_epochs": 10            // let the network float converge before fixing
  }
}
```

---

## 6. Gaps / new code required (from the audit)

| Item | Status | Notes |
|------|--------|-------|
| Reconstruct ambiguities + covariance | **partial** | `reconstructStation` exists; conditional-covariance-on-pinned-as-known is new (Phase 1). |
| Pure `ilsFix(â, Q)` entry point | **partial** | core is generic; expose it (Phase 0). |
| `selectFixableSubset_` reuse | **partial** | exists, currently private — expose. |
| Network DD formation + covariance | **missing** | new orchestration (Phase 2). |
| Integer re-pin + re-solve | **reusable** | extend the pin builder to integer targets (Phase 3). |
| Per-station MW aggregation for WL DD | **missing** | WL cascade only; defer (Phase 2 step 4). |
| Datum-handover guard for integers | **missing** | reuse `datum_changed` (Phase 3). |
| Product flags + discontinuity on datum change | **reusable** | `markDatumChange`, widen counter (Phase 4). |
| `consumer_undifferenced` enforcement | **reusable** | flag exists; add validation gate (Phase 4). |
| Closed-loop A/B + TTFF KPI | **reusable** | harness complete; add trace columns (Phase 5). |

---

## 7. Risk register (carry into review)

1. **Conditional covariance (CRITICAL).** Pinned shared columns have artificial `1/pin_scale` variance;
   condition DD ambiguities on them as *known*. Getting this wrong feeds LAMBDA a wrong `Q` →
   over/under-confident fixes that pass FFR spuriously. *Verify against a brute-force full-inverse on the
   3-station internal fixture.*
2. **Pivot ambiguity integrity.** Pivot pins define the datum; never search them. A non-zero/non-datum pivot
   fix jumps the bias (Odijk break).
3. **Datum handover.** Pivot-sat change invalidates carried integers; the network currently re-references
   without an S-transform. Guard hard; consider sticky-pivot hysteresis (cf. `DdPivotManager`) before
   multi-epoch production.
4. **Consumer frame.** Integer-leveled + BSSD = silent frame mismatch. Enforce `consumer_undifferenced`.
5. **Sign convention.** `+1.0` both sides; histogram fractions to catch a flip.
6. **GLONASS.** Float only — reuse the `non_integer_capable` gate; do not attempt.
7. **Numerical conditioning.** `pin_scale = max(diag N)` with heterogeneous satellite weighting can distort
   LDL pivoting; watch the re-solve residuals.
8. **Metric inflation.** Producer AR makes the closed-loop fix rate measure the fixed product — keep the
   float A run and say so.
9. **Validation honesty.** A single held-out station validates the bias datum but not inter-station DD
   geometry; add 2–3 held-out stations later.

---

## 8. Suggested commit sequence

One commit per phase, in phase order (0 → 5), then per-station MW aggregation + the WL→NL cascade, then the
closed-loop A/B trace columns run against both the `IGS_Pasadena` and the internal 3-station fixture. Each
step builds and is independently testable; **only Phase 2 onward changes output, and only when
`network.ambiguity_resolution.enable` is on.** Step 1 (Phase 0, the `IlsCore` extraction) is independently
mergeable with zero behaviour change and **has landed**; the rest is unspent under the DO-NOT-BUILD verdict.
