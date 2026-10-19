#pragma once
/**
 * @file ArcLedger.hpp
 * @brief Arc lifecycle state machine: one record per (station, satellite, signal),
 *        owned by @c NetworkRunContext.
 *
 * ### Purpose
 *
 * Of the 29 arc-keyed containers in @c RunContext.hpp, 5 are keyed at the
 * signal level, and 31 lifecycle holders emit nothing at their birth and death
 * edges.  Among them, @c pruneAmbiguitiesExcept destroys an
 * ambiguity after one missed epoch, with no window and no event, and the gap prune
 * of @c local_carry is silent.  The lifecycle vocabulary exists in three places
 * (@c adm_arc, @c pb_hold, @c gl_state) in three incompatible key spaces, and is
 * absent from the one container the producer depends on.  Audit:
 * `runs/_msc/sid_audit_report_2026-09-22.md`, Part B.
 *
 * This ledger is one state machine those containers can read.  It is built on
 * every network run; the solve reads it only through a signal-identity
 * reader armed by its own knob, so with none armed it counts and traces and the
 * solve is unchanged.
 *
 * ### The machine
 *
 * @verbatim
 *   Absent ---- present ----> Tracking          (birth, generation += 1)
 *   Tracking -- missed  ----> Gap               (gap opens)
 *   Gap ------- present ----> Reentered         (inside the horizon)
 *   Reentered - present ----> Tracking          (one epoch later, silently)
 *   Gap ------- horizon ----> Dead              (reason mask records why)
 *   Dead ------ present ----> Tracking          (birth, generation += 1)
 *   Dead ------ forget  ----> record reset (the arc's row stays)
 * @endverbatim
 *
 * ### Where the records live
 *
 * The ledger is the machine; the records are the arc table's.  Each record is the
 * @c ledger field of its arc's row (@c net::ArcState, network/state/Entities.hpp), beside
 * the arc's leveling carry and its other carried fields, so a reader that needs an
 * arc's lifecycle and its carry reads one row.  The ledger is bound to the run's
 * entities at construction and keys its calls by the (station index, satellite,
 * signal) tuple the roster speaks, converted to the arc's identity (a key that does
 * not round-trip is counted by the reach table and never stored).  Rows are never
 * erased (decision D8): the forget sweep resets the record and keeps the row, so the
 * row's @c seen lifecycle remembers an arc the ledger has forgotten, while the ledger
 * still reports a return after its forget window as a first sighting.
 *
 * The horizon is a time, not an epoch count, because the reader windows it covers
 * are all configured in seconds (@c network.phase_leveling_max_gap_s,
 * @c network.emit_pb_hold_max_gap_s, @c network.readmit_seed_horizon_s); a horizon
 * counted in epochs would change
 * meaning with an epoch interval other than 30 s.  The gap length is reported in
 * epochs, the unit of the existing re-entry instruments.
 *
 * The forget window is an epoch count, following the @c kAdmArcForgetEp precedent
 * (@c MeasurementAdmission.cpp).  A dead record is kept that long so a late
 * return is distinguishable from a first sighting; the anchor election does not
 * make that distinction.
 *
 * ### Absent stations
 *
 * An arc whose station reported nothing this epoch is left frozen, never gapped.
 * Otherwise a station outage would open a gap at every one of its arcs, while the
 * population of interest is the arc that vanished while its station kept
 * reporting.  @c SignalIntegrity.cpp applies the same rule through its
 * per-epoch @c seen set.
 *
 * ### Scale
 *
 * About 1 200 live records per epoch at 58 stations and about 6 000 at 305.  The
 * per-transition event stream is therefore holdoff-throttled per (station,
 * transition), and birth events are suppressed on the first epoch.  The
 * per-transition trace (`logs/network_arc_ledger.csv`) is not throttled and holds
 * every edge.  Each trace row names its side: raw, adm, or carry (a carry row is
 * written when the carry's gap prune retires an arc).
 */

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "common/GnssIdentity.hpp"   // ArcId (the record table's key)

namespace GnssProcessor {
    namespace net {
        struct NetworkEntities;   // network/state/Entities.hpp: the run's tables
        struct ArcState;          // network/state/Entities.hpp: an arc's row (its ledger field)
    }

