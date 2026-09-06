/**
* @file Planetary.cpp
* @brief Implementation of planetary utilities: Sun & Moon positions and ephemeris handling.
*
* Provides the rigorous IAU 1976/1980 Sun/Moon model with enhanced solar
* longitude series, GAST, and polar motion from ERP data, evaluated on
* demand per epoch.
*
* All principal-axis rotations use the static Rxyz() helper for consistency.
* All returned position vectors are ECEF Cartesian vectors in meters.
*/

#include "common/Constants.hpp"
#include "model/Planetary.hpp"
#include "common/PppTypes.hpp"

// IAU SOFA (vendored, third_party/sofa) — used by celestialToTerrestrial for the
// CIO-based IAU 2000A/2006 frame transform.  sofa.h carries its own extern "C"
// guards, so it can be included directly from this C++ TU.
#include "sofa.h"

namespace GnssProcessor {

    /**
     * @brief Construct a Planetary helper.
     */
    Planetary::Planetary(LogManager& log_mgr, bool enable_cio_frame,
                         bool correct_sun_frame)
        : log_manager_(log_mgr), enable_cio_frame_(enable_cio_frame),
          correct_sun_frame_(correct_sun_frame) {
        log_manager_.getLogger("planetary_model")->info(
            "Planetary model initialized (CIO frame: {}, sun-frame correction: {})",
            enable_cio_frame_ ? "on" : "off",
            correct_sun_frame_ ? "on" : "off");
    }

    /**
     * @brief Convert a GnssTime to Modified Julian Date (GPS time scale).
     *
     * MJD = JD - 2400000.5, where JD is on the GPS time scale.
     * For astronomical algorithms that need UTC MJD, use gpsTimeToUtcMjd() instead.
     */
    double Planetary::gpsTimeToMjd(const GnssTime& time) {
        return TimeUtils::gpsTimeToMjd(time);
    }

    // =====================================================================
    // Rigorous RTKLIB-style Sun/Moon model
    // =====================================================================

    /**
     * @brief Build a 3x3 rotation matrix about a principal axis (RTKLIB Rxyz).
     */
    Eigen::Matrix3d Planetary::Rxyz(double angle, int axis) {
        Eigen::Matrix3d R = Eigen::Matrix3d::Identity();
        const double c = std::cos(angle);
        const double s = std::sin(angle);
        if (axis == 1) {        // X-axis
            R(1, 1) =  c; R(1, 2) = s;
            R(2, 1) = -s; R(2, 2) = c;
        }
        else if (axis == 2) {   // Y-axis
            R(0, 0) =  c; R(0, 2) = -s;
            R(2, 0) =  s; R(2, 2) =  c;
        }
        else if (axis == 3) {   // Z-axis
            R(0, 0) =  c; R(0, 1) = s;
            R(1, 0) = -s; R(1, 1) = c;
        }
        return R;
    }

    /**
     * @brief Compute the five Delaunay fundamental arguments (RTKLIB ast_args).
     *
     * Returns [l, l', F, D, Omega] in radians.
     * Reference: IERS Conventions (2003), Section 5.7.2.
     */
    std::array<double, 5> Planetary::astArgs(double t) {
        constexpr double D2R = Constants::PI / 180.0;
        constexpr double AS2R = D2R / 3600.0;
        const double t2 = t * t;
        const double t3 = t2 * t;

        // l: Mean anomaly of the Moon (arcsec -> rad)
        double l  = (485868.249036 + (1717915923.2178) * t + 31.8792 * t2 + 0.051635 * t3) * AS2R;
        // l': Mean anomaly of the Sun
        double lp = (1287104.79305 + (129596581.0481)  * t - 0.5532 * t2 + 0.000136 * t3) * AS2R;
        // F: Mean argument of latitude of the Moon
        double F  = (335779.526232 + (1739527262.8478) * t - 12.7512 * t2 - 0.001037 * t3) * AS2R;
        // D: Mean elongation of Moon from Sun
        double D  = (1072260.70369 + (1602961601.2090) * t - 6.3706 * t2 + 0.006593 * t3) * AS2R;
        // Omega: Longitude of ascending node of Moon
        double Om = (450160.398036 - (6962890.5431)    * t + 7.4722 * t2 + 0.007702 * t3) * AS2R;

        return { l, lp, F, D, Om };
    }

