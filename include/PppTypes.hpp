// PppTypes.hpp
#pragma once

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <Eigen/Dense>
#include "TimeUtils.hpp"
#include "Constants.hpp"
#include "LogManager.hpp"

namespace GnssProcessor {

    /**
     * @brief Enumeration of supported GNSS constellations/systems.
     */
    enum class System {
        GPS,      /**< Global Positioning System (USA) */
        Galileo,  /**< Galileo (EU) */
        GLONASS,  /**< GLONASS (Russia) */
        BeiDou,   /**< BeiDou (China) */
        QZSS,     /**< QZSS (Japan) */
        SBAS,     /**< Satellite-Based Augmentation Systems (regional) */
        Unknown   /**< Unknown or unsupported system */
    };

    /**
     * @brief Enumeration of GNSS and auxiliary measurement types.
     *
     * Used to classify measurement rows so that the measurement model,
     * Jacobian builder and noise assignment can dispatch on the observable
     * category rather than parsing the first character of RINEX codes.
     */
    enum class MeasurementType {
        Code,       /**< Pseudorange (code) measurement (RINEX 'C' observables). */
        Phase,      /**< Carrier-phase measurement (RINEX 'L' observables). */
        Doppler,    /**< Doppler-shift measurement (RINEX 'D' observables, units: Hz). */
        SNR,        /**< Signal-to-noise ratio (RINEX 'S' observables). */
        Inertial,   /**< Inertial measurement (e.g. accelerometer / gyroscope). */
        Unknown     /**< Unrecognised or unsupported measurement type. */
    };

    /**
     * @brief Classify a RINEX-style observable key into a MeasurementType.
     *
     * The classification is based on the first character of @p key:
     *   'C' -> Code, 'L' -> Phase, 'D' -> Doppler, 'S' -> SNR.
     *
     * @param key Observable key (e.g. "C1W", "L1C", "D1C", "S1C").
     * @return Corresponding MeasurementType, or MeasurementType::Unknown.
     */
    inline MeasurementType classifyMeasurement(const std::string& key) {
        if (key.empty()) return MeasurementType::Unknown;
        switch (key[0]) {
        case 'C': return MeasurementType::Code;
        case 'L': return MeasurementType::Phase;
        case 'D': return MeasurementType::Doppler;
        case 'S': return MeasurementType::SNR;
        default:  return MeasurementType::Unknown;
        }
    }

    /**
     * @brief Observation information for a single satellite at an epoch.
     *
     * Contains raw and modeled observables, residuals and variances,
     * satellite geometry, and various auxiliary quantities used by PPP.
     */
    struct PppObservation {
        std::string satellite;                       /**< Satellite identifier (e.g. "G01") */
        System system;                               /**< Constellation/System for the satellite */
        GnssTime time;                               /**< Epoch time of the observation */
        std::map<std::string, double> observed;      /**< Raw observed measurement values keyed by observable name (e.g. "C1", "L1") */
        std::map<std::string, double> raw_observed;  /**< Original RINEX observables (L1C, C1W, etc.) for MW detection */
        std::map<std::string, double> modeled;       /**< Modeled values for the same observables */
        std::map<std::string, double> residuals;     /**< Residuals = observed - modeled */
        std::map<std::string, double> variances;     /**< Variance (or sigma^2) for each observable */
        std::map<std::string, int> lli_flags;        /**< Loss-of-lock indicators keyed by observable name */
        Eigen::Vector3d sat_pos;                     /**< Satellite ECEF position (meters) */
        Eigen::Vector3d sat_vel = Eigen::Vector3d::Zero(); /**< Satellite ECEF velocity (meters/second) */
        Eigen::Vector3d rx_pos_linearization;        /**< Receiver ECEF position used as the linearization point (meters) */
        double elevation;                            /**< Elevation angle (degrees) */
        double azimuth;                              /**< Azimuth angle (degrees) */
        Eigen::Vector3d los;                         /**< Line-of-sight unit vector from receiver to satellite */
        double range;                                /**< Geometric range (meters) */
        double mf_w;                                 /**< Wide-lane combination frequency value (Hz) or coefficient */
        double mf_d;                                 /**< Narrow-lane / difference combination frequency value (Hz) or coefficient */
    };

