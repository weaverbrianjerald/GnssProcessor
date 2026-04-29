#pragma once
/**
 * @file TraceLogger.hpp
 * @brief Lightweight CSV/text trace logger for PPP/RTK internal diagnostics.
 *
 * The TraceLogger writes epoch-level trace files for states, ambiguities,
 * residuals and WLS diagnostics. Files are opened during construction and
 * written from const methods (streams are marked mutable to allow that).
 */

#include <filesystem>
#include <fstream>
#include <string>
#include "logging/LogManager.hpp"
#include "config/GnssConfig.hpp"
#include "common/PppTypes.hpp"
#include "filter/StateManager.hpp"
#include "measurement/MeasurementModel.hpp" // for EpochMeasurement
#include "math/WlsEstimator.hpp"     // for WlsResult

namespace GnssProcessor {

/**
 * @class TraceLogger
 * @brief Produces human-readable trace files for debugging and offline analysis.
 *
 * Trace files include:
 * - state_trace_: receiver state vector and covariance per epoch
 * - ambiguity_trace_: ambiguity estimates per epoch
 * - residual_trace_: per-observation residuals and diagnostics
 * - wls_trace_: WLS solution / seeding diagnostics used when performing ambiguity resolution
 *
 * Streams are written from logically-const methods; therefore stream members are
 * declared as mutable to allow tracing from const contexts.
 */
class TraceLogger {
public:
    /**
     * @brief Construct a TraceLogger and open trace files in the supplied directory.
     *
     * If opening files fails, the logger remains in a non-initialized state and
     * subsequent calls to logging methods will be no-ops (or will only emit logs
     * via the provided LogManager).
     *
     * @param log_manager Reference to the application's LogManager used for internal logging.
     * @param log_dir Directory where trace files will be created.
     * @param is_ppp_rtk True when running a PPP-RTK variant (affects file naming/columns).
     * @param logging_config Logging configuration controlling which trace files are opened.
     */
    TraceLogger(LogManager& log_manager, const std::filesystem::path& log_dir, bool is_ppp_rtk,
                const LoggingConfig& logging_config = LoggingConfig{});

    /**
     * @brief Check whether trace files were successfully opened.
     * @return true If the TraceLogger is ready to write traces.
     * @return false If initialization failed or files are not writable.
     */
    bool isInitialized() const;

    /**
     * @brief Write the current filter state to the state trace file.
     *
     * The method extracts relevant state vector entries and covariance from the
     * supplied StateManager and writes a single epoch line.
     *
     * @param time Epoch time of the state.
     * @param state_manager StateManager providing access to current states and metadata.
     */
    void logState(const GnssTime& time, const StateManager& state_manager) const;

    /**
     * @brief Write ambiguity estimates (carrier-phase ambiguities) to the ambiguity trace file.
     *
     * The output typically contains satellite-signal identifiers, estimated
     * ambiguity values and associated variances or fix flags.
     *
     * @param time Epoch time for the ambiguity snapshot.
     * @param state_manager StateManager that owns ambiguity state values.
     */
    void logAmbiguities(const GnssTime& time, const StateManager& state_manager) const;

    /**
     * @brief Write per-satellite ionosphere state estimates to the ionosphere trace file.
     *
     * Each row contains satellite ID, slant iono delay estimate, standard deviation
     * and last-known elevation angle  suitable for offline analysis and plotting.
     *
     * @param time Epoch time for the ionosphere snapshot.
     * @param state_manager StateManager that owns per-satellite iono states and elevations.
     */
    void logIonosphere(const GnssTime& time, const StateManager& state_manager) const;

    /**
     * @brief Write observation residuals and diagnostics for an epoch.
     *
     * Residuals, standardized residuals, per-observation variances and other
     * diagnostics are written in a tabular or CSV-like format suitable for
     * later inspection.
     *
     * @param time Epoch time associated with the measurement set.
     * @param meas EpochMeasurement containing observations, residuals and matrices.
     */
    void logResiduals(const GnssTime& time, const EpochMeasurement& meas) const;

    /**
     * @brief Record WLS estimation results and seeding/quality diagnostics.
     *
     * This trace captures both the WLS solution and the decisions that lead to
     * seeding (or rejecting) ambiguity fixes. The fields are intended to help
     * reproduce ambiguity resolution logic during debugging.
     *
     * @param time Epoch time of the WLS result.
     * @param wls Result object returned by the WLS estimator.
     * @param wls_clock_bias_m WLS-estimated receiver clock bias (meters).
     * @param kf_clock_bias_m Kalman-filter clock bias (meters).
     * @param delta_clock_m Difference between WLS and KF clock bias (meters).
     * @param jump_detected True if a cycle-slip or jump was detected for this epoch.
     * @param wls_quality_good True if the WLS solution met internal quality checks.
     * @param delta_reasonable True if the difference between WLS and KF clocks is within tolerance.
     * @param should_seed True when the WLS solution will be used to seed ambiguity states.
     */
    void logWls(const GnssTime& time,
                const WlsResult& wls,
                double wls_clock_bias_m,
                double kf_clock_bias_m,
                double delta_clock_m,
                bool jump_detected,
                bool should_seed) const;

private:
    LogManager& log_manager_;   /**< Reference to LogManager used for internal logging. */
    bool is_ppp_rtk_{false};    /**< Mode flag: true when running PPP-RTK variant. */
    bool initialized_{false};   /**< True when trace files were successfully opened. */

    /** Streams are written from const methods; mark mutable to allow logging. */
    mutable std::ofstream state_trace_;     /**< Output stream for state trace file. */
    mutable std::ofstream ambiguity_trace_; /**< Output stream for ambiguity trace file. */
    mutable std::ofstream residual_trace_;  /**< Output stream for residual trace file. */
    mutable std::ofstream ionosphere_trace_; /**< Output stream for ionosphere trace file. */
    mutable std::ofstream wls_trace_;       /**< Output stream for WLS trace file. */
    mutable bool state_header_written_{false}; /**< True after dynamic state header columns have been written. */
};

} // namespace GnssProcessor