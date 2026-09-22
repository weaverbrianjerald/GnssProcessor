#pragma once
/**
 * @file SignalRoster.hpp
 * @brief Card SID phase 1: the per-station SIGNAL ROSTER and the observatory that
 *        measures every disagreement between it and the sites that decide the
 *        same question today (@c network.signal_roster, default OFF).
 *
 * ### The defect this observes
 *
 * The audit of 2026-09-22 (`runs/_msc/sid_audit_report_2026-09-22.md`, Part A)
 * found 128 sites in 38 files that decide WHICH signal stands for a band, under at
 * least nine different rules, with at least six independent implementations of
 * "the two lowest distinct bands" and different tie breaks.  Nothing compares
 * them.  Measured on the field baseline (`runs/_rf0hv8_field6`, Part D), the
 * arc-level slip tracker's band signal differs from the carried one at 46
 * station-band cells of 44 of 58 stations, and 0 of 31 band-1 re-entries at those
 * cells were answered against 55 of 67 elsewhere.
 *
 * ### What the roster is
 *
 * ONE object per epoch, owned by @c NetworkRunContext, answering for every
 * (station, constellation, band digit):
 *
 *  - TRACKED: the phase and code signals the receiver reported this epoch;
 *  - MODELLED: the phase and code the estimator carries, taken from the station's
 *    canonical latch (@c PppModel::exportCanonicalSignals) and never re-derived;
 *  - COMPANION: the band-1 codes that ride as card SCC companions;
 *  - EXCLUDED: the signals that exist and are deliberately not used, with a reason;
 *  - DATUM: the signal the S-basis pins.  EMPTY in phase 1: the IF pin pair is
 *    elected inside the datum seam, which runs far downstream of the roster's
 *    arrival site, so filling it needs the reader edit phase 2 makes.
 *
 * ### Where it is decided
 *
 * ONE call, @ref networkBuildSignalRoster, in @c ApplicationNetwork.cpp
 * immediately after the canonical-pair-admission seam returns the epoch and before
 * the reduced-RHS recorder.  That site carries the driver's strongest upstream
 * contract: @c computeModels, the T0 detectors, @c networkComputeRawObservables,
 * assembly, the RAIM pools and the traces all derive from the @c RinexObservation
 * it hands on.  Everything the roster needs is available there.
 *
 * ### Phase 1 is an OBSERVATORY
 *
 * Nothing reads the roster or the ledger except the disagreement counters and the
 * trace.  The solve, the wire and every graded trace are byte-identical with the
 * knob armed and disarmed (standing invariant 6 in both states), and the run-end
 * table is the ON-path telemetry (standing invariant 4).
 */

