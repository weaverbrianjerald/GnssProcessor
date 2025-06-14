#include "GnssConfig.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>

namespace GnssProcessor {

    GnssConfig::GnssConfig()
        : elevation_mask_deg_(5.0), positioning_mode_(PositioningMode::Static), stochastic_model_(StochasticModel::RandomWalk),
        initial_pos_std_(100.0), initial_clock_std_(1000.0), initial_tropo_std_(0.5), initial_iono_std_(5.0),
        initial_ambiguity_std_(10.0), pos_process_noise_std_(0.01), clock_process_noise_std_(0.1),
        tropo_process_noise_std_(0.001), iono_process_noise_std_(0.05), receiver_antenna_azimuth_deg_(0.0),
        receiver_antenna_elevation_deg_(0.0) {
    }

    bool GnssConfig::loadFromJson(const std::filesystem::path& config_path, LogManager& log_manager) {
        try {
            std::ifstream file(config_path);
            if (!file.is_open()) {
                log_manager.getLogger()->error("Failed to open config file: {}", config_path.string());
                return false;
            }
            nlohmann::json json;
            file >> json;

            // Set file paths
            if (json.contains("rinex_file_path")) setRinexFilePath(json["rinex_file_path"].get<std::string>());
            if (json.contains("sinex_file_path")) setSinexFilePath(json["sinex_file_path"].get<std::string>());
            if (json.contains("rtcm_file_path")) setRtcmFilePath(json["rtcm_file_path"].get<std::string>());
            if (json.contains("product_list_path")) setProductListPath(json["product_list_path"].get<std::string>());

            // Set other parameters
            if (json.contains("elevation_mask_deg")) setElevationMaskDeg(json["elevation_mask_deg"].get<double>());
            if (json.contains("signal_priorities")) {
                std::map<std::string, std::vector<std::string>> priorities;
                for (const auto& [key, value] : json["signal_priorities"].items()) {
                    priorities[key] = value.get<std::vector<std::string>>();
                }
                setSignalPriorities(priorities);
            }
            if (json.contains("stochastic_model")) {
                std::string model = json["stochastic_model"].get<std::string>();
                setStochasticModel(model == "white_noise" ? StochasticModel::WhiteNoise : StochasticModel::RandomWalk);
            }
            if (json.contains("initial_pos_std")) setInitialPosStd(json["initial_pos_std"].get<double>());
            if (json.contains("initial_clock_std")) setInitialClockStd(json["initial_clock_std"].get<double>());
            if (json.contains("initial_tropo_std")) setInitialTropoStd(json["initial_tropo_std"].get<double>());
            if (json.contains("initial_iono_std")) setInitialIonoStd(json["initial_iono_std"].get<double>());
            if (json.contains("initial_ambiguity_std")) setInitialAmbiguityStd(json["initial_ambiguity_std"].get<double>());
            if (json.contains("pos_process_noise_std")) setPosProcessNoiseStd(json["pos_process_noise_std"].get<double>());
            if (json.contains("clock_process_noise_std")) setClockProcessNoiseStd(json["clock_process_noise_std"].get<double>());
            if (json.contains("tropo_process_noise_std")) setTropoProcessNoiseStd(json["tropo_process_noise_std"].get<double>());
            if (json.contains("iono_process_noise_std")) setIonoProcessNoiseStd(json["iono_process_noise_std"].get<double>());
            if (json.contains("receiver_antenna_azimuth_deg")) setReceiverAntennaAzimuthDeg(json["receiver_antenna_azimuth_deg"].get<double>());
            if (json.contains("receiver_antenna_elevation_deg")) setReceiverAntennaElevationDeg(json["receiver_antenna_elevation_deg"].get<double>());

            log_manager.getLogger()->info("Loaded config from: {}", config_path.string());
            return true;
        }
        catch (const std::exception& e) {
            log_manager.getLogger()->error("Failed to parse config file {}: {}", config_path.string(), e.what());
            return false;
        }
    }

    void GnssConfig::setRinexFilePath(const std::string& path) { rinex_file_path_ = path; }
    void GnssConfig::setSinexFilePath(const std::string& path) { sinex_file_path_ = path; }
    void GnssConfig::setRtcmFilePath(const std::string& path) { rtcm_file_path_ = path; }
    void GnssConfig::setProductListPath(const std::string& path) { product_list_path_ = path; }
    void GnssConfig::setElevationMaskDeg(double deg) { elevation_mask_deg_ = deg; }
    void GnssConfig::setSignalPriorities(const std::map<std::string, std::vector<std::string>>& priorities) {
        signal_priorities_ = priorities;
    }
    void GnssConfig::setStochasticModel(StochasticModel model) { stochastic_model_ = model; }
    void GnssConfig::setInitialPosStd(double std) { initial_pos_std_ = std; }
    void GnssConfig::setInitialClockStd(double std) { initial_clock_std_ = std; }
    void GnssConfig::setInitialTropoStd(double std) { initial_tropo_std_ = std; }
    void GnssConfig::setInitialIonoStd(double std) { initial_iono_std_ = std; }
    void GnssConfig::setInitialAmbiguityStd(double std) { initial_ambiguity_std_ = std; }
    void GnssConfig::setPosProcessNoiseStd(double std) { pos_process_noise_std_ = std; }
    void GnssConfig::setClockProcessNoiseStd(double std) { clock_process_noise_std_ = std; }
    void GnssConfig::setTropoProcessNoiseStd(double std) { tropo_process_noise_std_ = std; }
    void GnssConfig::setIonoProcessNoiseStd(double std) { iono_process_noise_std_ = std; }
    void GnssConfig::setReceiverAntennaAzimuthDeg(double deg) { receiver_antenna_azimuth_deg_ = deg; }
    void GnssConfig::setReceiverAntennaElevationDeg(double deg) { receiver_antenna_elevation_deg_ = deg; }

} // namespace GnssProcessor
//#endif