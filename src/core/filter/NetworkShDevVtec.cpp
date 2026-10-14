/**
 * @file NetworkShDevVtec.cpp
 * @brief Implementation of @ref networkShDevVtec: the SB_SHDEV_VTEC block, moved unchanged
 *        (dedent-4 only) out of Application::runNetwork at INC-11b.
 */

#include "filter/NetworkShDevVtec.hpp"

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "common/GnssIdentity.hpp"     // systemOfSat, ionoDelayMetresPerTecu
#include "common/PppUtils.hpp"
#include "filter/NetworkCarryVce.hpp"
#include "filter/NetworkPivotHandover.hpp"
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

// The run-end reports at the end of this file (card NOM step TD1b):
#include "app/NetworkSetupResult.hpp"
#include "filter/NetworkRunEnd.hpp"

namespace GnssProcessor {

void networkShDevVtec(
    NetworkRunContext& ctx,
    const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkRunTraces& traces,
    NetworkSetupResult& setup,
    const SyncedEpoch& se,
    NetworkEstimator& est,
    const Eigen::VectorXd& x_s,
    const NetworkSbasis::Plan& plan,
    const std::vector<StationAssembly>& asm_res,
    const std::vector<int>& est_index_of,
    const std::vector<std::vector<PppObservation>>& modeled) {
    // --- Alias preamble: mechanically-derived run-TU aliases (+ reconstructions). --
    const bool sh_dev_on = ctx.knobs.sh_dev_on;   // was a parameter (card NOM3 step EP0)
    auto& sh_model = setup.sh_model;
    auto& sh_model2 = setup.sh_model2;   // Iono-2 dual-layer second shell (null unless armed)
    auto& stations = setup.stations;
    auto& vtec_sh_trace        = traces.vtec_sh_trace;
    auto& sh_dev_carry = ctx.carry.sh_dev_carry;
    const GlobalVtecShConfig& shc = config_.getGlobalVtecShConfig();


    // --- Iono-2 / T5v2: SH-deviation carry, exact update (serial) ----------
    // The solved pin functional û = ι̂ − Σ_l SH_l(ĉ) and its exact joint
    // posterior variance V̂ = cond_var + (m_ι + b_s)ᵀ Σ_free (m_ι + b_s)
    // (one stationRegression per LOS; the bordered inverse gives
    // Cov(ι, x_s) = −Σ_free·m_ι, which folds the −2·cross term into the
    // single quadratic form: the rowRedundancy identity with a_r = e_ι,
    // a_s = −b_s) drive the closed-form GM update shDevExactUpdate().
    // The update does not re-consume the pin-regularized residual as an
    // independent floor-noise observation: that is circular, collapsing P to
    // floor² after one update and turning the deviation into a lag-1 copy.
    // The exact gain uses the predicted variance and the recovered data-only
    // information U, so a weak epoch leaves P at P_pred and information
    // accumulates at the true rate (the per-LOS iono temporal-accumulation
    // channel).  Carried in vertical TECU via the layer-0 mapping (elevation-
    // stationary).
    // Insertions happen only here (serial), never in the assembly.
    if (sh_dev_on) {
        const auto& iwh = config_.getIonosphereWeightingConfig();
        const double Re_h = shc.earth_radius_km * 1000.0;
        const double tecu_to_L1m_h =
            ionoDelayMetresPerTecu(nominalCarrierFrequencyHz(FrequencyBand::GpsL1));
        const double lon_sun_h = shc.sun_fixed ? meanSunLongitude(se.time) : 0.0;
        const std::vector<ShLayer> lay_h =
            shLayers(shc, sh_model.get(), sh_model2.get());
        bool cols_ok = !lay_h.empty();
        std::vector<std::vector<Eigen::Index>> shcols_h(lay_h.size());
        for (std::size_t l = 0; l < lay_h.size() && cols_ok; ++l) {
            const int Kl = lay_h[l].model->numCoeffs();
            shcols_h[l].resize(static_cast<std::size_t>(Kl));
            for (int k = 0; k < Kl && cols_ok; ++k) {
                const auto& c = lay_h[l].model->coeffs()[static_cast<std::size_t>(k)];
                try {
                    shcols_h[l][static_cast<std::size_t>(k)] =
                        est.vtecCoeffColumn(c.n, c.m, c.trig, lay_h[l].layer);
                } catch (...) { cols_ok = false; }
            }
        }
        if (cols_ok) {
            // Datum-as-known shared covariance, built once per epoch: the
            // pinned and ensemble-gauge columns are exactly zero there, so the
            // regression row's gauge-direction terms cancel.  Pinned
            // coefficients therefore stay in the row and are not removed
            // (Card-1e).
            const Eigen::MatrixXd Sfree =
                est.datumFrameSharedCov(plan.shared_datum);
            const double now_s = se.time.week * 604800.0 + se.time.tow;
            const double floor_m2 = iwh.floor_m * iwh.floor_m;
            std::vector<double> bv;
            Eigen::VectorXd vrow;   // b_s + m_ι in shared space
            for (std::size_t i = 0; i < stations.size(); ++i) {
                if (est_index_of[i] < 0) continue;
                Eigen::VectorXd x_r;
                try {
                    x_r = est.reconstructStation(
                        static_cast<std::size_t>(est_index_of[i]), x_s);
                } catch (...) { continue; }
                const auto& h2l = asm_res[i].h2local;
                const Eigen::VectorXd x_lin = stations[i].sm->getStateVector();
                for (const auto& o : modeled[i]) {
                    if (o.satellite.empty() || systemOfSat(o.satellite) == System::GLONASS) continue;
                    if (o.elevation < iwh.elevation_cutoff_deg) continue;
                    const int iono_idx = stations[i].sm->getIonosphereIndex(o.satellite);
                    if (iono_idx < 0 || iono_idx >= static_cast<int>(x_lin.size())) continue;
                    const auto hit = h2l.find(iono_idx);
                    if (hit == h2l.end()) continue;
                    if (hit->second < 0
                        || static_cast<Eigen::Index>(hit->second) >= x_r.size()) continue;
                    const IppGeometry g0 = computeIppGeometry(
                        stations[i].ecef, o.sat_pos, Re_h, lay_h[0].shell_height_m);
                    if (!g0.valid) continue;
                    const double m0t = g0.mapping * tecu_to_L1m_h;
                    if (!(m0t > 0.0)) continue;
                    // û: the solved posterior pin functional (slant metres),
                    // and b_s: its border weight vector across all shells.
                    double sh_hat = 0.0;
                    vrow = Eigen::VectorXd::Zero(Sfree.rows());
                    for (std::size_t l = 0; l < lay_h.size(); ++l) {
                        const IppGeometry gl = (l == 0)
                            ? g0
                            : computeIppGeometry(stations[i].ecef, o.sat_pos, Re_h,
                                                 lay_h[l].shell_height_m);
                        if (!gl.valid) continue;
                        lay_h[l].model->basis(gl.ipp_lat_rad,
                                              gl.ipp_lon_rad - lon_sun_h, bv);
                        const double mlt = gl.mapping * tecu_to_L1m_h;
                        for (std::size_t j = 0; j < bv.size(); ++j) {
                            const Eigen::Index cj = shcols_h[l][j];
                            sh_hat   += mlt * bv[j] * x_s(cj);
                            vrow(cj) += mlt * bv[j];
                        }
                    }
                    const double iono_hat = x_lin(iono_idx) + x_r(hit->second);
                    const double u_hat = iono_hat - sh_hat;
                    NetworkNormalEquations::StationRegression reg;
                    try {
                        reg = est.stationRegression(
                            static_cast<std::size_t>(est_index_of[i]),
                            static_cast<Eigen::Index>(hit->second));
                    } catch (...) { continue; }
                    if (reg.row.size() == vrow.size()) vrow += reg.row;
                    const double v_hat = reg.cond_var + vrow.dot(Sfree * vrow);
                    ShDevCarry& dc =
                        sh_dev_carry[stations[i].name + "|" + o.satellite];
                    dc.var_ss = shc.representation_sigma_tecu
                                * shc.representation_sigma_tecu;
                    if (dc.t_s <= 0.0) {          // cold entry: seed at steady state
                        dc.dev = 0.0;
                        dc.var = dc.var_ss;
                        ++ctx.carry.shdev_cold_total;
                    } else {
                        ++ctx.carry.shdev_warm_total;
                    }
                    // Convert (û, V̂, effective floor) into the carry's
                    // vertical-TECU frame; then the exact closed form.
                    const double u_v  = u_hat / m0t;
                    const double v_v  = v_hat / (m0t * m0t);
                    const double f2_v = floor_m2 / (m0t * m0t);
                    if (v_v >= dc.var + f2_v) ++ctx.carry.shdev_nodata_total;
                    const auto upd =
                        shDevExactUpdate(dc.dev, dc.var, u_v, v_v, f2_v);
                    dc.dev = upd.first;
                    dc.var = upd.second;
                    dc.t_s = now_s;
                }
            }
        }
    }

    // --- Co-estimated SH-VTEC coefficient trace -------------------------
    // Dual-layer (Iono-2): with the second shell armed the rows carry a
    // `layer` column (the header matches, NetworkRunTraces::openAll); without
    // it the rows have no layer column and are byte-identical to the
    // single-layer format.  Written whenever the trace is open and an SH model
    // exists, independent of sh_dev_on.
    if (vtec_sh_trace.is_open() && sh_model) {
        const Eigen::MatrixXd& shcov = est.sharedCovariance();
        const bool lay_col = shc.layer2_max_degree > 0;
        for (const ShLayer& lay : shLayers(shc, sh_model.get(), sh_model2.get())) {
            for (const auto& c : lay.model->coeffs()) {
                Eigen::Index col;
                try { col = est.vtecCoeffColumn(c.n, c.m, c.trig, lay.layer); }
                catch (...) { continue; }
                const double val = x_s(col);
                const double v   = (col < shcov.rows() && col < shcov.cols())
                                       ? shcov(col, col) : 0.0;
                vtec_sh_trace << se.time.week << ','
                              << fmt::format("{:.3f}", se.time.tow) << ',';
                if (lay_col) vtec_sh_trace << lay.layer << ',';
                vtec_sh_trace << c.n << ',' << c.m << ',' << c.trig << ','
                              << fmt::format("{:.4f}", val) << ','
                              << fmt::format("{:.4f}", (v > 0.0) ? std::sqrt(v) : 0.0) << '\n';
            }
        }
    }
}

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

// Run-end report: the Iono-2 SH deviation carry.
int networkShDevCarryRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const bool sh_dev_on = v.ctx.knobs.sh_dev_on;   // the run's one derivation (card NOM3 step EP0)
    const GlobalVtecShConfig& shc = v.config.getGlobalVtecShConfig();
    const auto& sh_dev_carry = v.ctx.carry.sh_dev_carry;

