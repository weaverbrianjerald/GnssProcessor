/**
 * @file Application.cpp
 * @brief Top-level application driver for GNSS processing.
 *
 * Implements the Application class which orchestrates initialization of
 * configuration, logging and processing components (product parser, RINEX
 * parser, PPP model, measurement model, Kalman filter, WLS estimator, etc.)
 * and runs the epoch-by-epoch processing loop.
 *
 * The pipeline is decomposed into focused private helpers called from run():
 *  - initLoggingAndConfig()      – logging setup and JSON configuration load.
 *  - loadInputData()             – RINEX parsing and product cache population.
 *  - initializePosition()        – filter state seeding from RINEX header.
 *  - collectEpochObservations()  – per-epoch observation grouping and filtering.
 *  - handleWlsSeeding()          – WLS estimation and KF clock seeding.
 *  - detectCycleSlips()          – MW and IF phase slip detection / resets.
 *  - runFilterUpdateLoop()       – iterative KF measurement update loop.
 *  - runGeometryRelinearization()– optional second-pass geometry update.
 *  - updatePositionSigma()       – posterior 3D position uncertainty extraction.
 *  - logEpochSummary()           – per-epoch state logging.
 *  - writeEpochTraces()          – CSV trace output for offline analysis.
 *
 * Return value semantics for run():
 *  - 0: success
 *  - non-zero: various error conditions during startup or runtime.
 */

#include "Application.hpp"
#include "TimeUtils.hpp"
#include "ProductCache.hpp"
#include "Coordinates.hpp"
#include "ProductResolver.hpp"
#include "NtripClient.hpp"
#include "RtcmStreamReader.hpp"
#include "AmbiguityResolver.hpp"
#include <chrono>
#include <fstream>
#include <vector>
#include <algorithm>
#include <cmath>
#include <limits>
#include <fmt/format.h>

