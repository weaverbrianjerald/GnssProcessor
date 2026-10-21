/**
 * @file AmbStateTrace.hpp
 * @brief Per-arc ambiguity carry-state trace
 *        (@c network.trace_amb_state -> @c network_amb_state.csv).
 *
 * The trace is taken and read before any change to the ambiguity carry mechanisms: it
 * is the diagnostic those mechanisms must move, so a pre-change baseline is required.
 *
 * Purpose.  No other network trace records per-arc carry state, and
 * @c network_prior_residual.csv is aggregated per (family, label) per epoch, so it cannot
 * give a per-arc trajectory.  This file does: one row per carried (station, satellite,
 * signal) per traced epoch, with what was carried, what was folded, the trajectory's
 * abscissa and the arc's events.
 *
 * Measure-only.  Every value written is read off state the inject and the harvest
 * already computed: the fold variance and route are recorded at the inject into the
 * station's own @c StationAssembly::amb_state_fold, the carried quantities are read at
 * the stash, and the `erase:` rows are captured at the erase sites immediately before
 * the entry is deleted.  Nothing is written back.  Off by default: no stream, no file,
 * no rows, and the solve is byte-identical.
 *
 * The four reads (scripts/amb_state_read.py implements them).  The columns are fixed
 * once the baseline is taken:
 *  - **D1 the variance trajectory**: per arc, fit @c log(fold_var) = a − b·log(n_folds)
 *    over the first 30 folds; `b` at p50 across arcs.  `b > 1` (faster than 1/n) is an
 *    over-count.  The bar is `1.00 ± 0.15`.
 *  - **D2 the level trajectory**: the fraction of arcs whose @c value_abs at folds 1 to 3
 *    is within 0.3 m of its settled value, and the fraction whose |value_abs| grows
 *    monotonically.
 *  - **D3 the drop / erase inventory**: per hour by cause, with the fold variance
 *    immediately before and after and the folds taken to recover it.  A float_kf run
 *    with 0 drops indicates a wiring fault, not a quiet run.
 *  - **D4 σ̂₀(pl) by arc age** (first 5 folds / 5 to 30 / > 30).  A correctly weighted
 *    prior reads ≈ 1 in every bucket.  This read can refute the approach and otherwise
 *    calibrates the birth floor.
 *
 * D4's split cannot come from @c network_prior_residual.csv, which has no arc-age
 * column.  It is formed from consecutive rows of the same arc: @c e =
 * value_abs(t−1) − value_abs(t) is the harvest's own prior-residual numerator and
 * @c fold_var_m2 is its denominator; the census file remains the unsplit cross-check.
 * The census's caveat applies here too: under the conditional coupling the fold's
 * RHS is not @c value_abs alone (the coupling rows contribute Σ M_rj·ŝ_prev,j), so this
 * ê and the ê the fold saw differ by the regressor term.  That is a property of the
 * statistic, not a defect of this trace.
 */
#pragma once

#include "network/driver/RunTypes.hpp"

#include <memory>
#include <string>
#include <cstddef>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

