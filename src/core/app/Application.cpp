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

#include "app/Application.hpp"
#include "math/TimeUtils.hpp"
#include "products/ProductCache.hpp"
#include "math/Coordinates.hpp"
#include "products/PathResolver.hpp"
#include "products/ProductResolver.hpp"
#include "realtime/NtripClient.hpp"
#include "realtime/RtcmStreamReader.hpp"
#include "filter/AmbiguityResolver.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "model/ModelGeometry.hpp"
#include "products/ProductCache.hpp"
#include "slip/MwSlipDetector.hpp"
#include <chrono>
#include <set>
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

    // Apply logger enable/disable flags from the logging config section.
    {
        const auto& lc = config_.getLoggingConfig();
        std::set<std::string> disabled;
        // Enumerate all known logger names and check against config.
        static const std::vector<std::string> all_loggers = {
            "main_log", "gnss_config", "kalman_filter", "state_manager",
            "ppp_model", "measurement_model", "mw_slip_detector",
            "if_slip_detector", "gf_slip_detector", "cycle_slip_manager",
            "ambiguity_resolver",
            "corrections", "geometry", "troposphere", "ionosphere",
            "antenna", "antenna_model", "ocean_tide", "planetary_model",
            "stochastic_model", "rinex_parser", "product_parser",
            "product_cache", "resolver", "state_initializer",
            "bssd_pivot_manager", "bssd_transform", "ppp_utils",
            "downloader", "ntrip", "rt_engine", "imu"
        };
        for (const auto& name : all_loggers) {
            if (!lc.isLoggerEnabled(name)) disabled.insert(name);
        }
        if (!disabled.empty()) log_manager_.setDisabledLoggers(disabled);

        // Apply per-logger level overrides (logging.levels in JSON).
        // Disabled loggers take precedence; any override here only applies
        // to loggers not in the disabled set.
        if (!lc.per_logger_levels.empty()) {
            log_manager_.setLoggerLevels(lc.per_logger_levels);
        }
    }

    log_manager_.getLogger("main_log")->info("Configuration loaded from {}", config_path_.string());
    return true;
}

// ---------------------------------------------------------------------------
// Phase 1b: Resolve product and RINEX data via PathResolver.
//
// The dispatch on data_source happens inside PathResolver itself —
// internal mode globs the bundled data/ tree, production mode delegates
// to ProductResolver and downloads from CDDIS.  This driver function
// only has to apply whatever paths come back to GnssConfig so the rest
// of the pipeline sees a fully populated config without caring which
// mode produced the values.
// ---------------------------------------------------------------------------

