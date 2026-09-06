/**
 * @file test_planetary_sun_frame.cpp
 * @brief Unit tests for the Sun/Moon ECI frame double-count and its correction
 *        (estimation.enable_sun_frame_correction).
 *
 * `Planetary::sunMoonPosEci` evaluates its series with TROPICAL mean-longitude
 * rates (Sun 36000.770 deg/cy, Moon 481267.883 deg/cy; the sidereal rates are
 * 35999.373 and 481266.494) and rotates ecliptic->equatorial with the OF-DATE
 * mean obliquity, so its output is in the mean equinox and equator OF DATE.
 * Both ECI->ECEF chains then apply precession a SECOND time.  The delivered
 * ECEF Sun/Moon directions therefore carry a spurious rotation about the
 * ecliptic pole growing at ~50.3"/yr from J2000 -- 20.4' at 2024-05-08.
 *
 * The oracle here is SOFA's iauEpv00 (Earth barycentric/heliocentric ephemeris,
 * ~1e-8 AU) rotated to ITRS with iauC2t00a.  It is fully vendored, so these
 * tests need no product files and no network.
 *
 * Note the DIRECTION of the guarantee: with the correction OFF the code is
 * expected to DISAGREE with the oracle by ~20.4'.  `LegacyPathCarriesTheDefect`
 * pins that disagreement deliberately -- it is a characterization test of the
 * inherited RTKLIB behaviour, not an endorsement of it.  If someone flips the
 * loader default, that test is the one that should fail first.
 */

#include <gtest/gtest.h>

#include "model/Planetary.hpp"
#include "model/SatAttitude.hpp"
#include "common/Constants.hpp"
#include "math/TimeUtils.hpp"
#include "products/ProductTypes.hpp"

#include <Eigen/Dense>
#include <cmath>
#include <vector>

// SOFA oracle (vendored; sofa.h has its own extern "C" guards).
#include "sofa.h"

namespace GnssProcessor {

namespace {

constexpr double kMjdOffset = 2400000.5;
constexpr double kRad2ArcMin = 180.0 * 60.0 / Constants::PI;

/// Angle between two vectors, in arcminutes.
double angleArcMin(const Eigen::Vector3d& a, const Eigen::Vector3d& b) {
    const double c = std::clamp(a.normalized().dot(b.normalized()), -1.0, 1.0);
    return std::acos(c) * kRad2ArcMin;
}

/**
 * @brief Independent ECEF Sun direction from SOFA, for the same epoch.
 *
 * Geometric Earth->Sun direction (no aberration, no light-time -- matching what
 * the code's series computes), taken as the negated heliocentric Earth position
 * from iauEpv00, then rotated ICRS->ITRS by iauC2t00a.  ERP is zero here so
 * UT1 == UTC and the pole is at the origin, matching a call made with an empty
 * ERP vector.
 */
Eigen::Vector3d sofaSunDirEcef(const GnssTime& t) {
    const double gps_jd = TimeUtils::gpsTimeToJulianDate(t);
    const double utc_jd = TimeUtils::gpsJdToUtcJd(gps_jd);
    const double tt_jd  = TimeUtils::gpsJdToTtJd(gps_jd);

    // Earth heliocentric position (AU, ICRS axes).  TT for TDB is good to ~1.7 ms.
    double pvh[2][3], pvb[2][3];
    iauEpv00(kMjdOffset, tt_jd - kMjdOffset, pvh, pvb);
    const Eigen::Vector3d sun_icrs(-pvh[0][0], -pvh[0][1], -pvh[0][2]);

    // ICRS -> ITRS with zero polar motion, UT1 == UTC.
    double rc2t[3][3];
    iauC2t00a(kMjdOffset, tt_jd - kMjdOffset,
              kMjdOffset, utc_jd - kMjdOffset,
              0.0, 0.0, rc2t);
    Eigen::Matrix3d R;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) R(i, j) = rc2t[i][j];

    return (R * sun_icrs).normalized();
}

/// Sun ECEF unit vector from the production path, with the correction on or off.
Eigen::Vector3d codeSunDirEcef(const GnssTime& t, bool correct) {
    LogManager log;
    Planetary planetary(log, /*enable_cio_frame=*/false, /*correct_sun_frame=*/correct);
    const std::vector<ErpData> no_erp;
    Eigen::Vector3d sun = Eigen::Vector3d::Zero();
    Eigen::Vector3d moon = Eigen::Vector3d::Zero();
    planetary.computeSunMoonPosition(t, no_erp, sun, moon);
    return sun.normalized();
}

/// 2024-05-08 12:00 UTC -- the IGS_Pasadena / DOY-129 fixture day.
GnssTime fixtureEpoch() {
    return TimeUtils::gregorianToGpsTime(2024, 5, 8, 12, 0, 0.0);
}

}  // namespace

