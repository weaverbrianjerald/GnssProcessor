#pragma once
/**
 * @file NetworkChannelAll.hpp
 * @brief Card SID phase 6b: `processing.channel_mode: all` made CORRECT on the
 *        network path, and the telemetry that says what it did
 *        (docs/refound/SID_signal_identity_2026-09-22.md).
 *
 * ### The mechanism, in one paragraph
 *
 * The widening in @c PppModel::selectUncombinedSignals is the ONE entry: every
 * ADMITTED code and phase attribute of every selected band becomes its own row at
 * every station.  It SUBSUMES card SCC (the second code channel) and card SID
 * phase 6a (the second phase channel) rather than being refused beside them: the
 * rows it adds are reported through the same two sets those cards fill
 * (@c PppObservation::companion_codes and @c second_phases), so the receiver
 * inter-signal code state, the two sigma scales, the Melbourne-Wubbena guard, the
 * cell telemetry and both edges of standing invariant 7 keep working unchanged.
 * Their allow-lists are ignored; their sigma scales apply to every non-canonical
 * code row and non-canonical phase row respectively.
 *
 * Three things the widening alone does not settle, and where they live:
 *
 *  - the RECEIVER inter-signal code bias of an extra code on ANY band, in
 *    @c StateInitializer::resolveReceiverBias (card SCC's state, its band-1
 *    restriction and its allow-list dropped under the widening);
 *  - the SATELLITE inter-signal correction of a band-2-or-above extra code, which
 *    card RF-0f's @c network.sat_isc_state frees where a station bridges both
 *    signals of that satellite.  Under the widening every station bridges, so the
 *    knob stops being inert and the run must say whether it is armed;
 *  - the row-level gap re-entry response, which keys on the tracker's ONE phase
 *    per band, in @c networkSlipDrainGapReentry.
 *
 * ### What this file owns
 *
 * The FAMILY CENSUS and the CROSS-CHANNEL EXPOSURE, both measure-only.  Nothing
 * here touches the solve, the wire or any graded trace; every function returns
 * immediately when the widening is not active.
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
     * Peaks rather than sums, because the question the card owes an answer to is
     * how wide a family GOT, not how many epochs it lived.
     */
    struct ChannelAllFamilyRow {
        long long sta_tracking{ 0 };   ///< peak stations reporting the signal in one epoch.
        long long sta_modelling{ 0 };  ///< peak stations carrying a row for it in one epoch.
        long long columns{ 0 };        ///< peak distinct satellites carrying it in one epoch.
        long long rows_canonical{ 0 }; ///< row-epochs where it was that station's latched signal.
        long long rows_extra{ 0 };     ///< row-epochs where it was an EXTRA channel.
    };

    /**
     * @brief Run-scope counters of the widening.  Lives in @c NetworkServiceCounters.
     *
     * Every class carries its own denominator so a zero can be read: a zero with a
     * zero denominator is dead wiring, and the run-end table says so in those words
     * (standing invariant 4).
     */
    struct NetworkChannelAllCounters {
        long long epochs{ 0 };                 ///< epochs the fold ran (denominator).
        /// "<constellation>|<signal>" -> the census row.
        std::map<std::string, ChannelAllFamilyRow> family;
        /// Cell-epochs (station, satellite, band) carrying MORE THAN ONE modelled
        /// PHASE.  This is the CROSS-CHANNEL EXPOSURE of R5: a screen keyed on
        /// "SAT|<band>" or on (station, satellite) acts on every channel of such a
        /// cell, so the count is the population those screens cannot separate.
        long long xchan_phase_cells{ 0 };
        long long xchan_code_cells{ 0 };       ///< the same for CODE attributes.
        long long xchan_cells_total{ 0 };      ///< (station, satellite, band) cell-epochs seen (denominator).
        /// Stations holding at least one receiver inter-signal code state, summed
        /// over epochs, and the peak count of distinct SIGNALS those states name.
        long long isc_station_epochs{ 0 };
        long long isc_signals_max{ 0 };
        /// Peak distinct band digits carrying an extra CODE channel in one epoch, and
        /// the peak over band digits at or above 2 — the population whose satellite
        /// inter-signal correction card RF-0f's knob decides.
        long long extra_code_bands_max{ 0 };
        long long extra_code_bands_ge2_max{ 0 };
    };

    /**
     * @brief PURE: is @p signal an EXTRA channel of its band at this station?
     *
     * The one definition the widening, the receiver side and the census all share: a
     * signal is extra exactly when its band's LATCHED signal of the same observation
     * type is known, is present in the same record, and is not this signal.  An
     * undecided latch answers false, which is the "change nothing" contract every
     * latch reader keeps.
     *
     * @param signal    The RINEX observation key under test, e.g. "C2X".
     * @param canonical The band's latched signal of the same type, or empty.
     * @param canonical_present True when @p canonical is in the same record.
     * @return True when the signal is a non-canonical channel of its band.
     */
    bool channelAllIsExtra(const std::string& signal, const std::string& canonical,
                           bool canonical_present);

    /**
     * @brief PURE: the extra channels the widening adds for one record.
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

    /**
     * @brief Fold one epoch into the census.  No-op when the widening is not active.
     *
     * Called from the driver after @c present[] is final and after the per-epoch
     * second-code and second-phase bookkeeping has republished its cell sets, which
     * is where @c PppObservation::companion_codes / @c second_phases and the
     * stations' own state maps are all current.
     *
     * @param ctx      Run context (the counters).
     * @param config_  The run's configuration.
     * @param stations The producer's stations.
     * @param modeled  Per-station MODELLED observations of this epoch.
     * @param present  Per-station PASS-A presence.
     */
    void networkChannelAllEpoch(NetworkRunContext& ctx, const GnssConfig& config_,
                                const std::vector<NetStation>& stations,
                                const std::vector<std::vector<PppObservation>>& modeled,
                                const std::vector<bool>& present);

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
    void networkChannelAllRunEnd(NetworkRunContext& ctx, const GnssConfig& config_,
                                 const std::shared_ptr<spdlog::logger>& log,
                                 long long gap_extra_channel);

}  // namespace GnssProcessor