namespace GnssProcessor {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Application::Application(const std::filesystem::path& config_path)
    : config_path_(config_path) {
}

// ---------------------------------------------------------------------------
// GPS -> UTC helper (used by trace logging)
// ---------------------------------------------------------------------------

GnssTime Application::gpsToUtcTime(const GnssTime& gps_time) {
    const double gps_secs = TimeUtils::continuousGpsSeconds(gps_time);
    const double utc_secs = gps_secs - TimeUtils::LEAP_SECONDS;
    int utc_week = static_cast<int>(std::floor(utc_secs / TimeUtils::SECONDS_IN_WEEK));
    double utc_tow = utc_secs - static_cast<double>(utc_week) * TimeUtils::SECONDS_IN_WEEK;
    if (utc_tow < 0.0) { utc_tow += TimeUtils::SECONDS_IN_WEEK; --utc_week; }
    return GnssTime{ utc_week, utc_tow };
}

// ---------------------------------------------------------------------------
// Phase 1: Logging & configuration
// ---------------------------------------------------------------------------

bool Application::initLoggingAndConfig() {
    log_manager_.initialize(log_dir_, LogLevel::Info);
    log_manager_.getLogger("main_log")->info("Application started");

    if (!std::filesystem::exists(config_path_)) {
        log_manager_.getLogger("main_log")->error("Configuration file not found: {}", config_path_.string());
        return false;
    }
    if (!config_.loadFromJson(config_path_, log_manager_)) {
        log_manager_.getLogger("main_log")->error("Failed to load JSON configuration from {}", config_path_.string());
        return false;
    }
    log_manager_.getLogger("main_log")->info("Configuration loaded from {}", config_path_.string());
    return true;
}

// ---------------------------------------------------------------------------
// Phase 1b: Resolve (download/cache) product and RINEX data
// ---------------------------------------------------------------------------

bool Application::resolveData() {
    const auto& dl = config_.getDownloadConfig();
    if (!dl.enabled) {
        log_manager_.getLogger("main_log")->info("Auto data download disabled; using manually specified paths");
        return true;
    }

    // Map GnssConfig::DownloadConfig -> ProductResolver::DownloadConfig
    ProductResolver::DownloadConfig rc;
    rc.enabled           = dl.enabled;
    rc.data_root         = dl.data_root;
    rc.product_source    = dl.product_source;
    rc.product_type      = dl.product_type;
    rc.netrc_path        = dl.netrc_path;
    rc.station           = dl.station;
    rc.station_country   = dl.station_country;
    rc.year              = dl.year;
    rc.doy               = dl.doy;
    rc.download_rinex    = dl.download_rinex;
    rc.download_products = dl.download_products;
    rc.atx_path          = dl.atx_path;
    rc.nc_path           = dl.nc_path;
    rc.crx2rnx_path      = dl.crx2rnx_path;

    ProductResolver resolver(rc, log_manager_);
    ProductResolver::ResolvedPaths resolved;

    if (!resolver.resolve(resolved)) {
        log_manager_.getLogger("main_log")->error("Data resolution failed");
        return false;
    }

    // Apply resolved paths back into GnssConfig so the rest of the pipeline
    // picks them up transparently.
    if (!resolved.product_paths.empty()) {
        config_.setProductPaths(resolved.product_paths);
        log_manager_.getLogger("main_log")->info(
            "Auto-resolved {} product files into config", resolved.product_paths.size());
    }
    if (!resolved.navigation_path.empty()) {
        config_.setNavigationPath(resolved.navigation_path);
        log_manager_.getLogger("main_log")->info(
            "Auto-resolved navigation: {}", resolved.navigation_path.string());
    }
    if (!resolved.observation_path.empty()) {
        config_.setObservationPath(resolved.observation_path);
        log_manager_.getLogger("main_log")->info(
            "Auto-resolved observation RINEX: {}", resolved.observation_path.string());
    }

    return true;
}

// ---------------------------------------------------------------------------
// Phase 2: Input data (RINEX + products + cache)
// ---------------------------------------------------------------------------

bool Application::loadInputData() {
    product_parser_ = std::make_unique<ProductParser>(config_, log_manager_);

    // NOTE: do NOT call loadProducts() here. Products depend on RINEX epoch info.
    rinex_parser_ = std::make_unique<RinexParser>(config_, log_manager_, *product_parser_);
    std::filesystem::path rinex_path = config_.getObservationPath();
    if (rinex_path.empty() || !std::filesystem::exists(rinex_path)) {
        log_manager_.getLogger("main_log")->error("Observation RINEX file not specified or not found: {}", rinex_path.string());
        return false;
    }
    if (!rinex_parser_->parse()) {
        log_manager_.getLogger("main_log")->error("Failed to parse observation RINEX file: {}", rinex_path.string());
        return false;
    }

    // Load external products (SP3/CLK/ATX/BIA/ERP, Klobuchar, Sun ephemeris).
    // Must be done after RinexParser::parse() so ProductParser can use epoch times.
    product_parser_->loadProducts();

    // Pre-populate the product cache (load once from the parser).
    auto& cache = ProductCache::getInstance();
    if (!cache.loadProducts(*product_parser_, log_manager_)) {
        log_manager_.getLogger("main_log")->error("Failed to initialize product cache");
        return false;
    }
    log_manager_.getLogger("main_log")->info(
        "Product cache initialized: {} orbits, {} clocks loaded",
        cache.getOrbitCount(), cache.getClockCount());

    const auto& observations = rinex_parser_->getObservations();
    if (observations.empty()) {
        log_manager_.getLogger("main_log")->error("No observations parsed from RINEX file: {}", rinex_path.string());
        return false;
    }
    log_manager_.getLogger("main_log")->info("Parsed {} observations from RINEX file: {}",
        observations.size(), rinex_path.string());
    return true;
}

// ---------------------------------------------------------------------------
// Phase 3: Seed filter position from RINEX header
// ---------------------------------------------------------------------------

bool Application::initializePosition() {
    const Eigen::Vector3d fallback_position(0.0, 0.0, Constants::EARTH_RADIUS);
    auto approx_pos = rinex_parser_->getHeader().approx_position;
    bool valid_pos = true;
    for (double val : approx_pos) {
        if (!std::isfinite(val) || std::abs(val) > 1e8) { valid_pos = false; break; }
    }

    initial_position_ = fallback_position;
    if (valid_pos && approx_pos != std::array<double,3>{0.0,0.0,0.0}) {
        Eigen::Vector3d marker_pos(approx_pos[0], approx_pos[1], approx_pos[2]);

        // Apply antenna H/E/N offset (from marker to ARP) to get antenna phase center position.
        Eigen::Vector3d hen = rinex_parser_->getAntennaDeltaHen();
        if (hen.norm() > 1e-6 && marker_pos.norm() > 1e3) {
            Coordinates::Llh ref_llh = Coordinates::ecefToLlh(marker_pos);
            // RINEX H/E/N: H=Up, E=East, N=North -> ENU order is (E, N, U) = (hen[1], hen[2], hen[0])
            Eigen::Vector3d enu_offset(hen(1), hen(2), hen(0));
            Eigen::Vector3d ecef_offset = Coordinates::enuToEcef(enu_offset, ref_llh);
            initial_position_ = marker_pos + ecef_offset;

            log_manager_.getLogger("main_log")->info(
                "Applied antenna H/E/N offset ({:.4f}, {:.4f}, {:.4f}) m: marker ({:.3f}, {:.3f}, {:.3f}) -> ARP ({:.3f}, {:.3f}, {:.3f})",
                hen(0), hen(1), hen(2),
                marker_pos.x(), marker_pos.y(), marker_pos.z(),
                initial_position_.x(), initial_position_.y(), initial_position_.z());
        } else {
            initial_position_ = marker_pos;
            log_manager_.getLogger("main_log")->info(
                "Initialized position from RINEX marker (no/zero antenna offset): ({:.3f}, {:.3f}, {:.3f}) m",
                initial_position_.x(), initial_position_.y(), initial_position_.z());
        }

        Eigen::VectorXd sv = state_manager_->getStateVector();
        if (sv.size() >= static_cast<Eigen::Index>(state_manager_->getPositionIndex() + 3)) {
            sv.segment<3>(state_manager_->getPositionIndex()) = initial_position_;
            state_manager_->setStateVector(sv);
        } else {
            log_manager_.getLogger("main_log")->warn("State vector size {} too small to set position", sv.size());
            valid_pos = false;
        }
    }

    if (!valid_pos) {
        Eigen::VectorXd sv = state_manager_->getStateVector();
        if (sv.size() >= static_cast<Eigen::Index>(state_manager_->getPositionIndex() + 3)) {
            sv.segment<3>(state_manager_->getPositionIndex()) = fallback_position;
            state_manager_->setStateVector(sv);
            log_manager_.getLogger("main_log")->warn("Using fallback position: ({:.3f}, {:.3f}, {:.3f}) m",
                fallback_position.x(), fallback_position.y(), fallback_position.z());
        } else {
            log_manager_.getLogger("main_log")->error("State vector too small to set fallback position");
            return false;
        }
        initial_position_ = fallback_position;
    }
    return true;
}

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
// Epoch helper: WLS estimation & KF clock seeding
// ---------------------------------------------------------------------------

void Application::handleWlsSeeding(const GnssTime& current_time,
                                   const std::string& epoch_prefix,
                                   const std::vector<PppObservation>& modeled_obs,
                                   bool gap_detected) {
    const auto wls_result = wls_estimator_->estimate(current_time, modeled_obs);
    if (!wls_result.success() || wls_result.state.size() < 4) return;

    constexpr double kClockJumpThreshSec = 0.00005; // 0.5 ms

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
        if (first_epoch_) {
            first_epoch_ = false;
        }
    } else {
        log_manager_.getLogger("main_log")->trace(
            "{}Skipped WLS clock seeding: wls_clk={:.3f} m, kf_clk={:.3f} m, delta={:.3f} m, jump_detected={}",
            epoch_prefix, wls_clock_bias, kf_clock_bias, delta_clock_m, jump_detected);
    }

