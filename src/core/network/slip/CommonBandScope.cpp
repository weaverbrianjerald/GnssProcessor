/**
 * @file CommonBandScope.cpp
 * @brief Common-band slip scope: the pure rule and the per-epoch verdict seam.
 *
 * This file holds two pieces:
 *
 *  1. @ref networkCommonBandVerdict: the rule as a total function over
 *     synthetic evidence, so the worked cases are unit tests.
 *  2. @ref networkCommonBandVerdictEpoch: the seam the PASS-A slip scan calls
 *     once per epoch, immediately before the drains.  It turns the per-station
 *     PASS-A evidence into one verdict per (station, satellite), previews the
 *     IF-TD pooled flag so the veto can be applied before the drain that makes
 *     it, writes the event rows and fills the census.
 *
 * ### Why the IF-TD flag is previewed
 *
 * On the network path every station detector runs in deferred mode
 * (`setDeferFlagging(true)`): it caches its between-epoch IF double differences
 * and flags nothing.  The flag is a network-wide pooled decision taken inside
 * @c networkSlipIfTdDriver, which runs after the MW/LLI drain.  The verdict must
 * exist before that drain (one verdict per epoch, formed before the
 * drains), so the flag is recomputed here read-only.  The preview and the driver
 * call the same @c ifTdPoolStats / @c ifTdShrunkScale helpers and use the same
 * threshold expression.  The preview touches no detector or station state; in
 * particular it looks up `if_pair_scale` with `find`, not with `operator[]`,
 * which would insert.
 */

#include "network/slip/CommonBandScope.hpp"

#include "network/state/RunContext.hpp"
#include "config/GnssConfig.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/state/StationParams.hpp"   // the station's parameter set: its live bands
#include "network/diag/RunTraces.hpp"
#include "slip/IfPhaseSlipDetector.hpp"
#include "network/slip/SlipQc.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