    /**
     * @brief State vector container used by PPP filters.
     *
     * Holds receiver position, clock and atmospheric states and their covariance.
     */
    struct PppState {
        Eigen::Vector3d position;                    /**< Receiver ECEF position (meters) */
        Eigen::Vector3d velocity = Eigen::Vector3d::Zero(); /**< Receiver ECEF velocity (meters/second) */
        double clock_bias;                           /**< Receiver clock bias (meters) */
        double troposphere_zwd;                      /**< Zenith wet delay (meters) */
        double tropo_gradient_north{ 0.0 };          /**< Tropospheric north gradient (meters) */
        double tropo_gradient_east{ 0.0 };           /**< Tropospheric east gradient (meters) */
        double phase_clock_offset{ 0.0 };            /**< Receiver phase clock offset from code clock (meters) */
        double ionosphere_zenith_delay;              /**< Ionospheric zenith delay (meters), if modeled as a single parameter */
        std::map<std::string, double> slant_ionosphere; /**< Per-satellite slant ionosphere delays (meters), keyed by satellite ID (e.g. "G01") */
        std::map<std::string, double> system_time_offsets; /**< Inter-system time biases (meters) keyed by system name */
        Eigen::MatrixXd covariance;                  /**< State covariance matrix */
        std::map<std::string, double> ambiguities;   /**< Carrier-phase ambiguity estimates keyed by satellite-signal */
        GnssTime epoch_time;                         /**< Epoch associated with this state */
    };

    /**
     * @brief Measurements and matrices assembled for a single epoch.
     *
     * This struct contains the innovation vector, design matrix, measurement noise,
     * and residual diagnostics for all observations in an epoch.
     */
    struct EpochMeasurement {
        Eigen::VectorXd innovations;                 /**< Innovation vector (observed - predicted) */
        Eigen::MatrixXd H;                           /**< Design matrix (partial derivatives of observations w.r.t states) */
        Eigen::VectorXd R_v;                         /**< Diagonal of measurement noise variances (or packed vector form) */
        Eigen::MatrixXd R_full;                      /**< Full measurement noise covariance (set by BSSD; empty when unused) */
        GnssTime time;                               /**< Epoch time for these measurements */
        std::vector<PppObservation> observations;    /**< Per-satellite observations at this epoch */
        std::vector<std::string> satellite_ids;      /**< Satellite identifiers ordered to match vectors/matrices */
        std::vector<std::string> signal_types;       /**< Signal/observable types corresponding to measurement rows */
        Eigen::VectorXd residuals;                   /**< Post-fit residuals */
        Eigen::MatrixXd residuals_covariance;        /**< Covariance of residuals */
        Eigen::VectorXd residuals_standardized;      /**< Standardized (normalized) residuals for outlier detection */
        size_t bad_residual_count{ 0 };              /**< Count of residuals flagged as bad/outliers */
        std::set<std::string> excluded_measurements; /**< Set of "satellite_signaltype" keys excluded as code outliers (persists across rebuild passes) */
    };

    /**
     * @brief Satellite orbit solution (position and velocity) at a time.
     *
     * Used to hold ephemeris/precise orbit outputs or propagation results.
     */
    struct OrbitData {
        std::string satellite;                       /**< Satellite identifier */
        GnssTime time;                               /**< Epoch of the orbit data */
        Eigen::Vector3d position;                    /**< Satellite ECEF position (meters) */
        Eigen::Vector3d velocity;                    /**< Satellite ECEF velocity (meters/second) */
        bool position_valid{ false };                /**< True if position has been computed/validated */
        bool velocity_valid{ false };                /**< True if velocity has been computed/validated */
    };

