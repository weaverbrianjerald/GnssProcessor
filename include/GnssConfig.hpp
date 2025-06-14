#pragma once

#include "LogManager.hpp"
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace GnssProcessor {

    enum class PositioningMode { Static, Kinematic };
    enum class StochasticModel { RandomWalk, WhiteNoise };

    class GnssConfig {
    public:
        GnssConfig();
        bool loadFromJson(const std::filesystem::path& config_path, LogManager& log_manager);

        // Getters
        std::filesystem::path getRinexFilePath() const { return rinex_file_path_; }
        std::filesystem::path getSinexFilePath() const { return sinex_file_path_; }
        std::filesystem::path getRtcmFilePath() const { return rtcm_file_path_; }
        std::filesystem::path getProductListPath() const { return product_list_path_; }
        double getElevationMaskDeg() const { return elevation_mask_deg_; }
        std::map<std::string, std::vector<std::string>> getSignalPriorities() const { return signal_priorities_; }
        PositioningMode getPositioningMode() const { return positioning_mode_; }
        StochasticModel getStochasticModel() const { return stochastic_model_; }
        double getInitialPosStd() const { return initial_pos_std_; }
        double getInitialClockStd() const { return initial_clock_std_; }
        double getInitialTropoStd() const { return initial_tropo_std_; }
        double getInitialIonoStd() const { return initial_iono_std_; }
        double getInitialAmbiguityStd() const { return initial_ambiguity_std_; }
        double getPosProcessNoiseStd() const { return pos_process_noise_std_; }
        double getClockProcessNoiseStd() const { return clock_process_noise_std_; }
        double getTropoProcessNoiseStd() const { return tropo_process_noise_std_; }
        double getIonoProcessNoiseStd() const { return iono_process_noise_std_; }
        double getReceiverAntennaAzimuthDeg() const { return receiver_antenna_azimuth_deg_; }
        double getReceiverAntennaElevationDeg() const { return receiver_antenna_elevation_deg_; }

        // Setters
        void setRinexFilePath(const std::string& path);
        void setSinexFilePath(const std::string& path);
        void setRtcmFilePath(const std::string& path);
        void setProductListPath(const std::string& path);
        void setElevationMaskDeg(double deg);
        void setSignalPriorities(const std::map<std::string, std::vector<std::string>>& priorities);
        void setPositioningMode(PositioningMode mode) { positioning_mode_ = mode; }
        void setStochasticModel(StochasticModel model);
        void setInitialPosStd(double std);
        void setInitialClockStd(double std);
        void setInitialTropoStd(double std);
        void setInitialIonoStd(double std);
        void setInitialAmbiguityStd(double std);
        void setPosProcessNoiseStd(double std);
        void setClockProcessNoiseStd(double std);
        void setTropoProcessNoiseStd(double std);
        void setIonoProcessNoiseStd(double std);
        void setReceiverAntennaAzimuthDeg(double deg);
        void setReceiverAntennaElevationDeg(double deg);

    private:
        std::filesystem::path rinex_file_path_;
        std::filesystem::path sinex_file_path_;
        std::filesystem::path rtcm_file_path_;
        std::filesystem::path product_list_path_;
        double elevation_mask_deg_;
        std::map<std::string, std::vector<std::string>> signal_priorities_;
        PositioningMode positioning_mode_;
        StochasticModel stochastic_model_;
        double initial_pos_std_;
        double initial_clock_std_;
        double initial_tropo_std_;
        double initial_iono_std_;
        double initial_ambiguity_std_;
        double pos_process_noise_std_;
        double clock_process_noise_std_;
        double tropo_process_noise_std_;
        double iono_process_noise_std_;
        double receiver_antenna_azimuth_deg_;
        double receiver_antenna_elevation_deg_;
    };

} // namespace GnssProcessor

//#endif // GNSSCONFIG_HPP