/*!
 * \file StateManager.cpp
 * \brief Implementation of StateManager: manages Kalman filter state, covariance and ambiguities.
 *
 * This file implements helpers to manipulate the filter state vector and covariance,
 * build process-noise and transition matrices, manage ambiguity indices, and store
 * persistent per-measurement variance scaling. Logging is performed via LogManager.
 */

#include "StateManager.hpp"
#include "LogManager.hpp"
#include <stdexcept>
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <Eigen/Dense>

// Formatter specialization for Eigen::VectorXd
/*!
 * \brief fmt formatter specialization for printing Eigen::VectorXd.
 *
 * Produces a human-readable comma-separated list enclosed in square brackets,
 * e.g. "[1.234, 2.345, 3.456]".
 */
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

    /**
     * @brief Construct a StateManager instance.
     *
     * Initializes the internal state vector and covariance with core-state dimensions
     * provided by \p core_state_size. Initial covariance diagonal values for core
     * states are seeded from \p config.getInitialStd(). The LogManager is used for
     * informational tracing.
     *
     * @param core_state_size Number of core states (e.g., 5 for PPP, 6 for PPP-RTK).
     * @param log_manager Reference to LogManager used for diagnostic logging.
     * @param config Read-only configuration providing default initial standard deviations.
     */
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

    /**
     * @brief Return the epoch time associated with the current state.
     * @return GnssTime containing GPS week and time-of-week for the stored state.
     */
    GnssTime StateManager::getStateEpochTime() const {
        return state_epoch_time_;
    }

    /**
     * @brief Set the epoch time for the current state.
     *
     * Updates the internal epoch timestamp and emits a trace-level log.
     *
     * @param time GnssTime to associate with the current state.
     */
    void StateManager::setStateEpochTime(const GnssTime& time) {
        state_epoch_time_ = time;
        auto logger = log_manager_.getLogger("state_manager");
        logger->trace("Set state epoch time: week={}, TOW={:.3f}", time.week, time.tow);
    }

    /**
     * @brief Replace the internal state vector.
     *
     * The provided vector must have size equal to getStateSize(). The method
     * validates finiteness of values and will throw if the size is incorrect.
     *
     * @param state New state vector to set (copy).
     * @throws std::invalid_argument if the provided vector size does not match the expected state size.
     */
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

    /**
     * @brief Replace the internal covariance matrix.
     *
     * The matrix must be square and its dimensions must equal the current state size.
     * Non-finite entries are rejected.
     *
     * @param covariance New covariance matrix to set (copy).
     * @throws std::invalid_argument if dimensions do not match state size or the matrix contains non-finite values.
     */
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

    /**
     * @brief Total state dimension (core states + ambiguity states).
     * @return Number of states currently tracked.
     */
    size_t StateManager::getStateSize() const {
        return core_state_size_ + ambiguity_indices_.size();
    }

    /**
     * @brief Get a copy of the internal state vector.
     * @return Eigen::VectorXd copy of the state vector.
     */
    Eigen::VectorXd StateManager::getStateVector() const {
        return state_vector_;
    }

    /**
     * @brief Get a copy of the internal covariance matrix.
     * @return Eigen::MatrixXd copy of the covariance matrix.
     */
    Eigen::MatrixXd StateManager::getCovariance() const {
        return covariance_;
    }

    /**
     * @brief Build the process-noise matrix Q for a prediction interval dt.
     *
     * The implementation uses spectral densities stored in the configuration
     * (units: m^2/s) and multiplies by dt to obtain variances (m^2).
     * Core-state entries (position, clock, troposphere, optional ionosphere)
     * and any ambiguity diagonal entries are populated. Ambiguity process noise
     * is typically zero (or near-zero) for constant ambiguity modeling.
     *
     * @param dt Time interval in seconds for which Q is requested (must be > 0).
     * @return Q matrix sized (state_size x state_size).
     * @throws std::invalid_argument if dt <= 0.
     */
    Eigen::MatrixXd StateManager::getProcessNoiseMatrix(double dt) const {
        auto logger = log_manager_.getLogger("state_manager");
        if (dt <= 0.0) {
            logger->error("Invalid time step: {}", dt);
            throw std::invalid_argument("Invalid time step: " + std::to_string(dt));
        }

        size_t state_size = getStateSize();
        Eigen::MatrixXd Q = Eigen::MatrixXd::Zero(state_size, state_size);

        // Process noise: variance = spectral_density * dt
        // Config stores spectral density in units of m²/s (variance rate)
        const auto& pn = config_.getProcessNoise();

        size_t posIdx = getPositionIndex();
        size_t clockIdx = getClockIndex();
        size_t tropoIdx = getTropoIndex();

        // Position noise (m²/s * s = m²)
        double pos_var = pn.position * dt;
        Q.block(posIdx, posIdx, 3, 3) = Eigen::Matrix3d::Identity() * pos_var;

        // Clock noise - typically very large spectral density for white noise clock
        double clock_var = pn.clock * dt;
        Q(clockIdx, clockIdx) = clock_var;

        // Tropospheric noise
        double tropo_var = pn.troposphere * dt;
        Q(tropoIdx, tropoIdx) = tropo_var;

        // Ionospheric noise (PPP-RTK only)
        if (core_state_size_ >= 6) {
            size_t ionoIdx = getIonoIndex();
            double iono_var = pn.ionosphere * dt;
            Q(ionoIdx, ionoIdx) = iono_var;
        }

        // Ambiguity process noise (should be 0 for constant ambiguities)
        if (state_size > core_state_size_) {
            for (const auto& [key, idx] : ambiguity_indices_) {
                double amb_var = pn.ambiguity * dt;
                Q(idx, idx) = amb_var;
            }
        }

        logger->trace("Computed process noise matrix: size={}x{}, dt={:.3f} s, clock_var={:.3e}", 
            Q.rows(), Q.cols(), dt, clock_var);
        return Q;
    }

    /**
     * @brief Get the state transition matrix for interval dt.
     *
     * Current implementation returns the identity matrix (no dynamics) sized
     * to the current state dimension. The method logs computed matrix size.
     *
     * @param dt Time interval in seconds; currently unused but kept for API compatibility.
     * @return Identity transition matrix (state_size x state_size).
     */
    Eigen::MatrixXd StateManager::getTransitionMatrix(double dt) const {
        size_t state_size = getStateSize();
        Eigen::MatrixXd F = Eigen::MatrixXd::Identity(state_size, state_size); // simple, without dynamics
        auto logger = log_manager_.getLogger("state_manager");
        logger->trace("Computed transition matrix: size={}x{}, dt={:.3f} s", F.rows(), F.cols(), dt);
        return F;
    }

    /**
     * @brief Index of the first position element (x) in the state vector.
     * @return Zero-based index for position (always 0).
     */
    size_t StateManager::getPositionIndex() const {
        return 0;
    }

    /**
     * @brief Index of the receiver clock bias state within the state vector.
     * @return Zero-based index for clock bias (default 3).
     */
    size_t StateManager::getClockIndex() const {
        return 3;
    }

    /**
     * @brief Index of the tropospheric zenith wet delay (ZWD) state.
     * @return Zero-based index for tropo state (default 4).
     */
    size_t StateManager::getTropoIndex() const {
        return 4;
    }

    /**
     * @brief Index of the ionospheric zenith delay state (if present).
     *
     * If PPP-RTK mode is enabled (core_state_size_ >= 6) returns the ionosphere index;
     * otherwise returns std::numeric_limits<size_t>::max() to indicate absence.
     *
     * @return Iono state index or max() when iono state is not present.
     */
    size_t StateManager::getIonoIndex() const {
        return core_state_size_ >= 6 ? 5 : std::numeric_limits<size_t>::max();
    }

    /**
     * @brief Get ambiguity state index corresponding to a key.
     * @param key Ambiguity name/key (typically "G01_L1" or similar).
     * @return Zero-based index of the ambiguity state, or -1 if not found.
     */
    int StateManager::getAmbiguityIndex(const std::string& key) const {
        auto it = ambiguity_indices_.find(key);
        return it != ambiguity_indices_.end() ? static_cast<int>(it->second) : -1;
    }

    /**
     * @brief Get reference to the map of ambiguity keys to indices.
     * @return Const reference to the internal ambiguity index map.
     */
    const std::map<std::string, size_t>& StateManager::getAmbiguities() const {
        return ambiguity_indices_;
    }

    /**
     * @brief Add an ambiguity state and return its assigned index.
     *
     * If the ambiguity already exists the existing index is returned.
     * The state vector and covariance matrix are resized preserving existing
     * values; the new diagonal ambiguity variance is initialized from the
     * configuration ambiguity standard deviation.
     *
     * @param key Unique ambiguity key.
     * @param initial_value Initial ambiguity value in meters.
     * @return Index assigned to the ambiguity (existing or newly created).
     */
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

        const double amb_std_m = config_.getAmbiguityStd();
        new_cov(index, index) = amb_std_m * amb_std_m; // Initial ambiguity variance (std^2 -> meters^2)

        state_vector_ = new_state;
        covariance_ = new_cov;

        logger->info("Added ambiguity {} at index {} with initial value {:.3f}", key, index, initial_value);
        return index;
    }

    /**
     * @brief Populate a PppState from a raw state vector.
     *
     * Copies core-state entries (position, clock, troposphere, optional ionosphere)
     * and any tracked ambiguities into the provided \p ppp_state.
     *
     * @param current_state State vector to read values from.
     * @param ppp_state Output structure that will receive core states and ambiguities.
     * @param state_manager Reference used to resolve ambiguity indices (kept for API compatibility).
     */
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

    /**
     * @brief Synchronize values from a PppState into the internal state vector.
     *
     * Copies core-state values from \p ppp_state into the internal state vector and
     * calls setStateVector, which validates sizes and finiteness.
     *
     * @param ppp_state Source PppState containing values to sync.
     */
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

    /**
     * @brief Reset (or create and reset) a float ambiguity state.
     *
     * Behavior:
     *  - Adds the ambiguity if it does not exist.
     *  - Sets ambiguity value to 0.0 m.
     *  - Zeros cross-covariance rows/columns for that ambiguity.
     *  - Sets diagonal variance to (std_m)^2.
     *
     * @param key Ambiguity key identifying the float ambiguity.
     * @param std_m Standard deviation in meters to set on the ambiguity diagonal (must be > 0).
     * @return true if the ambiguity was reset (or created and reset); false on invalid input or failure.
     */
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

    /**
     * @brief Helper to build the persistent measurement key from satellite and signal.
     * @param sat Satellite identifier (e.g. "G01").
     * @param sig Signal type (e.g. "C1C" or "L1").
     * @return Concatenated key "sat_sig".
     */
    static std::string makeMeasKey(const std::string& sat, const std::string& sig) {
        return sat + "_" + sig;
    }

    /**
     * @brief Persistently set a measurement variance scaling factor for a satellite/signal.
     *
     * The scale should be > 0. The stored scale is later used to inflate measurement variances
     * for persistent outlier handling (e.g., increasing code noise for a misbehaving observable).
     *
     * @param sat Satellite identifier string.
     * @param signal_type Signal type string.
     * @param scale Multiplier to persist (must be > 0).
     */
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

    /**
     * @brief Retrieve the persistent measurement variance scale for a satellite/signal.
     * @param sat Satellite identifier.
     * @param signal_type Signal type string.
     * @return Stored multiplier if present and valid; otherwise returns 1.0 (no scaling).
     */
    double StateManager::getMeasurementVarianceScale(const std::string& sat, const std::string& signal_type) const {
        if (sat.empty() || signal_type.empty()) return 1.0;

        const std::string key = makeMeasKey(sat, signal_type);
        auto it = meas_var_scale_.find(key);
        return (it != meas_var_scale_.end() && std::isfinite(it->second) && it->second > 0.0) ? it->second : 1.0;
    }

    /**
     * @brief Multiply the stored measurement variance scale by \p factor.
     *
     * Convenience wrapper that reads the current scale (default 1.0), multiplies
     * by \p factor and stores the new value. Intended for persistent inflation
     * after repeated outliers.
     *
     * @param sat Satellite identifier.
     * @param signal_type Signal type string.
     * @param factor Multiplicative factor (must be > 0).
     */
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

    /**
     * @brief Clear all persisted measurement variance scales.
     *
     * Clears the internal map and logs the action.
     */
    void StateManager::clearMeasurementVarianceScales() {
        auto logger = log_manager_.getLogger("state_manager");
        meas_var_scale_.clear();
        logger->info("Cleared measurement variance scales");
    }

} // namespace GnssProcessor