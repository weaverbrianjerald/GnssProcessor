/**
 * @file NetworkSdEwlAr.cpp
 * @brief Implementation of @ref networkSdEwlAr, the SD extra-wide-lane integer
 *        decision (increment 2a, Route B plus the A2 DD-to-SD datum propagation).
 *        See the header for the algebra, the channel-identity precondition, and the
 *        standing-invariant-#1 flip response.
 */

#include "network/ar/NetworkSdEwlAr.hpp"

#include "network/state/NetworkRunContext.hpp"
#include "common/Constants.hpp"
#include "common/GnssIdentity.hpp"   // systemOfSat, letterOfSat, ObservableId, carrier frequencies
#include "config/GnssConfig.hpp"
#include "network/driver/NetworkRunTypes.hpp"
#include "network/solve/NetworkSbasis.hpp"
#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "network/diag/NetworkRunTraces.hpp"
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
#include "network/driver/NetworkSetupResult.hpp"
#include "network/runend/NetworkRunEnd.hpp"
#include <memory>
#include "network/driver/NetworkEpoch.hpp"   // NetworkEpoch (card NOM3 step EP1)
#include "network/driver/NetworkRun.hpp"     // NetworkRun (card NOM3 step EP1)
#include "rinex/MultiRinexSync.hpp"
#include "network/assembly/NetworkStationBuild.hpp"   // StationBuild, StationCarryFrame (card NOM3 step PB1b)
#include "network/solve/NetworkEstimator.hpp"
#include "network/assembly/NetworkContributions.hpp"
#include "network/carry/NetworkCoupling.hpp"
#include "network/assembly/NetworkParamInventory.hpp"
#include <Eigen/Dense>
#include <limits>

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
    net::StationTable& fix = ctx.entities.stations;   // each station's sd_ewl_fix (card NOM step N4a)
    // Cleared first and unconditionally: with the knob off, PASS B must see an empty map,
    // and a stale publication from a previous epoch would act as an unannounced hold.
    net::dropAll(fix, &net::StationState::sd_ewl_fix);
    if (!cfg.enable || !cfg.fix_enable) return;

    auto& stations = setup.stations;
    auto& C = ctx.counters;
    net::LosTable& los_rows = ctx.entities.los;   // the flip memory (card NOM step N4c)
    net::ConstellationTable& cons_rows = ctx.entities.constellations;   // sd_ewl_r0 (N4b)
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
            const std::string* sticky =
                net::get(cons_rows, systemFromLetter(cons), &net::ConstellationState::sd_ewl_r0);
            if (sticky != nullptr && graph.count(*sticky)) {
                r0 = *sticky;
            } else {
                std::size_t best_n = 0;
                for (const auto& [nm, ss] : graph)
                    if (ss.size() > best_n) { r0 = nm; best_n = ss.size(); }
                if (sticky != nullptr) ++C.sdewl_sd_ref_reelected;
                net::put(cons_rows, systemFromLetter(cons), &net::ConstellationState::sd_ewl_r0) = r0;
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
                // The flip memory of the line of sight (the old key's constellation is the
                // satellite's).
                const std::pair<long long, std::size_t>* pit =
                    net::getLos(los_rows, static_cast<int>(sta), sat, &net::LosState::sd_ewl_prev);
                const long long n_prev = pit != nullptr ? pit->first : 0;
                const SdEwlFlipVerdict v =
                    sdEwlFlipCheck(pit != nullptr, n_prev,
                                   pit != nullptr ? pit->second : 0,
                                   d.n_cyc, stat.count);
                if (v == SdEwlFlipVerdict::Restart) ++C.sdewl_dd_restart;
                if (v == SdEwlFlipVerdict::Flip)    ++C.sdewl_dd_flip;
                const bool vetoed = (v == SdEwlFlipVerdict::Flip);
                net::putLos(ctx.entities, static_cast<int>(sta), sat, &net::LosState::sd_ewl_prev) =
                    { d.n_cyc, stat.count };
                SdEwlFix f;
                f.n_cyc = d.n_cyc;
                f.sig1 = pk.first;
                f.sig2 = pk.second;
                f.frac_cyc = d.frac_cyc;
                f.ffr = d.ffr;
                f.measured = d.measured;
                f.usable = !vetoed;
                net::put(fix, StationId{static_cast<int>(sta)},
                         &net::StationState::sd_ewl_fix)[cons][sat] = f;
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
    net::eraseIf(los_rows, &net::LosState::sd_ewl_prev,
                 [&](const LosId& id, const std::pair<long long, std::size_t>&) {
                     const auto sta = static_cast<std::size_t>(id.station.index);
                     const std::string sat = net::satKey(id.sat);
                     const auto cit = chan.find(id.sat.letter());
                     const bool alive = cit != chan.end() && cit->second.count(sat)
                                        && cit->second.at(sat).second.count(sta);
                     return !alive;
                 });

    if (traces.sd_ewl_ar_trace) traces.sd_ewl_ar_trace.flush();
    log->debug("Network (SD EWL 2a) TOW {:.1f}: {} arc(s) admitted, {} DD attempt(s), "
               "{} fixed, {} rej-frac, {} rej-FFR, {} flip-vetoed, {} restart, "
               "{} component(s), r0 {}",
               se.time.tow, C.sdewl_dd_pairs_seen, C.sdewl_dd_fix_attempts,
               C.sdewl_dd_fixed, C.sdewl_dd_reject_frac, C.sdewl_dd_reject_ffr,
               C.sdewl_dd_flip, C.sdewl_dd_restart, C.sdewl_components,
               [&] { std::string s;
                     net::forEach(cons_rows, &net::ConstellationState::sd_ewl_r0,
                                  [&](System sys, const std::string& nm) {
                                      s += fmt::format("{}={} ", rinexLetter(sys), nm);
                                  });
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

// ---- PASS B station steps (card NOM3 step PB1b; generated by scripts/nom_pb1b_cut.py) ----

StationStep networkSdEwlStationRows(StationBuild& build) {
    // ---- From the station build (card NOM3 step PB1b; generated by scripts/nom_pb1b_cut.py,
    //      each line checked by scripts/nom_pb1b_verify.py). ----
    const NetworkRunContext& ctx = build.ctx;
    const GnssConfig& config_ = build.config;
    const NetworkEstimator& est = build.est;
    const SyncedEpoch& se = build.se;
    const std::vector<std::vector<PppObservation>>& modeled = build.pass_a.modeled;
    const int i = build.i;
    NetStation& st = build.st;
    StationAssembly& R = build.R;
    std::map<int, int>& h2local = R.h2local;
    // ---- End of the bindings ----

    // --- SD pre-reduce EWL constraint (docs/SPEC_sd_ewl_prereduce.md) --------
    // Appended after the iono rows and before the block reaches
    // addStationObservations, so it folds into N_rr/N_sr/N_ss pre-reduce and
    // composes exactly with the post-reduce Card-4 DD fold.  Off by default;
    // the call then returns immediately (byte-identical).
    if (!(config_.getNetworkConfig().sd_ewl.enable)) return StationStep::Next;
    const long long sd_rows0 = st.sd_ewl_ctr.rows_built;
    networkAppendSdEwlRows(config_, st, est, modeled[i], h2local, se.time,
                            R.A_s, R.A_r, R.w, R.y, R.row_tags, R.sd_ewl_trace,
                            &ctx.entities.stations, i);
    // The builder's own count this station-epoch, for the tag check (card NOM N2a).
    R.sd_ewl_rows = st.sd_ewl_ctr.rows_built - sd_rows0;
    return StationStep::Next;
}

// SD pre-reduce EWL constraint (docs/SPEC_sd_ewl_prereduce.md): the between-satellite
// single-differenced extra-wide-lane MW statistic as a pseudo-observation row in this
// station's own pre-reduce block.  The declaration in app/Application.hpp documents the
// algebra and the thread-safety contract.  Off by default: the function then returns
// immediately and the output is byte-identical.
void networkAppendSdEwlRows(const GnssConfig& config_, NetStation& st, const NetworkEstimator& est,
                            const std::vector<PppObservation>& sta_obs,
                            const std::map<int, int>& h2local, const GnssTime& t,
                            Eigen::MatrixXd& A_s, Eigen::MatrixXd& A_r, Eigen::VectorXd& w,
                            Eigen::VectorXd& y, std::vector<RowTag>& tags,
                            std::vector<std::string>& trace_rows,
                            const net::StationTable* fix,
                            std::size_t station_index) {
    const auto& cfg = config_.getNetworkConfig().sd_ewl;
    if (!cfg.enable) return;
    auto& ctr = st.sd_ewl_ctr;
    // The three trailing trace columns exist only when the integer path is armed
    // (increment 2b), so an increment-1 artifact set stays byte-identical (the header
    // in NetworkRunTraces::openAll follows the same rule).
    const bool fix_on = cfg.fix_enable && fix != nullptr;
    // Per-satellite trace sink (§6): rows are formatted here and drained by PASS B's
    // serial consume, because the produce phase must not touch a shared stream.
    const auto tr2 = [&](const char* status, const std::string& sat,
                         const std::string& pivot, const std::string& s1,
                         const std::string& s2, const std::string& c1,
                         const std::string& c2, std::size_t n_cand,
                         std::size_t n_arc, bool bia, double lam, double mean_m,
                         double pred_m, double resid_m, double sigma_m,
                         long long n_fix, bool row_fixed, bool grp_fixed) {
        if (!cfg.trace) return;
        std::string row = fmt::format(
            "{},{:.3f},{},{},{},{},{},{},{},{},{},{},{},{},{:.4f},{:.4f},{:.4f},"
            "{:.4f},{:.4f}",
            t.week, t.tow, st.name, letterOfSat(sat), sat, pivot,
            status, s1, s2, c1, c2, n_cand, n_arc, bia ? 1 : 0, lam, mean_m, pred_m,
            resid_m, sigma_m);
        if (cfg.fix_enable)
            row += fmt::format(",{},{},{}", n_fix, row_fixed ? 1 : 0, grp_fixed ? 1 : 0);
        row += '\n';
        trace_rows.push_back(std::move(row));
    };
    const auto tr = [&](const char* status, const std::string& sat,
                        const std::string& pivot, const std::string& s1,
                        const std::string& s2, const std::string& c1,
                        const std::string& c2, std::size_t n_cand,
                        std::size_t n_arc, bool bia, double lam, double mean_m,
                        double pred_m, double resid_m, double sigma_m) {
        tr2(status, sat, pivot, s1, s2, c1, c2, n_cand, n_arc, bia, lam, mean_m,
            pred_m, resid_m, sigma_m, 0, false, false);
    };
    // The MW arc accumulator exists only for non-pivot stations, and only under
    // phase_leveling && (mw_detect || turbo_edit).  Without it the station-epoch is
    // counted and skipped; the count keeps an armed but inactive feature visible
    // (invariant #4).
    if (!st.mw_slip || !st.sm) {
        ++ctr.skip_no_detector;
        tr("NO_DETECTOR", {}, {}, {}, {}, {}, {}, 0, 0, false, 0.0, 0.0, 0.0, 0.0, 0.0);
        return;
    }

    // --- Extra-wide-lane band pair per constellation -----------------------------------
    // Shared with the increment-2a integer decision through @ref sdEwlBands, a single
    // definition, so the row and the integer folded into it cannot disagree about which
    // lane they are on.  GPS (L2,L5) lambda_EWL 5.861 m; Galileo (E5b,E5a) 9.768 m.
    // BeiDou (deferred) and GLONASS (excluded) produce no rows.
    auto bandsFor = [](char cons, char& b1, char& b2, double& f1, double& f2) -> bool {
        return sdEwlBands(cons, b1, b2, f1, f2);
    };

    // One resolved arc: the ZD-consistent EWL statistic plus the six columns the row
    // needs (two local ambiguities, and the shared PB/CB pair per band).
    struct Arc {
        std::string sat;
        double      mean_m{ 0.0 };
        double      var_m2{ 0.0 };
        std::size_t count{ 0 };
        bool        bia{ false };
        double      lam_ewl{ 0.0 }, lam_b1{ 0.0 }, lam_b2{ 0.0 };
        double      f1{ 0.0 }, f2{ 0.0 };
        int         amb_col_b1{ -1 }, amb_col_b2{ -1 };
        Eigen::Index pb_b1{ -1 }, pb_b2{ -1 }, cb_b1{ -1 }, cb_b2{ -1 };
        /// This arc's linearized prediction of the MW observable, i.e. its half of
        /// @f$H\,x_{lin}@f$ (see the correction-frame note at the row build).  Split in
        /// two because increment 2b's fixed row drops its local ambiguity coefficients,
        /// and the linearization point must lose exactly the same terms in the same
        /// place, or the row is folded in a mixed frame.
        double      pred_amb_m{ 0.0 };   ///< the two local ambiguity terms of H·x_lin
        double      pred_bias_m{ 0.0 };  ///< the four shared PB/CB terms of H·x_lin
        double      pred_m{ 0.0 };       ///< pred_amb_m + pred_bias_m (the float row's frame)
        // Trace-only provenance (§6): the elected phase pair, the code channels the
        // MW actually consumed, and how many candidate pairs were searched.
        std::string sig1, sig2, code1, code2;
        std::size_t n_cand{ 0 };
        // The published integer for this arc, if any (increment 2b).  `n_cyc` is in the
        // r0-anchored frame, so only the between-satellite difference against the
        // group's pivot is meaningful.
        bool        has_fix{ false };
        long long   n_fix_cyc{ 0 };
        /// The integer came from a rounded double difference rather than from a frame
        /// choice.  A fixed group must contain at least one of these (see the group
        /// split below), or it asserts a hard constraint with no measurement behind it.
        bool        fix_measured{ false };
    };

    // --- Correction frame -------------------------------------------------------------
    // The station's whole block is in the correction frame: networkSplitStation_ fills
    // y from `em.innovations` = z − h(x_lin), with x_lin = st.sm->getStateVector().  A
    // pseudo-observation must follow the same convention, or it asserts an absolute
    // value against a design that solves for a correction.  The error would be large
    // here: the raw MW mean carries λ_EWL·N_EWL with N_EWL in the thousands of cycles
    // (|z_SD| of hundreds to thousands of metres against a 0.13 m row σ).
    //
    // Which columns need the x_lin term: the local ambiguity states are seeded from
    // code-minus-phase and reseeded on slips, so their term is large; it is the term
    // a correction-frame row subtracts as `z_lin` (z_corr = z_mw − z_lin).  The shared
    // satellite phase/code-bias states are allocated at value 0 by MeasurementModel, as
    // shared states the network solves, and never written back from the solve, so their
    // term is currently zero.  It is included rather than assumed, so that a future
    // seeding change cannot skew the frame unnoticed.
    const Eigen::VectorXd x_lin = st.sm->getStateVector();
    const auto xAt = [&x_lin](int idx) -> double {
        return (idx >= 0 && idx < static_cast<int>(x_lin.size())) ? x_lin(idx) : 0.0;
    };
    // Shared PB/CB state indices at this station (distinct from the estimator's border
    // column indices).  A column may be registered by another station; this station then
    // holds no state and its x_lin contribution is 0, which matches its real rows (em.H
    // has no such column, so the innovations omit it).
    std::map<std::pair<std::string, std::string>, int> pb_state, cb_state;
    for (const auto& kv : st.sm->getSatPhaseBiasStates()) {
        const auto p = StateKeys::parseSatPhaseBiasKey(kv.first);
        if (p) pb_state[{ p->satellite, p->signal }] = static_cast<int>(kv.second);
    }
    for (const auto& kv : st.sm->getSatBiasStates()) {
        const auto p = StateKeys::parseSatBiasKey(kv.first);
        if (p) cb_state[{ p->satellite, p->signal }] = static_cast<int>(kv.second);
    }
    const auto stateOf = [](const std::map<std::pair<std::string, std::string>, int>& m,
                            const std::string& sat, const std::string& sig) -> int {
        const auto it = m.find({ sat, sig });
        return it == m.end() ? -1 : it->second;
    };

    // --- This station's per-(satellite, band) ambiguity candidates ---------------------
    // Built once from StateManager's own key map (the authority) rather than by
    // reconstructing a key string per candidate: the ambiguity key format belongs to the
    // state layer, and a reconstructed key that does not round-trip fails without any
    // error (every arc looks absent).  A candidate also needs a local column this epoch:
    // h2local presence means "this arc has a phase row in A_r now".  Sorted ascending
    // by signal name so the pair search below follows a strict total order
    // (byte-identity: an insertion-order-dependent pick changes the row set from run to
    // run).  Entries are (signal, A_r local column, global state index); the state
    // index serves the correction-frame x_lin term, the local column the design.
    using Cand = std::tuple<std::string, int, int>;
    std::map<std::pair<std::string, char>, std::vector<Cand>> cand;
    for (const auto& [key, sidx] : st.sm->getAmbiguities()) {
        const auto parts = StateKeys::parseAmbiguityKey(key);
        if (!parts) continue;
        const std::string& sig = parts->signal;
        const auto phase = ObservableId::parse(systemOfSat(parts->satellite), sig);
        if (!phase || phase->type != ObsType::Phase) continue;
        const auto hit = h2local.find(static_cast<int>(sidx));
        if (hit == h2local.end()) continue;         // no phase row on this arc this epoch
        cand[{ parts->satellite, phase->signal.digit }].emplace_back(sig, hit->second,
                                                                     static_cast<int>(sidx));
    }
    for (auto& kv : cand)
        std::sort(kv.second.begin(), kv.second.end());   // ascending signal name first

    // --- Resolve every eligible arc, grouped by constellation (sorted: deterministic) --
    // The MW arc key carries the two phase signal names, and the channel the detector
    // holds an arc on is the detector's choice (collectUncombinedPairs' KF-chosen /
    // attribute-priority pick), not this function's.  The candidate pairs are therefore
    // searched in ascending order and the first with a usable arc is taken, instead of
    // electing one channel and declaring the arc missing when the detector picked the
    // other.  A one-channel band (the common case) has one candidate, so the search
    // reduces to a direct lookup.
    std::map<char, std::vector<Arc>> by_const;
    for (const auto& o : sta_obs) {
        if (o.satellite.empty()) continue;
        const char cons = letterOfSat(o.satellite);
        char b1 = 0, b2 = 0;
        double fb1 = 0.0, fb2 = 0.0;
        if (!bandsFor(cons, b1, b2, fb1, fb2)) continue;
        ++ctr.sat_seen;

        const auto c1it = cand.find({ o.satellite, b1 });
        const auto c2it = cand.find({ o.satellite, b2 });
        if (c1it == cand.end() || c2it == cand.end()) {
            ++ctr.skip_missing_amb_col;
            tr("NO_AMB_COL", o.satellite, {}, {}, {}, {}, {}, 0, 0, false,
               0.0, 0.0, 0.0, 0.0, 0.0);
            continue;
        }
        // The first candidate pair with a mature arc wins.  The best near-miss is
        // remembered so that a skip is attributed to a cause (never formed / young /
        // unreconstructable) rather than to one combined tally.
        MwSlipDetector::MwArcZdMean zd;                 // the winner
        std::string sig1, sig2;
        int col1 = -1, col2 = -1, sidx1 = -1, sidx2 = -1;
        std::size_t n_cand = 0, best_n = 0;
        bool any_exists = false, any_recon_fail = false;
        std::string near_s1, near_s2;
        // The published integer, if any, for this (station, satellite) (increment 2b).
        // It is looked up but does not influence the channel search below.  Trying the
        // decision's pair first would let the integer path change which pair the float
        // path elects, and so change the solve even when no integer is folded (an
        // `inert` A/B arm, fix armed with the FFR gate unreachable, would then differ
        // from the float arm).  The row elects its pair exactly as increment 1 does, and
        // the fix is used only if it was decided on that pair; otherwise
        // `fix_pair_mismatch` counts it (measured at 0, because 2a searches the same
        // candidates in the same ascending order).
        const SdEwlFix* my_fix = nullptr;
        if (fix_on) {
            const auto* sta_fix = net::get(*fix, StationId{static_cast<int>(station_index)},
                                           &net::StationState::sd_ewl_fix);
            const auto git = sta_fix ? sta_fix->find(cons) : net::SdEwlFixes::const_iterator{};
            if (sta_fix && git != sta_fix->end()) {
                const auto sit = git->second.find(o.satellite);
                if (sit != git->second.end() && sit->second.usable)
                    my_fix = &sit->second;
            }
        }
        for (const auto& [s1, k1, i1] : c1it->second) {
            for (const auto& [s2, k2, i2] : c2it->second) {
                ++n_cand;
                if (!sig1.empty()) continue;            // already decided; just count
                const auto q = st.mw_slip->arcZdMean(o.satellite, s1, s2);
                if (!q.exists) continue;                // this pair was never formed
                any_exists = true;
                // arcZdMean reports !valid for two different reasons: fewer than two
                // lagged samples, and an SD arc whose group MW pivot arc is missing.
                // Only the latter is a frame-reconstruction failure; conflating them
                // would report a warm-up epoch as a datum defect.
                if (!q.valid && q.count >= 2) any_recon_fail = true;
                if (q.count > best_n) { best_n = q.count; near_s1 = s1; near_s2 = s2; }
                if (q.valid && q.count >= static_cast<std::size_t>(cfg.min_arc_epochs)) {
                    zd = q; sig1 = s1; sig2 = s2;
                    col1 = k1; col2 = k2; sidx1 = i1; sidx2 = i2;
                }
            }
        }
        if (sig1.empty()) {
            // Ordered from most to least specific: a pair that exists but cannot be
            // framed is a different defect from one that is merely young, and both
            // differ from a pair the detector never built.
            const char* why = !any_exists      ? "NO_ARC"
                            : any_recon_fail   ? "FRAME_RECON"
                                               : "IMMATURE";
            if (!any_exists)          ++ctr.skip_no_arc;
            else if (any_recon_fail)  ++ctr.skip_frame_recon;
            else                      ++ctr.skip_immature;
            tr(why, o.satellite, {}, near_s1, near_s2, {}, {}, n_cand, best_n,
               false, 0.0, 0.0, 0.0, 0.0, 0.0);
            continue;
        }
        const double f1 = fb1, f2 = fb2;
        // λ cross-check: the detector's own λ_WL comes from the actual signal
        // frequencies, so a disagreement with the tier's nominal c/(f1−f2) means the
        // elected pair is not the tier this row's coefficients assume (a mis-tiered
        // row would fold a wrong-wavelength constraint without any error).
        const double lam_nom = Constants::SPEED_OF_LIGHT / (f1 - f2);
        if (!(f1 > 0.0) || !(f2 > 0.0) || std::abs(f1 - f2) < 1.0
            || !(zd.lambda_wl > 0.0) || !std::isfinite(zd.lambda_wl)
            || std::abs(zd.lambda_wl - lam_nom) > 1e-6 * std::abs(lam_nom)) {
            ++ctr.skip_bad_lambda;
            tr("BAD_LAMBDA", o.satellite, {}, sig1, sig2, zd.code1, zd.code2, n_cand,
               zd.count, zd.bia_corrected, zd.lambda_wl, zd.mean_m, 0.0, 0.0, 0.0);
            continue;
        }
        // The satellite code-bias columns must be the ones the MW observable actually
        // carries.  The arc reports its own code channels (matchChannel's same-attribute
        // pick, with an in-band fallback), so nothing is guessed: without them the OSB
        // attribution would be an assumption, and a wrong OSB column is undetectable
        // downstream.
        if (zd.code1.empty() || zd.code2.empty()) {
            ++ctr.skip_missing_code;
            tr("NO_CODE", o.satellite, {}, sig1, sig2, zd.code1, zd.code2, n_cand,
               zd.count, zd.bia_corrected, zd.lambda_wl, zd.mean_m, 0.0, 0.0, 0.0);
            continue;
        }
        Arc a;
        a.sat = o.satellite;
        a.mean_m = zd.mean_m;
        // Attach the published integer only if the search settled on the pair the
        // integer was decided on (increment 2b).  A mismatch is not an error (the
        // decision runs before assembly and does not know which pairs will have local
        // columns this epoch), but folding across it would put one bias frame's integer
        // into another frame's row, so it is counted and the arc stays float.
        if (my_fix) {
            if (sig1 == my_fix->sig1 && sig2 == my_fix->sig2) {
                a.has_fix = true;
                a.n_fix_cyc = my_fix->n_cyc;
                a.fix_measured = my_fix->measured;
            } else {
                ++ctr.fix_pair_mismatch;
            }
        }
        // arcZdMean returns a variance of the mean (~1/n); it is floored so a long quiet
        // arc cannot dominate the shared border with an unphysically tight row.  A fixed
        // row takes the per-sample floor instead (amendment A1): the accumulator is
        // arc-cumulative, so consecutive-epoch rows share n−1 of n samples, and refolding
        // a variance of the mean as white every epoch over-claims information as sqrt(k),
        // increasingly so as the fixed-row residuals fall to code-noise scale.  Folding
        // the current mean at per-sample weight adds about one sample of information per
        // epoch, which asymptotically matches the information the data carry.
        a.var_m2 = a.has_fix
                       ? std::max(zd.var_of_mean_m2,
                                  cfg.fix_sigma_floor_m * cfg.fix_sigma_floor_m)
                       : std::max(zd.var_of_mean_m2,
                                  cfg.sigma_floor_m * cfg.sigma_floor_m);
        a.count = zd.count;
        a.bia = zd.bia_corrected;
        a.lam_ewl = zd.lambda_wl;
        a.lam_b1 = Constants::SPEED_OF_LIGHT / f1;
        a.lam_b2 = Constants::SPEED_OF_LIGHT / f2;
        a.f1 = f1;
        a.f2 = f2;
        a.amb_col_b1 = col1;
        a.amb_col_b2 = col2;
        // Non-throwing shared lookups: phaseBiasColumn / codeBiasColumn use `index_.at()`
        // and throw when the key is unregistered.  A bare call here would escape into the
        // parallel produce loop's catch and drop the whole station's block (including its
        // valid phase rows) because of one absent bias column.
        const auto pbCol = [&est, &o](const std::string& sig) -> Eigen::Index {
            try { return est.phaseBiasColumn(o.satellite, sig); }
            catch (...) { return -1; }
        };
        const auto cbCol = [&est, &o](const std::string& code) -> Eigen::Index {
            try { return est.codeBiasColumn(o.satellite, code); }
            catch (...) { return -1; }
        };
        a.pb_b1 = pbCol(sig1);
        a.pb_b2 = pbCol(sig2);
        a.cb_b1 = cbCol(zd.code1);
        a.cb_b2 = cbCol(zd.code2);
        if (a.pb_b1 < 0 || a.pb_b2 < 0 || a.cb_b1 < 0 || a.cb_b2 < 0) {
            ++ctr.skip_missing_shared_col;
            tr("NO_SHARED_COL", o.satellite, {}, sig1, sig2, zd.code1, zd.code2,
               n_cand, zd.count, a.bia, a.lam_ewl, a.mean_m, 0.0, 0.0, 0.0);
            continue;
        }
        // This arc's half of H·x_lin, with exactly the coefficients the row folds in
        // emitGroup below (the two must stay in step, or design and frame drift apart).
        // Split into its ambiguity and bias halves so the fixed row, whose design drops
        // the ambiguity columns, can drop the matching linearization terms at one site.
        a.pred_amb_m = (a.lam_ewl / a.lam_b1) * xAt(sidx1)
                     - (a.lam_ewl / a.lam_b2) * xAt(sidx2);
        a.pred_bias_m = (f1 / (f1 - f2)) * xAt(stateOf(pb_state, o.satellite, sig1))
                      - (f2 / (f1 - f2)) * xAt(stateOf(pb_state, o.satellite, sig2))
                      - (f1 / (f1 + f2)) * xAt(stateOf(cb_state, o.satellite, zd.code1))
                      - (f2 / (f1 + f2)) * xAt(stateOf(cb_state, o.satellite, zd.code2));
        a.pred_m = a.pred_amb_m + a.pred_bias_m;
        a.sig1 = sig1;
        a.sig2 = sig2;
        a.code1 = zd.code1;
        a.code2 = zd.code2;
        a.n_cand = n_cand;
        by_const[cons].push_back(std::move(a));
    }

    // --- Build the rows ---------------------------------------------------------------
    // Increment 1 forms one whitened group per constellation.  Increment 2b forms two,
    // the arcs whose integer is fixed and the rest, because a group cannot be mixed: R
    // is whitened by a dense L^-1, which would re-populate a fixed row's zeroed local
    // ambiguity columns from its float siblings, and the pivot arc's ambiguity columns
    // appear in every row of a group, so one mixed group would both remove and
    // estimate the pivot ambiguity.  Splitting keeps both halves exact and makes "a
    // fixed row carries no local coefficients" true of the whitened block, not only of
    // H.  With the integer path off, one group holds every arc, so the float behaviour
    // is unchanged.
    const auto emitGroup = [&](const std::vector<const Arc*>& g, bool fixed_group) {
        if (g.size() < 2) {
            ++ctr.skip_no_pivot;
            for (const Arc* a : g)
                tr2("NO_PIVOT", a->sat, {}, a->sig1, a->sig2, a->code1, a->code2,
                    a->n_cand, a->count, a->bia, a->lam_ewl, a->mean_m, a->pred_m,
                    0.0, 0.0, a->n_fix_cyc, false, fixed_group);
            return;
        }
        const Arc& p = *g.front();
        tr2("PIVOT", p.sat, p.sat, p.sig1, p.sig2, p.code1, p.code2, p.n_cand, p.count,
            p.bia, p.lam_ewl, p.mean_m, p.pred_m, 0.0, std::sqrt(p.var_m2),
            p.n_fix_cyc, false, fixed_group);

        struct Row { const Arc* s; double z; double var; };
        std::vector<Row> rows;
        for (std::size_t k = 1; k < g.size(); ++k) {
            const Arc& s = *g[k];
            // Satellite-bias cancellation needs one frame across the difference
            // (counted as skip_mixed_frame).
            if (s.bia != p.bia) {
                ++ctr.skip_mixed_frame;
                tr2("MIXED_FRAME", s.sat, p.sat, s.sig1, s.sig2, s.code1, s.code2,
                    s.n_cand, s.count, s.bia, s.lam_ewl, s.mean_m, s.pred_m, 0.0, 0.0,
                    s.n_fix_cyc, false, fixed_group);
                continue;
            }
            // Correction frame: y = z_SD − H·x_lin, with H·x_lin = pred(s) − pred(p)
            // (the row's H is exactly the s-terms minus the p-terms).  The raw SD
            // difference is hundreds of metres; only this residual is consistent with
            // the station's block.
            //
            // Fixed row (increment 2b): the integer term is subtracted from the RHS and
            // the ambiguity columns leave H, so H·x_lin must lose exactly its ambiguity
            // half; a row whose design changed while its linearization point did not is
            // folded in a mixed frame.  Both edits are in this one expression for that
            // reason.
            const double z =
                fixed_group
                    ? (s.mean_m - p.mean_m)
                          - p.lam_ewl * static_cast<double>(s.n_fix_cyc - p.n_fix_cyc)
                          - (s.pred_bias_m - p.pred_bias_m)
                    : (s.mean_m - p.mean_m) - (s.pred_m - p.pred_m);
            // The limit gates the correction-frame residual, which is the quantity a
            // cold-clock x_lin error inflates (a 256 km x_lin makes pred absurd while
            // the raw statistic looks ordinary).
            if (cfg.max_abs_z_m > 0.0 && std::abs(z) > cfg.max_abs_z_m) {
                ++ctr.skip_big_z;
                tr2("BIG_Z", s.sat, p.sat, s.sig1, s.sig2, s.code1, s.code2, s.n_cand,
                    s.count, s.bia, s.lam_ewl, s.mean_m, s.pred_m, z, 0.0,
                    s.n_fix_cyc, false, fixed_group);
                continue;
            }
            if (!std::isfinite(z)) {   // a non-finite RHS must not reach u_r
                ++ctr.skip_bad_lambda;
                tr2("BAD_LAMBDA", s.sat, p.sat, s.sig1, s.sig2, s.code1, s.code2,
                    s.n_cand, s.count, s.bia, s.lam_ewl, s.mean_m, s.pred_m, z, 0.0,
                    s.n_fix_cyc, false, fixed_group);
                continue;
            }
            rows.push_back(Row{ &s, z, s.var_m2 });
        }
        if (rows.empty()) return;
        if (cfg.max_rows_per_station > 0
            && rows.size() > static_cast<std::size_t>(cfg.max_rows_per_station)) {
            std::stable_sort(rows.begin(), rows.end(),
                             [](const Row& a, const Row& b) { return a.var < b.var; });
            const std::size_t keep = static_cast<std::size_t>(cfg.max_rows_per_station);
            ctr.rows_capped += static_cast<long long>(rows.size() - keep);
            for (std::size_t k = keep; k < rows.size(); ++k) {
                const Arc& s = *rows[k].s;
                tr2("CAPPED", s.sat, p.sat, s.sig1, s.sig2, s.code1, s.code2, s.n_cand,
                    s.count, s.bia, s.lam_ewl, s.mean_m, s.pred_m, rows[k].z,
                    std::sqrt(rows[k].var), s.n_fix_cyc, false, fixed_group);
            }
            rows.resize(keep);
        }

        const int mc = static_cast<int>(rows.size());
        const Eigen::Index m0 = A_s.rows();
        const Eigen::Index ncol_s = A_s.cols();
        const Eigen::Index ncol_r = A_r.cols();
        A_s.conservativeResize(m0 + mc, Eigen::NoChange);
        A_s.bottomRows(mc).setZero();
        A_r.conservativeResize(m0 + mc, Eigen::NoChange);
        A_r.bottomRows(mc).setZero();
        w.conservativeResize(m0 + mc);
        y.conservativeResize(m0 + mc);

        // Every row shares the pivot arc's noise => R = diag(var_s) + var_p * 11^T,
        // structurally the GIM common-mode R.
        Eigen::MatrixXd R = Eigen::MatrixXd::Constant(mc, mc, p.var_m2);
        Eigen::MatrixXd Hr = Eigen::MatrixXd::Zero(mc, ncol_r);
        Eigen::MatrixXd Hs = Eigen::MatrixXd::Zero(mc, ncol_s);
        Eigen::VectorXd zc(mc);
        for (int k = 0; k < mc; ++k) {
            const Arc& s = *rows[static_cast<std::size_t>(k)].s;
            R(k, k) += rows[static_cast<std::size_t>(k)].var;
            zc(k) = rows[static_cast<std::size_t>(k)].z;
            ctr.sum_var_m2 += R(k, k);
            // Local: lambda_EWL * (amb_b1/lambda_b1 - amb_b2/lambda_b2), s minus p.
            // A fixed row has none of these: the local degree of freedom is removed
            // (rank reduction), not constrained with a tight weight.  A tight prior on
            // a gauge-adjacent local column is a stiff prior, and increment 2 exists to
            // stop the local nuisance from absorbing the row's information so that the
            // shared satellite bias receives it.
            if (!fixed_group) {
                Hr(k, s.amb_col_b1) += s.lam_ewl / s.lam_b1;
                Hr(k, s.amb_col_b2) -= s.lam_ewl / s.lam_b2;
                Hr(k, p.amb_col_b1) -= p.lam_ewl / p.lam_b1;
                Hr(k, p.amb_col_b2) += p.lam_ewl / p.lam_b2;
            }
            // Shared: MW = phi_WL - P_NL, and the satellite code bias enters the
            // pseudorange with +, hence the negative CB coefficients.
            const double wl_s1 = s.f1 / (s.f1 - s.f2), wl_s2 = -s.f2 / (s.f1 - s.f2);
            const double nl_s1 = -s.f1 / (s.f1 + s.f2), nl_s2 = -s.f2 / (s.f1 + s.f2);
            const double wl_p1 = p.f1 / (p.f1 - p.f2), wl_p2 = -p.f2 / (p.f1 - p.f2);
            const double nl_p1 = -p.f1 / (p.f1 + p.f2), nl_p2 = -p.f2 / (p.f1 + p.f2);
            Hs(k, s.pb_b1) += wl_s1;  Hs(k, s.pb_b2) += wl_s2;
            Hs(k, s.cb_b1) += nl_s1;  Hs(k, s.cb_b2) += nl_s2;
            Hs(k, p.pb_b1) -= wl_p1;  Hs(k, p.pb_b2) -= wl_p2;
            Hs(k, p.cb_b1) -= nl_p1;  Hs(k, p.cb_b2) -= nl_p2;
        }
        // Whiten by L^-1 (LL^T = R) into unit-information rows, so the block contributes
        // exactly H^T R^-1 H / H^T R^-1 z and stays in the diagonal-weight path.
        const Eigen::LLT<Eigen::MatrixXd> llt(R);
        if (llt.info() != Eigen::Success) {
            // Unwind the append.  conservativeResize leaves the new w/y entries
            // uninitialized; left in place, even against all-zero design rows, they
            // risk 0·NaN = NaN in N_rr/u_r, so the rows are removed, not just zeroed.
            // (A positive-variance R cannot fail the factorization; this is a guard.)
            A_s.conservativeResize(m0, Eigen::NoChange);
            A_r.conservativeResize(m0, Eigen::NoChange);
            w.conservativeResize(m0);
            y.conservativeResize(m0);
            ctr.skip_bad_lambda += mc;
            return;
        }
        A_r.middleRows(m0, mc) = llt.matrixL().solve(Hr);
        A_s.middleRows(m0, mc) = llt.matrixL().solve(Hs);
        y.segment(m0, mc) = llt.matrixL().solve(zc);
        w.segment(m0, mc).setOnes();
        // One tag per row (card NOM N2a), in row order: the row's satellite (the arc on
        // the diagonal of L^-1; the pivot arc is common to every row of the group).
        for (int k = 0; k < mc; ++k)
            tags.push_back(makeRowTag(ContributionKind::SdEwl,
                                      rows[static_cast<std::size_t>(k)].s->sat));
        ctr.rows_built += mc;
        if (fixed_group) { ctr.rows_fixed += mc; ++ctr.groups_fixed; }
        else             { ctr.rows_float += mc; }
        for (int k = 0; k < mc; ++k) {
            const Arc& s = *rows[static_cast<std::size_t>(k)].s;
            tr2("BUILT", s.sat, p.sat, s.sig1, s.sig2, s.code1, s.code2, s.n_cand,
                s.count, s.bia, s.lam_ewl, s.mean_m, s.pred_m,
                rows[static_cast<std::size_t>(k)].z, std::sqrt(R(k, k)),
                s.n_fix_cyc - p.n_fix_cyc, fixed_group, fixed_group);
        }
    };

    for (auto& [cons, arcs] : by_const) {
        // Pivot election: most mature arc, ties broken by ascending satellite name.  It
        // must be deterministic, or the row set (and with it byte-identity) changes from
        // run to run.  Sorted once over the whole constellation, so each partition
        // inherits the same ordering and elects its own most mature member as pivot.
        std::sort(arcs.begin(), arcs.end(), [](const Arc& a, const Arc& b) {
            if (a.count != b.count) return a.count > b.count;
            return a.sat < b.sat;
        });
        if (!fix_on) {
            std::vector<const Arc*> all;
            all.reserve(arcs.size());
            for (const Arc& a : arcs) all.push_back(&a);
            emitGroup(all, false);
            continue;
        }
        std::vector<const Arc*> fixed_g, float_g;
        for (const Arc& a : arcs) (a.has_fix ? fixed_g : float_g).push_back(&a);
        // A fixed group needs at least one measured integer.  Datum-valued integers (the
        // component reference station's own satellites, each station's link satellite, a
        // satellite framed on first sighting) carry no rounding decision, so no gate can
        // reject them.  A group built only from those would assert a hard constraint with
        // no integer information while still removing a real local degree of freedom, and
        // the feature could not be disarmed through its own gates (an `inert` A/B arm,
        // fix armed with the FFR gate unreachable, would still differ from the float
        // arm).  A group mixing datum-valued and measured integers is correct: the datum
        // value is the reference the measurement is relative to.
        if (!fixed_g.empty()
            && std::none_of(fixed_g.begin(), fixed_g.end(),
                            [](const Arc* a) { return a->fix_measured; })) {
            ctr.fix_datum_only += static_cast<long long>(fixed_g.size());
            float_g.insert(float_g.end(), fixed_g.begin(), fixed_g.end());
            std::stable_sort(float_g.begin(), float_g.end(),
                             [](const Arc* a, const Arc* b) {
                                 if (a->count != b->count) return a->count > b->count;
                                 return a->sat < b->sat;
                             });
            fixed_g.clear();
        }
        // A single fixed arc cannot form a group of its own (there is nothing to
        // difference against).  It is counted as an unusable fix (fix_lone_arc) and
        // returned to the float group, re-sorted by the constellation order above, so
        // the float pivot is the one the float-only path elects over the same arcs and
        // the partition stays a pure function of the published integers.
        if (fixed_g.size() == 1) {
            ++ctr.fix_lone_arc;
            float_g.insert(float_g.begin(), fixed_g.front());
            std::sort(float_g.begin(), float_g.end(),
                      [](const Arc* a, const Arc* b) {
                          if (a->count != b->count) return a->count > b->count;
                          return a->sat < b->sat;
                      });
            fixed_g.clear();
        }
        emitGroup(fixed_g, true);
        emitGroup(float_g, false);
    }
}

// ---- End of the PASS B station steps (card NOM3 step PB1b) ----

} // namespace GnssProcessor
