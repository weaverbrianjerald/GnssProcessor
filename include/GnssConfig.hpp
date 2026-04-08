#pragma once

#include "LogManager.hpp"
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
        PPP,    ///< Precise Point Positioning mode.
        PPP_RTK ///< Precise Point Positioning with Real-Time Kinematic enhancements.
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
        Constant,   ///< Use the a-priori ?² directly, no geometry scaling.
        Elevation,  ///< Classic 1/sin²(el) weighting (default).
        SNR         ///< SNR-based weighting using the SIGMA-? model.
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
        /// uncombined mode. Scaled to slant by 1/sin²(el) per satellite.
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
    };

    /**
     * @brief Struct for default measurement noise parameters.
     *
     * Provides fallback noise values when RINEX does not specify variances.
     */
    struct DefaultMeasurementNoise {
        double code_std_m{ 0.30 };   ///< Code measurement standard deviation (m).
        double phase_std_m{ 0.003 }; ///< Phase measurement standard deviation (m).
        double doppler_std_m{ 0.5 }; ///< Doppler measurement standard deviation (Hz).
    };

    /**
     * @brief Parameters for the SNR-based stochastic (SIGMA-?) model.
     *
     * The model computes measurement standard deviation as:
     *   ? = a + b · 10^(?SNR_dBHz / 10)
     * where SNR_dBHz is the signal-to-noise ratio in dB-Hz read from RINEX
     * 'S' observables.  The parameters @c a and @c b are specified
     * separately for code and phase measurements.
     *
     * When SNR is unavailable for a measurement the model falls back to
     * the classic elevation-based 1/sin²(el) weighting using the default
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

        /// Accelerometer noise density (m/s²/?Hz).  Typical MEMS: 0.01–0.1.
        double accel_noise_density{ 0.05 };
        /// Gyroscope noise density (rad/s/?Hz).  Typical MEMS: 1e-4–1e-3.
        double gyro_noise_density{ 5e-4 };
        /// Accelerometer bias instability (m/s³/?Hz, random-walk spectral density).
        double accel_bias_instability{ 1e-4 };
        /// Gyroscope bias instability (rad/s²/?Hz, random-walk spectral density).
        double gyro_bias_instability{ 1e-6 };

        /// Initial accelerometer bias standard deviation (m/s²).
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
        bool enable_relativistic_correction{ true };  ///< Periodic relativistic clock correction (-2 r·v / c²).
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
         * fractional-cycle geometric angle at each epoch, bounded to ±half cycle.
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

    private:
        ProcessingMode processing_mode_{ ProcessingMode::PPP }; ///< Processing mode (PPP or PPP-RTK).
        SignalMode signal_mode_{ SignalMode::Dual };            ///< Signal mode (Dual, Triple, All).
        SatelliteInterpolationMode satellite_interpolation_mode_{ SatelliteInterpolationMode::Lagrange }; ///< Satellite position/velocity interpolation mode (Lagrange, Finite, Hermite, Spline).
        SatelliteClockInterpolationMode satellite_clock_interpolation_mode_{ SatelliteClockInterpolationMode::Linear }; ///< Satellite clock interpolation mode (Linear, Lagrange).
        bool uncombined_enabled_{ false };                      ///< Enable uncombined processing.
        double elevation_mask_deg_{ 10.0 };                    ///< Elevation mask in degrees.
        ReceiverAntennaOrientation receiver_antenna_orientation_; ///< Antenna orientation parameters.
        ProcessNoise process_noise_;                          ///< Process noise parameters.
        InitialStd initial_std_;                              ///< Initial state uncertainties.
        DefaultMeasurementNoise default_meas_noise_;          ///< Default measurement noise (fallback).
        std::map<std::string, std::vector<std::string>> signal_priorities_; ///< Signal priorities per system.
        std::vector<std::string> active_systems_ = { "GPS", "Galileo", "GLONASS", "BeiDou", "QZSS", "SBAS" }; ///< Active GNSS systems.
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
        bool estimate_phase_clock_{ false };            ///< Estimate separate phase clock offset.
        bool enable_if_slip_detector_{ true };             ///< Enable the IF phase slip detector.
        bool accumulate_phase_windup_{ false };                ///< Accumulated (unwrapped) phase wind-up for IAR readiness.
        bool enable_between_satellite_sd_{ false };                ///< Between-satellite single-difference operator in measurement model.
        bool estimate_ifb_{ true };                                    ///< Estimate receiver inter-frequency bias (IFB) for non-reference code bands.
        bool estimate_velocity_{ false };                           ///< Estimate receiver velocity from Doppler observations.

        bool has_reference_position_{ false };                          ///< True if user supplied a reference ECEF position.
        std::array<double, 3> reference_position_ecef_{ 0.0, 0.0, 0.0 }; ///< User-supplied reference position ECEF (m).

        ModelCorrections model_corrections_;                            ///< Enable/disable flags for model corrections.
        StochasticModelType stochastic_model_{ StochasticModelType::Elevation }; ///< Measurement stochastic model selection.
        SnrModelParams snr_model_params_;                                ///< SNR-based model parameters.
        ImuConfig imu_config_;                                           ///< IMU integration configuration.
        DownloadConfig download_config_;                                  ///< Data download and caching configuration.
        AppMode app_mode_{ AppMode::PostProcessing };                    ///< Top-level execution mode.
        RealTimeConfig realtime_config_;                                  ///< Real-time streaming configuration.

        // --- Ambiguity Resolution ---
        int    ar_mode_{ 0 };                  ///< AR mode: 0=float, 1=continuous, 2=instantaneous, 3=fix-and-hold.
        double ar_ratio_threshold_{ 3.0 };     ///< LAMBDA ratio test threshold for acceptance.
        double ar_elevation_mask_{ 20.0 };     ///< Elevation mask for AR candidates (degrees).
        double ar_hold_variance_{ 0.001 };     ///< Pseudo-obs variance for fix-and-hold (m^2).
    };

} // namespace GnssProcessor
