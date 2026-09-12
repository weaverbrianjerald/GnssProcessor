#pragma once
/**
 * @file NetworkHandoverStransform.hpp
 * @brief Queue 1(b) — the clock-pivot handover S-TRANSFORM
 *        (docs/SPEC_pivot_visibility_gauge_defect.md §8; the card's measured
 *        motivation is docs/SPEC_event_harness.md §19).
 *
 * ## What this fixes
 *
 * The per-constellation clock gauge is realized by a hard Dirichlet pin on ONE
 * satellite's clock column (@ref NetworkSbasis::buildSharedDatum, `Pin{…, 0.0}`).
 * When that satellite sets, the pin moves to a successor and the whole
 * constellation's estimable clock solution re-references.  The producer's answer
 * so far is to SKIP the D2 shared-carry fold for the affected keys for one epoch
 * — "one epoch of free fall" (§8.2): stiff states (clock levels, code bias at
 * q ≈ 0, phase bias) solve data-only exactly once, re-level, and re-stash in the
 * new datum.  **That one-epoch re-level IS the product discontinuity.**
 *
 * Measured on the 60-station storm bed (`runs/golden_sim_campaign_zwdblk_scoped`,
 * §19.5), at the GPS handover tow 319260 with increment 0 already armed: the
 * changed constellation's own code-bias step rms goes 0.00003 → 0.00921 m and its
 * phase-bias step rms 0.00083 → 0.19914 m, decaying back over ~30 minutes, while
 * the consumer is told to reset 30 phase-bias and 30 code-bias keys.  None of
 * that is a datum move — the code-bias and phase-bias gauges are realized
 * independently of the clock pivot (§8.1 and @ref NetworkSbasis::clockNullDirection)
 * and their step MEANS are 1e-6 m — it is a carry that was discarded rather than
 * re-expressed.
 *
 * ## The transform (§8.1)
 *
 * Any minimally-constrained solution maps to any other EXACTLY by the LASER
 * S-transform; a pin swap p→q inside one constellation reduces it to a rank-1
 * update along that constellation's structural clock null direction @f$v_c@f$:
 *
 * @f[
 *   c_0 = \frac{x_q^{\text{pre}}}{v_c[q]}, \qquad
 *   x_i \leftarrow x_i - v_c[i]\,c_0, \qquad
 *   \mathrm{var}_i \leftarrow \mathrm{var}_i + v_c[i]^2\,\mathrm{var}_q
 * @f]
 *
 * with q the INCOMING pivot and @f$x_q^{\text{pre}}@f$ its carried level in the
 * outgoing frame.  Afterwards the incoming pivot's carried level is exactly 0 —
 * its pinned value — and every between-satellite difference inside the
 * constellation is unchanged to machine precision, which is the whole content of
 * "S-transform": the carried information was VALID, merely expressed in the datum
 * that just retired.
 *
 * Two rules this implementation obeys literally:
 *
 *  1. **@f$v_c@f$ is the STRUCTURAL direction @ref NetworkSbasis owns** — never
 *     hand-written here, never an eigenvector (same-scale eigenvectors mix;
 *     structural membership is exact and carries per-satellite identity).  Only
 *     satellite-clock columns of the constellation have a nonzero component; the
 *     code-bias and phase-bias carries are NOT transformed because their
 *     component is zero.  They are nevertheless RELEASED (below), which is where
 *     most of the measured benefit lives.
 *  2. **Dropping the cross term is CONSERVATIVE.**  The exact per-column variance
 *     of @f$x_i - v_i c_0@f$ is @f$\mathrm{var}_i + v_i^2\mathrm{var}_q -
 *     2 v_i \mathrm{cov}(i,q)@f$; clocks are positively correlated in the
 *     dominant common-mode case, so omitting the cross term OVER-states variance.
 *     The full @f$S D S^\top@f$ on the carry blob is deferred by §8.8 and
 *     §8.7 gate 6 watches the failure signature it would produce.
 *
 * ## The load-bearing half is the RELEASE (§8.4 INC-1)
 *
 * The transform alone would be inert: a re-expressed prior that is still skipped
 * folds nothing.  The seam therefore records the transformed constellations in
 * @c NetworkCarryState::sts_consts, and the RTA-2 `handover_affects` skips in
 * @ref networkInjectD2CarryPrior (diagonal and dense branches, plus the READMIT
 * parked-entry drop) and in `networkStashStatus` (the VCE monitor and the
 * prior-residual census `folded` bit) release for exactly those constellations,
 * counting @c sts_rows_retained.  **Invariant #4: transforms applied with
 * `sts_rows_retained == 0` means the release is mis-wired no matter how clean the
 * log looks.**
 *
 * ## What is deliberately NOT released
 *
 *  - The **clock-monitor jump test** (`NetworkClockQc`, the `handover_affects`
 *    suppression there).  Under the PIN datum its statistic is the RAW clock, and
 *    the emitted clock genuinely steps by the datum delta at a handover — by
 *    design, §8.2: "the wire still steps by the datum delta … and the existing
 *    disc channels still describe it".  Releasing it would flag the entire
 *    constellation.  The edge stays classified as FRAME there, which is §8.4
 *    INC-2's other permitted answer.
 *  - The **two-state clock oscillator** clear (`clk_model_warm`).  `clock_model`
 *    is not armed on any validated lineage (the card is a documented NO-GO), so
 *    transforming the oscillator state would be untested code on a dead path.
 *  - The **SH-VTEC** carry, which is constellation-less and whose skip is
 *    deliberately global (the RTA-2 loader banner says so).
 *
 * ## Cost
 *
 * Rank-1 over the carried key set, once per handover EDGE — not per epoch.  §8.6
 * budgets < 0.1 ms/edge and calls anything above 1 ms/edge a wrong implementation.
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

/// Outcome of @ref applyClockStransform — the numbers the seam counts and events.
struct ClockStransformResult {
    bool      applied{false};       ///< false ⇒ nothing was changed (no carried level for q, or no rows)
    double    c0{0.0};              ///< the coefficient applied (m)
    double    var_q{0.0};           ///< the incoming pivot's carried variance used for the bump
    long long rows{0};              ///< carried rows re-expressed
    double    x_q_post{0.0};        ///< the incoming pivot's carried level AFTER (must be 0)
    bool      selfcheck_ok{true};   ///< §8.4 INC-3: |x_q_post| and the re-derived c0 within tolerance
};

/// §8.4 INC-3 exactness tolerance (m): the transform is one subtraction per row,
/// so the residual is pure rounding — anything above this is a wiring fault.
inline constexpr double kStsExactTolM = 1e-9;

/**
 * @brief The PURE rank-1 S-transform kernel (§8.1) — no context, no I/O.
 *
 * Applies @f$x_i \leftarrow x_i - v_c[i] c_0@f$ and @f$\mathrm{var}_i \leftarrow
 * \mathrm{var}_i + v_c[i]^2 \mathrm{var}_q@f$ to every @c SharedParamKind::Clock
 * entry of @p carry whose satellite appears in @p v_c, with
 * @f$c_0 = x_q/v_c[q]@f$.  Entries of other kinds are NOT touched: their
 * component along the clock null direction is zero (see
 * @ref NetworkSbasis::clockNullDirection).  Membership is read from @p v_c, never
 * inferred from the satellite id, so a stale carry row for a satellite that is not
 * in this epoch's registered set is left alone.
 *
 * @param carry Carried shared priors, mutated in place.
 * @param v_c   The structural direction's nonzero (satellite, coefficient) entries.
 * @param q     The INCOMING pivot satellite.
 * @return What was done; @c applied false leaves @p carry untouched.
 */
