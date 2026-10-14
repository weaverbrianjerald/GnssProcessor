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
#include "logging/NetworkRunTraces.hpp"       // traces.trace
#include "slip/MwSlipDetector.hpp"            // MwSlipDetector::MwArcZd* / MwBridgeMean

#include <Eigen/Dense>

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace GnssProcessor {

void networkMwWlInject(
    NetworkRunContext& ctx,
    const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkRunTraces& traces,
    NetworkInvariantCarryDriver& carry_driver,
    const std::vector<NetStation>& stations,
    const SyncedEpoch& /*se*/,
    std::set<std::pair<int, std::string>>& /*carry_slipped_ambs*/,
    const WithinEpochCulprits& /*we_culprits*/,
    int /*n_threads*/,
    std::vector<EwlLane>& ewl_lanes,
    const NetworkMwWlLayer::Config& ewl_cfg,
    std::vector<WlInjectPlan>& wl_inject_epoch,
    std::vector<LaneTraceEntry>& lane_trace_epoch,
    WlCounters& wl_epoch,
    const std::map<std::pair<int, int>, Rev>& revmap,
    const std::map<std::pair<int, int>, double>& lin_of,
    const std::map<std::pair<int, std::string>, double>& iono_of,
    const std::map<std::pair<int, std::string>, PP>& primary,
    const std::map<std::pair<int, std::string>, std::pair<std::string, int>>& band_top_of,
    const std::function<double(const std::string&, const std::string&)>& lam,
    std::vector<NetworkNormalEquations::DdPriorBlock>& blocks,
    bool ewl_on,
    bool wl_inject_on,
    bool bridge_debias_on,
    bool mw_d5_dump,
    bool mw_lane_trace,
    bool mw_tagsets) {
    // --- Alias preamble: byte-copies of the run-TU / networkBuildCarryDdPriors
    //     aliases the relocated WL/EWL inject body reads. ----
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

  if (ewl_on) {   // --- EWL lane (WL-2): (band2, band-top) pair, fix gated on the FFR statistic ---
    for (auto& lane : ewl_lanes) lane.epoch = NetworkMwWlLayer::Counters{};
    // The eligible cycle is a primary-sig1 cycle, and the row is the top-tier DD
    // on the same (rx,sat) arcs, pairing the estimator's primary sig2 (channel
    // discipline) with the constellation's top band (GPS EWL (L2,L5)).  (hi,lo) is
    // ordered by actual frequency, so the MW statistic keys (sat, hi, lo) and λ_top
    // stays positive.  The top band is not carried (the census block excludes it), so
    // no carried column competes with these rows.
    auto ewl_resolver = [&](std::size_t est_station, Eigen::Index local_col,
                            NetworkMwWlLayer::ArcData& out) -> bool {
        const auto rit = revmap.find(
            { static_cast<int>(est_station), static_cast<int>(local_col) });
        if (rit == revmap.end()) return false;
        const Rev& rv = rit->second;
        const auto pit = primary.find({ rv.rx, rv.sat });
        const PP* pp = (pit != primary.end()) ? &pit->second : nullptr;
        if (pp && pp->ok && rv.signal == pp->sig1
            && static_cast<int>(local_col) == pp->col1) {
            out.is_primary_sig1 = true;
            const auto b5 = band_top_of.find({ rv.rx, rv.sat });
            if (b5 == band_top_of.end()) { out.col2 = -1; out.have_mw = false; return true; }
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
            out.mw_zd_mean_m  = zd.mean_m;
            out.mw_var_m2     = zd.var_of_mean_m2;
            out.mw_count      = static_cast<long long>(zd.count);
            out.lambda_wl     = zd.lambda_wl;              // = λ_top = c/(f_hi − f_lo)
            out.bia_corrected = zd.bia_corrected;
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
    for (auto& lane : ewl_lanes) {
        std::vector<NetworkMwWlLayer::FixedEwlRow> lane_fixed;
        // The returned row blocks are not folded: the lane decides, fixes and exposes its
        // integers (lane_fixed) and its counters only.
        (void)lane.layer.build(blocks, ewl_resolver, lane.epoch, &lane_fixed);
        ewl_fixed.insert(ewl_fixed.end(), lane_fixed.begin(), lane_fixed.end());
    }
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
  if (wl_inject_on) {
      wl_inject_epoch.clear();
      wl_epoch = WlCounters{};
  }
  if (mw_tagsets) d5_hold_chords.clear();

  if (wl_inject_on && !ewl_fixed.empty()) {
    // --- WL lane (WL-2): phase bridge → WL integer → hold row and inject plan (§3.3/§3.4) ---
    // For each FFR-fixed EWL cycle, bridge phase-only from the fixed N_EWL to a fast WL
    // integer w = [Σc·GF_b + λ_EWL·N_EWL]/λ_WL, cross-check it against the plain MW(1,2),
    // and either fold an integer-hard hold row (the carry already agrees with the lane,
    // so the innovation is about 0) or stash an inject (the carry moves to w post-solve,
    // NL preserved).
    // sat_of_vertex maps a chord's ArcKey.v back to its satellite label (the reverse of
    // carry_sat_vertex) for the primary-pair and group-id lookups.
    std::map<int, std::string> sat_of_vertex;
    for (const auto& [s, v] : carry_sat_vertex) sat_of_vertex[v] = s;
    // Group hold rows by their source carry block so shared (1,2) columns fold jointly.
    std::map<std::size_t, std::size_t> hold_block_of;   // carry block b → wl_hold_out index

    for (std::size_t fxi = 0; fxi < ewl_fixed.size(); ++fxi) {
        const auto& fx = ewl_fixed[fxi];
        if (fx.block >= blocks.size() || fx.block >= carry_driver.numBlocks()) {
            ++wl_epoch.rows_seen; continue;   // defensive; stays pooled
        }
        const auto& chords_b = carry_driver.blockChordsForTest(fx.block);
        if (fx.row >= chords_b.size() || fx.row >= blocks[fx.block].rows.size()) {
            ++wl_epoch.rows_seen; continue;   // defensive; stays pooled
        }
        const ArcKey chord = chords_b[fx.row];
        // Resolve the chord's constellation up front (the group lookups below need it).
        const auto svit0 = sat_of_vertex.find(chord.v);
        const std::string sat_lbl =
            svit0 != sat_of_vertex.end() ? svit0->second : std::string{};
        const char ccons = letterOfSat(sat_lbl);
        WlCounters& wc = wl_epoch;
        ++wc.rows_seen;
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
        for (const auto& t : terms) {
            const auto rit = revmap.find({ static_cast<int>(t.station),
                                           static_cast<int>(t.local_col) });
            if (rit == revmap.end()) { ok = false; break; }
            const Rev& rv = rit->second;
            const auto pit = primary.find({ rv.rx, rv.sat });
            if (pit == primary.end() || !pit->second.ok) { ok = false; break; }
            const PP& pp = pit->second;
            const auto b5 = band_top_of.find({ rv.rx, rv.sat });
            if (b5 == band_top_of.end()) { ok = false; break; }
            auto brg = stations[rv.rx].mw_slip
                ? stations[rv.rx].mw_slip->arcBridge(rv.sat, pp.sig1, pp.sig2, b5->second.first)
                : MwSlipDetector::MwBridgeMean{};
            const std::size_t brg_need =
                static_cast<std::size_t>(std::max(2, ewl_cfg.min_arc_epochs));
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

        const double z_wl = z_wl_direct, sigma_bridge = sigma_direct;

        if (sigma_bridge > nmw.bridge_max_sigma_cyc) { ++wc.skip_bridge_wide; continue; }
        const double w = std::round(z_wl);
        if (!(sigma_bridge < nmw.wl_fix_sigma_cyc &&
              std::abs(z_wl - w) < nmw.wl_fix_frac_cyc)) { ++wc.skip_not_fixed; continue; }

        // Closure: the plain MW(1,2) DD must agree with w within its own σ (a free
        // integrity check).  A cycle is blocked only when MW(1,2) is present and clearly
        // off; after a slip the primary-pair MW arc has usually reset as well (mw12_ok
        // false, so no block).
        if (mw12_ok) {
            const double tol = 0.5 + 3.0 * std::sqrt(std::max(R_mw12, 0.0));
            if (std::abs(z_mw12 - w) > tol) { ++wc.skip_closure; continue; }
        }

        ++wc.cycles_decided;
        wc.sum_bridge_R += R_bridge;

        // Group ids for the chord's two carried bands (chord.v → sat → primary pair,
        // resolved at the loop top).
        if (sat_lbl.empty()) { ++wc.skip_basis; continue; }
        const char cons = ccons;
        const auto pcit = primary.find({ chord.u, sat_lbl });
        if (pcit == primary.end() || !pcit->second.ok) { ++wc.skip_basis; continue; }

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
            const auto g1it = carry_group_id.find({ cons, pcit->second.sig1 });
            const auto g2it = carry_group_id.find({ cons, pcit->second.sig2 });
            if (g1it == carry_group_id.end() || g2it == carry_group_id.end()) continue;
            e.g1 = g1it->second; e.g2 = g2it->second;
            lane_trace_epoch.push_back(std::move(e));
        }
    }
  }
    for (auto& wb : wl_hold_out) blocks.push_back(std::move(wb));   // then WL hold rows
}

} // namespace GnssProcessor
