#pragma once
/**
 * @file ChannelAll.hpp
 * @brief `processing.channel_mode: all` on the network path and its telemetry,
 *        card SID phase 6b (docs/refound/SID_signal_identity_2026-09-22.md).
 *
 * ### Mechanism
 *
 * The widening in @c PppModel::selectUncombinedSignals is the single entry point:
 * every admitted code and phase attribute of every selected band becomes its own
 * row at every station.  It subsumes card SCC (the second code channel) and card
 * SID phase 6a (the second phase channel) instead of being refused beside them:
 * the rows it adds are reported through the same two sets those cards fill
 * (@c PppObservation::companion_codes and @c second_phases), so the receiver
 * inter-signal code state, the two sigma scales, the Melbourne-Wubbena guard, the
 * cell telemetry and both edges of standing invariant 7 work unchanged.  Their
 * allow-lists are ignored; their sigma scales apply to every non-canonical code
 * row and every non-canonical phase row respectively.
 *
 * Three questions the widening alone does not settle, and where they are handled:
 *
 *  - the receiver inter-signal code bias of an extra code on any band, in
 *    @c StateInitializer::resolveReceiverBias (card SCC's state, with its band-1
 *    restriction and its allow-list dropped under the widening);
 *  - the satellite inter-signal correction of an extra code on band 2 or above,
 *    which card RF-0f's @c network.sat_isc_state frees where a station bridges
 *    both signals of that satellite.  Under the widening every station bridges,
 *    so the knob acts on every widened station, and the run-end readout states
 *    whether it is armed;
 *  - the row-level gap re-entry response, which keys on the tracker's single phase
 *    per band, in @c networkSlipDrainGapReentry.
 *
 * ### Contents of this file
 *
 * The family census and the cross-channel exposure, both measure-only.  Nothing
 * here touches the solve, the wire or any graded trace.  The epoch fold returns
 * immediately unless the widening or a second constellation is active; the
 * run-end readout returns immediately unless the widening is active.
 */

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

    class GnssConfig;
    struct NetStation;
    struct NetworkRunContext;
    struct PppObservation;

    /**
     * @brief One (constellation, signal) row of the phase-6b family census.
     *
     * The station and column fields are peaks, not sums: they record how wide a
     * family became, not how many epochs it lasted.
     */
    struct ChannelAllFamilyRow {
        long long sta_tracking{ 0 };   ///< peak stations reporting the signal in one epoch.
        long long sta_modelling{ 0 };  ///< peak stations carrying a row for it in one epoch.
        long long columns{ 0 };        ///< peak distinct satellites carrying it in one epoch.
        long long rows_canonical{ 0 }; ///< row-epochs where it was that station's latched signal.
        long long rows_extra{ 0 };     ///< row-epochs where it was an extra channel.
    };

    /**
     * @brief Run-scope counters of the widening.  Lives in @c NetworkServiceCounters.
     *
     * Every class carries its own denominator so that a zero can be interpreted: a
     * zero with a zero denominator is dead wiring, and the run-end table reports it
     * as such (standing invariant 4).
     */
    struct NetworkChannelAllCounters {
        long long epochs{ 0 };                 ///< epochs the fold ran (denominator).
        /// "<constellation>|<signal>" -> the census row.
        std::map<std::string, ChannelAllFamilyRow> family;
        /// Cell-epochs (station, satellite, band) carrying more than one modelled
        /// phase: the cross-channel exposure of R5.  A screen keyed on "SAT|<band>"
        /// or on (station, satellite) acts on every channel of such a cell, so the
        /// count is the population those screens cannot separate.
        long long xchan_phase_cells{ 0 };
        long long xchan_code_cells{ 0 };       ///< the same for code attributes.
        long long xchan_cells_total{ 0 };      ///< (station, satellite, band) cell-epochs seen (denominator).
        /// Stations holding at least one receiver inter-signal code state, summed
        /// over epochs, and the peak count of distinct signals those states name.
        long long isc_station_epochs{ 0 };
        long long isc_signals_max{ 0 };
        /// Peak distinct band digits carrying an extra code channel in one epoch, and
        /// the same peak over band digits 2 and above: the population whose satellite
        /// inter-signal correction card RF-0f's knob decides.
        long long extra_code_bands_max{ 0 };
        long long extra_code_bands_ge2_max{ 0 };
    };

    /**
     * @brief Pure: is @p signal an extra channel of its band at this station?
     *
     * The single definition shared by the widening, the receiver side and the
     * census: a signal is extra when its band's latched signal of the same
     * observation type is known, is present in the same record, and is not this
     * signal.  An undecided latch answers false, which keeps the "change nothing"
     * contract every latch reader follows.
     *
     * @param signal    The RINEX observation key under test, e.g. "C2X".
     * @param canonical The band's latched signal of the same type, or empty.
     * @param canonical_present True when @p canonical is in the same record.
     * @return True when the signal is a non-canonical channel of its band.
     */
    bool channelAllIsExtra(const std::string& signal, const std::string& canonical,
                           bool canonical_present);

    /**
     * @brief Pure: the extra channels the widening adds for one record.
     *
     * Reproduces @c PppModel::selectUncombinedSignals' widening decision over one
     * observation's key set, so the unit suite can pin it without a model: every
     * admitted 'C' or 'L' key whose band digit is in @p bands and which is not that
     * band's canonical signal of the same type.
     *
     * @param keys      The record's observation keys.
     * @param bands     The band digits the run selected.
     * @param canon_code  band digit -> latched code, where known.
     * @param canon_phase band digit -> latched phase, where known.
     * @param allowed   Admitted keys (the network coverage gate), or nullptr for all.
     * @return The extra keys, ascending.
     */
    std::vector<std::string> channelAllExtraChannels(
        const std::vector<std::string>& keys,
        const std::vector<char>& bands,
        const std::map<char, std::string>& canon_code,
        const std::map<char, std::string>& canon_phase,
        const std::vector<std::string>* allowed);

    struct NetworkRun;              // network/driver/Run.hpp
    struct NetworkEpoch;            // network/driver/Epoch.hpp

    /**
     * @brief Fold one epoch into the census.  No-op unless the widening or a second
     *        constellation (@c networkMultiGnssActive, card GAL) is active.
     *
     * Called from the driver after @c present[] is final and after the per-epoch
     * second-code and second-phase bookkeeping has republished its cell sets; at
     * that point @c PppObservation::companion_codes / @c second_phases and the
     * stations' own state maps are all current.
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
     *   - @p ctx = run.ctx: Run context (the counters).
     *   - @p config_ = run.config: The run's configuration.
     *   - @p stations = run.setup.stations: The producer's stations.
     *   - @p modeled = ep.pass_a.modeled: Per-station modelled observations of this epoch.
     *   - @p present = ep.pass_a.present: Per-station PASS-A presence.
     */
    void networkChannelAllEpoch(NetworkRun& run, const NetworkEpoch& ep);

    /**
     * @brief Run-end readout: the family census and the exposure, or nothing when
     *        the widening is not active.
     *
     * @param ctx Run context (the counters).
     * @param config_ The run's configuration (the two sigma scales, the gate value).
     * @param log Producer logger.
     * @param gap_extra_channel @c SlipQcCounters::gap_reentry_extra_channel, which
     *        lives on the driver's slip channel rather than in the context.
     */
    void networkChannelAllRunEnd(const NetworkRunContext& ctx, const GnssConfig& config_,
                                 const std::shared_ptr<spdlog::logger>& log,
                                 long long gap_extra_channel);

    // ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

    struct NetworkRunEndView;

    /**
     * @brief Run-end report: the all-channel census (SID phase 6b).
     *
     * The table's row for the all-channel family census (SID phase 6b); the report is the
     * overload above.
     *
     * Gate: none of its own: the row calls the report above on every run, as the teardown
     * did, and that report applies its own.
     *
     * @param v The run at its end (network/runend/RunEnd.hpp).
     * @return 0 (this report does not set the exit code).
     */
    int networkChannelAllRunEnd(const NetworkRunEndView& v);

    // ---- End of the run-end reports ----

}  // namespace GnssProcessor