    // Only write expensive WLS trace when overall logging level is Debug.
    if (auto main_logger = log_manager_.getLogger("main_log")) {
        if (main_logger->level() == spdlog::level::debug) {
            trace_logger_->logWls(gpsToUtcTime(current_time),
                wls_result, wls_clock_bias, kf_clock_bias, delta_clock_m,
                jump_detected, should_seed);
        } else {
            main_logger->trace("{}WLS trace skipped (log level != Debug)", epoch_prefix);
        }
    }
}

// ---------------------------------------------------------------------------
// Epoch helper: MW + IF cycle slip detection
// ---------------------------------------------------------------------------

namespace {
    /// Extract satellite ID from an ambiguity key (e.g. "G01_L1C_L2W" -> "G01").
    std::string satIdFromAmbiguityKey(const std::string& key) {
        size_t underscore_pos = key.find('_');
        if (underscore_pos != std::string::npos && underscore_pos >= 2)
            return key.substr(0, underscore_pos);
        return {};
    }
} // anonymous namespace

void Application::detectCycleSlips(const std::string& epoch_prefix,
                                   const std::vector<PppObservation>& modeled_obs) {
    // --- MW / LLI detection ---
    // Run once per epoch, before the relinearization loop, to avoid duplicate
    // MWWL trace entries and zero-delta MW values on subsequent passes.
    {
        const auto reset_keys = mw_slip_detector_->detectLargeMwResets(modeled_obs);
        if (!reset_keys.empty()) {
            const double reset_std_m = config_.getAmbiguityStd();
            for (const auto& key : reset_keys) {
                state_manager_->resetAmbiguity(key, reset_std_m);
                // Phase wind-up is NOT reset: the accumulated wind-up is consistent
                // with the modeled values used to re-seed the ambiguity.
                const std::string sat_id = satIdFromAmbiguityKey(key);
                if (!sat_id.empty()) {
                    if_slip_detector_->resetSatellite(sat_id);
                    log_manager_.getLogger("main_log")->debug("{}Reset IF history for {} (MW cycle slip detected)", epoch_prefix, sat_id);
                }
            }
            log_manager_.getLogger("main_log")->warn("{}Applied {} MW/LLI ambiguity resets", epoch_prefix, reset_keys.size());
            state_manager_->updatePppStateFromVector(state_manager_->getStateVector(), ppp_state_, *state_manager_);
        }
    }

    // --- IF phase slip detection ---
    // Catches small slips (1-2 cycles) that the MW detector misses due to code noise.
    // Uses the post-update position sigma from the *previous* epoch to gate detection.
    if (config_.enableIfSlipDetector()) {
        const auto if_reset_keys = if_slip_detector_->detect(modeled_obs, prev_pos_sigma_3d_);
        if (!if_reset_keys.empty()) {
            const double reset_std_m = config_.getAmbiguityStd();
            for (const auto& key : if_reset_keys) {
                state_manager_->resetAmbiguity(key, reset_std_m);
                const std::string sat_id = satIdFromAmbiguityKey(key);
                if (!sat_id.empty()) {
                    mw_slip_detector_->resetSatellite(sat_id);
                    log_manager_.getLogger("main_log")->debug("{}Reset MW history for {} (IF phase slip detected)", epoch_prefix, sat_id);
                }
            }
            log_manager_.getLogger("main_log")->warn("{}Applied {} IF phase ambiguity resets", epoch_prefix, if_reset_keys.size());
            state_manager_->updatePppStateFromVector(state_manager_->getStateVector(), ppp_state_, *state_manager_);
        }
    }
}