    /**
     * @brief IAU 1980 nutation model — 106 term series.
     *
     * Reference: Seidelmann (1982), "1980 IAU Theory of Nutation".
     * Table columns: [l, l', F, D, Omega, period, psi_s, psi_t, eps_s, eps_t]
     *   dpsi = sum( (psi_s + psi_t * t) * sin(arg) ) * 1e-4 * AS2R
     *   deps = sum( (eps_s + eps_t * t) * cos(arg) ) * 1e-4 * AS2R
     */
    void Planetary::nutIau1980(double t, const std::array<double, 5>& f,
        double& dpsi, double& deps)
    {
        constexpr double AS2R = Constants::PI / 180.0 / 3600.0;

        // 106-term nutation table: [n_l, n_lp, n_F, n_D, n_Om, period, psi_s, psi_t, eps_s, eps_t]
        static constexpr double nut[106][10] = {
            {  0,  0,  0,  0,  1, -6798.4, -171996, -174.2,  92025,   8.9 },
            {  0,  0,  2, -2,  2,   182.6,  -13187,   -1.6,   5736,  -3.1 },
            {  0,  0,  2,  0,  2,    13.7,   -2274,   -0.2,    977,  -0.5 },
            {  0,  0,  0,  0,  2, -3399.2,    2062,    0.2,   -895,   0.5 },
            {  0, -1,  0,  0,  0,  -365.3,   -1426,    3.4,     54,  -0.1 },
            {  1,  0,  0,  0,  0,    27.6,     712,    0.1,     -7,   0.0 },
            {  0,  1,  2, -2,  2,   121.7,    -517,    1.2,    224,  -0.6 },
            {  0,  0,  2,  0,  1,    13.6,    -386,   -0.4,    200,   0.0 },
            {  1,  0,  2,  0,  2,     9.1,    -301,    0.0,    129,  -0.1 },
            {  0, -1,  2, -2,  2,   365.2,     217,   -0.5,    -95,   0.3 },
            { -1,  0,  0,  2,  0,    31.8,     158,    0.0,     -1,   0.0 },
            {  0,  0,  2, -2,  1,   177.8,     129,    0.1,    -70,   0.0 },
            { -1,  0,  2,  0,  2,    27.1,     123,    0.0,    -53,   0.0 },
            {  1,  0,  0,  0,  1,    27.7,      63,    0.1,    -33,   0.0 },
            {  0,  0,  0,  2,  0,    14.8,      63,    0.0,     -2,   0.0 },
            { -1,  0,  2,  2,  2,     9.6,     -59,    0.0,     26,   0.0 },
            { -1,  0,  0,  0,  1,   -27.4,     -58,   -0.1,     32,   0.0 },
            {  1,  0,  2,  0,  1,     9.1,     -51,    0.0,     27,   0.0 },
            { -2,  0,  0,  2,  0,  -205.9,     -48,    0.0,      1,   0.0 },
            { -2,  0,  2,  0,  1,  1305.5,      46,    0.0,    -24,   0.0 },
            {  0,  0,  2,  2,  2,     7.1,     -38,    0.0,     16,   0.0 },
            {  2,  0,  2,  0,  2,     6.9,     -31,    0.0,     13,   0.0 },
            {  2,  0,  0,  0,  0,    13.8,      29,    0.0,     -1,   0.0 },
            {  1,  0,  2, -2,  2,    23.9,      29,    0.0,    -12,   0.0 },
            {  0,  0,  2,  0,  0,    13.6,      26,    0.0,     -1,   0.0 },
            {  0,  0,  2, -2,  0,   173.3,     -22,    0.0,      0,   0.0 },
            { -1,  0,  2,  0,  1,    27.0,      21,    0.0,    -10,   0.0 },
            {  0,  2,  0,  0,  0,   182.6,      17,   -0.1,      0,   0.0 },
            {  0,  2,  2, -2,  2,    91.3,     -16,    0.1,      7,   0.0 },
            { -1,  0,  0,  2,  1,    32.0,      16,    0.0,     -8,   0.0 },
            {  0,  1,  0,  0,  1,   386.0,     -15,    0.0,      9,   0.0 },
            {  1,  0,  0, -2,  1,   -31.7,     -13,    0.0,      7,   0.0 },
            {  0, -1,  0,  0,  1,  -346.6,     -12,    0.0,      6,   0.0 },
            {  2,  0, -2,  0,  0, -1095.2,      11,    0.0,      0,   0.0 },
            { -1,  0,  2,  2,  1,     9.5,     -10,    0.0,      5,   0.0 },
            {  1,  0,  2,  2,  2,     5.6,      -8,    0.0,      3,   0.0 },
            {  0, -1,  2,  0,  2,    14.2,      -7,    0.0,      3,   0.0 },
            {  0,  0,  2,  2,  1,     7.1,      -7,    0.0,      3,   0.0 },
            {  1,  1,  0, -2,  0,   -34.8,      -7,    0.0,      0,   0.0 },
            {  0,  1,  2,  0,  2,    13.2,       7,    0.0,     -3,   0.0 },
            { -2,  0,  0,  2,  1,  -199.8,      -6,    0.0,      3,   0.0 },
            {  0,  0,  0,  2,  1,    14.8,      -6,    0.0,      3,   0.0 },
            {  2,  0,  2, -2,  2,    12.8,       6,    0.0,     -3,   0.0 },
            {  1,  0,  0,  2,  0,     9.6,       6,    0.0,      0,   0.0 },
            {  1,  0,  2, -2,  1,    23.9,       6,    0.0,     -3,   0.0 },
            {  0,  0,  0, -2,  1,   -14.7,      -5,    0.0,      3,   0.0 },
            {  0, -1,  2, -2,  1,   346.6,      -5,    0.0,      3,   0.0 },
            {  2,  0,  2,  0,  1,     6.9,      -5,    0.0,      3,   0.0 },
            {  1, -1,  0,  0,  0,    29.8,       5,    0.0,      0,   0.0 },
            {  1,  0,  0, -1,  0,   411.8,      -4,    0.0,      0,   0.0 },
            {  0,  0,  0,  1,  0,    29.5,      -4,    0.0,      0,   0.0 },
            {  0,  1,  0, -2,  0,   -15.4,      -4,    0.0,      0,   0.0 },
            {  1,  0, -2,  0,  0,   -26.9,       4,    0.0,      0,   0.0 },
            {  2,  0,  0, -2,  1,   212.3,       4,    0.0,     -2,   0.0 },
            {  0,  1,  2, -2,  1,   119.6,       4,    0.0,     -2,   0.0 },
            {  1,  1,  0,  0,  0,    25.6,      -3,    0.0,      0,   0.0 },
            {  1, -1,  0, -1,  0, -3232.9,      -3,    0.0,      0,   0.0 },
            { -1, -1,  2,  2,  2,     9.8,      -3,    0.0,      1,   0.0 },
            {  0, -1,  2,  2,  2,     7.2,      -3,    0.0,      1,   0.0 },
            {  1, -1,  2,  0,  2,     9.4,      -3,    0.0,      1,   0.0 },
            {  3,  0,  2,  0,  2,     5.5,      -3,    0.0,      1,   0.0 },
            { -2,  0,  2,  0,  2,  1615.7,      -3,    0.0,      1,   0.0 },
            {  1,  0,  2,  0,  0,     9.1,       3,    0.0,      0,   0.0 },
            { -1,  0,  2,  4,  2,     5.8,      -2,    0.0,      1,   0.0 },
            {  1,  0,  0,  0,  2,    27.8,      -2,    0.0,      1,   0.0 },
            { -1,  0,  2, -2,  1,   -32.6,      -2,    0.0,      1,   0.0 },
            {  0, -2,  2, -2,  1,  6786.3,      -2,    0.0,      1,   0.0 },
            { -2,  0,  0,  0,  1,   -13.7,      -2,    0.0,      1,   0.0 },
            {  2,  0,  0,  0,  1,    13.8,       2,    0.0,     -1,   0.0 },
            {  3,  0,  0,  0,  0,     9.2,       2,    0.0,      0,   0.0 },
            {  1,  1,  2,  0,  2,     8.9,       2,    0.0,     -1,   0.0 },
            {  0,  0,  2,  1,  2,     9.3,       2,    0.0,     -1,   0.0 },
            {  1,  0,  0,  2,  1,     9.6,      -1,    0.0,      0,   0.0 },
            {  1,  0,  2,  2,  1,     5.6,      -1,    0.0,      1,   0.0 },
            {  1,  1,  0, -2,  1,   -34.7,      -1,    0.0,      0,   0.0 },
            {  0,  1,  0,  2,  0,    14.2,      -1,    0.0,      0,   0.0 },
            {  0,  1,  2, -2,  0,   117.5,      -1,    0.0,      0,   0.0 },
            {  0,  1, -2,  2,  0,  -329.8,      -1,    0.0,      0,   0.0 },
            {  1,  0, -2,  2,  0,    23.8,      -1,    0.0,      0,   0.0 },
            {  1,  0, -2, -2,  0,    -9.5,      -1,    0.0,      0,   0.0 },
            {  1,  0,  2, -2,  0,    32.8,      -1,    0.0,      0,   0.0 },
            {  1,  0,  0, -4,  0,   -10.1,      -1,    0.0,      0,   0.0 },
            {  2,  0,  0, -4,  0,   -15.9,      -1,    0.0,      0,   0.0 },
            {  0,  0,  2,  4,  2,     4.8,      -1,    0.0,      0,   0.0 },
            {  0,  0,  2, -1,  2,    25.4,      -1,    0.0,      0,   0.0 },
            { -2,  0,  2,  4,  2,     7.3,      -1,    0.0,      1,   0.0 },
            {  2,  0,  2,  2,  2,     4.7,      -1,    0.0,      0,   0.0 },
            {  0, -1,  2,  0,  1,    14.2,      -1,    0.0,      0,   0.0 },
            {  0,  0, -2,  0,  1,   -13.6,      -1,    0.0,      0,   0.0 },
            {  0,  0,  4, -2,  2,    12.7,       1,    0.0,      0,   0.0 },
            {  0,  1,  0,  0,  2,   409.2,       1,    0.0,      0,   0.0 },
            {  1,  1,  2, -2,  2,    22.5,       1,    0.0,     -1,   0.0 },
            {  3,  0,  2, -2,  2,     8.7,       1,    0.0,      0,   0.0 },
            { -2,  0,  2,  2,  2,    14.6,       1,    0.0,     -1,   0.0 },
            { -1,  0,  0,  0,  2,   -27.3,       1,    0.0,     -1,   0.0 },
            {  0,  0, -2,  2,  1,  -169.0,       1,    0.0,      0,   0.0 },
            {  0,  1,  2,  0,  1,    13.1,       1,    0.0,      0,   0.0 },
            { -1,  0,  4,  0,  2,     9.1,       1,    0.0,      0,   0.0 },
            {  2,  1,  0, -2,  0,   131.7,       1,    0.0,      0,   0.0 },
            {  2,  0,  0,  2,  0,     7.1,       1,    0.0,      0,   0.0 },
            {  2,  0,  2, -2,  1,    12.8,       1,    0.0,     -1,   0.0 },
            {  2,  0, -2,  0,  1,  -943.2,       1,    0.0,      0,   0.0 },
            {  1, -1,  0, -2,  0,   -29.3,       1,    0.0,      0,   0.0 },
            { -1,  0,  0,  1,  1,  -388.3,       1,    0.0,      0,   0.0 },
            { -1, -1,  0,  2,  1,    35.0,       1,    0.0,      0,   0.0 },
            {  0,  1,  0,  1,  0,    27.3,       1,    0.0,      0,   0.0 }
        };

        dpsi = 0.0;
        deps = 0.0;
        for (int i = 0; i < 106; ++i) {
            double ang = 0.0;
            for (int j = 0; j < 5; ++j) {
                ang += nut[i][j] * f[j];
            }
            dpsi += (nut[i][6] + nut[i][7] * t) * std::sin(ang);
            deps += (nut[i][8] + nut[i][9] * t) * std::cos(ang);
        }
        dpsi *= 1e-4 * AS2R;  // 0.1 mas -> rad
        deps *= 1e-4 * AS2R;
    }

