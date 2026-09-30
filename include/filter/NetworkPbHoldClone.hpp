#pragma once

/**
 * @file NetworkPbHoldClone.hpp
 * @brief The smoothed phase-bias hold's clock clone (card SMH,
 *        @c network.emit_pb_hold_smoothed): c(t0|t) carried as a coupling row with no
 *        observation rows.
 *
 * The smoothed member of card LAH's hold family keeps
 * @f$(C - P_j)(t) = (c - p_j)(t_0|t_0) + \Phi(t)@f$ with @f$\Phi = c(t|t) - c(t_0|t)@f$, the
 * clock's smoothed motion since the generation start (card SMH sections 1 and 10).  The
 * fixed-point smoothed clock @f$c(t_0|t)@f$ is carried per hold key as a row in the
 * coupling-row convention
 * @f$x_0 = v - \sum_j M_j (x_j - \hat s_j)@f$ with residual variance @f$\sigma^2@f$:
 *
 * - Birth: at the stash of the generation's first epoch, one term on the satellite's
 *   clock with @f$M = -1@f$ (so @f$x_0 = c@f$), @f$v@f$ the stashed clock,
 *   @f$\sigma^2 = 0@f$, stamped with the stash time.
 * - Time update: each later epoch the row takes card CTU's exact route
 *   (@ref couplingExactRow over the epoch's @ref CouplingExactEpoch): S terms by the one
 *   rule, D terms (departed, reinit, re-referenced) re-attributed through P, the
 *   clock-family deficit onto the stash's pinned pivot; a term on a key outside the dense
 *   block takes the fold's own kappa (exact for a diagonal key) or is marginalized.
 * - Value: the row has no observations, so the conditional on the border is unchanged by
 *   the measurement update and @f$x_0(t|t) = x_0(t-1|t-1) - \sum m_s [\hat x_s(t|t) -
 *   \mathrm{prior}_s(t)]@f$, with @f$\mathrm{prior}_s@f$ the prior mean the solve used (the
 *   stash value after any in-place frame transform).  A pinned column contributes no
 *   revision.  A declared frame step therefore never steps the clone.
 * - Re-harvest: at the stash the row is re-expressed on the new stash's keys.
 * - No fallback: a clone that would take card 1d's route (an unfactored epoch, a stale
 *   stamp, its own clock outside the dense block) is destroyed, never shrunk.
 * - Transfer: the hold writes card LAH's transfer minus the clone's frame-free passed
 *   motion (@ref PbHoldClone::phi), so the members of the hold family share LAH's frame
 *   and its common-mode median and differ only by the passed motion (card SMH section 10).
 * - Lifetime: the parent clock's anchor engagement (card SMH section 9).  A LOOSE clock
 *   folds at the legacy rate, a datum device rather than a statement about motion, so a
 *   clone is born only at a stash at which card Q3's anchor holds its parent clock, and
 *   dies at the epoch the anchor withdraws the clock (a trip, a gap, a retired frame) or
 *   folds it at anything but the anchored variance.  The key carries card LAH's transfer
 *   in between, and the birth is queued again for the same generation.
 *
 * The row form equals the first stage of a clone column in the estimator (a zero-noise
 * column informed only by its prior correlation with the border).
 */

#include "filter/NetworkCouplingExact.hpp"
#include "filter/NetworkEstimator.hpp"

#include <Eigen/Dense>

#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

struct NetworkRunContext;   // app/NetworkRunContext.hpp defines it as a struct
struct EpochWorkspace;      // filter/NetworkRunTypes.hpp

/// One term of a clone row: the coupling-row convention of @c CoupledShared.
struct PbHoldCloneTerm {
    SharedParamKind kind{SharedParamKind::Clock};  ///< shared parameter kind
    std::string sat;          ///< satellite id
    std::string signal;       ///< signal id (PhaseBias / CodeBias; empty for Clock)
    double coeff{0.0};        ///< @f$M_j@f$
    double s_prev{0.0};       ///< the stashed value of the key at the row's stamp (m)
    double p_prev{0.0};       ///< the stashed variance (m^2); 0 = datum-pinned
    Eigen::Index col{-1};     ///< this epoch's shared column (set by the update; -1 = none)
};

/// Why a clone died.  After @c Withdrawn and @c Disengaged the key carries card LAH's
/// transfer until the anchor holds again and a new clone is born in the same generation;
/// after any other cause it carries LAH's transfer for the rest of its generation.
enum class PbHoldCloneDeath : int {
    None = 0,
    Unfactored,     ///< the epoch's exact inputs were not built or did not factor
    Stale,          ///< the row's stamp is not the stash the epoch's P belongs to
    ParentOutside,  ///< the satellite's own clock term fell outside the dense block
    RowFailed,      ///< the exact row came out non-finite
    NoStash,        ///< no dense stash at the epoch's end (the row cannot be re-harvested)
    Missed,         ///< alive at the stash but not updated this epoch
    Unlinked,       ///< every dense term was re-attributed away (no folded key carries the row)
    Withdrawn,      ///< card Q3's guard decoupled the parent clock this epoch (a trip, a gap, a retired frame); the birth is queued again
    Disengaged,     ///< the parent clock's fold this epoch was not the anchored one (LOOSE, or no prior folded); the birth is queued again
    kCount
};

