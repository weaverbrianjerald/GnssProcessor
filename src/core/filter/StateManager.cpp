#include "StateManager.hpp"
#include <stdexcept>
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <Eigen/Dense>

// Formatter specialization for Eigen::VectorXd
template <>
struct fmt::formatter<Eigen::VectorXd> {
    constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const Eigen::VectorXd& vec, FormatContext& ctx) const -> decltype(ctx.out()) {
        auto out = ctx.out();
        out = fmt::format_to(out, "[");
        for (Eigen::Index i = 0; i < vec.size(); ++i) {
            out = fmt::format_to(out, "{:.3f}", vec(i));
            if (i < vec.size() - 1) {
                out = fmt::format_to(out, ", ");
            }
        }
        out = fmt::format_to(out, "]");
        return out;
    }
};

namespace GnssProcessor {

    StateManager::StateManager(size_t core_state_size)
        : core_state_size_(core_state_size), state_epoch_time_{ 0, 0.0 } {
        state_vector_ = Eigen::VectorXd::Zero(core_state_size);
        covariance_ = Eigen::MatrixXd::Identity(core_state_size, core_state_size);
        // Default initial standard deviations
        double pos_var = 100.0 * 100.0;    // Position: 100 m
        double clock_var = 10.0 * 10.0; // Clock: 10 m
        double tropo_var = 0.5 * 0.5;      // Troposphere: 0.5 m
        double iono_var = 5.0 * 5.0;       // Ionosphere: 5 m (PPP-RTK only)
        covariance_.block(0, 0, 3, 3) = Eigen::Matrix3d::Identity() * pos_var;
        covariance_(3, 3) = clock_var;
        covariance_(4, 4) = tropo_var;
        if (core_state_size >= 6) {
            covariance_(5, 5) = iono_var;
        }
        // Log initialization
        spdlog::get("state_manager")->info("StateManager initialized with core state size: {}", core_state_size);
    }

    GnssTime StateManager::getStateEpochTime() const {
        return state_epoch_time_;
    }

    void StateManager::setStateEpochTime(const GnssTime& time) {
        state_epoch_time_ = time;
        spdlog::get("state_manager")->trace("Set state epoch time: week={}, TOW={:.3f}", time.week, time.tow);
    }

    void StateManager::setStateVector(const Eigen::VectorXd& state) {
        auto logger = spdlog::get("state_manager");
        if (static_cast<size_t>(state.size()) != getStateSize()) {
            logger->error("State vector size {} does not match expected size {}", state.size(), getStateSize());
            throw std::invalid_argument("State vector size " + std::to_string(state.size()) +
                " does not match expected size " + std::to_string(getStateSize()));
        }
        if (!state.allFinite()) {
            logger->error("State vector contains non-finite values: {}", state);
        }
        state_vector_ = state;
        logger->trace("Set state vector: size={}, values={}, expected_size={}, ambiguities={}", state.size(),
            state, getStateSize(), ambiguity_indices_.size());
    }

    /*
    void StateManager::setStateVectorPrevious(const Eigen::VectorXd& state) {
        auto logger = spdlog::get("state_manager");
        if (static_cast<size_t>(state.size()) != getStateSize()) {
            logger->error("State vector size {} does not match expected size {}", state.size(), getStateSize());
            throw std::invalid_argument("State vector size " + std::to_string(state.size()) +
                " does not match expected size " + std::to_string(getStateSize()));
        }
        if (!state.allFinite()) {
            logger->error("State vector contains non-finite values: {}", state);
        }
        state_vector_prev_ = state;
        logger->trace("Set state vector: size={}, values={}, expected_size={}, ambiguities={}", state.size(),
            state, getStateSize(), ambiguity_indices_.size());
    }
    */

    void StateManager::setCovariance(const Eigen::MatrixXd& covariance) {
        auto logger = spdlog::get("state_manager");
        if (static_cast<size_t>(covariance.rows()) != getStateSize() ||
            static_cast<size_t>(covariance.cols()) != getStateSize()) {
            logger->error("Covariance matrix dimensions ({}x{}) do not match state size {}",
                covariance.rows(), covariance.cols(), getStateSize());
            throw std::invalid_argument("Covariance matrix dimensions (" +
                std::to_string(covariance.rows()) + "x" +
                std::to_string(covariance.cols()) +
                ") do not match state size " + std::to_string(getStateSize()));
        }
        if (!covariance.allFinite()) {
            logger->error("Covariance matrix contains non-finite values");
            throw std::invalid_argument("Covariance matrix contains non-finite values");
        }
        covariance_ = covariance;
        logger->trace("Set covariance matrix: size={}x{}, expected_size={}, ambiguities={}", covariance.rows(), covariance.cols(),
            getStateSize(), ambiguity_indices_.size());
    }

