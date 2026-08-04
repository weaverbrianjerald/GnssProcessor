/**
 * @file NetworkShHealth.cpp
 * @brief Implementation of @ref networkShHealth — the SB_T1A block relocated VERBATIM
 *        (dedent-4, zero substitution) out of Application::runNetwork at INC-11b.
 */

#include "filter/NetworkShHealth.hpp"

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "common/Constants.hpp"
#include "common/PppUtils.hpp"
#include "filter/NetworkCarryVce.hpp"
#include "filter/NetworkIonoDeweightGate.hpp"
#include "filter/NetworkPivotHandover.hpp"
#include "filter/NetworkRhsAttrib.hpp"   // INC-5 flight-recorder trip latch
#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "math/Coordinates.hpp"

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

void networkShHealth(
    NetworkRunContext& ctx,
    const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkRunTraces& traces,
    NetworkSetupResult& setup,
    const SyncedEpoch& se,
    NetworkEstimator& est,
    const Eigen::VectorXd& x_s,
    const std::vector<StationAssembly>& asm_res,
    const std::vector<int>& est_index_of,
    const std::vector<std::vector<PppObservation>>& modeled,
    NetworkIonoDeweightGate& iono_deweight,
    NetworkCarryVce& carry_vce_est,
    bool datum_changed,
    double pl_now_s,
    double& first_vce_s,
    int& last_vce_hour) {
    // --- Alias preamble: mechanically-derived run-TU aliases (+ reconstructions). --
    auto& sh_model = setup.sh_model;
    auto& sh_model2 = setup.sh_model2;   // Iono-2 dual-layer second shell (null unless armed)
    auto& stations = setup.stations;
    auto& vtec_sh_health_trace = traces.vtec_sh_health_trace;
    auto& carry_vce_trace      = traces.carry_vce_trace;
    const double pl_var_floor  = ctx.knobs.pl_var_floor;  // lock-in guard; 0 = off
    const double pb_var_floor  = ctx.knobs.pb_var_floor;  // B4: D2 phase-bias carry floor; 0 = off
    const double cb_var_floor  = ctx.knobs.cb_var_floor;  // B4: D2 code-bias carry floor;  0 = off
    const bool   carry_vce     = ctx.knobs.carry_vce;
    auto& n_emitted = ctx.counters.n_emitted;
    auto& sh_a00_was_in_band = ctx.carry.sh_a00_was_in_band;
    const GlobalVtecShConfig& shc = config_.getGlobalVtecShConfig();
    // P1-2: engage/release thresholds, echoed into the IONO_DW event rows so a
    // reader sees the z-score against the gate it was judged by.
    const IonoConstraintDeweightConfig& idw_cfg = config_.getIonoConstraintDeweightConfig();


    // --- T1a: SH-field health probe + a₀₀ physical-band divergence WARN ---
    // (a) Always-on a₀₀ band WARN — fires whenever the SH field is active, so a
    //     diverging mean VTEC is visible even without the trace armed.  a₀₀ IS the
    //     mean VTEC (TECU) because P̄₀₀ ≡ 1; the SH↔slant misfit that drives the
    //     divergence is otherwise absorbed silently into the ambiguities.  Read-only.
    if (sh_model && shc.a00_warn_low_tecu < shc.a00_warn_high_tecu) {
        double a00 = std::numeric_limits<double>::quiet_NaN();
        try { a00 = x_s(est.vtecCoeffColumn(0, 0, 'C')); } catch (...) {}
        // Dual-layer: the physical band bounds the TOTAL mean VTEC — the
        // layer SUM (the split is separately anchored; its health is the
        // a00_l2 column of the health trace, not this WARN).
        if (sh_model2 && std::isfinite(a00)) {
            try { a00 += x_s(est.vtecCoeffColumn(0, 0, 'C', 1)); } catch (...) {}
        }
        if (std::isfinite(a00)) {
            const bool in_band =
                (a00 >= shc.a00_warn_low_tecu && a00 <= shc.a00_warn_high_tecu);
            // INC-5 flight-recorder trigger
            // (docs/SPEC_prefit_census_observability.md §7): the a00-BAND era.
            // Distinct from the SH GATE era above it in the pipeline — the WARN
            // fires on the FIRST out-of-band epoch while the gate waits
            // sh_gate_min_epochs, so this is the earliest of the four triggers
            // and the one most likely to catch a discharge in progress.
            // SB_T1A precedes SB_FRAMEDIAG, so the trip lands this same epoch.
            networkRhsTripEdge(ctx, "a00_band", !in_band);
            if (!in_band && (sh_a00_was_in_band || n_emitted % 60 == 0)) {
                log->warn("Network (SH-iono): a₀₀ = {:.1f} TECU OUTSIDE physical band "
                          "[{:.1f}, {:.1f}] — global VTEC field diverging (TOW {:.1f}); the "
                          "SH↔slant misfit is being squeezed into the ambiguities.",
                          a00, shc.a00_warn_low_tecu, shc.a00_warn_high_tecu, se.time.tow);
            } else if (in_band && !sh_a00_was_in_band) {
                log->info("Network (SH-iono): a₀₀ = {:.1f} TECU returned to physical band "
                          "(TOW {:.1f}).", a00, se.time.tow);
            }
            sh_a00_was_in_band = in_band;
        }
    }

    // (b) Full health metrics + station-adaptive deweight gate — ONE post-solve
    //     SH↔slant sweep feeding both consumers (H1 dedup).
    //     The SH health trace (shc.trace) writes one row per emitted epoch: a₀₀
    //     (+σ), coefficient RMS by degree, that RMS relative to the per-degree Kaula
    //     prior σ (σ_n = kaula/n, σ₀ = a₀₀ prior σ), and the network-wide SH→slant
    //     misfit reconstructed at POSTERIOR geometry (mean/rms/max |residual| +
    //     3σ-exceedance counts over LOS and stations).
    //     The station-adaptive deweight (v2) needs, per present station, the SAME
    //     standardized slant residual r/σ — its per-station RMS √mean((r/σ)²) is the
    //     gate's observe() input.  Both are derived from ONE reconstruct + degree-15
    //     SH evaluation per LOS: the gate accumulator is within-station (order-free),
    //     so folding it in is byte-identical to the old separate loop.  Gate-off (or
    //     a datum-change epoch) ⇒ no observe()/advance() ⇒ byte-identical; and with
    //     the trace on (the lineage recipe) the gate now rides the trace's existing
    //     sweep at ~zero marginal cov-consumer cost (the +303 reconstructStation
    //     calls/ep and duplicate SH sweep are gone).
    const bool sh_want_trace = vtec_sh_health_trace.is_open();
    const bool sh_want_gate  = iono_deweight.adaptive() && !datum_changed;
    if (sh_model && (sh_want_trace || sh_want_gate)) {
        const Eigen::MatrixXd& shcov = est.sharedCovariance();
        const auto& coeffs = sh_model->coeffs();
        Eigen::VectorXd coeff(static_cast<Eigen::Index>(coeffs.size()));
        std::vector<Eigen::Index> ccol(coeffs.size(), -1);
        bool coeff_ok = true;
        for (std::size_t k = 0; k < coeffs.size() && coeff_ok; ++k) {
            try {
                ccol[k] = est.vtecCoeffColumn(coeffs[k].n, coeffs[k].m, coeffs[k].trig);
                coeff(static_cast<Eigen::Index>(k)) = x_s(ccol[k]);
            } catch (...) { coeff_ok = false; }
        }
        // Iono-2 dual-layer: the second shell's solved coefficients, for the
        // misfit-vs-SUM sweep + the armed-only health columns.
        Eigen::VectorXd coeff2;
        double a00_l2 = 0.0;
        std::vector<double> deg2_sumsq;
        std::vector<int>    deg2_cnt;
        if (coeff_ok && sh_model2) {
            const auto& coeffs2 = sh_model2->coeffs();
            coeff2.resize(static_cast<Eigen::Index>(coeffs2.size()));
            deg2_sumsq.assign(static_cast<std::size_t>(sh_model2->maxDegree() + 1), 0.0);
            deg2_cnt.assign(static_cast<std::size_t>(sh_model2->maxDegree() + 1), 0);
            for (std::size_t k = 0; k < coeffs2.size() && coeff_ok; ++k) {
                try {
                    const Eigen::Index col =
                        est.vtecCoeffColumn(coeffs2[k].n, coeffs2[k].m, coeffs2[k].trig, 1);
                    const double val = x_s(col);
                    coeff2(static_cast<Eigen::Index>(k)) = val;
                    deg2_sumsq[static_cast<std::size_t>(coeffs2[k].n)] += val * val;
                    ++deg2_cnt[static_cast<std::size_t>(coeffs2[k].n)];
                    if (coeffs2[k].n == 0 && coeffs2[k].m == 0 && coeffs2[k].trig == 'C')
                        a00_l2 = val;
                } catch (...) { coeff_ok = false; }
            }
        }
        if (coeff_ok) {
            const int N = sh_model->maxDegree();
            double a00 = 0.0, a00_std = 0.0, all_sumsq = 0.0;
            int all_cnt = 0;
            std::vector<double> deg_sumsq(static_cast<std::size_t>(N + 1), 0.0);
            std::vector<int>    deg_cnt(static_cast<std::size_t>(N + 1), 0);
            for (std::size_t k = 0; k < coeffs.size(); ++k) {
                const std::size_t n = static_cast<std::size_t>(coeffs[k].n);
                const double val = coeff(static_cast<Eigen::Index>(k));
                deg_sumsq[n] += val * val;
                ++deg_cnt[n];
                all_sumsq += val * val;
                ++all_cnt;
                if (coeffs[k].n == 0 && coeffs[k].m == 0 && coeffs[k].trig == 'C') {
                    a00 = val;
                    const Eigen::Index col = ccol[k];
                    const double vv = (col >= 0 && col < shcov.rows() && col < shcov.cols())
                                          ? shcov(col, col) : 0.0;
                    a00_std = (vv > 0.0) ? std::sqrt(vv) : 0.0;
                }
            }
            const double rms_all = (all_cnt > 0) ? std::sqrt(all_sumsq / all_cnt) : 0.0;

            // Network-wide SH→slant misfit at posterior geometry (mirrors the
            // producer's own SH row math in appendShIonoRows, but evaluated on the
            // solved coefficients + reconstructed per-station slant states).
            const auto& iw = config_.getIonosphereWeightingConfig();
            const double Re = shc.earth_radius_km * 1000.0;
            const double H  = shc.layer_height_km * 1000.0;
            const double tecu_to_L1m =
                40.3 * 1.0e16 / (Constants::GPS_L1_FREQ * Constants::GPS_L1_FREQ);
            const double lon_sun = shc.sun_fixed ? meanSunLongitude(se.time) : 0.0;
            double res_sum = 0.0, res_sumsq = 0.0, res_max = 0.0;
            int n_los = 0, n_los_gt3 = 0, n_sta = 0, n_sta_gt3 = 0;
            for (std::size_t i = 0; i < stations.size(); ++i) {
                if (est_index_of[i] < 0) continue;
                Eigen::VectorXd x_r;
                try {
                    x_r = est.reconstructStation(
                        static_cast<std::size_t>(est_index_of[i]), x_s);
                } catch (...) { continue; }
                std::map<std::string, std::pair<double, Eigen::Vector3d>> geom;
                for (const auto& o : modeled[i])
                    if (!o.satellite.empty() && o.sat_pos.allFinite())
                        geom.emplace(o.satellite,
                                     std::make_pair(o.elevation, o.sat_pos));
                const auto& h2l = asm_res[i].h2local;
                bool sta_present = false, sta_gt3 = false;
                double zz_gate = 0.0; int nn_gate = 0;   // per-station Σ(r/σ)², count (deweight gate)
                for (const auto& kv : stations[i].sm->getIonosphereStates()) {
                    if (kv.first.empty() || kv.first.front() == 'R') continue;  // SH excludes GLONASS
                    const auto gt = geom.find(kv.first);
                    if (gt == geom.end()) continue;
                    if (gt->second.first < iw.elevation_cutoff_deg) continue;
                    const auto hit = h2l.find(static_cast<int>(kv.second));
                    if (hit == h2l.end() || hit->second < 0
                        || static_cast<Eigen::Index>(hit->second) >= x_r.size()) continue;
                    const IppGeometry g =
                        computeIppGeometry(stations[i].ecef, gt->second.second, Re, H);
                    if (!g.valid) continue;
                    const double lon_sf = g.ipp_lon_rad - lon_sun;
                    double sh_slant =
                        sh_model->vtec(coeff, g.ipp_lat_rad, lon_sf) * g.mapping * tecu_to_L1m;
                    // Dual-layer: the misfit is against the layer SUM — the
                    // second shell at its OWN pierce geometry.
                    if (sh_model2) {
                        const IppGeometry g2 = computeIppGeometry(
                            stations[i].ecef, gt->second.second, Re,
                            shc.layer2_height_km * 1000.0);
                        if (g2.valid)
                            sh_slant += sh_model2->vtec(coeff2, g2.ipp_lat_rad,
                                                        g2.ipp_lon_rad - lon_sun)
                                        * g2.mapping * tecu_to_L1m;
                    }
                    const double resid = x_r(hit->second) - sh_slant;
                    if (!std::isfinite(resid)) continue;
                    const double sig = std::max(
                        iw.floor_m, shc.representation_sigma_tecu * g.mapping * tecu_to_L1m);
                    // Deweight gate: this LOS's standardized residual (same sig>0
                    // guard the old separate loop used before dividing).
                    if (sh_want_gate && sig > 0.0) {
                        const double sr = resid / sig;
                        zz_gate += sr * sr; ++nn_gate;
                    }
                    if (!sh_want_trace) continue;   // gate-only: skip trace aggregates
                    const double ar = std::fabs(resid);
                    res_sum += ar;
                    res_sumsq += resid * resid;
                    if (ar > res_max) res_max = ar;
                    ++n_los;
                    sta_present = true;
                    if (sig > 0.0 && ar > 3.0 * sig) { ++n_los_gt3; sta_gt3 = true; }
                }
                if (sta_present) ++n_sta;
                if (sta_gt3) ++n_sta_gt3;
                // Feed the deweight gate this station's standardized-residual RMS
                // (serial station order — same as the old separate loop).
                if (sh_want_gate && nn_gate > 0)
                    iono_deweight.observe(stations[i].name, std::sqrt(zz_gate / nn_gate));
            }
            const double res_mean = (n_los > 0) ? res_sum / n_los : 0.0;
            const double res_rms  = (n_los > 0) ? std::sqrt(res_sumsq / n_los) : 0.0;
            const bool a00_in_band =
                (a00 >= shc.a00_warn_low_tecu && a00 <= shc.a00_warn_high_tecu);

            vtec_sh_health_trace
                << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                << fmt::format("{:.3f}", a00) << ','
                << fmt::format("{:.3f}", a00_std) << ','
                << fmt::format("{:.4f}", rms_all) << ','
                << fmt::format("{:.4f}", res_mean) << ','
                << fmt::format("{:.4f}", res_rms) << ','
                << fmt::format("{:.4f}", res_max) << ','
                << n_los << ',' << n_los_gt3 << ','
                << n_sta << ',' << n_sta_gt3 << ','
                << (a00_in_band ? 1 : 0);
            for (int n = 0; n <= N; ++n) {
                const std::size_t ni = static_cast<std::size_t>(n);
                const double rms = (deg_cnt[ni] > 0)
                    ? std::sqrt(deg_sumsq[ni] / deg_cnt[ni]) : 0.0;
                vtec_sh_health_trace << ',' << fmt::format("{:.4f}", rms);
            }
            for (int n = 0; n <= N; ++n) {
                const std::size_t ni = static_cast<std::size_t>(n);
                const double rms = (deg_cnt[ni] > 0)
                    ? std::sqrt(deg_sumsq[ni] / deg_cnt[ni]) : 0.0;
                const double ksig = (n == 0)
                    ? shc.mean_vtec_prior_sigma_tecu
                    : shc.kaula_scale_tecu / static_cast<double>(n);
                vtec_sh_health_trace
                    << ',' << fmt::format("{:.4f}", (ksig > 0.0) ? rms / ksig : 0.0);
            }
            // Iono-2 dual-layer (armed-only columns, matching the header):
            // the split anchor's health — layer-2 a₀₀ + per-degree RMS.
            if (sh_model2) {
                vtec_sh_health_trace << ',' << fmt::format("{:.3f}", a00_l2);
                for (int n = 0; n <= sh_model2->maxDegree(); ++n) {
                    const std::size_t ni = static_cast<std::size_t>(n);
                    const double rms = (deg2_cnt[ni] > 0)
                        ? std::sqrt(deg2_sumsq[ni] / deg2_cnt[ni]) : 0.0;
                    vtec_sh_health_trace << ',' << fmt::format("{:.4f}", rms);
                }
            }
            vtec_sh_health_trace << '\n';
            // Advance the deweight gate one clean epoch (EWMA + robust z +
            // hysteresis + slew).  Gate-off ⇒ never runs ⇒ byte-identical.
            if (sh_want_gate) {
                iono_deweight.advance(true);
                // --- SPEC_day129_frame_guards P1-2 -------------------------
                // This is the ONE serial site the gate's threading contract
                // allows a read from (advance/observe mutate; the assembly is
                // parallel).  Harvest the weight field for the epoch-status row
                // and emit each engage/release as a discrete IONO_DW event, so
                // "did a deweight release wave land at the collapse epoch?" is
                // answerable from the traces instead of from run-END totals
                // (326 engage / 307 release over the whole day tells you
                // nothing about WHEN).  Pure trace — no decision reads it.
                const auto agg = iono_deweight.aggregate();
                ctx.carry.ep_dw_engaged = agg.n_engaged;
                ctx.carry.ep_dw_zero    = agg.n_zero;
                ctx.carry.ep_dw_min_w   = agg.min_w;
                ctx.carry.ep_dw_mean_w  = agg.mean_w;
                for (const auto& tr : iono_deweight.lastTransitions())
                    traces.emitEvent(se.time, "IONO_DW",
                                     tr.engaged ? "engage" : "release",
                                     /*sat=*/"", tr.station, tr.z,
                                     tr.engaged ? idw_cfg.sigma_hi : idw_cfg.sigma_lo,
                                     tr.engaged ? "deweight" : "restore",
                                     fmt::format("w={:.4f} n_engaged={} n_zero={}",
                                                 tr.w, agg.n_engaged, agg.n_zero));
            }
        }
    }

    // NOTE (H1 dedup): the station-adaptive SH-VTEC-constraint deweight gate
    // update USED to live here as a SECOND per-station reconstruct + degree-15
    // SH sweep, identical to the health-trace sweep above (its own comment
    // conceded "the SAME resid/σ").  It added +303 reconstructStation
    // cov-consumer calls/ep plus a duplicate SH-eval sweep.  It is now FUSED
    // into the single sweep above: observe() per station + advance() once, from
    // the same per-LOS resid/σ.  See the merged block guarded on
    // (sh_want_trace || sh_want_gate).

    // M2 (LS-VCE): advance the carry-variance monitor for this emitted epoch —
    // apply exponential forgetting and set the clean-epoch gate (a datum /
    // pivot handover contaminates every carried prior, so the whole epoch's
    // rows are dropped).  On the content-hour roll, flush the just-completed
    // hour's σ̂ for every (family, constellation) to network_carry_vce.csv.
    // All gated on carry_vce ⇒ OFF is byte-identical.
    if (carry_vce) {
        if (first_vce_s < 0.0) first_vce_s = pl_now_s;
        const int cur_hour = static_cast<int>((pl_now_s - first_vce_s) / 3600.0);
        if (cur_hour != last_vce_hour) {
            if (last_vce_hour >= 0 && carry_vce_trace.is_open()) {
                for (const auto& fc : carry_vce_est.keys()) {
                    const auto e = carry_vce_est.estimate(fc.first, fc.second);
                    const double cfg_floor = (fc.first == "pb") ? pb_var_floor
                                           : (fc.first == "cb") ? cb_var_floor
                                                                : pl_var_floor;
                    const double rrat = (cfg_floor > 0.0) ? e.robust_floor / cfg_floor : 0.0;
                    const double mrat = (cfg_floor > 0.0)
                        ? e.sigma[NetworkCarryVce::kFloor] / cfg_floor : 0.0;
                    carry_vce_trace
                        << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                        << ',' << last_vce_hour << ',' << fc.first << ',' << fc.second
                        << ',' << e.n_rows << ',' << fmt::format("{:.1f}", e.n_eff)
                        << ',' << fmt::format("{:.6e}", e.robust_floor)
                        << ',' << fmt::format("{:.6e}", e.sigma[NetworkCarryVce::kFloor])
                        << ',' << fmt::format("{:.6e}", e.std[NetworkCarryVce::kFloor])
                        << ',' << fmt::format("{:.6e}", e.sigma[NetworkCarryVce::kRate])
                        << ',' << fmt::format("{:.6e}", e.std[NetworkCarryVce::kRate])
                        << ',' << fmt::format("{:.3e}", e.cond)
                        << ',' << (e.rate_frozen ? 1 : 0)
                        << ',' << fmt::format("{:.6e}", cfg_floor)
                        << ',' << fmt::format("{:.4f}", rrat)
                        << ',' << fmt::format("{:.4f}", mrat)
                        << ',' << e.floor_clamped << ',' << e.rate_clamped << '\n';
                }
                carry_vce_trace.flush();
            }
            last_vce_hour = cur_hour;
        }
        carry_vce_est.beginEpoch(pl_now_s, !datum_changed);
    }
}

} // namespace GnssProcessor
