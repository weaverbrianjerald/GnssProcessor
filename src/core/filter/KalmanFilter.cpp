#include "RinexParser.hpp"
#include "KalmanFilter.hpp"
#include "PppModel.hpp"
#include <spdlog/spdlog.h>
#include <Eigen/Dense>
#include <Eigen/QR>
#include <stdexcept>


namespace GnssProcessor {

    KalmanFilter::KalmanFilter(StateManager& state_manager, const GnssConfig& config, LogManager& log_manager, const PppModel& ppp_model)
        : state_manager_(state_manager), config_(config), log_manager_(log_manager), ppp_model_(ppp_model),
        R_(Eigen::MatrixXd::Identity(6, 6)), gf_histories_(), last_measurement_() {
        log_manager_.getLogger()->info("KalmanFilter initialized with state size: {}", state_manager_.getStateSize());
    }

    void KalmanFilter::initialize() {
        int n = state_manager_.getStateSize();
        R_ = Eigen::MatrixXd::Zero(n, n);
        x_ = Eigen::VectorXd::Zero(n);
        gf_histories_.clear();

        double pos_std = config_.getInitialPosStd();
        double clock_std = config_.getInitialClockStd();
        double tropo_std = config_.getInitialTropoStd();
        double amb_std = config_.getInitialAmbiguityStd();
        if (pos_std > 0) R_.block(0, 0, 3, 3) = Eigen::Matrix3d::Identity() / pos_std;
        if (clock_std > 0) R_(3, 3) = 1.0 / clock_std;
        if (tropo_std > 0) R_(4, 4) = 1.0 / tropo_std;
        for (int i = 5; i < n; ++i) {
            if (amb_std > 0) R_(i, i) = 1.0 / amb_std;
        }

        log_manager_.getLogger()->info("KalmanFilter initialized with R_ ({}x{}) and x_ ({})", R_.rows(), R_.cols(), x_.size());
    }

    void KalmanFilter::predict(double dt) {
        if (R_.rows() == 0 || R_.cols() == 0) {
            log_manager_.getLogger()->error("Filter not initialized");
            throw std::runtime_error("Filter not initialized");
        }

        // Get process noise matrix from StateManager
        Eigen::MatrixXd Q_k = state_manager_.getProcessNoiseMatrix(dt);
        if (Q_k.rows() != R_.rows() || Q_k.cols() != R_.cols()) {
            log_manager_.getLogger()->error("Process noise Q_k dimensions incompatible: Q_k ({}x{}), R_ ({}x{})",
                Q_k.rows(), Q_k.cols(), R_.rows(), R_.cols());
            throw std::invalid_argument("Q_k dimensions mismatch");
        }

        // Compute Cholesky factorization of Q_k: Q_k = L * L^T
        Eigen::LLT<Eigen::MatrixXd> llt(Q_k);
        if (llt.info() != Eigen::Success) {
            log_manager_.getLogger()->error("Cholesky factorization of Q_k failed");
            throw std::runtime_error("Q_k Cholesky factorization failed");
        }
        Eigen::MatrixXd L = llt.matrixL();

        // Form augmented matrix for prediction
        int n = R_.rows();
        Eigen::MatrixXd augmented(2 * n, n + 1);
        augmented.setZero();
        augmented.block(0, 0, n, n) = R_;           // Current R_
        augmented.block(0, n, n, 1) = x_;           // Current x_ (changed from b_)
        augmented.block(n, 0, n, n) = L.inverse();  // Inverse Cholesky factor

        // Perform QR decomposition
        Eigen::HouseholderQR<Eigen::MatrixXd> qr(augmented);
        Eigen::MatrixXd R_new = qr.matrixQR().topRows(n).triangularView<Eigen::Upper>();

        // Update R_ and x_
        R_ = R_new.block(0, 0, n, n);
        x_ = R_new.block(0, n, n, 1); // Changed from b_ to x_
        log_manager_.getLogger()->debug("Predict step completed: dt {}, R_ ({}x{}), x_ ({})", dt, R_.rows(), R_.cols(), x_.size());
    }

