#include "Application.hpp"
#include "LogManager.hpp"
#include "PppTypes.hpp"
#include "MwSlipDetector.hpp"
#include "WlsEstimator.hpp"
#include "TimeUtils.hpp"
#include <chrono>
#include <vector>
#include <algorithm>
#include <iostream>

namespace GnssProcessor {

Application::Application(const std::filesystem::path& config_path)
    : config_path_(config_path) {
}

int Application::run() {
    LogManager log_manager;
    std::filesystem::path log_dir = "logs";
    log_manager.initialize(log_dir, LogLevel::Debug);
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

    ProductParser product_parser(config, log_manager);
    std::vector<std::filesystem::path> sp3_paths = product_parser.getOrbitPaths();
    if (sp3_paths.empty()) {
        log_manager.getLogger("main_log")->error("No SP3 file paths specified in configuration");
        return 1;
    }
    for (const auto& path : sp3_paths) {
        if (!std::filesystem::exists(path)) {
            log_manager.getLogger("main_log")->error("SP3 file not found: {}", path.string());
            return 1;
        }
    }
    log_manager.getLogger("main_log")->info("Found {} SP3 files", sp3_paths.size());

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
    const auto& observations = rinex_parser.getObservations();
    if (observations.empty()) {
        log_manager.getLogger("main_log")->error("No observations parsed from RINEX file: {}", rinex_path.string());
        return 1;
    }
    log_manager.getLogger("main_log")->info("Parsed {} observations from RINEX file: {}", observations.size(), rinex_path.string());

    size_t core_state_size = config.getProcessingMode() == ProcessingMode::PPP ? 5 : 6;
    StateManager state_manager(core_state_size, log_manager, config);

    Eigen::Vector3d initial_position(0.0, 0.0, 6371000.0);
    auto approx_pos = rinex_parser.getHeader().approx_position;
    bool valid_pos = true;
    for (double val : approx_pos) {
        if (!std::isfinite(val) || std::abs(val) > 1e8) { valid_pos = false; break; }
    }
    if (valid_pos && approx_pos != std::array<double,3>{0.0,0.0,0.0}) {
        initial_position = Eigen::Vector3d(approx_pos[0], approx_pos[1], approx_pos[2]);
        Eigen::VectorXd sv = state_manager.getStateVector();
        if (sv.size() >= state_manager.getPositionIndex() + 3) {
            sv.segment<3>(state_manager.getPositionIndex()) = initial_position;
            state_manager.setStateVector(sv);
            log_manager.getLogger("main_log")->info("Initialized state position from RINEX header: ({:.3f}, {:.3f}, {:.3f}) m",
                initial_position.x(), initial_position.y(), initial_position.z());
        } else {
            log_manager.getLogger("main_log")->warn("State vector size {} too small to set position", sv.size());
            valid_pos = false;
        }
    }
    if (!valid_pos) {
        Eigen::VectorXd sv = state_manager.getStateVector();
        if (sv.size() >= state_manager.getPositionIndex() + 3) {
            sv.segment<3>(state_manager.getPositionIndex()) = initial_position;
            state_manager.setStateVector(sv);
            log_manager.getLogger("main_log")->warn("Using fallback position: ({:.3f}, {:.3f}, {:.3f}) m",
                initial_position.x(), initial_position.y(), initial_position.z());
        } else {
            log_manager.getLogger("main_log")->error("State vector too small to set fallback position");
            return 1;
        }
    }

    PppModel ppp_model(config, product_parser, log_manager, rinex_parser, state_manager);
    MeasurementModel meas_model(state_manager, ppp_model, config, log_manager);
    KalmanFilter kalman_filter(state_manager, config, log_manager, ppp_model);
    WlsEstimator wls_estimator(config, state_manager, log_manager);

    MwSlipDetector mw_slip_detector(config, log_manager);

    log_manager.getLogger("main_log")->info("Processing components initialized, mode={}",
        config.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");

    PppState ppp_state;
    ppp_model.initializeState(ppp_state);
    state_manager.syncPppStateToStateVector(ppp_state);
    std::vector<PppObservation> modeled_obs;
    auto start_time = std::chrono::high_resolution_clock::now();
    double prev_tow = 0.0;
    bool first_epoch = true;

    const auto& active_systems = config.getActiveSystems();

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
        if (first_epoch) {
            dt = rinex_parser.getHeader().interval.value_or(30.0);
            first_epoch = false;
        } else {
            dt = current_time.tow - prev_tow;
            double expected_interval = rinex_parser.getHeader().interval.value_or(30.0);
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

        const auto wls_result = wls_estimator.estimate(current_time, modeled_obs);
        if (wls_result.success() && wls_result.state.size() >= 4) {
            constexpr double kClockJumpThreshSec = 0.0005;
            constexpr double kGoodWlsRmsM = 3.0;
            constexpr double kGoodWlsMaxAbsM = 10.0;
            constexpr double kMaxNonJumpSeedDeltaM = 500.0;

            const double wls_clock_bias = wls_result.state(3);

            const Eigen::VectorXd sv0 = state_manager.getStateVector();
            const size_t clock_idx = state_manager.getClockIndex();
            const double kf_clock_bias = (sv0.size() > clock_idx) ? sv0(clock_idx) : 0.0;

            const double clock_jump_thresh_m = kClockJumpThreshSec * Constants::SPEED_OF_LIGHT;
            const double delta_clock_m = wls_clock_bias - kf_clock_bias;

            const double rms = wls_result.rms;
            const double max_abs = wls_result.max_abs;

            const bool jump_detected = std::abs(delta_clock_m) >= clock_jump_thresh_m;
            const bool wls_quality_good = std::isfinite(rms) && rms <= kGoodWlsRmsM &&
                                          std::isfinite(max_abs) && max_abs <= kGoodWlsMaxAbsM;
            const bool delta_reasonable = std::abs(delta_clock_m) <= kMaxNonJumpSeedDeltaM;

            const bool should_seed = jump_detected || (wls_quality_good && delta_reasonable);

            if (should_seed && sv0.size() > clock_idx) {
                Eigen::VectorXd sv = sv0;
                sv(clock_idx) = wls_clock_bias;
                state_manager.setStateVector(sv);
                ppp_state.clock_bias = wls_clock_bias;

                log_manager.getLogger("main_log")->info(
                    "{}Seeded KF clock from WLS: wls_clk={:.3f} m, kf_clk={:.3f} m, delta={:.3f} m, jump_detected={}, wls_rms={:.3f} m, wls_max_abs={:.3f} m",
                    epoch_prefix,
                    wls_clock_bias, kf_clock_bias, delta_clock_m,
                    jump_detected, rms, max_abs);
            } else {
                log_manager.getLogger("main_log")->debug(
                    "{}Skipped WLS clock seeding: wls_clk={:.3f} m, kf_clk={:.3f} m, delta={:.3f} m, jump_detected={}, wls_rms={:.3f} m, wls_max_abs={:.3f} m",
                    epoch_prefix,
                    wls_clock_bias, kf_clock_bias, delta_clock_m,
                    jump_detected, rms, max_abs);
            }
        }

        kalman_filter.predict(dt);

        try {
            state_manager.updatePppStateFromVector(state_manager.getStateVector(), ppp_state, state_manager);
        } catch (const std::exception& e) {
            log_manager.getLogger("main_log")->error("{}Failed to update ppp_state: {}", epoch_prefix, e.what());
            return 1;
        }

        constexpr double kPosRelinearThreshM  = 0.05;
        constexpr double kTropoRelinearThreshM = 0.01;

        int relinear_pass = 0;
        bool need_relinear = true;
        while (need_relinear && relinear_pass < 2) {
            ++relinear_pass;

            if (!ppp_model.computeModels(epoch_obs, ppp_state, modeled_obs)) {
                log_manager.getLogger("main_log")->warn("{}computeModels failed (pass {})", epoch_prefix, relinear_pass);
                break;
            }

            {
                const auto reset_keys = mw_slip_detector.detectLargeMwResets(modeled_obs);
                if (!reset_keys.empty()) {
                    const double reset_std_m = config.getAmbiguityStd();
                    for (const auto& key : reset_keys) {
                        state_manager.resetAmbiguity(key, reset_std_m);
                    }
                    log_manager.getLogger("main_log")->warn(
                        "{}Applied {} MW/LLI ambiguity resets (iter {})",
                        epoch_prefix, reset_keys.size(), relinear_pass);

                    state_manager.updatePppStateFromVector(state_manager.getStateVector(), ppp_state, state_manager);
                }
            }

            EpochMeasurement meas_update = meas_model.computeMeasurementUpdate(modeled_obs, state_manager.getStateVector());
            if (meas_update.innovations.size() == 0 || !meas_model.validateMeasurementUpdate(meas_update)) {
                log_manager.getLogger("main_log")->warn("{}Invalid or empty measurement update (pass {})",
                    epoch_prefix, relinear_pass);
                break;
            }

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
                log_manager.getLogger("main_log")->debug(
                    "{}KF requested measurement rebuild (pass {}): recalculating innovations after ambiguity reset/outlier handling",
                    epoch_prefix, relinear_pass);
                continue;
            }

            if (ppp_state.position.norm() < 1e-6 || !ppp_state.position.allFinite()) {
                log_manager.getLogger("main_log")->warn("{}Invalid position after Kalman update, reverting to initial position", epoch_prefix);
                ppp_state.position = initial_position;
                Eigen::VectorXd sv = state_manager.getStateVector();
                sv.segment<3>(state_manager.getPositionIndex()) = initial_position;
                state_manager.setStateVector(sv);
            }

            const double pos_jump   = (ppp_state.position - old_pos).norm();
            const double tropo_jump = std::abs(ppp_state.troposphere_zwd - old_tropo);

            log_manager.getLogger("main_log")->trace(
                "{}Post-update (pass {}): pos_jump={:.4f} m, tropo_jump={:.4f} m",
                epoch_prefix, relinear_pass, pos_jump, tropo_jump);

            need_relinear = (pos_jump > kPosRelinearThreshM) || (tropo_jump > kTropoRelinearThreshM);
        }

        if (config.getProcessingMode() == ProcessingMode::PPP) {
            log_manager.getLogger("main_log")->info("{}Position ({:.3f}, {:.3f}, {:.3f}) m, Clock {:.3f} m, Tropo {:.3f} m, mode=PPP",
                epoch_prefix,
                ppp_state.position.x(), ppp_state.position.y(), ppp_state.position.z(),
                ppp_state.clock_bias, ppp_state.troposphere_zwd);
        } else {
            log_manager.getLogger("main_log")->info("{}Position ({:.3f}, {:.3f}, {:.3f}) m, Clock {:.3f} m, Tropo {:.3f} m, Iono {:.3f} m, mode=PPP-RTK",
                epoch_prefix,
                ppp_state.position.x(), ppp_state.position.y(), ppp_state.position.z(),
                ppp_state.clock_bias, ppp_state.troposphere_zwd, ppp_state.ionosphere_zenith_delay);
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    log_manager.getLogger("main_log")->info("Processing completed in {} ms, mode={}", duration,
        config.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");

    return 0;
}

} // namespace GnssProcessor