#include "StateManager.hpp"
#include <stdexcept>

namespace GnssProcessor {

    StateManager::StateManager(size_t core_state_size)
        : core_state_size_(core_state_size), state_epoch_time_{ 0, 0.0 } {
        state_vector_ = Eigen::VectorXd::Zero(core_state_size);
        covariance_ = Eigen::MatrixXd::Identity(core_state_size, core_state_size);
        // Default initial standard deviations
        double pos_var = 100.0 * 100.0;    // Position: 100 m
        double clock_var = 1000.0 * 1000.0; // Clock: 1000 m
        double tropo_var = 0.5 * 0.5;      // Troposphere: 0.5 m
        double iono_var = 5.0 * 5.0;       // Ionosphere: 5 m (PPP-RTK only)
        covariance_.block(0, 0, 3, 3) = Eigen::Matrix3d::Identity() * pos_var;
        covariance_(3, 3) = clock_var;
        covariance_(4, 4) = tropo_var;
        if (core_state_size >= 6) {
            covariance_(5, 5) = iono_var;
        }
    }

    GnssTime StateManager::getStateEpochTime() const {
        return state_epoch_time_;
    }

    void StateManager::setStateEpochTime(const GnssTime& time) {
        state_epoch_time_ = time;
    }

    void StateManager::setStateVector(const Eigen::VectorXd& state) {
        if (static_cast<size_t>(state.size()) != getStateSize()) {
            throw std::invalid_argument("State vector size " + std::to_string(state.size()) +
                " does not match expected size " + std::to_string(getStateSize()));
        }
        if (!state.allFinite()) {
            throw std::invalid_argument("State vector contains non-finite values");
        }
        state_vector_ = state;
    }

    void StateManager::setCovariance(const Eigen::MatrixXd& covariance) {
        if (static_cast<size_t>(covariance.rows()) != getStateSize() ||
            static_cast<size_t>(covariance.cols()) != getStateSize()) {
            throw std::invalid_argument("Covariance matrix dimensions (" +
                std::to_string(covariance.rows()) + "x" +
                std::to_string(covariance.cols()) +
                ") do not match state size " + std::to_string(getStateSize()));
        }
        if (!covariance.allFinite()) {
            throw std::invalid_argument("Covariance matrix contains non-finite values");
        }
        covariance_ = covariance;
    }

    size_t StateManager::getStateSize() const {
        return core_state_size_ + ambiguity_indices_.size();
    }

    Eigen::VectorXd StateManager::getStateVector() const {
        return state_vector_;
    }

    Eigen::MatrixXd StateManager::getCovariance() const {
        return covariance_;
    }

    Eigen::MatrixXd StateManager::getProcessNoiseMatrix(double dt) const {
        if (dt <= 0.0) {
            throw std::invalid_argument("Invalid time step: " + std::to_string(dt));
        }

        size_t state_size = getStateSize();
        Eigen::MatrixXd Q = Eigen::MatrixXd::Zero(state_size, state_size);

        // Default process noise standard deviations
        double pos_std = 0.1;      // Position: 0.1 m/s
        double clock_std = 100.0;  // Clock: 100 m/s
        double tropo_std = 0.01;   // Troposphere: 0.01 m/s
        double iono_std = 0.02;    // Ionosphere: 0.02 m/s (PPP-RTK only)
        double amb_std = 0.001;    // Ambiguity: 0.001 cycles/s

        // Position noise
        double pos_noise = pos_std * dt;
        Q.block(0, 0, 3, 3) = Eigen::Matrix3d::Identity() * pos_noise * pos_noise;

        // Clock noise
        double clock_noise = clock_std * dt;
        Q(3, 3) = clock_noise * clock_noise;

        // Tropospheric noise
        double tropo_noise = tropo_std * dt;
        Q(4, 4) = tropo_noise * tropo_noise;

        // Ionospheric noise (PPP-RTK only)
        if (core_state_size_ >= 6) {
            double iono_noise = iono_std * dt;
            Q(5, 5) = iono_noise * iono_noise;
        }

        // Ambiguity noise
        for (const auto& [key, idx] : ambiguity_indices_) {
            double amb_noise = amb_std * dt;
            Q(idx, idx) = amb_noise * amb_noise;
        }

        return Q;
    }

    Eigen::MatrixXd StateManager::getTransitionMatrix(double dt) const {
        size_t state_size = getStateSize();
        Eigen::MatrixXd F = Eigen::MatrixXd::Identity(state_size, state_size);
        return F;
    }

    size_t StateManager::getPositionIndex() const {
        return 0;
    }

    size_t StateManager::getClockIndex() const {
        return 3;
    }

    size_t StateManager::getTropoIndex() const {
        return 4;
    }

    size_t StateManager::getIonoIndex() const {
        return core_state_size_ >= 6 ? 5 : std::numeric_limits<size_t>::max();
    }

    int StateManager::getAmbiguityIndex(const std::string& key) const {
        auto it = ambiguity_indices_.find(key);
        return it != ambiguity_indices_.end() ? static_cast<int>(it->second) : -1;
    }

    size_t StateManager::addAmbiguity(const std::string& key, double initial_value) {
        if (ambiguity_indices_.count(key)) {
            return ambiguity_indices_[key];
        }
        size_t index = getStateSize();
        ambiguity_indices_[key] = index;

        // Resize state and covariance
        Eigen::VectorXd new_state = Eigen::VectorXd::Zero(index + 1);
        new_state.head(index) = state_vector_;
        new_state(index) = initial_value;
        state_vector_ = new_state;

        Eigen::MatrixXd new_cov = Eigen::MatrixXd::Identity(index + 1, index + 1);
        new_cov.topLeftCorner(index, index) = covariance_;
        new_cov(index, index) = 0.1 * 0.1; // Initial ambiguity variance (cycles^2)
        covariance_ = new_cov;

        return index;
    }

} // namespace GnssProcessor
