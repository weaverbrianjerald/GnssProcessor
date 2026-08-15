# SPEC: Non-Causal Cycle-Slip Bridge + Post-Smoother AR Feedback

**Status (tree-verified 2026-08-14): MIXED — kernels LANDED, wiring OPEN. `processing.enable_noncausal_slip_bridge = true` IS A LIVE NO-OP.** Nothing in the pipeline calls the two built kernels. OPEN/OWED: §13.

Consolidated 2026-08-14 from `noncausal_smoother_impl_plan.md` (base), `noncausal_smoother_jumpfit_scope.md` and `noncausal_smoother_ar_feedback_addendum.md` — all three now in `docs/attic/`. Task labels **T1–T8** and the scope doc's phase labels survive verbatim so a label grep still lands. Post-processing only (`Application::run`, never `runRealTime` → `RealTimeEngine`). Provenance: `docs/slip_comparison_report.md` §3.2 and §4 (M-PAGES `fitGap` is the origin of this card).

> **⚠ §§9–12 specify capability (B), which is NOT BUILT.** Read every imperative there as design intent, not live mechanism (R6). Statements about existing code inside those sections are tagged *(live)*.

## 1. Status — what LANDED, and the live no-op

| BUILT artifact | Tests | CMake |
|---|---|---|
| `include/slip/JumpGapFitter.hpp` + `src/core/slip/JumpGapFitter.cpp` | `tests/test_jump_gap_fitter.cpp` (7 cases) | `CMakeLists.txt:134` |
| `include/slip/SlipBridgeKernel.hpp` + `src/core/slip/SlipBridgeKernel.cpp` | `tests/test_slip_bridge_kernel.cpp` (6 cases) | `CMakeLists.txt:135` |

Landed deviations from the plan's API sketch: `JumpGapFitter` is **time-CENTRED on the boundary** for conditioning; `SlipBridgeKernel` gained **`resolveOrder`**, which orders hi-freq-first internally; the GF/MW formulas mirror `computeGfMeters` / `computeMwMeters` verbatim.

**NOT LANDED — T4 entirely.** `src/core/app/ApplicationBridge.cpp` does not exist. `SlipBridgeKernel::recover` / `buildWindows` / `resolveOrder` are referenced **only** by `tests/test_slip_bridge_kernel.cpp` — zero references in `Application.cpp` or `Smoother.cpp`. `include/filter/NonCausalJumpReestimator.hpp` (scope-doc naming) and `include/filter/SmoothedAmbiguityResolver.hpp` (addendum naming) were never created. T8 [effort S] did not land either: none of the four keys appears in any shipped `config/*.json`. **Net effect: the config keys parse, the real-time force-off fires, and nothing else happens.**

## 2. Goal, value, and what v1 explicitly does NOT buy

At each forward RESET boundary, use the full past+future observation record to re-derive both-sides GF/MW windows, fit trend+step, recover `(N1,N2)` from a from-scratch `{GF,MW}` algebraic kernel, and — only when a conservative gate passes — **bridge** the two smoother segments so the spliced arc smooths across the boundary instead of being severed.

Today every reset discards the float arc's backward information **and** the slip integer: only *repairs* retain `dN` (`ambiguity_repairs`, carried as `dN·λ`); *resets* segment. The v1 win is therefore **float-ambiguity continuity and recovered slip integers** — longer continuous float arcs through the RTS recursion, plus a diagnostic ledger of recovered integers at would-be resets. **STATIC-PPP HONESTY.** Smoothed *position* is degenerate (constant across epochs by construction), so (A) buys ambiguity continuity and integer recovery, **not position**. The payoff concentrates in kinematic / long-arc / noisy datasets and is minimal in clean static runs. Do not let a reader mistake per-epoch trace rows for independent position solutions.

Capability **(B)** — the post-smoother AR feedback pass — rebuilds a transient `StateManager` per smoothed epoch and runs a fresh `AmbiguityResolver` at a strictly higher ratio, **emit-only** to a CSV. Its benefit is a higher final fix rate / cleaner fixed-ambiguity product — **not TTFF** (a causal metric, meaningless for a backward batch pass).

## 3. Feasibility: data retention, and why the existing detector windows cannot serve the after-side

- **Full parsed RINEX is resident for the whole run.** `RinexParser::getObservations()` returns `observations_` by const ref (`RinexParser.hpp:122,233`) and the forward loop already iterates it by index (`Application.cpp:1510-1518`, Jun-2026 anchor) ⇒ both-sides windows at ANY past boundary are re-derivable at **zero new bulk storage**.
- **Existing detector windows are forward-only and structurally cannot serve the after-side:** `MwArcStats` caps at 30/64; `GfArc` keeps only `prev_gf` / `prev_prev_gf` plus a 30-sample MAD deque.
- **The single biggest blocker:** the reset `dN` is **LOST** (resets re-seed + segment), so the jump magnitude must be re-estimated from observables — exactly what this pass does.
- **No per-signal reset truth on the snapshot.** `reset_satellites` is a `std::set<std::string>` of **SAT IDs**, prefix-matched in `ambSatWasReset` (`Smoother.cpp:37-45`) ⇒ v1 bridges the dual-freq pair **JOINTLY**; no per-band partial bridge.
- Boundary locations are already retained on `smoother_buffer_[k].reset_satellites` / `is_discontinuity`, consumed only at the `Smoother` construction site.

## 4. Landed kernels — T1 JumpGapFitter and T2 SlipBridgeKernel

**T1 — `JumpGapFitter`** [effort M] (pure, stateless, zero GNSS deps). Independent LSQ `y = a + b·t` per side via 2×2 normal equations (guard `det`), both evaluated at `t_boundary_s`; `step_m = post_level − pre_level`; `rms_resid_m` over both sides; `ok=false` if either side has `< min_window_points` (4) or any non-finite. Design row in the scope-doc formulation: `[1, step_ind(0 pre / 1 post), (n−n1)^1..^p]`, sentinels skipped in the normal equations, returning the **step coefficient** (jump in series units), per-side valid counts `k1,k2`, and a residual-based σ. Mirrors M-PAGES `fitGap` (`DbEdit.cpp:971-1033`). Unit-test contract (`test_jump_gap_fitter.cpp`, landed): synthetic `y = 2.0 + 0.1·t` with a `+0.862 m` step ⇒ `EXPECT_NEAR(step_m, 0.862, 1e-6)` and `EXPECT_NEAR(pre_slope, 0.1, 1e-9)`; `n_pre < 4` ⇒ `ok=false`; differing pre/post slopes recovered independently. **Polynomial-order rationale:** ship at **1** (constant + step). Order 2 over-fits a 5-point side once gaps drop samples, and lower order tightens σ ⇒ fewer mis-bridges. Bump to 2 only if traces show iono curvature biasing the step.

