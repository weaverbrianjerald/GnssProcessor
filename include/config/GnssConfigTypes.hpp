#pragma once

/**
 * @file GnssConfigTypes.hpp
 * @brief Configuration vocabulary for GnssConfig: the enums and POD
 *        config sub-structs consumed by GnssConfig and its callers.
 *
 * Extracted from GnssConfig.hpp so the 2.8k-line config header no longer
 * forces every includer to re-parse the full type vocabulary, and so a
 * caller that only needs a config enum/struct can depend on this lighter
 * header. GnssConfig.hpp includes this file. All types remain in namespace
 * GnssProcessor, exactly as before — this is a pure relocation.
 */

#include "logging/LogManager.hpp"   // LogLevel (used by LoggingConfig)
#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

    /**
     * @brief Enumeration for the top-level application execution mode.
     *
     * Selects between batch post-processing from files and real-time
     * streaming operation.
     */
    enum class AppMode {
        PostProcessing, ///< Batch processing from RINEX / product files.
        RealTime        ///< Streaming / live processing from RTCM data.
    };

    /**
     * @brief Selects how input files (RINEX observations, navigation,
     *        SP3/CLK/BIA/ERP, ATX, ocean loading) are located.
     *
     * - Internal: self-sufficient mode. PathResolver discovers files by
     *   globbing the configured @c internal_data_root (defaults to
     *   @c <project_root>/data). Suitable for unit tests, demos, and
     *   any run where the user wants a turnkey experience without a
     *   network connection. Forces @c DownloadConfig::enabled = false.
     * - Production: PathResolver delegates to @c ProductResolver, which
     *   computes IGS long-name filenames from year/doy/station and
     *   downloads any missing products from CDDIS into
     *   @c DownloadConfig::data_root.  Used for real PPP / PPP-RTK runs
     *   on arbitrary stations and dates.
     *
     * The two modes share the same on-disk layout (orbit/, clock/,
     * dcb/, erp/, brdc/, rinex/, atx/, nc/) so swapping data sources is
     * a one-line config change.
     */
    enum class DataSource {
        Internal,   ///< Use files under @c internal_data_root only.
        Production  ///< Auto-download from CDDIS into @c data_root.
    };

    /**
     * @brief Configuration for one station in a baseline / network processing run.
     *
     * Each station supplies its own RINEX observation file.  Reference
     * stations additionally require a known ECEF position (fixed during
     * the solve).  Rovers may optionally supply an a-priori position to
     * seed the filter; otherwise the position is initialized from the
     * RINEX header approximate position.
     */
    struct StationConfig {
        std::string name;                         ///< Short station identifier (e.g. "REF1", "ROV1").
        std::filesystem::path rinex_path;         ///< Path to the station's observation RINEX file.
        std::array<double, 3> position_ecef{ 0.0, 0.0, 0.0 }; ///< A-priori (or fixed) ECEF position (m).
        bool has_position{ false };               ///< True if @c position_ecef was supplied in the config.
        bool fixed{ false };                      ///< True when the station position is held fixed (reference station).
    };

    /**
     * @brief Configuration for double-differenced baseline / network processing.
     *
     * Used when @c ProcessingMode is @c BaselineSingle or @c BaselineMulti.
     * A single reference station is required in both cases.  For
     * @c BaselineSingle exactly one rover is expected; for @c BaselineMulti
     * one or more rovers may be listed — each is double-differenced
     * independently against the master reference (star topology, v1).
     */
    struct BaselineConfig {
        StationConfig reference;              ///< Master reference station (fixed coordinates).
        std::vector<StationConfig> rovers;    ///< Rover stations (one for Single, >=1 for Multi).
        double max_baseline_km{ 30.0 };       ///< Soft upper bound for the short-baseline atmosphere assumption.
        double epoch_sync_tolerance_s{ 0.01 };///< Max time-tag difference when matching epochs across receivers.
        bool code_only{ false };              ///< Debug toggle: skip phase DD updates (filter uses only code DDs, no ambiguity states).
        bool use_doppler{ false };            ///< Include Doppler ('D') observables when forming DDs.  Off by default because BaselineFilter has no velocity state yet — Doppler DDs are computed but not consumed.  Set true to surface Doppler residual statistics for diagnostics or when a velocity state is added in a future slice.
        bool signal_aliasing{ true };         ///< Pair signals across receivers at the band level when tracking attributes differ (e.g. rover C2W vs reference C2C, both GPS L2).  When false, only exact RINEX-3 string matches form DD rows (legacy strict behavior).  Default ON because mixed-receiver fleets are common and the strict path silently drops viable measurements.  Sat-DCB residual on cross-attribute code rows is small (typically <1 m) and is absorbed by the float ambiguity for phase rows.  Phase 2 plan adds DCB harmonization for ambiguity fixing.
        bool use_iono_free{ false };          ///< Use the dual-frequency ionosphere-free combination instead of per-band uncombined DDs.  Long-baseline (>10-20 km) DDs do NOT cancel ionosphere between receivers; the IF combination eliminates the first-order term at the cost of ~3x amplified code noise and a non-integer ambiguity (float-only — AR is automatically excluded for IF rows).  On short baselines IF is unnecessary and noisier than uncombined; the toggle exists as a testing / educational knob.  Per sat-pair and obs-type the row builder picks the two highest-frequency bands available at both rover and reference and collapses them into a single IF row.  Sat-pairs with fewer than two shared bands are dropped in IF mode.
        bool enable_pivot_handover{ true };   ///< When the per-constellation sticky pivot changes (incumbent left the shared-sat set), apply an S-transformation to re-reference the existing DD ambiguities to the new pivot instead of erasing and re-seeding them.  Preserves integer cycle information across pivot changes — the user-visible symptom of NOT doing this is a position discontinuity at every pivot handover, since the freshly-seeded float ambiguities have to absorb the rover's geometry residual all over again.  The transform is x_new = T·x_old, P_new = T·P_old·Tᵀ, where T is the standard DD S-transform that maps (DD relative to old pivot) to (DD relative to new pivot).  Default true; set false to revert to the legacy reset-on-handover behavior for testing or comparison.
        bool estimate_differential_tropo{ false };  ///< Estimate a single differential zenith wet delay (Δzwd ≡ ZWD_rov − ZWD_ref) as a state, mapped to slant via the wet mapping function.  PppModel applies only the Saastamoinen ZHD a-priori; the wet delay is otherwise unmodeled and on short baselines (< ~10 km) it cancels in BRSD because both stations see nearly identical wet path delays.  On longer baselines (especially with altitude differences) Δzwd can reach several centimeters and the residual smears into vertical position and float ambiguities.  Set true with use_iono_free for the typical long-baseline configuration.  Per phase DD row the H column entry is (mf_w_rov_i − mf_w_rov_p); short-baseline approximation assumes mapping functions match between rover and reference.  Single state (one Δzwd, one process-noise channel); not adequate for >100 km baselines where rover and reference mapping functions diverge.
        double differential_tropo_init_std_m{ 0.1 };  ///< Initial standard deviation of the differential ZWD state (m).  Default covers ~1 cm/10 km horizontal × 30 km plus modest altitude-difference contribution.  Used only when @c estimate_differential_tropo is true.
        double differential_tropo_process_noise_m_per_sqrt_s{ 1e-4 };  ///< Random-walk process noise spectral density (m/√s) for the differential ZWD state.  Default 1e-4 ≈ 6 mm/√h, a typical published value for short-to-medium-baseline differential troposphere drift.  Squared to a per-second variance rate inside @c getProcessNoiseMatrix.  Used only when @c estimate_differential_tropo is true.
    };

    /**
     * @brief Configuration for real-time streaming mode.
     *
     * When AppMode::RealTime is selected these settings control the NTRIP
     * caster connection, observation source and output behaviour.
     */
    struct RealTimeConfig {
        std::string ntrip_host;                   ///< NTRIP caster hostname.
        int         ntrip_port{ 2101 };           ///< NTRIP caster port.
        std::string ntrip_mountpoint;             ///< NTRIP mountpoint name.
        std::string ntrip_user;                   ///< NTRIP username (empty = anonymous).
        std::string ntrip_password;               ///< NTRIP password.
        std::string nmea_gga;                     ///< Optional GGA sentence for VRS.
        int         reconnect_delay_s{ 5 };       ///< Seconds before reconnect on failure.
        std::string input_type{ "ntrip" };        ///< Transport for the primary stream: "ntrip", "tcp", "file".
        std::string input_format{ "rtcm" };       ///< Wire format of the primary stream: "rtcm" or "sbf" (Septentrio Binary Format).
        std::string input_path;                   ///< Path for "file" input.
        std::string tcp_host;                     ///< Host for "tcp" input (e.g. receiver IP).
        int         tcp_port{ 28784 };            ///< Port for "tcp" input.
        std::string output_type{ "console" };     ///< Output destination: "console", "file", "nmea".
        std::string output_path;                  ///< Path for file-based output.
        size_t      max_epoch_buffer{ 100 };      ///< Max epochs to buffer before dropping oldest.

        /**
         * @brief Optional second RTCM stream carrying SSR corrections.
         *
         * Independent of the primary stream above: the primary feeds rover
         * observations into the engine; this feeds RTCM SSR (1057-1068,
         * 1240-1270, eventually 4076) into the SsrCorrectionStore.  Both
         * streams may be active concurrently — the realtime loop polls
         * each with a short timeout and dispatches frames to the engine.
         *
         * Disabled by default; setting @c enabled=true selects the
         * stream type via @c input_type ("ntrip" or "file").
         */
        struct Corrections {
            bool        enabled{ false };
            std::string input_type{ "ntrip" };    ///< "ntrip", "tcp", or "file" (replay).
            std::string ntrip_host;
            int         ntrip_port{ 2101 };
            std::string ntrip_mountpoint;         ///< e.g. "IGS01" on products.igs-ip.net
            std::string ntrip_user;
            std::string ntrip_password;
            std::string nmea_gga;
            int         reconnect_delay_s{ 5 };
            std::string input_path;               ///< Path for "file" input_type.
            std::string tcp_host;                 ///< Host for "tcp" input.
            int         tcp_port{ 28784 };        ///< Port for "tcp" input.
        };
        Corrections corrections;                   ///< Optional SSR corrections stream.

        /**
         * @brief Optional third RTCM stream carrying broadcast ephemeris (nav).
         *
         * For an observation-only station mountpoint — one that streams MSM
         * obs but no broadcast nav (1019/1020/1042/1044/1046) — this supplies
         * the broadcast orbits/clocks that the SSR overlay corrects.  Without
         * it, @c LiveEphemerisStore stays empty for the obs constellations and
         * every satellite is unpositionable, so @c PppModel::computeModels
         * fails for the whole epoch ("No valid PPP observations modeled") and
         * the session yields 0 epochs.  Point this at a dedicated broadcast-
         * ephemeris caster mountpoint (e.g. @c BCEP00BKG0 on
         * products.igs-ip.net).  Frames route to
         * @c RealTimeEngine::feedRtcmFrame exactly like the corrections stream,
         * so decoded nav lands in the live store transparently.
         *
         * Independent of @c corrections: a PPP-RTK run off an obs-only station
         * needs all three sources (obs / nav / SSR).  Disabled by default — a
         * station that bundles its own nav (e.g. BRUX00BEL0) needs only the
         * primary + corrections streams.  Reuses the @c Corrections transport
         * descriptor (host/port/mountpoint/file/tcp); the two streams differ
         * only in payload (SSR vs broadcast nav), not transport.
         */
        using Ephemeris = Corrections;
        Ephemeris ephemeris;                       ///< Optional broadcast-ephemeris (nav) stream.
    };

    /**
     * @brief Enumeration for GNSS processing modes.
     *
     * Defines the available processing modes for the GNSS processor.
     */
    enum class ProcessingMode {
        PPP,            ///< Precise Point Positioning (single receiver, external products).
        PPP_RTK,        ///< PPP with RTK enhancements (single receiver, uncombined).
        BaselineSingle, ///< Double-differenced single baseline: one rover + one reference station.
        BaselineMulti   ///< Double-differenced multi-receiver network (star topology: N rovers against one reference).
    };

    /**
     * @brief Frequency bands the Kalman filter estimates.
     *
     * Renamed from @c SignalMode — "frequency" reflects what this enum
     * actually controls (band scope), leaving "signal" to mean the full
     * (band, tracking-attribute) tuple.  Pair this with @c ChannelMode
     * to express the full KF active-signal set.
     */
    enum class FrequencyMode {
        Dual,   ///< Two bands per constellation (e.g. GPS L1+L2).
        Triple, ///< Three bands per constellation (e.g. GPS L1+L2+L5).
        All,    ///< Every band a constellation publishes.
        Independent ///< Like @c All, but drops a constellation's E5/B2 *sidebands*
                    ///< (Galileo E5a+E5b, BeiDou B2a+B2b — RINEX bands 5 & 7) for
                    ///< any satellite that also reports the coherent **wideband**
                    ///< (RINEX band 8: Galileo E5 alt-BOC, BeiDou B2(a+b)).  The
                    ///< wideband is the joint coherent processing of those same
                    ///< sidebands, so feeding all three to the diagonal-R filter
                    ///< double-counts the band as if independent — inflating its
                    ///< weight and letting one signal's bias drag the shared
                    ///< (iono/clock/tropo/position) states.  Pruning keeps only
                    ///< the independent information.  Decided per-satellite from
                    ///< the observed signals: when the wideband is absent the
                    ///< sidebands are retained (graceful fallback — no band lost).
    };

    /**
     * @brief Tracking-mode channel selection within an active band.
     *
     * @c Single picks one canonical tracking attribute per band via
     * @c signalAttributePriority (e.g. GPS L1 → L1W); @c All keeps every
     * tracked channel as an independent ambiguity state (e.g. L1W and
     * L1C both get their own ambiguity).  State-vector size scales with
     * the channel count, but the redundancy aids cross-consistency.
     */
    enum class ChannelMode {
        Single, ///< Canonical tracking attribute per band (default).
        All     ///< Every tracking-mode channel present in the band.
    };

    /**
     * @brief MW slip detector frequency-band scope.
     *
     * Independent of the KF's @c FrequencyMode so the detector can be
     * tuned for cross-consistency without inflating the state vector.
     * @c Mirror inherits the KF's @c FrequencyMode; the other values
     * override it.
     */
    enum class MwFrequencyMode {
        Mirror, ///< Match the KF's @c FrequencyMode (default).
        Dual,   ///< Override: two bands.
        Triple, ///< Override: three bands.
        All     ///< Override: every available band (from @c raw_observed).
    };

    /**
     * @brief MW slip detector channel-multiplicity scope.
     *
     * Independent of the KF's @c ChannelMode.  @c Mirror inherits the
     * KF setting; @c Single or @c All overrides it.  A common
     * combination is KF @c Single + MW @c All — lean state vector but
     * every tracked channel feeds slip cross-checks.
     */
    enum class MwChannelMode {
        Mirror, ///< Match the KF's @c ChannelMode (default).
        Single, ///< Override: canonical channel only.
        All     ///< Override: every tracked channel.
    };

    /**
     * @brief Carrier-phase ambiguity parameterization in the Kalman state.
     *
     * Selects how phase ambiguities are stored in the state vector — and,
     * by extension, how the measurement Jacobian is built and how
     * @c AmbiguityResolver / @c MwSlipDetector interact with the float
     * solution.
     *
     *  - @c PerBand : one ambiguity per (satellite, signal/IF combo), the
     *    historical layout.  Key format @c "<sat>_<sig>" (e.g. @c "G01_L1C",
     *    @c "G01_L1C_L5Q").  The MW detector resolves WL integers
     *    independently and feeds them to AmbiguityResolver as a
     *    pseudo-observation map.
     *
     *  - @c NlWl : one narrow-lane state per (sat, band) plus one
     *    wide-lane state per (sat, band-pair).  Key formats
     *    @c "<sat>_NL_<band>" and @c "<sat>_WL_<band_hi>_<band_lo>"
     *    (e.g. @c "G01_NL_1", @c "G01_WL_1_2").  WL slip-detection
     *    output writes directly into the WL state covariance, and
     *    LAMBDA can search the NL sub-vector with WL pre-constrained.
     *    Halves the integer search dimension and exposes the full WL
     *    information to the filter rather than only as an
     *    out-of-band hold update.
     *
     * Default is @c PerBand to preserve existing behavior; switch via
     * @c estimation.state_ambiguity_basis.
     */
    enum class StateAmbiguityBasis {
        PerBand, ///< One ambiguity per (sat, signal) — historical layout.
        NlWl     ///< Narrow-lane + wide-lane state pair per (sat, band-pair).
    };

    /**
     * @brief Enumeration for satellite velocity interpolation.
     *
     * Specifies the type of interpolation used for satellite velocity computation.
     */
    enum class SatelliteInterpolationMode {
        Lagrange,///< Lagrange analytical derivative.
        Finite,  ///< Finite central difference.
        Hermite, ///< Monotone Hermite slopes.
        Spline   ///< Cubic spline.
    };

    /**
     * @brief Enumeration for satellite clock interpolation.
     *
     * Specifies the type of interpolation used for satellite clock computation.
     */
    enum class SatelliteClockInterpolationMode {
        Linear,         ///< 2-node bracket linear interpolation (MMSE-optimal for pure Brownian motion with noise-free samples).
        Lagrange,       ///< Bracket-centered polynomial Lagrange (overfits noisy random-walk clocks; not recommended for 30 s IGS Final).
        LsqSatClock     ///< Weighted linear LSQ over multi-node window, anchored by explicit clock-rate observations at the
                        ///< coarse-cadence epochs (typ. every 5 min in IGS Final).  Reduces observation-noise component by
                        ///< ~sqrt(N/2) vs Linear at the cost of mm-level RW bias and discontinuity at clock epochs (window
                        ///< slides → LSQ refit).  Degrades gracefully to Linear when too few nodes are available.
    };

    /**
     * @brief Source of a priori pressure and temperature used by the
     *        Saastamoinen zenith hydrostatic delay.
     */
    enum class TroposphereAprioriSource {
        StandardAtmosphere, ///< Frozen sea-level @c receiver.zhd_pressure_mbar / @c zhd_temp_kelvin reduced by standard lapse.
        Gpt3                ///< Lookup pressure & temperature from the GPT3 5° grid at the receiver lat/lon/h/doy (Landskron & Boehm 2018).
    };

    /**
     * @brief Tropospheric mapping function used to map zenith to slant
     *        delays.
     *
     * @c Vmf3 supplies @c a_h/@c a_w from the GPT3 5° grid (empirical
     * climatology, no operational 6-hourly product chain) combined with
     * the published degree/order-12 spherical-harmonic expansion of
     * @c b/c (Landskron & Boehm 2018).  GPT3 must be loaded — selecting
     * @c Vmf3 automatically populates the GPT3 grid even when
     * @c troposphere_apriori_source is left at @c StandardAtmosphere.
     */
    enum class TroposphereMappingFunction {
        Gmf,   ///< Global Mapping Function (Boehm et al. 2006).  No external grid required.
        Vmf3   ///< VMF3 empirical from GPT3 (Landskron & Boehm 2018).  Requires gpt3_5.grd.
    };

    /**
     * @brief Enumeration for measurement stochastic (weighting) models.
     *
     * Selects how observation variances are scaled based on satellite geometry
     * or signal quality indicators.
     */
    /**
     * @brief Configuration for the post-fit residual history tracker.
     *
     * The tracker (see @c filter/ResidualHistoryTracker.hpp) maintains
     * an EWMA mean and variance of the post-fit residual for each
     * (satellite, signal) row.  When the spatial pivot-slip detector
     * in @c KalmanFilter flags a candidate group, the tracker
     * supplies a temporal corroboration signal: the fraction of
     * rows in the group whose residual exceeds @c z_threshold sigma
     * relative to its own EWMA history.  When that fraction exceeds
     * @c fraction_threshold the temporal evidence and spatial
     * evidence agree → high-confidence pivot slip.
     *
     * All settings live under the JSON path
     * @c estimation.residual_history.{enabled,alpha,warmup_n,
     * z_threshold,fraction_threshold}.
     */
    struct ResidualHistoryConfig {
        bool        enabled            = false; ///< Master toggle.  Default off; opt in via JSON.
        double      alpha              = 0.05;  ///< EWMA forgetting factor (≈30-epoch effective window at α=0.05).
        std::size_t warmup_n           = 20;    ///< Min samples per (sat, signal) before z-tests fire.
        double      z_threshold        = 4.0;   ///< |z| threshold for declaring a row temporally anomalous.
        double      fraction_threshold = 0.5;   ///< Group-level fraction-of-anomalous-rows threshold for corroborating a spatial pivot-slip request.
    };

    /**
     * @brief Configuration for the stuck-code-bias detector + reset path.
     *
     * The sat-bias estimator can fall into a closed loop where the
     * code residual on one (satellite, signal) row keeps tripping
     * the outlier gate: the bias state has over-converged around a
     * value that no longer matches the truth, every new innovation
     * shows up as a large residual, the gate excludes the row, and
     * the bias state never gets the chance to update.  Either or
     * both of two signals fire the reset:
     *  - @c min_streak consecutive outlier-rejection epochs on the
     *    same (sat, signal) — the "already-stuck" case where the
     *    bias is wrong enough that nothing ever survives the gate.
     *  - EWMA |mean| of post-fit residuals exceeding @c bias_floor_m
     *    on a warmed-up row — the slow-drift case where the row
     *    still survives the gate but its converged bias has drifted.
     *
     * On trigger the corresponding sat-bias state is re-seeded
     * (value snapped by the EWMA mean if available; variance
     * inflated back to @c initial_std.sat_bias²) via
     * @c StateManager::resetSatBiasState.  The tracker's history for
     * that row is also cleared so the next handful of epochs
     * re-establish the baseline against the fresh state.
     *
     * All settings live under
     * @c estimation.stuck_code_bias.{enabled,min_streak,bias_floor_m}.
     */
    struct StuckCodeBiasConfig {
        bool        enabled       = true; ///< Master toggle.  Default on — cheap to evaluate and protects against a well-known failure mode.
        std::size_t min_streak    = 5;    ///< Consecutive-rejection epochs that trigger a reset.  0 disables the streak branch.
        double      bias_floor_m  = 3.0;  ///< EWMA |mean| (m) above which a warmed-up row's bias is considered drifted.  0 disables the drift branch.
    };

    /**
     * @brief Configuration for sat-wide state re-inflation after a
     *        cycle-slip / LLI ambiguity reset.
     *
     * In PPP-RTK uncombined mode the per-sat iono state and the
     * per-(sat, signal) sat-bias states can co-converge to a joint
     * partition that absorbs slant iono in a non-unique way — the
     * iono state holds I_true minus some leakage δ/α, and the
     * sat-bias state holds b_true plus δ.  Phase ambiguities anchor
     * the partition while they're alive.  When a sat-wide cycle slip
     * or LLI lock-loss resets *every* phase ambiguity for that
     * satellite, the new ambiguities reseed against whatever the
     * current iono/sat-bias state say — baking the (possibly wrong)
     * pre-reset partition into the rest of the arc.  The residual
     * on the unanchored-by-phase signals (typically L5 in a
     * triple-frequency setup) then carries the
     * elevation-dependent unmodelable component for hundreds of
     * epochs.
     *
     * On sat-wide ambiguity reset the iono state's diagonal is
     * re-inflated to @c iono_std_m² and every sat-bias state for
     * that satellite has its diagonal re-inflated to
     * @c sat_bias_std_m² — values are preserved (slant iono and
     * hardware delays don't change just because the receiver lost
     * phase lock), only the cross-state covariance is broken so the
     * filter can re-partition cleanly via the next several epochs
     * of measurements.  Pivots are skipped at the StateManager layer
     * (their frozen (0, 0) row is detected by the diag==0 guard).
     *
     * All settings live under
     * @c estimation.slip_state_reinflation.{enabled,iono_std_m,sat_bias_std_m}.
     */
    struct SlipStateReinflationConfig {
        bool   enabled        = true; ///< Master toggle.  Default on — the partition-baking failure mode is structural to uncombined PPP-RTK with sat-bias estimation.
        double iono_std_m     = 5.0;  ///< Re-inflated iono diagonal std (m).  0 or negative disables the iono branch (StateManager falls back to initial_std.ionosphere).
        double sat_bias_std_m = 5.0;  ///< Re-inflated sat-bias diagonal std (m).  0 or negative disables the sat-bias branch (StateManager falls back to initial_std.sat_bias).
    };

    /**
     * @brief Real-time startup gate: defer estimation until the SSR correction
     *        stream is ready, so the filter seeds code+phase together instead of
     *        cold-starting code-only on metre-grade broadcast clocks.
     *
     * Cold-starting before SSR is in place is harmful: the filter converges
     * position / receiver clock / per-sat iono / sat biases against metre-level
     * broadcast clocks, and then each satellite's SSR flips it from
     * broadcast-grade to cm-grade at a DIFFERENT epoch — turning phase on
     * mid-arc and seeding a fresh ambiguity against an already-contaminated
     * (iono, bias) partition.  Waiting until corrections are ready avoids the
     * wasted code-only convergence and the staggered partition-baking flips.
     *
     * Readiness criterion (real-time only): at least @c min_satellites satellites
     * are broadcast+SSR (cm-grade clock, phase retained — NOT @c
     * PppObservation::broadcast_only).  By default that QUORUM is sufficient and
     * stragglers still on a broadcast-only clock join later as their SSR arrives
     * — a late sat joining an already-converged RTK filter is benign (its fresh
     * ambiguity initialises against a healthy state; the harm the gate prevents
     * is the GLOBAL code-only cold-start, not individual late joiners).  Set @c
     * require_all_above_mask to instead block until EVERY modeled above-mask sat
     * is broadcast+SSR (no straggler flips at all, but one lagging sat stalls the
     * start).  Until satisfied the engine accumulates nav/SSR, keeps refreshing
     * the a-priori, and runs no predict/update.  No-op in post-processing (all
     * products present at once, so @c computeModels never yields broadcast_only).
     *
     * Config: @c estimation.defer_until_corrections_ready.{enabled,min_satellites,require_all_above_mask,timeout_s}.
     */
    struct CorrectionsReadyGateConfig {
        bool   enabled        = false; ///< Master toggle.  Default off — preserves the incremental cold-start for existing configs.
        int    min_satellites = 5;     ///< Global quorum / solvable floor: start once at least this many sats (across all constellations) are cm-grade (broadcast+SSR).
        int    min_satellites_per_system = 4; ///< Per-constellation quorum: each active constellation with >=2 above-mask sats must have at least min(this, its above-mask count) cm-grade before start.  Guarantees multi-constellation phase geometry at start (prevents the GPS-only-phase cold start that drives the ZWD/height degeneracy).  <=0 disables the per-system check (flat global floor only).
        bool   require_all_above_mask = false; ///< If true, ALSO require every above-mask sat to be broadcast+SSR (no stragglers).  Default false = quorum (start at the floors; late sats join as SSR arrives).
        bool   drop_broadcast_grade = false; ///< If true, drop broadcast-grade (broadcast_only) sats from the solution ENTIRELY each epoch (not just their phase) once estimation runs — so metre-grade code never pollutes a precise SSR PPP-RTK solution and a broadcast-grade sat can never seat as a BSSD pivot.  Default false = keep broadcast-grade code (broadcast-only float).
        double timeout_s      = 0.0;   ///< If > 0, start in degraded mode after this many seconds without readiness.  0 = wait indefinitely.
    };

    /**
     * @brief Soft a-priori constraint on the zenith wet delay (ZWD) state.
     *
     * Under BSSD the only receiver-side common-mode states are position + ZWD,
     * and the [up, ZWD] subspace is near-collinear (both enter the single-
     * differenced range via monotone elevation functions).  With no anchor a
     * common-mode start error runs equal-and-opposite into that near-null
     * direction (ZWD and height to ~1000 m) until satellite geometry re-
     * conditions it.  A recurring soft pseudo-observation pulls ZWD toward a
     * climatological value AND shrinks the [up, ZWD] covariance correlation each
     * epoch — the covariance-consistent form of a "don't let ZWD run away"
     * clamp (a value-only clamp would fight the filter and never recover the
     * height).  At loose @c sigma_m it costs the converged solution nothing (the
     * cm-level geometric ZWD signal overrides the prior once geometry is good).
     *
     * Config: @c estimation.zwd_constraint.{enabled,apriori_m,sigma_m}.
     */
    struct ZwdConstraintConfig {
        bool   enabled   = false; ///< Master toggle.  Default off — opt in (set true for real-time BSSD PPP-RTK).
        double apriori_m = 0.10;  ///< Climatological ZWD prior mean (m).  ~0.1 m is a temperate-latitude default.
        double sigma_m   = 0.30;  ///< Prior 1-sigma (m).  Loose enough not to bias the converged cm-level ZWD; tight enough to bound the null-space runaway.  Tighten toward ~0.1 m for a harder anchor.
    };

    /**
     * @brief Reference level the MW detector measures slip deviations against.
     *
     * The MW slip test is |mw[k] − reference| > kσ·spread.  The choice of
     * reference determines what "deviation" means and which spread estimate
     * is statistically consistent with it:
     *
     *  - @c HatchMean — legacy behavior.  Reference is the slow Hatch
     *    (recursive) mean (α = 1/100 at maturity, ~100-epoch memory), spread
     *    is the MAD of first time-differences.  Under a smooth MW trend of
     *    slope s/epoch the lag offset settles at ≈ 99·s while the TD1 MAD
     *    stays at the noise floor — the statistics are mismatched, and mild
     *    drifts (≲ 1 mm/epoch) produce persistent false slips on mature
     *    arcs; steeper drifts (~7–9 mm/epoch) can even pass the repair
     *    fractional gate and publish a confident wrong integer.
     *
     *  - @c LocalMedian — reference is the median of a short causal window
     *    of recent accepted MW values (length @c mw_local_window), spread is
     *    the MAD of the same window (×√2 so the configured
     *    @c mw_sigma_multiplier keeps its legacy TD1-equivalent scale).
     *    Deviation and spread now measure the same quantity, so a trend
     *    lifts both and cancels to first order, while a genuine step still
     *    spikes the deviation.  Causal — no future data; the current epoch
     *    is excluded from its own reference.  Residual weakness: a smooth
     *    QUADRATIC signal (curvature) still walks the level away from the
     *    window median faster than the window forgets, leaking into the
     *    deviation as a soft ramp.
     *
     *  - @c TimeDifference — no level reference at all: the deviation IS
     *    the time-difference of the MW series (order selected by
     *    @c mw_td_order), gated by the MAD of the same time-difference
     *    history.  Memoryless, hence immune to trends AND curvature: a
     *    linear drift contributes a near-constant offset (first order) or
     *    exactly zero (second order) that the matching MAD absorbs, while
     *    a slip still spikes by dN·λ_WL for one epoch.  The price is
     *    noise amplification (σ_TD1 = √2·σ_MW, σ_TD2 = √6·σ_MW) — smaller
     *    real slips sit closer to the gate.  Second order additionally
     *    echoes a real slip with opposite sign one epoch later; the echo
     *    is contained by the erase-on-slip arc restart (unlocked arcs)
     *    or absorbed (locked arcs).
     *
     * In every mode the Hatch mean and the WL integer/lock machinery are
     * unaffected by this selection.
     *
     * Configured via @c estimation.cycle_slip.mw_detect_reference
     * ("hatch_mean" | "local_median" | "time_difference").
     */
    enum class MwDetectReference {
        HatchMean,      ///< Legacy: deviation from the slow Hatch mean, TD1-MAD spread.
        LocalMedian,    ///< Deviation from a short causal window median, window-MAD spread.
        TimeDifference  ///< Deviation IS the MW time-difference (order per MwTdOrder), TD-MAD spread.
    };

    /**
     * @brief Differencing order for @c MwDetectReference::TimeDifference.
     *
     *  - @c First  — dev = mw[k] − mw[k−1].  A linear MW trend of slope s
     *    per epoch appears as a constant ≈ s (millimeters — far below the
     *    √2-amplified noise), a quadratic as a slow ramp the matching TD1
     *    MAD tracks.  The default and the recommended starting point.
     *  - @c Second — dev = mw[k] − 2·mw[k−1] + mw[k−2].  Exactly zero for
     *    any linear trend and a tiny constant for a quadratic, at the cost
     *    of √3 further noise amplification over first order and a
     *    sign-flipped echo of every real slip one epoch later.  Reserve
     *    for data where curvature defeats the first-order statistic.
     *
     * Configured via @c estimation.cycle_slip.mw_td_order
     * ("first" | "second").
     */
    enum class MwTdOrder {
        First,   ///< mw[k] − mw[k−1] (default).
        Second   ///< mw[k] − 2·mw[k−1] + mw[k−2].
    };

    /**
     * @brief Selects which cycle-slip detectors run.
     *
     * Drives config-driven composition of the @c CycleSlipManager detector
     * set via @c SlipDetectorRegistry::createEnabled.  The Melbourne-Wübbena
     * wide-lane detector is the primary detector and the source of the WL
     * integers consumed by ambiguity resolution and the BSSD pivot machinery,
     * so @c mw defaults on; the auxiliary detectors default off (preserving
     * the historical MW-only behavior) and are opted in per kind.  Kinds whose
     * implementation is still STAGED (geometry-free @c gf, ionosphere-free
     * phase @c ifp) are accepted for forward compatibility but the registry
     * skips them with a warning until their .cpp is compiled in.
     *
     * All detector-selection settings live under
     * @c estimation.cycle_slip.{mw,graphic,ewl,gf,if}.
     *
     * @c turbo_edit is not a detector kind but a *coordination mode*: when set
     * (and the MW detector is enabled) the @c CycleSlipManager / @c RealTimeEngine
     * pair the MW wide-lane integer @f$\Delta N_{WL}=\Delta N_1-\Delta N_2@f$
     * with the geometry-free phase jump
     * @f$\Delta\Phi_{GF}=\lambda_1\Delta N_1-\lambda_2\Delta N_2@f$ to solve for
     * the per-signal integer slips @f$(\Delta N_1,\Delta N_2)@f$ (Blewitt
     * TurboEdit).  This resolves a dual-frequency slip into a targeted
     * covariance-preserving per-signal repair — which MW alone cannot do
     * (one equation, two unknowns) — and catches MW's @f$\Delta N_1=\Delta N_2@f$
     * blind spot.  It is RATE-ADAPTIVE: at high rate (≤1 s) the GF jump is
     * essentially iono-free and the full repair machinery engages; at coarser
     * rates (e.g. 30 s RINEX) the un-cancelled ionospheric residual honestly
     * inflates the measured GF noise, and every GF-backed decision is gated
     * on that noise against the integer lattice it must discriminate
     * (@c turbo_edit_split_max_sigma) — large MW-detected slips still split
     * and repair under quiet ionosphere, and flagged satellites fall back to
     * the conservative reset as activity rises.  A per-epoch cross-satellite
     * IVI gate additionally suppresses repair when the ionosphere is
     * disturbed, regardless of rate.  See the @c TurboEditResolver header's
     * "Rate adaptivity" note.
     */
    /**
     * @brief Which navigation aiding sources participate in the estimator.
     *
     * Selection tokens consumed by @c AidingSourceRegistry::createEnabled to
     * compose the @c AidingSourceManager source set — the navigation-aiding
     * analog of @c CycleSlipConfig / @c SlipDetectorRegistry.  Each flag maps to
     * an @c AidingSourceKind; the IMU is the primary (built first).  For
     * back-compatibility, @c imu defaults to the existing @c imu.enabled
     * (@c enableImu()) when @c estimation.aiding.imu is not set explicitly.
     * @c vision is STAGED (its .cpp is excluded from the build; the registry
     * warns and skips it if selected).
     *
     * @see docs/aiding_source_abstraction_design.md
     */
    struct AidingConfig {
        bool imu    = false;  ///< IMU INS error-state aiding (primary; default = enableImu()).
        bool zupt   = false;  ///< Zero-velocity update pseudo-measurement.
        bool nhc    = false;  ///< Non-holonomic constraint (lateral/vertical body velocity ~ 0).
        bool wheel  = false;  ///< Wheel/odometer speed.
        bool baro   = false;  ///< Barometric height.
        bool vision = false;  ///< Visual odometry / landmarks (STAGED — registry skips with a warning).

        /// ZUPT pseudo-measurement std (m/s) per velocity axis: the assumed
        /// uncertainty of the "velocity == 0 while stationary" constraint.
        /// Smaller => stronger pull to zero.  Via estimation.aiding.zupt_sigma_mps.
        double zupt_sigma_mps = 0.05;
    };

    struct CycleSlipConfig {
        bool mw         = true;   ///< Melbourne-Wübbena wide-lane detector (primary; default on).
        bool graphic    = false;  ///< GRAPHIC code+phase per-signal detector.
        bool ewl        = false;  ///< Extra-wide-lane MW detector (3+ frequency satellites).
        bool gf         = false;  ///< Geometry-free detector (STAGED — registry skips with a warning if set).
        bool ifp        = false;  ///< Ionosphere-free phase detector (STAGED — registry skips with a warning if set).
        bool turbo_edit = false;  ///< MW+GF joint per-signal slip repair (TurboEdit); rate-adaptive. See struct doc.
        /// MW-only code-domain veto (effective only with @c turbo_edit): an MW
        /// flag whose geometry-free phase is affirmatively stable (|TD2| within
        /// the arc's own MAD noise — with the zero gate sharp enough to
        /// discriminate the slip lattice — under quiet iono / mature arc /
        /// no LLI) is classified as a code-domain artifact (multipath, NLOS,
        /// DLL stress) and the mandated phase ambiguity reset is SKIPPED — no
        /// integer pair of plausible size is consistent with a flat GF, and the
        /// KF phase-innovation (residual) gate backstops the residual
        /// null-line-pair risk.  See the TurboEditResolver header's
        /// "code-domain veto" note.  Via
        /// @c estimation.cycle_slip.turbo_edit_mw_only_veto.
        bool turbo_edit_mw_only_veto = true;
        /// Gap-spanning GF bridge (effective only with @c turbo_edit): when a
        /// GF arc's chain breaks for at most this many seconds (per-signal
        /// dropout or full receiver gap), the pre-gap GF level is stashed and
        /// the ACROSS-GAP GF jump — drift-corrected by the arc's own EWMA GF
        /// rate — serves as the joint-solve evidence at the first post-gap
        /// epochs, instead of being blind until three consecutive epochs
        /// rebuild a TD2.  A λ-quantized jump attributes the re-lock slip per
        /// signal (continuous signal → integer repair; dropped signal → its
        /// ambiguity was pruned-and-reseeded on return, so the slip is moot
        /// and the mandate is suppressed); a flat jump classifies the MW flag
        /// as a code artifact (re-acquisition DLL transient).  Bounded by
        /// ionospheric coherence: beyond ~15 s even quiet-iono drift
        /// approaches the integer-pair spacing.  0 disables (pre-bridge
        /// behavior: post-gap MW flags are unadjudicated → reset).  Via
        /// @c estimation.cycle_slip.turbo_edit_max_gap_s.
        double turbo_edit_max_gap_s = 15.0;
        /// Soft-LLI bridge (effective only with @c turbo_edit + a non-zero
        /// @c turbo_edit_max_gap_s, high-rate only): when a receiver-declared
        /// loss of lock (LLI) on an AR-capable dual-frequency satellite would
        /// otherwise trigger an immediate full ambiguity reset, instead HOLD
        /// the satellite's phase ambiguities across the (short) LLI gap — keep
        /// the GF arc, snapshot the pre-gap values, exclude the held phase
        /// measurements from the KF update across the MW one-epoch confirmation
        /// delay — and let the gap bridge apply a covariance-preserving integer
        /// REPAIR at gap return (@c snapshot + dN·λ) rather than discarding the
        /// converged ambiguity precision and AR continuity.  This bets that the
        /// carrier loop held phase coherence across the LLI; if it instead
        /// re-acquired (arbitrary integer), the KF phase-innovation gate resets
        /// the wrong held-repair one epoch late — the same cost as the
        /// re-acquisition reset path.  Beyond @c turbo_edit_max_gap_s the
        /// classic full reset stands.  Default OFF (baseline unchanged).  Via
        /// @c estimation.cycle_slip.turbo_edit_bridge_lli.  See
        /// docs/turbo_edit_lli_bridge_design.md.
        bool turbo_edit_bridge_lli = false;
        /// WL-settle deferral cap (s) for the gap-bridge joint solve
        /// (effective only with @c turbo_edit + @c turbo_edit_bridge_lli +
        /// a non-zero @c turbo_edit_max_gap_s): when the across-gap GF jump
        /// is solid but the MW wide-lane evidence is float-only and
        /// AMBIGUOUS (two or more integer candidates fit the GF lattice —
        /// candidates k and k±2 differ by a null-line pair only a few mm
        /// apart in GF), the adjudication is DEFERRED instead of guessed:
        /// the soft-LLI holds stay in place (measurements excluded, states
        /// prune-protected), the pre-gap GF level stays stashed, and each
        /// subsequent epoch the post-restart MW arc's Hatch mean
        /// re-estimates the across-gap WL step with √n-averaged code noise
        /// (the instantaneous float at re-acquisition is NL-code-biased by
        /// up to several cycles).  The joint solve re-runs once that
        /// estimate is confidently roundable (the WL-lock criterion:
        /// |frac| + 3·σ_mean < 0.5 cycles, n ≥ 10); at this cap with no
        /// settle — or if the post-gap GF chain flags a new event — the
        /// satellite falls back to the conservative reset.  0 disables
        /// deferral (an ambiguous bridged solve resets immediately).  Via
        /// @c estimation.cycle_slip.turbo_edit_wl_settle_max_s.
        double turbo_edit_wl_settle_max_s = 45.0;
        /// Split-capability gate (effective only with @c turbo_edit): the
        /// maximum 1σ uncertainty, in CYCLES, of the float per-signal integer
        /// the TurboEdit joint solve rounds (σ_TD2 / |λ1−λ2|).  This is the
        /// lever that makes the resolver rate-adaptive: the per-signal split
        /// (and the bridged across-gap attribution) emits integers only when
        /// the arc's measured GF noise satisfies
        /// σ_TD2 ≤ this × |λ1−λ2| — at the 0.2 default a 3.5σ GF error still
        /// lands inside the rounding well of the true integer (the
        /// wrong-integer acceptance distance is 1 − frac_max = 0.7 cycles).
        /// Millimetric 1 s phase noise passes trivially (historical high-rate
        /// behavior unchanged); at 30 s the un-cancelled iono residual passes
        /// only while quiet, demoting MW-flagged satellites to the
        /// conservative reset otherwise.  Raising this admits repairs from
        /// noisier GF evidence (more repairs, more wrong-integer risk);
        /// lowering it tightens toward reset-always.  Via
        /// @c estimation.cycle_slip.turbo_edit_split_max_sigma.
        double turbo_edit_split_max_sigma = 0.2;

        // --- Per-detector MAD threshold scale factors (kσ gate multipliers) ---
        // Each adaptive slip threshold is `multiplier × max(σ_MAD, floor)`,
        // where σ_MAD is the robust (MAD×1.4826) noise estimate.  Raising the
        // multiplier widens the gate (fewer detections, fewer false resets, but
        // smaller real slips can slip through); lowering it tightens detection.
        // Defaults preserve the historical hardcoded values; each must be > 0.
        // Configured via @c estimation.cycle_slip.<name>_sigma_multiplier.
        double mw_sigma_multiplier         = 8.0;  ///< MW (Melbourne-Wübbena) wide-lane gate.
        double turbo_edit_sigma_multiplier = 6.0;  ///< TurboEdit GF-only (MW blind-spot) gate.
        double graphic_sigma_multiplier    = 6.0;  ///< GRAPHIC code+phase gate.
        double ewl_sigma_multiplier        = 5.0;  ///< Extra-wide-lane gate.

        // --- MW detection-reference selection (see MwDetectReference doc) ---
        /// Reference the MW slip deviation is measured against.
        /// "hatch_mean" (legacy) | "local_median" (trend-immune level test) |
        /// "time_difference" (trend- AND curvature-immune jump test).  Via
        /// @c estimation.cycle_slip.mw_detect_reference.
        MwDetectReference mw_detect_reference = MwDetectReference::HatchMean;
        /// Differencing order for the "time_difference" reference:
        /// "first" (default) | "second".  Ignored in the other modes.  Via
        /// @c estimation.cycle_slip.mw_td_order.
        MwTdOrder mw_td_order = MwTdOrder::First;
        /// Length (epochs) of the causal local-median window used when
        /// @c mw_detect_reference is @c LocalMedian.  Must be ≥ 15 (the
        /// detector's MAD-maturity floor, MwSlipDetector::kMinEpochsForAdaptive)
        /// and ≤ 64 (the fixed per-arc ring capacity); the loader clamps with
        /// a warning.  Via @c estimation.cycle_slip.mw_local_window.
        int mw_local_window = 20;
    };

    /**
     * @brief Configuration for the adaptive ionospheric process-noise scheme.
     *
     * In PPP-RTK uncombined mode the per-satellite slant ionospheric state
     * is driven by a single static process-noise spectral density
     * (@c ProcessNoise::ionosphere).  When @c enabled, @c IonoVariabilityMonitor
     * scales that baseline per satellite, per epoch, from a direct measure
     * of ionospheric disturbance: the running variability of the
     * geometry-free (L1-L2) carrier-phase second time-difference, a
     * TEC-acceleration proxy.
     *
     * The per-satellite multiplier is the squared ratio of a recent-window
     * MAD-sigma to a baseline-window MAD-sigma of that satellite's TD2
     * series, clamped to [1, max_multiplier].  Using the ratio
     * self-normalizes: the slowly varying geometric (obliquity-curvature)
     * component sits in both windows and cancels, so quiet conditions
     * yield a multiplier near 1 at any elevation and the existing
     * 1/sin^2(el) obliquity scaling is not double-counted.
     *
     * JSON: estimation.adaptive_iono_process_noise.{enable,recent_window,
     * baseline_window,max_multiplier,min_epochs,trace}.
     */
    /**
     * @brief Configuration for GIM-based ionosphere weighting (PPP-RTK).
     *
     * When enabled, an external IONEX global ionosphere map is applied
     * each epoch as a soft pseudo-observation on the per-satellite slant
     * ionosphere states.  A large per-constellation common-mode variance
     * leaves the absolute ionospheric level unconstrained (it carries an
     * un-modellable receiver datum bias), so only the trustworthy
     * between-satellite structure of the GIM is imposed.
     *
     * JSON: estimation.ionosphere_weighting.{enable,common_mode_sigma_m,
     * std_scale,floor_m,elevation_cutoff_deg}.
     */
    struct IonosphereWeightingConfig {
        bool   enabled = false;             ///< Master toggle.  Default off; opt in via JSON.
        double common_mode_sigma_m = 50.0;  ///< Per-constellation common-mode std (m); large => absolute level free.
        double std_scale = 1.0;             ///< Scale on the GIM RMS-derived per-row pseudo-obs std.
        double floor_m = 0.05;              ///< Lower floor on the per-row pseudo-obs std (m).
        double elevation_cutoff_deg = 10.0; ///< Satellites below this elevation are not GIM-constrained.
    };

    struct AdaptiveIonoQConfig {
        bool   enabled         = false; ///< Master toggle.  Default off; opt in via JSON.
        int    recent_window   = 20;    ///< Recent-window length (epochs) for the short-term TD2 MAD-sigma.
        int    baseline_window = 240;   ///< Baseline-window length (epochs) for the long-term TD2 MAD-sigma.
        double max_multiplier  = 100.0; ///< Upper clamp on the per-satellite process-noise multiplier (lower clamp 1.0).
        int    min_epochs      = 30;    ///< Min TD2 samples on an arc before its multiplier may deviate from 1.0.
        bool   trace           = false; ///< Write a per-satellite GF/TD2/variability CSV trace to the log directory.
    };

    enum class StochasticModelType {
        Constant,   ///< Use the a-priori ?� directly, no geometry scaling.
        Elevation,      ///< Classic 1/sin�(el) weighting (default).
        SNR,            ///< SNR-based weighting using the SIGMA-? model.
        SatelliteBlock  ///< Block/frequency-dependent zenith noise with 1/sin�(el) scaling.
    };

    /**
     * @brief Struct representing receiver antenna orientation.
     *
     * Contains azimuth and elevation angles for antenna orientation.
     */
    struct ReceiverAntennaOrientation {
        double azimuth_deg{ 0.0 };   ///< Antenna azimuth orientation in degrees.
        double elevation_deg{ 90.0 }; ///< Antenna elevation orientation in degrees.
    };

    /**
     * @brief Struct for process noise spectral densities (variance rates).
     *
     * Each field is a spectral density in m*m/s. The Kalman filter multiplies
     * by dt (seconds) to obtain the variance (m*m) added per prediction step:
     *   variance_per_epoch = q * dt
     * Exception: @c acceleration_psd is an acceleration PSD (m*m/s^3) and
     * expands to the coupled CWNA block, not a simple q*dt term.
     */
    struct ProcessNoise {
        /// Position (m*m/s). Zero for static positioning; use ~100 for kinematic.
        double position{ 0.0 };

        /// Receiver clock (m*m/s). 1e8 models the clock as white noise,
        /// making it effectively unconstrained each epoch. This is standard
        /// PPP practice for GNSS receivers without atomic clocks.
        /// At 30 s: sqrt(1e8 * 30) ~ 54 km, i.e. no constraint.
        /// However, clock is initialized by WLS at each epoch.
        double clock{ 1e4 };

        /// Troposphere ZWD (m*m/s). 1e-8 yields ~0.5 mm/sqrt(hr),
        /// consistent with mid-latitude IGS analysis center defaults
        /// (Boehm et al.). Allows the ZWD to track weather-driven changes
        /// over multi-hour arcs without over-constraining the estimate.
        double troposphere{ 1e-8 };

        /// Ionosphere zenith delay (m*m/s). Only active in PPP-RTK
        /// uncombined mode. Scaled to slant by 1/sin�(el) per satellite.
        /// 0.02 is within the typical 0.01-0.1 range for the zenith value.
        double ionosphere{ 0.10 };

        /// Carrier phase ambiguity (m*m/s).  Default 1e-12 is a "safety
        /// floor" — sub-mm accumulation per day, indistinguishable from
        /// the constant-ambiguity model in physical terms, but enough
        /// to keep the per-(sat, signal) ambiguity covariance from
        /// underflowing toward zero on long arcs.  Variance shrinks via
        /// every measurement update; without a floor, after hours of
        /// clean tracking the joint covariance Cholesky can lose
        /// positive-definiteness and trigger numerical errors.  Set to
        /// 0.0 to recover the strict constant-ambiguity model when
        /// you're sure the joint covariance can absorb the conditioning.
        double ambiguity{ 1e-12 };

        /// Receiver code bias / inter-frequency bias (m*m/s). In uncombined
        /// PPP-RTK, one code bias per non-reference signal absorbs the
        /// receiver hardware delay difference. Very stable short-term;
        /// 1e-4 allows ~3 mm/sqrt(30 s) drift.
        double code_bias{ 1e-4 };

        /// Inter-system time bias (m*m/s). ISBs are very stable short-term;
        /// 1e-7 allows ~0.17 mm/sqrt(30 s) drift, sufficient for
        /// GPS-Galileo/GLONASS/BeiDou time offset evolution.
        double system_time_offset{ 1e-7 };

        /// Troposphere horizontal gradient (m*m/s). North and East gradients
        /// use the same spectral density. 5e-12 yields ~0.01 mm/sqrt(hr),
        /// appropriate for slow azimuthal asymmetry evolution.
        double tropo_gradient{ 5e-12 };

        /// Phase clock offset delta (m*m/s). In the "common clock + delta"
        /// parameterization the delta captures the receiver code-phase hardware
        /// bias difference, which is physically stable (nearly constant over
        /// hours). A very small random-walk spectral density keeps the delta
        /// tightly constrained so the common clock absorbs epoch-to-epoch
        /// timing variations while ambiguities remain estimable.
        /// 1e-9 yields ~1 mm/sqrt(hr) drift.
        double phase_clock{ 1e-9 };

        /// White-noise acceleration power spectral density (m*m/s^3).  The
        /// single CWNA (continuous white-noise acceleration) parameter: when
        /// velocity is estimated it drives the coupled position+velocity
        /// process-noise block — Q[pos]=q*dt^3/3, Q[pos,vel]=q*dt^2/2,
        /// Q[vel]=q*dt — so position and velocity noise derive from one
        /// physical quantity with the correct cross-covariance.  Unlike the
        /// other fields it is not a simple q*dt term.  ~1e0-1e2 for
        /// pedestrian/vehicular dynamics, near 0 for a static receiver.
        /// Config key: acceleration_psd_q (legacy alias: velocity_q).
        double acceleration_psd{ 1e2 };

        /// Per-satellite signal bias (m*m/s).  Absorbs uncorrected satellite
        /// hardware delays on signals without BIA product coverage.  Nearly
        /// constant over hours; 1e-10 allows ~0.01 mm/sqrt(30 s) drift.
        double sat_bias{ 1e-10 };

        /// Receiver clock-rate spectral density (m*m/s^3).  Under the CWNA
        /// model on the (clock-bias, clock-rate) pair, clock-bias picks up
        /// q*dt^3/3, clock-rate picks up q*dt, with q*dt^2/2 cross-covariance.
        /// 1e-4 is appropriate for a typical TCXO; reduce by a few orders of
        /// magnitude for OCXO-grade receivers.  Active only when
        /// @c estimate_velocity is on (Doppler processing).
        double clock_rate{ 1e-4 };

        /// Inter-system clock-rate offset spectral density (m*m/s^3).  The
        /// rate counterpart of @c system_time_offset.  Single shared
        /// oscillator drives all bands, so ISBR is physically near zero —
        /// the tight default reflects this.  Active only when
        /// @c estimate_velocity is on.
        double system_time_rate_offset{ 1e-10 };
    };

    /**
     * @brief Struct for initial state standard deviations.
     *
     * Specifies initial uncertainties for state variables.
     */
    struct InitialStd {
        double position{ 1e2 };       ///< Initial position standard deviation (m).
        double velocity{ 1e2 };       ///< Initial velocity standard deviation (m/s).
        double clock{ 1e3 };          ///< Initial clock bias standard deviation (m).
        double troposphere{ 0.5 };      ///< Initial tropospheric zenith delay standard deviation (m).
        double ionosphere{ 5.0 };       ///< Initial ionospheric zenith delay standard deviation (m).
        double ambiguity{ 1e3 };      ///< Initial ambiguity standard deviation (m).
        double code_bias{ 10.0 };         ///< Initial receiver code bias (IFB) standard deviation (m).
        double system_time_offset{ 1e2 }; ///< Initial inter-system time bias standard deviation (m).
        double tropo_gradient{ 1e-3 };     ///< Initial tropo gradient standard deviation (m).
        double phase_clock{ 1e3 };          ///< Initial phase clock offset standard deviation (m).
        double sat_bias{ 10.0 };              ///< Initial per-satellite signal bias standard deviation (m).
        double clock_rate{ 1.0 };             ///< Initial receiver clock-rate standard deviation (m/s).
        double system_time_rate_offset{ 1e-2 }; ///< Initial inter-system clock-rate offset standard deviation (m/s).
    };

    /**
     * @brief Struct for default measurement noise parameters.
     *
     * Provides fallback noise values when RINEX does not specify variances.
     */
    struct DefaultMeasurementNoise {
        double code_std_m{ 0.30 };   ///< Code measurement standard deviation (m).
        double phase_std_m{ 0.002 }; ///< Phase measurement standard deviation (m).
        double doppler_std_m{ 0.5 }; ///< Doppler measurement standard deviation (Hz).
    };

    /**
     * @brief Parameters for the SNR-based stochastic model.
     *
     * The model computes measurement variance using the
     * variance-additive form (Hadas & Bosy 2015, building on the
     * Hartinger & Brunner 1999 SIGMA-ε framework):
     *
     *   σ² = a² + b² · 10^(-CN0_dBHz / 10)
     *
     * with @c a the high-SNR noise floor (the asymptote at
     * CN0 → ∞) and @c b the SNR-sensitivity coefficient.  Both have
     * units of metres.  Variance-additive is preferred over the
     * legacy σ-additive form (σ = a + b · 10^(-SNR/10)) because
     * independent noise contributions add in *variance*, not in
     * standard deviation; squaring the σ-additive form generates an
     * unphysical 2ab cross-term.
     *
     * On top of the variance, the model applies:
     *  - Per-band chip-rate scaling for *code* observations
     *    (reuses @c SatBlockNoiseParams::freq_band_scale).  Phase
     *    observations get no per-band scaling — chip rate doesn't
     *    enter the carrier-loop noise formula.
     *  - Wieser 2002 hybrid elevation amplification: the SNR-derived
     *    σ² is multiplied by 1/sin²(el) (gated by
     *    @c phase_elevation_weighting for phase rows).  This captures
     *    multipath-driven noise that pure SNR weighting misses.
     *
     * When the receiver-reported S-observable is missing for a given
     * row, the model synthesizes a CN0 from elevation using a typical
     * antenna gain pattern (CN0_zenith ≈ 50 dBHz, drop ≈ 15 dB to
     * horizon) rather than discontinuously switching to a different
     * functional form.
     *
     * Reference values (variance-additive form):
     *   code  : a ≈ 0.10–0.30 m,    b ≈ 30–100 m
     *   phase : a ≈ 0.001–0.003 m,  b ≈ 0.3–1.0 m
     *
     * References:
     *  - Hartinger H., Brunner F.K. (1999),
     *    "Variances of GPS phase observations: the SIGMA-ε model",
     *    GPS Solutions.
     *  - Wieser A. (2002),
     *    "Robust and fuzzy techniques for parameter estimation and
     *    quality assessment in GPS", Engineering Geodesy / TU Graz.
     *  - Hadas T., Bosy J. (2015),
     *    "IGS RTS precise orbits and clocks verification and accuracy
     *    of real time PPP", GPS Solutions 19(1).
     */
    struct SnrModelParams {
        double code_a{ 0.30 };        ///< High-SNR code σ floor (m).  σ → a as CN0 → ∞.
        double code_b{ 50.0 };        ///< Code SNR-sensitivity coefficient (m).
        double phase_a{ 0.001 };      ///< High-SNR phase σ floor (m).
        double phase_b{ 0.5 };        ///< Phase SNR-sensitivity coefficient (m).
        double snr_floor_dbhz{ 20.0 };///< CN0 floor; observations below this are clamped (or rejected upstream).
        double snr_ceil_dbhz{ 55.0 }; ///< CN0 ceiling; observations above this are clamped to avoid σ → a noise floor exactly.
    };

    /**
     * @brief Parameters for the satellite-block-dependent stochastic model.
     *
     * Assigns zenith-direction code and phase noise based on the satellite
     * block type extracted from the ATX antenna model (e.g. "BLOCK IIF",
     * "BLOCK IIIA", "GALILEO-2").  A per-frequency-band scale factor is
     * applied on top.  The result is then scaled by 1/sin�(el) like the
     * standard Elevation model.
     *
     * Block types are matched by substring (case-insensitive) so that
     * "BLOCKIIIA" matches an ATX type of "G04_BLOCKIIIA_G074".
     */
    struct SatBlockNoiseParams {
        /// Per-block scale factor (unitless).  Multiplied at config-
        /// load time by @c measurement_noise.code_std_m and
        /// @c measurement_noise.phase_std_m to populate the absolute
        /// @c block_code_std_m / @c block_phase_std_m tables below.
        ///
        /// The ratios encode the relative hardware-noise hierarchy:
        /// legacy blocks (Block II/IIR, GLONASS-M legacy, BeiDou-2) sit
        /// at 1.0× the configured baseline; modern hardware (Block III,
        /// Galileo FOC, BeiDou-3) gets ~0.85.  Intermediate blocks fall
        /// in between.  The spread is driven by satellite EIRP and
        /// signal-generation quality, which move received C/N0 by only
        /// ~1-3 dB block-to-block; since code σ ∝ 1/√(C/N0) that is a
        /// ~6-15% noise effect, so the block axis is deliberately the
        /// *narrowest* of the four (block × band × attribute × baseline).
        /// The user tunes the *baseline* σ once via
        /// @c measurement_noise.{code,phase}_std_m and the per-block
        /// proportions multiply it (consistently between code and phase).
        ///
        /// Overridable from JSON under
        /// @c filter.satellite_block.block_scale_factors as an ordered
        /// array of [substring, scale] pairs; omitting the JSON key
        /// keeps these defaults.
        ///
        /// Keys are substrings matched against the ATX type field;
        /// first hit wins.  Ordering matters — specific blocks must
        /// precede their parents (BLOCKIIIA before BLOCKIII before
        /// BLOCKII; BEIDOU-3 before BEIDOU; GLONASS-M+ before
        /// GLONASS-K before GLONASS).
        /// Defaults tuned for high-quality IGS-grade receivers
        /// (Septentrio PolaRx5/mosaic, Trimble Alloy/NetR9, Javad Delta,
        /// Leica GR50, etc).  The range is compressed to ~1.18×
        /// (best/worst), reflecting the empirical reality that on such a
        /// receiver the self-noise floor swamps block-to-block payload
        /// differences — chip-rate and signal-design effects (per-band,
        /// per-attribute) dominate over which satellite block
        /// transmitted them.  The earlier 3× spread was extrapolated
        /// from legacy / early-2000s receivers and no longer reflects
        /// production hardware.
        std::vector<std::pair<std::string, double>> block_scale_factors{
            {"BLOCKIIIA",  0.85},  ///< GPS III/IIIA — modern digital payload
            {"BLOCKIIIF",  0.85},  ///< GPS IIIF — same generation as IIIA
            {"BLOCKIII",   0.85},  ///< Fallback for unqualified BLOCK III
            {"BLOCKIIF",   0.90},  ///< GPS IIF — modernized but pre-III
            {"BLOCKIIR-M", 0.95},  ///< IIR-M — modernized civil L2C/M-code
            {"BLOCKIIR",   1.00},  ///< IIR — older but capable
            {"BLOCKII",    1.00},  ///< Legacy reference (mostly retired)
            {"GALILEO",    0.85},  ///< Modern signal design (BOC, pilots); IOV lumped in
            {"BEIDOU-3",   0.85},  ///< BDS-3 — modern, BOC signals
            {"BEIDOU",     1.00},  ///< BDS-2 — legacy, multipath-prone
            {"GLONASS-K",  0.90},  ///< CDMA-capable, modern
            {"GLONASS-M+", 1.00},  ///< Mid-tier GLONASS-M variants
            {"GLONASS",    1.00},  ///< Legacy FDMA reference
            {"QZSS-2",     0.85},  ///< QZSS Block II — modern signals
            {"QZSS",       0.90}   ///< QZSS fallback
        };

        /// Zenith code noise per block type (meters).  **Derived** at
        /// config-load time as @c block_scale_factors × @c
        /// measurement_noise.code_std_m.  Direct edits here are
        /// silently overwritten when the loader runs; this field
        /// exists so that the lookup-time consumer
        /// (@c StochasticModel) can keep its existing two-table
        /// interface without needing live access to the
        /// measurement-noise baseline.
        std::vector<std::pair<std::string, double>> block_code_std_m;

        /// Zenith phase noise per block type (meters).  **Derived** as
        /// @c block_scale_factors × @c measurement_noise.phase_std_m,
        /// preserving the same per-block ratio structure as the code
        /// table.  This lets the per-block phase differentiation
        /// follow the user's chosen baseline phase σ automatically
        /// rather than living in a parallel hand-tuned table.
        std::vector<std::pair<std::string, double>> block_phase_std_m;

        /// Per-observable (code / phase) scale tables.
        ///
        /// Wraps the per-band and per-(system, attribute) scale factors
        /// for a single observable type.  @c SatBlockNoiseParams holds
        /// two instances: @c code (chip-rate-driven defaults — narrow
        /// correlator C/A penalty, high-chip-rate band gains) and
        /// @c phase (carrier-tracking-driven defaults — no chip-rate
        /// effects, gentler per-band gradient, attribute differences
        /// largely flat).  Splitting the axis lets each observable use
        /// physically-motivated defaults instead of forcing one table
        /// to compromise between the two regimes — the chip-rate-
        /// driven 1.8× C/A penalty on GPS @c C, for example, has no
        /// analogue on phase (the carrier is the same physical signal
        /// regardless of the code attached), so phase keeps GPS @c C
        /// at 1.0.
        ///
        /// Lookup for @c tracking_attr_scale is: (1) @c [system][attr]
        /// → (2) @c ["default"][attr] → (3) 1.0.  System name matches
        /// @c PppUtils::getSystemString (e.g. "GPS", "Galileo").
        /// @c freq_band_scale is keyed by band digit (RINEX signal code
        /// second character) with 1.0 as the implicit fallback.
        struct ObservableScale {
            /// Per-band scale factor keyed by RINEX band digit.
            std::map<char, double> freq_band_scale;
            /// Per-system, per-attribute scale factor.  Outer key is
            /// system name (or "default"); inner key is the RINEX
            /// attribute character.
            std::map<std::string, std::map<char, double>> tracking_attr_scale;
        };

        /// Code-observable scale tables.  Chip-rate-driven calibration.
        ///
        /// @c freq_band_scale: anchored at band '1' = 1.0 (L1 C/A and
        /// P(Y) share this slot, with the C/A penalty captured under
        /// the attribute axis).  Band '5' family (GPS L5, Galileo
        /// E5a, BeiDou B2a) and '7' (E5b / B2b) get meaningful chip-
        /// rate gain; '8' (Galileo E5 alt-BOC wideband) is the cleanest
        /// code measurement available.
        ///
        /// @c tracking_attr_scale: the attribute axis encodes the
        /// *tracking-method* penalty/bonus relative to clean full-code
        /// BPSK of the same band.  GPS @c C/A carries a small narrow-
        /// correlator penalty (~1.15 on a high-quality receiver).  The
        /// big penalty is semi-codeless P(Y) (@c W = 1.40): cross-
        /// correlation / Z-tracking has a real SNR loss, so on a good
        /// receiver L1/L2 P(Y) code is actually *noisier* than C/A —
        /// the band axis gives band 2 no chip-rate bonus (L2C is
        /// BPSK(1)), so the W penalty is what makes C2W > C1C.  GLONASS
        /// FDMA @c C/A (@c C = 1.50) is the noisiest code in GNSS — its
        /// 0.511 Mcps half-chip-rate can't be seen on the GPS-anchored
        /// band axis, so it lives on the (system-specific) attribute.
        /// Combined I+Q channels (@c X = 0.85) are cleaner — coherent
        /// 3 dB carrier-power gain.  Pilot-only channels (@c Q, @c L,
        /// Galileo @c C, BeiDou B1C @c P) are data-free; data-only
        /// channels (@c I, @c S, @c B, @c D) carry a light data-
        /// modulation tracking penalty.
        ObservableScale code{
            // freq_band_scale
            {
                {'1', 1.0},   ///< L1 / E1 / B1 (reference, BPSK(1)-class)
                {'2', 1.0},   ///< L2 — L2C is BPSK(1) ≈ band 1; P(Y) noise handled by 'W' attr
                {'5', 0.4},   ///< L5 / E5a / B2a — BPSK(10), ~3.2× sharper thermal
                {'6', 0.5},   ///< E6 BPSK(5) / B3I — intermediate chip rate
                {'7', 0.4},   ///< E5b / B2b — BPSK(10), like band 5
                {'8', 0.25},  ///< E5 alt-BOC(15,10) wideband — cleanest code available
            },
            // tracking_attr_scale
            {
                // ---- Cross-system fallback ----
                {"default", {
                    {'P', 1.0},   ///< P-code reference (also: B1C pilot — see BeiDou override)
                    {'W', 1.40},  ///< Semi-codeless P(Y) — cross-correlation/Z-tracking SNR loss
                    {'Y', 1.0},   ///< Explicit Y-code
                    {'M', 1.0},   ///< Military M-code
                    {'L', 0.9},   ///< L2C(L) pilot — data-free
                    {'S', 1.05},  ///< L2C(M+L) data/mixed — light data-modulation penalty
                    {'X', 0.85},  ///< Combined I+Q — coherently 3 dB stronger
                    {'I', 1.05},  ///< Data-only — light data-modulation penalty
                    {'Q', 0.9},   ///< Pilot-only — data-free
                }},
                // ---- GPS ----
                {"GPS", {
                    {'C', 1.15},  ///< C/A (narrow correlator) — small penalty vs P(Y)
                    // 'W' (semi-codeless P(Y)) inherits default 'W' = 1.40.
                }},
                // ---- Galileo ----
                // 'C' is the E1C pilot, NOT the noisy GPS C/A.  E5a/E5b
                // pilots are 'Q'; data channels 'I'; combined 'X'.
                {"Galileo", {
                    {'A', 1.0},   ///< E1A PRS (placeholder; usually masked)
                    {'B', 1.05},  ///< E1B OS data (CBOC) — light data penalty
                    {'C', 0.85},  ///< E1C pilot — data-free
                    {'X', 0.85},  ///< E1 B+C / E5 I+Q (incl. alt-BOC) — coherent gain
                    {'Q', 0.9},   ///< E5a/E5b pilot-only
                    {'I', 1.05},  ///< E5a/E5b data-only
                    {'Z', 0.9},   ///< E6 B+C combined
                }},
                // ---- BeiDou ----
                // 'P' is the B1C pilot, not the GPS P-code.  'I' is the
                // legacy B1I/B2I/B3I data channel.
                {"BeiDou", {
                    {'I', 1.05},  ///< B1I / B2I / B3I data-only (legacy BPSK)
                    {'Q', 0.9},   ///< B2a / B2b / B3 pilot-only
                    {'X', 0.85},  ///< Combined I+Q — coherent gain
                    {'D', 1.05},  ///< B1C data
                    {'P', 0.85},  ///< B1C pilot (overrides default P-code reference)
                }},
                // ---- GLONASS ----
                // FDMA legacy signals: 'C' is the 0.511 Mcps C/A (lowest
                // chip rate in GNSS), 'P' the 5.11 Mcps P-code.  The band
                // axis is GPS-anchored and can't see GLONASS's chip rate,
                // so the chip-rate effect is carried here.  CDMA L3OC
                // (band 3, I/Q/X) falls through to the 'default' table.
                {"GLONASS", {
                    {'C', 1.50},  ///< L1OF/L2OF FDMA C/A — 0.511 Mcps
                    {'P', 0.70},  ///< L1SF/L2SF FDMA P — 5.11 Mcps
                }},
            }
        };

        /// Phase-observable scale tables.  Carrier-tracking-driven.
        ///
        /// Conservative defaults: the phase noise hierarchy on real
        /// data tracks code in *direction* (high-chip-rate bands
        /// cleaner, pilots slightly cleaner) but with *smaller spread*
        /// (chip rate doesn't enter the PLL noise formula at all; the
        /// observed phase-band differences are signal-strength and
        /// receiver-tracking artefacts, not 10× chip-rate effects).
        /// These are "effective σ in metres" ratios, so they already
        /// fold in the wavelength scaling (a fixed cycle-noise maps to
        /// more metres on a longer-λ carrier) together with C/N0 and
        /// processing gain.  Measured ratios on a recent IGS day show
        /// band 7 ≈ 0.35 and band 8 ≈ 0.33 of band 1; these defaults
        /// (0.45 / 0.40) sit just above the measured floor, leaving a
        /// little headroom for noisier receivers and stronger
        /// ionospheric conditions.  Users with calibrated data can
        /// tighten further via JSON override.
        ///
        /// @c freq_band_scale: gentler gradient than code.  Band 2 is
        /// only modestly below band 1 — the longer L2 wavelength works
        /// *against* a metre-noise reduction, and semi-codeless P(Y)
        /// degrades the carrier too (handled on the 'W' attribute), so
        /// the old aggressive 0.6 was too optimistic; 0.85 is closer to
        /// observed L2 phase.  Bands 5/7/8 are genuinely cleaner
        /// (stronger, wider modern signals overcome the wavelength
        /// penalty).
        ///
        /// @c tracking_attr_scale: flatter than code by system — the
        /// chip-rate-driven C/A penalty doesn't apply to phase, so GPS
        /// @c C is 1.0 (not 1.15 like code).  The one non-trivial entry
        /// is @c W = 1.10: semi-codeless tracking degrades the recovered
        /// carrier as well as the code, just far less than the 1.40 code
        /// penalty.  Pilot-vs-data differences are present but small
        /// (5-10%, not 20-30%) — phase tracks the carrier regardless of
        /// data wipe-off success.
        ObservableScale phase{
            // freq_band_scale
            {
                {'1', 1.0},   ///< L1 / E1 / B1 (reference)
                {'2', 0.85},  ///< L2 — modest gain; longer λ + semi-codeless offset the bonus
                {'5', 0.55},  ///< L5 / E5a / B2a — modern modulation, stronger signal
                {'6', 0.70},  ///< E6 / L6 / B3 — mid-tier
                {'7', 0.45},  ///< E5b / B2b — cleaner than band 5 on Galileo FOC
                {'8', 0.40},  ///< E5 alt-BOC wideband — cleanest phase available
            },
            // tracking_attr_scale
            {
                // ---- Cross-system fallback ----
                // Smaller deviations than code: phase carrier-loop noise
                // is less sensitive to the data-modulation distinction
                // than code correlator noise.
                {"default", {
                    {'P', 1.0},  ///< P-code reference
                    {'W', 1.10}, ///< Semi-codeless P(Y) — carrier degraded (far less than code's 1.40)
                    {'Y', 1.0},  ///< Explicit Y-code
                    {'M', 1.0},  ///< Military M-code
                    {'L', 0.95}, ///< L2C(L) pilot — light pilot bonus
                    {'S', 1.05}, ///< L2C(M+L) data/mixed — light data penalty
                    {'X', 0.9},  ///< Combined I+Q — coherent power gain (smaller than code)
                    {'I', 1.05}, ///< Data-only — light penalty
                    {'Q', 0.95}, ///< Pilot-only — light pilot bonus
                }},
                // ---- GPS ----
                // No chip-rate-driven C/A penalty on phase — the carrier
                // is the same physical signal regardless of code.  GPS C
                // phase residuals match GPS L (L1C pilot) and Galileo C
                // (E1C pilot) on real data.  'W' inherits default 1.10.
                {"GPS", {
                    {'C', 1.0},  ///< C/A — no narrow-correlator penalty on phase
                }},
                // ---- Galileo ----
                {"Galileo", {
                    {'A', 1.0},
                    {'B', 1.0},
                    {'C', 0.95}, ///< E1C pilot — light bonus
                    {'X', 0.90}, ///< E1 B+C / E5 I+Q (incl. alt-BOC) — coherent gain
                    {'Q', 0.95}, ///< E5a/E5b pilot-only — light bonus
                    {'I', 1.05}, ///< E5a/E5b data-only — light penalty
                    {'Z', 0.95}, ///< E6 B+C combined
                }},
                // ---- BeiDou ----
                {"BeiDou", {
                    {'I', 1.0},  ///< B1I/B2I/B3I data
                    {'Q', 0.95}, ///< pilot-only — light bonus
                    {'X', 0.90}, ///< combined I+Q — coherent gain
                    {'D', 1.0},  ///< B1C data
                    {'P', 0.95}, ///< B1C pilot — light bonus
                }},
                // ---- GLONASS ----
                // Phase noise is chip-rate-independent, so FDMA C/A gets
                // only a small C/N0-driven penalty (not the 1.50 code one).
                {"GLONASS", {
                    {'C', 1.05}, ///< L1OF/L2OF FDMA C/A — light penalty
                }},
            }
        };

        /// Fallback code noise when no block match is found (meters).
        /// **Derived** at config-load time as
        /// @c measurement_noise.code_std_m (i.e.\ scale factor 1.0,
        /// equivalent to the worst legacy block).
        double default_code_std_m{ 0.15 };
        /// Fallback phase noise when no block match is found (meters).
        /// **Derived** at config-load time as
        /// @c measurement_noise.phase_std_m.
        double default_phase_std_m{ 0.001 };

        /// Whether to route phase observations through the
        /// block × @c phase.freq_band_scale × @c phase.tracking_attr_scale
        /// × elevation pipeline (@c true, default), or use the simpler
        /// @c default_phase_std_m × elevation path (@c false).
        ///
        /// The flat path was historically the default on a PLL-physics
        /// argument: per-block carrier-loop noise differential sits
        /// below the multipath / atmospheric noise floor, and the
        /// chip-rate-driven freq scaling doesn't apply post-
        /// correlation.  Real-data phase residuals do show structure
        /// across bands and tracking attributes — just *different*
        /// structure than code — so the model keeps the structural
        /// shape but uses dedicated @c phase tables with carrier-
        /// tracking-driven defaults (no chip-rate C/A penalty,
        /// smaller band gradient, near-flat attribute differences).
        /// See @c ObservableScale doxygen for the per-axis rationale.
        ///
        /// When @c true, phase uses the per-block @c block_phase_std_m
        /// table (populated from @c block_scale_factors ×
        /// @c measurement_noise.phase_std_m at config load), the
        /// @c phase.freq_band_scale table, and the
        /// @c phase.tracking_attr_scale per-system table.  Elevation
        /// amplification uses @c phase_elevation_power (gentler than
        /// code's) via @c computeElevation's per-observable dispatch.
        ///
        /// Has no effect when the active stochastic model is anything
        /// other than @c SatelliteBlock — @c Constant / @c Elevation /
        /// @c SNR models keep their own phase paths unchanged.
        bool apply_to_phase{ true };
    };

    /**
     * @brief Selects how GNSS and INS measurements are fused.
     *
     * - LooseCoupled: GNSS position/velocity solutions update INS states
     *   as pseudo-measurements.  The existing MeasurementModel operates
     *   independently and IMU only drives the prediction step.
     * - TightCoupled: Raw GNSS code/phase/Doppler observations update the
     *   full INS+GNSS error-state vector directly.  The MeasurementModel
     *   Jacobian is extended with attitude-error partials so that GNSS
     *   measurements constrain attitude, sensor biases and ambiguities
     *   simultaneously.
     */
    enum class ImuCouplingMode {
        LooseCoupled,   ///< IMU prediction + GNSS solution update.
        TightCoupled    ///< IMU prediction + raw GNSS observation update.
    };

    /**
     * @brief Configuration for IMU (Inertial Measurement Unit) integration.
     *
     * When enabled, the Kalman filter uses IMU specific-force and angular-rate
     * measurements to propagate position, velocity and attitude between GNSS
     * epochs.  The coupling_mode selects loosely- or tightly-coupled fusion.
     * Nine additional states (3 attitude error + 3 accelerometer bias +
     * 3 gyroscope bias) are added to the filter.
     */
    struct ImuConfig {
        bool enabled{ false };                       ///< Master enable for IMU integration.
        ImuCouplingMode coupling_mode{ ImuCouplingMode::LooseCoupled }; ///< Fusion strategy.
        std::filesystem::path imu_file_path;         ///< Path to the IMU CSV data file.

        /// Accelerometer noise density (m/s�/?Hz).  Typical MEMS: 0.01�0.1.
        double accel_noise_density{ 0.05 };
        /// Gyroscope noise density (rad/s/?Hz).  Typical MEMS: 1e-4�1e-3.
        double gyro_noise_density{ 5e-4 };
        /// Accelerometer bias instability (m/s�/?Hz, random-walk spectral density).
        double accel_bias_instability{ 1e-4 };
        /// Gyroscope bias instability (rad/s�/?Hz, random-walk spectral density).
        double gyro_bias_instability{ 1e-6 };

        /// Initial accelerometer bias standard deviation (m/s�).
        double initial_accel_bias_std{ 0.1 };
        /// Initial gyroscope bias standard deviation (rad/s).
        double initial_gyro_bias_std{ 1e-3 };

        /// Initial attitude uncertainty (rad).  Used to seed the attitude
        /// block of the error-state covariance.
        double initial_attitude_std{ 0.1 };
    };


    /**
     * @brief Grouped enable/disable flags for core PPP model corrections.
     *
     * Each flag controls whether the corresponding correction term is
     * computed and applied during observation modelling.  When a flag is
     * false, the associated correction evaluates to zero (the neutral
     * element for an additive range correction) so the rest of the
     * processing chain is unaffected.
     *
     * All flags default to \c true so that the full model is active
     * unless the user explicitly disables individual terms.
     */
    struct ModelCorrections {
        // --- Atmosphere ---
        bool enable_troposphere{ true };               ///< Saastamoinen ZHD + GMF dry mapping (slant hydrostatic delay).
        bool enable_klobuchar_ionosphere{ false };     ///< Klobuchar ionospheric delay model (single-frequency fallback only).  IF combinations and DD short-baselines cancel ionosphere geometrically; PPP-RTK uncombined estimates per-satellite slant iono states.  In all three of those configs the Klobuchar model is a no-op — but constructing the @c Ionosphere instance still pays for an extra @c Model::Geometry load and the Klobuchar coefficient parse.  Default off.  Enable only for single-frequency PPP runs that need a-priori iono.
        bool enable_higher_order_ionosphere{ false };  ///< Apply second-order (B·cos(θ)·STEC/f³) and third-order (η·STEC²/f⁴) ionospheric delays as a per-signal observation-level correction, evaluated at the GIM-derived IPP using a centered tilted dipole geomagnetic field.  Requires a loaded IONEX GIM as the absolute-STEC source; the engine auto-loads the GIM when this flag is on even if @c estimation.ionosphere_weighting.enabled is false.  No-op when no GIM resolves (warning logged at startup).  Magnitudes: I₂ ~1–2 cm at L1 during high TEC, I₃ ~mm; the IF combination only partially cancels these, so the largest visible benefit is in standard PPP (IF).  For PPP-RTK uncombined and short-baseline DD the higher-order term mostly cancels in the SD / DD differencing, so this flag is closer to a consistency knob than a bias remover.  Default off.

        // --- Relativistic / propagation ---
        bool enable_relativistic_correction{ true };  ///< Periodic relativistic clock correction (-2 r�v / c�).
        bool enable_shapiro_delay{ true };             ///< Shapiro (gravitational path) delay.

        // --- Geophysical site displacements ---
        bool enable_solid_earth_tide{ true };          ///< IERS 2010 solid Earth tide displacement.
        bool enable_ocean_tide_loading{ true };        ///< Ocean tidal loading displacement (BLQ).
        bool enable_pole_tide{ true };                 ///< Pole tide displacement (from ERP).
        bool enable_atmospheric_loading{ true };       ///< Atmospheric pressure loading displacement.

        // --- Antenna phase centre ---
        bool enable_satellite_pco{ true };             ///< Satellite antenna phase centre offset.
        bool enable_satellite_pcv{ true };             ///< Satellite antenna phase centre variation.
        bool enable_receiver_pco{ true };              ///< Receiver antenna phase centre offset.
        bool enable_receiver_pcv{ true };              ///< Receiver antenna phase centre variation.
        std::string receiver_antenna_type_override{ "" };  ///< Receiver antenna type when no RINEX header supplies one (live SBF/RTCM). Format "MODEL RADOME", e.g. "LEIAR25         NONE". Empty = use RINEX header (which always wins). Real-time path only.
        bool enable_receiver_cross_system_atx_fallback{ false };  ///< RECEIVER-ONLY: when the receiver ATX entry lacks a non-GPS frequency, substitute the nearest GPS band (E1->G01, E5a->G05, B1C->G01, B2a->G05, ...). Valid: receiver phase pattern is frequency-dependent on shared hardware. NEVER applied to the satellite antenna (E01 and G01 are different spacecraft). Opt-in.
        bool enable_atx_frequency_fallback{ true };    ///< When the ATX entry doesn't have PCO/PCV for the requested frequency (common for L5 / E5* / B2a on older satellites or receiver antennas), look up the nearest available frequency on the same antenna and use its values instead of silently skipping the correction.  Cross-frequency PCO drift on a single physical antenna is typically <1 cm — better than the few-cm vertical residual that comes from skipping a frequency entirely.  Set false to revert to the legacy "skip if missing" behavior for A/B comparison.
        bool require_atx_coverage{ false };            ///< When true, drop observations whose (sat, signal) lacks complete ATX coverage on **both** the satellite and receiver entries — i.e., satellite PCO+PCV and receiver PCO+PCV must all resolve to non-empty entries for the row's exact frequency, *without* @c enable_atx_frequency_fallback being used to substitute a neighbouring frequency.  Default false (legacy behaviour: observations are kept and any missing PCO/PCV is silently zeroed).  Set true when you've explicitly chosen `signal_mode: "all"` and want the ATX file to be the authoritative whitelist, dropping rows that would otherwise process without proper antenna calibration.
        bool require_atx_coverage_for_ar{ false };     ///< When true, AR (WL constraints, LAMBDA NL search) skips (sat, signal) pairs lacking strict ATX coverage even though the underlying observations are still used in the float Kalman update.  Softer counterpart to @c require_atx_coverage: lets you keep "all"-signal float estimation alive while restricting integer fixing to the ATX-curated subset.  Default false (no AR-side gating).
        bool restrict_ar_for_substituted_atx{ false }; ///< When true, AR additionally skips (sat, signal) pairs whose ATX correction was computed via the frequency-fallback substitution chain (rather than an exact entry).  Targets specifically the cases where the antenna model is approximate; signals lacking ATX entirely (no substitution applied) are still allowed.  Use alongside or instead of @c require_atx_coverage_for_ar depending on which kind of imperfection you want to gate on.  Default false.
        bool loosen_amb_q_for_substituted_atx{ false }; ///< When true, ambiguity states whose underlying (sat, signal) had their ATX correction frequency-fallback-substituted use a looser process noise (@c ambiguity_q_substituted_atx) instead of the standard @c process_noise.ambiguity.  Compensates for the small but non-zero modeling error introduced by the substituted PCV/PCO.  Default false.
        double ambiguity_q_substituted_atx{ 1e-12 };   ///< Process noise (m²/s) applied to ambiguity states for substituted-ATX (sat, signal) pairs when @c loosen_amb_q_for_substituted_atx is true.  The PCV/PCO substitution does not inject *random* noise — it introduces a **deterministic, slowly-varying bias** in the LOS direction, driven by the mismatch between the requested-frequency antenna pattern and the substituted-frequency pattern (e.g. L2 standing in for L5).  That mismatch projects onto LOS as the satellite sweeps elevation/azimuth across the arc.  Order-of-magnitude estimate: a ~10 mm PCO_Up substitution mismatch combined with an MEO elevation rate of ~1.5e-4 rad/s yields a LOS bias drift of ~1 μm/s at mid-elevation, i.e. Q ≈ (1 μm/s)² · 1 s ≈ 1e-12 m²/s.  This coincides with the standard @c process_noise.ambiguity default — the correct conclusion that a well-formed substitution adds negligible Q on top of the baseline ambiguity random walk.  The override therefore exists as a deliberate "I've audited the substituted-ATX bias and the standard Q is sized to absorb it" affirmation rather than a noise-injection knob; raise to ~1e-11 only if you have direct evidence (e.g. an arc-correlated residual signature on substituted signals) that the standard Q is too tight.  Values above ~1e-10 will start leaking real estimation strength out of the ambiguity into other states.
        double ambiguity_std_substituted_atx{ 1e3 };   ///< Initial ambiguity standard deviation (m) for newly-seeded ambiguities whose (sat, signal) had their ATX correction frequency-fallback-substituted.  Replaces @c getAmbiguityStd() at addAmbiguity time when @c loosen_amb_q_for_substituted_atx is true.  Default 1e3 — same scale as the standard ambiguity init; raise to 5e3 or larger if you want the filter to be especially loose on substituted-ATX seeds because the substituted PCV can introduce a per-epoch sub-cycle bias that the float ambiguity has to absorb.
        double atx_substitution_phase_std_m{ 0.0 };    ///< Zenith standard deviation (m) of an **additive** carrier-phase measurement-noise term applied to phase rows whose receiver ATX correction was frequency-fallback-substituted (@c wasAtxSubstituted true).  Summed in quadrature onto the stochastic-model phase variance as @f$(\sigma/\sin\,\mathrm{el})^2@f$ *before* any iono-free combination (so the IF coefficients propagate it).  @c 0 disables — default off, opt-in like the other substituted-ATX knobs.  **Complementary to, not a duplicate of, @c ambiguity_q_substituted_atx.**  That knob lets the per-(sat, signal) ambiguity random walk absorb the *per-satellite, slowly-varying* LOS bias of a substituted PCO/PCV (which does not bias coordinates).  It cannot absorb the *azimuthally-symmetric, elevation-dependent* receiver-PCV residual, which is **common across satellites** at a given elevation and therefore aliases into the elevation-structured states — ZWD and station height — like a small tropo mismodel.  Tightening per-signal phase σ (e.g. Galileo E5 alt-BOC at sub-mm) raises the Kalman gain on exactly those rows, so without this term the unmodeled PCV residual leaks into the vertical with high confidence.  This inflation down-weights substituted phase to an honest effective σ while leaving properly-calibrated signals at full earned precision; elevation amplification is @f$1/\sin^2@f$ to match the phase thermal term and because the PCV residual and its vertical aliasing are both worst at low elevation.  Recommended ~0.003 (3 mm) for a fallback between neighbouring GNSS bands; raise toward 0.005–0.010 for a large inter-frequency PCO gap or if you observe an elevation-correlated phase-residual / ZWD signature on substituted signals.
        bool apply_phase_shifts{ false };              ///< When true, RINEX phase observations have the receiver-applied @c "SYS / PHASE SHIFT" values subtracted at parse time, recovering the unshifted phase convention.  Default false because most modern BIA products (CODE, CAS, GFZ) are calibrated against the receiver-shifted phases as recorded in RINEX, so leaving the shifts in place is the standard PPP convention.  Set true only when your bias product is explicitly calibrated for the unshifted convention, or when running without external BIA and using in-house bias estimation that benefits from a uniform shift-free input.

        // --- Carrier-phase ---
        bool enable_phase_windup{ true };              ///< Carrier-phase wind-up correction.

        // --- Product biases ---
        bool enable_code_phase_bias{ true };            ///< Code/phase bias corrections from BIA product files.

        // --- Processing strategy ---
        bool refine_emission_time{ true };              ///< When true, @c Geometry::computeSatelliteGeometry runs a
                                                        ///< second pass through @c computeEmissionState with the
                                                        ///< slant tropospheric ZHD and IF-combined satellite + receiver
                                                        ///< ATX LOS projection subtracted from the seed pseudorange.
                                                        ///< This drives the residual t_em error from tens of ns (driven
                                                        ///< by the ~3 m tropo + ~1-7 m PCO content of the raw pseudorange)
                                                        ///< down to ~ns / ~μm of sat displacement.  Cost: one extra
                                                        ///< Neville orbit interp + clock interp + 1-2 ATX evaluations
                                                        ///< per sat per epoch (~15-20% overall per-epoch overhead).
                                                        ///< Set false to disable the refinement pass entirely — useful
                                                        ///< for A/B comparison or when chasing artifacts that may be
                                                        ///< sensitive to the second-pass logic.  Independent of
                                                        ///< @c enable_troposphere and the antenna model: when this
                                                        ///< flag is true the refinement only adds whichever components
                                                        ///< (tropo, ATX) are individually enabled / wired in.
    };

    /**
     * @brief Configuration for automatic data download and local caching.
     *
     * When enabled, the application automatically downloads satellite orbit,
     * clock, bias, ERP and broadcast navigation products from CDDIS, and
     * optionally observation RINEX data.  Previously downloaded files are
     * reused from the local cache.
     */
    struct DownloadConfig {
        bool enabled{ false };                       ///< Master enable for auto-download.
        std::filesystem::path data_root;             ///< Local cache root directory.
        std::string product_source{ "COD" };         ///< 3-char analysis centre prefix.
        std::string product_type{ "FIN" };            ///< Product latency: FIN, RAP, ULT.
        std::filesystem::path netrc_path;            ///< Path to .netrc file (empty = default).
        std::string station;                         ///< 4- or 9-char station name for RINEX download.
        std::string station_country{ "USA" };         ///< 3-char country code.
        int year{ 0 };                               ///< Observation year (0 = auto-detect).
        int doy{ 0 };                                ///< Observation day-of-year (0 = auto-detect).
        bool download_rinex{ false };                ///< Download observation RINEX from CDDIS.
        bool download_products{ true };              ///< Download SP3/CLK/BIA/ERP/BRDC.
        bool download_gim{ false };                  ///< Download the IONEX GIM (global ionosphere map).
        std::filesystem::path atx_path;              ///< Path to local ATX file (not downloaded).
        std::filesystem::path nc_path;               ///< Path to local ocean-loading NC file.
        std::filesystem::path crx2rnx_path;          ///< Path to crx2rnx executable for Hatanaka decompression.
        bool keep_zip_after_unzip{ false };          ///< If true, keep the .gz after decompression. Default false (delete to save disk).
    };

    /**
     * @brief Configuration for spdlog file loggers and CSV trace outputs.
     *
     * Each flag controls whether the corresponding log file or trace CSV
     * is created and written at runtime.  Defaults are all-on so that
     * disabling output requires an explicit opt-out in the JSON config.
     */
    struct LoggingConfig {
        // --- spdlog file loggers (.log) ---
        bool enable_main_log{ true };              ///< logs/main_log.log
        bool enable_gnss_config{ true };            ///< logs/gnss_config.log
        bool enable_kalman_filter{ true };          ///< logs/kalman_filter.log
        bool enable_smoother{ true };               ///< logs/smoother.log
        bool enable_state_manager{ true };          ///< logs/state_manager.log
        bool enable_ppp_model{ true };              ///< logs/ppp_model.log
        bool enable_measurement_model{ true };      ///< logs/measurement_model.log
        bool enable_mw_slip_detector{ true };       ///< logs/mw_slip_detector.log
        bool enable_if_slip_detector{ true };       ///< logs/if_slip_detector.log
        bool enable_gf_slip_detector{ true };       ///< logs/gf_slip_detector.log
        bool enable_turbo_edit_resolver{ true };    ///< logs/turbo_edit_resolver.log
        bool enable_cycle_slip_manager{ true };     ///< logs/cycle_slip_manager.log
        bool enable_ambiguity_resolver{ true };     ///< logs/ambiguity_resolver.log
        bool enable_corrections{ true };            ///< logs/corrections.log
        bool enable_geometry{ true };               ///< logs/geometry.log
        bool enable_troposphere{ true };            ///< logs/troposphere.log
        bool enable_ionosphere{ true };             ///< logs/ionosphere.log
        bool enable_antenna{ true };                ///< logs/antenna.log
        bool enable_antenna_model{ true };          ///< logs/antenna_model.log
        bool enable_ocean_tide{ true };             ///< logs/ocean_tide.log
        bool enable_planetary_model{ true };        ///< logs/planetary_model.log
        bool enable_stochastic_model{ true };       ///< logs/stochastic_model.log
        bool enable_rinex_parser{ true };           ///< logs/rinex_parser.log
        bool enable_product_parser{ true };         ///< logs/product_parser.log
        bool enable_product_cache{ true };          ///< logs/product_cache.log
        bool enable_resolver{ true };               ///< logs/resolver.log
        bool enable_state_initializer{ true };      ///< logs/state_initializer.log
        bool enable_bssd_pivot_manager{ true };     ///< logs/bssd_pivot_manager.log
        bool enable_bssd_transform{ true };         ///< logs/bssd_transform.log
        bool enable_ppp_utils{ true };              ///< logs/ppp_utils.log
        bool enable_downloader{ true };             ///< logs/downloader.log
        bool enable_ntrip{ true };                  ///< logs/ntrip.log
        bool enable_rt_engine{ true };              ///< logs/rt_engine.log
        bool enable_imu{ true };                    ///< logs/imu.log

        // --- CSV trace files ---
        bool enable_state_trace{ true };            ///< logs/state_trace.csv
        bool enable_ambiguity_trace{ true };        ///< logs/ambiguity_trace.csv
        bool enable_ionosphere_trace{ true };       ///< logs/ionosphere_trace.csv
        bool enable_sat_bias_trace{ true };         ///< logs/sat_bias_trace.csv
        bool enable_sat_status_trace{ true };       ///< logs/satellite_status_trace.csv
        bool enable_dop_trace{ true };              ///< logs/dop_trace.csv
        bool enable_residual_trace{ true };         ///< logs/measurement_residuals.csv
        bool enable_wls_trace{ true };              ///< logs/wls_trace.csv
        bool enable_smoother_trace{ true };         ///< logs/smoother_trace.csv
        bool enable_mw_slip_trace{ true };          ///< logs/mwwl_trace.csv
        bool enable_if_slip_trace{ true };          ///< logs/if_slip_trace.csv
        bool enable_gf_slip_trace{ true };          ///< logs/gf_slip_trace.csv
        bool enable_turbo_edit_trace{ true };       ///< logs/turbo_edit_trace.csv
        bool enable_windup_trace{ false };          ///< logs/windup_trace.csv (phase wind-up diagnostic; off by default)

        /**
         * @brief Concise per-epoch logging for the Kalman-filter logger.
         *
         * When @c true, the @c kalman_filter logger only emits the FIRST
         * and LAST measurement-update pass within each epoch (regardless
         * of whether outliers were found). All intermediate rebuild
         * passes are suppressed.  Lets the user keep tight phase noise +
         * aggressive outlier rejection without the per-pass log volume
         * blowing up — typical reduction is 5-10× on outlier-heavy runs.
         *
         * Pass-1 messages are written immediately at their original
         * level; intermediate passes are buffered into a per-pass slot
         * and discarded; the final pass's slot is replayed when the
         * caller signals end-of-epoch.  No call-site changes — the
         * gate is implemented as a sink wrapper around the existing
         * @c kalman_filter sinks, applied at startup.
         */
        bool kalman_filter_concise_log{ false };

        /**
         * @brief Look up whether a given spdlog logger name is enabled.
         * @param name Logger name (e.g. "kalman_filter").
         * @return true if the logger should produce file output.
         */
        bool isLoggerEnabled(const std::string& name) const;

        /**
         * @brief Per-logger severity overrides parsed from JSON @c logging.levels.
         *
         * Map of logger name → LogLevel. Loggers absent from this map fall
         * back to the global level set in @c LogManager::initialize() (Info
         * by default). Disabled loggers (via the @c enable_X flags above)
         * take precedence over any entry here.
         *
         * Example JSON:
         * @code
         * "logging": {
         *   "levels": {
         *     "kalman_filter": "debug",
         *     "geometry": "warn",
         *     "ppp_model": "info"
         *   }
         * }
         * @endcode
         */
        std::map<std::string, LogLevel> per_logger_levels;
    };

} // namespace GnssProcessor
