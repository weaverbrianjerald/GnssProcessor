#pragma once


#include "config/GnssConfigTypes.hpp"   // enums + POD config structs (relocated)

#include "logging/LogManager.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <nlohmann/json_fwd.hpp>        // only the loadXxxSection_ decls need json; the
                                        // full <nlohmann/json.hpp> lives in GnssConfigLoad.cpp
#include <set>

namespace GnssProcessor {


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
        GnssConfig();   // defined in GnssConfig.cpp; member defaults live there

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
         * @brief Gets the troposphere a priori source.
         *
         * Controls whether @c Troposphere::computeSaastamoinenZHD uses
         * the frozen sea-level pressure/temperature pair
         * (@c StandardAtmosphere) or queries the GPT3 5° grid at the
         * receiver lat/lon/h/doy (@c Gpt3).
         */
        TroposphereAprioriSource getTroposphereAprioriSource() const { return tropo_apriori_source_; }

        /**
         * @brief Path to the GPT3 5° grid file (@c gpt3_5.grd).
         *
         * Empty if not configured.  Required when
         * @c getTroposphereAprioriSource() == @c TroposphereAprioriSource::Gpt3.
         */
        const std::filesystem::path& getGpt3GridPath() const { return gpt3_grid_path_; }

        /**
         * @brief Set the GPT3 grid path (used by @c PathResolver to inject
         *        the auto-discovered location in internal mode).
         */
        void setGpt3GridPath(const std::filesystem::path& p) { gpt3_grid_path_ = p; }

        /**
         * @brief Gets the tropospheric mapping function selection.
         */
        TroposphereMappingFunction getTroposphereMappingFunction() const { return tropo_mapping_function_; }

        /**
         * @brief Gets the processing mode.
         * @return Current processing mode (PPP or PPP-RTK).
         */
        ProcessingMode getProcessingMode() const { return processing_mode_; }

        /**
         * @brief Gets the KF frequency mode (band scope).
         * @return Current FrequencyMode (Dual, Triple, or All).
         */
        FrequencyMode getFrequencyMode() const { return frequency_mode_; }

        /**
         * @brief Gets the KF channel mode (tracking-attribute multiplicity).
         * @return Current ChannelMode (Single or All).
         */
        ChannelMode getChannelMode() const { return channel_mode_; }

        /**
         * @brief Gets the MW slip detector frequency mode (raw, including Mirror sentinel).
         *
         * Prefer @c getResolvedMwFrequencyMode at call sites that need a
         * concrete @c FrequencyMode — this raw form is for diagnostics.
         */
        MwFrequencyMode getMwFrequencyMode() const { return mw_frequency_mode_; }

        /**
         * @brief Gets the MW slip detector channel mode (raw, including Mirror sentinel).
         *
         * Prefer @c getResolvedMwChannelMode at call sites that need a
         * concrete @c ChannelMode.
         */
        MwChannelMode getMwChannelMode() const { return mw_channel_mode_; }

        /**
         * @brief Resolve the MW frequency mode against the KF setting.
         *
         * Returns the concrete @c FrequencyMode the MW detector should
         * use this run: if @c mw_frequency_mode_ is @c Mirror, returns
         * the KF's @c frequency_mode_; otherwise returns the
         * corresponding @c FrequencyMode for the override value.
         */
        FrequencyMode getResolvedMwFrequencyMode() const {
            switch (mw_frequency_mode_) {
            case MwFrequencyMode::Dual:   return FrequencyMode::Dual;
            case MwFrequencyMode::Triple: return FrequencyMode::Triple;
            case MwFrequencyMode::All:    return FrequencyMode::All;
            case MwFrequencyMode::Mirror:
            default:                      return frequency_mode_;
            }
        }

        /**
         * @brief Resolve the MW channel mode against the KF setting.
         */
        ChannelMode getResolvedMwChannelMode() const {
            switch (mw_channel_mode_) {
            case MwChannelMode::Single: return ChannelMode::Single;
            case MwChannelMode::All:    return ChannelMode::All;
            case MwChannelMode::Mirror:
            default:                    return channel_mode_;
            }
        }

        /**
         * @brief First-epoch sanity-check threshold for BIA-corrected MW
         *        arcs, in wide-lane cycles.
         *
         * On the very first sample of a fresh BIA-corrected arc, the MW
         * value should land within this distance of an integer × λ_WL —
         * otherwise the arc is presumed to have been initialized with a
         * slip already baked in (e.g. a slip occurred during a prior
         * data gap that triggered the reset) and an immediate ambiguity
         * reset is forced.
         *
         * Default 0.4 cycles is appropriate for low-noise code
         * observations (e.g. P(Y) tracking, L1W/L2W).  Code-noisier
         * combinations (e.g. C/A on L1C, ~3× the P(Y) noise) easily
         * exceed 0.4 cycles on a single sample purely from code noise,
         * triggering spurious slip detections at every arc start.  For
         * L1C-heavy runs, raise to 1.0–2.0 cycles to suppress the
         * false positives; the lock criterion downstream still vets the
         * arc statistically once enough samples accumulate.  The MW
         * detector additionally lifts this floor by 3σ of the cross-sat
         * MAD when one is available for the (constellation × signal-
         * pair) group.
         *
         * @return Threshold in WL cycles.
         */
        double getMwInitFracThreshold() const { return mw_init_frac_threshold_; }

        /**
         * @brief EWMA smoothing factor for the MW cross-satellite σ floor.
         *
         * The cross-satellite MAD that floors the MW slip-detection threshold
         * during arc warmup is recomputed every epoch from a thin
         * cross-section (one deviation per satellite per band-pair group), so
         * the raw per-epoch estimate is statistically jumpy. Because the MW
         * noise *envelope* varies slowly, each cross-sat tier is updated as an
         * exponentially-weighted moving average instead of overwritten:
         * @f$\sigma_t = \alpha\,\mathrm{MAD}_{epoch} + (1-\alpha)\,\sigma_{t-1}@f$,
         * with the first finite MAD as the bootstrap.
         *
         * Smaller @f$\alpha@f$ = heavier smoothing / slower adaptation; the
         * default 0.1 (~10-sample memory) suppresses the per-epoch jitter while
         * still tracking real noise-level drift over a session. Must lie in
         * (0, 1]; 1.0 disables smoothing (raw per-epoch MAD). Only affects the
         * warmup floor — mature arcs gate on their own per-arc MAD regardless.
         *
         * Configured via @c processing.mw_cross_sat_ewma_alpha.
         *
         * @return EWMA factor in (0, 1].
         */
        double getMwCrossSatEwmaAlpha() const { return mw_cross_sat_ewma_alpha_; }

        /**
         * @brief Whether the MW detector estimates per-(sat, signal-pair)
         *        phase biases in-house when external BIA is unavailable.
         *
         * When enabled and an arc reaches a tight statistical confidence
         * with its running mean within a small fraction of an integer cycle,
         * the residual is committed as an estimated bias and persisted across
         * future arcs of the same (satellite, signal-pair).  Subsequent
         * arcs apply the stored bias to the incoming MW samples and become
         * eligible for integer locking.  Effectively a self-calibrating
         * substitute for an external phase BIA product, suitable for
         * ultra-long EWL pairs (e.g. Galileo L5X/L8X at λ ≈ 19.5 m) whose
         * inter-satellite bias is small relative to the wavelength.
         *
         * Default off.
         *
         * @return True if in-house bias estimation is enabled.
         */
        bool getInhouseBiasEstimation() const { return enable_inhouse_bias_estimation_; }

        /**
         * @brief Get the ambiguity-parameterization basis for the Kalman state.
         *
         * @c PerBand keeps one ambiguity per (sat, signal/IF combo);
         * @c NlWl decomposes each dual-frequency ambiguity into a
         * narrow-lane state and a wide-lane state per band-pair.  See
         * @c StateAmbiguityBasis for the rationale.
         *
         * @return Current basis (default @c PerBand).
         */
        StateAmbiguityBasis getStateAmbiguityBasis() const { return state_ambiguity_basis_; }

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
         * @brief Whether satellites in Earth's umbral shadow should be excluded.
         *
         * During eclipse seasons GNSS satellites perform block-specific
         * midnight / noon-turn yaw manoeuvres that this codebase does not
         * model (we have no yaw-input data plumbed in).  With this flag
         * enabled, @c ModelGeometry::computeSatelliteGeometry rejects any
         * satellite whose ECEF position falls inside the cylindrical
         * Earth–Sun shadow, surfaced as @c DropReason::Eclipsing in the
         * satellite-status trace.
         *
         * Configured via @c processing.exclude_eclipsing_satellites.
         * Default false to preserve legacy behavior.
         *
         * @return True if eclipsing-satellite exclusion is enabled.
         */
        bool excludeEclipsingSatellites() const { return exclude_eclipsing_satellites_; }

        /**
         * @brief Shadow-cylinder radius multiplier for the eclipse test.
         *
         * Multiplier applied to @c Constants::EARTH_RADIUS in
         * @c Planetary::isSatelliteEclipsing.  A value of 1.0 corresponds
         * to the purely geometric umbra; values slightly greater than 1.0
         * widen the rejection region to also catch satellites that are
         * within the post-shadow yaw-recovery window.
         *
         * Configured via @c processing.eclipse_shadow_scale.
         * Default 1.0.
         *
         * @return Shadow-cylinder scale factor (unitless, >0).
         */
        double getEclipseShadowScale() const { return eclipse_shadow_scale_; }

