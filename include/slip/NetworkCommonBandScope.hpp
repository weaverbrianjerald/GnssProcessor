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
 *  - a MW pair is **silent** iff the detector PROCESSED a sample for it this
 *    epoch (@c MwSlipDetector::getEpochPairStatus), the arc's count at that
 *    sample reached `common_band_min_arc_epochs`, and it did NOT flag — no
 *    @c MwSlipResult for that pair, admitted OR dropped, since a dropped flag
 *    is still not silence.  An immature pair and an unprocessed pair are both
 *    NOT silent.
 *
 *    This is deliberately the DETECTION-grade question, not the
 *    cleanliness-grade one.  Iteration 1 of this card read
 *    @c getEpochCleanPairs, whose σ-awareness condition (3σ ≤ 0.7·λ_WL) a
 *    narrow lane on a noisy bed cannot satisfy however quiet it is: measured
 *    MW scatter σ_MAD p50 = 0.414 m on L1C+L2W against that pair's 0.201 m
 *    bar, so across 519 verdicts on the 60-station sim the set named ONLY the
 *    extra-wide lanes and the rule never engaged on a single-band slip.  The
 *    cleanliness set stays untouched for its TurboEdit consumer;
 *
 *  - a silence carries its own THRESHOLD (@ref CommonBandProcessed::thresh_wl).
 *    A pair whose gate stood at 8 WL cycles did not exclude a 6-cycle slip, it
 *    merely failed to see one; those are counted (@ref
 *    CommonBandVerdict::weak_silence) and still allowed to exonerate, so the
 *    cost of trusting them is measured rather than assumed;
 *  - a silent pair whose IF-TD pair FLAGGED this epoch cannot exonerate.  This
 *    closes the EQUAL-COUNT trap: L1 and L2 slipping by the same integer leave
 *    MW(1,2) silent while the iono-free combination moves 0.107 m per cycle.
 *    ⚠ The IF-TD detector forms ONE pair per satellite by default
 *    (`network.cycle_slip.if_phase_td_pairs = first`: L1C+L2W on GPS), so the
 *    veto reaches only that lane; `all` puts an IF-TD pair behind every lane
 *    (§14.10).  Measured on the storm bed at S37000SIM / G20, 4.99 h: truth L5Q
 *    −31 then L1C −29, MW(L1C+L5Q) moved 2 wide-lane cycles and stayed silent
 *    with no IF-TD pair behind it, and the verdict freed L2W — the one band that
 *    had not slipped;
 *
 *  - **RULE F2, the LLI SHADOW (§14.11)**: a pair is NOT silent — it is treated
 *    as immature — at any epoch where one of its BANDS carried a receiver
 *    loss-of-lock bit at the PREVIOUS epoch (@ref
 *    CommonBandProcessed::lli_shadow, exported by
 *    @c MwSlipDetector::getEpochPairStatus).  The uncombined LLI branch
 *    schedules the ambiguity resets and `continue`s, deliberately PRESERVING the
 *    MW arc, so the count runs straight through the flagged epoch and the first
 *    sample after it looks mature while the lane saw nothing at the epoch the
 *    receiver declared broken — and a band-common slip does not move the wide
 *    lane in any case.  Measured on the storm bed: all five spurious `keep`
 *    verdicts in 2 994 sat exactly one epoch after an LLI, and in every one the
 *    exonerating lane CONTAINED the flagged band (S37/G20 (L1C+L5Q) n 87→99
 *    across the LLI, thr 9.0 WL; S25/E36 (L1C+L5Q) n 164→173).  Band-scoped on
 *    purpose: a lane of two unflagged bands is untouched by the receiver's
 *    report and may still exonerate, which is what leaves the 853 correct late
 *    narrowings — the flagged band re-implicated one epoch later — alive;
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
/// What the MW detector did with one pair this epoch (from
/// @c MwSlipDetector::getEpochPairStatus).
struct CommonBandProcessed {
    /// Arc sample count at the flag decision — the maturity gate's input.
    int arc_n{ 0 };
    /// The detector's own verdict.  Advisory only: the rule decides "flagged"
    /// from the emitted @c MwSlipResult set, which is what the drains act on.
    bool flagged{ false };
    /// Detection threshold at that sample, in WIDE-LANE CYCLES.  A silence is
    /// only as strong as this is small — a pair whose threshold stood at 8 WL
    /// cycles did not exclude a 6-cycle slip, it merely failed to see one.
    /// Non-finite when the sample left before a threshold existed.
    double thresh_wl{ 0.0 };
    /// §14.11 RULE F2 — LLI SHADOW.  One of this pair's BANDS carried a receiver
    /// loss-of-lock bit at the PREVIOUS epoch, so the pair's previous sample was
    /// withheld and this one is the first after the gap.  The uncombined LLI
    /// branch preserves the MW arc deliberately, so the count runs straight
    /// through the flagged epoch and the pair looks mature; but a band-common
    /// slip does not move the wide lane, so its silence carries no information
    /// about the epoch the receiver declared broken.  Such a pair is treated as
    /// IMMATURE for one epoch — it may not exonerate.  Band-scoped on purpose: a
    /// lane made only of unflagged bands is untouched by the receiver's report
    /// and may still exonerate, which is what keeps the correct late narrowings
    /// (the flagged band re-implicated one epoch later) alive.
    bool lli_shadow{ false };
};

