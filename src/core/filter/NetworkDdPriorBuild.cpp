/**
 * @file NetworkDdPriorBuild.cpp
 * @brief Implementation of the network DD-prior build steps (see the header banner).
 *        Bodies relocated VERBATIM (dedent-8, zero substitution) out of
 *        Application::runNetwork at decomposition INC-9b (2026-07-22).
 *        networkBuildEnsembleGauges needs no alias preamble (params == captures);
 *        networkBuildCarryDdPriors opens with a byte-copy of the run-TU nc/nmw/trace +
 *        ctx.carry alias block so its 1,400-line body reads the original names.
 */

#include "filter/NetworkDdPriorBuild.hpp"

#include "app/NetworkRunContext.hpp"          // ctx.carry / ctx.counters / ctx.knobs
#include "config/GnssConfig.hpp"              // config_.getNetworkConfig() / .mw_wl
#include "common/PppUtils.hpp"                // mw_wl_ppp_utils->getWavelength
#include "filter/NetworkMwWlGlue.hpp"         // networkMwWlInject (WL/EWL/UWL inject seam)
#include "filter/NetworkNormalEquations.hpp"  // SumZeroGauge / DdPriorBlock / GroupInput
#include "filter/StateKeySchema.hpp"          // StateKeys::parse*
#include "logging/NetworkRunTraces.hpp"       // traces.trace
#include "slip/MwLaneTiers.hpp"               // mwLaneTierOf

#include <Eigen/Dense>

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

void networkBuildEnsembleGauges(NetworkEstimator& est,
                                const std::vector<NetStation>& stations,
                                const std::vector<int>& est_index_of,
                                const std::vector<StationAssembly>& asm_res,
                                const std::set<std::string>& shared_phase_keys,
                                const std::vector<CarryPrior>& prev_carry,
                                bool phase_datum_ensemble,
                                bool phase_hold_on) {
    if (!phase_datum_ensemble) return;
    // The (sat,signal) that actually have a shared phase-bias column this
    // epoch (a gauge is needed only for those; thin/gated signals float).
    std::set<std::string> shared_ss;
    for (const auto& pk : shared_phase_keys) {
        const auto p = StateKeys::parseSatPhaseBiasKey(pk);
        if (p) shared_ss.insert(p->satellite + "\x1f" + p->signal);
    }
    // Seed-once + time-lock: a (sat,signal) that already carries a HELD bias
    // (from the previous epoch's D2 stash) is datumed by that stiff carried
    // prior — do NOT also fold Σz=0 for it (a hard w→∞ fold would override the
    // finite hold prior and re-realize the churny level).  Only fresh /
    // re-seeded (sat,signal) get a gauge this epoch.  Empty when hold is off.
    std::set<std::string> held;
    if (phase_hold_on)
        for (const CarryPrior& cp : prev_carry)
            // var>0 mirrors the hold-prior injector's guard EXACTLY, so a
            // (sat,signal) is fold-skipped iff it will actually get a hold
            // prior — a var<=0 carry (e.g. a corrupt warm-resume snapshot) is
            // NOT held here, so it re-seeds via the fold instead of ending up
            // un-datumed.
            if (cp.kind == SharedParamKind::PhaseBias && cp.var > 0.0)
                held.insert(cp.sat + "\x1f" + cp.signal);
    std::map<std::string, NetworkNormalEquations::SumZeroGauge> by_key;
    for (size_t i = 0; i < stations.size(); ++i) {
        if (est_index_of[i] < 0) continue;
        for (const auto& [sat, sig, col] : asm_res[i].ens_amb_cols) {
            const std::string ss = sat + "\x1f" + sig;
            if (!shared_ss.count(ss)) continue;   // no shared phase bias → floats
            if (held.count(ss)) continue;         // held by the carried prior → skip the fold
            by_key[ss].terms.emplace_back(
                static_cast<std::size_t>(est_index_of[i]), col);
        }
    }
    std::vector<NetworkNormalEquations::SumZeroGauge> gauges;
    gauges.reserve(by_key.size());
    for (auto& [k, gz] : by_key) {
        if (gz.terms.empty()) continue;
        // Record the shared phase-bias column this gauge fixes, so the
        // estimator can treat it as an EXACT datum column in the datum-frame
        // conditional covariance (the fold leaves it carrying an artificial
        // gauge-direction variance — see SumZeroGauge::shared_col).  k is
        // "sat\x1fsignal"; every key in by_key came from shared_ss, so it has
        // a registered phase-bias column.
        const auto us = k.find('\x1f');
        if (us != std::string::npos) {
            const std::string sat = k.substr(0, us);
            const std::string sig = k.substr(us + 1);
            try { gz.shared_col = est.phaseBiasColumn(sat, sig); }
            catch (...) { gz.shared_col = -1; }
        }
        gauges.push_back(std::move(gz));
    }
    est.setPhaseSumZeroGauges(std::move(gauges));
}