    /**
     * @brief Satellite clock data covering an interval or epoch.
     */
    struct ClockData {
        std::string satellite;                       /**< Satellite identifier */
        GnssTime time;                               /**< Epoch of the clock sample */
        GnssTime start_time;                         /**< Validity interval start */
        GnssTime end_time;                           /**< Validity interval end */
        double clock_bias{ 0.0 };                    /**< Satellite clock bias (seconds or converted to meters as used) */
    };

    /**
     * @brief Antenna metadata including phase center offsets and variations.
     *
     * The maps use signal or frequency keys (e.g. "L1", "L2") to store PCO/PCV values.
     */
    struct AntennaData {
        std::string type;                            /**< Antenna model/type string */
        std::map<std::string, Eigen::Vector3d> pco;  /**< Phase center offsets for each frequency (meters) */
        std::map<std::string, std::vector<std::vector<double>>> pcv; /**< Phase center variation grid for each frequency */
        double dazi{ 0.0 };                          /**< Azimuth spacing for PCV grid (degrees) or metadata */
        double dzen{ 0.0 };                          /**< Zenith spacing for PCV grid (degrees) or metadata */
        double zen1{ 0.0 };                          /**< Start zenith/nadir angle for PCV grid (degrees) */
        GnssTime valid_from;                         /**< Start validity for antenna model */
        GnssTime valid_until;                        /**< End validity for antenna model */
    };

    /**
     * @brief Receiver or satellite bias entry (e.g. code-phase biases).
     */
    struct BiasData {
        std::string satellite;                       /**< Satellite identifier or receiver id */
        std::string signal;                          /**< Signal type (e.g. "L1", "C1") this bias applies to */
        double bias{ 0.0 };                          /**< Bias value (units depend on use; e.g. meters) */
        double std_dev;                              /**< Standard deviation of the bias */
        GnssTime start_time;                         /**< Validity interval start */
        GnssTime end_time;                           /**< Validity interval end */
    };

    /**
     * @brief Satellite clock reference observable pair parsed from the BIA DESCRIPTION block.
     *
     * The satellite clock for each GNSS system is estimated from an ionosphere-free
     * combination of two specific code signals (e.g. C1W+C2W for GPS).  In uncombined
     * PPP-RTK mode the receiver hardware delay for each signal relative to this IF
     * combination must be parameterised using geometry-free ionosphere-free (GF-IF)
     * coefficients.  This struct stores the two reference signal codes so the
     * measurement model can compute the correct coefficients.
     */
    struct ClockRefSignals {
        std::string sig1;                            /**< First reference code signal (e.g. "C1W") */
        std::string sig2;                            /**< Second reference code signal (e.g. "C2W") */
    };

    /** Per-system satellite clock reference observables (system char 'G','E',… -> pair). */
    using ClockRefMap = std::map<char, ClockRefSignals>;

    /**
     * @brief Earth rotation / pole correction parameters (ERP) at an epoch.
     */
    struct ErpData {
        GnssTime time;                               /**< Epoch of ERP values */
        double xp{ 0.0 };                            /**< Pole position X (arcseconds or radians depending on usage) */
        double yp{ 0.0 };                            /**< Pole position Y (arcseconds or radians depending on usage) */
        double ut1_utc{ 0.0 };                       /**< UT1-UTC correction (seconds) */
    };

    /**
     * @brief Ocean tide displacement for a receiver at a given time.
     */
    struct OceanTideData {
        GnssTime time;                               /**< Epoch of the ocean tide correction */
        Eigen::Vector3d displacement;                /**< Displacement vector to apply (meters) */
    };

    /**
     * @brief Sun ephemeris (position) at a given epoch.
     */
    struct SunEphemeris {
        GnssTime time;                               /**< Epoch of the Sun position */
        Eigen::Vector3d position;                    /**< Sun position in ECEF or chosen frame (meters) */
    };

