/**
 * @file Application.cpp
 * @brief Top-level application driver: the mode dispatch and the PPP / PPP-RTK pipeline
 *        spine.
 *
 * run() loads the configuration and dispatches on the mode: real time
 * (ApplicationRealtime.cpp), baseline (ApplicationBaseline.cpp), the network producer
 * (ApplicationNetwork.cpp), or the PPP / PPP-RTK pipeline, whose spine is runPpp().
 * runPpp() is a short ordered list of named stages (card UOM step U7b); each stage lives
 * in the file of its concern:
 *  - PppSetup.cpp         the a-priori position, the components and their wiring;
 *  - PppEpochSteps.cpp    one epoch up to the measurement update: the observations, the
 *                         decimation and duration gates, the time step and data gap, the
 *                         models, the WLS clock seed, the gap policy, the process-noise
 *                         overrides, the Kalman predict, the cycle slips;
 *  - PppEpochUpdate.cpp   the measurement update loop, the geometry relinearization, the
 *                         merged resets, the GIM update, ambiguity resolution;
 *  - PppEpochRecords.cpp  the smoother snapshots, the NEQ accumulation, the soft-LLI
 *                         snapshot, the summary line and the CSV traces;
 *  - PppRunEnd.cpp        the run end: the reports of kPppRunEndReports in order.
 * The values one epoch's stages share are the members of PppEpochWork
 * (app/PppPipelineWork.hpp).  This file also holds the input phases the modes share
 * (initLoggingAndConfig, resolveData, refreshAtxIfEnabled_, loadInputData) and the
 * end-of-run HTML report.
 *
 * Return value semantics for run():
 *  - 0: success
 *  - non-zero: various error conditions during startup or runtime.
 */

