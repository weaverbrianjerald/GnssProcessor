/*!
 * \file GnssConfigLoad.cpp
 * \brief JSON configuration loading split into per-section helpers.
 *
 * @c GnssConfig::loadFromJson lives here as a thin orchestrator that
 * opens the file, parses the JSON once, and dispatches to one private
 * member function per top-level config section.  Each section helper
 * is independently readable and follows the same shape:
 *
 *   1. Pull a section reference via @c detail::jsection(...) (or read
 *      a few scalars directly via @c detail::jval<T>(...)).
 *   2. Populate the corresponding GnssConfig private members.
 *   3. Emit one or two summary log lines on the @c gnss_config logger.
 *
 * The hierarchical/legacy-flat fallback chain — try the new dotted
 * path, fall back to the legacy top-level key, otherwise use a default
 * — is implemented once in @c GnssConfigJsonHelpers.hpp and reused by
 * every section.
 *
 * Behavior is unchanged from the previous monolithic
 * @c GnssConfig::loadFromJson; only the file layout has moved.  The
 * commented-out "smart defaults" block from the old file (PPP-RTK
 * sat-bias / require-bia coupling) has been deleted; git history
 * preserves it should it ever need to come back.
 */

#include "config/GnssConfig.hpp"
#include "config/GnssConfigJsonHelpers.hpp"
#include "config/BaselineFixtures.hpp"
#include <fstream>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

// CMake-baked absolute path to the project root.  Used as the default
// for @c files.internal_data_root in @c DataSource::Internal mode so
// fresh clones run out of the box without any path configuration.
// Defined in CMakeLists.txt via target_compile_definitions.
#ifndef GNSS_PROJECT_ROOT
#define GNSS_PROJECT_ROOT ""
#endif

namespace GnssProcessor {

    using detail::toLower;
    using detail::joinStrings;
    using detail::jval;
    using detail::jsection;

    // =================================================================
    // loadFromJson — orchestrator
    // =================================================================

    bool GnssConfig::loadFromJson(const std::filesystem::path& path, LogManager& log_manager) {
        try {
            std::ifstream file(path);
            if (!file.is_open()) {
                log_manager.getLogger()->error("Failed to open config file: {}", path.string());
                return false;
            }

            nlohmann::json json = nlohmann::json::parse(file,
                /*callback=*/ nullptr, /*allow_exceptions=*/ true, /*ignore_comments=*/ true);

            loadProcessingSection_(json, log_manager);
            loadEstimationSection_(json, log_manager);
            loadFilterSection_(json, log_manager);
            loadModelCorrectionsSection_(json, log_manager);
            loadReceiverSection_(json, log_manager);
            loadFilesSection_(json, log_manager);
            loadImuSection_(json, log_manager);
            loadDataDownloadSection_(json, log_manager);
            loadAppModeAndRealtimeSection_(json, log_manager);
            if (isBaselineMode()) {
                if (!loadBaselineSection_(json, log_manager)) return false;
            }
            loadLoggingSection_(json, log_manager);

            logConfigurationSummary_(log_manager);
            return true;
        }
        catch (const std::exception& e) {
            log_manager.getLogger()->error("Failed to parse config file {}: {}", path.string(), e.what());
            return false;
        }
    }