        /**
         * @brief Gets the data gap tolerance in seconds.
         *
         * When the elapsed time between consecutive epochs exceeds this
         * threshold the filter treats the gap as a data outage and
         * re-seeds the receiver clock from WLS.
         *
         * A value of 0 (the default) means "use the measurement rate"
         * � i.e. any epoch spacing larger than the nominal interval is
         * treated as a gap.
         *
         * @return Gap tolerance in seconds (0 = measurement-rate default).
         */
        double getDataGapTolerance() const { return data_gap_tolerance_s_; }

        /**
         * @brief Gets the long-gap tolerance in seconds.
         *
         * Threshold separating "short" from "long" gaps for the gap
         * recovery policy in @c Application::handleGap.  Short gaps
         * (between @c getDataGapTolerance and this value) only zero
         * stale sat-specific cross-covariances; long gaps additionally
         * drop ionosphere / ambiguity / sat-bias state and force fresh
         * BSSD/DD pivot election.
         *
         * Default 300 s, matching the sat-bias cache warm window
         * @c getSatBiasCacheWarmAgeS so that "long gap" coincides with
         * "sat-bias cache no longer trusted at full variance."
         *
         * Configured via @c processing.long_gap_tolerance_s.
         *
         * @return Long-gap tolerance in seconds.
         */
        double getLongGapTolerance() const { return long_gap_tolerance_s_; }

        /**
         * @brief Gets the effective GNSS processing rate in seconds.
         *
         * When greater than zero, the epoch loop decimates the input
         * observations: it processes the first epoch, then skips any
         * subsequent epoch whose time is less than @c gnss_rate_s after
         * the last processed epoch.  This lets a 30 s RINEX file be
         * processed at (for example) 300 s for static PPP, analogous to
         * the GIPSY-X sampling option.
         *
         * A value of 0 (the default) disables decimation: every epoch in
         * the RINEX file is processed.  Post-processing only; real-time
         * mode ignores this setting.
         *
         * Downsampling also relaxes the data-gap threshold and the
         * "unexpected dt" warning so that the intentionally larger
         * step between processed epochs is not flagged as an outage.
         *
         * @return Processing rate in seconds (0 = disabled).
         */
        double getGnssRate() const { return gnss_rate_s_; }

        /**
         * @brief Gets the maximum processing duration in seconds.
         *
         * When greater than zero, the epoch loop stops once the data-time
         * span from the first processed epoch reaches this value.  The span
         * is measured as GPS epoch time relative to the first processed
         * observation, NOT wall-clock time — so a 3600 s limit on 1 s data
         * yields one hour (≈3600 epochs) regardless of how fast the file
         * replays or the stream arrives.  Anchored on the first processed
         * epoch, so any leading decimation skip (post-processing) or
         * corrections-ready warmup defer (real-time) does not count against
         * the budget.
         *
         * A value of 0 (the default) disables the cutoff: processing runs to
         * RINEX EOF (post-processing) or until the stream ends / the user
         * interrupts (real-time).  Honoured by post-processing PPP/PPP-RTK
         * (@c Application::run), baseline (@c Application::runBaseline), and
         * the real-time engine (@c RealTimeEngine::processEpoch).
         *
         * Configured via @c processing.max_duration_s.
         *
         * @return Maximum processing duration in seconds (0 = unlimited).
         */
        double getMaxDuration() const { return max_duration_s_; }

        /**
         * @brief Real-time obs skip-to-latest master toggle.
         *
         * When true (real-time only; post-processing ignores it), the engine
         * drops observation epochs that trail wall-clock GPS time by more than
         * @c obsSkipMaxLagS, fast-forwarding the filter to the freshest epoch so
         * a lagging obs stream cannot be processed against fresher
         * orbit/clock/SSR corrections.  Default off — opt-in, and assumes an
         * NTP-synced host (the lag is measured against the system clock).
         * Configured via @c processing.enable_obs_skip_to_latest.
         */
        bool enableObsSkipToLatest() const { return enable_obs_skip_to_latest_; }

        /// @brief Max obs lag (s) behind wall-clock GPS before skip-to-latest
        ///        drops the epoch (real-time only).  Config @c processing.obs_skip_max_lag_s.
        double obsSkipMaxLagS() const { return obs_skip_max_lag_s_; }

        /// @brief Warn-only real-time obs-backlog threshold (s); ≤0 disables the
        ///        warning (e.g. deliberate file replay).  Config @c processing.rt_backlog_warn_s.
        double rtBacklogWarnSeconds() const { return rt_backlog_warn_s_; }

        /// @brief Max |obs − SSR-record| age (s) before the SSR orbit/clock
        ///        overlay is rejected and the satellite falls back to
        ///        broadcast-grade (real-time).  Config @c processing.ssr_max_age_s.
        double getSsrMaxAge() const { return ssr_max_age_s_; }

        /**
         * @brief Get the enabled RINEX observation-type letters.
         *
         * Returns the (uppercase, deduplicated) set of RINEX first-character
         * observation type codes that the parser will retain.  Codes whose
         * first character is not in this set are dropped at parse time and
         * never reach the downstream pipeline.
         *
         * The four legal RINEX letters are:
         *  - @c 'C' : pseudorange (code)
         *  - @c 'L' : carrier phase
         *  - @c 'D' : Doppler
         *  - @c 'S' : signal strength (SNR / C/N0)
         *
         * Default is @c "CL" — code + phase only, matching what the PPP /
         * PPP-RTK / baseline estimators actually consume.  Add @c 'D' to
         * retain Doppler (needed for @c estimation.estimate_velocity), or
         * @c 'S' to retain SNR samples (used by the SNR stochastic model
         * and by some logging traces).
         *
         * Configured via @c processing.rinex_types as a string of letters
         * in any order or case (e.g. @c "CLDS", @c "cl", @c "CLD").
         *
         * @return String containing the enabled type letters.
         */
        const std::string& getRinexTypes() const { return rinex_types_; }

        /**
         * @brief Check whether a RINEX observation-type letter is enabled.
         *
         * @param t First character of a RINEX 3 observation code
         *          (case-sensitive uppercase letter).
         * @return True if @p t is one of the letters in @c getRinexTypes().
         */
        bool isRinexTypeEnabled(char t) const {
            return rinex_types_.find(t) != std::string::npos;
        }

        /**
         * @brief Get the list of satellites excluded from processing.
         * @return Vector of satellite identifiers (e.g. "E14", "E18").
         */
        const std::vector<std::string>& getExcludedSatellites() const { return excluded_satellites_; }

        /**
         * @brief Check whether a satellite is excluded from processing.
         * @param satellite Satellite identifier (e.g. "E14").
         * @return true if the satellite should be skipped.
         */
        bool isSatelliteExcluded(const std::string& satellite) const {
            return std::find(excluded_satellites_.begin(), excluded_satellites_.end(), satellite)
                != excluded_satellites_.end();
        }

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
         * @brief Gets the model correction enable/disable flags.
         * @return Const reference to the ModelCorrections struct.
         */
        const ModelCorrections& getModelCorrections() const { return model_corrections_; }

        /**
         * @brief Gets the stochastic (weighting) model selection.
         *
         * The selected model controls the **code** weighting in full;
         * phase weighting is model-specific:
         *  - @c Constant       : phase = default_var (no scaling).
         *  - @c Elevation      : phase = default_var / sin²(el).
         *  - @c SNR            : variance-additive σ² with Wieser
         *                        elevation amplification.
         *  - @c SatelliteBlock : phase = phase_std_m² / sin²(el)
         *                        — the block table and per-band
         *                        chip-rate scaling are intentionally
         *                        bypassed for phase because the
         *                        per-block carrier-loop noise
         *                        differential is below the
         *                        multipath/atmospheric noise floor in
         *                        practice.  Code keeps the full
         *                        block + freq + elevation pipeline.
         *
         * @return Current StochasticModelType.
         */
        StochasticModelType getStochasticModel() const { return stochastic_model_; }

        /**
         * @brief σ-power for elevation amplification on code observations.
         *
         * The stochastic models apply elevation amplification of the
         * form
         *   σ_actual = σ_zen / sin^p(elev)
         *   var_actual = var_zen / sin^(2p)(elev)
         *
         * where @c p is this returned value for code rows and
         * @c getPhaseElevationPower() for phase.  The legacy
         * 1/sin²(elev) variance scaling corresponds to @c p = 1.0
         * (the default for code).  Code multipath and tropospheric
         * path-length effects on pseudorange both scale roughly as
         * csc²(θ), so 1.0 is the right default.
         *
         * Configurable via @c filter.code_elevation_power.
         */
        double getCodeElevationPower() const { return code_elevation_power_; }

