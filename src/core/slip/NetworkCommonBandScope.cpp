/**
 * @file NetworkCommonBandScope.cpp
 * @brief COMMON-BAND SCOPE (SPEC_event_harness.md §14) — the pure rule and the
 *        per-epoch verdict seam.
 *
 * Two pieces live here and nothing else:
 *
 *  1. @ref networkCommonBandVerdict — the §14.2 rule as a total function over
 *     synthetic evidence, so the specification's worked cases are unit tests.
 *  2. @ref networkCommonBandVerdictEpoch — the seam the PASS-A slip scan calls
 *     ONCE per epoch, immediately before the drains: it turns the per-station
 *     PASS-A evidence into one verdict per (station, satellite), previews the
 *     IF-TD pooled flag so the veto can be applied before the drain that makes
 *     it, writes the event rows and fills the census.
 *
 * ### Why the IF-TD flag has to be PREVIEWED
 *
 * On the network path every station detector runs in DEFERRED mode
 * (`setDeferFlagging(true)`): it caches its between-epoch IF double differences
 * and flags nothing, and the FLAG is a network-wide pooled decision taken inside
 * @c networkSlipIfTdDriver — which runs AFTER the MW/LLI drain.  The verdict
 * must exist before that drain (§14.2: one verdict per epoch, formed before the
 * drains), so the flag is recomputed here READ-ONLY.  Both the preview and the
 * driver call the same @c ifTdPoolStats / @c ifTdShrunkScale helpers and the
 * same threshold expression, and the preview touches no detector or station
 * state — in particular it does NOT use `if_pair_scale[pair]`, whose
 * `operator[]` would insert, but `find`.
 */

#include "slip/NetworkCommonBandScope.hpp"

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "slip/IfPhaseSlipDetector.hpp"
#include "slip/NetworkSlipQc.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

