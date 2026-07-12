#pragma once


#include "config/GnssConfigTypes.hpp"   // enums + POD config structs (relocated)

#include "logging/LogManager.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json_fwd.hpp>        // only the loadXxxSection_ decls need json; the
                                        // full <nlohmann/json.hpp> lives in GnssConfigLoad.cpp
#include <set>

namespace GnssProcessor {

    // Forward declaration; the full type is only needed in GnssConfigLoad.cpp
    // (which builds the registry) and at consumer call sites.  A shared_ptr to
    // an incomplete type is fine for a class member.
    class StationInfoFile;


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
         * @brief Override the processing mode after load.
         *
         * Used by the closed-loop harness to derive a CONSUMER config
         * (@c ProcessingMode::PPP_RTK) from the network producer config so the
         * held-out station runs the user-side pipeline.  Not part of the normal
         * JSON-driven flow — prefer the config file for production runs.
         *
         * @param mode New processing mode.
         */
        void setProcessingMode(ProcessingMode mode) { processing_mode_ = mode; }

        /**
         * @brief Derive a held-out CONSUMER config from this network producer config.
         *
         * Returns a copy with @c ProcessingMode::PPP_RTK (uncombined PPP-RTK
         * user), the network-forced shared-state estimation flags
         * (@c estimateSatClocks / @c estimateSatPhaseBiases / @c estimateSatBiases /
         * @c promoteSatCodeBias) turned back OFF — the consumer CONSUMES those
         * satellite products from the injected SSR store rather than estimating
         * them — and ambiguity resolution enabled per
         * @c NetworkConfig::closed_loop (the producer itself is float).  All other
         * settings (products, atmosphere, BSSD, antenna) are inherited unchanged
         * so the consumer's correction frame matches the producer's exactly.
         *
         * @return A PPP-RTK consumer @c GnssConfig.
         */
        GnssConfig deriveClosedLoopConsumerConfig() const;

        /**
         * @brief RTA-7(a): the SHARED network-consumer coupling logic.
         *
         * The single source of truth for the non-obvious couplings a config
         * needs to CONSUME network products instead of producing them: flips
         * the mode to @c ProcessingMode::PPP_RTK, turns the producer-forced
         * shared-state estimation flags back off
         * (@c estimate_sat_clocks / @c estimate_sat_phase_biases /
         * @c promote_sat_code_bias / @c estimate_network_sat_code_bias /
         * @c estimate_network_iono_sh + the SH-VTEC block), couples
         * @c estimate_sat_biases to the clock source (@p consume_clock: the
         * network clock folds the satellite code delay, so a full-mode consumer
         * must NOT re-estimate it; a bias-only consumer on the precise clock
         * SHOULD), restores the user-default @c no_bia_code_downweight (100),
         * and applies the user-model gates (@p undifferenced: BSSD off +
         * @c ar_wl_only off + AR position-σ gate capped at 0.20 m; otherwise
         * BSSD on).  Called by BOTH @c deriveClosedLoopConsumerConfig (the
         * in-process closed-loop harness) and @c applyNetworkConsumerProfile
         * (the standalone real-time consumer) so the two can never diverge.
         *
         * @param c             Config to mutate in place.
         * @param consume_clock True when the consumer takes the NETWORK clock
         *                      (drives the @c estimate_sat_biases coupling).
         * @param consume_bias  True when the consumer takes the network
         *                      code/phase bias.  No coupling keys off it today;
         *                      kept in the signature so future bias-side
         *                      couplings land in this one helper.
         * @param undifferenced True for the undifferenced uncombined PPP-RTK
         *                      user model, false for the legacy BSSD consumer.
         */
        static void applyNetworkConsumerCouplings(GnssConfig& c, bool consume_clock,
                                                  bool consume_bias, bool undifferenced);

        /**
         * @brief RTA-7(b): apply the @c processing.network_consumer profile to
         *        THIS config in place (standalone real-time consumer).
         *
         * Runs @ref applyNetworkConsumerCouplings with the profile's
         * clock/bias/undifferenced flags, then arms @c estimation.estimate_ifb
         * when the profile consumes network code biases and it is off — a
         * standalone consumer has no producer config to inherit it from, and
         * without receiver code-bias states a network code-bias datum
         * re-reference has nothing to absorb it (the engine's one-shot A5
         * warning becomes moot but stays in place for non-profile paths).
         *
         * RTA-9(a): additionally arms @c processing.enable_ssr_bias_max_age
         * when the profile consumes biases and the user did NOT explicitly set
         * the key (either the dotted or the legacy flat form — an explicit
         * @c false is respected).  Bias consumption without the staleness gate
         * silently applies unboundedly stale biases across producer withhold
         * gaps; the closed-loop harness is left untouched (this coupling lives
         * in the PROFILE, not the shared couplings helper).
         *
         * Called by @c Application::runRealTime BEFORE the engine is
         * constructed, so the couplings shape the state vector.  The caller
         * gates on @c getNetworkConsumerConfig().enable AND a product source
         * (@c realtime.network_products_path) being present.
         *
         * @param log_manager Logger sink for the applied-profile summary.
         */
        void applyNetworkConsumerProfile(LogManager& log_manager);

        /// RTA-7: the @c processing.network_consumer profile block (see
        /// @c NetworkConsumerConfig).  @c enable defaults false — inert.
        const NetworkConsumerConfig& getNetworkConsumerConfig() const { return network_consumer_config_; }

        /// Standalone @c processing.ar_ignore_mw_wl toggle (default false):
        /// suppress the MW wide-lane feed into ambiguity resolution so AR runs
        /// pure MLAMBDA on the Kalman float (@c RealTimeEngine::setArIgnoreMwWl,
        /// which previously had NO JSON key).  Needed whenever the float carries
        /// network satellite phase biases while the MW WL integers are in the
        /// separate .BIA frame.
        bool arIgnoreMwWl() const { return ar_ignore_mw_wl_; }

        /**
         * @brief Resolve the effective @c setArIgnoreMwWl value for the
         *        network-consumer profile.
         *
         * Mirrors the closed-loop driver exactly: the closed loop calls
         * @c setArIgnoreMwWl(!consumer_config.getArWlOnly()) on its DERIVED
         * config, so with the profile enabled the derived value here is
         * @c !getArWlOnly() evaluated AFTER @ref applyNetworkConsumerProfile has
         * run (the undifferenced couplings force @c ar_wl_only off, yielding
         * true).  An explicit @c processing.network_consumer.ar_ignore_mw_wl
         * overrides; the standalone @c processing.ar_ignore_mw_wl key ORs in
         * either way.  With the profile disabled this is just the standalone key.
         */
        bool networkConsumerArIgnoreMwWl() const {
            const NetworkConsumerConfig& nc = network_consumer_config_;
            if (nc.enable && nc.ar_ignore_mw_wl >= 0) return nc.ar_ignore_mw_wl != 0;
            if (nc.enable) return !ar_wl_only_ || ar_ignore_mw_wl_;
            return ar_ignore_mw_wl_;
        }

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

        /**
         * @brief Max |obs − SSR-record| age (s) before the SSR orbit/clock
         *        overlay is rejected and the satellite falls back to
         *        broadcast-grade (real-time).  Config @c processing.ssr_max_age_s.
         *
         * RTA-9(c) — cadence sizing: this window must comfortably contain the
         * correction provider's update interval or satellites flap between
         * cm-grade and broadcast-grade every cycle.  Rule of thumb: size it so
         * the producer emit cadence ≪ ssr_max_age/2 (the 30 s default suits the
         * 5-10 s IGS-RTS / network-product cadences; a 30 s-cadence provider
         * needs ≥ 60 s here).  For network products, producer WITHHOLD gaps
         * (RAIM/QC epochs that emit nothing for a satellite) can exceed the
         * cadence — @ref getSsrClockStaleHoldFactor buys margin for those
         * without loosening this base gate.
         */
        double getSsrMaxAge() const { return ssr_max_age_s_; }

        /**
         * @brief RTA-9(b): SSR clock stale-hold factor
         *        (@c processing.ssr_clock_stale_hold_factor, default 0 = off).
         *
         * The middle rung of the clock staleness ladder.  When > 0, a
         * satellite clock whose age lies in
         * (@ref getSsrMaxAge, factor × @ref getSsrMaxAge] is STILL APPLIED —
         * the existing C0 + C1·dt polynomial evaluated over the true dt —
         * instead of dropping the satellite to broadcast-grade (where
         * @c drop_broadcast_grade would wipe it).  The rows consuming a held
         * clock are de-weighted through the EXISTING URA-staleness machinery in
         * @c MeasurementModel: the ZOH term q²·dt³/3 grows with the true age
         * (q² = @ref networkUraStalenessQ2, with a documented fallback when the
         * URA channel is absent).  Beyond factor × max_age the original
         * broadcast-grade cliff applies.  Enforced where the clock lookup
         * enforces @c ssr_max_age — @c GnssCorrections::getSatelliteClockCorrection
         * passes @c SsrCorrectionStore::clockHoldWindow(max_age, factor) into
         * the store's freshness gate.  0 (default) is byte-identical; values in
         * (0, 1] clamp to 1 (no hold).  Sized against producer withhold gaps —
         * see @ref getSsrMaxAge for the cadence rule.
         */
        double getSsrClockStaleHoldFactor() const { return ssr_clock_stale_hold_factor_; }

        /**
         * @brief RTA-13 (opt-in): CODE-domain band-wise SSR bias fallback
         *        (@c processing.ssr_bias_band_fallback, default false).
         *
         * When true, an SSR code-bias lookup that misses on the exact RINEX obs
         * code but finds a SAME-BAND different-attribute sibling (receiver
         * tracks C5X, stream carries C5Q) consumes the sibling's bias; the
         * residual attribute DCB is absorbed by the receiver code-bias states
         * (@c estimate_ifb, armed by the network-consumer profile).  NEVER
         * applies to phase biases — a wrong-attribute phase bias destroys the
         * integer property — phase stays exact-match with count-only
         * diagnostics (see @c SsrCorrectionStore::biasAttributeMismatchCounts).
         * Armed on the singleton by @c RealTimeEngine::initialize and on the
         * owned network-product store by @c Application::runRealTime.
         */
        bool ssrBiasBandFallback() const { return ssr_bias_band_fallback_; }

        /// @brief Max |obs − SSR-bias-record| age (s) before the SSR code/phase-bias
        ///        overlay is rejected (A5).  Config @c processing.ssr_bias_max_age_s
        ///        (default matches @ref getSsrMaxAge).  Only enforced when
        ///        @ref enableSsrBiasMaxAge; the code/phase-bias getters otherwise never gate.
        double getSsrBiasMaxAge() const { return ssr_bias_max_age_s_; }

        /// @brief Whether the SSR code/phase-bias staleness gate is enforced
        ///        (@c processing.enable_ssr_bias_max_age, A5).  Default false =
        ///        the bias getters never gate on age (byte-identical).
        bool enableSsrBiasMaxAge() const { return enable_ssr_bias_max_age_; }

        /// @brief Stale-hold factor for the SSR bias staleness gate (A5).  The
        ///        APPLY gate (and the freshness-flip ambiguity reset keyed to it)
        ///        drops a bias only past @c hold_factor × @ref getSsrBiasMaxAge():
        ///        between the two limits the last record keeps being applied, so a
        ///        provider whose update interval hovers near the max age produces
        ///        NO model step and NO reset — one flip per genuine outage instead
        ///        of a reset storm per update cycle.  A held bias is applied at
        ///        FULL weight (no variance penalty in the hold zone; the separate
        ///        URA/staleness channel — @c network.consume_ura, keyed to
        ///        @c processing.ssr_max_age_s — is independent of this factor).
        ///        1.0 = no hold (drop exactly at max age).  Config
        ///        @c processing.ssr_bias_stale_hold_factor, default 2.0; clamped to ≥ 1.
        double getSsrBiasStaleHoldFactor() const { return ssr_bias_stale_hold_factor_; }

        /**
         * @brief obs↔SSR sync master toggle (real-time, live-SSR source only).
         *
         * When true the engine HOLDS observation epochs newer than the freshest
         * SSR content epoch + @c getSsrMaxAge — epochs no SSR can yet correct —
         * draining them once the SSR stream advances.  This keeps the obs stream
         * time-ALIGNED with the corrections instead of pinned to wall clock;
         * PPP-RTK needs alignment, not freshness.  Unlike @c enableObsSkipToLatest
         * (which fast-forwards obs to the bleeding edge, maximising the
         * obs-ahead-of-SSR gap), this bounds the obs front by the corrections.
         * A stalled SSR feed is bounded by @c getObsSsrSyncMaxHoldS, after which
         * the engine processes degraded (broadcast-grade) epochs so a dead feed
         * can never starve the filter.  Default off; opt-in via
         * @c processing.enable_obs_ssr_sync.  Inert unless the run uses a live
         * orbit/clock source (@c realtimeUsesLiveOrbitClockSource).
         */
        bool enableObsSsrSync() const { return enable_obs_ssr_sync_; }

        /// @brief Max seconds to hold an obs epoch waiting for SSR before falling
        ///        back to degraded (broadcast-grade) processing — the obs↔SSR
        ///        sync liveness bound.  Config @c processing.obs_ssr_sync_max_hold_s.
        double getObsSsrSyncMaxHoldS() const { return obs_ssr_sync_max_hold_s_; }

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
         * @brief Check whether receiver antenna azimuth orientation is applied.
         *
         * When true, the receiver PCV look-up azimuth is corrected for the
         * physical mounting orientation of the antenna (azimuth measured
         * clockwise from local North) before interpolating the ANTEX PCV
         * grid: @c az_pcv = wrap(az_measured - orientation_azimuth).  Only the
         * receiver PCV is affected — the satellite PCV (computed in the
         * satellite body frame) and the receiver PCO (a line-of-sight
         * projection, not an azimuth look-up) are intentionally unchanged.
         * Off by default so legacy runs stay byte-identical.
         *
         * @return True if receiver antenna orientation correction is enabled.
         * @see getReceiverAntennaOrientation()
         */
        bool enableRxAntennaOrientation() const { return enable_rx_antenna_orientation_; }

        /**
         * @brief Check whether receiver antenna model substitution is allowed.
         *
         * When true and the RINEX receiver antenna type has no matching ATX
         * entry, a rule-driven substitution is attempted before giving up: an
         * explicit deprecated-code table followed by the IGS radome→NONE
         * fallback (use the radome-agnostic "NONE" calibration when the
         * specific radome is uncalibrated).  The first valid substitute is
         * used and a warning is logged.  Off by default so a missing model
         * still yields a zero receiver correction exactly as before.
         *
         * @return True if receiver antenna substitution is enabled.
         */
        bool enableRxAntennaSubstitution() const { return enable_rx_antenna_substitution_; }

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
         * @brief Check if the a-posteriori variance factor (σ̂₀²) is computed.
         *
         * When true, the Kalman filter forms the a-posteriori variance of
         * unit weight σ̂₀² = (Σ vᵢ²/Rᵢᵢ)/dof from the post-fit residuals each
         * epoch (dof = Σ redundancy numbers), logs it on the residual-summary
         * line, and rescales a *reporting copy* of the position covariance
         * (σ_pos → formal·√σ̂₀²).  The propagated covariance P is never
         * touched, so the trajectory stays byte-identical to the flag-off
         * path.  Diagnostic only; estimation-domain toggle.
         *
         * @return True if the variance factor is computed and reported.
         */
        bool enableVarianceFactor() const { return enable_variance_factor_; }

        /**
         * @brief Check if Baarda data-snooping QC is enabled.
         *
         * When true the filter forms, per observation row, the redundancy
         * number rᵢ = Sr_ii/Rᵢᵢ, the Baarda w-test statistic
         * wᵢ = vᵢ/√Sr_ii, and the minimal detectable bias
         * MDBᵢ = σᵢ·δ₀/√rᵢ; surfaces them in the residual trace; flags
         * low-redundancy rows (rᵢ below an internal floor) as uncheckable;
         * and drives the post-fit outlier gate on |wᵢ| >
         * dataSnoopingCritical() over checkable rows instead of the fixed σ
         * threshold.  Default off → the existing outlier path is
         * byte-identical.
         *
         * @return True if Baarda data-snooping is enabled.
         */
        bool enableDataSnooping() const { return enable_data_snooping_; }

        /**
         * @brief Baarda w-test critical value (standardized-residual reject bound).
         *
         * Default 3.29 ≙ two-sided per-test significance α₀ = 0.001
         * (Baarda B-method).  Only consulted when enableDataSnooping() is
         * true.
         *
         * @return The |w| rejection threshold.
         */
        double dataSnoopingCritical() const { return data_snooping_critical_; }

        /**
         * @brief Check if the off-diagonal measurement covariance is honoured
         *        in the post-fit residual kernel.
         *
         * The gain/Joseph path already uses the full R whenever a dense
         * R_full block is supplied (BSSD / baseline-DD epochs).  When this
         * flag is true the *post-fit residual* covariance is also formed as
         * the full Sr = R − H·P⁺·Hᵀ (not just its diagonal), so the
         * redundancy numbers and w-test (see enableDataSnooping()) honour
         * correlated observations exactly.  Default off → the diagonal
         * standardization path is byte-identical.  Estimation-domain toggle.
         *
         * @return True if the off-diagonal R kernel is active.
         */
        bool enableFullRKernel() const { return enable_full_r_kernel_; }

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
         * @brief Gets the ocean pole tide load grid path (opoleloadcoefcmcor.txt).
         * @return Explicit grid path, or empty to use the internal static default.
         */
        std::filesystem::path getOptGridPath() const { return opt_grid_path_; }

        /**
         * @brief Gets the S1/S2 atmospheric-tide load grid path.
         * @return Explicit grid path, or empty to use the internal static default
         *         (<internal_data_root>/static/apl/).
         */
        std::filesystem::path getAtmTideGridPath() const { return atm_tide_grid_path_; }

        /**
         * @brief Whether the Station Information File (SIF) subsystem is enabled.
         *
         * When true and a SIF was successfully loaded, per-station metadata
         * (ocean tide loading, ocean pole tide, ITRF coordinates/velocity, and
         * antenna/receiver history) takes precedence over grid-interpolated or
         * RINEX-header sources for matched stations.  Defaults to false, which
         * keeps runs byte-identical to the pre-SIF behaviour.
         * @return True if SIF consumption is enabled.
         */
        bool enableStationInfo() const { return enable_station_info_; }

        /**
         * @brief SIF JSON file paths in load order (explicit or auto-discovered).
         * @return Resolved SIF paths; empty if none configured/found.
         */
        const std::vector<std::filesystem::path>& getStationInfoPaths() const { return station_info_paths_; }

        /**
         * @brief The loaded station-metadata registry, if any.
         *
         * Built once at config-load time from @c getStationInfoPaths().  Shared
         * (read-only) across all consumers and copies of this config.
         * @return Registry pointer, or @c nullptr when SIF is disabled / empty.
         */
        std::shared_ptr<const StationInfoFile> getStationInfo() const { return station_info_; }

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
         * @brief Check if cross-session normal-equation export is enabled.
         *
         * When true, the post-processing run accumulates a label-keyed normal
         * system alongside the Kalman filter — N += HᵀR⁻¹H, u += HᵀR⁻¹Δz,
         * ltpl += ΔzᵀR⁻¹Δz — eliminating epoch-local parameters (the receiver
         * clock) by per-epoch Schur reduction so the retained system spans
         * only the time-constant/persistent states, then writes
         * `<navigation_solution>.neq` (compact, round-trippable) and `.asc`
         * (human-readable) at end of run with the a-posteriori variance
         * factor σ̂₀².  The normal equations are accumulated from the same
         * (H, R, innovation) the filter accepts; the filter state is never
         * mutated.
         *
         * Post-processing only; forced off in real-time mode.
         *
         * @return True if the NEQ accumulator runs and exports.
         */
        bool enableNeqExport() const { return enable_neq_export_; }

        /**
         * @brief Whether the non-causal cycle-slip bridge runs before the smoother.
         *
         * When enabled (and @c runSmoother() is true), a post-forward pass
         * re-estimates the integer jump at each ambiguity-reset boundary from
         * both-sides GF+MW windows and, if a conservative independent-float gate
         * passes, bridges the smoother segments so the arc smooths as one.
         * Post-processing only; ignored in real-time mode.
         *
         * @return True if the non-causal slip bridge should run.
         */
        bool enableNoncausalSlipBridge() const { return enable_noncausal_slip_bridge_; }

        /**
         * @brief Whether SP3 satellite-position interpolation uses the
         *        sidereal-trigonometric (sin/cos) basis.
         *
         * When false (default) the legacy polynomial (Neville) path is used and
         * behaviour is byte-identical.  When true, position interpolation fits a
         * sin/cos series keyed to the satellite's near-sidereal orbital period
         * via Interpolate::trigSvdInterp (an 11-point LS smoother at the default
         * window size) to reduce the SP3 epoch-boundary sawtooth.
         *
         * Configured via @c processing.enable_trig_sp3_interp.  Default false.
         * @return True if sidereal-trig SP3 interpolation is enabled.
         */
        bool enableTrigSp3Interp() const { return enable_trig_sp3_interp_; }

        /**
         * @brief Whether satellites use beta-driven orbit-normal yaw attitude.
         *
         * When false (default) nominal yaw-steering is assumed (with the legacy
         * hard-coded BeiDou-2 GEO orbit-normal exception) and the satellite body
         * frame is byte-identical to the historical inline computation.  When
         * true, the body-frame mode is selected per the M-PAGES-style beta
         * threshold table (BeiDou beta<4 deg -> orbit-normal, QZSS beta<20 deg ->
         * orbit-normal label) via SatAttitude::compute.
         *
         * Configured via @c processing.enable_orbit_normal_attitude.  Default false.
         * @return True if orbit-normal attitude selection is enabled.
         */
        bool enableOrbitNormalAttitude() const { return enable_orbit_normal_attitude_; }

        /**
         * @brief Half-window size (epochs) used per side by the bridge fit.
         * @return Number of epochs sampled on each side of a reset boundary.
         */
        int getSlipBridgeHalfWindowEpochs() const { return slip_bridge_half_window_epochs_; }

        /**
         * @brief Accept tolerance (cycles) for the independent pre-bridge-float gate.
         *
         * The recovered integer step @f$ dN\cdot\lambda @f$ must agree with the
         * difference of the two independent forward floats (pre-slip converged vs
         * independently re-converged post-reset) to within this many cycles, or
         * the bridge is rejected and the boundary stays segmented.
         *
         * @return Maximum allowed disagreement in cycles.
         */
        double getPreBridgeFloatTolCycles() const { return pre_bridge_float_tol_cycles_; }

        /**
         * @brief Variance (m^2) below which a post-reset float counts as re-converged.
         * @return Diagonal-covariance threshold for the independent re-converged float.
         */
        double getPreBridgeConvVarM2() const { return pre_bridge_conv_var_m2_; }

        /**
         * @brief Whether to auto-generate an HTML report at the end of a
         *        post-processing run.
         *
         * When true, @c run() invokes the bundled @c pyviz/run_report.py against
         * the run's trace folder and writes @c gnss_report.html there.  Failure
         * to produce the report is non-fatal (logged as a warning).  No-op in
         * real-time mode.  JSON: @c processing.auto_report (default false).
         *
         * @return True if the end-of-run report should be generated.
         */
        bool autoReport() const { return enable_auto_report_; }

        /**
         * @brief Rendering backend for the auto report.
         * @return "matplotlib" (static PNG, default) or "plotly" (interactive).
         */
        const std::string& reportBackend() const { return report_backend_; }

        /**
         * @brief Optional python interpreter override for the auto report.
         * @return Explicit interpreter path, or "" to use the baked
         *         @c <project_root>/venv interpreter (falling back to PATH).
         */
        const std::string& reportPython() const { return report_python_; }

        /**
         * @brief Curated viewer subset for the auto report.
         * @return Viewer names passed to run_report.py via @c --only; empty =
         *         render every viewer.  JSON: @c processing.report_viewers.
         */
        const std::vector<std::string>& reportViewers() const { return report_viewers_; }