// ---------------------------------------------------------------------------
// Epoch helper: Iterative KF update / relinearization loop
// ---------------------------------------------------------------------------

void Application::runFilterUpdateLoop(const std::string& epoch_prefix,
                                      const std::vector<PppObservation>& modeled_obs,
                                      EpochMeasurement& applied_meas,
                                      bool& have_applied) {
    // Choose a relinearization threshold based on whether phase measurements are present.
    auto has_phase = [&modeled_obs]() {
        for (const auto& obs : modeled_obs) {
            for (const auto& kv : obs.observed) {
                if (!kv.first.empty() && kv.first[0] == 'L') return true;
            }
        }
        return false;
    };
    const double kPosRelinearThreshM = has_phase() ? 100.0 : 1000.0;

    int relinear_pass = 0;
    bool need_relinear = true;
    bool need_update = true;
    size_t carry_bad_residual_count = 0;
    std::set<std::string> carry_excluded_measurements;

    while ((need_update || need_relinear) && relinear_pass < 10) {
        ++relinear_pass;
        need_update = false;

        // In tightly-coupled mode, provide current INS attitude and specific
        // force to the measurement model so Doppler Jacobian rows include
        // attitude-error and accelerometer-bias partials.
        if (config_.tightlyCoupledImu() && imu_integrator_ && kalman_filter_) {
            const Eigen::VectorXd x = state_manager_->getStateVector();
            Eigen::Vector3d accel_bias = Eigen::Vector3d::Zero();
            if (state_manager_->hasImuBiases())
                accel_bias = x.segment<3>(state_manager_->getAccelBiasIndex());
            // Use a nominal gravity-subtracted specific force in body frame.
            // At rest, f_b ≈ C^T * (-g).  For moving platforms the last IMU
            // sample is a good approximation over the GNSS epoch interval.
            const Eigen::Vector3d g_e = ImuIntegrator::computeGravityEcef(
                x.segment<3>(state_manager_->getPositionIndex()));
            const Eigen::Matrix3d C = kalman_filter_->getAttitude();
            const Eigen::Vector3d f_b = C.transpose() * (-g_e) - accel_bias;
            meas_model_->setImuState(C, f_b);
        }

        EpochMeasurement meas_update = meas_model_->computeMeasurementUpdate(
            modeled_obs, state_manager_->getStateVector(), carry_excluded_measurements);
        if (meas_update.innovations.size() == 0 || !meas_model_->validateMeasurementUpdate(meas_update)) {
            log_manager_.getLogger("main_log")->warn("{}Invalid or empty measurement update (pass {})",
                epoch_prefix, relinear_pass);
            break;
        }

        // Carry forward the bad-residual count so the full-reset safety net accumulates across passes.
        meas_update.bad_residual_count = carry_bad_residual_count;

        const Eigen::Vector3d old_pos = ppp_state_.position;

        const auto result = kalman_filter_->update(meas_update);

        try {
            state_manager_->updatePppStateFromVector(state_manager_->getStateVector(), ppp_state_, *state_manager_);
        } catch (const std::exception& e) {
            log_manager_.getLogger("main_log")->error("{}Failed to update ppp_state after Kalman update: {}",
                epoch_prefix, e.what());
            break;
        }

        if (result == KalmanFilter::UpdateResult::NeedsMeasurementRebuild) {
            carry_bad_residual_count = meas_update.bad_residual_count;
            carry_excluded_measurements = meas_update.excluded_measurements;

            log_manager_.getLogger("main_log")->debug(
                "{}KF requested measurement rebuild (pass {}): recalculating innovations after ambiguity reset/outlier handling",
                epoch_prefix, relinear_pass);

            // Reset MW and IF slip detector history for satellites that had ambiguities reset.
            // Phase wind-up is NOT reset (see detectCycleSlips comment for rationale).
            if (kalman_filter_->wasCompleteReset()) {
                if_slip_detector_->resetAll();
                log_manager_.getLogger("main_log")->info(
                    "{}Reset IF slip detector state (complete filter reset)", epoch_prefix);
            } else {
                for (const auto& sat_id : kalman_filter_->getLastResetSatellites()) {
                    mw_slip_detector_->resetSatellite(sat_id);
                    if_slip_detector_->resetSatellite(sat_id);
                    log_manager_.getLogger("main_log")->debug(
                        "{}Reset MW and IF history for {} (KF ambiguity reset)", epoch_prefix, sat_id);
                }
            }

            need_update = true;
            continue;
        }

        if (result == KalmanFilter::UpdateResult::Applied) {
            applied_meas = meas_update;
            have_applied = true;
        }

        if (ppp_state_.position.norm() < 1e-6 || !ppp_state_.position.allFinite()) {
            log_manager_.getLogger("main_log")->warn("{}Invalid position after Kalman update, reverting to initial position", epoch_prefix);
            ppp_state_.position = initial_position_;
            Eigen::VectorXd sv = state_manager_->getStateVector();
            sv.segment<3>(state_manager_->getPositionIndex()) = initial_position_;
            state_manager_->setStateVector(sv);
        }

        const double pos_jump = (ppp_state_.position - old_pos).norm();
        log_manager_.getLogger("main_log")->trace(
            "{}Post-update (pass {}): pos_jump={:.4f} m",
            epoch_prefix, relinear_pass, pos_jump);

        need_relinear = (pos_jump > kPosRelinearThreshM);
    }
}

