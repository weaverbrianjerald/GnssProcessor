#pragma once
/**
 * @file RaimWithinEpoch.hpp
 * @brief Stages 13 and 14 of the network solve: the within-epoch RAIM tier
 *        T2 (assess, drop the dominant culprit, replay and re-solve, undo a failed re-solve,
 *        settle) and the A4 sigma0 ceiling (the per-constellation carve-out, the flight-recorder
 *        edge, the withhold-era edges and the withhold decision), with the sigma0 exit's tail.
 *        The tier's run-end report sits beside them.
 */

#include "network/driver/RunTypes.hpp"   // EpochAction, EpochWorkspace

#include <Eigen/Core>

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <cstddef>

namespace GnssProcessor {

struct EpochEvents;   // network/driver/Epoch.hpp

struct NetworkRun;
struct NetworkEpoch;
struct NetworkRunEndView;
struct EpochDdBuild;            // network/driver/Epoch.hpp
struct NetworkPostFitHandoff;   // network/solve/SolveGlue.hpp
struct SolveDatum;              // network/solve/SolveGlue.hpp
struct WithinEpochOutcome;      // network/solve/SolveGlue.hpp
struct Sigma0CeilingDecision;   // network/solve/SolveGlue.hpp
struct SolveSkipFacts;          // network/solve/SolveGlue.hpp
struct WithinEpochCulprits;     // network/qc/WithinEpochCulprits.hpp

/**
 * @brief Stage 13, T2: when the robust sigma0 shows the joint solve contaminated, drop the most
 *        prominent culprit (an arc, a satellite or a station), replay the captured shared system
 *        over a fresh beginEpoch() and re-solve, up to the adaptation budget; a re-solve that
 *        fails is undone to the last landed state.  Then the reduce-once
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

/// The culprit the adaptation loop drops next: its granularity and identity.
struct WeChoice {
    enum class Kind { None, Arc, Sat, Sta };
    Kind        kind{Kind::None};  ///< None when nothing clears the gate.
    int         arc_sta{-1};       ///< Arc culprit: the station.
    std::string arc_sat;           ///< Arc culprit: the satellite.
    std::string sat;               ///< Satellite culprit.
    int         sta{-1};           ///< Station culprit.
};

/**
 * @brief The culprit T2 drops next (pure): the best arc, satellite and
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
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkWithinEpochRaimRunEnd(const NetworkRunEndView& v);

/// @brief Run-scope counters of RaimWithinEpoch, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct RaimWithinEpochCounters {
    // Withhold-era release policy: era telemetry for both edges, per invariant #7.  All
    // zero unless one of the two knobs is armed (the era bookkeeping itself is
    // knob-gated to keep the events CSV byte-identical on off paths).
    long long n_wh_era_entries        = 0;  ///< SIGMA0/era_enter events
    long long n_wh_era_exits          = 0;  ///< SIGMA0/era_exit events (a run ending mid-era exits nowhere; max_len covers it)
    long long n_wh_era_max_len        = 0;  ///< longest era (withheld decides)
    long long n_wh_escalated_epochs   = 0;  ///< era epochs run at the escalated budget
    long long n_wh_escalate_landed    = 0;  ///< escalated epochs that landed σ̂₀ ≤ ceiling (took the accepted path)
    long long n_wh_escalate_exhausted = 0;  ///< escalated epochs that still withheld
    long long n_wh_free_erases        = 0;  ///< carry entries erased at the withhold branch (§3b)
    long long n_wh_perconst_exits     = 0;  ///< declared limitation: eras ended by the RTA-10 carve-out publishing (no escalation ran)
    long long n_pl_blk_refloat_rewhiten = 0;         ///< within-epoch RAIM refloats that re-whitened a block over its survivors (the re-coupling edge of invariant #7, counted)
    long long n_pl_blk_refloat_fail = 0;             ///< … whose re-whiten was non-SPD: the block's rows were dropped for that re-solve (a safe degradation: the refloated arcs re-float from data)

    // Run-end telemetry of WE-CI (network.raim_we_code_iono).  Standing invariant #4: an
    // armed screening whose counters never move on an eventful window is a miswire, not
    // a quiet run.
    std::size_t we_ci_code_trips = 0;   ///< epochs whose first-assess code-pool σ̂0 exceeded the code trigger
    std::size_t we_ci_iono_trips = 0;   ///< epochs whose first-assess iono-pool σ̂0 exceeded the iono trigger
    std::size_t we_ci_sta_excl   = 0;   ///< epoch-scoped station exclusions chosen from the code/iono pools
    std::size_t we_ci_sat_dw     = 0;   ///< network-wide satellite code-row de-weights chosen from the code pool
    double we_ci_s0_code_max = 0.0;     ///< max code-pool σ̂0 observed (trigger-calibration telemetry)
    double we_ci_s0_iono_max = 0.0;     ///< max iono-pool σ̂0 observed (trigger-calibration telemetry)

    // Run-end counters for the three response classes of the within-epoch tier (R0).
    // The withhold has its own run counter (n_sigma0_withheld); without these, the
    // refloats and exclusions would exist only as per-epoch T2 event rows, and a run
    // summary could not say how often the tier acted.  The clock move is the
    // impulse-channel meter of §5.07.2: how far the within-epoch re-solves moved a
    // satellite clock from the primary solve's value, with counters and identity per
    // invariant #7.
    long long we_refloat_sat_total = 0; ///< whole-sat within-epoch refloats settled over the run
    long long we_refloat_arc_total = 0; ///< isolated-arc within-epoch refloats settled over the run
    long long we_exclude_sta_total = 0; ///< epoch-scoped station exclusions settled over the run
    double      we_clk_move_max_m   = 0.0; ///< largest |final − primary| clock move (m)
    double      we_clk_move_max_tow = 0.0; ///< TOW of that move
    std::string we_clk_move_max_sat;       ///< satellite of that move
    // Failed within-epoch re-solves.  n_adapt counts landed
    // re-solves only, so a failed one appeared in no counter before this counter.  A
    // failure is undone to the last landed state; the undo drops, for the epoch, a
    // station it cannot re-add, and an undo that still cannot complete skips the
    // epoch.  The first failure's identity is kept (invariant #7).
    long long   we_resolve_failed   = 0;   ///< re-solves that failed and were undone to the last landed state
    long long   we_undo_sta_dropped = 0;   ///< station-epochs the undo could not re-add and left out of the final solve
    long long   we_restore_failed   = 0;   ///< undos that could not complete (epoch skipped)
    double      we_resolve_fail_tow = 0.0; ///< TOW of the first failed re-solve
    std::string we_resolve_fail_at;        ///< where it failed: a station name, or "shared system"
};

} // namespace GnssProcessor
