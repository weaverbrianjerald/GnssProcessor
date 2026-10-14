/**
 * @file NetworkDdPriorBuild.cpp
 * @brief Implementation of the network DD-prior build steps (see the header banner).
 *        Bodies relocated verbatim (8-column dedent, no substitutions) from
 *        Application::runNetwork at decomposition INC-9b.
 *        networkBuildEnsembleGauges needs no alias preamble (its parameters are the
 *        former captures); networkBuildCarryDdPriors opens with a copy of the run TU's
 *        nmw/trace and ctx.carry aliases so its body reads the original names.  The
 *        MW/WL inject part of the original body now lives in NetworkMwWlGlue.cpp.
 */

#include "filter/NetworkDdPriorBuild.hpp"

#include "app/NetworkRunContext.hpp"          // ctx.carry / ctx.counters / ctx.knobs
#include "config/GnssConfig.hpp"              // config_.getNetworkConfig() / .mw_wl
#include "common/GnssIdentity.hpp"            // systemOfSat, SignalId (band digit, wavelength)
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
    // Seed-once and time-lock: a (sat,signal) that already carries a held bias
    // (from the previous epoch's D2 stash) takes its datum from that stiff carried
    // prior, so no Σz=0 gauge is folded for it; a hard (w→∞) fold would override
    // the finite hold prior and re-realize the level, which then changes from
    // epoch to epoch.  Only fresh or re-seeded (sat,signal) get a gauge this
    // epoch.  Empty when the hold is off.
    std::set<std::string> held;
    if (phase_hold_on)
        for (const CarryPrior& cp : prev_carry)
            // var>0 is the same guard the hold-prior injector applies, so a
            // (sat,signal) skips the fold iff it will get a hold prior.  A
            // var<=0 carry (for example from a corrupt warm-resume snapshot) is
            // not treated as held here; it re-seeds via the fold instead of
            // being left without a datum.
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
        // estimator can treat it as an exact datum column in the datum-frame
        // conditional covariance (the fold leaves it with an artificial
        // gauge-direction variance; see SumZeroGauge::shared_col).  k is
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
    est.contributionKind(ContributionKind::EnsemblePhaseGauge);   // card NOM N2a
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
    const std::set<std::pair<int, std::string>>& carry_slipped_raw,
    const WithinEpochCulprits& we_culprits,
    int n_threads,
    NetworkLaneRun& lane_run,
    const std::function<bool(char, const std::string&)>& isUncarriedThirdBand) {
    // --- Alias preamble: copies of the run-TU aliases the relocated
    //     buildCarryDdPriors body reads (nmw/trace and the ctx.carry aliases). ----
    const auto& nmw = config_.getNetworkConfig().mw_wl;
    auto& trace = traces.trace;
    // The lane machinery (NetworkLaneRun) and the lane switches (NetworkKnobs), under their
    // former parameter names (card NOM step TD1a).  The third band is excluded from the
    // carry exactly when the EWL lane is armed, so ewl_carry_exclude is ewl_on.
    std::vector<EwlLane>& ewl_lanes = lane_run.ewl_lanes;
    const NetworkMwWlLayer::Config& ewl_cfg = lane_run.ewl_cfg;
    std::vector<WlInjectPlan>& wl_inject_epoch = lane_run.wl_inject_epoch;
    std::vector<LaneTraceEntry>& lane_trace_epoch = lane_run.lane_trace_epoch;
    WlCounters& wl_epoch = lane_run.wl_epoch;
    std::map<char, WlCounters>& wl_cons_epoch = lane_run.wl_cons_epoch;
    const bool carry_innov_gate = ctx.knobs.carry_innov_gate;
    const bool ewl_on = ctx.knobs.ewl_on;
    const bool uwl_on = ctx.knobs.uwl_on;
    const bool uwl_cold_on = ctx.knobs.uwl_cold_on;
    const bool uwl_mid_on = ctx.knobs.uwl_mid_on;
    const bool uwl_dual_on = ctx.knobs.uwl_dual_on;
    const bool uwl_family_on = ctx.knobs.uwl_family_on;
    const bool uwl_sib_gate_on = ctx.knobs.uwl_sib_gate_on;
    const bool wl_inject_on = ctx.knobs.wl_inject_on;
    const bool bridge_debias_on = ctx.knobs.bridge_debias_on;
    const bool ewl_carry_exclude = ctx.knobs.ewl_on;
    const bool lane_on = ctx.knobs.lane_on;
    const bool lane_meas_on = ctx.knobs.lane_meas_on;
    const bool lane_bridge_on = ctx.knobs.lane_bridge_on;
    const bool lane_fix_on = ctx.knobs.lane_fix_on;
    const bool lane_parallel_components = ctx.knobs.lane_parallel_components;
    const bool mw_d5_dump = ctx.knobs.mw_d5_dump;
    const bool mw_lane_trace = ctx.knobs.mw_lane_trace;
    const bool mw_tagsets = ctx.knobs.mw_tagsets;
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
    // Keep a funnel entry made only by D5 out of the carry-membership exclusion (see
    // ambSlipped below; card 4, M1, D5 v4).  The loader already forces the knob false
    // without carry_innov_gate; the && also covers a context not built by the loader.
    const bool retap_only =
        carry_innov_gate && config_.networkCarryInvariantInnovRetapOnly();

    if (!carry_invariant) return;
    // The reader's end of the culprit hand-off (card WCF): a rebuild that reads a culprit
    // satellite or arc is a T2 re-solve's, and what it withholds is counted here, where the
    // withhold takes effect.  The arc ambiguities it marks slipped for a culprit (and not for
    // the slip funnel) are collected in a set, so each counts once per rebuild however often
    // ambSlipped below is asked about it; they are added after the station loop.  Every arc
    // of the carry graph is asked, tree edges (which carry no DD) and arcs born this epoch
    // included, so the count bounds the carried DDs withheld from above.
    const bool culprits_read = we_culprits.namesAmbiguities();
    std::set<std::pair<int, std::string>> culprit_marked;   // (station, ambiguity key)
    if (culprits_read) {
        auto& wcf = ctx.counters.we_culprit_withhold;
        ++wcf.rebuilds;
        wcf.sats += static_cast<long long>(we_culprits.sats.size());
        wcf.arcs += static_cast<long long>(we_culprits.arcCount());
    }
    constexpr int kSatBase = 1000000;   // satellite vertices sit above every receiver index
    std::map<int, NetworkInvariantCarryDriver::GroupInput> groups;
    if (carry_innov_gate) { carry_arc_amb_key.clear(); carry_arc_amb_key2.clear(); }  // rebuilt with this call's arcs (D5)

    // --- Shared WL-2/WL-3 machinery (revmap / arc_signals / lin_of / iono_of /
    // primary / band_top_of / λ cache).  Built before the group build because the WL-3
    // pair regroup routes each primary-band ambiguity by its (rx,sat) primary pair.
    // Byte-identical when every lane is off: nothing here is populated, and building
    // it earlier has no effect on the solve (it only reads station state).
    const bool mw_mach_on = ewl_on || lane_on;
    // Reverse map (est_station,col)->arc identity and per-(rx,sat) ambiguity
    // signals over all resolvable ambiguities (both bands, including those not in
    // shared_phase_keys: the WL row needs the L2 column even when L2 is not an eq34
    // arc).  GLONASS is excluded (it never enters the carry blocks).
    std::map<std::pair<int, int>, Rev> revmap;
    std::map<std::pair<int, std::string>, std::vector<std::pair<std::string, int>>> arc_signals;
    // (est_station, local col) → ambiguity linearization value (m).  The kernel
    // solves corrections about x_lin (no KF write-back), so the WL layer
    // re-expresses the absolute MW measurement in the correction frame (F5).
    std::map<std::pair<int, int>, double> lin_of;
    // (rx app index, sat) → the estimated L1-referenced slant iono (m) for the bridge
    // de-bias (WL-2 §3.4 v2).  A Welford-variance guard alone (v1) misses a stable
    // differential iono: a constant iono bias has ~0 Welford variance but still biases
    // the rounded WL integer (a near-integer bias passes the FFR fraction gate).  The
    // network's own iono estimate is therefore subtracted; the remaining DD-iono
    // estimate error shows in |frac| and is caught by the FFR gate and the MW(1,2)
    // closure.
    std::map<std::pair<int, std::string>, double> iono_of;
    // Estimator primary pair per (rx,sat): the two lowest distinct bands (ties
    // lexicographic for determinism); v1 restricts WL rows to this pair (§11 Q3).
    std::map<std::pair<int, std::string>, PP> primary;
    // Top-tier band signal and column per (rx,sat), for the (band2, top band)
    // extra-wide lane (WL-2 EWL).  The top digit is '5' (GPS L5 / Galileo E5a)
    // unless uwl is armed, in which case the lane-tier table's b_top is used (see the
    // top_digit loop below).  arc_signals is sorted by band digit then name, so the
    // first match is deterministic.
    std::map<std::pair<int, std::string>, std::pair<std::string, int>> band_top_of;
    // The fallback top rung's signal and column per (rx,sat) (Galileo '7' when the
    // preferred b_top is '8'; increment 5, uwl_dual_top).  Empty unless a tier arms a
    // fallback rung; the fallback lane instance's resolver and the walk's
    // fallback-tagged rows read this map instead of band_top_of.
    std::map<std::pair<int, std::string>, std::pair<std::string, int>> band_top2_of;
    // The mid-rung band signal per (rx,sat) (E6 mid hop, uwl_mid_enable).  Only the
    // signal is needed: the hops are phase-only detector reads, and no estimator
    // column enters the chain.  Empty unless the tier arms a mid rung.
    std::map<std::pair<int, std::string>, std::string> band_mid_of;
    // The constellation's single census-preferred primary pair (WL-3).  Only arcs
    // whose primary pair matches it join the pair group; minority-channel arcs stay
    // fully per-band, so they keep their own carried cycles instead of forming small
    // pair groups that appear and disappear.  The third (EWL) band never becomes a
    // pair band.
    std::map<char, std::pair<std::string, std::string>> lane_pref_pair;
    // Per-(sat,signal) wavelength cache.  Every caller holds an arc of arc_signals or of
    // the carry below, and both exclude GLONASS, so no FDMA slot is needed (slot 0); a
    // code that does not parse has no wavelength (0).  The band is read from the digit
    // alone, as PppUtils::getWavelength read it, so BeiDou digit 1 is B1C here as at the
    // chain datum's rounding; the BeiDou digit-1 decision is owed (card NOM section 12).
    std::map<std::pair<std::string, std::string>, double> lam_cache;
    auto lam = [&](const std::string& sat, const std::string& sig) -> double {
        const auto k = std::make_pair(sat, sig);
        const auto it = lam_cache.find(k);
        if (it != lam_cache.end()) return it->second;
        const System sys = systemOfSat(sat);
        const auto s = SignalId::fromObsCode(sys, sig);
        const double v = s ? wavelengthM(bandOfDigit(sys, s->digit), /*glo_slot=*/0) : 0.0;
        lam_cache.emplace(k, v);
        return v;
    };
    // The RINEX band digit of one of a satellite's phase signals ('\0' when the code does
    // not parse).  The band ROLES below (primary pair, census channel, EWL top band) are
    // decided on this digit; card NOM step N3 moves those decisions to the plan.
    const auto bandDigit = [](System sys, const std::string& sig) -> char {
        const auto s = SignalId::fromObsCode(sys, sig);
        return s ? s->digit : '\0';
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
                if (systemOfSat(parts->satellite) == System::GLONASS) continue;   // GLONASS excluded
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
                // Age-bias fix: push this station's estimable slant-iono values for
                // the detector's per-sample bridge de-bias.  Takes effect from the
                // next detector pass (one-epoch lag, consistent with F1); idempotent
                // within an epoch, so the RAIM re-solve can repeat it.
                if (bridge_debias_on && stations[i].mw_slip)
                    stations[i].mw_slip->setBridgeIonoEstimates(std::move(sat_iono));
            }
        }
        // Network-preferred channel per (constellation, band) (WL-3).  The
        // lexicographic tie-break alone fragments the primary pairs across stations
        // (L2L where tracked, L2W elsewhere), which splits the pair groups' graphs
        // and loses every cycle that crosses the boundary (about 1/3 of the primary
        // cycle space at 50 stations).  Under lane_carry the pair prefers the
        // constellation's majority channel per band (deterministic census over this
        // build's resolvable ambiguities; ties lexicographic), and falls back to the
        // arc's own lowest channel where the majority channel is not tracked.
        // WL-1/WL-2-only shapes keep the legacy tie-break.
        std::map<std::pair<char, char>, std::string> chan_pref;
        if (lane_on) {
            std::map<std::pair<char, std::string>, long long> chan_count;
            for (const auto& [rs, sigs] : arc_signals) {
                const char cons = letterOfSat(rs.second);
                for (const auto& [sig, col] : sigs) ++chan_count[{cons, sig}];
            }
            for (const auto& [k, cnt] : chan_count) {
                const char band = bandDigit(systemFromLetter(k.first), k.second);
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
            // Joint pair census (UWL-1).  Channel choices are correlated per
            // receiver family: Galileo splits into (L1C,L5Q) and (L1X,L5X)
            // families, so independent per-band majorities can compose a pair such
            // as (L1C,L5X) that no station tracks, in which case no pair arc forms
            // and the whole constellation stays per-band without any warning.
            // With uwl armed, each (rx,sat)'s raw two-lowest-band channel pair is
            // counted, the majority pair per constellation is elected (ties
            // lexicographic, the §8.2 deterministic rule), and the per-band
            // preferences are steered to it so preferChan and the bridge keys
            // align with it.  The minority family stays per-band here (about half
            // the Galileo pair space); uwl_family_pairs admits it into its own pair
            // group further below.  uwl_enable=false keeps the per-band census
            // unchanged (the validated WL-3 shape, byte-identical).
            if (uwl_on) {
                std::map<std::pair<char, std::pair<std::string, std::string>>,
                         long long> pair_count;
                for (auto& [rs, sigs] : arc_signals) {
                    const System sys = systemOfSat(rs.second);
                    std::sort(sigs.begin(), sigs.end(),
                              [&](const std::pair<std::string, int>& a,
                                  const std::pair<std::string, int>& b) {
                        const char ba = bandDigit(sys, a.first);
                        const char bb = bandDigit(sys, b.first);
                        if (ba != bb) return ba < bb;
                        return a.first < b.first;
                    });
                    if (sigs.empty()) continue;
                    const char cons = rinexLetter(sys);
                    // Two lowest carried bands (the uncarried top band never
                    // pairs; this is the same exclusion the routing applies).
                    std::string s1;
                    char b1 = '\0';
                    for (const auto& [sig, col] : sigs)
                        if (bandDigit(sys, sig) != '\0' && !isUncarriedThirdBand(cons, sig)) {
                            s1 = sig; b1 = bandDigit(sys, sig); break;
                        }
                    if (b1 == '\0') continue;
                    for (const auto& [sig, col] : sigs)
                        if (bandDigit(sys, sig) != '\0' && bandDigit(sys, sig) != b1
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
                    const char b1 = bandDigit(systemFromLetter(cons), pr.first);
                    const char b2 = bandDigit(systemFromLetter(cons), pr.second);
                    if (b1 == '\0' || b2 == '\0') continue;
                    chan_pref[{cons, b1}] = pr.first;
                    chan_pref[{cons, b2}] = pr.second;
                }
            }
        }
        for (auto& [rs, sigs] : arc_signals) {
            const System sys = systemOfSat(rs.second);
            std::sort(sigs.begin(), sigs.end(),
                      [&](const std::pair<std::string, int>& a,
                          const std::pair<std::string, int>& b) {
                const char ba = bandDigit(sys, a.first);
                const char bb = bandDigit(sys, b.first);
                if (ba != bb) return ba < bb;
                return a.first < b.first;
            });
            PP pp;
            if (!sigs.empty()) { pp.sig1 = sigs.front().first; pp.col1 = sigs.front().second; }
            const char b1 = bandDigit(sys, pp.sig1);
            for (const auto& [sig, col] : sigs) {
                const char b = bandDigit(sys, sig);
                if (b != '\0' && b1 != '\0' && b != b1) {
                    pp.sig2 = sig; pp.col2 = col; break;
                }
            }
            if (lane_on && !sigs.empty()) {
                const char cons = rinexLetter(sys);
                const char b2 = bandDigit(sys, pp.sig2);
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
        // Admit minority family pairs into the run-latched admissible set (v2,
        // uwl_family_pairs).  The count is over the steered primary pairs, the
        // quantity the routing below matches, and not over the raw two-lowest census:
        // L2L sorts before L2W, so every GPS dual-tracker's raw pair reads (L1C,L2L),
        // which can inflate a pair's count while routing sees a single steered arc
        // and leaves a one-arc pair group that loses the arc's per-band cycles.  The
        // majority pair routes through lane_pref_pair as in v1 (its admission here is
        // a duplicate with no effect); a minority pair is admitted at the first epoch
        // its steered count reaches the floor (nmw.uwl_family_min_arcs), then stays
        // latched (see the declaration).
        if (uwl_family_on) {
            std::map<std::pair<char, std::pair<std::string, std::string>>,
                     long long> pp_count;
            for (const auto& [rs, pp] : primary) {
                if (!pp.ok) continue;
                const char cons = letterOfSat(rs.second);
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
            // The constellation's single preferred pair: the two lowest census bands,
            // skipping the EWL third band, which stays uncarried (WL-2 §3.1).
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
                // Census log (once per run per constellation): which pair the lane
                // carries.  Fragmentation across channels is the WL-3 failure mode
                // and a Galileo pre-flight check (SPEC_uwl_card.md §8.1).
                if (lane_census_logged.insert(cons).second)
                    log->info("Network (M5/WL-3 census): constellation '{}' preferred "
                              "primary pair = ({}, {}) over census bands [{}].",
                              cons, lane_pref_pair[cons].first,
                              lane_pref_pair[cons].second,
                              std::string(bands.begin(), bands.end()));
            }
            // Align the detector-side bridge arcs with the census.  Otherwise every
            // (rx,sat) whose lowest-name channel differs from the census pair has no
            // bridge arc under the caller's key, and the WL decision path gets few
            // inputs (9 of 29 cycles decided in the measurement).  Takes effect from
            // the next detector pass; the re-keyed arcs re-warm in about 2 epochs.
            if (lane_bridge_on && !chan_pref.empty())
                for (size_t i = 0; i < stations.size(); ++i)
                    if (stations[i].mw_slip)
                        stations[i].mw_slip->setBridgePreferredChannels(chan_pref);
        }
        for (auto& [rs, sigs] : arc_signals) {
            // The top-tier partner band digit (UWL-1 §3.1).  With uwl off it is the
            // first '5' for every constellation, which is degenerate for Galileo:
            // its '5' is E5a, the primary sig2 itself, giving the (L5Q,L5Q) pair
            // with no detector arc and no warning other than the one below.  With
            // uwl armed it comes from the lane-tier table (GPS '5' reproduces the
            // legacy pick bit-exactly; Galileo '7'/'8').
            char top_digit  = '5';
            char mid_digit  = '\0';
            char top2_digit = '\0';   // increment 5: the fallback rung's digit
            const System sys = systemOfSat(rs.second);
            const char cons = rinexLetter(sys);
            if (uwl_on) {
                const auto tier = mwLaneTierOf(cons, nmw.uwl_gal_top_band,
                                               nmw.uwl_mid_enable, nmw.uwl_dual_top);
                if (!tier) {
                    if (ewl_on && sys != System::Unknown && uwl_warned_cons.insert(cons).second)
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
            } else if (ewl_on && sys != System::GPS && sys != System::Unknown
                       && uwl_warned_cons.insert(cons).second) {
                log->warn("Network (M5/UWL-1): constellation '{}' enters the EWL lane "
                          "under the LEGACY GPS band digits — its top-tier pair is "
                          "degenerate and every cycle dies silently as skip_missing_mw "
                          "(SPEC_uwl_card.md §1 fact 3).  Arm network.mw_wl.uwl_enable "
                          "for the per-constellation lane-tier table.", cons);
            }
            for (const auto& [sig, col] : sigs)
                if (bandDigit(sys, sig) == top_digit) { band_top_of[rs] = { sig, col }; break; }
            if (top2_digit != '\0')
                for (const auto& [sig, col] : sigs)
                    if (bandDigit(sys, sig) == top2_digit) { band_top2_of[rs] = { sig, col }; break; }
            if (mid_digit != '\0')
                for (const auto& [sig, col] : sigs)
                    if (bandDigit(sys, sig) == mid_digit) { band_mid_of[rs] = sig; break; }
        }
    }
    // The carry driver needs this arc's ambiguity linearization value when either half
    // of the linearization-frame carry is armed: the write half stores Σ c·x_lin per
    // chord, and the read half re-bases a restored branch onto it (RS-2,
    // SPEC_event_harness §21.4).  Nothing else reads it, so with both off the
    // per-station state copy is not taken.
    const bool carry_lin_needed = config_.getNetworkConfig().state_snapshot_carry_lin
                                  || config_.networkResumeCarryRebase();
    for (size_t i = 0; i < stations.size(); ++i) {
        if (est_index_of[i] < 0) continue;
        const auto& h2l = asm_res[i].h2local;
        const Eigen::VectorXd x_lin_st =
            carry_lin_needed ? stations[i].sm->getStateVector() : Eigen::VectorXd();
        auto linOf = [&](std::size_t state_index) -> double {
            const Eigen::Index si = static_cast<Eigen::Index>(state_index);
            return (si >= 0 && si < x_lin_st.size()) ? x_lin_st(si) : 0.0;
        };
        for (const auto& [key, sidx] : stations[i].sm->getAmbiguities()) {
            const auto parts = StateKeys::parseAmbiguityKey(key);
            if (!parts || parts->satellite.empty()) continue;
            const System sys = systemOfSat(parts->satellite);
            const char cons = rinexLetter(sys);
            if (sys == System::GLONASS) {   // GLONASS FDMA: no common-λ integer DD (spec OPEN-3)
                if (!carry_glonass_warned) {
                    log->warn("Network[Card4]: GLONASS ambiguities are EXCLUDED from the "
                              "invariant carry (FDMA — no common wavelength for an integer "
                              "DD; IE-FDMA is a v2 item).  They keep their legacy treatment.");
                    carry_glonass_warned = true;
                }
                continue;
            }
            // The satellite phase-bias key is per satellite ("<sat>|<sig>|PB"), not the
            // per-system receiver-bias key.  Only ambiguities backing an ungated shared
            // phase bias (a vertex of the eq34 graph) become DD arcs.
            // With ewl_carry_exclude, the constellation's EWL third band is kept out of
            // the carry (an uncarried lane has no stiff prior, so the EWL rows govern;
            // WL-2 §3.1).  Off: the branch is not taken.
            if (ewl_carry_exclude && isUncarriedThirdBand(cons, parts->signal)) continue;
            const std::string pbkey =
                StateKeys::makeSatPhaseBiasKey(parts->satellite, parts->signal);
            if (!shared_phase_keys.count(pbkey)) continue;   // not an eq34 arc (gated/legacy lump)
            const auto hit = h2l.find(static_cast<int>(sidx));
            if (hit == h2l.end() || hit->second < 0) continue;   // unresolvable ⇒ chord refloats
            // Slip feed (C3c): an entry in the slip funnel carry_slipped_ambs (a PASS-A
            // detection by MW wide-lane / LLI / IF-phase, recorded at the detector so the
            // PASS-B pending-reseed clear cannot hide it, or another funnel source) or a
            // within-epoch RAIM culprit (§3) invalidates this arc's carried DD.  The arc
            // is then re-tapped fresh this epoch instead of folding a stiff pre-slip prior.
            auto ambSlipped = [&](const std::string& amb_key) -> bool {
                const std::pair<int, std::string> sk{static_cast<int>(i), amb_key};
                bool funnel_spared = false;   // a funnel entry the retap-only exception spared
                if (carry_slipped_ambs.count(sk)) {
                    // Retap-only exception (D5 v4, network.carry_invariant_innov_retap_only).
                    // A funnel entry made by a D5 innovation flag, on an arc whose raw
                    // observables were measured as continuous, has no integer jump behind
                    // it, and the flag has already had its effect: installPosterior
                    // re-tapped the chord in-epoch at the previous epoch (mean ẑ+v,
                    // variance σ̂₀²·Cov(v)_ii, cross terms zeroed).  Excluding the arc from
                    // A_old here would discard that reset and refloat the DD from a diffuse
                    // prior.  Under a stable tree a chord arc lies in exactly one
                    // fundamental cycle, its own (GATE 8), so the exclusion protects no
                    // other carried DD; on this population it only shortens chord
                    // lifetimes and inflates dd_sigma_cyc.
                    //
                    // The refloat is kept for real slips (GATE 10b measures it completing
                    // a real slip's reset).  Membership in the flat set therefore requires
                    // positive evidence of continuity (measured and flat at flag time), and
                    // this test also re-checks the current epoch's raw funnel: a detector
                    // that fired since still excludes the arc.  Stepped and unmeasurable
                    // arcs are never in the set.  Standing invariant #1.
                    //
                    // The funnel entry itself is unchanged either way: the Iono-2 v3k erase
                    // classifier, the WL-3 MW-row slip-skip, anchor hygiene, the PB emit
                    // filter and card G all read the same union as before.
                    if (!(retap_only && ctx.carry.carry_innov_flat_now.count(sk)
                          && !carry_slipped_raw.count(sk)))
                        return true;
                    funnel_spared = true;
                }
                if (we_culprits.names(static_cast<int>(i), parts->satellite)) {
                    culprit_marked.insert(sk);
                    return true;
                }
                // The suppressed kill is counted only where the arc survives: on a T2
                // re-solve a culprit kills a spared arc anyway (card WCF), and that arc is
                // counted in we_culprit_withhold.ambs, not here.
                if (funnel_spared) ++ctx.carry.carry_innov_kill_suppressed;
                return false;
            };
            int& sv = carry_sat_vertex[parts->satellite];
            if (sv == 0) sv = kSatBase + static_cast<int>(carry_sat_vertex.size());

            // --- WL-3 pair routing (SPEC_lane_carry_card.md §2): a primary-pair
            // ambiguity whose partner band also passes every per-band test (eq34-shared
            // pbkey and resolvable column) joins the constellation's pair group (emitted
            // once, from the band-1 iteration, with both columns); otherwise it falls
            // through to the residual per-band group.
            if (lane_on) {
                const auto pit = primary.find({ static_cast<int>(i), parts->satellite });
                const PP* pp = (pit != primary.end() && pit->second.ok)
                                   ? &pit->second : nullptr;
                // Only the constellation's census-preferred pair joins the pair group.
                // A minority-channel (rx,sat) keeps the per-band carry (its cycles
                // survive per band instead of forming a small pair group that appears
                // and disappears), and the EWL third band never pairs.
                // With uwl_family_pairs (v2), a latched minority-family pair also routes
                // into its own pair group (the group key is the pair string, so
                // families stay disjoint graphs).
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
                        // Per-band slip states.  The driver unions them under the lane
                        // shape (§3.4: either band refloats both) and keeps them per
                        // band under the §6.1 equivalence shape (the same behavior as
                        // the two per-band groups).
                        a.slipped     = ambSlipped(key);
                        a.slipped2    = ambSlipped(key2);
                        if (carry_lin_needed) {   // RS-2: both bands' linearization values
                            a.x_lin = linOf(sidx);
                            const auto& ambs2 = stations[i].sm->getAmbiguities();
                            const auto a2 = ambs2.find(key2);
                            if (a2 != ambs2.end()) a.x_lin2 = linOf(a2->second);
                        }
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
            if (carry_lin_needed) a.x_lin = linOf(sidx);   // RS-2
            groups[gk].key = gk;
            groups[gk].arcs.push_back(a);
            // Record arcs for the innovation-flag reverse lookup (D5).  Only chords can be
            // flagged, since tree edges carry no DD; tree edges are recorded too, which
            // has no effect.
            if (carry_innov_gate)
                carry_arc_amb_key[gk][ArcKey(static_cast<int>(i), sv)] = key;
        }
    }
    if (culprits_read)   // card WCF: the reader's ambiguity count, once per ambiguity
        ctx.counters.we_culprit_withhold.ambs += static_cast<long long>(culprit_marked.size());
    std::vector<NetworkInvariantCarryDriver::GroupInput> glist;
    glist.reserve(groups.size());
    for (auto& [k, g] : groups) glist.push_back(std::move(g));
    auto blocks = carry_driver.buildDdPriors(glist);
    // Group census (UWL-1; once per run, log only): which carry groups exist and which
    // are pair groups, the WL-3 fragmentation pre-flight check (SPEC §8.1).
    if (uwl_on && lane_census_logged.insert('#').second)
        for (const auto& [gk, gid] : carry_group_id) {
            std::size_t n_arcs = 0; bool is_pair = false;
            for (const auto& gi : glist)
                if (gi.key == gid) { n_arcs = gi.arcs.size(); is_pair = gi.pair; break; }
            log->info("Network (M5/UWL-1 group census): group {} = cons '{}' '{}'"
                      "{} ({} arc(s) this epoch).",
                      gid, gk.first, gk.second, is_pair ? " [PAIR]" : "", n_arcs);
        }

    // --- The lane step (WL-2 EWL decision, WL-3 lane increments, the WL hold rows). ---
    // It uses the carry's chord basis: each carry block row is a fundamental cycle on
    // one band's ambiguity columns.  Its hold rows are appended after the carry blocks so
    // their indices [0,Nc) stay valid for installPosterior (which reads only its own Nc
    // blocks).
    if (mw_mach_on)
        networkMwWlInject(
            ctx, config_, log, traces, carry_driver, stations,
            se, carry_slipped_ambs, we_culprits, n_threads, ewl_lanes,
            ewl_cfg, wl_inject_epoch, lane_trace_epoch,
            wl_epoch, wl_cons_epoch, revmap, lin_of, iono_of, primary,
            band_top_of, band_top2_of, band_mid_of, lam, blocks,
            ewl_on, uwl_on, uwl_cold_on, uwl_mid_on, uwl_dual_on, uwl_sib_gate_on,
            wl_inject_on, bridge_debias_on, lane_on, lane_meas_on, lane_bridge_on, lane_fix_on,
            lane_parallel_components, mw_d5_dump, mw_lane_trace, mw_tagsets);
    // Under fold_blocked the lane primitives skip their per-row re-symmetrize, so exact
    // symmetry is restored here once per joint block, after all of this epoch's MW /
    // bridge / fix rows are applied (no-op unless fold_blocked; LF-1 INC-1).  Runs on
    // every build including the RAIM re-solve, because the blocks are rebuilt each pass.
    carry_driver.finalizeLaneBatch(blocks);
    est.contributionKind(ContributionKind::DdPriorBlock);   // card NOM N2a
    est.setDdPriors(std::move(blocks));
}

} // namespace GnssProcessor