    // =================================================================
    //  Section: processing
    // =================================================================
    void GnssConfig::loadProcessingSection_(const nlohmann::json& json, LogManager& log_manager) {
        auto log = log_manager.getLogger("gnss_config");
        {
            std::string mode = jval<std::string>(json, "processing.mode", "processing_mode", "PPP");
            // Accept hyphen / underscore variants and different casing.
            std::string norm = toLower(mode);
            for (auto& c : norm) if (c == '_') c = '-';
            if      (norm == "ppp")              processing_mode_ = ProcessingMode::PPP;
            else if (norm == "ppp-rtk")          processing_mode_ = ProcessingMode::PPP_RTK;
            else if (norm == "baseline-single")  processing_mode_ = ProcessingMode::BaselineSingle;
            else if (norm == "baseline-multi")   processing_mode_ = ProcessingMode::BaselineMulti;
            else {
                log->warn("Unknown processing mode '{}', defaulting to PPP", mode);
                processing_mode_ = ProcessingMode::PPP;
            }
            const char* mode_label =
                processing_mode_ == ProcessingMode::PPP              ? "PPP"             :
                processing_mode_ == ProcessingMode::PPP_RTK          ? "PPP-RTK"         :
                processing_mode_ == ProcessingMode::BaselineSingle   ? "Baseline-Single" :
                                                                       "Baseline-Multi";
            log->info("Processing mode: {}", mode_label);
        }
        {
            std::string raw = jval<std::string>(json, "processing.signal_mode", "signal_mode", "Dual");
            std::string low = toLower(raw);
            if      (low == "dual")   signal_mode_ = SignalMode::Dual;
            else if (low == "triple") signal_mode_ = SignalMode::Triple;
            else if (low == "all")    signal_mode_ = SignalMode::All;
            else { log->warn("Invalid signal_mode '{}', defaulting to Dual", raw); signal_mode_ = SignalMode::Dual; }
            log->info("Signal mode: {}", raw);
        }
        {
            std::string raw = jval<std::string>(json, "processing.mw_signals", "mw_signals", "filter");
            std::string low = toLower(raw);
            if      (low == "filter") mw_signal_mode_ = MwSignalMode::Filter;
            else if (low == "all")    mw_signal_mode_ = MwSignalMode::All;
            else { log->warn("Invalid mw_signals '{}', defaulting to filter", raw); mw_signal_mode_ = MwSignalMode::Filter; }
            log->info("MW signal mode: {}", raw);
        }
        {
            std::string raw = jval<std::string>(json, "processing.satellite_interpolation_mode",
                                                 "satellite_interpolation_mode", "Lagrange");
            std::string low = toLower(raw);
            if      (low == "lagrange") satellite_interpolation_mode_ = SatelliteInterpolationMode::Lagrange;
            else if (low == "finite")   satellite_interpolation_mode_ = SatelliteInterpolationMode::Finite;
            else if (low == "hermite")  satellite_interpolation_mode_ = SatelliteInterpolationMode::Hermite;
            else if (low == "spline")   satellite_interpolation_mode_ = SatelliteInterpolationMode::Spline;
            else { log->warn("Invalid satellite_interpolation_mode '{}', defaulting to Lagrange", raw);
                   satellite_interpolation_mode_ = SatelliteInterpolationMode::Lagrange; }
            log->info("Satellite Pos Vel interpolation mode: {}", raw);
        }
        {
            std::string raw = jval<std::string>(json, "processing.satellite_clock_interpolation_mode",
                                                 "satellite_clock_interpolation_mode", "Linear");
            std::string low = toLower(raw);
            if      (low == "linear")   satellite_clock_interpolation_mode_ = SatelliteClockInterpolationMode::Linear;
            else if (low == "lagrange") satellite_clock_interpolation_mode_ = SatelliteClockInterpolationMode::Lagrange;
            else { log->warn("Invalid satellite_clock_interpolation_mode '{}', defaulting to Linear", raw);
                   satellite_clock_interpolation_mode_ = SatelliteClockInterpolationMode::Linear; }
            log->info("Satellite Clock interpolation mode: {}", raw);
        }

        uncombined_enabled_ = jval<bool>(json, "processing.uncombined_enabled", "uncombined_enabled", false);
        if (processing_mode_ == ProcessingMode::PPP_RTK) {
            uncombined_enabled_ = true;
            log->info("PPP-RTK mode: forcing uncombined_enabled=true");
        }
        if (processing_mode_ == ProcessingMode::BaselineSingle
         || processing_mode_ == ProcessingMode::BaselineMulti) {
            // Baseline DD processes each signal independently: the
            // ionosphere-free combination used by combined PPP mode
            // mixes band codes into strings like "C1C_C2W" that don't
            // cross-match across receivers when they track different
            // L2 attributes (e.g. rover C2W, reference C2S).  DD
            // cancels ionosphere on its own for short baselines, so
            // IF combinations are neither needed nor wanted here.
            uncombined_enabled_ = true;
            log->info("Baseline mode: forcing uncombined_enabled=true (per-signal DD)");
        }
        elevation_mask_deg_ = jval<double>(json, "processing.elevation_mask_deg", "elevation_mask_deg", 10.0);
        data_gap_tolerance_s_ = jval<double>(json, "processing.data_gap_tolerance_s", "data_gap_tolerance_s", 0.0);
        gnss_rate_s_ = jval<double>(json, "processing.gnss_rate_s", "gnss_rate_s", 0.0);
        if (gnss_rate_s_ < 0.0) {
            log->warn("Negative gnss_rate_s ({:.3f}) ignored; decimation disabled", gnss_rate_s_);
            gnss_rate_s_ = 0.0;
        }
        if (gnss_rate_s_ > 0.0) {
            log->info("GNSS processing rate: {:.1f} s (input epochs will be decimated)", gnss_rate_s_);
        }
        leap_seconds_tai_   = jval<double>(json, "processing.leap_seconds_tai", "leap_seconds_tai", 37.0);

        // Relinearize geometry lives under processing (it affects the model loop).
        relinearize_geometry_ = jval<bool>(json, "processing.relinearize_geometry", "relinearize_geometry", false);
        relinearize_dpos_threshold_phase_m_ = jval<double>(json,
            "processing.relinearize_dpos_threshold_phase_m",
            "relinearize_dpos_threshold_phase_m", 1.0);
        relinearize_dpos_threshold_code_only_m_ = jval<double>(json,
            "processing.relinearize_dpos_threshold_code_only_m",
            "relinearize_dpos_threshold_code_only_m", 100.0);
        relinearize_max_count_        = jval<int>(json,
            "processing.relinearize_max_count",        "relinearize_max_count",        -1);

        // Eigen thread count.  Default 1 (sequential) — PPP-RTK matrix sizes
        // (n≈350, m≈250) sit below the threshold where OpenMP fork/join
        // overhead is absorbed by the parallel speedup.  0 = OpenMP default.
        eigen_threads_ = jval<int>(json, "processing.eigen_threads", "eigen_threads", 1);

        // Backward RTS smoother: post-processing only. Real-time guard
        // is applied by loadAppModeAndRealtimeSection_, once app_mode_
        // has been parsed.
        run_smoother_ = jval<bool>(json, "processing.run_smoother", "run_smoother", false);

        // Active systems
        active_systems_.clear();
        {
            const auto& arr = jsection(json, "processing.active_systems", "active_systems");
            if (arr.is_array())
                active_systems_ = arr.get<std::vector<std::string>>();
        }
        if (active_systems_.empty()) {
            active_systems_ = { "GPS", "Galileo", "GLONASS", "BeiDou", "QZSS", "SBAS" };
            log->info("No active systems specified, using default: GPS, Galileo, GLONASS, BeiDou, QZSS, SBAS");
        } else {
            for (const auto& system : active_systems_)
                if (system != "GPS" && system != "Galileo" && system != "GLONASS" &&
                    system != "BeiDou" && system != "QZSS" && system != "SBAS")
                    log->warn("Unknown system in active_systems: {}", system);
            log->info("Active systems: {}", joinStrings(active_systems_));
        }

        // Excluded satellites
        excluded_satellites_.clear();
        {
            const auto& arr = jsection(json, "processing.excluded_satellites", "excluded_satellites");
            if (arr.is_array())
                excluded_satellites_ = arr.get<std::vector<std::string>>();
        }
        if (!excluded_satellites_.empty())
            log->info("Excluded satellites: {}", joinStrings(excluded_satellites_));

        // Signal priorities
        static const std::map<std::string, std::vector<char>> system_bands = {
            {"GPS",     {'1','2','5'}},
            {"Galileo", {'1','5','7','6','8'}},
            {"GLONASS", {'1','2','3','4','6'}},
            {"BeiDou",  {'1','2','5','6','7','8'}},
            {"QZSS",    {'1','2','5','6'}},
            {"SBAS",    {'1','5'}}
        };
        static const char obs_types[] = {'C','L','D','S'};

        signal_priorities_.clear();
        {
            const auto& sp = jsection(json, "processing.signal_priorities", "signal_priorities");
            if (sp.is_object()) {
                for (const auto& [system, value] : sp.items()) {
                    if (value.is_string()) {
                        std::string attrs = value.get<std::string>();
                        auto bands_it = system_bands.find(system);
                        if (bands_it == system_bands.end()) {
                            log->warn("No band definition for system '{}', skipping signal priorities", system);
                            continue;
                        }
                        std::vector<std::string> expanded;
                        for (char band : bands_it->second)
                            for (char ot : obs_types)
                                for (char attr : attrs)
                                    expanded.push_back(std::string{ot, band, attr});
                        signal_priorities_[system] = std::move(expanded);
                    } else if (value.is_array()) {
                        signal_priorities_[system] = value.get<std::vector<std::string>>();
                    }
                }
            }
        }
    }