// ---------------------------------------------------------------------------
// Epoch helper: Geometry relinearization
// ---------------------------------------------------------------------------

void Application::runGeometryRelinearization(const std::string& epoch_prefix,
                                             const std::vector<RinexObservation>& epoch_obs,
                                             EpochMeasurement& applied_meas,
                                             bool have_applied) {
    if (!config_.relinearizeGeometry() || !have_applied) return;

    try {
        state_manager_->updatePppStateFromVector(state_manager_->getStateVector(), ppp_state_, *state_manager_);
    } catch (const std::exception& e) {
        log_manager_.getLogger("main_log")->error("{}Relinearize: failed to sync ppp_state: {}", epoch_prefix, e.what());
        return;
    }

    std::vector<PppObservation> relinear_obs;
    if (!ppp_model_->computeModels(epoch_obs, ppp_state_, relinear_obs) || relinear_obs.empty()) {
        log_manager_.getLogger("main_log")->debug("{}Geometry relinearization: computeModels failed or empty", epoch_prefix);
        return;
    }

    // Set IMU state for tightly-coupled Jacobian partials.
    if (config_.tightlyCoupledImu() && imu_integrator_ && kalman_filter_) {
        const Eigen::VectorXd x = state_manager_->getStateVector();
        Eigen::Vector3d accel_bias = Eigen::Vector3d::Zero();
        if (state_manager_->hasImuBiases())
            accel_bias = x.segment<3>(state_manager_->getAccelBiasIndex());
        const Eigen::Vector3d g_e = ImuIntegrator::computeGravityEcef(
            x.segment<3>(state_manager_->getPositionIndex()));
        const Eigen::Matrix3d C = kalman_filter_->getAttitude();
        const Eigen::Vector3d f_b = C.transpose() * (-g_e) - accel_bias;
        meas_model_->setImuState(C, f_b);
    }

    EpochMeasurement relinear_meas = meas_model_->computeMeasurementUpdate(
        relinear_obs, state_manager_->getStateVector());
    if (relinear_meas.innovations.size() == 0 || !meas_model_->validateMeasurementUpdate(relinear_meas)) return;

    auto relinear_result = kalman_filter_->update(relinear_meas);
    if (relinear_result == KalmanFilter::UpdateResult::Applied) {
        applied_meas = relinear_meas;
        try {
            state_manager_->updatePppStateFromVector(state_manager_->getStateVector(), ppp_state_, *state_manager_);
        } catch (const std::exception& e) {
            log_manager_.getLogger("main_log")->error("{}Relinearize: failed to sync after update: {}", epoch_prefix, e.what());
        }
        log_manager_.getLogger("main_log")->debug("{}Geometry relinearization applied", epoch_prefix);
    } else {
        log_manager_.getLogger("main_log")->debug("{}Geometry relinearization update not applied (result={})",
            epoch_prefix, static_cast<int>(relinear_result));
    }
}

// ---------------------------------------------------------------------------
// Epoch helper: Update stored 3D position sigma
// ---------------------------------------------------------------------------

void Application::updatePositionSigma() {
    const Eigen::MatrixXd P = state_manager_->getCovariance();
    const size_t pi = state_manager_->getPositionIndex();
    if (P.rows() > static_cast<Eigen::Index>(pi + 2) &&
        P.cols() > static_cast<Eigen::Index>(pi + 2)) {
        const double var_x = P(pi, pi);
        const double var_y = P(pi + 1, pi + 1);
        const double var_z = P(pi + 2, pi + 2);
        if (std::isfinite(var_x) && std::isfinite(var_y) && std::isfinite(var_z) &&
            var_x >= 0.0 && var_y >= 0.0 && var_z >= 0.0) {
            prev_pos_sigma_3d_ = std::sqrt(var_x + var_y + var_z);
        }
    }
}

// ---------------------------------------------------------------------------
// Epoch helper: Log epoch summary
// ---------------------------------------------------------------------------

