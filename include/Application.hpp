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
#include "WlsEstimator.hpp"
#include "MwSlipDetector.hpp"
#include "IfPhaseSlipDetector.hpp"
#include "TraceLogger.hpp"
#include "PppTypes.hpp"
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @class Application
 * @brief High-level program driver for GNSSProcessor.
 *
 * Constructs the runtime application with an optional path to a configuration
 * file and exposes a single `run()` method that executes the program.
 *
 * Internally, `run()` delegates to a sequence of private helpers that each
 * handle one logical phase of the pipeline:
 *  - Logging and configuration loading.
 *  - Input data parsing (RINEX, products).
 *  - Filter state initialization (position seeding).
 *  - Per-epoch processing (observation collection, WLS seeding, cycle slip
 *    detection, Kalman predict/update, geometry relinearization, trace logging).
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
    // -----------------------------------------------------------------------
    // Pipeline phase helpers (called from run())
    // -----------------------------------------------------------------------

    /**
     * @brief Initialize logging subsystem and load JSON configuration.
     * @return true on success, false on failure.
     */
    bool initLoggingAndConfig();

    /**
     * @brief Parse RINEX observations and load external products into the cache.
     * @return true on success, false on failure.
     */
    bool loadInputData();

    /**
     * @brief Seed the filter state vector with an initial receiver position.
     *
     * Uses the RINEX header approximate position (plus antenna offsets) when
     * available, otherwise falls back to a default Earth-surface position.
     *
     * @return true on success, false on unrecoverable error.
     */
    bool initializePosition();

    /**
     * @brief Collect all observations belonging to the current epoch and
     *        advance the epoch index past them.
     *
     * Observations whose constellation is not in the active-systems list are
     * silently dropped.
     *
     * @param observations Full sorted observation vector from the RINEX parser.
     * @param[in,out] epoch_idx Current index into @p observations; advanced on return.
     * @param[out] epoch_obs Observations for the current epoch (output).
     * @param[out] current_time Epoch time extracted from the first observation.
     * @return true if at least one observation was collected, false if the
     *         epoch is empty after constellation filtering.
     */
    bool collectEpochObservations(const std::vector<RinexObservation>& observations,
                                  size_t& epoch_idx,
                                  std::vector<RinexObservation>& epoch_obs,
                                  GnssTime& current_time);

    /**
     * @brief Run WLS estimation and optionally seed the Kalman filter clock.
     *
     * Seeds when a clock jump, data gap, or first epoch is detected.
     *
     * @param current_time  Epoch time.
     * @param epoch_prefix  Log prefix string (e.g. "[Week 2345, TOW 123.00] ").
     * @param modeled_obs   Modeled observations for this epoch.
     * @param gap_detected  True if a data gap was detected before this epoch.
     */
    void handleWlsSeeding(const GnssTime& current_time,
                          const std::string& epoch_prefix,
                          const std::vector<PppObservation>& modeled_obs,
                          bool gap_detected);

    /**
     * @brief Detect MW and IF phase cycle slips and reset affected ambiguities.
     *
     * @param epoch_prefix  Log prefix string.
     * @param modeled_obs   Modeled observations for this epoch.
     */
    void detectCycleSlips(const std::string& epoch_prefix,
                          const std::vector<PppObservation>& modeled_obs);

    /**
     * @brief Run the iterative Kalman filter update / relinearization loop.
     *
     * @param epoch_prefix      Log prefix string.
     * @param modeled_obs       Modeled observations for this epoch.
     * @param[out] applied_meas The measurement set that was accepted (if any).
     * @param[out] have_applied True if at least one update was applied.
     */
    void runFilterUpdateLoop(const std::string& epoch_prefix,
                             const std::vector<PppObservation>& modeled_obs,
                             EpochMeasurement& applied_meas,
                             bool& have_applied);

    /**
     * @brief Optionally re-run geometry modelling at the updated position and
     *        perform one additional KF measurement update.
     *
     * @param epoch_prefix      Log prefix string.
     * @param epoch_obs         Raw RINEX observations for the epoch.
     * @param[in,out] applied_meas The accepted measurement set (updated if relinearization succeeds).
     * @param have_applied      Whether an update was applied in the main loop.
     */
    void runGeometryRelinearization(const std::string& epoch_prefix,
                                    const std::vector<RinexObservation>& epoch_obs,
                                    EpochMeasurement& applied_meas,
                                    bool have_applied);

    /**
     * @brief Update the stored 3D position sigma from the posterior covariance.
     */
    void updatePositionSigma();

    /**
     * @brief Log the epoch summary (position, clock, tropo) to the main log.
     * @param epoch_prefix Log prefix string.
     */
    void logEpochSummary(const std::string& epoch_prefix);

    /**
     * @brief Write per-epoch CSV trace records (state, ambiguities, residuals).
     *
     * @param current_time  GPS epoch time (converted to UTC internally).
     * @param applied_meas  Accepted measurement set (residuals are logged from this).
     * @param have_applied  Whether an update was applied.
     */
    void writeEpochTraces(const GnssTime& current_time,
                          const EpochMeasurement& applied_meas,
                          bool have_applied);

    /**
     * @brief Convert a GPS-scale GnssTime to a UTC-scale GnssTime.
     * @param gps_time GPS epoch (week + tow).
     * @return GnssTime on the UTC time scale.
     */
    static GnssTime gpsToUtcTime(const GnssTime& gps_time);

    // -----------------------------------------------------------------------
    // Data members
    // -----------------------------------------------------------------------
    std::filesystem::path config_path_;       /**< Path to the configuration file. */
    std::filesystem::path log_dir_{"logs"};   /**< Directory where log files are written. */

    LogManager log_manager_;                  /**< Application-wide logging manager. */
    GnssConfig config_;                       /**< Loaded JSON configuration. */

    std::unique_ptr<ProductParser>       product_parser_;
    std::unique_ptr<RinexParser>         rinex_parser_;
    std::unique_ptr<StateManager>        state_manager_;
    std::unique_ptr<PppModel>            ppp_model_;
    std::unique_ptr<MeasurementModel>    meas_model_;
    std::unique_ptr<KalmanFilter>        kalman_filter_;
    std::unique_ptr<WlsEstimator>        wls_estimator_;
    std::unique_ptr<MwSlipDetector>      mw_slip_detector_;
    std::unique_ptr<IfPhaseSlipDetector> if_slip_detector_;
    std::unique_ptr<TraceLogger>         trace_logger_;

    PppState ppp_state_;                      /**< Current PPP state (synced with filter). */
    Eigen::Vector3d initial_position_;        /**< Position used for sanity-check reversion. */

    bool first_epoch_{true};                  /**< True until first WLS seeding completes. */
    double prev_tow_{0.0};                    /**< Previous epoch TOW for dt computation. */
    double prev_pos_sigma_3d_{std::numeric_limits<double>::max()}; /**< Post-update 3D position sigma. */
};

} // namespace GnssProcessor