# Implementation Plan: Non-Causal Cycle-Slip Bridge + Post-Smoother AR Feedback

**Scope:** Post-processing pipeline only (`Application::run`, never `runRealTime` → `RealTimeEngine`). Two coupled capabilities, both default-off, decomposed smallest-viable-first. This plan incorporates the build/correctness review: every build blocker is resolved inline, the pre-bridge-float independence verdict is addressed head-on (the design is sound — it is *not* circular, and I state exactly why), and the sequencing/correctness fixes are applied. Honest limitations (static-PPP behavior, the missing AR-on config, coherent mis-bridge exposure) are called out where they bite.

---

## 1. Summary & sequencing

Capability **(A)** — the non-causal cycle-slip bridge — is the higher-value, fully-testable half and lands first. At each forward RESET boundary, the full past+future observation record is used to re-derive both-sides GF/MW windows, fit trend+step, and recover `(N1,N2)` from a from-scratch `{GF,MW}` algebraic kernel. The recovered integer is emitted **as a repair** (reuse the existing F1 fold at `Application.cpp:1751-1763`: add `dN·λ` to the boundary snapshot's `x_pred` and erase the sat from that snapshot's `reset_satellites`) **only if** an independent pre-bridge-float cross-check passes. Capability **(B)** — the post-smoother AR feedback pass — rebuilds a transient `StateManager` per smoothed epoch and runs a fresh `AmbiguityResolver` at a strictly higher ratio, emit-only to a CSV (no write-back). (B) is gated behind a HARD reconstitution test (T6) that must prove a transient SM built from the *forward* posterior reproduces forward AR exactly **before any smoothed value reaches AR**. Per the review, (B)'s end-to-end data validation is blocked on the user supplying an `ar_mode>0` PPP config (none exists in-tree today), so (B) ships default-off and numerically validated only.

| Phase | Tasks | Effort | Ships in PR |
|---|---|---|---|
| Kernels | T1 JumpGapFitter, T2 SlipBridgeKernel | M + L | PR-1 (with A) |
| Config | T3 toggles, T8 config files | M + S | PR-1 (A) / PR-2 (B keys) |
| Bridge wiring (A) | T4 retain floats + build/recover/gate + F1-fold emit | L | PR-1 |
| Smoothed-AR (B) | T5 transient SM, T6 HARD GATE, T7 smoothed AR pass | L + M + L | PR-2 (default-off) |

**Default-off:** all five new flags default false/0; both feature bools force-off in `AppMode::RealTime`. **Gate ordering:** T6 (reconstitution from forward posterior) blocks T7. The pre-bridge-float gate (Section 3) runs *before* the F1 fold, so post-smoother AR never sees an un-vetted bridge.

**Deferral recommendation (from review):** land A first (PR-1). B (T5–T7) is buildable and numerically testable but cannot be *data*-validated until an AR-on config exists; ship it default-off in PR-2 and treat the per-epoch trace machinery as provisional.

---

## 2. Task-by-task plan

### Phase: Kernels

#### T1 — JumpGapFitter: pure both-sides trend+step fitter
**Files:**
- `include/slip/JumpGapFitter.hpp` (NEW). Pure stateless fitter, heavy Doxygen (`@brief/@param/@return` on every public member — match the codebase norm):
  ```cpp
  struct JumpGapFitter {
    struct Window { std::vector<double> t_s; std::vector<double> y_m; };
    struct FitResult { bool ok{false}; double pre_level_m{0}; double post_level_m{0};
                       double step_m{0}; double pre_slope{0}; double post_slope{0};
                       double rms_resid_m{0}; int n_pre{0}; int n_post{0}; };
    int min_window_points{4};
    FitResult fitStep(const Window& pre, const Window& post, double t_boundary_s) const;
  };
  ```
- `src/core/slip/JumpGapFitter.cpp` (NEW). Independent LSQ `y=a+b·t` per side via 2×2 normal equations (guard `det`); evaluate both at `t_boundary_s`; `step_m = post_level − pre_level`; `rms_resid_m` over both sides; `ok=false` if either side `<min_window_points` or any non-finite. **Zero GNSS deps** — raw `(t,y)` only.
- `CMakeLists.txt`: add `src/core/slip/JumpGapFitter.cpp` to the gnss_core `slip/` block (lines 99-116, **TAB** indentation); add `tests/test_jump_gap_fitter.cpp` to `add_executable(gnss_tests ...)` (lines 191-210).

**Depends on:** none.
**Verify:** `test_jump_gap_fitter.cpp` — `y=2.0+0.1·t` with a `+0.862 m` step → `EXPECT_NEAR(step_m,0.862,1e-6)`, `EXPECT_NEAR(pre_slope,0.1,1e-9)`; `n_pre<4` → `ok=false`; differing pre/post slopes recovered independently. Build gnss_core target first to catch a missing CMake entry early.