void Application::logEpochSummary(const std::string& epoch_prefix) {
    const bool is_ppp = (config_.getProcessingMode() == ProcessingMode::PPP);
    const bool phase_clk = config_.estimatePhaseClock();
    const bool has_vel = config_.estimateVelocity();
    const auto& pos = ppp_state_.position;
    const auto& vel = ppp_state_.velocity;

    // Build optional velocity suffix.
    std::string vel_str;
    if (has_vel)
        vel_str = fmt::format(", Vel ({:.3f}, {:.3f}, {:.3f}) m/s", vel.x(), vel.y(), vel.z());

    if (is_ppp) {
        if (phase_clk) {
            log_manager_.getLogger("main_log")->info(
                "{}Position ({:.3f}, {:.3f}, {:.3f}) m, Clock {:.3f} m, PhaseClk {:.3f} m, Tropo {:.3f} m{}, mode=PPP",
                epoch_prefix, pos.x(), pos.y(), pos.z(),
                ppp_state_.clock_bias, ppp_state_.phase_clock_offset, ppp_state_.troposphere_zwd, vel_str);
        } else {
            log_manager_.getLogger("main_log")->info(
                "{}Position ({:.3f}, {:.3f}, {:.3f}) m, Clock {:.3f} m, Tropo {:.3f} m{}, mode=PPP",
                epoch_prefix, pos.x(), pos.y(), pos.z(),
                ppp_state_.clock_bias, ppp_state_.troposphere_zwd, vel_str);
        }
    } else {
        if (phase_clk) {
            log_manager_.getLogger("main_log")->info(
                "{}Position ({:.3f}, {:.3f}, {:.3f}) m, Clock {:.3f} m, PhaseClk {:.3f} m, Tropo {:.3f} m{}, IonoStates {}, mode=PPP-RTK",
                epoch_prefix, pos.x(), pos.y(), pos.z(),
                ppp_state_.clock_bias, ppp_state_.phase_clock_offset, ppp_state_.troposphere_zwd,
                vel_str, ppp_state_.slant_ionosphere.size());
        } else {
            log_manager_.getLogger("main_log")->info(
                "{}Position ({:.3f}, {:.3f}, {:.3f}) m, Clock {:.3f} m, Tropo {:.3f} m{}, IonoStates {}, mode=PPP-RTK",
                epoch_prefix, pos.x(), pos.y(), pos.z(),
                ppp_state_.clock_bias, ppp_state_.troposphere_zwd,
                vel_str, ppp_state_.slant_ionosphere.size());
        }
    }
}

// ---------------------------------------------------------------------------
// Epoch helper: Write CSV trace records
// ---------------------------------------------------------------------------

void Application::writeEpochTraces(const GnssTime& current_time,
                                   const EpochMeasurement& applied_meas,
                                   bool have_applied) {
    const GnssTime utc_time = gpsToUtcTime(current_time);
    trace_logger_->logState(utc_time, *state_manager_);
    trace_logger_->logAmbiguities(utc_time, *state_manager_);
    trace_logger_->logIonosphere(utc_time, *state_manager_);
    if (have_applied) {
        trace_logger_->logResiduals(utc_time, applied_meas);
    }
}

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------

