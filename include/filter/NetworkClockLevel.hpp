#pragma once

/**
 * @file NetworkClockLevel.hpp
 * @brief The satellite clock level, estimated from the code alone and re-based into
 *        the carry (@c network.clock_level_carry; card CLV,
 *        docs/refound/CLV_clock_level_2026-09-22.md).
 *
 * The level direction of satellite s moves its clock column and every one of its
 * phase-bias columns by the same amount.  MeasurementModel.cpp writes the satellite
 * clock with a -1 partial on code and phase rows (the per-satellite clock block) and
 * the satellite phase bias with a +1 partial on phase rows only (the per-satellite
 * signal phase-bias block), and networkSplitStation_ in ApplicationNetworkAssembly.cpp
 * copies those columns verbatim into the shared border.  A shift d along the direction
 * therefore leaves every phase row invariant (-d + d) and moves every code row of s by
 * -d: the level is a code-only quantity.
 *
 * Why it freezes.  The time update of a coupling row (card 1d) shrinks each border term
 * by kappa = p / (p + q dt) with the legacy clock rate q = filter.process_noise.sat_clock_q
 * (1e-3 m^2/s: kappa about 3e-4 for a clock carried at the Q3 anchor's variance floor),
 * while the phase-bias terms keep kappa about 1.  A carried ambiguity, whose exact
 * regression is on the phase side (-clock + phase bias), then states its phase bias
 * alone, and the phase rows turn that into a phase-precision statement about the level.
 * Every carried ambiguity does this, so no single knob releases the level (the 22-arm
 * ladder in the card).
 *
 * What the card does.  The solve is not loosened: freeing the level inside the solve
 * makes it the cheapest outlet for any station's phase inconsistency (on the field bed,
 * one undetected L2W event at one station moved four satellite clocks by metres in one
 * epoch and tripped the Q3 anchor on each).  Instead, after every kept epoch:
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
 * Seam contract: free functions over @c NetworkRunContext that own no state; every
 * persistent value is a reference into @c ctx.carry.clk_level.
 */

#include "filter/NetworkRunTypes.hpp"   // StationAssembly, ClockLevelState

#include <Eigen/Dense>

#include "filter/NetworkCarrySites.hpp"   // CarrySiteCounts (card PRM phase 2)

#include <map>
#include <set>
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

/// Information weight at or above which a code row counts as full weight: the
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
/// Card CLV2 R-A2: standard deviation (m) of one station's same-band code difference (the
/// free signal minus the pin pair's first signal), the robust scatter the field baseline
/// shows about the two-way fit (0.061 to 0.077 m; 0.04 to 0.06 above 40 degrees of
/// elevation, 0.13 to 0.15 below 20).  Both codes share the band's multipath, which is why
/// it is a fraction of a single code's.
constexpr double kClvFcbSigmaM = 0.08;
/// Card CLV2 R-A2: floor (m) on a free column filter's standard deviation.  Above the
/// white-noise figure of one epoch's fit (about 0.02 m at ten cells), because the
/// multipath in the difference is correlated over minutes.
constexpr double kClvFcbFloorM = 0.02;
/// Card CLV2 R-A2: fewest station cells a satellite needs in an epoch's same-band fit.
constexpr int kClvFcbMinCells = 3;

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
    /// satellite -> variance of its effect at unit weight (the diagonal of the fit's
    /// inverse normals, the station offsets eliminated and the sum-to-zero constraint
    /// imposed): the effect's variance is this times @c s0sq (card CTU section 14).
    std::map<std::string, double> var;
    /// A posteriori unit variance of the fit: sum w (v - e + c)^2 over the used pairs,
    /// over used - (S - 1) - R.  NaN with no redundancy.
    double s0sq{std::numeric_limits<double>::quiet_NaN()};
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