#### T2 — SlipBridgeKernel: window builder + from-scratch {GF,MW}→integer recovery
**Files:**
- `include/slip/SlipBridgeKernel.hpp` (NEW). Holds a `PppUtils&` (FDMA inherited from the borrowed instance — never recompute FCN). Heavy Doxygen.
  ```cpp
  struct RecoveredSlip { bool ok{false}; int dN1{0}; int dN2{0}; int dN_wl{0};
                         double frac_resid_cyc{0}; double gf_step_m{0}; double mw_step_m{0};
                         std::string sig1; std::string sig2; };
  struct BridgeWindows { JumpGapFitter::Window gf_pre, gf_post, mw_pre, mw_post;
                         double t_boundary_s{0}; };
  BridgeWindows buildWindows(const std::vector<RinexObservation>& all_obs,
      const std::string& sat, const std::string& sig1, const std::string& sig2,
      const std::string& code1, const std::string& code2,
      double t_boundary_s, int half_window_epochs) const;
  RecoveredSlip recover(const BridgeWindows& w, const std::string& sat, System system,
      const std::string& sig1, const std::string& sig2, const JumpGapFitter& fitter) const;
  ```
- `src/core/slip/SlipBridgeKernel.cpp` (NEW).
  - **`getObservations()` returns a FLAT `std::vector<RinexObservation>`** (one sat per element per epoch, *not* grouped) — `buildWindows` must filter by `.satellite` AND split by `.time` itself, partitioning pre(`<t_boundary`)/post(`>=t_boundary`) within `±half_window_epochs`.
  - **`RinexObservation` field is `.measurements`** (RinexParser.hpp:38-50), NOT `.raw_observed` (that belongs to `PppObservation`). The findings' GF/MW arithmetic reads `raw_observed` — the kernel must remap to `.measurements`. `L*` values are cycles, `C*` values are metres.
  - Per-epoch GF (metres): `GF = L1_cyc·lam1 − L2_cyc·lam2`, `lam = ppp_utils_.getWavelength(system, sig, sat)`.
  - Per-epoch MW (metres): `MW = (f1·L1m − f2·L2m)/(f1−f2) − (f1·P1 + f2·P2)/(f1+f2)`, `f = ppp_utils_.getFrequency(sig, system, sat)`, `Lm = Lcyc·c/f`. **Mirror the ASYMMETRIC arg order EXACTLY:** `getFrequency(sig, sys, sat)` vs `getWavelength(sys, sig, sat)` — the #1 bug source.
  - `recover`: fit `gf`+`mw` via the `JumpGapFitter`; `lambda_wl = c/|f1−f2|`; **re-derive the two linear relations, do NOT transcribe the `td2/denom` substitution** (the `lambda1`-in-numerator asymmetry is the documented easy bug):
    - constraint GF: `lambda1·dN1 − lambda2·dN2 = gf_step_m`
    - constraint MW: `dN1 − dN2 = dN_wl`
    - solve: `dN_wl = round(mw_step_m/lambda_wl)`; `dN2 = round((gf_step_m − lambda1·dN_wl)/(lambda1−lambda2))`; `dN1 = dN2 + dN_wl`; `frac_resid_cyc = |((gf_step_m − lambda1·dN_wl)/(lambda1−lambda2)) − dN2|`.
  - **Enforce `sig1 = higher frequency`** (assert `getFrequency(sig1)>getFrequency(sig2)` or swap) — a wrong-order pair flips the WL-integer sign on E5/E7 etc. and silently corrupts attribution.
  - Guards: `|lambda1−lambda2|<1e-6` → `ok=false` (degenerate same-band); GLONASS sat with unset slot returns `freq 0.0` → `ok=false` (safe-fail). Acceptance: `frac_resid_cyc<=0.30`, `|dN1|,|dN2|<=1000`, not both zero.
- `CMakeLists.txt`: add `src/core/slip/SlipBridgeKernel.cpp` to the `slip/` block; add `tests/test_slip_bridge_kernel.cpp` to gnss_tests.

**Depends on:** T1.
**Verify:** `test_slip_bridge_kernel.cpp` — synth GPS sat (L1C/L2W/C1W/C2W) with injected `dN1=+2, dN2=+1` (`dN_wl=+1`) → `EXPECT_EQ` recovery; `lambda_wl≈0.862 m`; wrong-order `sig1=lo` → swap/assert path; degenerate same-band → `ok=false`; GLONASS path with `setGlonassSlots` applied to the borrowed PppUtils (FDMA-correct).

### Phase: Config

#### T3 — Config toggles (4-touch-point pattern + ctor init)
Five new members. **Do not forget the ctor member-init (the easily-missed "fifth placement").**
**Files:**
- `include/config/GnssConfig.hpp`: private members + inline Doxygen getters (full `@return`, both bools noted post-processing-only):
  `bool enable_noncausal_slip_bridge_;` `bool enable_smoothed_ar_pass_;` `double smoothed_ar_min_ratio_;` `double pre_bridge_float_tol_cycles_;` `double pre_bridge_conv_var_m2_;` `int slip_bridge_half_window_epochs_;` → getters `enableNoncausalSlipBridge()`, `enableSmoothedArPass()`, `getSmoothedArMinRatio()`, `getPreBridgeFloatTolCycles()`, `getPreBridgeConvVarM2()`, `getSlipBridgeHalfWindowEpochs()`.