#include "filter/ArcLedger.hpp"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

    class GnssConfig;
    class NetworkRunTraces;
    struct NetStation;               // filter/NetworkRunTypes.hpp
    struct NetworkRunContext;
    struct SyncedEpoch;              // rinex/MultiRinexSync.hpp
    struct RawSlipEpoch;             // slip/NetworkRawObservables.hpp
    struct NetworkSolutionEpoch;     // products/NetworkProductEmitter.hpp

    /**
     * @brief The role a signal holds at one station, one constellation, one band.
     *
     * A reader asks for a ROLE rather than re-deriving a rule.  Phase 1 assigns
     * them and counts; phases 2 to 6 migrate the readers one at a time.
     */
    enum class SignalRole {
        Tracked,    ///< reported by the receiver this epoch.
        Modelled,   ///< the estimator's choice for the band (the latch).
        Datum,      ///< pinned by the S-basis (unfilled in phase 1).
        Companion,  ///< a card SCC second code channel.
        Excluded    ///< present and deliberately unused, with a reason.
    };

    /// @brief Human-readable name of a role, for traces and log text.
    const char* signalRoleName(SignalRole r);

    /**
     * @brief One (station, constellation, band digit) cell of the roster.
     *
     * @c modelled_extra is the phase-6 set of additional modelled phases per band
     * (@c processing.channel_mode "all" on the network path) and stays EMPTY in
     * phase 1, where the estimator carries exactly one phase per band.
     */
    struct BandRoster {
        std::vector<std::string> tracked_phase;   ///< raw phase keys reported this epoch, ascending.
        std::vector<std::string> tracked_code;    ///< raw code keys reported this epoch, ascending.
        std::string modelled_phase;               ///< the latched phase of this band, or empty.
        std::string modelled_code;                ///< the latched code of this band, or empty.
        std::vector<std::string> modelled_extra;  ///< phase 6 only; empty in phase 1.
        std::vector<std::string> companion_code;  ///< card SCC companions, in configured order.
        std::string datum_phase;                  ///< the S-basis pinned phase; empty in phase 1.
        std::string datum_code;                   ///< the S-basis pinned code; empty in phase 1.
        /// (signal, reason) for every tracked signal that is neither modelled nor a
        /// companion.  Reasons: "allowlist", "not_latched", "no_latch".
        std::vector<std::pair<std::string, const char*>> excluded;
    };

    /// @brief One station's roster: constellation -> band digit -> cell.
    using StationRoster = std::map<std::string, std::map<char, BandRoster>>;

    /**
     * @brief One flat row of a station's canonical latch, as the roster reads it.
     *
     * Deliberately not @c PppModel::CanonicalSignal, so the pure builder below can
     * be exercised without constructing a model.
     */
    struct RosterLatchRow {
        char        obs_type{ 'C' };  ///< 'C' (code) or 'L' (phase).
        std::string signal;           ///< the latched RINEX code, e.g. "L1W".
    };

    /**
     * @brief Build one station's roster cells for one constellation.  PURE.
     *
     * The rule is REPRODUCED, never re-derived: @c modelled_phase and
     * @c modelled_code are the latch's own rows, and the companion walk is the
     * card SCC rule (@c NetworkSecondCodeScope.cpp) in its configured order.
     *
     * @param tracked           Raw observation keys this epoch; anything that is
     *                          not a three-or-more character 'C' or 'L' key is
     *                          ignored.  Order is irrelevant (the output is sorted).
     * @param latch             This constellation's latch rows, in any order.
     * @param companion_allow   @c network.second_code_signals, in configured order.
     * @param companions_armed  @c network.second_code_channel.
     * @param allowlist         The network signal allow list for this constellation,
     *                          or nullptr when every tracked code is admitted.
     * @return band digit -> cell, for every band digit that has a tracked signal or
     *         a latched one.
     */
    std::map<char, BandRoster> signalRosterBuildCells(
        const std::vector<std::string>& tracked,
        const std::vector<RosterLatchRow>& latch,
        const std::vector<std::string>& companion_allow,
        bool companions_armed,
        const std::set<std::string>* allowlist);

    /**
     * @brief The arc-level tracker's own band pick, reproduced for comparison.
     *
     * @c NetworkRawObservables.cpp forms its primary pair from the phases that have
     * a matching code, sorted on the BAND DIGIT ALONE, and takes the front plus the
     * first entry of a distinct band.  The comparator is not a strict weak ordering
     * over same-band signals and @c std::sort is not stable, so the same-band order
     * is formally unspecified; the toolchain gives lexical order, which is what this
     * helper states explicitly.  Exposed so the unit suite can pin it.
     *
     * @param tracked Raw observation keys this epoch.
     * @return band digit -> the tracker's phase for that band (lexically lowest
     *         phase of the band that also has its matching code).
     */
    std::map<char, std::string> signalRosterTrackerPicks(
        const std::vector<std::string>& tracked);

    /**
     * @brief Run-scope state of the observatory: the epoch product plus the caches
     *        and census accumulators the run-end table is read off.
     *
     * Lives in @c NetworkRunContext (@c ctx.roster), which is where carried state
     * belongs on this path.  Deliberately NOT snapshotted and NOT part of the
     * warm-resume fingerprint: nothing here is read by the solve, so a resumed run
     * re-seats every cell at its first post-resume epoch.
     */
    struct SignalRoster {
        bool      armed{ false };   ///< network.signal_roster.
        long long epochs{ 0 };      ///< seam invocations: THE processed-epoch clock of this card.

        /// station index -> this epoch's cells.  Rebuilt every epoch.
        std::vector<StationRoster> by_station;
        /// station index -> constellation -> its latch rows, cached once decided.
        std::vector<std::map<std::string, std::vector<RosterLatchRow>>> latch_cache;
        /// station index -> constellations whose count-against-priority comparison ran.
        std::vector<std::set<std::string>> rule_done;
        /// Stations at which the count rule and the priority rule pick differently on
        /// ANY (band, obs type).
        std::set<std::string> rule_differs;
        /// Stations at which the two rules pick a different BAND-1 PHASE, which is the
        /// population card CSL names and the audit measured at six stations.
        std::set<std::string> rule_differs_b1;
        /// Stations at which the comparison ran at least once.  The complement of this
        /// set is the unreachable population; it is tracked positively because the
        /// roster sees every constellation in the RAW record, including ones the run
        /// does not process, and a per-station negative flag would be set by those.
        std::set<std::string> rule_reached;

        /// CARRY-SIDE presence memory: (station, satellite, signal) -> the last epoch
        /// with a @c local_carry entry.  The audit's gap population is measured on the
        /// carried arcs (`network_amb_state.csv`), which is a strict SUBSET of the raw
        /// tracked set the ledger sees at its arrival site (that site is upstream of
        /// the elevation mask, the allow list and every admission screen), so the two
        /// populations are counted separately and never conflated.
        std::map<std::tuple<int, std::string, std::string>, long long> carry_last_ep;

        /// (station index, band digit, tracker signal) -> rows, for the run-end cell census.
        std::map<std::tuple<int, char, std::string>, long long> tracker_rows;
        /// (station index, band digit) -> the modelled phase of that cell (last seen).
        std::map<std::pair<int, char>, std::string> modelled_of_cell;

        /// "sat|signal" -> the per-column-epoch count of carried MODELLED stations.
        std::map<std::string, std::vector<int>> family_support;

        /// Anchor keys present in @c anchor_incumbent at the PREVIOUS epoch's audit.
        std::set<std::string> anchor_prev;

        /// Names of the producer stations, in roster order (for the trace and events).
        std::vector<std::string> station_names;

        /// Resolved horizon inputs, reported at run end so the number is auditable.
        double horizon_s{ 0.0 };
        double horizon_pl_s{ 0.0 };
        double horizon_iono_s{ 0.0 };
        double horizon_pbhold_s{ 0.0 };
        double horizon_readmit_s{ 0.0 };
    };

    /**
     * @brief Run-end counters of the observatory: one named class per disagreement.
     *
     * Lives in @c NetworkServiceCounters.  Every class is counted against a
     * denominator so a zero can be read: a zero with a zero denominator is dead
     * wiring, and the teardown says so in those words.
     */
    struct NetworkSignalRosterCounters {
        // --- the roster itself -------------------------------------------------
        long long epochs{ 0 };                ///< epochs the build ran (denominator).
        long long cells{ 0 };                 ///< (station, constellation, band) cells built, summed over epochs.
        long long cells_modelled{ 0 };        ///< of those, cells with a modelled phase or code.
        long long tracked_signals{ 0 };       ///< tracked signals seen, summed over epochs.
        long long companion_signals{ 0 };     ///< companion codes assigned, summed over epochs.
        long long excluded_signals{ 0 };      ///< excluded signals with a reason, summed over epochs.
        long long excl_allowlist{ 0 };        ///< of those, excluded by the network allow list.
        long long excl_not_latched{ 0 };      ///< of those, a band with a latch that chose another signal.
        long long excl_no_latch{ 0 };         ///< of those, a band the latch does not carry at all.
        /// P2 SELF-CHECK, bar 0: cells whose Modelled role differs from the latch.
        /// The roster is BUILT from the latch, so any nonzero value is a bug here.
        long long roster_latch_disagree{ 0 };

        // --- (a) the arc-level tracker against modelled_phase ------------------
        long long trk_rows{ 0 };              ///< RawSlipObs band slots compared (denominator).
        long long trk_rows_mismatch{ 0 };     ///< of those, the tracker's signal is not the modelled one.
        long long trk_rows_no_modelled{ 0 };  ///< band slots whose band has no modelled phase (not comparable).
        long long trk_cells{ 0 };             ///< run-end: (station, band) cells with at least one row.
        long long trk_cells_mismatch{ 0 };    ///< run-end: cells whose DOMINANT tracker signal is not the modelled one.
        long long trk_cells_b1{ 0 };          ///< of those, on band digit 1.
        long long trk_cells_b2{ 0 };          ///< of those, on band digit 2.
        long long trk_stations_mismatch{ 0 }; ///< run-end: distinct stations with at least one mismatched cell.

        // --- (b) the count rule against the priority rule ----------------------
        long long rule_stations{ 0 };         ///< stations whose candidate list was reachable (denominator).
        long long rule_stations_differ{ 0 };  ///< of those, stations the two rules latch differently on ANY (band, obs type).
        long long rule_stations_differ_b1{ 0 };///< of those, stations whose BAND-1 PHASE differs (card CSL's own population).
        long long rule_unreachable{ 0 };      ///< stations whose candidates were never retained.

        // --- (c) local_carry against the ledger --------------------------------
        long long lc_entries{ 0 };            ///< carry entries examined, summed over epochs (denominator).
        long long lc_arc_dead{ 0 };           ///< entries whose arc is Dead (the carry outlived the arc).
        long long lc_arc_unknown{ 0 };        ///< entries the ledger has never seen at all.
        long long lc_tracking_no_entry{ 0 };  ///< MODELLED arcs Tracking with no carry entry.

        // --- (d) StateManager ambiguity presence against the ledger ------------
        long long amb_states{ 0 };            ///< ambiguity states examined, summed over epochs (denominator).
        long long amb_arc_gapped{ 0 };        ///< states alive while their arc is Gap (carried across a gap).
        long long amb_destroyed_in_gap{ 0 };  ///< arcs in Gap INSIDE the horizon whose state is gone.
        long long amb_arc_unknown{ 0 };       ///< ambiguity states the ledger has no arc for.

        // --- (e) the anchor election against the ledger ------------------------
        long long anchor_first_sightings{ 0 };    ///< keys that gained an incumbent with none before (denominator).
        long long anchor_first_sighting_reentry{ 0 }; ///< of those, whose arcs were Reentered or Gap: a RETURN read as a first sighting.

        // --- (f) phase-bias family support -------------------------------------
        long long fam_families{ 0 };          ///< run-end: families with at least one alive epoch.
        long long fam_thin{ 0 };              ///< run-end: families below 3 carried stations in more than 10 per cent of their alive epochs.

        // --- (g) wire family coverage, from the emitter's own epoch -------------
        long long wire_sat_epochs{ 0 };       ///< satellite-epochs emitted with a clock record (denominator).
        std::map<std::string, long long> wire_pb_missing;  ///< signal -> satellite-epochs with a clock and no phase bias.
        std::map<std::string, long long> wire_cb_missing;  ///< signal -> satellite-epochs with a clock and no code bias.
        std::map<std::string, long long> wire_pb_present;  ///< signal -> satellite-epochs that carried one.

        // --- (h) coverage-gate drop edges against the ledger -------------------
        long long gate_drop_edges{ 0 };       ///< PHASE_ANCHOR gate_drop edges seen (denominator).
        long long gate_drop_edges_gapped{ 0 };///< of those, whose family has at least one arc in Gap.
        long long gate_drop_edges_tracking{ 0 }; ///< of those, whose family is entirely Tracking.

        // --- the ledger's own transitions --------------------------------------
        long long arc_birth{ 0 };
        long long arc_gap{ 0 };
        long long arc_reentry{ 0 };
        long long arc_death{ 0 };
        long long arc_events{ 0 };            ///< ARC rows written to network_events.csv (holdoff-throttled).
        long long arc_trace_rows{ 0 };        ///< rows written to network_arc_ledger.csv (one per transition).
        long long arc_live_max{ 0 };          ///< peak live arcs in one epoch.
        long long arc_known_max{ 0 };         ///< peak retained records (live + gapped + dead-not-forgotten).
        long long gap_2_30{ 0 };              ///< RAW-side re-entries after 2 to 30 missed epochs at an arc with a live carry.
        long long gap_2_30_answered{ 0 };     ///< of those, answered by the gap re-entry tier in the same epoch.
        long long gap_any{ 0 };               ///< RAW-side re-entries after any number of missed epochs.
        /// CARRY-SIDE gap census, the audit's own population (`network_amb_state.csv`
        /// presence, which is what the 195 / 68 of the card were measured on).
        long long carry_gap_any{ 0 };         ///< carry returns after at least one missed epoch.
        long long carry_gap_2_30{ 0 };        ///< of those, after 2 to 30 missed epochs.
        long long carry_gap_2_30_answered{ 0 };///< of those, answered by the gap re-entry tier at the return epoch.
        long long carry_entries_seen{ 0 };    ///< carry RE-STASHES walked (denominator of the census).

        double    build_ms{ 0.0 };            ///< wall time inside the build seam, summed.
        double    audit_ms{ 0.0 };            ///< wall time inside the audit seam, summed.
    };

    /**
     * @brief Arm the observatory from config (once, at run start).
     *
     * Resolves the ledger's HORIZON as the maximum of the readers' gap windows
     * (@c network.phase_leveling_max_gap_s, @c network.iono_carry_max_gap_s,
     * @c network.emit_pb_hold_max_gap_s, @c network.readmit_seed_horizon_s), sizes
     * the per-station caches and records the station names.
     *
     * @param ctx      Run context; @c ctx.roster and @c ctx.arc_ledger are filled in.
     * @param config_  The run's config.
     * @param log      Producer logger (one arming line when armed).
     * @param stations The PRODUCER roster.  The held-out consumer is not on it.
     */
    void networkSignalRosterArm(NetworkRunContext& ctx, const GnssConfig& config_,
                                const std::shared_ptr<spdlog::logger>& log,
                                const std::vector<NetStation>& stations);

    /**
     * @brief Build this epoch's roster and advance the arc ledger.  ONE call.
     *
     * Disarmed it returns immediately: no walk, no allocation, no counter moved.
     *
     * @param ctx      Run context (the roster, the ledger and the counters).
     * @param config_  The run's config.
     * @param log      Producer logger.
     * @param traces   Trace bundle; the @c ARC event rows and the per-transition CSV.
     * @param se       The epoch as it reaches the seam (post canonical-pair admission).
     * @param stations The PRODUCER roster and the source of each station's latch.
     */
    void networkBuildSignalRoster(NetworkRunContext& ctx, const GnssConfig& config_,
                                  const std::shared_ptr<spdlog::logger>& log,
                                  NetworkRunTraces& traces,
                                  const SyncedEpoch& se,
                                  const std::vector<NetStation>& stations);

    /**
     * @brief The disagreement pass: every class of P4, counted and nothing else.
     *
     * Runs LATE in the epoch, where each input exists: the tracker's rows are built
     * in PASS A, @c local_carry is post-harvest, the ambiguity states are
     * post-assembly, the anchor incumbents are post-election and the solution epoch
     * is post-emit.  Reads only; changes nothing.
     *
     * @param ctx       Run context.
     * @param log       Producer logger.
     * @param se        This epoch.
     * @param stations  The PRODUCER roster.
     * @param raw       This epoch's raw slip observables, or nullptr when the
     *                  tracker did not run (the class then reports a zero
     *                  denominator rather than a zero count).
     * @param freed     Ambiguity keys freed this epoch, as (station index, key).
     * @param sol       The emitted solution epoch.
     * @param emitted   True when this epoch actually reached the wire.
     */
    void networkSignalRosterAudit(NetworkRunContext& ctx,
                                  const std::shared_ptr<spdlog::logger>& log,
                                  const SyncedEpoch& se,
                                  const std::vector<NetStation>& stations,
                                  const RawSlipEpoch* raw,
                                  const std::set<std::pair<int, std::string>>& freed,
                                  const NetworkSolutionEpoch& sol,
                                  bool emitted);

    /**
     * @brief Count one coverage-gate DROP EDGE against the ledger (class h).
     *
     * Called from the driver's existing @c gate_drop.events loop, so the edge is
     * counted exactly where it is already evented.  No-op when disarmed.
     *
     * @param ctx    Run context.
     * @param sat    Satellite of the dropped family.
     * @param signal Signal of the dropped family.
     */
    void networkSignalRosterGateDrop(NetworkRunContext& ctx, const std::string& sat,
                                     const std::string& signal);

    /**
     * @brief Run-end readout: the table of every class, or nothing when disarmed.
     *
     * @param ctx Run context (counters, roster state and ledger).
     * @param log Producer logger.
     */
    void networkSignalRosterRunEnd(NetworkRunContext& ctx,
                                   const std::shared_ptr<spdlog::logger>& log);

} // namespace GnssProcessor
