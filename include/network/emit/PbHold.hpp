/**
 * @file PbHold.hpp
 * @brief Emit-side phase-bias hold seam (@c network.emit_pb_hold).
 *
 * A single-concern seam in the network-producer decision tree.  The emitted phase
 * bias of a (satellite, signal) is frozen at the first value of its generation, and
 * each new generation is declared, so a float consumer re-floats on the declaration
 * instead of integrating a wandering correction.
 *
 * ### Why a hold and not a signed delta
 *
 * Graded against truth on both beds, the within-arc RMS of the phase bias alone is
 * 0.0227 m on the sim and 0.0467 m on the field, against 0.0030 / 0.0274 m for the
 * clock, and the first differences of the two errors correlate at +0.006: the phase
 * bias error adds to the clock error instead of compensating it.  The simulator's
 * true satellite phase bias is constant in time, so on that bed every movement of
 * the emitted value is error.  A delta-passing contract would therefore transfer, as
 * a frame move, a change that is mostly not one.  An identity-passing contract,
 * like the @c iode= ephemeris cutover, states that the value has not changed and
 * names the epoch at which that statement restarts.  See
 * docs/refound/PBH_phase_bias_hold_2026-09-19.md §1.
 *
 * ### Ordering contract
 *
 * @ref networkPbHold is the last writer of @c NetworkPhaseBias::bias_m in an epoch.
 * Only read-only steps (the ledger-emit capture and the signal-roster declaration
 * build) run between it and @c NetworkProductEmitter::emitToStore.  It runs:
 *
 *  - after the carry-invariant emission re-base, which rewrites @c bias_m in @c sol,
 *    so the hold freezes its output instead of being overwritten by it;
 *  - ahead of the emitter's phase-bias value-jump test, so that test screens the held
 *    value and the declared population agrees with the payload;
 *  - after every marking site of the epoch, so @ref NetworkProductEmitter::pendingMarkChannels
 *    reports the complete set of channels about to be declared, which the coupled-mark
 *    cost telemetry needs;
 *  - before the on-disk product write, which consumes the same @c sol, so the file
 *    and the in-process store carry the same held values.
 *
 * The estimator is not touched: with the knob on, the solve, the C records and the
 * B records are byte-identical; with it off, the seam returns before reading
 * anything.
 *
 * ### Invariant #7, both edges
 *
 * The decoupling edge is the hold itself.  What it can accumulate is measured: every
 * held emission adds |estimate - held| to a run-end distribution, so the amount the
 * hold hides is reported in the run summary.  The re-coupling edge is the generation
 * start: a @c PB_HOLD / @c gen_start event with its cause, a counter, and the
 * @c gen= declaration the consumer re-floats on.  The hold does not transfer a
 * coupled frame move (a mark naming the clock and the phase bias together), so that
 * cost is measured as well: the de-commoned satellite clock step at every such mark
 * that reaches a held key.
 *
 * ### The phase-clock mode (@c network.emit_pb_hold_phase_clock)
 *
 * A held phase bias frozen while the emitted clock moves puts every clock move on the
 * wire's phase side @f$c - p@f$.  Under the exact coupling time update the
 * satellite's level converges, moving the clock and every phase bias of the satellite
 * together (invisible to phase), and the plain hold turns that code-only correction
 * into a phase-side drift the consumer's float ambiguities cannot follow.  The mode
 * holds the phase-visible clock instead: the held emission is
 * @f$p(t_0) + [\tilde c(t) - \tilde c(t_0)]@f$ with @f$\tilde c = c - M_k@f$, @f$M_k@f$ the
 * accumulated median emitted-clock step of the satellite's constellation @f$k@f$ over its
 * satellites present at consecutive emissions.  Every member of the family @f$c - p_{wire} = (c - p)(t_0) + \Phi(t)@f$ is
 * blind to the level on the phase side; this mode is @f$\Phi = 0@f$ (up to the common
 * mode).  The smoothed member (@f$\Phi@f$ = the fixed-point smoothed estimate of the
 * clock's motion since @f$t_0@f$) was derived and rejected: under the estimator's own
 * covariance the phase-visible ionosphere-free clock is known to a few millimetres,
 * its regression on the generation start decays within minutes, and the rule then
 * passes the ionosphere-free phase-bias wander.  Posture limit: within a
 * generation the consumer's phase-visible clock motion is the a priori clock
 * product's, which is valid with a precise a priori clock and not with a broadcast one.
 */
