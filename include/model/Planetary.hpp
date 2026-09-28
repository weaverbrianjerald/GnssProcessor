#pragma once

/**
* @file Planetary.hpp
* @brief Planetary utilities: Sun and Moon positions and ephemeris handling.
*
* Computes Sun and Moon positions in ECEF.  The default ECI→ECEF transform is
* the IAU 1976/1980 precession/nutation chain with GAST and polar motion; an
* optional CIO-based IAU 2000A/2006 transform (SOFA) replaces it.  All
* principal-axis rotations use the static Rxyz() helper.
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
     * `computeSunMoonPosition` evaluates the IAU 1976/1980 Sun/Moon model on
     * demand for each epoch.  `isSatelliteEclipsing` is a static Earth-shadow
     * predicate on the resulting Sun position.
     *
     * All positions are ECEF Cartesian vectors in meters.
     * All principal-axis rotations use the static `Rxyz()` helper.
     */
    class Planetary {
    public:
        /**
         * @brief Construct a Planetary helper.
         * @param log_mgr Reference to the LogManager used for logging.
         * @param enable_cio_frame When true, the ECI→ECEF transform used for
         *        Sun/Moon positions switches from the legacy IAU 1976/1980
         *        equinox chain to the CIO-based IAU 2000A/2006 transform
         *        (Planetary::celestialToTerrestrial, via SOFA).  Defaults to
         *        false, in which case the legacy path is byte-identical.
         * @param correct_sun_frame When true, the of-date Sun/Moon ECI vectors
         *        are de-precessed to J2000 before the ECI→ECEF rotation, which
         *        cancels the precession double-count inherited from RTKLIB.
         *        Defaults to false, in which case the legacy path (which applies
         *        precession twice) is byte-identical.
         */
        explicit Planetary(LogManager& log_mgr, bool enable_cio_frame = false,
                           bool correct_sun_frame = false);

        /**
         * @brief Convert a GnssTime to Modified Julian Date (GPS time scale).
         *
         * Delegates to TimeUtils::gpsTimeToMjd().  The result is on the GPS
         * time scale; for UTC MJD use TimeUtils::gpsTimeToUtcMjd().
         *
         * @param time GNSS epoch (GPS week + TOW).
         * @return Modified Julian Date (days, GPS time scale).
         */
        static double gpsTimeToMjd(const GnssTime& time);

        // -----------------------------------------------------------------
        // RTKLIB-style Sun/Moon model with IAU precession/nutation
        // -----------------------------------------------------------------

        /**
         * @brief Compute Sun and Moon positions in ECEF using the IAU 1976/1980
         *        precession/nutation model with GAST and polar motion.
         *
         * Equivalent of RTKLIB sunmoonpos().  ECI positions come from
         * sunMoonPosEci(); eci2ecef() then applies IAU 1976 precession,
         * IAU 1980 nutation (106 terms), GAST, and polar motion from ERP data.
         * With enable_cio_frame the ECI→ECEF step uses celestialToTerrestrial()
         * instead; with correct_sun_frame the ECI vectors are first de-precessed
         * to J2000 (see precessionIau1976()).
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
         * Cylindrical Earth-shadow model: the satellite is eclipsed if and only
         * if it is on the anti-Sun side of Earth (the projection of its
         * geocentric position onto the Sun direction is negative) and its
         * perpendicular distance to the Earth–Sun axis is smaller than Earth's
         * radius scaled by @p shadow_scale.
         *
         * During eclipse and the post-shadow recovery window, GNSS satellites
         * perform block-specific yaw manoeuvres (midnight and noon turns) that
         * this codebase does not model, so the body-frame attitude, and with it
         * satellite PCO/PCV and phase windup, is unreliable.  This predicate
         * lets the geometry stage skip such satellites.
         *
         * The cylindrical model is conservative (the true umbra is a narrowing
         * cone), and at GNSS altitudes the difference is sub-degree.
         * @p shadow_scale > 1.0 adds a configurable margin around the
         * geometric terminator.
         *
         * @param sat_pos_ecef Satellite ECEF position (meters).
         * @param sun_pos_ecef Sun ECEF position (meters).  A vector shorter than
         *                     1e6 m (not yet computed) or a non-finite input
         *                     returns false.
         * @param shadow_scale Multiplier on Earth's radius (default 1.0 =
         *                     pure geometric shadow; >1.0 = additional margin;
         *                     a value <= 0 is treated as 1.0).
         * @return True if the satellite is in (or within the scaled margin of)
         *         Earth's umbra.
         */
        static bool isSatelliteEclipsing(const Eigen::Vector3d& sat_pos_ecef,
                                         const Eigen::Vector3d& sun_pos_ecef,
                                         double shadow_scale = 1.0);

        /**
         * @brief Build the CIO-based celestial-to-terrestrial (ECI→ECEF) matrix.
         *
         * Implements the IAU 2000A/2006 CIO-based transform with the SOFA
         * library, following M-PAGES Coordinate::NgsFormC2T:
         *
         *   1. CIP coordinates and CIO locator (X, Y, s) from `iauXys00a`
         *      (IAU 2000A precession-nutation).
         *   2. Apply the observed celestial-pole offsets dX, dY (zero by
         *      default, in which case the result is identical to `iauC2i00a`).
         *   3. Form the celestial-to-intermediate matrix with `iauC2ixys`.
         *   4. Earth Rotation Angle from `iauEra00` (UT1).
         *   5. Polar-motion matrix from `iauPom00` (TIO locator `sp` via
         *      `iauSp00`).
         *   6. Compose with `iauC2tcio`.
         *
         * The returned matrix R satisfies `r_ecef = R · r_eci` (GCRS→ITRS),
         * the same orientation as the legacy `eci2ecef()` output.
         *
         * @param tt_jd   Terrestrial Time Julian Date.
         * @param ut1_jd  UT1 Julian Date.
         * @param xp_rad  Polar motion X (radians).
         * @param yp_rad  Polar motion Y (radians).
         * @param dX_rad  CIP celestial-pole offset dX (radians; 0 = pure model).
         * @param dY_rad  CIP celestial-pole offset dY (radians; 0 = pure model).
         * @return 3×3 celestial-to-terrestrial (ECI→ECEF) rotation matrix.
         */
        static Eigen::Matrix3d celestialToTerrestrial(double tt_jd, double ut1_jd,
            double xp_rad, double yp_rad, double dX_rad, double dY_rad);

        /**
         * @brief IAU 1976 precession matrix P (J2000 → mean equinox/equator of date).
         *
         * Single definition shared by @c eci2ecef and the of-date→J2000
         * de-precession in @c computeSunMoonPosition, so both evaluate the same
         * expression in the same operand order.  @c eci2ecef delegates here with
         * the expression and operand order of its own legacy code, so the legacy
         * path stays bit-identical (MSVC builds at /fp:precise with no /arch:
         * flag, hence no reassociation and no FMA contraction).
         *
         * Pᵀ maps a mean-of-date vector back to J2000.  The output of
         * @c sunMoonPosEci needs that before either ECI→ECEF chain is applied,
         * because its tropical mean-longitude rates and of-date mean obliquity
         * already place it in the mean equinox and equator of date.
         *
         * @param t_tt Julian centuries of TT from J2000.0.
         * @return 3×3 precession matrix P = R3(−z)·R2(θ)·R3(−ζ).
         */
        static Eigen::Matrix3d precessionIau1976(double t_tt);

        /**
         * @brief Build a 3×3 rotation matrix about a principal axis.
         *
         * @param angle Rotation angle (radians).
         * @param axis  Axis index: 1 = X, 2 = Y, 3 = Z.
         * @return 3×3 rotation matrix.
         */
        static Eigen::Matrix3d Rxyz(double angle, int axis);

    private:
        LogManager& log_manager_;                         /**< Logger for debugging and info. */
        bool enable_cio_frame_ = false;                   /**< Use the CIO IAU 2000A/2006 ECI→ECEF transform (SOFA) for Sun/Moon. */
        mutable bool cio_diag_logged_ = false;            /**< One-shot guard for the startup CIO-vs-legacy Sun-direction diagnostic. */
        bool correct_sun_frame_ = false;                  /**< De-precess the of-date Sun/Moon ECI vectors to J2000 before the ECI→ECEF rotation (cancels the precession double-count). */

        /**
         * @brief Compute Sun and Moon positions in ECI, mean equator and equinox
         *        of date.
         *
         * Extended form of RTKLIB sunmoonpos_eci(): obliquity of the ecliptic,
         * an extended solar longitude series (third harmonic, Venus, Jupiter
         * and Moon perturbations, and ecliptic latitude), and a lunar series
         * based on the Delaunay fundamental arguments.  The tropical
         * mean-longitude rates and the of-date mean obliquity put the result in
         * the mean-of-date frame, not J2000 (see precessionIau1976()).
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
         * @brief Compute Greenwich Mean Sidereal Time (GMST) from UT1.
         *
         * Implements the RTKLIB utc2gmst() algorithm (IAU 1982 GMST); the
         * arguments are UT1, not UTC, despite the name.
         *
         * @param t_ut1  Julian centuries of UT1 from J2000.0.
         * @param ut1_d  Fraction of the UT1 day elapsed since 0h UT1.
         * @return GMST in radians.
         */
        static double utc2gmst(double t_ut1, double ut1_d);

        /**
         * @brief Interpolate ERP (xp, yp, ut1_utc, dX, dY) for a given epoch.
         *
         * Linear interpolation between the two entries that bracket the epoch;
         * outside the tabulated span the nearest end entry is held, and with no
         * entries every output is 0.  @p erps must be sorted by time.
         *
         * @param time   GNSS epoch for which ERP values are needed.
         * @param erps   Vector of parsed ERP entries, sorted by time.
         * @param[out] xp       Interpolated pole position X (arcseconds).
         * @param[out] yp       Interpolated pole position Y (arcseconds).
         * @param[out] ut1_utc  Interpolated UT1-UTC (seconds).
         * @param[out] dX       Interpolated CIP offset dX (arcseconds; 0 when
         *                      the ERP source carries no celestial-pole offsets).
         * @param[out] dY       Interpolated CIP offset dY (arcseconds).
         */
        static void interpolateErp(const GnssTime& time,
            const std::vector<ErpData>& erps,
            double& xp, double& yp, double& ut1_utc,
            double& dX, double& dY);

    };

}
