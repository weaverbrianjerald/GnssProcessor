#include "KalmanFilter.hpp"
#include "KalmanFilterLogger.hpp"
#include "MeasurementModel.hpp"
#include "PppModel.hpp"
#include <spdlog/spdlog.h>
#include <Eigen/Dense>
#include <Eigen/QR>
#include <stdexcept>
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

        // Initialize state vector and matrices from StateManager
        Eigen::VectorXd x_ = state_manager_.getStateVector();
        Eigen::MatrixXd P_ = state_manager_.getCovariance();
        //Eigen::MatrixXd Q_ = state_manager_.getProcessNoiseMatrix(/*dt*/);

        // Set initial variances for core states from config
        const auto& initial_std = config_.getInitialStd();
        double pos_std = initial_std.position > 0 ? initial_std.position : 100.0;
        double clock_std = initial_std.clock > 0 ? initial_std.clock : 300.0;
        double tropo_std = initial_std.troposphere > 0 ? initial_std.troposphere : 0.5;
        double iono_std = initial_std.ionosphere > 0 ? initial_std.ionosphere : 5.0;
        double amb_std = initial_std.ambiguity > 0 ? initial_std.ambiguity : 10.0;

        size_t pos_idx = state_manager_.getPositionIndex();
        size_t clock_idx = state_manager_.getClockIndex();
        size_t tropo_idx = state_manager_.getTropoIndex();
        size_t iono_idx = state_manager_.getIonoIndex();

        P_.block(pos_idx, pos_idx, 3, 3) = Eigen::Matrix3d::Identity() * pos_std * pos_std;
        P_(clock_idx, clock_idx) = clock_std * clock_std;
        P_(tropo_idx, tropo_idx) = tropo_std * tropo_std;
        if (config_.getProcessingMode() == ProcessingMode::PPP_RTK && iono_idx < state_size) {
            P_(iono_idx, iono_idx) = iono_std * iono_std;
        }

        // Set initial variances for ambiguities (if any)
        if (state_size > core_size) {
            P_.bottomRightCorner(state_size - core_size, state_size - core_size).diagonal().setConstant(amb_std * amb_std);
            //R_.bottomRightCorner(state_size - core_size, state_size - core_size).diagonal().setConstant(0.0015 * 0.0015); // Phase variance (m^2)
            log_manager_.getLogger("kalman_filter")->info("Initialized {} ambiguity states with variance: {:.6f} meters^2",
                state_size - core_size, amb_std * amb_std);
        }

        log_manager_.getLogger("kalman_filter")->info("KalmanFilter initialized with state size: {}, mode: {}, P_ ({}x{}), x_ ({})",
            state_size, config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK",
            P_.rows(), P_.cols(), x_.size());

        // Log initial state and covariance
        state_manager_.setCovariance(P_);
        logger_.logStateVector(x_, "Initial State Vector");
        logger_.logCovarianceMatrix(P_, "Initial Covariance Matrix");
    }

    KalmanFilter::~KalmanFilter() = default;

    void KalmanFilter::predict(double dt) {
        Eigen::VectorXd x_ = state_manager_.getStateVector();
        Eigen::MatrixXd P_ = state_manager_.getCovariance();
        if (x_.size() == 0 || P_.rows() == 0 || P_.cols() == 0) {
            log_manager_.getLogger("kalman_filter")->error("Filter not initialized");
            throw std::runtime_error("Filter not initialized");
        }

        // Get total state size (core states + ambiguities)
        size_t state_size = state_manager_.getStateSize();

        // Get process noise matrix
        Eigen::MatrixXd Q_ = state_manager_.getProcessNoiseMatrix(dt);
        if (Q_.rows() != static_cast<long>(state_size) || Q_.cols() != static_cast<long>(state_size)) {
            log_manager_.getLogger("kalman_filter")->error("Process noise Q_ dimensions incompatible: Q_ ({}x{}), expected ({}x{})",
                Q_.rows(), Q_.cols(), state_size, state_size);
            throw std::invalid_argument("Q_ dimensions mismatch");
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
        logger_.logMatrix(Q_, "Process Noise Matrix Q");

        // Predict state
        x_ = F * x_;
        P_ = F * P_ * F.transpose() + Q_;
        log_manager_.getLogger("kalman_filter")->trace("Predicted state for dt={:.3f} s: x_ size={}, P_ size={}x{}",
            dt, x_.size(), P_.rows(), P_.cols());

        // Store
        state_manager_.setStateVector(x_);
        state_manager_.setCovariance(P_);
        //state_manager_.setStateVectorPrevious(x_);
        //state_manager_.setCovariancePrevious(P_);

        // Log after prediction
        logger_.logStateVector(x_, "Predicted State Vector");
        logger_.logCovarianceMatrix(P_, "Predicted Covariance Matrix");
    }

    void KalmanFilter::update(EpochMeasurement& measurement) {
        if (measurement.innovations.size() == 0 || measurement.H.rows() == 0 || measurement.R_v.size() == 0) {
            log_manager_.getLogger("kalman_filter")->warn("Invalid measurement update: innovations ({}), H ({}x{}), R_v ({})",
                measurement.innovations.size(), measurement.H.rows(), measurement.H.cols(), measurement.R_v.size());
            return;
        }

        size_t state_size = state_manager_.getStateSize();
        if (static_cast<size_t>(measurement.H.cols()) != state_size) {
            log_manager_.getLogger("kalman_filter")->error("Measurement matrix H columns ({}) do not match state size ({})",
                measurement.H.cols(), state_size);
            throw std::invalid_argument("H dimensions mismatch");
        }

        bool is_outlier_found = true;
        int iter = 0;
        while (is_outlier_found && iter < 100) {
            iter++;

            // Construct measurement noise matrix R
            R_ = Eigen::MatrixXd::Zero(measurement.innovations.size(), measurement.innovations.size());
            for (Eigen::Index i = 0; i < measurement.innovations.size(); ++i) {
                R_(i, i) = measurement.R_v(i);
            }

            // Get state and covariance
            Eigen::VectorXd x_ = state_manager_.getStateVector();
            Eigen::MatrixXd P_ = state_manager_.getCovariance();

            // Store state and covariance before the update (i.e., the last stable conditions)
            //state_manager_.setStateVectorPrevious(x_);
            //state_manager_.setCovariancePrevious(P_);

            // Log measurement-related components
            logger_.logVector(measurement.innovations, "Innovations Vector");
            logger_.logMatrix(measurement.H, "Measurement Matrix H");
            logger_.logMatrix(R_, "Measurement Noise Matrix R");

            // Kalman gain
            Eigen::MatrixXd Ht = measurement.H.transpose();
            Eigen::MatrixXd S = measurement.H * P_ * Ht + R_;
            Eigen::JacobiSVD<Eigen::MatrixXd> svd(S);
            double cond = svd.singularValues()(0) / svd.singularValues()(svd.singularValues().size() - 1);
            log_manager_.getLogger("kalman_filter")->trace("S matrix condition number: {:.3e}", cond);
            Eigen::MatrixXd S_inv;
            if (S.determinant() < 1e-6 || cond > 1e10) {
                log_manager_.getLogger("kalman_filter")->warn("Ill-conditioned S matrix, using pseudo-inverse, determinant={:.3e}, condition={:.3e}",
                    S.determinant(), cond);
                S_inv = Eigen::CompleteOrthogonalDecomposition<Eigen::MatrixXd>(S).pseudoInverse();
            }
            else {
                S_inv = S.inverse();
            }
            Eigen::MatrixXd K = P_ * Ht * S_inv;

            // Log Kalman gain
            logger_.logKalmanGain(K, "Kalman Gain Matrix K");

            // Store pre-update state and covariance for potential reset
            Eigen::VectorXd x_prev = x_;
            Eigen::MatrixXd P_prev = P_;

            // Update state
            Eigen::VectorXd dx = K * measurement.innovations;
            x_ = x_ + dx;

            // Update covariance using Joseph form: P = (I - K H) P (I - K H)^T + K R K^T
            Eigen::MatrixXd I = Eigen::MatrixXd::Identity(state_size, state_size);
            Eigen::MatrixXd KH = K * measurement.H;
            Eigen::MatrixXd I_minus_KH = I - KH;
            P_ = I_minus_KH * P_ * I_minus_KH.transpose() + K * R_ * K.transpose();

            // Log update details
            log_manager_.getLogger("kalman_filter")->trace("Kalman filter update: innovations size={}, H ({}x{}), R ({}x{}), K ({}x{}), mode={}",
                measurement.innovations.size(), measurement.H.rows(), measurement.H.cols(), R_.rows(), R_.cols(), K.rows(), K.cols(),
                config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");

            // Compute post-fit residuals
            Eigen::VectorXd res = measurement.innovations - measurement.H * dx;
            measurement.residuals = res;

            // Compute residual covariance: vres = abs(R + H*P*H')
            //Eigen::MatrixXd vres = (R_ + (measurement.H * P_ * measurement.H.transpose())).cwiseAbs();
            //measurement.residuals_covariance = vres;
            Eigen::MatrixXd I_m = Eigen::MatrixXd::Identity(measurement.H.rows(), measurement.H.rows());
            Eigen::MatrixXd HK = measurement.H * K; // rows(obs) x rows(obs) after multiplication
            Eigen::MatrixXd I_minus_HK = I_m - HK;
            Eigen::MatrixXd vres = I_minus_HK * S * I_minus_HK.transpose();
            measurement.residuals_covariance = vres;

            // Standardized residuals: sres_i = |res_i| / sqrt(vres_ii)
            Eigen::VectorXd sres(res.size());
            for (Eigen::Index i = 0; i < res.size(); ++i) {
                double var_i = std::max(vres(i, i), 1e-6); // avoid tiny/negative
                sres(i) = std::abs(res(i)) / std::sqrt(var_i);
                if (!std::isfinite(sres(i))) sres(i) = 0.0;
            }
            measurement.residuals_standardized = sres;

            // Validate post-fit residuals and revert if necessary
            if (validatePostFitResiduals(measurement, x_prev, P_prev)) {
                // Revert to pre-update state if outliers detected
                x_ = x_prev;
                P_ = P_prev;
                log_manager_.getLogger("kalman_filter")->info("Reverted to pre-update state due to outliers");
            }
            else {
                is_outlier_found = false;
            }

        // Update StateManager
        state_manager_.setStateVector(x_);
        state_manager_.setCovariance(P_);
        state_manager_.setStateEpochTime(measurement.time);

        // Log updated state and covariance
        logger_.logStateVector(x_, "Updated State Vector");
        logger_.logCovarianceMatrix(P_, "Updated Covariance Matrix");

        log_manager_.getLogger("kalman_filter")->trace("Kalman filter update: innovations size={}, H ({}x{}), R ({}x{}), K ({}x{}), mode={}",
            measurement.innovations.size(), measurement.H.rows(), measurement.H.cols(),
            measurement.R_v.rows(), measurement.R_v.cols(), K.rows(), K.cols(),
            config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
        }

    }

    ///*
    bool KalmanFilter::validatePostFitResiduals(EpochMeasurement& measurement, Eigen::VectorXd& x_, Eigen::MatrixXd& P_) {
        // Configurable parameters (from config_)
        const double threshold = 6.0; // Standardized residual threshold (e.g., 6-sigma)
        const double bad_meas_percent = 0.5; // Fraction of bad measurements triggering complete reset
        const double amb_std = 500.0; // Initial ambiguity standard deviation (m)
        const double code_noise_amplify = 500.0; // Amplified code variance for outliers (m^2)

        bool is_outlier_found = false;

        //Eigen::VectorXd x_ = state_manager_.getStateVector();
        //Eigen::MatrixXd P_ = state_manager_.getCovariance();
        //Eigen::VectorXd x_prev = state_manager_.getStateVectorPrevious();
        //Eigen::MatrixXd P_prev = state_manager_.getCovariancePrevious();

        // Extract post-fit residuals
        Eigen::VectorXd res = measurement.residuals;

        // Extract residual covariance: vres = abs(R + H*P*H')
        Eigen::MatrixXd vres = measurement.residuals_covariance;

        // Extract standardized residual
        Eigen::VectorXd sres = measurement.residuals_standardized;

        size_t num_obs = res.size();
        if (num_obs == 0) {
            log_manager_.getLogger("kalman_filter")->warn("No observations for post-fit validation");
            return is_outlier_found;
        }

        // Find the maximum standardized residual above threshold
        size_t max_idx = 0;
        double max_sres = sres.maxCoeff(&max_idx);
        size_t num_phase = 0;
        size_t num_bad_meas_iter = 0;
        std::vector<bool> bad_meas_idx(num_obs, false);

        // Count phase measurements and bad measurements
        for (size_t i = 0; i < num_obs; ++i) {
            if (measurement.signal_types[i][0] == 'L') ++num_phase;
            if (sres(i) > threshold) {
                ++num_bad_meas_iter; // TODO: not used because it is contaminated by outlier(s)
                bad_meas_idx[i] = true;
            }
        }

        // Handle the maximum residual if it exceeds the threshold
        if (max_sres > threshold) {
            log_manager_.getLogger("kalman_filter")->warn(
                "Maximum standardized residual for {} {}: {:.3f} (residual {:.3f} m, sigma {:.3f} m)",
                measurement.satellite_ids[max_idx], measurement.signal_types[max_idx],
                max_sres, res(max_idx), std::sqrt(vres(max_idx, max_idx)));

            if (measurement.signal_types[max_idx][0] == 'L') {
                // Phase measurement: reinitialize ambiguity
                std::string amb_key = measurement.satellite_ids[max_idx] + "_" + measurement.signal_types[max_idx];
                int amb_idx = state_manager_.getAmbiguityIndex(amb_key);
                if (amb_idx >= 0) {
                    x_(amb_idx) = 0.0;
                    P_(amb_idx, amb_idx) = amb_std * amb_std;
                    P_.row(amb_idx).setZero();
                    P_.col(amb_idx).setZero();
                    log_manager_.getLogger("kalman_filter")->info(
                        "Reinitialized ambiguity for {} {} (index {}): 0.0 m",
                        measurement.satellite_ids[max_idx], measurement.signal_types[max_idx], amb_idx);
                }
            }
            else {
                // Code measurement: amplify R matrix value
                measurement.R_v(max_idx) = code_noise_amplify;
                log_manager_.getLogger("kalman_filter")->info(
                    "Amplified code noise for {} {}: new variance {:.3f} m^2",
                    measurement.satellite_ids[max_idx], measurement.signal_types[max_idx], code_noise_amplify);
            }
            is_outlier_found = true; // found an outlier
            measurement.bad_residual_count++;
        }
        else {
            // Log all residuals if no outlier is found
            for (size_t i = 0; i < num_obs; ++i) {
                log_manager_.getLogger("kalman_filter")->trace(
                    "Standardized residual for {} {}: {:.3f} (residual {:.3f} m, sigma {:.3f} m)",
                    measurement.satellite_ids[i], measurement.signal_types[i],
                    sres(i), res(i), std::sqrt(vres(i, i)));
            }
        }

        size_t num_bad_meas = measurement.bad_residual_count;

        // Check if filter reset is needed due to excessive outliers
        if (num_bad_meas > static_cast<size_t>(bad_meas_percent * num_obs)) {
            log_manager_.getLogger("kalman_filter")->warn(
                "Resetting filter: too many outliers ({}/{})", num_bad_meas, num_obs);
            //x_prev.setZero(); // TODO: Reset ambiguities
            P_.setZero();
            size_t core_size = config_.getProcessingMode() == ProcessingMode::PPP ? 5 : 6;
            P_.diagonal().head(core_size) << 10000.0, 10000.0, 10000.0, // Position (100 m std)
                90000.0, // Clock (300 m std)
                0.25; // Tropo (0.5 m std)
            if (core_size == 6) {
                P_(5, 5) = 25.0; // Iono (5 m std)
            }
            P_.diagonal().tail(num_phase).setConstant(amb_std * amb_std);
            log_manager_.getLogger("kalman_filter")->info("Filter reset: state and covariance reinitialized");
            is_outlier_found = true;
        }

        /*
        if (is_outlier_found) {
            // Update StateManager with modified state and covariance
            // Use the last stable conditions (previous values)
            state_manager_.setStateVector(x_);
            state_manager_.setCovariance(P_);
            //state_manager_.setStateVectorPrevious(x_prev);
            //state_manager_.setCovariancePrevious(P_prev);

            // Log updated state and covariance
            x_ = state_manager_.getStateVector();
            P_ = state_manager_.getCovariance();
            logger_.logStateVector(x_, "Adapted State Vector");
            logger_.logCovarianceMatrix(P_, "Adapted Covariance Matrix");
        }
        */

        return is_outlier_found;
    }
    //*/

} // namespace GnssProcessor