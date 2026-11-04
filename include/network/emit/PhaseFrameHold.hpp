#pragma once
/**
 * @file PhaseFrameHold.hpp
 * @brief The emitted phase-visible clock in a per-constellation held frame
 *        (@c network.emit_phase_frame_hold,
 *        docs/refound/ICL_inter_constellation_level_2026-11-03.md).
 *
 * ## Problem addressed
 *
 * A receiver estimates one GPS-to-Galileo offset for code and phase.  The clock frame hold
 * (@c network.emit_clock_frame_hold, network/emit/ClockEmitRebase.hpp) emits each
 * constellation's clocks in a held frame, so the code-visible level a receiver sees moves with
 * the median of the satellites' a priori increments and not with the clock datum.  The phase
 * biases are written after it by the phase-bias hold, and under the smoothed hold
 * (@c network.emit_pb_hold_smoothed) the phase-visible clock follows the clone's passed motion
 * c(t|t) - c(t0|t): the solve's motion in the solve's frame, in which one pivot satellite per
 * constellation is pinned to its a priori.  Each constellation's phase-visible level is then
 * its pivot's a priori error.  On the broadcast a priori that is the pivot's broadcast misfit;
 * under the tracked a priori it is a random walk, since the track sums the IF-TD column's
 * per-epoch errors and nothing corrects the datum satellite's.  On the broadcast a priori with
 * the two-way IF-TD centring and the tracked a priori (DOY 129 and 132 of 2024, 6 h), the
 * GPS-to-Galileo phase-visible level moved 188 and 193 mm over 120 epochs against the CODE
 * clock (the GPS + Galileo golden: 2.5 mm) while the code-visible one moved 23 and 28 mm; a
 * receiver whose offset is frozen takes the difference into its position.
 *
 * ## The mechanism
 *
 * After the phase-bias hold (the last writer of the phase biases) and before the broadcast
 * wire, per constellation: each satellite with an emitted clock and both phase biases of a
 * modelled pair gives its phase-visible correction against the raw a priori,
 * v = C - P_IF + U (U the tracked a priori's offset, 0 without it).  The satellites that
 * continue from the previous emitted epoch (the same pair, the same generation of both phase
 * biases, within the gap horizon) and carry no pending mark (a datum change of their
 * constellation, a retro bump of their own) vote; with at least @c min_voters of them the
 * frame G moves by the median of their innovations, otherwise it stays.  Every phase bias of
 * every satellite of the constellation is emitted as P + G, so the phase-visible clock moves
 * by -G while every between-satellite difference and every combination of one satellite's
 * phase biases across signals is unchanged.  The removed direction is the constellation's
 * phase-visible common mode, the one a receiver's inter-system offset (and, for the
 * reference constellation, its clock) absorbs: the phase side then moves like the code side.
 *
 * ## Both edges
 *
 * A constellation's first vote starts its frame (@c PHASE_FRAME @c frame_start).  When |G|
 * exceeds @c leash_m the constellation re-anchors (G = 0) and the caller pairs the re-anchor
 * with a datum mark, so a receiver re-inflates its offset (@c reanchor).  A warm resume whose
 * first armed epoch follows emitted products marks every constellation present the same way
 * (@c resume_seam), since the frame restarts cold.  A pending datum change leaves the
 * constellation's frame where it is (its step reaches the wire, already marked).  Run-end
 * line: @ref networkPhaseFrameHoldRunEnd; trace @c network_phase_frame.csv.
 *
 * Outputs only: no estimate moves.  Default off; requires @c network.emit_pb_hold, whose
 * generations delimit the vote.
 *
 * ## The vote in the hold's level (@c network.phase_frame_hold_level_vote)
 *
 * The vote above takes the phase side to the median of the raw broadcast a priori increments,
 * while the clock frame hold takes the code side to the median of the tracked ones, so each
 * constellation's code-visible and phase-visible levels drift apart (20 to 25 mm per 120 epochs
 * on the broadcast posture with the track;
 * docs/refound/ICL2_frame_day_one_level_2026-11-03.md).  In this mode each satellite votes on
 * w = C - P_IF - M, M the phase-bias hold's accumulated median step of its constellation's
 * emitted clocks (@c pb_hold_frz_M, the level the phase-clock hold keeps on code and phase
 * alike).  The phase-clock hold writes each satellite's de-commoned clock step into its held
 * phase biases, so w is constant within a generation and the frame does not move: an exact
 * no-op, with a step of at most @c step_floor_m (1e-9 m, far above the hold's rounding and far
 * below any physical motion) not applied.  The smoothed hold writes the same transfer minus the
 * clone's passed motion Phi, so w moves with Phi and the frame removes Phi's common mode: each
 * constellation's phase-visible level then follows M, its code-visible one.  The decoupling edge
 * is the same @c frame_start event, named with the vote; the re-coupling edges are the same
 * re-anchor and resume seam.
 */

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