    // =================================================================
    //  Section: estimation
    // =================================================================
    void GnssConfig::loadEstimationSection_(const nlohmann::json& json, LogManager& log_manager) {
        auto log = log_manager.getLogger("gnss_config");
        estimate_tropo_gradients_ = jval<bool>(json, "estimation.estimate_tropo_gradients",
                                                "estimate_tropo_gradients", false);
        estimate_phase_clock_     = jval<bool>(json, "estimation.estimate_phase_clock",
                                                "estimate_phase_clock", false);
        estimate_velocity_        = jval<bool>(json, "estimation.estimate_velocity",
                                                "estimate_velocity", false);
        estimate_ifb_             = jval<bool>(json, "estimation.estimate_ifb",
                                                "estimate_ifb", false);
        lock_signals_to_clock_ref_ = jval<bool>(json, "estimation.lock_signals_to_clock_ref",
                                                 "lock_signals_to_clock_ref", false);
        require_bia_for_signals_   = jval<bool>(json, "estimation.require_bia_for_signals",
                                                 "require_bia_for_signals", false);
        estimate_sat_biases_       = jval<bool>(json, "estimation.estimate_sat_biases",
                                                 "estimate_sat_biases", false);
        no_bia_code_downweight_    = jval<double>(json, "estimation.no_bia_code_downweight",
                                                  "no_bia_code_downweight", 100.0);
        if (no_bia_code_downweight_ < 1.0) no_bia_code_downweight_ = 1.0;
        enable_if_slip_detector_  = jval<bool>(json, "estimation.enable_if_slip_detector",
                                                "enable_if_slip_detector", false);
        accumulate_phase_windup_  = jval<bool>(json, "estimation.accumulate_phase_windup",
                                                "accumulate_phase_windup", false);

        // Between-satellite single differencing
        enable_between_satellite_sd_ = jval<bool>(json, "estimation.between_satellite_sd.enabled",
                                                   "enable_between_satellite_sd", false);
        enable_glonass_bssd_         = jval<bool>(json, "estimation.between_satellite_sd.enable_glonass",
                                                   "enable_glonass_bssd", false);
        bssd_pivot_min_elevation_deg_ = jval<double>(json, "estimation.between_satellite_sd.pivot_min_elevation",
                                                      "bssd_pivot_min_elevation", 45.0);

        // Residual history tracker (post-fit temporal corroboration of
        // spatial pivot-slip detection).  Defaults are off — opt in
        // via JSON.  See ResidualHistoryTracker.hpp for the EWMA
        // recurrence and reset semantics.
        residual_history_.enabled = jval<bool>(json,
            "estimation.residual_history.enabled",   "residual_history_enabled",   false);
        residual_history_.alpha   = jval<double>(json,
            "estimation.residual_history.alpha",     "residual_history_alpha",     0.05);
        residual_history_.warmup_n = static_cast<std::size_t>(std::max<int>(1, jval<int>(json,
            "estimation.residual_history.warmup_n",  "residual_history_warmup_n",  20)));
        residual_history_.z_threshold = jval<double>(json,
            "estimation.residual_history.z_threshold", "residual_history_z_threshold", 4.0);
        residual_history_.fraction_threshold = jval<double>(json,
            "estimation.residual_history.fraction_threshold", "residual_history_fraction_threshold", 0.5);
        if (residual_history_.enabled) {
            log->info("Residual history tracker: alpha={:.3f}, warmup_n={}, "
                "z_threshold={:.2f}, fraction_threshold={:.2f}",
                residual_history_.alpha, residual_history_.warmup_n,
                residual_history_.z_threshold, residual_history_.fraction_threshold);
        }

        // Ambiguity resolution
        ar_mode_            = jval<int>(json, "estimation.ambiguity_resolution.mode",
                                        "ar_mode", 0);
        ar_ratio_threshold_ = jval<double>(json, "estimation.ambiguity_resolution.ratio_threshold",
                                            "ar_ratio_threshold", 3.0);
        ar_elevation_mask_  = jval<double>(json, "estimation.ambiguity_resolution.elevation_mask",
                                            "ar_elevation_mask", 20.0);
        ar_hold_variance_   = jval<double>(json, "estimation.ambiguity_resolution.hold_variance",
                                            "ar_hold_variance", 0.001);
        ar_min_epochs_           = jval<int>(json, "estimation.ambiguity_resolution.min_epochs",
                                              "ar_min_epochs", 10);
        ar_max_position_sigma_m_ = jval<double>(json, "estimation.ambiguity_resolution.max_position_sigma_m",
                                                 "ar_max_position_sigma_m", 0.05);
        ar_ratio_upper_bound_    = jval<double>(json, "estimation.ambiguity_resolution.ratio_upper_bound",
                                                 "ar_ratio_upper_bound", 1e4);
        ar_wl_only_              = jval<bool>(json, "estimation.ambiguity_resolution.wl_only",
                                               "ar_wl_only", false);
        ar_wl_only_pseudo_var_   = jval<double>(json, "estimation.ambiguity_resolution.wl_only_pseudo_var",
                                                 "ar_wl_only_pseudo_var", 1e-4);

        log->info("Toggles: tropo_grad={}, relin_geom={}, smoother={}, phase_clk={}, if_slip={}, accum_windup={}, bssd={}, velocity={}, ifb={}, lock_clkref={}, req_bia={}, sat_bias={}, no_bia_dw={:.0f}, imu={}",
            estimate_tropo_gradients_ ? "on" : "off",
            relinearize_geometry_     ? "on" : "off",
            run_smoother_             ? "on" : "off",
            estimate_phase_clock_     ? "on" : "off",
            enable_if_slip_detector_  ? "on" : "off",
            accumulate_phase_windup_  ? "on" : "off",
            enable_between_satellite_sd_ ? "on" : "off",
            estimate_velocity_        ? "on" : "off",
            estimate_ifb_             ? "on" : "off",
            lock_signals_to_clock_ref_ ? "on" : "off",
            require_bia_for_signals_  ? "on" : "off",
            estimate_sat_biases_      ? "on" : "off",
            no_bia_code_downweight_,
            imu_config_.enabled
                ? (imu_config_.coupling_mode == ImuCouplingMode::TightCoupled ? "tight" : "loose")
                : "off");
    }

    // =================================================================
    //  Section: filter
    // =================================================================
    void GnssConfig::loadFilterSection_(const nlohmann::json& json, LogManager& log_manager) {
        auto log = log_manager.getLogger("gnss_config");
        {
            const auto& pn = jsection(json, "filter.process_noise", "process_noise");
            process_noise_.position            = pn.value("position_q", 0.0);
            process_noise_.clock               = pn.value("clock_bias_q", 1e4);
            process_noise_.troposphere         = pn.value("troposphere_q", 1e-8);
            process_noise_.ionosphere          = pn.value("ionosphere_q", 0.10);
            process_noise_.system_time_offset  = pn.value("system_time_offset_q", 1e-7);
            process_noise_.code_bias           = pn.value("code_bias_q", 1e-4);
            process_noise_.tropo_gradient      = pn.value("tropo_gradient_q", 5e-12);
            process_noise_.phase_clock         = pn.value("phase_clock_q", 1e-9);
            process_noise_.velocity            = pn.value("velocity_q", 1e2);
            process_noise_.sat_bias            = pn.value("sat_bias_q", 1e-10);
            // Ambiguity process noise.  Default 1e-12 is a numerical
            // safety floor — sub-mm accumulation per day, physically
            // indistinguishable from the constant-ambiguity model,
            // keeps per-(sat, signal) ambiguity covariance from
            // underflowing on long phase arcs (which on its own can
            // trigger non-positive-definite Cholesky downstream).
            // Set 0.0 in the JSON to enforce strict constant
            // ambiguity.
            process_noise_.ambiguity           = pn.value("ambiguity_q", 1e-12);
        }
        {
            const auto& is = jsection(json, "filter.initial_std", "initial_std");
            initial_std_.position            = is.value("position", 1e2);
            initial_std_.velocity            = is.value("velocity", 1e2);
            initial_std_.clock               = is.value("clock", 1e3);
            initial_std_.troposphere         = is.value("troposphere", 0.5);
            initial_std_.ionosphere          = is.value("ionosphere", 5.0);
            initial_std_.ambiguity           = is.value("ambiguity", 1e3);
            initial_std_.system_time_offset  = is.value("system_time_offset", 1e3);
            initial_std_.code_bias           = is.value("code_bias", 10.0);
            initial_std_.tropo_gradient      = is.value("tropo_gradient", 1e-3);
            initial_std_.phase_clock         = is.value("phase_clock", 1e3);
            initial_std_.sat_bias            = is.value("sat_bias", 10.0);
        }
        {
            const auto& mn = jsection(json, "filter.measurement_noise", "default_measurement_noise");
            default_meas_noise_.code_std_m    = mn.value("code_std_m", 0.600);
            default_meas_noise_.phase_std_m   = mn.value("phase_std_m", 0.002);
            default_meas_noise_.doppler_std_m = mn.value("doppler_std_m", 0.5);
        }
        log->info("Default measurement noise: code_std={:.3f} m, phase_std={:.3f} m, doppler_std={:.3f} Hz",
            default_meas_noise_.code_std_m, default_meas_noise_.phase_std_m, default_meas_noise_.doppler_std_m);

        // -----------------------------------------------------------
        // Derive per-block code/phase σ tables from the unitless
        // ratio table × the measurement_noise baselines.  The user
        // tunes one σ per observable type (code, phase) via
        // filter.measurement_noise; the per-block proportions follow
        // automatically.  This keeps the relative hardware-quality
        // hierarchy (Block III at 1/3 of legacy Block II, etc.)
        // anchored to a single user knob and prevents drift between
        // the code and phase block tables that previously had to be
        // hand-edited in tandem.
        // -----------------------------------------------------------
        {
            auto& bp = sat_block_noise_params_;
            bp.block_code_std_m.clear();
            bp.block_phase_std_m.clear();
            bp.block_code_std_m.reserve(bp.block_scale_factors.size());
            bp.block_phase_std_m.reserve(bp.block_scale_factors.size());
            for (const auto& [block, scale] : bp.block_scale_factors) {
                bp.block_code_std_m.emplace_back(block,
                    scale * default_meas_noise_.code_std_m);
                bp.block_phase_std_m.emplace_back(block,
                    scale * default_meas_noise_.phase_std_m);
            }
            // Fallback when no block match: scale factor 1.0 →
            // baseline σ unchanged (conservative — matches the
            // legacy / worst-case zenith noise).
            bp.default_code_std_m  = default_meas_noise_.code_std_m;
            bp.default_phase_std_m = default_meas_noise_.phase_std_m;

            log->info("Sat-block noise: derived from measurement_noise "
                "(code_zen ∈ [{:.3f}, {:.3f}] m, phase_zen ∈ [{:.4f}, {:.4f}] m) "
                "× per-block scale factors",
                default_meas_noise_.code_std_m  / 3.0, default_meas_noise_.code_std_m,
                default_meas_noise_.phase_std_m / 3.0, default_meas_noise_.phase_std_m);
        }

        // Stochastic model.  Drives both code and phase weighting,
        // but each model handles phase per its own per-model rule
        // (see GnssConfig::getStochasticModel() docstring).  In
        // particular, SatelliteBlock uses the block + freq + elevation
        // pipeline on code while phase reduces to just the configured
        // phase_std_m amplified by elevation.
        {
            std::string raw = jval<std::string>(json, "filter.stochastic_model", "stochastic_model", "elevation");
            const std::string low = toLower(raw);
            if      (low == "constant")        stochastic_model_ = StochasticModelType::Constant;
            else if (low == "elevation")       stochastic_model_ = StochasticModelType::Elevation;
            else if (low == "snr")             stochastic_model_ = StochasticModelType::SNR;
            else if (low == "satellite_block") stochastic_model_ = StochasticModelType::SatelliteBlock;
            else {
                log->warn("Invalid stochastic_model '{}', defaulting to Elevation", raw);
                stochastic_model_ = StochasticModelType::Elevation;
            }
            log->info("Stochastic model: {}", raw);
        }
        // Elevation-amplification σ-power, separately configurable for
        // code and phase.  Variance scales as 1/sin^(2·power)(elev),
        // i.e. σ = σ_zen / sin^power(elev).  Defaults: code=1.0
        // (legacy csc²(θ) on variance, fits code multipath and tropo
        // path-length physics), phase=0.5 (csc(θ) on variance, fits
        // phase carrier-loop noise + PCV miscalibration physics and
        // keeps low-elevation residual checks usable).
        code_elevation_power_  = jval<double>(json,
            "filter.code_elevation_power",  "code_elevation_power",  1.0);
        phase_elevation_power_ = jval<double>(json,
            "filter.phase_elevation_power", "phase_elevation_power", 0.5);
        log->info("Elevation power: code={:.2f}, phase={:.2f}  "
            "(var_actual = var_zen / sin^(2·power)(elev))",
            code_elevation_power_, phase_elevation_power_);
        {
            // SNR model defaults are tuned for the variance-additive
            // form  σ² = a² + b²·10^(-CN0/10).  These differ from any
            // values left over from the σ-additive era; configs from
            // before the formula switch should be re-tuned (the old
            // code_b ≈ 1.0 etc. produces a near-flat σ vs CN0 under
            // the new form, defeating the SNR axis).
            const auto& sp = jsection(json, "filter.snr_model_params", "snr_model_params");
            snr_model_params_.code_a         = sp.value("code_a", 0.30);
            snr_model_params_.code_b         = sp.value("code_b", 50.0);
            snr_model_params_.phase_a        = sp.value("phase_a", 0.001);
            snr_model_params_.phase_b        = sp.value("phase_b", 0.5);
            snr_model_params_.snr_floor_dbhz = sp.value("snr_floor_dbhz", 20.0);
            snr_model_params_.snr_ceil_dbhz  = sp.value("snr_ceil_dbhz", 55.0);
            log->info("SNR model params (variance-additive): code_a={:.3f}, code_b={:.2f}, "
                "phase_a={:.4f}, phase_b={:.2f}, floor={:.0f} dBHz, ceil={:.0f} dBHz",
                snr_model_params_.code_a, snr_model_params_.code_b,
                snr_model_params_.phase_a, snr_model_params_.phase_b,
                snr_model_params_.snr_floor_dbhz, snr_model_params_.snr_ceil_dbhz);
        }
    }

