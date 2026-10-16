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
 *
 * Card SMD (an instrument of the smoothed hold; its wire selection was removed): a block
 * clone carries, on one lifetime, a clock row and the row of the ionosphere-free phase-visible clock
 * @f$D = c - \sum_j w_j p_j@f$ over one phase bias per band, the pair fixed at birth.  The
 * exact rule is linear in a row's coefficients, so the D row is the functional of the
 * (c, p_1, p_2) block with every cross term (@ref couplingExactCross).  Its passed motion
 * @f$\Phi_D = D(t|t) - D(t_0|t)@f$ is blind to the level (@f$\sum w = 1@f$); the clone
 * retires at every declared step of its phase-bias pair (a re-reference, a new anchor
 * tenure, an emitter mark on the phase channel) and is born again on the new frame.
 */

#include "network/carry/NetworkCouplingExact.hpp"
#include "network/solve/NetworkEstimator.hpp"

#include <Eigen/Dense>

#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

struct NetworkRunContext;   // network/state/NetworkRunContext.hpp defines it as a struct
struct EpochWorkspace;      // network/driver/NetworkRunTypes.hpp
struct SharedHistoryState;  // network/carry/NetworkSharedHistory.hpp (card SCL2 X2)

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
    Followed,       ///< card SMD: a declared phase-datum step of the block clone's phase-bias pair (@ref PbHoldFollow); the birth is queued again
    PairLost,       ///< card SMD: a phase bias of the pair had no column or no folded prior this epoch; the birth is queued again
    kCount
};

/// Card SMD: the declared step a block clone followed by retiring (@ref PbHoldCloneDeath::Followed).
enum class PbHoldFollow : int {
    None = 0,
    Reref,      ///< a pair key re-referenced (the fold withheld its prior: a cold handover, an incumbent-less pin, a gate drop)
    Tenure,     ///< a new anchor tenure of a pair key's family without a re-reference (a continuous handover)
    Mark,       ///< an emitter mark on the satellite's phase-bias channel
    kCount
};

/// One key of a clone row's own functional (card SMD): the combination whose smoothed value
/// the row carries.  Card SMH's clock row is the clock alone (g = 1); card SMD's phase-visible
/// row is the clock (g = 1) and one phase bias per band (g = -w_j).
struct PbHoldCloneOwn {
    SharedParamKind kind{SharedParamKind::Clock};  ///< Clock or PhaseBias
    std::string     signal;                        ///< the phase bias's signal (empty for the clock)
    double          g{1.0};                        ///< the functional's coefficient
};

/// Card SMD: the band pair of a satellite's phase-visible functional.
struct PbHoldPair {
    std::vector<std::string> sig;   ///< one phase-bias signal per band, in band order
    std::vector<double>      w;     ///< the weights: sum 1, and sum w lambda^2 = 0
};

/**
 * @brief Card SCL2 X2b (amendment A1): one clone row's terms in card SCL2's shared history.
 *
 * Under @c network.carry_coupling_shared_history with the smoothed hold armed, a clone row is
 * not marginalized per row at the update: its coefficients @f$r_T@f$ on Y (the clock clones and
 * the history columns) are kept pending (@ref rc) and projected at the harvest's end onto the
 * coupling rows' kept directions of that epoch's compression ("form p"): with
 * @f$F_T = r_T^\top L@f$ the row keeps @f$F_T V_{k_t}@f$ as its history coefficients (@ref coef,
 * revised with the history columns at the next epoch's update) and adds
 * @f$\lVert F_T V_{\text{rest}}\rVert^2@f$ to its own variance; the compression is unchanged.
 * Empty off.
 */
struct PbHoldCloneShx {
    std::vector<double> coef;   ///< history coefficients of the compression at @ref stash_s (m)
    double stash_s{0.0};        ///< the compression (harvest second) @ref coef belongs to
    Eigen::VectorXd rc;         ///< pending: the row's coefficients on Y at the update (m / m)
    double rc_var{0.0};         ///< pending: @f$r_T^\top \Sigma_{Y\mid s} r_T@f$, the per-row term a failed projection adds (m^2)
    long long rc_ep{-1};        ///< n_loop_epochs of the pending update (-1: none pending)