**T2 — `SlipBridgeKernel`** [effort L] (holds a `PppUtils&`; FDMA inherited from the borrowed instance — never recompute FCN). `buildWindows` + `recover` (+ the landed `resolveOrder`).

### THE {GF,MW} ARITHMETIC TRAPS — the highest-risk math in the card

Per-epoch **GF** (m) = `L1_cyc·λ1 − L2_cyc·λ2`. Per-epoch **MW** (m) = `(f1·L1m − f2·L2m)/(f1−f2) − (f1·P1 + f2·P2)/(f1+f2)`, `Lm = Lcyc·c/f`.

- **MIRROR THE ASYMMETRIC ARG ORDER EXACTLY** — `getFrequency(sig, sys, sat)` vs `getWavelength(sys, sig, sat)`. This is the **#1 bug source**.
- **RE-DERIVE the two linear relations; do NOT transcribe the `td2/denom` substitution** — the `λ1`-in-numerator asymmetry is the documented easy bug. Constraint GF: `λ1·dN1 − λ2·dN2 = gf_step_m`; constraint MW: `dN1 − dN2 = dN_wl`. Solve: `λ_wl = c/|f1−f2|`; `dN_wl = round(mw_step_m/λ_wl)`; `dN2 = round((gf_step_m − λ1·dN_wl)/(λ1−λ2))`; `dN1 = dN2 + dN_wl`; `frac_resid_cyc = |((gf_step_m − λ1·dN_wl)/(λ1−λ2)) − dN2|`.
- **ENFORCE `sig1` = higher frequency** (assert or swap) — a wrong-order pair flips the WL-integer sign on E5/E7 and silently corrupts attribution. Verified constant: **λ_wl ≈ 0.862 m** for GPS L1/L2.
- **Guards:** `|λ1−λ2| < 1e-6` ⇒ `ok=false` (degenerate same-band); a GLONASS sat with an unset slot returns freq `0.0` ⇒ `ok=false` (safe-fail). **Acceptance:** `frac_resid_cyc ≤ 0.30`; `|dN1|,|dN2| ≤ 1000`; not both zero.
- **The integer-injection tests are the ONLY guard against the asymmetric-arg / λ1-in-numerator bug — never delete them.** Cases: GPS L1C/L2W/C1W/C2W with injected `(dN1,dN2)` → `EXPECT_EQ`; `λ_wl ≈ 0.862 m`; wrong-order `sig1<sig2` → swap/assert; degenerate same-band → `ok=false`; GLONASS via `setGlonassSlots` on the borrowed `PppUtils`.

### API facts pinned by the build (the window builder depends on all four)

- `getObservations()` returns a **FLAT `std::vector<RinexObservation>`** — one sat per element per epoch, **NOT grouped**. The builder must filter by `.satellite` AND split by `.time` itself.
- The field is **`.measurements`** (`RinexParser.hpp:38-50`), **NOT `.raw_observed`** (that belongs to `PppObservation`). `L*` values are **cycles**, `C*` values are **metres**.
- Time via `TimeUtils::continuousGpsSeconds`; `freq_type` takes the **full phase code** (`"L1C"`).
- **Elevation-agnostic robust σ** is committed for v1: `RinexObservation` carries no elevation, so there is no detector-parity `1/sin(el)` weighting.

## 5. Config surface — T3 (four landed keys), the two that never landed, and the abandoned `ncjr_*` vocabulary

**LANDED (verified 2026-08-14), all default-OFF, none fingerprinted** — this is the post-processing PPP path, not the network producer. T3 effort M.

| Key | Default | Getter |
|---|---|---|
| `processing.enable_noncausal_slip_bridge` | `false` | `enableNoncausalSlipBridge()` — `GnssConfig.hpp:1096` |
| `processing.slip_bridge_half_window_epochs` | `30` | `getSlipBridgeHalfWindowEpochs()` — `GnssConfig.hpp:1132` |
| `processing.pre_bridge_float_tol_cycles` | `0.5` | `getPreBridgeFloatTolCycles()` — `GnssConfig.hpp:1144` |
| `processing.pre_bridge_conv_var_m2` | `0.0025` = (0.05 m)² | `getPreBridgeConvVarM2()` — `GnssConfig.hpp:1150` |

Parse `GnssConfigLoad.cpp:710-713`; ctor init `GnssConfig.cpp:127-130`; RealTime force-off `GnssConfigLoad.cpp:2588-2590`; toggle echo `nc_bridge={}` at `GnssConfigLoad.cpp:3136` (argument at `:3160`). Touch point (4) `isLoggerEnabled` is **N/A** — the pass reuses the existing `"smoother"` logger; no new named logger. Config-load test contract: defaults off; RealTime `app_mode` forces both feature bools off. **NEVER LANDED:** `estimation.enable_smoothed_ar_pass` and `estimation.smoothed_ar_min_ratio` — **capability (B) has no config surface at all.** The addendum's third key, `logging.enable_smoother_fixed_trace` (default `false`, column registration through `TraceLogger` rather than an ad-hoc writer, added to `isLoggerEnabled`), also never landed; it is the addendum's preferred alternative to the base plan's "reuse the `smoother` logger and write the CSV directly".

**SUPERSEDED VOCABULARY — the `ncjr_*` names do not exist anywhere in the tree.** Kept as a mapping table so no one greps for dead keys, and so the **tuning VALUES** (the real content) survive:

| Abandoned `ncjr_*` name | Section | Value | Fate / meaning |
|---|---|---|---|
| `processing.run_noncausal_jump_reest` | `processing` | `false` | → `enable_noncausal_slip_bridge` |
| `ncjr_epochs_surrounding_cs` | `estimation` | `5` | → `slip_bridge_half_window_epochs` (landed at 30) |
| `ncjr_polynomial_fit_terms` | `estimation` | `1` | constant+step; see §4 rationale |
| `ncjr_cs_fix_fraction_limit` | `estimation` | `0.15` | per-jump fractional accept gate; `0.0` = log float, never bridge |
| `ncjr_consistency_tol_cycles` | `estimation` | `0.20` | GF↔MW mutual-consistency tolerance (§7) |
| `ncjr_max_gap_seconds` | `estimation` | `120` | windows never span a larger gap |
| `ncjr_min_valid_per_side` | `estimation` | `3` | minimum valid epochs per side per series |
| `ncjr_enable_glonass` | `estimation` | `false` | FDMA biases distort GF/MW jump magnitudes |