    /**
     * @brief The five lifecycle states of one (station, satellite, signal) arc.
     *
     * @c Absent is never stored: a record exists only from its first sighting on,
     * and the value is the "before the first sighting" reading a step reports as
     * its @c from state.
     */
    enum class ArcState {
        Absent,     ///< never seen, or forgotten after death.
        Tracking,   ///< present at this epoch and at the previous one.
        Gap,        ///< missed at least one epoch, still inside the horizon.
        Reentered,  ///< returned this epoch after a gap (one epoch wide).
        Dead        ///< the gap outran the horizon; kept for the forget window.
    };

    /// @brief The four transitions the ledger reports (plus @c None for no edge).
    enum class ArcTransition {
        None,     ///< the state did not change in a way worth reporting.
        Birth,    ///< Absent or Dead to Tracking.
        Gap,      ///< Tracking or Reentered to Gap.
        Reentry,  ///< Gap to Reentered, inside the horizon.
        Death     ///< Gap to Dead, past the horizon.
    };

    /**
     * @brief Why an arc left, as a bit mask on @c ArcRecord::reason_mask (raw
     *        side, where only @ref kArcReasonHorizon is written) and on
     *        @c ArcRecord::adm_reason_mask (admitted side).
     *
     * Every screen that removes a modelled row, a satellite or a station records
     * the arcs it removed, with its own bit, in the per-epoch @ref ArcReasonEpoch
     * buffer, and the fold in @ref ArcLedger::admitEpoch stamps that mask on the
     * admitted gap the removal opened.  An admitted gap
     * that no writer explains carries @ref kArcReasonUnknown; that bit counts the
     * phase-3 no-verdict returns that remain unexplained.
     */
    enum ArcDeathReason : unsigned {
        kArcReasonNone      = 0u,       ///< no reason recorded.
        kArcReasonHorizon   = 1u << 0,  ///< the gap outran the readers' longest window.
        kArcReasonMask      = 1u << 1,  ///< the elevation mask refused the LOS.
        kArcReasonSlip      = 1u << 2,  ///< a slip tier erased or refloated the arc.
        kArcReasonGate      = 1u << 3,  ///< the coverage gate dropped the family's column.
        kArcReasonPrune     = 1u << 4,  ///< the state vector pruned the column.
        kArcReasonAllowlist = 1u << 5,  ///< the network signal allow list excluded the signal.
        kArcReasonAdmission = 1u << 6,  ///< the measurement-admission / DW screen withheld the band.
        kArcReasonReadmit   = 1u << 7,  ///< the readmit horizon parked the satellite's carries.
        kArcReasonGross     = 1u << 8,  ///< the gross-LOS screen removed the arc or its satellite.
        kArcReasonDeweight  = 1u << 9,  ///< a de-weight, not a removal; excluded from every gap mask.
        kArcReasonNoModel   = 1u << 10, ///< the model built no row (no product, no usable pair, eclipsing).
        kArcReasonUnknown   = 1u << 11  ///< an admitted gap opened and no writer fired.
    };

    /// @brief Number of reason bits, for the per-reason counter arrays.
    inline constexpr int kArcReasonCount = 12;

    /**
     * @brief Bits that may explain an admitted gap.
     *
     * @ref kArcReasonDeweight is excluded by construction: a de-weight leaves the
     * row in the solve, so counting it as a gap opener would attribute a removal
     * to a screen that removed nothing.  @ref
     * kArcReasonUnknown is excluded because the fold writes it when this mask
     * comes out empty.
     */
    inline constexpr unsigned kArcReasonGapMask =
        kArcReasonHorizon | kArcReasonMask | kArcReasonSlip | kArcReasonGate
        | kArcReasonPrune | kArcReasonAllowlist | kArcReasonAdmission
        | kArcReasonReadmit | kArcReasonGross | kArcReasonNoModel;

    /**
     * @brief Bits whose writer fires after the epoch's fold.
     *
     * The readmit park runs inside @c networkStashStatus, downstream of the
     * roster's audit pass, so its record cannot reach the fold of its own epoch.
     * These bits alone survive one @ref arcReasonBeginEpoch, so the gap they can
     * open (at the next epoch at the earliest, since the satellite has already
     * departed) still finds them.  Every other bit is cleared, so an early writer
     * cannot attribute a gap it did not open.
     */
    inline constexpr unsigned kArcReasonLateMask = kArcReasonReadmit;

