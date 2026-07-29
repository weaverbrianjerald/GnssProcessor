/**
 * @file NetworkInstallCarry.cpp
 * @brief Implementation of @ref networkInstallCarry, the SB_INSTALL block relocated
 *        VERBATIM (dedent-8, zero substitution) out of Application::runNetwork at
 *        decomposition INC-9b (2026-07-22).  Byte-copy of the former if(carry_invariant)
 *        body; the alias preamble reconstructs nc/nmw/trace, the ctx.carry aliases and
 *        the wlAccumulate lambda so the code reads the original names.
 */

#include "filter/NetworkInstallCarry.hpp"

#include "app/NetworkRunContext.hpp"     // ctx.carry / ctx.counters
#include "config/GnssConfig.hpp"         // config_.getNetworkConfig() / .mw_wl
#include "filter/StateKeySchema.hpp"     // StateKeys::parse*
#include "slip/NetworkRawObservables.hpp"// SlipQcChannel
#include "logging/NetworkRunTraces.hpp"  // traces.trace

#include <Eigen/Dense>

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace GnssProcessor {

void networkInstallCarry(
    NetworkRunContext& ctx,
    const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkRunTraces& traces,
    NetworkInvariantCarryDriver& carry_driver,
    const NetworkEstimator& est,
    const Eigen::VectorXd& x_s,
    const NetworkSbasis::Plan& plan,
    const SyncedEpoch& se,
    const std::vector<NetStation>& stations,
    SlipQcChannel& slip_qc,
    const std::map<int, std::map<ArcKey, std::string>>& carry_arc_amb_key,
    const std::map<int, std::map<ArcKey, std::string>>& carry_arc_amb_key2,
    const std::filesystem::path& log_dir_,
    bool carry_par,
    int n_threads,
    int seam_threads,
    bool carry_innov_gate,
    bool mw_wl_on,
    bool lane_on,
    bool ewl_on,
    bool uwl_on,
    bool wl_inject_on,
    bool lane_fix_on,
    bool mw_d5_dump,
    bool mw_lane_trace,
    bool mw_tagsets,
    bool mw_wl_f6_dump,
    std::vector<EwlLane>& ewl_lanes,
    std::vector<WlInjectPlan>& wl_inject_epoch,
    std::vector<LaneTraceEntry>& lane_trace_epoch,
    NetworkMwWlLayer::Counters& mw_wl_run,
    NetworkMwWlLayer::Counters& mw_wl_epoch,
    WlCounters& wl_run,
    WlCounters& wl_epoch,
    std::map<char, WlCounters>& wl_cons_run,
    std::map<char, WlCounters>& wl_cons_epoch,
    F6Capture& mw_wl_f6) {
    // --- Alias preamble: reconstructs the run-TU aliases the relocated
    //     SB_INSTALL body reads (nc/nmw/trace, the ctx.carry aliases, and the
    //     wlAccumulate lambda).  Byte-copies of the former runNetwork() lines. ----
    const auto& nc  = config_.getNetworkConfig();
    const auto& nmw = config_.getNetworkConfig().mw_wl;
    auto& trace = traces.trace;
    auto& lane_trace_header_written = ctx.carry.lane_trace_header_written;
    auto& d5_ewl_stasat = ctx.carry.d5_ewl_stasat;
    auto& d5_hold_chords = ctx.carry.d5_hold_chords;
    auto& d5_inject_chords_prev = ctx.carry.d5_inject_chords_prev;
    auto& d5_inject_chords_now = ctx.carry.d5_inject_chords_now;
    auto& d5_dump_header_written = ctx.carry.d5_dump_header_written;
    auto& carry_prev_tow = ctx.carry.carry_prev_tow;
    auto& carry_epochs = ctx.carry.carry_epochs;
    auto& carry_born_total = ctx.carry.carry_born_total;
    auto& carry_marg_total = ctx.carry.carry_marg_total;
    auto& carry_merges_total = ctx.carry.carry_merges_total;
    auto& carry_splits_total = ctx.carry.carry_splits_total;
    auto& carry_swaps_total = ctx.carry.carry_swaps_total;
    auto& carry_zdim_last = ctx.carry.carry_zdim_last;
    auto& carry_slipkills_total = ctx.carry.carry_slipkills_total;
    auto& carry_dropped_total = ctx.carry.carry_dropped_total;
    auto& carry_innov_pending = ctx.carry.carry_innov_pending;
    auto& carry_innov_flags_total = ctx.carry.carry_innov_flags_total;
    auto& carry_innov_retaps_total = ctx.carry.carry_innov_retaps_total;
    auto& carry_innov_map_miss = ctx.carry.carry_innov_map_miss;
    auto wlAccumulate = [](WlCounters& a, const WlCounters& b) {
        a.rows_seen           += b.rows_seen;
        a.cycles_decided      += b.cycles_decided;
        a.holds_folded        += b.holds_folded;
        a.injects             += b.injects;
        a.inject_abs_sum      += b.inject_abs_sum;
        a.skip_bridge_missing += b.skip_bridge_missing;
        a.skip_no_iono        += b.skip_no_iono;
        a.skip_bridge_wide    += b.skip_bridge_wide;
        a.skip_not_fixed      += b.skip_not_fixed;
        a.skip_closure        += b.skip_closure;
        a.skip_quarantine     += b.skip_quarantine;
        a.skip_basis          += b.skip_basis;
        a.sum_bridge_R        += b.sum_bridge_R;
        a.lane_mw_rows        += b.lane_mw_rows;
        a.lane_bridge_rows    += b.lane_bridge_rows;
        a.lane_fix_folded     += b.lane_fix_folded;
        a.lane_fix_unchanged  += b.lane_fix_unchanged;
        a.lane_skip_meas      += b.lane_skip_meas;
        a.lane_pinned_skips   += b.lane_pinned_skips;
        a.cold_bridge_cycles  += b.cold_bridge_cycles;
        a.mid_decided         += b.mid_decided;
        a.mid_fallback        += b.mid_fallback;
        a.mid_closure         += b.mid_closure;
        a.mid_disagree        += b.mid_disagree;
        a.top2_decided        += b.top2_decided;
        a.dual_dedupe         += b.dual_dedupe;
        a.sib_checked         += b.sib_checked;
        a.sib_unavail         += b.sib_unavail;
        a.sib_int_off         += b.sib_int_off;
        a.sib_blocked         += b.sib_blocked;
        a.sib_abs_frac_sum    += b.sib_abs_frac_sum;
    };

    // parallel_carry: thread installPosterior's posterior-cov GEMMs.  Run it
    // SERIAL-over-blocks (n_threads=1, its byte-identical-to-serial path) so Eigen's
    // threading does not nest with the seam's per-block OpenMP.
    try { CarryEigenScope es(carry_par, n_threads);
          carry_driver.installPosterior(est, x_s, plan.shared_datum,
                                        carry_par ? 1 : seam_threads); }
    catch (const std::exception& ex) {
        log->warn("Network[Card4]: DD-carry install failed at TOW {:.1f} ({}); "
                  "carry reset (refloats next epoch).", se.time.tow, ex.what());
        carry_driver.reset();
    }
    carry_prev_tow = se.time.tow;
    // M5 card WL-1: fold the accepted build's WL eligibility tallies into the run
    // totals (this install runs only for an accepted epoch; the last buildCarryDdPriors
    // before the accepted solve set mw_wl_epoch).  Invariant #4: counters or it didn't happen.
    if (mw_wl_on) {
        mw_wl_run.cycles_seen       += mw_wl_epoch.cycles_seen;
        mw_wl_run.rows_built        += mw_wl_epoch.rows_built;
        mw_wl_run.rows_capped       += mw_wl_epoch.rows_capped;
        mw_wl_run.skip_not_primary  += mw_wl_epoch.skip_not_primary;
        mw_wl_run.skip_missing_band += mw_wl_epoch.skip_missing_band;
        mw_wl_run.skip_missing_mw   += mw_wl_epoch.skip_missing_mw;
        mw_wl_run.skip_young_arc    += mw_wl_epoch.skip_young_arc;
        mw_wl_run.skip_bad_lambda   += mw_wl_epoch.skip_bad_lambda;
        mw_wl_run.skip_mixed_frame  += mw_wl_epoch.skip_mixed_frame;
        mw_wl_run.sum_R             += mw_wl_epoch.sum_R;

        // --- F6 one-shot attribution write (log-only; docs/HANDOFF_mw_wl_card.md §6c). ---
        // The accepted epoch's x_s is final here; add reconstructed_abs (the estimator's
        // current WL DD over the same cycle) + the kernel WL-block innovation, then dump.
        if (mw_wl_f6_dump && !mw_wl_f6.written && mw_wl_f6.pending
            && mw_wl_f6.tow == se.time.tow) {
            std::map<int, Eigen::VectorXd> xr_of;   // est_station -> local corrections
            for (const auto& fr : mw_wl_f6.rows)
                for (const auto& a : fr.arcs)
                    if (a.est_station >= 0 && !xr_of.count(a.est_station)) {
                        try { xr_of[a.est_station] = est.reconstructStation(
                                  static_cast<std::size_t>(a.est_station), x_s); }
                        catch (...) { xr_of[a.est_station] = Eigen::VectorXd(); }
                    }
            // Kernel innovation v = z_free − Gᵀx_s − ẑ.  ddPriorInnovation is BLOCK-scoped, so
            // under f6_all_blocks each block gets its own request and each row indexes into
            // its own block's vector (rows of a block are contiguous in the capture).
            std::map<std::size_t, Eigen::VectorXd> v_of;
            {
                const Eigen::MatrixXd cov0 = Eigen::MatrixXd::Zero(x_s.size(), x_s.size());
                std::set<std::size_t> want(mw_wl_f6.row_block.begin(),
                                           mw_wl_f6.row_block.end());
                if (want.empty()) want.insert(mw_wl_f6.wl_block_index);
                for (const std::size_t bi : want) {
                    Eigen::VectorXd v; Eigen::MatrixXd cov_v;   // (cov unused; v is cov-free)
                    try { est.ddPriorInnovation(bi, x_s, cov0, v, cov_v); }
                    catch (...) { v.resize(0); }
                    v_of[bi] = std::move(v);
                }
            }
            std::map<std::size_t, std::size_t> row_in_block;   // block -> running row index
            std::error_code ec_f6;
            std::filesystem::create_directories(log_dir_, ec_f6);
            std::ofstream f6(log_dir_ / "network_mw_wl_f6.csv");
            if (f6) {
                f6 << "# M5 WL-1 F6 attribution dump — first arming epoch (week "
                   << mw_wl_f6.week << ", tow " << fmt::format("{:.1f}", mw_wl_f6.tow)
                   << ", loop_epoch " << mw_wl_f6.loop_epoch << ", wl_block_index "
                   << mw_wl_f6.wl_block_index << ")\n";
                f6 << "row,n_arcs,z_mw_cyc,z_mw_frac,z_lin_cyc,z_corr_cyc,zhat_block_cyc,"
                      "reconstructed_abs_cyc,recon_minus_zmw_cyc,recon_abs_frac,innovation_v_cyc\n";
                for (std::size_t r = 0; r < mw_wl_f6.rows.size(); ++r) {
                    const auto& fr = mw_wl_f6.rows[r];
                    double recon = 0.0; bool recon_ok = true;
                    for (const auto& a : fr.arcs) {
                        const auto it = xr_of.find(a.est_station);
                        double xr1 = 0.0, xr2 = 0.0;
                        if (it != xr_of.end()
                            && a.col1 >= 0 && static_cast<Eigen::Index>(a.col1) < it->second.size()
                            && a.col2 >= 0 && static_cast<Eigen::Index>(a.col2) < it->second.size()) {
                            xr1 = it->second(a.col1); xr2 = it->second(a.col2);
                        } else { recon_ok = false; }
                        if (a.lambda1 > 0.0 && a.lambda2 > 0.0)
                            recon += (a.coeff / a.lambda1) * (a.x_lin1 + xr1)
                                   - (a.coeff / a.lambda2) * (a.x_lin2 + xr2);
                    }
                    const double zmw_frac = fr.z_mw - std::round(fr.z_mw);
                    const std::size_t bi = (r < mw_wl_f6.row_block.size())
                                               ? mw_wl_f6.row_block[r]
                                               : mw_wl_f6.wl_block_index;
                    const std::size_t rb = row_in_block[bi]++;
                    const auto vit = v_of.find(bi);
                    const double vrow =
                        (vit != v_of.end() && static_cast<Eigen::Index>(rb) < vit->second.size())
                            ? vit->second(static_cast<Eigen::Index>(rb))
                            : std::numeric_limits<double>::quiet_NaN();
                    f6 << r << ',' << fr.arcs.size() << ','
                       << fmt::format("{:.4f}", fr.z_mw) << ','
                       << fmt::format("{:.4f}", zmw_frac) << ','
                       << fmt::format("{:.4f}", fr.z_lin) << ','
                       << fmt::format("{:.4f}", fr.z_corr) << ','
                       << fmt::format("{:.4f}", fr.zhat_block) << ','
                       << (recon_ok ? fmt::format("{:.4f}", recon) : std::string("nan")) << ','
                       << (recon_ok ? fmt::format("{:.4f}", recon - fr.z_mw) : std::string("nan")) << ','
                       << (recon_ok ? fmt::format("{:.4f}", recon - std::round(recon)) : std::string("nan")) << ','
                       << fmt::format("{:.4f}", vrow) << '\n';
                }
                f6 << "row,arc,est_station,sat,sig1,sig2,coeff,mw_mean_m,mw_arc_cyc,mw_count,"
                      "mw_sigma_m,bia_corrected,col1,col2,lambda1,lambda2,lambda_wl,"
                      "x_lin1_m,x_lin2_m,xr1_m,xr2_m,n1_abs_cyc,n2_abs_cyc,wl_arc_kf_cyc\n";
                for (std::size_t r = 0; r < mw_wl_f6.rows.size(); ++r) {
                    const auto& fr = mw_wl_f6.rows[r];
                    for (std::size_t k = 0; k < fr.arcs.size(); ++k) {
                        const auto& a = fr.arcs[k];
                        const auto it = xr_of.find(a.est_station);
                        double xr1 = 0.0, xr2 = 0.0; bool have_xr = false;
                        if (it != xr_of.end()
                            && a.col1 >= 0 && static_cast<Eigen::Index>(a.col1) < it->second.size()
                            && a.col2 >= 0 && static_cast<Eigen::Index>(a.col2) < it->second.size()) {
                            xr1 = it->second(a.col1); xr2 = it->second(a.col2); have_xr = true;
                        }
                        const double n1abs = (a.lambda1 > 0.0) ? (a.x_lin1 + xr1) / a.lambda1 : 0.0;
                        const double n2abs = (a.lambda2 > 0.0) ? (a.x_lin2 + xr2) / a.lambda2 : 0.0;
                        const double mw_arc_cyc = (a.lambda_wl > 0.0) ? a.mw_mean_m / a.lambda_wl : 0.0;
                        f6 << r << ',' << k << ',' << a.est_station << ','
                           << a.sat << ',' << a.sig1 << ',' << a.sig2 << ','
                           << fmt::format("{:.1f}", a.coeff) << ','
                           << fmt::format("{:.4f}", a.mw_mean_m) << ','
                           << fmt::format("{:.4f}", mw_arc_cyc) << ','
                           << a.mw_count << ','
                           << fmt::format("{:.4f}", std::sqrt(std::max(a.mw_var_m2, 0.0))) << ','
                           << (a.bia_corrected ? 1 : 0) << ','
                           << a.col1 << ',' << a.col2 << ','
                           << fmt::format("{:.6f}", a.lambda1) << ','
                           << fmt::format("{:.6f}", a.lambda2) << ','
                           << fmt::format("{:.6f}", a.lambda_wl) << ','
                           << fmt::format("{:.4f}", a.x_lin1) << ','
                           << fmt::format("{:.4f}", a.x_lin2) << ','
                           << (have_xr ? fmt::format("{:.4f}", xr1) : std::string("nan")) << ','
                           << (have_xr ? fmt::format("{:.4f}", xr2) : std::string("nan")) << ','
                           << fmt::format("{:.4f}", n1abs) << ','
                           << fmt::format("{:.4f}", n2abs) << ','
                           << fmt::format("{:.4f}", n1abs - n2abs) << '\n';
                    }
                }
                mw_wl_f6.written = true;
                mw_wl_f6.pending = false;
                log->info("Network (M5/WL-1 F6): wrote {} — first arming epoch tow {:.1f}, "
                          "{} row(s) over {} WL block(s); attribute z_mw_frac (MW "
                          "integer-ness) + recon_minus_zmw (state-frame gap).  ⚠️ This is a "
                          "capped SAMPLE (network.mw_wl.f6_max_rows={}, f6_all_blocks={}) — "
                          "its size is NOT the WL population; read the run-end WL-1 counters "
                          "for that.",
                          (log_dir_ / "network_mw_wl_f6.csv").string(), mw_wl_f6.tow,
                          mw_wl_f6.rows.size(),
                          std::set<std::size_t>(mw_wl_f6.row_block.begin(),
                                                mw_wl_f6.row_block.end()).size(),
                          config_.getNetworkConfig().mw_wl.f6_max_rows,
                          config_.getNetworkConfig().mw_wl.f6_all_blocks);
            }
        }
    }
    // M5 card WL-2 EWL lane: same accepted-install accumulation (invariant #4),
    // per lane instance (UWL-1: one per tabled constellation when armed).
    if (ewl_on) {
        for (auto& lane : ewl_lanes) {
            lane.run.cycles_seen       += lane.epoch.cycles_seen;
            lane.run.rows_built        += lane.epoch.rows_built;
            lane.run.rows_fixed        += lane.epoch.rows_fixed;
            lane.run.rows_fixed_cold   += lane.epoch.rows_fixed_cold;
            lane.run.rows_cold_unfixed += lane.epoch.rows_cold_unfixed;
            lane.run.rows_capped       += lane.epoch.rows_capped;
            lane.run.skip_not_primary  += lane.epoch.skip_not_primary;
            lane.run.skip_missing_band += lane.epoch.skip_missing_band;
            lane.run.skip_missing_mw   += lane.epoch.skip_missing_mw;
            lane.run.skip_young_arc    += lane.epoch.skip_young_arc;
            lane.run.skip_bad_lambda   += lane.epoch.skip_bad_lambda;
            lane.run.skip_mixed_frame  += lane.epoch.skip_mixed_frame;
            lane.run.skip_quarantine   += lane.epoch.skip_quarantine;
            lane.run.sum_R             += lane.epoch.sum_R;
        }
    }
    // M5 card WL-2 WL lane (§3.3): apply the stashed inject plan against the FRESH
    // posterior (this epoch's NL), THEN fold the run counters.  The plan was decided
    // from DETECTOR statistics (§3.0.1) in the accepted build; here we read the carried
    // lane value + NL sum and, only if the carry still disagrees with the WL integer by
    // more than inject_disagree_min_cyc (self-limiting), set the lane difference to the
    // integer while preserving the phase-determined sum.  injectCarriedChord marks each
    // moved chord D5-exempt for the NEXT install (the §3.3 storm guard) internally.
    if (wl_inject_on || lane_fix_on) {
        for (const auto& pl : wl_inject_epoch) {
            double z1 = 0.0, v1 = 0.0, z2 = 0.0, v2 = 0.0;
            if (pl.pair
                    ? !carry_driver.carriedPairValue(pl.group_l1, pl.chord, z1, v1, z2, v2)
                    : (!carry_driver.carriedChordValue(pl.group_l1, pl.chord, z1, v1) ||
                       !carry_driver.carriedChordValue(pl.group_l2, pl.chord, z2, v2))) {
                ++wl_run.skip_basis;   // not 1:1 across BOTH bands' carry bases — skip (§3.3)
                if (uwl_on) ++wl_cons_run[pl.cons ? pl.cons : '?'].skip_basis;
                continue;
            }
            const double A1 = (z1 + pl.z_lin1) / pl.lambda1;   // absolute band DDs (cyc)
            const double A2 = (z2 + pl.z_lin2) / pl.lambda2;
            const double lane = A1 - A2, s = A1 + A2;          // WL DD + preserved NL sum
            if (std::abs(pl.w - lane) <= nmw.inject_disagree_min_cyc)
                continue;   // already lane-consistent (hold/fix row / prior inject) ⇒ no fight
            const double A1p = 0.5 * (pl.w + s), A2p = 0.5 * (s - pl.w);
            const double sg1 = nmw.wl_fix_sigma_cyc * pl.lambda1;
            const double sg2 = nmw.wl_fix_sigma_cyc * pl.lambda2;
            bool moved = false;
            if (pl.pair) {
                // WL-3 fallback: one joint edit, both bands, one D5 exemption (§3.3
                // "should fire ~never in steady state — telemetry proves it").
                moved = carry_driver.injectCarriedChordPair(
                    pl.group_l1, pl.chord,
                    A1p * pl.lambda1 - pl.z_lin1, sg1 * sg1,
                    A2p * pl.lambda2 - pl.z_lin2, sg2 * sg2);
            } else {
                const bool i1 = carry_driver.injectCarriedChord(
                    pl.group_l1, pl.chord, A1p * pl.lambda1 - pl.z_lin1, sg1 * sg1);
                const bool i2 = carry_driver.injectCarriedChord(
                    pl.group_l2, pl.chord, A2p * pl.lambda2 - pl.z_lin2, sg2 * sg2);
                moved = i1 && i2;
            }
            if (moved) { ++wl_run.injects; wl_run.inject_abs_sum += std::abs(pl.w - lane);
                         if (uwl_on) {
                             auto& wcr = wl_cons_run[pl.cons ? pl.cons : '?'];
                             ++wcr.injects; wcr.inject_abs_sum += std::abs(pl.w - lane);
                         }
                         if (mw_tagsets) d5_inject_chords_now.insert(pl.chord); }
            else       { ++wl_run.skip_basis;
                         if (uwl_on) ++wl_cons_run[pl.cons ? pl.cons : '?'].skip_basis; }
        }
        // Accepted-install accumulation: the pooled epoch tallies (also the whole walk
        // when the UWL card is off) plus, under UWL-1, the per-cons walk tallies —
        // folded into BOTH the aggregate and the per-cons run totals.
        wlAccumulate(wl_run, wl_epoch);
        for (const auto& [wcons, we] : wl_cons_epoch) {
            wlAccumulate(wl_run, we);
            wlAccumulate(wl_cons_run[wcons], we);
        }

        // F-B lane trace: write every entry's POST-install carried lane + tags.
        if (mw_lane_trace && !lane_trace_epoch.empty()) {
            std::map<ArcKey, double> w_of;   // decided cycles' integer targets
            for (const auto& pl : wl_inject_epoch) w_of[pl.chord] = pl.w;
            std::error_code ec_lt;
            std::filesystem::create_directories(log_dir_, ec_lt);
            std::ofstream ltf(log_dir_ / "network_wl_lane_trace.csv", std::ios::app);
            if (ltf) {
                if (!lane_trace_header_written && ltf.tellp() == std::streampos(0))
                    ltf << "week,tow,chord_u,chord_v,station,sat,lane_cyc,frac_cyc,"
                           "w,decided,held,injected\n";
                lane_trace_header_written = true;
                for (const auto& e : lane_trace_epoch) {
                    double z1 = 0.0, v1 = 0.0, z2 = 0.0, v2 = 0.0;
                    if (lane_on
                            ? !carry_driver.carriedPairValue(e.g1, e.chord, z1, v1, z2, v2)
                            : (!carry_driver.carriedChordValue(e.g1, e.chord, z1, v1) ||
                               !carry_driver.carriedChordValue(e.g2, e.chord, z2, v2)))
                        continue;   // not 1:1 carried in both bands this epoch
                    const double lane = (z1 + e.z_lin1) / e.lam1
                                      - (z2 + e.z_lin2) / e.lam2;
                    const double frac = lane - std::round(lane);
                    const auto wit = w_of.find(e.chord);
                    const std::string sta_name =
                        (e.chord.u >= 0 && e.chord.u < static_cast<int>(stations.size()))
                            ? stations[static_cast<std::size_t>(e.chord.u)].name
                            : std::string{};
                    ltf << se.time.week << ',' << fmt::format("{:.1f}", se.time.tow)
                        << ',' << e.chord.u << ',' << e.chord.v << ',' << sta_name
                        << ',' << e.sat << ',' << fmt::format("{:.4f}", lane) << ','
                        << fmt::format("{:.4f}", frac) << ',';
                    if (wit != w_of.end()) ltf << fmt::format("{:.0f}", wit->second);
                    ltf << ',' << (wit != w_of.end() ? 1 : 0) << ','
                        << (d5_hold_chords.count(e.chord) ? 1 : 0) << ','
                        << (d5_inject_chords_now.count(e.chord) ? 1 : 0) << '\n';
                }
            }
        }
    }
    const auto& cc = carry_driver.counters();
    carry_epochs       = cc.epochs;
    carry_born_total   = cc.chords_born;
    carry_marg_total   = cc.chords_marginalized;
    carry_merges_total = cc.merges;
    carry_splits_total = cc.splits;
    carry_swaps_total  = cc.tree_swaps;
    carry_zdim_last    = cc.z_dim;
    carry_slipkills_total = cc.slip_kills;
    carry_dropped_total   = cc.chords_dropped;
    carry_innov_flags_total  = cc.innov_flags;
    carry_innov_retaps_total = cc.innov_retaps;
    // P1-1 (SPEC_day129_frame_guards): difference the RUN-CUMULATIVE driver
    // counters into THIS epoch's churn, for the per-epoch trace columns.  Taken
    // here — the one site that owns the cumulative copy — so the deltas can never
    // disagree with the run-end totals.  Pure trace; nothing reads them back.
    ctx.carry.ep_chord_births  = cc.chords_born  - ctx.carry.carry_prev_born;
    ctx.carry.ep_slip_refloats = cc.slip_kills   - ctx.carry.carry_prev_slipkills;
    ctx.carry.ep_d5_flags      = cc.innov_flags  - ctx.carry.carry_prev_innov_flags;
    ctx.carry.ep_d5_retaps     = cc.innov_retaps - ctx.carry.carry_prev_innov_retaps;
    ctx.carry.carry_prev_born           = cc.chords_born;
    ctx.carry.carry_prev_slipkills      = cc.slip_kills;
    ctx.carry.carry_prev_innov_flags    = cc.innov_flags;
    ctx.carry.carry_prev_innov_retaps   = cc.innov_retaps;
    // D5: fold the post-solve innovation flags into the NEXT epoch's slip funnel.
    // Each (group, chord) maps back to its (station, ambiguity) via the reverse map
    // built with this epoch's blocks; next epoch's buildCarryDdPriors excludes it from
    // the A_old membership so the affected cycle re-taps fresh (the C3c path).
    if (carry_innov_gate) {
        for (const auto& [gk, chord] : carry_driver.lastInnovFlags()) {
            const auto git = carry_arc_amb_key.find(gk);
            const auto ait = (git != carry_arc_amb_key.end())
                                 ? git->second.find(chord) : std::map<ArcKey, std::string>::const_iterator{};
            if (git == carry_arc_amb_key.end() || ait == git->second.end()) {
                // A flagged chord that the reverse map (built with THIS epoch's blocks)
                // cannot resolve should never happen — both come from the same
                // buildCarryDdPriors.  Warn once so a wiring regression is not silent;
                // the flag is dropped (safe: q_z + next-epoch detection are the backstop).
                if (++carry_innov_map_miss == 1)
                    log->warn("Network[Card4 D5]: an innovation flag (group {}, arc {}→{}) did "
                              "not resolve to an ambiguity in the reverse map at TOW {:.1f} — "
                              "the next-epoch re-tap is skipped for it (reverse-map/block "
                              "mismatch; investigate if this recurs).",
                              gk, chord.u, chord.v, se.time.tow);
                continue;
            }
            // The insert RESULT is the de-duplication signal the attribution
            // needs: carry_innov_pending was cleared at this epoch's head
            // (the C3c drain), so `.second` is true exactly once per
            // (station, ambiguity) per epoch.  Under the WL-3 joint pair
            // block the gate flags per band ROW, so the same (group, chord)
            // arrives twice and only the first insert is a real re-tap.
            const bool first_this_epoch =
                carry_innov_pending.insert({chord.u, ait->second}).second;
            // Card G(c): make the D5 re-tap counter ATTRIBUTABLE.  The audit's
            // 1 560 re-taps were un-decomposable — a raw-step coincidence test
            // at the same (station, satellite, epoch) separates "the gate caught
            // a real observable event" from "the gate is firing on model
            // tension".  Instrumentation only; the flag is unchanged either way.
            // Both units are counted: FLAG ROWS (continuity with the 2026-07-21
            // headline) and distinct funnel ARCS (what actually re-taps).
            if (nc.slip_qc.gap_instrument) {
                ++slip_qc.counters.d5_retaps;
                if (first_this_epoch) ++slip_qc.counters.d5_retap_ambs;
                if (const auto pk = StateKeys::parseAmbiguityKey(ait->second))
                    if (slip_qc.epoch.stepped.count({chord.u, pk->satellite})) {
                        ++slip_qc.counters.d5_retaps_stepped;
                        if (first_this_epoch) ++slip_qc.counters.d5_retap_ambs_stepped;
                    }
            }
            // WL-3 §3.4: a pair chord's flag refloats BOTH bands next epoch (they
            // share the graph edge) — feed the band-2 ambiguity into the funnel too.
            if (lane_on) {
                const auto g2it = carry_arc_amb_key2.find(gk);
                if (g2it != carry_arc_amb_key2.end()) {
                    const auto a2it = g2it->second.find(chord);
                    if (a2it != g2it->second.end())
                        carry_innov_pending.insert({chord.u, a2it->second});
                }
            }
        }
    }
    // F-A attribution dump (log-only, network.mw_wl.d5_dump): one CSV row per D5
    // flag with adjacency tags.  Uses the SAME reverse map as the funnel above.
    //
    // Card D5-A/D2 (docs/SPEC_slip_qc_followups.md §1): the trailing
    // `raw_stepped` column is the CHEAP EXACT-EPOCH raw-step tag — did the
    // receiver's own MW/GF level step at this (station, satellite) on this
    // epoch?  It is deliberately TRI-STATE, not 0/1:
    //   1     — this (station, sat) is in slip_qc.epoch.stepped (Card G's
    //           empirical per-station gate fired);
    //   0     — a step COULD be formed (adjacent-epoch dual-band predecessor)
    //           and it was flat;
    //   blank — no evidence at all (raw observables not computed this run, arc
    //           start / gap / single-band, or the flag's ambiguity key did not
    //           resolve).  Writing 0 there would be a false "no raw step"
    //           claim on exactly the population D5-A exists to decompose;
    //           absence of evidence is not evidence of absence.
    // The ±1-epoch offline join in scripts/grade_slip_agreement.py --d5 stays
    // the source of truth; this column is the zero-cost exact-epoch tag that
    // makes network_d5_flags.csv self-contained.  Log-only either way: the
    // solve never reads it.
    if (mw_d5_dump && carry_innov_gate) {
        const auto& flags  = carry_driver.lastInnovFlags();
        const auto& sigmas = carry_driver.lastInnovFlagSigmas();
        if (!flags.empty()) {
            std::error_code ec_d5;
            std::filesystem::create_directories(log_dir_, ec_d5);
            std::ofstream d5f(log_dir_ / "network_d5_flags.csv", std::ios::app);
            if (d5f) {
                if (!d5_dump_header_written && d5f.tellp() == std::streampos(0))
                    d5f << "week,tow,group,chord_u,chord_v,station,amb_key,std_innov,"
                           "ewl_stasat,hold_chord,injected_prev,raw_stepped\n";
                d5_dump_header_written = true;
                for (std::size_t fi = 0; fi < flags.size(); ++fi) {
                    const auto& [gk, chord] = flags[fi];
                    std::string amb, sat;
                    const auto git = carry_arc_amb_key.find(gk);
                    if (git != carry_arc_amb_key.end()) {
                        const auto ait = git->second.find(chord);
                        if (ait != git->second.end()) amb = ait->second;
                    }
                    if (!amb.empty())
                        if (const auto parts = StateKeys::parseAmbiguityKey(amb))
                            sat = parts->satellite;
                    const std::string sta_name =
                        (chord.u >= 0 && chord.u < static_cast<int>(stations.size()))
                            ? stations[static_cast<std::size_t>(chord.u)].name
                            : std::string{};
                    // D2 tri-state raw-step tag (see the banner above).  Reads
                    // the SAME per-epoch evidence the run-end d5_retaps_stepped
                    // counter uses, so the CSV and the counter cannot disagree.
                    std::string raw_stepped;
                    if (slip_qc.epoch.computed && !sat.empty() && chord.u >= 0
                        && static_cast<std::size_t>(chord.u)
                               < slip_qc.epoch.verdict.size()) {
                        if (slip_qc.epoch.stepped.count({ chord.u, sat })) {
                            raw_stepped = "1";
                        } else {
                            const auto& vmap =
                                slip_qc.epoch.verdict[static_cast<std::size_t>(chord.u)];
                            const auto vit = vmap.find(sat);
                            if (vit != vmap.end() && vit->second.measured)
                                raw_stepped = "0";
                        }
                    }
                    d5f << se.time.week << ',' << fmt::format("{:.1f}", se.time.tow)
                        << ',' << gk << ',' << chord.u << ',' << chord.v << ','
                        << sta_name << ',' << amb << ','
                        << fmt::format("{:.2f}",
                               fi < sigmas.size() ? sigmas[fi] : 0.0) << ','
                        << (d5_ewl_stasat.count({ chord.u, sat }) ? 1 : 0) << ','
                        << (d5_hold_chords.count(chord) ? 1 : 0) << ','
                        << (d5_inject_chords_prev.count(chord) ? 1 : 0) << ','
                        << raw_stepped << '\n';
                }
            }
        }
    }
    if (mw_tagsets) {   // rotate: this epoch's applied injects tag NEXT install's flags
        d5_inject_chords_prev.swap(d5_inject_chords_now);
        d5_inject_chords_now.clear();
    }
}

} // namespace GnssProcessor
