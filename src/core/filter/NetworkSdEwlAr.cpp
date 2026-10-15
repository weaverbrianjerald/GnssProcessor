/**
 * @file NetworkSdEwlAr.cpp
 * @brief Implementation of @ref networkSdEwlAr, the SD extra-wide-lane integer
 *        decision (increment 2a, Route B plus the A2 DD-to-SD datum propagation).
 *        See the header for the algebra, the channel-identity precondition, and the
 *        standing-invariant-#1 flip response.
 */

#include "filter/NetworkSdEwlAr.hpp"

#include "app/NetworkRunContext.hpp"
#include "common/Constants.hpp"
#include "common/GnssIdentity.hpp"   // systemOfSat, letterOfSat, ObservableId, carrier frequencies
#include "config/GnssConfig.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "filter/NetworkSbasis.hpp"
#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "slip/MwSlipDetector.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// The run-end reports at the end of this file (card NOM step TD1b):
#include "app/NetworkSetupResult.hpp"
#include "filter/NetworkRunEnd.hpp"
#include <memory>
#include "app/NetworkEpoch.hpp"   // NetworkEpoch (card NOM3 step EP1)
#include "app/NetworkRun.hpp"     // NetworkRun (card NOM3 step EP1)
#include "rinex/MultiRinexSync.hpp"

