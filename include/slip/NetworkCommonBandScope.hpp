#pragma once
/**
 * @file NetworkCommonBandScope.hpp
 * @brief Common-band slip scope: on a slip, free only the bands the evidence
 *        implicates (`network.cycle_slip.common_band_scope`,
 *        docs/SPEC_event_harness.md §14).
 *
 * ### Problem
 *
 * A single-band slip on a triple-frequency arc moves every Melbourne-Wübbena
 * pair that contains the band: an L5 slip fires (1,5) and (2,5) and leaves (1,2)
 * silent.  Each firing pair's drain refloats both of its bands, so the per-epoch
 * union is all three.  That all-band free costs the §9 floor (0.45–0.65 cyc for
 * 45 epochs at low elevation, slant iono −0.08 m) for a slip that touched one
 * band.  Most slips fire two lanes (about 2.2 admissions per detection row on the
 * storm bed), so most unscoped responses are all-band.
 *
 * ### Rule (§14.2)
 *
 * Per (station, satellite, epoch), over the tracked phase bands B:
 *
 *  - An MW pair is silent iff the detector processed a sample for it this epoch
 *    (@c MwSlipDetector::getEpochPairStatus), the arc's count at that sample
 *    reached `common_band_min_arc_epochs`, and it did not flag: there is no
 *    @c MwSlipResult for that pair, admitted or dropped (a dropped flag is still
 *    not silence).  Immature and unprocessed pairs are not silent.
 *
 *    This is the detection-grade question, not the cleanliness-grade one.
 *    @c getEpochCleanPairs requires 3σ ≤ 0.7·λ_WL, which a narrow lane on a
 *    noisy bed cannot meet however quiet it is (60-station sim: MW scatter
 *    σ_MAD p50 = 0.414 m on L1C+L2W against that pair's 0.201 m bar).  That set
 *    names only the extra-wide lanes, so a rule built on it never engages on a
 *    single-band slip.  The cleanliness set is unchanged for its TurboEdit
 *    consumer.
 *
 *  - A silence carries its own threshold (@ref CommonBandProcessed::thresh_wl).
 *    A pair whose gate stood at 8 WL cycles did not exclude a 6-cycle slip; it
 *    could not have seen one.  Such silences are counted
 *    (@ref CommonBandVerdict::weak_silence) and still allowed to exonerate, so
 *    the cost of trusting them is measured.
 *
 *  - A silent pair whose IF-TD pair flagged this epoch cannot exonerate.  This
 *    covers the equal-count case: L1 and L2 slipping by the same integer leave
 *    MW(1,2) silent while the iono-free combination moves 0.107 m per cycle.
 *    By default the IF-TD detector forms one pair per satellite
 *    (`network.cycle_slip.if_phase_td_pairs = first`: L1C+L2W on GPS), so the
 *    veto reaches only that lane; `all` puts an IF-TD pair behind every lane
 *    (§14.10).  Under `first`, an L1C+L5Q slip of nearly equal counts leaves
 *    MW(L1C+L5Q) silent with no IF-TD pair behind it, and the verdict frees L2W,
 *    the one band that did not slip.
 *
 *  - Reference-arc clause (`common_band_ref_arc`, default off, §22).  The veto
 *    can fire only where the pooled IF-TD test can flag.  On the chain's
 *    reference satellite `dd ≡ 0` by construction, so `|dd − med| > thr` does not
 *    respond to a slip however large.  With the clause armed, such a pair is
 *    untested (@ref CommonBandEvidence::iftd_untested) rather than silent, and is
 *    refused as an exonerator in the same way as `immature` and `lli_shadow`
 *    pairs: counted in @ref CommonBandVerdict::iftd_ref_arc, named
 *    `iftd(<pair>)=ref_arc` (inline when the refusal branch reaches it, otherwise
 *    in the basis tail, as the flagged set is named), and never listed as
 *    `silent`.  Without the clause, an equal-count L1C+L5X slip on a satellite
 *    that is the reference of both L5 chains narrows to the innocent band L2W
 *    under both `first` and `all`, while the same slip on a non-reference
 *    satellite is vetoed (§20.1).  Cost: a single-band L1 slip on a reference arc
 *    also loses its narrowing, because its only silent pair is now untested.
 *
 *  - LLI shadow (rule F2, §14.11).  A pair is not silent (it is treated as
 *    immature) at any epoch where one of its bands carried a receiver
 *    loss-of-lock bit at the previous epoch (@ref
 *    CommonBandProcessed::lli_shadow, exported by
 *    @c MwSlipDetector::getEpochPairStatus).  The uncombined LLI branch
 *    schedules the ambiguity resets and `continue`s while preserving the MW arc,
 *    so the count runs through the flagged epoch and the first sample after it
 *    looks mature although the lane saw nothing at the epoch the receiver
 *    declared broken; a band-common slip does not move the wide lane in any case.
 *    On the storm bed every spurious `keep` verdict came one epoch after an LLI,
 *    on a lane containing the flagged band.  The rule is band-scoped: a lane of
 *    two unflagged bands is unaffected by the receiver's report and may still
 *    exonerate, which keeps the correct late narrowings (the flagged band
 *    re-implicated one epoch later).
 *  - A band carrying a loss-of-lock flag is implicated regardless.
 *  - `exonerated(b) ⇔ no LLI on b ∧ ∃ silent, un-vetoed pair p ∋ b`, and
 *    `implicated = B \ exonerated`.
 *
 * Narrowing requires positive exoneration.  Three fallbacks keep the unscoped
 * response (free every band any drain would have freed): no MW pair fired;
 * nothing was exonerated; or `implicated` came out empty although a pair fired
 * (inconsistent evidence, e.g. (1,2) firing alone while (1,5) and (2,5) are both
 * silent).  The anchor clause (§14.5, @ref CommonBandOutcome::AnchorColocate)
 * also keeps the unscoped response.
 *
 * The verdict is formed once per epoch, before the drains, and governs every
 * drain for that (station, satellite): the MW refloat, the LLI free and the
 * IF-TD pair refloat each free (their own bands ∩ implicated), so the per-epoch
 * union is exactly `implicated` (Study 3's per-epoch invariant, §Z.2).
 *
 * ### Relation to refloat_band_scope
 *
 * @c network.cycle_slip.refloat_band_scope (§Z) picks the band to keep from the
 * MW/GF decomposition and was falsified on the storm bed: its fallback kept L1C
 * regardless of what had happened to it and collapsed the integers.  This scope
 * is set-theoretic: it needs no decomposition, keeps only bands that were
 * positively exonerated, and with no exoneration does nothing.
 *
 * Off by default, and byte-identical when off: the drains receive a null verdict
 * pointer, for which @ref commonBandMayFree answers true for every signal.  The
 * counters are collected whether or not the knob is armed (invariant #4), so an
 * off run's run-end line already states what arming would have narrowed.
 */

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

