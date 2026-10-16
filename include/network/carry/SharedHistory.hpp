#pragma once

/**
 * @file SharedHistory.hpp
 * @brief The shared history (@c network.carry_coupling_shared_history,
 *        docs/refound/SCL2_level_residue_under_the_clone_2026-10-06.md, sections 4, 5, 6 and 9).
 *
 * The clock clone (B1, filter/NetworkClockClone) marginalizes the clones out of every
 * carried row at the harvest, one station at a time: each row takes the clones' conditional
 * variance @f$r_C^\top B' r_C@f$ on its own diagonal (and within a station on the F3 block),
 * and the cross-station half of that shared term is dropped.  One epoch's dropped piece is
 * small, but the exact recursion accumulates those pieces into a cross-station correlation of
 * the carried ambiguities, and the per-station form keeps none of it: at long horizons the
 * satellite clock level freezes while its statement keeps shrinking.
 * The shared history keeps that term shared, in a bounded set of border columns:
 *
 * - The harvest (B1's row step, @c networkClockCloneHarvestRow): with
 *   @f$Y = [\text{clones}; \text{history}]@f$ the border's trailing nuisance columns, each
 *   carried row's coefficients @f$r_Y@f$ are regressed onto the free border through
 *   @f$K_Y = -J_{YY}^{-1}J_{YS'}@f$ (@c NetworkEstimator::cloneConditional over Y), and
 *   @f$r_Y@f$ is kept (@ref networkSharedHistoryNoteEntry records where its entry is
 *   written) instead of adding @f$r_Y^\top \Sigma_{Y\mid s} r_Y@f$ to the row.
 * - The compression (@ref networkSharedHistoryHarvestEnd, once every carried row of the
 *   network is harvested): @f$L = \mathrm{chol}(\Sigma_{Y\mid s})@f$, @f$F_r = r_Y^\top L@f$,
 *   the Gram @f$F^\top F = V \Lambda V^\top@f$, @f$k_t = \min(k\,n_{\text{clones}},
 *   \text{rank})@f$.  Each row's history coefficients become @f$F_r V_{k_t}@f$; the
 *   remainder @f$F_r V_{\text{rest}}@f$ is added to the row's variance and, within a
 *   station, to the F3 block; the remainder's cross-station part is dropped and counted.
 *   Exact when the shared term's rank is within the cap (@ref sharedHistoryCompress).
 * - The next epoch (@ref networkSharedHistoryRegister, @ref networkSharedHistoryFold): the
 *   referenced history directions are registered after every other column (after B1's
 *   clones), keyed @c "HIST|j", and folded by identity with the prior N(0, 1), independent of
 *   the border; the rows emit their history terms (@ref sharedHistoryRowTerms, the exact
 *   route of the coupling update).  A history column is read by the gate view and the S-basis
 *   completion as nuisance (marginalized with the clones), never emitted, never stashed and
 *   never snapshotted.
 * - Events: a row of an older compression (a stale station, card 1d's route, a history
 *   column not registered this epoch) marginalizes its history terms into its variance
 *   (counted); a re-stash without history terms invalidates the old coefficients; a
 *   snapshot writes each row's marginal, @c var plus the squared norm of its coefficients.
 *
 * Every function is a no-op when the knob is off, which keeps the off path byte-identical.
 */

#include "network/carry/ClockClone.hpp"     // ClockCloneHarvest
#include "network/carry/CouplingExact.hpp"  // CouplingExactCounters
#include "network/solve/Estimator.hpp"
#include "network/driver/RunTypes.hpp"       // LocalCarry, EpochWorkspace

#include <Eigen/Dense>

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

struct NetworkRunContext;   // network/state/RunContext.hpp defines it as a struct

/// The largest @c network.carry_coupling_shared_history_rank the loader accepts.
inline constexpr int kSharedHistoryRankMax = 8;
/// A direction of the shared term whose Gram eigenvalue is at or below this fraction of the
/// largest is empty (the unit fixture's tolerance, design section 9).
inline constexpr double kSharedHistoryRankTol = 1e-14;
/// Two continuous-GPS stamps within this many seconds are the same epoch (the exact coupling update's test).
inline constexpr double kSharedHistoryStampTol = 1e-6;

