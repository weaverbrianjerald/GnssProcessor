#pragma once

/**
* @file Planetary.hpp
* @brief Planetary utilities: Sun & Moon positions and ephemeris handling.
*
* Provides rigorous routines to compute celestial body positions (Sun, Moon)
* using the IAU 1976/1980 precession/nutation model with GAST and polar
* motion.  All rotations use the static Rxyz() helper for consistency.
*/

#include "logging/LogManager.hpp"
#include <Eigen/Dense>
#include <array>
#include "math/TimeUtils.hpp"
#include "common/PppTypes.hpp"

namespace GnssProcessor {

    /**
     * @class Planetary
     * @brief Compute and manage planetary ephemerides used by GNSS models.
     *
     * The `Planetary` class provides the rigorous IAU 1976/1980 Sun/Moon
     * model (`computeSunMoonPosition`) and manages an in-memory Sun
     * ephemeris table for pre-computed positions.
     *
     * All positions are represented as ECEF Cartesian vectors in meters.
     * All principal-axis rotations use the static `Rxyz()` helper.
     */
    class Planetary {
    public:
        /**
         * @brief Construct a Planetary helper.
         * @param log_mgr Reference to the LogManager used for logging.
         */
        explicit Planetary(LogManager& log_mgr);

        /**
         * @brief Build Sun ephemeris for observation epochs.
         *
         * Computes Sun ephemeris entries for each epoch in `epoch_times`.
         *
         * @param epoch_times Input vector of GNSS epochs for which ephemeris is required.
         * @param[out] sun_ephemeris Output vector filled with Sun ephemeris entries.
         * @return true if at least one valid entry was computed; false otherwise.
         */
        bool parseSunEphemeris(const std::vector<GnssTime>& epoch_times, std::vector<SunEphemeris>& sun_ephemeris) const;

        /**
         * @brief Convert a GnssTime to Modified Julian Date (GPS time scale).
         *
         * Delegates to TimeUtils::gpsTimeToMjd(). Note: the result is on the
         * GPS time scale. For UTC MJD use TimeUtils::gpsTimeToUtcMjd().
         *
         * @param time GNSS epoch (GPS week + TOW).
         * @return Modified Julian Date (days, GPS time scale).
         */
        static double gpsTimeToMjd(const GnssTime& time);

        // -----------------------------------------------------------------
        // Rigorous RTKLIB-style Sun/Moon model with IAU precession/nutation
        // -----------------------------------------------------------------

        /**
         * @brief Compute Sun and Moon positions in ECEF using the rigorous
         *        IAU 1976/1980 precession/nutation model with GAST and polar motion.
         *
         * This is the RTKLIB-equivalent of sunmoonpos(). It computes ECI
         * positions via sunmoonpos_eci(), then transforms to ECEF using
         * eci2ecef() which applies IAU 1976 precession, IAU 1980 nutation
         * (106 terms), GAST, and polar motion from ERP data.
         *
         * @param time   GNSS epoch (GPS week + TOW) on the GPS time scale.
         * @param erps   Available Earth rotation parameter samples.
         * @param[out] sun_ecef  Sun position in ECEF (meters).
         * @param[out] moon_ecef Moon position in ECEF (meters).
         */
        void computeSunMoonPosition(const GnssTime& time,
            const std::vector<ErpData>& erps,
            Eigen::Vector3d& sun_ecef,
            Eigen::Vector3d& moon_ecef) const;

        /**
         * @brief Test whether a satellite is in Earth's umbral shadow.
         *
         * Standard cylindrical Earth-shadow model: the satellite is eclipsed
         * iff it sits on the anti-Sun side of Earth (projection of its
         * geocentric position onto the Sun direction is negative) AND its
         * perpendicular distance to the Earth–Sun axis is smaller than
         * Earth's radius scaled by @p shadow_scale.
         *
         * During eclipse and the post-shadow recovery window, GNSS satellites
         * execute block-specific yaw manoeuvres (midnight and noon turns) that
         * this codebase does not model, so the body-frame attitude — and
         * therefore satellite PCO/PCV and phase windup — become unreliable.
         * Use this predicate to skip such satellites at the geometry stage.
         *
         * The cylindrical model is conservative (the true Earth shadow is a
         * narrowing cone), but at GNSS altitudes the difference is sub-degree;
         * @p shadow_scale > 1.0 provides a configurable safety buffer around
         * the geometric terminator if a larger margin is desired.
         *
         * @param sat_pos_ecef Satellite ECEF position (meters).
         * @param sun_pos_ecef Sun ECEF position (meters).
         * @param shadow_scale Multiplier on Earth's radius (default 1.0 =
         *                     pure geometric shadow; >1.0 = additional buffer).
         * @return True if the satellite is in (or within the buffered margin of)
         *         Earth's umbra.
         */
        static bool isSatelliteEclipsing(const Eigen::Vector3d& sat_pos_ecef,
                                         const Eigen::Vector3d& sun_pos_ecef,
                                         double shadow_scale = 1.0);

