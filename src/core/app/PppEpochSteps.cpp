/**
 * @file PppEpochSteps.cpp
 * @brief The stages of one PPP / PPP-RTK epoch up to the measurement update.
 *
 * In the order Application::runPpp (Application.cpp) calls them: the epoch's
 * observations, the decimation (processing.gnss_rate_s) and duration
 * (processing.max_duration_s) gates, the epoch stamp with the time step and the data-gap
 * test, the ppp_state_ sync, the models, the WLS clock seed and the data-gap policy, the
 * per-epoch process-noise overrides (ATX-substituted ambiguities, adaptive ionosphere),
 * the Kalman predict and the cycle-slip detection.  The stages that were inline blocks of
 * the former single Application::run read and write the epoch's PppEpochWork
 * (app/PppPipelineWork.hpp) through an alias block, so their bodies read as they did
 *.
 */

#include "app/Application.hpp"
#include "app/PppPipelineWork.hpp"
#include "math/TimeUtils.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>
#include <fmt/format.h>

namespace GnssProcessor {


// ---------------------------------------------------------------------------
// Epoch helper: Collect observations for one epoch
// ---------------------------------------------------------------------------

bool Application::collectEpochObservations(const std::vector<RinexObservation>& observations,
                                           size_t& epoch_idx,
                                           std::vector<RinexObservation>& epoch_obs,
                                           GnssTime& current_time) {
    current_time = observations[epoch_idx].time;
    epoch_obs.clear();
    const auto& active_systems = config_.getActiveSystems();

    while (epoch_idx < observations.size() && observations[epoch_idx].time.tow == current_time.tow) {
        std::string obs_system;
        switch (observations[epoch_idx].system) {
        case System::GPS:     obs_system = "GPS";     break;
        case System::Galileo: obs_system = "Galileo"; break;
        case System::GLONASS: obs_system = "GLONASS"; break;
        case System::BeiDou:  obs_system = "BeiDou";  break;
        case System::QZSS:    obs_system = "QZSS";    break;
        case System::SBAS:    obs_system = "SBAS";    break;
        default:              obs_system = "Unknown";  break;
        }
        if (std::find(active_systems.begin(), active_systems.end(), obs_system) != active_systems.end()) {
            epoch_obs.push_back(observations[epoch_idx]);
        }
        ++epoch_idx;
    }
    return !epoch_obs.empty();
}

// ---------------------------------------------------------------------------
// Epoch stage 4a2: input-rate decimation
// ---------------------------------------------------------------------------

bool Application::skipDecimatedEpoch_(const PppEpochWork& w) const {
    const auto& current_time = w.current_time;

    // 4a2. Optional input-rate decimation (processing.gnss_rate_s).
    // With a processing rate coarser than the RINEX interval (e.g. 300 s
    // output from a 30 s file), input epochs that fall between processed
    // epochs are skipped without a log line.  The first epoch anchors the
    // schedule.
    if (config_.getGnssRate() > 0.0 && !first_epoch_) {
        // 1 ms tolerance absorbs GPS-time rounding.
        const double since_last = current_time.tow - last_processed_tow_;
        if (since_last + 1e-3 < config_.getGnssRate()) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Epoch stage 4a3: the max-duration cutoff
// ---------------------------------------------------------------------------

bool Application::reachedMaxDuration_(const PppEpochWork& w) {
    const auto& current_time = w.current_time;

    // 4a3. Optional max-duration cutoff (processing.max_duration_s).
    // Stop the run once the data-time span from the first processed epoch
    // reaches the configured limit.  The span is epoch GPS time relative to
    // the first processed observation (3600 s is one hour, or about 3600
    // epochs of 1 s data), independent of replay speed.  The first epoch
    // anchors the window and is reached only after any leading decimation
    // skip above, so skipped epochs do not consume the budget.  0 (default)
    // means no limit.  Real-time has its own copy in
    // RealTimeEngine::processEpoch.
    if (config_.getMaxDuration() > 0.0) {
        if (first_epoch_) {
            max_duration_anchor_s_ = TimeUtils::continuousGpsSeconds(current_time);
        } else {
            const double span = TimeUtils::continuousGpsSeconds(current_time)
                              - max_duration_anchor_s_;
            if (span >= config_.getMaxDuration()) {
                log_manager_.getLogger("main_log")->info(
                    "Reached max processing duration: {:.1f} s of data processed "
                    "(limit {:.1f} s); stopping epoch loop", span, config_.getMaxDuration());
                return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Epoch stage 4b: the epoch stamp, the time step and the data-gap test
// ---------------------------------------------------------------------------

void Application::stepEpochClock_(PppEpochWork& w) {
    const auto& current_time = w.current_time;
    const auto& epoch_prefix = w.epoch_prefix;
    auto& dt = w.dt;
    auto& gap_detected = w.gap_detected;
    auto& long_gap = w.long_gap;

    state_manager_->setStateEpochTime(current_time);
    LogManager::setCurrentEpochTime(current_time);

    // 4b. Compute time step and detect data gaps.
    // With decimation active the effective cadence is
    // max(gnss_rate, rinex_interval); it sets the gap threshold and the
    // "unexpected dt" warning, so decimated steps are not flagged as
    // outages.
    const double rinex_interval = rinex_parser_->getHeader().interval.value_or(30.0);
    const double effective_interval = config_.getGnssRate() > 0.0
                                    ? std::max(config_.getGnssRate(), rinex_interval)
                                    : rinex_interval;
    if (first_epoch_) {
        dt = effective_interval;
        log_manager_.getLogger("main_log")->info("{}First epoch, assuming dt = {}s", epoch_prefix, dt);
    } else {
        dt = current_time.tow - prev_tow_;
        const double gap_tol = config_.getDataGapTolerance() > 0.0
                             ? config_.getDataGapTolerance()
                             : effective_interval * 1.5;
        gap_detected = dt > gap_tol;
        long_gap     = gap_detected && (dt > config_.getLongGapTolerance());
        if (std::abs(dt - effective_interval) > 0.5) {
            log_manager_.getLogger("main_log")->warn("{}Unexpected time step {}s (expected {}s)",
                epoch_prefix, dt, effective_interval);
        }
    }
    prev_tow_ = current_time.tow;
    prev_epoch_time_ = current_time;
    last_processed_tow_ = current_time.tow;
}

// ---------------------------------------------------------------------------
// Epoch stage 4c: ppp_state_ from the state vector
// ---------------------------------------------------------------------------

bool Application::syncPppState_(const PppEpochWork& w) {
    const auto& epoch_prefix = w.epoch_prefix;

    try {
        state_manager_->updatePppStateFromVector(state_manager_->getStateVector(), ppp_state_, *state_manager_);
    } catch (const std::exception& e) {
        log_manager_.getLogger("main_log")->error("{}Failed to update ppp_state: {}", epoch_prefix, e.what());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Epoch stage 4d: the modeled observations
// ---------------------------------------------------------------------------

bool Application::computeEpochModels_(PppEpochWork& w) {
    const auto& epoch_obs = w.epoch_obs;
    const auto& epoch_prefix = w.epoch_prefix;
    auto& modeled_obs = w.modeled_obs;
    auto& drop_reasons = w.drop_reasons;

    // 4d. Compute modeled observations.
    // drop_reasons is populated by computeModels for every sat that
    // fails to make it into modeled_obs (BelowMask / NoProduct /
    // NoModel).  Consumed by writeEpochTraces -> logSatelliteStatus
    // so the strip chart distinguishes actionable failure modes.
    if (!ppp_model_->computeModels(epoch_obs, ppp_state_, modeled_obs, &drop_reasons)) {
        log_manager_.getLogger("main_log")->warn("{}Failed to compute models for epoch", epoch_prefix);
        return false;
    }
    {
        std::string sat_clk_summary;
        for (const auto& obs : modeled_obs) {
            sat_clk_summary += fmt::format("{}:r={:.1f}km ", obs.satellite, obs.range / 1000.0);
        }
        log_manager_.getLogger("main_log")->debug(
            "{}Modeled {} sats: {}", epoch_prefix, modeled_obs.size(), sat_clk_summary);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Epoch helper: WLS estimation & KF clock seeding
// ---------------------------------------------------------------------------

WlsResult Application::handleWlsSeeding(const GnssTime& current_time,
                                        const std::string& epoch_prefix,
                                        const std::vector<PppObservation>& modeled_obs,
                                        bool gap_detected) {
    // Under BSSD the KF carries no receiver clock state, so there is nothing to
    // seed and the WLS clock estimate has no counterpart in the SD filter.  The
    // whole seeding path is skipped.
    if (!state_manager_->hasClock()) {
        return WlsResult{};
    }

    auto wls_result = wls_estimator_->estimate(current_time, modeled_obs);
    if (!wls_result.success() || wls_result.state.size() < 4) return wls_result;

    constexpr double kClockJumpThreshSec = 0.00005; // 50 µs (about 15 km)

    const double wls_clock_bias = wls_result.state(3);
    const Eigen::VectorXd sv0 = state_manager_->getStateVector();
    const size_t clock_idx = state_manager_->getClockIndex();
    const double kf_clock_bias = (sv0.size() > static_cast<Eigen::Index>(clock_idx)) ? sv0(clock_idx) : 0.0;

    const double clock_jump_thresh_m = kClockJumpThreshSec * Constants::SPEED_OF_LIGHT;
    const double delta_clock_m = wls_clock_bias - kf_clock_bias;

    const bool jump_detected = std::abs(delta_clock_m) >= clock_jump_thresh_m;
    const bool should_seed = jump_detected || gap_detected || first_epoch_;

    if (should_seed && sv0.size() > static_cast<Eigen::Index>(clock_idx)) {
        Eigen::VectorXd sv = sv0;
        sv(clock_idx) = wls_clock_bias;
        state_manager_->setStateVector(sv);
        ppp_state_.clock_bias = wls_clock_bias;
        log_manager_.getLogger("main_log")->info(
            "{}Seeded KF clock from WLS (jump): wls_clk={:.3f} m, kf_clk={:.3f} m, delta={:.3f} m",
            epoch_prefix, wls_clock_bias, kf_clock_bias, delta_clock_m);
    } else {
        log_manager_.getLogger("main_log")->trace(
            "{}Skipped WLS clock seeding: wls_clk={:.3f} m, kf_clk={:.3f} m, delta={:.3f} m, jump_detected={}",
            epoch_prefix, wls_clock_bias, kf_clock_bias, delta_clock_m, jump_detected);
    }

    // Only write expensive WLS trace when overall logging level is Debug.
    if (auto main_logger = log_manager_.getLogger("main_log")) {
        if (main_logger->level() == spdlog::level::debug) {
            trace_logger_->logWls(current_time,
                wls_result, wls_clock_bias, kf_clock_bias, delta_clock_m,
                jump_detected, should_seed);
        } else {
            main_logger->trace("{}WLS trace skipped (log level != Debug)", epoch_prefix);
        }
    }

    return wls_result;
}

// ---------------------------------------------------------------------------
// Epoch helper: dynamics-mode classification + gap recovery policy
// ---------------------------------------------------------------------------

Application::DynamicsMode Application::classifyDynamicsMode() const {
    // Six orders of magnitude below any process noise that represents
    // real motion, six orders above the typical 1e-12 m²/s numeric-
    // stability epsilon some users set in static configs.  Crossing
    // this threshold is unambiguous in practice.
    constexpr double kKinematicPnPositionThreshold = 1e-6;  // m²/s

    if (state_manager_->hasVelocity()) {
        return config_.enableImu() ? DynamicsMode::KinematicIMU
                                   : DynamicsMode::KinematicNoImu;
    }
    return (config_.getProcessNoise().position >= kKinematicPnPositionThreshold)
               ? DynamicsMode::KinematicNoImu
               : DynamicsMode::Static;
}

void Application::handleGap(double dt,
                            bool long_gap,
                            const std::string& epoch_prefix,
                            const WlsResult& wls) {
    auto log = log_manager_.getLogger("main_log");
    const DynamicsMode mode = classifyDynamicsMode();

    // KinematicIMU returns early: the IMU has propagated position and velocity
    // through the predict step at realistic uncertainty, so the gap is already
    // handled.  Satellite-specific cross-covariances are still suspect (geometry
    // and multipath have changed), but iono Q inflation covers the dominant
    // component.  Only the clock cross-covariances are zeroed, so the WLS clock
    // reseed does not propagate into position.
    if (mode == DynamicsMode::KinematicIMU) {
        if (log) {
            log->info("{}handleGap: dt={:.1f}s, mode=KinematicIMU, long={} "
                      "→ IMU bridging; zeroing clock×sat cross-cov only",
                      epoch_prefix, dt, long_gap);
        }
        state_manager_->zeroClockCrossCovariancesToSatStates();
        return;
    }

    // Common to Static and KinematicNoImu: remove stale clock-to-satellite-state
    // correlations after the WLS clock reseed.
    state_manager_->zeroClockCrossCovariancesToSatStates();

    if (mode == DynamicsMode::Static && !long_gap) {
        if (log) {
            log->info("{}handleGap: dt={:.1f}s, mode=Static-short "
                      "→ zeroing position×sat cross-cov",
                      epoch_prefix, dt);
        }
        state_manager_->zeroPositionCrossCovariancesToSatStates();
        return;
    }

    // Static-long and KinematicNoImu both get a heavier reset.  Common part:
    // drop all satellite-specific states so ensureRequiredStates rebuilds them
    // around the current geometry, and drop the BSSD/DD pivots so the next epoch
    // elects new ones.
    state_manager_->zeroPositionCrossCovariancesToSatStates();
    state_manager_->pruneIonosphereStatesExcept({});
    state_manager_->pruneAmbiguitiesExcept({});
    state_manager_->pruneSatBiasStatesExcept({});

    // The ambiguity teardown above invalidates every soft-LLI / gap-bridge hold
    // and snapshot.  A hold that outlived its state would later restore a
    // pre-gap value onto a newly rebuilt ambiguity.
    if (cycle_slip_manager_) cycle_slip_manager_->resetSoftLliBridge();
    if (meas_model_) meas_model_->setProtectedAmbiguityKeys({});

    if (config_.enableBetweenSatelliteSd() && meas_model_) {
        meas_model_->getPivotManager().dropAllPivots(*state_manager_);
    }
    if (meas_model_) {
        // Baseline DD path: no-op outside baseline mode (sticky map is empty).
        meas_model_->resetBaselinePivots();
    }

    if (mode == DynamicsMode::Static) {
        // Static-long: the position value is still the best estimate (the
        // antenna did not move), but the converged variance is too tight to
        // absorb post-gap residuals, so it is inflated modestly.
        constexpr double kStaticLongPosSigmaInflation_m = 0.05;  // 5 cm
        state_manager_->inflatePositionDiagonal(kStaticLongPosSigmaInflation_m);
        if (log) {
            log->info("{}handleGap: dt={:.1f}s, mode=Static-long → pos σ+={:.3f}m, "
                      "dropped iono/amb/sat-bias states, dropped pivots",
                      epoch_prefix, dt, kStaticLongPosSigmaInflation_m);
        }
        return;
    }

    // KinematicNoImu: full cold start.  Reseed position from WLS, inflate it to
    // a large prior, reset velocity, and clear the sat-bias cache (the receiver
    // may have power-cycled, so pre-gap hardware estimates are no more reliable
    // than a single-epoch seed).
    constexpr double kKinematicPosSigma_m  = 100.0;  // loose prior
    constexpr double kKinematicVelSigma_mps = 30.0;  // vehicle-scale
    if (wls.success() && wls.state.size() >= 3) {
        Eigen::VectorXd sv = state_manager_->getStateVector();
        const Eigen::Index pos = static_cast<Eigen::Index>(state_manager_->getPositionIndex());
        sv(pos + 0) = wls.state(0);
        sv(pos + 1) = wls.state(1);
        sv(pos + 2) = wls.state(2);
        state_manager_->setStateVector(sv);
        ppp_state_.position = Eigen::Vector3d(wls.state(0), wls.state(1), wls.state(2));
        if (log) {
            log->info("{}handleGap: reseeded position from WLS [{:.3f}, {:.3f}, {:.3f}] m",
                      epoch_prefix, wls.state(0), wls.state(1), wls.state(2));
        }
    } else if (log) {
        log->warn("{}handleGap: WLS unavailable, leaving position value unchanged "
                  "but inflating variance", epoch_prefix);
    }
    state_manager_->inflatePositionDiagonal(kKinematicPosSigma_m);
    state_manager_->resetVelocityToLoose(kKinematicVelSigma_mps);
    state_manager_->clearSatBiasCache();
    if (log) {
        log->info("{}handleGap: dt={:.1f}s, mode=KinematicNoImu → cold start "
                  "(pos σ+={:.1f}m, vel σ={:.1f}m/s, dropped iono/amb/sat-bias/cache/pivots)",
                  epoch_prefix, dt, kKinematicPosSigma_m, kKinematicVelSigma_mps);
    }
}

// ---------------------------------------------------------------------------
// Epoch stage 4e2: process-noise overrides for ATX-substituted ambiguities
// ---------------------------------------------------------------------------

void Application::refreshSubstitutedAtxQ_(const PppEpochWork& w) {
    const auto& epoch_prefix = w.epoch_prefix;

    // (4e2) Refresh per-ambiguity process-noise overrides for
    // substituted-ATX states, if enabled.  Walks the current
    // ambiguity map and tags any state whose underlying signal(s)
    // had their ATX correction frequency-fallback-substituted, so
    // the next predict step inflates that state's variance with
    // @c ambiguity_q_substituted_atx instead of the standard
    // @c process_noise.ambiguity.  Refreshed every epoch because
    // the ambiguity set evolves (new sats, slip resets, pivot
    // handovers); mid-epoch state additions pick up the override
    // on the next epoch.
    const auto& mc_atx = config_.getModelCorrections();
    if (mc_atx.loosen_amb_q_for_substituted_atx && ppp_model_) {
        std::map<std::string, double> overrides;
        for (const auto& [amb_key, idx] : state_manager_->getAmbiguities()) {
            const auto u1 = amb_key.find('_');
            if (u1 == std::string::npos) continue;
            const std::string sat = amb_key.substr(0, u1);
            if (sat.empty()) continue;
            const System sys =
                sat[0] == 'G' ? System::GPS :
                sat[0] == 'E' ? System::Galileo :
                sat[0] == 'R' ? System::GLONASS :
                sat[0] == 'C' ? System::BeiDou :
                sat[0] == 'J' ? System::QZSS :
                sat[0] == 'S' ? System::SBAS : System::Unknown;
            if (sys == System::Unknown) continue;
            const std::string rest = amb_key.substr(u1 + 1);
            const auto u2 = rest.find('_');
            const std::string sig1 = (u2 == std::string::npos) ? rest : rest.substr(0, u2);
            const std::string sig2 = (u2 == std::string::npos) ? std::string{} : rest.substr(u2 + 1);

            bool sub = ppp_model_->wasAtxSubstituted(sat, sig1, sys);
            if (!sub && !sig2.empty()) {
                sub = ppp_model_->wasAtxSubstituted(sat, sig2, sys);
            }
            if (sub) {
                overrides[amb_key] = mc_atx.ambiguity_q_substituted_atx;
            }
        }
        state_manager_->setAmbiguityProcessNoiseOverrides(overrides);
        log_manager_.getLogger("main_log")->debug(
            "{}ATX-sub q overrides refreshed: {} ambiguities flagged "
            "(toggle=ON, q={:.3e} m²/s)",
            epoch_prefix, overrides.size(),
            mc_atx.ambiguity_q_substituted_atx);
    } else {
        state_manager_->setAmbiguityProcessNoiseOverrides({});
        log_manager_.getLogger("main_log")->trace(
            "{}ATX-sub q overrides cleared (toggle=OFF; all ambiguities "
            "use process_noise.ambiguity)",
            epoch_prefix);
    }
}

// ---------------------------------------------------------------------------
// Epoch stage 4e3: adaptive ionospheric process noise
// ---------------------------------------------------------------------------

void Application::refreshAdaptiveIonoQ_(const PppEpochWork& w) {
    const auto& modeled_obs = w.modeled_obs;

    // (4e3) Adaptive ionospheric process noise.  Refresh per-satellite
    // iono process-noise overrides from the geometry-free phase
    // second-difference variability before the predict step consumes
    // them.  Active only when estimation.adaptive_iono_process_noise is
    // enabled (the monitor is null otherwise), leaving the override map
    // empty so getProcessNoiseMatrix() uses process_noise.ionosphere.
    if (iono_variability_monitor_) {
        iono_variability_monitor_->update(modeled_obs);
        state_manager_->setIonoProcessNoiseOverrides(
            iono_variability_monitor_->processNoiseOverrides());
    }
}

// ---------------------------------------------------------------------------
// Epoch stage 4f: the Kalman prediction
// ---------------------------------------------------------------------------

void Application::predictEpoch_(const PppEpochWork& w) {
    const auto& current_time = w.current_time;
    const auto& dt = w.dt;

    // 4f. Kalman prediction.
    if (aiding_manager_ && !aiding_manager_->empty() && !first_epoch_) {
        // Aiding-augmented prediction (currently IMU strapdown).  predictAided
        // delegates to predict(dt) when no source contributes this interval
        // (e.g. no IMU samples), which is the GNSS-only prediction.
        // The state vector is held in a local so the context's reference
        // outlives collectPredict.
        const Eigen::VectorXd aiding_x = state_manager_->getStateVector();
        const AidingEpochContext aiding_ctx{
            aiding_x, prev_epoch_time_, current_time, first_epoch_ };
        kalman_filter_->predictAided(dt, aiding_manager_->collectPredict(aiding_ctx));
    } else {
        kalman_filter_->predict(dt);
    }
    // Predict is the last site in the epoch body that reads first_epoch_.
    // The flag is cleared unconditionally here, so it changes once per run
    // regardless of WLS success or whether the mode has a clock state
    // (BSSD has neither).
    first_epoch_ = false;
}

// ---------------------------------------------------------------------------
// Epoch helper: Detect and repair cycle slips
// ---------------------------------------------------------------------------

CycleSlipEpochResult Application::detectCycleSlips(const std::string& epoch_prefix,
                               const std::vector<PppObservation>& modeled_obs) {
    auto result = cycle_slip_manager_->detectAndRepair(
        epoch_prefix, modeled_obs, *state_manager_, *meas_model_);
    if (result.state_modified) {
        state_manager_->updatePppStateFromVector(state_manager_->getStateVector(), ppp_state_, *state_manager_);
    }
    return result;
}

} // namespace GnssProcessor
