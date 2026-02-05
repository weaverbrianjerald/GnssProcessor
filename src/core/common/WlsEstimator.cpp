/**
 * @file WlsEstimator.cpp
 * @brief Weighted least-squares (WLS) epoch estimator implementation.
 *
 * Implements the WlsEstimator which forms the design and weight matrices from
 * epoch observations, iteratively solves the normal equations for receiver
 * position, clock and troposphere, and returns a WlsResult containing the
 * estimated state, covariance and quality metrics.
 *
 * The implementation:
 * - filters observations by system, geometry and elevation mask,
 * - forms iono-free code combinations (as provided in PppObservation),
 * - assembles H, W and innovation vectors, then iterates to convergence,
 * - computes RMS and max-absolute residuals and logs diagnostic matrices via WlsLogger.
 */

#include "WlsEstimator.hpp"
#include "PppTypes.hpp"
#include "TimeUtils.hpp"
#include <algorithm>
#include <cmath>

namespace GnssProcessor {

/**
 * @brief Construct a WlsEstimator.
 *
 * The estimator stores references to the provided configuration and state
 * manager and uses the provided LogManager for logging. A WlsLogger is
 * constructed for detailed WLS debug traces.
 *
 * @param config Reference to global GNSS processing configuration.
 * @param state_manager Reference to the StateManager used for state access/updates.
 * @param log_manager Reference to the LogManager for logging.
 */
WlsEstimator::WlsEstimator(const GnssConfig& config, StateManager& state_manager, LogManager& log_manager)
    : config_(config),
      state_manager_(state_manager),
      log_manager_(log_manager),
      wls_logger_(nullptr) {
    // Only enable the WLS trace file when the main logger is at debug level.
    // If the LogManager hasn't created the "main_log" logger yet, getLogger will create a module logger;
    // that's acceptable since we only query its level.
    try {
        if (auto main_logger = log_manager_.getLogger("main_log")) {
            if (main_logger->level() == spdlog::level::debug) {
                wls_logger_ = std::make_unique<WlsLogger>("wls_debug.log");
            }
        }
    }
    catch (...) {
        // Fail silently: logging is best-effort; if logger access fails, keep wls_logger_ null.
    }
}

/**
 * @brief Perform WLS estimation for a single epoch.
 *
 * This method:
 * - Validates input observation count.
 * - Filters observations by system (GPS only in this implementation), geometry and elevation mask.
 * - Extracts iono-free code combinations and associated modeled values, variances and mapping coefficients.
 * - Iteratively forms H, W and z and solves the normal equations for the core state:
 *   position (3), receiver clock and troposphere wet delay (total 5 states).
 * - Computes the final covariance (inverse of H^T W H), RMS and maximum absolute residual.
 * - Logs diagnostic matrices/vectors and the final state via WlsLogger and LogManager.
 *
 * The algorithm uses a fixed maximum number of iterations and a position-convergence
 * tolerance to stop the iterative refinement.
 *
 * @param epoch_time Epoch time at which to run the WLS (used for logging/traces).
 * @param observations Vector of per-satellite PppObservation used in the WLS.
 * @return WlsResult Result object containing the state vector, covariance and quality metrics.
 */
WlsResult WlsEstimator::estimate(const GnssTime& epoch_time, const std::vector<PppObservation>& observations) {
    WlsResult result;

    constexpr size_t kCoreSize = 5; // position(3) + clock + tropo
    if (observations.empty() || observations.size() < kCoreSize) {
        log_manager_.getLogger("ppp_model")->warn(
            "No or insufficient observations for WLS at TOW {:.3f}: {} available, {} required",
            epoch_time.tow, observations.size(), kCoreSize);
        return result;
    }

    std::vector<double> observation_vector;
    std::vector<double> modeled_vector;
    std::vector<double> variances;
    std::vector<double> mappings;
    std::vector<std::string> satellite_ids;
    std::vector<std::string> combo_types;
    std::vector<Eigen::Vector3d> sat_positions;
    std::vector<double> initial_ranges;
    std::vector<double> elevations;

    for (const auto& obs : observations) {
        const std::string system_str = PppUtils::getSystemString(obs.system);
        if (system_str != "GPS") {
            log_manager_.getLogger("ppp_model")->debug("Skipping non-GPS satellite {} at TOW {:.3f}", obs.satellite, obs.time.tow);
            continue;
        }
        if (obs.sat_pos.norm() == 0.0 || !obs.sat_pos.allFinite() || obs.range <= 0.0 ||
            !std::isfinite(obs.elevation) || !std::isfinite(obs.azimuth) ||
            obs.los.norm() == 0.0 || !obs.los.allFinite()) {
            log_manager_.getLogger("ppp_model")->debug("Invalid geometry for satellite {} at TOW {:.3f}", obs.satellite, obs.time.tow);
            continue;
        }
        if (obs.elevation < config_.getElevationMaskDeg()) {
            log_manager_.getLogger("ppp_model")->debug(
                "Satellite {} below elevation mask ({} < {} deg)", obs.satellite, obs.elevation, config_.getElevationMaskDeg());
            continue;
        }

        for (const auto& [type, observed] : obs.observed) {
            if (type.find("C1") != std::string::npos && type.find("C2") != std::string::npos) {
                if (!obs.modeled.count(type) || !obs.variances.count(type)) {
                    log_manager_.getLogger("ppp_model")->debug(
                        "Missing modeled or variance data for {} {} at TOW {:.3f}", obs.satellite, type, obs.time.tow);
                    continue;
                }
                observation_vector.push_back(observed);
                modeled_vector.push_back(obs.modeled.at(type));
                variances.push_back(obs.variances.at(type));
                mappings.push_back(obs.mf_w);
                satellite_ids.push_back(obs.satellite);
                combo_types.push_back(type);
                sat_positions.push_back(obs.sat_pos);
                initial_ranges.push_back(obs.range);
                elevations.push_back(obs.elevation);
            }
        }
    }

    if (observation_vector.size() < kCoreSize) {
        log_manager_.getLogger("ppp_model")->warn(
            "Insufficient iono-free code combinations for WLS at TOW {:.3f}: {} available, {} required",
            epoch_time.tow, observation_vector.size(), kCoreSize);
        return result;
    }

    Eigen::VectorXd state_vector = state_manager_.getStateVector();
    Eigen::Vector3d current_pos = state_vector.segment<3>(state_manager_.getPositionIndex());
    double current_clock = state_vector(state_manager_.getClockIndex());
    double current_trop_z = state_vector(state_manager_.getTropoIndex());

    constexpr int kMaxIter = 5;
    constexpr double kTol = 0.1;
    constexpr double kMinVariance = 1e-6;

    Eigen::MatrixXd HtWH;
    int iter = 0;
    double epoch_tow = epoch_time.tow;
    const std::string epoch_tag = TimeUtils::formatGnssTime(epoch_time);

    for (iter = 0; iter < kMaxIter; ++iter) {
        Eigen::MatrixXd H(observation_vector.size(), kCoreSize);
        Eigen::VectorXd z(observation_vector.size());
        Eigen::MatrixXd W = Eigen::MatrixXd::Zero(observation_vector.size(), observation_vector.size());

        for (size_t i = 0; i < observation_vector.size(); ++i) {
            const Eigen::Vector3d delta_r = sat_positions[i] - current_pos;
            const double current_range = delta_r.norm();
            if (current_range == 0.0) continue;
            const Eigen::Vector3d current_los = delta_r / current_range;

            const double range_delta = current_range - initial_ranges[i];
            const double predicted = (modeled_vector[i] + range_delta) + current_clock + mappings[i] * current_trop_z;

            z(i) = observation_vector[i] - predicted;

            H(i, 0) = -current_los.x();
            H(i, 1) = -current_los.y();
            H(i, 2) = -current_los.z();
            H(i, 3) = 1.0;
            H(i, 4) = mappings[i];

            const double var_i = std::max(variances[i], kMinVariance);
            W(i, i) = 1.0 / var_i;
        }

        HtWH = H.transpose() * W * H;
        Eigen::JacobiSVD<Eigen::MatrixXd> svd(HtWH);
        const double cond = svd.singularValues()(0) /
            svd.singularValues()(svd.singularValues().size() - 1);

        if (wls_logger_) wls_logger_->logDesignMatrix(H, "WLS Design Matrix H", epoch_tow, iter + 1, satellite_ids, combo_types, epoch_tag);
        if (wls_logger_) wls_logger_->logInnovationsVector(z, "WLS Innovations Vector z", epoch_tow, iter + 1, satellite_ids, combo_types, epoch_tag);
        if (wls_logger_) wls_logger_->logWeightMatrix(W, "WLS Weight Matrix W", epoch_tow, iter + 1, epoch_tag);
        if (wls_logger_) wls_logger_->logNormalMatrix(HtWH, "WLS Normal Matrix HtWH", epoch_tow, iter + 1, cond, epoch_tag);

        if (HtWH.determinant() <= 1e-6) {
            log_manager_.getLogger("ppp_model")->warn(
                "WLS failed at TOW {:.3f}, iter {}: singular HtWH, det={:.3e}", epoch_tow, iter, HtWH.determinant());
        }

        const Eigen::VectorXd delta = HtWH.inverse() * H.transpose() * W * z;
        current_pos += delta.segment<3>(0);
        current_clock += delta(3);
        current_trop_z += delta(4);

        Eigen::VectorXd current_state(kCoreSize);
        current_state << current_pos, current_clock, current_trop_z;
        if (wls_logger_) wls_logger_->logStateVector(current_state, "WLS Intermediate State Vector", epoch_tow, iter + 1, epoch_tag);

        if (delta.segment<3>(0).norm() < kTol) {
            break;
        }
    }

    Eigen::VectorXd init_state(kCoreSize);
    init_state << current_pos, current_clock, current_trop_z;
    Eigen::MatrixXd init_covariance = HtWH.inverse();

    double rms = std::numeric_limits<double>::infinity();
    double max_abs = std::numeric_limits<double>::infinity();
    if (!observation_vector.empty()) {
        double sum_sq = 0.0;
        double local_max = 0.0;
        for (size_t i = 0; i < observation_vector.size(); ++i) {
            const Eigen::Vector3d delta_r = sat_positions[i] - current_pos;
            const double r = delta_r.norm();
            if (!(r > 0.0 && std::isfinite(r))) continue;
            const double range_delta = r - initial_ranges[i];
            const double pred = (modeled_vector[i] + range_delta) + current_clock + mappings[i] * current_trop_z;
            const double e = observation_vector[i] - pred;
            sum_sq += e * e;
            local_max = std::max(local_max, std::abs(e));
        }
        rms = std::sqrt(sum_sq / static_cast<double>(observation_vector.size()));
        max_abs = local_max;
    }

    log_manager_.getLogger("ppp_model")->info(
        "Iterated WLS converged after {} iterations using {} iono-free code combos at TOW {:.3f}: "
        "pos=({:.3f}, {:.3f}, {:.3f}) m, clock={:.3e} m, tropo={:.3f} m, code_rms={:.3f} m, code_max_abs={:.3f} m",
        iter + 1, observation_vector.size(), epoch_tow,
        init_state(0), init_state(1), init_state(2),
        init_state(3), init_state(4), rms, max_abs);

    if (wls_logger_) wls_logger_->logStateVector(init_state, "WLS Final State Vector", epoch_tow, iter + 1, epoch_tag);
    if (wls_logger_) wls_logger_->logCovarianceMatrix(init_covariance, "WLS Final Covariance Matrix", epoch_tow, iter + 1, epoch_tag);

    result.state = std::move(init_state);
    result.covariance = std::move(init_covariance);
    result.rms = rms;
    result.max_abs = max_abs;
    result.observation_count = observation_vector.size();
    result.iterations = iter + 1;
    return result;
}

} // namespace GnssProcessor