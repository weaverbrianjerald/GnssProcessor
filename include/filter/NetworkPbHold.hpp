/**
 * @file NetworkPbHold.hpp
 * @brief Card PBH: the emit-side phase-bias HOLD seam (@c network.emit_pb_hold).
 *
 * One concern, one seam, per the network-producer decision tree: the emitted
 * phase bias of a (satellite, signal) is frozen at the first value of its
 * generation and a new generation is DECLARED, so a float consumer re-floats on
 * the declaration instead of integrating a wandering correction.
 *
 * ### Why a hold and not a signed delta
 *
 * PBH read 3 graded the wire against truth on both beds.  Within-arc RMS of the
 * phase bias ALONE is 0.0227 m on the sim and 0.0467 m on the field, against the
 * clock's 0.0030 / 0.0274 m, and the first differences of the two errors
 * correlate at +0.006 — the phase bias does not compensate the clock, it ADDS to
 * it.  The simulator's true satellite phase bias is constant in time, so on that
 * bed every movement of the emitted value is error by construction.  A
 * delta-passing contract would therefore transfer a frame move that is mostly
 * not a frame move; an IDENTITY-passing contract, in the manner of the @c iode=
 * ephemeris cutover, states instead that the value has not changed and names the
 * epoch at which that claim restarts.
 *
 * ### Ordering contract
 *
 * @ref networkPbHold is the LAST writer of @c NetworkPhaseBias::bias_m in an
 * epoch and runs immediately before @c NetworkProductEmitter::emitToStore:
 *
 *  - AFTER the carry-invariant emission re-base and the PB emission filter, both
 *    of which rewrite @c bias_m in @c sol, so the hold freezes what they produced
 *    rather than being overwritten by them;
 *  - AHEAD of the emitter's phase-bias value-jump test, so that test screens the
 *    HELD value — the declared population and the payload agree by construction;
 *  - AFTER every marking site of the epoch, so @ref NetworkProductEmitter::pendingMarkChannels
 *    reports the complete set of channels about to be declared, which is what the
 *    coupled-mark cost telemetry needs;
 *  - BEFORE the on-disk product write, which consumes the same @c sol, so the file
 *    and the in-process store carry the same held values.
 *
 * The estimator is never touched: the solve, the C records and the B records are
 * byte-identical with the knob ON, and OFF the seam returns before reading
 * anything.
 *
 * ### Invariant #7, both edges
 *
 * The DECOUPLING edge is the hold itself, and what it can accumulate is bounded
 * and MEASURED rather than asserted: every held emission contributes
 * |estimate - held| to a run-end distribution, so "what the hold hides" is a
 * number in the summary, not a hope.  The RE-COUPLING edge is the generation
 * start — a @c PB_HOLD / @c gen_start event with its cause, a counter, and the
 * @c gen= declaration the consumer re-floats on.  The one thing the hold
 * deliberately does NOT transfer is a coupled frame move (a mark naming the clock
 * and the phase bias together), so that cost is measured too: the de-commoned
 * satellite clock step at every such mark reaching a held key.
 */
#ifndef GNSSPROCESSOR_NETWORK_PB_HOLD_HPP
#define GNSSPROCESSOR_NETWORK_PB_HOLD_HPP

#include <memory>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
class NetworkProductEmitter;
class NetworkRunTraces;
struct NetworkRunContext;
struct NetworkSolutionEpoch;
struct SyncedEpoch;

/**
 * @brief Apply the card-PBH emit-side phase-bias hold to one epoch's solution.
 *
 * Per emitted (satellite, signal):
 *  - no held entry yet, or the key was last emitted more than
 *    @c network.emit_pb_hold_max_gap_s ago ⇒ START a generation.  The current
 *    estimate becomes the held value, the generation advances (first = 1), and a
 *    @c PB_HOLD / @c gen_start event is emitted with action @c first or @c gap.
 *  - otherwise ⇒ overwrite @c NetworkPhaseBias::bias_m with the held value and
 *    record |estimate - held| into the run-end distribution.
 *
 * Either way the entry is stamped with its generation, which travels to the
 * consumer on @c SsrPhaseBiasEntry::hold_generation and to the file on the
 * P-record @c gen= token.
 *
 * @param ctx      Run context — owns the hold state (@c ctx.carry.pb_hold*), the
 *                 counters and the knob snapshot.  The seam owns nothing.
 * @param config_  Run configuration (read for nothing but the knob's documented
 *                 home; the live values come from @c ctx.knobs).
 * @param log      Run logger (a one-shot arming line on the first held epoch).
 * @param traces   Trace sinks — the @c PB_HOLD event rows.
 * @param emitter  The product emitter, read ONLY through
 *                 @ref NetworkProductEmitter::pendingMarkChannels for the coupled
 *                 clock+phase-bias mark census.  Never mutated.
 * @param se       The synced epoch (its @c time stamps the event rows).
 * @param sol      The epoch about to be emitted; its @c phase_bias values are the
 *                 ONLY thing this seam writes.
 *
 * @note A no-op returning immediately when @c ctx.knobs.emit_pb_hold is false, so
 *       the OFF path is byte-identical and allocates nothing.
 */
void networkPbHold(NetworkRunContext& ctx,
                   const GnssConfig& config_,
                   const std::shared_ptr<spdlog::logger>& log,
                   NetworkRunTraces& traces,
                   const NetworkProductEmitter& emitter,
                   const SyncedEpoch& se,
                   NetworkSolutionEpoch& sol);

}  // namespace GnssProcessor

#endif  // GNSSPROCESSOR_NETWORK_PB_HOLD_HPP
