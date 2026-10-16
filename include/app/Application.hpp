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

struct NetworkSetupResult;   // network/driver/SetupResult.hpp (decomposition INC-5)
struct NetworkRunContext;    // network/state/RunContext.hpp (decomposition INC-4; INC-7 signatures)
struct NetworkRunTraces;     // network/diag/RunTraces.hpp (decomposition INC-3; INC-7 signatures)
struct SdEwlFix;             // network/driver/RunTypes.hpp (SD EWL integer, increment 2a)
struct NetStation;           // network/driver/RunTypes.hpp (decomposition INC-7)
struct StationAssembly;      // network/driver/RunTypes.hpp (decomposition INC-7)
enum class ParamDisposition : std::uint8_t;  // network/assembly/ParamInventory.hpp
struct RxIfbRule;            // network/assembly/ParamInventory.hpp
struct RowTag;               // network/assembly/Contributions.hpp
enum class GimRowsOutcome : std::uint8_t;  // network/assembly/Contributions.hpp
struct ShDevCarry;           // network/driver/RunTypes.hpp (decomposition INC-7)
struct SyncedEpoch;          // rinex/MultiRinexSync.hpp, namespace scope (decomposition INC-7)
class  NetworkEstimator;     // network/solve/Estimator.hpp (decomposition INC-7)
class  SphericalHarmonicIonosphere;  // atmosphere/SphericalHarmonicIonosphere.hpp (decomposition INC-7)
class  PppUtils;             // common/PppUtils.hpp (decomposition INC-7)
struct SlipQcChannel;        // network/slip/RawObservables.hpp (slip-QC hardening)
enum class EpochAction;      // network/driver/RunTypes.hpp, opaque enum declaration (decomposition INC-7)
struct NetworkRun;           // network/driver/Run.hpp
struct NetworkEpoch;         // network/driver/Epoch.hpp
struct EpochEvents;             // network/driver/Epoch.hpp
struct EpochPassA;           // network/driver/Epoch.hpp
struct NetworkSlipFunnel;    // network/driver/Epoch.hpp
struct EpochColumns;         // network/driver/Epoch.hpp
struct EpochAssembly;        // network/driver/Epoch.hpp

struct PppEpochWork;         // app/PppPipelineWork.hpp: the values one PPP epoch's stages share
struct PppRunEndView;        // app/PppPipelineWork.hpp: what the PPP run-end reports read
struct BaselineRunWork;      // app/ApplicationBaseline.cpp: what one baseline run's stages share
struct BaselineRoverWork;    // app/ApplicationBaseline.cpp: what one rover's stages share
struct BaselineEpochWork;    // app/ApplicationBaseline.cpp: what one epoch of one rover shares
enum class BaselineEpochAction;  // app/ApplicationBaseline.cpp: how a baseline epoch ends
class  MultiRinexSync;       // rinex/MultiRinexSync.hpp