    void KalmanFilter::update(const EpochMeasurement& measurement, PppState& ppp_state) {
        if (measurement.innovations.size() == 0 || measurement.H.rows() == 0) {
            log_manager_.getLogger()->warn("Invalid measurement update: empty innovations or Jacobian");
            return;
        }

        // Store measurement
        last_measurement_ = measurement;

        // Time update
        timeUpdate(measurement.time);

        // Cycle-slip detection
        int n = state_manager_.getStateSize();
        int m = measurement.innovations.size();
        Eigen::MatrixXd H = measurement.H;
        Eigen::VectorXd z = measurement.innovations;
        Eigen::VectorXd R_v = measurement.R_v;

        Eigen::VectorXd weights = computeRobustWeights(z, R_v);

        std::vector<std::pair<std::string, std::string>> cycle_slips;
        int row = 0;
        for (const auto& obs : measurement.observations) {
            for (const auto& [type, value] : obs.observed) {
                if (type[0] == 'L' && row < m) {
                    if (detectCycleSlip(measurement, obs.satellite, type, z(row), R_v(row))) {
                        cycle_slips.emplace_back(obs.satellite, type);
                    }
                    row++;
                }
                else if (row < m) {
                    row++;
                }
            }
        }

        for (const auto& [sat, type] : cycle_slips) {
            std::string amb_key = sat + "_" + type;
            state_manager_.removeAmbiguity(amb_key);
            ppp_state.ambiguities.erase(amb_key);
            log_manager_.getLogger()->info("Cycle-slip detected for {} {}, ambiguity reinitialized", sat, type);
        }

        // Measurement update
        if (R_.rows() != m || R_.cols() != m) {
            R_ = Eigen::MatrixXd::Identity(m, m);
            log_manager_.getLogger()->debug("Resized R_ to {}x{} for measurement update", m, m);
        }
        for (int i = 0; i < m; ++i) {
            double r_v = R_v(i) > 0 ? R_v(i) : 1e-6;
            R_(i, i) = weights(i) > 0 ? 1.0 / std::sqrt(r_v / weights(i)) : 1.0 / std::sqrt(1e-6);
        }

        Eigen::MatrixXd A(n + m, n + 1);
        A.topRows(n) << R_, x_;
        A.bottomRows(m) << R_ * H, R_* z;

        Eigen::HouseholderQR<Eigen::MatrixXd> qr(A);
        Eigen::MatrixXd R_new = qr.matrixQR().topRows(n).triangularView<Eigen::Upper>();

        R_ = R_new.topLeftCorner(n, n);
        x_ = R_new.topRightCorner(n, 1);

        if (R_.diagonal().cwiseAbs().minCoeff() < 1e-10) {
            log_manager_.getLogger()->warn("Singular SRIF matrix detected, skipping update");
            return;
        }

        Eigen::VectorXd state_update = R_.triangularView<Eigen::Upper>().solve(x_);
        Eigen::VectorXd current_state = state_manager_.getStateVector();
        current_state += state_update;
        state_manager_.setStateVector(current_state);

        Eigen::MatrixXd covariance = R_.inverse() * R_.inverse().transpose();

        // Ambiguity resolution using LAMBDA
        if (resolveAmbiguities(ppp_state, current_state, covariance)) {
            state_manager_.setStateVector(current_state);
            state_manager_.setCovariance(covariance);
            log_manager_.getLogger()->info("Ambiguities resolved successfully at epoch TOW {}", measurement.time.tow);
        }

        // Update PppState
        ppp_state.position = current_state.segment<3>(state_manager_.getPositionIndex());
        ppp_state.clock_bias = current_state(state_manager_.getClockIndex()) / Constants::SPEED_OF_LIGHT;
        ppp_state.troposphere_zenith_delay = current_state(state_manager_.getTropoIndex());
        ppp_state.ionosphere_zenith_delay = current_state(state_manager_.getIonoIndex());
        for (const auto& [key, idx] : state_manager_.getAmbiguityIndices()) {
            ppp_state.ambiguities[key] = current_state(idx);
        }
        ppp_state.covariance = covariance;
        ppp_state.epoch_time = measurement.time;

        log_manager_.getLogger()->info("Kalman filter updated: state size {}, measurement size {}, epoch TOW {}",
            n, m, measurement.time.tow);
    }