        /**
         * @brief Whether to launch the auto report detached (non-blocking).
         * @return True to fire-and-forget so the run returns without waiting on
         *         rendering; false (default) to run synchronously and log the
         *         outcome.  JSON: @c processing.report_async.
         */
        bool reportAsync() const { return report_async_; }

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
         * @brief Check if the constant-velocity slant-iono model is enabled.
         *
         * When true, each per-satellite slant ionosphere state I_s gains a
         * companion rate state Idot_s, forming a constant-velocity pair with
         * transition [[1,dt],[0,1]] and a coupled CWNA process-noise block
         * driven by @c process_noise.iono_rate_psd.  The rate carries the
         * largely-deterministic slant-iono ramp (obliquity change, pierce-point
         * motion, diurnal VTEC trend), so the level prior tightens dramatically
         * — decorrelating the ionosphere from the ambiguities (faster/stronger
         * AR and float position) and turning an iono disturbance (scintillation,
         * TID, storm onset) into a large in-band innovation instead of noise
         * buried in the ramp.  Active only in PPP-RTK uncombined mode; the rate
         * state is unobserved directly (H=0) and driven purely by the
         * F-coupling into I_s, exactly like the receiver clock-rate.  Orthogonal
         * to @c adaptive_iono_process_noise (which inflates the rate PSD in
         * storms).  Defaults off — when off the iono is the classic random walk.
         *
         * @return True if per-satellite iono-rate estimation is enabled.
         */
        bool enableIonoRate() const { return enable_iono_rate_; }

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
         * @brief Estimate a per-FCN-slot GLONASS receiver code IFB.
         *
         * GLONASS is FDMA: every slot k transmits on a distinct carrier
         * frequency, so the receiver group delay differs per slot.  A single
         * per-band IFB/DCB state cannot represent that.  When true, the engine
         * estimates one receiver code-bias state per (FDMA band, slot k) with
         * unit coefficient (key "GLONASS_IFB<band>_k<±slot>"); the central
         * channel k=0 is the held datum (no state).  These per-slot states
         * survive between-satellite single differencing (unlike receiver-common
         * DCB/IFB), so GLONASS code can contribute cleanly under BSSD.
         *
         * Requires @c estimate_ifb=true and uncombined PPP-RTK; default false
         * keeps the GLONASS path byte-identical.  See M-PAGES backlog item #4.
         */
        bool enableGlonassIfb() const { return enable_glonass_ifb_; }

        /**
         * @brief Admit GLONASS to the PPP-RTK NETWORK producer (code-only scope).
         *
         * When set, GLONASS satellites contribute to the joint network solve with
         * a per-satellite shared CLOCK column (the integer/clock product), while
         * the FDMA receiver group delay is carried by the per-FCN-slot receiver
         * IFB as a LOCAL nuisance (Schur-eliminated, never emitted).  NO satellite
         * phase bias is emitted for GLONASS: a single shared per-(sat,signal) phase
         * bias is unphysical for FDMA (the receiver group-delay-vs-frequency
         * response is per-receiver/per-antenna and so not portable to consumers).
         *
         * Network mode only.  Requires uncombined PPP-RTK and the per-slot IFB
         * machinery; @c loadNetworkSection_ forces @c estimate_ifb +
         * @c enable_glonass_ifb on and requires GLONASS in @c active_systems (its
         * observations would otherwise pin the receiver clock and corrupt the
         * GPS/Galileo products).  Default false keeps the GPS+Galileo network
         * byte-identical.
         */
        bool enableGlonassNetwork() const { return enable_glonass_network_; }

        /**
         * @brief Estimate satellite CODE biases (OSB) in the PPP-RTK network producer.
         *
         * When on (@c network.estimate_sat_code_bias), the network promotes the
         * per-(satellite, signal) code bias to a first-class shared border column
         * (GPS + Galileo, raw code signals) so the engine emits its OWN satellite
         * code-bias product instead of relying on an external @c .BIA.  It is the
         * single switch that lands the coupled W6 slice: @c loadNetworkSection_
         * also forces @c estimate_sat_biases / @c promote_sat_code_bias / @c
         * estimate_ifb (receiver DCB) on and the GIM ionosphere weighting on (the
         * load-bearing iono constraint that makes the satellite DCB observable),
         * and forces @c enable_code_phase_bias OFF so the network estimates the
         * full bias from scratch rather than a residual to @c .BIA.  Default false
         * keeps the clock+phase-bias-only V1 network byte-identical.
         */
        bool estimateNetworkSatCodeBias() const { return estimate_network_sat_code_bias_; }

        /**
         * @brief Co-estimate the global SH-VTEC ionosphere in the network producer.
         *
         * When on (@c network.estimate_iono_sh), the network producer estimates its
         * OWN global single-layer spherical-harmonic VTEC field jointly with the
         * satellite code biases, replacing the external GIM (`.INX`) as the iono
         * constraint that makes the satellite DCB observable.  It FORCES
         * @c network.estimate_sat_code_bias on (the W6 coupled slice — sat code bias
         * + rx DCB + iono weighting machinery) and switches the iono row builder from
         * the external-GIM target to the co-estimated SH field.  Mutually exclusive
         * with loading an external GIM for the same purpose (no double-apply).
         * Default false keeps the external-GIM W6 path byte-identical.
         */
        bool estimateNetworkIonoSh() const { return estimate_network_iono_sh_; }

        /**
         * @brief A-priori satellite orbit/clock source for the network producer
         *        (@c network.apriori_source).
         *
         * @c Sp3 (default) resolves precise SP3(+CLK) files — the emitted clock
         * C0 is a delta against the SP3/CLK product identity (today's behaviour,
         * byte-identical everywhere).  @c BrdcRts references the a-priori clock to
         * the broadcast ephemeris (@c LiveEphemerisStore) plus external IGS-SSR
         * (4076) orbit corrections; the estimable clock state then absorbs the
         * broadcast-clock error and the emitted C0 becomes a delta against
         * broadcast(IODE) + the applied RTS orbit (the self-sufficient real-time
         * service mode — see @c docs/rt_service_conventions.md §1).  The choice is
         * orthogonal to @c network.service_mode (an input-source selection, not a
         * bundled toggle) — @c service_mode does NOT arm it.  Consumed by the
         * producer core path (later RTF-1 cards); this getter is config plumbing.
         */
        NetworkAprioriSource networkAprioriSource() const { return network_apriori_source_; }

        /**
         * @brief A-priori satellite-clock anchor pseudo-observation σ (m) for the
         *        network producer (@c network.apriori_clock_anchor_sigma_m).
         *
         * The per-satellite a-priori clock is bound to the solve by a loose anchor
         * pseudo-observation whose information is @f$1/\sigma^2@f$.  @c 0.0
         * (default) means AUTO: the mode-appropriate value is resolved at the
         * producer (unchanged in @c Sp3 mode; broadcast-grade ~3 m in @c BrdcRts,
         * per @c docs/rtf12_brdc_rts_plan.md D6).  An explicit positive value
         * overrides the auto in both modes.  Parsed and stored here; consumed by
         * the producer core (a later RTF-1 card).
         */
        double networkAprioriClockAnchorSigmaM() const { return network_apriori_clock_anchor_sigma_m_; }

        /**
         * @brief Card 5 (plan D6): the RESOLVED a-priori satellite-clock anchor σ (m)
         *        the network producer actually folds, given the mode + AUTO policy.
         *
         * Resolution (single source of truth — the driver's anchor fold AND its B7
         * information-fraction diagnostic both call this so they never diverge):
         *   1. explicit @c network.apriori_clock_anchor_sigma_m > 0 → that value,
         *      in BOTH modes;
         *   2. else @c BrdcRts (auto) → @c 3.0 m (broadcast-grade; conventions §1 —
         *      binds the anchor because the driver's < 100 m gate then passes);
         *   3. else @c Sp3 (auto) → @c initial_std.sat_clock, UNCHANGED (the default
         *      1000 m leaves the anchor OFF at the < 100 m gate ⇒ every existing sp3
         *      network config is byte-identical).
         *
         * This does NOT apply the < 100 m binding gate (the driver keeps that) and
         * does NOT touch @c initial_std.sat_clock (the mode-independent loose state
         * init).  Pure function of config members — unit-testable in isolation.
         *
         * @return The resolved anchor σ in metres.
         */
        double resolveNetworkAprioriClockAnchorSigmaM() const {
            const double explicit_sigma = network_apriori_clock_anchor_sigma_m_;
            if (explicit_sigma > 0.0) return explicit_sigma;   // explicit wins in both modes
            if (network_apriori_source_ == NetworkAprioriSource::BrdcRts)
                return 3.0;                                    // brdc_rts auto (conventions §1 / D6)
            return initial_std_.sat_clock;                     // sp3 auto (unchanged)
        }

        /**
         * @brief Carry per-station carrier-phase ambiguities across epochs in the
         *        PPP-RTK network producer (phase-leveling, Increment 1).
         *
         * When on (@c network.phase_leveling), each non-pivot station's float
         * ambiguities are carried as a diagonal information prior on its own local
         * block (injected via @ref NetworkEstimator::addStationPrior before the
         * Schur reduction), so the network finally extracts carrier-phase precision
         * — not just code — into the satellite clock / phase-bias / code-bias
         * products.  The carry is reduction-orthogonal to the shared border and to
         * the datum gauge, so it sharpens the locals without touching the
         * minimum-constraint S-basis.  Default false keeps the per-epoch ABSOLUTE
         * snapshot solve byte-identical.
         */
        bool networkPhaseLeveling() const { return network_phase_leveling_; }

        /**
         * @brief Process noise (m²/s) inflating a carried ambiguity's variance over
         *        the inter-epoch gap (@c network.phase_leveling_q_amb).
         *
         * @c var_eff = var + q_amb·Δt_since_last_seen.  Must be @c >= 0.
         *
         * The physically correct process model for a carrier-phase ambiguity is a
         * RANDOM CONSTANT punctuated by discrete cycle-slip resets — i.e. ZERO
         * process noise between slips.  With @c q_amb = 0 (the default) the carried
         * ambiguity therefore accumulates carrier-phase precision indefinitely,
         * which is the whole point of phase-leveling: continuity is enforced by the
         * IF-phase / MW-GF slip detectors (which refloat a slipped arc) and long
         * gaps by @ref networkPhaseLevelingMaxGapS (which drops and re-acquires),
         * NOT by continuous dilation.  A small positive @c q_amb is available only
         * as an optional SOFT gap-reacquisition knob (weakly re-floats an ambiguity
         * across a gap without fully dropping it); note it also caps steady-state
         * accumulation at @c ~q_amb·Δt and so partially defeats the unlock.
         */
        double networkPhaseLevelingQAmb() const { return network_phase_leveling_q_amb_; }

        /**
         * @brief Emitted-epoch count before a carried ambiguity is trusted
         *        (@c network.phase_leveling_warmup_epochs).
         *
         * Cold-start epochs are ill-conditioned (the clock/SH field has not
         * converged); carrying an ambiguity before then would freeze a contaminated
         * value.  No carry is stashed or injected until this many product epochs
         * have been emitted.  (A convergence-tied warmup is the intended refinement.)
         */
        int networkPhaseLevelingWarmupEpochs() const { return network_phase_leveling_warmup_epochs_; }

        /// @brief T1d: gate the leveling-carry warmup on SH-field convergence instead of the
        ///        fixed emitted-epoch count (@c network.phase_leveling_warmup_on_convergence).
        bool networkPhaseLevelingWarmupOnConvergence() const { return network_phase_leveling_warmup_on_convergence_; }
        /// @brief T1d: one-shot refloat of all carried ambiguities at the SH-field convergence
        ///        rising edge (@c network.phase_leveling_refloat_at_convergence).
        bool networkPhaseLevelingRefloatAtConvergence() const { return network_phase_leveling_refloat_at_convergence_; }
        /// @brief T1d: SH coefficient-vector RMS-change threshold (TECU) defining field
        ///        stationarity for the convergence latch (@c network.phase_leveling_conv_rms_thresh_tecu).
        double networkPhaseLevelingConvRmsThreshTecu() const { return network_phase_leveling_conv_rms_thresh_tecu_; }

        /**
         * @brief Maximum gap (s) a carried ambiguity survives unseen before it is
         *        dropped (@c network.phase_leveling_max_gap_s).
         *
         * Bounds the carry container and prevents re-injecting a stale value when a
         * satellite returns after a long outage (where a slip is likely and, in
         * Increment 1, undetected).  A benign housekeeping drop — distinct from the
         * slip-driven REFLOAT/DROP of later increments.
         */
        double networkPhaseLevelingMaxGapS() const { return network_phase_leveling_max_gap_s_; }

        /**
         * @brief Floor (m²) on a carried ambiguity's effective prior variance — a
         *        lock-in guard (@c network.phase_leveling_var_floor_m2).
         *
         * The full §3.7 fading-memory guard is co-owned by the slip tiers (T0/T1/T2,
         * later increments): a sub-cycle/multipath bias that never trips a slip
         * detector would otherwise see its carried variance collapse monotonically
         * and freeze a contaminated value into the products.  This single scalar caps
         * the carried information (@c var_eff ≥ floor ⇒ @c Λ ≤ 1/floor) so fresh data
         * can always pull a slowly-biasing ambiguity back.  Default 0 disables it
         * (no cap), keeping behaviour identical until the user opts in for longer runs.
         */
        double networkPhaseLevelingVarFloorM2() const { return network_phase_leveling_var_floor_m2_; }

        /// @brief Floor (m²) on the D2 satellite-PHASE-bias carried prior variance
        ///        (@c network.shared_carry_var_floor_pb_m2, B4).  Caps the carry's
        ///        information so a stiff zero-q phase-bias carry cannot lock a biased
        ///        product in, while preserving the FCB time-averaging; 0 = off.
        double networkSharedCarryVarFloorPbM2() const { return network_shared_carry_var_floor_pb_m2_; }
        /// @brief Floor (m²) on the D2 satellite-CODE-bias carried prior variance
        ///        (@c network.shared_carry_var_floor_cb_m2, B4); 0 = off.
        double networkSharedCarryVarFloorCbM2() const { return network_shared_carry_var_floor_cb_m2_; }

        /**
         * @brief Whether the D2 shared-parameter carry keeps the FULL dense
         *        cross-covariance of the shared border instead of per-key
         *        variances only (@c network.shared_carry_full_cov, C1).
         *
         * Legacy (OFF): each carried shared parameter (satellite clock in
         * scalar-carry mode, phase bias, code bias) is re-anchored next epoch by
         * an INDEPENDENT diagonal prior @c Λ_k = 1/(var_k + q·dt) — every
         * cross-covariance the solve produced is discarded at the stash.  For
         * stiff carries (zero-q phase biases) that diagonal approximation is
         * statistically inconsistent: information accumulates against a
         * correlated error floor, the formal variance collapses, and a biased
         * value can lock in (the mechanism the post-fit RAIM / clock-monitor
         * tiers compensate for).  The strongest discarded terms are the
         * clock↔phase-bias off-diagonals of each satellite.
         *
         * When ON, the stash keeps the dense posterior covariance block over the
         * carried keys (@ref NetworkEstimator::sharedCovariance, identity-keyed
         * so it survives layout changes) and the next epoch folds
         * @c Λ = (P_prev + Q)⁻¹ as ONE dense information block
         * (@ref NetworkEstimator::addSharedPriorDense) — the epoch loop becomes
         * an information-form Kalman filter on the shared border.  Departed,
         * datum-pinned, handover-retired, and clock-model-carried keys are
         * MARGINALIZED (rows/cols of P dropped before inversion); per-key
         * process noise, clock-monitor soft reinits (decorrelated), and the B4
         * variance floors apply to P's diagonal exactly as they apply to the
         * scalar path.  The SH-VTEC coefficients keep their own diagonal
         * pre-gate fold (separate physics), and under @ref networkClockModel the
         * satellite clock stays on the two-state oscillator carry, outside the
         * dense block.  A numerically non-invertible carried block degrades to
         * the legacy diagonal fold for that epoch (warned + event-logged), and a
         * warm resume seeds diagonally from the v1 snapshot for one epoch — the
         * toggle is therefore deliberately NOT part of the warm-resume config
         * fingerprint (both modes carry the same per-key values/variances).
         * Default false = the legacy diagonal carry (byte-identical).
         *
         * @see networkSharedCarryVarFloorPbM2, networkClockMonitorReinitSigmaM,
         *      NetworkEstimator::addSharedPriorDense
         */
        bool networkSharedCarryFullCov() const { return network_shared_carry_full_cov_; }

        /**
         * @brief Carry each persistent LOCAL parameter (phase-leveling ambiguity,
         *        ZWD) across epochs as a CONDITIONAL pseudo-observation row coupling
         *        it to the shared border, instead of an independent diagonal prior
         *        (@c network.carry_conditional_coupling, Card 1).
         *
         * The legacy per-station carry (@ref networkPhaseLeveling /
         * @ref networkCarryZwd) re-injects each local's absolute value with its
         * MARGINAL variance as a diagonal prior on the local column alone
         * (@c NetworkNormalEquations::addStationPrior).  That marginal already
         * encodes the shared border (through the Schur reduction), so re-asserting
         * it while the dense border carry ALSO re-asserts the border DOUBLE-COUNTS
         * the shared information; compounded over epochs the formal variance
         * collapses (blinded only by the @c shared_carry_var_floor_* floors) and a
         * biased value can lock in.
         *
         * When ON, the local carry becomes ONE pseudo-observation row spanning
         * @f$(x_s\mid x_r)@f$: coefficient @c +1 on the local column, the posterior
         * regression @f$M_r=N_{sr}N_{rr}^{-1}e_r@f$ (restricted to the FREE, non-
         * datum-pinned shared columns) on the border, and measurement variance the
         * PURE conditional variance @f$(N_{rr}^{-1})_{rr}@f$ (+ the same per-column
         * process noise).  Departed shared columns are marginalized into the row
         * variance (@f$+M_{rj}^2 P_{\text{prev},jj}@f$).  With the local information
         * now flowing conditionally and the border information carried ONLY by the
         * dense border carry, the double count is gone by construction — the
         * sequential recursion equals the batch solve.
         *
         * REQUIRES @ref networkSharedCarryFullCov (a hard error at load otherwise):
         * the conditional design is exact only against a dense border carry.  The
         * plain @c addStationPrior path is not executed for the carried columns when
         * this is armed.  Default false = the legacy diagonal carry (byte-identical).
         *
         * @see networkSharedCarryFullCov, networkPhaseLeveling, networkCarryZwd,
         *      NetworkEstimator::stationRegression
         */
        bool networkCarryConditionalCoupling() const { return network_carry_conditional_coupling_; }

        /**
         * @brief JOINTLY whiten a station's conditional coupling rows instead of
         *        marginalizing each departed shared column per-row into a scalar
         *        variance (@c network.carry_coupling_joint_whiten, Card 1b).
         *
         * Card 1 (@ref networkCarryConditionalCoupling) marginalizes a shared column
         * that has DEPARTED the layout into the coupling row's SCALAR noise
         * (@f$\sigma^2 \mathrel{+}= \sum_{j\,\text{departed}} M_{rj}^2 P_{\text{prev},jj}@f$).
         * That scalar is exactly the DIAGONAL of the true joint marginalization
         * covariance @f$M_w\,\mathrm{diag}(P_w)\,M_w^\top@f$ — it drops the OFF-diagonal
         * correlation between the station's coupling rows that share a departed column.
         * In any between-station double difference those common departed-column terms
         * CANCEL, so discarding their correlation deweights every row individually to
         * nothing while remaining honest: the carried ambiguities' formal σ goes flat
         * (the observed T7 signature) even as the σ-calibration stays ≈ 1.
         *
         * When ON, the station's coupling rows are built AS A GROUP with the full joint
         * row covariance @f$R = \mathrm{diag}(\text{var\_eff}_r) + M_w\,\mathrm{diag}(P_w)\,M_w^\top@f$
         * (@f$M_w@f$ = the coefficients on the distinct departed shared columns,
         * @f$P_w@f$ = each departed column's last-epoch datum-frame marginal variance)
         * and LLᵀ-whitened by the SAME factor (mirroring @c appendShIonoRows), so the
         * common departed directions act common-mode-free and the between-row (DD)
         * information ORTHOGONAL to them is preserved instead of destroyed.  A per-station
         * @f$O(n_\text{carried})@f$ LLT; on a non-SPD factor it retries once with a
         * @f$10^{-12}\cdot\max\mathrm{diag}@f$ ridge, then falls back to the Card-1 scalar
         * path for that station this epoch (counted + warned).
         *
         * REQUIRES @ref networkCarryConditionalCoupling (a hard error at load otherwise).
         * Default false = the Card-1 per-row scalar marginalization (byte-identical).
         *
         * @note Present shared columns still enter as ordinary @f$A_s@f$ coefficients
         *       (unchanged from Card 1); only the DEPARTED-column marginalization is
         *       upgraded from scalar to joint.  A present-but-un-stashed WHITE per-epoch
         *       state (e.g. a satellite clock carried by the oscillator outside the dense
         *       D2 carry under @c clock_model) is NOT yet reclassified into @f$M_w@f$ —
         *       see docs/HANDOFF_carry_rigor_card1b.md (the a-priori-vs-posterior variance
         *       for that case is scenario-dependent and pending the card's empirical
         *       pre-verification).
         *
         * @see networkCarryConditionalCoupling, networkSharedCarryFullCov
         */
        bool networkCarryCouplingJointWhiten() const { return network_carry_coupling_joint_whiten_; }

        /**
         * @brief Apply the shared-border TIME UPDATE to each conditional coupling
         *        row at injection (@c network.carry_coupling_time_update, Card 1d).
         *
         * A coupling row harvested at epoch @f$k@f$ is an exact pseudo-observation
         * of @f$u = r + M_r^\top s@f$ <em>at the harvest epoch</em>
         * (@f$\mathrm{Var}(u\mid D_k)@f$ = the pure conditional variance, with
         * @f$u \perp s@f$ by construction of the regression).  Between epochs the
         * border moves, @f$s^+ = s + w@f$, @f$w \sim N(0,Q)@f$ — the SAME
         * @f$Q@f$ the dense border carry applies — so re-expressing the row
         * against @f$s^+@f$ requires the per-column shrinkage
         * @f$M' = (P+Q)^{-1}P\,M_r@f$ and the variance inflation
         * @f$+\,M_r^\top(P^{-1}+Q^{-1})^{-1}M_r@f$.  Card 1 applies NEITHER to
         * layout-PRESENT columns (its departed-column marginalization is the
         * @f$q\to\infty@f$ limit of this rule), so every row over-asserts by
         * @f$\approx M^2 q\,\Delta t@f$ at full weight — re-injected each epoch,
         * this is the carried-DD random walk / flat-σ signature (Card 1c verdict,
         * docs/HANDOFF_carry_timeupdate_card1d.md).
         *
         * When ON, each PRESENT coupled column @f$j@f$ of every coupling row is
         * shrunk by the scalar @f$\kappa_j = P_{\text{prev},jj}/(P_{\text{prev},jj}
         * + q_j\,\Delta t)@f$ (coefficient AND its RHS term) and the row variance
         * gains @f$M_{rj}^2\,\kappa_j\,q_j\,\Delta t@f$, with the per-kind
         * @f$q_j@f$ ladder exactly the dense fold's (clock / code-bias /
         * phase-bias process noise; SH coefficients use the per-degree
         * @f$q/n^2@f$).  A clock-monitor reinit-flagged satellite's clock is
         * treated as @f$q=\infty@f$ (full marginalization, mirroring the fold's
         * decorrelation).  The scalar per-column form is exact when the border
         * covariance is diagonal across a row's coupled columns and consistent
         * (never overconfident) otherwise; it is conservative while border
         * cross-covariances are strong.
         *
         * REQUIRES @ref networkCarryConditionalCoupling (a hard error at load
         * otherwise).  Default false = the raw harvested rows (byte-identical).
         *
         * @see networkCarryConditionalCoupling, networkSharedCarryFullCov
         */
        bool networkCarryCouplingTimeUpdate() const { return network_carry_coupling_time_update_; }

