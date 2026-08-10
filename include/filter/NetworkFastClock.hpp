#pragma once
/**
 * @file NetworkFastClock.hpp
 * @brief FC-1: the split-cadence clocks-only FAST PASS of the PPP-RTK network producer
 *        (docs/SPEC_fast_clock_split_cadence.md).
 *
 * The full pipeline (assembly, solve, carry, QC, emit) keeps running at its own cadence.
 * Epochs decimated out by @c network.epoch_decimation_n — a bare `continue` at the T2e
 * hook today — instead run a CLOCKS-ONLY tick that measures each satellite's clock change
 * since the last EMITTED full solve.  Everything else stays frozen and rides ZOH, which
 * is exactly what a consumer already does inside one full-solve era; the fast tick merely
 * refreshes the one state whose ZOH error dominates.
 *
 * Why this and not a faster full pipeline: the measured 305-station all-on baseline is
 * 27.7 s/epoch of which the border SOLVE is 41 ms, so a 1 s product cadence is
 * unreachable by optimizing the per-epoch pipeline.  What is expensive is discovering the
 * STRUCTURE (ambiguities, iono, biases, datum, QC); once known, re-measuring clocks is
 * microseconds.  The C1 card supplies the mandate from the other side: the emitted c0 is
 * a random walk (0.157 m per 30 s), so ZOH is already the MMSE predictor GIVEN a 30 s
 * solve — the only way to serve better fast clocks is to MEASURE them at the served rate.
 * (C1's own clock-RATE product was measured NET-HARMFUL and stays dead.)
 *
 * @section fc1_obs The observable
 * At the anchor t0 (the last EMITTED full solve) each used line-of-sight caches its
 * model-corrected phase @f$\tilde\Phi_j = \Phi_j^{obs} + c\,\delta t^s_{ap}@f$ — observed
 * phase plus the SAME a-priori satellite clock the full model applies (product/broadcast
 * polynomial at emission + periodic relativistic term) — split into geometry plus "lump":
 * @f[ \tilde\Phi_j(t_0) = \rho(t_0) + M_j(t_0) @f]
 * where @f$M_j@f$ absorbs the receiver clock, tropo, iono, ambiguity, phase biases,
 * wind-up, PCO/PCV, tides, Shapiro and the RESIDUAL (beyond-a-priori) satellite clock —
 * the quantity the producer actually emits — and @f$\dot M_j@f$ is finite-differenced
 * against the previous emitted full epoch.  Building the lump on a-priori-corrected
 * rather than raw phase is what keeps the recovered clock rate in the emitted frame: a
 * raw-phase lump rate carries the full physical rate (broadcast af1 drift + relativistic
 * rate), which the consumer's own a-priori evaluation already advances, and emitting it
 * inside K double-counts both (see the .cpp @c fc1_apriori section for the measured
 * damage).  At a tick @f$t@f$ (@f$\Delta t = t - t_0@f$):
 * @f[ y_j(t) = \tilde\Phi_j(t) - [\,\rho(t) + M_j(t_0) + \dot M_j\,\Delta t\,] @f]
 * with @f$\rho(t)@f$ recomputed exactly (the ONLY nonlinear term per tick).  IF-combining
 * the arc's two cached bands cancels the ionospheric change, leaving
 * @f[ y_{IF}(r,s,t) = \delta\tau_r(t) - \delta c^s(t) + \varepsilon @f]
 * — the receiver clock change (a nuisance, re-estimated fresh per tick) and the residual
 * satellite clock change (the product).  Frozen ambiguities and biases cancel EXACTLY in
 * the delta on a live arc.
 *
 * Every tick differences against the SAME anchor, never tick-to-tick, so per-tick
 * measurement error does not random-walk across a fast window.
 *
 * @section fc1_frame Frame contract
 * The fast datum is the same @c network.clock_datum_station pin as the slow solve, and
 * there is NO re-election inside a fast window ever: a tick whose datum station is absent
 * or screened is WITHHELD whole.  The emitted fast clock is
 * @f$c_0^s(t) = c_0^s\_emitted(t_0) + \dot c^s\,\Delta t + \delta c^s(t)@f$ — a delta on
 * the EMITTED value, so it lives in the emitted frame by construction and any per-era
 * frame constant (re-base, datum, pivot, IF-pin realization) cancels, because re-bases
 * only ever happen inside the full pipeline at era boundaries.  Both delta terms are in
 * the emitted frame too: @f$\dot c^s@f$ is the RESIDUAL clock rate (the lump is built on
 * a-priori-corrected phase, @ref fc1_obs) and @f$\delta c^s@f$ the residual change beyond
 * that trend — emitting the full physical rate here would double-count the broadcast
 * drift + relativistic rate the consumer's own a-priori evaluation already advances.
 *
 * @section fc1_iso Isolation contract
 * A tick mutates NOTHING outside @ref FastClockCarry.  Full epochs must be byte-identical
 * whether or not ticks ran; that is what makes the card safe to land incrementally, and
 * it is gated at every increment.
 *
 * Seam contract per CLAUDE.md: free functions over the run context, owning no state —
 * every carry object is a reference into the caller's @ref NetworkRunContext.
 */

