#pragma once
/**
 * @file NetworkClockAnchor.hpp
 * @brief Guarded temporal clock anchor for the network satellite clocks (card Q3,
 *        docs/SPEC_clock_temporal_anchor.md).
 *
 * ## Problem addressed
 *
 * Without the anchor, the emitted per-satellite clock is a random walk with a robust
 * step σ of about 9.2 cm per 30 s, against a physical change of about 2 cm
 * (carrier-phase measured, card FC-1).  The walk is estimator noise, not clock
 * physics: the per-satellite σ varies by only 1.37× (7.93 … 10.87 cm) across 32
 * satellites of three clock technologies, which matches per-epoch estimator noise and
 * no physical process.
 *
 * The cause is one line of the D2 carry fold.  The satellite clock's carried prior
 * variance is `cp.var + sat_clock_q · dt`; with `sat_clock_q = 1e-3 m²/s` that is
 * √(1e-3·30) = 17.3 cm, above the data's own per-epoch spread.  A random-walk prior
 * constrains only when its spread is below the data's; above it the prior has no
 * effect and the emitted clock is re-drawn from each epoch's data, which produces the
 * walk.
 *
 * The anchor makes that prior bind, and supplies the event response without which a
 * binding prior would violate standing invariant #1.
 *
 * ## The two exact identities it rests on
 *
 * For the scalar carry (prior N(μ, P⁻), data information 1/R, posterior
 * P⁺ = (1/P⁻ + 1/R)⁻¹):
 *
 *  1. `Var(x̂ − μ) = P⁻ − P⁺` exactly.  This is the guard's denominator, the
 *     prior-residual census's own `disp`, and Teunissen's generalized predicted
 *     residual for a predictable function (the GKF form).  `NetworkClockQc` applies
 *     the same identity to its two-state clock model (the B3 data-incest fix); only
 *     the identity is shared here, not that model (NO-GO).
 *  2. Once the carry binds, `Var(x̂_k − x̂_{k−1}) → q·Δt`.  The emitted walk is then
 *     √(q·Δt), so @c network.clock_anchor_q sets the walk target.
 *
 * ## The guard, and why it sits where it sits
 *
 * `w = (x̂ − μ − median_const) / √max(P⁻ − P⁺, σ_floor²)`, tripping when |w| exceeds
 * @c network.clock_anchor_k or when the median-removed residual exceeds the absolute
 * backstop (`clock_anchor_abs_m`).  The per-constellation median removal is required:
 * the clock common mode is a free per-epoch gauge (standing invariant #5).
 *
 * A trip decouples the satellite and, when it can, withdraws the offending prior and
 * re-solves the epoch before emission (adapt and emit; never a withhold, never a
 * de-weight).  The re-solve is skipped when `network.clock_anchor_resolve` is off,
 * when the column is prior-dominated, or when no captured clock prior exists to
 * withdraw.  It runs before the within-epoch RAIM adaptation, and that order is
 * required: a stiff prior resisting a real step moves the tension into the residuals
 * and raises σ̂₀, and RAIM would then attribute a prior fault to a satellite's or a
 * station's data and drop it.  A prior fault is resolved before the measurement-fault
 * machinery runs.
 *
 * Cost: a shared-border information delta passes through the Schur reduction
 * unchanged (@ref NetworkNormalEquations::addSharedInfo's reduction-preserving fast
 * path), so the re-solve is one datum plus a factorization of the reduced K×K system,
 * with no re-reduction and no re-assembly.
 *
 * ## Both edges (standing invariant #7)
 *
 * Per satellite the anchor is a two-state machine, ANCHORED or LOOSE:
 *
 *  - Decoupling (trip, stale carry, resume seam, datum change, or a refloat by an
 *    external QC tier) moves the satellite to LOOSE, which folds at the legacy
 *    `sat_clock_q`, the behaviour without the anchor.  That bounds what the
 *    decoupled state can accumulate: it is not unconstrained, it is on the carry that
 *    the existing `clock_runaway_check` monitor already bounds.  The edge is evented,
 *    counted, and (under @c network.clock_anchor_mark) marked on the wire per
 *    satellite, because that clock has just changed both temporal regime and value.
 *  - Re-coupling requires @c network.clock_anchor_recouple_epochs consecutive in-band
 *    tests (the re-validation) and re-seeds: the first anchored fold uses the LOOSE
 *    posterior variance, not a steady-state tight one.  It is evented and counted
 *    but not marked on the wire: re-engaging a prior whose mean is the last posterior
 *    adds information without stepping the emitted value, so a consumer has nothing
 *    to reset.
 *
 * ## Per-satellite elapsed time
 *
 * The driver's `dt` is the inter-epoch gap, and every skip path advances it while the
 * D2 carry survives untouched (a gate can withhold hundreds of consecutive epochs).
 * A binding prior folded over `dt` would treat a clock that is many minutes old as
 * 30 s old, the invariant-#1 failure.  The anchor stamps a per-satellite `t_last` at
 * every fold, inflates over `pl_now_s − t_last`, and decouples beyond
 * @c network.clock_anchor_max_gap_s (0 disables that bound).  This is the same defect
 * class as the clock model's P0-3 / R11 fix.
 *
 * ## Covariance treatment
 *
 * The anchor uses the existing D2 fold instead of adding a second prior, so under
 * `network.shared_carry_full_cov` the clock stays inside the C1 dense block and its
 * off-diagonals are retained: the independence assumption that C1 removed is not
 * re-created on the state the anchor makes stiffest.  The residual inconsistency of
 * any stiff carry is bounded by @c network.clock_anchor_var_floor_m2, the clock
 * analogue of the B4 phase-bias and code-bias floors (which exclude the clock).  A
 * refloat marginalizes the key out of the carried block and never conditions on it,
 * the treatment that @ref NetworkEstimator::addSharedPriorDense documents as exact
 * for a departed key.
 */

