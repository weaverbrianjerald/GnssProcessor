#pragma once
/**
 * @file NetworkHandoverStransform.hpp
 * @brief The clock-pivot handover S-transform (queue 1(b);
 *        docs/SPEC_pivot_visibility_gauge_defect.md §8; the measured
 *        motivation is in docs/SPEC_event_harness.md §19).
 *
 * ## Problem
 *
 * The per-constellation clock gauge is realized by a hard Dirichlet pin on one
 * satellite's clock column (@ref NetworkSbasis::buildSharedDatum, `Pin{…, 0.0}`).
 * When that satellite sets, the pin moves to a successor and the estimable clock
 * solution of the whole constellation re-references.  Without this transform the
 * producer skips the D2 shared-carry fold for the affected keys for one epoch
 * ("one epoch of free fall", §8.2): stiff states (clock levels, code bias at
 * q ≈ 0, phase bias) solve from data alone once, re-level, and re-stash in the new
 * datum.  That one-epoch re-level is the product discontinuity.
 *
 * On the 60-station storm bed (§19.5) the changed constellation's code-bias and
 * phase-bias step rms rise by more than two orders of magnitude at a handover and
 * decay over about 30 minutes, and the consumer is told to reset the code-bias and
 * phase-bias keys of that constellation.  This is not a datum move: the code-bias
 * and phase-bias gauges are realized independently of the clock pivot (§8.1 and
 * @ref NetworkSbasis::clockNullDirection), and their mean steps are about 1e-6 m.
 * It is a carry that is discarded instead of re-expressed.
 *
 * ## The transform (§8.1)
 *
 * Any minimally-constrained solution maps exactly to any other by the LASER
 * S-transform.  A pin swap p→q inside one constellation reduces it to a rank-1
 * update along that constellation's structural clock null direction @f$v_c@f$:
 *
 * @f[
 *   c_0 = \frac{x_q^{\text{pre}}}{v_c[q]}, \qquad
 *   x_i \leftarrow x_i - v_c[i]\,c_0, \qquad
 *   \mathrm{var}_i \leftarrow \mathrm{var}_i + v_c[i]^2\,\mathrm{var}_q
 * @f]
 *
 * with q the incoming pivot and @f$x_q^{\text{pre}}@f$ its carried level in the
 * outgoing frame.  Afterwards the incoming pivot's carried level is exactly 0 (its
 * pinned value), and every between-satellite difference inside the constellation is
 * unchanged to machine precision.  The carried information stays valid; only the
 * datum it is expressed in changes.
 *
 * The implementation follows two rules:
 *
 *  1. **@f$v_c@f$ is the structural direction that @ref NetworkSbasis owns.**  It is
 *     not written out here and is not an eigenvector (eigenvectors of equal
 *     eigenvalue mix; structural membership is exact and keeps per-satellite
 *     identity).  Only the constellation's satellite-clock columns have a nonzero
 *     component, so the code-bias and phase-bias carries are not transformed.
 *     They are still released (below), which is where most of the measured benefit
 *     comes from.
 *  2. **Dropping the cross term is conservative.**  The exact per-column variance
 *     of @f$x_i - v_i c_0@f$ is @f$\mathrm{var}_i + v_i^2\mathrm{var}_q -
 *     2 v_i \mathrm{cov}(i,q)@f$.  Clocks are positively correlated in the
 *     dominant common-mode case, so omitting the cross term overstates the
 *     variance.  The full @f$S D S^\top@f$ on the carry blob is deferred by §8.8;
 *     §8.7 gate 6 watches for the failure signature the omission would produce.
 *
 * ## The release (§8.4 INC-1)
 *
 * The transform alone changes nothing in the solve: a re-expressed prior that is
 * still skipped folds nothing.  The seam therefore records the transformed
 * constellations in @c NetworkCarryState::sts_consts.  For exactly those
 * constellations the RTA-2 `handover_affects` skips are released in
 * @ref networkInjectD2CarryPrior (diagonal and dense branches, plus the READMIT
 * parked-entry drop) and in `networkStashStatus` (the VCE monitor and the
 * prior-residual census `folded` bit); each released row counts in
 * @c sts_rows_retained.  Per invariant #4, transforms applied with
 * `sts_rows_retained == 0` mean the release is mis-wired, whatever the log shows.
 *
 * The knob is `network.handover_stransform` (default off).  It is inert without
 * `network.scoped_handover_response`, and the config loader warns in that case.
 *
 * ## Deliberately not released
 *
 *  - The **clock-monitor jump test** (`NetworkClockQc`, its `handover_affects`
 *    suppression).  Under the pin datum its statistic is the raw clock, and the
 *    emitted clock does step by the datum delta at a handover, by design (§8.2:
 *    "the wire still steps by the datum delta … and the existing disc channels
 *    still describe it").  Releasing it would flag the entire constellation.  The
 *    edge stays classified as FRAME there, which is the other answer §8.4 INC-2
 *    permits.
 *  - The **two-state clock oscillator** clear (`clk_model_warm`).  `clock_model`
 *    is not armed on any validated lineage (the card is a documented NO-GO), so
 *    transforming the oscillator state would be untested code on an unused path.
 *  - The **SH-VTEC** carry, which has no constellation and whose skip is
 *    deliberately global (see the RTA-2 loader warning).
 *
 * ## Cost
 *
 * Rank-1 over the carried key set, once per handover edge, not per epoch.  §8.6
 * budgets < 0.1 ms per edge and treats anything above 1 ms per edge as a wrong
 * implementation.
 */