    /**
     * @brief Compute Greenwich Mean Sidereal Time (GMST) from UT1.
     *
     * Implements the RTKLIB utc2gmst() algorithm (ERA-based + equation of equinoxes).
     *
     * @param t_ut1  Julian centuries of UT1 from J2000.0.
     * @param ut1_d  UT1 day fraction (fractional part of UT1 JD).
     * @return GMST in radians.
     */
    double Planetary::utc2gmst(double t_ut1, double ut1_d) {
        // Earth Rotation Angle (ERA) — IERS 2003 / RTKLIB
        double ut1_jd = TimeUtils::J2000_JD + t_ut1 * TimeUtils::DAYS_PER_JULIAN_CENTURY;
        // tut0 must be Julian centuries from J2000.0 at 0h UT1 of the current day.
        // Since JD is referenced to noon (JD xxx.0 = noon), the start of the
        // calendar day (0h = midnight) is at floor(JD - 0.5) + 0.5.
        // Using floor(JD) alone gives the previous noon, which is 12 hours off
        // for times after midnight — producing ~0.5° GMST error and ~10^6 km
        // Sun position error.
        double tut0 = (std::floor(ut1_jd - 0.5) + 0.5 - TimeUtils::J2000_JD) / TimeUtils::DAYS_PER_JULIAN_CENTURY;

        // GMST at 0h UT1 (degrees) — IAU 1982 model
        double gmst0 = 24110.54841 + 8640184.812866 * tut0
            + 0.093104 * tut0 * tut0
            - 6.2e-6 * tut0 * tut0 * tut0;
        // Convert from seconds to radians and add Earth rotation for fractional day
        double gmst = gmst0 + 1.002737909350795 * ut1_d * 86400.0;
        // Convert seconds -> radians: 2*pi / 86400 = 7.272205...e-5
        gmst = std::fmod(gmst, 86400.0) * (2.0 * Constants::PI / 86400.0);
        if (gmst < 0.0) gmst += 2.0 * Constants::PI;
        return gmst;
    }