/// Signal pair, ordered as the detector reports it (sig1 = higher-frequency band).
using CommonBandPair = std::pair<std::string, std::string>;

/// What the MW detector did with one pair this epoch (from
/// @c MwSlipDetector::getEpochPairStatus).
struct CommonBandProcessed {
    /// Arc sample count at the flag decision (the maturity gate's input).
    int arc_n{ 0 };
    /// The detector's own verdict.  Advisory only: the rule decides "flagged"
    /// from the emitted @c MwSlipResult set, which is what the drains act on.
    bool flagged{ false };
    /// Detection threshold at that sample, in wide-lane cycles.  A silence is
    /// only as informative as this threshold is small: a pair whose threshold
    /// stood at 8 WL cycles could not have seen a 6-cycle slip.  Non-finite when
    /// the sample left before a threshold existed.
    double thresh_wl{ 0.0 };
    /// LLI shadow (rule F2, §14.11): one of this pair's bands carried a receiver
    /// loss-of-lock bit at the previous epoch, so the pair's previous sample was
    /// withheld and this one is the first after the gap.  The uncombined LLI
    /// branch preserves the MW arc, so the count runs through the flagged epoch
    /// and the pair looks mature; but a band-common slip does not move the wide
    /// lane, so its silence says nothing about the epoch the receiver declared
    /// broken.  Such a pair is treated as immature for one epoch and may not
    /// exonerate.  Band-scoped: a lane made only of unflagged bands is unaffected
    /// by the receiver's report and may still exonerate, which keeps the correct
    /// late narrowings (the flagged band re-implicated one epoch later).
    bool lli_shadow{ false };
};

