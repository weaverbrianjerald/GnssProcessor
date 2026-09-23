#pragma once

/**
 * @file NetworkClockLevel.hpp
 * @brief Card CLV (docs/refound/CLV_clock_level_2026-09-22.md): the satellite clock
 *        LEVEL, estimated from the code alone and re-based into the carry
 *        (@c network.clock_level_carry).
 *
 * The level direction of satellite s moves its clock column and every one of its
 * phase-bias columns by the same amount.  MeasurementModel.cpp writes the satellite
 * clock with a -1 partial on code AND phase rows (lines 1588-1594) and the satellite
 * phase bias with a +1 partial on phase rows only (lines 1546-1551), and
 * networkSplitStation_ copies those columns verbatim into the shared border
 * (ApplicationNetworkAssembly.cpp:113-144).  So a shift d along the direction leaves
 * every phase row invariant (-d + d) and moves every code row of s by -d: the level is
 * a code-only quantity.
 *
 * Why it freezes.  Card 1d's time update of a coupling row shrinks each border term by
 * kappa = p / (p + q dt) with the legacy clock rate q = filter.process_noise.sat_clock_q
 * (1e-3 m^2/s: kappa about 3e-4 for a clock carried at the Q3 anchor's variance floor),
 * while the phase-bias terms keep kappa about 1.  A carried ambiguity, whose exact
 * regression is on the phase side (-clock + phase bias), then states its phase bias
 * alone, and the phase rows turn that into a phase-precision statement about the level.
 * Every carried ambiguity does this, which is the 22-arm ladder's finding that no single
 * knob releases the level.
 *
 * What the card does.  The solve is not loosened: freeing the level inside the solve
 * makes it the cheapest outlet for any station's phase inconsistency (measured on the
 * field bed: an undetected L2W event at one station moved four satellite clocks by 4 to
 * 8 m in one epoch and tripped the Q3 anchor on each).  Instead, after every kept epoch:
 *  1. the post-fit ionosphere-free code residual of each (station, satellite) pair of
 *     full-weight code rows on the constellation's pin bands is formed
 *     (@ref clockLevelIfResidual, @ref clockLevelIfInformation);
 *  2. a two-way least-squares fit separates one level effect per satellite from one
 *     offset per (station, constellation) (what the receiver clocks absorbed), the
 *     satellite effects summing to zero per constellation (@ref clockLevelTwoWay);
 *  3. a scalar filter per satellite with the code-only information I_L turns the
 *     effect into a shift (@ref clockLevelFilterUpdate), I_L decaying with the horizon
 *     and capped at 1 / floor^2, restarted when the satellite becomes eligible (the Q3
 *     anchor takes its clock) and when its residual breaks the filter's prediction;
 *  4. the carry is moved along the direction by the shift (@ref clockLevelShiftCarry):
 *     the carried clock and every carried phase bias of the satellite, every coupling
 *     term that references them (so every carried row keeps its residual), the
 *     clock-jump baselines, and the emit-side hold's transfer, so the wire's clock
 *     record and phase-bias records move together and its phase side does not.
 *
 * Seam contract: free functions over @c NetworkRunContext that own NO state; every
 * persistent value is a reference into @c ctx.carry.clk_level.
 */

#include "filter/NetworkRunTypes.hpp"   // StationAssembly, ClockLevelState

#include <Eigen/Dense>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

class NetworkEstimator;
class NetworkProductEmitter;
struct NetworkSolutionEpoch;
struct NetworkRunContext;
struct NetworkCarryState;
struct NetworkRunTraces;
struct GnssTime;

/// Information weight at or above which a code row counts as FULL weight: the
/// population network_pf_rows.csv grades (it excludes the gross line-of-sight screen's
/// deweighted rows and anything stronger).
constexpr double kClvFullWeight = 1e-2;
/// Fewest ionosphere-free (station) pairs a satellite needs for an update.
constexpr int kClvMinPairs = 3;
/// Robust screen of the per-satellite pairs: a pair farther than this many scaled
/// median absolute deviations from the satellite's median is left out of the fit.
constexpr double kClvClipK = 4.0;
/// Floor (m) of that screen's scale, so a satellite whose pairs agree closely does not
/// clip its own noise.
constexpr double kClvClipFloorM = 0.05;
/// Innovation break: a level residual beyond this many predicted standard deviations
/// restarts the filter (the carried level was not what its information claimed).
constexpr double kClvBreakK = 4.0;

