#pragma once
/**
 * @file ClockAnchor.hpp
 * @brief Guarded temporal clock anchor for the network satellite clocks.
 *
 * ## Problem addressed
 *
 * Without the anchor, the emitted per-satellite clock is a random walk with a robust
 * step σ of about 9.2 cm per 30 s, against a physical change of about 2 cm
 * (carrier-phase measured).  The walk is estimator noise, not clock
 * physics: the per-satellite σ varies by only 1.37× (7.93 … 10.87 cm) across 32
 * satellites of three clock technologies, which matches per-epoch estimator noise and
 * no physical process.
 *
 * The cause is one line of the satellite-clock carry fold.  The clock's carried prior
 * variance is `cp.var + sat_clock_q · dt`; with `sat_clock_q = 1e-3 m²/s` that is
 * √(1e-3·30) = 17.3 cm, above the data's own per-epoch spread.  A random-walk prior
 * constrains only when its spread is below the data's; above it the prior has no
 * effect and the emitted clock is re-drawn from each epoch's data, which produces the
 * walk.
 *
 * The anchor makes that prior bind, and supplies the event response without which a
 * binding prior carried past a clock jump would warp the solution.
 *
 * ## The two exact identities it rests on
 *
 * For the scalar carry (prior N(μ, P⁻), data information 1/R, posterior
 * P⁺ = (1/P⁻ + 1/R)⁻¹):
 *
 *  1. `Var(x̂ − μ) = P⁻ − P⁺` exactly.  This is the guard's denominator, the
 *     prior-residual census's own `disp`, and Teunissen's generalized predicted
 *     residual for a predictable function.  `networkClockQc` applies the same
 *     identity to its two-state clock model; only the identity is shared here, not
 *     that model.
 *  2. Once the carry binds, `Var(x̂_k − x̂_{k−1}) → q·Δt`.  The emitted walk is then
 *     √(q·Δt), so @c network.clock_anchor_q sets the walk target.
 *
 * ## The guard, and why it sits where it sits
 *
 * `w = (x̂ − μ − median_const) / √max(P⁻ − P⁺, σ_floor²)`, tripping when |w| exceeds
 * @c network.clock_anchor_k or when the median-removed residual exceeds the absolute
 * backstop (`clock_anchor_abs_m`).  The per-constellation median removal is required:
 * the clock common mode is a free per-epoch gauge.  With @c network.clock_anchor_guard_space
 * `phase`, the |w| test reads the statistic of @ref GnssProcessor::clockAnchorPhaseStatistic "clockAnchorPhaseStatistic" instead.
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
 * unchanged (@ref GnssProcessor::NetworkNormalEquations::addSharedInfo "NetworkNormalEquations::addSharedInfo"'s reduction-preserving fast
 * path), so the re-solve is one datum plus a factorization of the reduced K×K system,
 * with no re-reduction and no re-assembly.
 *
 * ## Both edges
 *
 * Per satellite the anchor is a two-state machine, ANCHORED or LOOSE:
 *
 *  - Decoupling (trip, stale carry, datum change, or a refloat by an
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
 * carry survives untouched (a gate can withhold hundreds of consecutive epochs).
 * A binding prior folded over `dt` would treat a clock that is many minutes old as
 * 30 s old.  The anchor stamps a per-satellite `t_last` at
 * every fold, inflates over `pl_now_s − t_last`, and decouples beyond
 * @c network.clock_anchor_max_gap_s (0 disables that bound).
 *
 * ## Covariance treatment
 *
 * The anchor uses the existing carry fold instead of adding a second prior, so under
 * `network.shared_carry_full_cov` the clock stays inside the dense carried block and its
 * off-diagonals are retained: the independence assumption that the dense block removes
 * is not re-created on the state the anchor makes stiffest.  The residual inconsistency
 * of any stiff carry is bounded by @c network.clock_anchor_var_floor_m2, the clock
 * analogue of the phase-bias and code-bias floors (@c network.shared_carry_var_floor_pb_m2
 * and @c network.shared_carry_var_floor_cb_m2, which exclude the clock).  A
 * refloat marginalizes the key out of the carried block and never conditions on it,
 * the treatment that @ref GnssProcessor::NetworkEstimator::addSharedPriorDense "NetworkEstimator::addSharedPriorDense" documents as exact
 * for a departed key.
 */

