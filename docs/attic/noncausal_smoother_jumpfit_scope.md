# SCOPE: Non-Causal Cycle-Slip Jump Re-Estimation Pass for the Post-Processing RTS Smoother

## 1. Goal & value

Add a **pre-smoother, non-causal both-sides jump re-estimation pass** that, at each forward-flagged ambiguity-reset boundary in `smoother_buffer_`, fits a shared low-order polynomial-plus-step model across a pre-window and post-window of geometry-free (GF) and Melbourne-Wübbena (MW) series re-derived from the still-resident raw RINEX, recovers a per-frequency integer jump `(N1,N2)`, and — only when a conservative gate passes — **bridges** the two smoother segments by a covariance-preserving `+dN·λ` shift so the spliced arc smooths across the boundary instead of being severed. Today every reset discards the float arc's backward information (the segment restarts forward) **and** discards the slip integer entirely (only *repairs* retain `dN`; *resets* segment). The concrete v1 win is **float-ambiguity continuity and recovered slip integers**: longer continuous float arcs through the RTS recursion → lower-variance, better-decorrelated smoothed float ambiguities, plus a diagnostic ledger of recovered integers at would-be resets. **Be explicit about what v1 does *not* buy:** in static PPP the smoothed *position* is degenerate by construction (constant at convergence), and AR runs on the **forward** solution captured *before* the smoother (`Application.cpp:1797-1800`) and is **not** fed back in v1 — so the "more fixable / faster TTFF" payoff does **not** materialize until a later AR-feedback increment. The headline benefit is improved smoothed float trajectory + integer recovery, concentrated in kinematic/long-arc/noisy datasets; minimal in clean static runs.

## 2. Feasibility verdict

**GREEN for the math + diagnostics increment (Phases 1–2). YELLOW for the live-bridge increment (Phase 3), gated on three must-fix correctness items below.**

- **Single biggest enabler (data retention reality):** the full parsed RINEX is resident for the entire run — `RinexParser::getObservations()` returns `observations_` by const ref (`RinexParser.hpp:122,233`) and `Application::run()` already iterates it by index (`Application.cpp:1510-1518`). So both-sides GF/MW windows at any past boundary are re-derivable at **zero new bulk storage**; the existing detector windows are forward-only and too short (`MwArcStats` caps at 30/64; `GfArc` keeps only `prev_gf`/`prev_prev_gf` + a 30-sample MAD deque) and **cannot** serve the after-side. The boundary locations are already retained on `smoother_buffer_[k].reset_satellites` / `is_discontinuity`, consumed only at `Application.cpp:1916`.
- **Single biggest blocker:** the reset `dN` is **lost** (resets re-seed + segment; only `ambiguity_repairs` carry `dN·λ`), so jump magnitude must be *re-estimated* from observables — which is exactly what this pass does. The smoother also has **no per-signal reset truth** on the snapshot (`reset_satellites` is a `set<string>` of *sat IDs*, matched by prefix in `ambSatWasReset`, `Smoother.cpp:37-45`), so v1 bridges the **dual-freq pair jointly**, not per-band.

## 3. Design

### Data flow

```
                 (post-processing only; RealTime path never builds the buffer)
forward pass ──► smoother_buffer_  ──[reset_satellites / is_discontinuity / pivot_satellites]
                      │                          │
RinexParser::         │                          ▼
 observations_ ──────►│            NonCausalJumpReestimator::run()  (PRE-smoother, by &ref)
 (raw L1/L2/P1/P2)    │              for each soft reset boundary:
                      │                1. build GF & MW windows from getObservations()
                      │                2. JumpGapFitter twice  -> g_GF, g_MW (+σ each)
                      │                3. jumpToIntegers({GF,MW}) -> (N1,N2,frac1,frac2)
                      │                4. CONSERVATIVE accept gate (see §5)
                      ▼                5a. ACCEPT: write bridged_ambiguity_offsets[key]=dN·λ
        std::move ──► Smoother(...)         and ERASE sat from reset_satellites
                      │                5b. REJECT: leave reset_satellites  -> existing break
                      ▼
            Smoother::run():
              collectCommonIndices keeps bridged keys
              backward step shifts COPIES of k+1 slices by +dN·λ (cov unchanged)
                      ▼
            ncjr_bridge_trace columns (in smoother_trace.csv)
```