namespace GnssProcessor {

namespace {

/// Render a pair for the basis string.  `+` and not `,`: the event writer keeps
/// `detail` one CSV field by rewriting every comma to a semicolon, which would
/// make `mw(L1C,L2W)` read as a separator.
std::string pairText(const CommonBandPair& p) {
    return p.first + "+" + p.second;
}

/// Render a silence threshold in wide-lane cycles for the basis string; `?`
/// when the sample left before a threshold existed.
std::string thrText(double thresh_wl) {
    if (!std::isfinite(thresh_wl)) return "?WL";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fWL", thresh_wl);
    return buf;
}

/// One IF-TD pair evaluation, as the read-only preview reconstructs it for a
/// (station, satellite).  @c is_ref is the detector's own flag
/// (@c IfJumpInfo::is_reference) and is carried, not dropped, because a sample
/// on the chain's reference has `dd ≡ 0` — so @c flagged is structurally false
/// there and says nothing (§22).
struct IfTdPreview {
    CommonBandPair pair;
    bool           flagged{ false };
    bool           is_ref{ false };
};

/// Render an ordered signal set as `a;b;c`, or `none` when empty.
std::string setText(const std::set<std::string>& s) {
    if (s.empty()) return "none";
    std::string out;
    for (const auto& v : s) {
        if (!out.empty()) out += ";";
        out += v;
    }
    return out;
}

} // namespace

CommonBandVerdict networkCommonBandVerdict(const std::set<std::string>& live_bands,
                                           const CommonBandEvidence& ev,
                                           int min_arc_epochs,
                                           bool station_anchors_satellite) {
    CommonBandVerdict v;
    // Every non-narrowed outcome returns TODAY'S response, and today's response
    // is "free every band any drain would have freed" — the full tracked set.
    v.implicated = live_bands;

    // FALLBACK 1 — no MW pair fired.  Not a verdict at all: there is no response
    // to scope, so this is not counted against the census either.
    if (ev.flagged_pairs.empty()) {
        v.outcome = CommonBandOutcome::NoFlag;
        return v;
    }

    // Positive exoneration.  Ordered containers throughout, so the basis string
    // and every counter are deterministic.
    std::string basis;
    auto append = [&basis](const std::string& t) {
        if (!basis.empty()) basis += ";";
        basis += t;
    };

    std::set<std::string> cleared;          // bands some silent, un-vetoed pair covers
    std::set<CommonBandPair> iftd_noted;    // IF-TD flags the veto branch already named
    for (const auto& [pr, st] : ev.processed) {
        // FLAGGED beats everything: a pair that emitted an MwSlipResult this
        // epoch — admitted to the refloat path OR dropped on the admission
        // gate — is not silent.  The emitted set is the authority, not the
        // detector's advisory `st.flagged`, because the emitted set is what the
        // drains act on.
        if (ev.flagged_pairs.count(pr)) continue;
        if (st.arc_n < min_arc_epochs) {
            // An arc too young to have flagged cannot testify that it didn't.
            ++v.immature_pairs;
            append("mw(" + pairText(pr) + ")immature(n=" + std::to_string(st.arc_n) + ")");
            continue;
        }
        // RULE F2 (§14.11) — the LLI SHADOW.  One of this pair's bands carried a
        // receiver loss-of-lock bit at the PREVIOUS epoch, so the detector
        // withheld the pair's sample there and preserved the arc, and THIS
        // sample is the first after the gap.  The count therefore reads mature
        // while the lane saw nothing at the epoch the receiver declared broken —
        // and a band-common slip leaves the wide lane unmoved anyway.  Treated
        // exactly as immature: refused, counted, named.  Measured on the storm
        // bed: all five spurious `keep` verdicts in 2 994 sat one epoch after an
        // LLI, and in every one the exonerating lane CONTAINED the flagged band.
        // Band-scoped, so a lane of two unflagged bands still exonerates — which
        // is what leaves the 853 correct late narrowings untouched.
        if (st.lli_shadow) {
            ++v.lli_shadow;
            append("mw(" + pairText(pr) + ")lli_shadow(n=" + std::to_string(st.arc_n) + ")");
            continue;
        }
        const std::string desc = "mw(" + pairText(pr) + ")silent(n="
                                 + std::to_string(st.arc_n) + ",thr=" + thrText(st.thresh_wl) + ")";
        // §22 THE REFERENCE-ARC CLAUSE.  This pair's IF-TD sample is the chain's
        // REFERENCE satellite, whose double difference is identically 0 by
        // construction, so the pooled test `|dd − med| > thr` could not have
        // flagged it however large the slip.  The veto below is therefore
        // structurally unavailable here, and the silence is not a clean bill of
        // health — it is the ABSENCE of a test.  Refused exactly as `immature`
        // and `lli_shadow` are: counted, named, and never listed as `silent`.
        // Field-measured (§20.1): an equal-count L1C+L5X slip on GOPE/G24 — the
        // reference of both L5 chains — narrowed to the innocent band under
        // `first` AND `all`, while the same injection at G25 vetoed at 29×
        // threshold.  Empty unless `common_band_ref_arc` is armed, which is what
        // keeps the knob-off path byte-identical.
        if (ev.iftd_untested.count(pr)) {
            ++v.iftd_ref_arc;
            v.iftd_ref_arc_pairs.insert(pr);
            append(desc + ";iftd(" + pairText(pr) + ")=ref_arc");
            continue;
        }
        if (ev.iftd_flagged.count(pr)) {
            // The EQUAL-COUNT trap: L1 and L2 slipping by the same integer leave
            // MW(1,2) silent, and only the iono-free combination sees it.
            ++v.iftd_veto;
            v.iftd_veto_pairs.insert(pr);   // §14.10: the veto's own identity
            append(desc + ";iftd(" + pairText(pr) + ")=flagged");
            iftd_noted.insert(pr);
            continue;
        }
        append(desc);
        // Reported, not acted on: the pair still exonerates.  This counts how
        // much of the narrowing rests on a gate too loose to have excluded the
        // slip sizes the scope is responding to.
        if (std::isfinite(st.thresh_wl) && st.thresh_wl > kWeakSilenceWlCyc)
            ++v.weak_silence;
        cleared.insert(pr.first);
        cleared.insert(pr.second);
    }
    for (const auto& pr : ev.flagged_pairs)
        append("mw(" + pairText(pr) + ")flag");
    for (const auto& pr : ev.iftd_flagged)
        if (!iftd_noted.count(pr)) append("iftd(" + pairText(pr) + ")=flagged");
    for (const auto& pr : ev.iftd_silent)
        append("iftd(" + pairText(pr) + ")silent");
    append("lli=" + setText(ev.lli_signals));
    v.basis = basis;

    // A band with a loss-of-lock bit is implicated regardless of what any pair
    // says: the receiver itself reported the arc broken.
    for (const auto& b : live_bands)
        if (cleared.count(b) && !ev.lli_signals.count(b)) v.exonerated.insert(b);

    // FALLBACK 2 — a pair fired and nothing was exonerated.
    if (v.exonerated.empty()) {
        v.outcome = CommonBandOutcome::NoExoneration;
        return v;
    }

    std::set<std::string> impl;
    for (const auto& b : live_bands)
        if (!v.exonerated.count(b)) impl.insert(b);

    // FALLBACK 3 — every tracked band was exonerated although a pair fired.  The
    // evidence contradicts itself (e.g. (1,2) fires alone while (1,5) and (2,5)
    // are both silent) and the honest response is today's.
    if (impl.empty()) {
        v.outcome = CommonBandOutcome::Inconsistent;
        v.exonerated.clear();
        return v;
    }

    // FALLBACK 4 — the ANCHOR CLAUSE (§14.5).  The evidence is sound and would
    // narrow, but this station currently anchors a phase-bias family of this
    // satellite.  Narrowing there frees only the implicated band's family, so the
    // election hands THAT family to a successor and leaves the satellite's other
    // families pinned here: the per-frequency biases end up realized at different
    // stations whose arc levels walk independently.  That is what the iteration-1
    // storm arm paid for (split satellite-epochs 6.6 % → 16.5 %, 27 % in hour 6,
    // consumer settled p50 0.174 → 0.230 m with every producer channel equal or
    // better).  On an anchor arc the response therefore stays ALL-BAND so every
    // family moves together in one epoch and the sticky election can place them
    // at ONE successor.  `exonerated` is left populated: the evidence was right;
    // the datum topology is what refuses to act on it.
    if (station_anchors_satellite) {
        v.outcome         = CommonBandOutcome::AnchorColocate;
        v.narrowed        = false;
        v.anchor_colocate = 1;
        v.implicated      = live_bands;
        return v;
    }

    v.implicated = std::move(impl);
    v.outcome    = CommonBandOutcome::Narrowed;
    v.narrowed   = true;
    return v;
}

CommonBandVerdict networkCommonBandLliVerdict(const std::set<std::string>& live_bands,
                                             const CommonBandEvidence& ev,
                                             bool station_anchors_satellite) {
    CommonBandVerdict v;
    v.outcome   = CommonBandOutcome::LliOnly;
    v.lli_epoch = true;
    // The receiver's bits, intersected with what the drains can actually act on.
    std::set<std::string> impl;
    for (const auto& b : live_bands)
        if (ev.lli_signals.count(b)) impl.insert(b);
    // No flagged band is live: there is nothing to scope, and `implicated` must
    // stay the full set so a caller that registers this verdict anyway cannot
    // restrict a drain by accident.
    v.implicated = impl.empty() ? live_bands : impl;
    v.narrowed   = (!impl.empty() && impl.size() < live_bands.size());
    // No exoneration step: the bands the receiver did NOT flag are not
    // "exonerated" by any test, they are simply outside the scope the receiver
    // named.  Leaving `exonerated` empty keeps the bands_kept census honest.
    v.basis = "lli=" + setText(ev.lli_signals) + ";mw=withheld";

    // §14.12.5 — the ANCHOR CLAUSE, the same one the normal path runs (§14.5) and
    // for the same reason.  Narrowing on an arc that anchors a family of this
    // satellite frees only the flagged band's family, so the election hands THAT
    // family to a successor and leaves the others pinned here: the satellite's
    // per-frequency references separate and their arc levels walk independently.
    // Leaving F3 without the clause re-entered iteration 2's defect through the
    // LLI door — measured on the storm arm, split-referenced satellites 3.5 → 7.6
    // of 52.8 per epoch (6.6 % → 14.4 %) and single-family anchor handovers
    // 57 → 103, with 49 of the new ones on an arc carrying an F3 verdict that
    // epoch, while the normal path's `keep` rows produced ZERO.  The §14.10 S3
    // leg could not see it: S21000SIM anchors nothing.
    //
    // The clause is applied AFTER the basis is built, so the row still names the
    // evidence that WOULD have narrowed — it is the datum topology that refuses
    // to act on it, not the evidence.
    if (station_anchors_satellite && v.narrowed) {
        v.outcome         = CommonBandOutcome::AnchorColocate;
        v.narrowed        = false;
        v.anchor_colocate = 1;
        v.implicated      = live_bands;
    }
    return v;
}

void networkCommonBandVerdictEpoch(const PassABuffers& pa,
                                   const GnssConfig& config_,
                                   const std::vector<NetStation>& stations,
                                   const std::vector<bool>& present,
                                   int i_lo, int n_sta_total,
                                   const SyncedEpoch& se,
                                   NetworkRunTraces& traces,
                                   const std::shared_ptr<spdlog::logger>& log,
                                   const std::map<std::string, std::string>* anchor_incumbent,
                                   CommonBandEpoch& out) {
    (void)log;
    out.clear();
    out.armed = config_.networkCycleSlipCommonBandScope();
    const int min_arc = config_.networkCycleSlipCommonBandMinArcEpochs();

    // ---- IF-TD FLAG PREVIEW (read-only; see the file banner) ---------------
    // Keyed (station, satellite) → EVERY pair the detector evaluated this epoch,
    // each with whether the pooled test flags it.  Empty when the IF-TD detector
    // is not armed: no evaluation, no veto.
    //
    // §14.10: with `network.cycle_slip.if_phase_td_pairs = all` a satellite has
    // one entry per band pair rather than one in total, which is what lets the
    // veto reach a silent MW lane whose own IF pair was never watched before —
    // the S37/G20 shape, where MW(L1C+L5Q) stayed silent through a −31 / −29
    // two-band slip and the response freed the one band that had not moved.
    // With it `first` there is exactly one entry per (station, satellite) and
    // every expression below reduces to the pre-knob one.
    //
    // §22: each evaluation also carries the sample's `is_ref`, which the preview
    // used to compute and drop.  It is what separates "the pooled test ran and
    // said nothing" from "the pooled test could not run" — see the clause in
    // @ref networkCommonBandVerdict.
    std::map<std::pair<int, std::string>, std::vector<IfTdPreview>> iftd;
    const bool iftd_all_pairs =
        config_.networkCycleSlipIfPhaseTdPairs() == NetworkIfPhaseTdPairs::All;
    // §22 THE REFERENCE-ARC CLAUSE.  Off ⇒ `iftd_untested` is never populated,
    // every sample folds into flagged/silent exactly as before, and the rule's
    // new branch is unreachable: the OFF path is byte-identical.
    const bool ref_arc_clause = config_.networkCycleSlipCommonBandRefArc();
    if (config_.networkCycleSlipIfPhaseTd()) {
        const double k_mad        = config_.networkCycleSlipIfPhaseTdKMad();
        const double floor_frac   = config_.networkCycleSlipIfPhaseTdFloorFrac();
        const double shrink_kappa = config_.networkCycleSlipIfPhaseTdShrinkKappa();

        struct PrevSample { int sta; std::string sat, sig1, sig2;
                            double dd, el, lambda; bool is_ref; };
        std::map<std::string, std::vector<PrevSample>> by_pair;
        for (int i = i_lo; i < n_sta_total; ++i) {
            if (i < 0 || static_cast<std::size_t>(i) >= stations.size()) continue;
            if (!present[static_cast<std::size_t>(i)] || !stations[i].if_slip) continue;
            // Exactly the driver's two source branches, so the preview pools the
            // same samples the decision will (§14.10).
            if (!iftd_all_pairs) {
                for (const auto& [sat, jp] : stations[i].if_slip->getLastIfJumps()) {
                    if (!jp.valid || !std::isfinite(jp.delta_grad_m)) continue;
                    if (jp.sig1.empty() || jp.sig2.empty()) continue;
                    by_pair[jp.sig1 + "_" + jp.sig2].push_back(
                        { i, sat, jp.sig1, jp.sig2, jp.delta_grad_m,
                          jp.elevation_deg, jp.lambda_if_m, jp.is_reference });
                }
            } else {
                for (const auto& [key, jp] : stations[i].if_slip->getLastIfJumpsAllPairs()) {
                    if (!jp.valid || !std::isfinite(jp.delta_grad_m)) continue;
                    if (jp.sig1.empty() || jp.sig2.empty()) continue;
                    by_pair[key.second].push_back(
                        { i, key.first, jp.sig1, jp.sig2, jp.delta_grad_m,
                          jp.elevation_deg, jp.lambda_if_m, jp.is_reference });
                }
            }
        }

        for (const auto& [pair, samples] : by_pair) {
            std::vector<double> dd, el;
            std::vector<char>   ref;
            dd.reserve(samples.size());
            el.reserve(samples.size());
            ref.reserve(samples.size());
            for (const auto& s : samples) {
                dd.push_back(s.dd);
                el.push_back(s.el);
                ref.push_back(s.is_ref ? char{ 1 } : char{ 0 });
            }
            const IfTdPairPool pstat = ifTdPoolStats(dd, el, ref);

            // Group by station exactly as the driver does — each station-pair
            // thresholds with its OWN shrunk scale.
            std::map<int, std::vector<const PrevSample*>> by_sta;
            for (const auto& s : samples) by_sta[s.sta].push_back(&s);
            for (const auto& [sta, plist] : by_sta) {
                // find, NOT operator[]: the preview must not insert into the
                // station's persistent EWMA map.  An absent entry is
                // {ewma 0, count 0}, which is what operator[] would have
                // default-constructed anyway.
                NetStation::IfPairScale sc{};
                const auto sit = stations[sta].if_pair_scale.find(pair);
                if (sit != stations[sta].if_pair_scale.end()) sc = sit->second;
                const double s_eff =
                    ifTdShrunkScale(sc.count, sc.ewma_scale, pstat.s_pool, shrink_kappa);
                for (const PrevSample* sp : plist) {
                    const double emap    = ifElevMap(sp->el);
                    const double floor_m = floor_frac * sp->lambda;
                    const double thr     = std::max(k_mad * s_eff * emap, floor_m);
                    const bool   flagged = std::abs(sp->dd - pstat.med) > thr;
                    iftd[{ sp->sta, sp->sat }].push_back(
                        IfTdPreview{ { sp->sig1, sp->sig2 }, flagged, sp->is_ref });
                }
            }
        }
    }

    // ---- ONE VERDICT PER (station, satellite) ------------------------------
    for (int i = i_lo; i < n_sta_total; ++i) {
        if (i < 0 || static_cast<std::size_t>(i) >= pa.sta_cb_ev.size()) continue;
        if (!present[static_cast<std::size_t>(i)] || !stations[i].sm) continue;
        for (const auto& [sat, ev_in] : pa.sta_cb_ev[static_cast<std::size_t>(i)]) {
            // §14.12 RULE F3.  An epoch the receiver flagged and no MW pair could
            // see (every sample of the satellite withheld, §14.11) used to fall
            // through FALLBACK 1 as "no verdict", which means the drains run
            // UNSCOPED — harmless while the LLI drain is the only actor, since it
            // is already scoped by the receiver's bits, but not once an IF-TD pair
            // containing a flagged band fires in the same epoch.
            const bool lli_only = ev_in.flagged_pairs.empty()
                               && !ev_in.lli_signals.empty();
            if (ev_in.flagged_pairs.empty() && !lli_only) continue;  // FALLBACK 1, uncounted

            // Fold in this epoch's IF-TD evaluation (the preview above).
            CommonBandEvidence ev = ev_in;
            const auto iit = iftd.find({ i, sat });
            if (iit != iftd.end())
                for (const auto& p : iit->second) {
                    // §22: a sample on the chain's REFERENCE has dd ≡ 0, so
                    // `flagged` is structurally false there and the pair is
                    // UNTESTED, not silent.  Three-way and exclusive: with the
                    // clause off the first arm never fires and this is the
                    // pre-card two-way fold, byte for byte.
                    if (ref_arc_clause && p.is_ref) ev.iftd_untested.insert(p.pair);
                    else if (p.flagged)             ev.iftd_flagged.insert(p.pair);
                    else                            ev.iftd_silent.insert(p.pair);
                }

            // B = the arc's TRACKED phase bands, i.e. the live ambiguity signals
            // the drains would act on.  Same source the §Z scope reads.
            std::set<std::string> live;
            for (const auto& kv : stations[i].sm->getAmbiguities()) {
                const auto parts = StateKeys::parseAmbiguityKey(kv.first);
                if (parts && parts->satellite == sat) live.insert(parts->signal);
            }
            if (live.size() < 2) continue;  // nothing a band scope can narrow

            // §14.5 ANCHOR CLAUSE input: does THIS station anchor any family of
            // this satellite?  Same map and the same match the anchor-slip
            // handover staging uses (anchor_incumbent[<sat>_<signal>] == name),
            // scanned over every band rather than one.  The map is the election's
            // sticky incumbent, i.e. last epoch's anchors — which is exactly the
            // set the drains are about to disturb.
            bool anchors_sat = false;
            if (anchor_incumbent) {
                const std::string pfx = sat + "_";
                for (auto ait = anchor_incumbent->lower_bound(pfx);
                     ait != anchor_incumbent->end()
                     && ait->first.compare(0, pfx.size(), pfx) == 0; ++ait) {
                    if (ait->second == stations[i].name) { anchors_sat = true; break; }
                }
            }

            // §14.12: the LLI-only path takes the receiver's bits directly; the
            // ANCHOR CLAUSE is deliberately NOT applied to it, because the LLI
            // drain ALREADY frees exactly those bands today — so F3 changes only
            // the sibling drains, and refusing to scope them here would restore
            // the very all-band free the rule exists to remove.  Whether that
            // trade costs co-location is measurable: the always-on
            // split-referenced-satellite census reads it.
            const CommonBandVerdict v =
                lli_only ? networkCommonBandLliVerdict(live, ev, anchors_sat)
                         : networkCommonBandVerdict(live, ev, min_arc, anchors_sat);

            if (lli_only) {
                ++out.lli_verdicts;
                if (v.narrowed) ++out.lli_narrowed;
                // §14.12.5: folded into the SHARED anchor counter so the run-end
                // total stays the honest population, and into its own so the two
                // paths' co-locates are distinguishable (the storm arm needed
                // exactly that split to attribute the regression).
                if (v.outcome == CommonBandOutcome::AnchorColocate) {
                    out.anchor_colocate     += v.anchor_colocate;
                    out.lli_anchor_colocate += v.anchor_colocate;
                }
                out.all.emplace_back(std::make_pair(i, sat), v);
                if (out.armed && v.narrowed) out.narrowed_at[{ i, sat }] = v;
                continue;
            }

            ++out.verdicts;
            out.iftd_veto      += v.iftd_veto;
            for (const auto& pr : v.iftd_veto_pairs)   // §14.10 breakdown
                ++out.iftd_veto_by_pair[pr.first + "_" + pr.second];
            out.iftd_ref_arc   += v.iftd_ref_arc;      // §22
            for (const auto& pr : v.iftd_ref_arc_pairs)
                ++out.iftd_ref_arc_by_pair[pr.first + "_" + pr.second];
            out.immature_pairs += v.immature_pairs;
            out.lli_shadow     += v.lli_shadow;       // §14.11 F2
            out.weak_silence   += v.weak_silence;
            switch (v.outcome) {
                case CommonBandOutcome::Narrowed:
                    ++out.narrowed_n;
                    out.bands_kept += static_cast<long long>(v.exonerated.size());
                    if (!out.armed) ++out.would_narrow;
                    break;
                case CommonBandOutcome::NoExoneration:  ++out.all_no_exoneration; break;
                case CommonBandOutcome::Inconsistent:   ++out.all_inconsistent;   break;
                case CommonBandOutcome::AnchorColocate: ++out.anchor_colocate;    break;
                case CommonBandOutcome::NoFlag:         break;   // unreachable here
                case CommonBandOutcome::LliOnly:        break;   // §14.12, handled above
            }
            out.all.emplace_back(std::make_pair(i, sat), v);
            if (out.armed && v.narrowed) out.narrowed_at[{ i, sat }] = v;
        }
    }

    // ---- EVENT ROWS — armed only (a knob-OFF run must stay byte-identical) --
    // Invariant #7 wants an IDENTITY at the edge, not a counter alone: the
    // narrowed row names what was spared and on what evidence, and the fallback
    // row names WHY the response stayed all-band.  Both are cheap — the
    // population is one row per (station, satellite) that actually fired.
    if (!out.armed) return;
    for (const auto& [key, v] : out.all) {
        const int          i   = key.first;
        const std::string& sat = key.second;
        const std::string  sta = (static_cast<std::size_t>(i) < stations.size())
                                     ? stations[static_cast<std::size_t>(i)].name
                                     : std::string{};
        if (v.outcome == CommonBandOutcome::LliOnly) {
            // §14.12: `keep` whether or not it restricts anything — the row's
            // job is to say that this epoch HAD a verdict, which is exactly what
            // was missing before.  `exonerated` is absent on purpose: nothing was
            // tested, the receiver named the scope.
            traces.emitEvent(se.time, "T0_band_scope", "common_band", sat, sta,
                             std::nullopt, std::nullopt, "keep",
                             "rule=lli_epoch;implicated=" + setText(v.implicated)
                                 + ";narrowed=" + std::string(v.narrowed ? "1" : "0")
                                 + ";basis=" + v.basis);
        } else if (v.narrowed) {
            traces.emitEvent(se.time, "T0_band_scope", "common_band", sat, sta,
                             std::nullopt, std::nullopt, "keep",
                             "implicated=" + setText(v.implicated)
                                 + ";exonerated=" + setText(v.exonerated)
                                 + ";basis=" + v.basis);
        } else {
            traces.emitEvent(se.time, "T0_band_scope", "common_band", sat, sta,
                             std::nullopt, std::nullopt, "all",
                             // §14.12.5: name the RULE on a co-locate row too, or
                             // the F3 path's refusals cannot be told from the
                             // normal path's in the trace.
                             (v.lli_epoch ? std::string("rule=lli_epoch;")
                                          : std::string{})
                                 + "reason="
                                 + (v.outcome == CommonBandOutcome::Inconsistent
                                        ? "inconsistent"
                                        : v.outcome == CommonBandOutcome::AnchorColocate
                                              ? "anchor_colocate" : "no_exoneration")
                                 + ";implicated=" + setText(v.implicated)
                                 + ";basis=" + v.basis);
        }
    }
}

} // namespace GnssProcessor