    // =================================================================
    //  Section: model_corrections  (unchanged leaf keys)
    // =================================================================
    void GnssConfig::loadModelCorrectionsSection_(const nlohmann::json& json, LogManager& log_manager) {
        auto log = log_manager.getLogger("gnss_config");
        {
            const auto& mc = jsection(json, "model_corrections", "model_corrections");
            model_corrections_.enable_troposphere              = mc.value("enable_troposphere", true);
            model_corrections_.enable_relativistic_correction = mc.value("enable_relativistic_correction", true);
            model_corrections_.enable_shapiro_delay           = mc.value("enable_shapiro_delay", true);
            model_corrections_.enable_solid_earth_tide        = mc.value("enable_solid_earth_tide", true);
            model_corrections_.enable_ocean_tide_loading      = mc.value("enable_ocean_tide_loading", true);
            model_corrections_.enable_pole_tide               = mc.value("enable_pole_tide", true);
            model_corrections_.enable_atmospheric_loading     = mc.value("enable_atmospheric_loading", true);
            model_corrections_.enable_satellite_pco           = mc.value("enable_satellite_pco", true);
            model_corrections_.enable_satellite_pcv           = mc.value("enable_satellite_pcv", true);
            model_corrections_.enable_receiver_pco            = mc.value("enable_receiver_pco", true);
            model_corrections_.enable_receiver_pcv            = mc.value("enable_receiver_pcv", true);
            model_corrections_.enable_phase_windup            = mc.value("enable_phase_windup", true);
            model_corrections_.enable_code_phase_bias        = mc.value("enable_code_phase_bias", true);
            model_corrections_.enable_atx_frequency_fallback  = mc.value("enable_atx_frequency_fallback", true);
            model_corrections_.require_atx_coverage           = mc.value("require_atx_coverage",          false);
            model_corrections_.enable_klobuchar_ionosphere    = mc.value("enable_klobuchar_ionosphere", false);
        }
        log->info("Model corrections: tropo={}, rel={}, shapiro={}, set={}, otl={}, pole={}, atm={}, "
            "sat_pco={}, sat_pcv={}, rx_pco={}, rx_pcv={}, windup={}, cpb={}, atx_fallback={}",
            model_corrections_.enable_troposphere              ? "on" : "off",
            model_corrections_.enable_relativistic_correction ? "on" : "off",
            model_corrections_.enable_shapiro_delay           ? "on" : "off",
            model_corrections_.enable_solid_earth_tide        ? "on" : "off",
            model_corrections_.enable_ocean_tide_loading      ? "on" : "off",
            model_corrections_.enable_pole_tide               ? "on" : "off",
            model_corrections_.enable_atmospheric_loading     ? "on" : "off",
            model_corrections_.enable_satellite_pco           ? "on" : "off",
            model_corrections_.enable_satellite_pcv           ? "on" : "off",
            model_corrections_.enable_receiver_pco            ? "on" : "off",
            model_corrections_.enable_receiver_pcv            ? "on" : "off",
            model_corrections_.enable_phase_windup            ? "on" : "off",
            model_corrections_.enable_code_phase_bias        ? "on" : "off",
            model_corrections_.enable_atx_frequency_fallback  ? "on" : "off");
        if (model_corrections_.require_atx_coverage) {
            log->info("ATX coverage: STRICT — observations without complete sat+rx PCO+PCV "
                "for the exact signal frequency will be dropped.");
        }
    }

    // =================================================================
    //  Section: receiver
    // =================================================================
    void GnssConfig::loadReceiverSection_(const nlohmann::json& json, LogManager& log_manager) {
        auto log = log_manager.getLogger("gnss_config");
        {
            const auto& ao = jsection(json, "receiver.antenna_orientation", "receiver_antenna_orientation");
            receiver_antenna_orientation_.azimuth_deg   = ao.value("azimuth_deg", 0.0);
            receiver_antenna_orientation_.elevation_deg = ao.value("elevation_deg", 90.0);
        }
        zhd_pressure_mbar = jval<double>(json, "receiver.zhd_pressure_mbar", "zhd_pressure_mbar", 1013.25);
        zhd_temp_kelvin   = jval<double>(json, "receiver.zhd_temp_kelvin", "zhd_temp_kelvin", 288.15);
        log->info("ZHD defaults: P={:.1f} mbar, T={:.1f} K", zhd_pressure_mbar, zhd_temp_kelvin);

        // Reference position (optional)
        {
            const auto& rp = jsection(json, "receiver.reference_position_ecef", "reference_position_ecef");
            if (rp.is_object() && rp.contains("x") && rp.contains("y") && rp.contains("z")) {
                reference_position_ecef_ = { rp.value("x", 0.0), rp.value("y", 0.0), rp.value("z", 0.0) };
                has_reference_position_ = true;
                log->info("Reference position ECEF: ({:.4f}, {:.4f}, {:.4f}) m",
                    reference_position_ecef_[0], reference_position_ecef_[1], reference_position_ecef_[2]);
            }
        }
    }

