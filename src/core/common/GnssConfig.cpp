/*!
 * \file GnssConfig.cpp
 * \brief JSON configuration loading for GnssConfig.
 *
 * Organization:
 *  - String / enum helpers
 *  - Backward-compatible JSON accessors
 *  - loadFromJson  (grouped by parsing section)
 *  - getProductPath
 *
 * The parser accepts both the new hierarchical JSON layout and the
 * legacy flat layout.  Each setting is read through a helper that
 * tries the new nested path first, then falls back to the old
 * top-level key so that existing config files keep working.
 */

#include "GnssConfig.hpp"
#include <fstream>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace GnssProcessor {

    // =================================================================
    // Helpers
    // =================================================================

    namespace {
        std::string toLower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), ::tolower);
            return s;
        }

        std::string joinStrings(const std::vector<std::string>& v, const char* sep = ", ") {
            std::string r;
            for (const auto& s : v) { if (!r.empty()) r += sep; r += s; }
            return r;
        }

        /// Navigate into a nested JSON object using a dot-separated path.
        /// Returns a pointer to the value, or nullptr if any segment is missing.
        const nlohmann::json* resolvePath(const nlohmann::json& root, const std::string& dotPath) {
            const nlohmann::json* node = &root;
            std::string::size_type start = 0;
            while (start < dotPath.size()) {
                auto dot = dotPath.find('.', start);
                auto key = dotPath.substr(start, dot - start);
                if (!node->is_object() || !node->contains(key)) return nullptr;
                node = &(*node)[key];
                start = (dot == std::string::npos) ? dotPath.size() : dot + 1;
            }
            return node;
        }

        /// Read a scalar value from JSON trying `newPath` (dot-separated) first,
        /// then `legacyKey` (top-level), then returning `defaultVal`.
        template <typename T>
        T jval(const nlohmann::json& root, const std::string& newPath,
               const std::string& legacyKey, const T& defaultVal) {
            if (auto* p = resolvePath(root, newPath))
                return p->get<T>();
            if (!legacyKey.empty() && root.contains(legacyKey))
                return root[legacyKey].get<T>();
            return defaultVal;
        }

        /// Return a reference to a nested JSON object (new path or legacy key).
        /// Falls back to an empty static object.
        const nlohmann::json& jsection(const nlohmann::json& root,
                                       const std::string& newPath,
                                       const std::string& legacyKey = "") {
            if (auto* p = resolvePath(root, newPath))
                return *p;
            if (!legacyKey.empty() && root.contains(legacyKey))
                return root[legacyKey];
            static const nlohmann::json empty = nlohmann::json::object();
            return empty;
        }
    }

    // =================================================================
    // loadFromJson
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
            auto log = log_manager.getLogger("gnss_config");

            // =============================================================
            //  Section: processing
            // =============================================================
            {
                std::string mode = jval<std::string>(json, "processing.mode", "processing_mode", "PPP");
                processing_mode_ = (mode == "PPP-RTK") ? ProcessingMode::PPP_RTK : ProcessingMode::PPP;
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
            elevation_mask_deg_ = jval<double>(json, "processing.elevation_mask_deg", "elevation_mask_deg", 10.0);
            leap_seconds_tai_   = jval<double>(json, "processing.leap_seconds_tai", "leap_seconds_tai", 37.0);

            // Relinearize geometry lives under processing (it affects the model loop).
            relinearize_geometry_ = jval<bool>(json, "processing.relinearize_geometry", "relinearize_geometry", false);

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

            // =============================================================
            //  Section: estimation
            // =============================================================
            estimate_tropo_gradients_ = jval<bool>(json, "estimation.estimate_tropo_gradients",
                                                    "estimate_tropo_gradients", false);
            estimate_phase_clock_     = jval<bool>(json, "estimation.estimate_phase_clock",
                                                    "estimate_phase_clock", false);
            estimate_velocity_        = jval<bool>(json, "estimation.estimate_velocity",
                                                    "estimate_velocity", false);
            estimate_ifb_             = jval<bool>(json, "estimation.estimate_ifb",
                                                    "estimate_ifb", true);
            lock_signals_to_clock_ref_ = jval<bool>(json, "estimation.lock_signals_to_clock_ref",
                                                     "lock_signals_to_clock_ref", true);
            require_bia_for_signals_   = jval<bool>(json, "estimation.require_bia_for_signals",
                                                     "require_bia_for_signals", true);
            estimate_sat_biases_       = jval<bool>(json, "estimation.estimate_sat_biases",
                                                     "estimate_sat_biases", false);
            no_bia_code_downweight_    = jval<double>(json, "estimation.no_bia_code_downweight",
                                                      "no_bia_code_downweight", 100.0);
            if (no_bia_code_downweight_ < 1.0) no_bia_code_downweight_ = 1.0;
            enable_if_slip_detector_  = jval<bool>(json, "estimation.enable_if_slip_detector",
                                                    "enable_if_slip_detector", true);
            accumulate_phase_windup_  = jval<bool>(json, "estimation.accumulate_phase_windup",
                                                    "accumulate_phase_windup", false);

            // Between-satellite single differencing
            enable_between_satellite_sd_ = jval<bool>(json, "estimation.between_satellite_sd.enabled",
                                                       "enable_between_satellite_sd", false);
            bssd_pivot_min_elevation_deg_ = jval<double>(json, "estimation.between_satellite_sd.pivot_min_elevation",
                                                          "bssd_pivot_min_elevation", 45.0);

            // Ambiguity resolution
            ar_mode_            = jval<int>(json, "estimation.ambiguity_resolution.mode",
                                            "ar_mode", 0);
            ar_ratio_threshold_ = jval<double>(json, "estimation.ambiguity_resolution.ratio_threshold",
                                                "ar_ratio_threshold", 3.0);
            ar_elevation_mask_  = jval<double>(json, "estimation.ambiguity_resolution.elevation_mask",
                                                "ar_elevation_mask", 20.0);
            ar_hold_variance_   = jval<double>(json, "estimation.ambiguity_resolution.hold_variance",
                                                "ar_hold_variance", 0.001);

            if (uncombined_enabled_ && !estimate_phase_clock_) {
                estimate_phase_clock_ = true;
                log->info("Uncombined mode: forcing estimate_phase_clock=true "
                    "(required to separate code/phase receiver hardware delays)");
            }

            log->info("Toggles: tropo_grad={}, relin_geom={}, phase_clk={}, if_slip={}, accum_windup={}, bssd={}, velocity={}, ifb={}, lock_clkref={}, req_bia={}, sat_bias={}, no_bia_dw={:.0f}, imu={}",
                estimate_tropo_gradients_ ? "on" : "off",
                relinearize_geometry_     ? "on" : "off",
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

            // =============================================================
            //  Section: filter
            // =============================================================
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
                process_noise_.phase_bias           = pn.value("phase_bias_q", 1e-6);
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

            // Stochastic model
            {
                std::string raw = jval<std::string>(json, "filter.stochastic_model", "stochastic_model", "elevation");
                std::string low = toLower(raw);
                if      (low == "constant")  stochastic_model_ = StochasticModelType::Constant;
                else if (low == "elevation") stochastic_model_ = StochasticModelType::Elevation;
                else if (low == "snr")       stochastic_model_ = StochasticModelType::SNR;
                else { log->warn("Invalid stochastic_model '{}', defaulting to Elevation", raw);
                       stochastic_model_ = StochasticModelType::Elevation; }
                log->info("Stochastic model: {}", raw);
            }
            {
                const auto& sp = jsection(json, "filter.snr_model_params", "snr_model_params");
                snr_model_params_.code_a         = sp.value("code_a", 0.30);
                snr_model_params_.code_b         = sp.value("code_b", 300.0);
                snr_model_params_.phase_a        = sp.value("phase_a", 0.002);
                snr_model_params_.phase_b        = sp.value("phase_b", 0.3);
                snr_model_params_.snr_floor_dbhz = sp.value("snr_floor_dbhz", 20.0);
                snr_model_params_.snr_ceil_dbhz  = sp.value("snr_ceil_dbhz", 55.0);
                log->info("SNR model params: code_a={:.3f}, code_b={:.1f}, phase_a={:.4f}, phase_b={:.3f}, floor={:.0f} dBHz, ceil={:.0f} dBHz",
                    snr_model_params_.code_a, snr_model_params_.code_b,
                    snr_model_params_.phase_a, snr_model_params_.phase_b,
                    snr_model_params_.snr_floor_dbhz, snr_model_params_.snr_ceil_dbhz);
            }

            // =============================================================
            //  Section: model_corrections  (unchanged leaf keys)
            // =============================================================
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
            }
            log->info("Model corrections: tropo={}, rel={}, shapiro={}, set={}, otl={}, pole={}, atm={}, "
                "sat_pco={}, sat_pcv={}, rx_pco={}, rx_pcv={}, windup={}, cpb={}",
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
                model_corrections_.enable_code_phase_bias        ? "on" : "off");

            // =============================================================
            //  Section: receiver
            // =============================================================
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

            // =============================================================
            //  Section: files
            // =============================================================
            observation_path_ = jval<std::string>(json, "files.observation_path", "observation_path", "");
            navigation_path_  = jval<std::string>(json, "files.navigation_path", "navigation_path", "");
            if (observation_path_.empty() || !std::filesystem::exists(observation_path_)) {
                log->warn("Invalid or missing observation RINEX file path: {}", observation_path_.string());
                observation_path_ = "C:/GnssProcessorSupport/products/rnx/GCGO_20241290000_01D_30S_5Epochs.rnx";
            }
            if (navigation_path_.empty() || !std::filesystem::exists(navigation_path_)) {
                log->warn("Invalid or missing navigation RINEX file path: {}", navigation_path_.string());
                navigation_path_ = "C:/GnssProcessorSupport/products/rnx/BRDC00IGS_R_20241290000_01D_MN.rnx";
            }

            product_list_path_ = jval<std::string>(json, "files.product_list_path", "product_list_path", "");
            if (!product_list_path_.empty() && std::filesystem::exists(product_list_path_)) {
                std::ifstream product_file(product_list_path_);
                std::string line;
                while (std::getline(product_file, line)) {
                    line.erase(line.find_last_not_of(" \t\r\n") + 1);
                    if (!line.empty()) product_paths_.push_back(line);
                }
            } else {
                log->warn("Invalid or missing product list path: {}", product_list_path_.string());
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

            // =============================================================
            //  Section: imu
            // =============================================================
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

            // =============================================================
            //  Section: data_download
            // =============================================================
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

                if (download_config_.enabled) {
                    log->info("Data download enabled: root={}, source={}, type={}, station={}, year={}, doy={}",
                        download_config_.data_root.string(), download_config_.product_source,
                        download_config_.product_type, download_config_.station,
                        download_config_.year, download_config_.doy);
                }
            }

            // =============================================================
            //  Section: application mode & realtime
            // =============================================================
            {
                std::string mode_str = json.value("app_mode", "post");
                if (mode_str == "realtime" || mode_str == "real_time" || mode_str == "rt") {
                    app_mode_ = AppMode::RealTime;
                } else {
                    app_mode_ = AppMode::PostProcessing;
                }
                log->info("Application mode: {}", app_mode_ == AppMode::RealTime ? "realtime" : "post");
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
                realtime_config_.output_type        = rt.value("output_type", "console");
                realtime_config_.output_path        = rt.value("output_path", "");
                realtime_config_.max_epoch_buffer   = rt.value("max_epoch_buffer", 100U);
                if (app_mode_ == AppMode::RealTime) {
                    log->info("Real-time config: input={}, ntrip={}:{}/{}, output={}",
                        realtime_config_.input_type,
                        realtime_config_.ntrip_host,
                        realtime_config_.ntrip_port,
                        realtime_config_.ntrip_mountpoint,
                        realtime_config_.output_type);
                }
            }

            // =============================================================
            //  Summary
            // =============================================================
            log_manager.getLogger()->info("Loaded configuration: mode={}, uncombined_enabled={}, elevation_mask={} deg, "
                "observation_path={}, navigation_path={}, {} products, leap_seconds={}",
                processing_mode_ == ProcessingMode::PPP ? "PPP" : "PPP-RTK",
                uncombined_enabled_, elevation_mask_deg_, observation_path_.string(),
                navigation_path_.string(), product_paths_.size(), leap_seconds_tai_);
            return true;
        }
        catch (const std::exception& e) {
            log_manager.getLogger()->error("Failed to parse config file {}: {}", path.string(), e.what());
            return false;
        }
    }

    // =================================================================
    // Product path lookup
    // =================================================================

    std::filesystem::path GnssConfig::getProductPath(const std::string& type) const {
        for (const auto& path : product_paths_) {
            std::string ext = path.extension().string();
            if ((type == "SP3" && ext == ".SP3") ||
                (type == "CLK" && ext == ".CLK") ||
                (type == "ATX" && ext == ".atx") ||
                (type == "BIA" && ext == ".BIA") ||
                (type == "ERP" && ext == ".ERP") ||
                (type == "NC"  && ext == ".nc"))
                return path;
        }
        if (type == "RNX") return navigation_path_;
        return {};
    }

} // namespace GnssProcessor