    /*
    void StateManager::setCovariancePrevious(const Eigen::MatrixXd& covariance) {
        auto logger = spdlog::get("state_manager");
        if (static_cast<size_t>(covariance.rows()) != getStateSize() ||
            static_cast<size_t>(covariance.cols()) != getStateSize()) {
            logger->error("Covariance matrix dimensions ({}x{}) do not match state size {}",
                covariance.rows(), covariance.cols(), getStateSize());
            throw std::invalid_argument("Covariance matrix dimensions (" +
                std::to_string(covariance.rows()) + "x" +
                std::to_string(covariance.cols()) +
                ") do not match state size " + std::to_string(getStateSize()));
        }
        if (!covariance.allFinite()) {
            logger->error("Covariance matrix contains non-finite values");
            throw std::invalid_argument("Covariance matrix contains non-finite values");
        }
        covariance_prev_ = covariance;
        logger->trace("Set covariance matrix: size={}x{}, expected_size={}, ambiguities={}", covariance.rows(), covariance.cols(),
            getStateSize(), ambiguity_indices_.size());
    }
    */

    size_t StateManager::getStateSize() const {
        return core_state_size_ + ambiguity_indices_.size();
    }

    Eigen::VectorXd StateManager::getStateVector() const {
        return state_vector_;
    }

    /*
    Eigen::VectorXd StateManager::getStateVectorPrevious() const {
        return state_vector_prev_;
    }
    */

    Eigen::MatrixXd StateManager::getCovariance() const {
        return covariance_;
    }

    /*
    Eigen::MatrixXd StateManager::getCovariancePrevious() const {
        return covariance_prev_;
    }
    */

    Eigen::MatrixXd StateManager::getProcessNoiseMatrix(double dt) const {
        auto logger = spdlog::get("state_manager");
        if (dt <= 0.0) {
            logger->error("Invalid time step: {}", dt);
            throw std::invalid_argument("Invalid time step: " + std::to_string(dt));
        }

        size_t state_size = getStateSize();
        Eigen::MatrixXd Q = Eigen::MatrixXd::Zero(state_size, state_size);

        // Default process noise standard deviations
        double pos_std = 0.0;      // Position
        double clock_std = 300.0;  // Clock
        double tropo_std = 0.0005;   // Troposphere
        double iono_std = 0.02;    // Ionosphere
        double amb_std = 0.0;    // Ambiguity

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

        // Ambiguities might not yet be included
        if (state_size > core_state_size_) {
            // Ambiguity noise
            for (const auto& [key, idx] : ambiguity_indices_) {
                double amb_noise = amb_std * dt;
                Q(idx, idx) = amb_noise * amb_noise;
            }
        }

        logger->trace("Computed process noise matrix: size={}x{}, dt={:.3f} s", Q.rows(), Q.cols(), dt);
        return Q;
    }

    Eigen::MatrixXd StateManager::getTransitionMatrix(double dt) const {
        size_t state_size = getStateSize();
        Eigen::MatrixXd F = Eigen::MatrixXd::Identity(state_size, state_size); // simple, without dynamics
        spdlog::get("state_manager")->trace("Computed transition matrix: size={}x{}, dt={:.3f} s", F.rows(), F.cols(), dt);
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

    const std::map<std::string, size_t>& StateManager::getAmbiguities() const {
        return ambiguity_indices_;
    }

    size_t StateManager::addAmbiguity(const std::string& key, double initial_value) {
        auto logger = spdlog::get("state_manager");
        if (ambiguity_indices_.count(key)) {
            logger->trace("Ambiguity {} already exists at index {}", key, ambiguity_indices_[key]);
            return ambiguity_indices_[key];
        }
        size_t index = getStateSize();
        ambiguity_indices_[key] = index;

        // Resize state and covariance, preserving existing values
        Eigen::VectorXd new_state(index + 1);
        Eigen::MatrixXd new_cov(index + 1, index + 1);
        new_state.setZero();
        new_cov.setIdentity();

        // Copy existing state and covariance
        if (index > 0) {
            new_state.head(index) = state_vector_;
            new_cov.topLeftCorner(index, index) = covariance_;
        }
        new_state(index) = initial_value;
        new_cov(index, index) = 300.0; // Initial ambiguity variance (meters^2)

        state_vector_ = new_state;
        covariance_ = new_cov;

        logger->info("Added ambiguity {} at index {} with initial value {:.3f}", key, index, initial_value);
        return index;
    }

    void StateManager::updatePppStateFromVector(const Eigen::VectorXd& current_state, PppState& ppp_state, const StateManager& state_manager) const {
        // Position
        ppp_state.position = current_state.segment<3>(state_manager.getPositionIndex());

        // Clock bias
        ppp_state.clock_bias = current_state(state_manager.getClockIndex());

        // Troposphere zenith delay
        ppp_state.troposphere_zenith_delay = current_state(state_manager.getTropoIndex());

        // Ionosphere zenith delay only if using PPP-RTK
        if (core_state_size_ >= 6) {
            ppp_state.ionosphere_zenith_delay = current_state(state_manager.getIonoIndex());
        }

        // Ambiguities
        const auto& ambiguities = state_manager.getAmbiguities();
        for (const auto& [key, idx] : ambiguities) {
            if (state_manager.getAmbiguityIndex(key) > 0) {
                ppp_state.ambiguities[key] = current_state(state_manager.getAmbiguityIndex(key));
            }
        }
    }


} // namespace GnssProcessor