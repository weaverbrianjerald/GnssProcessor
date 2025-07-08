#pragma once
#ifndef GNSSCONFIG_HPP
#define GNSSCONFIG_HPP

#include "LogManager.hpp"
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace GnssProcessor {

    enum class ProcessingMode { PPP, PPP_RTK };

    struct ReceiverAntennaOrientation {
        double azimuth_deg{ 0.0 };   // Antenna azimuth orientation (degrees)
        double elevation_deg{ 90.0 }; // Antenna elevation orientation (degrees)
    };

    struct ProcessNoise {
        double position_std{ 0.1 };     // Position process noise standard deviation (m)
        double clock_bias_std{ 100.0 }; // Clock bias process noise standard deviation (m)
        double troposphere_std{ 0.01 }; // Tropospheric zenith delay process noise standard deviation (m)
        double ionosphere_std{ 0.02 };  // Ionospheric zenith delay process noise standard deviation (m)
    };

    struct InitialStd {
        double position{ 100.0 };    // Initial position standard deviation (m)
        double clock{ 1000.0 };      // Initial clock bias standard deviation (m)
        double troposphere{ 0.5 };   // Initial tropospheric zenith delay standard deviation (m)
        double ionosphere{ 5.0 };    // Initial ionospheric zenith delay standard deviation (m)
        double ambiguity{ 0.1 };     // Initial ambiguity standard deviation (cycles)
    };

    class GnssConfig {
    public:
        GnssConfig() = default;

        // Loads configuration from JSON file
        bool loadFromJson(const std::filesystem::path& path, LogManager& log_manager);

        // Getters
        ProcessingMode getProcessingMode() const { return processing_mode_; }
        bool isUncombinedEnabled() const { return uncombined_enabled_; }
        double getElevationMaskDeg() const { return elevation_mask_deg_; }
        const ReceiverAntennaOrientation& getReceiverAntennaOrientation() const { return receiver_antenna_orientation_; }
        const ProcessNoise& getProcessNoise() const { return process_noise_; }
        const InitialStd& getInitialStd() const { return initial_std_; }
        const std::map<std::string, std::vector<std::string>>& getSignalPriorities() const { return signal_priorities_; }
        std::filesystem::path getProductListPath() const { return product_list_path_; }
        std::vector<std::filesystem::path> getProductPaths() const { return product_paths_; }
        std::filesystem::path getProductPath(const std::string& type) const;
        std::filesystem::path getObservationPath() const { return observation_path_; }
        std::filesystem::path getNavigationPath() const { return navigation_path_; }

    private:
        ProcessingMode processing_mode_{ ProcessingMode::PPP }; // Processing mode (PPP or PPP-RTK)
        bool uncombined_enabled_{ false };                      // Enable uncombined processing
        double elevation_mask_deg_{ 10.0 };                    // Elevation mask (degrees)
        ReceiverAntennaOrientation receiver_antenna_orientation_; // Antenna orientation
        ProcessNoise process_noise_;                          // Process noise parameters
        InitialStd initial_std_;                              // Initial state uncertainties
        std::map<std::string, std::vector<std::string>> signal_priorities_; // Signal priorities per system
        std::filesystem::path product_list_path_;            // Path to product list file
        std::vector<std::filesystem::path> product_paths_;   // Product file paths (SP3, CLK, etc.)
        std::filesystem::path observation_path_;             // Path to observation RINEX file
        std::filesystem::path navigation_path_;              // Path to navigation RINEX file
    };

} // namespace GnssProcessor

#endif // GNSSCONFIG_HPP