class NetworkRunTraces;
struct NetworkRunContext;
struct NetworkRunEndView;
struct NetworkSatSolution;
struct NetworkSolutionEpoch;
struct SyncedEpoch;

/// One satellite's phase-visible correction at one emitted epoch.
struct PhaseFrameSample {
    std::string   pair;      ///< The modelled pair, as "L1W+L2W".
    std::uint32_t gen1{0};   ///< Hold generation of the pair's first phase bias.
    std::uint32_t gen2{0};   ///< Hold generation of the pair's second phase bias.
    double        v_m{0.0};  ///< C - P_IF + offset (m), before the frame: + U against the raw a priori, - M in the hold's level.
    double        t_s{0.0};  ///< Product content time (s): week * 604800 + tow.
};

/**
 * @brief The satellite's phase-visible correction against the raw a priori, or nothing.
 *
 * Takes the first modelled pair whose two phase biases are emitted: GPS L1W with L2W, then
 * L1C with L2W; Galileo L1C with L5Q, then L1X with L5X; BeiDou L2I with L6I.  The
 * ionosphere-free phase bias is P_IF = a P1 - (a - 1) P2 with a = f1^2 / (f1^2 - f2^2).
 *
 * @param s    The emitted satellite: its clock and its phase biases after the phase-bias hold.
 * @param u_m  The offset added to C - P_IF (m): its tracked a priori offset U for the vote
 *             against the raw a priori (0 without the track), or -M, its constellation's hold
 *             level, for the vote in the hold's level.
 * @param t_s  Product content time (s).
 * @return Empty when the satellite has no emitted clock or no complete modelled pair.
 */
std::optional<PhaseFrameSample> phaseFrameSample(const NetworkSatSolution& s, double u_m,
                                                 double t_s);

/// One satellite entering an epoch of the frame.
struct PhaseFrameEntry {
    std::string                     sat;            ///< RINEX satellite id ("G01").
    std::optional<PhaseFrameSample> now;            ///< This epoch's sample; empty: no vote, nothing stored.
    bool                            forced{false};  ///< A datum change of its constellation or a retro bump of its own is pending: no vote.
};

/**
 * @brief The step floor of the vote in the hold's level (m).
 *
 * Under the phase-clock hold every satellite's C - P_IF - M is constant within a generation in
 * exact arithmetic; rebuilt from the emitted doubles it moves by at most about 1e-14 m per epoch
 * (measured on the GPS + Galileo golden, DOY 129 and 132).  A median step of at most this floor is
 * that rounding and is not applied, so the frame stays exactly 0 and the products are unchanged.
 */
inline constexpr double kPhaseFrameLevelVoteFloorM = 1e-9;

/// Tunables of the frame.
struct PhaseFrameConfig {
    double leash_m{1.0};      ///< |G| re-anchor bound (m), @c network.phase_frame_leash_m.
    int    min_voters{4};     ///< Fewer voters: the frame does not move this epoch (the clock frame hold's @c min_common).
    double max_gap_s{600.0};  ///< A previous sample older than this (s) does not vote (the clock frame hold's horizon).
    double step_floor_m{0.0}; ///< A median step with |step| at most this (m) is not applied (@ref PhaseFrameStep::floored); 0: every step applies.  1e-9 m in the vote in the hold's level.
};

/// One constellation's frame at one epoch.
struct PhaseFrameStep {
    int    voters{0};          ///< Satellites that voted.
    int    forced{0};          ///< Satellites left out for a pending mark.
    bool   moved{false};       ///< At least @c min_voters voted.
    double step_m{0.0};        ///< The median innovation (m); 0 unless @ref moved, and 0 when @ref floored.
    bool   floored{false};     ///< The median innovation was at most @c step_floor_m in magnitude and was not applied.
    bool   started{false};     ///< This epoch is the frame's first vote.
    bool   reanchored{false};  ///< |G| exceeded the leash and was reset to 0.
    double g_peak_m{0.0};      ///< G after the step, before any re-anchor (m).
    double g_m{0.0};           ///< G after the step and any re-anchor (m).
};

