#include "GnssConfig.hpp"
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

            // Parse other fields
            uncombined_enabled_ = json.value("uncombined_enabled", false);
            elevation_mask_deg_ = json.value("elevation_mask_deg", 10.0);
            receiver_antenna_orientation_.azimuth_deg = json["receiver_antenna_orientation"].value("azimuth_deg", 0.0);
            receiver_antenna_orientation_.elevation_deg = json["receiver_antenna_orientation"].value("elevation_deg", 90.0);
            process_noise_.position_std = json["process_noise"].value("position_std", 0.1);
            process_noise_.clock_bias_std = json["process_noise"].value("clock_bias_std", 100.0);
            process_noise_.troposphere_std = json["process_noise"].value("troposphere_std", 0.01);
            process_noise_.ionosphere_std = json["process_noise"].value("ionosphere_std", 0.02);
            initial_std_.position = json["initial_std"].value("position", 100.0);
            initial_std_.clock = json["initial_std"].value("clock", 1000.0);
            initial_std_.troposphere = json["initial_std"].value("troposphere", 0.5);
            initial_std_.ionosphere = json["initial_std"].value("ionosphere", 5.0);
            initial_std_.ambiguity = json["initial_std"].value("ambiguity", 0.1);

            // Parse signal priorities
            if (json.contains("signal_priorities")) {
                for (auto& [system, signals] : json["signal_priorities"].items()) {
                    signal_priorities_[system] = signals.get<std::vector<std::string>>();
                }
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

            log_manager.getLogger()->info("Loaded configuration: mode={}, uncombined_enabled={}, elevation_mask={} deg, "
                "observation_path={}, navigation_path={}, {} products",
                processing_mode_ == ProcessingMode::PPP ? "PPP" : "PPP-RTK",
                uncombined_enabled_, elevation_mask_deg_, observation_path_.string(),
                navigation_path_.string(), product_paths_.size());
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
