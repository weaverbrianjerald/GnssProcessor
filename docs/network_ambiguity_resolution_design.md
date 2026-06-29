# Producer-side Network Ambiguity Resolution (integer-leveled satellite phase biases)

**Status:** scope / implementation plan — NOT yet implemented.
**Author:** drafted 2026-06-28 for B. Weaver, to execute on return.
**Companion memory:** `project_ar_validation_criterion.md`, `project_ppp_rtk_network.md`,
`project_ssystem_conformance.md`.

> ⚠️ Line numbers in this doc are *indicative only* — `ApplicationNetwork.cpp`
> is under active concurrent edit. Every integration point is anchored on a
> **symbol** (function / struct / method). Re-grep the symbol before editing.

---

## 1. Goal and one-paragraph summary

Today the PPP-RTK network producer (`runNetwork` → `NetworkEstimator`) emits
**float** satellite phase biases: the estimable bias absorbs the *pivot
receiver's* ambiguity lump as a real-valued offset (Odijk Eq. 4), so the bias
carries non-integer cycles. This plan adds **producer-side integer ambiguity
resolution** so the emitted bias is **integer-leveled** — its integer part
referred to a stable network datum, its fractional part the true satellite
phase UPD. A held-out user then sees a tighter, integer-consistent bias surface
→ higher fix rate, faster TTFF, lower converged RMS.

The key finding from the internals audit: **this is a low-invention feature.**
The two hardest prerequisites already exist:

1. **Network ambiguities are NOT Schur-eliminated.** They live in each station's
   local block `A_r` and are recoverable after the float solve via
   `NetworkNormalEquations::reconstructStation(station, x_s)`, with the cached
   LDLᵀ factor available for the conditional covariance. *This is the
   make-or-break enabler — confirmed present.*
2. **The MLAMBDA + FFR core is already decoupled from `StateManager`.**
   `AmbiguityResolver::{mlambda, ldlDecompose, reduction, search,
   computeBootstrapSuccessRate, selectFixableSubset_}` operate purely on
   `(â cycles, Q cycles)`. They are reusable verbatim; only a thin pure entry
   point needs exposing.

The genuinely new work is: (a) reconstruct the DD float block with the *correct*
conditional covariance, (b) form between-station DDs and call the existing ILS
core, (c) fold the fixed integers back as pins and re-solve. Everything
downstream (emission, discontinuity signalling, closed-loop validation) is
reused.

---

## 2. Theory — what "integer-leveled" means in this S-basis

The network already uses an **explicit minimum-constraint S-basis** (the one
place in the codebase that does). The datum is:

- **Shared:** one satellite-clock pin per constellation (`NetworkSbasis::buildSharedDatum`, hard pin at 0).
- **Local:** tight-prior pins (`w = 1e10`) on the **pivot receiver's** ambiguities,
  driving each to its *absolute float value* (`innovation = −current_ambiguity`).
  These eliminate in the Schur reduction, leaving the emitted phase bias
  `b̃ₛʲ = bₛʲ + λ·N_pivotʲ` — Odijk Eq. 4.

Because `N_pivotʲ` is pinned to a **real** number, the product is float.

The integer-estimable quantities are **between-station, between-satellite
double differences**:

```
∇ΔN = (N_r^j − N_r^p) − (N_1^j − N_1^p)        station r vs pivot station 1,
                                                sat j vs pivot sat p
```

These are integer because all four ambiguities ride the same code channel, so
receiver and satellite phase biases cancel. (Undifferenced ambiguities are
*not* integer — they absorb both biases. This is why the float product is
correct-but-loose, not wrong.)

**The integer-leveling move is small:** change the pivot-receiver pins from
*"pin to float value"* to *"pin to the nearest integer"*, and add the fixed DDs
as additional integer constraints. Then `N_pivotʲ` is an integer on a defined
datum, so `b̃ₛʲ`'s integer part is referred to that datum and its fractional
part is the satellite UPD. The product becomes integer-leveled without changing
its *meaning* to the consumer — only its tightness and its datum stability.

**Datum choice for the integer part:** reference station = existing
`clock_datum_station` (pivot station); reference satellite per constellation =
existing `Plan::clock_pivot`. Set the (pivot-station, pivot-sat) undifferenced
ambiguity per signal to an integer (round-of-float, or 0). All other
ambiguities follow from the fixed DDs relative to that reference.

