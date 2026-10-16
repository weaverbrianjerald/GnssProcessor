#pragma once

/**
 * @file ClockClone.hpp
 * @brief The one-epoch clock clone of the self-contained clock level (@c network.carry_coupling_clock_clone,
 *        docs/refound/SCL_self_contained_clock_level_2026-10-03.md, sections 4, 4.1 and 5).
 *
 * The exact coupling rule re-expresses each station's carried row on the moved border
 * separately and drops the time-update covariance between rows,
 * @f$M_a^\top B M_b@f$ with @f$B = P_{KK} - P_{KS}(P_{SS}+Q_S)^{-1}P_{SK}@f$.  With
 * per-satellite phase anchors and a clock walk at the broadcast grade, @f$Q_S@f$ dominates
 * the previous clock's variance, every row states its ambiguity with an independent copy
 * of the previous clock's uncertainty, and the n rows of a satellite state its level about
 * n times too tight (the unit network: variance ratio 0.163 at 16 stations).  The clone
 * keeps the shared uncertainty shared:
 *
 * - Registration: each satellite clock with a row in the stashed dense block gets a clone
 *   column @f$c_s(t-1)@f$, registered after every other column
 *   (@ref networkClockCloneRegister), so no existing index moves and the clones are the
 *   last columns.
 * - The rows' epoch: a copy of the exact coupling epoch with each clock whose walk is positive on
 *   its clone at @f$Q=0@f$ (@ref networkClockClonePrepare); a row's harvested clock
 *   coefficient stays where it was harvested.  The exact coupling epoch itself is unchanged: the
 *   fold's counters read it, and the smoothed-hold clone builds its own copy with the clocks whose clones
 *   stood in at the fold on their clones (@c pbHoldCloneOnClones), reading the exact coupling epoch's Q for
 *   its walk share, then marginalizes its rows' clone terms with this epoch's regression
 *   (@c pbHoldCloneMarginalizeClones).
 * - The joint fold, in information form (@ref networkClockCloneDenseFold): the stashed
 *   block over the stand-ins (each cloned clock stood in for by its clone at the stashed
 *   variance, every other key on its own column at its decided variance), and for each
 *   cloned clock the walk factor @f$c_s(t) - c_s(t-1)@f$ at the fold's own variance
 *   @f$Q_s = \mathrm{var}_{\mathrm{eff}} - P_{ss}@f$.  The marginal of this prior over the
 *   epoch's own columns is the exact coupling epoch's prior exactly.
 * - The gate (@ref networkClockCloneGateView): the rank gate, the S-basis completion and
 *   the datum-direction ratio judge the border with every referenced clone marginalized
 *   (a Schur complement, which keeps the declared clock gauge null) and every idle clone
 *   dropped; a clone has a prior and coupling rows, not observations.
 * - The harvest (@ref networkClockCloneHarvestRow): a carried row's coefficients on the
 *   clones are regressed onto the rest of the border through the posterior, and the
 *   clones' conditional covariance is added to the row (and within a station to the F3
 *   block); the cross-station half is dropped, the second-order term of the design
 *   section 4.1 (mode 5 of the unit network).  A clone is never stashed, snapshotted or
 *   emitted.
 *
 * Every function is a no-op when the knob is off, which keeps the off path byte-identical.
 */

#include "network/carry/CouplingExact.hpp"
#include "network/solve/Estimator.hpp"
#include "network/driver/RunTypes.hpp"   // CarryPrior, PlHarvestBlock, EpochWorkspace

#include <Eigen/Dense>

#include <cstddef>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

struct EpochEvents;   // network/driver/Epoch.hpp

struct NetworkRunContext;   // network/state/RunContext.hpp defines it as a struct

/// A cloned clock's walk must exceed this fraction of its stashed variance: below it the
/// walk factor's information (1/Q) would dwarf every real pivot of the shared normals, and
/// the per-row form is exact anyway (no walk, no dropped term).
inline constexpr double kClockCloneMinQRel = 1e-9;