    // =================================================================
    //  Section: files
    //
    //  Parses the top-level @c data_source selector and the @c files
    //  block.  Path validation and discovery is deferred to
    //  @c PathResolver, which is invoked by the application driver
    //  after the config has finished loading.  This keeps the loader
    //  responsibility narrow: stash whatever the user wrote, plus
    //  defaults for anything they omitted.
    // =================================================================
    void GnssConfig::loadFilesSection_(const nlohmann::json& json, LogManager& log_manager) {
        auto log = log_manager.getLogger("gnss_config");

        // --- Top-level data_source selector ---------------------------
        // Accepts "internal" / "production" (any case).  Defaults to
        // Internal so a fresh checkout with no extra config still runs
        // against the data/ tree shipped with the repo.
        {
            std::string raw = jval<std::string>(json, "data_source", "data_source", "internal");
            std::string low = toLower(raw);
            if      (low == "internal")   data_source_ = DataSource::Internal;
            else if (low == "production") data_source_ = DataSource::Production;
            else {
                log->warn("Unknown data_source '{}', defaulting to internal", raw);
                data_source_ = DataSource::Internal;
            }
            log->info("Data source: {}",
                data_source_ == DataSource::Internal ? "internal (data/)" : "production (download)");
        }

        // --- Internal data root --------------------------------------
        // Relative paths are resolved against the project root baked
        // in at CMake configure time.  An empty / missing field falls
        // back to <project_root>/data, which is the canonical fixture
        // location for unit tests and demos.
        {
            std::string raw = jval<std::string>(json,
                "files.internal_data_root", "internal_data_root", "");
            const std::filesystem::path project_root = GNSS_PROJECT_ROOT;
            if (raw.empty()) {
                internal_data_root_ = project_root / "data";
            } else {
                std::filesystem::path p(raw);
                if (p.is_relative()) {
                    // "./data" and friends resolve under the project
                    // root so the same JSON works regardless of CWD.
                    internal_data_root_ = std::filesystem::weakly_canonical(project_root / p);
                } else {
                    internal_data_root_ = std::filesystem::weakly_canonical(p);
                }
            }
            if (data_source_ == DataSource::Internal) {
                if (std::filesystem::exists(internal_data_root_)) {
                    log->info("Internal data root: {}", internal_data_root_.string());
                } else {
                    log->warn("Internal data root does not exist: {} "
                              "(PathResolver will fail if no files are found)",
                              internal_data_root_.string());
                }
            } else {
                log->debug("Internal data root: {} (unused in production mode)",
                    internal_data_root_.string());
            }
        }

        // --- Direct file overrides (optional) -------------------------
        // The user may still pin specific paths in the config.  When
        // set, these win over PathResolver auto-discovery.  Empty
        // values let PathResolver fill them in.  No hard-coded fallback
        // strings — PathResolver is the single source of truth for
        // "what to do when the user didn't specify".
        observation_path_ = jval<std::string>(json, "files.observation_path", "observation_path", "");
        navigation_path_  = jval<std::string>(json, "files.navigation_path",  "navigation_path",  "");

        // --- Legacy product_list_path (optional override) -------------
        // When non-empty, the file's contents seed @c product_paths_
        // directly and PathResolver leaves the list alone.  This is the
        // escape hatch for runs that need a hand-curated product set
        // (e.g. comparing analysis centres on the same epoch).  Empty
        // means "let PathResolver do it" — the common case.
        product_list_path_ = jval<std::string>(json, "files.product_list_path", "product_list_path", "");
        if (!product_list_path_.empty()) {
            if (std::filesystem::exists(product_list_path_)) {
                std::ifstream product_file(product_list_path_);
                std::string line;
                while (std::getline(product_file, line)) {
                    line.erase(line.find_last_not_of(" \t\r\n") + 1);
                    if (!line.empty()) product_paths_.push_back(line);
                }
                log->info("Loaded {} product paths from list file: {}",
                    product_paths_.size(), product_list_path_.string());
            } else {
                log->warn("product_list_path set but file does not exist: {} "
                          "(PathResolver will populate the list instead)",
                          product_list_path_.string());
            }
        }

        // Ocean tidal loading BLQ
        {
            std::string otl_path_str = jval<std::string>(json, "files.otl_blq_path", "otl_blq_path", "");
            if (!otl_path_str.empty()) {
                otl_blq_path_ = otl_path_str;
                if (std::filesystem::exists(otl_blq_path_))
                    log->info("OTL BLQ file: {}", otl_blq_path_.string());
                else {
                    log->warn("OTL BLQ file not found: {}", otl_blq_path_.string());
                    otl_blq_path_.clear();
                }
            }
        }
    }

    // =================================================================
    //  Section: imu
    // =================================================================
    void GnssConfig::loadImuSection_(const nlohmann::json& json, LogManager& log_manager) {
        auto log = log_manager.getLogger("gnss_config");
        {
            const auto& ic = jsection(json, "imu", "imu_config");
            imu_config_.enabled = jval<bool>(json, "imu.enabled", "enable_imu", false);
            std::string imu_path_str = ic.value("imu_file_path", "");
            if (!imu_path_str.empty()) {
                imu_config_.imu_file_path = imu_path_str;
                if (!std::filesystem::exists(imu_config_.imu_file_path))
                    log->warn("IMU file not found: {}", imu_config_.imu_file_path.string());
            }
            {
                std::string raw = ic.value("coupling_mode",
                                    ic.value("imu_coupling_mode", "loosely_coupled"));
                std::string low = toLower(raw);
                if (low == "tightly_coupled" || low == "tight")
                    imu_config_.coupling_mode = ImuCouplingMode::TightCoupled;
                else
                    imu_config_.coupling_mode = ImuCouplingMode::LooseCoupled;
            }
            imu_config_.accel_noise_density    = ic.value("accel_noise_density", 0.05);
            imu_config_.gyro_noise_density     = ic.value("gyro_noise_density", 5e-4);
            imu_config_.accel_bias_instability = ic.value("accel_bias_instability", 1e-4);
            imu_config_.gyro_bias_instability  = ic.value("gyro_bias_instability", 1e-6);
            imu_config_.initial_accel_bias_std = ic.value("initial_accel_bias_std", 0.1);
            imu_config_.initial_gyro_bias_std  = ic.value("initial_gyro_bias_std", 1e-3);
            imu_config_.initial_attitude_std   = ic.value("initial_attitude_std", 0.1);
        }
        if (imu_config_.enabled) {
            const char* coupling_str = (imu_config_.coupling_mode == ImuCouplingMode::TightCoupled)
                ? "tightly_coupled" : "loosely_coupled";
            log->info("IMU integration enabled: coupling={}, file={}, accel_noise={:.4f}, gyro_noise={:.6f}, "
                "accel_bias_inst={:.6f}, gyro_bias_inst={:.8f}",
                coupling_str,
                imu_config_.imu_file_path.string(),
                imu_config_.accel_noise_density, imu_config_.gyro_noise_density,
                imu_config_.accel_bias_instability, imu_config_.gyro_bias_instability);

            if (imu_config_.coupling_mode == ImuCouplingMode::TightCoupled && !estimate_velocity_) {
                estimate_velocity_ = true;
                log->info("Tightly-coupled IMU: forcing estimate_velocity=true");
            }
        }
    }