bool Application::resolveData() {
    PathResolver path_resolver(config_, log_manager_);
    PathResolver::ResolvedPaths resolved;
    if (!path_resolver.resolve(resolved)) {
        log_manager_.getLogger("main_log")->error("PathResolver failed");
        return false;
    }

    // Merge resolved paths back into GnssConfig.  Empty fields are
    // preserved so any explicit values the user typed in JSON still win.
    if (!resolved.product_paths.empty()) {
        // Replace; resolver output is the authoritative product set.
        config_.setProductPaths(resolved.product_paths);
        log_manager_.getLogger("main_log")->info(
            "Resolved {} product files into config", resolved.product_paths.size());
    }
    if (!resolved.navigation_path.empty() && config_.getNavigationPath().empty()) {
        config_.setNavigationPath(resolved.navigation_path);
        log_manager_.getLogger("main_log")->info(
            "Resolved navigation: {}", resolved.navigation_path.string());
    }
    if (!resolved.observation_path.empty() && config_.getObservationPath().empty()) {
        config_.setObservationPath(resolved.observation_path);
        log_manager_.getLogger("main_log")->info(
            "Resolved observation RINEX: {}", resolved.observation_path.string());
    }

    // Baseline RINEX (PPP / PPP-RTK runs ignore these fields).
    if (config_.isBaselineMode()) {
        if (!resolved.baseline_reference_rinex_path.empty() &&
            config_.getBaselineConfig().reference.rinex_path.empty()) {
            config_.setBaselineReferenceRinexPath(resolved.baseline_reference_rinex_path);
            log_manager_.getLogger("main_log")->info(
                "Resolved baseline reference RINEX: {}",
                resolved.baseline_reference_rinex_path.string());
        }
        for (size_t i = 0; i < resolved.baseline_rover_rinex_paths.size(); ++i) {
            const auto& p = resolved.baseline_rover_rinex_paths[i];
            if (p.empty()) continue;
            const auto& rovers = config_.getBaselineConfig().rovers;
            if (i < rovers.size() && rovers[i].rinex_path.empty()) {
                config_.setBaselineRoverRinexPath(i, p);
                log_manager_.getLogger("main_log")->info(
                    "Resolved baseline rover '{}' RINEX: {}",
                    rovers[i].name, p.string());
            }
        }
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
    /// Extract satellite ID from an ambiguity key (e.g. "G01_L1C" -> "G01").
    std::string satIdFromAmbiguityKey(const std::string& key) {
        size_t underscore_pos = key.find('_');
        if (underscore_pos != std::string::npos && underscore_pos >= 2)
            return key.substr(0, underscore_pos);
        return {};
    }

    /// Extract signal from an ambiguity key (e.g. "G01_L1C" -> "L1C").
    std::string signalFromAmbiguityKey(const std::string& key) {
        size_t underscore_pos = key.find('_');
        if (underscore_pos != std::string::npos && underscore_pos + 1 < key.size())
            return key.substr(underscore_pos + 1);
        return {};
    }
} // anonymous namespace

// ---------------------------------------------------------------------------
// Epoch helper: Detect and repair cycle slips
// ---------------------------------------------------------------------------

void Application::detectCycleSlips(const std::string& epoch_prefix,
                               const std::vector<PppObservation>& modeled_obs) {
    auto result = cycle_slip_manager_->detectAndRepair(
        epoch_prefix, modeled_obs, *state_manager_, *meas_model_);
    if (result.state_modified) {
        state_manager_->updatePppStateFromVector(state_manager_->getStateVector(), ppp_state_, *state_manager_);
    }
}

// ---------------------------------------------------------------------------
// Epoch helper: Iterative KF update / relinearization loop
// ---------------------------------------------------------------------------

void Application::runFilterUpdateLoop(const std::string& epoch_prefix,
                                      const std::vector<PppObservation>& modeled_obs,
                                      EpochMeasurement& applied_meas,
                                      bool& have_applied,
                                      std::set<std::string>& reset_satellites,
                                      bool& was_complete_reset) {
    reset_satellites.clear();
    was_complete_reset = false;

    // Snapshot the pre-update position so runGeometryRelinearization can
    // gate its second pass on |Δpos|.  Captured before any KF update so
    // outlier-rebuild loops inside this function don't mask the true epoch
    // displacement.
    relinearize_pre_update_pos_ =
        state_manager_->getStateVector().head<3>();
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

    // The maximum number of passes must accommodate one rebuild per
    // outlier/ambiguity-reset plus a few geometry-relinearization passes.
    // For PPP-RTK the number of individual ambiguity states can be large
    // (satellites × frequencies), so a fixed cap of 10 is insufficient.
    const int max_passes = static_cast<int>(
        std::max<size_t>(10, state_manager_->getAmbiguities().size() + 3));

    int relinear_pass = 0;
    bool need_relinear = true;
    bool need_update = true;
    size_t carry_bad_residual_count = 0;
    std::set<std::string> carry_excluded_measurements;

    // Concise-log gate: marks the start of a new epoch's update loop. When
    // logging.kalman_filter_concise_log is on, pass-1 messages emit live
    // and passes ≥2 are buffered; endEpoch (below) replays the buffer
    // accumulated by the FINAL pass and discards the rest. No-op when the
    // flag is off.
    if (kalman_filter_) kalman_filter_->beginEpoch();

    while ((need_update || need_relinear) && relinear_pass < max_passes) {
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

            // Pivot-slip handover requests from the KF post-fit residual
            // detector.  Forward each constellation's request to the
            // MeasurementModel's PivotManager — the next pass's
            // computeMeasurementUpdate calls handlePivotHandover, which
            // honors the request, picks a healthy replacement pivot, and
            // re-references all SD ambiguities / iono / sat biases
            // losslessly to the new pivot.  Take-and-clear semantics so
            // the same request doesn't re-fire on subsequent passes if
            // the detector doesn't fire again.
            auto handover_cons = kalman_filter_->takeRequestedPivotHandovers();
            auto& res_hist = kalman_filter_->getResidualHistoryTracker();
            for (const auto& cons : handover_cons) {
                log_manager_.getLogger("main_log")->info(
                    "{}Forwarding KF-detected pivot slip on {} to PivotManager for handover next pass",
                    epoch_prefix, cons);
                meas_model_->getPivotManager().requestForcedHandover(
                    cons, "KF post-fit residual detector flagged coherent pivot offset");
                // Reset residual-history entries whose constellation
                // matches the handover.  After re-referencing to a new
                // pivot the BSSD residuals are no longer comparable to
                // the pre-handover EWMA statistics, so clearing them
                // forces a fresh warm-up.  No-op when the tracker is
                // disabled.
                const char cons_prefix =
                    cons == "GPS"     ? 'G' :
                    cons == "Galileo" ? 'E' :
                    cons == "BeiDou"  ? 'C' :
                    cons == "GLONASS" ? 'R' :
                    cons == "QZSS"    ? 'J' :
                    cons == "SBAS"    ? 'S' : '\0';
                if (cons_prefix != '\0' && res_hist.isEnabled()) {
                    res_hist.resetConstellationSignal(cons_prefix, /*signal=*/"");
                }
            }

            // Reset MW slip detector history for satellites that had ambiguities reset.
            // Phase wind-up is NOT reset (see detectCycleSlips comment for rationale).
            if (kalman_filter_->wasCompleteReset()) {
                // Full reset: clear all MW arc history so stale stats
                // don't contaminate post-reset arcs.
                was_complete_reset = true;
                for (const auto& obs : modeled_obs) {
                    cycle_slip_manager_->resetSatellite(obs.satellite);
                    reset_satellites.insert(obs.satellite);
                }
                if (res_hist.isEnabled()) res_hist.resetAll();
                log_manager_.getLogger("main_log")->info(
                    "{}Reset MW slip detector state (complete filter reset)", epoch_prefix);
            } else {
                for (const auto& sat_id : kalman_filter_->getLastResetSatellites()) {
                    cycle_slip_manager_->resetSatellite(sat_id);
                    reset_satellites.insert(sat_id);
                    // Mirror the satellite-level MW reset on the
                    // residual-history tracker — every (sat_id,
                    // signal) entry's stats are invalidated by the
                    // ambiguity reset.
                    if (res_hist.isEnabled()) res_hist.resetSatellite(sat_id);
                    log_manager_.getLogger("main_log")->debug(
                        "{}Reset MW history for {} (KF ambiguity reset)", epoch_prefix, sat_id);
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

    // Concise-log gate: replay the final pass's buffer (the "last pass"
    // visible in the kalman_filter log) and restore passthrough mode. No-op
    // when the gate is null. Must run regardless of how the loop exited
    // (max_passes hit, no more rebuilds, or measurement failure) so that
    // the gate state is always consistent for the next epoch's beginEpoch.
    if (kalman_filter_) kalman_filter_->endEpoch();
}

// ---------------------------------------------------------------------------
// Epoch helper: Geometry relinearization
// ---------------------------------------------------------------------------

void Application::runGeometryRelinearization(const std::string& epoch_prefix,
                                             const std::vector<RinexObservation>& epoch_obs,
                                             EpochMeasurement& applied_meas,
                                             bool have_applied) {
    if (!config_.relinearizeGeometry() || !have_applied) return;

    // ---- Gates -------------------------------------------------------
    // 1. |Δpos| threshold (auto-picked by data availability): skip
    //    relinearization when the primary update moved the position less
    //    than the threshold.  Phase data pins position tightly so a
    //    1 m default catches only early-convergence epochs; code-only
    //    epochs see SPP-style ~5 m jitter so a 100 m default fires only
    //    on truly large jumps (cold start, post-gap recovery).
    // 2. Per-run cap: hard limit on total relinearizations to bound
    //    worst-case cost on pathological inputs.
    const Eigen::Vector3d post_update_pos =
        state_manager_->getStateVector().head<3>();
    const double dpos = (post_update_pos - relinearize_pre_update_pos_).norm();

    bool has_phase = false;
    for (const auto& obs : epoch_obs) {
        for (const auto& [code, val] : obs.measurements) {
            if (!code.empty() && code[0] == 'L') { has_phase = true; break; }
        }
        if (has_phase) break;
    }
    const double thresh = has_phase
        ? config_.relinearizeDposThresholdPhaseM()
        : config_.relinearizeDposThresholdCodeOnlyM();
    if (thresh > 0.0 && dpos < thresh) {
        log_manager_.getLogger("main_log")->debug(
            "{}Geometry relinearization skipped: |Δpos|={:.4f} m < threshold {:.4f} m ({})",
            epoch_prefix, dpos, thresh, has_phase ? "phase" : "code-only");
        return;
    }
    const int cap = config_.relinearizeMaxCount();
    if (cap >= 0 && relinearize_count_ >= cap) {
        // Don't log per-epoch once the cap is hit — would spam.  One
        // info-level "cap reached" line is emitted when we cross it.
        if (relinearize_count_ == cap) {
            log_manager_.getLogger("main_log")->info(
                "{}Geometry relinearization cap ({}) reached; suppressing further passes",
                epoch_prefix, cap);
            ++relinearize_count_;  // bump past so the message fires once
        }
        return;
    }

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
    ++relinearize_count_;
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
// Epoch helper: Smoother snapshot captures
// ---------------------------------------------------------------------------

void Application::capturePriorSnapshot(const GnssTime& current_time, double dt) {
    if (!config_.runSmoother()) return;

    SmootherEpochSnapshot snap;
    snap.t  = current_time;
    snap.dt = dt;
    snap.x_pred = state_manager_->getStateVector();
    snap.P_pred = state_manager_->getCovariance();
    // Transition matrix from the previous epoch to this one.  Evaluated
    // at the current (post-predict) state size, which equals the state
    // size at the end of the previous epoch (predict does not grow the
    // state).  At the first epoch there is no "previous posterior" to
    // propagate, so the smoother terminates at k=0 and the stored F is
    // not used.
    snap.F = state_manager_->getTransitionMatrix(dt);
    // Default x_post/P_post mirror the prior so that has_posterior=false
    // (skipped update) falls through to the forward solution.
    snap.x_post = snap.x_pred;
    snap.P_post = snap.P_pred;
    smoother_buffer_.push_back(std::move(snap));
}

void Application::capturePosteriorSnapshot(const std::set<std::string>& reset_satellites,
                                           bool was_complete_reset,
                                           bool update_applied) {
    if (!config_.runSmoother()) return;
    if (smoother_buffer_.empty()) return;   // guard: prior half wasn't captured

    SmootherEpochSnapshot& snap = smoother_buffer_.back();
    snap.x_post        = state_manager_->getStateVector();
    snap.P_post        = state_manager_->getCovariance();
    snap.has_posterior = update_applied;

    // Core index metadata — read after the update so indices match x_post.
    snap.pos_idx         = state_manager_->getPositionIndex();
    snap.clock_idx       = state_manager_->getClockIndex();
    snap.tropo_idx       = state_manager_->getTropoIndex();
    snap.velocity_idx    = state_manager_->hasVelocity()
                            ? state_manager_->getVelocityIndex()
                            : std::numeric_limits<size_t>::max();
    snap.phase_clock_idx = state_manager_->hasPhaseClock()
                            ? state_manager_->getPhaseClockIndex()
                            : std::numeric_limits<size_t>::max();
    snap.gradient_n_idx  = state_manager_->hasTropoGradients()
                            ? state_manager_->getTropoGradientNorthIndex()
                            : std::numeric_limits<size_t>::max();
    snap.gradient_e_idx  = state_manager_->hasTropoGradients()
                            ? state_manager_->getTropoGradientEastIndex()
                            : std::numeric_limits<size_t>::max();

    // Dynamic-key maps — copy, not reference, since the StateManager mutates
    // these across epochs.
    snap.ambiguities = state_manager_->getAmbiguities();
    snap.ionospheres = state_manager_->getIonosphereStates();
    snap.code_biases = state_manager_->getCodeBiasStates();
    snap.sat_biases  = state_manager_->getSatBiasStates();
    for (const auto& sys : state_manager_->getSystemsWithTimeOffsets()) {
        snap.sys_offsets[sys] = state_manager_->getSystemTimeOffsetIndex(sys);
    }

    snap.reset_satellites = reset_satellites;
    snap.is_discontinuity = was_complete_reset;
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

// Apply Eigen thread count.  Default in config is 1 (sequential) because
// PPP-RTK / baseline matrix sizes (n≈350, m≈250) sit below the size at
// which OpenMP fork/join overhead is offset by the parallel speedup —
// in profile traces vcomp140 idle-at-barrier showed up as ~13% of the
// run.  Set processing.eigen_threads to 0 (OpenMP default) or any N > 1
// to opt back into multi-threaded.
const int eigen_threads = config_.eigenThreads();
if (eigen_threads > 0) {
    Eigen::setNbThreads(eigen_threads);
    log_manager_.getLogger("main_log")->info(
        "Eigen thread count: {} (config-pinned)", eigen_threads);
} else {
    log_manager_.getLogger("main_log")->info(
        "Eigen thread count: {} (OpenMP default)", Eigen::nbThreads());
}

// --- Branch on application mode ---
if (config_.getAppMode() == AppMode::RealTime) {
    return runRealTime();
}

// --- Branch on processing mode: Baseline modes take the DD pipeline. ---
if (config_.isBaselineMode()) {
    return runBaseline();
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
    cycle_slip_manager_  = std::make_unique<CycleSlipManager>(config_, log_manager_);

    // Provide BIA satellite biases to the MW and EWL detectors for integer rounding
    cycle_slip_manager_->setCorrections(&ppp_model_->getCorrections());

    // Adaptive MW arc-reset gap tolerance.  When IMU isn't bridging gaps
    // and the RINEX sampling interval is coarser than 5 s, any inter-epoch
    // gap longer than the nominal cadence is suspicious — tightening the
    // gap tolerance to the data rate prevents the MW detector from
    // silently initializing a fresh arc on a slip-corrupted sample after
    // a brief outage.  IMU mode keeps the default 120 s because INS
    // propagation legitimately bridges gaps that long.
    if (!config_.enableImu()) {
        const double rinex_interval = rinex_parser_->getHeader().interval.value_or(0.0);
        if (rinex_interval > 5.0) {
            cycle_slip_manager_->mwDetector().setMaxGapSeconds(rinex_interval);
        }
    }

    // Provide MW detector to the BSSD pivot manager for quality-based pivot scoring.
    meas_model_->setMwDetector(&cycle_slip_manager_->mwDetector());

    // Provide MW data to the KF outlier handler for tiebreaking.
    kalman_filter_->setCycleSlipManager(cycle_slip_manager_.get());

    const auto& logging_cfg = config_.getLoggingConfig();
    if (logging_cfg.enable_mw_slip_trace) {
        cycle_slip_manager_->enableMwTracing(log_dir_);
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
    trace_logger_ = std::make_unique<TraceLogger>(log_manager_, log_dir_, is_ppp_rtk, config_.getLoggingConfig());
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

        // 4a2. Optional input-rate decimation (processing.gnss_rate_s).
        // When the user configures a coarser processing rate than the RINEX
        // interval (e.g. 300 s output from a 30 s file), silently skip
        // input epochs that fall between processed epochs.  The first
        // epoch always anchors the schedule.
        if (config_.getGnssRate() > 0.0 && !first_epoch_) {
            // 1 ms tolerance absorbs GPS-time rounding.
            const double since_last = current_time.tow - last_processed_tow_;
            if (since_last + 1e-3 < config_.getGnssRate()) {
                continue;
            }
        }

        state_manager_->setStateEpochTime(current_time);
        LogManager::setCurrentEpochTime(current_time);

        // The unified [%E] field in the spdlog pattern (LogManager) now
        // carries the GPS week / TOW / GPS-scale datetime for every log
        // line; an in-message-body epoch prefix would duplicate it.  Kept
        // as an empty variable so the existing "{}..." format strings
        // need no edits.
        const std::string epoch_prefix;

        // 4b. Compute time step and detect data gaps.
        // When downsampling is active the effective cadence is
        // max(gnss_rate, rinex_interval) — use that for the gap
        // threshold and the "unexpected dt" warning so intentionally
        // decimated steps are not flagged as outages.
        double dt;
        bool gap_detected = false;
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
            if (std::abs(dt - effective_interval) > 0.5) {
                log_manager_.getLogger("main_log")->warn("{}Unexpected time step {}s (expected {}s)",
                    epoch_prefix, dt, effective_interval);
            }
        }
        prev_tow_ = current_time.tow;
        prev_epoch_time_ = current_time;
        last_processed_tow_ = current_time.tow;

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

        // Smoother prior capture: after predict(), before any cycle-slip or
        // update-driven state modifications, so the recorded a-priori matches
        // F * x_post_{prev} mathematically.
        capturePriorSnapshot(current_time, dt);

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
        std::set<std::string> epoch_reset_sats;
        bool epoch_complete_reset = false;
        runFilterUpdateLoop(epoch_prefix, modeled_obs, applied_meas, have_applied,
                            epoch_reset_sats, epoch_complete_reset);

        // 4i. Optional geometry relinearization.
        runGeometryRelinearization(epoch_prefix, epoch_obs, applied_meas, have_applied);

        // Relinearization can also trigger ambiguity/complete resets via its
        // internal update() call.  Merge any such resets into the epoch's
        // accumulators before the smoother captures the posterior.
        if (kalman_filter_->wasCompleteReset()) epoch_complete_reset = true;
        for (const auto& sat_id : kalman_filter_->getLastResetSatellites()) {
            epoch_reset_sats.insert(sat_id);
        }

        // Smoother posterior capture: taken BEFORE AR so the smoother
        // operates on float ambiguities (v1 policy).  AR-fixed states
        // are applied below for the forward solution only.
        capturePosteriorSnapshot(epoch_reset_sats, epoch_complete_reset, have_applied);

        // 4i2. Integer ambiguity resolution (MLAMBDA).
        if (ambiguity_resolver_ && have_applied) {
            const auto wl_ambs = cycle_slip_manager_->getResolvedWlAmbiguities();
            auto ar_result = ambiguity_resolver_->resolve(*state_manager_, modeled_obs, wl_ambs);
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

    // --- Phase 5: Optional backward RTS smoother pass ---
    if (config_.runSmoother() && !smoother_buffer_.empty()) {
        auto smooth_log = log_manager_.getLogger("smoother");
        if (smooth_log) smooth_log->info("Running backward RTS smoother over {} epochs...",
                                         smoother_buffer_.size());

        Smoother smoother(std::move(smoother_buffer_), log_manager_);
        if (smoother.run() && config_.getLoggingConfig().enable_smoother_trace) {
            const std::filesystem::path out_path = log_dir_ / "smoother_trace.csv";
            std::ofstream ofs(out_path);
            if (!ofs) {
                log_manager_.getLogger("main_log")->error(
                    "Failed to open smoother trace file: {}", out_path.string());
            } else {
                ofs << "week,tow,x,y,z,sigma_x,sigma_y,sigma_z,"
                       "clock_m,sigma_clock_m,tropo_zwd_m,sigma_tropo_m,smoothed\n";
                const auto& results = smoother.results();
                auto sig = [](const Eigen::MatrixXd& P, size_t i) {
                    const double v = P(static_cast<Eigen::Index>(i),
                                       static_cast<Eigen::Index>(i));
                    return (std::isfinite(v) && v >= 0.0) ? std::sqrt(v) : 0.0;
                };
                for (const auto& s : results) {
                    const size_t px = s.pos_idx;
                    ofs << s.t.week << ","
                        << fmt::format("{:.3f}", s.t.tow) << ","
                        << fmt::format("{:.4f}", s.x[static_cast<Eigen::Index>(px)])     << ","
                        << fmt::format("{:.4f}", s.x[static_cast<Eigen::Index>(px + 1)]) << ","
                        << fmt::format("{:.4f}", s.x[static_cast<Eigen::Index>(px + 2)]) << ","
                        << fmt::format("{:.4f}", sig(s.P, px))     << ","
                        << fmt::format("{:.4f}", sig(s.P, px + 1)) << ","
                        << fmt::format("{:.4f}", sig(s.P, px + 2)) << ","
                        << fmt::format("{:.4f}", s.x[static_cast<Eigen::Index>(s.clock_idx)]) << ","
                        << fmt::format("{:.4f}", sig(s.P, s.clock_idx)) << ","
                        << fmt::format("{:.4f}", s.x[static_cast<Eigen::Index>(s.tropo_idx)]) << ","
                        << fmt::format("{:.4f}", sig(s.P, s.tropo_idx)) << ","
                        << (s.smoothed ? 1 : 0) << "\n";
                }
                log_manager_.getLogger("main_log")->info(
                    "Smoother trace written: {} ({} epochs)", out_path.string(), results.size());
            }
        }
    }

    auto wall_end = std::chrono::high_resolution_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(wall_end - wall_start).count();
    log_manager_.getLogger("main_log")->info("Processing completed in {} ms, mode={}",
        duration_ms, is_ppp_rtk ? "PPP-RTK" : "PPP");

    if (kalman_filter_) kalman_filter_->logKalmanFilterTimings();

    return 0;
}

} // namespace GnssProcessor
