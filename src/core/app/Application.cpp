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
#include "aiding/ImuAidingSource.hpp"   // full type for aiding_manager_->imu() wiring
#include "app/EpochUpdateSupport.hpp"
#include "math/TimeUtils.hpp"
#include "products/ProductCache.hpp"
#include "math/Coordinates.hpp"
#include "products/PathResolver.hpp"
#include "products/ProductResolver.hpp"
#include "products/StationInfoFile.hpp"
#include "realtime/NtripClient.hpp"
#include "realtime/RtcmStreamReader.hpp"
#include "filter/AmbiguityResolver.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "model/ModelGeometry.hpp"
#include "products/ProductCache.hpp"
#include "slip/MwSlipDetector.hpp"
#include "common/PppUtils.hpp"
#include <chrono>
#include <set>
#include <fstream>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <cstdlib>
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

            // ENABLED-BUT-SILENCED census.  `enable_<logger>` and
            // `levels.<logger>` are independent, and on an inherited config chain
            // they routinely come from DIFFERENT files: a child flipping
            // `enable_ambiguity_resolver: true` inherits a
            // `levels.ambiguity_resolver: "warn"` from a base it never reads, and
            // the run produces a 0-byte log with no diagnostic.  The author's
            // intent ("give me this logger") is defeated by a level they did not
            // set.  That silence has already cost a 7-minute producer arm, which
            // read the empty file as a BROKEN logger.  One line at startup makes
            // it self-diagnosing.
            const auto level_name = [](LogLevel l) -> const char* {
                switch (l) {
                    case LogLevel::Trace: return "trace";
                    case LogLevel::Debug: return "debug";
                    case LogLevel::Info:  return "info";
                    case LogLevel::Warn:  return "warn";
                    case LogLevel::Error: return "error";
                    default:              return "critical/off";
                }
            };
            std::vector<std::string> silenced;
            for (const auto& [name, level] : lc.per_logger_levels) {
                if (disabled.count(name)) continue;   // already reported as disabled
                if (level > LogLevel::Info)           // enum is ordered Trace<Debug<Info<Warn<...
                    silenced.push_back(name + "=" + level_name(level));
            }
            if (!silenced.empty()) {
                std::string joined;
                for (const auto& s : silenced) { if (!joined.empty()) joined += ", "; joined += s; }
                log_manager_.getLogger("gnss_config")->info(
                    "Logging: {} logger(s) are ENABLED but level-SILENCED above info — {}.  Their "
                    ".log files will be EMPTY even though the logger is on, because "
                    "`logging.levels.<name>` and `enable_<name>` are independent and on an "
                    "inherited chain usually come from different files.  To actually see one, set "
                    "BOTH `logging.enable_<name>: true` AND `logging.levels.<name>: \"info\"` (or "
                    "\"debug\") in the child config.",
                    silenced.size(), joined);
            }
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

    // Opt-in ATX freshness refresh, after paths are resolved (so the ATX path is
    // known) and before loadInputData() reads the antenna model.
    refreshAtxIfEnabled_();

    return true;
}