    /**
     * @brief Compute Sun and Moon ECI positions with enhanced Sun longitude series.
     *
     * Extends the RTKLIB sunmoonpos_eci() Sun model with additional terms for
     * the 3rd harmonic of the mean anomaly, Venus perturbation, Jupiter
     * perturbation, Moon perturbation, and ecliptic latitude of the Sun.
     * These terms reduce the ecliptic longitude error from ~1' to ~10"
     * and improve the Sun position accuracy from ~1000 km to ~100 km.
     *
     * Reference: Meeus, "Astronomical Algorithms" (1991), Ch. 25;
     *            Montenbruck & Gill, "Satellite Orbits" (2000), Ch. 3.3.2.
     *
     * @param t  Julian centuries of UT1 from J2000.0.
     * @param f  Five Delaunay fundamental arguments [l, l', F, D, Omega] (radians).
     * @param[out] rsun  Sun position in ECI (meters).
     * @param[out] rmoon Moon position in ECI (meters).
     */
    void Planetary::sunMoonPosEci(double t, const std::array<double, 5>& f,
        Eigen::Vector3d& rsun, Eigen::Vector3d& rmoon)
    {
        constexpr double AU = 149597870691.0;          // meters (RTKLIB value)
        constexpr double D2R = Constants::PI / 180.0;
        constexpr double RE = Constants::Ellipsoid::WGS84::a;  // 6378137.0 m

        // Obliquity of the ecliptic
        double eps = 23.439291 - 0.0130042 * t;  // degrees
        double sine = std::sin(eps * D2R);
        double cose = std::cos(eps * D2R);

        // --- Sun position in ECI (enhanced series) ---
        double Ms = 357.5277233 + 35999.05034 * t;  // mean anomaly of the Sun (deg)
        double Ms_rad = Ms * D2R;

        // Mean longitude of Venus and Jupiter for perturbation terms
        double Lv = 181.979800853 + 58517.8156748 * t;  // Venus mean longitude (deg)
        double Lj = 34.351518723  + 3034.9056746  * t;  // Jupiter mean longitude (deg)
        double Lm = 218.3164591   + 481267.88134  * t;  // Moon mean longitude (deg)

        // Ecliptic longitude with extended terms (degrees)
        double ls = 280.460 + 36000.770 * t
            + 1.914666471 * std::sin(Ms_rad)           // equation of center, 1st term
            + 0.019994643 * std::sin(2.0 * Ms_rad)     // equation of center, 2nd term
            + 0.000289226 * std::sin(3.0 * Ms_rad)     // equation of center, 3rd term
            + 0.003422    * std::sin((2.0 * Lv - 2.0 * (280.460 + 36000.770 * t)) * D2R)  // Venus perturbation
            + 0.000344    * std::sin((Lj - (280.460 + 36000.770 * t)) * D2R)              // Jupiter perturbation
            + 0.000117    * std::sin((2.0 * Lm - 2.0 * (280.460 + 36000.770 * t)) * D2R); // Moon perturbation

        // Ecliptic latitude of the Sun (arcseconds -> degrees)
        // The Sun's ecliptic latitude is very small but non-zero due to
        // the barycentric motion of the Earth-Moon system.
        double bs = (0.03 * std::sin((Lm - ls) * D2R)) / 3600.0;  // degrees

        // Distance (AU) — include 3rd harmonic
        double rs_au = 1.000140612
            - 0.016708617 * std::cos(Ms_rad)
            - 0.000139589 * std::cos(2.0 * Ms_rad)
            - 0.000005    * std::cos(3.0 * Ms_rad);
        double rs = AU * rs_au;                         // distance (m)

        // Convert ecliptic (lambda, beta) to equatorial ECI
        double sinl = std::sin(ls * D2R);
        double cosl = std::cos(ls * D2R);
        double sinb = std::sin(bs * D2R);
        double cosb = std::cos(bs * D2R);
        rsun(0) = rs * cosb * cosl;
        rsun(1) = rs * (cose * cosb * sinl - sine * sinb);
        rsun(2) = rs * (sine * cosb * sinl + cose * sinb);

        // --- Moon position in ECI ---
        double lm = 218.32 + 481267.883 * t
            + 6.29 * std::sin(f[0]) - 1.27 * std::sin(f[0] - 2.0 * f[3])
            + 0.66 * std::sin(2.0 * f[3]) + 0.21 * std::sin(2.0 * f[0])
            - 0.19 * std::sin(f[1]) - 0.11 * std::sin(2.0 * f[2]);
        double pm = 5.13 * std::sin(f[2])
            + 0.28 * std::sin(f[0] + f[2]) - 0.28 * std::sin(f[2] - f[0])
            - 0.17 * std::sin(f[2] - 2.0 * f[3]);
        double rm = RE / std::sin((0.9508
            + 0.0518 * std::cos(f[0])
            + 0.0095 * std::cos(f[0] - 2.0 * f[3])
            + 0.0078 * std::cos(2.0 * f[3])
            + 0.0028 * std::cos(2.0 * f[0])) * D2R);

        sinl = std::sin(lm * D2R);
        cosl = std::cos(lm * D2R);
        double sinp = std::sin(pm * D2R);
        double cosp = std::cos(pm * D2R);
        rmoon(0) = rm * cosp * cosl;
        rmoon(1) = rm * (cose * cosp * sinl - sine * sinp);
        rmoon(2) = rm * (sine * cosp * sinl + cose * sinp);
    }

