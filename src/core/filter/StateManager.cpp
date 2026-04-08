/*!
/*!
 * \file StateManager.cpp
 * \brief Implementation of StateManager: manages Kalman filter state, covariance and ambiguities.
 *
 * The implementation is organized as:
 *  - Internal helpers:    appendState(), removeStateAtIndex()
 *  - Construction
 *  - Core accessors/mutators
 *  - Process noise / transition
 *  - Index getters
 *  - Ambiguity management
 *  - PppState sync
 *  - Ionosphere state management
 *  - Code bias state management
 */

#include "StateManager.hpp"
#include "LogManager.hpp"
#include "Constants.hpp"
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
            if (i < vec.size() - 1)
                out = fmt::format_to(out, ", ");
        }
        return fmt::format_to(out, "]");
    }
};

namespace GnssProcessor {

    // ===================================================================
    // File-scope helpers
    // ===================================================================

    namespace {

    /// Map System enum to ISB constellation name (empty for GPS = reference).
    std::string systemToIsbName(System system) {
        switch (system) {
        case System::GPS:     return "";
        case System::Galileo: return "Galileo";
        case System::GLONASS: return "GLONASS";
        case System::BeiDou:  return "BeiDou";
        case System::QZSS:    return "QZSS";
        case System::SBAS:    return "SBAS";
        default:              return "";
        }
    }

    /// Map a band digit + GNSS system to the carrier frequency (Hz).
    /// Used by computeAmbiguitySeed() when PppUtils is not available.
    double bandFrequency(char band, System system) {
        switch (system) {
        case System::GPS:
        case System::QZSS:
        case System::SBAS:
            if (band == '2') return Constants::GPS_L2_FREQ;
            if (band == '5') return Constants::GPS_L5_FREQ;
            return Constants::GPS_L1_FREQ;
        case System::Galileo:
            if (band == '5') return Constants::GAL_E5A_FREQ;
            if (band == '7') return Constants::GAL_E5B_FREQ;
            if (band == '8') return Constants::GAL_E5_FREQ;
            if (band == '6') return Constants::GAL_E6_FREQ;
            return Constants::GPS_L1_FREQ;
        case System::BeiDou:
            if (band == '2') return Constants::BDS_B1_FREQ;
            if (band == '5') return Constants::BDS_B2A_FREQ;
            if (band == '7') return Constants::BDS_B2_FREQ;
            if (band == '8') return Constants::BDS_B2AB_FREQ;
            if (band == '6') return Constants::BDS_B3_FREQ;
            return Constants::GPS_L1_FREQ;
        case System::GLONASS:
            if (band == '2') return Constants::GLONASS_G2_BASE_FREQ;
            if (band == '3') return Constants::GLONASS_G3_BASE_FREQ;
            return Constants::GPS_L1_FREQ;
        default:
            return Constants::GPS_L1_FREQ;
        }
    }

    } // anonymous namespace

    // ===================================================================
    // Internal state vector / covariance manipulation
    // ===================================================================

    size_t StateManager::appendState(double initial_value, double initial_variance) {
        const size_t index = getStateSize();
        Eigen::VectorXd new_sv(index + 1);
        Eigen::MatrixXd new_cov = Eigen::MatrixXd::Zero(index + 1, index + 1);

        if (index > 0) {
            new_sv.head(index) = state_vector_;
            new_cov.topLeftCorner(index, index) = covariance_;
        }
        new_sv(index) = initial_value;
        new_cov(index, index) = initial_variance;

        state_vector_ = std::move(new_sv);
        covariance_ = std::move(new_cov);
        return index;
    }

