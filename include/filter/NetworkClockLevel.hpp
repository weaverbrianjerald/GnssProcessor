#pragma once

/**
 * @file NetworkClockLevel.hpp
 * @brief The satellite clock level read from the code alone: card CTU's level monitor,
 *        its innovation instrument and the G5 restore raise
 *        (docs/refound/CTU_coupling_time_update_2026-09-28.md section 9.3).
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
 * The read, after every kept epoch:
 *  1. the post-fit ionosphere-free code residual of each (station, satellite) pair of
 *     full-weight code rows on the constellation's pin bands is formed
 *     (@ref clockLevelIfResidual, @ref clockLevelIfInformation);
 *  2. a two-way least-squares fit separates one level effect per satellite from one
 *     offset per (station, constellation) (what the receiver clocks absorbed), the
 *     satellite effects summing to zero per constellation (@ref clockLevelTwoWay).
 * The monitor compares the code-only information with the carried border's variance
 * along each satellite's level direction and, armed, raises the border to that bound
 * (@ref networkClockLevelMonitor); it never moves a value.
 *
 * Seam contract: free functions over @c NetworkRunContext that own no state; every
 * persistent value is a reference into @c ctx.carry (the @c clm_* members).
 */

#include "filter/NetworkRunTypes.hpp"   // StationAssembly

#include <Eigen/Dense>

#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

class NetworkEstimator;
struct NetworkRunContext;
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

struct NetworkRun;              // app/NetworkRun.hpp (card NOM3 step EP1)
struct NetworkEpoch;            // app/NetworkEpoch.hpp (card NOM3 step EP1)

/**
 * @brief Card CTU G5 (section 9.3): after a warm resume, raise the restored dense border
 *        along each satellite's level direction (its clock and phase-bias rows moved
 *        together) by the code-only level variance 1/I_c of the writer monitor's
 *        contrast-frame information (the snapshot's LB rows), once, before the first
 *        leg epoch's rows and fold.  A raise, never a set: every phase-visible
 *        combination keeps its variance and no information is added.  No-op unless
 *        @c network.clock_level_restore_raise armed it at the restore.
 *
 * Epoch form (card NOM3 step EP1; generated by scripts/nom_ep1_cut.py): the run and the epoch
 * replace the explicit parameters; the definition binds each former parameter in its preamble,
 * under its former name.
 *
 * @param run The run (app/NetworkRun.hpp). Written through non-const bindings: run.ctx (ctx).
 *            Read: run.log (log).
 *
 * The former parameters, by binding:
 *   - @p ctx = run.ctx: the run context (the carry is modified).
 *   - @p log = run.log: the network logger.
 */
void networkClockLevelRestoreRaise(NetworkRun& run);

/**
 * @brief The level monitor and bound raise of card CTU
 *        (@c network.clock_level_monitor, @c network.clock_level_bound_raise;
 *        docs/refound/CTU_coupling_time_update_2026-09-28.md section 9.3).
 *
 * Reads the level (the pin pair's post-fit ionosphere-free code, the two-way fit, see
 * the file banner): per satellite it accumulates the code-only level information I_L,
 * undecayed and uncapped, and reads the stashed border block's variance along the
 * satellite's level direction u (its clock and every carried phase bias),
 * P_L = u'Pu/(u'u)^2.  The ratio P_L/(1/I_L) is traced (logs/network_level_bound.csv
 * under @c network.trace_events) and read out at run end; a consistent estimator keeps
 * it at or above 1.  With the raise armed, a satellite below 1 has the block raised,
 * P += (1/I_L - P_L) u u', and its carried clock and phase-bias variances with it, so
 * the next fold starts the level at its code-only bound.  A datum-changed epoch is
 * skipped (the residuals straddle two frames).  The constellation's pinned clock is
 * not in the block and is not read.  No-op unless the monitor is armed.
 *
 * Epoch form (card NOM3 step EP1; generated by scripts/nom_ep1_cut.py): the run and the epoch
 * replace the explicit parameters; the definition binds each former parameter in its preamble,
 * under its former name.
 *
 * @param run The run (app/NetworkRun.hpp). Written through non-const bindings: run.ctx (ctx),
 *            run.traces (traces). Read: run.engines.est (est).
 * @param ep The epoch (app/NetworkEpoch.hpp), read only. Bindings: ep.se.time (t),
 *           ep.solve.x_s (x_s), ep.assembly.asm_res (asm_res), ep.assembly.est_index_of
 *           (est_index_of), ep.solve.datum_changed (datum_changed).
 *
 * The former parameters, by binding:
 *   - @p ctx = run.ctx: the run context (knobs, carry, counters).
 *   - @p traces = run.traces: the run's traces.
 *   - @p t = ep.se.time: the epoch time.
 *   - @p est = run.engines.est: the estimator after this epoch's final solve.
 *   - @p x_s = ep.solve.x_s: the shared-border solution.
 *   - @p asm_res = ep.assembly.asm_res: the epoch's station assemblies.
 *   - @p est_index_of = ep.assembly.est_index_of: station -> estimator index (-1 = not in the
 *     solve).
 *   - @p datum_changed = ep.solve.datum_changed: whether the epoch changed the datum.
 */
void networkClockLevelMonitor(NetworkRun& run, const NetworkEpoch& ep);

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the CTU level monitor.
 *
 * The CTU level monitor: epochs, skips at a datum change and the level's motion.
 *
 * Gate: returns 0 at once unless `ctx.knobs.clm_on`.
 *
 * @param v The run at its end (filter/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkClockLevelMonitorRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

}  // namespace GnssProcessor
