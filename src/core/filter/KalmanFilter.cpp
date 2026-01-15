#include "KalmanFilter.hpp"
#include "KalmanFilterLogger.hpp"
#include "MeasurementModel.hpp"
#include "PppModel.hpp"
#include "TimeUtils.hpp"
#include <spdlog/spdlog.h>
#include <Eigen/Dense>
#include <Eigen/QR>
#include <stdexcept>
#include <limits>
#include "Constants.hpp"

namespace GnssProcessor {

    KalmanFilter::KalmanFilter(StateManager& state_manager, const GnssConfig& config,
        LogManager& log_manager, const PppModel& ppp_model)
        : state_manager_(state_manager), config_(config), log_manager_(log_manager),
        ppp_model_(ppp_model), measurement_model_(state_manager, ppp_model, config, log_manager),
        logger_("kalman_debug.log") {

        // Get total state size (core states + initial ambiguities)
        size_t core_size = config.getProcessingMode() == ProcessingMode::PPP ? 5 : 6;
        size_t state_size = state_manager_.getStateSize();
        if (state_size < core_size) {
            log_manager_.getLogger("kalman_filter")->error("Invalid state size from StateManager: {}, minimum {} required for mode {}",
                state_size, core_size,
                config.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            throw std::invalid_argument("Invalid state size");
        }

        // Initialize state vector and matrices from StateManager (local temporaries only)
        Eigen::VectorXd x = state_manager_.getStateVector();
        Eigen::MatrixXd P = state_manager_.getCovariance();

        // Set initial variances for core states from config
        const auto& initial_std = config_.getInitialStd();
        double pos_std = initial_std.position > 0 ? initial_std.position : 100.0;
        double clock_std = initial_std.clock > 0 ? initial_std.clock : 100.0;
        double tropo_std = initial_std.troposphere > 0 ? initial_std.troposphere : 0.5;
        double iono_std = initial_std.ionosphere > 0 ? initial_std.ionosphere : 5.0;
        double amb_std = initial_std.ambiguity > 0 ? initial_std.ambiguity : 10.0;

        size_t pos_idx = state_manager_.getPositionIndex();
        size_t clock_idx = state_manager_.getClockIndex();
        size_t tropo_idx = state_manager_.getTropoIndex();
        size_t iono_idx = state_manager_.getIonoIndex();

        auto is_unset = [](double v) {
            // treat zero or non-finite as unset
            return !(std::isfinite(v) && v > 0.0);
        };

        // Only override diagonal entries if they are unset/invalid (allow StateManager seeding to persist)
        if (is_unset(P(pos_idx, pos_idx)))
            P.block(pos_idx, pos_idx, 3, 3) = Eigen::Matrix3d::Identity() * pos_std * pos_std;
        if (is_unset(P(clock_idx, clock_idx)))
            P(clock_idx, clock_idx) = clock_std * clock_std;
        if (is_unset(P(tropo_idx, tropo_idx)))
            P(tropo_idx, tropo_idx) = tropo_std * tropo_std;
        if (config_.getProcessingMode() == ProcessingMode::PPP_RTK && iono_idx < state_size && is_unset(P(iono_idx, iono_idx)))
            P(iono_idx, iono_idx) = iono_std * iono_std;

        // Set initial variances for ambiguities (if any)
        if (state_size > core_size) {
            P.bottomRightCorner(state_size - core_size, state_size - core_size).diagonal().setConstant(amb_std * amb_std);
            log_manager_.getLogger("kalman_filter")->info("Initialized {} ambiguity states with variance: {:.6f} meters^2",
                state_size - core_size, amb_std * amb_std);
        }

        log_manager_.getLogger("kalman_filter")->info("KalmanFilter initialized with state size: {}, mode: {}, P ({}x{}), x ({})",
            state_size, config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK",
            P.rows(), P.cols(), x.size());

        // Persist to StateManager
        state_manager_.setCovariance(P);

        // Log initial state/covariance
        logger_.logStateVector(x, "Initial State Vector");
        logger_.logCovarianceMatrix(P, "Initial Covariance Matrix");
    }

    KalmanFilter::~KalmanFilter() = default;

    void KalmanFilter::predict(double dt) {
        Eigen::VectorXd x = state_manager_.getStateVector();
        Eigen::MatrixXd P = state_manager_.getCovariance();
        if (x.size() == 0 || P.rows() == 0 || P.cols() == 0) {
            log_manager_.getLogger("kalman_filter")->error("Filter not initialized");
            throw std::runtime_error("Filter not initialized");
        }

        // Get total state size (core states + ambiguities)
        size_t state_size = state_manager_.getStateSize();

        // Get process noise matrix
        Eigen::MatrixXd Q = state_manager_.getProcessNoiseMatrix(dt);
        if (Q.rows() != static_cast<long>(state_size) || Q.cols() != static_cast<long>(state_size)) {
            log_manager_.getLogger("kalman_filter")->error("Process noise Q dimensions incompatible: Q ({}x{}), expected ({}x{})",
                Q.rows(), Q.cols(), state_size, state_size);
            throw std::invalid_argument("Q dimensions mismatch");
        }

        // Get transition matrix
        Eigen::MatrixXd F = state_manager_.getTransitionMatrix(dt);
        if (F.rows() != static_cast<long>(state_size) || F.cols() != static_cast<long>(state_size)) {
            log_manager_.getLogger("kalman_filter")->error("Transition matrix F dimensions incompatible: F ({}x{}), expected ({}x{})",
                F.rows(), F.cols(), state_size, state_size);
            throw std::invalid_argument("F dimensions mismatch");
        }

        // Log before prediction
        logger_.logMatrix(F, "Transition Matrix F");
        logger_.logMatrix(Q, "Process Noise Matrix Q");

        // Predict state
        x = F * x;
        P = F * P * F.transpose() + Q;
        log_manager_.getLogger("kalman_filter")->trace("Predicted state for dt={:.3f} s: x size={}, P size={}x{}",
            dt, x.size(), P.rows(), P.cols());

        // Store
        state_manager_.setStateVector(x);
        state_manager_.setCovariance(P);

        // Log after prediction
        logger_.logStateVector(x, "Predicted State Vector");
        logger_.logCovarianceMatrix(P, "Predicted Covariance Matrix");
    }

    KalmanFilter::UpdateResult KalmanFilter::update(EpochMeasurement& measurement) {
        auto klogger = log_manager_.getLogger("kalman_filter");
        const std::string epoch_tag = TimeUtils::formatGnssTime(measurement.time);
        const std::string epoch_prefix = "[" + epoch_tag + "] ";
        const std::string epoch_label = " @ " + epoch_tag;

        if (measurement.innovations.size() == 0 || measurement.H.rows() == 0 || measurement.R_v.size() == 0) {
            klogger->warn("{}Invalid measurement update: innovations ({}), H ({}x{}), R_v ({})",
                epoch_prefix,
                measurement.innovations.size(), measurement.H.rows(), measurement.H.cols(), measurement.R_v.size());
            return UpdateResult::Skipped;
        }

        const size_t state_size = state_manager_.getStateSize();
        if (static_cast<size_t>(measurement.H.cols()) != state_size) {
            klogger->error("{}Measurement matrix H columns ({}) do not match state size ({})",
                epoch_prefix, measurement.H.cols(), state_size);
            throw std::invalid_argument("H dimensions mismatch");
        }

        // Build diagonal R
        R_ = Eigen::MatrixXd::Zero(measurement.innovations.size(), measurement.innovations.size());
        for (Eigen::Index i = 0; i < measurement.innovations.size(); ++i) {
            R_(i, i) = measurement.R_v(i);
        }

        // Pull current state
        Eigen::VectorXd x = state_manager_.getStateVector();
        Eigen::MatrixXd P = state_manager_.getCovariance();

        logger_.logVector(measurement.innovations, "Innovations Vector", epoch_tag);
        logger_.logMatrix(measurement.H, "Measurement Matrix H", epoch_tag);
        logger_.logMatrix(R_, "Measurement Noise Matrix R", epoch_tag);

        // Kalman gain
        Eigen::MatrixXd Ht = measurement.H.transpose();
        Eigen::MatrixXd S = measurement.H * P * Ht + R_;
        Eigen::JacobiSVD<Eigen::MatrixXd> svd(S);
        double cond = svd.singularValues()(0) / svd.singularValues()(svd.singularValues().size() - 1);
        klogger->trace("{}S matrix condition number: {:.3e}", epoch_prefix, cond);

        Eigen::MatrixXd S_inv;
        if (S.determinant() < 1e-6 || cond > 1e10) {
            klogger->warn("{}Ill-conditioned S matrix, using pseudo-inverse, determinant={:.3e}, condition={:.3e}",
                epoch_prefix, S.determinant(), cond);
            S_inv = Eigen::CompleteOrthogonalDecomposition<Eigen::MatrixXd>(S).pseudoInverse();
        } else {
            S_inv = S.inverse();
        }

        // -----------------------------------------------------------------
        // Pre-fit gating using innovations and the innovation covariance S
        //
        // Per-measurement normalized innovation (NIV):
        //   gamma_i = v_i^2 / S_ii
        //
        // Global normalized innovation squared (NIS):
        //   NIS = v^T S^{-1} v
        //
        // This is more stable than post-fit residual covariance heuristics.
        // -----------------------------------------------------------------
        const Eigen::VectorXd& v = measurement.innovations;

        // Store per-measurement normalized innovations into residuals_standardized for logging/QC.
        // Note: this is "innovation standardized", not post-fit residual standardized.
        Eigen::VectorXd niv(v.size());
        for (Eigen::Index i = 0; i < v.size(); ++i) {
            const double sii = (i < S.rows() && i < S.cols()) ? S(i, i) : std::numeric_limits<double>::quiet_NaN();
            const double denom = std::max(sii, 1e-6); // guard
            const double gi = (v(i) * v(i)) / denom;  // chi-square with 1 dof (approximately)
            niv(i) = std::isfinite(gi) ? std::sqrt(gi) : 0.0; // keep comparable scale to a "sigma" count
        }
        measurement.residuals_standardized = niv;

        const double nis = (v.transpose() * S_inv * v)(0, 0);
        klogger->debug(
            "{}Pre-fit gating: n_meas={}, NIS={:.3f}, cond(S)={:.3e}",
            epoch_prefix, static_cast<int>(v.size()), nis, cond);

        Eigen::MatrixXd K = P * Ht * S_inv;
        logger_.logKalmanGain(K, "Kalman Gain Matrix K", epoch_tag);

        // Save pre-update copy for outlier handling (and *allow* validatePostFitResiduals to modify these)
        Eigen::VectorXd x_prev = x;
        Eigen::MatrixXd P_prev = P;

        // Update
        Eigen::VectorXd dx = K * measurement.innovations;
        x = x + dx;

        klogger->debug("{}Kalman update dx.norm={:.3e}", epoch_prefix, dx.norm());

        // Joseph covariance update
        Eigen::MatrixXd I = Eigen::MatrixXd::Identity(state_size, state_size);
        Eigen::MatrixXd KH = K * measurement.H;
        Eigen::MatrixXd I_minus_KH = I - KH;
        P = I_minus_KH * P * I_minus_KH.transpose() + K * R_ * K.transpose();

        // Post-fit residuals: r = v - H*dx = (I - H K) v
        measurement.residuals = measurement.innovations - measurement.H * dx;

        // Post-fit residual covariance: Sr = (I - H K) * S * (I - H K)^T
        // Use measurement-space identity (m x m) because S is m x m.
        const Eigen::Index m = measurement.H.rows();
        Eigen::MatrixXd Im = Eigen::MatrixXd::Identity(m, m); // m x m
        Eigen::MatrixXd HK = measurement.H * K;               // m x m
        Eigen::MatrixXd W = Im - HK;                          // m x m
        Eigen::MatrixXd Sr = W * S * W.transpose();           // m x m
        measurement.residuals_covariance = Sr;

        // Standardized post-fit residuals (sigma units) for QC/outlier checks
        Eigen::VectorXd sres(measurement.residuals.size());
        for (Eigen::Index i = 0; i < sres.size(); ++i) {
            const double sii = (i < Sr.rows() && i < Sr.cols()) ? Sr(i, i) : std::numeric_limits<double>::quiet_NaN();
            const double denom = std::sqrt(std::max(sii, 1e-12));
            const double val = measurement.residuals(i);
            sres(i) = std::isfinite(val) && std::isfinite(denom) && denom > 0.0 ? (val / denom) : 0.0;
        }
        measurement.residuals_standardized = sres;

        // Outlier handling: may modify x_prev/P_prev (ambiguity reset/R inflation/full reset)
        bool is_residual_outlier = false;
        bool is_complete_reset = false;
        std::tie(is_residual_outlier, is_complete_reset) = validatePostFitResiduals(measurement, x_prev, P_prev);

        if (is_residual_outlier) {
            state_manager_.setStateVector(x_prev);
            state_manager_.setCovariance(P_prev);

            if (is_complete_reset) {
                klogger->info("{}Complete filter reset requested by residual check", epoch_prefix);
            } else {
                klogger->info("{}Outlier handled (ambiguity reset/R change). Measurement rebuild required.", epoch_prefix);
            }

            logger_.logStateVector(x_prev, "State After Outlier Handling", epoch_tag);
            logger_.logCovarianceMatrix(P_prev, "Covariance After Outlier Handling", epoch_tag);

			return UpdateResult::NeedsMeasurementRebuild; // cancel the update and reconstruct measurements for fresh innovations
        }

        // Accept update
        state_manager_.setStateVector(x);
        state_manager_.setCovariance(P);
        state_manager_.setStateEpochTime(measurement.time);

        logger_.logStateVector(x, "Updated State Vector", epoch_tag);
        logger_.logCovarianceMatrix(P, "Updated Covariance Matrix", epoch_tag);

        return UpdateResult::Applied;
    }

    std::pair<bool, bool> KalmanFilter::validatePostFitResiduals(EpochMeasurement& measurement, Eigen::VectorXd& x, Eigen::MatrixXd& P) {
        auto klogger = log_manager_.getLogger("kalman_filter");
        const std::string epoch_tag = TimeUtils::formatGnssTime(measurement.time);
        const std::string epoch_prefix = "[" + epoch_tag + "] ";

        // (unchanged body; it already operates on passed-in x/P references)
        const double threshold = config_.getResidualThreshold();
        const double bad_meas_percent = config_.getBadMeasPercent();
        const double amb_std = config_.getAmbiguityStd();        // meters
        const double code_noise_amplify = config_.getCodeNoiseAmplify(); // m^2

        bool is_outlier_found = false;
        bool is_complete_reset = false;

        Eigen::VectorXd res = measurement.residuals;
        Eigen::MatrixXd vres = measurement.residuals_covariance;
        Eigen::VectorXd sres = measurement.residuals_standardized;

        const size_t num_obs = static_cast<size_t>(res.size());
        if (num_obs == 0) {
            klogger->warn("{}No observations for post-fit validation", epoch_prefix);
            return { false, false };
        }

        const size_t meta_n = std::min({ num_obs, measurement.satellite_ids.size(), measurement.signal_types.size() });

        klogger->debug(
            "{}Post-fit residual check: n_obs={}, threshold={:.3f}, bad_count={}, bad_percent={:.3f}",
            epoch_prefix, num_obs, threshold, measurement.bad_residual_count, bad_meas_percent);

        for (size_t i = 0; i < meta_n; ++i) {
            const double var_i = (i < static_cast<size_t>(vres.rows()) && i < static_cast<size_t>(vres.cols()))
                ? vres(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(i))
                : std::numeric_limits<double>::quiet_NaN();
            const double sigma_i = std::sqrt(std::max(var_i, 0.0));
            const double s_i = sres(static_cast<Eigen::Index>(i));
            const bool is_out = std::isfinite(s_i) && (s_i > threshold);

            klogger->debug(
                "{}  res[{:02d}] {} {}: v={:+.4f} m, sigma={:.4f} m, s={:.3f}, outlier={}",
                epoch_prefix,
                static_cast<int>(i),
                measurement.satellite_ids[i],
                measurement.signal_types[i],
                res(static_cast<Eigen::Index>(i)),
                sigma_i,
                s_i,
                is_out ? "YES" : "no");
        }

        if (meta_n != num_obs) {
            klogger->warn(
                "{}Residual metadata size mismatch: res={}, sat_ids={}, sig_types={}. Logged {} entries.",
                epoch_prefix,
                num_obs, measurement.satellite_ids.size(), measurement.signal_types.size(), meta_n);
        }

        size_t max_idx = 0;
        const double max_sres = sres.maxCoeff(&max_idx);

        size_t num_phase = 0;
        for (size_t i = 0; i < num_obs; ++i) {
            if (!measurement.signal_types[i].empty() && measurement.signal_types[i][0] == 'L') ++num_phase;
        }

        size_t num_over = 0;
        for (Eigen::Index i = 0; i < sres.size(); ++i) {
            if (std::isfinite(sres(i)) && sres(i) > threshold) ++num_over;
        }

        klogger->debug(
            "{}Residual summary: max_sres={:.3f} at idx={}, over_threshold={}/{}, num_phase={}",
            epoch_prefix,
            max_sres, max_idx, num_over, num_obs, num_phase);

        if (max_sres > threshold) {
            const double sigma_max = std::sqrt(std::max(vres(static_cast<Eigen::Index>(max_idx), static_cast<Eigen::Index>(max_idx)), 0.0));

            klogger->warn(
                "{}Maximum standardized residual for {} {}: {:.3f} (residual {:.3f} m, sigma {:.3f} m, idx {})",
                epoch_prefix,
                measurement.satellite_ids[max_idx], measurement.signal_types[max_idx],
                max_sres, res(static_cast<Eigen::Index>(max_idx)), sigma_max, max_idx);

            if (!measurement.signal_types[max_idx].empty() && measurement.signal_types[max_idx][0] == 'L') {
                const std::string amb_key = measurement.satellite_ids[max_idx] + "_" + measurement.signal_types[max_idx];
                const int amb_idx = state_manager_.getAmbiguityIndex(amb_key);

                klogger->info(
                    "{}Phase outlier action: ambiguity reset requested for key={}, amb_idx={}", epoch_prefix, amb_key, amb_idx);

                if (amb_idx >= 0) {
                    const double prev_amb = x(amb_idx);
                    x(amb_idx) = 0.0;

                    P.row(amb_idx).setZero();
                    P.col(amb_idx).setZero();
                    P(amb_idx, amb_idx) = amb_std * amb_std;

                    klogger->info(
                        "{}Reinitialized ambiguity for {} {} (index {}): prev={:.6f} m -> 0.0 m, std={:.3f} m",
                        epoch_prefix,
                        measurement.satellite_ids[max_idx], measurement.signal_types[max_idx], amb_idx, prev_amb, amb_std);
                } else {
                    klogger->warn("{}Phase outlier but ambiguity key not found: {}", epoch_prefix, amb_key);
                }
            } else {
                const double base_var = measurement.R_v(static_cast<Eigen::Index>(max_idx));
                double factor = 1.0;

                if (std::isfinite(base_var) && base_var > 0.0) {
                    factor = code_noise_amplify / base_var;
                }
                if (!std::isfinite(factor) || factor < 1.0) factor = 1.0;

                state_manager_.inflateMeasurementVarianceScale(
                    measurement.satellite_ids[max_idx],
                    measurement.signal_types[max_idx],
                    factor);

                klogger->info(
                    "{}Code outlier action: inflated persistent variance scale for {} {}: base_var={:.3f} m^2, target_var={:.3f} m^2, factor={:.3f}",
                    epoch_prefix,
                    measurement.satellite_ids[max_idx],
                    measurement.signal_types[max_idx],
                    base_var,
                    code_noise_amplify,
                    factor);
            }

            is_outlier_found = true;
            measurement.bad_residual_count++;

            klogger->warn(
                "{}Outlier flagged: idx={}, sat={}, sig={}, max_sres={:.3f} > threshold={:.3f}; bad_residual_count={}",
                epoch_prefix,
                max_idx,
                measurement.satellite_ids[max_idx],
                measurement.signal_types[max_idx],
                max_sres,
                threshold,
                measurement.bad_residual_count);
        } else {
            klogger->debug(
                "{}No outliers: max_sres={:.3f} <= threshold={:.3f}", epoch_prefix, max_sres, threshold);
        }

        if (measurement.bad_residual_count > static_cast<size_t>(bad_meas_percent * num_obs)) {
            klogger->warn(
                "{}Resetting filter: too many outliers ({}/{}), limit={:.3f}*n={}",
                epoch_prefix,
                measurement.bad_residual_count,
                num_obs,
                bad_meas_percent,
                static_cast<size_t>(bad_meas_percent * num_obs));

            P.setZero();
            const size_t core_size = config_.getProcessingMode() == ProcessingMode::PPP ? 5 : 6;

            P.diagonal().head(core_size) << 10000.0, 10000.0, 10000.0,
                90000.0,
                0.25;

            if (core_size == 6) {
                P(5, 5) = 25.0;
            }

            const size_t state_size = static_cast<size_t>(P.rows());
            if (state_size > core_size) {
                P.diagonal().tail(state_size - core_size).setConstant(amb_std * amb_std);
            }

            is_outlier_found = true;
            is_complete_reset = true;
            measurement.bad_residual_count = 0;

            klogger->warn(
                "{}Complete reset applied: P core_size={}, state_size={}, ambiguity_std={:.3f} m",
                epoch_prefix, core_size, state_size, amb_std);
        }

        klogger->debug(
            "{}Post-fit residual check result: outlier_found={}, complete_reset={}",
            epoch_prefix,
            is_outlier_found ? "true" : "false",
            is_complete_reset ? "true" : "false");

        return { is_outlier_found, is_complete_reset };
    }

} // namespace GnssProcessor