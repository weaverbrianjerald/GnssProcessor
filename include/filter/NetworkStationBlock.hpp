#pragma once
/**
 * @file NetworkStationBlock.hpp
 * @brief SPEC_event_harness §24.10 card ZP — the per-station LOCAL-BLOCK audit
 *        (@c network.trace_station_block) and the pure algebra behind it.
 *
 * §24.9 measured a contradiction the tree had no instrument for: a station whose
 * own post-fit code residuals are 0.3–0.55 m reports a reconstructed ZWD of
 * −18 ± 4.5 m, and 47 of its 130 rows (36 %) carry zero redundancy.  A formal σ
 * of 4.5 m on a column whose partial is the wet mapping function against ten
 * code rows of σ 0.4 m is not possible unless the column's EFFECTIVE information
 * — what is left of it after every other local state has been eliminated — is
 * orders below its raw @f$\sum w\,m_\text{wet}^2@f$.  This file measures exactly
 * that, and names the local directions the block cannot resolve.
 *
 * The audited object is the station's LOCAL normal block
 * @f[
 *     N_{rr} \;=\; A_r^\top W A_r \;+\; \text{(folded station priors)} ,
 * @f]
 * with the SHARED states held at their solved values @f$x_s@f$ — the same
 * conditioning @ref NetworkEstimator::reconstructStation applies when it forms
 * @f$x_r=N_{rr}^{-1}\!\left(u_r-N_{rs}x_s\right)@f$.  It is NOT the station's
 * stand-alone normal matrix (which would have to carry the satellite clocks and
 * biases as unknowns) and it is not the reduced shared system.
 *
 * MEASURE-ONLY.  @ref networkStationBlock runs in the trigger-instrumentation
 * group AFTER the solve, reads @ref NetworkEstimator through const accessors,
 * and writes one CSV row per station per traced epoch.  Nothing it touches feeds
 * @c x, @c N or the order of operations, armed or not (standing invariant #6).
 *
 * The split below is deliberate: @ref StationBlockAudit holds the whole
 * numerical content as pure functions over @c (N_rr, A_r, w, labels), so the
 * rank / span / effective-information / redundancy algebra is unit-testable
 * without a network run, and the seam itself is only label plumbing and CSV.
 */

#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkRunTypes.hpp"
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
struct NetworkRunContext;   // app/NetworkRunContext.hpp — included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp — included in the .cpp only

/**
 * @brief The pure algebra of the §24.10 station-block audit — no estimator, no
 *        config, no I/O.
 *
 * Every function here is a function of the local block @f$N_{rr}@f$ (symmetric,
 * @c n_local × @c n_local) and, where a row census is involved, the station's
 * local design rows @f$A_r@f$ with their information weights @f$w@f$.  The
 * NON-NULL subspace is defined once, by @ref kRankTolRel, and used consistently
 * for the rank, the condition number, the pseudo-inverse in the effective
 * information and the pseudo-inverse in the redundancy numbers — a block this
 * close to singular gives different answers under different tolerances, so the
 * card fixes one.
 */