/// One clock moved onto its clone in the rows' epoch this epoch.
struct ClockCloneActive {
    int          p{-1};      ///< the clock's row of the stashed dense block
    int          s{-1};      ///< its S index in the exact coupling epoch
    Eigen::Index col{-1};    ///< its clone column
    double       q{0.0};     ///< the walk variance the rows' epoch removed (m^2)
    double       shat{0.0};  ///< the clone's prior mean: the stashed value the rows use (m)
};

/// The identity of one border clone (its key's kind, satellite and signal).
struct BorderCloneId {
    SharedParamKind kind{SharedParamKind::Clock};
    std::string sat;
    std::string signal;   ///< empty for a clock
};

/// One key moved onto its clone in the rows' epoch by the border-clone mechanism
/// (docs/refound/BCL_previous_border_on_clones_2026-10-08.md): a walking bias (BCL-a, with a
/// walk) or a key the exact coupling update would re-attribute (BCL-d, a time t-1 state with no walk).
struct BorderCloneActive {
    BorderCloneId id;
    int          p{-1};      ///< its row of the stashed dense block
    int          s{-1};      ///< its S index in the rows' epoch
    Eigen::Index col{-1};    ///< its clone column
    double       q{0.0};     ///< BCL-a: the walk variance the rows' epoch removed (m^2); 0 for BCL-d
    double       shat{0.0};  ///< the clone's prior mean: the stashed value the rows use (m)
    int          route{0};   ///< BCL-d: the CouplingExactDRoute the exact coupling epoch gave it (Present for BCL-a)
};

/// The clock clone's per-epoch state (@c ctx.carry.ccl).  Rebuilt every epoch at the column
/// registration; nothing here survives to the next epoch.
struct ClockCloneEpoch {
    std::map<std::string, Eigen::Index>     col;      ///< satellite -> clone column registered this epoch
    std::map<std::string, ClockCloneActive> active;   ///< satellites on their clone in the rows' epoch
    // Border clones (network.carry_coupling_bias_clone / network.carry_coupling_departed_clone),
    // keyed by sharedCarryKey.  Empty with both knobs off.
    std::map<std::string, Eigen::Index>      xcol;     ///< BCL clones registered this epoch (biases; clocks the clock clone did not cover)
    std::map<std::string, BorderCloneId>     xid;      ///< their identities
    std::map<std::string, BorderCloneActive> xactive;  ///< BCL-a: bias keys on their clones in the rows' epoch
    std::map<std::string, BorderCloneActive> xdep;     ///< BCL-d: re-attributed keys on their clones in the rows' epoch
    std::set<std::string> xreferenced;                 ///< BCL clones with row information at the fold
    std::set<std::string> xstandin;                    ///< BCL-a: bias clones stood in with a walk factor this epoch
    std::set<std::string> xdprior;                     ///< BCL-d: clones given the stashed prior with no walk this epoch (any kind)
    CouplingExactEpoch rows_epoch;                    ///< the exact coupling epoch with the active clocks on their clones
    bool rows_on{false};                              ///< PASS B reads @c rows_epoch this epoch
    std::set<std::string> referenced;                 ///< clones with row information at the fold
    std::set<std::string> standin;                    ///< clones folded as stand-ins (with a walk factor) this epoch
    bool gate_counted{false};                         ///< the gate view's census was taken this epoch
    /// The smoothed-hold clone's regression this epoch (@c NetworkEstimator::cloneConditional at the
    /// smoothed hold's update, which precedes the harvest), kept for the harvest's
    /// cross-check (@ref networkClockCloneHarvestBegin).  @c smh_reg false: not formed.
    bool smh_reg{false};
    Eigen::MatrixXd smh_K;                            ///< its K (clones x numShared)
    Eigen::MatrixXd smh_B;                            ///< its B' (clones x clones)

    /// Forget the previous epoch.
    void clear() {
        col.clear();
        active.clear();
        xcol.clear();
        xid.clear();
        xactive.clear();
        xdep.clear();
        xreferenced.clear();
        xstandin.clear();
        xdprior.clear();
        rows_epoch = CouplingExactEpoch{};
        rows_on = false;
        referenced.clear();
        standin.clear();
        gate_counted = false;
        smh_reg = false;
        smh_K.resize(0, 0);
        smh_B.resize(0, 0);
    }
};

