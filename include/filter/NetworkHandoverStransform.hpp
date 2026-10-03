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
 *     Card SCL build B2 (@c network.handover_stransform_dense, default off) is that
 *     full transform: @ref applyClockStransformDense re-expresses the values and the
 *     dense block together by @f$S P S^\top@f$, with the diagonal kernel as its fallback.
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

/// Self-check tolerance of the dense kernel's contrast invariance (card SCL B2), relative to
/// the largest clock variance of the constellation's block.  The update is three
/// subtractions per entry, so the change of a contrast variance is a few roundings of that
/// scale; a value above this is a wiring fault.
inline constexpr double kStsDenseContrastTolRel = 1e-10;

/**
 * @brief The rank-1 S-transform kernel (§8.1) through the traversal of carried references
 *        (card PRM phase 2, @ref forEachCarriedReference); no context, no I/O.
 *
 * Applies @f$x_i \leftarrow x_i - v_c[i] c_0@f$ and @f$\mathrm{var}_i \leftarrow
 * \mathrm{var}_i + v_c[i]^2 \mathrm{var}_q@f$, with @f$c_0 = x_q/v_c[q]@f$ and @c var_q read
 * from the incoming pivot's carried clock entry before anything moves, to the satellite-clock
 * references of the sites of @p mech.  Entries of other kinds are not touched: their
 * component along the clock null direction is zero (see
 * @ref NetworkSbasis::clockNullDirection).
 *
 * With @c CarryMechanism::Handover (the transform after PASS B) the sites are the carried
 * clock entries (@c prev_carry), and membership is read from @p v_c, not inferred from the
 * satellite id, so a stale carry row for a satellite outside this epoch's registered set is
 * left alone.  With @c CarryMechanism::HandoverBeforeRows (the transform before the coupling
 * rows are built) the sites are every clock reference of the incoming pivot's constellation
 * the rows read: the carried clocks, an unregistered or parked satellite of the constellation
 * included at the structural direction's common coefficient 1; the s_prev of every coupling
 * term on them, and the term's p_prev by @f$v_i^2\mathrm{var}_q@f$ when the term is free
 * (@c p_prev > 0; a pinned term keeps its pinned route); and the stash's pinned pivot value.
 *
 * @param carry  The run's carried state, mutated in place.
 * @param v_c    The structural direction's nonzero (satellite, coefficient) entries.
 * @param q      The incoming pivot satellite.
 * @param mech   @c Handover or @c HandoverBeforeRows.
 * @param counts Optional out: per-site tallies of the walk (accumulated).
 * @return What was done; @c applied false leaves @p carry untouched.  @c rows counts the
 *         carried clock entries re-expressed.
 */
ClockStransformResult applyClockStransform(
    NetworkCarryState& carry,
    const std::vector<std::pair<std::string, double>>& v_c,
    const std::string& q, CarryMechanism mech = CarryMechanism::Handover,
    CarrySiteCounts* counts = nullptr);

/// Outcome of @ref applyClockStransformDense.
struct ClockStransformDenseResult {
    bool        applied{false};       ///< false: nothing was changed (see @c reason); the caller falls back to the diagonal kernel
    const char* reason{""};           ///< why it was not applied: "empty_block", "no_q_entry", "no_q_row", "q_is_p"
    double      c0{0.0};              ///< the incoming pivot's carried level before (m)
    double      pqq{0.0};             ///< the incoming pivot's block variance before, P_qq (m^2)
    std::size_t k_old{0};             ///< block size before the outgoing pivot was appended
    int         iq{-1};               ///< the incoming pivot's block row
    long long   rows{0};              ///< carried clocks with a block row re-expressed (q included)
    long long   parked{0};            ///< carried clocks of the constellation without a block row (the diagonal rule)
    bool        p_added{false};       ///< the outgoing pivot was appended (value -c0, variance P_qq)
    double      x_q_post{0.0};        ///< the incoming pivot's carried level after (expected 0)
    double      var_saved{0.0};       ///< summed (var_i + P_qq) - P'_ii over the re-expressed clocks other than q (m^2): what the diagonal bump over-states
    double      contrast_maxdev{0.0}; ///< largest |change| of a clock-contrast variance P_ii + P_jj - 2 P_ij over the constellation's rows (m^2)
    bool        selfcheck_ok{true};   ///< |x'_q|, the zero row, finite, symmetric, contrast invariance
    CarrySiteCounts counts{};         ///< the traversal's tallies of the value moves (card PRM phase 2)
};