/**
 * @brief This epoch's slip evidence for one (station, satellite).
 *
 * Collected in the PASS-A parallel region (per-station slots only), except for
 * the IF-TD sets (@ref iftd_flagged, @ref iftd_silent, @ref iftd_untested), which
 * come from the network-pooled decision and are previewed serially just before
 * the verdict.
 */
struct CommonBandEvidence {
    /// Every MW pair the detector processed a sample for this epoch.  This is
    /// the silence source: a pair is silent iff it is here, mature, not
    /// LLI-shadowed, and absent from @ref flagged_pairs.  It is not the
    /// cleanliness set; see @ref MwSlipDetector::getEpochPairStatus for why.
    std::map<CommonBandPair, CommonBandProcessed> processed;
    /// MW pairs that flagged this epoch, whether admitted to the refloat path or
    /// dropped on the admission gate.  A dropped flag still means not silent.
    std::set<CommonBandPair> flagged_pairs;
    /// IF-TD pairs flagged this epoch (the pooled, network-wide decision).
    std::set<CommonBandPair> iftd_flagged;
    /// IF-TD pairs evaluated this epoch and not flagged.  Recorded for the
    /// verdict's basis string only; this silence exonerates nothing on its own.
    std::set<CommonBandPair> iftd_silent;
    /// Reference-arc clause (§22): pairs whose IF-TD sample at this (station,
    /// satellite) is on the chain's reference satellite.  Its double difference
    /// is 0 by construction (@c IfJumpInfo::is_reference: grad(ref) − grad(ref)
    /// = 0 every epoch), so the pooled test `flagged = |dd − med| > thr` cannot
    /// respond to a slip there.  The pair is untested rather than silent, and
    /// the rule refuses it as an exonerator (@ref CommonBandVerdict::iftd_ref_arc).
    /// Populated only when `network.cycle_slip.common_band_ref_arc` is armed;
    /// with the knob off it stays empty and such a sample is folded into
    /// @ref iftd_flagged or @ref iftd_silent by its flag, which keeps the off
    /// path byte-identical.
    std::set<CommonBandPair> iftd_untested;
    /// Phase signals carrying a loss-of-lock bit this epoch.
    std::set<std::string> lli_signals;
};

/// Why a verdict came out the way it did.
enum class CommonBandOutcome {
    NoFlag,          ///< No MW pair fired: nothing to scope, no verdict counted.
    Narrowed,        ///< Positive exoneration: the response is restricted to @c implicated.
    NoExoneration,   ///< A pair fired and nothing was exonerated: unscoped response.
    Inconsistent,    ///< A pair fired, yet every band was exonerated: unscoped response.
    /// The evidence would have narrowed, but this station currently anchors a
    /// phase-bias family of this satellite (§14.5).  Narrowing there splits the
    /// satellite's per-frequency references across stations: an L1-only response
    /// hands over only the L1C family and leaves L2W/L5Q pinned where they were,
    /// after which the bands' arc levels walk independently.  On the storm bed
    /// this raised the share of satellite-epochs with families anchored at
    /// different stations from 6.6 % to 16.5 % and the consumer's settled p50
    /// from 0.174 to 0.230 m, while every producer channel held or improved.  On
    /// an anchor arc the response therefore stays all-band, so every family of
    /// the satellite moves in the same epoch and the sticky election can place
    /// them at one successor.
    AnchorColocate,
    /// LLI-epoch verdict (rule F3, §14.12).  The receiver flagged one or more
    /// bands at this epoch, and the uncombined LLI branch withholds every MW
    /// sample of that satellite (§14.11), so no pair could flag and the §14.2
    /// rule forms no verdict.  Without a verdict the drains run unscoped.  That
    /// is harmless while the LLI drain is the only actor, since the receiver's
    /// bits already scope it.  Once an IF-TD pair containing a flagged band can
    /// fire in the same epoch (`if_phase_td_pairs = all`), the unscoped pair
    /// refloat frees both of its bands and every LLI epoch becomes an all-band
    /// free (§14.10 S3 leg: level residual at +45 went 0.005 → 0.335 cyc, slant
    /// iono peak 0.004 → 0.143 m).  Here the receiver's bits are the scope:
    /// implicated = the flagged live bands, with no exoneration step (the LLI
    /// drain already trusts them that far).
    LliOnly
};