#ifndef GNSSPROCESSOR_NETWORK_PB_HOLD_HPP
#define GNSSPROCESSOR_NETWORK_PB_HOLD_HPP

#include <map>
#include <memory>
#include <string>
#include <vector>
#include <utility>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
class NetworkProductEmitter;
class NetworkRunTraces;
struct NetworkRunContext;
struct NetworkSolutionEpoch;
struct SyncedEpoch;

/**
 * @brief Apply the emit-side phase-bias hold to one epoch's solution.
 *
 * Per emitted (satellite, signal):
 *  - no held entry yet, or @c network.emit_pb_hold_max_gap_s is positive and the key
 *    was last emitted more than that many seconds ago: start a generation.  The
 *    current estimate becomes the held value, the generation advances (the first is
 *    1), and a @c PB_HOLD / @c gen_start event is emitted with action @c first or
 *    @c gap.
 *  - otherwise: overwrite @c NetworkPhaseBias::bias_m with the held value and record
 *    |estimate - held| into the run-end distribution.  With the level-aware hold
 *    (@c network.emit_pb_hold_phase_clock) armed, the held value is shifted by the
 *    satellite's common-removed clock change since the generation started,
 *    @f$\tilde c(t) - \tilde c(t_0)@f$ (see the file header), so the phase-visible
 *    clock is held; an epoch without the satellite's clock reuses the last transfer,
 *    and a generation started without one is held plainly.
 *
 * In both cases the entry is stamped with its generation, which travels to the
 * consumer on @c SsrPhaseBiasEntry::hold_generation and to the file on the
 * P-record @c gen= token.
 *
 * @param ctx      Run context: owns the hold state (@c ctx.carry.pb_hold*), the
 *                 counters and the knob snapshot.  The seam owns nothing.
 * @param config_  Run configuration.  Not read; the live values come from
 *                 @c ctx.knobs.  The parameter keeps the seam signature uniform.
 * @param log      Run logger (one arming line, at the end of the first epoch that
 *                 starts a generation).
 * @param traces   Trace sinks: the @c PB_HOLD event rows.
 * @param emitter  The product emitter, read only through
 *                 @ref NetworkProductEmitter::pendingMarkChannels for the coupled
 *                 clock+phase-bias mark census.  Not mutated.
 * @param se       The synced epoch (its @c time stamps the event rows).
 * @param sol      The epoch about to be emitted; its @c phase_bias values are the
 *                 only thing this seam writes.
 *
 * @note Returns immediately when @c ctx.knobs.emit_pb_hold is false (or the epoch
 *       has no satellites), so the off path is byte-identical and allocates nothing.
 */
void networkPbHold(NetworkRunContext& ctx,
                   const GnssConfig& config_,
                   const std::shared_ptr<spdlog::logger>& log,
                   NetworkRunTraces& traces,
                   const NetworkProductEmitter& emitter,
                   const SyncedEpoch& se,
                   NetworkSolutionEpoch& sol);

/**
 * @brief The median emitted-clock step of one constellation in one epoch.
 *
 * The median, over the satellites of @p sol of constellation @p constellation (the
 * first character of the satellite id) with a finite emitted clock that also have an
 * entry in @p prev_clock_m, of @c clock_c0_m minus that entry (an even count takes the
 * mean of the two middle values).  Per constellation because each constellation's
 * clock common mode moves on its own (its own pivot, its own frame-hold re-anchor), and
 * the consumer absorbs it through its receiver clock and inter-system bias.
 *
 * @param sol            The epoch about to be emitted.
 * @param prev_clock_m   Satellite -> emitted clock (m) at the previous emission.
 * @param constellation  The constellation letter ('G', 'E', ...).
 * @param median_m       Out: the median step (m); untouched when the result is false.
 * @return false when no satellite of the constellation has a clock at both emissions.
 */
bool pbHoldCommonClockStep(const NetworkSolutionEpoch& sol,
                           const std::map<std::string, double>& prev_clock_m,
                           char constellation, double& median_m);