struct CommonBandEvidence {
    /// Every MW pair the detector PROCESSED a sample for this epoch.  The
    /// silence source: a pair is silent iff it is here, mature, and absent from
    /// @ref flagged_pairs.  NOT the cleanliness set — see @ref
    /// MwSlipDetector::getEpochPairStatus for why that is the wrong instrument.
    std::map<CommonBandPair, CommonBandProcessed> processed;
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
    Inconsistent,    ///< A pair fired, yet every band was exonerated ⇒ today's response.
    /// The evidence WOULD have narrowed, but this station currently anchors a
    /// phase-bias family of this satellite.  §14.5: narrowing there SPLITS the
    /// satellite's per-frequency references across stations — an L1-only response
    /// hands over only the L1C family and leaves L2W/L5Q pinned where they were,
    /// after which the bands' arc levels walk independently.  Measured on the
    /// iteration-1 storm arm: satellite-epochs with families anchored at
    /// DIFFERENT stations went 2 500 (6.6 %) → 6 262 (16.5 %), 27 % in hour 6,
    /// and the consumer paid for it (settled p50 0.174 → 0.230 m) while every
    /// producer channel held or improved.  So on an anchor arc the response stays
    /// ALL-BAND and every family of that satellite moves together in one epoch,
    /// which is what lets the sticky election place them at ONE successor.
    AnchorColocate
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
    /// §14.10: WHICH pairs those were.  The card's claim is that the vetoes come
    /// from band pairs the single-pair IF-TD detector never watched, and a total
    /// alone cannot show that — so the veto carries its own identity (invariant
    /// #7) and the run-end clause breaks it down by pair kind.
    std::set<CommonBandPair> iftd_veto_pairs;
    /// Silent pairs refused as exonerators for arc immaturity.
    int immature_pairs{ 0 };
    /// §14.11 RULE F2: silent pairs refused as exonerators because one of their
    /// bands carried an LLI at the previous epoch (@ref
    /// CommonBandProcessed::lli_shadow).  Counted separately from @ref
    /// immature_pairs even though the response is the same, because the two
    /// refusals have different causes and only this one is a receiver report.
    int lli_shadow{ 0 };
    /// Silent pairs whose detection threshold exceeded @ref kWeakSilenceWlCyc —
    /// a silence that could not have excluded a slip of that size.  REPORTED,
    /// never acted on: the pair still exonerates.  It is the honest measure of
    /// how much of the narrowing rests on a threshold too loose to mean much.
    int weak_silence{ 0 };
    /// 1 when the ANCHOR CLAUSE (§14.5) converted a would-be narrowing into
    /// today's all-band response because this station anchors a family of this
    /// satellite.  @ref exonerated is left populated — the evidence was sound;
    /// it is the datum topology, not the evidence, that refuses the narrowing.
    int anchor_colocate{ 0 };
};

