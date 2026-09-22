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
 *  - DATUM: the signal the S-basis pins.  Empty in phase 1; PHASE 2 makes the IF pin
 *    pair election a roster WRITER (@ref networkSignalRosterDatumPin), which runs in
 *    the datum seam far downstream of the roster's arrival site.  Still an
 *    observatory: nothing reads the fields, and the run-end table carries the census.
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
#include "filter/NetworkAnchorReentry.hpp"  // Card SID phase 4: AnchorReentry

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
    struct PppObservation;           // common/PppObservation.hpp
    struct SyncedEpoch;              // rinex/MultiRinexSync.hpp
    struct GnssTime;                 // common/GnssTypes.hpp
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
        /// Phase 2: the phase the pinned code accompanies at THIS station, filled only
        /// where @ref datum_code is also this cell's @ref modelled_code (otherwise
        /// naming one would derive a signal the estimator does not carry).
        std::string datum_phase;
        /// Phase 2: the CODE signal the S-basis ionosphere-free pin realized on this
        /// (constellation, band) this epoch.  The pin is per SATELLITE, so this is the
        /// mode over the epoch's pinned satellites; see @ref networkSignalRosterDatumPin.
        std::string datum_code;
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
     * @param second_phase_allow `network.second_phase_signals`, in configured order
     *                          (card SID phase 6a).
     * @param second_phase_armed `network.second_phase_channel`.
     * @param all_channels      Card SID phase 6b: `processing.channel_mode: all` on
     *                          the network path.  SUBSUMES the two flags above (the
     *                          caller passes them as false), and fills
     *                          @c modelled_extra and @c companion_code on EVERY band
     *                          from the tracked, admitted attributes rather than from
     *                          an allow-list.
     * @return band digit -> cell, for every band digit that has a tracked signal or
     *         a latched one.
     */
    std::map<char, BandRoster> signalRosterBuildCells(
        const std::vector<std::string>& tracked,
        const std::vector<RosterLatchRow>& latch,
        const std::vector<std::string>& companion_allow,
        bool companions_armed,
        const std::set<std::string>* allowlist,
        const std::vector<std::string>& second_phase_allow = {},
        bool second_phase_armed = false,
        bool all_channels = false);

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

        /// PHASE 2, the IF pin as a WRITER: this epoch's realized per-satellite
        /// ionosphere-free pin, as @c NetworkSbasis::selectIfPinPair returned it
        /// (satellite -> the two CODE signals).  Kept for the run-end census only;
        /// nothing reads it, and the per-cell designation it produces lives in
        /// @c BandRoster::datum_code / @c datum_phase.
        std::map<std::string, std::pair<std::string, std::string>> datum_pin;
        /// "<constellation>|<band digit>" -> code signal -> SATELLITES that pinned it
        /// at this epoch.  The pin is per SATELLITE while a roster cell is per (station,
        /// constellation, band), so the designation written into the cells is the
        /// per-(constellation, band) MODE over this epoch's pinned satellites and a
        /// (constellation, band) with more than one pinned code is counted as
        /// AMBIGUOUS rather than silently collapsed.
        std::map<std::string, std::map<std::string, long long>> datum_votes;

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

        // =================================================================
        // PHASE 2 (network.roster_tracker, the IF pin writer, the ADMITTED
        // lifecycle).  Every class below is reported BESIDE its raw-side twin and
        // never added to it.
        // =================================================================

        // --- the tracker's roster read (invariant 4: is the reader doing the work?)
        long long trk_roster_hits{ 0 };       ///< lookups answered by a roster cell's modelled phase.
        /// Lookups the roster could not answer that the station's LATCH could.  The
        /// roster is built upstream of computeModels, so the FIRST epoch of a
        /// constellation has no modelled role yet; a count far above one epoch's
        /// worth of cells means the reader is not reading the roster.
        long long trk_roster_fallback{ 0 };
        long long trk_roster_none{ 0 };       ///< lookups neither could answer (a band with no latch at all).

        // --- the IF pin as a roster WRITER (observatory: nothing reads the fields)
        long long datum_epochs{ 0 };          ///< epochs the pin writer ran with a nonempty pin (denominator).
        long long datum_sat_pins{ 0 };        ///< per-SATELLITE pins seen, summed over epochs.
        long long datum_cells{ 0 };           ///< roster cells given a datum CODE, summed over epochs.
        long long datum_cells_phase{ 0 };     ///< of those, that also carry a datum PHASE.
        /// Cells whose (constellation, band) had more than one pinned code across the
        /// epoch's satellites: the designation written is the mode, and this says so.
        long long datum_cells_ambiguous{ 0 };
        /// Cells whose pinned code is NOT that station's modelled code, so no datum
        /// phase can be named without inventing one.
        long long datum_cells_off_model{ 0 };
        /// "<constellation>|<band>|<signal>" -> CELLS the designation was written to,
        /// summed over epochs (the same unit as @ref datum_cells).
        std::map<std::string, long long> datum_code_census;
        std::map<std::string, long long> datum_phase_census;

        // --- the ADMITTED lifecycle -------------------------------------------
        long long adm_rows{ 0 };              ///< admitted PHASE rows fed to the ledger, summed over epochs.
        long long adm_birth{ 0 };
        long long adm_gap{ 0 };
        long long adm_reentry{ 0 };
        long long adm_death{ 0 };
        long long adm_trace_rows{ 0 };        ///< admitted-side rows written to network_arc_ledger.csv.
        long long adm_live_max{ 0 };          ///< peak admitted-live arcs in one epoch.

        // --- (c) local_carry against the ADMITTED lifecycle --------------------
        long long lc_adm_arc_dead{ 0 };           ///< carry entries whose ADMITTED arc is Dead.
        long long lc_adm_arc_unknown{ 0 };        ///< carry entries the admitted side has never seen.
        long long lc_adm_tracking_no_entry{ 0 };  ///< MODELLED arcs admitted-Tracking with no carry entry.

        // --- (d) StateManager ambiguities against the ADMITTED lifecycle -------
        long long amb_adm_arc_gapped{ 0 };        ///< states alive while their ADMITTED arc is Gap.
        long long amb_adm_arc_unknown{ 0 };       ///< states with no admitted arc at all.
        long long amb_adm_destroyed_in_gap{ 0 };  ///< ADMITTED arcs in a Gap inside the horizon whose state is gone.

        // --- the carry-side gap census ON THE ADMITTED LIFECYCLE ---------------
        // The phase-1 census reads the carry's own RE-STASH stamp; this one reads
        // the admitted arc's re-entry edge.  Both are reported so the two
        // definitions can be compared instead of one silently replacing the other.
        long long carry_adm_gap_any{ 0 };         ///< admitted re-entries at an arc with a live carry.
        long long carry_adm_gap_2_30{ 0 };        ///< of those, after 2 to 30 missed epochs.
        long long carry_adm_gap_2_30_answered{ 0 };///< of those, answered by the gap re-entry tier at the return epoch.

        // =================================================================
        // PHASE 3 (network.ledger_reentry): the prune and expiry edges, and the
        // UNIFIED answered split.  The first three are OBSERVATORY and ride the
        // phase-1 knob; the split is reported in BOTH states of the phase-3 knob.
        // =================================================================

        /// (R3a) `local_carry` entries the gap prune retired at
        /// `phase_leveling_max_gap_s`, each now an `ARC arc_death reason=horizon`.
        long long carry_prune_deaths{ 0 };
        long long carry_prune_events{ 0 };    ///< of those, evented (holdoff-throttled).
        /// (R3b) ambiguity columns the state-vector prune destroyed while their
        /// ADMITTED arc was in a Gap INSIDE the horizon: the silent death path,
        /// now a ledger mark.  The prune itself is unchanged.
        long long state_pruned_marks{ 0 };
        long long state_prune_keys{ 0 };      ///< columns the prune destroyed, all causes (denominator).
        long long state_prune_unknown{ 0 };   ///< of those, on an arc the ledger has never admitted.
        /// ADMITTED re-entries that landed on a RE-CREATED column (the arc carried
        /// a `state_pruned` mark through its gap).
        long long adm_state_recreated{ 0 };

        // --- (R4) the unified four-way answered split -------------------------
        // Population: ADMITTED re-entries after 2 to 30 missed epochs.  Read off
        // the response's OWN record per arc, not inferred, and exhaustive.
        long long ans_total{ 0 };             ///< the population (denominator).
        long long ans_freed{ 0 };             ///< the response reached the arc and the mark landed.
        long long ans_bridged_v{ 0 };         ///< a raw tier saw the return, called it CLEAN, and bridged it by design.
        long long ans_bridged_nv{ 0 };        ///< nothing looked at the return: bridged with no verdict.
        long long ans_no_carry{ 0 };          ///< a response was due but there was nothing to protect.
        long long ans_no_carry_entry{ 0 };    ///< the arc has no `local_carry` entry at all (outside the response's reach).

        // =================================================================
        // PHASE 4a (network.ledger_screens): the REASON-MASK WRITERS and the
        // anchor read.  Every class is OBSERVATORY; phase 4b adds the two
        // restore counters at the end, reported in both states of its knob.
        // =================================================================

        /// (S1) Admitted gaps the fold stamped a reason on (denominator).
        long long gap_folded{ 0 };
        /// (S1) Of those, gaps whose mask carries more than one reason.
        long long gap_multi{ 0 };
        /// (S1) Per reason INDEX (@ref arcReasonIndex): admitted gaps it opened.
        /// A gap with two reasons increments both, so these do not sum to
        /// @ref gap_folded; that is stated in the table rather than hidden.
        std::vector<long long> gap_by_reason{ std::vector<long long>(kArcReasonCount, 0) };
        /// (S1) Per reason INDEX: WRITER CALLS, i.e. (arc, screen) records made.
        std::vector<long long> writer_calls{ std::vector<long long>(kArcReasonCount, 0) };
        /// (S1) Per reason INDEX: writer invocations that reached no admitted arc.
        std::vector<long long> writer_unknown{ std::vector<long long>(kArcReasonCount, 0) };
        /// (S1) The phase-3 NO-VERDICT population split by the reason mask of the
        /// gap that preceded the return.  Same indices; a return whose gap mask is
        /// empty (no fold ran, e.g. the knob armed mid-lineage) is counted in
        /// @ref nv_no_mask.
        std::vector<long long> nv_by_reason{ std::vector<long long>(kArcReasonCount, 0) };
        long long nv_total{ 0 };              ///< the no-verdict population seen at the split.
        long long nv_no_mask{ 0 };            ///< of those, with no reason mask at all.

        // --- (S2) the anchor election read against the ledger -------------
        long long anchor_fs{ 0 };             ///< first sightings the election made (denominator).
        long long anchor_fs_return{ 0 };      ///< of those, whose family had arcs in Reentered or Gap.
        long long anchor_fs_same_gen{ 0 };    ///< of those, a same-generation return of a dropped family.
        long long anchor_fs_new_gen{ 0 };     ///< of those, a return with new generations.
        long long anchor_fs_events{ 0 };      ///< ARC anchor_first_sighting_on_return rows written.
        /// Coverage-gate DROP EDGES seen by this class (denominator; the same
        /// edges class (h) counts, read here with the generation test as well).
        long long anchor_drop_edges{ 0 };
        long long anchor_drop_arcs_tracking{ 0 }; ///< arcs Tracking at the edges, summed.
        long long anchor_drop_arcs_gap{ 0 };      ///< arcs Gap or Reentered at the edges, summed.
        long long anchor_drop_arcs_dead{ 0 };     ///< arcs Dead at the edges, summed.
        /// (S2) `anchor_arc_age` against the ledger's admitted age.  The container
        /// exists only while network.eq34_anchor_min_arc_ep, the pin bridge or the
        /// fresh-pin ramp is armed, so a zero DENOMINATOR here means the hygiene
        /// seam never ran and the class is dead wiring, not a clean agreement.
        long long age_compared{ 0 };
        long long age_disagree{ 0 };
        long long age_no_ledger{ 0 };         ///< arcs the ledger could give no admitted age for.

        // --- (B1 / B2) the behaviour half, reported in BOTH knob states ----
        long long anchor_restore{ 0 };        ///< returns where the previous incumbent was restored.
        long long anchor_restore_refused{ 0 };///< same-generation returns the election rules refused.
        long long anchor_reentry_new{ 0 };    ///< returns that proceeded as today (new generations).
        long long age_from_ledger{ 0 };       ///< arc ages taken from the ledger instead of the counter.

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
     * @brief PHASE 2 — the roster's answer to "which phase stands for this band?".
     *
     * The READ side of @c network.roster_tracker.  Pure and const: it looks the cell
     * up and returns its @c modelled_phase, or an empty string when the roster has
     * no answer (the station reported nothing, the constellation has no priority
     * list, the band has no latch, or this is the FIRST epoch of a constellation,
     * whose latch is decided inside @c computeModels downstream of the build seam).
     * The caller decides what an empty answer means.
     *
     * @param rs      The run's roster (@c ctx.roster).
     * @param station Producer station index.
     * @param cons    Constellation string, as @c PppUtils::getSystemString gives it.
     * @param band    RINEX band digit.
     */
    std::string signalRosterModelledPhase(const SignalRoster& rs, int station,
                                          const std::string& cons, char band);

    /**
     * @brief The ADMITTED phase arcs of this epoch.  PURE.
     *
     * @c modeled[i] exists only for satellites that survived the elevation mask,
     * and @c PppObservation::observed holds only the signals the model built (the
     * canonical latch, its card SCC companions and whatever the allow list
     * admitted), which is exactly the set @c local_carry and the ambiguity states
     * are keyed from.  ONE definition, shared by the roster's admitted lifecycle
     * and by card SID phase 3's re-entry population, so the two cannot drift.
     *
     * @param modeled Per-station MODELLED observations of this epoch.
     * @param present Per-station PASS-A presence.
     * @param out     Receives the (station, satellite, signal) phase arcs.  Cleared first.
     */
    void signalRosterAdmittedArcs(
        const std::vector<std::vector<PppObservation>>& modeled,
        const std::vector<bool>& present,
        std::set<ArcLedger::Key>& out);

    /**
     * @brief Card SID phase 3 — the admitted arcs whose signal the roster calls
     *        Modelled.  PURE.
     *
     * @ref signalRosterAdmittedArcs restricted by the ROSTER's role, which is the
     * population the re-entry response is defined on: the response frees a carried
     * LEVEL, and only a modelled signal has one.  A cell the roster cannot answer
     * for (a constellation with no priority list, or the FIRST epoch of a
     * constellation, whose latch is decided inside @c computeModels downstream of
     * the build seam) rejects the arc and is COUNTED, never silently admitted.
     *
     * @param rs             The run's roster.
     * @param modeled        Per-station MODELLED observations of this epoch.
     * @param present        Per-station PASS-A presence.
     * @param out            Receives the arcs.  Cleared first.
     * @param n_not_modelled Optional: incremented once per rejected arc.
     */
    void signalRosterModelledArcs(
        const SignalRoster& rs,
        const std::vector<std::vector<PppObservation>>& modeled,
        const std::vector<bool>& present,
        std::set<ArcLedger::Key>& out,
        long long* n_not_modelled = nullptr);

    /// @brief What one station's datum write did (@ref signalRosterApplyDatum).
    struct SignalRosterDatumWrite {
        long long cells{ 0 };      ///< cells given a datum CODE.
        long long with_phase{ 0 }; ///< of those, that also carry a datum PHASE.
        long long off_model{ 0 };  ///< of those, whose pinned code is not the modelled one.
        long long ambiguous{ 0 };  ///< of those, on a (constellation, band) with more than one pinned code.
    };

    /**
     * @brief PHASE 2 — resolve the per-SATELLITE IF pins to one code designation per
     *        (constellation, band digit).  PURE.
     *
     * The constellation key is the ROSTER's, that is the system string
     * @c PppUtils::getSystemString gives ("GPS"), not the satellite id's first
     * character ("G") that the datum seam keys its own maps by.
     *
     * @param if_pin_real Satellite -> the two realized IF-pin CODE signals.
     * @param votes_out   Optional: the full tally, "<cons>|<band>" -> signal -> count.
     * @param ambiguous   Optional: receives every "<cons>|<band>" whose satellites did
     *                    not agree on one code.
     * @return "<cons>|<band>" -> the designated code signal (the mode; the lexically
     *         lowest code on a tie, which is the pin's own tie break within a band).
     */
    std::map<std::string, std::string> signalRosterDatumPicks(
        const std::map<std::string, std::pair<std::string, std::string>>& if_pin_real,
        std::map<std::string, std::map<std::string, long long>>* votes_out = nullptr,
        std::set<std::string>* ambiguous = nullptr);

    /**
     * @brief PHASE 2 — write the datum designation into one station's cells.  PURE.
     *
     * @param sr        The station's roster, MUTATED.
     * @param picks     @ref signalRosterDatumPicks output.
     * @param ambiguous Optional: the not-unanimous set, counted rather than acted on.
     */
    SignalRosterDatumWrite signalRosterApplyDatum(
        StationRoster& sr, const std::map<std::string, std::string>& picks,
        const std::set<std::string>* ambiguous = nullptr);

    /**
     * @brief PHASE 2 — the IF pin pair election as a roster WRITER.
     *
     * Called once per epoch straight after @c NetworkSbasis::buildCodeBiasDatumRows
     * returns its realized pins, which is the only site that knows which signals the
     * S-basis actually pinned this epoch.
     *
     * GRANULARITY, stated because it is not the roster's: the pin is elected PER
     * SATELLITE over the network's registered code keys and it names two CODE
     * signals.  A roster cell is per (station, constellation, band), so the
     * designation written into the cells is the per-(constellation, band) MODE over
     * the epoch's pinned satellites; a (constellation, band) whose satellites did not
     * agree is counted as AMBIGUOUS rather than collapsed silently.  @c datum_phase
     * is filled only where the pinned code IS that station's modelled code, in which
     * case the phase it accompanies is that cell's modelled phase; anywhere else it
     * stays empty rather than being derived from the code's attribute.
     *
     * OBSERVATORY: nothing reads @c BandRoster::datum_phase or @c datum_code.  The
     * fields are rebuilt with the roster at the next epoch, so the census is
     * accumulated here.
     *
     * @param ctx         Run context (the roster and the counters).
     * @param if_pin_real Satellite -> the two realized IF-pin CODE signals.
     */
    void networkSignalRosterDatumPin(
        NetworkRunContext& ctx,
        const std::map<std::string, std::pair<std::string, std::string>>& if_pin_real);

    /**
     * @brief The disagreement pass: every class of P4, counted and nothing else.
     *
     * Runs LATE in the epoch, where each input exists: the tracker's rows are built
     * in PASS A, @c local_carry is post-harvest, the ambiguity states are
     * post-assembly, the anchor incumbents are post-election and the solution epoch
     * is post-emit.  Reads only; changes nothing.
     *
     * PHASE 2 adds the ADMITTED lifecycle here, before every class that reads it:
     * the station's MODELLED phase rows (@c PppObservation::observed, which is what
     * survives the elevation mask, the allow list and the admission screens, and is
     * the same source @c local_carry and the ambiguity states are keyed from) advance
     * a second state machine on each arc.  Classes (c) and (d) are then stated on
     * BOTH sides, and the carry-side gap census gains an admitted-lifecycle reading
     * beside its re-stash-stamp one.
     *
     * @param ctx       Run context.
     * @param log       Producer logger.
     * @param traces    Trace bundle (the admitted-side rows of network_arc_ledger.csv).
     * @param se        This epoch.
     * @param stations  The PRODUCER roster.
     * @param modeled   Per-station MODELLED observations of this epoch: the admitted
     *                  row set the admitted lifecycle is fed from.
     * @param present   Per-station PASS-A presence.
     * @param raw       This epoch's raw slip observables, or nullptr when the
     *                  tracker did not run (the class then reports a zero
     *                  denominator rather than a zero count).
     * @param freed     Ambiguity keys freed this epoch, as (station index, key).
     * @param sol       The emitted solution epoch.
     * @param emitted   True when this epoch actually reached the wire.
     */
    void networkSignalRosterAudit(NetworkRunContext& ctx,
                                  const std::shared_ptr<spdlog::logger>& log,
                                  NetworkRunTraces& traces,
                                  const SyncedEpoch& se,
                                  const std::vector<NetStation>& stations,
                                  const std::vector<std::vector<PppObservation>>& modeled,
                                  const std::vector<bool>& present,
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
     * @brief Card SID phase 4 (S2 / B1) — fold the election's ledger read.
     *
     * Called from the driver immediately after @c networkElectPhaseAnchors returns,
     * which is where the per-epoch @ref AnchorReentry result is in scope and where
     * every other election readback is already folded. Counts the first sightings
     * the ledger says were RETURNS, emits their identity, and reports phase 4b's
     * restore decisions. No-op when the read is not armed.
     *
     * @param ctx     Run context (the counters).
     * @param traces  Trace bundle (the @c ARC and @c PHASE_ANCHOR event rows).
     * @param t       This epoch's time stamp.
     * @param re      The election's per-epoch read result.
     */
    void networkSignalRosterAnchorRead(NetworkRunContext& ctx,
                                       NetworkRunTraces& traces,
                                       const GnssTime& t,
                                       const AnchorReentry& re);

    /**
     * @brief Run-end readout: the table of every class, or nothing when disarmed.
     *
     * @param ctx Run context (counters, roster state and ledger).
     * @param log Producer logger.
     */
    void networkSignalRosterRunEnd(NetworkRunContext& ctx,
                                   const std::shared_ptr<spdlog::logger>& log);

} // namespace GnssProcessor
