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
         * Used by the closed-loop harness to derive a consumer config
         * (@c ProcessingMode::PPP_RTK) from the network producer config, so the
         * held-out station runs the user-side pipeline.  Not part of the normal
         * JSON-driven flow; production runs set the mode in the config file.
         *
         * @param mode New processing mode.
         */
        void setProcessingMode(ProcessingMode mode) { processing_mode_ = mode; }

        /**
         * @brief Derive a held-out consumer config from this network producer config.
         *
         * Returns a copy passed through @ref applyNetworkConsumerCouplings:
         * @c ProcessingMode::PPP_RTK (uncombined PPP-RTK user), the
         * network-forced shared-state estimation flags turned back off (the
         * consumer takes those satellite products from the injected SSR store
         * instead of estimating them), @c estimate_sat_biases coupled to the
         * clock source, and BSSD set by @c consumer_undifferenced.  The
         * consumer-side options of @c NetworkConfig::closed_loop are then
         * applied: the ambiguity-resolution mode and systems (the producer
         * itself is float), the SSR bias age gate when products are consumed
         * and the key was not set explicitly, the channel mode, and the
         * phase-bias and ambiguity-level options.  The canonical-signal rule is
         * reset to @c CanonicalSignalRule::Count and the extra-band and
         * second-channel options to their defaults.  All other settings
         * (products, atmosphere, antenna) are inherited unchanged, so the
         * consumer's correction frame matches the producer's.
         *
         * @return A PPP-RTK consumer @c GnssConfig.
         */
        GnssConfig deriveClosedLoopConsumerConfig() const;

        /**
         * @brief Shared network-consumer coupling logic (RTA-7(a)).
         *
         * The single definition of the couplings a config needs to consume
         * network products instead of producing them.  It sets the mode to
         * @c ProcessingMode::PPP_RTK; turns the producer-forced shared-state
         * estimation flags back off (@c estimate_sat_clocks /
         * @c estimate_sat_phase_biases / @c promote_sat_code_bias /
         * @c estimate_network_sat_code_bias / @c estimate_network_iono_sh and the
         * SH-VTEC block); couples
         * @c estimate_sat_biases to the clock source (@p consume_clock: the
         * network clock folds the satellite code delay, so a consumer of the
         * network clock does not re-estimate it, while a bias-only consumer on
         * the precise clock does); restores the user default
         * @c no_bia_code_downweight (100); and applies the user-model gates
         * (@p undifferenced: BSSD off, @c ar_wl_only off and the AR position-σ
         * gate capped at 0.20 m; otherwise BSSD on).  Both
         * @c deriveClosedLoopConsumerConfig (the in-process closed-loop harness)
         * and @c applyNetworkConsumerProfile (the standalone real-time consumer)
         * call it, so the two paths cannot diverge.
         *
         * @param c             Config to mutate in place.
         * @param consume_clock True when the consumer takes the network clock
         *                      (drives the @c estimate_sat_biases coupling).
         * @param consume_bias  True when the consumer takes the network
         *                      code/phase bias.  No coupling reads it at present;
         *                      it stays in the signature so bias-side couplings
         *                      are added to this one helper.
         * @param undifferenced True for the undifferenced uncombined PPP-RTK
         *                      user model, false for the legacy BSSD consumer.
         */
        static void applyNetworkConsumerCouplings(GnssConfig& c, bool consume_clock,
                                                  bool consume_bias, bool undifferenced);

        /**
         * @brief Apply the @c processing.network_consumer profile to this
         *        config in place, for the standalone real-time consumer (RTA-7(b)).
         *
         * Runs @ref applyNetworkConsumerCouplings with the profile's
         * clock/bias/undifferenced flags, then arms @c estimation.estimate_ifb
         * when the profile consumes network code biases and it is off.  A
         * standalone consumer has no producer config to inherit it from, and
         * without receiver code-bias states nothing absorbs a network code-bias
         * datum re-reference.  (The engine's one-shot A5 warning is then moot
         * but stays in place for non-profile paths.)
         *
         * It also arms @c processing.enable_ssr_bias_max_age when the profile
         * consumes biases and the key was not set explicitly, in either the
         * dotted or the legacy flat form; an explicit @c false is respected
         * (RTA-9(a)).  Without the staleness gate, bias consumption applies
         * arbitrarily stale biases across producer withhold gaps.  The rule
         * belongs to the profile, not to the shared couplings helper;
         * @ref deriveClosedLoopConsumerConfig applies the same rule to its
         * derived config.
         *
         * @c Application::runRealTime calls it before the engine is
         * constructed, so the couplings shape the state vector.  The caller
         * gates on @c getNetworkConsumerConfig().enable and on a product source
         * (@c realtime.network_products_path) being present.
         *
         * @param log_manager Logger sink for the applied-profile summary.
         */
        void applyNetworkConsumerProfile(LogManager& log_manager);

        /// The @c processing.network_consumer profile block (see
        /// @c NetworkConsumerConfig).  @c enable defaults to false (inert).
        const NetworkConsumerConfig& getNetworkConsumerConfig() const { return network_consumer_config_; }

        /// Standalone @c processing.ar_ignore_mw_wl toggle (default false):
        /// suppresses the MW wide-lane feed into ambiguity resolution so AR runs
        /// pure MLAMBDA on the Kalman float (@c RealTimeEngine::setArIgnoreMwWl).
        /// Needed whenever the float carries network satellite phase biases
        /// while the MW WL integers are in the separate .BIA frame.
        bool arIgnoreMwWl() const { return ar_ignore_mw_wl_; }

        /**
         * @brief Resolve the effective @c setArIgnoreMwWl value for the
         *        network-consumer profile.
         *
         * Mirrors the closed-loop driver, which calls
         * @c setArIgnoreMwWl(!consumer_config.getArWlOnly()) on its derived
         * config.  With the profile enabled, the value here is therefore
         * @c !getArWlOnly() evaluated after @ref applyNetworkConsumerProfile has
         * run (the undifferenced couplings force @c ar_wl_only off, which gives
         * true), ORed with the standalone @c processing.ar_ignore_mw_wl key.  An
         * explicit @c processing.network_consumer.ar_ignore_mw_wl overrides both.
         * With the profile disabled this is the standalone key.
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
         * @brief Gets the canonical-signal decision rule.
         *
         * Read once per station by @c PppModel::decideConstellationCanonicals at
         * that station's first processed epoch.  @c CanonicalSignalRule::Count
         * is the default and reproduces the latch that predates the rule.
         *
         * @return The rule selected by @c processing.canonical_signal_rule.
         */
        CanonicalSignalRule canonicalSignalRule() const { return canonical_signal_rule_; }

        /**
         * @brief Coverage floor for @c CanonicalSignalRule::Priority.
         *
         * A priority-list signal is eligible when its satellite count at the
         * decision epoch is at least this fraction of the largest count of the
         * same band and observation type.  Parsed from
         * @c processing.canonical_signal_min_coverage and clamped to [0, 1];
         * 0 accepts any tracked signal, 1 demands the maximum count.  Ignored
         * under @c CanonicalSignalRule::Count.
         *
         * @return The clamped fraction; default 0.5.
         */
        double canonicalSignalMinCoverage() const { return canonical_signal_min_coverage_; }

        /**
         * @brief Stations the canonical-signal rule is scoped to.
         *
         * Empty (the default) means the rule of @ref canonicalSignalRule applies
         * at every station.  A non-empty list scopes it: a listed station
         * decides its latch under @c CanonicalSignalRule::Priority and every
         * other station under @c CanonicalSignalRule::Count.  Names are compared
         * case-insensitively and are stored upper-cased by the loader.  Parsed
         * from @c processing.canonical_signal_priority_stations.
         *
         * @return The scoped station names; empty when the knob is unset.
         */
        const std::vector<std::string>& canonicalSignalPriorityStations() const {
            return canonical_signal_priority_stations_;
        }

        /**
         * @brief Whether the canonical code follows the phase's tracking
         *        attribute.
         *
         * True (the default) keeps the CSL-1 behaviour: under
         * @c CanonicalSignalRule::Priority a band's code pick moves onto the
         * tracking attribute its phase latched, where such a code exists at the
         * station and clears the coverage floor.  The cost of that clause: a
         * station that tracks L1C phase with C1W code has its code moved to C1C,
         * which thins the C1W code biases on the wire, and a consumer on C1W
         * code then sees code-bias dropouts (section 4).
         *
         * False decides the code by the same first-in-the-priority-list rule as
         * the phase, on the code's own list, with no reference to the phase.
         * Read only under @c CanonicalSignalRule::Priority; @c Count has no such
         * clause.  Parsed from @c processing.canonical_signal_code_follows_phase.
         *
         * @return True when the code follows the phase; default true.
         */
        bool canonicalSignalCodeFollowsPhase() const {
            return canonical_signal_code_follows_phase_;
        }

        /**
         * @brief Gets the MW slip detector frequency mode (raw, including Mirror sentinel).
         *
         * Prefer @c getResolvedMwFrequencyMode at call sites that need a
         * concrete @c FrequencyMode; this raw form is for diagnostics.
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
         * On the first sample of a fresh BIA-corrected arc, the MW value
         * should land within this distance of an integer × λ_WL.  Otherwise
         * the arc is presumed to have been initialized with a slip already in
         * it (e.g. a slip during a prior data gap that triggered the reset)
         * and an immediate ambiguity reset is forced.
         *
         * The default 0.4 cycles suits low-noise code observations (e.g.
         * P(Y) tracking, L1W/L2W).  Noisier code (e.g. C/A on L1C, ~3× the
         * P(Y) noise) often exceeds 0.4 cycles on a single sample from code
         * noise alone, which triggers spurious slip detections at every arc
         * start.  For L1C-heavy runs, 1.0–2.0 cycles suppresses the false
         * positives; the lock criterion downstream still vets the arc
         * statistically once enough samples accumulate.  The MW detector also
         * lifts this floor by 3σ of the cross-sat MAD when one is available
         * for the (constellation × signal-pair) group.
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
         * the raw per-epoch estimate is statistically noisy.  Because the MW
         * noise envelope varies slowly, each cross-sat tier is updated as an
         * exponentially-weighted moving average instead of overwritten:
         * @f$\sigma_t = \alpha\,\mathrm{MAD}_{epoch} + (1-\alpha)\,\sigma_{t-1}@f$,
         * with the first finite MAD as the bootstrap.
         *
         * Smaller @f$\alpha@f$ = heavier smoothing / slower adaptation; the
         * default 0.1 (~10-sample memory) suppresses the per-epoch jitter while
         * still tracking real noise-level drift over a session. Must lie in
         * (0, 1]; 1.0 disables smoothing (raw per-epoch MAD). Affects only the
         * warmup floor; mature arcs gate on their own per-arc MAD.
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
         * model (no yaw-input data is available to it).  With this flag
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
         * A value of 0 (the default) means "use the measurement rate",
         * i.e. any epoch spacing larger than the nominal interval is
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
         * observation, not wall-clock time, so a 3600 s limit on 1 s data
         * yields one hour (≈3600 epochs) regardless of how fast the file
         * replays or the stream arrives.  Because the span is anchored on the
         * first processed epoch, a leading decimation skip (post-processing)
         * or corrections-ready warmup defer (real-time) does not count
         * against the budget.
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
         * a lagging obs stream is not processed against fresher
         * orbit/clock/SSR corrections.  Default off (opt-in).  It assumes an
         * NTP-synced host, since the lag is measured against the system clock.
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
         * Cadence sizing (RTA-9(c)): the window must comfortably contain the
         * correction provider's update interval, or satellites flap between
         * cm-grade and broadcast-grade every cycle.  Rule of thumb: size it so
         * the producer emit cadence ≪ ssr_max_age/2 (the 30 s default suits the
         * 5-10 s IGS-RTS / network-product cadences; a 30 s-cadence provider
         * needs ≥ 60 s here).  For network products, producer withhold gaps
         * (RAIM/QC epochs that emit nothing for a satellite) can exceed the
         * cadence; @ref getSsrClockStaleHoldFactor adds margin for those
         * without loosening this base gate.
         */
        double getSsrMaxAge() const { return ssr_max_age_s_; }

        /**
         * @brief SSR clock stale-hold factor
         *        (@c processing.ssr_clock_stale_hold_factor, default 0 = off; RTA-9(b)).
         *
         * The middle rung of the clock staleness ladder.  When > 0, a
         * satellite clock whose age lies in
         * (@ref getSsrMaxAge, factor × @ref getSsrMaxAge] is still applied
         * (the existing C0 + C1·dt polynomial evaluated over the true dt)
         * instead of dropping the satellite to broadcast-grade, where
         * @c drop_broadcast_grade would remove it.  Rows that consume a held
         * clock are de-weighted through the existing URA-staleness machinery
         * in @c MeasurementModel: the ZOH term q²·dt³/3 grows with the true
         * age (q² = @ref networkUraStalenessQ2, with a documented fallback when
         * the URA channel is absent).  Beyond factor × max_age the satellite
         * falls back to broadcast-grade.  Enforced where the clock lookup
         * enforces @c ssr_max_age: @c GnssCorrections::getSatelliteClockCorrection
         * passes @c SsrCorrectionStore::clockHoldWindow(max_age, factor) into
         * the store's freshness gate.  0 (default) is byte-identical; values in
         * (0, 1] clamp to 1 (no hold).  The factor is sized against producer
         * withhold gaps; see @ref getSsrMaxAge for the cadence rule.
         */
        double getSsrClockStaleHoldFactor() const { return ssr_clock_stale_hold_factor_; }

        /**
         * @brief Code-domain band-wise SSR bias fallback
         *        (@c processing.ssr_bias_band_fallback, default false; RTA-13, opt-in).
         *
         * When true, an SSR code-bias lookup that misses on the exact RINEX obs
         * code but finds a same-band sibling with a different tracking
         * attribute (receiver tracks C5X, stream carries C5Q) consumes the
         * sibling's bias; the residual attribute DCB is absorbed by the
         * receiver code-bias states (@c estimate_ifb, armed by the
         * network-consumer profile).  Never applies to phase biases, because a
         * wrong-attribute phase bias breaks the integer property: phase stays
         * exact-match, with count-only diagnostics (see
         * @c SsrCorrectionStore::biasAttributeMismatchCounts).
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
        ///        apply gate (and the freshness-flip ambiguity reset keyed to it)
        ///        drops a bias only past @c hold_factor × @ref getSsrBiasMaxAge().
        ///        Between the two limits the last record is still applied, so a
        ///        provider whose update interval hovers near the max age produces
        ///        no model step and no reset: one flip per real outage instead of
        ///        a reset per update cycle.  A held bias is applied at full weight
        ///        (no variance penalty in the hold zone; the separate
        ///        URA/staleness channel, @c network.consume_ura keyed to
        ///        @c processing.ssr_max_age_s, is independent of this factor).
        ///        1.0 = no hold (drop exactly at max age).  Config
        ///        @c processing.ssr_bias_stale_hold_factor, default 2.0; clamped to ≥ 1.
        double getSsrBiasStaleHoldFactor() const { return ssr_bias_stale_hold_factor_; }

        /// @brief Carry the float ambiguity across a declared SSR phase-bias step
        ///        (disc-counter bump / A5 freshness flip) by the exact translation
        ///        @c amb − Δ instead of resetting it
        ///        (@c processing.ssr_phase_bias_disc_carry, default false = the
        ///        existing reset path, byte-identical).  The
        ///        closed-loop harness derives it from
        ///        @c network.closed_loop.pb_disc_carry.
        ///        See @c RealTimeEngine::carryOnPhaseBiasDisc_.
        bool ssrPhaseBiasDiscCarry() const { return ssr_phase_bias_disc_carry_; }

        /// @brief Translate the float ambiguity by @c −Δ on every change in the
        ///        applied SSR phase bias, declared or not, rather than only on a
        ///        declared one (@c processing.ssr_phase_bias_step_translate,
        ///        default false = the existing reset / declared-step carry path,
        ///        byte-identical).  A change in the applied product
        ///        carries no information about the receiver position, so the
        ///        phase model stays consistent only if the (sat, signal)
        ///        ambiguity absorbs it exactly; a move common to every tracked
        ///        satellite is absorbed uniformly and is exactly consistent.  The
        ///        closed-loop harness derives it from
        ///        @c network.closed_loop.pb_step_translate.  When armed it
        ///        replaces both the reset and the declared-step carry reactions, so a
        ///        declared step is translated exactly once.
        ///        See @c RealTimeEngine::translatePhaseBiasStep_.
        bool ssrPhaseBiasStepTranslate() const { return ssr_phase_bias_step_translate_; }

        /// @brief Answer a presence edge of a HELD phase bias like any other A5 edge
        ///        (@c processing.ssr_pb_hold_presence_flip, default false = the phase-bias-hold
        ///        dispatch, byte-identical).
        ///
        /// Under the producer's hold the hold seam claims every key whose
        /// store entry carries a generation, and the three phase-bias reactions skip a
        /// claimed key entirely, including the A5 applied-value flag.  A held bias
        /// that appears on a row the consumer was already using without it, or that
        /// leaves it, therefore steps the applied correction by the whole bias with no
        /// reaction.  Armed, a claimed key skips only the discontinuity-counter part
        /// (the hold's), and the A5 edge runs as on an unheld key: the reset path
        /// re-floats the ambiguity, the declared-step carry path carries it by the applied step,
        /// and the step-translation path translates it on the edge only.  Needs the staleness
        /// gate (@c processing.enable_ssr_bias_max_age), which the closed-loop harness
        /// arms under consumption.  The closed-loop harness derives it from
        /// @c network.closed_loop.pb_hold_presence_flip.
        /// See @c RealTimeEngine::resetOnPhaseBiasDisc.
        bool ssrPbHoldPresenceFlip() const { return ssr_pb_hold_presence_flip_; }
        /**
         * @brief Answer a declared code-level step by raising the variance along
         *        the stepped satellite's phase-blind ionosphere-ambiguity direction
         *        (@c processing.ssr_level_step_inflate, or derived from
         *        @c network.closed_loop.level_step_inflate).
         *
         * The direction (ionosphere +1, each of the satellite's phase ambiguities +alpha_f,
         * alpha_f = (f_L1 / f)^2) moves the satellite's code rows and no phase row, which
         * is where an undifferenced consumer absorbs a satellite's code-level error.
         * @return true when armed.
         */
        bool ssrLevelStepInflate() const { return ssr_level_step_inflate_; }
        /// @brief That inflation's standard deviation over the declared code step
        ///        (@c processing.ssr_level_step_inflate_k, default 1).
        /// @return the scale (positive).
        double ssrLevelStepInflateK() const { return ssr_level_step_inflate_k_; }

        /// @brief Dead band of @ref ssrPhaseBiasStepTranslate: a
        ///        step at or below this magnitude (m) is left for the filter to
        ///        absorb as noise and nothing is written to the state
        ///        (@c processing.ssr_phase_bias_step_translate_eps_m, default 0.005).
        ///        An empty declaration (a generation bump with no product motion)
        ///        falls in the dead band and therefore costs nothing.
        double getSsrPhaseBiasStepTranslateEpsM() const {
            return ssr_phase_bias_step_translate_eps_m_;
        }

        /// @brief Re-validate each float ambiguity against its own code level
        ///        and refloat the ones the code rejects
        ///        (@c processing.amb_level_check, default false = nothing is
        ///        measured and nothing is written, byte-identical).
        ///
        /// The declared-step carry and the step translation preserve an ambiguity across a product step; neither
        /// checks whether the preserved value is still correct.  This check is
        /// the re-coupling edge (invariant #7) for both: two windowed levels
        /// formed from the consumer's own corrected observations (the
        /// Melbourne-Wübbena wide-lane and the ionosphere-free combination of
        /// the pre-fit innovations, both geometry- and ionosphere-free) are
        /// compared with the float states' own WL and IF combinations, and a
        /// satellite whose code level disagrees by more than
        /// @ref getAmbLevelCheckK sigma is refloated through the existing
        /// first-sighting/reset path.  The closed-loop harness derives it from
        /// @c network.closed_loop.amb_level_check.
        /// See @c RealTimeEngine::checkAmbiguityLevel_ and @ref AmbLevelCheck.
        bool ambLevelCheck() const { return amb_level_check_; }

        /// @brief Samples per satellite before the level test may run
        ///        (@c processing.amb_level_check_window_ep, default 20 = 10 min at
        ///        30 s).
        int getAmbLevelCheckWindowEp() const { return amb_level_check_window_ep_; }

        /// @brief Level-test trip threshold, |state − level| > k · σ_level
        ///        (@c processing.amb_level_check_k, default 4.0).
        double getAmbLevelCheckK() const { return amb_level_check_k_; }

        /// @brief Action on a level-test trip: @c "act" (refloat both
        ///        of the satellite's ambiguities) or @c "observe" (count only,
        ///        nothing written).  @c processing.amb_level_check_mode, default @c "act".
        const std::string& getAmbLevelCheckMode() const { return amb_level_check_mode_; }

        /**
         * @brief obs↔SSR sync master toggle (real-time, live-SSR source only).
         *
         * When true the engine holds observation epochs newer than the
         * freshest SSR content epoch + @c getSsrMaxAge (epochs no SSR can yet
         * correct) and drains them once the SSR stream advances.  This keeps
         * the obs stream time-aligned with the corrections instead of pinned
         * to wall clock; PPP-RTK needs alignment, not freshness.  Unlike
         * @c enableObsSkipToLatest (which fast-forwards obs to the newest
         * epoch, maximising the obs-ahead-of-SSR gap), this bounds the obs
         * front by the corrections.  A stalled SSR feed is bounded by
         * @c getObsSsrSyncMaxHoldS, after which the engine processes degraded
         * (broadcast-grade) epochs, so a dead feed cannot starve the filter.
         * Default off; opt-in via @c processing.enable_obs_ssr_sync.  Inert
         * unless the run uses a live orbit/clock source
         * (@c realtimeUsesLiveOrbitClockSource).
         */
        bool enableObsSsrSync() const { return enable_obs_ssr_sync_; }

        /// @brief Max seconds to hold an obs epoch waiting for SSR before falling
        ///        back to degraded (broadcast-grade) processing: the obs↔SSR
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
         * Default is @c "CL" (code + phase only), matching what the PPP /
         * PPP-RTK / baseline estimators consume.  Add @c 'D' to retain
         * Doppler (needed for @c estimation.estimate_velocity), or @c 'S' to
         * retain SNR samples (used by the SNR stochastic model and by some
         * logging traces).
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
         * @brief Whether the RINEX 3 observation parser skips written 0.0 values.
         *
         * Per RINEX 3.0x §6 "Missing observations are written as 0.0 or blanks":
         * a written zero is a receiver's data-not-available sentinel (Trimble
         * ALLOY emits them for weak-signal tracking), not a measurement.  The
         * RINEX 2.x branch always skips them.  With this off, the 3.x branch
         * ingests them as real 0-cycle phases / 0-m ranges, which is a
         * ~2·10⁷ m pseudo-observation when the cell sits above the elevation
         * mask.  Default false keeps the 3.x path byte-identical; new configs
         * arm it via @c processing.rinex_skip_zero_values (see
         * docs/SPEC_rinex_zero_sentinel.md; promotion-ledger candidate).
         */
        bool rinexSkipZeroValues() const { return rinex_skip_zero_values_; }

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
         * receiver PCV is affected; the satellite PCV (computed in the
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
         * used and a warning is logged.  Off by default, so a missing model
         * yields a zero receiver correction.
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
         * @brief Mutable access to the model-correction enable/disable flags.
         *
         * Non-const overload used by the simulator's @c sim::EphemerisProvider to
         * isolate the satellite-only antenna term when reusing
         * @c Antenna::getAntennaCorrection: it forces receiver PCO/PCV and phase
         * wind-up off (both default true) so the returned scalar is exactly
         * @c (-sat_pco + sat_pcv), matching a run config with
         * @c enable_satellite_pco/pcv on and the receiver/wind-up flags off.  Reading
         * callers on a non-const config are unaffected (they still see the flags).
         * @return Mutable reference to the ModelCorrections struct.
         */
        ModelCorrections& getModelCorrections() { return model_corrections_; }

        /**
         * @brief Gets the stochastic (weighting) model selection.
         *
         * The selected model controls the code weighting in full; phase
         * weighting is model-specific:
         *  - @c Constant       : phase = default_var (no scaling).
         *  - @c Elevation      : phase = default_var / sin²(el).
         *  - @c SNR            : variance-additive σ² with Wieser
         *                        elevation amplification.
         *  - @c SatelliteBlock : code uses the full block + band +
         *                        elevation pipeline.  Phase uses the
         *                        same pipeline when
         *                        @c SatBlockNoiseParams::apply_to_phase
         *                        is true (the default); with it false,
         *                        phase is @c default_phase_std_m²
         *                        scaled by elevation only
         *                        (StochasticModel.cpp, computeSatBlock
         *                        versus computeElevation).
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
         * Defaults to 1.0, giving the same 1/sin²(elev) variance scaling as
         * code.  This is the project standard: every shipped production
         * config sets 1.0 explicitly, and the loader default matches it.
         *
         * Setting @c phase_elevation_power = 0.5 in the JSON selects
         * the gentler 1/sin(elev) scaling.  Rationale for that option:
         * phase noise at low elevation is dominated by carrier-loop
         * SNR (which falls slowly with elevation, not as csc²) and
         * antenna PCV miscalibration; neither follows the csc²(θ)
         * mapping that drives code multipath.  At a 10° mask, p=0.5
         * amplifies σ by 2.4× rather than 5.8×, keeping
         * standardized-residual cycle-slip detection effective at
         * low elevation.
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
         * @brief Base configs merged in via @c "extends", in application order.
         *
         * Empty for a config that declares no inheritance. The run manifest records this
         * chain (with a per-base content hash) because the manifest's own
         * @c config_hash_fnv1a64 covers only the leaf file's bytes; without the chain, two
         * leaves with identical bytes but different bases would be indistinguishable in
         * the provenance record.
         *
         * @return the merged base paths, outermost base first.
         */
        const std::vector<std::string>& configExtendsChain() const {
            return config_extends_chain_;
        }

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
         * line, and rescales a reporting copy of the position covariance
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
         * flag is true the post-fit residual covariance is also formed as
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
         * keeps runs byte-identical to the path without SIF.
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
         * system alongside the Kalman filter (N += HᵀR⁻¹H, u += HᵀR⁻¹Δz,
         * ltpl += ΔzᵀR⁻¹Δz), eliminating epoch-local parameters (the receiver
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
         * frame is byte-identical to the legacy inline computation.  When
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
         * Phase rows pin the position state tightly: once the filter has
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
         * and only fires on large jumps (e.g. cold start).
         */
        double relinearizeDposThresholdCodeOnlyM() const { return relinearize_dpos_threshold_code_only_m_; }

        /**
         * @brief Hard cap on relinearization passes per run.  -1 (default)
         *        means unlimited.
         *
         * Safety net for noisy modes (code-only single-frequency with ~5 m
         * SPP-style position jitter) where the |Δpos| threshold would fire
         * every epoch and double the per-epoch cost for the whole run.
         * After the cap is reached, relinearization is silently skipped for
         * the rest of the run.
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
         * @c Eigen::setNbThreads().  The build still links OpenMP, so threading
         * can be re-enabled at runtime by editing the config, without a rebuild.
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
         * Intended to run the between-satellite, between-epoch IF phase OMC
         * double-difference detector each epoch to catch small cycle slips
         * (1-2 IF cycles) that the MW detector misses.  No user-path code reads
         * this toggle at present, so it has no effect; the network path runs
         * the detector through @c network.cycle_slip.if_phase_td.
         *
         * @return True if the IF phase slip detector is enabled.
         */
        bool enableIfSlipDetector() const { return enable_if_slip_detector_; }

        /**
         * @brief Check if accumulated (unwrapped) phase wind-up is enabled.
         *
         * When false (default), the phase wind-up correction is computed as the
         * fractional-cycle geometric angle at each epoch, bounded to ±half cycle.
         * The float ambiguity absorbs any integer-cycle component.
         *
         * When true, the wind-up angle is accumulated across epochs using 2π
         * unwrapping so that the correction is continuous.  This is required
         * for integer ambiguity resolution (IAR) where the ambiguity must
         * remain at a constant integer value.  Accumulation chains
         * epoch-to-epoch; a gap longer than @ref phaseWindUpMaxGapS (default
         * 300 s) restarts it from the raw angle.  On the single-receiver path
         * a restart cannot bias the solution: an absent satellite's ambiguity
         * is pruned and re-seeded fresh on re-acquisition, absorbing whatever
         * wind-up value is current.  The network path with a leveling carry
         * is exposed; see @ref phaseWindUpMaxGapS.
         *
         * @return True if accumulated phase wind-up is enabled.
         */
        bool accumulatePhaseWindUp() const { return accumulate_phase_windup_; }

        /**
         * @brief Accumulator continuity horizon for phase wind-up (seconds).
         *
         * A processed-epoch gap longer than this restarts the unwrap from the
         * raw @c atan2 angle, which applies an exact integer-cycle step to every
         * signal of that satellite.  Default 300.0, the former fixed value, so
         * the default is behaviour-preserving.
         *
         * The network lineages (the goldens included) run
         * @c network.phase_leveling_max_gap_s at 900 s, three times this horizon.
         * A gap in (300, 900] s therefore steps the model while the carried
         * ambiguity and the zero-q leveling level survive and are re-injected at
         * their pre-gap values: a stiff carry that needs a wired jump response
         * (standing invariant #1).  @c estimation.trace_windup_census counts that
         * exposure, and @c network.cycle_slip.windup_restart_refloat is the wired
         * response (SPEC_phase_windup_sign §10.4).  Do not arm
         * @c accumulate_phase_windup on a network lineage without that response.
         *
         * @return Continuity horizon in seconds (> 0).
         */
        double phaseWindUpMaxGapS() const { return phase_windup_max_gap_s_; }

        /**
         * @brief Arm the read-only phase wind-up census.
         *
         * Counts, per receiver, the step in the applied wind-up between
         * consecutive processed epochs, plus the gap distribution bucketed
         * against this horizon and the leveling-carry horizon.  It can be armed
         * in either accumulation posture.  That matters for the default
         * (fractional) posture, which the field lineages run and which no other
         * instrument covers on the network path: @c windup_trace.csv is written
         * only from @c Application::run, which @c runNetwork does not reach.
         *
         * Telemetry only: arming it leaves the solve byte-identical.
         *
         * @return True if the wind-up census is armed.
         */
        bool traceWindUpCensus() const { return trace_windup_census_; }

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
         * motion, diurnal VTEC trend), so the level prior tightens considerably.
         * That decorrelates the ionosphere from the ambiguities (faster, stronger
         * AR and float position) and turns an iono disturbance (scintillation,
         * TID, storm onset) into a large in-band innovation instead of noise
         * hidden in the ramp.  Active only in PPP-RTK uncombined mode; the rate
         * state is not observed directly (H=0) and is driven only by the
         * F-coupling into I_s, like the receiver clock-rate.  Orthogonal
         * to @c adaptive_iono_process_noise (which inflates the rate PSD in
         * storms).  Default off; with it off, the iono is the classic random walk.
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
         * GLONASS uses FDMA (each satellite has its own carrier frequency),
         * so the receiver hardware delay differs per channel and does not
         * cancel in between-satellite differencing.  By default (false)
         * GLONASS is dropped at three gates: ensureRequiredStates,
         * isUsableMeasurement, and @c PivotManager pivot selection.
         *
         * When true, GLONASS observations flow through BSSD.  Per-(sat,
         * signal) sat-bias states absorb the receiver inter-channel bias
         * (ICB) through the existing @c estimate_sat_biases path, which
         * adds states whenever BIA coverage is missing (always the case
         * for GLONASS in standard products).  GLONASS phase ambiguities
         * are auto-marked unresolvable so LAMBDA never tries to
         * integer-fix the ICB-contaminated value.
         *
         * Recommended: false unless the extra GLONASS geometry is needed.
         * GPS+Galileo+BeiDou already provide 30+ satellites, more than
         * enough for a clean PPP-RTK solution.  Activation needs this flag
         * and @c estimate_sat_biases=true.
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
         * @brief Admit GLONASS to the PPP-RTK network producer (code-only scope).
         *
         * When set, GLONASS satellites contribute to the joint network solve with
         * a per-satellite shared clock column (the integer/clock product), while
         * the FDMA receiver group delay is carried by the per-FCN-slot receiver
         * IFB as a local nuisance (Schur-eliminated, never emitted).  No satellite
         * phase bias is emitted for GLONASS: a single shared per-(sat,signal) phase
         * bias is unphysical for FDMA (the receiver group-delay-vs-frequency
         * response is per-receiver/per-antenna and so not portable to consumers).
         *
         * Network mode only.  Requires uncombined PPP-RTK and the per-slot IFB
         * machinery: @c loadNetworkSection_ forces @c estimate_ifb and
         * @c enable_glonass_ifb on (without the per-slot IFB the GLONASS
         * observations would pin the receiver clock and corrupt the GPS/Galileo
         * products), and rejects the config unless GLONASS is in
         * @c active_systems with a @c processing.signal_priorities entry.
         * Default false keeps the GPS+Galileo network byte-identical.
         */
        bool enableGlonassNetwork() const { return enable_glonass_network_; }

        /**
         * @brief Estimate satellite CODE biases (OSB) in the PPP-RTK network producer.
         *
         * When on (@c network.estimate_sat_code_bias), the network promotes the
         * per-(satellite, signal) code bias to a first-class shared border column
         * (all non-GLONASS constellations: GPS, Galileo, and BeiDou @c C## raw
         * code signals; GLONASS FDMA is excluded) so the engine emits its own
         * satellite code-bias product instead of relying on an external @c .BIA.
         * It is the single switch that lands the coupled W6 slice: @c
         * loadNetworkSection_ also forces @c estimate_sat_biases / @c
         * promote_sat_code_bias / @c estimate_ifb (receiver DCB) on, and the GIM
         * ionosphere weighting on (the iono constraint that makes the satellite
         * DCB observable) unless @c network.w6_iono_constraint is @c none (then
         * forced off).  It forces @c enable_code_phase_bias and
         * @c estimate_phase_clock off, so the network estimates the full bias
         * from scratch rather than a residual to @c .BIA.  Default false keeps
         * the clock+phase-bias-only V1 network byte-identical.  See
         * @ref networkCbAdmission for the run-latched admission census that
         * gates which OSB columns become shared (vs kept as station-local
         * sponges); the census is the remedy for BeiDou sparsity.
         */
        bool estimateNetworkSatCodeBias() const { return estimate_network_sat_code_bias_; }

        /**
         * @brief Master enable for the W6 code-bias admission census and
         *        station-local quarantine ("CB-ADM").
         *
         * When on (@c network.cb_admission) and @ref estimateNetworkSatCodeBias
         * is armed, a one-shot startup census over the first
         * @ref networkCbAdmissionCensusEpochs epochs latches each (sat,signal)
         * code pair as admitted (a shared border column, as a registered pair
         * is without the census) or quarantined (measurements kept, the
         * per-station sat-bias state kept and never pruned, never registered,
         * so a station-local sponge).  This replaces the per-epoch >= 2-station
         * gate, whose count flaps at BeiDou scale (many pairs at 1-2 stations)
         * and drives prune / re-register / re-seed churn and datum-orphan
         * near-null directions that withhold whole epochs.  Latched once and
         * immutable for the run.  Default false ⇒ the legacy per-epoch path is
         * byte-identical.
         * @see network/assembly/BiasAdmission.hpp, docs/SPEC_cb_admission_card.md
         */
        bool networkCbAdmission() const { return network_cb_admission_; }

        /**
         * @brief Minimum supporting-station count for a code pair to be an
         *        admitted-candidate in the CB-ADM census (@c
         *        network.cb_admission_min_stations, default 4).
         *
         * Folded into @c effective_min = max(2, min(this, n_census_stations)):
         * 2 is the hard estimability floor, and the census-roster clamp keeps
         * small dev rosters runnable.  A station "supports" a pair when it supplies a
         * usable raw code observation in >= ceil(K_eff/2) of the census epochs.
         */
        int networkCbAdmissionMinStations() const { return network_cb_admission_min_stations_; }

        /**
         * @brief Length K of the CB-ADM census window (@c
         *        network.cb_admission_census_epochs, default 15).
         *
         * The census scans @c [i0, i0+K) of the synced-epoch vector where @c i0
         * is the first epoch the run loop processes (post @c start_offset_hours
         * fast-forward).  Clamped to the available span, so a short smoke run
         * still classifies (over K_eff <= K epochs).
         */
        int networkCbAdmissionCensusEpochs() const { return network_cb_admission_census_epochs_; }

        /**
         * @brief Co-estimate the global SH-VTEC ionosphere in the network producer.
         *
         * When on (@c network.estimate_iono_sh), the network producer estimates its
         * own global single-layer spherical-harmonic VTEC field jointly with the
         * satellite code biases, replacing the external GIM (`.INX`) as the iono
         * constraint that makes the satellite DCB observable.  It forces
         * @c network.estimate_sat_code_bias on (the W6 coupled slice: sat code bias,
         * rx DCB and the iono weighting machinery) and switches the iono row builder
         * from the external-GIM target to the co-estimated SH field.  Mutually
         * exclusive with loading an external GIM for the same purpose (no
         * double-apply).  Default false keeps the external-GIM W6 path byte-identical.
         */
        bool estimateNetworkIonoSh() const { return estimate_network_iono_sh_; }

        /**
         * @brief Gauge the receiver code-bias set with one @f$\sum_r d_{r,c}=0@f$
         *        per constellation (@c network.rx_code_bias_datum).
         *
         * The receiver-side counterpart of the W6 per-(constellation,signal)
         * satellite code-bias sum-to-zero, and the standard IGS
         * zero-mean-over-receivers DCB convention.  Needed whenever the
         * satellite half of that gauge is absent (no W6 satellite code-bias
         * columns; it was built for the retired external-.BIA posture, EXT-CB),
         * leaving the receiver-DCB common mode confounded with the global
         * ionosphere level.
         *
         * Realized before the reduction as a diagonal prior
         * @f$d_r\sim N(0,\sigma^2)@f$ on every station's receiver DCB
         * (σ = @ref networkRxCodeBiasDatumSigmaM), through the same prior
         * channel the Kaula prior uses for satellite OSBs.  The between-station
         * differences are estimable, so the prior does not move them; the
         * common mode has almost no data information, so the prior sets it to
         * zero mean over receivers.  It is not a post-reduce cross-station
         * @ref NetworkNormalEquations::SumZeroGauge, because
         * @c NetworkEstimator::solve does not compose a post-reduce Woodbury
         * gauge with the DD priors.  Requires @c estimation.estimate_ifb:
         * without an explicit receiver-DCB state there is nothing to gauge and
         * the toggle is inert (warned).  Default false ⇒ byte-identical.
         *
         * @see networkRxCodeBiasDatumSigmaM
         */
        bool networkRxCodeBiasDatum() const { return network_rx_code_bias_datum_; }

        /**
         * @brief Loose physical-bound sigma (m) for the receiver code-bias datum
         *        (@c network.rx_code_bias_datum_sigma_m, default 10.0).
         *
         * The datum is a diagonal @f$d_r\sim N(0,\sigma^2)@f$ prior on every
         * station's receiver DCB, so @f$\sigma@f$ must be loose against the real
         * per-station DCB spread (tens of ns, i.e. metres): it is a bound, not a
         * measurement.  The realized common-mode information is @f$1/\sigma^2@f$,
         * independent of the station count, so it must still clear the per-epoch
         * rank-gate cutoff (@f$\approx 10^{-9}\lambda_{\max}@f$): 10 m gives 0.01,
         * about 45× the cutoff measured on a 63-station EXT-CB arm.  Same
         * loose-physical-bound role as @ref networkCbKaulaSigmaM on the satellite side.
         */
        double networkRxCodeBiasDatumSigmaM() const { return network_rx_code_bias_datum_sigma_m_; }

        /**
         * @brief Pre-seed a cold receiver clock from GPS code before the first
         *        network ambiguity seed (@c network.cold_clock_seed).
         *
         * On the PPP-RTK network path nothing pre-solves the per-station
         * receiver clock before ambiguities are first seeded:
         * @c StateManager::syncPppStateToStateVector (the PPP/user-path writer of
         * @c x(clockIndex) from a code clock) is not called from
         * @c ApplicationNetwork, so the clock state is 0 at the first epoch.
         * @c StateManager::computeAmbiguitySeed subtracts @c clk_term=x(clock)=0,
         * so a free-running receiver's entire clock offset (e.g. 0.856 ms →
         * 2.6e5 m) enters every cold ambiguity seed uniformly: the cold-start clock
         * leak into the ambiguity seeds.
         *
         * When on, @c MeasurementModel::ensureRequiredStates calls
         * @c StateInitializer::seedColdReceiverClock once per station (before the
         * ambiguity-seed loop, after the iono seed) to write the median GPS code
         * residual (the GPS-referenced receiver clock) into @c x(clockIndex);
         * the existing @c clk_term subtraction then removes it from every seed.
         * It fires only while the clock is cold (state == 0), so it never
         * overwrites the PPP path's already-synced clock.
         *
         * Default false: a run with it off never calls the seeder and is
         * byte-identical.  Enabling it changes bytes for every fixture whose
         * receiver clock ≠ 0, so byte-identity is guaranteed by the toggle,
         * not by a threshold.
         */
        bool networkColdClockSeed() const { return network_cold_clock_seed_; }

        /**
         * @brief A-priori satellite orbit/clock source for the network producer
         *        (@c network.apriori_source).
         *
         * @c Sp3 (default) resolves precise SP3(+CLK) files; the emitted clock
         * C0 is a delta against the SP3/CLK product (the default behaviour,
         * byte-identical everywhere).  @c BrdcRts references the a-priori clock to
         * the broadcast ephemeris (@c LiveEphemerisStore) plus external IGS-SSR
         * (4076) orbit corrections; the estimable clock state then absorbs the
         * broadcast-clock error and the emitted C0 becomes a delta against
         * broadcast(IODE) + the applied RTS orbit (the self-sufficient real-time
         * service mode; see @c docs/rt_service_conventions.md §1).  The choice is
         * orthogonal to @c network.service_mode (an input-source selection, not a
         * bundled toggle); @c service_mode does not arm it.  Read by the network
         * producer setup (RTF-1).
         */
        NetworkAprioriSource networkAprioriSource() const { return network_apriori_source_; }

        /**
         * @brief The native broadcast clock a priori is armed:
         *        @c network.apriori_source @c sp3_brdc_clock.
         *
         * SP3 orbits, the broadcast clock of the navigation file selected by
         * @ref networkBrdcSelect, no CLK product.  The producer's a priori is
         * continuous across cut-overs (a per-ephemeris offset table) and its wire
         * refers to the stamped ephemeris.  Fingerprinted; fresh convergence only.
         *
         * @return True under @c sp3_brdc_clock.
         */
        bool networkAprioriBrdcClock() const {
            return network_apriori_source_ == NetworkAprioriSource::Sp3BrdcClock;
        }

        /**
         * @brief Ephemeris selection rule of the native broadcast clock a priori
         *        (@c network.brdc_select).  Read only under
         *        @c sp3_brdc_clock; @c ttom is the only rule.
         *
         * @return The rule.
         */
        NetworkBrdcSelect networkBrdcSelect() const { return network_brdc_select_; }

        /**
         * @brief A-priori satellite-clock anchor pseudo-observation σ (m) for the
         *        network producer (@c network.apriori_clock_anchor_sigma_m).
         *
         * The per-satellite a-priori clock is bound to the solve by a loose anchor
         * pseudo-observation whose information is @f$1/\sigma^2@f$.  @c 0.0
         * (default) means auto: the mode-appropriate value is resolved at the
         * producer (unchanged in @c Sp3 mode; broadcast-grade ~3 m in @c BrdcRts,
         * per @c docs/rt_service_conventions.md §6 D6).  An explicit positive value
         * overrides the auto in both modes.  Parsed and stored here; the producer
         * reads it through @ref resolveNetworkAprioriClockAnchorSigmaM (RTF-1).
         */
        double networkAprioriClockAnchorSigmaM() const { return network_apriori_clock_anchor_sigma_m_; }

        /**
         * @brief The resolved a-priori satellite-clock anchor σ (m) that the
         *        network producer folds, given the mode and the auto policy
         *        (plan D6).
         *
         * Resolution (the single definition: the driver's anchor fold and its B7
         * information-fraction diagnostic both call this, so they cannot diverge):
         *   1. explicit @c network.apriori_clock_anchor_sigma_m > 0 → that value,
         *      in both modes;
         *   2. else @c BrdcRts (auto) → @c 3.0 m (broadcast-grade; conventions §1),
         *      which binds the anchor because the driver's < 100 m gate then passes;
         *   3. else @c Sp3 (auto) → @c initial_std.sat_clock, unchanged (the default
         *      1000 m leaves the anchor off at the < 100 m gate, so every existing
         *      sp3 network config is byte-identical).
         *
         * This does not apply the < 100 m binding gate (the driver keeps it) and
         * does not modify @c initial_std.sat_clock (the mode-independent loose
         * state init).  Pure function of config members, so it is unit-testable
         * in isolation.
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
         * Schur reduction), so the network carries carrier-phase precision, not
         * only code precision, into the satellite clock / phase-bias / code-bias
         * products.  The carry is reduction-orthogonal to the shared border and to
         * the datum gauge, so it sharpens the locals without touching the
         * minimum-constraint S-basis.  Default false keeps the per-epoch absolute
         * snapshot solve byte-identical.
         */
        bool networkPhaseLeveling() const { return network_phase_leveling_; }

        /**
         * @brief Process noise (m²/s) inflating a carried ambiguity's variance over
         *        the inter-epoch gap (@c network.phase_leveling_q_amb).
         *
         * @c var_eff = var + q_amb·Δt_since_last_seen.  Must be @c >= 0.
         *
         * The physical process model for a carrier-phase ambiguity is a random
         * constant with discrete cycle-slip resets, i.e. zero process noise
         * between slips.  With @c q_amb = 0 (the default) the carried ambiguity
         * therefore accumulates carrier-phase precision without limit, which is
         * the purpose of phase-leveling.  Continuity is enforced by the IF-phase /
         * MW-GF slip detectors (which refloat a slipped arc) and long gaps by
         * @ref networkPhaseLevelingMaxGapS (which drops and re-acquires), not by
         * continuous dilation.  A small positive @c q_amb serves only as an
         * optional soft gap-reacquisition knob (it weakly re-floats an ambiguity
         * across a gap without dropping it); it also caps steady-state
         * accumulation at @c ~q_amb·Δt and so gives up part of the precision
         * gain.
         */
        double networkPhaseLevelingQAmb() const { return network_phase_leveling_q_amb_; }

        /**
         * @brief Emitted-epoch count before a carried ambiguity is trusted
         *        (@c network.phase_leveling_warmup_epochs).
         *
         * Cold-start epochs are ill-conditioned (the clock/SH field has not
         * converged); carrying an ambiguity before then would freeze a contaminated
         * value.  No carry is stashed or injected until this many product epochs
         * have been emitted.
         */
        int networkPhaseLevelingWarmupEpochs() const { return network_phase_leveling_warmup_epochs_; }

        /**
         * @brief Maximum gap (s) a carried ambiguity survives unseen before it is
         *        dropped (@c network.phase_leveling_max_gap_s).
         *
         * Bounds the carry container and prevents re-injecting a stale value when a
         * satellite returns after a long outage, where a slip is likely and may go
         * undetected.  A housekeeping drop, distinct from the slip-driven refloat
         * and drop responses.
         */
        double networkPhaseLevelingMaxGapS() const { return network_phase_leveling_max_gap_s_; }

        /**
         * @brief Floor (m²) on a carried ambiguity's effective prior variance, a
         *        lock-in guard (@c network.phase_leveling_var_floor_m2).
         *
         * The §3.7 fading-memory guard is shared with the slip tiers (T0/T1/T2): a
         * sub-cycle or multipath bias that never trips a slip detector would
         * otherwise see its carried variance shrink monotonically and freeze a
         * contaminated value into the products.  This scalar caps the carried
         * information (@c var_eff ≥ floor ⇒ @c Λ ≤ 1/floor) so fresh data can
         * always pull a slowly biasing ambiguity back.  Default 0 disables it
         * (no cap, behaviour unchanged); it is an opt-in for longer runs.
         */
        double networkPhaseLevelingVarFloorM2() const { return network_phase_leveling_var_floor_m2_; }

        /**
         * @brief Station-wide jointly whitened local block (F3) on the
         *        phase-leveling carry inject (@c network.phase_leveling_local_block,
         *        default off).
         *
         * F1 (the retired per-LOS variance scale, @c network.phase_leveling_los_scale)
         * was exact along each LOS's iono direction but left the station-common
         * directions (receiver clock, ZWD, receiver DCB) over-counted ~n_LOS×, because
         * every carried ambiguity row of a station asserts the same station-common
         * datum again (on the quiet sim bed, a ZWD formal σ of 0.005 m against 0.35 m
         * actual at hour 2).
         *
         * Armed, each station's injectable carried ambiguity columns c_1..c_n are
         * carried as one block.  The harvest stashes, beside each column's pure
         * conditional variance (N_rr⁻¹)_cc, the conditional cross covariances
         * (N_rr⁻¹)_{c_i c_j} from the same per-column solve (no extra solve).  The
         * inject assembles Σ over the columns admitted this epoch under the
         * unchanged predicates (departed / stale / freed / reseed / pinned columns
         * are marginalized out of Σ, never conditioned on), adds q·gap and the var
         * floor on the diagonal only, factors Σ = LLᵀ (LLT with a ridge retry; on
         * failure it falls back to the per-column rows and counts it), and emits n
         * unit-weight rows L⁻¹·[(value_abs − x_lin) + Σ_j M_j ŝ_prev,j], with A_r = L⁻¹
         * over the block's local columns and A_s = L⁻¹·(per-column regressor rows).
         *
         * That is exact for every local-local direction given the border (the per-LOS
         * iono direction and the station-common ones alike).  The harvest re-stashes
         * the whole block each epoch from the posterior, so Σ does not drift.
         *
         * Requires @c network.carry_conditional_coupling (the rows live in A_s/A_r);
         * otherwise it is inert with a loader warning.  It is also inert
         * under carry_invariant.  Costs one n×n LLT per station-epoch (n ≈ 75).
         * The run-end 'Network (leveling local-block F3)' line is the on-path
         * telemetry.  Off = byte-identical.
         *
         * Whitening makes the lane directions (band differences of one LOS)
         * phase-tight across epochs, so an undetected single-band slip would
         * propagate into its sibling bands' priors (invariants #1 and #7).  The
         * covering response is the band-scoped free-epoch latch, which
         * marginalizes the detector-marked column out of the block that epoch,
         * plus the RAIM pool refloat, which re-whitens a refloated column's block
         * over the survivors.  The run-end marginalized-column and refloat
         * re-whiten counters show whether that response acted; the absence of a
         * warning does not.
         */
        bool networkPhaseLevelingLocalBlock() const { return network_phase_leveling_local_block_; }

        /// @brief Floor (m²) on the D2 satellite phase-bias carried prior variance
        ///        (@c network.shared_carry_var_floor_pb_m2, B4).  Caps the carry's
        ///        information so a stiff zero-q phase-bias carry cannot lock a biased
        ///        product in, while preserving the FCB time-averaging; 0 = off.
        double networkSharedCarryVarFloorPbM2() const { return network_shared_carry_var_floor_pb_m2_; }
        /// @brief Floor (m²) on the D2 satellite code-bias carried prior variance
        ///        (@c network.shared_carry_var_floor_cb_m2, B4); 0 = off.
        double networkSharedCarryVarFloorCbM2() const { return network_shared_carry_var_floor_cb_m2_; }

        /**
         * @brief Whether the D2 shared-parameter carry keeps the full dense
         *        cross-covariance of the shared border instead of per-key
         *        variances only (@c network.shared_carry_full_cov, C1).
         *
         * Legacy (off): each carried shared parameter (satellite clock in
         * scalar-carry mode, phase bias, code bias) is re-anchored next epoch by
         * an independent diagonal prior @c Λ_k = 1/(var_k + q·dt); every
         * cross-covariance the solve produced is discarded at the stash.  For
         * stiff carries (zero-q phase biases) that diagonal approximation is
         * statistically inconsistent: information accumulates against a
         * correlated error floor, the formal variance collapses, and a biased
         * value can lock in (the mechanism the post-fit RAIM / clock-monitor
         * tiers compensate for).  The strongest discarded terms are the
         * clock↔phase-bias off-diagonals of each satellite.
         *
         * When on, the stash keeps the dense posterior covariance block over the
         * carried keys (@ref NetworkEstimator::sharedCovariance, identity-keyed
         * so it survives layout changes) and the next epoch folds
         * @c Λ = (P_prev + Q)⁻¹ as one dense information block
         * (@ref NetworkEstimator::addSharedPriorDense), so the epoch loop becomes
         * an information-form Kalman filter on the shared border.  Departed,
         * datum-pinned, handover-retired, and clock-model-carried keys are
         * marginalized (rows/cols of P dropped before inversion); per-key
         * process noise, clock-monitor soft reinits (decorrelated), and the B4
         * variance floors apply to P's diagonal exactly as they apply to the
         * scalar path.  The SH-VTEC coefficients keep their own diagonal
         * pre-gate fold (separate physics), and under @ref networkClockModel the
         * satellite clock stays on the two-state oscillator carry, outside the
         * dense block.  A numerically non-invertible carried block degrades to
         * the legacy diagonal fold for that epoch (warned + event-logged), and a
         * warm resume seeds diagonally from the v1 snapshot for one epoch.  The
         * toggle is therefore deliberately not part of the warm-resume config
         * fingerprint (both modes carry the same per-key values and variances).
         * Default false = the legacy diagonal carry (byte-identical).
         *
         * @see networkSharedCarryVarFloorPbM2, networkClockMonitorReinitSigmaM,
         *      NetworkEstimator::addSharedPriorDense
         */
        bool networkSharedCarryFullCov() const { return network_shared_carry_full_cov_; }

        /**
         * @brief Carry each persistent local parameter (phase-leveling ambiguity,
         *        ZWD) across epochs as a conditional pseudo-observation row coupling
         *        it to the shared border, instead of an independent diagonal prior
         *        (@c network.carry_conditional_coupling).
         *
         * The legacy per-station carry (@ref networkPhaseLeveling /
         * @ref networkCarryZwd) re-injects each local's absolute value with its
         * marginal variance as a diagonal prior on the local column alone
         * (@c NetworkNormalEquations::addStationPrior).  That marginal already
         * encodes the shared border (through the Schur reduction), so re-asserting
         * it while the dense border carry also re-asserts the border double-counts
         * the shared information; compounded over epochs, the formal variance
         * collapses (masked only by the @c shared_carry_var_floor_* floors) and a
         * biased value can lock in.
         *
         * When on, the local carry becomes one pseudo-observation row spanning
         * @f$(x_s\mid x_r)@f$: coefficient @c +1 on the local column, the posterior
         * regression @f$M_r=N_{sr}N_{rr}^{-1}e_r@f$ (restricted to the free,
         * non-datum-pinned shared columns) on the border, and measurement variance
         * the pure conditional variance @f$(N_{rr}^{-1})_{rr}@f$ (+ the same
         * per-column process noise).  Departed shared columns are marginalized into
         * the row variance (@f$+M_{rj}^2 P_{\text{prev},jj}@f$).  With the local
         * information flowing conditionally and the border information carried only
         * by the dense border carry, there is no double count by construction: the
         * sequential recursion equals the batch solve.
         *
         * Requires @ref networkSharedCarryFullCov (a hard error at load otherwise):
         * the conditional design is exact only against a dense border carry.  The
         * plain @c addStationPrior path is not executed for the carried columns when
         * this is armed.  Default false = the legacy diagonal carry (byte-identical).
         *
         * @see networkSharedCarryFullCov, networkPhaseLeveling, networkCarryZwd,
         *      NetworkEstimator::stationRegression
         */
        bool networkCarryConditionalCoupling() const { return network_carry_conditional_coupling_; }

        /**
         * @brief Apply the shared-border time update to each conditional coupling
         *        row at injection (@c network.carry_coupling_time_update).
         *
         * A coupling row harvested at epoch @f$k@f$ is an exact pseudo-observation
         * of @f$u = r + M_r^\top s@f$ at the harvest epoch
         * (@f$\mathrm{Var}(u\mid D_k)@f$ = the pure conditional variance, with
         * @f$u \perp s@f$ by construction of the regression).  Between epochs the
         * border moves, @f$s^+ = s + w@f$, @f$w \sim N(0,Q)@f$ (the same
         * @f$Q@f$ the dense border carry applies), so re-expressing the row
         * against @f$s^+@f$ requires the per-column shrinkage
         * @f$M' = (P+Q)^{-1}P\,M_r@f$ and the variance inflation
         * @f$+\,M_r^\top(P^{-1}+Q^{-1})^{-1}M_r@f$.  Without this knob neither is
         * applied to columns present in the layout (the departed-column
         * marginalization is the @f$q\to\infty@f$ limit of this rule), so every
         * row over-asserts by @f$\approx M^2 q\,\Delta t@f$ at full weight;
         * re-injected each epoch, that produces the carried-DD random walk and
         * flat-σ signature (docs/SPEC_card1_coupling_carry.md).
         *
         * When on, each present coupled column @f$j@f$ of every coupling row is
         * shrunk by the scalar @f$\kappa_j = P_{\text{prev},jj}/(P_{\text{prev},jj}
         * + q_j\,\Delta t)@f$ (coefficient and its RHS term) and the row variance
         * gains @f$M_{rj}^2\,\kappa_j\,q_j\,\Delta t@f$, with the per-kind
         * @f$q_j@f$ ladder equal to the dense fold's (clock / code-bias /
         * phase-bias process noise; SH coefficients use the per-degree
         * @f$q/n^2@f$).  A clock-monitor reinit-flagged satellite's clock is
         * treated as @f$q=\infty@f$ (full marginalization, mirroring the fold's
         * decorrelation).  The scalar per-column form is exact when the border
         * covariance is diagonal across a row's coupled columns and consistent
         * (never overconfident) otherwise; it is conservative while border
         * cross-covariances are strong.
         *
         * Requires @ref networkCarryConditionalCoupling (a hard error at load
         * otherwise).  Default false = the raw harvested rows (byte-identical).
         *
         * @see networkCarryConditionalCoupling, networkSharedCarryFullCov
         */
        bool networkCarryCouplingTimeUpdate() const { return network_carry_coupling_time_update_; }

        /**
         * @brief Replace the scalar kappa of @ref networkCarryCouplingTimeUpdate by the
         *        exact conditional time update (@c network.carry_coupling_exact_update,
         *        docs/refound/CTU_coupling_time_update_2026-09-28.md).
         *
         * Each coupling row harvested at the stash the dense border block belongs to is
         * re-expressed on the moved border by the one rule
         * @f$M_{new,S}=(P_{SS}+Q_S)^{-1}P_{S,K}M_K@f$, with the variance
         * @f$M_K^\top[P_{KK}-P_{K,S}(P_{SS}+Q_S)^{-1}P_{S,K}]M_K@f$ added to the row
         * (the per-row form: the same bracket between two rows of a station is
         * @ref networkCarryCouplingExactCross, off by default).  @f$P@f$ is the stashed
         * dense block (@c prev_carry_cov),
         * @f$Q_S@f$ the inflation the D2 fold will apply to each key this epoch (read
         * from a side-effect-free mirror of the fold, checked against the fold every
         * epoch), @f$S@f$ the keys the fold carries with a finite inflation and @f$D@f$
         * the rest, whose coefficients are re-attributed through @f$P@f$.  The scalar
         * kappa at the legacy clock rate is the root of the frozen satellite clock
         * level; this rule keeps each row's co-movement with the
         * level.  The pinned clock pivot takes each row's clock-family deficit, so a
         * row keeps its identity along the declared clock gauge without the gauge-projection
         * restore.  Rows harvested earlier than the stash, terms with no dense row and
         * SH terms keep the scalar-κ treatment (counted).
         *
         * Requires @ref networkCarryCouplingTimeUpdate and
         * @ref networkSharedCarryFullCov (hard errors at load).  Fingerprinted.
         * Default false = the scalar-κ rows (byte-identical).
         */
        bool networkCarryCouplingExactUpdate() const { return network_carry_coupling_exact_update_; }

        /**
         * @brief Carry the satellite clocks' process noise in the pivot frame
         *        (@c network.clock_pivot_frame_q,
         *        docs/refound/PFQ_pivot_frame_process_noise_2026-10-08.md).
         *
         * The clock datum pins one satellite clock per constellation, so a carried clock
         * is @f$c_j - c_p@f$ and its state equation is the S-transform of the absolute one
         * (Snow, *Adjustment Computations*, 15.9 with chapter 18).  Each folding satellite
         * clock other than the pivot takes the pivot's walk over its own interval,
         * @f$r_p\,\Delta t_j@f$, on top of its own increment, and two of them share
         * @f$r_p \min(\Delta t_j, \Delta t_k)@f$ in the dense fold and in the exact
         * coupling update's @f$Q_S@f$.  The pivot is the one the stashed posterior was
         * pinned on; it is not carried, so @f$r_p@f$ is the model's physical per-satellite
         * rate (the anchor's q with the anchor armed, the legacy rate without it;
         * network/carry/PivotFrameQ.hpp).
         *
         * Requires @ref networkSharedCarryFullCov (the pairwise half needs the dense
         * fold), and refuses the anchor-free clock datums (sum-to-zero, broadcast-anchored)
         * and @c network.clock_model, which have no pivot or no carried clock (hard errors
         * at load).  Fingerprinted.  Default false (byte-identical).
         */
        bool networkClockPivotFrameQ() const { return network_clock_pivot_frame_q_; }

        /**
         * @brief Add the exact time update's covariance between two carried rows of one
         *        station to the station's whitened local block
         *        (@c network.carry_coupling_exact_cross,
         *        docs/refound/WSX_within_station_cross_terms_2026-09-30.md).
         *
         * The exact update (@ref networkCarryCouplingExactUpdate) adds each row's own
         * inflation @f$M_i^\top B M_i@f$ and drops the bracket between rows,
         * @f$M_i^\top B M_k@f$ with @f$B=P-P_{K,S}(P_{SS}+Q_S)^{-1}P_{S,K}@f$.  With this
         * knob, every pair of rows of a station's local block that both took the exact
         * route gets that term in @f$\Sigma_{ik}@f$ (@ref couplingExactCross), after the
         * harvested conditional covariances; a row on the scalar-κ route adds none.  The
         * cross-station half of the same covariance stays out.  Inert without the
         * whitened local block (counted at the run end).
         *
         * Requires @ref networkCarryCouplingExactUpdate (hard error at load).
         * Fingerprinted.  Default false = the per-row form (byte-identical).
         */
        bool networkCarryCouplingExactCross() const { return network_carry_coupling_exact_cross_; }

        /**
         * @brief Carry the previous epoch's satellite clocks as one-epoch clone columns
         *        in the shared border (@c network.carry_coupling_clock_clone,
         *        docs/refound/SCL_self_contained_clock_level_2026-10-03.md, section 5).
         *
         * The exact update (@ref networkCarryCouplingExactUpdate) re-expresses each
         * station's carried row on the moved border separately and drops the time-update
         * covariance between rows, @f$M_a^\top B M_b@f$.  With per-satellite phase anchors
         * and a clock walk at the broadcast grade (anchor q about 5e-6 m^2/s) that states
         * the satellite clock level several times too tight in variance and locks it.
         * With this knob each satellite clock that has a row in the
         * stashed dense block gets a clone column @f$c_s(t-1)@f$ for one epoch, registered
         * after every other column: the rows keep their harvested clock coefficients on the
         * clone (a second copy of the exact-update epoch with each cloned clock on its clone at
         * Q = 0; the smoothed hold's rows advance on the same construction over the clones that
         * stood in, and marginalize them after the solve), the border prior is
         * folded over the stand-ins (each cloned clock stood in for by its clone at the
         * stashed variance) with the walk @f$c_s(t)-c_s(t-1)@f$ added once per clock at the
         * fold's own variance @f$Q_s = \mathrm{var}_{\mathrm{eff}} - P_{ss}@f$, the rank
         * gate and the S-basis completion judge the border with the clones marginalized
         * (an idle clone dropped), and the harvest marginalizes the clones out of every
         * carried row through the posterior regression (the per-station form: the
         * cross-station half of the clone's conditional covariance is dropped, the
         * second-order term).  A clone is never stashed, snapshotted or emitted.  Run-end
         * telemetry and an INERT warning in the exact-update block.
         *
         * Requires @ref networkCarryCouplingExactUpdate and
         * @ref networkSharedCarryFullCov; refuses @c network.clock_model (no clock in the
         * carried border) and @c network.clock_anchor_gauge_free (the gauge-free
         * downdate's clock direction would have to include the clones); hard errors at
         * load.  Fingerprinted (armed-only token).  Default false = the exact update's per-row
         * form (byte-identical).
         * UNVALIDATED: built for the broadcast-grade clock walk; not recommended.
         */
        bool networkCarryCouplingClockClone() const { return network_carry_coupling_clock_clone_; }

        /**
         * @brief Carry the clock clones' shared conditional as history columns
         *        (@c network.carry_coupling_shared_history,
         *        docs/refound/SCL2_level_residue_under_the_clone_2026-10-06.md).
         *
         * The clock-clone harvest marginalizes the clones out of every carried row one station at
         * a time and drops the cross-station half of their conditional covariance; the exact
         * recursion accumulates those pieces into a cross-station correlation of the carried
         * ambiguities, so the per-station form freezes the satellite clock level at long
         * horizons while its statement keeps shrinking.  With this knob
         * the harvest regresses each row's terms on Y (the clones and the previous history)
         * onto the free border, and at the harvest's end compresses the rows' shared term
         * @f$R\,\Sigma_{Y\mid s}R^\top@f$ to its leading directions across stations, at most
         * @ref networkCarryCouplingSharedHistoryRank per cloned clock: each row keeps its
         * coefficients on those directions, the next epoch registers them as border columns
         * after the clones (key @c "HIST|j", prior N(0, 1) independent of the border, read by
         * the gate view as nuisance, never emitted, stashed or snapshotted), and the rows on
         * the exact route emit them.  The remainder is added per row and within a
         * station; its cross-station part is dropped and counted.  Exact when the shared
         * term's rank is within the cap.  A row of an older compression marginalizes its
         * history terms; the snapshot writes each row's marginal variance.
         *
         * Requires @ref networkCarryCouplingClockClone (hard error at load).  Fingerprinted
         * (armed-only token with the rank); fresh convergence only.  Default false = the
         * clock-clone per-station harvest (byte-identical).  UNVALIDATED.
         */
        bool networkCarryCouplingSharedHistory() const { return network_carry_coupling_shared_history_; }

        /**
         * @brief History columns kept per cloned clock by
         *        @ref networkCarryCouplingSharedHistory
         *        (@c network.carry_coupling_shared_history_rank).
         *
         * The compression keeps at most this many directions times the clones registered in
         * the epoch.  Default 1, gate U2's decision; range [1, 8],
         * checked at load whether or not the knob is armed.
         */
        int networkCarryCouplingSharedHistoryRank() const { return network_carry_coupling_shared_history_rank_; }

        /**
         * @brief Carry the satellite phase and code biases on one-epoch clones like the
         *        clocks (@c network.carry_coupling_bias_clone,
         *        docs/refound/BCL_previous_border_on_clones_2026-10-08.md).
         *
         * The clock clone keeps the clocks' walk shared between the coupling rows; the
         * biases stay on the exact per-row update, which drops their walk's covariance
         * between rows.  With this knob each phase and code bias with a row in the stashed
         * dense block gets a clone column (@c NetworkEstimator::registerClone), the rows keep
         * their harvested bias coefficients on it (a bias in S whose walk passes the clock clone's
         * screen, not a seam key), the stashed block folds over the stand-ins and each bias
         * walk folds once, in the clones' joint walk factor, and the smoothed hold's rows advance on
         * the stood-in bias clones.  The harvest, the gate view and the shared history
         * treat them as the clock clones (a bias clone takes no part in the clock-family
         * deficit).
         *
         * Requires @ref networkCarryCouplingClockClone (hard error at load).  Fingerprinted
         * (armed-only token).  Default false = the biases on the exact per-row update
         * (byte-identical).  UNVALIDATED.
         */
        bool networkCarryCouplingBiasClone() const { return network_carry_coupling_bias_clone_; }

        /**
         * @brief Carry every stashed key the exact update would re-attribute on a one-epoch clone
         *        (@c network.carry_coupling_departed_clone,
         *        docs/refound/BCL_previous_border_on_clones_2026-10-08.md).
         *
         * The exact update re-attributes a row's terms on a key the fold does not carry (departed,
         * reinit, a re-referenced phase-bias frame, skipped) through the stashed block, one
         * row at a time, and adds the conditional variance of the departed part to each row
         * on its own.  With this knob such a key, when it has a carried entry and a
         * registered clone (a clock through the clock clone's key, a bias through
         * @c NetworkEstimator::registerClone), moves into the rows' S on its clone at Q = 0,
         * and the fold gives the clone the stashed block's row and column as a time t-1 state
         * with no walk factor.  A clone moved in the rows whose key then does not fold as
         * decided takes the same prior instead of being left free.  A joint fold that does
         * not factor with these clones is retried without them (counted).
         *
         * Requires @ref networkCarryCouplingClockClone (hard error at load).  Fingerprinted
         * (armed-only token).  Default false = the exact update's re-attribution (byte-identical).
         * UNVALIDATED.
         */
        bool networkCarryCouplingDepartedClone() const { return network_carry_coupling_departed_clone_; }

        /**
         * @brief Hold a truncated coupling row's per-(constellation,kind) coefficient
         *        sum at its harvest value (`network.carry_coupling_gauge_project`).
         *
         * A harvested coupling row is gauge-invariant through the regression identity
         * @f$M^{\mathsf T}g_s=-g_r(c)@f$: it observes each datum gauge exactly as much as
         * its own @f$A_r=+1@f$ local requires.  Truncation breaks that: a departed column
         * is marginalized by deleting its coefficient, leaving
         * @f$\Sigma_{\text{family}}M=-M_{dep}@f$, so the row becomes a high-weight
         * observation of the current gauge pinned at the old realization.  Seven clock
         * departures in one epoch of a 24-station sim lifted the smallest null-space
         * eigenvalue from 1e-11 to 9.43e-3, five decades above the rank-gate cutoff,
         * which produced spurious over-specified withholds that cleared only when the
         * carry expired at `pl_max_gap_s` (a silent mass cold restart).
         *
         * Armed, the deficit that the re-projection of @ref networkCarryCouplingTimeUpdate
         * repairs is extended to the truncated columns' coefficients, which handles
         * departures and the κ shrink in one expression and reduces exactly to the
         * scalar-κ behaviour when nothing was truncated.  This is deliberately not
         * "annihilate the family sum": the harvest target is @f$-g_r(c)@f$, and a Galileo
         * ambiguity row measures @f$\Sigma_E=-1@f$ exactly, so annihilation would itself
         * inject a gauge observation.
         *
         * Default false (byte-identical).  Requires `carry_conditional_coupling`.
         *
         * @see networkCarryCouplingReinitSources, networkCarryCouplingTimeUpdate
         */
        bool networkCarryCouplingGaugeProject() const { return network_carry_coupling_gauge_project_; }

        /**
         * @brief Widen the coupling reinit feed to every realization-step source and
         *        decouple it from κ (`network.carry_coupling_reinit_sources`).
         *
         * A shared column that was re-realized since a row's harvest (phase-anchor
         * re-reference, eq34 chain re-reference, datum-slip PB re-realization) carries an
         * unknown frame step in its stashed @f$\hat s_{\text{prev}}@f$.  Each of those
         * mechanisms erases the re-referenced family's own carry, but every other carried
         * local still holds a `coupling[]` entry on that column, folded at full weight in
         * the retired frame, which injects the step as row bias at conditional-variance
         * (millimetre) confidence.
         *
         * Armed, the reinit history is keyed `(kind, sat, signal)` with an empty signal as
         * the sat-wide wildcard (the clock-QC feeds mark whole satellites; the
         * realization-step feeds mark one (sat,signal) family), it is fed from those
         * realization-step sets, and it is honoured whether or not the κ border time update
         * is armed.  Re-realized columns then take the departed route (κ=0), the only
         * correct treatment for an unknown step, and @ref networkCarryCouplingGaugeProject
         * repairs the family-sum deficit that the marginalization opens.  Arming this alone
         * is legal but logs a warning: it trades a stale frame step for a gauge observation.
         *
         * Default false (byte-identical).  Requires `carry_conditional_coupling`.
         *
         * @see networkCarryCouplingGaugeProject
         */
        bool networkCarryCouplingReinitSources() const { return network_carry_coupling_reinit_sources_; }

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
         * Default false keeps the per-epoch absolute snapshot solve byte-identical.
         */
        bool networkCarryZwd() const { return network_carry_zwd_; }

        /**
         * @brief Carry the station ZWD column inside the F3 jointly whitened local block
         *        rather than as a separate diagonal row
         *        (@c network.carry_zwd_in_block, sub-knob of @ref networkCarryZwd).
         *
         * @ref networkCarryZwd (B9) folds the carried ZWD as a diagonal coupling row
         * outside the station's F3 block, so the ZWD direction is asserted twice per
         * epoch: once by that row and once, implicitly, by every carried ambiguity row
         * of the same station (a station-common direction the diagonal carry over-counts
         * ~n_LOS×; SPEC_amb_iono_split_overcount §7 item 17, block all carried local
         * columns).  That double count is why B9 alone was falsified on the quiet
         * 60-station truth bed (consumer 3D rms 0.204 → 0.723 m).
         *
         * When on, the ZWD column joins the station's harvest block: its
         * @f$(N_{rr}^{-1})_{c\,c_j}@f$ cross terms against every carried ambiguity column
         * are gathered by the same per-column solve at harvest (row @c n of
         * @c PlHarvestBlock::C), the entry is stashed with @c blk / @c blk_idx like an
         * ambiguity, and at inject it is pushed as one more @c LocalBlockCol so the whole
         * station folds through one Cholesky.  The prior is then exact along every
         * local-local direction, including the station-common ones, which is what makes
         * carrying the ZWD valid.
         *
         * Interlocks (loader-enforced, warn + disable): requires @ref networkCarryZwd
         * and @ref networkPhaseLevelingLocalBlock, so the knob is never armed and inert.
         * When the block does not factorize the ZWD degrades to the existing B9 diagonal
         * row (counted, @c pl_blk_zwd_fallback_diag).  Default false = byte-identical
         * (the harvest @c cols set, and therefore @c C's dimension, is unchanged).
         */
        bool networkCarryZwdInBlock() const { return network_carry_zwd_in_block_; }

        /**
         * @brief Carry the per-LOS slant-ionosphere columns inside the F3 jointly
         *        whitened local block (@c network.carry_iono_in_block): the I half of
         *        the ZWD-in-block carry.
         *
         * Motivation (SPEC_amb_iono_split_overcount §7 item 17, block all carried
         * local columns; §6.16.7d for the ρ half): on the slipped 60-station bed the
         * producer's per-band DD errors sit at ~1.7 cycles with the WL intact, a
         * station-wide @f$(\rho, I)@f$ split error frozen into the zero-q levels at
         * every re-level (§6.17e).  Carrying the ZWD (the @f$\rho@f$ half) inside the
         * block took the ZWD truth rms from 2.10 to 1.19 m and the consumer from 1.75
         * to 1.09 m.  This knob is the @f$I@f$ half.
         *
         * It differs from the retired diagonal carry @c network.carry_iono (QUEUE 8(b),
         * FALSIFIED in four arms): that carry folded each LOS as an independent diagonal
         * prior row, which asserted the per-LOS iono direction a second time on top of the assertion
         * the station's B carried band priors already make implicitly.  That is the
         * B-fold over-count of §7 item 17, the same structural defect that falsified
         * the diagonal ZWD carry (consumer 3D rms 0.204 → 0.723 m on the quiet bed,
         * against 0.088 m in-block).  Inside the jointly whitened block the harvested
         * @f$(N_{rr}^{-1})_{c\,c_j}@f$ cross terms subtract that overlap exactly.  The
         * two are therefore mutually exclusive: arming both is a hard loader error,
         * not a warning.
         *
         * Wiring: at the harvest the station's present per-LOS iono columns are
         * appended to the block's @c cols (between the ambiguities and the ZWD, so the
         * ZWD keeps its last-row slot) and each is harvested with the same per-column
         * solve the ambiguities use, so @c PlHarvestBlock::C becomes
         * @f$(n_{amb} + n_{iono} + [1\ \mathrm{ZWD}])^2@f$; the entry stashes with the
         * same @c blk / @c blk_idx bookkeeping, and PASS B pushes one more
         * @c LocalBlockCol per carried LOS so the whole station folds through one
         * Cholesky.  The carried value is absolute (@c x_lin + @c x_r, mirroring
         * @c LocalCarry::value_abs), because a carry must be frame-complete;
         * @c network_ionosphere.csv writes @c x_r alone and is not.
         *
         * Deliberate differences from the QUEUE-8(b) diagonal carry: no inject-side
         * variance floor (the floor bounded how far a double-counted prior could
         * out-stiffen the code channel; the block's exactness replaces it); the
         * "first sighting ⇒ no carry" rule gives every newborn LOS a one-epoch cold
         * start.
         *
         * Interlocks (loader-enforced): requires @ref networkPhaseLeveling and
         * @ref networkPhaseLevelingLocalBlock (warn + disable otherwise, since an
         * armed but inert knob is not allowed; invariant #4); disabled with a warning
         * under @ref networkCarryInvariant (F3 is structurally inert there).  Default
         * false = byte-identical
         * (the harvest @c cols set, and therefore @c C's dimension, is unchanged).
         */
        bool networkCarryIonoInBlock() const { return network_carry_iono_in_block_; }

        /**
         * @brief Per-epoch random-walk process noise of the in-block iono carry, m² of
         *        slant delay at the GPS L1 frequency (@c network.carry_iono_in_block_q_m2).
         *
         * Unit and frame are stated explicitly because they differ from every other
         * carry q in this config: the per-LOS iono state is metres of slant delay at L1
         * (@c StateManager::addIonosphereState takes "initial slant ionosphere delay in
         * meters"; @c MeasurementModel scales it per signal by
         * @f$\alpha = f_{L1}^2/f_i^2@f$).  This knob is therefore m² at L1, and it is
         * per epoch, not per second (@ref networkZwdCarryQ is m²/s).  The gap is
         * converted to epochs with the
         * run's own observed inter-epoch spacing (the current PASS-B time minus the
         * previous harvest stamp), floored at one epoch, so the knob means the same
         * thing at 1 s and at 30 s cadence and needs no nominal-rate config.
         *
         * The inflated variance @f$var + q \cdot gap_{ep}@f$ goes on the block's diagonal
         * for the iono columns only (Σ's off-diagonals are the harvested cross terms),
         * which is @f$C' = C + \mathrm{diag}(q\,gap)@f$ restricted to those rows/columns:
         * PD-preserving by construction (raising a diagonal of a PD matrix).
         *
         * Default 2.5e-3 m² ⇒ σ ≈ 5 cm of L1 slant delay per 30 s epoch: loose enough
         * that the carry never resists a real ionospheric gradient, tight enough that
         * it supplies the between-epoch continuity a per-epoch white iono column lacks.
         */
        double networkCarryIonoInBlockQM2() const { return network_carry_iono_in_block_q_m2_; }

        /**
         * @brief Staleness bound of the in-block iono carry, seconds
         *        (@c network.carry_iono_in_block_max_gap_s).
         *
         * A LOS not seen for longer than this is dropped (counted,
         * @c n_iono_blk_gap_drop) rather than folded: the event response for the
         * absent-satellite edge (invariant #1) and the decoupling edge (invariant #7).
         * Defaults to @ref networkPhaseLevelingMaxGapS: the iono carry rides the same
         * block the leveling carry does, so a horizon longer than the block's own would
         * keep an entry alive with no block to join (the QUEUE-8(b) knob ran longer
         * than the leveling gap on purpose, because its diagonal row needed no block).
         * A JSON value ≤ 0 is the "use the leveling gap" sentinel; a positive value
         * overrides.
         */
        double networkCarryIonoInBlockMaxGapS() const { return network_carry_iono_in_block_max_gap_s_; }

        /**
         * @brief Carry the station's receiver clock column inside the F3 jointly
         *        whitened local block as a random walk about a carried drift
         *        (@c network.carry_rx_clock_in_block): the @f$\rho@f$-direction sibling
         *        of @ref networkCarryZwdInBlock and @ref networkCarryIonoInBlock.
         *
         * Motivation.  On the network producer the per-station receiver clock is a
         * per-epoch local parameter: it is a column of @f$A_r@f$ (see the linearity
         * invariant of @c Application::networkSplitStation_: "rx clock, tropo ZWD,
         * per-sat slant iono, ambiguities, code/phase biases, ISBs"), it is eliminated
         * by the Schur reduction every epoch, and it has no temporal prior at all;
         * @c filter.process_noise.clock_bias_q is inert on this path (an arm with q
         * 1000 → 0.09 produced a byte-identical @c products.txt).  On the slipped bed
         * that free per-epoch column is the sink for the @f$\rho@f$ direction: it
         * absorbs, instantly and without cost, whatever station-common error the
         * levels and the ZWD could not (rx-clock contrast vs truth rms 8.3 m).
         * Carrying it gives that direction a price.
         *
         * Why in the block and not as a diagonal row: the argument is the ZWD's
         * (SPEC_amb_iono_split_overcount §7 item 17, block all carried local columns).
         * The receiver clock is the most station-common direction there is, since
         * every carried ambiguity row of the station asserts it implicitly, so an
         * independent diagonal prior would assert it a further @f$n_{LOS}@f$ times.
         * That over-count is what falsified the diagonal ZWD carry (consumer 3D rms
         * 0.204 → 0.723 m on the quiet bed, against 0.088 m in-block).  Inside the
         * block the harvested @f$(N_{rr}^{-1})_{c\,c_j}@f$ cross terms subtract the
         * overlap exactly.
         *
         * The drift, and why this is not a bare random walk.  The producer never
         * writes its solution back into the station @c StateManager, so
         * @c x_lin(clock) is set once (0, or the latched @ref networkColdClockSeed
         * median) and is constant for the whole run: the entire time-varying receiver
         * clock (offset, drift and jitter) lives in @f$x_r@f$ every epoch.  A real
         * receiver clock is @f$c(t)=c_0+d\,t+\mathrm{RW}@f$ with a drift @f$d@f$ of
         * order 1e-9 s/s (0.3 m/s ⇒ 9 m per 30 s epoch; the sim truth bed measures
         * 0.29994 m/s with 0.299 m RW increments), so a pure RW prior would be wrong by
         * ~30σ every epoch.  The carry therefore holds a (bias, drift) pair: the block
         * column holds the bias, and the drift is a per-station scalar Kalman state
         * (its own @ref networkCarryRxClockRateQ) updated from the harvested
         * increments, whose contribution @f$\sigma_d^2\,\Delta t^2@f$ joins the
         * injected variance.  The injected prior mean is
         * @f$\hat c_{prev} + \hat d\,\Delta t@f$, not @f$\hat c_{prev}@f$.  With an
         * uninformative first rate variance (@ref networkCarryRxClockRateVar0) the
         * first-sighting prior is deliberately near-useless (σ ≈ 30 m) and tightens to
         * σ ≈ 0.3 m after one increment: self-limiting, never a stiff wrong prior.
         *
         * Interlocks (loader-enforced): requires @ref networkPhaseLeveling,
         * @ref networkPhaseLevelingLocalBlock and a structurally buildable F3 block
         * (@ref networkCarryConditionalCoupling on), otherwise warn + disable, since an
         * armed but inert knob is not allowed (invariant #4); disabled with a warning
         * under @ref networkCarryInvariant (F3 is structurally inert there); a hard error
         * on a negative q / gap / rate variance.  Default false = byte-identical (the
         * harvest @c cols set, and therefore @c C's dimension, is unchanged).
         */
        bool networkCarryRxClockInBlock() const { return network_carry_rx_clock_in_block_; }

        /**
         * @brief Per-epoch random-walk process variance of the carried receiver-clock
         *        bias, m² (@c network.carry_rx_clock_q_m2).
         *
         * Per epoch, not per second (@ref networkZwdCarryQ is m²/s), matching
         * @ref networkCarryIonoInBlockQM2: the gap is converted to
         * epochs with the run's own observed inter-epoch spacing (the current PASS-B time
         * minus the previous harvest stamp), floored at one epoch, so the knob means the
         * same thing at 1 s and at 30 s cadence and needs no nominal-rate config.
         * The inflation @f$var + q\cdot gap_{ep} + \sigma_d^2\Delta t^2@f$ lands on the
         * block's diagonal for the clock column only (Σ's off-diagonals are the harvested
         * cross terms), i.e. @f$C' = C + \mathrm{diag}(\cdot)@f$, PD-preserving by
         * construction.
         *
         * Default 0.09 m² ⇒ σ = 0.30 m of clock jitter per 30 s epoch, which matches the
         * sim truth bed's detrended increment std (0.2988 m, lag-1 correlation −0.004:
         * a white random walk about the drift line).  A field receiver's short-term
         * Allan deviation sets the field value; 0.09 does not necessarily transfer.
         */
        double networkCarryRxClockQM2() const { return network_carry_rx_clock_q_m2_; }

        /**
         * @brief Staleness bound of the receiver-clock carry, seconds
         *        (@c network.carry_rx_clock_max_gap_s).
         *
         * A station unseen for longer than this has its clock carry dropped (counted,
         * @c n_rxclk_carry_gap_drop) rather than folded: the absent-station edge
         * (invariant #1) and the decoupling edge (invariant #7).  Defaults to
         * @ref networkPhaseLevelingMaxGapS via a 0 sentinel resolved at load: the clock
         * carry rides the same block the leveling carry does, so a longer horizon would
         * keep an entry alive with no block to join.  It is also the horizon over which
         * the extrapolated drift term @f$\hat d\,\Delta t@f$ is trusted at all.
         */
        double networkCarryRxClockMaxGapS() const { return network_carry_rx_clock_max_gap_s_; }

        /**
         * @brief Initial variance of the carried receiver-clock drift state, m²/s²
         *        (@c network.carry_rx_clock_rate_var0_m2ps2).
         *
         * The drift is unknown at a station's first sighting, so its prior starts here and
         * the scalar rate filter collapses it after one harvested increment
         * (@f$R_{rate}\approx(2\,var+q)/\Delta t^2@f$, tiny against this).  It is
         * deliberately large (default 1.0 m²/s² ⇒ σ 1 m/s, three times any plausible
         * receiver drift): the resulting first-epoch injected variance
         * @f$\sigma_d^2\Delta t^2 = 900@f$ m² makes the pre-rate prior near-uninformative
         * on purpose, so a station whose drift is not yet known is never given a stiff
         * wrong clock.  Lowering it needs evidence; raising it costs only one more
         * near-free epoch.
         */
        double networkCarryRxClockRateVar0() const { return network_carry_rx_clock_rate_var0_; }

        /**
         * @brief Per-epoch random-walk process variance of the carried receiver-clock
         *        drift state, m²/s² (@c network.carry_rx_clock_rate_q_m2ps2).
         *
         * The drift itself wanders (temperature, ageing).  Default 1e-10 m²/s² per epoch
         * ⇒ σ 1e-5 m/s per epoch, ~1.1e-4 m/s over an hour at 30 s: three orders under a
         * typical receiver drift, so the rate filter tracks slow drift changes without
         * chasing the bias jitter.  0 gives a strictly constant-drift model (valid on the
         * sim bed, whose drift is exactly linear).
         */
        double networkCarryRxClockRateQ() const { return network_carry_rx_clock_rate_q_; }

        /**
         * @brief Innovation gate (in σ) on the carried receiver clock's one-step prediction
         *        (@c network.carry_rx_clock_jump_k).
         *
         * The wired event response of invariant #1.  Field receivers step their clocks (a
         * millisecond = 3e5 m is routine), and the network path has no other
         * receiver-clock jump detector.  At each harvest the newly solved absolute clock
         * is compared with the prediction the previous epoch would have injected;
         * @f$|v| > k\sqrt{var_{pred}}@f$ means the receiver jumped (or the solve did), and
         * the response erases the bias and the drift state and restarts them, counted as
         * @c n_rxclk_carry_refused.  Both edges are explicit (invariant #7): the
         * decoupling edge bounds the damage (nothing is carried across the jump), and the
         * re-coupling edge re-seeds cold from data with the uninformative rate prior, so
         * the two epochs after a jump inject a near-free prior rather than a stale one.
         * Default 10.0 σ: loose enough that ordinary RW jitter never trips it (a 10σ
         * event on a Gaussian is ~1e-23), tight enough that a 1 ms step (1e6 σ) always does.
         */
        double networkCarryRxClockJumpK() const { return network_carry_rx_clock_jump_k_; }

        /**
         * @brief Also carry the station's inter-system bias (ISB) columns in the same
         *        block (@c network.carry_rx_clock_isb, sub-knob of
         *        @ref networkCarryRxClockInBlock).
         *
         * The per-system time-offset states (@c StateManager::getSystemTimeOffsetStates)
         * are local @f$A_r@f$ columns like the clock, and physically they are
         * near-constant biases (the sim truth injects a fixed 8.000 m GPS→Galileo offset),
         * so they take a pure random walk with no drift state; a drifting ISB would be a
         * receiver defect rather than a model.  They share @ref networkCarryRxClockQM2,
         * which is conservative for them by a wide margin.  Counted separately
         * (@c n_pl_blk_rxclk_isb_cols) so an arm can read the two families apart; 0 there
         * is not an alarm (a BSSD or GPS-only posture allocates no ISB state at all).
         * Default true: the ISB is a local column of the same block, and leaving it white
         * while pricing the clock would move the sink one column over.
         */
        bool networkCarryRxClockIsb() const { return network_carry_rx_clock_isb_; }

        /**
         * @brief ZWD carry random-walk process noise (m²/s) inflating the carried ZWD
         *        variance over the inter-epoch gap (@c network.zwd_carry_q, B9).
         *
         * @c var_eff = var + q·Δt_since_last_seen.  ZWD is a slow random walk (τ ~ hours),
         * so a small positive q (default 1e-9 m²/s ≈ 1.9 mm/√h) models the between-epoch
         * drift.  Must be @c >= 0.  Used only when @ref networkCarryZwd is on.
         */
        double networkZwdCarryQ() const { return network_zwd_carry_q_; }

        /**
         * @brief Seam-scoped post-resume ZWD re-seed (@c network.zwd_seam_reseed,
         *        SPEC_synthetic_minimum_network §17 carry_zwd re-base arms, finding 3).
         *
         * On a warm resume that restored Z rows, the B9 ZWD carry (inject + rolling
         * stash, identical fold mechanics) runs for the first
         * @ref networkZwdSeamReseedEpochs emitted epochs after the seam and then
         * retires.  The per-station filter state is deliberately not snapshotted, so its
         * ZWD memory cold-starts at every resume; the seam window bridges that re-warm
         * with the snapshot's h-vintage posterior.  It never runs on continuous epochs,
         * where the blanket @ref networkCarryZwd was FALSIFIED (structural information
         * double-count against the station filter's own in-process persistence).  Uses
         * @ref networkZwdCarryQ for the RW inflation.  Deliberately not in the
         * warm-resume fingerprint: it is a forward-only resume response policy (the
         * relevel_free_epoch exemption class) that changes what a resumed run does with
         * restored Z rows, re-references nothing retroactively, and changes the meaning
         * of no serialized state.  Default false = byte-identical.
         */
        bool networkZwdSeamReseed() const { return network_zwd_seam_reseed_; }

        /**
         * @brief Post-resume ZWD re-seed horizon in emitted epochs
         *        (@c network.zwd_seam_reseed_epochs).
         *
         * The seam window's retirement clock: how long the restored ZWD keeps being
         * folded while the (un-snapshotted) station filter re-establishes its own
         * memory.  Default 480 (4 h at 30 s) sits past the measured ~3.1 h
         * cold-resume re-convergence and below the ≥ 6–8 h onset of the
         * continuous-carry double-memory failure; 720 epochs has also been validated
         * as safe.  Only used when @ref networkZwdSeamReseed is on.
         */
        int networkZwdSeamReseedEpochs() const { return network_zwd_seam_reseed_epochs_; }

        /**
         * @brief Seam-window ZWD re-seed random-walk process noise (m²/s)
         *        (@c network.zwd_seam_reseed_q, RS-10b,
         *        docs/SPEC_event_harness.md §21.21).
         *
         * The stiffness of the @ref networkZwdSeamReseed response, decoupled from the
         * continuous-run carry parameter.  Default @c -1 (any negative value) is the
         * "unset" sentinel: the seam fold then uses @ref networkZwdCarryQ, so an unset
         * knob is byte-identical.  A positive value replaces it, but only inside the
         * live seam window (@c var_eff = var + q·Δt on the restored Z row); a
         * continuous epoch's @ref networkCarryZwd row always uses @ref networkZwdCarryQ.
         *
         * Reason (§21.21): @ref networkZwdCarryQ is in the warm-resume fingerprint
         * (carry q's are solve-affecting on both sides of a seam and must cold-start),
         * while the seam response that borrows its weight deliberately is not.  Without
         * this knob the one parameter a resume response exists to tune cannot be varied
         * on a resume leg: a @c zwd_carry_q bracket is refused at the fingerprint check
         * (the inherited 1e-9 gives σ 1.7e-4 m over 30 s, while a measured ZWD sink
         * increment implies q ≈ 1.7e-2).  This knob is in the same exemption class as
         * @ref networkZwdSeamReseed itself (forward-only resume response policy, not
         * fingerprinted), so a bracket runs off one snapshot.
         *
         * Must be @c > 0 when set; @c 0 is rejected at load (it would make the
         * inflation a no-op while reading as "armed").  A negative value means
         * "borrow @c network.zwd_carry_q".
         */
        double networkZwdSeamReseedQ() const { return network_zwd_seam_reseed_q_; }
        /**
         * @brief Resume frame restore (@c network.resume_frame_restore,
         *        SPEC_synthetic_minimum_network §18.10).
         *
         * Without it, a warm resume that restores every carry still loses the
         * realization frame: the per-(sat,signal) phase-anchor incumbent and its R2
         * sticky pin target, the Q3 clock anchor's per-satellite state and the CLK-R
         * held frame are not restored.  The first warm epoch's election then runs
         * without incumbents (every family marked as re-referenced, all restored
         * leveling carries erased, their D2 phase-bias prior marginalized, the frame
         * re-pinned at fresh code seeds), every clock folds loose for the anchor's
         * re-couple horizon, and the emitted common mode restarts at zero; the wire
         * then re-converges its phase-bias split for ~4 h (§18.9).  With this knob the
         * snapshot's frame rows (captured under network.state_snapshot_frame_capture,
         * snapshot format v2) are restored before the first election / fold / emit,
         * so the resumed run continues the pre-seam realization.  Applies at the
         * restore site only; continuous epochs never execute it.  Deliberately not in
         * the warm-resume fingerprint (forward-only resume response policy, the
         * exemption class of @ref networkZwdSeamReseed).  Default false =
         * byte-identical.
         */
        bool networkResumeFrameRestore() const { return network_resume_frame_restore_; }
        /**
         * @brief Resume coupling-regressor restore (@c network.resume_coupling_restore,
         *        SPEC_synthetic_minimum_network §18.12).
         *
         * The re-coupling edge for the leveling carry (invariant #7).  The base
         * snapshot serializes a @c LocalCarry's @c (value_abs, var, last_seen_s) but
         * not its @c coupling, so without this knob a warm-resumed row injects with an
         * empty regression: a plain diagonal prior asserted at the pure conditional
         * variance, which no continuous epoch produces (the diagonal carry is stiff
         * at the marginal variance; the coupling row is stiff at the conditional one
         * but shared-neutral).  Such rows drive the border about 100× harder than the
         * anchored clock prior and trip most restored anchors at the first fold.
         *
         * Armed, the restore rebuilds each carry's regression verbatim from the v3
         * snapshot's QS dictionary and QL/QZ rows, bit-exact and in harvest order.
         * Coefficients are not re-sparsified and @c ŝ_prev / @c P_prev are not
         * re-derived: @c P_prev == 0 is the meaningful "datum-pinned, exactly
         * known" value, and re-deriving it would activate the κ shrink.  It also
         * seeds @c tu_reinit_hist from the columns the write epoch was re-drawing,
         * which wires the decoupling edge.  A row with no QL/QZ record keeps its
         * empty coupling and degrades to the diagonal.
         *
         * @warning Changes the row's direction only, never @c var.  A weight change
         *          (the retired seam variance re-inflation) lets the level re-float on
         *          the cold station layer (the consumer walked 5.13 m in hour 12).  The
         *          loader requires @ref networkResumeFrameRestore,
         *          @ref networkCarryConditionalCoupling and
         *          @ref networkCarryCouplingGaugeProject.
         *
         * Requires a writer armed with @c network.state_snapshot_coupling_capture; on
         * a pre-v3 or uncaptured file the restore is inert and warns (inert, not
         * clean).  Deliberately not in the warm-resume fingerprint (forward-only
         * resume response policy).  Default false = byte-identical.
         */
        bool networkResumeCouplingRestore() const { return network_resume_coupling_restore_; }
        /**
         * @brief Resume datum restore (@c network.resume_datum_restore,
         *        docs/SPEC_event_harness.md §21.7): the reader half of
         *        @c network.state_snapshot_datum_capture.
         *
         * The re-coupling edge (invariant #7) for two datum memories the base snapshot
         * does not carry.  The restored DD branch itself is correct across a
         * warm-resume seam (RS-2 / RS-2b); what still re-realizes is the wire's datum:
         *
         *  1. The eq34 chain election continues last epoch's spanning tree from
         *     @c eq34_datum_rx (the sticky datum receiver per (constellation,signal)),
         *     @c eq34_rx_parent (each receiver's parent-edge satellite) and
         *     @c eq34_sat_root, plus @c eq34_prev_target / @c eq34_prev_el /
         *     @c eq34_retained_edges.  Without this knob only the satellite-side
         *     parents (@c anchor_incumbent) survive a resume, via
         *     @ref networkResumeFrameRestore, so the receiver side is rebuilt from the
         *     cold arc registry, the pinned arcs change, and every phase-bias family
         *     re-realizes by the integer between the old and the new pinned arc.  The
         *     DD branch cannot prevent it, because the branch is invariant to the
         *     datum by construction.
         *  2. The time-linked code-bias datum's previous-value map (@c code_bias_prev_ /
         *     @c code_bias_sat_seen_) is not persisted either, so without this knob the
         *     resume block sets @c code_bias_datum_resumed_ and every first post-resume
         *     re-seed disc-bumps.
         *
         * Without it, the first post-resume epoch at a field seam emits bursts of
         * @c PHASE_DATUM/eq34_reref and @c CB_DATUM/reseed_disc_landed events, and the
         * wire steps by metres in phase bias and in datum-free clock against the
         * continuous run.
         *
         * Armed, the six eq34 maps are restored before the first election, so the
         * sticky-tree seed continues the writer's tree, and the code-bias memory is
         * restored with @c code_bias_datum_resumed_ left false: the datum continues, so
         * a re-seed bumps only where it actually re-seeds.  If either half is absent
         * from the file, that half behaves as if off and one warning is logged (inert,
         * not clean).
         *
         * The loader requires @ref networkResumeFrameRestore: the satellite-side parents
         * are the other half of the same tree, and without them the first election runs
         * without incumbents and re-references every family before these maps can
         * matter.  Deliberately not in the warm-resume fingerprint (the forward-only
         * resume-response exemption class of @ref networkResumeFrameRestore and
         * @ref networkResumeCouplingRestore).  Default false = byte-identical.
         */
        bool networkResumeDatumRestore() const { return network_resume_datum_restore_; }
        /**
         * @brief Resume emit-layer restore (@c network.resume_emit_restore,
         *        SPEC_event_harness §21.10; RS-5): the read half of
         *        @c network.state_snapshot_emit_capture.
         *
         * Armed, a warm resume restores the outputs-only emit engine before the first
         * emit: @c NetworkEmitRebase's emission frame (the @c ER rows).  Without them it
         * cold-starts at the seam, so the first post-resume epoch re-bases nothing: it
         * emits the raw solve-frame phase bias while the continuous wire at the same
         * epoch carries the whole accumulated re-base (metres per (sat,signal)).
         *
         * Inert and logged when its rows are absent (as in RS-3).  The loader requires
         * @ref networkResumeFrameRestore: an emit layer restored on top of a
         * re-elected realization frame would re-base this run's biases onto a frame
         * the writer never emitted.  Outputs-only: no estimate moves.  Deliberately
         * not in the warm-resume fingerprint (the forward-only resume-response
         * exemption class of @ref networkResumeFrameRestore,
         * @ref networkResumeDatumRestore and @ref networkResumeCouplingRestore).
         * Default false = byte-identical.
         */
        bool networkResumeEmitRestore() const { return network_resume_emit_restore_; }
        /**
         * @brief Resume station-layer restore (@c network.resume_station_restore,
         *        SPEC_event_harness §21.13; RS-6): the read half of
         *        @c network.state_snapshot_station_capture.
         *
         * Armed, a warm resume restores, before the first epoch, the two per-station
         * memories that otherwise restart cold at the seam: the rx-CB carry (the @c RC
         * rows, through @c makeRestoredLocalCarry so the entries carry the restored-arc
         * sentinel of §15.4.1) and the T5 / Iono-2 per-LOS deviation carry (the @c SD
         * rows, as written; @c ShDevCarry's own cold test is @c t_s <= 0, so a restored
         * entry reads warm by carrying its harvest stamp, and the epoch's aging pre-pass
         * ages it over the true gap).
         *
         * Each half is inert and logged when its rows are absent (as in RS-3 / RS-5,
         * neither half borrows the other's verdict).  The loader requires
         * @ref networkResumeFrameRestore: both memories are expressed in the
         * realization frame underneath them, so restoring them onto a re-elected frame
         * would re-assert a level the writer never held.  Deliberately not in the
         * warm-resume fingerprint (the forward-only resume-response exemption class of
         * @ref networkResumeFrameRestore, @ref networkResumeDatumRestore,
         * @ref networkResumeEmitRestore and @ref networkResumeCouplingRestore).
         * Default false = byte-identical.
         */
        bool networkResumeStationRestore() const { return network_resume_station_restore_; }
        /**
         * @brief Diagnostic sub-switch: restore the rx-CB half
         *        (@c network.resume_station_restore_rxcb, default true; RS-6).
         *
         * Meaningful only with @ref networkResumeStationRestore armed, and there only
         * so one leg of the attribution matrix can restore the T5 deviation half alone
         * (§21.13 gate P8c: the half that moves a channel owns it).  Not a posture knob:
         * the default is true, so an ordinary armed resume restores both halves.
         */
        bool networkResumeStationRestoreRxCb() const { return network_resume_station_restore_rxcb_; }
        /**
         * @brief Resume canonical-signal restore
         *        (@c network.resume_signal_canon_restore, SPEC_event_harness §21.16;
         *        RS-8): the read half of @c network.state_snapshot_signal_capture.
         *
         * Armed, a warm resume restores each station's per-(constellation, band,
         * obs-type) canonical-signal latch (the @c CN rows) into its @c PppModel before
         * the first epoch, so the first-epoch guard of
         * @c PppModel::decideConstellationCanonicals treats the station as already
         * decided instead of re-deciding the latch from the resume epoch's satellite
         * counts.  Without it, a re-decided latch can flip a station's signal (for
         * example L2X→L2W); the L2X tracker set then changes, the eq34 L2X component
         * loses its datum receiver, and the L2X phase-bias families step by tens of
         * metres at the first post-resume epoch.
         *
         * A snapshot without @c CN rows is inert and logged (as in RS-3 / RS-5 /
         * RS-6).  The loader requires @ref networkResumeFrameRestore: the arcs the
         * restored realization frame names are those of the writer's canonical
         * signals.  Deliberately not in the warm-resume fingerprint (the forward-only
         * resume-response exemption class of @ref networkResumeFrameRestore,
         * @ref networkResumeDatumRestore, @ref networkResumeEmitRestore,
         * @ref networkResumeStationRestore and @ref networkResumeCouplingRestore).
         * Default false = byte-identical.
         */
        bool networkResumeSignalCanonRestore() const { return network_resume_signal_canon_restore_; }
        /**
         * @brief Resume dense-border restore (@c network.resume_border_restore,
         *        SPEC_event_harness §21.14; RS-7): fold the shared border dense from the
         *        first warm epoch instead of re-seeding it from one epoch of data.
         *
         * Under @ref networkSharedCarryFullCov the D2 shared carry (satellite clocks,
         * code biases, phase biases) folds as one dense @f$(P_{prev}+Q)^{-1}@f$ block.
         * Without this knob a warm resume restores that block's values and per-key
         * variances but not its cross-covariance, so every carried key misses
         * @c prev_carry_cov.row and folds through the legacy diagonal path (the C1
         * line's "warm resume restored N diagonal carries"), although the snapshot's
         * @c QK / @c QP records (written by @c network.state_snapshot_coupling_capture)
         * carry the block.  Armed, the restore rebuilds @c SharedCarryCov from those
         * records for exactly the keys that also have a restored D row; keys with no
         * live row are dropped (row and column) and counted, and the per-key fold
         * degrades for them to the diagonal path.
         *
         * Deliberately not gated on the gauge projection.  The same records feed two independent
         * restores: the coupling rows (@c QL / @c QZ regressors) are a per-row
         * regression on the border and stay behind @ref networkResumeCouplingRestore
         * with its @c carry_coupling_gauge_project interlock (a truncated row breaks the
         * family identity @f$M^Tg_s=-g_r(c)@f$, which is what 1f repairs); the dense
         * block restored here is a covariance, carries no regression and no gauge
         * identity, and is therefore legal on a @c carry_invariant lineage where 1f is
         * out of regime.
         *
         * The loader requires @ref networkResumeFrameRestore (the border is expressed in
         * the realization frame, the same argument as for RS-6), and warn-disarms
         * without @ref networkSharedCarryFullCov (nothing consumes the block).
         * Deliberately not in the warm-resume fingerprint (the forward-only
         * resume-response exemption class of @ref networkResumeFrameRestore,
         * @ref networkResumeDatumRestore, @ref networkResumeEmitRestore,
         * @ref networkResumeCouplingRestore and @ref networkResumeStationRestore).
         * Default false = byte-identical.
         */
        bool networkResumeBorderRestore() const { return network_resume_border_restore_; }
        /**
         * @brief Resume rx-CB linearization-point restore
         *        (@c network.resume_rxcb_lin_restore, SPEC_event_harness §21.18; RS-9):
         *        restore the reference the rx-CB inject gate judges the restored carry
         *        against.
         *
         * The RS-6 restore covers the rx-CB carry's numerator (@c value_abs / @c var /
         * @c last_seen_s, the @c RC rows) and leaves its reference cold.  The gate's only
         * statistic is @f$|value\_abs - x\_lin(cb)|@f$ against a fixed 5.13 m bar (k = 10,
         * guard 0.25 m², @c var_eff p50 1.3e-2 m²), and @c x_lin(cb) is the one-shot seed
         * @c StateInitializer::initializeCodeBiases writes on the station's first processed
         * epoch; on a resumed leg that comes from the resume epoch's code instead of the
         * writer's start of day.  Without this knob, stations whose seed differs fall
         * outside the bar every epoch of the leg: their carry is refreshed and refused
         * indefinitely, with no rx-CB memory, while the run-end line still counts them as
         * carried, and their code-bias, receiver-clock and ZWD errors grow
         * (§21.18(a)/(b)).  Armed, the snapshot's @c RL rows are stashed at the RS-6
         * restore and consumed at each station's first-epoch code-bias initialization, so
         * the gate judges the restored value against the frame the writer judged it
         * against.  This completes the re-coupling edge (invariant #7): RS-6 re-validates
         * the value it reconnects, and this knob restores its frame.
         *
         * Solve-invariant by construction under this posture: a code bias enters the
         * observation equation linearly, so an @c x_lin shift is an exact solve no-op
         * (project_network_xlin_invariance) and the only consumer of the value is the gate.
         * The one mechanism that would break that is @ref networkRxCodeBiasDatum, whose RHS
         * reads @c x_lin (@f$\eta = \lambda(0 - x\_lin)@f$) and would therefore move the
         * solve; the loader refuses the pair.
         *
         * The loader requires @ref networkResumeStationRestore with its
         * @ref networkResumeStationRestoreRxCb half (restoring a reference for a carry that
         * was not restored would judge a fresh value against a stale frame, the same
         * defect inverted).  Deliberately not in the warm-resume fingerprint (the
         * forward-only resume-response exemption class of @ref networkResumeFrameRestore,
         * @ref networkResumeDatumRestore, @ref networkResumeEmitRestore,
         * @ref networkResumeStationRestore and @ref networkResumeBorderRestore).
         * Default false = byte-identical.
         */
        bool networkResumeRxCbLinRestore() const { return network_resume_rxcb_lin_restore_; }
        /**
         * @brief Warm-resume phase-bias seam mark (@c network.resume_pb_seam_mark,
         *        SPEC_event_harness §21.9; RS-4): declare the seam on the wire when the
         *        first post-resume emit re-realizes the phase-bias level.
         *
         * The CLK-R @c resume_seam rule (EmitStore.cpp) marks a warm resume only
         * when the held-frame clock engine reports @c epochs==0, so a lineage that
         * restores that engine is never marked, which is exactly the case where the
         * clock is continuous and the phase biases are not.  The emitted phase biases
         * can then move by metres per (sat,signal) across the seam against a restored
         * per-key baseline, with @c datum_changed 0 and no R record: a re-coupling edge
         * (invariant #7) that a file consumer cannot see.
         *
         * Armed, at the first emit after a warm resume (one shot) the emitter compares
         * each emitted phase bias with its restored per-key baseline
         * (@c last_phase_bias_, from the snapshot's @c epb rows) before the jump test
         * overwrites it, over exactly the jump test's own population (a key with a
         * baseline that is not stale under the staleness horizon).  A constellation with
         * more than @ref kResumePbSeamMarkFraction of that population beyond
         * @c discThresholdM is marked as the CLK-R rule marks: one
         * @c markDatumChange for that constellation, one @c PB_FRAME / @c resume_seam
         * event carrying the fraction and the counts, and the constellation merged into
         * the E record's datum set so the wire carries @c datum_changed 1.
         *
         * Outputs-only: it moves no estimate, only the declaration. On a cold start the
         * latch is never set, so it cannot fire. Deliberately not in the warm-resume
         * fingerprint (the forward-only resume-response exemption class of
         * @ref networkResumeFrameRestore, @ref networkResumeDatumRestore and
         * @ref networkResumeCouplingRestore). Default false = byte-identical.
         */
        bool networkResumePbSeamMark() const { return network_resume_pb_seam_mark_; }
        /// Fraction of a constellation's fresh-baseline phase-bias keys that must exceed
        /// @c discThresholdM at the first post-resume emit for the seam to be declared
        /// (RS-4; the single definition for the rule and its unit test).
        static constexpr double kResumePbSeamMarkFraction = 0.10;
        /**
         * @brief Re-admission continuity (READMIT): seed-first satellite re-admission
         *        (@c network.readmit_seed_horizon_s, seconds; STABILITY campaign E2,
         *        SPEC_synthetic_minimum_network §18.4 / SPEC_amb_iono_split_overcount §7).
         *
         * The re-admission edge: a satellite excluded for eclipse (~55 min) returns with
         * its clock / code-bias columns re-registered cold, the first solve realizes them
         * from code, and the Q3 anchor, whose per-satellite carry is older than
         * @c clock_anchor_max_gap_s, trips `gap`, re-draws the clock loose on a code-only
         * border and re-couples on that re-draw three epochs later, which freezes a
         * metre-class clock tail for hours (T3, §18.1).  The same cold seed drives the
         * consumer's post-eclipse excursion.
         *
         * With this knob armed, a departing satellite's D2 clock and code-bias carry is
         * parked (@c CarryPrior::t_park_s) for up to this many seconds (walked by every
         * frame re-base that walks @c prev_carry, dropped at a clock-datum change of its
         * constellation, never serialized), and on re-registration inside the horizon it
         * seeds the column at @c var + q·gap.  The Q3 anchor then folds anchored over its
         * true per-satellite gap (bounded by this horizon instead of the staleness bound),
         * and its NIS guard acts as mark-if-moved (a trip within @c kReadmitVerifyWindowS
         * of the seed is counted as the data rejecting it).  The phase-bias family is
         * deliberately not seeded: its frame is re-realized at the cold re-pin without
         * incumbents (the leveling carries do not survive a 55 min absence at the 120 s
         * leveling gap), and a stiff seed would conflict with the pin.
         *
         * Default 0 = off, byte-identical (nothing is ever parked).  Not fingerprinted:
         * parking adds entries and changes no carried value's meaning; a snapshot never
         * carries a parked entry (a seam inside an absence gives the cold return).
         */
        double networkReadmitSeedHorizonS() const { return network_readmit_seed_horizon_s_; }
        /**
         * @brief Re-admission continuity (READMIT): the block/row re-coupling edge
         *        (@c network.readmit_coupling_marg_ep, epochs).
         *
         * For this many epochs after a satellite's clock column returns from an absence
         * of >= 3 loop epochs, every coupling term on its shared columns takes the
         * departed route (σ² += M²·P_prev, coefficient dropped) in every station's rows
         * and F3 block, so no carried ambiguity is conditioned on a column whose
         * realization is still settling from its cold re-registration (the re-coupling
         * edge of invariant #7, per SPEC_amb_iono_split_overcount §7).  Both edges are
         * evented (`READMIT readmit` / `marg_expired`) and the diverted-term count is the
         * run-end denominator.  Requires the coupling rows (inert otherwise,
         * loader-warned).  Default 0 = off, byte-identical.
         */
        int networkReadmitCouplingMargEp() const { return network_readmit_coupling_marg_ep_; }
        /**
         * @brief Re-admission continuity (READMIT): also park and seed the code-bias
         *        columns (@c network.readmit_seed_code_bias; sub-knob of
         *        @ref networkReadmitSeedHorizonS, inert without it).
         *
         * Default true (the clock and the per-signal code biases of a departing satellite
         * are parked and seeded).  false seeds the clock alone.  The option exists because
         * a third-frequency seed (C5Q) can carry the §18.1 cold-start misrealization into
         * the return through a stiff (σ 1.9 cm) seed, which the data then reject and
         * which can trip the anchored clock seed's `nis` guard; clock-only seeding keeps
         * the target (the clock re-draw) and lets the weakly determined third-frequency
         * bias re-solve from data.  The clock-only variant was FALSIFIED as an
         * improvement: the IF-pair code-bias seeds anchor the clock-vs-code-bias split at
         * the return, and without them the clock tails return (promotion ledger row for
         * @c network.readmit_seed_code_bias, RECOMMEND_SCOPED with the default true).
         */
        bool networkReadmitSeedCodeBias() const { return network_readmit_seed_code_bias_; }
        /**
         * @brief Canonical-pair admission (@c network.canonical_pair_admission,
         *        network producer only).
         *
         * A (station, satellite) epoch enters the network path only while the station
         * holds its canonical phase and code signal on band digits 1 and 2, as that
         * station's own canonical latch (@c PppModel::decideConstellationCanonicals)
         * decided them; a record without all four is withheld before the first good
         * epoch and beyond @ref networkCanonicalPairKeepEpochs epochs after the last
         * one, and the withheld record is absent for everything downstream exactly as
         * if the RINEX record were missing.  A constellation with no canonical pair
         * decided on those two band digits is left untouched.
         *
         * An emulation over the input files
         * (docs/refound/GRS_gap_reentry_per_signal_2026-09-20.md §6) raised the dual
         * field network from 203 to 627 of 720 epochs below a decimetre, and the triple
         * network with L5 at all stations from 6 to 461.  The built rule withholds
         * exactly the emulated records but does not reproduce that gain, and on the
         * dual field bed it is worse than no rule: the emulation arms had moved the
         * producer's first-epoch canonical-signal latch (@c processing.canonical_signal_rule)
         * instead.  Ledger verdict KEEP_OFF
         * (docs/refound/CPA_canonical_pair_admission_2026-09-20.md).  Default false =
         * byte-identical (the seam returns the caller's own epoch, no copy).  Not
         * fingerprinted.
         */
        bool networkCanonicalPairAdmission() const { return network_canonical_pair_admission_; }
        /**
         * @brief Admission tolerance of the canonical-pair rule, in processed epochs
         *        (@c network.canonical_pair_keep_epochs; inert without
         *        @ref networkCanonicalPairAdmission).
         *
         * A record whose canonical pair is absent is still admitted while the last
         * good processed epoch is at most this many epochs back.  0 admits only the
         * epochs that hold the pair.  Clamped at 0 by the loader.  Default 3, which is
         * the emulation's best arm on both postures: on the field triple bed N = 3 kept
         * 461 epochs below a decimetre against N = 0's 288, because with three bands a
         * satellite without its band-2 phase is still a two-band satellite and deleting
         * it costs information; on the dual field bed the two were equal.
         */
        int networkCanonicalPairKeepEpochs() const { return network_canonical_pair_keep_epochs_; }
        /**
         * @brief Post-resume MW/LLI slip-drain warm-up (@c network.resume_slip_warmup_ep,
         *        epochs, SPEC_synthetic_minimum_network §18.10).
         *
         * A warm resume (RTB-7) restores tight carries but no slip-detector history,
         * so the MW detector's first post-resume difference (two samples, no Welford
         * reference) declares integer WL slips from noise, re-levelling about 5 % of
         * the restored arcs at the second warm epoch (§18.9).  For this many processed
         * epochs after a resume the T0 (MW/LLI) drain is diagnostic-only (its
         * detections are logged, not acted on); the T1 IF-TD and wind-up drains are
         * untouched, so a real in-gap slip keeps a live response.  0 = off.  Not
         * fingerprinted.
         */
        int networkResumeSlipWarmupEp() const { return network_resume_slip_warmup_ep_; }

        /**
         * @brief Post-resume D5 innovation-gate warm-up (@c network.resume_d5_warmup_ep,
         *        epochs; RS-2b, docs/SPEC_event_harness.md §21.6).
         *
         * A measurement knob, not a repair.  With the retired resume carry re-base (RS-2)
         * the restored branch was folded in the resumed run's own linearization and the
         * first post-resume solve was healthy, yet about half of the re-based chords still
         * tripped the D5 gate at +1 and the branch was erased at +2.  Two readings fit
         * that: the branch is correct and the cold station layer disagrees with it, or the
         * branch is inconsistent.  The gate cannot tell them apart while it re-taps,
         * because the re-tap destroys the hypothesis it would test.
         *
         * For this many processed epochs after a warm resume the D5 gate is therefore
         * diagnostic-only: a chord that trips k·σ̂₀ is counted (@c d5_warmup_suppressed
         * and the epoch's
         * @c n_d5_flags, while @c n_d5_retaps stays 0) but is not re-tapped, not marked
         * slipped for the next epoch, and its block is not refolded.  Everything else is
         * untouched: the σ̂₀ accumulator is fed before the decision, so calibration is not
         * censored; the v3 corroboration gate still runs; and the MW / LLI / IF-TD drains
         * stay live, so a real in-gap slip keeps a response (invariant #1).
         *
         * Counted in processed epochs at the install seam (the one site the gate lives
         * at), so a run that never installs a posterior never uses up the window.  Armed
         * only at a warm resume; a cold start never sets it.  0 = off = byte-identical.
         * Not fingerprinted (forward-only resume response policy, the same exemption
         * class as @ref networkResumeSlipWarmupEp).
         */
        int networkResumeD5WarmupEp() const { return network_resume_d5_warmup_ep_; }

        /**
         * @brief Carry per-station receiver code biases across epochs in the network
         *        producer as diagonal random-walk priors (@c network.carry_rx_code_bias,
         *        docs/SPEC_rx_cb_carry.md).
         *
         * Receiver code biases are hardware-stable (cm over hours); the per-epoch
         * network solve re-estimates them white at the 10 m initial σ, so the level
         * allocation (rx CB ↔ iono level ↔ SH field) is re-derived from code noise
         * every epoch.  When on, each station's present code-bias columns are carried
         * as diagonal information priors on its own local block (same
         * @ref NetworkEstimator::addStationPrior path as the ambiguity/ZWD carries)
         * with @c var_eff = var + q·Δt, and harvested post-solve outside the
         * phase-leveling gate, so the carry is live on the carry_invariant lineage.
         *
         * Guards: forced off (with a warning) unless @ref networkCodeDatumTimeLinked
         * (a time-linked code datum is what makes the carried lump frame-continuous);
         * a loader warning recommends arming @c estimation.global_vtec_sh.rep_error_demean
         * with it (the rx-CB ↔ iono-level ↔ deviation-mean three-way quasi-null).
         * Event response (invariant #1): an inject-time innovation gate withholds the
         * fold at receiver-reset scale and the harvest overwrites the stale entry.
         * Default false = byte-identical.
         */
        bool networkCarryRxCodeBias() const { return network_carry_rx_code_bias_; }

        /**
         * @brief Carry the receiver code-bias columns inside the station's jointly
         *        whitened local block, as conditional coupling rows
         *        (@c network.carry_rx_code_bias_in_block,
         *        RXCB-IN-BLOCK).
         *
         * The diagonal rx-CB fold of @ref networkCarryRxCodeBias uses the pure
         * conditional variance (N_rr⁻¹)_cc, which is the variance of the column given
         * the shared border, as an absolute prior.  It therefore re-asserts the border
         * (the column's regression on it) at that precision every epoch, on top of the
         * dense border carry that already holds the same information: the information
         * along each satellite's level doubles per epoch (measured on the sim bed:
         * P_L 188, 63, 27, 13, 6.3 m² over epochs 1 to 5 against 188/k without the
         * carry).  With this knob the columns are harvested with their regression on
         * the border and their cross covariances to the station's other carried
         * columns, and folded as rows of the same whitened block as the ambiguities
         * (the receiver-clock and ZWD in-block route), so neither the border nor
         * the local-local overlap is counted twice.  There is no diagonal fallback:
         * before the leveling carry is warm, and on a non-SPD block, the columns carry
         * nothing (counted).
         *
         * Requires @ref networkCarryRxCodeBias, @ref networkCarryConditionalCoupling
         * and a local block (the float_kf vehicle, or
         * @ref networkPhaseLevelingLocalBlock).  Fingerprinted.  Default false =
         * byte-identical.
         */
        bool networkCarryRxCodeBiasInBlock() const { return network_carry_rx_code_bias_in_block_; }

        /**
         * @brief A float-vehicle ambiguity drop clears the arc's border regression and its
         *        block row even when the variance is already at birth strength
         *        (@c network.amb_float_drop_decouple_always).
         *
         * The drop restores max(var_birth, var_first, floors) and, when the variance moves,
         * clears the regression and the block row.  An arc that slips one epoch after birth
         * is already at that variance, so the drop returned early and the arc kept folding
         * its pre-slip value with full cross terms and a full border regression.  Under the
         * exact time update the station's ionosphere-free ties are phase-tight and the
         * stale value went into the satellite clocks (field, BSHM00ISR G07 L2W at epoch 8:
         * metre-scale level jumps on six satellites).  Default false = byte-identical.
         * Inert without the float_kf vehicle.
         */
        bool networkAmbFloatDropDecoupleAlways() const { return network_amb_float_drop_decouple_always_; }

        /**
         * @brief Relative-pivot bound of the dense border fold
         *        (@c network.dense_fold_kappa,
         *        default 1e-12).
         *
         * See NetworkEstimator::setDenseFoldKappa.  Must be in (0, 1e-6].  The bound
         * admits the carried blocks of a consistent level (loose level, phase-precise
         * clock-minus-bias combinations), which the historical 1e-6 sent to the diagonal
         * fallback at the anchor's engagement.  The default moved from 1e-6 to 1e-12 on
         * 2026-09-29: the golden posture is byte-identical at both
         * values (2 h, both beds); the exact-update posture's clean start is unchanged or
         * better on the level gates and the phase-visible monitor, and the smoothed hold's clones survive
         * the engagement.  1e-6 restores the historical behaviour.
         */
        double networkDenseFoldKappa() const { return network_dense_fold_kappa_; }

        /**
         * @brief rx-CB carry random-walk process noise (m²/s)
         *        (@c network.rx_cb_carry_q, docs/SPEC_rx_cb_carry.md).
         *
         * @c var_eff = var + q·Δt_since_last_seen.  Hardware code biases drift at
         * the mm-over-hours class, so the default 1e-9 (≈ 3 mm 1σ over 3 h) models
         * temperature-scale drift while remaining far stiffer than the per-epoch
         * white re-estimate.  Must be @c >= 0.  Used only when
         * @ref networkCarryRxCodeBias is on.
         */
        double networkRxCbCarryQ() const { return network_rx_cb_carry_q_; }

        /**
         * @brief Run the M2 LS-VCE carry-row variance-component monitor
         *        (@c network.carry_vce).
         *
         * When on, the network producer accumulates the post-fit innovations of
         * the carry pseudo-observations (carried value − freshly-solved value)
         * into a sequential LS-VCE estimator and logs, per carry family per
         * constellation, a measured σ̂ decomposed into a Floor component (m²,
         * cofactor 1; the measured counterpart of the hand-set @c var_floor) and a
         * Rate component (m²/s, cofactor Δt; the measured counterpart of the
         * hand-set @c q_amb).  It is a calibration instrument: it never feeds back
         * into the solve (that is the @ref networkCarryVceApply v2 path), so the
         * solution is byte-identical to the monitor-off run.
         * Foundation: Teunissen & Amiri-Simkooei 2008 (LS-VCE), residual form.
         * Default false = no accumulator, no logging (byte-identical).
         */
        bool networkCarryVce() const { return network_carry_vce_; }

        /**
         * @brief Apply path (M2 v2): feed the measured σ̂ back into the carry
         *        stochastic model (@c network.carry_vce_apply).
         *
         * Implements NGS LASER Guide v2.7 5-93..5-95 "VCE by use of factors"
         * (Koch 1999 §§3.6.1–3.6.2): per (family × constellation) the fold
         * variance of every carry pseudo-observation is multiplied by a variance
         * factor α² that iterates toward the monitor's measured
         * @c robust_floor / @c hand_fold_var ratio, inflate-only (α² ≥ 1),
         * slew-limited, and frozen on contaminated / under-populated epochs.  See
         * @ref NetworkCarryVceApply and docs/SPEC_carry_vce_apply.md.
         *
         * Solve-affecting and fingerprinted: a warm resume with this armed is a
         * hard run error (v1 is fresh-convergence-only; α² restarting at 1
         * mid-chain would change the solve across the seam).  Requires
         * @ref networkCarryVce.  Default false = no factor, byte-identical.
         */
        bool networkCarryVceApply() const { return network_carry_vce_apply_; }

        /**
         * @brief Carry families the M2 v2 apply path scales
         *        (@c network.carry_vce_apply_families, default @c "pl").
         *
         * Must be a subset of @ref networkCarryVceComponents (a family that is
         * not monitored has no σ̂ to apply; a hard load error, not a silent
         * no-op).  Only used when @ref networkCarryVceApply.
         */
        const std::string& networkCarryVceApplyFamilies() const { return network_carry_vce_apply_families_; }

        /**
         * @brief Per-epoch maximum multiplicative step of α², both directions
         *        (@c network.carry_vce_apply_slew, default 1.05).
         *
         * Keeps the solve's conditioning quiet while the factor tracks.  It is not
         * the safety argument (a configured rate limit did not prevent the era-2
         * discharge); the inflate-only clamp is.  @c <=1 disables the limiter.
         */
        double networkCarryVceApplySlew() const { return network_carry_vce_apply_slew_; }

        /**
         * @brief Ceiling on α² (@c network.carry_vce_apply_max, default 1000).
         *
         * Entry and exit are events (invariant #7).  The floor is 1 by
         * construction (inflate-only) and is not configurable.
         */
        double networkCarryVceApplyMax() const { return network_carry_vce_apply_max_; }

        /**
         * @brief Effective (forgetting-weighted) monitor rows a group needs before
         *        its factor may move (@c network.carry_vce_apply_min_rows, 200).
         *
         * Below it the group freezes at its current α² and the trace flags it.
         */
        int networkCarryVceApplyMinRows() const { return network_carry_vce_apply_min_rows_; }

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
         * @brief Per-station T0 cycle-slip repair for the carried ambiguities
         *        (phase-leveling Increment 2; @c network.cycle_slip.turbo_edit).
         *
         * When on (and @ref networkPhaseLeveling is on), each non-pivot station runs
         * a persistent MW + GF TurboEdit detector pair every epoch; a confident
         * per-(sat,signal) integer slip @c dN is applied as a continuous,
         * covariance-preserving shift to the carried ambiguity
         * (@c value_abs += dN·λ), so a repaired cycle slip does not corrupt the
         * carry.  Refloat/drop of unrepairable slips and pivot-slip handover belong
         * to other mechanisms (later increments).  Default false keeps the
         * carry-only path (Increment 1) byte-identical.
         */
        bool networkCycleSlipTurboEdit() const { return network_cycle_slip_turbo_edit_; }

        /**
         * @brief Integer-confidence gate for a network T0 slip repair
         *        (@c network.cycle_slip.turbo_edit_frac_max; phase-leveling Inc 2).
         *
         * A repair is applied (continuous @c value_abs += dN·λ) only when its rounding
         * residual @c frac (0 = exact integer, 0.5 = worst) is ≤ this threshold and
         * @c |dN| ≤ @ref networkCycleSlipTurboEditDnMax.  The network runs the MW
         * detector without external @c .BIA (it estimates its own satellite biases),
         * so the wide-lane is float-based and TurboEdit's own ~0.3 gate is too loose.
         * A repair that fails either bound is left untouched (carry kept): for the
         * BIA-less network those low-confidence emitted repairs are predominantly
         * float-MW false positives, so refloating/dropping them measurably degrades
         * the products.  The lock-in of a rare real but rejected slip is bounded by
         * @ref networkPhaseLevelingVarFloorM2 / the max-gap drop and is resolved by
         * the cross-station RAIM consensus (Increment 3).  Default 0.15.
         */
        double networkCycleSlipTurboEditFracMax() const { return network_cycle_slip_turbo_edit_frac_max_; }

        /**
         * @brief Magnitude bound (cycles) on a trusted network T0 slip repair
         *        (@c network.cycle_slip.turbo_edit_dn_max; phase-leveling Inc 2).
         *
         * @c frac gates rounding cleanliness, not magnitude: a BIA-less float can
         * round cleanly to a large spurious integer (TurboEdit's own cap is 1000
         * cycles ≈ 190 m).  A repair with @c |dN| above this bound is treated as
         * untrustworthy and left untouched (carry kept) rather than shifted, so only
         * small, confident integers move the carry.  Default 50 cycles; it can be
         * raised once external BIA makes the wide-lane integer trustworthy.
         */
        int networkCycleSlipTurboEditDnMax() const { return network_cycle_slip_turbo_edit_dn_max_; }

        /**
         * @brief Whether the MW detector runs as a pure detector in the network
         *        (@c network.cycle_slip.mw_detect).
         *
         * A carried carrier-phase ambiguity is a zero-process-noise random
         * constant: an unscreened cycle slip makes the post-slip phase
         * self-consistent with the slipped integer, so the stiff carry freezes
         * the wrong value into the shared clock.  This complements
         * @ref networkCycleSlipIfPhaseTd (the ionosphere-free primary) with two
         * more iono-safe signals: a confident wide-lane integer jump from
         * @ref MwSlipDetector::detectLargeMwResets, and the receiver's own LLI
         * loss-of-lock flag.  Either refloats the affected ambiguity (marks it
         * pending-reseed so PASS B re-estimates it from this epoch's data).  It
         * never applies an integer, so unlike the TurboEdit MW+GF repair path it
         * is valid at the 30 s product rate (the GF ionospheric drift that
         * corrupts a 30 s integer repair is not used).  Non-pivot stations only;
         * the refloat is meaningful only with @ref networkPhaseLeveling on
         * (without it, the detector is diagnostic).  Default false
         * keeps the solve byte-identical.
         */
        bool networkCycleSlipMwDetect() const { return network_cycle_slip_mw_detect_; }

        /**
         * @brief Whether MW/LLI slip detection also covers the datum station, with a
         *        network re-reference response (@c network.cycle_slip.datum_station_detect).
         *
         * Under the legacy single-pivot phase datum, @c stations[0] is skipped by
         * every slip-detection loop (its ambiguities are gauge-pinned, so a local
         * refloat is meaningless), which leaves a blind spot (§9.7.4): an undetected
         * slip there re-references the emitted phase bias of that satellite for
         * the whole network, and nothing records it.  This toggle covers it with
         * the mechanism-specific response instead of a local refloat: a confident
         * MW wide-lane jump or a qualified LLI assertion at the datum station
         * (1) emits a @c datum_slip event and counts, (2) skips the stash of that
         * satellite's shared phase-bias carry so the next kept epoch re-seeds the
         * bias at the new pivot level, (3) reseeds every other station's
         * ambiguities for that satellite in the same epoch the bias re-seeds
         * (deferred one drain, aligned across withholds), and
         * (4) queues a per-satellite discontinuity mark for the next emit
         * (invariant #7: both edges evented, plus run-end counters).  Requires
         * @ref networkCycleSlipMwDetect (the detection vehicle; the loader
         * disables it otherwise) and is inert under
         * @ref networkPhaseDatumPrioritized (station 0 is an ordinary station
         * there).  Default false keeps the solve byte-identical.
         */
        bool networkCycleSlipDatumStationDetect() const {
            return network_cycle_slip_datum_station_detect_;
        }

        /**
         * @brief Whether the network IF-phase triple-difference cycle-slip detector is on
         *        (@c network.cycle_slip.if_phase_td).
         *
         * Between-satellite, between-epoch ionosphere-free phase OMC
         * (@ref IfPhaseSlipDetector, used on the network, where the fixed
         * near-truth station coordinates remove its only blocker, the
         * pre-convergence position-uncertainty gate).  Detection-only: a flagged
         * IF pair refloats both of its band ambiguities (marks them pending-reseed,
         * so the carry re-estimates them from this epoch's data), with no MW/GF
         * confirmation and no integer repair.  The refloat action is meaningful
         * only with @ref networkPhaseLeveling on; with the carry off the detector
         * runs as a pure diagnostic.  Non-pivot stations
         * only.  Default false keeps the snapshot/carry solve byte-identical.
         */
        bool networkCycleSlipIfPhaseTd() const { return network_cycle_slip_if_phase_td_; }

        /**
         * @brief k for the network IF-phase MAD gate (@c network.cycle_slip.if_phase_td_k_mad).
         *
         * A flag fires when @f$|\Delta\Delta\phi_{IF} - \mathrm{med}| > \max(k\cdot
         * 1.4826\cdot\mathrm{MAD},\ \mathrm{floor})@f$, the MAD pooled network-wide per
         * signal pair.  Deliberately loose (8–12); a per-epoch post-estimation phase
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
         * itself: flags jumps at or above the smallest integer slip; a clean network's
         * pooled MAD is ~mm, so this floor binds and the k·MAD term is dormant).  A
         * value near 0.7 catches a bare (1,1) reliably at the cost of more flags.
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
         * equally.  Default 20 (a dense pair, ~10 sats/epoch, becomes station-dominated
         * within a few epochs; a sparse pair leans on the pool longer, as intended).
         */
        double networkCycleSlipIfPhaseTdShrinkKappa() const { return network_cycle_slip_if_phase_td_shrink_kappa_; }

        /**
         * @brief How many band pairs the IF-TD detector evaluates per satellite
         *        (@c network.cycle_slip.if_phase_td_pairs, SPEC_event_harness §14.10).
         *
         * `first` (default) keeps the original detector: one pair per satellite, the
         * first two distinct phase bands (L1C+L2W on GPS, L1C+L5Q on Galileo), so
         * every other pair is unwatched and the equal-count trap that the common-band
         * scope's IF-TD veto closes stays open on all of them.  `all` evaluates every
         * distinct pair of the satellite's tracked bands (3 on a triple-band arc),
         * each with its own pool, gradient chain, reference election and reset keys,
         * so the first pair's decisions are unchanged and the extra pairs are a pure
         * addition.
         *
         * Stored in @ref NetworkConfig; the getter name follows the sibling
         * @c if_phase_td knobs.  Inert without @ref networkCycleSlipIfPhaseTd.
         *
         * @return The configured pair mode.
         */
        NetworkIfPhaseTdPairs networkCycleSlipIfPhaseTdPairs() const {
            return network_config_.cycle_slip_if_phase_td_pairs;
        }

        /**
         * @brief Which Melbourne-Wubbena pairs the network's wide-lane detector
         *        forms (@c network.cycle_slip.mw_detect_pairs).
         *
         * `all` (default) forms every pair the detector produced (the original
         * path).  `primary` forms only the pair whose two signals both sit on the
         * constellation's ionosphere-free pin bands (the station's latched band-1 /
         * band-2 pair on a lineage that latches one signal per band), so a pair
         * carrying band 5 never becomes a candidate, never refloats an arc and
         * never enters the common-band scope's evidence.
         *
         * Detection-shaping when armed, inert on a two-band lineage (there is only
         * the primary pair) and inert without @ref networkCycleSlipMwDetect.
         *
         * @return The configured pair scope.
         */
        NetworkMwDetectPairs networkCycleSlipMwDetectPairs() const {
            return network_config_.cycle_slip_mw_detect_pairs;
        }

        /**
         * @brief Sigma multiplier for extra-band code rows in the network solve
         *        (@c network.extra_band_code_sigma_scale).
         *
         * An extra-band signal is one whose band is neither band of the
         * constellation's ionosphere-free pin pair (GPS
         * band 5 against the pinned L1/L2 on the RF-0hv posture).  Its code rows
         * fold at sigma x scale, i.e. at information / scale squared, in the
         * network solve only: the same weight the pre-fit census, the post-fit
         * screens and @c network_pf_rows.csv read, because they all read the one
         * row-weight vector this scale is applied to.
         *
         * With the extra-band code rows armed the band-5 code-bias column stays determined by the
         * band-5 code rows alone, so the wire keeps a band-5 code bias while the
         * rows lose their pull on the shared states.  1.0 is the default and is
         * never applied, so an unarmed run is byte-identical; the loader refuses a
         * value below 1.  The closed-loop consumer is pinned to 1.
         *
         * @return The multiplier; 1.0 (no scaling) by default.
         */
        double networkExtraBandCodeSigmaScale() const {
            return network_config_.extra_band_code_sigma_scale;
        }

        /**
         * @brief Cold-start absolute bound of the gross line-of-sight screen, metres
         *        (@c network.gross_los_cold_bound_m;
         *        docs/refound/ICF_ignition_code_fault_2026-10-09.md).
         *
         * Inside the screen's maturity horizon (@c network.gross_los_warmup_epochs
         * emitted epochs) a line of sight whose largest post-fit code residual exceeds
         * this many metres is declared at once, with the screen's response (carry erased,
         * code rows folded at @c network.gross_los_deweight).  While the declaration
         * stands, each epoch over the bound re-erases the carried ambiguity; the release
         * erases it once more, so the line of sight reconnects on a fresh ambiguity.  The
         * statistic is the residual the screen already forms, @f$v = y - A_s x_s - A_r
         * x_r@f$, in metres: the prefit @f$y@f$ is no use here, since it carries the
         * receiver clock's linearization offset (hundreds of kilometres on clean rows).
         *
         * 0 (the default) is off and byte-identical.  The value is read as given: the
         * loader clamps a negative or non-finite value to 0 with a warning, and the run
         * context resolves it to 0 unless @c network.gross_los_screen is on and
         * @c network.gross_los_bound is off (@c NetworkRunContext::Knobs::gl_cold_bound_m).
         *
         * @return The bound in metres; 0 when the cold path is off.
         */
        double networkGrossLosColdBoundM() const {
            return network_config_.gross_los_cold_bound_m;
        }

        /**
         * @brief Whether the second code channel is armed.
         *
         * `network.second_code_channel`.  When true a network station contributes,
         * as code-only rows, every allow-listed band-1 code it reports besides the
         * one its canonical latch selected, each modelled with its own receiver
         * inter-signal-bias state (@c StateKeys::makeReceiverIscKey).  Default off
         * and byte-identical; the loader refuses it with `processing.channel_mode:
         * all` and without @ref networkCarryRxCodeBias.
         *
         * @return True when the second code channel is armed.
         */
        bool networkSecondCodeChannel() const {
            return network_config_.second_code_channel;
        }

        /**
         * @brief The band-1 code signals a station may contribute.
         *
         * `network.second_code_signals`, validated at load: every entry is a
         * 3-character code type on band 1.  Read by
         * @c PppModel::selectUncombinedSignals (which codes to add) and by
         * @c StateInitializer::resolveReceiverBias (which codes route through the
         * inter-signal state).  Meaningless unless @ref networkSecondCodeChannel.
         *
         * @return The allow-list, in configured order.
         */
        const std::vector<std::string>& networkSecondCodeSignals() const {
            return network_config_.second_code_signals;
        }

        /**
         * @brief Sigma multiplier for the second code's rows.
         *
         * Applied in @c Application::networkSplitStation_ to every code row whose
         * receiver bias is the inter-signal state, i.e. the companion rows and any
         * fallback code.  1.0 is the default and is never applied (byte-identical);
         * the loader refuses a value below 1.  The closed-loop consumer is pinned to
         * 1 with the knob itself.
         *
         * @return The multiplier; 1.0 (no scaling) by default.
         */
        double networkSecondCodeSigmaScale() const {
            return network_config_.second_code_sigma_scale;
        }

        /**
         * @brief Whether `processing.channel_mode: all` is active on the network path.
         *
         * The one entry of the all-channel widening. Every admitted code and phase
         * attribute of a selected band becomes its own row at every station, which
         * subsumes the second code channel (@ref networkSecondCodeChannel) and the second phase channel
         * (@ref networkSecondPhaseChannel): their allow-lists are ignored and their
         * sigma scales apply to every non-canonical code row and non-canonical phase
         * row respectively.
         *
         * Restricted to the network path because the receiver inter-signal code
         * state, the satellite inter-signal bridging and the row census all live
         * there; a user-side `all` run keeps its existing behaviour.
         *
         * @return True when the widening is active.
         */
        bool networkChannelModeAll() const {
            return channel_mode_ == ChannelMode::All
                && processing_mode_ == ProcessingMode::PPP_RTK_Network;
        }

        /**
         * @brief Whether non-canonical code rows exist this run.
         *
         * True under the second code channel's own knob or under the phase-6b subsumption. Every
         * site that decides whether a companion code row can exist tests this
         * predicate rather than @ref networkSecondCodeChannel, so one predicate
         * answers for both mechanisms and the two cannot drift.
         *
         * @return True when a companion code row may be present.
         */
        bool networkSecondCodeRowsActive() const {
            return network_config_.second_code_channel || networkChannelModeAll();
        }

        /**
         * @brief Whether non-canonical phase rows exist this run.
         *
         * The phase twin of @ref networkSecondCodeRowsActive: the second phase channel's own
         * knob, or the phase-6b subsumption.
         *
         * @return True when a second-phase row may be present.
         */
        bool networkSecondPhaseRowsActive() const {
            return network_config_.second_phase_channel || networkChannelModeAll();
        }

        /**
         * @brief Whether the second phase channel is armed.
         *
         * `network.second_phase_channel`.  When true a network station contributes,
         * as ordinary float-ambiguity phase rows, every allow-listed band-1 carrier
         * phase it reports besides the one its canonical latch selected, so the
         * satellite L1C and L1W phase-bias families stop competing for the same
         * stations.  Default off and byte-identical; the loader refuses it with
         * `processing.channel_mode: all`, off the float vehicle, and with fewer than
         * two admitted entries in the allow-list.
         *
         * @return True when the second phase channel is armed.
         */
        bool networkSecondPhaseChannel() const {
            return network_config_.second_phase_channel;
        }

        /**
         * @brief The band-1 phase signals a station may add.
         *
         * `network.second_phase_signals`, validated at load: every entry is a
         * 3-character phase type on band 1.  Read by
         * @c PppModel::selectUncombinedSignals (which phases to add) and by the
         * roster's `modelled_extra` walk.  Meaningless unless
         * @ref networkSecondPhaseChannel.
         *
         * @return The allow-list, in configured order.
         */
        const std::vector<std::string>& networkSecondPhaseSignals() const {
            return network_config_.second_phase_signals;
        }

        /**
         * @brief Sigma multiplier for second-phase rows.
         *
         * Applied in @c Application::networkSplitStation_ to every phase row whose
         * (satellite, signal) cell the second phase channel opened at that station.  1.0 is the
         * default and is never applied (byte-identical); the loader refuses a value
         * below 1.  The closed-loop consumer is pinned to 1 with the knob itself.
         *
         * @return The multiplier; 1.0 (no scaling) by default.
         */
        double networkSecondPhaseSigmaScale() const {
            return network_config_.second_phase_sigma_scale;
        }

        /// @brief Latch a detector-marked arc's free epoch where the leveling-carry
        ///        inject can still see it (SPEC_slip_relevel_walk §6).  The
        ///        pending-reseed mark is consumed by the PASS-B measurement
        ///        pre-pass before the inject reads it, so without the latch the
        ///        stale pre-slip level injects at converged weight (the measured
        ///        cause of the level walk).  Default off; byte-identical off;
        ///        fingerprint-exempt (forward-only response policy).
        bool networkCycleSlipRelevelFreeEpoch() const { return network_cycle_slip_relevel_free_epoch_; }
        /// @brief Anchor-slip handover (docs/SPEC_event_harness.md §12.6/§12.7):
        ///        when a slip response frees the arc that currently anchors a
        ///        (sat, signal) phase-bias family, a one-epoch candidacy ban is
        ///        staged for that family, so the election held later in the same
        ///        epoch hands it to a successor at the successor's own carried
        ///        @c value_abs, before the freed arc's level is re-drawn from one
        ///        epoch of code.  Without it the sticky election keeps the slipped
        ///        incumbent and the family's gauge is re-realized from the freed
        ///        arc (on the harness, a 9 m step in the satellite's emitted clock
        ///        and a 10 m swing in the station's own clock, against 2 mm / 1 cm
        ///        with the handover).  Default off; byte-identical off;
        ///        fingerprint-exempt (forward-only response policy: it changes
        ///        only what a run does after a slip, never the state a snapshot
        ///        restores).
        bool networkCycleSlipAnchorSlipHandover() const { return network_config_.cycle_slip_anchor_slip_handover; }
        /// @brief MW evidence refloat: admit a strong, mature, bias-free-lane
        ///        Melbourne-Wubbena detection to the network's detect-only refloat
        ///        path regardless of integer cleanliness.
        ///
        /// Without it, PASS A admits an MW refloat candidate only on
        /// @code r.slip_wl_known && r.slip_wl_cycles != 0
        ///       && r.slip_frac <= turbo_edit_frac_max @endcode, the T0 integer
        /// repair gate, re-used on a path whose consumer (the refloat drain) never
        /// applies the integer.  How cleanly a step rounds says nothing about how
        /// far above the noise it stood: a -99 cycle L5 slip flagged at 78x its
        /// lane's threshold with the integer known but a 0.183 cycle rounding
        /// residual (1.07 m on the 5.86 m wide lane, below that lane's own
        /// 1 sigma) is dropped, as is the same event flagged on another lane with
        /// an unknown integer, with no counter and no event row.  Such misses can
        /// drive a station's ZWD error to metres.
        ///
        /// Armed, a candidate is also admitted when the detection itself is
        /// strong: a bias-free lane (@ref MwSlipResult::bia_corrected false; the
        /// BIA-corrected between-satellite lanes carry the detector's own MW-pivot
        /// S-transform re-references, the measured false-alarm population), an arc
        /// at least @ref networkCycleSlipMwEvidenceMinArcEpochs samples old, and a
        /// finite @ref MwSlipResult::evidence_ratio at or above
        /// @ref networkCycleSlipMwEvidenceRatioMin.  The refloat is unchanged
        /// (still both bands, still never applying an integer), and a candidate
        /// with an unknown integer also skips the delta-bridge intercept, so a
        /// zero integer can never be bridged into a carried level.
        ///
        /// Requires @ref networkCycleSlipMwDetect (inert without the detector; the
        /// loader disarms and warns).  Default off; byte-identical off.  The four
        /// admission counters are collected whether or not the knob is armed, so
        /// the run-end line of a run with it off already reports what the rule
        /// would have admitted.  Fingerprint-exempt (forward-only response policy).
        bool networkCycleSlipMwEvidenceRefloat() const { return network_config_.cycle_slip_mw_evidence_refloat; }
        /// @brief Evidence gate for @ref networkCycleSlipMwEvidenceRefloat: the
        ///        minimum |deviation| / threshold at the flag.  Default 3.0, the
        ///        value the false-alarm census was taken at (every mature
        ///        bias-free detection at or above it on a quiet 240-epoch
        ///        60-station leg was a real slip).
        /// @return Minimum evidence ratio, dimensionless.
        double networkCycleSlipMwEvidenceRatioMin() const { return network_config_.cycle_slip_mw_evidence_ratio_min; }
        /// @brief Evidence gate for @ref networkCycleSlipMwEvidenceRefloat: the
        ///        minimum arc sample count at the flag (arc maturity).  Default
        ///        10.  A young arc's level is still converging, so its deviation
        ///        is measured against a threshold that is itself unsettled; the
        ///        init-suspicious emission path reports 0 here by construction and
        ///        is therefore always refused.
        /// @return Minimum arc epochs, samples.
        int networkCycleSlipMwEvidenceMinArcEpochs() const { return network_config_.cycle_slip_mw_evidence_min_arc_epochs; }
        /// @brief Common-band scope: free only the band the evidence names
        ///        (`network.cycle_slip.common_band_scope`, SPEC_event_harness §14).
        ///
        /// A single-band slip on a triple-frequency arc fires every MW pair that
        /// contains the band (an L5 slip fires (1,5) and (2,5), leaving (1,2)
        /// silent), and each firing pair's drain refloats both of its bands, so
        /// the per-epoch union is all bands (the §9 all-band floor) for a slip
        /// that touched one.  Armed, one verdict is formed per (station,
        /// satellite, epoch) before any drain runs:
        ///
        ///  - a MW pair is silent iff it was evaluated this epoch, is mature
        ///    (arc count >= @ref networkCycleSlipCommonBandMinArcEpochs) and did
        ///    not flag (the detector's affirmative-cleanliness set);
        ///  - a silent pair whose IF-TD pair flagged this epoch cannot exonerate
        ///    (the equal-count trap: L1 and L2 slipping by the same integer leave
        ///    MW(1,2) silent while the iono-free combination moves 0.107 m/cyc);
        ///  - a band carrying a loss-of-lock flag is implicated regardless;
        ///  - exonerated(b) iff no LLI on b and some silent, un-vetoed pair
        ///    contains b;  implicated = tracked bands \\ exonerated.
        ///
        /// Every drain then frees (its own bands intersect implicated), so the
        /// per-epoch union is exactly `implicated` (Study 3's per-epoch
        /// invariant).  Three fallbacks return the unscoped response verbatim:
        /// no MW pair fired, nothing exonerated, or implicated empty although a
        /// pair fired (inconsistent evidence).
        ///
        /// Requires @ref networkCycleSlipMwDetect, because the exoneration
        /// evidence is the MW detector's clean-pair set (the loader disarms and
        /// warns without it).  Default off; byte-identical off (no verdict object
        /// is consulted).  Fingerprint-exempt (forward-only response policy).
        /// @return True when the common-band scope is armed.
        bool networkCycleSlipCommonBandScope() const { return network_config_.cycle_slip_common_band_scope; }
        /// @brief Exoneration maturity gate for @ref networkCycleSlipCommonBandScope:
        ///        the minimum MW arc sample count at which a silent pair may
        ///        exonerate its two bands.  Default 10.
        ///
        /// An immature arc could not have flagged at all, so its silence is not
        /// evidence; exonerating from it would keep a slipped band pinned.  The
        /// MW detector's own affirmative-cleanliness set already requires 10
        /// samples, so the default is inert by construction; a larger value
        /// tightens the rule, and the run-end `immature pairs` counter reports
        /// what it refused.
        /// @return Minimum MW arc epochs for exoneration, samples.
        int networkCycleSlipCommonBandMinArcEpochs() const { return network_config_.cycle_slip_common_band_min_arc_epochs; }
        /// @brief Reference-arc clause for @ref networkCycleSlipCommonBandScope
        ///        (`network.cycle_slip.common_band_ref_arc`, SPEC_event_harness §22).
        ///
        /// The scope's IF-TD veto refuses a silent MW pair only where the pooled
        /// IF-TD test flagged it, and on the chain's reference satellite that
        /// test cannot fire: `dd ≡ 0` by construction, so `|dd − med| > thr` is
        /// structurally false however large the slip, and the pair reaches the
        /// rule as a clean silence.  Armed, a pair whose IF-TD sample is the
        /// chain's reference is untested: refused as an exonerator (counted
        /// `ref-arc refusals`, named `iftd(<pair>)=ref_arc`, never listed as
        /// `silent`), and the verdict falls through to the all-band response
        /// when nothing else exonerates.  Applied to every chain's reference,
        /// including the first: the per-station reference gate is a separate
        /// instrument whose decision the preview does not see.
        ///
        /// Without it, an equal-count L1C+L5X slip injected on the reference
        /// satellite of both L5 chains was narrowed to the wrong band (L2W) under
        /// both `if_phase_td_pairs` settings, `first` and `all`, while the same
        /// injection on another satellite vetoed at 29× threshold (§20.1).  Cost:
        /// a real single-band slip on a reference arc also loses its narrowing
        /// when its only silent pair is the untested one.
        ///
        /// Requires @ref networkCycleSlipCommonBandScope (the loader disarms and
        /// warns without it).  Default off; byte-identical off.
        /// @return True when the reference-arc clause is armed.
        bool networkCycleSlipCommonBandRefArc() const { return network_config_.cycle_slip_common_band_ref_arc; }
        /// @brief Float per-band delta bridge at the MW/IF-TD drains: when the
        ///        slip's magnitude was measured (confident wide-lane integer +
        ///        IF-TD phase step), shift the carried level in place instead of
        ///        freeing the arc, so the level survives the slip at phase
        ///        precision.  Requires relevel_free_epoch (fallbacks land there),
        ///        mw_detect + if_phase_td; mutually exclusive with turbo_edit.
        bool networkCycleSlipRelevelDeltaBridge() const { return network_cycle_slip_relevel_delta_bridge_; }
        /// @brief Bridge reject: either |ΔN_band| above this (cycles) falls back
        ///        to the free epoch (detector-confusion guard).
        double networkCycleSlipRelevelBridgeMaxCycles() const { return network_cycle_slip_relevel_bridge_max_cycles_; }
        /// @brief Bridge reject: either propagated σ(ΔN_band) above this
        ///        (cycles) falls back to the free epoch.
        double networkCycleSlipRelevelBridgeMaxSigmaCyc() const { return network_cycle_slip_relevel_bridge_max_sigma_cyc_; }
        /// @brief Floor on the wide-lane integer's 1σ (cycles) entering the
        ///        bridge variance (the MW frac can read implausibly small).
        double networkCycleSlipRelevelBridgeWlSigmaFloorCyc() const { return network_cycle_slip_relevel_bridge_wl_sigma_floor_cyc_; }
        /// @brief Floor on the IF-TD step's 1σ (metres) entering the bridge
        ///        variance (the pooled scale can read implausibly small early).
        double networkCycleSlipRelevelBridgeIfSigmaFloorM() const { return network_cycle_slip_relevel_bridge_if_sigma_floor_m_; }
        /// @brief Bridge v2 corroboration gate (cycles per band): the bridged
        ///        level must agree with the interim free re-level within this.
        double networkCycleSlipRelevelBridgeCorrobCyc() const { return network_cycle_slip_relevel_bridge_corrob_cyc_; }
        /// @brief Bridge v2 pend expiry window (s) awaiting the MW integer.
        double networkCycleSlipRelevelBridgePendWindowS() const { return network_cycle_slip_relevel_bridge_pend_window_s_; }
        /// @brief Bridge v3, E4 (@c network.cycle_slip.relevel_bridge_gf): capture
        ///        the same-pair GF-phase TD at the flag epoch and let the kernel
        ///        close the (1,2) plane from (δIF, dGF) in the same epoch: the
        ///        float second equation, which does not wait for the MW integer and
        ///        carries zero integer-hypothesis risk.  σ is empirical (the
        ///        station's own clean-step EWMA), floored at
        ///        @ref networkCycleSlipRelevelBridgeGfSigmaFloorM.  Rider on
        ///        relevel_delta_bridge; default false is byte-identical.
        bool networkCycleSlipRelevelBridgeGf() const { return network_cycle_slip_relevel_bridge_gf_; }
        /// @brief Floor on the GF-TD step's 1σ (metres) entering the v3 kernel.
        double networkCycleSlipRelevelBridgeGfSigmaFloorM() const { return network_cycle_slip_relevel_bridge_gf_sigma_floor_m_; }
        /**
         * @brief Wired response to a wind-up accumulator restart
         *        (@c network.cycle_slip.windup_restart_refloat; SPEC_phase_windup_sign
         *        §10.4).
         *
         * An accumulated-mode wind-up restart applies an exact integer-cycle
         * step to every signal of the satellite while the carried ambiguity
         * and the zero-q leveling level survive at their pre-gap values: a
         * stiff carry with no other wired response (invariant #1), and
         * MW-blind (the step is band-common, ΔWL = 0).  When armed, the restart
         * ledger the accumulator fills is drained at PASS-A serial time and the
         * stepped (station, satellite)'s ambiguities take the free-epoch
         * refloat, with identities at both edges.  Default off; the loader
         * disarms it (warn) without @c estimation.accumulate_phase_windup
         * (the verdict does not exist in fractional mode, a structural zero)
         * or without @c network.phase_leveling (nothing stale to refloat).
         */
        bool networkCycleSlipWindupRestartRefloat() const { return network_cycle_slip_windup_restart_refloat_; }

        /**
         * @brief Whether the post-fit network RAIM backstop is on
         *        (@c network.raim; phase-leveling Increment 3, tier T2).
         *
         * The safety net for the zero-process-noise carry: the pre-processing slip
         * detectors (MW/GF TurboEdit, IF-phase triple-difference) are per-station and
         * threshold/pool-limited, so a real slip on a satellite that is setting (few
         * stations, under the pooled MAD count) can pass undetected, and with zero
         * process noise the stale carry then freezes into the products indefinitely.
         *
         * This runs after the joint solve and uses the network redundancy the
         * per-station detectors cannot see.  A bad carried ambiguity injects its
         * error into the shared satellite clock/phase-bias, where the faulty
         * station's own residual does not show it; every other station tracking that
         * satellite is pinned to its own (correct) carry and cannot absorb the shared
         * shift, so its post-fit phase residual jumps.  A cross-station consensus on
         * those standardized residuals (Baarda data-snooping, aggregated per
         * satellite) therefore detects the contamination the faulty station masks,
         * and refloats the offending carries (marks them pending-reseed, so the next
         * epoch re-estimates them from data: the same one-shot lifecycle the IF-phase
         * detector uses).
         * Meaningful only with @ref networkPhaseLeveling on.  Default false keeps the
         * carry solve byte-identical.
         */
        bool networkRaim() const { return network_raim_; }

        /**
         * @brief Whether a vanish-return pivot change signals a datum change
         *        (@c network.pivot_vanish_return_signal, A1).
         *
         * The adjacent-epoch pivot check misses a constellation that drops out for
         * one epoch and returns with a different pivot satellite: it reads as a clean
         * first-sighting, so the consumer is never told the shared solution
         * re-referenced.  When on, a persistent last-ever-pivot map catches that case
         * and forces the same discontinuity bump.  Default false = byte-identical
         * (only the adjacent-epoch change signals).
         */
        bool networkPivotVanishReturnSignal() const { return network_pivot_vanish_return_signal_; }

        /**
         * @brief Whether the network emits a per-satellite clock discontinuity
         *        channel and scopes the forced datum bump per constellation
         *        (@c network.emit_clock_disc, A2).
         *
         * When on, the emitter maintains a per-satellite clock discontinuity counter
         * (bumped when that satellite's constellation clock datum re-references) and
         * the forced phase/code/clock bump on a pivot handover is scoped to the
         * affected constellation(s) instead of every constellation.  Default false:
         * no clock disc channel and a global bump on any handover (byte-identical).
         *
         * @note Auto-enabled by the loader when @c network.ura_retro_bump is on (R4):
         *       the retro-bump's clock-counter step is a silent no-op without this
         *       channel, so arming the retro-bump arms the channel, and with it the
         *       per-constellation handover-bump scoping above.
         */
        bool networkEmitClockDisc() const { return network_emit_clock_disc_; }

        /**
         * @brief Whether the emitter's code-bias discontinuity counter also bumps
         *        on a fresh-baseline value jump beyond the phase disc threshold
         *        (@c network.code_bias_disc_jump_test).
         *
         * The H1 structural triggers (per-(const,signal) sum-to-zero set change,
         * per-sat IF-pin band change, forced datum bump) detect gauge
         * re-references only; a wire audit measured large unsignalled code-bias
         * value movement beyond them (weak-separation wander: 7.6k jumps > 0.05 m
         * against 2.8k signalled over 60 epochs at 305 stations).  This layers the
         * phase channel's magnitude jump test onto code, so whatever still moves
         * is signalled and the consumer re-inflates its receiver code-bias states
         * instead of dragging them.  Travels in the
         * S-contract (@c cbjump=) so file replay reproduces the counters.
         * Default false = structural triggers only (byte-identical).
         */
        bool networkCodeBiasDiscJumpTest() const { return network_code_bias_disc_jump_test_; }

        /**
         * @brief Per-λ discontinuity threshold as a fraction of a carrier cycle
         *        (@c network.disc_threshold_frac_cycles).
         *
         * When > 0 the emitter's phase (and, under @ref networkCodeBiasDiscJumpTest,
         * code) jump test uses a per-(sat,signal) threshold of @c frac·λ(sat,signal)
         * instead of the flat 0.05 m @c disc_threshold_m.  This is still a fixed
         * producer↔consumer contract (λ is a public, static band identity, so both
         * ends resolve the same threshold), but it makes the gate uniform in cycles
         * (the harm model) rather than metres.  Recommended 0.25 (L1/E1 4.76 cm ≈ the
         * legacy 0.05 m, L2 6.1 cm, L5/E5a 6.37 cm).  Travels in the S-contract
         * (@c discfrac=) so file replay reproduces the counters.  Default 0.0 = the
         * legacy flat-metre gate (byte-identical).
         */
        double networkDiscThresholdFracCycles() const { return network_disc_threshold_frac_cycles_; }

        /**
         * @brief Whether the emitter bumps a constellation's clock discontinuity
         *        counters when its clock-satellite set changes between emits
         *        (@c network.clock_disc_on_set_change).
         *
         * Under the sum-to-zero clock datum a membership change is a datum
         * re-reference (Σδtˢ=0 re-imposed on the new set redistributes the
         * departing/arriving common mode onto every survivor, measured up to
         * 45 cm/epoch at a 3-station roster), but the pivot-handover channel
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
         * The shared clock datum pins one satellite's clock per constellation
         * (@ref NetworkSbasis::buildSharedDatum).  The legacy auto-pick is the
         * lexically smallest registered satellite of the constellation, re-derived
         * every epoch, so every time a lexically lower satellite rises the pivot
         * flips: a constellation-wide datum re-reference that costs the consumer an
         * ambiguity reset.  When on, the incumbent pivot (the pivot of the last
         * emitted product containing that constellation) is kept as long as it is
         * still present in the epoch's registered clock satellites; a new pivot is
         * selected only when the incumbent is absent, and the new pivot then
         * becomes the sticky incumbent in turn (no flip-back when the old one
         * returns).  A config-pinned satellite (@c network.clock_datum_satellite)
         * has priority over the incumbent in both modes: pinning declares user
         * intent, so the pinned satellite reclaims the datum on its return (that
         * flip-back is deliberate and still signalled as a handover).  Default
         * false = the legacy per-epoch lexical selection (byte-identical).
         */
        bool networkStickyPivot() const { return network_sticky_pivot_; }

        /**
         * @brief Whether the per-constellation satellite-clock gauge is realized by
         *        a sum-to-zero over that constellation's satellite clocks instead of
         *        a hard pin on one pivot satellite (@c network.clock_datum_sum_to_zero).
         *
         * Legacy (off): the clock gauge for each constellation is fixed by pinning
         * one satellite's clock (the "pivot"; @ref NetworkSbasis::buildSharedDatum,
         * @ref networkStickyPivot).  It is anchor-based: a pivot that sets or is
         * deselected forces a constellation-wide datum re-reference (handover) that
         * costs the consumer an ambiguity reset, and an epoch missing every
         * candidate pivot has no datum and is skipped wholesale.
         *
         * When on, the gauge is the constellation-wide sum-to-zero Σ δtˢ = 0,
         * folded as a tight all-ones constraint row onto the shared normals before
         * the null-space gate, in the same way as the satellite code-bias
         * per-(constellation,signal) level is pinned (@c network.estimate_sat_code_bias).
         * Fully anchor-free: there is no pivot satellite and so no handover seam,
         * the clock_pivot map and shared hard-pin datum stay empty, and the datum is
         * defined for any epoch with ≥1 clock satellite per constellation.  The
         * emitted product self-describes the change via the S-basis contract's
         * @c isb_datum field (@ref SbasisDeclaration::IsbDatum::SumToZeroPerConstellation),
         * and a per-constellation @c clock_datum_satellite pin is ignored (there is
         * no pivot to place).  The two datums differ only by a per-constellation
         * constant a consumer absorbs into its receiver clock (a pure S-transform of
         * the estimable, between-satellite clock solution), so the change is
         * consumer-transparent for a float user and phase-bias/AR-frame-neutral.
         * Default false = the legacy per-constellation pivot pin (byte-identical).
         *
         * @see networkStickyPivot, NetworkSbasis, SbasisDeclaration::IsbDatum
         */
        bool networkClockDatumSumToZero() const { return network_clock_datum_sum_to_zero_; }

        /**
         * @brief Whether the satellite-clock gauge is realized by per-satellite
         *        broadcast anchors (@c network.clock_datum_broadcast_anchored,
         *        @c apriori_source=brdc_rts only).
         *
         * The real-time datum-stability contract: under @c brdc_rts the shared
         * clock column is Δδtˢ relative to each satellite's own broadcast clock,
         * so a soft per-satellite anchor toward 0 (σ =
         * @ref resolveNetworkAprioriClockAnchorSigmaM, auto 3.0 m) folded before
         * the null-space gate both regularizes cold start and realizes the
         * gauge, with no pivot pin and no sum-to-zero row.  Unlike the sum-to-zero
         * datum (whose reference is the raw per-epoch satellite set, measured to
         * redistribute ∓ΔC0/N onto the survivors as satellites rise/set), each
         * anchor is independent, so the emitted absolute clock frame does not
         * wander with membership; it stays bound to the broadcast reference the
         * consumer already reconstructs against.  The product self-describes
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
         * @brief Impose the W6 code-bias datum (per-satellite IF pin + per-
         *        (constellation,signal) sum-to-zero) as a time datum: pin the change
         *        from the previous epoch's solved values, not the level to zero
         *        (@c network.code_datum_time_linked).
         *
         * @par The problem it addresses
         * The legacy datum re-imposes, every epoch and at stiff weight over the
         * current satellite set with target zero, one IF-pin row per satellite
         * (fixing the clock↔code split) and one sum-to-zero row per
         * (constellation,signal) (fixing the channel level).  The same shared
         * columns carry the near-zero-@f$q@f$ code-bias prior, so on any rise/set
         * the sum-to-zero target shifts by (departed bias)/@f$m'@f$, the stiff carry
         * resists, and the level relaxes over many epochs toward the new target: a
         * hysteretic, unsignalled common-mode wander in the emitted OSBs that leaks
         * into the clock through the IF pin.  The disc counter also fires once at
         * the set change while the value drifts afterward, the worst ordering for a
         * consumer holding ambiguities.
         *
         * @par The fix: pin the change, not the level
         * When on, the same number of datum rows is imposed (rank unchanged, so the
         * null-space gate still passes), but their targets reference the previous
         * epoch's solved values.  The sum-to-zero row spans the present satellites
         * that also have a previous solved value ("veterans") with RHS
         * @f$\sum_{\text{vet}}\hat b_{s,k-1}@f$, pinning the change of the veteran
         * mean to zero; a channel with no veterans seeds at @f$\sum=0@f$, a real
         * re-reference that bumps its satellites' disc counters.  The IF-pin RHS
         * becomes @f$a_1\hat b_{\sigma_1,k-1}+a_2\hat b_{\sigma_2,k-1}@f$, seeded at
         * 0 on first appearance with a disc bump on any later re-seed.  The gauge
         * removes the same @f$1+m@f$ DOF per channel but as a time datum (the
         * Odijk-consistent treatment of CC-S set-dependence), so the target agrees
         * with the carry by construction and no relaxation dynamics exist.
         *
         * @par Bookkeeping
         * One map per (constellation,signal) → per-satellite previous solved value,
         * refreshed at the D2 harvest from the solved shared vector (kept epochs
         * only; a departed satellite is dropped, so its return re-seeds it alone).
         * The map is carried across a warm resume only when
         * @c network.state_snapshot_datum_capture writes it and
         * @ref networkResumeDatumRestore reads it; otherwise a warm resume re-seeds
         * with disc bumps (correct and signalled).  The toggle is in the snapshot
         * fingerprint (estimation-relevant).
         *
         * Meaningful only under @ref estimateNetworkSatCodeBias (the datum it
         * re-times only exists in W6); inert and warned otherwise.  Default false =
         * the legacy zero-target datum (byte-identical).
         *
         * @see estimateNetworkSatCodeBias, NetworkSbasis::codeS2zTimeLink,
         *      NetworkSbasis::selectIfPinPair
         */
        bool networkCodeDatumTimeLinked() const { return network_code_datum_time_linked_; }

        /**
         * @brief Continue the satellite clock across an ionosphere-free pin pair change,
         *        holding the old pair for the epoch an entering signal needs to become
         *        continuable (@c network.code_datum_pair_continuity).
         *
         * @par The problem it addresses
         * Under @ref networkCodeDatumTimeLinked the per-satellite IF pin carries its
         * previous solved combination only while the pin pair is unchanged.  The pair is
         * re-selected every epoch from the code signals registered at two or more
         * stations (@c NetworkSbasis::selectIfPinPair), so on a two-family Galileo roster
         * it moves between (C1C, C5Q), (C1C, C5X) and (C1X, C5X) as the pilot family's
         * coverage of the satellite comes and goes.  Each move re-seeds the pin at 0, steps
         * the emitted clock by the satellite's inter-family code bias, and marks the
         * satellite for a code and clock discontinuity; with @c network.emit_clock_disc the
         * emit site also declares the re-selection as a full-scope discontinuity
         * (the Galileo clock wire's steps fall at these changes).
         *
         * @par The fix: hold, then continue
         * 1. Hold.  At an ENTERING edge (a lower-ordered signal reaching two stations,
         *    e.g. C5Q beside a pinned (C1C, C5X) when the pilot family returns to E11 or
         *    E12) the entering signal has no previous solved value, so the change cannot
         *    be continued.  The satellite then keeps its last kept pair for that epoch,
         *    its pin carrying the held pair's own previous combination as for an
         *    unchanged pair (no pair change and no re-seed of the pin).  The entering
         *    signal is solved meanwhile, so at the next kept epoch the hold is released
         *    and the pin moves to the selection by step 2, without a clock step.  A hold
         *    therefore lasts one kept epoch per entering signal, and from then on the pin
         *    pair is again a function of the epoch's signal set (a lasting per-satellite
         *    split would drop the constellation's @c cbpin= declaration).  No hold when
         *    the held pair has no previous values (its pin would re-seed anyway), nor under
         *    @c network.sat_gf_datum broadcast_tgd when the group delay describes the
         *    selected pair and not the held one (the entering signal would anchor at the
         *    held pair's zero instead of the group delay); the change then re-seeds as
         *    without the arm.  Each held satellite-epoch is counted, with those a
         *    channel-level re-seed marked anyway, and the begin and the end of each hold
         *    are "CLK_BASIS"/"held" events.
         * 2. Continue.  At a pin pair change where both signals of the new pair have a
         *    previous solved value (the release of a hold, or a LEAVING edge: a pinned
         *    signal dropped below two stations), the pin's target is the new pair's
         *    previous solved IF combination a1·b̂(σ1') + a2·b̂(σ2').  Those values were
         *    realized against the old pin, so the target is the one that leaves the clock
         *    where it was.  The satellite is not marked for a re-seed, the emit site leaves
         *    the re-selection discontinuity undeclared when its last emitted pair is the
         *    pair continued from, and the change is logged as a "CLK_BASIS"/"continued"
         *    event and counted once per kept epoch.
         * A new pair without previous values and without a hold (a held signal left in
         * the same epoch a signal entered, or one of the refusals above) re-seeds as
         * without the arm.
         *
         * Both steps keep the clock only when every other row on the satellite's code-bias
         * columns also carries its previous value: no geometry-free datum, or
         * @c network.sat_gf_datum with @c network.sat_gf_datum_time_linked
         * (@c NetworkSbasis::pinPairContinuityBlocker; otherwise the arm is inert with a
         * warning, because those rows re-assert an absolute target on the new pair and
         * the step would go out undeclared).  The geometry-free pair and extra rows
         * follow the pinned (held) pair.  A channel-level re-seed (the sum-to-zero or
         * family-difference row of a channel with no veteran) still marks the satellites
         * it re-references, held or continued ones included, and that mark is declared.
         *
         * Read only under @ref networkCodeDatumTimeLinked (and the W6 satellite code
         * bias it re-times); inert with a warning otherwise.  Changes the realized datum,
         * so it is in the snapshot fingerprint when armed (fresh convergence only).
         * Unvalidated (pre-registered).  Default false: the selection every
         * epoch and the re-seed at every pair change (byte-identical).
         *
         * @see networkCodeDatumFamilyDifference, NetworkSbasis::buildCodeBiasDatumRows
         */
        bool networkCodeDatumPairContinuity() const { return network_code_datum_pair_continuity_; }

        /**
         * @brief Realize the inter-family code offset as a family difference instead of
         *        a sum-to-zero (@c network.code_datum_family_difference).
         *
         * @par The problem it addresses
         * With no receiver tracking both of a constellation's channel families (Galileo
         * pilot C1C/C5Q against combined C1X/C5X), the offset between the families is a
         * datum quantity.  Today the time-linked sum-to-zero row of the combined channel
         * (E|C1X) realizes it, seeded at 0 over the satellites present at ignition, and
         * the realization differs between satellites first pinned on one family and on
         * the other (C1X minus C1C about +1.1 m on pilot-born
         * satellites, about 0 on combined-born ones; COD puts it at 0.000 m mean).
         *
         * @par The fix
         * For a secondary-family (attribute X) Galileo code channel whose sum-to-zero row
         * is not suppressed by the geometry-free datum and that has a primary-family
         * channel on the same band, the row is replaced by Σ (d_X − d_P) over the
         * satellites carrying both channels, the inter-family mean zero of COD's
         * convention.  The partner is the same-band primary channel with the most
         * satellites in common (ties: the lexically first signal).  The time link is the
         * sum-to-zero one: veterans (previous solved values on both columns) carry Σ of
         * their previous differences; a row with no veteran re-seeds at 0, with the
         * channel's satellites seen before marked for a discontinuity, but only when the
         * channel's own sum-to-zero time link would re-seed too (otherwise the sum-to-zero
         * row carries the level for that epoch and the family row continues from it).
         * The sum-to-zero row is also kept with no satellite carrying both channels, and
         * when every satellite the family row would span has its X column pinned by a
         * geometry-free row or an extra-band hold (the family row would then add no rank).
         * Every outcome is counted per band, and every switch between the two rows is a
         * "CB_DATUM"/"family_row" event.  Galileo only: the evidence and COD's
         * convention are Galileo's; a GPS or BeiDou X channel keeps its sum-to-zero row.
         *
         * Read only under @ref networkCodeDatumTimeLinked (and the W6 satellite code
         * bias it re-times); inert with a warning otherwise.  Changes the realized datum,
         * so it is in the snapshot fingerprint when armed (fresh convergence only).
         * Unvalidated (pre-registered).  Default false: the sum-to-zero row
         * (byte-identical).
         *
         * @see networkCodeDatumPairContinuity, NetworkSbasis::channelFamilyOf
         */
        bool networkCodeDatumFamilyDifference() const { return network_code_datum_family_difference_; }

        /**
         * @brief Standard deviation, in metres, of the first-sighting family prior
         *        (@c network.code_family_birth_prior_sigma_m); 0 is off.
         *
         * @par The problem it addresses
         * With the family-difference datum (@ref networkCodeDatumFamilyDifference) the
         * inter-family code offset has one realization, but a secondary-family (X) column
         * born beside an uncertain satellite clock still absorbs part of that clock error
         * (in ionosphere-free terms all of it), and with the stiff satellite code-bias
         * process noise it keeps it: the data that would pull it back arrive only through
         * the code of the other family's stations
         * (1 to 2.4 m of ionosphere-free code error on the hemisphere-split sim
         * satellites that never changed pin pair).
         *
         * @par The prior
         * For Galileo band 1 (the band the family-difference row acts on; the partner is
         * the step-1 choice, the same-band primary-family channel with the most satellites
         * in common), at an epoch where a satellite's secondary (X) column or its partner
         * column is born (the carry does not continue it: no carried value, a park past
         * the seed horizon, or a clock-monitor reinit) while the other column of that
         * satellite is registered too, one soft shared row
         * @f$d_X - d_P = \mu@f$ is folded with information @f$1/\sigma^2@f$.
         * @f$\mu@f$ is the mean of @f$(\hat d_X - \hat d_P)_{k-1}@f$ over the veterans of
         * the family row (the satellites carrying both columns this epoch with a previous
         * value on both); under the family row on veterans it is the realized family mean
         * the row holds.  With no veteran it is 0 only where the family row re-seeds the
         * offset at 0; a birth on a channel whose level the sum-to-zero frame carries
         * instead (bridged, or dependent with no veteran) has no realized family mean to
         * target and folds no row (counted).  The row is folded once per (satellite,
         * birth): the birth test reads the carry, so the born column is carried from the
         * next kept epoch on and gets no second row (a per-epoch soft row on a carried
         * state would accumulate into a hard one), and a return inside
         * @c network.readmit_seed_horizon_s, which READMIT seeds from the column's parked
         * pre-gap carry, is not a birth (counted).  A return past the horizon is.  A
         * withheld epoch advances no carry, so its row is folded again at the next epoch,
         * which is not a second fold of the same information.  Each fold is counted per
         * (constellation, band) and declared as a "CB_DATUM" / "family_birth_prior" event
         * once per kept epoch.  A sigma at or below about 0.032 m (information at or above
         * the code-bias gauge weight 1e3) is warned at load, and one below 0.01 m refused:
         * the prior would compete with the datum rows it refines.
         *
         * Read only with @ref networkCodeDatumFamilyDifference under
         * @ref networkCodeDatumTimeLinked (and the W6 satellite code bias); inert with a
         * warning otherwise.  Changes the realized code biases, so it is in the snapshot
         * fingerprint when armed (fresh convergence only).  Unvalidated (
         * pre-registered).  Default 0: no row (byte-identical).  A negative, non-finite or
         * sub-centimetre value is refused at load.
         *
         * @see networkCodeDatumFamilyDifference, NetworkSbasis::codeFamilyBirthPriors
         */
        double networkCodeFamilyBirthPriorSigmaM() const { return network_code_family_birth_prior_sigma_m_; }

        /**
         * @brief Whether the satellite phase-bias gauge is realized by a
         *        per-(satellite,signal) prioritized anchor receiver instead of the
         *        single clock-pivot station (@c network.phase_datum_prioritized).
         *
         * Legacy (off): the phase-bias↔ambiguity gauge for every emitted
         * (satellite,signal) is pinned on the one clock-datum station's ambiguity
         * (@c is_pivot, @ref NetworkSbasis header, Odijk Eq. 4), and the emittable
         * phase-bias set is capped at exactly that station's tracked signals.  This
         * has two consequences for the consumer: (a) when the pivot loses a
         * low-elevation satellite, that satellite's phase bias vanishes from the
         * product and its gauge re-references on re-acquisition (a gauge jump); and
         * (b) an epoch in which the pivot is absent has no datum at all and is
         * skipped wholesale (a single point of failure).
         *
         * When on, the gauge is still a minimum constraint (exactly one ambiguity
         * pin per (satellite,signal) phase-bias freedom), but each such pin is
         * placed at the highest-priority receiver that currently tracks that
         * satellite with a usable ambiguity, decoupled from the clock pivot.  Per
         * constellation the candidate receivers are ranked by descending coverage
         * (satellites tracked above @ref networkPhaseDatumPrefElevDeg), so the
         * majority of a constellation stays anchored to one high-coverage receiver
         * (frame-consistent) and only a satellite that receiver cannot see falls
         * back to the next.  Each such per-(satellite,signal) anchor handover is
         * signalled to the consumer as a discontinuity (@c markSatDiscontinuity), so
         * the emitted bias stays continuous or signalled rather than jumping
         * silently.  The clock datum (one satellite-clock pin per constellation) is
         * independent and unchanged.  Default false = the legacy single-pivot pins
         * (byte-identical product + traces).
         *
         * @see networkPhaseDatumPrefElevDeg, networkPhaseDatumSticky
         */
        bool networkPhaseDatumPrioritized() const { return network_phase_datum_prioritized_; }

        /**
         * @brief Whether the satellite phase-bias↔ambiguity gauge is realized by
         *        the Eq-34 / de Jonge single-receiver-chain datum
         *        (@c network.phase_datum_eq34).
         *
         * The master toggle for the only datum that keeps the emitted
         * between-satellite phase-bias reference integer-consistent across all
         * satellites while staying full-rank at global scale.  It gives the
         * receiver fractional phase bias its own estimable state (@ref
         * StateKeys::makeReceiverPhaseBiasKey), so it cannot lump into the
         * emitted satellite phase bias, then fixes the enlarged gauge with one
         * receiver datum pin + a spanning-tree of ambiguity pins over the
         * bipartite (receiver × satellite) observation graph (per
         * constellation×signal connected component).  Then every satellite pair's
         * reference difference is an integer combination, including satellites
         * the datum receiver never sees (each chain hop contributes only an
         * integer, because the hop receiver's fractional bias is a separate
         * estimable state, not part of the pin).  Reuses the prioritized machinery
         * that removes the single point of failure (no pivot-first, every station
         * carries its arcs) but replaces the anchor election with a chain election;
         * supersedes @ref networkPhaseDatumPrioritized.  Default false = the
         * prioritized-anchor / legacy pin datum (byte-identical: no RPB state is ever
         * allocated, the chain election never runs).
         *
         * @see networkPhaseDatumEq34Receiver, networkPhaseDatumEq34PinRound,
         *      networkPhaseDatumPrioritized
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
         * handovers).  Inert unless @ref networkPhaseDatumEq34.
         */
        const std::string& networkPhaseDatumEq34Receiver() const {
            return network_phase_datum_eq34_receiver_;
        }

        /**
         * @brief Whether an eq34 re-pin of an already-lit (satellite,signal) edge
         *        targets the rounded carried float (@c network.phase_datum_eq34_pin_round).
         *
         * Any integer target is a valid gauge; on a re-attach / re-pin,
         * true (default) selects the integer λ·round(x̂_prev/λ) of the new edge
         * station's carried float so the emitted-bias frame step is ≈ 0 cycles
         * (typically no wire discontinuity once the floats are consistent); false
         * always pins to 0 (an A/B lever: a legal but usually non-zero-step
         * frame).  Inert unless @ref networkPhaseDatumEq34.
         *
         * @see networkPhaseDatumEq34
         */
        bool networkPhaseDatumEq34PinRound() const { return network_phase_datum_eq34_pin_round_; }

        /**
         * @brief Seed-round: wherever an eq34 pin target would otherwise be
         *        the cold absolute 0 (a first-sighting edge, an incumbent whose
         *        phase-leveling carry is dead, or every epoch under @c carry_invariant,
         *        where pin-to-round is disabled by design), target λ·round(x/λ) of the
         *        value the ambiguity state holds.  This bounds the injected pin
         *        tension at λ/2 per pin instead of the full phase−code lump (RINEX
         *        guarantees no alignment; the synthetic fixture draws
         *        N ∈ [1e5,1e6) cycles and without this pays a km-scale GF discharge;
         *        SPEC_synthetic_minimum_network §12.8).  Default off; refused without
         *        @c network.phase_datum_eq34.
         */
        bool networkPhaseDatumEq34SeedRound() const { return network_phase_datum_eq34_seed_round_; }

        /**
         * @brief Frame-continuous re-host: when a spanning-tree pin moves to a new
         *        host station and no phase-leveling carry exists there, compute the
         *        frame step from the host's own ambiguity state (the exact pre-pin
         *        float) and S-transform the carried quantities into the new frame,
         *        instead of declaring the step unknown and re-referencing the (sat,σ).
         *
         * INC-1 freezes the target of a pin that stays put; without this knob a
         * re-host realizes a fresh frame at the new station, and an unknown step
         * marginalizes that satellite's carried phase-bias prior for the epoch
         * (D4 hygiene), about once per epoch on a 60-station triple-frequency
         * arm (suspect (a) of the §8.5 divergence).  The step is computable under
         * seed-round: the pin displaces the new host's float from @c x to
         * @c target, so Δb_s = −(target − x) exactly, which is what the existing
         * v2-shift walk applies.
         *
         * Distinct from @ref networkPhaseDatumEq34CarryShift, whose field harm came
         * from stepping the frame by a drifted phase-leveling carry: here the pre-pin
         * value is the estimator's own state, so that drift term is structurally
         * absent.  Still default off and A/B-graded: the residual risk (the solve
         * spreads the pin displacement across the weakly held δ split rather than into
         * b_s alone) is the root that remains open, and the truth-graded synthetic
         * arm is where it can be decided.
         *
         * Default off; refused without @c network.phase_datum_eq34_seed_round.
         *
         * @see networkPhaseDatumEq34SeedRound, networkPhaseDatumEq34CarryShift
         */
        bool networkPhaseDatumEq34SeedRoundRehostFrame() const { return network_phase_datum_eq34_seed_round_rehost_frame_; }

        /**
         * @brief Loose zero-mean prior σ (metres) applied per epoch to every
         *        receiver phase-bias (RPB) column, in its station's own block
         *        (@c network.phase_datum_eq34_rpb_prior_sigma_m).
         *
         * The chain pins fix the per-(constellation,signal) gauge exactly, but the
         * estimable RPB↔ambiguity↔ionosphere separation inside a station block can
         * be nearly rank-deficient (few tracked satellites / one signal): without an
         * information floor the W5 observability gate withholds epochs, or, under
         * @ref networkStationPdPinAware, the near-singular block stays in and
         * degrades the shared solve (a field A/B measured the datum-invariant DD step
         * rising from 0.085 to 36–118 cyc/epoch).  This is a soft floor (weight
         * 1/σ²) on an already-estimable quantity, not a second datum: the gauge is
         * fixed by the pins, and the pull toward 0 is far below one phase row's
         * weight at the default.  0 disables.  Default 1.0 m, loose against dm-level
         * physical receiver phase biases (GLONASS local-anchor precedent).  Inert unless
         * @ref networkPhaseDatumEq34.
         *
         * @see networkPhaseDatumEq34, networkStationPdPinAware
         */
        double networkPhaseDatumEq34RpbPriorSigmaM() const { return network_phase_datum_eq34_rpb_prior_sigma_m_; }

        /**
         * @brief Process noise q (m²/s) of the local receiver phase-bias
         *        time-linking carry (@c network.phase_datum_eq34_rpb_q_m2s).
         *
         * Without the carry (v1), δ_{r,σ} is re-estimated fresh each epoch; because
         * its observability inside a station block is weak, the per-epoch estimate
         * bounces, and the bounce enters every individual ambiguity float
         * anti-correlated (DDs cancel it; the per-key phase-leveling carry does
         * not).  This is the spec's D6-v2 trigger condition, met in a field A/B
         * (datum-invariant DD step 0.085 → 10–100 cyc/epoch).  The carry folds a
         * per-epoch prior toward the previous δ̂_{r,σ} with variance
         * @c var + q·gap: the minimal local realization of D6 v2 (no shared-border
         * promotion, no coupling-row footprint).  Default 3e-6 m²/s admits ~0.1 m/h
         * physical receiver-bias drift while suppressing dm-level per-epoch bounce.
         * 0 disables (v1 behaviour).  Inert unless @ref networkPhaseDatumEq34.
         *
         * @see networkPhaseDatumEq34, networkPhaseDatumEq34RpbPriorSigmaM
         */
        double networkPhaseDatumEq34RpbQ() const { return network_phase_datum_eq34_rpb_q_m2s_; }

        /**
         * @brief Experimental: on an eq34 pin displacement, S-transform the
         *        carried values into the new frame instead of the erase/skip
         *        hygiene (@c network.phase_datum_eq34_carry_shift).
         *
         * Theoretically exact in the chain algebra (Δb_s / Δδ_r propagate down the
         * tree with alternating sign; local, RPB, and shared-PB carries shift by
         * their exact frame moves, variances untouched).  Field result (overnight
         * A/B): the realized redistribution in the full model differs from the
         * ideal chain algebra, and the shift injected error (DD step 86 against
         * 11–15 cyc/epoch for erase-based hygiene); root cause open (suspect: the
         * weakly held δ_{r,σ} split absorbing part of the pin move that the walk
         * attributes to b_s).  Default false = erase-based hygiene.  Inert unless
         * @ref networkPhaseDatumEq34.
         */
        bool networkPhaseDatumEq34CarryShift() const { return network_phase_datum_eq34_carry_shift_; }

        /**
         * @brief Longevity-aware BFS growth of the eq34 spanning tree
         *        (@c network.phase_datum_eq34_longevity_election).
         *
         * Spec D2 step 3 grows the spanning tree from r1 "preferring high-coverage
         * stations and high-elevation arcs"; the sticky two-queue BFS build does not
         * apply that preference, so without this knob a new tree edge is chosen by
         * lexical sat/station order, regardless of how long the arc will survive.
         * A tree edge that dies (its arc sets) forces a b_s re-reference and a carry
         * erase; picking the longest-lived available edge reduces that churn at the
         * source (an interim mitigation in the spirit of de Jonge, complementary to
         * the extend-only membership item).  When on, the new-edge frontier of the
         * BFS (the incumbent-edge seed is untouched) is ordered by arc longevity: a
         * rising arc (elevation increased since the last epoch, so it will not set
         * soon) is lifted above every setting arc, and within a class higher
         * elevation wins; receiver choice prefers high per-constellation coverage
         * first (the more robust tracker).  Ties break deterministically
         * (sat name / station index) as in the legacy order.  Pure edge-selection
         * change: the gauge dimension, pins, and every emitted value are unchanged
         * for any tree it happens to reproduce.  Default false = legacy lexical
         * growth (the B9 baseline, for a clean reref-counter A/B).  Inert unless
         * @ref networkPhaseDatumEq34.
         *
         * @see networkPhaseDatumEq34
         */
        bool networkPhaseDatumEq34LongevityElection() const {
            return network_phase_datum_eq34_longevity_election_;
        }

        /**
         * @brief Extend-only sticky-tree membership across momentary row gaps
         *        (@c network.phase_datum_eq34_extend_only, sibling of
         *        @c network.phase_datum_eq34_longevity_election).
         *
         * de Jonge §3.5: the S-basis is extend-only over time; a basis edge is
         * removed only on a true arc death, never on a per-epoch usability flap.
         * In this running filter that is realized by retaining a tree edge's sticky
         * identity (its sat-side / receiver-side parent, component root, and pinned
         * integer) while its phase-leveling memory (@c local_carry) is still alive,
         * so the epoch its arc resumes it is a recognized incumbent that re-pins to
         * the same integer (emitted-bias step ≈ 0, DDs continuous) instead of
         * re-leveling from a first-sighting 0.  That re-leveling would be a silent
         * integer re-reference that corrupts every peer station's surviving carry
         * for that satellite.  @c local_carry liveness is the age stamp: it is
         * gap-pruned at @c network.phase_leveling_max_gap_s, the same horizon that
         * bounds the carries the whole datum rides on, so an edge absent longer than
         * that (or whose carry was erased by a real reroute/handover) is not
         * retained and re-floats as a first sighting, by which point the peer
         * carries are gone too.  Tree growth is unchanged (the seed admits live
         * arcs only, so a retained but absent edge is inert until its arc returns
         * and the spanning-forest invariant holds); reroute/handover churn is out
         * of scope.  Default false = legacy wholesale replace (the B9
         * baseline).  Inert unless @ref networkPhaseDatumEq34.
         *
         * @see networkPhaseDatumEq34, networkPhaseLevelingMaxGapS
         */
        bool networkPhaseDatumEq34ExtendOnly() const {
            return network_phase_datum_eq34_extend_only_;
        }

        /// Time-link the DD cycle-basis invariant carry (@c network.carry_invariant).
        bool networkCarryInvariant() const { return network_carry_invariant_; }
        /// Z process noise m²/s (@c network.carry_invariant_qz).
        double networkCarryInvariantQz() const { return network_carry_invariant_qz_; }
        /// Emit-layer frame re-basing (@c network.carry_invariant_emit_rebase).
        bool networkCarryInvariantEmitRebase() const { return network_carry_invariant_emit_rebase_; }
        /// Estimation-side fine-slip innovation gate (@c network.carry_invariant_innov_gate).
        bool networkCarryInvariantInnovGate() const { return network_carry_invariant_innov_gate_; }
        /// Innovation-gate per-chord standardized threshold (@c network.carry_invariant_innov_k).
        double networkCarryInvariantInnovK() const { return network_carry_invariant_innov_k_; }
        /// LS-VCE-calibrated innovation gate (@c network.carry_invariant_innov_calibrate).
        bool networkCarryInvariantInnovCalibrate() const { return network_carry_invariant_innov_calibrate_; }
        /// σ̂₀² forgetting half-life in epochs (@c network.carry_invariant_innov_calibrate_window).
        double networkCarryInvariantInnovCalibrateWindow() const { return network_carry_invariant_innov_calibrate_window_; }
        /// Require raw-observable corroboration before a D5 re-tap (@c network.carry_invariant_innov_corroborate).
        bool networkCarryInvariantInnovCorroborate() const { return network_carry_invariant_innov_corroborate_; }
        /// Magnitude backstop for the corroboration gate (@c network.carry_invariant_innov_corroborate_backstop_k).
        double networkCarryInvariantInnovCorroborateBackstopK() const { return network_carry_invariant_innov_corroborate_backstop_k_; }
        /// A D5 flag re-taps its chord in-epoch only and does not also refloat the arc next epoch (@c network.carry_invariant_innov_retap_only).
        bool networkCarryInvariantInnovRetapOnly() const { return network_carry_invariant_innov_retap_only_; }
        /// Gate only chords carried into the epoch; a born chord has no H0 to test (@c network.carry_invariant_innov_carried_only).
        bool networkCarryInvariantInnovCarriedOnly() const { return network_carry_invariant_innov_carried_only_; }
        /// PD-safe DD fold: scale-following posterior/factorization, counted repair, and bounded degenerate-block rebirth (@c network.carry_invariant_pd_safe).
        bool networkCarryInvariantPdSafe() const { return network_carry_invariant_pd_safe_; }
        /// Carry-aware covariance readback: dd_sigma_cyc and the AR probe read Cov(x_r) from the model the solve used (@c network.carry_aware_covariance).
        bool networkCarryAwareCovariance() const { return network_carry_aware_covariance_; }
        /// Keep this epoch's slipped arcs out of the eq34 spanning tree, so a slip costs one chord, not every cycle through a tree edge (@c network.phase_datum_eq34_slip_aware_election).
        bool networkPhaseDatumEq34SlipAwareElection() const { return network_phase_datum_eq34_slip_aware_election_; }

        /// Emit the satellite clocks in a per-constellation held frame:
        /// the per-epoch common-mode innovation (median vs the previous
        /// emitted values) is accumulated into a frame offset that never re-enters
        /// the wire.  Outputs-only (solve byte-identical); the removed
        /// direction is the clock-ensemble gauge a consumer's per-epoch receiver
        /// clock absorbs, and between-satellite differences are preserved exactly.
        bool networkEmitClockFrameHold() const { return network_emit_clock_frame_hold_; }
        /// |frame accumulator| bound (m) beyond which the constellation re-anchors
        /// to the solve frame with a datum mark (markDatumChange): the marked
        /// re-coupling edge (invariant #7).
        double networkClockFrameLeashM() const { return network_clock_frame_leash_m_; }
        /// Mark per-satellite common-removed clock steps beyond
        /// networkClockDiscJumpM() via the markSatDiscontinuity retro-bump (R record
        /// + per-sat counters, the proven consumer reset chain).
        /// Independent of the frame hold: alone it marks the raw wire with the
        /// census's own statistic.  The emitter's clock counter keeps no internal
        /// magnitude test; this test lives at the emission-prep tier.
        bool networkClockDiscJumpTest() const { return network_clock_disc_jump_test_; }
        /// Flat per-sat step threshold (m).  Empirical
        /// default 1.5 ≈ 16 σ of the measured per-epoch walk step (SPEC
        /// clock_emit_rebase §1): ~7 marks/h on the reference internal lineage;
        /// the census outlier floor (0.5 m) would mark ~10× as often.
        double networkClockDiscJumpM() const { return network_clock_disc_jump_m_; }
        /// On a return after more than max_gap_s, test the common-removed step
        /// against the retained pre-gap baseline with this threshold (m)
        /// (@c network.clock_disc_gap_jump_m).  0 (default)
        /// = off: a stale return stays an untested first sighting, which leaves
        /// eclipse re-admissions unmarked (SPEC_synthetic_minimum_network §18.1:
        /// 6.3–6.7 m clock↔C5Q split re-realizations with zero CLK_STEP marks).  On
        /// Q3-armed postures cta_disc_landed already marks those returns; this knob
        /// covers postures without clock_temporal_anchor + clock_anchor_mark (the
        /// current goldens arm both).  Inert without networkClockDiscJumpTest().
        double networkClockDiscGapJumpM() const { return network_clock_disc_gap_jump_m_; }

        /// @name Guarded temporal clock anchor
        /// Without the anchor, the per-satellite clock's D2
        /// carry folds at @c filter.process_noise.sat_clock_q, whose one-epoch
        /// spread √(q·Δt) is above the realized spread, so the prior never binds
        /// and the emitted clock is re-drawn from each epoch's data (measured robust
        /// σ_w 9.23 cm/30 s, uniform to 1.37× across a 32-satellite fleet of three
        /// clock technologies, against ~2 cm of physical change).  The anchor gives
        /// that estimable content temporal stiffness together with the event
        /// response that stiffness requires (invariant #1).
        /// Solve-affecting and fingerprinted: fresh convergence only.
        /// @{
        /// Arm the anchor.  Off (default) ⇒ the clock carry is byte-identical to
        /// the legacy fold and none of the per-satellite state is allocated.
        bool   networkClockTemporalAnchor() const { return network_clock_temporal_anchor_; }
        /// The anchor's random-walk rate (m²/s).  The emitted per-satellite walk
        /// is √(q·Δt) once the carry binds (an exact steady-state identity), so
        /// this knob is the walk target: 1e-4 → 5.5 cm, 3e-5 → 3.0, 1e-5 → 1.7.
        double networkClockAnchorQ() const { return network_clock_anchor_q_; }
        /// Consistency floor on the folded carry variance (m²), the clock
        /// analogue of the B4 @c phase_bias_var_floor / @c code_bias_var_floor.
        /// Caps the folded prior information at 1/floor so an independence-
        /// assuming carry cannot accumulate unbounded information against a
        /// time-correlated error floor (assessment deviation #1, ranked-plan
        /// item 3).  0 = no floor.
        double networkClockAnchorVarFloorM2() const { return network_clock_anchor_var_floor_m2_; }
        /// Per-satellite staleness bound (s).  Beyond it the carried clock is not
        /// folded at the anchor; the satellite decouples.  Load-bearing: the
        /// driver's @c dt is the inter-epoch gap and every skip path advances it
        /// while the carry survives, so without a per-satellite elapsed time a
        /// stiff prior would fold an arbitrarily old value as if it were 30 s old.
        double networkClockAnchorMaxGapS() const { return network_clock_anchor_max_gap_s_; }
        /// Trip threshold on |w|, the standardized generalized predicted residual
        /// (posterior − prior, common-mode removed, over √(P⁻ − P⁺)).  Empirical:
        /// read off the measured prior-residual ladder, never from the stochastic
        /// model.
        double networkClockAnchorK() const { return network_clock_anchor_k_; }
        /// Absolute backstop on |ê| (m): trips even if the standardizing variance
        /// were mis-set.  0 = off.
        double networkClockAnchorAbsM() const { return network_clock_anchor_abs_m_; }
        /// Floor on √S (m) so a weak-data epoch (where the data removed no
        /// information, P⁺ → P⁻) yields w ≈ 0 instead of a 0/0 blow-up that would
        /// false-trip the lowest-confidence satellites.  Same reasoning as the
        /// two-state model's B3 @c clock_model_meas_std_m floor.
        double networkClockAnchorSigmaFloorM() const { return network_clock_anchor_sigma_floor_m_; }

        /**
         * @brief Space of the Q3 anchor guard's statistic
         *        (@c network.clock_anchor_guard_space: @c clock (default), @c phase_shadow,
         *        @c phase).
         *
         * The anchor binds the satellite clock's walk, which the phase sees; the level
         * (the clock and every phase bias moved together) is seen by the code only and
         * converges at code rate once the coupling rows' time update is consistent.  The
         * clock statistic mixes the two and trips on the level's own motion.  @c phase
         * tests the ionosphere-free phase clock instead, d = c - sum_j w_j p_j with
         * sum w_j = 1 (blind to the level) and sum w_j lambda_j^2 = 0 (blind to the
         * satellite's ionosphere direction), against its own P- - P+ projected on the
         * constellation contrast and floored at @ref networkClockAnchorPhaseSigmaFloorM.
         * The absolute backstop stays on the clock.  @c phase_shadow computes, ladders and
         * traces the phase statistic while the clock statistic decides (solve
         * byte-identical).  @c clock = byte-identical.
         * @return 0 clock, 1 phase_shadow, 2 phase.
         */
        int    networkClockAnchorGuardSpace() const { return network_clock_anchor_guard_space_; }

        /**
         * @brief Floor on the phase-space statistic's sqrt(S) (m),
         *        @c network.clock_anchor_phase_sigma_floor_m (default 0.02).
         *        Set from the @c phase_shadow ladder.
         */
        double networkClockAnchorPhaseSigmaFloorM() const { return network_clock_anchor_phase_sigma_floor_m_; }
        /// Consecutive in-band epochs a decoupled satellite must serve before the
        /// anchor re-engages: the re-coupling re-validation of invariant #7.
        int    networkClockAnchorRecoupleEpochs() const { return network_clock_anchor_recouple_epochs_; }
        /// Wire-mark the decoupling edge (per-satellite discontinuity through the
        /// B1 retro-bump chain: R record + phase/code/clock counters).  A satellite
        /// whose clock just changed both temporal regime and value must not be
        /// integrated across.
        bool   networkClockAnchorMark() const { return network_clock_anchor_mark_; }
        /// Adapt-and-emit: on a trip, withdraw the offending prior and re-solve the
        /// epoch before emission, so the trip epoch publishes the data's value
        /// rather than the stale anchor's.  The re-solve reuses the already-reduced
        /// normals (no re-reduction, no re-assembly).  Off degrades to
        /// detect + mark + decouple-next-epoch, which is correct but ships one
        /// anchor-distorted epoch per event.
        bool   networkClockAnchorResolve() const { return network_clock_anchor_resolve_; }
        /// Fold the clock prior gauge-free: marginalize the
        /// per-constellation satellite-clock common direction out of the folded
        /// information (rank-1 downdate Λ ← Λ − Λu(uᵀΛu)⁻¹uᵀΛ), so the anchor
        /// constrains only the estimable between-satellite differences and leaves
        /// the common mode entirely to the S-basis datum.  The motive is measured:
        /// over 3 h the plain anchor reduced CLK-R's drained common mode Σ|μ| from
        /// 49.4 m to 27.8 m against its own control, i.e. the carry did 44 % of the
        /// gauge's work, which violates invariant #5 at the mechanism level (datum
        /// invariance: an estimator should constrain estimable functions, not
        /// gauges).
        bool   networkClockAnchorGaugeFree() const { return network_clock_anchor_gauge_free_; }
        /// @brief Which denominator the anchor guard and the census's @c clk family
        ///        standardize by (@c network.clock_anchor_gauge_free_stat).
        ///
        /// A bisection knob.  The gauge-free arm changes the fold and the guard's
        /// denominator in one step, so without this knob its trip collapse (65 → 3
        /// over 3 h) cannot be apportioned; this selects the denominator
        /// independently of the fold.  @c Auto is the shipped pairing and is
        /// byte-identical to runs made before the knob existed.
        NetworkClockAnchorStat networkClockAnchorGaugeFreeStat() const {
            return network_clock_anchor_gauge_free_stat_;
        }
        /// @}

        /// @name Satellite clock level
        /// A shift of satellite s's clock and of every one of its phase biases by the
        /// same amount (the level direction; the clock enters every observation at -1 and
        /// a phase bias enters a phase row at +1, so the shift has the same sign on both)
        /// leaves every phase row invariant and moves every code row of s: the level is a
        /// code-only quantity.  These knobs read it from the code (network/solve/ClockLevel.hpp).
        /// @{
        /// @brief Measure-only level monitor (@c network.clock_level_monitor).
        ///
        /// Runs the level read (the pair build and the two-way fit of the post-fit
        /// ionosphere-free code) and accumulates, per satellite, the code-only information
        /// about its level, I_L, undecayed and uncapped (the level has no process noise, so
        /// the formal bound keeps growing; a decaying accumulator would turn the raise
        /// below into a per-epoch level variance).  Each kept epoch it reads the stashed
        /// border block's variance along the satellite's level direction u (its clock
        /// and every phase bias together), P_L = u'Pu/(u'u)^2, and reports the ratio
        /// P_L / (1/I_L), which a consistent estimator keeps at or above 1: the field's
        /// truth-free consistency instrument for the level (logs/network_level_bound.csv
        /// under @c network.trace_events; run-end line).  Trace class: the solve is
        /// byte-identical.  Requires @c network.shared_carry_full_cov.
        bool   networkClockLevelMonitor() const { return network_clock_level_monitor_; }
        /// @brief Raise the stashed border block to the code-only level bound (
        ///        @c network.clock_level_bound_raise).
        ///
        /// After each kept epoch, for every satellite whose monitor ratio is below 1, the
        /// stashed block is raised along its level direction, P += (1/I_L - P_L) u u', and
        /// the matching carried variances with it, so the next fold carries the level at no
        /// more than the code-only information (a raise, never a set).  Counted
        /// per satellite with the raised amount (invariant 7: the re-coupling of the level
        /// to the code).  Requires @ref networkClockLevelMonitor.  Solve-affecting and
        /// fingerprinted.
        bool   networkClockLevelBoundRaise() const { return network_clock_level_bound_raise_; }

        /**
         * @brief Window of the level innovation instrument, epochs
         *        (@c network.clock_level_innov_window_ep, default 60).
         *
         * The instrument runs inside the level monitor (@ref networkClockLevelMonitor,
         * trace class): per epoch the innovation of the ionosphere-free code along each
         * satellite's level and its NIS, per window the mean innovation's chi2(1)
         * statistic and the split-half truth-free level NEES.  Must be >= 2.
         */
        int    networkClockLevelInnovWindowEp() const { return network_clock_level_innov_window_ep_; }

        /**
         * @brief The phase-visible consistency instrument (@c network.phase_visible_monitor,
         *        default false; docs/refound/PVN_phase_visible_nees_2026-09-29.md).
         *
         * Measure-only.  Per satellite with a clock and phase biases on two bands in the
         * folded dense block, the ionosphere-free phase-visible clock
         * @f$D = c - \sum_j w_j p_j@f$ (@f$\sum w = 1@f$, @f$\sum w\lambda^2 = 0@f$, one phase
         * bias per band): its contrast update against the fold's prior, normalized by the
         * contrast-projected @f$P^- - P^+@f$ with no floor (NIS, toward 1 for an honest
         * covariance), and its posterior contrast value and variance (the sim's within-arc
         * state NEES is graded offline from them).  Trace @c logs/network_phase_visible.csv
         * and a run-end line.  Requires @c network.raim_within_epoch (its fold capture is
         * the prior).  The solve and every product are byte-identical to off.
         */
        bool   networkPhaseVisibleMonitor() const { return network_phase_visible_monitor_; }
        /**
         * @brief Window of the phase-visible instrument's run-end means, epochs
         *        (@c network.phase_visible_window_ep, default 60).  Must be >= 2.
         */
        int    networkPhaseVisibleWindowEp() const { return network_phase_visible_window_ep_; }

        /**
         * @brief Raise the border block restored by a warm resume along each satellite's
         *        level direction, once, at the first leg epoch
         *        (@c network.clock_level_restore_raise).
         *
         * A snapshot written by a carry whose level recursion was not consistent (every
         * lineage before the exact update) holds a level variance far below what the code
         * supports, and the exact update switched on over it keeps the level frozen
         * (measured: 0.14 m unchanged after the seam).  The raise adds
         * V_L u_s u_s' to the block and V_L to the carried clock and phase-bias variances,
         * V_L the inverse of the writer monitor's contrast-frame code information (the
         * snapshot's LB rows), u_s the indicator of the satellite's clock and phase-bias
         * rows: the level loosens to its code-only variance and every phase-visible
         * combination keeps its variance (a raise, never a set; no information is added).
         * Requires @ref networkClockLevelMonitor.  Default false = byte-identical.
         */
        bool   networkClockLevelRestoreRaise() const { return network_clock_level_restore_raise_; }
        /**
         * @brief Admit a warm resume from a snapshot written WITHOUT the exact update's tokens into a
         *        run that arms them (@c network.ctu_switch_on_resume).
         *
         * The resume gate then also accepts the live fingerprint with the exact update, the
         * bound raise, the in-block receiver code-bias carry, the drop decouple and the
         * phase-space guard tokens removed: the switch-on seam that was graded
         * (G5 FAILED: the restored ambiguities hold the frozen level).  Off (the default),
         * a snapshot written without those knobs does not resume into a run that arms them,
         * which is the "fresh convergence only" rule of the ledger rows.
         * Default false; a resume-path decision only, so a cold run is byte-identical.
         * @return True when the switch-on direction is admitted.
         */
        bool   networkCtuSwitchOnResume() const { return network_ctu_switch_on_resume_; }
        /**
         * @brief Epochs of code information the level restored by a warm resume is
         *        credited with (@c network.clock_level_restore_credit_ep).
         *
         * 0 (default): all the snapshot's epochs, V = 1/I_c.  k > 0: V = n / (k I_c), the
         * variance k epochs of the writer's average code information would leave; a small
         * k re-opens an inherited level that is wrong beyond its own information (the
         * lineages before the exact update) to the code at the seam.
         */
        int    networkClockLevelRestoreCreditEp() const { return network_clock_level_restore_credit_ep_; }
        /// @}

        /**
         * @brief Broadcast satellite health on the network path
         *        (@c network.sat_health).
         *
         * The network's a-priori orbits and clocks come from precise products, which can
         * carry a satellite the broadcast flags unhealthy (DOY 129: G01, health 63 in every
         * record and the lexically first GPS satellite, hence the GPS clock pivot; E14 and
         * E18).  "off" (the default): health is not consulted.  "pivot": the clock-pivot
         * election passes over a satellite flagged unhealthy at the epoch (the sticky
         * incumbent and the lexical first; a declared @c network.clock_datum_satellite is
         * kept), and its observations are processed.  "exclude": the observations of a
         * satellite flagged unhealthy at an epoch are removed from the synced epochs at
         * setup, beside the constellation door, so at that epoch it is neither estimated nor
         * on the wire.  The status at an epoch is the record in force
         * (@ref NetworkSbasis::satHealthSeries).  Needs a broadcast navigation file.
         * SOLVE-AFFECTING and FINGERPRINTED.
         * @return 0 off, 1 "pivot", 2 "exclude".
         */
        int    networkSatHealth() const { return network_sat_health_; }

        /**
         * @brief Coverage-ranking elevation preference (deg) for the prioritized
         *        phase-bias anchor (@c network.phase_datum_pref_elev_deg).
         *
         * A satellite counts toward a receiver's per-constellation coverage rank
         * only when tracked at or above this elevation, so the elected anchor is a
         * receiver seeing that constellation well (not merely at the horizon).  It
         * does not mask the pin itself: a receiver that is the last remaining
         * tracker of a setting satellite can still anchor it (any usable ambiguity
         * qualifies); the preference only orders the candidates.  Inert unless
         * @ref networkPhaseDatumPrioritized.  Default 15°.
         */
        double networkPhaseDatumPrefElevDeg() const { return network_phase_datum_pref_elev_deg_; }

        /**
         * @brief Whether a per-(satellite,signal) anchor is sticky
         *        (@c network.phase_datum_sticky).
         *
         * When on, the incumbent anchor for a (satellite,signal) is kept as long as
         * it still tracks that satellite with a usable ambiguity, even if a peer
         * gains marginally higher constellation coverage; a re-election (and its
         * consumer-visible discontinuity) fires only when the incumbent loses the
         * satellite.  This minimizes anchor-handover discontinuities; it is the
         * analogue of @ref networkStickyPivot for the phase-bias gauge.  Inert
         * unless @ref networkPhaseDatumPrioritized.  Default true.
         */
        bool networkPhaseDatumSticky() const { return network_phase_datum_sticky_; }

        /**
         * @brief Make a phase-bias anchor handover continuous: an S-transform
         *        rather than a re-realization
         *        (@c network.phase_datum_handover_continuous).
         *
         * The phase-bias gauge is one constraint per (satellite,signal): "station
         * r*'s ambiguity for this key = 0".  Without this knob, every mechanism that
         * moves r* re-pins the successor at 0, which realizes a different frame; the
         * whole (satellite,signal) family steps by the inter-station ambiguity
         * difference and the satellite clock absorbs it.  That is a gauge change
         * applied with no S-transform: under invariant #7, an unbounded decoupling
         * edge and an implicit re-coupling edge.
         *
         * On a truth-clean 24-station fixture a legacy pivot losing one satellite
         * stepped the emitted clock by -12.06 m without recovery, while
         * @ref networkPhaseDatumPrioritized turns that one large step into fifteen
         * smaller ones.  Neither posture fixes the gauge; each handles only one
         * edge.
         *
         * When armed, the election hands the pin to the incoming anchor at that
         * station's own absolute ambiguity estimate in the outgoing frame instead
         * of at 0, so the realized frame is unchanged by construction and nothing
         * steps.  Both edges are counted (@c handover_continuous /
         * @c handover_cold) and emitted as @c PBGAUGE trace rows carrying the
         * outgoing anchor, the incoming anchor and the target applied.
         *
         * @warning The value used is @b not @c getStateVector()(amb_idx): that is
         *          @c x_lin, a linearization frame, and shifting it on a local
         *          column is an exact solve no-op, so it carries no information
         *          about where the arc sits.  The estimate is @c x_lin +
         *          back-substituted Δx, which exists only after the solve, so the
         *          ledger is harvested post-solve and consumed one epoch later (the
         *          same deferral shape as the C3c funnel).
         *
         * @warning A successor with no ledger entry (a fresh arc, or the run's
         *          first epoch) is pinned at 0 and its frame does step.  That case
         *          is counted as @c handover_cold and traced, so the consumer is
         *          told about the unavoidable re-realization instead of receiving
         *          a silent one.
         *
         * Requires @ref networkPhaseDatumPrioritized and not the eq34 chain datum
         * (which owns its own pin-target policy); the loader forces it off otherwise.
         * Off = byte-identical: no ledger is allocated, no census is widened, and
         * the election writes the 0.0 target.
         *
         * @see networkPhaseDatumSticky, networkPhaseDatumPrioritized,
         *      docs/SPEC_pivot_visibility_gauge_defect.md
         */
        bool networkPhaseDatumHandoverContinuous() const {
            return network_phase_datum_handover_continuous_;
        }
        /// @brief Whether the phase datum is held as a constant S-basis (RF-2 item 1).
        /// @return network.phase_datum_held_basis (requires the float_kf vehicle).
        bool networkPhaseDatumHeldBasis() const {
            return network_phase_datum_held_basis_;
        }
        /// @brief Minimum arc age (loop epochs) to hold the S-basis (RF-2 item 3).
        /// @return network.phase_anchor_min_age_ep; 0 disables the gate.
        int networkPhaseAnchorMinAgeEp() const {
            return network_phase_anchor_min_age_ep_;
        }
        /// @brief Screen-flag quiet window (loop epochs) for the S-basis (RF-2 item 3).
        /// @return network.phase_anchor_flag_quiet_ep; 0 disables the gate.
        int networkPhaseAnchorFlagQuietEp() const {
            return network_phase_anchor_flag_quiet_ep_;
        }
        /**
         * @brief Whether a cold held-basis handover keeps its retired-frame release (RF-2d).
         *
         * A cold handover pins the successor at 0, so the (satellite, signal) family's
         * frame steps by the successor's level in the outgoing frame.  The phase-anchor
         * reref set (@c network.phase_anchor_carry_reref) holds exactly the cold and
         * first-sighting families and releases their retired-frame carries (the shared
         * PB carry for one epoch, the local ambiguity carries at every station), so the
         * step lands on the free phase bias.  Without this knob the held basis (RF-2)
         * clears that set on every held-basis election; the released priors then fold
         * against the re-realized design and the satellite clock absorbs the step
         * (RF-3-read: on the sim, a 51.5 m L2W basis step became a 65 m clock step).
         * On = the set is kept, as on the level vehicle.
         *
         * @return network.phase_datum_held_cold_release; inert unless
         *         network.phase_datum_held_basis.  Off = byte-identical.
         */
        bool networkPhaseDatumHeldColdRelease() const {
            return network_phase_datum_held_cold_release_;
        }
        /**
         * @brief Whether a held-basis handover is declared to the consumer iff it is cold
         *        (RF-2d).
         *
         * Without this knob, RF-2b marks a handover iff |value − prev_value| exceeds the
         * exact tolerance.  That difference is the between-station level difference, not
         * the wire step: a continuous handover pins the successor at its level in the
         * outgoing frame and
         * does not step (sim: 50 of 50 IF-correction steps under 1 cm) yet always reads a
         * non-zero difference (144 of 145 field marks were continuous), while a cold
         * handover out of a cold tenure reads 0 − 0 and goes unmarked although the family
         * re-realizes (10 of 12 on the field).  On = mark exactly the cold ones.
         *
         * @return network.phase_datum_held_mark_cold; inert unless
         *         network.phase_datum_held_basis.  Off = byte-identical.
         */
        bool networkPhaseDatumHeldMarkCold() const {
            return network_phase_datum_held_mark_cold_;
        }

        /**
         * @brief Conditional-variance ceiling (m²) on a handover ledger value
         *        before it may be pinned
         *        (@c network.phase_datum_handover_max_var_m2).
         *
         * The re-coupling edge must re-validate what it reconnects (invariant #7).
         * The pin is a hard ~1e10-weight constraint, so stamping in an
         * ill-determined estimate injects that estimate's own error as a datum (an
         * unvalidated +17.08 m target produced a 378-sigma residual spike on the
         * following epoch, worse than the step it replaced).  A ledger value whose
         * conditional variance exceeds this ceiling falls through to the cold path
         * (pin at 0, counted, traced) instead.
         *
         * Inert unless @ref networkPhaseDatumHandoverContinuous.  Default 1.0 m²
         * (σ = 1 m): still an order of magnitude better than the metre-to-tens-of-
         * metres step a re-zeroing handover produces, while refusing a code-seeded
         * fresh arc outright.
         */
        double networkPhaseDatumHandoverMaxVarM2() const {
            return network_phase_datum_handover_max_var_m2_;
        }

        /**
         * @brief Handover ceiling relative to the arc's own first-stash variance
         *        (@c network.phase_datum_handover_max_var_ratio).
         *
         * @ref networkPhaseDatumHandoverMaxVarM2 is an absolute variance, and its
         * field value (1e-4 m²) was sized to the over-counted σ of the diagonal
         * phase-leveling carry: under the corrected σ of F1/F3 it refuses 93 of 93
         * successors, and the 1e-2 m² that restores continuity is a hand-set value.
         * This knob asks the posture-invariant question instead: whether this arc
         * has converged since its birth.  A ledger value is usable only if its
         * conditional variance is <= ratio × the arc's first-stash conditional
         * variance (@c LocalCarry::var_first, kept across re-stashes; an
         * erase-and-rebirth starts it over, and a re-level keeps it, so a refloated
         * arc reads unconverged until it re-earns the level).  An unknown first-stash
         * variance (a fresh arc that was never carried) is refused.  The absolute
         * ceiling stays as a rail; both refusals are counted separately and every
         * PBGAUGE handover row then carries the successor's var / var_first.
         * Ledger verdict KEEP_OFF: the first-stash denominator is biased by pin
         * tenure.
         *
         * Inert unless @ref networkPhaseDatumHandoverContinuous.  0 (default) = off:
         * the absolute ceiling alone, byte-identical.
         */
        double networkPhaseDatumHandoverMaxVarRatio() const {
            return network_phase_datum_handover_max_var_ratio_;
        }

        /**
         * @brief Cold anchor fallback: make a usable carried value a candidacy
         *        condition of the anchor election
         *        (@c network.phase_datum_candidacy_usable).
         *
         * The problem (census, docs/SPEC_event_harness.md §15 + §16.8): without this
         * knob the election sweeps the coverage-ranked candidates and takes the first
         * that tracks the key; only afterwards does the continuous branch look the
         * successor up in the one-epoch handover ledger and apply its usability
         * predicate.  A family whose top-ranked tracker has no usable ledger value
         * therefore goes cold while a lower-ranked tracker with a mature usable arc
         * sits unused (on the storm arm, 43 of 47 non-cold-start cold handovers had
         * three or more mature alternatives).  A cold handover re-realizes the
         * family's gauge, marks it for re-reference and erases its leveling carries
         * at every station.
         *
         * On: when the sticky incumbent is not kept and a handover is therefore
         * about to happen, the sweep gains a first pass over the same coverage-ranked
         * list that accepts only candidates whose arc is usable: the continuous
         * branch's own predicate (finite value, 0 < var <= the ceiling, the ratio
         * gate when armed), plus "not itself freed this epoch" and the
         * @ref networkPhaseDatumCandidacyMinAgeEp maturity test.  The existing
         * coverage order ranks the usable candidates and the first is elected.  With
         * no usable candidate the election falls through to the two unscreened
         * sweeps (the cold path), counted.
         *
         * Property: with this armed, a handover goes cold only when no tracking
         * candidate satisfies the continuous branch's own predicate, so
         * @c handover_cold then counts exactly the unavoidable ones.  Ledger verdict
         * KEEP_OFF (SPEC_event_harness §15).
         *
         * Inert unless @ref networkPhaseDatumHandoverContinuous (no ledger exists to
         * screen against) and the anchored election is the datum.  Off (the default)
         * = byte-identical: the pass is not entered and no counter moves.
         *
         * @see networkPhaseDatumCandidacyMinAgeEp, networkPhaseDatumHandoverMaxVarM2,
         *      docs/SPEC_event_harness.md §15
         */
        bool networkPhaseDatumCandidacyUsable() const {
            return network_phase_datum_candidacy_usable_;
        }

        /**
         * @brief Cold anchor fallback, iteration 2: the satellite clause
         *        (@c network.phase_datum_candidacy_satellite; default true, inert
         *        unless @ref networkPhaseDatumCandidacyUsable).
         *
         * Iteration 1's screen is per-family: each (sat, signal) independently elects
         * the host whose carry is best for its band.  On the storm arm
         * (SPEC_event_harness §15.6) the producer side improved on every channel but
         * the user's median rose 49 %, with split-referenced satellites rising from
         * 0.0 to 0.9 mean / 3 max: a satellite whose three bands elect three hosts
         * carries an inter-frequency inconsistency that the per-band producer
         * instruments cannot see and the consumer pays for (the pattern of
         * §16.7/§16.8, handled there with an anchor clause).
         *
         * On, the election resolves a satellite's handover jointly:
         *  (a) every family of satellite s handing over this epoch takes one successor,
         *      the first coverage-ranked candidate usable for the largest number of
         *      them (ties → coverage order).  A family for which that host is not
         *      usable is still pinned there, through the cold path
         *      (@c sat_clause_cold_colocated): a cold pin on the same host is a frame
         *      step, not a split.  No candidate usable for any of them ⇒ the
         *      unscreened sweeps, unchanged.
         *  (b) when only some of s's families move, the station already anchoring the
         *      others is tried first and, if it tracks and is usable for at least one
         *      mover, takes them all (@c sat_clause_colocated); this is the only
         *      reading under which (c) can hold.
         *  (c) the run-end clause reports the per-satellite distinct-anchor census
         *      from the same counters the storm grade reads, so the clause and the
         *      grade agree by construction.
         *
         * Off is exactly iteration 1 (per-family), so the A/B is a single knob.
         */
        bool networkPhaseDatumCandidacySatellite() const {
            return network_phase_datum_candidacy_satellite_;
        }

        /**
         * @brief Minimum arc age, in loop epochs, for a candidate to be usable
         *        (@c network.phase_datum_candidacy_min_age_ep; 0 = no age test).
         *
         * Age is `n_loop_epochs − LocalCarry::birth_ep` of the candidate's own
         * phase-leveling carry for the key.  An unknown age (no carry entry, or a
         * warm-resume-restored entry whose @c birth_ep reads the restored sentinel
         * @ref LocalCarry::kBirthEpRestored, SPEC_event_harness §15.4) counts as mature,
         * the restored-entry convention (@ref localCarryYoung): the conservative
         * direction is to let the variance ceiling decide rather than refuse a candidate
         * the ledger says is converged.
         *
         * The census behind the default: the 68 cold handovers of the §15 storm arm
         * sat at successor-arc age p50 39 epochs against the 237 continuous ones'
         * p50 236, so 60 epochs (30 min at 30 s) separates the two populations
         * without reaching into the continuous one.
         *
         * Inert unless @ref networkPhaseDatumCandidacyUsable.
         */
        int networkPhaseDatumCandidacyMinAgeEp() const {
            return network_phase_datum_candidacy_min_age_ep_;
        }

        /**
         * @brief Marginalize a handed-over (sat,signal) family's carried state for
         *        one epoch (@c network.phase_anchor_carry_reref; QUEUE item 1(d)).
         *
         * The third named cause of the prioritized arm's clock churn: at a
         * PHASE_ANCHOR handover the datum re-realizes on the design matrix only; the
         * incoming anchor is re-pinned (at 0, or at the ledger target) while the
         * previous epoch's carried bˢ_σ, a value in the retired frame, is folded
         * forward at @c sat_phase_bias_q = 1e-12.  Against @c sat_clock_q = 0.01
         * (1e10:1) the satellite clock absorbs the entire frame step (a 41 σ step
         * was measured).  Neither existing carry-skip guard fires there:
         * @c datum_changed reads the clock-pivot map only, and @c eq34_reref is
         * populated only by the eq34 chain election.
         *
         * On: the anchor election populates a per-epoch @c phase_anchor_reref
         * (sat,signal) set alongside its handover events; the D2/C1 shared-carry
         * consume marginalizes those PB keys for that one epoch (the @c eq34_reref
         * treatment: the retired-frame prior is not folded, so the family realizes
         * the new frame without the clock absorbing the step), and the family's
         * phase-leveling local ambiguity carries are erased at every station (they
         * are frame-dependent; refloat next epoch).
         *
         * The @c eq34_reref template deletes the prior for one epoch; the
         * theoretically correct action is an S-transform of the carry family (that a
         * transform beats a delete is inferred, not measured; SPEC §5.07.6).
         *
         * Inert unless @ref networkPhaseDatumPrioritized, and disabled under the
         * eq34 chain (owns its own re-reference).  Off = byte-identical: the set is never populated and
         * every consumer tests an empty set.
         *
         * @see networkPhaseDatumHandoverContinuous, networkPhaseDatumPrioritized,
         *      docs/QUEUE_principled_replacements.md item 1(d)
         */
        bool networkPhaseAnchorCarryReref() const {
            return network_phase_anchor_carry_reref_;
        }

        /**
         * @brief Coverage threshold on the prioritized anchored union: minimum
         *        number of present stations that must track a (satellite,signal)
         *        with a usable ambiguity for its shared phase-bias column to be
         *        registered and emitted (@c network.phase_datum_min_stations).
         *
         * Under @ref networkPhaseDatumPrioritized the shared phase-bias set is the
         * anchored union over all present stations, which admits columns tracked by
         * a single receiver.  Those thin columns cause three problems (found in the
         * pdp validation): (a) a marginal tracking station's local block barely
         * separates the column from its own ambiguity, so the station is ejected as
         * non-PD; (b) a one-station anchor pool re-levels the bias by a full
         * ambiguity lump (tens of metres) whenever its only anchor sets; (c) weakly
         * separated columns leak their anchor lump into the clock↔phase-bias split
         * (median |dev| 0.8 → 10 m, cancelling for clock+bias users but distorting
         * clock-only comparisons).
         *
         * A gated-out key costs the product only that bias: the tracking stations'
         * phase rows stay in the solve and serve the satellite clock through their
         * own float ambiguities, as legacy non-shared signals (and GLONASS network
         * phase) do.  On the float vehicle that holds only with
         * @ref networkPhaseDatumGateDropErase (see there).  The allocated
         * per-station phase-bias states of a gated key are pruned so the station
         * column sets stay consistent with the gated union.
         *
         * 1 (default) = no gating (the original prioritized-union behaviour,
         * byte-identical).  2–3 recommended once armed.  Inert unless
         * @ref networkPhaseDatumPrioritized.
         *
         * @see networkPhaseDatumPrioritized, networkPhaseDatumSticky
         */
        int networkPhaseDatumMinStations() const { return network_phase_datum_min_stations_; }

        /**
         * @brief Erase the family's carried ambiguities at the coverage gate's drop
         *        edge (@c network.phase_datum_gate_drop_erase).
         *
         * @ref networkPhaseDatumMinStations states that a gated-out key costs the
         * product only that bias, because the tracking stations' phase rows keep
         * serving the satellite clock through their own float ambiguities.  That
         * holds where the float ambiguity is refloated at the edge.  On the float
         * vehicle (@c network.ambiguity_vehicle = float_kf) the surviving station's
         * ambiguity is a carried state with a centimetre prior, so it absorbs the
         * whole removed bias in one epoch with no refloat, no re-seed and no variance
         * inflation (on the field bed, +15.8 m against a variance of 2.8e-4 m^2);
         * what the prior does not absorb lands in the satellite clocks, and every
         * emitted clock trips the Q3 anchor gate in that epoch.  On the field bed
         * these edges accounted for 49 of 50 clock-anchor trips in 6 h
         * (docs/refound/EV7_all_sat_clock_decouple_2026-09-21.md §5.3, §6.1).
         *
         * Armed, a drop edge (a key the gate excluded this epoch that had an anchor
         * incumbent last epoch) is added to the epoch's re-reference set, so
         * @c networkApplyPhaseAnchorCarryReref erases every station's carried
         * ambiguity of the family before assembly, as the family's return already
         * does.  The re-born ambiguity absorbs the bias once at birth strength and is
         * carried normally afterwards; the erase does not repeat on the following
         * gated epochs, because by then the key has no incumbent.
         *
         * Inert without @ref networkPhaseAnchorCarryReref (which supplies the
         * re-reference set) and without the gate itself
         * (@c phase_datum_min_stations > 1).  Off (default) is byte-identical: the
         * edges are still detected and counted (so a run with it off reports the size
         * of the class; invariant #4), but nothing is inserted anywhere and the only
         * artifact is extra @c PHASE_ANCHOR / @c gate_drop trace rows.
         * Not fingerprinted: a forward-only response, no carried state changes
         * meaning.
         *
         * @see networkPhaseDatumMinStations, networkPhaseAnchorCarryReref
         */
        bool networkPhaseDatumGateDropErase() const {
            return network_phase_datum_gate_drop_erase_;
        }

        /**
         * @brief Relative eigenvalue tolerance of the shared-border null-space rank
         *        gate (@c network.rank_gate_tol).
         *
         * The gate eigen-decomposes the reduced shared normals and asserts that
         * the number of eigenvalues below @c tol·max(λmax,1) equals the declared
         * S-basis dimension.  The default (1e-9,
         * @ref NetworkEstimator::kNullSpaceTol) equals the former compile-time
         * constant, so an unset key is byte-identical (the same double travels
         * through the same arithmetic).  The resolved value reaches all three
         * consumers (the gate, the estimability dump, the prioritized
         * drop-and-heal) through @ref NetworkEstimator::setRankGateTol; no
         * consumer keeps a private tolerance.
         *
         * @par Why this is a calibration parameter, not a constant
         * The reduced system is deliberately ill-conditioned, so "null" is a
         * placement decision in a spectral gap, and the gap moves with the
         * constellation mix.  GPS+Galileo needed 1e-6 → 1e-9 because 1e-6 sat
         * above the weak-but-real directions and miscounted them as deficiencies.
         * Adding BeiDou moves it again: the measured 305-station spectrum puts a
         * weak-but-real BDS family layer at ≈1.7e-10 of λmax, three decades deeper
         * (relatively) than the weakest-real layer of GPS+Galileo, with a
         * six-decade gap down to the declared clock gauges.
         *
         * @par Binding placement rule (@c docs/SPEC_beidou_w6_datum.md §2, DG3)
         * Any non-default value must be justified from that config's own
         * estimability dump (@c network.trace_estimability →
         * @c logs/network_estimability.csv): the resulting cutoff must sit ≥ 2
         * decades above the config's gauge depth and ≥ 2 decades below its
         * weakest-real layer, with the three numbers recorded.  The gate is an
         * exact-equality test that throws in both directions, so a tolerance
         * pushed below a config's own gauge depth stops counting the declared
         * gauges and withholds a healthy config ("over-specified").  The value is
         * therefore per-config and is never edited globally.  Example: the
         * 305-station GPS+Gal+BDS storm config arms 1e-12 (cutoff ≈ 6e-6, mid-gap,
         * ≥ 2 decades of margin each side).
         *
         * Validated > 0 at load and clamped up to 1e-16, so an accidental 0
         * cannot silently turn the gate off.
         *
         * @see networkTraceEstimability, NetworkEstimator::setRankGateTol
         */
        double networkRankGateTol() const { return network_rank_gate_tol_; }

        /**
         * @brief Arm the loose physical-bound (Kaula) prior on every admitted
         *        satellite code-bias (OSB) column (@c network.cb_kaula_prior).
         *        Default off ⇒ byte-identical.
         *
         * The conditional second lever of the BeiDou W6 datum work, after the
         * rank-gate calibration (@ref networkRankGateTol) and the phase
         * drop-and-heal.  Its trigger is a residual below-cutoff direction that is
         * code-dominated: the drop-and-heal only anchors phase-bias columns, so a
         * near-null direction carrying zero phase weight is structurally beyond it,
         * and no gate value clears a truly null direction.  A satellite's OSBs are
         * physically bounded (tens of ns), so a loose prior toward 0 is real prior
         * information: it lifts that direction while leaving every well-observed
         * OSB untouched, since their data information is orders of magnitude above
         * 1/σ².
         *
         * Inert unless @ref estimateNetworkSatCodeBias (there are no OSB columns
         * otherwise).  @see networkCbKaulaSigmaM for the σ and its placement rule.
         */
        bool networkCbKaulaPrior() const { return network_cb_kaula_prior_; }

        /**
         * @brief σ (metres) of the admitted-OSB physical-bound prior
         *        (@c network.cb_kaula_sigma_m).  Default 10 m ≈ 33 ns.
         *
         * @par Placement rule (K2, anti-fragility), binding
         * An earlier loose-prior quarantine was rejected because its
         * information sat numerically at the rank-gate cutoff, so it either
         * tripped the gate or survived by coincidence.  This prior avoids that
         * failure mode by construction: its information @f$1/σ^2@f$ must land at
         * least two decades above the armed cutoff (@c rank_gate_tol · max(λmax,1))
         * of every config it ships in.  The producer computes that margin from the
         * epoch's own λmax and logs it once; a margin under two decades warns,
         * because that is the rejected configuration.
         *
         * Measured on the 305-station storm arm: cutoff 6.022e-06, so σ = 10 m
         * (info = 1e-2) clears it by ≈ 3.2 decades, and the residual direction's
         * own CB weights (≈ 0.145 each over four signals) lift it by ≈ 8e-4,
         * ~2.1 decades above the cutoff.  Both margins hold.
         *
         * Too loose is safe but inert (the direction stays below the cutoff and
         * the epoch still withholds); too tight starts biasing well-observed OSBs
         * toward zero, which is a product error rather than a datum one.  10 m
         * sits deliberately at the loose end of the physical bound.
         */
        double networkCbKaulaSigmaM() const { return network_cb_kaula_sigma_m_; }

        /**
         * @brief Arm the per-epoch S-basis dimension completion
         *        (@c network.sbasis_complete; SBC).  Default off ⇒ byte-identical.
         *
         * The declared S-basis dimension counts only the structural gauges (one
         * clock gauge per constellation in PIN mode).  A multi-constellation
         * epoch can realize more near-null directions than that: on a 12 h
         * GPS+Galileo field run, 41 % of epochs carried 1–3 per-satellite
         * phase-bias↔slant-iono trade directions confined to one receiver channel
         * family (the Galileo C/Q-vs-X tracking split), with eigenvector components
         * ∝ μ_σ over that family's phase biases and only the SH-VTEC prior
         * resisting (λ ≈ 1e-6 ≪ cutoff) when the family's code support thins.
         * Without completion the rank gate withholds the epoch, or the fallback
         * soft-anchors columns (a column prior that also touches the estimable
         * part and misses spread directions).
         *
         * Armed, the producer derives the epoch's realized dimension from the
         * reduced normals and folds one minimum-constraint gauge row per residual
         * direction: the direction's own unit eigenvector, with RHS = its carried
         * level (continuity: the unobservable combination holds instead of
         * stepping, and nothing estimable moves, by S-transform invariance).  Both
         * edges are events (@c sbc_engage / @c sbc_release, tier GATE;
         * invariant #7) with per-direction attribution, and every refusal path is
         * counted (see @ref networkSbasisCompleteMax).  The fallback stays armed as the
         * backstop for what the completion refuses (orphaned exact gauges land in
         * the degenerate refusal on purpose).
         *
         * @see NetworkSbasis::completeSbasisDimension for the derivation and
         *      the refusal contract; @c NetworkSbasis::kSbasisCompleteWeight
         *      for the row weight and its K2 margin rule.
         */
        bool networkSbasisComplete() const { return network_sbasis_complete_; }

        /**
         * @brief Per-epoch cap on completed (constrained) directions
         *        (@c network.sbasis_complete_max; SBC).  Default 6.
         *
         * Beyond the cap the completion refuses (nothing folded, epoch falls
         * through to the gate → withhold) rather than mass-pinning: a
         * deficiency burst that size is a pathological epoch or a
         * @c network.rank_gate_tol counting weak-but-real directions as null,
         * and silently constraining dozens of them would fabricate a solution.
         * Refusals are counted and reported at run end (invariant #3: a bound that
         * trips is never silent).  Measured need on a GPS+Galileo field arm: 1–3
         * extras per firing epoch, so 6 leaves headroom without admitting a
         * burst; ≤ 0 removes the cap (diagnostic only).
         */
        int networkSbasisCompleteMax() const { return network_sbasis_complete_max_; }

        /**
         * @brief Whether the network kernel's per-station local-block positive-
         *        definiteness test uses the pin-aware median-|pivot| reference
         *        scale instead of the maximum pivot
         *        (@c network.station_pd_pin_aware).
         *
         * The legacy test drops a station when its smallest local LDLᵀ pivot
         * falls below 1e-12 × the largest pivot.  Under
         * @ref networkPhaseDatumPrioritized every station elected as a
         * phase-bias anchor carries 1e10 gauge-pin rows in its own local block,
         * inflating that bar by ~10 orders: a marginal station whose weakest real
         * parameter sits at a usable ~1e-3 information is then ejected every
         * epoch (the same block passes when the station holds no anchor pin),
         * which permanently thins the network.  An A/B showed this ejection class
         * is not affected by the anchored-union coverage gate (identical failing
         * pivots with the gate on and off): the pins, not the thin columns, set
         * the bar.  With this toggle the reference scale is the median |pivot|,
         * which is robust to any minority of pin-scale pivots and still catches a
         * truly singular direction.  Applies to every network mode but only matters
         * where huge in-block pins exist (the prioritized datum; the legacy
         * single pivot station).  Default false = legacy max-pivot test
         * (byte-identical).
         *
         * @see networkPhaseDatumPrioritized, networkPhaseDatumMinStations,
         *      NetworkNormalEquations::setStationPdMedianScale
         */
        bool networkStationPdPinAware() const { return network_station_pd_pin_aware_; }

        /**
         * @brief Whether the producer's internal response to a clock-pivot handover
         *        is scoped to the constellation(s) whose datum changed
         *        (@c network.scoped_handover_response, RTA-2).
         *
         * On a pivot handover the carried cross-epoch state of the changed
         * constellation lives in a retired datum: its D2 shared-carry priors must
         * be skipped, its two-state clock-model oscillators cleared, and its
         * clock-monitor jump baselines suppressed for the epoch.  The legacy
         * response applies that invalidation globally, so one constellation's
         * handover degrades every constellation's product for the warmup horizon.
         * When on, those internal responses apply only to satellites of the
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
         * @brief Whether a clock-pivot handover re-expresses the changed
         *        constellation's carried shared state instead of dropping it
         *        (@c network.handover_stransform, queue 1(b)).
         *
         * Increment 0 (@ref networkScopedHandoverResponse) stops a handover from
         * invalidating the constellations whose datum did not move.  This knob
         * addresses the remainder: the changed constellation's own carry, which
         * without it is dropped for one epoch
         * (docs/SPEC_pivot_visibility_gauge_defect.md §8.2).  When on, at the
         * switch epoch the carried shared means of that constellation are
         * transformed along the structural clock null direction
         * @ref NetworkSbasis::clockNullDirection: @f$c_0 = x_q/v_c[q]@f$ with q
         * the incoming pivot, @f$x \leftarrow x - v_c c_0@f$, @f$\mathrm{var}
         * \leftarrow \mathrm{var} + v_c[i]^2\,\mathrm{var}_q@f$ (the conservative
         * diagonal dispersion bump of §8.1; the full @f$SDS^\top@f$ is deferred by
         * §8.8).  The D2 shared-carry fold is then not skipped for it, so the prior
         * folds in the new datum instead of being discarded.  The emitted wire
         * still steps by the datum delta by design; this is a producer-internal
         * continuity fix, not a consumer fix.
         *
         * Requires @ref networkScopedHandoverResponse (inert and warned otherwise):
         * the transform handles the changed constellation and scoping handles the
         * rest; releasing one skip without the other would leave the other half
         * dropped anyway.  Inert under an anchor-free clock datum (sum-to-zero /
         * broadcast-anchored), which has no pivot and no handover seam.
         * Fingerprinted when armed (armed-only token, so every existing snapshot
         * fingerprint is unchanged): it changes which priors fold, so fresh
         * convergence only, and it must not be flipped mid-chain.  Default false =
         * the carry is skipped (byte-identical).
         */
        bool networkHandoverStransform() const { return network_handover_stransform_; }

        /**
         * @brief Whether the clock-pivot handover's S-transform runs before the coupling rows
         *        are built (@c network.handover_stransform_before_rows).
         *
         * By default the transform runs in the solve glue, after PASS B has built the
         * coupling rows on the stashed variance, and only on the carried clocks, so the
         * handover epoch's rows are conditioned on the variance before the transform while
         * the fold uses @f$\mathrm{var}_i + v_i^2\mathrm{var}_q@f$ (
         * 31 GPS clocks at the 6 h goldens' handover).  With this knob the election and the
         * handover test are previewed before the fold decision and PASS B (both pure), and the
         * transform re-expresses, through the traversal of carried references, every clock
         * reference of the changed constellation the rows read: the carried clocks (an
         * unregistered or parked satellite of the constellation included), the s_prev and the
         * free p_prev of every coupling term on them, and the stash's pivot value.  The
         * decision, the exact update's prepare and the rows then read the re-expressed carry,
         * and the solve glue's transform does only the bookkeeping (counters, events, the fold
         * release).  A handover re-detected after a skipped epoch finds the carry already in
         * the incoming datum and is released without a second transform.
         *
         * Requires @ref networkHandoverStransform (inert and warned otherwise).  Refused with
         * @c network.apriori_source brdc_rts: the ephemeris cut-over re-level runs in the solve
         * glue and would then follow the transform it must precede.  Refused with
         * @c network.carry_rx_clock_in_block: the receiver clock is on the clock datum's
         * direction and its carried value is not re-expressed.  Fingerprinted when armed
         * (armed-only token): fresh convergence only.  Default false = the transform after
         * PASS B (byte-identical).
         */
        bool networkHandoverStransformBeforeRows() const {
            return network_handover_stransform_before_rows_;
        }

        /**
         * @brief Whether the clock-pivot handover's S-transform re-expresses the stashed
         *        dense border exactly, by @f$S P S^\top@f$, instead of the diagonal bump
         *        (@c network.handover_stransform_dense).
         *
         * The transform after PASS B (@ref networkHandoverStransform) moves the carried
         * clock values by @f$-c_0@f$ and adds @f$\mathrm{var}_q@f$ to each carried clock
         * variance, while the dense side-car @c prev_carry_cov keeps its off-diagonals in
         * the outgoing frame.  The fold then combines bumped diagonals with stale
         * off-diagonals: every between-satellite clock contrast is over-stated by
         * @f$2\,\mathrm{var}_q@f$ and every clock-bias cross-covariance is stale by
         * @f$-P_{bq}@f$.  With this knob the values
         * and the block are re-expressed together: @f$S = I - u e_q^\top@f$ over the
         * constellation's clock family @f$u@f$ (every clock row of the constellation in the
         * block, registered or not), @f$x' = S x@f$, @f$P' = S P S^\top@f$; the incoming
         * pivot's row and column become exactly 0, and the outgoing pivot (pinned at the
         * stash, so it has no row) is appended with value @f$-c_0@f$, variance
         * @f$P_{qq}@f$ and covariance @f$-P_{qj} + P_{qq}u_j@f$.  A clock of the
         * constellation without a block row (a parked entry) takes the diagonal rule.  An
         * edge the dense kernel cannot apply (the incoming pivot has no block row, an empty
         * block) falls back to the diagonal kernel, counted.  The kernel is idempotent.
         *
         * Inert (forced false, warned) without @ref networkHandoverStransform or without
         * @ref networkSharedCarryFullCov.  Refused (a load error) with
         * @ref networkHandoverStransformBeforeRows (the exact transform zeroes the incoming
         * pivot's row, which before the rows would put a pinned column into the exact update's S
         * and swap the pinned route of every row, and the undo of a skipped preview would
         * need the saved block) and with @c network.carry_rx_clock_in_block (the receiver
         * clock sits on the clock direction and its carried value is not re-expressed).
         * Fingerprinted when armed (armed-only token): fresh convergence only.  Default
         * false = the diagonal kernel (byte-identical).
         */
        bool networkHandoverStransformDense() const { return network_handover_stransform_dense_; }

        /**
         * @brief Whether the clock anchor stays engaged at a clock-pivot handover whose
         *        carry the dense S-transform re-expressed exactly
         *        (@c network.clock_anchor_datum_keep).
         *
         * By default the anchor's guard decouples every anchored clock of the changed
         * constellation at a handover (reason datum): four LOOSE epochs follow, a
         * per-satellite clock disc mark is written for each, and the smoothed hold's clones
         * die with the withdrawal.  With this knob the guard skips that decouple for a
         * satellite when all of these hold: the constellation was re-expressed this epoch by
         * the dense kernel (@ref networkHandoverStransformDense) with its self-check passed;
         * the satellite has a row in the re-expressed block; and its fold this epoch was the
         * anchored one, on the re-expressed entry.  The guard's statistic then tests the
         * satellite as usual (a genuine fault still trips), the PIVOT datum bump on the wire
         * stays the consumer's signal, and each kept satellite is counted
         * (@c cta_datum_kept) and written as a CLK_ANCHOR cta_datum_kept event.  Under the
         * diagonal kernel the decouple stays: that fold is not exact.
         *
         * Inert (forced false, warned) without @ref networkHandoverStransformDense or
         * without @ref networkClockTemporalAnchor.  Fingerprinted when armed (armed-only
         * token).  Default false = the decouple (byte-identical).
         */
        bool networkClockAnchorDatumKeep() const { return network_clock_anchor_datum_keep_; }

        /**
         * @brief Whether each carry re-base mechanism also moves the carried references its
         *        exemption table marks as owed
         *        (@c network.carry_rebase_complete).
         *
         * The mechanisms that move a border parameter's frame walk the carried references
         * through one traversal (@c network/state/CarrySites.hpp), each over
         * its own site set; the references its hand-written list never moved are declared as
         * owed exemptions.  With this knob the three walks
         * that carry such gaps also move them, each with its own rule: the
         * ephemeris cut-over re-level (the stash's pivot value, the code-bias excursion and
         * phase-hold clock baselines, and every other coupling map's clock terms under the
         * conditional coupling), the eq34 D4 shift and the float
         * vehicle's declared anchor handover (the last solved phase bias, the gross screen's
         * held ambiguity, every other coupling map's phase-bias terms, and for the declared
         * handover an open pin bridge).  The by-design exemptions stay, and so does the phase
         * hold's held value (an emitted value).  The clock-pivot handover's two transforms are
         * unaffected.
         *
         * Inert, warned and disarmed when no such mechanism is armed (@c network.apriori_source
         * brdc_rts, the eq34 chain datum's carry shift or seed-round re-host, the float
         * vehicle's declared anchor handover).  Fingerprinted when armed (armed-only token):
         * fresh convergence only.  Default false = each mechanism's phase 2 site set
         * (byte-identical).
         *
         * @return @c network.carry_rebase_complete as resolved by the loader's interlock.
         */
        bool networkCarryRebaseComplete() const { return network_carry_rebase_complete_; }

        /**
         * @brief Producer: stamp a solution-generation id on every emitted epoch
         *        (@c network.emit_generation_id, RTA-3).
         *
         * The existing E-line @c iod_ssr is a per-epoch increment and carries no
         * generation information: a producer restart re-seeds every emitter counter
         * at zero, so a warm consumer that stored counter=0 sees 0→0 ("no change")
         * while the phase-bias datum was completely re-realized, which leads to
         * silently wrong fixes.  When on, the producer derives a uint32 generation
         * id from the first emitted epoch's GPS time (week·604800 + ⌊tow⌋):
         * deterministic under file replay (the id is serialized per epoch as an
         * optional @c I record and travels in the S contract as a @c gid= field)
         * and different across live restarts (a restarted producer first-emits
         * later).  The consumer treats a change of a present id as a global datum
         * break (reset all network-corrected ambiguities + re-inflate receiver
         * code-bias states); an absent id (old product / RTCM stream) never
         * triggers a reaction.  Default false ⇒ no @c I record, store generation
         * unset, byte-identical.
         */
        bool networkEmitGenerationId() const { return network_emit_generation_id_; }

        /**
         * @brief Producer: serialize the current discontinuity-counter values on the
         *        wire (@c network.serialize_disc_counters, RTA-4).
         *
         * The v1/v2 product file carries bump triggers (E-line @c datum_changed /
         * @c datum_consts, @c R records) but not counter values, so a late-joining /
         * tail reader can only reconstruct the counters by replaying from epoch 1.
         * When on, each record gains an optional trailing field carrying the
         * post-emit counter for this epoch (identical to what @c emitToStore pushed
         * into the store): C-line field 5 = clock disc counter (field 4 = C1 drift,
         * always written, 0 when absent, so the counter position is fixed),
         * B-line field 5 = per-signal code-bias disc counter, P-line field 7 =
         * per-signal phase-bias disc counter.  The reader adopts present values
         * into the reconstructed store records, keeping the replay recompute as a
         * self-check (warn on mismatch).  Old files (fields absent) parse
         * unchanged; default false ⇒ the file is byte-identical.
         */
        bool networkSerializeDiscCounters() const { return network_serialize_disc_counters_; }

        /**
         * @brief Producer: re-broadcast the v2 'S' contract line every N seconds of
         *        product time (@c network.contract_rebroadcast_s, RTA-5; 0 = off).
         *
         * The S contract is otherwise written exactly once at first emit: its
         * realized per-constellation pivot map goes stale after the first
         * handover, and a late joiner reading the file tail never sees the
         * contract at all.  When > 0, a current S line (refreshed realized pivots
         * from the driver's emit-baseline pivot map, same convention fields,
         * generation id when RTA-3 is armed) is re-emitted before the epoch block
         * every N seconds of
         * product time, and additionally on any epoch whose emit carried a datum
         * change (markDatumChange fired).  The reader accepts repeated S lines
         * positionally (each updates its current contract state, surfaced per
         * epoch via @c NetworkProductRecord::contract_refresh); S lines never feed
         * the replay emitter, so replay stays deterministic.  The S contract itself
         * is always written.  Default 0 ⇒ a single header S line,
         * byte-identical.
         */
        double networkContractRebroadcastS() const { return network_contract_rebroadcast_s_; }

        /**
         * @brief Producer: emitter staleness horizon in seconds of product time
         *        (@c network.emitter_staleness_horizon_s, RTA-11; 0 = off).
         *
         * The emitter's @c last_phase_bias_ / @c last_code_bias_ / clock
         * first-sighting markers persist across a multi-hour satellite vanish; on
         * the satellite's return the |Δbias| jump test compares against the stale
         * value and bumps the discontinuity counter, although semantically this is
         * a first sighting after a gap, not a fault (the consumer holds no
         * surviving ambiguity for a satellite gone that long).  When > 0 the
         * emitter keeps a last-emitted epoch time per (sat[,signal]) key (product
         * time, never wall clock, so replay is deterministic), and a key
         * reappearing after a gap larger than the horizon is treated as a first
         * sighting on the phase-bias, code-bias and clock channels alike: the
         * baseline reseeds and no jump/forced bump lands.  The horizon travels in
         * the serialized S contract (@c stale= field) so @c makeReplayEmitter
         * reproduces identical counters with no out-of-band knowledge.  Default 0 ⇒
         * the legacy keep-forever baseline, byte-identical.
         */
        double networkEmitterStalenessHorizonS() const { return network_emitter_staleness_horizon_s_; }

        /**
         * @brief Whether the curated RT-service producer profile is on
         *        (@c network.service_mode, RTB-8).
         *
         * One switch that arms the coherent RT-service producer toggle set at
         * load time (clock/URA discontinuity channels + retro-bump, two-state
         * clock model, phase-leveling, both RAIM tiers + the σ̂0 withhold and
         * its per-constellation carve-out, epoch-status heartbeat, pivot
         * vanish-return signalling, sticky pivot, scoped handover response,
         * generation id, disc-counter serialization, v2 S contract +
         * re-broadcast, per-epoch flush, health heartbeat, trace rotation).
         * The bundle wins: it overrides explicit disables of the armed keys, and
         * each override is warned at load; hand-tuning requires running without
         * @c service_mode.  By the time the config is loaded the armed toggles
         * already carry their service values, so at runtime this getter records
         * provenance only: the L15 run manifest (and operator tooling) records
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
         *  than the a-priori stochastic model).  A lower value (e.g. 0) lets σ̂0 fall
         *  below 1 when the network fits inside its a-priori σ; diagnostic only, and it
         *  tightens the gate. */
        double networkRaimSigma0Floor() const { return network_raim_sigma0_floor_; }

        /**
         * @brief Cross-station consensus count for the network RAIM
         *        (@c network.raim_min_stations).
         *
         * A satellite whose carried ambiguities are flagged at @b at-least this many
         * stations is refloated network-wide (all stations, including the faulty one
         * whose slip is hidden in the shared clock).  The consensus rejects
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
         * repeat on the same arc, so it never reaches the threshold.  This
         * suppresses the useless one-off refloats that churn the emitted
         * phase-bias datum (which at 305 stations overwhelmed the consumer)
         * without delaying real slips or touching the corroborated consensus /
         * whole-satellite tiers.  Applies only to the isolated-arc tier of the
         * attribution refloat
         * (@ref networkRaimAttribution); whole-satellite and cross-station
         * consensus refloats are unaffected.  Default 1 = act on the first
         * spike (byte-identical to the ungated behaviour); 2 requires two
         * consecutive epochs, etc.  The per-arc counter resets after a refloat
         * (so a persistent arc refloats at most once per K epochs) and on any
         * epoch the arc is not flagged.
         */
        int networkRaimArcPersistEpochs() const { return network_raim_arc_persist_epochs_; }

        /// @brief Gate isolated-arc RAIM refloats on filter maturity (@c network.raim_refloat_maturity_gate; T2d).
        bool networkRaimRefloatMaturityGate() const { return network_raim_refloat_maturity_gate_; }
        /// @brief Emitted-epoch maturity horizon for the isolated-arc refloat gate (@c network.raim_refloat_maturity_epochs; T2d).
        int networkRaimRefloatMaturityEpochs() const { return network_raim_refloat_maturity_epochs_; }

        /**
         * @brief Minimum network-station-coverage fraction for a signal to enter
         *        the network solve (@c network.signal_min_station_fraction).
         *
         * A unique RINEX observation code (e.g. GPS @c L2S, @c L1W) is admitted
         * to the producer only if at least this fraction of the stations that
         * observe its constellation track it across the network.  This is a
         * network-wide coverage gate; unlike @c channel_mode, which selects a
         * canonical channel @b per station (and so mixes L1W at one station with
         * L1C at another for the same satellite), it drops a sparsely tracked
         * channel everywhere, so every retained signal's
         * satellite bias is estimated from a dense, consistent station set.  The
         * per-band canonical then falls through to the best @b admitted channel
         * (e.g. L1W excluded ⇒ L1C).  Applies to @c PPP_RTK_Network mode only.
         * Default 0 = off (admit every tracked channel; byte-identical).  A
         * typical strict value is 0.5.
         */
        double networkSignalMinStationFraction() const { return network_signal_min_station_fraction_; }

        /**
         * @brief Minimum network-station-coverage fraction for a signal to get an
         *        estimated satellite phase bias (@c network.phase_bias_min_station_fraction).
         *
         * The softer sibling of @ref networkSignalMinStationFraction: instead of
         * dropping a sparsely tracked signal, it keeps the observation but
         * estimates no per-(satellite,signal) phase-bias state for it.  The float
         * ambiguity then absorbs the bias, so the signal still contributes its
         * geometry to the clock/iono/position solve but is not eligible for integer
         * AR (the producer emits no phase bias for it, so a consumer that respects
         * coverage excludes it from AR automatically).  This removes the
         * ill-conditioned, wandering shared phase-bias state (the source of churn
         * and differential instability) without discarding the measurement's
         * information, which is preferable to dropping when the signal is still
         * geometrically useful.  Applies to @c PPP_RTK_Network mode only.  Default
         * 0 = off (every tracked phase signal gets a bias state; byte-identical).  A
         * typical strict value is 0.5.
         */
        double networkPhaseBiasMinStationFraction() const { return network_phase_bias_min_station_fraction_; }

        /**
         * @brief Whether a station whose receiver antenna still has no ATX model
         *        match after the always-on radome→NONE fallback is dropped from
         *        the network at station-build time
         *        (@c network.exclude_unmodeled_antenna_stations).
         *
         * A policy-level backstop.
         * The always-on type-preserving radome→NONE fallback (@ref Antenna::findReceiverAntennaEntry)
         * already recovers every station whose only problem is an uncalibrated radome
         * code on an otherwise-known antenna; a station that still fails to match has an
         * unknown antenna type, so its receiver PCO/PCV is entirely unmodelled.  Unlike
         * measurement noise (which the stochastic model handles) or ionosphere
         * (handled elsewhere), an unmodelled receiver phase centre is a systematic error
         * in every code/phase observation the station contributes, and those observations
         * feed the shared satellite clocks / code biases / phase biases the whole network
         * emits.  When on, such a station is excluded from the network entirely (never
         * built), counted at the build summary, and echoed in the run manifest.  The
         * clock-datum (pivot) station cannot be silently dropped, because it realizes the
         * datum, so an unmodelled pivot is a hard build error directing the user to supply its
         * ATX or choose a different reference.  Applies to @c PPP_RTK_Network mode only.
         * Default false (every station that loads is built; byte-identical).  With the
         * always-on radome fallback the current 305-station networks exclude zero
         * stations, so arming this is inert on known-antenna data and only affects an
         * antenna of unknown type.
         *
         * @see Antenna::hasReceiverAntennaMatch
         */
        bool networkExcludeUnmodeledAntennaStations() const { return network_exclude_unmodeled_antenna_stations_; }

        /**
         * @brief Reduce every roster station's configured coordinate from the
         *        monument to the antenna reference point at setup
         *        (@c network.station_marker_to_arp).
         *
         * A network station is held fixed at @c network.stations[].position_ecef,
         * and that value is a published coordinate (IGS SSC / SINEX, NGS SIF, a
         * surveyed RINEX header), which by convention references the monument.
         * The observations refer to the antenna, and the antenna model
         * (@c igs20.atx phase-centre offsets and variations) is defined relative
         * to the antenna reference point, which sits `ANTENNA: DELTA H/E/N` above
         * the monument.  Holding the station at the monument therefore misfits
         * every range of that station by its eccentricity times the sine of the
         * elevation.  The station's own clock and zenith delay absorb most of it
         * (the zenith wet delay takes about a quarter of H), and what remains is
         * an elevation-dependent pattern the satellite clocks take the regional
         * mean of, so a consumer in the region is pulled down.
         *
         * Armed, each roster station's position becomes
         * @c Coordinates::markerToArp(position_ecef, header H/E/N) once, at
         * setup, before the state vector, the model or any geometry sees it: the
         * same helper and the same frame as the held-out consumer's truth
         * (@ref NetworkSetupResult::cl_truth_arp).  A station whose RINEX header
         * carries no @c ANTENNA: DELTA H/E/N record is left at its configured
         * coordinate and warned; a recorded zero eccentricity is an exact no-op.
         *
         * On the field bed (48 of 59 stations with an eccentricity, median
         * 0.053 m, up to 1.96 m) the reduction removes the held-out consumer's
         * standing Up offset (mean Up within 0.005 m of zero from hour 4 instead of
         * -0.085 m) and about halves the clock-wire error on its satellites
         * (docs/refound/UPO_consumer_up_offset_2026-09-22.md).
         *
         * The held-out closed-loop consumer is not a roster station and is
         * untouched; its truth and its seed are reduced already.  Default false;
         * off is byte-identical (the eccentricities are read and reported, so a
         * run with it off states the size of the offset; invariant #4).
         * Fingerprinted: the carried state of a resumed run means a station at a
         * different place, so a warm resume across the flip cold-starts.
         *
         * @see Coordinates::markerToArp, RinexParser::hasAntennaDeltaHen
         */
        bool networkStationMarkerToArp() const { return network_station_marker_to_arp_; }

        /**
         * @brief Write the arc ledger's trace (@c network.signal_roster,
         *        a trace switch).
         *
         * The per-station signal roster, the arc ledger and
         * their disagreement census are built on every network run, and a "signals as
         * planned" table is logged and written to the run manifest.  This key decides
         * only whether the ledger's trace is written: @c logs/network_arc_ledger.csv
         * (one row per arc transition) and the @c ARC event rows of the roster's build
         * and audit in @c network_events.csv.  It keeps its name, so a config that
         * arms it loads and gets the trace it asked for.  The readers of the roster
         * (@ref networkRosterTracker, @ref networkLedgerReentry,
         * @ref networkLedgerScreens, @ref networkLedgerEmit) no longer need it.
         *
         * Why the roster exists: the signal-identity audit
         * (`runs/_msc/sid_audit_report_2026-09-22.md`, summarized in
         * docs/refound/SID_signal_identity_2026-09-22.md) lists 128 sites
         * in 38 files that decide which signal stands for a band, under at least nine
         * rules and six independent implementations of "the two lowest distinct
         * bands", and arc-keyed lifecycle holders with both edges silent.  On the
         * field baseline the arc-level slip tracker's band signal differed from the
         * carried one at 46 station-band cells of 44 of 58 stations, none of 31
         * band-1 re-entries at those cells was answered, and the wire's L1W
         * phase-bias family was absent for 23.6 per cent of satellite-epochs.
         *
         * One call in the epoch body fills @c NetworkRunContext::roster from the
         * station canonical latch, the epoch's raw records and the config, and
         * advances @c NetworkRunContext::arc_ledger; a second, read-only pass counts
         * every disagreement between the roster or the ledger and the site that
         * currently decides the same question.  With no SID reader armed nothing
         * else reads either object, so the solve, the wire and every graded trace are
         * byte-identical with this key on and off (invariant #6 in both states).
         *
         * Not fingerprinted: no carried quantity changes meaning across the flip, so
         * the key is legal on a warm resume (the trace-class knob precedent).
         * Network mode only.  Default false.
         */
        bool networkSignalRoster() const { return network_signal_roster_; }

        /**
         * @brief The named shared columns of the attribution's solution-share readout
         *        (@c network.rhs_attrib_columns).
         *
         * A list of shared-column labels as `logs/rhs_attrib/vec_<tow>.csv` prints them
         * ("CLK:G07", "CB:G07:C1W", "PB:G07:L1C").  When it names any, every
         * attribution dump (the armed @c network.rhs_attrib_tow window and the flight
         * recorder's trip dumps) also writes `cols_<tow>.csv`: for every (station,
         * contribution kind), every (station, kind, satellite, signal), every shared fold
         * kind, the DD folds and the datum pins, the reduced right-hand-side
         * contribution at each named column and its solution share there under the
         * solve's own datum, with the kind totals and a closure line per column
         * (@c networkRhsAttribColumnShares, network/qc/RhsAttrib.hpp).  A label that
         * names no shared column at a dump epoch keeps its slot, empty, and is named in
         * the log.
         *
         * A label `LOC:<station>:<key>` names a station-local column instead
         * ("LOC:MAJU00MHL:GPS_DCB", "LOC:MAJU00MHL:CLK", "LOC:MAJU00MHL:IONO:G08"; the key
         * spellings are those of @c rhsAttribResolveLocalKey): the armed dumps and a trip's
         * current-epoch dump then also write `loc_<tow>.csv`, the decomposition of the
         * station's solved increment of that column into its data rows, its local fold
         * kinds, the shared feedback (split by station, with each station's net) and the
         * constraint corrections, with closure lines and checks of the single parts, and
         * the same for the station's geometry-free level direction
         * (@c networkRhsAttribLocalShares).  A trip's retained previous frame
         * holds no solve @c x_s and writes no `loc_` file (the loader warns when the trip
         * recorder is the only dump armed).  A local label that does not resolve keeps its
         * slot and is named once in the log.
         *
         * Diagnostics only, the same class as @c network.rhs_attrib_tow: inert without a
         * dump, never read by the solve, not fingerprinted, so it is legal on a warm
         * resume.  The loader keeps the string entries of a JSON array, in order and
         * without duplicates, and refuses anything else with an error (the list then
         * stays empty).  Network mode only.  Default empty: no `cols_` or `loc_` file.
         *
         * @return The labels, in the order the dump writes them.
         */
        const std::vector<std::string>& networkRhsAttribColumns() const {
            return network_rhs_attrib_columns_;
        }

        /**
         * @brief The arc-level slip tracker reads the roster
         *        (@c network.roster_tracker).
         *
         * Phase 1 builds the roster as an observatory; armed, this knob makes the
         * tracker its first reader: @c networkComputeRawObservables names each band by the
         * roster's @c modelled_phase for that (station, constellation, band) when
         * that phase is reported with its matching code this epoch, and falls back
         * to the lexical rule only on a band the roster names none for.  The
         * per-signal tier of the gap re-entry response is untouched (it keys on the signal).
         *
         * It replaces @c network.cycle_slip.tracker_modeled_first by
         * construction: the roster's modelled role is the canonical latch that knob
         * reads (phase 1's @c roster_latch_disagree, bar 0), so the two name the same
         * signal at every cell and a run with both armed is byte-identical to a run
         * with either.  Both may be armed; the roster read supersedes the latch
         * lookup and the loader says so.
         *
         * The roster it reads is built on every network run, so
         * the knob needs no other; the run-end counters say how many lookups the
         * roster answered and how many fell back to the latch.
         *
         * Not fingerprinted: the tracker's own history is exempt from the state
         * snapshot and no carried quantity changes meaning
         * across the flip.  Network mode only.  Default false.
         */
        bool networkRosterTracker() const { return network_roster_tracker_; }

        /**
         * @brief Drive the gap re-entry response from the arc ledger's admitted
         *        lifecycle (@c network.ledger_reentry).
         *
         * Armed, the ledger defines the population of the response: every arc whose
         * admitted state goes Gap to Reentered at this epoch, whose signal the
         * roster calls Modelled, and whose gap is inside
         * @c network.phase_leveling_max_gap_s (the window the PASS-B inject folds a
         * level across). The raw tiers keep running unchanged, and their verdicts
         * are looked up per arc: the row-level tier's resumption verdict for the
         * arc's satellite, the per-signal tier's for the arc's signal. An arc
         * neither tier saw has no verdict and is handled by
         * @ref networkLedgerReentryNoVerdict.
         *
         * Solve-affecting when armed: it changes which carried levels survive a
         * break. Graded as a behaviour change, never as a refactor.
         *
         * Refused (error and disarm) without @c network.roster_tracker: without the
         * roster read the tracker's rows would still name bands by a different rule
         * than the population does.  The ledger it reads is built on every network
         * run.
         *
         * Fingerprinted (armed-only append): a warm resume
         * across the flip would restore carried levels that the other side of the
         * flip would have refloated. Network mode only. Default false.
         */
        bool networkLedgerReentry() const { return network_ledger_reentry_; }

        /**
         * @brief What the re-entry response does with an arc that has no raw
         *        verdict (@c network.ledger_reentry_no_verdict).
         *
         * @c Bridge (default) keeps the behaviour without the ledger but makes it
         * visible: the arc is left to the PASS-B inject and is counted and evented
         * as @c ARC @c arc_reentry with @c verdict=none. @c Refloat is the
         * conservative response: the arc is treated as unmeasurable, which the existing
         * @c network.cycle_slip.gap_reentry_relevel_unmeasurable sub-knob then
         * decides. Inert without @ref networkLedgerReentry.
         */
        LedgerReentryNoVerdict networkLedgerReentryNoVerdict() const {
            return network_ledger_reentry_no_verdict_;
        }

        /**
         * @brief Reason-mask writers and the anchor observatory
         *        (@c network.ledger_screens).
         *
         * Armed, every screen that removes a modelled row, a satellite or a
         * station records the arcs it removed and its own reason bit into the
         * run context's per-epoch buffer, and the roster's audit pass folds that
         * buffer into the ledger so an admitted gap carries the reason(s) that
         * opened it. The anchor election is read against the ledger at the same
         * time: a first sighting whose family has arcs in @c Reentered or @c Gap
         * is counted and evented, each coverage-gate drop edge reports its arcs'
         * states and the generation test at the family's return, and the
         * @c anchor_arc_age container is compared with the ledger's own age.
         *
         * Observatory: nothing in the solve reads any of it, so the solve, the
         * wire and every graded trace are byte-identical armed and disarmed
         * (invariant #6 in both states). The ledger it folds into is built on every
         * network run. Not fingerprinted. Network mode only.
         */
        bool networkLedgerScreens() const { return network_ledger_screens_; }

        /**
         * @brief The anchor election treats a return as a return
         *        (@c network.ledger_anchor).
         *
         * Armed, a family the coverage gate's drop edge erased and that returns
         * with arcs of the same generation restores its previous incumbent as the
         * candidate of record, subject to the election's existing rules, instead
         * of being elected afresh as a first sighting; a return with new
         * generations is elected as it would be without the knob. Both outcomes are
         * events (@c PHASE_ANCHOR @c reentry_restore / @c reentry_new) with
         * counters. The @c anchor_arc_age container becomes a reader of the
         * ledger's admitted age at the same time.
         *
         * Solve-affecting when armed (the election's memory across a gap changes
         * what the carried anchor means), so it is graded as a behaviour change
         * and it is fingerprinted (armed-only append).
         * Refused (error plus disarm) without @ref networkLedgerScreens.
         * Network mode only.
         */
        bool networkLedgerAnchor() const { return network_ledger_anchor_; }

        /**
         * @brief The emitter read against the ledger, and the
         *        producer's declaration of its roster on the wire
         *        (@c network.ledger_emit).
         *
         * Armed, two comparisons run at every emitted epoch: the phase-bias hold's
         * generation against the ledger's admitted arc births (a new generation
         * with no birth behind it, and a birth the generation never reports, are
         * separate counted classes with @c ARC events), and the emitter's own
         * first-sighting test against the ledger's memory of the family (a return
         * read as a first sighting is named). At the same time the producer writes
         * a block of @c N records declaring, per constellation and RINEX band
         * digit, the modelled phase and code signals it carries, the second-code
         * companion codes and the S-basis datum designation.
         *
         * Observatory: nothing on the producer reads either back, so the solve and
         * every C / B / P / E record are byte-identical against an unarmed run;
         * the only difference in @c products.txt is the @c N records. Its two
         * inputs, the roster and the ledger, are built on every network run.
         * Not fingerprinted. Network mode only.
         */
        bool networkLedgerEmit() const { return network_ledger_emit_; }

        /**
         * @brief The consumer's band latch and its freshness response follow the
         *        declared roster
         *        (@c network.closed_loop.roster_latch).
         *
         * Consumer-side. Armed, the consumer's canonical signal per band is taken
         * from the producer's declaration when the consumer tracks a declared
         * signal (the priority rule keyed to the declaration, the
         * declaration's own order), falling back to the consumer's own rule
         * otherwise; and a fresh-to-stale freshness flip on a signal the
         * declaration does not name is not treated as a staleness event, because
         * the network structurally carries no family for it. A family the network
         * does carry keeps the unchanged response.
         *
         * Inert (logged once) when the wire carries no declaration, which is every
         * producer that did not arm @ref networkLedgerEmit. Derived onto the
         * closed-loop consumer config; the producer run is untouched.
         */
        bool consumerRosterLatch() const { return consumer_roster_latch_; }

        /**
         * @brief Whether RAIM refloats are driven by prominence attribution
         *        (@c network.raim_attribution) instead of the per-satellite consensus.
         *
         * When on, each RAIM epoch's post-fit residuals are decomposed by a robust 2-way
         * median polish into satellite / station / isolated-arc effects, and the refloat
         * is targeted at the dominant dimension: a bad satellite refloats network-wide, a
         * bad station refloats only that station's arcs, and an isolated arc refloats just
         * that (station, sat), instead of refloating every station of any
         * consensus-flagged satellite.  The gate is @c effect/σ̂0 > @ref networkRaimKSigma per dimension.
         * Default false keeps the per-satellite consensus behaviour.
         */
        bool networkRaimAttribution() const { return network_raim_attribution_; }

        /**
         * @brief Run the post-fit RAIM screen on the invariant-carry lineage
         *        (@c network.raim_carry_invariant; pool re-host).  Measure-only.
         *
         * Without this knob, post-fit RAIM is structurally inert under
         * @ref networkCarryInvariant for two independent reasons, neither a tunable
         * threshold:
         *
         *  1. The seam is not called: its call site requires `carry_warm`, and
         *     `carry_warm` requires @ref networkPhaseLeveling, so a lineage that
         *     leaves leveling off never executes `networkPostFitRaim`.  With this
         *     knob the call site also admits the seam (`carry_warm || raim_ci_on`).
         *  2. Even with leveling on, the residual pool is empty.  Every RAIM walk keys
         *     on `StationAssembly::pl_amb`, whose only fill site sits inside
         *     `do_amb_carry = (…) && !carry_invariant` (the DD carry replaces the
         *     phase-leveling ambiguity carry as a solve input), so the record list
         *     that hosts RAIM's QC substrate is never built.
         *
         * The problem is where the enumeration lives, not the statistics: the
         * enumeration RAIM needs (which ambiguity columns are present, unpinned and
         * checkable this epoch) is carry-agnostic, but it lives on the phase-leveling
         * carry's bookkeeping vector.  This flag re-hosts it onto
         * @c StationAssembly::qc_amb, a carry-vehicle-neutral census built in PASS B,
         * and points the walks at it.
         *
         * @warning This knob is measure-only by design but is @b not emission-neutral.
         *          With the pool empty, `sigma0` never leaves its 1.0 initializer; filling
         *          it makes σ̂₀ real, and σ̂₀ feeds both the A4 withhold
         *          (@ref networkRaimWithholdSigma0) and the URA EWMA (@ref networkEmitUra).
         *          The first run with it on is graded as emission-affecting.
         *
         * @note The screen has little power on this lineage.  Under phase-leveling a
         *       carried ambiguity is pinned by a stiff prior, so a missed slip cannot be
         *       absorbed and shows as a post-fit residual jump (RAIM's stated mechanism).
         *       Under the invariant carry the local ambiguity column is free each epoch and the
         *       time-link lives in the DD chord's prior, so the tension is absorbed
         *       locally and does not reach the column's residual (measured: post-fit
         *       phase residuals carry no information there; SPEC_raim_pool_rehost
         *       §7.2/§7.3).  Re-hosting makes RAIM runnable, not powerful; the refloat
         *       actuator that once rode on it was retired.
         *
         * Requires @ref networkCarryInvariant and @ref networkRaim (loader forces off
         * otherwise).  Off = byte-identical: this knob fills no @c qc_amb (only the
         * continuous phase-datum handover can also fill it), and the pool selector
         * falls through to @c pl_amb whenever @c qc_amb is empty.
         *
         * @see docs/SPEC_raim_pool_rehost.md
         */
        bool networkRaimCarryInvariant() const { return network_raim_carry_invariant_; }

        /**
         * @brief Restrict the carried-phase RAIM pools to observation phase rows
         *        (@c network.raim_pool_data_rows_only;
         *        docs/refound/RPA_raim_pool_data_rows_2026-10-08.md).
         *
         * Both pools (the post-fit pass, network/qc/Raim.cpp, and the within-epoch
         * assess, network/solve/SolveGlue.cpp) walk the rows of a station block once per
         * carried ambiguity column.  Off, a row enters a column's pool when its
         * coefficient on the column exceeds 0.5 and its kind is one the declared rule
         * admits (@c raimPoolAdmitsKind, network/assembly/RowClassify.hpp): the observation
         * phase rows, the coupling and whitened block carried-prior rows and the SD-EWL
         * rows.  On the float vehicle about 90 % of the pool's samples are then
         * carried-prior rows, and they decide
         * most T4 isolated-arc refloats.
         *
         * On, a row enters a column's pool only when it is an observation phase row
         * (@c data_phase, @c data_phase_second) whose tag names the column's satellite
         * and signal; the carried-prior and SD-EWL rows enter no pool.  The σ̂0 pool, the
         * per-satellite consensus (T3), the median-polish attribution (T4) and the
         * within-epoch adaptation then read observation residuals only, as do the
         * measure-only riders on the post-fit walk (the MDB feed, the data side of the
         * constraint-aware global σ̂0, the station factor and the redundancy census).  The
         * prior side of the prior-residual census is fed elsewhere and is unchanged.  The
         * run-end admission lines count the refused kinds and cross-check, per phase row,
         * that the column its tag names is the column on which its coefficient exceeds
         * 0.5 (0 disagreements expected).
         *
         * A QC knob: it moves refloat decisions, not the state layout, so it is not in
         * the warm-resume fingerprint (as for the other RAIM knobs).  It changes the
         * solve from the first refloat it moves.  Requires @ref networkRaim (the loader
         * disables it otherwise).  Off = today's rule, byte-identical.
         *
         * @return True when the pools admit observation phase rows only.
         */
        bool networkRaimPoolDataRowsOnly() const { return network_raim_pool_data_rows_only_; }

        /**
         * @brief Whether a "so bad" epoch triggers within-epoch adaptation
         *        (@c network.raim_within_epoch).
         *
         * When the robust a-posteriori variance factor σ̂0 exceeds @ref
         * networkRaimSigma0Trigger the joint solve is contaminated enough that emitting it
         * would be wrong; this arms the detect→drop-culprit→re-solve adaptation.  The
         * adaptation runs only on carry-warm epochs, so it is inert without
         * @ref networkPhaseLeveling.  Default false.
         *
         * @note A satellite/arc culprit is handled by refloating its carried ambiguity: the
         * re-solve withholds the stale carry prior so the ambiguity re-estimates from full-weight
         * data (the network analogue of a large-q reinitialization) and absorbs the slip locally,
         * and the harvest re-stashes the corrected value.  This is self-contained (no lock-in even
         * at zero carry process noise) and keeps the ambiguity observable (no drop-induced
         * singularity).  A whole-station culprit (a data-quality problem, not a slip) is instead
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
         * @brief σ̂0 ceiling above which a within-epoch adaptation failure withholds the
         *        epoch instead of emitting it (@c network.raim_withhold_sigma0, A4; used
         *        only when @ref networkRaimWithinEpoch).
         *
         * After the capped drop-culprit/re-solve adaptation, if σ̂0 still exceeds this
         * ceiling the shared solution is too contaminated to publish; it is withheld
         * through the same disposition as the observability gate (no emit, no carry
         * stash, epoch skipped) so the contaminated solve never enters the store or the
         * carry.  0 (default) = off: the epoch is emitted anyway with a warning.  A
         * sensible value is ~2× @ref networkRaimSigma0Trigger.
         */
        double networkRaimWithholdSigma0() const { return network_raim_withhold_sigma0_; }

        /**
         * @brief Escalated within-epoch adaptation budget on withhold-era epochs
         *        (@c network.raim_withhold_adapt_escalate;
         *        `SPEC_withhold_era_release.md` §3(a)).
         *
         * When the previous epoch was A4-withheld (the era-continuity state, C5/C7 of
         * the SPEC), the within-epoch drop-culprit/re-solve loop runs up to this many
         * iterations instead of the stock cap (5).  Nothing else changes (same culprit
         * scoring, same k-gate, same σ̂0, same ceiling): a successful escalation emits
         * through the normal accepted path (§1.5: harvest + post-fit commit), and an
         * exhausted one withholds as it would without the knob.  Motivation: in a
         * 29-epoch field withhold era, n_adapt sat at the cap on every epoch with
         * culprits still clearing the k-gate.  0 (default) = off (byte-identical).  Values in (0, 5]
         * are rejected to 0 at load (an "escalation" at or below the stock budget is a
         * no-op that would read as armed).  Inert without a withhold ceiling.
         */
        int networkRaimWithholdAdaptEscalate() const { return network_raim_withhold_adapt_escalate_; }

        /**
         * @brief Erase detector-freed leveling carries AT the A4 withhold site
         *        (@c network.raim_withhold_free_erase;
         *        `SPEC_withhold_era_release.md` §3(b)).
         *
         * Without it, a slip detected on a withheld epoch loses its response: the
         * free-epoch latch is consumed against a solve that is then discarded, the
         * completing harvest is skipped, and the latch is cleared at the next PASS-A,
         * so the next accepted epoch re-injects the stale pre-slip level at converged
         * weight (the QUEUE-9 walk mechanism, re-opened through the withhold path;
         * observed in a field withhold era).  With this armed, the withhold
         * branch erases the freed (station, sat, signal) entries from the leveling
         * carry so carry-absence makes the free re-level durable to the next accepted
         * epoch.  Reset-class only (no value from the contaminated solve is stored);
         * delta-bridged bands are structurally excluded (removed from the latch before
         * this site).  Requires @c cycle_slip.relevel_free_epoch and a withhold
         * ceiling; off = byte-identical.
         */
        bool networkRaimWithholdFreeErase() const { return network_raim_withhold_free_erase_; }

        /**
         * @brief Extend the within-epoch screening to the code and iono-constraint
         *        rows (@c network.raim_we_code_iono, WE-CI; used only when
         *        @ref networkRaimWithinEpoch).
         *
         * Without it, the within-epoch adaptation's detection pool is the
         * carried-ambiguity phase rows only; code rows and the per-LOS↔SH iono
         * constraint rows are screened by no tier (a field SH event entered through
         * exactly those rows).  When armed, each assess() additionally pools the
         * code rows (CB-column-touching, non-phase) and the iono rows
         * (SH-column-touching) into two per-pool robust σ̂0 statistics with their own
         * triggers (@ref networkRaimWeCodeTrigger / @ref networkRaimWeIonoTrigger).
         * A trip adds two response classes to the same capped drop→re-solve loop:
         * the dominant station is excluded for the epoch, or the dominant
         * satellite's code rows are de-weighted network-wide (σ×10, a fallback, not
         * a drop).  The phase pool, σ̂0_adapt, and the A4 ceiling are unchanged; off
         * (default) is byte-identical everywhere.
         */
        bool networkRaimWeCodeIono() const { return network_raim_we_code_iono_; }

        /**
         * @brief CODE-pool σ̂0 trigger for WE-CI (@c network.raim_we_code_trigger;
         *        used only when @ref networkRaimWeCodeIono).
         *
         * Empirically derived (field day, 305 stations): quiet code-pool σ̂0 runs
         * ~2.5–4.7 with q99 ≈ 8, while three events read 17.8, 28.0 and 288.2, so
         * the default 10.0 separates them cleanly.  Per-pool statistic:
         * the code rows' own 1.4826·median|v·√w| (B2-normalized when armed).
         */
        double networkRaimWeCodeTrigger() const { return network_raim_we_code_trigger_; }

        /**
         * @brief IONO-pool σ̂0 trigger for WE-CI (@c network.raim_we_iono_trigger;
         *        used only when @ref networkRaimWeCodeIono).
         *
         * The iono-constraint rows (per-LOS iono ↔ SH field coupling) have no other
         * screening statistic, so this default (12.0) is a
         * conservative first placement; the run-end WE-CI telemetry reports the
         * maximum observed pool σ̂0 so the trigger can be calibrated from the
         * first armed run (MW-thresholds discipline: empirical, never σ-model).
         */
        double networkRaimWeIonoTrigger() const { return network_raim_we_iono_trigger_; }

        /**
         * @brief Per-constellation σ̂0 carve-out of the A4 withhold
         *        (@c network.per_const_withhold, RTA-10; used only when
         *        @ref networkRaimWithholdSigma0 > 0).
         *
         * When the whole-epoch σ̂0 withhold trips, recompute σ̂0 restricted to each
         * constellation's carried-ambiguity phase rows (the same standardized-residual
         * pool the global σ̂0 is the median of, partitioned by satellite constellation)
         * and publish the constellations that individually pass the same ceiling with
         * at least @ref networkPerConstWithholdMinRows rows, so a localized fault (e.g.
         * one constellation's clock product) reduces scope instead of availability.
         * Constellations failing either test are pruned from the emission like the
         * RT0-5 runaway quarantine; if none passes, the whole-epoch withhold applies
         * unchanged.  The rank/observability gate keeps whole-epoch semantics (a rank
         * deficiency is an invalid datum, so no healthy subset exists).  Default off ⇒
         * the A4 withhold is all-or-nothing (byte-identical).
         */
        bool networkPerConstWithhold() const { return network_per_const_withhold_; }

        /**
         * @brief Minimum standardized-residual rows a constellation needs to be
         *        judgeable by the per-constellation carve-out
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
         * product file, including fully withheld epochs (observability-gate and A4
         * σ̂0 withholds), which otherwise emit nothing, so a standalone tail consumer
         * can distinguish "producer withheld" from "producer died":
         * @c H,week,tow,status,sigma0,n_sats_published,withheld with
         * status ∈ {ok, partial, withheld_sigma0, withheld_gate}.  Like the S contract
         * line it is positional and never feeds the replay emitter.  Coverage starts
         * at the first emit (the deferred product writer opens then).  Default off ⇒
         * no H records (byte-identical product file).
         */
        bool networkEmitEpochStatusRecord() const { return network_emit_epoch_status_record_; }

        /**
         * @brief Census-fed H heartbeat σ̂₀ (@c network.h_sigma0_prefit_census,
         *        docs/HANDOFF_edge_response_queue.md §1.6 item 3a; H-rider).
         *
         * Without this knob, the H record's sigma0 field is fed by the within-epoch
         * RAIM's σ̂₀ (@c sigma0_adapt), which is computed only inside
         * @c if(within_epoch_armed && carry_warm), so on every lineage without
         * leveling the wire carries a constant 0.0000 ("unavailable"), and on a
         * quiet leveled run a degenerate 1.0000; the emit path then has no usable
         * post-hoc residual statistic.
         *
         * When on, every H site instead carries the prefit census's pooled
         * robust σ̂₀ (@c networkPrefitSigma0: 1.4826·median of the per-station
         * common-mode-removed standardized deviations over
         * @ref networkHSigma0Classes, computed before the solve so even the
         * observability-gate withhold's H has a value).  This is a different
         * statistic from the RAIM one (pre-solve and common-mode-removed, against
         * post-adaptation and redundancy-normalized), so the S contract advertises
         * it (@c hsig=pc) and a consumer thresholding σ̂₀ across producer
         * versions can tell the populations apart.  It never feeds the A4
         * withhold ceiling: that gate reads @c sigma0_adapt only.
         *
         * Requires @ref networkEmitEpochStatusRecord (no H records otherwise)
         * and @c network.trace_prefit_census (the born-cell deferral reads the
         * census walk's lag-1 presence map); the loader forces this knob off
         * with a warning when either is missing.  Changes product-file bytes when
         * armed (H field 5 and the S line's hsig token), so the default is false.
         */
        bool networkHSigma0PrefitCensus() const { return network_h_sigma0_prefit_census_; }

        /**
         * @brief Data classes pooled into the census-fed H σ̂₀
         *        (@c network.h_sigma0_classes, default "code,sh,other").
         *
         * Same token grammar as the census domains (@c parseDataClassMask):
         * code / phase / other / sh / all / none.  The default matches the
         * census event domain; @c obs_phase stays out because phase prefit
         * innovations are arc-specific, not a common-mode population (census
         * deviation 2c), and pooling them would dilute the statistic with arc
         * geometry.  Only used when @ref networkHSigma0PrefitCensus is on.
         */
        const std::string& networkHSigma0Classes() const { return network_h_sigma0_classes_; }

        /**
         * @brief Whether the network RAIM standardizes by per-row redundancy
         *        (@c network.raim_redundancy_normalize, B2).
         *
         * When on, each carried-ambiguity phase row's residual is divided by
         * @f$\sigma_i\sqrt{r_i}@f$ with the Baarda redundancy number
         * @f$r_i = 1 - w_i\,a_i^\top \mathrm{Cov}(\theta)\,a_i@f$ computed from the cached
         * per-station Schur factors (@f$N_{rr}^{-1}, N_{sr}@f$) and the datum-frame shared
         * covariance (no re-solve) before the σ̂0 pooling and median-polish attribution.
         * A low-redundancy arc (few checking stations), whose residual response is only
         * @f$r_i\cdot\text{fault}@f$, is otherwise systematically under-flagged.  Default
         * false reproduces the plain @f$|v|\sqrt{w}@f$ standardization bit-for-bit.
         */
        bool networkRaimRedundancyNormalize() const { return network_raim_redundancy_normalize_; }

        /**
         * @brief Declare the per-station variance factor as its own statistic
         *        (@c network.raim_station_factor; `SPEC_t3_null_derivation.md`
         *        §6/§7.5; QUEUE item 4 R2).
         *
         * Measure-only: per emitted epoch, each present non-pivot station gets the
         * trimmed variance component σ̂²_sta = Σw·v²/Σr_i over its post-fit RAIM rows
         * (spike rows above k·σ̂₀ trimmed from both sums), EWMA-tracked as the slow
         * channel (one station-epoch is ~24 % noise, §6) and declared (trace rows,
         * both-edge depart/return events at the ratio threshold, counters), never
         * absorbed into any denominator (§7.8.2: absorption would hide a measured
         * 0.11–2.55 m per-station iono error inside the noise model; that option,
         * R1, is held).  Computes r_i via the solve-neutral rowRedundancy read (the
         * census disjunct).  Requires @ref networkRaim; off = byte-identical.
         */
        bool networkRaimStationFactor() const { return network_raim_station_factor_; }

        /**
         * @brief R2 EWMA time constant, seconds (@c network.raim_station_factor_tau_s).
         *
         * §12.1b: the slow factor must track (half-to-half transferability falls with
         * window length), and the right constant is not yet measured; this knob
         * exists to measure it.  Default 1800 s (the settled-warmup scale).
         */
        double networkRaimStationFactorTauS() const { return network_raim_station_factor_tau_s_; }

        /**
         * @brief R2 departure threshold (@c network.raim_station_factor_ratio): a
         *        station whose EWMA factor exceeds this ratio to the network median
         *        of EWMA factors gets a depart event (hysteresis return at 0.8×).
         *
         * Default 6.0, above the 4.7× model-error spread measured in §4, so the
         * declaration flags genuine station departures, not the (declared,
         * traced) heterogeneity itself.
         */
        double networkRaimStationFactorRatio() const { return network_raim_station_factor_ratio_; }

        /**
         * @brief T3/T3c population hygiene (@c network.raim_rate_hygiene;
         *        `SPEC_t3_null_derivation.md` §6; QUEUE item 4 R4).
         *
         * Two accounting fixes to the flag-rate telemetry, both measure-only:
         * structurally untestable T3 cells (s_raw exactly 0: a carried ambiguity
         * with no phase row this epoch, 1.67 % on the fitted arm) leave the T3
         * opportunity count (counted separately), and T3c's opportunity becomes the
         * unconditional population (satellites with cells at ≥ raim_min_stations
         * stations) so its implied_alpha is like-for-like instead of comparing a
         * conditional rate against an unconditional α.  Requires
         * @ref networkRaim + trace_flag_rates (the counters it corrects);
         * off = byte-identical.
         */
        bool networkRaimRateHygiene() const { return network_raim_rate_hygiene_; }

        /**
         * @brief Internal per-epoch level-authority census
         *        (@c network.level_authority_csgf;
         *        `SPEC_level_authority_internal.md`; LV-6).
         *
         * Measure-only: per epoch, the mature CS-GF states reduce to one fleet
         * level L_cs(t) (per-station information-weighted means, fleet median;
         * `levelAuthorityReduce`), traced with the per-station means (the
         * d_r,GF-class diagnostic).  Folds nothing: the fold is a future step
         * gated on this census's §4 tracking verdict.  Arms the CS-GF advance
         * independently of the (falsified, off) iono-carry inject path.  Any
         * census A/B pair must pin `cycle_slip.gap_instrument` in both arms so
         * `decisionInstrumentArmed()` is equal across the pair (§5.3: the qualify pass's
         * verdicts feed dormant D5 consumers on corroborate-armed lineages).
         * Off = byte-identical; on = solve-byte-identical (trace only).
         */
        bool networkLevelAuthorityCsgf() const { return network_level_authority_csgf_; }

        /// Minimum CS-GF sample count for a LOS to enter the census
        /// (@c network.level_authority_min_n, default 10: the kernel's √K
        /// maturity, ~5 min at 30 s).
        int networkLevelAuthorityMinN() const { return network_level_authority_min_n_; }

        /// Minimum qualifying LOS for a station to enter the fleet median
        /// (@c network.level_authority_min_los, default 4).
        int networkLevelAuthorityMinLos() const { return network_level_authority_min_los_; }

        /**
         * @brief Dump the census's own per-LOS pierce points
         *        (@c network.level_authority_los_dump; LV-6 G2 support).
         *
         * Writes `network_level_authority_los.csv`: one row per (station,
         * satellite) row that entered tier 1, carrying the IPP latitude /
         * longitude, the thin-shell mapping, the vertical TECU and the
         * information weight the reduction actually used.
         *
         * Purpose: §4's G2 grades L_cs(t) against censusOperator{GIM}(t) (the
         * reference field pushed through the same two-tier reduction), and §5
         * rejected the naive fleet-median-vs-planetary-mean grade because an
         * unmatched sampling term measures the roster's geography instead of the
         * level.  The census CSV publishes the station set and each station's
         * tier-1 mean, but not the pierce points or the weights, so an offline
         * reference could only re-derive a LOS set (all visible satellites) that is
         * not the one the census used (mature CS-GF states only).  This dump
         * removes that approximation: the reference is evaluated at exactly the
         * same IPPs with exactly the same weights.
         *
         * It is its own file, never extra columns on the census CSV, so the
         * FLEET/STA rows stay byte-identical and G3's certification is untouched.
         * Trace-only and O(stations x LOS) per epoch; inert unless
         * @ref networkLevelAuthorityCsgf.
         */
        bool networkLevelAuthorityLosDump() const { return network_level_authority_los_dump_; }

        /**
         * @brief Whether the within-epoch adaptation drops the finest contaminated unit
         *        (@c network.raim_surgical_station; used only when @ref networkRaimWithinEpoch).
         *
         * The robust median polish separates a contaminated satellite (whole column), a
         * contaminated station (whole row), and an isolated (station, satellite) arc
         * (interaction term).  With this on (default) the within-epoch re-solve can peel a
         * single bad arc (de-weighting just that station-satellite's rows) instead of
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
         * over the best satellite/arc candidate when its median-polish row effect, divided by
         * this factor, still dominates, i.e. it must be @c raim_station_drop_bias× worse than
         * any single satellite or arc before the whole station is dropped.  Must be ≥ 1;
         * 1 disables the bias (station competes at face value).  Default 1.5.
         */
        double networkRaimStationDropBias() const { return network_raim_station_drop_bias_; }

        /**
         * @brief Whether the per-satellite carried-clock sanity monitor is armed
         *        (@c network.clock_monitor).
         *
         * The post-fit σ̂0 RAIM catches contamination that moves the shared residual level
         * (many unaffected stations jump at once); it does not see a lone satellite whose
         * carried clock has locked into a physically impossible value: the clock absorbs the
         * error self-consistently, so σ̂0 stays ≈1 and the stiff @c sat_clock_q carry then
         * freezes the wrong value for the rest of the run (the multi-hour single-satellite
         * lock-in).  This monitor is the detector for that mode: on the final
         * (post-adaptation) solution it flags any satellite whose de-meaned clock exceeds
         * @ref networkClockMonitorAbsMaxM or whose one-epoch jump exceeds @ref
         * networkClockMonitorJumpMaxM, then reinitializes its carried shared state (the
         * shared-parameter analogue of the ambiguity refloat).  By default the next epoch
         * folds the clock (and its coupled phase/code bias) at the loose variance of
         * @ref networkClockMonitorReinitSigmaM instead of the stiff carry; with that σ ≤ 0
         * the state is not re-stashed at all, so no stale prior is injected and the clock
         * re-estimates freely toward its data/anchor value.  With
         * @ref networkClockMonitorCoupleRaim it also reinitializes the carried clock of any
         * satellite the within-epoch / post-fit RAIM already flagged, so a slip that leaked
         * into the shared clock reinitializes the clock too, not just the ambiguity.  Default
         * false ⇒ byte-identical (no clock carry is ever reinitialized).
         */
        bool networkClockMonitor() const { return network_clock_monitor_; }

        /**
         * @brief De-meaned per-satellite clock magnitude (m) above which the carried clock is
         *        refloated (@c network.clock_monitor_abs_max_m; used with @ref networkClockMonitor).
         *
         * The test is on @c |Δδtˢ − median(Δδtˢ)| so the per-epoch datum/gauge offset (a
         * few metres, not error) is removed and only real per-satellite excursions remain.  A
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
         * the lock-in onset is a single-epoch jump of hundreds of metres.  Tested only across
         * adjacent epochs of a stable datum (skipped on a pivot handover and right after a
         * refloat, which re-baselines).  Default 10 m (~100× a healthy epoch step).
         */
        double networkClockMonitorJumpMaxM() const { return network_clock_monitor_jump_max_m_; }

        /**
         * @brief Whether the clock monitor also refloats the carried clock of any satellite the
         *        within-epoch / post-fit RAIM already flagged (@c network.clock_monitor_couple_raim).
         *
         * The ambiguity RAIM tiers flag a satellite whose slip leaked into the shared clock; with
         * this on, that satellite's carried clock (and coupled phase/code bias) is reinitialized
         * alongside its ambiguity rather than left frozen.  It is off by default because those
         * tiers fire on false-positive-like flags on a clean network (product-neutral for the
         * ambiguity refloat, but it would needlessly churn clock carries) and because the target
         * failure (a lone lock-in) is below the RAIM trigger and caught by the monitor's own
         * absolute/jump bounds regardless.  Off ⇒ the monitor acts only on its own bounds, so a
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
         * not withheld from the stash; instead the next epoch folds it with @c var = this σ²
         * rather than the stiff @c sat_clock_q: a soft large-q reinitialization.  The weak prior
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
         * normalized innovation (a detectable dynamics fault) rather than an invisible
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
         * predicted offset variance and q2·dt to the drift variance.  It is obtained by inverting the
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
         * The R in the model's measurement update, i.e. how tightly the model tracks each
         * epoch's solved clock.  Default 0.05 m (the per-epoch solved-clock precision on a dense network).
         */
        double networkClockModelMeasStdM() const { return network_clock_model_meas_std_m_; }

        /**
         * @brief Max unseen gap (s) a satellite's carried two-state clock model survives
         *        before its state is treated as stale (@c network.clock_model_max_gap_s, R11).
         *
         * Governs the clock-model carry only, distinct from @ref networkPhaseLevelingMaxGapS,
         * which drops carried ambiguities / ZWD.  Past this gap the predicted clock prior is
         * dropped (reseed from data + the a-priori anchor), the drift finite-difference is reset
         * to 0, and the emitted C1 drift falls back to ZOH (0) rather than shipping an hours-old
         * rate for one epoch after a vanish-return.  Defaults to @ref networkPhaseLevelingMaxGapS
         * so existing clock-model configs are byte-identical; an independent value gives the
         * (slower-varying) clock a longer carry horizon than the ambiguities.
         */
        double networkClockModelMaxGapS() const { return network_clock_model_max_gap_s_; }

        /// @brief Emit a per-satellite URA quality channel from the network product
        ///        (@c network.emit_ura, B1).  URA = EWMA(σ̂0) × formal clock σ (from the
        ///        shared covariance).  Default off ⇒ the product is byte-identical.
        bool networkEmitUra() const { return network_emit_ura_; }
        /// @brief Producer: stamp each emitted epoch with a wall-clock time of generation
        ///        (@c network.emit_generation_time, RT0-4), serialized as an optional @c G
        ///        record, so a consumer can measure product latency (received − generated).
        ///        Default off ⇒ no @c G record ⇒ the product is byte-identical.  The stamp
        ///        is wall-clock and therefore non-deterministic, so a byte-reproducible
        ///        (regression) run must leave this off.
        bool networkEmitGenerationTime() const { return network_emit_generation_time_; }
        /// @brief Producer: per-constellation clock-runaway quarantine (@c network.clock_runaway_check,
        ///        RT0-5).  When on, a constellation whose median |clock − global median| exceeds
        ///        @ref networkClockRunawayMagM (or whose median |drift| exceeds
        ///        @ref networkClockRunawayRateMps) is dropped from this epoch's emission and flagged,
        ///        so a runaway (e.g. a BeiDou clock divergence) reduces scope (one constellation)
        ///        instead of contaminating the whole product.  Default off ⇒ every
        ///        constellation emitted ⇒ byte-identical.
        bool networkClockRunawayCheck() const { return network_clock_runaway_check_; }
        /// @brief Per-constellation median clock-magnitude ceiling in meters (RT0-5).  Inert unless
        ///        @ref networkClockRunawayCheck.  Default 1000 m: far above a healthy few-metre
        ///        spread, below the runaway hundreds-of-km regime.
        double networkClockRunawayMagM() const { return network_clock_runaway_mag_m_; }
        /// @brief Per-constellation median clock-rate ceiling in m/s (RT0-5).  Inert unless
        ///        @ref networkClockRunawayCheck and the two-state clock model emits a drift
        ///        (@c network.clock_model).  Default 10 m/s: far above a healthy sub-0.1 m/s drift.
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
        /// @brief Producer: emit a per-(sat,signal) 1σ for the satellite phase-bias
        ///        corrections (@c network.emit_phase_bias_sigma, B1b).  σ = EWMA(σ̂0) ×
        ///        formal phase-bias σ from the shared covariance, gauge-exact (datum-
        ///        pinned columns treated as exact, σ=0).  Carried
        ///        in @c SsrPhaseBiasEntry.sigma_m in the in-process store and serialized
        ///        as the optional @c sigma= token of the P record.  Default off ⇒ σ stays
        ///        0 ⇒ the product/store is byte-identical.  Tienstra-§6.6 phase-bias
        ///        analogue of the clock URA: without it the consumer applies phase-bias
        ///        corrections as if exact, so this transmits the diagonal of Qŷ_B1 for the
        ///        phase-bias block (the named defect-2 driver, 6–69 cm differential wander).
        bool networkEmitPhaseBiasSigma() const { return network_emit_phase_bias_sigma_; }
        /// @brief Consumer: inflate the measurement R of a phase row by the network
        ///        phase-bias correction σ² (@c network.consume_phase_bias_sigma, B1b), so a
        ///        wandering phase-bias correction is down-weighted instead of trusted as
        ///        exact.  Per-(sat,signal) via @c getPhaseBias; IF rows carry no per-signal
        ///        bias and are untouched.  Default off ⇒ R byte-identical.
        bool networkConsumePhaseBiasSigma() const { return network_consume_phase_bias_sigma_; }

        /// @brief Producer emits the per-key combined-correction σ
        ///        (@c network.emit_combined_sigma; M4, contract v2).  When on, the driver computes the
        ///        combined phase σ (clock + phase bias) into @c NetworkPhaseBias.sigc_m and
        ///        the combined code σ + code/phase correlation (clock + code bias) into
        ///        @c NetworkSatSolution.sigcc_m / @c rho from the shared posterior
        ///        covariance's cross-terms, serialized as the @c sigc= / @c sigcc= /
        ///        @c rho= wire tokens.  Off ⇒ every value 0 ⇒ no token ⇒ product/store
        ///        byte-identical.  (Zhang 2021 / Psychas 2024b: the combined σ is mm-level
        ///        where the individual clock/bias σ are cm-level during rise-reconvergence.)
        bool networkEmitCombinedSigma() const { return network_emit_combined_sigma_; }
        /// @brief Age-scaling coefficient the producer advertises on the S-line
        ///        (@c network.combined_sigma_agek; M4, contract v2).  A consumer inflates the combined
        ///        variance as σc²·(1 + agek·Δt_age²) (correction content age Δt_age in s),
        ///        replacing the ad-hoc URA staleness ramp for network-corrected rows.
        ///        Default 0 ⇒ no @c agek= token ⇒ no age inflation (byte-identical).
        double networkCombinedSigmaAgek() const { return network_combined_sigma_agek_; }
        /// @brief Consumer folds the combined-correction σ (age-inflated) into a
        ///        network-corrected row's measurement variance instead of the URA +
        ///        phase-bias-σ RSS (@c closed_loop.consume_combined_sigma; M4, contract v2).  Convenience
        ///        accessor onto the closed-loop sub-config so measurement-side callers match
        ///        the flat B1b getter style.  Default off ⇒ v1 weighting (byte-identical).
        bool networkConsumeCombinedSigma() const {
            return getNetworkConfig().closed_loop.consume_combined_sigma;
        }

        /// @brief Gate consumer AR eligibility on the product's @c signal_integer flag
        ///        (@c network.require_signal_integer_for_ar; T3a).  Off = coverage-only gate
        ///        (byte-identical).
        bool networkRequireSignalIntegerForAr() const { return network_require_signal_integer_for_ar_; }
        /// @brief Producer: retro-bump a flagged satellite's discontinuity counter on the
        ///        next emit after a post-emit QC flag (@c network.ura_retro_bump, B1): a
        ///        per-satellite reset (P1-3), not constellation-wide.  Default off.
        ///        Arming it auto-enables @c network.emit_clock_disc (R4: the clock counter
        ///        only exists on that channel), which also scopes handover datum bumps per
        ///        constellation; it needs at least one flag-producing QC tier armed
        ///        (@c network.raim / @c raim_within_epoch / @c clock_monitor /
        ///        @c clock_model) or it can never fire (loader warns).
        bool networkUraRetroBump() const { return network_ura_retro_bump_; }
        /// Mark an IF-TD reference-path slip as a per-satellite discontinuity (R
        /// record) so the consumer resets that satellite's ambiguity (Edge-1 piece 2).
        /// Targets the subset the emitter's value-jump test cannot see (SPEC 25.3:
        /// excursion onsets are refsat_reset-enriched 5.0x with PB value steps = 0).
        /// Default off; product-affecting when armed.
        bool networkMarkRefsatResetDisc() const { return network_mark_refsat_reset_disc_; }
        /// @brief Producer: URA multiplier on a flagged satellite for the phase-leveling
        ///        warmup horizon after a refloat (@c network.ura_refloat_inflate, B1).  1 = off.
        double networkUraRefloatInflate() const { return network_ura_refloat_inflate_; }

        /**
         * @brief Testing fast-forward: hours of data to skip before the network begins
         *        estimating (@c network.start_offset_hours).
         *
         * Skips every synced epoch until this many hours have elapsed from the first one, then
         * begins a cold-start solve at the offset (nothing is stashed while skipping).  Lets a
         * run reach a late event (e.g. a single-satellite lock-in ~4.3 h into the data) in a
         * handful of warm-up epochs instead of hours of processing.  Because the carry restarts
         * cold, this reproduces an externally driven event (eclipse / product anomaly after a
         * fresh warm-up) rather than the exact accumulated history.  Default 0 ⇒ process from the start.
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
         * chain.  Defaults to false, which keeps the legacy path
         * byte-identical.  Mode-agnostic.
         *
         * @return True if the CIO frame transform is enabled.
         */
        bool enableCioFrame() const { return enable_cio_frame_; }

        /**
         * @brief Correct the Sun/Moon ECI frame double-count before the ECI→ECEF rotation.
         *
         * `Planetary::sunMoonPosEci` evaluates its Sun/Moon series with tropical
         * (of-date) mean-longitude rates (36000.770 °/cy for the Sun, 481267.883
         * °/cy for the Moon) and rotates ecliptic→equatorial with the of-date mean
         * obliquity, so its output is already in the mean equinox and equator of
         * date (MOD).  Both ECI→ECEF chains then apply precession a second time:
         * the legacy `eci2ecef` via its IAU-1976 `P`, and `celestialToTerrestrial`
         * via the SOFA bias-precession-nutation matrix.  The result is a spurious
         * rotation about the ecliptic pole of 50.3″/yr since J2000 (20.41′,
         * 0.34019°, at 2024-05-08) in the delivered ECEF Sun and Moon directions.
         *
         * When true, the of-date vectors are de-precessed to J2000 (Pᵀ) before the
         * ECI→ECEF rotation, cancelling the double-count.  Nutation and GAST are
         * not double-counted (the series carries no nutation term and uses the mean
         * obliquity), so they are left untouched.
         *
         * Default false: the defect is inherited from RTKLIB and is present in every
         * existing product, sim fixture, and stored fingerprint, so flipping it is a
         * migration, not an edit.  See @c estimation.enable_sun_frame_correction.
         *
         * @return True when the of-date→J2000 de-precession is applied.
         */
        bool enableSunFrameCorrection() const { return enable_sun_frame_correction_; }

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
         * @c 0.0 disables the floor (pure coherence detection; useful
         * when validating the detector against data with known
         * sub-cycle slips, or in static contexts where sub-cm coherent
         * offsets are real disturbances worth handling).
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
         * instantaneous residual against cycle-slip semantics.  This
         * gate tracks the historical tendency of a particular
         * (satellite, signal) row to carry a non-zero residual mean
         * across many epochs (multipath signatures, PCV
         * miscalibration, inter-channel hardware bias, or any
         * persistent unmodeled effect).  Sourced from @c ResidualHistoryTracker's EWMA mean.
         *
         * When @c |mean| · sign > threshold · λ for a row whose
         * sample count exceeds @c warmup_n, the row's diagonal R
         * entry is inflated by @c mean² so the KF stops trusting the
         * channel as much as its formal stochastic noise suggests.
         * Inflation is automatic and recovers as the EWMA mean
         * decays (e.g., once the satellite climbs above a multipath
         * geometry).
         *
         * Default 0.10 (λ/10 ≈ 2 cm on L1): small enough to catch
         * persistent biases that the pivot/outlier paths ignore,
         * large enough that random per-epoch noise does not trip it.
         * 0 disables it.
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
         *        in-update, with no history needed.
         *
         * Complements the EWMA-based persistent-bias inflation: the
         * EWMA mechanism only fires once a row has accumulated enough
         * samples (~20 epochs at α=0.05), too slow to catch a satellite
         * whose phase residual is already biased in its first epochs of
         * tracking.  When a phase row's standardized residual exceeds
         * this threshold and its residual is sub-cycle (i.e. below
         * @c pivot_slip_min_cycles · λ; anything larger is treated as a
         * possible cycle slip and left to the post-fit outlier handler), the
         * row's diagonal @c R is inflated so the row stays in the
         * update with reduced weight rather than pulling the KF state
         * toward a persistent sub-cycle bias.  Two schemes are
         * available, selected by @c enableHuberInnovation(): a post-fit
         * IRLS that converges the standardized residual to the
         * threshold over several rebuild passes (default), or an
         * innovation-based closed-form one-shot.
         *
         * Default value: @c 6.0, the same threshold as the outlier
         * reset.  The two mechanisms are kept apart by the sub-cycle
         * gate, not by this threshold: above it, a sub-cycle phase row
         * is Huber-downweighted (too small to be a slip) while a
         * non-sub-cycle row is reset by the outlier handler.  0
         * disables it.
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
         * @c |innov|/sqrt(H·P·H' + R), which is monotone in @c R, so
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
         * even if the threshold is positive.  Useful for comparison
         * studies that deactivate the entire outlier-downweighting
         * branch without setting the threshold to a sentinel value.
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
         * small the residual is; i.e., the λ-fraction floor is
         * bypassed.
         *
         * This is independent of @c getPivotSlipMinCycles(), which
         * sets the floor's value; the bool gates whether the floor
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
         * Turning it off suits runs whose SSR code-bias corrections are
         * complete, with no residual receiver IFB to estimate; that reduces
         * the state dimension and improves observability of the ionosphere
         * state.
         *
         * @return True if IFB estimation is enabled.
         */
        bool estimateIfb() const { return estimate_ifb_; }

        /**
         * @brief Std (m) the receiver code-bias state is re-inflated to on a
         *        consumed satellite code-bias datum discontinuity.
         *
         * Sized to the gauge re-reference step (cm–dm), deliberately not the
         * full @c initial_std.code_bias (which would make the near-frozen
         * IFB/DCB a free parameter on every rise/set).  Used by the PPP-RTK
         * user consumer when @c getCodeBiasDiscontinuity reports an increment.
         *
         * @return Re-inflation standard deviation in meters.
         */
        double codeBiasReinflateStd() const { return code_bias_reinflate_std_; }

        /**
         * @brief Std (m) the inter-system time-offset (ISB) state is re-inflated
         *        to on a consumed satellite clock datum discontinuity
         *        (@c estimation.clock_disc_isb_reinflate_std).
         *
         * A network clock-datum re-reference steps one constellation's satellite
         * clocks by a common constant.  The receiver clock absorbs it for the
         * reference constellation, but for a non-reference constellation the step
         * is differential and lands on the near-frozen ISB state (q ≈ 1e-12),
         * which then lags for many epochs (the GPS-vs-Galileo seam seen in a
         * closed-loop wire audit).  When > 0 the consumer's clock-disc handler
         * re-inflates the stepped constellation's ISB
         * (@c StateManager::reinflateSystemTimeOffsetVariance) so the next update
         * re-absorbs it in one epoch.  0 (default) keeps the handler note-only
         * (byte-identical).  Fires only on network products with the clock-disc
         * channel armed; RTCM-decoded clocks carry disc_counter=0.
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
         * False allows all available frequency bands (the legacy
         * behaviour); extra bands then require IFB estimation.
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
         *        signal) phase-bias states.
         *
         * PPP_RTK_Network only.  When true, the network estimates the estimable
         * satellite phase bias (Odijk Eq. 4; it carries the network pivot-
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
         * @brief Whether the satellite code-bias state is promoted to first-class
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
         * When a sat-bias state is pruned (its satellite/signal is not
         * observed in the current epoch), its value and diagonal
         * variance are stashed in an in-memory cache.  If the same
         * (sat, signal) reacquires within this many seconds, the
         * cached state is restored verbatim with the variance aged
         * forward by @c process_noise.sat_bias_q × gap_s: a "warm
         * restart" that preserves the converged bias estimate across
         * brief visibility losses (low-elevation glitches, geometry
         * holes, masking).
         *
         * Beyond this window but within @c getSatBiasCacheMaxAgeS()
         * the cached value is used as the seed but the variance is
         * reset to @c initial_std.sat_bias²: a "cold restart from
         * cached value", useful when the gap is long enough that the
         * stashed variance is no longer trustworthy but the value
         * itself is still a better starting point than the WLS
         * single-epoch seed.
         *
         * Default 300 s (5 minutes).  A value equal to
         * @c getSatBiasCacheMaxAgeS() skips the cold-restart band
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
         * restored; the next acquisition uses the WLS-derived
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
         * 1.0 disables it (no extra downweighting).
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
         * (meters, ITRF/IGS14) specified in the config file, used to fix
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
         * involved; this is a deterministic shift of the fixed a-priori.
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
         * @return True if IMU is enabled and coupling_mode == TightCoupled.
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
         * @return @c DataSource::Internal (default; globs
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
         * from the live stream rather than from loaded precise products, i.e.
         * an IGS-SSR corrections stream is enabled (broadcast ephemeris + SSR
         * orbit/clock corrections) and/or the primary input is a live socket
         * (@c "ntrip" / @c "tcp", as opposed to @c "file" replay).  In that case
         * the post-fit SP3/CLK files are not inputs: loading them only risks a
         * stale fixture shadowing today's live PRNs (the coverage gate in
         * @c Geometry::getSatellitePV exists to guard against that), and in
         * production mode would trigger pointless CDDIS downloads.
         *
         * Real-time file replay with corrections disabled returns false, so a
         * historical run can still benefit from precise products (SP3-first
         * beats broadcast+SSR for a matching date).  Post-processing always
         * returns false; precise products are the authoritative source there.
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
         * @brief Whether a live source supplants precise SP3 orbits specifically.
         *
         * Splits the orbit half out of @ref realtimeUsesLiveOrbitClockSource
         * so a config can pin precise (e.g. ultra-rapid) orbits while satellite
         * clocks still come from the live/network stream ("ULT orbits + network
         * clocks").  With @c realtime.use_precise_orbits false (default) this
         * returns exactly @ref realtimeUsesLiveOrbitClockSource, so orbit and clock
         * gates move together and every product-resolution site is byte-identical.
         * When true (real-time only) it returns false (precise orbits are loaded)
         * while the clock gate (@ref realtimeUsesLiveOrbitClockSource) stays live,
         * so only the precise CLK is skipped.
         *
         * @return True when precise SP3 orbits should be skipped in favour of the
         *         live orbit path.
         */
        bool realtimeUsesLiveOrbitSource() const {
            if (app_mode_ != AppMode::RealTime) return false;
            if (realtime_config_.use_precise_orbits) return false;  // pin precise orbits (RTB-4)
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
         * tends to lock onto wrong integers and cascade: once a wrong integer
         * is constrained into the state, subsequent LAMBDA searches see a
         * degenerate covariance direction and produce inflated ratios that
         * look confident but are invalid.  10 epochs at 30 s rate ≈ 5 minutes
         * is the minimum reasonable gate; 30+ minutes is conservative.  Skipped
         * epochs return @c success=false from resolve().
         */
        int getArMinEpochs() const { return ar_min_epochs_; }

        /**
         * @brief Maximum 3D formal position σ for AR to fire (m).
         *
         * Convergence guard based on filter confidence rather than wall time.
         * AR is skipped when @c sqrt(P(0,0) + P(1,1) + P(2,2)) exceeds this.
         * Default 5 cm: once the filter is this confident in position, the
         * float ambiguity covariance is also typically converged enough for
         * a meaningful integer search.
         */
        double getArMaxPositionSigma() const { return ar_max_position_sigma_m_; }

        /**
         * @brief Upper bound on the LAMBDA ratio test (reject above this).
         *
         * Healthy AR ratios sit in the 3–1000 range.  Ratios above ~1e4
         * indicate the float covariance has a near-zero-variance direction
         * (degenerate from a previous wrong-integer hold or a numerical
         * problem), making the second-best integer's chi² distance
         * effectively infinite.  Such fixes look highly confident but are
         * mathematically suspect, so they are rejected and the solution
         * stays float.
         */
        double getArRatioUpperBound() const { return ar_ratio_upper_bound_; }

        /**
         * @brief WL-only AR mode toggle.
         *
         * When true, AR applies MW-resolved DD WL integers as Kalman
         * pseudo-observations on the SD ambiguity state and skips the
         * LAMBDA NL search entirely.  It serves as a conservative
         * verification stage before the full WL+NL LAMBDA pipeline:
         * WL via MW is robust even when the float NL covariance has not
         * converged enough for safe integer rounding.
         */
        bool getArWlOnly() const { return ar_wl_only_; }

        /**
         * @brief Partial ambiguity resolution toggle.
         *
         * When true, a LAMBDA fix that fails its full-set acceptance gates
         * (ratio / bootstrap success rate / per-pair fractional cap) is retried
         * on progressively smaller subsets (dropping the least-fixable
         * ambiguity each round and re-validating the full battery), so a few
         * noisy satellites do not force the whole epoch back to float.
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
        /// @brief Apply the bootstrap FFR gate to the WL-only / MW-WL constraint paths.
        /// Those paths assert MW integers without any LAMBDA search, so their `ratio` is
        /// the `+inf` "no ratio test ran" sentinel and the only confidence number
        /// available is the bootstrap success rate.  Default on.
        bool   getArWlOnlyFfr() const { return ar_wl_only_ffr_; }
        /// Minimum MW arc sample count admissible under @ref getArWlOnlyFfr.
        int    getArWlOnlyMinArcCount() const { return ar_wl_only_min_arc_count_; }
        /// Max innovation (cycles) a WL-only constraint may assert against the float.
        double getArWlOnlyMaxInnovCyc() const { return ar_wl_only_max_innov_cyc_; }

        /**
         * @brief Maximum worst-case fractional residual (cycles) accepted
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
         * Default 0.40 cyc: accepts arcs whose 99.7% confidence interval
         * for the rounded integer leaves at least 10% of a cycle of margin
         * before the half-cycle boundary.  This gates both locked and
         * BIA-corrected-but-converging arcs uniformly: arcs whose σ_mean
         * has shrunk enough are accepted; freshly BIA-corrected arcs with
         * wide σ_mean wait until they have averaged enough samples.
         *
         * A tighter value (e.g. 0.30) gives stricter AR; a looser one (e.g.
         * 0.45) helps when AR is starved.  Values < 0.20 admit only very
         * mature, clean locks (which can take many epochs of averaging).
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
         * problem geometry), this threshold is self-tuning: it
         * becomes harder to pass when the search is high-dimensional
         * or poorly conditioned.
         *
         * Default 0.999 → at most 0.1% probability of accepting a
         * wrong-integer fix.  0.9999 suits safety-critical
         * applications; 0.99 suits cases where integrity is less
         * critical and acceptance latency matters.
         */
        double getArMinSuccessRate() const { return ar_min_success_rate_; }
        /// @brief Per-candidate AR arc-maturity gate (@c estimation.ambiguity_resolution.min_arc_info), in zenith-epoch-equivalents; 0 = off.
        double getArMinArcInfo() const { return ar_min_arc_info_; }

        /**
         * @brief Maximum per-NL-pair |float − fixed| residual (cycles)
         *        for AR acceptance.
         *
         * Even when the joint LAMBDA ratio test passes, individual
         * pairs whose float landed close to the half-cycle boundary
         * are likely contributing the wrong integer to the joint set
         * (the marginal fixes that destabilize AR on the next epoch).
         * The entire fix is rejected when any pair's |frac| exceeds this
         * cap.
         *
         * Default 0.15 cyc.  0.10 suits runs that show wrong-fix
         * symptoms (ratio collapse on the next epoch); 0.20 suits runs
         * where AR rejects too many otherwise-good fixes.
         */
        double getArNlMaxPerPairFrac() const { return ar_nl_max_per_pair_frac_; }

        /**
         * @brief Use the exact ionosphere-free row for the WL-constrained N_1
         *        search (@c estimation.ambiguity_resolution.nl_iono_free_row).
         *
         * The legacy search functional is the narrowlane row
         * @f$(f_1,f_2)/(f_1{+}f_2)@f$, which picks up 0.959 cycles per TECU of
         * slant-iono error along the ionospheric direction
         * @f$\alpha=(1,(f_1/f_2)^2)@f$.  Against the default 0.15-cycle
         * @ref getArNlMaxPerPairFrac cap that is an implicit demand on the
         * ionosphere of 0.156 TECU, roughly 20× better than any available
         * ionospheric representation delivers.
         *
         * With this flag the same integer is observed through
         * @f$(f_1^2,-f_2^2)/(f_1^2-f_2^2)@f$, whose inner product with
         * @f$\alpha@f$ is identically zero: the N_1 target becomes exactly
         * iono-free.
         *
         * @warning The iono-free row's norm and its widelane
         * sensitivity both scale by @f$(f_1{+}f_2)/(f_1{-}f_2)@f$ = 8.06 for
         * GPS L1/L2, so a 1-cycle widelane error moves the N_1 target 3.53
         * cycles rather than 0.438.  Arm this only alongside a hardened
         * widelane; on its own it converts an ionospheric bias into
         * deterministic wrong integers.  Default false (byte-identical).
         */
        bool getArNlIonoFreeRow() const { return ar_nl_iono_free_row_; }

        /**
         * @brief Get the list of constellations eligible for AR.
         *
         * Empty list = all active systems are eligible (default).
         * Non-empty = only listed constellations enter the AR candidate
         * set.  Excluded constellations stay in the float solution
         * (their phase observations contribute to position/troposphere/
         * iono estimation and their ambiguities are still float-tracked)
         * but are skipped by @c buildDdPairs and
         * @c applyMwWlConstraintsNlWl_.
         *
         * Intended for a constellation with residual model bias
         * (uncalibrated antenna PCV, mismatched phase wind-up
         * convention, missing per-attribute BIA entries, etc.) that
         * drags down the joint LAMBDA ratio test even though its float
         * contribution is sound; a common production strategy when one
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

        // -------------------------------------------------------------
        //  network subsection helpers   (GnssConfigLoadNetwork.cpp)
        //
        //  The `network` section carries more knobs than the other eleven
        //  top-level sections combined and every new feature adds to it, so it
        //  gets a second level of the same split: loadNetworkSection_ is a
        //  thin orchestrator over one helper per JSON subsection.  That
        //  keeps concurrent knob additions out of one merge hunk.  A new
        //  knob goes into the helper that owns its subsection, not into
        //  the driver.
        //
        //  The call order is load-bearing and is fixed in loadNetworkSection_.
        //  Every cross-knob forcing in the section is expressed as "a later
        //  helper reads what an earlier one resolved": the RTF-3
        //  inert-arming refusal, the D5 corroboration gate auto-arming the
        //  slip-QC instrumentation that feeds it, the RAIM tier
        //  auto-couplings, the RTB-8 service arming and its P3
        //  re-validation, and the network.process_noise table (resolved
        //  first, settled last).  A new subsection is added as a new helper
        //  at the point where its prerequisites are final; the existing
        //  calls are never reordered.
        //
        //  Helpers that can reject the config return bool (the driver
        //  propagates false straight out of loadFromJson); the rest return
        //  void.  A helper that takes no `json` is a pure cross-knob
        //  validator over already-parsed state.  Each is documented at its
        //  definition.
        // -------------------------------------------------------------
        bool loadNetworkProcessNoise_(const nlohmann::json& json,
                                      const std::shared_ptr<spdlog::logger>& log);
        bool loadNetworkRosterAndDatum_(const nlohmann::json& json,
                                        const nlohmann::json& nw,
                                        const std::shared_ptr<spdlog::logger>& log);
        void loadNetworkProducerAr_(const nlohmann::json& json,
                                    const std::shared_ptr<spdlog::logger>& log);
        void loadNetworkLaneLayers_(const nlohmann::json& json,
                                    const std::shared_ptr<spdlog::logger>& log);
        void loadNetworkOutputAndParallel_(const nlohmann::json& json,
                                           const std::shared_ptr<spdlog::logger>& log);
        bool loadNetworkStreaming_(const nlohmann::json& json,
                                   const std::shared_ptr<spdlog::logger>& log);
        void loadNetworkObservabilityAndSnapshot_(const nlohmann::json& json,
                                                  const std::shared_ptr<spdlog::logger>& log);
        bool loadNetworkSharedStateForcings_(const nlohmann::json& json,
                                             const std::shared_ptr<spdlog::logger>& log);
        bool loadNetworkAprioriAndGlonass_(const nlohmann::json& json,
                                           const std::shared_ptr<spdlog::logger>& log);
        void loadNetworkCarries_(const nlohmann::json& json,
                                 const std::shared_ptr<spdlog::logger>& log);
        void loadNetworkSlipDetectors_(const nlohmann::json& json,
                                       const std::shared_ptr<spdlog::logger>& log);
        void loadNetworkQcTiers_(const nlohmann::json& json,
                                 const std::shared_ptr<spdlog::logger>& log);
        void loadNetworkClockModelAndChannels_(const nlohmann::json& json,
                                               const std::shared_ptr<spdlog::logger>& log);
        void loadNetworkDatumSignalling_(const nlohmann::json& json,
                                         const std::shared_ptr<spdlog::logger>& log);
        void loadNetworkReattachHygiene_(const nlohmann::json& json,
                                         const std::shared_ptr<spdlog::logger>& log);
        void loadNetworkSignalIntegrity_(const nlohmann::json& json,
                                         const std::shared_ptr<spdlog::logger>& log);
        bool loadNetworkEmissionFilters_(const nlohmann::json& json,
                                         const std::shared_ptr<spdlog::logger>& log);
        bool validateNetworkDatumCoherence_(const std::shared_ptr<spdlog::logger>& log);
        bool validateNetworkLaneLayers_(const nlohmann::json& json,
                                        const std::shared_ptr<spdlog::logger>& log);
        bool validateNetworkPhaseDatum_(const std::shared_ptr<spdlog::logger>& log);
        void loadNetworkContractHardening_(const nlohmann::json& json,
                                           const std::shared_ptr<spdlog::logger>& log);
        void applyNetworkServiceProfile_(const nlohmann::json& json,
                                         const std::shared_ptr<spdlog::logger>& log);
        bool validateNetworkCarryPosture_(const nlohmann::json& json,
                                          const std::shared_ptr<spdlog::logger>& log);
        /// The interlock warnings and the posture line of
        /// @c network.estimate_rx_extra_band_code_delay; prints nothing unless
        /// the knob is armed.  Called after @c validateNetworkCarryPosture_, so the carry
        /// and datum knobs it names are final.
        void logNetworkRxExtraBandDelay_(const std::shared_ptr<spdlog::logger>& log) const;
        void loadNetworkClosedLoop_(const nlohmann::json& nw,
                                    const std::shared_ptr<spdlog::logger>& log);
        /// Parse @c network.inject (scripted producer-side faults; event harness).
        /// Strict: a malformed event is a load failure, never a silent drop (a test
        /// fixture that silently does not exist would be graded as a null).
        bool loadNetworkInject_(const nlohmann::json& nw,
                                const std::shared_ptr<spdlog::logger>& log);
        /// The fixed-cycle fold's interlocks, on the resolved values of the knobs
        /// they read (several are parsed after @ref loadNetworkProducerAr_).  Throws
        /// std::runtime_error on a refused posture (loadFromJson then returns false).
        void validateNetworkFixedCycles_(const std::shared_ptr<spdlog::logger>& log) const;
        void logNetworkSummary_(const std::shared_ptr<spdlog::logger>& log) const;
        void finalizeNetworkProcessNoise_(const nlohmann::json& json,
                                          const std::shared_ptr<spdlog::logger>& log);

        // -------------------------------------------------------------
        //  network posture readout   (GnssConfigNetworkPosture.cpp)
        //
        //  New work ships default-off by convention and nothing moves it to
        //  default-on, so validated options would otherwise accumulate
        //  unarmed.  The loader logs everything it arms; this readout logs
        //  the inverse.  Regime-gated (a knob validated at 50 stations may
        //  be harmful at 305) and bundle-aware (a strict subset of a
        //  coupled set is its own failure mode).  Log-only.
        //
        //  It is its own TU because it changes for a different reason than
        //  the loader: landing a knob edits the loader, validating one adds
        //  a row here.
        //
        //  @see docs/CONFIG_promotion_ledger.md, scripts/config_posture_check.py
        // -------------------------------------------------------------
        /// Which of the two recommendation tiers a row belongs to: an ON-tier
        /// gap is logged as a warning (no known counter-scope in this regime), a
        /// SCOPED-tier gap only as info (there are 40+, and warning on all of them
        /// would bury the ON tier).
        enum class PostureTier { ON, SCOPED };
        /// One recommendation row.
        ///
        /// `fp` = the knob is part of the warm-resume fingerprint (the add() set of
        /// networkSnapshotFingerprint in SnapshotFingerprint.cpp).  This is the chain
        /// axis, and it is why a recommendation cannot be read as an instruction: on a
        /// resumed leg, arming a fingerprinted knob does not take effect mid-chain.  With
        /// state_snapshot_require on the leg hard-aborts; with it off the leg silently
        /// cold-starts, which is the worse failure and the one this readout exists to
        /// prevent.  Such a knob has to be armed in a fresh lineage (a new seed), never added
        /// to an existing chain.
        struct PostureItem { bool applies; bool on; PostureTier tier; bool fp;
                             const char* knob; const char* why; };
        /// One companion knob of a coupled bundle.
        struct PostureMember { const char* knob; bool on; };
        /// A set of knobs that is only correct when armed together.
        struct PostureBundle { bool engaged; const char* name; const char* rule;
                               std::vector<PostureMember> members; };
        /// The regime predicates every row filters on, computed once per run.
        struct PostureRegime {
            std::size_t n_sta;        ///< Roster size.
            std::size_t n_sys;        ///< Active-constellation count.
            bool        tri;          ///< Triple/All frequency mode.
            bool        warm;         ///< A state-snapshot path is set (warm-resume lineage).
            bool        rt;           ///< Real-time or service posture.
            bool        par;          ///< Per-station parallelism actually engages at this size.
            double      spacing_s;    ///< Config-determinable epoch spacing (0 = native RINEX rate).
            bool        gps_only;     ///< active_systems == {GPS}.
            bool        gps_gal_only; ///< active_systems ⊆ {GPS, Galileo}.
        };
        PostureRegime networkPostureRegime_() const;
        void appendNetworkPostureItemsOn_(const PostureRegime& r,
                                          std::vector<PostureItem>& out) const;
        void appendNetworkPostureItemsScoped_(const PostureRegime& r,
                                              std::vector<PostureItem>& out) const;
        std::vector<PostureBundle> networkPostureBundles_() const;
        void logNetworkPosture_(const std::shared_ptr<spdlog::logger>& log) const;

        void loadLoggingSection_(const nlohmann::json& json, LogManager& log_manager);
        void logConfigurationSummary_(LogManager& log_manager) const;

        ProcessingMode processing_mode_; ///< Processing mode (PPP or PPP-RTK).
        FrequencyMode   frequency_mode_;       ///< KF frequency band scope.
        ChannelMode     channel_mode_;         ///< KF tracking-mode channel multiplicity per band.
        MwFrequencyMode mw_frequency_mode_;///< MW detector frequency mode (Mirror inherits KF).
        MwChannelMode   mw_channel_mode_;    ///< MW detector channel mode (Mirror inherits KF).
        /// Per-band canonical tracking-attribute rule.  @c Count is the
        /// legacy, byte-identical default; @c Priority walks the priority list.
        CanonicalSignalRule canonical_signal_rule_{ CanonicalSignalRule::Count };
        /// Coverage floor for @c CanonicalSignalRule::Priority, clamped to [0, 1] at
        /// load.  Unread under @c Count.
        double canonical_signal_min_coverage_{ 0.5 };
        /// Stations the rule is scoped to, upper-cased at load.  Empty is
        /// the byte-identical default (the rule applies everywhere); a non-empty list
        /// puts the listed stations on @c Priority and every other station on @c Count.
        std::vector<std::string> canonical_signal_priority_stations_;
        /// Whether the canonical code follows the phase's tracking attribute under
        /// @c Priority.  True is the CSL-1 behaviour and the byte-identical
        /// default; false decides the code on its own priority list.  Unread under
        /// @c Count, which has no such clause.
        bool canonical_signal_code_follows_phase_{ true };
        bool enable_inhouse_bias_estimation_;       ///< In-house per-(sat, sig-pair) MW phase bias estimation; default off.
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
        double ssr_bias_max_age_s_{ 30.0 };               ///< Max |obs − SSR-code/phase-bias-record| age (s) before the bias overlay is rejected (A5); default matches ssr_max_age_s_.  Only enforced when enable_ssr_bias_max_age_.
        bool   enable_ssr_bias_max_age_{ false };         ///< Enforce the SSR code/phase-bias staleness gate (A5); off = the bias getters never gate on age (byte-identical).
        bool   enable_ssr_bias_max_age_explicit_{ false };///< True iff the config sets processing.enable_ssr_bias_max_age (either key form), so applyNetworkConsumerProfile can arm the gate under bias consumption while respecting an explicit false (RTA-9(a)).
        double ssr_bias_stale_hold_factor_{ 2.0 };        ///< Apply-gate drop limit = factor × ssr_bias_max_age_s_ (A5): the last bias is held over sub-limit staleness gaps, so no artificial model step or burst of resets occurs; 1.0 = drop exactly at max age.  Only meaningful when enable_ssr_bias_max_age_.
        bool   ssr_phase_bias_disc_carry_{ false };       ///< Carry the float ambiguity across a declared phase-bias step (amb − Δ, value-only) instead of resetting it; off = the existing reset path (byte-identical).  Set directly (processing.ssr_phase_bias_disc_carry) or derived from network.closed_loop.pb_disc_carry.
        bool   ssr_level_step_inflate_{ false };      ///< Raise the variance along a stepped satellite's phase-blind ionosphere-ambiguity direction by its declared code-level step (C and B record lvl=). OFF = byte-identical.  Set directly (processing.ssr_level_step_inflate) or derived from network.closed_loop.level_step_inflate.
        double ssr_level_step_inflate_k_{ 1.0 };      ///< The inflation's standard deviation over the declared code step.
        bool   ssr_phase_bias_step_translate_{ false };   ///< Translate the float ambiguity by −Δ on every change in the applied phase bias, declared or not (covers undeclared steps and empty declarations in one rule, see EV6 and CST §7.1); off = the existing reset / declared-step carry path (byte-identical).  Set directly (processing.ssr_phase_bias_step_translate) or derived from network.closed_loop.pb_step_translate.  When on it replaces both reactions ⇒ a declared step is translated exactly once.
        double ssr_phase_bias_step_translate_eps_m_{ 0.005 }; ///< Dead band of the step translation (m): at or below it nothing is written and the filter absorbs the move as noise.  processing.ssr_phase_bias_step_translate_eps_m / network.closed_loop.pb_step_translate_eps_m.
        bool   ssr_pb_hold_presence_flip_{ false };        ///< A held phase bias's presence edge (it appears on, or leaves, a row already in use) takes the A5 reaction of an unheld key instead of none; off = the phase-bias-hold dispatch, byte-identical.  processing.ssr_pb_hold_presence_flip / network.closed_loop.pb_hold_presence_flip.
        bool   amb_level_check_{ false };                 ///< Re-validate each float ambiguity against its own windowed code level (MW wide-lane + iono-free, both from the consumer's own corrected observations) and refloat the ones the code rejects; off = nothing measured, nothing written (byte-identical).  processing.amb_level_check / network.closed_loop.amb_level_check.
        int    amb_level_check_window_ep_{ 20 };          ///< Samples per satellite before the level test may run (20 = 10 min at 30 s).  processing.amb_level_check_window_ep.
        double amb_level_check_k_{ 4.0 };                 ///< Level-test trip at |state − level| > k · σ_level.  processing.amb_level_check_k.
        std::string amb_level_check_mode_{ "act" };       ///< Level-test action: "act" (refloat both bands) or "observe" (count only).  processing.amb_level_check_mode.
        double ssr_clock_stale_hold_factor_{ 0.0 };       ///< Clock stale-hold rung (RTA-9(b)): a clock aged in (ssr_max_age, factor × ssr_max_age] is still applied with URA-staleness R inflation; 0 (default) = off (byte-identical two-state gate); (0,1] clamps to 1.
        bool   ssr_bias_band_fallback_{ false };          ///< Code-domain band-wise bias fallback (RTA-13, opt-in): an exact-attribute miss consumes the same-band sibling code bias; phase never falls back.  Default off = store lookups byte-identical.
        bool   enable_obs_ssr_sync_{ false };             ///< RT-only: hold obs ahead of the SSR coverage horizon (obs↔SSR sync) instead of processing against absent SSR.
        double obs_ssr_sync_max_hold_s_{ 30.0 };          ///< RT-only: max seconds to hold obs for SSR before degraded fallback (obs↔SSR sync liveness bound).
        std::string rinex_types_;                        ///< Enabled RINEX observation type letters (subset of "CLDS"); default code+phase.
        bool rinex_skip_zero_values_{false};             ///< RINEX 3 parser: skip written 0.0 values (the standard's missing-observation sentinel).  Default false = legacy ingest (see rinexSkipZeroValues()).
        ReceiverAntennaOrientation receiver_antenna_orientation_; ///< Antenna orientation parameters.
        bool enable_rx_antenna_orientation_{ false };  ///< Apply receiver antenna azimuth orientation to the RX PCV look-up (off → byte-identical).
        bool enable_rx_antenna_substitution_{ false }; ///< Allow rule-driven receiver antenna model substitution (deprecated-code table + radome→NONE) when no exact ATX match.
        ProcessNoise process_noise_;                          ///< Process noise parameters.
        InitialStd initial_std_;                              ///< Initial state uncertainties.
        DefaultMeasurementNoise default_meas_noise_;          ///< Default measurement noise (fallback).
        std::map<std::string, std::vector<std::string>> signal_priorities_; ///< Signal priorities per system.
        std::vector<std::string> active_systems_; ///< Active GNSS systems.
        /// Base configs merged in via `"extends"`, in application order; empty when the
        /// config declares no inheritance.
        std::vector<std::string> config_extends_chain_;
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
        bool enable_if_slip_detector_;             ///< Inert: no user-path code reads this toggle.  IfPhaseSlipDetector is built for the network path only (network.cycle_slip.if_phase_td; see include/slip/IfPhaseSlipDetector.hpp).  Parsed for forward compatibility; toggling it has no effect.
        bool accumulate_phase_windup_;                ///< Accumulated (unwrapped) phase wind-up for IAR readiness.
        double phase_windup_max_gap_s_{300.0};        ///< Wind-up accumulator continuity horizon (s); see @ref phaseWindUpMaxGapS.
        bool trace_windup_census_{false};             ///< Read-only wind-up step/gap census; see @ref traceWindUpCensus.
        bool enable_between_satellite_sd_;                ///< Between-satellite single-difference operator in measurement model.
        bool enable_glonass_bssd_;                        ///< Allow GLONASS satellites in BSSD (FDMA ICB → off by default).
        bool enable_glonass_ifb_;                         ///< Estimate per-FCN-slot GLONASS receiver code IFB (requires estimate_ifb + uncombined).
        bool enable_glonass_network_{false};              ///< Admit GLONASS to the PPP-RTK network producer (code-only: sat clock + per-slot rx IFB; no FDMA phase bias).
        bool estimate_network_sat_code_bias_{false};      ///< Estimate satellite code bias (OSB) in the PPP-RTK network producer (W6; forces sat-bias/ifb on, iono weighting on unless the W6 iono constraint is none, .BIA off).
        bool estimate_network_iono_sh_{false};            ///< Co-estimate the global SH-VTEC ionosphere in the network producer (forces W6; replaces external GIM as the iono constraint).
        bool network_rx_code_bias_datum_{false};        ///< Receiver-DCB datum (RXCB-DATUM): a loose zero-mean prior on every receiver DCB that realizes one sum-to-zero-over-receivers gauge per constellation (the IGS convention; replaces the satellite-side zero-mean where no satellite code-bias column exists).  Default off => byte-identical.
        double network_rx_code_bias_datum_sigma_m_{10.0};  ///< Loose physical-bound sigma (m) of the receiver-DCB datum (RXCB-DATUM); realized common-mode info = 1/sigma^2, station-count independent.
        bool network_cb_admission_{false};                ///< Run-latched W6 code-bias admission census + station-local quarantine (CB-ADM; replaces the per-epoch >=2-station gate as the remedy for BeiDou sparsity).  Default off => legacy path byte-identical.
        int  network_cb_admission_min_stations_{4};       ///< Min supporting-station count for an admitted code pair (CB-ADM); effective_min = max(2, min(this, n_census_stations)).
        int  network_cb_admission_census_epochs_{15};     ///< Census window length K (epochs from the loop start; CB-ADM); clamped to the available span.
        bool network_cold_clock_seed_{false};             ///< Pre-seed a cold per-station receiver clock from the median GPS code residual before the first ambiguity seed (removes the network cold-start clock leak into cold ambiguity seeds).  Default off => byte-identical.
        NetworkAprioriSource network_apriori_source_{NetworkAprioriSource::Sp3}; ///< Producer a-priori orbit/clock source (network.apriori_source; RTF-1); Sp3 (default) = precise SP3/CLK identity (byte-identical), BrdcRts = broadcast eph + external RTS orbits.  Read by the network producer setup.
        NetworkBrdcSelect network_brdc_select_{NetworkBrdcSelect::Ttom};  ///< Ephemeris selection rule of the native broadcast clock a priori (network.brdc_select); read only under apriori_source sp3_brdc_clock.
        double network_apriori_clock_anchor_sigma_m_{0.0};  ///< A-priori satellite-clock anchor pseudo-obs σ (m; RTF-1/D6); 0 (default) = auto (mode-resolved at the producer), explicit positive wins.  Read through resolveNetworkAprioriClockAnchorSigmaM().
        bool   network_phase_leveling_{false};            ///< Carry per-station carrier-phase ambiguities across epochs (phase-leveling, Increment 1); off = byte-identical absolute solve.
        double network_phase_leveling_q_amb_{0.0};        ///< Carried-ambiguity process noise (m²/s) inflating var over the inter-epoch gap; 0 (default) = pure zero-process-noise accumulation (random-constant model).
        int    network_phase_leveling_warmup_epochs_{6};  ///< Emitted-epoch count before a carried ambiguity is trusted (cold-start convergence guard).
        double network_phase_leveling_max_gap_s_{120.0};  ///< Max unseen gap (s) a carried ambiguity survives before it is dropped (stale-carry / map-growth guard).
        double network_phase_leveling_var_floor_m2_{0.0}; ///< Floor (m²) on the carried prior variance (caps Λ; lock-in guard); 0 = off (no cap).
        bool   network_phase_leveling_local_block_{false}; ///< Carry each station's injectable ambiguity set as one jointly whitened local block (F3; harvested conditional cross covariances on Σ's off-diagonals, departed columns marginalized): exact for every local-local direction, including the station-common ones (SPEC_amb_iono_split_overcount).  Off = byte-identical.
        double network_shared_carry_var_floor_pb_m2_{0.0}; ///< Floor (m²) on the D2 sat-phase-bias carried prior variance (B4; caps Λ, which bounds the zero-q lock-in while preserving Ge-2008 FCB averaging); 0 = off.
        double network_shared_carry_var_floor_cb_m2_{0.0}; ///< Floor (m²) on the D2 sat-code-bias carried prior variance (B4; caps Λ); 0 = off.
        bool   network_shared_carry_full_cov_{false};      ///< Carry the full dense cross-covariance of the shared border across epochs (C1; information-form KF on the border that recovers the clock↔phase-bias off-diagonals the diagonal D2 stash discards): fold Λ=(P_prev+Q)⁻¹ as one dense block, marginalize departed/pinned/retired keys, degrade to the diagonal fold on a non-PD block.  Off = legacy per-key diagonal carry (byte-identical).
        bool   network_carry_conditional_coupling_{false}; ///< Carry each persistent local parameter (phase-leveling ambiguity, ZWD) as a conditional pseudo-observation row coupling it to the free shared border (coeff +1 on the local, regression M_r on the border, variance = pure conditional (N_rr⁻¹)_{rr}), not as an independent diagonal prior; this removes the shared-information double count so the sequential recursion equals the batch solve.  Requires shared_carry_full_cov (hard error otherwise).  Off = legacy diagonal addStationPrior carry (byte-identical).
        bool   network_carry_coupling_gauge_project_{false};   ///< Hold each (constellation,kind) coefficient sum of a truncated coupling row at its harvest value, so a departed/re-realized column stops pinning the current datum gauge at the old realization (without it the null eigenvalue rose from 1e-11 to 9.43e-3 in one epoch, causing spurious over-specified withholds); requires carry_conditional_coupling.  Off = byte-identical.
        bool   network_carry_coupling_reinit_sources_{false};  ///< Key the coupling reinit history (kind,sat,signal) with an empty-signal sat-wide wildcard, feed it from the phase-anchor / eq34 / datum-slip re-reference sets, and honour it independently of carry_coupling_time_update, so a re-realized column takes the departed route instead of folding a retired-frame ŝ_prev at full weight; requires carry_conditional_coupling.  Off = byte-identical.
        bool   network_carry_coupling_time_update_{false};  ///< Apply the border time update to each coupling row at injection: shrink every present coupled column by κ_j = P_prev/(P_prev + q_j·Δt) (coefficient + RHS term) and inflate the row variance by M²·κ_j·q_j·Δt, with the per-kind q ladder equal to the dense fold's (clock-monitor-reinit sats: q=∞ ⇒ full marginalization).  Without it the rows re-assert the harvest-epoch border at full strength every epoch (the carried-DD random-walk / flat-σ defect).  Requires carry_conditional_coupling (hard error otherwise).  Off = raw harvested rows (byte-identical).
        bool   network_carry_coupling_shared_history_{false};    ///< The clones' shared conditional carried as history columns.  Requires carry_coupling_clock_clone.  Off = the clock-clone per-station harvest (byte-identical).
        int    network_carry_coupling_shared_history_rank_{1};   ///< History columns per cloned clock, [1, 8].
        bool   network_carry_coupling_bias_clone_{false};        ///< The satellite phase and code biases on one-epoch clones with walk factors.  Requires carry_coupling_clock_clone.  Off = the exact per-row update for the biases (byte-identical).
        bool   network_carry_coupling_departed_clone_{false};    ///< Every key the exact update re-attributes on a one-epoch clone with the stashed prior and no walk.  Requires carry_coupling_clock_clone.  Off = the exact update's re-attribution (byte-identical).
        bool   network_carry_coupling_clock_clone_{false};   ///< Carry the previous epoch's satellite clocks as one-epoch clone columns in the shared border (the rows keep their clock coefficients on the clone, the walk enters once in the border prior, the harvest marginalizes the clones).  Requires carry_coupling_exact_update and shared_carry_full_cov; refuses clock_model and clock_anchor_gauge_free.  Off = the exact per-row form (byte-identical).
        bool   network_carry_coupling_exact_cross_{false};   ///< Add the exact time update's within-station row covariance M_i'BM_k to the whitened local block.  Requires carry_coupling_exact_update.  Off = the per-row form (byte-identical).
        bool   network_clock_pivot_frame_q_{false};          ///< The carried satellite clocks' process noise in the pivot frame (each folding non-pivot clock takes the pivot's walk r_p dt_j; pairs share r_p min(dt_j, dt_k) in the dense fold and the exact update's Q_S).  Requires shared_carry_full_cov; refuses the anchor-free clock datums and clock_model.  Off = byte-identical.
        bool   network_carry_coupling_exact_update_{false};  ///< Replace the scalar kappa by the exact conditional time update of the coupling rows: M_new = (P_SS+Q_S)^-1 P_S,K M_K over the stashed dense border block with the fold's own inflation, the bracket variance added per row (the per-row form: no time-update covariance between rows), departed/decoupled columns re-attributed through P, the clock-family deficit onto the pinned pivot.  Requires carry_coupling_time_update and shared_carry_full_cov.  Off = scalar-κ rows (byte-identical).
        bool   network_carry_zwd_{false};                 ///< Carry per-station tropospheric ZWD across epochs as a diagonal random-walk prior (B9; via addStationPrior, alongside the ambiguity carry); off = per-epoch white-noise ZWD (byte-identical).  FALSIFIED for continuous runs of the standard posture (consumer 0.204 → 0.723 m rms: an information double count against the station filter's own persistence); its seam value is delivered by network.zwd_seam_reseed instead.
        bool   network_carry_zwd_in_block_{false};         ///< Carry the station ZWD column inside the F3 jointly whitened local block (B9-in-block; harvested (N_rr⁻¹) cross terms against every carried ambiguity column) instead of as a separate diagonal coupling row, which makes the ZWD carry exact along the station-common directions the diagonal row over-counts (SPEC_amb_iono_split_overcount §7 item 17).  Requires network.carry_zwd and network.phase_leveling_local_block (the loader warns and disables otherwise).  Off = byte-identical.
        bool   network_carry_iono_in_block_{false};        ///< Carry the per-LOS slant-iono columns inside the F3 jointly whitened local block (I-in-block; harvested (N_rr⁻¹) cross terms against every other carried column of the station): the I half of the ZWD-in-block carry (SPEC_amb_iono_split_overcount §7 item 17, §6.16.7d).  It replaces the retired diagonal network.carry_iono (QUEUE 8b, FALSIFIED in four arms): a diagonal row re-asserts the per-LOS iono direction the B band priors already assert.  Requires network.phase_leveling + network.phase_leveling_local_block; disabled under carry_invariant.  Off = byte-identical.
        double network_carry_iono_in_block_q_m2_{2.5e-3}; ///< Per-epoch random-walk process noise of the in-block iono carry, m² of slant delay at L1 (the per-LOS iono state's own unit).  Per epoch, not per second (zwd_carry_q is m²/s); the gap is converted with the run's observed inter-epoch spacing.  var_eff = var + q·gap_ep on the block diagonal only.
        double network_carry_iono_in_block_max_gap_s_{0.0}; ///< Staleness bound (s) of the in-block iono carry, past which a LOS's carry is dropped (counted).  0 = the sentinel meaning "use network.phase_leveling_max_gap_s"; the loader resolves it to that value at load, so the getter always reports the effective seconds.
        bool   network_carry_rx_clock_in_block_{false};    ///< Carry the station receiver clock column inside the F3 jointly whitened local block as a random walk about a carried drift (RX-CLK-in-block; the producer never writes its solution back, so x_lin(clock) is a run constant and the whole clock, offset + ~0.3 m/s drift + jitter, lives in x_r).  The rho-direction sibling of carry_zwd_in_block / carry_iono_in_block: the per-epoch clock column is the free sink those two push their residual error into.  Requires network.phase_leveling + network.phase_leveling_local_block; disabled under carry_invariant.  Off = byte-identical.
        double network_carry_rx_clock_q_m2_{0.09};         ///< Per-epoch random-walk process variance of the carried clock bias, m^2 (RX-CLK-in-block).  (Per epoch, not per second: zwd_carry_q is m^2/s; the gap is converted with the run's observed inter-epoch spacing.)  Default 0.09 = sigma 0.30 m per 30 s, the sim truth bed's measured detrended increment std.
        double network_carry_rx_clock_max_gap_s_{0.0};     ///< Staleness bound (s) past which a station's clock carry is dropped (counted; RX-CLK-in-block).  0 = the sentinel meaning "use network.phase_leveling_max_gap_s"; the loader resolves it at load, so the getter always reports effective seconds.
        double network_carry_rx_clock_rate_var0_{1.0};     ///< Initial variance of the carried drift state, m^2/s^2 (RX-CLK-in-block).  Deliberately large (sigma 1 m/s) so a station whose drift is not yet estimated injects a near-uninformative prior (sigma ~30 m at 30 s) instead of a stiff wrong one; collapses after one harvested increment.
        double network_carry_rx_clock_rate_q_{1e-10};      ///< Per-epoch random-walk process variance of the carried drift state, m^2/s^2 (RX-CLK-in-block).  Default 1e-10 (sigma 1e-5 m/s per epoch) tracks slow drift wander without chasing the bias jitter; 0 = strictly constant drift.
        double network_carry_rx_clock_jump_k_{10.0};       ///< Innovation gate (sigma) on the one-step clock prediction at harvest (RX-CLK-in-block).  A receiver clock step (1 ms = 3e5 m) has no other detector on the network path; exceeding this erases both the bias and the drift carry (counted, cold rebirth) so the jump never enters the block.
        bool   network_carry_rx_clock_isb_{true};          ///< Also carry the per-system inter-system bias (ISB) columns in the same block, as a pure random walk (RX-CLK-in-block; no drift state, since an ISB is a near-constant bias; the sim truth injects a fixed 8.000 m GPS->Galileo offset).  Counted separately; 0 ISB columns is not an alarm (a GPS-only or BSSD posture allocates none).
        double network_zwd_carry_q_{1e-9};                ///< ZWD carry random-walk process noise (m²/s; B9) inflating the carried ZWD variance over the inter-epoch gap (var_eff = var + q·Δt). Used by network.carry_zwd and by the network.zwd_seam_reseed post-resume window.
        bool   network_zwd_seam_reseed_{false};           ///< Seam-scoped ZWD re-seed: after a warm resume that restored Z rows, the B9 inject+stash runs for zwd_seam_reseed_epochs emitted epochs and then retires (the station filter has re-established its own memory).  Never runs on continuous epochs.  Not fingerprinted (forward-only resume response policy).  Off = byte-identical.
        double network_zwd_seam_reseed_q_{-1.0};          ///< The seam window's own RW process noise (m²/s; RS-10b, §21.21).  Negative (default -1) = the "unset" sentinel: the seam fold borrows network.zwd_carry_q (byte-identical).  Positive = used instead of zwd_carry_q, and only while the seam window is live (a continuous carry_zwd row always uses zwd_carry_q).  Not fingerprinted (forward-only resume response policy); that is the reason for the knob, because zwd_carry_q is fingerprinted and the response's stiffness could otherwise not be varied on a resume leg.
        int    network_zwd_seam_reseed_epochs_{480};      ///< Seam re-seed horizon (emitted epochs post-resume).  Default 480 = 4 h at 30 s: past the measured ~3.1 h cold re-convergence, below the ≥6–8 h onset of the double-memory failure, within the 720-epoch validated bound.  Only used when network.zwd_seam_reseed.
        bool   network_resume_frame_restore_{false};       ///< Resume frame restore (SPEC_synthetic_minimum_network §18.10): at a warm resume, restore the snapshot's phase-anchor incumbents + sticky pin targets, the Q3 clock-anchor per-satellite state and the CLK-R held frame, so the first post-resume election / clock fold / emit continue the pre-seam realization.  Capture has its own write knob, network.state_snapshot_frame_capture (format v2); this knob only decides whether a resume uses the rows.  Not fingerprinted (forward-only resume response policy).  Off = byte-identical.
        bool   network_resume_coupling_restore_{false};    ///< Resume coupling-regressor restore (§18.12): at a warm resume, rebuild every restored leveling / ZWD carry's coupling regression on the free shared border from the v3 snapshot's QS dictionary + QL/QZ rows (verbatim, harvest order; never re-sparsified, ŝ_prev / P_prev never re-derived) and seed tu_reinit_hist from the columns the write epoch was re-drawing (loose CA anchors, pending eph cutovers, the QR refloat set), so the first PASS-B inject builds a conditional row (A_s = M_r, y = (value_abs − x_lin) + Σ M_j·ŝ_prev,j at the pure conditional variance) instead of the plain diagonal prior asserted at that conditional variance, which outvotes most Q3 anchored clock priors at the first fold (§18.11).  The loader requires resume_frame_restore + carry_conditional_coupling + carry_coupling_gauge_project).  The restore changes the row's direction only; var is never modified.  Not fingerprinted (forward-only resume response policy).  Off = byte-identical.
        bool   network_resume_datum_restore_{false};       ///< Resume datum restore (RS-3, SPEC_event_harness §21.7), the reader half of network.state_snapshot_datum_capture: at a warm resume, restore the eq34 chain datum's last-epoch spanning tree (eq34_datum_rx / eq34_rx_parent / eq34_sat_root / eq34_prev_target / eq34_prev_el / eq34_retained_edges) before the first election, so the sticky-tree seed continues the writer's tree instead of re-electing it from the cold arc registry, and restore the time-linked code-bias memory (code_bias_prev_ / code_bias_sat_seen_) with code_bias_datum_resumed_ left false, so a re-seed disc-bumps only where it actually re-seeds.  The loader requires resume_frame_restore (the sat-side half of the same tree).  If a half's rows are absent, that half behaves as if off and a warning is logged.  Not fingerprinted (forward-only resume response policy).  Off = byte-identical.
        bool   network_resume_emit_restore_{false};        ///< Resume emit-layer restore (RS-5, SPEC_event_harness §21.10), the reader half of network.state_snapshot_emit_capture: at a warm resume, restore the eq34 emit re-base engine's emission frame (NetworkEmitRebase::last_emit_, the ER rows) before the first emit, so the emitted phase biases continue the writer's emission level instead of stepping to the raw solve frame (metres per (sat,signal) at a field seam).  Inert and logged when its ER rows are absent.  The loader requires resume_frame_restore.  Outputs-only.  Not fingerprinted (forward-only resume response policy).  Off = byte-identical.
        bool   network_resume_station_restore_{false};     ///< Resume station-layer restore (RS-6, SPEC_event_harness §21.13), the reader half of network.state_snapshot_station_capture: at a warm resume, restore the rx-CB carry (the RC rows, through makeRestoredLocalCarry so each entry carries the §15.4.1 restored-arc sentinel) and the T5 / Iono-2 per-LOS deviation carry (the SD rows, as written; ShDevCarry's cold test is t_s <= 0, so the restored harvest stamp is what makes the entry read warm) before the first epoch, instead of cold-starting both (without it the rx-CB inject gate withholds about twice as many priors on a leg as on the seed, and hundreds of deviation entries cold-seed at +1).  Each half is inert and logged when its rows are absent.  The loader requires resume_frame_restore.  Not fingerprinted (forward-only resume response policy).  Off = byte-identical.
        bool   network_resume_station_restore_rxcb_{true}; ///< Diagnostic sub-switch: restore the rx-CB half (RS-6).  Meaningful only under network_resume_station_restore_, and only so one leg of the §21.13 attribution matrix can restore one half.  Default true ⇒ an ordinary armed resume restores both.
        bool   network_resume_signal_canon_restore_{false};///< Resume canonical-signal restore (RS-8, SPEC_event_harness §21.16), the reader half of network.state_snapshot_signal_capture: at a warm resume, restore each station's per-(constellation, band, obs-type) canonical-signal latch (the CN rows) into its PppModel before the first epoch, so the first-epoch guard of decideConstellationCanonicals leaves it alone instead of re-deciding it from the resume epoch's satellite counts (a re-decided latch can flip a station's L2 signal, change the L2X tracker set, cost the eq34 L2X component its datum receiver, and step the L2X phase-bias families by tens of metres at +1).  A file without CN rows is inert and logged.  The loader requires resume_frame_restore.  Not fingerprinted (forward-only resume response policy).  Off = byte-identical.
        bool   network_resume_rxcb_lin_restore_{false};    ///< Resume rx-CB linearization-point restore (RS-9, SPEC_event_harness §21.18), the reader half of network.state_snapshot_rxcb_lin_capture: at a warm resume, seed each station's code-bias state from the snapshot's RL rows instead of from the resume epoch's one-shot code median, so the rx-CB inject gate judges the RS-6-restored carry against the frame the writer judged it against (without it, stations whose seed differs are refused every epoch of a leg and lose their receiver-DCB memory).  Solve-invariant under this posture (an x_lin shift is an exact solve no-op; the only consumer is the gate); the loader refuses the pair with network.rx_code_bias_datum, whose RHS reads x_lin.  The loader requires resume_station_restore + its rxcb half.  Not fingerprinted (forward-only resume response policy).  Off = byte-identical.
        bool   network_resume_border_restore_{false};      ///< Resume dense-border restore (RS-7, SPEC_event_harness §21.14): at a warm resume, rebuild the C1 dense shared-carry covariance (SharedCarryCov) from the snapshot's QK/QP records for the keys that also have a restored D row, so the first warm fold runs the dense (P_prev+Q)^-1 path instead of the per-key diagonal the resume otherwise degrades to (the "warm resume restored N diagonal carries" line).  Deliberately separate from network_resume_coupling_restore_: that knob also restores the coupling rows (QL/QZ regressors), which need the gauge projection for the family identity M^T g_s = -g_r(c); a covariance block carries no regression and no gauge identity, so it is legal on a carry_invariant lineage where 1f is out of regime.  The loader requires resume_frame_restore and warn-disarms without shared_carry_full_cov.  Not fingerprinted (forward-only resume response policy).  Off = byte-identical.
        bool   network_resume_pb_seam_mark_{false};        ///< Warm-resume phase-bias seam mark (RS-4, SPEC_event_harness §21.9): at the first emit after a warm resume, if more than kResumePbSeamMarkFraction of a constellation's fresh-baseline (sat,signal) phase-bias keys differ from their restored emitter baseline by more than discThresholdM, markDatumChange that constellation as the CLK-R resume_seam rule does (E-record datum_changed 1 + a PB_FRAME/resume_seam event + the n_pb_seam_marks counter).  Covers the case the CLK-R rule misses: that rule fires only when the held-frame clock engine reports epochs==0, so a lineage that restores the frame is never marked, which is exactly when the clock is continuous and the phase-bias level is not.  One shot, warm resumes only (the latch is never set on a cold start).  Outputs-only: no estimate moves.  Not fingerprinted (forward-only resume response policy).  Off = byte-identical.
        double network_readmit_seed_horizon_s_{0.0};       ///< Re-admission continuity (READMIT, E2 seed-first re-admission): park a departed satellite's D2 clock + code-bias carry for this many seconds and seed its re-registered columns at var + q·gap (the Q3 anchor folds anchored over the true gap up to this bound; its guard is the mark-if-moved).  0 = off (byte-identical).  Not fingerprinted.
        int    network_readmit_coupling_marg_ep_{0};       ///< Re-admission re-coupling edge (READMIT): epochs after a satellite's re-admission during which every coupling term on its shared columns takes the departed route (block/row edge).  0 = off.
        bool   network_readmit_seed_code_bias_{true};      ///< Re-admission seed sub-knob (READMIT): also park + seed the departing satellite's per-signal code-bias columns (true) or the clock alone (false; FALSIFIED as an improvement).  Inert without readmit_seed_horizon_s.
        bool   network_canonical_pair_admission_{false};   ///< Canonical-pair admission (docs/refound/CPA_canonical_pair_admission_2026-09-20.md; ledger KEEP_OFF): a (station, satellite) record enters the network path only while the station holds its canonical phase and code on band digits 1 and 2 (its own PppModel latch decides them), or the last good processed epoch is within canonical_pair_keep_epochs; a withheld record is absent downstream exactly as if the RINEX record were missing, so the row-level gap re-entry tier sees the return as a re-entry.  A constellation with no canonical pair decided on those digits is left untouched.  Off = byte-identical (no copy of the epoch is taken).  Not fingerprinted.
        int    network_canonical_pair_keep_epochs_{3};     ///< Canonical-pair admission tolerance, in processed epochs: a record without its canonical pair is still admitted while the last good epoch is at most this many epochs back.  Clamped at 0 (0 admits only the epochs that hold the pair).  Inert without canonical_pair_admission.
        int    network_resume_d5_warmup_ep_{0};            ///< Epochs after a warm resume during which the D5 innovation gate is diagnostic-only (RS-2b, SPEC_event_harness §21.6): a tripping chord is counted (d5_warmup_suppressed and the epoch's n_d5_flags) but not re-tapped, not marked slipped, and its block is not refolded, so "branch correct / cold station layer wrong" can be told from "branch inconsistent".  The σ̂₀ feed, the v3 corroboration gate and the MW/LLI/IF-TD drains are untouched.  Counted in processed epochs at the install seam.  0 = off (byte-identical).  Not fingerprinted.
        int    network_resume_slip_warmup_ep_{0};          ///< Epochs after a warm resume during which the MW/LLI (T0) slip drain is diagnostic-only (its cold reference fires on many arcs at the second warm epoch); T1 IF-TD and wind-up drains stay live.  0 = off.  Not fingerprinted.
        bool   network_carry_rx_code_bias_{false};        ///< Carry per-station receiver code biases across epochs as diagonal RW priors, harvested outside the phase-leveling gate (rx-CB carry, SPEC_rx_cb_carry). Forced off without code_datum_time_linked. Off = byte-identical.
        bool   network_carry_rx_code_bias_in_block_{false};  ///< Carry the receiver code-bias columns inside the station's jointly whitened local block as conditional coupling rows (RXCB-IN-BLOCK), instead of the diagonal fold at the conditional variance, which re-asserts the shared border every epoch.  Requires carry_rx_code_bias, carry_conditional_coupling and a local block.  Off = byte-identical.
        bool   network_amb_float_drop_decouple_always_{false};  ///< A float-vehicle drop clears the regression and block row even at birth strength.
        double network_dense_fold_kappa_{1e-12};          ///< Relative-pivot bound of the dense border fold; default 1e-12, 1e-6 = the historical constant.
        double network_rx_cb_carry_q_{1e-9};              ///< rx-CB carry RW process noise (m²/s); var_eff = var + q·Δt. Only used when network.carry_rx_code_bias.
        bool   network_carry_vce_{false};                 ///< Run the carry-row variance-component monitor (M2, LS-VCE): estimate σ̂ (Floor m² + Rate m²/s) per carry family per constellation from the post-fit carry innovations and log it against the hand-set floors. A calibration instrument only; no feedback into the solve. Off = byte-identical (no accumulator, no logging).
        bool   network_carry_vce_apply_{false};           ///< LASER 5-95 variance-factor iteration (M2 v2, docs/SPEC_carry_vce_apply.md): multiply every carry fold variance by a measured, inflate-only, slew-limited α² per (family × constellation). Requires carry_vce. Solve-affecting and fingerprinted; a warm resume with it armed is a hard error (fresh convergence only). Off = byte-identical.
        std::string network_carry_vce_apply_families_{"pl"}; ///< Carry families the factor scales (M2 v2; comma list of pl / pb / cb). Must be a subset of carry_vce_components (hard error otherwise). Only used when carry_vce_apply.
        double network_carry_vce_apply_slew_{1.05};       ///< Per-epoch max multiplicative step of α² (× and ÷; M2 v2); <=1 disables the limiter. Conditioning hygiene, not the safety argument (the inflate-only clamp is).
        double network_carry_vce_apply_max_{1000.0};      ///< Ceiling on α² (M2 v2; the floor is 1 by construction: inflate-only). Entry/exit are evented.
        int    network_carry_vce_apply_min_rows_{200};    ///< Effective (forgetting-weighted) monitor rows a group needs before its factor may move (M2 v2); below it the group freezes and the trace flags it.
        std::string network_carry_vce_components_{"pl"};  ///< Carry families the monitor accumulates (M2; comma list of pl / pb / cb). Default "pl" (the phase-leveling ambiguity carry, the primary floor). Only used when carry_vce.
        int    network_carry_vce_window_epochs_{60};      ///< Exponential-forgetting half-life (emitted epochs) of the VCE accumulator (M2); ≤0 ⇒ whole-run accumulation. Only used when carry_vce.
        bool   network_cycle_slip_turbo_edit_{false};     ///< Per-station T0 MW/GF cycle-slip repair of the carried ambiguities (phase-leveling Increment 2); off = byte-identical (Increment 1).
        double network_cycle_slip_turbo_edit_frac_max_{0.15}; ///< Max rounding residual for a network T0 repair to be trusted (float-MW gate; default 0.15).
        int    network_cycle_slip_turbo_edit_dn_max_{50};      ///< Max |dN| (cycles) for a trusted network T0 repair; a larger repair is left untouched and the carry kept (float-MW magnitude guard).
        bool   network_cycle_slip_mw_detect_{false};           ///< Use the MW detector as a pure detector (not the TurboEdit repair): a confident wide-lane integer jump or a receiver LLI loss-of-lock refloats the carried ambiguity (marks pending-reseed), never applying an integer.  Iono-safe at the 30 s product rate, where MW+GF integer repair is not.  Complements the IF-phase TD detector; off = byte-identical.
        bool   network_cycle_slip_datum_station_detect_{false};///< Extend MW/LLI slip detection to the gauge-pinned datum station (legacy pivot datum), responding with a network re-reference (hard PB carry skip-stash + all-station ambiguity reseed + disc mark) instead of a local refloat.  Requires mw_detect; inert under phase_datum_prioritized; off = byte-identical.
        bool   network_cycle_slip_if_phase_td_{false};         ///< IF-phase between-sat/between-epoch triple-difference slip detection (refloats both bands of a flagged IF pair via pending-reseed when the carry is active); off = byte-identical.
        double network_cycle_slip_if_phase_td_k_mad_{8.0};      ///< k for the network IF-phase MAD gate: flag if |ΔΔφ_IF−med| > max(k·1.4826·MAD, floor). Loose (8–12); post-estimation phase validation is the sharp second pass.
        double network_cycle_slip_if_phase_td_floor_frac_{1.0}; ///< Floor as a fraction of the (1,1) IF jump c/(f1+f2)≈10.7 cm; 1.0 = the (1,1) jump itself (default; binds on a clean network), ~0.7 to catch a bare (1,1) reliably.
        int    network_cycle_slip_if_phase_td_min_pool_{30};    ///< Min pooled per-pair samples for a robust network MAD (advisory; under-count pairs are logged).
        double network_cycle_slip_if_phase_td_ewma_alpha_{0.15};   ///< EWMA recency weight for the per-(station,pair) elevation-normalized IF-phase noise scale; smaller = longer memory.
        double network_cycle_slip_if_phase_td_shrink_kappa_{20.0}; ///< Clean-sample count at which the per-station IF EWMA scale and the network pool prior are weighted equally (empirical-Bayes shrinkage w=count/(count+κ)).
        bool   network_cycle_slip_relevel_free_epoch_{false};             ///< Latched free epoch for detector-marked arcs (SPEC_slip_relevel_walk).
        bool   network_cycle_slip_relevel_delta_bridge_{false};           ///< Float 2×2 level bridge from MW + IF-TD (SPEC_slip_relevel_walk).
        double network_cycle_slip_relevel_bridge_max_cycles_{200.0};      ///< Bridge reject: |ΔN_band| cap (cycles).
        double network_cycle_slip_relevel_bridge_max_sigma_cyc_{0.50};    ///< Bridge reject: σ(ΔN_band) cap (cycles).
        double network_cycle_slip_relevel_bridge_wl_sigma_floor_cyc_{0.02}; ///< WL integer 1σ floor (cycles; a confident integer's residual, not a float's).
        double network_cycle_slip_relevel_bridge_if_sigma_floor_m_{0.005};  ///< IF step 1σ floor (m).
        double network_cycle_slip_relevel_bridge_corrob_cyc_{3.0};          ///< Free-level corroboration gate (cyc; bridge v2).
        double network_cycle_slip_relevel_bridge_pend_window_s_{120.0};     ///< Pend expiry window (s; bridge v2).
        bool   network_cycle_slip_relevel_bridge_gf_{false};                ///< Same-pair GF-phase-TD float row, which closes the (1,2) plane without waiting for the MW integer (bridge v3, E4).
        double network_cycle_slip_relevel_bridge_gf_sigma_floor_m_{0.01};   ///< σ floor (m) on the empirical GF-step scale (bridge v3, E4).
        bool   network_cycle_slip_windup_restart_refloat_{false};           ///< Refloat arcs that a wind-up accumulator restart stepped (SPEC_phase_windup_sign §10.4); see @ref networkCycleSlipWindupRestartRefloat.
        bool   network_raim_{false};              ///< Post-fit network RAIM (Increment 3, T2): cross-station post-fit residual consensus refloats a carried ambiguity whose slip the pre-processors missed; off = byte-identical.
        double network_raim_k_sigma_{6.0};        ///< Standardized post-fit residual threshold (σ) above which a carried-ambiguity phase row is flagged slip-contaminated.
        double network_raim_sigma0_floor_{1.0};   ///< Floor on the RAIM robust a-posteriori variance factor σ̂0 = max(floor, 1.4826·median|v·√w|); 1.0 default (never below the a-priori σ); lower lets σ̂0 drop below 1 (tightens the gate).
        int    network_raim_min_stations_{3};     ///< Cross-station consensus count: a satellite flagged at ≥ this many stations is refloated network-wide (catches the faulty station whose slip is hidden in the shared clock).
        int    network_raim_arc_persist_epochs_{1}; ///< Consecutive-epoch persistence gate for isolated-arc attribution refloats: an isolated (station,sat) spike is refloated only after it trips the gate this many epochs in a row (real slips persist; heavy-tail spikes do not). 1 = act immediately (byte-identical). Only affects the isolated-arc tier, not consensus/whole-sat refloats.
        bool   network_raim_refloat_maturity_gate_{false}; ///< Suppress isolated-arc attribution refloats until the filter has matured (T2d; innovation-variance / σ̂0-EWMA stabilized, mirroring the warmup URA ramp): the isolated-arc refloat drives a self-sustaining warmup churn (2 → 1063 per epoch), and early arcs are warmup transients, not real slips. Whole-sat consensus refloats (network-wide contamination) are not gated. Off = byte-identical (arcs refloat immediately).
        int    network_raim_refloat_maturity_epochs_{0};   ///< Emitted-epoch maturity horizon for the isolated-arc refloat gate above (T2d; passed as the warmup-ramp horizon and combined with @c phase_leveling_warmup_epochs and the σ̂0-EWMA update count as in @c networkWarmupRampLifted). 0 = no horizon beyond the carry warmup (the gate is then effectively inert, since RAIM normally runs only once carry-warm); a value > phase_leveling_warmup_epochs is needed to suppress anything.
        double network_signal_min_station_fraction_{0.0}; ///< Network-wide signal coverage gate: a RINEX code is admitted to the producer only if ≥ this fraction of stations observing its constellation track it (drops sparse channels like L2S everywhere, unlike per-station channel_mode). 0 = off (admit all tracked channels, byte-identical). Network mode only.
        double network_phase_bias_min_station_fraction_{0.0}; ///< Phase-bias coverage gate: a signal below this network coverage keeps its observation but gets no satellite phase-bias state (float ambiguity absorbs it, signal not AR-eligible); the "float, not drop" alternative. 0 = off (every phase signal gets a bias, byte-identical). Network mode only.
        bool   network_station_marker_to_arp_{false};              ///< Reduce every roster station's configured position_ecef from the monument to the antenna reference point at setup (network.station_marker_to_arp), via Coordinates::markerToArp with that station's RINEX `ANTENNA: DELTA H/E/N`: the helper and frame the held-out consumer's truth already uses. The roster is held fixed at a published (monument) coordinate while the observations refer to the antenna and the ATX model is ARP-relative, so the station misfits every range by H·sin(elev); its zenith wet delay absorbs about a quarter of H and the rest leaks into the satellite clocks regionally. A station whose header has no eccentricity record is left unchanged and warned; a zero eccentricity is an exact no-op. Off = byte-identical (eccentricities are read and reported only). Fingerprinted (armed-only append). Network mode only.
        std::vector<std::string> network_rhs_attrib_columns_;      ///< Named shared columns of the attribution's solution-share readout (network.rhs_attrib_columns): every attribution dump also writes logs/rhs_attrib/cols_<tow>.csv, each contribution's reduced-RHS load and solution share at these columns; a LOC:<station>:<key> label names a station-local column instead, decomposed into loc_<tow>.csv at the armed dumps and a trip's current-epoch dump. Empty (default) = no cols_ or loc_ file. Diagnostics only, not fingerprinted. Network mode only.
        bool   network_signal_roster_{false};                      ///< Write the arc ledger's trace (network.signal_roster, a trace switch): logs/network_arc_ledger.csv and the ARC event rows of the roster's build and audit. The per-station signal roster, the arc ledger and their disagreement census are built on every network run whatever this says. Trace class. Not fingerprinted. Network mode only.
        bool   network_roster_tracker_{false};                     ///< The arc-level slip tracker (networkComputeRawObservables) names each band by the roster's modelled_phase for that (station, constellation, band) when that phase is reported with its matching code this epoch, falling back to the lexical rule only where the roster names none (network.roster_tracker). The first reader of the roster, which is built on every run; supersedes network.cycle_slip.tracker_modeled_first by construction (the roster's modelled role is that knob's latch), and both may be armed. Not fingerprinted. Network mode only.
        bool   network_ledger_reentry_{false};                     ///< The gap re-entry response takes its population from the arc ledger's admitted lifecycle (network.ledger_reentry): every arc whose admitted state goes Gap to Reentered this epoch, whose signal the roster calls Modelled, and whose gap is inside phase_leveling_max_gap_s; it looks the raw tiers' verdict up per arc instead of being driven by them. The raw tiers keep running unchanged. An arc no raw tier saw has no verdict and is handled by network.ledger_reentry_no_verdict. Solve-affecting when armed (it changes which carried levels survive a break). Refused (error + disarm) without network.roster_tracker. Fingerprinted (armed-only append). Network mode only.
        bool   network_ledger_screens_{false};                     ///< Reason-mask writers at every screen that removes a modelled row, a satellite or a station, folded into the ledger so an admitted gap carries the reason(s) that opened it, plus the anchor election read against the ledger (first-sighting-on-return cases, the coverage-gate drop edges' arc states and generation test, the anchor_arc_age comparison) (network.ledger_screens). Observatory: byte-identical armed. Not fingerprinted. Network mode only.
        bool   network_ledger_emit_{false};                        ///< The emitter's per-family bookkeeping read against the arc ledger (the phase-bias hold's generation against the ledger's admitted arc births, both directions; the emitter's first-sighting test against the ledger's memory of the family) and the producer's declaration of its signal roster on the wire as a block of N records (network.ledger_emit). Observatory: the solve and every C / B / P / E record are byte-identical armed; the only wire difference is the N records. Not fingerprinted. Network mode only.
        bool   consumer_roster_latch_{false};                      ///< Consumer side (network.closed_loop.roster_latch): the consumer's per-band canonical latch follows the producer's declared roster where it tracks a declared signal, and a freshness flip on a signal the declaration does not name is not a staleness event. Inert (logged once) on a wire with no declaration. Derived onto the closed-loop consumer config only.
        bool   network_ledger_anchor_{false};                      ///< A family the coverage gate's drop edge erased and that returns with arcs of the same generation restores its previous incumbent as the candidate of record (subject to the election's existing rules) instead of being elected as a first sighting (network.ledger_anchor); a new-generation return is elected as without the knob. anchor_arc_age becomes a reader of the ledger's admitted age. Solve-affecting when armed. Refused (error + disarm) without network.ledger_screens. Fingerprinted (armed-only append). Network mode only.
        LedgerReentryNoVerdict network_ledger_reentry_no_verdict_{LedgerReentryNoVerdict::Bridge}; ///< Response to a re-entry with no raw verdict (network.ledger_reentry_no_verdict): `bridge` (default) leaves it to the PASS-B inject and only counts and events it as ARC arc_reentry with verdict=none; `refloat` treats it as unmeasurable, which network.cycle_slip.gap_reentry_relevel_unmeasurable then decides. Inert without network.ledger_reentry.
        bool   network_exclude_unmodeled_antenna_stations_{false}; ///< Hard exclusion (policy backstop): drop from the network entirely any station whose receiver antenna still has no ATX match after the always-on radome→NONE fallback (an unknown antenna type → fully unmodelled receiver PCO/PCV, a systematic error in every obs feeding the shared clocks/biases). Never built, counted at the build summary, echoed in the manifest; an unmodelled clock-datum pivot is a hard build error instead. Off = every loaded station is built (byte-identical). Network mode only.
        bool   network_raim_attribution_{false};  ///< Drive RAIM refloats by prominence attribution (median-polish sat/station/arc) instead of per-satellite consensus; off = consensus behaviour.
        bool   network_raim_carry_invariant_{false};         ///< Run the post-fit RAIM screen on the invariant-carry lineage (pool re-host, measure-only) by building the carry-neutral `qc_amb` census and widening the seam's call-site gate. Otherwise post-fit RAIM is structurally inert under carry_invariant: the seam is not called (carry_warm needs phase_leveling) and its pool `pl_amb` is never filled (its fill site is under `!carry_invariant`). Not emission-neutral: filling the pool makes σ̂0 real, which feeds the A4 withhold and the URA EWMA. Requires network_carry_invariant_ + network_raim_. Off = byte-identical.
        bool   network_raim_pool_data_rows_only_{false};     ///< The carried-phase RAIM pools (post-fit and within-epoch) admit the observation phase rows only, each to the pool of the ambiguity column its tag names; the carried-prior and SD-EWL rows are refused. Requires network_raim_ (loader disables it otherwise). Off = today's declared rule (byte-identical). QC knob: fingerprint-safe.
        bool   network_raim_within_epoch_{false}; ///< Arm within-epoch adaptation (detect→drop-culprit→re-solve) when σ̂0 exceeds the trigger; off = emit the first solve (byte-identical).
        double network_raim_sigma0_trigger_{3.0}; ///< σ̂0 above which an epoch is "so bad" that within-epoch adaptation fires.
        double network_raim_withhold_sigma0_{0.0}; ///< Within-epoch escape-hatch ceiling (A4): if σ̂0 still exceeds this after the 5-pass adaptation, the epoch is withheld (reusing the observability-gate disposition) instead of emitted with a warning; 0 = off (emit anyway). Used only when @ref networkRaimWithinEpoch. Typically ~2× @ref networkRaimSigma0Trigger.
        int    network_raim_withhold_adapt_escalate_{0}; ///< Withhold-era release (SPEC_withhold_era_release §3a): escalated within-epoch adaptation budget on epochs whose predecessor was A4-withheld; 0 = off (stock kMaxAdapt = 5, byte-identical); (0,5] rejected to 0 at load.
        bool   network_raim_withhold_free_erase_{false}; ///< Withhold-era release (SPEC_withhold_era_release §3b): at the A4 withhold branch, erase detector-freed leveling-carry entries so the free re-level survives the discarded epoch (reset-class only). Requires relevel_free_epoch + a withhold ceiling; off = byte-identical.
        bool   network_raim_we_code_iono_{false};  ///< Extend the within-epoch screening to the code and iono-constraint row pools (WE-CI; own triggers, station-exclusion / sat-code-deweight responses); the phase pool, σ̂0_adapt and A4 are unchanged. Off = byte-identical.
        double network_raim_we_code_trigger_{10.0}; ///< Code-pool σ̂0 trigger (WE-CI; empirical: quiet 2.5–4.7 / q99 ≈ 8; events 17.8–288 on a 305-station field day). Inert unless network_raim_we_code_iono_.
        double network_raim_we_iono_trigger_{12.0}; ///< Iono-pool σ̂0 trigger (WE-CI; a first placement, since the pool has no other statistic; calibrated from the run-end max-observed telemetry). Inert unless network_raim_we_code_iono_.
        bool   network_per_const_withhold_{false};  ///< When the A4 σ̂0 withhold trips, split the epoch per constellation (RTA-10): publish each constellation whose own σ̂0_c (same standardized-residual pool, restricted to its rows) passes the same ceiling with ≥ min_rows rows; prune the rest (RT0-5-quarantine pattern). Off = all-or-nothing withhold (byte-identical).
        int    network_per_const_withhold_min_rows_{20}; ///< Minimum σ̂0-pool rows for a constellation to be judgeable by the carve-out (RTA-10); fewer ⇒ withheld (unjudgeable under a contaminated epoch). Inert unless network_per_const_withhold_.
        bool   network_emit_epoch_status_record_{false}; ///< Write one positional H heartbeat record per epoch into the product file (RTA-10), including fully withheld epochs (gate + σ̂0), which otherwise emit nothing, so a tail consumer can distinguish "withheld" from "producer died". Positional (never feeds replay). Off = no H records (byte-identical).
        bool   network_h_sigma0_prefit_census_{false};   ///< Feed the H heartbeat's sigma0 field from the prefit census's pooled robust σ̂₀ (H-rider, edge-queue §1.6 item 3a) instead of the carry_warm-gated within-epoch RAIM statistic, which is structurally 0.000 on every lineage without leveling and a degenerate constant on leveled quiet runs.  Uniform per wire: every H site carries the census statistic when armed, and the S contract says so (hsig=pc).  Never feeds the A4 withhold ceiling (that stays sigma0_adapt).  Requires emit_epoch_status_record + trace_prefit_census.  Changes H bytes ⇒ default off (byte-identical).
        std::string network_h_sigma0_classes_{"code,sh,other"};   ///< Data classes pooled into the census-fed H σ̂₀ (H-rider; token list: code/phase/other/sh/all/none).  Default matches the census event domain; phase is deliberately excluded (arc-specific innovations are not a common-mode population, census deviation 2c).  Only used when network.h_sigma0_prefit_census.
        bool   network_raim_surgical_station_{true};   ///< Within-epoch adaptation may peel a single (station,sat) arc instead of excluding the whole station, and de-rates whole-station exclusion by @ref network_raim_station_drop_bias_; false = coarse sat-vs-whole-station peel (A/B baseline).
        double network_raim_station_drop_bias_{1.5};   ///< A whole-station exclusion is chosen only when its median-polish row effect / this factor still beats the best satellite/arc candidate (≥1; 1 disables the bias).
        bool   network_raim_redundancy_normalize_{false}; ///< Standardize the post-fit / within-epoch RAIM residual by v_i/(σ_i·√r_i) with the Baarda redundancy r_i = 1 − w_i·a_iᵀCov(θ)a_i (B2; per row, from the cached per-station Schur factors) before σ̂0 pooling / median-polish; low-redundancy arcs are otherwise under-flagged (response ∝ r_i·fault). Off = plain |v|·√w (byte-identical); on deliberately changes the statistic.
        bool   network_raim_station_factor_{false};        ///< Declare the per-station trimmed variance component Σw·v²/Σr_i as its own EWMA-tracked statistic (QUEUE item 4 R2, SPEC_t3_null_derivation §6/§7.5): trace + both-edge events + counters, never absorbed into a denominator. Measure-only; requires raim; off = byte-identical.
        double network_raim_station_factor_tau_s_{1800.0}; ///< EWMA time constant (s) of the R2 statistic; §12.1b: not yet measured, and this knob exists to measure it.
        double network_raim_station_factor_ratio_{6.0};    ///< Depart threshold of the R2 statistic: EWMA factor / network median of EWMA factors (return at 0.8×). Above the 4.7× model-error spread measured in §4.
        bool   network_raim_rate_hygiene_{false};          ///< T3/T3c population hygiene (QUEUE item 4 R4, SPEC_t3_null_derivation §6): the T3 opportunity count excludes s_raw==0 untestable cells (counted separately); the T3c opportunity becomes the unconditional ≥min_sta-cells population. Telemetry accounting only; requires raim + trace_flag_rates; off = byte-identical.
        bool   network_level_authority_csgf_{false};       ///< Per-epoch fleet CS-GF level census (LV-6, SPEC_level_authority_internal): measure-only, folds nothing; arms the CS advance without the carry; off = byte-identical.
        int    network_level_authority_min_n_{10};         ///< LOS maturity screen, CS-GF samples since reset (LV-6).
        int    network_level_authority_min_los_{4};        ///< Per-station qualifying-LOS floor for the fleet median (LV-6).
        bool   network_level_authority_los_dump_{false};   ///< Emit the census's own per-LOS pierce points + weights to network_level_authority_los.csv so a reference field can be reduced through the identical sampling (LV-6 G2; §4 G2 / §5). Trace-only, own file, inert unless network_level_authority_csgf_.
        bool   network_clock_monitor_{false};           ///< Per-satellite carried-clock sanity monitor: flag a satellite clock past a physical bound and reinitialize its carry (by default the soft reinit of network_clock_monitor_reinit_sigma_m_), which breaks the multi-hour single-satellite lock-in the σ̂0 RAIM does not see; off = byte-identical.
        double network_clock_monitor_abs_max_m_{20.0};  ///< |Δδtˢ − median| bound (m): a satellite whose de-meaned clock exceeds this has its carried shared state reinitialized.
        double network_clock_monitor_jump_max_m_{10.0}; ///< One-epoch clock jump bound (m): a satellite whose clock moves more than this across adjacent stable-datum epochs has its carried shared state reinitialized.
        bool   network_clock_monitor_couple_raim_{false}; ///< Also reinitialize the carried clock of any satellite the within-epoch / post-fit RAIM flags (opt-in; off = the monitor acts on its own abs/jump bounds only, byte-identical on a clean window).
        double network_clock_monitor_reinit_sigma_m_{30.0}; ///< Reinit σ (m) a flagged sat's carried clock/phase/code bias is loosened to next epoch (soft large-q reinit that keeps a weak anchor); ≤0 = hard skip-stash refloat.
        bool   network_clock_model_{false};              ///< Carry the satellite clock with the two-state (offset+drift) oscillator model + Allan-derived Q instead of the scalar random walk; a lock-in becomes a detectable dynamics fault. Default false = scalar carry.
        double network_clock_model_q1_{0.0};             ///< White-FM diffusion q1 (m²/s) of the two-state clock model (negligible for the product-residual clock).
        double network_clock_model_q2_{1.5e-7};          ///< Random-walk-FM diffusion q2 (m²/s³) of the two-state clock model (Allan-derived; dominant term).
        double network_clock_model_k_sigma_{6.0};        ///< Normalized-innovation (σ) threshold at which the two-state clock model flags a satellite (datum common-mode removed).
        bool   network_pivot_vanish_return_signal_{false}; ///< Also raise a datum change (markDatumChange) when a constellation absent last epoch returns with a clock pivot != its last-ever pivot (A1); off = only the adjacent-epoch pivot change signals (byte-identical).
        bool   network_emit_clock_disc_{false};            ///< Maintain a per-satellite clock discontinuity counter (bumped on a per-constellation datum re-reference) and scope the forced phase/code bump to the affected constellation(s) (A2); off = no clock disc channel + a global bump (byte-identical).
        bool   network_code_bias_disc_jump_test_{false};   ///< Emitter code-bias magnitude jump test: also bump disc_per_signal on a fresh-baseline value jump > the phase disc threshold (0.05 m), which signals the weak-separation value wander the H1 structural triggers cannot see; travels in the S-contract (cbjump=). Off = structural triggers only (byte-identical).
        double network_disc_threshold_frac_cycles_{0.0};   ///< Per-λ discontinuity threshold as a fraction of a carrier cycle (H3); when > 0 the emitter jump test uses frac·λ(sat,signal) instead of the flat 0.05 m disc_threshold_m (uniform in cycles, still a fixed contract); travels in the S-contract (discfrac=). 0 = legacy flat-metre gate (byte-identical).
        bool   network_clock_disc_on_set_change_{false};   ///< Sum-to-zero datum-move signalling: bump a constellation's clock disc counters when its clock-satellite set changes between emits (under Σδtˢ=0 a membership change re-references the datum, which otherwise goes unsignalled); requires clock_datum_sum_to_zero, arms emit_clock_disc; travels in the S-contract (clkset=). Off = forced-bump-only clock channel (byte-identical).
        bool   network_sticky_pivot_{false};               ///< Keep the incumbent clock-pivot satellite per constellation while it remains registered (RTA-1; re-select only when it sets; a config-pinned satellite still wins); off = lexical first-registered auto-pick every epoch (byte-identical).
        bool   network_clock_datum_sum_to_zero_{false};    ///< Realize the per-constellation clock gauge by a sum-to-zero over that constellation's satellite clocks (folded like the code-bias gauge) instead of a hard pivot pin: anchor-free, no handover seam, no pivot-absent SPOF; ignores clock_datum_satellite. Off = legacy per-constellation pivot pin (byte-identical).
        bool   network_clock_datum_broadcast_anchored_{false}; ///< brdc_rts only: realize the clock gauge by per-satellite soft anchors to each satellite's own broadcast clock (the a-priori; σ = resolveNetworkAprioriClockAnchorSigmaM(), folded before the gate as the datum): no pivot, no handover seam, and unlike sum-to-zero no common-mode redistribution wander on rise/set, so the absolute clock-product frame holds still against the broadcast reference. Mutually exclusive with clock_datum_sum_to_zero; requires apriori_source=brdc_rts (fail-loud). Off = unchanged (byte-identical).
        bool   network_code_datum_time_linked_{false};     ///< Impose the code-bias datum (per-sat IF pin + per-(const,signal) sum-to-zero) as a time datum (W6 only): pin the change from the previous epoch's solved values (veteran-referenced sum-to-zero RHS Σ b̂_prev, IF-pin RHS a1·b̂_prev(σ1)+a2·b̂_prev(σ2)) instead of the level to zero. Same row count (rank unchanged), so no set-mean wander/relaxation and no conflict between gauge and carry weights; a no-veteran channel or first split re-seeds at 0 with a per-sat disc bump (value step ↔ signalled discontinuity). The prev-value map crosses a warm resume only with state_snapshot_datum_capture + resume_datum_restore; otherwise a warm resume re-seeds, signalled. Requires estimate_sat_code_bias (inert and warned otherwise). Off = legacy zero-target datum (byte-identical).
        bool   network_code_datum_pair_continuity_{false};  ///< Hold then continue: at an IF-pin pair change with previous solved values on both new signals, target the new pair's previous IF combination (the clock continues) instead of re-seeding at 0, with no re-seed mark and the re-selection discontinuity left undeclared when the last emitted pair is the pair continued from; where an entering signal has no previous value yet, keep the last kept pair (its own previous combination carries) for the one kept epoch until it has, unless the held pair has no previous values or, under broadcast_tgd, the group delay describes the selected pair and not the held one. Read only under code_datum_time_linked and where no row re-asserts an absolute target at a pair change (NetworkSbasis::pinPairContinuityBlocker); inert and warned otherwise. Fingerprinted when armed. Off = re-seed at every pair change (byte-identical).
        bool   network_code_datum_family_difference_{false}; ///< Replace a secondary-family (X) Galileo code channel's surviving sum-to-zero row by Sigma (d_X - d_P) over the satellites carrying both it and its same-band primary-family partner (time-linked like the sum-to-zero row; the sum-to-zero row kept with no partner, no common satellite, all spanned X columns pinned, or for the one epoch a family re-seed would replace a carried level). Read only under code_datum_time_linked (inert and warned otherwise). Fingerprinted when armed. Off = the sum-to-zero row (byte-identical).
        double network_code_family_birth_prior_sigma_m_{0.0}; ///< Sigma (m) of the first-sighting family prior, one soft shared row d_X - d_P = mu (mu = the family row's veteran mean of the previous differences, 0 with no veteran) folded once per (satellite, birth) when a Galileo band-1 secondary (X) column or its primary partner is born (the carry does not continue it; a READMIT seed does) while the other is registered; no row where no realized family mean exists (bridged / dependent without a veteran). Read only with code_datum_family_difference under code_datum_time_linked (inert and warned otherwise). Fingerprinted when armed. 0 = off (byte-identical); negative, non-finite or < 0.01 m refused at load, <= 1/sqrt(1e3) m warned.
        bool   network_scoped_handover_response_{false};   ///< Scope the producer's internal pivot-handover response (RTA-2; D2 carry skip, clock-oscillator clear, clock-monitor jump suppression) to the constellation(s) whose datum changed; off = global response (byte-identical).
        bool   network_handover_stransform_{false};        ///< At a clock-pivot handover, transform the changed constellation's carried shared means along NetworkSbasis::clockNullDirection (c0 = x_q/v_c[q]; x -= v_c*c0; var += v_c^2*var_q) and then fold them instead of skipping (queue 1(b)); the carried information is valid, only expressed in the retired datum. Requires scoped_handover_response (inert and warned otherwise); inert under an anchor-free clock datum. Fingerprinted when armed (armed-only token) ⇒ fresh convergence only. Off = the legacy one-epoch drop (byte-identical).
        bool   network_handover_stransform_before_rows_{false};  ///< Run the handover S-transform before the fold decision and PASS B, through the traversal of carried references, so the coupling rows read the re-expressed carry.  Requires handover_stransform (inert and warned otherwise); refused with apriori_source brdc_rts.  Fingerprinted when armed.  Off = the transform after PASS B (byte-identical).
        bool   network_handover_stransform_dense_{false};  ///< At a clock-pivot handover re-express the stashed border by the exact S P S^T (values and prev_carry_cov together; the incoming pivot's row zeroed, the outgoing pivot appended at -c0) instead of the diagonal bump.  Inert without handover_stransform or shared_carry_full_cov; refused with handover_stransform_before_rows and carry_rx_clock_in_block.  Fingerprinted when armed.  Off = the diagonal kernel (byte-identical).
        bool   network_clock_anchor_datum_keep_{false};    ///< The clock anchor's guard does not decouple (reason datum) an anchored clock whose constellation the dense S-transform re-expressed this epoch with its self-check passed, when the clock has a block row and folded anchored.  Inert without handover_stransform_dense or clock_temporal_anchor.  Fingerprinted when armed.  Off = the decouple (byte-identical).
        bool   network_carry_rebase_complete_{false};      ///< Each carry re-base mechanism (the ephemeris re-level, the eq34 D4 shift, the declared anchor handover) also moves the carried references its exemption table marks as owed, the phase hold's held value excepted.  Inert and disarmed without such a mechanism.  Fingerprinted when armed.  Off = the phase 2 site sets (byte-identical).
        bool   network_phase_datum_prioritized_{false};    ///< Realize the phase-bias gauge by a per-(sat,signal) prioritized anchor receiver (highest-coverage tracker) instead of pinning only the single clock-pivot station; removes the low-elevation gauge jump and the pivot-absent SPOF, with anchor handovers disc-signalled. Off = legacy single-pivot pins (byte-identical).
        bool   network_phase_datum_eq34_{false};           ///< Realize the phase-bias gauge by the Eq-34 / de Jonge single-receiver-chain datum: per-(receiver,system,signal) RPB split state + one RPB pin + a spanning tree of ambiguity pins per (const,signal) component, so the emitted between-satellite reference stays integer-consistent across all satellites while staying full-rank at global scale. Reuses the prioritized machinery that removes the SPOF but replaces the anchor election with a chain election; supersedes phase_datum_prioritized. Off = anchor-pin datum (byte-identical: no RPB state allocated, chain election never runs).
        std::string network_phase_datum_eq34_receiver_{};  ///< Preferred datum receiver (component 0) for the eq34 chain datum; empty = coverage auto-pick. The sticky incumbent still wins while present. Inert unless network_phase_datum_eq34_.
        bool   network_phase_datum_eq34_pin_round_{true};  ///< On an eq34 re-attach/re-pin of an already-lit (sat,signal) edge, target the rounded carried float (λ·round(x̂_prev/λ)) so the frame step is ≈0 cycles; false = always pin to 0 (A/B lever). Inert unless network_phase_datum_eq34_.
        bool   network_phase_datum_eq34_seed_round_{false}; ///< Seed-round (docs/SPEC_eq34_seed_round.md): round otherwise-zero pin targets to λ·round(x_state/λ) (first sighting, dead-carry incumbents, and every carry_invariant epoch).  |pin innovation| ≤ λ/2 by construction.  Default off; refused without network_phase_datum_eq34_.
        bool   network_phase_datum_eq34_seed_round_rehost_frame_{false}; ///< Seed-round re-host (docs/SPEC_eq34_seed_round.md §8.5): a tree pin that moves to a new host takes its frame step from that host's own ambiguity state (exact pre-pin float) and S-transforms the carries, instead of an unknown step that re-references the (sat,σ) and marginalizes its carried PB prior. Default off; refused without network_phase_datum_eq34_seed_round_.
        double network_phase_datum_eq34_rpb_prior_sigma_m_{1.0}; ///< Loose per-epoch zero-mean prior σ (m) on every RPB column: the station-local information floor for the near-null RPB↔ambiguity split (field A/B instrument); 0 disables. Inert unless network_phase_datum_eq34_.
        double network_phase_datum_eq34_rpb_q_m2s_{3E-06};       ///< q (m²/s) of the local δ_{r,σ} time-linking carry (D6 v2, local form), which suppresses the per-epoch RPB re-estimation bounce that corrupts the individual-float phase-leveling carries; 0 = v1 fresh re-estimation. Inert unless network_phase_datum_eq34_.
        bool   network_phase_datum_eq34_carry_shift_{false};     ///< Experimental: S-transform carried values on pin displacement instead of erase/skip (exact in the chain algebra; in the field it injects error through the weakly held δ split, DD step 86 against 11–15 cyc/epoch for erase-based hygiene; root cause open). Off = erase-based hygiene. Inert unless network_phase_datum_eq34_.
        bool   network_phase_datum_eq34_longevity_election_{false}; ///< Grow the eq34 spanning tree's new edges by arc longevity (rising arcs first, then high elevation; high-coverage receivers first) instead of lexical order, so tree edges are chosen to outlive the epoch, which reduces b_s re-reference churn at the source (the preference of spec D2 step 3). Incumbent-edge seed untouched; deterministic tie-break; off = legacy lexical growth (B9 baseline). Inert unless network_phase_datum_eq34_.
        bool   network_phase_datum_eq34_extend_only_{false};       ///< Extend-only membership (de Jonge §3.5): retain a tree edge's sticky identity (parents/root/pinned integer) across a momentary row gap while its local_carry is alive (age-stamped to phase_leveling_max_gap_s), so a returning arc re-pins to its pre-gap integer (step ≈0) instead of silently re-leveling to a first-sighting 0. Tree growth unchanged (live arcs only); reroute/handover out of scope. Off = legacy wholesale replace (B9 baseline). Inert unless network_phase_datum_eq34_.
        bool   network_carry_invariant_{false};            ///< Time-link the DD cycle-basis invariant carry (NetworkInvariantCarryDriver) through the producer solve: the S-system-invariant replacement for the S-dependent PB/local/rpb carries. Per (const,signal) component the eq34 tree's chords are the carried DDs (q_z=0); each epoch the carry is basis-projected, folded as a rank-K DD prior, and the posterior read back. Requires phase_datum_eq34 (needs the RPB split + spanning tree; load-time reject otherwise); GLONASS excluded (FDMA). Supersedes the PhaseBias shared carry + phase-leveling local carry + rpb carry + eq34 pin-round/shift/reref hygiene for phase keys. Off = no z-store allocated, all folds gated (byte-identical).
        double network_carry_invariant_qz_{0.0};           ///< z process noise (m²/s) on the carried DDs; 0 = constant ambiguities (correct in the absence of slips). Inert unless network_carry_invariant_.
        bool   network_carry_invariant_emit_rebase_{true}; ///< Re-base the emitted per-key biases onto the previous emission frame (phase D; integer + common-fractional continuity vs the last emission): an S-selection on the outputs only; user DDs are invariant. Inert unless network_carry_invariant_ (phase-D emit layer).
        bool   network_carry_invariant_innov_gate_{false}; ///< Estimation-side fine-slip innovation gate on the carried DDs (D5): the invariant-carry-native replacement for the RAIM fine-slip layer the invariant bypass empties. Per chord, standardize the post-solve innovation v=z_free−Gᵀx_s−ẑ by its exact H0 covariance M⁻¹−GᵀΣG; a chord above the threshold is re-tapped fresh (reset, not repair) and its arc marked slipped for the next epoch (the C3c membership-exclusion path). Requires network_carry_invariant_ (load-time reject otherwise). Off = the posterior is installed unconditionally (byte-identical to the C3c path).
        double network_carry_invariant_innov_k_{4.0};      ///< Per-chord standardized flag threshold |v_i|/√Cov(v)_ii for the innovation gate (D5; default 4.0 ≈ 8-10 cyc floor at the current σ_v; k<3 fragments the carry with false alarms, warned). Inert unless network_carry_invariant_innov_gate_.
        bool   network_carry_invariant_innov_calibrate_{false}; ///< Standardize the innovation gate by the LS-VCE-estimated empirical innovation variance σ̂₀² (D5 v2; robust geo-mean of the standardized innovations, the M2 NetworkCarryVce instrument) instead of the raw formal Cov(v), which the M2 monitor measured 10-30× optimistic. Makes network_carry_invariant_innov_k a true Nσ threshold that adapts per regime (so one k serves both low- and high-elevation masks). Off = raw formal Cov(v) (v1; k is an uncalibrated knob). Inert unless network_carry_invariant_innov_gate_.
        double network_carry_invariant_innov_calibrate_window_{60.0}; ///< Exponential-forgetting half-life (emitted epochs) of the σ̂₀² accumulator (D5 v2); larger = smoother, slower-adapting. Inert unless network_carry_invariant_innov_calibrate_.
        bool   network_carry_invariant_innov_corroborate_{false}; ///< Suppress a D5 re-tap when the raw observables positively measured the arc as continuous, i.e. no MW/GF step (D5 v3). Measured: only 2.1% of D5 re-taps coincide with a raw step, against 95% on the same run's known arc resumptions, so the gate fires on persistent model error, not slips, and re-taps continuous arcs indefinitely (97.4% of inter-reset gaps are 1 epoch), which pins dd_sigma_cyc at ~3.9 cyc and keeps DD fractions uniform at every baseline. Fail-safe: only measured-and-flat suppresses; without evidence the chord still re-taps, so the response required by invariant #1 is narrowed, never removed. Inert unless network_carry_invariant_innov_gate_ and a slip-QC decision instrument ran the qualify pass (SlipQcState::decisionInstrumentArmed(): cycle_slip.gap_instrument / .mw_lli_corroborate / .relevel_bridge_gf / the CS-GF authority; network.trace_slip_raw does not qualify, since it computes the observables without adjudicating them). The loader auto-arms gap_instrument when nothing else would (loadNetworkSection_ tail). Off = byte-identical to D5 v2.
        double network_carry_invariant_innov_corroborate_backstop_k_{0.0}; ///< Re-tap regardless of corroboration when the standardized innovation exceeds this multiple of the gate threshold, k·σ̂₀ (D5 v3). 0 = inert (pure corroboration), the default and the recommended value: corroboration was measured to fall with innovation size (2.1% below 25σ, 0.0% at 100-200σ, 1.2% above 200σ), so a magnitude backstop re-admits the population with the least evidence of being a real slip. Provided for sweeps only. Inert unless network_carry_invariant_innov_corroborate_.
        bool   network_carry_invariant_innov_retap_only_{false}; ///< On a D5-flagged chord whose arc the raw observables measured as continuous, the in-epoch re-tap (installPosterior: mean ẑ+v, variance σ̂₀²·Cov(v)_ii, cross zeroed) is the whole response (D5 v4): the funnel entry does not also exclude the arc from next epoch's A_old membership, so the DD keeps its history instead of being refloated from a diffuse prior. Under a stable tree a chord arc has cycle support exactly 1 (its own fundamental cycle), so on that population the exclusion protects no other DD and only adds churn (on a 24 h field run, 98% of "slip refloats" were the gate's own flag→re-tap→funnel limit cycle, mean chord lifetime 4.3 epochs, dd_sigma_cyc pinned ~3.9 cyc). It is not a general removal of the refloat: tests/network/test_network_invariant_carry_driver.cpp GATE 10b measures the refloat completing a real slip's reset (the in-epoch move is partial: v is the post-fit innovation, already shrunk by the fold's −GᵀΣG border term) to 0.48 cyc, so the same fail-safe tri-state as the v3 corroboration gate applies: stepped ⇒ refloat · measured-and-flat ⇒ keep the carry · no evidence ⇒ refloat. An arc a raw detector flags in the current epoch refloats regardless. The response required by invariant #1 is narrowed to the diagnosed population, never removed. The funnel insert is untouched, so every side consumer (Iono-2 v3k erase classifier, WL-3 MW-row slip-skip, anchor hygiene) is unaffected. Composes with `_innov_corroborate`, which suppresses the flat population one level earlier; with v3 armed this knob is largely inert by construction, and the pair is the "re-centre the DD" vs "leave it alone" A/B on model tension. Inert unless network_carry_invariant_innov_gate_ and a slip-QC decision instrument ran the qualify pass (SlipQcState::decisionInstrumentArmed(): cycle_slip.gap_instrument / .mw_lli_corroborate / .relevel_bridge_gf / the CS-GF authority; network.trace_slip_raw does not qualify, since it computes the observables without adjudicating them). The loader auto-arms gap_instrument when nothing else would (loadNetworkSection_ tail). Off = byte-identical to D5 v3.
        bool   network_carry_invariant_innov_carried_only_{false}; ///< Evaluate the innovation gate only on chords carried into this epoch (D5 v5). The test is |v_i| > k·σ̂₀·√Cov(v)_ii with Cov(v) the H0 covariance about the carried prior; a chord born this epoch has no carried prior (its prior is the diffuse fresh_prior_var), so there is no hypothesis to test, the reset it triggers is a no-op (a diffuse chord's posterior already is the data-only tap-off), and the flag's only lasting effect is the funnel entry that re-births it next epoch, which flags again. On a 63-station field run, 95.3 % of flags sat on born chords at exactly the diffuse √var = 1000.000 m. A chord re-born every epoch never accumulates anything, which is why dd_sigma_cyc cannot leave its pin and med|frac_cyc| stays uniform at 0.25; this is the cause of the D5 churn, upstream of both the v3 corroboration gate and the v4 re-tap-only knob, which suppress the same population for incidental reasons. The D5 v2 design excludes born chords from the σ̂₀ accumulator for the same reason (a born chord's diffuse Cov(v) would drag σ̂₀² down); this knob applies that exclusion to the gate decision, reusing the same chord_carried mask so the two cannot drift. Inert unless network_carry_invariant_innov_gate_. Off = byte-identical.
        bool   network_carry_invariant_pd_safe_{false};    ///< PD-safe DD fold (docs/SPEC_pd_safe_dd_fold.md). The carried Pz's positive-definiteness is an algebraic invariant of the exact recursion (independent chords => full-row-rank W => W.PSD.W' + fresh diagonal is PD; the exact posterior of a PD prior is PD) and breaks only numerically. Armed, the fold (a) reads the posterior back in product form sym(Pz.M.B) instead of subtracting two 1e6-scale operands to reach a 1e-4-scale mature row, (b) factorizes the equilibrated S(Pz+B)S against a scale-relative pivot floor (1e-12.dmax) instead of the absolute minCoeff()<=0 screen, (c) repairs a still-tripping block by clamping its eigenvalues and counts it, and (d) rebirths a block that still cannot factor (means kept, Pz re-seeded diffuse) at install and at warm-resume restore rather than freezing it indefinitely. Without it, 78% of the carried state was measured diffusing behind the silent M:=0 branch this replaces. Inert unless network_carry_invariant_; fingerprinted (fresh convergence only). Off = byte-identical.
        bool   network_carry_aware_covariance_{false};     ///< Carry-aware covariance readback (docs/SPEC_carry_aware_covariance.md). foldReducedDdPriors is a per-block rank-K Woodbury update on the pristine per-station factors, i.e. exactly the Schur complement of A~^-1 = N_rr^-1 - SUM_b Y_b M_b Y_b' (Y_b = N_rr^-1 C_b'), and reconstructStation already reads that model back on the mean (the kappa correction). Without this knob the four covariance readbacks use N_rr^-1 while NetworkEstimator's shared covariance (inverted from the folded reduced normals) is carry-aware, so every sigma the producer publishes is a hybrid that is the covariance of no estimator. Armed, jointConditionalCovariance (dd_sigma_cyc, the AR probe's Q_cyc) uses A~^-1 and A~^-1.N_rs instead, including the cross-station local term N_rr^-1 cannot express and a between-station DD needs. Consistent with the fold that ran, not with the true posterior: the fold composes one Woodbury update per block against the pristine N_rr^-1, which is exact only if no station is touched by two blocks, and a block is one connected component of one (const,signal) group, so a station tracking two signals sits in both. The correction therefore inherits the same block-Jacobi truncation the solve made (deliberately: a covariance from a different model than the mean would reintroduce the same defect), and its non-positive-variance guard (refuse, never clamp, always count) is the first measurement of that truncation's size. rowRedundancy deliberately gets no carry_aware flag: it is on the armed RAIM path and the correction would make the detector fire less. Both consumers are read-only (NetworkProducerAr is log-only, Phase 2 read-only), so on must be byte-identical to off on the solve; that is the acceptance gate. Why it matters: med|frac| ~ 0.25 is the expected reading whenever sigma_DD >~ 0.3 cyc, so the conclusion "no DD integer information at this cadence/geometry" (6 h + GPS+Gal) cannot be tested without a valid sigma, and without this knob the only sigma emitted is carry-blind (pinned ~3.9 cyc; scripts/grade_pdfix_arms.py excludes it on purpose). Not fingerprinted (no stored state changes meaning). Inert unless network_carry_invariant_. Off = byte-identical.
        bool   network_phase_datum_eq34_slip_aware_election_{false}; ///< Make the eq34 spanning-tree election slip-aware, so a slipped arc is kept out of the tree wherever the graph allows it. Two effects, both fail-safe: a slipped incumbent is not sticky-seeded (it must re-win as a new edge), and a slipped arc ranks below every clean arc in the I4 longevity order (inert unless network_phase_datum_eq34_longevity_election_). Reason: a slipped tree edge corrupts every fundamental cycle through it, so all dependent chords refloat at once, while a slipped chord costs exactly one DD (its cycle support is 1). Demoting the arc to a chord turns the mass refloat into a single-chord refloat without discarding any information: the DD through the slipped arc still refloats, and its neighbours are not refloated with it. If the arc is the component's only connection, the completion re-admits it and the mass refloat happens anyway, which is correct: the topology, not the policy, forces it. Under network_carry_invariant_ the resulting basis change is a non-event by construction (the driver re-expresses exactly). Off = byte-identical.
        bool   network_emit_clock_frame_hold_{false};      ///< Emit the satellite clocks in a per-constellation held frame (docs/SPEC_clock_emit_rebase.md): the per-epoch common-mode innovation (median vs the previous emitted values, ≥4 fresh unforced keys) accumulates into a frame offset A_c that never re-enters the wire. The removed direction is the per-epoch clock-ensemble gauge (a consumer's per-epoch receiver clock absorbs it; between-sat differences preserved exactly); in offline replay of a 12 h run it removed all 613 census step epochs (max 17.7 m). Outputs-only (solve byte-identical even when on; fingerprint-safe); baked into the product (replay-safe). Off = the leaf is never called (byte-identical).
        double network_clock_frame_leash_m_{50.0};         ///< |A_c| re-anchor bound (m). Crossing it re-anchors the constellation to the solve frame with markDatumChange (the marked re-coupling edge of invariant #7; never silent). Measured 12 h wander on the reference lineage: 23 m. Inert unless network_emit_clock_frame_hold_.
        bool   network_clock_disc_jump_test_{false};       ///< Mark per-satellite common-removed clock steps > network_clock_disc_jump_m_ via the markSatDiscontinuity retro-bump: R record + per-sat phase/code/clock counters, the consumer-reset chain Q4 proved end-to-end. Independent of the frame hold (alone it marks the raw wire with the census's own statistic). Per RTA-11 the emitter's clock counter keeps no internal magnitude test; this test lives at the emission-prep tier and replay reproduces it from the serialized R/E records. Off = byte-identical.
        double network_clock_disc_jump_m_{1.5};            ///< Flat per-sat step threshold (m). Empirical (SPEC clock_emit_rebase §1): 1.5 ≈ 16σ of the measured per-epoch walk step (robust σ 0.092 m) ⇒ ~7 marks/h on the reference internal lineage; the census outlier floor (0.5) would mark 73/h, a burst of consumer resets. Inert unless network_clock_disc_jump_test_.
        double network_clock_disc_gap_jump_m_{0.0};        ///< Step threshold (m) for a return after more than the staleness horizon, tested against the retained pre-gap baseline.  0 = off (a stale return is an untested first sighting, which leaves eclipse re-admissions unmarked).  Covers postures without Q3.  Inert unless network_clock_disc_jump_test_.
        bool   network_clock_temporal_anchor_{false};      ///< The guarded temporal clock anchor (docs/SPEC_clock_temporal_anchor.md). The satellite clock's D2 carry folds at network_clock_anchor_q_ instead of filter.process_noise.sat_clock_q, over a per-satellite elapsed time, with a variance floor, plus the guard (standardized generalized predicted residual, common-mode removed), the in-epoch withdraw+re-solve, the LOOSE/ANCHORED re-validation state machine, and marks at the decoupling edge. Rationale: √(sat_clock_q·Δt) = 17.3 cm is above the realized 9.2 cm walk, so the prior never binds and the emitted clock is per-epoch estimation noise (uniform to 1.37× across 32 satellites, not clock physics) against ~2 cm of physical change. Solve-affecting and fingerprinted. Off = byte-identical (no fold change, no state allocated).
        double network_clock_anchor_q_{1e-4};              ///< The anchor's random-walk rate (m²/s). Steady-state identity: the emitted per-satellite walk is √(q·Δt), so this knob is the walk target (1e-4 → 5.5 cm, 3e-5 → 3.0, 1e-5 → 1.7; the FC-1 physical floor ~2 cm sits at 1.3e-5). Default 1e-4 is the conservative first rung, below the measured binding knee and well above the physical floor. Inert unless network_clock_temporal_anchor_.
        double network_clock_anchor_var_floor_m2_{0.0};    ///< Consistency floor on the folded carry variance (m²): the clock analogue of the B4 pb/cb floors, which exempt the clock because of its own two-state model / monitor, an exemption that does not hold once the carry binds. Caps folded prior information at 1/floor so an independence-assuming (diagonal or dense-diagonal) carry cannot accumulate unbounded information against a time-correlated error floor (assessment deviation #1, ranked-plan item 3). 0 = no floor. Inert unless network_clock_temporal_anchor_.
        double network_clock_anchor_max_gap_s_{120.0};     ///< Per-satellite staleness bound (s). Beyond it the carried clock is not folded at the anchor and the satellite decouples. Load-bearing, not hygiene: the driver's dt is the inter-epoch gap and every skip path (gate withhold, σ̂₀ withhold, solve failure) advances it while the D2 carry survives untouched (an SH gate on a field day withheld 278 consecutive epochs), so without a per-satellite elapsed time a binding prior would fold a 95-minute-old value as if it were 30 s old. Same defect class as the clock model's P0-3/R11 fix. Inert unless network_clock_temporal_anchor_.
        double network_clock_anchor_k_{6.0};               ///< Trip threshold on |w|, the standardized generalized predicted residual ê/√(P⁻−P⁺) with the per-constellation common mode removed. Empirical: derived from the measured prior-residual ladder of the `clk` census family at the armed q, never from the stochastic model. Target trip rate ≈ 1–2 network-wide per hour on a quiet day (in CLK-R, 16σ produced ~7 marks/h and 106 consumer epochs > 20 m). Inert unless network_clock_temporal_anchor_.
        double network_clock_anchor_abs_m_{2.0};           ///< Floor-independent absolute backstop (m) on the common-removed innovation |ê|: trips even if the standardizing variance were mis-set. Empirical band: the 12 h dev-channel p99 is 0.68 m and its max 10.0 m, so 1.5–3 m isolates real steps without touching the body. 0 = off. Inert unless network_clock_temporal_anchor_.
        double network_clock_anchor_sigma_floor_m_{0.02};  ///< Floor on √S (m). A weak-data epoch removes no information (P⁺ → P⁻ ⇒ S → 0) and would otherwise yield a 0/0 statistic that false-trips exactly the lowest-confidence satellites; the floor makes it read w ≈ 0 instead. A lower bound on the denominator, never an addition to it (the addition form, used before B3, double-counts the data). Default 2 cm = the measured physical per-30 s change. Inert unless network_clock_temporal_anchor_.
        int    network_clock_anchor_guard_space_{0};          ///< Q3 guard statistic space: 0 clock, 1 phase_shadow, 2 phase.
        double network_clock_anchor_phase_sigma_floor_m_{0.02};  ///< Floor on the phase-space statistic's sqrt(S) (m).
        int    network_clock_anchor_recouple_epochs_{4};   ///< Consecutive in-band epochs a decoupled satellite must serve before the anchor re-engages: the re-coupling edge must re-validate or re-seed what it reconnects (invariant #7). The re-engaged fold also re-seeds (its first variance is cp.var + q·Δt off the LOOSE posterior, not the steady-state tight value). Inert unless network_clock_temporal_anchor_.
        bool   network_clock_anchor_mark_{true};           ///< Wire-mark the decoupling edge (markSatDiscontinuity → R record + per-sat phase/code/clock counters, the chain Q4 proved end-to-end). A satellite whose clock changed both temporal regime and value must not be integrated across. Unlike CLK-R stage 2, whose marked population was ~98 % churn and cost the quiet-day consumer, this population is by construction not churn (the churn is what the anchor removes), but the rate discipline still applies, and the knob exists so a 12 h A/B can bisect hold vs marks as CLK-R's did. Inert unless network_clock_temporal_anchor_.
        bool   network_clock_anchor_gauge_free_{false};    ///< Fold the clock prior with the per-constellation clock gauge direction marginalized out (Λ ← Λ − Λu(uᵀΛu)⁻¹uᵀΛ; diagonally this is "anchor each clock to its carried value minus the information-weighted mean", i.e. anchor the differences). Applied to the joint dense block, so the clock↔code-bias off-diagonals the C1 carry retains are preserved; only the gauge is removed. A constellation with < 2 carried clocks contributes nothing (its lone key is the gauge). Solve-affecting; part of the anchor's fingerprint. Inert unless network_clock_temporal_anchor_.
        bool   network_clock_level_monitor_{false};      ///< Measure-only level monitor: per satellite the undecayed code-only level information I_L against the stashed block's level variance P_L, the ratio P_L/(1/I_L) traced and read out; solve byte-identical.  Requires shared_carry_full_cov.
        bool   network_clock_level_bound_raise_{false};  ///< Raise the stashed border block along each satellite's level direction to 1/I_L where the monitor ratio is below 1 (a raise, never a set), with the carried variances.  Requires clock_level_monitor.  Solve-affecting, fingerprinted.
        int    network_clock_level_innov_window_ep_{60};  ///< Window (epochs) of the level innovation instrument of the monitor.
        bool   network_phase_visible_monitor_{false};     ///< Measure-only phase-visible consistency instrument (the ionosphere-free phase-visible clock's normalized update and posterior contrast); requires raim_within_epoch.
        int    network_phase_visible_window_ep_{60};      ///< Window (epochs) of the phase-visible instrument's run-end means.
        bool   network_ctu_switch_on_resume_{false};       ///< Admit a warm resume from a snapshot written without the exact update's tokens into a run that arms them (off = fresh convergence only).
        bool   network_clock_level_restore_raise_{false};    ///< Raise the restored border along each satellite's level to its code-only variance at the first leg epoch.
        int    network_clock_level_restore_credit_ep_{0};     ///< Epochs of code information the restored level is credited with; 0 = all.
        int    network_sat_health_{0};                     ///< Broadcast satellite health on the network path: 0 off, 1 "pivot" (the clock-pivot election passes over an unhealthy satellite), 2 "exclude" (an unhealthy satellite's observations are removed at the synced-epoch door). OFF = byte-identical.
        NetworkClockAnchorStat network_clock_anchor_gauge_free_stat_{NetworkClockAnchorStat::Auto};  ///< Bisection knob: which denominator the guard (and the census's clk family) standardizes by, independently of which prior the fold applied. The gauge-free arm moves the fold and the denominator together, so without this knob its 65 → 3 trip collapse cannot be apportioned. `Auto` = follow the fold (byte-identical to runs made before the knob); `Raw`/`Post`/`Proj` force one. Solve-affecting through the trip population; appended to the fingerprint only when it is not `Auto`. Inert unless network_clock_temporal_anchor_.
        bool   network_clock_anchor_resolve_{true};        ///< Adapt-and-emit: on a trip, withdraw the offending clock prior from the shared system and re-solve the epoch before emission, so the trip epoch publishes the data's value instead of the stale anchor's. Cheap: a shared-border information delta carries through the Schur reduction unchanged, so the re-solve is one datum + factorization of the reduced K×K system (no re-reduction, no re-assembly). Deliberately placed before the within-epoch RAIM adaptation: a prior fault must be resolved before the measurement-fault machinery attributes it to a satellite's or a station's data. Off degrades to detect+mark+decouple-next-epoch. Inert unless network_clock_temporal_anchor_.
        double network_phase_datum_pref_elev_deg_{15.0};   ///< Elevation (deg) above which a satellite counts toward a receiver's per-constellation coverage rank when ordering prioritized anchor candidates; does not mask the pin. Inert unless network_phase_datum_prioritized_.
        bool   network_phase_datum_sticky_{true};          ///< Keep the incumbent per-(sat,signal) anchor while it still tracks a usable ambiguity (re-elect only when it sets) to minimize handover disc. Inert unless network_phase_datum_prioritized_.
        double network_phase_datum_handover_max_var_ratio_{0.0}; ///< Handover ceiling relative to the arc's own first-stash variance: a ledger value is usable only if var <= ratio × the arc's first-stash conditional variance (unknown ⇒ refused); the absolute ceiling stays a rail.  Ledger KEEP_OFF.  0 = off.  Inert unless network_phase_datum_handover_continuous_.
        double network_phase_datum_handover_max_var_m2_{1.0}; ///< Conditional-variance ceiling (m^2) a handover ledger value must beat to be pinned; above it the handover falls through to the cold path. Inert unless network_phase_datum_handover_continuous_.
        bool   network_phase_datum_handover_continuous_{false}; ///< Hand an anchor handover over at the successor's own outgoing-frame ambiguity value instead of re-pinning at 0, so the gauge change is an S-transform and the emitted family does not step. Both edges counted + traced. Inert unless network_phase_datum_prioritized_ and not eq34. Off = byte-identical (no ledger allocated).
        /// The phase datum as a held S-basis (RF-2 item 1,
        /// docs/refound/RF-1_steps345_mechanism_2026-09-15.md).  The anchor station's
        /// ambiguity for each pinned (satellite, signal) is the S-basis, frozen at its
        /// value at election for the whole of that anchor's tenure and never re-derived
        /// mid-tenure, so the satellite phase bias is estimable as (PB + that constant)
        /// rather than against a datum that converges, drops and is re-flagged like any
        /// other float state.  On the level vehicle the zero-q anchor provided this
        /// implicitly; under `float_kf` the datum is otherwise a live state (the field's
        /// T4 hit the anchor 454 times an hour).
        /// Requires network.ambiguity_vehicle = "float_kf".  Off = byte-identical.
        bool   network_phase_datum_held_basis_{false};
        /// Minimum arc age (loop epochs since first stash) a candidate must have before it
        /// may hold the S-basis (RF-2 item 3).  A newborn low arc is never used: a leg born
        /// at a satellite's re-admission carries its code-level birth error, and stamping
        /// that in at the gauge weight makes it the network's datum.  Inert unless
        /// network_phase_datum_held_basis_.
        int    network_phase_anchor_min_age_ep_{10};
        /// A candidate flagged by any screen (a slip response, a T4 / T3 refloat, a
        /// gross-LOS declaration) within this many loop epochs is refused the basis (RF-2
        /// item 3).  The datum must be the quietest arc available, not merely an old one.
        /// Inert unless network_phase_datum_held_basis_.
        int    network_phase_anchor_flag_quiet_ep_{5};
        /// Keep the cold / first-sighting families in the phase-anchor reref release under
        /// the held basis instead of clearing the set (RF-2d,
        /// docs/refound/RF-3-read_handover_frame_2026-09-15.md).  Inert unless
        /// network_phase_datum_held_basis_.
        bool   network_phase_datum_held_cold_release_{false};
        /// Declare a held-basis handover to the consumer iff it is cold, not on
        /// |value − prev_value| (RF-2d).  Inert unless network_phase_datum_held_basis_.
        bool   network_phase_datum_held_mark_cold_{false};
        bool   network_phase_datum_candidacy_usable_{false}; ///< Cold anchor fallback (SPEC_event_harness §15; ledger KEEP_OFF): when a handover is about to happen, sweep the coverage-ranked candidates for one whose carried value is usable by the continuous branch's own predicate (finite, 0 < var <= ceiling, ratio gate, not freed this epoch, arc age >= network_phase_datum_candidacy_min_age_ep_) before falling through to the take-the-first-tracker sweeps.  A family with no usable candidate keeps the cold path.  Inert unless network_phase_datum_handover_continuous_ (no ledger to screen against).  Off = byte-identical (the pass is never entered).
        bool   network_phase_datum_candidacy_satellite_{true}; ///< Cold anchor fallback, iteration 2 (SPEC_event_harness §15.8): resolve a satellite's handover jointly; every family of the satellite moving this epoch takes one successor (the first coverage-ranked candidate usable for the most of them; a family it is not usable for is pinned there through the cold path, which is a frame step, not a split), and a partial handover prefers the station already anchoring the satellite's other families.  Iteration 1's per-family screen gained producer quality at the cost of split references (0.0 → 0.9/3) and a 49 % worse consumer.  Default true; inert unless network_phase_datum_candidacy_usable_; off = exactly iteration 1 (the single-knob A/B).
        int    network_phase_datum_candidacy_min_age_ep_{60}; ///< Cold anchor fallback maturity bar in loop epochs (n_loop_epochs − LocalCarry::birth_ep); 0 = no age test, unknown age = mature (the restored-entry convention: a warm-resume-restored entry carries LocalCarry::kBirthEpRestored, SPEC_event_harness §15.4).  Sized on the §15 census: cold successors p50 39 ep vs continuous p50 236 ep.  Inert unless network_phase_datum_candidacy_usable_.
        bool   network_phase_anchor_carry_reref_{false};   ///< At a PHASE_ANCHOR handover, marginalize the (sat,signal) family's shared PB carry for one epoch (the eq34_reref treatment) and erase its phase-leveling local carries at every station (QUEUE item 1(d), SPEC_pivot_visibility_gauge_defect §5.07.5 R1): the retired-frame prior is not folded against the re-realized design matrix, so the frame step lands on the free phase bias instead of being absorbed by the satellite clock (measured: 41-sigma clock innovations). Inert unless network_phase_datum_prioritized_ and not eq34. Off = byte-identical (the set is never populated).
        bool   network_phase_datum_gate_drop_erase_{false};///< At the coverage gate's drop edge (a key excluded this epoch that had an anchor incumbent last epoch), add the family to the epoch's re-reference set, so every station's carried ambiguity of it is erased before assembly, as at the family's return edge. Without it, on the float vehicle the surviving station's carried float absorbs the whole removed bias in one epoch against a centimetre prior (+15.829 m measured) and steps the satellite clocks. Inert without network_phase_anchor_carry_reref_ and without the gate (min_stations > 1). Off = byte-identical (edges detected + counted only).
        int    network_phase_datum_min_stations_{1};       ///< Coverage-threshold gate on the prioritized anchored union: register/emit a shared per-(sat,signal) phase-bias column only when >= this many present stations track it with a usable ambiguity (1 = no gating, byte-identical; 2-3 excludes the thin columns behind one-station-pool re-levels and part of the clock↔PB split leak; gated keys' phase rows still serve the clock via their local ambiguities). Inert unless network_phase_datum_prioritized_.
        bool   network_cb_kaula_prior_{false};             ///< Fold a loose physical-bound prior (value 0, info 1/sigma^2) onto every admitted satellite code-bias column (network.cb_kaula_prior), lifting a code-dominated near-null direction the phase drop-and-heal structurally cannot reach (it anchors PB columns only). Real prior information (satellite OSBs are bounded at tens of ns), not an ad hoc regularizer. Folded through the diagonal SharedPrior channel, not into the tight kCodeBiasGaugeWeight datum rows, so declared_dimension is unchanged by construction. Off = byte-identical; inert unless estimate_sat_code_bias.
        double network_cb_kaula_sigma_m_{10.0};            ///< Sigma (m) of the admitted-OSB prior; 10 m ~ 33 ns, the loose end of the physical bound. K2 anti-fragility rule: 1/sigma^2 must sit >= 2 decades above the armed rank-gate cutoff (an earlier loose prior that sat at the cutoff was rejected); the producer computes and logs that margin once and warns below 2 decades. Inert unless network_cb_kaula_prior_.
        double network_rank_gate_tol_{1e-9};               ///< Relative eigenvalue tolerance of the shared-border null-space rank gate (network.rank_gate_tol), resolved once into NetworkEstimator::setRankGateTol and read from there by every consumer (the gate, the estimability dump, the prioritized drop-and-heal, the S-basis completion and the margin logs). Default 1e-9 == NetworkEstimator::kNullSpaceTol ⇒ byte-identical. A calibration parameter, not a constant: "null" is a placement decision in a spectral gap that moves with the constellation mix (GPS+Gal required moving it from 1e-6 to 1e-9; BeiDou's weak but real layer sits ~3 decades deeper, relatively). Set per config and justified from the dump: the gate is exact-equality/both-direction, so a value below a config's own gauge depth withholds a valid config ("over-specified"). See the getter for the binding placement rule.
        bool   network_sbasis_complete_{false};            ///< Derive the epoch's realized S-basis dimension before the gate and fold one minimum-constraint eigenvector gauge row (RHS = the combination's carried level) per near-null direction beyond the declared structural dimension (SBC, network.sbasis_complete): the phase-bias-datum fix that scales with the constellation set, for GPS+Gal epochs that observe 4 near-null directions against 2 declared and are withheld or drive bursts of PA drop-and-heal events (per-satellite channel-family PB↔iono trades the anchor pins cannot close). Engage/release events (invariant #7), every refusal counted. Off = byte-identical; the gate/PA path is untouched.
        int    network_sbasis_complete_max_{6};            ///< Per-epoch cap on completed directions (SBC); beyond it the completion refuses (counted) and the epoch withholds: a burst that size indicates a pathological epoch or a misplaced rank_gate_tol, not a datum to complete automatically. Measured need 1-3; <=0 = uncapped (diagnostic only). Inert unless network_sbasis_complete_.
        bool   network_station_pd_pin_aware_{false};       ///< Pin-aware station PD guard: the addStation local-block rank test references the median |LDLᵀ pivot| instead of the max, so the prioritized datum's in-block 1e10 anchor pins do not inflate the rejection bar by ~10 orders and eject valid marginal stations every epoch. Off = legacy max-pivot test (byte-identical).
        bool   network_emit_generation_id_{false};         ///< Stamp a solution-generation id (uint32 = first-emitted epoch's week·604800+⌊tow⌋) on every epoch (RTA-3; optional I record + store field + S-line gid=); a consumer treats a change as a global datum break; off = no I record, store generation unset (byte-identical).
        bool   network_serialize_disc_counters_{false};    ///< Append the post-emit discontinuity-counter values as optional trailing fields (RTA-4; C field 5 clock / B field 5 code / P field 7 phase) so a tail reader reconstructs counters without replaying from epoch 1; off = old field counts (byte-identical).
        double network_contract_rebroadcast_s_{0.0};       ///< Re-broadcast a current v2 'S' contract line (refreshed realized pivots) every N seconds of product time and on any datum-change emit (RTA-5); the S contract is always written; 0 = single header S line (byte-identical).
        double network_emitter_staleness_horizon_s_{0.0};  ///< Per-key last-emitted product-time horizon (s; RTA-11); a (sat[,signal]) key returning after a longer gap is a first sighting (baseline reseed, no jump/forced disc bump) on phase/code/clock channels; travels in the S contract as stale=; 0 = keep-forever baselines (byte-identical).
        bool   network_service_mode_{false};               ///< Curated RT-service profile switch (RTB-8, network.service_mode): the loader arms the coherent producer toggle set (the bundle wins over explicit disables, each override warned; see applyNetworkServiceProfile_, called from loadNetworkSection_).  The member is kept only for provenance (getter + L15 run manifest); off = nothing armed (byte-identical).
        double network_clock_model_meas_std_m_{0.05};    ///< Std (m) of the network-solved offset as the two-state model's measurement (the update R).
        double network_clock_model_max_gap_s_{120.0};    ///< Maximum unseen gap (s) the carried two-state clock model survives before reseed / drift-reset / C1-ZOH (R11); the loader defaults it to network_phase_leveling_max_gap_s_ (clock-model carry only, distinct from the ambiguity/ZWD max-gap).
        // --- product generation-time metadata (RT0-4) ---
        bool   network_emit_generation_time_{false}; ///< Stamp a wall-clock time of generation per emitted epoch (RT0-4; optional G record) so a consumer can measure product latency; off = no G record (byte-identical). Wall clock ⇒ non-deterministic when on.
        // --- per-constellation clock-runaway quarantine (RT0-5) ---
        bool   network_clock_runaway_check_{false};  ///< Drop a constellation from the epoch's emission when its median clock magnitude/rate exceeds the ceilings below (RT0-5; a runaway degrades scope, not the whole product); off = every constellation emitted (byte-identical).
        double network_clock_runaway_mag_m_{1000.0}; ///< Per-constellation median |clock − global median| ceiling (m; RT0-5). Inert unless network_clock_runaway_check_.
        double network_clock_runaway_rate_m_s_{10.0};///< Per-constellation median |clock drift| ceiling (m/s; RT0-5). Inert unless network_clock_runaway_check_ and the two-state clock model emits a drift.
        // --- URA quality channel (B1) ---
        bool   network_emit_ura_{false};            ///< Emit a per-sat URA quality channel (B1; EWMA(σ̂0)×formal clock σ); off = product byte-identical.
        double network_ura_ewma_alpha_{0.1};        ///< EWMA α for the network-wide σ̂0 that scales the emitted URA (B1).
        bool   network_consume_ura_{false};         ///< Consumer inflates measurement R by the network URA on corrected rows (B1); off = R byte-identical.
        double network_ura_staleness_q2_{0.0};      ///< Consumer ZOH clock-staleness q² (m²/s³; B1) folded as q²·dt³/3 into the applied clock variance; 0 = off.  The loader defaults it to network_clock_model_q2_ (P1-3) so it is active whenever consume_ura is on.
        bool   network_mark_refsat_reset_disc_{false};  ///< Mark a per-sat discontinuity (R record) on a ref-path slip (Edge-1 piece 2); off = byte-identical.
        bool   network_ura_retro_bump_{false};      ///< On a post-emit QC flag, retro-bump the flagged satellite's disc counter on the next emit (B1; the consumer resets the ambiguity that integrated the contaminated epoch); off = no bump.
        double network_ura_refloat_inflate_{1.0};   ///< URA multiplier applied to a flagged sat for the phase-leveling warmup horizon after any refloat (B1); 1 = off.
        // --- phase-bias σ channel (B1b; Tienstra product covariance, phase-bias block) ---
        bool   network_emit_phase_bias_sigma_{false};    ///< Producer emits per-(sat,signal) phase-bias formal σ (B1b; EWMA(σ̂0)×√(shared-cov diag), gauge-exact) into SsrPhaseBiasEntry.sigma_m, serialized as the P record's sigma= token; off = σ 0, byte-identical.
        bool   network_consume_phase_bias_sigma_{false}; ///< Consumer inflates R on a phase row by the network phase-bias σ² (B1b); off = R byte-identical.
        bool   network_emit_combined_sigma_{false};      ///< Producer emits per-key combined-correction σ (sigc phase, sigcc code) + code/phase corr (rho) from the shared-cov cross-terms → sigc=/sigcc=/rho= wire tokens (M4, contract v2); off = every value 0, no token, byte-identical.
        double network_combined_sigma_agek_{0.0};        ///< Age-scaling coefficient advertised on the S-line (M4, contract v2; agek=); the consumer inflates combined variance as σc²·(1+agek·Δt_age²). 0 = no token, no age inflation (byte-identical).
        bool   network_require_signal_integer_for_ar_{false}; ///< Consumer marks an ambiguity unresolvable (AR-ineligible) when its covering network phase bias declares signal_integer=false (T3a; e.g. a GLONASS phase bias, whose FDMA bias is not integer-preserving). Off = the coverage-only gate (byte-identical), under which no consumer reads signal_integer.
        double network_start_offset_hours_{0.0};        ///< Testing fast-forward: skip epochs until this many hours of data have elapsed from the first synced epoch, then cold-start (0 = process from the start).
        bool enable_cio_frame_{false};                    ///< Use the CIO IAU 2000A/2006 ECI↔ECEF transform (SOFA) for Sun/Moon; legacy equinox chain when off.
        bool enable_sun_frame_correction_{false};         ///< De-precess the of-date Sun/Moon ECI vectors to J2000 before the ECI→ECEF rotation, cancelling the inherited RTKLIB precession double-count (20.41' in 2024, the precession accumulated since J2000). Default off: fixing it moves solid tides, satellite attitude/PCO, wind-up and the eclipse gate on every existing lineage.
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
        bool estimate_sat_phase_biases_{false};                  ///< Estimate explicit per-(sat,signal) phase-bias states (PPP_RTK_Network).
        bool estimate_sat_clocks_{false};                        ///< Estimate explicit per-satellite clock states, orbit held fixed (PPP_RTK_Network).
        bool promote_sat_code_bias_{false};                      ///< Estimate satellite code bias on every code row, not fallback-only (PPP_RTK_Network).
        double sat_bias_cache_warm_age_s_;                       ///< Sat-bias cache: warm-restore window (sec).  Within this, restore value + aged variance.
        double sat_bias_cache_max_age_s_;                       ///< Sat-bias cache: max entry lifetime (sec).  Beyond this, evict; in [warm, max], restore value with cold-start variance.
        double no_bia_code_downweight_;                            ///< Variance multiplier for code on signals without BIA coverage.
        double code_bias_reinflate_std_{0.5};                      ///< Std (m) the rx code-bias state is re-inflated to on a consumed sat code-bias datum discontinuity (H1).
        double clock_disc_isb_reinflate_std_{0.0};                 ///< Std (m) the non-reference-constellation ISB state is re-inflated to on a consumed sat clock datum discontinuity; 0 (default) = note-only clock-disc handler (byte-identical).
        bool estimate_velocity_;                           ///< Estimate receiver velocity from Doppler observations.
        bool enable_iono_rate_{false};                     ///< Estimate a per-sat slant-iono rate (constant-velocity [I,Idot] block) driven by process_noise.iono_rate_psd (PPP-RTK uncombined).
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
        double code_elevation_power_;  ///< σ-power on the elevation amplification for code observations: σ = σ_zen / sin^p(elev).  p=1.0 reproduces the legacy 1/sin²(elev) variance scaling.  See @c getCodeElevationPower().
        double phase_elevation_power_; ///< σ-power for phase: default 1.0 gives the symmetric 1/sin²(elev) variance scaling (matching code); 0.5 selects the gentler 1/sin(elev).  See @c getPhaseElevationPower().
        SnrModelParams snr_model_params_;                                ///< SNR-based model parameters.
        SatBlockNoiseParams sat_block_noise_params_;                      ///< Satellite-block stochastic model parameters.
        ImuConfig imu_config_;                                           ///< IMU integration configuration.
        DownloadConfig download_config_;                                  ///< Data download and caching configuration.
        DataSource data_source_;                  ///< Selects internal-data vs production-download mode.
        std::filesystem::path internal_data_root_;                        ///< Absolute path to the internal data tree (resolved at load time).
        AppMode app_mode_;                    ///< Top-level execution mode.
        RealTimeConfig realtime_config_;                                  ///< Real-time streaming configuration.
        NetworkConsumerConfig network_consumer_config_;                   ///< processing.network_consumer profile (RTA-7; standalone network-product consumer; enable defaults false).
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
        bool   ar_ignore_mw_wl_{false}; ///< processing.ar_ignore_mw_wl (RTA-7): drop the MW WL feed into AR (pure MLAMBDA on the KF float); the config key for the RealTimeEngine::setArIgnoreMwWl seam.
        bool   ar_wl_only_ffr_{ true };      ///< Apply the bootstrap FFR gate to the WL-only / MW-WL constraint paths (without it they have only the wl_max_frac point test). Default on; false reproduces the fail-unsafe behaviour without the gate.
        int    ar_wl_only_min_arc_count_{ 10 };  ///< Minimum MW arc sample count for an arc to carry a WL constraint under ar_wl_only_ffr_ (a 2-sample arc reports sigma~0 on one degree of freedom).
        double ar_wl_only_max_innov_cyc_{ 0.5 };  ///< Max |asserted MW WL integer - predicted DD widelane| (cycles) for a WL-only constraint. The analogue of nl_max_per_pair_frac for this path; catches a frame conflict, which no success-rate gate can. 0 disables.
        double ar_wl_only_pseudo_var_; ///< Pseudo-obs variance (cycles²) for WL-only constraints.
        double ar_wl_max_frac_;        ///< Max worst-case fractional residual (|frac|+3σ_mean/λ, cyc) for an MW WL to enter AR.
        double ar_min_success_rate_;  ///< Min bootstrap success rate (FFR-style) for AR fix acceptance.
        double ar_min_arc_info_{ 0.0 };   ///< Per-candidate AR maturity gate: minimum accumulated sin^2(el) on both arcs of a candidate. 0 = off (byte-identical). Makes AR safe, not useful; see AmbiguityResolverConfig::min_arc_info.
        double ar_nl_max_per_pair_frac_; ///< Max per-NL-pair |float − fixed| (cyc); rejects marginal fixes.
        bool   ar_nl_iono_free_row_{ false }; ///< WL-constrained N_1 search runs on the exact iono-free row instead of the narrowlane row. Default off (byte-identical); see AmbiguityResolverConfig::nl_iono_free_row for the 8.06× WL-sensitivity trade.
        bool   ar_enable_partial_ar_;    ///< Partial AR: on full-set fix failure, retry on the largest gate-passing subset.
        std::vector<std::string> ar_active_systems_; ///< Constellations eligible for AR (empty = all).

        LoggingConfig logging_config_;             ///< Logger and trace file enable/disable flags.
    };

} // namespace GnssProcessor