    /// @brief Short name of one reason bit, for the run-end table and the events.
    const char* arcReasonName(unsigned bit);

    /// @brief Index of one reason bit in a @ref kArcReasonCount array, or -1.
    int arcReasonIndex(unsigned bit);

    /// @brief '|'-joined names of every bit in @p mask ("-" when empty).
    std::string arcReasonMaskName(unsigned mask);

    /// @brief Human-readable name of a state, for the trace and the events.
    const char* arcStateName(ArcState s);

    /// @brief Human-readable name of a transition, for the trace and the events.
    const char* arcTransitionName(ArcTransition t);

    /**
     * @brief One arc's whole lifecycle memory.
     *
     * Epoch indices are the ledger's own processed-epoch clock (one increment per
     * @c ArcLedger::beginEpoch), never a downstream seam's: a seam that skips an
     * epoch would stop its clock at the epoch where an arc dropped out.
     */
    struct ArcRecord {
        ArcState  state{ ArcState::Absent };  ///< current state.
        long long ep_first_seen{ -1 };        ///< epoch of the first sighting ever (survives a death).
        long long ep_last_seen{ -1 };         ///< epoch of the most recent sighting.
        long long ep_state_changed{ -1 };     ///< epoch the current state was entered.
        long long ep_gap_started{ -1 };       ///< epoch the current or last gap opened.
        double    t_last_seen_s{ 0.0 };       ///< continuous GPS seconds of the most recent sighting.
        int       gap_len_ep{ 0 };            ///< epochs missed by the current or last gap; 0 while Tracking.
        int       generation{ 0 };            ///< increments at every birth; 1 at the first sighting.
        bool      modelled{ false };          ///< the roster gave this signal the Modelled role at the last sighting.
        unsigned  reason_mask{ kArcReasonNone }; ///< why the arc last left (see @ref ArcDeathReason).

        // --- Admitted lifecycle, kept beside the raw one -----
        //
        // The two populations differ: the raw side is observed upstream of the
        // elevation mask, the allow list and every admission screen, while
        // `local_carry` and the ambiguity states are keyed from the station's
        // modelled rows.  The two sides are reported separately and never summed;
        // a reader that keys on the ledger (phase 3) reads this side.
        ArcState  adm_state{ ArcState::Absent };    ///< current admitted-side state.
        long long ep_first_admitted{ -1 };          ///< epoch of the first admitted row ever.
        long long ep_last_admitted{ -1 };           ///< epoch of the most recent admitted row.
        long long ep_adm_state_changed{ -1 };       ///< epoch @ref adm_state was entered.
        double    t_last_admitted_s{ 0.0 };         ///< continuous GPS seconds of the most recent admitted row.
        int       adm_gap_len_ep{ 0 };              ///< epochs missed by the current or last admitted gap.
        int       adm_generation{ 0 };              ///< increments at every admitted birth.
        unsigned  adm_reason_mask{ kArcReasonNone };///< why the admitted side last left.

        // --- State-vector prune, recorded as a mark ----------
        //
        // @c pruneAmbiguitiesExcept destroys an ambiguity column
        // after one missed epoch, with no window and no event; the audit found it
        // the largest silent death path.  The prune itself is unchanged (the state
        // vector has to shrink); these fields record that it happened, so the
        // arc's next admitted re-entry or birth can report that its column was
        // re-created rather than carried.  Nothing in the solve reads them.
        bool      state_pruned{ false };            ///< the column was pruned while this arc was in an admitted Gap inside the horizon.
        long long ep_state_pruned{ -1 };            ///< epoch of that prune.
        long long n_state_pruned{ 0 };              ///< how often it has happened to this arc.

        /**
         * @brief Epochs the admitted side has missed as of @p ep.
         *
         * Returns -1 when the arc has never been admitted.  That is a different
         * statement from "admitted and gapped for 0 epochs" and stays separable,
         * because the raw side sees satellites the estimator never models.
         */
        long long admittedGapEp(long long ep) const {
            return ep_last_admitted < 0 ? -1 : ep - ep_last_admitted - 1;
        }
    };

