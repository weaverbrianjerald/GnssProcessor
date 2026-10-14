/*!
 * \file StateManagerProcessNoise.cpp
 * \brief StateManager's process noise and transition matrices (card UOM step U6c).
 *
 * Owns getProcessNoiseMatrix, a short driver over one function per block of Q, and
 * getTransitionMatrix.  The blocks are called in the order of the original single
 * function: motion, then (baseline mode) the baseline block and return, else the
 * receiver clock, phase clock, troposphere, slant ionosphere, receiver code bias,
 * satellite code bias, satellite phase bias, satellite clock, inter-system offsets and
 * ambiguities.  Every entry of Q is written by exactly one block, so the order of the
 * calls does not change Q.
 */

#include "filter/StateManager.hpp"
#include "filter/StateKeySchema.hpp"
#include "logging/LogManager.hpp"
#include "common/Constants.hpp"
#include <cmath>
#include <stdexcept>
#include <string>
#include <spdlog/spdlog.h>
#include <Eigen/Dense>

namespace GnssProcessor {

    // ===================================================================
    // Process noise: the driver
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

        addMotionQ_(Q, pn, dt);

        // Baseline mode: no clock / tropo / iono / system-offset / bias states
        // in the state vector.  Apply ambiguity process noise below (if any),
        // optionally apply random-walk noise to the differential ZWD state,
        // and return.  DD ambiguities are physically constant → pn.ambiguity
        // typically 0, so Q is essentially the position block plus (optionally)
        // a 1×1 Δzwd entry.
        if (is_baseline_) {
            addBaselineQ_(Q, pn, dt, state_size);
            return Q;
        }

        addReceiverClockQ_(Q, pn, dt);
        addPhaseClockQ_(Q, pn, dt);
        addTroposphereQ_(Q, pn, dt);
        addIonosphereQ_(Q, pn, dt);
        addReceiverCodeBiasQ_(Q, pn, dt);
        addSatBiasQ_(Q, pn, dt);
        addSatPhaseBiasQ_(Q, pn, dt);
        addSatClockQ_(Q, pn, dt);
        addSystemTimeOffsetQ_(Q, pn, dt);
        addAmbiguityQ_(Q, pn, dt, state_size);