/// The shared history's carried state (@c ctx.carry.shx).  The compression's stamp and width
/// persist to the next epoch; @c col and @c on are rebuilt at every registration.
struct SharedHistoryState {
    bool   valid{false};                ///< a compression is held (@c stash_s, @c kt)
    double stash_s{0.0};                ///< continuous-GPS s of the harvest that compressed it
    int    kt{0};                       ///< its width (history directions kept)
    std::vector<Eigen::Index> col;      ///< this epoch: history index j -> its column (-1 = not registered)
    bool   on{false};                   ///< this epoch: at least one history column is registered

    /// Forget the previous epoch's columns (the compression is kept).
    void clearEpoch() {
        col.clear();
        on = false;
    }
};

/// The shared history's run-end telemetry (@c ctx.counters.shx; standing invariant 4).  The rows'
/// terms at the inject are in @c CouplingExactCounters (@c rows_hist and siblings).
struct SharedHistoryCounters {
    long long epochs = 0;              ///< epochs with history columns registered
    long long registered = 0;          ///< history columns registered, summed over epochs
    long long registered_max = 0;      ///< the most registered in one epoch
    long long unreferenced = 0;        ///< kept directions no carried row referenced (not registered)
    long long gen_mismatch = 0;        ///< epochs whose held compression was not of the stash the rows' epoch belongs to (nothing registered)
    long long fold_priors = 0;         ///< N(0, 1) priors folded, summed
    long long compress_epochs = 0;     ///< harvests that compressed
    long long compress_failed = 0;     ///< harvests whose compression failed (the rows took B1's per-station form)
    long long chol_fallback = 0;       ///< compressions whose Sigma_Y|s needed the clamped eigen square root
    long long noreg_epochs = 0;        ///< harvests with history registered but no regression (B1 dropped the terms)
    long long kt_sum = 0;              ///< kept width, summed over compressions
    long long kt_max = 0;              ///< the widest compression
    long long kt_capped = 0;           ///< compressions whose numerical rank exceeded the cap
    long long rank_sum = 0;            ///< numerical rank of the shared term, summed
    long long rank_max = 0;            ///< its largest value
    long long ny_sum = 0;              ///< width of Y (clones + history) at the compressions, summed
    long long rows_compressed = 0;     ///< carried rows that entered a compression
    long long rows_lost = 0;           ///< noted rows whose entry was gone (or re-stamped) at the compression
    long long rows_unnoted = 0;        ///< rows with Y terms whose entry was never noted (their Y variance is lost)
    long long rows_fallback = 0;       ///< rows given B1's per-station term after a failed compression
    long long f3_pairs = 0;            ///< within-station F3 pairs that took the remainder's cross term
    double    rem_var_sum = 0.0;       ///< summed remainder variance added to the rows (m^2)
    std::vector<double> kept_share;    ///< per compression: kept eigenvalue mass over the total
    std::vector<double> rem_share;     ///< per compression: remainder mass over the total (the fixture's "dropped mass")
    std::vector<double> cross_share;   ///< per compression: Frobenius mass of the dropped cross-station remainder over that of the whole shared term
    // Amendment A1 (build X2b): the smoothed-hold clone's rows in the history, form p (counted only
    // when the smoothed hold is armed).
    long long hold_pending = 0;        ///< hold rows whose Y term was kept pending at the update
    long long hold_projected = 0;      ///< hold rows projected onto the kept directions at the harvest's end
    long long hold_proj_failed = 0;    ///< pending hold rows whose projection failed with a compression formed this epoch (a width mismatch, a non-finite result): B1's per-row term added
    long long hold_nocomp_end = 0;     ///< pending hold rows at a harvest's end that formed no compression (no regression, no compressed row, a failed compression): the per-row term
    long long hold_nocomp_unreached = 0; ///< pending hold rows whose epoch never reached the harvest's end (no carry harvest that epoch): the per-row term at the re-harvest or the next update
    double    hold_fallback_var = 0.0; ///< the per-row variance the three kinds above took (m^2)
    double    hold_rem_var = 0.0;      ///< remainder variance added to projected hold rows (m^2)
    long long hold_rows_hist = 0;      ///< hold rows that carried history terms into an update on registered columns
    long long hold_terms_hist = 0;     ///< those terms
    long long hold_rows_stale = 0;     ///< hold rows that marginalized history terms (a stale generation, a column not registered)
    long long hold_terms_stale = 0;    ///< those terms
    double    hold_stale_var = 0.0;    ///< their squared coefficients added to the rows' variance (m^2)
    std::vector<double> hold_kept_share;   ///< per projected hold row: kept share of its shared term
    std::vector<double> hold_rem_share;    ///< per projected hold row: remainder share
};