### New components

| Component | File | Responsibility |
|---|---|---|
| `JumpGapFitter` (class) | `include/slip/JumpGapFitter.hpp` + `src/core/slip/JumpGapFitter.cpp` | Pure both-sides LS step estimator. Design row `[1, step_ind(0 pre/1 post), (n−n1)^1..^p]`, obs = series value; accumulate normal equations skipping sentinels; solve; return the **step coefficient** (jump in series units), per-side valid counts `k1,k2`, and a residual-based σ. No GNSS knowledge. Mirrors M-PAGES `fitGap` (`DbEdit.cpp:971-1033`). |
| `jumpToIntegers` (free fn, declared in `JumpGapFitter.hpp`) | `src/core/slip/JumpGapFitter.cpp` | Frame-agnostic 2×2 attribution **for the {GF,MW} pair — derived, not copied** from M-PAGES's {GF,IF} form (`DbEdit.cpp:925-926` is for `(ION_RESID,ION_FREE)` and must NOT be reused verbatim). Inputs `(g_GF, g_MW, λ1, λ2, f1, f2)` → `(N1, N2, frac1, frac2)`, **round-to-nearest**. Promoted/generalized from the inline TurboEdit kernel `dN2=round((jump−λ1·k)/(λ1−λ2)); dN1=dN2+k` (`TurboEditResolver.cpp:1310-1313`). **Must be unit-tested in Phase 1 with known synthetic `(N1,N2)` incl. MW vs GF sign conventions** — this is the single highest-risk arithmetic. |
| `NonCausalJumpReestimator` (class) | `include/filter/NonCausalJumpReestimator.hpp` + `src/core/filter/NonCausalJumpReestimator.cpp` | Orchestrates the pass: iterate soft boundaries, build GF/MW windows from `getObservations()` honoring clamps, call fitter ×2 + kernel, apply gates, write offsets + prune `reset_satellites` on accept, emit diagnostics. Owns per-sat-signal observable reconstruction. |
| `SmootherEpochSnapshot.bridged_ambiguity_offsets` (`std::map<std::string,double>`) | `include/filter/Smoother.hpp` (struct ~line 117, after `clean_handovers`) | Per-epoch `ambiguity_key → dN·λ` (**meters**, consistent with `ambiguity_repairs` and the F1 fold). Consumed by `Smoother::run` to shift and by `collectCommonIndices` to keep the key. Analogous to `clean_handovers`. Full Doxygen. |
| bridged-allow override in `collectCommonIndices` | `src/core/filter/Smoother.cpp:120-128` | Before the `ambSatWasReset` exclusion, if `amb_key ∈ sk1.bridged_ambiguity_offsets`, **keep** the key (**per-KEY**, not per-sat). |

### Algorithm (numbered)

