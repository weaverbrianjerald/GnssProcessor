#pragma once
/**
 * @file Application.hpp
 * @brief Top-level application entry wrapper for the GNSS processing executable.
 *
 * The Application class encapsulates high-level startup and run logic for the
 * GNSS processing pipeline. It loads configuration, initializes core managers
 * and orchestrates parsing and processing of input products.
 */

#include "GnssConfig.hpp"
#include "LogManager.hpp"
#include "ProductParser.hpp"
#include "RinexParser.hpp"
#include "StateManager.hpp"
#include "PppModel.hpp"
#include "MeasurementModel.hpp"
#include "KalmanFilter.hpp"
#include "PppTypes.hpp"
#include <filesystem>
#include <string>

namespace GnssProcessor {

/**
 * @class Application
 * @brief High-level program driver for GNSSProcessor.
 *
 * Constructs the runtime application with an optional path to a configuration
 * file and exposes a single `run()` method that executes the program. Concrete
 * initialization and processing steps are implemented in the corresponding
 * source file.
 */
class Application {
public:
    /**
     * @brief Construct the application.
     *
     * The constructor accepts a path to a JSON configuration file. If no path
     * is provided a reasonable default relative path is used.
     *
     * @param config_path Path to the GNSS configuration file (default:
     * "config/gnss_config.json").
     */
    explicit Application(const std::filesystem::path& config_path = "config/gnss_config.json");

    /**
     * @brief Run the application.
     *
     * This function performs startup (configuration parsing, logger setup,
     * resource initialization) and then executes the processing pipeline. It
     * returns an integer exit code where `0` indicates success and non-zero
     * values indicate failure conditions.
     *
     * @return int Exit code (0 = success).
     */
    int run();

private:
    std::filesystem::path config_path_; /**< Path to the configuration file used at startup. */
};

} // namespace GnssProcessor