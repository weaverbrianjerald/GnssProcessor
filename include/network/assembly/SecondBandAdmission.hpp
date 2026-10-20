#pragma once
/**
 * @file SecondBandAdmission.hpp
 * @brief Admit a constellation's rows at a station only while the station holds the
 *        constellation on two modelled bands.
 *
 * ### Problem addressed
 *
 * A station that tracks a constellation other than the time reference on one band only
 * (BeiDou B1I without B3I, for example) leaves three directions of its local block
 * collinear: the constellation's inter-system bias, the slant ionosphere of its lines of
 * sight and their ambiguities. One band gives a code row and a phase row per satellite
 * against two free unknowns (the slant delay and the ambiguity), so a common shift of the
 * bias is absorbed by the unknowns of every satellite and nothing in the block separates
 * them. The addStation positive-definiteness guard then rejects the whole station at every
 * epoch, and its other constellations (the GPS and Galileo rows that are perfectly well
 * posed) leave the solve with it.
 *
 * ### The rule
 *
 * A band is modelled for a satellite when the epoch carries, for that satellite, an
 * admitted phase signal and an admitted code signal on it. Admitted means a signal of the
 * constellation's priority list (@c processing.signal_priorities) that the network
 * coverage gate (@c PppModel::signalAllowlist) has not excluded: the candidates the
 * canonical latch draws from. Bands are physical bands (@c classifySignal), not RINEX
 * digits, so two signals of one carrier count once.
 *
 * Under @ref SecondBandScope::Station (the default) a constellation is admitted at a
 * station while at least @c network.second_band_min_sats of its satellites are on two
 * modelled bands there in the epoch. Every satellite of the constellation is then
 * admitted, on one band or two. Under @ref SecondBandScope::Satellite each satellite is
 * admitted on its own while it is on two modelled bands. Either way the unit that fails is
 * withheld whole (every record of the constellation at the station, or of the satellite),
 * and the station keeps its other constellations.
 *
 * A record on no modelled band at all (the station spells BeiDou B1I as C1I and L1I, which the
 * priority list does not name) forms no row in the producer whatever the rule does, so it is
 * left alone and counted apart: under station scope a constellation none of whose records
 * carries a modelled band is not screened, and under satellite scope such a record is not.
 *
 * The time reference constellation is outside the rule: it carries no inter-system bias,
 * its clock is the receiver clock, and withholding it would remove the station's clock
 * reference. A constellation with no priority list is outside the rule as well (the
 * producer models none of its signals).
 *
 * ### Where it acts, and why there
 *
 * Immediately after the canonical-pair admission, on a per-epoch mutable copy of the
 * @c SyncedEpoch, upstream of the roster, the models, the slip detectors and assembly. A
 * withheld record is therefore absent for everything downstream exactly as if the RINEX
 * record were missing: nothing has to be pruned by hand, and the row-level gap
 * re-entry response (wired for every data gap) reads the return of a unit as a re-entry,
 * not as the continuation of an arc. The station's canonical latch for the constellation
 * is also decided only when the constellation is first admitted, so a station that gains
 * its second band later latches both bands at that moment instead of keeping a one-band
 * latch for the run.
 *
 * The held-out closed-loop consumer is not touched: the seam walks the producer roster,
 * which never contains it.
 *
 * ### Both edges are events
 *
 * A unit entering the withheld state (decoupling edge) and leaving it (re-coupling edge)
 * each write one @c ADMISSION / @c second_band event row and one log line naming the
 * station and the constellation (and the satellite under satellite scope), on the
 * transition only, never once per withheld epoch. The run-end report counts both edges,
 * the withheld records and the stations affected; a rule that withheld nothing is reported
 * as armed and inert.
 *
 * ### Byte-identity when off
 *
 * With @c network.second_band_admission false the seam returns a reference to the
 * caller's own epoch on its first statement (no copy, no walk, no counter, no entity row),
 * so the products, traces and wire are unchanged. The mechanism is a forward-only
 * admission policy and is not fingerprinted.
 */

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "common/GnssTypes.hpp"      // System
#include "common/SignalBand.hpp"     // FrequencyBand

namespace spdlog { class logger; }

namespace GnssProcessor {

    struct NetworkRun;               // network/driver/Run.hpp
    struct NetworkRunContext;        // network/state/RunContext.hpp
    struct NetworkRunEndView;        // network/runend/RunEnd.hpp
    struct RinexObservation;         // rinex/RinexParser.hpp
    struct SyncedEpoch;              // rinex/MultiRinexSync.hpp

    /**
     * @brief The signals of one constellation the producer can model, with their bands.
     *
     * Built once per epoch from the constellation's priority list and the network coverage
     * gate (see @ref secondBandCandidates). A signal outside both maps is never modelled.
     */
    struct SecondBandCandidates {
        std::map<std::string, FrequencyBand> code;    ///< Admitted code signal ("C2I") to its band.
        std::map<std::string, FrequencyBand> phase;   ///< Admitted phase signal ("L2I") to its band.
    };

    /**
     * @brief The modelled-signal candidates of one constellation.
     *
     * Pure. Keeps the code and phase signals of @p priority that @p admitted lets through
     * and whose band @c classifySignal knows; signals of other observation types (Doppler,
     * signal strength) and signals whose band is unknown are dropped.
     *
     * @param sys      The constellation.
     * @param priority Its priority list (@c processing.signal_priorities).
     * @param admitted The network coverage gate's admitted signals for the constellation,
     *                 or null when every signal is admitted.
     * @return The candidates.
     */
    SecondBandCandidates secondBandCandidates(System sys,
                                              const std::vector<std::string>& priority,
                                              const std::set<std::string>* admitted);