Two further superseded plan decisions. (a) `smoothed_ar_min_ratio` was specified as `ar_ratio_threshold_ + 0.5` with clamp-up-and-warn, parsed immediately after `ar_ratio_threshold_` — which the plan pinned at **line 1031** of `loadEstimationSection_` and which is **now at `GnssConfigLoad.cpp:1558`**. (b) The scope doc's phasing ("Phase 2 diagnostics-only, the project should STOP there for the first PR") was superseded twice — the user locked *the bridge MUTATES state in the first PR*, and then the increment that actually shipped was Phase 1 kernels alone. The plan's PR split (PR-1 = A, PR-2 = B default-off) died with it.

## 6. The bridge mechanism — CORRECT (copy-and-shift inside `Smoother::run`) vs the SUPERSEDED F1-fold shortcut

> ### ⚠ THE SUPERSESSION — do not re-lose this
> **The base plan's own T4 — "emit the bridge as a repair by reusing the existing F1 fold in `Application.cpp`: add `dN·λ` to the boundary snapshot's `x_pred` and erase the sat from that snapshot's `reset_satellites`" — is WRONG FOR A RESET, and is SUPERSEDED.**
>
> **Why F1 works for a REPAIR:** `repairAmbiguityBssdAware` (`CycleSlipManager.cpp:258-336`, Jun-2026 anchor) is a covariance-preserving `+dN·λ` shift of **ONE CONTINUOUS state**, so only the a-priori needs folding — exactly what the F1 fold does (`Application.cpp:1947-1959`; the code comment there already says *"A key also RESET this epoch is segmented, not compensated"*).
> **Why it fails for a RESET:** `resetAmbiguity` (`StateManager.cpp:1108`) **SEVERS** — `covariance_.row(idx).setZero(); covariance_.col(idx).setZero(); covariance_(idx,idx) = std_m²`, then re-seeds via `ensureRequiredStates`. The pre and post arcs are two **INDEPENDENT** estimates differing by ≈`dN·λ`. **No single `x_pred` edit re-couples them** — the smoother would bias the pre arc by `C·dN·λ`.
> **Therefore T4 MUST edit `Smoother.{hpp,cpp}`** (a delicate RTS-recursion change), not just `Application.cpp`. Effort is **[L]**, not the plan's **[S–M]** "reclassification reusing proven machinery". **The F1 fold stays exactly as-is for repairs.**

**The correct mechanism (from the absorbed scope doc; this is the design to build):**

1. `SmootherEpochSnapshot` gains `bridged_ambiguity_offsets` — `std::map<std::string,double>`, amb_key → `dN·λ` **in METRES** (consistent with `ambiguity_repairs` and the F1 fold), declared after `clean_handovers` (scope doc: `Smoother.hpp` ~line 117; `clean_handovers` is live at `Smoother.hpp:118`). Full Doxygen; note that it overrides the `reset_satellites` exclusion. Analogous to `clean_handovers`.
2. **Per-KEY allow-override in `collectCommonIndices`** (`Smoother.cpp:120-128`, the `ambSatWasReset` exclusion at `:121`): if `amb_key ∈ sk1.bridged_ambiguity_offsets`, **keep** the key. Per-KEY, not per-sat.
3. In the backward loop, **copy-and-shift BOTH `x_pred_k1` AND `x_s_k1`** for the bridged index by the same `dN·λ`, so the RTS innovation `x_s_{k+1} − x_pred_{k+1}` stays coherent (the RTS step itself is `Smoother.cpp:405`).
4. **NEVER mutate the stored `x_post(k+1)` / `smk1.x`** — that would corrupt the already-completed `k+1 → k+2` step. Follow the existing `prepareHandoverFrame` / `inverseReReferenceGroup` copy-and-adjust pattern (`Smoother.cpp:140-162`, `Smoother.cpp:351-360`; live code at `:355-365`). **Covariance unchanged.**
5. On ACCEPT the pass also **erases** the sat from `reset_satellites[k+1]`; on REJECT it leaves it and the existing break stands.
6. Annotate `smoother_buffer_` **by reference BEFORE** the `std::move` into the `Smoother` ctor (`Application.cpp:1916`, Jun-2026 anchor). The pass runs once per process; mutations are not re-entrant.
7. **INDEX-RESOLUTION SEQUENCING FIX:** resolve the ambiguity index against the **SNAPSHOT'S OWN** ambiguity map, **not** the post-loop live `StateManager` (which may have grown). The F1 fold uses the live map only because it runs at forward-loop time.

## 7. The conservative accept gate

A wrong bridge is **strictly worse than a clean break**: bridging keeps the converged pre-slip *tight* covariance across a real slip, so a wrong integer injects a confident lie into the smoother. **Why the causal threshold cannot be reused verbatim:** the both-sides σ averages pre+post and is **SMALLER**, so the same nominal fraction limit is effectively **LOOSER** — hence **0.15** here vs the causal TurboEdit **0.25/0.30**. **Bias every default toward false-negative (break) over false-positive (mis-bridge).**

Accept **only if ALL FIVE** hold:

1. `k1 ≥ ncjr_min_valid_per_side` **and** `k2 ≥ ncjr_min_valid_per_side`, on **BOTH** series.
2. `frac1 < 0.15` **AND** `frac2 < 0.15`.
3. **Split-capability on the symmetric σ** (reuse the `splitCapable` philosophy, `TurboEditResolver.cpp:214-222`): `σ ≤ split_max_sigma·|λ1−λ2|`.
4. **MANDATORY mutual-consistency cross-check:** the MW-implied WL integer must agree with `N1−N2` from the GF inversion within **0.20 cyc**. Without it the 2×2 returns a clean-looking `(N1,N2)` from two *inconsistent* jumps.
5. Not all-zero; `|N|` sanity cap; finite `|λ1−λ2|` separation margin.

