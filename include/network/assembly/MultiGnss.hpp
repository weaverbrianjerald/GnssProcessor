#pragma once
/**
 * @file MultiGnss.hpp
 * @brief Telemetry for a second constellation on the network path
 *        (docs/refound/GAL_gps_galileo_dual_2026-09-22.md).
 *
 * ### Contents of this file
 *
 * Measure-only counters and run-end lines, all silent unless a constellation other
 * than GPS is active on the network path: listed in @c processing.active_systems
 * and carrying a @c processing.signal_priorities list, the two conditions under
 * which the producer admits it.  A GPS-only run makes one flag test per call and
 * writes nothing, so its log, its traces and its wire are byte-identical.
 *
 *  - the inter-system bias: the stations holding a state per system, the
 *    station-epochs whose local block carried the column, and the station-epochs
 *    with no GPS row at all (clock and inter-system bias are then collinear, the
 *    local block is rank-deficient and the addStation PD test drops the station
 *    for that epoch; such epochs are counted, and the drops counted separately);
 *  - the clock S-basis per constellation: the realized pivot satellite at the
 *    first and the last emit, the pivot handovers between emitted products, and
 *    the declared dimension (one clock direction per constellation in pin mode);
 *  - the geometry-free datum counters of @c NetworkSbasis split per constellation
 *    (the global line in @c networkSbasisDatumRunEnd keeps its text);
 *  - the (constellation, signal) family census, which on a
 *    single-channel posture runs only because this telemetry is active;
 *  - the guarded temporal clock anchor's folds and trips per constellation.
 *
 * Nothing here touches the solve, the wire or any graded trace.
 */

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

    class GnssConfig;
    struct NetStation;
    struct NetworkRunContext;
    struct PppObservation;

    /**
     * @brief Run-scope counters of the second-constellation telemetry.  Lives in @c NetworkServiceCounters.
     *
     * Each map is keyed either by the constellation name the state manager uses for
     * the inter-system bias ("Galileo") or by the constellation letter the wire uses
     * ("E"), as each field states.  A zero with a zero denominator is dead wiring,
     * and the run-end line reports it as such.
     */
    struct NetworkMultiGnssCounters {
        long long epochs{ 0 };   ///< Epochs the per-epoch census ran (denominator).
        /// System name -> peak number of stations holding an inter-system bias state.
        std::map<std::string, long long> isb_stations_max;
        /// System name -> station-epochs whose modelled observations carried at least
        /// one row of that system, i.e. whose local block held the bias column.
        std::map<std::string, long long> isb_station_epochs;
        /// Constellation letter -> modelled satellite-epochs summed over stations.
        std::map<std::string, long long> sat_rows;
        /// Constellation letter -> peak distinct satellites modelled in one epoch.
        std::map<std::string, long long> sats_max;
        /// Station-epochs whose modelled observations held rows of a non-GPS system
        /// and no GPS row (receiver clock and inter-system bias collinear).
        long long nongps_only_station_epochs{ 0 };
        /// Of those, the station-epochs the addStation PD rejection dropped.
        long long nongps_only_pd_drops{ 0 };
        /// The stations that ever had such an epoch.
        std::set<std::string> nongps_only_stations;
        /// Emit epochs seen by @ref networkMultiGnssNotePivots.
        long long emits{ 0 };
        /// Constellation letter -> the clock pivot satellite at the first emit.
        std::map<std::string, std::string> first_emit_pivot;
        /// Constellation letter -> the clock pivot satellite at the last emit.
        std::map<std::string, std::string> last_emit_pivot;
        /// Constellation letter -> pivot changes between successive emitted products.
        std::map<std::string, long long> pivot_handovers;
        /// Declared clock-datum dimension (pivot count) over the emitted epochs.
        long long declared_min{ -1 };
        long long declared_max{ -1 };
        /// Constellation letter -> guarded temporal clock anchor folds in the anchored state.
        std::map<std::string, long long> anchor_folds_anchored;
        /// Constellation letter -> anchor folds in the loose state.
        std::map<std::string, long long> anchor_folds_loose;
        /// Constellation letter -> anchor decouplings (every cause), each an event.
        std::map<std::string, long long> anchor_trips;
    };

    /**
     * @brief Is a constellation other than GPS active on the network path?
     *
     * True when some entry of @c processing.active_systems other than "GPS" also
     * has a @c processing.signal_priorities list: the producer admits a
     * constellation only through its priority list and allocates the inter-system
     * bias only for active systems, so both halves are needed for the second
     * constellation to exist in the solve.
     *
     * @param config The loaded configuration.
     * @return @c true when the second-constellation telemetry is live.
     */
    bool networkMultiGnssActive(const GnssConfig& config);

    /**
     * @brief Pure: does a station's modelled epoch carry rows of a non-GPS system
     *        and no GPS row at all?
     *
     * @param modeled The station's modelled observations of the epoch.
     * @return @c true when the station's clock and inter-system bias are collinear.
     */
    bool networkStationNonGpsOnly(const std::vector<PppObservation>& modeled);

    struct NetworkRun;              // network/driver/Run.hpp
    struct NetworkEpoch;            // network/driver/Epoch.hpp

    /**
     * @brief Per-epoch census: inter-system bias columns, satellites per
     *        constellation, non-GPS-only station-epochs.
     *
     * Called once per epoch after the solve, beside the phase-6b census, with the
     * final @p present vector (a station the PD test dropped is already false).
     * Returns on its first line unless @ref networkMultiGnssActive.
     *
     * Epoch form (generated by scripts/nom_ep1_cut.py): the run and the epoch
     * replace the explicit parameters; the definition binds each former parameter in its preamble,
     * under its former name.
     *
     * @param run The run (network/driver/Run.hpp). Written through non-const bindings: run.ctx (ctx).
     *            Read: run.config (config_), run.setup.stations (stations).
     * @param ep The epoch (network/driver/Epoch.hpp), read only. Bindings: ep.pass_a.modeled
     *           (modeled), ep.pass_a.present (present).
     *
     * The former parameters, by binding:
     *   - @p ctx = run.ctx: Run context (counters written).
     *   - @p config_ = run.config: Configuration.
     *   - @p stations = run.setup.stations: The station roster.
     *   - @p modeled = ep.pass_a.modeled: Per-station modelled observations of the epoch.
     *   - @p present = ep.pass_a.present: Per-station presence after the consume loop.
     */
    void networkMultiGnssEpoch(NetworkRun& run, const NetworkEpoch& ep);

    /**
     * @brief Emit-site record of the realized clock pivots per constellation.
     *
     * Called at the single site where the handover baseline advances
     * (NetworkSolPrep, immediately before @c prev_pivot is replaced), so a handover
     * is counted between emitted products as the consumer sees it, never on a
     * withheld epoch.  Returns on its first line unless @ref networkMultiGnssActive.
     *
     * @param ctx         Run context (counters written).
     * @param config_     Configuration.
     * @param clock_pivot This epoch's realized pivots (constellation -> satellite).
     * @param last_pivot  The pivots of the last emitted product that contained each
     *                    constellation, before this epoch's merge.
     */
    void networkMultiGnssNotePivots(NetworkRunContext& ctx, const GnssConfig& config_,
                                    const std::map<std::string, std::string>& clock_pivot,
                                    const std::map<std::string, std::string>& last_pivot);

    /**
     * @brief Run-end lines of the second-constellation telemetry.  Silent unless @ref networkMultiGnssActive.
     *
     * @param ctx     Run context (counters read).
     * @param config_ Configuration.
     * @param log     The run logger.
     */
    void networkMultiGnssRunEnd(const NetworkRunContext& ctx, const GnssConfig& config_,
                                const std::shared_ptr<spdlog::logger>& log);

    /**
     * @brief The manifest fragment of the second-constellation telemetry: `"inter_system_bias": {...}`.
     *
     * @param ctx     Run context (counters read).
     * @param config_ Configuration.
     * @return A JSON member (key and object, no surrounding comma), or an empty
     *         string when @ref networkMultiGnssActive is false, so a GPS-only
     *         manifest is byte-identical.
     */
    std::string networkMultiGnssManifestJson(const NetworkRunContext& ctx,
                                             const GnssConfig& config_);

    // ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

    struct NetworkRunEndView;

    /**
     * @brief Run-end report: the second constellation (GAL).
     *
     * The table's row for the second-constellation lines; the report is the
     * overload above.
     *
     * Gate: none of its own: the row calls the report above on every run, as the teardown
     * did, and that report applies its own.
     *
     * @param v The run at its end (network/runend/RunEnd.hpp).
     * @return 0 (this report does not set the exit code).
     */
    int networkMultiGnssRunEnd(const NetworkRunEndView& v);

    // ---- End of the run-end reports ----

}  // namespace GnssProcessor