/**
 * @brief The epoch's de-commoned emitted-clock step of every satellite, in metres.
 *
 * For a satellite @c s of constellation @c k with a finite emitted clock at this and the
 * previous emission, @c d_s = (c_s(k) - c_s(k-1)) - M_k, where @c M_k is the median of
 * the steps of the satellites of constellation @c k alone (an even count takes the mean of
 * the two middle values). The median is per constellation, as in @ref pbHoldCommonClockStep:
 * each constellation's clock common mode (its pivot handover, its frame-hold re-anchor) moves
 * on its own, so one median across constellations leaves the difference of the two modes in
 * every satellite's step. What remains is the satellite-specific step, the quantity a coupled
 * frame move would have to transfer. A satellite without a previous clock, or a step that
 * is not finite, is absent from the result.
 *
 * @param sol            The epoch about to be emitted.
 * @param prev_clock_m   Satellite -> emitted clock (m) at the previous emission.
 * @return Satellite -> de-commoned step (m).
 */
std::map<std::string, double> pbHoldDecommonedClockStep(
    const NetworkSolutionEpoch& sol,
    const std::map<std::string, double>& prev_clock_m);

/// @brief Run-scope counters of PbHold, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct PbHoldCounters {
    // --- The emit-side phase-bias hold (network.emit_pb_hold) ---
    // On-path telemetry of invariant #4 and the two edges of invariant #7.  All zero on
    // an off run, so a run reporting 0 held emissions has an inert hold however clean
    // the log reads.  The cost of the decoupling edge is not a count but a distribution
    // (ctx.carry.pb_hold_dev_abs_m, |estimate - held| over every held emission) plus the
    // coupled-mark clock step (pb_hold_coupled_step_m); the re-coupling edge is
    // n_pbh_gen_first + n_pbh_gen_gap, one PB_HOLD/gen_start event each.
    long long n_pbh_keys           = 0;  ///< distinct (sat, signal) keys the hold ever held
    long long n_pbh_gen_first      = 0;  ///< generations started by a first sighting
    long long n_pbh_gen_gap        = 0;  ///< generations started by a gap > emit_pb_hold_max_gap_s
    long long n_pbh_held_emissions = 0;  ///< emissions whose value the hold overwrote
    long long n_pbh_coupled_marks  = 0;  ///< coupled clock+phase-bias marks reaching a held key
    // --- Level-aware hold (network.emit_pb_hold_phase_clock) ---
    long long n_pbh_frz_moved      = 0;  ///< held emissions whose value the transfer moved (non-zero)
    long long n_pbh_frz_noclock    = 0;  ///< held emissions on an epoch without the satellite's emitted clock (last transfer reused)
    long long n_pbh_frz_noref      = 0;  ///< held emissions of a generation started without a clock (held plainly)
    long long n_pbh_frz_epochs     = 0;  ///< epochs that advanced M (at least one satellite with a clock at both emissions)
    long long n_pbh_frz_coupled    = 0;  ///< coupled clock+phase-bias marks whose clock step the mode transferred
    long long smh_bridges          = 0;  ///< switches from a live clone to the phase-clock transfer at a disengagement
    long long smh_held_bridged     = 0;  ///< held emissions on the phase-clock transfer while the key waits for a clone
    long long smh_reverts          = 0;  ///< held keys switched to LAH's transfer by their clone's death
    long long smh_held_smoothed    = 0;  ///< held emissions that wrote the clone's transfer
    long long smh_held_lah         = 0;  ///< held emissions that wrote LAH's transfer after a revert (the rest of the generation)
    long long smh_w_invalid        = 0;  ///< held emissions whose W is void (the parent clock left S this generation)
    std::vector<double> smh_xfer_abs;    ///< |the clone's transfer| per smoothed held emission (m)
    std::vector<double> smh_diff_abs;    ///< |the clone's transfer - LAH's| per smoothed held emission (m)
    std::vector<double> smh_phi_abs;     ///< |Phi| per smoothed held emission (m)
    std::map<std::pair<char, int>, std::pair<double, long long>> smh_r;   ///< (constellation, hour) -> (sum r, count), r = Phi^2 / (W - V_post)
};

}  // namespace GnssProcessor

#endif  // GNSSPROCESSOR_NETWORK_PB_HOLD_HPP