namespace GnssProcessor {

bool sdEwlBands(char cons, char& b1, char& b2, double& f1, double& f2) {
    const System sys = systemFromLetter(cons);
    if (sys == System::GPS) {       // GPS EWL (L2,L5): lambda_EWL = c/(f2-f5) = 5.861 m
        b1 = '2'; b2 = '5';
        f1 = nominalCarrierFrequencyHz(FrequencyBand::GpsL2);
        f2 = nominalCarrierFrequencyHz(FrequencyBand::GpsL5);
        return true;
    }
    if (sys == System::Galileo) {   // Galileo UWL (E5b,E5a): lambda = c/(f7-f5) = 9.768 m
        b1 = '7'; b2 = '5';
        f1 = nominalCarrierFrequencyHz(FrequencyBand::GalE5b);
        f2 = nominalCarrierFrequencyHz(FrequencyBand::GalE5a);
        return true;
    }
    return false;        // BeiDou deferred (clock divergence), GLONASS excluded (FDMA)
}

SdEwlFlipVerdict sdEwlFlipCheck(bool have_prev, long long n_prev,
                                std::size_t count_prev, long long n_now,
                                std::size_t count_now) {
    if (!have_prev) return SdEwlFlipVerdict::Fresh;
    if (count_now < count_prev) return SdEwlFlipVerdict::Restart;
    return n_now == n_prev ? SdEwlFlipVerdict::Stable : SdEwlFlipVerdict::Flip;
}

double sdEwlRoundingFfr(double sigma_cyc) {
    if (!(sigma_cyc > 0.0)) return 1.0;          // a noiseless scalar rounds perfectly
    return std::erf(0.5 / (sigma_cyc * std::sqrt(2.0)));
}

std::map<std::string, std::map<std::string, SdEwlDecision>> sdEwlPropagate(
    const std::map<std::string, std::map<std::string, SdEwlArcStat>>& arcs,
    const std::string& r0,
    double max_frac_cyc,
    double min_ffr,
    SdEwlPropagateTally& tally) {
    std::map<std::string, std::map<std::string, SdEwlDecision>> out;
    const auto r0it = arcs.find(r0);
    if (arcs.size() < 2 || r0it == arcs.end()) return out;

    // Adjacency: two stations can form a DD only if they share >= 2 satellites, one for
    // the quantity itself and one for the link that removes the two station terms.
    std::map<std::string, std::set<std::string>> adj;
    for (auto a = arcs.begin(); a != arcs.end(); ++a) {
        auto b = a; ++b;
        for (; b != arcs.end(); ++b) {
            std::size_t common = 0;
            for (const auto& [sat, st] : a->second) {
                (void)st;
                if (b->second.count(sat) && ++common >= 2) break;
            }
            if (common >= 2) { adj[a->first].insert(b->first);
                               adj[b->first].insert(a->first); }
        }
    }
    if (!adj.count(r0)) return out;

    // The A2 datum is N_SD(r0) == 0.  r0's own true integers are absorbed into the
    // per-satellite frame constant, which is the classical WL-FCB modulo-lambda
    // definition.
    //
    // `framed` tracks which satellites have a frame constant.  r0 seeds it, and a
    // satellite that r0 cannot see is framed at the first station that reaches it;
    // otherwise it could never receive an integer, since the propagation only relays
    // satellites the parent already holds.  The extension is valid because the frame
    // constant is per satellite: wherever it is defined, the residual offset in any SD
    // difference stays one constant per satellite pair and is identical at every station
    // reading it, which is the only property A2 needs (and what SdEwlDatumTest pins).  On
    // a 50-station global roster r0 sees roughly a third of a constellation at once, so
    // without the extension the yield would be about a third of the arcs.
    std::set<std::string> framed;
    for (const auto& [sat, st] : r0it->second) {
        (void)st;
        out[r0][sat] = SdEwlDecision{ 0, 0.0, 1.0, false };
        framed.insert(sat);
        ++tally.datum;
    }

    const auto statOf = [&](const std::string& sta,
                            const std::string& sat) -> const SdEwlArcStat* {
        const auto it = arcs.find(sta);
        if (it == arcs.end()) return nullptr;
        const auto jt = it->second.find(sat);
        return jt == it->second.end() ? nullptr : &jt->second;
    };

    // One propagation step: assign station `r` from an already-assigned station `q`.
    const auto propagate = [&](const std::string& q, const std::string& r) -> bool {
        const auto qit = out.find(q);
        if (qit == out.end()) return false;
        // Link satellite: co-visible, already assigned at q, maximizing the weaker of the
        // two arc maturities; ties go to the lower satellite name.  The link absorbs the
        // per-station free integer a_r, so it is a datum choice and is not gated: gating
        // a gauge only shrinks coverage and adds no rigour.
        std::string link;
        std::size_t link_n = 0;
        for (const auto& [sat, dec] : qit->second) {
            (void)dec;
            const SdEwlArcStat* aq = statOf(q, sat);
            const SdEwlArcStat* ar = statOf(r, sat);
            if (!aq || !ar) continue;
            const std::size_t m = std::min(aq->count, ar->count);
            if (m > link_n || (m == link_n && !link.empty() && sat < link)) {
                link = sat; link_n = m;
            }
        }
        if (link.empty()) return false;
        const SdEwlArcStat* aq_l = statOf(q, link);
        const SdEwlArcStat* ar_l = statOf(r, link);
        auto& dst = out[r];
        if (!dst.count(link)) {
            dst[link] = SdEwlDecision{ qit->second.at(link).n_cyc, 0.0, 1.0, false };
            ++tally.datum;
        }
        for (const auto& [sat, dec] : qit->second) {
            if (sat == link || dst.count(sat)) continue;
            const SdEwlArcStat* aq = statOf(q, sat);
            const SdEwlArcStat* ar = statOf(r, sat);
            if (!aq || !ar) continue;
            // Route B, in cycles:
            //   DD = [u(q,s) - u(q,link)] - [u(r,s) - u(r,link)]
            // The receiver EWL bias cancels inside each station's own SD; the satellite
            // bias pair cancels between the two stations.  Pure integer + noise.
            const double dd = (aq->u_cyc - aq_l->u_cyc) - (ar->u_cyc - ar_l->u_cyc);
            if (!std::isfinite(dd)) continue;
            ++tally.attempts;
            const double nd = std::round(dd);
            const double frac = dd - nd;
            const double sigma = std::sqrt(aq->var_cyc2 + aq_l->var_cyc2
                                           + ar->var_cyc2 + ar_l->var_cyc2);
            const double ffr = sdEwlRoundingFfr(sigma);
            if (std::abs(frac) > max_frac_cyc) { ++tally.reject_frac; continue; }
            if (ffr < min_ffr)                 { ++tally.reject_ffr;  continue; }
            // n(r,s) = n(q,s) - round(DD(q,r; s,link))
            dst[sat] = SdEwlDecision{ dec.n_cyc - static_cast<long long>(nd),
                                      std::abs(frac), ffr, true };
            framed.insert(sat);
            ++tally.fixed;
        }
        // Frame extension: any satellite this station holds that has no frame constant
        // yet gets one here, as a datum (see the `framed` note above).  Station r's own
        // constant a_r is already fixed by the link assigned above, so this is a
        // well-defined choice of b_s and not a measurement, and it is not gated.
        for (const auto& [sat, st] : arcs.at(r)) {
            (void)st;
            if (framed.count(sat) || dst.count(sat)) continue;
            dst[sat] = SdEwlDecision{ 0, 0.0, 1.0, false };
            framed.insert(sat);
            ++tally.datum;
        }
        return true;
    };

    // BFS spanning tree from r0, neighbours in ascending station name, so the traversal
    // and therefore every rounding are deterministic.
    std::set<std::string> done{ r0 };
    std::deque<std::string> queue{ r0 };
    while (!queue.empty()) {
        const std::string q = queue.front();
        queue.pop_front();
        for (const std::string& r : adj[q]) {
            if (done.count(r)) continue;
            if (!propagate(q, r)) continue;   // no admissible link: try another parent
            done.insert(r);
            queue.push_back(r);
        }
    }
    // Bounded closure sweeps: a satellite visible at r but not at r's tree parent gets
    // no integer from the tree alone, while a different neighbour may carry both it and
    // a link.  This adds coverage, not a second estimator.  At most four sweeps run,
    // stopping early once a sweep changes nothing.
    for (int sweep = 0; sweep < 4; ++sweep) {
        bool changed = false;
        for (const auto& [r, st] : arcs) {
            (void)st;
            for (const std::string& q : adj[r]) {
                if (!out.count(q)) continue;
                const std::size_t before = out[r].size();
                propagate(q, r);
                if (out[r].size() != before) changed = true;
            }
        }
        if (!changed) break;
    }
    return out;
}

namespace {

using PairKey = std::pair<std::string, std::string>;

/// One admitted arc: @ref SdEwlArcStat plus the BIA frame (a per-row property the
/// propagation never sees, because the caller keeps each graph frame-uniform).
struct Arc {
    double      u_cyc{ 0.0 };
    double      var_cyc2{ 0.0 };
    std::size_t count{ 0 };
    bool        bia{ false };
};

}  // namespace

void networkSdEwlAr(NetworkRun& run, const NetworkEpoch& ep) {
    // ---- From the run and the epoch (card NOM3 step EP1; generated by scripts/nom_ep1_cut.py,
    //      each line checked by scripts/nom_ep1_verify.py). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    NetworkRunContext& ctx = run.ctx;
    const GnssConfig& config_ = run.config;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    NetworkRunTraces& traces = run.traces;
    NetworkSetupResult& setup = run.setup;
    const SyncedEpoch& se = ep.se;
    double now_s = ep.clock.pl_now_s;
    const std::vector<std::vector<PppObservation>>& modeled = ep.pass_a.modeled;
    const std::vector<bool>& present = ep.pass_a.present;
    // ---- End of the bindings ----
    const auto& cfg = config_.getNetworkConfig().sd_ewl;
    auto& fix = ctx.carry.sd_ewl_fix;
    // Cleared first and unconditionally: with the knob off, PASS B must see an empty map,
    // and a stale publication from a previous epoch would act as an unannounced hold.
    fix.clear();
    if (!cfg.enable || !cfg.fix_enable) return;

    auto& stations = setup.stations;
    auto& C = ctx.counters;
    auto& prev = ctx.carry.sd_ewl_prev;
    auto& r0_sticky = ctx.carry.sd_ewl_r0;
    ++C.sdewl_epochs;
    (void)now_s;   // arc continuity is judged by the detector's own sample count

    const auto tr = [&](const char* status, char cons, const std::string& sat,
                        const std::string& sta, const PairKey& pk,
                        const std::string& r0, const std::string& parent,
                        const std::string& link, std::size_t count, double dd,
                        double frac, double sigma, double ffr, long long n,
                        long long n_prev) {
        if (!traces.sd_ewl_ar_trace) return;
        traces.sd_ewl_ar_trace
            << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
            << cons << ',' << sat << ',' << sta << ',' << status << ','
            << pk.first << ',' << pk.second << ',' << r0 << ',' << parent << ','
            << link << ',' << count << ','
            << fmt::format("{:.4f},{:.4f},{:.4f},{:.6f}", dd, frac, sigma, ffr) << ','
            << n << ',' << n_prev << '\n';
    };

    // ---------------------------------------------------------------------------
    // 1. Collect every mature EWL arc, keyed by the channel pair it sits on.
    //    Candidate phase signals come from StateManager's own ambiguity key map (the
    //    authority; a reconstructed key that fails to round-trip makes every arc look
    //    absent), not from h2local: this seam runs before assembly, and an arc without a
    //    local column this epoch still carries valid MW information for the DD.  The row
    //    builder applies its own column test later.
    // ---------------------------------------------------------------------------
    // cons -> sat -> channel pair -> station index -> Arc
    std::map<char, std::map<std::string, std::map<PairKey, std::map<std::size_t, Arc>>>> pool;

    for (std::size_t i = 0; i < stations.size(); ++i) {
        if (i >= present.size() || !present[i]) continue;
        NetStation& st = stations[i];
        if (!st.mw_slip || !st.sm) continue;      // no MW accumulator at this station

        // (satellite, band) -> the estimated phase signals on that band, ascending.
        std::map<std::pair<std::string, char>, std::vector<std::string>> cand;
        for (const auto& [key, sidx] : st.sm->getAmbiguities()) {
            (void)sidx;
            const auto parts = StateKeys::parseAmbiguityKey(key);
            if (!parts) continue;
            const std::string& sig = parts->signal;
            const std::optional<ObservableId> obs_id =
                ObservableId::parse(systemOfSat(parts->satellite), sig);
            if (!obs_id || obs_id->type != ObsType::Phase) continue;
            cand[{ parts->satellite, obs_id->signal.digit }].push_back(sig);
        }
        for (auto& kv : cand) std::sort(kv.second.begin(), kv.second.end());

        const std::vector<PppObservation>& obs =
            (i < modeled.size()) ? modeled[i] : std::vector<PppObservation>{};
        std::set<std::string> seen_sats;
        for (const auto& o : obs) {
            if (o.satellite.empty() || !seen_sats.insert(o.satellite).second) continue;
            const char cons = letterOfSat(o.satellite);
            char b1 = 0, b2 = 0;
            double f1 = 0.0, f2 = 0.0;
            if (!sdEwlBands(cons, b1, b2, f1, f2)) continue;
            const auto c1 = cand.find({ o.satellite, b1 });
            const auto c2 = cand.find({ o.satellite, b2 });
            if (c1 == cand.end() || c2 == cand.end()) continue;
            const double lam_nom = Constants::SPEED_OF_LIGHT / (f1 - f2);
            for (const std::string& s1 : c1->second) {
                for (const std::string& s2 : c2->second) {
                    const auto q = st.mw_slip->arcZdMean(o.satellite, s1, s2);
                    if (!q.exists || !q.valid) continue;
                    if (q.count < static_cast<std::size_t>(cfg.min_arc_epochs)) continue;
                    // Mis-tiering guard: the detector's lambda comes from the actual
                    // frequencies, so a mismatch (relative tolerance 1e-6) means this
                    // pair is not the tier whose lambda the integer is expressed in.
                    if (!(q.lambda_wl > 0.0) || !std::isfinite(q.lambda_wl)
                        || std::abs(q.lambda_wl - lam_nom) > 1e-6 * std::abs(lam_nom))
                        continue;
                    if (!std::isfinite(q.mean_m)) continue;
                    Arc a;
                    a.u_cyc = q.mean_m / q.lambda_wl;
                    // Variance of the mean, floored at fix_sigma_floor_m^2 / n: the
                    // served variance shrinks as 1/n and was measured 13-60x optimistic
                    // against the rows' own residual spread, and an optimistic sigma
                    // inflates FFR and admits wrong fixes (A1, decision half).
                    const double n = static_cast<double>(std::max<std::size_t>(q.count, 1));
                    const double floor_m2 =
                        cfg.fix_sigma_floor_m * cfg.fix_sigma_floor_m / n;
                    a.var_cyc2 = std::max(q.var_of_mean_m2, floor_m2)
                                 / (q.lambda_wl * q.lambda_wl);
                    a.count = q.count;
                    a.bia = q.bia_corrected;
                    if (!std::isfinite(a.var_cyc2)) continue;
                    pool[cons][o.satellite][PairKey{ s1, s2 }][i] = a;
                }
            }
        }
    }

    // ---------------------------------------------------------------------------
    // 2. Per (constellation, satellite): elect the network-common channel pair.
    //    B_EWL^s is a per-signal combination of the satellite phase and code biases,
    //    so it cancels in the between-station DD only if both stations' MW sits on the
    //    same two channels.  Consensus (most stations), ties broken by ascending pair
    //    name for byte-identity.  Stations without an arc on the elected pair are
    //    excluded and counted.
    // ---------------------------------------------------------------------------
    // cons -> sat -> (elected pair, station -> Arc)
    std::map<char, std::map<std::string, std::pair<PairKey, std::map<std::size_t, Arc>>>> chan;
    for (const auto& [cons, sats] : pool) {
        for (const auto& [sat, pairs] : sats) {
            const PairKey* best = nullptr;
            std::size_t best_n = 0;
            for (const auto& [pk, bysta] : pairs) {
                if (bysta.size() > best_n || (bysta.size() == best_n && best
                                              && pk < *best)) {
                    best = &pk; best_n = bysta.size();
                }
            }
            if (!best) continue;
            const PairKey pk = *best;
            // A station excluded by the consensus is one with no arc on the elected pair.
            // Counting every non-elected (pair, station) entry instead would also count
            // stations that hold arcs on several pairs and were admitted on the elected
            // one; 41 % of resolved arcs have two candidates, so that error is large.
            // A station is counted once per non-elected pair it holds.
            std::size_t excluded = 0;
            for (const auto& [other, bysta] : pairs) {
                if (other == pk) continue;
                for (const auto& [sta, a] : bysta) {
                    (void)a;
                    if (!pairs.at(pk).count(sta)) ++excluded;
                }
            }
            C.sdewl_dd_skip_channel += static_cast<long long>(excluded);
            // The BIA frame must be uniform across the DD's four legs: the satellite-bias
            // cancellation needs one frame (the same rule the retired MW/WL lane layer
            // applied per cycle).  Majority vote within the elected pair (a tie selects the
            // uncorrected frame); the minority is dropped and counted.  Every EWL arc
            // measured so far reports bia_corrected = false, so this guards against a
            // future mixed state and is not a routine loss.
            const auto& bysta = pairs.at(pk);
            std::size_t n_bia = 0;
            for (const auto& [sta, a] : bysta) { (void)sta; if (a.bia) ++n_bia; }
            const bool want_bia = 2 * n_bia > bysta.size();
            std::map<std::size_t, Arc> keep;
            for (const auto& [sta, a] : bysta) {
                if (a.bia == want_bia) keep[sta] = a;
                else ++C.sdewl_dd_skip_frame;
            }
            if (keep.empty()) continue;
            C.sdewl_dd_pairs_seen += static_cast<long long>(keep.size());
            chan[cons][sat] = { pk, std::move(keep) };
        }
    }

    // ---------------------------------------------------------------------------
    // 3. Per constellation: elect r0 (sticky), propagate, respond to flips, publish.
    // ---------------------------------------------------------------------------
    for (auto& [cons, sats] : chan) {
        // Re-key into the name-addressed graph the pure propagation takes, and keep the
        // name -> index map for the publication (the produce loop is index-addressed).
        std::map<std::string, std::map<std::string, SdEwlArcStat>> graph;
        std::map<std::string, std::size_t> idx_of;
        for (const auto& [sat, ps] : sats)
            for (const auto& [sta, a] : ps.second) {
                const std::string& nm = stations[sta].name;
                idx_of[nm] = sta;
                graph[nm][sat] = SdEwlArcStat{ a.u_cyc, a.var_cyc2, a.count };
            }
        if (graph.size() < 2) {
            C.sdewl_dd_no_graph += static_cast<long long>(graph.size());
            continue;
        }
        // r0 is sticky: the previous epoch's choice if it is still in the graph, else the
        // station with the most admitted arcs, ties broken by ascending name.  Only r0
        // sets the SD frame (the spanning tree and each station's link satellite cancel
        // in the between-satellite difference), so keeping r0 stable keeps the folded
        // integers from re-datuming under ordinary topology churn.  A change is a counted
        // event, because it shifts every propagated integer in the component.
        std::string r0 = graph.begin()->first;
        {
            const auto it = r0_sticky.find(cons);
            if (it != r0_sticky.end() && graph.count(it->second)) {
                r0 = it->second;
            } else {
                std::size_t best_n = 0;
                for (const auto& [nm, ss] : graph)
                    if (ss.size() > best_n) { r0 = nm; best_n = ss.size(); }
                if (it != r0_sticky.end()) ++C.sdewl_sd_ref_reelected;
                r0_sticky[cons] = r0;
            }
        }
        ++C.sdewl_components;

        SdEwlPropagateTally tally;
        const auto decided = sdEwlPropagate(graph, r0, cfg.fix_max_frac_cyc,
                                            cfg.fix_min_ffr, tally);
        C.sdewl_dd_fix_attempts += tally.attempts;
        C.sdewl_dd_reject_frac  += tally.reject_frac;
        C.sdewl_dd_reject_ffr   += tally.reject_ffr;
        C.sdewl_dd_datum        += tally.datum;

        // -----------------------------------------------------------------------
        // 4. Flip response (standing invariant #1) and publication.
        // -----------------------------------------------------------------------
        for (const auto& [nm, ss] : graph) {
            const std::size_t sta = idx_of.at(nm);
            const auto dit = decided.find(nm);
            for (const auto& [sat, stat] : ss) {
                const PairKey& pk = sats.at(sat).first;
                const bool have = dit != decided.end() && dit->second.count(sat);
                if (!have) {
                    ++C.sdewl_dd_no_link;
                    tr("NO_LINK", cons, sat, nm, pk, r0, "", "", stat.count,
                       0.0, 0.0, 0.0, 0.0, 0, 0);
                    continue;
                }
                const SdEwlDecision& d = dit->second.at(sat);
                const auto key = std::make_tuple(sta, cons, sat);
                const auto pit = prev.find(key);
                const long long n_prev = pit != prev.end() ? pit->second.first : 0;
                const SdEwlFlipVerdict v =
                    sdEwlFlipCheck(pit != prev.end(), n_prev,
                                   pit != prev.end() ? pit->second.second : 0,
                                   d.n_cyc, stat.count);
                if (v == SdEwlFlipVerdict::Restart) ++C.sdewl_dd_restart;
                if (v == SdEwlFlipVerdict::Flip)    ++C.sdewl_dd_flip;
                const bool vetoed = (v == SdEwlFlipVerdict::Flip);
                prev[key] = { d.n_cyc, stat.count };
                SdEwlFix f;
                f.n_cyc = d.n_cyc;
                f.sig1 = pk.first;
                f.sig2 = pk.second;
                f.frac_cyc = d.frac_cyc;
                f.ffr = d.ffr;
                f.measured = d.measured;
                f.usable = !vetoed;
                fix[{ sta, cons }][sat] = f;
                if (f.usable) ++C.sdewl_dd_fixed;
                tr(vetoed ? "FLIP" : (nm == r0 ? "R0" : (d.ffr >= 1.0 ? "LINK"
                                                                      : "FIXED")),
                   cons, sat, nm, pk, r0, "", "", stat.count, 0.0, d.frac_cyc, 0.0,
                   d.ffr, d.n_cyc, n_prev);
            }
        }
    }

    // Drop the flip memory of arcs that vanished, so a satellite that disappears for an
    // hour and returns is not compared against a stale integer (that comparison would
    // charge a spurious flip and veto a valid fix on the return epoch).
    for (auto it = prev.begin(); it != prev.end();) {
        const auto& [sta, cons, sat] = it->first;
        const auto cit = chan.find(cons);
        const bool alive = cit != chan.end() && cit->second.count(sat)
                           && cit->second.at(sat).second.count(sta);
        it = alive ? std::next(it) : prev.erase(it);
    }

    if (traces.sd_ewl_ar_trace) traces.sd_ewl_ar_trace.flush();
    log->debug("Network (SD EWL 2a) TOW {:.1f}: {} arc(s) admitted, {} DD attempt(s), "
               "{} fixed, {} rej-frac, {} rej-FFR, {} flip-vetoed, {} restart, "
               "{} component(s), r0 {}",
               se.time.tow, C.sdewl_dd_pairs_seen, C.sdewl_dd_fix_attempts,
               C.sdewl_dd_fixed, C.sdewl_dd_reject_frac, C.sdewl_dd_reject_ffr,
               C.sdewl_dd_flip, C.sdewl_dd_restart, C.sdewl_components,
               [&] { std::string s; for (const auto& [c, nm] : r0_sticky)
                                        s += fmt::format("{}={} ", c, nm);
                     return s; }());
}

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

// Run-end report: the SD pre-reduce EWL rows and integer fix.
int networkSdEwlRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const std::vector<NetStation>& stations = v.setup.stations;