Integer rounding is **round-to-nearest** with a symmetric fraction gate (M-PAGES uses asymmetric `(int)` truncation — deliberately not copied). Second observable is **`{GF,MW}`**, chosen over M-PAGES's `{GF,IF}` because the IF SD is sensitive to pivot changes. If MW yields only a float WL candidate (`slip_wl_float`, `MwSlipDetector.hpp:52-74`), use the **candidate-`k` loop** form (`TurboEditResolver.cpp:1009-1014`), not single-`k`.

## 8. The pre-bridge-float independence gate

This is the base plan's unique contribution and **the ONLY genuine backstop identified anywhere in the three docs.** A naive backstop WOULD be circular: the smoothed float across the boundary already contains the bridge jump, so comparing recovered `dN` to the *smoothed* float step is self-confirming. We never do that.

**The two genuinely independent floats, both captured DURING the forward loop.** **FLOAT #1 (pre-slip converged arc):** the forward Kalman posterior for that key in the epoch immediately BEFORE the reset — `smoother_buffer_[k-1].x_post(idx)`, SD frame, mature/converged/pre-slip. **FLOAT #2 (independently re-converged post-reset arc):** the **geometry-reseeded** value, captured **LAZILY** at the first forward epoch where `P(idx,idx) < pre_bridge_conv_var_m2`. A reset does not retain the pre-slip value; `computeAmbiguitySeed` recomputes it as `observed − modeled − clk − tropo − isb` (`MeasurementModel.cpp:396-443`), BSSD subtracting the pivot same-signal seed.

**The retention API that FEEDS this gate (T4, unbuilt), on `Application.hpp`:**

```cpp
struct BoundaryFloatRecord {
  size_t snapshot_index; std::string sat; std::string sig;
  int idx_at_reset; double pre_slip_float_m; double post_reset_converged_float_m;
  bool post_converged{false}; bool pivot_changed{false};
};
std::map<std::string, BoundaryFloatRecord> boundary_float_records_;
void retainBoundaryFloats(const std::set<std::string>& reset_sats, size_t prior_snapshot_index);
void updatePostResetConvergedFloats();
bool applyNoncausalSlipBridge();
```

Three hook sites, ordering load-bearing. **(a)** At the reset-merge site, AFTER `epoch_reset_sats` is finalized and BEFORE `capturePosteriorSnapshot` (plan `Application.cpp:1797-1800`; live `:1983` and `:1996`) — call `retainBoundaryFloats`, take FLOAT #1 from the prior snapshot's `x_post(idx)`, set `pivot_changed` from a `pivot_satellites` delta between `k-1` and `k`, record `idx_at_reset` AND `snapshot_index`. **(b)** Immediately AFTER `capturePosteriorSnapshot` — call `updatePostResetConvergedFloats()` for records not yet `post_converged`. **(c)** The Phase-5 prep call `applyNoncausalSlipBridge()` sits inside `if (runSmoother() && !smoother_buffer_.empty())`, BEFORE the `Smoother` construction.

**Two-axis independence.** (1) *Bridge-independence*: the fold runs at Phase-5 prep, AFTER the whole forward pass, so neither float ever saw the mutation. (2) *Algebra-independence*: FLOAT #2 is a phase-**GEOMETRY**/clock/tropo solution, entirely separate from the GF/MW combination algebra that yields `dN`.

```
float_step_m     = post_reset_converged_float_m − pre_slip_float_m   // same SD frame, same signal
recovered_step_m = dN_sig · λ_sig                                     // L1→dN1·λ1, L2→dN2·λ2
PASS  iff  post_converged == true
      AND  |recovered_step_m − float_step_m| <= pre_bridge_float_tol_cycles · λ_sig
```

Checked on **BOTH** signals of the pair (or on the WL projection `(dN1−dN2)`). Any signal failing ⇒ no bridge; the sat stays in `reset_satellites`, segmented exactly as today.

**FRAME GUARD, fail-closed.** If `pivot_changed` at the boundary (a BSSD pivot handover for that sat — `pivot_satellites` / `clean_handovers` differ between `k-1` and `k`), the two floats are in different SD frames and are not comparable ⇒ **AUTO-FAIL**. This is the **ONLY** protection against a frame-mismatched bridge, so `pivot_changed` detection must be complete and fail-closed.

**IRREDUCIBLE RESIDUAL RISK.** A **coherent mis-bridge** — where the GF/MW algebra and the geometry float BOTH mis-attribute by the same integer — **can pass the gate**. The gate bounds error to integer-consistent attributions; it does not eliminate a globally-coherent wrong jump. This is why (A) ships default-OFF with trace inspection required. Also: a sat that never re-converges below `pre_bridge_conv_var_m2` keeps `post_converged=false` ⇒ fail-closed, so short arcs after a slip are never bridged.

## 9. Capability B — T5 post-smoother AR pass: transient `StateManager` reconstitution and its three blockers

*(NOT BUILT — design. Effort L.)*

**AR is a PURE PASS over a snapshot** *(live)*. `resolve()` / `resolveDd()` read `x`, `P` and the ambiguity key→index map entirely from a `const StateManager&` and return a self-contained `AmbiguityResolution{fixed_state, fixed_covariance}` (`AmbiguityResolver.cpp:488-489`; `AmbiguityResolver.hpp:35-42`). The **only** forward-loop coupling is the per-instance `epoch_count_` min-epochs counter (`AmbiguityResolver.cpp:297, 450-455`) ⇒ a **fresh resolver instance** handles it. **Rejected alternatives, explicitly:** (i) an integer-constraint re-run *inside* the forward loop — that is the live bridge, already scoped; (ii) a new arbitrary-`(x,P)` `resolve()` overload baked into the smoother — unnecessary, the snapshot path already yields a faithful per-epoch layout. **CONVENTION NOTE (state-vector discipline).** The transient-state populate helper takes **RAW `(x, P, key→index maps)`, NOT a `SmootherEpochSnapshot`.** `capturePosteriorSnapshot` (`Application.cpp:1148-1197`, Jun-2026 anchor; definition now `:1320`) reads *out of* `StateManager` via public getters; coupling `StateManager` to a smoother/application type inverts that and violates the state-vector-discipline rule.

