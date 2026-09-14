/**
 * @file NetworkAmbStateTrace.hpp
 * @brief RF-1 §7.4 — the per-arc AMBIGUITY CARRY-STATE trace
 *        (@c network.trace_amb_state -> @c network_amb_state.csv).
 *
 * Card: docs/refound/RF-1_float_ambiguities_design.md §7.3 step 2, under
 * docs/refound/NETWORK_REFOUNDING_2026-09-14.md.  **This lands and is READ before any
 * mechanism**: it is the diagnostic that must move, and a pre-change baseline is
 * required.
 *
 * WHY IT EXISTS.  R1's headline statistic — "the fraction of arcs whose level is set at
 * birth" — was the right question asked of the wrong quantity.  It read the per-LOS CODE
 * RESIDUAL because no trace of the carry itself existed: the network's forty-odd traces
 * carry no per-arc carry state, and @c network_prior_residual.csv is aggregated per
 * (family, label) per EPOCH, so it structurally cannot give a per-arc trajectory.  This
 * file is that trajectory: one row per carried (station, satellite, signal) per traced
 * epoch with what was carried, what was FOLDED, the trajectory's abscissa and the arc's
 * events.
 *
 * MEASURE-ONLY.  Every value written is read off state the inject and the harvest
 * already computed — the fold variance and route are recorded at the inject into the
 * station's own @c StationAssembly::amb_state_fold (the @c pl_los_beff discipline), the
 * carried quantities are read at the stash, and the `erase:` rows are captured at the
 * four erase sites immediately BEFORE the entry is deleted.  Nothing is written back.
 * Default OFF ⇒ no stream, no file, no rows, and the solve is byte-identical.
 *
 * THE FOUR PRE-REGISTERED READS (design §7.4; scripts/amb_state_read.py implements them
 * verbatim), so the columns are not negotiable after the baseline is taken:
 *  - **D1 the variance trajectory** — per arc, fit @c log(fold_var) = a − b·log(n_folds)
 *    over the first 30 folds; `b` at p50 across arcs.  RF-0 predicts `b > 1` (faster
 *    than 1/n: the over-count).  RF-1's bar is `1.00 ± 0.15`;
 *  - **D2 the level trajectory** — R1 re-asked on the carry: the fraction of arcs whose
 *    @c value_abs at folds 1–3 is within 0.3 m of its settled value, and the fraction
 *    whose |value_abs| grows monotonically (RF-0 proxies: 0.39–0.46 and 74–77 %);
 *  - **D3 the drop / erase inventory** — per hour by cause, with the fold variance
 *    immediately before and after and the folds taken to recover it.  Invariant #4 in
 *    the other direction: a float_kf run with 0 drops is a MISWIRE, not a quiet run;
 *  - **D4 σ̂₀(pl) by arc age** (first 5 folds / 5–30 / > 30).  An honest prior reads ≈ 1
 *    in every bucket.  This is the read that can REFUTE the card before any mechanism
 *    is built, and the one that calibrates the birth floor if it does not.
 *
 * ⚠ D4's split cannot come from @c network_prior_residual.csv, which has no arc-age
 * column; it is formed from CONSECUTIVE rows of the same arc here — @c e =
 * value_abs(t−1) − value_abs(t) is exactly the harvest's own prior-residual numerator
 * and @c fold_var_m2 is exactly its denominator — with the census's file kept as the
 * unsplit cross-check.  The census's standing caveat travels with it: under Card-1's
 * conditional coupling the fold's RHS is not @c value_abs alone (the coupling rows
 * contribute Σ M_rj·ŝ_prev,j), so this ê and the ê the fold saw differ by the regressor
 * term.  That is an open card on the census, not a defect of this trace.
 */
#pragma once

#include "filter/NetworkRunTypes.hpp"

#include <memory>
#include <string>

namespace spdlog { class logger; }

namespace GnssProcessor {

    class GnssConfig;
    struct NetworkRunContext;
    struct NetworkRunTraces;