/// One (station, satellite, epoch) verdict.
struct CommonBandVerdict {
    /// Bands the response may free.  On every fallback this is the full tracked
    /// set, i.e. the unscoped response.
    std::set<std::string> implicated;
    /// Bands a silent, mature, un-vetoed pair positively cleared.
    std::set<std::string> exonerated;
    CommonBandOutcome outcome{ CommonBandOutcome::NoFlag };
    /// True only for @c CommonBandOutcome::Narrowed and for an
    /// @c CommonBandOutcome::LliOnly verdict whose flagged set is a strict subset
    /// of the live bands: the only states in which a drain skips anything.
    bool narrowed{ false };
    /// Human-readable evidence trail for the event row (`;`-separated, no
    /// commas: the event writer keeps `detail` a single CSV field).
    std::string basis;
    /// Silent pairs refused as exonerators because IF-TD flagged them.
    int iftd_veto{ 0 };
    /// Which pairs those were (§14.10).  The scope's claim is that the vetoes
    /// come from band pairs the single-pair IF-TD detector does not watch, which
    /// a total cannot show, so the veto carries its own identity (invariant #7)
    /// and the run-end clause breaks it down by pair kind.
    std::set<CommonBandPair> iftd_veto_pairs;
    /// Silent pairs refused as exonerators because their IF-TD sample sits on
    /// the chain's reference arc (§22, @ref CommonBandEvidence::iftd_untested),
    /// where the pooled test cannot flag: a structural dead zone, not evidence
    /// of a clean arc.  Counted apart from @ref iftd_veto although the response
    /// is the same: a veto is evidence that the pair moved, this is the absence
    /// of a usable test, and only this one is the price the clause pays (an
    /// equal-count slip on a reference arc falls back to all-band, the cost
    /// named in §22.2).
    int iftd_ref_arc{ 0 };
    /// Which pairs those were (§22), the same identity @ref iftd_veto_pairs
    /// carries for the veto (invariant #7).  The run-end clause breaks the
    /// refusals down by pair kind, so the dead zone can be attributed to the
    /// chains whose reference the arc is.
    std::set<CommonBandPair> iftd_ref_arc_pairs;
    /// Silent pairs refused as exonerators for arc immaturity.
    int immature_pairs{ 0 };
    /// Rule F2 (§14.11): silent pairs refused as exonerators because one of their
    /// bands carried an LLI at the previous epoch (@ref
    /// CommonBandProcessed::lli_shadow).  Counted apart from @ref immature_pairs
    /// although the response is the same, because the two refusals have
    /// different causes and only this one is a receiver report.
    int lli_shadow{ 0 };
    /// Silent pairs whose detection threshold exceeded @ref kWeakSilenceWlCyc: a
    /// silence that could not have excluded a slip of that size.  Reported only;
    /// the pair still exonerates.  It measures how much of the narrowing rests on
    /// a threshold too loose to mean much.
    int weak_silence{ 0 };
    /// True when this verdict came from the LLI-epoch rule (F3, §14.12) rather
    /// than the §14.2 rule.  Carried so the event row can name the rule even when
    /// the anchor clause rewrites the outcome to
    /// @ref CommonBandOutcome::AnchorColocate; without it the two paths'
    /// co-locate rows are indistinguishable in the trace.
    bool lli_epoch{ false };
    /// 1 when the anchor clause (§14.5) turned a would-be narrowing into the
    /// all-band response because this station anchors a family of this
    /// satellite.  @ref exonerated stays populated: the evidence was sound, and
    /// the datum topology, not the evidence, refuses the narrowing.
    int anchor_colocate{ 0 };
};

/// A silence is "weak" above this threshold, in wide-lane cycles.  6 cycles is
/// the low end of the harness's injected slip sizes, so a pair whose gate stood
/// above it did not exclude the event class the scope responds to.
inline constexpr double kWeakSilenceWlCyc = 6.0;

/**
 * @brief The pure rule of §14.2.
 *
 * Total over its inputs and free of I/O, config and global state, so the seven
 * worked cases of the specification are unit-testable directly.
 *
 * @param live_bands      The arc's tracked phase bands B (the station's live
 *                        ambiguity signals for this satellite).  Empty ⇒ nothing
 *                        to scope.
 * @param ev              This epoch's evidence for the (station, satellite).
 * @param min_arc_epochs  Maturity bar a silent pair must reach to exonerate.
 * @param station_anchors_satellite  True when the station currently anchors any
 *        phase-bias family of this satellite (any band).  Anchor clause (§14.5):
 *        a would-be narrowing there becomes
 *        @ref CommonBandOutcome::AnchorColocate instead, so the satellite's
 *        families are never left realized at different stations.
 * @return The verdict.  @c implicated is always a set a drain can intersect
 *         with; on every non-narrowed outcome it equals @p live_bands.
 */
