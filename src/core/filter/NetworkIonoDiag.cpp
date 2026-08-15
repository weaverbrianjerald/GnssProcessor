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
#include <limits>
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
    auto& rx_states_trace      = traces.rx_states_trace;
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
                // FRAME FIX (SPEC_t3_null_derivation §7.8).  `iono_m` is the
                // reconstructed CORRECTION x_r alone, which is what this trace has
                // always emitted and what CLAUDE.md flags as frame-broken: the per-LOS
                // iono state is SEEDED at allocation, so x_r is a correction to that
                // seed, not the absolute slant ionosphere.  The column is kept
                // unchanged (pyviz consumes it); the absolute value is appended
                // instead, on the ZWD block's pattern below — x_lin from the station's
                // own state vector, iono_total_m = x_lin + x_r.  Grade
                // `iono_total_m`, never `iono_m`.
                const double x_lin =
                    stations[i].sm->getStateVector()(static_cast<Eigen::Index>(kv.second));
                const double iono_total = x_lin + x_r(hit->second);
                iono_trace << se.time.week << ','
                           << fmt::format("{:.3f}", se.time.tow) << ','
                           << stations[i].name << ',' << kv.first << ','
                           << fmt::format("{:.4f}", x_r(hit->second)) << ','
                           << fmt::format("{:.2f}", e->second.first) << ','
                           << fmt::format("{:.2f}", e->second.second) << ','
                           << fmt::format("{:.6f}", x_lin) << ','
                           << fmt::format("{:.6f}", iono_total) << '\n';
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

    // --- L14b: per-station RECEIVER state trace (network.trace_rx_states) ------
    // The receiver half of the §4.8 export gap.  Under the W6 slice the loader FORCES
    // estimate_ifb on (the W6 slice in GnssConfigLoadNetwork.cpp, where
    // estimate_sat_code_bias promotes estimate_ifb_) — an explicit receiver DCB state
    // per (station, RINEX code), without which the receiver GF code lump rides inside
    // the slant ionosphere and fights the SH field.  Those columns are nonetheless
    // exported NOWHERE as estimates: they appear only as prior-residual census LABELS
    // carrying standardized residuals.  So the Table-8 receiver term
    // (contract §B.4: ι̃ = ι + d̄_r,GF − d_GF^s) cannot be graded against an injected
    // receiver-bias truth, which is exactly what the synthetic fixture provides.
    //
    // Same construction as the ZWD block above (which is the pattern to copy, per the
    // SPEC): reconstructStation for the value, conditionalLocalVarianceDiag for the
    // datum-conditioned σ, and the column located via getClockIndex / getCodeBiasStates
    // + h2local rather than any carry-gated flag — a carry-gated lookup would emit no
    // rows on a cold run and read as "the states do not exist".
    //
    // One conditionalLocalVarianceDiag call per station covering ALL of that station's
    // receiver columns, not one call per column: the cost is in the conditioning, and
    // the diagonal it returns is per requested column anyway.
    if (rx_states_trace.is_open()) {
        for (size_t i = 0; i < stations.size(); ++i) {
            if (est_index_of[i] < 0) continue;               // not in this epoch's solve
            const auto& h2l = asm_res[i].h2local;
            // Collect (kind, key, state_index, local_column) for the clock + every rx CB.
            struct RxCol { const char* kind; std::string key; int sidx; Eigen::Index col; };
            std::vector<RxCol> want;
            const size_t clk_sz = stations[i].sm->getClockIndex();
            if (clk_sz != std::numeric_limits<size_t>::max()) {
                const auto hit = h2l.find(static_cast<int>(clk_sz));
                if (hit != h2l.end() && hit->second >= 0)
                    want.push_back({ "CLK", std::string(), static_cast<int>(clk_sz),
                                     static_cast<Eigen::Index>(hit->second) });
            }
            for (const auto& [key, idx] : stations[i].sm->getCodeBiasStates()) {
                const auto hit = h2l.find(static_cast<int>(idx));
                if (hit == h2l.end() || hit->second < 0) continue;
                want.push_back({ "CB", key, static_cast<int>(idx),
                                 static_cast<Eigen::Index>(hit->second) });
            }
            if (want.empty()) continue;
            std::vector<Eigen::Index> cols;
            cols.reserve(want.size());
            for (const auto& w : want) cols.push_back(w.col);
            Eigen::VectorXd x_r, vard;
            try {
                x_r = est.reconstructStation(static_cast<std::size_t>(est_index_of[i]), x_s);
                vard = est.conditionalLocalVarianceDiag(
                    static_cast<std::size_t>(est_index_of[i]), cols, plan.shared_datum);
            } catch (...) { continue; }
            for (size_t k = 0; k < want.size(); ++k) {
                const auto& w = want[k];
                if (w.col >= x_r.size()) continue;
                const double x_lin = stations[i].sm->getStateVector()(w.sidx);
                const double value  = x_lin + x_r(w.col);
                const double var    = (static_cast<Eigen::Index>(k) < vard.size())
                                          ? vard(static_cast<Eigen::Index>(k)) : -1.0;
                if (!std::isfinite(value)) continue;
                rx_states_trace << se.time.week << ','
                                << fmt::format("{:.3f}", se.time.tow) << ','
                                << stations[i].name << ',' << w.kind << ',' << w.key << ','
                                << fmt::format("{:.6f}", value) << ',';
                // Blank σ = "no honest conditional variance this epoch", never 0.
                if (var > 0.0 && std::isfinite(var))
                    rx_states_trace << fmt::format("{:.6e}", std::sqrt(var));
                rx_states_trace << '\n';
            }
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
