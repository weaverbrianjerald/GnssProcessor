#pragma once
#ifndef GNSSCONFIG_HPP
#define GNSSCONFIG_HPP

#include "LogManager.hpp"
//#include "RinexParser.hpp" // TODO: Access System
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <set>

namespace GnssProcessor {

    enum class ProcessingMode { PPP, PPP_RTK };

    enum class SignalMode { Dual, Triple, All };

    struct ReceiverAntennaOrientation {
        double azimuth_deg{ 0.0 };   // Antenna azimuth orientation (degrees)
        double elevation_deg{ 90.0 }; // Antenna elevation orientation (degrees)
    };

    struct ProcessNoise {
        double position{ 0.0 };         // Position process noise standard deviation (m*m/s)
        double clock{ 5.0 };            // Clock bias process noise standard deviation (m*m/s)
        double troposphere{ 0.00005 };  // Tropospheric zenith delay process noise standard deviation (m*m/s)
        double ionosphere{ 0.02 };      // Ionospheric zenith delay process noise standard deviation (m*m/s)
        double ambiguity{ 0.0 };        // Carrier phase ambiguity process noise standard deviation (m*m/s)
    };

    struct InitialStd {
        double position{ 100.0 };       // Initial position standard deviation (m)
        double clock{ 100.0 };          // Initial clock bias standard deviation (m)
        double troposphere{ 0.5 };      // Initial tropospheric zenith delay standard deviation (m)
        double ionosphere{ 5.0 };       // Initial ionospheric zenith delay standard deviation (m)
        double ambiguity{ 300.0 };      // Initial ambiguity standard deviation (m)
    };

    struct DefaultMeasurementNoise {
        // Fallback measurement noise (standard deviations) used when RINEX does not provide variances.
        double code_std_m{ 1.0 };   // meters
        double phase_std_m{ 0.01 }; // meters (1 cm)
    };

    class GnssConfig {
    public:
        GnssConfig() = default;

        // Loads configuration from JSON file
        bool loadFromJson(const std::filesystem::path& path, LogManager& log_manager);

        double getZhdPressure() const { return zhd_pressure_mbar; }
        double getZhdTemp() const { return zhd_temp_kelvin; }

        // Getters
        ProcessingMode getProcessingMode() const { return processing_mode_; }
        SignalMode getSignalMode() const { return signal_mode_; }
        bool isUncombinedEnabled() const { return uncombined_enabled_; }
        double getElevationMaskDeg() const { return elevation_mask_deg_; }
        const ReceiverAntennaOrientation& getReceiverAntennaOrientation() const { return receiver_antenna_orientation_; }
        const ProcessNoise& getProcessNoise() const { return process_noise_; }
        const InitialStd& getInitialStd() const { return initial_std_; }
        const DefaultMeasurementNoise& getDefaultMeasurementNoise() const { return default_meas_noise_; }
        const std::map<std::string, std::vector<std::string>>& getSignalPriorities() const { return signal_priorities_; }
        std::filesystem::path getProductListPath() const { return product_list_path_; }
        std::vector<std::filesystem::path> getProductPaths() const { return product_paths_; }
        std::filesystem::path getProductPath(const std::string& type) const;
        std::filesystem::path getObservationPath() const { return observation_path_; }
        std::filesystem::path getNavigationPath() const { return navigation_path_; }
        const std::vector<std::string>& getActiveSystems() const { return active_systems_; }

        double getResidualThreshold() const { return residual_threshold_; }
        double getBadMeasPercent() const { return bad_meas_percent_; }
        double getAmbiguityStd() const { return ambiguity_std_; }
        double getCodeNoiseAmplify() const { return code_noise_amplify_; }
        double getLeapSeconds() const { return leap_seconds_; }

    private:
        ProcessingMode processing_mode_{ ProcessingMode::PPP }; // Processing mode (PPP or PPP-RTK)
        SignalMode signal_mode_{ SignalMode::Dual };            // Signal mode (Dual, Triple, All)
        bool uncombined_enabled_{ false };                      // Enable uncombined processing
        double elevation_mask_deg_{ 10.0 };                    // Elevation mask (degrees)
        ReceiverAntennaOrientation receiver_antenna_orientation_; // Antenna orientation
        ProcessNoise process_noise_;                          // Process noise parameters
        InitialStd initial_std_;                              // Initial state uncertainties
        DefaultMeasurementNoise default_meas_noise_;          // Default measurement noise (fallback)
        std::map<std::string, std::vector<std::string>> signal_priorities_; // Signal priorities per system
        std::vector<std::string> active_systems_ = { "GPS", "Galileo", "GLONASS", "BeiDou", "QZSS", "SBAS" };
        std::filesystem::path product_list_path_;            // Path to product list file
        std::vector<std::filesystem::path> product_paths_;   // Product file paths (SP3, CLK, etc.)
        std::filesystem::path observation_path_;             // Path to observation RINEX file
        std::filesystem::path navigation_path_;              // Path to navigation RINEX file

        double zhd_pressure_mbar = 1013.25;  // Sea-level default; override via JSON/RINEX
        double zhd_temp_kelvin = 288.15;

        double leap_seconds_ = 37.0;  // Default: Oct 20, 2025 (no change since 2016)

        double residual_threshold_{ 6.0 };
        double bad_meas_percent_{ 0.5 };
        double ambiguity_std_{ 1000.0 };
        double code_noise_amplify_{ 1000.0 };
    };

} // namespace GnssProcessor

#endif // GNSSCONFIG_HPP
