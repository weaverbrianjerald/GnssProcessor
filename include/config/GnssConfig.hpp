#pragma once


#include "logging/LogManager.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <set>

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
        std::string input_type{ "ntrip" };        ///< Input source: "ntrip", "tcp", "serial", "file".
        std::string input_path;                   ///< Path or address for non-NTRIP input.
        std::string output_type{ "console" };     ///< Output destination: "console", "file", "nmea".
        std::string output_path;                  ///< Path for file-based output.
        size_t      max_epoch_buffer{ 100 };      ///< Max epochs to buffer before dropping oldest.
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
     * @brief Enumeration for signal processing modes.
     *
     * Specifies the number of signals to use for measurements.
     */
    enum class SignalMode { 
        Dual,   ///< Use dual-frequency signals.
        Triple, ///< Use triple-frequency signals.
        All     ///< Use all available signals.
    };

    /**
     * @brief Enumeration for MW slip detector signal selection.
     *
     * Controls which frequency bands the Melbourne-Wubbena slip detector
     * uses to form dual-frequency MW pairs.
     */
    enum class MwSignalMode {
        Filter, ///< Use only the bands selected by the KF signal_mode (default).
        All     ///< Use all available RINEX bands (raw_observed).
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
        Linear,   ///< Linear interpolation.
        Lagrange  ///< Lagrange interpolation.
    };

    /**
     * @brief Enumeration for measurement stochastic (weighting) models.
     *
     * Selects how observation variances are scaled based on satellite geometry
     * or signal quality indicators.
     */
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

        /// Carrier phase ambiguity (m*m/s). Zero for constant-ambiguity
        /// float model (standard).
        double ambiguity{ 0.0 };

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

        /// Receiver velocity (m*m/s). Models the expected velocity variation
        /// between epochs.  A large value (~1e2) treats velocity as nearly
        /// unconstrained, suitable for pedestrian or vehicular dynamics.
        /// Smaller values smooth the velocity estimate over time.
        double velocity{ 1e2 };

        /// Per-satellite signal bias (m*m/s).  Absorbs uncorrected satellite
        /// hardware delays on signals without BIA product coverage.  Nearly
        /// constant over hours; 1e-10 allows ~0.01 mm/sqrt(30 s) drift.
        double sat_bias{ 1e-10 };
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
     * @brief Parameters for the SNR-based stochastic (SIGMA-?) model.
     *
     * The model computes measurement standard deviation as:
     *   ? = a + b � 10^(?SNR_dBHz / 10)
     * where SNR_dBHz is the signal-to-noise ratio in dB-Hz read from RINEX
     * 'S' observables.  The parameters @c a and @c b are specified
     * separately for code and phase measurements.
     *
     * When SNR is unavailable for a measurement the model falls back to
     * the classic elevation-based 1/sin�(el) weighting using the default
     * measurement noise values.
     *
     * Reference: Brunner F.K., Hartinger H., Troyer L. (1999),
     *            "GPS signal diffraction modelling: the stochastic
     *            SIGMA-? model", Journal of Geodesy.
     */
    struct SnrModelParams {
        double code_a{ 0.30 };        ///< Additive floor for code ? (meters).
        double code_b{ 300.0 };       ///< Multiplier for code exponential term (meters).
        double phase_a{ 0.002 };      ///< Additive floor for phase ? (meters).
        double phase_b{ 0.3 };        ///< Multiplier for phase exponential term (meters).
        double snr_floor_dbhz{ 20.0 };///< Minimum SNR (dB-Hz) to clamp before computing ?.
        double snr_ceil_dbhz{ 55.0 }; ///< Maximum SNR (dB-Hz) to clamp before computing ?.
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
        /// Zenith code noise per block type (meters).  Key is a substring
        /// that will be matched against the ATX type field (e.g. "BLOCKIII",
        /// "BLOCKIIF", "GALILEO").  First match wins.
        std::vector<std::pair<std::string, double>> block_code_std_m{
            {"BLOCKIIIA", 0.10},
            {"BLOCKIII",  0.10},
            {"BLOCKIIR",  0.20},
            {"BLOCKIIF",  0.15},
            {"BLOCKII",   0.30},
            {"GALILEO",   0.10},
            {"BEIDOU-3",  0.15},
            {"BEIDOU",    0.25},
            {"GLONASS-M+",0.20},
            {"GLONASS-K", 0.15},
            {"GLONASS",   0.30}
        };

        /// Zenith phase noise per block type (meters).  Same matching rules.
        std::vector<std::pair<std::string, double>> block_phase_std_m{
            /*{"BLOCKIIIA", 0.001},
            {"BLOCKIII",  0.001},
            {"BLOCKIIR",  0.002},
            {"BLOCKIIF",  0.0015},
            {"BLOCKII",   0.003},
            {"GALILEO",   0.001},
            {"BEIDOU-3",  0.0015},
            {"BEIDOU",    0.002},
            {"GLONASS-K", 0.0015},
            {"GLONASS",   0.003}*/
            // Phase noise is quite consistent across blocks, so we use the same values for all to avoid overfitting to the limited ATX types.
            // TODO read from config phase_std_m because nothing is changed for the phase data
            {"BLOCKIIIA", 0.001},
            {"BLOCKIII",  0.001},
            {"BLOCKIIR",  0.001},
            {"BLOCKIIF",  0.001},
            {"BLOCKII",   0.001},
            {"GALILEO",   0.001},
            {"BEIDOU-3",  0.001},
            {"BEIDOU",    0.001},
            {"GLONASS-M+",0.001},
            {"GLONASS-K", 0.001},
			{"GLONASS",   0.001}
        };

        /// Per-frequency-band scale factor applied to the zenith noise.
        /// The frequency band digit (second character of the RINEX signal
        /// code, e.g. '1' in "L1W") determines the scale.
        /// Missing bands default to 1.0.
        std::map<char, double> freq_band_scale{
            {'1', 1.0},
            {'2', 1.0},
            {'5', 0.5},
            {'6', 0.8},
            {'7', 0.7},
            {'8', 0.6}
        };

        /// Fallback code noise when no block match is found (meters).
        double default_code_std_m{ 0.30 };
        /// Fallback phase noise when no block match is found (meters).
        double default_phase_std_m{ 0.002 };
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

        // --- Carrier-phase ---
        bool enable_phase_windup{ true };              ///< Carrier-phase wind-up correction.

        // --- Product biases ---
        bool enable_code_phase_bias{ true };            ///< Code/phase bias corrections from BIA product files.
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
        bool enable_residual_trace{ true };         ///< logs/measurement_residuals.csv
        bool enable_wls_trace{ true };              ///< logs/wls_trace.csv
        bool enable_smoother_trace{ true };         ///< logs/smoother_trace.csv
        bool enable_mw_slip_trace{ true };          ///< logs/mwwl_trace.csv
        bool enable_if_slip_trace{ true };          ///< logs/if_slip_trace.csv
        bool enable_gf_slip_trace{ true };          ///< logs/gf_slip_trace.csv

        /**
         * @brief Look up whether a given spdlog logger name is enabled.
         * @param name Logger name (e.g. "kalman_filter").
         * @return true if the logger should produce file output.
         */
        bool isLoggerEnabled(const std::string& name) const;
    };

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
        GnssConfig() = default;

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
         * @brief Gets the processing mode.
         * @return Current processing mode (PPP or PPP-RTK).
         */
        ProcessingMode getProcessingMode() const { return processing_mode_; }

        /**
         * @brief Gets the signal mode.
         * @return Current signal mode (Dual, Triple, or All).
         */
        SignalMode getSignalMode() const { return signal_mode_; }

        /**
         * @brief Gets the MW slip detector signal selection mode.
         *
         * Controls whether the MW detector forms pairs from the KF-selected
         * bands only (Filter, default) or from all available RINEX bands (All).
         *
         * @return Current MwSignalMode.
         */
        MwSignalMode getMwSignalMode() const { return mw_signal_mode_; }

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
         * @return Current StochasticModelType (Constant, Elevation or SNR).
         */
        StochasticModelType getStochasticModel() const { return stochastic_model_; }

        /**
         * @brief Gets the SNR-based stochastic model parameters.
         * @return Const reference to the SnrModelParams struct.
         */
        const SnrModelParams& getSnrModelParams() const { return snr_model_params_; }

        /**
         * @brief Check if elevation-based variance scaling is applied to phase observations.
         *
         * When true (the default), carrier-phase measurement variance is
         * scaled by @c 1/sin²(el) under the Elevation and SatelliteBlock
         * models, and by the elevation fallback inside the SNR model when
         * no @c 'S' observable is available.
         *
         * When false, phase observations are returned at their unscaled
         * @c default_var (or block-specific zenith noise under
         * SatelliteBlock).  This matches empirical behavior of modern
         * receivers where phase tracking-loop noise is nearly elevation-
         * independent above ~10°; the apparent low-elevation "noise" is
         * usually multipath bias rather than zero-mean noise.
         *
         * Disabling phase elevation weighting keeps standardized-residual
         * outlier / cycle-slip detection usable at low elevations — at
         * 10° an @c 1/sin²(el) inflation drops a 1-cycle L1 slip from
         * ~95σ to ~3σ, below typical rejection thresholds.
         *
         * Code observations are unaffected by this flag.
         *
         * @return True if phase observations receive @c 1/sin²(el) scaling.
         */
        bool phaseElevationWeighting() const { return phase_elevation_weighting_; }

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
         * remain at a constant integer value.  Note: accumulated mode has
         * known interactions with ambiguity re-seeding and slip-detector
         * resets that can introduce position biases until those paths are
         * updated to be accumulation-aware.
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

        ProcessingMode processing_mode_{ ProcessingMode::PPP }; ///< Processing mode (PPP or PPP-RTK).
        SignalMode signal_mode_{ SignalMode::Dual };            ///< Signal mode (Dual, Triple, All).
        MwSignalMode mw_signal_mode_{ MwSignalMode::Filter };  ///< MW detector signal selection (Filter or All).
        SatelliteInterpolationMode satellite_interpolation_mode_{ SatelliteInterpolationMode::Lagrange }; ///< Satellite position/velocity interpolation mode (Lagrange, Finite, Hermite, Spline).
        SatelliteClockInterpolationMode satellite_clock_interpolation_mode_{ SatelliteClockInterpolationMode::Linear }; ///< Satellite clock interpolation mode (Linear, Lagrange).
        bool uncombined_enabled_{ false };                      ///< Enable uncombined processing.
        double elevation_mask_deg_{ 10.0 };                    ///< Elevation mask in degrees.
        double data_gap_tolerance_s_{ 0.0 };                     ///< Data gap tolerance (s); 0 = use measurement rate.
        double gnss_rate_s_{ 0.0 };                              ///< Effective processing rate (s); 0 = process every RINEX epoch.
        ReceiverAntennaOrientation receiver_antenna_orientation_; ///< Antenna orientation parameters.
        ProcessNoise process_noise_;                          ///< Process noise parameters.
        InitialStd initial_std_;                              ///< Initial state uncertainties.
        DefaultMeasurementNoise default_meas_noise_;          ///< Default measurement noise (fallback).
        std::map<std::string, std::vector<std::string>> signal_priorities_; ///< Signal priorities per system.
        std::vector<std::string> active_systems_ = { "GPS", "Galileo", "GLONASS", "BeiDou", "QZSS", "SBAS" }; ///< Active GNSS systems.
        std::vector<std::string> excluded_satellites_;                ///< Satellites excluded from processing (e.g. "E14", "E18").
        std::filesystem::path product_list_path_;            ///< Path to product list file.
        std::vector<std::filesystem::path> product_paths_;   ///< Product file paths (SP3, CLK, etc.).
        std::filesystem::path observation_path_;             ///< Path to observation RINEX file.
        std::filesystem::path navigation_path_;              ///< Path to navigation RINEX file.

        double zhd_pressure_mbar = 1013.25;  ///< Zenith hydrostatic delay pressure (mbar).
        double zhd_temp_kelvin = 288.15;     ///< Zenith hydrostatic delay temperature (Kelvin).

        double leap_seconds_tai_ = 37.0;  ///< Number of leap seconds (TAI-UTC=37) since 2017-01-01.

        double residual_threshold_{ 6.0 };      ///< Residual threshold for outlier detection.
        double bad_meas_percent_{ 0.5 };        ///< Percentage of bad measurements allowed.
        double ambiguity_std_{ 1e3 };           ///< Ambiguity standard deviation.
        double code_noise_amplify_{ 1e4 };      ///< Code noise amplification factor.

        std::filesystem::path otl_blq_path_;    ///< Path to BLQ ocean loading file (optional).

        bool estimate_tropo_gradients_{ false };   ///< Estimate troposphere horizontal gradients (Gn, Ge).
        bool relinearize_geometry_{ false };            ///< Re-run computeModels after first KF update to recompute geometry at updated position.
        bool run_smoother_{ false };                    ///< Run backward RTS smoother after forward Kalman pass (post-processing only).
        bool estimate_phase_clock_{ false };            ///< Estimate separate phase clock offset.
        bool enable_if_slip_detector_{ false };             ///< Enable the IF phase slip detector (experimental, disabled by default).
        bool accumulate_phase_windup_{ false };                ///< Accumulated (unwrapped) phase wind-up for IAR readiness.
        bool enable_between_satellite_sd_{ false };                ///< Between-satellite single-difference operator in measurement model.
        double bssd_pivot_min_elevation_deg_{ 45.0 };              ///< Elevation threshold below which the BSSD pivot is handed over (degrees).
        bool estimate_ifb_{ false };                                    ///< Estimate receiver inter-frequency bias (IFB) for non-reference code bands.
        bool lock_signals_to_clock_ref_{ false };                        ///< Restrict uncombined PPP-RTK to clock-reference frequency bands only.
        bool require_bia_for_signals_{ false };                           ///< Only use signals with BIA product coverage.
        bool estimate_sat_biases_{ false };                               ///< Estimate per-satellite bias for signals without BIA coverage.
        double no_bia_code_downweight_{ 100.0 };                            ///< Variance multiplier for code on signals without BIA coverage.
        bool estimate_velocity_{ false };                           ///< Estimate receiver velocity from Doppler observations.

        bool has_reference_position_{ false };                          ///< True if user supplied a reference ECEF position.
        std::array<double, 3> reference_position_ecef_{ 0.0, 0.0, 0.0 }; ///< User-supplied reference position ECEF (m).

        ModelCorrections model_corrections_;                            ///< Enable/disable flags for model corrections.
        StochasticModelType stochastic_model_{ StochasticModelType::Elevation }; ///< Measurement stochastic model selection.
        bool phase_elevation_weighting_{ true };                                ///< Apply 1/sin²(el) scaling to carrier-phase observations.
        SnrModelParams snr_model_params_;                                ///< SNR-based model parameters.
        SatBlockNoiseParams sat_block_noise_params_;                      ///< Satellite-block stochastic model parameters.
        ImuConfig imu_config_;                                           ///< IMU integration configuration.
        DownloadConfig download_config_;                                  ///< Data download and caching configuration.
        DataSource data_source_{ DataSource::Internal };                  ///< Selects internal-data vs production-download mode.
        std::filesystem::path internal_data_root_;                        ///< Absolute path to the internal data tree (resolved at load time).
        AppMode app_mode_{ AppMode::PostProcessing };                    ///< Top-level execution mode.
        RealTimeConfig realtime_config_;                                  ///< Real-time streaming configuration.
        BaselineConfig baseline_config_;                                  ///< Baseline / network processing configuration (used when isBaselineMode()).

        // --- Ambiguity Resolution ---
        int    ar_mode_{ 0 };                  ///< AR mode: 0=float, 1=continuous, 2=instantaneous, 3=fix-and-hold.
        double ar_ratio_threshold_{ 3.0 };     ///< LAMBDA ratio test threshold for acceptance.
        double ar_elevation_mask_{ 20.0 };     ///< Elevation mask for AR candidates (degrees).
        double ar_hold_variance_{ 0.001 };     ///< Pseudo-obs variance for fix-and-hold (m^2).

        LoggingConfig logging_config_;             ///< Logger and trace file enable/disable flags.
    };

} // namespace GnssProcessor