1. For each boundary `k+1` with non-empty `reset_satellites`. **Skip** `is_discontinuity` / `!has_posterior` / dimension-mismatch boundaries entirely (hard, whole-filter, not per-sat bridgeable; `Smoother.cpp:320-337`).
2. For each candidate sat `s ∈ reset_satellites[k+1]`: **skip** if `s` is a BSSD pivot at `k` or `k+1` (replicate `repairAmbiguityBssdAware` pivot refusal, `CycleSlipManager.cpp:274-284`); **skip the entire constellation** if its pivot changed at this boundary (`sk.pivot_satellites != sk1.pivot_satellites` for that cons — a frame change surfaces on *many* peers; mirror M-PAGES `refSatSlip`, let `clean_handovers`/segmentation own it); **skip GLONASS** unless `ncjr_enable_glonass`.
3. Window bounds: `n1 = max(k+1−N, prevBoundaryForSat+1, arcStart)`, `n2 = k`; `n3 = k+1`, `n4 = min(k+1+N−1, nextBoundaryForSat−1, arcEnd)`. `N = ncjr_epochs_surrounding_cs`. Clamp so windows never cross an adjacent slip or a gap > `ncjr_max_gap_seconds`.
4. Reconstruct GF = `computeGfMeters(L1,L2)` (`TurboEditResolver.cpp:170-190`) and MW = `computeMwMeters` / `computeWideLaneWavelengthMeters` (`MwSlipDetector.cpp:344-363,554-598`) over `[n1..n4]` from `getObservations()`; skip missing/sentinel epochs. Require `k1>0 && k2>0` on **both** series. (Note: `RinexObservation` has no elevation — v1 commits to an **elevation-agnostic robust σ**; no detector-parity 1/sin weighting.)
5. `JumpGapFitter` on GF → `g_GF, σ_GF`; on MW → `g_MW, σ_MW`. Shared constant+poly trend absorbs iono/geometry drift; the step coefficient is the jump.
6. `jumpToIntegers(g_GF, g_MW, λ1, λ2, f1, f2)` → `(N1, N2, frac1, frac2)`, round-to-nearest. If MW gives only a float WL candidate (`slip_wl_float`, NaN-or-float, `MwSlipDetector.hpp:52-74`), use the **candidate-`k` loop form** (`TurboEditResolver.cpp:1009-1014`), not single-`k`.
7. **Accept** iff all of §5's guards hold (conservative).
8. **ACCEPT:** for each affected signal `delta_m = dN_sig·λ_sig`; set `smoother_buffer_[k+1].bridged_ambiguity_offsets[sat_signal_key]=delta_m` and **erase** `s` from `reset_satellites[k+1]`; emit a bridged diagnostic row.
9. **REJECT:** leave `reset_satellites[k+1]` → existing break; emit a not-bridged row.
10. `Smoother::run` proceeds as today except: (a) `collectCommonIndices` keeps bridged keys; (b) at each step where `sk1` has bridged offsets, **on COPIES** of `x_pred_k1` and `x_s_k1` for the bridged index, subtract `dN·λ` so the post-boundary value re-enters epoch `k`'s frame before the gain step. **Never mutate stored `x_post(k+1)` / `smk1.x`** (would corrupt the already-completed `k+1→k+2` step). Follow the `prepareHandoverFrame`/`inverseReReferenceGroup` copy-and-adjust pattern (`Smoother.cpp:351-360`). Covariance unchanged.

### Exact hook points

- `src/core/app/Application.cpp` Phase 5, **immediately before** `Smoother smoother(std::move(smoother_buffer_), ...)` (~`1910-1916`): if `config_.runNonCausalJumpReest() && mode_ != AppMode::RealTime && config_.runSmoother() && !smoother_buffer_.empty()`, construct `NonCausalJumpReestimator` with `&smoother_buffer_` (before the move), `rinex_parser_->getObservations()`, the wavelength source (`ppp_utils_`), and the BSSD pivot map; call `run()`; then proceed with the existing `std::move`.
- `include/filter/Smoother.hpp` — add `bridged_ambiguity_offsets` to `SmootherEpochSnapshot` (full Doxygen, note it overrides `reset_satellites` exclusion).
- `src/core/filter/Smoother.cpp:120-128` — `collectCommonIndices` per-key allow override.
- `src/core/filter/Smoother.cpp` `run()` backward loop (~`339-400`) — copy-and-shift the k+1 slices.
- `src/core/config/GnssConfigLoad.cpp` `loadProcessingSection_` (master toggle) / `loadEstimationSection_` (tuning) — `jval` parses.
- `include/config/GnssConfig.hpp` — private members + Doxygen getters.
- `CMakeLists.txt` — add `src/core/slip/JumpGapFitter.cpp` and `src/core/filter/NonCausalJumpReestimator.cpp` (explicit list, no globbing).
- `src/core/app/Application.cpp` `smoother_trace.csv` writer (~`1917-1951`) — **fold bridge columns into the existing `smoother_trace.csv`** (it already has a per-epoch `smoothed` flag) rather than a second file; add the toggle to `isLoggerEnabled` and the summary toggle-log line.