namespace StationBlockAudit {

/// Relative eigenvalue floor defining the NON-NULL subspace: an eigenvalue is
/// treated as structurally zero when @f$\lambda \le @f$ @c kRankTolRel times the
/// largest.  Fixed by the card so rank, condition number, effective information
/// and redundancy all speak about the same subspace.
inline constexpr double kRankTolRel = 1e-9;

/// A row's redundancy is reported as ZERO (the row is UNCHECKABLE — fully
/// absorbed by free local parameters) below this.  §24.9's census bar.
inline constexpr double kZeroRedundancy = 1e-6;

/**
 * @brief The spectral half of the audit: rank, condition number and the three
 *        smallest eigenpairs with the local states that span them.
 */
struct Spectrum {
    bool        ok{false};        ///< The decomposition converged and @f$N_{rr}@f$ was usable.
    Eigen::Index n_local{0};      ///< Block dimension.
    Eigen::Index rank{0};         ///< @f$\#\{\lambda > \text{kRankTolRel}\cdot\lambda_\max\}@f$.
    double      cond{0.0};        ///< @f$\lambda_\max/\lambda_\min@f$ over the NON-NULL subspace (0 when rank < 1).
    /// The three SMALLEST eigenvalues, ascending (unused slots are NaN).
    std::array<double, 3> ev_val{{0.0, 0.0, 0.0}};
    /// For each, the local states carrying its squared loading, as up to three
    /// @c "label=share" items joined by @c '|', truncated once the cumulative
    /// share reaches 0.90 (see @ref spanOf).  Empty for an unused slot.
    std::array<std::string, 3> ev_span{{{}, {}, {}}};
    /// Eigenvalues ascending (full spectrum) — retained so callers that already
    /// paid for the decomposition can reuse it.
    Eigen::VectorXd eigenvalues;
    /// Eigenvectors, column @c k matching @c eigenvalues(k).
    Eigen::MatrixXd eigenvectors;
};

/**
 * @brief Format one eigenvector's span as a compact @c "label=share" list.
 *
 * The share of local state @c k is @f$v_k^2@f$ (the eigenvector is unit-norm, so
 * the shares sum to one).  States are listed in DESCENDING share; the list stops
 * at three items or once the cumulative share reaches @p cover, whichever comes
 * first.  Items are joined by @c '|' and shares by @c '=' so the field survives
 * a CSV column and so a label that itself contains colons
 * (@c "amb:G24:L5X") stays unambiguous.
 *
 * @param v      Eigenvector (length @c n_local).
 * @param labels Per-local-column kind labels (same length; a short vector yields
 *               @c "col<k>" for the missing entries).
 * @param cover  Cumulative squared-loading share at which to stop (0.90 for the card).
 * @return The formatted span, or an empty string for an empty vector.
 */
std::string spanOf(const Eigen::VectorXd& v, const std::vector<std::string>& labels,
                   double cover = 0.90);

/**
 * @brief Decompose the local block: rank, condition number, three smallest eigenpairs.
 *
 * @param Nrr    The local normal block (symmetric, @c n_local × @c n_local).  Its
 *               LOWER triangle is read (@c Eigen::SelfAdjointEigenSolver's contract).
 * @param labels Per-local-column kind labels for the spans.
 * @return A @ref Spectrum with @c ok=false when the block is empty, non-finite,
 *         or the decomposition failed to converge.
 */
Spectrum spectrum(const Eigen::MatrixXd& Nrr, const std::vector<std::string>& labels);

/**
 * @brief The ZWD column's EFFECTIVE information — what is left of it once every
 *        OTHER local state has been eliminated.
 *
 * The Schur complement of column @p col against all other local columns,
 * @f$N_{zz}-N_{zo}N_{oo}^{-1}N_{oz}@f$, is identically @f$1/(N_{rr}^{-1})_{zz}@f$.
 * The identity is used directly, with the pseudo-inverse taken on the non-null
 * subspace of the SAME decomposition the rank is read from:
 * @f[
 *     \text{info}_\text{eff}(z) \;=\;
 *     \Big(\textstyle\sum_{\lambda_k>\epsilon} V_{zk}^2/\lambda_k\Big)^{-1}.
 * @f]
 * Cheaper than a second decomposition of @f$N_{oo}@f$ and, unlike an explicit
 * elimination, numerically well-behaved when @f$N_{oo}@f$ is itself singular.
 *
 * @param sp  A converged @ref Spectrum of the block.
 * @param col The column of interest (the ZWD column for the card).
 * @return The effective information, or 0.0 when @p col carries NO mass in the
 *         non-null subspace (a column the block cannot see at all) or @p sp is
 *         not usable.
 */
double effectiveInformation(const Spectrum& sp, Eigen::Index col);

/**
 * @brief Per-row redundancy numbers on the LOCAL block.
 *
 * @f[
 *     r_i \;=\; 1 - w_i\,h_i^\top N_{rr}^{+} h_i ,
 * @f]
 * with @f$h_i@f$ the row's LOCAL partials (row @c i of @p A_r) and the
 * pseudo-inverse taken on the non-null subspace of @p sp.  @f$r_i\approx 0@f$
 * says the row is fully absorbed by free local parameters — it changes nothing
 * it does not also change the estimate of, so no residual of it is CHECKABLE and
 * no QC screen, however armed, can see a fault on it.
 *
 * This is the LOCAL-block redundancy: the shared states are held at their solved
 * values, which is the frame the reconstruction and the ZWD trace live in.  A
 * row can be locally unchecked and still constrain the network.
 *
 * @param sp  A converged @ref Spectrum of the block.
 * @param A_r Station's local design rows (@c m × @c n_local).
 * @param w   Per-row information weights (length @c m; a short vector is read as 1.0).
 * @return One redundancy per row (empty when @p sp is unusable or the shapes
 *         disagree).  Values are NOT clamped — a small negative is round-off and
 *         reads the same as zero against @ref kZeroRedundancy.
 */
Eigen::VectorXd redundancy(const Spectrum& sp, const Eigen::MatrixXd& A_r,
                           const Eigen::VectorXd& w);

}  // namespace StationBlockAudit