/// The carry map a noted entry was written to (@ref networkSharedHistoryNoteEntry).
enum class SharedHistoryMap : int {
    Amb = 0,   ///< @c local_carry, key (station, satellite, signal)
    Zwd,       ///< @c zwd_carry, key station
    Iono,      ///< @c iono_blk_carry, key (station, satellite)
    RxClk,     ///< @c rx_clk_carry, key (station, kind)
    RxCb,      ///< @c rx_cb_carry, key (station, code-bias key)
    Pc,        ///< @c pc_carry, key (station, satellite)
};

/// One epoch's harvest-side record (@ref networkSharedHistoryHarvestBegin).
struct SharedHistoryHarvest {
    bool on{false};   ///< the knob is armed and B1's regression is available
    /// Where one harvested row's entry was written: its carry map and identity key, and the
    /// (estimator station, local column) its Y coefficients are keyed by in
    /// @c ClockCloneHarvest::rc.  An identity, not a pointer: entries can be erased between
    /// their write and the compression.
    struct Handle {
        SharedHistoryMap map{SharedHistoryMap::Amb};
        int          sta{-1};
        std::string  sat;
        std::string  sig;
        std::size_t  est_idx{0};
        Eigen::Index col{-1};
    };
    std::vector<Handle> handles;
};

/// The compression of one epoch's shared term (@ref sharedHistoryCompress).
struct SharedHistoryCompression {
    bool ok{false};              ///< formed (false: Sigma_Y|s or the Gram could not be factored)
    bool chol_fallback{false};   ///< Sigma_Y|s took the clamped eigen square root
    int  rank{0};                ///< numerical rank of the shared term
    int  kt{0};                  ///< directions kept, min(cap, rank)
    Eigen::MatrixXd coef;        ///< rows x kt: the rows' history coefficients
    Eigen::MatrixXd rem;         ///< rows x (nY - kt): the remainder's factor
    double total{0.0};           ///< trace of the shared term (m^2): the Gram's eigenvalue sum
    double kept{0.0};            ///< its kept share (m^2)
    double remainder{0.0};       ///< its remainder (m^2)
    double frob_total{0.0};      ///< squared Frobenius norm of the shared term (m^4)
    double cross{0.0};           ///< squared Frobenius norm of the remainder's cross-group blocks, dropped (m^4)
    Eigen::MatrixXd L;           ///< nY x nY: the factor, @f$L L^\top = \Sigma_{Y\mid s}@f$ (amendment A1 projects on it)
    Eigen::MatrixXd V;           ///< nY x nY: the Gram's eigenvectors, decreasing; the first @c kt are the kept directions
};

/// One row projected onto a compression's kept directions (@ref sharedHistoryProject).
struct SharedHistoryProjection {
    bool ok{false};              ///< formed (false: a width mismatch or a non-finite result)
    Eigen::VectorXd coef;        ///< kt: the row's history coefficients @f$(F_T V_{k_t})^\top@f$ (m)
    double rem{0.0};             ///< @f$\lVert F_T V_{\text{rest}}\rVert^2@f$, the remainder added to the row's variance (m^2)
    double total{0.0};           ///< @f$\lVert F_T\rVert^2 = r^\top \Sigma_{Y\mid s} r@f$, the row's whole shared term (m^2)
};

