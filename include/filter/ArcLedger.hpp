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

    /// @brief Why an arc left, as a bit mask on @c ArcRecord::reason_mask.
    enum ArcDeathReason : unsigned {
        kArcReasonNone    = 0u,       ///< no reason recorded.
        kArcReasonHorizon = 1u << 0,  ///< the gap outran the readers' longest window.
        kArcReasonMask    = 1u << 1,  ///< reserved: an elevation-mask exit (phase 3).
        kArcReasonSlip    = 1u << 2,  ///< reserved: a slip tier erased the arc (phase 3).
        kArcReasonGate    = 1u << 3,  ///< reserved: a coverage gate dropped it (phase 4).
        kArcReasonPrune   = 1u << 4   ///< reserved: the state vector pruned it (phase 3).
    };

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
    };

    /**
     * @brief One step of the machine: the edge crossed and the identity of it.
     */
    struct ArcStep {
        ArcTransition transition{ ArcTransition::None };
        ArcState  from{ ArcState::Absent };   ///< state before the step.
        ArcState  to{ ArcState::Absent };     ///< state after the step.
        int       gap_len_ep{ 0 };            ///< epochs missed (meaningful at Reentry and Death).
        int       generation{ 0 };            ///< the generation AFTER the step.
        int       prev_generation{ 0 };       ///< the generation before a Birth (0 = a true first sighting).
        unsigned  reason_mask{ kArcReasonNone };
    };

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

        /// @brief Read-only access to every record the ledger still remembers.
        const std::map<Key, ArcRecord>& arcs() const { return arcs_; }

        /// @brief The record for one arc, or nullptr when it is unknown or forgotten.
        const ArcRecord* find(const Key& key) const;

        /// @brief Arcs currently in @c Tracking or @c Reentered.
        std::size_t liveCount() const;

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

} // namespace GnssProcessor