        /**
         * @brief Fully marginalize the SH-VTEC coefficient terms of every coupling
         *        row instead of shrink-carrying them
         *        (@c network.carry_coupling_sh_marginalize, Card 1d-SH).
         *
         * The SH border coefficients are carried as a mean-reverting Gauss-Markov
         * prior (random walk toward the previous estimate PLUS an always-on Kaula
         * climatology anchor), but the Card-1d κ time update models a PURE random
         * walk: a coupling row's VtecSh term therefore re-asserts the harvest
         * epoch's field against the climatology pull at compounding weight — a
         * positive feedback that let the field run away once the accumulated row
         * weight crossed the data+climatology anchoring (observed as an
         * exponential a₀₀ collapse ~4 h into the 12 h low-floor runs).  When ON,
         * VtecSh coupling terms take the q=∞ limit (departed treatment: the
         * coefficient is dropped and @f$M^2 P_{\text{prev}}@f$ marginalized into
         * the row variance) — unconditionally consistent, conservative, and it
         * severs the amb↔SH carry feedback while keeping the clock/bias coupling
         * carry live.  REQUIRES @ref networkCarryConditionalCoupling.  Default
         * false (byte-identical).
         *
         * @see networkCarryCouplingTimeUpdate, networkCarryConditionalCoupling
         */
        bool networkCarryCouplingShMarginalize() const { return network_carry_coupling_sh_marginalize_; }

        /**
         * @brief Carry per-station tropospheric ZWD across epochs in the network
         *        producer as a diagonal random-walk prior (@c network.carry_zwd, B9).
         *
         * ZWD has a physical correlation time of hours; re-estimating it fresh each
         * epoch is an implicit white-noise model.  When on, each non-pivot station's ZWD
         * is carried as a diagonal information prior on its own local block (injected via
         * @ref NetworkEstimator::addStationPrior before the Schur reduction, alongside the
         * phase-leveling ambiguity carry), inflated over the inter-epoch gap by
         * @ref networkZwdCarryQ.  Slant iono is left white.  Reuses the phase-leveling
         * warmup / max-gap housekeeping (so it is inert unless @ref networkPhaseLeveling).
         * Default false keeps the per-epoch ABSOLUTE snapshot solve byte-identical.
         */
        bool networkCarryZwd() const { return network_carry_zwd_; }

        /**
         * @brief ZWD carry random-walk process noise (m²/s) inflating the carried ZWD
         *        variance over the inter-epoch gap (@c network.zwd_carry_q, B9).
         *
         * @c var_eff = var + q·Δt_since_last_seen.  ZWD is a slow random walk (τ ~ hours),
         * so a small positive q (default 1e-9 ≈ 0.2 mm/√h) models the between-epoch drift.
         * Must be @c >= 0.  Used only when @ref networkCarryZwd is on.
         */
        double networkZwdCarryQ() const { return network_zwd_carry_q_; }

        /**
         * @brief Run the M2 LS-VCE carry-row variance-component MONITOR
         *        (@c network.carry_vce).
         *
         * When on, the network producer accumulates the post-fit innovations of
         * the carry pseudo-observations (carried value − freshly-solved value)
         * into a sequential LS-VCE estimator and logs, per carry family per
         * constellation, a measured σ̂ decomposed into a Floor component (m²,
         * cofactor 1 — the honest replacement for the hand @c var_floor) and a
         * Rate component (m²/s, cofactor Δt — the honest replacement for the
         * hand @c q_amb).  It is a CALIBRATION / honesty instrument: it never
         * feeds back into the solve (that is the deferred @ref networkCarryVceApply
         * v2 path), so the solution is byte-identical to the monitor-off run.
         * Foundation: Teunissen & Amiri-Simkooei 2008 (LS-VCE), residual form.
         * Default false = no accumulator, no logging (byte-identical).
         */
        bool networkCarryVce() const { return network_carry_vce_; }

        /**
         * @brief M2 v2 APPLY path — feed the estimated σ̂ back into the carry
         *        stochastic model (@c network.carry_vce_apply).
         *
         * NOT IMPLEMENTED in this build: v1 is monitor-only.  Requires
         * @ref networkCarryVce; setting it true is a hard load error (rather than
         * a silent no-op).  Reserved so configs and the loader validate the
         * dependency now.
         */
        bool networkCarryVceApply() const { return network_carry_vce_apply_; }

        /**
         * @brief Carry families the M2 monitor accumulates
         *        (@c network.carry_vce_components, comma list of @c pl/@c pb/@c cb).
         *
         * @c pl = phase-leveling ambiguity carry (the primary floor, default);
         * @c pb / @c cb = the D2 shared satellite phase-/code-bias carries.  Each
         * requested family is estimated per constellation.  Only used when
         * @ref networkCarryVce.
         */
        const std::string& networkCarryVceComponents() const { return network_carry_vce_components_; }

        /**
         * @brief Exponential-forgetting half-life (emitted epochs) of the M2
         *        VCE accumulator (@c network.carry_vce_window_epochs).
         *
         * The running σ̂ tracks a trailing window of this many emitted epochs;
         * @c <=0 accumulates over the whole run.  Clamped to @c >=1 at load.
         * Only used when @ref networkCarryVce.
         */
        int networkCarryVceWindowEpochs() const { return network_carry_vce_window_epochs_; }

        /**
         * @brief Per-station T0 cycle-slip REPAIR for the carried ambiguities
         *        (phase-leveling Increment 2; @c network.cycle_slip.turbo_edit).
         *
         * When on (and @ref networkPhaseLeveling is on), each non-pivot station runs
         * a persistent MW + GF TurboEdit detector pair every epoch; a confident
         * per-(sat,signal) integer slip @c dN is applied as a continuous,
         * covariance-preserving shift to the carried ambiguity
         * (@c value_abs += dN·λ), so a cycle slip no longer corrupts the carry.
         * REFLOAT/DROP of unrepairable slips and pivot-slip handover are later
         * increments.  Default false keeps Increment 1 (carry-only) byte-identical.
         */
        bool networkCycleSlipTurboEdit() const { return network_cycle_slip_turbo_edit_; }

        /**
         * @brief Integer-confidence gate for a network T0 slip repair
         *        (@c network.cycle_slip.turbo_edit_frac_max; phase-leveling Inc 2).
         *
         * A repair is APPLIED (continuous @c value_abs += dN·λ) only when its rounding
         * residual @c frac (0 = exact integer, 0.5 = worst) is ≤ this threshold AND
         * @c |dN| ≤ @ref networkCycleSlipTurboEditDnMax.  The network runs the MW
         * detector WITHOUT external @c .BIA (it estimates its own satellite biases),
         * so the wide-lane is float-based and TurboEdit's own ~0.3 gate is too loose.
         * A repair that FAILS either bound is LEFT untouched (carry kept): for the
         * BIA-less network those low-confidence emitted repairs are predominantly
         * float-MW FALSE positives, so refloating/dropping them measurably degrades
         * the products.  The rare real-but-rejected slip's lock-in is bounded by
         * @ref networkPhaseLevelingVarFloorM2 / the max-gap drop and is properly
         * resolved by Increment 3's cross-station RAIM consensus.  Default 0.15.
         */
        double networkCycleSlipTurboEditFracMax() const { return network_cycle_slip_turbo_edit_frac_max_; }

        /**
         * @brief Magnitude bound (cycles) on a trusted network T0 slip repair
         *        (@c network.cycle_slip.turbo_edit_dn_max; phase-leveling Inc 2).
         *
         * @c frac gates rounding cleanliness, not magnitude: a BIA-less float can
         * round cleanly to a large spurious integer (TurboEdit's own cap is 1000
         * cycles ≈ 190 m).  A repair with @c |dN| above this bound is treated as
         * untrustworthy and LEFT untouched (carry kept) rather than shifted — only
         * small, confident integers move the carry.  Default 50 cycles — raise it
         * once external BIA makes the wide-lane integer trustworthy.
         */
        int networkCycleSlipTurboEditDnMax() const { return network_cycle_slip_turbo_edit_dn_max_; }

        /**
         * @brief Whether the MW detector runs as a pure DETECTOR in the network
         *        (@c network.cycle_slip.mw_detect).
         *
         * A carried carrier-phase ambiguity is a zero-process-noise random
         * constant: an un-screened cycle slip makes the post-slip phase
         * self-consistent with the SLIPPED integer, so the stiff carry freezes
         * the wrong value into the shared clock (the G29-class artifact).  This
         * complements @ref networkCycleSlipIfPhaseTd (the ionosphere-free
         * primary) with two more iono-safe signals: a CONFIDENT wide-lane
         * integer jump from @ref MwSlipDetector::detectLargeMwResets, and the
         * receiver's own LLI loss-of-lock flag.  Either REFLOATS the affected
         * ambiguity (marks it pending-reseed so PASS B re-estimates it from this
         * epoch's data) — it never applies an integer, so unlike the TurboEdit
         * MW+GF REPAIR path it is valid at the 30 s product rate (the GF
         * ionospheric drift that corrupts a 30 s integer repair is never used).
         * Non-pivot stations only; the refloat is meaningful only with
         * @ref networkPhaseLeveling on (cold, it is diagnostic).  Default false
         * keeps the solve byte-identical.
         */
        bool networkCycleSlipMwDetect() const { return network_cycle_slip_mw_detect_; }

        /**
         * @brief Whether the network IF-phase triple-difference cycle-slip DETECTOR is on
         *        (@c network.cycle_slip.if_phase_td).
         *
         * Between-satellite, between-epoch ionosphere-free phase OMC (the staged
         * @ref IfPhaseSlipDetector, revived for the network where the fixed
         * near-truth station coordinates remove its only blocker — the
         * pre-convergence position-uncertainty gate).  Detection-only: a flagged
         * IF pair refloats BOTH of its band ambiguities (marks them pending-reseed,
         * so the carry re-estimates them from this epoch's data) — no MW/GF
         * confirmation and no integer repair (those are later increments).  The
         * refloat ACTION is meaningful only with @ref networkPhaseLeveling on; with
         * the carry off the detector runs as a pure diagnostic.  Non-pivot stations
         * only.  Default false keeps the snapshot/carry solve byte-identical.
         */
        bool networkCycleSlipIfPhaseTd() const { return network_cycle_slip_if_phase_td_; }

        /**
         * @brief k for the network IF-phase MAD gate (@c network.cycle_slip.if_phase_td_k_mad).
         *
         * A flag fires when @f$|\Delta\Delta\phi_{IF} - \mathrm{med}| > \max(k\cdot
         * 1.4826\cdot\mathrm{MAD},\ \mathrm{floor})@f$, the MAD pooled network-wide per
         * signal pair.  Deliberately loose (8–12) — a per-epoch post-estimation phase
         * validation is the intended sharper second pass.  Default 8.
         */
        double networkCycleSlipIfPhaseTdKMad() const { return network_cycle_slip_if_phase_td_k_mad_; }

        /**
         * @brief Detection floor as a fraction of the (1,1) IF jump
         *        (@c network.cycle_slip.if_phase_td_floor_frac).
         *
         * The smallest integer slip in the ionosphere-free phase is (ΔN1,ΔN2)=(1,1) =
         * @f$c/(f_1+f_2)@f$ ≈ 10.7 cm (GPS L1/L2).  The MAD gate is clamped to never
         * fall below @c floor_frac times this jump, so on a clean pair real slips are
         * still caught while sub-slip noise is not.  Default 1.0 (the (1,1) jump
         * itself — flags jumps at/above the smallest integer slip; a clean network's
         * pooled MAD is ~mm so this floor binds and the k·MAD term is dormant).  Drop
         * toward ~0.7 to catch a bare (1,1) reliably at the cost of more flags.
         */
        double networkCycleSlipIfPhaseTdFloorFrac() const { return network_cycle_slip_if_phase_td_floor_frac_; }

        /**
         * @brief Minimum pooled samples for a robust per-pair network MAD
         *        (@c network.cycle_slip.if_phase_td_min_pool).
         *
         * A 50-station network yields hundreds of between-epoch IF triple-differences
         * per signal pair per epoch, so the per-epoch cross-satellite MAD is robust and
         * replaces between-epoch temporal variability.  Advisory: pairs under this count
         * are logged (their MAD is less reliable).  Default 30.
         */
        int networkCycleSlipIfPhaseTdMinPool() const { return network_cycle_slip_if_phase_td_min_pool_; }

        /**
         * @brief EWMA smoothing factor for the per-(station, signal-pair) IF-phase noise
         *        scale (@c network.cycle_slip.if_phase_td_ewma_alpha).
         *
         * Each (station, signal-pair) tracks an EWMA of its own elevation-normalized
         * robust scale (1.4826·median|z|, z = (ΔΔφ_IF − network-median)·sin el).  This is
         * the recency weight per epoch: scale ← (1−α)·scale + α·scale_epoch.  Smaller =
         * longer memory (steadier), larger = faster tracking of a changing multipath
         * environment.  Default 0.15 (~13-epoch memory).
         */
        double networkCycleSlipIfPhaseTdEwmaAlpha() const { return network_cycle_slip_if_phase_td_ewma_alpha_; }

        /**
         * @brief Shrinkage strength blending the network pool into the per-station IF-phase
         *        scale (@c network.cycle_slip.if_phase_td_shrink_kappa).
         *
         * The effective scale is a count-weighted blend of the station-pair's own EWMA and
         * the network-pooled per-pair scale: @f$w = \mathrm{count}/(\mathrm{count}+\kappa)@f$,
         * @f$s_{eff} = w\cdot\mathrm{EWMA} + (1-w)\cdot\mathrm{pool}@f$.  Cold (count 0) →
         * pure pool prior (instant, huge sample size); warm (count ≫ κ) → the station's own
         * data-driven scale.  κ is the clean-sample count at which the two are weighted
         * equally.  Default 20 (a dense pair — ~10 sats/epoch — becomes station-dominated
         * within a few epochs; a sparse pair leans on the pool longer, which is correct).
         */
        double networkCycleSlipIfPhaseTdShrinkKappa() const { return network_cycle_slip_if_phase_td_shrink_kappa_; }

        /**
         * @brief Whether the post-fit network RAIM backstop is on
         *        (@c network.raim; phase-leveling Increment 3, tier T2).
         *
         * The safety net for the zero-process-noise carry: the pre-processing slip
         * detectors (MW/GF TurboEdit, IF-phase triple-difference) are per-station and
         * threshold/pool-limited, so a real slip on a satellite that is setting (few
         * stations, under the pooled MAD count) passes undetected — and with zero
         * process noise the stale carry then freezes into the products indefinitely.
         *
         * This runs AFTER the joint solve and exploits the network redundancy the
         * per-station detectors cannot see.  A carried ambiguity gone bad injects its
         * error into the SHARED satellite clock/phase-bias; the guilty station hides
         * it there, but every OTHER station tracking that satellite is pinned to its
         * own (correct) carry and so CANNOT absorb the shared shift — its post-fit
         * phase residual spikes.  A cross-station consensus on those standardized
         * residuals (Baarda data-snooping, aggregated per satellite) therefore
         * detects the contamination the guilty station masks, and refloats the
         * offending carries (marks them pending-reseed, so the next epoch re-estimates
         * them from data — the same one-shot lifecycle the IF-phase detector uses).
         * Meaningful only with @ref networkPhaseLeveling on.  Default false keeps the
         * carry solve byte-identical.
         */
        bool networkRaim() const { return network_raim_; }

        /**
         * @brief Whether a vanish-return pivot change signals a datum change
         *        (@c network.pivot_vanish_return_signal, A1).
         *
         * The adjacent-epoch pivot check misses a constellation that drops out for
         * one epoch and returns with a DIFFERENT pivot satellite: it reads as a clean
         * first-sighting, so the consumer is never told the shared solution
         * re-referenced.  When on, a persistent last-ever-pivot map catches that case
         * and forces the same discontinuity bump.  Default false = byte-identical
         * (only the adjacent-epoch change signals).
         */
        bool networkPivotVanishReturnSignal() const { return network_pivot_vanish_return_signal_; }

        /**
         * @brief Whether the network emits a per-satellite CLOCK discontinuity
         *        channel and scopes the forced datum bump per constellation
         *        (@c network.emit_clock_disc, A2).
         *
         * When on, the emitter maintains a per-satellite clock discontinuity counter
         * (bumped when that satellite's constellation clock datum re-references) and
         * the forced phase/code/clock bump on a pivot handover is scoped to the
         * affected constellation(s) instead of every constellation.  Default false =
         * the pre-A2 behavior: no clock disc channel and a global bump on any
         * handover (byte-identical).
         *
         * @note Auto-enabled by the loader when @c network.ura_retro_bump is on (R4):
         *       the retro-bump's clock-counter step is a silent no-op without this
         *       channel, so arming retro arms the channel — and with it the
         *       per-constellation handover-bump scoping above.
         */
        bool networkEmitClockDisc() const { return network_emit_clock_disc_; }

        /**
         * @brief Whether the emitter's CODE-bias discontinuity counter also bumps
         *        on a fresh-baseline VALUE jump beyond the phase disc threshold
         *        (@c network.code_bias_disc_jump_test).
         *
         * The H1 structural triggers (per-(const,signal) sum-to-zero set change,
         * per-sat IF-pin band change, forced datum bump) detect GAUGE
         * re-references only; the Jul-2026 wire audit measured large unsignalled
         * code-bias VALUE movement beyond them (weak-separation wander — 7.6k
         * jumps > 0.05 m vs 2.8k signalled over 60 epochs at net305).  This
         * layers the phase channel's magnitude jump test onto code so whatever
         * still moves is honestly signalled and the consumer re-inflates its
         * receiver code-bias states instead of dragging.  Travels in the
         * S-contract (@c cbjump=) so file replay reproduces the counters.
         * Default false = structural triggers only (byte-identical).
         */
        bool networkCodeBiasDiscJumpTest() const { return network_code_bias_disc_jump_test_; }

        /**
         * @brief Whether the emitter bumps a constellation's CLOCK discontinuity
         *        counters when its clock-satellite SET changes between emits
         *        (@c network.clock_disc_on_set_change).
         *
         * Under the sum-to-zero clock datum a membership change IS a datum
         * re-reference — Σδtˢ=0 re-imposed on the new set redistributes the
         * departing/arriving common-mode onto every survivor (measured up to
         * 45 cm/epoch at a 3-station roster) — but the pivot-handover channel
         * never signals it (there is no pivot).  This trigger derives the set
         * change from the emitted epoch itself (replay-reproducible; travels in
         * the S-contract as @c clkset=) and steps the affected constellation's
         * clock counters so the consumer can react scoped (e.g. re-inflate a
         * frozen ISB).  Requires @c network.clock_datum_sum_to_zero (the only
         * datum whose gauge moves with membership) and arms
         * @c network.emit_clock_disc.  Default false (byte-identical).
         */
        bool networkClockDiscOnSetChange() const { return network_clock_disc_on_set_change_; }

        /**
         * @brief Whether the auto-picked clock-pivot satellite is sticky
         *        (@c network.sticky_pivot, RTA-1).
         *
         * The shared clock datum pins ONE satellite's clock per constellation
         * (@ref NetworkSbasis::buildSharedDatum).  The legacy auto-pick is the
         * lexically smallest registered satellite of the constellation, re-derived
         * EVERY epoch — so every time a lexically lower satellite RISES the pivot
         * flips, a constellation-wide datum re-reference the consumer pays for with
         * an ambiguity reset.  When on, the incumbent pivot (the pivot of the last
         * EMITTED product containing that constellation) is kept as long as it is
         * still present in the epoch's registered clock satellites; a new pivot is
         * selected only when the incumbent is absent, and the NEW pivot then
         * becomes the sticky incumbent in turn (no flip-back when the old one
         * returns).  A config-pinned satellite (@c network.clock_datum_satellite)
         * has priority over the incumbent in BOTH modes — pinning declares user
         * intent, so the pinned satellite reclaims the datum on its return (that
         * flip-back is deliberate and still signalled as a handover).  Default
         * false = the legacy per-epoch lexical selection (byte-identical).
         */
        bool networkStickyPivot() const { return network_sticky_pivot_; }

        /**
         * @brief Whether the per-constellation satellite-clock gauge is realized by
         *        a SUM-TO-ZERO over that constellation's satellite clocks instead of
         *        a hard pin on one pivot satellite (@c network.clock_datum_sum_to_zero).
         *
         * Legacy (OFF): the clock gauge for each constellation is fixed by pinning
         * ONE satellite's clock (the "pivot"; @ref NetworkSbasis::buildSharedDatum,
         * @ref networkStickyPivot).  Anchor-based — a pivot that sets or is
         * deselected forces a constellation-wide datum re-reference (handover) the
         * consumer pays for with an ambiguity reset, and an epoch missing every
         * candidate pivot has no datum and is skipped wholesale.
         *
         * When ON, the gauge is instead the constellation-wide SUM-TO-ZERO Σ δtˢ = 0,
         * folded as a tight all-ones constraint row onto the shared normals before
         * the null-space gate — exactly how the satellite code-bias
         * per-(constellation,signal) level is pinned (@c network.estimate_sat_code_bias).
         * Fully anchor-free: there is NO pivot satellite, so no handover seam ever,
         * the clock_pivot map and shared hard-pin datum stay empty, and the datum is
         * defined for any epoch with ≥1 clock satellite per constellation.  The
         * emitted product self-describes the change via the S-basis contract's
         * @c isb_datum field (@ref SbasisDeclaration::IsbDatum::SumToZeroPerConstellation),
         * and a per-constellation @c clock_datum_satellite pin is ignored (there is
         * no pivot to place).  The two datums differ only by a per-constellation
         * constant a consumer absorbs into its receiver clock — a pure S-transform of
         * the estimable (between-satellite) clock solution — so the change is
         * consumer-transparent for a float user and phase-bias/AR-frame-neutral.
         * Default false = the legacy per-constellation pivot pin (byte-identical).
         *
         * @see networkStickyPivot, NetworkSbasis, SbasisDeclaration::IsbDatum
         */
        bool networkClockDatumSumToZero() const { return network_clock_datum_sum_to_zero_; }

        /**
         * @brief Whether the satellite-clock gauge is realized by PER-SATELLITE
         *        broadcast anchors (@c network.clock_datum_broadcast_anchored,
         *        @c apriori_source=brdc_rts only).
         *
         * The real-time datum-stability contract: under @c brdc_rts the shared
         * clock column is Δδtˢ relative to each satellite's OWN broadcast clock,
         * so a soft per-satellite anchor toward 0 (σ =
         * @ref resolveNetworkAprioriClockAnchorSigmaM, auto 3.0 m) folded BEFORE
         * the null-space gate both regularizes cold start AND realizes the
         * gauge — no pivot pin, no sum-to-zero row.  Unlike the sum-to-zero
         * datum (whose reference is the raw per-epoch satellite set, measured to
         * redistribute ∓ΔC0/N onto the survivors as satellites rise/set), each
         * anchor is independent, so the emitted absolute clock frame does NOT
         * wander with membership — it stays bound to the broadcast reference
         * the consumer already reconstructs against.  The product self-describes
         * via the S-contract
         * @ref SbasisDeclaration::IsbDatum::BroadcastAnchoredPerSatellite.
         * Requires @c apriori_source=brdc_rts and is mutually exclusive with
         * @c network.clock_datum_sum_to_zero (both fail-loud at load).  Default
         * false = unchanged (byte-identical).
         *
         * @see networkClockDatumSumToZero, resolveNetworkAprioriClockAnchorSigmaM,
         *      NetworkSbasis, SbasisDeclaration::IsbDatum
         */
        bool networkClockDatumBroadcastAnchored() const { return network_clock_datum_broadcast_anchored_; }