/// The clock clone's run-end telemetry (@c ctx.counters.ccl; standing invariant 4).
struct ClockCloneCounters {
    long long epochs = 0;              ///< epochs with clones registered
    long long registered = 0;          ///< clones registered, summed over epochs
    long long registered_max = 0;      ///< the most clones registered in one epoch
    long long rows_epochs = 0;         ///< epochs whose rows read the clone epoch
    long long active = 0;              ///< clones on their clone in the rows' epoch, summed
    long long prepare_unfactored = 0;  ///< clone epochs that did not factor (the rows read the exact coupling epoch)
    long long q_off_dropped = 0;       ///< off-diagonal process-noise pairs dropped for touching a cloned clock
    long long gate_epochs = 0;         ///< epochs the gate view was formed
    long long gate_referenced = 0;     ///< clones with row information at the gate (first evaluation), summed
    long long gate_idle = 0;           ///< clones with no row information at the gate, summed (dropped from the view)
    long long gate_null_dirs = 0;      ///< directions of the referenced clones' block with no information (dropped)
    long long gate_failed = 0;         ///< gate views not formed (eigensolve failure): the gate read the full border
    long long fold_epochs = 0;         ///< epochs the joint fold ran
    long long fold_standin = 0;        ///< clones folded as stand-ins with a walk factor, summed
    long long fold_free = 0;           ///< referenced clones left without a prior (their clock folded on its own column, or not at all)
    long long fold_isolated = 0;       ///< idle clones given an isolated prior at the stashed variance (a solve no-op on the rest)
    long long fold_q_nonpos = 0;       ///< active clones whose fold-time walk was not positive (their clock folded on its own column)
    long long fold_retired = 0;        ///< active clones whose clock the fold did not fold (a handover retire, a screen)
    long long fold_reexpressed = 0;    ///< active clones whose carry moved after the rows were built (handover transform, reinit, re-expression)
    long long fold_walk_moved = 0;     ///< walk factors with a nonzero mean (the carry moved after the rows were built)
    long long fold_fallback = 0;       ///< dense-fold fallbacks on a clone epoch (the clones then free or isolated)
    long long fold_dense_epochs = 0;   ///< joint folds across a dense-transformed handover (stand-ins in the outgoing frame)
    long long fold_dense_standin = 0;  ///< clones stood in across it (the incoming pivot's included)
    long long fold_dense_noref = 0;    ///< dense-transformed constellations whose clones could not stand in (no usable incoming-pivot clone, a failed self-check, no copy of the block before)
    long long harvest_epochs = 0;      ///< harvests with clones registered
    long long harvest_noreg = 0;       ///< of those, harvests whose clone regression was unavailable
    long long harvest_rows = 0;        ///< carried rows with a clone term, marginalized
    long long harvest_rows_dropped = 0;///< carried rows with a clone term dropped without the regression
    double    harvest_reg_mass = 0.0;  ///< sum of |K' r_C| over the marginalized rows (the coefficients moved onto the border)
    double    harvest_var_mass = 0.0;  ///< sum of r_C' B' r_C (m^2), the conditional variance added
    long long harvest_f3_pairs = 0;    ///< F3 within-station pairs that took r_C,a' B' r_C,b
    long long harvest_gauge_held = 0;  ///< clock-family deficits restored on the constellation's pinned clock
    long long harvest_gauge_orphan = 0;///< clock-family deficits with no pinned clock of the constellation (the row stays gauge-variant)
    double    harvest_gauge_abs = 0.0; ///< summed |deficit| restored
    long long refloat_walks = 0;       ///< walk factors withdrawn by the temporal clock anchor's in-epoch refloat
    // Pivot-frame process noise on the clone (network.clock_pivot_frame_q with the clone): the pivot's walk
    // as a joint walk factor over the stood-in clocks.
    long long pfq_walk_epochs = 0;     ///< joint folds whose walks carried the pivot's walk pairs
    long long pfq_walk_pairs = 0;      ///< walk pairs given r_p min(dt_a, dt_b), summed
    long long pfq_walk_notpd = 0;      ///< joint walk covariances that did not factor (folded per walk)
    long long pfq_pairs_lost = 0;      ///< pairs between a stood-in clone and a clock on its own column (no place in the factorization)
    // Border clones (network.carry_coupling_bias_clone, BCL-a; network.carry_coupling_departed_clone,
    // BCL-d): the clones of the rest of the stashed border.
    long long bcl_epochs = 0;          ///< epochs with BCL clones registered
    long long bcl_registered = 0;      ///< BCL clones registered, summed over epochs
    long long bcl_registered_max = 0;  ///< the most registered in one epoch
    long long bcl_a_active = 0;        ///< BCL-a: bias keys on their clones in the rows' epoch, summed
    long long bcl_a_screened = 0;      ///< BCL-a: bias keys in S left on their columns (walk below the screen, a seam, a reinit), summed
    long long bcl_a_standin = 0;       ///< BCL-a: bias clones stood in with a walk factor, summed
    long long bcl_a_moved = 0;         ///< BCL-a: active bias keys whose carry moved or did not fold after the rows were built
    long long bcl_d_active[kCouplingExactRoutes]{};   ///< BCL-d: re-attributed keys on their clones in the rows' epoch, by coupling route
    long long bcl_d_noclone[kCouplingExactRoutes]{};  ///< BCL-d: re-attributed keys with a carried entry and no registered clone (its exact coupling route), by route
    long long bcl_d_nocarry = 0;       ///< BCL-d: stashed rows with no carried entry (no value to centre a clone on)
    long long bcl_d_prepare_retry = 0; ///< rows' epochs that did not factor with the BCL-d keys and were rebuilt without them
    long long bcl_d_prior = 0;         ///< clones given the stashed prior with no walk at the fold, summed
    long long bcl_d_fallen = 0;        ///< of those, clones moved in the rows whose key did not fold as decided (any kind)
    long long bcl_d_bad_row = 0;       ///< BCL-d clones whose row lies outside the block the rows were built on (left without a prior)
    long long bcl_d_fold_retry = 0;    ///< joint folds that did not factor with the BCL-d priors and were folded without them
    long long bcl_free = 0;            ///< BCL clones referenced at the fold and left without a prior
    long long bcl_isolated = 0;        ///< BCL clones idle at the fold, given an isolated prior
    std::vector<double> post_sd;       ///< sqrt(diag B') of each stand-in clone at each harvest (m): the size of the second-order term
    // The smoothed-hold clone rows on the clone epoch (network.emit_pb_hold_smoothed with the clone;
    // pbHoldCloneOnClones / pbHoldCloneMarginalizeClones).
    long long smh_epochs = 0;          ///< epochs the smoothed hold's rows advanced on the clone epoch
    long long smh_moved = 0;           ///< clocks on their clones in those epochs, summed
    long long smh_partial = 0;         ///< of those epochs, ones whose stood-in set was smaller than the rows' clone set
    long long smh_kept_unfolded = 0;   ///< clocks the temporal clock anchor's refloat unfolded that stayed on their (stood-in) clones
    long long smh_unfactored = 0;      ///< clone epochs of the smoothed hold that did not factor (its rows then die unfactored)
    long long smh_noreg = 0;           ///< epochs whose clone regression was not formed (rows with clone terms die)
    long long smh_rows = 0;            ///< rows re-expressed (clone terms marginalized)
    long long smh_clone_terms = 0;     ///< clone terms marginalized
    long long smh_moved_terms = 0;     ///< new terms the regression wrote
    long long smh_dropped = 0;         ///< regression coefficients on columns with no identity, dropped
    double    smh_mass_dropped = 0.0;  ///< their summed |coefficient|
    long long smh_gauge_held = 0;      ///< clock-family deficits restored on the pinned clock
    long long smh_gauge_orphan = 0;    ///< clock-family deficits with no pinned clock (dropped)
    double    smh_var_add = 0.0;       ///< summed r_C' B' r_C (m^2)
    long long smh_reg_checked = 0;     ///< harvests that compared their regression with the smoothed hold's
    long long smh_reg_mismatch = 0;    ///< of those, harvests whose K or B' differed (any bit)
    double    smh_reg_maxdiff = 0.0;   ///< the largest |difference| of an element seen
};