    /**
     * @brief One step of the machine: the edge crossed and its identity.
     *
     * A step describes one side.  @ref transition, @ref from, @ref to,
     * @ref gap_len_ep and @ref generation describe the raw side when the step came
     * from @c observe or @c endEpoch, and the admitted side when it came from
     * @c admitEpoch.  @ref adm_state and @ref adm_gap_len_ep always report the
     * admitted side as it stands when the step is taken (on a raw step, before
     * this epoch's admitted pass has run), so a raw re-entry carries both gap
     * lengths without a second lookup.
     */
    struct ArcStep {
        ArcTransition transition{ ArcTransition::None };
        ArcState  from{ ArcState::Absent };   ///< state before the step.
        ArcState  to{ ArcState::Absent };     ///< state after the step.
        int       gap_len_ep{ 0 };            ///< epochs missed (meaningful at Reentry and Death).
        int       generation{ 0 };            ///< the generation after the step.
        int       prev_generation{ 0 };       ///< the generation before a Birth (0 = a true first sighting).
        unsigned  reason_mask{ kArcReasonNone };
        ArcState  adm_state{ ArcState::Absent }; ///< the admitted state after the step.
        /// Epochs the admitted side has missed as of this epoch; -1 when the arc has
        /// never been admitted.
        long long adm_gap_len_ep{ -1 };
        int       adm_generation{ 0 };        ///< the admitted generation after the step.
        /// The ambiguity column of this arc was pruned during the gap this step
        /// closes, so a re-entry returns to a fresh column.
        bool      state_recreated{ false };
    };

    /**
     * @brief One arc of the ledger-driven re-entry population.
     *
     * The population uses the same (station, satellite, signal) key as the
     * response, so the population and the action cannot disagree on the key
     * (the gap re-entry key mismatch).
     */
    struct ArcReentryArc {
        int         station{ -1 };
        std::string satellite;
        std::string signal;
        int         gap_ep{ 0 };            ///< admitted epochs missed.
        double      gap_s{ 0.0 };           ///< seconds since the last admitted row.
        bool        state_recreated{ false };///< the column was pruned inside the gap.
    };

    /**
     * @brief This epoch's ledger-driven re-entry population.
     *
     * Built once per epoch at the PASS-A slip site and handed to both re-entry
     * drains.  With @ref armed false every drain stays on its raw-driven path and
     * the output is byte-identical.
     */
    struct ArcReentryEpoch {
        bool armed{ false };                    ///< network.ledger_reentry.
        bool refloat_no_verdict{ false };       ///< network.ledger_reentry_no_verdict == refloat.
        std::vector<ArcReentryArc> arcs;        ///< the population, in key order.
        long long outside_horizon{ 0 };         ///< admitted returns past phase_leveling_max_gap_s (not in @ref arcs).
        long long not_modelled{ 0 };            ///< admitted returns whose signal the roster does not call Modelled.
    };

    /// @brief The per-epoch reason buffer, defined below the class.
    struct ArcReasonEpoch;

    /**
     * @brief The forget window, in processed epochs, following the
     *        @c kAdmArcForgetEp precedent of @c MeasurementAdmission.cpp.
     *
     * A dead record is retained this long so that a return after it is still
     * reported with its previous generation rather than as a first sighting.
     */
    inline constexpr long long kArcLedgerForgetEp = 2880;

    /**
     * @brief Per (station, transition) event holdoff, in processed epochs.
     *
     * Bounds the @c ARC event stream at 305 stations, where an unthrottled birth
     * edge would write about 12 000 rows on the first epoch alone (the figure
     * @c SignalIntegrity.cpp gives for suppressing its own birth event).
     * The per-transition trace is not throttled.
     */
    inline constexpr long long kArcLedgerEventHoldoffEp = 30;

    /**
     * @brief The ledger: every arc of the run, and the machine that advances them.
     *
     * Held by @c NetworkRunContext (@c ctx.arc_ledger), bound to
     * @c ctx.entities: the records are the @c ledger fields of the arc table's rows
     * (see the file header).  Not part of the warm-resume fingerprint and not
     * snapshotted: the solve reads no value from it, so a resumed run re-seats every
     * arc at its first post-resume sighting, as a fresh observer would.
     */
    class ArcLedger {
    public:
        /// (station index, satellite, signal).
        using Key = std::tuple<int, std::string, std::string>;

        /// The table the records live in: the run's arc table, in (station, satellite,
        /// signal) order, which is the order of @ref Key (O1).
        using Table = std::map<ArcId, net::ArcState>;