    /**
     * @brief Utility functions used by PPP processing modules.
     *
     * Wraps frequency/wavelength lookups and system string conversions.
     * For GLONASS, actual satellite-specific frequencies are computed using
     * the frequency channel number (FDMA). Call setGlonassSlots() to provide
     * the satellite-to-channel mapping before processing GLONASS observations.
     */
    class PppUtils{
    public:
        /**
         * @brief Construct a new PppUtils instance.
         * @param log_mgr Reference to the application's LogManager for logging.
         */
        explicit PppUtils(LogManager& log_mgr);

        /**
         * @brief Set GLONASS frequency slot (channel) assignments.
         *
         * Must be called before processing GLONASS observations to enable
         * satellite-specific frequency computation.
         *
         * @param slots Map of satellite ID (e.g., "R01") to frequency channel number (k ∈ [-7, +6]).
         */
        void setGlonassSlots(const std::map<std::string, int>& slots);

        /**
         * @brief Get the GLONASS frequency channel for a satellite.
         * @param satellite Satellite identifier (e.g., "R01").
         * @return Channel number if found, 0 otherwise.
         */
        int getGlonassChannel(const std::string& satellite) const;

        /**
         * @brief Get the actual frequency (Hz) for a given frequency type and satellite.
         *
         * For GLONASS, this returns the satellite-specific frequency computed from
         * the base frequency and channel offset. For other systems, returns the
         * nominal carrier frequency.
         *
         * @param freq_type A string identifying the frequency (e.g. "L1", "L2", "E1").
         * @param system The GNSS system enum for the satellite.
         * @param satellite Satellite identifier string (required for GLONASS).
         * @return Frequency in Hz, or 0.0 on error.
         */
        double getFrequency(const std::string& freq_type, System system, const std::string& satellite) const;

        /**
         * @brief Get the wavelength (meters) for a given system, frequency type, and satellite.
         *
         * For GLONASS, computes satellite-specific wavelength using actual frequency.
         *
         * @param system The GNSS system enum.
         * @param freq_type Frequency identifier string.
         * @param satellite Satellite identifier (required for GLONASS).
         * @return Wavelength in meters, or 0.0 on error.
         */
        double getWavelength(System system, const std::string& freq_type, const std::string& satellite) const;

        /**
         * @brief Get the wavelength (meters) for a given system and frequency type.
         *
         * For non-GLONASS systems only. For GLONASS, use the overload with satellite ID.
         *
         * @param system The GNSS system enum.
         * @param freq_type Frequency identifier string.
         * @return Wavelength in meters, or 0.0 on error.
         */
        double getWavelength(System system, const std::string& freq_type) const;

        /**
         * @brief Get a nominal reference frequency (Hz) for a frequency type and system.
         *
         * For GLONASS, returns the base/center frequency (channel k=0).
         * Use this for logging, records, or when satellite-specific frequency is not needed.
         *
         * @param freq_type Frequency identifier string.
         * @param system The GNSS system enum.
         * @return Reference frequency in Hz, or 0.0 on error.
         */
        double getReferenceFrequency(const std::string& freq_type, System system) const;

        /**
         * @brief Convert a System enum value to its printable string representation.
         * @param system System enum value.
         * @return std::string Human-readable system name (e.g. "GPS", "Galileo").
         */
        static std::string getSystemString(System system);

        /**
        * @brief Convert a satellite identifier to its system string representation.
        * @param satellite Satellite identifier (e.g., "G01", "E11", "R05").
        * @return std::string Human-readable system name (e.g. "GPS", "Galileo").
        */
        static std::string getSystemString(const std::string& satellite);

    private:
        LogManager& log_manager_;                     /**< Reference to LogManager used for logging inside utilities */
        std::map<std::string, int> glonass_slots_;    /**< GLONASS satellite -> frequency channel mapping */

        /**
         * @brief Default GLONASS frequency channel assignments.
         *
         * Used as fallback when no slot data is provided via setGlonassSlots().
         */
        static const std::unordered_map<std::string, int> DEFAULT_GLONASS_CHANNELS;
    };

}