    // =================================================================
    //  Section: data_download
    // =================================================================
    void GnssConfig::loadDataDownloadSection_(const nlohmann::json& json, LogManager& log_manager) {
        auto log = log_manager.getLogger("gnss_config");
        if (json.contains("data_download")) {
            const auto& dl = json["data_download"];
            download_config_.enabled           = dl.value("enabled", false);
            download_config_.data_root         = dl.value("data_root", "");
            download_config_.product_source    = dl.value("product_source", "COD");
            download_config_.product_type      = dl.value("product_type", "FIN");
            download_config_.netrc_path        = dl.value("netrc_path", "");
            download_config_.station           = dl.value("station", "");
            download_config_.station_country   = dl.value("station_country", "USA");
            download_config_.year              = dl.value("year", 0);
            download_config_.doy               = dl.value("doy", 0);
            download_config_.download_rinex    = dl.value("download_rinex", false);
            download_config_.download_products = dl.value("download_products", true);
            std::string atx_str = dl.value("atx_path", "");
            if (!atx_str.empty()) download_config_.atx_path = atx_str;
            std::string nc_str = dl.value("nc_path", "");
            if (!nc_str.empty()) download_config_.nc_path = nc_str;
            std::string crx_str = dl.value("crx2rnx_path", "");
            if (!crx_str.empty()) download_config_.crx2rnx_path = crx_str;
            download_config_.keep_zip_after_unzip = dl.value("keep_zip_after_unzip", false);

            if (download_config_.enabled) {
                log->info("Data download enabled: root={}, source={}, type={}, station={}, year={}, doy={}",
                    download_config_.data_root.string(), download_config_.product_source,
                    download_config_.product_type, download_config_.station,
                    download_config_.year, download_config_.doy);
            }
        }

        // Internal mode is mutually exclusive with auto-download.
        // loadFilesSection_ runs before this, so data_source_ is set.
        // Forcing the flag here means downstream code (Application,
        // ApplicationBaseline, PathResolver) only has to check one
        // condition — DownloadConfig::enabled — to know whether it
        // should hit the network.
        if (data_source_ == DataSource::Internal && download_config_.enabled) {
            log->info("data_source=internal overrides data_download.enabled "
                      "(no network access in internal mode)");
            download_config_.enabled = false;
        }
    }

    // =================================================================
    //  Section: application mode & realtime
    // =================================================================
    void GnssConfig::loadAppModeAndRealtimeSection_(const nlohmann::json& json, LogManager& log_manager) {
        auto log = log_manager.getLogger("gnss_config");
        {
            std::string mode_str = json.value("app_mode", "post");
            if (mode_str == "realtime" || mode_str == "real_time" || mode_str == "rt") {
                app_mode_ = AppMode::RealTime;
            } else {
                app_mode_ = AppMode::PostProcessing;
            }
            log->info("Application mode: {}", app_mode_ == AppMode::RealTime ? "realtime" : "post");

            // Smoother is post-processing only.
            if (run_smoother_ && app_mode_ == AppMode::RealTime) {
                run_smoother_ = false;
                log->warn("run_smoother=true ignored in real-time mode (smoothing is post-processing only)");
            }
        }
        {
            const auto& rt = jsection(json, "realtime", "realtime_config");
            realtime_config_.ntrip_host        = rt.value("ntrip_host", "");
            realtime_config_.ntrip_port         = rt.value("ntrip_port", 2101);
            realtime_config_.ntrip_mountpoint   = rt.value("ntrip_mountpoint", "");
            realtime_config_.ntrip_user         = rt.value("ntrip_user", "");
            realtime_config_.ntrip_password     = rt.value("ntrip_password", "");
            realtime_config_.nmea_gga           = rt.value("nmea_gga", "");
            realtime_config_.reconnect_delay_s  = rt.value("reconnect_delay_s", 5);
            realtime_config_.input_type         = rt.value("input_type", "ntrip");
            realtime_config_.input_path         = rt.value("input_path", "");
            realtime_config_.tcp_host           = rt.value("tcp_host", "");
            realtime_config_.tcp_port           = rt.value("tcp_port", 28784);
            realtime_config_.output_type        = rt.value("output_type", "console");
            realtime_config_.output_path        = rt.value("output_path", "");
            realtime_config_.max_epoch_buffer   = rt.value("max_epoch_buffer", 100U);

            // Optional SSR corrections stream (separate NTRIP mountpoint
            // or file-replay).  Default: disabled — single-stream behaviour.
            if (rt.contains("corrections") && rt.at("corrections").is_object()) {
                const auto& cc = rt.at("corrections");
                auto& c = realtime_config_.corrections;
                c.enabled            = cc.value("enabled", false);
                c.input_type         = cc.value("input_type", std::string{"ntrip"});
                c.ntrip_host         = cc.value("ntrip_host", std::string{});
                c.ntrip_port         = cc.value("ntrip_port", 2101);
                c.ntrip_mountpoint   = cc.value("ntrip_mountpoint", std::string{});
                c.ntrip_user         = cc.value("ntrip_user", std::string{});
                c.ntrip_password     = cc.value("ntrip_password", std::string{});
                c.nmea_gga           = cc.value("nmea_gga", std::string{});
                c.reconnect_delay_s  = cc.value("reconnect_delay_s", 5);
                c.input_path         = cc.value("input_path", std::string{});
                c.tcp_host           = cc.value("tcp_host", std::string{});
                c.tcp_port           = cc.value("tcp_port", 28784);
            }

            if (app_mode_ == AppMode::RealTime) {
                log->info("Real-time config: input={}, ntrip={}:{}/{}, output={}",
                    realtime_config_.input_type,
                    realtime_config_.ntrip_host,
                    realtime_config_.ntrip_port,
                    realtime_config_.ntrip_mountpoint,
                    realtime_config_.output_type);
                if (realtime_config_.corrections.enabled) {
                    const auto& c = realtime_config_.corrections;
                    if (c.input_type == "ntrip") {
                        log->info("Real-time corrections: ntrip={}:{}/{}",
                                  c.ntrip_host, c.ntrip_port, c.ntrip_mountpoint);
                    } else {
                        log->info("Real-time corrections: file={}", c.input_path);
                    }
                }
            }
        }
    }