/**
 * @brief Register this epoch's clock clones, after every other column.
 *
 * Armed and with a valid stash stamp: one clone per registered satellite clock that has a
 * carried entry, a row @c "sat|CLK" in the stashed dense block and no reinit mark.  The
 * datum-pinned pivot has no stashed row, so it gets none.  Records satellite -> column in
 * @c ctx.carry.ccl (cleared here every armed epoch).  Border clones: then, by key, a clone of
 * every phase and code bias with a stashed row (BCL-a) and of every key that will or may be
 * re-attributed (BCL-d: a clock the clock clone did not cover; without BCL-a, a bias whose column is
 * absent, whose phase-bias frame is re-referenced or held, or which is parked), in
 * @c ccl.xcol.  No-op off.
 * @param events The epoch's events, read: the chain election's re-referenced (sat, signal) set.
 */
void networkClockCloneRegister(NetworkRunContext& ctx, NetworkEstimator& est,
                               const std::vector<std::string>& registered_clock_sats, const EpochEvents& events);

/**
 * @brief Build the rows' epoch: the exact coupling epoch with each cloned clock on its clone
 *.
 *
 * Called at the end of @c networkCouplingExactPrepare once the exact coupling epoch is built.  A
 * clock in S whose walk @f$Q_s@f$ is positive (and above @ref kClockCloneMinQRel of its
 * stashed variance), with a registered clone, no reinit mark and no switch-on seam noise,
 * moves onto its clone: column = the clone, @f$Q = 0@f$, mean = the stashed value.  The
 * off-diagonal process noise pairs touching a moved clock are dropped, and the epoch is
 * re-factored.  On a factor failure the rows read the exact coupling epoch (counted).  BCL-a: a
 * phase or code bias in S with a registered clone moves the same way, by the same screen.
 * BCL-d: every key the exact coupling epoch puts in D, with a carried entry and a registered
 * clone, joins S on its clone at Q = 0 (mean its carried value), so no term is
 * re-attributed; when that epoch does not factor it is rebuilt without those keys (counted).
 * Always resets the previous epoch's rows' epoch; a no-op beyond that when off.
 * @param cp_of_p the carried entry of each stashed dense row (null for none), as the
 *                prepare resolved it.
 */