    void KalmanFilter::timeUpdate(const GnssTime& new_time) {
        // Compute time difference using StateManager::state_epoch_time_
        double dt = new_time.tow - state_manager_.getStateEpochTime().tow;
        if (dt < 0.0 || dt > 300.0) {
            log_manager_.getLogger()->warn("Invalid time step dt = {}, using default 1.0 s", dt);
            dt = 1.0;
        }

        // Get process noise matrix
        Eigen::MatrixXd Q = state_manager_.getProcessNoiseMatrix(dt);

        // Augment SRIF matrix with process noise
        int n = state_manager_.getStateSize();
        Eigen::LLT<Eigen::MatrixXd> llt(Q);
        if (llt.info() != Eigen::Success) {
            log_manager_.getLogger()->error("Cholesky factorization of Q failed");
            throw std::runtime_error("Cholesky factorization failed");
        }
        Eigen::MatrixXd L = llt.matrixL();
        Eigen::MatrixXd R_Q = L.inverse(); // Compute inverse of Cholesky factor
        Eigen::MatrixXd A(n + n, n + 1);
        A.topRows(n) << R_, x_;
        A.bottomRows(n) << R_Q, Eigen::VectorXd::Zero(n);

        // Householder QR decomposition
        Eigen::HouseholderQR<Eigen::MatrixXd> qr(A);
        R_ = qr.matrixQR().topLeftCorner(n, n).triangularView<Eigen::Upper>();
        x_ = qr.matrixQR().topRightCorner(n, 1);

        // Update state epoch
        state_manager_.setStateEpochTime(new_time);
        log_manager_.getLogger()->debug("Time update completed: dt = {}, new epoch TOW {}", dt, new_time.tow);
    }

    Eigen::VectorXd KalmanFilter::getStateEstimate() const {
        return state_manager_.getStateVector();
    }

    bool KalmanFilter::detectCycleSlip(const EpochMeasurement& measurement, const std::string& sat_id, const std::string& type, double innovation, double variance) const {
        // Check innovation residual
        double std_dev = std::sqrt(variance);
        if (std::abs(innovation) > CYCLE_SLIP_THRESHOLD * std_dev) {
            log_manager_.getLogger()->debug("Cycle-slip suspected for {} {}: innovation = {}, threshold = {}", sat_id, type, innovation, CYCLE_SLIP_THRESHOLD * std_dev);
            return true;
        }

        // Check LLI flag
        for (const auto& obs : measurement.observations) {
            if (obs.satellite == sat_id && obs.lli_flags.count(type) && obs.lli_flags.at(type) > 0) {
                log_manager_.getLogger()->debug("Cycle-slip detected for {} {}: LLI flag = {}", sat_id, type, obs.lli_flags.at(type));
                return true;
            }
        }

        // Check geometry-free combination
        for (const auto& obs : measurement.observations) {
            if (obs.satellite == sat_id && type[0] == 'L') {
                double gf_residual;
                if (detectGeometryFreeCycleSlip(obs, gf_residual)) {
                    log_manager_.getLogger()->debug("Cycle-slip detected for {} {}: GF residual = {}, threshold = {}", sat_id, type, gf_residual, GF_CYCLE_SLIP_THRESHOLD);
                    return true;
                }
                // Time-series analysis
                double deviation;
                if (detectTimeSeriesCycleSlip(obs, gf_residual, deviation)) {
                    log_manager_.getLogger()->debug("Cycle-slip detected for {} {}: TS deviation = {}, threshold = {}", sat_id, type, deviation, TS_CYCLE_SLIP_THRESHOLD);
                    return true;
                }
            }
        }

        return false;
    }