        logger->trace("Computed process noise matrix: size={}x{}, dt={:.3f} s, clock_q={:.3e}, bssd={}",
            Q.rows(), Q.cols(), dt, pn.clock, config_.enableBetweenSatelliteSd() ? "on" : "off");
        return Q;
    }

    // ===================================================================
    // Process noise: one function per block, in the driver's order
    // ===================================================================

    void StateManager::addMotionQ_(Eigen::MatrixXd& Q, const ProcessNoise& pn, double dt) const {
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
    }

    void StateManager::addBaselineQ_(Eigen::MatrixXd& Q, const ProcessNoise& pn, double dt, size_t state_size) const {
        // Baseline (DD) mode: the ambiguities (no frozen-pivot skip) and the
        // optional differential ZWD.
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
    }

    void StateManager::addReceiverClockQ_(Eigen::MatrixXd& Q, const ProcessNoise& pn, double dt) const {
        size_t clockIdx = getClockIndex();

        // Clock noise, in two regimes:
        //   (a) Clock-rate state present (Doppler processed): continuous
        //       white-noise acceleration with spectral density pn.clock_rate
        //       (m²/s³).  The clock bias receives q·dt³/3, the clock rate q·dt,
        //       and the cross-covariance q·dt²/2.
        //   (b) No clock-rate state: random walk on the clock bias with spectral
        //       density pn.clock (m²/s).
        //
        // Under BSSD the constructor allocates neither the clock bias nor the
        // clock rate (both cancel in the between-satellite difference), so
        // neither regime writes anything there.  The !BSSD tests below are
        // therefore always true when they are reached: the one inside the
        // clock-rate branch can never be false, because the clock rate exists
        // only when BSSD is off.
        if (hasClockRate()) {
            const size_t crIdx = getClockRateIndex();
            const double q_cr  = pn.clock_rate;
            const double dt2   = dt * dt;
            const double q_cc  = q_cr * dt2 * dt / 3.0;
            const double q_x   = q_cr * dt2 / 2.0;
            const double q_rr  = q_cr * dt;
            // Always true here (its false side is unreachable): the constructor allocates no clock rate under BSSD.
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
    }

    void StateManager::addPhaseClockQ_(Eigen::MatrixXd& Q, const ProcessNoise& pn, double dt) const {
        // Phase clock offset noise (when enabled)
        // Also suppressed under BSSD for the same reason as the clock.
        if (config_.estimatePhaseClock() && !config_.enableBetweenSatelliteSd()) {
            size_t phaseClockIdx = getPhaseClockIndex();
            double phase_clock_var = pn.phase_clock * dt;
            Q(phaseClockIdx, phaseClockIdx) = phase_clock_var;
        }
    }

    void StateManager::addTroposphereQ_(Eigen::MatrixXd& Q, const ProcessNoise& pn, double dt) const {
        size_t tropoIdx = getTropoIndex();

        // Tropospheric noise
        double tropo_var = pn.troposphere * dt;
        Q(tropoIdx, tropoIdx) = tropo_var;

        // Tropospheric gradient noise (Gn, Ge)
        if (config_.estimateTropoGradients()) {
            double grad_var = pn.tropo_gradient * dt;
            Q(gradient_north_idx_, gradient_north_idx_) = grad_var;
            Q(gradient_east_idx_, gradient_east_idx_) = grad_var;
        }
    }

    void StateManager::addIonosphereQ_(Eigen::MatrixXd& Q, const ProcessNoise& pn, double dt) const {
        // Per-satellite ionospheric noise, applied only when the processing mode
        // is PPP_RTK.  The states are born whenever uncombined processing is on
        // (in any mode), so a PPP run with uncombined processing gives them no
        // process noise (card UOM defect S1).
        // Scale zenith spectral density by obliquity factor: q_slant = q_zenith / sin²(el)
        if (config_.getProcessingMode() == ProcessingMode::PPP_RTK) {
            const double q_zenith_default = pn.ionosphere;
            const bool   iono_cv          = config_.enableIonoRate();
            const double q_rate_zenith    = pn.iono_rate_psd; // accel PSD (m^2/s^3)
            const double dt2              = dt * dt;
            for (const auto& [sat, idx] : ionosphere_indices_) {
                // No process noise for BSSD pivot iono states (unobservable).
                // The whole satellite is skipped, so the companion rate state
                // also receives no Q and its frozen (0,0) value accrues no
                // variance, matching the treatment of the level state.
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

                if (iono_cv) {
                    auto rit = iono_rate_indices_.find(sat);
                    if (rit != iono_rate_indices_.end()) {
                        // Constant-velocity [I_s, Idot_s] CWNA block.  The
                        // acceleration PSD is obliquity-scaled to slant like the
                        // level state (empirically, q_zenith/sin² reproduces the
                        // observed slant rate scatter across elevation).  When the
                        // adaptive monitor has flagged this satellite, the rate PSD
                        // is inflated by the ratio applied to the level random walk
                        // (override / static level q).  The Sage-Husa storm response
                        // then drives the rate term Idot, and because the ratio is
                        // dimensionless the m²/s to m²/s³ unit change needs no
                        // conversion.
                        double q_rate = q_rate_zenith / sin2_el;
                        if (q_ov != iono_q_overrides_.end() && q_zenith_default > 0.0) {
                            q_rate *= (q_ov->second / q_zenith_default);
                        }
                        const size_t r = rit->second;
                        Q(idx, idx) = q_rate * dt2 * dt / 3.0; // level: q*dt^3/3
                        Q(idx, r)   = q_rate * dt2 / 2.0;      // cross: q*dt^2/2
                        Q(r,   idx) = q_rate * dt2 / 2.0;
                        Q(r,   r)   = q_rate * dt;             // rate:  q*dt
                        continue;
                    }
                }
                Q(idx, idx) = (q_zenith / sin2_el) * dt; // random-walk fallback
            }
        }
    }

    void StateManager::addReceiverCodeBiasQ_(Eigen::MatrixXd& Q, const ProcessNoise& pn, double dt) const {
        // Receiver code bias (IFB) noise (uncombined processing with estimate_ifb).
        // Suppressed under BSSD: receiver DCB/IFB columns are zeroed in the SD
        // H-matrix, so those states are unobservable.  The exception is the
        // per-FCN-slot GLONASS IFB states: BssdTransform does not zero their
        // columns, so they survive SD and keep their random walk with BSSD on.
        if (!code_bias_indices_.empty()) {
            const double cb_var = pn.code_bias * dt;
            const bool sd = config_.enableBetweenSatelliteSd();
            for (const auto& [key, idx] : code_bias_indices_) {
                if (sd && !StateKeys::isGlonassPerSlotIfbKey(key)) continue;
                Q(idx, idx) = cb_var;
            }
        }
    }

    void StateManager::addSatBiasQ_(Eigen::MatrixXd& Q, const ProcessNoise& pn, double dt) const {
        // Per-satellite signal bias noise (estimate_sat_biases; on a network
        // station the same map holds the satellite OSB columns).
        // These absorb uncorrected satellite hardware delays on signals
        // without BIA coverage.  They are nearly constant, so the random walk
        // is very small.  Suppressed for BSSD pivot satellites (absorbed into the
        // eliminated clock).
        if (!sat_bias_indices_.empty()) {
            const double sb_var = pn.sat_bias * dt;
            for (const auto& [key, idx] : sat_bias_indices_) {
                // Key format: StateKeySchema.hpp.  A key without a pipe is
                // taken whole as the satellite.
                const std::string sat = StateKeys::satelliteFromSatBiasKey(key);
                if (frozen_pivot_sats_.count(sat)) continue;
                Q(idx, idx) = sb_var;
            }
        }
    }

    void StateManager::addSatPhaseBiasQ_(Eigen::MatrixXd& Q, const ProcessNoise& pn, double dt) const {
        // Per-satellite signal phase-bias noise (PPP_RTK_Network producer).
        // The phase bias is more stable than the code bias; a small random walk
        // lets the satellite column follow slow drift, where a strictly
        // constant (q=0) state would over-converge.  Suppressed for the S-basis
        // datum (frozen-pivot) satellite, whose phase bias is held at (value=0,
        // variance=0); Q there would let it drift off the datum.
        if (!sat_phase_bias_indices_.empty()) {
            const double spb_var = pn.sat_phase_bias * dt;
            for (const auto& [key, idx] : sat_phase_bias_indices_) {
                const std::string sat = StateKeys::satelliteFromSatPhaseBiasKey(key);
                if (frozen_pivot_sats_.count(sat)) continue;
                Q(idx, idx) = spb_var;
            }
        }
    }

    void StateManager::addSatClockQ_(Eigen::MatrixXd& Q, const ProcessNoise& pn, double dt) const {
        // Per-satellite clock noise (PPP_RTK_Network producer).  The state is
        // Δδtˢ relative to the a-priori product clock; a small random walk lets
        // the estimated clock follow drift between epochs while the
        // carried-forward prior provides continuity.  Suppressed for the S-basis datum
        // (frozen-pivot) satellite, whose clock is held at (value=0, variance=0).
        if (!sat_clock_indices_.empty()) {
            const double sc_var = pn.sat_clock * dt;
            for (const auto& [key, idx] : sat_clock_indices_) {
                const std::string sat = StateKeys::satelliteFromSatClockKey(key);
                if (frozen_pivot_sats_.count(sat)) continue;
                Q(idx, idx) = sc_var;
            }
        }
    }

    void StateManager::addSystemTimeOffsetQ_(Eigen::MatrixXd& Q, const ProcessNoise& pn, double dt) const {
        // Inter-system time offset noise (and rate offset, when present).
        //
        // Per-system counterpart of the clock-bias / clock-rate handling above.
        // When the ISBR (rate offset) states exist, each ISB/ISBR pair gets a
        // CWNA block with spectral density pn.system_time_rate_offset;
        // otherwise the bias gets the legacy random walk with
        // pn.system_time_offset.  Under BSSD the constructor allocates neither
        // the ISBs nor the ISBRs, so both maps are empty there; the bssd tests
        // below never select a different branch (inside the ISBR branch, bssd is
        // always false).
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
                // Always true here (its false side is unreachable): the constructor allocates no ISBR under BSSD.
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
    }

    void StateManager::addAmbiguityQ_(Eigen::MatrixXd& Q, const ProcessNoise& pn, double dt, size_t state_size) const {
        // Ambiguity process noise (zero for constant ambiguities).
        // Ambiguities missing BIA/ATX phase coverage absorb a time-constant
        // satellite bias into the float value, so they need no extra process
        // noise; they are excluded from integer AR through the
        // unresolvable-ambiguity set.  BSSD pivot ambiguities are frozen
        // (value=0, variance=0 by construction of the S-transform) and get no Q:
        // Q would grow their variance past PivotManager::isPivotDisturbed's
        // 1e-12 threshold every epoch and trigger spurious handovers.
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
    }

    // ===================================================================
    // Transition
    // ===================================================================

    Eigen::MatrixXd StateManager::getTransitionMatrix(double dt) const {
        size_t state_size = getStateSize();
        Eigen::MatrixXd F = Eigen::MatrixXd::Identity(state_size, state_size);

        // Position-velocity coupling: pos(k+1) = pos(k) + vel(k) * dt
        if (hasVelocity()) {
            F.block(getPositionIndex(), getVelocityIndex(), 3, 3) = Eigen::Matrix3d::Identity() * dt;
        }

        // Clock-bias / clock-rate coupling: δt_r(k+1) = δt_r(k) + dt · ḋt_r(k).
        // Skipped under BSSD: SD cancels the clock bias, so it is unobservable
        // from code and phase, and propagating the rate into it would let it
        // drift without bound.  The per-system ISB / ISBR pairs follow the
        // same rule.
        if (hasClockRate() && !config_.enableBetweenSatelliteSd()) {
            F(getClockIndex(), getClockRateIndex()) = dt;
            for (const auto& [sys_name, isb_idx] : system_time_offset_indices_) {
                auto rit = system_time_rate_offset_indices_.find(sys_name);
                if (rit == system_time_rate_offset_indices_.end()) continue;
                F(isb_idx, rit->second) = dt;
            }
        }

        // Per-satellite slant-iono level/rate coupling (constant-velocity model):
        // I_s(k+1) = I_s(k) + dt * Idot_s(k).  Unlike the clock (unobservable
        // after SD and gated off under BSSD), the slant-iono level stays
        // observable under BSSD, where only the pivot column is zeroed, so the
        // coupling is kept for every non-pivot satellite.  A frozen pivot's rate
        // is held at (0,0) with its Q suppressed; its coupling is skipped because
        // it would move the pivot level off the SD datum.
        if (config_.enableIonoRate()) {
            for (const auto& [sat, iono_idx] : ionosphere_indices_) {
                if (frozen_pivot_sats_.count(sat)) continue;
                auto rit = iono_rate_indices_.find(sat);
                if (rit != iono_rate_indices_.end()) {
                    F(iono_idx, rit->second) = dt;
                }
            }
        }

        auto logger = log_manager_.getLogger("state_manager");
        logger->trace("Computed transition matrix: size={}x{}, dt={:.3f} s, velocity_coupling={}, clock_rate_coupling={}",
            F.rows(), F.cols(), dt,
            hasVelocity() ? "on" : "off",
            (hasClockRate() && !config_.enableBetweenSatelliteSd()) ? "on" : "off");
        return F;
    }

} // namespace GnssProcessor