### Constellation scope
- **GPS + Galileo only** (V1 network scope). 
- **GLONASS stays float, always.** FDMA per-slot IFB does not cancel in
  between-station SD without per-slot IFB calibration. The infrastructure to
  exclude it already exists: `MeasurementModel::non_integer_capable_dd_ambiguity_keys_`
  and `markAmbiguityUnresolvable()`. Reuse the same gate — do not invent a new one.

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

**The AR step runs strictly between `solve()` and `toSolutionEpoch()`.** It
reconstructs ambiguities, fixes DDs, re-pins to integers, re-solves, and hands
the integer-consistent `x_s` to `toSolutionEpoch`.

---

## 4. Implementation phases

### Phase 0 — Extract a pure ILS entry point  *(refactor, no behavior change)*

Expose the already-generic core as a free/static function so both the user path
and the new network path call one implementation:

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

`IlsGateConfig` = `{ratio_threshold, ratio_upper_bound, min_success_rate,
nl_max_per_pair_frac, enable_partial_ar}`. Re-point `AmbiguityResolver::resolve`
/ `resolveDd` / `resolveWlConstrained` at `ilsFix` internally (they keep their
StateManager wrappers). This phase is independently unit-testable against the
existing 190-test suite (the CLAUDE.md "no tests" note is stale — see
`project_test_suite_initiative`).

### Phase 1 — Reconstruct DD float block with the correct conditional covariance

After `solve()`, for each non-pivot station call `reconstructStation(i, x_s)` to
get `x_r` (incl. float ambiguities). The **covariance is the subtle part** and
the #1 correctness risk:

- `shared_cov_` is from the **datum-constrained** reduced normals: pinned
  columns carry an artificial variance `1/pin_scale`, NOT a true posterior.
- The ambiguities must be conditioned on the pinned shared params as **KNOWN
  constants**, not random. So the naïve
  `Cov(x_r) = N_rr⁻¹ + N_rr⁻¹ N_rs Cov(x_s) N_sr N_rr⁻¹` is wrong if it pulls in
  pinned-column variance.

Correct recipe: build the *conditional* covariance using only the **free**
(non-datum-pinned) shared block:

```
Cov(x_r | x_s^free) = N_rr⁻¹ + N_rr⁻¹ N_rs · Cov_free(x_s) · N_sr N_rr⁻¹
```

where `Cov_free` zeroes the rows/cols of datum-pinned columns (they are exact).
The LDLᵀ factor of `N_rr` is still cached on the station block — reuse it; do not
refactor `N_rr`. Form the DD covariance by differencing in the standard way
(`Qb(i,j) = P(i,i) − P(i,ref) − P(ref,i) + P(ref,ref)` across station/sat).

**Validation gate (free):** reconstruct the *pivot* station's pinned
ambiguities; they must equal their pins to ~machine precision. If not, the float
solve is suspect — skip AR this epoch and emit float (fail-open).

### Phase 2 — Form DDs, run WL→NL cascade, fix

1. Pick pivot station (`clock_datum_station`) and per-constellation pivot sat
   (`Plan::clock_pivot`) — reuse, do not re-derive.
2. Form between-station DDs vs the pivot station for every other station; build
   the spanning-tree set (one independent DD per extra station per extra sat).
3. **Exclude** GLONASS and code-channel-mismatch DDs via the existing
   `non_integer_capable_dd_ambiguity_keys_` gate.
4. **WL first (recommended):** form Melbourne-Wübbena DD widelanes from
   per-station MW arcs, fix WL integers (geometry-free, robust). *Caveat:* the
   network has no per-station MW aggregation today — see Gaps §6. For a first
   cut, the **NL-direct LAMBDA** path (no WL cascade) is acceptable on the
   strong network geometry; add the WL cascade in a second iteration.
5. Call `ilsFix(â_DD_cycles, Q_DD_cycles, gates)` with **strict** gates
   (§5). Use partial AR — the network should fix the decisively-fixable subset
   and leave the rest float, never force a full-set fix.

### Phase 3 — Re-pin to integers and re-solve