#include "app/NetworkSetupResult.hpp"
#include "common/PppObservation.hpp"
#include "common/PppUtils.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "model/ModelGeometry.hpp"
#include "products/NetworkProductEmitter.hpp"   // NetworkSolutionEpoch
#include "rinex/MultiRinexSync.hpp"
#include "rinex/RinexParser.hpp"                // RinexObservation

#include <Eigen/Core>

#include <cstddef>
#include <memory>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;   // app/NetworkRunContext.hpp — included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp — included in the .cpp only

/// FC-1: the outcome of advancing the anchor's emitted-epoch streak.
struct FastClockArmDecision {
    bool        chain_broken{false};      ///< The emitted chain was interrupted (or this is the first anchor).
    std::size_t consecutive_emitted{0};   ///< Streak length after this epoch (1 on a break).
    bool        armed{false};             ///< True once the streak reaches 2 — the lump RATE exists.
};

/**
 * @brief Advance the fast-clock arming streak across one EMITTED full epoch.  Pure.
 *
 * The anchor advances only on emitted epochs, so this function never sees a withheld one
 * and cannot reset the streak from it directly.  The break is inferred from the GAP: a
 * stretch longer than the R11 emission bound means the consumer went without a full
 * solve, which is exactly when a lump rate finite-differenced across it stops being a
 * defensible linearization.  Same knob as the emission gate, same meaning — EMISSION
 * continuity, never solve continuity.
 *
 * Arming needs two consecutive emitted epochs because the first supplies the lump
 * @f$M_j@f$ and the second its rate @f$\dot M_j@f$.  A cold start and a warm resume both
 * begin unarmed by construction (the carry is not snapshotted), so a resumed leg can
 * never ride an anchor referring to a frame it did not itself emit.
 *
 * @param prev_t0_s      previous anchor time (continuous GPS s); <= 0 means "no anchor yet".
 * @param prev_streak    previous consecutive-emitted count.
 * @param now_s          this emitted epoch's time (continuous GPS s).
 * @param emit_max_gap_s the R11 bound (@c network.fast_clock.emit_max_gap_s).
 */
inline FastClockArmDecision fastClockAdvanceArm(double prev_t0_s, std::size_t prev_streak,
                                                double now_s, double emit_max_gap_s) {
    FastClockArmDecision d;
    d.chain_broken = (prev_t0_s <= 0.0) || ((now_s - prev_t0_s) > emit_max_gap_s);
    d.consecutive_emitted = d.chain_broken ? std::size_t{1} : prev_streak + 1;
    d.armed = (d.consecutive_emitted >= 2);
    return d;
}

/**
 * @brief Harvest the fast-clock anchor from an epoch that ACTUALLY EMITTED.
 *
 * Rebuilds @c ctx.carry.fast_clock from scratch: per-LOS cached bands with their lump
 * @f$M_j(t_0)@f$ and (once two consecutive emitted epochs exist) lump rate
 * @f$\dot M_j@f$, plus the emitted @f$c_0^s@f$ per satellite and the anchor time.
 *
 * @warning ORDERING — MUST be called only when this epoch's solution reached the wire.
 *          "Emitted", not "solved", is load-bearing: this is the R11 lesson from the C1
 *          card, where a gate keyed on SOLVE continuity let a delta measured inside a
 *          withheld era be published across a consumer-visible gap.  The caller decides
 *          by observing that @c ctx.counters.n_emitted advanced across @ref
 *          networkEmitStore, which every whole-epoch withhold path `continue`s before.
 *
 * Read-only with respect to every pipeline object it is handed; the sole mutation is
 * @c ctx.carry.fast_clock (plus FC-1 counters).  No-op unless
 * @c network.fast_clock.enable.
 *
 * @param ctx   [in/out] carry (fast_clock) + counters.  Taken whole so a dropped `&`
 *              cannot silently drop the anchor update.
 * @param sol   the solution epoch AFTER @ref networkEmitStore has re-based / filtered it,
 *              i.e. the values the consumer actually received.
 */