        /**
         * @brief Binds the ledger to the entities whose arc table holds its records.
         * @param ents The run's entities (the arc table, and the converter tally a key
         *             that does not round-trip is counted in).  Must outlive the ledger,
         *             and stay where it is: a copy of the ledger shares the same table.
         */
        explicit ArcLedger(net::NetworkEntities& ents) : ents_(&ents) {}

        /**
         * @brief Set the two windows the machine runs on.
         *
         * @param horizon_s Longest reader gap window, in seconds; a gap that
         *                  outruns it is a death.  Values at or below zero
         *                  disable the death edge (every gap stays a gap).
         * @param forget_ep Epochs a dead record is retained (see
         *                  @ref kArcLedgerForgetEp).
         */
        void configure(double horizon_s, long long forget_ep);

        /// @brief The configured horizon, in seconds.
        double horizonS() const { return horizon_s_; }

        /// @brief The configured forget window, in processed epochs.
        long long forgetEp() const { return forget_ep_; }

        /**
         * @brief Open one processed epoch.
         *
         * @param epoch_idx Processed-epoch index (the ledger's own clock), strictly
         *                  increasing from one call to the next: an arc observed or
         *                  admitted at the open epoch is recognized by its
         *                  @c ep_last_seen / @c ep_last_admitted stamp equalling it.
         * @param t_s       Continuous GPS seconds of the epoch.
         */
        void beginEpoch(long long epoch_idx, double t_s);

        /// @brief The epoch index of the epoch currently open.
        long long epochIndex() const { return epoch_idx_; }

        /**
         * @brief Report this arc as present at the open epoch.
         *
         * @param key      The arc.
         * @param modelled The roster's @c Modelled role for this signal this epoch.
         * @return The step taken: @c Birth, @c Reentry, or @c None.
         */
        ArcStep observe(const Key& key, bool modelled);

        /**
         * @brief Close the open epoch: gap and kill the arcs that were not observed.
         *
         * Only arcs whose station is in @p present_stations are gapped or killed,
         * so a station outage never opens a gap (see the file header).  Dead
         * records older than the forget window are erased at every station,
         * present or not.
         *
         * @param present_stations Station indices that reported at least one
         *                         observation at this epoch.
         * @param out              Receives one entry per arc that crossed an edge,
         *                         in key order.  Cleared first.
         */
        void endEpoch(const std::set<int>& present_stations,
                      std::vector<std::pair<Key, ArcStep>>& out);

        /**
         * @brief Advance the admitted lifecycle of the open epoch.
         *
         * Called once per epoch (at most once per @ref beginEpoch: the sweep
         * recognizes this epoch's admitted arcs by their @c ep_last_admitted stamp),
         * downstream of the raw pass, with the arcs whose
         * modelled row survived the elevation mask, the allow list and the admission
         * screens.  The machine is the one the raw side runs, on its own
         * timestamps, so an arc can be raw-Tracking and admitted-Gap at the same
         * epoch; phase 3 keys on that difference.
         *
         * The stations considered are those that contributed at least one admitted
         * row, for the same reason the raw pass takes the present set: a station
         * outage must not open a gap at every one of its arcs.
         *
         * When @p reasons is non-null and armed, the reason mask of every admitted
         * gap this call opens is stamped from it: the screens' records for that arc,
         * narrowed to @ref kArcReasonGapMask, or @ref kArcReasonUnknown when nothing
         * explains the gap.  With @c nullptr the machine
         * behaves as in phase 3.
         *
         * @param admitted The (station, satellite, signal) arcs admitted at this epoch.
         * @param out      Receives one entry per admitted-side edge, in key order.
         *                 Cleared first.  @c transition / @c from / @c to describe
         *                 the admitted side on these steps.
         * @param reasons  The epoch's reason buffer, or nullptr.
         */
        void admitEpoch(const std::set<Key>& admitted,
                        std::vector<std::pair<Key, ArcStep>>& out,
                        const ArcReasonEpoch* reasons = nullptr);

        /**
         * @brief Record that this arc's ambiguity column was pruned.
         *
         * A mark, not a transition: the prune is not an arc event (the receiver
         * reports or not independently of the state vector); it means the column a
         * later re-entry returns to will be a fresh one.  No record is created for
         * an arc the ledger has never seen, since the ledger has nothing to say
         * about a key it does not know.
         *
         * @param key The arc.
         * @return true when a record existed and was marked.
         */
        bool markStatePruned(const Key& key);