#include "app/Application.hpp"
#include "app/PppPipelineWork.hpp"   // PppEpochWork, PppRunEndView: the spine's working values
#include "products/PathResolver.hpp"
#include "products/ProductCache.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Application::Application(const std::filesystem::path& config_path)
    : config_path_(config_path) {
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

            // Report loggers that are enabled but whose level is above info.
            // `enable_<logger>` and `levels.<logger>` are independent, and on an
            // inherited config chain they often come from different files: a child
            // that sets `enable_ambiguity_resolver: true` can inherit
            // `levels.ambiguity_resolver: "warn"` from a base, and the run then
            // writes an empty log with no diagnostic.  One line at startup names
            // every such logger.
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
// PathResolver dispatches on data_source: internal mode globs the bundled
// data/ tree, production mode delegates to ProductResolver and downloads
// from CDDIS.  This function applies the returned paths to GnssConfig, so
// the rest of the pipeline sees a populated config regardless of which
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

    // Baseline RINEX paths (PPP and PPP-RTK runs ignore these fields).
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

    // Interpreter: the report_python override if set, else the project venv, else
    // "python" from PATH.
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
    // std::system runs the command through cmd /c, which strips the outermost
    // quotes, so the already-quoted command gets an extra pair (same idiom as
    // generateReportFor_).
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

    // loadProducts() is called after RINEX parsing, not here: product loading
    // depends on the RINEX epoch span.
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
// Main entry point
// ---------------------------------------------------------------------------

int Application::run() {
    // --- Phase 1: Logging & configuration ---
    if (!initLoggingAndConfig()) return 1;

    // Apply the Eigen thread count.  The config default is 1 (sequential): PPP-RTK
    // and baseline matrix sizes (n≈350, m≈250) are below the size at which the
    // parallel speedup offsets OpenMP fork/join overhead (vcomp140 idle-at-barrier
    // measured at about 13 % of the run).  processing.eigen_threads = 0 selects the
    // OpenMP default; any N > 1 selects N threads.
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

    // --- Branch on processing mode: the network producer takes the joint
    //     multi-station pipeline.  PPP_RTK_User takes the PPP path below as an
    //     uncombined PPP-RTK consumer. ---
    if (config_.getProcessingMode() == ProcessingMode::PPP_RTK_Network) {
        // The network producer signals a refused start by throwing
        // (`network.state_snapshot_require` on any warm-resume refusal, the
        // carry_vce_apply warm-resume block, a carry blob that does not bind).
        // main() has no handler, so an uncaught throw aborts with exit code
        // 0xC0000409 and no reason in the log, where the last line is the
        // preceding cold-start warning.  The handler logs e.what() as a single
        // error line and returns 1.  A successful run never reaches it, so its
        // output is byte-identical.  See docs/SPEC_event_harness.md §6b (HB-1).
        try {
            return runNetwork();
        } catch (const std::exception& e) {
            auto log = log_manager_.getLogger("main_log");
            log->error("{}", e.what());
            log->flush();
            return 1;
        }
    }

    // --- The PPP / PPP-RTK pipeline: the spine below ---
    return runPpp();
}

// ---------------------------------------------------------------------------
// The PPP / PPP-RTK pipeline: the spine
// ---------------------------------------------------------------------------

int Application::runPpp() {
    // --- Phase 1b: Resolve (download/cache) product and RINEX data ---
    if (!resolveData()) return 1;

    // --- Phase 2: Parse RINEX & load products ---
    if (!loadInputData()) return 1;

    // --- Phase 3: Create processing components ---
    // A hint only: outside baseline mode the StateManager constructor derives the core
    // layout from the configuration (no receiver clock under BSSD; velocity, gradients,
    // IMU and the inter-system offsets when armed).
    constexpr size_t core_state_size = 5;
    state_manager_ = std::make_unique<StateManager>(core_state_size, log_manager_, config_);
    state_manager_->logFamiliesAsArmed();   // run-start table "state families as armed"

    if (!initializePosition()) return 1;

    // The components and their wiring, in this order (PppSetup.cpp).
    buildPppComponents_();          // PppModel, MeasurementModel, KalmanFilter, WLS, CycleSlipManager
    buildGlobalIonosphereMap_();    // the GIM, for ionosphere weighting or the higher-order term
    buildIonoVariabilityMonitor_(); // estimation.adaptive_iono_process_noise
    buildNeqAccumulator_();         // processing.enable_neq_export
    wireCycleSlipManager_();        // biases, MW gap tolerance, sample interval, KF tiebreak
    enablePppTraceStreams_();       // MW, TurboEdit, wind-up, census and iono-monitor traces
    buildAidingSources_();          // the IMU and the other aiding sources
    initializePppState_();          // PppModel::initializeState; the SIF a-priori re-applied
    buildAmbiguityResolver_();      // MLAMBDA, when ambiguity_resolution.mode > 0

    const bool is_ppp_rtk = (config_.getProcessingMode() == ProcessingMode::PPP_RTK);
    buildTraceLogger_(is_ppp_rtk);  // the CSV trace streams

    log_manager_.getLogger("main_log")->info("Processing components initialized, mode={}",
        is_ppp_rtk ? "PPP-RTK" : "PPP");

    // --- Phase 4: Epoch processing loop ---
    const auto& observations = rinex_parser_->getObservations();
    auto wall_start = std::chrono::high_resolution_clock::now();

    // One epoch: the stages of PppEpochSteps.cpp, PppEpochUpdate.cpp and
    // PppEpochRecords.cpp, in this order, over the epoch's PppEpochWork.
    for (size_t epoch_idx = 0; epoch_idx < observations.size(); ) {
        PppEpochWork w;
        // 4a. Collect epoch observations (advances epoch_idx).
        if (!collectEpochObservations(observations, epoch_idx, w.epoch_obs, w.current_time))
            continue;
        if (skipDecimatedEpoch_(w)) continue;   // 4a2 processing.gnss_rate_s decimation
        if (reachedMaxDuration_(w)) break;      // 4a3 processing.max_duration_s cutoff
        stepEpochClock_(w);                     // 4b  epoch stamp, time step, data gap

        // 4c. Sync PppState from the state vector.
        if (!syncPppState_(w)) return 1;
        if (!computeEpochModels_(w)) continue;  // 4d  modeled observations, drop reasons

        // 4e. WLS estimation & clock seeding.
        w.wls_result = handleWlsSeeding(w.current_time, w.epoch_prefix, w.modeled_obs, w.gap_detected);

        // 4e1. Gap recovery policy.
        // Runs after the clock has been re-seeded, so the gap helpers edit
        // the covariance of the post-seed state.  No-op when gap_detected is
        // false, which includes the first epoch (dt is not computed against a
        // prior epoch there).
        if (w.gap_detected) {
            handleGap(w.dt, w.long_gap, w.epoch_prefix, w.wls_result);
        }

        refreshSubstitutedAtxQ_(w);             // 4e2 ATX-substituted ambiguity Q overrides
        refreshAdaptiveIonoQ_(w);               // 4e3 adaptive ionospheric Q overrides
        predictEpoch_(w);                       // 4f  Kalman prediction; first_epoch_ cleared

        // Smoother prior capture: after predict() and before any cycle-slip or
        // update-driven state change, so the recorded a-priori equals
        // F * x_post_{prev}.
        capturePriorSnapshot(w.current_time, w.dt);
        if (!syncPppState_(w)) return 1;        //     ppp_state_ after the predict

        // 4g. Cycle slip detection & ambiguity resets.
        w.slip_result =
            detectCycleSlips(w.epoch_prefix, w.modeled_obs);
        foldSlipRepairsIntoPrior_(w);           //     repairs into the smoother a-priori

        // 4h. Iterative KF measurement update loop.
        runFilterUpdateLoop(w.epoch_prefix, w.modeled_obs, w.applied_meas, w.have_applied,
                            w.epoch_reset_sats, w.epoch_complete_reset);

        // 4i. Optional geometry relinearization.
        runGeometryRelinearization(w.epoch_prefix, w.epoch_obs, w.applied_meas, w.have_applied);
        mergeEpochResets_(w);                   //     KF, relinearization and slip resets

        // 4h2. Ionosphere weighting: apply the GIM as a between-satellite
        // soft constraint on the slant-iono states.  Post-processing only
        // (runRealTime() does not reach this point).  It runs after the update
        // loop so the smoother posterior capture below records the
        // GIM-constrained state, and before AR so fixing uses it.
        applyGimIonoUpdate(w.current_time, w.modeled_obs);

        // Smoother posterior capture, taken before AR so the smoother operates
        // on float ambiguities (v1 policy).  AR-fixed states are applied below
        // to the forward solution only.
        capturePosteriorSnapshot(w.epoch_reset_sats, w.epoch_complete_reset, w.have_applied);

        accumulateNormalEquations_(w);          //     processing.enable_neq_export
        recordBridgeSnapshot_(w);               //     the soft-LLI bridge's last-good snapshot
        resolveEpochAmbiguities_(w);            // 4i2 integer ambiguity resolution (MLAMBDA)

        // 4k. Per-epoch logging & traces.
        logEpochSummary(w.epoch_prefix);
        writeEpochTraces(w.current_time, w.epoch_obs, w.modeled_obs, w.drop_reasons,
                         w.applied_meas, w.have_applied, w.epoch_reset_sats);
    }

    // --- Phase 5: the run end, the reports of kPppRunEndReports in order (PppRunEnd.cpp) ---
    reportPppRunEnd_(PppRunEndView{ wall_start, is_ppp_rtk });

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

    // Interpreter: the report_python override if set, else the project venv, else
    // "python" from PATH.
    std::string python = config_.reportPython();
    if (python.empty()) {
        const fs::path venv_py = root / "venv" / "Scripts" / "python.exe";
        python = fs::exists(venv_py) ? venv_py.string() : std::string("python");
    }

    const fs::path log_abs = fs::absolute(trace_dir);
    const fs::path out     = log_abs / "gnss_report.html";
    const std::string backend = config_.reportBackend().empty()
                                  ? std::string("matplotlib") : config_.reportBackend();

    // Every path is quoted (paths may contain spaces).  On Windows std::system
    // runs the string through cmd /c, which strips the outermost quotes, so a
    // command that already has quoted tokens gets an extra wrapping pair.
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