/**
 * @brief SPEC_event_harness §24.10 card ZP: write the per-station LOCAL-BLOCK
 *        audit for this epoch (@c network.trace_station_block).
 *
 * Runs in the measure-only trigger-instrumentation group, AFTER the solve and
 * after @ref networkEmitStore, so @c ctx.counters.n_emitted already carries this
 * epoch's emission.  Writes on EMITTED epochs 1…20 and every 60th thereafter;
 * an epoch whose solution never reached the wire writes nothing and is not
 * counted (the audit is of what the consumer received, invariant #4's
 * "counters are the telemetry" applied to the trace's own cadence).
 *
 * Per station present in this epoch's solve: @f$N_{rr}@f$ via
 * @c NetworkEstimator::normals().stationLocalNormals (rebuilt from the cached
 * @f$LDL^\top@f$ factor, so the folded station priors ARE included — which is
 * precisely what distinguishes the @c phase_leveling vehicle's block from the
 * @c carry_invariant vehicle's), one symmetric eigen-decomposition, the ZWD
 * column located through @c StateManager::getTropoIndex + the assembly's
 * @c h2local map (never through a carry-gated flag — a carry-gated lookup emits
 * no rows on a cold run and reads as "the state does not exist"), and the row
 * census over @c A_r / @c w.  Rows are classified code vs phase by the
 * measurement model, exactly as the P1-4 post-fit pass does: a row touching a
 * satellite PHASE-bias shared column, or an AMBIGUITY local column, is phase; a
 * row touching a satellite CODE-bias shared column is code; SH pseudo-observation
 * rows and Card-1 coupling rows are not data and are excluded.
 *
 * @param ctx          Run context (counters; nothing is read back into the solve).
 * @param config_      Live config (the toggle is expressed through the open trace).
 * @param log          Main logger (open-failure / decomposition warnings only).
 * @param traces       Trace streams — the sweep is a no-op while
 *                     @c station_block_trace is closed.
 * @param se           This epoch (week / TOW stamp).
 * @param stations     Per-station run state (name + @c StateManager for the labels).
 * @param est          The SOLVED estimator — read through const accessors only.
 * @param x_s          Solved shared vector (for @c reconstructStation).
 * @param asm_res      Per-station assembly blocks (@c A_r, @c w, @c h2local).
 * @param est_index_of Station index -> estimator index (<0 = not in the solve).
 */
void networkStationBlock(NetworkRunContext& ctx, const GnssConfig& config_,
                         const std::shared_ptr<spdlog::logger>& log,
                         NetworkRunTraces& traces, const SyncedEpoch& se,
                         const std::vector<NetStation>& stations,
                         NetworkEstimator& est,
                         const Eigen::VectorXd& x_s,
                         const std::vector<StationAssembly>& asm_res,
                         const std::vector<int>& est_index_of);

}  // namespace GnssProcessor