    void StateManager::removeStateAtIndex(size_t rem_idx) {
        const size_t state_size = getStateSize();
        const size_t new_size = state_size - 1;
        const auto ri = static_cast<Eigen::Index>(rem_idx);

        Eigen::VectorXd new_sv = Eigen::VectorXd::Zero(new_size);
        Eigen::MatrixXd new_cov = Eigen::MatrixXd::Zero(new_size, new_size);

        // Head (before removed index)
        if (rem_idx > 0) {
            new_sv.head(ri) = state_vector_.head(ri);
            new_cov.topLeftCorner(ri, ri) = covariance_.topLeftCorner(ri, ri);
        }

        // Tail (after removed index)
        if (rem_idx + 1 < state_size) {
            const auto tail = static_cast<Eigen::Index>(state_size - rem_idx - 1);
            new_sv.segment(ri, tail) = state_vector_.segment(ri + 1, tail);
            new_cov.block(ri, ri, tail, tail) = covariance_.block(ri + 1, ri + 1, tail, tail);
            // Cross-covariance between head and tail
            if (rem_idx > 0) {
                new_cov.block(0, ri, ri, tail) = covariance_.block(0, ri + 1, ri, tail);
                new_cov.block(ri, 0, tail, ri) = covariance_.block(ri + 1, 0, tail, ri);
            }
        }

        state_vector_ = std::move(new_sv);
        covariance_ = std::move(new_cov);

        // Decrement all dynamic-state indices that were above the removed index
        for (auto& kv : ambiguity_indices_)   if (kv.second > rem_idx) --kv.second;
        for (auto& kv : code_bias_indices_)   if (kv.second > rem_idx) --kv.second;
        for (auto& kv : ionosphere_indices_)  if (kv.second > rem_idx) --kv.second;
    }

    // ===================================================================
    // Construction
    // ===================================================================

    StateManager::StateManager(size_t core_state_size, LogManager& log_manager, const GnssConfig& config)
        : base_core_size_(core_state_size), state_epoch_time_{ 0, 0.0 }, log_manager_(log_manager), config_(config),
          phase_clock_idx_(std::numeric_limits<size_t>::max()),
          gradient_north_idx_(std::numeric_limits<size_t>::max()),
          gradient_east_idx_(std::numeric_limits<size_t>::max()) {

        // Optional phase clock offset state (after ZWD).
        if (config_.estimatePhaseClock()) {
            phase_clock_idx_ = base_core_size_;
            base_core_size_ += 1;
        }

        // If tropo gradients are enabled, insert Gn and Ge right after the last
        // base core state (after phase clock if present, or after ZWD).
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
        double time_offset_var = init.system_time_offset * init.system_time_offset;
        
        covariance_.block(0, 0, 3, 3) = Eigen::Matrix3d::Identity() * pos_var;
        covariance_(3, 3) = clock_var;
        covariance_(4, 4) = tropo_var;
        
        // Initialize phase clock offset covariance if enabled
        if (config_.estimatePhaseClock()) {
            double phase_clock_var = init.phase_clock * init.phase_clock;
            covariance_(phase_clock_idx_, phase_clock_idx_) = phase_clock_var;
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
        logger->info("StateManager initialized with core state size: {} (base={}, phase_clock={}, gradients={}, time_offsets={})",
            core_state_size_, base_core_size_, config_.estimatePhaseClock() ? 1 : 0,
            config_.estimateTropoGradients() ? 2 : 0, systems_with_time_offsets_.size());
        
        if (!systems_with_time_offsets_.empty()) {
            std::string offset_systems;
            for (const auto& sys : systems_with_time_offsets_) {
                offset_systems += sys + " ";
            }
            logger->info("Inter-system time offsets enabled for: {}", offset_systems);
        }
    }

    // ===================================================================
    // Core accessors / mutators
    // ===================================================================

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
        return core_state_size_ + code_bias_indices_.size() + ionosphere_indices_.size() + ambiguity_indices_.size();
    }

    Eigen::VectorXd StateManager::getStateVector() const {
        return state_vector_;
    }

    Eigen::MatrixXd StateManager::getCovariance() const {
        return covariance_;
    }

    // ===================================================================
    // Process noise / transition
    // ===================================================================

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

        // Clock noise - typically very large spectral density for white noise clock.
        // When BSSD is active the clock column is zeroed in H, so the clock
        // receives no measurement update.  Skip process noise to prevent
        // unbounded variance growth; the WLS seeder maintains the clock value.
        if (!config_.enableBetweenSatelliteSd()) {
            double clock_var = pn.clock * dt;
            Q(clockIdx, clockIdx) = clock_var;
        }

        // Phase clock offset noise (when enabled)
        // Also suppressed under BSSD for the same reason as the clock.
        if (config_.estimatePhaseClock() && !config_.enableBetweenSatelliteSd()) {
            size_t phaseClockIdx = getPhaseClockIndex();
            double phase_clock_var = pn.phase_clock * dt;
            Q(phaseClockIdx, phaseClockIdx) = phase_clock_var;
        }

