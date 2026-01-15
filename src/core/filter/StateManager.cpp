#include "StateManager.hpp"
#include "LogManager.hpp"
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

    StateManager::StateManager(size_t core_state_size, LogManager& log_manager, const GnssConfig& config)
        : core_state_size_(core_state_size), state_epoch_time_{ 0, 0.0 }, log_manager_(log_manager), config_(config) {
        state_vector_ = Eigen::VectorXd::Zero(core_state_size);
        covariance_ = Eigen::MatrixXd::Identity(core_state_size, core_state_size);
        // Default initial standard deviations
        const auto& init = config_.getInitialStd();
        double pos_var = init.position * init.position;
        double clock_var = init.clock * init.clock;
        double tropo_var = init.troposphere * init.troposphere;
        double iono_var = init.ionosphere * init.ionosphere;
        covariance_.block(0, 0, 3, 3) = Eigen::Matrix3d::Identity() * pos_var;
        covariance_(3, 3) = clock_var;
        covariance_(4, 4) = tropo_var;
        if (core_state_size >= 6) {
            covariance_(5, 5) = iono_var;
        }
        // Log initialization using LogManager-provided logger
        auto logger = log_manager_.getLogger("state_manager");
        logger->info("StateManager initialized with core state size: {}", core_state_size);
    }

    GnssTime StateManager::getStateEpochTime() const {
        return state_epoch_time_;
    }

    void StateManager::setStateEpochTime(const GnssTime& time) {
        state_epoch_time_ = time;
        auto logger = log_manager_.getLogger("state_manager");
        logger->trace("Set state epoch time: week={}, TOW={:.3f}", time.week, time.tow);
    }

    void StateManager::setStateVector(const Eigen::VectorXd& state) {
        auto logger = log_manager_.getLogger("state_manager");
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

    void StateManager::setCovariance(const Eigen::MatrixXd& covariance) {
        auto logger = log_manager_.getLogger("state_manager");
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
        auto logger = log_manager_.getLogger("state_manager");
        if (dt <= 0.0) {
            logger->error("Invalid time step: {}", dt);
            throw std::invalid_argument("Invalid time step: " + std::to_string(dt));
        }

        size_t state_size = getStateSize();
        Eigen::MatrixXd Q = Eigen::MatrixXd::Zero(state_size, state_size);

        // Default process noise standard deviations
        const auto& init = config_.getProcessNoise();
        double pos_std = init.position;
        double clock_std = init.clock;
        double tropo_std = init.troposphere;
        double iono_std = init.ionosphere;
        double amb_std = init.ambiguity;

        size_t posIdx = getPositionIndex();
        size_t clockIdx = getClockIndex();
        size_t tropoIdx = getTropoIndex();

        // Position noise
        double pos_var = (pos_std * dt) * (pos_std * dt);
        Q.block(posIdx, posIdx, 3, 3) = Eigen::Matrix3d::Identity() * pos_var;

        // Clock noise
        double clock_var = (clock_std * dt) * (clock_std * dt);
        Q(clockIdx, clockIdx) = clock_var;

        // Tropospheric noise (variance)
        double tropo_var = (tropo_std * dt) * (tropo_std * dt);
        Q(tropoIdx, tropoIdx) = tropo_var;

        // Ionospheric noise (PPP-RTK only)
        if (core_state_size_ >= 6) {
            size_t ionoIdx = getIonoIndex();
            double iono_var = (iono_std * dt) * (iono_std * dt);
            Q(ionoIdx, ionoIdx) = iono_var;
        }

        // Ambiguities might not yet be included
        if (state_size > core_state_size_) {
            // Ambiguity noise (variance)
            for (const auto& [key, idx] : ambiguity_indices_) {
                double amb_var = (amb_std * dt) * (amb_std * dt);
                Q(idx, idx) = amb_var;
            }
        }

        logger->trace("Computed process noise matrix: size={}x{}, dt={:.3f} s", Q.rows(), Q.cols(), dt);
        return Q;
    }

    Eigen::MatrixXd StateManager::getTransitionMatrix(double dt) const {
        size_t state_size = getStateSize();
        Eigen::MatrixXd F = Eigen::MatrixXd::Identity(state_size, state_size); // simple, without dynamics
        auto logger = log_manager_.getLogger("state_manager");
        logger->trace("Computed transition matrix: size={}x{}, dt={:.3f} s", F.rows(), F.cols(), dt);
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
        auto logger = log_manager_.getLogger("state_manager");
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
        new_cov(index, index) = config_.getAmbiguityStd(); // Initial ambiguity variance (meters^2)

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
        ppp_state.troposphere_zwd = current_state(state_manager.getTropoIndex());

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

    void StateManager::syncPppStateToStateVector(const PppState& ppp_state)
    {
        Eigen::VectorXd sv = state_vector_;
        sv.segment<3>(getPositionIndex()) = ppp_state.position;
        sv(getClockIndex()) = ppp_state.clock_bias;
        sv(getTropoIndex()) = ppp_state.troposphere_zwd;
        if (core_state_size_ >= 6) {
            sv(getIonoIndex()) = ppp_state.ionosphere_zenith_delay;
        }
        setStateVector(sv);
    }

    bool StateManager::resetAmbiguity(const std::string& key, double std_m) {
        auto logger = log_manager_.getLogger("state_manager");

        if (key.empty()) {
            logger->warn("resetAmbiguity called with empty key");
            return false;
        }
        if (!(std::isfinite(std_m) && std_m > 0.0)) {
            logger->warn("resetAmbiguity called with invalid std_m for key {}: {}", key, std_m);
            return false;
        }

        int idx_i = getAmbiguityIndex(key);
        if (idx_i < 0) {
            addAmbiguity(key, 0.0);
            idx_i = getAmbiguityIndex(key);
        }
        if (idx_i < 0) {
            logger->error("Failed to add ambiguity for key {}", key);
            return false;
        }

        const size_t idx = static_cast<size_t>(idx_i);

        if (idx >= static_cast<size_t>(state_vector_.size()) ||
            idx >= static_cast<size_t>(covariance_.rows()) ||
            idx >= static_cast<size_t>(covariance_.cols())) {
            logger->error("resetAmbiguity index out of bounds for key {}: idx={}, state_size={}, cov={}x{}",
                key, idx, state_vector_.size(), covariance_.rows(), covariance_.cols());
            return false;
        }

        state_vector_(static_cast<Eigen::Index>(idx)) = 0.0;

        // Zero cross-covariances first, then restore diagonal (avoid wiping it to 0).
        covariance_.row(static_cast<Eigen::Index>(idx)).setZero();
        covariance_.col(static_cast<Eigen::Index>(idx)).setZero();
        covariance_(static_cast<Eigen::Index>(idx), static_cast<Eigen::Index>(idx)) = std_m * std_m;

        logger->info("Reset ambiguity {} at index {}: value=0.0 m, std={:.3f} m (var={:.3e})",
            key, idx, std_m, std_m * std_m);

        return true;
    }

    static std::string makeMeasKey(const std::string& sat, const std::string& sig) {
        return sat + "_" + sig;
    }

    void StateManager::setMeasurementVarianceScale(const std::string& sat, const std::string& signal_type, double scale) {
        auto logger = log_manager_.getLogger("state_manager");
        if (sat.empty() || signal_type.empty()) {
            logger->warn("setMeasurementVarianceScale called with empty sat/signal");
            return;
        }
        if (!(std::isfinite(scale) && scale > 0.0)) {
            logger->warn("setMeasurementVarianceScale called with invalid scale for {} {}: {}", sat, signal_type, scale);
            return;
        }

        const std::string key = makeMeasKey(sat, signal_type);
        meas_var_scale_[key] = scale;

        logger->info("Set measurement variance scale: {} -> {:.3f}", key, scale);
    }

    double StateManager::getMeasurementVarianceScale(const std::string& sat, const std::string& signal_type) const {
        if (sat.empty() || signal_type.empty()) return 1.0;

        const std::string key = makeMeasKey(sat, signal_type);
        auto it = meas_var_scale_.find(key);
        return (it != meas_var_scale_.end() && std::isfinite(it->second) && it->second > 0.0) ? it->second : 1.0;
    }

    void StateManager::inflateMeasurementVarianceScale(const std::string& sat, const std::string& signal_type, double factor) {
        auto logger = log_manager_.getLogger("state_manager");
        if (sat.empty() || signal_type.empty()) {
            logger->warn("inflateMeasurementVarianceScale called with empty sat/signal");
            return;
        }
        if (!(std::isfinite(factor) && factor > 0.0)) {
            logger->warn("inflateMeasurementVarianceScale called with invalid factor for {} {}: {}", sat, signal_type, factor);
            return;
        }

        const double current = getMeasurementVarianceScale(sat, signal_type);
        const double next = current * factor;
        setMeasurementVarianceScale(sat, signal_type, next);

        logger->info("Inflated measurement variance scale: {}_{}: {:.3f} -> {:.3f}",
            sat, signal_type, current, next);
    }

    void StateManager::clearMeasurementVarianceScales() {
        auto logger = log_manager_.getLogger("state_manager");
        meas_var_scale_.clear();
        logger->info("Cleared measurement variance scales");
    }

} // namespace GnssProcessor