        /**
         * @brief Whether the per-satellite clock↔code-bias split (the
         *        ionosphere-free "signal basis" the emitted satellite clock is
         *        anchored to) is HELD FIXED across epochs
         *        (@c network.clock_signal_basis_hold, W6 only).
         *
         * @par The datum this fixes (distinct from the per-constellation gauge)
         * Two independent clock datums exist in the uncombined network:
         *  - the per-CONSTELLATION common-mode (1 dof/const) — pivot / sum-to-zero /
         *    broadcast-anchored; a receiver-clock-estimating consumer absorbs it, so
         *    it is NOT the consumer-vertical driver; and
         *  - the per-SATELLITE clock↔code-bias split (1 dof/sat) — WHICH
         *    ionosphere-free code-signal pair the satellite clock @f$\delta t^s@f$ is
         *    referenced to.  Realized by the W6 IF pin
         *    @f$a_1 d^s_{\sigma_1}+a_2 d^s_{\sigma_2}=0@f$ (ApplicationNetwork).
         *    The BETWEEN-satellite difference is datum-invariant at a single epoch,
         *    but a per-epoch CHANGE of the reference pair @f$(\sigma_1,\sigma_2)@f$
         *    injects a PER-SATELLITE (differential, de-median-surviving) step into
         *    the emitted clock time series that a receiver clock CANNOT absorb.
         *
         * @par Why it moves by default
         * The reference pair is re-selected every epoch from that epoch's
         * @c registered_code_keys (the two lowest distinct-frequency ≥2-station
         * signals).  When a sparse auxiliary channel enters/leaves ≥2-station
         * coverage — the band-1 canonical flips C1C↔C1W, or a band drops — the
         * pair changes and the clock re-anchors by the satellite's inter-channel
         * code bias.  Over a run this is a continual, unsignalled, per-satellite
         * clock wander (the "defect 2" differential-clock instability the consumer
         * absorbs into per-sat iono → vertical).
         *
         * @par The fix (a fixed, time-consistent convention — the IGS clock model)
         * ON: each satellite LATCHES its reference pair on first registration and
         * HOLDS it while both signals stay registered, so the emitted
         * @f$(\delta t^s, d^s_\sigma)@f$ pair is time-consistent — the clock is
         * anchored to ONE signal basis for the whole run (mirroring the fixed
         * IF(C1W,C2W)/IF(C1C,C5Q) convention IGS clock products publish).  The pair
         * re-latches only when a held reference signal genuinely leaves the
         * ≥2-station set (rare for the dense canonical codes), a much smaller and
         * far less frequent datum move than the per-epoch re-selection it replaces.
         * Composes with any per-constellation gauge (pin / sum-to-zero /
         * broadcast-anchored) — orthogonal directions.  Requires
         * @c network.estimate_sat_code_bias (the IF pin only exists in W6);
         * INERT + warned otherwise.  Default false = per-epoch re-selection
         * (byte-identical).
         *
         * @see estimateNetworkSatCodeBias, networkClockDatumSumToZero
         */
        bool networkClockSignalBasisHold() const { return network_clock_signal_basis_hold_; }

        /**
         * @brief Impose the W6 code-bias datum (per-satellite IF pin + per-
         *        (constellation,signal) sum-to-zero) as a TIME datum — pin the
         *        CHANGE of the previous epoch's solved values, not the level to zero
         *        (@c network.code_datum_time_linked, Card 2).
         *
         * @par The defect it cures
         * The legacy datum re-imposes, every epoch, one IF-pin row per satellite
         * (@f$a_1 d^s_{\sigma_1}+a_2 d^s_{\sigma_2}=0@f$, fixing the clock↔code
         * split) and one sum-to-zero row per (constellation,signal)
         * (@f$\sum_s d^s_k=0@f$, fixing the channel level) at a stiff weight over
         * the CURRENT satellite set, with target ZERO.  The SAME shared columns
         * carry the near-zero-@f$q@f$ code-bias prior.  On any rise/set the sum-to-
         * zero target shifts by (departed bias)/@f$m'@f$, the stiff carry resists,
         * and the level RELAXES over many epochs toward the new target — a
         * hysteretic, unsignalled common-mode wander in the emitted OSBs that leaks
         * into the emitted clock through the IF pin (the documented code-clock
         * wander).  The disc counter fires once AT the set change while the value
         * drifts AFTERWARD — the worst ordering for a consumer holding ambiguities.
         *
         * @par The fix (pin the change, not the level)
         * ON: the SAME number of datum rows is imposed (rank unchanged — the null-
         * space gate must still pass), but their TARGETS reference the previous
         * epoch's solved values.  The sum-to-zero row spans the present satellites
         * that ALSO have a previous-epoch solved value ("veterans") with RHS
         * @f$\sum_{\text{vet}}\hat b_{s,k-1}@f$ — pinning the CHANGE of the veteran
         * mean to zero; a channel with NO veterans seeds at @f$\sum=0@f$ (a REAL
         * re-reference: its satellites' disc counters are bumped that epoch).  The
         * IF-pin RHS becomes @f$a_1\hat b_{\sigma_1,k-1}+a_2\hat b_{\sigma_2,k-1}@f$
         * (seed 0 at first appearance; disc bump on any later re-seed).  The gauge
         * still removes exactly the same @f$1+m@f$ DOF per channel but as a TIME
         * datum (continuity) — the Odijk-consistent cure for CC-S set-dependence —
         * so the target agrees with the carry by construction and no relaxation
         * dynamics exist.
         *
         * @par Bookkeeping
         * One map per (constellation,signal) → per-satellite previous solved value,
         * refreshed at the D2 harvest from the solved shared vector (kept epochs
         * only; a departed satellite is dropped, so its return re-seeds IT alone).
         * NOT persisted in the snapshot — a warm resume re-seeds with disc bumps
         * (correct and signalled).  The toggle is in the snapshot fingerprint
         * (estimation-relevant).
         *
         * REQUIRES @ref networkClockSignalBasisHold to be OFF (a hard error at load
         * if both are armed): both mechanisms re-target the SAME IF-pin row, and
         * time-linking subsumes the hold for the pin VALUE.  Meaningful only under
         * @ref estimateNetworkSatCodeBias (the datum it re-times only exists in W6);
         * INERT + warned otherwise.  Default false = the legacy zero-target datum
         * (byte-identical).
         *
         * @see estimateNetworkSatCodeBias, networkClockSignalBasisHold,
         *      NetworkSbasis::codeS2zTimeLink, NetworkSbasis::selectIfPinPair
         */
        bool networkCodeDatumTimeLinked() const { return network_code_datum_time_linked_; }

        /**
         * @brief Whether the satellite phase-bias gauge is realized by a
         *        per-(satellite,signal) PRIORITIZED ANCHOR RECEIVER instead of the
         *        single clock-pivot station (@c network.phase_datum_prioritized).
         *
         * Legacy (OFF): the phase-bias↔ambiguity gauge for every emitted
         * (satellite,signal) is pinned on the ONE clock-datum station's ambiguity
         * (@c is_pivot, @ref NetworkSbasis header, Odijk Eq. 4), and the emittable
         * phase-bias set is capped at exactly that station's tracked signals.  Two
         * consequences the consumer pays for: (a) when the pivot loses a
         * low-elevation satellite that satellite's phase bias vanishes from the
         * product and its gauge re-references on re-acquisition — a gauge JUMP; and
         * (b) an epoch in which the pivot is absent has no datum at all and is
         * skipped wholesale (a single point of failure).
         *
         * When ON, the gauge is still a MINIMUM constraint — exactly one ambiguity
         * pin per (satellite,signal) phase-bias freedom — but each such pin is
         * placed at the HIGHEST-PRIORITY receiver that currently tracks that
         * satellite with a usable ambiguity, decoupled from the clock pivot.  Per
         * constellation the candidate receivers are ranked by descending coverage
         * (satellites tracked above @ref networkPhaseDatumPrefElevDeg), so the
         * majority of a constellation stays anchored to one high-coverage receiver
         * (frame-consistent) and only a satellite that receiver cannot see falls
         * back to the next — each such per-(satellite,signal) anchor HANDOVER is
         * signalled to the consumer as a discontinuity (@c markSatDiscontinuity), so
         * the emitted bias stays continuous-or-signalled rather than jumping
         * silently.  The clock datum (one satellite-clock pin per constellation) is
         * independent and unchanged.  Default false = the legacy single-pivot pins
         * (byte-identical product + traces).
         *
         * @see networkPhaseDatumPrefElevDeg, networkPhaseDatumSticky
         */
        bool networkPhaseDatumPrioritized() const { return network_phase_datum_prioritized_; }

        /**
         * @brief Whether the satellite phase-bias↔ambiguity gauge is realized by an
         *        anchor-free SUM-TO-ZERO over the tracking receivers
         *        (@c network.phase_datum_ensemble) — Fix A.
         *
         * The estimable satellite phase bias @f$\tilde\delta^s_\sigma@f$ is
         * co-linear with the tracking receivers' ambiguities @f$\{z^s_{r,\sigma}\}_r@f$
         * (one gauge dof per (satellite, signal)).  The prioritized-anchor datum
         * (@ref networkPhaseDatumPrioritized) fixes it by pinning ONE elected
         * receiver's ambiguity, which (a) re-references — a consumer-visible step —
         * whenever the anchor hands over (Odijk's moving-S-basis violation → phase
         * never accumulates), and (b) references DIFFERENT satellites to DIFFERENT
         * receivers, so the user's between-satellite fractional-bias term does not
         * cancel and its double-differenced ambiguity is non-integer (0 fixes).
         * ENSEMBLE instead fixes the gauge by @f$\sum_r z^s_{r,\sigma}=0@f$ over the
         * receivers — no pivot, no handover, no single-anchor SPOF, and EVERY
         * satellite referenced to the same receiver ensemble (the between-satellite
         * fractional term cancels consistently, keeping the user DD integer).
         * Realized by the kernel's Schur-projected rank-1 fold
         * (@ref NetworkEstimator::setPhaseSumZeroGauges); reuses the prioritized
         * SPOF-cure machinery (no pivot-first, every station carries its arcs) but
         * elects no anchor.  SUPERSEDES @ref networkPhaseDatumPrioritized when both
         * are set.  Default false = the prioritized-anchor / legacy pin datum
         * (byte-identical).
         *
         * @see networkPhaseDatumPrioritized, networkPhaseDatumMinStations
         */
        bool networkPhaseDatumEnsemble() const { return network_phase_datum_ensemble_; }

        /**
         * @brief Seed-once + time-lock hold sigma (m) for the ensemble phase-bias
         *        datum (@c network.phase_datum_hold_sigma_m).  0 = re-realize every
         *        epoch (churny; the un-held ensemble).
         *
         * The anchor-free @f$\sum_r z^s_{r,\sigma}=0@f$ datum references the emitted
         * bias to @f$\mathrm{mean}_r(\hat z^s_{r,\sigma})@f$ — a per-epoch-re-realized
         * functional of the FLOAT ambiguities, whose absolute LEVEL is code/iono-
         * precision-limited, so the emitted bias churns ~metre/epoch (a dynamic datum
         * on a static product; the between-satellite structure is stable but the level
         * is not).  This makes it usable: SEED the @f$\sum z=0@f$ gauge ONCE when a
         * (sat,signal) first appears, then HOLD the shared bias with a stiff temporal
         * prior of this sigma (a clean fixed variance, NOT the fold-contaminated
         * covariance column) so the level stops re-realizing.  A re-seed occurs when a
         * (sat,signal) reappears with no carried bias (arc break / vanish-return).
         * Requires @ref networkPhaseDatumEnsemble; inert (byte-identical) otherwise or
         * when 0.  Typical: 0.01 m.
         *
         * @see networkPhaseDatumEnsemble
         */
        double networkPhaseDatumHoldSigmaM() const { return network_phase_datum_hold_sigma_m_; }

        /**
         * @brief Whether the satellite phase-bias↔ambiguity gauge is realized by
         *        the Eq-34 / de Jonge SINGLE-RECEIVER-CHAIN datum
         *        (@c network.phase_datum_eq34) — Card 3.
         *
         * The master toggle for the only datum that keeps the emitted
         * between-satellite phase-bias reference integer-consistent across ALL
         * satellites while staying full-rank at global scale.  It gives the
         * receiver fractional phase bias its OWN estimable state (@ref
         * StateKeys::makeReceiverPhaseBiasKey) so it can no longer lump into the
         * emitted satellite phase bias, then fixes the enlarged gauge with one
         * receiver datum pin + a spanning-tree of ambiguity pins over the
         * bipartite (receiver × satellite) observation graph (per
         * constellation×signal connected component).  Then every satellite pair's
         * reference difference is an integer combination — including satellites
         * the datum receiver never sees (each chain hop contributes only an
         * integer, because the hop receiver's fractional bias is a separate
         * estimable state, not part of the pin).  Reuses the prioritized SPOF-cure
         * machinery (no pivot-first, every station carries its arcs) but replaces
         * the anchor election with a chain election; SUPERSEDES @ref
         * networkPhaseDatumPrioritized and is MUTUALLY EXCLUSIVE with @ref
         * networkPhaseDatumEnsemble (two different realizations of the same gauge,
         * a load-time throw).  Default false = the prioritized-anchor / legacy pin
         * datum (byte-identical: no RPB state is ever allocated, the chain election
         * never runs).
         *
         * @see networkPhaseDatumEq34Receiver, networkPhaseDatumEq34PinRound,
         *      networkPhaseDatumPrioritized, networkPhaseDatumEnsemble
         */
        bool networkPhaseDatumEq34() const { return network_phase_datum_eq34_; }

        /**
         * @brief Preferred datum receiver for the eq34 chain datum's component 0
         *        (@c network.phase_datum_eq34_receiver).  Empty = coverage auto-pick.
         *
         * When set and present in a connected component, this station is elected
         * the component's datum receiver r1 (its RPB is pinned).  Empty (default)
         * picks the highest-coverage candidate.  Sticky-incumbent selection still
         * wins over this preference while the incumbent remains present (fewest
         * handovers).  INERT unless @ref networkPhaseDatumEq34.
         */
        const std::string& networkPhaseDatumEq34Receiver() const {
            return network_phase_datum_eq34_receiver_;
        }

        /**
         * @brief Whether an eq34 re-pin of an already-lit (satellite,signal) edge
         *        targets the ROUNDED carried float (@c network.phase_datum_eq34_pin_round).
         *
         * Any integer target is a valid gauge; on a re-attach / re-pin,
         * true (default) selects the integer λ·round(x̂_prev/λ) of the new edge
         * station's carried float so the emitted-bias frame step is ≈ 0 cycles
         * (typically no wire discontinuity once the floats are honest); false
         * always pins to 0 (an A/B lever — a legal but usually non-zero-step
         * frame).  INERT unless @ref networkPhaseDatumEq34.
         *
         * @see networkPhaseDatumEq34
         */
        bool networkPhaseDatumEq34PinRound() const { return network_phase_datum_eq34_pin_round_; }

        /**
         * @brief Card 3: loose zero-mean prior σ (metres) applied per epoch to every
         *        receiver phase-bias (RPB) column, in its station's own block
         *        (@c network.phase_datum_eq34_rpb_prior_sigma_m).
         *
         * The chain pins fix the per-(constellation,signal) GAUGE exactly, but the
         * estimable RPB↔ambiguity↔ionosphere separation INSIDE a station block can
         * be nearly rank-deficient (few tracked satellites / one signal): without an
         * information floor the W5 observability gate withholds epochs, or — under
         * @ref networkStationPdPinAware — the near-singular block stays in and
         * degrades the shared solve (Jul-10 field A/B: datum-invariant DD step
         * 0.085 → 36–118 cyc/epoch).  This is a soft floor (weight 1/σ²) on an
         * already-estimable quantity, NOT a second datum: the gauge is fixed by the
         * pins, and the pull toward 0 is far below one phase row's weight at the
         * default.  0 disables.  Default 1.0 m — loose against dm-level physical
         * receiver phase biases (GLONASS local-anchor precedent).  INERT unless
         * @ref networkPhaseDatumEq34.
         *
         * @see networkPhaseDatumEq34, networkStationPdPinAware
         */
        double networkPhaseDatumEq34RpbPriorSigmaM() const { return network_phase_datum_eq34_rpb_prior_sigma_m_; }

        /**
         * @brief Card 3: process noise q (m²/s) of the LOCAL receiver phase-bias
         *        time-linking carry (@c network.phase_datum_eq34_rpb_q_m2s).
         *
         * v1 re-estimated δ_{r,σ} fresh each epoch; because its observability
         * inside a station block is weak, the per-epoch estimate bounces, and the
         * bounce enters every individual ambiguity float anti-correlated (DDs
         * cancel it — the per-key phase-leveling carry does not).  This is the
         * spec's D6-v2 trigger condition, met in the Jul-10 field A/B (datum-
         * invariant DD step 0.085 → 10–100 cyc/ep).  The carry folds a per-epoch
         * prior toward the previous δ̂_{r,σ} with variance @c var + q·gap — the
         * MINIMAL local realization of D6 v2 (no shared-border promotion, no
         * coupling-row footprint).  Default 3e-6 m²/s admits ~0.1 m/h physical
         * receiver-bias drift while suppressing dm-level per-epoch bounce.
         * 0 disables (v1 behavior).  INERT unless @ref networkPhaseDatumEq34.
         *
         * @see networkPhaseDatumEq34, networkPhaseDatumEq34RpbPriorSigmaM
         */
        double networkPhaseDatumEq34RpbQ() const { return network_phase_datum_eq34_rpb_q_m2s_; }

        /**
         * @brief Card 3 EXPERIMENTAL: on an eq34 pin displacement, S-TRANSFORM the
         *        carried values into the new frame instead of the erase/skip
         *        hygiene (@c network.phase_datum_eq34_carry_shift).
         *
         * Theoretically exact in the chain algebra (Δb_s / Δδ_r propagate down the
         * tree with alternating sign; local, RPB, and shared-PB carries shift by
         * their exact frame moves, variances untouched).  Field result (Jul-11
         * overnight A/B): the realized redistribution in the FULL model differs
         * from the ideal chain algebra — the shift injected error (DD step 86 vs
         * 11–15 cyc/ep for erase-based hygiene), root cause open (suspect: the
         * weakly-held δ_{r,σ} split absorbing part of the pin move that the walk
         * attributes to b_s).  Default false = erase-based hygiene.  INERT unless
         * @ref networkPhaseDatumEq34.
         */
        bool networkPhaseDatumEq34CarryShift() const { return network_phase_datum_eq34_carry_shift_; }

        /**
         * @brief Card 3 (I4): longevity-aware BFS growth of the eq34 spanning tree
         *        (@c network.phase_datum_eq34_longevity_election).
         *
         * Spec D2 step 3 asks the spanning tree to be grown from r1 "preferring
         * high-coverage stations and high-elevation arcs" — the preference was
         * dropped in the sticky two-queue BFS build, so a NEW tree edge is chosen
         * by lexical sat/station order, blind to how long the arc will survive.
         * A tree edge that dies (its arc sets) forces a b_s re-reference + carry
         * erase; picking the LONGEST-LIVED available edge cuts that churn at the
         * source (the de Jonge-spirit interim mitigation, complementary to the
         * extend-only membership item).  When ON, the NEW-edge frontier of the BFS
         * (the incumbent-edge seed is untouched) is ordered by arc longevity: a
         * RISING arc (elevation increased vs last epoch — won't set for a long
         * time) is lifted above every setting arc, and within a class higher
         * elevation wins; receiver choice prefers high per-constellation coverage
         * first (the more robust tracker).  Ties break deterministically
         * (sat name / station index) exactly as the legacy order.  Pure edge-
         * selection change: the gauge dimension, pins, and every emitted value are
         * unchanged for any tree it happens to reproduce.  Default false =
         * legacy lexical growth (the B9 resting baseline, for a clean reref-counter
         * A/B).  INERT unless @ref networkPhaseDatumEq34.
         *
         * @see networkPhaseDatumEq34
         */
        bool networkPhaseDatumEq34LongevityElection() const {
            return network_phase_datum_eq34_longevity_election_;
        }

        /**
         * @brief Card 3 (I2): extend-only sticky-tree membership across momentary
         *        row gaps (@c network.phase_datum_eq34_longevity_election's sibling,
         *        @c network.phase_datum_eq34_extend_only).
         *
         * de Jonge §3.5: the S-basis is EXTEND-ONLY over time — a basis edge is
         * removed only on a true arc death, never on a per-epoch usability flap.
         * In this running filter the sound realization is to RETAIN a tree edge's
         * sticky identity (its sat-side / receiver-side parent, component root, and
         * pinned integer) while its phase-leveling memory (@c local_carry) is still
         * alive, so the epoch its arc RESUMES it is a recognized incumbent that
         * re-pins to the SAME integer (emitted-bias step ≈ 0, DDs continuous)
         * instead of re-leveling from a first-sighting 0 — the silent integer
         * re-reference that otherwise poisons every peer station's surviving carry
         * for that satellite.  @c local_carry liveness is the age stamp: it is
         * gap-pruned at @c network.phase_leveling_max_gap_s, the same horizon that
         * bounds the carries the whole datum rides on, so an edge absent longer than
         * that (or whose carry was erased by a genuine reroute/handover) is NOT
         * retained and correctly re-floats as a first sighting — by which point the
         * peer carries are gone too.  Tree GROWTH is unchanged (the seed admits LIVE
         * arcs only ⇒ a retained-but-absent edge is inert until its arc returns, so
         * the spanning-forest invariant holds); reroute/handover churn is out of
         * scope (Card 4).  Default false = legacy wholesale replace (the B9
         * baseline).  INERT unless @ref networkPhaseDatumEq34.
         *
         * @see networkPhaseDatumEq34, networkPhaseLevelingMaxGapS
         */
        bool networkPhaseDatumEq34ExtendOnly() const {
            return network_phase_datum_eq34_extend_only_;
        }

        /**
         * @brief Coverage-ranking elevation preference (deg) for the prioritized
         *        phase-bias anchor (@c network.phase_datum_pref_elev_deg).
         *
         * A satellite counts toward a receiver's per-constellation coverage rank
         * only when tracked at or above this elevation, so the elected anchor is a
         * receiver seeing that constellation WELL (not merely at the horizon).  It
         * does NOT mask the pin itself — a receiver that is the last remaining
         * tracker of a setting satellite can still anchor it (any usable ambiguity
         * qualifies), the preference only orders the candidates.  INERT unless
         * @ref networkPhaseDatumPrioritized.  Default 15°.
         */
        double networkPhaseDatumPrefElevDeg() const { return network_phase_datum_pref_elev_deg_; }

        /**
         * @brief Whether a per-(satellite,signal) anchor is STICKY
         *        (@c network.phase_datum_sticky).
         *
         * When on, the incumbent anchor for a (satellite,signal) is kept as long as
         * it still tracks that satellite with a usable ambiguity, even if a peer
         * gains marginally higher constellation coverage — a re-election (and its
         * consumer-visible discontinuity) fires only when the incumbent actually
         * loses the satellite.  This minimizes anchor-handover discontinuities, the
         * exact analogue of @ref networkStickyPivot for the phase-bias gauge.  INERT
         * unless @ref networkPhaseDatumPrioritized.  Default true.
         */
        bool networkPhaseDatumSticky() const { return network_phase_datum_sticky_; }

        /**
         * @brief Coverage threshold on the prioritized anchored union: minimum
         *        number of present stations that must track a (satellite,signal)
         *        with a usable ambiguity for its shared phase-bias column to be
         *        registered and emitted (@c network.phase_datum_min_stations).
         *
         * Under @ref networkPhaseDatumPrioritized the shared phase-bias set is the
         * anchored UNION over all present stations, which admits columns tracked by
         * a single receiver.  Those thin columns are the pathology class the pdp
         * validation surfaced: (a) a marginal tracking station's local block barely
         * separates the column from its own ambiguity, ejecting the station as
         * non-PD (the TWTF/VOIM 121-of-121 symptom); (b) a one-station anchor pool
         * re-levels the bias by a full ambiguity lump whenever its only anchor sets
         * (the −72.7 m G29 L2L event); (c) weakly-separated columns leak their
         * anchor lump into the clock↔phase-bias split (median|dev| 0.8→10 m,
         * cancelling for clock+bias users but polluting clock-only comparisons).
         *
         * A gated-out key costs the product nothing but that bias: the tracking
         * stations' PHASE rows stay in the solve and serve the satellite clock
         * through their own float ambiguities — exactly how legacy non-shared
         * signals (and GLONASS network phase) have always contributed.  The
         * allocated per-station phase-bias states of a gated key are pruned so the
         * station column sets stay consistent with the gated union.
         *
         * 1 (default) = no gating — the original prioritized-union behavior,
         * byte-identical.  2–3 recommended once armed.  INERT unless
         * @ref networkPhaseDatumPrioritized.
         *
         * @see networkPhaseDatumPrioritized, networkPhaseDatumSticky
         */
        int networkPhaseDatumMinStations() const { return network_phase_datum_min_stations_; }