    /// @return The pending share of the row's variance at epoch @p ep (0 when none is pending).
    double pendingVar(long long ep) const { return rc_ep == ep ? rc_var : 0.0; }
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
    // Card SMD: a block clone (@c ctx.carry.smh_block) carries, on this one lifetime, the
    // clock row above and the phase-visible row below.  Unused on card SMH's clock clones.
    bool        block{false};                  ///< a card SMD block clone
    PbHoldPair  pv_pair;                       ///< the band pair, fixed at birth
    std::vector<PbHoldCloneTerm> pv_terms;     ///< the phase-visible row's terms
    double      pv_value{0.0};                 ///< its @f$x_0 = D(t_0|t)@f$ at the stamp (m, solve frame)
    double      pv_var{0.0};                   ///< its residual variance (m^2)
    /// @f$\Phi_D@f$, the phase-visible passed motion since birth, frame-free as @ref phi.
    double      pv_phi{0.0};
    bool        pv_phi_ok{false};
    double      pv_W{0.0};                     ///< sum of the fold inflation along D, g'Qg per epoch (m^2)
    bool        pv_w_valid{true};              ///< false once a key of the functional left S
    double      pv_vpost{0.0};                 ///< posterior variance of @f$D(t) - D(t_0)@f$ (m^2)
    double      pv_step_d{0.0};                ///< this epoch's frame-free step of D (m), read at the hold against the emitter's bar
    // Card REV: the instruments of the revision member's replay (trace only).
    /// The revision @f$R = D(t_0|t) - D(t_0|t_0)@f$ since birth: the sum of the changes of
    /// @ref pv_value, frame-free as @ref pv_phi, so @f$\Phi_D + R = S_D@f$ (@ref pv_sd).
    double      pv_rev{0.0};
    double      pv_sd{0.0};                    ///< @f$S_D@f$, the sum of @ref pv_step_d since birth (m)
    /// The row's stated variance of its own value, @f$V = \mathrm{Var}(D(t_0)\,|\,t)@f$
    /// (@ref pbHoldCloneRowValueVar), m^2.
    double      pv_vd0{0.0};
    double      pv_vd0_birth{0.0};             ///< @f$V@f$ at birth, the filtered variance of D at the stash (m^2)
    double      pv_pd{0.0};                    ///< the filtered variance of D now, @f$g'\Sigma g@f$ (m^2)
    double      pv_dw{0.0};                    ///< this epoch's fold inflation along D (0 with a key of the functional outside S) (m^2)
    PbHoldFollow follow{PbHoldFollow::None};   ///< the step a @c Followed death followed
    long long   died_ep{-1};                   ///< n_loop_epochs of the death
    // Card SCL2 X2b: the rows' terms in the shared history (empty unless X2 is armed).
    PbHoldCloneShx shx;                        ///< the clock row's
    PbHoldCloneShx pv_shx;                     ///< card SMD: the phase-visible row's
};