#include <array>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <Eigen/Dense>

#include "network/solve/Estimator.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/solve/Sbasis.hpp"   // NetworkSbasis::Plan (the re-solve's datum)

namespace spdlog { class logger; }

namespace GnssProcessor {

struct EpochEvents;   // network/driver/Epoch.hpp

class GnssConfig;
struct GnssTime;
struct NetworkRunContext;
struct NetworkRunTraces;

/// @brief Gauge-projected variance of each member of one constellation's
///        carried-clock group.
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
/// @param var  the diagonal of the covariance: one variance per member (m^2)
/// @return one projected variance per member, or empty when n < 2.
std::vector<double> clockAnchorGaugeProjectDiag(const std::vector<double>& var);

/// Rungs of the guard's dimensionless |w| ladder (@c NetworkServiceCounters::cta_w_ge).
/// Spans the k value in use (6, the default) and the tail beyond it, so one armed run
/// reports how many rows each candidate threshold k would have tripped.
inline constexpr std::array<double, 8> kCtaWLadder{1.0, 2.0, 3.0, 4.0, 6.0, 8.0, 12.0, 20.0};

/// One satellite's phase-space guard statistic.
struct ClockAnchorPhiRow {
    bool   ok{false};    ///< the statistic is usable: the floored variance @c S and @c e_hat are finite
    double e{0.0};       ///< the phase clock's innovation, (c - mu_c) - sum_j w_j (p_j - mu_pj) (m)
    double e_hat{0.0};   ///< …with the constellation median removed (the clock gauge)
    double S{0.0};       ///< P- - P+ of the contrast, floored (m^2)
    bool   floored{false};   ///< the raw contrast variance P- - P+ was not above the floor, so @c S is the floor
    int    n_pb{0};      ///< phase-bias columns in the functional
    // The same functional read as a state (the phase-visible instrument); the
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
    // The functional's parts on the pass that computed it (measure-only: the guard trace
    // reads them apart, so a trip's statistic is attributed to the clock's own innovation or
    // to one phase-bias key's).  In the order of @ref sigs.
    double dclk{0.0};            ///< the clock's innovation x_c - prior_c (m), before the median
    double clk_sd{0.0};          ///< the clock key's folded standard deviation (m)
    std::vector<double> w;       ///< the weight of each chosen signal
    std::vector<double> dpb;     ///< each chosen phase-bias key's innovation x_p - prior_p (m)
    std::vector<double> pb_sd;   ///< each chosen key's folded standard deviation (m)
};

/// @brief Minimum-norm weights w over a satellite's phase-bias signals with sum w = 1 (blind to
/// the level) and sum w alpha = 0 (alpha_j = lambda_j^2: blind to the ionosphere direction).
/// Two bands give the ionosphere-free pair (GPS L1/L2: 2.5457, -1.5457).  Empty when all
/// signals share one band.
/// @param alpha  per signal, the squared carrier wavelength lambda_j^2 (m^2)
/// @return one weight per signal, in the order of @p alpha (dimensionless); empty when there
///         are fewer than two signals or all of them share one band
std::vector<double> clockAnchorPhaseClockWeights(const std::vector<double>& alpha);

/**
 * @brief The phase statistic's parts as the guard trace's trailing columns (measure-only).
 *
 * @param row  The satellite's row this pass, or null when no statistic was formed.
 * @return `sigs,dclk,clk_sd,w1,dpb1,pb1_sd,w2,dpb2,pb2_sd` (nine fields, empty when not formed).
 */
std::string clockAnchorGuardPartsCsv(const ClockAnchorPhiRow* row);

/// What the phase-mode statistic test decides for one anchored row.
enum class ClockAnchorPhaseVerdict {
    InBand,    ///< no phase statistic, or |w_phi| within k
    Trip,      ///< |w_phi| over k, and the clock carries a share (or no share is required)
    Withheld,  ///< |w_phi| over k with the clock still: the clock-share requirement withholds the trip
};

/**
 * @brief The phase-mode statistic test with the clock-share requirement
 *        (@c network.clock_anchor_clock_k).
 *
 * A phase statistic over @p k is a trip when @p clock_k is 0 (the requirement off: the
 * decision as before) or when the clock-space statistic carries a share of the event,
 * |w_clk| >= @p clock_k.  A phase statistic over @p k with |w_clk| below @p clock_k is a
 * phase-bias-key event with the clock still, and is withheld.  The absolute backstop and the
 * satellite clock event's forced trip are decided apart and are not inputs here.
 *
 * @param phi_ok   The row has a phase statistic this pass.
 * @param w_phi    The phase statistic e_phi / sqrt(S_phi).
 * @param w_clk    The clock-space statistic (e - med) / sqrt(S) of the same row.
 * @param k        @c network.clock_anchor_k.
 * @param clock_k  @c network.clock_anchor_clock_k (0 = no requirement).
 * @return InBand, Trip or Withheld.
 */
ClockAnchorPhaseVerdict clockAnchorPhaseTripVerdict(bool phi_ok, double w_phi, double w_clk,
                                                    double k, double clock_k);

/**
 * @brief The phase-bias keys' share of a phase statistic, (-sum_j w_j dpb_j) / e_hat
 *        (measure-only: the withheld trip's event detail).
 *
 * e = dclk - sum_j w_j dpb_j, so the keys' part is -sum_j w_j dpb_j; the share is read
 * against the median-removed e_hat the test standardized, as the guard trace's parts are.
 * @param row  The satellite's phase row (its @c w, @c dpb and @c e_hat).
 * @return The share, or 0 when e_hat is zero or the parts are missing.
 */
double clockAnchorPhaseKeysShare(const ClockAnchorPhiRow& row);

/**
 * @brief The phase-space statistic of every satellite with a clock and two or more phase-bias
 * keys in the folded dense block (@p keys / @p cov, the fold's capture): the innovation
 * of the ionosphere-free phase clock against the folded prior means, its variance
 * P- - P+ (the folded block and the posterior, projected on the constellation contrast)
 * floored at @p floor_m^2, and the constellation median removed.  A constellation with
 * fewer than two such satellites has no contrast and yields no rows.
 * @param est    the estimator holding this epoch's solved system (its shared covariance is the
 *               posterior side and its column lookups place each key)
 * @param x_s    the shared solution of that solve (the posterior values)
 * @param keys   the dense fold's captured keys (a clone key is not a satellite's clock)
 * @param cov    the fold's captured prior covariance over @p keys (m^2); a size that does not
 *               match @p keys yields no rows
 * @param floor_m  the floor of the contrast variance S = P- - P+, as a standard deviation (m);
 *               the variance is floored at its square
 * @return satellite id to its statistic row.  A satellite without a clock, with fewer than two
 *         usable bands, or alone in its constellation has no entry.
 */
std::map<std::string, ClockAnchorPhiRow> clockAnchorPhaseStatistic(
    const NetworkEstimator& est, const Eigen::VectorXd& x_s,
    const std::vector<NetworkEstimator::SharedPrior>& keys, const Eigen::MatrixXd& cov,
    double floor_m);

/// Rungs of the guard's absolute |ê| ladder in metres (@c NetworkServiceCounters::cta_e_ge).
/// The top rung is the `clock_anchor_abs_m` default (2 m), so the last column gives the
/// backstop's own realized rate.
inline constexpr std::array<double, 8> kCtaELadder{0.005, 0.01, 0.02, 0.05, 0.10, 0.30, 1.00, 2.00};

/// Disposition of one satellite's clock carry for one epoch, as decided by
/// @ref clockAnchorFoldDecide and re-formed on a carried variance by @ref clockAnchorFoldAt.
struct ClockAnchorFold {
    double var_eff{0.0};      ///< the variance to fold (already floored)
    bool   anchored{false};   ///< folded at the anchor rate (false = LOOSE, legacy rate)
    bool   floor_hit{false};  ///< the consistency floor bound this fold
    bool   decoupled{false};  ///< this fold found an anchored satellite's carry stale, or took it LOOSE for a satellite clock event; the guard emits the decouple event and the mark
    /// The satellite was ANCHORED and the IF-TD pools read a clock event for it this epoch
    /// (@c network.cycle_slip.if_phase_td_sat_clock_event), so the fold took the LOOSE legacy
    /// increment and set @c decoupled.  The commit records it as @c slip_clk_pending instead
    /// of @c stale_pending, and the guard decouples the satellite with reason @c slip_clk.
    bool   slip_clk{false};
    /// The increment over the carried variance before the floor (m^2): cta_q·dt_sat
    /// anchored, the legacy rate over the driver's step (or over the true gap for a
    /// re-admission seed) loose.  @c var_eff is the carried variance plus this, floored
    /// when anchored (the decision is the increment, the base is the
    /// caller's, @ref clockAnchorFoldAt).
    double inc_m2{0.0};
    /// The interval @c inc_m2 covers (s) and its rate (m^2/s), so that
    /// @c inc_m2 = @c rate_m2ps · @c dt_s.  The pivot-frame process noise reads them: a carried clock's
    /// pivot-frame term is the pivot's rate over this clock's own interval.
    double dt_s{0.0};
    double rate_m2ps{0.0};   ///< the rate of @c inc_m2 over @c dt_s (m^2/s)
};

/// @brief Decide the satellite clock's carried-prior variance for this epoch, and
///        advance the per-satellite anchor bookkeeping: @ref GnssProcessor::clockAnchorFoldDecide "clockAnchorFoldDecide"
///        followed by @ref GnssProcessor::clockAnchorFoldCommit "clockAnchorFoldCommit".
///
/// The carry fold does not call it: the decision is made before
/// PASS B (@ref GnssProcessor::networkCarryDecide "networkCarryDecide"), read by the coupling rows and by both fold branches
/// (the diagonal one and the dense block's `cov_eff` diagonal, so the marginal prior on
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
///        0 (default) keeps the unchanged rule.
ClockAnchorFold clockAnchorFoldVariance(NetworkRunContext& ctx,
                                        const std::string& sat,
                                        double prev_var, double dt_epoch,
                                        double pl_now_s, double value,
                                        double seed_horizon_s = 0.0);

/// @brief The anchor's decision for this satellite this epoch, read from the current state
///        without advancing it: the decision half of
///        @ref GnssProcessor::clockAnchorFoldVariance "clockAnchorFoldVariance".
///
/// The coupling rows are built in PASS B, before the carry fold runs, and the exact time update
/// (network.carry_coupling_exact_update) has to use the fold's own inflation, so the decision
/// is made there and committed at the fold.  Evaluates the anchor's rule on a copy of the
/// per-satellite state with `stale_pending` taken as cleared (@ref GnssProcessor::clockAnchorBeginEpoch "clockAnchorBeginEpoch"
/// clears it before the fold) and returns the decision with its increment
/// (@ref GnssProcessor::ClockAnchorFold::inc_m2 "ClockAnchorFold::inc_m2") and the variance on @p prev_var.  Writes nothing: no state,
/// no counter.  @ref GnssProcessor::clockAnchorFoldCommit "clockAnchorFoldCommit" applies it.
/// @param ctx run context (read only); the other parameters as for the fold.
/// @param sat           satellite id
/// @param prev_var      the stashed posterior variance (m^2)
/// @param dt_epoch      the driver's inter-epoch gap (s), the legacy inflation basis
/// @param pl_now_s      continuous GPS seconds of this epoch
/// @param seed_horizon_s  re-admission continuity horizon (s), as for
///        @ref GnssProcessor::clockAnchorFoldVariance "clockAnchorFoldVariance"; 0 keeps the unchanged rule.
/// @param slip_clk the IF-TD pools read a satellite clock event for @p sat this epoch
///        (@c EpochEvents::Slip::sat_clock_step).  An ANCHORED satellite then folds LOOSE
///        exactly as a stale carry does (the legacy increment, no floor) with
///        @ref GnssProcessor::ClockAnchorFold::decoupled "ClockAnchorFold::decoupled" and @ref GnssProcessor::ClockAnchorFold::slip_clk "ClockAnchorFold::slip_clk" set, so the
///        stiff prior never meets the step and nothing has to be withdrawn.  A LOOSE
///        satellite is unchanged.  false (the default) is the unchanged rule.
/// @return the decision: the increment over @p prev_var with its interval and rate, the
///         resulting @c var_eff, and @c decoupled / @c slip_clk when a stale anchored carry or
///         a satellite clock event takes it LOOSE.  With the anchor disarmed it is the legacy
///         increment, @c var_eff = @p prev_var plus the clock process noise times @p dt_epoch.
ClockAnchorFold clockAnchorFoldDecide(const NetworkRunContext& ctx, const std::string& sat,
                                      double prev_var, double dt_epoch, double pl_now_s,
                                      double seed_horizon_s = 0.0, bool slip_clk = false);

/// @brief The anchor's decision @p f re-formed on the carried variance @p prev_var: the
///        variance is @p prev_var plus the decision's increment, floored at
///        `clock_anchor_var_floor_m2` when anchored.
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
///        the bookkeeping half of @ref GnssProcessor::clockAnchorFoldVariance "clockAnchorFoldVariance".
///
/// Marks a decoupling (`stale_pending`, or `slip_clk_pending` for a satellite clock event; the
/// guard turns either into the edge), counts the fold
/// (anchored or loose, the floor hits, per constellation) and stamps the satellite's last
/// fold (time, value, variance, folded, folded anchored).  Called by the fold only, so an
/// epoch that ends before the fold advances nothing.  A no-op with the anchor disarmed.
/// @param ctx      run context (state and counters mutated)
/// @param sat      satellite id
/// @param f        the fold on the variance the fold folds (@ref GnssProcessor::clockAnchorFoldAt "clockAnchorFoldAt")
/// @param pl_now_s continuous GPS seconds of this epoch
/// @param value    the carried value being folded (recorded for the guard)
void clockAnchorFoldCommit(NetworkRunContext& ctx, const std::string& sat,
                           const ClockAnchorFold& f, double pl_now_s, double value);

/// @brief Mark every satellite as not yet folded for a new epoch, and clear the
///        previous epoch's trip set.
///
/// Call once per epoch before the carry fold.  Separate from the fold call because a
/// satellite absent from `prev_carry` folds nothing and must not be tested by the
/// guard.
/// @param ctx  run context (the per-satellite folded and pending marks and the trip sets are
///             reset; a no-op unless the anchor is armed)
void clockAnchorBeginEpoch(NetworkRunContext& ctx);

/// @brief The guard: test every folded satellite's generalized predicted
///        residual, decouple and mark anchored trips, withdraw their priors and
///        re-solve, and run the LOOSE to ANCHORED re-validation.
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
///                            constellations and dense record, and the satellites the
///                            IF-TD pools read as a clock event
///                            (@c events.slip.sat_clock_step).  The fold already took an
///                            anchored such satellite LOOSE (@ref GnssProcessor::clockAnchorFoldDecide "clockAnchorFoldDecide"), and
///                            the guard decouples it with reason @c slip_clk before the test
///                            (counted in @c cta_trip_slip_clk, no re-solve).  In the test a
///                            LOOSE such satellite has its in-band run reset, and an anchored
///                            one (a fold path that did not read the decision) is tripped as
///                            a backstop with reason @c slip_clk_late.  A trip of the test
///                            (statistic, backstop or @c slip_clk_late) and a @c slip_clk
///                            decoupling are recorded in @c ctx.carry.clk_anchor.stepped.
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

// ---- Run-end reports ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the guarded temporal clock anchor.
 *
 * Epochs, carried clocks, trips, re-solves, re-coupling edges, wire marks and the
 * guard ladder.
 *
 * Gate: returns 0 at once unless `ctx.knobs.cta_on`.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkClockAnchorRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

// ---- Solve stages 10 and 12 helpers ----

struct NetworkRun;
struct NetworkEpoch;
struct NetworkPostFitHandoff;   // network/solve/SolveGlue.hpp
struct SolveDatum;              // network/solve/SolveGlue.hpp

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
 * @brief Stage 12 of the network solve: the clock anchor guard between the primary solve and
 *        the within-epoch tier, one call to @ref GnssProcessor::networkClockAnchorGuard "networkClockAnchorGuard".
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

/// @brief Run-scope counters of ClockAnchor, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct ClockAnchorCounters {
    // --- Guarded temporal clock anchor.
    // `cta_folded_anchored == 0` on an armed run means the
    // mechanism is inert however clean the log reads, so every disposition is counted
    // against a denominator.  Trips are split by reason because the reasons are not
    // interchangeable evidence: a `gap` trip shows the staleness guard works, an `nis`
    // trip shows the detector works, and a run that only ever shows `datum` or `ext`
    // has tested neither.
    long long cta_epochs = 0;            ///< epochs the anchor seam ran (denominator)
    long long cta_folded_anchored = 0;   ///< (sat,epoch) clock priors folded at the anchor variance
    long long cta_folded_loose = 0;      ///< (sat,epoch) clock priors folded at the legacy variance (loose)
    long long cta_floor_hits = 0;        ///< folds where the variance floor bound
    long long cta_trip_nis = 0;          ///< decouples: |w| over network.clock_anchor_k
    long long cta_trip_abs = 0;          ///< decouples: |ê| over network.clock_anchor_abs_m
    long long cta_trip_slip_clk = 0;     ///< decouples: the IF-TD pools read a satellite clock event (network.cycle_slip.if_phase_td_sat_clock_event) and the fold took the anchored clock LOOSE; 0 with the knob off
    long long cta_trip_slip_clk_late = 0; ///< trips of the test's backstop: an event satellite whose fold was nevertheless anchored (a fold path that did not read the decision); 0 expected, and 0 with the knob off
    long long cta_trip_gap = 0;          ///< decouples: per-sat elapsed time over network.clock_anchor_max_gap_s
    long long cta_trip_datum = 0;        ///< decouples: a datum change retired this satellite's frame
    long long cta_trip_ext = 0;          ///< decouples: an external tier (clock monitor / RAIM) already refloated this satellite
    long long cta_resolves = 0;          ///< in-epoch re-solves executed (the adapt-and-emit half)
    long long cta_resolve_deferred_coupled = 0;  ///< tripped clocks whose value correction was deferred to the next epoch because the exact coupling rows reference their own column (network.clock_anchor_defer_coupled); 0 with the knob off
    long long cta_resolve_deferred_fault = 0;    ///< the fault-class (nis, abs) share of cta_resolve_deferred_coupled, the rest being backstop trips; the run-end "ZERO in-epoch re-solves" warning leaves these out
    long long cta_resolve_failed = 0;    ///< in-epoch re-solves that threw and were reverted to the anchored solution
    long long cta_datum_kept = 0;           ///< anchored clocks kept engaged at a dense-transformed handover (no datum decouple)
    long long cta_datum_kept_refused = 0;   ///< armed datum decouples the keep rule refused (no dense edge, no row, a loose fold)
    long long cta_recouples = 0;         ///< re-coupling edges: a loose satellite re-earned the anchor
    long long cta_marks = 0;             ///< per-sat wire discontinuities queued at a decoupling edge
    double    cta_max_w = 0.0;           ///< largest |w| seen (any disposition)
    double    cta_max_w_tow = 0.0;       ///< …its epoch
    std::string cta_max_w_sat;           ///< …its satellite
    // --- The guard's own realized distribution (the k derivation).
    // `k` is read off a measured |w| ladder, and the ladder moves with q: the
    // denominator √(P⁻ − P⁺) shrinks as the anchor tightens, so a fixed k means a
    // different number of metres at every rung.  Without these, re-deriving k per
    // rung would need a second, disarmed run per rung.  Accumulated over anchored rows
    // on the first guard pass only (a later pass re-tests survivors against a moved
    // median and would count them twice).
    //
    // The population is truncated across epochs, not within one: a tripped satellite
    // goes loose and stops contributing until it re-couples, so the guard's own action
    // suppresses the upper tail.  For the untruncated null, run an arm with
    // `clock_anchor_k` beyond the top rung and `clock_anchor_abs_m: 0` (the guard
    // disarmed), the only reading that calibrates k without the truncation.
    long long cta_w_rows = 0;            ///< anchored rows contributing to the two ladders below (denominator)
    std::array<long long, 8> cta_w_ge{}; ///< |w| ≥ @ref kCtaWLadder[i]: the dimensionless ladder k is read off
    std::array<long long, 8> cta_e_ge{}; ///< |ê| ≥ @ref kCtaELadder[i] (m): the ladder the absolute backstop's rail is read off
    double    cta_sum_sqrt_s = 0.0;      ///< Σ √S over those rows, so mean √S = the metres that one unit of k buys at this rung
    double    cta_sum_abs_e = 0.0;       ///< Σ |ê| over those rows (m)
    // The phase-space statistic (network.clock_anchor_guard_space).
    long long cta_phi_rows = 0;          ///< anchored pass-0 rows with a phase statistic
    long long cta_phi_unavail = 0;       ///< rows tested without one (no dense clock + two-band PB prior this epoch): abs only in phase mode
    long long cta_phi_floor_rows = 0;    ///< rows whose phase sqrt(S) sat at its floor
    std::array<long long, 8> cta_phi_w_ge{};   ///< |w_phi| >= kCtaWLadder[i]
    std::array<long long, 8> cta_phi_e_ge{};   ///< |e_phi| >= kCtaELadder[i] (m)
    double    cta_phi_sum_w2 = 0.0;      ///< sum w_phi^2 (the phase statistic's own sigma0)
    long long cta_x_both = 0;            ///< anchored pass-0 rows over k on both statistics
    long long cta_x_clk_only = 0;        ///< …on the clock statistic only (the level's motion)
    long long cta_x_phi_only = 0;        ///< …on the phase statistic only
    long long cta_trip_phi = 0;          ///< decouples decided by the phase statistic (phase mode)
    // The clock-share requirement (network.clock_anchor_clock_k).  0 everywhere with the knob off;
    // "0 withheld" is also the normal reading of an armed quiet day, not inert wiring.
    long long cta_phi_withheld = 0;          ///< (sat, epoch) phase trips withheld (|w_phi| over k, |w_clk| below clock_k, no other trip cause)
    long long cta_phi_withheld_episodes = 0; ///< withhold episodes opened: a satellite's first withheld epoch after one without
    long long cta_phi_withheld_end_band = 0; ///< episodes closed with the satellite still anchored (the phase statistic back within k, or no row that epoch)
    long long cta_phi_withheld_end_trip = 0; ///< episodes closed by a decoupling of the satellite in the closing epoch (a trip with a clock share, the backstop or another cause)
    int       cta_phi_withheld_max_run = 0;  ///< the longest episode (consecutive withheld epochs)
    double    cta_phi_withheld_max_w_phi = 0.0;  ///< the largest |w_phi| among the withheld trips
    double    cta_phi_withheld_max_w_clk = 0.0;  ///< the largest |w_clk| among the withheld trips (below clock_k by construction)
    double    cta_sum_w2 = 0.0;          ///< Σ w², so √(Σw²/n) is the guard's own σ̂₀, a same-statistic cross-check on the census's σ̂₀_clk
    long long n_readmit_seed_moved    = 0;  ///< nis/abs trips of the anchor guard within kReadmitVerifyWindowS of a clock seed: the data rejected the seed
};

}  // namespace GnssProcessor