// ---------------------------------------------------------------------------
// Byte-identity guard on the refactor: precessionIau1976 was extracted verbatim
// out of eci2ecef, so it must reproduce the original inline expression EXACTLY
// -- not approximately.  MSVC builds this tree at /fp:precise with no /arch:
// flag (no reassociation, no FMA contraction), so bitwise equality is the
// correct assertion; if it ever fails, the OFF path is no longer byte-identical
// and invariant #6 is broken.
// ---------------------------------------------------------------------------

TEST(PlanetarySunFrame, PrecessionExtractionIsBitwiseIdentical) {
    constexpr double kAs2R = Constants::PI / 180.0 / 3600.0;

    for (double t_tt : {-0.5, 0.0, 0.1234567, 0.24350445, 1.0}) {
        const double t2 = t_tt * t_tt;
        const double t3 = t2 * t_tt;

        // The literal pre-refactor block from eci2ecef.
        const double ze = (2306.2181 * t_tt + 0.30188 * t2 + 0.017998 * t3) * kAs2R;
        const double th = (2004.3109 * t_tt - 0.42665 * t2 - 0.041833 * t3) * kAs2R;
        const double z  = (2306.2181 * t_tt + 1.09468 * t2 + 0.018203 * t3) * kAs2R;
        const Eigen::Matrix3d expected =
            Planetary::Rxyz(-z, 3) * Planetary::Rxyz(th, 2) * Planetary::Rxyz(-ze, 3);

        const Eigen::Matrix3d got = Planetary::precessionIau1976(t_tt);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                EXPECT_EQ(got(i, j), expected(i, j))
                    << "bitwise mismatch at t_tt=" << t_tt << " (" << i << "," << j << ")";
    }
}

// ---------------------------------------------------------------------------
// The correction restores agreement with an independent ephemeris.
// ---------------------------------------------------------------------------

TEST(PlanetarySunFrame, CorrectedPathMatchesSofaEphemeris) {
    const GnssTime t = fixtureEpoch();
    const double err = angleArcMin(codeSunDirEcef(t, /*correct=*/true), sofaSunDirEcef(t));

    // Residual is the Sun series' own truncation (~0.3' typical) plus the 23 mas
    // J2000-vs-GCRS frame bias.  2' leaves ample margin without being vacuous.
    EXPECT_LT(err, 2.0) << "corrected Sun direction is " << err << "' from SOFA";
}

// ---------------------------------------------------------------------------
// Characterization: with the correction OFF the legacy path is ~20.4' out.
// ---------------------------------------------------------------------------

TEST(PlanetarySunFrame, LegacyPathCarriesTheDefect) {
    const GnssTime t = fixtureEpoch();
    const double err = angleArcMin(codeSunDirEcef(t, /*correct=*/false), sofaSunDirEcef(t));

    // p*T = 5028.796"/cy * 0.24350 cy = 1224.5" = 20.41'.
    EXPECT_GT(err, 15.0) << "expected the inherited precession double-count";
    EXPECT_LT(err, 25.0) << "error is larger than the precession double-count explains";
}

// ---------------------------------------------------------------------------
// Structural signature: P(0) = I, so the two arms must coincide at J2000 and
// diverge linearly away from it.  This needs no ephemeris at all.
// ---------------------------------------------------------------------------

TEST(PlanetarySunFrame, CorrectionVanishesAtJ2000) {
    const GnssTime t = TimeUtils::gregorianToGpsTime(2000, 1, 1, 12, 0, 0.0);
    const double d = angleArcMin(codeSunDirEcef(t, true), codeSunDirEcef(t, false));

    EXPECT_LT(d, 0.05) << "precession is identity at J2000; arms must agree";
}

TEST(PlanetarySunFrame, CorrectionGrowsLinearlyFromJ2000) {
    const GnssTime t1 = TimeUtils::gregorianToGpsTime(2012, 1, 1, 12, 0, 0.0);  // 12 yr
    const GnssTime t2 = TimeUtils::gregorianToGpsTime(2024, 1, 1, 12, 0, 0.0);  // 24 yr

    const double d1 = angleArcMin(codeSunDirEcef(t1, true), codeSunDirEcef(t1, false));
    const double d2 = angleArcMin(codeSunDirEcef(t2, true), codeSunDirEcef(t2, false));

    // ~50.3"/yr => 0.838'/yr.
    EXPECT_NEAR(d1, 0.838 * 12.0, 0.5);
    EXPECT_NEAR(d2, 0.838 * 24.0, 0.5);
    EXPECT_NEAR(d2 / d1, 2.0, 0.05) << "the defect must be linear in time from J2000";
}

