#pragma once
/**
 * @file NetworkRaimWithinEpoch.hpp
 * @brief Stages 13 and 14 of the network solve (card NOM3 step SG): the within-epoch RAIM tier
 *        T2 (assess, drop the dominant culprit, replay and re-solve, undo a failed re-solve,
 *        settle) and the A4 sigma0 ceiling (the per-constellation carve-out, the flight-recorder
 *        edge, the withhold-era edges and the withhold decision), with the sigma0 exit's tail.
 *        The tier's run-end report sits beside them.
 */

#include "network/driver/NetworkRunTypes.hpp"   // EpochAction, EpochWorkspace

#include <Eigen/Core>

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

struct EpochEvents;   // network/driver/NetworkEpoch.hpp

struct NetworkRun;
struct NetworkEpoch;
struct NetworkRunEndView;
struct EpochDdBuild;            // network/driver/NetworkEpoch.hpp
struct NetworkPostFitHandoff;   // network/solve/NetworkSolveGlue.hpp
struct SolveDatum;              // network/solve/NetworkSolveGlue.hpp
struct WithinEpochOutcome;      // network/solve/NetworkSolveGlue.hpp
struct Sigma0CeilingDecision;   // network/solve/NetworkSolveGlue.hpp
struct SolveSkipFacts;          // network/solve/NetworkSolveGlue.hpp
struct WithinEpochCulprits;     // network/qc/NetworkWithinEpochCulprits.hpp

/**
 * @brief Stage 13, T2: when the robust sigma0 shows the joint solve contaminated, drop the most
 *        prominent culprit (an arc, a satellite or a station), replay the captured shared system
 *        over a fresh beginEpoch() and re-solve, up to the adaptation budget; a re-solve that
 *        fails is undone to the last landed state (card RPD section 7).  Then the reduce-once
 *        check.
 * @param run          The run: the estimator is rebuilt by each replay; ctx carry and counters
 *                     and the events are written.
 * @param ep           The epoch, read: se, clock (carry_warm), columns, assembly (asm_res).
 * @param datum        The clock datum (read: plan).
 * @param x_s          [in/out] The shared solution, replaced by each landed re-solve.
 * @param ws           [in/out] The replay capture and the culprit sets (adapted, then cleared).
 * @param est_index_of [in/out] The station-slot map, rebuilt by each replay.
 * @param dd_build     [in/out] The DD carry build, rewritten by each replay's DD rebuild.
 * @param handoff      [in/out] The post-fit hand-off: the tier's refloat and retro-feed sets.
 * @param we           [out] The tier's outcome (WithinEpochOutcome).
 * @return SkipEpoch when an undo cannot complete (exit WeRestoreFailed), else Proceed.
 * @param events The epoch's events; writes solve (the tier's first-assess sigma0 and impulse meters).
 */
EpochAction networkWithinEpochAdapt(NetworkRun& run, const NetworkEpoch& ep, const SolveDatum& datum,
                                    Eigen::VectorXd& x_s, EpochWorkspace& ws,
                                    std::vector<int>& est_index_of, EpochDdBuild& dd_build,
                                    NetworkPostFitHandoff& handoff, WithinEpochOutcome& we, EpochEvents& events);

/// The culprit the adaptation loop drops next (card NOM3 step SG2): its granularity and identity.
struct WeChoice {
    enum class Kind { None, Arc, Sat, Sta };
    Kind        kind{Kind::None};  ///< None when nothing clears the gate.
    int         arc_sta{-1};       ///< Arc culprit: the station.
    std::string arc_sat;           ///< Arc culprit: the satellite.
    std::string sat;               ///< Satellite culprit.
    int         sta{-1};           ///< Station culprit.
};

/**
 * @brief The culprit T2 drops next (pure; card NOM3 step SG2): the best arc, satellite and
 *        station not yet dropped, each gated on effect / sigma0 > raim_k; a satellite with at
 *        least raim_min_sta arcs over the gate takes its arc's place (consensus escalation);
 *        on an equal score the preference is arc, then satellite, then station (surgical-first),
 *        and a station's score is de-rated by sta_bias.
 * @param arc_eff      The per-(station, satellite) interactions of the median polish.
 * @param sat_eff      The per-satellite effects.
 * @param sta_eff      The per-station effects.
 * @param culprits     The culprits already dropped this epoch (excluded).
 * @param sigma0       The current robust sigma0.
 * @param surgical     network.raim_surgical_station: arcs are candidates.
 * @param sta_bias     The station de-rating (1 unless surgical).
 * @param raim_k       The flag threshold on effect / sigma0.
 * @param raim_min_sta The cross-station consensus count.
 * @return The choice; kind None when nothing clears the gate.
 */
WeChoice weChooseCulprit(const std::map<std::pair<int, std::string>, double>& arc_eff,
                         const std::map<std::string, double>& sat_eff, const std::map<int, double>& sta_eff,
                         const WithinEpochCulprits& culprits, double sigma0, bool surgical, double sta_bias,
                         double raim_k, int raim_min_sta);

/**
 * @brief Stage 14, the A4 ceiling: the per-constellation carve-out (RTA-10), the flight-recorder
 *        edge (INC-5), both withhold-era edges (SPEC_withhold_era_release section 3) and the
 *        withhold decision; on a kept epoch, the PRM response of the landed replay.
 * @param run     The run: ctx carry (wh_era_*) and counters, the recorder latch and the events
 *                are written.
 * @param ep      The epoch, read: se.
 * @param we      [in/out] The tier's outcome (read; its per-constellation pools are permuted).
 * @param ceiling [out] The carve-out decision (Sigma0CeilingDecision).
 * @return SkipEpoch when the epoch is withheld (exit Sigma0Withheld), else Proceed.
 * @param events The epoch's events; writes qc (the flight recorder's trip reason).
 */
EpochAction networkSigma0Ceiling(NetworkRun& run, const NetworkEpoch& ep, WithinEpochOutcome& we,
                                 Sigma0CeilingDecision& ceiling, EpochEvents& events);

/**
 * @brief The sigma0 exit's tail: the flight-recorder dump of the withheld epoch, then the free
 *        erase of the withhold-era response (G5-safe order).
 * @param run The run: the recorder traces, ctx carry (local_carry) and counters are written.
 * @param ep  The epoch, read: se, assembly (asm_res).
 * @param f   The exit's facts (read: plan, est_index_of).
 */
void networkSigma0WithholdTail(NetworkRun& run, const NetworkEpoch& ep, const SolveSkipFacts& f);

/**
 * @brief Run-end report: within-epoch RAIM: the WE-CI screen and the T2 responses.
 *
 * The within-epoch RAIM tier: the WE-CI code and ionosphere screen
 * (network.raim_we_code_iono) and the T2 within-epoch responses with their failed
 * re-solves (network.raim_within_epoch).
 *
 * Gate: each section under its own condition, as in the teardown:
 * `config_.networkRaimWeCodeIono()`; `config_.networkRaimWithinEpoch()`.
 *
 * @param v The run at its end (network/runend/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkWithinEpochRaimRunEnd(const NetworkRunEndView& v);

} // namespace GnssProcessor