    bool KalmanFilter::detectGeometryFreeCycleSlip(const PppObservation& obs, double& gf_residual) const {
        std::string system_str;
        switch (obs.system) {
        case System::GPS: system_str = "GPS"; break;
        case System::Galileo: system_str = "Galileo"; break;
        case System::GLONASS: system_str = "GLONASS"; break;
        case System::BeiDou: system_str = "BeiDou"; break;
        case System::QZSS: system_str = "QZSS"; break;
        case System::SBAS: system_str = "SBAS"; break;
        default: return false;
        }

        auto sig_it = config_.getSignalPriorities().find(system_str);
        if (sig_it == config_.getSignalPriorities().end() || sig_it->second.size() < 2) {
            return false;
        }

        std::string l1_type, l2_type;
        for (const auto& signal : sig_it->second) {
            if (signal[0] == 'L') {
                if (l1_type.empty() && (signal.find("L1") != std::string::npos || signal.find("E1") != std::string::npos || signal.find("G1") != std::string::npos)) {
                    l1_type = signal;
                }
                else if (l2_type.empty() && (signal.find("L2") != std::string::npos || signal.find("E5") != std::string::npos || signal.find("G2") != std::string::npos)) {
                    l2_type = signal;
                }
            }
        }

        if (l1_type.empty() || l2_type.empty() || !obs.observed.count(l1_type) || !obs.observed.count(l2_type)) {
            return false;
        }

        double lambda1 = ppp_model_.getWavelength(l1_type, obs.system);
        double lambda2 = ppp_model_.getWavelength(l2_type, obs.system);
        if (lambda1 <= 0.0 || lambda2 <= 0.0) {
            return false;
        }

        double phi1 = obs.observed.at(l1_type) / lambda1;
        double phi2 = obs.observed.at(l2_type) / lambda2;
        double freq1 = GnssProcessor::Constants::SPEED_OF_LIGHT / lambda1;
        double freq2 = GnssProcessor::Constants::SPEED_OF_LIGHT / lambda2;
        double iono_factor = (freq2 * freq2 - freq1 * freq1) / (freq1 * freq1 * freq2 * freq2);
        double gf_measured = lambda1 * phi1 - lambda2 * phi2;

        double iono_delay = ppp_model_.computeIonosphericDelay(obs.time, state_manager_.getStateVector().segment<3>(0),
            ppp_model_.getSatellitePosition(obs.satellite, obs.time),
            l1_type, obs.system, obs.observed);
        double gf_expected = iono_factor * iono_delay;

        gf_residual = std::abs(gf_measured - gf_expected);

        // Update GF history
        std::string key = obs.satellite + "_" + l1_type + "_" + l2_type;
        auto& history = gf_histories_[key];
        history.gf_values.push_back(gf_measured);
        history.timestamps.push_back(obs.time);
        if (history.gf_values.size() > GfHistory::WINDOW_SIZE) {
            history.gf_values.pop_front();
            history.timestamps.pop_front();
        }

        return gf_residual > GF_CYCLE_SLIP_THRESHOLD;
    }