## 4. Config

Following the project convention (`bool enable_X_{false}` + `enableX()` getter w/ Doxygen + `jval` parse + summary log). **Master toggle lives in `processing`** (loop/phase-level, alongside `run_smoother`, the precedent that also *gates* this feature); tuning params in `estimation`.

| Flag | Section | Default | Purpose |
|---|---|---|---|
| `run_noncausal_jump_reest` | `processing` | `false` | Master toggle. Requires `run_smoother` (consumes `smoother_buffer_`); post-processing only (hard-skip RealTime). |
| `ncjr_epochs_surrounding_cs` | `estimation` | `5` | Half-window `N` per side (M-PAGES `epochs_surrounding_cs`). |
| `ncjr_polynomial_fit_terms` | `estimation` | `1` | Shared trend order: `1` = constant+step (default — order-2 over-fits a 5-pt side once gaps drop samples, and lower order tightens σ → fewer mis-bridges). Bump to `2` only if traces show iono curvature biasing the step. |
| `ncjr_cs_fix_fraction_limit` | `estimation` | `0.15` | Per-jump fractional accept gate (**tighter than the causal TurboEdit 0.25/0.30** — see §5). `0.0` disables fixing (float logged, never bridged). |
| `ncjr_consistency_tol_cycles` | `estimation` | `0.20` | Max disagreement between MW-implied `dN_WL` and `N1−N2` from the GF inversion (mutual-consistency gate, §5). |
| `ncjr_max_gap_seconds` | `estimation` | `120` | Windows never span a gap larger than this (arc-break semantics from MW/TurboEdit). |
| `ncjr_min_valid_per_side` | `estimation` | `3` | Minimum valid epochs per side per series (false-negative bias). |
| `ncjr_enable_glonass` | `estimation` | `false` | Allow GLONASS; off because FDMA inter-frequency code/phase biases distort GF/MW jump magnitudes. |

`jval` example: `run_noncausal_jump_reest_ = jval<bool>(json, "processing.run_noncausal_jump_reest", "run_noncausal_jump_reest", false);`

## 5. Edge cases & correctness guards

**Conservative accept gate (a wrong bridge MUST NOT happen — a tight-covariance mis-bridge is strictly worse than a clean break).** Bridging keeps the converged pre-slip *tight* covariance across a real slip; a wrong integer injects a confident lie into the smoother. The causal TurboEdit gate is **not sufficient verbatim** because the both-sides σ averages pre+post and is *smaller*, making the same nominal fraction limit effectively looser. Accept **only if ALL** hold:
1. Both windows had data: `k1 ≥ ncjr_min_valid_per_side` and `k2 ≥ ncjr_min_valid_per_side`, both series.
2. `frac1 < ncjr_cs_fix_fraction_limit` **and** `frac2 < ncjr_cs_fix_fraction_limit` (default 0.15, tighter than causal 0.25).
3. **Split-capability** on the *symmetric* σ (reuse `splitCapable` philosophy, `TurboEditResolver.cpp:214-222`): `σ ≤ split_max_sigma·|λ1−λ2|`.
4. **Mutual-consistency cross-check (new, mandatory):** `g_GF` and `g_MW` must be mutually consistent before trusting the 2×2 — require the MW-implied WL integer to agree with `N1−N2` from the GF inversion within `ncjr_consistency_tol_cycles`. The 2×2 can otherwise return a clean-looking `(N1,N2)` from two inconsistent jumps.
5. Not all-zero integers; `|N| ≤` sanity cap; `|λ1−λ2|` separation margin finite.
Bias every default toward **false-negative (break) over false-positive (mis-bridge)**.

