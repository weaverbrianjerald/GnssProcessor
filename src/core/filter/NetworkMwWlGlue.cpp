/**
 * @file NetworkMwWlGlue.cpp
 * @brief Body of networkMwWlInject: the second `if (mw_mach_on)` block of
 *        networkBuildCarryDdPriors, relocated with a 4-column dedent and no substitutions
 *        (decomposition INC-13).  The alias preamble copies the run-TU and caller aliases
 *        the body reads; the `if (mw_mach_on)` guard stays at the call site.  See
 *        NetworkMwWlGlue.hpp for the concern boundary.
 */
#include "filter/NetworkMwWlGlue.hpp"

#include "app/NetworkRunContext.hpp"          // ctx.carry / ctx.counters
#include "common/GnssIdentity.hpp"            // systemOfSat / SignalId (band digit)
#include "config/GnssConfig.hpp"              // config_.getNetworkConfig().mw_wl
#include "filter/StateKeySchema.hpp"          // StateKeys::makeAmbiguityKey
#include "logging/NetworkRunTraces.hpp"       // traces.trace
#include "slip/MwLaneTiers.hpp"               // mwLaneTierOf / MwLaneTier
#include "slip/MwSlipDetector.hpp"            // MwSlipDetector::MwArcZd* / MwBridgeMean

#include <Eigen/Dense>

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>