/**
 * @brief The exact clock-pivot S-transform of the carried values and the dense block
 *        together (card SCL build B2, @c network.handover_stransform_dense); no context, no
 *        I/O.
 *
 * The diagonal kernel (@ref applyClockStransform) moves the carried clock values and adds
 * @f$\mathrm{var}_q@f$ to each carried clock variance, and leaves the dense side-car
 * @c prev_carry_cov in the outgoing frame.  This kernel applies @f$S = I - u e_q^\top@f$ to
 * both, with @f$u_i = 1@f$ on every clock row of constellation @p c in the block (registered
 * or not, because a block held in two frames is not a covariance) and 0 on every other row
 * (phase biases, code biases, other constellations):
 *
 * @f[
 *   r = P e_q,\quad c_0 = x_q,\quad
 *   x' = x - u\,c_0,\quad
 *   P' = P - u r^\top - r u^\top + P_{qq}\,u u^\top .
 * @f]
 *
 * The update is formed on the upper triangle and mirrored, so @f$P'@f$ is exactly symmetric;
 * rows with @f$u = 0@f$ on both sides are not touched (bitwise unchanged); the incoming
 * pivot's row and column are set to exactly 0.  The outgoing pivot @p p_out, pinned at the
 * stash and so without a row, is exact in the outgoing frame (value 0, zero row): when it has
 * neither a block row nor a carried entry it is appended with value @f$-c_0@f$, variance
 * @f$P_{qq}@f$ and covariance @f$P'_{pj} = -r_j + P_{qq} u_j@f$, as a fresh @c CarryPrior
 * (its fold decision is made at the fold, which counts it as undecided).  A carried clock of
 * the constellation with a block row takes @f$\mathrm{var} = P'_{ii}@f$; one without (a
 * parked READMIT entry) takes the diagonal rule @f$x - c_0@f$, @f$\mathrm{var} + P_{qq}@f$.
 * The value moves go through the traversal of carried references
 * (@ref forEachCarriedReference, the @c Handover sites), so card PRM's tallies count them.
 *
 * Every between-satellite contrast @f$a@f$ with @f$a^\top u = 0@f$ keeps its variance
 * exactly (up to rounding): the diagonal kernel over-states a clock contrast by
 * @f$2\,\mathrm{var}_q@f$ and leaves the clock-bias cross-covariances stale by
 * @f$-P_{bq}@f$.  The kernel is idempotent: a second call finds @f$c_0 = 0@f$,
 * @f$r = 0@f$ and the outgoing pivot present, and changes nothing.  It is singular (the
 * incoming pivot's row is lost), so it cannot be undone without the saved block: the loader
 * refuses it with @c network.handover_stransform_before_rows, the only path with an undo.
 *
 * Not applied (nothing changed, @c reason set): an empty block, no carried clock entry for
 * @p q, no block row for @p q, or @p q equal to @p p_out.
 *
 * @param carry The run's carried state: @c prev_carry and @c prev_carry_cov, mutated in place.
 * @param c     The constellation's leading character.
 * @param q     The incoming pivot.
 * @param p_out The outgoing pivot (the frame the block is in); empty: nothing is appended.
 * @return What was done.
 */
ClockStransformDenseResult applyClockStransformDense(NetworkCarryState& carry, char c,
                                                     const std::string& q,
                                                     const std::string& p_out);

/**
 * @brief Undo a transform @ref applyClockStransform applied with
 *        @c CarryMechanism::HandoverBeforeRows (card PRM phase 2): every clock reference of the
 *        constellation moves back by @p c0 and every re-expressed variance (a carried clock's,
 *        a free coupling term's p_prev) gives back @p var_q.  The structural direction's
 *        coefficient is 1 for every clock of the constellation, so the membership is the
 *        constellation's, as the forward transform's before the rows.
 * @param carry         The run's carried state.
 * @param constellation The constellation's leading character.
 * @param c0            The coefficient the transform applied (m).
 * @param var_q         The variance it added (m^2).
 * @param counts        Optional out: per-site tallies (accumulated).
 * @return the number of references moved back.
 */
long long undoClockStransform(NetworkCarryState& carry, char constellation, double c0,
                              double var_q, CarrySiteCounts* counts = nullptr);

/**
 * @brief The handover's S-transform before the coupling rows (card PRM phase 2,
 *        @c network.handover_stransform_before_rows).
 *
 * Call once per epoch before the fold decision (@ref networkCarryDecide) and PASS B.  The
 * call always clears @c NetworkCarryState::sts_pre first.  With the knob armed it previews
 * this epoch's clock datum election and handover test (both pure: the election reads only
 * the registered clock columns, the declaration, the sticky baseline and the health
 * filter, which PASS B does not change; @ref networkClockDatumDeclaration is the one
 * declaration) and, for each constellation whose pivot changed, re-expresses through
 * @ref applyClockStransform with @c CarryMechanism::HandoverBeforeRows every clock reference
 * of the constellation the rows read.  The fold decision, the exact update's prepare and the
 * rows then read the re-expressed carry.  The solve glue's @ref networkHandoverStransform
 * does the bookkeeping (counters, events, the fold release) from the record and does not
 * transform again.
 *
 * The transform is recorded (@c sts_carry_pivot) until the next stash re-expresses the carry
 * in the solve's datum.  A skipped epoch leaves the pivot unadopted and the record set: when
 * the next election chooses the same pivot, the handover is detected again and released
 * without a second transform; when it chooses another (the outgoing pivot again, or a third
 * one), the transform is undone first (@ref undoClockStransform), so the carry is back in the
 * outgoing datum before anything else moves it.  A failing election (an exception) is left to
 * the solve glue, which skips the epoch.
 *
 * @param ctx                   Run context (carry, counters, knobs).
 * @param config_               Config (the declaration).
 * @param log                   Logger (unused; the seam signature).
 * @param traces                Event sink (STRANSFORM sts_undone rows).
 * @param sat_health            The broadcast health the election passes over (card HLT).
 * @param est                   This epoch's estimator, columns registered.
 * @param t                     This epoch's time.
 * @param registered_clock_sats This epoch's registered satellite clocks.
 * @param clk_anchor_sigma      The resolved broadcast anchor sigma (m; the declaration).
 */
void networkHandoverStransformBeforeRows(NetworkRunContext& ctx,
                                         const GnssConfig& config_,
                                         const std::shared_ptr<spdlog::logger>& log,
                                         NetworkRunTraces& traces,
                                         const NetworkSbasis::SatHealth& sat_health,
                                         const NetworkEstimator& est,
                                         const GnssTime& t,
                                         const std::vector<std::string>& registered_clock_sats,
                                         double clk_anchor_sigma);

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