/// A silence is "weak" above this threshold, in wide-lane cycles.  6 cycles is
/// the low end of the harness's injected slip sizes, so a pair whose gate stood
/// above it did not exclude the event class the scope is responding to.
inline constexpr double kWeakSilenceWlCyc = 6.0;

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
 * @param station_anchors_satellite  True when the station currently anchors ANY
 *        phase-bias family of this satellite (any band).  The §14.5 ANCHOR
 *        CLAUSE: a would-be narrowing there becomes
 *        @ref CommonBandOutcome::AnchorColocate instead, so the satellite's
 *        families are never left realized at different stations.
 * @return The verdict.  @c implicated is always a set a drain can intersect
 *         with; on every non-narrowed outcome it equals @p live_bands.
 */
CommonBandVerdict networkCommonBandVerdict(const std::set<std::string>& live_bands,
                                           const CommonBandEvidence& ev,
                                           int min_arc_epochs,
                                           bool station_anchors_satellite = false);

/// Per-epoch reading of how fragmented the phase-bias datum is across stations.
struct SplitRefCount {
    /// Satellites with >= 2 anchored families whose anchors are NOT all at the
    /// same station — the population whose per-frequency biases are realized at
    /// stations whose arc levels walk independently.
    int split{ 0 };
    /// Satellites with >= 2 anchored families: the population that CAN split, and
    /// the honest denominator for @ref split.
    int multi_anchored{ 0 };
};

/**
 * @brief Count split-referenced satellites in an anchor incumbent map.
 *
 * ALWAYS ON and knob-independent (invariant #4): this is the instrument that
 * caught the iteration-1 storm regression, and it has to read the same on a run
 * with the scope off.  Cheap — one pass over the map, which holds one entry per
 * anchored (satellite, signal).
 *
 * @param anchor_incumbent  "&lt;sat&gt;_&lt;signal&gt;" → anchoring station name,
 *                          as left by the election for this epoch.
 * @return The split and multi-anchored satellite counts.
 */
inline SplitRefCount networkSplitReferencedSats(
        const std::map<std::string, std::string>& anchor_incumbent) {
    // sat -> (first station seen, saw a different one)
    std::map<std::string, std::pair<std::string, bool>> per_sat;
    std::map<std::string, int> n_fam;
    for (const auto& [amb, sta] : anchor_incumbent) {
        const std::size_t us = amb.find('_');
        if (us == std::string::npos || us == 0 || us + 1 >= amb.size()) continue;
        const std::string sat = amb.substr(0, us);
        ++n_fam[sat];
        auto it = per_sat.find(sat);
        if (it == per_sat.end()) per_sat.emplace(sat, std::make_pair(sta, false));
        else if (it->second.first != sta) it->second.second = true;
    }
    SplitRefCount out;
    for (const auto& [sat, st] : per_sat) {
        if (n_fam[sat] < 2) continue;
        ++out.multi_anchored;
        if (st.second) ++out.split;
    }
    return out;
}

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
    long long lli_shadow{ 0 };        ///< §14.11 F2: silent pairs refused because a band carried an LLI at the previous epoch.
    long long weak_silence{ 0 };      ///< Exonerating silences whose threshold exceeded 6 WL cycles.
    long long anchor_colocate{ 0 };   ///< Would-be narrowings refused because the station anchors the satellite (§14.5).
    long long would_narrow{ 0 };      ///< Knob OFF: verdicts that WOULD have narrowed.
    /// §14.10: @ref iftd_veto attributed to the IF-TD pair KIND that vetoed
    /// ("L1C_L5Q" → n).  Under `if_phase_td_pairs = first` only the one watched
    /// pair can appear here, which is precisely the measurement the card rests on.
    std::map<std::string, long long> iftd_veto_by_pair;
    /// @}

    /// Drop every per-epoch value (called once per processed epoch).
    void clear() {
        narrowed_at.clear();
        all.clear();
        verdicts = narrowed_n = all_no_exoneration = all_inconsistent = 0;
        bands_kept = iftd_veto = immature_pairs = weak_silence = lli_shadow = 0;
        anchor_colocate = would_narrow = 0;
        iftd_veto_by_pair.clear();
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