void Application::refreshAtxIfEnabled_() {
    namespace fs = std::filesystem;
    const auto& dl = config_.getDownloadConfig();
    if (!dl.atx_auto_refresh) return;
    auto log = log_manager_.getLogger("main_log");

    // Locate the ATX among the resolved inputs.  Works for both modes: internal
    // globs static/atx into the product set; production pushes atx_path there.
    fs::path atx = dl.atx_path;
    if (atx.empty() || !fs::exists(atx)) {
        for (const auto& p : config_.getProductPaths()) {
            std::string e = p.extension().string();
            std::transform(e.begin(), e.end(), e.begin(),
                           [](char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; });
            if (e == ".atx") { atx = p; break; }
        }
    }
    if (atx.empty()) {
        if (log) log->warn("atx_auto_refresh: no ATX path resolved - skipping refresh");
        return;
    }

    const fs::path root   = fs::path(GNSS_PROJECT_ROOT);
    const fs::path script = root / "scripts" / "refresh_atx.py";
    if (!fs::exists(script)) {
        if (log) log->warn("atx_auto_refresh: {} not found - skipping refresh", script.string());
        return;
    }

    // Interpreter: explicit report_python override, else baked venv, else PATH.
    std::string python = config_.reportPython();
    if (python.empty()) {
        const fs::path venv_py = root / "venv" / "Scripts" / "python.exe";
        python = fs::exists(venv_py) ? venv_py.string() : std::string("python");
    }

    std::string inner = "\"" + python + "\" \"" + script.string() + "\" --atx \""
                      + fs::absolute(atx).string() + "\" --max-age-days "
                      + std::to_string(dl.atx_refresh_max_age_days);
    if (!dl.atx_refresh_url.empty())
        inner += " --url \"" + dl.atx_refresh_url + "\"";

    if (log) log->info("atx_auto_refresh: checking {} for updates (max_age={} d)",
                       atx.string(), dl.atx_refresh_max_age_days);

#ifdef _WIN32
    // std::system pipes through cmd /c, which strips the outermost quotes -- wrap
    // the already-quoted command in an extra pair (same idiom as generateReportFor_).
    const std::string cmd = "\"" + inner + "\"";
#else
    const std::string cmd = inner;
#endif
    const int rc = std::system(cmd.c_str());
    if (rc == 0) {
        if (log) log->info("atx_auto_refresh: ATX is current or was updated "
                           "(see refresh_atx.py output above)");
    } else {
        if (log) log->warn("atx_auto_refresh: refresh exited with code {} - "
                           "continuing with the local ATX (network down? see stderr)", rc);
    }
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
            initial_position_ = Coordinates::markerToArp(marker_pos, hen);

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

    // Station Information File a-priori: when enabled and the station resolves,
    // seed the filter with the authoritative ITRF coordinate propagated to the
    // first-observation epoch (velocity + seasonal + post-seismic terms).  This
    // is more accurate and frame-consistent (ITRF2020 / IGS20, matching the
    // products) than the RINEX APPROX header.  The OCCUPATION coordinate is
    // ARP-referenced like the state vector, so no antenna delta is applied.
    if (config_.enableStationInfo()) {
        if (const auto sif = config_.getStationInfo()) {
            const std::string marker = rinex_parser_->getHeader().marker_name;
            if (const StationInfo* si = sif->find(marker)) {
                const GnssTime t0 = rinex_parser_->getTimeOfFirstObs();
                const double year = TimeUtils::mjdToDecimalYear(TimeUtils::gpsTimeToUtcMjd(t0));
                const ResolvedStationCoord rc = resolveStationCoordinate(*si, year);
                if (rc.valid && rc.ecef.allFinite() && rc.ecef.norm() > 1e6) {
                    const Eigen::Vector3d prev = initial_position_;
                    initial_position_ = rc.ecef;
                    Eigen::VectorXd sv = state_manager_->getStateVector();
                    if (sv.size() >= static_cast<Eigen::Index>(state_manager_->getPositionIndex() + 3)) {
                        sv.segment<3>(state_manager_->getPositionIndex()) = initial_position_;
                        state_manager_->setStateVector(sv);
                    }
                    log_manager_.getLogger("main_log")->info(
                        "SIF a-priori for marker '{}' (matched {}) @ {:.3f}: ITRF {} "
                        "({:.3f}, {:.3f}, {:.3f}) m; shift vs RINEX/fallback = {:.3f} m",
                        marker, si->id, year, si->reference_frame,
                        initial_position_.x(), initial_position_.y(), initial_position_.z(),
                        (initial_position_ - prev).norm());
                }

                // Antenna / receiver metadata QC against the SIF active windows
                // at the first-obs epoch: flag DO_NOT_USE equipment and antenna
                // type disagreements between the RINEX header and the SIF.
                const auto trimmed = [](std::string v) {
                    const auto b = v.find_first_not_of(" \t");
                    if (b == std::string::npos) return std::string{};
                    const auto e = v.find_last_not_of(" \t");
                    return v.substr(b, e - b + 1);
                };
                for (const auto& aw : si->antennas) {
                    if (year < aw.start_year || year >= aw.end_year) continue;
                    if (aw.do_not_use)
                        log_manager_.getLogger("main_log")->warn(
                            "SIF: antenna window for '{}' at {:.3f} is flagged DO_NOT_USE "
                            "(type '{}')", marker, year, aw.type);
                    const std::string rinex_ant = trimmed(rinex_parser_->getHeader().antenna_type);
                    const std::string sif_ant   = trimmed(aw.type);
                    if (!rinex_ant.empty() && !sif_ant.empty() && rinex_ant != sif_ant)
                        log_manager_.getLogger("main_log")->warn(
                            "SIF: antenna type mismatch for '{}': RINEX='{}' vs SIF='{}'",
                            marker, rinex_ant, sif_ant);
                    break;  // one covering window
                }
                for (const auto& rw : si->receivers) {
                    if (year < rw.start_year || year >= rw.end_year) continue;
                    if (rw.do_not_use)
                        log_manager_.getLogger("main_log")->warn(
                            "SIF: receiver window for '{}' at {:.3f} is flagged DO_NOT_USE "
                            "(type '{}')", marker, year, rw.type);
                    break;
                }
            }
        }
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

WlsResult Application::handleWlsSeeding(const GnssTime& current_time,
                                        const std::string& epoch_prefix,
                                        const std::vector<PppObservation>& modeled_obs,
                                        bool gap_detected) {
    // Under BSSD the KF carries no receiver clock state — there's nothing
    // to seed, and the WLS clock estimate is meaningless to the SD filter.
    // Skip the entire seeding path.
    if (!state_manager_->hasClock()) {
        return WlsResult{};
    }

    auto wls_result = wls_estimator_->estimate(current_time, modeled_obs);
    if (!wls_result.success() || wls_result.state.size() < 4) return wls_result;

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

    // Per-mode short-circuit on KinematicIMU: the IMU has been
    // propagating position/velocity through the predict step at
    // realistic uncertainty, so the gap is already correctly handled
    // structurally.  Sat-specific cross-covs are still suspect
    // (geometry / multipath have changed) but iono Q inflation
    // covers the dominant component.  We only zero the clock cross-
    // covs to protect the WLS-clock jolt from contaminating position.
    if (mode == DynamicsMode::KinematicIMU) {
        if (log) {
            log->info("{}handleGap: dt={:.1f}s, mode=KinematicIMU, long={} "
                      "→ IMU bridging; zeroing clock×sat cross-cov only",
                      epoch_prefix, dt, long_gap);
        }
        state_manager_->zeroClockCrossCovariancesToSatStates();
        return;
    }

    // Common to Static and KinematicNoImu: protect against stale
    // clock↔sat-state correlations after the WLS clock jolt.
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

    // Static-long and KinematicNoImu both need a heavier reset.  The
    // common backbone: drop all sat-specific states so
    // ensureRequiredStates rebuilds them around current geometry,
    // drop BSSD/DD pivots so the next epoch elects fresh.
    state_manager_->zeroPositionCrossCovariancesToSatStates();
    state_manager_->pruneIonosphereStatesExcept({});
    state_manager_->pruneAmbiguitiesExcept({});
    state_manager_->pruneSatBiasStatesExcept({});

    // The ambiguity teardown above invalidates every soft-LLI / gap-bridge
    // hold and snapshot — a hold outliving its state would later "restore"
    // a pre-gap value onto a freshly rebuilt ambiguity.
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
        // Static-long: position value is still the best estimate (the
        // antenna did not move), but the converged variance is too
        // tight to absorb post-gap residuals — inflate modestly.
        constexpr double kStaticLongPosSigmaInflation_m = 0.05;  // 5 cm
        state_manager_->inflatePositionDiagonal(kStaticLongPosSigmaInflation_m);
        if (log) {
            log->info("{}handleGap: dt={:.1f}s, mode=Static-long → pos σ+={:.3f}m, "
                      "dropped iono/amb/sat-bias states, dropped pivots",
                      epoch_prefix, dt, kStaticLongPosSigmaInflation_m);
        }
        return;
    }

    // KinematicNoImu: full cold-start.  Reseed position from WLS,
    // inflate to a large prior, reset velocity, clear sat-bias cache
    // (the receiver may have power-cycled — pre-gap hardware estimates
    // are no more trustworthy than a fresh single-epoch seed).
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

CycleSlipEpochResult Application::detectCycleSlips(const std::string& epoch_prefix,
                               const std::vector<PppObservation>& modeled_obs) {
    auto result = cycle_slip_manager_->detectAndRepair(
        epoch_prefix, modeled_obs, *state_manager_, *meas_model_);
    if (result.state_modified) {
        state_manager_->updatePppStateFromVector(state_manager_->getStateVector(), ppp_state_, *state_manager_);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Epoch helper: GIM ionosphere-weighting pseudo-observation update
// ---------------------------------------------------------------------------

void Application::applyGimIonoUpdate(const GnssTime& current_time,
                                     const std::vector<PppObservation>& modeled_obs) {
    const auto& iw = config_.getIonosphereWeightingConfig();
    if (!iw.enabled || !gim_ || !gim_->isLoaded() || !kalman_filter_ || !state_manager_) {
        return;
    }

    const Eigen::VectorXd x = state_manager_->getStateVector();
    if (x.size() < 3) return;
    const Eigen::Vector3d rx = x.head<3>();

    // One pseudo-observation row per satellite that owns a slant-iono
    // state and has a valid GIM value above the elevation cutoff.
    std::vector<int>    idx;    // iono-state column index
    std::vector<double> innov;  // GIM slant delay - current iono state (m)
    std::vector<double> var;    // per-row pseudo-obs variance (m^2)
    std::vector<System> cons;   // constellation, for the common-mode block structure

    // BSSD pivot satellites hold their slant-iono state frozen (value 0,
    // variance 0) as the rank-fixing datum; they must be excluded from the
    // pseudo-observation entirely (see the per-row guard below).
    const std::set<std::string>& frozen_pivots =
        state_manager_->getFrozenPivotSatellites();

    for (const auto& obs : modeled_obs) {
        if (obs.elevation < iw.elevation_cutoff_deg) continue;
        const int iono_idx = state_manager_->getIonosphereIndex(obs.satellite);
        if (iono_idx < 0 || iono_idx >= static_cast<int>(x.size())) continue;
        // Skip BSSD pivot / any frozen-datum iono row.  A zero-variance state
        // receives zero Kalman gain, so it cannot benefit from the GIM; but
        // because it shares the per-constellation common-mode block in R, its
        // spurious innovation (GIM_STEC - 0 ~ 5-30 m) would otherwise leak into
        // every other same-constellation iono state through the off-diagonal
        // S^-1.  The pivot's datum offset is exactly what the common-mode block
        // is meant to absorb from the *remaining* rows, so it must not appear
        // as a row itself.
        if (frozen_pivots.count(obs.satellite) ||
            state_manager_->getStateVariance(iono_idx) <= 0.0) {
            continue;
        }
        // The slant-iono state is referenced to GPS L1 for every
        // constellation (see the MeasurementModel iono Jacobian).
        const GimQuery q = gim_->slantIonoDelay(current_time, rx, obs.sat_pos,
                                                Constants::GPS_L1_FREQ);
        if (!q.valid) continue;
        const double sigma = std::max(iw.floor_m, iw.std_scale * q.std_m);
        idx.push_back(iono_idx);
        innov.push_back(q.delay_m - x(iono_idx));
        var.push_back(sigma * sigma);
        cons.push_back(obs.system);
    }

    const int m = static_cast<int>(idx.size());
    if (m == 0) return;
    const int n = static_cast<int>(x.size());

    Eigen::MatrixXd H = Eigen::MatrixXd::Zero(m, n);
    Eigen::VectorXd z(m);
    for (int k = 0; k < m; ++k) {
        H(k, idx[k]) = 1.0;
        z(k) = innov[k];
    }

    // R = block-diagonal over constellations of  diag(var) + sigma_c^2 * 11^T.
    // The all-ones block leaves the per-constellation common mode (the
    // un-modellable receiver iono-datum bias) unconstrained, so only the
    // between-satellite structure of the GIM is imposed.
    const double common_var = iw.common_mode_sigma_m * iw.common_mode_sigma_m;
    Eigen::MatrixXd R = Eigen::MatrixXd::Zero(m, m);
    for (int r = 0; r < m; ++r) {
        for (int c = 0; c < m; ++c) {
            if (cons[r] == cons[c]) R(r, c) = common_var;
        }
        R(r, r) += var[r];
    }

    kalman_filter_->applyPseudoObservation(H, z, R);

    try {
        state_manager_->updatePppStateFromVector(state_manager_->getStateVector(),
                                                 ppp_state_, *state_manager_);
    } catch (const std::exception& e) {
        log_manager_.getLogger("main_log")->error(
            "GIM iono update: failed to sync ppp_state: {}", e.what());
    }

    double sum_abs = 0.0;
    for (double e : innov) sum_abs += std::abs(e);
    log_manager_.getLogger("ionosphere")->debug(
        "GIM iono update: {} sats constrained, mean|innov|={:.3f} m",
        m, sum_abs / static_cast<double>(m));
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

    // Snapshot the pre-update position for runGeometryRelinearization's |Δpos|
    // gate.  Captured before any KF update so the outlier-rebuild loop below
    // doesn't mask the true epoch displacement.  The IEKF's full-state anchor
    // (x⁻, P⁻) is NOT captured here — it comes from the prior the primary
    // update() stashes (KalmanFilter::getLastUpdatePriorX/P), which is
    // dimension-matched to the post-augmentation state.
    relinearize_pre_update_pos_ = state_manager_->getStateVector().head<3>();
    // When the proper IEKF relinearization is active (non-tightly-coupled),
    // suppress the in-loop linear relinearization below so all relinearization
    // routes through the prior-anchored IEKF in runGeometryRelinearization.
    const bool use_iekf_relinear =
        config_.relinearizeGeometry() && !config_.tightlyCoupledImu();
    // Choose a relinearization threshold based on whether phase measurements are present.
    auto has_phase = [&modeled_obs]() {
        for (const auto& obs : modeled_obs) {
            for (const auto& kv : obs.observed) {
                if (!kv.first.empty() && kv.first[0] == 'L') return true;
            }
        }
        return false;
    };
    // When the proper IEKF relinearization is active, suppress the in-loop
    // linear relinearization (the pos_jump gate below) entirely: all
    // relinearization is then handled by runGeometryRelinearization — anchored
    // on the prior, single covariance commit.  An in-loop pass here would only
    // double-update a posterior the IEKF subsequently discards.  When the IEKF
    // path is off (feature disabled, or tightly-coupled IMU using the legacy
    // naive pass), keep the original thresholds.
    const double kPosRelinearThreshM =
        use_iekf_relinear ? std::numeric_limits<double>::infinity()
                          : (has_phase() ? 100.0 : 1000.0);

    // Pass budget — sized so the complete-reset backstop, not the pass cap,
    // terminates a pathological epoch (see computeRebuildPassBudget; shared with
    // the baseline and real-time drivers so the sizing can't drift between them).
    const int max_passes =
        computeRebuildPassBudget(*state_manager_, config_, modeled_obs);

    int relinear_pass = 0;
    bool need_relinear = true;
    bool need_update = true;
    size_t carry_bad_residual_count = 0;
    std::set<std::string> carry_excluded_measurements;
    // Soft-LLI bridge: phase signals currently held across a receiver LLI gap
    // pending a TurboEdit integer repair are excluded from the KF update so
    // their still-slipped ambiguities cannot contaminate shared states across
    // the MW one-epoch confirmation delay.  Empty unless turbo_edit_bridge_lli.
    if (cycle_slip_manager_) {
        auto held = cycle_slip_manager_->getHeldPendingBridgeKeys();
        carry_excluded_measurements.insert(held.begin(), held.end());
        // The held ambiguity STATES must also survive the measurement
        // model's prune-on-first-miss for the duration of the outage —
        // deleting them would destroy the pre-gap state the bridge repair
        // restores.  Same source set as the exclusion above, so protection
        // and exclusion cannot diverge; empty when nothing is held.
        meas_model_->setProtectedAmbiguityKeys(std::move(held));
    }

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
        // attitude-error and accelerometer-bias partials.  (Relocated into
        // ImuAidingSource; the setImuState arithmetic is unchanged.)
        if (aiding_manager_) aiding_manager_->applyTcJacobianInputs(*meas_model_);

        EpochMeasurement meas_update = meas_model_->computeMeasurementUpdate(
            modeled_obs, state_manager_->getStateVector(), carry_excluded_measurements);
        if (meas_update.innovations.size() == 0 || !meas_model_->validateMeasurementUpdate(meas_update)) {
            log_manager_.getLogger("main_log")->warn("{}Invalid or empty measurement update (pass {})",
                epoch_prefix, relinear_pass);
            break;
        }

        // Carry forward the bad-residual count so the full-reset safety net accumulates across passes.
        meas_update.bad_residual_count = carry_bad_residual_count;

        // Append composable aiding measurement rows (ZUPT/baro/...) into the
        // finalized measurement before the KF consumes it.  Row sources are
        // restricted to undifferenced PPP for now; appendAidingRow hard-errors
        // if R_full is populated (BSSD/DD active).  The state vector is held in
        // a local so the context's reference outlives collectMeasurement.
        if (aiding_manager_ && !aiding_manager_->empty()) {
            const Eigen::VectorXd aiding_meas_x = state_manager_->getStateVector();
            const AidingEpochContext aiding_meas_ctx{
                aiding_meas_x, prev_epoch_time_, meas_update.time, false };
            for (const auto& row : aiding_manager_->collectMeasurement(aiding_meas_ctx))
                MeasurementModel::appendAidingRow(meas_update, row);
        }

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

            // Forward the KF post-fit detector's forced pivot-handover requests
            // (drained take-and-clear) to the PivotManager, notify the MW pivot
            // picker, reset per-constellation residual history, and release the
            // handled constellations' carried exclusions so their SD rows re-form
            // against the new pivot.  Shared with the real-time driver via
            // EpochUpdateSupport so the two paths can never drift on this — the
            // exact divergence that once left real-time without any handover at all.
            forwardKfPivotHandovers(
                *kalman_filter_, *meas_model_,
                cycle_slip_manager_ ? cycle_slip_manager_->mwDetector() : nullptr,
                log_manager_, epoch_prefix, carry_excluded_measurements);

            // Residual-history tracker, shared by the MW arc-history reset below
            // (the per-constellation handover reset is done inside the helper).
            auto& res_hist = kalman_filter_->getResidualHistoryTracker();

            // Reset MW slip detector history for satellites that had ambiguities reset.
            // Phase wind-up is NOT reset: it is a continuous geometric quantity,
            // unaffected by the integer-cycle jump of an in-arc cycle slip.
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
                // Prefer per-(sat, signal) MW arc cleanup when the KF
                // tracked which signal was reset.  Spares MW arcs on
                // signal pairs that don't involve the slipped signal
                // (e.g. an L1C slip leaves the L2W/L5X arc intact
                // instead of nuking it as collateral damage).  Falls
                // back to the per-sat clear for anything in
                // last_reset_satellites_ that didn't get a per-signal
                // entry.
                std::set<std::string> covered_sats;
                for (const auto& [sat_id, sig] : kalman_filter_->getLastResetSignals()) {
                    cycle_slip_manager_->resetSatelliteSignal(sat_id, sig);
                    reset_satellites.insert(sat_id);
                    covered_sats.insert(sat_id);
                    // Residual-history is sat-level; record once per sat.
                    log_manager_.getLogger("main_log")->debug(
                        "{}Reset MW history for {} {} (KF ambiguity reset, per-signal)",
                        epoch_prefix, sat_id, sig);
                }
                for (const auto& sat_id : kalman_filter_->getLastResetSatellites()) {
                    if (covered_sats.count(sat_id)) {
                        // Sat already covered by per-signal reset; just
                        // mirror to residual-history once and skip the
                        // sat-wide MW clear.
                        if (res_hist.isEnabled()) res_hist.resetSatellite(sat_id);
                        continue;
                    }
                    cycle_slip_manager_->resetSatellite(sat_id);
                    reset_satellites.insert(sat_id);
                    if (res_hist.isEnabled()) res_hist.resetSatellite(sat_id);
                    log_manager_.getLogger("main_log")->debug(
                        "{}Reset MW history for {} (KF ambiguity reset, per-sat fallback)",
                        epoch_prefix, sat_id);
                }
            }

            // Sat-wide (iono, sat-bias) re-inflation for satellites whose phase
            // ambiguities were fully reset this pass — mirror of the
            // CycleSlipManager full-reset path, lifted into EpochUpdateSupport so
            // the real-time engine performs the identical partition-baking guard
            // (it previously did not — the known PP/RT asymmetry).  No-op on a
            // complete reset (checked inside) and when slip_state_reinflation is
            // disabled.
            reinflateResetSatStates(*kalman_filter_, *state_manager_,
                                    *meas_model_, config_);

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

    // ---- Tightly-coupled IMU: legacy single naive second pass ----------
    // Re-deriving the posterior from the epoch prior conflicts with the
    // error-state attitude/bias nominal feedback the primary update already
    // folded into C_b_e_ (and zeroed).  Doing it properly there needs
    // deferred nominal feedback; until a tightly-coupled config actually
    // exercises relinearization, keep the original behavior on that path.
    if (aiding_manager_ && aiding_manager_->anySourceRequiresNaiveRelinearization()) {
        runGeometryRelinearizationNaive(epoch_prefix, epoch_obs, applied_meas);
        return;
    }

    // ---- Proper IEKF: iterate computeModels → updateFromPrior ----------
    // Anchor every Gauss–Newton step on the captured epoch prior (x⁻, P⁻),
    // holding the measurement set FIXED to the one the primary update
    // converged on (same outlier exclusions).  The step is then a clean
    // Gauss–Newton iteration on the MAP estimate and the covariance is
    // (I−KH)·P⁻ committed once — no double-counting.  GNSS geometry is nearly
    // linear, so this converges in a single pass except after large
    // cold-start / post-gap jumps.
    // Anchor the IEKF on the prior the primary update() stashed this epoch.
    // It already reflects this epoch's ambiguity augmentation and outlier
    // resets, so its dimension matches the post-update state and updateFromPrior
    // never hits a dimension mismatch.  have_applied (checked above) guarantees
    // it is fresh.  The references stay valid across the loop — updateFromPrior
    // does not touch the stash (only update() writes it).
    const Eigen::VectorXd& prior_x = kalman_filter_->getLastUpdatePriorX();
    const Eigen::MatrixXd& prior_P = kalman_filter_->getLastUpdatePriorP();
    if (prior_x.size() == 0 || prior_P.size() == 0) {
        log_manager_.getLogger("main_log")->debug(
            "{}Geometry relinearization (IEKF): no stashed prior; skipping", epoch_prefix);
        return;
    }

    // Copy (not reference) the converged exclusion set: applied_meas is
    // reassigned inside the loop, which would otherwise mutate the set out
    // from under the iteration.
    const std::set<std::string> fixed_excludes = applied_meas.excluded_measurements;
    constexpr int    kMaxIekfIters = 5;
    constexpr double kIekfPosTolM  = 1e-4;   // 0.1 mm: geometry change is sub-noise
    bool any_applied = false;

    for (int iter = 0; iter < kMaxIekfIters; ++iter) {
        // Sync ppp_state_ to the current iterate so computeModels recomputes
        // the geometry (sat positions, LOS, troposphere, corrections) about it.
        try {
            state_manager_->updatePppStateFromVector(
                state_manager_->getStateVector(), ppp_state_, *state_manager_);
        } catch (const std::exception& e) {
            log_manager_.getLogger("main_log")->error(
                "{}Relinearize(IEKF): failed to sync ppp_state: {}", epoch_prefix, e.what());
            break;
        }

        std::vector<PppObservation> relinear_obs;
        if (!ppp_model_->computeModels(epoch_obs, ppp_state_, relinear_obs) ||
            relinear_obs.empty()) {
            log_manager_.getLogger("main_log")->debug(
                "{}Geometry relinearization (IEKF): computeModels failed or empty (iter {})",
                epoch_prefix, iter);
            break;
        }

        EpochMeasurement relinear_meas = meas_model_->computeMeasurementUpdate(
            relinear_obs, state_manager_->getStateVector(), fixed_excludes);
        if (relinear_meas.innovations.size() == 0 ||
            !meas_model_->validateMeasurementUpdate(relinear_meas)) {
            log_manager_.getLogger("main_log")->debug(
                "{}Geometry relinearization (IEKF): empty/invalid measurement (iter {})",
                epoch_prefix, iter);
            break;
        }

        const Eigen::Vector3d pos_before = state_manager_->getStateVector().head<3>();
        const auto res = kalman_filter_->updateFromPrior(
            relinear_meas, prior_x, prior_P);
        if (res != KalmanFilter::UpdateResult::Applied) {
            log_manager_.getLogger("main_log")->debug(
                "{}Geometry relinearization (IEKF): updateFromPrior not applied (iter {}, result={})",
                epoch_prefix, iter, static_cast<int>(res));
            break;
        }
        any_applied = true;
        applied_meas = relinear_meas;

        const Eigen::Vector3d pos_after = state_manager_->getStateVector().head<3>();
        const double step = (pos_after - pos_before).norm();
        if (step < kIekfPosTolM) {
            log_manager_.getLogger("main_log")->debug(
                "{}Geometry relinearization (IEKF) converged at iter {} (step={:.2e} m)",
                epoch_prefix, iter, step);
            break;
        }
        if (iter + 1 == kMaxIekfIters) {
            log_manager_.getLogger("main_log")->debug(
                "{}Geometry relinearization (IEKF) hit iter cap {} (last step={:.4f} m)",
                epoch_prefix, kMaxIekfIters, step);
        }
    }

    if (any_applied) {
        ++relinearize_count_;
        try {
            state_manager_->updatePppStateFromVector(
                state_manager_->getStateVector(), ppp_state_, *state_manager_);
        } catch (const std::exception& e) {
            log_manager_.getLogger("main_log")->error(
                "{}Relinearize(IEKF): failed to sync after update: {}", epoch_prefix, e.what());
        }
        log_manager_.getLogger("main_log")->debug(
            "{}Geometry relinearization (IEKF) applied", epoch_prefix);
    }
}

// ---------------------------------------------------------------------------
// Epoch helper: Geometry relinearization — legacy naive second pass
// ---------------------------------------------------------------------------

void Application::runGeometryRelinearizationNaive(const std::string& epoch_prefix,
                                                  const std::vector<RinexObservation>& epoch_obs,
                                                  EpochMeasurement& applied_meas) {
    // A single extra full update() layered on top of the primary posterior.
    // Statistically this re-applies the same measurement information against
    // an already-tightened covariance (mild over-confidence); it is retained
    // ONLY for the tightly-coupled IMU path, where the proper prior-anchored
    // IEKF would conflict with the error-state nominal feedback.  The |Δpos|
    // and per-run-cap gates are evaluated by the caller.
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

    // Set IMU state for tightly-coupled Jacobian partials (relocated into
    // ImuAidingSource; the setImuState arithmetic is unchanged).
    if (aiding_manager_) aiding_manager_->applyTcJacobianInputs(*meas_model_);

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
    snap.iono_rates  = state_manager_->getIonosphereRateStates(); // CV model; empty otherwise
    snap.code_biases = state_manager_->getCodeBiasStates();
    snap.sat_biases  = state_manager_->getSatBiasStates();
    for (const auto& sys : state_manager_->getSystemsWithTimeOffsets()) {
        snap.sys_offsets[sys] = state_manager_->getSystemTimeOffsetIndex(sys);
    }

    snap.reset_satellites = reset_satellites;
    snap.is_discontinuity = was_complete_reset;

    // BSSD frame metadata for the backward pass (audit P2-1).  getBssdPivot-
    // Satellites is the FINAL per-epoch pivot map (after routine, forced-rebuild,
    // and slip-phase handovers); getCleanHandovers reports the undisturbed,
    // losslessly-invertible legs.  Both empty when BSSD is inactive.
    if (meas_model_) {
        snap.pivot_satellites = meas_model_->getBssdPivotSatellites();
        snap.clean_handovers  = meas_model_->getPivotManager().getCleanHandovers();
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
                                   const std::vector<RinexObservation>& epoch_obs,
                                   const std::vector<PppObservation>& modeled_obs,
                                   const std::map<std::string, DropReason>& drop_reasons,
                                   const EpochMeasurement& applied_meas,
                                   bool have_applied,
                                   const std::set<std::string>& reset_satellites) {
    // Trace files carry pure GPS time (week + tow_s in GPS scale) so the
    // timestamps line up exactly with RINEX epoch records.  Earlier code
    // here subtracted the GPS-UTC leap-second offset via gpsToUtcTime(),
    // which produced timestamps 18 s behind RINEX and required downstream
    // MATLAB consumers to add 18 s back -- two leap-second adjustments in
    // opposite directions that only worked correctly by accident.  No
    // adjustment is done anywhere now: trace = RINEX = GPS time.
    const GnssTime gps_time = current_time;
    trace_logger_->logState(gps_time, *state_manager_);
    trace_logger_->logAmbiguities(gps_time, *state_manager_);
    trace_logger_->logIonosphere(gps_time, *state_manager_);
    trace_logger_->logSatBiases(gps_time, *state_manager_);
    trace_logger_->logReceiverCodeBiases(gps_time, *state_manager_);
    if (have_applied) {
        trace_logger_->logResiduals(gps_time, applied_meas);
    }

    // Per-satellite epoch status: gather slip set from the CSM's last MW
    // detector results (only sats that slipped are cached in the map) and
    // combine with the KF-reset set passed in by the caller.  Both feed
    // the "Slip" status category in TraceLogger::logSatelliteStatus.  The
    // raw epoch_obs vector enables the logger to surface "BelowMask" rows
    // for sats that were tracked in RINEX but dropped by ModelGeometry.
    std::set<std::string> slipped_sats;
    if (cycle_slip_manager_) {
        for (const auto& [sat, mw_res] : cycle_slip_manager_->getLastMwSlips()) {
            (void)mw_res;
            slipped_sats.insert(sat);
        }
    }
    trace_logger_->logSatelliteStatus(
        gps_time, epoch_obs, modeled_obs, drop_reasons, applied_meas, have_applied,
        *state_manager_, slipped_sats, reset_satellites,
        config_.getElevationMaskDeg());

    // Per-epoch DOPs (PDOP/HDOP/VDOP/TDOP/GDOP) and ambiguity DOP, plus
    // per-system + total used-sat counts.
    trace_logger_->logDops(
        gps_time, modeled_obs, applied_meas, have_applied, *state_manager_);
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

// --- Branch on processing mode: the network PRODUCER takes the joint
//     multi-station pipeline.  PPP_RTK_User flows through the normal PPP
//     path below as an uncombined PPP-RTK consumer. ---
if (config_.getProcessingMode() == ProcessingMode::PPP_RTK_Network) {
    return runNetwork();
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

    // GIM construction.  Two independent consumers can request it:
    //   - estimation.ionosphere_weighting.enabled  → soft pseudo-obs on
    //     the per-satellite slant iono state (PPP-RTK).
    //   - model_corrections.enable_higher_order_ionosphere → STEC source
    //     for the I₂ + I₃ correction applied per-signal in PppModel.
    // Either flag triggers the IONEX parse; downstream code uses
    // gim_->isLoaded() to decide whether its work runs.
    const bool need_gim_for_weighting =
        config_.getIonosphereWeightingConfig().enabled;
    const bool need_gim_for_higher_order =
        config_.getModelCorrections().enable_higher_order_ionosphere;
    if (need_gim_for_weighting || need_gim_for_higher_order) {
        gim_ = std::make_unique<GlobalIonosphereMap>(
            config_.getProductPath("INX").string(), log_manager_);
        if (!gim_->isLoaded()) {
            log_manager_.getLogger("ionosphere")->warn(
                "GIM is required by the configuration ({}{}{}) but no usable "
                "IONEX file resolved — the dependent feature(s) will be skipped. "
                "Set data_download.download_gim (production mode) or place an "
                ".INX file under the date's products/ directory (internal mode). "
                "For bias consistency, source the GIM from the same analysis "
                "centre as the code-bias (OSB) product.",
                need_gim_for_weighting ? "ionosphere_weighting" : "",
                (need_gim_for_weighting && need_gim_for_higher_order) ? " + " : "",
                need_gim_for_higher_order ? "higher_order_ionosphere" : "");
        } else if (need_gim_for_higher_order) {
            // PppModel needs the pointer to evaluate the I₂+I₃ correction
            // each epoch.  Pointer is non-owning; gim_ lifetime is the
            // Application's, which outlives ppp_model_ via destruction order.
            ppp_model_->setGim(gim_.get());
        }
    }

    // Adaptive ionospheric process-noise monitor (opt-in via
    // estimation.adaptive_iono_process_noise.enable).  Constructed only
    // when enabled; when absent the iono process noise stays static.
    if (config_.getAdaptiveIonoQConfig().enabled) {
        iono_variability_monitor_ =
            std::make_unique<IonoVariabilityMonitor>(config_, log_manager_);
    }

    // Cross-session normal-equation accumulator (item #13B).  Post-processing
    // only (the config loader forces enable_neq_export off in real-time mode).
    // Accumulates Hᵀ R⁻¹ H / Hᵀ R⁻¹ Δz from each accepted update, eliminating
    // epoch-local states by Schur reduction, and exports .neq / .asc at end of
    // run.  Filter state is never touched.
    if (config_.enableNeqExport()) {
        neq_accumulator_ = std::make_unique<NeqAccumulator>();
        log_manager_.getLogger("main_log")->info(
            "Normal-equation export enabled: accumulating cross-session NEQ "
            "(.neq/.asc will be written beside the navigation solution)");
    }

    // Provide BIA satellite biases to the MW and EWL detectors for integer rounding
    cycle_slip_manager_->setCorrections(&ppp_model_->getCorrections());

    // Adaptive MW arc-reset gap tolerance.  When IMU isn't bridging gaps
    // and the RINEX sampling interval is coarser than 5 s, any inter-epoch
    // gap longer than the nominal cadence is suspicious — tightening the
    // gap tolerance to the data rate prevents the MW detector from
    // silently initializing a fresh arc on a slip-corrupted sample after
    // a brief outage.  IMU mode keeps the default 120 s because INS
    // propagation legitimately bridges gaps that long.
    //
    // Apply a 1.5× margin to the nominal cadence so that a "perfect"
    // back-to-back epoch (gap == nominal interval) doesn't trip the
    // strict-greater-than comparison inside MwSlipDetector when
    // floating-point drift in (epoch_s - last_epoch_s) inevitably
    // produces values like 30.000000001 against a 30 s threshold.  A
    // real missed epoch produces a gap of ~2× the nominal cadence
    // (e.g. 60 s on a 30 s file) which still trips the 1.5× threshold,
    // so genuine outages are still caught.
    //
    // Honor processing.gnss_rate_s if the user opted into epoch
    // decimation — the effective inter-epoch spacing in MW is the
    // decimated cadence, not the underlying RINEX interval.
    {
        const double rinex_interval = rinex_parser_->getHeader().interval.value_or(0.0);
        const double effective_interval = (config_.getGnssRate() > 0.0)
                                              ? config_.getGnssRate()
                                              : rinex_interval;
        if (!config_.enableImu() && effective_interval > 5.0) {
            if (auto* mw = cycle_slip_manager_->mwDetector())
                mw->setMaxGapSeconds(effective_interval * 1.5);
        }
        // TurboEdit needs the cadence regardless of IMU mode: it tightens its
        // own GF-arc gap tolerance to the cadence and logs the coarse-rate
        // advisory (GF trust itself is evidence-gated per arc — see the
        // TurboEditResolver header's "Rate adaptivity" note).
        cycle_slip_manager_->setSampleInterval(effective_interval);
    }

    // Provide MW data to the KF outlier handler for tiebreaking.
    kalman_filter_->setCycleSlipManager(cycle_slip_manager_.get());

    const auto& logging_cfg = config_.getLoggingConfig();
    if (logging_cfg.enable_mw_slip_trace) {
        cycle_slip_manager_->enableMwTracing(log_dir_);
    }
    if (logging_cfg.enable_turbo_edit_trace) {
        cycle_slip_manager_->enableTurboEditTracing(log_dir_);
    }
    if (logging_cfg.enable_windup_trace && ppp_model_) {
        ppp_model_->enableWindUpTracing(log_dir_);
    }
    // Wind-up census (estimation.trace_windup_census): the same read-only
    // step/gap counters the network teardown sums across its roster, armed here
    // for the single-receiver paths.  carry_gap_s = 0 disables the leveling-carry
    // bucketing, which has no meaning off the network producer.
    if (config_.traceWindUpCensus() && ppp_model_) {
        ppp_model_->enableWindUpCensus(config_.phaseWindUpMaxGapS(), 0.0);
    }
    if (iono_variability_monitor_ && config_.getAdaptiveIonoQConfig().trace) {
        iono_variability_monitor_->enableTracing(log_dir_);
    }

    // Aiding sources (GNSS/INS coupling; the IMU is the first client, with
    // ZUPT/NHC/baro/... composing later via AidingSourceRegistry).  The
    // AidingSourceManager owns the config-selected set; ImuAidingSource wraps
    // the integrator (load + graceful GNSS-only fallback on failure, logged in
    // its ctor).  state_manager_ and kalman_filter_ are already constructed
    // above, so the IMU source can be wired immediately.
    aiding_manager_ = std::make_unique<AidingSourceManager>(config_, log_manager_);
    aiding_manager_->reserveAllStates(*state_manager_);   // every source stashes StateManager / reserves blocks
    if (auto* imu = aiding_manager_->imu()) {
        imu->attachFilter(*kalman_filter_);    // IMU-only: nominal-attitude owner handle (C_b_e_)
        if (imu->hasData() && imu->integrator()) {
            const char* mode_str = config_.tightlyCoupledImu()
                ? "tightly-coupled" : "loosely-coupled";
            log_manager_.getLogger("main_log")->info(
                "IMU integration enabled ({}): {} samples loaded",
                mode_str, imu->integrator()->sampleCount());
        }
    }

    ppp_model_->initializeState(ppp_state_);
    state_manager_->syncPppStateToStateVector(ppp_state_);

    // Ambiguity resolution (MLAMBDA)
    if (config_.getArMode() > 0) {
        AmbiguityResolverConfig ar_config;
        ar_config.ar_mode               = config_.getArMode();
        ar_config.ratio_threshold       = config_.getArRatioThreshold();
        ar_config.elevation_mask_ar     = config_.getArElevationMask();
        ar_config.hold_variance         = config_.getArHoldVariance();
        ar_config.min_epochs            = config_.getArMinEpochs();
        ar_config.max_position_sigma_m  = config_.getArMaxPositionSigma();
        ar_config.ratio_upper_bound     = config_.getArRatioUpperBound();
        ar_config.wl_only               = config_.getArWlOnly();
        ar_config.wl_only_pseudo_var    = config_.getArWlOnlyPseudoVar();
        ar_config.wl_max_frac           = config_.getArWlMaxFrac();
        ar_config.min_success_rate      = config_.getArMinSuccessRate();
        ar_config.wl_only_ffr           = config_.getArWlOnlyFfr();
        ar_config.wl_only_min_arc_count = config_.getArWlOnlyMinArcCount();
        ar_config.wl_only_max_innov_cyc = config_.getArWlOnlyMaxInnovCyc();
        ar_config.min_arc_info          = config_.getArMinArcInfo();
        ar_config.nl_max_per_pair_frac  = config_.getArNlMaxPerPairFrac();
        ar_config.nl_iono_free_row      = config_.getArNlIonoFreeRow();
        ar_config.enable_partial_ar     = config_.getArEnablePartialAr();
        ar_config.ar_active_systems     = config_.getArActiveSystems();
        ar_config.state_ambiguity_basis = config_.getStateAmbiguityBasis();
        ambiguity_resolver_ = std::make_unique<AmbiguityResolver>(ar_config, log_manager_);
        log_manager_.getLogger("main_log")->info(
            "Ambiguity resolution enabled: mode={}, wl_only={}, partial_ar={}, nl_row={}, "
            "ratio=[{:.1f}, {:.1e}], elev_mask={:.1f} deg, "
            "min_epochs={}, max_pos_σ={:.3f} m",
            ar_config.ar_mode, ar_config.wl_only ? "on" : "off", ar_config.enable_partial_ar ? "on" : "off",
            ar_config.nl_iono_free_row ? "iono-free" : "narrowlane",
            ar_config.ratio_threshold, ar_config.ratio_upper_bound,
            ar_config.elevation_mask_ar, ar_config.min_epochs, ar_config.max_position_sigma_m);
    }

    const bool is_ppp_rtk = (config_.getProcessingMode() == ProcessingMode::PPP_RTK);
    trace_logger_ = std::make_unique<TraceLogger>(log_manager_, log_dir_, is_ppp_rtk, config_.getLoggingConfig());
    // Only a FAILED set of requested opens is worth a warning; an unarmed set of
    // enable_*_trace toggles leaves the logger uninitialized by design.
    if (!trace_logger_->isInitialized() && trace_logger_->requestedStreamCount() > 0) {
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

        // 4a3. Optional max-duration cutoff (processing.max_duration_s).
        // Stop the run once the data-time span from the first processed
        // epoch reaches the configured limit — measured as epoch GPS time
        // relative to the first processed obs (e.g. 3600 s => one hour, or
        // ~3600 epochs of 1 s data), independent of replay speed.  The
        // first epoch anchors the window; it is reached only after any
        // leading decimation skip above, so the budget is not spent on
        // skipped epochs.  0 (default) = no limit.  Real-time uses its own
        // copy in RealTimeEngine::processEpoch.
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
                    break;
                }
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
        bool long_gap = false;
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

        // 4c. Sync PppState from the state vector.
        try {
            state_manager_->updatePppStateFromVector(state_manager_->getStateVector(), ppp_state_, *state_manager_);
        } catch (const std::exception& e) {
            log_manager_.getLogger("main_log")->error("{}Failed to update ppp_state: {}", epoch_prefix, e.what());
            return 1;
        }

        // 4d. Compute modeled observations.
        // drop_reasons is populated by computeModels for every sat that
        // fails to make it into modeled_obs (BelowMask / NoProduct /
        // NoModel).  Consumed by writeEpochTraces -> logSatelliteStatus
        // so the strip chart distinguishes actionable failure modes.
        std::vector<PppObservation> modeled_obs;
        std::map<std::string, DropReason> drop_reasons;
        if (!ppp_model_->computeModels(epoch_obs, ppp_state_, modeled_obs, &drop_reasons)) {
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
        const auto wls_result = handleWlsSeeding(current_time, epoch_prefix, modeled_obs, gap_detected);

        // 4e1. Gap recovery policy.
        // Runs after the clock has been re-seeded so the gap helpers
        // edit covariance against the post-seed state.  No-op when
        // gap_detected is false.  Skipped on the first epoch because
        // first_epoch_ → gap_detected is intentionally false (dt
        // wasn't actually computed against a prior epoch).
        if (gap_detected) {
            handleGap(dt, long_gap, epoch_prefix, wls_result);
        }

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
        {
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

        // 4f. Kalman prediction.
        if (aiding_manager_ && !aiding_manager_->empty() && !first_epoch_) {
            // Aiding-augmented prediction (IMU strapdown today).  predictAided
            // delegates to predict(dt) when no source contributes this interval
            // (e.g. no IMU samples), matching the legacy GNSS-only fallback.
            // The state vector is held in a local so the context's reference
            // outlives collectPredict.
            const Eigen::VectorXd aiding_x = state_manager_->getStateVector();
            const AidingEpochContext aiding_ctx{
                aiding_x, prev_epoch_time_, current_time, first_epoch_ };
            kalman_filter_->predictAided(dt, aiding_manager_->collectPredict(aiding_ctx));
        } else {
            kalman_filter_->predict(dt);
        }
        // Predict is the last site in the epoch body that reads first_epoch_;
        // clear unconditionally here so the flag flips exactly once per run
        // regardless of WLS success or whether the mode owns a clock state
        // (BSSD has neither).
        first_epoch_ = false;

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
        const CycleSlipEpochResult slip_result =
            detectCycleSlips(epoch_prefix, modeled_obs);

        // Smoother: fold covariance-preserving repairs into the just-captured
        // a-priori as a deterministic control input (audit F1).  A repair shifts
        // the ambiguity by a known dN·λ in the SAME (pre-update) frame x_pred was
        // captured in; adding it to x_pred prevents the backward pass from
        // absorbing the jump as a spurious measurement innovation.  Indices
        // resolve against the live layout, which still equals x_pred's layout
        // here (predict + slip detection do not reindex existing keys; state
        // growth happens later inside the update loop).  A key also RESET this
        // epoch is segmented, not compensated (R6).
        if (config_.runSmoother() && !smoother_buffer_.empty()
            && !slip_result.ambiguity_repairs.empty()) {
            SmootherEpochSnapshot& snap = smoother_buffer_.back();
            for (const auto& [amb_key, delta_m] : slip_result.ambiguity_repairs) {
                const auto us = amb_key.find('_');
                const std::string sat = (us != std::string::npos)
                                            ? amb_key.substr(0, us) : amb_key;
                if (slip_result.reset_satellites.count(sat)) continue;  // segmented
                const int idx = state_manager_->getAmbiguityIndex(amb_key);
                if (idx >= 0 && static_cast<Eigen::Index>(idx) < snap.x_pred.size())
                    snap.x_pred(idx) += delta_m;
            }
        }

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
        // Merge the slip-phase resets (SD-aware resets, pivot-disturbance
        // inflations, soft-LLI bridge restores, post-handover deferred repairs)
        // so the smoother segments them too — previously only KF-internal resets
        // reached the snapshot (audit F1).
        epoch_reset_sats.insert(slip_result.reset_satellites.begin(),
                                slip_result.reset_satellites.end());

        // 4h2. Ionosphere weighting: apply the GIM as a between-satellite
        // soft constraint on the slant-iono states.  Post-processing path
        // only — runRealTime() never reaches here.  Placed after the
        // update loop so the smoother posterior capture below records the
        // GIM-constrained state, and before AR so fixing benefits from it.
        applyGimIonoUpdate(current_time, modeled_obs);

        // Smoother posterior capture: taken BEFORE AR so the smoother
        // operates on float ambiguities (v1 policy).  AR-fixed states
        // are applied below for the forward solution only.
        capturePosteriorSnapshot(epoch_reset_sats, epoch_complete_reset, have_applied);

        // Cross-session NEQ accumulation (item #13B).  Mirrors the accepted
        // update's (H, R, innovation) into a label-keyed normal system; the
        // state labels are captured here so they match applied_meas.H columns
        // (no state resize occurs between the accepted update and this point).
        // Pure sibling consumer — does not touch the filter state.
        if (neq_accumulator_ && have_applied) {
            const std::vector<std::string> state_labels = state_manager_->getStateLabels();
            if (static_cast<Eigen::Index>(state_labels.size()) == applied_meas.H.cols()) {
                neq_accumulator_->accumulate(applied_meas, state_labels, epoch_reset_sats);
            } else {
                // Should never fire (StateManager only appends/prunes, so
                // labels are positionally aligned with H columns); warn rather
                // than debug so a broken invariant is visible without enabling
                // debug logging — a silent per-epoch drop would corrupt the NEQ.
                log_manager_.getLogger("main_log")->warn(
                    "{}NEQ accumulation skipped: state labels ({}) != H columns ({})",
                    epoch_prefix, state_labels.size(), applied_meas.H.cols());
            }
        }

        // Soft-LLI bridge: roll the "last good" ambiguity snapshot from the
        // converged FLOAT posterior (before AR fix-and-hold tightens the
        // covariance).  Only converged keys update it, so it holds the pre-gap
        // anchor a later LLI hold repairs against.  No-op unless
        // turbo_edit_bridge_lli.
        if (cycle_slip_manager_)
            cycle_slip_manager_->recordAmbiguitySnapshot(
                *state_manager_, TimeUtils::continuousGpsSeconds(current_time));

        // 4i2. Integer ambiguity resolution (MLAMBDA).
        if (ambiguity_resolver_ && have_applied) {
            auto wl_ambs  = cycle_slip_manager_->getResolvedWlAmbiguities();
            auto wl_fracs = cycle_slip_manager_->getResolvedWlFractions();

            // Optional AR-side ATX gates: drop WL entries based on ATX
            // coverage status of either signal in the pair.  The
            // observations themselves stay in the float Kalman update —
            // we only forbid integer fixing for these signals because
            // missing or approximate antenna PCV introduces sub-cycle
            // errors that bias the integer determination.
            //
            // Two complementary gates:
            //   require_atx_coverage_for_ar   — drop pairs lacking STRICT
            //                                   coverage on either signal
            //                                   (substituted OR missing).
            //   restrict_ar_for_substituted_atx — drop pairs where either
            //                                   signal's ATX correction was
            //                                   produced via the
            //                                   frequency-fallback chain
            //                                   (substituted only — pairs
            //                                   with no ATX at all are
            //                                   allowed through).
            // Use either, both, or neither depending on policy.
            const auto& mc = config_.getModelCorrections();
            const bool gate_strict      = mc.require_atx_coverage_for_ar;
            const bool gate_substituted = mc.restrict_ar_for_substituted_atx;
            if ((gate_strict || gate_substituted) && ppp_model_) {
                int n_dropped_strict      = 0;
                int n_dropped_substituted = 0;
                for (auto it = wl_ambs.begin(); it != wl_ambs.end(); ) {
                    // Key format: "<sat>_<sig1>_<sig2>".
                    const auto& key = it->first;
                    const auto u1 = key.find('_');
                    const auto u2 = (u1 == std::string::npos) ? std::string::npos
                                                                : key.find('_', u1 + 1);
                    if (u1 == std::string::npos || u2 == std::string::npos) {
                        ++it; continue;
                    }
                    const std::string sat  = key.substr(0, u1);
                    const std::string sig1 = key.substr(u1 + 1, u2 - u1 - 1);
                    const std::string sig2 = key.substr(u2 + 1);
                    const System sys = sat.empty() ? System::Unknown :
                        sat[0] == 'G' ? System::GPS :
                        sat[0] == 'E' ? System::Galileo :
                        sat[0] == 'R' ? System::GLONASS :
                        sat[0] == 'C' ? System::BeiDou :
                        sat[0] == 'J' ? System::QZSS :
                        sat[0] == 'S' ? System::SBAS : System::Unknown;
                    bool drop = false;
                    if (gate_strict) {
                        const bool a1 = ppp_model_->hasFullAtxCoverage(sat, sig1, sys);
                        const bool a2 = ppp_model_->hasFullAtxCoverage(sat, sig2, sys);
                        if (!a1 || !a2) { drop = true; ++n_dropped_strict; }
                    }
                    if (!drop && gate_substituted) {
                        const bool s1 = ppp_model_->wasAtxSubstituted(sat, sig1, sys);
                        const bool s2 = ppp_model_->wasAtxSubstituted(sat, sig2, sys);
                        if (s1 || s2) { drop = true; ++n_dropped_substituted; }
                    }
                    if (drop) {
                        wl_fracs.erase(key);
                        it = wl_ambs.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (n_dropped_strict > 0 || n_dropped_substituted > 0) {
                    log_manager_.getLogger("main_log")->debug(
                        "{}AR ATX gate: dropped {} (strict-coverage), {} (substituted) WL entries",
                        epoch_prefix, n_dropped_strict, n_dropped_substituted);
                }
            }

            // WL-only FFR gate input (see AmbiguityResolverConfig::wl_only_ffr).
            // Built from the unfiltered detector map on purpose: entries whose
            // integer the ATX gate above dropped simply never get looked up,
            // because no constraint is formed for them.
            std::map<std::string, WlArcQuality> wl_quality;
            for (const auto& [k, q] : cycle_slip_manager_->getResolvedWlQuality())
                wl_quality[k] = WlArcQuality{ q.sigma_mean_cycles, q.count, q.exact };
            auto ar_result = ambiguity_resolver_->resolve(*state_manager_, modeled_obs, wl_ambs, wl_fracs, wl_quality);
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
        writeEpochTraces(current_time, epoch_obs, modeled_obs, drop_reasons,
                         applied_meas, have_applied, epoch_reset_sats);
    }

    // --- Wind-up census readout (estimation.trace_windup_census) ------------
    // Single-receiver counterpart of the network teardown's roster sum.  This
    // is the path SPEC_phase_windup_sign §7's 43-jumps/day figure came from, so
    // an armed PPP run is the cross-check on the network census's calibration.
    if (config_.traceWindUpCensus() && ppp_model_) {
        const auto& w = ppp_model_->windUpCensus();
        auto wu_log = log_manager_.getLogger("ppp_model");
        const bool acc = config_.accumulatePhaseWindUp();
        if (wu_log) {
            wu_log->info("Wind-up census (mode {}, horizon {:.0f} s): {} forward "
                         "transitions ({} out-of-order, excluded).  IN-horizon: {} "
                         "transitions, {} step(s) >= 0.9 cyc, mean |step| {:.4f} cyc, "
                         "max {:.3f}.  OUT-of-horizon: {} transitions, {} stepped "
                         ">= 0.9 cyc (max {:.3f}).  Accumulator RESTARTS (its own "
                         "verdict, not a threshold): {}.",
                         acc ? "ACCUMULATED" : "fractional",
                         config_.phaseWindUpMaxGapS(), w.steps, w.backwards,
                         w.gap_lt_wu, w.wraps,
                         w.gap_lt_wu > 0
                             ? w.sum_abs_step_cyc / static_cast<double>(w.gap_lt_wu)
                             : 0.0,
                         w.max_abs_step_in_cyc,
                         w.steps - w.gap_lt_wu, w.gap_steps, w.max_abs_step_gap_cyc,
                         w.restarts);
            wu_log->info("Wind-up census READING: {}  0 transitions on an armed run = "
                         "dead wiring (invariant #4).",
                         acc ? "`wraps` is 0 by CONSTRUCTION in accumulated mode — every "
                               "in-horizon transition is unwrapped into ±0.5 cyc, so a "
                               "threshold on the applied step is blind; the restart "
                               "population is the count reported above, and each restart "
                               "steps the model by an exact integer cycle."
                             : "the in-horizon steps >= 0.9 cyc ARE the one-cycle "
                               "staircase (SPEC_phase_windup_sign §7); this is the "
                               "single-receiver cross-check on the network census, and "
                               "the path §7's own 43/day figure came from.");
        }
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

    // --- Phase 5b: Cross-session normal-equation export (item #13B) ---
    // The accumulator mirrored each accepted update's (H, R, innovation) into a
    // label-keyed normal system with the epoch-local states Schur-eliminated.
    // x̂ = N⁻¹u is the Gauss-Newton INCREMENT about the per-epoch operating
    // points (≈0 for a converged static run, confirming the filter solution is
    // a stationary point of the batch system); absolute coordinates are the
    // final filter state plus that increment.  N, u, the labels and σ̂₀² are
    // the combinable product written to .neq/.asc.
    if (neq_accumulator_ && !neq_accumulator_->empty()) {
        const NeqAccumulator::Solution sol = neq_accumulator_->solve();
        const std::filesystem::path stem = log_dir_ / "normal_equations";
        const bool wrote = neq_accumulator_->writeFiles(stem, sol);
        auto neq_log = log_manager_.getLogger("main_log");
        if (sol.ok) {
            double dx = 0.0, dy = 0.0, dz = 0.0;
            for (std::size_t i = 0; i < sol.labels.size(); ++i) {
                const auto& lab = sol.labels[i];
                const double xi = sol.x(static_cast<Eigen::Index>(i));
                if (lab == "POS_X") dx = xi;
                else if (lab == "POS_Y") dy = xi;
                else if (lab == "POS_Z") dz = xi;
            }
            const Eigen::Vector3d final_pos =
                state_manager_->getStateVector().head<3>();
            const double incr = std::sqrt(dx * dx + dy * dy + dz * dz);
            neq_log->info(
                "NEQ export: {} params, {} obs, {} local eliminated, dof={:.1f}, "
                "sigma0^2={:.3f}",
                sol.n_params, sol.n_obs, sol.n_local, sol.dof, sol.sigma0_sq);
            neq_log->info(
                "NEQ position: increment |dx_pos|={:.4f} m, "
                "absolute=({:.4f}, {:.4f}, {:.4f}) m",
                incr, final_pos.x() + dx, final_pos.y() + dy, final_pos.z() + dz);
        } else {
            neq_log->warn(
                "NEQ export: system not solvable ({} params, {} obs); "
                "matrices still written for inspection",
                sol.n_params, sol.n_obs);
        }
        if (wrote)
            neq_log->info("Normal equations written: {}.neq / {}.asc",
                          stem.string(), stem.string());
        else
            neq_log->error("Failed to write normal-equation export to {}.*",
                           stem.string());
    }

    auto wall_end = std::chrono::high_resolution_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(wall_end - wall_start).count();
    log_manager_.getLogger("main_log")->info("Processing completed in {} ms, mode={}",
        duration_ms, is_ppp_rtk ? "PPP-RTK" : "PPP");

    if (kalman_filter_) kalman_filter_->logKalmanFilterTimings();

    // Optional end-of-run HTML report (pyviz/run_report.py -> gnss_report.html).
    generateReport_();

    return 0;
}

void Application::generateReport_() {
    if (!config_.autoReport()) return;
    // Ensure all buffered trace records are on disk before the report process
    // reads them (MW / TurboEdit traces already flush per-record).
    if (trace_logger_) trace_logger_->flush();
    generateReportFor_(std::filesystem::absolute(log_dir_));
}

void Application::generateReportFor_(const std::filesystem::path& trace_dir) {
    namespace fs = std::filesystem;
    auto log = log_manager_.getLogger("main_log");

    const fs::path root   = fs::path(GNSS_PROJECT_ROOT);
    const fs::path script = root / "pyviz" / "run_report.py";
    if (!fs::exists(script)) {
        if (log) log->warn("auto_report: {} not found - skipping report generation",
                           script.string());
        return;
    }

    // Interpreter: explicit override, else baked venv, else PATH "python".
    std::string python = config_.reportPython();
    if (python.empty()) {
        const fs::path venv_py = root / "venv" / "Scripts" / "python.exe";
        python = fs::exists(venv_py) ? venv_py.string() : std::string("python");
    }

    const fs::path log_abs = fs::absolute(trace_dir);
    const fs::path out     = log_abs / "gnss_report.html";
    const std::string backend = config_.reportBackend().empty()
                                  ? std::string("matplotlib") : config_.reportBackend();

    // Quote every path (handles spaces).  On Windows std::system pipes the
    // string through cmd /c, which strips the outermost quotes -- so the whole
    // command needs an extra wrapping pair when it already has quoted tokens.
    std::string inner = "\"" + python + "\" \"" + script.string() + "\" \""
                      + log_abs.string() + "\" -o \"" + out.string()
                      + "\" --backend " + backend;

    // Curated viewer subset (empty => render all).
    if (!config_.reportViewers().empty()) {
        inner += " --only";
        for (const std::string& v : config_.reportViewers())
            inner += " " + v;
    }

    if (log) log->info("auto_report: generating {} (backend={}, {})", out.string(),
                       backend, config_.reportAsync() ? "async" : "sync");

#ifdef _WIN32
    // Async: 'start' launches detached so the run returns without waiting.  The
    // empty "" is start's (required) window-title argument.
    const std::string cmd = config_.reportAsync()
        ? "start \"\" /b " + inner
        : "\"" + inner + "\"";
#else
    const std::string cmd = config_.reportAsync() ? inner + " &" : inner;
#endif

    const int rc = std::system(cmd.c_str());
    if (config_.reportAsync()) {
        if (log) log->info("auto_report: launched in background -> {}", out.string());
    } else if (rc == 0) {
        if (log) log->info("auto_report: wrote {}", out.string());
    } else {
        if (log) log->warn("auto_report: report generation exited with code {} "
                          "(is pyviz set up? see pyviz/README.md)", rc);
    }
}

} // namespace GnssProcessor