        /**
         * @brief Whether the network kernel's per-station local-block positive-
         *        definiteness test uses the pin-aware MEDIAN-|pivot| reference
         *        scale instead of the maximum pivot
         *        (@c network.station_pd_pin_aware).
         *
         * The legacy test drops a station when its smallest local LDLᵀ pivot
         * falls below 1e-12 × the LARGEST pivot.  Under
         * @ref networkPhaseDatumPrioritized every station elected as a
         * phase-bias anchor carries 1e10 gauge-pin rows in its OWN local block,
         * inflating that bar by ~10 orders: a marginal station whose weakest
         * genuine parameter sits at a perfectly usable ~1e-3 information is
         * then ejected EVERY epoch (the TWTF/VOIM class — the identical block
         * passes when the same station holds no anchor pin), permanently
         * thinning the network.  The 2026-07-04 A/B showed this ejection class
         * is UNTOUCHED by the anchored-union coverage gate (identical failing
         * pivots with the gate on/off): the pins, not the thin columns, set the
         * bar.  With this toggle the reference scale is the MEDIAN |pivot| —
         * robust to any minority of pin-scale pivots, still catching a truly
         * singular direction.  Applies to every network mode but only matters
         * where huge in-block pins exist (the prioritized datum; the legacy
         * single pivot station).  Default false = legacy max-pivot test
         * (byte-identical).
         *
         * @see networkPhaseDatumPrioritized, networkPhaseDatumMinStations,
         *      NetworkNormalEquations::setStationPdMedianScale
         */
        bool networkStationPdPinAware() const { return network_station_pd_pin_aware_; }

        /**
         * @brief Whether the producer's INTERNAL response to a clock-pivot handover
         *        is scoped to the constellation(s) whose datum actually changed
         *        (@c network.scoped_handover_response, RTA-2).
         *
         * On a pivot handover the carried cross-epoch state of the CHANGED
         * constellation lives in a retired datum: its D2 shared-carry priors must
         * be skipped, its two-state clock-model oscillators cleared, and its
         * clock-monitor jump baselines suppressed for the epoch.  The legacy
         * response applies that invalidation GLOBALLY — one constellation's
         * handover degrades every constellation's product for the warmup horizon.
         * When on, those internal responses apply ONLY to satellites of the
         * constellation(s) whose pivot changed (exactly the set the A2
         * per-constellation emit bump uses); carried state of unaffected
         * constellations folds normally.  The SH-VTEC carry skip deliberately
         * stays global (constellation-less columns; see the driver comment at the
         * SH prior fold).  The emitter-side datum bump is already scoped by
         * @ref networkEmitClockDisc and is unaffected by this toggle.  Default
         * false = the global internal response (byte-identical).
         */
        bool networkScopedHandoverResponse() const { return network_scoped_handover_response_; }

        /**
         * @brief Producer: stamp a solution-GENERATION id on every emitted epoch
         *        (@c network.emit_generation_id, RTA-3).
         *
         * The existing E-line @c iod_ssr is a per-epoch increment — meaningless as a
         * generation marker: a producer RESTART re-seeds every emitter counter at
         * zero, so a warm consumer that stored counter=0 sees 0→0 ("no change")
         * while the phase-bias datum was completely re-realized → silently wrong
         * fixes.  When on, the producer derives a uint32 generation id from the
         * FIRST EMITTED epoch's GPS time (week·604800 + ⌊tow⌋) — deterministic
         * under file replay (the id is serialized per epoch as an optional @c I
         * record and travels in the S contract as a @c gid= field), different
         * across live restarts (a restarted producer first-emits later).  The
         * consumer treats a CHANGE of a PRESENT id as a global datum break
         * (reset all network-corrected ambiguities + re-inflate receiver
         * code-bias states); an absent id (old product / RTCM stream) never
         * reacts.  Default false ⇒ no @c I record, store generation unset —
         * byte-identical.
         */
        bool networkEmitGenerationId() const { return network_emit_generation_id_; }

        /**
         * @brief Producer: serialize the CURRENT discontinuity-counter VALUES on the
         *        wire (@c network.serialize_disc_counters, RTA-4).
         *
         * The v1/v2 product file carries bump TRIGGERS (E-line @c datum_changed /
         * @c datum_consts, @c R records) but not counter VALUES, so a late-joining /
         * tail reader can only reconstruct the counters by replaying from epoch 1.
         * When on, each record gains an optional trailing field carrying the
         * POST-EMIT counter for this epoch (identical to what @c emitToStore pushed
         * into the store): C-line field 5 = clock disc counter (field 4 = C1 drift,
         * always written — 0 when absent — so the counter position is fixed),
         * B-line field 5 = per-signal code-bias disc counter, P-line field 7 =
         * per-signal phase-bias disc counter.  The reader ADOPTS present values
         * into the reconstructed store records, keeping the replay recompute as a
         * self-check (warn on mismatch).  Old files (fields absent) parse
         * unchanged; default false ⇒ the file is byte-identical.
         */
        bool networkSerializeDiscCounters() const { return network_serialize_disc_counters_; }

        /**
         * @brief Producer: re-broadcast the v2 'S' contract line every N seconds of
         *        PRODUCT time (@c network.contract_rebroadcast_s, RTA-5; 0 = off).
         *
         * The S contract is otherwise written exactly once at first emit: its
         * realized per-constellation pivot map goes stale after the first
         * handover, and a late joiner reading the file tail never sees the
         * contract at all.  When > 0 (and @c network.serialize_sbasis_contract is
         * on), a CURRENT S line — refreshed realized pivots from the driver's
         * emit-baseline pivot map, same convention fields, generation id when
         * RTA-3 is armed — is re-emitted before the epoch block every N seconds of
         * product time, and additionally on any epoch whose emit carried a datum
         * change (markDatumChange fired).  The reader accepts repeated S lines
         * positionally (each updates its current contract state, surfaced per
         * epoch via @c NetworkProductRecord::contract_refresh); S lines never feed
         * the replay emitter, so replay stays deterministic.  Default 0 ⇒ single
         * header S line — byte-identical.
         */
        double networkContractRebroadcastS() const { return network_contract_rebroadcast_s_; }

        /**
         * @brief Producer: emitter staleness horizon in seconds of PRODUCT time
         *        (@c network.emitter_staleness_horizon_s, RTA-11; 0 = off).
         *
         * The emitter's @c last_phase_bias_ / @c last_code_bias_ / clock
         * first-sighting markers persist across a multi-hour satellite vanish; on
         * the satellite's return the |Δbias| jump test compares against the STALE
         * value and bumps the discontinuity counter — semantically a
         * first-sighting-after-gap, not a fault (the consumer holds no surviving
         * ambiguity for a satellite gone that long).  When > 0 the emitter keeps a
         * last-emitted EPOCH TIME per (sat[,signal]) key — product time, never
         * wall clock, so replay is deterministic — and a key reappearing after a
         * gap larger than the horizon is treated as a first sighting on the
         * phase-bias, code-bias AND clock channels alike: the baseline reseeds and
         * no jump/forced bump lands.  The horizon travels in the serialized S
         * contract (@c stale= field) so @c makeReplayEmitter reproduces identical
         * counters with no out-of-band knowledge.  Default 0 ⇒ the legacy
         * keep-forever baseline — byte-identical.
         */
        double networkEmitterStalenessHorizonS() const { return network_emitter_staleness_horizon_s_; }

        /**
         * @brief Whether the curated RT-service producer profile is on
         *        (@c network.service_mode, RTB-8).
         *
         * One switch that ARMS the coherent RT-service producer toggle set at
         * load time (clock/URA discontinuity channels + retro-bump, two-state
         * clock model, phase-leveling, both RAIM tiers + the σ̂0 withhold and
         * its per-constellation carve-out, epoch-status heartbeat, pivot
         * vanish-return signalling, sticky pivot, scoped handover response,
         * generation id, disc-counter serialization, v2 S contract +
         * re-broadcast, per-epoch flush, health heartbeat, trace rotation).
         * BUNDLE-WINS: it overrides explicit disables of the armed keys — each
         * override is individually warned at load; run without @c service_mode
         * to hand-tune.  By the time the config is loaded the armed toggles
         * already carry their service values, so at runtime this getter is
         * PROVENANCE only: the L15 run manifest (and operator tooling) records
         * that the run's toggle set came from the bundle rather than
         * hand-assembly.  Default false ⇒ nothing armed (byte-identical).
         */
        bool networkServiceMode() const { return network_service_mode_; }

        /**
         * @brief Standardized post-fit residual threshold σ for the network RAIM
         *        (@c network.raim_k_sigma).
         *
         * A carried-ambiguity phase row whose standardized post-fit residual
         * @f$|r|\sqrt{w}@f$ exceeds this is flagged.  A large blunder (a frozen
         * multi-cycle slip leaking metres into the shared clock) sits far above the
         * mm-level phase noise, so a wide default (6σ) catches catastrophic misses
         * while leaving routine 1-cycle slips to the pre-processors.  Default 6.
         */
        double networkRaimKSigma() const { return network_raim_k_sigma_; }

        /** @brief Floor on the robust a-posteriori variance factor σ̂0 in the network
         *  RAIM gate (@c effect/σ̂0 > raim_k).  Default 1.0 (never trust the fit tighter
         *  than the a-priori stochastic model).  Lower (e.g. 0) to let σ̂0 fall below 1
         *  when the network fits inside its a-priori σ — diagnostic; TIGHTENS the gate. */
        double networkRaimSigma0Floor() const { return network_raim_sigma0_floor_; }

        /**
         * @brief Cross-station consensus count for the network RAIM
         *        (@c network.raim_min_stations).
         *
         * A satellite whose carried ambiguities are flagged at @b at-least this many
         * stations is refloated network-wide (all stations, including the guilty one
         * that hides the slip in its shared clock).  The consensus rejects
         * single-station noise; a directly-flagged (station, satellite) is also
         * refloated on its own.  Default 3.
         */
        int networkRaimMinStations() const { return network_raim_min_stations_; }

        /**
         * @brief Consecutive-epoch persistence gate for isolated-arc RAIM refloats
         *        (@c network.raim_arc_persist_epochs).
         *
         * A single (station, satellite) interaction spike surfaced by the
         * prominence attribution (an "isolated arc": no bad-satellite and no
         * bad-station attribution) is refloated only after its standardized
         * interaction residual has exceeded the gate for this many
         * @b consecutive epochs.  A genuine cycle slip persists as an integer
         * step until refloated, so it trips the gate every epoch and clears
         * the counter; a heavy-tail / model-error statistical spike does not
         * repeat on the same arc, so it never reaches the threshold — this
         * starves the futile once-off refloats that churn the emitted
         * phase-bias datum (the 305-station consumer-DoS) without delaying real
         * slips or touching the corroborated consensus / whole-satellite tiers.
         * Applies ONLY to the isolated-arc tier of the attribution refloat
         * (@ref networkRaimAttribution); whole-satellite and cross-station
         * consensus refloats are unaffected.  Default 1 = act on the first
         * spike (byte-identical to the ungated behaviour); 2 requires two
         * consecutive epochs, etc.  The per-arc counter resets after a refloat
         * (so a persistent arc refloats at most once per K epochs) and on any
         * epoch the arc is not flagged.
         */
        int networkRaimArcPersistEpochs() const { return network_raim_arc_persist_epochs_; }

        /// @brief T2d: gate isolated-arc RAIM refloats on filter maturity (@c network.raim_refloat_maturity_gate).
        bool networkRaimRefloatMaturityGate() const { return network_raim_refloat_maturity_gate_; }
        /// @brief T2d: emitted-epoch maturity horizon for the isolated-arc refloat gate (@c network.raim_refloat_maturity_epochs).
        int networkRaimRefloatMaturityEpochs() const { return network_raim_refloat_maturity_epochs_; }

        /**
         * @brief Minimum network-station-coverage fraction for a signal to enter
         *        the network solve (@c network.signal_min_station_fraction).
         *
         * A unique RINEX observation code (e.g. GPS @c L2S, @c L1W) is admitted
         * to the producer only if at least this fraction of the stations that
         * observe its constellation actually track it across the network.  This
         * is a NETWORK-WIDE coverage gate — unlike @c channel_mode which selects
         * a canonical channel @b per station (and so mixes L1W at one station
         * with L1C at another for the same satellite), this drops a
         * sparsely-tracked channel everywhere, so every retained signal's
         * satellite bias is estimated from a dense, consistent station set.  The
         * per-band canonical then falls through to the best @b admitted channel
         * (e.g. L1W excluded ⇒ L1C).  Applies to @c PPP_RTK_Network mode only.
         * Default 0 = off (admit every tracked channel — byte-identical).  A
         * typical strict value is 0.5.
         */
        double networkSignalMinStationFraction() const { return network_signal_min_station_fraction_; }

        /**
         * @brief Minimum network-station-coverage fraction for a signal to get an
         *        estimated satellite PHASE BIAS (@c network.phase_bias_min_station_fraction).
         *
         * The softer sibling of @ref networkSignalMinStationFraction: instead of
         * DROPPING a sparsely-tracked signal, KEEP its observation but do not
         * estimate a per-(satellite,signal) phase-bias state for it — the float
         * ambiguity then absorbs the bias, so the signal still contributes its
         * geometry to the clock/iono/position solve but is NOT integer-AR-eligible
         * (the producer emits no phase bias for it, so a coverage-honest consumer
         * excludes it from AR automatically).  This removes the ill-conditioned,
         * wandering shared phase-bias state (the churn/differential-instability
         * source) WITHOUT discarding the measurement's information — preferable to
         * dropping when the signal is still geometrically useful.  Applies to
         * @c PPP_RTK_Network mode only.  Default 0 = off (every tracked phase signal
         * gets a bias state — byte-identical).  A typical strict value is 0.5.
         */
        double networkPhaseBiasMinStationFraction() const { return network_phase_bias_min_station_fraction_; }

        /**
         * @brief Whether a station whose receiver antenna has no ATX model match is
         *        excluded from satellite phase-bias emission contributions
         *        (@c network.phase_bias_exclude_unmatched_antenna).
         *
         * A receiver antenna type that matches no entry in the loaded ANTEX model
         * gets ZERO receiver PCO/PCV correction (@ref Antenna::getAntennaCorrection
         * silently returns the satellite-side terms only) — a centimetre-level,
         * elevation-dependent phase-centre error left in that station's carrier
         * phase.  Folded into the joint solve it biases the per-(satellite,signal)
         * phase biases the whole network shares and emits.  When on, such a station's
         * carrier phase is WITHHELD from every satellite phase-bias state (its float
         * ambiguity absorbs the arc, exactly as @ref networkPhaseBiasMinStationFraction
         * does per-signal) while it still contributes to the satellite clock, code
         * bias, ionosphere and its own position — the per-station analogue of the
         * phase-bias coverage float-gate.  The clock-datum (pivot) station is never
         * excluded (it realizes the phase-bias datum); an unmatched pivot is warned
         * about instead.  Applies to @c PPP_RTK_Network mode only.  Default false
         * (every station contributes — byte-identical).
         *
         * @see networkPhaseBiasMinStationFraction, Antenna::hasReceiverAntennaMatch
         */
        bool networkPhaseBiasExcludeUnmatchedAntenna() const { return network_phase_bias_exclude_unmatched_antenna_; }

        /**
         * @brief Whether RAIM refloats are driven by prominence attribution
         *        (@c network.raim_attribution) instead of the per-satellite consensus.
         *
         * When on, each RAIM epoch's post-fit residuals are decomposed by a robust 2-way
         * median polish into satellite / station / isolated-arc effects, and the refloat
         * is targeted at the dominant dimension: a bad SATELLITE refloats network-wide, a
         * bad STATION refloats only that station's arcs, an isolated ARC refloats just that
         * (station, sat) — instead of refloating every station of any consensus-flagged
         * satellite.  The gate is @c effect/σ̂0 > @ref networkRaimKSigma per dimension.
         * Default false keeps the per-satellite consensus behaviour.
         */
        bool networkRaimAttribution() const { return network_raim_attribution_; }

        /**
         * @brief Whether a "so bad" epoch triggers within-epoch adaptation
         *        (@c network.raim_within_epoch).
         *
         * When the robust a-posteriori variance factor σ̂0 exceeds @ref
         * networkRaimSigma0Trigger the joint solve is contaminated enough that emitting it
         * would be wrong; this arms the detect→drop-culprit→re-solve adaptation.  Default
         * false.
         *
         * @note A satellite/arc culprit is handled by REFLOATING its carried ambiguity — the
         * re-solve withholds the stale carry prior so the ambiguity re-estimates from full-weight
         * data (the network analogue of a large-q reinitialization) and absorbs the slip locally,
         * and the harvest re-stashes the corrected value.  This is self-contained (no lock-in even
         * at zero carry process noise) and keeps the ambiguity observable (no drop-induced
         * singularity).  A whole-station culprit — a data-quality problem, not a slip — is instead
         * excluded.  @ref networkRaim and @ref networkRaimAttribution are auto-enabled alongside
         * this flag as the complementary post-fit tier (catching slips below the σ̂0 trigger).
         */
        bool networkRaimWithinEpoch() const { return network_raim_within_epoch_; }

        /**
         * @brief σ̂0 above which an epoch is "so bad" that within-epoch adaptation fires
         *        (@c network.raim_sigma0_trigger; used only when @ref networkRaimWithinEpoch).
         *
         * σ̂0 = max(1, 1.4826·median|v·√w|) is the network-wide robust residual level: 1 on a
         * clean epoch, several on an all-move-together contamination.  Default 3.0.
         */
        double networkRaimSigma0Trigger() const { return network_raim_sigma0_trigger_; }

        /**
         * @brief σ̂0 ceiling above which a within-epoch adaptation FAILURE withholds the
         *        epoch instead of emitting it (@c network.raim_withhold_sigma0, A4; used
         *        only when @ref networkRaimWithinEpoch).
         *
         * After the capped drop-culprit/re-solve adaptation, if σ̂0 STILL exceeds this
         * ceiling the shared solution is too contaminated to publish; it is withheld
         * through the same disposition as the observability gate (no emit, no carry
         * stash, epoch skipped) so the contaminated solve never enters the store or the
         * carry.  0 (default) = off: the epoch is emitted anyway with a warning (current
         * behaviour).  A sensible value is ~2× @ref networkRaimSigma0Trigger.
         */
        double networkRaimWithholdSigma0() const { return network_raim_withhold_sigma0_; }

        /**
         * @brief Per-CONSTELLATION σ̂0 carve-out of the A4 withhold
         *        (@c network.per_const_withhold, RTA-10; used only when
         *        @ref networkRaimWithholdSigma0 > 0).
         *
         * When the whole-epoch σ̂0 withhold trips, recompute σ̂0 restricted to each
         * constellation's carried-ambiguity phase rows (the SAME standardized-residual
         * pool the global σ̂0 is the median of, partitioned by satellite constellation)
         * and publish the constellations that individually pass the SAME ceiling with
         * at least @ref networkPerConstWithholdMinRows rows — a localized fault (e.g.
         * one constellation's clock product) then degrades SCOPE instead of
         * availability.  Constellations failing either test are pruned from the
         * emission exactly like the RT0-5 runaway quarantine; if NONE passes the
         * whole-epoch withhold applies unchanged.  The rank/observability gate keeps
         * whole-epoch semantics (a rank deficiency is an invalid datum — no healthy
         * subset exists).  Default off ⇒ the A4 withhold is all-or-nothing
         * (byte-identical).
         */
        bool networkPerConstWithhold() const { return network_per_const_withhold_; }

        /**
         * @brief Minimum standardized-residual rows a constellation needs to be
         *        JUDGEABLE by the per-constellation carve-out
         *        (@c network.per_const_withhold_min_rows, RTA-10).
         *
         * A constellation with fewer rows in the σ̂0 pool has too little redundancy
         * for its σ̂0_c to certify cleanliness under a globally-contaminated epoch, so
         * it is withheld rather than published unjudged.  Default 20.
         */
        int networkPerConstWithholdMinRows() const { return network_per_const_withhold_min_rows_; }

        /**
         * @brief Product-level epoch-status heartbeat
         *        (@c network.emit_epoch_status_record, RTA-10).
         *
         * When on, the producer writes one positional @c H record per epoch into the
         * product file — INCLUDING fully withheld epochs (observability-gate and A4
         * σ̂0 withholds), which otherwise emit nothing — so a standalone tail consumer
         * can distinguish "producer withheld" from "producer died":
         * @c H,week,tow,status,sigma0,n_sats_published,withheld with
         * status ∈ {ok, partial, withheld_sigma0, withheld_gate}.  Like the S contract
         * line it is positional and never feeds the replay emitter.  Coverage starts
         * at the first emit (the deferred product writer opens then).  Default off ⇒
         * no H records (byte-identical product file).
         */
        bool networkEmitEpochStatusRecord() const { return network_emit_epoch_status_record_; }

        /**
         * @brief Whether the network RAIM standardizes by per-row redundancy
         *        (@c network.raim_redundancy_normalize, B2).
         *
         * When on, each carried-ambiguity phase row's residual is divided by
         * @f$\sigma_i\sqrt{r_i}@f$ with the Baarda redundancy number
         * @f$r_i = 1 - w_i\,a_i^\top \mathrm{Cov}(\theta)\,a_i@f$ computed from the cached
         * per-station Schur factors (@f$N_{rr}^{-1}, N_{sr}@f$) and the datum-frame shared
         * covariance — no re-solve — before the σ̂0 pooling and median-polish attribution.
         * A low-redundancy arc (few checking stations), whose residual response is only
         * @f$r_i\cdot\text{fault}@f$, is otherwise systematically under-flagged.  Default
         * false reproduces the plain @f$|v|\sqrt{w}@f$ standardization bit-for-bit.
         */
        bool networkRaimRedundancyNormalize() const { return network_raim_redundancy_normalize_; }

        /**
         * @brief Whether the within-epoch adaptation drops the finest contaminated unit
         *        (@c network.raim_surgical_station; used only when @ref networkRaimWithinEpoch).
         *
         * The robust median polish separates a contaminated satellite (whole column), a
         * contaminated station (whole row), and an isolated (station, satellite) ARC
         * (interaction term).  With this on (default) the within-epoch re-solve can peel a
         * single bad ARC — de-weighting just that station-satellite's rows — instead of
         * escalating to excluding the entire station, and a whole-station exclusion is only
         * chosen when its effect exceeds the best satellite/arc candidate by @ref
         * networkRaimStationDropBias.  Off reproduces the coarse satellite-vs-whole-station
         * peel (no arc granularity, no station de-rating) for A/B comparison.
         */
        bool networkRaimSurgicalStation() const { return network_raim_surgical_station_; }

        /**
         * @brief Hysteresis factor biasing the within-epoch peel away from whole-station
         *        exclusion (@c network.raim_station_drop_bias; used only when @ref
         *        networkRaimSurgicalStation).
         *
         * A whole-station exclusion discards that station's good arcs as well as its bad
         * ones, so it is the most destructive within-epoch action.  A station is only chosen
         * over the best satellite/arc candidate when its median-polish row effect, DIVIDED by
         * this factor, still dominates — i.e. it must be @c raim_station_drop_bias× worse than
         * any single satellite or arc before the whole station is dropped.  Must be ≥ 1;
         * 1 disables the bias (station competes at face value).  Default 1.5.
         */
        double networkRaimStationDropBias() const { return network_raim_station_drop_bias_; }