        /**
         * @brief σ-power for elevation amplification on phase observations.
         *
         * Defaults to 0.5, giving a 1/sin(elev) variance scaling
         * (gentler than code's 1/sin²(elev)).  Justification: phase
         * noise at low elevation is dominated by carrier-loop SNR
         * (which falls slowly with elevation, not as csc²) and
         * antenna PCV miscalibration — neither follows the csc²(θ)
         * mapping function that drives code multipath.  At a 10°
         * mask, p=0.5 amplifies σ by 2.4× rather than 5.8×, keeping
         * standardized-residual cycle-slip detection effective at
         * low elevation.
         *
         * Setting @c phase_elevation_power = 1.0 in the JSON
         * recovers the legacy symmetric csc²(θ) phase weighting.
         *
         * Configurable via @c filter.phase_elevation_power.
         */
        double getPhaseElevationPower() const { return phase_elevation_power_; }

        /**
         * @brief Gets the SNR-based stochastic model parameters.
         * @return Const reference to the SnrModelParams struct.
         */
        const SnrModelParams& getSnrModelParams() const { return snr_model_params_; }

        /**
         * @brief Gets the satellite-block stochastic model parameters.
         * @return Const reference to the SatBlockNoiseParams struct.
         */
        const SatBlockNoiseParams& getSatBlockNoiseParams() const { return sat_block_noise_params_; }

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
         * @brief Gets the ocean loading BLQ file path.
         * @return Path to the BLQ file (empty if not configured).
         */
        std::filesystem::path getOtlBlqPath() const { return otl_blq_path_; }

        /**
         * @brief Gets the number of leap seconds.
         * @return Leap seconds value.
         */
        double getLeapSeconds() const { return leap_seconds_tai_; }

        /**
         * @brief Check if tropospheric azimuthal gradient estimation is enabled.
         * @return True if gradient states (Gn, Ge) are estimated.
         */
        bool estimateTropoGradients() const { return estimate_tropo_gradients_; }

        /**
         * @brief Check if the backward RTS smoother is enabled.
         *
         * When true, the application runs a forward Kalman pass, buffers
         * per-epoch state/covariance snapshots, and then executes a
         * Rauch-Tung-Striebel backward recursion to produce a smoothed
         * trajectory. The smoother output is written to a separate
         * CSV trace; the forward solution is left unchanged.
         *
         * Post-processing only; ignored in real-time mode.
         *
         * @return True if the backward smoother should run after the forward pass.
         */
        bool runSmoother() const { return run_smoother_; }

        /**
         * @brief Check if geometry relinearization is enabled.
         *
         * When true, the epoch loop will re-run computeModels() once after the
         * first Kalman update to recompute satellite geometry (orbits, clocks,
         * LOS, corrections) at the updated receiver position, then perform a
         * second measurement update.  This eliminates the first-order
         * linearization error inherent in a single-pass EKF update and can
         * improve convergence during early epochs or after large position jumps.
         *
         * @return True if one geometry relinearization pass is enabled.
         */
        bool relinearizeGeometry() const { return relinearize_geometry_; }

        /**
         * @brief Threshold on |Δpos| for relinearization when at least one
         *        phase observation is available in the epoch.  Default 1.0 m.
         *
         * Phase rows pin the position state tightly — once the filter has
         * converged, |Δpos| drops to sub-cm and the second-pass H matrix
         * is essentially identical to the first.  Even a relaxed 1 m
         * threshold relinearizes only during early convergence.
         */
        double relinearizeDposThresholdPhaseM() const { return relinearize_dpos_threshold_phase_m_; }

        /**
         * @brief Threshold on |Δpos| for relinearization in code-only mode
         *        (no phase observables in the epoch).  Default 100.0 m.
         *
         * SPP-style code-only solutions exhibit ~5 m epoch-to-epoch
         * jitter even after convergence, so a low threshold would fire
         * every epoch.  100 m skips relinearization on routine epochs
         * and only fires on truly large jumps (e.g. cold start).
         */
        double relinearizeDposThresholdCodeOnlyM() const { return relinearize_dpos_threshold_code_only_m_; }

        /**
         * @brief Hard cap on relinearization passes per run.  -1 (default)
         *        means unlimited.
         *
         * Safety net for noisy modes (code-only single-frequency with ~5 m
         * SPP-style position jitter) where the |Δpos| threshold would fire
         * every epoch and we'd be paying double the per-epoch cost
         * indefinitely.  After the cap is reached, relinearization is
         * silently skipped for the rest of the run.
         */
        int relinearizeMaxCount() const { return relinearize_max_count_; }

        /**
         * @brief Number of threads Eigen should use for dense matmul / solve.
         *
         * 0 → leave the OpenMP runtime default (typically the core count).
         * 1 → single-threaded (recommended for small-matrix workloads where
         *      fork/join overhead exceeds the parallel speedup; PPP-RTK
         *      with n≈350, m≈250 falls in this range).
         * N → pin to N threads.
         *
         * Applied once during @c Application::run() via
         * @c Eigen::setNbThreads().  Build still links OpenMP so threading
         * can be re-enabled at runtime by editing the config — no rebuild.
         */
        int eigenThreads() const { return eigen_threads_; }

        /**
         * @brief Check if separate phase clock estimation is enabled.
         *
         * When true, a separate receiver phase clock offset state is estimated
         * in addition to the common receiver clock. The common clock enters all
         * measurements (code and phase) and the phase clock offset is an additive
         * delta for phase measurements only: phase = common_clock + delta.
         * This "common clock + delta" parameterization avoids rank deficiency.
         *
         * @return True if phase clock offset estimation is enabled.
         */
        bool estimatePhaseClock() const { return estimate_phase_clock_; }

        /**
         * @brief Check if the IF phase slip detector is enabled.
         *
         * When true, the between-satellite, between-epoch IF phase OMC
         * double-difference detector runs each epoch to catch small cycle
         * slips (1-2 IF cycles) that the MW detector misses.
         *
         * @return True if the IF phase slip detector is enabled.
         */
        bool enableIfSlipDetector() const { return enable_if_slip_detector_; }

        /**
         * @brief Check if accumulated (unwrapped) phase wind-up is enabled.
         *
         * When false (default), the phase wind-up correction is computed as the
         * fractional-cycle geometric angle at each epoch, bounded to �half cycle.
         * The float ambiguity absorbs any integer-cycle component.
         *
         * When true, the wind-up angle is accumulated across epochs using 2?
         * unwrapping so that the correction is continuous.  This is required
         * for integer ambiguity resolution (IAR) where the ambiguity must
         * remain at a constant integer value.  Accumulation chains
         * epoch-to-epoch; a data gap exceeding 5 minutes restarts it from
         * the raw angle.  A restart cannot bias the solution: an absent
         * satellite's ambiguity is pruned and re-seeded fresh on
         * re-acquisition, absorbing whatever wind-up value is current.
         *
         * @return True if accumulated phase wind-up is enabled.
         */
        bool accumulatePhaseWindUp() const { return accumulate_phase_windup_; }

        /**
         * @brief Check if receiver velocity estimation from Doppler is enabled.
         *
         * When true, three velocity states (Vx, Vy, Vz) are added to the
         * Kalman filter state vector and Doppler ('D') measurements are
         * used for updates.  This requires that the RINEX data contain
         * Doppler observables.
         *
         * @return True if velocity estimation is enabled.
         */
        bool estimateVelocity() const { return estimate_velocity_; }

        /**
         * @brief Check if between-satellite single differencing is enabled.
         *
         * When true, the measurement model applies a between-satellite
         * single-difference (BSSD) operator to the design matrix, innovation
         * vector and measurement noise.  For each constellation the
         * highest-elevation satellite is chosen as the pivot; all other
         * satellites of the same constellation are differenced against it.
         * This eliminates the receiver clock (and ISB) from the observation
         * equations, improving observability of ambiguities and ionosphere
         * states and accelerating PPP-RTK convergence.
         *
         * The differencing is applied in the state space (operator D premultiplies
         * H, z and R) so the state vector parameterization is unchanged.
         *
         * @return True if BSSD is enabled.
         */
        bool enableBetweenSatelliteSd() const { return enable_between_satellite_sd_; }

        /**
         * @brief Allow GLONASS satellites to participate in BSSD.
         *
         * GLONASS uses FDMA — each satellite has its own carrier frequency
         * — so the receiver hardware delay differs per-channel and does
         * NOT cancel in between-satellite differencing.  By default
         * (false) GLONASS is dropped at three gates: ensureRequiredStates,
         * isUsableMeasurement, and BssdPivotManager pivot selection.
         *
         * When true, GLONASS observations flow through BSSD.  Per-(sat,
         * signal) sat-bias states absorb the receiver inter-channel bias
         * (ICB) — leverages the existing @c estimate_sat_biases path,
         * which adds states whenever BIA coverage is missing (always
         * the case for GLONASS in standard products).  GLONASS phase
         * ambiguities are auto-marked unresolvable so LAMBDA never
         * tries to integer-fix the ICB-contaminated value.
         *
         * Recommended: keep false unless you really need the extra
         * geometry from GLONASS.  GPS+Galileo+BeiDou already provide
         * 30+ satellites, more than enough for a clean PPP-RTK
         * solution.  Set true and also @c estimate_sat_biases=true to
         * activate.
         */
        bool enableGlonassBssd() const { return enable_glonass_bssd_; }

