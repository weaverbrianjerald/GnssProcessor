/**
 * @file NetworkIonoDiag.cpp
 * @brief Implementation of @ref networkIonoDiag — the SB_IONODIAG block relocated VERBATIM
 *        (dedent-4, zero substitution) out of Application::runNetwork at INC-11b.
 */

#include "filter/NetworkIonoDiag.hpp"

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "common/Constants.hpp"
#include "common/PppUtils.hpp"
#include "filter/NetworkCarryVce.hpp"
#include "filter/NetworkIonoDeweightGate.hpp"
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

namespace GnssProcessor {

void networkIonoDiag(
    NetworkRunContext& ctx,
    const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkRunTraces& traces,
    NetworkSetupResult& setup,
    const EpochWorkspace& ws,
    const SyncedEpoch& se,
    NetworkEstimator& est,
    const Eigen::VectorXd& x_s,
    const NetworkSbasis::Plan& plan,
    const std::vector<StationAssembly>& asm_res,
    const std::vector<int>& est_index_of,
    const std::vector<std::vector<PppObservation>>& modeled,
    const std::vector<std::string>& registered_clock_sats,
    double clk_anchor_sigma) {
    // --- Alias preamble: mechanically-derived run-TU aliases (+ reconstructions). --
    auto& stations = setup.stations;
    auto& iono_trace           = traces.iono_trace;
    auto& anchor_info_trace    = traces.anchor_info_trace;
    auto& zwd_trace            = traces.zwd_trace;
    auto& clk_prior_info = ws.clk_prior_info;


    // --- Network-estimated slant ionosphere (viz trace) -----------------
    // Back-substitute each present station's local nuisance block (the slant
    // iono lives there, an absolute per-epoch estimate with linearization
    // point 0) and emit one row per (station, satellite) at the measured
    // elevation/azimuth, so the pyviz viewer can map each value to its
    // ionospheric pierce point.  Gated by network.trace_ionosphere.
    if (iono_trace.is_open()) {
        for (size_t i = 0; i < stations.size(); ++i) {
            if (est_index_of[i] < 0) continue;   // not added to this epoch's solve
            Eigen::VectorXd x_r;
            try {
                x_r = est.reconstructStation(
                    static_cast<std::size_t>(est_index_of[i]), x_s);
            } catch (...) { continue; }
            // Elevation/azimuth per satellite from the modeled observations
            // (first observation of each satellite carries the geometry).
            std::map<std::string, std::pair<double, double>> elaz;
            for (const auto& o : modeled[i])
                elaz.emplace(o.satellite, std::make_pair(o.elevation, o.azimuth));
            const auto& h2l = asm_res[i].h2local;
            for (const auto& kv : stations[i].sm->getIonosphereStates()) {
                const auto hit = h2l.find(static_cast<int>(kv.second));
                if (hit == h2l.end()) continue;
                if (hit->second < 0
                    || static_cast<Eigen::Index>(hit->second) >= x_r.size())
                    continue;
                const auto e = elaz.find(kv.first);
                if (e == elaz.end()) continue;
                iono_trace << se.time.week << ','
                           << fmt::format("{:.3f}", se.time.tow) << ','
                           << stations[i].name << ',' << kv.first << ','
                           << fmt::format("{:.4f}", x_r(hit->second)) << ','
                           << fmt::format("{:.2f}", e->second.first) << ','
                           << fmt::format("{:.2f}", e->second.second) << '\n';
            }
        }
    }

    // --- Per-station ZWD trace (L7) -------------------------------------
    // One row per present station each emitted epoch: the estimated ZWD (a-priori state +
    // reconstructed correction) and its datum-conditioned formal σ (the V7 border-sharpening
    // instrument).  Independent of the carry — locate the ZWD column via getTropoIndex /
    // h2local, NOT R.pl_zwd_present (that is only set under carry_warm && zwd_on, so run A of
    // the V7 A/B and warmup would otherwise emit no rows).
    if (zwd_trace.is_open()) {
        for (size_t i = 0; i < stations.size(); ++i) {
            if (est_index_of[i] < 0) continue;               // not in this epoch's solve
            const size_t tropo_idx_sz = stations[i].sm->getTropoIndex();
            if (tropo_idx_sz == std::numeric_limits<size_t>::max()) continue;  // no ZWD state
            const int tropo_idx = static_cast<int>(tropo_idx_sz);
            const auto& h2l = asm_res[i].h2local;
            const auto zhit = h2l.find(tropo_idx);
            if (zhit == h2l.end() || zhit->second < 0) continue;   // ZWD not in A_r this epoch
            const Eigen::Index zcol = static_cast<Eigen::Index>(zhit->second);
            Eigen::VectorXd x_r, vard;
            try {
                x_r = est.reconstructStation(static_cast<std::size_t>(est_index_of[i]), x_s);
                vard = est.conditionalLocalVarianceDiag(
                    static_cast<std::size_t>(est_index_of[i]), {zcol}, plan.shared_datum);
            } catch (...) { continue; }
            if (zcol >= x_r.size()) continue;
            const double x_lin = stations[i].sm->getStateVector()(tropo_idx);  // ≡0.0 today; kept for robustness
            const double zwd_m = x_lin + x_r(zcol);
            const double var   = (vard.size() > 0) ? vard(0) : -1.0;
            if (!(var > 0.0) || !std::isfinite(var) || !std::isfinite(zwd_m)) continue;
            zwd_trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                      << stations[i].name << ',' << fmt::format("{:.6f}", zwd_m) << ','
                      << fmt::format("{:.6e}", std::sqrt(var)) << ','
                      << (asm_res[i].pl_zwd_injected ? 1 : 0) << '\n';
        }
    }

    // --- A-priori-clock-anchor information-fraction diagnostic (B7) ------
    // Certify the estimable network clock is DATA-dominated, not defined by the
    // a-priori CODE product it is softly anchored to.  anchor_info is the constant
    // Tikhonov pull (1/σ²); total_info is the reduced-normals diagonal on the clock
    // column (data + carry prior + datum pin).  P1-4: the legacy anchor/total fraction
    // is MISLEADING for a locked-in (carry-dominated) clock — its huge carry info
    // shrinks the fraction and certifies "data-dominated" for a column that is really
    // defined by its own past.  Decompose the denominator: subtract the carried
    // clock-prior info (clk_prior_info, folded this epoch) and the anchor, leaving
    // data_info; report anchor_frac_data = anchor/(anchor+data_info) — independent of
    // carry stiffness — plus carry_frac = carry/total, which lights up on a lock-in.
    // Read-only; the anchor fold itself is unchanged.
    if (anchor_info_trace.is_open()) {
        const double clk_sigma_a = clk_anchor_sigma;  // D6-resolved (matches the fold site above)
        const bool anchor_active =
            std::isfinite(clk_sigma_a) && clk_sigma_a > 0.0 && clk_sigma_a < 100.0;
        const double anchor_info =
            anchor_active ? 1.0 / (clk_sigma_a * clk_sigma_a) : 0.0;
        const Eigen::MatrixXd& Nss = est.reducedSharedNormals();
        for (const std::string& sat : registered_clock_sats) {
            Eigen::Index col;
            try { col = est.clockColumn(sat); } catch (...) { continue; }
            if (col < 0 || col >= Nss.rows()) continue;
            const double total_info = Nss(col, col);
            const auto cpi = clk_prior_info.find(sat);
            const double carry_info = (cpi != clk_prior_info.end()) ? cpi->second : 0.0;
            // Data information = everything not the carry prior or the anchor.  For a
            // pivot the datum pin is ALSO in total_info (is_pivot flags it — its
            // decomposition is pin-dominated and not a data-dominance statement).
            const double data_info = std::max(0.0, total_info - carry_info - anchor_info);
            const double denom_ad  = anchor_info + data_info;
            const double anchor_frac_data = (denom_ad > 0.0) ? (anchor_info / denom_ad) : 0.0;
            const double carry_frac = (total_info > 0.0) ? (carry_info / total_info) : 0.0;
            const double frac = (total_info > 0.0) ? (anchor_info / total_info) : 0.0;
            const auto pit = plan.clock_pivot.find(NetworkSbasis::constellationOf(sat));
            const bool is_pivot = (pit != plan.clock_pivot.end() && pit->second == sat);
            anchor_info_trace
                << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                << sat << ',' << fmt::format("{:.6e}", anchor_info) << ','
                << fmt::format("{:.6e}", carry_info) << ','
                << fmt::format("{:.6e}", data_info) << ','
                << fmt::format("{:.6e}", total_info) << ','
                << fmt::format("{:.6f}", anchor_frac_data) << ','
                << fmt::format("{:.6f}", carry_frac) << ','
                << fmt::format("{:.6f}", frac) << ',' << (is_pivot ? 1 : 0) << '\n';
        }
    }
}

} // namespace GnssProcessor