CommonBandVerdict networkCommonBandVerdict(const std::set<std::string>& live_bands,
                                           const CommonBandEvidence& ev,
                                           int min_arc_epochs,
                                           bool station_anchors_satellite = false);

/**
 * @brief LLI-epoch verdict (rule F3, §14.12), for an epoch the §14.2 rule
 *        cannot see.
 *
 * Formed only when the receiver flagged at least one band of this (station,
 * satellite) and no MW pair flagged.  On the uncombined path that means every MW
 * sample of the satellite was withheld (§14.11), so the §14.2 rule has nothing
 * to weigh.  The receiver's bits are taken as the scope directly:
 * `implicated = lli_signals ∩ live_bands`, with no exoneration step, because the
 * LLI drain already frees exactly those bands and trusts them that far.  The
 * verdict makes every sibling drain firing in the same epoch (each IF-TD pair
 * refloat) follow the same set instead of freeing both of its bands.
 *
 * When an MW pair did flag at an LLI epoch this function is not called: the
 * §14.2 rule runs, and it already implicates every LLI band (a band with a
 * loss-of-lock bit is implicated regardless).
 *
 * @param live_bands    The arc's tracked phase bands.
 * @param ev            This epoch's evidence; only @ref
 *                      CommonBandEvidence::lli_signals is read.
 * @param station_anchors_satellite  True when the station currently anchors any
 *        phase-bias family of this satellite.  The anchor clause (§14.5) applies
 *        here as on the normal path, for the same reason (§14.12.5): narrowing on
 *        an anchor arc frees only the flagged band's family, so the election
 *        hands that family over alone and the satellite's per-frequency
 *        references end up at different stations.  Without the clause on this
 *        path, the storm arm's split-referenced satellites rose from 6.6 % to
 *        14.4 % per epoch, and most of the added single-family anchor handovers
 *        fell on arcs carrying an F3 verdict.
 * @return A @ref CommonBandOutcome::LliOnly verdict, or
 *         @ref CommonBandOutcome::AnchorColocate when the clause refuses it.
 *         @c narrowed is true only when the flagged set is a strict subset of
 *         @p live_bands; otherwise the scope would equal the unscoped response
 *         and there is nothing to gate.  @c implicated is @p live_bands whenever
 *         no flagged band is live, so a caller that registers the verdict anyway
 *         cannot restrict a drain by accident.
 */
CommonBandVerdict networkCommonBandLliVerdict(const std::set<std::string>& live_bands,
                                             const CommonBandEvidence& ev,
                                             bool station_anchors_satellite = false);

/// Per-epoch reading of how fragmented the phase-bias datum is across stations.
struct SplitRefCount {
    /// Satellites with >= 2 anchored families whose anchors are not all at the
    /// same station: the population whose per-frequency biases are realized at
    /// stations whose arc levels walk independently.
    int split{ 0 };
    /// Satellites with >= 2 anchored families: the population that can split,
    /// and the denominator for @ref split.
    int multi_anchored{ 0 };
};

/**
 * @brief Count split-referenced satellites in an anchor incumbent map.
 *
 * Always on and independent of the scope knob (invariant #4): it measures the
 * effect of the anchor clause, so it must read the same on a run with the scope
 * off.  Cost is one pass over the map, which holds one entry per anchored
 * (satellite, signal).
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
 * Only narrowed verdicts reach @ref narrowed_at: a drain that finds no entry
 * for its (station, satellite) runs the unscoped response, which makes the off
 * path and every fallback structurally identical.
 */
struct CommonBandEpoch {
    /// True when `network.cycle_slip.common_band_scope` is armed.  With it
    /// false the census still runs (invariant #4), but @ref narrowed_at stays
    /// empty and no event row is written.
    bool armed{ false };

    /// (station index, satellite) → verdict, narrowed verdicts only.
    std::map<std::pair<int, std::string>, CommonBandVerdict> narrowed_at;

    /// Every verdict formed this epoch, narrowed or not, in station/satellite
    /// order: the source of the event rows.
    std::vector<std::pair<std::pair<int, std::string>, CommonBandVerdict>> all;

