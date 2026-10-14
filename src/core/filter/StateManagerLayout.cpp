/*!
 * \file StateManagerLayout.cpp
 * \brief StateManager's construction: the fixed core layout (card UOM step U6d).
 *
 * Owns the constructor and its four steps.  Baseline (DD) mode lays out position
 * (plus the optional differential ZWD) in layoutBaseline_ and returns; the PPP /
 * PPP-RTK modes run layoutCore_ (the core indices), seedCoreVariances_ (x, P and the
 * initial variances) and logLayout_ (the run-start layout lines), in that order.
 */

#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"
#include <limits>
#include <string>
#include <spdlog/spdlog.h>
#include <Eigen/Dense>

namespace GnssProcessor {

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

        // Baseline mode: the core is position only.  DD processing cancels the
        // receiver clocks and ISBs and, on a short baseline, the iono and tropo,
        // so none of those states is estimated.  Ambiguities are still added
        // dynamically through addAmbiguity(); their keys are DD-specific
        // ("DD|GPS|G05|L1C|G01"), and StateManager does not parse the format.
        if (is_baseline_) {
            layoutBaseline_();
            return;
        }

        layoutCore_();
        seedCoreVariances_();
        logLayout_();
    }

    // ===================================================================
    // Construction steps, in the constructor's order
    // ===================================================================

    void StateManager::layoutBaseline_() {
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
    }

    void StateManager::layoutCore_() {
        // Core PPP / PPP-RTK layout:
        //   position(3) → [optional clock(1)] → tropo(1) → ...
        //
        // The receiver clock-bias state is allocated only when BSSD is off.
        // Same-system SD cancels the receiver clock identically, so under BSSD the
        // bias is unobservable and would only drift on process noise without ever
        // receiving an update.  Omitting it leaves no receiver-side state after the
        // SD transform and saves one row and column.
        //
        // The constructor argument @c core_state_size (pos + clk + tropo = 5) is
        // not used on this path: base_core_size_ is rebuilt here from position, so
        // the later indices (velocity, ISBs) stay consistent whether or not the
        // clock slot exists.
        base_core_size_ = 3;                            // position
        if (!config_.enableBetweenSatelliteSd()) {
            clock_idx_ = base_core_size_;
            ++base_core_size_;
        }
        tropo_idx_ = base_core_size_;
        ++base_core_size_;

        // Optional velocity states (Vx, Vy, Vz) right after position + clock + tropo.
        // The receiver clock-rate state is allocated when estimateVelocity is on
        // and BSSD is off.  Under BSSD, Doppler measurements are SD-transformed
        // with code and phase, and same-system SD cancels the receiver clock rate
        // identically, so the state would be unobservable; omitting it leaves no
        // receiver-side state after the SD transform.  It sits directly after
        // velocity, so a consumer that inspects the layout sees position,
        // velocity and clock rate as one motion block.
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

        // Per-system clock-rate offset states (ISBR), appended after the ISBs.
        // Gated like the receiver clock-rate state: allocated when velocity
        // (Doppler) is estimated and BSSD is off.  Under BSSD, same-system
        // SD-Doppler cancels the ISBR identically, so no state is allocated.
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
    }

    void StateManager::seedCoreVariances_() {
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

            // Clock-rate initial variance, set only when the state was allocated
            // (see the gating above; under BSSD it is absent).  init.clock_rate
            // is sized for TCXO drift uncertainty at startup; a few m/s in
            // c·df_r/dt leaves ample room.
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

        // Initialize the inter-system clock-rate offset (ISBR) covariances.  The
        // true offsets are typically sub-mm/s; the variance is seeded from the
        // configured init.system_time_rate_offset so the filter can move them.
        const double time_rate_offset_var =
            init.system_time_rate_offset * init.system_time_rate_offset;
        for (const auto& [sys_name, idx] : system_time_rate_offset_indices_) {
            covariance_(idx, idx) = time_rate_offset_var;
        }
    }

    void StateManager::logLayout_() {
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

} // namespace GnssProcessor