// The run-end reports at the end of this file:
#include "network/runend/RunEnd.hpp"
#include <fmt/format.h>
#include <memory>
#include <string>

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
/// (@c IfJumpInfo::is_reference).  It is carried because a sample on the
/// chain's reference has `dd ≡ 0`, so @c flagged does not respond to a slip
/// there and carries no information.
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
    // Every non-narrowed outcome returns the unscoped response, "free every band
    // any drain would have freed", which is the full tracked set.
    v.implicated = live_bands;

    // Fallback 1: no MW pair fired.  This is not a verdict: there is no response
    // to scope, so it is not counted in the census either.
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
        // A flagged pair is never silent: one that emitted an MwSlipResult this
        // epoch, admitted to the refloat path or dropped on the admission gate,
        // is skipped first.  The emitted set is the authority, not the
        // detector's advisory `st.flagged`, because the emitted set is what the
        // drains act on.
        if (ev.flagged_pairs.count(pr)) continue;
        if (st.arc_n < min_arc_epochs) {
            // An arc too young to have flagged cannot testify that it did not.
            ++v.immature_pairs;
            append("mw(" + pairText(pr) + ")immature(n=" + std::to_string(st.arc_n) + ")");
            continue;
        }
        // LLI shadow.  One of this pair's bands carried a
        // receiver loss-of-lock bit at the previous epoch, so the detector
        // withheld the pair's sample there and preserved the arc, and this
        // sample is the first after the gap.  The count therefore reads mature
        // although the lane saw nothing at the epoch the receiver declared
        // broken, and a band-common slip leaves the wide lane unmoved anyway.
        // Treated as immature: refused, counted and named.  On the storm bed
        // every spurious `keep` verdict came one epoch after an LLI, on a lane
        // containing the flagged band.  Band-scoped, so a lane of two unflagged
        // bands still exonerates, which keeps the correct late narrowings.
        if (st.lli_shadow) {
            ++v.lli_shadow;
            append("mw(" + pairText(pr) + ")lli_shadow(n=" + std::to_string(st.arc_n) + ")");
            continue;
        }
        const std::string desc = "mw(" + pairText(pr) + ")silent(n="
                                 + std::to_string(st.arc_n) + ",thr=" + thrText(st.thresh_wl) + ")";
        // Reference-arc clause.  This pair's IF-TD sample is on the
        // chain's reference satellite, whose double difference is 0 by
        // construction, so the pooled test `|dd − med| > thr` does not respond
        // to a slip there however large.  The veto below is structurally
        // unavailable, and the silence is the absence of a test, not evidence of
        // a clean arc.  Refused in the same way as `immature` and `lli_shadow`
        // pairs: counted, named, and never listed as `silent`.  Without the
        // clause, an equal-count L1C+L5X slip on a satellite that is the
        // reference of both L5 chains narrows to the innocent band under both
        // `first` and `all`, while the same slip on a non-reference satellite is
        // vetoed.  The untested set is empty unless
        // `common_band_ref_arc` is armed, which keeps the knob-off path
        // byte-identical.
        if (ev.iftd_untested.count(pr)) {
            ++v.iftd_ref_arc;
            v.iftd_ref_arc_pairs.insert(pr);
            append(desc + ";iftd(" + pairText(pr) + ")=ref_arc");
            iftd_noted.insert(pr);
            continue;
        }
        if (ev.iftd_flagged.count(pr)) {
            // Equal-count case: L1 and L2 slipping by the same integer leave
            // MW(1,2) silent, and only the iono-free combination sees it.
            ++v.iftd_veto;
            v.iftd_veto_pairs.insert(pr);   // the veto's identity
            append(desc + ";iftd(" + pairText(pr) + ")=flagged");
            iftd_noted.insert(pr);
            continue;
        }
        append(desc);
        // Reported, not acted on: the pair still exonerates.  This counts how
        // much of the narrowing rests on a gate too loose to have excluded the
        // slip sizes the scope responds to.
        if (std::isfinite(st.thresh_wl) && st.thresh_wl > kWeakSilenceWlCyc)
            ++v.weak_silence;
        cleared.insert(pr.first);
        cleared.insert(pr.second);
    }
    for (const auto& pr : ev.flagged_pairs)
        append("mw(" + pairText(pr) + ")flag");
    for (const auto& pr : ev.iftd_flagged)
        if (!iftd_noted.count(pr)) append("iftd(" + pairText(pr) + ")=flagged");
    // The untested set gets the same tail as the flagged set.  Without
    // it, an untested pair the refusal branch never reached (MW-flagged,
    // immature, or not processed) would be missing from the row, and a reader
    // could not tell an evaluated, quiet lane from one whose IF-TD test could
    // not run.  Always `=ref_arc`, never `silent`.
    for (const auto& pr : ev.iftd_untested)
        if (!iftd_noted.count(pr)) append("iftd(" + pairText(pr) + ")=ref_arc");
    for (const auto& pr : ev.iftd_silent)
        append("iftd(" + pairText(pr) + ")silent");
    append("lli=" + setText(ev.lli_signals));
    v.basis = basis;

    // A band with a loss-of-lock bit is implicated regardless of what any pair
    // says: the receiver itself reported the arc broken.
    for (const auto& b : live_bands)
        if (cleared.count(b) && !ev.lli_signals.count(b)) v.exonerated.insert(b);

    // Fallback 2: a pair fired and nothing was exonerated.
    if (v.exonerated.empty()) {
        v.outcome = CommonBandOutcome::NoExoneration;
        return v;
    }

    std::set<std::string> impl;
    for (const auto& b : live_bands)
        if (!v.exonerated.count(b)) impl.insert(b);

    // Fallback 3: every tracked band was exonerated although a pair fired.  The
    // evidence contradicts itself (e.g. (1,2) fires alone while (1,5) and (2,5)
    // are both silent), so the response stays unscoped.
    if (impl.empty()) {
        v.outcome = CommonBandOutcome::Inconsistent;
        v.exonerated.clear();
        return v;
    }

    // Fallback 4: the anchor clause.  The evidence is sound and would
    // narrow, but this station currently anchors a phase-bias family of this
    // satellite.  Narrowing there frees only the implicated band's family, so the
    // election hands that family to a successor and leaves the satellite's other
    // families pinned here: the per-frequency biases end up realized at
    // different stations whose arc levels walk independently (the consumer cost
    // is recorded at CommonBandOutcome::AnchorColocate).  On an anchor arc the
    // response therefore stays all-band, so every family moves in the same epoch
    // and the sticky election can place them at one successor.  `exonerated`
    // stays populated: the evidence was right, and the datum topology is what
    // refuses to act on it.
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
    // The receiver's bits, intersected with what the drains can act on.
    std::set<std::string> impl;
    for (const auto& b : live_bands)
        if (ev.lli_signals.count(b)) impl.insert(b);
    // No flagged band is live: there is nothing to scope, and `implicated` stays
    // the full set so a caller that registers this verdict anyway cannot
    // restrict a drain by accident.
    v.implicated = impl.empty() ? live_bands : impl;
    v.narrowed   = (!impl.empty() && impl.size() < live_bands.size());
    // No exoneration step: the bands the receiver did not flag are not
    // exonerated by any test, they are outside the scope the receiver named.
    // Leaving `exonerated` empty keeps the bands_kept census limited to tested
    // bands.
    v.basis = "lli=" + setText(ev.lli_signals) + ";mw=withheld";

    // The anchor clause, the same one the normal path runs
    // and for the same reason.  Narrowing on an arc that anchors a family of
    // this satellite frees only the flagged band's family, so the election
    // hands that family to a successor and leaves the others pinned here: the
    // satellite's per-frequency references separate and their arc levels walk
    // independently.  Without the clause this path reintroduced that split
    // on the storm arm (split-referenced satellites 6.6 % to 14.4 % per epoch),
    // while the normal path produced none.
    //
    // The clause is applied after the basis is built, so the row still names
    // the evidence that would have narrowed: the datum topology, not the
    // evidence, refuses to act on it.
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

    // ---- IF-TD flag preview (read-only; see the file banner) ---------------
    // Keyed (station, satellite) to every pair the detector evaluated this epoch,
    // each with whether the pooled test flags it.  Empty when the IF-TD detector
    // is not armed: no evaluation, no veto.
    //
    // With `network.cycle_slip.if_phase_td_pairs = all` a satellite has
    // one entry per band pair rather than one in total, which lets the veto reach
    // a silent MW lane whose own IF pair `first` does not watch.  Without it, a
    // two-band slip that leaves MW(L1C+L5Q) silent can free the one band that did
    // not move.  With `first` there is exactly one entry per (station, satellite)
    // and every expression below reduces to the single-pair form.
    //
    // Each evaluation also carries the sample's `is_ref`, which separates
    // "the pooled test ran and did not flag" from "the pooled test could not
    // respond"; see the clause in @ref networkCommonBandVerdict.
    std::map<std::pair<int, std::string>, std::vector<IfTdPreview>> iftd;
    const bool iftd_all_pairs =
        config_.networkCycleSlipIfPhaseTdPairs() == NetworkIfPhaseTdPairs::All;
    // Reference-arc clause.  When off, `iftd_untested` is never populated,
    // every sample folds into flagged/silent, and the rule's clause branch is
    // unreachable: the off path is byte-identical.
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
            // The driver's two source branches, so the preview pools the same
            // samples the decision will.
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

            // Group by station as the driver does: each station-pair thresholds
            // with its own shrunk scale.
            std::map<int, std::vector<const PrevSample*>> by_sta;
            for (const auto& s : samples) by_sta[s.sta].push_back(&s);
            for (const auto& [sta, plist] : by_sta) {
                // find, not operator[]: the preview must not insert into the
                // station's persistent EWMA map.  An absent entry reads as
                // {ewma 0, count 0}, which is what operator[] would have
                // default-constructed.
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

    // ---- One verdict per (station, satellite) ------------------------------
    for (int i = i_lo; i < n_sta_total; ++i) {
        if (i < 0 || static_cast<std::size_t>(i) >= pa.sta_cb_ev.size()) continue;
        if (!present[static_cast<std::size_t>(i)]) continue;
        for (const auto& [sat, ev_in] : pa.sta_cb_ev[static_cast<std::size_t>(i)]) {
            // LLI-only verdict.  At an epoch the receiver flagged and no MW pair
            // could see (every sample of the satellite withheld),
            // fallback 1 alone would form no verdict and the drains would run
            // unscoped.  That is harmless while the LLI drain is the only actor,
            // since the receiver's bits already scope it, but not once an IF-TD
            // pair containing a flagged band fires in the same epoch.
            const bool lli_only = ev_in.flagged_pairs.empty()
                               && !ev_in.lli_signals.empty();
            if (ev_in.flagged_pairs.empty() && !lli_only) continue;  // fallback 1, not counted

            // Fold in this epoch's IF-TD evaluation (the preview above).
            CommonBandEvidence ev = ev_in;
            const auto iit = iftd.find({ i, sat });
            if (iit != iftd.end())
                for (const auto& p : iit->second) {
                    // A sample on the chain's reference has dd ≡ 0, so `flagged`
                    // does not respond to a slip there and the pair is untested,
                    // not silent.  The three arms are exclusive; with the
                    // clause off the first never fires and this is the two-way
                    // fold, byte for byte.
                    if (ref_arc_clause && p.is_ref) ev.iftd_untested.insert(p.pair);
                    else if (p.flagged)             ev.iftd_flagged.insert(p.pair);
                    else                            ev.iftd_silent.insert(p.pair);
                }

            // B = the arc's tracked phase bands, i.e. the live ambiguity signals
            // the drains would act on.
            std::set<std::string> live;
            for (const auto& [arc, slot] : stations[i].params.amb) {
                (void)slot;
                if (net::satKey(arc.sat) == sat) live.insert(net::signalKey(arc.signal));
            }
            if (live.size() < 2) continue;  // nothing a band scope can narrow

            // Anchor clause input: does this station anchor any family of
            // this satellite?  Same map and same match as the anchor-slip
            // handover staging (anchor_incumbent[<sat>_<signal>] == name),
            // scanned over every band rather than one.  The map is the
            // election's sticky incumbent, i.e. last epoch's anchors, which is
            // the set the drains are about to disturb.
            bool anchors_sat = false;
            if (anchor_incumbent) {
                const std::string pfx = sat + "_";
                for (auto ait = anchor_incumbent->lower_bound(pfx);
                     ait != anchor_incumbent->end()
                     && ait->first.compare(0, pfx.size(), pfx) == 0; ++ait) {
                    if (ait->second == stations[i].name) { anchors_sat = true; break; }
                }
            }

            // The LLI-only path takes the receiver's bits directly.  It
            // changes only the sibling drains, since the LLI drain already frees
            // exactly the flagged bands.  Both rules receive anchors_sat, so the
            // anchor clause applies on either path; the always-on
            // split-referenced-satellite census reads what co-location costs.
            const CommonBandVerdict v =
                lli_only ? networkCommonBandLliVerdict(live, ev, anchors_sat)
                         : networkCommonBandVerdict(live, ev, min_arc, anchors_sat);

            if (lli_only) {
                ++out.lli_verdicts;
                if (v.narrowed) ++out.lli_narrowed;
                // Anchor refusals on this path go into the shared anchor counter,
                // so the run-end total covers both paths, and into their own, so
                // the two paths' co-locates stay distinguishable.
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
            for (const auto& pr : v.iftd_veto_pairs)   // per pair kind
                ++out.iftd_veto_by_pair[pr.first + "_" + pr.second];
            out.iftd_ref_arc   += v.iftd_ref_arc;      // reference-arc refusals
            for (const auto& pr : v.iftd_ref_arc_pairs)
                ++out.iftd_ref_arc_by_pair[pr.first + "_" + pr.second];
            out.immature_pairs += v.immature_pairs;
            out.lli_shadow     += v.lli_shadow;       // LLI-shadow refusals
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
                case CommonBandOutcome::LliOnly:        break;   // handled above
            }
            out.all.emplace_back(std::make_pair(i, sat), v);
            if (out.armed && v.narrowed) out.narrowed_at[{ i, sat }] = v;
        }
    }

    // ---- Event rows, armed runs only (a knob-off run stays byte-identical) --
    // Each row carries an identity, not a counter alone: the
    // narrowed row names what was spared and on what evidence, and the fallback
    // row names why the response stayed all-band.  The population is one row
    // per verdict formed this epoch, so the rows are cheap.
    if (!out.armed) return;
    for (const auto& [key, v] : out.all) {
        const int          i   = key.first;
        const std::string& sat = key.second;
        const std::string  sta = (static_cast<std::size_t>(i) < stations.size())
                                     ? stations[static_cast<std::size_t>(i)].name
                                     : std::string{};
        if (v.outcome == CommonBandOutcome::LliOnly) {
            // `keep` whether or not the verdict restricts anything: the
            // row records that this epoch had a verdict.  `exonerated` is omitted
            // because nothing was tested; the receiver named the scope.
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
                             // Name the rule on a co-locate row too,
                             // or the LLI-only path's refusals cannot be told from the
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

// ---- Run-end reports ----

// Run-end report: the common-band scope verdicts.
int networkCommonBandScopeRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;

    // ---- Common-band scope (network.cycle_slip.common_band_scope) ------
    // Printed on every run, armed or not, for the same reason as the MW detect-only
    // admission line: the would-narrow count sizes the knob on a lineage without
    // running it armed, and an armed run reporting narrowed 0 while verdicts > 0 is
    // dead wiring, not a quiet network.  `bands kept` is the main
    // number: under the all-band union every live band of a fired arc would have been
    // freed.
    const auto& C = ctx.counters;
    const double ep = static_cast<double>(std::max<long long>(C.n_cb_split_epochs, 1));
    // Refusals of the reference-arc clause, with the per-chain breakdown
    // inline: each refusal costs one dead zone per chain whose reference the arc
    // is, so the total alone cannot say which chains bore the cost.
    std::string ref_arc_by;
    for (const auto& [pr, n] : C.n_cb_iftd_ref_arc_by_pair)
        ref_arc_by += fmt::format("{}{}: {}", ref_arc_by.empty() ? "" : ", ", pr, n);
    log->info("Network (common-band scope): verdicts {}, narrowed {} (bands kept "
              "{}), all-band {} (no exoneration {} / inconsistent {} / anchor "
              "co-locate {}), IF-TD vetoes {}, ref-arc refusals {}{}, "
              "immature pairs {}, LLI shadows {}, "
              "LLI-epoch verdicts {} ({} narrowed, {} anchor co-locate), "
              "weak silences {}; "
              "split-referenced satellites mean {:.1f} / max {} of {:.1f} anchored "
              "[knob {}{}].",
              C.n_cb_verdicts, C.n_cb_narrowed, C.n_cb_bands_kept,
              C.n_cb_all_no_exoneration + C.n_cb_all_inconsistent
                  + C.n_cb_anchor_colocate,
              C.n_cb_all_no_exoneration, C.n_cb_all_inconsistent,
              C.n_cb_anchor_colocate,
              C.n_cb_iftd_veto,
              C.n_cb_iftd_ref_arc,
              ref_arc_by.empty() ? std::string{} : fmt::format(" [{}]", ref_arc_by),
              C.n_cb_immature_pairs, C.n_cb_lli_shadow,
              C.n_cb_lli_verdicts, C.n_cb_lli_narrowed,
              C.n_cb_lli_anchor_colocate,
              C.n_cb_weak_silence,
              static_cast<double>(C.n_cb_split_sats_sum) / ep,
              C.n_cb_split_sats_max,
              static_cast<double>(C.n_cb_split_multi_sum) / ep,
              config_.networkCycleSlipCommonBandScope() ? "on" : "off",
              config_.networkCycleSlipCommonBandScope()
                  ? std::string{}
                  : fmt::format("; would-narrow {}", C.n_cb_would_narrow));
    // Attribute the vetoes to the IF-TD pair kind that made them.  The
    // expectation is that the vetoes come from pairs the single-pair detector never
    // watched, and the total alone cannot show that.
    if (!C.n_cb_iftd_veto_by_pair.empty()) {
        std::string by;
        for (const auto& [pr, n] : C.n_cb_iftd_veto_by_pair)
            by += fmt::format("{}{} {}", by.empty() ? "" : ", ", pr, n);
        log->info("Network (common-band scope): IF-TD vetoes by pair — {}.", by);
    }
    return 0;
}

// ---- End of the run-end reports ----

} // namespace GnssProcessor