#include <map>
#include <set>
#include <string>
#include <vector>

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkSbasis.hpp"
#include "logging/NetworkRunTraces.hpp"

namespace GnssProcessor {

/// Outcome of @ref applyClockStransform: the values the seam counts and writes as events.
struct ClockStransformResult {
    bool      applied{false};       ///< false ⇒ nothing was changed (q not in v_c or zero coefficient, no carried level for q, or no rows)
    double    c0{0.0};              ///< the coefficient applied (m)
    double    var_q{0.0};           ///< the incoming pivot's carried variance used for the variance increase
    long long rows{0};              ///< carried rows re-expressed
    double    x_q_post{0.0};        ///< the incoming pivot's carried level after the transform (expected 0)
    bool      selfcheck_ok{true};   ///< |x_q_post| and the re-derived c0 both within kStsExactTolM (§8.4 INC-3)
};

/// Exactness tolerance (m) for the self-check (§8.4 INC-3).  The transform is one
/// subtraction per row, so the residual is rounding only; a value above this is a
/// wiring fault.
inline constexpr double kStsExactTolM = 1e-9;

/**
 * @brief The pure rank-1 S-transform kernel (§8.1); no context, no I/O.
 *
 * Applies @f$x_i \leftarrow x_i - v_c[i] c_0@f$ and @f$\mathrm{var}_i \leftarrow
 * \mathrm{var}_i + v_c[i]^2 \mathrm{var}_q@f$ to every @c SharedParamKind::Clock
 * entry of @p carry whose satellite appears in @p v_c, with
 * @f$c_0 = x_q/v_c[q]@f$.  Entries of other kinds are not touched: their
 * component along the clock null direction is zero (see
 * @ref NetworkSbasis::clockNullDirection).  Membership is read from @p v_c, not
 * inferred from the satellite id, so a stale carry row for a satellite outside
 * this epoch's registered set is left alone.
 *
 * @param carry Carried shared priors, mutated in place.
 * @param v_c   The structural direction's nonzero (satellite, coefficient) entries.
 * @param q     The incoming pivot satellite.
 * @return What was done; @c applied false leaves @p carry untouched.
 */
ClockStransformResult applyClockStransform(
    std::vector<CarryPrior>& carry,
    const std::vector<std::pair<std::string, double>>& v_c,
    const std::string& q);

/**
 * @brief Re-express the carried shared state of each changed constellation in the
 *        incoming clock datum, and mark those constellations for fold release.
 *
 * Call once per epoch from the driver seam that derives @p datum_changed, before
 * any fold reads @c ctx.carry.prev_carry (that is, before
 * @ref networkInjectD2CarryPrior).  The call always clears
 * @c NetworkCarryState::sts_consts / @c sts_c0 first, so a release set cannot
 * carry over into a later epoch.
 *
 * On a handover epoch the call counts the edge per constellation in
 * @c pivot_handovers whether or not the knob is on.  Beyond that it is a no-op,
 * and the solve is byte-identical, when the knob is off, when no pivot changed, or
 * when the incoming pivot has no carried clock level to re-reference from (a first
 * sighting or a vanish-return past the carry horizon).  The last case is counted
 * as @c sts_noop_no_carry and written as an event.  A constellation without an
 * incoming pivot in @p plan, or with an empty @f$v_c@f$, is skipped without a
 * count.
 *
 * @param ctx                   Run context (carry + counters + knobs).
 * @param config_               Config (uniform seam signature; unused).
 * @param log                   Logger for the self-check warning.
 * @param traces                Event sink (STRANSFORM rows).
 * @param t                     This epoch's time (event stamping).
 * @param plan                  This epoch's datum plan; supplies the incoming
 *                              pivot per constellation.
 * @param datum_changed         Whether any pivot re-referenced this epoch.
 * @param changed_consts        The constellations whose pivot changed.
 * @param registered_clock_sats This epoch's registered satellite-clock set (the
 *                              same vector @ref NetworkSbasis::buildSharedDatum
 *                              receives), used to build @f$v_c@f$.
 */
void networkHandoverStransform(NetworkRunContext& ctx,
                               const GnssConfig& config_,
                               const std::shared_ptr<spdlog::logger>& log,
                               NetworkRunTraces& traces,
                               const GnssTime& t,
                               const NetworkSbasis::Plan& plan,
                               bool datum_changed,
                               const std::set<char>& changed_consts,
                               const std::vector<std::string>& registered_clock_sats);

}  // namespace GnssProcessor
