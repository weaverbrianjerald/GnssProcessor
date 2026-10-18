#pragma once
/**
 * @file StationBlock.hpp
 * @brief Per-station local-block audit (@c network.trace_station_block) and the pure
 *        algebra behind it (SPEC_event_harness §24.10).
 *
 * The audit addresses a case recorded in §24.9: a station with post-fit code
 * residuals of 0.3–0.55 m reported a reconstructed ZWD of −18 ± 4.5 m, with 36 % of
 * its rows at zero redundancy.  A formal σ of 4.5 m on a column whose partial is the
 * wet mapping function, against about ten code rows of σ 0.4 m, requires the column's
 * effective information (what remains after every other local state is eliminated)
 * to be orders of magnitude below its raw @f$\sum w\,m_\text{wet}^2@f$.  This file
 * measures that effective information and names the local directions the block
 * cannot resolve.
 *
 * The audited object is the station's local normal block
 * @f[
 *     N_{rr} \;=\; A_r^\top W A_r \;+\; \text{(folded station priors)} ,
 * @f]
 * with the shared states held at their solved values @f$x_s@f$.  This is the same
 * conditioning @ref NetworkEstimator::reconstructStation applies when it forms
 * @f$x_r=N_{rr}^{-1}\!\left(u_r-N_{rs}x_s\right)@f$.  It is neither the station's
 * stand-alone normal matrix (which would carry the satellite clocks and biases as
 * unknowns) nor the reduced shared system.
 *
 * Measure-only: @ref networkStationBlock runs in the trigger-instrumentation group
 * after the solve, reads @ref NetworkEstimator through const accessors, and writes one
 * CSV row per station per traced epoch.  Nothing it touches feeds @c x, @c N or the
 * order of operations, whether armed or not (standing invariant #6).
 *
 * @ref StationBlockAudit holds the numerical content as pure functions over
 * @c (N_rr, A_r, w, labels), so the rank, span, effective-information and redundancy
 * algebra is unit-testable without a network run.  The seam itself only builds labels
 * and writes the CSV.
 */

#include "network/solve/Estimator.hpp"
#include "network/driver/RunTypes.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <Eigen/Dense>

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;   // network/state/RunContext.hpp; included in the .cpp only
struct NetworkRunTraces;    // network/diag/RunTraces.hpp; included in the .cpp only

/**
 * @brief Pure algebra of the §24.10 station-block audit: no estimator, no config,
 *        no I/O.
 *
 * Every function here takes the local block @f$N_{rr}@f$ (symmetric,
 * @c n_local × @c n_local) and, for the row census, the station's local design rows
 * @f$A_r@f$ with their information weights @f$w@f$.  The non-null subspace is defined
 * once, by @ref kRankTolRel, and used for the rank, the condition number, and the
 * pseudo-inverse in both the effective information and the redundancy numbers.  A
 * block this close to singular gives different answers under different tolerances,
 * so this file fixes one.
 */