**BLOCKER 1 — there is NO public `addSystemTimeOffset`.** `StateManager` exposes only `getSystemTimeOffsetIndex` / `getSystemsWithTimeOffsets` / `getNumSystemTimeOffsets` (the Jun-2026 anchors `StateManager.hpp:444/467/477` — **re-grepped 2026-08-15: now `:554`, `:577`, `:587` respectively**; `addSystemTimeOffset` still does not exist). ISB/ISBR slots are allocated **INTERNALLY during construction from config** (`StateManager.cpp:307-333`), contiguous in core BEFORE any dynamic key. Because the transient SM is constructed with the **same `config_`**, the ctor reproduces the ISB layout automatically ⇒ the helper **must NOT register sys_offsets at all**. Registering a sys-offset adder **DOES NOT COMPILE**.

**BLOCKER 2 — INTERLEAVED index order.** Dynamic states are appended via `appendState` (`StateManager.cpp:114`; decl `StateManager.hpp:1168` → **re-grepped `:1363`**) monotonically in **first-creation order ACROSS MULTIPLE CATEGORIES**, so a sat's iono and ambiguity interleave globally with other sats' states. Per-category sorted registration yields a different global append order and the index-equality assertion fires on essentially every real run. **FIX: merge ALL FOUR dynamic maps into a SINGLE ascending-index sequence** (sort the union of `(index, key, category)` by index) and dispatch each key to `addAmbiguity` / `addIonosphereState` / `addCodeBiasState` / `addSatBiasState` in that one global order. **ASSERT returned index == source index per key as a HARD failure**, not best-effort.

**BLOCKER 3 — PIVOT RECONSTRUCTION.** `pivot_satellites` maps constellation → sat-ID, **NOT** to an amb_key. AR detects a frozen pivot by `P(idx,idx) == 0.0` **EXACTLY** (`AmbiguityResolver.cpp:314`; `:351` in the Jun-2026 anchors) and skips it in `buildDdPairs` (`AmbiguityResolver.cpp:349-362`, Jun-2026); smoothed `P` has **nonzero** pivot variance. For each pivot sat, reconstruct the pivot amb_key(s) `sat + "_" + signal` for **EVERY** active signal/band of that pivot sat present in the ambiguity map, and zero each diagonal in a **LOCAL COPY** of `P` before `setCovariance` (no `StateManager` API change). Zeroing only one band while the pivot carries multiple lets AR silently stop excluding the other-band pivot. **The backward pass can perturb a `var==0` column, so re-imposition is mandatory — copying `P_s` through is insufficient.**

**Offline test — `tests/test_transient_state_manager.cpp` (NEW, never written).** Round-trip a real SM through the `capturePosteriorSnapshot` getters → `buildTransientStateManager` → getters: `EXPECT_TRUE x2.isApprox(x)`, covariance `isApprox`, `getAmbiguities() == source`, pivot diagonal(s) `== 0.0` exactly. **Crucially include an INTERLEAVED-index fixture** (sat A amb, sat B amb, sat A iono, …) so the single-global-order registration of BLOCKER 2 is actually exercised. Round-trips go through **PUBLIC getters/setters → no friend-shim needed**; the `XxxTestAccess` seam is reserved for genuinely private numeric kernels. Needs a manual entry in `add_executable(gnss_tests …)`.

## 10. Capability B — T6, the HARD GATE

*(NOT BUILT — design. Effort M.)*

Before any smoothed value reaches AR: rebuild a transient SM from the **FORWARD** `(x_post, P_post, maps)`, run a fresh `AmbiguityResolver` with the forward `ar_config` and `min_epochs=0` over the **SAME** `modeled_obs`, and assert `num_fixed` / `ratio` / `fixed_state` match the forward result. On mismatch: **hard-error and ABORT the smoothed AR pass.** Prove the gate has teeth by deliberately skipping the pivot zeroing (BLOCKER 3) and confirming it **FAILS**. Offline analogue — `tests/test_smoothed_ar_reconstitution.cpp` (NEW, never written; needs its own `gnss_tests` entry): serialize `(x,P,maps,obs)` from a forward-AR-fixable epoch, rebuild the transient SM, run AR, `EXPECT_EQ num_fixed`, `EXPECT_NEAR ratio`, `EXPECT_TRUE fixed_state.isApprox(forward_fixed_state)`; **`GTEST_SKIP` if no AR fixture** — honest, because no AR-on data config ships. **HONEST CAVEAT:** with all shipped configs at `ar_mode=0` there are **zero AR-eligible epochs**, so the *runtime* self-check never fires on real data today — the "teeth on real data" claim is **vacuous** until an `ar_mode>0` config exists. **NO IN-TREE AR-ON CONFIG EXISTS**, so capability (B) cannot be data-validated at all; creating and validating one is a **USER deliverable** and the gating item for (B).

**T7 emit contract** [effort L]. Accepted rows go to `smoother_fixed_trace.csv` (`week,tow,sat,signal,fixed_int,float_m,ratio,ratio_gate,num_fixed,num_candidates,smoothed`). **Partial fix is acceptable** — `resolve()` already supports `num_fixed < num_candidates`; report `num_fixed/num_candidates` per segment. **T6/T7 CUT from v1, with reasons:** `smoothed_ar_require_bridge_confidence` — the circular cross-check cannot deliver the safety property it would imply (§8); and `smoothed_ar_segment_epoch="all"` — ship `last_before_boundary` only, add `all` if traces show boundary-adjacent arcs left unfixed.

## 11. What the ratio test does NOT protect

*(NOT BUILT — these are design constraints on (B). The code facts cited are live.)*

