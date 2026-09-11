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
    std::map<std::pair<int, std::string>,
             std::vector<std::pair<CommonBandPair, bool>>> iftd;
    const bool iftd_all_pairs =
        config_.networkCycleSlipIfPhaseTdPairs() == NetworkIfPhaseTdPairs::All;
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
                    iftd[{ sp->sta, sp->sat }].push_back({ { sp->sig1, sp->sig2 }, flagged });
                }
            }
        }
    }

    // ---- ONE VERDICT PER (station, satellite) ------------------------------
    for (int i = i_lo; i < n_sta_total; ++i) {
        if (i < 0 || static_cast<std::size_t>(i) >= pa.sta_cb_ev.size()) continue;
        if (!present[static_cast<std::size_t>(i)] || !stations[i].sm) continue;
        for (const auto& [sat, ev_in] : pa.sta_cb_ev[static_cast<std::size_t>(i)]) {
            if (ev_in.flagged_pairs.empty()) continue;   // FALLBACK 1, uncounted

            // Fold in this epoch's IF-TD evaluation (the preview above).
            CommonBandEvidence ev = ev_in;
            const auto iit = iftd.find({ i, sat });
            if (iit != iftd.end())
                for (const auto& [pr, flagged] : iit->second) {
                    if (flagged) ev.iftd_flagged.insert(pr);
                    else         ev.iftd_silent.insert(pr);
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

            const CommonBandVerdict v =
                networkCommonBandVerdict(live, ev, min_arc, anchors_sat);

            ++out.verdicts;
            out.iftd_veto      += v.iftd_veto;
            for (const auto& pr : v.iftd_veto_pairs)   // §14.10 breakdown
                ++out.iftd_veto_by_pair[pr.first + "_" + pr.second];
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
        if (v.narrowed) {
            traces.emitEvent(se.time, "T0_band_scope", "common_band", sat, sta,
                             std::nullopt, std::nullopt, "keep",
                             "implicated=" + setText(v.implicated)
                                 + ";exonerated=" + setText(v.exonerated)
                                 + ";basis=" + v.basis);
        } else {
            traces.emitEvent(se.time, "T0_band_scope", "common_band", sat, sta,
                             std::nullopt, std::nullopt, "all",
                             std::string("reason=")
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