    /**
     * @brief IAU 1976 precession matrix P (J2000 → mean equinox/equator of date).
     *
     * Verbatim extraction of the block that previously lived inline in
     * eci2ecef(), kept as a single definition so the rotation chain and the
     * of-date→J2000 de-precession cannot drift apart.
     */
    Eigen::Matrix3d Planetary::precessionIau1976(double t_tt)
    {
        constexpr double AS2R = Constants::PI / 180.0 / 3600.0;

        const double t2 = t_tt * t_tt;
        const double t3 = t2 * t_tt;

        double ze = (2306.2181 * t_tt + 0.30188 * t2 + 0.017998 * t3) * AS2R;
        double th = (2004.3109 * t_tt - 0.42665 * t2 - 0.041833 * t3) * AS2R;
        double z  = (2306.2181 * t_tt + 1.09468 * t2 + 0.018203 * t3) * AS2R;
        return Rxyz(-z, 3) * Rxyz(th, 2) * Rxyz(-ze, 3);
    }

    /**
     * @brief Compute the ECI→ECEF transformation matrix (RTKLIB eci2ecef).
     *
     * Applies: U = W · R3(GAST) · N · P
     *   P = IAU 1976 precession
     *   N = IAU 1980 nutation
     *   GAST = GMST + equation of equinoxes
     *   W = polar motion (xp, yp)
     */
    Eigen::Matrix3d Planetary::eci2ecef(double t_tt,
        const std::array<double, 5>& f, double gmst,
        double xp, double yp)
    {
        constexpr double AS2R = Constants::PI / 180.0 / 3600.0;

        const double t2 = t_tt * t_tt;
        const double t3 = t2 * t_tt;

        // --- IAU 1976 precession (shared with computeSunMoonPosition's de-precession) ---
        double eps = (84381.448 - 46.8150 * t_tt - 0.00059 * t2 + 0.001813 * t3) * AS2R;
        Eigen::Matrix3d P = precessionIau1976(t_tt);

        // --- IAU 1980 nutation ---
        double dpsi = 0.0, deps = 0.0;
        nutIau1980(t_tt, f, dpsi, deps);
        Eigen::Matrix3d N = Rxyz(-eps - deps, 1) * Rxyz(-dpsi, 3) * Rxyz(eps, 1);

        // --- Greenwich Apparent Sidereal Time (GAST) ---
        double gast = gmst + dpsi * std::cos(eps);
        gast += (0.00264 * std::sin(f[4]) + 0.000063 * std::sin(2.0 * f[4])) * AS2R;

        // --- Polar motion (W matrix) ---
        // xp, yp are in radians when passed in
        Eigen::Matrix3d W = Rxyz(-xp, 2) * Rxyz(-yp, 1);

        // --- Full transformation: U = W · R3(GAST) · N · P ---
        Eigen::Matrix3d R3_gast = Rxyz(gast, 3);
        return W * R3_gast * N * P;
    }