struct EpochEvents;   // network/driver/Epoch.hpp

    class GnssConfig;
    struct NetworkRunContext;
    struct NetworkRunTraces;

    /// @brief The @c network_amb_state.csv column header.
    ///
    /// A named constant, unit-tested against its literal, because
    /// scripts/amb_state_read.py parses by column name: renaming or reordering a column
    /// changes what every read means without any error, and the baseline and every
    /// later arm must be read the same way.
    inline constexpr const char* kAmbStateHeader =
        "week,tow,station,sat,signal,value_abs_m,var_m2,var_first_m2,var_birth_m2,"
        "fold_var_m2,n_folds,age_ep,age_since_relevel_ep,folded,route,event\n";

    /**
     * @brief The `route` cell for a fold route; the trace's route vocabulary is defined
     *        here alone.
     * @param r The route the inject recorded.
     * @return "" (not folded) | "diag" | "block" | "coupling-scalar".
     */
    const char* ambStateRouteName(AmbFoldRoute r);

    /**
     * @brief Format one @c network_amb_state.csv row.
     *
     * Separate from the writer so the seam and tests/network/test_amb_float_vehicle.cpp use the
     * same row rule.  @c value_abs_m and the four variances are written at full double
     * precision: a settled arc's fold variance is at the 1e-4 m² scale, and a
     * 6-significant-digit default would quantize it enough to flatten D1's slope.
     *
     * @param r The row to format.
     * @return The CSV line, newline-terminated, with exactly as many cells as
     *         @ref kAmbStateHeader.
     */
    std::string ambStateFormatRow(const AmbStateRow& r);

    /**
     * @brief Capture an @c erase:<cause> row for an entry that is about to be deleted.
     *
     * The erase paths (the attribution refloat, the consensus refloat, the
     * @c phase_leveling_max_gap_s prune and the phase-anchor carry re-reference) all
     * destroy the entry, so the row is taken here, before the erase; otherwise D3's
     * inventory has no "variance before" and no identity.  This makes the decoupling
     * edge auditable.  Where the bounded drop replaces an erase (the attribution and
     * consensus refloats on the float vehicle), the site records a drop row through
     * ambStateNoteDrop() instead.
     *
     * A no-op unless @c network.trace_amb_state is armed, so every call site is one
     * unconditional line and the off path costs one bool test.
     *
     * @param ctx     The run context (the row buffer, the loop-epoch clock, the knob).
     * @param week    GPS week of the erasing epoch.
     * @param tow     GPS time of week (s) of the erasing epoch.
     * @param station Station name.
     * @param sat     Satellite.
     * @param signal  RINEX signal.
     * @param lc      The entry as it stands immediately before deletion.
     * @param cause   Short cause tag: @c t4_attrib, @c t3_consensus, @c gap_prune,
     *                @c anchor_reref, or @c gate_drop (the anchor re-reference site,
     *                for an arc of a family the coverage gate dropped).
     *                Written as @c "erase:<cause>".
     */
    void ambStateNoteErase(NetworkRunContext& ctx, long long week, double tow,
                           const std::string& station, const std::string& sat,
                           const std::string& signal, const LocalCarry& lc,
                           const char* cause);

    /**
     * @brief Capture a @c drop:<cause> row for an entry whose prior was just dropped
     *        back to its birth variance.
     *
     * Taken after the drop, so the row carries the restored variance.  That is D3's
     * "variance after", and it distinguishes a bounded drop from the erase it replaces
     * (an erase row's "after" can only come from the successor arc).
     *
     * A no-op unless @c network.trace_amb_state is armed.
     *
     * @param ctx     The run context (the row buffer, the loop-epoch clock, the knob).
     * @param week    GPS week of the epoch.
     * @param tow     GPS time of week (s) of the epoch.
     * @param station Station name.
     * @param sat     Satellite.
     * @param signal  RINEX signal.
     * @param lc      The entry as it stands immediately after the drop.
     * @param cause   One of @ref AmbDropCause.  Written as @c "drop:<cause>".
     */
    void ambStateNoteDrop(NetworkRunContext& ctx, long long week, double tow,
                          const std::string& station, const std::string& sat,
                          const std::string& signal, const LocalCarry& lc,
                          const char* cause);

    struct NetworkRun;              // network/driver/Run.hpp
    struct NetworkEpoch;            // network/driver/Epoch.hpp

    /**
     * @brief Drain this epoch's buffered carry-state rows to @c network_amb_state.csv.
     *
     * The serial drain seam.  Called once per processed epoch at the end of the
     * stash/status block, after every row source has run: the leveling harvest and the
     * gap prune (SB_LEVELING), the attribution and consensus refloats (SB_RAIM) and the
     * phase-anchor carry re-reference.  Always clears the buffer, so a stream that
     * failed to open cannot make it grow without bound.
     *
     * @param run The run (network/driver/Run.hpp).  Written: run.ctx (owns the row buffer
     *            and the run counters), run.traces (owns @c amb_state_trace).  Read: run.log
     *            (used only to warn when buffered rows are discarded because the stream is
     *            not open).
     * @param ep  The epoch (not read; the observer form).
     */
    void networkAmbStateTrace(NetworkRun& run, const NetworkEpoch& ep);

    /**
     * @brief Writes the epoch's buffered phase-colour and ambiguity-information
     *        trace rows to their streams.
     *
     * Last inside the stash bracket, after every row source has run.  A buffer whose stream is
     * not open is skipped unwritten.
     *
     * @param run The run: the traces.
     * @param ep  The epoch: the harvest events' pc_trace_rows and amb_if_rows (read).
     */
    void networkBufferedTraceDrains(NetworkRun& run, const NetworkEpoch& ep);
    // ---- Run-end reports ----

    struct NetworkRunEndView;

    /**
     * @brief Run-end report: the ambiguity-state trace.
     *
     * The ambiguity-state trace: events and erase rows written.
     *
     * Gate: prints under `config_.getNetworkConfig().trace_amb_state`.
     *
     * @param v The run at its end (network/runend/RunEnd.hpp).
     * @return 0 (this report does not set the exit code).
     */
    int networkAmbStateTraceRunEnd(const NetworkRunEndView& v);

    // ---- End of the run-end reports ----

// ---- Harvest half, declarations ----

struct NetworkRun;                          // network/driver/Run.hpp
struct NetworkEpoch;                        // network/driver/Epoch.hpp
struct HarvestBorder;                       // network/carry/LocalCarryHarvest.hpp
struct StationHarvestBlock;                 // network/carry/LocalCarryHarvest.hpp

/**
 * @brief The ionosphere-free ambiguity's prior and posterior variance per line of sight
 *        with both bands in the block, raw and de-meaned per constellation. Measure-only; no-op
 *        unless network.trace_amb_if.
 *
 * @param run The run: its context (knobs, entities, counters) and setup (stations).
 * @param ep This epoch after the solve (the assembly and the clock are read; nothing is
 *           written).
 * @param sb Station @p i's harvest layout (its block columns, segment rows, back-substituted locals
 *           and posterior covariance block).
 * @param i The station index.
 * @param events The epoch's events; writes harvest (the buffered rows).
 */
void networkAmbIfTraceStation(NetworkRun& run, const NetworkEpoch& ep,
                              const StationHarvestBlock& sb, std::size_t i, EpochEvents& events);

// ---- End of the harvest half, declarations ----

/// @brief Run-scope counters of AmbStateTrace, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct AmbStateTraceCounters {
    long long n_amb_state_rows        = 0;  ///< network_amb_state.csv: trajectory rows written (one per carried (station,sat,signal) per traced epoch).
    long long n_amb_state_events      = 0;  ///< network_amb_state.csv: rows carrying a non-empty `event` (birth / drop / erase).
    long long n_amb_state_epochs      = 0;  ///< network_amb_state.csv: epochs that wrote at least one row.
    long long n_amb_state_erase_rows  = 0;  ///< network_amb_state.csv: `erase:<cause>` rows, entries captured at the four erase sites before deletion.
};

}  // namespace GnssProcessor
