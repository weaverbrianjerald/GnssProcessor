#pragma once
/**
 * @file NetworkDdPriorBuild.hpp
 * @brief The two per-epoch DD-prior build steps of the PPP-RTK network producer: the
 *        ensemble sum-zero phase-bias gauges, and the invariant DD-carry prior blocks
 *        (card 4) with the WL/UWL lane machinery (mw_wl / ewl / uwl / lane_carry).
 *        Relocated verbatim from Application::runNetwork (decomposition INC-9b).
 *
 * The run TU keeps both lambdas as thin `[&]` wrappers that forward to these free
 * functions, so the initial gauge/DD build and the within-epoch RAIM re-solve
 * (`resolve_dropping`) keep byte-identical capture semantics.  These functions own no
 * state.  Every WL run-state instance (ewl_lanes / wl_inject_epoch / mw_wl_layer /
 * mw_wl_f6 / ...) is a reference into the run TU.  Two inputs carry state across
 * epochs or passes and need care: carry_slipped_ambs, the driver's per-epoch slip
 * funnel, passed by reference, and lane_admit_pairs, read and written through
 * ctx.carry.  Every other argument keeps the name of the former runNetwork() capture.
 */

#include "filter/NetworkEstimator.hpp"          // NetworkEstimator
#include "filter/NetworkSbasis.hpp"             // NetworkSbasis::Plan
#include "filter/NetworkInvariantCarryDriver.hpp" // NetworkInvariantCarryDriver
#include "filter/NetworkMwWlLayer.hpp"          // NetworkMwWlLayer (+ ::Config)
#include "filter/NetworkRunTypes.hpp"           // NetStation / StationAssembly / CarryPrior
#include "filter/NetworkWlRunTypes.hpp"         // EwlLane / WlInjectPlan / LaneTraceEntry / WlCounters / F6Capture
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
 *        unless carry_invariant, so with it off the output is byte-identical.  All WL
 *        run-state arguments are in/out references into instances owned by the run TU.
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
    std::map<int, std::map<ArcKey, std::string>>& carry_arc_amb_key2,
    std::set<std::pair<int, std::string>>& carry_slipped_ambs,
    /// The raw-detector subset of this epoch's slip funnel (card 4, M1, D5 v4).  Every
    /// raw-observable inserter writes both sets (NetworkSlipQc via the PASS-A binding,
    /// networkSignalIntegrity, networkMeasurementAdmission, the deweight-release
    /// drain), so the complement `carry_slipped_ambs \ carry_slipped_raw` holds the
    /// estimation-side entries: the D5 innovation drain and the post-fit RAIM refloat
    /// drain (raim_refloat_pending).  The Iono-2 v3k erase-policy split uses the same
    /// idiom.  Read only under network.carry_invariant_innov_retap_only.
    const std::set<std::pair<int, std::string>>& carry_slipped_raw,
    const std::set<std::string>* carry_we_drop_sats,
    const std::map<int, std::set<std::string>>* carry_we_drop_arcs,
    int n_threads,
    std::vector<EwlLane>& ewl_lanes,
    const NetworkMwWlLayer::Config& ewl_cfg,
    NetworkMwWlLayer& mw_wl_layer,
    F6Capture& mw_wl_f6,
    NetworkMwWlLayer::Counters& mw_wl_epoch,
    std::vector<WlInjectPlan>& wl_inject_epoch,
    std::vector<LaneTraceEntry>& lane_trace_epoch,
    WlCounters& wl_epoch,
    std::map<char, WlCounters>& wl_cons_epoch,
    const std::function<bool(char, const std::string&)>& isUncarriedThirdBand,
    bool carry_innov_gate,
    bool mw_wl_on,
    bool ewl_on,
    bool uwl_on,
    bool uwl_cold_on,
    bool uwl_mid_on,
    bool uwl_dual_on,
    bool uwl_family_on,
    bool uwl_sib_gate_on,
    bool wl_inject_on,
    bool bridge_debias_on,
    bool ewl_carry_exclude,
    bool lane_on,
    bool lane_meas_on,
    bool lane_bridge_on,
    bool lane_fix_on,
    bool lane_parallel_components,
    bool mw_d5_dump,
    bool mw_lane_trace,
    bool mw_tagsets,
    bool mw_wl_f6_dump);

} // namespace GnssProcessor
