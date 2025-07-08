#include "KalmanFilter.hpp"
#include "PppModel.hpp"
#include <spdlog/spdlog.h>
#include <Eigen/Dense>
#include <Eigen/QR>
#include <stdexcept>
#include "Constants.hpp"

namespace GnssProcessor {

    KalmanFilter::KalmanFilter(StateManager& state_manager, const GnssConfig& config, LogManager& log_manager, const PppModel& ppp_model)
        : state_manager_(state_manager), config_(config), log_manager_(log_manager), ppp_model_(ppp_model) {
        // Get total state size (core states + initial ambiguities)
        size_t core_size = config.getProcessingMode() == ProcessingMode::PPP ? 5 : 6;
        size_t state_size = state_manager_.getStateSize();
        if (state_size < core_size) {
            log_manager_.getLogger()->error("Invalid state size from StateManager: {}, minimum {} required for mode {}",
                state_size, core_size,
                config.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            throw std::invalid_argument("Invalid state size");
        }

        // Initialize state vector and matrices
        x_ = Eigen::VectorXd::Zero(state_size);
        P_ = Eigen::MatrixXd::Identity(state_size, state_size);
        R_ = Eigen::MatrixXd::Identity(state_size, state_size);

        // Set initial variances for core states from config
        const auto& initial_std = config_.getInitialStd();
        double pos_std = initial_std.position;
        double clock_std = initial_std.clock;
        double tropo_std = initial_std.troposphere;
        double iono_std = initial_std.ionosphere;
        double amb_std = initial_std.ambiguity;
        if (pos_std <= 0 || clock_std <= 0 || tropo_std <= 0 ||
            (config_.getProcessingMode() == ProcessingMode::PPP_RTK && iono_std <= 0) || amb_std <= 0) {
            log_manager_.getLogger()->warn("Invalid initial standard deviations, using defaults: pos=100, clock=1000, tropo=0.5, iono=5.0, amb=0.1");
            pos_std = 100.0;
            clock_std = 1000.0;
            tropo_std = 0.5;
            iono_std = 5.0;
            amb_std = 0.1;
        }
        P_.block(0, 0, 3, 3) *= pos_std * pos_std;
        P_(3, 3) = clock_std * clock_std;
        P_(4, 4) = tropo_std * tropo_std;
        if (config_.getProcessingMode() == ProcessingMode::PPP_RTK) {
            P_(5, 5) = iono_std * iono_std;
            R_(5, 5) = 0.3; // Code variance for iono
        }
        R_.block(0, 0, 3, 3) *= 0.3; // Code variance for position (m^2)
        R_(3, 3) = 0.3; // Code variance for clock
        R_(4, 4) = 0.3; // Code variance for tropo

        // Set initial variances for ambiguities (if any)
        if (state_size > core_size) {
            P_.bottomRightCorner(state_size - core_size, state_size - core_size) *= amb_std * amb_std;
            R_.bottomRightCorner(state_size - core_size, state_size - core_size) *= 0.0015; // Phase variance (m^2)
            log_manager_.getLogger()->info("Initialized {} ambiguity states with variance: {:.6f} cycles^2",
                state_size - core_size, amb_std * amb_std);
        }

        log_manager_.getLogger()->info("KalmanFilter initialized with state size: {}, mode: {}, R_ ({}x{}), P_ ({}x{}), x_ ({})",
            state_size, config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK",
            R_.rows(), R_.cols(), P_.rows(), P_.cols(), x_.size());
    }

    void KalmanFilter::predict(double dt) {
        if (x_.size() == 0 || P_.rows() == 0 || P_.cols() == 0) {
            log_manager_.getLogger()->error("Filter not initialized");
            throw std::runtime_error("Filter not initialized");
        }

        // Get total state size (core states + ambiguities)
        size_t state_size = state_manager_.getStateSize();
        if (state_size != static_cast<size_t>(x_.size()) || state_size != static_cast<size_t>(P_.rows()) ||
            state_size != static_cast<size_t>(R_.rows())) {
            // Resize state vector and matrices
            Eigen::VectorXd new_x(state_size);
            Eigen::MatrixXd new_P(state_size, state_size);
            Eigen::MatrixXd new_R(state_size, state_size);
            size_t old_size = std::min(static_cast<size_t>(x_.size()), state_size);
            new_x.setZero();
            new_P.setIdentity();
            new_R.setIdentity();
            new_x.head(old_size) = x_.head(old_size);
            new_P.topLeftCorner(old_size, old_size) = P_.topLeftCorner(old_size, old_size);
            // Initialize core state variances
            const auto& initial_std = config_.getInitialStd();
            double pos_std = initial_std.position;
            double clock_std = initial_std.clock;
            double tropo_std = initial_std.troposphere;
            double iono_std = initial_std.ionosphere;
            double amb_std = initial_std.ambiguity;
            if (pos_std <= 0 || clock_std <= 0 || tropo_std <= 0 ||
                (config_.getProcessingMode() == ProcessingMode::PPP_RTK && iono_std <= 0) || amb_std <= 0) {
                pos_std = 100.0;
                clock_std = 1000.0;
                tropo_std = 0.5;
                iono_std = 5.0;
                amb_std = 0.1;
            }
            new_P.block(0, 0, 3, 3) *= pos_std * pos_std;
            new_P(3, 3) = clock_std * clock_std;
            new_P(4, 4) = tropo_std * tropo_std;
            if (config_.getProcessingMode() == ProcessingMode::PPP_RTK) {
                new_P(5, 5) = iono_std * iono_std;
                new_R(5, 5) = 0.3; // Code variance for iono
            }
            new_R.block(0, 0, 3, 3) *= 0.3; // Code variance for position
            new_R(3, 3) = 0.3; // Code variance for clock
            new_R(4, 4) = 0.3; // Code variance for tropo
            // Initialize ambiguity variances
            size_t core_size = config_.getProcessingMode() == ProcessingMode::PPP ? 5 : 6;
            if (state_size > core_size) {
                new_P.bottomRightCorner(state_size - core_size, state_size - core_size) *= amb_std * amb_std;
                new_R.bottomRightCorner(state_size - core_size, state_size - core_size) *= 0.0015; // Phase variance
            }
            x_ = new_x;
            P_ = new_P;
            R_ = new_R;
            log_manager_.getLogger()->info("Resized Kalman filter state to {}: {} core + {} ambiguities, R_ ({}x{}), P_ ({}x{})",
                state_size, core_size, state_size - core_size, R_.rows(), R_.cols(), P_.rows(), P_.cols());
        }

        // Get process noise matrix
        Eigen::MatrixXd Q_k = state_manager_.getProcessNoiseMatrix(dt);
        if (Q_k.rows() != static_cast<long>(state_size) || Q_k.cols() != static_cast<long>(state_size)) {
            log_manager_.getLogger()->error("Process noise Q_k dimensions incompatible: Q_k ({}x{}), expected ({}x{})",
                Q_k.rows(), Q_k.cols(), state_size, state_size);
            throw std::invalid_argument("Q_k dimensions mismatch");
        }

        // Get transition matrix
        Eigen::MatrixXd F = state_manager_.getTransitionMatrix(dt);
        if (F.rows() != static_cast<long>(state_size) || F.cols() != static_cast<long>(state_size)) {
            log_manager_.getLogger()->error("Transition matrix F dimensions incompatible: F ({}x{}), expected ({}x{})",
                F.rows(), F.cols(), state_size, state_size);
            throw std::invalid_argument("F dimensions mismatch");
        }

        // Predict state
        x_ = F * x_;
        P_ = F * P_ * F.transpose() + Q_k;
        log_manager_.getLogger()->trace("Predicted state for dt={:.3f} s: x_ size={}, P_ size={}x{}, R_ size={}x{}",
            dt, x_.size(), P_.rows(), P_.cols(), R_.rows(), R_.cols());
    }

    void KalmanFilter::update(const EpochMeasurement& measurement) {
        if (measurement.innovations.size() == 0 || measurement.H.rows() == 0 || measurement.R_v.size() == 0) {
            log_manager_.getLogger()->warn("Invalid measurement update: innovations ({}), H ({}x{}), R_v ({})",
                measurement.innovations.size(), measurement.H.rows(), measurement.H.cols(), measurement.R_v.size());
            return;
        }

        size_t state_size = state_manager_.getStateSize();
        if (static_cast<size_t>(measurement.H.cols()) != state_size) {
            log_manager_.getLogger()->error("Measurement matrix H columns ({}) do not match state size ({})",
                measurement.H.cols(), state_size);
            throw std::invalid_argument("H dimensions mismatch");
        }

        // Construct measurement noise matrix R
        Eigen::MatrixXd R = Eigen::MatrixXd::Zero(measurement.innovations.size(), measurement.innovations.size());
        for (Eigen::Index i = 0; i < measurement.innovations.size(); ++i) {
            R(i, i) = measurement.R_v(i);
        }

        // Kalman gain
        Eigen::MatrixXd Ht = measurement.H.transpose();
        Eigen::MatrixXd S = measurement.H * P_ * Ht + R;
        Eigen::MatrixXd K = P_ * Ht * S.inverse();

        // Update state
        x_ = x_ + K * measurement.innovations;

        // Update covariance
        Eigen::MatrixXd I = Eigen::MatrixXd::Identity(state_size, state_size);
        P_ = (I - K * measurement.H) * P_;

        // Log update details
        log_manager_.getLogger()->trace("Kalman filter update: innovations size={}, H ({}x{}), R ({}x{}), K ({}x{})",
            measurement.innovations.size(), measurement.H.rows(), measurement.H.cols(),
            R.rows(), R.cols(), K.rows(), K.cols());

        // Update StateManager
        state_manager_.setStateVector(x_);
        state_manager_.setCovariance(P_);
        state_manager_.setStateEpochTime(measurement.time);
    }

} // namespace GnssProcessor
//#endif