        /**
         * @brief Get the BSSD pivot minimum elevation (degrees).
         *
         * The current pivot satellite is retained as long as its elevation
         * stays above this threshold.  A new highest-elevation satellite is
         * chosen only when the current pivot drops below it or disappears.
         * This hysteresis avoids frequent handovers between satellites with
         * similar elevations.
         *
         * @return Pivot retention elevation threshold in degrees.
         */
        double getBssdPivotMinElevation() const { return bssd_pivot_min_elevation_deg_; }

        /**
         * @brief Minimum |median SD residual|, in cycles of the band's
         *        wavelength, required to flag a pivot slip.
         *
         * The KF post-fit pivot-slip detector finds groups of SD rows
         * whose residuals share a coherent non-zero median.  Without an
         * absolute floor, sub-cycle coherent biases from multipath,
         * PCV miscal, or kinematic position dynamics fire the
         * handover even though no actual cycle slip occurred.  Real
         * cycle slips are integer multiples of the wavelength, so a
         * threshold of @c 0.25 (λ/4) cleanly rejects everything below
         * the smallest physically meaningful slip while preserving
         * full detection of 1-cycle-and-larger events.
         *
         * Computed per-band: the floor at evaluation time is
         * (@c pivot_slip_min_cycles × λ_band).  L1 (~0.19 m) → ~4.8 cm;
         * L2 (~0.24 m) → ~6 cm; L5 (~0.26 m) → ~6.4 cm.
         *
         * Set to @c 0.0 to disable the floor (revert to pure coherence
         * detection — useful when validating the detector against
         * data with known sub-cycle slips, or in static contexts where
         * sub-cm coherent offsets are real disturbances worth handling).
         *
         * @return Wavelength-fraction threshold (cycles).  Default 0.25.
         */
        double getPivotSlipMinCycles() const { return pivot_slip_min_cycles_; }

        /**
         * @brief Minimum EWMA-mean |residual|, in cycles of the band's
         *        wavelength, that triggers R-inflation downweighting
         *        for a phase row's persistent bias.
         *
         * Distinct from @c getPivotSlipMinCycles, which gates the
         * INSTANTANEOUS residual against cycle-slip semantics.  This
         * gate tracks the *historical* tendency of a particular
         * (satellite, signal) row to carry a non-zero residual mean
         * across many epochs — multipath fingerprints, PCV miscal,
         * inter-channel hardware bias, or any persistent unmodeled
         * effect.  Sourced from @c ResidualHistoryTracker's EWMA mean.
         *
         * When @c |mean| · sign > threshold · λ for a row whose
         * sample count exceeds @c warmup_n, the row's diagonal R
         * entry is inflated by @c mean² so the KF stops trusting the
         * channel as much as its formal stochastic noise suggests.
         * Inflation is automatic and recovers as the EWMA mean
         * decays (e.g., once the satellite climbs above a multipath
         * geometry).
         *
         * Default 0.10 (λ/10 ≈ 2 cm on L1) — small enough to catch
         * persistent biases that the pivot/outlier paths now ignore,
         * large enough that random per-epoch noise doesn't trip it.
         * Set to 0 to disable.
         *
         * @return Wavelength-fraction threshold (cycles).
         */
        double getPersistentBiasMinCycles() const {
            return persistent_bias_min_cycles_;
        }

        /**
         * @brief Stuck-code-bias detector configuration.
         *
         * Returned by const-reference; consumers (KalmanFilter's
         * outlier handler, the post-commit stuck-bias scan) read the
         * @c enabled flag once per epoch and only touch the
         * thresholds when firing.
         *
         * @return Const ref to the stored config (JSON path
         *         @c estimation.stuck_code_bias).
         */
        const StuckCodeBiasConfig& getStuckCodeBiasConfig() const {
            return stuck_code_bias_;
        }

        /**
         * @brief Sat-wide state re-inflation configuration.
         *
         * Read by the cycle-slip orchestration layer
         * (@c CycleSlipManager::detectAndRepair, @c Application's
         * sat-wide-reset path) to decide whether and how to call
         * @c StateManager::reinflateIonosphereVariance and
         * @c reinflateSatBiasVariancesForSat after every phase
         * ambiguity for a satellite has been reset.
         *
         * @return Const ref to the stored config (JSON path
         *         @c estimation.slip_state_reinflation).
         */
        const SlipStateReinflationConfig& getSlipStateReinflationConfig() const {
            return slip_state_reinflation_;
        }

        /**
         * @brief Gets the real-time corrections-ready startup gate config.
         *
         * When @c enabled (real-time only), @c RealTimeEngine::processEpoch
         * defers all estimation until every modeled above-mask satellite is
         * broadcast+SSR (cm-grade clock), so code and phase are seeded together
         * rather than cold-starting code-only on broadcast clocks.
         *
         * @return Const reference to the gate config
         *         (@c estimation.defer_until_corrections_ready).
         */
        const CorrectionsReadyGateConfig& getCorrectionsReadyGate() const {
            return corrections_ready_gate_;
        }

        /**
         * @brief Gets the soft a-priori ZWD constraint config.
         *
         * When @c enabled, the real-time loop applies a per-epoch soft pseudo-
         * observation anchoring the zenith wet delay to a climatological prior,
         * which breaks the BSSD [up, ZWD] null space that otherwise lets ZWD and
         * height run equal-and-opposite to ~1000 m during convergence.
         *
         * @return Const reference to the ZWD constraint config
         *         (@c estimation.zwd_constraint).
         */
        const ZwdConstraintConfig& getZwdConstraint() const {
            return zwd_constraint_;
        }

        /**
         * @brief Gets the cycle-slip detector selection.
         *
         * Consumed by @c SlipDetectorRegistry::createEnabled to compose the
         * @c CycleSlipManager detector set.
         *
         * @return Const ref to the stored config (JSON path
         *         @c estimation.cycle_slip).
         */
        const CycleSlipConfig& getCycleSlipConfig() const {
            return cycle_slip_;
        }

        /**
         * @brief Gets the navigation aiding-source selection.
         *
         * Consumed by @c AidingSourceRegistry::createEnabled to compose the
         * @c AidingSourceManager source set.  @c imu back-compat-defaults to
         * @c enableImu() when @c estimation.aiding.imu is not set explicitly.
         *
         * @return Const ref to the stored config (JSON path
         *         @c estimation.aiding).
         */
        const AidingConfig& getAidingConfig() const {
            return aiding_;
        }

        /**
         * @brief Huber threshold (standardized residual) above which a
         *        sub-cycle phase row's measurement variance is inflated
         *        in-update — no history needed.
         *
         * Complements the EWMA-based persistent-bias inflation: the
         * EWMA mechanism only fires once a row has accumulated enough
         * samples (~20 epochs at α=0.05), too slow to catch a satellite
         * whose phase residual is already biased in its first epochs of
         * tracking.  When a phase row's standardized residual exceeds
         * this threshold AND its residual is sub-cycle (i.e. below
         * @c pivot_slip_min_cycles · λ — anything larger is cycle-slip
         * territory and is left to the post-fit outlier handler), the
         * row's diagonal @c R is inflated so the row stays in the
         * update with reduced weight rather than pulling the KF state
         * toward a persistent sub-cycle bias.  Two schemes are
         * available, selected by @c enableHuberInnovation(): a post-fit
         * IRLS that converges the standardized residual to the
         * threshold over several rebuild passes (default), or an
         * innovation-based closed-form one-shot.
         *
         * Default value: @c 6.0 — the same threshold as the outlier
         * reset.  The two mechanisms are kept apart by the sub-cycle
         * gate, not by this threshold: above it, a sub-cycle phase row
         * is Huber-downweighted (too small to be a slip) while a
         * non-sub-cycle row is reset by the outlier handler.  Set to 0
         * to disable.
         *
         * @return Threshold on |standardized residual|.  Default 6.0.
         */
        double getPhaseHuberSresThreshold() const {
            return phase_huber_sres_threshold_;
        }

        /**
         * @brief Whether the in-update Huber R-inflation uses the
         *        innovation-based one-shot scheme instead of post-fit IRLS.
         *
         * False (default): a flagged sub-cycle phase row has its @c R
         * inflated by @c (|sres|/k)^2 each rebuild pass, where @c |sres|
         * is the post-fit residual standardized by the working
         * @c R(i,i); @c |sres| converges to the threshold @c k
         * asymptotically over several passes.
         *
         * True: the row is standardized by its innovation covariance,
         * @c |innov|/sqrt(H·P·H' + R), which is monotone in @c R — so
         * the @c R that lands the standardized residual exactly on @c k
         * is closed-form (@c R_target = innov^2/k^2 - H·P·H', the prior
         * variance projected into the row) and applied in a single
         * rebuild pass.  @c k is @c getPhaseHuberSresThreshold().
         *
         * JSON: @c estimation.enable_huber_innovation.  Default false.
         *
         * @return True if the innovation-based one-shot scheme is active.
         */
        bool enableHuberInnovation() const { return enable_huber_innovation_; }