    /**
     * @brief CIO-based IAU 2000A/2006 celestial-to-terrestrial (ECI→ECEF) matrix.
     *
     * Mirrors M-PAGES Coordinate::NgsFormC2T using SOFA.  The (X, Y, s) path
     * (iauXys00a + iauC2ixys) is used instead of bare iauC2i00a so the observed
     * celestial-pole offsets dX, dY can be injected; with dX=dY=0 the result is
     * identical to iauC2i00a.  Returns R such that r_ecef = R · r_eci.
     */
    Eigen::Matrix3d Planetary::celestialToTerrestrial(double tt_jd, double ut1_jd,
        double xp_rad, double yp_rad, double dX_rad, double dY_rad)
    {
        // Two-part Julian dates using the (MJD_OFFSET, MJD) split for numerical
        // precision, matching M-PAGES (tta = 2400000.5, ttb = TT MJD; likewise UT1).
        const double tta = TimeUtils::MJD_OFFSET;          // 2400000.5
        const double ttb = tt_jd - TimeUtils::MJD_OFFSET;  // TT  as MJD
        const double uta = TimeUtils::MJD_OFFSET;
        const double utb = ut1_jd - TimeUtils::MJD_OFFSET;  // UT1 as MJD

        // CIP coordinates (X, Y) and CIO locator s — IAU 2000A precession-nutation.
        double x = 0.0, y = 0.0, s = 0.0;
        iauXys00a(tta, ttb, &x, &y, &s);

        // Apply observed celestial-pole offsets (radians); 0 → pure model.
        x += dX_rad;
        y += dY_rad;

        // Celestial-to-intermediate matrix.
        double rc2i[3][3];
        iauC2ixys(x, y, s, rc2i);

        // Earth Rotation Angle (UT1).
        const double era = iauEra00(uta, utb);

        // Polar-motion matrix (TIO locator sp from TT).
        const double sp = iauSp00(tta, ttb);
        double rpom[3][3];
        iauPom00(xp_rad, yp_rad, sp, rpom);

        // Compose celestial-to-terrestrial: rc2t = rpom · R3(era) · rc2i.
        double rc2t[3][3];
        iauC2tcio(rc2i, era, rpom, rc2t);

        Eigen::Matrix3d U;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                U(i, j) = rc2t[i][j];
        return U;
    }

    /**
     * @brief Interpolate ERP values for a given epoch.
     *
     * Uses binary search (O(log n)) to locate the bracketing ERP entries
     * instead of a linear scan. The ERP vector is assumed to be sorted
     * chronologically; if not, the function falls back to the nearest entry.
     */
    void Planetary::interpolateErp(const GnssTime& time,
        const std::vector<ErpData>& erps,
        double& xp, double& yp, double& ut1_utc,
        double& dX, double& dY)
    {
        xp = 0.0;
        yp = 0.0;
        ut1_utc = 0.0;
        dX = 0.0;
        dY = 0.0;

        if (erps.empty()) return;

        double t_query = TimeUtils::continuousGpsSeconds(time);

        // Binary search for the first ERP entry with time > t_query
        auto upper = std::lower_bound(erps.begin(), erps.end(), t_query,
            [](const ErpData& e, double t) {
                return TimeUtils::continuousGpsSeconds(e.time) < t;
            });

        const ErpData* before = nullptr;
        const ErpData* after  = nullptr;

        if (upper != erps.end()) {
            after = &(*upper);
        }
        if (upper != erps.begin()) {
            before = &(*(upper - 1));
        }

        if (before && after) {
            double t0 = TimeUtils::continuousGpsSeconds(before->time);
            double t1 = TimeUtils::continuousGpsSeconds(after->time);
            double span = t1 - t0;
            if (span > 1e-9) {
                double frac = (t_query - t0) / span;
                xp = before->xp + frac * (after->xp - before->xp);
                yp = before->yp + frac * (after->yp - before->yp);
                ut1_utc = before->ut1_utc + frac * (after->ut1_utc - before->ut1_utc);
                dX = before->dX + frac * (after->dX - before->dX);
                dY = before->dY + frac * (after->dY - before->dY);
            }
            else {
                xp = before->xp;
                yp = before->yp;
                ut1_utc = before->ut1_utc;
                dX = before->dX;
                dY = before->dY;
            }
        }
        else if (before) {
            xp = before->xp;
            yp = before->yp;
            ut1_utc = before->ut1_utc;
            dX = before->dX;
            dY = before->dY;
        }
        else if (after) {
            xp = after->xp;
            yp = after->yp;
            ut1_utc = after->ut1_utc;
            dX = after->dX;
            dY = after->dY;
        }
    }

