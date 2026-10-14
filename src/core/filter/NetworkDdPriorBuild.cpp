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
    const bool carry_innov_gate = ctx.knobs.carry_innov_gate;
    const bool ewl_on = ctx.knobs.ewl_on;
    const bool wl_inject_on = ctx.knobs.wl_inject_on;
    const bool bridge_debias_on = ctx.knobs.bridge_debias_on;
    const bool ewl_carry_exclude = ctx.knobs.ewl_on;
    const bool mw_d5_dump = ctx.knobs.mw_d5_dump;
    const bool mw_lane_trace = ctx.knobs.mw_lane_trace;
    const bool mw_tagsets = ctx.knobs.mw_tagsets;
    auto& carry_glonass_warned = ctx.carry.carry_glonass_warned;
    auto& carry_group_id = ctx.carry.carry_group_id;
    const bool carry_invariant = ctx.knobs.carry_invariant;
    auto& carry_sat_vertex = ctx.carry.carry_sat_vertex;
    auto& d5_ewl_stasat = ctx.carry.d5_ewl_stasat;
    auto& d5_hold_chords = ctx.carry.d5_hold_chords;
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
    if (carry_innov_gate) carry_arc_amb_key.clear();   // rebuilt with this call's arcs (D5)

    // --- Shared WL-2 machinery (revmap / arc_signals / lin_of / iono_of / primary /
    // band_top_of / λ cache).  Byte-identical when every lane is off: nothing here is
    // populated, and building it has no effect on the solve (it only reads station state).
    const bool mw_mach_on = ewl_on;
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
    // extra-wide lane (WL-2 EWL).  The top digit is '5' (GPS L5 / Galileo E5a; see the
    // top_digit loop below).  arc_signals is sorted by band digit then name, so the
    // first match is deterministic.
    std::map<std::pair<int, std::string>, std::pair<std::string, int>> band_top_of;
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
            if (wl_inject_on) {
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
            pp.ok = (!pp.sig1.empty() && !pp.sig2.empty());
            primary[rs] = pp;
        }
        for (auto& [rs, sigs] : arc_signals) {
            // The top-tier partner band digit: the first '5' for every constellation,
            // which is degenerate for Galileo: its '5' is E5a, the primary sig2 itself,
            // giving the (L5Q,L5Q) pair with no detector arc and no warning other than
            // the one below.
            const char top_digit = '5';
            const System sys = systemOfSat(rs.second);
            const char cons = rinexLetter(sys);
            if (ewl_on && sys != System::GPS && sys != System::Unknown
                && uwl_warned_cons.insert(cons).second) {
                log->warn("Network (M5/WL-2): constellation '{}' enters the EWL lane "
                          "under the GPS band digits: its top-tier pair is "
                          "degenerate and every cycle dies silently as skip_missing_mw "
                          "(SPEC_uwl_card.md §1 fact 3).", cons);
            }
            for (const auto& [sig, col] : sigs)
                if (bandDigit(sys, sig) == top_digit) { band_top_of[rs] = { sig, col }; break; }
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
                    // classifier, anchor hygiene, the PB emit filter and card G all read the
                    // same union as before.
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

    // --- The lane step (WL-2 EWL decision and the WL hold rows). ---
    // It uses the carry's chord basis: each carry block row is a fundamental cycle on
    // one band's ambiguity columns.  Its hold rows are appended after the carry blocks so
    // their indices [0,Nc) stay valid for installPosterior (which reads only its own Nc
    // blocks).
    if (mw_mach_on)
        networkMwWlInject(
            ctx, config_, log, traces, carry_driver, stations,
            se, carry_slipped_ambs, we_culprits, n_threads, ewl_lanes,
            ewl_cfg, wl_inject_epoch, lane_trace_epoch,
            wl_epoch, revmap, lin_of, iono_of, primary,
            band_top_of, lam, blocks, ewl_on,
            wl_inject_on, bridge_debias_on, mw_d5_dump, mw_lane_trace, mw_tagsets);
    est.contributionKind(ContributionKind::DdPriorBlock);   // card NOM N2a
    est.setDdPriors(std::move(blocks));
}

} // namespace GnssProcessor