    // --- SD pre-reduce EWL constraint run-end telemetry (SPEC_sd_ewl_prereduce.md) ---
    // rows_built == 0 on an armed run means the wiring never fired, which is not the same
    // as "the feature had no effect" (invariant #4), so the skip tallies are reported too
    // and an all-zero build gets a warning that names the dominant cause.
    if (!config_.getNetworkConfig().sd_ewl.enable) return 0;
    NetStation::SdEwlCounters t;
    for (const auto& st : stations) t += st.sd_ewl_ctr;
    const double mean_var = t.rows_built > 0
                                ? t.sum_var_m2 / static_cast<double>(t.rows_built)
                                : 0.0;
    log->info("Network (SD pre-reduce EWL): {} row(s) built over the run from {} "
              "(station,epoch,satellite) visit(s) on an EWL-tier constellation "
              "(mean SD variance {:.5f} m^2, i.e. sigma ~ {:.4f} m); {} capped.  "
              "Skips — {} no-detector station-epoch(s), {} no-pivot (<2 arcs at the "
              "station), {} NO MW ARC on any candidate signal pair, {} immature arc "
              "(< {} lagged samples), {} unreconstructable SD frame (group MW pivot "
              "arc absent), {} mixed BIA frame, {} no ambiguity-column pair, {} "
              "missing shared PB/CB column, {} arc without code channels, {} bad "
              "lambda, {} |z| over limit.  FLOAT rows only (increment 1); these fold "
              "PRE-reduce so they compose exactly with the Card-4 post-reduce DD fold.",
              t.rows_built, t.sat_seen, mean_var, std::sqrt(std::max(mean_var, 0.0)),
              t.rows_capped, t.skip_no_detector, t.skip_no_pivot, t.skip_no_arc,
              t.skip_immature, config_.getNetworkConfig().sd_ewl.min_arc_epochs,
              t.skip_frame_recon, t.skip_mixed_frame, t.skip_missing_amb_col,
              t.skip_missing_shared_col, t.skip_missing_code, t.skip_bad_lambda,
              t.skip_big_z);
    if (t.rows_built == 0) {
        // Name the dominant cause: "zero rows" next to a table of ten tallies is hard
        // to read, and the three failure classes (wiring, signal availability,
        // maturity) have entirely different fixes.
        const char* lead =
            t.sat_seen == 0                                  ? "the appender never saw a single EWL-tier satellite — the hook is not running, or no GPS/Galileo station-epoch reached PASS B"
          : t.skip_missing_amb_col >= t.sat_seen / 2         ? "the dominant cause is NO AMBIGUITY-COLUMN PAIR: the second EWL band (GPS L5 / Galileo E5a) is not being ESTIMATED at these stations — check processing.frequency_mode / signal_priorities, not the detector"
          : t.skip_no_arc >= t.sat_seen / 4                  ? "the dominant cause is NO MW ARC on the (band1,band2) pair: the detector never formed that pair — check mw_frequency_mode (Mirror reads obs.observed, so an unprocessed band yields no pair) and mw_channel_mode"
          : t.skip_immature >= t.sat_seen / 4                ? "the dominant cause is IMMATURE arcs: the run is shorter than sd_ewl.min_arc_epochs, or arcs keep refloating"
          : t.skip_frame_recon >= t.sat_seen / 4             ? "the dominant cause is an UNRECONSTRUCTABLE SD frame: the arcs are BIA-differenced but their group MW pivot arc is missing"
                                                            : "no single cause dominates — read the tallies above";
        log->warn("Network (SD pre-reduce EWL): ARMED but built ZERO rows — the "
                  "feature is INERT on this run.  {}.  (An all-zero no-detector "
                  "count with sat_seen > 0 means the hook DID run; a large "
                  "no-detector count means phase_leveling / cycle_slip.mw_detect "
                  "are off or every station is the clock datum.)", lead);
    }
    // --- Integer path, increments 2a and 2b (docs/SPEC_sd_ewl_prereduce.md §8) ------
    if (config_.getNetworkConfig().sd_ewl.fix_enable) {
        const auto& K = ctx.counters;
        log->info("Network (SD EWL integer fix, 2a decision): {} epoch(s), {} "
                  "component(s), {} arc(s) admitted to the integer graph; {} DD "
                  "rounding attempt(s) -> {} usable integer(s), of which {} are DATUM "
                  "values (reference/link/first-sighting frame choices that carry no "
                  "rounding decision and so cannot be wrong).  Rejected — {} on "
                  "|frac| > {:.3f} cyc, {} on FFR < {:.4f}.  Events — {} FLIP "
                  "(disagreed with the previous epoch on a continuous arc: counted "
                  "AND vetoed, the standing-invariant-#1 response), {} arc restart "
                  "(no flip charged), {} reference-station re-election (re-datums a "
                  "whole component's SD frame).  Excluded — {} on a non-consensus "
                  "tracking-channel pair (B^s would NOT cancel in the DD), {} on a "
                  "mixed BIA frame, {} with no admissible link satellite, {} with no "
                  "DD graph.",
                  K.sdewl_epochs, K.sdewl_components, K.sdewl_dd_pairs_seen,
                  K.sdewl_dd_fix_attempts, K.sdewl_dd_fixed, K.sdewl_dd_datum,
                  K.sdewl_dd_reject_frac,
                  config_.getNetworkConfig().sd_ewl.fix_max_frac_cyc,
                  K.sdewl_dd_reject_ffr,
                  config_.getNetworkConfig().sd_ewl.fix_min_ffr,
                  K.sdewl_dd_flip, K.sdewl_dd_restart, K.sdewl_sd_ref_reelected,
                  K.sdewl_dd_skip_channel, K.sdewl_dd_skip_frame,
                  K.sdewl_dd_no_link, K.sdewl_dd_no_graph);
        log->info("Network (SD EWL integer fix, 2b fold): {} FIXED row(s) in {} "
                  "all-fixed whitened group(s) + {} float row(s) (of {} built).  {} "
                  "arc(s) had an integer on a DIFFERENT channel pair than the row "
                  "settled on and went float; {} lone fixed arc(s) had nothing to "
                  "difference against; {} arc(s) were demoted because their group "
                  "held DATUM values only (a frame choice carries no integer "
                  "information, so it does not earn a removed local DOF).  A fixed row carries ZERO local ambiguity "
                  "coefficients — the local DOF is removed, not constrained — and is "
                  "weighted at the per-SAMPLE sigma floor {:.2f} m (amendment A1: "
                  "arcZdMean is arc-CUMULATIVE, so refolding a variance-of-mean as "
                  "white every epoch over-claims information as sqrt(k)).",
                  t.rows_fixed, t.groups_fixed, t.rows_float, t.rows_built,
                  t.fix_pair_mismatch, t.fix_lone_arc, t.fix_datum_only,
                  config_.getNetworkConfig().sd_ewl.fix_sigma_floor_m);
        if (t.rows_fixed == 0) {
            // Invariant #4 one level down: the row layer can work while the integer
            // layer stays inert, and the two have different fixes.  Name the dominant
            // cause rather than leaving nine tallies to read.
            const char* lead2 =
                K.sdewl_epochs == 0
                    ? "the 2a decision seam never ran — the call site is not reached "
                      "(every epoch skipped before pre-assembly?)"
              : K.sdewl_dd_pairs_seen == 0
                    ? "no arc reached the integer graph: the MW accumulator has no "
                      "MATURE EWL arc at any station (raise the run length or lower "
                      "sd_ewl.min_arc_epochs), or no station has both EWL bands "
                      "ESTIMATED"
              : K.sdewl_dd_no_graph >= K.sdewl_dd_pairs_seen / 2
                    ? "the DD GRAPH is empty: stations do not share two co-visible "
                      "satellites on a consensus channel pair, so no double "
                      "difference can be formed at all"
              : K.sdewl_dd_skip_channel >= K.sdewl_dd_pairs_seen / 2
                    ? "channel DIVERSITY: most arcs sit on a non-consensus tracking "
                      "pair, so they are excluded to keep the satellite-bias "
                      "cancellation exact — check processing.channel_mode and "
                      "signal_priorities"
              : K.sdewl_dd_reject_ffr >= K.sdewl_dd_fix_attempts / 2
                    ? "the FFR gate: the DDs are too noisy to round confidently "
                      "(arcs are short, or sd_ewl.fix_min_ffr is set too tight for "
                      "this maturity)"
              : K.sdewl_dd_reject_frac >= K.sdewl_dd_fix_attempts / 2
                    ? "the |frac| gate: the DDs are NOT sitting on integers, which "
                      "is itself the finding — Route B's cancellation is failing, so "
                      "suspect channel identity or the BIA frame before the gate"
              : K.sdewl_dd_flip >= K.sdewl_dd_fixed
                    ? "FLIP vetoes: integers are decided but disagree epoch to "
                      "epoch, so none is ever folded.  That is the invariant-#1 "
                      "response doing its job; the integers are not stable enough "
                      "to fold"
              : t.fix_pair_mismatch > 0
                    ? "CHANNEL MISMATCH between the decision and the row: integers "
                      "exist but the row builder settles on a different pair"
                    : "no single cause dominates — read the 2a tallies above";
            log->warn("Network (SD EWL integer fix): ARMED but folded ZERO fixed "
                      "rows.  {}.", lead2);
        }
    }
    return 0;
}

// ---- End of the run-end reports ----

} // namespace GnssProcessor
