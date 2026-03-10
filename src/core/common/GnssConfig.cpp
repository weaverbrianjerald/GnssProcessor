/*!
 * \file GnssConfig.cpp
 * \brief Implementation of GnssConfig JSON loading and product path lookup.
 *
 * Adds JSON-based configuration parsing and helpers used by the GNSS processor.
 * Doxygen-style comments document runtime behaviour, defaults, and logging.
 */

#include "GnssConfig.hpp"
//#include "RinexParser.hpp"
#include <fstream>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace GnssProcessor {

    /**
     * @brief Load configuration values from a JSON file into this GnssConfig instance.
     *
     * The method opens and parses the JSON file at \p path, then populates
     * internal configuration members such as processing and signal modes,
     * measurement / process noise defaults, antenna orientation, file paths,
     * product list entries, active GNSS systems, troposphere defaults and
     * leap seconds. The function uses \p log_manager to emit informative,
     * warning and error messages during parsing and when falling back to
     * defaults.
     *
     * Supported JSON keys (examples):
     *  - "processing_mode": "PPP" or "PPP-RTK"
     *  - "signal_mode": "Dual", "Triple", "All" (case-insensitive)
     *  - "uncombined_enabled": bool
     *  - "elevation_mask_deg": number
     *  - "receiver_antenna_orientation": { "azimuth_deg": number, "elevation_deg": number }
     *  - "process_noise": { "position_q": number, "clock_bias_q": number, "troposphere_q": number, "ionosphere_q": number }
     *  - "initial_std": { "position": number, "clock": number, "troposphere": number, "ionosphere": number, "ambiguity": number }
     *  - "default_measurement_noise": { "code_std_m": number, "phase_std_m": number }
     *  - "signal_priorities": object mapping GNSS system -> array of signal strings
     *  - "active_systems": array of strings (e.g. ["GPS", "Galileo", ...])
     *  - "observation_path", "navigation_path", "product_list_path"
     *  - "zhd_pressure_mbar", "zhd_temp_kelvin"
     *  - "leap_seconds"
     *
     * If certain keys or files are missing, sensible defaults are applied and a
     * warning is logged. For example, missing active systems will result in a
     * default set of {"GPS","Galileo","GLONASS","BeiDou","QZSS","SBAS"}.
     *
     * @param path Filesystem path to the JSON configuration file.
     * @param log_manager Reference to a LogManager used for logging messages.
     * @return true if configuration loaded and parsed successfully; false on
     *         file open error or parsing exception.
     */
    bool GnssConfig::loadFromJson(const std::filesystem::path& path, LogManager& log_manager) {
        try {
            std::ifstream file(path);
            if (!file.is_open()) {
                log_manager.getLogger()->error("Failed to open config file: {}", path.string());
                return false;
            }

            nlohmann::json json;
            file >> json;

            // Parse processing mode
            std::string mode = json.value("processing_mode", "PPP");
            processing_mode_ = (mode == "PPP-RTK") ? ProcessingMode::PPP_RTK : ProcessingMode::PPP;

            // Parse signal mode
            std::string signal_mode = json.value("signal_mode", "Dual");
            std::string signal_mode_lower = signal_mode;
            std::transform(signal_mode_lower.begin(), signal_mode_lower.end(), signal_mode_lower.begin(), ::tolower);
            if (signal_mode_lower == "dual") {
                signal_mode_ = SignalMode::Dual;
            }
            else if (signal_mode_lower == "triple") {
                signal_mode_ = SignalMode::Triple;
            }
            else if (signal_mode_lower == "all") {
                signal_mode_ = SignalMode::All;
            }
            else {
                log_manager.getLogger("gnss_config")->warn("Invalid signal_mode '{}', defaulting to Dual", signal_mode);
                signal_mode_ = SignalMode::Dual;
            }
            log_manager.getLogger("gnss_config")->info("Signal mode: {}", signal_mode);

            // Parse satellite position and velocity interpolation mode
            std::string sv_mode = json.value("satellite_interpolation_mode", "Lagrange");
            std::string sv_mode_lower = sv_mode;
            std::transform(sv_mode.begin(), sv_mode.end(), sv_mode.begin(), ::tolower);
            if (sv_mode == "lagrange") {
                satellite_interpolation_mode_ = SatelliteInterpolationMode::Lagrange;
            }
            else if (sv_mode == "finite") {
                satellite_interpolation_mode_ = SatelliteInterpolationMode::Finite;
            }
            else if (sv_mode == "hermite") {
                satellite_interpolation_mode_ = SatelliteInterpolationMode::Hermite;
            }
            else if (sv_mode == "spline") {
                satellite_interpolation_mode_ = SatelliteInterpolationMode::Spline;
            }
            else {
                log_manager.getLogger("gnss_config")->warn("Invalid satellite_interpolation_mode '{}', defaulting to Lagrange", sv_mode);
                satellite_interpolation_mode_ = SatelliteInterpolationMode::Lagrange;
            }
            log_manager.getLogger("gnss_config")->info("Satellite Pos Vel interpolation mode: {}", sv_mode);

            // Parse satellite clock interpolation mode
            std::string sv_clock_mode = json.value("satellite_clock_interpolation_mode", "Linear");
            std::string sv_clock_mode_lower = sv_clock_mode;
            std::transform(sv_clock_mode.begin(), sv_clock_mode.end(), sv_clock_mode.begin(), ::tolower);
            if (sv_clock_mode == "linear") {
                satellite_clock_interpolation_mode_ = SatelliteClockInterpolationMode::Linear;
            }
            else if (sv_clock_mode == "lagrange") {
                satellite_clock_interpolation_mode_ = SatelliteClockInterpolationMode::Lagrange;
            }
            else {
                log_manager.getLogger("gnss_config")->warn("Invalid satellite_interpolation_mode '{}', defaulting to Lagrange", sv_clock_mode);
                satellite_clock_interpolation_mode_ = SatelliteClockInterpolationMode::Linear;
            }
            log_manager.getLogger("gnss_config")->info("Satellite Clock interpolation mode: {}", sv_clock_mode);

            // Parse other fields
            uncombined_enabled_ = json.value("uncombined_enabled", false);
            elevation_mask_deg_ = json.value("elevation_mask_deg", 10.0);

            receiver_antenna_orientation_.azimuth_deg = json["receiver_antenna_orientation"].value("azimuth_deg", 0.0);
            receiver_antenna_orientation_.elevation_deg = json["receiver_antenna_orientation"].value("elevation_deg", 90.0);

            // Process noise spectral densities (m²/s): variance_per_epoch = q * dt
            process_noise_.position = json["process_noise"].value("position_q", 0.0);       // 0 for static
            process_noise_.clock = json["process_noise"].value("clock_bias_q", 1e8);        // white-noise clock
            process_noise_.troposphere = json["process_noise"].value("troposphere_q", 1e-8); // ~0.5 mm/?hr (IGS default)
            process_noise_.ionosphere = json["process_noise"].value("ionosphere_q", 0.02);   // PPP-RTK only
            process_noise_.system_time_offset = json["process_noise"].value("system_time_offset_q", 1e-7); // stable ISB drift
            process_noise_.tropo_gradient = json["process_noise"].value("tropo_gradient_q", 5e-12); // ~0.01 mm/sqrt(hr)

            // Troposphere gradient estimation toggle
            estimate_tropo_gradients_ = json.value("estimate_tropo_gradients", false);
            log_manager.getLogger("gnss_config")->info("Troposphere gradient estimation: {}", estimate_tropo_gradients_ ? "enabled" : "disabled");

            // Geometry relinearization toggle
            relinearize_geometry_ = json.value("relinearize_geometry", false);
            log_manager.getLogger("gnss_config")->info("Geometry relinearization: {}", relinearize_geometry_ ? "enabled" : "disabled");

            initial_std_.position = json["initial_std"].value("position", 100.0);
            initial_std_.clock = json["initial_std"].value("clock", 1e5);
            initial_std_.troposphere = json["initial_std"].value("troposphere", 0.5);
            initial_std_.ionosphere = json["initial_std"].value("ionosphere", 5.0);
            initial_std_.ambiguity = json["initial_std"].value("ambiguity", 1000.0);
            initial_std_.system_time_offset = json["initial_std"].value("system_time_offset", 100.0);
            initial_std_.tropo_gradient = json["initial_std"].value("tropo_gradient", 0.001);

            // Default measurement noise (fallback)
            if (json.contains("default_measurement_noise")) {
                const auto& j = json["default_measurement_noise"];
				default_meas_noise_.code_std_m = j.value("code_std_m", 0.30);       // all code  (C1W, C2W, C2X, etc)
                default_meas_noise_.phase_std_m = j.value("phase_std_m", 0.003);    // all phase (L1W, L2W, L2X, etc)
            } else {
                default_meas_noise_.code_std_m = 0.30;
                default_meas_noise_.phase_std_m = 0.003;
            }
            log_manager.getLogger("gnss_config")->info(
                "Default measurement noise: code_std={:.3f} m, phase_std={:.3f} m",
                default_meas_noise_.code_std_m, default_meas_noise_.phase_std_m);

            // Parse signal priorities
            signal_priorities_.clear();
            if (json.contains("signal_priorities")) {
                for (const auto& [system, signals] : json["signal_priorities"].items()) {
                    signal_priorities_[system] = signals.get<std::vector<std::string>>();
                }
            }

            // Parse active systems
            active_systems_.clear();
            if (json.contains("active_systems")) {
                active_systems_ = json["active_systems"].get<std::vector<std::string>>();
                for (const auto& system : active_systems_) {
                    if (system != "GPS" && system != "Galileo" && system != "GLONASS" &&
                        system != "BeiDou" && system != "QZSS" && system != "SBAS") {
                        log_manager.getLogger()->warn("Unknown system in active_systems: {}", system);
                    }
                }
            }
            if (active_systems_.empty()) {
                //active_systems_ = { System::GPS, System::Galileo, System::GLONASS, System::BeiDou, System::QZSS, System::SBAS };
                active_systems_ = { "GPS", "Galileo", "GLONASS", "BeiDou", "QZSS", "SBAS" };
                log_manager.getLogger()->info("No active systems specified, using default: GPS, Galileo, GLONASS, BeiDou, QZSS, SBAS");
            }
            else {
                std::string active_systems_str = active_systems_.empty() ? "none" : "";
                for (const auto& sys : active_systems_) {
                    active_systems_str += sys + ", ";
                }
                if (!active_systems_str.empty()) {
                    active_systems_str = active_systems_str.substr(0, active_systems_str.size() - 2);
                }
                log_manager.getLogger()->info("Active systems: {}", active_systems_str);
            }

            // Parse observation and navigation paths
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

            // Parse product list path
            product_list_path_ = json.value("product_list_path", "");
            if (!product_list_path_.empty() && std::filesystem::exists(product_list_path_)) {
                std::ifstream product_file(product_list_path_);
                std::string line;
                while (std::getline(product_file, line)) {
                    line.erase(line.find_last_not_of(" \t\r\n") + 1);
                    if (!line.empty()) {
                        product_paths_.push_back(line);
                    }
                }
                product_file.close();
            }
            else {
                log_manager.getLogger()->warn("Invalid or missing product list path: {}", product_list_path_.string());
            }

            // Troposphere
            zhd_pressure_mbar = json.value("zhd_pressure_mbar", 1013.25);
            zhd_temp_kelvin = json.value("zhd_temp_kelvin", 288.15);
            log_manager.getLogger("gnss_config")->info("ZHD defaults: P={:.1f} mbar, T={:.1f} K", zhd_pressure_mbar, zhd_temp_kelvin);

            // Leap seconds
            leap_seconds_tai_ = json.value("leap_seconds_tai", 37.0);  // Oct 2025 TAI-UTC value

            // Ocean tidal loading BLQ file path (optional)
            std::string otl_path_str = json.value("otl_blq_path", "");
            if (!otl_path_str.empty()) {
                otl_blq_path_ = otl_path_str;
                if (std::filesystem::exists(otl_blq_path_)) {
                    log_manager.getLogger("gnss_config")->info("OTL BLQ file: {}", otl_blq_path_.string());
                } else {
                    log_manager.getLogger("gnss_config")->warn("OTL BLQ file not found: {}", otl_blq_path_.string());
                    otl_blq_path_.clear();
                }
            }

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

    /**
     * @brief Retrieve a product file path matching the requested product \p type.
     *
     * The function searches the previously-loaded product list (\c product_paths_)
     * for a file with an extension matching common product types. The search is
     * case-sensitive with respect to the requested \p type. If \p type equals
     * "RNX", the method returns the configured navigation RINEX path.
     *
     * Recognized mappings:
     *  - "SP3" -> file extension ".SP3"
     *  - "CLK" -> file extension ".CLK"
     *  - "ATX" -> file extension ".atx"
     *  - "BIA" -> file extension ".BIA"
     *  - "ERP" -> file extension ".ERP"
     *  - "NC"  -> file extension ".nc"
     *  - "RNX" -> returns navigation_path_
     *
     * @param type Product type string (e.g. "SP3", "CLK", "RNX").
     * @return filesystem::path to the first matching product; empty path if not found.
     */
    std::filesystem::path GnssConfig::getProductPath(const std::string& type) const {
        for (const auto& path : product_paths_) {
            std::string ext = path.extension().string();
            if ((type == "SP3" && ext == ".SP3") ||
                (type == "CLK" && ext == ".CLK") ||
                (type == "ATX" && ext == ".atx") ||
                (type == "BIA" && ext == ".BIA") ||
                (type == "ERP" && ext == ".ERP") ||
                (type == "NC" && ext == ".nc")) {
                return path;
            }
        }
        if (type == "RNX") {
            return navigation_path_;
        }
        return std::filesystem::path();
    }

} // namespace GnssProcessor