void networkBuildCarryDdPriors(
    NetworkRunContext& ctx,
    const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkRunTraces& traces,
    NetworkInvariantCarryDriver& carry_driver,
    NetworkEstimator& est,
    const std::vector<NetStation>& stations,
    const std::vector<StationAssembly>& asm_res,
    const std::vector<int>& est_index_of,
    const std::vector<bool>& present,
    const std::set<std::string>& shared_phase_keys,
    const SyncedEpoch& se,
    std::map<int, std::map<ArcKey, std::string>>& carry_arc_amb_key,
    std::map<int, std::map<ArcKey, std::string>>& carry_arc_amb_key2,
    std::set<std::pair<int, std::string>>& carry_slipped_ambs,
    const std::set<std::string>* carry_we_drop_sats,
    const std::map<int, std::set<std::string>>* carry_we_drop_arcs,
    int n_threads,
    std::vector<EwlLane>& ewl_lanes,
    const NetworkMwWlLayer::Config& ewl_cfg,
    NetworkMwWlLayer& mw_wl_layer,
    const std::unique_ptr<PppUtils>& mw_wl_ppp_utils,
    F6Capture& mw_wl_f6,
    NetworkMwWlLayer::Counters& mw_wl_epoch,
    std::vector<WlInjectPlan>& wl_inject_epoch,
    std::vector<LaneTraceEntry>& lane_trace_epoch,
    WlCounters& wl_epoch,
    std::map<char, WlCounters>& wl_cons_epoch,
    const std::function<bool(char, const std::string&)>& isUncarriedThirdBand,
    bool carry_innov_gate,
    bool mw_wl_on,
    bool ewl_on,
    bool uwl_on,
    bool uwl_cold_on,
    bool uwl_mid_on,
    bool uwl_dual_on,
    bool uwl_family_on,
    bool uwl_sib_gate_on,
    bool wl_inject_on,
    bool bridge_debias_on,
    bool ewl_carry_exclude,
    bool lane_on,
    bool lane_meas_on,
    bool lane_bridge_on,
    bool lane_fix_on,
    bool lane_parallel_components,
    bool mw_d5_dump,
    bool mw_lane_trace,
    bool mw_tagsets,
    bool mw_wl_f6_dump) {
    // --- Alias preamble: byte-copies of the run-TU aliases the relocated
    //     buildCarryDdPriors body reads (nc/nmw/trace + the ctx.carry aliases). ----
    const auto& nmw = config_.getNetworkConfig().mw_wl;
    auto& trace = traces.trace;
    auto& carry_glonass_warned = ctx.carry.carry_glonass_warned;
    auto& carry_group_id = ctx.carry.carry_group_id;
    const bool carry_invariant = ctx.knobs.carry_invariant;
    auto& carry_sat_vertex = ctx.carry.carry_sat_vertex;
    auto& d5_ewl_stasat = ctx.carry.d5_ewl_stasat;
    auto& d5_hold_chords = ctx.carry.d5_hold_chords;
    auto& lane_admit_pairs = ctx.carry.lane_admit_pairs;
    auto& lane_census_logged = ctx.carry.lane_census_logged;
    auto& lane_family_logged = ctx.carry.lane_family_logged;
    auto& lane_pair_arcs = ctx.carry.lane_pair_arcs;
    auto& lane_pair_tree_mismatch = ctx.carry.lane_pair_tree_mismatch;
    auto& lane_resid_arcs = ctx.carry.lane_resid_arcs;
    auto& n_loop_epochs = ctx.counters.n_loop_epochs;
    auto& uwl_warned_cons = ctx.carry.uwl_warned_cons;

    if (!carry_invariant) return;
    constexpr int kSatBase = 1000000;   // satellite vertices sit above every receiver index
    std::map<int, NetworkInvariantCarryDriver::GroupInput> groups;
    if (carry_innov_gate) { carry_arc_amb_key.clear(); carry_arc_amb_key2.clear(); }  // D5: rebuilt with this call's arcs

    // --- Shared WL-1/WL-2/WL-3 machinery (revmap / arc_signals / lin_of / iono_of /
    // primary / band_top_of / λ cache).  HOISTED above the group build because the WL-3
    // pair regroup routes each primary-band ambiguity by its (rx,sat) primary pair.
    // Byte-identical when every lane is off: nothing here is populated, and building
    // it earlier has no effect on the solve (pure reads of station state).
    const bool mw_mach_on = mw_wl_on || ewl_on || lane_on;
    auto sysFromSat = [](const std::string& sat) -> System {
        if (sat.empty()) return System::Unknown;
        switch (sat.front()) {
            case 'G': return System::GPS;     case 'E': return System::Galileo;
            case 'R': return System::GLONASS; case 'C': return System::BeiDou;
            case 'J': return System::QZSS;    case 'S': return System::SBAS;
            default:  return System::Unknown;
        }
    };
    // Reverse map (est_station,col)->arc identity and per-(rx,sat) ambiguity
    // signals over ALL resolvable ambiguities (both bands, incl. those NOT in
    // shared_phase_keys — the WL row needs the L2 column even when L2 is not an
    // eq34 arc).  GLONASS excluded (never enters the carry blocks anyway).
    std::map<std::pair<int, int>, Rev> revmap;
    std::map<std::pair<int, std::string>, std::vector<std::pair<std::string, int>>> arc_signals;
    // (est_station, local col) → ambiguity LINEARIZATION value (m).  The kernel
    // solves corrections about x_lin (no KF write-back), so the WL layer must
    // re-express the absolute MW measurement in the correction frame (F5).
    std::map<std::pair<int, int>, double> lin_of;
    // WL-2 §3.4 v2: (rx app index, sat) → the estimated L1-referenced slant iono (m)
    // for the bridge de-bias.  The v1 Welford-only guard is falsified by a STABLE
    // differential iono (Fable): a constant iono bias has ~0 Welford variance yet
    // biases the rounded WL integer (near-integer bias ⇒ slips the FFR frac gate).
    // We subtract the network's own iono estimate; the residual DD-iono estimate
    // error surfaces in |frac| and is caught by the FFR gate + MW(1,2) closure.
    std::map<std::pair<int, std::string>, double> iono_of;
    // Estimator primary pair per (rx,sat): the two lowest DISTINCT bands (ties
    // lexicographic for determinism); v1 restricts WL rows to this pair (§11 Q3).
    std::map<std::pair<int, std::string>, PP> primary;
    // WL-2 EWL: the band-5 (GPS L5 / Galileo E5a) signal + column per (rx,sat),
    // for the (band2, band5) extra-wide lane.  arc_signals is sorted by band digit
    // then name, so the first '5' is deterministic.
    std::map<std::pair<int, std::string>, std::pair<std::string, int>> band_top_of;
    // Increment 5 (uwl_dual_top): the FALLBACK top rung's signal + column per
    // (rx,sat) (Galileo '7' when the preferred b_top is '8').  Empty unless a
    // tier arms a fallback rung; the fallback lane instance's resolver and the
    // walk's fallback-tagged rows read THIS map instead of band_top_of.
    std::map<std::pair<int, std::string>, std::pair<std::string, int>> band_top2_of;
    // E6 mid hop (uwl_mid_enable): the mid-rung band signal per (rx,sat) — only
    // the SIGNAL is needed (the hops are phase-only detector reads; no estimator
    // column enters the chain).  Empty unless the tier arms a mid rung.
    std::map<std::pair<int, std::string>, std::string> band_mid_of;
    // WL-3: the constellation's ONE census-preferred primary pair — only arcs whose
    // primary pair matches it join the pair group; minority-channel arcs stay fully
    // per-band (as today), so they keep their own carried cycles instead of spawning
    // flickering micro pair-groups.  The third (EWL) band never becomes a pair band.
    std::map<char, std::pair<std::string, std::string>> lane_pref_pair;
    // Per-(sat,signal) wavelength cache (FDMA-correct via the satellite id).
    std::map<std::pair<std::string, std::string>, double> lam_cache;
    auto lam = [&](const std::string& sat, const std::string& sig) -> double {
        const auto k = std::make_pair(sat, sig);
        const auto it = lam_cache.find(k);
        if (it != lam_cache.end()) return it->second;
        const double v = mw_wl_ppp_utils->getWavelength(sysFromSat(sat), sig, sat);
        lam_cache.emplace(k, v);
        return v;
    };
    if (mw_mach_on) {
        for (size_t i = 0; i < stations.size(); ++i) {
            if (est_index_of[i] < 0) continue;
            const int es = est_index_of[i];
            const auto& h2l = asm_res[i].h2local;
            const Eigen::VectorXd x_lin = stations[i].sm->getStateVector();
            for (const auto& [key, sidx] : stations[i].sm->getAmbiguities()) {
                const auto parts = StateKeys::parseAmbiguityKey(key);
                if (!parts || parts->satellite.empty() || parts->signal.empty()) continue;
                if (parts->satellite.front() == 'R') continue;   // GLONASS excluded
                const auto hit = h2l.find(static_cast<int>(sidx));
                if (hit == h2l.end() || hit->second < 0) continue;
                const int col = static_cast<int>(hit->second);
                revmap[{es, col}] = { static_cast<int>(i), parts->satellite, parts->signal };
                arc_signals[{static_cast<int>(i), parts->satellite}]
                    .push_back({ parts->signal, col });
                if (static_cast<Eigen::Index>(sidx) < x_lin.size())
                    lin_of[{es, col}] = x_lin(static_cast<Eigen::Index>(sidx));
            }
            if (wl_inject_on || lane_bridge_on) {
                std::map<std::string, double> sat_iono;   // per-sample de-bias push
                for (const auto& [sat, iidx] : stations[i].sm->getIonosphereStates()) {
                    const Eigen::Index ii = static_cast<Eigen::Index>(iidx);
                    if (ii >= 0 && ii < x_lin.size()) {
                        iono_of[{ static_cast<int>(i), sat }] = x_lin(ii);
                        if (bridge_debias_on) sat_iono[sat] = x_lin(ii);
                    }
                }
                // 60-ep age-bias fix: push this station's estimable slant-iono
                // values for the detector's per-sample bridge de-bias.  Applies
                // from the NEXT detector pass (one-epoch lag, F1-consistent);
                // idempotent within an epoch (RAIM re-solve safe).
                if (bridge_debias_on && stations[i].mw_slip)
                    stations[i].mw_slip->setBridgeIonoEstimates(std::move(sat_iono));
            }
        }
        // WL-3: network-preferred channel per (constellation, band).  The
        // lexicographic tie-break fragments the primary pairs across stations
        // (L2L where tracked, L2W elsewhere), which splits the pair groups'
        // graphs and forfeits every cross-boundary cycle (~1/3 of the primary
        // cycle space at 50 stations, measured).  Under lane_carry the pair
        // prefers the constellation's MAJORITY channel per band (deterministic
        // census over this build's resolvable ambiguities; ties lexicographic),
        // falling back to the arc's own lowest channel where the majority one
        // is untracked.  WL-1/WL-2-only shapes keep the legacy tie-break.
        std::map<std::pair<char, char>, std::string> chan_pref;
        if (lane_on) {
            std::map<std::pair<char, std::string>, long long> chan_count;
            for (const auto& [rs, sigs] : arc_signals) {
                const char cons = rs.second.empty() ? '?' : rs.second.front();
                for (const auto& [sig, col] : sigs) ++chan_count[{cons, sig}];
            }
            for (const auto& [k, cnt] : chan_count) {
                const char band = k.second.size() >= 2 ? k.second[1] : '\0';
                if (band == '\0') continue;
                std::string& best = chan_pref[{k.first, band}];
                long long bc = -1;
                if (!best.empty()) {
                    const auto bit = chan_count.find({k.first, best});
                    bc = (bit != chan_count.end()) ? bit->second : -1;
                }
                if (best.empty() || cnt > bc || (cnt == bc && k.second < best))
                    best = k.second;
            }
            // UWL-1 (measured falsification of the per-band census): channel
            // choices are CORRELATED per receiver family — Galileo splits into
            // (L1C,L5Q) and (L1X,L5X) families, and the per-band majorities
            // composed the PHANTOM pair (L1C,L5X) that NO station tracks: zero
            // pair arcs formed and the whole constellation silently stayed
            // per-band (group census, 10-ep GPS+GAL smoke).  Under the armed
            // card the census is therefore JOINT: count each (rx,sat)'s raw
            // two-lowest-band channel pair and elect the majority pair per
            // constellation (ties lexicographic — the §8.2 deterministic seam),
            // then steer the per-band preferences to the elected pair so
            // preferChan / the bridge keys align with it.  The minority FAMILY
            // stays fully per-band (v1 accepts ~half the Galileo pair space;
            // per-family pair groups are the v2 recovery — the group keying
            // already supports them).  uwl_enable=false keeps the per-band
            // census verbatim (the WL-3 validated shape, byte-identical).
            if (uwl_on) {
                std::map<std::pair<char, std::pair<std::string, std::string>>,
                         long long> pair_count;
                for (auto& [rs, sigs] : arc_signals) {
                    std::sort(sigs.begin(), sigs.end(),
                              [](const std::pair<std::string, int>& a,
                                 const std::pair<std::string, int>& b) {
                        const char ba = a.first.size() >= 2 ? a.first[1] : '\0';
                        const char bb = b.first.size() >= 2 ? b.first[1] : '\0';
                        if (ba != bb) return ba < bb;
                        return a.first < b.first;
                    });
                    if (sigs.empty()) continue;
                    const char cons = rs.second.empty() ? '?' : rs.second.front();
                    // Two lowest CARRIED bands (the uncarried top band never
                    // pairs — mirror the routing's exclusion exactly).
                    std::string s1;
                    char b1 = '\0';
                    for (const auto& [sig, col] : sigs)
                        if (sig.size() >= 2 && !isUncarriedThirdBand(cons, sig)) {
                            s1 = sig; b1 = sig[1]; break;
                        }
                    if (b1 == '\0') continue;
                    for (const auto& [sig, col] : sigs)
                        if (sig.size() >= 2 && sig[1] != b1
                            && !isUncarriedThirdBand(cons, sig)) {
                            ++pair_count[{cons, { s1, sig }}];
                            break;
                        }
                }
                std::map<char, std::pair<std::string, std::string>> joint_best;
                std::map<char, long long> joint_cnt;
                for (const auto& [k, cnt] : pair_count) {
                    auto itb = joint_best.find(k.first);
                    if (itb == joint_best.end() || cnt > joint_cnt[k.first]
                        || (cnt == joint_cnt[k.first] && k.second < itb->second)) {
                        joint_best[k.first] = k.second;
                        joint_cnt[k.first]  = cnt;
                    }
                }
                for (const auto& [cons, pr] : joint_best) {
                    const char b1 = pr.first.size()  >= 2 ? pr.first[1]  : '\0';
                    const char b2 = pr.second.size() >= 2 ? pr.second[1] : '\0';
                    if (b1 == '\0' || b2 == '\0') continue;
                    chan_pref[{cons, b1}] = pr.first;
                    chan_pref[{cons, b2}] = pr.second;
                }
            }
        }
        for (auto& [rs, sigs] : arc_signals) {
            std::sort(sigs.begin(), sigs.end(),
                      [](const std::pair<std::string, int>& a,
                         const std::pair<std::string, int>& b) {
                const char ba = a.first.size() >= 2 ? a.first[1] : '\0';
                const char bb = b.first.size() >= 2 ? b.first[1] : '\0';
                if (ba != bb) return ba < bb;
                return a.first < b.first;
            });
            PP pp;
            if (!sigs.empty()) { pp.sig1 = sigs.front().first; pp.col1 = sigs.front().second; }
            const char b1 = pp.sig1.size() >= 2 ? pp.sig1[1] : '\0';
            for (const auto& [sig, col] : sigs) {
                if (sig.size() >= 2 && b1 != '\0' && sig[1] != b1) {
                    pp.sig2 = sig; pp.col2 = col; break;
                }
            }
            if (lane_on && !sigs.empty()) {
                const char cons = rs.second.empty() ? '?' : rs.second.front();
                const char b2 = pp.sig2.size() >= 2 ? pp.sig2[1] : '\0';
                auto preferChan = [&](char band, std::string& sig_io, int& col_io) {
                    const auto pit2 = chan_pref.find({cons, band});
                    if (pit2 == chan_pref.end() || pit2->second == sig_io) return;
                    for (const auto& [sig, col] : sigs)
                        if (sig == pit2->second) { sig_io = sig; col_io = col; return; }
                };
                if (b1 != '\0') preferChan(b1, pp.sig1, pp.col1);
                if (b2 != '\0') preferChan(b2, pp.sig2, pp.col2);
            }
            pp.ok = (!pp.sig1.empty() && !pp.sig2.empty());
            primary[rs] = pp;
        }
        // v2 (uwl_family_pairs): admit MINORITY family pairs into the run-latched
        // admissible set.  Counted over the STEERED primary pairs — the exact
        // quantity the routing below matches — NOT the raw two-lowest census
        // (measured defect, smoke E: L2L < L2W lexicographically, so every GPS
        // dual-tracker's RAW pair read (L1C,L2L) and inflated its count to 143
        // while routing saw 1 steered arc — a stranded 1-arc micro pair-group
        // that forfeits the arc's per-band cycles).  The majority pair routes
        // through lane_pref_pair exactly as v1 (its admission here is a harmless
        // duplicate); a minority pair is admitted the first epoch its steered
        // count reaches the floor, then stays latched (see the declaration).
        if (uwl_family_on) {
            std::map<std::pair<char, std::pair<std::string, std::string>>,
                     long long> pp_count;
            for (const auto& [rs, pp] : primary) {
                if (!pp.ok) continue;
                const char cons = rs.second.empty() ? '?' : rs.second.front();
                ++pp_count[{ cons, { pp.sig1, pp.sig2 } }];
            }
            for (const auto& [k, cnt] : pp_count) {
                if (cnt < nmw.uwl_family_min_arcs) continue;
                if (lane_admit_pairs[k.first].insert(k.second).second
                    && lane_family_logged
                           .insert({ k.first,
                                     k.second.first + "+" + k.second.second })
                           .second)
                    log->info("Network (M5/UWL-1 family census): constellation '{}' "
                              "admitted pair ({}, {}) — {} steered primary arc(s) "
                              "(floor {}); latched for the run.",
                              k.first, k.second.first, k.second.second, cnt,
                              nmw.uwl_family_min_arcs);
            }
        }
        if (lane_on) {
            // The constellation's ONE preferred pair: the two lowest census bands,
            // skipping the EWL third band (it must stay UNCARRIED — WL-2 §3.1).
            std::map<char, std::vector<char>> cons_bands;
            for (const auto& [k, sig] : chan_pref) {
                const std::string probe = std::string("L") + k.second;   // "L<band>"
                if (isUncarriedThirdBand(k.first, probe)) continue;
                cons_bands[k.first].push_back(k.second);
            }
            for (auto& [cons, bands] : cons_bands) {
                std::sort(bands.begin(), bands.end());
                if (bands.size() < 2) continue;
                lane_pref_pair[cons] = { chan_pref[{cons, bands[0]}],
                                         chan_pref[{cons, bands[1]}] };
                // Census visibility (once per run per cons): which pair the lane
                // carries — fragmentation across channels is the WL-3 lesson and
                // a Galileo pre-flight (SPEC_uwl_card.md §8.1); measure, don't assume.
                if (lane_census_logged.insert(cons).second)
                    log->info("Network (M5/WL-3 census): constellation '{}' preferred "
                              "primary pair = ({}, {}) over census bands [{}].",
                              cons, lane_pref_pair[cons].first,
                              lane_pref_pair[cons].second,
                              std::string(bands.begin(), bands.end()));
            }
            // Align the detector-side bridge arcs with the census: without this,
            // every (rx,sat) whose min-name channel differs from the census pair
            // has NO bridge arc under the caller's key and the WL decision path
            // starves (measured 9-of-29 decided cycles).  Applies from the next
            // detector pass; the re-keyed arcs re-warm in ~2 epochs.
            if (lane_bridge_on && !chan_pref.empty())
                for (size_t i = 0; i < stations.size(); ++i)
                    if (stations[i].mw_slip)
                        stations[i].mw_slip->setBridgePreferredChannels(chan_pref);
        }
        for (auto& [rs, sigs] : arc_signals) {
            // UWL-1 §3.1: the top-tier partner band digit.  Legacy (card OFF) =
            // first '5' for EVERY constellation — which is what silently killed
            // Galileo (its '5' is E5a, the primary sig2 itself ⇒ the degenerate
            // (L5Q,L5Q) pair with no detector arc).  Armed = the lane-tier table
            // (GPS '5' reproduces the legacy pick bit-exactly; Galileo '7'/'8').
            char top_digit  = '5';
            char mid_digit  = '\0';
            char top2_digit = '\0';   // increment 5: the fallback rung's digit
            const char cons = rs.second.empty() ? '?' : rs.second.front();
            if (uwl_on) {
                const auto tier = mwLaneTierOf(cons, nmw.uwl_gal_top_band,
                                               nmw.uwl_mid_enable, nmw.uwl_dual_top);
                if (!tier) {
                    if (ewl_on && cons != '?' && uwl_warned_cons.insert(cons).second)
                        log->warn("Network (M5/UWL-1): constellation '{}' has NO lane "
                                  "tier in the armed table — its cycles contribute no "
                                  "EWL/UWL statistic and no bridge (BDS-3 deferred on "
                                  "clock divergence; GLONASS FDMA-excluded).  Loud by "
                                  "design (SPEC §3.1).", cons);
                    continue;   // no top-band entry ⇒ counted skip_missing_band downstream
                }
                top_digit  = tier->b_top;
                mid_digit  = tier->b_mid;
                top2_digit = tier->b_top2;
            } else if (ewl_on && cons != 'G' && cons != '?'
                       && uwl_warned_cons.insert(cons).second) {
                log->warn("Network (M5/UWL-1): constellation '{}' enters the EWL lane "
                          "under the LEGACY GPS band digits — its top-tier pair is "
                          "degenerate and every cycle dies silently as skip_missing_mw "
                          "(SPEC_uwl_card.md §1 fact 3).  Arm network.mw_wl.uwl_enable "
                          "for the per-constellation lane-tier table.", cons);
            }
            for (const auto& [sig, col] : sigs)
                if (sig.size() >= 2 && sig[1] == top_digit) { band_top_of[rs] = { sig, col }; break; }
            if (top2_digit != '\0')
                for (const auto& [sig, col] : sigs)
                    if (sig.size() >= 2 && sig[1] == top2_digit) { band_top2_of[rs] = { sig, col }; break; }
            if (mid_digit != '\0')
                for (const auto& [sig, col] : sigs)
                    if (sig.size() >= 2 && sig[1] == mid_digit) { band_mid_of[rs] = sig; break; }
        }
    }
    for (size_t i = 0; i < stations.size(); ++i) {
        if (est_index_of[i] < 0) continue;
        const auto& h2l = asm_res[i].h2local;
        for (const auto& [key, sidx] : stations[i].sm->getAmbiguities()) {
            const auto parts = StateKeys::parseAmbiguityKey(key);
            if (!parts || parts->satellite.empty()) continue;
            const char cons = parts->satellite.front();
            if (cons == 'R') {   // GLONASS FDMA: no common-λ integer DD (spec OPEN-3)
                if (!carry_glonass_warned) {
                    log->warn("Network[Card4]: GLONASS ambiguities are EXCLUDED from the "
                              "invariant carry (FDMA — no common wavelength for an integer "
                              "DD; IE-FDMA is a v2 item).  They keep their legacy treatment.");
                    carry_glonass_warned = true;
                }
                continue;
            }
            // The satellite phase-bias key is per-SATELLITE ("<sat>|<sig>|PB") — NOT
            // the per-system receiver-bias key.  Only ambiguities backing an ungated
            // shared phase bias (a vertex of the eq34 graph) become DD arcs.
            // WL-2 §3.1: keep the constellation's EWL third band OUT of the carry
            // (uncarried lane ⇒ no stiff prior ⇒ EWL rows win).  OFF ⇒ not taken.
            if (ewl_carry_exclude && isUncarriedThirdBand(cons, parts->signal)) continue;
            const std::string pbkey =
                StateKeys::makeSatPhaseBiasKey(parts->satellite, parts->signal);
            if (!shared_phase_keys.count(pbkey)) continue;   // not an eq34 arc (gated/legacy lump)
            const auto hit = h2l.find(static_cast<int>(sidx));
            if (hit == h2l.end() || hit->second < 0) continue;   // unresolvable ⇒ chord refloats
            // C3c slip feed: a PASS-A slip detection (MW wide-lane / LLI / IF-phase,
            // recorded at the detector so the PASS-B pending-reseed clear can't hide
            // it) or a within-epoch RAIM culprit (§3) invalidates this arc's carried
            // DD — re-tap it fresh this epoch instead of folding a stiff pre-slip prior.
            auto ambSlipped = [&](const std::string& amb_key) -> bool {
                if (carry_slipped_ambs.count({static_cast<int>(i), amb_key})) return true;
                if (carry_we_drop_sats
                    && carry_we_drop_sats->count(parts->satellite)) return true;
                if (carry_we_drop_arcs) {
                    const auto dit = carry_we_drop_arcs->find(static_cast<int>(i));
                    if (dit != carry_we_drop_arcs->end()
                        && dit->second.count(parts->satellite)) return true;
                }
                return false;
            };
            int& sv = carry_sat_vertex[parts->satellite];
            if (sv == 0) sv = kSatBase + static_cast<int>(carry_sat_vertex.size());

            // --- WL-3 pair routing (SPEC_lane_carry_card.md §2): a primary-pair
            // ambiguity whose partner band ALSO passes every per-band test (eq34-
            // shared pbkey + resolvable column) joins the constellation's PAIR
            // group (emitted once, from the band-1 iteration, with both columns);
            // otherwise it falls through to the residual per-band group as today.
            if (lane_on) {
                const auto pit = primary.find({ static_cast<int>(i), parts->satellite });
                const PP* pp = (pit != primary.end() && pit->second.ok)
                                   ? &pit->second : nullptr;
                // Only the constellation's census-preferred pair joins the pair
                // group — a minority-channel (rx,sat) keeps today's per-band carry
                // (its cycles survive per band instead of spawning a flickering
                // micro pair-group), and the EWL third band never pairs.
                // v2 (uwl_family_pairs): additionally, a LATCHED minority-family
                // pair routes into its OWN pair group (the group key is already
                // the pair string, so families stay disjoint graphs).
                const auto lpp = lane_pref_pair.find(cons);
                bool pair_admissible =
                    pp && lpp != lane_pref_pair.end()
                    && pp->sig1 == lpp->second.first && pp->sig2 == lpp->second.second;
                if (!pair_admissible && uwl_family_on && pp) {
                    const auto fit = lane_admit_pairs.find(cons);
                    pair_admissible = fit != lane_admit_pairs.end()
                        && fit->second.count({ pp->sig1, pp->sig2 }) != 0;
                }
                if (pair_admissible
                    && (parts->signal == pp->sig1 || parts->signal == pp->sig2)) {
                    const bool is1 = parts->signal == pp->sig1;
                    const std::string& other_sig = is1 ? pp->sig2 : pp->sig1;
                    const int other_col          = is1 ? pp->col2 : pp->col1;
                    const bool other_ok = other_col >= 0
                        && shared_phase_keys.count(
                               StateKeys::makeSatPhaseBiasKey(parts->satellite, other_sig));
                    if (other_ok) {
                        if (!is1) continue;   // the pair arc was/will be emitted from band-1
                        int& pgk = carry_group_id[{cons, pp->sig1 + "+" + pp->sig2}];
                        if (pgk == 0) pgk = static_cast<int>(carry_group_id.size());
                        auto& G = groups[pgk];
                        G.key = pgk;
                        if (!G.pair) {
                            G.pair    = true;
                            G.lambda1 = lam(parts->satellite, pp->sig1);
                            G.lambda2 = lam(parts->satellite, pp->sig2);
                        }
                        const std::string key2 =
                            StateKeys::makeAmbiguityKey(parts->satellite, pp->sig2);
                        NetworkInvariantCarryDriver::ArcInput a;
                        a.rx_vertex   = static_cast<int>(i);
                        a.sat_vertex  = sv;
                        a.tree_edge   = stations[i].pinned_amb_targets.count(key) != 0;
                        a.est_station = est_index_of[i];
                        a.local_col   = static_cast<int>(hit->second);
                        a.local_col2  = pp->col2;
                        // Per-band slip states; the driver unions them under the LANE
                        // shape (§3.4 — either band refloats both) and keeps them
                        // per-band under the §6.1 equivalence shape (exactly the two
                        // per-band groups' behavior).
                        a.slipped     = ambSlipped(key);
                        a.slipped2    = ambSlipped(key2);
                        // Diagnostic: band-1's eq34 tree tag governs the pair basis;
                        // count disagreements with band-2's election (a mismatch means
                        // the two per-band trees differ on the shared graph, so the
                        // §6.1 equivalence leg is not exact for this component).
                        if (a.tree_edge
                            != (stations[i].pinned_amb_targets.count(key2) != 0))
                            ++lane_pair_tree_mismatch;
                        ++lane_pair_arcs;
                        G.arcs.push_back(a);
                        if (carry_innov_gate) {
                            carry_arc_amb_key[pgk][ArcKey(static_cast<int>(i), sv)]  = key;
                            carry_arc_amb_key2[pgk][ArcKey(static_cast<int>(i), sv)] = key2;
                        }
                        continue;
                    }
                    ++lane_resid_arcs;   // partner missing/unshared → residual per-band group
                }
            }

            int& gk = carry_group_id[{cons, parts->signal}];
            if (gk == 0) gk = static_cast<int>(carry_group_id.size());
            NetworkInvariantCarryDriver::ArcInput a;
            a.rx_vertex   = static_cast<int>(i);
            a.sat_vertex  = sv;
            a.tree_edge   = stations[i].pinned_amb_targets.count(key) != 0;
            a.est_station = est_index_of[i];
            a.local_col   = static_cast<int>(hit->second);
            a.slipped     = ambSlipped(key);
            groups[gk].key = gk;
            groups[gk].arcs.push_back(a);
            // D5: record chord arcs for the innovation-flag reverse lookup (tree edges
            // never carry a DD, so they cannot be flagged — but record all; harmless).
            if (carry_innov_gate)
                carry_arc_amb_key[gk][ArcKey(static_cast<int>(i), sv)] = key;
        }
    }
    std::vector<NetworkInvariantCarryDriver::GroupInput> glist;
    glist.reserve(groups.size());
    for (auto& [k, g] : groups) glist.push_back(std::move(g));
    auto blocks = carry_driver.buildDdPriors(glist);
    // UWL-1 group census (once per run, log-only): which carry groups exist and
    // which are PAIR groups — the WL-3 fragmentation pre-flight (SPEC §8.1).
    if (uwl_on && lane_census_logged.insert('#').second)
        for (const auto& [gk, gid] : carry_group_id) {
            std::size_t n_arcs = 0; bool is_pair = false;
            for (const auto& gi : glist)
                if (gi.key == gid) { n_arcs = gi.arcs.size(); is_pair = gi.pair; break; }
            log->info("Network (M5/UWL-1 group census): group {} = cons '{}' '{}'"
                      "{} ({} arc(s) this epoch).",
                      gid, gk.first, gk.second, is_pair ? " [PAIR]" : "", n_arcs);
        }

    // --- M5 card WL-1: append the MW-seeded wide-lane pseudo-obs blocks. ---
    // Ride the carry's chord basis: each carry block row is a fundamental cycle on
    // ONE band's ambiguity columns; the WL layer pairs each cycle arc to its L2
    // ambiguity column and folds Σ c·MW̄/λ_WL as a rank-K constraint on the
    // cross-band columns.  Appended AFTER the carry blocks so their indices [0,Nc)
    // stay valid for installPosterior (which reads only its own Nc blocks).
    if (mw_mach_on)
        networkMwWlInject(
            ctx, config_, log, traces, carry_driver, stations,
            se, carry_slipped_ambs, carry_we_drop_sats, carry_we_drop_arcs, n_threads, ewl_lanes,
            ewl_cfg, mw_wl_layer, mw_wl_f6, mw_wl_epoch, wl_inject_epoch, lane_trace_epoch,
            wl_epoch, wl_cons_epoch, revmap, lin_of, iono_of, primary,
            band_top_of, band_top2_of, band_mid_of, lam, blocks, mw_wl_on,
            ewl_on, uwl_on, uwl_cold_on, uwl_mid_on, uwl_dual_on, uwl_sib_gate_on,
            wl_inject_on, bridge_debias_on, lane_on, lane_meas_on, lane_bridge_on, lane_fix_on,
            lane_parallel_components, mw_d5_dump, mw_lane_trace, mw_tagsets, mw_wl_f6_dump);
    // LF-1 INC-1: under fold_blocked the lane primitives skipped their per-row
    // re-symmetrize; restore exact symmetry once per joint block now that this epoch's
    // MW / bridge / fix rows are all applied (no-op unless fold_blocked).  Runs on every
    // build incl. the RAIM re-solve — the blocks are rebuilt each pass.
    carry_driver.finalizeLaneBatch(blocks);
    est.setDdPriors(std::move(blocks));
}

} // namespace GnssProcessor