- **Static-PPP value framing.** Bridging changes *float ambiguities*, not the (constant) smoothed position. Do not advertise a position gain; the value is ambiguity continuity + integer recovery (and AR only via a later feedback increment).
- **AR interaction.** Smoother and this pass operate on **float** ambiguities, captured **before** AR (`Application.cpp:1797-1800`). Bridged offsets are integer-valued shifts of the float state; they must **not** be confused with or double-applied against forward AR fix-and-hold (forward-only). Keep the pre-AR capture invariant. v1 does **not** feed bridged arcs back into AR.
- **Double-apply with the F1 repair fold.** The F1 fold (`Application.cpp:1751-1763`) shifts `x_pred(k)` for repairs whose sat is **not** in `reset_satellites`; this pass acts **only** on reset keys. The sets are **disjoint by construction** (a key is repaired-and-folded *or* reset-and-segmented, never both). State this invariant and add a **defensive assert** so a future change that lets a key be both can't silently stack two offsets.
- **Smoother shift consistency.** Shift `x_pred_k1` *and* `x_s_k1` (the smoothed value entering this step) by the same `dN·λ` on copies, so the RTS innovation `x_s_{k+1} − x_pred_{k+1}` stays coherent; never the stored vectors. If `P_pred_{k+1}` LDLT fails and the step falls back to forward (`Smoother.cpp:392-398`), the bridge silently no-ops for that epoch — **record "bridge attempted but step fell back"** in the trace so validation isn't misled.
- **Pivot / ref-sat boundaries.** Never integer-shift a BSSD pivot SD (frozen at 0). On a constellation pivot handover, skip **all** sats of that constellation (a slipped old pivot surfaces as `reset_satellites` on many peers; `clean_handovers` records only *clean* handovers — `PivotManager.cpp:708-716`); let the existing handover/segmentation path own it. BSSD SD-frame coupling: apply bridge only to keys **not** involved in a handover at that boundary.
- **GLONASS FDMA.** Per-slot λ and inter-frequency biases distort GF/MW; gate behind `ncjr_enable_glonass` (default off). λ1/λ2/f1/f2 in the inversion are **per-satellite** even for enabled constellations; resolve λ from an in-window observation (mirror `repairAmbiguityBssdAware` fallback), reject if unavailable.
- **Combination-key safety.** v1 handles only the standard dual-band pair (`G01_L1W_L2W`). `bridged_ambiguity_offsets` is in **meters**; WL/NL keys are in **cycles** (`StateKeySchema.hpp:167-198`). **Explicitly assert/skip** combination keys (don't rely on split-on-first-`'_'`, which can't tell `G01_L1W_L2W` from `G01_WL_...`); fall back to break otherwise. Multi/triple-freq is **out of v1 scope**.
- **Per-band granularity.** Snapshot has no per-signal reset truth and `jumpToIntegers` yields a joint `(N1,N2)`, so **bridge the dual-freq pair jointly**; no per-band partial bridge in v1.
- **Data gaps inside a window.** Skip sentinel samples in the normal equations (M-PAGES pattern); require `≥ ncjr_min_valid_per_side` per side per series or reject. Sparse/short arcs → reject → break.
- **RealTime-mode guard.** Hard-skip under `AppMode::RealTime` (the engine never builds `smoother_buffer_`); an unguarded call would read stale/empty state.
- **Buffer lifetime / idempotency.** Annotate `smoother_buffer_` by ref **before** the `std::move` at `Application.cpp:1916`. The pass runs once per process; mutations are not re-entrant. Document that after the move the buffer is moved-from (already true today).

## 6. Phasing (independently shippable, smallest first)