- **THE RATIO TEST DOES NOT CATCH A COHERENT INTEGER MIS-BRIDGE.** It measures separation between best and second-best candidate, **not correctness relative to truth**: a whole-arc integer shift yields a float DD still close to *an* integer with the same tight covariance, and LAMBDA fixes it to the **wrong** integer with a **HIGH** ratio.
- **SMOOTHED-P RATIO INFLATION.** `P_s` is genuinely smaller than `P_post`, so the integer search runs on a tighter `Q_b̂` than the forward pass ever saw, raising the ratio. The future data folded in is time-correlated through the process model, so the ratio test's independent-information assumption is **MORE** violated by `P_s` than by `P_post` ⇒ part of any "higher fix rate" is a **false-accept artifact**, not new information. Guard: `smoothed_ar_min_ratio` **strictly above** the forward `ratio_threshold` (placeholder `forward + 0.5`, explicitly flagged for a data sweep, **NOT final**).
- **`max_position_sigma_m` provides NO extra protection** *(live)* (`AmbiguityResolver.cpp:495-503`, Jun-2026 anchor; live sites `:440/:505/:895`): in static PPP the smoothed position covariance is degenerate, so the gate equals the forward final-epoch value. Explicitly **not** claimed as an independent safeguard. **`ratio_upper_bound`** *(live)* (`AmbiguityResolver.cpp:528`, Jun-2026; live `:538/:672/:1051`) exists because *suspiciously high* ratios are a smell — so a PASSING ratio is not containment either.
- **THE `+inf` RATIO SENTINEL DEFECT** *(live)*. `wl_only` and `NlWl` set `ratio = +inf` (`AmbiguityResolver.hpp:35-42`), so `ratio > smoothed_ar_min_ratio` is trivially true and the stricter-ratio gate is **MEANINGLESS for exactly the WL/NL basis that is the shipped default**. Policy: for `+inf` results fall back to `success && num_fixed >= forward_num_fixed_at_epoch_i` and stamp the trace row `ratio_gate="sentinel"`. For PerBand-with-LAMBDA the ratio gate is real; for `wl_only`/`NlWl` it degrades to a non-regression check.
- **WL/NL `(N1,N2)` mapping risk is NOT CLOSED** *(live)*. `resolveWlConstrained` (`AmbiguityResolver.cpp:1095-1166`, Jun-2026; live `:533`) pins the MW-resolved DD-WL as a fixed integer and LAMBDA-searches NL only. A correct WL with a **WRONG NL** back-transforms cleanly (`DD1 = (nNL + f2/(f1+f2)·dd_wl)·λ1`) and passes the NL ratio test. Inherited unchanged — a residual wrong-fix path identical to the forward pass, **not a new exposure**. **`NlWl` basis is DEGENERATE — gate the smoothed pass OFF there**: full LAMBDA NL in `NlWl` is explicit future work (`AmbiguityResolver.cpp:463-477` applies WL pseudo-obs only and returns).

**STRUCTURAL CONTAINMENT = NO WRITE-BACK** (addendum §2.5 — the only real containment until a pre-repair-float cross-check exists). Smoothed fixes would go **only** to the auditable `smoother_fixed_trace.csv` — never `setStateVector` / `holdAmbiguities`, never back into `smoother_buffer_` or the live `state_manager_`. A wrong accepted fix corrupts only the trace. On AR reject the arc is left float; **no fallback into live state, no retry.**

**STATIC-PPP NO-OP PROOF for (B).** With no resets there is **ONE** segment; its last-before-boundary epoch **IS** `N-1`; and `N-1` is seeded directly from the forward posterior (`Smoother.cpp:295-302`, live `:302-306`) — it is **not** back-smoothed. So smoothed AR **== forward AR** there, and **for a clean static run the ENTIRE feature is a no-op.** ⚠ **This SUPERSEDES the base plan's contrary claim** (impl_plan §8: *"(B) is **not** a pure no-op — smoothed *ambiguity* floats DO vary and can re-fix"*), which was written without the `N-1` seeding argument; the addendum's proof is adopted. Gain exists only for multi-segment runs (short arcs, late-rising sats, arcs converging near end of visibility). **Internal tension:** a successful bridge merges segments and *shrinks* the gain; a failed bridge leaves a short post-gap arc with little backward info. **The trace must LOG the no-gain `N-1` case** rather than report it as an improvement.

## 12. Edge cases, guards and traps