        /**
         * @brief Whether the per-satellite carried-clock sanity monitor is armed
         *        (@c network.clock_monitor).
         *
         * The post-fit σ̂0 RAIM catches contamination that moves the SHARED residual level
         * (many innocent stations spike at once); it is BLIND to a lone satellite whose
         * carried clock has quietly locked into a physically impossible value — the clock
         * absorbs the error self-consistently, so σ̂0 stays ≈1 and the stiff @c sat_clock_q
         * carry then FREEZES the wrong value for the rest of the run (the multi-hour
         * single-satellite lock-in).  This monitor is the missing detector for that mode: on
         * the final (post-adaptation) solution it flags any satellite whose de-meaned clock
         * exceeds @ref networkClockMonitorAbsMaxM or whose one-epoch jump exceeds @ref
         * networkClockMonitorJumpMaxM, then REFLOATS its carried shared state — the clock (and
         * its coupled phase/code bias) are NOT re-stashed, so the next epoch injects no stale
         * prior and the clock re-estimates freely toward its data/anchor value (the shared-
         * parameter analogue of the ambiguity refloat).  When armed it also refloats the
         * carried clock of any satellite the within-epoch / post-fit RAIM already flagged, so a
         * slip that leaked into the shared clock reinitializes the clock too, not just the
         * ambiguity.  Default false ⇒ byte-identical (no clock ever withheld from the stash).
         */
        bool networkClockMonitor() const { return network_clock_monitor_; }

        /**
         * @brief De-meaned per-satellite clock magnitude (m) above which the carried clock is
         *        refloated (@c network.clock_monitor_abs_max_m; used with @ref networkClockMonitor).
         *
         * The test is on @c |Δδtˢ − median(Δδtˢ)| so the per-epoch datum/gauge offset (a
         * few metres, not error) is removed and only genuine per-satellite blow-ups remain.  A
         * healthy precise-product clock sits sub-metre to a few metres de-meaned; the lock-in
         * satellite reaches hundreds.  Default 20 m (well clear of the ordinary few-metre
         * spread, far below a lock-in).
         */
        double networkClockMonitorAbsMaxM() const { return network_clock_monitor_abs_max_m_; }

        /**
         * @brief One-epoch clock jump (m) above which the carried clock is refloated
         *        (@c network.clock_monitor_jump_max_m; used with @ref networkClockMonitor).
         *
         * Δδtˢ vs a precise product is smooth (sub-metre per 30 s for a healthy satellite);
         * the lock-in onset is a single-epoch lurch of hundreds of metres.  Tested only across
         * ADJACENT epochs of a stable datum (skipped on a pivot handover and right after a
         * refloat, which re-baselines).  Default 10 m (~100× a healthy epoch step).
         */
        double networkClockMonitorJumpMaxM() const { return network_clock_monitor_jump_max_m_; }

        /**
         * @brief Whether the clock monitor also refloats the carried clock of any satellite the
         *        within-epoch / post-fit RAIM already flagged (@c network.clock_monitor_couple_raim).
         *
         * The ambiguity RAIM tiers flag a satellite whose slip leaked into the SHARED clock; with
         * this on, that satellite's carried clock (and coupled phase/code bias) is reinitialized
         * alongside its ambiguity rather than left frozen.  It is OFF by default because those
         * tiers fire on false-positive-like flags on a clean network (product-neutral for the
         * ambiguity refloat, but it would needlessly churn clock carries) and because the target
         * failure — a lone lock-in — is BELOW the RAIM trigger and caught by the monitor's own
         * absolute/jump bounds regardless.  OFF ⇒ the monitor acts only on its own bounds, so a
         * clean window is byte-identical to @ref networkClockMonitor off.  Used only when @ref
         * networkClockMonitor.
         */
        bool networkClockMonitorCoupleRaim() const { return network_clock_monitor_couple_raim_; }

        /**
         * @brief Reinitialization standard deviation (m) a flagged satellite's carried shared
         *        state is loosened to (@c network.clock_monitor_reinit_sigma_m; with @ref
         *        networkClockMonitor).
         *
         * When the monitor flags a satellite, its carried clock (and coupled phase/code bias) is
         * NOT withheld from the stash; instead the next epoch folds it with @c var = this σ²
         * rather than the stiff @c sat_clock_q — a soft large-q reinitialization.  The weak prior
         * still anchors the parameter at its previous value (curbing the collective datum drift a
         * hard refloat induces on the rest of the constellation) yet is loose enough for the data
         * to walk a frozen clock back toward truth.  Larger = closer to a hard refloat; smaller =
         * gentler (more smoothing).  A value ≤ 0 restores the hard skip-stash refloat (no prior at
         * all).  Default 30 m.
         */
        double networkClockMonitorReinitSigmaM() const { return network_clock_monitor_reinit_sigma_m_; }

        /**
         * @brief Whether the carried satellite clock uses the two-state oscillator model
         *        (@c network.clock_model) instead of the scalar random-walk carry.
         *
         * Replaces the 1-state carry (prior = previous offset, var_eff = var + @c sat_clock_q·dt)
         * with a Zucca–Tavella two-state model x = [offset, drift] whose process noise Q(dt) is
         * sized from the clock's Allan/Hadamard deviation (@ref networkClockModelQ1, @ref
         * networkClockModelQ2).  The drift-following prediction is physically tight (~cm/epoch vs
         * the scalar model's ~0.5 m/epoch), so a self-consistent lock-in produces a large
         * NORMALIZED innovation — a detectable dynamics fault — rather than an invisible
         * magnitude; the clock monitor then flags on that per-satellite normalized innovation
         * (@ref networkClockModelKSigma), with the per-constellation datum common-mode removed,
         * instead of the hand-tuned absolute/jump bounds.  Default false ⇒ the scalar carry.
         */
        bool networkClockModel() const { return network_clock_model_; }

        /**
         * @brief White-FM diffusion coefficient q1 (m²/s) of the two-state clock model
         *        (@c network.clock_model_q1; used with @ref networkClockModel).
         *
         * The white-frequency-modulation term of the process noise; contributes q1·dt to the
         * predicted offset variance.  For the residual Δδtˢ (network clock vs the precise
         * product) this is typically negligible (RWFM-dominated); default 0.
         */
        double networkClockModelQ1() const { return network_clock_model_q1_; }

        /**
         * @brief Random-walk-FM diffusion coefficient q2 (m²/s³) of the two-state clock model
         *        (@c network.clock_model_q2; used with @ref networkClockModel).
         *
         * The dominant term for a precise-product residual clock; contributes q2·dt³/3 to the
         * predicted offset variance and q2·dt to the drift variance.  Obtain it by inverting the
         * clock's Allan/Hadamard deviation (Var of the second difference over dt = 2·q2·dt³/3).
         * Default 1.5e-7 (empirical GPS≈1.1e-7 / Galileo≈1.8e-7 for the network Δδtˢ residual).
         */
        double networkClockModelQ2() const { return network_clock_model_q2_; }

        /**
         * @brief Normalized-innovation threshold (σ) at which the two-state clock model flags a
         *        satellite (@c network.clock_model_k_sigma; used with @ref networkClockModel).
         *
         * The dynamics-fault statistic is |innovation − per-constellation-median-innovation| / √S,
         * S the predicted innovation variance.  A healthy clock sits near 1; a lock-in reaches
         * hundreds to thousands.  Default 6.
         */
        double networkClockModelKSigma() const { return network_clock_model_k_sigma_; }

        /**
         * @brief Std (m) of the network-solved offset treated as the two-state model's
         *        measurement (@c network.clock_model_meas_std_m; used with @ref networkClockModel).
         *
         * The R in the model's measurement update — how tightly the model tracks each epoch's
         * solved clock.  Default 0.05 m (the per-epoch solved-clock precision on a dense network).
         */
        double networkClockModelMeasStdM() const { return network_clock_model_meas_std_m_; }

        /**
         * @brief Max unseen gap (s) a satellite's carried two-state clock model survives
         *        before its state is treated as stale (@c network.clock_model_max_gap_s, R11).
         *
         * Governs the clock-model carry ONLY — distinct from @ref networkPhaseLevelingMaxGapS,
         * which drops carried ambiguities / ZWD.  Past this gap the predicted clock prior is
         * dropped (reseed from data + the a-priori anchor), the drift finite-difference is reset
         * to 0, and the emitted C1 drift falls back to ZOH (0) rather than shipping an hours-old
         * rate for one epoch after a vanish-return.  Defaults to @ref networkPhaseLevelingMaxGapS
         * so existing clock-model configs are byte-identical; set it independently to give the
         * (slower-varying) clock a longer carry horizon than the ambiguities.
         */
        double networkClockModelMaxGapS() const { return network_clock_model_max_gap_s_; }

        /// @brief Emit a per-satellite URA quality channel from the network product
        ///        (@c network.emit_ura, B1).  URA = EWMA(σ̂0) × formal clock σ (from the
        ///        shared covariance).  Default off ⇒ the product is byte-identical.
        bool networkEmitUra() const { return network_emit_ura_; }
        /// @brief Producer: stamp each emitted epoch with a wall-clock TIME-OF-GENERATION
        ///        (@c network.emit_generation_time, RT0-4), serialized as an optional @c G
        ///        record, so a consumer can measure product latency (received − generated).
        ///        Default off ⇒ no @c G record ⇒ the product is byte-identical.  The stamp
        ///        is wall-clock and therefore NON-deterministic, so a byte-reproducible
        ///        (regression) run must leave this off.
        bool networkEmitGenerationTime() const { return network_emit_generation_time_; }
        /// @brief Producer: per-constellation clock-runaway quarantine (@c network.clock_runaway_check,
        ///        RT0-5).  When on, a constellation whose median |clock − global median| exceeds
        ///        @ref networkClockRunawayMagM (or whose median |drift| exceeds
        ///        @ref networkClockRunawayRateMps) is DROPPED from this epoch's emission and flagged,
        ///        so a runaway (e.g. the open BeiDou clock-divergence bug) degrades SCOPE (one
        ///        constellation) instead of contaminating the whole product.  Default off ⇒ every
        ///        constellation emitted ⇒ byte-identical.
        bool networkClockRunawayCheck() const { return network_clock_runaway_check_; }
        /// @brief Per-constellation median clock-magnitude ceiling in meters (RT0-5).  Inert unless
        ///        @ref networkClockRunawayCheck.  Default 1000 m — far above a healthy few-metre
        ///        spread, below the runaway hundreds-of-km regime.
        double networkClockRunawayMagM() const { return network_clock_runaway_mag_m_; }
        /// @brief Per-constellation median clock-rate ceiling in m/s (RT0-5).  Inert unless
        ///        @ref networkClockRunawayCheck and the two-state clock model emits a drift
        ///        (@c network.clock_model).  Default 10 m/s — far above healthy sub-0.1 m/s drift.
        double networkClockRunawayRateMps() const { return network_clock_runaway_rate_m_s_; }
        /// @brief EWMA smoothing factor α for the network-wide σ̂0 that scales the URA
        ///        (@c network.ura_ewma_alpha, B1).  Default 0.1.
        double networkUraEwmaAlpha() const { return network_ura_ewma_alpha_; }
        /// @brief Consumer: inflate measurement R by the network URA on rows using that
        ///        satellite's corrections (@c network.consume_ura, B1).  Default off ⇒ R byte-identical.
        bool networkConsumeUra() const { return network_consume_ura_; }
        /// @brief Consumer: ZOH clock-staleness q² (m²/s³) folded as q²·dt³/3 into the
        ///        applied clock variance (@c network.ura_staleness_q2, B1).  The loader
        ///        defaults it to @ref networkClockModelQ2 (P1-3), not 0, so it is active
        ///        whenever @c network.consume_ura is on; an explicit 0 disables the inflation.
        double networkUraStalenessQ2() const { return network_ura_staleness_q2_; }
        /// @brief Producer: emit a per-(sat,signal) 1σ for the satellite PHASE-BIAS
        ///        corrections (@c network.emit_phase_bias_sigma, B1b).  σ = EWMA(σ̂0) ×
        ///        formal phase-bias σ from the shared covariance, gauge-exact (datum-
        ///        pinned + folded ensemble-gauge columns treated as EXACT, σ=0).  Rides
        ///        @c SsrPhaseBiasEntry.sigma_m in the in-process store (no wire record —
        ///        an internal, closed-loop-only channel).  Default off ⇒ σ stays 0 ⇒
        ///        the product/store is byte-identical.  Tienstra-§6.6 phase-bias analogue
        ///        of the clock URA: the consumer currently applies phase-bias corrections
        ///        as if EXACT, so this transmits the diagonal of Qŷ_B1 for the phase-bias
        ///        block — the named defect-2 driver (6–69 cm differential wander).
        bool networkEmitPhaseBiasSigma() const { return network_emit_phase_bias_sigma_; }
        /// @brief Consumer: inflate the measurement R of a PHASE row by the network
        ///        phase-bias correction σ² (@c network.consume_phase_bias_sigma, B1b), so a
        ///        wandering phase-bias correction is down-weighted instead of trusted as
        ///        exact.  Per-(sat,signal) via @c getPhaseBias; IF rows carry no per-signal
        ///        bias and are untouched.  Default off ⇒ R byte-identical.
        bool networkConsumePhaseBiasSigma() const { return network_consume_phase_bias_sigma_; }

        /// @brief M4 (contract v2): producer emits the per-key COMBINED-correction σ
        ///        (@c network.emit_combined_sigma).  When on, the driver computes the
        ///        combined PHASE σ (clock + phase bias) into @c NetworkPhaseBias.sigc_m and
        ///        the combined CODE σ + code/phase correlation (clock + code bias) into
        ///        @c NetworkSatSolution.sigcc_m / @c rho from the shared posterior
        ///        covariance's cross-terms, serialized as the @c sigc= / @c sigcc= /
        ///        @c rho= wire tokens.  Off ⇒ every value 0 ⇒ no token ⇒ product/store
        ///        byte-identical.  (Zhang 2021 / Psychas 2024b: the combined σ is mm-level
        ///        where the individual clock/bias σ are cm-level during rise-reconvergence.)
        bool networkEmitCombinedSigma() const { return network_emit_combined_sigma_; }
        /// @brief M4 (contract v2): age-scaling coefficient the producer advertises on the
        ///        S-line (@c network.combined_sigma_agek).  A consumer inflates the combined
        ///        variance as σc²·(1 + agek·Δt_age²) (correction content age Δt_age in s),
        ///        replacing the ad-hoc URA staleness ramp for network-corrected rows.
        ///        Default 0 ⇒ no @c agek= token ⇒ no age inflation (byte-identical).
        double networkCombinedSigmaAgek() const { return network_combined_sigma_agek_; }
        /// @brief M4 (contract v2): consumer folds the combined-correction σ (age-inflated)
        ///        into a network-corrected row's measurement variance INSTEAD OF the URA +
        ///        phase-bias-σ RSS (@c closed_loop.consume_combined_sigma).  Convenience
        ///        accessor onto the closed-loop sub-config so measurement-side callers match
        ///        the flat B1b getter style.  Default off ⇒ v1 weighting (byte-identical).
        bool networkConsumeCombinedSigma() const {
            return getNetworkConfig().closed_loop.consume_combined_sigma;
        }

        /// @brief T3a: gate consumer AR-eligibility on the product's @c signal_integer flag
        ///        (@c network.require_signal_integer_for_ar).  OFF = coverage-only gate (byte-identical).
        bool networkRequireSignalIntegerForAr() const { return network_require_signal_integer_for_ar_; }
        /// @brief Producer: retro-bump a flagged SATELLITE's discontinuity counter on the
        ///        NEXT emit after a post-emit QC flag (@c network.ura_retro_bump, B1) — a
        ///        per-satellite reset (P1-3), not constellation-wide.  Default off.
        ///        Arming it auto-enables @c network.emit_clock_disc (R4: the clock counter
        ///        only exists on that channel), which also scopes handover datum bumps per
        ///        constellation; it needs at least one flag-producing QC tier armed
        ///        (@c network.raim / @c raim_within_epoch / @c clock_monitor /
        ///        @c clock_model) or it can never fire (loader warns).
        bool networkUraRetroBump() const { return network_ura_retro_bump_; }
        /// @brief Producer: URA multiplier on a flagged satellite for the phase-leveling
        ///        warmup horizon after a refloat (@c network.ura_refloat_inflate, B1).  1 = off.
        double networkUraRefloatInflate() const { return network_ura_refloat_inflate_; }

        /**
         * @brief Testing fast-forward: hours of data to skip before the network begins
         *        estimating (@c network.start_offset_hours).
         *
         * Skips every synced epoch until this many hours have elapsed from the first one, then
         * begins a COLD-start solve at the offset (nothing is stashed while skipping).  Lets a
         * run reach a late event — e.g. the ~4.3 h single-satellite lock-in — in a handful of
         * warm-up epochs instead of hours of processing.  Because the carry restarts cold, this
         * reproduces an EXTERNALLY-driven event (eclipse / product anomaly after a fresh warm-up)
         * rather than the exact accumulated history.  Default 0 ⇒ process from the start.
         */
        double networkStartOffsetHours() const { return network_start_offset_hours_; }

        /**
         * @brief Access the co-estimated global SH-VTEC ionosphere configuration.
         * @return Const reference to the global SH-VTEC config.
         */
        const GlobalVtecShConfig& getGlobalVtecShConfig() const { return global_vtec_sh_; }

        /**
         * @brief Use the CIO-based IAU 2000A/2006 ECI↔ECEF frame transform.
         *
         * When true, Sun/Moon positions (and therefore solid-Earth tide,
         * ocean-tide loading, pole tide, antenna attitude and phase windup,
         * which all consume them) are computed with the rigorous CIO transform
         * (SOFA: iauXys00a / iauC2ixys / iauEra00 / iauSp00 / iauPom00 /
         * iauC2tcio, with observed dX/dY celestial-pole offsets when the ERP
         * source carries them) instead of the legacy IAU 1976/1980 equinox
         * chain.  Defaults to false, in which case the legacy path is
         * byte-identical to the historical behaviour.  Mode-agnostic.
         *
         * @return True if the CIO frame transform is enabled.
         */
        bool enableCioFrame() const { return enable_cio_frame_; }

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
         * @brief Std (m) the receiver code-bias state is re-inflated to on a
         *        consumed satellite code-bias datum discontinuity.
         *
         * Sized to the gauge re-reference step (cm–dm), deliberately NOT the
         * full @c initial_std.code_bias (which would make the near-frozen
         * IFB/DCB a free parameter on every rise/set).  Used by the PPP-RTK
         * user consumer when @c getCodeBiasDiscontinuity reports an increment.
         *
         * @return Re-inflation standard deviation in meters.
         */
        double codeBiasReinflateStd() const { return code_bias_reinflate_std_; }

        /**
         * @brief Std (m) the inter-system time-offset (ISB) state is re-inflated
         *        to on a consumed satellite CLOCK datum discontinuity
         *        (@c estimation.clock_disc_isb_reinflate_std).
         *
         * A network clock-datum re-reference steps ONE constellation's satellite
         * clocks by a common constant.  The receiver clock absorbs it for the
         * reference constellation, but for a non-reference constellation the step
         * is differential and lands on the near-frozen ISB state (q ≈ 1e-12),
         * which drags it for many epochs — the GPS-vs-GAL seam of the Jul-2026
         * closed-loop wire audit.  When > 0 the consumer's clock-disc handler
         * re-inflates the stepped constellation's ISB
         * (@c StateManager::reinflateSystemTimeOffsetVariance) so the next update
         * re-absorbs it in one epoch.  0 (default) keeps the handler note-only
         * (byte-identical).  Fires only on network products with the clock-disc
         * channel armed — RTCM-decoded clocks carry disc_counter=0.
         *
         * @return Re-inflation standard deviation in meters (0 = off).
         */
        double clockDiscIsbReinflateStd() const { return clock_disc_isb_reinflate_std_; }

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
         * @brief Whether the network producer estimates explicit per-(satellite,
         *        signal) PHASE-bias states.
         *
         * PPP_RTK_Network only.  When true, the network estimates the estimable
         * satellite phase bias (Odijk Eq. 4 — it carries the network pivot-
         * receiver ambiguity lump) on undifferenced phase rows instead of
         * importing it as an SSR correction.  Inert in every other mode.
         *
         * @return True if satellite phase-bias estimation is enabled.
         */
        bool estimateSatPhaseBiases() const { return estimate_sat_phase_biases_; }

        /**
         * @brief Whether the network producer estimates explicit per-satellite
         *        clock states (Δδtˢ relative to the a-priori product clock).
         * @return True if satellite clock estimation is enabled (PPP_RTK_Network).
         */
        bool estimateSatClocks() const { return estimate_sat_clocks_; }

        /**
         * @brief Whether the satellite CODE-bias state is promoted to first-class
         *        (always estimated) rather than created only on missing BIA/SSR
         *        coverage.
         *
         * The network producer must estimate its own satellite code bias on every
         * uncombined code row regardless of product coverage (to produce the
         * network's own bias product and detect product-side errors).  Defaults
         * false so the single-receiver user side keeps its fallback-only
         * behaviour byte-identical.
         *
         * @return True if the satellite code-bias state is always-on.
         */
        bool promoteSatCodeBias() const { return promote_sat_code_bias_; }

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
         * @brief Whether to propagate the reference position with an ITRF station velocity.
         *
         * When true (and a non-zero @c reference_velocity_ecef plus a non-zero
         * @c reference_position_epoch_year are configured), the fixed a-priori
         * reference position is propagated to the observation epoch via linear
         * plate motion: r(t) = r0 + v·(t − t0).  Mode-agnostic.
         *
         * @return True if ITRF velocity propagation is enabled.
         */
        bool enableItrfVelocity() const { return enable_itrf_velocity_; }

        /**
         * @brief Get the configured reference-station ITRF velocity (m/yr, ECEF).
         * @return Velocity as {Vx, Vy, Vz} in meters/year.
         */
        const std::array<double, 3>& getReferenceVelocityEcef() const { return reference_velocity_ecef_; }

        /**
         * @brief Get the epoch (decimal year) at which @c reference_position_ecef is defined.
         * @return Reference-position epoch as a decimal year (e.g. 2015.0); 0 if unset.
         */
        double getReferencePositionEpochYear() const { return reference_position_epoch_year_; }

        /**
         * @brief Reference ECEF position propagated to a given epoch by ITRF velocity.
         *
         * Returns r0 + v·(year(t) − t0) when ITRF velocity propagation is enabled
         * and a velocity + reference epoch are configured; otherwise returns the
         * unpropagated @c reference_position_ecef.  An invalid epoch @p t
         * (week ≤ 0) also yields the unpropagated value.  No filter state is
         * involved — this is a deterministic shift of the fixed a-priori.
         *
         * @param t Observation epoch to propagate to.
         * @return Propagated reference position as {X, Y, Z} in meters.
         */
        std::array<double, 3> getReferencePositionEcefAtEpoch(const GnssTime& t) const;

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
         * @brief Const access to the PPP-RTK network producer configuration.
         * @return The NetworkConfig (default-constructed/unused unless network mode).
         */
        const NetworkConfig& getNetworkConfig() const { return network_config_; }

        /**
         * @brief Check if the active mode is the PPP-RTK network producer.
         * @return True when @c processing_mode_ is @c PPP_RTK_Network.
         */
        bool isNetworkMode() const {
            return processing_mode_ == ProcessingMode::PPP_RTK_Network;
        }