ClockStransformResult applyClockStransform(
    std::vector<CarryPrior>& carry,
    const std::vector<std::pair<std::string, double>>& v_c,
    const std::string& q);

/**
 * @brief Re-express the changed constellation(s)' carried shared state in the
 *        INCOMING clock datum, and mark them for fold release.
 *
 * Call once per epoch from the driver seam that derives @p datum_changed, BEFORE
 * any fold reads @c ctx.carry.prev_carry (i.e. before
 * @ref networkInjectD2CarryPrior).  Always clears
 * @c NetworkCarryState::sts_consts / @c sts_c0 first, so a stale release set can
 * never leak into a later epoch.
 *
 * No-op — and byte-identical — when the knob is off, when no pivot changed, or
 * when the incoming pivot has no carried clock level to re-reference from (a
 * first sighting or a vanish-return past the carry horizon); the last is counted
 * as @c sts_noop_no_carry and evented, never silently skipped.
 *
 * @param ctx                   Run context (carry + counters + knobs).
 * @param config_               Config (seam-uniform signature; unused today).
 * @param log                   Logger for the self-check warning.
 * @param traces                Event sink (STRANSFORM rows).
 * @param t                     This epoch's time (event stamping).
 * @param plan                  This epoch's datum plan — supplies the INCOMING
 *                              pivot per constellation.
 * @param datum_changed         Whether any pivot re-referenced this epoch.
 * @param changed_consts        The constellations whose pivot changed.
 * @param registered_clock_sats This epoch's registered satellite-clock set (the
 *                              same vector @ref NetworkSbasis::buildSharedDatum
 *                              was given), used to build @f$v_c@f$.
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