        /// @brief The arc table: a row whose @c ledger field is present holds a record
        ///        the ledger still remembers (every other row is skipped by a reader).
        const Table& arcs() const;

        /// @brief The record for one arc, or nullptr when it is unknown or forgotten.
        const ArcRecord* find(const Key& key) const;

        /// @brief Arcs currently in @c Tracking or @c Reentered.
        std::size_t liveCount() const;

        /// @brief Arcs whose admitted side is in @c Tracking or @c Reentered.
        std::size_t admittedLiveCount() const;

        /// @brief Records retained, live plus gapped plus not-yet-forgotten dead.
        std::size_t size() const;

        /**
         * @brief Whether this transition may be written as an event at this epoch.
         *
         * Births are suppressed on epoch 0, and every transition is rate limited
         * per (station, transition) by @ref kArcLedgerEventHoldoffEp.  A call that
         * returns true consumes the holdoff slot, so call it once per candidate
         * event.
         *
         * @param station   Station index.
         * @param t         The transition.
         * @return true when the caller should write the event row.
         */
        bool eventAllowed(int station, ArcTransition t);

    private:
        double    horizon_s_{ 0.0 };
        long long forget_ep_{ kArcLedgerForgetEp };
        long long epoch_idx_{ -1 };
        double    t_s_{ 0.0 };
        net::NetworkEntities* ents_;   ///< The bound entities (the records' table).
        std::map<std::pair<int, int>, long long> ev_last_; ///< (station, transition) -> epoch of its last event.
    };

    /**
     * @brief One arc's reason record inside the epoch.
     */
    struct ArcReasonRec {
        unsigned  bits{ kArcReasonNone };  ///< the reasons recorded for this arc.
        long long ep{ -1 };                ///< the epoch they were recorded at.
    };

    /**
     * @brief The per-epoch reason buffer the screens write.
     *
     * Owned by @c NetworkRunContext.  Every screen that removes a modelled row, a
     * satellite or a station records the arcs it removed, with its own bit, here;
     * the roster's audit pass folds the buffer into the ledger, so an admitted gap
     * carries the reasons that opened it.
     *
     * With @c armed false (the default, @c network.ledger_screens off) every
     * writer returns on its first line: no walk, no allocation, no counter moved.
     * The fold then hands @c nullptr to the machine, so the ledger's behaviour is
     * unchanged and the solve is byte-identical.
     *
     * The buffer is opened at the start of the fold's epoch through
     * @ref arcReasonBeginEpoch rather than cleared at the build seam, because the
     * readmit park writes downstream of the audit pass; see @ref kArcReasonLateMask.
     * The gross-LOS screen writes before the fold (its calls precede
     * networkSignalRosterAudit in ApplicationNetwork.cpp), so its bits reach the
     * fold of their own epoch.
     */
    struct ArcReasonEpoch {
        bool      armed{ false };   ///< network.ledger_screens.
        long long ep{ -1 };         ///< the epoch currently open.
        std::map<ArcLedger::Key, ArcReasonRec> rec;   ///< arc -> the reasons recorded at this epoch.
        /// The @ref kArcReasonLateMask bits carried over from the previous epoch,
        /// kept apart from @ref rec so the carry is one epoch wide and an early
        /// writer cannot inherit a stale bit.  Rebuilt at every
        /// @ref arcReasonBeginEpoch.
        std::map<ArcLedger::Key, unsigned> late;
        /// Writer calls per reason index.  A writer with zero calls is dead wiring
        /// (invariant 4), and the run-end table reports it as such.
        std::vector<long long> calls{ std::vector<long long>(kArcReasonCount, 0) };
        /// Writer calls that named an arc the ledger has never admitted, so the
        /// screen fired on something outside the admitted population.
        std::vector<long long> calls_unknown{ std::vector<long long>(kArcReasonCount, 0) };
    };

    /**
     * @brief Open one epoch of the reason buffer.  Cleared except for the late bits.
     *
     * @param buf The buffer.
     * @param ep  The ledger's own epoch index for the epoch being opened.
     */
    void arcReasonBeginEpoch(ArcReasonEpoch& buf, long long ep);