/// The clone of one hold key.
struct PbHoldClone {
    std::string sat;                   ///< satellite id
    std::string signal;                ///< the hold key's signal
    unsigned    generation{0};         ///< the hold generation the clone belongs to
    bool        alive{false};
    PbHoldCloneDeath death{PbHoldCloneDeath::None};
    double      t0_s{0.0};             ///< birth stash time (continuous GPS s)
    double      stamp_s{0.0};          ///< the stash the terms are expressed on
    double      value{0.0};            ///< @f$x_0@f$ at the stamp (m, solve frame)
    double      c_ref{0.0};            ///< @f$c(t_0|t_0)@f$ (m, solve frame)
    double      var{0.0};              ///< the row's residual variance @f$\sigma^2@f$ (m^2)
    std::vector<PbHoldCloneTerm> terms;
    long long   updated_ep{-1};        ///< n_loop_epochs of the last update
    long long   queued_ep{-1};         ///< n_loop_epochs at which the birth was queued (a pending clone)
    bool        rebirth{false};        ///< queued by a disengagement within the generation, not by its start
    // Instruments (card SMH section 4), valid after an update.
    double      W{0.0};                ///< sum of the fold inflations applied to the parent clock (m^2)
    bool        w_valid{true};         ///< false once the parent left S (reinit / departure)
    /// The passed motion since birth, frame-free (card SMH section 10): the sum over the
    /// clone's epochs of the parent clock's step net of declared frame steps (its posterior
    /// minus the prior mean the solve folded) minus the revision of @f$x_0@f$.  Without a
    /// frame step it equals @f$\hat c(t|t) - x_0(t|t)@f$.  The hold writes card LAH's
    /// transfer minus this, so the smoothed and the phase-clock members share one frame.
    double      phi{0.0};
    bool        phi_ok{false};         ///< the parent clock had a column this epoch
    double      vpost{0.0};            ///< posterior variance of @f$c(t) - c(t_0)@f$ (m^2)
    // The G3 prediction's inputs (card SMH section 5), from the stash at birth; NaN when the
    // clock or the key's phase bias has no dense row (a pinned clock).
    double      pd0{std::numeric_limits<double>::quiet_NaN()};  ///< stated variance of D = c - p_j at birth (m^2)
    double      f0{std::numeric_limits<double>::quiet_NaN()};   ///< the clock's share of D at birth, (P_cc - P_cp) / P_D
};

/// The epoch-level inputs of @ref pbHoldCloneAdvance, built once per epoch.
struct PbHoldCloneInputs {
    const CouplingExactEpoch* xe{nullptr};
    std::vector<PbHoldCloneTerm> desc_of_p;   ///< P row -> its key (kind, sat, signal)
    Eigen::VectorXd post_s;                   ///< S index -> this epoch's posterior value (m)
    Eigen::VectorXd prior_s;                  ///< S index -> the prior mean the solve used (m)
    std::map<char, std::string> pivot_sat;    ///< constellation -> the stash's pinned clock
    /// Column of a key this epoch (-1 = not in the layout).
    std::function<Eigen::Index(const PbHoldCloneTerm&)> col_of;
    /// Posterior value of a column this epoch.
    std::function<double(Eigen::Index)> post_of_col;
    /// Prior mean the solve used for a key outside the dense block (NaN = unknown).
    std::function<double(const PbHoldCloneTerm&)> prior_of;
};

/// Outcome of one time-and-measurement step of a clone row.
struct PbHoldCloneStep {
    bool ok{false};
    PbHoldCloneDeath death{PbHoldCloneDeath::None};
    double value{0.0};                       ///< @f$x_0(t|t)@f$ (m)
    double var{0.0};                         ///< the new @f$\sigma^2@f$ (m^2)
    std::vector<PbHoldCloneTerm> terms;      ///< the new coefficients, with columns
    double dW{0.0};                          ///< the fold inflation of the parent clock this epoch (m^2)
    bool   parent_in_s{false};               ///< the parent clock took the S route
    int    n_s{0};                           ///< terms that took the S route
    int    n_d{0};                           ///< terms re-attributed through P
    int    n_outside{0};                     ///< terms outside the block (kappa or marginalized)
    bool   deficit_orphan{false};            ///< a clock-family deficit had no pivot (dropped)
};

/**
 * @brief Advance one clone row over one epoch: the exact time update and the
 *        measurement update of its value.
 *
 * Pure: reads the epoch's exact inputs and the row, writes nothing.  Fails (with the
 * death cause) when the epoch is not factored, the row's stamp is stale, the parent
 * clock's own term is outside the dense block, or the exact row is not finite.
 * @param in    the epoch's inputs (@ref PbHoldCloneInputs).
 * @param c     the clone, expressed on the previous stash.
 * @return the new value, variance and coefficients, or the death cause.
 */