/// @brief Is @p signal a free same-band extra of the pin pair (@p pin1, @p pin2)?
///
/// Pure.  A code signal on the band of the pair's first signal that is neither signal of
/// the pair (GPS C1W against C1C/C2W, Galileo C1X against C1C/C5Q).  The S-basis leaves
/// such a column free (its difference to the first pin signal is a pure inter-signal
/// correction the data reach), so it absorbs the level at ignition like the clock.  A
/// companion on the SECOND pin band is pinned at the broadcast group delay and is not
/// free, so it answers false.
/// @param signal RINEX code, e.g. "C1W".
/// @param pin1   the pin pair's first signal, e.g. "C1C".
/// @param pin2   the pin pair's second signal, e.g. "C2W".
/// @return true for a free same-band extra.
bool clockLevelIsFreeSameBand(const std::string& signal, const std::string& pin1,
                              const std::string& pin2);

/// @brief The share of a level shift d a free same-band code-bias column follows.
///
/// Pure.  Mode 0: none.  Mode 2: all of it.  Mode 1: 1 - (f2/f1)^2.  Derivation: a
/// station reading band 1 through the free extra k and band 2 through the pinned second
/// signal sees, after the clock moves by d, the code rows -d + x_k + I and -d + mu2 I
/// (mu2 = (f1/f2)^2); both vanish for I = d / mu2 and x_k = d (1 - 1/mu2), and the
/// ionosphere-free combination of the two rows is then unchanged, so a consumer on that
/// pair sees no step.  GPS L1/L2: 0.3928; Galileo E1/E5a: 0.4424.
/// @param mode  0 off, 1 "if", 2 "full".
/// @param f1_hz the pin pair's first carrier frequency.
/// @param f2_hz its second carrier frequency.
/// @return the follow factor (0 when a frequency is not positive in mode 1).
double clockLevelFreeCbFollowFactor(int mode, double f1_hz, double f2_hz);

/// Per-satellite input of the code-bias follow in @ref clockLevelShiftCarry.
struct ClockLevelCbFollow {
    double      factor{0.0};  ///< share of the satellite's level shift its free columns follow
    std::string pin1;         ///< the satellite's realized pin pair, first signal
    std::string pin2;         ///< …and second signal
};

/// Outcome of @ref clockLevelConfirmStep.
struct ClockLevelConfirm {
    bool   apply{false};  ///< the arm or break is confirmed: restart with @c e and @c info
    bool   reset{false};  ///< a pending sequence was dropped (a sign change)
    double e{0.0};        ///< the information-weighted mean effect of the confirmed epochs (m)
    double info{0.0};     ///< their summed information (1/m^2)
};

/// @brief Accumulate one epoch towards an arm (@p break_sign 0) or a break (+1 or -1).
///
/// Adds this epoch's effect and information to the satellite's pending sequence; a break
/// of the other sign than the pending one first drops the sequence.  When the sequence
/// reaches @p n_needed epochs it is returned as confirmed and cleared.
/// @param ls         the satellite's filter state (its pend_* fields are updated).
/// @param break_sign 0 for an arm, the sign of the innovation for a break.
/// @param i_ep       this epoch's information (1/m^2, > 0).
/// @param e          this epoch's level effect (m).
/// @param n_needed   the confirmation length (at least 1).
/// @return whether the restart is confirmed, with the mean effect and summed information.
ClockLevelConfirm clockLevelConfirmStep(ClockLevelSat& ls, int break_sign, double i_ep,
                                        double e, int n_needed);

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
/// or in the hold accumulates d (@c clk_level.sat[sat].xfer).  Without @p cb_follow the
/// code biases and every other satellite are untouched.  With it (card CLV return plan
/// R-A), every free same-band code-bias column of a listed satellite
/// (@ref clockLevelIsFreeSameBand against its pin pair) moves by factor x d: its carried
/// value, the @c s_prev of every coupling term on it, its time-linked datum reference
/// (@c code_bias_prev_), and its pending B-record declaration (@c cb_pending).  With
/// @p cb_direct (card CLV2 R-A2) a listed (satellite, signal) column moves by its own step
/// through the same references, whether or not the satellite has a level shift; a column
/// in @p cb_held does not follow the level (its own re-base replaces the follow share).
/// @param carry     the run's carried state.
/// @param shift     satellite -> shift along its level direction (m).
/// @param cb_follow optional satellite -> code-bias follow (null = none).
/// @param cb_moved  optional out: (satellite, signal) -> the code-bias step applied (m).
/// @param cb_direct optional (satellite, signal) -> a code-bias column's own step (m).
/// @param cb_held   optional (satellite, signal) columns that do not follow the level.
/// @param counts    optional out: per-site tallies of the walk (accumulated).
/// @return the number of carried references moved.
///
/// The walk is the traversal of carried references (card PRM phase 2,
/// @ref forEachCarriedReference) over the sites of @c CarryMechanism::ClockLevel, with the
/// phase hold's keys read for the transfer; the sites the re-base does not move are declared
/// with their reason in the mechanism table (card LGM's @c pc_carry among them).
long long clockLevelShiftCarry(
    NetworkCarryState& carry, const std::map<std::string, double>& shift,
    const std::map<std::string, ClockLevelCbFollow>* cb_follow = nullptr,
    std::map<std::pair<std::string, std::string>, double>* cb_moved = nullptr,
    const std::map<std::pair<std::string, std::string>, double>* cb_direct = nullptr,
    const std::set<std::pair<std::string, std::string>>* cb_held = nullptr,
    CarrySiteCounts* counts = nullptr);

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

