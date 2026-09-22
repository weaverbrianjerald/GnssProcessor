#pragma once
/**
 * @file ArcLedger.hpp
 * @brief Card SID phase 1: the ARC LIFECYCLE state machine, one record per
 *        (station, satellite, signal), owned by @c NetworkRunContext.
 *
 * ### The defect this observes
 *
 * The audit of 2026-09-22 (`runs/_msc/sid_audit_report_2026-09-22.md`, Part B)
 * found 29 arc-keyed containers in @c NetworkRunContext.hpp of which only 5 are
 * keyed at the SIGNAL level, and 31 lifecycle holders whose birth and death edges
 * emit nothing at all.  Among them is
 * @c StateManager::pruneAmbiguitiesExcept, which destroys an ambiguity after ONE
 * missed epoch with no window and no event, and @c local_carry's gap prune, which
 * is silent.  The lifecycle vocabulary exists three times in the codebase
 * (@c adm_arc, @c pb_hold, @c gl_state) in three incompatible key spaces, and is
 * absent from the one container the producer depends on.
 *
 * This ledger is the single state machine those containers can later read.  In
 * phase 1 NOTHING reads it: it is built, it counts, it traces, and the solve is
 * byte-identical with it armed and disarmed.
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
 *   Dead ------ forget  ----> record erased
 * @endverbatim
 *
 * The HORIZON is a time, not an epoch count: the readers whose windows it has to
 * cover are all configured in seconds (@c network.phase_leveling_max_gap_s,
 * @c network.iono_carry_max_gap_s, @c network.emit_pb_hold_max_gap_s,
 * @c network.readmit_seed_horizon_s), and a run whose epoch interval differs from
 * 30 s would silently mean something different if the horizon were counted in
 * epochs.  The gap LENGTH is still reported in epochs, because that is the unit
 * every existing re-entry instrument uses.
 *
 * The FORGET window is an epoch count on the @c kAdmArcForgetEp precedent
 * (@c NetworkMeasurementAdmission.cpp): a dead record is kept that long so a late
 * return is still distinguishable from a first sighting, which is exactly the
 * distinction the anchor election does not make today.
 *
 * ### Absent stations
 *
 * An arc whose STATION reported nothing this epoch is left frozen, never gapped.
 * A station outage would otherwise manufacture a gap at every one of its arcs, and
 * the population that matters is the arc that vanished while its station kept
 * reporting.  That is the same rule @c NetworkSignalIntegrity.cpp applies through
 * its per-epoch @c seen set.
 *
 * ### Scale
 *
 * About 1 200 live records per epoch at 58 stations and about 6 000 at 305, so the
 * per-transition EVENT stream is holdoff-throttled per (station, transition) from
 * the start and birth events are suppressed on the first epoch entirely.  The
 * per-transition TRACE (`logs/network_arc_ledger.csv`) is complete: it is a file,
 * not a log, and a reader that needs every edge reads it there.
 */

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace GnssProcessor {

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
     * @brief Why an arc left, as a bit mask on @c ArcRecord::reason_mask.
     *
     * Card SID phase 4 (S1) turns the reserved bits into WRITTEN ones: every
     * screen that removes a modelled row, a satellite or a station records the
     * arcs it removed and its own bit into the per-epoch @ref ArcReasonEpoch
     * buffer, and the fold in @ref ArcLedger::admitEpoch stamps that mask on the
     * admitted gap the removal opened.  An admitted gap that no writer explains
     * carries @ref kArcReasonUnknown, which is the number phase 3's 87
     * no-verdict returns are owed.
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
        kArcReasonDeweight  = 1u << 9,  ///< a DEWEIGHT, never a removal: excluded from every gap mask.
        kArcReasonNoModel   = 1u << 10, ///< the model built no row (no product, no usable pair, eclipsing).
        kArcReasonUnknown   = 1u << 11  ///< an admitted gap opened and no writer fired.
    };

    /// @brief Number of reason bits, for the per-reason counter arrays.
    inline constexpr int kArcReasonCount = 12;

    /**
     * @brief Bits that may explain an admitted GAP.
     *
     * @ref kArcReasonDeweight is excluded by construction: a de-weight leaves the
     * row in the solve, so counting it as a gap opener would attribute a removal
     * to a screen that removed nothing (the brief's own rule).  @ref
     * kArcReasonUnknown is excluded because it is what the fold writes when this
     * mask comes out empty.
     */
    inline constexpr unsigned kArcReasonGapMask =
        kArcReasonHorizon | kArcReasonMask | kArcReasonSlip | kArcReasonGate
        | kArcReasonPrune | kArcReasonAllowlist | kArcReasonAdmission
        | kArcReasonReadmit | kArcReasonGross | kArcReasonNoModel;

    /**
     * @brief Bits whose writer fires AFTER the epoch's fold.
     *
     * The readmit park runs inside @c networkStashStatus, downstream of the
     * roster's audit pass, so its record cannot reach the fold of its own epoch.
     * Exactly these bits survive one @ref arcReasonBeginEpoch so the gap they can
     * open — which is at the NEXT epoch at the earliest, the satellite having
     * already departed — still finds them.  Every other bit is cleared, so an
     * early writer can never attribute a gap it did not open.
     */
    inline constexpr unsigned kArcReasonLateMask = kArcReasonReadmit;

    /// @brief Short name of one reason BIT, for the run-end table and the events.
    const char* arcReasonName(unsigned bit);

    /// @brief Index of one reason BIT in a @ref kArcReasonCount array, or -1.
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
     * Epoch indices are the ledger's OWN processed-epoch clock (one increment per
     * @c ArcLedger::beginEpoch), never a downstream seam's, because a seam that
     * skips an epoch would stop the clock exactly where an arc dropped out.
     */
    struct ArcRecord {
        ArcState  state{ ArcState::Absent };  ///< current state.
        long long ep_first_seen{ -1 };        ///< epoch of the FIRST sighting ever (survives a death).
        long long ep_last_seen{ -1 };         ///< epoch of the most recent sighting.
        long long ep_state_changed{ -1 };     ///< epoch the current state was entered.
        long long ep_gap_started{ -1 };       ///< epoch the current or last gap opened.
        double    t_last_seen_s{ 0.0 };       ///< continuous GPS seconds of the most recent sighting.
        int       gap_len_ep{ 0 };            ///< epochs missed by the current or last gap; 0 while Tracking.
        int       generation{ 0 };            ///< increments at every birth; 1 at the first sighting.
        bool      modelled{ false };          ///< the roster gave this signal the Modelled role at the last sighting.
        unsigned  reason_mask{ kArcReasonNone }; ///< why the arc last left (see @ref ArcDeathReason).

        // --- card SID phase 2: the ADMITTED lifecycle, beside the raw one -----
        //
        // Phase 1 measured that the two populations are not the same population:
        // the raw side arrives upstream of the elevation mask, the allow list and
        // every admission screen, while `local_carry` and the ambiguity states are
        // keyed from the station's MODELLED rows.  Reported separately and never
        // added; a reader that keys on the ledger (phase 3) reads THIS side.
        ArcState  adm_state{ ArcState::Absent };    ///< current admitted-side state.
        long long ep_first_admitted{ -1 };          ///< epoch of the first admitted row ever.
        long long ep_last_admitted{ -1 };           ///< epoch of the most recent admitted row.
        long long ep_adm_state_changed{ -1 };       ///< epoch @ref adm_state was entered.
        double    t_last_admitted_s{ 0.0 };         ///< continuous GPS seconds of the most recent admitted row.
        int       adm_gap_len_ep{ 0 };              ///< epochs missed by the current or last ADMITTED gap.
        int       adm_generation{ 0 };              ///< increments at every admitted birth.
        unsigned  adm_reason_mask{ kArcReasonNone };///< why the admitted side last left.

        // --- card SID phase 3: the state-vector prune as a ledger MARK --------
        //
        // @c StateManager::pruneAmbiguitiesExcept destroys an ambiguity column
        // after one missed epoch, with no window and no event: the largest silent
        // death path the audit found.  Phase 3 does NOT change the prune (the
        // state vector must shrink); it records that it happened, so the arc's
        // next admitted re-entry can state that the column it returns to was
        // RE-CREATED rather than carried.  Observatory: nothing in the solve
        // reads either field.
        bool      state_pruned{ false };            ///< the column was pruned while this arc was in an admitted Gap inside the horizon.
        long long ep_state_pruned{ -1 };            ///< epoch of that prune.
        long long n_state_pruned{ 0 };              ///< how often it has happened to this arc.

        /**
         * @brief Epochs the ADMITTED side has missed as of @p ep.
         *
         * Returns -1 when the arc has never been admitted, which is a different
         * statement from "admitted and gapped for 0 epochs" and has to stay
         * separable: the raw side sees satellites the estimator never models.
         */
        long long admittedGapEp(long long ep) const {
            return ep_last_admitted < 0 ? -1 : ep - ep_last_admitted - 1;
        }
    };

    /**
     * @brief One step of the machine: the edge crossed and the identity of it.
     *
     * A step describes ONE side.  @ref transition, @ref from, @ref to,
     * @ref gap_len_ep and @ref generation describe the RAW side when the step came
     * from @c observe or @c endEpoch and the ADMITTED side when it came from
     * @c admitEpoch; @ref adm_state and @ref adm_gap_len_ep always report the
     * admitted side as it stands after the step, so a raw re-entry carries both
     * gap lengths without a second lookup.
     */
    struct ArcStep {
        ArcTransition transition{ ArcTransition::None };
        ArcState  from{ ArcState::Absent };   ///< state before the step.
        ArcState  to{ ArcState::Absent };     ///< state after the step.
        int       gap_len_ep{ 0 };            ///< epochs missed (meaningful at Reentry and Death).
        int       generation{ 0 };            ///< the generation AFTER the step.
        int       prev_generation{ 0 };       ///< the generation before a Birth (0 = a true first sighting).
        unsigned  reason_mask{ kArcReasonNone };
        ArcState  adm_state{ ArcState::Absent }; ///< the ADMITTED state after the step.
        /// Epochs the ADMITTED side has missed as of this epoch; -1 when the arc has
        /// never been admitted at all.
        long long adm_gap_len_ep{ -1 };
        int       adm_generation{ 0 };        ///< the admitted generation after the step.
        /// Card SID phase 3: the ambiguity column of this arc was pruned during the
        /// gap this step closes, so the column a re-entry returns to is a fresh one.
        bool      state_recreated{ false };
    };

    /**
     * @brief Card SID phase 3: one arc of the ledger-driven re-entry population.
     *
     * The unit of the response is the same (station, satellite, signal) the
     * response itself is keyed by, so a key mismatch between the population and
     * the action cannot occur by construction (card GRK's defect).
     */
    struct ArcReentryArc {
        int         station{ -1 };
        std::string satellite;
        std::string signal;
        int         gap_ep{ 0 };            ///< ADMITTED epochs missed.
        double      gap_s{ 0.0 };           ///< seconds since the last admitted row.
        bool        state_recreated{ false };///< the column was pruned inside the gap.
    };

    /**
     * @brief Card SID phase 3: this epoch's ledger-driven re-entry population.
     *
     * Built once per epoch at the PASS-A slip site and handed to both re-entry
     * drains.  @ref armed false leaves every drain on its historical raw-driven
     * path, byte for byte.
     */
    struct ArcReentryEpoch {
        bool armed{ false };                    ///< network.ledger_reentry.
        bool refloat_no_verdict{ false };       ///< network.ledger_reentry_no_verdict == refloat.
        std::vector<ArcReentryArc> arcs;        ///< the population, in key order.
        long long outside_horizon{ 0 };         ///< admitted returns past phase_leveling_max_gap_s (not in @ref arcs).
        long long not_modelled{ 0 };            ///< admitted returns whose signal the roster does not call Modelled.
    };

    /// @brief Card SID phase 4 (S1): the per-epoch reason buffer (defined below the class).
    struct ArcReasonEpoch;

    /**
     * @brief The forget window, in processed epochs, on the @c kAdmArcForgetEp
     *        precedent of @c NetworkMeasurementAdmission.cpp.
     *
     * A dead record is retained this long so that a return after it is still
     * reported with its previous generation rather than as a first sighting.
     */
    inline constexpr long long kArcLedgerForgetEp = 2880;

    /**
     * @brief Per (station, transition) event holdoff, in processed epochs.
     *
     * Bounds the @c ARC event stream at 305 stations, where an unthrottled birth
     * edge would write about 12 000 rows on the first epoch alone (the number
     * @c NetworkSignalIntegrity.cpp gives as its reason for suppressing its own
     * birth event).  The per-transition TRACE is not throttled.
     */
    inline constexpr long long kArcLedgerEventHoldoffEp = 30;

    /**
     * @brief The ledger: every arc of the run, and the machine that advances them.
     *
     * Owned by @c NetworkRunContext (@c ctx.arc_ledger).  Deliberately NOT part of
     * the warm-resume fingerprint and not snapshotted: it carries no value the
     * solve reads, so a resumed run simply re-seats every arc at its first
     * post-resume sighting, which is the honest posture for a fresh observer.
     */
    class ArcLedger {
    public:
        /// (station index, satellite, signal).
        using Key = std::tuple<int, std::string, std::string>;

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
         * @param epoch_idx Monotonic processed-epoch index (the ledger's own clock).
         * @param t_s       Continuous GPS seconds of the epoch.
         */
        void beginEpoch(long long epoch_idx, double t_s);

        /// @brief The epoch index of the epoch currently open.
        long long epochIndex() const { return epoch_idx_; }

        /**
         * @brief Report this arc as PRESENT at the open epoch.
         *
         * @param key      The arc.
         * @param modelled The roster's @c Modelled role for this signal this epoch.
         * @return The step taken: @c Birth, @c Reentry, or @c None.
         */
        ArcStep observe(const Key& key, bool modelled);

        /**
         * @brief Close the open epoch: gap and kill the arcs that were not observed.
         *
         * Only arcs whose station is in @p present_stations are considered, so a
         * station outage never manufactures a gap (see the file header).
         *
         * @param present_stations Station indices that reported at least one
         *                         observation at this epoch.
         * @param out              Receives one entry per arc that crossed an edge,
         *                         in key order.  Cleared first.
         */
        void endEpoch(const std::set<int>& present_stations,
                      std::vector<std::pair<Key, ArcStep>>& out);

        /**
         * @brief Card SID phase 2: advance the ADMITTED lifecycle of the open epoch.
         *
         * Called ONCE per epoch, downstream of the raw pass, with the arcs whose
         * MODELLED row survived the elevation mask, the allow list and the admission
         * screens.  The machine is the same one the raw side runs, on its own
         * timestamps, so an arc can be raw-Tracking and admitted-Gap at the same
         * epoch, which is exactly the difference phase 1 measured and phase 3 has to
         * key on.
         *
         * The stations considered are those that contributed at least one admitted
         * row, for the same reason the raw pass takes the present set: a station
         * outage must not manufacture a gap at every one of its arcs.
         *
         * Card SID phase 4 (S1): @p reasons, when non-null and armed, stamps the
         * REASON MASK of every admitted gap this call opens.  The mask is the
         * screens' own records for that arc, narrowed to @ref kArcReasonGapMask,
         * and @ref kArcReasonUnknown when nothing explains it.  Passing @c nullptr
         * reproduces the phase-3 machine exactly.
         *
         * @param admitted The (station, satellite, signal) arcs admitted at this epoch.
         * @param out      Receives one entry per admitted-side edge, in key order.
         *                 Cleared first.  @c transition / @c from / @c to describe
         *                 the ADMITTED side on these steps.
         * @param reasons  The epoch's reason buffer, or nullptr.
         */
        void admitEpoch(const std::set<Key>& admitted,
                        std::vector<std::pair<Key, ArcStep>>& out,
                        const ArcReasonEpoch* reasons = nullptr);

        /**
         * @brief Card SID phase 3: record that this arc's ambiguity column was pruned.
         *
         * A MARK, never a transition: the prune is not an arc event (the receiver
         * is still reporting, or not, quite independently of the state vector), it
         * is the fact that the column a later re-entry returns to will be a fresh
         * one.  No record is created for an arc the ledger has never seen, because
         * a key the ledger does not know is not an arc it can say anything about.
         *
         * @param key The arc.
         * @return true when a record existed and was marked.
         */
        bool markStatePruned(const Key& key);

        /// @brief Read-only access to every record the ledger still remembers.
        const std::map<Key, ArcRecord>& arcs() const { return arcs_; }

        /// @brief The record for one arc, or nullptr when it is unknown or forgotten.
        const ArcRecord* find(const Key& key) const;

        /// @brief Arcs currently in @c Tracking or @c Reentered.
        std::size_t liveCount() const;

        /// @brief Arcs whose ADMITTED side is in @c Tracking or @c Reentered.
        std::size_t admittedLiveCount() const;

        /// @brief Records retained, live plus gapped plus not-yet-forgotten dead.
        std::size_t size() const { return arcs_.size(); }

        /**
         * @brief Should this transition be EVENTED at this epoch?
         *
         * Births are suppressed on epoch 0 outright, and every transition is then
         * rate limited per (station, transition) by @ref kArcLedgerEventHoldoffEp.
         * Calling it CONSUMES the holdoff slot, so it is called exactly once per
         * candidate event.
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
        std::map<Key, ArcRecord> arcs_;
        std::set<Key> seen_;                              ///< observed at the open epoch.
        std::map<std::pair<int, int>, long long> ev_last_; ///< (station, transition) -> epoch of its last event.
    };

    /**
     * @brief Card SID phase 4 (S1) — one arc's reason record inside the epoch.
     */
    struct ArcReasonRec {
        unsigned  bits{ kArcReasonNone };  ///< the reasons recorded for this arc.
        long long ep{ -1 };                ///< the epoch they were recorded at.
    };

    /**
     * @brief Card SID phase 4 (S1) — the per-epoch REASON buffer the screens write.
     *
     * Owned by @c NetworkRunContext.  Every screen that removes a modelled row, a
     * satellite or a station records the arcs it removed and its own bit here; the
     * roster's audit pass folds the buffer into the ledger, so an admitted gap
     * carries the reason(s) that opened it instead of being anonymous.
     *
     * @c armed false (the default, @c network.ledger_screens off) makes every
     * writer return on its first line: no walk, no allocation, no counter moved,
     * and the fold hands @c nullptr to the machine, so the ledger's own behaviour
     * is unchanged and the solve is byte-identical.
     *
     * The buffer is opened at the START of the fold's epoch through
     * @ref arcReasonBeginEpoch rather than cleared at the build seam, because two
     * writers (the readmit park and the gross-LOS screen) run downstream of the
     * audit pass; see @ref kArcReasonLateMask.
     */
    struct ArcReasonEpoch {
        bool      armed{ false };   ///< network.ledger_screens.
        long long ep{ -1 };         ///< the epoch currently open.
        std::map<ArcLedger::Key, ArcReasonRec> rec;   ///< arc -> the reasons recorded THIS epoch.
        /// The @ref kArcReasonLateMask bits carried over from the PREVIOUS epoch,
        /// kept apart from @ref rec so the carry is exactly one epoch wide and an
        /// early writer can never inherit a stale bit.  Rebuilt at every
        /// @ref arcReasonBeginEpoch.
        std::map<ArcLedger::Key, unsigned> late;
        /// Writer CALLS per reason index (invariant 4: a writer with zero calls is
        /// dead wiring, and the run-end table says so in those words).
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
     * @brief Record ONE reason for ONE arc.  The whole writer API.
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
     * @brief Record one reason for every ADMITTED-ALIVE arc of one satellite at
     *        one station.  The EXPANSION rule for a screen with no per-arc identity.
     *
     * The elevation mask refuses a whole LOS, the readmit horizon parks a whole
     * satellite, and the coverage gate removes a whole family: none of them names
     * a signal.  The brief's rule is that such a writer expands to the arcs the
     * screen actually removed, which on the admitted side is the set that was
     * alive when the screen fired.
     *
     * @param buf     The buffer; a no-op when it is not armed.
     * @param led     The ledger, READ-ONLY.
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
     * @brief Card SID phase 3 — the ADMITTED Gap-to-Reentered arcs of this epoch.  PURE.
     *
     * A QUERY, not a step: the admitted machine is advanced once per epoch far
     * downstream (the roster's audit pass), while the re-entry response runs in
     * PASS A, so the population has to be read off the timestamps the ledger
     * already holds rather than by advancing the machine twice.  An arc qualifies
     * when it has been admitted before (@c ep_last_admitted at or above zero), has
     * missed at least one epoch since, and is admitted again at @p ep.
     *
     * @param led        The ledger, READ-ONLY.
     * @param admitted   The (station, satellite, signal) phase arcs admitted at this epoch.
     * @param ep         The ledger's own epoch index for this epoch.
     * @param now_s      Continuous GPS seconds of this epoch.
     * @param max_gap_s  The CARRY horizon (@c network.phase_leveling_max_gap_s): a
     *                   return past it is counted in @c outside_horizon and left out.
     * @param out        Receives the population in @p admitted key order; @c arcs is
     *                   cleared first, the counters are accumulated.
     */
    void arcLedgerReentryPopulation(const ArcLedger& led,
                                    const std::set<ArcLedger::Key>& admitted,
                                    long long ep, double now_s, double max_gap_s,
                                    ArcReentryEpoch& out);

    /**
     * @brief Card SID phase 3 — what the re-entry response did to ONE arc.
     *
     * The unified "answered" definition (R4).  Phases 1 and 2 approximated it two
     * different ways (a read of @c relevel.free_ambs with the gap cause, and a
     * match on @c reentry_relevel events) and the two disagreed by 5 of 51.  From
     * phase 3 the response RECORDS what it did, per arc, and the census reads that
     * instead of inferring it.  Written in BOTH knob states: unarmed the raw tiers
     * record their own outcomes, so the four-way split is comparable across the
     * flip.
     */
    enum class ArcReentryOutcome {
        None,              ///< no record: the response never looked at this arc.
        Freed,             ///< reached: a carry existed and the refloat mark landed.
        BridgedVerdict,    ///< a raw tier saw the return and called it CLEAN: bridged by design.
        BridgedNoVerdict,  ///< no raw tier saw the return: bridged with nothing looked at.
        NoCarry            ///< a response was due, but there was nothing to protect.
    };

    /// @brief Human-readable name of an outcome, for the run-end table.
    const char* arcReentryOutcomeName(ArcReentryOutcome o);

    /// @brief This epoch's per-arc response outcomes (@ref ArcReentryOutcome).
    using ArcReentryOutcomeMap = std::map<ArcLedger::Key, ArcReentryOutcome>;


} // namespace GnssProcessor
