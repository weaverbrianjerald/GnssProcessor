/*!
 * \file GnssConfig.cpp
 * \brief JSON configuration loading for GnssConfig.
 *
 * Organization:
 *  - String / enum helpers
 *  - loadFromJson  (grouped by parsing section)
 *  - getProductPath
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

            nlohmann::json json;
            file >> json;
            auto log = log_manager.getLogger("gnss_config");

            // --- Processing mode ---
            std::string mode = json.value("processing_mode", "PPP");
            processing_mode_ = (mode == "PPP-RTK") ? ProcessingMode::PPP_RTK : ProcessingMode::PPP;

            // --- Signal mode ---
            {
                std::string raw = json.value("signal_mode", "Dual");
                std::string low = toLower(raw);
                if      (low == "dual")   signal_mode_ = SignalMode::Dual;
                else if (low == "triple") signal_mode_ = SignalMode::Triple;
                else if (low == "all")    signal_mode_ = SignalMode::All;
                else { log->warn("Invalid signal_mode '{}', defaulting to Dual", raw); signal_mode_ = SignalMode::Dual; }
                log->info("Signal mode: {}", raw);
            }

            // --- Satellite position/velocity interpolation ---
            {
                std::string raw = json.value("satellite_interpolation_mode", "Lagrange");
                std::string low = toLower(raw);
                if      (low == "lagrange") satellite_interpolation_mode_ = SatelliteInterpolationMode::Lagrange;
                else if (low == "finite")   satellite_interpolation_mode_ = SatelliteInterpolationMode::Finite;
                else if (low == "hermite")  satellite_interpolation_mode_ = SatelliteInterpolationMode::Hermite;
                else if (low == "spline")   satellite_interpolation_mode_ = SatelliteInterpolationMode::Spline;
                else { log->warn("Invalid satellite_interpolation_mode '{}', defaulting to Lagrange", raw); satellite_interpolation_mode_ = SatelliteInterpolationMode::Lagrange; }
                log->info("Satellite Pos Vel interpolation mode: {}", raw);
            }

            // --- Satellite clock interpolation ---
            {
                std::string raw = json.value("satellite_clock_interpolation_mode", "Linear");
                std::string low = toLower(raw);
                if      (low == "linear")   satellite_clock_interpolation_mode_ = SatelliteClockInterpolationMode::Linear;
                else if (low == "lagrange") satellite_clock_interpolation_mode_ = SatelliteClockInterpolationMode::Lagrange;
                else { log->warn("Invalid satellite_clock_interpolation_mode '{}', defaulting to Linear", raw); satellite_clock_interpolation_mode_ = SatelliteClockInterpolationMode::Linear; }
                log->info("Satellite Clock interpolation mode: {}", raw);
            }

            // --- Scalar parameters ---
            uncombined_enabled_ = json.value("uncombined_enabled", false);
            if (processing_mode_ == ProcessingMode::PPP_RTK) {
                uncombined_enabled_ = true;
                log->info("PPP-RTK mode: forcing uncombined_enabled=true");
            }
            elevation_mask_deg_ = json.value("elevation_mask_deg", 10.0);

            // --- Antenna orientation ---
            receiver_antenna_orientation_.azimuth_deg = json["receiver_antenna_orientation"].value("azimuth_deg", 0.0);
            receiver_antenna_orientation_.elevation_deg = json["receiver_antenna_orientation"].value("elevation_deg", 90.0);

            // --- Process noise spectral densities (m²/s) ---
            {
                const auto& pn = json["process_noise"];
                process_noise_.position            = pn.value("position_q", 0.0);
                process_noise_.clock               = pn.value("clock_bias_q", 1e4);
                process_noise_.troposphere         = pn.value("troposphere_q", 1e-8);
                process_noise_.ionosphere          = pn.value("ionosphere_q", 0.10);
                process_noise_.system_time_offset  = pn.value("system_time_offset_q", 1e-7);
                process_noise_.code_bias           = pn.value("code_bias_q", 1e-4);
                process_noise_.tropo_gradient      = pn.value("tropo_gradient_q", 5e-12);
                process_noise_.phase_clock         = pn.value("phase_clock_q", 1e-9);
                process_noise_.velocity            = pn.value("velocity_q", 1e2);
            }

            // --- Initial standard deviations ---
            {
                const auto& is = json["initial_std"];
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
            }

            // --- Default measurement noise ---
            if (json.contains("default_measurement_noise")) {
                const auto& j = json["default_measurement_noise"];
                default_meas_noise_.code_std_m  = j.value("code_std_m", 0.600);
                default_meas_noise_.phase_std_m = j.value("phase_std_m", 0.002);
                default_meas_noise_.doppler_std_m = j.value("doppler_std_m", 0.5);
            } else {
                default_meas_noise_.code_std_m  = 0.600;
                default_meas_noise_.phase_std_m = 0.002;
                default_meas_noise_.doppler_std_m = 0.5;
            }
            log->info("Default measurement noise: code_std={:.3f} m, phase_std={:.3f} m, doppler_std={:.3f} Hz",
                default_meas_noise_.code_std_m, default_meas_noise_.phase_std_m, default_meas_noise_.doppler_std_m);

            // --- Boolean toggles ---
            estimate_tropo_gradients_   = json.value("estimate_tropo_gradients", false);
            relinearize_geometry_       = json.value("relinearize_geometry", false);
            estimate_phase_clock_       = json.value("estimate_phase_clock", false);
            enable_if_slip_detector_    = json.value("enable_if_slip_detector", true);
            accumulate_phase_windup_    = json.value("accumulate_phase_windup", false);
            enable_between_satellite_sd_ = json.value("enable_between_satellite_sd", false);
            estimate_velocity_           = json.value("estimate_velocity", false);
            estimate_ifb_                = json.value("estimate_ifb", true);

            ar_mode_                = json.value("ar_mode", 0);
            ar_ratio_threshold_     = json.value("ar_ratio_threshold", 3.0);
            ar_elevation_mask_      = json.value("ar_elevation_mask", 20.0);
            ar_hold_variance_       = json.value("ar_hold_variance", 0.001);

            if (uncombined_enabled_ && !estimate_phase_clock_) {
                estimate_phase_clock_ = true;
                log->info("Uncombined mode: forcing estimate_phase_clock=true "
                    "(required to separate code/phase receiver hardware delays)");
            }

            log->info("Toggles: tropo_grad={}, relin_geom={}, phase_clk={}, if_slip={}, accum_windup={}, bssd={}, velocity={}, ifb={}, imu={}",
                estimate_tropo_gradients_ ? "on" : "off",
                relinearize_geometry_     ? "on" : "off",
                estimate_phase_clock_     ? "on" : "off",
                enable_if_slip_detector_  ? "on" : "off",
                accumulate_phase_windup_  ? "on" : "off",
                enable_between_satellite_sd_ ? "on" : "off",
                estimate_velocity_        ? "on" : "off",
                estimate_ifb_             ? "on" : "off",
                imu_config_.enabled
                    ? (imu_config_.coupling_mode == ImuCouplingMode::TightCoupled ? "tight" : "loose")
                    : "off");

            // --- Signal priorities ---
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
            if (json.contains("signal_priorities")) {
                for (const auto& [system, value] : json["signal_priorities"].items()) {
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

            // --- Active systems ---
            active_systems_.clear();
            if (json.contains("active_systems")) {
                active_systems_ = json["active_systems"].get<std::vector<std::string>>();
                for (const auto& system : active_systems_)
                    if (system != "GPS" && system != "Galileo" && system != "GLONASS" &&
                        system != "BeiDou" && system != "QZSS" && system != "SBAS")
                        log_manager.getLogger()->warn("Unknown system in active_systems: {}", system);
            }
            if (active_systems_.empty()) {
                active_systems_ = { "GPS", "Galileo", "GLONASS", "BeiDou", "QZSS", "SBAS" };
                log_manager.getLogger()->info("No active systems specified, using default: GPS, Galileo, GLONASS, BeiDou, QZSS, SBAS");
            } else {
                log_manager.getLogger()->info("Active systems: {}", joinStrings(active_systems_));
            }

            // --- File paths ---
            observation_path_ = json.value("observation_path", "");
            navigation_path_ = json.value("navigation_path", "");
            if (observation_path_.empty() || !std::filesystem::exists(observation_path_)) {
                log_manager.getLogger()->warn("Invalid or missing observation RINEX file path: {}", observation_path_.string());
                observation_path_ = "C:/GnssProcessorSupport/products/rnx/GCGO_20241290000_01D_30S_5Epochs.rnx";
            }
            if (navigation_path_.empty() || !std::filesystem::exists(navigation_path_)) {
                log_manager.getLogger()->warn("Invalid or missing navigation RINEX file path: {}", navigation_path_.string());
                navigation_path_ = "C:/GnssProcessorSupport/products/rnx/BRDC00IGS_R_20241290000_01D_MN.rnx";
            }

            // --- Product list ---
            product_list_path_ = json.value("product_list_path", "");
            if (!product_list_path_.empty() && std::filesystem::exists(product_list_path_)) {
                std::ifstream product_file(product_list_path_);
                std::string line;
                while (std::getline(product_file, line)) {
                    line.erase(line.find_last_not_of(" \t\r\n") + 1);
                    if (!line.empty()) product_paths_.push_back(line);
                }
            } else {
                log_manager.getLogger()->warn("Invalid or missing product list path: {}", product_list_path_.string());
            }

            // --- Troposphere defaults ---
            zhd_pressure_mbar = json.value("zhd_pressure_mbar", 1013.25);
            zhd_temp_kelvin   = json.value("zhd_temp_kelvin", 288.15);
            log->info("ZHD defaults: P={:.1f} mbar, T={:.1f} K", zhd_pressure_mbar, zhd_temp_kelvin);

            // --- Leap seconds ---
            leap_seconds_tai_ = json.value("leap_seconds_tai", 37.0);

            // --- Ocean tidal loading BLQ ---
            std::string otl_path_str = json.value("otl_blq_path", "");
            if (!otl_path_str.empty()) {
                otl_blq_path_ = otl_path_str;
                if (std::filesystem::exists(otl_blq_path_))
                    log->info("OTL BLQ file: {}", otl_blq_path_.string());
                else {
                    log->warn("OTL BLQ file not found: {}", otl_blq_path_.string());
                    otl_blq_path_.clear();
                }
            }

            // --- Reference position (optional) ---
            if (json.contains("reference_position_ecef")) {
                const auto& rp = json["reference_position_ecef"];
                if (rp.is_object() && rp.contains("x") && rp.contains("y") && rp.contains("z")) {
                    reference_position_ecef_ = { rp.value("x", 0.0), rp.value("y", 0.0), rp.value("z", 0.0) };
                    has_reference_position_ = true;
                    log->info("Reference position ECEF: ({:.4f}, {:.4f}, {:.4f}) m",
                        reference_position_ecef_[0], reference_position_ecef_[1], reference_position_ecef_[2]);
                } else {
                    log->warn("reference_position_ecef present but missing x/y/z fields — ignored");
                }
            }

            // --- Model correction flags ---
            if (json.contains("model_corrections")) {
                const auto& mc = json["model_corrections"];
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

            // --- Stochastic model ---
            {
                std::string raw = json.value("stochastic_model", "elevation");
                std::string low = toLower(raw);
                if      (low == "constant")  stochastic_model_ = StochasticModelType::Constant;
                else if (low == "elevation") stochastic_model_ = StochasticModelType::Elevation;
                else if (low == "snr")       stochastic_model_ = StochasticModelType::SNR;
                else { log->warn("Invalid stochastic_model '{}', defaulting to Elevation", raw); stochastic_model_ = StochasticModelType::Elevation; }
                log->info("Stochastic model: {}", raw);
            }
            if (json.contains("snr_model_params")) {
                const auto& sp = json["snr_model_params"];
                snr_model_params_.code_a        = sp.value("code_a", 0.30);
                snr_model_params_.code_b        = sp.value("code_b", 300.0);
                snr_model_params_.phase_a       = sp.value("phase_a", 0.002);
                snr_model_params_.phase_b       = sp.value("phase_b", 0.3);
                snr_model_params_.snr_floor_dbhz = sp.value("snr_floor_dbhz", 20.0);
                snr_model_params_.snr_ceil_dbhz  = sp.value("snr_ceil_dbhz", 55.0);
                log->info("SNR model params: code_a={:.3f}, code_b={:.1f}, phase_a={:.4f}, phase_b={:.3f}, floor={:.0f} dBHz, ceil={:.0f} dBHz",
                    snr_model_params_.code_a, snr_model_params_.code_b,
                    snr_model_params_.phase_a, snr_model_params_.phase_b,
                    snr_model_params_.snr_floor_dbhz, snr_model_params_.snr_ceil_dbhz);
            }

            // --- IMU configuration ---
            imu_config_.enabled = json.value("enable_imu", false);
            if (json.contains("imu_config")) {
                const auto& ic = json["imu_config"];
                std::string imu_path_str = ic.value("imu_file_path", "");
                if (!imu_path_str.empty()) {
                    imu_config_.imu_file_path = imu_path_str;
                    if (!std::filesystem::exists(imu_config_.imu_file_path)) {
                        log->warn("IMU file not found: {}", imu_config_.imu_file_path.string());
                    }
                }
                // Coupling mode
                {
                    std::string raw = ic.value("imu_coupling_mode", "loosely_coupled");
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

                // Tightly-coupled requires velocity estimation (Doppler updates velocity).
                if (imu_config_.coupling_mode == ImuCouplingMode::TightCoupled && !estimate_velocity_) {
                    estimate_velocity_ = true;
                    log->info("Tightly-coupled IMU: forcing estimate_velocity=true");
                }
            }

            // --- Data download configuration ---
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

            // --- Application mode ---
            {
                std::string mode_str = json.value("app_mode", "post");
                if (mode_str == "realtime" || mode_str == "real_time" || mode_str == "rt") {
                    app_mode_ = AppMode::RealTime;
                } else {
                    app_mode_ = AppMode::PostProcessing;
                }
                log->info("Application mode: {}", app_mode_ == AppMode::RealTime ? "realtime" : "post");
            }

            // --- Real-time configuration ---
            if (json.contains("realtime_config")) {
                const auto& rt = json["realtime_config"];
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

            // --- Summary ---
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