    bool KalmanFilter::detectTimeSeriesCycleSlip(const PppObservation& obs, double gf_residual, double& deviation) const {
        std::string system_str;
        switch (obs.system) {
        case System::GPS: system_str = "GPS"; break;
        case System::Galileo: system_str = "Galileo"; break;
        case System::GLONASS: system_str = "GLONASS"; break;
        case System::BeiDou: system_str = "BeiDou"; break;
        case System::QZSS: system_str = "QZSS"; break;
        case System::SBAS: system_str = "SBAS"; break;
        default: return false;
        }

        auto sig_it = config_.getSignalPriorities().find(system_str);
        if (sig_it == config_.getSignalPriorities().end() || sig_it->second.size() < 2) {
            return false;
        }

        std::string l1_type, l2_type;
        for (const auto& signal : sig_it->second) {
            if (signal[0] == 'L') {
                if (l1_type.empty() && (signal.find("L1") != std::string::npos || signal.find("E1") != std::string::npos || signal.find("G1") != std::string::npos)) {
                    l1_type = signal;
                }
                else if (l2_type.empty() && (signal.find("L2") != std::string::npos || signal.find("E5") != std::string::npos || signal.find("G2") != std::string::npos)) {
                    l2_type = signal;
                }
            }
        }

        if (l1_type.empty() || l2_type.empty()) {
            return false;
        }

        std::string key = obs.satellite + "_" + l1_type + "_" + l2_type;
        auto it = gf_histories_.find(key);
        if (it == gf_histories_.end() || it->second.gf_values.size() < GfHistory::WINDOW_SIZE) {
            deviation = 0.0;
            return false; // Insufficient history
        }

        // Check for time continuity
        const auto& timestamps = it->second.timestamps;
        for (size_t i = 1; i < timestamps.size(); ++i) {
            double dt = timestamps[i].tow - timestamps[i - 1].tow;
            if (dt > GfHistory::TIME_GAP_THRESHOLD) {
                log_manager_.getLogger()->debug("Time gap too large for {}: dt = {} s", key, dt);
                return false;
            }
        }

        // Compute mean and standard deviation of GF values
        const auto& gf_values = it->second.gf_values;
        double sum = 0.0, sum_sq = 0.0;
        size_t n = gf_values.size();
        for (double value : gf_values) {
            sum += value;
            sum_sq += value * value;
        }
        double mean = sum / n;
        double variance = (sum_sq / n - mean * mean) * n / (n - 1);
        double std_dev = variance > 0.0 ? std::sqrt(variance) : 1e-6;

        // Compute deviation of current GF residual
        double current_gf = gf_values.back();
        deviation = std::abs(current_gf - mean) / std_dev;

        return deviation > TS_CYCLE_SLIP_THRESHOLD;
    }

    Eigen::VectorXd KalmanFilter::computeRobustWeights(const Eigen::VectorXd& innovations, const Eigen::VectorXd& variances) const {
        int m = innovations.size();
        Eigen::VectorXd weights = Eigen::VectorXd::Ones(m);
        for (int i = 0; i < m; ++i) {
            double std_dev = std::sqrt(variances(i) > 0 ? variances(i) : 1e-6);
            double residual = std::abs(innovations(i)) / std_dev;
            if (residual > HUBER_K) {
                weights(i) = HUBER_K / residual; // Huber weight
            }
        }
        return weights;
    }