    /**
     * @brief Compute Sun and Moon positions in ECEF using the rigorous model.
     *
     * Full pipeline matching RTKLIB sunmoonpos():
     * 1. Interpolate ERP (xp, yp, ut1_utc)
     * 2. Compute UT1 time from GPS time + leap seconds + UT1-UTC
     * 3. Compute fundamental arguments (ast_args)
     * 4. Compute Sun/Moon ECI positions (sunmoonpos_eci)
     * 5. Compute TT for precession/nutation
     * 6. Compute GMST
     * 7. Build ECI→ECEF matrix (eci2ecef) with precession, nutation, GAST, polar motion
     * 8. Transform to ECEF
     */
    void Planetary::computeSunMoonPosition(const GnssTime& time,
        const std::vector<ErpData>& erps,
        Eigen::Vector3d& sun_ecef,
        Eigen::Vector3d& moon_ecef) const
    {
        constexpr double AS2R = Constants::PI / 180.0 / 3600.0;

        // Step 1: Interpolate ERP values (xp, yp, dX, dY in arcseconds, ut1_utc in seconds)
        double xp_as = 0.0, yp_as = 0.0, ut1_utc = 0.0, dX_as = 0.0, dY_as = 0.0;
        interpolateErp(time, erps, xp_as, yp_as, ut1_utc, dX_as, dY_as);

        // Convert xp, yp from arcseconds to radians for the rotation matrices
        double xp_rad = xp_as * AS2R;
        double yp_rad = yp_as * AS2R;

        // Step 2: Compute UTC time, then UT1 = UTC + (UT1-UTC)
        double gps_jd = TimeUtils::gpsTimeToJulianDate(time);
        double utc_jd = TimeUtils::gpsJdToUtcJd(gps_jd);
        double ut1_jd = utc_jd + ut1_utc / static_cast<double>(TimeUtils::SECONDS_IN_DAY);

        // Julian centuries of UT1 from J2000.0
        double t_ut1 = TimeUtils::jdToJ2000Centuries(ut1_jd);

        // UT1 fractional day
        double ut1_d = std::fmod(ut1_jd + 0.5, 1.0);

        // Step 3: Compute fundamental arguments
        // TT for precession/nutation
        double t_tt = TimeUtils::gpsTimeToTtCenturies(time);

        // Fundamental arguments (use TT as in RTKLIB eci2ecef)
        std::array<double, 5> f = astArgs(t_tt);

        // Step 4: Compute Sun/Moon ECI positions using UT1 time
        // RTKLIB: tut=timeadd(tutc,erpv(3)) then sunmoonpos_eci(tut)
        // The ECI positions use UT1 time for the series evaluation
        Eigen::Vector3d rsun_eci, rmoon_eci;
        sunMoonPosEci(t_ut1, f, rsun_eci, rmoon_eci);

        // Step 4b: FRAME REPAIR (default OFF -- estimation.enable_sun_frame_correction).
        //
        // sunMoonPosEci evaluates its series with TROPICAL mean-longitude rates
        // (Sun 36000.770 deg/cy, Moon 481267.883 deg/cy -- the sidereal rates are
        // 35999.373 and 481266.494) and rotates ecliptic->equatorial with the
        // OF-DATE mean obliquity, so rsun_eci / rmoon_eci are in the mean equinox
        // and equator OF DATE, not J2000.  Both ECI->ECEF chains below then apply
        // precession a SECOND time -- eci2ecef through its IAU-1976 P, and
        // celestialToTerrestrial through the SOFA bias-precession-nutation matrix.
        // The delivered ECEF directions therefore carry a spurious rotation about
        // the ecliptic pole growing at 50.3"/yr from J2000: 1224.7" = 20.41' at
        // 2024-05-08.  (Nutation and GAST are NOT double-counted -- the series
        // carries no nutation term and uses the MEAN obliquity -- so N and the
        // equation of the equinoxes are correct and are left alone.)
        //
        // Applying P^T here returns the vectors to J2000, after which the existing
        // chain is exactly right.  Doing the repair on the VECTOR rather than in
        // the matrix keeps one code path for both the legacy and CIO transforms.
        // Residual for the CIO arm: the J2000 mean equinox differs from GCRS by
        // the 23 mas frame bias, which is 5e-5 of the 1224.7" being removed and
        // well under the Sun series' own 18.6" mean truncation error.
        if (correct_sun_frame_) {
            const Eigen::Matrix3d Pt = precessionIau1976(t_tt).transpose();
            rsun_eci  = Pt * rsun_eci;
            rmoon_eci = Pt * rmoon_eci;
        }

        // Step 5: Compute GMST
        double gmst = utc2gmst(t_ut1, ut1_d);

        // Step 6: Compute ECI→ECEF matrix.  With enable_cio_frame_, use the
        // CIO-based IAU 2000A/2006 transform (SOFA); otherwise the legacy
        // IAU 1976/1980 equinox chain (byte-identical to the historical path).
        Eigen::Matrix3d U;
        if (enable_cio_frame_) {
            const double tt_jd  = TimeUtils::gpsJdToTtJd(gps_jd);
            const double dX_rad = dX_as * AS2R;
            const double dY_rad = dY_as * AS2R;
            U = celestialToTerrestrial(tt_jd, ut1_jd, xp_rad, yp_rad, dX_rad, dY_rad);

            // One-shot startup diagnostic: angular difference between the CIO and
            // legacy Sun directions (expected « 1e-3 rad — frame bias + model diff).
            if (!cio_diag_logged_) {
                cio_diag_logged_ = true;
                const Eigen::Matrix3d U_legacy = eci2ecef(t_tt, f, gmst, xp_rad, yp_rad);
                const Eigen::Vector3d s_cio    = (U * rsun_eci).normalized();
                const Eigen::Vector3d s_legacy = (U_legacy * rsun_eci).normalized();
                double cos_d = s_cio.dot(s_legacy);
                if (cos_d > 1.0) cos_d = 1.0;
                else if (cos_d < -1.0) cos_d = -1.0;
                // Route this one-time, run-level validation through the primary
                // pipeline logger ("main_log") so it surfaces regardless of which
                // per-component loggers a given config enables.
                log_manager_.getLogger("main_log")->info(
                    "CIO frame enabled: Sun-direction delta vs legacy = {:.3e} rad "
                    "(xp={:.4f}\" yp={:.4f}\" ut1_utc={:.5f}s dX={:.4f}\" dY={:.4f}\")",
                    std::acos(cos_d), xp_as, yp_as, ut1_utc, dX_as, dY_as);
            }
        }
        else {
            U = eci2ecef(t_tt, f, gmst, xp_rad, yp_rad);
        }

        // Step 7: Transform to ECEF
        sun_ecef = U * rsun_eci;
        moon_ecef = U * rmoon_eci;

        // Validate outputs
        if (!sun_ecef.allFinite() || !moon_ecef.allFinite()) {
            log_manager_.getLogger("planetary_model")->warn(
                "Non-finite Sun/Moon ECEF at GPS week {} TOW {:.3f}",
                time.week, time.tow);
            sun_ecef = Eigen::Vector3d(1.496e11, 0.0, 0.0);  // ~1 AU along x-axis
            moon_ecef = Eigen::Vector3d(3.844e8, 0.0, 0.0);  // ~mean lunar distance
            return;
        }

        log_manager_.getLogger("planetary_model")->trace(
            "Rigorous Sun/Moon at GPS week {} TOW {:.3f}: "
            "Sun=({:.0f}, {:.0f}, {:.0f}) m, Moon=({:.0f}, {:.0f}, {:.0f}) m, "
            "ERP: xp={:.3f}\", yp={:.3f}\", ut1_utc={:.4f}s",
            time.week, time.tow,
            sun_ecef.x(), sun_ecef.y(), sun_ecef.z(),
            moon_ecef.x(), moon_ecef.y(), moon_ecef.z(),
            xp_as, yp_as, ut1_utc);
    }

