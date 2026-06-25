#pragma once

/**
 * @file ProductTypes.hpp
 * @brief Plain-data records for orbit, clock, antenna, bias, ERP, ocean tide, and Sun ephemeris products.
 *
 * Read by @c ProductParser, cached by @c ProductCache, consumed by
 * @c PppModel and @c GnssCorrections.  No behavior — just the shapes
 * the product loaders fill in and the model layers query.  Each type
 * carries enough metadata (timestamp, validity window where applicable)
 * for the cache to interpolate or look up by epoch.
 *
 * @c ClockRefSignals + @c ClockRefMap describe the per-system
 * iono-free reference signal pair that satellite clocks are estimated
 * against; the BIA file's DESCRIPTION block populates this so the
 * uncombined PPP-RTK measurement model can compute the right GF-IF
 * coefficients per signal.
 */

#include "math/TimeUtils.hpp"
#include <Eigen/Dense>
#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {

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
        int  iode{ -1 };                             /**< Issue of Data, Ephemeris (broadcast-nav only).
                                                          -1 = unknown / not from broadcast (e.g. SP3-derived).
                                                          Used by the SSR-overlay path to ensure the correction
                                                          applies to the same ephemeris baseline. */
        float accuracy_sigma_m{ 0.0f };              /**< Orbit position accuracy 1-sigma (meters), derived
                                                          from the SP3 per-satellite accuracy exponent (the
                                                          '++' header block, sigma = 2^acc mm) or, when present,
                                                          the per-record SP3-c/d position std-dev exponents.
                                                          0.0 = not present / unknown; trigSvdInterp treats a
                                                          zero (or all-equal) sigma set as a uniform unit weight,
                                                          so the SP3 interpolation is unweighted in that case.
                                                          Not binary-serialized (ProductCache serializes only
                                                          AntennaData); rides in value-semantic OrbitData
                                                          vectors and is default-initialized in every copy. */
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
        double clock_bias_sigma{ 0.0 };              /**< Reported bias σ (s) from AS record (col 2 of NV pair). 0 if not present. */
        double clock_rate{ 0.0 };                    /**< Satellite clock rate / drift (s/s) from AS continuation line.
                                                          Typically present only at coarse-cadence epochs (e.g. every 5 min
                                                          in many IGS Final products).  Use @c has_rate to discriminate. */
        double clock_rate_sigma{ 0.0 };              /**< Reported rate σ (s/s) from AS continuation line.  0 if not present. */
        bool   has_rate{ false };                    /**< True iff @c clock_rate was read from a valid AS continuation line. */
        int    iode{ -1 };                           /**< IODE of the broadcast ephemeris record this sample
                                                          was generated from.  -1 = unknown / not from broadcast
                                                          (e.g. CLK-derived).  Used by the SSR-overlay path
                                                          to keep clock corrections paired with the right
                                                          broadcast baseline. */
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
        double dX{ 0.0 };                            /**< CIP celestial-pole offset dX (arcseconds). From IERS finals2000A;
                                                          absent in standard IGS .ERP files, so defaults to 0.  Consumed only
                                                          by the CIO frame transform (Planetary::celestialToTerrestrial). */
        double dY{ 0.0 };                            /**< CIP celestial-pole offset dY (arcseconds). See dX. */
    };

    /**
     * @brief Ocean tide displacement for a receiver at a given time.
     */
    struct OceanTideData {
        GnssTime time;                               /**< Epoch of the ocean tide correction */
        Eigen::Vector3d displacement;                /**< Displacement vector to apply (meters) */
    };

} // namespace GnssProcessor
