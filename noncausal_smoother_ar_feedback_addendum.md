# Addendum: AR-Feedback Increment

*Extends the non-causal smoother-bridge scope. Covers only the AR-feedback delta; the base bridge (GF+MW slip recovery, in-place `repairAmbiguityBssdAware` jump, segment-spanning) is scoped separately and assumed.*

---

## 1. What changes vs the base scope

Three decisions are locked relative to the base bridge scope. **(a) GF+MW is the confirmed bridge evidence pair** — no new detector; the recovered `dN` is the deterministic additive jump already implemented in `CycleSlipManager::repairAmbiguityBssdAware` (`src/core/slip/CycleSlipManager.cpp:258-336`). **(b) The bridge actually applies (mutates state) in the first PR** — *not* diagnostics-only. ("Live" here means applied, not deferred; the pass is still **non-causal/post-forward** — a both-sides fit cannot run in the causal forward loop, which is what the existing causal TurboEdit already does.) **(c) AR feedback is added** — a *post-smoother final AR pass* over the smoothed (bridged) float ambiguities, exploiting backward-accumulated information for **earlier in-segment** epochs that the forward causal pass could not. Sequencing consequence: AR feedback is **strictly downstream** of the bridge and the smoother, lands in the same PR as a default-off stage, and is gated so it is a no-op unless `run_smoother && getArMode()>0 && app_mode_ != RealTime`. The post-processing benefit is a **higher final fix rate and a cleaner fixed-ambiguity product** — **not** TTFF (a causal metric, meaningless for a backward batch pass).

