#pragma once
/**
 * @file NetworkFastClock.hpp
 * @brief Split-cadence clocks-only fast pass of the PPP-RTK network producer (FC-1,
 *        docs/SPEC_fast_clock_split_cadence.md).
 *
 * The full pipeline (assembly, solve, carry, QC, emit) keeps running at its own cadence.
 * Epochs skipped by @c network.epoch_decimation_n (a bare `continue` at the T2e hook when
 * the fast tier is off) run a clocks-only tick instead, which measures each satellite's
 * clock change since the last emitted full solve.  Every other state stays frozen and is
 * held by zero-order hold (ZOH), which is what a consumer already does inside one
 * full-solve era; the fast tick refreshes the one state whose ZOH error dominates.
 *
 * Rationale for a separate tier instead of a faster full pipeline: at 305 stations with
 * the full armed set, an epoch costs 27.7 s, of which the border solve is 41 ms, so a 1 s
 * product cadence is out of reach by optimizing the per-epoch pipeline.  The expensive
 * part is resolving the structure (ambiguities, iono, biases, datum, QC); once that is
 * known, re-measuring clocks costs microseconds.  The emitted c0 is a random walk
 * (0.157 m per 30 s), so ZOH is already the MMSE predictor given a 30 s solve, and better
 * fast clocks require measuring them at the served rate (C1 card).  The C1 clock-rate
 * product itself is NET-HARMFUL and stays off.
 *
 * @section fc1_obs The observable
 * At the anchor t0 (the last emitted full solve) each used line of sight caches its
 * model-corrected phase @f$\tilde\Phi_j = \Phi_j^{obs} + c\,\delta t^s_{ap}@f$: observed
 * phase plus the same a-priori satellite clock the full model applies (product/broadcast
 * polynomial at emission plus the periodic relativistic term), split into geometry plus
 * a "lump":
 * @f[ \tilde\Phi_j(t_0) = \rho(t_0) + M_j(t_0) @f]
 * where @f$M_j@f$ absorbs the receiver clock, tropo, iono, ambiguity, phase biases,
 * wind-up, PCO/PCV, tides, Shapiro and the residual (beyond-a-priori) satellite clock,
 * which is the quantity the producer emits.  @f$\dot M_j@f$ is finite-differenced against
 * the previous emitted full epoch.  Building the lump on a-priori-corrected phase keeps
 * the recovered clock rate in the emitted frame.  A lump on raw phase carries the full
 * physical rate (broadcast af1 drift plus relativistic rate), which the consumer's own
 * a-priori evaluation already advances, so emitting it inside K would count both twice
 * (the .cpp section @c fc1_apriori gives the measured size).  At a tick @f$t@f$
 * (@f$\Delta t = t - t_0@f$):
 * @f[ y_j(t) = \tilde\Phi_j(t) - [\,\rho(t) + M_j(t_0) + \dot M_j\,\Delta t\,] @f]
 * with @f$\rho(t)@f$ recomputed exactly (the only nonlinear term per tick).  The
 * ionosphere-free combination of the arc's two cached bands cancels the ionospheric
 * change, leaving
 * @f[ y_{IF}(r,s,t) = \delta\tau_r(t) - \delta c^s(t) + \varepsilon @f]
 * where @f$\delta\tau_r@f$ is the receiver clock change (a nuisance, re-estimated per
 * tick) and @f$\delta c^s@f$ the residual satellite clock change (the product).  Frozen
 * ambiguities and biases cancel exactly in the delta on a live arc.
 *
 * Every tick differences against the anchor, never against the previous tick, so
 * per-tick measurement error does not random-walk across a fast window.
 *
 * @section fc1_frame Frame contract
 * The fast datum is the same @c network.clock_datum_station pin as the slow solve, and it
 * is never re-elected inside a fast window: a tick whose datum station is absent is
 * withheld whole.  The emitted fast clock is
 * @f$c_0^s(t) = c_0^s\_emitted(t_0) + \dot c^s\,\Delta t + \delta c^s(t)@f$, a delta on
 * the emitted value, so it lies in the emitted frame by construction.  Any per-era frame
 * constant (re-base, datum, pivot, IF-pin realization) cancels, because re-bases happen
 * only inside the full pipeline at era boundaries.  Both delta terms are in the emitted
 * frame as well: @f$\dot c^s@f$ is the residual clock rate (the lump is built on
 * a-priori-corrected phase, @ref fc1_obs) and @f$\delta c^s@f$ is the residual change
 * beyond that trend.  Emitting the full physical rate here would count twice the
 * broadcast drift and relativistic rate that the consumer's own a-priori evaluation
 * already advances.
 *
 * @section fc1_iso Isolation contract
 * A tick writes @ref FastClockCarry, its FC-1 counters and the fast-clock trace, and on
 * an emitting tick the fast-clock product rows and the producer's own SSR store
 * (@c pushFastClock).  It changes no state the full pipeline reads, so full epochs are
 * byte-identical whether or not ticks ran.  This allows the card to land incrementally,
 * and each increment is gated on it.
 *
 * Seam contract per CLAUDE.md: free functions over the run context that own no state;
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
struct NetworkRunContext;   // app/NetworkRunContext.hpp, included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp, included in the .cpp only

/// Outcome of advancing the anchor's emitted-epoch streak (FC-1).
struct FastClockArmDecision {
    bool        chain_broken{false};      ///< The emitted chain was interrupted (or this is the first anchor).
    std::size_t consecutive_emitted{0};   ///< Streak length after this epoch (1 on a break).
    bool        armed{false};             ///< True once the streak reaches 2, when the lump rate exists.
};

/**
 * @brief Advance the fast-clock arming streak across one emitted full epoch.  Pure.
 *
 * The anchor advances only on emitted epochs, so this function never sees a withheld
 * epoch and cannot reset the streak from one directly.  The break is inferred from the
 * gap instead: an interval longer than the R11 emission bound means the consumer went
 * without a full solve, and a lump rate finite-differenced across that interval is no
 * longer a defensible linearization.  The knob is the one the emission gate uses, with
 * the same meaning: emission continuity, not solve continuity.
 *
 * Arming needs two consecutive emitted epochs because the first supplies the lump
 * @f$M_j@f$ and the second its rate @f$\dot M_j@f$.  A cold start and a warm resume both
 * begin unarmed (the carry is not snapshotted), so a resumed leg never uses an anchor
 * that refers to a frame it did not emit itself.
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
 * @brief Harvest the fast-clock anchor from an epoch whose solution was emitted.
 *
 * Rebuilds @c ctx.carry.fast_clock from scratch: per-LOS cached bands with their lump
 * @f$M_j(t_0)@f$ and (once two consecutive emitted epochs exist) lump rate
 * @f$\dot M_j@f$, the emitted @f$c_0^s@f$ per satellite, and the anchor time.
 *
 * @warning Call only when this epoch's solution reached the wire.  The condition is
 *          "emitted", not "solved" (R11, C1 card): a gate keyed on solve continuity lets a
 *          delta measured inside a withheld era be published across a gap the consumer
 *          sees.  The caller detects emission by observing that
 *          @c ctx.counters.n_emitted advanced across @ref networkEmitStore; every
 *          whole-epoch withhold path `continue`s before that call.
 *
 * Read-only with respect to every pipeline object passed in; the only writes are to
 * @c ctx.carry.fast_clock and the FC-1 counters.  No-op unless
 * @c network.fast_clock.enable.
 *
 * @param ctx   [in/out] carry (fast_clock) and counters.  Passed whole so that a dropped
 *              `&` cannot silently discard the anchor update.
 * @param sol   the solution epoch after @ref networkEmitStore has re-based and filtered
 *              it, i.e. the values the consumer received.
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
 * Recomputes @f$\rho(t)@f$ for the cached LOS, forms the per-band delta from the anchor
 * fit, combines the bands ionosphere-free, runs a two-way median polish whose
 * interactions feed the per-LOS @f$k\sigma@f$ screen, enforces the estimability floors,
 * solves one elevation-weighted WLS with the station clocks Schur-eliminated, and (INC-3)
 * emits the surviving satellite clocks.
 *
 * Not a loop epoch: it does not increment @c n_loop_epochs and does not enter assembly,
 * solve, carry or QC.  Its writes are listed in @ref fc1_iso.  No-op unless
 * @c network.fast_clock.enable is set and the anchor is armed (two consecutive emitted
 * full epochs).
 *
 * @param shadow_pass true when invoked on a full epoch for grading only
 *                    (@c network.fast_clock.shadow); the result is traced and discarded,
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
 * @brief Geometric range, elevation and a-priori satellite clock for one line of sight,
 *        as the fast tier computes them.  Reads only the orbit and clock products through
 *        @p geom.
 *
 * Uses a pseudorange-seeded single-pass transmit time plus the Sagnac rotation into the
 * reception-time ECEF frame, the formulation of the full model's
 * @c Geometry::computeEmissionState.  The difference is that the full model re-evaluates
 * the satellite clock at the corrected emission time; the emission epoch is the same,
 * and the clock value differs by the clock drift over about 1 ms (micrometres in range).
 * The function also returns the a-priori satellite clock it evaluates for the light time
 * (plus the periodic relativistic term, the full model's dtr convention), in metres: the
 * phase correction that keeps the lump in the emitted frame (@ref fc1_obs).
 *
 * @warning Both ends of a fast delta must come from this function, not from
 *          @c PppObservation::range.  @f$\rho(t)-\rho(t_0)@f$ is then a difference of one
 *          function and the convention cancels exactly; mixing the full model's range at
 *          @f$t_0@f$ with this range at @f$t@f$ would leave the convention offset in the
 *          measurement.  The same rule applies to @p apriori_clk_m: both ends of a delta
 *          must correct the phase through this function's convention.
 *
 * @param geom      any station's Geometry (all share one orbit product; only the
 *                  per-instance node index differs).
 * @param rx_ecef   station ECEF (held fixed for the whole run).
 * @param ro        the raw RINEX record; its code pseudorange seeds the light time.
 * @param include_relativistic mirror of @c model_corrections.enable_relativistic_correction,
 *                  so the correction matches what the full model applies.
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

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the FC-1 split-cadence fast clock.
 *
 * The FC-1 split-cadence fast clock: ticks driven and estimated, its rows and its wall
 * cost.
 *
 * Gate: returns 0 at once unless `config_.getNetworkConfig().fast_clock.enable`.
 *
 * @param v The run at its end (filter/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkFastClockRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

} // namespace GnssProcessor
