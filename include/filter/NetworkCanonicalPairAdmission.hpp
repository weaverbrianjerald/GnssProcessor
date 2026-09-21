/**
 * @file NetworkCanonicalPairAdmission.hpp
 * @brief Card CPA: admit a (station, satellite) to the network path only while
 *        the station's CANONICAL phase and code pair on bands 1 and 2 is held.
 *
 * ### The defect this answers
 *
 * A receiver tracks a satellite on its civil signals for a while before (and
 * after) it holds the semi-codeless pair the station's canonical latch actually
 * selected.  Such a (station, satellite) epoch reaches the solve with one band
 * short, or with a FALLBACK tracking attribute on the band the latch did not
 * pick, and it is harmful on both beds: removed from the RINEX files by an
 * emulation of the rule below, the dual field network went from 203 to 627
 * epochs below a decimetre of 720 and its first hour from 0.53 to 0.19 m, and
 * the triple network with L5 at all 59 stations from 6 to 461
 * (docs/refound/GRS_gap_reentry_per_signal_2026-09-20.md §5 and §6).  This seam
 * moves that rule into the producer.
 *
 * ### The rule
 *
 * For every (station, satellite) the CANONICAL PAIR is the station's canonical
 * phase and code signal on band 1 and on band 2, as the producer's OWN latch
 * decided them (@c PppModel::decideConstellationCanonicals, exported through
 * @c PppModel::exportCanonicalSignals).  Band 1 and band 2 are read as the
 * RINEX band DIGIT of the latched code, which is what the evidence's emulation
 * meant by "band" and what makes the four slots well defined without a second
 * band-name table here.  An epoch is GOOD for that (station, satellite) when
 * all four canonical observables are present in the epoch's raw data.  The
 * satellite's observation at that station is ADMITTED when the epoch is good,
 * or when the last good PROCESSED epoch is at most
 * @c network.canonical_pair_keep_epochs epochs back; it is WITHHELD before the
 * first good epoch (@c before_first) and beyond the tolerance
 * (@c beyond_keep), until the pair returns.
 *
 * A constellation for which the latch has decided NO canonical pair (no code
 * and phase on both band digits: every constellation on its first processed
 * epoch, and structurally e.g. Galileo, whose bands are 1/5/7/6 and which
 * therefore has no band-2 canonical at all) is LEFT UNTOUCHED: its
 * observations pass exactly as they do today.  Only GPS is in scope on the
 * RF-0hv postures this card was measured on.
 *
 * ### Where it acts, and why there
 *
 * At the top of the epoch body, on a per-epoch mutable copy of the
 * @c SyncedEpoch, immediately after the EVENT HARNESS injector and therefore
 * upstream of @c networkFastClockTick, of @c PppModel::computeModels, of every
 * T0 detector, of @c networkComputeRawObservables and of assembly. That is the same
 * single site and the same scratch-buffer contract as
 * @ref networkInjectEpoch, for the same reason: everything downstream is
 * DERIVED from the @c RinexObservation, so dropping the record here is exactly
 * "as if the RINEX record were missing".  That is what makes the row-level gap
 * re-entry tier see the satellite's return as a RE-ENTRY, which is what the
 * emulation relied on.
 *
 * The ordering has one consequence worth stating plainly: the latch is decided
 * INSIDE @c computeModels, on the first epoch a station reports the
 * constellation, so on that one epoch this seam has no pair to screen against
 * and leaves the station alone.  The alternative (screen the epoch the latch is
 * decided on) is not available to a causal rule at all, and would change the
 * latch's own tally.
 *
 * The held-out closed-loop consumer is NOT touched: the seam walks the
 * PRODUCER roster, which never contains it.
 *
 * ### Byte-identity when OFF
 *
 * @c network.canonical_pair_admission false leaves
 * @c NetworkCanonicalPairState::armed false.  The seam then returns a reference
 * to the caller's own @c SyncedEpoch (no copy, no roster walk, no counter
 * moved), so an OFF run is structurally byte-identical.
 *
 * ### Both edges are events (standing invariants 4 and 7)
 *
 * The DECOUPLING edge (@c withhold) and the RE-COUPLING edge (@c readmit) each
 * emit one @c ADMISSION / @c canonical_pair row on the TRANSITION only, never
 * once per withheld epoch, with the reason and the gap length in the detail.
 * The run-end counters (records withheld per reason, distinct (station,
 * satellite) pairs affected, readmissions) are the ON-path telemetry: an armed
 * run that withheld nothing is reported as ARMED AND INERT.
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
     * Filled from @c PppModel::exportCanonicalSignals by band DIGIT: @c l1 / @c c1
     * are the phase and code whose RINEX code has digit '1', @c l2 / @c c2 those
     * with digit '2'.  A constellation whose latch supplies fewer than all four
     * is not screened at all (@ref decided is then false).
     */
    struct CanonicalPair {
        std::string l1;   ///< canonical PHASE on band digit 1 (e.g. "L1W").
        std::string c1;   ///< canonical CODE  on band digit 1 (e.g. "C1W").
        std::string l2;   ///< canonical PHASE on band digit 2 (e.g. "L2W").
        std::string c2;   ///< canonical CODE  on band digit 2 (e.g. "C2W").
        /// @brief True when the latch supplied all four slots (the only case screened).
        bool decided() const {
            return !l1.empty() && !c1.empty() && !l2.empty() && !c2.empty();
        }
    };

    /// @brief What the state machine decided for one (station, satellite) epoch.
    enum class CanonicalPairAction {
        AdmitGood,            ///< the pair is held this epoch.
        AdmitWithinKeep,      ///< the pair is absent but the last good epoch is inside the tolerance.
        WithholdBeforeFirst,  ///< the pair has never been held. Reason @c before_first.
        WithholdBeyondKeep    ///< the pair was held once, longer ago than the tolerance. Reason @c beyond_keep.
    };

    /**
     * @brief Per (station, satellite) admission state: the whole memory the rule needs.
     *
     * Lives in @c NetworkCarryState (the anti-regrowth rule for carried state);
     * nothing here is a function local and nothing here is snapshotted.  A warm
     * resume therefore starts every pair at "never good", which is the same
     * forward-only posture the other observer states on this path take: the
     * first good epoch after the resume seats it.
     */
    struct CanonicalPairTrack {
        long long last_good{ -1 };    ///< processed-epoch index of the last GOOD epoch; -1 = never good.
        long long withheld_ep{ 0 };   ///< consecutive epochs withheld so far (0 = currently admitted).
    };

    /**
     * @brief One step of the state machine, and the two edges it may have crossed.
     */
    struct CanonicalPairStep {
        CanonicalPairAction action{ CanonicalPairAction::AdmitGood };
        bool      withhold_edge{ false }; ///< FIRST withheld epoch of this run of withholds.
        bool      readmit_edge{ false };  ///< first admitted epoch after at least one withheld one.
        /// At a withhold edge: processed epochs since the last good one (0 when there
        /// has never been one).  At a readmit edge: the number of epochs just withheld.
        long long gap_ep{ 0 };
    };

    /**
     * @brief Advance one (station, satellite)'s admission state by one processed epoch.
     *
     * The PURE state machine of the card, free of the roster, the config and the
     * traces so it can be driven directly by the unit suite.
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

    /// @brief The event/counter reason string for a withholding action ("" otherwise).
    const char* canonicalPairReason(CanonicalPairAction a);

    /**
     * @brief Run-scope state of the rule: the arm, the epoch clock, the per-pair
     *        memory and the latched per-station canonical pairs.
     *
     * Deliberately NOT part of the warm-resume fingerprint and not snapshotted
     * (same posture as the other forward-only observer states on this path).
     * @c armed == false leaves every member untouched and the seam unreachable.
     */
    struct NetworkCanonicalPairState {
        bool armed{ false };      ///< network.canonical_pair_admission.
        int  keep_ep{ 3 };        ///< network.canonical_pair_keep_epochs (clamped at 0).
        long long epochs{ 0 };    ///< seam invocations: THE processed-epoch clock of this card.
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
     * @brief Run-end counters: the ON-path telemetry of the card (invariant 4).
     *
     * Lives in @c NetworkServiceCounters.  Every disposition is counted against a
     * denominator, so "the log looks clean" can never be mistaken for "the rule
     * acted": @c withheld_total at zero on an armed run is dead wiring, and the
     * teardown says so in those words.
     */
    struct NetworkCanonicalPairCounters {
        long long epochs{ 0 };               ///< epochs the seam ran (denominator).
        long long screened{ 0 };             ///< (station, satellite) records screened against a decided pair.
        long long unscreened{ 0 };           ///< records left alone: their constellation has no decided pair.
        long long withheld_before_first{ 0 };///< records withheld because the pair had never been held.
        long long withheld_beyond_keep{ 0 }; ///< records withheld because the last good epoch is past the tolerance.
        long long withheld_total{ 0 };       ///< sum of the two above (the headline).
        long long admit_within_keep{ 0 };    ///< records admitted by the tolerance alone (the knob's own effect).
        long long withhold_edges{ 0 };       ///< DECOUPLING edges (invariant 7).
        long long readmit_edges{ 0 };        ///< RE-COUPLING edges (invariant 7).
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
     * Disabled ⇒ @c armed stays false and nothing else in the card is reachable.
     */
    void networkCanonicalPairArm(NetworkRunContext& ctx, const GnssConfig& config_,
                                 int n_sta);

    /**
     * @brief Apply the rule to this epoch's observations.
     *
     * @param ctx      Run context (state + counters).
     * @param log      Producer logger (the first withhold is logged once, at debug).
     * @param traces   Trace bundle; one @c ADMISSION row per TRANSITION.
     * @param stations The PRODUCER roster: the screened set, and the source of
     *                 each station's canonical latch.  The held-out closed-loop
     *                 consumer is not on it and is therefore never screened.
     * @param se_in    The epoch as it reaches the seam (post-injector).
     * @param scratch  Caller-owned buffer that receives the pruned copy.
     * @return @p se_in itself when the rule is disarmed or withheld nothing this
     *         epoch (NO copy is taken, the byte-identical path), otherwise a
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
     * @param ctx Run context (counters + state).
     * @param log Producer logger.
     */
    void networkCanonicalPairRunEnd(NetworkRunContext& ctx,
                                    const std::shared_ptr<spdlog::logger>& log);

} // namespace GnssProcessor
