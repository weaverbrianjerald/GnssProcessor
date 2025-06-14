#include "StateManager.hpp"
#include <stdexcept>

namespace GnssProcessor {

    StateManager::StateManager(const GnssConfig& config)
        : config_(config), log_manager_(), next_index_(6), state_epoch_time_{ 0, 0.0 } {
        log_manager_.initialize(config.getRinexFilePath().parent_path().parent_path() / "logs", LogLevel::Debug);
        state_vector_ = Eigen::VectorXd::Zero(6);
        covariance_ = Eigen::MatrixXd::Zero(6, 6);
        double pos_var = config_.getInitialPosStd() * config_.getInitialPosStd();
        covariance_.block(0, 0, 3, 3) = Eigen::Matrix3d::Identity() * pos_var;
        covariance_(3, 3) = config_.getInitialClockStd() * config_.getInitialClockStd();
        covariance_(4, 4) = config_.getInitialTropoStd() * config_.getInitialTropoStd();
        covariance_(5, 5) = config_.getInitialIonoStd() * config_.getInitialIonoStd();
        log_manager_.getLogger()->info("StateManager initialized with positioning mode: {}, state size: 6",
            config_.getPositioningMode() == PositioningMode::Static ? "Static" : "Kinematic");
    }

    void StateManager::setStateVector(const Eigen::VectorXd& state) {
        if (state.size() != state_vector_.size()) {
            log_manager_.getLogger()->error("State vector size mismatch: expected {}, got {}", state_vector_.size(), state.size());
            throw std::invalid_argument("State vector size mismatch");
        }
        state_vector_ = state;
    }

    void StateManager::setCovariance(const Eigen::MatrixXd& cov) {
        if (cov.rows() != state_vector_.size() || cov.cols() != state_vector_.size()) {
            log_manager_.getLogger()->error("Covariance matrix size mismatch: expected {}x{}, got {}x{}",
                state_vector_.size(), state_vector_.size(), cov.rows(), cov.cols());
            throw std::invalid_argument("Covariance matrix size mismatch");
        }
        covariance_ = cov;
    }

    Eigen::MatrixXd StateManager::getProcessNoiseMatrix(double dt) const {
        if (dt <= 0.0) {
            log_manager_.getLogger()->error("Invalid time step dt: {}", dt);
            throw std::invalid_argument("Invalid time step");
        }

        int n = state_vector_.size();
        Eigen::MatrixXd Q = Eigen::MatrixXd::Zero(n, n);

        // Position noise
        double pos_noise = config_.getPosProcessNoiseStd() * dt;
        Q.block(0, 0, 3, 3) = Eigen::Matrix3d::Identity() * pos_noise * pos_noise;

        // Clock noise
        double clock_noise = config_.getClockProcessNoiseStd() * dt;
        Q(3, 3) = clock_noise * clock_noise;

        // Tropospheric noise
        double tropo_noise = config_.getTropoProcessNoiseStd() * dt;
        Q(4, 4) = tropo_noise * tropo_noise;

        // Ionospheric noise
        double iono_noise = config_.getIonoProcessNoiseStd() * dt;
        Q(5, 5) = iono_noise * iono_noise;

        // Ambiguity noise (if any)
        double amb_noise = config_.getInitialAmbiguityStd() * dt;
        for (const auto& [key, idx] : ambiguity_indices_) {
            Q(idx, idx) = amb_noise * amb_noise;
        }

        return Q;
    }

    std::optional<int> StateManager::getAmbiguityIndex(const std::string& key) const {
        auto it = ambiguity_indices_.find(key);
        if (it != ambiguity_indices_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    void StateManager::addAmbiguity(const std::string& key, double value) {
        if (ambiguity_indices_.count(key) > 0) {
            log_manager_.getLogger()->warn("Ambiguity key {} already exists, overwriting", key);
            return;
        }

        // Resize state vector and covariance
        int old_size = state_vector_.size();
        state_vector_.conservativeResize(old_size + 1);
        state_vector_(old_size) = value;

        Eigen::MatrixXd new_cov = Eigen::MatrixXd::Zero(old_size + 1, old_size + 1);
        new_cov.topLeftCorner(old_size, old_size) = covariance_;
        new_cov(old_size, old_size) = config_.getInitialAmbiguityStd() * config_.getInitialAmbiguityStd();
        covariance_ = new_cov;

        // Add ambiguity index
        ambiguity_indices_[key] = old_size;
        next_index_ = old_size + 1;

        log_manager_.getLogger()->debug("Added ambiguity {} at index {} with value {}", key, old_size, value);
    }

    void StateManager::removeAmbiguity(const std::string& key) {
        auto it = ambiguity_indices_.find(key);
        if (it == ambiguity_indices_.end()) {
            log_manager_.getLogger()->warn("Ambiguity key {} not found", key);
            return;
        }

        int idx = it->second;
        int old_size = state_vector_.size();

        // Remove ambiguity from state vector
        Eigen::VectorXd new_state(old_size - 1);
        if (idx > 0) {
            new_state.head(idx) = state_vector_.head(idx);
        }
        if (idx < old_size - 1) {
            new_state.tail(old_size - 1 - idx) = state_vector_.tail(old_size - 1 - idx);
        }
        state_vector_ = new_state;

        // Remove ambiguity from covariance
        Eigen::MatrixXd new_cov(old_size - 1, old_size - 1);
        if (idx > 0) {
            new_cov.topLeftCorner(idx, idx) = covariance_.topLeftCorner(idx, idx);
            new_cov.topRightCorner(idx, old_size - 1 - idx) = covariance_.topRightCorner(idx, old_size - 1 - idx);
        }
        if (idx < old_size - 1) {
            new_cov.bottomLeftCorner(old_size - 1 - idx, idx) = covariance_.bottomLeftCorner(old_size - 1 - idx, idx);
            new_cov.bottomRightCorner(old_size - 1 - idx, old_size - 1 - idx) =
                covariance_.bottomRightCorner(old_size - 1 - idx, old_size - 1 - idx);
        }
        covariance_ = new_cov;

        // Update ambiguity indices
        ambiguity_indices_.erase(it);
        for (auto& [k, v] : ambiguity_indices_) {
            if (v > idx) {
                --v;
            }
        }
        --next_index_;

        log_manager_.getLogger()->debug("Removed ambiguity {} at index {}", key, idx);
    }

} // namespace GnssProcessor