- **Increment 1 (first PR) — Phase 1 + Phase 2.**
  - **Phase 1 (S–M, inert):** `JumpGapFitter` (both-sides LS step fit) + `jumpToIntegers` {GF,MW} kernel as pure, fully-Doxygen'd units with **unit tests on synthetic windows with known `(N1,N2)`, incl. MW/GF sign conventions** (gating — the highest-risk math, verified before anything touches the smoother). No wiring.
  - **Phase 2 (M–L, diagnostics-only):** `NonCausalJumpReestimator` + config flags (master toggle default `false`). Iterate boundaries, reconstruct GF/MW windows, run fitter+kernel+conservative gates, write would-bridge/break + integers + fractions into `smoother_trace.csv`. **Do NOT mutate the buffer.** Lets the author validate recovered integers against known slips on real data at zero risk. **The project should STOP here for the first PR.**
- **Increment 2 (separate, gated PR) — Phase 3 (M, bridging live).** Add `bridged_ambiguity_offsets`, the `collectCommonIndices` per-key allow-override, the `Smoother::run` copy-and-shift; `NonCausalJumpReestimator` writes offsets + prunes `reset_satellites` on accept. Guard RealTime, gate on `run_smoother`. **Ships only after Phase 2 traces demonstrate recovered integers match known slips under the conservative gate.** Must land with the three must-fix items (copy-only shift, tighter gate + consistency check, derived & tested inversion).
- **Increment 3 (later) — Phase 4 (M, hardening / widening).** BSSD pivot + handover-boundary skips already in v1; tune the conservative gate from Phase 2/3 traces; double-apply assert. **Defer multi-freq/iono-free/triple-freq generalization until v1 is data-validated** (YAGNI — v1 already falls back to break for those).
- Config plumbing across all increments: **S**.

## 7. Open questions / decisions for the author

1. **Integer rounding:** round-to-nearest with a symmetric fraction gate (recommended; M-PAGES uses asymmetric `(int)` truncation). Confirm.
2. **Second observable:** `{GF,MW}` (geometry-free, no ref-sat correction, matches existing detector series) vs M-PAGES `{GF,IF}` (IF SD is sensitive to pivot changes). Recommend `{GF,MW}`. Confirm.
3. **AR feedback:** keep smoother-only in v1 (recommended) or feed bridged arcs to AR? Recommend defer; note the v1 value prop is scoped accordingly.
4. **Capture gate:** require `run_smoother` for v1 (recommended — the buffer only exists then; an independent capture gate would duplicate per-epoch capture cost). Recommend **close as "no"** for v1.
5. **Window source cost:** recompute GF/MW from `getObservations()` per in-window epoch (recommended, no new storage) vs a light per-arc GF/MW deque. Recommend recompute.
6. **Polynomial order default:** ship at `1` (constant+step); revisit `2` only if real-arc traces show iono curvature biasing the step.
7. **σ for the gates:** the symmetric σ is smaller than the causal MAD σ; confirm the tightened `ncjr_cs_fix_fraction_limit` (0.15) and `ncjr_min_valid_per_side` (3) defaults, or set from Phase 2 data.

---

**Key file:line anchors** (from findings): boundary source `Smoother.cpp:320-337`; key exclusion `Smoother.cpp:37-45,120-128`; F1 repair fold `Application.cpp:1751-1763`; copy-and-adjust template `Smoother.cpp:140-162,351-360`; RTS step `Smoother.cpp:405`; snapshot struct `Smoother.hpp:37-128`; smoother invocation/move `Application.cpp:1910-1952`; raw obs source `RinexParser.hpp:122,233`; GF/MW computers `TurboEditResolver.cpp:170-190`, `MwSlipDetector.cpp:344-363,554-598`; reusable 2×2 kernel `TurboEditResolver.cpp:1310-1313` (candidate-`k` `1009-1014`); accept-gate philosophy `TurboEditResolver.cpp:214-222`; pivot refusal `CycleSlipManager.cpp:274-284`; clean-handover recording `PivotManager.cpp:708-716`; cycles-vs-meters key convention `StateKeySchema.hpp:167-198`; M-PAGES `fitGap` `DbEdit.cpp:971-1033` and the {GF,IF}-specific inversion to **not** copy `DbEdit.cpp:925-926`.