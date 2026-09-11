#pragma once
/**
 * @file NetworkCommonBandScope.hpp
 * @brief COMMON-BAND SCOPE — free only the band the evidence NAMES
 *        (`network.cycle_slip.common_band_scope`, docs/SPEC_event_harness.md §14).
 *
 * ### The defect
 *
 * A single-band slip on a triple-frequency arc moves every Melbourne-Wübbena
 * pair that CONTAINS the band: an L5 slip fires (1,5) and (2,5) and leaves
 * (1,2) silent.  Each firing pair's drain refloats BOTH of its bands, so the
 * per-epoch union is all three — an ALL-BAND free, the §9 floor (0.45–0.65 cyc
 * for 45 epochs at low elevation, slant iono −0.08 m), paid for a slip that
 * touched one band.  On the storm bed the run-end clause reads ~2.2 admissions
 * per detection row: most slips fire two lanes, so most responses are all-band.
 *
 * ### The rule (§14.2)
 *
 * Per (station, satellite, epoch), over the tracked phase bands B:
 *
 *  - a MW pair is **silent** iff it was evaluated this epoch, is MATURE (arc
 *    count ≥ `common_band_min_arc_epochs`) and did not flag — the detector's
 *    affirmative-cleanliness set (@c MwSlipDetector::getEpochCleanPairs).  A
 *    flagged pair (admitted OR dropped — a dropped flag is still not silent),
 *    an immature pair and an absent pair are all NOT silent;
 *  - a silent pair whose IF-TD pair FLAGGED this epoch cannot exonerate.  This
 *    closes the EQUAL-COUNT trap: L1 and L2 slipping by the same integer leave
 *    MW(1,2) silent while the iono-free combination moves 0.107 m per cycle;
 *  - a band carrying a loss-of-lock flag is implicated regardless;
 *  - `exonerated(b) ⇔ no LLI on b ∧ ∃ silent, un-vetoed pair p ∋ b`, and
 *    `implicated = B \ exonerated`.
 *
 * Narrowing needs POSITIVE exoneration.  Three fallbacks return TODAY'S
 * response verbatim — free every band any drain would have freed: no MW pair
 * fired; nothing was exonerated; or `implicated` came out EMPTY although a pair
 * fired (inconsistent evidence, e.g. (1,2) firing alone while (1,5) and (2,5)
 * are both silent).
 *
 * The verdict is formed ONCE per epoch, BEFORE the drains, and governs every
 * drain for that (station, satellite): the MW refloat, the LLI free and the
 * IF-TD pair refloat each free (their own bands ∩ implicated), so the per-epoch
 * union is exactly `implicated` — Study 3's per-epoch invariant (§Z.2), whose
 * violation was the first defect that study's harness found.
 *
 * ### Not the Study-3 mechanism
 *
 * @c network.cycle_slip.refloat_band_scope (§Z) picks the band to KEEP from the
 * MW/GF DECOMPOSITION and was falsified on the storm bed — its fallback ran as
 * "keep L1C whatever happened to it" 1 091 times and collapsed the integers.
 * This card is set-theoretic: it needs no decomposition, it never keeps a band
 * that was not positively exonerated, and with no exoneration it does nothing.
 *
 * Default OFF is byte-identical: the drains are handed a null verdict pointer
 * and their hooks are not reached.  The counters are collected KNOB-
 * INDEPENDENTLY (invariant #4), so an OFF run's run-end line already states
 * what arming would have narrowed.
 */

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

/// Signal pair, ordered as the detector reports it (sig1 = high band).
using CommonBandPair = std::pair<std::string, std::string>;

/**
 * @brief This epoch's slip evidence for ONE (station, satellite).
 *
 * Collected in the PASS-A parallel region (per-station slots only) except for
 * @ref iftd_flagged, which is the network POOLED decision and is previewed
 * serially just before the verdict.
 */
struct CommonBandEvidence {
    /// Affirmatively-clean (silent) MW pairs this epoch → that arc's sample
    /// count.  From @c MwSlipDetector::getEpochCleanPairs / @c getEpochCleanArcN.
    std::map<CommonBandPair, int> clean_pairs;
    /// MW pairs that FLAGGED this epoch — admitted to the refloat path OR
    /// dropped on the admission gate.  A dropped flag is still not silent.
    std::set<CommonBandPair> flagged_pairs;
    /// IF-TD pairs FLAGGED this epoch (the pooled, network-wide decision).
    std::set<CommonBandPair> iftd_flagged;
    /// IF-TD pairs EVALUATED this epoch and NOT flagged — recorded for the
    /// verdict's basis string only; silence here exonerates nothing on its own.
    std::set<CommonBandPair> iftd_silent;
    /// Phase signals carrying a loss-of-lock bit this epoch.
    std::set<std::string> lli_signals;
};

/// Why a verdict came out the way it did.
enum class CommonBandOutcome {
    NoFlag,          ///< No MW pair fired — nothing to scope, no verdict counted.
    Narrowed,        ///< Positive exoneration: the response is restricted to @c implicated.
    NoExoneration,   ///< A pair fired, nothing was exonerated ⇒ today's response.
    Inconsistent     ///< A pair fired, yet every band was exonerated ⇒ today's response.
};