        /**
         * @brief Master on/off for the in-update Huber R-inflation pass.
         *
         * Independent of @c getPhaseHuberSresThreshold(): when this
         * returns false, the post-fit Huber pass is skipped entirely
         * even if the threshold is positive.  Useful for fair
         * comparison studies where one wants to deactivate the entire
         * outlier-downweighting branch without also having to set the
         * threshold to a sentinel value.
         *
         * Defaults to true so existing configs that rely on the
         * threshold-only gate continue to behave identically.
         *
         * JSON: @c estimation.enable_huber_r_inflation.
         *
         * @return True if the Huber R-inflation pass is active.
         */
        bool enableHuberRInflation() const { return enable_huber_r_inflation_; }

        /**
         * @brief Master on/off for the per-row sub-cycle slip
         *        suppression in the phase outlier dispatch.
         *
         * When true (default), a flagged phase row whose |residual|
         * is below @c getPivotSlipMinCycles() · λ is treated as a
         * sub-cycle bias rather than a cycle slip: the row is
         * excluded from this rebuild but the ambiguity state is
         * preserved.  When false, every flagged phase outlier
         * triggers the normal ambiguity-reset path regardless of how
         * small the residual is — i.e., the λ-fraction floor is
         * bypassed.
         *
         * This is independent of @c getPivotSlipMinCycles(), which
         * sets the floor's *value*; the bool gates whether the floor
         * is consulted at all.  Useful for comparing PPP (which
         * historically resets on any flagged outlier through the
         * iono-free signal naming) against PPP-RTK (which gets the
         * suppression on bare per-band codes) on equal terms.
         *
         * Defaults to true so existing configs are unchanged.
         *
         * JSON: @c estimation.enable_subcycle_slip_suppression.
         *
         * @return True if sub-cycle phase outliers should skip the
         *         ambiguity reset; false to reset unconditionally.
         */
        bool enableSubcycleSlipSuppression() const {
            return enable_subcycle_slip_suppression_;
        }

        /**
         * @brief Check if receiver inter-frequency bias (IFB) estimation is enabled.
         *
         * When true (default in PPP-RTK uncombined mode), per-system code-bias
         * states are added for every non-reference code band.  These absorb the
         * receiver hardware delay difference between frequency bands that is not
         * removed by external SSR code-bias products.
         *
         * Disable this when SSR code-bias corrections are complete and no
         * residual receiver IFB needs to be estimated, reducing the state
         * dimension and improving observability of the ionosphere state.
         *
         * @return True if IFB estimation is enabled.
         */
        bool estimateIfb() const { return estimate_ifb_; }

        /**
         * @brief Check if signal selection is locked to clock-reference bands.
         *
         * When true (default) and uncombined PPP-RTK mode is active, only the
         * two frequency bands corresponding to the satellite clock reference
         * observables from the BIA DESCRIPTION block are used.  This avoids
         * extra IFB states for non-reference bands and ensures the measurement
         * model is fully consistent with the clock/bias products.
         *
         * Set to false to allow all available frequency bands (the old
         * behaviour).  Extra bands will require IFB estimation.
         *
         * @return True if signal selection is restricted to clock-reference bands.
         */
        bool lockSignalsToClockRef() const { return lock_signals_to_clock_ref_; }

        /**
         * @brief Check if signals without BIA product coverage should be excluded.
         *
         * When true and estimate_sat_biases is false, the uncombined observation
         * processor will skip any signal for which the BIA product file does not
         * contain a bias entry.  When estimate_sat_biases is true this toggle is
         * ignored (uncovered signals get an estimated bias state instead).
         *
         * @return True if BIA coverage is required for signal acceptance.
         */
        bool requireBiaForSignals() const { return require_bia_for_signals_; }

        /**
         * @brief Check if per-satellite signal bias estimation is enabled.
         *
         * When true, signals without BIA product coverage are kept and a
         * per-satellite-per-signal bias state is added to the Kalman filter
         * to absorb the uncorrected satellite hardware delay.  This allows
         * using all available signals even when the BIA product has incomplete
         * coverage.
         *
         * @return True if satellite bias estimation is enabled.
         */
        bool estimateSatBiases() const { return estimate_sat_biases_; }

        /**
         * @brief Warm-restore window for the satellite-bias cache (seconds).
         *
         * When a sat-bias state is pruned (its satellite/signal isn't
         * observed in the current epoch), its value and diagonal
         * variance are stashed in an in-memory cache.  If the same
         * (sat, signal) reacquires within this many seconds, the
         * cached state is restored verbatim with the variance aged
         * forward by @c process_noise.sat_bias_q × gap_s — a "warm
         * restart" that preserves the converged bias estimate across
         * brief visibility losses (low-elevation glitches, geometry
         * holes, masking).
         *
         * Beyond this window but within @c getSatBiasCacheMaxAgeS()
         * the cached value is used as the seed but the variance is
         * reset to @c initial_std.sat_bias² — a "cold restart from
         * cached value", useful when the gap is long enough that the
         * stashed variance is no longer trustworthy but the value
         * itself is still a better starting point than the WLS
         * single-epoch seed.
         *
         * Default 300 s (5 minutes).  Set equal to
         * @c getSatBiasCacheMaxAgeS() to skip the cold-restart band
         * entirely (warm-or-evict).  Configured via
         * @c estimation.sat_bias_cache.warm_age_s.
         *
         * @return Warm-restore window in seconds.
         */
        double getSatBiasCacheWarmAgeS() const { return sat_bias_cache_warm_age_s_; }

        /**
         * @brief Maximum cache lifetime for satellite-bias entries (seconds).
         *
         * Cache entries older than this are evicted and never
         * restored — the next acquisition uses the WLS-derived
         * single-epoch seed, exactly as if no cache existed.  Caps the
         * memory footprint and prevents very stale values from
         * silently re-entering the filter after long outages.
         *
         * Must be >= @c getSatBiasCacheWarmAgeS(); if the JSON sets
         * @c max_age_s lower it is clamped up to @c warm_age_s at load
         * time and a warning is logged.
         *
         * Default 3600 s (1 hour).  Configured via
         * @c estimation.sat_bias_cache.max_age_s.
         *
         * @return Cache lifetime in seconds.
         */
        double getSatBiasCacheMaxAgeS() const { return sat_bias_cache_max_age_s_; }

        /**
         * @brief Configuration block for the post-fit residual history
         *        tracker.
         *
         * The tracker is a temporal corroboration layer over the
         * spatial pivot-slip detector in @c KalmanFilter.  When
         * @c enabled is false (default) the existing detection path
         * runs unchanged; when true, candidate pivot-slip groups are
         * queried against per-(sat, signal) EWMA residual statistics
         * and the joint signal is logged as high-confidence vs
         * spatial-only.  See @c filter/ResidualHistoryTracker.hpp.
         */
        const ResidualHistoryConfig& getResidualHistoryConfig() const { return residual_history_; }

        /**
         * @brief Access the adaptive ionospheric process-noise configuration.
         *
         * When @c AdaptiveIonoQConfig::enabled, @c IonoVariabilityMonitor
         * scales the per-satellite ionospheric process noise from the
         * geometry-free phase second-time-difference variability.
         * Default-constructed (disabled) when the JSON block is absent.
         *
         * @return Const reference to the adaptive iono-Q configuration.
         */
        const AdaptiveIonoQConfig& getAdaptiveIonoQConfig() const { return adaptive_iono_q_; }

        /**
         * @brief Access the GIM ionosphere-weighting configuration.
         * @return Const reference to the ionosphere-weighting config.
         */
        const IonosphereWeightingConfig& getIonosphereWeightingConfig() const { return iono_weighting_; }

        /**
         * @brief Variance multiplier for code measurements without BIA coverage.
         *
         * When > 1.0 and estimate_sat_biases is false, code observations on
         * signals that lack a .BIA product entry have their measurement
         * variance inflated by this factor.  The uncorrected satellite
         * hardware delay is treated as unmodelled noise on code, while the
         * corresponding phase measurement absorbs it into the float
         * ambiguity (no inflation needed).
         *
         * Set to 1.0 to disable (no extra downweighting).
         *
         * @return Variance inflation factor (dimensionless, >= 1.0).
         */
        double noBiaCodeDownweight() const { return no_bia_code_downweight_; }

        /**
         * @brief Check if a user-supplied reference position is configured.
         * @return True if reference_position_ecef was specified in the JSON config.
         */
        bool hasReferencePosition() const { return has_reference_position_; }

        /**
         * @brief Get the user-supplied reference ECEF position.
         *
         * When hasReferencePosition() is true this returns the XYZ coordinates
         * (meters, ITRF/IGS14) specified in the config file. Use this to fix
         * the receiver position for residual analysis.
         *
         * @return Reference position as {X, Y, Z} in meters.
         */
        const std::array<double, 3>& getReferencePositionEcef() const { return reference_position_ecef_; }

        /**
         * @brief Check if IMU integration is enabled.
         * @return True if IMU processing is active.
         */
        bool enableImu() const { return imu_config_.enabled; }

        /**
         * @brief Check if tightly-coupled GNSS/INS mode is active.
         *
         * Tightly coupled means raw GNSS observations (code, phase, Doppler)
         * directly update the full INS+GNSS error-state vector, with the
         * measurement Jacobian extended to include attitude-error partials.
         *
         * @return True if IMU is enabled AND coupling_mode == TightCoupled.
         */
        bool tightlyCoupledImu() const {
            return imu_config_.enabled &&
                   imu_config_.coupling_mode == ImuCouplingMode::TightCoupled;
        }

