#pragma once
/**
 * @file NetworkDdPriorBuild.hpp
 * @brief The two per-epoch DD-prior build steps of the PPP-RTK network producer: the
 *        ensemble sum-zero phase-bias gauges, and the invariant DD-carry prior blocks
 *        (card 4) with the WL/UWL lane machinery (mw_wl / ewl / uwl).
 *        Relocated verbatim from Application::runNetwork (decomposition INC-9b).
 *
 * The run TU keeps both lambdas as thin `[&]` wrappers that forward to these free
 * functions, so the initial gauge/DD build and the within-epoch RAIM re-solve
 * (`resolve_dropping`) keep byte-identical capture semantics.  These functions own no
 * state.  Every WL run-state instance (ewl_lanes / wl_inject_epoch / wl_epoch /
 * lane_trace_epoch / ...) is a member of the driver's @ref NetworkLaneRun, and the lane switches
 * are read from NetworkKnobs (card NOM step TD1a).  One input carries state across
 * epochs or passes and needs care: carry_slipped_ambs, the driver's per-epoch slip
 * funnel, passed by reference.  Every other argument keeps the name of the former
 * runNetwork() capture.
 */

#include "filter/NetworkEstimator.hpp"          // NetworkEstimator
#include "filter/NetworkSbasis.hpp"             // NetworkSbasis::Plan
#include "filter/NetworkInvariantCarryDriver.hpp" // NetworkInvariantCarryDriver
#include "filter/NetworkMwWlLayer.hpp"          // NetworkMwWlLayer (+ ::Config)
#include "filter/NetworkRunTypes.hpp"           // NetStation / StationAssembly / CarryPrior
#include "filter/NetworkWlRunTypes.hpp"         // EwlLane / WlInjectPlan / LaneTraceEntry / WlCounters
#include "filter/NetworkRunGroups.hpp"          // NetworkLaneRun (card NOM step TD1a)
#include "filter/NetworkZStore.hpp"             // ArcKey (carry_arc_amb_key maps)
#include "rinex/MultiRinexSync.hpp"             // SyncedEpoch

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;
struct NetworkRunTraces;

/**
 * @brief Fold the ensemble Sum-Zero phase-bias gauges for this epoch into @p est.
 *        No-op unless @p phase_datum_ensemble.  The body is documented in the .cpp.
 */
void networkBuildEnsembleGauges(NetworkEstimator& est,
                                const std::vector<NetStation>& stations,
                                const std::vector<int>& est_index_of,
                                const std::vector<StationAssembly>& asm_res,
                                const std::set<std::string>& shared_phase_keys,
                                const std::vector<CarryPrior>& prev_carry,
                                bool phase_datum_ensemble,
                                bool phase_hold_on);


/**
 * @brief Build this epoch's invariant DD-carry prior blocks (card 4) and the WL/UWL
 *        lane rows, and install them via NetworkEstimator::setDdPriors.  Returns early
 *        unless carry_invariant, so with it off the output is byte-identical.
 * @param lane_run The driver's lane machinery; its per-build members are rewritten on
 *                 every build (in/out).
 * @param isUncarriedThirdBand Whether a (constellation, signal) is the EWL pair's
 *                 uncarried band, left out of the carry while the EWL lane is armed.
 *
 * The lane wavelengths come from the identity layer (@c SignalId::wavelengthM, card
 * NOM step N0b); GLONASS never enters the carry, so no FDMA slot is needed.
 */
void networkBuildCarryDdPriors(
    NetworkRunContext& ctx,
    const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkRunTraces& traces,
    NetworkInvariantCarryDriver& carry_driver,
    NetworkEstimator& est,
    const std::vector<NetStation>& stations,
    const std::vector<StationAssembly>& asm_res,
    const std::vector<int>& est_index_of,
    const std::vector<bool>& present,
    const std::set<std::string>& shared_phase_keys,
    const SyncedEpoch& se,
    std::map<int, std::map<ArcKey, std::string>>& carry_arc_amb_key,
    std::set<std::pair<int, std::string>>& carry_slipped_ambs,
    /// The raw-detector subset of this epoch's slip funnel (card 4, M1, D5 v4).  Every
    /// raw-observable inserter writes both sets (NetworkSlipQc via the PASS-A binding,
    /// networkSignalIntegrity, networkMeasurementAdmission, the deweight-release
    /// drain), so the complement `carry_slipped_ambs \ carry_slipped_raw` holds the
    /// estimation-side entries: the D5 innovation drain and the post-fit RAIM refloat
    /// drain (raim_refloat_pending).  The Iono-2 v3k erase-policy split uses the same
    /// idiom.  Read only under network.carry_invariant_innov_retap_only.
    const std::set<std::pair<int, std::string>>& carry_slipped_raw,
    /// The within-epoch RAIM tier's culprit sets (card C3c §3; card WCF): every carried
    /// ambiguity of a culprit satellite or arc is treated as slipped.  The epoch
    /// workspace's member (@ref EpochWorkspace::we_culprits),
    /// naming something only on a T2 re-solve's rebuild; each rebuild that reads a culprit
    /// is counted, with what it withheld, in @c NetworkServiceCounters::we_culprit_withhold.
    const WithinEpochCulprits& we_culprits,
    int n_threads,
    NetworkLaneRun& lane_run,
    const std::function<bool(char, const std::string&)>& isUncarriedThirdBand);

} // namespace GnssProcessor