/// @brief Decay accumulated information to @p now_s.
/// @param info      information valid at @p t_info (1/m^2).
/// @param t_info    continuous-GPS seconds of @p info (0 = never set).
/// @param now_s     continuous-GPS seconds to decay to.
/// @param horizon_s e-folding horizon (s); a non-positive horizon disables the decay.
/// @return info * exp(-(now_s - t_info) / horizon_s); 0 when never set.
double clockLevelDecayedInfo(double info, double t_info, double now_s, double horizon_s);

/// @brief The level's code-only variance V_L = max(1 / I_L, floor^2).
/// @param info_now     the information I_L (1/m^2).
/// @param floor_m      the standard-deviation floor (m).
/// @param ignition_var the variance reported when there is no information.
/// @param at_floor     optional out: true when the floor bound.
/// @return V_L in m^2.
double clockLevelVariance(double info_now, double floor_m, double ignition_var,
                          bool* at_floor = nullptr);

/// @brief Information one ionosphere-free pair of code rows carries about the level.
/// @param w1    information weight (1/m^2) of the band-1 code row.
/// @param f1_hz its carrier frequency.
/// @param w2    information weight of the band-2 code row.
/// @param f2_hz its carrier frequency.
/// @return 1 / (A^2 / w1 + B^2 / w2), A = f1^2 / (f1^2 - f2^2), B = f2^2 / (f1^2 - f2^2);
///         0 when a weight is not positive or the frequencies coincide.
double clockLevelIfInformation(double w1, double f1_hz, double w2, double f2_hz);

/// @brief The ionosphere-free combination A v1 - B v2 of two code residuals.
/// @param v1    band-1 post-fit code residual (m).
/// @param f1_hz its carrier frequency.
/// @param v2    band-2 post-fit code residual (m).
/// @param f2_hz its carrier frequency.
/// @return the ionosphere-free residual (m); NaN when the frequencies coincide.
double clockLevelIfResidual(double v1, double f1_hz, double v2, double f2_hz);

/// One ionosphere-free pair of post-fit code residuals: one station, one satellite.
struct ClockLevelPair {
    std::string sat;        ///< RINEX satellite id
    int         station{-1};///< station index (any stable integer)
    double      v_if{0.0};  ///< ionosphere-free post-fit residual (m)
    double      w_if{0.0};  ///< its information (1/m^2), @ref clockLevelIfInformation
};

/// Outcome of @ref clockLevelTwoWay.
struct ClockLevelTwoWay {
    bool ok{false};                        ///< the fit ran and its factorization succeeded
    std::map<std::string, double> effect;  ///< satellite -> level effect (m; positive = the clock is high)
    std::map<std::string, double> info;    ///< satellite -> summed information of its used pairs (1/m^2)
    std::map<std::string, int>    n_pairs; ///< satellite -> its used pairs
    long long used{0};                     ///< pairs in the fit
    long long clipped{0};                  ///< pairs removed by the robust screen
    long long few{0};                      ///< satellites dropped for having fewer than the minimum pairs
};

/// @brief Two-way fit of the ionosphere-free residuals: v = e_sat - c_(station, constellation).
///
/// Each satellite's pairs are screened against their own median (|v - median| over
/// max(clip_floor_m, clip_k x 1.4826 MAD) is left out); satellites with fewer than
/// @p min_pairs pairs after the screen are dropped; the remaining pairs are fitted in
/// weighted least squares with one effect per satellite, one offset per (station,
/// constellation) and a sum-to-zero constraint on the satellite effects of each
/// constellation (the first character of the satellite id), which fixes the one
/// direction the offsets cannot separate and leaves the clock datum untouched.  The
/// offsets are per constellation, so a receiver's inter-system bias stays in them.
/// @param pairs        the epoch's pairs.
/// @param min_pairs    fewest pairs per satellite.
/// @param clip_k       screen width in scaled MADs.
/// @param clip_floor_m screen width floor (m).
/// @return the satellite effects and the bookkeeping.
ClockLevelTwoWay clockLevelTwoWay(const std::vector<ClockLevelPair>& pairs, int min_pairs,
                                  double clip_k, double clip_floor_m);

/// Outcome of @ref clockLevelFilterUpdate.
struct ClockLevelUpdate {
    double gain{0.0};       ///< fraction of the level residual the shift removes
    double info{0.0};       ///< the filter's information after the update (1/m^2)
    bool   restarted{false};///< the filter was (re)armed at this update
    bool   broke{false};    ///< …because the residual broke the prediction
    bool   at_floor{false}; ///< the information was capped at 1 / floor^2
};