int Application::run() {
// --- Phase 1: Logging & configuration ---
if (!initLoggingAndConfig()) return 1;

// --- Branch on application mode ---
if (config_.getAppMode() == AppMode::RealTime) {
    return runRealTime();
}

// --- Phase 1b: Resolve (download/cache) product and RINEX data ---
if (!resolveData()) return 1;

// --- Phase 2: Parse RINEX & load products ---
if (!loadInputData()) return 1;

// --- Phase 3: Create processing components ---
constexpr size_t core_state_size = 5; // Position(3) + Clock(1) + Tropo(1)
state_manager_ = std::make_unique<StateManager>(core_state_size, log_manager_, config_);

    if (!initializePosition()) return 1;

    ppp_model_       = std::make_unique<PppModel>(config_, *product_parser_, log_manager_, *rinex_parser_, *state_manager_);
    meas_model_      = std::make_unique<MeasurementModel>(*state_manager_, *ppp_model_, config_, log_manager_);
    kalman_filter_   = std::make_unique<KalmanFilter>(*state_manager_, config_, log_manager_, *ppp_model_);
    wls_estimator_   = std::make_unique<WlsEstimator>(config_, *state_manager_, log_manager_);
    mw_slip_detector_  = std::make_unique<MwSlipDetector>(config_, log_manager_);
    if_slip_detector_  = std::make_unique<IfPhaseSlipDetector>(config_, log_manager_);

    mw_slip_detector_->enableTracing(log_dir_);
    if (config_.enableIfSlipDetector()) {
        if_slip_detector_->enableTracing(log_dir_);
    }

    // IMU integrator (GNSS/INS coupled mode)
    if (config_.enableImu()) {
        imu_integrator_ = std::make_unique<ImuIntegrator>(config_, log_manager_);
        const auto& imu_path = config_.getImuConfig().imu_file_path;
        if (!imu_path.empty()) {
            if (!imu_integrator_->loadData(imu_path)) {
                log_manager_.getLogger("main_log")->warn(
                    "IMU enabled but failed to load data from {}; falling back to GNSS-only",
                    imu_path.string());
                imu_integrator_.reset();
            } else {
                const char* mode_str = config_.tightlyCoupledImu()
                    ? "tightly-coupled" : "loosely-coupled";
                log_manager_.getLogger("main_log")->info(
                    "IMU integration enabled ({}): {} samples loaded",
                    mode_str, imu_integrator_->sampleCount());
            }
        } else {
            log_manager_.getLogger("main_log")->warn("IMU enabled but no imu_file_path configured");
            imu_integrator_.reset();
        }
    }

    ppp_model_->initializeState(ppp_state_);
    state_manager_->syncPppStateToStateVector(ppp_state_);

    // Ambiguity resolution (MLAMBDA)
    if (config_.getArMode() > 0) {
        AmbiguityResolverConfig ar_config;
        ar_config.ar_mode            = config_.getArMode();
        ar_config.ratio_threshold    = config_.getArRatioThreshold();
        ar_config.elevation_mask_ar  = config_.getArElevationMask();
        ar_config.hold_variance      = config_.getArHoldVariance();
        ambiguity_resolver_ = std::make_unique<AmbiguityResolver>(ar_config, log_manager_);
        log_manager_.getLogger("main_log")->info(
            "Ambiguity resolution enabled: mode={}, ratio_thresh={:.1f}, elev_mask={:.1f} deg",
            ar_config.ar_mode, ar_config.ratio_threshold, ar_config.elevation_mask_ar);
    }

    const bool is_ppp_rtk = (config_.getProcessingMode() == ProcessingMode::PPP_RTK);
    trace_logger_ = std::make_unique<TraceLogger>(log_manager_, log_dir_, is_ppp_rtk);
    if (!trace_logger_->isInitialized()) {
        log_manager_.getLogger("main_log")->warn(
            "TraceLogger could not open CSV trace files in '{}'; continuing without CSV traces", log_dir_.string());
    }

    log_manager_.getLogger("main_log")->info("Processing components initialized, mode={}",
        is_ppp_rtk ? "PPP-RTK" : "PPP");

    // --- Phase 4: Epoch processing loop ---
    const auto& observations = rinex_parser_->getObservations();
    auto wall_start = std::chrono::high_resolution_clock::now();

    for (size_t epoch_idx = 0; epoch_idx < observations.size(); ) {
        // 4a. Collect epoch observations (advances epoch_idx).
        GnssTime current_time{};
        std::vector<RinexObservation> epoch_obs;
        if (!collectEpochObservations(observations, epoch_idx, epoch_obs, current_time))
            continue;

        state_manager_->setStateEpochTime(current_time);

        const std::string epoch_prefix = "[" + TimeUtils::formatGnssTime(current_time) + "] ";

        // 4b. Compute time step and detect data gaps.
        double dt;
        bool gap_detected = false;
        if (first_epoch_) {
            dt = rinex_parser_->getHeader().interval.value_or(30.0);
            log_manager_.getLogger("main_log")->info("{}First epoch, assuming dt = {}s", epoch_prefix, dt);
        } else {
            dt = current_time.tow - prev_tow_;
            const double expected_interval = rinex_parser_->getHeader().interval.value_or(30.0);
            gap_detected = dt > expected_interval * 2.0;
            if (std::abs(dt - expected_interval) > 1e-3) {
                log_manager_.getLogger("main_log")->warn("{}Unexpected time step {}s (expected {}s)",
                    epoch_prefix, dt, expected_interval);
            }
        }
        prev_tow_ = current_time.tow;
        prev_epoch_time_ = current_time;

        // 4c. Sync PppState from the state vector.
        try {
            state_manager_->updatePppStateFromVector(state_manager_->getStateVector(), ppp_state_, *state_manager_);
        } catch (const std::exception& e) {
            log_manager_.getLogger("main_log")->error("{}Failed to update ppp_state: {}", epoch_prefix, e.what());
            return 1;
        }

        // 4d. Compute modeled observations.
        std::vector<PppObservation> modeled_obs;
        if (!ppp_model_->computeModels(epoch_obs, ppp_state_, modeled_obs)) {
            log_manager_.getLogger("main_log")->warn("{}Failed to compute models for epoch", epoch_prefix);
            continue;
        }
        {
            std::string sat_clk_summary;
            for (const auto& obs : modeled_obs) {
                sat_clk_summary += fmt::format("{}:r={:.1f}km ", obs.satellite, obs.range / 1000.0);
            }
            log_manager_.getLogger("main_log")->debug(
                "{}Modeled {} sats: {}", epoch_prefix, modeled_obs.size(), sat_clk_summary);
        }

        // 4e. WLS estimation & clock seeding.
        handleWlsSeeding(current_time, epoch_prefix, modeled_obs, gap_detected);

        // 4f. Kalman prediction.
        if (imu_integrator_ && imu_integrator_->hasData() && !first_epoch_) {
            // IMU-aided prediction: use strapdown mechanization.
            kalman_filter_->predictImu(prev_epoch_time_, current_time, *imu_integrator_);
        } else {
            kalman_filter_->predict(dt);
        }
        try {
            state_manager_->updatePppStateFromVector(state_manager_->getStateVector(), ppp_state_, *state_manager_);
        } catch (const std::exception& e) {
            log_manager_.getLogger("main_log")->error("{}Failed to update ppp_state: {}", epoch_prefix, e.what());
            return 1;
        }

        // 4g. Cycle slip detection & ambiguity resets.
        detectCycleSlips(epoch_prefix, modeled_obs);

        // 4h. Iterative KF measurement update loop.
        EpochMeasurement applied_meas{};
        bool have_applied = false;
        runFilterUpdateLoop(epoch_prefix, modeled_obs, applied_meas, have_applied);

        // 4i. Optional geometry relinearization.
        runGeometryRelinearization(epoch_prefix, epoch_obs, applied_meas, have_applied);

        // 4i2. Integer ambiguity resolution (MLAMBDA).
        if (ambiguity_resolver_ && have_applied) {
            auto ar_result = ambiguity_resolver_->resolve(*state_manager_, modeled_obs);
            if (ar_result.success) {
                // Apply fixed state to the filter
                state_manager_->setStateVector(ar_result.fixed_state);
                state_manager_->setCovariance(ar_result.fixed_covariance);

                // Fix-and-hold: constrain ambiguities to integer values
                ambiguity_resolver_->holdAmbiguities(*state_manager_, ar_result);

                log_manager_.getLogger("main_log")->info(
                    "{}AR: fixed {}/{} ambs, ratio={:.2f}",
                    epoch_prefix, ar_result.num_fixed, ar_result.num_candidates,
                    ar_result.ratio);
            }
        }

        // 4j. Store posterior position sigma for IF slip gating on the next epoch.
        updatePositionSigma();

        // 4k. Per-epoch logging & traces.
        logEpochSummary(epoch_prefix);
        writeEpochTraces(current_time, applied_meas, have_applied);
    }

    auto wall_end = std::chrono::high_resolution_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(wall_end - wall_start).count();
    log_manager_.getLogger("main_log")->info("Processing completed in {} ms, mode={}",
        duration_ms, is_ppp_rtk ? "PPP-RTK" : "PPP");

    return 0;
}