// ---------------------------------------------------------------------------
// The Moon carries the same defect (its series is of-date too).
// ---------------------------------------------------------------------------

TEST(PlanetarySunFrame, MoonCarriesTheSameDefect) {
    const GnssTime t = fixtureEpoch();
    LogManager log;
    const std::vector<ErpData> no_erp;

    Eigen::Vector3d sun_on, moon_on, sun_off, moon_off;
    Planetary(log, false, true).computeSunMoonPosition(t, no_erp, sun_on, moon_on);
    Planetary(log, false, false).computeSunMoonPosition(t, no_erp, sun_off, moon_off);

    const double d_moon = angleArcMin(moon_on, moon_off);
    const double d_sun  = angleArcMin(sun_on, sun_off);

    EXPECT_GT(d_moon, 15.0);
    EXPECT_LT(d_moon, 25.0);
    // Both bodies are rotated by the same P, so the shifts agree closely.
    EXPECT_NEAR(d_moon, d_sun, 0.5);
}

// ---------------------------------------------------------------------------
// Attitude: getSatellitePV returns ECEF velocity; the orbit normal is inertial.
// ---------------------------------------------------------------------------

TEST(PlanetarySunFrame, InertialVelocityInvertsTheEcefConversion) {
    const Eigen::Vector3d r(2.0e7, 1.5e7, 5.0e6);
    const Eigen::Vector3d v_in(-1.5e3, 2.0e3, 3.0e3);

    // Exactly the conversion getSatellitePV applies (ModelGeometry.cpp).
    const double w = Constants::EARTH_ANGULAR_VELOCITY;
    const Eigen::Vector3d v_ecef(v_in.x() + w * r.y(), v_in.y() - w * r.x(), v_in.z());

    const Eigen::Vector3d recovered = SatAttitude::inertialVelocity(r, v_ecef);
    EXPECT_NEAR(recovered.x(), v_in.x(), 1e-9);
    EXPECT_NEAR(recovered.y(), v_in.y(), 1e-9);
    EXPECT_NEAR(recovered.z(), v_in.z(), 1e-9);
}

TEST(PlanetarySunFrame, BetaUsesTheInertialOrbitNormal) {
    // Circular MEO orbit, 55 deg inclination, satellite at the ascending node --
    // the phase where r x (omega x r) is fully perpendicular to h and the ECEF
    // normal error is largest.
    constexpr double kR = 2.656e7;
    constexpr double kV = 3.874e3;
    const double inc = 55.0 * Constants::PI / 180.0;

    const Eigen::Vector3d r(kR, 0.0, 0.0);
    const Eigen::Vector3d v_in(0.0, kV * std::cos(inc), kV * std::sin(inc));

    const double w = Constants::EARTH_ANGULAR_VELOCITY;
    const Eigen::Vector3d v_ecef(v_in.x() + w * r.y(), v_in.y() - w * r.x(), v_in.z());

    // Sun placed on the true inertial orbit normal: beta must read +90 deg.
    const Eigen::Vector3d n_true = r.cross(v_in).normalized();
    const Eigen::Vector3d sun = n_true * 1.496e11;

    EXPECT_NEAR(SatAttitude::computeBeta(r, v_ecef, sun, /*correct_velocity_frame=*/true),
                90.0, 0.5);

    // Gate OFF must reproduce the legacy (wrong-frame) answer, so the toggle is
    // proven to actuate and the OFF path is proven unchanged.
    EXPECT_GT(std::abs(SatAttitude::computeBeta(r, v_ecef, sun, false) - 90.0), 20.0);

    // Guard the premise: had the raw ECEF velocity been used, the normal would
    // have been badly wrong -- ~30 deg at this phase.
    const Eigen::Vector3d n_ecef = r.cross(v_ecef).normalized();
    const double normal_err_deg =
        std::acos(std::clamp(n_true.dot(n_ecef), -1.0, 1.0)) * 180.0 / Constants::PI;
    EXPECT_GT(normal_err_deg, 20.0);
}

}  // namespace GnssProcessor