    bool KalmanFilter::resolveAmbiguities(PppState& ppp_state, Eigen::VectorXd& state_vector, Eigen::MatrixXd& covariance) {
        auto amb_indices = state_manager_.getAmbiguityIndices();
        int n_amb = amb_indices.size();
        if (n_amb == 0) {
            log_manager_.getLogger()->debug("No ambiguities to resolve");
            return false;
        }

        // Extract float ambiguities and covariance
        Eigen::VectorXd float_amb(n_amb);
        Eigen::MatrixXd Q_amb(n_amb, n_amb);
        std::vector<std::string> amb_keys;
        int i = 0;
        for (const auto& [key, idx] : amb_indices) {
            float_amb(i) = state_vector(idx);
            amb_keys.push_back(key);
            int j = 0;
            for (const auto& [key2, idx2] : amb_indices) {
                Q_amb(i, j) = covariance(idx, idx2);
                j++;
            }
            i++;
        }

        // Check for numerical stability
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eig(Q_amb);
        if (eig.eigenvalues().minCoeff() < 1e-10) {
            log_manager_.getLogger()->warn("Ambiguity covariance matrix is near-singular, skipping resolution");
            return false;
        }

        // Try full ambiguity resolution
        Eigen::MatrixXd Z = Eigen::MatrixXd::Identity(n_amb, n_amb);
        Eigen::VectorXd a = float_amb;
        decorrelateAmbiguities(Q_amb, Z, a);
        double best_ratio;
        Eigen::VectorXi int_amb = searchIntegerAmbiguities(a, Q_amb, best_ratio);
        Eigen::VectorXd fixed_amb = Z * int_amb.cast<double>();

        if (best_ratio >= RATIO_TEST_THRESHOLD &&
            validateFixedAmbiguities(fixed_amb, amb_keys, last_measurement_.H, last_measurement_.innovations, last_measurement_.R_v)) {
            // Apply full fixed ambiguities
            i = 0;
            for (const auto& key : amb_keys) {
                auto idx_it = amb_indices.find(key);
                if (idx_it != amb_indices.end()) {
                    state_vector(idx_it->second) = fixed_amb(i);
                    ppp_state.ambiguities[key] = fixed_amb(i);
                }
                i++;
            }
            Eigen::MatrixXd G = Eigen::MatrixXd::Zero(state_manager_.getStateSize(), n_amb);
            i = 0;
            for (const auto& [key, idx] : amb_indices) {
                G(idx, i) = 1.0;
                i++;
            }
            Eigen::MatrixXd Q_fixed = covariance - covariance * G * (G.transpose() * covariance * G + Q_amb).inverse() * G.transpose() * covariance;
            if (Q_fixed.diagonal().minCoeff() >= 0.0) {
                covariance = Q_fixed;
                log_manager_.getLogger()->info("Resolved {} ambiguities (full) with ratio = {}", n_amb, best_ratio);
                return true;
            }
            log_manager_.getLogger()->warn("Full covariance update produced negative variances, attempting partial fixing");
        }

        // Fall back to partial ambiguity resolution
        return resolvePartialAmbiguities(ppp_state, state_vector, covariance, last_measurement_.H, last_measurement_.innovations, last_measurement_.R_v);
    }