    /// @brief The @c network_amb_state.csv column header — design §7.4, verbatim.
    ///
    /// Pinned as a named constant (and unit-tested against the design's literal) because
    /// scripts/amb_state_read.py parses by NAME: a column renamed or reordered silently
    /// changes what every pre-registered read means, and the whole point of step 2 is
    /// that the RF-0 baseline and every later arm are read the same way.
    inline constexpr const char* kAmbStateHeader =
        "week,tow,station,sat,signal,value_abs_m,var_m2,var_first_m2,var_birth_m2,"
        "fold_var_m2,n_folds,age_ep,age_since_relevel_ep,folded,route,event\n";

    /**
     * @brief The `route` cell for a fold route — the trace's vocabulary, in one place.
     * @param r The route the inject recorded.
     * @return "" (not folded) | "diag" | "block" | "coupling-scalar".
     */
    const char* ambStateRouteName(AmbFoldRoute r);

    /**
     * @brief Format ONE @c network_amb_state.csv row.
     *
     * Factored out of the writer so the ROW RULE — not a paraphrase of it — is what the
     * seam calls and what tests/test_amb_float_vehicle.cpp checks.  @c value_abs_m and
     * the four variances are written at full double precision on purpose: a settled
     * arc's fold variance lives at the 1e-4 m² scale, and a 6-significant-digit default
     * would quantize it enough to flatten D1's own slope.
     *
     * @param r The row to format.
     * @return The CSV line, newline-terminated, with exactly as many cells as
     *         @ref kAmbStateHeader.
     */
    std::string ambStateFormatRow(const AmbStateRow& r);

    /**
     * @brief Capture an @c erase:<cause> row for an entry that is about to be DELETED.
     *
     * The four erase paths of design §1.5 / §1.6 — the T4 attribution refloat, the T3
     * consensus refloat, the @c phase_leveling_max_gap_s prune and the phase-anchor
     * carry re-reference — all destroy the entry, so the row has to be taken here,
     * BEFORE the erase, or D3's inventory has no "variance before" and no identity.
     * That is standing invariant #7's decoupling edge made auditable: today these are
     * the paths that fully discard a converged level, and the whole of RF-1 step 4 is
     * the claim that a bounded DROP should replace three of them.
     *
     * A no-op unless @c network.trace_amb_state is armed, so every call site is one
     * unconditional line and the OFF path costs one bool test.
     *
     * @param ctx     The run context (the row buffer, the loop-epoch clock, the knob).
     * @param week    GPS week of the erasing epoch.
     * @param tow     GPS time of week (s) of the erasing epoch.
     * @param station Station name.
     * @param sat     Satellite.
     * @param signal  RINEX signal.
     * @param lc      The entry as it stands immediately before deletion.
     * @param cause   Short cause tag: @c t4_attrib, @c t3_consensus, @c gap_prune,
     *                @c anchor_reref.  Written as @c "erase:<cause>".
     */
    void ambStateNoteErase(NetworkRunContext& ctx, long long week, double tow,
                           const std::string& station, const std::string& sat,
                           const std::string& signal, const LocalCarry& lc,
                           const char* cause);

    /**
     * @brief Drain this epoch's buffered carry-state rows to @c network_amb_state.csv.
     *
     * The serial drain seam (CLAUDE.md's network decision tree: a new diagnostic sweep
     * gets its own seam, and trace-stream ownership lives in the traces object, not in
     * the driver).  Called once per processed epoch at the end of the stash/status
     * block, which is AFTER every row source has run: the leveling harvest and the gap
     * prune (SB_LEVELING), the T4 / T3 refloats (SB_RAIM) and the phase-anchor carry
     * re-reference.  Always clears the buffer, so a stream that failed to open cannot
     * grow it without bound.
     *
     * @param ctx    The run context (owns the row buffer and the run counters).
     * @param log    The network logger (used only for the one-shot open failure).
     * @param traces The run's trace streams (owns @c amb_state_trace).
     */
    void networkAmbStateTrace(NetworkRunContext& ctx,
                              const std::shared_ptr<spdlog::logger>& log,
                              NetworkRunTraces& traces);

}  // namespace GnssProcessor