    /**
     * @brief Record one reason for one arc.  This is the whole writer API.
     *
     * @param buf   The buffer; a no-op when it is not armed.
     * @param key   The arc the screen removed.
     * @param bit   Exactly one @ref ArcDeathReason bit.
     * @param known Whether the ledger knows this arc (for the dead-wiring split).
     */
    void arcReasonRecord(ArcReasonEpoch& buf, const ArcLedger::Key& key,
                         unsigned bit, bool known = true);

    /// @brief The reasons recorded for @p key at the open epoch, or @c kArcReasonNone.
    unsigned arcReasonOf(const ArcReasonEpoch& buf, const ArcLedger::Key& key);

    /**
     * @brief Record one reason for every admitted-alive arc of one satellite at
     *        one station: the expansion rule for a screen with no per-arc identity.
     *
     * The elevation mask refuses a whole LOS, the readmit horizon parks a whole
     * satellite, and the coverage gate removes a whole family; none of them names
     * a signal.  Such a writer expands to the arcs the screen removed, which on
     * the admitted side are the arcs alive (admitted Tracking or Reentered) when
     * the screen fired.
     *
     * @param buf     The buffer; a no-op when it is not armed.
     * @param led     The ledger, read-only.
     * @param station Station index, or -1 for every station.
     * @param sat     Satellite id.
     * @param signal  Signal id, or empty for every signal of the satellite.
     * @param bit     Exactly one @ref ArcDeathReason bit.
     * @return The number of arcs the expansion reached.
     */
    long long arcReasonExpand(ArcReasonEpoch& buf, const ArcLedger& led,
                              int station, const std::string& sat,
                              const std::string& signal, unsigned bit);

    /**
     * @brief The admitted Gap-to-Reentered arcs of this epoch.
     *        Pure function.
     *
     * A query, not a step: the admitted machine is advanced once per epoch far
     * downstream (the roster's audit pass), while the re-entry response runs in
     * PASS A, so the population is read off the timestamps the ledger already
     * holds instead of advancing the machine twice.  An arc qualifies when it has
     * been admitted before (@c ep_last_admitted at or above zero), has missed at
     * least one epoch since, and is admitted again at @p ep.
     *
     * @param led        The ledger, read-only.
     * @param admitted   The (station, satellite, signal) phase arcs admitted at this epoch.
     * @param ep         The ledger's own epoch index for this epoch.
     * @param now_s      Continuous GPS seconds of this epoch.
     * @param max_gap_s  The carry horizon (@c network.phase_leveling_max_gap_s): a
     *                   return past it is counted in @c outside_horizon and left out.
     * @param out        Receives the population in @p admitted key order; @c arcs is
     *                   cleared first, and @c outside_horizon is accumulated.
     */
    void arcLedgerReentryPopulation(const ArcLedger& led,
                                    const std::set<ArcLedger::Key>& admitted,
                                    long long ep, double now_s, double max_gap_s,
                                    ArcReentryEpoch& out);

    /**
     * @brief What the re-entry response did to one arc.
     *
     * The unified definition of an answered re-entry (R4).  The response records
     * its outcome per arc, and the census reads that record instead of inferring
     * the outcome; the two earlier approximations (a read of
     * @c relevel.free_ambs with the gap cause, and a match on @c reentry_relevel
     * events) disagreed.  The record is written in both knob states: unarmed, the
     * raw tiers record their own outcomes, so the four-way split is comparable
     * across the flip.
     */
    enum class ArcReentryOutcome {
        None,              ///< no record: the response never looked at this arc.
        Freed,             ///< reached: a carry existed and the refloat mark landed.
        BridgedVerdict,    ///< a raw tier saw the return and judged it clean; bridged by design.
        BridgedNoVerdict,  ///< no raw tier saw the return; bridged with nothing looked at.
        NoCarry            ///< a response was due, but there was nothing to protect.
    };

    /// @brief Human-readable name of an outcome, for the run-end table.
    const char* arcReentryOutcomeName(ArcReentryOutcome o);

    /// @brief This epoch's per-arc response outcomes (@ref ArcReentryOutcome).
    using ArcReentryOutcomeMap = std::map<ArcLedger::Key, ArcReentryOutcome>;


} // namespace GnssProcessor
