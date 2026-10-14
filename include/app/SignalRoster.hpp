#pragma once
/**
 * @file SignalRoster.hpp
 * @brief The per-station signal roster and the observatory that counts every
 *        disagreement between it and the sites that currently decide the same
 *        question (card SID phase 1), built on every network run since card NOM
 *        step N3a, with the "signals as planned" table.
 *
 * ### What is built, and what the knob does (card NOM step N3a)
 *
 * The roster, the arc ledger and the disagreement census are built on every network
 * run.  @c network.signal_roster no longer arms them: it is the switch of the arc
 * ledger's trace only (@c logs/network_arc_ledger.csv, one row per arc transition,
 * and the @c ARC event rows the build and the audit write to @c network_events.csv).
 * The readers of the roster and the ledger (@c network.roster_tracker,
 * @c network.ledger_reentry, @c network.ledger_screens, @c network.ledger_emit) need
 * no other knob, because what they read now exists on every run.  The solve reads
 * neither object unless one of those readers is armed, so building them changes no
 * product.  After the first emitted epoch whose roster carries a modelled role, and at
 * run end, a "signals as planned" table is logged and written to the run manifest
 * (@ref signalRosterPlanTable): per constellation and band, the modelled, companion,
 * datum and excluded signals with the number of stations holding each role, then one
 * line per decision site the census compares with the roster
 * (@ref signalRosterSiteSpecs).
 *
 * ### Motivation
 *
 * The SID audit (`runs/_msc/sid_audit_report_2026-09-22.md`, Part A) lists 128 sites
 * in 38 files that choose which signal represents a band, under at least nine
 * different rules, including at least six independent implementations of "the two
 * lowest distinct bands" with different tie breaks.  No code compares them.  Part D
 * of the same report measures the effect on the field baseline: the arc-level slip
 * tracker's band signal differs from the carried one at 46 station-band cells (44 of
 * 58 stations), and band-1 re-entries at those cells go unanswered (0 of 31, against
 * 55 of 67 elsewhere).
 *
 * ### Contents
 *
 * One object per epoch, owned by @c NetworkRunContext, answers for every
 * (station, constellation, band digit):
 *
 *  - Tracked: the phase and code signals the receiver reported this epoch.
 *  - Modelled: the phase and code the estimator carries, copied from the station's
 *    canonical latch (@c PppModel::exportCanonicalSignals), never re-derived.
 *  - Companion: the band-1 codes carried as card SCC companions.
 *  - Excluded: the signals that are present and not used, each with a reason.
 *  - Datum: the signal the S-basis pins.  Phase 1 leaves it empty.  Phase 2 makes
 *    the IF pin pair election write it (@ref networkSignalRosterDatumPin), from the
 *    datum seam, which runs well after the roster is built.  Nothing in the solve
 *    reads the datum fields; the wire's declaration (@c network.ledger_emit) and the
 *    tables do.
 *
 * ### Build site
 *
 * A single call, @ref networkBuildSignalRoster, in @c ApplicationNetwork.cpp,
 * directly after the canonical-pair-admission seam returns the epoch and before
 * the reduced-RHS recorder.  @c computeModels, the T0 detectors,
 * @c networkComputeRawObservables, assembly, the RAIM pools and the traces all
 * derive from the @c RinexObservation handed on at that site, so the roster sees
 * the same record as all of them, and everything it needs is available there.
 *
 * ### Observation only, unless a reader is armed
 *
 * The roster and the arc ledger are read by the disagreement counters, the tables
 * and the trace.  With no reader armed, the solve and the wire are byte-identical
 * whether the trace is on or off (standing invariant 6 in both states), and the
 * run-end tables are the on-path telemetry (standing invariant 4).  The SID readers
 * read them under their own knobs.
 */

#include "common/GnssTypes.hpp"         // System (the tracker cell census key)
#include "filter/ArcLedger.hpp"
#include "filter/NetworkAnchorReentry.hpp"  // AnchorReentry (card SID phase 4)