void networkFastClockHarvest(
    NetworkRunContext& ctx,
    const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkSetupResult& setup,
    const SyncedEpoch& se,
    const std::vector<bool>& present,
    const std::vector<std::vector<PppObservation>>& modeled,
    const NetworkSolutionEpoch& sol,
    const std::unique_ptr<PppUtils>& fc_ppp_utils);

/**
 * @brief Run one clocks-only fast tick on a decimated-out epoch.
 *
 * Recomputes @f$\rho(t)@f$ for the cached LOS, forms the per-band delta-from-fit,
 * IF-combines, seeds with a two-way median polish, solves one elevation-weighted WLS with
 * the station clocks Schur-eliminated, applies the per-LOS @f$k\sigma@f$ screen, enforces
 * the estimability floors, and (INC-3) hands the surviving satellite deltas to emission.
 *
 * Not a loop epoch: it does not increment @c n_loop_epochs, does not enter assembly /
 * solve / carry / QC, and mutates nothing outside @c ctx.carry.fast_clock.  No-op unless
 * @c network.fast_clock.enable and the anchor is armed (two consecutive emitted full
 * epochs).
 *
 * @param shadow_pass true when invoked on a FULL epoch for grading only
 *                    (@c network.fast_clock.shadow) — the result is traced and discarded,
 *                    never emitted, so the full pipeline stays byte-identical.
 */
void networkFastClockTick(
    NetworkRunContext& ctx,
    const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkRunTraces& traces,
    NetworkSetupResult& setup,
    const SyncedEpoch& se,
    bool shadow_pass);

/**
 * @brief Geometric range, elevation and a-priori satellite clock for one line-of-sight,
 *        the FC-1 way.  Pure-ish (reads only the orbit/clock products through @p geom).
 *
 * A pseudorange-seeded single-pass transmit time plus the Sagnac rotation into the
 * reception-time ECEF frame — the same formulation the full model uses, minus the
 * refinement pass (whose effect on the emission epoch is nanoseconds, i.e. sub-micrometre
 * satellite displacement).  Also surfaces the a-priori satellite clock it evaluates for
 * the light time (plus the periodic relativistic term, the full model's dtr convention),
 * projected to metres — the phase correction that keeps the lump in the emitted frame
 * (@ref fc1_obs).
 *
 * @warning This function, not @c PppObservation::range, must supply BOTH ends of a fast
 *          delta.  @f$\rho(t)-\rho(t_0)@f$ is then a difference of ONE function and the
 *          convention cancels exactly; mixing the full model's refined range at @f$t_0@f$
 *          with a seed-only range at @f$t@f$ would leave the convention offset in the
 *          measurement instead.  The same rule binds @p apriori_clk_m: both ends of a
 *          delta must correct the phase through THIS function's convention.
 *
 * @param geom      any station's Geometry (they share one orbit product; only the
 *                  per-instance node index differs).
 * @param rx_ecef   station ECEF (held fixed for the whole run).
 * @param ro        the raw RINEX record — its code pseudorange seeds the light time.
 * @param include_relativistic mirror of @c model_corrections.enable_relativistic_correction,
 *                  so the correction matches what the full model actually applies.
 * @param[out] rho_m          geometric range (m).
 * @param[out] elevation_deg  topocentric elevation (deg).
 * @param[out] apriori_clk_m  @f$c\,\delta t^s_{ap}@f$ (m): polynomial/product clock at
 *                  emission plus (when enabled) the periodic relativistic term.
 * @return false when no usable code pseudorange or no orbit sample exists.
 */
bool fastClockGeometricRange(const Model::Geometry& geom,
                             const Eigen::Vector3d& rx_ecef,
                             const RinexObservation& ro,
                             bool include_relativistic,
                             double& rho_m,
                             double& elevation_deg,
                             double& apriori_clk_m);

} // namespace GnssProcessor