#include <array>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <Eigen/Dense>

#include "network/solve/NetworkEstimator.hpp"
#include "network/driver/NetworkRunTypes.hpp"
#include "network/solve/NetworkSbasis.hpp"   // NetworkSbasis::Plan (the re-solve's datum)

namespace spdlog { class logger; }

namespace GnssProcessor {

struct EpochEvents;   // network/driver/NetworkEpoch.hpp

class GnssConfig;
struct GnssTime;
struct NetworkRunContext;
struct NetworkRunTraces;

/// @brief Gauge-projected variance of each member of one constellation's
///        carried-clock group (§2.1 of card Q3).
///
/// The guard (and the census's `clk` family) standardize the common-removed residual,
/// so both halves of `P⁻ − P⁺` must be the variance of that contrast:
///
///   Var(xₛ − mean_t x_t) = C_ss − 2·mean_t C_st + mean_{t,u} C_tu.
///
/// Applied to the posterior block this gives the matched P⁺; applied to the prior it
/// gives the matched P⁻, and it is correct for both folds:
///
///  * gauge-free fold: the folded prior is `diag(var_eff) + ∞·11ᵀ` (zero information
///    along the gauge), and the contrast is orthogonal to 1, so the infinite part
///    drops out exactly and only `diag(var_eff)` remains;
///  * raw fold: the prior is `diag(var_eff)`, and the same functional gives the
///    contrast's variance directly.
///
/// The raw marginal is the variance of the raw state, which is not the quantity the
/// numerator measures.  It still contains the common-mode variance that the median
/// removed, so it overstates P⁺, understates S = P⁻ − P⁺, and makes the guard trip
/// too often.
///
/// @param cov  covariance containing the group's columns
/// @param cols the group's columns into @p cov, in member order
/// @return one projected variance per member, or empty when n < 2 (a lone clock is
///         its constellation's gauge, so there is no estimable contrast to project
///         onto, and the caller keeps the raw marginal)
std::vector<double> clockAnchorGaugeProject(const Eigen::MatrixXd& cov,
                                            const std::vector<Eigen::Index>& cols);

/// @brief The same functional for a diagonal covariance supplied directly: the prior
///        side, where C = diag(@p var).  Reduces to `v_s − 2·v_s/n + (Σ v_t)/n²`.
/// @return one projected variance per member, or empty when n < 2.
std::vector<double> clockAnchorGaugeProjectDiag(const std::vector<double>& var);

/// Rungs of the guard's dimensionless |w| ladder (@ref NetworkServiceCounters::cta_w_ge).
/// Spans the k value in use (6, the default) and the tail beyond it, so one armed run
/// reports how many rows each candidate threshold k would have tripped.
inline constexpr std::array<double, 8> kCtaWLadder{1.0, 2.0, 3.0, 4.0, 6.0, 8.0, 12.0, 20.0};

/// One satellite's phase-space guard statistic (card CTU section 14).
struct ClockAnchorPhiRow {
    bool   ok{false};
    double e{0.0};       ///< the phase clock's innovation, (c - mu_c) - sum_j w_j (p_j - mu_pj) (m)
    double e_hat{0.0};   ///< …with the constellation median removed (the clock gauge)
    double S{0.0};       ///< P- - P+ of the contrast, floored (m^2)
    bool   floored{false};
    int    n_pb{0};      ///< phase-bias columns in the functional
    // The same functional read as a state (card PVN, the phase-visible instrument); the
    // guard reads none of these.
    double Sm{0.0};         ///< the contrast-projected prior variance P- (m^2), unfloored
    double Sp{0.0};         ///< the contrast-projected posterior variance P+ (m^2), unfloored
    double d_post{0.0};     ///< the functional's posterior value c - sum_j w_j p_j (m)
    double d_post_hat{0.0}; ///< …with the constellation median removed (the clock gauge)
    /// The phase-bias signals the functional took this epoch, one per band, joined by '+'
    /// (e.g. "L1C+L2W").  The choice is per epoch (the best-determined signal of a band), so
    /// the functional's value changes definition when it does: a series of @c d_post is
    /// continuous only between changes of this string.
    std::string sigs;
};

/// Minimum-norm weights w over a satellite's phase-bias signals with sum w = 1 (blind to
/// the level) and sum w alpha = 0 (alpha_j = lambda_j^2: blind to the ionosphere direction).
/// Two bands give the ionosphere-free pair (GPS L1/L2: 2.5457, -1.5457).  Empty when all
/// signals share one band.
std::vector<double> clockAnchorPhaseClockWeights(const std::vector<double>& alpha);

/// The phase-space statistic of every satellite with a clock and two or more phase-bias
/// keys in the folded dense block (@p keys / @p cov, the fold's capture): the innovation
/// of the ionosphere-free phase clock against the folded prior means, its variance
/// P- - P+ (the folded block and the posterior, projected on the constellation contrast)
/// floored at @p floor_m^2, and the constellation median removed.  A constellation with
/// fewer than two such satellites has no contrast and yields no rows.
std::map<std::string, ClockAnchorPhiRow> clockAnchorPhaseStatistic(
    const NetworkEstimator& est, const Eigen::VectorXd& x_s,
    const std::vector<NetworkEstimator::SharedPrior>& keys, const Eigen::MatrixXd& cov,
    double floor_m);

/// Rungs of the guard's absolute |ê| ladder in metres (@ref NetworkServiceCounters::cta_e_ge).
/// The top rung is the `clock_anchor_abs_m` default (2 m), so the last column gives the
/// backstop's own realized rate.
inline constexpr std::array<double, 8> kCtaELadder{0.005, 0.01, 0.02, 0.05, 0.10, 0.30, 1.00, 2.00};

/// Disposition of one satellite's clock carry for one epoch, as decided by
/// @ref clockAnchorFoldDecide and re-formed on a carried variance by @ref clockAnchorFoldAt.
struct ClockAnchorFold {
    double var_eff{0.0};      ///< the variance to fold (already floored)
    bool   anchored{false};   ///< folded at the anchor rate (false = LOOSE, legacy rate)
    bool   floor_hit{false};  ///< the consistency floor bound this fold
    bool   decoupled{false};  ///< this fold found an anchored satellite's carry stale; the guard emits the decouple event and the mark
    /// The increment over the carried variance before the floor (m^2): cta_q·dt_sat
    /// anchored, the legacy rate over the driver's step (or over the true gap for a
    /// re-admission seed) loose.  @c var_eff is the carried variance plus this, floored
    /// when anchored (card PRM phase 1b: the decision is the increment, the base is the
    /// caller's, @ref clockAnchorFoldAt).
    double inc_m2{0.0};
    /// The interval @c inc_m2 covers (s) and its rate (m^2/s), so that
    /// @c inc_m2 = @c rate_m2ps · @c dt_s.  Card PFQ reads them: a carried clock's
    /// pivot-frame term is the pivot's rate over this clock's own interval.
    double dt_s{0.0};
    double rate_m2ps{0.0};
};

/// @brief Decide the satellite clock's carried-prior variance for this epoch, and
///        advance the per-satellite anchor bookkeeping (card Q3): @ref clockAnchorFoldDecide
///        followed by @ref clockAnchorFoldCommit.
///
/// The D2 fold no longer calls it: since card PRM phase 1b the decision is made before
/// PASS B (@ref networkCarryDecide), read by the coupling rows and by both fold branches
/// (the diagonal one and the C1 dense block's `cov_eff` diagonal, so the marginal prior on
/// the clock is the same either way, which makes the guard statistic independent of the
/// fold path), and committed at the fold.  This call is the two halves together, for a
/// caller that decides and folds at one point.
///
/// @param ctx        run context (knobs and the per-satellite anchor state, mutated)
/// @param sat        satellite id
/// @param prev_var   the stashed posterior variance (`cp.var`)
/// @param dt_epoch   the driver's inter-epoch gap (the legacy inflation basis)
/// @param pl_now_s   continuous GPS seconds of this epoch
/// @param value      the carried value being folded (recorded for the guard)
/// @return the fold decision.  With the anchor disarmed this returns
///         `{prev_var + q_clock·dt_epoch, false, false, false}`, the legacy
///         expression, byte-identical.
/// @param seed_horizon_s Re-admission continuity (network.readmit_seed_horizon_s): > 0
///        when the carry being folded is a parked pre-gap realization seeding a
///        re-registered column.  The staleness bound becomes
///        max(clock_anchor_max_gap_s, this), so an anchored satellite folds anchored over
///        its true per-satellite gap instead of decoupling to a code-only LOOSE re-draw,
///        and a LOOSE one inflates over the true gap instead of the driver's dt.
///        0 (default) keeps the unchanged Q3 rule.
ClockAnchorFold clockAnchorFoldVariance(NetworkRunContext& ctx,
                                        const std::string& sat,
                                        double prev_var, double dt_epoch,
                                        double pl_now_s, double value,
                                        double seed_horizon_s = 0.0);

/// @brief The anchor's decision for this satellite this epoch, read from the current state
///        without advancing it (card PRM phase 1b): the decision half of
///        @ref clockAnchorFoldVariance.
///
/// The coupling rows are built in PASS B, before the D2 fold runs, and the exact time update
/// (network.carry_coupling_exact_update) has to use the fold's own inflation, so the decision
/// is made there and committed at the fold.  Evaluates the anchor's rule on a copy of the
/// per-satellite state with `stale_pending` taken as cleared (@ref clockAnchorBeginEpoch
/// clears it before the fold) and returns the decision with its increment
/// (@ref ClockAnchorFold::inc_m2) and the variance on @p prev_var.  Writes nothing: no state,
/// no counter.  @ref clockAnchorFoldCommit applies it.
/// @param ctx run context (read only); the other parameters as for the fold.
ClockAnchorFold clockAnchorFoldDecide(const NetworkRunContext& ctx, const std::string& sat,
                                      double prev_var, double dt_epoch, double pl_now_s,
                                      double seed_horizon_s = 0.0);

/// @brief The anchor's decision @p f re-formed on the carried variance @p prev_var: the
///        variance is @p prev_var plus the decision's increment, floored at
///        `clock_anchor_var_floor_m2` when anchored (card PRM phase 1b).
///
/// The one implementation of the anchor's floor: the decision applies it on the variance it
/// was made on, and the fold on the variance it folds (a clock-pivot handover re-expresses
/// the variance in between).
/// @param ctx      run context (read only: the floor)
/// @param f        the decision (its increment and flags are kept)
/// @param prev_var the carried variance (m^2)
/// @return @p f with @c var_eff and @c floor_hit on @p prev_var.
ClockAnchorFold clockAnchorFoldAt(const NetworkRunContext& ctx, const ClockAnchorFold& f,
                                  double prev_var);

/// @brief Apply the anchor's fold decision @p f to the per-satellite state and counters,
///        the bookkeeping half of @ref clockAnchorFoldVariance (card PRM phase 1b).
///
/// Marks a decoupling (`stale_pending`, which the guard turns into the edge), counts the fold
/// (anchored or loose, the floor hits, per constellation) and stamps the satellite's last
/// fold (time, value, variance, folded, folded anchored).  Called by the fold only, so an
/// epoch that ends before the fold advances nothing.  A no-op with the anchor disarmed.
/// @param ctx      run context (state and counters mutated)
/// @param sat      satellite id
/// @param f        the fold on the variance the fold folds (@ref clockAnchorFoldAt)
/// @param pl_now_s continuous GPS seconds of this epoch
/// @param value    the carried value being folded (recorded for the guard)
void clockAnchorFoldCommit(NetworkRunContext& ctx, const std::string& sat,
                           const ClockAnchorFold& f, double pl_now_s, double value);

/// @brief Mark every satellite as not yet folded for a new epoch, and clear the
///        previous epoch's trip set (card Q3).
///
/// Call once per epoch before the D2 fold.  Separate from the fold call because a
/// satellite absent from `prev_carry` folds nothing and must not be tested by the
/// guard.
void clockAnchorBeginEpoch(NetworkRunContext& ctx);

/// @brief The guard (card Q3): test every folded satellite's generalized predicted
///        residual, decouple and mark anchored trips, withdraw their priors and
///        re-solve, and run the LOOSE→ANCHORED re-validation.
///
/// Call immediately after the primary `est.solve()` and before the within-epoch RAIM
/// adaptation (see the file comment).  No-op unless `ctx.knobs.cta_on`.
///
/// @param ctx                 run context (state and counters, mutated)
/// @param config_             config (unused; the seam signature is uniform)
/// @param log                 logger
/// @param traces              event sink
/// @param se_time             epoch time
/// @param est                 the estimator holding this epoch's solved system
/// @param plan                the S-basis plan (its datum drives the re-solve)
/// @param x_s                 in/out: the shared solution; replaced by the re-solved
///                            vector when a trip is value-corrected
/// @param ws                  epoch workspace; `cap_priors` / `cap_dense_*` are
///                            updated in step so that a later RAIM replay cannot
///                            re-inject a withdrawn prior
/// @param registered_clock_sats  this epoch's clock columns
/// @param clock_refloat_sats  external QC refloats already known this epoch (input
///                            only: the guard does not write into it, so the CB/PB
///                            stash refloat policy is unchanged)
/// @param datum_changed       a datum change this epoch; the affected anchored
///                            satellites decouple before the test
/// @param changed_consts      constellations whose datum changed (scopes
///                            @p datum_changed under a scoped handover)
/// @param events              the epoch's events, read: the handover's re-expressed
///                            constellations and dense record
void networkClockAnchorGuard(NetworkRunContext& ctx, const GnssConfig& config_,
                             const std::shared_ptr<spdlog::logger>& log,
                             NetworkRunTraces& traces, const GnssTime& se_time,
                             NetworkEstimator& est,
                             const NetworkSbasis::Plan& plan,
                             Eigen::VectorXd& x_s, EpochWorkspace& ws,
                             const std::vector<std::string>& registered_clock_sats,
                             const std::set<std::string>& clock_refloat_sats,
                             bool datum_changed,
                             const std::set<char>& changed_consts, const EpochEvents& events);

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the Q3 guarded temporal clock anchor.
 *
 * The Q3 guarded temporal clock anchor: epochs, carried clocks, trips, re-solves,
 * re-coupling edges, wire marks and the guard ladder.
 *
 * Gate: returns 0 at once unless `ctx.knobs.cta_on`.
 *
 * @param v The run at its end (network/runend/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkClockAnchorRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

// ---- Solve stages 10 and 12 helpers (card NOM3 step SG) ----

struct NetworkRun;
struct NetworkEpoch;
struct NetworkPostFitHandoff;   // network/solve/NetworkSolveGlue.hpp
struct SolveDatum;              // network/solve/NetworkSolveGlue.hpp

/**
 * @brief The cold-start a-priori satellite-clock Tikhonov anchor (the last post-gate fold of
 *        stage 10): each clock column pulled toward the a-priori product clock when the resolved
 *        sigma binds (< 100 m) and the datum is not broadcast-anchored.
 * @param run The run: the estimator is folded.
 * @param ep  The epoch, read: columns (registered_clock_sats).
 * @param ws  [in/out] The replay capture (cap_priors, cap_prior_kinds).
 */
void networkFoldColdClockAnchor(NetworkRun& run, const NetworkEpoch& ep, EpochWorkspace& ws);

/**
 * @brief Stage 12 of the network solve: the Q3 guard between the primary solve and the
 *        within-epoch tier, one call to @ref networkClockAnchorGuard.
 * @param run     The run: the estimator may re-solve; ctx carry and the traces are written.
 * @param ep      The epoch, read: se, columns (registered_clock_sats).
 * @param datum   The clock datum (read: plan, datum_changed, changed_consts).
 * @param handoff The post-fit hand-off (read: clock_refloat_sats, empty at this call).
 * @param x_s     [in/out] The shared solution, replaced by a guard re-solve.
 * @param ws      [in/out] The replay capture.
 */
void networkClockAnchorGuardEpoch(NetworkRun& run, const NetworkEpoch& ep, const SolveDatum& datum,
                                  const NetworkPostFitHandoff& handoff, Eigen::VectorXd& x_s,
                                  EpochWorkspace& ws);

}  // namespace GnssProcessor
