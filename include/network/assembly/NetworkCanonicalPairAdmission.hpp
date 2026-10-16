/**
 * @file NetworkCanonicalPairAdmission.hpp
 * @brief Admit a (station, satellite) to the network path only while the station's
 *        canonical phase and code pair on bands 1 and 2 is held (card CPA).
 *
 * ### Problem addressed
 *
 * A receiver tracks a satellite on its civil signals for a while before (and
 * after) it holds the semi-codeless pair that the station's canonical latch
 * selected.  Such a (station, satellite) epoch reaches the solve with one band
 * missing, or with a fallback tracking attribute on the band the latch did not
 * pick, and it degrades the solution on both beds.  An emulation of the rule below
 * that removed these records from the RINEX files increased the number of epochs
 * below a decimetre on the dual-frequency field network and on the triple-frequency
 * network with L5 at all 59 stations, and lowered the dual network's first-hour
 * error (docs/refound/GRS_gap_reentry_per_signal_2026-09-20.md §5 and §6).  This
 * seam applies that rule inside the producer.
 *
 * ### The rule
 *
 * For every (station, satellite), the canonical pair is the station's canonical
 * phase and code signal on band 1 and on band 2, as decided by the producer's own
 * latch (@c PppModel::decideConstellationCanonicals, exported through
 * @c PppModel::exportCanonicalSignals).  Bands 1 and 2 are the RINEX band digit
 * of each latched signal code, which matches what the emulation meant by "band"
 * and defines the four slots without a second band-name table.  An epoch is good
 * for that (station, satellite) when all four canonical observables are present in
 * the epoch's raw data.  The satellite's observation at that station is admitted
 * when the epoch is good, or when the last good processed epoch is at most
 * @c network.canonical_pair_keep_epochs epochs back.  It is withheld before the
 * first good epoch (@c before_first) and beyond the tolerance (@c beyond_keep),
 * until the pair returns.
 *
 * A constellation for which the latch has decided no canonical pair (no code and
 * phase on both band digits) is left untouched: its observations pass unchanged.
 * This covers every constellation on its first processed epoch, and structurally
 * Galileo, whose bands are 1/5/7/6 and which has no band-2 canonical.  On the
 * RF-0hv postures the card was measured on, only GPS is in scope.
 *
 * ### Where it acts, and why there
 *
 * At the top of the epoch body, on a per-epoch mutable copy of the
 * @c SyncedEpoch, immediately after the event-harness injector and therefore
 * upstream of @c PppModel::computeModels, every T0 detector,
 * @c networkComputeRawObservables and assembly.  It uses the same site
 * and the same scratch-buffer contract as @ref networkInjectEpoch, for the same
 * reason: everything downstream is derived from the @c RinexObservation, so
 * dropping the record here is equivalent to a missing RINEX record.  The row-level
 * gap re-entry tier then reads the satellite's return as a re-entry, which is what
 * the emulation relied on.
 *
 * Consequence of this ordering: the latch is decided inside @c computeModels, on
 * the first epoch a station reports the constellation, so on that epoch this seam
 * has no pair to screen against and leaves the station alone.  Screening the epoch
 * on which the latch is decided is not possible for a causal rule, and would
 * change the latch's own tally.
 *
 * The held-out closed-loop consumer is not touched: the seam walks the producer
 * roster, which never contains it.
 *
 * ### Byte-identity when off
 *
 * With @c network.canonical_pair_admission false,
 * @c NetworkCanonicalPairState::armed stays false.  The seam then returns a
 * reference to the caller's own @c SyncedEpoch (no copy, no roster walk, no
 * counter changed), so an off run is structurally byte-identical.
 *
 * ### Both edges are events (standing invariants 4 and 7)
 *
 * The decoupling edge (@c withhold) and the re-coupling edge (@c readmit) each
 * emit one @c ADMISSION / @c canonical_pair row on the transition only, never one
 * per withheld epoch, with the reason and the gap length in the detail.  The
 * run-end counters (records withheld per reason, distinct (station, satellite)
 * pairs affected, readmissions) are the on-path telemetry: an armed run that
 * withheld nothing is reported as ARMED AND INERT.
 */