    private:
        LogManager& log_manager_;                         /**< Logger for debugging and info. */
        std::vector<SunEphemeris> sun_ephemeris_;         /**< In-memory Sun ephemeris table. */

        /**
         * @brief Compute Sun and Moon positions in the ECI (J2000) frame.
         *
         * Implements an enhanced sunmoonpos_eci() algorithm using the obliquity
         * of the ecliptic, an extended solar longitude series (including Venus,
         * Jupiter, and Moon perturbations and ecliptic latitude), and lunar
         * series based on Delaunay fundamental arguments.
         *
         * @param t  Julian centuries of UT1 from J2000.0.
         * @param f  Five Delaunay fundamental arguments (radians).
         * @param[out] rsun  Sun position in ECI (meters).
         * @param[out] rmoon Moon position in ECI (meters).
         */
        static void sunMoonPosEci(double t, const std::array<double, 5>& f,
            Eigen::Vector3d& rsun, Eigen::Vector3d& rmoon);

        /**
         * @brief Compute the ECI→ECEF transformation matrix.
         *
         * Implements the RTKLIB eci2ecef() algorithm: IAU 1976 precession,
         * IAU 1980 nutation (106 terms), Greenwich Apparent Sidereal Time
         * (GAST), and polar motion (W matrix from ERP xp, yp).
         *
         * @param t_tt    Julian centuries of TT from J2000.0.
         * @param f       Five Delaunay fundamental arguments (radians).
         * @param gmst    GMST (radians) as computed by utc2gmst().
         * @param xp      Pole position X from ERP (radians).
         * @param yp      Pole position Y from ERP (radians).
         * @return 3×3 rotation matrix U = W · R(GAST) · N · P.
         */
        static Eigen::Matrix3d eci2ecef(double t_tt,
            const std::array<double, 5>& f, double gmst,
            double xp, double yp);

        /**
         * @brief Compute IAU 1980 nutation (dpsi, deps) using the 106-term series.
         *
         * @param t  Julian centuries of TT from J2000.0.
         * @param f  Five Delaunay fundamental arguments (radians).
         * @param[out] dpsi Nutation in longitude (radians).
         * @param[out] deps Nutation in obliquity (radians).
         */
        static void nutIau1980(double t, const std::array<double, 5>& f,
            double& dpsi, double& deps);

        /**
         * @brief Compute the five Delaunay fundamental arguments (RTKLIB ast_args).
         *
         * Returns [l, l', F, D, Omega] in radians for the given Julian
         * centuries of TT from J2000.0.
         *
         * @param t Julian centuries of TT from J2000.0.
         * @return Array of 5 fundamental arguments (radians).
         */
        static std::array<double, 5> astArgs(double t);

        /**
         * @brief Compute Greenwich Mean Sidereal Time (GMST) from a UTC time.
         *
         * Implements the RTKLIB utc2gmst() algorithm.
         *
         * @param t_ut1  Julian centuries of UT1 from J2000.0.
         * @param ut1_d  UT1 day fraction (fractional day).
         * @return GMST in radians.
         */
        static double utc2gmst(double t_ut1, double ut1_d);

        /**
         * @brief Interpolate ERP (xp, yp, ut1_utc) for a given epoch.
         *
         * Performs linear interpolation between the two nearest ERP entries.
         *
         * @param time   GNSS epoch for which ERP values are needed.
         * @param erps   Vector of parsed ERP entries.
         * @param[out] xp       Interpolated pole position X (arcseconds).
         * @param[out] yp       Interpolated pole position Y (arcseconds).
         * @param[out] ut1_utc  Interpolated UT1-UTC (seconds).
         */
        static void interpolateErp(const GnssTime& time,
            const std::vector<ErpData>& erps,
            double& xp, double& yp, double& ut1_utc);

        /**
         * @brief Build a 3×3 rotation matrix about a principal axis.
         *
         * @param angle Rotation angle (radians).
         * @param axis  Axis index: 1 = X, 2 = Y, 3 = Z.
         * @return 3×3 rotation matrix.
         */
        static Eigen::Matrix3d Rxyz(double angle, int axis);
    };

}