    /// @name Per-epoch census (folded into NetworkServiceCounters by the caller)
    /// @{
    long long verdicts{ 0 };          ///< (station, satellite) pairs with ≥1 fired MW pair.
    long long narrowed_n{ 0 };        ///< Of those, narrowed by positive exoneration.
    long long all_no_exoneration{ 0 };///< Fallback: a pair fired, nothing exonerated.
    long long all_inconsistent{ 0 };  ///< Fallback: a pair fired, every band exonerated.
    long long bands_kept{ 0 };        ///< Bands spared by a narrowed verdict (Σ|exonerated|).
    long long iftd_veto{ 0 };         ///< Silent pairs refused as exonerators by an IF-TD flag.
    /// Silent pairs refused because their IF-TD sample is the chain's reference
    /// (dd ≡ 0, so the pooled test cannot flag it; §22).  Nonzero only with
    /// `common_band_ref_arc` armed.  This is the clause's on-path telemetry
    /// (invariant #4): an armed leg on a known reference arc that reports 0 here
    /// has dead wiring, not a quiet arc.
    long long iftd_ref_arc{ 0 };
    long long immature_pairs{ 0 };    ///< Silent pairs refused as exonerators for arc age.
    long long lli_shadow{ 0 };        ///< Silent pairs refused because a band carried an LLI at the previous epoch (rule F2, §14.11).
    /// Verdicts formed from the receiver's LLI bits alone (F3, §14.12), at an
    /// epoch where every MW sample of the satellite was withheld so the normal
    /// rule could not run.  Counted whether or not the knob is armed
    /// (invariant #4), and kept out of @ref verdicts, whose documented
    /// denominator is "(station, satellite) pairs with >= 1 fired MW pair".
    long long lli_verdicts{ 0 };
    /// Of those, the ones that restrict a drain (the flagged set is a strict
    /// subset of the live bands).
    long long lli_narrowed{ 0 };
    /// Of the LLI-epoch verdicts, the ones the anchor clause refused
    /// (§14.12.5).  Also counted in @ref anchor_colocate, which stays the total.
    /// Kept separate so a split-reference change can be attributed to the F3
    /// path or to the normal one.
    long long lli_anchor_colocate{ 0 };
    long long weak_silence{ 0 };      ///< Exonerating silences whose threshold exceeded 6 WL cycles.
    long long anchor_colocate{ 0 };   ///< Would-be narrowings refused because the station anchors the satellite (§14.5).
    long long would_narrow{ 0 };      ///< Knob off: verdicts that would have narrowed.
    /// @ref iftd_veto broken down by the IF-TD pair kind that vetoed
    /// ("L1C_L5Q" → n, §14.10).  Under `if_phase_td_pairs = first` only the one
    /// watched pair can appear here, which is the measurement the scope's claim
    /// rests on.
    std::map<std::string, long long> iftd_veto_by_pair;
    /// @ref iftd_ref_arc broken down by the IF-TD pair kind whose reference arc
    /// it was ("L1C_L5X" → n, §22).  The clause's cost is per chain: a satellite
    /// that is the reference of both L5 chains loses two pairs while the (1,2)
    /// reference loses one, which the total cannot show.
    std::map<std::string, long long> iftd_ref_arc_by_pair;
    /// @}

    /// Drop every per-epoch value (called once per processed epoch).
    void clear() {
        narrowed_at.clear();
        all.clear();
        verdicts = narrowed_n = all_no_exoneration = all_inconsistent = 0;
        bands_kept = iftd_veto = immature_pairs = weak_silence = lli_shadow = 0;
        anchor_colocate = would_narrow = iftd_ref_arc = 0;
        lli_verdicts = lli_narrowed = lli_anchor_colocate = 0;
        iftd_veto_by_pair.clear();
        iftd_ref_arc_by_pair.clear();
    }

    /**
     * @brief The bands a drain may free for (@p i, @p sat).
     * @return Pointer to the implicated set when this (station, satellite) has a
     *         narrowed verdict; @c nullptr otherwise, meaning the unscoped
     *         response, which every drain must then run unchanged.
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
 * diverge.  A null @p cbs (the knob off), an unarmed epoch, or a (station,
 * satellite) with no narrowed verdict answers true for every signal: the
 * unscoped response.
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
