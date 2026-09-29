/**
 * @file NetworkPhaseVisibleDiag.cpp
 * @brief Implementation of @ref networkPhaseVisibleMonitor, the phase-visible consistency
 *        instrument (card PVN).
 *
 * Written as a seam, not relocated from the driver.  Seam contract: a free function over
 * the run context that owns no state; the open window is in @c ctx.carry, the run totals in
 * @c ctx.counters.
 */

#include "filter/NetworkPhaseVisibleDiag.hpp"

#include "app/NetworkRunContext.hpp"
#include "filter/NetworkClockAnchor.hpp"
#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace GnssProcessor {

namespace {

/// NIS values above this are counted (chi2(1), 1 %), as the level instrument counts them.
constexpr double kPvNisGate = 6.63;
/// Epochs after which the NIS enters the run totals (the level instrument's settling).
constexpr long long kPvSettledEp = 15;

}  // namespace

bool phaseVisibleNis(double e_hat, double s_m, double s_p, double& nis) {
    const double su = s_m - s_p;
    if (!std::isfinite(e_hat) || !std::isfinite(su) || !(su > 0.0)) return false;
    nis = e_hat * e_hat / su;
    return true;
}

void networkPhaseVisibleMonitor(NetworkRunContext& ctx, NetworkRunTraces& traces,
                                const SyncedEpoch& se, const NetworkEstimator& est,
                                const Eigen::VectorXd& x_s, const EpochWorkspace& ws) {
    if (!ctx.knobs.pv_on) return;
    auto& k = ctx.counters;
    auto& c = ctx.carry;
    ++k.pv_epochs;
    const bool settled = k.pv_epochs > kPvSettledEp;
    if (ws.cap_dense_keys.empty() || ws.cap_dense_cov.rows() == 0) {
        ++k.pv_epochs_nocap;
    } else {
        // The anchor guard's functional and contrast, read without a floor.
        const auto rows = clockAnchorPhaseStatistic(est, x_s, ws.cap_dense_keys,
                                                    ws.cap_dense_cov, 0.0);
        auto& tr = traces.phase_visible_trace;
        for (const auto& [sat, r] : rows) {
            if (!r.ok) continue;
            ++k.pv_rows;
            double nis = 0.0;
            const bool has_nis = phaseVisibleNis(r.e_hat, r.Sm, r.Sp, nis);
            const double sqrt_sp = std::sqrt(std::max(0.0, r.Sp));
            if (!has_nis) {
                ++k.pv_su_nonpos;
            } else if (settled) {
                k.pv_nis_sum += nis;
                ++k.pv_nis_n;
                if (nis > kPvNisGate) ++k.pv_nis_gt;
                k.pv_abs_e.push_back(std::abs(r.e_hat));
                k.pv_sqrt_sp.push_back(sqrt_sp);
                k.pv_sqrt_su.push_back(std::sqrt(r.Sm - r.Sp));
                c.pv_win_nis_sum += nis;
                ++c.pv_win_nis_n;
                c.pv_win_e2_sum += r.e_hat * r.e_hat;
                c.pv_win_su_sum += r.Sm - r.Sp;
            }
            if (tr)
                tr << fmt::format("{},{:.3f},{},{},{},{:.6f},{:.6f},{:.6f},{},{}\n",
                                  se.time.week, se.time.tow, sat, r.sigs, r.n_pb, r.d_post_hat, sqrt_sp,
                                  r.e_hat,
                                  has_nis ? fmt::format("{:.6f}", std::sqrt(r.Sm - r.Sp))
                                          : std::string{},
                                  has_nis ? fmt::format("{:.4f}", nis) : std::string{});
        }
    }
    // Close the window on the epoch count (epochs without a capture count too, so windows
    // stay aligned with time).
    if (++c.pv_win_ep >= ctx.knobs.pv_window_ep) {
        if (c.pv_win_nis_n > 0) {
            k.pv_win_nis.push_back(c.pv_win_nis_sum / static_cast<double>(c.pv_win_nis_n));
            k.pv_win_ratio.push_back(c.pv_win_su_sum > 0.0
                                         ? std::sqrt(c.pv_win_e2_sum / c.pv_win_su_sum)
                                         : 0.0);
        }
        c.pv_win_ep = 0;
        c.pv_win_nis_sum = 0.0;
        c.pv_win_nis_n = 0;
        c.pv_win_e2_sum = 0.0;
        c.pv_win_su_sum = 0.0;
    }
}

}  // namespace GnssProcessor
