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
#include "Constants.hpp"
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
     * For multi-GNSS processing, inter-system time biases are added for each active
     * system except GPS (which serves as the reference). The core_state_size is
     * extended to include these additional states.
     *
     * @param core_state_size Number of base core states (e.g., 5 for PPP, 6 for PPP-RTK).
     * @param log_manager Reference to LogManager used for diagnostic logging.
     * @param config Read-only configuration providing default initial standard deviations.
     */
    StateManager::StateManager(size_t core_state_size, LogManager& log_manager, const GnssConfig& config)
        : base_core_size_(core_state_size), state_epoch_time_{ 0, 0.0 }, log_manager_(log_manager), config_(config),
          gradient_north_idx_(std::numeric_limits<size_t>::max()),
          gradient_east_idx_(std::numeric_limits<size_t>::max()) {
        
        // If tropo gradients are enabled, insert Gn and Ge right after the last
        // base core state (after ZWD for PPP, after iono for PPP-RTK).
        if (config_.estimateTropoGradients()) {
            gradient_north_idx_ = base_core_size_;
            gradient_east_idx_ = base_core_size_ + 1;
            base_core_size_ += 2;
        }

        // Determine which systems need inter-system time bias states
        // GPS is the reference system, so we add time offset states for all other active systems
        const auto& active_systems = config_.getActiveSystems();
        systems_with_time_offsets_.clear();
        system_time_offset_indices_.clear();
        
        size_t time_offset_idx = base_core_size_;
        for (const auto& sys : active_systems) {
            if (sys != "GPS") {
                systems_with_time_offsets_.push_back(sys);
                system_time_offset_indices_[sys] = time_offset_idx;
                ++time_offset_idx;
            }
        }
        
        // Total core state size includes base states plus inter-system time offsets
        core_state_size_ = base_core_size_ + systems_with_time_offsets_.size();
        
        state_vector_ = Eigen::VectorXd::Zero(core_state_size_);
        covariance_ = Eigen::MatrixXd::Identity(core_state_size_, core_state_size_);
        
        // Default initial standard deviations
        const auto& init = config_.getInitialStd();
        double pos_var = init.position * init.position;
        double clock_var = init.clock * init.clock;
        double tropo_var = init.troposphere * init.troposphere;
        double iono_var = init.ionosphere * init.ionosphere;
        double time_offset_var = init.system_time_offset * init.system_time_offset;
        
        covariance_.block(0, 0, 3, 3) = Eigen::Matrix3d::Identity() * pos_var;
        covariance_(3, 3) = clock_var;
        covariance_(4, 4) = tropo_var;
        if (config_.getProcessingMode() == ProcessingMode::PPP_RTK) {
            covariance_(5, 5) = iono_var;
        }
        
        // Initialize gradient covariances if enabled
        if (config_.estimateTropoGradients()) {
            double grad_var = init.tropo_gradient * init.tropo_gradient;
            covariance_(gradient_north_idx_, gradient_north_idx_) = grad_var;
            covariance_(gradient_east_idx_, gradient_east_idx_) = grad_var;
        }
        
        // Initialize inter-system time offset covariances
        for (const auto& [sys_name, idx] : system_time_offset_indices_) {
            covariance_(idx, idx) = time_offset_var;
        }
        
        // Log initialization using LogManager-provided logger
        auto logger = log_manager_.getLogger("state_manager");
        logger->info("StateManager initialized with core state size: {} (base={}, gradients={}, time_offsets={})",
            core_state_size_, base_core_size_, config_.estimateTropoGradients() ? 2 : 0, systems_with_time_offsets_.size());
        
        if (!systems_with_time_offsets_.empty()) {
            std::string offset_systems;
            for (const auto& sys : systems_with_time_offsets_) {
                offset_systems += sys + " ";
            }
            logger->info("Inter-system time offsets enabled for: {}", offset_systems);
        }
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
     * Core-state entries (position, clock, troposphere, optional ionosphere,
     * inter-system time offsets) and any ambiguity diagonal entries are populated.
     * Ambiguity process noise is typically zero (or near-zero) for constant
     * ambiguity modeling. Inter-system time offsets use a very small process
     * noise to model the slow drift of inter-system biases.
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

        // Tropospheric gradient noise (Gn, Ge)
        if (config_.estimateTropoGradients()) {
            double grad_var = pn.tropo_gradient * dt;
            Q(gradient_north_idx_, gradient_north_idx_) = grad_var;
            Q(gradient_east_idx_, gradient_east_idx_) = grad_var;
        }

        // Ionospheric noise (PPP-RTK only)
        if (config_.getProcessingMode() == ProcessingMode::PPP_RTK) {
            size_t ionoIdx = getIonoIndex();
            double iono_var = pn.ionosphere * dt;
            Q(ionoIdx, ionoIdx) = iono_var;
        }

        // Inter-system time offset noise (very small for slow drift)
        double time_offset_var = pn.system_time_offset * dt;
        for (const auto& [sys_name, idx] : system_time_offset_indices_) {
            Q(idx, idx) = time_offset_var;
        }

        // Ambiguity process noise (should be 0 for constant ambiguities)
        if (state_size > core_state_size_) {
            for (const auto& [key, idx] : ambiguity_indices_) {
                double amb_var = pn.ambiguity * dt;
                Q(idx, idx) = amb_var;
            }
        }

        logger->trace("Computed process noise matrix: size={}x{}, dt={:.3f} s, clock_var={:.3e}, time_offset_var={:.3e}", 
            Q.rows(), Q.cols(), dt, clock_var, time_offset_var);
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
     * If PPP-RTK mode is enabled returns the ionosphere index (always 5);
     * otherwise returns std::numeric_limits<size_t>::max() to indicate absence.
     *
     * @return Iono state index or max() when iono state is not present.
     */
    size_t StateManager::getIonoIndex() const {
        // Iono is always at index 5 when base PPP-RTK mode is active (before gradients were inserted).
        // But gradients are inserted after iono, so iono stays at 5.
        return (config_.getProcessingMode() == ProcessingMode::PPP_RTK) ? 5 : std::numeric_limits<size_t>::max();
    }

    size_t StateManager::getTropoGradientNorthIndex() const {
        return gradient_north_idx_;
    }

    size_t StateManager::getTropoGradientEastIndex() const {
        return gradient_east_idx_;
    }

    bool StateManager::hasTropoGradients() const {
        return gradient_north_idx_ != std::numeric_limits<size_t>::max();
    }

    /**
     * @brief Get the index of the inter-system time bias for a given system.
     *
     * @param system_name Name of the GNSS system (e.g., "Galileo", "GLONASS", "BeiDou").
     * @return Index of the system time bias state, or std::numeric_limits<size_t>::max() if not present.
     */
    size_t StateManager::getSystemTimeOffsetIndex(const std::string& system_name) const {
        auto it = system_time_offset_indices_.find(system_name);
        return it != system_time_offset_indices_.end() ? it->second : std::numeric_limits<size_t>::max();
    }

    /**
     * @brief Get the number of inter-system time bias states.
     *
     * @return Number of inter-system time bias states.
     */
    size_t StateManager::getNumSystemTimeOffsets() const {
        return systems_with_time_offsets_.size();
    }

    /**
     * @brief Get the list of systems that have time offset states.
     *
     * @return Vector of system names with time offset states.
     */
    const std::vector<std::string>& StateManager::getSystemsWithTimeOffsets() const {
        return systems_with_time_offsets_;
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
     * Copies core-state entries (position, clock, troposphere, optional ionosphere,
     * inter-system time offsets) and any tracked ambiguities into the provided \p ppp_state.
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

        // Troposphere gradients
        if (state_manager.hasTropoGradients()) {
            ppp_state.tropo_gradient_north = current_state(state_manager.getTropoGradientNorthIndex());
            ppp_state.tropo_gradient_east = current_state(state_manager.getTropoGradientEastIndex());
        }

        // Ionosphere zenith delay only if using PPP-RTK
        if (config_.getProcessingMode() == ProcessingMode::PPP_RTK) {
            ppp_state.ionosphere_zenith_delay = current_state(state_manager.getIonoIndex());
        }

        // Inter-system time offsets
        ppp_state.system_time_offsets.clear();
        for (const auto& sys : systems_with_time_offsets_) {
            size_t idx = getSystemTimeOffsetIndex(sys);
            if (idx < static_cast<size_t>(current_state.size())) {
                ppp_state.system_time_offsets[sys] = current_state(idx);
            }
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
     * Copies core-state values (position, clock, tropo, iono, inter-system time offsets)
     * from \p ppp_state into the internal state vector and calls setStateVector,
     * which validates sizes and finiteness.
     *
     * @param ppp_state Source PppState containing values to sync.
     */
    void StateManager::syncPppStateToStateVector(const PppState& ppp_state)
    {
        Eigen::VectorXd sv = state_vector_;
        sv.segment<3>(getPositionIndex()) = ppp_state.position;
        sv(getClockIndex()) = ppp_state.clock_bias;
        sv(getTropoIndex()) = ppp_state.troposphere_zwd;
        if (hasTropoGradients()) {
            sv(getTropoGradientNorthIndex()) = ppp_state.tropo_gradient_north;
            sv(getTropoGradientEastIndex()) = ppp_state.tropo_gradient_east;
        }
        if (config_.getProcessingMode() == ProcessingMode::PPP_RTK) {
            sv(getIonoIndex()) = ppp_state.ionosphere_zenith_delay;
        }
        
        // Sync inter-system time offsets
        for (const auto& [sys, value] : ppp_state.system_time_offsets) {
            size_t idx = getSystemTimeOffsetIndex(sys);
            if (idx < static_cast<size_t>(sv.size())) {
                sv(idx) = value;
            }
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
     * @brief Remove a tracked ambiguity from the state vector and covariance.
     *
     * Safely removes the ambiguity row/column and shifts subsequent ambiguity indices down.
     * Returns false when the key is not found or when attempting to remove a core-state index.
     */
    bool StateManager::removeAmbiguity(const std::string& key) {
        auto logger = log_manager_.getLogger("state_manager");
        if (key.empty()) {
            logger->warn("removeAmbiguity called with empty key");
            return false;
        }
        auto it = ambiguity_indices_.find(key);
        if (it == ambiguity_indices_.end()) {
            logger->trace("removeAmbiguity: key {} not found", key);
            return false;
        }

        const size_t rem_idx = it->second;
        const size_t state_size = getStateSize();
        if (rem_idx < core_state_size_ || state_size <= core_state_size_) {
            logger->warn("removeAmbiguity: attempting to remove invalid index {} (core_state_size={}, state_size={}) for key {}",
                rem_idx, core_state_size_, state_size, key);
            return false;
        }

        // Erase mapping first
        ambiguity_indices_.erase(it);

        const size_t new_size = state_size - 1;
        Eigen::VectorXd new_state = Eigen::VectorXd::Zero(new_size);
        Eigen::MatrixXd new_cov = Eigen::MatrixXd::Zero(new_size, new_size);

        // Copy head portion before removed index (may include core states)
        if (rem_idx > 0) {
            new_state.head(static_cast<int>(rem_idx)) = state_vector_.head(static_cast<int>(rem_idx));
            new_cov.topLeftCorner(static_cast<int>(rem_idx), static_cast<int>(rem_idx)) =
                covariance_.topLeftCorner(static_cast<int>(rem_idx), static_cast<int>(rem_idx));
        }

        // Copy tail portion after removed index, shifted left by one
        if (rem_idx + 1 < state_size) {
            const size_t tail = state_size - rem_idx - 1;
            new_state.tail(static_cast<int>(tail)) = state_vector_.segment(static_cast<int>(rem_idx + 1), static_cast<int>(tail));
            new_cov.topRightCorner(static_cast<int>(rem_idx), static_cast<int>(tail)).setZero();
            new_cov.bottomLeftCorner(static_cast<int>(tail), static_cast<int>(rem_idx)).setZero();

            new_cov.block(static_cast<int>(rem_idx), static_cast<int>(rem_idx),
                static_cast<int>(tail), static_cast<int>(tail)) =
                covariance_.block(static_cast<int>(rem_idx + 1), static_cast<int>(rem_idx + 1),
                    static_cast<int>(tail), static_cast<int>(tail));
            // Copy cross terms between head and tail
            if (rem_idx > 0) {
                new_cov.topRightCorner(static_cast<int>(rem_idx), static_cast<int>(tail)) =
                    covariance_.topRightCorner(static_cast<int>(rem_idx), static_cast<int>(tail));
                new_cov.bottomLeftCorner(static_cast<int>(tail), static_cast<int>(rem_idx)) =
                    covariance_.bottomLeftCorner(static_cast<int>(tail), static_cast<int>(rem_idx));
            }
        }

        state_vector_ = std::move(new_state);
        covariance_ = std::move(new_cov);

        // Update indices for remaining ambiguities (decrement indices > rem_idx)
        for (auto& kv : ambiguity_indices_) {
            if (kv.second > rem_idx) --kv.second;
        }

        logger->info("Removed ambiguity {} at former index {}. New state size={}", key, rem_idx, getStateSize());
        return true;
    }

    double StateManager::computeAmbiguitySeed(const PppObservation& obs,
                                               const std::string& ref_type,
                                               const Eigen::VectorXd& x) const {
        try {
            if (!obs.observed.count(ref_type) || !obs.modeled.count(ref_type))
                return 0.0;

            const double observed_m = obs.observed.at(ref_type);
            const double modeled_m  = obs.modeled.at(ref_type);
            const double rx_clock   = x(getClockIndex());
            const double zwd        = x(getTropoIndex());
            const double tropo_wet  = obs.mf_w * zwd;

            // Inter-system time offset for non-GPS satellites
            double isb = 0.0;
            {
                std::string system_name;
                switch (obs.system) {
                    case System::GPS:     system_name = ""; break;
                    case System::Galileo: system_name = "Galileo"; break;
                    case System::GLONASS: system_name = "GLONASS"; break;
                    case System::BeiDou:  system_name = "BeiDou"; break;
                    case System::QZSS:    system_name = "QZSS"; break;
                    case System::SBAS:    system_name = "SBAS"; break;
                    default:              system_name = ""; break;
                }
                size_t isb_idx = getSystemTimeOffsetIndex(system_name);
                if (isb_idx < static_cast<size_t>(x.size())) {
                    isb = x(static_cast<Eigen::Index>(isb_idx));
                }
            }

            // Tropospheric gradient contribution
            double tropo_grad = 0.0;
            if (hasTropoGradients()) {
                const double el_rad = obs.elevation * Constants::PI / 180.0;
                const double az_rad = obs.azimuth * Constants::PI / 180.0;
                const double sin_el = std::sin(el_rad);
                const double tan_el = std::tan(el_rad);
                const double mf_grad = 1.0 / (sin_el * tan_el + 0.0032);
                const double gn = x(static_cast<Eigen::Index>(getTropoGradientNorthIndex()));
                const double ge = x(static_cast<Eigen::Index>(getTropoGradientEastIndex()));
                tropo_grad = mf_grad * (gn * std::cos(az_rad) + ge * std::sin(az_rad));
            }

            return observed_m - modeled_m - rx_clock - tropo_wet - isb - tropo_grad;
        } catch (...) {
            return 0.0;
        }
    }

    void StateManager::pruneAmbiguitiesExcept(const std::set<std::string>& keep_keys) {
        auto logger = log_manager_.getLogger("state_manager");
        if (ambiguity_indices_.empty()) {
            logger->trace("pruneAmbiguitiesExcept: no ambiguities to prune");
            return;
        }

        // Collect keys to remove
        std::vector<std::string> to_remove;
        to_remove.reserve(ambiguity_indices_.size());
        for (const auto& kv : ambiguity_indices_) {
            if (keep_keys.find(kv.first) == keep_keys.end()) to_remove.push_back(kv.first);
        }

        if (to_remove.empty()) {
            logger->trace("pruneAmbiguitiesExcept: nothing to remove (keep_keys size={})", keep_keys.size());
            return;
        }

        logger->info("Pruning {} ambiguities (keeping {} keys)", to_remove.size(), keep_keys.size());
        // Remove in reverse-order by index to avoid repeated shifting costs is not necessary
        // because removeAmbiguity updates indices correctly. Remove in the order collected.
        for (const auto& key : to_remove) {
            removeAmbiguity(key);
        }
    }

} // namespace GnssProcessor