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

    /**
     * @brief Enumeration for GNSS processing modes.
     *
     * Defines the available processing modes for the GNSS processor.
     */
    enum class ProcessingMode { 
        PPP,    ///< Precise Point Positioning mode.
        PPP_RTK ///< Precise Point Positioning with Real-Time Kinematic enhancements.
    };

    /**
     * @brief Enumeration for signal processing modes.
     *
     * Specifies the number of signals to use for measurements.
     */
    enum class SignalMode { 
        Dual,   ///< Use dual-frequency signals.
        Triple, ///< Use triple-frequency signals.
        All     ///< Use all available signals.
    };

    /**
     * @brief Enumeration for satellite velocity interpolation.
     *
     * Specifies the type of interpolation used for satellite velocity computation.
     */
    enum class SatelliteInterpolationMode {
        Lagrange,///< Lagrange analytical derivative.
        Finite,  ///< Finite central difference.
        Hermite, ///< Monotone Hermite slopes.
        Spline   ///< Cubic spline.
    };

    /**
     * @brief Enumeration for satellite clock interpolation.
     *
     * Specifies the type of interpolation used for satellite clock computation.
     */
    enum class SatelliteClockInterpolationMode {
        Linear,   ///< Linear interpolation.
        Lagrange  ///< Lagrange interpolation.
    };

    /**
     * @brief Struct representing receiver antenna orientation.
     *
     * Contains azimuth and elevation angles for antenna orientation.
     */
    struct ReceiverAntennaOrientation {
        double azimuth_deg{ 0.0 };   ///< Antenna azimuth orientation in degrees.
        double elevation_deg{ 90.0 }; ///< Antenna elevation orientation in degrees.
    };

    /**
     * @brief Struct for process noise parameters.
     *
     * Defines standard deviations for process noise in the Kalman filter.
     */
    struct ProcessNoise {
        double position{ 0.0 };             ///< Position process noise standard deviation (m²/s).
        double clock{ 1e4 };                ///< Clock bias process noise standard deviation (m²/s).
        double troposphere{ 1e-7 };         ///< Tropospheric zenith delay process noise standard deviation (m²/s).
        double ionosphere{ 0.20 };          ///< Ionospheric zenith delay process noise standard deviation (m²/s).
        double ambiguity{ 0.0 };            ///< Carrier phase ambiguity process noise standard deviation (m²/s).
        double system_time_offset{ 1e-6 };  ///< Inter-system time bias process noise standard deviation (m²/s).
    };

    /**
     * @brief Struct for initial state standard deviations.
     *
     * Specifies initial uncertainties for state variables.
     */
    struct InitialStd {
        double position{ 100.0 };       ///< Initial position standard deviation (m).
        double clock{ 100.0 };          ///< Initial clock bias standard deviation (m).
        double troposphere{ 0.5 };      ///< Initial tropospheric zenith delay standard deviation (m).
        double ionosphere{ 5.0 };       ///< Initial ionospheric zenith delay standard deviation (m).
        double ambiguity{ 300.0 };      ///< Initial ambiguity standard deviation (m).
        double system_time_offset{ 100.0 }; ///< Initial inter-system time bias standard deviation (m).
    };

    /**
     * @brief Struct for default measurement noise parameters.
     *
     * Provides fallback noise values when RINEX does not specify variances.
     */
    struct DefaultMeasurementNoise {
        double code_std_m{ 0.30 };   ///< Code measurement standard deviation (m).
        double phase_std_m{ 0.003 }; ///< Phase measurement standard deviation (m).
    };

    /**
     * @brief Configuration class for GNSS processing.
     *
     * This class loads and stores configuration parameters from a JSON file,
     * providing access to settings for processing modes, noise parameters,
     * file paths, and other GNSS-related options.
     */
    class GnssConfig {
    public:
        /**
         * @brief Default constructor.
         */
        GnssConfig() = default;

        /**
         * @brief Loads configuration from a JSON file.
         *
         * Parses the JSON file and sets internal configuration parameters.
         * Logs errors or warnings via the provided LogManager.
         *
         * @param path Path to the JSON configuration file.
         * @param log_manager Reference to the LogManager for logging.
         * @return True if loading succeeded, false otherwise.
         */
        bool loadFromJson(const std::filesystem::path& path, LogManager& log_manager);

        /**
         * @brief Gets the zenith hydrostatic delay pressure.
         * @return Pressure in millibars.
         */
        double getZhdPressure() const { return zhd_pressure_mbar; }

        /**
         * @brief Gets the zenith hydrostatic delay temperature.
         * @return Temperature in Kelvin.
         */
        double getZhdTemp() const { return zhd_temp_kelvin; }

        /**
         * @brief Gets the processing mode.
         * @return Current processing mode (PPP or PPP-RTK).
         */
        ProcessingMode getProcessingMode() const { return processing_mode_; }

        /**
         * @brief Gets the signal mode.
         * @return Current signal mode (Dual, Triple, or All).
         */
        SignalMode getSignalMode() const { return signal_mode_; }

        /**
         * @brief Gets the satellite position and velocity interpolation mode.
         * @return Current satellite position and velocity interpolation mode (lagrange, finite, hermite, or spline).
         */
        SatelliteInterpolationMode getSatelliteInterpolationMode() const { return satellite_interpolation_mode_; }

        /**
         * @brief Gets the satellite clock interpolation mode.
         * @return Current satellite clock interpolation mode (linear, lagrange).
         */
        SatelliteClockInterpolationMode getSatelliteClockInterpolationMode() const { return satellite_clock_interpolation_mode_; }

        /**
         * @brief Checks if uncombined processing is enabled.
         * @return True if enabled, false otherwise.
         */
        bool isUncombinedEnabled() const { return uncombined_enabled_; }

        /**
         * @brief Gets the elevation mask in degrees.
         * @return Elevation mask value.
         */
        double getElevationMaskDeg() const { return elevation_mask_deg_; }

        /**
         * @brief Gets the receiver antenna orientation.
         * @return Reference to the ReceiverAntennaOrientation struct.
         */
        const ReceiverAntennaOrientation& getReceiverAntennaOrientation() const { return receiver_antenna_orientation_; }

        /**
         * @brief Gets the process noise parameters.
         * @return Reference to the ProcessNoise struct.
         */
        const ProcessNoise& getProcessNoise() const { return process_noise_; }

        /**
         * @brief Gets the initial standard deviations.
         * @return Reference to the InitialStd struct.
         */
        const InitialStd& getInitialStd() const { return initial_std_; }

        /**
         * @brief Gets the default measurement noise parameters.
         * @return Reference to the DefaultMeasurementNoise struct.
         */
        const DefaultMeasurementNoise& getDefaultMeasurementNoise() const { return default_meas_noise_; }

        /**
         * @brief Gets the signal priorities map.
         * @return Reference to the map of signal priorities per system.
         */
        const std::map<std::string, std::vector<std::string>>& getSignalPriorities() const { return signal_priorities_; }

        /**
         * @brief Gets the product list file path.
         * @return Path to the product list file.
         */
        std::filesystem::path getProductListPath() const { return product_list_path_; }

        /**
         * @brief Gets the list of product file paths.
         * @return Vector of product file paths (e.g., SP3, CLK).
         */
        std::vector<std::filesystem::path> getProductPaths() const { return product_paths_; }

        /**
         * @brief Gets the product path for a specific type.
         * @param type Product type (e.g., "SP3", "CLK").
         * @return Path to the product file of the specified type.
         */
        std::filesystem::path getProductPath(const std::string& type) const;

        /**
         * @brief Gets the observation file path.
         * @return Path to the observation RINEX file.
         */
        std::filesystem::path getObservationPath() const { return observation_path_; }

        /**
         * @brief Gets the navigation file path.
         * @return Path to the navigation RINEX file.
         */
        std::filesystem::path getNavigationPath() const { return navigation_path_; }

        /**
         * @brief Gets the list of active GNSS systems.
         * @return Vector of active system names (e.g., "GPS", "Galileo").
         */
        const std::vector<std::string>& getActiveSystems() const { return active_systems_; }

        /**
         * @brief Gets the residual threshold for outlier detection.
         * @return Residual threshold value.
         */
        double getResidualThreshold() const { return residual_threshold_; }

        /**
         * @brief Gets the percentage of bad measurements allowed.
         * @return Bad measurement percentage.
         */
        double getBadMeasPercent() const { return bad_meas_percent_; }

        /**
         * @brief Gets the ambiguity standard deviation.
         * @return Ambiguity standard deviation.
         */
        double getAmbiguityStd() const { return ambiguity_std_; }

        /**
         * @brief Gets the code noise amplification factor.
         * @return Code noise amplification value.
         */
        double getCodeNoiseAmplify() const { return code_noise_amplify_; }

        /**
         * @brief Gets the number of leap seconds.
         * @return Leap seconds value.
         */
        double getLeapSeconds() const { return leap_seconds_tai_; }

    private:
        ProcessingMode processing_mode_{ ProcessingMode::PPP }; ///< Processing mode (PPP or PPP-RTK).
        SignalMode signal_mode_{ SignalMode::Dual };            ///< Signal mode (Dual, Triple, All).
        SatelliteInterpolationMode satellite_interpolation_mode_{ SatelliteInterpolationMode::Lagrange }; ///< Satellite PV mode (Lagrange, Finite, Hermite, Spline).
        SatelliteClockInterpolationMode satellite_clock_interpolation_mode_{ SatelliteClockInterpolationMode::Linear }; ///< Satellite T mode (Linear, Lagrange).
        bool uncombined_enabled_{ false };                      ///< Enable uncombined processing.
        double elevation_mask_deg_{ 10.0 };                    ///< Elevation mask in degrees.
        ReceiverAntennaOrientation receiver_antenna_orientation_; ///< Antenna orientation parameters.
        ProcessNoise process_noise_;                          ///< Process noise parameters.
        InitialStd initial_std_;                              ///< Initial state uncertainties.
        DefaultMeasurementNoise default_meas_noise_;          ///< Default measurement noise (fallback).
        std::map<std::string, std::vector<std::string>> signal_priorities_; ///< Signal priorities per system.
        std::vector<std::string> active_systems_ = { "GPS", "Galileo", "GLONASS", "BeiDou", "QZSS", "SBAS" }; ///< Active GNSS systems.
        std::filesystem::path product_list_path_;            ///< Path to product list file.
        std::vector<std::filesystem::path> product_paths_;   ///< Product file paths (SP3, CLK, etc.).
        std::filesystem::path observation_path_;             ///< Path to observation RINEX file.
        std::filesystem::path navigation_path_;              ///< Path to navigation RINEX file.

        double zhd_pressure_mbar = 1013.25;  ///< Zenith hydrostatic delay pressure (mbar).
        double zhd_temp_kelvin = 288.15;     ///< Zenith hydrostatic delay temperature (Kelvin).

        double leap_seconds_tai_ = 37.0;  ///< Number of leap seconds (TAI-UTC=37) since 2017-01-01.

        double residual_threshold_{ 6.0 };   ///< Residual threshold for outlier detection.
        double bad_meas_percent_{ 0.5 };     ///< Percentage of bad measurements allowed.
        double ambiguity_std_{ 1000.0 };     ///< Ambiguity standard deviation.
        double code_noise_amplify_{ 1000.0 }; ///< Code noise amplification factor.
    };

} // namespace GnssProcessor

#endif // GNSSCONFIG_HPP
