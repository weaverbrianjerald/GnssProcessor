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
#include "filter/NeqAccumulator.hpp"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

struct NetworkSetupResult;   // app/NetworkSetupResult.hpp (decomposition INC-5)
struct NetworkRunContext;    // app/NetworkRunContext.hpp (decomposition INC-4; INC-7 signatures)
struct NetworkRunTraces;     // logging/NetworkRunTraces.hpp (decomposition INC-3; INC-7 signatures)
struct SdEwlFix;             // filter/NetworkRunTypes.hpp (SD EWL integer, increment 2a)
struct NetStation;           // filter/NetworkRunTypes.hpp (decomposition INC-7)
struct StationAssembly;      // filter/NetworkRunTypes.hpp (decomposition INC-7)
enum class ParamDisposition : std::uint8_t;  // filter/NetworkParamInventory.hpp (card NOM N1)
struct RxIfbRule;            // filter/NetworkParamInventory.hpp (card NOM N1)
struct RowTag;               // filter/NetworkContributions.hpp (card NOM N2a)
enum class GimRowsOutcome : std::uint8_t;  // filter/NetworkContributions.hpp (card NOM N2a)
struct ShDevCarry;           // filter/NetworkRunTypes.hpp (decomposition INC-7)
struct SyncedEpoch;          // rinex/MultiRinexSync.hpp, namespace scope (decomposition INC-7)
class  NetworkEstimator;     // filter/NetworkEstimator.hpp (decomposition INC-7)
class  SphericalHarmonicIonosphere;  // atmosphere/SphericalHarmonicIonosphere.hpp (decomposition INC-7)
class  PppUtils;             // common/PppUtils.hpp (decomposition INC-7)
struct SlipQcChannel;        // slip/NetworkRawObservables.hpp (slip-QC hardening)
enum class EpochAction;      // filter/NetworkRunTypes.hpp, opaque enum declaration (decomposition INC-7)
struct NetworkRun;           // app/NetworkRun.hpp (card NOM3 step EP1)
struct NetworkEpoch;         // app/NetworkEpoch.hpp (card NOM3 step EP1)
struct EpochPassA;           // app/NetworkEpoch.hpp (card NOM3 step EP1)
struct NetworkSlipFunnel;    // app/NetworkEpoch.hpp (card NOM3 step EP1)
struct EpochColumns;         // app/NetworkEpoch.hpp (card NOM3 step EP1)
struct EpochAssembly;        // app/NetworkEpoch.hpp (card NOM3 step EP1)

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
     * Runs PathResolver for the configured @c data_source.  Internal mode globs
     * the bundled data tree; production mode delegates to ProductResolver, which
     * downloads missing product files from CDDIS when @c data_download.enabled is
     * set and otherwise leaves the configured paths as they are.  The resolved
     * paths are merged into GnssConfig: a non-empty resolved product set replaces
     * the configured one, and the navigation, observation and baseline RINEX
     * paths are filled only where the config left them empty.  Ends with
     * @ref refreshAtxIfEnabled_.
     *
     * @return true on success, false when PathResolver fails.
     */
    bool resolveData();

    /**
     * @brief Opt-in: refresh the IGS ANTEX (igs20.atx) before it is loaded.
     *
     * When @c data_download.atx_auto_refresh is set, shells out to
     * @c scripts/refresh_atx.py (the single source of truth for the refresh:
     * conditional GET, ANTEX validation, backup, atomic replace) against the
     * resolved ATX path, throttled by @c atx_refresh_max_age_days.  Best-effort
     * and non-fatal: a network or validation failure logs a warning and the run
     * proceeds with the existing local ATX.  No-op when the toggle is off, so
     * runs stay reproducible by default.  Called at the end of @ref resolveData,
     * so it covers every mode that resolves data (post-processing, baseline,
     * real-time, network and the consumer replay) and runs before the antenna
     * model is loaded (@ref loadInputData on the post-processing path).
     */
    void refreshAtxIfEnabled_();

    /**
     * @brief Parse RINEX observations and load external products into the cache.
     * @return true on success, false on failure.
     */
    bool loadInputData();

    /**
     * @brief Seed the filter state vector with an initial receiver position.
     *
     * Uses the RINEX header approximate position, moved from the marker to the
     * ARP by the header's H/E/N antenna offset when one is given; otherwise
     * falls back to a default Earth-surface position.  When station information
     * (SIF) is enabled and the marker resolves, the SIF coordinate propagated to
     * the first-observation epoch replaces either one.  That coordinate is a
     * monument coordinate, so it is reduced to the ARP first, by the header's
     * H/E/N offset or, when the header carries none, by the SIF antenna window's
     * ARP-minus-GRP vector (@c reduceSifCoordinateToArp).  PppModel::initializeState
     * later re-derives the position from a configured reference position or the
     * RINEX header, so run() re-applies the SIF seed after it unless a reference
     * position is configured (@c sif_apriori_seeded_).
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
     * Seeds when a clock jump (WLS and filter clocks at least 0.5 ms apart), a
     * data gap, or the first epoch is detected.  When the state has no receiver
     * clock (BSSD), WLS is not run and nothing is seeded.
     *
     * @param current_time  Epoch time.
     * @param epoch_prefix  Log prefix string (empty in the post-processing loop,
     *                      where the log pattern carries the epoch).
     * @param modeled_obs   Modeled observations for this epoch.
     * @param gap_detected  True if a data gap was detected before this epoch.
     * @return The WLS result (state, covariance, metrics); the caller checks
     *         @c WlsResult::success, which is false when WLS fails.  Empty
     *         (default-constructed) when the state has no receiver clock.  The
     *         result is also consumed by @c handleGap when a gap is flagged
     *         (kinematic-no-IMU reseeds position from it).
     */
    WlsResult handleWlsSeeding(const GnssTime& current_time,
                               const std::string& epoch_prefix,
                               const std::vector<PppObservation>& modeled_obs,
                               bool gap_detected);

    /**
     * @brief Dynamics-mode classification used by @c handleGap.
     *
     * Inferred from existing config + state flags; there is no standalone
     * "is_kinematic" toggle.  See @c classifyDynamicsMode for the threshold
     * logic.
     */
    enum class DynamicsMode {
        Static,           ///< Position pinned: no velocity state and pn.position below kinematic threshold.
        KinematicIMU,     ///< Velocity state present and IMU integration enabled (the IMU bridges the gap).
        KinematicNoImu    ///< Velocity state without IMU integration, or no velocity state and pn.position at or above kinematic threshold; no IMU bridging.
    };

    /**
     * @brief Classify the current dynamics mode from config + state.
     *
     * Rules:
     *   - hasVelocity() && enableImu()       → KinematicIMU
     *   - hasVelocity() && !enableImu()      → KinematicNoImu
     *   - !hasVelocity() && pn.position >= 1e-6  → KinematicNoImu (quasi-kinematic)
     *   - !hasVelocity() && pn.position <  1e-6  → Static
     *
     * The 1e-6 m²/s threshold separates a numeric-stability epsilon from any
     * spectral density that represents real motion.  The position-only random
     * walk adds pn.position·dt of variance per axis, so an epsilon of
     * 1e-12 m²/s grows σ by about 0.02 mm per axis over a 7-minute gap.
     */
    DynamicsMode classifyDynamicsMode() const;

    /**
     * @brief Apply the gap-recovery policy to filter state.
     *
     * Called only when a gap is detected, after @c handleWlsSeeding (which
     * has already re-seeded the clock from WLS) and before the Kalman predict
     * and the cycle-slip / measurement update phase.  Per-mode actions:
     *
     *   - Common to all modes on @c gap_detected: zero the clock cross-
     *     covariances to per-satellite states (the re-seeded clock would
     *     otherwise propagate along stale correlations).
     *
     *   - Static-short (gap_detected && !long_gap):
     *       zero P(pos, sat-specific) cross-covariances.
     *
     *   - Static-long (long_gap):
     *       all of the above + mild position σ inflation, drop all
     *       iono/ambiguity/sat-bias states and the soft-LLI bridge holds,
     *       drop the BSSD and baseline DD pivots.
     *
     *   - KinematicNoImu (gap_detected):
     *       cold-start position to the WLS solution with loose σ (when WLS
     *       failed, the position value is kept and only its σ is loosened),
     *       reset velocity to zero with loose σ,
     *       drop all sat-specific states + pivots + sat-bias cache.
     *
     *   - KinematicIMU (gap_detected):
     *       nothing beyond the common step; the IMU-aided predict that
     *       follows bridges the gap with realistic Q.
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
     * @brief Detect cycle slips and reset or repair the affected ambiguities.
     *
     * Delegates to CycleSlipManager::detectAndRepair(), which runs the MW
     * detector first and then the enabled auxiliary detectors (graphic, EWL).
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
     * Gauss–Newton step on the epoch prior (x⁻, P⁻) that the primary update
     * stashed (@c KalmanFilter::getLastUpdatePriorX/P) and holding the
     * measurement set (its outlier exclusions) fixed to the one the primary
     * update converged on; the posterior covariance is therefore (I−KH)·P⁻
     * committed once, with no double-counting.  Tightly-coupled IMU mode
     * delegates to @ref runGeometryRelinearizationNaive (see that method and
     * @c KalmanFilter::updateFromPrior for why the IEKF is not applied there).
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
     * common mode is left free; see IonosphereWeightingConfig).  Called
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
     * Called after the measurement update loop finishes (and after the
     * geometry relinearization and the GIM iono update), before AR.  Fills in
     * the a-posteriori state and covariance, the post-update dynamic-key index
     * maps, the reset-satellite set, the discontinuity flag, and the BSSD pivot
     * metadata.
     *
     * No-op when config_.runSmoother() is false, or when no snapshot has been
     * started (empty buffer).  It completes the most recent snapshot, so
     * capturePriorSnapshot() must run earlier in the same epoch.
     *
     * @param reset_ambiguities Satellite IDs whose ambiguities were reset this
     *                          epoch (update loop, relinearization and slip-phase resets).
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
     * @brief Write per-epoch CSV trace records (state, ambiguities, ionosphere,
     *        satellite and receiver biases, residuals, satellite status, DOPs).
     *
     * @param current_time      GPS epoch time.  The traces are stamped in GPS
     *                          time, matching the RINEX epochs (no UTC
     *                          conversion).
     * @param epoch_obs         Raw RINEX observations for this epoch.  Sats
     *                          here but absent from @p modeled_obs were
     *                          dropped during modeling (ModelGeometry /
     *                          PppModel); the status trace reports them with
     *                          their reason from @p drop_reasons.
     * @param modeled_obs       Geometry-resolved observations for this epoch
     *                          (drives the per-sat status trace's used set
     *                          + elevation/azimuth/n_signals_total).
     * @param drop_reasons      Per-satellite drop reason recorded during
     *                          modeling (BelowMask, NoProduct, ...); a dropped
     *                          satellite with no entry is reported as NoModel.
     * @param applied_meas      Accepted measurement set (residuals are logged
     *                          from this; excluded_measurements drives the
     *                          "CodeOutlier" status category).
     * @param have_applied      Whether an update was applied.
     * @param reset_satellites  Sats whose ambiguities were reset this epoch
     *                          (update loop, relinearization and slip-phase
     *                          resets); contributes to the "Slip" status
     *                          category alongside the CycleSlipManager's MW
     *                          detector results.
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
     * @brief Run the double-differenced baseline processing pipeline (one or
     *        several rovers against one reference station).
     *
     * Dispatched from @c run() when @c ProcessingMode is @c BaselineSingle
     * or @c BaselineMulti, before run()'s own @ref resolveData call, so it
     * calls @ref resolveData itself.  Aligns the reference and rover RINEX
     * epochs (MultiRinexSync), models the reference station each epoch with
     * its own PppModel, and processes each rover against the reference in
     * turn (a star topology for BaselineMulti): DD measurement model, DD state
     * vector and integer ambiguity resolution.  Implemented in
     * @c ApplicationBaseline.cpp.
     *
     * @return 0 on clean shutdown, non-zero on error (including a run in which
     *         no rover overlaps the reference on any synced epoch).
     */
    int runBaseline();

    /**
     * @brief Run the minimum-constraint S-system PPP-RTK network producer.
     *
     * Dispatched from @c run() when @c ProcessingMode is @c PPP_RTK_Network.
     * The network is a joint multi-station estimator (per-station local normals
     * reduced onto a shared satellite-parameter block via an exact Schur
     * complement) that estimates the estimable satellite clock, satellite
     * phase/code bias, and atmosphere under an explicitly declared S-basis, and
     * emits them in IGS-SSR convention for the companion @c PPP_RTK_User
     * consumer.  Implemented in @c ApplicationNetwork.cpp.
     *
     * Order: the consumer-only replay (@ref runConsumerReplay) when
     * @c network.closed_loop.replay_products_path is set; otherwise input
     * validation (@ref networkValidateInputs_), @ref resolveData for products
     * and navigation (a failure is logged and not fatal), setup
     * (@ref networkSetup_), then the epoch loop over the synchronized epochs.
     *
     * @return 0 on clean shutdown, non-zero on error.  A refused start (for
     *         example a refused warm resume) is thrown as an exception;
     *         @c run() catches it, logs the message and returns 1.
     */
    int runNetwork();

    /// What one epoch tells the epoch source (formerly runNetwork's local enum).
    enum class LoopSignal { Next, Stop };

    /**
     * @brief One synced epoch through the network producer (card NOM3 step EP1).
     *
     * The admission (start offset, event injector, canonical-pair screen), the NetworkEpoch,
     * then the stages in spine order: roster, admission tail (broadcast feed, maximum
     * duration, decimation), epoch top, the slip funnel, PASS A and the screens, the phase
     * datum, the columns, pre-assembly, PASS B, the DD build, the solve, the
     * install, AR and fixed cycles, solution prep, emit, the post-fit diagnostics, RAIM,
     * clock QC, the measure-only instruments, the stash, the D0 readout and the snapshot.
     * Four stages end the epoch early (networkPassAHead_, networkAssemblyRegisterColumns_,
     * networkPassB_, networkSolveGlue), the admission three ways.
     * Implemented in ApplicationNetworkEpoch.cpp (generated by scripts/nom_ep1_cut.py).
     *
     * @param run    The run, built once by runNetwork after the traces.
     * @param se_raw The synced epoch as the source delivered it.
     * @return LoopSignal::Stop at the maximum-duration cutoff, else LoopSignal::Next.
     */
    LoopSignal networkProcessEpoch_(NetworkRun& run, const SyncedEpoch& se_raw);

    friend struct NetworkEpochContractProbe;   // tests/test_network_epoch_contract.cpp reads the five private seam types

    /**
     * @brief Consumer-only replay (Q5): drive the held-out PPP-RTK consumer from
     *        a network product file written earlier (no producer solve).
     *
     * Dispatched from @c runNetwork() when
     * @c network.closed_loop.replay_products_path is non-empty, before any
     * producer setup.  Reconstructs the in-process closed-loop wiring with the
     * same calls (@c deriveClosedLoopConsumerConfig, a @c RealTimeEngine fed
     * epoch-by-epoch from the station's RINEX, an owned @c SsrCorrectionStore
     * injected via @c setSsrStore rather than the process-wide one), except
     * that the store is filled by @c NetworkProductReader::replay from the
     * recorded file instead of by the live emitter.  Writes the same
     * @c closed_loop_trace.csv schema (@c pos_err_3d_m,east_m,north_m,up_m) so
     * both sources grade identically.
     *
     * Implemented in @c ApplicationConsumerReplay.cpp.
     *
     * @return 0 on clean shutdown, non-zero on error.
     */
    int runConsumerReplay();

    /**
     * @brief Check that every @c network.stations[] entry has an explicit
     *        @c rinex_path (network setup phase 0; decomposition INC-5).
     *
     * Implemented in @c ApplicationNetworkSetup.cpp.
     *
     * @return 0 ok, 1 on the first station missing a path (after logging).
     */
    int networkValidateInputs_();

    /**
     * @brief Run network setup phases 2-4 (multi-station RINEX ingestion and
     *        epoch alignment; products, product cache and GIM; per-station
     *        Kalman resources), then build the product sinks, the S-basis
     *        contract and the closed-loop consumer (decomposition INC-5).
     *
     * Fills @p setup in place; @p setup must already sit at its final storage,
     * because run-long references pin its members and the struct is therefore
     * non-movable (see NetworkSetupResult.hpp).  Each fatal failure logs and
     * returns 1; the caller forwards the status.  Implemented in
     * @c ApplicationNetworkSetup.cpp.
     *
     * @param setup Out-parameter, default-constructed by the caller.
     * @return 0 ok, 1 on any fatal setup failure (after logging).
     */
    int networkSetup_(NetworkSetupResult& setup);

    /**
     * @brief True when station @p i's (sat, signal) ambiguity is gauge-pinned,
     *        that is, part of the phase datum rather than a free (carriable)
     *        ambiguity (decomposition INC-7).
     *
     * Legacy datum: every pivot-station ambiguity.  Prioritized datum: exactly
     * the elected @c pinned_amb_targets entries.  Implemented in
     * @c ApplicationNetworkAssembly.cpp.
     */
    bool networkIsGaugePinned_(const std::vector<NetStation>& stations,
                               bool phase_datum_prioritized, size_t i,
                               const std::string& sat,
                               const std::string& signal) const;

    /**
     * @brief Column-split helper: split station @p st's H into the shared
     *        border @p A_s (clock/phase-bias/code-bias columns) and the local
     *        nuisance block @p A_r (decomposition INC-7).
     *
     * Fills the H-column → A_r-column map @p h2local and, per state column, the
     * disposition it took (@p col_disp: border, local block, held coordinate,
     * held at its seed by @p ifb_rule, or unobserved), which the parameter
     * inventory records (card NOM N1).  Throws (linearity invariant) if a
     * position column ever survives into A_r; the per-station try/catch of the
     * PASS-B produce loop catches the throw.  Implemented in
     * @c ApplicationNetworkAssembly.cpp.
     *
     * @param ifb_rule The run's receiver IFB rule (@c ctx.param_inventory.rx_ifb,
     *                 built once per run by @c networkRxIfbRule): which receiver
     *                 IFB columns are left out of the local block.
     * @param[out] tags One contribution tag per data row (card NOM N2a): @c DataCode or
     *                 @c DataPhase with the row's satellite and signal
     *                 (@c tagDataRows), changed to @c DataCodeCompanion or
     *                 @c DataPhaseSecond by the weight pass that scales the row, so a
     *                 tag and a weight cannot disagree.
     */
    void networkSplitStation_(NetStation& st, const NetworkEstimator& est,
                              const EpochMeasurement& em,
                              const RxIfbRule& ifb_rule,
                              Eigen::MatrixXd& A_s, Eigen::MatrixXd& A_r,
                              Eigen::VectorXd& w, Eigen::VectorXd& y,
                              std::map<int, int>& h2local,
                              std::vector<ParamDisposition>& col_disp,
                              std::vector<RowTag>& tags) const;

    /**
     * @brief GIM ionosphere pseudo-observation rows (W6 satellite code-bias
     *        prerequisite), folded into station @p st's own block with the
     *        per-constellation common-mode-free R (decomposition INC-7).
     *        Implemented in @c ApplicationNetworkAssembly.cpp.
     *
     * @param[in,out] A_s,A_r,w,y  The station's design blocks, extended in place.
     * @param[in,out] tags  The station's row tags (card NOM N2a): one @c GimIono tag per
     *                      appended row, in row order.
     * @return What the builder did (card NOM section 16, suspect 4): no GIM loaded, the
     *         ionosphere weighting off, no qualifying line of sight, or rows appended.
     */
    GimRowsOutcome networkAppendGimIonoRows_(NetStation& st,
                                   const std::vector<PppObservation>& sta_obs,
                                   const std::map<int, int>& h2local,
                                   const GnssTime& t,
                                   Eigen::MatrixXd& A_s, Eigen::MatrixXd& A_r,
                                   Eigen::VectorXd& w, Eigen::VectorXd& y,
                                   std::vector<RowTag>& tags) const;

    /**
     * @brief Co-estimated SH-VTEC ionosphere pseudo-observation rows: the
     *        joint slant-iono ↔ SH-field constraint, including the T5 deviation
     *        carry treatment (decomposition INC-7).
     *
     * The run-loop state it reads arrives as explicit parameters: @p sh_model /
     * @p sh_model2 (the Iono-2 dual-layer second shell, null on the single-layer
     * path), @p shc,
     * @p sh_dev_on, @p sh_dev_carry (const: the serial pre-pass ages it, and
     * this function only looks entries up).  Implemented in
     * @c ApplicationNetworkAssembly.cpp.
     *
     * @param[in,out] tags  The station's row tags (card NOM N2a): one @c ShIonoTie tag per
     *                      appended row (its line of sight's satellite), in row order.
     */
    void networkAppendShIonoRows_(NetStation& st, const NetworkEstimator& est,
                                  const std::vector<PppObservation>& sta_obs,
                                  const std::map<int, int>& h2local,
                                  const GnssTime& t,
                                  const std::unique_ptr<SphericalHarmonicIonosphere>& sh_model,
                                  const std::unique_ptr<SphericalHarmonicIonosphere>& sh_model2,
                                  const GlobalVtecShConfig& shc, bool sh_dev_on,
                                  const std::map<std::string, ShDevCarry>& sh_dev_carry,
                                  Eigen::MatrixXd& A_s, Eigen::MatrixXd& A_r,
                                  Eigen::VectorXd& w, Eigen::VectorXd& y,
                                  std::vector<RowTag>& tags) const;

    /**
     * @brief Append the SD pre-reduce EWL constraint rows to one station's own block.
     *        See docs/SPEC_sd_ewl_prereduce.md.
     *
     * For each satellite @f$s@f$ paired against a pivot satellite @f$p@f$ on the
     * constellation's extra-wide-lane band pair @f$(b_1,b_2)@f$ (GPS 2/5, Galileo 7/5),
     * one pseudo-observation row
     * @f[
     *   z_{SD}=\overline{MW}(r,s)-\overline{MW}(r,p)
     *         =\lambda_{EWL}\big[N_{EWL}(r,s)-N_{EWL}(r,p)\big]+\big[B^s_{EWL}-B^p_{EWL}\big]
     * @f]
     * The pivot is elected here, per constellation: the arc with the most samples, ties
     * broken by ascending satellite name.  The receiver EWL bias is common to both arcs at
     * the same station and cancels identically, so the row carries only the SD ambiguity
     * combination and the SD satellite bias lump.  That lump is the reason for the row: it
     * reaches the shared satellite phase and code bias columns, which DD-chord rows barely
     * reach (about 80 % of them are eq34 tree pins; measured effect: +0.6 % on the
     * soft-end eigenvalue).
     *
     * Local coefficients (@p A_r), on the metre-valued per-band ambiguity states:
     * @f$+\lambda_{EWL}/\lambda_{b1}@f$, @f$-\lambda_{EWL}/\lambda_{b2}@f$ for @f$s@f$ and
     * negated for @f$p@f$.  Shared coefficients (@p A_s), from
     * @f$MW=\varphi_{WL}-P_{NL}@f$: @f$+f_1/(f_1-f_2)@f$ on @c PB|s|b1,
     * @f$-f_2/(f_1-f_2)@f$ on @c PB|s|b2, @f$-f_1/(f_1+f_2)@f$ on @c CB|s|b1,
     * @f$-f_2/(f_1+f_2)@f$ on @c CB|s|b2, negated for @f$p@f$.
     *
     * All rows of a group (one constellation at this station, split in two under
     * increment 2b below) share the pivot arc's noise, so
     * @f$R=\mathrm{diag}(\mathrm{var}_s)+\mathrm{var}_p\mathbf{1}\mathbf{1}^\top@f$,
     * structurally the GIM common-mode R.  R is whitened by @f$L^{-1}@f$
     * (@f$LL^\top=R@f$) into unit-information rows, as
     * @ref networkAppendGimIonoRows_ does.
     *
     * @par Why pre-reduce
     * These rows enter the station block that reaches @c addStationObservations, so they
     * fold into @f$N_{rr}/N_{sr}/N_{ss}@f$ before @c reduce() and therefore compose
     * exactly with the post-reduce Card-4 DD fold.  Two post-reduce Woodbury folds over
     * coupled local columns drop the cross term (see the warning on
     * @c NetworkNormalEquations::foldReducedDdPriors in NetworkNormalEquations.hpp).
     * @c addStationPrior cannot be used: it is diagonal-only
     * (@f$N_{rr}(c,c)\mathrel{+}=\Lambda@f$) and cannot express this cross-column row.
     *
     * @par Which signal pair
     * The MW arc key carries the two phase signal names, and the tracking channel the
     * detector holds an arc on is the detector's choice (@c collectUncombinedPairs'
     * KF-chosen / attribute-priority pick).  This function therefore searches every
     * candidate (band1, band2) phase pair that has both ambiguities in the station's local
     * block, in ascending signal name (a strict total order, for byte-identity), and takes
     * the first with a usable arc, rather than electing one channel and reporting the arc
     * absent when the detector picked the other.  The satellite code-bias columns come
     * from @c MwArcZdMean::code1/code2, the channels the observable actually consumed, so
     * the OSB attribution is not a guess.
     *
     * @par Thread safety
     * Runs inside the parallel produce phase.  It only reads @p est, through the const
     * @c phaseBiasColumn / @c codeBiasColumn lookups (never @c register*, which would
     * race and is illegal after @c beginEpoch()); both lookups use @c map::at and throw
     * on an absent key, so both are wrapped.  It writes only this station's own blocks,
     * @c st.sd_ewl_ctr and @p trace_rows; no shared state is mutated.
     *
     * @param st        This station (provides @c mw_slip, @c sm, @c name, the counters).
     * @param est       Shared-column lookup only (const).
     * @param sta_obs   This station's modeled observations this epoch.
     * @param h2local   Global state index → local column within @p A_r.
     * @param t         Epoch time (trace stamp only).
     * @param[in,out] A_s,A_r,w,y  The station's design blocks, extended in place.
     * @param[in,out] tags  The station's row tags (card NOM N2a): one @c SdEwl tag per
     *                      appended row (its satellite), in row order.
     * @param[out] trace_rows  Pre-formatted @c network_sd_ewl.csv lines for this
     *                  station-epoch, drained by PASS B's serial consume.  Untouched
     *                  unless @c network.sd_ewl.trace.
     *
     * @par Increment 2b: folding the fixed integer
     * When @c network.sd_ewl.fix_enable published an integer for an arc (@p fix, decided
     * serially before assembly by @ref networkSdEwlAr and read-only here), the row folds
     * @f$\lambda_{EWL}\nabla N^{fixed}@f$ out of the RHS and drops its four local
     * ambiguity coefficients: the local degree of freedom is removed, not constrained.
     * A tight prior on a gauge-adjacent local column would be a stiff prior, not a rank
     * reduction; increment 2 exists so that the local nuisance stops absorbing the row's
     * information and the shared satellite bias receives it.  @c pred_m loses its
     * ambiguity terms in step with the design, because a row whose H changed while its
     * linearization point did not would be folded in a mixed frame.
     *
     * Fixed and float rows coexist at one station, but in separate whitened groups, each
     * with its own pivot and its own common-mode noise.  They cannot share one:
     * @f$R=\mathrm{diag}+\mathrm{var}_p\mathbf{1}\mathbf{1}^\top@f$ is whitened by a dense
     * @f$L^{-1}@f$, which would re-populate a fixed row's zeroed local columns from its
     * float siblings, and the pivot arc's ambiguity columns appear in every row of a
     * group, so a mixed group would both remove and estimate the pivot ambiguity.  Two
     * groups keep both halves exact and make "a fixed row has zero local coefficients"
     * true of the whitened block as well as of H.
     *
     * @param fix  The published integers for this epoch, keyed by (station index,
     *             constellation) and then satellite; this station's entries are read at
     *             @p station_index.  @c nullptr, @c network.sd_ewl.fix_enable off, or no
     *             usable entry for an arc => that row takes the increment-1 float path.
     * @param station_index  This station's index into @p fix.
     */
    void networkAppendSdEwlRows_(NetStation& st, const NetworkEstimator& est,
                                 const std::vector<PppObservation>& sta_obs,
                                 const std::map<int, int>& h2local,
                                 const GnssTime& t,
                                 Eigen::MatrixXd& A_s, Eigen::MatrixXd& A_r,
                                 Eigen::VectorXd& w, Eigen::VectorXd& y,
                                 std::vector<RowTag>& tags,
                                 std::vector<std::string>& trace_rows,
                                 const std::map<std::pair<std::size_t, char>,
                                                std::map<std::string, SdEwlFix>>* fix,
                                 std::size_t station_index) const;

    /**
     * @brief PASS-A head: under the legacy datum, model the pivot station first and
     *        seed the shared phase-bias key set from its states (decomposition INC-7).
     *
     * The two sync_skip exits do their bookkeeping here (n_skipped, a trace row of
     * 9 zeros, an empty status row) and do not advance prev_week/prev_tow.  Under
     * the prioritized datum the pivot is modelled as an ordinary station in
     * @ref networkPassASlipScan_, so this function does nothing and leaves
     * @p shared_phase_keys empty; a missing pivot then does not cost the epoch (the
     * pivot is not a single point of failure).
     * Implemented in @c ApplicationNetworkAssembly.cpp.
     *
     * Epoch form (card NOM3 step EP1; generated by scripts/nom_ep1_cut.py): the run and the epoch
     * replace the explicit parameters; the definition binds each former parameter in its preamble,
     * under its former name.
     *
     * @param run The run (app/NetworkRun.hpp). Written through non-const bindings: run.ctx (ctx),
     *            run.setup (setup), run.traces (traces).
     * @param ep The epoch (app/NetworkEpoch.hpp), read only. Bindings: ep.se (se).
     * @param pass_a [in/out] The layout (ep.pass_a), written: pass_a.modeled (modeled),
     *     pass_a.present (present), pass_a.shared_phase_keys (shared_phase_keys).
     *
     * The former parameters, by binding:
     *   - @p se = ep.se.
     *   - @p ctx = run.ctx.
     *   - @p setup = run.setup.
     *   - @p traces = run.traces.
     *   - @p modeled = pass_a.modeled.
     *   - @p present = pass_a.present.
     *   - @p shared_phase_keys = pass_a.shared_phase_keys.
     *
     * @return Proceed, or SkipEpoch when the legacy datum station is absent
     *         or fails to model (the caller then ends the epoch).
     */
    EpochAction networkPassAHead_(NetworkRun& run, const NetworkEpoch& ep, EpochPassA& pass_a);

    /**
     * @brief PASS-A parallel tail and slip scans: per-station geometry
     *        (computeModels / ensureNetworkSharedStates) in an OpenMP loop that
     *        also runs the T0 / MW / IF-TD slip detection into PassABuffers slots,
     *        the serial fold of present[], the raw slip observables into @p qc, and
     *        the serial slip-QC drains declared in slip/NetworkSlipQc.hpp: T0
     *        repair, MW / LLI refloat, IF-TD driver, wind-up restart, gap re-entry
     *        and injected-arc free (decomposition INC-7).
     *
     * The caller brackets the call with subBegin/subEnd(SB_SLIP), the D0b section
     * timer for the slip scans.  @p carry_slipped_ambs receives, by non-const
     * reference, the (station index, ambiguity key) pairs the drains flag this
     * epoch.  The driver passes its raw-detector set carry_slipped_raw here and
     * unions it into the per-epoch slip-event funnel carry_slipped_ambs (Card-4 C3c)
     * right after the call; a copy would drop the drains' inserts and leave the DD
     * carry without its slip events (standing invariant #1).
     * Implemented in @c ApplicationNetworkAssembly.cpp.
     *
     * Epoch form (card NOM3 step EP1; generated by scripts/nom_ep1_cut.py): the run and the epoch
     * replace the explicit parameters; the definition binds each former parameter in its preamble,
     * under its former name.
     *
     * @param run The run (app/NetworkRun.hpp). Written through non-const bindings: run.ctx (ctx),
     *            run.setup (setup), run.traces (traces), run.engines.slip_qc (qc).
     * @param ep The epoch (app/NetworkEpoch.hpp), read only. Bindings: ep.se (se), ep.clock.dt
     *           (dt), ep.clock.pl_now_s (pl_now_s), ep.clock.carry_warm (carry_warm),
     *           ep.pass_a.shared_phase_keys (shared_phase_keys).
     * @param pass_a [in/out] The layout (ep.pass_a), written: pass_a.modeled (modeled),
     *     pass_a.present (present).
     * @param funnel [in/out] The slip funnel (ep.funnel), written: funnel.carry_slipped_raw
     *     (carry_slipped_ambs).
     *
     * The former parameters, by binding:
     *   - @p se = ep.se.
     *   - @p ctx = run.ctx.
     *   - @p setup = run.setup.
     *   - @p traces = run.traces.
     *   - @p dt = ep.clock.dt.
     *   - @p pl_now_s = ep.clock.pl_now_s.
     *   - @p carry_warm = ep.clock.carry_warm.
     *   - @p modeled = pass_a.modeled.
     *   - @p present = pass_a.present.
     *   - @p shared_phase_keys = ep.pass_a.shared_phase_keys.
     *   - @p carry_slipped_ambs = funnel.carry_slipped_raw.
     *   - @p qc = run.engines.slip_qc.
     */
    void networkPassASlipScan_(NetworkRun& run, const NetworkEpoch& ep, EpochPassA& pass_a,
                               NetworkSlipFunnel& funnel);

    /**
     * @brief Raw slip-observable trace rows and the cross-station observability
     *        pre-filter (decomposition INC-7).  The rows are written before the
     *        prune, so a satellite about to be dropped is still captured.  Prunes
     *        single-station satellites from @p modeled and from the per-station
     *        shared states; @p n_dropped_sats receives their count for the epoch.
     *        Implemented in @c ApplicationNetworkAssembly.cpp.
     *
     * Epoch form (card NOM3 step EP1; generated by scripts/nom_ep1_cut.py): the run and the epoch
     * replace the explicit parameters; the definition binds each former parameter in its preamble,
     * under its former name.
     *
     * @param run The run (app/NetworkRun.hpp). Written through non-const bindings: run.ctx (ctx),
     *            run.setup (setup), run.traces (traces). Read: run.engines.slip_qc (qc).
     * @param ep The epoch (app/NetworkEpoch.hpp), read only. Bindings: ep.se (se),
     *           ep.pass_a.present (present).
     * @param pass_a [in/out] The layout (ep.pass_a), written: pass_a.modeled (modeled),
     *     pass_a.n_dropped_sats (n_dropped_sats).
     *
     * The former parameters, by binding:
     *   - @p se = ep.se.
     *   - @p ctx = run.ctx.
     *   - @p setup = run.setup.
     *   - @p traces = run.traces.
     *   - @p qc = run.engines.slip_qc.
     *   - @p modeled = pass_a.modeled.
     *   - @p present = ep.pass_a.present.
     *   - @p n_dropped_sats = pass_a.n_dropped_sats.
     */
    void networkAssemblyPrefilter_(NetworkRun& run, const NetworkEpoch& ep, EpochPassA& pass_a);

    /**
     * @brief Shared-column registration: est.reset(), the clock / phase-bias /
     *        W6-OSB / SH-VTEC column registration, the epoch counts and
     *        heartbeat stores, the estimability column descriptors, and the
     *        empty-system skip (decomposition INC-7).
     *
     * Ends with @c est.beginEpoch() on the Proceed path.  The skip does its
     * bookkeeping here (n_skipped, the trace row with n_present, the status row,
     * the prev_week/prev_tow advance); on SkipEpoch the caller ends the epoch
     * without calling subEnd(SB_PREASM).  The epoch counts are the int&
     * out-parameters.  Implemented in @c ApplicationNetworkAssembly.cpp.
     *
     * Epoch form (card NOM3 step EP1; generated by scripts/nom_ep1_cut.py): the run and the epoch
     * replace the explicit parameters; the definition binds each former parameter in its preamble,
     * under its former name.
     *
     * @param run The run (app/NetworkRun.hpp). Written through non-const bindings: run.ctx (ctx),
     *            run.setup (setup), run.traces (traces), run.engines.est (est).
     * @param ep The epoch (app/NetworkEpoch.hpp), read only. Bindings: ep.se (se),
     *           ep.pass_a.present (present), ep.pass_a.shared_phase_keys (shared_phase_keys).
     * @param columns [in/out] The shared columns (ep.columns), written:
     *     columns.registered_clock_sats (registered_clock_sats), columns.registered_code_keys
     *     (registered_code_keys), columns.col_desc (col_desc), columns.n_clock (n_clock),
     *     columns.n_pb (n_pb), columns.n_cb (n_cb), columns.n_present (n_present).
     *
     * The former parameters, by binding:
     *   - @p se = ep.se.
     *   - @p ctx = run.ctx.
     *   - @p setup = run.setup.
     *   - @p traces = run.traces.
     *   - @p est = run.engines.est.
     *   - @p present = ep.pass_a.present.
     *   - @p shared_phase_keys = ep.pass_a.shared_phase_keys.
     *   - @p registered_clock_sats = columns.registered_clock_sats.
     *   - @p registered_code_keys = columns.registered_code_keys.
     *   - @p col_desc = columns.col_desc.
     *   - @p n_clock = columns.n_clock.
     *   - @p n_pb = columns.n_pb.
     *   - @p n_cb = columns.n_cb.
     *   - @p n_present = columns.n_present.
     */
    EpochAction networkAssemblyRegisterColumns_(NetworkRun& run, const NetworkEpoch& ep,
                                                EpochColumns& columns);

    /**
     * @brief PASS B: the D0 assembly wall timer (a raw chrono pair), the T5 SH-dev
     *        aging, the Card-1d reinit history, the OpenMP produce loop (per
     *        station: computeMeasurementUpdate, networkSplitStation_, pin folds,
     *        iono and SD-EWL rows, the phase-leveling carry build and the
     *        coupling-injection cluster), and the serial consume/scatter
     *        (decomposition INC-7).
     *
     * The failed/empty-epoch skip does its bookkeeping here (n_skipped, the trace
     * row with n_present/n_clock/n_pb/total_pins, the status row, the
     * prev_week/prev_tow advance).  @p asm_res, @p est_index_of and @p total_pins
     * are the driver's epoch locals, filled here for the solve, RAIM and emit
     * seams downstream; @p sec_asm_ms_epoch / @p sec_asm_wall_ms receive the D0
     * wall time, which accumulates even when the epoch skips.  The SH-deviation carry's
     * switches (sh_dev_tau, sh_dev_on, sh_dev_demean, sh_dev_d5_keep) are read from
     * @c ctx.knobs (card NOM3 step EP0).
     * Implemented in @c ApplicationNetworkAssembly.cpp.
     *
     * Epoch form (card NOM3 step EP1; generated by scripts/nom_ep1_cut.py): the run and the epoch
     * replace the explicit parameters; the definition binds each former parameter in its preamble,
     * under its former name.
     *
     * @param run The run (app/NetworkRun.hpp). Written through non-const bindings: run.ctx (ctx),
     *            run.setup (setup), run.traces (traces), run.engines.est (est),
     *            run.timing.asm_wall_ms (sec_asm_wall_ms).
     * @param ep The epoch (app/NetworkEpoch.hpp), read only. Bindings: ep.se (se),
     *           ep.funnel.carry_slipped_ambs (carry_slipped_ambs), ep.funnel.carry_slipped_raw
     *           (carry_slipped_raw), ep.clock.carry_warm (carry_warm), ep.clock.pl_now_s
     *           (pl_now_s), ep.pass_a.shared_phase_keys (shared_phase_keys),
     *           ep.columns.registered_clock_sats (registered_clock_sats),
     *           ep.columns.registered_code_keys (registered_code_keys), ep.columns.n_clock
     *           (n_clock), ep.columns.n_pb (n_pb), ep.columns.n_present (n_present).
     * @param pass_a [in/out] The layout (ep.pass_a), written: pass_a.modeled (modeled),
     *     pass_a.present (present).
     * @param assembly [in/out] The station blocks (ep.assembly), written: assembly.asm_res
     *     (asm_res), assembly.est_index_of (est_index_of), assembly.total_pins (total_pins),
     *     assembly.sec_asm_ms_epoch (sec_asm_ms_epoch).
     *
     * The former parameters, by binding:
     *   - @p se = ep.se.
     *   - @p ctx = run.ctx.
     *   - @p setup = run.setup.
     *   - @p traces = run.traces.
     *   - @p est = run.engines.est.
     *   - @p carry_slipped_ambs = ep.funnel.carry_slipped_ambs.
     *   - @p carry_slipped_raw = ep.funnel.carry_slipped_raw.
     *   - @p carry_warm = ep.clock.carry_warm.
     *   - @p pl_now_s = ep.clock.pl_now_s.
     *   - @p modeled = pass_a.modeled.
     *   - @p present = pass_a.present.
     *   - @p shared_phase_keys = ep.pass_a.shared_phase_keys.
     *   - @p registered_clock_sats = ep.columns.registered_clock_sats.
     *   - @p registered_code_keys = ep.columns.registered_code_keys.
     *   - @p n_clock = ep.columns.n_clock.
     *   - @p n_pb = ep.columns.n_pb.
     *   - @p n_present = ep.columns.n_present.
     *   - @p asm_res = assembly.asm_res.
     *   - @p est_index_of = assembly.est_index_of.
     *   - @p total_pins = assembly.total_pins.
     *   - @p sec_asm_ms_epoch = assembly.sec_asm_ms_epoch.
     *   - @p sec_asm_wall_ms = run.timing.asm_wall_ms.
     *
     * @return Proceed, or SkipEpoch when the epoch failed assembly or has an
     *         empty shared system (the caller then ends the epoch).
     */
    EpochAction networkPassB_(NetworkRun& run, const NetworkEpoch& ep, EpochPassA& pass_a,
                              EpochAssembly& assembly);

    /**
     * @brief Generate the end-of-run HTML report via the bundled pyviz tool.
     *
     * No-op unless @c config_.autoReport() is set.  Flushes the trace logger,
     * then delegates to @ref generateReportFor_ against @c log_dir_, which shells
     * out to @c <project_root>/pyviz/run_report.py and writes @c gnss_report.html
     * there.  The interpreter is @c config_.reportPython() if set, else the baked
     * @c <project_root>/venv interpreter, else @c python on PATH; the backend is
     * @c config_.reportBackend() (matplotlib when empty).  Always non-fatal: a
     * missing pyviz script or interpreter, or a non-zero exit, is logged as a
     * warning and processing still returns success.
     *
     * Baseline multi-rover runs (whose traces live in per-rover subdirs) call
     * @ref generateReportFor_ directly, once per rover dir.
     */
    void generateReport_();

    /**
     * @brief Shell out to run_report.py for one trace directory.
     *
     * Writes @c <trace_dir>/gnss_report.html using the configured backend /
     * viewer subset / interpreter (sync or detached per @c config_).  Assumes
     * the caller has already checked @c autoReport() and flushed the traces.
     *
     * @param trace_dir Folder holding the run's *_trace.csv files.
     */
    void generateReportFor_(const std::filesystem::path& trace_dir);

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
    std::unique_ptr<AidingSourceManager> aiding_manager_;   //!< Owns the config-selected aiding sources (IMU first client); there is no separate ImuIntegrator member.
    std::unique_ptr<AmbiguityResolver>   ambiguity_resolver_;
    std::unique_ptr<GlobalIonosphereMap> gim_;  /**< IONEX global ionosphere map for PPP-RTK ionosphere weighting (post-processing), the STEC source of PppModel's higher-order ionosphere correction, and the network producer's GIM rows.  Null unless one of them requested it; check isLoaded() before use. */
    std::unique_ptr<NeqAccumulator>      neq_accumulator_;  /**< Cross-session normal-equation accumulator (item #13B); non-null only when processing.enable_neq_export. */

    PppState ppp_state_;                      /**< Current PPP state (synced with filter).  In baseline mode this is the rover state. */
    Eigen::Vector3d initial_position_;        /**< Position used for sanity-check reversion. */
    bool sif_apriori_seeded_{ false };        /**< True when @ref initializePosition seeded @c initial_position_ from the SIF (ARP-reduced); run() then re-applies it after PppModel::initializeState, which re-derives the position from the RINEX header. */

    // Baseline-mode reference-station companions.  In baseline mode PppModel
    // runs twice per epoch (rover + reference), so per-receiver errors
    // (satellite clock, corrections, geometry) are captured the same way
    // as in PPP.  DD is then formed by subtracting per-receiver residuals
    // rather than recomputing geometry from scratch.
    std::unique_ptr<StateManager> ref_state_manager_; /**< Baseline reference station state (position-only, fixed). */
    std::unique_ptr<PppModel>     ref_ppp_model_;     /**< PppModel instance bound to the reference station. */
    PppState                      ref_ppp_state_;     /**< Reference-station PppState (fixed position, zero clock/tropo). */

    bool first_epoch_{true};                  /**< True until the Kalman predict of the first processed epoch (its last reader) clears it; gates the initial-dt assumption, decimation skip, processing.max_duration_s anchor, IMU-aided predict, and WLS clock seeding. */
    double prev_tow_{0.0};                    /**< Previous epoch TOW for dt computation. */
    double last_processed_tow_{std::numeric_limits<double>::lowest()}; /**< TOW of the most recently processed epoch; the getGnssRate() decimation measures the spacing from here.  Set together with prev_tow_ after the decimation skip, so a skipped input epoch updates neither. */
    double max_duration_anchor_s_{0.0};       /**< Continuous GPS seconds of the first processed epoch; the processing.max_duration_s cutoff measures elapsed data time from here. Only meaningful once first_epoch_ has cleared. */
    GnssTime prev_epoch_time_{0, 0.0};        /**< Previous epoch full GnssTime for IMU integration. */
    double prev_pos_sigma_3d_{std::numeric_limits<double>::max()}; /**< Post-update 3D position sigma. */

    /// Pre-update position captured at the top of each runFilterUpdateLoop;
    /// used by runGeometryRelinearization for its |Δpos| gate on both the IEKF
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