namespace StationBlockAudit {

/// Relative eigenvalue floor defining the non-null subspace: an eigenvalue is
/// treated as structurally zero when @f$\lambda \le @f$ @c kRankTolRel times the
/// largest.  Fixed here so that rank, condition number, effective information
/// and redundancy all refer to the same subspace.
inline constexpr double kRankTolRel = 1e-9;

/// Redundancy below which a row counts as zero-redundancy (uncheckable: fully
/// absorbed by free local parameters).  The census threshold of §24.9.
inline constexpr double kZeroRedundancy = 1e-6;

/**
 * @brief Spectral part of the audit: rank, condition number, and the three
 *        smallest eigenpairs with the local states that span them.
 */
struct Spectrum {
    bool        ok{false};        ///< The decomposition converged and @f$N_{rr}@f$ was usable.
    Eigen::Index n_local{0};      ///< Block dimension.
    Eigen::Index rank{0};         ///< @f$\#\{\lambda > \text{kRankTolRel}\cdot\lambda_\max\}@f$.
    double      cond{0.0};        ///< @f$\lambda_\max/\lambda_\min@f$ over the non-null subspace (0 when rank < 1).
    /// The three smallest eigenvalues, ascending (unused slots are NaN).
    std::array<double, 3> ev_val{{0.0, 0.0, 0.0}};
    /// For each, the local states carrying its squared loading, as up to three
    /// @c "label=share" items joined by @c '|', truncated once the cumulative
    /// share reaches 0.90 (see @ref spanOf).  Empty for an unused slot.
    std::array<std::string, 3> ev_span{{{}, {}, {}}};
    /// Full spectrum, ascending; kept so that callers can reuse the decomposition.
    Eigen::VectorXd eigenvalues;
    /// Eigenvectors, column @c k matching @c eigenvalues(k).
    Eigen::MatrixXd eigenvectors;
};

/**
 * @brief Format one eigenvector's span as a compact @c "label=share" list.
 *
 * The share of local state @c k is @f$v_k^2@f$ divided by @f$\|v\|^2@f$ (for a
 * unit-norm eigenvector the shares sum to one).  States are listed in descending
 * share; the list stops at three items or once the cumulative share reaches
 * @p cover, whichever comes first.  Items are joined by @c '|' and label and share by
 * @c '=', so the field fits one CSV column and a label that contains colons
 * (@c "amb:G24:L5X") stays unambiguous.  Any @c ',', @c '|' or @c '=' inside a label
 * is replaced by @c ';'.
 *
 * @param v      Eigenvector (length @c n_local).
 * @param labels Per-local-column kind labels (same length; a missing or empty entry
 *               yields @c "col<k>").
 * @param cover  Cumulative squared-loading share at which to stop (0.90 in the station-block trace).
 * @return The formatted span, or an empty string for an empty or zero vector.
 */
std::string spanOf(const Eigen::VectorXd& v, const std::vector<std::string>& labels,
                   double cover = 0.90);

/**
 * @brief Decompose the local block: rank, condition number, three smallest eigenpairs.
 *
 * @param Nrr    The local normal block (symmetric, @c n_local × @c n_local).  Its
 *               lower triangle is read (@c Eigen::SelfAdjointEigenSolver's contract).
 * @param labels Per-local-column kind labels for the spans.
 * @return A @ref Spectrum with @c ok=false when the block is empty, non-square,
 *         non-finite, or the decomposition failed to converge.
 */
Spectrum spectrum(const Eigen::MatrixXd& Nrr, const std::vector<std::string>& labels);

/**
 * @brief Effective information of one column (the ZWD column in the station-block audit): what
 *        remains of it once every other local state has been eliminated.
 *
 * The Schur complement of column @p col against all other local columns,
 * @f$N_{zz}-N_{zo}N_{oo}^{-1}N_{oz}@f$, equals @f$1/(N_{rr}^{-1})_{zz}@f$.
 * The identity is used directly, with the pseudo-inverse taken on the non-null
 * subspace of the same decomposition the rank is read from:
 * @f[
 *     \text{info}_\text{eff}(z) \;=\;
 *     \Big(\textstyle\sum_{\lambda_k>\epsilon} V_{zk}^2/\lambda_k\Big)^{-1}.
 * @f]
 * This avoids a second decomposition of @f$N_{oo}@f$ and, unlike an explicit
 * elimination, stays well-behaved when @f$N_{oo}@f$ is itself singular.
 *
 * @param sp  A converged @ref Spectrum of the block.
 * @param col The column of interest (the ZWD column in the station-block audit).
 * @return The effective information, or 0.0 when @p col has no mass in the
 *         non-null subspace (a column the block cannot see) or @p sp is not usable.
 */
double effectiveInformation(const Spectrum& sp, Eigen::Index col);

/**
 * @brief Per-row redundancy numbers on the local block.
 *
 * @f[
 *     r_i \;=\; 1 - w_i\,h_i^\top N_{rr}^{+} h_i ,
 * @f]
 * with @f$h_i@f$ the row's local partials (row @c i of @p A_r) and the
 * pseudo-inverse taken on the non-null subspace of @p sp.  @f$r_i\approx 0@f$ means
 * the row is fully absorbed by free local parameters: its residual is not checkable,
 * and no QC screen can detect a fault on it.
 *
 * This is the local-block redundancy: the shared states are held at their solved
 * values, which is the frame of the reconstruction and of the ZWD trace.  A row can
 * be locally unchecked and still constrain the network.
 *
 * @param sp  A converged @ref Spectrum of the block.
 * @param A_r Station's local design rows (@c m × @c n_local).
 * @param w   Per-row information weights (length @c m; a missing entry is read as 1.0).
 * @return One redundancy per row (empty when @p sp is unusable or the shapes
 *         disagree; all ones for a rank-0 block).  Values are not clamped: a small
 *         negative value is round-off and compares like zero against
 *         @ref kZeroRedundancy.
 */
Eigen::VectorXd redundancy(const Spectrum& sp, const Eigen::MatrixXd& A_r,
                           const Eigen::VectorXd& w);

}  // namespace StationBlockAudit

/**
 * @brief The kind label of every local column of a station's block, for the span field.
 *
 * Read from the station's parameter set through the assembly's state-index to local-column
 * map, so the labels are correct under both vehicles (nothing here is carry-gated).  The
 * labels: the audit's @c clk, @c zwd, @c rxcb:<key>, @c iono:<sat>, @c amb:<sat>:<signal>, plus
 * @c pos:<xyz>, @c isb:<system>, @c satbias:<key>, @c satpb:<key>, @c rxpb:<key> and
 * @c satclk:<key>.  The phase colour has no label: its column appears in a span as
 * @c col<k>, and an unnamed slot (a layout column of the single-receiver filter) the same.
 *
 * @param R       The station's assembly of the epoch (its @c h2local).
 * @param set     The station's parameter set, built at the same epoch's split.
 * @param n_local Number of local columns (the width of the block).
 * @return One label per local column; empty for a column nothing names.
 */
