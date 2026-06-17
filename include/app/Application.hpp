#pragma once
/**
 * @file Application.hpp
 * @brief Top-level application entry wrapper for the GNSS processing executable.
 *
 * The Application class encapsulates high-level startup and run logic for the
 * GNSS processing pipeline. It loads configuration, initializes core managers
 * and orchestrates parsing and processing of input products.
 */

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "products/ProductParser.hpp"
#include "products/ProductResolver.hpp"
#include "rinex/RinexParser.hpp"
#include "filter/StateManager.hpp"
#include "model/PppModel.hpp"
#include "measurement/MeasurementModel.hpp"
#include "atmosphere/GlobalIonosphereMap.hpp"
#include "filter/KalmanFilter.hpp"
#include "math/WlsEstimator.hpp"
#include "slip/CycleSlipManager.hpp"
#include "slip/IonoVariabilityMonitor.hpp"
#include "logging/TraceLogger.hpp"
#include "filter/ImuIntegrator.hpp"
#include "aiding/AidingSourceManager.hpp"
#include "realtime/RealTimeEngine.hpp"
#include "filter/AmbiguityResolver.hpp"
#include "common/PppTypes.hpp"
#include "filter/Smoother.hpp"
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
     * @brief Resolve (and optionally download) product and RINEX data files.
     *
     * When data_download is enabled in the config, this phase downloads
     * missing product files from CDDIS and updates GnssConfig paths.
     * When disabled, this is a no-op.
     *
     * @return true on success, false on failure.
     */
    bool resolveData();

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
     * @return The WLS result (state, covariance, metrics).  Empty when
     *         WLS fails — caller checks @c WlsResult::success.  The
     *         result is also consumed by @c handleGap when a gap is
     *         flagged (kinematic-no-IMU reseeds position from it).
     */
    WlsResult handleWlsSeeding(const GnssTime& current_time,
                               const std::string& epoch_prefix,
                               const std::vector<PppObservation>& modeled_obs,
                               bool gap_detected);

    /**
     * @brief Dynamics-mode classification used by @c handleGap.
     *
     * Inferred from existing config + state flags (Option B): no
     * standalone "is_kinematic" toggle.  See @c classifyDynamicsMode
     * for the threshold logic.
     */
    enum class DynamicsMode {
        Static,           ///< Position pinned: no velocity state and pn.position below kinematic threshold.
        KinematicIMU,     ///< Velocity state present AND IMU integration enabled (IMU bridges the gap).
        KinematicNoImu    ///< Velocity state present OR pn.position above kinematic threshold; no IMU bridging.
    };

    /**
     * @brief Classify the current dynamics mode from config + state.
     *
     * Rules (Option B inference):
     *   - hasVelocity() && enableImu()       → KinematicIMU
     *   - hasVelocity() && !enableImu()      → KinematicNoImu
     *   - !hasVelocity() && pn.position >= 1e-6  → KinematicNoImu (quasi-kinematic)
     *   - !hasVelocity() && pn.position <  1e-6  → Static
     *
     * The 1e-6 m²/s threshold separates a numeric-stability epsilon
     * (e.g. 1e-12 m²/s, producing ~0.7 mm σ over a 7-min gap) from
     * any spectral density that represents real motion.
     */
    DynamicsMode classifyDynamicsMode() const;

    /**
     * @brief Apply the gap-recovery policy to filter state.
     *
     * Runs after @c handleWlsSeeding (which has already jolted the
     * clock value from WLS) and before the cycle-slip / measurement
     * update phase.  Per-mode actions:
     *
     *   - Common to all modes on @c gap_detected: zero clock cross-
     *     covariances to per-satellite states (the WLS-clock jolt
     *     would otherwise leak along stale correlations).
     *
     *   - Static-short (gap_detected && !long_gap):
     *       zero P(pos, sat-specific) cross-covariances.
     *
     *   - Static-long (long_gap):
     *       all of the above + mild position σ inflation, drop all
     *       iono/ambiguity/sat-bias states, drop BSSD pivots.
     *
     *   - KinematicNoImu (gap_detected):
     *       cold-start position to WLS solution with loose σ,
     *       reset velocity to zero with loose σ,
     *       drop all sat-specific states + pivots + sat-bias cache.
     *
     *   - KinematicIMU (gap_detected):
     *       no-op (IMU integration bridges; the predict step has
     *       already propagated position/velocity with realistic Q).
     *
     * @param dt            Inter-epoch gap in seconds.
     * @param long_gap      True when @c dt exceeds the long-gap threshold.
     * @param epoch_prefix  Log prefix.
     * @param wls           WLS result from @c handleWlsSeeding.  Used
     *                      only in KinematicNoImu mode to reseed
     *                      position; ignored otherwise.
     */
    void handleGap(double dt,
                   bool long_gap,
                   const std::string& epoch_prefix,
                   const WlsResult& wls);

    /**
     * @brief Detect MW and IF phase cycle slips and reset affected ambiguities.
     *
     * Delegates to CycleSlipManager::detectAndRepair().
     *
     * @param epoch_prefix  Log prefix string.
     * @param modeled_obs   Modeled observations for this epoch.
     * @return The epoch's slip result (repair / reset identities included), so
     *         the caller can forward them to the RTS smoother snapshot.
     */
    CycleSlipEpochResult detectCycleSlips(const std::string& epoch_prefix,
                                          const std::vector<PppObservation>& modeled_obs);

    /**
     * @brief Run the iterative Kalman filter update / relinearization loop.
     *
     * @param epoch_prefix      Log prefix string.
     * @param modeled_obs       Modeled observations for this epoch.
     * @param[out] applied_meas The measurement set that was accepted (if any).
     * @param[out] have_applied True if at least one update was applied.
     * @param[out] reset_satellites Satellite IDs whose ambiguities were reset
     *                              across all iterations of the loop
     *                              (accumulated for smoother bookkeeping).
     * @param[out] was_complete_reset True if any iteration performed a
     *                                complete filter reset.
     */
    void runFilterUpdateLoop(const std::string& epoch_prefix,
                             const std::vector<PppObservation>& modeled_obs,
                             EpochMeasurement& applied_meas,
                             bool& have_applied,
                             std::set<std::string>& reset_satellites,
                             bool& was_complete_reset);

    /**
     * @brief Optionally re-run geometry modelling at the updated position and
     *        refine the estimate with an iterated-EKF (IEKF) pass.
     *
     * Gated by @c config_.relinearizeGeometry(), the |Δpos| thresholds, and the
     * per-run cap.  On the non-tightly-coupled path this iterates
     * computeModels → @c KalmanFilter::updateFromPrior, anchoring every
     * Gauss–Newton step on the epoch prior (x⁻, P⁻) captured at the top of
     * runFilterUpdateLoop and holding the measurement set fixed to the one the
     * primary update converged on; the posterior covariance is therefore
     * (I−KH)·P⁻ committed once, with no double-counting.  Tightly-coupled IMU
     * mode delegates to @ref runGeometryRelinearizationNaive (see that method
     * and @c KalmanFilter::updateFromPrior for why the IEKF is not applied
     * there).
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
     * @brief Legacy single-pass geometry relinearization (one extra full
     *        @c KalmanFilter::update on top of the primary posterior).
     *
     * Retained only for the tightly-coupled IMU path, where re-deriving the
     * posterior from the epoch prior would conflict with the error-state
     * attitude/bias nominal feedback @ref KalmanFilter::update already applied.
     * Statistically this re-applies the same measurement information against an
     * already-tightened covariance (mild over-confidence).  Gates (|Δpos|,
     * per-run cap) are evaluated by @ref runGeometryRelinearization before this
     * is called.
     *
     * @param epoch_prefix      Log prefix string.
     * @param epoch_obs         Raw RINEX observations for the epoch.
     * @param[in,out] applied_meas The accepted measurement set (updated if the pass succeeds).
     */
    void runGeometryRelinearizationNaive(const std::string& epoch_prefix,
                                         const std::vector<RinexObservation>& epoch_obs,
                                         EpochMeasurement& applied_meas);

    /**
     * @brief Apply the GIM ionosphere-weighting pseudo-observation update.
     *
     * Post-processing only.  When ionosphere weighting is enabled and an
     * IONEX GIM is loaded, constrains the per-satellite slant-iono states
     * toward the GIM's between-satellite structure (the per-constellation
     * common mode is left free — see IonosphereWeightingConfig).  Called
     * after the measurement-update loop and before the smoother posterior
     * capture / AR.  No-op when disabled or no GIM is available.
     *
     * @param current_time Epoch time (GPS scale).
     * @param modeled_obs  Modeled observations for this epoch.
     */
    void applyGimIonoUpdate(const GnssTime& current_time,
                            const std::vector<PppObservation>& modeled_obs);

    /**
     * @brief Update the stored 3D position sigma from the posterior covariance.
     */
    void updatePositionSigma();

    /**
     * @brief Capture the a-priori half of an RTS smoother snapshot.
     *
     * Called immediately after the Kalman predict step (before cycle-slip
     * detection and the measurement update).  Stores the transition matrix,
     * time step, a-priori state and a-priori covariance for the current
     * epoch.  The a-posteriori half is filled in later by
     * capturePosteriorSnapshot().
     *
     * No-op when config_.runSmoother() is false.
     *
     * @param current_time Epoch time.
     * @param dt Time step from the previous epoch (seconds).
     */
    void capturePriorSnapshot(const GnssTime& current_time, double dt);

    /**
     * @brief Complete the current epoch's RTS smoother snapshot.
     *
     * Called after the measurement update loop finishes.  Fills in the
     * a-posteriori state and covariance, the post-update dynamic-key index
     * maps, the reset-ambiguity set, and the discontinuity flag.
     *
     * No-op when config_.runSmoother() is false, or when the prior half
     * of the current snapshot was not captured (guards against callers
     * forgetting the first half).
     *
     * @param reset_ambiguities Ambiguity keys whose states were reset by the update loop.
     * @param was_complete_reset True if the filter performed a complete reset.
     * @param update_applied True if at least one measurement update was applied.
     */
    void capturePosteriorSnapshot(const std::set<std::string>& reset_ambiguities,
                                  bool was_complete_reset,
                                  bool update_applied);

    /**
     * @brief Log the epoch summary (position, clock, tropo) to the main log.
     * @param epoch_prefix Log prefix string.
     */
    void logEpochSummary(const std::string& epoch_prefix);

    /**
     * @brief Write per-epoch CSV trace records (state, ambiguities, residuals,
     *        sat-bias, satellite status).
     *
     * @param current_time      GPS epoch time (converted to UTC internally).
     * @param epoch_obs         Raw RINEX observations for this epoch.  Sats
     *                          here but absent from @p modeled_obs were
     *                          dropped by ModelGeometry (below mask /
     *                          missing ephemeris / etc.) -- the status
     *                          trace surfaces them as "BelowMask" rows.
     * @param modeled_obs       Geometry-resolved observations for this epoch
     *                          (drives the per-sat status trace's used set
     *                          + elevation/azimuth/n_signals_total).
     * @param applied_meas      Accepted measurement set (residuals are logged
     *                          from this; excluded_measurements drives the
     *                          "code outlier" status category).
     * @param have_applied      Whether an update was applied.
     * @param reset_satellites  Sats whose ambiguities were reset by the
     *                          Kalman update loop -- contributes to the
     *                          "slip" status category alongside the
     *                          CycleSlipManager's MW detector results.
     */
    void writeEpochTraces(const GnssTime& current_time,
                          const std::vector<RinexObservation>& epoch_obs,
                          const std::vector<PppObservation>& modeled_obs,
                          const std::map<std::string, DropReason>& drop_reasons,
                          const EpochMeasurement& applied_meas,
                          bool have_applied,
                          const std::set<std::string>& reset_satellites);

    /**
     * @brief Convert a GPS-scale GnssTime to a UTC-scale GnssTime.
     * @param gps_time GPS epoch (week + tow).
     * @return GnssTime on the UTC time scale.
     */
    static GnssTime gpsToUtcTime(const GnssTime& gps_time);

    /**
     * @brief Run the real-time processing pipeline.
     *
     * Connects to the configured data source (NTRIP, TCP, file replay),
     * decodes RTCM frames, and processes each epoch through a
     * RealTimeEngine instance.
     *
     * @return 0 on clean shutdown, non-zero on error.
     */
    int runRealTime();

    /**
     * @brief Run the double-differenced baseline / network processing pipeline.
     *
     * Dispatched from @c run() when @c ProcessingMode is @c BaselineSingle
     * or @c BaselineMulti.  Slice 1 scaffolds only: logs a
     * "not yet implemented" message and returns 0 cleanly.  Subsequent
     * slices fill in multi-station RINEX ingestion, DD measurement model,
     * DD state vector, integer ambiguity resolution, and multi-rover
     * orchestration.
     *
     * @return 0 on clean shutdown, non-zero on error.
     */
    int runBaseline();

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
    std::unique_ptr<CycleSlipManager>     cycle_slip_manager_;
    std::unique_ptr<IonoVariabilityMonitor> iono_variability_monitor_;
    std::unique_ptr<TraceLogger>         trace_logger_;
    std::unique_ptr<AidingSourceManager> aiding_manager_;   //!< Owns the config-selected aiding sources (IMU first client). Replaces the bespoke ImuIntegrator member.
    std::unique_ptr<AmbiguityResolver>   ambiguity_resolver_;
    std::unique_ptr<GlobalIonosphereMap> gim_;  /**< IONEX global ionosphere map for PPP-RTK ionosphere weighting (post-processing). */

    PppState ppp_state_;                      /**< Current PPP state (synced with filter).  In baseline mode this is the rover state. */
    Eigen::Vector3d initial_position_;        /**< Position used for sanity-check reversion. */

    // Baseline-mode reference-station companions.  In baseline mode we run
    // PppModel twice per epoch (rover + reference) so per-receiver errors
    // (satellite clock, corrections, geometry) are captured the same way
    // as PPP.  DD is then formed by subtracting per-receiver residuals
    // rather than recomputing geometry from scratch.
    std::unique_ptr<StateManager> ref_state_manager_; /**< Baseline reference station state (position-only, fixed). */
    std::unique_ptr<PppModel>     ref_ppp_model_;     /**< PppModel instance bound to the reference station. */
    PppState                      ref_ppp_state_;     /**< Reference-station PppState (fixed position, zero clock/tropo). */

    bool first_epoch_{true};                  /**< True until the first processed epoch completes; gates the initial-dt assumption, decimation skip, IMU-aided predict, and WLS clock seeding. */
    double prev_tow_{0.0};                    /**< Previous epoch TOW for dt computation. */
    double last_processed_tow_{std::numeric_limits<double>::lowest()}; /**< TOW of the most recently PROCESSED epoch; lags prev_tow_ when downsampling via getGnssRate(). */
    double max_duration_anchor_s_{0.0};       /**< Continuous GPS seconds of the first processed epoch; the processing.max_duration_s cutoff measures elapsed data time from here. Only meaningful once first_epoch_ has cleared. */
    GnssTime prev_epoch_time_{0, 0.0};        /**< Previous epoch full GnssTime for IMU integration. */
    double prev_pos_sigma_3d_{std::numeric_limits<double>::max()}; /**< Post-update 3D position sigma. */

    /// Pre-update position captured at the top of each runFilterUpdateLoop;
    /// used by runGeometryRelinearization for its |Δpos| gate on BOTH the IEKF
    /// and the legacy naive path.  The IEKF's full-state anchor (x⁻, P⁻) comes
    /// instead from KalmanFilter::getLastUpdatePriorX/P, which is dimension-
    /// matched to the post-augmentation state (position alone never changes
    /// dimension, so it is safe to snapshot up front here).
    Eigen::Vector3d relinearize_pre_update_pos_{ Eigen::Vector3d::Zero() };
    /// Total relinearization passes applied this run; capped by
    /// config_.relinearizeMaxCount() when that value is non-negative.
    int relinearize_count_{ 0 };

    std::vector<SmootherEpochSnapshot> smoother_buffer_; /**< Per-epoch snapshots captured for the backward RTS smoother (populated only when config_.runSmoother() is true). */
};

} // namespace GnssProcessor