/**
 * @brief Compress a shared conditional term (mode 6 of the unit network).
 *
 * The rows' joint term is @f$R\,\Sigma\,R^\top@f$ with @f$R@f$ the rows' coefficients on Y.
 * With @f$L L^\top = \Sigma@f$ and @f$F = R L@f$, the leading right singular directions of
 * @f$F@f$ (eigenvectors of the Gram @f$F^\top F@f$, in decreasing order) up to @p cap become
 * the history (@c coef = @f$F V_{k_t}@f$, so @f$\text{coef}\,\text{coef}^\top@f$ is the kept
 * term) and the rest the remainder (@c rem = @f$F V_{\text{rest}}@f$).  The caller keeps the
 * remainder's within-group blocks and drops the cross-group ones; when the rank is within
 * the cap the remainder vanishes and the compression is exact.
 * @param R      rows x nY coefficients (m / m).
 * @param Sigma  nY x nY: @f$\Sigma_{Y\mid s}@f$ (m^2), symmetrized here.
 * @param cap    the most directions kept (>= 0).
 * @param group  per row: its station (the cross-group mass is measured over it).
 * @return the compression; @c ok false (and nothing else set) on a factor failure.
 */
SharedHistoryCompression sharedHistoryCompress(const Eigen::MatrixXd& R,
                                               const Eigen::MatrixXd& Sigma, int cap,
                                               const std::vector<std::size_t>& group);

/**
 * @brief Project one row onto a compression's kept directions (amendment A1, "form p").
 *
 * With @f$F_T = r^\top L@f$ the row's shared factor, its history coefficients are
 * @f$F_T V_{k_t}@f$ and its remainder @f$\lVert F_T V_{\text{rest}}\rVert^2@f$; the two
 * sum to @f$r^\top L L^\top r@f$ (V is orthonormal), so the row's own variance is kept and
 * only the remainder's covariance with the other rows is dropped.  The directions are not
 * steered by the row (the compression is the coupling rows' alone).
 * @param r   nY: the row's coefficients on Y (m / m).
 * @param L   nY x nY: the compression's factor (@c SharedHistoryCompression::L).
 * @param V   nY x nY: its directions, the kept ones first (@c SharedHistoryCompression::V).
 * @param kt  the directions kept (0 <= kt <= nY).
 * @return the projection; @c ok false on a width mismatch or a non-finite result.
 */
SharedHistoryProjection sharedHistoryProject(const Eigen::VectorXd& r, const Eigen::MatrixXd& L,
                                             const Eigen::MatrixXd& V, int kt);

/// @return Whether @p lc holds history coefficients of the compression it was last stashed
///         at: non-empty and @c shx_stash_s equal to @c last_seen_s.
bool sharedHistoryCoefValid(const LocalCarry& lc);

/// @return The marginal variance of @p lc: @c var plus the squared norm of
///         its valid history coefficients, @c var otherwise.  What the snapshot writes.
double sharedHistoryMarginalVar(const LocalCarry& lc);

/**
 * @brief The history terms of one coupling row at the inject.
 *
 * With @p emit (the exact coupling route) and the row's coefficients of the compression the
 * registered columns belong to, each nonzero coefficient is added to @p emit on its
 * column (the right-hand side does not move: the prior mean is 0).  Otherwise (card 1d's
 * route, @p emit null; a stale generation; a column not registered) the coefficient's
 * square is added to @p marg_var: the term is marginalized at its harvest variance, the
 * conservative direction.  Counted in @p X.  No-op on a row with no valid coefficient.
 */
void sharedHistoryRowTerms(const SharedHistoryState& shx, const LocalCarry& lc,
                           std::map<Eigen::Index, double>* emit, double& marg_var,
                           CouplingExactCounters& X);

/**
 * @brief Register this epoch's history columns, after every other column
 *        (call it after @c networkClockCloneRegister).
 *
 * Armed, with a compression held of the stash the rows' epoch belongs to
 * (@c ctx.carry.cxu_stash_s; otherwise nothing is registered and the rows marginalize):
 * each kept direction that at least one carried row references (a valid nonzero
 * coefficient of that compression in any of the six carry maps) gets column
 * @c "HIST|j"; an unreferenced one is counted and not registered.  No-op off.
 */
void networkSharedHistoryRegister(NetworkRunContext& ctx, NetworkEstimator& est);