#include <array>
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
     * Readers ask for a role instead of re-deriving a selection rule.  Phase 1
     * assigns the roles and counts them; phases 2 to 6 move the readers over one at
     * a time.
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
     * @c modelled_extra holds the additional modelled phases of the band (card SID
     * phase 6): band-1 second phases under @c network.second_phase_channel, or every
     * other tracked, admitted phase of the band under @c processing.channel_mode
     * "all" on the network path.  With neither armed it is empty and the estimator
     * carries one phase per band.
     */
    struct BandRoster {
        std::vector<std::string> tracked_phase;   ///< raw phase keys reported this epoch, ascending.
        std::vector<std::string> tracked_code;    ///< raw code keys reported this epoch, ascending.
        std::string modelled_phase;               ///< the latched phase of this band, or empty.
        std::string modelled_code;                ///< the latched code of this band, or empty.
        std::vector<std::string> modelled_extra;  ///< additional modelled phases (phase 6); else empty.
        std::vector<std::string> companion_code;  ///< companion codes (card SCC), in configured order.
        /// The phase that the pinned code accompanies at this station (phase 2).  Filled
        /// only where @ref datum_code equals this cell's @ref modelled_code; anywhere
        /// else, naming a phase would derive a signal the estimator does not carry.
        std::string datum_phase;
        /// The code signal that the S-basis ionosphere-free pin realized on this
        /// (constellation, band) this epoch (phase 2).  The pin is per satellite, so
        /// this is the mode over the epoch's pinned satellites; see
        /// @ref networkSignalRosterDatumPin.
        std::string datum_code;
        /// (signal, reason) for every tracked signal that is neither modelled (latched
        /// or in @ref modelled_extra) nor a companion.  Reasons, in precedence order:
        /// "allowlist" (not on the network allow list), "not_latched" (the band's latch
        /// chose another signal), "no_latch" (the band has no latch).
        std::vector<std::pair<std::string, const char*>> excluded;
    };

    /// @brief One station's roster: constellation -> band digit -> cell.
    using StationRoster = std::map<std::string, std::map<char, BandRoster>>;

    /**
     * @brief One flat row of a station's canonical latch, as the roster reads it.
     *
     * A separate type from @c PppModel::CanonicalSignal, so the pure builder below
     * can be tested without constructing a model.
     */
    struct RosterLatchRow {
        char        obs_type{ 'C' };  ///< 'C' (code) or 'L' (phase).
        std::string signal;           ///< the latched RINEX code, e.g. "L1W".
    };

    /**
     * @brief Build one station's roster cells for one constellation.  Pure function.
     *
     * The selection rules are reproduced, not re-derived: @c modelled_phase and
     * @c modelled_code are the latch's own rows, and the companion walk applies the
     * card SCC rule (@c NetworkSecondCodeScope.cpp) in its configured order.
     *
     * @param tracked           Raw observation keys this epoch.  Keys that are not
     *                          'C' or 'L' keys of at least three characters are
     *                          ignored.  Input order does not matter (the output is
     *                          sorted).
     * @param latch             This constellation's latch rows, in any order.
     * @param companion_allow   @c network.second_code_signals, in configured order.
     * @param companions_armed  @c network.second_code_channel.
     * @param allowlist         The network signal allow list for this constellation,
     *                          or nullptr when every tracked code is admitted.
     * @param second_phase_allow `network.second_phase_signals`, in configured order
     *                          (card SID phase 6a).
     * @param second_phase_armed `network.second_phase_channel`.
     * @param all_channels      `processing.channel_mode: all` on the network path
     *                          (card SID phase 6b).  Replaces the two flags above
     *                          (the caller passes them as false) and fills
     *                          @c modelled_extra and @c companion_code on every band
     *                          from the tracked, admitted signals instead of from an
     *                          allow list.
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
     * a matching code, sorted on the band digit alone, and takes the first entry plus
     * the first entry of a different band.  That comparator is not a strict weak
     * ordering over same-band signals and @c std::sort is not stable, so the
     * same-band order is formally unspecified.  The current toolchain yields lexical
     * order, and this helper states that order explicitly.  Exposed so the unit suite
     * can pin it.
     *
     * @param tracked Raw observation keys this epoch.
     * @return band digit -> the tracker's phase for that band (lexically lowest
     *         phase of the band that also has its matching code).
     */
    std::map<char, std::string> signalRosterTrackerPicks(
        const std::vector<std::string>& tracked);

    // =====================================================================
    // The decision sites (card NOM step N3a)
    // =====================================================================

    /**
     * @brief A site of the code that decides a question the roster or the arc ledger
     *        also answers, and whose disagreement with them the census counts.
     *
     * The first six decide a band role or a signal choice (card NOM section 18,
     * groups A to G, lists every such site; these are the ones with a counter today).
     * The last five hold an arc's lifecycle, which the arc ledger also tracks.  The
     * order is the table's.
     */
    enum class RosterSite {
        CanonicalLatch,   ///< the station's canonical latch, which the roster copies.
        LatchRule,        ///< the count rule against the priority rule of the latch.
        SlipTrackerBand,  ///< the arc-level slip tracker's signal for a band.
        IfPinPair,        ///< the S-basis ionosphere-free pin pair.
        WirePhaseBias,    ///< the phase-bias families on the wire.
        WireCodeBias,     ///< the code-bias families on the wire.
        LocalCarry,       ///< the leveling carry's entries.
        AmbiguityStates,  ///< the station state vectors' ambiguity columns.
        AnchorElection,   ///< the phase-datum anchor election's first sightings.
        GapReentry,       ///< the gap re-entry response.
        CoverageGate,     ///< the phase-datum coverage gate's drop edges.
        kCount            ///< number of sites (not a site).
    };

    /// @brief Number of decision sites in @ref RosterSite.
    inline constexpr std::size_t kRosterSites = static_cast<std::size_t>(RosterSite::kCount);

    /**
     * @brief The declared text of one decision site, one per @ref RosterSite.
     *
     * Plain data, read by the table and the manifest record.  Each text answers one
     * question a reader of the table asks: which code decides, what the census
     * compares there, and what it counts as a disagreement.
     */
    struct RosterSiteSpec {
        RosterSite  site;          ///< the site this row declares (its index in the table).
        const char* name;          ///< stable identifier, used as the JSON key.
        const char* group;         ///< "signal" (a band role or signal choice) or "arc" (an arc's lifecycle).
        const char* where;         ///< the deciding code, by file or function.
        const char* compared;      ///< the unit of the compared count.
        const char* disagreement;  ///< what is counted as a disagreement.
    };

    /**
     * @brief The declared site texts, in @ref RosterSite order (checked by
     *        @c static_assert).
     *
     * @return One @ref RosterSiteSpec per site, with static storage duration.
     */
    const std::array<RosterSiteSpec, kRosterSites>& signalRosterSiteSpecs();

    /// @brief The census at one site: how many cases were compared, how many disagreed.
    struct RosterSiteCount {
        long long compared{ 0 };  ///< cases compared (the denominator).
        long long disagree{ 0 };  ///< of those, cases that disagreed with the roster or the ledger.
    };

    /// @brief One count per decision site, in @ref RosterSite order.
    using RosterSiteCounts = std::array<RosterSiteCount, kRosterSites>;

    /**
     * @brief The difference of two cumulative site censuses, site by site.  Pure.
     *
     * @param now   The census now.
     * @param start The census at an earlier point (the start of an epoch).
     * @return now minus start, per site.
     */
    RosterSiteCounts signalRosterSiteDelta(const RosterSiteCounts& now,
                                           const RosterSiteCounts& start);

    // =====================================================================
    // The plan: what the roster says one epoch modelled (card NOM step N3a)
    // =====================================================================

    /// @brief One signal in one role, with the number of stations that hold it in
    ///        that role (or, for a datum code, the number of satellites that pinned it).
    struct RosterPlanEntry {
        std::string signal;      ///< RINEX observation code ("L1C", "C2W").
        int         count{ 0 };  ///< stations holding the role (satellites for a datum vote).
    };

    /**
     * @brief One (constellation, band digit) of the plan, summed over the stations.
     *
     * Every list is ordered by count, highest first, then by signal name.
     */
    struct RosterPlanBand {
        std::string constellation;            ///< system name, as the roster keys it ("GPS").
        char        band{ '0' };              ///< RINEX band digit.
        int         stations_reporting{ 0 };  ///< stations that tracked a signal of the band this epoch.
        int         stations_modelling{ 0 };  ///< stations whose cell carries a modelled phase or code.
        std::vector<RosterPlanEntry> phase;        ///< the modelled (latched) phase.
        std::vector<RosterPlanEntry> code;         ///< the modelled (latched) code.
        std::vector<RosterPlanEntry> extra_phase;  ///< additional modelled phases (second phases, all-channel).
        std::vector<RosterPlanEntry> companion;    ///< card SCC companion codes (all-channel extra codes).
        /// The code the ionosphere-free pin designated for this (constellation, band),
        /// written into the cells (@c BandRoster::datum_code), counted in stations.
        std::vector<RosterPlanEntry> datum_code;
        /// The same designation's votes: each code the per-satellite pin used on this
        /// (constellation, band), counted in SATELLITES (the pin is elected per
        /// satellite; more than one code here is the ambiguous case).
        std::vector<RosterPlanEntry> datum_votes;
        /// The phase the pinned code accompanies, counted in stations; named only at a
        /// station whose modelled code is the pinned code.
        std::vector<RosterPlanEntry> datum_phase;
        /// Reason -> the excluded signals with their station counts, in the reason
        /// order "allowlist", "not_latched", "no_latch"; a reason with no signal is absent.
        std::vector<std::pair<std::string, std::vector<RosterPlanEntry>>> excluded;
    };

    /// @brief The plan of one epoch: every (constellation, band) with a cell at any station.
    struct RosterPlan {
        std::vector<RosterPlanBand> bands;  ///< constellation name order, then band digit.
        int stations{ 0 };                  ///< stations with at least one cell.

        /// @brief True when at least one cell carries a modelled phase or code.
        bool hasModelledRole() const;
    };

    /**
     * @brief Sum one epoch's cells over the stations.  Pure.
     *
     * @param by_station  The roster's cells, one map per station (@c SignalRoster::by_station).
     * @param datum_votes "<constellation>|<band>" -> pinned code -> satellites that
     *                    pinned it this epoch (@c SignalRoster::datum_votes).
     * @return One @ref RosterPlanBand per (constellation, band) with a cell at any
     *         station, and the number of stations with a cell.
     */
    RosterPlan signalRosterPlanOf(
        const std::vector<StationRoster>& by_station,
        const std::map<std::string, std::map<std::string, long long>>& datum_votes);

    /// @brief What the table's first line states about the epoch and the run it
    ///        describes, and the run's latch rule, which the latch-rule row names.
    struct RosterPlanHeader {
        /// "first emitted epoch with a modelled role", "run end (the last audited
        /// epoch)" or, with no audited epoch, "run end".
        std::string scope;
        /// False when no epoch reached the audit: there is then no epoch to describe,
        /// so the table prints no band row and "-" in the epoch columns, and the record
        /// writes null for the week, the time and each site's epoch counts.
        bool        audited{ true };
        int         week{ 0 };       ///< GPS week of the epoch whose roster the band rows show.
        double      tow{ 0.0 };      ///< its time of week (s).
        long long   epochs{ 0 };     ///< epochs the roster was built over the run so far.
        int         week_first{ 0 }; ///< GPS week of the first epoch the roster was built at.
        double      tow_first{ 0.0 };///< its time of week (s).
        int         week_last{ 0 };  ///< GPS week of the latest epoch the roster was built at.
        double      tow_last{ 0.0 }; ///< its time of week (s).
        /// This run's @c processing.canonical_signal_rule ("count" or "priority").
        std::string latch_rule{ "count" };
        /// Number of stations in @c processing.canonical_signal_priority_stations.  When
        /// it is above 0 the listed stations latch by the priority rule and every other
        /// station by the count rule, whatever @ref latch_rule says.
        int         latch_rule_priority_stations{ 0 };
    };

    /**
     * @brief The "signals as planned" table, one string per log line.  Pure.
     *
     * The first line states the epoch the band rows describe and the run the site
     * rows count over.  Then, for every (constellation, band), one row per role:
     * stations, phase, code, second phase, companion, datum, excluded (a role with
     * no signal reads "-").  Then one row per decision site with its compared and
     * disagreeing counts at that epoch and over the run, where it is decided and what
     * a disagreement is; the latch-rule row also names the rule this run latched by.
     * The last line names what the table does not cover yet.  With
     * @c hdr.audited false the first line says that no epoch was audited, no band row
     * is printed and the epoch columns read "-".
     *
     * @param plan   @ref signalRosterPlanOf of the epoch.
     * @param epoch  The site census of that epoch (@ref signalRosterSiteDelta).
     * @param run    The cumulative site census.
     * @param hdr    The epoch and run the table describes.
     * @return The table's lines, without a log prefix and with no trailing blank.
     */
    std::vector<std::string> signalRosterPlanTable(const RosterPlan& plan,
                                                   const RosterSiteCounts& epoch,
                                                   const RosterSiteCounts& run,
                                                   const RosterPlanHeader& hdr);

    /**
     * @brief The same table as one JSON object for the run manifest.  Pure.
     *
     * @c {"record":"signals_as_planned","scope":...,"audited":...,"week":...,"tow":...,
     * "epochs":...,"stations":...,"canonical_signal_rule":...,
     * "canonical_signal_priority_stations":...,"bands":[...],"sites":[...]}, on one
     * line.  A band carries its station counts and one signal -> count object per role
     * ("phase", "code", "second_phase", "companion", "datum_code", "datum_pinned_by",
     * "datum_phase") and "excluded" (reason -> signal -> stations); a site carries its
     * name, group and the compared and disagreeing counts of the epoch and of the run.
     * With @c hdr.audited false, "week", "tow" and every site's "epoch" are null.
     *
     * @param plan      @ref signalRosterPlanOf of the epoch.
     * @param epoch     The site census of that epoch.
     * @param run       The cumulative site census.
     * @param hdr       The epoch and run the record describes.
     * @param scope_key The machine form of the scope: "first_emitted_epoch" or "run".
     * @return The record, without a trailing newline.
     */
    std::string signalRosterPlanJson(const RosterPlan& plan, const RosterSiteCounts& epoch,
                                     const RosterSiteCounts& run, const RosterPlanHeader& hdr,
                                     const std::string& scope_key);

    /**
     * @brief One phase-bias family's carried support over the run, as a histogram
     *        (census class (f); card NOM step N3a).
     *
     * Each epoch in which the family is carried adds one sample: the number of
     * stations whose modelled arc of it is alive in the ledger and present in
     * @c local_carry.  The run end reads two statistics of those samples, the median
     * and the number below a bar, and both are exact on the histogram, so the samples
     * themselves are not kept.  The memory is bounded by the largest station count
     * seen, not by the length of the run, which matters now that the census runs on
     * every network run.
     */
    struct RosterFamilySupport {
        /// Index n -> epochs in which exactly n stations carried the family.
        std::vector<long long> epochs_by_support;
        /// Total samples (the sum of @ref epochs_by_support).
        long long epochs{ 0 };

        /**
         * @brief Add one epoch's sample.
         * @param n Stations carrying the family at that epoch (negative is ignored).
         */
        void add(int n);

        /**
         * @brief Samples strictly below a bar.
         * @param bar The bar, in stations.
         * @return The number of epochs in which fewer than @p bar stations carried
         *         the family.
         */
        long long below(int bar) const;

        /**
         * @brief The median sample, as the sorted-vector median would give it: the
         *        middle sample of an odd count, the mean of the two middle samples of
         *        an even count, and 0 with no sample.
         * @return The median, in stations.
         */
        double median() const;
    };

    /**
     * @brief Run-scope state of the observatory: the epoch product plus the caches
     *        and census accumulators from which the run-end table is read.
     *
     * Lives in @c NetworkRunContext (@c ctx.roster), where carried state belongs on
     * this path.  Not snapshotted and not part of the warm-resume fingerprint, by
     * design: the solve reads none of it, so a resumed run rebuilds every cell at its
     * first post-resume epoch.
     */
    struct SignalRoster {
        /// Set up for this run by @ref networkSignalRosterArm, which every network run
        /// calls (card NOM step N3a); false only in a context no run set up, such as a
        /// unit test's, where every seam returns on its first line.
        bool      active{ false };
        /// @c network.signal_roster: write the arc ledger's trace
        /// (@c logs/network_arc_ledger.csv and the @c ARC event rows of the build and
        /// the audit).  Decides what is written, never what is built or counted.
        bool      trace{ false };
        long long epochs{ 0 };      ///< seam invocations; the processed-epoch clock of this card.

        // --- The "signals as planned" table (card NOM step N3a) -------------------
        /// The site census at the start of this epoch's build, so the audit can state
        /// the epoch's own counts (@ref signalRosterSiteDelta).
        RosterSiteCounts site_epoch_start{};
        /// The site census of the last epoch whose audit ran (the table's epoch column).
        RosterSiteCounts site_last_epoch{};
        /// The plan of that same epoch and its time, for the run-end table.  The last
        /// build of a run can belong to an epoch that ends before the datum seam and the
        /// audit, so the run-end table reads this snapshot rather than the last cells,
        /// and its band rows and epoch columns describe one epoch.
        RosterPlan plan_last{};
        /// Set at the first audit.  False at run end means no epoch reached the audit,
        /// so @ref plan_last, @ref site_last_epoch and the audit time describe nothing
        /// and the run-end table says so.
        bool   audited{ false };
        int    week_audit{ 0 };    ///< GPS week of the last epoch whose audit ran.
        double tow_audit{ 0.0 };   ///< its time of week (s).
        /// Set once the first-epoch table is out (after the first emitted epoch whose
        /// roster carries a modelled role).
        bool plan_table_written{ false };
        int    week_first{ 0 };    ///< GPS week of the first epoch the roster was built at.
        double tow_first{ 0.0 };   ///< its time of week (s).
        int    week_last{ 0 };     ///< GPS week of the latest epoch the roster was built at.
        double tow_last{ 0.0 };    ///< its time of week (s).

        /// station index -> this epoch's cells.  Rebuilt every epoch.
        std::vector<StationRoster> by_station;
        /// station index -> constellation -> its latch rows, cached once decided.
        std::vector<std::map<std::string, std::vector<RosterLatchRow>>> latch_cache;
        /// station index -> constellations whose count-against-priority comparison ran.
        std::vector<std::set<std::string>> rule_done;
        /// Stations at which the count rule and the priority rule pick differently on
        /// any (band, obs type).
        std::set<std::string> rule_differs;
        /// Stations at which the two rules pick a different band-1 phase: the
        /// population card CSL names, measured by the audit at six stations.
        std::set<std::string> rule_differs_b1;
        /// Stations at which the comparison ran at least once; the complement is the
        /// unreachable population.  Tracked as a positive set because the roster sees
        /// every constellation in the raw record, including ones the run does not
        /// process, and those would set a per-station negative flag.
        std::set<std::string> rule_reached;

        /// Carry-side presence memory: (station, satellite, signal) -> the last epoch
        /// at which the carry stashed a @c local_carry entry for it.  The audit's gap
        /// population is measured on the carried arcs (`network_amb_state.csv`), a
        /// strict subset of the raw tracked set that the ledger sees at its arrival
        /// site (upstream of the elevation mask, the allow list and every admission
        /// screen).  The two populations are counted separately and never combined.
        std::map<std::tuple<int, std::string, std::string>, long long> carry_last_ep;

        /// (station index, constellation, band digit, tracker signal) -> rows, for the
        /// run-end cell census.
        std::map<std::tuple<int, System, char, std::string>, long long> tracker_rows;
        /// (station index, constellation, band digit) -> the modelled phase of that cell
        /// (last seen).
        std::map<std::tuple<int, System, char>, std::string> modelled_of_cell;

        /// "sat|signal" -> the family's carried support over the run: per epoch in
        /// which the family is carried, the number of stations whose modelled arc of
        /// it is alive in the ledger and present in local_carry, held as a histogram
        /// (@ref RosterFamilySupport), so its size does not grow with the run.
        std::map<std::string, RosterFamilySupport> family_support;

        /// Anchor keys present in @c anchor_incumbent at the previous epoch's audit.
        std::set<std::string> anchor_prev;

        /// Names of the producer stations, in roster order (for the trace and events).
        std::vector<std::string> station_names;

        /// This epoch's realized per-satellite ionosphere-free pin, as
        /// @c NetworkSbasis::selectIfPinPair returned it (satellite -> the two code
        /// signals), recorded by the IF pin as a roster writer (phase 2).  Kept for the
        /// run-end census only; nothing reads it.  The per-cell designation it
        /// produces lives in @c BandRoster::datum_code / @c datum_phase.
        std::map<std::string, std::pair<std::string, std::string>> datum_pin;
        /// "<constellation>|<band digit>" -> code signal -> number of satellites that
        /// pinned it this epoch.  The pin is per satellite while a roster cell is per
        /// (station, constellation, band), so the designation written into the cells
        /// is the per-(constellation, band) mode over this epoch's pinned satellites,
        /// and a (constellation, band) with more than one pinned code is counted as
        /// ambiguous instead of being collapsed without notice.
        std::map<std::string, std::map<std::string, long long>> datum_votes;

        /// Resolved horizon inputs, reported at run end so the number is auditable.
        double horizon_s{ 0.0 };
        double horizon_pl_s{ 0.0 };
        double horizon_pbhold_s{ 0.0 };
        double horizon_readmit_s{ 0.0 };
    };

    /**
     * @brief Run-end counters of the observatory: one named class per disagreement.
     *
     * Lives in @c NetworkServiceCounters.  Every class is counted against a
     * denominator so that a zero can be interpreted: a zero with a zero denominator
     * is dead wiring, and the run-end log reports it as such.
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
        /// Self-check (P2, bar 0): cells whose Modelled role differs from the latch.
        /// The roster is built from the latch, so any nonzero value is a bug here.
        long long roster_latch_disagree{ 0 };
        /// Cells the self-check compared with the latch (its denominator; card NOM
        /// step N3a).  A cell of a station with no model is not compared.
        long long latch_cells_checked{ 0 };

        // --- (a) the arc-level tracker against modelled_phase ------------------
        long long trk_rows{ 0 };              ///< RawSlipObs band slots compared (denominator).
        long long trk_rows_mismatch{ 0 };     ///< of those, the tracker's signal is not the modelled one.
        long long trk_rows_no_modelled{ 0 };  ///< band slots whose band has no modelled phase (not comparable).
        long long trk_cells{ 0 };             ///< run-end: (station, constellation, band) cells with at least one row.
        long long trk_cells_mismatch{ 0 };    ///< run-end: cells whose most frequent tracker signal is not the modelled one.
        long long trk_cells_b1{ 0 };          ///< of those, on band digit 1.
        long long trk_cells_b2{ 0 };          ///< of those, on band digit 2.
        long long trk_stations_mismatch{ 0 }; ///< run-end: distinct stations with at least one mismatched cell.

        // --- (b) the count rule against the priority rule ----------------------
        long long rule_stations{ 0 };         ///< stations whose candidate list was reachable (denominator).
        long long rule_stations_differ{ 0 };  ///< of those, stations the two rules latch differently on any (band, obs type).
        long long rule_stations_differ_b1{ 0 };///< of those, stations whose band-1 phase differs (the card CSL population).
        long long rule_unreachable{ 0 };      ///< stations whose candidates were never retained.

        // --- (c) local_carry against the ledger --------------------------------
        long long lc_entries{ 0 };            ///< carry entries examined, summed over epochs (denominator).
        long long lc_arc_dead{ 0 };           ///< entries whose arc is Dead (the carry outlived the arc).
        long long lc_arc_unknown{ 0 };        ///< entries the ledger has never seen at all.
        long long lc_tracking_no_entry{ 0 };  ///< modelled arcs in Tracking with no carry entry.

        // --- (d) StateManager ambiguity presence against the ledger ------------
        long long amb_states{ 0 };            ///< ambiguity states examined, summed over epochs (denominator).
        long long amb_arc_gapped{ 0 };        ///< states alive while their arc is Gap (carried across a gap).
        long long amb_destroyed_in_gap{ 0 };  ///< arcs in Gap within the horizon whose state is gone.
        long long amb_arc_unknown{ 0 };       ///< ambiguity states the ledger has no arc for.

        // --- (e) the anchor election against the ledger ------------------------
        long long anchor_first_sightings{ 0 };    ///< keys that gained an incumbent with none before (denominator).
        long long anchor_first_sighting_reentry{ 0 }; ///< of those, whose arcs were Reentered or Gap: a return read as a first sighting.

        // --- (f) phase-bias family support -------------------------------------
        long long fam_families{ 0 };          ///< run-end: families with at least one alive epoch.
        long long fam_thin{ 0 };              ///< run-end: families below 3 carried stations in more than 10 per cent of their alive epochs.

        // --- (g) wire family coverage, from the emitter's own epoch -------------
        long long wire_sat_epochs{ 0 };       ///< satellite-epochs emitted with a clock record (denominator).
        std::map<std::string, long long> wire_pb_missing;  ///< signal -> satellite-epochs with a clock and no phase bias.
        std::map<std::string, long long> wire_cb_missing;  ///< signal -> satellite-epochs with a clock and no code bias.
        std::map<std::string, long long> wire_pb_present;  ///< signal -> satellite-epochs that carried one.
        /// signal -> satellite-epochs that carried a code bias for it (the code half's
        /// denominator beside @ref wire_cb_missing; card NOM step N3a).
        std::map<std::string, long long> wire_cb_present;

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
        long long gap_2_30{ 0 };              ///< raw-side re-entries after 2 to 30 missed epochs at an arc with a live carry.
        long long gap_2_30_answered{ 0 };     ///< of those, answered by the gap re-entry tier in the same epoch.
        long long gap_any{ 0 };               ///< raw-side re-entries after any number of missed epochs.
        /// Carry-side gap census: the audit's own population, presence in
        /// `network_amb_state.csv`, on which the card's figures were measured.
        long long carry_gap_any{ 0 };         ///< carry returns after at least one missed epoch.
        long long carry_gap_2_30{ 0 };        ///< of those, after 2 to 30 missed epochs.
        long long carry_gap_2_30_answered{ 0 };///< of those, answered by the gap re-entry tier at the return epoch.
        long long carry_entries_seen{ 0 };    ///< carry re-stashes walked (denominator of the census).

        // =================================================================
        // Phase 2 (network.roster_tracker, the IF pin writer, the admitted
        // lifecycle).  Every class below is reported beside its raw-side
        // counterpart and never added to it.
        // =================================================================

        // --- the tracker's roster read (invariant 4: whether the reader does the work)
        long long trk_roster_hits{ 0 };       ///< lookups answered by a roster cell's modelled phase.
        /// Lookups the roster could not answer and the station's latch could.  The
        /// roster is built upstream of computeModels, so on the first epoch of a
        /// constellation no modelled role exists yet; a count well above one epoch's
        /// worth of cells means the reader is not reading the roster.
        long long trk_roster_fallback{ 0 };
        long long trk_roster_none{ 0 };       ///< lookups neither could answer (a band with no latch at all).

        // --- the IF pin as a roster writer (observatory: nothing reads the fields)
        long long datum_epochs{ 0 };          ///< epochs the pin writer ran with a nonempty pin (denominator).
        long long datum_sat_pins{ 0 };        ///< per-satellite pins seen, summed over epochs.
        long long datum_cells{ 0 };           ///< roster cells given a datum code, summed over epochs.
        long long datum_cells_phase{ 0 };     ///< of those, that also carry a datum phase.
        /// Cells whose (constellation, band) had more than one pinned code across the
        /// epoch's satellites; the designation written is the mode.
        long long datum_cells_ambiguous{ 0 };
        /// Cells whose pinned code is not that station's modelled code, so no datum
        /// phase can be named without deriving one.
        long long datum_cells_off_model{ 0 };
        /// Of those, cells with no modelled code to compare with (the roster of a
        /// constellation's first epoch; card NOM step N3a, the IF pin site's
        /// denominator leaves them out).
        long long datum_cells_unmodelled{ 0 };
        /// "<constellation>|<band>|<signal>" -> cells the designation was written to,
        /// summed over epochs (the same unit as @ref datum_cells).
        std::map<std::string, long long> datum_code_census;
        std::map<std::string, long long> datum_phase_census;

        // --- the admitted lifecycle -------------------------------------------
        long long adm_rows{ 0 };              ///< admitted phase rows fed to the ledger, summed over epochs.
        long long adm_birth{ 0 };
        long long adm_gap{ 0 };
        long long adm_reentry{ 0 };
        long long adm_death{ 0 };
        long long adm_trace_rows{ 0 };        ///< admitted-side rows written to network_arc_ledger.csv.
        long long adm_live_max{ 0 };          ///< peak admitted-live arcs in one epoch.

        // --- (c) local_carry against the admitted lifecycle --------------------
        long long lc_adm_arc_dead{ 0 };           ///< carry entries whose admitted arc is Dead.
        long long lc_adm_arc_unknown{ 0 };        ///< carry entries the admitted side has never seen.
        long long lc_adm_tracking_no_entry{ 0 };  ///< modelled arcs admitted-Tracking with no carry entry.

        // --- (d) StateManager ambiguities against the admitted lifecycle -------
        long long amb_adm_arc_gapped{ 0 };        ///< states alive while their admitted arc is Gap.
        long long amb_adm_arc_unknown{ 0 };       ///< states with no admitted arc at all.
        long long amb_adm_destroyed_in_gap{ 0 };  ///< admitted arcs in a Gap within the horizon whose state is gone.

        // --- the carry-side gap census on the admitted lifecycle ---------------
        // The phase-1 census reads the carry's own re-stash stamp; this one reads
        // the admitted arc's re-entry edge.  Both are reported so the two
        // definitions can be compared, and neither replaces the other.
        long long carry_adm_gap_any{ 0 };         ///< admitted re-entries at an arc with a live carry.
        long long carry_adm_gap_2_30{ 0 };        ///< of those, after 2 to 30 missed epochs.
        long long carry_adm_gap_2_30_answered{ 0 };///< of those, answered by the gap re-entry tier at the return epoch.

        // =================================================================
        // Phase 3 (network.ledger_reentry): the prune and expiry edges, and the
        // unified answered split.  The first three classes are observation only and
        // follow the phase-1 knob; the split is reported with the phase-3 knob on
        // and off.
        // =================================================================

        /// `local_carry` entries the gap prune retired at `phase_leveling_max_gap_s`,
        /// each recorded as an `ARC arc_death reason=horizon` (R3a).
        long long carry_prune_deaths{ 0 };
        long long carry_prune_events{ 0 };    ///< of those, evented (holdoff-throttled).
        /// Ambiguity columns the state-vector prune destroyed while their admitted arc
        /// was in a Gap within the horizon.  This death path emits nothing else, so the
        /// ledger records it as a mark; the prune itself is unchanged (R3b).
        long long state_pruned_marks{ 0 };
        long long state_prune_keys{ 0 };      ///< columns the prune destroyed, all causes (denominator).
        long long state_prune_unknown{ 0 };   ///< of those, on an arc the ledger has never admitted.
        /// Admitted re-entries that landed on a re-created column (the arc carried
        /// a `state_pruned` mark through its gap).
        long long adm_state_recreated{ 0 };

        // --- the unified four-way answered split (R4) --------------------------
        // Population: admitted re-entries after 2 to 30 missed epochs.  Read from
        // the response's own record per arc (not inferred); the classes are
        // exhaustive.
        long long ans_total{ 0 };             ///< the population (denominator).
        long long ans_freed{ 0 };             ///< the response reached the arc and the mark landed.
        long long ans_bridged_v{ 0 };         ///< a raw tier saw the return, judged it clean, and bridged it by design.
        long long ans_bridged_nv{ 0 };        ///< nothing examined the return: bridged with no verdict.
        long long ans_no_carry{ 0 };          ///< a response was due but there was nothing to protect.
        long long ans_no_carry_entry{ 0 };    ///< the arc has no `local_carry` entry at all (outside the response's reach).

        // =================================================================
        // Phase 4a (network.ledger_screens): the reason-mask writers and the
        // anchor read.  Every class is observation only; phase 4b adds the two
        // restore counters at the end, reported with its knob on and off.
        // =================================================================

        /// Admitted gaps the fold stamped a reason on (denominator) (S1).
        long long gap_folded{ 0 };
        /// Of those, gaps whose mask carries more than one reason (S1).
        long long gap_multi{ 0 };
        /// Per reason index (@ref arcReasonIndex): admitted gaps it opened (S1).
        /// A gap with two reasons increments both, so these do not sum to
        /// @ref gap_folded; the run-end table states this.
        std::vector<long long> gap_by_reason{ std::vector<long long>(kArcReasonCount, 0) };
        /// Per reason index: writer calls, i.e. (arc, screen) records made (S1).
        std::vector<long long> writer_calls{ std::vector<long long>(kArcReasonCount, 0) };
        /// Per reason index: writer invocations that reached no admitted arc (S1).
        std::vector<long long> writer_unknown{ std::vector<long long>(kArcReasonCount, 0) };
        /// The phase-3 no-verdict population split by the reason mask of the gap that
        /// preceded the return (S1).  Same indices; a return whose gap mask is empty
        /// (no fold ran, e.g. the knob was armed mid-lineage) is counted in
        /// @ref nv_no_mask.
        std::vector<long long> nv_by_reason{ std::vector<long long>(kArcReasonCount, 0) };
        long long nv_total{ 0 };              ///< the no-verdict population seen at the split.
        long long nv_no_mask{ 0 };            ///< of those, with no reason mask at all.

        // --- the anchor election read against the ledger (S2) -------------
        long long anchor_fs{ 0 };             ///< first sightings the election made (denominator).
        long long anchor_fs_return{ 0 };      ///< of those, whose family had arcs in Reentered or Gap.
        long long anchor_fs_same_gen{ 0 };    ///< of those, a same-generation return of a dropped family.
        long long anchor_fs_new_gen{ 0 };     ///< of those, a return with new generations.
        long long anchor_fs_events{ 0 };      ///< ARC anchor_first_sighting_on_return rows written.
        /// Coverage-gate drop edges seen by this class (denominator; the same edges
        /// class (h) counts, read here with the generation test as well).
        long long anchor_drop_edges{ 0 };
        long long anchor_drop_arcs_tracking{ 0 }; ///< arcs Tracking at the edges, summed.
        long long anchor_drop_arcs_gap{ 0 };      ///< arcs Gap or Reentered at the edges, summed.
        long long anchor_drop_arcs_dead{ 0 };     ///< arcs Dead at the edges, summed.
        /// `anchor_arc_age` against the ledger's admitted age (S2).  The container
        /// exists only while network.eq34_anchor_min_arc_ep, the pin bridge or the
        /// fresh-pin ramp is armed, so a zero denominator here means the hygiene seam
        /// never ran and the class is dead wiring, not a clean agreement.
        long long age_compared{ 0 };
        long long age_disagree{ 0 };
        long long age_no_ledger{ 0 };         ///< arcs the ledger could give no admitted age for.

        // --- the behaviour half (B1 / B2), reported with the knob on and off ----
        long long anchor_restore{ 0 };        ///< returns where the previous incumbent was restored.
        long long anchor_restore_refused{ 0 };///< same-generation returns the election rules refused.
        long long anchor_reentry_new{ 0 };    ///< returns that proceeded with new generations, as without the knob.
        long long age_from_ledger{ 0 };       ///< arc ages taken from the ledger instead of the counter.

        double    build_ms{ 0.0 };            ///< wall time inside the build seam, summed.
        double    audit_ms{ 0.0 };            ///< wall time inside the audit seam, summed.
    };

    /**
     * @brief The cumulative census at every decision site, read off the counters.  Pure.
     *
     * One place states which counter is each site's denominator and which is its
     * disagreement, so the table, the manifest record and the unit suite read the
     * same definition (the texts are @ref signalRosterSiteSpecs):
     *
     *  - canonical latch: @c latch_cells_checked, @c roster_latch_disagree;
     *  - latch rule: stations compared (@c SignalRoster::rule_reached), stations whose
     *    two rules differ (@c SignalRoster::rule_differs);
     *  - slip tracker band: @c trk_rows, @c trk_rows_mismatch;
     *  - IF pin pair: @c datum_cells, @c datum_cells_off_model, each less
     *    @c datum_cells_unmodelled (a cell with no modelled code is not comparable);
     *  - wire phase bias and code bias: present plus missing, missing (summed over
     *    signals);
     *  - local carry: @c lc_entries, @c lc_adm_arc_dead + @c lc_adm_arc_unknown;
     *  - ambiguity states: @c amb_states, @c amb_adm_arc_gapped + @c amb_adm_arc_unknown;
     *  - anchor election: @c anchor_first_sightings, @c anchor_first_sighting_reentry;
     *  - gap re-entry: @c ans_total, @c ans_bridged_nv;
     *  - coverage gate: @c gate_drop_edges, @c gate_drop_edges_gapped.
     *
     * @param c  The census counters.
     * @param rs The roster (the latch-rule station sets live there).
     * @return The cumulative compared and disagreeing counts, one per site.
     */
    RosterSiteCounts signalRosterSiteCounts(const NetworkSignalRosterCounters& c,
                                            const SignalRoster& rs);

    /**
     * @brief Set the roster and the arc ledger up for this run (once, at run start;
     *        every network run calls it).
     *
     * Resolves the ledger's horizon as the maximum of the readers' gap windows
     * (@c network.phase_leveling_max_gap_s, @c network.emit_pb_hold_max_gap_s,
     * @c network.readmit_seed_horizon_s), sizes
     * the per-station caches, records the station names, and reads
     * @c network.signal_roster as the trace switch.
     *
     * @param ctx      Run context; @c ctx.roster and @c ctx.arc_ledger are filled in.
     * @param config_  The run's config.
     * @param log      Producer logger (one line stating what is built and whether the
     *                 trace is written).
     * @param stations The producer roster.  The held-out consumer is not on it.
     */
    void networkSignalRosterArm(NetworkRunContext& ctx, const GnssConfig& config_,
                                const std::shared_ptr<spdlog::logger>& log,
                                const std::vector<NetStation>& stations);

    /**
     * @brief Build this epoch's roster and advance the arc ledger (the single call
     *        site per epoch).
     *
     * Runs on every network epoch.  It also opens the epoch's decision-site census
     * (@c SignalRoster::site_epoch_start).  The trace rows are written only under
     * @c network.signal_roster; the ledger and the counters advance either way.
     *
     * @param ctx      Run context (the roster, the ledger and the counters).
     * @param config_  The run's config.
     * @param log      Producer logger.
     * @param traces   Trace bundle; the @c ARC event rows and the per-transition CSV.
     * @param se       The epoch as it reaches the seam (post canonical-pair admission).
     * @param stations The producer roster and the source of each station's latch.
     */
    void networkBuildSignalRoster(NetworkRunContext& ctx, const GnssConfig& config_,
                                  const std::shared_ptr<spdlog::logger>& log,
                                  NetworkRunTraces& traces,
                                  const SyncedEpoch& se,
                                  const std::vector<NetStation>& stations);

    /**
     * @brief The roster's answer to "which phase represents this band?" (phase 2).
     *
     * The read side of @c network.roster_tracker.  Pure and const: looks the cell up
     * and returns its @c modelled_phase, or an empty string when the roster has no
     * answer (the station reported nothing, the constellation has no priority list,
     * the band has no latch, or this is the first epoch of a constellation, whose
     * latch is decided inside @c computeModels downstream of the build seam).  The
     * caller decides what an empty answer means.
     *
     * @param rs      The run's roster (@c ctx.roster).
     * @param station Producer station index.
     * @param cons    Constellation name, as @c systemName gives it ("GPS").
     * @param band    RINEX band digit.
     */
    std::string signalRosterModelledPhase(const SignalRoster& rs, int station,
                                          const std::string& cons, char band);

    /**
     * @brief The admitted phase arcs of this epoch.  Pure function.
     *
     * @c modeled[i] exists only for satellites that passed the elevation mask, and
     * @c PppObservation::observed holds only the signals the model built (the
     * canonical latch, its card SCC companions and whatever the allow list admitted),
     * which is the set that @c local_carry and the ambiguity states are keyed from.
     * The roster's admitted lifecycle and the card SID phase 3 re-entry population
     * both use this one definition, so the two cannot diverge.
     *
     * @param modeled Per-station modelled observations of this epoch.
     * @param present Per-station PASS-A presence.
     * @param out     Receives the (station, satellite, signal) phase arcs.  Cleared first.
     */
    void signalRosterAdmittedArcs(
        const std::vector<std::vector<PppObservation>>& modeled,
        const std::vector<bool>& present,
        std::set<ArcLedger::Key>& out);

    /**
     * @brief The admitted arcs whose signal the roster calls Modelled.  Pure
     *        function (card SID phase 3).
     *
     * @ref signalRosterAdmittedArcs restricted by the roster's role.  This is the
     * population the re-entry response is defined on: the response frees a carried
     * level, and only a modelled signal has one.  An arc whose cell the roster cannot
     * answer for (a constellation with no priority list, or the first epoch of a
     * constellation, whose latch is decided inside @c computeModels downstream of the
     * build seam) is rejected and counted, not admitted.
     *
     * @param rs             The run's roster.
     * @param modeled        Per-station modelled observations of this epoch.
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
        long long cells{ 0 };      ///< cells given a datum code.
        long long with_phase{ 0 }; ///< of those, that also carry a datum phase.
        long long off_model{ 0 };  ///< of those, whose pinned code is not the modelled one.
        /// Of @ref off_model, cells with no modelled code at all (a constellation's first
        /// epoch, built before its latch is decided): nothing to compare the pin with.
        long long unmodelled{ 0 };
        long long ambiguous{ 0 };  ///< of those, on a (constellation, band) with more than one pinned code.
    };

    /**
     * @brief Resolve the per-satellite IF pins to one code designation per
     *        (constellation, band digit).  Pure function (phase 2).
     *
     * The constellation key is the roster's: the system name @c systemName gives
     * ("GPS"), not the letter ("G") by which the datum seam keys its own maps.
     *
     * @param if_pin_real Satellite -> the two realized IF-pin code signals.
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
     * @brief Write the datum designation into one station's cells.  Pure function
     *        (phase 2).
     *
     * @param sr        The station's roster; modified in place.
     * @param picks     @ref signalRosterDatumPicks output.
     * @param ambiguous Optional: the non-unanimous set, counted and not acted on.
     */
    SignalRosterDatumWrite signalRosterApplyDatum(
        StationRoster& sr, const std::map<std::string, std::string>& picks,
        const std::set<std::string>* ambiguous = nullptr);

    /**
     * @brief The IF pin pair election as a roster writer (phase 2).
     *
     * Called once per epoch directly after @c NetworkSbasis::buildCodeBiasDatumRows
     * returns its realized pins; that is the only site that knows which signals the
     * S-basis pinned this epoch.
     *
     * Granularity differs from the roster's: the pin is elected per satellite over
     * the network's registered code keys and names two code signals, while a roster
     * cell is per (station, constellation, band).  The designation written into the
     * cells is therefore the per-(constellation, band) mode over the epoch's pinned
     * satellites, and a (constellation, band) whose satellites did not agree is
     * counted as ambiguous.  @c datum_phase is filled only where the pinned code is
     * that station's modelled code, in which case the phase it accompanies is that
     * cell's modelled phase; anywhere else it stays empty instead of being derived
     * from the code's attribute.
     *
     * Observation only: nothing reads @c BandRoster::datum_phase or @c datum_code.
     * The fields are rebuilt with the roster at the next epoch, so the census is
     * accumulated here.
     *
     * @param ctx         Run context (the roster and the counters).
     * @param if_pin_real Satellite -> the two realized IF-pin code signals.
     */
    void networkSignalRosterDatumPin(
        NetworkRunContext& ctx,
        const std::map<std::string, std::pair<std::string, std::string>>& if_pin_real);

    /**
     * @brief The disagreement pass: counts every class of P4 and does nothing else.
     *
     * Runs late in the epoch, where each input exists: the tracker's rows are built
     * in PASS A, @c local_carry is post-harvest, the ambiguity states are
     * post-assembly, the anchor incumbents are post-election and the solution epoch
     * is post-emit.  Read only; changes nothing.
     *
     * Phase 2 adds the admitted lifecycle here, ahead of every class that reads it:
     * each station's modelled phase rows (@c PppObservation::observed, which is what
     * passes the elevation mask, the allow list and the admission screens, and the
     * same source @c local_carry and the ambiguity states are keyed from) advance a
     * second state machine on each arc.  Classes (c) and (d) are then reported on
     * both sides, and the carry-side gap census gains an admitted-lifecycle reading
     * beside its re-stash-stamp reading.
     *
     * Card NOM step N3a closes the epoch's decision-site census here
     * (@c SignalRoster::site_last_epoch), and after the first emitted epoch whose
     * roster carries a modelled role it logs the "signals as planned" table and
     * appends its record to the run manifest.  That is the only output it writes
     * outside the counters and the trace.
     *
     * @param ctx       Run context.
     * @param config_   The run's config (the run manifest switch).
     * @param log       Producer logger (the first-epoch table).
     * @param traces    Trace bundle (the admitted-side rows of network_arc_ledger.csv,
     *                  and the run manifest).
     * @param se        This epoch.
     * @param stations  The producer roster.
     * @param modeled   Per-station modelled observations of this epoch: the admitted
     *                  row set that feeds the admitted lifecycle.
     * @param present   Per-station PASS-A presence.
     * @param raw       This epoch's raw slip observables, or nullptr when the
     *                  tracker did not run (the class then reports a zero
     *                  denominator instead of a zero count).
     * @param freed     Ambiguity keys freed this epoch, as (station index, key).
     * @param sol       The emitted solution epoch.
     * @param emitted   True when this epoch reached the wire.
     */
    void networkSignalRosterAudit(NetworkRunContext& ctx, const GnssConfig& config_,
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
     * @brief Count one coverage-gate drop edge against the ledger (class h).
     *
     * Called from the driver's existing @c gate_drop.events loop, so the edge is
     * counted where it is already evented.  No-op in a context no run set up.
     *
     * @param ctx    Run context.
     * @param sat    Satellite of the dropped family.
     * @param signal Signal of the dropped family.
     */
    void networkSignalRosterGateDrop(NetworkRunContext& ctx, const std::string& sat,
                                     const std::string& signal);

    /**
     * @brief Fold the election's ledger read (card SID phase 4, S2 / B1).
     *
     * Called from the driver after @c networkElectPhaseAnchors returns and its
     * @c gate_drop.events loop has run, where the per-epoch @ref AnchorReentry result
     * is in scope and every other election readback is already folded.  Counts the
     * first sightings the ledger identifies as returns, emits their identity, and
     * reports phase 4b's restore decisions.  No-op when the read is not armed.
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
     * @brief Run-end "signals as planned" table (card NOM step N3a): the band rows
     *        and the epoch columns of the last audited epoch, the run columns of every
     *        epoch the roster was built at, logged and appended to the run manifest
     *        as a @c signals_as_planned record.  Nothing in a context no run set up.
     *
     * Called by the teardown beside the "parameters as armed" and "contributions as
     * armed" tables, before the run manifest's @c complete record, so that record
     * stays the manifest's last.  With no audited epoch the table says so
     * (@ref RosterPlanHeader::audited).
     *
     * @param ctx     Run context (counters, roster state and ledger).
     * @param config_ The run's config (the run manifest switch, the latch rule).
     * @param log     Producer logger.
     * @param traces  Trace bundle (the run manifest).
     */
    void networkSignalRosterPlanRunEnd(const NetworkRunContext& ctx, const GnssConfig& config_,
                                       const std::shared_ptr<spdlog::logger>& log,
                                       NetworkRunTraces& traces);

    /**
     * @brief Run-end readout: the census table of every class, with its denominators
     *        and the arc ledger's transitions, horizon and cost.  Nothing in a context
     *        no run set up.
     *
     * A count that only the ledger's trace switch (@c network.signal_roster) moves is
     * printed only with the trace on; with it off the line says that nothing was
     * written, so no trace-only zero reads as dead wiring.
     *
     * @param ctx Run context (counters, roster state and ledger).
     * @param log Producer logger.
     */
    void networkSignalRosterRunEnd(const NetworkRunContext& ctx,
                                   const std::shared_ptr<spdlog::logger>& log);

    // ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

    struct NetworkRunEndView;

    /**
     * @brief Run-end report: signals as planned (NOM N3a).
     *
     * The table's row for "signals as planned" (card NOM N3a) and its manifest record; the
     * report is the overload above.
     *
     * Gate: none of its own: the row calls the report above on every run, as the teardown
     * did, and that report applies its own.
     *
     * @param v The run at its end (filter/NetworkRunEnd.hpp).
     * @return 0 (this report does not set the exit code).
     */
    int networkSignalRosterPlanRunEnd(const NetworkRunEndView& v);

    /**
     * @brief Run-end report: the signal roster and the arc ledger (SID phase 1).
     *
     * The table's row for the signal roster's run end (SID phase 1); the report is the
     * overload above.
     *
     * Gate: none of its own: the row calls the report above on every run, as the teardown
     * did, and that report applies its own.
     *
     * @param v The run at its end (filter/NetworkRunEnd.hpp).
     * @return 0 (this report does not set the exit code).
     */
    int networkSignalRosterRunEnd(const NetworkRunEndView& v);

    // ---- End of the run-end reports ----

} // namespace GnssProcessor