void networkClockClonePrepare(NetworkRunContext& ctx,
                              const std::vector<const CarryPrior*>& cp_of_p);

/**
 * @brief The border the rank gate, the S-basis completion and the datum-direction ratio
 *        judge when clones are registered.
 *
 * The reduced shared normals over the non-clone columns (the first
 * @c numShared - numClockClones, whose indices are those of the full layout), with every
 * clone that has row information marginalized (the Schur complement through a
 * correlation-scaled pseudo-inverse of the clones' block: a direction of that block with
 * no information is dropped) and every idle clone (a zero column: no row references it)
 * dropped.  The marginalization keeps the declared clock gauge null (the joint shift of
 * the clocks, the clones and the receiver clocks projects onto the clocks' shift), which a
 * principal sub-matrix would not.  The census (referenced, idle) is counted once per epoch.
 * Shared history: the columns, registered after the clones, are marginalized
 * with them (the trailing @c numBorderNuisance columns), and an epoch with history columns
 * and no clone forms the view too.
 * @param[out] out the view (left untouched when false is returned).
 * @return false when off, when no clone is registered, or when the eigensolve fails; the
 *         caller then reads @c est.reducedSharedNormals() as before.
 */
bool networkClockCloneGateView(NetworkRunContext& ctx, NetworkEstimator& est,
                               Eigen::MatrixXd& out);