    bool KalmanFilter::resolvePartialAmbiguities(PppState& ppp_state, Eigen::VectorXd& state_vector, Eigen::MatrixXd& covariance,
        const Eigen::MatrixXd& H, const Eigen::VectorXd& z, const Eigen::VectorXd& R_v) {
        auto amb_indices = state_manager_.getAmbiguityIndices();
        int n_amb = amb_indices.size();
        if (n_amb == 0) {
            return false;
        }

        // Select ambiguities with low variance
        std::vector<std::string> fixable_keys;
        std::vector<int> fixable_indices;
        std::vector<int> float_indices;
        Eigen::VectorXd float_amb(n_amb);
        Eigen::MatrixXd Q_amb(n_amb, n_amb);
        std::vector<std::string> amb_keys;
        int i = 0;
        for (const auto& [key, idx] : amb_indices) {
            float_amb(i) = state_vector(idx);
            amb_keys.push_back(key);
            int j = 0;
            for (const auto& [key2, idx2] : amb_indices) {
                Q_amb(i, j) = covariance(idx, idx2);
                j++;
            }
            if (Q_amb(i, i) < VARIANCE_THRESHOLD) {
                fixable_keys.push_back(key);
                fixable_indices.push_back(i);
            }
            else {
                float_indices.push_back(i);
            }
            i++;
        }

        int n_fix = fixable_indices.size();
        if (n_fix < MIN_FIX_FRACTION * n_amb) {
            log_manager_.getLogger()->debug("Insufficient fixable ambiguities: {} < {} required", n_fix, MIN_FIX_FRACTION * n_amb);
            return false;
        }

        // Extract fixable ambiguities
        Eigen::VectorXd fixable_amb(n_fix);
        Eigen::MatrixXd Q_fix(n_fix, n_fix);
        for (int k = 0; k < n_fix; ++k) {
            fixable_amb(k) = float_amb(fixable_indices[k]);
            for (int l = 0; l < n_fix; ++l) {
                Q_fix(k, l) = Q_amb(fixable_indices[k], fixable_indices[l]);
            }
        }

        // LAMBDA on fixable subset
        Eigen::MatrixXd Z = Eigen::MatrixXd::Identity(n_fix, n_fix);
        Eigen::VectorXd a = fixable_amb;
        decorrelateAmbiguities(Q_fix, Z, a);
        double best_ratio;
        Eigen::VectorXi int_amb = searchIntegerAmbiguities(a, Q_fix, best_ratio);
        if (best_ratio < RATIO_TEST_THRESHOLD) {
            log_manager_.getLogger()->debug("Partial ambiguity resolution failed: ratio = {} < {}", best_ratio, RATIO_TEST_THRESHOLD);
            return false;
        }

        // Transform back and validate
        Eigen::VectorXd fixed_amb_subset = Z * int_amb.cast<double>();
        Eigen::VectorXd fixed_amb = float_amb;
        for (int k = 0; k < n_fix; ++k) {
            fixed_amb(fixable_indices[k]) = fixed_amb_subset(k);
        }

        if (!validateFixedAmbiguities(fixed_amb, amb_keys, H, z, R_v)) {
            log_manager_.getLogger()->debug("Partial fixed ambiguities failed residual validation");
            return false;
        }

        // Apply fixed ambiguities
        for (int k = 0; k < n_fix; ++k) {
            int orig_idx = fixable_indices[k];
            auto idx_it = amb_indices.find(amb_keys[orig_idx]);
            if (idx_it != amb_indices.end()) {
                state_vector(idx_it->second) = fixed_amb(orig_idx);
                ppp_state.ambiguities[amb_keys[orig_idx]] = fixed_amb(orig_idx);
            }
        }

        // Update covariance
        Eigen::MatrixXd G = Eigen::MatrixXd::Zero(state_manager_.getStateSize(), n_fix);
        for (int k = 0; k < n_fix; ++k) {
            auto idx_it = amb_indices.find(amb_keys[fixable_indices[k]]);
            if (idx_it != amb_indices.end()) {
                G(idx_it->second, k) = 1.0;
            }
        }
        Eigen::MatrixXd Q_fixed = covariance - covariance * G * (G.transpose() * covariance * G + Q_fix).inverse() * G.transpose() * covariance;
        if (Q_fixed.diagonal().minCoeff() >= 0.0) {
            covariance = Q_fixed;
            log_manager_.getLogger()->info("Resolved {} of {} ambiguities (partial) with ratio = {}", n_fix, n_amb, best_ratio);
            return true;
        }

        log_manager_.getLogger()->warn("Partial covariance update produced negative variances");
        return false;
    }

    void KalmanFilter::decorrelateAmbiguities(Eigen::MatrixXd& Q, Eigen::MatrixXd& Z, Eigen::VectorXd& a) {
        int n = Q.rows();
        Z = Eigen::MatrixXd::Identity(n, n);
        Eigen::MatrixXd Q_temp = Q;

        for (int k = n - 1; k >= 0; --k) {
            for (int i = k - 1; i >= 0; --i) {
                double mu = std::round(Q_temp(i, k) / Q_temp(k, k));
                if (mu != 0) {
                    Z.col(i) -= mu * Z.col(k);
                    Q_temp.row(i) -= mu * Q_temp.row(k);
                    Q_temp.col(i) -= mu * Q_temp.col(k);
                    a(i) -= mu * a(k);
                }
            }
        }

        Q = Q_temp;
    }