**Precondition — the control-input channel ALREADY EXISTS (tree-verified; the adversarial review's "blocking" alarm was based on a too-narrow grep and is RETRACTED).** `Application.cpp:1751-1763` is the documented "audit F1" fold: it folds `slip_result.ambiguity_repairs` (covariance-preserving `dN·λ` jumps) into the boundary snapshot's `x_pred` as a deterministic control input *precisely so the backward pass does not absorb the jump as a spurious measurement innovation*. The smoother seeds from the a-posteriori (`Smoother.cpp:295-302`), so an **uncompensated** jump would indeed be smoothed through — but F1 compensates exactly that for **repairs** (keys **not** in `reset_satellites`); **reset** keys are deliberately segmented (`Application.cpp:1758`, and merged into the segment set at `1787-1788`). The agent grepped `Smoother.cpp` for an invented `epoch_repairs_` symbol and missed the channel because it lives in `Application.cpp` writing `snap.x_pred`. **So the precondition is NOT "build a channel" — it is "emit a non-causally-recovered slip as a repair (fold into `x_pred` + drop from the snapshot's `reset_satellites`) instead of a reset."** That reuses proven machinery: an [S–M] reclassification, not an [L] new build. (For the post-forward pass, because the forward solution already reset+reseeded the boundary, the bridge writes the recovered `dN·λ` to the boundary snapshot's `x_pred` and removes the key from that snapshot's `reset_satellites` so `collectCommonIndices` keeps it and the segment is not broken — the same field and discipline F1 uses.)

---

## 2. AR-feedback mechanism

**Chosen approach: a new, separate post-smoother stage (Phase 6), reusing `AmbiguityResolver` unchanged at the algorithm level.** The findings are decisive that AR is a *pure pass over a snapshot*: `resolve()` / `resolveDd()` read `x`, `P`, and the ambiguity key→index map entirely from a `const StateManager&` and return a self-contained `AmbiguityResolution{fixed_state, fixed_covariance}` (`AmbiguityResolver.cpp:488-489`, `include/filter/AmbiguityResolver.hpp:35-42`). The only forward-loop coupling is the per-instance `epoch_count_` min-epochs counter (`AmbiguityResolver.cpp:297, 450-455`), handled trivially with a **fresh resolver instance** (own `epoch_count_=0`).

Rejected alternatives, explicitly: (i) an integer-constraint re-run *inside* the forward loop — that is the live bridge, already scoped; (ii) a new arbitrary-`(x,P)` `resolve()` overload baked into the smoother — unnecessary, since the snapshot path already yields a faithful per-epoch layout.

**Data flow and hook points (file:function):**

1. **Cache AR inputs at the existing pre-AR snapshot hook.** `Application::run` already calls `capturePosteriorSnapshot()` **before** AR (`Application.cpp:1797-1800`), so `smoother_buffer_` holds pre-AR **float** ambiguities. At the same site, push a `SmootherArInputs{ trimmed modeled-obs, getResolvedWlAmbiguities(), getResolvedWlFractions() }` onto a new `ar_input_buffer_` (1:1 with `smoother_buffer_`). The snapshots carry the dynamic key maps (`Application.cpp:1178-1184`) but **not** observations or MW WL integers, which `buildDdPairs` needs for elevation/pivot selection and the WL constraint. Apply the **same ATX WL-gate filtering** (`Application.cpp:1838-1883`) to the cached WL maps so the smoothed pass honors identical AR eligibility.
2. **Keep the smoother and its snapshots alive past the trace block** (`Application.cpp:1910-1952`, currently a local destroyed at scope exit — `Smoother.cpp:71-72` `std::move`s the snapshots in and frees them with the object). Promote the `Smoother` to a scoped object whose lifetime reaches Phase 6, or extract `results()` plus the snapshot key maps.
3. **Phase 6 — `Application::runSmoothedAr_`.** For each continuous segment, select the **last smoothed epoch before each boundary** (boundary predicate per `Smoother.cpp:320-358`). For target epoch *i*: take `results()[i].x / .P` (full-dim smoothed float) and `snapshots_[i].ambiguities` (+ iono/code-bias/sat-bias/sys-offset maps — identical `StateKeySchema` keys AR already uses).
4. **Reconstitute a transient `StateManager`** from `x_s`/`P_s` + the snapshot key maps (the inverse of `capturePosteriorSnapshot`), then hand it to a **fresh `AmbiguityResolver`** with `ar_input_buffer_[i]`: `ar_smoothed_->resolve(transient_sm, obs, wl_ambs, wl_fracs)`.
5. **Emit the result as a separate product** (`smoother_fixed_trace.csv`). Fixed integers are **never** written back into `smoother_buffer_` or the live `state_manager_` — the smoother stays float-only by contract and the forward solution is already finalized.

Baseline DD mirrors this with `resolveDd()` + `exclude_keys = meas_model_->getNonIntegerCapableDdAmbiguityKeys()` (`ApplicationBaseline.cpp:882-895`).

---

## 3. The independence problem (addressed head-on)

Two failure modes, both flagged by the adversarial review. **Stated honestly, the design's original guards do not contain them — here is what actually holds.**

**(a) Smoothed covariance inflates the ratio.** `P_s` is genuinely *smaller* than the forward posterior `P_post` — that is the point of smoothing. Feeding `P_s` into the ratio test runs the integer search on a tighter `Q_b̂` than the forward pass ever saw, raising the ratio. The future data folded in is time-correlated through the process model, so the ratio test's independent-information assumption is *more* violated by `P_s` than by `P_post`. **Consequence: part of the "higher fix rate" is a false-accept artifact, not new information.** Guard: `smoothed_ar_min_ratio` defaults to **strictly above** the forward `ratio_threshold` (not equal, as originally proposed), so the smoothed product is never accepted on weaker effective evidence. The `max_position_sigma_m` gate (`AmbiguityResolver.cpp:495-503`) provides **no** extra protection here — in static PPP the smoothed position covariance is degenerate, so this gate equals the forward final-epoch value; it is not claimed as an independent safeguard.

**(b) A coherent integer mis-bridge passes the ratio test.** The ratio test measures separation between the best and second-best integer candidate — **not correctness relative to truth.** A wrong-but-self-consistent `dN` (the whole arc shifted by an integer number of cycles after repair) yields a float DD that is still close to *an* integer, with the same tight covariance; LAMBDA fixes it to the wrong integer with a **high** ratio. **The ratio test is therefore NOT a backstop against a coherent integer error in the input float** — it only backstops noise-level float errors. The `ratio_upper_bound` guard (`AmbiguityResolver.cpp:528`) exists precisely because suspiciously high ratios are a smell, so a passing ratio is *not* containment.

**The contradiction guard as originally specified is circular and is dropped.** Cross-checking the recovered `dN` against the *smoothed* float fails because the smoothed float already contains the bridge jump (`repairAmbiguityBssdAware` mutated `x_post`; the smoother seeds from `x_post`). A wrong `dN` shifts both the forward arc and every smoothed value derived from it by the same amount — the check is tautological. A genuine independent check would require retaining the **pre-repair** float, which the live bridge does not currently keep.

**What is actually handed to AR and what we therefore commit to:**
- AR receives the **smoothed float `(x_s, P_s)`** via a transient `StateManager` and a **fresh resolver** — so the ratio is computed on genuine float covariance, never on a covariance already pinned by a prior forward fix-and-hold.
- AR's ratio/bootstrap/`nl_max_per_pair_frac`/`ratio_upper_bound` gates remain the validation layer for **noise-level** errors and are run identically.
- For **coherent mis-bridges**, we make **no safety claim**: the smoothed pass inherits the same exposure as the forward bridge and is documented as **not mis-bridge-safe**. The only real containment until a pre-repair-float cross-check is built is structural — **no write-back** (§2.5): a wrong accepted fix corrupts only the auditable `smoother_fixed_trace.csv`, never the forward solution or the float smoothed trajectory. **When AR rejects** (ratio/bootstrap below threshold), the arc is simply left float in the product — no fallback into live state, no retry.

The `smoothed_ar_require_bridge_confidence` flag is **cut from v1** (§4): it would imply a safety property the circular cross-check cannot deliver. Defer until a pre-repair-float channel exists.

---

## 4. Config (convention-following)

Two flags only for v1. Each follows the `enable_X_ / enableX() / jval<bool>(...)` convention; the master toggle is wired into `isLoggerEnabled` and the summary toggle-log line; the trace toggle is wired into `isLoggerEnabled`.

| Flag | Section | Default | Purpose |
|---|---|---|---|
| `enable_smoothed_ar` | `estimation` | `false` | Master toggle for the post-smoother final AR pass. No-op unless `run_smoother && getArMode()>0`; guarded against `AppMode::RealTime`. |
| `smoothed_ar_min_ratio` | `estimation` | `> ratio_threshold` (e.g. forward + a margin) | Independent ratio threshold for the smoothed pass. **Strictly ≥ forward** to offset `P_s` ratio inflation (§3a); confirm the margin with the author. |
| `enable_smoother_fixed_trace` | `logging` | `false` | Emit `smoother_fixed_trace.csv` (per-segment fixed/candidate/ratio/bootstrap/per-pair fixed-vs-float). Mirrors `enable_smoother_trace`; column registration goes through `TraceLogger`, **not** an ad-hoc writer. Added to `isLoggerEnabled`. |

**Cut from v1:** `smoothed_ar_require_bridge_confidence` (circular guard, §3); `smoothed_ar_segment_epoch="all"` (speculative diagnostic knob — ship `last_before_boundary` only, add `all` if traces show boundary-adjacent arcs left unfixed). Parse sites: `loadEstimationSection_` / `loadProcessingSection_` in `GnssConfigLoad.cpp`; getters in `GnssConfig.hpp`.

---

## 5. Correctness guards & edge cases

- **Static-PPP fix-rate honesty.** With no resets there is **one segment**, its last-before-boundary epoch **is N-1**, and N-1 is seeded directly to the forward posterior (`Smoother.cpp:295-302`) — it is **not** back-smoothed, so smoothed AR **== forward AR** there. **For a clean static run the entire feature is a no-op.** The gain exists **only** for multi-segment runs (short arcs, late-rising sats, arcs that converged near the end of visibility) — exactly the cases the live bridge tries to *prevent*. There is real internal tension: a successful bridge merges segments and shrinks the gain toward the no-gain case; a failed bridge leaves a short post-gap arc with little backward info. **Scope it as "longer arcs help short/late-rising sats," not a general fix-rate lift.** The trace must log the no-gain N-1 case explicitly rather than report it as an improvement.
- **WL/NL (N1,N2) mapping risk — NOT closed.** `resolveWlConstrained` (`AmbiguityResolver.cpp:1095-1166`) pins the MW-resolved DD-WL as a fixed integer and LAMBDA-searches **NL only**. A correct WL with a wrong NL back-transforms cleanly (`DD1=(nNL + f2/(f1+f2)·dd_wl)·λ1`) and passes the **NL** ratio test. The smoothed pass inherits this path unchanged and adds **no** NL-side independent validation — document it as a residual wrong-fix path identical to the forward pass, not a new exposure.
- **NlWl basis is degenerate — gate off.** Full LAMBDA NL in the `NlWl` basis is explicitly future work (`AmbiguityResolver.cpp:463-477` applies WL pseudo-obs only and returns). The smoothed pass would be WL-only there; gate it to per-band / WL-NL basis until NL LAMBDA lands.
- **Pivot exclusion + re-freeze.** The BSSD pivot is frozen at value 0 / variance 0 and skipped by `buildDdPairs` (`AmbiguityResolver.cpp:349-362`). The backward recursion touches the common subspace and **may write a non-zero covariance into a column the forward pass froze at var==0**, which would let `buildDdPairs` mis-classify the pivot as fixable and corrupt the SD frame. The transient `StateManager` reconstitution must **re-impose** the frozen-pivot `var==0` marking after smoothing — copying `P_s` through is insufficient. Reproduce the per-epoch `pivot_satellites` frame; confirm the snapshot's pivot map is sufficient.
- **GLONASS (FDMA).** Inherit the **same `ar_active_systems`** list so the smoothed pass skips exactly the constellations the forward pass deliberately excluded. The WL=N1−N2 / `resolveWlConstrained` integer-cycle assumption breaks for FDMA; confirm `ar_active_systems` is consulted **inside `buildDdPairs`** (not only at a higher gate) so a transient resolver cannot enumerate GLONASS DD pairs, and exclude GLONASS keys from any "smoothed-improved" bookkeeping.
- **Smoother-improved only when genuinely smoothed.** Treat an ambiguity as backward-informed only when `SmoothedEpoch.smoothed == true` **and** the key was in that step's common subspace (`Smoother.cpp:78-134`). Reset/orphaned/newly-added keys carry the forward float and confer no benefit — exclude them so a stale forward value is not mistaken for evidence.
- **Partial fix.** `resolve()` already supports `num_fixed < num_candidates`. Accept partial smoothed fixes under the smoothed pass's own gates and report `num_fixed/num_candidates` per segment — a partial smoothed fix is still a strictly better product than forward partial/no-fix.
- **Empty/no-posterior epochs, long-gap teardowns, single-epoch segments.** Segment boundaries (`has_posterior==false`, `x_pred/x_post` size mismatch). The selector skips them and picks the last **valid** smoothed epoch in the preceding segment. Single-epoch segments accumulate no backward info → skip or mark non-improved.

---

## 6. Revised phasing (folded into the single-PR-with-live-bridging decision)

Effort tags: **S** ≈ <0.5d, **M** ≈ 1–2d, **L** ≈ 3+d.

- **Phase A — Bridge-as-repair reclassification (reuses existing F1 fold). [S–M]** The control-input channel already exists (`Application.cpp:1751-1763`, §1). What's needed: have the non-causal pass write the recovered `dN·λ` into the boundary snapshot's `x_pred` and **remove the key from that snapshot's `reset_satellites`**, so the F1 fold compensates it and `collectCommonIndices` keeps the segment continuous instead of breaking it. No new smoother channel. Ships **in the base-bridge PR**. (NOT the previously-claimed [L] new-channel build — that alarm was retracted after tree verification.)
- **Phase B — Plumbing, no behavior change. [M]** Add `ar_input_buffer_` cache at the pre-AR snapshot hook (`Application.cpp:1797-1800`) with **trimmed** obs (per-sat elevation, sat id, signal — confirm these are the **only** fields `buildDdPairs` reads, `AmbiguityResolver.cpp:316-378`, before sizing); keep the `Smoother`/snapshots alive past the trace block; expose `snapshots_[i].ambiguities` (or copy onto `SmoothedEpoch`) + the boundary classification. Lands with/after the live-bridge PR (both touch the snapshot/smoother path).
- **Phase C — Transient-state reuse seam + faithful-reconstitution gate (HARD GATE). [M]** Implement the `StateManager` populate helper that takes **raw `(x, P, key→index maps)`** (not a `SmootherEpochSnapshot` — see §convention below), with frozen-pivot re-imposition. **Acceptance test:** `resolve()` on a transient `StateManager` built from the **forward posterior** reproduces the forward AR result **exactly**. This proves reconstitution is faithful before smoothed values are introduced. Do not proceed to D without it.
- **Phase D — Phase-6 pass (PPP / PPP-RTK). [L]** Add `SmoothedAmbiguityResolver` + `runSmoothedAr_`, fresh resolver from the same `AmbiguityResolverConfig` builder (`Application.cpp:1474-1490`, with `min_epochs=0` justified by converged `P_s`, all other gates inherited, `smoothed_ar_min_ratio` ≥ forward), segment-epoch selection (`last_before_boundary`), `smoother_fixed_trace.csv` via `TraceLogger`. Gated behind `enable_smoothed_ar` (default false) + RealTime guard. **AR-feedback lands here; its gate is the inherited ratio/bootstrap/`ratio_upper_bound`/`max_position_sigma_m` + `smoothed_ar_min_ratio`, with no write-back.** Validate on real data: smoothed-fixed vs forward-fixed acceptance counts/ratios; confirm an intentionally corrupted `dN` shows up as a low ratio **and** acknowledge it can still pass coherently (§3b).

**Deferred out of v1 (explicitly):**
- **Baseline DD mirror** (`resolveDd` + `exclude_keys`) — baseline AR ships mode=0 and the DD path is not yet data-validated; build only after the PPP path is proven.
- **`NlWl` basis** — until full NL LAMBDA lands.
- **Pre-repair-float cross-check + `smoothed_ar_require_bridge_confidence`** — the only path to genuine mis-bridge containment; needs retaining the pre-repair float.
- **Fix-then-resmooth (RTS over integer-constrained ambiguities)** — **firmly killed, not a "natural next increment."** The smoother is float-only by contract; an integer-constrained backward pass is a substantially more invasive change and is out of scope.

---

## 7. Open decisions remaining

1. **Smoother control-input shape (Phase A).** Exact representation of the bridge `dN·λ` as a smoother a-priori control input, and whether it lives on the snapshot or a parallel `epoch_repairs_` record. Blocks everything.
2. **`smoothed_ar_min_ratio` margin.** How far above the forward `ratio_threshold` to set the default, to offset `P_s` ratio inflation (§3a) without killing the genuine gain. Needs a data sweep + author sign-off.
3. **`ar_input_buffer_` memory cost.** A 24 h 1 Hz multi-GNSS run is ~86 400 epochs. Confirm the trimmed obs set (elevation / sat-id / signal) is sufficient for `buildDdPairs` so the cache is viable — must be resolved in Phase B, not left open.
4. **`AmbiguityResolverConfig` for the smoothed pass.** Recommend `min_epochs=0` (converged `P_s`), all other gates inherited, ratio ≥ forward — confirm.
5. **BSSD frame reconstitution.** Whether the snapshot's pivot map alone is enough to reproduce frozen-pivot variances and SD framing at the chosen epoch, or the full per-epoch `pivot_satellites` map must be cached.
6. **AR-on validation baseline.** Every shipped config is mode=0; there is currently **no in-tree config under which this feature does anything.** An AR-on config must be created and data-validated before any fix-rate claim can be measured.
7. **Segment-epoch policy.** `last_before_boundary` is the v1 default; revisit (fix both sides of a boundary and reconcile) only if traces show boundary-adjacent arcs left unfixed.

---

**Convention note (placement):** put the transient-state populate helper on `StateManager` taking **raw `(x, P, key→index maps)`**, *not* a `SmootherEpochSnapshot` — `capturePosteriorSnapshot` lives in `Application.cpp:1148-1197` and reads *out of* `StateManager` via public getters; coupling `StateManager` to a smoother/application type would invert that and violate the state-vector-discipline rule. `SmoothedAmbiguityResolver` translates snapshot → raw maps and calls the generic populate, keeping `StateManager` ignorant of smoother types.

**Relevant files (absolute):**
- `C:\GnssProcessor\src\core\app\Application.cpp` (snapshot hook 1797-1800; smoother block 1910-1952; AR site 1885-1898; AR config 1474-1490; `capturePosteriorSnapshot` 1148-1197)
- `C:\GnssProcessor\src\core\app\ApplicationBaseline.cpp` (DD AR site 882-895)
- `C:\GnssProcessor\src\core\filter\AmbiguityResolver.cpp` (`resolve` 419-547; `buildDdPairs` 316-413; `resolveWlConstrained` 1095-1166; `epoch_count_` gate 450-455; pivot skip 349-362)
- `C:\GnssProcessor\include\filter\AmbiguityResolver.hpp` (`AmbiguityResolution` 35-42; `resolve` sig 93-108)
- `C:\GnssProcessor\src\core\filter\Smoother.cpp` (segment boundary 320-358; seed 295-302; common subspace 78-134; snapshot move 71-72)
- `C:\GnssProcessor\src\core\app\Application.cpp` **F1 repair control-input fold 1751-1763 (EXISTS; reset keys skipped at 1758, segmented at 1787-1788)** — the channel the addendum's Phase A reuses
- `C:\GnssProcessor\include\filter\Smoother.hpp` (`SmoothedEpoch` 180-194; `results()` 196-200; float-only contract 149-150)
- `C:\GnssProcessor\src\core\slip\CycleSlipManager.cpp` (`repairAmbiguityBssdAware` 258-336)
- `C:\GnssProcessor\src\core\filter\StateManager.cpp` (`resetAmbiguity` 964-1017; `addAmbiguity` 858-875)
- New: `include/filter/SmoothedAmbiguityResolver.hpp` + `src/core/filter/SmoothedAmbiguityResolver.cpp`; config in `include/config/GnssConfig.hpp` + `src/core/config/GnssConfigLoad.cpp`