/// Pad gauge rows formed over the gate view (@ref networkClockCloneGateView) with zero
/// clone columns, to the full width @p n_shared.
Eigen::MatrixXd networkClockClonePadRows(const Eigen::MatrixXd& H, Eigen::Index n_shared);

/**
 * @brief Record which clones carry row information, at the entry of the D2 fold
 *.
 *
 * At that point only the coupling rows have written to a clone column (no data row and
 * no pre-gate prior touches one), so a clone column of the reduced normals that is not
 * exactly zero is referenced.  Resets this epoch's stand-in set.  No-op off.
 */
void networkClockCloneFoldBegin(NetworkRunContext& ctx, NetworkEstimator& est);

/**
 * @brief The joint fold over the stand-ins and the walk factors, in place of
 *        the exact coupling update's dense fold when at least one clone stands in.
 *
 * For each dense key that is an active clone's clock: when its decision folds it on the
 * stashed row the rows used, with no reinit, no re-expression since the decision, no
 * handover transform of its constellation and a positive fold-time walk
 * @f$Q = \mathrm{var}_{\mathrm{eff}} - P@f$, its entry becomes the clone (mean = the rows'
 * stashed value, variance = the stashed @f$P@f$, the off-diagonals unchanged) and a walk
 * factor @f$c(t) - \text{clone} = \text{value}_{\text{fold}} - \hat s@f$ at variance Q is
 * added.  Every other key folds as before.  The block goes through
 * @c addSharedPriorDense (it throws exactly when that does, folding nothing: the caller's
 * fallback applies), then @c addCloneWalk.  With @p within_epoch_armed the joint fold is
 * captured in @c ws.cap_ccl_* for the within-epoch replay; @c ws.cap_dense_* keep the
 * marginal (the exact coupling epoch's prior) for their readers.
 *
 * A constellation the dense S-transform re-expressed this epoch
 * (@c NetworkCarryState::sts_dense, @c network.handover_stransform_dense) is not a
 * re-expression for this test when nothing else moved its entries between the decision and
 * the transform.  The clones are time-(t-1) states in the outgoing frame and the epoch's
 * columns are in the incoming one, so the prior is the law of @f$(y_C, S y + w)@f$: the
 * stand-ins' rows and columns are rebuilt from the exact coupling epoch's copy of the block before the
 * transform (@c cxu_epoch.P, taken before PASS B), @f$P_{ab}@f$ between stand-ins and
 * @f$P_{ab} - u_b P_{aq}@f$ against a current key; the current keys keep
 * @f$S P S^\top@f$; each stood-in clock's walk is @f$c_s(t) - c_s(t-1) + c_q(t-1)@f$
 * (@c CloneWalk::ref); the incoming pivot's clone stands in with no walk of its own, and its
 * pinned column keeps the fold's independent prior as an extra key.  It needs the kernel's
 * self-check, the copy at the size before the outgoing pivot was appended, and the incoming
 * pivot's clone active, unmoved and passing the walk screen; otherwise the constellation's
 * clocks fold on their own columns, as before (counted).  The one-epoch walk-frame term
 * (the walks are independent in the incoming frame, where the exact walk noise has a
 * common part @f$Q_q@f$) is not added.
 *
 * BCL-a: each bias key on its clone in the rows stands in by the same rule, its walk in
 * the same joint factor (the pivot-frame process noise's pairs stay between clocks).  BCL-d: each key on its
 * clone in the rows' D set, and each moved clone whose key the fold retires, screens or folds
 * decorrelated, is appended as a time t-1 state: the row and column of the exact coupling epoch's copy of the
 * block, against a current clock of a dense-transformed constellation less the incoming
 * pivot's row, zero against a reinit key, and no walk.  A block that does not factor with
 * them is folded without them (counted); with no walk left the function then returns false.
 * @param clk_prior_info B7 trace (null when closed): the folded @f$\Lambda(c,c)@f$ of each
 *        clock, 1/Q for a cloned clock (its column appears only in its walk factor).
 * @return true when the joint fold ran (the caller then skips the exact coupling update's fold); false when
 *         no clone stands in this epoch (nothing folded).
 * @param events The epoch's events, read: the handover's re-expressed constellations and dense record.
 */
