/**
 * @file IonoDiag.cpp
 * @brief Implementation of @ref networkIonoDiag: the iono / ZWD / receiver-state /
 *        anchor-info viz traces of one network epoch.
 */

#include "network/diag/IonoDiag.hpp"

#include "network/state/RunContext.hpp"
#include "config/GnssConfig.hpp"
#include "network/diag/RunTraces.hpp"
#include "common/Constants.hpp"
#include "common/GnssIdentity.hpp"
#include "common/PppUtils.hpp"
#include "network/carry/CarryVce.hpp"
#include "network/solve/PivotHandover.hpp"
#include "math/Coordinates.hpp"
#include "network/state/StationParams.hpp"

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
#include "network/driver/Epoch.hpp"   // NetworkEpoch
#include "network/driver/Run.hpp"     // NetworkRun
#include "network/driver/SetupResult.hpp"
#include "rinex/MultiRinexSync.hpp"

namespace GnssProcessor {

void networkIonoDiag(NetworkRun& run, const NetworkEpoch& ep) {
    // ---- From the run and the epoch. Reads bind through `ep`; a write to an entity table
    //      is marked `// written`. ----
    NetworkRunContext& ctx = run.ctx;
    const GnssConfig& config_ = run.config;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    NetworkRunTraces& traces = run.traces;
    NetworkSetupResult& setup = run.setup;
    const EpochWorkspace& ws = ep.ws;
    const SyncedEpoch& se = ep.se;
    NetworkEstimator& est = run.engines.est;
    const Eigen::VectorXd& x_s = ep.solve.x_s;
    const NetworkSbasis::Plan& plan = ep.solve.plan;
    const std::vector<StationAssembly>& asm_res = ep.assembly.asm_res;
    const std::vector<int>& est_index_of = ep.assembly.est_index_of;
    const std::vector<std::vector<PppObservation>>& modeled = ep.pass_a.modeled;
    const std::vector<std::string>& registered_clock_sats = ep.columns.registered_clock_sats;
    // ---- End of the bindings ----
    // --- Local aliases ---
    const double clk_anchor_sigma = ctx.knobs.clk_anchor_sigma;
    auto& stations = setup.stations;
    auto& iono_trace           = traces.iono_trace;
    auto& anchor_info_trace    = traces.anchor_info_trace;
    auto& zwd_trace            = traces.zwd_trace;
    auto& rx_states_trace      = traces.rx_states_trace;
    auto& clk_prior_info = ws.clk_prior_info;


    // --- Network-estimated slant ionosphere (viz trace) -----------------
    // Back-substitute each present station's local nuisance block (the slant
    // iono states live there) and emit one row per (station, satellite) at the
    // measured elevation/azimuth, so the pyviz viewer can map each value to its
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
            const StationParams& params = stations[i].params;
            for (const auto& [sat, slot_idx] : params.iono) {
                const auto hit = h2l.find(slot_idx);
                if (hit == h2l.end()) continue;
                if (hit->second < 0
                    || static_cast<Eigen::Index>(hit->second) >= x_r.size())
                    continue;
                const std::string sat_text = sat.str();
                const auto e = elaz.find(sat_text);
                if (e == elaz.end()) continue;
                // Frame of the columns.  `iono_m` is the reconstructed correction x_r
                // alone.  The per-LOS iono state is seeded at allocation, so x_r is a
                // correction to that seed, not the absolute slant ionosphere.  The
                // column stays as it is because pyviz consumes it; the absolute value
                // is appended after it, on the ZWD block's pattern below: x_lin from
                // the station's own state vector, iono_total_m = x_lin + x_r.  Grade
                // `iono_total_m`, not `iono_m`.
                const double x_lin = params.slots[static_cast<std::size_t>(slot_idx)].x_lin;
                const double iono_total = x_lin + x_r(hit->second);
                iono_trace << se.time.week << ','
                           << fmt::format("{:.3f}", se.time.tow) << ','
                           << stations[i].name << ',' << sat_text << ','
                           << fmt::format("{:.4f}", x_r(hit->second)) << ','
                           << fmt::format("{:.2f}", e->second.first) << ','
                           << fmt::format("{:.2f}", e->second.second) << ','
                           << fmt::format("{:.6f}", x_lin) << ','
                           << fmt::format("{:.6f}", iono_total) << '\n';
            }
        }
    }

    // --- Per-station ZWD trace -------------------------------------------
    // One row per present station each emitted epoch: the estimated ZWD (a-priori state +
    // reconstructed correction) and its datum-conditioned formal σ (the instrument for
    // border-station σ sharpening).  Independent of the carry: the ZWD column is located via
    // the station's parameter set and h2local, not R.pl_zwd_present, which is set only when
    // the ZWD carry (network.carry_zwd) or snapshot capture is active (a lookup through it
    // would emit no rows on a run with neither).
    if (zwd_trace.is_open()) {
        for (size_t i = 0; i < stations.size(); ++i) {
            if (est_index_of[i] < 0) continue;               // not in this epoch's solve
            const StationParams& params = stations[i].params;
            if (params.zwd < 0) continue;                    // no ZWD state
            const int tropo_idx = params.zwd;
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
            const double x_lin = params.slots[static_cast<std::size_t>(tropo_idx)].x_lin;  // ≡0.0 today; kept for robustness
            const double zwd_m = x_lin + x_r(zcol);
            const double var   = (vard.size() > 0) ? vard(0) : -1.0;
            if (!(var > 0.0) || !std::isfinite(var) || !std::isfinite(zwd_m)) continue;
            zwd_trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                      << stations[i].name << ',' << fmt::format("{:.6f}", zwd_m) << ','
                      << fmt::format("{:.6e}", std::sqrt(var)) << ','
                      << (asm_res[i].pl_zwd_injected ? 1 : 0) << '\n';
        }
    }

    // --- Per-station receiver state trace (network.trace_rx_states) ------
    // With estimate_sat_code_bias on, the loader forces estimate_ifb on
    // (GnssConfigLoadNetwork.cpp, where estimate_sat_code_bias promotes estimate_ifb_): an
    // explicit receiver DCB state per (station, RINEX code), without which the receiver GF
    // code lump rides inside the slant ionosphere and pulls against the spherical-harmonic
    // (SH) field.  No other output exports those columns as estimates; they appear only
    // as prior-residual census labels carrying standardized residuals.  Without this
    // trace, the receiver term (ι̃ = ι + d̄_r,GF − d_GF^s) cannot be graded against the
    // injected receiver-bias truth that the synthetic fixture provides.
    //
    // Same construction as the ZWD block above: reconstructStation for the value,
    // conditionalLocalVarianceDiag for the datum-conditioned σ, and the column located via
    // the parameter set's clock and receiver code biases + h2local rather than any
    // carry-gated flag.  A carry-gated lookup would emit no rows on a cold run, which would
    // read as "the states do not exist".
    //
    // One conditionalLocalVarianceDiag call per station covers all of that station's
    // receiver columns: the cost is in the conditioning, and the returned diagonal is
    // per requested column either way.
    if (rx_states_trace.is_open()) {
        for (size_t i = 0; i < stations.size(); ++i) {
            if (est_index_of[i] < 0) continue;               // not in this epoch's solve
            const auto& h2l = asm_res[i].h2local;
            // Collect (kind, key, state_index, local_column) for the clock, each rx CB and each ISB.
            struct RxCol { const char* kind; std::string key; int sidx; Eigen::Index col; };
            std::vector<RxCol> want;
            const StationParams& params = stations[i].params;
            if (params.clock >= 0) {
                const auto hit = h2l.find(params.clock);
                if (hit != h2l.end() && hit->second >= 0)
                    want.push_back({ "CLK", std::string(), params.clock,
                                     static_cast<Eigen::Index>(hit->second) });
            }
            for (const auto& [cb, idx] : params.rx_cb) {
                const auto hit = h2l.find(idx);
                if (hit == h2l.end() || hit->second < 0) continue;
                want.push_back({ "CB", keyText(params.slots[static_cast<std::size_t>(idx)].ref), idx,
                                 static_cast<Eigen::Index>(hit->second) });
            }
            // The receiver inter-system bias: one row per non-GPS system whose column
            // is in this station's block this epoch (kind ISB, key = the system name).
            // A GPS-only run allocates no such state and writes no such row.
            for (const auto& [sys, idx] : params.isb) {
                const auto hit = h2l.find(idx);
                if (hit == h2l.end() || hit->second < 0) continue;
                want.push_back({ "ISB", systemName(sys), idx,
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
                const double x_lin = params.slots[static_cast<std::size_t>(w.sidx)].x_lin;
                const double value  = x_lin + x_r(w.col);
                const double var    = (static_cast<Eigen::Index>(k) < vard.size())
                                          ? vard(static_cast<Eigen::Index>(k)) : -1.0;
                if (!std::isfinite(value)) continue;
                rx_states_trace << se.time.week << ','
                                << fmt::format("{:.3f}", se.time.tow) << ','
                                << stations[i].name << ',' << w.kind << ',' << w.key << ','
                                << fmt::format("{:.6f}", value) << ',';
                // A blank σ means no valid conditional variance this epoch; 0 is never
                // written.
                if (var > 0.0 && std::isfinite(var))
                    rx_states_trace << fmt::format("{:.6e}", std::sqrt(var));
                rx_states_trace << '\n';
            }
        }
    }

    // --- A-priori-clock-anchor information-fraction diagnostic -----------
    // Checks that the estimable network clock is determined by the data, not by the
    // a-priori code product it is softly anchored to.  anchor_info is the constant
    // Tikhonov pull (1/σ²); total_info is the reduced-normals diagonal on the clock
    // column (data + carry prior + datum pin).  The legacy anchor/total fraction
    // misleads for a locked-in (carry-dominated) clock: the large carry information
    // shrinks the fraction and reads as data-dominated for a column that is in fact
    // defined by its own past.  The denominator is therefore decomposed: the
    // carried clock-prior info (clk_prior_info, folded this epoch) and the anchor are
    // subtracted, leaving data_info.  The trace reports anchor_frac_data =
    // anchor/(anchor+data_info), which does not depend on carry stiffness, and
    // carry_frac = carry/total, which rises on a lock-in.  Read-only; the anchor fold
    // itself is unchanged.
    if (anchor_info_trace.is_open()) {
        const double clk_sigma_a = clk_anchor_sigma;  // resolved anchor sigma (m), the value the anchor fold uses
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
            // Data information = everything that is neither the carry prior nor the
            // anchor.  For a pivot, total_info also holds the datum pin, so its
            // decomposition is pin-dominated and says nothing about data dominance;
            // the is_pivot column flags those rows.
            const double data_info = std::max(0.0, total_info - carry_info - anchor_info);
            const double denom_ad  = anchor_info + data_info;
            const double anchor_frac_data = (denom_ad > 0.0) ? (anchor_info / denom_ad) : 0.0;
            const double carry_frac = (total_info > 0.0) ? (carry_info / total_info) : 0.0;
            const double frac = (total_info > 0.0) ? (anchor_info / total_info) : 0.0;
            const auto pit = plan.clock_pivot.find(constellationKey(systemOfSat(sat)));
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