    Eigen::VectorXi KalmanFilter::searchIntegerAmbiguities(const Eigen::VectorXd& float_amb, const Eigen::MatrixXd& Q_amb, double& best_ratio) {
        int n = float_amb.size();
        Eigen::VectorXi best_int_amb = float_amb.array().round().cast<int>();
        double best_sq_norm = std::numeric_limits<double>::max();
        Eigen::VectorXi second_best_int_amb = best_int_amb;
        double second_best_sq_norm = best_sq_norm;

        // Simple search-and-shrink approach
        Eigen::VectorXd diff = float_amb - best_int_amb.cast<double>();
        double sq_norm = diff.transpose() * Q_amb.inverse() * diff;
        best_sq_norm = sq_norm;

        int candidates = 0;
        Eigen::VectorXi cand(n);
        std::function<void(int, double)> search = [&](int level, double curr_sq_norm) {
            if (candidates >= MAX_SEARCH_CANDIDATES) return;
            if (level == n) {
                candidates++;
                if (curr_sq_norm < best_sq_norm) {
                    second_best_int_amb = best_int_amb;
                    second_best_sq_norm = best_sq_norm;
                    best_int_amb = cand;
                    best_sq_norm = curr_sq_norm;
                }
                else if (curr_sq_norm < second_best_sq_norm) {
                    second_best_int_amb = cand;
                    second_best_sq_norm = curr_sq_norm;
                }
                return;
            }

            double a_i = float_amb(level);
            int center = std::round(a_i);
            double bound = std::sqrt((best_sq_norm - curr_sq_norm) * Q_amb(level, level));
            int min_c = std::ceil(a_i - bound);
            int max_c = std::floor(a_i + bound);

            for (int c = min_c; c <= max_c; ++c) {
                cand(level) = c;
                double delta = c - a_i;
                double new_sq_norm = curr_sq_norm + delta * delta / Q_amb(level, level);
                if (new_sq_norm < best_sq_norm) {
                    search(level + 1, new_sq_norm);
                }
            }
            };

        search(0, 0.0);

        // Compute ratio test
        best_ratio = second_best_sq_norm / best_sq_norm;
        return best_int_amb;
    }

    bool KalmanFilter::validateFixedAmbiguities(const Eigen::VectorXd& fixed_amb, const std::vector<std::string>& amb_keys,
        const Eigen::MatrixXd& H, const Eigen::VectorXd& z, const Eigen::VectorXd& R_v) const {
        int n_amb = fixed_amb.size();
        int m = z.size();
        if (H.rows() != m || H.cols() < n_amb) {
            log_manager_.getLogger()->debug("Invalid H matrix dimensions for residual validation: {}x{}", H.rows(), H.cols());
            return false;
        }

        // Compute residuals with fixed ambiguities
        Eigen::VectorXd state_temp = state_manager_.getStateVector();
        int i = 0;
        for (const auto& key : amb_keys) {
            auto idx_it = state_manager_.getAmbiguityIndices().find(key);
            if (idx_it != state_manager_.getAmbiguityIndices().end()) {
                state_temp(idx_it->second) = fixed_amb(i);
            }
            i++;
        }

        Eigen::VectorXd predicted = H * state_temp;
        if (predicted.size() != m) {
            log_manager_.getLogger()->debug("Prediction size mismatch: {} vs {}", predicted.size(), m);
            return false;
        }

        Eigen::VectorXd residuals = z - predicted;
        for (int j = 0; j < m; ++j) {
            double std_dev = std::sqrt(R_v(j) > 0 ? R_v(j) : 1e-6);
            if (std::abs(residuals(j)) > RESIDUAL_THRESHOLD * std_dev) {
                log_manager_.getLogger()->debug("Residual validation failed for observation {}: residual = {}, threshold = {}",
                    j, residuals(j), RESIDUAL_THRESHOLD * std_dev);
                return false;
            }
        }

        return true;
    }

} // namespace GnssProcessor
//#endif