    if (!sh_dev_on) return 0;
    log->info("Network (Iono-2/T5v2 per-LOS deviation carry): {} warm harvest(s), "
              "{} cold seed(s), {} slip reset(s) (C3c funnel), {} no-information "
              "harvest skip(s), {} deviation(s) carried at run end (τ={} s, "
              "σ_ss={} TECU vertical, exact posterior harvest).",
              ctx.carry.shdev_warm_total, ctx.carry.shdev_cold_total,
              ctx.carry.shdev_slip_resets_total, ctx.carry.shdev_nodata_total,
              sh_dev_carry.size(), shc.rep_error_tau_s,
              shc.representation_sigma_tecu);
    // v3k clause only when armed, so the v2/v3 lines stay byte-identical with it off.
    if (shc.rep_error_d5_keep)
        log->info("Network (Iono-2/v3k D5-keep): {} D5-only re-tap(s) KEPT the level "
                  "(variance re-tapped to σ_ss), {} raw-confirmed erase(s) — the erase "
                  "policy split (SPEC_iono2_mechB_v3 addendum).",
                  ctx.carry.shdev_d5_keeps_total, ctx.carry.shdev_slip_resets_total);
    // v3 clause only when armed, so the v2 line stays byte-identical with it off.
    if (shc.rep_error_demean)
        log->info("Network (Iono-2/v3 de-mean): global common-mode removed on {} epoch(s) "
                  "(mean |g| = {:.3f} TECU), per-station means removed {} station-epoch(s) "
                  "(anti-ratchet; the a00-twin / receiver-lump quasi-null directions stay "
                  "with the field and receiver states — SPEC_iono2_mechB_v3).",
                  ctx.carry.shdev_demean_epochs_total,
                  ctx.carry.shdev_demean_epochs_total > 0
                      ? ctx.carry.shdev_demean_abs_g_sum
                            / static_cast<double>(ctx.carry.shdev_demean_epochs_total)
                      : 0.0,
                  ctx.carry.shdev_demean_sta_total);
    return 0;
}

// ---- End of the run-end reports ----

} // namespace GnssProcessor