/// @brief One update of a satellite's level filter.
///
/// Unarmed: the filter starts with this epoch's information and gain 1 (the level is
/// set to this epoch's code estimate).  Armed: when |e| exceeds @p break_k x
/// sqrt(1 / info_prior + 1 / i_ep) the filter restarts the same way; otherwise
/// info = min(info_prior + i_ep, 1 / floor^2) and gain = i_ep / info, the running mean of
/// the code's level over the filter's memory.
/// @param info_prior the armed filter's information decayed to this epoch (1/m^2).
/// @param armed      whether the filter is running.
/// @param i_ep       this epoch's information (1/m^2, > 0).
/// @param e          this epoch's level residual (m).
/// @param break_k    innovation break width.
/// @param floor_m    the standard-deviation floor (m).
/// @return the gain, the new information and the flags.
ClockLevelUpdate clockLevelFilterUpdate(double info_prior, bool armed, double i_ep, double e,
                                        double break_k, double floor_m);

/// @brief Move the carry along each satellite's level direction.
///
/// For every (satellite, shift d) of @p shift: the carried clock and every carried
/// phase bias of the satellite (@c prev_carry, parked entries included) move by d; so do
/// the @c s_prev of every coupling term on that clock or phase bias in every carried
/// local map (ambiguities, ZWD, ionosphere, receiver clock, receiver phase and code
/// biases, the gross-screen holds), which keeps every carried row's residual unchanged;
/// the clock-jump baselines (@c prev_clk, @c cb_exc_prev_clk, @c pb_hold_prev_clock_m),
/// the last solved phase bias (@c pb_level_prev) and any open pin bridge move with them;
/// and the emit-side transfer of every phase-bias signal the satellite has in the carry
/// or in the hold accumulates d (@c clk_level.sat[sat].xfer).  Code biases and every
/// other satellite are untouched.
/// @param carry the run's carried state.
/// @param shift satellite -> shift along its level direction (m).
/// @return the number of carried references moved.
long long clockLevelShiftCarry(NetworkCarryState& carry,
                               const std::map<std::string, double>& shift);

/// @brief The re-base seam: measure, filter and apply (see the file banner).
///
/// Runs after the stash of a kept epoch.  No-op unless @c network.clock_level_carry,
/// and skipped whole at a datum change (the residuals then straddle two frames).  A
/// satellite whose Q3 anchor decoupled this epoch is skipped (its clock stepped for a
/// reason the code level does not describe), and one the anchor does not hold (LOOSE:
/// the ignition, or the re-validation after a decouple) is not eligible; the filter
/// restarts when it becomes eligible.  Without the anchor every satellite is eligible.
/// @param ctx          the run context (knobs, carry, counters).
/// @param traces       the run's traces (network_clock_level.csv).
/// @param t            the epoch time.
/// @param est          the estimator after this epoch's final solve.
/// @param x_s          the shared-border solution.
/// @param asm_res      the epoch's station assemblies.
/// @param est_index_of station -> estimator index (-1 = not in the solve).
/// @param datum_changed whether the epoch changed the datum.
void networkClockLevelRebase(NetworkRunContext& ctx, NetworkRunTraces& traces,
                             const GnssTime& t, const NetworkEstimator& est,
                             const Eigen::VectorXd& x_s,
                             const std::vector<StationAssembly>& asm_res,
                             const std::vector<int>& est_index_of, bool datum_changed);

/// @brief Stamp each emitted satellite's level step (the shifts applied since its
///        previous emission) onto @c NetworkSatSolution::level_step_m and clear it.
///
/// Called at the emit before anything reads the clock values, so the clock emission
/// re-base, the emitter's phase-bias jump test and the C record's @c lvl= token carry
/// one declaration.  No-op unless @c network.clock_level_carry.
/// @param ctx the run context.
/// @param sol the epoch being emitted.
void networkClockLevelDeclare(NetworkRunContext& ctx, NetworkSolutionEpoch& sol);

/// @brief Run-end readout: V_L per constellation, the mechanism's counters and the
///        wire's.  Silent unless @c network.clock_level_carry.
/// @param ctx     the run context.
/// @param emitter the product emitter (its level-transfer jump-test count), or null.
/// @param log     the run log.
void networkClockLevelTeardown(const NetworkRunContext& ctx, const NetworkProductEmitter* emitter,
                               const std::shared_ptr<spdlog::logger>& log);

}  // namespace GnssProcessor
