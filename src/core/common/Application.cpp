/**
 * @file Application.cpp
 * @brief Top-level application driver for GNSS processing.
 *
 * Implements the Application class which orchestrates initialization of
 * configuration, logging and processing components (product parser, RINEX
 * parser, PPP model, measurement model, Kalman filter, WLS estimator, etc.)
 * and runs the epoch-by-epoch processing loop.
 *
 * The main responsibilities implemented here:
 * - Initialize LogManager and load JSON configuration.
 * - Validate availability of product/SP3 and RINEX observation files.
 * - Initialize processing components and the filter state.
 * - Iterate over RINEX epochs: build models, run WLS, run Kalman predict/update,
 *   detect slips/resets and emit CSV traces for diagnostics.
 *
 * Return value semantics for run():
 *  - 0: success
 *  - non-zero: various error conditions during startup or runtime (missing files,
 *    parse failures, unrecoverable exceptions, etc.)
 */

#include "Application.hpp"
#include "LogManager.hpp"
#include "PppTypes.hpp"
#include "MwSlipDetector.hpp"
#include "IfPhaseSlipDetector.hpp"
#include "Namespaces.hpp"
#include "WlsEstimator.hpp"
#include "TimeUtils.hpp"
#include "TraceLogger.hpp"
#include "ProductCache.hpp"
#include "Coordinates.hpp"
#include <chrono>
#include <vector>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <cmath>
#include <limits>
#include <fmt/format.h>