#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

    class GnssConfig;
    class NetworkRunTraces;
    struct NetStation;             // filter/NetworkRunTypes.hpp
    struct NetworkRunContext;
    struct SyncedEpoch;            // rinex/MultiRinexSync.hpp, namespace-level

    /**
     * @brief The four latched observables that make up one constellation's
     *        canonical pair at one station.
     *
     * Filled from @c PppModel::exportCanonicalSignals by band digit: @c l1 / @c c1
     * are the phase and code whose RINEX code has digit '1', @c l2 / @c c2 those
     * with digit '2'.  A constellation whose latch supplies fewer than all four is
     * not screened (@ref decided is then false).
     */
    struct CanonicalPair {
        std::string l1;   ///< canonical phase on band digit 1 (e.g. "L1W").
        std::string c1;   ///< canonical code on band digit 1 (e.g. "C1W").
        std::string l2;   ///< canonical phase on band digit 2 (e.g. "L2W").
        std::string c2;   ///< canonical code on band digit 2 (e.g. "C2W").
        /// @brief True when the latch supplied all four slots (the only case screened).
        bool decided() const {
            return !l1.empty() && !c1.empty() && !l2.empty() && !c2.empty();
        }
    };

    /// @brief Decision of the state machine for one (station, satellite) epoch.
    enum class CanonicalPairAction {
        AdmitGood,            ///< the pair is held this epoch.
        AdmitWithinKeep,      ///< the pair is absent but the last good epoch is inside the tolerance.
        WithholdBeforeFirst,  ///< the pair has never been held. Reason @c before_first.
        WithholdBeyondKeep    ///< the pair was held once, longer ago than the tolerance. Reason @c beyond_keep.
    };

    /**
     * @brief Per (station, satellite) admission state: all the memory the rule needs.
     *
     * Lives in @c NetworkCarryState (the anti-regrowth rule for carried state); it
     * is neither a function local nor snapshotted.  A warm resume therefore starts
     * every pair at "never good", the same forward-only posture as the other
     * observer states on this path: the first good epoch after the resume seats it.
     */
    struct CanonicalPairTrack {
        long long last_good{ -1 };    ///< processed-epoch index of the last good epoch; -1 = never good.
        long long withheld_ep{ 0 };   ///< consecutive epochs withheld so far (0 = currently admitted).
    };

    /**
     * @brief One step of the state machine, and the two edges it may have crossed.
     */
    struct CanonicalPairStep {
        CanonicalPairAction action{ CanonicalPairAction::AdmitGood };
        bool      withhold_edge{ false }; ///< first withheld epoch of this run of withholds.
        bool      readmit_edge{ false };  ///< first admitted epoch after at least one withheld one.
        /// On a step that is not good (including a withhold edge): processed epochs
        /// since the last good one (0 when there has never been one).  On a good step
        /// (including a readmit edge): the number of epochs just withheld.
        long long gap_ep{ 0 };
    };

    /**
     * @brief Advance one (station, satellite)'s admission state by one processed epoch.
     *
     * The pure state machine of the card, independent of the roster, the config and
     * the traces, so the unit suite can drive it directly.
     *
     * @param tr        [in/out] that pair's carried state.
     * @param good      all four canonical observables are present this epoch.
     * @param epoch_idx the monotonic processed-epoch index (must not decrease).
     * @param keep      @c network.canonical_pair_keep_epochs (already clamped at 0).
     * @return the verdict plus the edges crossed.
     */
    CanonicalPairStep canonicalPairStep(CanonicalPairTrack& tr, bool good,
                                        long long epoch_idx, int keep);

    /// @brief True for the two @ref CanonicalPairAction values that withhold.
    inline bool canonicalPairWithholds(CanonicalPairAction a) {
        return a == CanonicalPairAction::WithholdBeforeFirst
            || a == CanonicalPairAction::WithholdBeyondKeep;
    }

    /// @brief The event and counter reason string for a withholding action ("" otherwise).
    const char* canonicalPairReason(CanonicalPairAction a);

    /**
     * @brief Run-scope state of the rule: the arm, the epoch clock, the per-pair
     *        memory and the latched per-station canonical pairs.
     *
     * Not part of the warm-resume fingerprint and not snapshotted, like the other
     * forward-only observer states on this path.  With @c armed false, every
     * member stays untouched and the seam is unreachable.
     */
    struct NetworkCanonicalPairState {
        bool armed{ false };      ///< network.canonical_pair_admission.
        int  keep_ep{ 3 };        ///< network.canonical_pair_keep_epochs (clamped at 0).
        long long epochs{ 0 };    ///< seam invocations: the processed-epoch clock of this card.
        /// (station index, satellite) -> its admission state.
        std::map<std::pair<int, std::string>, CanonicalPairTrack> track;
        /// station index -> constellation -> its latched canonical pair.  Refreshed
        /// from the station's @c PppModel only while a constellation it reports is
        /// still missing here, because the latch is decided once and never revised.
        std::vector<std::map<std::string, CanonicalPair>> pairs;
        /// Distinct (station index, satellite) pairs this run has withheld at least once.
        std::set<std::pair<int, std::string>> affected;
    };

    /**
     * @brief Run-end counters: the on-path telemetry of the card (invariant 4).
     *
     * Lives in @c NetworkServiceCounters.  Every disposition is counted against a
     * denominator, so a clean log is not mistaken for evidence that the rule acted:
     * @c withheld_total at zero on an armed run is reported by the teardown as
     * ARMED AND INERT.
     */
    struct NetworkCanonicalPairCounters {
        long long epochs{ 0 };               ///< epochs the seam ran (denominator).
        long long screened{ 0 };             ///< (station, satellite) records screened against a decided pair.
        long long unscreened{ 0 };           ///< records left alone: their constellation has no decided pair.
        long long withheld_before_first{ 0 };///< records withheld because the pair had never been held.
        long long withheld_beyond_keep{ 0 }; ///< records withheld because the last good epoch is past the tolerance.
        long long withheld_total{ 0 };       ///< sum of the two above (the headline).
        long long admit_within_keep{ 0 };    ///< records admitted by the tolerance alone (the knob's own effect).
        long long withhold_edges{ 0 };       ///< decoupling edges (invariant 7).
        long long readmit_edges{ 0 };        ///< re-coupling edges (invariant 7).
        long long max_gap_ep{ 0 };           ///< longest withheld run that ended in a readmission.
        long long epochs_copied{ 0 };        ///< epochs that took the scratch copy (something was withheld).
    };

    /**
     * @brief Arm the rule from config (once, at run start).
     *
     * @param ctx     Run context; @c ctx.carry.cpa is filled in.
     * @param config_ The run's config (reads the two @c network.canonical_pair_* knobs).
     * @param n_sta   Producer roster size (sizes the per-station latch cache).
     *
     * When disabled, @c armed stays false and nothing else in the card is reachable.
     */
    void networkCanonicalPairArm(NetworkRunContext& ctx, const GnssConfig& config_,
                                 int n_sta);

    /**
     * @brief Apply the rule to this epoch's observations.
     *
     * @param ctx      Run context (state and counters).
     * @param log      Producer logger (the first withhold is logged once, at debug).
     * @param traces   Trace bundle; one @c ADMISSION row per transition.
     * @param stations The producer roster: the screened set, and the source of each
     *                 station's canonical latch.  The held-out closed-loop consumer
     *                 is not on it and is therefore never screened.
     * @param se_in    The epoch as it reaches the seam (after the injector).
     * @param scratch  Caller-owned buffer that receives the pruned copy.
     * @return @p se_in itself when the rule is disarmed or withheld nothing this
     *         epoch (no copy is taken; the byte-identical path), otherwise a
     *         reference to @p scratch with the withheld records erased.
     *
     * @note @p scratch must outlive the returned reference for the whole epoch
     *       body; declare it immediately before the call.
     */
    const SyncedEpoch& networkCanonicalPairAdmission(NetworkRunContext& ctx,
                                                     const std::shared_ptr<spdlog::logger>& log,
                                                     NetworkRunTraces& traces,
                                                     const std::vector<NetStation>& stations,
                                                     const SyncedEpoch& se_in,
                                                     SyncedEpoch& scratch);

    /**
     * @brief Run-end readout: one line, or nothing when the rule is disarmed.
     *
     * @param ctx Run context (counters and state).
     * @param log Producer logger.
     */
    void networkCanonicalPairRunEnd(const NetworkRunContext& ctx,
                                    const std::shared_ptr<spdlog::logger>& log);

    // ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

    struct NetworkRunEndView;

    /**
     * @brief Run-end report: the canonical-pair admission (CPA).
     *
     * The table's row for the canonical-pair admission (card CPA); the report is the
     * overload above.
     *
     * Gate: none of its own: the row calls the report above on every run, as the teardown
     * did, and that report applies its own.
     *
     * @param v The run at its end (filter/NetworkRunEnd.hpp).
     * @return 0 (this report does not set the exit code).
     */
    int networkCanonicalPairRunEnd(const NetworkRunEndView& v);

    // ---- End of the run-end reports ----

} // namespace GnssProcessor