- `src/core/config/GnssConfig.cpp` (ctor member-init near `run_smoother_{false}` at :125): `, enable_noncausal_slip_bridge_{false}, enable_smoothed_ar_pass_{false}, smoothed_ar_min_ratio_{0.0}, pre_bridge_float_tol_cycles_{0.5}, pre_bridge_conv_var_m2_{0.0025}, slip_bridge_half_window_epochs_{30}`.
- `src/core/config/GnssConfigLoad.cpp`:
  - `loadProcessingSection_` (helper at :136): parse the bridge keys (see Section 4 snippets).
  - `loadEstimationSection_`: parse `enable_smoothed_ar_pass_`; then parse `smoothed_ar_min_ratio_` **after** `ar_ratio_threshold_` (which is parsed at **line 1031** in this helper — place the new parse+clamp *after* 1031), defaulting to `ar_ratio_threshold_+0.5`, with clamp-up-and-warn if `<= ar_ratio_threshold_`.
  - Summary Toggles `log->info` line (:1079-1082): add two `{}` slots `nc_bridge={}, smoothed_ar={}` with matching `? "on":"off"` args.
  - `loadAppModeAndRealtimeSection_` (:1883, mirror the `run_smoother` guard at :1895-1897): force both bools false in `AppMode::RealTime` with a warn.
- **Touch point (4) `isLoggerEnabled` is N/A** — the smoothed-AR pass reuses the existing `"smoother"` logger; no new named logger. Document this in the PR.

**Depends on:** none.
**Verify:** config-load gtest — defaults off; `smoothed_ar_min_ratio` resolves to `forward+0.5` when unset; a JSON value `<= ar_ratio_threshold_` clamps up with `smoothed_ar_min_ratio_ > ar_ratio_threshold_`; RealTime app_mode forces both off.

### Phase: Bridge wiring (A)

#### T4 — Wire the non-causal bridge into Phase-5 prep
**Files:**
- `include/app/Application.hpp`:
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
  (Doxygen on these to match the norm.)
