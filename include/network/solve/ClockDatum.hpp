#pragma once
/**
 * @file ClockDatum.hpp
 * @brief Stage 3 of the network solve: the clock datum.  The declaration
 *        the election reads, the HLT-aware sticky election (NetworkSbasis::buildSharedDatum),
 *        the constraint-realized clock rows and the pivot-handover verdict.
 */

#include "network/solve/Sbasis.hpp"     // SbasisDeclaration
#include <map>
#include <string>
#include "network/driver/RunTypes.hpp"   // EpochAction

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;
struct NetworkRun;
struct NetworkEpoch;
struct SolveDatum;   // network/solve/SolveGlue.hpp
struct GnssTime;
struct NetworkSetupResult;

/**
 * @brief The clock datum's declaration the epoch's election reads
 *        (@ref NetworkSbasis::buildSharedDatum): the reference receiver, the self-described
 *        code-bias and phase-bias frames, and the clock gauge (broadcast-anchored,
 *        sum-to-zero, or a configured pivot satellite).
 *
 * One implementation, read by the election in @ref networkClockDatum and by the handover
 * preview before PASS B (@ref networkHandoverStransformBeforeRows), so the
 * two elect from the same declaration.
 * @param ctx              the run context (the knobs).
 * @param config_          the configuration (the network block).
 * @param clk_anchor_sigma the resolved broadcast anchor sigma (m).
 * @return the declaration.
 */
SbasisDeclaration networkClockDatumDeclaration(const NetworkRunContext& ctx,
                                               const GnssConfig& config_,
                                               double clk_anchor_sigma);

/**
 * @brief The coverage election of one epoch, bound for @ref NetworkSbasis::buildSharedDatum
 *        (@c network.pivot_election).
 *
 * Built by each caller of the election at the top of its epoch: the ranker is bound to the
 * epoch time and to the run's predictor (@c setup.pivot_coverage), and the standby map is
 * the constellations' @c elected_pivot rows as they stand before the epoch. Two callers in
 * one epoch (the preview before PASS B and the solve glue) therefore elect from the same
 * inputs, and the predictor's memo gives them the same ranking at the cost of one.
 * Not copyable: the election points into the standby map of this object.
 */
class NetworkPivotElectionEpoch {
public:
    /**
     * @param ctx   The run context (the knob, the entity tables).
     * @param setup The setup result (the predictor).
     * @param t     The epoch time.
     */
    NetworkPivotElectionEpoch(const NetworkRunContext& ctx, const NetworkSetupResult& setup,
                              const GnssTime& t);
    NetworkPivotElectionEpoch(const NetworkPivotElectionEpoch&) = delete;
    NetworkPivotElectionEpoch& operator=(const NetworkPivotElectionEpoch&) = delete;

    /// @return The election for @ref NetworkSbasis::buildSharedDatum, or null when the knob is off.
    const NetworkSbasis::PivotElection* get() const { return active_ ? &election_ : nullptr; }

private:
    bool                               active_;
    std::map<std::string, std::string> standby_;
    NetworkSbasis::PivotElection       election_;
};

/**
 * @brief Stage 3: elects the clock datum (HLT-aware, sticky), folds the constraint-realized
 *        clock rows and decides the pivot-handover verdict.
 * @param run   The run: the estimator is folded; ctx counters (hlt_*) are written.
 * @param ep    The epoch, read: se, columns (registered_clock_sats).
 * @param datum [out] The plan, datum_changed and changed_consts (SolveDatum).
 * @return SkipEpoch when the election throws (exit DatumFailed), else Proceed.
 */
EpochAction networkClockDatum(NetworkRun& run, const NetworkEpoch& ep, SolveDatum& datum);

/// @brief Run-scope counters of ClockDatum, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct ClockDatumCounters {
    // Broadcast health on the network path.  All zero unless network.sat_health.
    long long hlt_unhealthy_registered = 0; ///< (sat,epoch) registered clock satellites the broadcast flags unhealthy (processed in "pivot" mode, absent in "exclude")
    long long hlt_pivot_skips = 0;          ///< (constellation,epoch) pivot picks that passed over an unhealthy satellite
    long long hlt_pivot_fallbacks = 0;      ///< (constellation,epoch) with no healthy registered satellite, where the lexical first stood
    // The coverage election on the network path.  All zero unless network.pivot_election.
    long long piv_ranked = 0;               ///< (constellation,epoch) picks a fresh election by the coverage rule made
    long long piv_rank_failures = 0;        ///< (constellation,epoch) elections where no candidate could be ranked and the first candidate stood
    long long piv_standby_kept = 0;         ///< (constellation,epoch) picks that kept an earlier election's satellite that no emitted product had adopted yet
};

} // namespace GnssProcessor