PbHoldCloneStep pbHoldCloneAdvance(const PbHoldCloneInputs& in, const PbHoldClone& c);

/**
 * @brief The epoch's exact inputs restricted to the keys the final solve folded.
 *
 * Every S key of @p xe whose P row is in @p unfolded (its carried prior was not in the
 * solve: a clock the Q3 guard withdrew and re-solved, a constellation the fold skipped
 * at an un-transformed datum handover) moves to D on the Reinit route, S is renumbered,
 * the seam's off-diagonal noise is remapped (entries on a moved key dropped) and
 * @f$P_{SS}+Q_S@f$ is refactored (@c ok false if it does not factor).  With @p unfolded
 * empty the result equals @p xe.
 * @param xe        the epoch's inputs as built before the solve.
 * @param unfolded  P rows whose key the solve did not fold.
 * @return the restricted inputs.
 */
CouplingExactEpoch pbHoldCloneRestrictEpoch(const CouplingExactEpoch& xe,
                                            const std::vector<int>& unfolded);

/**
 * @brief Re-express an advanced clone on a new stash (the re-harvest).
 *
 * A term whose key is in the new stash with a dense row takes the stash's value and
 * variance; the constellation's pinned clock keeps p_prev = 0 with its value; any other
 * term keeps its coefficient with the posterior value and variance given by @p fallback
 * (the next epoch then routes it outside the block).  Coefficients below 1e-8 of the
 * largest are dropped.
 * @param terms     the advanced coefficients (with columns).
 * @param stash     key -> (value, variance) of the new stash's dense keys.
 * @param pinned    keys pinned by the datum this epoch -> their value.
 * @param fallback  column -> (posterior value, variance) for the remaining terms.
 * @return the terms of the re-harvested row.
 */
std::vector<PbHoldCloneTerm> pbHoldCloneRebase(
    const std::vector<PbHoldCloneTerm>& terms,
    const std::map<std::string, std::pair<double, double>>& stash,
    const std::map<std::string, double>& pinned,
    const std::function<std::pair<double, double>(Eigen::Index)>& fallback);

/**
 * @brief Card SMH, before emit: advance every live clone over this epoch.
 *
 * The keys and prior means are the final solve's: the dense fold's capture
 * (@c ws.cap_dense_keys, which card Q3's in-epoch refloat trims) and the diagonal priors
 * (@c ws.cap_priors).  An S key of @c ctx.carry.cxu_epoch that the solve did not fold
 * takes the Reinit route (@ref pbHoldCloneRestrictEpoch), so a withdrawn prior or an
 * un-carried datum re-realization never reaches a clone as a revision.  Each live clone
 * is advanced (@ref pbHoldCloneAdvance), its instruments recorded (the passed motion, the
 * posterior variance of the increment from the solve's covariance, the fold inflation of
 * the parent), and a clone that cannot take the exact route, or whose row lost every
 * dense term, dies.  Before any of it, a clone whose parent clock card Q3's guard
 * decoupled this epoch dies @c Withdrawn, and one whose parent was not folded at the
 * anchored variance dies @c Disengaged (a clone on the constellation's pinned clock is
 * exempt from the second); both queue the birth again.  No-op unless
 * @c network.emit_pb_hold_smoothed.
 * @param ctx  the run context.
 * @param log  the network logger.
 * @param est  the estimator after this epoch's final solve.
 * @param x_s  the shared-border solution.
 * @param ws   the epoch's workspace (the fold's capture).
 */
void networkPbHoldCloneUpdate(NetworkRunContext& ctx,
                              const std::shared_ptr<spdlog::logger>& log,
                              const NetworkEstimator& est, const Eigen::VectorXd& x_s,
                              const EpochWorkspace& ws);

/**
 * @brief Card SMH, after the stash: re-harvest the advanced clones on the new stash and
 *        give birth to the clones of the generations that started at this epoch's emit.
 *
 * A clone alive but not advanced this epoch, or any clone at an epoch whose stash has no
 * dense block, dies.  A birth takes the satellite's stashed clock (M = -1, sigma2 = 0),
 * and only while card Q3's anchor holds that clock (anchored after this epoch's guard,
 * not decoupled in it), or, for the constellation's pinned clock, a pinned term (a
 * transfer of zero).  A pending birth whose clock is not stashed or not anchored waits
 * for a later stash (counted by cause, with the wait); its key carries card LAH's
 * transfer meanwhile.  A pending birth of a superseded generation is dropped.  No-op
 * unless @c network.emit_pb_hold_smoothed.
 * @param ctx  the run context.
 * @param est  the estimator after this epoch's final solve.
 * @param x_s  the shared-border solution.
 * @param now_s this epoch's continuous GPS seconds.
 */
void networkPbHoldCloneHarvest(NetworkRunContext& ctx, const NetworkEstimator& est,
                               const Eigen::VectorXd& x_s, double now_s);

}  // namespace GnssProcessor