/// @brief Card CTU G5 (section 9.3): after a warm resume, raise the restored dense border
///        along each satellite's level direction (its clock and phase-bias rows moved
///        together) by the code-only level variance 1/I_c of the writer monitor's
///        contrast-frame information (the snapshot's LB rows), once, before the first
///        leg epoch's rows and fold.  A raise, never a set: every phase-visible
///        combination keeps its variance and no information is added.  No-op unless
///        @c network.clock_level_restore_raise armed it at the restore.
/// @param ctx the run context (the carry is modified).
/// @param log the network logger.
void networkClockLevelRestoreRaise(NetworkRunContext& ctx,
                                   const std::shared_ptr<spdlog::logger>& log);

/// @brief The level monitor and bound raise of card CTU
///        (@c network.clock_level_monitor, @c network.clock_level_bound_raise;
///        docs/refound/CTU_coupling_time_update_2026-09-28.md section 9.3).
///
/// Reads the level as the re-base does (the pin pair's post-fit ionosphere-free code,
/// the two-way fit), with no shift: per satellite it accumulates the code-only level
/// information I_L, undecayed and uncapped, and reads the stashed border block's variance
/// along the satellite's level direction u (its clock and every carried phase bias),
/// P_L = u'Pu/(u'u)^2.  The ratio P_L/(1/I_L) is traced (logs/network_level_bound.csv
/// under @c network.trace_events) and read out at run end; a consistent estimator keeps
/// it at or above 1.  With the raise armed, a satellite below 1 has the block raised,
/// P += (1/I_L - P_L) u u', and its carried clock and phase-bias variances with it, so
/// the next fold starts the level at its code-only bound.  A datum-changed epoch is
/// skipped (the residuals straddle two frames).  The constellation's pinned clock is
/// not in the block and is not read.  No-op unless the monitor is armed.
/// @param ctx          the run context (knobs, carry, counters).
/// @param traces       the run's traces.
/// @param t            the epoch time.
/// @param est          the estimator after this epoch's final solve.
/// @param x_s          the shared-border solution.
/// @param asm_res      the epoch's station assemblies.
/// @param est_index_of station -> estimator index (-1 = not in the solve).
/// @param datum_changed whether the epoch changed the datum.
void networkClockLevelMonitor(NetworkRunContext& ctx, NetworkRunTraces& traces,
                              const GnssTime& t, const NetworkEstimator& est,
                              const Eigen::VectorXd& x_s,
                              const std::vector<StationAssembly>& asm_res,
                              const std::vector<int>& est_index_of, bool datum_changed);

/// @brief Stamp each emitted satellite's level step (the shifts applied since its
///        previous emission) onto @c NetworkSatSolution::level_step_m and clear it.
///
/// With @c network.clock_level_free_cb_follow the per-signal code-bias follow steps are
/// stamped the same way onto @c NetworkSatSolution::cb_level_step_m (the B record's
/// @c lvl= token) and cleared.
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