- `src/core/app/Application.cpp`:
  - **(a)** At the reset-merge site (~:1787, after `epoch_reset_sats` finalized, BEFORE `capturePosteriorSnapshot` at :1800) call `retainBoundaryFloats(epoch_reset_sats, prior_converged_snapshot_index)`. Capture `pre_slip_float_m` from the PRIOR snapshot's `x_post(idx)` (INDEPENDENT FLOAT #1); set `pivot_changed` via a `pivot_satellites` delta between `k-1` and `k`. Record `idx_at_reset` AND `snapshot_index`.
  - **(b)** Immediately after `capturePosteriorSnapshot` (~:1800) call `updatePostResetConvergedFloats()`: for records not yet `post_converged`, if current `P(idx,idx) < pre_bridge_conv_var_m2` record `x_post(idx)` as `post_reset_converged_float_m` (INDEPENDENT FLOAT #2) and set `post_converged`.
  - **(c)** NEW Phase-5 prep inside `if(runSmoother() && !smoother_buffer_.empty())` **BEFORE** the `Smoother` construction at :1916: `if(config_.enableNoncausalSlipBridge()) applyNoncausalSlipBridge();`
- `src/core/app/ApplicationBridge.cpp` (NEW). Implements all three helpers.
  - `applyNoncausalSlipBridge`: per boundary record, build windows from `rinex_parser_->getObservations()`, derive `sig1/sig2/code1/code2` for the reset key (signal from `snapshot.ambiguities` keys whose sat prefix matches), `SlipBridgeKernel::buildWindows`+`recover`, run the pre-bridge-float gate (Section 3). On PASS: **resolve the index against `snapshot[record.snapshot_index]`'s OWN ambiguity map, NOT the live StateManager** (the live SM is post-loop and may have grown; the F1 fold at :1759 uses the live map only because it runs at forward-loop time). Then `snapshot[idx_local].x_pred(idx_local) += dN_sig·lambda_sig` AND `snapshot[record.snapshot_index].reset_satellites.erase(sat)`. On FAIL: leave segmented; log bridge diagnostics to the `"smoother"` logger.
- `CMakeLists.txt`: add `src/core/app/ApplicationBridge.cpp` to the gnss_core `app/` block (alongside `ApplicationBaseline.cpp`/`ApplicationRealtime.cpp`); declarations live in `Application.hpp` so they link.

**Build-blocker / sequencing fixes applied:**
- Index resolution uses the **snapshot-local** map (review sequencing fix), not `state_manager_->getAmbiguityIndex` post-loop.
- `applyNoncausalSlipBridge` runs **after** the whole forward pass, so the retained floats are provably bridge-free (independence — Section 3).
- Confirm `rinex_parser_`/observation record is retained past the forward loop (review risk). If not, add retention; cost ≈ one full obs record.

**Depends on:** T1, T2, T3.
**Verify:** Full build via vcvars64. PPP fixture (`data_source=internal`, `run_smoother=true`, `enable_noncausal_slip_bridge=true`) on an arc with a known slip: a clean (non-pivot) reconverged boundary gets bridged (sat removed from `reset_satellites`, `x_pred` shifted by `dN·λ`); a pivot-handover boundary stays segmented (gate auto-fail). Diff `smoother_trace.csv` position continuity across the bridged boundary vs bridge-off.

### Phase: Smoothed-AR (B)

#### T5 — Transient StateManager reconstitution helper
**Build blockers from review resolved here.**
**Files:**
- `include/app/Application.hpp`:
  ```cpp
  std::unique_ptr<StateManager> buildTransientStateManager(
      const Eigen::VectorXd& x, const Eigen::MatrixXd& P,
      const std::map<std::string,size_t>& ambiguities,
      const std::map<std::string,size_t>& ionospheres,
      const std::map<std::string,size_t>& code_biases,
      const std::map<std::string,size_t>& sat_biases,
      const std::map<std::string,std::string>& pivot_satellites) const;
  ```
  Takes RAW `(x,P,maps)`, NOT smoother types. **Note: NO `sys_offsets` parameter** — see blocker (1).
- `src/core/app/ApplicationBridge.cpp` — implement:
  - **BLOCKER (1) FIX — no system-time-offset adder exists.** `StateManager` exposes only `getSystemTimeOffsetIndex`/`getSystemsWithTimeOffsets`/`getNumSystemTimeOffsets` (verified: StateManager.hpp:444/467/477) — there is **no public `addSystemTimeOffset`**. ISB/ISBR slots are allocated *internally during construction from config* (StateManager.cpp:307-333), contiguous in core BEFORE any dynamic key. Because the transient SM is constructed with the **same `config_`**, the ctor reproduces the ISB layout automatically. The helper therefore **must NOT register sys_offsets at all** — it only re-adds ambiguities/iono/code_biases/sat_biases. (Registering a sys-offset adder, as the original plan said, does not compile.)
  - **BLOCKER (2) FIX — interleaved index order.** Dynamic states are appended via `appendState` (StateManager.cpp:868, verified at hpp:1168) monotonically in **first-creation order across MULTIPLE categories** — a sat's iono and ambiguity interleave globally with other sats' states. Per-category sorted registration (all ambiguities, then all iono, …) yields a different global append order and the index-equality assertion fires on essentially every real run. **FIX:** merge ALL FOUR dynamic maps into a SINGLE ascending-index sequence (sort the union of `(index, key, category)` tuples by index) and dispatch each key to its category adder — `addAmbiguity(key,0.0)` / `addIonosphereState(...)` / `addCodeBiasState(key,0.0)` / `addSatBiasState(key,0.0)` — in that one global order. ASSERT returned index == source index per key (hard fail on mismatch — keep it as a guard, not best-effort).
  - Then `setStateVector(x)`; **then re-impose frozen-pivot `var==0` before `setCovariance`** — see blocker (3).
  - **BLOCKER (3) FIX — pivot key reconstruction.** `pivot_satellites` maps constellation→sat-ID, **not** to an amb_key (verified: PivotManager / `getBssdPivotSatellites`). AR detects a frozen pivot by `P(idx,idx)==0.0` EXACTLY (AmbiguityResolver.cpp:351). Smoothed `P` has nonzero pivot variance. The helper must, for each pivot sat, reconstruct the pivot amb_key(s) `sat + "_" + signal` for **every active signal/band of that pivot sat** present in the ambiguity map, and zero each diagonal in a local copy of `P` before `setCovariance` (no StateManager API change). Zeroing only one band while the pivot carries multiple lets AR silently stop excluding the other-band pivot.

**Depends on:** T3.
**Verify:** `test_transient_state_manager.cpp` — build a SM with interleaved ambiguities/iono/pivot, read `(x,P,maps)` via the `capturePosteriorSnapshot` getters, feed `buildTransientStateManager`, `EXPECT_TRUE x2.isApprox(x)`, covariance `isApprox`, `getAmbiguities()==source`, pivot diagonal(s) `== 0.0` exactly. Round-trips via PUBLIC getters/setters → no friend-shim needed.

#### T6 — HARD GATE: faithful reconstitution from FORWARD posterior reproduces forward AR
**Files:**
- `src/core/app/ApplicationBridge.cpp`: runtime self-check (gated behind `enable_smoothed_ar_pass_`, runs ONCE on the first AR-eligible forward epoch's captured snapshot). Rebuild a transient SM from the FORWARD `(x_post,P_post,maps)`, run a fresh `AmbiguityResolver` (forward `ar_config`, **`min_epochs=0`** — a fresh resolver restarts `epoch_count_` at 0) over the SAME `modeled_obs`, assert `num_fixed`/`ratio`/`fixed_state` match the forward AR result within tol. On mismatch: hard-error and **abort the smoothed AR pass** (refuse to let smoothed values flow to AR). Log PASS/FAIL to `"smoother"`.
- `tests/test_smoothed_ar_reconstitution.cpp` (NEW). Offline analogue: a fixture epoch where forward AR fixes N ambiguities; serialize `(x,P,maps,obs)`; rebuild transient SM; run AR; `EXPECT_EQ num_fixed`, `EXPECT_NEAR ratio`, `EXPECT_TRUE fixed_state.isApprox(forward_fixed_state)`. `GTEST_SKIP` if no AR fixture (honest — no AR-on data config ships).
- `CMakeLists.txt`: add `tests/test_smoothed_ar_reconstitution.cpp` to gnss_tests.

**Depends on:** T5.
**Verify:** gtest PASSes on a synthetic AR-fixable epoch (or SKIPs honestly). Deliberately perturb the transient build (skip pivot zeroing) and confirm it FAILS — proving the gate has teeth. **Honest caveat (review):** with all shipped configs at `ar_mode=0`, there are zero AR-eligible epochs, so the *runtime* self-check never fires on real data today — the "teeth on real data" claim is vacuous until an `ar_mode>0` config exists.

#### T7 — Post-smoother AR feedback pass (emit-only, NO write-back)
**Files:**
- `include/app/Application.hpp`: members to retain per-epoch `(modeled_obs, wl_ambs, wl_fracs, snapshot maps, pivots)` **copied BEFORE the `smoother_buffer_` move at :1916** (a second consumption of the moved-from buffer is empty). Document the memory cost. Declare `void runSmoothedArPass(const std::vector<Smoother::SmoothedEpoch>& results);`.
- `src/core/app/ApplicationBridge.cpp`: implement `runSmoothedArPass`.
  - Build an `ar_config` like the forward builder (Application.cpp:1474-1490) but `min_epochs=0` and `ratio_threshold=config_.getSmoothedArMinRatio()`; construct a FRESH `AmbiguityResolver(ar_config, log_manager_)`.
  - Per smoothed epoch `i`: `buildTransientStateManager` from `results[i].(x,P)` + retained maps/pivots (results are index-aligned to input snapshots; **`SmoothedEpoch` does NOT carry the dynamic maps** — source them from the matched `SmootherEpochSnapshot`). Replicate the call-site ATX gating on `wl_ambs`/`wl_fracs` (lives in `Application.cpp`, NOT the resolver) using the retained per-epoch wl maps. `resolver.resolve(transient_sm, modeled_obs_i, wl_ambs_i, wl_fracs_i)`.
  - **CORRECTNESS GAP (review) — the `+inf` ratio sentinel.** `wl_only` and `NlWl` set `ratio=+inf` (AmbiguityResolver.hpp:35-42). `ratio > smoothed_ar_min_ratio` is trivially true for `+inf`, so the stricter-ratio gate is **meaningless for exactly the WL/NL basis that is the shipped default**. Explicit policy: for `+inf`-sentinel results, fall back to the `success` flag **and** a finite secondary criterion — accept only if `success && resolution.num_fixed >= forward_num_fixed_at_epoch_i` (retain forward per-epoch `num_fixed`), and **stamp the trace row `ratio_gate="sentinel"`** so the user knows the "strictly-stronger AR" guarantee does NOT hold on the WL/NL paths. State this limitation in the PR — for PerBand-with-LAMBDA the ratio gate is real; for wl_only/NlWl it degrades to a non-regression check.
  - On accept: EMIT a row to `smoother_fixed_trace.csv` (`week,tow,sat,signal,fixed_int,float_m,ratio,ratio_gate,num_fixed,num_candidates,smoothed`). **NO `setStateVector`/`holdAmbiguities`; NO feedback into `smoother_buffer_`.**
- `src/core/app/Application.cpp`: in Phase-5 AFTER `smoother.run()` and the existing smoother_trace write (~:1951): `if(config_.enableSmoothedArPass()) runSmoothedArPass(smoother.results());` (already inside the `runSmoother()` guard; RealTime forced off by T3).
- `docs/report/main.tex`: short subsection documenting the non-causal bridge + post-smoother AR math (keep the LaTeX math reference in sync). **Flagged optional / deferrable** until an AR-on config exists (review: don't build untestable trace+docs machinery early).

**Depends on:** T4, T6.
**Verify:** Full build. PPP fixture `run_smoother+enable_smoothed_ar_pass` — `smoother_fixed_trace.csv` written; forward solution traces **byte-identical** with/without the pass (no write-back); RealTime config never triggers it. With current `ar_mode=0` configs the pass runs but AR yields 0 fixes (honest).

### Phase: Config files

#### T8 — Ship default-off configs + document the AR-on gap
**Files:**
- `config/ppp_internal.json` and `src/core/config/gnss_config.json`: add all five keys default-OFF for discoverability (`processing.enable_noncausal_slip_bridge=false`, `processing.slip_bridge_half_window_epochs=30`, `processing.pre_bridge_float_tol_cycles=0.5`, `processing.pre_bridge_conv_var_m2=0.0025`; `estimation.enable_smoothed_ar_pass=false`, `estimation.smoothed_ar_min_ratio=<forward+0.5>`). Inline note: `smoothed_ar_min_ratio` must exceed `ar.ratio_threshold`. **Does NOT fabricate an unvalidated `ar_mode>0` config** — that is a user deliverable (Section 7).

**Depends on:** T3.
**Verify:** load each edited config through `GnssConfig::loadFromJson` — no parse errors, all toggles OFF, `smoothed_ar_min_ratio > ar_ratio_threshold` invariant holds.

---

## 3. The pre-bridge-float cross-check (independence verdict addressed)

**The review's verdict: the design is sound and NOT circular.** I confirm it and restate the load-bearing reasons, then specify the gate.

**Why a naive backstop WOULD be circular:** the smoothed float across the boundary already contains the bridge jump (the F1 fold mutated `x_pred`, which the backward recursion propagated). Comparing recovered `dN` to the *smoothed* float step is self-confirming and useless. We never do that.

**The two genuinely independent floats — both captured DURING the forward loop, before the bridge fold:**

- **INDEPENDENT FLOAT #1 (pre-slip converged arc):** the forward Kalman posterior float for that sat key in the epoch *immediately before* the reset — `smoother_buffer_[k-1].x_post(idx)` (the prior snapshot's converged value, SD frame). Mature, converged, pre-slip.
- **INDEPENDENT FLOAT #2 (independently re-converged post-reset arc):** the **geometry-reseeded** value the reset path produced. A reset does not retain the pre-slip value — `resetAmbiguity` zeroes covariance + marks pending; `MeasurementModel::ensureRequiredStates` recomputes the seed via `computeAmbiguitySeed` = `observed − modeled − clk − tropo − isb` (MeasurementModel.cpp:396-443), BSSD subtracting the pivot same-signal seed. We retain `post_reset_converged_float_m` LAZILY: the first forward epoch where post-reset `P(idx,idx) < pre_bridge_conv_var_m2` (default `(0.05 m)²`), recording `x_post(idx)`.

**Two-axis independence:**
1. *Bridge-independence:* `applyNoncausalSlipBridge` (the F1 fold) runs at Phase-5 prep, AFTER the entire forward pass. Both floats were captured during the forward loop. Neither ever saw the bridge mutation. ✔
2. *Algebra-independence:* FLOAT #2 is a phase-**geometry**/clock/tropo solution (`computeAmbiguitySeed`), entirely separate from the GF/MW combination algebra that yields `dN`. The gate cross-checks a phase-geometry float-step against a GF/MW integer-step — two independent observables. ✔

**The gate (in `applyNoncausalSlipBridge`, BEFORE the F1 fold):**
```
float_step_m     = post_reset_converged_float_m − pre_slip_float_m   // same SD frame, same signal
recovered_step_m = dN_sig · lambda_sig                                // L1→dN1·λ1, L2→dN2·λ2
PASS  iff  post_converged == true
      AND  |recovered_step_m − float_step_m| <= pre_bridge_float_tol_cycles · lambda_sig
```
Both signals of the pair are checked (or the WL projection: `(dN1−dN2)` vs the float step projected onto WL). If any signal fails, the bridge is NOT emitted; the sat stays in `reset_satellites` (segmented exactly as today).

**FRAME GUARD (fail-closed):** if `pivot_changed` (the boundary involved a BSSD pivot handover for that sat — `pivot_satellites`/`clean_handovers` differ between `k-1` and `k`), the two floats are in different SD frames and not comparable → auto-FAIL. This mirrors the findings' rule that post-handover repairs are recorded as resets, not repairs. **Caveat (review):** this frame guard is the *only* protection against a frame-mismatched bridge, so the `pivot_changed` detection must be complete and fail-closed.

**Documented limitations:**
- A sat that never re-converges below `pre_bridge_conv_var_m2` within the buffer keeps `post_converged=false` → fail-closed (stays segmented). Short arcs after a slip are never bridged — acceptable, documented.
- **Coherent mis-bridge residual exposure:** if the GF/MW algebra and the geometry float *both* mis-attribute by the same integer (a coherent, physically-plausible wrong jump), the gate can pass. The gate bounds the error to integer-consistent attributions; it does not eliminate a globally-coherent mis-bridge. This is the irreducible residual risk and is why (A) ships default-off with trace inspection required.

---

## 4. Config (verbatim 4-touch-point snippets)

**`loadProcessingSection_` (GnssConfigLoad.cpp):**
```cpp
enable_noncausal_slip_bridge_ = jval<bool>(json, "processing.enable_noncausal_slip_bridge", "enable_noncausal_slip_bridge", false);
slip_bridge_half_window_epochs_ = jval<int>(json, "processing.slip_bridge_half_window_epochs", "slip_bridge_half_window_epochs", 30);
pre_bridge_float_tol_cycles_ = jval<double>(json, "processing.pre_bridge_float_tol_cycles", "pre_bridge_float_tol_cycles", 0.5);
pre_bridge_conv_var_m2_ = jval<double>(json, "processing.pre_bridge_conv_var_m2", "pre_bridge_conv_var_m2", 0.0025);  // (0.05 m)^2 post-reset float-convergence gate
```

**`loadEstimationSection_` — parse AFTER `ar_ratio_threshold_` (line 1031):**
```cpp
enable_smoothed_ar_pass_ = jval<bool>(json, "estimation.enable_smoothed_ar_pass", "enable_smoothed_ar_pass", false);
smoothed_ar_min_ratio_ = jval<double>(json, "estimation.smoothed_ar_min_ratio", "smoothed_ar_min_ratio", ar_ratio_threshold_ + 0.5);
if (smoothed_ar_min_ratio_ <= ar_ratio_threshold_) {
    log->warn("smoothed_ar_min_ratio ({:.2f}) must exceed forward ratio_threshold ({:.2f}); clamping to {:.2f}",
              smoothed_ar_min_ratio_, ar_ratio_threshold_, ar_ratio_threshold_ + 0.5);
    smoothed_ar_min_ratio_ = ar_ratio_threshold_ + 0.5;
}
```

**Summary Toggles line (GnssConfigLoad.cpp:1079-1082) — add two `{}` slots + args:**
```cpp
//   ..., nc_bridge={}, smoothed_ar={}
//   enable_noncausal_slip_bridge_ ? "on" : "off", enable_smoothed_ar_pass_ ? "on" : "off"
```

**`loadAppModeAndRealtimeSection_` (mirror :1895-1897):**
```cpp
if ((enable_noncausal_slip_bridge_ || enable_smoothed_ar_pass_) && app_mode_ == AppMode::RealTime) {
    enable_noncausal_slip_bridge_ = false;
    enable_smoothed_ar_pass_ = false;
    log->warn("non-causal slip bridge / smoothed-AR pass ignored in real-time mode (post-processing only)");
}
```

**Defaults:** `smoothed_ar_min_ratio = forward ratio_threshold + 0.5` — a placeholder margin **flagged for a data sweep**; the empirical value depends on the (currently nonexistent) AR-on config, so 0.5 is NOT final. Touch point (4) `isLoggerEnabled` is N/A (reuses `"smoother"`).

---

## 5. Build wiring (CMakeLists.txt)

- **gnss_core `slip/` block (lines 99-116, TAB indentation):** add `src/core/slip/JumpGapFitter.cpp` and `src/core/slip/SlipBridgeKernel.cpp`.
- **gnss_core `app/` block (alongside `ApplicationBaseline.cpp`/`ApplicationRealtime.cpp`):** add `src/core/app/ApplicationBridge.cpp`.
- **`add_executable(gnss_tests ...)` (lines 191-210):** add `tests/test_jump_gap_fitter.cpp`, `tests/test_slip_bridge_kernel.cpp`, `tests/test_transient_state_manager.cpp`, `tests/test_smoothed_ar_reconstitution.cpp`.
- No globbing — a forgotten `.cpp` surfaces as an undefined-reference link error. New headers (`JumpGapFitter.hpp`, `SlipBridgeKernel.hpp`) are header-only-included, no list entry needed; `ApplicationBridge.cpp` methods are declared in `Application.hpp` so they link into gnss_core.

---

## 6. Tests (GoogleTest; `#include <gtest/gtest.h>`, `TEST`/`TEST_F`, `EXPECT_*`/`GTEST_SKIP`, no `main()`)

- **`test_jump_gap_fitter.cpp`** — linear-with-step recovery to `1e-6`; `n<min_window_points` → `ok=false`; independent pre/post slopes.
- **`test_slip_bridge_kernel.cpp`** — GPS L1C/L2W/C1W/C2W with injected `(dN1,dN2)` → `EXPECT_EQ`; `lambda_wl≈0.862 m`; wrong-order `sig1<sig2` → swap/assert; degenerate same-band → `ok=false`; GLONASS path with `setGlonassSlots` on the borrowed PppUtils. **The integer-injection cases are the only guard against the asymmetric-arg / `lambda1`-in-numerator bug — keep them.**
- **`test_transient_state_manager.cpp`** — round-trip through getters→`buildTransientStateManager`→getters; `x`/`P` `isApprox`; maps equal; pivot diagonal(s) `== 0.0`. **Crucially include an INTERLEAVED-index fixture** (sat A amb, sat B amb, sat A iono, …) so the single-global-order registration (blocker 2) is exercised.
- **`test_smoothed_ar_reconstitution.cpp` (HARD GATE analogue)** — forward-AR-fixable epoch → rebuild transient SM from FORWARD posterior → fresh AR (`min_epochs=0`) reproduces `num_fixed`/`ratio`/`fixed_state`; `GTEST_SKIP` if no AR fixture.
- **Config-load test** (extend `test_estimation_core.cpp` or new) — defaults off; `smoothed_ar_min_ratio = forward+0.5` + clamp-up-with-warn; RealTime forces both off.
- **Runtime hard-gate self-check** (T6, in `ApplicationBridge.cpp`) — first AR-eligible epoch reconstitution reproduces forward AR or aborts; logs PASS/FAIL. Private kernels reached via PUBLIC getters/setters → **no friend-shim needed** (the `XxxTestAccess` seam is reserved for genuinely private numeric kernels).

---

## 7. Validation on real data

- **Build cadence:** after each kernel task, build gnss_core only:
  `cmd /c '"<vcvars64>" >nul && cmake --build C:\GnssProcessor\build\x64-Debug --target gnss_core 2>&1'` (catch a forgotten CMake entry early). Full build (core+processor+tests) after T4 and T7.
- **Bridge data run (A):** PPP fixture (`data_source=internal`, `run_smoother=true`, `enable_noncausal_slip_bridge=true`) on an arc with a known cycle slip. Inspect bridge diagnostics; diff `smoother_trace.csv` position continuity across the bridged boundary vs bridge-off. Confirm a pivot-handover boundary stays segmented (gate auto-fail).
- **Pre-bridge-float gate teeth:** set `pre_bridge_float_tol_cycles` tight (`0.05`) → marginal bridges rejected (stay in `reset_satellites`); loosen → they pass. This proves the **independent-float** gate (not the smoothed float) drives acceptance.
- **THE MISSING AR-ON CONFIG (must be created first for B):** no data-validated `ar_mode>0` PPP config exists in-tree (shipped AR configs are `ar_mode=0`, per memory `project_iono_ar_review_jun2026`). The user must produce/validate an `ar_mode>0` PPP config before (B) can be exercised end-to-end. Until then, (B) is validated only via the kernel + reconstitution gtests.
- **Smoothed AR pass (B), once an AR-on config exists:** `enable_smoothed_ar_pass=true` → `smoother_fixed_trace.csv` emitted; forward traces byte-identical with/without the pass (no write-back). Measure: per-epoch `num_fixed` vs forward, ratio distribution, `ratio_gate="sentinel"` row fraction (the WL/NL paths where the stricter-ratio guarantee degrades). Then run a sweep on `smoothed_ar_min_ratio` to replace the 0.5 placeholder.
- **Non-regression:** run standard PPP/PPP-RTK fixtures with both toggles OFF → output traces byte-identical to a pre-change baseline (features inert when off). Confirm RealTime config never triggers either (warn logged, getters off).

---

## 8. Risks & deferred-out-of-v1

- **No AR-on data config exists** → (B)'s T7 pass and T6 runtime self-check cannot be end-to-end data-validated today. Ships default-off, numerically validated only. **Recommendation:** defer T7's full per-epoch trace machinery and the `docs/main.tex` subsection until a real AR-on config exists — building them now buys an untestable feature. Land **(A) first** (PR-1); (A) is the higher-value, fully-testable half.
- **`+inf` ratio sentinel** makes `smoothed_ar_min_ratio` vacuous for the shipped `wl_only`/`NlWl` basis — the "strictly stronger AR" headline does NOT hold there; the fallback (`success && num_fixed >= forward`) is a non-regression check, stamped `ratio_gate="sentinel"`. Honest scope limit.
- **Static-PPP behavior (honest no-op nuance):** smoothed *position* is degenerate (constant across epochs by construction — CLAUDE.md Smoother note), so (B) is **not** a pure no-op — smoothed *ambiguity* floats DO vary and can re-fix — but `smoother_fixed_trace.csv` will show position-invariant rows with epoch-varying ambiguity fixes. Document so the user does not read per-epoch "fixes" as independent position solutions.
- **Coherent mis-bridge residual exposure** (Section 3): the pre-bridge-float gate bounds error to integer-consistent attributions but cannot catch a globally-coherent wrong jump shared by both the GF/MW algebra and the geometry float. Irreducible; mitigated by default-off + trace inspection.
- **`smoother_buffer_` move (`Application.cpp:1916`):** retained per-epoch maps/pivots/obs/wl maps for T7 MUST be copied out **before** the move (a second read of the moved-from buffer is empty — silent zero-fix, not a build error). Wire the copy at the pre-move site, not in `runSmoothedArPass`.
- **`rinex_parser_` retention:** T4 needs the full obs record at Phase-5; confirm the parser/observations survive past the forward loop or add retention (~one obs record).
- **Frame-guard completeness:** the `pivot_changed` auto-fail is the only protection against a frame-mismatched bridge — must be fail-closed and detect every handover from `pivot_satellites`/`clean_handovers` deltas.
- **`smoothed_ar_min_ratio = forward+0.5` is a placeholder** flagged for a data sweep; the empirical value depends on the nonexistent AR-on config — do not treat 0.5 as final.

**Deferred out of v1:** any write-back of smoothed/fixed state into the smoother buffer (locked out — emit-only); real-time variants (post-processing only); `docs/main.tex` math subsection (defer with T7); staging more than the WL/NL + PerBand AR paths the forward builder already supports.

**Relevant files (absolute):** `C:\GnssProcessor\src\core\app\Application.cpp` (F1 fold :1751-1763, posterior capture :1800, smoother move :1916, AR builder :1474-1490), `C:\GnssProcessor\include\filter\StateManager.hpp` (no `addSystemTimeOffset`; adders at :276/:510/:616/:658; `appendState` :1168), `C:\GnssProcessor\src\core\filter\AmbiguityResolver.cpp` (frozen-pivot `P==0.0` :351), `C:\GnssProcessor\src\core\config\GnssConfigLoad.cpp` (`ar_ratio_threshold_` :1031, Toggles :1079-1082, RT guard :1895-1897), `C:\GnssProcessor\CMakeLists.txt` (slip 99-116, gnss_tests 191-210). NEW: `include/slip/JumpGapFitter.hpp`, `src/core/slip/JumpGapFitter.cpp`, `include/slip/SlipBridgeKernel.hpp`, `src/core/slip/SlipBridgeKernel.cpp`, `src/core/app/ApplicationBridge.cpp`, four `tests/test_*.cpp`.