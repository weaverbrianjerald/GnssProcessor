/*!
 * \file WlsEstimator.cpp
 * \brief Weighted least-squares (WLS) epoch estimator implementation.
 *
 * Organization:
 *  - Construction
 *  - Private helpers:  filterObservations, predictObs, computeResiduals
 *  - Public interface: estimate
 */

#include "WlsEstimator.hpp"
#include "PppTypes.hpp"
#include "TimeUtils.hpp"
#include <algorithm>
#include <cmath>

namespace GnssProcessor {

// =====================================================================
// Construction
// =====================================================================

WlsEstimator::WlsEstimator(const GnssConfig& config, StateManager& state_manager, LogManager& log_manager)
    : config_(config),
      state_manager_(state_manager),
      log_manager_(log_manager),
      wls_logger_(nullptr) {
    try {
        if (auto main_logger = log_manager_.getLogger("main_log"))
            if (main_logger->level() == spdlog::level::debug)
                wls_logger_ = std::make_unique<WlsLogger>("wls_debug.log");
    }
    catch (...) {
        // Fail silently: logging is best-effort.
    }
}

// =====================================================================
// Private helpers
// =====================================================================

std::vector<WlsEstimator::WlsObs> WlsEstimator::filterObservations(
    const std::vector<PppObservation>& observations, double epoch_tow) const {

    std::vector<WlsObs> out;
    out.reserve(observations.size());
    auto logger = log_manager_.getLogger("ppp_model");

    for (const auto& obs : observations) {
        if (PppUtils::getSystemString(obs.system) != "GPS") {
            logger->debug("Skipping non-GPS satellite {} at TOW {:.3f}", obs.satellite, obs.time.tow);
            continue;
        }
        if (obs.sat_pos.norm() == 0.0 || !obs.sat_pos.allFinite() || obs.range <= 0.0 ||
            !std::isfinite(obs.elevation) || !std::isfinite(obs.azimuth) ||
            obs.los.norm() == 0.0 || !obs.los.allFinite()) {
            logger->debug("Invalid geometry for satellite {} at TOW {:.3f}", obs.satellite, obs.time.tow);
            continue;
        }
        if (obs.elevation < config_.getElevationMaskDeg()) {
            logger->debug("Satellite {} below elevation mask ({} < {} deg)",
                obs.satellite, obs.elevation, config_.getElevationMaskDeg());
            continue;
        }

        for (const auto& [type, observed] : obs.observed) {
            const bool is_code = (!type.empty() && type[0] == 'C');
            const bool is_iono_free = (type.find("C1") != std::string::npos && type.find("C2") != std::string::npos);
            const bool is_individual_code = (is_code && type.find('_') == std::string::npos);
            if (!is_iono_free && !is_individual_code) continue;

            if (!obs.modeled.count(type) || !obs.variances.count(type)) {
                logger->debug("Missing modeled or variance data for {} {} at TOW {:.3f}",
                    obs.satellite, type, obs.time.tow);
                continue;
            }

            out.push_back({
                observed,
                obs.modeled.at(type),
                obs.variances.at(type),
                obs.mf_w,
                obs.range,
                obs.elevation,
                obs.sat_pos,
                obs.satellite,
                type
            });

            if (is_individual_code) break; // one code obs per satellite for WLS
        }
    }
    return out;
}

double WlsEstimator::predictObs(const WlsObs& obs,
    const Eigen::Vector3d& pos, double clock, double trop_z) {
    const double current_range = (obs.sat_pos - pos).norm();
    const double range_delta = current_range - obs.initial_range;
    return (obs.modeled + range_delta) + clock + obs.mf_w * trop_z;
}

void WlsEstimator::computeResiduals(const std::vector<WlsObs>& wls_obs,
    const Eigen::Vector3d& pos, double clock, double trop_z,
    double& rms, double& max_abs) {

    if (wls_obs.empty()) {
        rms = std::numeric_limits<double>::infinity();
        max_abs = std::numeric_limits<double>::infinity();
        return;
    }

    double sum_sq = 0.0;
    double local_max = 0.0;
    for (const auto& obs : wls_obs) {
        const double r = (obs.sat_pos - pos).norm();
        if (!(r > 0.0 && std::isfinite(r))) continue;
        const double e = obs.observed - predictObs(obs, pos, clock, trop_z);
        sum_sq += e * e;
        local_max = std::max(local_max, std::abs(e));
    }
    rms = std::sqrt(sum_sq / static_cast<double>(wls_obs.size()));
    max_abs = local_max;
}

// =====================================================================
// Public interface
// =====================================================================

WlsResult WlsEstimator::estimate(const GnssTime& epoch_time,
    const std::vector<PppObservation>& observations) {

    WlsResult result;
    const double epoch_tow = epoch_time.tow;
    const std::string epoch_tag = TimeUtils::formatGnssTime(epoch_time);

    if (observations.empty() || observations.size() < kCoreSize) {
        log_manager_.getLogger("ppp_model")->warn(
            "No or insufficient observations for WLS at TOW {:.3f}: {} available, {} required",
            epoch_tow, observations.size(), kCoreSize);
        return result;
    }

    // --- Filter observations ---
    const auto wls_obs = filterObservations(observations, epoch_tow);
    if (wls_obs.size() < kCoreSize) {
        log_manager_.getLogger("ppp_model")->warn(
            "Insufficient code combinations for WLS at TOW {:.3f}: {} available, {} required",
            epoch_tow, wls_obs.size(), kCoreSize);
        return result;
    }

    // Collect satellite IDs and types for WlsLogger
    std::vector<std::string> satellite_ids, combo_types;
    satellite_ids.reserve(wls_obs.size());
    combo_types.reserve(wls_obs.size());
    for (const auto& o : wls_obs) {
        satellite_ids.push_back(o.satellite);
        combo_types.push_back(o.combo_type);
    }

    // --- Seed state from StateManager ---
    const Eigen::VectorXd state_vector = state_manager_.getStateVector();
    Eigen::Vector3d current_pos = state_vector.segment<3>(state_manager_.getPositionIndex());
    double current_clock = state_vector(state_manager_.getClockIndex());
    double current_trop_z = state_vector(state_manager_.getTropoIndex());

    // --- Iterative WLS ---
    constexpr int kMaxIter = 5;
    constexpr double kTol = 0.1;
    constexpr double kMinVariance = 1e-6;
    const auto n = static_cast<Eigen::Index>(wls_obs.size());

    Eigen::MatrixXd HtWH;
    int iter = 0;

    for (iter = 0; iter < kMaxIter; ++iter) {
        Eigen::MatrixXd H(n, kCoreSize);
        Eigen::VectorXd z(n);
        Eigen::MatrixXd W = Eigen::MatrixXd::Zero(n, n);

        for (Eigen::Index i = 0; i < n; ++i) {
            const auto& obs = wls_obs[static_cast<size_t>(i)];
            const Eigen::Vector3d delta_r = obs.sat_pos - current_pos;
            const double current_range = delta_r.norm();
            if (current_range == 0.0) continue;
            const Eigen::Vector3d los = delta_r / current_range;

            z(i) = obs.observed - predictObs(obs, current_pos, current_clock, current_trop_z);

            H(i, 0) = -los.x();
            H(i, 1) = -los.y();
            H(i, 2) = -los.z();
            H(i, 3) = 1.0;
            H(i, 4) = obs.mf_w;

            W(i, i) = 1.0 / std::max(obs.variance, kMinVariance);
        }

        HtWH = H.transpose() * W * H;

        if (wls_logger_) {
            const Eigen::JacobiSVD<Eigen::MatrixXd> svd(HtWH);
            const double cond = svd.singularValues()(0) /
                svd.singularValues()(svd.singularValues().size() - 1);
            wls_logger_->logDesignMatrix(H, "WLS Design Matrix H", epoch_tow, iter + 1, satellite_ids, combo_types, epoch_tag);
            wls_logger_->logInnovationsVector(z, "WLS Innovations Vector z", epoch_tow, iter + 1, satellite_ids, combo_types, epoch_tag);
            wls_logger_->logWeightMatrix(W, "WLS Weight Matrix W", epoch_tow, iter + 1, epoch_tag);
            wls_logger_->logNormalMatrix(HtWH, "WLS Normal Matrix HtWH", epoch_tow, iter + 1, cond, epoch_tag);
        }

        if (HtWH.determinant() <= 1e-6) {
            log_manager_.getLogger("ppp_model")->warn(
                "WLS failed at TOW {:.3f}, iter {}: singular HtWH, det={:.3e}",
                epoch_tow, iter, HtWH.determinant());
            return result;
        }

        const Eigen::VectorXd delta = HtWH.inverse() * H.transpose() * W * z;
        current_pos   += delta.segment<3>(0);
        current_clock += delta(3);
        current_trop_z += delta(4);

        if (wls_logger_) {
            Eigen::VectorXd current_state(kCoreSize);
            current_state << current_pos, current_clock, current_trop_z;
            wls_logger_->logStateVector(current_state, "WLS Intermediate State", epoch_tow, iter + 1, epoch_tag);
        }

        if (delta.segment<3>(0).norm() < kTol)
            break;
    }

    // --- Assemble result ---
    Eigen::VectorXd final_state(kCoreSize);
    final_state << current_pos, current_clock, current_trop_z;
    Eigen::MatrixXd final_covariance = HtWH.inverse();

    double rms, max_abs;
    computeResiduals(wls_obs, current_pos, current_clock, current_trop_z, rms, max_abs);

    log_manager_.getLogger("ppp_model")->info(
        "Iterated WLS converged after {} iterations using {} code combos at TOW {:.3f}: "
        "pos=({:.3f}, {:.3f}, {:.3f}) m, clock={:.3e} m, tropo={:.3f} m, rms={:.3f} m, max_abs={:.3f} m",
        iter + 1, wls_obs.size(), epoch_tow,
        final_state(0), final_state(1), final_state(2),
        final_state(3), final_state(4), rms, max_abs);

    if (wls_logger_) {
        wls_logger_->logStateVector(final_state, "WLS Final State", epoch_tow, iter + 1, epoch_tag);
        wls_logger_->logCovarianceMatrix(final_covariance, "WLS Final Covariance", epoch_tow, iter + 1, epoch_tag);
    }

    result.state = std::move(final_state);
    result.covariance = std::move(final_covariance);
    result.rms = rms;
    result.max_abs = max_abs;
    result.observation_count = wls_obs.size();
    result.iterations = iter + 1;
    return result;
}

} // namespace GnssProcessor