    // =====================================================================
    // Earth-shadow eclipse predicate
    // =====================================================================

    bool Planetary::isSatelliteEclipsing(const Eigen::Vector3d& sat_pos_ecef,
                                          const Eigen::Vector3d& sun_pos_ecef,
                                          double shadow_scale) {
        // Defensive: a zero Sun vector means the cache hasn't been populated
        // yet (first call before computeSunMoonPosition).  Return false so we
        // don't spuriously eclipse every satellite at startup; the elevation
        // mask remains the operative early filter.
        const double sun_norm = sun_pos_ecef.norm();
        if (sun_norm < 1e6 || !sat_pos_ecef.allFinite() || !sun_pos_ecef.allFinite()) {
            return false;
        }

        // Direction to the Sun, viewed from Earth's centre.
        const Eigen::Vector3d sun_dir = sun_pos_ecef / sun_norm;

        // Signed distance of the satellite along the Earth->Sun axis.
        //   s_along > 0 : satellite sits on the Sun-facing side.
        //   s_along < 0 : satellite sits on the anti-Sun (shadow) side.
        const double s_along = sat_pos_ecef.dot(sun_dir);
        if (s_along >= 0.0) return false;  // sun-side; never eclipsed.

        // Perpendicular distance from the satellite to the Earth->Sun axis.
        // This is the in-shadow-cylinder test radius.
        const Eigen::Vector3d perp = sat_pos_ecef - s_along * sun_dir;
        const double s_perp = perp.norm();

        const double shadow_radius =
            Constants::EARTH_RADIUS * (shadow_scale > 0.0 ? shadow_scale : 1.0);
        return s_perp < shadow_radius;
    }


}