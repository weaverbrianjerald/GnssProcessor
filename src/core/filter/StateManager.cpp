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

#include "filter/StateManager.hpp"
#include "filter/StateKeySchema.hpp"
#include "logging/LogManager.hpp"
#include "common/Constants.hpp"
#include "math/TimeUtils.hpp"
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
            if (band == '2') return Constants::GPS_L2_FREQ;
            if (band == '5') return Constants::GPS_L5_FREQ;
            return Constants::GPS_L1_FREQ;
        case System::QZSS:
            if (band == '2') return Constants::QZSS_L2_FREQ;
            if (band == '5') return Constants::QZSS_L5_FREQ;
            if (band == '6') return Constants::QZSS_L6_FREQ;
            return Constants::QZSS_L1_FREQ;
        case System::SBAS:
            if (band == '5') return Constants::SBAS_L5_FREQ;
            return Constants::SBAS_L1_FREQ;
        case System::Galileo:
            if (band == '5') return Constants::GAL_E5A_FREQ;
            if (band == '7') return Constants::GAL_E5B_FREQ;
            if (band == '8') return Constants::GAL_E5_FREQ;
            if (band == '6') return Constants::GAL_E6_FREQ;
            return Constants::GPS_L1_FREQ;
        case System::BeiDou:
            if (band == '1') return Constants::BDS_B1C_FREQ;
            if (band == '2') return Constants::BDS_B1_FREQ;
            if (band == '5') return Constants::BDS_B2A_FREQ;
            if (band == '7') return Constants::BDS_B2_FREQ;
            if (band == '8') return Constants::BDS_B2AB_FREQ;
            if (band == '6') return Constants::BDS_B3_FREQ;
            return Constants::BDS_B1C_FREQ;
        case System::GLONASS:
            if (band == '2') return Constants::GLONASS_G2_BASE_FREQ;
            if (band == '3') return Constants::GLONASS_G3_BASE_FREQ;
            if (band == '4') return Constants::GLONASS_G1A_FREQ;
            if (band == '6') return Constants::GLONASS_G2A_FREQ;
            return Constants::GLONASS_G1_BASE_FREQ;
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
        for (auto& kv : ambiguity_indices_)      if (kv.second > rem_idx) --kv.second;
        for (auto& kv : code_bias_indices_)      if (kv.second > rem_idx) --kv.second;
        for (auto& kv : sat_bias_indices_)       if (kv.second > rem_idx) --kv.second;
        for (auto& kv : sat_phase_bias_indices_) if (kv.second > rem_idx) --kv.second;
        for (auto& kv : sat_clock_indices_)      if (kv.second > rem_idx) --kv.second;
        for (auto& kv : ionosphere_indices_)     if (kv.second > rem_idx) --kv.second;
    }

    // ===================================================================
    // Construction
    // ===================================================================

    StateManager::StateManager(size_t core_state_size, LogManager& log_manager, const GnssConfig& config)
        : base_core_size_(core_state_size), state_epoch_time_{ 0, 0.0 }, log_manager_(log_manager), config_(config),
          is_baseline_(config.isBaselineMode()),
          clock_idx_(std::numeric_limits<size_t>::max()),
          tropo_idx_(std::numeric_limits<size_t>::max()),
          velocity_idx_(std::numeric_limits<size_t>::max()),
          clock_rate_idx_(std::numeric_limits<size_t>::max()),
          phase_clock_idx_(std::numeric_limits<size_t>::max()),
          gradient_north_idx_(std::numeric_limits<size_t>::max()),
          gradient_east_idx_(std::numeric_limits<size_t>::max()),
          attitude_idx_(std::numeric_limits<size_t>::max()),
          accel_bias_idx_(std::numeric_limits<size_t>::max()),
          gyro_bias_idx_(std::numeric_limits<size_t>::max()),
          differential_tropo_idx_(std::numeric_limits<size_t>::max()) {

        // Baseline mode: strip to position-only core.  DD processing
        // cancels receiver-side clocks/ISBs and (short-baseline) iono/tropo,
        // so none of those states are estimated.  Ambiguities are still
        // added dynamically via addAmbiguity() — the keys are DD-specific
        // ("GPS|G05|L1C|G01") but StateManager doesn't care about format.
        if (is_baseline_) {
            core_state_size_ = base_core_size_;   // just position (3)
            systems_with_time_offsets_.clear();
            system_time_offset_indices_.clear();
            system_time_rate_offset_indices_.clear();

            // Optional differential ZWD state for long-baseline mode.
            // Slotted right after position so the layout stays simple
            // (position 0..2, Δzwd at 3); ambiguities still get appended
            // by addAmbiguity() above this fixed core.
            const bool with_diff_tropo =
                config_.getBaselineConfig().estimate_differential_tropo;
            if (with_diff_tropo) {
                differential_tropo_idx_ = core_state_size_;
                ++core_state_size_;
            }

            state_vector_ = Eigen::VectorXd::Zero(core_state_size_);
            covariance_ = Eigen::MatrixXd::Identity(core_state_size_, core_state_size_);
            const auto& init = config_.getInitialStd();
            const double pos_var = init.position * init.position;
            if (core_state_size_ >= 3) {
                covariance_.block(0, 0, 3, 3) = Eigen::Matrix3d::Identity() * pos_var;
            }
            if (with_diff_tropo) {
                const double dt_std =
                    config_.getBaselineConfig().differential_tropo_init_std_m;
                covariance_(differential_tropo_idx_, differential_tropo_idx_) =
                    dt_std * dt_std;
                log_manager_.getLogger("state_manager")->info(
                    "StateManager initialised in BASELINE mode: core size={}, "
                    "position + differential ZWD (init_std={:.3f} m)",
                    core_state_size_, dt_std);
            } else {
                log_manager_.getLogger("state_manager")->info(
                    "StateManager initialised in BASELINE mode: core size={}, "
                    "position-only (no clock/tropo/iono/system-offset states)",
                    core_state_size_);
            }
            return;
        }

        // Core PPP / PPP-RTK layout:
        //   position(3) → [optional clock(1)] → tropo(1) → ...
        //
        // The receiver clock-bias state is gated on non-BSSD mode.  Under BSSD
        // same-system SD cancels the receiver clock identically, so the bias
        // is unobservable from code/phase measurements; allocating it would
        // only let it drift on process noise without ever receiving a
        // measurement update.  Skipping allocation keeps the BSSD philosophy
        // intact — no receiver-side state survives the SD transform — and
        // shrinks the state vector + covariance by one row/col.
        //
        // The caller-supplied @c core_state_size assumes pos + clk + tropo
        // (=5); we drop one slot when the clock is gated off so the index
        // accounting (velocity, ISBs, etc.) stays self-consistent.
        base_core_size_ = 3;                            // position
        if (!config_.enableBetweenSatelliteSd()) {
            clock_idx_ = base_core_size_;
            ++base_core_size_;
        }
        tropo_idx_ = base_core_size_;
        ++base_core_size_;

        // Optional velocity states (Vx, Vy, Vz) right after position + clock + tropo.
        // The receiver clock-rate state is gated on estimateVelocity AND non-BSSD
        // mode.  Under BSSD, Doppler measurements are SD-transformed alongside
        // code/phase (same-system SD cancels the receiver clock-rate identically),
        // so the clock-rate state is unobservable and unnecessary — skipping
        // allocation keeps the BSSD philosophy intact: no receiver-side state
        // survives the SD transform.  Slotted immediately after velocity so any
        // consumer that locates the clock-rate by inspecting the layout sees
        // position-velocity-clock-rate as a single motion-related block.
        if (config_.estimateVelocity()) {
            velocity_idx_ = base_core_size_;
            base_core_size_ += 3;
            if (!config_.enableBetweenSatelliteSd()) {
                clock_rate_idx_ = base_core_size_;
                base_core_size_ += 1;
            }
        }

        // Optional phase clock offset state.
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

        // Optional IMU error states: attitude(3) + accel_bias(3) + gyro_bias(3) = 9 states
        if (config_.enableImu()) {
            attitude_idx_   = base_core_size_;
            base_core_size_ += 3;
            accel_bias_idx_ = base_core_size_;
            base_core_size_ += 3;
            gyro_bias_idx_  = base_core_size_;
            base_core_size_ += 3;
        }

        // Determine which systems need inter-system time bias states.  GPS is
        // the reference system, so non-GPS active systems would normally each
        // get an ISB state.
        //
        // BSSD gating: same-system BSSD cancels the per-system clock identically
        // (the ISB is just the difference between two cancelled per-system
        // clocks), so under BSSD the ISBs are unobservable from code/phase.
        // Skipping allocation keeps the receiver-side state vector empty
        // post-SD and shrinks the state by one slot per non-GPS active system.
        const auto& active_systems = config_.getActiveSystems();
        systems_with_time_offsets_.clear();
        system_time_offset_indices_.clear();
        system_time_rate_offset_indices_.clear();

        size_t time_offset_idx = base_core_size_;
        if (!config_.enableBetweenSatelliteSd()) {
            for (const auto& sys : active_systems) {
                if (sys != "GPS") {
                    systems_with_time_offsets_.push_back(sys);
                    system_time_offset_indices_[sys] = time_offset_idx;
                    ++time_offset_idx;
                }
            }
        }

        // Per-system clock-rate offset states (ISBR), appended after the
        // existing ISBs.  Gated identically to the global clock-rate state:
        // only allocated when Doppler is being processed AND BSSD is off.
        // Under BSSD same-system SD-Doppler cancels the ISBR identically;
        // skipping allocation keeps the receiver-side states empty after SD.
        if (config_.estimateVelocity() && !config_.enableBetweenSatelliteSd()) {
            for (const auto& sys : systems_with_time_offsets_) {
                system_time_rate_offset_indices_[sys] = time_offset_idx;
                ++time_offset_idx;
            }
        }

        // Total core state size includes base states plus inter-system time offsets
        // (and the matching rate offsets when velocity is estimated).
        core_state_size_ = base_core_size_ + systems_with_time_offsets_.size()
                         + system_time_rate_offset_indices_.size();
        
        state_vector_ = Eigen::VectorXd::Zero(core_state_size_);
        covariance_ = Eigen::MatrixXd::Identity(core_state_size_, core_state_size_);
        
        // Default initial standard deviations
        const auto& init = config_.getInitialStd();
        double pos_var = init.position * init.position;
        double clock_var = init.clock * init.clock;
        double tropo_var = init.troposphere * init.troposphere;
        double time_offset_var = init.system_time_offset * init.system_time_offset;
        
        covariance_.block(0, 0, 3, 3) = Eigen::Matrix3d::Identity() * pos_var;
        if (hasClock())
            covariance_(clock_idx_, clock_idx_) = clock_var;
        covariance_(tropo_idx_, tropo_idx_) = tropo_var;

        // Initialize velocity covariance if enabled
        if (config_.estimateVelocity()) {
            double vel_var = init.velocity * init.velocity;
            covariance_.block(velocity_idx_, velocity_idx_, 3, 3) = Eigen::Matrix3d::Identity() * vel_var;

            // Clock-rate initial variance (only when the state was actually
            // allocated — see gating above; skipped under BSSD where the
            // state is unobservable after SD).  Sized for typical TCXO drift
            // uncertainty at startup (a few m/s in c*df_r/dt is roomy).
            if (hasClockRate()) {
                const double clock_rate_var = init.clock_rate * init.clock_rate;
                covariance_(clock_rate_idx_, clock_rate_idx_) = clock_rate_var;
            }
        }
        
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

        // Initialize IMU bias covariances if enabled
        if (config_.enableImu()) {
            const auto& ic = config_.getImuConfig();
            double att_var = ic.initial_attitude_std * ic.initial_attitude_std;
            covariance_.block(attitude_idx_, attitude_idx_, 3, 3) = Eigen::Matrix3d::Identity() * att_var;
            double ab_var = ic.initial_accel_bias_std * ic.initial_accel_bias_std;
            covariance_.block(accel_bias_idx_, accel_bias_idx_, 3, 3) = Eigen::Matrix3d::Identity() * ab_var;
            double gb_var = ic.initial_gyro_bias_std * ic.initial_gyro_bias_std;
            covariance_.block(gyro_bias_idx_, gyro_bias_idx_, 3, 3) = Eigen::Matrix3d::Identity() * gb_var;
        }
        
        // Initialize inter-system time offset covariances
        for (const auto& [sys_name, idx] : system_time_offset_indices_) {
            covariance_(idx, idx) = time_offset_var;
        }

        // Initialize inter-system clock-rate offset (ISBR) covariances —
        // typically very small (sub-mm/s) but seeded at the configured
        // init.system_time_rate_offset for filter freedom.
        const double time_rate_offset_var =
            init.system_time_rate_offset * init.system_time_rate_offset;
        for (const auto& [sys_name, idx] : system_time_rate_offset_indices_) {
            covariance_(idx, idx) = time_rate_offset_var;
        }
        
        // Log initialization using LogManager-provided logger
        auto logger = log_manager_.getLogger("state_manager");
        logger->info("StateManager initialized with core state size: {} (base={}, clock={}, velocity={}, clock_rate={}, phase_clock={}, gradients={}, imu={}, time_offsets={}, time_rate_offsets={})",
            core_state_size_, base_core_size_,
            hasClock() ? 1 : 0,
            config_.estimateVelocity() ? 3 : 0,
            hasClockRate() ? 1 : 0,
            config_.estimatePhaseClock() ? 1 : 0,
            config_.estimateTropoGradients() ? 2 : 0,
            config_.enableImu() ? 9 : 0,
            systems_with_time_offsets_.size(),
            system_time_rate_offset_indices_.size());
        if (config_.enableBetweenSatelliteSd()) {
            logger->info("BSSD active — receiver clock + ISB states omitted "
                         "(receiver-side states cancel in SD)");
        }
        
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
        return core_state_size_ + code_bias_indices_.size() + sat_bias_indices_.size()
             + sat_phase_bias_indices_.size() + sat_clock_indices_.size()
             + ionosphere_indices_.size() + ambiguity_indices_.size();
    }

    Eigen::VectorXd StateManager::getStateVector() const {
        return state_vector_;
    }

    Eigen::MatrixXd StateManager::getCovariance() const {
        return covariance_;
    }

    double StateManager::getStateVariance(int idx) const {
        if (idx < 0 || idx >= covariance_.rows()) return 0.0;
        return covariance_(idx, idx);
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
        // Config stores spectral density in units of m�/s (variance rate)
        const auto& pn = config_.getProcessNoise();

        size_t posIdx = getPositionIndex();

        // Position / velocity process noise.
        if (hasVelocity()) {
            // Continuous white-noise acceleration (CWNA) model: one
            // acceleration PSD q (m*m/s^3) drives the coupled block.  The
            // same unmodeled acceleration perturbs velocity (single integral)
            // and position (double integral), so the block carries the
            // dt^3/3, dt^2/2, dt structure with a non-zero pos/vel
            // cross-covariance.  pn.position is unused here.
            const size_t velIdx = getVelocityIndex();
            const double q    = pn.acceleration_psd;
            const double dt2  = dt * dt;
            const double q_pp = q * dt2 * dt / 3.0;
            const double q_pv = q * dt2 / 2.0;
            const double q_vv = q * dt;
            Q.block(posIdx, posIdx, 3, 3) = Eigen::Matrix3d::Identity() * q_pp;
            Q.block(posIdx, velIdx, 3, 3) = Eigen::Matrix3d::Identity() * q_pv;
            Q.block(velIdx, posIdx, 3, 3) = Eigen::Matrix3d::Identity() * q_pv;
            Q.block(velIdx, velIdx, 3, 3) = Eigen::Matrix3d::Identity() * q_vv;
        } else {
            // Position-only random walk: pn.position is the spectral density
            // (m*m/s) of the position drift.
            Q.block(posIdx, posIdx, 3, 3) =
                Eigen::Matrix3d::Identity() * (pn.position * dt);
        }

        // Baseline mode: no clock / tropo / iono / system-offset / bias states
        // in the state vector.  Apply ambiguity process noise below (if any),
        // optionally apply random-walk noise to the differential ZWD state,
        // and return.  DD ambiguities are physically constant → pn.ambiguity
        // typically 0, so Q is essentially the position block plus (optionally)
        // a 1×1 Δzwd entry.
        if (is_baseline_) {
            if (state_size > core_state_size_) {
                for (const auto& [key, idx] : ambiguity_indices_) {
                    auto ov = amb_q_overrides_.find(key);
                    const double q = (ov != amb_q_overrides_.end()) ? ov->second : pn.ambiguity;
                    Q(idx, idx) = q * dt;
                }
            }
            if (hasDifferentialTropo()) {
                // Spectral density (m/√s) → variance per second when squared.
                const double sigma =
                    config_.getBaselineConfig().differential_tropo_process_noise_m_per_sqrt_s;
                Q(differential_tropo_idx_, differential_tropo_idx_) =
                    sigma * sigma * dt;
            }
            return Q;
        }

        size_t clockIdx = getClockIndex();
        size_t tropoIdx = getTropoIndex();

        // Velocity noise (m�/s� per s)
        // Velocity process noise is set above, in the CWNA position/velocity block.

        // Clock noise.
        //
        // Two regimes:
        //   (a) Clock-rate state active (Doppler being processed) — apply a
        //       continuous white-noise acceleration (CWNA) model with spectral
        //       density pn.clock_rate (m²/s³).  Clock-bias picks up dt³/3,
        //       clock-rate picks up dt, with dt²/2 cross-covariance.
        //   (b) Clock-rate state inactive — use the legacy random-walk Q on
        //       clock-bias with spectral density pn.clock (m²/s).
        //
        // BSSD handling: when BSSD is active the clock-bias column of H is
        // zeroed for code/phase measurements (clock cancels in SD), so the
        // bias receives no measurement update — suppress its Q to prevent
        // unbounded variance growth.  Doppler is BSSD-exempt, so the
        // clock-rate state still gets a measurement update and must keep
        // its Q regardless (otherwise its variance freezes at 0 and the
        // filter rejects every Doppler).
        if (hasClockRate()) {
            const size_t crIdx = getClockRateIndex();
            const double q_cr  = pn.clock_rate;
            const double dt2   = dt * dt;
            const double q_cc  = q_cr * dt2 * dt / 3.0;
            const double q_x   = q_cr * dt2 / 2.0;
            const double q_rr  = q_cr * dt;
            if (!config_.enableBetweenSatelliteSd()) {
                Q(clockIdx, clockIdx) = q_cc;
                Q(clockIdx, crIdx)    = q_x;
                Q(crIdx, clockIdx)    = q_x;
            }
            Q(crIdx, crIdx) = q_rr;
        } else if (!config_.enableBetweenSatelliteSd()) {
            const double clock_var = pn.clock * dt;
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
        // Scale zenith spectral density by obliquity factor: q_slant = q_zenith / sin�(el)
        if (config_.getProcessingMode() == ProcessingMode::PPP_RTK) {
            const double q_zenith_default = pn.ionosphere;
            for (const auto& [sat, idx] : ionosphere_indices_) {
                // Suppress process noise for BSSD pivot iono states (unobservable).
                if (frozen_pivot_sats_.count(sat)) continue;

                // Per-satellite override from the adaptive iono process-noise
                // scheme (IonoVariabilityMonitor) when present; otherwise the
                // static config zenith spectral density.  Mirrors the
                // amb_q_overrides_ pattern used for ambiguities above.
                auto q_ov = iono_q_overrides_.find(sat);
                const double q_zenith =
                    (q_ov != iono_q_overrides_.end()) ? q_ov->second : q_zenith_default;

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

        // Receiver code bias (IFB) noise (PPP-RTK uncombined).
        // Normally suppressed under BSSD: receiver DCB/IFB columns are zeroed
        // in the SD H-matrix, so those states are unobservable.  The per-FCN-
        // slot GLONASS IFB states are the exception — they survive SD (their
        // columns are NOT zeroed in BssdTransform), so they must keep their
        // random walk even when BSSD is on.
        if (!code_bias_indices_.empty()) {
            const double cb_var = pn.code_bias * dt;
            const bool sd = config_.enableBetweenSatelliteSd();
            for (const auto& [key, idx] : code_bias_indices_) {
                if (sd && !StateKeys::isGlonassPerSlotIfbKey(key)) continue;
                Q(idx, idx) = cb_var;
            }
        }

        // Per-satellite signal bias noise (PPP-RTK uncombined).
        // These absorb uncorrected satellite hardware delays on signals
        // without BIA coverage.  Nearly constant � very small random walk.
        // Suppressed for BSSD pivot satellites (absorbed into eliminated clock).
        if (!sat_bias_indices_.empty()) {
            const double sb_var = pn.sat_bias * dt;
            for (const auto& [key, idx] : sat_bias_indices_) {
                // key format documented in StateKeySchema.hpp;
                // helper preserves the lenient "no pipe -> whole key"
                // fallback the original loop relied on.
                const std::string sat = StateKeys::satelliteFromSatBiasKey(key);
                if (frozen_pivot_sats_.count(sat)) continue;
                Q(idx, idx) = sb_var;
            }
        }

        // Per-satellite signal PHASE bias noise (PPP_RTK_Network producer).
        // Even more stable than the code bias; a tiny random walk keeps the
        // satellite column responsive to genuine slow drift without letting a
        // strictly-constant (q=0) state over-converge.  Suppressed for the
        // S-basis datum (frozen-pivot) satellite, whose phase bias is held at
        // (value=0, variance=0) — applying Q would let it drift off the datum.
        if (!sat_phase_bias_indices_.empty()) {
            const double spb_var = pn.sat_phase_bias * dt;
            for (const auto& [key, idx] : sat_phase_bias_indices_) {
                const std::string sat = StateKeys::satelliteFromSatPhaseBiasKey(key);
                if (frozen_pivot_sats_.count(sat)) continue;
                Q(idx, idx) = spb_var;
            }
        }

        // Per-satellite CLOCK noise (PPP_RTK_Network producer).  Δδtˢ relative to
        // the a-priori product clock; a small random walk lets the estimated
        // clock track genuine drift between epochs while the carried-forward
        // prior provides continuity.  Suppressed for the S-basis datum
        // (frozen-pivot) satellite, whose clock is held at (value=0, variance=0).
        if (!sat_clock_indices_.empty()) {
            const double sc_var = pn.sat_clock * dt;
            for (const auto& [key, idx] : sat_clock_indices_) {
                const std::string sat = StateKeys::satelliteFromSatClockKey(key);
                if (frozen_pivot_sats_.count(sat)) continue;
                Q(idx, idx) = sc_var;
            }
        }

        // Inter-system time offset noise (and rate offset, when present).
        //
        // Mirrors the clock-bias / clock-rate handling above on a per-system
        // basis.  When the ISBR (rate offset) state is present we use a CWNA
        // pair with spectral density pn.system_time_rate_offset; otherwise we
        // fall back to the legacy random walk on the bias with
        // pn.system_time_offset.  Under BSSD the ISB column is zeroed for
        // code/phase (no measurement update on the bias), but Doppler is
        // BSSD-exempt and updates the ISBR, so its Q stays active.
        if (!system_time_rate_offset_indices_.empty()) {
            const double q_isbr = pn.system_time_rate_offset;
            const double dt2    = dt * dt;
            const double q_bb   = q_isbr * dt2 * dt / 3.0;
            const double q_x    = q_isbr * dt2 / 2.0;
            const double q_rr   = q_isbr * dt;
            const bool bssd     = config_.enableBetweenSatelliteSd();
            for (const auto& [sys_name, isb_idx] : system_time_offset_indices_) {
                auto rit = system_time_rate_offset_indices_.find(sys_name);
                if (rit == system_time_rate_offset_indices_.end()) continue;
                const size_t isbr_idx = rit->second;
                if (!bssd) {
                    Q(isb_idx, isb_idx)  = q_bb;
                    Q(isb_idx, isbr_idx) = q_x;
                    Q(isbr_idx, isb_idx) = q_x;
                }
                Q(isbr_idx, isbr_idx) = q_rr;
            }
        } else if (!config_.enableBetweenSatelliteSd()) {
            const double time_offset_var = pn.system_time_offset * dt;
            for (const auto& [sys_name, idx] : system_time_offset_indices_) {
                Q(idx, idx) = time_offset_var;
            }
        }

        // Ambiguity process noise (zero for constant ambiguities).
        // Ambiguities missing BIA/ATX phase coverage absorb a time-constant
        // satellite bias into the float value — no extra process noise needed.
        // They are excluded from integer AR via the unresolvable-ambiguity set.
        // BSSD pivot ambiguities are also frozen (value=0, variance=0 by S-
        // transform construction); applying Q would grow their variance past
        // PivotManager::isPivotDisturbed's 1e-12 threshold every epoch and
        // trigger spurious handovers.
        if (state_size > core_state_size_) {
            for (const auto& [key, idx] : ambiguity_indices_) {
                const auto us = key.find('_');
                if (us != std::string::npos) {
                    const std::string sat = key.substr(0, us);
                    if (frozen_pivot_sats_.count(sat)) continue;
                }
                auto ov = amb_q_overrides_.find(key);
                const double q = (ov != amb_q_overrides_.end()) ? ov->second : pn.ambiguity;
                Q(idx, idx) = q * dt;
            }
        }

        logger->trace("Computed process noise matrix: size={}x{}, dt={:.3f} s, clock_q={:.3e}, bssd={}",
            Q.rows(), Q.cols(), dt, pn.clock, config_.enableBetweenSatelliteSd() ? "on" : "off");
        return Q;
    }

    Eigen::MatrixXd StateManager::getTransitionMatrix(double dt) const {
        size_t state_size = getStateSize();
        Eigen::MatrixXd F = Eigen::MatrixXd::Identity(state_size, state_size);

        // Position-velocity coupling: pos(k+1) = pos(k) + vel(k) * dt
        if (hasVelocity()) {
            F.block(getPositionIndex(), getVelocityIndex(), 3, 3) = Eigen::Matrix3d::Identity() * dt;
        }

        // Clock-bias / clock-rate coupling: δt_r(k+1) = δt_r(k) + dt · ḋt_r(k).
        // Skipped under BSSD because the clock-bias is unobservable from
        // code/phase (SD cancels it) — propagating a value into it via the
        // rate would drift it unboundedly without observability.  Same logic
        // for the per-system ISB / ISBR pairs.
        if (hasClockRate() && !config_.enableBetweenSatelliteSd()) {
            F(getClockIndex(), getClockRateIndex()) = dt;
            for (const auto& [sys_name, isb_idx] : system_time_offset_indices_) {
                auto rit = system_time_rate_offset_indices_.find(sys_name);
                if (rit == system_time_rate_offset_indices_.end()) continue;
                F(isb_idx, rit->second) = dt;
            }
        }

        auto logger = log_manager_.getLogger("state_manager");
        logger->trace("Computed transition matrix: size={}x{}, dt={:.3f} s, velocity_coupling={}, clock_rate_coupling={}",
            F.rows(), F.cols(), dt,
            hasVelocity() ? "on" : "off",
            (hasClockRate() && !config_.enableBetweenSatelliteSd()) ? "on" : "off");
        return F;
    }

    // ===================================================================
    // Index getters
    // ===================================================================

    size_t StateManager::getPositionIndex() const { return 0; }
    size_t StateManager::getVelocityIndex() const { return velocity_idx_; }
    bool   StateManager::hasVelocity() const { return velocity_idx_ != std::numeric_limits<size_t>::max(); }
    size_t StateManager::getClockIndex() const { return clock_idx_; }
    bool   StateManager::hasClock() const { return clock_idx_ != std::numeric_limits<size_t>::max(); }
    size_t StateManager::getClockRateIndex() const { return clock_rate_idx_; }
    bool   StateManager::hasClockRate() const { return clock_rate_idx_ != std::numeric_limits<size_t>::max(); }
    size_t StateManager::getPhaseClockIndex() const { return phase_clock_idx_; }
    bool   StateManager::hasPhaseClock() const { return phase_clock_idx_ != std::numeric_limits<size_t>::max(); }
    size_t StateManager::getTropoIndex() const { return tropo_idx_; }
    size_t StateManager::getDifferentialTropoIndex() const {
        return differential_tropo_idx_;
    }
    bool StateManager::hasDifferentialTropo() const {
        return differential_tropo_idx_ != std::numeric_limits<size_t>::max();
    }

    size_t StateManager::getIonoIndex() const {
        // Deprecated: per-satellite iono states are managed dynamically.
        return std::numeric_limits<size_t>::max();
    }

    size_t StateManager::getTropoGradientNorthIndex() const { return gradient_north_idx_; }
    size_t StateManager::getTropoGradientEastIndex() const { return gradient_east_idx_; }
    bool   StateManager::hasTropoGradients() const { return gradient_north_idx_ != std::numeric_limits<size_t>::max(); }

    bool   StateManager::hasImuBiases() const { return accel_bias_idx_ != std::numeric_limits<size_t>::max(); }
    size_t StateManager::getAccelBiasIndex() const { return accel_bias_idx_; }
    size_t StateManager::getGyroBiasIndex() const { return gyro_bias_idx_; }
    size_t StateManager::getAttitudeIndex() const { return attitude_idx_; }



    size_t StateManager::getSystemTimeOffsetIndex(const std::string& system_name) const {
        auto it = system_time_offset_indices_.find(system_name);
        return it != system_time_offset_indices_.end() ? it->second : std::numeric_limits<size_t>::max();
    }

    size_t StateManager::getSystemTimeRateOffsetIndex(const std::string& system_name) const {
        auto it = system_time_rate_offset_indices_.find(system_name);
        return it != system_time_rate_offset_indices_.end()
             ? it->second : std::numeric_limits<size_t>::max();
    }

    size_t StateManager::getNumSystemTimeOffsets() const {
        return systems_with_time_offsets_.size();
    }

    std::vector<std::string> StateManager::getStateLabels() const {
        const size_t n = getStateSize();
        std::vector<std::string> labels(n);
        for (size_t i = 0; i < n; ++i) labels[i] = "STATE_" + std::to_string(i);

        constexpr size_t kNone = std::numeric_limits<size_t>::max();
        auto put = [&](size_t idx, const std::string& name) {
            if (idx < n) labels[idx] = name;
        };

        // --- Fixed-core block.  Core states carry no stored name strings, so
        //     synthesize labels from the index getters (guarded by their
        //     has*()/sentinel checks; the layout is mode-dependent). ---
        put(0, "POS_X");
        put(1, "POS_Y");
        put(2, "POS_Z");
        if (hasClock())               put(getClockIndex(), "CLK");
        if (getTropoIndex() != kNone) put(getTropoIndex(), "ZWD");
        if (hasVelocity()) {
            put(getVelocityIndex() + 0, "VEL_X");
            put(getVelocityIndex() + 1, "VEL_Y");
            put(getVelocityIndex() + 2, "VEL_Z");
        }
        if (hasClockRate())  put(getClockRateIndex(),  "CLK_RATE");
        if (hasPhaseClock()) put(getPhaseClockIndex(), "PHASE_CLK");
        if (hasTropoGradients()) {
            put(getTropoGradientNorthIndex(), "TGRAD_N");
            put(getTropoGradientEastIndex(),  "TGRAD_E");
        }
        if (hasImuBiases()) {
            for (size_t k = 0; k < 3; ++k) {
                put(getAttitudeIndex()  + k, "ATT_"  + std::to_string(k));
                put(getAccelBiasIndex() + k, "BACC_" + std::to_string(k));
                put(getGyroBiasIndex()  + k, "BGYR_" + std::to_string(k));
            }
        }
        if (hasDifferentialTropo()) put(getDifferentialTropoIndex(), "DZWD");

        // Inter-system time / rate offsets (ISB / ISBR), keyed by system name.
        for (const auto& sys : systems_with_time_offsets_) {
            const size_t oi = getSystemTimeOffsetIndex(sys);
            if (oi != kNone) put(oi, "ISB_" + sys);
            const size_t ri = getSystemTimeRateOffsetIndex(sys);
            if (ri != kNone) put(ri, "ISBR_" + sys);
        }

        // --- Dynamic key->index maps.  The keys ARE the stable labels
        //     (StateKeySchema); prefix a category tag so the disjoint maps
        //     never alias (e.g. iono "G01" vs sat-bias prefix "G01|C1W"). ---
        for (const auto& kv : getAmbiguities())        put(kv.second, "AMB:"   + kv.first);
        for (const auto& kv : getIonosphereStates())   put(kv.second, "IONO:"  + kv.first);
        for (const auto& kv : getCodeBiasStates())     put(kv.second, "CBIAS:" + kv.first);
        for (const auto& kv : getSatBiasStates())      put(kv.second, "SATB:"  + kv.first);
        for (const auto& kv : getSatPhaseBiasStates()) put(kv.second, "SATPB:" + kv.first);
        for (const auto& kv : getSatClockStates())     put(kv.second, "SATCLK:" + kv.first);

        return labels;
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

    size_t StateManager::addAmbiguity(const std::string& key, double initial_value,
                                        double initial_std_override) {
        auto logger = log_manager_.getLogger("state_manager");
        if (ambiguity_indices_.count(key)) {
            logger->trace("Ambiguity {} already exists at index {}", key, ambiguity_indices_[key]);
            return ambiguity_indices_[key];
        }
        const double amb_std = (initial_std_override > 0.0)
                                ? initial_std_override
                                : config_.getAmbiguityStd();
        const size_t index = appendState(initial_value, amb_std * amb_std);
        ambiguity_indices_[key] = index;

        logger->info("Added ambiguity {} at index {} with initial value {:.3f}, std={:.3f} m{}",
                     key, index, initial_value, amb_std,
                     initial_std_override > 0.0 ? " (override)" : "");
        return index;
    }

    // ===================================================================
    // PppState synchronization
    // ===================================================================

    void StateManager::updatePppStateFromVector(const Eigen::VectorXd& current_state, PppState& ppp_state, const StateManager& state_manager) const {
        ppp_state.position         = current_state.segment<3>(state_manager.getPositionIndex());
        ppp_state.clock_bias       = state_manager.hasClock()
                                     ? current_state(state_manager.getClockIndex())
                                     : 0.0;
        ppp_state.troposphere_zwd  = current_state(state_manager.getTropoIndex());

        if (state_manager.hasVelocity())
            ppp_state.velocity = current_state.segment<3>(state_manager.getVelocityIndex());

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
        if (hasClock())
            sv(getClockIndex()) = ppp_state.clock_bias;
        sv(getTropoIndex()) = ppp_state.troposphere_zwd;

        if (hasVelocity())
            sv.segment<3>(getVelocityIndex()) = ppp_state.velocity;

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

    bool StateManager::noteSsrDiscontinuity(const std::string& key, int counter) {
        auto it = last_ssr_discontinuity_.find(key);
        if (it == last_ssr_discontinuity_.end()) {
            last_ssr_discontinuity_.emplace(key, counter);   // first sighting: record only
            return false;
        }
        if (it->second != counter) {                         // changed (any direction) -> bump
            it->second = counter;
            return true;
        }
        return false;
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

        const int idx_i = getAmbiguityIndex(key);
        if (idx_i < 0) {
            // Refuse to auto-create the state for an unknown key.
            //
            // Slip detectors iterate @c obs.raw_observed (which preserves
            // every RINEX-tracked signal) but the state vector only holds
            // ambiguities that pass the @c signal_mode band filter
            // (typically L1/L2 for Dual, +L5 for Triple, etc.).  Without
            // this guard, a slip detector reporting a reset for L5X on a
            // Dual-mode run would have @c addAmbiguity called here,
            // sneaking a band-filtered signal into the state vector.
            //
            // The "create-on-reset" semantic was originally intended for
            // first-epoch state creation, but @c ensureRequiredStates now
            // owns that responsibility and seeds new ambiguities with a
            // proper geometry-derived value.  Slip-detector reset keys
            // for non-existent ambiguities are no-ops here.
            logger->debug("resetAmbiguity ignoring unknown key {} "
                          "(slip detector referenced signal not in current state — "
                          "likely band-filtered out by signal_mode)", key);
            return false;
        }

        const auto idx = static_cast<Eigen::Index>(idx_i);
        if (idx >= state_vector_.size() || idx >= covariance_.rows()) {
            logger->error("resetAmbiguity index out of bounds for key {}: idx={}, state_size={}, cov={}x{}",
                key, idx_i, state_vector_.size(), covariance_.rows(), covariance_.cols());
            return false;
        }

        // Keep the current value � it will be overwritten by a proper
        // geometry-based seed in ensureRequiredStates().
        covariance_.row(idx).setZero();
        covariance_.col(idx).setZero();
        covariance_(idx, idx) = std_m * std_m;

        pending_reseed_ambiguities_.insert(key);

        logger->info("Reset ambiguity {} at index {}: marked pending reseed, std={:.3f} m (var={:.3e})",
            key, idx_i, std_m, std_m * std_m);
        return true;
    }

    bool StateManager::renameAmbiguity(const std::string& old_key, const std::string& new_key) {
        auto logger = log_manager_.getLogger("state_manager");
        if (old_key.empty() || new_key.empty()) {
            logger->warn("renameAmbiguity called with empty key (old='{}', new='{}')", old_key, new_key);
            return false;
        }
        if (old_key == new_key) return true;  // no-op
        auto it = ambiguity_indices_.find(old_key);
        if (it == ambiguity_indices_.end()) {
            logger->trace("renameAmbiguity: old_key '{}' not found", old_key);
            return false;
        }
        if (ambiguity_indices_.count(new_key)) {
            logger->warn("renameAmbiguity: new_key '{}' already exists (old='{}')", new_key, old_key);
            return false;
        }
        const size_t idx = it->second;
        ambiguity_indices_.erase(it);
        ambiguity_indices_[new_key] = idx;
        logger->trace("Renamed ambiguity '{}' -> '{}' at index {}", old_key, new_key, idx);
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

            // Receiver clock (+ phase clock delta when enabled).  Under BSSD
            // the clock cancels in SD and is not in the state vector — the
            // ambiguity seed sees a 0 clock term, which is exactly what the
            // SD residual expects.
            double clk_term = hasClock() ? x(getClockIndex()) : 0.0;
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

            // Per-satellite signal bias � code-only estimation.
            // Phase ambiguities absorb satellite phase biases, so no
            // sat-bias state exists for L signals and the term is zero.
            double sat_bias_term = 0.0;

            return observed_m - modeled_m - clk_term - tropo_wet - isb - tropo_grad - iono_term - sat_bias_term;
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

    size_t StateManager::addIonosphereState(const std::string& satellite, double initial_value,
                                             double std_scale) {
        auto logger = log_manager_.getLogger("state_manager");
        if (ionosphere_indices_.count(satellite)) {
            logger->trace("Ionosphere state for {} already exists at index {}", satellite, ionosphere_indices_[satellite]);
            return ionosphere_indices_[satellite];
        }

        // Scale initial variance by obliquity factor 1/sin²(el) when elevation is available.
        // std_scale applies on top to inflate uncertainty when the seed value is unreliable.
        const double iono_std = config_.getInitialStd().ionosphere * std_scale;
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
            ionosphere_azimuths_.erase(sat);

            logger->debug("Removed ionosphere state for {} at former index {}", sat, rem_idx);
        }
    }

    void StateManager::reinflateIonosphereVariance(const std::string& satellite,
                                                    double new_variance) {
        auto logger = log_manager_.getLogger("state_manager");

        const auto it = ionosphere_indices_.find(satellite);
        if (it == ionosphere_indices_.end()) {
            logger->debug("reinflateIonosphereVariance: no iono state for {}", satellite);
            return;
        }
        const auto idx = static_cast<Eigen::Index>(it->second);
        if (idx >= state_vector_.size() || idx >= covariance_.rows()) {
            logger->error("reinflateIonosphereVariance index out of bounds for {}: idx={}, state_size={}, cov={}x{}",
                satellite, idx, state_vector_.size(), covariance_.rows(), covariance_.cols());
            return;
        }

        // Refuse to disturb a frozen pivot row — the BSSD pivot's
        // iono state is held at (0, 0) by PivotManager and must not
        // be re-inflated.  Caller (CycleSlipManager / Application)
        // should already skip pivots, but the guard prevents subtle
        // BSSD-frame corruption if a future call site forgets.
        const double current_diag = covariance_(idx, idx);
        if (!(current_diag > 0.0)) {
            logger->warn("reinflateIonosphereVariance refused on frozen pivot {} "
                          "(idx={}, P_diag={:.3e}); BSSD invariant preserved",
                          satellite, idx, current_diag);
            return;
        }

        if (!(new_variance > 0.0)) {
            const double io_std = config_.getInitialStd().ionosphere;
            logger->warn("reinflateIonosphereVariance: non-positive variance {} for {}; "
                          "falling back to initial_std.ionosphere² = {}",
                          new_variance, satellite, io_std * io_std);
            new_variance = io_std * io_std;
        }

        const double prev_value = state_vector_(idx);
        const double prev_diag  = current_diag;

        covariance_.row(idx).setZero();
        covariance_.col(idx).setZero();
        covariance_(idx, idx) = new_variance;
        // Value intentionally preserved: slant iono doesn't change
        // due to the receiver losing phase lock — only the joint
        // (iono, sat-bias) covariance partition needs to be broken.

        logger->info("Reinflated iono state for {} at index {}: "
                      "value={:+.4f} m (unchanged), std {:.4f} -> {:.4f} m",
                      satellite, idx, prev_value,
                      std::sqrt(std::max(prev_diag, 0.0)),
                      std::sqrt(new_variance));
    }

    void StateManager::setIonosphereElevation(const std::string& satellite, double elevation_deg) {
        ionosphere_elevations_[satellite] = elevation_deg;
    }

    const std::map<std::string, double>& StateManager::getIonosphereElevations() const {
        return ionosphere_elevations_;
    }

    void StateManager::setIonosphereAzimuth(const std::string& satellite, double azimuth_deg) {
        ionosphere_azimuths_[satellite] = azimuth_deg;
    }

    const std::map<std::string, double>& StateManager::getIonosphereAzimuths() const {
        return ionosphere_azimuths_;
    }

    void StateManager::setFrozenPivotSatellites(const std::set<std::string>& sats) {
        frozen_pivot_sats_ = sats;
    }

    void StateManager::setAmbiguityProcessNoiseOverrides(
        const std::map<std::string, double>& overrides) {
        amb_q_overrides_ = overrides;
    }

    void StateManager::setIonoProcessNoiseOverrides(
        const std::map<std::string, double>& overrides) {
        iono_q_overrides_ = overrides;
    }

    const std::set<std::string>& StateManager::getFrozenPivotSatellites() const {
        return frozen_pivot_sats_;
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

    // ===================================================================
    // Per-satellite signal bias state management
    // ===================================================================

    int StateManager::getSatBiasIndex(const std::string& key) const {
        auto it = sat_bias_indices_.find(key);
        return it != sat_bias_indices_.end() ? static_cast<int>(it->second) : -1;
    }

    const std::map<std::string, size_t>& StateManager::getSatBiasStates() const {
        return sat_bias_indices_;
    }

    size_t StateManager::addSatBiasState(const std::string& key, double initial_value) {
        const double sb_std = config_.getInitialStd().sat_bias;
        return addSatBiasStateWithVariance(key, initial_value, sb_std * sb_std);
    }

    size_t StateManager::addSatBiasStateWithVariance(const std::string& key,
                                                     double initial_value,
                                                     double initial_variance) {
        auto logger = log_manager_.getLogger("state_manager");
        if (sat_bias_indices_.count(key)) {
            logger->trace("Sat bias state for {} already exists at index {}", key, sat_bias_indices_[key]);
            return sat_bias_indices_[key];
        }
        if (!(initial_variance > 0.0)) {
            const double sb_std = config_.getInitialStd().sat_bias;
            logger->warn("addSatBiasStateWithVariance: non-positive variance {} for {}; "
                         "falling back to initial_std.sat_bias² = {}",
                         initial_variance, key, sb_std * sb_std);
            initial_variance = sb_std * sb_std;
        }

        const size_t index = appendState(initial_value, initial_variance);
        sat_bias_indices_[key] = index;

        logger->info("Added sat bias state for {} at index {}, initial={:.3f} m, std={:.3f} m",
            key, index, initial_value, std::sqrt(initial_variance));
        return index;
    }

    void StateManager::resetSatBiasState(const std::string& key,
                                          double new_value,
                                          double new_variance) {
        auto logger = log_manager_.getLogger("state_manager");

        const auto it = sat_bias_indices_.find(key);
        if (it == sat_bias_indices_.end()) {
            logger->debug("resetSatBiasState: unknown key {}", key);
            return;
        }
        const auto idx = static_cast<Eigen::Index>(it->second);
        if (idx >= state_vector_.size() || idx >= covariance_.rows()) {
            logger->error("resetSatBiasState index out of bounds for key {}: idx={}, state_size={}, cov={}x{}",
                key, idx, state_vector_.size(), covariance_.rows(), covariance_.cols());
            return;
        }

        // Defensive: refuse to disturb a frozen pivot row.  The BSSD
        // pivot's sat-bias is held at (value=0, variance=0) by
        // PivotManager — re-inflating its variance here would let it
        // drift away from 0 on the next update and break the BSSD
        // frame for every other sat in the constellation/band group.
        // Callers should already screen pivots via
        // getFrozenPivotSatellites(), but a hard guard here prevents
        // subtle frame corruption if a future call site forgets.
        const double current_diag = covariance_(idx, idx);
        if (!(current_diag > 0.0)) {
            logger->warn("resetSatBiasState refused on frozen pivot row {} "
                          "(idx={}, P_diag={:.3e}); BSSD invariant preserved",
                          key, idx, current_diag);
            return;
        }

        if (!(new_variance > 0.0)) {
            const double sb_std = config_.getInitialStd().sat_bias;
            logger->warn("resetSatBiasState: non-positive variance {} for {}; "
                          "falling back to initial_std.sat_bias² = {}",
                          new_variance, key, sb_std * sb_std);
            new_variance = sb_std * sb_std;
        }

        const double prev_value = state_vector_(idx);
        const double prev_diag  = current_diag;

        // Same surgery as resetAmbiguity: decorrelate (zero row/col)
        // and seed a fresh diagonal.  Off-diagonals are wiped because
        // the prior covariance was learned under the assumption that
        // the bias state was correct; that assumption no longer
        // holds, so its cross-correlations to position / clock /
        // other biases are now misleading.
        covariance_.row(idx).setZero();
        covariance_.col(idx).setZero();
        covariance_(idx, idx) = new_variance;
        state_vector_(idx)    = new_value;

        logger->info("Reset sat bias state {} at index {}: "
                      "value {:+.4f} -> {:+.4f} m, std {:.4f} -> {:.4f} m",
                      key, idx,
                      prev_value, new_value,
                      std::sqrt(std::max(prev_diag, 0.0)),
                      std::sqrt(new_variance));
    }

    void StateManager::reinflateSatBiasVariancesForSat(const std::string& satellite,
                                                         double new_variance) {
        auto logger = log_manager_.getLogger("state_manager");

        if (sat_bias_indices_.empty()) return;

        if (!(new_variance > 0.0)) {
            const double sb_std = config_.getInitialStd().sat_bias;
            logger->warn("reinflateSatBiasVariancesForSat: non-positive variance {} for {}; "
                          "falling back to initial_std.sat_bias² = {}",
                          new_variance, satellite, sb_std * sb_std);
            new_variance = sb_std * sb_std;
        }

        // Walk the key map; the keys are "satellite|signal" so we
        // can prefix-match without parsing.  Skipping pivots is
        // delegated to the per-state diag==0 guard below — the same
        // mechanism resetSatBiasState uses.
        const std::string prefix = satellite + "|";
        std::vector<std::pair<std::string, Eigen::Index>> touched;
        std::vector<std::string> frozen;
        for (const auto& [key, raw_idx] : sat_bias_indices_) {
            if (key.compare(0, prefix.size(), prefix) != 0) continue;
            const auto idx = static_cast<Eigen::Index>(raw_idx);
            if (idx >= state_vector_.size() || idx >= covariance_.rows()) {
                logger->error("reinflateSatBiasVariancesForSat: idx {} out of bounds for {}",
                               idx, key);
                continue;
            }
            const double current_diag = covariance_(idx, idx);
            if (!(current_diag > 0.0)) {
                // Frozen pivot row — leave it alone (the BSSD pivot
                // bias is held at (0, 0); reinflating would let it
                // drift on the next update and break the BSSD frame
                // for the whole constellation/band group).
                frozen.push_back(key);
                continue;
            }
            covariance_.row(idx).setZero();
            covariance_.col(idx).setZero();
            covariance_(idx, idx) = new_variance;
            // Value preserved — hardware delay is constant.
            touched.emplace_back(key, idx);
        }

        if (!touched.empty()) {
            std::string joined;
            for (size_t i = 0; i < touched.size(); ++i) {
                if (i) joined += ", ";
                joined += touched[i].first;
            }
            logger->info("Reinflated {} sat-bias state(s) for {} to std={:.4f} m: {}",
                          touched.size(), satellite,
                          std::sqrt(new_variance), joined);
        }
        if (!frozen.empty()) {
            logger->debug("reinflateSatBiasVariancesForSat: skipped {} frozen pivot row(s) for {}",
                           frozen.size(), satellite);
        }
    }

    void StateManager::pruneSatBiasStatesExcept(const std::set<std::string>& keep_keys) {
        auto logger = log_manager_.getLogger("state_manager");
        // Always run an eviction sweep so cache entries don't accumulate
        // indefinitely on satellites that never re-acquire.  Cheap when
        // the cache is small and a no-op when it's empty.
        evictExpiredSatBiasCache(state_epoch_time_);

        if (sat_bias_indices_.empty()) return;

        std::vector<std::string> to_remove;
        for (const auto& [key, idx] : sat_bias_indices_)
            if (keep_keys.find(key) == keep_keys.end())
                to_remove.push_back(key);
        if (to_remove.empty()) return;

        logger->info("Pruning {} sat bias states (keeping {}; cache holds {} stale entries pre-prune)",
            to_remove.size(), keep_keys.size(), sat_bias_cache_.size());
        for (const auto& key : to_remove) {
            auto it = sat_bias_indices_.find(key);
            if (it == sat_bias_indices_.end()) continue;

            const size_t rem_idx = it->second;
            if (rem_idx < core_state_size_ || getStateSize() <= core_state_size_) {
                logger->warn("pruneSatBiasStatesExcept: invalid index {} for {}", rem_idx, key);
                continue;
            }

            // Stash the value + diagonal variance before removal so a
            // near-term re-acquisition can warm-restart instead of
            // paying the cold-start re-seed cost.  Overwrites any
            // pre-existing cache entry for the same key (most recent
            // drop wins — older estimate would have been worse anyway).
            const double cached_value    = state_vector_(static_cast<Eigen::Index>(rem_idx));
            const double cached_variance = covariance_(static_cast<Eigen::Index>(rem_idx),
                                                       static_cast<Eigen::Index>(rem_idx));
            sat_bias_cache_[key] = SatBiasCacheEntry{
                cached_value, cached_variance, state_epoch_time_
            };

            removeStateAtIndex(rem_idx);
            sat_bias_indices_.erase(key);

            logger->debug("Removed sat bias state for {} at former index {} "
                          "(cached value={:+.4f} m, std={:.4f} m for potential restore)",
                          key, rem_idx, cached_value, std::sqrt(std::max(cached_variance, 0.0)));
        }
    }

    // ===================================================================
    // Satellite PHASE-bias states (PPP_RTK_Network producer)
    // -------------------------------------------------------------------
    // Mirror of the satellite CODE-bias block above, on the separate
    // sat_phase_bias_indices_ map.  The warm-restart cache the code-bias
    // block carries is intentionally NOT mirrored yet (deferred to W4 with
    // the network estimator's restart semantics); prune simply removes.
    // ===================================================================

    int StateManager::getSatPhaseBiasIndex(const std::string& key) const {
        auto it = sat_phase_bias_indices_.find(key);
        return it != sat_phase_bias_indices_.end() ? static_cast<int>(it->second) : -1;
    }

    const std::map<std::string, size_t>& StateManager::getSatPhaseBiasStates() const {
        return sat_phase_bias_indices_;
    }

    size_t StateManager::addSatPhaseBiasState(const std::string& key, double initial_value) {
        const double spb_std = config_.getInitialStd().sat_phase_bias;
        return addSatPhaseBiasStateWithVariance(key, initial_value, spb_std * spb_std);
    }

    size_t StateManager::addSatPhaseBiasStateWithVariance(const std::string& key,
                                                          double initial_value,
                                                          double initial_variance) {
        auto logger = log_manager_.getLogger("state_manager");
        if (sat_phase_bias_indices_.count(key)) {
            logger->trace("Sat phase-bias state for {} already exists at index {}",
                          key, sat_phase_bias_indices_[key]);
            return sat_phase_bias_indices_[key];
        }
        if (!(initial_variance > 0.0)) {
            const double spb_std = config_.getInitialStd().sat_phase_bias;
            logger->warn("addSatPhaseBiasStateWithVariance: non-positive variance {} for {}; "
                         "falling back to initial_std.sat_phase_bias^2 = {}",
                         initial_variance, key, spb_std * spb_std);
            initial_variance = spb_std * spb_std;
        }

        const size_t index = appendState(initial_value, initial_variance);
        sat_phase_bias_indices_[key] = index;

        logger->info("Added sat phase-bias state for {} at index {}, initial={:.3f} m, std={:.3f} m",
            key, index, initial_value, std::sqrt(initial_variance));
        return index;
    }

    void StateManager::resetSatPhaseBiasState(const std::string& key,
                                              double new_value,
                                              double new_variance) {
        auto logger = log_manager_.getLogger("state_manager");

        const auto it = sat_phase_bias_indices_.find(key);
        if (it == sat_phase_bias_indices_.end()) {
            logger->debug("resetSatPhaseBiasState: unknown key {}", key);
            return;
        }
        const auto idx = static_cast<Eigen::Index>(it->second);
        if (idx >= state_vector_.size() || idx >= covariance_.rows()) {
            logger->error("resetSatPhaseBiasState index out of bounds for key {}: idx={}, state_size={}, cov={}x{}",
                key, idx, state_vector_.size(), covariance_.rows(), covariance_.cols());
            return;
        }

        // Refuse to disturb a frozen-pivot row (held at value=0, variance=0 by
        // the S-basis datum) — re-inflating it would let the datum drift on the
        // next update and break the frame for the constellation/band group.
        const double current_diag = covariance_(idx, idx);
        if (!(current_diag > 0.0)) {
            logger->warn("resetSatPhaseBiasState refused on frozen pivot row {} "
                          "(idx={}, P_diag={:.3e}); S-basis datum preserved",
                          key, idx, current_diag);
            return;
        }

        if (!(new_variance > 0.0)) {
            const double spb_std = config_.getInitialStd().sat_phase_bias;
            logger->warn("resetSatPhaseBiasState: non-positive variance {} for {}; "
                          "falling back to initial_std.sat_phase_bias^2 = {}",
                          new_variance, key, spb_std * spb_std);
            new_variance = spb_std * spb_std;
        }

        const double prev_value = state_vector_(idx);
        const double prev_diag  = current_diag;

        covariance_.row(idx).setZero();
        covariance_.col(idx).setZero();
        covariance_(idx, idx) = new_variance;
        state_vector_(idx)    = new_value;

        logger->info("Reset sat phase-bias state {} at index {}: "
                      "value {:+.4f} -> {:+.4f} m, std {:.4f} -> {:.4f} m",
                      key, idx,
                      prev_value, new_value,
                      std::sqrt(std::max(prev_diag, 0.0)),
                      std::sqrt(new_variance));
    }

    void StateManager::reinflateSatPhaseBiasVariancesForSat(const std::string& satellite,
                                                            double new_variance) {
        auto logger = log_manager_.getLogger("state_manager");

        if (sat_phase_bias_indices_.empty()) return;

        if (!(new_variance > 0.0)) {
            const double spb_std = config_.getInitialStd().sat_phase_bias;
            logger->warn("reinflateSatPhaseBiasVariancesForSat: non-positive variance {} for {}; "
                          "falling back to initial_std.sat_phase_bias^2 = {}",
                          new_variance, satellite, spb_std * spb_std);
            new_variance = spb_std * spb_std;
        }

        // Keys are "<sat>|<signal>|PB"; prefix-match on "<sat>|" without parsing.
        const std::string prefix = satellite + "|";
        std::vector<std::pair<std::string, Eigen::Index>> touched;
        std::vector<std::string> frozen;
        for (const auto& [key, raw_idx] : sat_phase_bias_indices_) {
            if (key.compare(0, prefix.size(), prefix) != 0) continue;
            const auto idx = static_cast<Eigen::Index>(raw_idx);
            if (idx >= state_vector_.size() || idx >= covariance_.rows()) {
                logger->error("reinflateSatPhaseBiasVariancesForSat: idx {} out of bounds for {}",
                               idx, key);
                continue;
            }
            const double current_diag = covariance_(idx, idx);
            if (!(current_diag > 0.0)) {
                frozen.push_back(key);   // frozen pivot row — leave the datum alone
                continue;
            }
            covariance_.row(idx).setZero();
            covariance_.col(idx).setZero();
            covariance_(idx, idx) = new_variance;
            // Value preserved — phase hardware delay is constant.
            touched.emplace_back(key, idx);
        }

        if (!touched.empty()) {
            std::string joined;
            for (size_t i = 0; i < touched.size(); ++i) {
                if (i) joined += ", ";
                joined += touched[i].first;
            }
            logger->info("Reinflated {} sat-phase-bias state(s) for {} to std={:.4f} m: {}",
                          touched.size(), satellite,
                          std::sqrt(new_variance), joined);
        }
        if (!frozen.empty()) {
            logger->debug("reinflateSatPhaseBiasVariancesForSat: skipped {} frozen pivot row(s) for {}",
                           frozen.size(), satellite);
        }
    }

    void StateManager::pruneSatPhaseBiasStatesExcept(const std::set<std::string>& keep_keys) {
        auto logger = log_manager_.getLogger("state_manager");
        if (sat_phase_bias_indices_.empty()) return;

        std::vector<std::string> to_remove;
        for (const auto& [key, idx] : sat_phase_bias_indices_)
            if (keep_keys.find(key) == keep_keys.end())
                to_remove.push_back(key);
        if (to_remove.empty()) return;

        logger->info("Pruning {} sat phase-bias states (keeping {})",
            to_remove.size(), keep_keys.size());
        for (const auto& key : to_remove) {
            auto it = sat_phase_bias_indices_.find(key);
            if (it == sat_phase_bias_indices_.end()) continue;

            const size_t rem_idx = it->second;
            if (rem_idx < core_state_size_ || getStateSize() <= core_state_size_) {
                logger->warn("pruneSatPhaseBiasStatesExcept: invalid index {} for {}", rem_idx, key);
                continue;
            }

            // removeStateAtIndex decrements all higher indices across every
            // dynamic map (incl. sat_phase_bias_indices_), so the next key's
            // index is re-read fresh from the map on the following iteration.
            removeStateAtIndex(rem_idx);
            sat_phase_bias_indices_.erase(key);

            logger->debug("Removed sat phase-bias state for {} at former index {}", key, rem_idx);
        }
    }

    // ===================================================================
    // Satellite CLOCK states (PPP_RTK_Network producer) — mirror the
    // satellite phase-bias block above, keyed "<sat>|SC", PER-SATELLITE.
    // ===================================================================

    int StateManager::getSatClockIndex(const std::string& key) const {
        auto it = sat_clock_indices_.find(key);
        return it != sat_clock_indices_.end() ? static_cast<int>(it->second) : -1;
    }

    const std::map<std::string, size_t>& StateManager::getSatClockStates() const {
        return sat_clock_indices_;
    }

    size_t StateManager::addSatClockState(const std::string& key, double initial_value) {
        const double sc_std = config_.getInitialStd().sat_clock;
        return addSatClockStateWithVariance(key, initial_value, sc_std * sc_std);
    }

    size_t StateManager::addSatClockStateWithVariance(const std::string& key,
                                                      double initial_value,
                                                      double initial_variance) {
        auto logger = log_manager_.getLogger("state_manager");
        if (sat_clock_indices_.count(key)) {
            logger->trace("Sat clock state for {} already exists at index {}",
                          key, sat_clock_indices_[key]);
            return sat_clock_indices_[key];
        }
        if (!(initial_variance > 0.0)) {
            const double sc_std = config_.getInitialStd().sat_clock;
            logger->warn("addSatClockStateWithVariance: non-positive variance {} for {}; "
                         "falling back to initial_std.sat_clock^2 = {}",
                         initial_variance, key, sc_std * sc_std);
            initial_variance = sc_std * sc_std;
        }

        const size_t index = appendState(initial_value, initial_variance);
        sat_clock_indices_[key] = index;

        logger->info("Added sat clock state for {} at index {}, initial={:.3f} m, std={:.3f} m",
            key, index, initial_value, std::sqrt(initial_variance));
        return index;
    }

    void StateManager::resetSatClockState(const std::string& key,
                                          double new_value,
                                          double new_variance) {
        auto logger = log_manager_.getLogger("state_manager");

        const auto it = sat_clock_indices_.find(key);
        if (it == sat_clock_indices_.end()) {
            logger->debug("resetSatClockState: unknown key {}", key);
            return;
        }
        const auto idx = static_cast<Eigen::Index>(it->second);
        if (idx >= state_vector_.size() || idx >= covariance_.rows()) {
            logger->error("resetSatClockState index out of bounds for key {}: idx={}, state_size={}, cov={}x{}",
                key, idx, state_vector_.size(), covariance_.rows(), covariance_.cols());
            return;
        }

        // Refuse to disturb a frozen-pivot row (held at value=0, variance=0 by
        // the S-basis datum) — re-inflating it would let the datum drift.
        const double current_diag = covariance_(idx, idx);
        if (!(current_diag > 0.0)) {
            logger->warn("resetSatClockState refused on frozen pivot row {} "
                          "(idx={}, P_diag={:.3e}); S-basis datum preserved",
                          key, idx, current_diag);
            return;
        }

        if (!(new_variance > 0.0)) {
            const double sc_std = config_.getInitialStd().sat_clock;
            logger->warn("resetSatClockState: non-positive variance {} for {}; "
                          "falling back to initial_std.sat_clock^2 = {}",
                          new_variance, key, sc_std * sc_std);
            new_variance = sc_std * sc_std;
        }

        const double prev_value = state_vector_(idx);
        const double prev_diag  = current_diag;

        covariance_.row(idx).setZero();
        covariance_.col(idx).setZero();
        covariance_(idx, idx) = new_variance;
        state_vector_(idx)    = new_value;

        logger->info("Reset sat clock state {} at index {}: "
                      "value {:+.4f} -> {:+.4f} m, std {:.4f} -> {:.4f} m",
                      key, idx,
                      prev_value, new_value,
                      std::sqrt(std::max(prev_diag, 0.0)),
                      std::sqrt(new_variance));
    }

    void StateManager::reinflateSatClockVarianceForSat(const std::string& satellite,
                                                       double new_variance) {
        auto logger = log_manager_.getLogger("state_manager");
        if (sat_clock_indices_.empty()) return;

        const std::string key = StateKeys::makeSatClockKey(satellite);
        const auto it = sat_clock_indices_.find(key);
        if (it == sat_clock_indices_.end()) return;

        if (!(new_variance > 0.0)) {
            const double sc_std = config_.getInitialStd().sat_clock;
            logger->warn("reinflateSatClockVarianceForSat: non-positive variance {} for {}; "
                          "falling back to initial_std.sat_clock^2 = {}",
                          new_variance, satellite, sc_std * sc_std);
            new_variance = sc_std * sc_std;
        }

        const auto idx = static_cast<Eigen::Index>(it->second);
        if (idx >= state_vector_.size() || idx >= covariance_.rows()) {
            logger->error("reinflateSatClockVarianceForSat: idx {} out of bounds for {}", idx, key);
            return;
        }
        const double current_diag = covariance_(idx, idx);
        if (!(current_diag > 0.0)) {   // frozen pivot row — leave the datum alone
            logger->debug("reinflateSatClockVarianceForSat: skipped frozen pivot row {}", key);
            return;
        }
        covariance_.row(idx).setZero();
        covariance_.col(idx).setZero();
        covariance_(idx, idx) = new_variance;
        // Value preserved — the clock estimate continuity is intentional.
        logger->info("Reinflated sat-clock state {} to std={:.4f} m", key, std::sqrt(new_variance));
    }

    void StateManager::pruneSatClockStatesExcept(const std::set<std::string>& keep_keys) {
        auto logger = log_manager_.getLogger("state_manager");
        if (sat_clock_indices_.empty()) return;

        std::vector<std::string> to_remove;
        for (const auto& [key, idx] : sat_clock_indices_)
            if (keep_keys.find(key) == keep_keys.end())
                to_remove.push_back(key);
        if (to_remove.empty()) return;

        logger->info("Pruning {} sat clock states (keeping {})",
            to_remove.size(), keep_keys.size());
        for (const auto& key : to_remove) {
            auto it = sat_clock_indices_.find(key);
            if (it == sat_clock_indices_.end()) continue;

            const size_t rem_idx = it->second;
            if (rem_idx < core_state_size_ || getStateSize() <= core_state_size_) {
                logger->warn("pruneSatClockStatesExcept: invalid index {} for {}", rem_idx, key);
                continue;
            }

            // removeStateAtIndex decrements all higher indices across every
            // dynamic map (incl. sat_clock_indices_), so the next key's index is
            // re-read fresh from the map on the following iteration.
            removeStateAtIndex(rem_idx);
            sat_clock_indices_.erase(key);

            logger->debug("Removed sat clock state for {} at former index {}", key, rem_idx);
        }
    }

    namespace {
        /**
         * @brief Seconds elapsed from @p earlier to @p later.
         *
         * Works across GPS-week rollovers because GnssTime carries the
         * absolute GPS week.  Negative result indicates @p later is
         * actually earlier than @p earlier — callers treat that as a
         * malformed gap (effectively zero).
         */
        double gnssTimeDiffSeconds(const GnssTime& later, const GnssTime& earlier) {
            return static_cast<double>(later.week - earlier.week)
                       * static_cast<double>(TimeUtils::SECONDS_IN_WEEK)
                 + (later.tow - earlier.tow);
        }
    }

    std::optional<StateManager::SatBiasRestoration>
    StateManager::tryRestoreSatBias(const std::string& key, const GnssTime& now) {
        auto it = sat_bias_cache_.find(key);
        if (it == sat_bias_cache_.end()) return std::nullopt;

        const double gap_s = std::max(0.0, gnssTimeDiffSeconds(now, it->second.drop_time));
        const double warm_age = config_.getSatBiasCacheWarmAgeS();
        const double max_age  = config_.getSatBiasCacheMaxAgeS();

        if (gap_s > max_age) {
            // Entry is too stale to be trustworthy as either value or
            // variance; evict and tell the caller to use a cold WLS seed.
            sat_bias_cache_.erase(it);
            return std::nullopt;
        }

        SatBiasRestoration r{};
        r.value  = it->second.value;
        r.gap_s  = gap_s;
        if (gap_s <= warm_age) {
            // Warm restart: preserve the cached variance, aged forward
            // by the configured random-walk PSD so the filter sees the
            // appropriate "uncertainty grew during the outage" inflation.
            const double q = config_.getProcessNoise().sat_bias;
            r.variance = it->second.variance + std::max(0.0, q) * gap_s;
            r.is_warm  = true;
        } else {
            // Cold-value restart: stashed variance no longer trustworthy
            // (gap too long).  Reuse the seed value but reset variance
            // to the cold-start initial.  Still strictly better than a
            // WLS single-epoch seed when the value is correct.
            const double sb_std = config_.getInitialStd().sat_bias;
            r.variance = sb_std * sb_std;
            r.is_warm  = false;
        }
        return r;
    }

    void StateManager::consumeSatBiasCacheEntry(const std::string& key) {
        sat_bias_cache_.erase(key);
    }

    void StateManager::evictExpiredSatBiasCache(const GnssTime& now) {
        const double max_age = config_.getSatBiasCacheMaxAgeS();
        if (sat_bias_cache_.empty()) return;
        for (auto it = sat_bias_cache_.begin(); it != sat_bias_cache_.end(); ) {
            const double gap_s = gnssTimeDiffSeconds(now, it->second.drop_time);
            if (gap_s > max_age || gap_s < 0.0) {
                it = sat_bias_cache_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // ===================================================================
    // Unresolvable ambiguity support
    // ===================================================================

    void StateManager::markAmbiguityUnresolvable(const std::string& amb_key) {
        unresolvable_ambiguities_.insert(amb_key);
    }

    bool StateManager::isAmbiguityUnresolvable(const std::string& amb_key) const {
        return unresolvable_ambiguities_.count(amb_key) > 0;
    }

    void StateManager::clearUnresolvableAmbiguities() {
        unresolvable_ambiguities_.clear();
    }

    bool StateManager::isAmbiguityPendingReseed(const std::string& amb_key) const {
        return pending_reseed_ambiguities_.count(amb_key) > 0;
    }

    void StateManager::clearPendingReseed(const std::string& amb_key) {
        pending_reseed_ambiguities_.erase(amb_key);
    }

    void StateManager::markAmbiguityPendingReseed(const std::string& amb_key) {
        pending_reseed_ambiguities_.insert(amb_key);
    }

    // ===================================================================
    // Gap-recovery helpers
    // ===================================================================

    namespace {
        // Collect the state indices that are "per-satellite" — iono,
        // ambiguity, sat bias.  These are the ones whose cross-cov
        // with position/clock is structurally invalidated by a gap
        // (different geometry, different multipath, possibly different
        // satellite set).  Code bias is receiver-side hardware and is
        // intentionally excluded — it's near-constant and its cross-cov
        // to position is physically meaningful.
        std::vector<Eigen::Index>
        collectSatSpecificIndices(const StateManager& sm) {
            std::vector<Eigen::Index> out;
            out.reserve(sm.getIonosphereStates().size()
                      + sm.getAmbiguities().size()
                      + sm.getSatBiasStates().size());
            for (const auto& [_, idx] : sm.getIonosphereStates())
                out.push_back(static_cast<Eigen::Index>(idx));
            for (const auto& [_, idx] : sm.getAmbiguities())
                out.push_back(static_cast<Eigen::Index>(idx));
            for (const auto& [_, idx] : sm.getSatBiasStates())
                out.push_back(static_cast<Eigen::Index>(idx));
            return out;
        }
    }

    void StateManager::zeroPositionCrossCovariancesToSatStates() {
        auto logger = log_manager_.getLogger("state_manager");
        const auto sat_idx = collectSatSpecificIndices(*this);
        if (sat_idx.empty()) {
            logger->trace("zeroPositionCrossCov: no sat-specific states; no-op");
            return;
        }
        const Eigen::Index pos = static_cast<Eigen::Index>(getPositionIndex());
        for (Eigen::Index k = 0; k < 3; ++k) {
            for (Eigen::Index j : sat_idx) {
                covariance_(pos + k, j) = 0.0;
                covariance_(j, pos + k) = 0.0;
            }
        }
        logger->info("Zeroed position cross-covariances to {} sat-specific states",
                     sat_idx.size());
    }

    void StateManager::zeroClockCrossCovariancesToSatStates() {
        auto logger = log_manager_.getLogger("state_manager");
        if (!hasClock()) {
            logger->trace("zeroClockCrossCov: no clock state (BSSD/baseline); no-op");
            return;
        }
        const auto sat_idx = collectSatSpecificIndices(*this);
        if (sat_idx.empty()) {
            logger->trace("zeroClockCrossCov: no sat-specific states; no-op");
            return;
        }
        const Eigen::Index clk = static_cast<Eigen::Index>(getClockIndex());
        for (Eigen::Index j : sat_idx) {
            covariance_(clk, j) = 0.0;
            covariance_(j, clk) = 0.0;
        }
        logger->info("Zeroed clock cross-covariances to {} sat-specific states",
                     sat_idx.size());
    }

    void StateManager::inflatePositionDiagonal(double sigma_m) {
        auto logger = log_manager_.getLogger("state_manager");
        if (!(std::isfinite(sigma_m) && sigma_m > 0.0)) {
            logger->warn("inflatePositionDiagonal: invalid sigma_m={}, skipping", sigma_m);
            return;
        }
        const double var_add = sigma_m * sigma_m;
        const Eigen::Index pos = static_cast<Eigen::Index>(getPositionIndex());
        for (Eigen::Index k = 0; k < 3; ++k) {
            covariance_(pos + k, pos + k) += var_add;
        }
        logger->info("Inflated position diagonal by sigma={:.3f} m (var+={:.3e} m²)",
                     sigma_m, var_add);
    }

    void StateManager::resetVelocityToLoose(double sigma_mps) {
        auto logger = log_manager_.getLogger("state_manager");
        if (!hasVelocity()) {
            logger->trace("resetVelocityToLoose: no velocity state; no-op");
            return;
        }
        if (!(std::isfinite(sigma_mps) && sigma_mps > 0.0)) {
            logger->warn("resetVelocityToLoose: invalid sigma_mps={}, skipping", sigma_mps);
            return;
        }
        const Eigen::Index vel = static_cast<Eigen::Index>(getVelocityIndex());
        const double var = sigma_mps * sigma_mps;
        for (Eigen::Index k = 0; k < 3; ++k) {
            // Zero value, zero cross-cov, then set diagonal — same
            // pattern as resetAmbiguity for symmetry.
            state_vector_(vel + k) = 0.0;
            covariance_.row(vel + k).setZero();
            covariance_.col(vel + k).setZero();
            covariance_(vel + k, vel + k) = var;
        }
        logger->info("Reset velocity to 0 with sigma={:.3f} m/s (var={:.3e})",
                     sigma_mps, var);
    }

    void StateManager::clearSatBiasCache() {
        auto logger = log_manager_.getLogger("state_manager");
        const auto n = sat_bias_cache_.size();
        sat_bias_cache_.clear();
        if (n) logger->info("Cleared sat-bias cache ({} entries dropped)", n);
    }

} // namespace GnssProcessor