- **SKIP RULES.** Skip `is_discontinuity` / `!has_posterior` / dimension-mismatch boundaries entirely (`Smoother.cpp:320-337`, enclosing predicate `Smoother.cpp:320-358`; live `:326-340`) — hard, whole-filter, not per-sat bridgeable. Skip a sat that is a BSSD pivot at `k` **or** `k+1` (mirror the `repairAmbiguityBssdAware` pivot refusal, `CycleSlipManager.cpp:274-284`). Skip the **ENTIRE constellation** if its pivot changed at that boundary — a frame change surfaces on many peers, and `clean_handovers` records only *clean* handovers (`PivotManager.cpp:708-716`). Never integer-shift a BSSD pivot SD (frozen at 0).
- **Window bounds.** `n1 = max(k+1−N, prevBoundaryForSat+1, arcStart)`, `n2 = k`; `n3 = k+1`, `n4 = min(k+1+N−1, nextBoundaryForSat−1, arcEnd)`. Clamp so a window never crosses an adjacent slip or a gap larger than the max-gap value. Skip sentinel samples in the normal equations; require the per-side minimum on **each** series or reject. Sparse/short arcs ⇒ reject ⇒ break.
- **GLONASS FDMA.** `λ1/λ2/f1/f2` in the inversion are **PER-SATELLITE even for enabled constellations** — resolve λ from an in-window observation (mirror the `repairAmbiguityBssdAware` fallback) and reject if unavailable. For (B), inherit the same `ar_active_systems` list and **CONFIRM it is consulted INSIDE `buildDdPairs`** (not only at a higher gate) so a transient resolver cannot enumerate GLONASS DD pairs; exclude GLONASS keys from any "smoothed-improved" bookkeeping.
- **COMBINATION-KEY SAFETY TRAP.** `bridged_ambiguity_offsets` is in **METRES** but WL/NL keys are in **CYCLES** (`StateKeySchema.hpp:167-198`). **Explicitly assert/skip combination keys** — do NOT rely on split-on-first-`'_'`, which cannot tell `G01_L1W_L2W` from `G01_WL_...`. Multi/triple-freq is out of v1 scope; fall back to break.
- **DOUBLE-APPLY INVARIANT + defensive assert.** The F1 fold acts only on repairs whose sat is **NOT** in `reset_satellites`; this pass acts **ONLY** on reset keys — **disjoint by construction**. Add an assert so a future change that lets a key be both cannot silently stack two offsets.
- **LDLT-FALLBACK OBSERVABILITY GOTCHA.** If `P_pred_{k+1}` LDLT fails and the step falls back to forward (`Smoother.cpp:392-398`; live `:398-405`), **the bridge silently no-ops for that epoch** — the trace MUST record *"bridge attempted but step fell back"* or validation is misled.
- **BUFFER-MOVE ORDERING TRAP.** Any per-epoch state capability (B) needs — `modeled_obs`, `wl_ambs`, `wl_fracs`, snapshot maps, pivots — MUST be copied out **BEFORE** the `Smoother smoother(std::move(smoother_buffer_), ...)` move (the ctor `std::move`s and owns the snapshots, `Smoother.cpp:71-72`). A second read of the moved-from buffer is **EMPTY** — a **silent zero-fix, not a build error**. Wire the copy at the pre-move site, not inside the pass. The smoother and its snapshots must also stay **alive past the trace block** (`Application.cpp:1910-1952`, Jun-2026 anchor; live `:2134-2152`) — it is scope-local today. Also confirm `rinex_parser_` / the observation record survives past the forward loop (cost ≈ one full obs record).
- **⚠ `SmoothedEpoch` DOES NOT CARRY THE DYNAMIC MAPS.** `results()` is index-aligned to the input snapshots, but the maps (ambiguities + iono/code-bias/sat-bias/sys-offset) must be sourced from the **matched `SmootherEpochSnapshot`**, never read off the `SmoothedEpoch`. Boundary selection for (B) uses the predicate at `Smoother.cpp:320-358`.
- **⚠ ATX AR-ELIGIBILITY PARITY.** (B) must replicate the call-site ATX WL-gate filtering on `wl_ambs`/`wl_fracs` — it **lives in `Application.cpp`, NOT the resolver** (plan `Application.cpp:1838-1883`; live `:2029-2100`: `require_atx_coverage_for_ar` strict-coverage drop + `restrict_ar_for_substituted_atx`). Copying the buffers listed in the BUFFER-MOVE trap without this gate gives the smoothed pass **different AR eligibility than the forward pass**. The AR-input cache itself hangs off the pre-AR snapshot hook (`Application.cpp:1178-1184` / `Application.cpp:1797-1800`, Jun-2026 anchors) with trimmed obs; confirm elevation / sat-id / signal are the **only** fields `buildDdPairs` reads (`AmbiguityResolver.cpp:316-378`, Jun-2026) before sizing any cache.
- **PRE-AR CAPTURE INVARIANT.**  `smoother_buffer_` holds **pre-AR FLOAT** ambiguities because `capturePosteriorSnapshot()` is called **BEFORE** AR. Bridged offsets are integer-valued shifts of the *float* state and must not be confused with, or double-applied against, forward AR fix-and-hold. Preserve this ordering.
- **"Smoother-improved" bookkeeping.** Treat an ambiguity as backward-informed **only** when `SmoothedEpoch.smoothed == true` **AND** the key was in that step's common subspace (`Smoother.cpp:78-134`). Reset / orphaned / newly-added keys carry the forward float and confer no benefit. Empty / no-posterior epochs, long-gap teardowns, and `x_pred`/`x_post` size mismatch all mark boundaries; the selector skips them and picks the last **VALID** smoothed epoch in the preceding segment. Single-epoch segments accumulate no backward info ⇒ skip or mark non-improved.
- **RealTime** *(live)*: hard-skipped by the loader force-off (`GnssConfigLoad.cpp:2588-2590`); the RT engine never builds `smoother_buffer_`. **FIX-THEN-RESMOOTH (RTS over integer-constrained ambiguities) is FIRMLY KILLED** — not a "natural next increment"; the smoother is **float-only by contract** (`Smoother.hpp:149-150`).
- **Baseline DD mirror is DEFERRED:** `resolveDd()` + `exclude_keys = meas_model_->getNonIntegerCapableDdAmbiguityKeys()` (`ApplicationBaseline.cpp:882-895`). Baseline AR ships `mode=0` and the DD path is not data-validated — build only after the PPP path is proven. **Also deferred out of v1:** any write-back of smoothed/fixed state (locked out — emit-only); real-time variants; the `docs/report/main.tex` math subsection; and **staging any AR path beyond the WL/NL + PerBand set the forward builder already supports**.
- **VERIFICATION-METHOD LESSON (standing rule).** An adversarial review raised a BLOCKING *"the smoother control-input channel does not exist"* alarm that was **RETRACTED** after tree verification: the reviewer grepped `Smoother.cpp` for an **invented** `epoch_repairs_` symbol and missed the channel because it lives in `Application.cpp` writing `snap.x_pred`. Matches the standing *symbol-absence needs TWO tools* rule.

## 13. Open and owed

| # | Item | Owner | Blocks |
|---|---|---|---|
| i | **T4 wiring** [L] — the `Smoother` copy-and-shift (§6), the `BoundaryFloatRecord` retention API + its three hook sites (§8), and `applyNoncausalSlipBridge` in a NEW `src/core/app/ApplicationBridge.cpp`, plus its build and a real-data smoother run | unowned | **(A)** — the toggle is a no-op until this lands |
| ii | An **`ar_mode>0` PPP config**, created and data-validated | **USER** | **(B) entirely** |
| iii | The `smoothed_ar_min_ratio` **margin sweep** to replace the 0.5 placeholder | — | blocked on (ii) |
| iv | The `docs/report/main.tex` math subsection (file EXISTS) | — | deferred with T7 |
| v | Confirm the trimmed obs set (elevation / sat-id / signal) is sufficient for `buildDdPairs` before sizing any per-epoch cache — a 24 h 1 Hz multi-GNSS run is ~86,400 epochs | — | (B) plumbing |

**Validation cadence when T4 lands** (build per CLAUDE.md §Build → `scripts/vs2026_build.ps1`; never grade in Debug): PPP fixture (`data_source=internal`, `run_smoother=true`, `enable_noncausal_slip_bridge=true`) on an arc with a known slip — a clean non-pivot reconverged boundary gets bridged (sat removed from `reset_satellites`, `x_pred` shifted by `dN·λ`), a pivot-handover boundary stays segmented (gate auto-fail); diff `smoother_trace.csv` position continuity vs bridge-off. **Gate-teeth check:** tighten `pre_bridge_float_tol_cycles` to `0.05` ⇒ marginal bridges rejected; loosen ⇒ they pass, proving the *independent-float* gate (not the smoothed float) drives acceptance. **Non-regression:** both toggles OFF ⇒ byte-identical traces against a same-build baseline.

**Grading recipe for (B), item iii** — do not run the sweep blind. Measure three quantities per epoch against the forward pass: **`num_fixed` vs forward**, the **ratio distribution**, and the **`ratio_gate="sentinel"` row fraction** (the WL/NL paths where the stricter-ratio guarantee degrades to non-regression, §11). Only then sweep `smoothed_ar_min_ratio` to replace the 0.5 placeholder.

