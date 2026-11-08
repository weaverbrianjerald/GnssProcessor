/**
 * @file ConsumerTrace.cpp
 * @brief Row helpers and the pure CSV formatter of `logs/closed_loop_trace.csv`.
 *
 * Kept apart from the two writers (the network emit/store seam and the consumer
 * replay) so both write the same bytes and a unit test can pin the schema without
 * constructing a RealTimeEngine or touching a file.
 */

#include "realtime/ConsumerTrace.hpp"

#include "filter/AmbiguityResolver.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>

namespace GnssProcessor {

std::string consumerArGateLabel(bool ar_ran, const AmbiguityResolution& ar) {
    if (!ar_ran) return "not_run";
    switch (ar.gate_reason) {
        case 1:  return "disabled";
        case 2:  return "min_epochs";
        case 3:  return fmt::format("sigma_gate({:.3f}m)", ar.gate_sigma_3d);
        case 4:  return "no_candidates";
        case 5:  return "wl_guard";
        default: return ar.success ? (ar.wl_only_fix ? "wl_only" : "fixed")
                                   : (ar.ratio > 0.0 ? "ratio_battery" : "mlambda_fail");
    }
}

void fillConsumerTraceAr(ConsumerTraceRow& row, bool ar_ran, const AmbiguityResolution& ar) {
    row.ar_ran         = ar_ran;
    row.ar_success     = ar_ran && ar.success;
    row.num_fixed      = ar_ran ? ar.num_fixed : 0;
    row.num_candidates = ar_ran ? ar.num_candidates : 0;
    row.ratio          = ar_ran ? ar.ratio : 0.0;
    row.ar_gate        = consumerArGateLabel(ar_ran, ar);
    row.p_succ         = ar_ran ? ar.p_succ : 0.0;
}

Eigen::Vector3d enuSigmaFromEcefCovariance(const Eigen::Matrix3d& p_ecef,
                                           const Coordinates::Llh& ref) {
    const Eigen::Matrix3d r = Coordinates::enuToEcefMatrix(ref.lat_rad, ref.lon_rad);
    const Eigen::Matrix3d p_enu = r.transpose() * p_ecef * r;
    return Eigen::Vector3d(std::sqrt(std::max(0.0, p_enu(0, 0))),
                           std::sqrt(std::max(0.0, p_enu(1, 1))),
                           std::sqrt(std::max(0.0, p_enu(2, 2))));
}

double sigma3dFromCovariance(const Eigen::Matrix3d& p_ecef) {
    return std::sqrt(std::max(0.0, p_ecef.trace()));
}

std::string formatConsumerTraceRow(const ConsumerTraceRow& r) {
    return fmt::format(
        "{},{:.3f},{},{},{},{},{},{:.2f},"        // week,tow,n_sats,ar_ran,ar_success,num_fixed,num_candidates,ratio
        "{:.4f},{:.4f},{:.4f},{:.4f},"            // pos_err_3d_m,east_m,north_m,up_m
        "{},{:.6f},"                              // ar_gate,p_succ
        "{:.4f},{:.4f},{:.4f},{:.4f}\n",          // sig_e_m,sig_n_m,sig_u_m,sig_3d_m
        r.week, r.tow, r.n_sats, r.ar_ran ? 1 : 0, r.ar_success ? 1 : 0,
        r.num_fixed, r.num_candidates, r.ratio,
        r.err_3d_m, r.err_enu_m.x(), r.err_enu_m.y(), r.err_enu_m.z(),
        r.ar_gate, r.p_succ,
        r.sig_enu_m.x(), r.sig_enu_m.y(), r.sig_enu_m.z(), r.sig_3d_m);
}

} // namespace GnssProcessor