    /**
     * @brief The number of modelled bands of one satellite record.
     *
     * Pure. A band counts when the record carries, among @p cand, a code signal and a
     * phase signal of that band. Two signals of one band count once.
     *
     * @param obs  One satellite's record of the epoch.
     * @param cand The constellation's candidates.
     * @return The number of distinct bands with both a candidate code and a candidate phase.
     */
    int secondBandCount(const RinexObservation& obs, const SecondBandCandidates& cand);

    /// @brief The edges one step of a unit's state machine may have crossed.
    struct SecondBandStep {
        bool      withhold_edge{ false };   ///< The unit entered the withheld state at this epoch.
        bool      readmit_edge{ false };    ///< The unit left the withheld state at this epoch.
        long long gap_ep{ 0 };              ///< On a re-admission: processed epochs the unit was withheld.
    };

    /**
     * @brief Advance one unit's withheld memory by one epoch.
     *
     * Pure apart from @p since. The unit (a constellation name under station scope, a
     * satellite under satellite scope) is in @p since exactly while it is withheld; the
     * value is the processed-epoch index at which the withhold began.
     *
     * @param since     [in/out] The station's withheld units to their first epoch.
     * @param unit      The unit tested this epoch.
     * @param admitted  The verdict of this epoch.
     * @param epoch_idx The seam's processed-epoch index.
     * @return The edges crossed.
     */
    SecondBandStep secondBandStep(std::map<std::string, long long>& since,
                                  const std::string& unit, bool admitted,
                                  long long epoch_idx);

    /**
     * @brief Run-scope counters of the rule: the on-path telemetry.
     *
     * Every disposition is counted against a denominator, so a clean log is not mistaken
     * for evidence that the rule acted. Untouched unless @c network.second_band_admission.
     */
    struct NetworkSecondBandCounters {
        long long epochs{ 0 };               ///< Epochs the seam ran (denominator).
        long long records_screened{ 0 };     ///< Records screened (they carry at least one modelled band), summed over stations and epochs.
        long long records_unmodelled{ 0 };   ///< Records of screened constellations left alone because none of the unit's records carries a modelled band.
        long long units_screened{ 0 };       ///< Units tested (station-constellations, or satellites under satellite scope).
        long long units_withheld{ 0 };       ///< Unit-epochs withheld.
        long long records_withheld{ 0 };     ///< Records removed from the epoch.
        long long withhold_edges{ 0 };       ///< Decoupling edges.
        long long readmit_edges{ 0 };        ///< Re-coupling edges.
        long long max_gap_ep{ 0 };           ///< Longest withhold that ended in a re-admission.
        long long epochs_copied{ 0 };        ///< Epochs that took the scratch copy (something was withheld).
        long long log_lines_dropped{ 0 };    ///< Edge log lines not written once the line limit was reached (the event rows are not limited).
        /// Constellation name to the stations that had a unit of it withheld at least once.
        std::map<std::string, std::set<std::string>> affected;
        /// Constellation name to the records withheld.
        std::map<std::string, long long> withheld_by_cons;
    };

    /**
     * @brief Apply the rule to this epoch's observations.
     *
     * @param run      The run (network/driver/Run.hpp). Read: run.config, run.log,
     *                 run.setup.stations, run.ctx.knobs. Written: run.ctx (counters and the
     *                 stations table of the entities), run.traces (event rows).
     * @param se_in    The epoch as it reaches the seam (after the canonical-pair admission).
     * @param scratch  Caller-owned buffer that receives the pruned copy.
     * @return @p se_in itself when the rule is off or withheld nothing this epoch (no
     *         copy is taken), otherwise a reference to @p scratch with the withheld
     *         records erased.
     *
     * @note @p scratch must outlive the returned reference for the whole epoch body;
     *       declare it immediately before the call.
     */
    const SyncedEpoch& networkSecondBandAdmission(NetworkRun& run, const SyncedEpoch& se_in,
                                                  SyncedEpoch& scratch);

    /**
     * @brief The run-end lines of the second-band admission.
     *
     * Silent unless @c network.second_band_admission is on. One summary line (screened
     * denominators, withheld records, both edges, the armed-and-inert clause) and one line
     * per constellation that had a unit withheld, naming the stations.
     *
     * @param ctx Run context (knobs and counters read).
     * @param log The run logger.
     */
    void networkSecondBandAdmissionReport(const NetworkRunContext& ctx,
                                          const std::shared_ptr<spdlog::logger>& log);

    /**
     * @brief Run-end report: the second-band admission.
     *
     * The table's row for the second-band admission; the report is the overload above.
     *
     * Gate: the report above applies its own (silent unless the rule is on).
     *
     * @param v The run at its end (network/runend/RunEnd.hpp).
     * @return 0 (this report does not set the exit code).
     */
    int networkSecondBandAdmissionRunEnd(const NetworkRunEndView& v);

    /// @brief Run-scope counters of SecondBandAdmission, a base of NetworkServiceCounters (network/state/RunContext.hpp).
    struct SecondBandAdmissionCounters {
        /// The on-path readout of the rule: zero withheld records on an armed run is
        /// dead wiring or a property of the data, and the run-end line says which.
        NetworkSecondBandCounters sba;
    };

} // namespace GnssProcessor