        // Tropospheric noise
        double tropo_var = pn.troposphere * dt;
        Q(tropoIdx, tropoIdx) = tropo_var;

        // Tropospheric gradient noise (Gn, Ge)
        if (config_.estimateTropoGradients()) {
            double grad_var = pn.tropo_gradient * dt;
            Q(gradient_north_idx_, gradient_north_idx_) = grad_var;
            Q(gradient_east_idx_, gradient_east_idx_) = grad_var;
        }

        // Per-satellite ionospheric noise (PPP-RTK uncombined)
        // Scale zenith spectral density by obliquity factor: q_slant = q_zenith / sin²(el)
        if (config_.getProcessingMode() == ProcessingMode::PPP_RTK) {
            const double q_zenith = pn.ionosphere;
            for (const auto& [sat, idx] : ionosphere_indices_) {
                double sin2_el = 1.0; // default: zenith (no scaling)
                auto el_it = ionosphere_elevations_.find(sat);
                if (el_it != ionosphere_elevations_.end() && el_it->second > 0.0) {
                    const double sin_el = std::sin(el_it->second * Constants::PI / 180.0);
                    sin2_el = sin_el * sin_el;
                    if (sin2_el < 0.01) sin2_el = 0.01; // floor at ~5.7 deg to avoid blow-up
                }
                Q(idx, idx) = (q_zenith / sin2_el) * dt;
            }
        }

        // Receiver code bias (IFB) noise (PPP-RTK uncombined)
        if (!code_bias_indices_.empty()) {
            double cb_var = pn.code_bias * dt;
            for (const auto& [key, idx] : code_bias_indices_) {
                Q(idx, idx) = cb_var;
            }
        }

        // Inter-system time offset noise (very small for slow drift).
        // Suppressed under BSSD: ISB columns are zeroed in the SD H-matrix.
        if (!config_.enableBetweenSatelliteSd()) {
            double time_offset_var = pn.system_time_offset * dt;
            for (const auto& [sys_name, idx] : system_time_offset_indices_) {
                Q(idx, idx) = time_offset_var;
            }
        }

        // Ambiguity process noise (should be 0 for constant ambiguities)
        if (state_size > core_state_size_) {
            for (const auto& [key, idx] : ambiguity_indices_) {
                double amb_var = pn.ambiguity * dt;
                Q(idx, idx) = amb_var;
            }
        }