/// The epoch-level inputs of @ref pbHoldCloneAdvance, built once per epoch.
struct PbHoldCloneInputs {
    const CouplingExactEpoch* xe{nullptr};
    /// Card SCL: the epoch whose inflation @c q_s / @c q_off is the fold's walk, read only
    /// by the walk-share instrument (@c PbHoldCloneStep::dW).  Set when @c xe carries clocks
    /// on their clones at @f$Q=0@f$ (@ref pbHoldCloneOnClones): it is then card CTU's epoch
    /// over the same S index space, so W keeps its meaning (the modelled walk since birth).
    /// Null: @c xe itself.
    const CouplingExactEpoch* xe_walk{nullptr};
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
    int    outside_own{-1};                  ///< with @c ParentOutside: the index of the own key outside the block
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
 * @brief Advance one row of any functional over one epoch (card SMD): the rule of
 *        @ref pbHoldCloneAdvance with the row's own keys in place of the parent clock.
 *
 * Pure.  A key of @p own outside the dense block (not stashed, not pinned) is the death
 * @c ParentOutside with @c outside_own its index; @c dW is the fold inflation along the
 * functional, @f$\sum_{a,b} g_a g_b Q_{ab}@f$ over the own keys in S (the seam's
 * off-diagonal noise included), and @c parent_in_s is true when every own key is in S.
 * With @p own the clock alone (g = 1) it is @ref pbHoldCloneAdvance.
 * @param in      the epoch's inputs.
 * @param sat     the satellite of the own keys.
 * @param own     the row's functional (@ref PbHoldCloneOwn).
 * @param terms   the row's terms on the previous stash.
 * @param value   the row's @f$x_0@f$ at the previous stash.
 * @param var     the row's residual variance.
 * @param stamp_s the stash the terms are expressed on.
 * @return the new value, variance and coefficients, or the death cause.
 */
PbHoldCloneStep pbHoldCloneAdvanceRow(const PbHoldCloneInputs& in, const std::string& sat,
                                      const std::vector<PbHoldCloneOwn>& own,
                                      const std::vector<PbHoldCloneTerm>& terms,
                                      double value, double var, double stamp_s);

/**
 * @brief Card SMD: the band pair of a satellite's phase-visible functional.
 *
 * Per band, the phase bias with the smallest stashed variance (card PVN's rule, read once at
 * birth), and the minimum-norm weights with @f$\sum w = 1@f$ and
 * @f$\sum w \lambda^2 = 0@f$ (@ref clockAnchorPhaseClockWeights; GPS L1 / L2 2.5457 /
 * -1.5457).  A tie keeps the lexically first signal.
 * @param sat     the satellite (its constellation sets the bands).
 * @param pb_var  signal -> stashed variance of the satellite's phase biases with a dense row.
 * @return the pair, or an empty one when fewer than two bands are stashed.
 */
PbHoldPair pbHoldClonePickPair(const std::string& sat, const std::map<std::string, double>& pb_var);

/// Card SMD: the functional of a block clone's phase-visible row, {clock: 1, pair: -w_j}.
std::vector<PbHoldCloneOwn> pbHoldCloneOwnPv(const PbHoldPair& pair);

/**
 * @brief Card REV: the stated variance of a clone row's own value, the smoothed
 *        @f$x_0@f$ (for the phase-visible row, @f$D(t_0|t)@f$).
 *
 * @f$\sigma^2 + m'\Sigma m@f$, with @f$m@f$ the row's coefficients on the epoch's columns
 * (terms with a column and a dense prior; a datum-pinned term carries no variance): the
 * quantity card SMD's U0 checks against an augmented filter that carries the functional at
 * @f$t_0@f$ as a zero-noise state.  A column at or beyond @p Sig's dimension is skipped, as
 * in the posterior variance of the increment.  The revision member's freeze reads its fall
 * per epoch against the stated walk (card REV section 2).
 * @param terms  the row's terms with this epoch's columns (after an advance, or at birth).
 * @param var    the row's residual variance @f$\sigma^2@f$ (m^2).
 * @param Sig    the shared border's posterior covariance this epoch.
 * @return the variance (m^2).
 */
double pbHoldCloneRowValueVar(const std::vector<PbHoldCloneTerm>& terms, double var,
                              const Eigen::MatrixXd& Sig);

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
 * @brief Card SCL: the epoch's exact inputs with the given clocks on their clones.
 *
 * Under @c network.carry_coupling_clock_clone the solve's prior is the joint one over the
 * stand-ins (each cloned clock stood in for by its clone @f$c_s(t-1)@f$ at the stashed
 * covariance) plus the walk factor @f$c_s(t) - c_s(t-1)@f$, and the coupling rows reference
 * the clone.  The new data then inform @f$c_s(t-1)@f$ directly, through the rows, and not
 * only through @f$c_s(t)@f$, so a row with no observations (card SMH's) is conditionally
 * independent of the epoch's data given the stand-ins, not given @f$s(t)@f$.  Each S key
 * whose P row is in @p clone_of_p moves onto its clone: column = the clone, @f$Q = 0@f$, its
 * mean the stashed value (unchanged), @c clone_s set; the off-diagonal process noise pairs
 * touching a moved key are dropped; @f$P_{SS}+Q_S@f$ is refactored (@c ok false if it does
 * not factor).  The S index space is unchanged, so @p xe stays the walk-share epoch
 * (@c PbHoldCloneInputs::xe_walk).  With the moved set equal to the clones the rows were
 * built on, the result equals @c ClockCloneEpoch::rows_epoch.
 * @param xe          the epoch's inputs on the epoch's own columns (card CTU's, restricted to
 *                    the folded keys when the solve did not fold some).
 * @param clone_of_p  P row -> clone column, for each clock whose clone stood in at the fold.
 * @return the inputs on the clones (a copy of @p xe when nothing moves).
 */
CouplingExactEpoch pbHoldCloneOnClones(const CouplingExactEpoch& xe,
                                       const std::map<int, Eigen::Index>& clone_of_p);

/// Outcome of @ref pbHoldCloneMarginalizeClones (card SCL).
struct PbHoldCloneMarg {
    bool   ok{true};          ///< false: the regression was not usable (a row with clone terms is then not re-expressed)
    int    n_clone{0};        ///< terms on clone columns marginalized
    int    n_moved{0};        ///< new terms the regression wrote (a coefficient onto an existing key is merged, not counted)
    int    n_dropped{0};      ///< regression coefficients on columns with no identity (SH-VTEC), dropped
    double mass_dropped{0.0}; ///< sum of their |coefficient|
    int    n_held{0};         ///< clock-family deficits restored on the constellation's pinned clock
    int    n_orphan{0};       ///< clock-family deficits with no pinned clock of the constellation (dropped)
    double var_add{0.0};      ///< @f$r_C^\top B' r_C@f$ added to the row's variance (m^2)
};

/**
 * @brief Card SCL: marginalize a clone row's clock-clone terms through the solve's
 *        posterior regression, so the row carries no term on a clone into the next epoch.
 *
 * A clone lives one epoch.  An advanced row on @ref pbHoldCloneOnClones's epoch has its
 * clocks' terms on the clones, @f$x_0 = v - m_{S'}^\top(s' - \hat s') - r_C^\top(c - \hat c)
 * + e@f$.  With @f$c \mid s' = \hat c + K(s' - \hat s') + \epsilon@f$,
 * @f$\epsilon \sim N(0, B')@f$ (@c NetworkEstimator::cloneConditional, the same regression as
 * the coupling rows' harvest), the row becomes @f$m_{S'} \mathrel{+}= K^\top r_C@f$,
 * @f$\sigma^2 \mathrel{+}= r_C^\top B' r_C@f$, the clone terms removed; the row's value and
 * the variance of @f$x_0@f$ given the data are unchanged.  A regression coefficient onto a key
 * the row already carries on the same column is merged into it; a new one takes the column's
 * identity from @p term_of_col (with its posterior value and variance, which the re-harvest
 * replaces by the stash's).  Per constellation the clock-family deficit (the clones'
 * coefficients minus what the regression put on the constellation's clocks) goes onto the
 * pinned clock in @p pin, as the harvest's gauge restore does: the row keeps its identity
 * along the declared clock gauge.  No-op (ok) when the row has no clone term.
 * @param terms       the row's terms (clone terms are those on columns [n0, n0 + nc)).
 * @param var         the row's residual variance, raised in place.
 * @param K           nc x (n0 + nc): E[c | s'] coefficients, zero on the clone and pinned columns.
 * @param B           nc x nc: Var(c | s', data).
 * @param n0          the first clone column (the clones are the last nc columns).
 * @param term_of_col column -> a term with its identity, value and variance; false when the
 *                    column has none (the coefficient is dropped, counted).
 * @param pin         constellation -> its datum-pinned clock (p_prev 0, with its column and value).
 * @param defer_rc    card SCL2 X2b: non-null defers the conditional variance: the row's
 *                    coefficients on the trailing block (Y = the clones and the shared
 *                    history) are written here, @p var is not raised, and @c var_add reports
 *                    the term that was not added (the harvest's end projects it,
 *                    @ref PbHoldCloneShx).  Null (the default): the form above, byte for byte.
 * @return the counts; @c ok false (row untouched) when @p K / @p B are not shaped for @p n0.
 */
PbHoldCloneMarg pbHoldCloneMarginalizeClones(
    std::vector<PbHoldCloneTerm>& terms, double& var, const Eigen::MatrixXd& K,
    const Eigen::MatrixXd& B, Eigen::Index n0,
    const std::function<bool(Eigen::Index, PbHoldCloneTerm&)>& term_of_col,
    const std::map<char, PbHoldCloneTerm>& pin, Eigen::VectorXd* defer_rc = nullptr);

/// Card SCL2 X2b: the counts of @ref pbHoldCloneAttachHistory.
struct PbHoldCloneHistAttach {
    int    n_terms{0};     ///< history terms attached on registered columns
    int    n_marg{0};      ///< history terms marginalized (a stale generation or a column not registered)
    double marg_var{0.0};  ///< their squared coefficients, added to the row's variance (m^2)
};

/**
 * @brief Card SCL2 X2b: a clone row's history terms at the epoch after their compression.
 *
 * A history column is a constant of the epoch with the prior N(0, 1), so a term needs no time
 * update: with the compression @p sh belongs to equal to the one whose columns are registered
 * this epoch (@c shx.on and equal stamps), each nonzero coefficient becomes a term on its
 * column (p_prev 1, s_prev 0) and the row's value takes its revision,
 * @f$x_0 \mathrel{-}= h_j \hat z_j@f$; any other coefficient (a stale generation, a column not
 * registered or outside @p x_s) is marginalized into @p var at its prior,
 * @f$\sigma^2 \mathrel{+}= h_j^2@f$.  The coefficients are consumed (@c sh.coef cleared).  No-op
 * on a row with no coefficient.
 * @param shx    this epoch's shared-history state (registered columns and their compression).
 * @param sh     the row's shared-history record.
 * @param x_s    the shared-border solution.
 * @param terms  the advanced row's terms (history terms appended).
 * @param value  the row's value @f$x_0@f$, revised in place.
 * @param var    the row's residual variance, raised in place by marginalized terms.
 * @return the counts.
 */
PbHoldCloneHistAttach pbHoldCloneAttachHistory(const SharedHistoryState& shx, PbHoldCloneShx& sh,
                                               const Eigen::VectorXd& x_s,
                                               std::vector<PbHoldCloneTerm>& terms,
                                               double& value, double& var);

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

struct NetworkRun;              // network/driver/NetworkRun.hpp (card NOM3 step EP1)
struct NetworkEpoch;            // network/driver/NetworkEpoch.hpp (card NOM3 step EP1)

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
 * exempt from the second); both queue the birth again.  The block clones of card SMD
 * (@c ctx.carry.smh_block, carried whenever the smoothed hold is armed) take the same
 * engagement rule, then retire (@c Followed, the birth queued again) at a re-reference or a
 * new anchor tenure of either phase bias of their pair, and otherwise advance both rows:
 * the clock row as above and the phase-visible row over its functional, with a pair phase
 * bias lacking a column or a folded prior the death @c PairLost.  No-op unless
 * @c network.emit_pb_hold_smoothed.
 *
 * Card SCL (@c network.carry_coupling_clock_clone), on an epoch where a clone stood in at
 * the fold: every row advances on @ref pbHoldCloneOnClones's epoch (the stood-in clocks on
 * their clones at @f$Q=0@f$; a refloat's unfolded clock whose clone stood in stays there,
 * since the clone keeps its stand-in prior), the revision reads each clone's posterior
 * against its prior mean (the stashed value the fold centred it on), the walk share reads
 * card CTU's epoch, and the clone terms are then marginalized
 * (@ref pbHoldCloneMarginalizeClones) through the regression the coupling rows' harvest uses,
 * evaluated here on this epoch's final solve (the update precedes the harvest; the harvest
 * compares its own regression with this one, counted).  A row with clone terms and no
 * regression dies @c RowFailed.  Off, or with no clone stood in, the update is the one
 * above, byte for byte.
 *
 * Epoch form (card NOM3 step EP1); the explicit form and its forwarding adapter
 * retired in card NOM3 step EP2 (the unit tests call this form through
 * tests/support/NetworkTestRun.hpp).
 *
 * @param run The run (network/driver/NetworkRun.hpp). Written through non-const bindings: run.ctx (ctx).
 *            Read: run.log (log), run.engines.est (est).
 * @param ep The epoch (network/driver/NetworkEpoch.hpp), read only. Bindings: ep.solve.x_s (x_s), ep.ws
 *           (ws), ep.solve.plan.shared_datum (datum).
 *
 * The former parameters, by binding:
 *   - @p ctx = run.ctx: the run context.
 *   - @p log = run.log: the network logger.
 *   - @p est = run.engines.est: the estimator after this epoch's final solve.
 *   - @p x_s = ep.solve.x_s: the shared-border solution.
 *   - @p ws = ep.ws: the epoch's workspace (the fold's capture).
 *   - @p datum = ep.solve.plan.shared_datum: the datum the final solve applied (card SCL's regression; unread otherwise, so a caller without the knob may leave it empty).
 */
void networkPbHoldCloneUpdate(NetworkRun& run, const NetworkEpoch& ep);

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
 * transfer meanwhile.  A pending birth of a superseded generation is dropped.  A block clone
 * (card SMD) is re-harvested row by row and born under the same rule, when the stash also
 * holds a phase bias of the satellite on two bands (@ref pbHoldClonePickPair; otherwise it
 * waits, counted).  No-op unless @c network.emit_pb_hold_smoothed.
 *
 * Epoch form (card NOM3 step EP1); the explicit form and its forwarding adapter
 * retired in card NOM3 step EP2 (the unit tests call this form through
 * tests/support/NetworkTestRun.hpp).
 *
 * @param run The run (network/driver/NetworkRun.hpp). Written through non-const bindings: run.ctx (ctx).
 *            Read: run.engines.est (est).
 * @param ep The epoch (network/driver/NetworkEpoch.hpp), read only. Bindings: ep.solve.x_s (x_s),
 *           TimeUtils::continuousGpsSeconds(ep.se.time) (now_s).
 *
 * The former parameters, by binding:
 *   - @p ctx = run.ctx: the run context.
 *   - @p est = run.engines.est: the estimator after this epoch's final solve.
 *   - @p x_s = ep.solve.x_s: the shared-border solution.
 *   - @p now_s = TimeUtils::continuousGpsSeconds(ep.se.time): this epoch's continuous GPS seconds.
 */
void networkPbHoldCloneHarvest(NetworkRun& run, const NetworkEpoch& ep);

/**
 * @brief Card SMD, at the hold: one held key's block clone against the emitter's marks.
 *
 * A block clone of @p key and @p generation that was advanced this epoch retires
 * (@c Followed, @ref PbHoldFollow::Mark, the birth queued again) when @p pb_marked (a mark on
 * the satellite's phase-bias channel is pending at the emit).  One that retired at a
 * re-reference this epoch with no such mark is counted.  One that stays live has the
 * frame-free step of D this epoch (in which a level update and an ionosphere-direction
 * update cancel, while a re-realization of a pair phase bias steps it by w_j times its
 * step) compared with the smallest jump bar of its pair; a step above it is an unmarked
 * step, passed
 * as motion (counted, with the largest), and the split of its passed motion recorded (the
 * level part @f$\Phi_c - \Phi_D@f$ and @f$\Phi_D@f$, and @f$r_D@f$ by hour).  No-op unless
 * @c network.emit_pb_hold_smoothed.
 * @param ctx         the run context.
 * @param key         the hold key ("sat|signal").
 * @param generation  the key's hold generation.
 * @param pb_marked   the satellite's phase-bias channel is marked this epoch.
 * @param epoch_s     this epoch's continuous GPS seconds (the hour of @f$r_D@f$).
 * @param bar         the emitter's jump bar of a phase-bias signal of the satellite (m).
 */
void networkPbHoldBlockAtHold(NetworkRunContext& ctx, const std::string& key,
                              unsigned generation, bool pb_marked, double epoch_s,
                              const std::function<double(const std::string&)>& bar);

}  // namespace GnssProcessor
