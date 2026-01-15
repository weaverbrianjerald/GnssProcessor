#include "GnssConfig.hpp"
//#include "RinexParser.hpp"
#include <fstream>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace GnssProcessor {

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

            // Parse other fields
            uncombined_enabled_ = json.value("uncombined_enabled", false);
            elevation_mask_deg_ = json.value("elevation_mask_deg", 10.0);
            receiver_antenna_orientation_.azimuth_deg = json["receiver_antenna_orientation"].value("azimuth_deg", 0.0);
            receiver_antenna_orientation_.elevation_deg = json["receiver_antenna_orientation"].value("elevation_deg", 90.0);
            process_noise_.position = json["process_noise"].value("position_std", 0.0);
            process_noise_.clock = json["process_noise"].value("clock_bias_std", 5.0);
            process_noise_.troposphere = json["process_noise"].value("troposphere_std", 0.00005);
            process_noise_.ionosphere = json["process_noise"].value("ionosphere_std", 0.02);
            initial_std_.position = json["initial_std"].value("position", 100.0);
            initial_std_.clock = json["initial_std"].value("clock", 100.0);
            initial_std_.troposphere = json["initial_std"].value("troposphere", 0.5);
            initial_std_.ionosphere = json["initial_std"].value("ionosphere", 5.0);
            initial_std_.ambiguity = json["initial_std"].value("ambiguity", 300.0);

            // Default measurement noise (fallback)
            if (json.contains("default_measurement_noise")) {
                const auto& j = json["default_measurement_noise"];
                default_meas_noise_.code_std_m = j.value("code_std_m", 1.0);
                default_meas_noise_.phase_std_m = j.value("phase_std_m", 0.01);
            } else {
                default_meas_noise_.code_std_m = 1.0;
                default_meas_noise_.phase_std_m = 0.01;
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
            leap_seconds_ = json.value("leap_seconds", 37.0);  // Oct 2025 value

            log_manager.getLogger()->info("Loaded configuration: mode={}, uncombined_enabled={}, elevation_mask={} deg, "
                "observation_path={}, navigation_path={}, {} products, leap_seconds={}",
                processing_mode_ == ProcessingMode::PPP ? "PPP" : "PPP-RTK",
                uncombined_enabled_, elevation_mask_deg_, observation_path_.string(),
                navigation_path_.string(), product_paths_.size(),leap_seconds_);
            return true;
        }
        catch (const std::exception& e) {
            log_manager.getLogger()->error("Failed to parse config file {}: {}", path.string(), e.what());
            return false;
        }
    }

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