Reuse the existing pivot-pin machinery (the synthetic tight-prior rows appended
to `A_r` in `splitStation`):

- Change the pivot-station ambiguity pins from *float target* to *integer
  target* (round, on the chosen datum).
- For each fixed DD, append an integer constraint (one extra tight-prior row per
  fixed DD, same `w = 1e10`, target = fixed integer).
- **Re-reduce + re-solve** (`reduce()` + `solve()` again). This is cheap
  relative to assembly — the per-station `N_rr` LDLᵀ factors are cached; only
  the appended pin rows change the local blocks.
- `toSolutionEpoch(x_s_fixed, ...)` now yields integer-leveled biases.

**Hard guard:** if `datum_changed` (pivot sat handover this epoch), do NOT carry
or apply previous-epoch integers — re-fix from scratch or emit float. A handover
invalidates the integer datum.

**Pivot exclusion:** never let AR fix a *pivot-receiver* ambiguity to a value
inconsistent with the datum integer — that would break Odijk Eq. 4 and jump the
bias. The pivot ambiguities define the datum; they are set, not searched.

### Phase 4 — Product semantics and consumer frame

**Recommended (V1): no new serialization field.** Emit the (now
integer-consistent) total bias in meters as today. The consumer's *own* AR
absorbs the integer part natively against a stable surface — it does not need a
separate `integer_absorbed`. This avoids a `NetworkProductIO` format bump.

Do two things:
- Set `signal_integer = true` (and `wide_lane_integer` if WL fixed) on fixed
  (sat, signal) entries.
- **Bump `discontinuity_counter` on any integer-datum change** (pivot
  re-reference, or a fixed integer that flips) even when `bias_m` is smooth.
  Use `NetworkProductEmitter::markDatumChange()` so the consumer's
  `StateManager::noteSsrDiscontinuity` → `resetAmbiguity` fires cleanly.
  *Widen the counter to ≥32-bit to avoid the 2-bit IGS-SSR aliasing wrap.*

**Consumer MUST run `consumer_undifferenced = true`.** Integer-leveled bias +
BSSD = frame conflict (BSSD keeps the ambiguity definition receiver-relative).
The undifferenced user model applies the bias directly per-band and dissolves
the `.BIA`↔network frame conflict — this matches the locked direction in
`project_ppp_rtk_network`. Add a **config validation gate**: if the consumer
estimates sat phase biases against a network-AR product and is *not*
undifferenced → hard error.

**Sign convention:** `kSsrPhaseBiasSign = +1.0` (bias added to clean model).
Keep producer and consumer identical. Add a fractional-residual histogram
diagnostic — if post-AR float fractions cluster near 0.5 instead of 0, the sign
is flipped.

### Phase 5 — Validation (the closed-loop harness is the KPI)

The held-out closed-loop consumer (`cl_*` counters, `closed_loop_trace.csv`) is
the built-in A/B rig. Run:

- **A (baseline):** producer float-only → consumer AR fix rate / TTFF / RMS.
- **B (this feature):** producer integer-leveled → same metrics.
- Isolate bias quality with `consume_network_clock = false` (precise clock +
  network phase bias only — the cleanest test).

Expected: B shows **higher `fix_rate`, lower `time-to-first-fix`, lower
converged 3D RMS**. Add `time_to_first_fix` and per-epoch `num_candidates` to
the trace if absent.

**Metric-inflation caveat (log it):** once producer AR runs, the consumer fix
rate is no longer a "clean" indirect validator of the *float* product — it now
measures the *fixed* product. Keep the float-producer A run as the reference and
state this explicitly in the summary line.

---

## 5. Validation criterion (ties to the FFR decision)

Per `project_ar_validation_criterion`: gate with the **fixed-failure-rate
(bootstrap success rate)**, not ADOP. On the network use a **stricter** target
than the user, because a wrong network fix poisons every downstream user:

| gate | user default | **network default (proposed)** |
|------|--------------|-------------------------------|
| `min_success_rate` (FFR) | 0.999 | **0.9999** (1e-4 target) |
| `ratio_threshold` | 3.0 | 3.0 (backstop) |
| `ratio_upper_bound` | 1e4 | 1e4 |
| `nl_max_per_pair_frac` | 0.15 cyc | **0.10 cyc** |
| `enable_partial_ar` | false | **true** (fix the fixable subset) |