bool networkClockCloneDenseFold(NetworkRunContext& ctx, EpochWorkspace& ws,
                                NetworkEstimator& est,
                                const std::vector<NetworkEstimator::SharedPrior>& dkeys,
                                const std::vector<std::size_t>& drow,
                                const std::vector<double>& dvar_eff,
                                const std::vector<char>& dinflate,
                                const std::vector<char>& dreexp,
                                const Eigen::MatrixXd& cov_eff, bool within_epoch_armed,
                                std::map<std::string, double>* clk_prior_info, const EpochEvents& events);

/// Count one dense-fold fallback on a clone epoch (the clones are then free or isolated).
void networkClockCloneNoteFallback(NetworkRunContext& ctx);

/**
 * @brief The end of the D2 fold: every registered clone that did not stand in
 *        is either referenced (left free: the rows determine it, and their information
 *        on the previous level is marginalized) or idle, and an idle clone gets an
 *        isolated prior at its stashed variance (it couples to nothing, so the prior is a
 *        solve no-op on every other state and keeps the normals definite).  Captured for
 *        the replay with @p within_epoch_armed.  Border clones: the same, by key,
 *        skipping those the joint fold gave a stand-in or a stashed prior.  No-op off.
 */
void networkClockCloneFoldEnd(NetworkRunContext& ctx, EpochWorkspace& ws,
                              NetworkEstimator& est, bool within_epoch_armed);

/**
 * @brief Re-fold the clone fold on the within-epoch replay's fresh arrowhead.
 * Folds the idle clones' isolated priors, and, when the joint fold was captured, the
 * stand-in block and the walk factors.
 * @return true when the joint fold was replayed (the caller then skips
 *         @c cap_dense_keys); false otherwise, and always when off.
 */
bool networkClockCloneReplayFold(const NetworkRunContext& ctx, const EpochWorkspace& ws,
                                 NetworkEstimator& est);

/**
 * @brief The temporal clock anchor's in-epoch refloat on a clone epoch: withdraw (or, with
 *        @p restore, re-fold) the walk factors of the dropped satellites and marginalize
 *        any other dropped clock out of the stand-in block.  The clones stay: the rows
 *        still reference them, and the refloat frees the clock, not its previous value.
 * @return true when the joint fold was captured this epoch (the caller then skips its own
 *         refold of @c cap_dense_keys); false otherwise.
 */
bool networkClockCloneRefloat(NetworkRunContext& ctx, EpochWorkspace& ws, NetworkEstimator& est,
                              const std::set<std::string>& drop_sats, bool restore);

/// Keep the joint fold's capture in step with a successful refloat: remove the
/// dropped satellites' walk factors and marginalize their stand-in-block keys.
void networkClockCloneRefloatTrim(EpochWorkspace& ws, const std::set<std::string>& drop_sats);

/// The harvest's clone marginalization for one epoch (see
/// @ref networkClockCloneHarvestBegin).
struct ClockCloneHarvest {
    bool on{false};            ///< the regression is available: rows are marginalized
    bool noreg{false};         ///< clones are registered but the regression is not: clone terms are dropped (counted)
    bool shx{false};           ///< shared history: Y includes the shared history and the rows' conditional variance is deferred to its compression
    Eigen::Index n_shared{0};  ///< width of a regression row
    Eigen::Index n0{0};        ///< first clone column (shared history: first column of Y = clones and history)
    Eigen::MatrixXd K;         ///< clones x n_shared: E[c | s'] coefficients (X2: rows over Y)
    Eigen::MatrixXd B;         ///< clones x clones: Var(c | s', data) (X2: Sigma_Y|s)
    std::vector<char> col_cc;  ///< per non-clone column: the constellation of a satellite clock column, '\0' otherwise
    std::vector<char> clone_cc;///< per clone (column n0 + i): its constellation ('\0' for a history column)
    std::map<char, Eigen::Index> pin_col;   ///< constellation -> its datum-pinned clock column
    std::map<std::pair<std::size_t, Eigen::Index>, Eigen::VectorXd> rc;   ///< (station, local column) -> the row's clone coefficients (X2: its Y coefficients)
    /// One station's F3 block, for the within-station term.
    struct Blk {
        std::size_t est_idx{0};
        std::shared_ptr<PlHarvestBlock> blk;
        std::vector<Eigen::Index> cols;
    };
    std::vector<Blk> blocks;   ///< the stations' F3 blocks this epoch
};