## 14. Source anchors — current, and the drift table

**All three source docs' `file:line` anchors were written against a ~1,950-line `Application.cpp`; it is now 2,307 lines. Do not copy an old anchor forward without checking it here.**

| What | Doc said | ACTUAL (verified 2026-08-14) |
|---|---|---|
| F1 repair fold | `Application.cpp:1751-1763` | **`:1947-1959`**; reset keys deliberately segmented at old line 1758 (`Application.cpp:1758`), merged into the segment set at `1787-1788` → live `:1954` and `:1983` |
| `capturePosteriorSnapshot` call, and the AR-input cache hook on it | `Application.cpp:1797-1800`; hook `Application.cpp:1178-1184` | call **`:1996`**; definition `Application.cpp:1148-1197` → **`:1320`** (`push_back` `:1317`), hook lives in that body |
| Forward loop iterating `getObservations()` by index | `Application.cpp:1510-1518` | still in `run()`; not re-verified line-by-line |
| ATX WL gate on `wl_ambs`/`wl_fracs` | `Application.cpp:1838-1883` | **`:2029-2100`** |
| Smoother invocation + trace block; `smoother_buffer_` `std::move` | `Application.cpp:1910-1952`; `Application.cpp:1916` | **`:2134-2152`**; move at **`:2139`** |
| `Smoother.cpp` sites: ctor takes ownership `Smoother.cpp:71-72` · copy-and-adjust template `Smoother.cpp:140-162` + `:351-360` · boundary predicate `Smoother.cpp:320-358` · RTS step (G_k / LDLT) `Smoother.cpp:405` | as listed | ctor and RTS step unchanged; template live `:355-365`; predicate live `:326-340` (file is 438 lines) |
| `SmootherEpochSnapshot` struct / new-field site | `Smoother.hpp:37-128`, "~line 117 after `clean_handovers`" | struct still `:37`; `clean_handovers` **`:118`** |
| `repairAmbiguityBssdAware` | `CycleSlipManager.cpp:258-336` | pivot refusal at `:274-284`; not re-verified |
| AR config builder | `Application.cpp:1474-1490` | **`:1661`+** |
| `ar_ratio_threshold_` parse | `GnssConfigLoad.cpp` line 1031 | **`:1558`** |
| Toggles summary line | `GnssConfigLoad.cpp:1079-1082` | **`:3136`** (args `:3160`); the OLD site carries an explicit NOTE at **`:1641`** reading *"the consolidated 'Toggles: …' echo used to print HERE"* |
| RealTime force-off guard | `GnssConfigLoad.cpp:1895-1897` | **`:2588-2590`** |
| CMake slip block | `CMakeLists.txt:99-116` | **`:116-137`** (the two landed files at `:134-135`) |
| CMake `gnss_tests` block | `CMakeLists.txt:191-210` | **`:310`+** (93 test files; the two landed slip tests at `:396-397`) |
| `resetAmbiguity` · `appendState` · `addAmbiguity` | (unanchored) · `StateManager.cpp:868` · `StateManager.cpp:858-875` | **`StateManager.cpp:1108`** (zero row/col + `var=std²` at `:1152-1154`) · **`:114`** (decl `StateManager.hpp:1168` → **`:1363`**) · **`:993`** |
| frozen-pivot `P==0.0` | `AmbiguityResolver.cpp:351` | **`:314`** (`buildDdPairs` at **`:250`**; the pivot-skip body `AmbiguityResolver.cpp:349-362` and the field-read span `AmbiguityResolver.cpp:316-378` are Jun-2026 anchors, unverified) |

**⚠ CMakeLists.txt INDENTATION IS HETEROGENEOUS — a live trap for T4.** The `gnss_core` **`slip/` block (`:116-137`) is TAB-indented**; the **`app/` block (`:107-115`) is 4-space indented**. T4 adds `src/core/app/ApplicationBridge.cpp` to the *app* block, so it takes **spaces**, not the tabs of the neighbouring slip entries. There is **no globbing** — a forgotten `.cpp` surfaces as an undefined-reference link error, not a configure error. New **headers** (`JumpGapFitter.hpp`, `SlipBridgeKernel.hpp`) need no list entry (header-only-included), and `ApplicationBridge.cpp`'s methods are declared in `Application.hpp`, so they link into `gnss_core` without further wiring.

**Plausible but NOT re-verified line-by-line** (`Smoother.cpp` is 438 lines; spot checks agree within a few lines): `Smoother.cpp` 37-45, 78-134, 120-128, 295-302, 320-358, 351-360, 392-398. **NOT re-verified — treat as "as-of Jun-2026, unverified":** every `AmbiguityResolver.cpp` and `StateManager.cpp` anchor not listed in the table above. The 2026-08-15 re-grep of the four `StateManager.hpp` anchors found ALL FOUR stale (§9, §14) — assume the rest of the Jun-2026 set has drifted by a comparable amount. Provenance paths of the form `runs/<name>` in this lineage resolve under `D:\GnssProcessor_archive\2026-08-12\runs\`, not in the working tree.

## 15. Provenance: M-PAGES `fitGap`, and what must NOT be copied from it

`JumpGapFitter` is modeled on M-PAGES `fitGap` (`DbEdit.cpp:971-1033`) — a low-order LS polynomial-plus-step over symmetric pre/post windows. **DO NOT reuse the `{GF,IF}` inversion at `DbEdit.cpp:925-926` verbatim**: it is for `(ION_RESID, ION_FREE)`, a different pair, and transcribing it is how the `λ1`-in-numerator bug enters. The reusable 2×2 kernel in *our* tree is `TurboEditResolver.cpp:1310-1313`; the candidate-`k` loop form is `:1009-1014`; the accept-gate philosophy is `:214-222`. GF/MW series computers: `computeGfMeters` (`TurboEditResolver.cpp:170-190`) and `computeMwMeters` / `computeWideLaneWavelengthMeters` (`MwSlipDetector.cpp:344-363, 554-598`).

The broader architectural comparison — including *why* GnssProcessor structurally cannot match non-causal two-sided jump estimation today — is `docs/slip_comparison_report.md` §3.2 and §4 (the merge folded the old §5 "Where M-PAGES is stronger" into §4; §5 is now the file list).