ADOP stays a **diagnostic / feasibility** scalar (per-sat bias estimability,
geometry adequacy, logged QC) — not a gate. Do not build the Verhagen–Teunissen
FF-RT critical-value machinery; the bootstrap gate already controls failure
rate.

New config section (mirror `estimation.ambiguity_resolution`):

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

Wire via a `loadNetworkSection_` helper in `GnssConfigLoad.cpp` (follow the
`jval` pattern in CLAUDE.md conventions). Default `enable=false` so the float
product is bit-identical until explicitly turned on.

---

## 6. Gaps / new code required (from the audit)

| Item | Status | Notes |
|------|--------|-------|
| Reconstruct ambiguities + covariance | **partial** | `reconstructStation` exists; conditional-covariance-on-pinned-as-known is new (Phase 1). |
| Pure `ilsFix(â, Q)` entry point | **partial** | core is generic; expose it (Phase 0). |
| `selectFixableSubset_` reuse | **partial** | exists, currently private — expose. |
| Network DD formation + covariance | **missing** | new orchestration (Phase 2). |
| Integer re-pin + re-solve | **reusable** | extend existing pin builder to integer targets (Phase 3). |
| Per-station MW aggregation for WL DD | **missing** | needed only for the WL cascade; defer (Phase 2 step 4). |
| Datum-handover guard for integers | **missing** | reuse `datum_changed` flag (Phase 3). |
| Product flags + discontinuity on datum change | **reusable** | `markDatumChange`, widen counter (Phase 4). |
| `consumer_undifferenced` enforcement | **reusable** | flag exists; add validation gate (Phase 4). |
| Closed-loop A/B + TTFF KPI | **reusable** | harness complete; add trace columns (Phase 5). |

---

## 7. Risk register (carry into review)

1. **Conditional covariance (CRITICAL).** Pinned shared columns have artificial
   `1/pin_scale` variance. Condition DD ambiguities on them as *known*. Getting
   this wrong feeds LAMBDA a wrong `Q` → over/under-confident fixes that pass
   FFR spuriously. *Verify against a brute-force full-inverse on the 3-station
   internal fixture.*
2. **Pivot ambiguity integrity.** Pivot pins define the datum; never search
   them. A non-zero/non-datum pivot fix jumps the bias (Odijk break).
3. **Datum handover.** Pivot-sat change invalidates carried integers; the
   network currently re-references without an S-transform. Guard hard; consider
   sticky-pivot hysteresis (cf. `DdPivotManager`) before multi-epoch production.
4. **Consumer frame.** Integer-leveled + BSSD = silent frame mismatch. Enforce
   `consumer_undifferenced`. 
5. **Sign convention.** `+1.0` both sides; histogram fractions to catch a flip.
6. **GLONASS.** Float only — reuse `non_integer_capable` gate; do not attempt.
7. **Numerical conditioning.** `pin_scale = max(diag N)` with heterogeneous
   satellite weighting can distort LDL pivoting; watch the re-solve residuals.
8. **Metric inflation.** Producer AR makes the closed-loop fix rate measure the
   fixed product, not the float — keep the float A run and say so.
9. **Validation honesty.** Single held-out station validates the bias datum but
   not inter-station DD geometry; add 2–3 held-out stations later.

---

## 8. Suggested commit sequence

1. `IlsCore` extraction + unit tests (Phase 0) — independently mergeable, zero
   behavior change.
2. `reconstructStationAmbiguities` + conditional covariance + pivot
   self-consistency check + unit test on the 3-station fixture (Phase 1).
3. DD formation + `ilsFix` call, gated behind `network.ambiguity_resolution.enable`
   (Phase 2, NL-direct first).
4. Integer re-pin + re-solve + handover guard (Phase 3).
5. Product flags + discontinuity-on-datum-change + `consumer_undifferenced`
   validation gate (Phase 4).
6. Closed-loop A/B trace columns + run the IGS_Pasadena and internal 3-station
   fixtures; record fix-rate/TTFF/RMS deltas (Phase 5).
7. (Later) per-station MW aggregation + WL→NL cascade.

Each step builds and is independently testable; only step 3 onward changes
output, and only when the flag is on.