/** @namespace GnssProcessor
*  @brief Top-level namespace for the GNSSProcessor project.
*
*  Contains models, parsers, filters and utilities for PPP and PPP-RTK
*  processing. Use nested namespaces (e.g. ::Model, ::Filter, ::IO, ::internal)
*  for logical grouping and to separate public API from implementation details.
*/
namespace GnssProcessor {

/**
 * @brief Construct an Application instance.
 *
 * @param config_path Path to the JSON configuration file. Defaults to
 * "config/gnss_config.json" when not provided.
 */
Application::Application(const std::filesystem::path& config_path)
    : config_path_(config_path) {
}

/**
 * @brief Run the GNSS processing application.
 *
 * This method performs:
 *  - Logging setup and configuration loading.
 *  - Validation of product and observation file paths.
 *  - Parsing of RINEX observations and initialization of the processing state.
 *  - Creation and initialization of core components: PppModel, MeasurementModel,
 *    KalmanFilter, WlsEstimator, MwSlipDetector and a TraceLogger.
 *  - Epoch loop that:
 *      - Groups observations by epoch and filters active constellations.
 *      - Computes modeled observations via PppModel.
 *      - Optionally runs a WLS position estimate and may seed the filter clock.
 *      - Predicts and updates the Kalman filter, handling ambiguity resets and
 *        relinearization when needed.
 *      - Emits per-epoch CSV traces for state, ambiguities and residuals.
 *
 * The method logs progress and returns an integer exit code:
 *  - 0 on success, non-zero on failure (missing files, parse failures, or other errors).
 *
 * @return int Exit code (0 = success).
 */
int Application::run() {
    LogManager log_manager;
    std::filesystem::path log_dir = "logs";
    log_manager.initialize(log_dir, LogLevel::Info);
    log_manager.getLogger("main_log")->info("Application started");

    GnssConfig config;
    if (!std::filesystem::exists(config_path_)) {
        log_manager.getLogger("main_log")->error("Configuration file not found: {}", config_path_.string());
        return 1;
    }
    if (!config.loadFromJson(config_path_, log_manager)) {
        log_manager.getLogger("main_log")->error("Failed to load JSON configuration from {}", config_path_.string());
        return 1;
    }
    log_manager.getLogger("main_log")->info("Configuration loaded from {}", config_path_.string());

    // Initialize product parser
    ProductParser product_parser(config, log_manager);

    // NOTE: do NOT call product_parser.loadProducts() here. Products depend on RINEX epoch info.
    // Pre-population of cache will happen after RinexParser parses and product_parser.loadProducts() is invoked.

    RinexParser rinex_parser(config, log_manager, product_parser);
    std::filesystem::path rinex_path = config.getObservationPath();
    if (rinex_path.empty() || !std::filesystem::exists(rinex_path)) {
        log_manager.getLogger("main_log")->error("Observation RINEX file not specified or not found: {}", rinex_path.string());
        return 1;
    }
    if (!rinex_parser.parse()) {
        log_manager.getLogger("main_log")->error("Failed to parse observation RINEX file: {}", rinex_path.string());
        return 1;
    }

    // Load external products (SP3/CLK/ATX/BIA/ERP, Klobuchar, Sun ephemeris).
    // Must be done after RinexParser::parse() so ProductParser can use epoch times.
    product_parser.loadProducts();

    // Pre-populate the cache (load once from the parser)
    {
        auto& cache = ProductCache::getInstance();
        if (!cache.loadProducts(product_parser, log_manager)) {
            log_manager.getLogger("main_log")->error("Failed to initialize product cache");
            return 1;
        }
        log_manager.getLogger("main_log")->info(
            "Product cache initialized: {} orbits, {} clocks loaded",
            cache.getOrbitCount(), cache.getClockCount()
        );
    }

    const auto& observations = rinex_parser.getObservations();
    if (observations.empty()) {
        log_manager.getLogger("main_log")->error("No observations parsed from RINEX file: {}", rinex_path.string());
        return 1;
    }
    log_manager.getLogger("main_log")->info("Parsed {} observations from RINEX file: {}", observations.size(), rinex_path.string());

    size_t core_state_size = config.getProcessingMode() == ProcessingMode::PPP ? 5 : 6;
    StateManager state_manager(core_state_size, log_manager, config);

    // Initial position will be set properly by ppp_model.initializeState() which applies antenna H/E/N offsets.
    // We just need a fallback position in case RINEX header has no valid position.
    Eigen::Vector3d fallback_position(0.0, 0.0, Constants::EARTH_RADIUS);
    auto approx_pos = rinex_parser.getHeader().approx_position;
    bool valid_pos = true;
    for (double val : approx_pos) {
        if (!std::isfinite(val) || std::abs(val) > 1e8) { valid_pos = false; break; }
    }
    
    Eigen::Vector3d initial_position = fallback_position;
    if (valid_pos && approx_pos != std::array<double,3>{0.0,0.0,0.0}) {
        Eigen::Vector3d marker_pos(approx_pos[0], approx_pos[1], approx_pos[2]);
        
        // Apply antenna H/E/N offset (from marker to ARP) to get antenna phase center position
        Eigen::Vector3d hen = rinex_parser.getAntennaDeltaHen();
        if (hen.norm() > 1e-6 && marker_pos.norm() > 1e3) {
            // Convert ENU offset to ECEF
            Coordinates::Llh ref_llh = Coordinates::ecefToLlh(marker_pos);
            // RINEX H/E/N: H=Up, E=East, N=North -> ENU order is (E, N, U) = (hen[1], hen[2], hen[0])
            Eigen::Vector3d enu_offset(hen(1), hen(2), hen(0));
            Eigen::Vector3d ecef_offset = Coordinates::enuToEcef(enu_offset, ref_llh);
            initial_position = marker_pos + ecef_offset;
            
            log_manager.getLogger("main_log")->info(
                "Applied antenna H/E/N offset ({:.4f}, {:.4f}, {:.4f}) m: marker ({:.3f}, {:.3f}, {:.3f}) -> ARP ({:.3f}, {:.3f}, {:.3f})",
                hen(0), hen(1), hen(2),
                marker_pos.x(), marker_pos.y(), marker_pos.z(),
                initial_position.x(), initial_position.y(), initial_position.z());
        } else {
            initial_position = marker_pos;
            log_manager.getLogger("main_log")->info(
                "Initialized position from RINEX marker (no/zero antenna offset): ({:.3f}, {:.3f}, {:.3f}) m",
                initial_position.x(), initial_position.y(), initial_position.z());
        }
        
        Eigen::VectorXd sv = state_manager.getStateVector();
        if (sv.size() >= state_manager.getPositionIndex() + 3) {
            sv.segment<3>(state_manager.getPositionIndex()) = initial_position;
            state_manager.setStateVector(sv);
        } else {
            log_manager.getLogger("main_log")->warn("State vector size {} too small to set position", sv.size());
            valid_pos = false;
        }
    }
    if (!valid_pos) {
        Eigen::VectorXd sv = state_manager.getStateVector();
        if (sv.size() >= state_manager.getPositionIndex() + 3) {
            sv.segment<3>(state_manager.getPositionIndex()) = fallback_position;
            state_manager.setStateVector(sv);
            log_manager.getLogger("main_log")->warn("Using fallback position: ({:.3f}, {:.3f}, {:.3f}) m",
                fallback_position.x(), fallback_position.y(), fallback_position.z());
        } else {
            log_manager.getLogger("main_log")->error("State vector too small to set fallback position");
            return 1;
        }
        initial_position = fallback_position;
    }

    PppModel ppp_model(config, product_parser, log_manager, rinex_parser, state_manager);
    MeasurementModel meas_model(state_manager, ppp_model, config, log_manager);
    KalmanFilter kalman_filter(state_manager, config, log_manager, ppp_model);
    WlsEstimator wls_estimator(config, state_manager, log_manager);

    MwSlipDetector mw_slip_detector(config, log_manager);
    IfPhaseSlipDetector if_slip_detector(config, log_manager);
    const bool if_slip_enabled = config.enableIfSlipDetector();

    // Enable MWWL tracing for cycle slip visualization
    mw_slip_detector.enableTracing(log_dir);
    if (if_slip_enabled) {
        if_slip_detector.enableTracing(log_dir);
    }

    log_manager.getLogger("main_log")->info("Processing components initialized, mode={}",
        config.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");

    PppState ppp_state;
    ppp_model.initializeState(ppp_state);
    state_manager.syncPppStateToStateVector(ppp_state);
    std::vector<PppObservation> modeled_obs;
    auto start_time = std::chrono::high_resolution_clock::now();
    double prev_tow = 0.0;
    bool first_epoch = true;

    const bool is_ppp_rtk = config.getProcessingMode() == ProcessingMode::PPP_RTK;

    // CSV traces for MATLAB plotting
    TraceLogger trace_logger(log_manager, log_dir, is_ppp_rtk);
    if (!trace_logger.isInitialized()) {
        // Continue even if CSV files couldn't be created; trace logging is best-effort.
        log_manager.getLogger("main_log")->warn("TraceLogger could not open CSV trace files in '{}'; continuing without CSV traces", log_dir.string());
    }

    const auto& active_systems = config.getActiveSystems();

    // Post-update 3D position sigma from the previous epoch.
    // The IF slip detector uses this to gate detection during pre-convergence.
    // Initialised to a large value so the first epoch is always gated (no
    // previous posterior covariance is available).
    double prev_pos_sigma_3d = std::numeric_limits<double>::max();

    for (size_t epoch_idx = 0; epoch_idx < observations.size(); ) {
        GnssTime current_time = observations[epoch_idx].time;
        const std::string epoch_tag = TimeUtils::formatGnssTime(current_time);
        const std::string epoch_prefix = "[" + epoch_tag + "] ";
        std::vector<RinexObservation> epoch_obs;
        while (epoch_idx < observations.size() && observations[epoch_idx].time.tow == current_time.tow) {
            std::string obs_system;
            switch (observations[epoch_idx].system) {
            case System::GPS: obs_system = "GPS"; break;
            case System::Galileo: obs_system = "Galileo"; break;
            case System::GLONASS: obs_system = "GLONASS"; break;
            case System::BeiDou: obs_system = "BeiDou"; break;
            case System::QZSS: obs_system = "QZSS"; break;
            case System::SBAS: obs_system = "SBAS"; break;
            default: obs_system = "Unknown"; break;
            }
            if (std::find(active_systems.begin(), active_systems.end(), obs_system) != active_systems.end()) {
                epoch_obs.push_back(observations[epoch_idx]);
            }
            ++epoch_idx;
        }
        if (epoch_obs.empty()) continue;

        state_manager.setStateEpochTime(current_time);

        double dt;
        bool gap_detected = false;
        if (first_epoch) {
            dt = rinex_parser.getHeader().interval.value_or(30.0);
			//first_epoch = false; // wait until after WLS seeding to clear first_epoch flag
			log_manager.getLogger("main_log")->info("{}First epoch, assuming dt = {}s", epoch_prefix, dt);
        } else {
            dt = current_time.tow - prev_tow;
            double expected_interval = rinex_parser.getHeader().interval.value_or(30.0);
            gap_detected = dt > expected_interval * 2.0; // heuristic: >2x indicates a gap
            if (std::abs(dt - expected_interval) > 1e-3) {
                log_manager.getLogger("main_log")->warn("{}Unexpected time step {}s (expected {}s)",
                    epoch_prefix, dt, expected_interval);
            }
        }
        prev_tow = current_time.tow;

        Eigen::VectorXd current_state = state_manager.getStateVector();
        try {
            state_manager.updatePppStateFromVector(current_state, ppp_state, state_manager);
        } catch (const std::exception& e) {
            log_manager.getLogger("main_log")->error("{}Failed to update ppp_state: {}", epoch_prefix, e.what());
            return 1;
        }

        if (!ppp_model.computeModels(epoch_obs, ppp_state, modeled_obs)) {
            log_manager.getLogger("main_log")->warn("{}Failed to compute models for epoch", epoch_prefix);
            continue;
        }

        // Diagnostic: Log satellite clock values for each modeled observation to detect jumps
        {
            std::string sat_clk_summary;
            for (const auto& obs : modeled_obs) {
                // Extract satellite clock from the modeled values (embedded in range calculation)
                // We can infer it from the difference between geometric range and modeled pseudorange
                sat_clk_summary += fmt::format("{}:r={:.1f}km ", obs.satellite, obs.range / 1000.0);
            }
            log_manager.getLogger("main_log")->debug(
                "{}Modeled {} sats: {}", epoch_prefix, modeled_obs.size(), sat_clk_summary);
        }

        const auto wls_result = wls_estimator.estimate(current_time, modeled_obs);
        if (wls_result.success() && wls_result.state.size() >= 4){
			constexpr double kClockJumpThreshSec = 0.00005; // 0.5 ms

            const double wls_clock_bias = wls_result.state(3);

            const Eigen::VectorXd sv0 = state_manager.getStateVector();
            const size_t clock_idx = state_manager.getClockIndex();
            const double kf_clock_bias = (sv0.size() > clock_idx) ? sv0(clock_idx) : 0.0;

            const double clock_jump_thresh_m = kClockJumpThreshSec * Constants::SPEED_OF_LIGHT;
            const double delta_clock_m = wls_clock_bias - kf_clock_bias;

            const double rms = wls_result.rms;
            const double max_abs = wls_result.max_abs;

            const bool jump_detected = std::abs(delta_clock_m) >= clock_jump_thresh_m;
			const bool should_seed = jump_detected || gap_detected || first_epoch; // seed kalman clock from wls clock on jump, gap, or first epoch

            if (should_seed && sv0.size() > clock_idx) {
                Eigen::VectorXd sv = sv0;
                sv(clock_idx) = wls_clock_bias;
                state_manager.setStateVector(sv);
                ppp_state.clock_bias = wls_clock_bias;
                log_manager.getLogger("main_log")->info(
                    "{}Seeded KF clock from WLS (jump): wls_clk={:.3f} m, kf_clk={:.3f} m, delta={:.3f} m",
                    epoch_prefix, wls_clock_bias, kf_clock_bias, delta_clock_m);
				// Always seed at first epoch
                if (first_epoch) {
                    first_epoch = false; // clear first_epoch flag after first seeding
				}
            } else {
                log_manager.getLogger("main_log")->trace(
                    "{}Skipped WLS clock seeding: wls_clk={:.3f} m, kf_clk={:.3f} m, delta={:.3f} m, jump_detected={}",
                    epoch_prefix, wls_clock_bias, kf_clock_bias, delta_clock_m, jump_detected);
            }

            // Only write expensive WLS trace when overall logging level is Debug.
            // Use the main logger's spdlog level to decide.
            if (auto main_logger = log_manager.getLogger("main_log")) {
                if (main_logger->level() == spdlog::level::debug) {
                    // Convert current GPS time to UTC-referenced GnssTime for consistent CSV traces (same as other traces).
                    double gps_secs = TimeUtils::continuousGpsSeconds(current_time);
                    double utc_secs = gps_secs - TimeUtils::LEAP_SECONDS;
                    int utc_week = static_cast<int>(std::floor(utc_secs / TimeUtils::SECONDS_IN_WEEK));
                    double utc_tow = utc_secs - static_cast<double>(utc_week) * TimeUtils::SECONDS_IN_WEEK;
                    if (utc_tow < 0.0) { utc_tow += TimeUtils::SECONDS_IN_WEEK; --utc_week; }
                    GnssTime utc_time{ utc_week, utc_tow };

                    trace_logger.logWls(utc_time,
                        wls_result,
                        wls_clock_bias,
                        kf_clock_bias,
                        delta_clock_m,
                        jump_detected,
                        should_seed);
                }
                else {
                    main_logger->trace("{}WLS trace skipped (log level != Debug)", epoch_prefix);
                }
            }
        }

        kalman_filter.predict(dt);

        try {
            state_manager.updatePppStateFromVector(state_manager.getStateVector(), ppp_state, state_manager);
        } catch (const std::exception& e) {
            log_manager.getLogger("main_log")->error("{}Failed to update ppp_state: {}", epoch_prefix, e.what());
            return 1;
        }

        // Choose a relinearization threshold based on whether phase measurements are present.
        // Phase: small threshold to keep Jacobians accurate. Code-only: allow a few meters.
        auto has_phase = [&modeled_obs]() {
            for (const auto& obs : modeled_obs) {
                for (const auto& kv : obs.observed) {
                    if (!kv.first.empty() && kv.first[0] == 'L') return true; // phase keys like L1C, L2W
                }
            }
            return false;
        };

		const double kPosRelinearThreshM = has_phase() ? 100.0 : 1000.0; // big for code+phase, bigger for code-only

        // Detect MW/LLI cycle slips once per epoch, before the relinearization loop.
        // Running this inside the loop would cause duplicate MWWL trace entries and
        // zero-delta MW values on subsequent passes (since last_mw_m_ is already updated).
        {
            const auto reset_keys = mw_slip_detector.detectLargeMwResets(modeled_obs);
            if (!reset_keys.empty()) {
                const double reset_std_m = config.getAmbiguityStd();
                for (const auto& key : reset_keys) {
                    state_manager.resetAmbiguity(key, reset_std_m);
                    
                    // Extract satellite ID from ambiguity key (format: "G01_L1C_L2W" -> "G01")
                    // and reset phase wind-up state to avoid 2π discontinuities
                    size_t underscore_pos = key.find('_');
                    if (underscore_pos != std::string::npos && underscore_pos >= 2) {
                        std::string sat_id = key.substr(0, underscore_pos);
                        ppp_model.resetPhaseWindUp(sat_id);
                        if_slip_detector.resetSatellite(sat_id);
                        log_manager.getLogger("main_log")->debug("{}Reset phase wind-up for {} (MW cycle slip detected)", epoch_prefix, sat_id);
                    }
                }
                log_manager.getLogger("main_log")->warn("{}Applied {} MW/LLI ambiguity resets", epoch_prefix, reset_keys.size());

                state_manager.updatePppStateFromVector(state_manager.getStateVector(), ppp_state, state_manager);
            }
        }

        // Detect IF phase cycle slips (between-satellite, between-epoch double differences).
        // Catches small slips (1-2 cycles) that the MW detector misses due to code noise.
        // Uses the post-update position sigma from the *previous* epoch to gate
        // detection.  Reading the predicted (pre-update) covariance of the
        // current epoch would include inflated process noise and the initial
        // large uncertainty, causing the detector to stay gated far longer than
        // necessary.
        if (if_slip_enabled) {
            const auto if_reset_keys = if_slip_detector.detect(modeled_obs, prev_pos_sigma_3d);
            if (!if_reset_keys.empty()) {
                const double reset_std_m = config.getAmbiguityStd();
                for (const auto& key : if_reset_keys) {
                    state_manager.resetAmbiguity(key, reset_std_m);
                    
                    size_t underscore_pos = key.find('_');
                    if (underscore_pos != std::string::npos && underscore_pos >= 2) {
                        std::string sat_id = key.substr(0, underscore_pos);
                        ppp_model.resetPhaseWindUp(sat_id);
                        mw_slip_detector.resetSatellite(sat_id);
                        log_manager.getLogger("main_log")->debug("{}Reset phase wind-up for {} (IF phase slip detected)", epoch_prefix, sat_id);
                    }
                }
                log_manager.getLogger("main_log")->warn("{}Applied {} IF phase ambiguity resets", epoch_prefix, if_reset_keys.size());

                state_manager.updatePppStateFromVector(state_manager.getStateVector(), ppp_state, state_manager);
            }
        }

        int relinear_pass = 0;
        bool need_relinear = true;
        bool need_update = true;
        EpochMeasurement applied_meas{};
        bool have_applied_meas = false;
        size_t carry_bad_residual_count = 0;
        while ((need_update || need_relinear) && relinear_pass < 10) {
            ++relinear_pass;
            need_update = false; // will be set true again if KF requests rebuild

            EpochMeasurement meas_update = meas_model.computeMeasurementUpdate(modeled_obs, state_manager.getStateVector());
            if (meas_update.innovations.size() == 0 || !meas_model.validateMeasurementUpdate(meas_update)) {
                log_manager.getLogger("main_log")->warn("{}Invalid or empty measurement update (pass {})",
                    epoch_prefix, relinear_pass);
                break;
            }

            // Carry forward the bad-residual count from previous rebuild passes
            // so the full-reset safety net can accumulate across passes.
            meas_update.bad_residual_count = carry_bad_residual_count;

            const Eigen::Vector3d old_pos = ppp_state.position;
            const double old_tropo = ppp_state.troposphere_zwd;

            const auto result = kalman_filter.update(meas_update);

            try {
                state_manager.updatePppStateFromVector(state_manager.getStateVector(), ppp_state, state_manager);
            } catch (const std::exception& e) {
                log_manager.getLogger("main_log")->error("{}Failed to update ppp_state after Kalman update: {}",
                    epoch_prefix, e.what());
                break;
            }

            if (result == KalmanFilter::UpdateResult::NeedsMeasurementRebuild) {
                // Preserve the bad-residual count for the next rebuild pass
                carry_bad_residual_count = meas_update.bad_residual_count;

                log_manager.getLogger("main_log")->debug(
                    "{}KF requested measurement rebuild (pass {}): recalculating innovations after ambiguity reset/outlier handling",
                    epoch_prefix, relinear_pass);
                
                // Reset phase wind-up and MW arc history for satellites that had ambiguities reset
                if (kalman_filter.wasCompleteReset()) {
                    // Complete filter reset: clear all phase wind-up state
                    ppp_model.resetAllPhaseWindUp();
                    if_slip_detector.resetAll();
                    log_manager.getLogger("main_log")->info(
                        "{}Reset all phase wind-up state (complete filter reset)", epoch_prefix);
                } else {
                    // Individual ambiguity resets: clear specific satellites
                    for (const auto& sat_id : kalman_filter.getLastResetSatellites()) {
                        ppp_model.resetPhaseWindUp(sat_id);
                        mw_slip_detector.resetSatellite(sat_id);
                        if_slip_detector.resetSatellite(sat_id);
                        log_manager.getLogger("main_log")->debug(
                            "{}Reset phase wind-up, MW and IF history for {} (KF ambiguity reset)", epoch_prefix, sat_id);
                    }
                }
                
                 need_update = true;
                 continue;
            }

            if (result == KalmanFilter::UpdateResult::Applied) {
                applied_meas = meas_update;
                have_applied_meas = true;
            }

            if (ppp_state.position.norm() < 1e-6 || !ppp_state.position.allFinite()) {
                log_manager.getLogger("main_log")->warn("{}Invalid position after Kalman update, reverting to initial position", epoch_prefix);
                ppp_state.position = initial_position;
                Eigen::VectorXd sv = state_manager.getStateVector();
                sv.segment<3>(state_manager.getPositionIndex()) = initial_position;
                state_manager.setStateVector(sv);
            }

            const double pos_jump = (ppp_state.position - old_pos).norm();

            log_manager.getLogger("main_log")->trace(
                "{}Post-update (pass {}): pos_jump={:.4f} m",
                epoch_prefix, relinear_pass, pos_jump);

            need_relinear = (pos_jump > kPosRelinearThreshM);
        }

        // --- Geometry relinearization pass ---
        // When enabled, re-run computeModels() at the updated position so that
        // satellite geometry (orbits, clocks, LOS, corrections) is recomputed
        // at the post-update linearization point.  Then perform one additional
        // KF measurement update using the fresh geometry.
        if (config.relinearizeGeometry() && have_applied_meas) {
            try {
                state_manager.updatePppStateFromVector(state_manager.getStateVector(), ppp_state, state_manager);
            } catch (const std::exception& e) {
                log_manager.getLogger("main_log")->error("{}Relinearize: failed to sync ppp_state: {}", epoch_prefix, e.what());
            }

            std::vector<PppObservation> relinear_obs;
            if (ppp_model.computeModels(epoch_obs, ppp_state, relinear_obs) && !relinear_obs.empty()) {
                EpochMeasurement relinear_meas = meas_model.computeMeasurementUpdate(relinear_obs, state_manager.getStateVector());
                if (relinear_meas.innovations.size() > 0 && meas_model.validateMeasurementUpdate(relinear_meas)) {
                    auto relinear_result = kalman_filter.update(relinear_meas);
                    if (relinear_result == KalmanFilter::UpdateResult::Applied) {
                        applied_meas = relinear_meas;
                        try {
                            state_manager.updatePppStateFromVector(state_manager.getStateVector(), ppp_state, state_manager);
                        } catch (const std::exception& e) {
                            log_manager.getLogger("main_log")->error("{}Relinearize: failed to sync after update: {}", epoch_prefix, e.what());
                        }
                        log_manager.getLogger("main_log")->debug("{}Geometry relinearization applied", epoch_prefix);
                    } else {
                        log_manager.getLogger("main_log")->debug("{}Geometry relinearization update not applied (result={})",
                            epoch_prefix, static_cast<int>(relinear_result));
                    }
                }
            } else {
                log_manager.getLogger("main_log")->debug("{}Geometry relinearization: computeModels failed or empty", epoch_prefix);
            }
        }

        // Store the post-update 3D position sigma for the IF slip detector on
        // the next epoch.  This uses the posterior (converged) covariance which
        // accurately reflects how well the position is known, unlike the
        // predicted covariance which is inflated by process noise.
        {
            const Eigen::MatrixXd P = state_manager.getCovariance();
            const size_t pi = state_manager.getPositionIndex();
            if (P.rows() > static_cast<Eigen::Index>(pi + 2) &&
                P.cols() > static_cast<Eigen::Index>(pi + 2)) {
                const double var_x = P(pi, pi);
                const double var_y = P(pi + 1, pi + 1);
                const double var_z = P(pi + 2, pi + 2);
                if (std::isfinite(var_x) && std::isfinite(var_y) && std::isfinite(var_z) &&
                    var_x >= 0.0 && var_y >= 0.0 && var_z >= 0.0) {
                    prev_pos_sigma_3d = std::sqrt(var_x + var_y + var_z);
                }
            }
        }

        if (config.getProcessingMode() == ProcessingMode::PPP) {
            if (config.estimatePhaseClock()) {
                log_manager.getLogger("main_log")->info("{}Position ({:.3f}, {:.3f}, {:.3f}) m, Clock {:.3f} m, PhaseClk {:.3f} m, Tropo {:.3f} m, mode=PPP",
                    epoch_prefix,
                    ppp_state.position.x(), ppp_state.position.y(), ppp_state.position.z(),
                    ppp_state.clock_bias, ppp_state.phase_clock_offset, ppp_state.troposphere_zwd);
            } else {
                log_manager.getLogger("main_log")->info("{}Position ({:.3f}, {:.3f}, {:.3f}) m, Clock {:.3f} m, Tropo {:.3f} m, mode=PPP",
                    epoch_prefix,
                    ppp_state.position.x(), ppp_state.position.y(), ppp_state.position.z(),
                    ppp_state.clock_bias, ppp_state.troposphere_zwd);
            }
        } else {
            if (config.estimatePhaseClock()) {
                log_manager.getLogger("main_log")->info("{}Position ({:.3f}, {:.3f}, {:.3f}) m, Clock {:.3f} m, PhaseClk {:.3f} m, Tropo {:.3f} m, Iono {:.3f} m, mode=PPP-RTK",
                    epoch_prefix,
                    ppp_state.position.x(), ppp_state.position.y(), ppp_state.position.z(),
                    ppp_state.clock_bias, ppp_state.phase_clock_offset, ppp_state.troposphere_zwd, ppp_state.ionosphere_zenith_delay);
            } else {
                log_manager.getLogger("main_log")->info("{}Position ({:.3f}, {:.3f}, {:.3f}) m, Clock {:.3f} m, Tropo {:.3f} m, Iono {:.3f} m, mode=PPP-RTK",
                    epoch_prefix,
                    ppp_state.position.x(), ppp_state.position.y(), ppp_state.position.z(),
                    ppp_state.clock_bias, ppp_state.troposphere_zwd, ppp_state.ionosphere_zenith_delay);
            }
        }

        // ---- Per-epoch state/ambiguity/residual traces for MATLAB plotting ----
        // Convert GPS GnssTime `current_time` to an UTC-referenced GnssTime for CSV traces.
        // Use TimeUtils to build continuous GPS seconds and use configured leap-seconds.
        {
            // GPS seconds since GPS epoch
            double gps_secs = TimeUtils::continuousGpsSeconds(current_time);

            // Use configured leap-seconds (kept in GnssConfig) to convert GPS -> UTC
            double utc_secs = gps_secs - TimeUtils::LEAP_SECONDS;

            // Build UTC GnssTime (week + tow). Keep tow in [0, 604800).
            int utc_week = static_cast<int>(std::floor(utc_secs / TimeUtils::SECONDS_IN_WEEK));
            double utc_tow = utc_secs - static_cast<double>(utc_week) * TimeUtils::SECONDS_IN_WEEK;
            if (utc_tow < 0.0) { utc_tow += TimeUtils::SECONDS_IN_WEEK; --utc_week; }

            GnssTime utc_time{ utc_week, utc_tow };

            // Use UTC time for CSV traces
            trace_logger.logState(utc_time, state_manager);
            trace_logger.logAmbiguities(utc_time, state_manager);
            if (have_applied_meas) {
                trace_logger.logResiduals(utc_time, applied_meas);
            }
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    log_manager.getLogger("main_log")->info("Processing completed in {} ms, mode={}", duration,
        config.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");

    return 0;
}

} // namespace GnssProcessor