/**
 * @brief Prepare the harvest's clone marginalization: the posterior regression
 *        of the clones on the rest of the border, @f$K@f$ and @f$B'@f$
 *        (@c NetworkEstimator::cloneConditional), once per epoch, and the clock
 *        constellations the gauge restore needs.  Records the stand-in clones' posterior
 *        standard deviations.  Inert (@c on false) off.
 * @param datum    The datum the solve applied (its pins get no coefficient and take the
 *                 clock-family deficit).
 * @param col2desc The harvest's column descriptors (column -> kind and satellite).
 */
ClockCloneHarvest networkClockCloneHarvestBegin(NetworkRunContext& ctx,
                                                const NetworkEstimator& est,
                                                const NetworkEstimator::Datum& datum,
                                                const std::map<Eigen::Index, CoupledShared>& col2desc);

/**
 * @brief Marginalize the clones out of one carried row's regression (mode 5 of
 *        the unit network): with @f$r_C@f$ the row's clone coefficients,
 *        @f$r_{S'} \mathrel{+}= K^\top r_C@f$, @f$\mathrm{cond\_var} \mathrel{+}=
 *        r_C^\top B' r_C@f$, @f$r_C := 0@f$.  Then, per constellation, the clock-family
 *        deficit (the clones' coefficients minus what the re-attribution put on the
 *        constellation's clock columns) goes onto its datum-pinned clock column: the
 *        station regression satisfies @f$M^\top g = -g_r@f$ along the declared clock gauge
 *        with the clones as members of it, the posterior regression carries the priors'
 *        information on that direction, and the pinned column (exactly known, carried
 *        with @c p_prev = 0) restores the identity without moving the row's value, as the exact coupling update's
 *        deficit rule does.  Without the regression the clone terms are dropped (as
 *        the identity-keyed harvest drops every column it does not know) and counted.
 *        No-op when @p h is inert or the row has no clone term.  With the shared history
 *        (@c h.shx): Y is the clones and the shared history, a history column takes no
 *        part in the clock-family deficit (it is no gauge member), and
 *        @f$r_Y^\top \Sigma_{Y\mid s} r_Y@f$ is not added (the history's compression keeps
 *        it shared and adds the row's remainder).
 */
void networkClockCloneHarvestRow(ClockCloneHarvest& h, NetworkRunContext& ctx,
                                 std::size_t est_idx, Eigen::Index local_col,
                                 Eigen::VectorXd& row, double& cond_var);

/// Register a station's F3 harvest block for the within-station term.  No-op
/// when @p h is inert or @p blk is null.
void networkClockCloneHarvestBlock(ClockCloneHarvest& h, std::size_t est_idx,
                                   const std::shared_ptr<PlHarvestBlock>& blk,
                                   const std::vector<Eigen::Index>& cols);

/// Add the within-station half of the clones' conditional covariance to each registered
/// F3 block: @f$C(a,b) \mathrel{+}= r_{C,a}^\top B' r_{C,b}@f$ over the pairs
/// whose rows both had clone terms (the diagonal is the stashed variance, already raised
/// by @ref networkClockCloneHarvestRow).  No-op when @p h is inert, and with the shared history
/// (@c h.shx), whose compression adds the remainder's within-station term instead.
void networkClockCloneHarvestEnd(ClockCloneHarvest& h, NetworkRunContext& ctx);

/// The run-end line of the clock clone and its INERT warning (invariant 4).  No-op off.
void networkClockCloneRunEnd(const NetworkRunContext& ctx,
                               const std::shared_ptr<spdlog::logger>& log);

}  // namespace GnssProcessor