/// One (station, satellite, epoch) verdict.
struct CommonBandVerdict {
    /// Bands the response may free.  On every fallback this is the full tracked
    /// set, i.e. exactly today's response.
    std::set<std::string> implicated;
    /// Bands a silent, mature, un-vetoed pair positively cleared.
    std::set<std::string> exonerated;
    CommonBandOutcome outcome{ CommonBandOutcome::NoFlag };
    /// True only for @c CommonBandOutcome::Narrowed — the one state in which a
    /// drain skips anything.
    bool narrowed{ false };
    /// Human-readable evidence trail for the event row (`;`-separated, no
    /// commas — the event writer keeps `detail` a single CSV field).
    std::string basis;
    /// Silent pairs refused as exonerators because IF-TD flagged them.
    int iftd_veto{ 0 };
    /// Silent pairs refused as exonerators for arc immaturity.
    int immature_pairs{ 0 };
};

/**
 * @brief The PURE rule of §14.2.
 *
 * Total over its inputs and free of I/O, config and global state, so the seven
 * worked cases of the specification are unit-testable directly.
 *
 * @param live_bands      The arc's tracked phase bands B (the station's live
 *                        ambiguity signals for this satellite).  Empty ⇒ nothing
 *                        to scope.
 * @param ev              This epoch's evidence for the (station, satellite).
 * @param min_arc_epochs  Maturity bar a SILENT pair must reach to exonerate.
 * @return The verdict.  @c implicated is always a set a drain can intersect
 *         with; on every non-narrowed outcome it equals @p live_bands.
 */
CommonBandVerdict networkCommonBandVerdict(const std::set<std::string>& live_bands,
                                           const CommonBandEvidence& ev,
                                           int min_arc_epochs);

/**
 * @brief This epoch's verdicts plus the census, owned by the PASS-A slip scan.
 *
 * Only NARROWED verdicts reach @ref narrowed_at: a drain that finds no entry
 * for its (station, satellite) runs today's response, which is what makes the
 * OFF path and every fallback structurally identical.
 */
struct CommonBandEpoch {
    /// True when `network.cycle_slip.common_band_scope` is armed.  With it
    /// false the census still runs (invariant #4) but @ref narrowed_at stays
    /// empty and no event row is written.
    bool armed{ false };

    /// (station index, satellite) → verdict, NARROWED verdicts only.
    std::map<std::pair<int, std::string>, CommonBandVerdict> narrowed_at;

    /// Every verdict formed this epoch, narrowed or not, in station/satellite
    /// order — the event-row source for the fallback rows.
    std::vector<std::pair<std::pair<int, std::string>, CommonBandVerdict>> all;

    /// @name Per-epoch census (folded into NetworkServiceCounters by the caller)
    /// @{
    long long verdicts{ 0 };          ///< (station, satellite) pairs with ≥1 fired MW pair.
    long long narrowed_n{ 0 };        ///< Of those, narrowed by positive exoneration.
    long long all_no_exoneration{ 0 };///< Fallback: a pair fired, nothing exonerated.
    long long all_inconsistent{ 0 };  ///< Fallback: a pair fired, every band exonerated.
    long long bands_kept{ 0 };        ///< Bands spared by a narrowed verdict (Σ|exonerated|).
    long long iftd_veto{ 0 };         ///< Silent pairs refused as exonerators by an IF-TD flag.
    long long immature_pairs{ 0 };    ///< Silent pairs refused as exonerators for arc age.
    long long would_narrow{ 0 };      ///< Knob OFF: verdicts that WOULD have narrowed.
    /// @}

    /// Drop every per-epoch value (called once per processed epoch).
    void clear() {
        narrowed_at.clear();
        all.clear();
        verdicts = narrowed_n = all_no_exoneration = all_inconsistent = 0;
        bands_kept = iftd_veto = immature_pairs = would_narrow = 0;
    }

    /**
     * @brief The bands a drain may free for (@p i, @p sat).
     * @return Pointer to the implicated set when this (station, satellite) has a
     *         NARROWED verdict; @c nullptr otherwise — meaning today's response,
     *         which every drain must then run unchanged.
     */
    const std::set<std::string>* implicatedAt(int i, const std::string& sat) const {
        const auto it = narrowed_at.find({ i, sat });
        return (it == narrowed_at.end()) ? nullptr : &it->second.implicated;
    }
};

/**
 * @brief Drain hook: may this drain free @p signal of (@p i, @p sat)?
 *
 * The single predicate every freeing site consults, so the three drains cannot
 * drift apart.  A null @p cbs (the knob off) or a (station, satellite) with no
 * narrowed verdict answers TRUE for every signal — today's response.
 *
 * @param cbs     This epoch's verdicts, or @c nullptr when the scope is off.
 * @param i       Station index.
 * @param sat     Satellite id.
 * @param signal  The phase signal the drain is about to free.
 * @return True when the drain may free it.
 */
inline bool commonBandMayFree(const CommonBandEpoch* cbs, int i,
                              const std::string& sat, const std::string& signal) {
    if (!cbs || !cbs->armed) return true;
    const std::set<std::string>* impl = cbs->implicatedAt(i, sat);
    return !impl || impl->count(signal) > 0;
}

} // namespace GnssProcessor