std::vector<std::string> stationBlockLabels(const StationAssembly& R, const StationParams& set,
                                            Eigen::Index n_local);

struct NetworkRun;              // network/driver/Run.hpp
struct NetworkEpoch;            // network/driver/Epoch.hpp

/**
 * @brief Write the per-station local-block audit for this epoch
 *        (@c network.trace_station_block; SPEC_event_harness §24.10).
 *
 * Runs in the measure-only trigger-instrumentation group, after the solve and
 * after @ref networkEmitStore, so @c ctx.counters.n_emitted already includes this
 * epoch's emission.  Writes on emitted epochs 1…20 and every 60th thereafter, once
 * per emission count.  An epoch whose solution was not emitted writes nothing and is
 * not counted, so the trace describes what the consumer received (invariant #4
 * applied to the trace's own cadence).
 *
 * For each station present in this epoch's solve:
 *  - @f$N_{rr}@f$ from @c NetworkEstimator::normals().stationLocalNormals, rebuilt
 *    from the cached @f$LDL^\top@f$ factor.  It therefore includes the folded
 *    station priors, which is what distinguishes the @c phase_leveling vehicle's
 *    block from the @c carry_invariant vehicle's.
 *  - One symmetric eigen-decomposition.
 *  - The ZWD column, located through @c StateManager::getTropoIndex and the
 *    assembly's @c h2local map.  A carry-gated flag is not used: it would emit no
 *    rows on a cold run, which would read as "the state does not exist".
 *  - The row census over @c A_r / @c w.  Rows are classified code or phase as the
 *    P1-4 post-fit pass does: a row touching a satellite phase-bias shared column or
 *    an ambiguity local column is phase; a row touching a satellite code-bias shared
 *    column is code.  SH pseudo-observation rows and coupling rows are not
 *    data and are excluded.
 *
 * Epoch form (generated by scripts/nom_ep1_cut.py): the run and the epoch
 * replace the explicit parameters; the definition binds each former parameter in its preamble,
 * under its former name.
 *
 * @param run The run (network/driver/Run.hpp). Written through non-const bindings: run.ctx (ctx),
 *            run.traces (traces). Read: run.config (config_), run.log (log), run.setup.stations
 *            (stations), run.engines.est (est).
 * @param ep The epoch (network/driver/Epoch.hpp), read only. Bindings: ep.se (se), ep.solve.x_s
 *           (x_s), ep.assembly.asm_res (asm_res), ep.assembly.est_index_of (est_index_of).
 *
 * The former parameters, by binding:
 *   - @p ctx = run.ctx: Run context (counters; nothing is read back into the solve).
 *   - @p config_ = run.config: Live config (unused; the toggle acts through the open trace).
 *   - @p log = run.log: Main logger (one debug line when a traced emission audits no station).
 *   - @p traces = run.traces: Trace streams; the sweep is a no-op while @c station_block_trace
 *     is closed.
 *   - @p se = ep.se: This epoch (week / TOW stamp).
 *   - @p stations = run.setup.stations: Per-station run state (name + @c StateManager for the
 *     labels).
 *   - @p est = run.engines.est: The solved estimator, read through const accessors only.
 *   - @p x_s = ep.solve.x_s: Solved shared vector (for @c reconstructStation).
 *   - @p asm_res = ep.assembly.asm_res: Per-station assembly blocks (@c A_r, @c w,
 *     @c h2local).
 *   - @p est_index_of = ep.assembly.est_index_of: Station index -> estimator index (<0 = not
 *     in the solve).
 */
void networkStationBlock(NetworkRun& run, const NetworkEpoch& ep);

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the ZP station-block trace.
 *
 * The ZP station-block trace: stations and blocks traced, skipped and failed.
 *
 * Gate: prints under `config_.getNetworkConfig().trace_station_block`.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkStationBlockRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

/// @brief Run-scope counters of StationBlock, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct StationBlockCounters {
    // Run-end accounting of the per-station local-block audit (SPEC_event_harness
    // §24.10, network.trace_station_block).  Invariant #4 needs all four: "0
    // rows under an armed trace" must be visible, and the two refusal counts say which
    // of the two ways the sweep can find nothing happened: no station reached the
    // audit at all, or the block itself could not be decomposed (a non-finite or
    // empty N_rr).
    long long sb_rows_written     = 0;   ///< station-epoch rows written to network_station_block.csv
    long long sb_epochs_written   = 0;   ///< traced epochs that wrote at least one row
    long long sb_stations_skipped = 0;   ///< station-epochs skipped: not in the solve, no usable assembly, or no local block
    long long sb_decomp_failed    = 0;   ///< station-epochs skipped: N_rr unavailable or its eigen-decomposition did not converge
    std::size_t sb_last_emitted   = 0;   ///< n_emitted of the last audited emission (one row set per emitted epoch, never per solve)
};

}  // namespace GnssProcessor