        /**
         * @brief Get the IMU configuration block.
         * @return Const reference to the ImuConfig struct.
         */
        const ImuConfig& getImuConfig() const { return imu_config_; }

        /**
         * @brief Get the data download configuration block.
         * @return Const reference to the DownloadConfig struct.
         */
        const DownloadConfig& getDownloadConfig() const { return download_config_; }

        /**
         * @brief Get the input-file source selector.
         *
         * @return @c DataSource::Internal (default — globs
         *         @c internal_data_root) or @c DataSource::Production
         *         (auto-download via @c data_download).
         */
        DataSource getDataSource() const { return data_source_; }

        /**
         * @brief Convenience predicate: true when running against the
         *        internal data tree.
         */
        bool isInternalDataSource() const { return data_source_ == DataSource::Internal; }

        /**
         * @brief Convenience predicate: true when running with auto-download.
         */
        bool isProductionDataSource() const { return data_source_ == DataSource::Production; }

        /**
         * @brief Root directory used in @c DataSource::Internal mode.
         *
         * Resolved at config load time:
         *  - If @c files.internal_data_root is set in the JSON, that
         *    value is used; relative paths are resolved against the
         *    config file's parent directory.
         *  - Otherwise it falls back to @c <project_root>/data, where
         *    @c project_root is the CMake-baked @c GNSS_PROJECT_ROOT.
         *
         * Always an absolute path (never empty after a successful load).
         */
        const std::filesystem::path& getInternalDataRoot() const { return internal_data_root_; }

        /**
         * @brief Get the top-level application execution mode.
         * @return AppMode (PostProcessing or RealTime).
         */
        AppMode getAppMode() const { return app_mode_; }

        /**
         * @brief Get the real-time processing configuration.
         * @return Const reference to the RealTimeConfig struct.
         */
        const RealTimeConfig& getRealTimeConfig() const { return realtime_config_; }

        /**
         * @brief Get the baseline / network processing configuration.
         *
         * Populated when @c getProcessingMode() returns @c BaselineSingle or
         * @c BaselineMulti.  For PPP / PPP-RTK the returned struct is
         * default-constructed and unused.
         *
         * @return Const reference to the BaselineConfig struct.
         */
        const BaselineConfig& getBaselineConfig() const { return baseline_config_; }

        /**
         * @brief Check if the active mode is a baseline / network solve.
         * @return True when @c processing_mode_ is @c BaselineSingle or @c BaselineMulti.
         */
        bool isBaselineMode() const {
            return processing_mode_ == ProcessingMode::BaselineSingle
                || processing_mode_ == ProcessingMode::BaselineMulti;
        }

        /**
         * @brief Whether a live orbit/clock source supplants precise SP3/CLK products.
         *
         * True only in real-time mode when satellite orbits and clocks arrive
         * from the live stream rather than from loaded precise products — i.e.
         * an IGS-SSR corrections stream is enabled (broadcast ephemeris + SSR
         * orbit/clock corrections) and/or the primary input is a live socket
         * (@c "ntrip" / @c "tcp", as opposed to @c "file" replay).  In that case
         * the post-fit SP3/CLK files are not inputs: loading them only risks a
         * stale fixture shadowing today's live PRNs (the coverage gate in
         * @c Geometry::getSatellitePV exists precisely to defend against that),
         * and in production mode would trigger pointless CDDIS downloads.
         *
         * Real-time *file* replay with corrections disabled returns false, so a
         * historical run can still benefit from precise products (SP3-first
         * beats broadcast+SSR for a matching date).  Post-processing always
         * returns false — precise products are the authoritative source there.
         *
         * @return True when precise SP3/CLK should be skipped in favour of the
         *         live broadcast+SSR orbit/clock path.
         */
        bool realtimeUsesLiveOrbitClockSource() const {
            if (app_mode_ != AppMode::RealTime) return false;
            return realtime_config_.corrections.enabled
                || realtime_config_.input_type != "file";
        }

        /**
         * @brief Get the integer ambiguity resolution mode.
         * @return AR mode: 0=float-only, 1=continuous, 2=instantaneous, 3=fix-and-hold.
         */
        int getArMode() const { return ar_mode_; }

        /**
         * @brief Get the LAMBDA ratio test threshold.
         * @return Ratio threshold (e.g. 3.0).
         */
        double getArRatioThreshold() const { return ar_ratio_threshold_; }

        /**
         * @brief Get the elevation mask for AR candidates.
         * @return Elevation mask in degrees.
         */
        double getArElevationMask() const { return ar_elevation_mask_; }

        /**
         * @brief Get the fix-and-hold pseudo-observation variance.
         * @return Variance in m^2.
         */
        double getArHoldVariance() const { return ar_hold_variance_; }

        /**
         * @brief Minimum filter epochs before AR is allowed.
         *
         * Convergence guard: AR runs LAMBDA on the float ambiguity covariance,
         * and a marginal fix accepted before the float covariance has settled
         * tends to lock onto wrong integers and cascade — once a wrong integer
         * is constrained into the state, subsequent LAMBDA searches see a
         * degenerate covariance direction and produce inflated ratios that
         * look confident but are mathematically pathological.  10 epochs at
         * 30s rate ≈ 5 minutes is the minimum sane gate; 30+ minutes is
         * conservative.  Skipped epochs return @c success=false from resolve().
         */
        int getArMinEpochs() const { return ar_min_epochs_; }

        /**
         * @brief Maximum 3D formal position σ for AR to fire (m).
         *
         * Convergence guard based on filter confidence rather than wall time.
         * AR is skipped when @c sqrt(P(0,0) + P(1,1) + P(2,2)) exceeds this.
         * Default 5 cm — once the filter is this confident in position, the
         * float ambiguity covariance is also typically converged enough for
         * a meaningful integer search.
         */
        double getArMaxPositionSigma() const { return ar_max_position_sigma_m_; }

        /**
         * @brief Upper bound on the LAMBDA ratio test (reject above this).
         *
         * Healthy AR ratios sit in the 3–1000 range.  Ratios above ~1e4
         * indicate the float covariance has a near-zero-variance direction
         * (degenerate from a previous wrong-integer hold or numerical
         * pathology), making the second-best integer's chi² distance
         * effectively infinite.  Such fixes look ultra-confident but are
         * mathematically suspect.  Reject them and stay float.
         */
        double getArRatioUpperBound() const { return ar_ratio_upper_bound_; }

        /**
         * @brief WL-only AR mode toggle.
         *
         * When true, AR applies MW-resolved DD WL integers as Kalman
         * pseudo-observations on the SD ambiguity state and skips the
         * LAMBDA NL search entirely.  Use this as a conservative
         * verification stage before turning on the full WL+NL LAMBDA
         * pipeline: WL via MW is robust even when float NL covariance
         * hasn't converged enough for safe integer rounding.
         */
        bool getArWlOnly() const { return ar_wl_only_; }

        /**
         * @brief Pseudo-observation variance (cycles²) for WL-only constraints.
         *
         * Default 1e-4 cycle² = (0.01 cycle)² σ.  MW-resolved WL integers
         * are typically locked to within a small fraction of a cycle, so
         * the constraint can be tight.
         */
        double getArWlOnlyPseudoVar() const { return ar_wl_only_pseudo_var_; }

        /**
         * @brief Maximum *worst-case* fractional residual (cycles) accepted
         *        for an MW WL integer to enter the AR pseudo-observation update.
         *
         * The MW detector reports a per-arc worst-case fractional residual
         *
         *     worst_case_frac = |mean/λ_WL − rounded_int| + 3 · σ_mean / λ_WL
         *
         * combining the running mean's offset from its rounded integer with
         * a 3σ bound on the standard error of the mean.  An arc with
         * worst_case_frac < 0.5 is the same condition that would fire the
         * lock criterion, so locked arcs always have worst_case < 0.5.
         *
         * Default 0.40 cyc — accepts arcs whose 99.7% confidence interval
         * for the rounded integer leaves at least 10% of a cycle of margin
         * before the half-cycle boundary.  This gates BOTH locked and
         * BIA-corrected-but-converging arcs uniformly: arcs whose σ_mean
         * has shrunk enough are accepted; freshly BIA-corrected arcs with
         * wide σ_mean wait until they've averaged enough samples.
         *
         * Tighten (e.g. 0.30) for stricter AR; loosen (e.g. 0.45) if AR is
         * starved.  Setting < 0.20 will admit only very mature, clean
         * locks (which can take many epochs of averaging).
         */
        double getArWlMaxFrac() const { return ar_wl_max_frac_; }

        /**
         * @brief Minimum bootstrap success rate for AR fix acceptance.
         *
         * Bootstrap success rate P_s,boot is a closed-form lower bound
         * on the integer least-squares (ILS) success rate, computed
         * from the LDL diagonal of the decorrelated DD covariance:
         *
         *     P_s,boot = Π_i  erf( 1 / (2·√(2·d_i)) )
         *
         * Acts as a Fixed-Failure-Rate (FFR) acceptance criterion: AR
         * commits a fix only when P_s,boot ≥ this threshold.  Unlike
         * the ratio test (which uses a fixed threshold regardless of
         * problem geometry), this threshold is **self-tuning** — it
         * becomes harder to pass when the search is high-dimensional
         * or poorly conditioned.
         *
         * Default 0.999 → at most 0.1% probability of accepting a
         * wrong-integer fix.  Tighten to 0.9999 for safety-critical
         * applications; loosen to 0.99 when integrity is less
         * critical and acceptance latency matters.
         */
        double getArMinSuccessRate() const { return ar_min_success_rate_; }