// ---------------------------------------------------------------------------
// Real-time processing entry point
// ---------------------------------------------------------------------------

int Application::runRealTime() {
    auto log = log_manager_.getLogger("main_log");
    log->info("Starting real-time processing mode");

    // --- Phase 1b: Resolve product data (still needed for orbits/clocks) ---
    if (!resolveData()) return 1;

    // --- Initialize real-time engine ---
    RealTimeEngine engine(config_, log_manager_);
    if (!engine.initialize()) {
        log->error("Failed to initialize real-time engine");
        return 1;
    }

    const auto& rt_config = config_.getRealTimeConfig();

    // --- Select input source ---
    if (rt_config.input_type == "file") {
        // File replay mode: read RTCM binary file and feed frames.
        log->info("Real-time file replay from: {}", rt_config.input_path);

        std::ifstream stream(rt_config.input_path, std::ios::binary);
        if (!stream.is_open()) {
            log->error("Failed to open RTCM replay file: {}", rt_config.input_path);
            return 1;
        }

        RtcmStreamReader reader;
        constexpr size_t kChunkSize = 4096;
        std::vector<uint8_t> chunk(kChunkSize);

        auto wall_start = std::chrono::high_resolution_clock::now();

        while (stream.good()) {
            stream.read(reinterpret_cast<char*>(chunk.data()), kChunkSize);
            auto bytes_read = static_cast<size_t>(stream.gcount());
            if (bytes_read == 0) break;

            reader.feed(chunk.data(), bytes_read);

            while (auto frame = reader.nextFrame()) {
                engine.feedRtcmFrame(*frame);
            }
        }

        auto wall_end = std::chrono::high_resolution_clock::now();
        auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(wall_end - wall_start).count();
        log->info("File replay complete: {} epochs in {} ms, {} frames extracted, {} bytes discarded",
                  engine.epochCount(), dur_ms,
                  reader.framesExtracted(), reader.bytesDiscarded());
    }
    else if (rt_config.input_type == "ntrip") {
        // NTRIP live stream mode.
        NtripConfig ntrip_cfg;
        ntrip_cfg.host              = rt_config.ntrip_host;
        ntrip_cfg.port              = rt_config.ntrip_port;
        ntrip_cfg.mountpoint        = rt_config.ntrip_mountpoint;
        ntrip_cfg.username          = rt_config.ntrip_user;
        ntrip_cfg.password          = rt_config.ntrip_password;
        ntrip_cfg.nmea_gga          = rt_config.nmea_gga;
        ntrip_cfg.reconnect_delay_s = rt_config.reconnect_delay_s;

        NtripClient client(ntrip_cfg, log_manager_);
        if (!client.start()) {
            log->error("Failed to start NTRIP client");
            return 1;
        }

        log->info("NTRIP client started, streaming from {}:{}/{}",
                  ntrip_cfg.host, ntrip_cfg.port, ntrip_cfg.mountpoint);

        RtcmStreamReader reader;
        std::vector<uint8_t> buf;

        // Main processing loop — runs until the connection is closed or
        // the process is interrupted.
        while (client.isConnected()) {
            size_t n = client.read(buf, 4096);
            if (n == 0) continue;

            reader.feed(buf);

            while (auto frame = reader.nextFrame()) {
                engine.feedRtcmFrame(*frame);
            }
        }

        client.stop();
        log->info("NTRIP session ended: {} epochs processed", engine.epochCount());
    }
    else {
        log->error("Unsupported real-time input_type: '{}'", rt_config.input_type);
        return 1;
    }

    return 0;
}

} // namespace GnssProcessor