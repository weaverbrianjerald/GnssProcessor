// PppTypes.hpp
#pragma once

#include <map>
#include <string>
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
        std::map<std::string, double> modeled;       /**< Modeled values for the same observables */
        std::map<std::string, double> residuals;     /**< Residuals = observed - modeled */
        std::map<std::string, double> variances;     /**< Variance (or sigma^2) for each observable */
        std::map<std::string, int> lli_flags;        /**< Loss-of-lock indicators keyed by observable name */
        Eigen::Vector3d sat_pos;                     /**< Satellite ECEF position (meters) */
        double elevation;                            /**< Elevation angle (radians) */
        double azimuth;                              /**< Azimuth angle (radians) */
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
        double clock_bias;                           /**< Receiver clock bias (meters) */
        double troposphere_zwd;                      /**< Zenith wet delay (meters) */
        double ionosphere_zenith_delay;              /**< Ionospheric zenith delay (meters), if modeled as a single parameter */
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
        GnssTime time;                               /**< Epoch time for these measurements */
        std::vector<PppObservation> observations;    /**< Per-satellite observations at this epoch */
        std::vector<std::string> satellite_ids;      /**< Satellite identifiers ordered to match vectors/matrices */
        std::vector<std::string> signal_types;       /**< Signal/observable types corresponding to measurement rows */
        Eigen::VectorXd residuals;                   /**< Post-fit residuals */
        Eigen::MatrixXd residuals_covariance;        /**< Covariance of residuals */
        Eigen::VectorXd residuals_standardized;      /**< Standardized (normalized) residuals for outlier detection */
        size_t bad_residual_count;                   /**< Count of residuals flagged as bad/outliers */
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
     */
    class PppUtils{
    public:
        /**
         * @brief Construct a new PppUtils instance.
         * @param log_mgr Reference to the application's LogManager for logging.
         */
        explicit PppUtils(LogManager& log_mgr);

        /**
         * @brief Get the frequency (Hz) for a given frequency type and satellite system.
         *
         * @param freq_type A string identifying the frequency (e.g. "L1", "L2", "E1").
         * @param system The GNSS system enum for the satellite.
         * @param satellite Satellite identifier string (may be used for system-specific offsets).
         * @return Frequency in Hz.
         */
        double getFrequency(const std::string& freq_type, System system, const std::string& satellite) const;

        /**
         * @brief Get the wavelength (meters) for a given system and frequency type.
         * @param system The GNSS system enum.
         * @param freq_type Frequency identifier string.
         * @return Wavelength in meters.
         */
        double getWavelength(System system, const std::string& freq_type) const;

        /**
         * @brief Get a nominal reference frequency (Hz) for a frequency type and system.
         *
         * This is useful for forming linear combinations and reference conversions
         * when multiple frequencies are present for a signal type.
         */
        double getReferenceFrequency(const std::string& freq_type, System system) const;

        /**
         * @brief Convert a System enum value to its printable string representation.
         * @param system System enum value.
         * @return std::string Human-readable system name (e.g. "GPS", "Galileo").
         */
        static std::string getSystemString(System system);
    private:
        LogManager& log_manager_;                     /**< Reference to LogManager used for logging inside utilities */
    };

}