    // =================================================================
    //  Section: baseline  (BaselineSingle / BaselineMulti)
    // =================================================================
    bool GnssConfig::loadBaselineSection_(const nlohmann::json& json, LogManager& log_manager) {
        auto log = log_manager.getLogger("gnss_config");
        const auto& bl = jsection(json, "baseline", "baseline");
        if (bl.is_null() || !bl.is_object()) {
            log->error("Baseline mode selected but no 'baseline' section found in config");
            return false;
        }

        // --- Reference station (required) ---
        const auto ref_it = bl.find("reference");
        if (ref_it == bl.end() || !ref_it->is_object()) {
            log->error("Baseline: missing 'reference' station block");
            return false;
        }
        baseline_config_.reference.name       = ref_it->value("name", std::string{"REF"});
        baseline_config_.reference.rinex_path = ref_it->value("rinex_path", std::string{});
        baseline_config_.reference.fixed      = ref_it->value("fixed", true);
        if (ref_it->contains("position_ecef") && (*ref_it)["position_ecef"].is_array()
            && (*ref_it)["position_ecef"].size() == 3) {
            const auto& p = (*ref_it)["position_ecef"];
            baseline_config_.reference.position_ecef = {
                p[0].get<double>(), p[1].get<double>(), p[2].get<double>()
            };
            baseline_config_.reference.has_position = true;
        }
        // NOTE: an empty rinex_path is NOT an error here — PathResolver
        // runs after config-load (in runBaseline → resolveData) and
        // fills it in by station-name match against the internal data
        // tree.  Empty position_ecef is similarly tolerated here: the
        // baseline-fixture preset block below may fill it in from the
        // catalog.  Final validation happens AFTER the preset has had
        // its chance — see the @c has_position check just past the
        // preset block.

        // --- Rovers (required: one for Single, one-or-more for Multi) ---
        baseline_config_.rovers.clear();
        auto addRover = [&](const nlohmann::json& rj) {
            StationConfig sc;
            sc.name       = rj.value("name", std::string{"ROV"});
            sc.rinex_path = rj.value("rinex_path", std::string{});
            sc.fixed      = false;
            if (rj.contains("position_ecef") && rj["position_ecef"].is_array()
                && rj["position_ecef"].size() == 3) {
                sc.position_ecef = {
                    rj["position_ecef"][0].get<double>(),
                    rj["position_ecef"][1].get<double>(),
                    rj["position_ecef"][2].get<double>()
                };
                sc.has_position = true;
            }
            baseline_config_.rovers.push_back(std::move(sc));
        };
        if (bl.contains("rover") && bl["rover"].is_object()) {
            addRover(bl["rover"]);
        }
        if (bl.contains("rovers") && bl["rovers"].is_array()) {
            for (const auto& rj : bl["rovers"]) {
                if (rj.is_object()) addRover(rj);
            }
        }

        // ---- Apply named-fixture preset for any blank slots --------
        // Top-level @c baseline_fixture chooses one of the bundled
        // presets (see include/config/BaselineFixtures.hpp).  The
        // preset only fills fields the user left empty: explicit
        // entries in baseline.reference / baseline.rover always win.
        // Unknown values fall back to "Custom" (no defaults applied).
        // Empty / missing field also defaults to Custom so legacy
        // configs keep working.
        {
            std::string preset_raw = jval<std::string>(json,
                "baseline_fixture", "baseline_fixture", std::string{"Custom"});
            std::string preset_low = toLower(preset_raw);
            // Compact whitespace and dashes/underscores to a single
            // representation so "IGS_Pasadena" / "igs-pasadena" /
            // "igs pasadena" all hit the same key.
            for (auto& c : preset_low) if (c == '-' || c == ' ') c = '_';
            if (preset_low.empty() || preset_low == "custom" || preset_low == "none") {
                log->info("Baseline fixture: custom (no preset applied)");
            } else {
                const auto& presets = baselineFixturePresets();
                auto it = presets.find(preset_low);
                if (it == presets.end()) {
                    log->warn("Unknown baseline_fixture '{}' — known presets: ", preset_raw);
                    for (const auto& kv : presets) log->warn("    {}", kv.first);
                    log->warn("Continuing as if baseline_fixture=Custom.");
                } else {
                    const auto& p = it->second;
                    bool applied_anything = false;

                    // Reference station: name + ECEF.
                    auto& ref = baseline_config_.reference;
                    const bool ref_name_empty =
                        ref.name.empty() || ref.name == "REF";
                    if (ref_name_empty) {
                        ref.name = p.ref_name;
                        applied_anything = true;
                    }
                    if (!ref.has_position) {
                        ref.position_ecef = p.ref_ecef;
                        ref.has_position  = true;
                        applied_anything  = true;
                    }

                    // Rovers: when none configured, seed one from the
                    // preset.  When one slot already exists with a
                    // placeholder name ("ROV") that we want to fill,
                    // fill it; otherwise leave the user's roster
                    // untouched (e.g. multi-rover networks).
                    if (baseline_config_.rovers.empty()) {
                        StationConfig rv;
                        rv.name  = p.rover_name;
                        rv.fixed = false;
                        baseline_config_.rovers.push_back(std::move(rv));
                        applied_anything = true;
                    } else if (baseline_config_.rovers.size() == 1) {
                        auto& rv = baseline_config_.rovers.front();
                        if (rv.name.empty() || rv.name == "ROV") {
                            rv.name = p.rover_name;
                            applied_anything = true;
                        }
                    }

                    // Production-mode date fields: only push the
                    // preset's year/doy into data_download when the
                    // user hasn't typed their own.  This lets a
                    // baseline run flip between fixtures by changing
                    // a single field without nuking a year/doy that
                    // was set up for an unrelated PPP run.
                    if (download_config_.year == 0) download_config_.year = p.year;
                    if (download_config_.doy == 0) download_config_.doy = p.doy;

                    if (applied_anything) {
                        log->info("Baseline fixture preset '{}' applied: {}",
                            preset_low, p.description);
                    } else {
                        log->info("Baseline fixture preset '{}' selected but every "
                                  "field was already user-supplied: {}",
                                  preset_low, p.description);
                    }
                }
            }
        }

        // ---- Post-preset validation --------------------------------
        // Reference station must have a position by now: either the
        // user supplied position_ecef explicitly, or a recognised
        // baseline_fixture preset filled it in.  Fail loudly when
        // neither happened — without coordinates the DD solution has
        // no anchor and the rover absolute position is meaningless.
        if (!baseline_config_.reference.has_position) {
            log->error("Baseline: reference station '{}' requires position_ecef [X,Y,Z] "
                       "(neither user-supplied nor filled by a baseline_fixture preset).",
                       baseline_config_.reference.name.empty()
                           ? std::string{"<unnamed>"} : baseline_config_.reference.name);
            return false;
        }
        if (baseline_config_.reference.name.empty()) {
            log->error("Baseline: reference station has no name. "
                       "Set baseline.reference.name explicitly or pick a "
                       "baseline_fixture preset that supplies it.");
            return false;
        }

        baseline_config_.max_baseline_km        = bl.value("max_baseline_km", 30.0);
        baseline_config_.epoch_sync_tolerance_s = bl.value("epoch_sync_tolerance_s", 0.01);
        baseline_config_.code_only              = bl.value("code_only", false);
        baseline_config_.use_doppler            = bl.value("use_doppler", false);
        baseline_config_.signal_aliasing        = bl.value("signal_aliasing", true);
        baseline_config_.use_iono_free          = bl.value("use_iono_free", false);
        baseline_config_.enable_pivot_handover  = bl.value("enable_pivot_handover", true);
        baseline_config_.estimate_differential_tropo =
            bl.value("estimate_differential_tropo", false);
        baseline_config_.differential_tropo_init_std_m =
            bl.value("differential_tropo_init_std_m", 0.1);
        baseline_config_.differential_tropo_process_noise_m_per_sqrt_s =
            bl.value("differential_tropo_process_noise_m_per_sqrt_s", 1e-4);
        if (baseline_config_.code_only) {
            log->info("Baseline: code_only=true — phase DDs will be skipped by the filter");
        }
        if (baseline_config_.use_doppler) {
            log->info("Baseline: use_doppler=true — Doppler DDs will be formed (filter update ignores them until a velocity state is added)");
        }
        if (baseline_config_.signal_aliasing) {
            log->info("Baseline: signal_aliasing=true — DD rows pair across tracking attributes "
                      "within the same band (e.g. rover C2W vs reference C2C). "
                      "Set to false for strict RINEX-code matching.");
        } else {
            log->info("Baseline: signal_aliasing=false — strict RINEX-code matching only");
        }
        if (baseline_config_.use_iono_free) {
            log->info("Baseline: use_iono_free=true — per-band rows collapsed into the dual-frequency "
                      "ionosphere-free combination.  Phase IF ambiguities are float-only (no AR). "
                      "Sat-pairs with <2 shared bands are dropped in this mode.");
        }
        if (!baseline_config_.enable_pivot_handover) {
            log->info("Baseline: enable_pivot_handover=false — DD ambiguities will be erased and "
                      "re-seeded on every pivot change (legacy behavior).");
        }
        if (baseline_config_.estimate_differential_tropo) {
            log->info("Baseline: estimate_differential_tropo=true — adding 1 differential ZWD "
                      "state (init_std={:.3f} m, process_noise={:.2e} m/sqrt(s)).  Recommended "
                      "for >10-20 km baselines where the wet delay no longer cancels in BRSD.",
                      baseline_config_.differential_tropo_init_std_m,
                      baseline_config_.differential_tropo_process_noise_m_per_sqrt_s);
        }

        // --- Validate against mode ---
        if (baseline_config_.rovers.empty()) {
            log->error("Baseline: at least one rover station required "
                "(provide 'rover' object or 'rovers' array)");
            return false;
        }
        if (processing_mode_ == ProcessingMode::BaselineSingle
            && baseline_config_.rovers.size() > 1) {
            log->warn("Baseline-Single configured with {} rovers; "
                "only the first will be processed. Use Baseline-Multi for a network.",
                baseline_config_.rovers.size());
            baseline_config_.rovers.resize(1);
        }
        // Rover rinex_path may be empty here too — PathResolver fills
        // it in on the same pass that resolves the reference station.
        // Final rinex_path check happens in runBaseline() after
        // resolveData().  Names, however, must be set by now: either
        // the user typed them or a baseline_fixture preset filled
        // them in.  An empty rover name would defeat PathResolver's
        // station-name match (findObservationByStation returns empty
        // on empty input), and the downstream "no rinex_path" error
        // would be confusing.
        for (size_t i = 0; i < baseline_config_.rovers.size(); ++i) {
            if (baseline_config_.rovers[i].name.empty()) {
                log->error("Baseline: rover #{} has no name. "
                           "Set baseline.rover[s].name explicitly or pick a "
                           "baseline_fixture preset that supplies it.", i);
                return false;
            }
        }

        // --- Summary log ---
        log->info("Baseline config: reference='{}' ({}), rovers={}, max_baseline={:.1f} km, sync_tol={:.3f} s",
            baseline_config_.reference.name,
            baseline_config_.reference.fixed ? "fixed" : "weakly constrained",
            baseline_config_.rovers.size(),
            baseline_config_.max_baseline_km,
            baseline_config_.epoch_sync_tolerance_s);
        for (const auto& rv : baseline_config_.rovers) {
            log->info("  rover '{}': rinex={}{}",
                rv.name, rv.rinex_path.string(),
                rv.has_position ? ", a-priori position supplied" : "");
        }
        return true;
    }