        logger->trace("Computed process noise matrix: size={}x{}, dt={:.3f} s, clock_q={:.3e}, bssd={}",
            Q.rows(), Q.cols(), dt, pn.clock, config_.enableBetweenSatelliteSd() ? "on" : "off");
        return Q;
    }

    Eigen::MatrixXd StateManager::getTransitionMatrix(double dt) const {
        size_t state_size = getStateSize();
        Eigen::MatrixXd F = Eigen::MatrixXd::Identity(state_size, state_size); // simple, without dynamics
        auto logger = log_manager_.getLogger("state_manager");
        logger->trace("Computed transition matrix: size={}x{}, dt={:.3f} s", F.rows(), F.cols(), dt);
        return F;
    }

    // ===================================================================
    // Index getters
    // ===================================================================

    size_t StateManager::getPositionIndex() const { return 0; }
    size_t StateManager::getClockIndex() const { return 3; }
    size_t StateManager::getPhaseClockIndex() const { return phase_clock_idx_; }
    bool   StateManager::hasPhaseClock() const { return phase_clock_idx_ != std::numeric_limits<size_t>::max(); }
    size_t StateManager::getTropoIndex() const { return 4; }

    size_t StateManager::getIonoIndex() const {
        // Deprecated: per-satellite iono states are managed dynamically.
        return std::numeric_limits<size_t>::max();
    }

    size_t StateManager::getTropoGradientNorthIndex() const { return gradient_north_idx_; }
    size_t StateManager::getTropoGradientEastIndex() const { return gradient_east_idx_; }
    bool   StateManager::hasTropoGradients() const { return gradient_north_idx_ != std::numeric_limits<size_t>::max(); }

    size_t StateManager::getSystemTimeOffsetIndex(const std::string& system_name) const {
        auto it = system_time_offset_indices_.find(system_name);
        return it != system_time_offset_indices_.end() ? it->second : std::numeric_limits<size_t>::max();
    }

    size_t StateManager::getNumSystemTimeOffsets() const {
        return systems_with_time_offsets_.size();
    }

    const std::vector<std::string>& StateManager::getSystemsWithTimeOffsets() const {
        return systems_with_time_offsets_;
    }

    // ===================================================================
    // Ambiguity management
    // ===================================================================

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
        const double amb_std = config_.getAmbiguityStd();
        const size_t index = appendState(initial_value, amb_std * amb_std);
        ambiguity_indices_[key] = index;

        logger->info("Added ambiguity {} at index {} with initial value {:.3f}", key, index, initial_value);
        return index;
    }

    // ===================================================================
    // PppState synchronization
    // ===================================================================

    void StateManager::updatePppStateFromVector(const Eigen::VectorXd& current_state, PppState& ppp_state, const StateManager& state_manager) const {
        ppp_state.position         = current_state.segment<3>(state_manager.getPositionIndex());
        ppp_state.clock_bias       = current_state(state_manager.getClockIndex());
        ppp_state.troposphere_zwd  = current_state(state_manager.getTropoIndex());

        if (state_manager.hasPhaseClock())
            ppp_state.phase_clock_offset = current_state(state_manager.getPhaseClockIndex());
        if (state_manager.hasTropoGradients()) {
            ppp_state.tropo_gradient_north = current_state(state_manager.getTropoGradientNorthIndex());
            ppp_state.tropo_gradient_east  = current_state(state_manager.getTropoGradientEastIndex());
        }

        if (config_.getProcessingMode() == ProcessingMode::PPP_RTK) {
            ppp_state.slant_ionosphere.clear();
            for (const auto& [sat, idx] : ionosphere_indices_)
                if (idx < static_cast<size_t>(current_state.size()))
                    ppp_state.slant_ionosphere[sat] = current_state(idx);
        }

        ppp_state.system_time_offsets.clear();
        for (const auto& sys : systems_with_time_offsets_) {
            const size_t idx = getSystemTimeOffsetIndex(sys);
            if (idx < static_cast<size_t>(current_state.size()))
                ppp_state.system_time_offsets[sys] = current_state(idx);
        }

        for (const auto& [key, idx] : state_manager.getAmbiguities())
            if (state_manager.getAmbiguityIndex(key) > 0)
                ppp_state.ambiguities[key] = current_state(state_manager.getAmbiguityIndex(key));
    }

    void StateManager::syncPppStateToStateVector(const PppState& ppp_state) {
        Eigen::VectorXd sv = state_vector_;
        sv.segment<3>(getPositionIndex()) = ppp_state.position;
        sv(getClockIndex()) = ppp_state.clock_bias;
        sv(getTropoIndex()) = ppp_state.troposphere_zwd;

        if (hasPhaseClock())
            sv(getPhaseClockIndex()) = ppp_state.phase_clock_offset;
        if (hasTropoGradients()) {
            sv(getTropoGradientNorthIndex()) = ppp_state.tropo_gradient_north;
            sv(getTropoGradientEastIndex())  = ppp_state.tropo_gradient_east;
        }

        if (config_.getProcessingMode() == ProcessingMode::PPP_RTK) {
            for (const auto& [sat, value] : ppp_state.slant_ionosphere) {
                auto it = ionosphere_indices_.find(sat);
                if (it != ionosphere_indices_.end() && it->second < static_cast<size_t>(sv.size()))
                    sv(it->second) = value;
            }
        }

        for (const auto& [sys, value] : ppp_state.system_time_offsets) {
            const size_t idx = getSystemTimeOffsetIndex(sys);
            if (idx < static_cast<size_t>(sv.size()))
                sv(idx) = value;
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

        const auto idx = static_cast<Eigen::Index>(idx_i);
        if (idx >= state_vector_.size() || idx >= covariance_.rows()) {
            logger->error("resetAmbiguity index out of bounds for key {}: idx={}, state_size={}, cov={}x{}",
                key, idx_i, state_vector_.size(), covariance_.rows(), covariance_.cols());
            return false;
        }

        state_vector_(idx) = 0.0;
        covariance_.row(idx).setZero();
        covariance_.col(idx).setZero();
        covariance_(idx, idx) = std_m * std_m;

        logger->info("Reset ambiguity {} at index {}: value=0.0 m, std={:.3f} m (var={:.3e})",
            key, idx_i, std_m, std_m * std_m);
        return true;
    }

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
        if (rem_idx < core_state_size_ || getStateSize() <= core_state_size_) {
            logger->warn("removeAmbiguity: invalid index {} for key {}", rem_idx, key);
            return false;
        }

        removeStateAtIndex(rem_idx);
        ambiguity_indices_.erase(key);

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
            const double tropo_wet  = obs.mf_w * x(getTropoIndex());

            // Receiver clock (+ phase clock delta when enabled)
            double clk_term = x(getClockIndex());
            if (hasPhaseClock())
                clk_term += x(getPhaseClockIndex());

            // Inter-system time offset
            double isb = 0.0;
            {
                const std::string isb_name = systemToIsbName(obs.system);
                const size_t isb_idx = getSystemTimeOffsetIndex(isb_name);
                if (isb_idx < static_cast<size_t>(x.size()))
                    isb = x(static_cast<Eigen::Index>(isb_idx));
            }

            // Tropospheric gradient contribution
            double tropo_grad = 0.0;
            if (hasTropoGradients()) {
                const double el_rad = obs.elevation * Constants::PI / 180.0;
                const double az_rad = obs.azimuth  * Constants::PI / 180.0;
                const double mf_grad = 1.0 / (std::sin(el_rad) * std::tan(el_rad) + 0.0032);
                const double gn = x(static_cast<Eigen::Index>(getTropoGradientNorthIndex()));
                const double ge = x(static_cast<Eigen::Index>(getTropoGradientEastIndex()));
                tropo_grad = mf_grad * (gn * std::cos(az_rad) + ge * std::sin(az_rad));
            }

            // Per-satellite ionosphere (PPP-RTK uncombined, phase advance)
            double iono_term = 0.0;
            if (config_.isUncombinedEnabled() && !ref_type.empty() && ref_type[0] == 'L') {
                const int iono_idx = getIonosphereIndex(obs.satellite);
                if (iono_idx >= 0 && iono_idx < static_cast<int>(x.size())) {
                    double alpha = 1.0;
                    if (ref_type.size() >= 2) {
                        const double freq = bandFrequency(ref_type[1], obs.system);
                        alpha = (Constants::GPS_L1_FREQ * Constants::GPS_L1_FREQ) / (freq * freq);
                    }
                    iono_term = -alpha * x(iono_idx);
                }
            }

            return observed_m - modeled_m - clk_term - tropo_wet - isb - tropo_grad - iono_term;
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

        std::vector<std::string> to_remove;
        to_remove.reserve(ambiguity_indices_.size());
        for (const auto& [key, idx] : ambiguity_indices_)
            if (keep_keys.find(key) == keep_keys.end())
                to_remove.push_back(key);

        if (to_remove.empty()) {
            logger->trace("pruneAmbiguitiesExcept: nothing to remove (keep_keys size={})", keep_keys.size());
            return;
        }

        logger->info("Pruning {} ambiguities (keeping {} keys)", to_remove.size(), keep_keys.size());
        for (const auto& key : to_remove)
            removeAmbiguity(key);
    }

    // ===================================================================
    // Per-satellite ionosphere state management (PPP-RTK uncombined)
    // ===================================================================

    int StateManager::getIonosphereIndex(const std::string& satellite) const {
        auto it = ionosphere_indices_.find(satellite);
        return it != ionosphere_indices_.end() ? static_cast<int>(it->second) : -1;
    }

    const std::map<std::string, size_t>& StateManager::getIonosphereStates() const {
        return ionosphere_indices_;
    }

    size_t StateManager::addIonosphereState(const std::string& satellite, double initial_value) {
        auto logger = log_manager_.getLogger("state_manager");
        if (ionosphere_indices_.count(satellite)) {
            logger->trace("Ionosphere state for {} already exists at index {}", satellite, ionosphere_indices_[satellite]);
            return ionosphere_indices_[satellite];
        }

        // Scale initial variance by obliquity factor 1/sin²(el) when elevation is available
        const double iono_std = config_.getInitialStd().ionosphere;
        double init_var = iono_std * iono_std;
        auto el_it = ionosphere_elevations_.find(satellite);
        if (el_it != ionosphere_elevations_.end() && el_it->second > 0.0) {
            double sin2_el = std::sin(el_it->second * Constants::PI / 180.0);
            sin2_el *= sin2_el;
            if (sin2_el < 0.01) sin2_el = 0.01;
            init_var /= sin2_el;
        }

        const size_t index = appendState(initial_value, init_var);
        ionosphere_indices_[satellite] = index;

        logger->info("Added ionosphere state for {} at index {} with initial value {:.3f} m, init_std={:.1f} m",
            satellite, index, initial_value, std::sqrt(init_var));
        return index;
    }

    void StateManager::pruneIonosphereStatesExcept(const std::set<std::string>& keep_keys) {
        auto logger = log_manager_.getLogger("state_manager");
        if (ionosphere_indices_.empty())
            return;

        std::vector<std::string> to_remove;
        for (const auto& [sat, idx] : ionosphere_indices_)
            if (keep_keys.find(sat) == keep_keys.end())
                to_remove.push_back(sat);
        if (to_remove.empty()) return;

        logger->info("Pruning {} ionosphere states (keeping {} satellites)", to_remove.size(), keep_keys.size());
        for (const auto& sat : to_remove) {
            auto it = ionosphere_indices_.find(sat);
            if (it == ionosphere_indices_.end()) continue;

            const size_t rem_idx = it->second;
            if (rem_idx < core_state_size_ || getStateSize() <= core_state_size_) {
                logger->warn("pruneIonosphereStatesExcept: invalid index {} for {}", rem_idx, sat);
                continue;
            }

            removeStateAtIndex(rem_idx);
            ionosphere_indices_.erase(sat);
            ionosphere_elevations_.erase(sat);

            logger->debug("Removed ionosphere state for {} at former index {}", sat, rem_idx);
        }
    }

    void StateManager::setIonosphereElevation(const std::string& satellite, double elevation_deg) {
        ionosphere_elevations_[satellite] = elevation_deg;
    }

    const std::map<std::string, double>& StateManager::getIonosphereElevations() const {
        return ionosphere_elevations_;
    }

    // ===================================================================
    // Receiver code bias (inter-frequency bias) state management
    // ===================================================================

    int StateManager::getCodeBiasIndex(const std::string& key) const {
        auto it = code_bias_indices_.find(key);
        return it != code_bias_indices_.end() ? static_cast<int>(it->second) : -1;
    }

    const std::map<std::string, size_t>& StateManager::getCodeBiasStates() const {
        return code_bias_indices_;
    }

    size_t StateManager::addCodeBiasState(const std::string& key, double initial_value) {
        auto logger = log_manager_.getLogger("state_manager");
        if (code_bias_indices_.count(key)) {
            logger->trace("Code bias state for {} already exists at index {}", key, code_bias_indices_[key]);
            return code_bias_indices_[key];
        }

        const double cb_std = config_.getInitialStd().code_bias;
        const size_t index = appendState(initial_value, cb_std * cb_std);
        code_bias_indices_[key] = index;

        logger->info("Added code bias state for {} at index {}, initial={:.3f} m, std={:.1f} m",
            key, index, initial_value, cb_std);
        return index;
    }

    void StateManager::pruneCodeBiasStatesExcept(const std::set<std::string>& keep_keys) {
        auto logger = log_manager_.getLogger("state_manager");
        if (code_bias_indices_.empty()) return;

        std::vector<std::string> to_remove;
        for (const auto& [key, idx] : code_bias_indices_)
            if (keep_keys.find(key) == keep_keys.end())
                to_remove.push_back(key);
        if (to_remove.empty()) return;

        logger->info("Pruning {} code bias states (keeping {})", to_remove.size(), keep_keys.size());
        for (const auto& key : to_remove) {
            auto it = code_bias_indices_.find(key);
            if (it == code_bias_indices_.end()) continue;

            const size_t rem_idx = it->second;
            if (rem_idx < core_state_size_ || getStateSize() <= core_state_size_) {
                logger->warn("pruneCodeBiasStatesExcept: invalid index {} for {}", rem_idx, key);
                continue;
            }

            removeStateAtIndex(rem_idx);
            code_bias_indices_.erase(key);

            logger->debug("Removed code bias state for {} at former index {}", key, rem_idx);
        }
    }

} // namespace GnssProcessor