        /**
         * @brief Maximum per-NL-pair |float − fixed| residual (cycles)
         *        for AR acceptance.
         *
         * Even when the joint LAMBDA ratio test passes, individual
         * pairs whose float landed close to the half-cycle boundary
         * are likely contributing the wrong integer to the joint set
         * (the marginal fixes that destabilize AR on the next epoch).
         * Reject the entire fix when any pair's |frac| exceeds this
         * cap.
         *
         * Default 0.15 cyc.  Tighten to 0.10 if you observe wrong-fix
         * symptoms (ratio collapse on next epoch); loosen to 0.20 if
         * AR is too aggressive on rejecting otherwise-good fixes.
         */
        double getArNlMaxPerPairFrac() const { return ar_nl_max_per_pair_frac_; }

        /**
         * @brief Get the list of constellations eligible for AR.
         *
         * Empty list = all active systems are eligible (current default).
         * Non-empty = only listed constellations enter the AR candidate
         * set.  Excluded constellations stay in the float solution
         * (their phase observations contribute to position/troposphere/
         * iono estimation and their ambiguities are still float-tracked)
         * but are skipped by @c buildDdPairs and
         * @c applyMwWlConstraintsNlWl_.
         *
         * Usage: when one constellation has residual model bias
         * (uncalibrated antenna PCV, mismatched phase wind-up
         * convention, missing per-attribute BIA entries, etc.) that
         * drags down the joint LAMBDA ratio test even though its float
         * contribution is healthy.  Common production strategy when one
         * system's bias model is less mature than the others.
         *
         * Configured via @c estimation.ambiguity_resolution.active_systems
         * as a JSON list of system names: ["GPS"], ["GPS","Galileo"], etc.
         */
        const std::vector<std::string>& getArActiveSystems() const { return ar_active_systems_; }

        /**
         * @brief Get the logging configuration block.
         * @return Const reference to the LoggingConfig struct.
         */
        const LoggingConfig& getLoggingConfig() const { return logging_config_; }

        /**
         * @brief Override the observation path (used by auto-download).
         * @param path New observation RINEX path.
         */
        void setObservationPath(const std::filesystem::path& path) { observation_path_ = path; }

        /**
         * @brief Override the navigation path (used by auto-download).
         * @param path New broadcast navigation RINEX path.
         */
        void setNavigationPath(const std::filesystem::path& path) { navigation_path_ = path; }

        /**
         * @brief Override the product paths list (used by auto-download).
         * @param paths New vector of product file paths.
         */
        void setProductPaths(const std::vector<std::filesystem::path>& paths) { product_paths_ = paths; }

        /**
         * @brief Override the baseline reference station's RINEX path.
         *
         * Used by @c PathResolver in @c DataSource::Internal mode to
         * fill in an empty @c baseline.reference.rinex_path from the
         * internal data tree by station-name match.  No-op when
         * @c isBaselineMode() is false.
         *
         * @param path New RINEX path.
         */
        void setBaselineReferenceRinexPath(const std::filesystem::path& path) {
            baseline_config_.reference.rinex_path = path;
        }

        /**
         * @brief Override one rover station's RINEX path.
         * @param idx Index into the rovers vector.
         * @param path New RINEX path.
         */
        void setBaselineRoverRinexPath(size_t idx, const std::filesystem::path& path) {
            if (idx < baseline_config_.rovers.size())
                baseline_config_.rovers[idx].rinex_path = path;
        }

    private:
        // -------------------------------------------------------------
        //  loadFromJson section helpers
        //
        //  loadFromJson() is split into one private helper per top-level
        //  JSON section, defined in GnssConfigLoad.cpp.  Each helper
        //  takes the parsed root JSON object plus the LogManager and
        //  mutates @c this in place.  Sections that can fail
        //  (currently only baseline) return a bool; sections that only
        //  populate fields return @c void.
        // -------------------------------------------------------------
        void loadProcessingSection_(const nlohmann::json& json, LogManager& log_manager);
        void loadEstimationSection_(const nlohmann::json& json, LogManager& log_manager);
        void loadFilterSection_(const nlohmann::json& json, LogManager& log_manager);
        void loadModelCorrectionsSection_(const nlohmann::json& json, LogManager& log_manager);
        void loadReceiverSection_(const nlohmann::json& json, LogManager& log_manager);
        void loadFilesSection_(const nlohmann::json& json, LogManager& log_manager);
        void loadImuSection_(const nlohmann::json& json, LogManager& log_manager);
        void loadDataDownloadSection_(const nlohmann::json& json, LogManager& log_manager);
        void loadAppModeAndRealtimeSection_(const nlohmann::json& json, LogManager& log_manager);
        bool loadBaselineSection_(const nlohmann::json& json, LogManager& log_manager);
        void loadLoggingSection_(const nlohmann::json& json, LogManager& log_manager);
        void logConfigurationSummary_(LogManager& log_manager) const;

        ProcessingMode processing_mode_; ///< Processing mode (PPP or PPP-RTK).
        FrequencyMode   frequency_mode_;       ///< KF frequency band scope.
        ChannelMode     channel_mode_;         ///< KF tracking-mode channel multiplicity per band.
        MwFrequencyMode mw_frequency_mode_;///< MW detector frequency mode (Mirror inherits KF).
        MwChannelMode   mw_channel_mode_;    ///< MW detector channel mode (Mirror inherits KF).
        bool enable_inhouse_bias_estimation_;         ///< In-house per-(sat, sig-pair) MW phase bias estimation; default off.
        double mw_init_frac_threshold_;                 ///< First-epoch sanity check: max |frac| (WL cycles) before forcing a slip-on-init.  Raise for code-noisy combinations (L1C-based MW).
        double mw_cross_sat_ewma_alpha_;                 ///< EWMA factor (0,1] for smoothing the MW cross-sat σ warmup floor; smaller = smoother. Default 0.1.
        StateAmbiguityBasis state_ambiguity_basis_; ///< Ambiguity parameterization (PerBand or NlWl).
        SatelliteInterpolationMode satellite_interpolation_mode_; ///< Satellite position/velocity interpolation mode (Lagrange, Finite, Hermite, Spline).
        SatelliteClockInterpolationMode satellite_clock_interpolation_mode_; ///< Satellite clock interpolation mode (Linear, Lagrange).
        bool uncombined_enabled_;                      ///< Enable uncombined processing.
        double elevation_mask_deg_;                    ///< Elevation mask in degrees.
        bool exclude_eclipsing_satellites_;           ///< Drop satellites in Earth's umbral shadow at the geometry stage. Default off (legacy behavior); enable to avoid unmodelled yaw manoeuvres during eclipse and recovery.
        double eclipse_shadow_scale_;                   ///< Multiplier on Earth's radius for the cylindrical shadow test. 1.0 = pure geometric umbra; >1.0 widens the exclusion margin.
        double data_gap_tolerance_s_;                     ///< Data gap tolerance (s); 0 = use measurement rate.
        double long_gap_tolerance_s_;                   ///< Long-gap threshold (s); separates short vs long gap policy in handleGap.
        double gnss_rate_s_;                              ///< Effective processing rate (s); 0 = process every RINEX epoch.
        double max_duration_s_;                           ///< Max data-time span to process (s); 0 = unlimited. Measured as epoch GPS time relative to the first processed obs.
        bool   enable_obs_skip_to_latest_{ false };       ///< RT-only: skip obs epochs far behind wall-clock GPS (drop backlog, process freshest).
        double obs_skip_max_lag_s_{ 5.0 };                ///< RT-only: drop an obs epoch lagging wall-clock GPS by more than this (s).
        double rt_backlog_warn_s_{ 5.0 };                 ///< RT-only: warn-only obs-backlog threshold (s); ≤0 disables.
        double ssr_max_age_s_{ 30.0 };                    ///< RT-only: max |obs − SSR-record| age (s) before SSR overlay rejected to broadcast-grade.
        std::string rinex_types_;                        ///< Enabled RINEX observation type letters (subset of "CLDS"); default code+phase.
        ReceiverAntennaOrientation receiver_antenna_orientation_; ///< Antenna orientation parameters.
        ProcessNoise process_noise_;                          ///< Process noise parameters.
        InitialStd initial_std_;                              ///< Initial state uncertainties.
        DefaultMeasurementNoise default_meas_noise_;          ///< Default measurement noise (fallback).
        std::map<std::string, std::vector<std::string>> signal_priorities_; ///< Signal priorities per system.
        std::vector<std::string> active_systems_; ///< Active GNSS systems.
        std::vector<std::string> excluded_satellites_;                ///< Satellites excluded from processing (e.g. "E14", "E18").
        std::filesystem::path product_list_path_;            ///< Path to product list file.
        std::vector<std::filesystem::path> product_paths_;   ///< Product file paths (SP3, CLK, etc.).
        std::filesystem::path observation_path_;             ///< Path to observation RINEX file.
        std::filesystem::path navigation_path_;              ///< Path to navigation RINEX file.