/**
 * @brief Pure: one epoch of the per-constellation frame.
 *
 * For each constellation with an entry, the voters are the entries with a sample that are not
 * forced and whose satellite has a stored sample of the same pair and generations, no older than
 * @c max_gap_s; their innovation is v - v_prev.  With at least @c min_voters voters the step is
 * their median (an even count takes the mean of the two middle values), set to 0 when its
 * magnitude is at most @c step_floor_m (@ref PhaseFrameStep::floored): G += step, G created at
 * 0 by the first such epoch (@ref PhaseFrameStep::started).  |G| above @c leash_m resets G to 0
 * (@ref PhaseFrameStep::reanchored).  Then @p prev is replaced by this epoch's samples.
 *
 * @param entries  This epoch's satellites.
 * @param t_s      This epoch's content time (s).
 * @param prev     Satellite -> its sample at the previous emitted epoch; replaced.
 * @param frame_g  Constellation letter -> G (m), present once the frame has started; updated.
 * @param cfg      Tunables.
 * @return Constellation letter -> its step, for every constellation with an entry.
 */
std::map<char, PhaseFrameStep> phaseFrameHoldEpoch(const std::vector<PhaseFrameEntry>& entries,
                                                   double t_s,
                                                   std::map<std::string, PhaseFrameSample>& prev,
                                                   std::map<char, double>& frame_g,
                                                   const PhaseFrameConfig& cfg);

/**
 * @brief The seam: one emitted epoch of the phase-visible frame (@c network.emit_phase_frame_hold).
 *
 * Builds each satellite's sample from @p sol after the phase-bias hold, runs
 * @ref phaseFrameHoldEpoch on the constellation table's frames (@c phase_frame_g_m) and the
 * satellite table's previous samples (@c phase_frame_prev), adds each constellation's G to every
 * phase bias of its satellites, and writes the counters, the trace rows and the events.  Called
 * by networkEmitStore after networkPbHold and before networkBrdcWire.  No-op unless the knob is
 * armed.
 *
 * @param ctx            Run context (the knob snapshot, the counters, the entity tables).
 * @param traces         Trace sinks (the @c PHASE_FRAME events, @c network_phase_frame.csv).
 * @param se             The synced epoch (its time stamps the rows).
 * @param marked_consts  Constellation letters with a datum change pending this epoch.
 * @param global_bump    A datum change of every constellation is pending.
 * @param retro_sats     Satellites with a retro bump pending this epoch.
 * @param n_emitted      Epochs emitted before this one (the warm-resume seam reads it).
 * @param sol            The epoch about to be emitted; its phase biases are written.
 * @return The constellation letters the caller marks as a datum change (re-anchors and the
 *         resume seam).
 */
std::set<char> networkPhaseFrameHold(NetworkRunContext& ctx, NetworkRunTraces& traces,
                                     const SyncedEpoch& se, const std::set<char>& marked_consts,
                                     bool global_bump, const std::set<std::string>& retro_sats,
                                     long long n_emitted, NetworkSolutionEpoch& sol);

/**
 * @brief Run-end report: the phase-visible frame hold.
 *
 * Per constellation the epochs voted and paused, the datum-change epochs left out, the voters,
 * the |step| ladder, the summed |step| drained, the largest |G| and the re-anchors; then the
 * resume seams, the phase biases shifted and the satellite-epochs without a complete pair.
 * "0 voted" is inert wiring.  Silent unless the knob is armed.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkPhaseFrameHoldRunEnd(const NetworkRunEndView& v);

/// @brief Run-scope counters of PhaseFrameHold, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct PhaseFrameHoldCounters {
    long long phf_epochs{0};                              ///< Emitted epochs the frame ran on.
    std::map<char, long long> phf_voted;                  ///< Constellation -> epochs its frame moved.
    std::map<char, long long> phf_paused;                 ///< Constellation -> epochs with fewer voters than the minimum (no datum change pending).
    std::map<char, long long> phf_marked;                 ///< Constellation -> epochs left out for a pending datum change.
    std::map<char, long long> phf_voters;                 ///< Constellation -> voters summed over the voted epochs.
    std::map<char, std::array<long long, 6>> phf_step_hist;  ///< Constellation -> |step| below 0.5, 1, 2, 5, 10 mm and above.
    std::map<char, double> phf_drained_m;                 ///< Constellation -> summed |step| (m): the common mode removed from the phase side.
    std::map<char, double> phf_max_abs_g_m;               ///< Constellation -> largest |G| (m): the headroom against the leash.
    std::map<char, long long> phf_reanchors;              ///< Constellation -> leash re-anchors (each a datum mark).
    long long phf_resume_seams{0};                        ///< Constellations marked at a warm resume's first armed epoch.
    long long phf_biases_shifted{0};                      ///< Phase-bias values emitted with a nonzero G.
    long long phf_no_pair{0};                             ///< Satellite-epochs with phase biases and a clock but no complete modelled pair.
    std::map<char, long long> phf_floored;                ///< Constellation -> voted epochs whose step was at most the floor and not applied (the vote in the hold's level).
    long long phf_no_level{0};                            ///< Satellite-epochs left without a sample because their constellation had no hold level M (the vote in the hold's level).
};

}  // namespace GnssProcessor