namespace GnssProcessor {

void networkMwWlInject(
    NetworkRunContext& ctx,
    const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkRunTraces& traces,
    NetworkInvariantCarryDriver& carry_driver,
    const std::vector<NetStation>& stations,
    const SyncedEpoch& /*se*/,
    std::set<std::pair<int, std::string>>& carry_slipped_ambs,
    const WithinEpochCulprits& we_culprits,
    int n_threads,
    std::vector<EwlLane>& ewl_lanes,
    const NetworkMwWlLayer::Config& ewl_cfg,
    std::vector<WlInjectPlan>& wl_inject_epoch,
    std::vector<LaneTraceEntry>& lane_trace_epoch,
    WlCounters& wl_epoch,
    std::map<char, WlCounters>& wl_cons_epoch,
    const std::map<std::pair<int, int>, Rev>& revmap,
    const std::map<std::pair<int, int>, double>& lin_of,
    const std::map<std::pair<int, std::string>, double>& iono_of,
    const std::map<std::pair<int, std::string>, PP>& primary,
    const std::map<std::pair<int, std::string>, std::pair<std::string, int>>& band_top_of,
    const std::map<std::pair<int, std::string>, std::pair<std::string, int>>& band_top2_of,
    const std::map<std::pair<int, std::string>, std::string>& band_mid_of,
    const std::function<double(const std::string&, const std::string&)>& lam,
    std::vector<NetworkNormalEquations::DdPriorBlock>& blocks,
    bool ewl_on,
    bool uwl_on,
    bool uwl_cold_on,
    bool uwl_mid_on,
    bool uwl_dual_on,
    bool uwl_sib_gate_on,
    bool wl_inject_on,
    bool bridge_debias_on,
    bool lane_on,
    bool lane_meas_on,
    bool lane_bridge_on,
    bool lane_fix_on,
    bool lane_parallel_components,
    bool mw_d5_dump,
    bool mw_lane_trace,
    bool mw_tagsets) {
    // --- Alias preamble: byte-copies of the run-TU / networkBuildCarryDdPriors
    //     aliases the relocated WL/EWL/UWL inject body reads. ----
    const auto& nmw = config_.getNetworkConfig().mw_wl;
    auto& trace = traces.trace;
    auto& carry_group_id = ctx.carry.carry_group_id;
    auto& carry_sat_vertex = ctx.carry.carry_sat_vertex;
    auto& d5_ewl_stasat = ctx.carry.d5_ewl_stasat;
    auto& d5_hold_chords = ctx.carry.d5_hold_chords;

    // The lanes read the pure carry blocks, not each other's rows.  The EWL lane folds no
    // row (it decides, FFR-fixes and exposes its integers only); the WL bridge lane's
    // integer-hard hold rows on the (1,2) columns are collected here and appended last.
    std::vector<NetworkNormalEquations::DdPriorBlock> wl_hold_out;
    std::vector<NetworkMwWlLayer::FixedEwlRow> ewl_fixed;   // bridge input (WL-2 §3.4)
    // Per-entry top-rung tag, parallel to ewl_fixed: '\0' on the legacy single lane, the
    // lane's b_top or b_top2 digit under the UWL card (Increment 5).
    std::vector<char> ewl_fixed_top;

  if (ewl_on) {   // --- EWL lane (WL-2): (band2, band-top) pair, fix gated on the FFR statistic ---
    for (auto& lane : ewl_lanes) lane.epoch = NetworkMwWlLayer::Counters{};
    // The eligible cycle is a primary-sig1 cycle, and the row is the top-tier DD
    // on the same (rx,sat) arcs, pairing the estimator's primary sig2 (channel
    // discipline) with the constellation's top band: GPS EWL (L2,L5); Galileo UWL
    // (E5b|E5, E5a) under the UWL card.  (hi,lo) is ordered by actual frequency (GPS's
    // shared band is the pair's high side, Galileo's its low side), so the MW statistic
    // keys (sat, hi, lo) and λ_top stays positive (UWL-1 §2).  The top band is not
    // carried (the census block excludes it), so no carried column competes with these rows.
    // The lane instance under construction selects the top-rung map its resolver reads:
    // band_top_of for the preferred instance and every single-top instance, band_top2_of
    // for the fallback instance.  It is set per lane in the build loop below; single-top
    // shapes never repoint it (Increment 5).
    const auto* ewl_top_map = &band_top_of;
    auto ewl_resolver = [&](std::size_t est_station, Eigen::Index local_col,
                            NetworkMwWlLayer::ArcData& out) -> bool {
        const auto rit = revmap.find(
            { static_cast<int>(est_station), static_cast<int>(local_col) });
        if (rit == revmap.end()) return false;
        const Rev& rv = rit->second;
        out.cons = letterOfSat(rv.sat);
        const auto pit = primary.find({ rv.rx, rv.sat });
        const PP* pp = (pit != primary.end()) ? &pit->second : nullptr;
        if (pp && pp->ok && rv.signal == pp->sig1
            && static_cast<int>(local_col) == pp->col1) {
            out.is_primary_sig1 = true;
            const auto b5 = ewl_top_map->find({ rv.rx, rv.sat });
            if (b5 == ewl_top_map->end()) { out.col2 = -1; out.have_mw = false; return true; }
            const std::string& sig_m = pp->sig2;           // shared band = primary sig2
            const int          col_m = pp->col2;
            const std::string& sig_t = b5->second.first;   // top-tier partner band
            const int          col_t = b5->second.second;
            const double lam_m = lam(rv.sat, sig_m);
            const double lam_t = lam(rv.sat, sig_t);
            // (hi, lo) by actual frequency (smaller λ = higher f).  GPS: L2 > L5
            // reproduces the legacy assignment bit-exactly.
            const bool m_hi = lam_m > 0.0 && lam_t > 0.0 && lam_m < lam_t;
            const std::string& sig_hi = m_hi ? sig_m : sig_t;
            const std::string& sig_lo = m_hi ? sig_t : sig_m;
            const int col_hi = m_hi ? col_m : col_t;
            const int col_lo = m_hi ? col_t : col_m;
            out.col1    = col_hi;                          // high-frequency side
            out.col2    = col_lo;                          // low-frequency side
            out.lambda1 = m_hi ? lam_m : lam_t;
            out.lambda2 = m_hi ? lam_t : lam_m;
            MwSlipDetector::MwArcZdMean zd;
            if (stations[rv.rx].mw_slip)                    // (higher, lower)-freq order
                zd = stations[rv.rx].mw_slip->arcZdMean(rv.sat, sig_hi, sig_lo);
            out.have_mw       = zd.valid;
            out.mw_cold       = false;
            out.mw_zd_mean_m  = zd.mean_m;
            out.mw_var_m2     = zd.var_of_mean_m2;
            out.mw_count      = static_cast<long long>(zd.count);
            out.lambda_wl     = zd.lambda_wl;              // = λ_top = c/(f_hi − f_lo)
            out.bia_corrected = zd.bia_corrected;
            // Cold fix (UWL-1 §3.3): when the lagged statistic is immature, serve the
            // unlagged n≥1 cold read.  It feeds the decision only, and only on tiers with
            // λ ≥ uwl_cold_min_lambda_m (the floor excludes the 5.861 m GPS EWL).  At n=1
            // the Welford variance does not exist; the a-priori per-sample σ
            // (uwl_cold_sigma_m) is substituted.
            if (uwl_cold_on && stations[rv.rx].mw_slip
                && (!zd.valid
                    || zd.count < static_cast<std::size_t>(ewl_cfg.min_arc_epochs))) {
                const auto zc = stations[rv.rx].mw_slip->arcZdMeanCold(
                                    rv.sat, sig_hi, sig_lo);
                if (zc.valid && zc.lambda_wl >= nmw.uwl_cold_min_lambda_m) {
                    out.have_mw       = true;
                    out.mw_cold       = true;
                    out.mw_zd_mean_m  = zc.mean_m;
                    out.mw_var_m2     = std::isfinite(zc.var_of_mean_m2)
                                            ? zc.var_of_mean_m2
                                            : nmw.uwl_cold_sigma_m * nmw.uwl_cold_sigma_m;
                    out.mw_count      = static_cast<long long>(zc.count);
                    out.lambda_wl     = zc.lambda_wl;
                    out.bia_corrected = zc.bia_corrected;
                }
            }
            const auto lmit = lin_of.find({ static_cast<int>(est_station), col_hi });
            const auto ltit = lin_of.find({ static_cast<int>(est_station), col_lo });
            if (lmit == lin_of.end() || ltit == lin_of.end()) out.have_mw = false;
            else { out.x_lin1 = lmit->second; out.x_lin2 = ltit->second; }
        } else {
            const auto sig = SignalId::fromObsCode(systemOfSat(rv.sat), rv.signal);
            out.is_primary_sig1 = (!pp || !pp->ok) && (sig && sig->digit == '1');
            out.col1    = local_col;
            out.col2    = -1;
            out.have_mw = false;
        }
        return true;
    };
    ewl_fixed.clear();
    ewl_fixed_top.clear();
    for (auto& lane : ewl_lanes) {
        // The fallback instance ('7' under a dual tier) resolves against band_top2_of;
        // every other instance keeps band_top_of.  The tag travels with each fixed row
        // so the WL walk reads the bridge and cold arcs of the rung the decision came
        // from (Increment 5).
        const bool is_top2 = uwl_dual_on && lane.top != '\0'
            && [&] {
                   const auto t = mwLaneTierOf(lane.cons, nmw.uwl_gal_top_band,
                                               nmw.uwl_mid_enable, nmw.uwl_dual_top);
                   return t && t->b_top2 != '\0' && lane.top == t->b_top2;
               }();
        ewl_top_map = is_top2 ? &band_top2_of : &band_top_of;
        std::vector<NetworkMwWlLayer::FixedEwlRow> lane_fixed;
        // The returned row blocks are not folded: the lane decides, fixes and exposes its
        // integers (lane_fixed) and its counters only.
        (void)lane.layer.build(blocks, ewl_resolver, lane.epoch, &lane_fixed);
        ewl_fixed.insert(ewl_fixed.end(), lane_fixed.begin(), lane_fixed.end());
        ewl_fixed_top.insert(ewl_fixed_top.end(), lane_fixed.size(), lane.top);
    }
    ewl_top_map = &band_top_of;   // leave the pointer in its legacy state
    if (mw_d5_dump) {   // F-A dump: the (station,sat) pairs touched by EWL-fixed cycles
        d5_ewl_stasat.clear();
        for (const auto& fx : ewl_fixed)
            for (const auto& t : blocks[fx.block].rows[fx.row]) {
                const auto rit = revmap.find({ static_cast<int>(t.station),
                                               static_cast<int>(t.local_col) });
                if (rit != revmap.end())
                    d5_ewl_stasat.insert({ rit->second.rx, rit->second.sat });
            }
    }
  }  // --- end WL-2 EWL lane ---

  // Per-build reset (the last build before the accepted solve wins; RAIM re-solve safe).
  if (wl_inject_on || lane_on) {
      wl_inject_epoch.clear();
      wl_epoch = WlCounters{};
      wl_cons_epoch.clear();   // per-constellation walk tallies (UWL-1)
  }
  if (mw_tagsets) d5_hold_chords.clear();

  if (lane_meas_on) {
    // --- Per-epoch MW(1,2) lane increments on every eligible pair chord (WL-3 §3.2);
    // the carried block becomes the code-coherence accumulator.  Each arc contributes
    // its detector-lagged raw sample exactly once (the Welford mean stays on the
    // decision side).  A cycle is skipped for the epoch when an arc is immature, stale
    // or of mixed frame, slipped or was dropped this epoch (its lagged sample predates
    // the slip), or has a quarantined linearization.  Chords run in (block, chord)
    // order, the §8.2 deterministic seam, before the bridge and fix walk below.
    constexpr double kLaneMwSigmaFloorCyc = 0.2;   // young-arc Welford collapse guard
    // Each joint block's MW folds are independent (no shared Pᶻ) and stay in order
    // within the block, so fanning out over blocks is byte-identical to the serial loop:
    // the per-block counter deltas sum independently of order, and Eigen is pinned to one
    // thread inside the region (the pcarry un-pin stays outside it, which avoids the
    // nested parallelism seen on the dev300s tier).  The per-op driver timer is disabled
    // for the parallel pass because concurrent updates would race on a single double;
    // the app-side row counts stay exact through the per-block deltas (LF-1 INC-4 §3.5)
    const long long mw_nb = static_cast<long long>(carry_driver.numBlocks());
    std::vector<WlCounters> mw_pb(static_cast<std::size_t>(std::max<long long>(0, mw_nb)));
    const bool mw_par        = lane_parallel_components && n_threads > 1;
    const int  mw_eigen_save = Eigen::nbThreads();
    const bool mw_perf_save  = carry_driver.perfTiming();
    if (mw_par) { Eigen::setNbThreads(1); carry_driver.setPerfTiming(false); }
    #pragma omp parallel for schedule(dynamic) num_threads(n_threads) if(mw_par)
    for (long long bi = 0; bi < mw_nb; ++bi) {
        const std::size_t b = static_cast<std::size_t>(bi);
        if (carry_driver.blockPairBand(b) != 3) continue;
        WlCounters& mwc = mw_pb[b];   // this block's counter delta (merged serially below)
        const auto& chords_b = carry_driver.blockChordsForTest(b);
        for (std::size_t k = 0; k < chords_b.size(); ++k) {
            if (k >= blocks[b].rows.size()) break;
            const auto& terms = blocks[b].rows[k];   // band-1 cycle terms
            if (terms.empty()) continue;
            // A lane pinned by a committed fix (σ=0.01) barely moves under an MW increment
            // (σ≈0.06-0.2), so the O(K²) downdate is skipped.  Never taken with
            // skip_pinned_increments off (LF-1 INC-2).
            if (carry_driver.laneIncrementSkippable(b, k)) {
                ++mwc.lane_pinned_skips; continue;
            }
            double z_mw_m = 0.0, R_m2 = 0.0, z_lin_wl = 0.0, lam_wl = 0.0;
            double stamp = 0.0;
            bool   have_stamp = false, ok = true;
            int    frame = -1;   // uniform bia_corrected frame guard
            for (const auto& t : terms) {
                const auto rit = revmap.find({ static_cast<int>(t.station),
                                               static_cast<int>(t.local_col) });
                if (rit == revmap.end()) { ok = false; break; }
                const Rev& rv = rit->second;
                const auto pit = primary.find({ rv.rx, rv.sat });
                if (pit == primary.end() || !pit->second.ok
                    || rv.signal != pit->second.sig1
                    || static_cast<int>(t.local_col) != pit->second.col1) {
                    ok = false; break;
                }
                const PP& pp = pit->second;
                // A slip or a within-epoch RAIM drop re-tapped this chord this epoch; the
                // lagged sample predates it and would corrupt the fresh lane, so the
                // chord is skipped for one epoch.
                bool slipped = carry_slipped_ambs.count(
                                   { rv.rx, StateKeys::makeAmbiguityKey(rv.sat, pp.sig1) })
                            || carry_slipped_ambs.count(
                                   { rv.rx, StateKeys::makeAmbiguityKey(rv.sat, pp.sig2) });
                if (!slipped && we_culprits.names(rv.rx, rv.sat)) slipped = true;
                if (slipped) { ok = false; break; }
                MwSlipDetector::MwArcZdLast zl;
                if (stations[rv.rx].mw_slip)
                    zl = stations[rv.rx].mw_slip->arcZdLast(rv.sat, pp.sig1, pp.sig2);
                if (!zl.valid || !(zl.lambda_wl > 0.0)) { ok = false; break; }
                if (frame < 0) frame = zl.bia_corrected ? 1 : 0;
                else if (frame != (zl.bia_corrected ? 1 : 0)) { ok = false; break; }
                if (!have_stamp) { stamp = zl.epoch_s; have_stamp = true; }
                else if (std::abs(zl.epoch_s - stamp) > 0.1) { ok = false; break; }
                const auto l1it = lin_of.find({ static_cast<int>(t.station), pp.col1 });
                const auto l2it = lin_of.find({ static_cast<int>(t.station), pp.col2 });
                if (l1it == lin_of.end() || l2it == lin_of.end()) { ok = false; break; }
                if (nmw.lane_max_abs_xlin_m > 0.0 &&
                    (std::abs(l1it->second) > nmw.lane_max_abs_xlin_m ||
                     std::abs(l2it->second) > nmw.lane_max_abs_xlin_m)) { ok = false; break; }
                const double la1 = lam(rv.sat, pp.sig1), la2 = lam(rv.sat, pp.sig2);
                if (!(la1 > 0.0 && la2 > 0.0)) { ok = false; break; }
                z_mw_m   += t.coeff * zl.value_m;
                R_m2     += t.coeff * t.coeff * zl.var_m2;
                z_lin_wl += (t.coeff / la1) * l1it->second
                          - (t.coeff / la2) * l2it->second;
                lam_wl = zl.lambda_wl;
            }
            if (!ok || !(lam_wl > 0.0)) { ++mwc.lane_skip_meas; continue; }
            const double y = z_mw_m / lam_wl - z_lin_wl;
            const double R = std::max(R_m2 / (lam_wl * lam_wl),
                                      kLaneMwSigmaFloorCyc * kLaneMwSigmaFloorCyc);
            if (carry_driver.applyLaneMeasurement(blocks, b, k, y, R))
                ++mwc.lane_mw_rows;
            else
                ++mwc.lane_skip_meas;
        }
    }
    if (mw_par) { Eigen::setNbThreads(mw_eigen_save); carry_driver.setPerfTiming(mw_perf_save); }
    for (const auto& d : mw_pb) {   // deterministic serial merge (order-independent sums)
        wl_epoch.lane_pinned_skips += d.lane_pinned_skips;
        wl_epoch.lane_skip_meas    += d.lane_skip_meas;
        wl_epoch.lane_mw_rows      += d.lane_mw_rows;
    }
  }

  if ((wl_inject_on || lane_fix_on || lane_bridge_on) && !ewl_fixed.empty()) {
    // --- WL lane (WL-2): phase bridge → WL integer → hold row and inject plan (§3.3/§3.4) ---
    // For each FFR-fixed EWL cycle, bridge phase-only from the fixed N_EWL to a fast WL
    // integer w = [Σc·GF_b + λ_EWL·N_EWL]/λ_WL, cross-check it against the plain MW(1,2),
    // and either fold an integer-hard hold row (the carry already agrees with the lane,
    // so the innovation is about 0) or stash an inject (the carry moves to w post-solve,
    // NL preserved).  With lane_fix_on, the lane fix below replaces both.
    // sat_of_vertex maps a chord's ArcKey.v back to its satellite label (the reverse of
    // carry_sat_vertex) for the primary-pair and group-id lookups.
    std::map<int, std::string> sat_of_vertex;
    for (const auto& [s, v] : carry_sat_vertex) sat_of_vertex[v] = s;
    // Group hold rows by their source carry block so shared (1,2) columns fold jointly.
    std::map<std::size_t, std::size_t> hold_block_of;   // carry block b → wl_hold_out index
    // Dual top tier (uwl_dual_top, Increment 5): a (block,row) decided by the preferred
    // rung is consumed, and the fallback rung's entry for the same row is deduplicated
    // (the preferred instance's entries precede the fallback's in ewl_fixed by
    // construction).  A preferred entry that fails a gate does not consume the row, so
    // the fallback proceeds in the same epoch; a block by the sibling gate therefore
    // demotes the cycle at no cost.  The lane bridge increment is separately limited to
    // once per row: both rungs' bridges measure the same lane and must not both fold.
    std::set<std::pair<std::size_t, std::size_t>> walk_decided, lane_incr_done;

    for (std::size_t fxi = 0; fxi < ewl_fixed.size(); ++fxi) {
        const auto& fx = ewl_fixed[fxi];
        const char  fx_top = fxi < ewl_fixed_top.size() ? ewl_fixed_top[fxi] : '\0';
        if (fx.block >= blocks.size() || fx.block >= carry_driver.numBlocks()) {
            ++wl_epoch.rows_seen; continue;   // defensive; stays pooled
        }
        const auto& chords_b = carry_driver.blockChordsForTest(fx.block);
        if (fx.row >= chords_b.size() || fx.row >= blocks[fx.block].rows.size()) {
            ++wl_epoch.rows_seen; continue;   // defensive; stays pooled
        }
        const ArcKey chord = chords_b[fx.row];
        // Resolve the chord's constellation up front: under the UWL card the walk tallies
        // per constellation; otherwise wc is wl_epoch, bit-identical (UWL-1).
        const auto svit0 = sat_of_vertex.find(chord.v);
        const std::string sat_lbl =
            svit0 != sat_of_vertex.end() ? svit0->second : std::string{};
        const char ccons = letterOfSat(sat_lbl);
        WlCounters& wc = uwl_on ? wl_cons_epoch[ccons] : wl_epoch;
        ++wc.rows_seen;
        // Resolve the tier of the chord's constellation once: which rung this entry came
        // from, which top map its bridge reads, and whether an untaken sibling exists for
        // the closure statistic (Increment 5).
        const auto ctier = uwl_on
            ? mwLaneTierOf(ccons, nmw.uwl_gal_top_band, nmw.uwl_mid_enable,
                           nmw.uwl_dual_top)
            : std::optional<MwLaneTier>{};
        const bool fx_is_top2 = ctier && ctier->b_top2 != '\0'
                                && fx_top == ctier->b_top2;
        const bool dual_cons  = uwl_dual_on && ctier && ctier->b_top2 != '\0';
        const auto& walk_top_map = fx_is_top2 ? band_top2_of : band_top_of;
        const auto& sib_top_map  = fx_is_top2 ? band_top_of : band_top2_of;
        if (dual_cons && walk_decided.count({ fx.block, fx.row })) {
            ++wc.dual_dedupe;   // the preferred rung already decided this row
            continue;
        }
        const auto&  terms = blocks[fx.block].rows[fx.row];   // the fundamental cycle
        if (terms.empty()) continue;

        // Walk the cycle arcs: accumulate the bridge DD, the MW(1,2) closure DD, and
        // the per-band linearization DD (F5).  Bail on the first ineligible arc.
        double sum_gf = 0.0, R_bridge = 0.0;          // Σc·GF_b (m), Σc²·var/λ_wl² (cyc²)
        double z_mw12 = 0.0, R_mw12 = 0.0;            // plain MW(1,2) DD (cyc) + var
        double z_lin1 = 0.0, z_lin2 = 0.0;            // Σc·x_lin per band (m)
        double iono_dd = 0.0, iono_co = 0.0;         // de-bias (§3.4 v2): Σc·Î₁ (m), C_iono
        double lam_wl = 0.0, lam_ewl = 0.0, lam1 = 0.0, lam2 = 0.0;
        bool   ok = true, mw12_ok = true, quarantined = false, iono_ok = true;
        // Mid hop through E6 (uwl_mid_enable): accumulators for hop-hi (mid lane against
        // top lane) and hop-lo (WL lane against mid lane), plus the MW(6,5) closure for
        // the intermediate integer.  mid_ok stays true only if every arc serves both hop
        // bridges mature; otherwise the cycle falls back to the direct hop.
        bool   mid_ok = uwl_mid_on;
        double sum_gf_hi = 0.0, R_hi = 0.0;           // Σc·GF_hop-hi (m), Σc²·var/λ_mid² (cyc²)
        double sum_gf_lo = 0.0, R_lo = 0.0;           // Σc·GF_hop-lo (m), Σc²·var/λ_wl² (cyc²)
        double iono_co_hi = 0.0, iono_co_lo = 0.0, lam_mid = 0.0;
        double z_mw65 = 0.0, R_mw65 = 0.0;            // MW(mid,b2) closure DD (mid cyc) + var
        bool   mw65_ok = true;
        // Sibling closure (Increment 5): the MW statistic of the untaken {7,8} rung as a
        // DD over the same cycle.  It is measured on every dual-tier decision; the hard
        // gate is separate (uwl_sib_gate).  An arc without the sibling band or a valid MW
        // arc drops the check (counted in sib_unavail).
        bool   sib_ok = dual_cons;
        double z_sib = 0.0, R_sib = 0.0, lam_sib = 0.0;
        // Bridge lane increment of WL-3 §3.2: the lagged raw GF_b DD (used once, phase
        // precision), accumulated alongside the Welford decision statistic.
        double sum_gf_raw = 0.0, R_gf_raw = 0.0;      // Σc·GF_b_raw (m), Σc²·var/λ_wl² (cyc²)
        double raw_stamp = 0.0;
        bool   raw_ok = lane_bridge_on, raw_have_stamp = false;
        bool   bridge_cold = false;   // any arc served by the cold bridge (UWL-1 §3.3)
        for (const auto& t : terms) {
            const auto rit = revmap.find({ static_cast<int>(t.station),
                                           static_cast<int>(t.local_col) });
            if (rit == revmap.end()) { ok = false; break; }
            const Rev& rv = rit->second;
            const auto pit = primary.find({ rv.rx, rv.sat });
            if (pit == primary.end() || !pit->second.ok) { ok = false; break; }
            const PP& pp = pit->second;
            const auto b5 = walk_top_map.find({ rv.rx, rv.sat });
            if (b5 == walk_top_map.end()) { ok = false; break; }
            auto brg = stations[rv.rx].mw_slip
                ? stations[rv.rx].mw_slip->arcBridge(rv.sat, pp.sig1, pp.sig2, b5->second.first)
                : MwSlipDetector::MwBridgeMean{};
            const std::size_t brg_need =
                static_cast<std::size_t>(std::max(2, ewl_cfg.min_arc_epochs));
            if (uwl_cold_on && stations[rv.rx].mw_slip
                && (!brg.valid || brg.count < brg_need)) {
                // After a slip the lagged bridge trails by one epoch; the unlagged cold
                // serve (n≥2) re-pins one epoch earlier.  It feeds the decision only: the
                // lane increment below stays on the lagged arcBridgeLast, and the λ floor
                // (uwl_cold_min_lambda_m) restricts it to the top tier, i.e. Galileo
                // (UWL-1 §3.3).
                const auto bc = stations[rv.rx].mw_slip->arcBridgeCold(
                                    rv.sat, pp.sig1, pp.sig2, b5->second.first);
                if (bc.valid && bc.count >= brg_need
                    && bc.lambda_ewl >= nmw.uwl_cold_min_lambda_m) {
                    brg = bc;
                    bridge_cold = true;
                }
            }
            if (!brg.valid || brg.count < brg_need) {
                ok = false; break;
            }
            const double l1 = lam(rv.sat, pp.sig1), l2 = lam(rv.sat, pp.sig2);
            if (!(l1 > 0.0 && l2 > 0.0 && brg.lambda_wl > 0.0 && brg.lambda_ewl > 0.0)) {
                ok = false; break;
            }
            const double coeff = t.coeff;
            sum_gf   += coeff * brg.gf_mean_m;
            R_bridge += coeff * coeff * brg.var_of_mean_m2 / (brg.lambda_wl * brg.lambda_wl);
            // Mid hop (E6): read this arc's two hop bridges (lagged; the cold serve is
            // reserved for the direct hop) and the MW(mid,b2) closure.  A missing or
            // immature hop bridge drops the whole cycle to the direct hop (λ_mid must be
            // uniform across the cycle's arcs); a missing closure read only clears
            // mw65_ok, which skips the closure check.
            if (mid_ok) {
                const auto b6 = band_mid_of.find({ rv.rx, rv.sat });
                MwSlipDetector::MwBridgeMean bh, blo;
                if (b6 != band_mid_of.end() && stations[rv.rx].mw_slip) {
                    bh  = stations[rv.rx].mw_slip->arcBridge(
                              rv.sat, b6->second, pp.sig2, b5->second.first);
                    blo = stations[rv.rx].mw_slip->arcBridge(
                              rv.sat, pp.sig1, pp.sig2, b6->second);
                }
                if (!bh.valid || bh.count < brg_need
                    || !blo.valid || blo.count < brg_need
                    || !(bh.lambda_wl > 0.0) || !(blo.lambda_ewl > 0.0)) {
                    mid_ok = false;
                } else {
                    sum_gf_hi += coeff * bh.gf_mean_m;
                    R_hi      += coeff * coeff * bh.var_of_mean_m2
                                 / (bh.lambda_wl * bh.lambda_wl);
                    sum_gf_lo += coeff * blo.gf_mean_m;
                    R_lo      += coeff * coeff * blo.var_of_mean_m2
                                 / (blo.lambda_wl * blo.lambda_wl);
                    iono_co_hi = bh.iono_coeff;
                    iono_co_lo = blo.iono_coeff;
                    lam_mid    = bh.lambda_wl;   // == blo.lambda_ewl (CDMA-uniform)
                    if (mw65_ok) {   // MW(mid,b2) closure for the mid integer
                        const auto mw65 = stations[rv.rx].mw_slip->arcZdMean(
                                              rv.sat, b6->second, pp.sig2);
                        if (mw65.valid && mw65.lambda_wl > 0.0) {
                            z_mw65 += coeff * mw65.mean_m / mw65.lambda_wl;
                            R_mw65 += coeff * coeff * mw65.var_of_mean_m2
                                      / (mw65.lambda_wl * mw65.lambda_wl);
                        } else {
                            mw65_ok = false;
                        }
                    }
                }
            }
            // This arc's MW read for the untaken sibling (Increment 5): a fix from the '7'
            // rung reads the '8' rung's (8,5) statistic, a fix from the '8' rung the finer
            // (7,5) one.  Ordered (hi,lo) by actual frequency like every MW key.
            if (sib_ok) {
                const auto bs = sib_top_map.find({ rv.rx, rv.sat });
                MwSlipDetector::MwArcZdMean mws;
                if (bs != sib_top_map.end() && stations[rv.rx].mw_slip) {
                    const double lam_s = lam(rv.sat, bs->second.first);
                    const double lam_m = lam(rv.sat, pp.sig2);
                    const bool s_hi = lam_s > 0.0 && lam_m > 0.0 && lam_s < lam_m;
                    mws = stations[rv.rx].mw_slip->arcZdMean(
                              rv.sat,
                              s_hi ? bs->second.first : pp.sig2,
                              s_hi ? pp.sig2 : bs->second.first);
                }
                if (mws.valid && mws.lambda_wl > 0.0) {
                    z_sib  += coeff * mws.mean_m / mws.lambda_wl;
                    R_sib  += coeff * coeff * mws.var_of_mean_m2
                              / (mws.lambda_wl * mws.lambda_wl);
                    lam_sib = mws.lambda_wl;
                } else {
                    sib_ok = false;
                }
            }
            if (raw_ok) {   // raw lagged sample (WL-3); skipped on slip, stale or immature
                bool slipped = carry_slipped_ambs.count(
                                   { rv.rx, StateKeys::makeAmbiguityKey(rv.sat, pp.sig1) })
                            || carry_slipped_ambs.count(
                                   { rv.rx, StateKeys::makeAmbiguityKey(rv.sat, pp.sig2) });
                if (!slipped && we_culprits.names(rv.rx, rv.sat)) slipped = true;
                MwSlipDetector::MwBridgeLast bl;
                if (!slipped && stations[rv.rx].mw_slip)
                    bl = stations[rv.rx].mw_slip->arcBridgeLast(
                             rv.sat, pp.sig1, pp.sig2, b5->second.first);
                if (slipped || !bl.valid || !(bl.lambda_wl > 0.0)) {
                    raw_ok = false;
                } else if (!raw_have_stamp) {
                    raw_stamp = bl.epoch_s; raw_have_stamp = true;
                } else if (std::abs(bl.epoch_s - raw_stamp) > 0.1) {
                    raw_ok = false;   // mixed epochs: the DD belongs to no single epoch
                }
                if (raw_ok) {
                    sum_gf_raw += coeff * bl.gf_m;
                    R_gf_raw   += coeff * coeff * bl.var_m2
                                  / (bl.lambda_wl * bl.lambda_wl);
                }
            }
            // Iono de-bias (§3.4 v2): subtract the DD of the network's own slant-iono
            // estimate.  The local marginal iono variance is left out of the σ guard: it
            // is dominated by the absolute (common-mode) iono uncertainty, which cancels
            // in a closed DD cycle, so Σc²·var would grossly overstate the DD-iono error
            // and gate the bridge out of its advantage (a decision by epoch ~4–6).  The
            // DD-iono estimate error, small because the common mode cancels, shows up
            // directly in |frac| and is caught by the FFR gate; the iono-free MW(1,2)
            // closure is the backstop.  The exact joint DD-iono covariance (an iono-column
            // fold) is left to a v3 refinement.
            const auto iit = iono_of.find({ rv.rx, rv.sat });
            if (iit == iono_of.end()) { iono_ok = false; break; }
            iono_dd += coeff * iit->second;
            const auto l1it = lin_of.find({ static_cast<int>(t.station), pp.col1 });
            const auto l2it = lin_of.find({ static_cast<int>(t.station), pp.col2 });
            if (l1it == lin_of.end() || l2it == lin_of.end()) { ok = false; break; }
            z_lin1 += coeff * l1it->second;
            z_lin2 += coeff * l2it->second;
            if (nmw.lane_max_abs_xlin_m > 0.0 &&
                (std::abs(l1it->second) > nmw.lane_max_abs_xlin_m ||
                 std::abs(l2it->second) > nmw.lane_max_abs_xlin_m)) { quarantined = true; break; }
            // Plain MW(1,2) closure statistic (integrity cross-check, §3.4).
            MwSlipDetector::MwArcZdMean mw = stations[rv.rx].mw_slip
                ? stations[rv.rx].mw_slip->arcZdMean(rv.sat, pp.sig1, pp.sig2)
                : MwSlipDetector::MwArcZdMean{};
            if (mw.valid && mw.lambda_wl > 0.0) {
                z_mw12 += coeff * mw.mean_m / mw.lambda_wl;
                R_mw12 += coeff * coeff * mw.var_of_mean_m2 / (mw.lambda_wl * mw.lambda_wl);
            } else {
                mw12_ok = false;
            }
            lam_wl = brg.lambda_wl; lam_ewl = brg.lambda_ewl; lam1 = l1; lam2 = l2;
            iono_co = brg.iono_coeff;
        }
        if (quarantined) { ++wc.skip_quarantine; continue; }
        if (!ok || !(lam_wl > 0.0)) { ++wc.skip_bridge_missing; continue; }
        if (!iono_ok)  { ++wc.skip_no_iono; continue; }  // no iono estimate ⇒ can't de-bias

        // --- Fold the per-epoch bridge lane increment (WL-3 §3.2): the raw lagged GF_b
        // DD, iono de-biased, with its per-sample R.  It is folded before the FFR decision
        // gates because the continuous lane tightening is wanted even on epochs where the
        // integer decision is withheld.  The raw sample enters here and nowhere else; the
        // Welford mean below stays on the decision side.
        // Both rungs' bridges measure the same lane, so at most one bridge increment
        // folds per (block,row) per build; a failed preferred attempt does not consume
        // the row, so the fallback may still tighten it (Increment 5).
        if (lane_bridge_on
            && !(dual_cons && lane_incr_done.count({ fx.block, fx.row }))
            // A lane pinned by a committed fix skips its bridge increment; never taken
            // with skip_pinned_increments off (LF-1 INC-2).
            && carry_driver.laneIncrementSkippable(fx.block, fx.row)) {
            ++wc.lane_pinned_skips;
        } else if (lane_bridge_on
            && !(dual_cons && lane_incr_done.count({ fx.block, fx.row }))) {
            constexpr double kLaneBridgeSigmaFloorCyc = 0.02;   // phase-precision floor
            if (raw_ok) {
                const double z_raw = (sum_gf_raw
                                      - (bridge_debias_on ? 0.0 : iono_co * iono_dd)
                                      + lam_ewl * static_cast<double>(fx.integer)) / lam_wl;
                const double y_raw = z_raw - (z_lin1 / lam1 - z_lin2 / lam2);
                const double R_raw = std::max(R_gf_raw,
                    kLaneBridgeSigmaFloorCyc * kLaneBridgeSigmaFloorCyc);
                if (carry_driver.applyLaneMeasurement(blocks, fx.block, fx.row, y_raw, R_raw)) {
                    ++wc.lane_bridge_rows;
                    if (dual_cons) lane_incr_done.insert({ fx.block, fx.row });
                } else {
                    ++wc.lane_skip_meas;
                }
            } else {
                ++wc.lane_skip_meas;
            }
        }

        // z_WL^bridge = [Σc·GF_b − C_iono·Σc·Î₁ + λ_EWL·N_EWL]/λ_WL (cyc).  The FFR test
        // runs on the iono de-biased detector statistic (the decision source, never the
        // posterior).  The de-bias removes the integer-valued differential-iono bias that
        // falsified v1; the remaining DD-iono estimate error shows up in |frac|.
        const double sigma_direct = std::sqrt(std::max(R_bridge, 0.0));   // Welford phase noise
        // With the per-sample de-bias on (bridge_debias_on), sum_gf is already iono-free
        // frame by frame, so the read-time subtraction is disabled to avoid subtracting twice.
        const double z_wl_direct  = (sum_gf
                                     - (bridge_debias_on ? 0.0 : iono_co * iono_dd)
                                     + lam_ewl * static_cast<double>(fx.integer)) / lam_wl;

        // --- Mid hop through E6: chain top → mid → WL.  Hop-hi determines the mid-rung
        // integer m = round([Σc·GF_hi − C_hi·Σc·Î₁ + λ_TOP·N_TOP]/λ_MID) under its own FFR
        // gates and the MW(mid,b2) closure; hop-lo then re-expresses the WL statistic
        // against the rounded m, which removes the top hop's λ_TOP-scale phase and iono
        // amplification (a λ multiplier about 3.3× smaller at λ_MID 2.931 m).  A failed
        // mid fix or closure falls back to the direct hop and is counted (mid_fallback,
        // mid_closure); a cycle without mature hop bridges (mid_ok false) takes the
        // direct hop without a count.
        double z_wl = z_wl_direct, sigma_bridge = sigma_direct;
        bool   used_mid = false;
        if (mid_ok && lam_mid > 0.0) {
            const double sigma_hi = std::sqrt(std::max(R_hi, 0.0));
            const double z_mid = (sum_gf_hi
                                  - (bridge_debias_on ? 0.0 : iono_co_hi * iono_dd)
                                  + lam_ewl * static_cast<double>(fx.integer)) / lam_mid;
            const double m = std::round(z_mid);
            bool mid_fix = sigma_hi < nmw.wl_fix_sigma_cyc
                        && std::abs(z_mid - m) < nmw.wl_fix_frac_cyc;
            if (mid_fix && mw65_ok) {
                // The mid integer has its own free integrity check: the plain
                // MW(mid,b2) DD must agree within its σ (invariant #1).
                const double tol65 = 0.5 + 3.0 * std::sqrt(std::max(R_mw65, 0.0));
                if (std::abs(z_mw65 - m) > tol65) { mid_fix = false; ++wc.mid_closure; }
            } else if (!mid_fix) {
                ++wc.mid_fallback;
            }
            if (mid_fix) {
                z_wl = (sum_gf_lo
                        - (bridge_debias_on ? 0.0 : iono_co_lo * iono_dd)
                        + lam_mid * m) / lam_wl;
                sigma_bridge = std::sqrt(std::max(R_lo, 0.0));
                used_mid = true;
            }
        }

        if (sigma_bridge > nmw.bridge_max_sigma_cyc) { ++wc.skip_bridge_wide; continue; }
        const double w = std::round(z_wl);
        if (!(sigma_bridge < nmw.wl_fix_sigma_cyc &&
              std::abs(z_wl - w) < nmw.wl_fix_frac_cyc)) { ++wc.skip_not_fixed; continue; }
        // Wrong-integer backstop: if the direct hop also passes the FFR gates on its own
        // and rounds to a different integer, neither is decided (counted in mid_disagree).
        if (used_mid) {
            const double wd = std::round(z_wl_direct);
            if (sigma_direct < nmw.wl_fix_sigma_cyc
                && std::abs(z_wl_direct - wd) < nmw.wl_fix_frac_cyc
                && wd != w) { ++wc.mid_disagree; continue; }
        }

        // Closure: the plain MW(1,2) DD must agree with w within its own σ (a free
        // integrity check).  A cycle is blocked only when MW(1,2) is present and clearly
        // off.  In a post-slip cold window the primary-pair MW arc has usually reset as
        // well (mw12_ok false, so no block); a cold decision is instead gated on |frac|,
        // backed by D5, and re-decided idempotently as the statistics mature (UWL-1 §3.3).
        if (mw12_ok) {
            const double tol = 0.5 + 3.0 * std::sqrt(std::max(R_mw12, 0.0));
            if (std::abs(z_mw12 - w) > tol) { ++wc.skip_closure; continue; }
        }

        // --- Sibling closure (Increment 5, §1b): the untaken rung's statistic must sit on
        // N_top·λ_top/λ_sib.  A fix from '8' checks z(7,5) against 2·N₈ (the finer
        // sibling: a ±1 error in N₈ moves the residual by 2, which is readily detected);
        // a fix from '7' checks z(8,5) against N₇/2 (an odd N₇ makes the target
        // half-integer, the AltBOC-coherence parity, visible in |frac|).  The statistic is
        // measured on every dual decision.  The hard gate (uwl_sib_gate) blocks only a
        // disagreement beyond 0.5 + 3σ sibling cycles and so demotes the cycle to the
        // fallback rung (the fallback entry follows in ewl_fixed and is not deduplicated,
        // because a blocked decision never consumes the row).
        if (dual_cons) {
            if (sib_ok && lam_sib > 0.0 && lam_ewl > 0.0) {
                ++wc.sib_checked;
                const double expect =
                    static_cast<double>(fx.integer) * (lam_ewl / lam_sib);
                const double r     = z_sib - expect;
                const double rfrac = r - std::round(r);
                wc.sib_abs_frac_sum += std::abs(rfrac);
                if (std::llround(r) != 0) ++wc.sib_int_off;
                if (uwl_sib_gate_on) {
                    const double tol_s =
                        0.5 + 3.0 * std::sqrt(std::max(R_sib, 0.0));
                    if (std::abs(r) > tol_s) { ++wc.sib_blocked; continue; }
                }
            } else {
                ++wc.sib_unavail;
            }
        }

        ++wc.cycles_decided;
        wc.sum_bridge_R += R_bridge;
        if (bridge_cold) ++wc.cold_bridge_cycles;
        if (used_mid)    ++wc.mid_decided;
        if (fx_is_top2)  ++wc.top2_decided;
        if (dual_cons)   walk_decided.insert({ fx.block, fx.row });

        // Group ids for the chord's two carried bands (chord.v → sat → primary pair,
        // resolved at the loop top).
        if (sat_lbl.empty()) { ++wc.skip_basis; continue; }
        const char cons = ccons;
        const auto pcit = primary.find({ chord.u, sat_lbl });
        if (pcit == primary.end() || !pcit->second.ok) { ++wc.skip_basis; continue; }

        if (lane_fix_on) {
            // --- FFR fix on the pair block (WL-3 §3.3): an information-idempotent
            // lane-coordinate set (recenter and recollapse) applied on every decided
            // epoch.  A one-shot fold on change is not enough: the decided lane drifts
            // back to MW quality under q and the noisy increments.  The pair inject
            // fallback plan is stashed as well; it is applied post-solve only if the carry
            // still disagrees by more than inject_disagree_min_cyc, which the retirement
            // telemetry shows is about 0 in steady state.
            const auto pgit = carry_group_id.find(
                { cons, pcit->second.sig1 + "+" + pcit->second.sig2 });
            if (pgit == carry_group_id.end()) { ++wc.skip_basis; continue; }
            const double z_lin_wl = z_lin1 / lam1 - z_lin2 / lam2;
            const int rc = carry_driver.applyLaneFix(
                blocks, fx.block, fx.row, static_cast<long long>(std::llround(w)),
                w - z_lin_wl, nmw.fix_weight_sigma_cyc);
            if (rc == 1) {
                ++wc.lane_fix_folded;
                if (mw_tagsets) d5_hold_chords.insert(chord);
            } else if (rc == 0) {
                ++wc.lane_fix_unchanged;
            } else {
                ++wc.skip_basis;   // not a pair chord (residual-group cycle)
                continue;
            }
            WlInjectPlan pl;
            pl.pair = true;
            pl.group_l1 = pl.group_l2 = pgit->second;
            pl.chord = chord;
            pl.w = w;
            pl.z_lin1 = z_lin1; pl.z_lin2 = z_lin2;
            pl.lambda1 = lam1;  pl.lambda2 = lam2;
            pl.cons = cons;
            wl_inject_epoch.push_back(pl);
            continue;
        }

        const auto g1it = carry_group_id.find({ cons, pcit->second.sig1 });
        const auto g2it = carry_group_id.find({ cons, pcit->second.sig2 });
        if (g1it == carry_group_id.end() || g2it == carry_group_id.end()) {
            ++wc.skip_basis; continue;
        }
        const int group_l1 = g1it->second, group_l2 = g2it->second;

        // Pre-solve carried lane value, for the hold-row gate: the integer-hard hold row
        // is folded only when the carry already agrees within inject_disagree_min_cyc
        // (innovation about 0, so the row does not contend with the stiff carry as in F6);
        // otherwise it is skipped this epoch and the post-solve inject moves the value.
        double z1p = 0.0, v1p = 0.0, z2p = 0.0, v2p = 0.0;
        const bool have_l1 = carry_driver.carriedChordValue(group_l1, chord, z1p, v1p);
        const bool have_l2 = carry_driver.carriedChordValue(group_l2, chord, z2p, v2p);
        bool agree = false;
        if (have_l1 && have_l2) {
            const double A1 = (z1p + z_lin1) / lam1, A2 = (z2p + z_lin2) / lam2;
            agree = std::abs(w - (A1 - A2)) <= nmw.inject_disagree_min_cyc;
        }
        if (agree) {
            // Build/append the WL hold row (WL-cycle correction frame): +c/λ1 on col1,
            // −c/λ2 on col2; zhat = w − Σ[(c/λ1)x_lin1 − (c/λ2)x_lin2]; Pz = fix_weight².
            std::size_t ob;
            auto hit = hold_block_of.find(fx.block);
            if (hit == hold_block_of.end()) {
                ob = wl_hold_out.size();
                hold_block_of[fx.block] = ob;
                wl_hold_out.emplace_back();
            } else { ob = hit->second; }
            std::vector<NetworkNormalEquations::DdPriorBlock::Term> hrow;
            double z_lin_wl = 0.0;
            hrow.reserve(terms.size() * 2);
            bool hok = true;
            for (const auto& t : terms) {
                const auto rit = revmap.find({ static_cast<int>(t.station),
                                               static_cast<int>(t.local_col) });
                if (rit == revmap.end()) { hok = false; break; }
                const auto pit = primary.find({ rit->second.rx, rit->second.sat });
                if (pit == primary.end() || !pit->second.ok) { hok = false; break; }
                const PP& pp = pit->second;
                const double l1 = lam(rit->second.sat, pp.sig1);
                const double l2 = lam(rit->second.sat, pp.sig2);
                if (!(l1 > 0.0 && l2 > 0.0)) { hok = false; break; }
                hrow.push_back({ t.station, pp.col1, +t.coeff / l1 });
                hrow.push_back({ t.station, pp.col2, -t.coeff / l2 });
                const auto l1it = lin_of.find({ static_cast<int>(t.station), pp.col1 });
                const auto l2it = lin_of.find({ static_cast<int>(t.station), pp.col2 });
                if (l1it == lin_of.end() || l2it == lin_of.end()) { hok = false; break; }
                z_lin_wl += (t.coeff / l1) * l1it->second - (t.coeff / l2) * l2it->second;
            }
            if (hok) {
                auto& dst = wl_hold_out[ob];
                dst.rows.push_back(std::move(hrow));
                const Eigen::Index k = static_cast<Eigen::Index>(dst.rows.size());
                dst.zhat.conservativeResize(k);
                dst.zhat(k - 1) = w - z_lin_wl;
                dst.Pz.conservativeResize(k, k);
                dst.Pz.row(k - 1).setZero();
                dst.Pz.col(k - 1).setZero();
                dst.Pz(k - 1, k - 1) = nmw.fix_weight_sigma_cyc * nmw.fix_weight_sigma_cyc;
                ++wc.holds_folded;
                if (mw_tagsets) d5_hold_chords.insert(chord);
            }
        }
        // Stash the inject plan (applied post-solve against the fresh posterior).  The
        // 1:1-basis requirement is re-checked there (both bands must carry the chord).
        WlInjectPlan pl;
        pl.group_l1 = group_l1; pl.group_l2 = group_l2; pl.chord = chord;
        pl.w = w; pl.z_lin1 = z_lin1; pl.z_lin2 = z_lin2;
        pl.lambda1 = lam1; pl.lambda2 = lam2;
        pl.cons = cons;
        wl_inject_epoch.push_back(pl);
    }
  }  // --- end WL-2 WL lane ---

  if (mw_lane_trace) {   // lane-trace entries for every resolvable primary cycle (F-B)
    lane_trace_epoch.clear();
    std::map<int, std::string> sat_of_v;
    for (const auto& [s, v] : carry_sat_vertex) sat_of_v[v] = s;
    for (std::size_t b = 0; b < blocks.size(); ++b) {   // pure carry blocks (pre-append)
        const auto& chords_b = carry_driver.blockChordsForTest(b);
        for (std::size_t r = 0; r < blocks[b].rows.size(); ++r) {
            if (r >= chords_b.size()) break;
            const auto& terms = blocks[b].rows[r];
            if (terms.empty()) continue;
            LaneTraceEntry e;
            e.chord = chords_b[r];
            bool ok = true;
            for (const auto& t : terms) {
                const auto rit = revmap.find({ static_cast<int>(t.station),
                                               static_cast<int>(t.local_col) });
                if (rit == revmap.end()) { ok = false; break; }
                const auto pit = primary.find({ rit->second.rx, rit->second.sat });
                if (pit == primary.end() || !pit->second.ok
                    || rit->second.signal != pit->second.sig1
                    || static_cast<int>(t.local_col) != pit->second.col1) { ok = false; break; }
                const PP& pp = pit->second;
                const auto l1it = lin_of.find({ static_cast<int>(t.station), pp.col1 });
                const auto l2it = lin_of.find({ static_cast<int>(t.station), pp.col2 });
                if (l1it == lin_of.end() || l2it == lin_of.end()) { ok = false; break; }
                e.z_lin1 += t.coeff * l1it->second;
                e.z_lin2 += t.coeff * l2it->second;
                e.lam1 = lam(rit->second.sat, pp.sig1);
                e.lam2 = lam(rit->second.sat, pp.sig2);
            }
            if (!ok || !(e.lam1 > 0.0 && e.lam2 > 0.0)) continue;
            const auto svit = sat_of_v.find(e.chord.v);
            if (svit == sat_of_v.end()) continue;
            e.sat = svit->second;
            const char cons = letterOfSat(e.sat);
            const auto pcit = primary.find({ e.chord.u, e.sat });
            if (pcit == primary.end() || !pcit->second.ok) continue;
            if (lane_on) {
                // With the lane layer (WL-3) both bands live in one pair group; the write
                // site reads the joint block through carriedPairValue (g1 == g2 marks it).
                const auto pgit = carry_group_id.find(
                    { cons, pcit->second.sig1 + "+" + pcit->second.sig2 });
                if (pgit == carry_group_id.end()) continue;
                e.g1 = e.g2 = pgit->second;
            } else {
                const auto g1it = carry_group_id.find({ cons, pcit->second.sig1 });
                const auto g2it = carry_group_id.find({ cons, pcit->second.sig2 });
                if (g1it == carry_group_id.end() || g2it == carry_group_id.end()) continue;
                e.g1 = g1it->second; e.g2 = g2it->second;
            }
            lane_trace_epoch.push_back(std::move(e));
        }
    }
  }
    for (auto& wb : wl_hold_out) blocks.push_back(std::move(wb));   // then WL hold rows
}

} // namespace GnssProcessor