    // =================================================================
    //  Section: logging
    // =================================================================
    void GnssConfig::loadLoggingSection_(const nlohmann::json& json, LogManager& log_manager) {
        const auto& lg = jsection(json, "logging", "logging");
        // All logger toggles default to *false*: a missing key means
        // "off."  Configs explicitly opt loggers in by setting their
        // enable_* key to true.  This keeps batch-mode runs (and any
        // other quiet-by-design caller) from inheriting verbose
        // per-epoch output through omission.  Interactive configs
        // that want rich diagnostics list each logger they want on.
        // spdlog file loggers
        logging_config_.enable_main_log           = lg.value("enable_main_log", false);
        logging_config_.enable_gnss_config        = lg.value("enable_gnss_config", false);
        logging_config_.enable_kalman_filter      = lg.value("enable_kalman_filter", false);
        logging_config_.enable_smoother           = lg.value("enable_smoother", false);
        logging_config_.enable_state_manager      = lg.value("enable_state_manager", false);
        logging_config_.enable_ppp_model          = lg.value("enable_ppp_model", false);
        logging_config_.enable_measurement_model  = lg.value("enable_measurement_model", false);
        logging_config_.enable_mw_slip_detector   = lg.value("enable_mw_slip_detector", false);
        logging_config_.enable_if_slip_detector   = lg.value("enable_if_slip_detector", false);
        logging_config_.enable_gf_slip_detector   = lg.value("enable_gf_slip_detector", false);
        logging_config_.enable_cycle_slip_manager = lg.value("enable_cycle_slip_manager", false);
        logging_config_.enable_ambiguity_resolver = lg.value("enable_ambiguity_resolver", false);
        logging_config_.enable_corrections        = lg.value("enable_corrections", false);
        logging_config_.enable_geometry           = lg.value("enable_geometry", false);
        logging_config_.enable_troposphere        = lg.value("enable_troposphere", false);
        logging_config_.enable_ionosphere         = lg.value("enable_ionosphere", false);
        logging_config_.enable_antenna            = lg.value("enable_antenna", false);
        logging_config_.enable_antenna_model      = lg.value("enable_antenna_model", false);
        logging_config_.enable_ocean_tide         = lg.value("enable_ocean_tide", false);
        logging_config_.enable_planetary_model    = lg.value("enable_planetary_model", false);
        logging_config_.enable_stochastic_model   = lg.value("enable_stochastic_model", false);
        logging_config_.enable_rinex_parser       = lg.value("enable_rinex_parser", false);
        logging_config_.enable_product_parser     = lg.value("enable_product_parser", false);
        logging_config_.enable_product_cache      = lg.value("enable_product_cache", false);
        logging_config_.enable_resolver           = lg.value("enable_resolver", false);
        logging_config_.enable_state_initializer  = lg.value("enable_state_initializer", false);
        logging_config_.enable_bssd_pivot_manager = lg.value("enable_bssd_pivot_manager", false);
        logging_config_.enable_bssd_transform     = lg.value("enable_bssd_transform", false);
        logging_config_.enable_ppp_utils          = lg.value("enable_ppp_utils", false);
        logging_config_.enable_downloader         = lg.value("enable_downloader", false);
        logging_config_.enable_ntrip              = lg.value("enable_ntrip", false);
        logging_config_.enable_rt_engine          = lg.value("enable_rt_engine", false);
        logging_config_.enable_imu                = lg.value("enable_imu", false);
        // CSV trace files
        logging_config_.enable_state_trace        = lg.value("enable_state_trace", false);
        logging_config_.enable_ambiguity_trace    = lg.value("enable_ambiguity_trace", false);
        logging_config_.enable_ionosphere_trace   = lg.value("enable_ionosphere_trace", false);
        logging_config_.enable_residual_trace     = lg.value("enable_residual_trace", false);
        logging_config_.enable_wls_trace          = lg.value("enable_wls_trace", false);
        logging_config_.enable_smoother_trace     = lg.value("enable_smoother_trace", false);
        logging_config_.enable_mw_slip_trace      = lg.value("enable_mw_slip_trace", false);
        logging_config_.enable_if_slip_trace      = lg.value("enable_if_slip_trace", false);
        logging_config_.enable_gf_slip_trace      = lg.value("enable_gf_slip_trace", false);

        // Concise KF log mode — suppresses intermediate rebuild passes,
        // keeps pass-1 + final-pass output verbatim. Default off (full).
        logging_config_.kalman_filter_concise_log = lg.value("kalman_filter_concise_log", false);

        // Per-logger level overrides: logging.levels = { "name": "level", ... }
        // Each entry sets one logger's emission threshold; loggers absent from
        // the map fall back to the global level set in LogManager::initialize().
        // Disabled loggers (enable_X = false) take precedence over any entry here.
        logging_config_.per_logger_levels.clear();
        if (lg.contains("levels") && lg["levels"].is_object()) {
            for (const auto& [name, val] : lg["levels"].items()) {
                if (!val.is_string()) continue;
                try {
                    logging_config_.per_logger_levels[name] = logLevelFromString(val.get<std::string>());
                } catch (const std::invalid_argument& e) {
                    log_manager.getLogger("gnss_config")->warn(
                        "Ignoring unknown log level for '{}': {}", name, e.what());
                }
            }
        }
    }

    // =================================================================
    //  Summary
    // =================================================================
    void GnssConfig::logConfigurationSummary_(LogManager& log_manager) const {
        const char* mode_str =
            processing_mode_ == ProcessingMode::PPP              ? "PPP"             :
            processing_mode_ == ProcessingMode::PPP_RTK          ? "PPP-RTK"         :
            processing_mode_ == ProcessingMode::BaselineSingle   ? "Baseline-Single" :
                                                                   "Baseline-Multi";
        log_manager.getLogger()->info("Loaded configuration: mode={}, uncombined_enabled={}, elevation_mask={} deg, "
            "observation_path={}, navigation_path={}, {} products, leap_seconds={}",
            mode_str,
            uncombined_enabled_, elevation_mask_deg_, observation_path_.string(),
            navigation_path_.string(), product_paths_.size(), leap_seconds_tai_);
    }

} // namespace GnssProcessor