        /**
         * @brief Override one network station's RINEX path (PathResolver fill-in).
         * @param idx Index into the network stations vector.
         * @param path New RINEX path.
         */
        void setNetworkStationRinexPath(size_t idx, const std::filesystem::path& path) {
            if (idx < network_config_.stations.size())
                network_config_.stations[idx].rinex_path = path;
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
         * @brief Whether a live source supplants precise SP3 ORBITS specifically.
         *
         * RTB-4: splits the orbit half out of @ref realtimeUsesLiveOrbitClockSource
         * so a config can pin PRECISE (e.g. ultra-rapid) orbits while satellite
         * CLOCKS still come from the live/network stream ("ULT orbits + network
         * clocks").  With @c realtime.use_precise_orbits false (default) this
         * returns EXACTLY @ref realtimeUsesLiveOrbitClockSource, so orbit and clock
         * gates move together and every product-resolution site is byte-identical.
         * When true (real-time only) it returns false — precise orbits ARE loaded —
         * while the clock gate (@ref realtimeUsesLiveOrbitClockSource) stays live,
         * so only the precise CLK is skipped.
         *
         * @return True when precise SP3 orbits should be skipped in favour of the
         *         live orbit path.
         */
        bool realtimeUsesLiveOrbitSource() const {
            if (app_mode_ != AppMode::RealTime) return false;
            if (realtime_config_.use_precise_orbits) return false;  // RTB-4: pin precise orbits
            return realtimeUsesLiveOrbitClockSource();
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
         * @brief Partial ambiguity resolution toggle.
         *
         * When true, a LAMBDA fix that fails its full-set acceptance gates
         * (ratio / bootstrap success rate / per-pair fractional cap) is retried
         * on progressively smaller subsets — dropping the least-fixable
         * ambiguity each round and re-validating the full battery — so a few
         * noisy satellites no longer force the whole epoch back to float.
         * Complements (does not replace) the full-set validation; the AR trace
         * is bit-identical to the legacy path when off.
         */
        bool getArEnablePartialAr() const { return ar_enable_partial_ar_; }

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
        bool loadNetworkSection_(const nlohmann::json& json, LogManager& log_manager);
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
        double ssr_bias_max_age_s_{ 30.0 };               ///< A5: max |obs − SSR-code/phase-bias-record| age (s) before the bias overlay is rejected; default matches ssr_max_age_s_.  Only enforced when enable_ssr_bias_max_age_.
        bool   enable_ssr_bias_max_age_{ false };         ///< A5: enforce the SSR code/phase-bias staleness gate; OFF = the bias getters never gate on age (byte-identical).
        bool   enable_ssr_bias_max_age_explicit_{ false };///< RTA-9(a): true iff the user WROTE processing.enable_ssr_bias_max_age (either key form) — lets applyNetworkConsumerProfile arm the gate under bias consumption while respecting an explicit false.
        double ssr_bias_stale_hold_factor_{ 2.0 };        ///< A5: apply-gate drop limit = factor × ssr_bias_max_age_s_ (hold the last bias over sub-limit staleness gaps → no manufactured model step / reset storm); 1.0 = drop exactly at max age.  Only meaningful when enable_ssr_bias_max_age_.
        double ssr_clock_stale_hold_factor_{ 0.0 };       ///< RTA-9(b): clock stale-hold rung — a clock aged in (ssr_max_age, factor × ssr_max_age] is still applied with URA-staleness R inflation; 0 (default) = off (byte-identical two-state gate); (0,1] clamps to 1.
        bool   ssr_bias_band_fallback_{ false };          ///< RTA-13 (opt-in): CODE-domain band-wise bias fallback — exact-attribute miss consumes the same-band sibling code bias; phase never falls back.  Default off = store lookups byte-identical.
        bool   enable_obs_ssr_sync_{ false };             ///< RT-only: hold obs ahead of the SSR coverage horizon (obs↔SSR sync) instead of processing against absent SSR.
        double obs_ssr_sync_max_hold_s_{ 30.0 };          ///< RT-only: max seconds to hold obs for SSR before degraded fallback (obs↔SSR sync liveness bound).
        std::string rinex_types_;                        ///< Enabled RINEX observation type letters (subset of "CLDS"); default code+phase.
        ReceiverAntennaOrientation receiver_antenna_orientation_; ///< Antenna orientation parameters.
        bool enable_rx_antenna_orientation_{ false };  ///< Apply receiver antenna azimuth orientation to the RX PCV look-up (off → byte-identical).
        bool enable_rx_antenna_substitution_{ false }; ///< Allow rule-driven receiver antenna model substitution (deprecated-code table + radome→NONE) when no exact ATX match.
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
        bool   enable_variance_factor_{ false };   ///< Compute & report the a-posteriori variance factor σ̂₀² (reporting only; propagated P untouched).
        bool   enable_data_snooping_{ false };     ///< Baarda data-snooping QC (per-row redundancy/w-test/MDB + w-test outlier gate).
        double data_snooping_critical_{ 3.29 };    ///< Baarda w-test critical value (|w| reject bound; α₀=0.001 → 3.29).
        bool   enable_full_r_kernel_{ false };     ///< Honour off-diagonal R in the post-fit residual-covariance kernel (Sr = R − H·P⁺·Hᵀ).
        double bad_meas_percent_;        ///< Percentage of bad measurements allowed.
        double ambiguity_std_;           ///< Ambiguity standard deviation.
        double code_noise_amplify_;      ///< Code noise amplification factor.

        std::filesystem::path otl_blq_path_;    ///< Path to BLQ ocean loading file (optional).
        std::filesystem::path opt_grid_path_;   ///< Path to IERS ocean pole tide load grid (optional; default <data>/static/opole/).
        std::filesystem::path atm_tide_grid_path_;  ///< Path to S1/S2 atmospheric-tide load grid (optional; default <data>/static/apl/).

        bool enable_station_info_{ false };                     ///< Master toggle for the SIF subsystem (default off = byte-identical).
        std::vector<std::filesystem::path> station_info_paths_; ///< SIF JSON paths in load order (explicit or auto-discovered).
        std::shared_ptr<const StationInfoFile> station_info_;   ///< Loaded SIF registry (nullptr when disabled/empty).

        bool estimate_tropo_gradients_;   ///< Estimate troposphere horizontal gradients (Gn, Ge).
        bool   relinearize_geometry_;            ///< Re-run computeModels after first KF update to recompute geometry at updated position.
        double relinearize_dpos_threshold_phase_m_;     ///< |Δpos| threshold (m) when phase observables are present.
        double relinearize_dpos_threshold_code_only_m_; ///< |Δpos| threshold (m) when no phase observables (code-only).
        int    relinearize_max_count_;              ///< Hard cap on total relinearizations per run; -1 = unlimited.
        int    eigen_threads_;                       ///< Eigen thread count (0=default, 1=sequential, N=pinned).  See eigenThreads().
        bool run_smoother_;                    ///< Run backward RTS smoother after forward Kalman pass (post-processing only).
        bool enable_neq_export_{ false };      ///< Accumulate & export cross-session normal equations (.neq/.asc); post-processing only.
        bool enable_trig_sp3_interp_{ false }; ///< Use sidereal-trig (sin/cos) SP3 position interpolation instead of the default polynomial (Neville) path. Default off (byte-identical legacy).
        bool enable_orbit_normal_attitude_{ false }; ///< Use beta-driven orbit-normal yaw attitude for satellite PCO/PCV; else nominal yaw-steering + legacy BeiDou-2 GEO orbit-normal. Default off.
        bool enable_noncausal_slip_bridge_;    ///< Non-causal cycle-slip bridge before the smoother (post-processing only).
        int  slip_bridge_half_window_epochs_;  ///< Half-window (epochs) per side for the bridge GF/MW fit.
        double pre_bridge_float_tol_cycles_;   ///< Accept tolerance (cycles) for the independent pre-bridge-float gate.
        double pre_bridge_conv_var_m2_;        ///< Var (m^2) below which a post-reset float counts as re-converged.
        bool enable_auto_report_;              ///< Generate gnss_report.html via pyviz at the end of a post-processing run.
        std::string report_backend_;           ///< Auto-report backend: "matplotlib" (static) | "plotly" (interactive).
        std::string report_python_;            ///< Optional python interpreter override for the auto report ("" = baked venv -> PATH).
        std::vector<std::string> report_viewers_; ///< Curated viewer subset for the auto report (empty = all viewers).
        bool report_async_;                    ///< Launch the auto report detached so the run returns without waiting on rendering.
        bool estimate_phase_clock_;            ///< Estimate separate phase clock offset.
        bool enable_if_slip_detector_;             ///< INERT — IfPhaseSlipDetector is staged/uncompiled and has no instantiation path yet (see include/slip/IfPhaseSlipDetector.hpp). Parsed for forward-compat; toggling it currently has no effect.
        bool accumulate_phase_windup_;                ///< Accumulated (unwrapped) phase wind-up for IAR readiness.
        bool enable_between_satellite_sd_;                ///< Between-satellite single-difference operator in measurement model.
        bool enable_glonass_bssd_;                        ///< Allow GLONASS satellites in BSSD (FDMA ICB → off by default).
        bool enable_glonass_ifb_;                         ///< Estimate per-FCN-slot GLONASS receiver code IFB (requires estimate_ifb + uncombined).
        bool enable_glonass_network_{false};              ///< Admit GLONASS to the PPP-RTK network producer (code-only: sat clock + per-slot rx IFB; no FDMA phase bias).
        bool estimate_network_sat_code_bias_{false};      ///< Estimate satellite CODE bias (OSB) in the PPP-RTK network producer (W6; forces sat-bias/ifb/iono-weighting on, .BIA off).
        bool estimate_network_iono_sh_{false};            ///< Co-estimate the global SH-VTEC ionosphere in the network producer (forces W6; replaces external GIM as the iono constraint).
        NetworkAprioriSource network_apriori_source_{NetworkAprioriSource::Sp3}; ///< RTF-1: producer a-priori orbit/clock source (network.apriori_source); Sp3 (default) = precise SP3/CLK identity (byte-identical), BrdcRts = broadcast eph + external RTS orbits. Parse+store in Card 1; consumed by the producer core (later RTF-1 card).
        double network_apriori_clock_anchor_sigma_m_{0.0};  ///< RTF-1/D6: a-priori satellite-clock anchor pseudo-obs σ (m); 0 (default) = auto (mode-resolved at the producer), explicit positive wins. Parse+store only in Card 1.
        bool   network_phase_leveling_{false};            ///< Carry per-station carrier-phase ambiguities across epochs (phase-leveling, Increment 1); OFF = byte-identical absolute solve.
        double network_phase_leveling_q_amb_{0.0};        ///< Carried-ambiguity process noise (m²/s) inflating var over the inter-epoch gap; 0 (default) = pure zero-process-noise accumulation (random-constant model).
        int    network_phase_leveling_warmup_epochs_{6};  ///< Emitted-epoch count before a carried ambiguity is trusted (cold-start convergence guard).
        double network_phase_leveling_max_gap_s_{120.0};  ///< Max unseen gap (s) a carried ambiguity survives before it is dropped (stale-carry / map-growth guard).
        double network_phase_leveling_var_floor_m2_{0.0}; ///< Floor (m²) on the carried prior variance (caps Λ — lock-in guard); 0 = off (no cap).
        double network_shared_carry_var_floor_pb_m2_{0.0}; ///< B4: floor (m²) on the D2 sat-PHASE-bias carried prior variance (caps Λ — bounds the zero-q lock-in while preserving Ge-2008 FCB averaging); 0 = off.
        double network_shared_carry_var_floor_cb_m2_{0.0}; ///< B4: floor (m²) on the D2 sat-CODE-bias carried prior variance (caps Λ); 0 = off.
        bool   network_phase_leveling_warmup_on_convergence_{false}; ///< T1d: gate the leveling-carry warmup on SH-field convergence (epoch-over-epoch coeff RMS Δ < thresh AND a₀₀ in physical band) instead of the fixed emitted-epoch count — avoids locking the wrong iono/ambiguity split while a₀₀ is still diverging.  Falls back to the epoch count when no SH field is active.  OFF = byte-identical.
        bool   network_phase_leveling_refloat_at_convergence_{false}; ///< T1d: one-shot flush of all carried ambiguities at the SH-field convergence rising edge (pairs with the DEFAULT early warmup: keep the epoch-count warm-up but drop the possibly-contaminated early split once the field settles, so the carry re-accumulates from the converged state).  OFF = byte-identical.
        double network_phase_leveling_conv_rms_thresh_tecu_{0.5};  ///< T1d: epoch-over-epoch SH coefficient-vector RMS-change threshold (TECU) defining "stationary" for the convergence latch.  Heuristic; flagged for tuning.
        bool   network_shared_carry_full_cov_{false};      ///< C1: carry the FULL dense cross-covariance of the shared border across epochs (information-form KF on the border; recovers the clock↔phase-bias off-diagonals the diagonal D2 stash discards) — fold Λ=(P_prev+Q)⁻¹ as one dense block, marginalize departed/pinned/retired keys, degrade to the diagonal fold on a non-PD block. OFF = legacy per-key diagonal carry (byte-identical).
        bool   network_carry_conditional_coupling_{false}; ///< Card 1: carry each persistent LOCAL parameter (phase-leveling ambiguity, ZWD) as a CONDITIONAL pseudo-observation row coupling it to the free shared border (coeff +1 on the local, regression M_r on the border, variance = pure conditional (N_rr⁻¹)_{rr}), NOT an independent diagonal prior — removes the shared-info double count so the sequential recursion equals the batch solve. REQUIRES shared_carry_full_cov (hard error otherwise). OFF = legacy diagonal addStationPrior carry (byte-identical).
        bool   network_carry_coupling_joint_whiten_{false}; ///< Card 1b: JOINTLY whiten a station's coupling rows (R = diag(var_eff) + M_w·diag(P_w)·M_wᵀ over the DISTINCT departed shared columns, LLᵀ-whitened) instead of marginalizing each departed column per-row into a scalar variance — restores the between-row (DD) correlation the diagonal marginalization destroys (the T7 flat-σ bug). REQUIRES carry_conditional_coupling (hard error otherwise). OFF = Card-1 per-row scalar marginalization (byte-identical).
        bool   network_carry_coupling_sh_marginalize_{false};  ///< Card 1d-SH: fully marginalize (q=∞ / departed treatment) the VtecSh terms of every coupling row — severs the amb↔SH carry feedback (GM-carried border vs RW-modeled κ mismatch); REQUIRES carry_conditional_coupling. OFF = shrink-carry SH terms (byte-identical).
        bool   network_carry_coupling_time_update_{false};  ///< Card 1d: apply the border TIME UPDATE to each coupling row at injection — shrink every PRESENT coupled column by κ_j = P_prev/(P_prev + q_j·Δt) (coefficient + RHS term) and inflate the row variance by M²·κ_j·q_j·Δt, with the per-kind q ladder exactly the dense fold's (clock-monitor-reinit sats: q=∞ ⇒ full marginalization).  Without it the rows re-assert the harvest-epoch border at full strength every epoch (the carried-DD random-walk / flat-σ defect, Card 1c). REQUIRES carry_conditional_coupling (hard error otherwise). OFF = raw harvested rows (byte-identical).
        bool   network_carry_zwd_{false};                 ///< B9: carry per-station tropospheric ZWD across epochs as a diagonal random-walk prior (via addStationPrior, alongside the ambiguity carry); OFF = per-epoch white-noise ZWD (byte-identical).
        double network_zwd_carry_q_{1e-9};                ///< B9: ZWD carry random-walk process noise (m²/s) inflating the carried ZWD variance over the inter-epoch gap (var_eff = var + q·Δt). Only used when network.carry_zwd.
        bool   network_carry_vce_{false};                 ///< M2 (LS-VCE): run the carry-row variance-component MONITOR — estimate σ̂ (Floor m² + Rate m²/s) per carry family per constellation from the post-fit carry innovations and log it against the hand floors. Calibration/honesty instrument ONLY; no feedback into the solve. OFF = byte-identical (no accumulator, no logging).
        bool   network_carry_vce_apply_{false};           ///< M2 v2: feed σ̂ back into the carry stochastic model (replace the hand floors). REQUIRES carry_vce. NOT IMPLEMENTED in this build (monitor v1 only) — setting it true is a hard load error.
        std::string network_carry_vce_components_{"pl"};  ///< M2: carry families the monitor accumulates (comma list of pl / pb / cb). Default "pl" (the phase-leveling ambiguity carry — the primary floor). Only used when carry_vce.
        int    network_carry_vce_window_epochs_{60};      ///< M2: exponential-forgetting half-life (emitted epochs) of the VCE accumulator; ≤0 ⇒ whole-run accumulation. Only used when carry_vce.
        bool   network_cycle_slip_turbo_edit_{false};     ///< T0 per-station MW/GF cycle-slip REPAIR of the carried ambiguities (phase-leveling Increment 2); OFF = Increment-1 byte-identical.
        double network_cycle_slip_turbo_edit_frac_max_{0.15}; ///< Max rounding residual for a network T0 repair to be trusted (float-MW gate; default 0.15).
        int    network_cycle_slip_turbo_edit_dn_max_{50};      ///< Max |dN| (cycles) for a trusted network T0 repair; larger ⇒ DROP/re-float (float-MW magnitude guard).
        bool   network_cycle_slip_mw_detect_{false};           ///< Use the MW detector as a pure DETECTOR (not the TurboEdit repair): a confident wide-lane integer jump OR a receiver LLI loss-of-lock REFLOATS the carried ambiguity (marks pending-reseed), never applying an integer.  Iono-safe at the 30 s product rate where MW+GF integer REPAIR is not.  Complements the IF-phase TD detector; OFF = byte-identical.
        bool   network_cycle_slip_if_phase_td_{false};         ///< IF-phase between-sat/between-epoch triple-difference slip DETECTION (refloats both bands of a flagged IF pair via pending-reseed when the carry is active); OFF = byte-identical.
        double network_cycle_slip_if_phase_td_k_mad_{8.0};      ///< k for the network IF-phase MAD gate: flag if |ΔΔφ_IF−med| > max(k·1.4826·MAD, floor). Loose (8–12); post-estimation phase validation is the sharp second pass.
        double network_cycle_slip_if_phase_td_floor_frac_{1.0}; ///< Floor as a fraction of the (1,1) IF jump c/(f1+f2)≈10.7 cm; 1.0 = the (1,1) jump itself (default; binds on a clean network), ~0.7 to catch a bare (1,1) reliably.
        int    network_cycle_slip_if_phase_td_min_pool_{30};    ///< Min pooled per-pair samples for a robust network MAD (advisory; under-count pairs are logged).
        double network_cycle_slip_if_phase_td_ewma_alpha_{0.15};   ///< EWMA recency weight for the per-(station,pair) elevation-normalized IF-phase noise scale; smaller = longer memory.
        double network_cycle_slip_if_phase_td_shrink_kappa_{20.0}; ///< Clean-sample count at which the per-station IF EWMA scale and the network pool prior are weighted equally (empirical-Bayes shrinkage w=count/(count+κ)).
        bool   network_raim_{false};              ///< Post-fit network RAIM (Increment 3, T2): cross-station post-fit residual consensus refloats a carried ambiguity whose slip the pre-processors missed; OFF = byte-identical.
        double network_raim_k_sigma_{6.0};        ///< Standardized post-fit residual threshold (σ) above which a carried-ambiguity phase row is flagged slip-contaminated.
        double network_raim_sigma0_floor_{1.0};   ///< Floor on the RAIM robust a-posteriori variance factor σ̂0 = max(floor, 1.4826·median|v·√w|); 1.0 default (never below the a-priori σ); lower lets σ̂0 drop below 1 (tightens the gate).
        int    network_raim_min_stations_{3};     ///< Cross-station consensus count: a satellite flagged at ≥ this many stations is refloated network-wide (catches the guilty station that hides its slip in the shared clock).
        int    network_raim_arc_persist_epochs_{1}; ///< Consecutive-epoch persistence gate for isolated-arc attribution refloats: an isolated (station,sat) spike is refloated only after it trips the gate this many epochs in a row (real slips persist; heavy-tail spikes don't). 1 = act immediately (byte-identical). Only affects the isolated-arc tier, not consensus/whole-sat refloats.
        bool   network_raim_refloat_maturity_gate_{false}; ///< T2d: suppress isolated-arc attribution refloats until the filter has MATURED (innovation-variance / σ̂0-EWMA stabilized, mirroring the warmup URA ramp) — the isolated-arc refloat is the self-sustaining warmup churn driver (2→1063/epoch); early arcs are warmup transients, not real slips. Whole-sat consensus refloats (network-wide contamination) are NOT gated. OFF = byte-identical (arcs refloat immediately).
        int    network_raim_refloat_maturity_epochs_{0};   ///< T2d: emitted-epoch maturity horizon for the isolated-arc refloat gate above (passed as the warmup-ramp horizon; combined with @c phase_leveling_warmup_epochs and the σ̂0-EWMA update count exactly like @c networkWarmupRampLifted). 0 = no horizon beyond the carry warmup (the gate is then effectively inert since RAIM already only runs once carry-warm); set > phase_leveling_warmup_epochs to actually suppress.
        double network_signal_min_station_fraction_{0.0}; ///< Network-wide signal coverage gate: a RINEX code is admitted to the producer only if ≥ this fraction of stations observing its constellation track it (drops sparse channels like L2S everywhere, unlike per-station channel_mode). 0 = off (admit all tracked channels, byte-identical). Network mode only.
        double network_phase_bias_min_station_fraction_{0.0}; ///< Phase-bias coverage gate: a signal below this network coverage keeps its observation but gets NO satellite phase-bias state (float ambiguity absorbs it, signal not AR-eligible) — the "float not drop" alternative. 0 = off (every phase signal gets a bias, byte-identical). Network mode only.
        bool   network_phase_bias_exclude_unmatched_antenna_{false}; ///< Exclude a station whose receiver antenna has NO ATX model match (zero receiver PCO/PCV → uncalibrated carrier phase) from every satellite phase-bias state — its arc is absorbed by the local float ambiguity (like phase_bias_min_station_fraction, but per-station and all-signals) while it still feeds the satellite clock/code-bias/iono/position. The clock-datum pivot is never excluded (warned instead). OFF = every station contributes (byte-identical). Network mode only.
        bool   network_raim_attribution_{false};  ///< Drive RAIM refloats by prominence attribution (median-polish sat/station/arc) instead of per-satellite consensus; OFF = consensus behaviour.
        bool   network_raim_within_epoch_{false}; ///< Arm within-epoch adaptation (detect→drop-culprit→re-solve) when σ̂0 exceeds the trigger; OFF = emit the first solve as today.
        double network_raim_sigma0_trigger_{3.0}; ///< σ̂0 above which an epoch is "so bad" that within-epoch adaptation fires.
        double network_raim_withhold_sigma0_{0.0}; ///< A4: within-epoch escape-hatch ceiling — if σ̂0 STILL exceeds this after the 5-pass adaptation, WITHHOLD the epoch (reuse the observability-gate disposition) instead of emitting with a warning; 0 = off (emit anyway, current behaviour). Used only when @ref networkRaimWithinEpoch. Typically ~2× @ref networkRaimSigma0Trigger.
        bool   network_per_const_withhold_{false};  ///< RTA-10: when the A4 σ̂0 withhold trips, carve the epoch per CONSTELLATION — publish each constellation whose own σ̂0_c (same standardized-residual pool, restricted to its rows) passes the SAME ceiling with ≥ min_rows rows; prune the rest (RT0-5-quarantine pattern). OFF = all-or-nothing withhold (byte-identical).
        int    network_per_const_withhold_min_rows_{20}; ///< RTA-10: minimum σ̂0-pool rows for a constellation to be JUDGEABLE by the carve-out; fewer ⇒ withheld (unjudgeable under a contaminated epoch). Inert unless network_per_const_withhold_.
        bool   network_emit_epoch_status_record_{false}; ///< RTA-10: write one positional H heartbeat record per epoch into the product file — including fully withheld epochs (gate + σ̂0), which otherwise emit nothing — so a tail consumer distinguishes "withheld" from "producer died". Positional (never feeds replay). OFF = no H records (byte-identical).
        bool   network_raim_surgical_station_{true};   ///< Within-epoch adaptation may peel a single (station,sat) ARC instead of excluding the whole station, and de-rates whole-station exclusion by @ref network_raim_station_drop_bias_; false = coarse sat-vs-whole-station peel (A/B baseline).
        double network_raim_station_drop_bias_{1.5};   ///< A whole-station exclusion is chosen only when its median-polish row effect / this factor still beats the best satellite/arc candidate (≥1; 1 disables the bias).
        bool   network_raim_redundancy_normalize_{false}; ///< B2: standardize the post-fit / within-epoch RAIM residual by v_i/(σ_i·√r_i) with the Baarda redundancy r_i = 1 − w_i·a_iᵀCov(θ)a_i (per-row, from the cached per-station Schur factors) before σ̂0 pooling / median-polish; low-redundancy arcs are otherwise under-flagged (response ∝ r_i·fault). OFF = plain |v|·√w (byte-identical); ON deliberately changes the statistic.
        bool   network_clock_monitor_{false};           ///< Per-satellite carried-clock sanity monitor: flag+refloat a sat clock past a physical bound (breaks the multi-hour single-sat lock-in the σ̂0 RAIM is blind to); OFF = byte-identical (no clock withheld from the stash).
        double network_clock_monitor_abs_max_m_{20.0};  ///< |Δδtˢ − median| bound (m): a satellite whose de-meaned clock exceeds this has its carried shared state refloated.
        double network_clock_monitor_jump_max_m_{10.0}; ///< One-epoch clock jump bound (m): a satellite whose clock moves more than this across adjacent stable-datum epochs is refloated.
        bool   network_clock_monitor_couple_raim_{false}; ///< Also refloat the carried clock of any satellite the within-epoch / post-fit RAIM flags (opt-in; OFF = monitor acts on its own abs/jump bounds only, byte-identical on a clean window).
        double network_clock_monitor_reinit_sigma_m_{30.0}; ///< Reinit σ (m) a flagged sat's carried clock/phase/code bias is loosened to next epoch (soft large-q reinit that keeps a weak anchor); ≤0 = hard skip-stash refloat.
        bool   network_clock_model_{false};              ///< Carry the satellite clock with the two-state (offset+drift) oscillator model + Allan-derived Q instead of the scalar random walk; a lock-in becomes a detectable dynamics fault. Default false = scalar carry.
        double network_clock_model_q1_{0.0};             ///< White-FM diffusion q1 (m²/s) of the two-state clock model (negligible for the product-residual clock).
        double network_clock_model_q2_{1.5e-7};          ///< Random-walk-FM diffusion q2 (m²/s³) of the two-state clock model (Allan-derived; dominant term).
        double network_clock_model_k_sigma_{6.0};        ///< Normalized-innovation (σ) threshold at which the two-state clock model flags a satellite (datum common-mode removed).
        bool   network_pivot_vanish_return_signal_{false}; ///< A1: also raise a datum-change (markDatumChange) when a constellation ABSENT last epoch RETURNS with a clock pivot != its last-ever pivot; OFF = only the adjacent-epoch pivot change signals (byte-identical).
        bool   network_emit_clock_disc_{false};            ///< A2: maintain a per-satellite CLOCK discontinuity counter (bumped on a per-constellation datum re-reference) and scope the forced phase/code bump to the affected constellation(s); OFF = no clock disc channel + a global bump (byte-identical).
        bool   network_code_bias_disc_jump_test_{false};   ///< Emitter CODE-bias magnitude jump test: also bump disc_per_signal on a fresh-baseline value jump > the phase disc threshold (0.05 m) — signals the weak-separation value wander the H1 structural triggers cannot see; travels in the S-contract (cbjump=). OFF = structural triggers only (byte-identical).
        bool   network_clock_disc_on_set_change_{false};   ///< Sum-to-zero datum-move signalling: bump a constellation's clock disc counters when its clock-satellite SET changes between emits (under Σδtˢ=0 a membership change re-references the datum, previously unsignalled); requires clock_datum_sum_to_zero, arms emit_clock_disc; travels in the S-contract (clkset=). OFF = forced-bump-only clock channel (byte-identical).
        bool   network_sticky_pivot_{false};               ///< RTA-1: keep the incumbent clock-pivot satellite per constellation while it remains registered (re-select only when it sets; config-pinned satellite still wins); OFF = lexical first-registered auto-pick every epoch (byte-identical).
        bool   network_clock_datum_sum_to_zero_{false};    ///< Realize the per-constellation clock gauge by a SUM-TO-ZERO over that constellation's satellite clocks (folded like the code-bias gauge) instead of a hard pivot pin — anchor-free, no handover seam, no pivot-absent SPOF; ignores clock_datum_satellite. OFF = legacy per-constellation pivot pin (byte-identical).
        bool   network_clock_datum_broadcast_anchored_{false}; ///< brdc_rts only: realize the clock gauge by PER-SATELLITE soft anchors to each satellite's OWN broadcast clock (the a-priori; σ = resolveNetworkAprioriClockAnchorSigmaM(), folded PRE-gate as the datum) — no pivot, no handover seam, and unlike sum-to-zero NO common-mode redistribution wander on rise/set: the absolute clock-product frame holds still against the broadcast reference. Mutually exclusive with clock_datum_sum_to_zero; requires apriori_source=brdc_rts (fail-loud). OFF = unchanged (byte-identical).
        bool   network_clock_signal_basis_hold_{false};    ///< W6 only: HOLD the per-satellite clock↔code-bias IF-pin reference pair (the ionosphere-free "signal basis" the emitted satellite clock is anchored to) FIXED across epochs — latch on first registration, re-latch only when a held reference signal leaves the ≥2-station set — so the emitted (δtˢ, dˢ) split is time-consistent (the IGS fixed-clock-convention analogue) instead of re-anchoring by the sat's inter-channel bias every time a sparse channel enters/leaves coverage (defect-2 differential-clock wander). Requires estimate_sat_code_bias (INERT+warned otherwise); orthogonal to the per-constellation gauge. OFF = per-epoch re-selection (byte-identical).
        bool   network_code_datum_time_linked_{false};     ///< Card 2 (W6 only): impose the code-bias datum (per-sat IF pin + per-(const,signal) sum-to-zero) as a TIME datum — pin the CHANGE of the previous epoch's solved values (veteran-referenced sum-to-zero RHS Σ b̂_prev, IF-pin RHS a1·b̂_prev(σ1)+a2·b̂_prev(σ2)) instead of the level to ZERO. Same row count (rank unchanged), so no set-mean wander/relaxation and no gauge-vs-carry weight fight; a no-veteran channel or first split re-seeds at 0 with a per-sat disc bump (value step ↔ signalled discontinuity). Prev-value map NOT snapshotted (warm resume re-seeds, signalled). Mutually exclusive with clock_signal_basis_hold (both re-target the IF-pin row — fail-loud). Requires estimate_sat_code_bias (INERT+warned otherwise). OFF = legacy zero-target datum (byte-identical).
        bool   network_scoped_handover_response_{false};   ///< RTA-2: scope the producer's INTERNAL pivot-handover response (D2 carry skip, clock-oscillator clear, clock-monitor jump suppression) to the constellation(s) whose datum changed; OFF = global response (byte-identical).
        bool   network_phase_datum_prioritized_{false};    ///< Realize the phase-bias gauge by a per-(sat,signal) PRIORITIZED ANCHOR receiver (highest-coverage tracker) instead of pinning only the single clock-pivot station; cures the low-elevation gauge jump + the pivot-absent SPOF, anchor handovers disc-signalled. OFF = legacy single-pivot pins (byte-identical).
        bool   network_phase_datum_ensemble_{false};       ///< Fix A: realize the phase-bias gauge by an ANCHOR-FREE sum-to-zero over the tracking receivers' ambiguities (Σ_r zˢ_{r,σ}=0, kernel Schur-projected fold) instead of a single-anchor pin — no pivot, no handover (kills the moving-datum churn), no SPOF, and every sat referenced to the same receiver ensemble (user DD stays integer, kills the AR-poison). Reuses the prioritized SPOF-cure machinery but elects no anchor; SUPERSEDES phase_datum_prioritized. OFF = anchor-pin datum (byte-identical).
        double network_phase_datum_hold_sigma_m_{0.0};     ///< Fix A seed-once + time-lock: HOLD the ensemble satellite phase bias with a stiff prior of this sigma (m) after seeding Σ_r zˢ=0 once per (sat,signal); cures the ~metre/epoch churn of the un-held ensemble (the emitted absolute bias references a per-epoch-re-realized float-ambiguity mean → dynamic datum on a static product). 0 = re-realize every epoch. INERT unless network_phase_datum_ensemble_.
        bool   network_phase_datum_eq34_{false};           ///< Card 3: realize the phase-bias gauge by the Eq-34 / de Jonge SINGLE-RECEIVER-CHAIN datum — per-(receiver,system,signal) RPB split state + one RPB pin + a spanning-tree of ambiguity pins per (const,signal) component, so the emitted between-satellite reference stays integer-consistent across ALL satellites while staying full-rank at global scale. Reuses the prioritized SPOF-cure machinery but replaces the anchor election with a chain election; SUPERSEDES phase_datum_prioritized, MUTUALLY EXCLUSIVE with phase_datum_ensemble (load-time throw). OFF = anchor-pin datum (byte-identical: no RPB state allocated, chain election never runs).
        std::string network_phase_datum_eq34_receiver_{};  ///< Card 3: preferred datum receiver (component 0) for the eq34 chain datum; empty = coverage auto-pick. Sticky incumbent still wins while present. INERT unless network_phase_datum_eq34_.
        bool   network_phase_datum_eq34_pin_round_{true};  ///< Card 3: on an eq34 re-attach/re-pin of an already-lit (sat,signal) edge, target the ROUNDED carried float (λ·round(x̂_prev/λ)) so the frame step is ≈0 cycles; false = always pin to 0 (A/B lever). INERT unless network_phase_datum_eq34_.
        double network_phase_datum_eq34_rpb_prior_sigma_m_{1.0}; ///< Card 3: loose per-epoch zero-mean prior σ (m) on every RPB column — the station-local information floor for the near-null RPB↔ambiguity split (Jul-10 field A/B instrument); 0 disables. INERT unless network_phase_datum_eq34_.
        double network_phase_datum_eq34_rpb_q_m2s_{3E-06};       ///< Card 3: q (m²/s) of the LOCAL δ_{r,σ} time-linking carry (D6 v2, local form) — suppresses the per-epoch RPB re-estimation bounce that poisons the individual-float phase-leveling carries; 0 = v1 fresh re-estimation. INERT unless network_phase_datum_eq34_.
        bool   network_phase_datum_eq34_carry_shift_{false};     ///< Card 3 EXPERIMENTAL: S-transform carried values on pin displacement instead of erase/skip (exact in the chain algebra; field Jul-11: injects error through the weakly-held δ split — DD step 86 vs 11-15 cyc/ep erase-based — root open). OFF = erase-based hygiene. INERT unless network_phase_datum_eq34_.
        bool   network_phase_datum_eq34_longevity_election_{false}; ///< Card 3 (I4): grow the eq34 spanning tree's NEW edges by arc longevity (rising arcs first, then high elevation; high-coverage receivers first) instead of lexical order, so tree edges are chosen to outlive the epoch — cuts b_s re-reference churn at the source (spec D2 step 3's dropped preference). Incumbent-edge seed untouched; deterministic tie-break; OFF = legacy lexical growth (B9 baseline). INERT unless network_phase_datum_eq34_.
        bool   network_phase_datum_eq34_extend_only_{false};       ///< Card 3 (I2): de Jonge §3.5 extend-only membership — retain a tree edge's sticky identity (parents/root/pinned integer) across a momentary row gap while its local_carry is alive (age-stamped to phase_leveling_max_gap_s), so a returning arc re-pins to its pre-gap integer (step ≈0) instead of silently re-leveling to a first-sighting 0. Tree growth unchanged (live arcs only); reroute/handover out of scope. OFF = legacy wholesale replace (B9 baseline). INERT unless network_phase_datum_eq34_.
        double network_phase_datum_pref_elev_deg_{15.0};   ///< Elevation (deg) above which a satellite counts toward a receiver's per-constellation coverage rank when ordering prioritized anchor candidates; does NOT mask the pin. INERT unless network_phase_datum_prioritized_.
        bool   network_phase_datum_sticky_{true};          ///< Keep the incumbent per-(sat,signal) anchor while it still tracks a usable ambiguity (re-elect only when it sets) to minimize handover disc. INERT unless network_phase_datum_prioritized_.
        int    network_phase_datum_min_stations_{1};       ///< Coverage-threshold gate on the prioritized anchored union: register/emit a shared per-(sat,signal) phase-bias column only when >= this many present stations track it with a usable ambiguity (1 = no gating, byte-identical; 2-3 excludes the thin columns behind one-station-pool re-levels and part of the clock↔PB split leak — gated keys' phase rows still serve the clock via their local ambiguities). INERT unless network_phase_datum_prioritized_.
        bool   network_station_pd_pin_aware_{false};       ///< Pin-aware station PD guard: the addStation local-block rank test references the MEDIAN |LDLᵀ pivot| instead of the max, so the prioritized datum's in-block 1e10 anchor pins no longer inflate the rejection bar ~10 orders and eject healthy marginal stations every epoch (TWTF/VOIM class). OFF = legacy max-pivot test (byte-identical).
        bool   network_emit_generation_id_{false};         ///< RTA-3: stamp a solution-GENERATION id (uint32 = first-emitted epoch's week·604800+⌊tow⌋) on every epoch (optional I record + store field + S-line gid=); a consumer treats a CHANGE as a global datum break; OFF = no I record, store generation unset (byte-identical).
        bool   network_serialize_disc_counters_{false};    ///< RTA-4: append the POST-EMIT discontinuity-counter VALUES as optional trailing fields (C field 5 clock / B field 5 code / P field 7 phase) so a tail reader reconstructs counters without replaying from epoch 1; OFF = old field counts (byte-identical).
        double network_contract_rebroadcast_s_{0.0};       ///< RTA-5: re-broadcast a CURRENT v2 'S' contract line (refreshed realized pivots) every N seconds of product time AND on any datum-change emit; requires serialize_sbasis_contract; 0 = single header S line (byte-identical).
        double network_emitter_staleness_horizon_s_{0.0};  ///< RTA-11: per-key last-emitted PRODUCT-time horizon (s); a (sat[,signal]) key returning after a longer gap is a first sighting (baseline reseed, no jump/forced disc bump) on phase/code/clock channels; travels in the S contract as stale=; 0 = keep-forever baselines (byte-identical).
        bool   network_service_mode_{false};               ///< RTB-8: curated RT-service profile switch (network.service_mode) — the loader arms the coherent producer toggle set (bundle-wins over explicit disables, each override warned; see loadNetworkSection_).  Member kept purely for PROVENANCE (getter + L15 run manifest); OFF = nothing armed (byte-identical).
        double network_clock_model_meas_std_m_{0.05};    ///< Std (m) of the network-solved offset as the two-state model's measurement (the update R).
        double network_clock_model_max_gap_s_{120.0};    ///< R11: max unseen gap (s) the carried two-state clock model survives before reseed / drift-reset / C1-ZOH; loader defaults it to network_phase_leveling_max_gap_s_ (clock-model carry only, distinct from the ambiguity/ZWD max-gap).
        // --- RT0-4: product generation-time metadata ---
        bool   network_emit_generation_time_{false}; ///< RT0-4: stamp a wall-clock time-of-generation per emitted epoch (optional G record) so a consumer can measure product latency; OFF = no G record (byte-identical). Wall-clock ⇒ non-deterministic when on.
        // --- RT0-5: per-constellation clock-runaway quarantine ---
        bool   network_clock_runaway_check_{false};  ///< RT0-5: drop a constellation from the epoch's emission when its median clock magnitude/rate exceeds the ceilings below (a runaway degrades scope, not the whole product); OFF = every constellation emitted (byte-identical).
        double network_clock_runaway_mag_m_{1000.0}; ///< RT0-5: per-constellation median |clock − global median| ceiling (m). Inert unless network_clock_runaway_check_.
        double network_clock_runaway_rate_m_s_{10.0};///< RT0-5: per-constellation median |clock drift| ceiling (m/s). Inert unless network_clock_runaway_check_ AND the two-state clock model emits a drift.
        // --- B1: URA quality channel ---
        bool   network_emit_ura_{false};            ///< B1: emit a per-sat URA quality channel (EWMA(σ̂0)×formal clock σ); OFF = product byte-identical.
        double network_ura_ewma_alpha_{0.1};        ///< B1: EWMA α for the network-wide σ̂0 that scales the emitted URA.
        bool   network_consume_ura_{false};         ///< B1: consumer inflates measurement R by the network URA on corrected rows; OFF = R byte-identical.
        double network_ura_staleness_q2_{0.0};      ///< B1: consumer ZOH clock-staleness q² (m²/s³) folded as q²·dt³/3 into the applied clock variance; 0 = off.  Loader defaults it to network_clock_model_q2_ (P1-3) so it is active whenever consume_ura is on, not silently inert.
        bool   network_ura_retro_bump_{false};      ///< B1: on a post-emit QC flag, retro-bump the flagged SATELLITE's disc counter on the NEXT emit (consumer resets the ambiguity that integrated the contaminated epoch); OFF = no bump.
        double network_ura_refloat_inflate_{1.0};   ///< B1: URA multiplier applied to a flagged sat for the phase-leveling warmup horizon after any refloat; 1 = off.
        // --- B1b: phase-bias σ channel (Tienstra product-covariance, phase-bias block) ---
        bool   network_emit_phase_bias_sigma_{false};    ///< B1b: producer emits per-(sat,signal) phase-bias formal σ (EWMA(σ̂0)×√(shared-cov diag), gauge-exact) into SsrPhaseBiasEntry.sigma_m; OFF = σ 0, byte-identical.
        bool   network_consume_phase_bias_sigma_{false}; ///< B1b: consumer inflates R on a phase row by the network phase-bias σ²; OFF = R byte-identical.
        bool   network_emit_combined_sigma_{false};      ///< M4 (contract v2): producer emits per-key COMBINED-correction σ (sigc phase, sigcc code) + code/phase corr (rho) from the shared-cov cross-terms → sigc=/sigcc=/rho= wire tokens; OFF = every value 0, no token, byte-identical.
        double network_combined_sigma_agek_{0.0};        ///< M4 (contract v2): age-scaling coefficient advertised on the S-line (agek=); consumer inflates combined variance as σc²·(1+agek·Δt_age²). 0 = no token, no age inflation (byte-identical).
        bool   network_require_signal_integer_for_ar_{false}; ///< T3a: consumer marks an ambiguity UNRESOLVABLE (AR-ineligible) when its covering network phase bias declares signal_integer=false (e.g. the anchor-free ensemble datum, whose bias references the receiver-ensemble mean and is NOT integer-preserving). OFF = the historical coverage-only gate (byte-identical); no consumer reads signal_integer today.
        double network_start_offset_hours_{0.0};        ///< Testing fast-forward: skip epochs until this many hours of data have elapsed from the first synced epoch, then cold-start (0 = process from the start).
        bool enable_cio_frame_{false};                    ///< Use the CIO IAU 2000A/2006 ECI↔ECEF transform (SOFA) for Sun/Moon; legacy equinox chain when off.
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
        bool estimate_sat_phase_biases_{false};                  ///< PPP_RTK_Network: estimate explicit per-(sat,signal) phase-bias states.
        bool estimate_sat_clocks_{false};                        ///< PPP_RTK_Network: estimate explicit per-satellite clock states (orbit held fixed).
        bool promote_sat_code_bias_{false};                      ///< PPP_RTK_Network: estimate satellite code bias on every code row (not fallback-only).
        double sat_bias_cache_warm_age_s_;                       ///< Sat-bias cache: warm-restore window (sec).  Within this, restore value + aged variance.
        double sat_bias_cache_max_age_s_;                       ///< Sat-bias cache: max entry lifetime (sec).  Beyond this, evict; in [warm, max], restore value with cold-start variance.
        double no_bia_code_downweight_;                            ///< Variance multiplier for code on signals without BIA coverage.
        double code_bias_reinflate_std_{0.5};                      ///< Std (m) the rx code-bias state is re-inflated to on a consumed sat code-bias datum discontinuity (H1).
        double clock_disc_isb_reinflate_std_{0.0};                 ///< Std (m) the non-reference-constellation ISB state is re-inflated to on a consumed sat CLOCK datum discontinuity; 0 (default) = note-only clock-disc handler (byte-identical).
        bool estimate_velocity_;                           ///< Estimate receiver velocity from Doppler observations.
        bool enable_iono_rate_{false};                     ///< PPP-RTK uncombined: estimate a per-sat slant-iono RATE (constant-velocity [I,Idot] block) driven by process_noise.iono_rate_psd.
        ResidualHistoryConfig residual_history_;                    ///< Per-(sat, signal) post-fit residual history tracker config.
        StuckCodeBiasConfig   stuck_code_bias_;                     ///< Stuck-code-bias detector + reset thresholds.
        SlipStateReinflationConfig slip_state_reinflation_;         ///< Sat-wide iono + sat-bias variance re-inflation on phase reset.
        CorrectionsReadyGateConfig corrections_ready_gate_;         ///< Real-time: defer estimation until all above-mask sats are broadcast+SSR.
        ZwdConstraintConfig    zwd_constraint_;                     ///< Soft a-priori ZWD constraint (anchors the BSSD [up,ZWD] null space).
        CycleSlipConfig        cycle_slip_;                         ///< Which cycle-slip detectors run (SlipDetectorRegistry selection).
        AidingConfig           aiding_;                             ///< Which aiding sources run (AidingSourceRegistry selection).
        AdaptiveIonoQConfig adaptive_iono_q_;                       ///< Adaptive ionospheric process-noise (GF second-difference) config.
        IonosphereWeightingConfig iono_weighting_;                  ///< GIM ionosphere-weighting (PPP-RTK soft iono constraint) config.
        GlobalVtecShConfig global_vtec_sh_;                         ///< Co-estimated global SH-VTEC ionosphere (network producer) config.

        bool has_reference_position_;                          ///< True if user supplied a reference ECEF position.
        std::array<double, 3> reference_position_ecef_; ///< User-supplied reference position ECEF (m).
        bool enable_itrf_velocity_{ false };                   ///< Propagate reference_position_ecef_ to the obs epoch via ITRF station velocity (r(t)=r0+v·dt).
        bool has_reference_velocity_{ false };                 ///< True if a reference_velocity_ecef was supplied.
        std::array<double, 3> reference_velocity_ecef_{ 0.0, 0.0, 0.0 }; ///< Reference-station ITRF velocity (m/yr, ECEF).
        double reference_position_epoch_year_{ 0.0 };          ///< Epoch (decimal year) at which reference_position_ecef_ is defined; 0 = unset.

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
        NetworkConsumerConfig network_consumer_config_;                   ///< RTA-7: processing.network_consumer profile (standalone network-product consumer; enable defaults false).
        BaselineConfig baseline_config_;                                  ///< Baseline / network processing configuration (used when isBaselineMode()).
        NetworkConfig  network_config_;                                   ///< PPP-RTK network producer configuration (used when isNetworkMode()).

        // --- Ambiguity Resolution ---
        int    ar_mode_;                  ///< AR mode: 0=float, 1=continuous, 2=instantaneous, 3=fix-and-hold.
        double ar_ratio_threshold_;     ///< LAMBDA ratio test threshold for acceptance.
        double ar_elevation_mask_;     ///< Elevation mask for AR candidates (degrees).
        double ar_hold_variance_;     ///< Pseudo-obs variance for fix-and-hold (m^2).
        int    ar_min_epochs_;           ///< Minimum filter epochs before AR is allowed (convergence guard).
        double ar_max_position_sigma_m_;  ///< Maximum 3D formal position σ for AR to fire (m).
        double ar_ratio_upper_bound_;   ///< Reject fixes whose ratio exceeds this (covariance pathology).
        bool   ar_wl_only_;           ///< WL-only AR: apply MW-resolved WL integers as Kalman pseudo-obs, skip LAMBDA NL search.
        bool   ar_ignore_mw_wl_{false}; ///< RTA-7: processing.ar_ignore_mw_wl — drop the MW WL feed into AR (pure MLAMBDA on the KF float); the previously key-less RealTimeEngine::setArIgnoreMwWl seam.
        double ar_wl_only_pseudo_var_; ///< Pseudo-obs variance (cycles²) for WL-only constraints.
        double ar_wl_max_frac_;        ///< Max worst-case fractional residual (|frac|+3σ_mean/λ, cyc) for an MW WL to enter AR.
        double ar_min_success_rate_;  ///< Min bootstrap success rate (FFR-style) for AR fix acceptance.
        double ar_nl_max_per_pair_frac_; ///< Max per-NL-pair |float − fixed| (cyc); rejects marginal fixes.
        bool   ar_enable_partial_ar_;    ///< Partial AR: on full-set fix failure, retry on the largest gate-passing subset.
        std::vector<std::string> ar_active_systems_; ///< Constellations eligible for AR (empty = all).

        LoggingConfig logging_config_;             ///< Logger and trace file enable/disable flags.
    };

} // namespace GnssProcessor
