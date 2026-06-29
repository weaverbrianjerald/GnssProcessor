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
#include <map>
#include <set>
#include <string>
#include <vector>
#include "logging/LogManager.hpp"
#include "config/GnssConfig.hpp"
#include "common/PppTypes.hpp"
#include "filter/StateManager.hpp"
#include "measurement/MeasurementModel.hpp" // for EpochMeasurement
#include "math/WlsEstimator.hpp"     // for WlsResult
#include "rinex/RinexParser.hpp"     // for RinexObservation (sat-status logger)

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
     * @brief Flush all open trace streams to disk.
     *
     * The per-record writers use '\n' without an implicit flush, so buffered
     * tail records may not be on disk until the logger is destroyed.  Call this
     * when another process must read complete traces during the logger's
     * lifetime (e.g. the end-of-run auto report).
     */
    void flush() const;

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
     * @brief Write receiver code-bias (DCB / per-band IFB) state estimates to the
     *        code-bias trace file (long format: week,tow_s,key,value_m,std_m).
     *
     * One row per receiver code-bias state held by the @c StateManager — the
     * per-system DCB and per-band IFB columns allocated under @c estimate_ifb in
     * uncombined PPP-RTK (and the per-FCN-slot GLONASS IFB).  Lets an analyst see
     * each receiver hardware-delay state converge; complements the satellite-side
     * @c sat_bias_trace.csv.
     *
     * @param time Epoch time for the snapshot.
     * @param state_manager StateManager that owns the code-bias state values.
     */
    void logReceiverCodeBiases(const GnssTime& time, const StateManager& state_manager) const;

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
     * @brief Write per-(satellite, signal) hardware code-bias state estimates.
     *
     * One row per sat-bias state (key "<sat>|<signal>"):
     * value, sigma, system, last-known elevation (when available from the
     * per-sat iono elevation map -- PPP-RTK only), and BSSD pivot flag.
     * PPP runs that estimate sat biases without iono states write NaN for
     * elevation.  Pivot satellites' bias states are frozen at value=0,
     * variance=0 by the BSSD S-transform; the @c is_pivot column makes
     * those frozen rows distinguishable from a genuine zero estimate.
     *
     * @param time Epoch time for the snapshot.
     * @param state_manager StateManager owning the @c sat_bias_indices_ map.
     */
    void logSatBiases(const GnssTime& time, const StateManager& state_manager) const;

    /**
     * @brief Write a per-satellite epoch-status row for every tracked sat.
     *
     * NRCAN PPP-style satellite track summary: one row per satellite that
     * appeared in @p epoch_obs (the raw RINEX observations).  The
     * @c status_code column collapses each sat's epoch outcome into a
     * single integer for direct rendering as a strip chart:
     *
     *  | code | label        | meaning                                       |
     *  |------|--------------|-----------------------------------------------|
     *  |  0   | Clean        | Used in update, no flags                      |
     *  |  1   | Pivot        | BSSD pivot (frozen at value/var = 0)          |
     *  |  2   | Slip         | Cycle slip detected (MW or KF residual reset) |
     *  |  3   | CodeOutlier  | At least one code signal excluded             |
     *  |  4   | BelowMask    | Elevation below the configured mask           |
     *  |  6   | NoProduct    | Missing orbit / clock / ephemeris             |
     *  |  7   | NoModel      | Signal selection / correction failure         |
     *
     * Status precedence on collision (most actionable wins): Slip >
     * CodeOutlier > Pivot > Clean > (drop_reason).  Code 5 is reserved
     * (formerly NotUsed) and no longer emitted -- the three-way
     * drop_reasons split makes the catch-all obsolete.
     *
     * "BelowMask" is now an honest label: only sats with elevation
     * actually below the configured mask receive it.  Sats above the
     * mask but dropped for other reasons split cleanly into
     * NoProduct (missing product data) and NoModel (no usable signal
     * pair after BIA / ATX filtering, unknown system, configured
     * exclusion, etc.).  BelowMask / NoProduct / NoModel rows all
     * carry elevation_deg = NaN because elevation isn't preserved on
     * the geometry-drop path.  Sats absent from @p epoch_obs entirely
     * produce no row at all (= "not tracked", rendered transparent).
     *
     * @param time              Epoch time.
     * @param epoch_obs         Raw RINEX observations for the epoch.  Sats
     *                          here but not in @p modeled_obs were dropped
     *                          upstream; @p drop_reasons distinguishes why.
     * @param modeled_obs       Geometry-resolved observations for this epoch.
     * @param drop_reasons      Per-satellite drop reason produced by
     *                          @c PppModel::computeModels.  Used to split
     *                          the previously-blanket "BelowMask" bucket
     *                          into BelowMask / NoProduct / NoModel.
     * @param applied_meas      The measurement set the filter actually applied.
     * @param have_applied      True if a Kalman update ran this epoch.
     * @param state_manager     For BSSD pivot lookup.
     * @param slipped_sats      Sats with cycle slips detected this epoch.
     * @param reset_sats        Sats whose ambiguities were reset by the KF.
     * @param elevation_mask_deg Configured elevation mask in degrees (logged
     *                          on BelowMask rows in a future schema bump;
     *                          unused for now but accepted for symmetry).
     */
    void logSatelliteStatus(const GnssTime& time,
                            const std::vector<RinexObservation>& epoch_obs,
                            const std::vector<PppObservation>& modeled_obs,
                            const std::map<std::string, DropReason>& drop_reasons,
                            const EpochMeasurement& applied_meas,
                            bool have_applied,
                            const StateManager& state_manager,
                            const std::set<std::string>& slipped_sats,
                            const std::set<std::string>& reset_sats,
                            double elevation_mask_deg) const;

    /**
     * @brief Write per-epoch satellite counts and DOPs (PDOP/HDOP/VDOP/
     *        TDOP/GDOP, ADOP).
     *
     * One row per epoch.  Per-system counts cover the sats that actually
     * contributed to the update (those in @c applied_meas.satellite_ids
     * plus BSSD pivots, which contribute via the S-transform even though
     * they don't appear as observation rows).  Position DOPs are built
     * from those used sats' geometry (single-system H with one clock
     * column).  ADOP is the Teunissen ambiguity DOP computed from the
     * ambiguity-subblock of the filter covariance, in meters, with
     * frozen pivot ambiguities excluded -- their variance is zero by
     * BSSD construction and would collapse the determinant.
     *
     * @param time         Epoch time.
     * @param modeled_obs  Geometry-resolved observations (provides per-sat
     *                     elevation / azimuth).
     * @param applied_meas Accepted measurement set (drives "used" set).
     * @param have_applied True if an update was applied.
     * @param state_manager Source of ambiguity covariance + pivot set.
     */
    void logDops(const GnssTime& time,
                 const std::vector<PppObservation>& modeled_obs,
                 const EpochMeasurement& applied_meas,
                 bool have_applied,
                 const StateManager& state_manager) const;

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
    mutable std::ofstream sat_bias_trace_;   /**< Output stream for per-(sat,signal) hardware bias trace file. */
    mutable std::ofstream code_bias_trace_;  /**< Output stream for receiver code-bias (DCB/IFB) trace file. */
    mutable std::ofstream sat_status_trace_; /**< Output stream for per-satellite epoch status trace file. */
    mutable std::ofstream dop_trace_;        /**< Output stream for per-epoch DOP + sat-count trace file. */
    mutable std::ofstream wls_trace_;       /**< Output stream for WLS trace file. */
    mutable bool state_header_written_{false}; /**< True after dynamic state header columns have been written. */
};

} // namespace GnssProcessor