        double zhd_pressure_mbar;  ///< Zenith hydrostatic delay pressure (mbar).
        double zhd_temp_kelvin;     ///< Zenith hydrostatic delay temperature (Kelvin).

        TroposphereAprioriSource tropo_apriori_source_; ///< P/T source for Saastamoinen ZHD.
        std::filesystem::path gpt3_grid_path_;     ///< Path to gpt3_5.grd; used when tropo_apriori_source_ == Gpt3 or tropo_mapping_function_ == Vmf3.
        TroposphereMappingFunction tropo_mapping_function_; ///< Hydrostatic/wet mapping function selection.

        double leap_seconds_tai_;  ///< Number of leap seconds (TAI-UTC=37) since 2017-01-01.

        double residual_threshold_;      ///< Residual threshold for outlier detection.
        double bad_meas_percent_;        ///< Percentage of bad measurements allowed.
        double ambiguity_std_;           ///< Ambiguity standard deviation.
        double code_noise_amplify_;      ///< Code noise amplification factor.

        std::filesystem::path otl_blq_path_;    ///< Path to BLQ ocean loading file (optional).

        bool estimate_tropo_gradients_;   ///< Estimate troposphere horizontal gradients (Gn, Ge).
        bool   relinearize_geometry_;            ///< Re-run computeModels after first KF update to recompute geometry at updated position.
        double relinearize_dpos_threshold_phase_m_;     ///< |Δpos| threshold (m) when phase observables are present.
        double relinearize_dpos_threshold_code_only_m_; ///< |Δpos| threshold (m) when no phase observables (code-only).
        int    relinearize_max_count_;              ///< Hard cap on total relinearizations per run; -1 = unlimited.
        int    eigen_threads_;                       ///< Eigen thread count (0=default, 1=sequential, N=pinned).  See eigenThreads().
        bool run_smoother_;                    ///< Run backward RTS smoother after forward Kalman pass (post-processing only).
        bool estimate_phase_clock_;            ///< Estimate separate phase clock offset.
        bool enable_if_slip_detector_;             ///< INERT — IfPhaseSlipDetector is staged/uncompiled and has no instantiation path yet (see include/slip/IfPhaseSlipDetector.hpp). Parsed for forward-compat; toggling it currently has no effect.
        bool accumulate_phase_windup_;                ///< Accumulated (unwrapped) phase wind-up for IAR readiness.
        bool enable_between_satellite_sd_;                ///< Between-satellite single-difference operator in measurement model.
        bool enable_glonass_bssd_;                        ///< Allow GLONASS satellites in BSSD (FDMA ICB → off by default).
        double bssd_pivot_min_elevation_deg_;              ///< Elevation threshold below which the BSSD pivot is handed over (degrees).
        double pivot_slip_min_cycles_;                     ///< Minimum |median SD residual| in band-wavelength cycles for pivot-slip detection.  0 disables.
        double persistent_bias_min_cycles_;                ///< Min EWMA |mean| (in cycles) before a phase row's R is inflated to downweight persistent bias.  0 disables.
        double phase_huber_sres_threshold_;                 ///< In-update Huber R-inflation threshold on |sres| for sub-cycle phase rows.  0 disables.
        bool enable_huber_innovation_;               ///< Use innovation-based closed-form Huber R-inflation instead of post-fit IRLS.
        bool enable_huber_r_inflation_;               ///< Master on/off for the in-update Huber R-inflation pass.  When false, the pass is skipped regardless of the threshold value.
        bool enable_subcycle_slip_suppression_;       ///< Master on/off for the per-row λ-fraction floor that suppresses ambiguity resets on sub-cycle phase outliers.
        bool estimate_ifb_;                                    ///< Estimate receiver inter-frequency bias (IFB) for non-reference code bands.
        bool lock_signals_to_clock_ref_;                        ///< Restrict uncombined PPP-RTK to clock-reference frequency bands only.
        bool require_bia_for_signals_;                           ///< Only use signals with BIA product coverage.
        bool estimate_sat_biases_;                               ///< Estimate per-satellite bias for signals without BIA coverage.
        double sat_bias_cache_warm_age_s_;                       ///< Sat-bias cache: warm-restore window (sec).  Within this, restore value + aged variance.
        double sat_bias_cache_max_age_s_;                       ///< Sat-bias cache: max entry lifetime (sec).  Beyond this, evict; in [warm, max], restore value with cold-start variance.
        double no_bia_code_downweight_;                            ///< Variance multiplier for code on signals without BIA coverage.
        bool estimate_velocity_;                           ///< Estimate receiver velocity from Doppler observations.
        ResidualHistoryConfig residual_history_;                    ///< Per-(sat, signal) post-fit residual history tracker config.
        StuckCodeBiasConfig   stuck_code_bias_;                     ///< Stuck-code-bias detector + reset thresholds.
        SlipStateReinflationConfig slip_state_reinflation_;         ///< Sat-wide iono + sat-bias variance re-inflation on phase reset.
        CorrectionsReadyGateConfig corrections_ready_gate_;         ///< Real-time: defer estimation until all above-mask sats are broadcast+SSR.
        ZwdConstraintConfig    zwd_constraint_;                     ///< Soft a-priori ZWD constraint (anchors the BSSD [up,ZWD] null space).
        CycleSlipConfig        cycle_slip_;                         ///< Which cycle-slip detectors run (SlipDetectorRegistry selection).
        AidingConfig           aiding_;                             ///< Which aiding sources run (AidingSourceRegistry selection).
        AdaptiveIonoQConfig adaptive_iono_q_;                       ///< Adaptive ionospheric process-noise (GF second-difference) config.
        IonosphereWeightingConfig iono_weighting_;                  ///< GIM ionosphere-weighting (PPP-RTK soft iono constraint) config.

        bool has_reference_position_;                          ///< True if user supplied a reference ECEF position.
        std::array<double, 3> reference_position_ecef_; ///< User-supplied reference position ECEF (m).

        ModelCorrections model_corrections_;                            ///< Enable/disable flags for model corrections.
        StochasticModelType stochastic_model_; ///< Measurement stochastic model selection.
        double code_elevation_power_;  ///< σ-power on the elevation amplification for **code** observations: σ = σ_zen / sin^p(elev).  p=1.0 reproduces the legacy 1/sin²(elev) variance scaling.  See @c getCodeElevationPower().
        double phase_elevation_power_; ///< σ-power for **phase**: default 0.5 gives variance scaling 1/sin(elev), much gentler than the code's 1/sin²(elev).  See @c getPhaseElevationPower().
        SnrModelParams snr_model_params_;                                ///< SNR-based model parameters.
        SatBlockNoiseParams sat_block_noise_params_;                      ///< Satellite-block stochastic model parameters.
        ImuConfig imu_config_;                                           ///< IMU integration configuration.
        DownloadConfig download_config_;                                  ///< Data download and caching configuration.
        DataSource data_source_;                  ///< Selects internal-data vs production-download mode.
        std::filesystem::path internal_data_root_;                        ///< Absolute path to the internal data tree (resolved at load time).
        AppMode app_mode_;                    ///< Top-level execution mode.
        RealTimeConfig realtime_config_;                                  ///< Real-time streaming configuration.
        BaselineConfig baseline_config_;                                  ///< Baseline / network processing configuration (used when isBaselineMode()).

        // --- Ambiguity Resolution ---
        int    ar_mode_;                  ///< AR mode: 0=float, 1=continuous, 2=instantaneous, 3=fix-and-hold.
        double ar_ratio_threshold_;     ///< LAMBDA ratio test threshold for acceptance.
        double ar_elevation_mask_;     ///< Elevation mask for AR candidates (degrees).
        double ar_hold_variance_;     ///< Pseudo-obs variance for fix-and-hold (m^2).
        int    ar_min_epochs_;           ///< Minimum filter epochs before AR is allowed (convergence guard).
        double ar_max_position_sigma_m_;  ///< Maximum 3D formal position σ for AR to fire (m).
        double ar_ratio_upper_bound_;   ///< Reject fixes whose ratio exceeds this (covariance pathology).
        bool   ar_wl_only_;           ///< WL-only AR: apply MW-resolved WL integers as Kalman pseudo-obs, skip LAMBDA NL search.
        double ar_wl_only_pseudo_var_; ///< Pseudo-obs variance (cycles²) for WL-only constraints.
        double ar_wl_max_frac_;        ///< Max worst-case fractional residual (|frac|+3σ_mean/λ, cyc) for an MW WL to enter AR.
        double ar_min_success_rate_;  ///< Min bootstrap success rate (FFR-style) for AR fix acceptance.
        double ar_nl_max_per_pair_frac_; ///< Max per-NL-pair |float − fixed| (cyc); rejects marginal fixes.
        std::vector<std::string> ar_active_systems_; ///< Constellations eligible for AR (empty = all).

        LoggingConfig logging_config_;             ///< Logger and trace file enable/disable flags.
    };

} // namespace GnssProcessor