/**
 * @class Application
 * @brief High-level program driver for GNSSProcessor.
 *
 * Constructs the runtime application with an optional path to a configuration
 * file and exposes a single `run()` method that executes the program.
 *
 * `run()` loads the configuration and dispatches on the mode: real time
 * (@ref runRealTime), baseline (@ref runBaseline), the network producer
 * (@ref runNetwork), or the PPP / PPP-RTK pipeline (@ref runPpp).  runPpp() is
 * a short ordered list of named private stages, declared
 * below in pipeline order under the file each lives in:
 *  - the input phases (Application.cpp): logging and configuration, data
 *    resolution, RINEX and product loading;
 *  - setup (PppSetup.cpp): the a-priori position, the components and their
 *    wiring, the traces;
 *  - one epoch (PppEpochSteps.cpp): the observations, the decimation and
 *    duration gates, the time step and data gap, the models, the WLS clock
 *    seed, the gap policy, the process-noise overrides, the Kalman predict,
 *    the cycle slips;
 *  - the update (PppEpochUpdate.cpp): the update loop, the geometry
 *    relinearization, the merged resets, the GIM update, ambiguity resolution;
 *  - the epoch's records (PppEpochRecords.cpp): the smoother snapshots, the
 *    NEQ, the soft-LLI snapshot, the summary line and the CSV traces;
 *  - the run end (PppRunEnd.cpp): a declared table of reports.
 * The baseline pipeline (@ref runBaseline, ApplicationBaseline.cpp) has the
 * same shape: a spine over setup, per-rover and per-epoch stages.
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
    // The input phases (Application.cpp): run() loads the configuration; the
    // modes resolve and load their own inputs
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
     * @brief The PPP / PPP-RTK pipeline: the spine @c run() dispatches to for the PPP,
     *        PPP-RTK and PPP-RTK-user modes.
     *
     * A short ordered list of named stages: the input phases
     * (@ref resolveData, @ref loadInputData), the setup stages (PppSetup.cpp), the epoch
     * loop over the stages of PppEpochSteps.cpp, PppEpochUpdate.cpp and
     * PppEpochRecords.cpp with one @c PppEpochWork per epoch, and the run end
     * (@ref reportPppRunEnd_, then @ref generateReport_).  Defined in Application.cpp.
     *
     * @return 0 on success; 1 when an input phase or the position seed fails, or when
     *         ppp_state_ cannot be synced from the state vector.
     */
    int runPpp();

    // -----------------------------------------------------------------------
    // PPP setup (PppSetup.cpp), in the order runPpp calls the stages
    // -----------------------------------------------------------------------

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
     * RINEX header, so @ref initializePppState_ re-applies the SIF seed after it unless a reference
     * position is configured (@c sif_apriori_seeded_).
     *
     * @return true on success, false on unrecoverable error.
     */
    bool initializePosition();

    /**
     * @brief Build the PPP processing components: PppModel, MeasurementModel,
     *        KalmanFilter, WlsEstimator and CycleSlipManager, in that order.
     *
     * Requires @c state_manager_, @c product_parser_ and @c rinex_parser_.
     */
    void buildPppComponents_();

    /**
     * @brief Build the global ionosphere map (GIM) when ionosphere weighting
     *        (estimation.ionosphere_weighting) or the higher-order ionosphere
     *        correction (model_corrections.enable_higher_order_ionosphere) asks for it,
     *        and hand it to PppModel for the latter.
     *
     * A missing IONEX file is a warning; the dependent features then skip their work.
     */
    void buildGlobalIonosphereMap_();

    /**
     * @brief Build the adaptive ionospheric process-noise monitor when
     *        estimation.adaptive_iono_process_noise.enable is set; otherwise it stays
     *        null and the ionospheric process noise stays static.
     */
    void buildIonoVariabilityMonitor_();

    /**
     * @brief Build the cross-session normal-equation accumulator when
     *        processing.enable_neq_export is set, and log that the export is on.
     */
    void buildNeqAccumulator_();

    /**
     * @brief Wire the cycle-slip manager: the BIA satellite biases for the MW and EWL
     *        detectors, the MW arc gap tolerance and the TurboEdit sample interval from
     *        the processing cadence, and the manager as the KalmanFilter's MW tiebreak
     *        source.
     */
    void wireCycleSlipManager_();

    /**
     * @brief Open the module traces the configuration arms: the MW and TurboEdit
     *        traces, the wind-up trace and census (estimation.trace_windup_census), and
     *        the adaptive ionospheric monitor's trace.
     */
    void enablePppTraceStreams_();

    /**
     * @brief Build the aiding sources (the IMU first), reserve their states and attach
     *        the IMU to the KalmanFilter; log the IMU sample count when it has data.
     */
    void buildAidingSources_();

    /**
     * @brief Initialize @c ppp_state_ through PppModel::initializeState, re-apply the
     *        SIF a-priori position it would overwrite (a configured reference position
     *        keeps precedence), and sync the state vector from it.
     */
    void initializePppState_();

    /**
     * @brief Build the ambiguity resolver from the ambiguity_resolution section when its
     *        mode is above 0, and log its configuration.
     */
    void buildAmbiguityResolver_();

    /**
     * @brief Open the CSV trace logger in @c log_dir_; warns when requested streams fail
     *        to open.
     * @param is_ppp_rtk The processing mode is PPP-RTK (the PPP-RTK trace columns).
     */
    void buildTraceLogger_(bool is_ppp_rtk);

    // -----------------------------------------------------------------------
    // One PPP epoch up to the measurement update (PppEpochSteps.cpp), in the order
    // runPpp calls the stages.  A stage taking a PppEpochWork reads and writes the
    // epoch's working value (app/PppPipelineWork.hpp).
    // -----------------------------------------------------------------------

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
     * @brief Stage 4a2: whether processing.gnss_rate_s decimation skips this epoch
     *        (closer than the processing rate to the last processed epoch, with 1 ms of
     *        tolerance).  The first epoch anchors the schedule and is never skipped.
     * @param w The epoch's working value (reads current_time).
     * @return True to skip the epoch, without a log line.
     */
    bool skipDecimatedEpoch_(const PppEpochWork& w) const;

    /**
     * @brief Stage 4a3: the processing.max_duration_s cutoff.  The first processed
     *        epoch anchors the window; an epoch whose data-time span reaches the limit
     *        ends the loop, with a log line.
     * @param w The epoch's working value (reads current_time).
     * @return True when the run stops at this epoch.
     */
    bool reachedMaxDuration_(const PppEpochWork& w);

    /**
     * @brief Stage 4b: stamp the epoch on the state manager and the log, compute the
     *        time step and the data-gap flags, and advance the previous-epoch markers
     *        (@c prev_tow_, @c prev_epoch_time_, @c last_processed_tow_).
     * @param w The epoch's working value; writes dt, gap_detected and long_gap.
     */
    void stepEpochClock_(PppEpochWork& w);

    /**
     * @brief Stage 4c: sync @c ppp_state_ from the state vector; runPpp calls it again
     *        after the predict.
     * @param w The epoch's working value (reads epoch_prefix).
     * @return False when the sync throws (logged); runPpp then returns 1.
     */
    bool syncPppState_(const PppEpochWork& w);

    /**
     * @brief Stage 4d: model the epoch's observations at @c ppp_state_ and log the
     *        modeled satellites (debug).
     * @param w The epoch's working value; writes modeled_obs and drop_reasons.
     * @return False when modelling fails (logged); the epoch is then skipped.
     */
    bool computeEpochModels_(PppEpochWork& w);

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
     * @brief Stage 4e2: refresh the per-ambiguity process-noise overrides of the
     *        ambiguities whose signals' ATX corrections were frequency-substituted
     *        (model_corrections.loosen_amb_q_for_substituted_atx), or clear them when the
     *        toggle is off.  The next predict reads them.
     * @param w The epoch's working value (reads epoch_prefix).
     */
    void refreshSubstitutedAtxQ_(const PppEpochWork& w);

    /**
     * @brief Stage 4e3: refresh the per-satellite ionospheric process-noise overrides
     *        from the adaptive monitor; no-op when the monitor is off.
     * @param w The epoch's working value (reads modeled_obs).
     */
    void refreshAdaptiveIonoQ_(const PppEpochWork& w);

    /**
     * @brief Stage 4f: the Kalman prediction (aiding-augmented when an aiding source is
     *        active after the first epoch), then clear @c first_epoch_, whose last reader
     *        it is.
     * @param w The epoch's working value (reads current_time and dt).
     */
    void predictEpoch_(const PppEpochWork& w);

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

    // -----------------------------------------------------------------------
    // The measurement update and ambiguity resolution (PppEpochUpdate.cpp)
    // -----------------------------------------------------------------------

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
     * @brief Merge into the epoch's reset set the KalmanFilter's last reset flags and the
     *        slip phase's resets, so the smoother segments every one of them.
     * @param w The epoch's working value; reads slip_result, writes epoch_reset_sats and
     *          epoch_complete_reset.
     */
    void mergeEpochResets_(PppEpochWork& w);

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
     * @brief Stage 4i2: integer ambiguity resolution (MLAMBDA) on an epoch with an
     *        applied update: the WL entries from the cycle-slip manager, the AR-side ATX
     *        gates, the WL-only FFR quality input, then on a fix the fixed state and
     *        covariance and the fix-and-hold constraint.  No-op without a resolver.
     * @param w The epoch's working value (reads epoch_prefix, modeled_obs and
     *          have_applied).
     */
    void resolveEpochAmbiguities_(const PppEpochWork& w);

    /**
     * @brief The AR-side ATX gates: drop the WL entries, and their fractions, whose
     *        either signal lacks strict ATX coverage
     *        (model_corrections.require_atx_coverage_for_ar) or had its correction
     *        frequency-substituted (model_corrections.restrict_ar_for_substituted_atx).
     *        The observations stay in the float update.
     * @param epoch_prefix     Log prefix.
     * @param[in,out] wl_ambs  WL integers keyed "<sat>_<sig1>_<sig2>".
     * @param[in,out] wl_fracs Their fractional parts, under the same keys.
     */
    void dropAtxGatedWlEntries_(const std::string& epoch_prefix,
                                std::map<std::string, int>& wl_ambs,
                                std::map<std::string, double>& wl_fracs);

    // -----------------------------------------------------------------------
    // What the epoch records (PppEpochRecords.cpp): the smoother snapshots, the
    // NEQ, the soft-LLI snapshot, the summary line and the CSV traces
    // -----------------------------------------------------------------------

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
     * @brief Fold the slip phase's covariance-preserving integer repairs into the
     *        a-priori of the snapshot just captured (a deterministic control input), so
     *        the backward pass does not absorb the jump; a satellite also reset this
     *        epoch is segmented instead.  No-op without the smoother or a repair.
     * @param w The epoch's working value (reads slip_result).
     */
    void foldSlipRepairsIntoPrior_(const PppEpochWork& w);

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
     * @brief Mirror the accepted update into the cross-session normal equations, a
     *        read-only consumer of the filter; warns when the state labels do not match
     *        the update's H columns.  No-op without the accumulator.
     * @param w The epoch's working value (reads epoch_prefix, applied_meas,
     *          have_applied and epoch_reset_sats).
     */
    void accumulateNormalEquations_(const PppEpochWork& w);

    /**
     * @brief Update the soft-LLI bridge's last-good ambiguity snapshot from the float
     *        posterior, before AR tightens it; no-op unless turbo_edit_bridge_lli.
     * @param w The epoch's working value (reads current_time).
     */
    void recordBridgeSnapshot_(const PppEpochWork& w);

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

    // -----------------------------------------------------------------------
    // The PPP run end (PppRunEnd.cpp): the rows of kPppRunEndReports in order
    // -----------------------------------------------------------------------

    /**
     * @brief Walk the run-end table kPppRunEndReports, declared in this function as the
     *        run end's table of contents, and call each report in order.
     * @param v What the reports read beyond the Application's members.
     */
    void reportPppRunEnd_(const PppRunEndView& v);

    /**
     * @brief Report: the wind-up census readout (estimation.trace_windup_census): the
     *        forward transitions, the in-horizon steps, the accumulator restarts and the
     *        line that says how to read them.
     * @param v The run-end view (unused: the census is PppModel's).
     */
    void reportWindUpCensus_(const PppRunEndView& v);

    /**
     * @brief Run the backward RTS smoother over the buffered snapshots
     *        (processing.run_smoother) and write its trace when
     *        logging.enable_smoother_trace is set.  The buffer moves into the smoother.
     * @param v The run-end view (unused).
     */
    void runSmootherPass_(const PppRunEndView& v);

    /**
     * @brief Write smoother_trace.csv: per smoothed epoch the position, the receiver
     *        clock and the zenith wet delay with their sigmas (a state the layout does
     *        not hold as NaN).
     * @param smoother The smoother after a successful backward pass.
     */
    void writeSmootherTrace_(const Smoother& smoother);

    /**
     * @brief Report: solve and write the cross-session normal equations
     *        (processing.enable_neq_export) and log the solution summary.
     * @param v The run-end view (unused).
     */
    void exportNormalEquations_(const PppRunEndView& v);

    /**
     * @brief Report: the "Processing completed in N ms" line, timed from the start of the
     *        epoch loop to this row.
     * @param v Reads wall_start and is_ppp_rtk.
     */
    void reportProcessingTime_(const PppRunEndView& v);

    /**
     * @brief Report: the state manager's re-inflation counters.
     * @param v Reads is_ppp_rtk.
     */
    void reportReinflations_(const PppRunEndView& v);

    /**
     * @brief Report: the KalmanFilter's timing block and QC counters.
     * @param v The run-end view (unused).
     */
    void reportKalmanFilterRunEnd_(const PppRunEndView& v);

    /**
     * @brief Report: the cycle-slip manager's run-end counters.
     * @param v The run-end view (unused).
     */
    void reportCycleSlipRunEnd_(const PppRunEndView& v);

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
     * @c ApplicationBaseline.cpp as a short ordered driver over the stages declared
     * below.
     *
     * @return 0 on clean shutdown, non-zero on error (including a run in which
     *         no rover overlaps the reference on any synced epoch).
     */
    int runBaseline();

    // -----------------------------------------------------------------------
    // The baseline pipeline's stages (ApplicationBaseline.cpp),
    // in the order runBaseline calls them.  The working values BaselineRunWork,
    // BaselineRoverWork and BaselineEpochWork are defined in that file.
    // -----------------------------------------------------------------------

    /**
     * @brief Setup: the PathResolver pass (production mode points the observation path
     *        at the reference station first, for the date inference).
     * @param log        The main_log logger.
     * @param bc_initial The baseline configuration before resolution.
     * @return False when PathResolver fails (logged).
     */
    bool resolveBaselineData_(const std::shared_ptr<spdlog::logger>& log,
                              const BaselineConfig& bc_initial);

    /**
     * @brief Setup: log the reference and the rovers, and check that each has a RINEX
     *        path after resolution; warn when no navigation file was resolved.
     * @param log The main_log logger.
     * @param bc  The baseline configuration after resolution.
     * @return False when a station has no RINEX path (logged).
     */
    bool checkBaselineInputs_(const std::shared_ptr<spdlog::logger>& log,
                              const BaselineConfig& bc);

    /**
     * @brief Setup: load every station's RINEX file and align the epochs.
     * @param log  The main_log logger.
     * @param sync The multi-station reader.
     * @return False when a file fails to load, the alignment fails or no epoch aligns
     *         (logged).
     */
    bool synchronizeBaselineStations_(const std::shared_ptr<spdlog::logger>& log,
                                      MultiRinexSync& sync);

    /**
     * @brief Setup: load the products and fill the product cache, which must hold SP3
     *        orbits for the satellite geometry.
     * @param log The main_log logger.
     * @return False when the cache fails to load or holds no orbit (logged).
     */
    bool loadBaselineProducts_(const std::shared_ptr<spdlog::logger>& log);

    /**
     * @brief Setup: the reference station's StateManager (its position fixed at the
     *        configured ECEF), its PppModel, and the GIM for the higher-order ionosphere
     *        when that correction is on.
     * @param ref_parser The reference station's RINEX parser.
     * @param ref_pos    The reference ECEF (the ARP, used as given).
     * @param synced     The aligned epochs (the first sets the state epoch).
     */
    void buildBaselineReference_(const RinexParser* ref_parser,
                                 const Eigen::Vector3d& ref_pos,
                                 const std::vector<SyncedEpoch>& synced);

    /**
     * @brief One rover against the reference: reset the shared reference state, check
     *        the overlap, build the rover's a-priori position, filter, AR and traces, run
     *        its epochs, then log its summaries and roll its totals into the run's.
     * @param run       The run's working value (the totals are written).
     * @param rover_idx The rover's index in the configuration.
     * @return False only when the early-abort diagnostic fired (runBaseline then
     *         returns 1); a skipped rover returns true.
     */
    bool processBaselineRover_(BaselineRunWork& run, size_t rover_idx);

    /**
     * @brief The zero-overlap guard: count the synced epochs where the rover and the
     *        reference are both present; with none, log the error and skip the rover.
     * @param run        The run's working value (rovers_with_data is counted).
     * @param rover_name The rover.
     * @return True when the rover overlaps the reference on at least one epoch.
     */
    bool roverOverlapsReference_(BaselineRunWork& run, const std::string& rover_name);

    /**
     * @brief The rover's a-priori position: the configured one, else the RINEX header's
     *        ARP (marker reduced by the antenna H/E/N offset), else zero; logs the a-priori
     *        baseline length and warns above max_baseline_km.
     * @param run        The run's working value.
     * @param rover_cfg  The rover's configuration.
     * @param rov_parser The rover's RINEX parser.
     * @return The a-priori ECEF (ARP).
     */
    Eigen::Vector3d baselineRoverApriori_(const BaselineRunWork& run,
                                          const StationConfig& rover_cfg,
                                          const RinexParser* rov_parser);

    /**
     * @brief The rover's filter: its StateManager seeded at the a-priori position, its
     *        PppModel (with the GIM when loaded), MeasurementModel and KalmanFilter.
     * @param rov_parser  The rover's RINEX parser.
     * @param rov_apriori The rover's a-priori ECEF.
     * @param synced      The aligned epochs (the first sets the state epoch).
     */
    void buildBaselineRoverFilter_(const RinexParser* rov_parser,
                                   const Eigen::Vector3d& rov_apriori,
                                   const std::vector<SyncedEpoch>& synced);

    /**
     * @brief The rover's ambiguity resolver when ambiguity_resolution.mode is above 0,
     *        else none (logged once, for the first rover).
     * @param run        The run's working value.
     * @param rover_idx  The rover's index.
     * @param rover_name The rover.
     */
    void buildBaselineRoverAr_(const BaselineRunWork& run, size_t rover_idx,
                               const std::string& rover_name);

    /**
     * @brief The rover's trace directory (log_dir_/rover_<name> on a multi-rover run) and
     *        its TraceLogger.
     * @param run        The run's working value.
     * @param rover_name The rover.
     * @return The trace directory.
     */
    std::filesystem::path openBaselineRoverTraces_(const BaselineRunWork& run,
                                                   const std::string& rover_name);

    /**
     * @brief Remove every DD ambiguity of the rover's state whose satellite or pivot is
     *        @p sat.
     * @param sat The slipped satellite.
     * @return The number of ambiguities removed.
     */
    std::size_t eraseBaselineAmbiguitiesForSat_(const std::string& sat);

    /**
     * @brief Open baseline_trace.csv in the rover's trace directory and write its header
     *        when logging.enable_state_trace is set.
     * @param run   The run's working value.
     * @param rover The rover's working value (trace_ofs and trace_path are written).
     */
    void openBaselineTraceCsv_(const BaselineRunWork& run, BaselineRoverWork& rover);

    /**
     * @brief One epoch of one rover, as an ordered list of stages.
     * @param run   The run's working value.
     * @param rover The rover's working value.
     * @param se    The synced epoch.
     * @return What the rover's epoch loop does next.
     */
    BaselineEpochAction processBaselineEpoch_(const BaselineRunWork& run,
                                              BaselineRoverWork& rover,
                                              const SyncedEpoch& se);

    /**
     * @brief The processing.max_duration_s cutoff: the rover's first synced epoch anchors
     *        the window; an epoch whose span reaches the limit stops the rover (logged).
     * @param run   The run's working value.
     * @param rover The rover's working value (the anchor is written).
     * @param se    The synced epoch.
     * @return True when the rover's epoch loop stops here.
     */
    bool reachedBaselineMaxDuration_(const BaselineRunWork& run, BaselineRoverWork& rover,
                                     const SyncedEpoch& se);

    /**
     * @brief Find the rover and the reference in the synced epoch and compute the time
     *        step from the rover's last processed epoch (week rollover included).
     * @param run   The run's working value.
     * @param rover The rover's working value.
     * @param ep    The epoch's working value; writes rov_it, ref_it, both_present and dt.
     */
    void locateBaselineStations_(const BaselineRunWork& run, const BaselineRoverWork& rover,
                                 BaselineEpochWork& ep);

    /**
     * @brief The hard-gap reset (both stations present and dt above the data-gap
     *        tolerance, 60 s by default): drop every DD ambiguity, forget both MW arcs and
     *        the sticky DD pivots, then the dynamics-mode policy of @ref handleGap.
     * @param run   The run's working value.
     * @param rover The rover's working value (n_amb_resets is counted).
     * @param ep    The epoch's working value.
     */
    void resetAtBaselineDataGap_(const BaselineRunWork& run, BaselineRoverWork& rover,
                                 const BaselineEpochWork& ep);

    /**
     * @brief The MW pre-filter on both stations, before the DD build: a slipped
     *        satellite's DD ambiguities are erased, its MW arcs reset at both receivers,
     *        and the DD pivots re-ranked.
     * @param run   The run's working value.
     * @param rover The rover's working value (the slip and reset counters are written).
     * @param ep    The epoch's working value.
     */
    void screenBaselineMwSlips_(const BaselineRunWork& run, BaselineRoverWork& rover,
                                const BaselineEpochWork& ep);

    /**
     * @brief Predict the rover's state, advance its previous-epoch markers, sync both
     *        stations' PppStates and model both stations' observations.
     * @param run   The run's working value.
     * @param rover The rover's working value (prev_week, prev_tow and the epoch counters
     *              are written).
     * @param ep    The epoch's working value; writes rov_modeled and ref_modeled.
     * @return False when modelling fails or leaves a station empty (the epoch is skipped).
     */
    bool modelBaselineEpoch_(const BaselineRunWork& run, BaselineRoverWork& rover,
                             BaselineEpochWork& ep);

    /**
     * @brief The rebuild-pass update loop: build the DD measurement, update the filter,
     *        drain the KF's pivot-handover requests, and on NeedsMeasurementRebuild
     *        exclude the flagged rows and rebuild, within the shared pass budget.
     * @param run   The run's working value.
     * @param rover The rover's working value (n_updates is counted).
     * @param ep    The epoch's working value; writes em, carry_excluded, built_count,
     *              applied, committed and epoch_failed.
     */
    void runBaselineUpdateLoop_(const BaselineRunWork& run, BaselineRoverWork& rover,
                                BaselineEpochWork& ep);

    /**
     * @brief One pass's DD measurement, with its exception envelope.
     * @param run  The run's working value.
     * @param ep   The epoch's working value (em is written).
     * @param pass The pass index.
     * @return False when computeMeasurementUpdate throws (logged).
     */
    bool buildBaselineDdPass_(const BaselineRunWork& run, BaselineEpochWork& ep, int pass);

    /**
     * @brief Drain the KalmanFilter's coherent-pivot-slip requests and force a DD pivot
     *        re-rank on each flagged constellation, excluding the KF's replacement
     *        exclusions.
     * @param log The main_log logger.
     */
    void drainBaselinePivotHandovers_(const std::shared_ptr<spdlog::logger>& log);

    /**
     * @brief On a committed epoch: the per-observable DD residual statistics and pivot
     *        counts, DD ambiguity resolution, and the CSV traces.  No-op otherwise.
     * @param run   The run's working value.
     * @param rover The rover's working value (the residual statistics are written).
     * @param ep    The epoch's working value.
     */
    void recordCommittedBaselineEpoch_(const BaselineRunWork& run, BaselineRoverWork& rover,
                                       const BaselineEpochWork& ep);

    /**
     * @brief The early-abort diagnostic after 20 epochs with DD observations and no
     *        update (a product window mismatch): logs, closes the position trace.  As
     *        written it cannot fire (n_geom_ok counts with n_with_dd).
     * @param run   The run's working value.
     * @param rover The rover's working value (the trace is closed).
     * @param ep    The epoch's working value.
     * @return True when it fired; runBaseline then returns 1.
     */
    bool abortBaselineWithoutUpdates_(const BaselineRunWork& run, BaselineRoverWork& rover,
                                      const BaselineEpochWork& ep);

    /**
     * @brief One row of baseline_trace.csv: the rover position, its sigmas, the rows
     *        applied and built, the ambiguity count.
     * @param rover The rover's working value (the trace).
     * @param ep    The epoch's working value.
     */
    void writeBaselineTraceRow_(BaselineRoverWork& rover, const BaselineEpochWork& ep);

    /**
     * @brief Close the rover's baseline_trace.csv and log its path.
     * @param run   The run's working value.
     * @param rover The rover's working value.
     */
    void closeBaselineTraceCsv_(const BaselineRunWork& run, BaselineRoverWork& rover);

    /**
     * @brief The rover's DD build summary: epochs, DD scalars per observable and
     *        constellation, the dominant pivots, the residual statistics.
     * @param run   The run's working value.
     * @param rover The rover's working value.
     */
    void reportBaselineDdBuild_(const BaselineRunWork& run, const BaselineRoverWork& rover);

    /**
     * @brief The rover's solution summary: updates, final position and sigma, the
     *        displacement from the a-priori, the baseline length, the slip counters.
     * @param run   The run's working value.
     * @param rover The rover's working value.
     */
    void reportBaselineSolution_(const BaselineRunWork& run, const BaselineRoverWork& rover);

    /**
     * @brief The run's aggregate summary (multi-rover runs), the closing line and the
     *        KalmanFilter's timings.
     * @param run The run's working value.
     */
    void reportBaselineAggregate_(const BaselineRunWork& run);

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
     * @brief One synced epoch through the network producer.
     *
     * The admission (start offset, event injector, canonical-pair screen), the NetworkEpoch,
     * then the stages in spine order: roster, admission tail (broadcast feed, maximum
     * duration, decimation), epoch top, the slip funnel, PASS A and the screens, the phase
     * datum, the columns, pre-assembly, PASS B, the DD build, the solve, the
     * install, AR and fixed cycles, solution prep, emit, the post-fit diagnostics, RAIM,
     * clock QC, the measure-only instruments, the stash, the D0 readout and the snapshot.
     * Four stages end the epoch early (networkPassAHead_, networkAssemblyRegisterColumns_,
     * networkPassB_, networkSolveGlue), the admission three ways.
     * Implemented in ApplicationNetworkEpoch.cpp: written by scripts/nom_ep1_cut.py,
     * maintained by hand since, guarded by scripts/nom_epoch_lint.py.
     *
     * @param run    The run, built once by runNetwork after the traces.
     * @param se_raw The synced epoch as the source delivered it.
     * @return LoopSignal::Stop at the maximum-duration cutoff, else LoopSignal::Next.
     */
    LoopSignal networkProcessEpoch_(NetworkRun& run, const SyncedEpoch& se_raw);

    friend struct NetworkEpochContractProbe;   // tests/network/test_network_epoch_contract.cpp reads the five private seam types

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
     * non-movable (see SetupResult.hpp).  Each fatal failure logs and
     * returns 1; the caller forwards the status.  Implemented in
     * @c ApplicationNetworkSetup.cpp.
     *
     * @param setup Out-parameter, default-constructed by the caller.
     * @return 0 ok, 1 on any fatal setup failure (after logging).
     */
    int networkSetup_(NetworkSetupResult& setup);

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
     * Epoch form (generated by scripts/nom_ep1_cut.py): the run and the epoch
     * replace the explicit parameters; the definition binds each former parameter in its preamble,
     * under its former name.
     *
     * @param run The run (network/driver/Run.hpp). Written through non-const bindings: run.ctx (ctx),
     *            run.setup (setup), run.traces (traces).
     * @param ep The epoch (network/driver/Epoch.hpp), read only. Bindings: ep.se (se).
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
     *        the serial slip-QC drains declared in network/slip/SlipQc.hpp: T0
     *        repair, MW / LLI refloat, IF-TD driver, wind-up restart, gap re-entry
     *        and injected-arc free (decomposition INC-7).
     *
     * The caller brackets the call with networkSubBegin/End(SB_SLIP), the D0b section
     * timer for the slip scans.  @p carry_slipped_ambs receives, by non-const
     * reference, the (station index, ambiguity key) pairs the drains flag this
     * epoch.  The driver passes its raw-detector set carry_slipped_raw here and
     * unions it into the per-epoch slip-event funnel carry_slipped_ambs (Card-4 C3c)
     * right after the call; a copy would drop the drains' inserts and leave the DD
     * carry without its slip events (standing invariant #1).
     * Implemented in @c ApplicationNetworkAssembly.cpp.
     *
     * Epoch form (generated by scripts/nom_ep1_cut.py): the run and the epoch
     * replace the explicit parameters; the definition binds each former parameter in its preamble,
     * under its former name.
     *
     * @param run The run (network/driver/Run.hpp). Written through non-const bindings: run.ctx (ctx),
     *            run.setup (setup), run.traces (traces), run.engines.slip_qc (qc).
     * @param ep The epoch (network/driver/Epoch.hpp), read only. Bindings: ep.se (se), ep.clock.dt
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
     * @param events The epoch's events; written: slip (relevel, sid_reentry_outcome).
     */
    void networkPassASlipScan_(NetworkRun& run, const NetworkEpoch& ep, EpochPassA& pass_a,
                               NetworkSlipFunnel& funnel, EpochEvents& events);

    /**
     * @brief Raw slip-observable trace rows and the cross-station observability
     *        pre-filter (decomposition INC-7).  The rows are written before the
     *        prune, so a satellite about to be dropped is still captured.  Prunes
     *        single-station satellites from @p modeled and from the per-station
     *        shared states; @p n_dropped_sats receives their count for the epoch.
     *        Implemented in @c ApplicationNetworkAssembly.cpp.
     *
     * Epoch form (generated by scripts/nom_ep1_cut.py): the run and the epoch
     * replace the explicit parameters; the definition binds each former parameter in its preamble,
     * under its former name.
     *
     * @param run The run (network/driver/Run.hpp). Written through non-const bindings: run.ctx (ctx),
     *            run.setup (setup), run.traces (traces). Read: run.engines.slip_qc (qc).
     * @param ep The epoch (network/driver/Epoch.hpp), read only. Bindings: ep.se (se),
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
     * without calling networkSubEnd(SB_PREASM).  The epoch counts are members of
     * @p columns.  Implemented in
     * @c ApplicationNetworkAssembly.cpp.
     *
     * Epoch form (generated by scripts/nom_ep1_cut.py): the run and the epoch
     * replace the explicit parameters; the definition binds each former parameter in its preamble,
     * under its former name.
     *
     * @param run The run (network/driver/Run.hpp). Written through non-const bindings: run.ctx (ctx),
     *            run.setup (setup), run.traces (traces), run.engines.est (est).
     * @param ep The epoch (network/driver/Epoch.hpp), read only. Bindings: ep.se (se),
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
     * are members of the epoch's assembly group (ep.assembly),
     * filled here for the solve, RAIM and emit
     * seams downstream; @p sec_asm_ms_epoch / @p sec_asm_wall_ms receive the D0
     * wall time, which accumulates even when the epoch skips.  The SH-deviation carry's
     * switches (sh_dev_tau, sh_dev_on, sh_dev_demean, sh_dev_d5_keep) are read from
     * @c ctx.knobs.
     * Implemented in @c ApplicationNetworkAssembly.cpp.
     *
     * Epoch form (generated by scripts/nom_ep1_cut.py): the run and the epoch
     * replace the explicit parameters; the definition binds each former parameter in its preamble,
     * under its former name.
     *
     * @param run The run (network/driver/Run.hpp). Written through non-const bindings: run.ctx (ctx),
     *            run.setup (setup), run.traces (traces), run.engines.est (est),
     *            run.timing.asm_wall_ms (sec_asm_wall_ms).
     * @param ep The epoch (network/driver/Epoch.hpp), read only. Bindings: ep.se (se),
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
    bool sif_apriori_seeded_{ false };        /**< True when @ref initializePosition seeded @c initial_position_ from the SIF (ARP-reduced); initializePppState_ then re-applies it after PppModel::initializeState, which re-derives the position from the RINEX header. */

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