/**
 * @brief Fold the registered history columns' priors: N(0, 1) on each,
 *        independent of the border, by identity.  Call after @c networkClockCloneFoldEnd.
 *        Captured in @c ws.cap_shx with @p within_epoch_armed.  No-op off.
 */
void networkSharedHistoryFold(NetworkRunContext& ctx, EpochWorkspace& ws, NetworkEstimator& est,
                              bool within_epoch_armed);

/// Re-fold the captured history priors on the within-epoch replay's fresh arrowhead.
/// No-op off and when nothing was captured.
void networkSharedHistoryReplayFold(const NetworkRunContext& ctx, const EpochWorkspace& ws,
                                    NetworkEstimator& est);

/**
 * @brief Open the harvest's history record.  Inert (@c on false) off and when
 *        B1's regression is not available this epoch (B1 then drops the rows' Y terms, as
 *        it drops its clone terms; counted when history was registered).
 * @param h B1's harvest record, formed by @c networkClockCloneHarvestBegin over Y.
 */
SharedHistoryHarvest networkSharedHistoryHarvestBegin(NetworkRunContext& ctx,
                                                      const ClockCloneHarvest& h);

/// Note where a harvested row's entry was written: call after each of the six
/// carry-map writes of the harvest.  No-op when @p H is inert.
void networkSharedHistoryNoteEntry(SharedHistoryHarvest& H, std::size_t est_idx, Eigen::Index col,
                                   SharedHistoryMap map, int sta, const std::string& sat = "",
                                   const std::string& sig = "");

/**
 * @brief The compression, once every carried row of the network is harvested:
 *        the rows' Y coefficients through @ref sharedHistoryCompress at the cap
 *        @c rank x the clones registered this epoch, each noted entry's history
 *        coefficients and stamp written, its remainder added to its variance, the
 *        within-station remainder added to the F3 blocks B1 registered, the cross-station
 *        remainder dropped and counted, and the compression's stamp and width stored for
 *        the next epoch.  On a failed compression every row takes B1's per-station term
 *        (counted).  Call in place of @c networkClockCloneHarvestEnd's role (that function
 *        returns at once under X2).  No-op when @p H is inert.
 * @param now_s the harvest's continuous-GPS second (the entries' @c last_seen_s).
 */
void networkSharedHistoryHarvestEnd(SharedHistoryHarvest& H, const ClockCloneHarvest& h,
                                    NetworkRunContext& ctx, double now_s);

/**
 * @brief Amendment A1 (build X2b): the smoothed-hold clone's rows at the harvest's end.
 *
 * Every live clone row (the smoothed-hold clone's clock rows; the smoothed-hold block member's clock and phase-visible rows) that
 * kept its Y coefficients pending at this epoch's update (@c PbHoldCloneShx::rc, stamped with
 * @c n_loop_epochs) is projected onto @p X's kept directions (@ref sharedHistoryProject): its
 * history coefficients are stamped @p now_s (emitted with the history columns at the next
 * update) and its remainder is added to its variance.  With @p X null or not formed, or a
 * width mismatch, the row takes the per-row term instead (B1's form, counted as a projection
 * failure).  The rows never steer the compression.  No-op unless the shared history and the
 * smoothed hold are both armed.
 * @param X     this epoch's compression, or null when none was formed.
 * @param now_s the harvest's continuous-GPS second (the compression's stamp).
 */
void networkSharedHistoryProjectHoldRows(NetworkRunContext& ctx, const SharedHistoryCompression* X,
                                         double now_s);

/**
 * @brief Amendment A1: give every hold row still pending at epoch @p ep the per-row term
 *        (B1's form, counted as a projection failure).  Called at the hold's re-harvest,
 *        after the harvest's end, so a row whose epoch never reached the compression (an
 *        early-returning harvest) keeps its marginal; a no-op when every row was projected.
 */
void networkSharedHistoryFinalizeHoldRows(NetworkRunContext& ctx, long long ep);

/// The run-end line of the shared history and its INERT warning (invariant 4).  No-op off.
void networkSharedHistoryRunEnd(const NetworkRunContext& ctx,
                                  const std::shared_ptr<spdlog::logger>& log);

}  // namespace GnssProcessor
