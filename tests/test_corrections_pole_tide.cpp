/**
 * @file test_corrections_pole_tide.cpp
 * @brief Sign and value tests of the pole tide displacement of a station.
 *
 * GnssCorrections::computePoleTideCorrection turns the polar motion of an Earth
 * rotation product into a millimetre-level station displacement.  A sign error in
 * one component is invisible in a residual check (the displacement is small and
 * slowly varying) yet biases every coordinate solution, which is the kind of
 * defect the project has met twice before in other correction models (phase
 * wind-up, ocean tide phases).  Each case therefore writes out the conventional
 * formula inside the test and compares the code with it:
 *
 *   rotational deformation (IERS Conventions 2010, eq. 7.26), in colatitude
 *   theta and longitude lambda, millimetres for m1, m2 in arcseconds:
 *       S_r      = -33 sin(2 theta) (m1 cos lambda + m2 sin lambda)
 *       S_theta  =  -9 cos(2 theta) (m1 cos lambda + m2 sin lambda)   (toward south)
 *       S_lambda =  +9 cos(theta)   (m1 sin lambda - m2 cos lambda)
 *   with East = S_lambda, North = -S_theta, Up = S_r and the wobble
 *       m1 = xp - xp_mean,   m2 = -(yp - yp_mean)
 *   about the IERS 2018 secular mean pole
 *       xp_mean = 55.0 + 1.677 (t - 2000) mas,  yp_mean = 320.5 + 3.460 (t - 2000) mas.
 *
 * The formula below uses the geodetic latitude for theta, as the code and RTKLIB
 * do; the difference from the geocentric latitude is a relative 0.6 percent of a
 * few millimetres at mid latitude and does not touch any sign.
 *
 * The code returns an ECEF vector.  The cases project it on the local East,
 * North and Up axes built here from the station's latitude and longitude.
 */

#include <gtest/gtest.h>

#include "common/Constants.hpp"
#include "corrections/GnssCorrections.hpp"
#include "logging/LogManager.hpp"
#include "math/Coordinates.hpp"
#include "math/TimeUtils.hpp"
#include "products/ProductTypes.hpp"

#include <Eigen/Dense>
#include <array>
#include <cmath>
#include <vector>

namespace GnssProcessor {
namespace {

using Eigen::Vector3d;

constexpr double kDeg = Constants::PI / 180.0;
constexpr double kAs2Rad = Constants::PI / 648000.0;

// The code and the formula below agree to rounding (both are a few products of
// sines); 1 nm is six orders below the smallest component asserted.
constexpr double kTol = 1e-9;

struct Enu { double e{0.0}, n{0.0}, u{0.0}; };

/// Decimal year of a GPS epoch, from its Modified Julian Date (J2000 = MJD 51544.5).
double decimalYear(const GnssTime& t) {
    const double mjd = 44244.0 + 7.0 * t.week + t.tow / 86400.0;
    return 2000.0 + (mjd - 51544.5) / 365.25;
}

/// IERS 2018 secular mean pole (arcseconds) at a GPS epoch.
double xpMean(const GnssTime& t) { return (55.0  + 1.677 * (decimalYear(t) - 2000.0)) * 1.0e-3; }
double ypMean(const GnssTime& t) { return (320.5 + 3.460 * (decimalYear(t) - 2000.0)) * 1.0e-3; }

/// The conventional rotational-deformation pole tide (metres) at geodetic
/// latitude @p lat_rad and longitude @p lon_rad for the wobble m1, m2 (arcsec).
Enu conventionalPoleTide(double lat_rad, double lon_rad, double m1, double m2) {
    const double theta = 0.5 * Constants::PI - lat_rad;   // colatitude
    const double cl = std::cos(lon_rad), sl = std::sin(lon_rad);
    const double s_r      = -33.0e-3 * std::sin(2.0 * theta) * (m1 * cl + m2 * sl);
    const double s_theta  =  -9.0e-3 * std::cos(2.0 * theta) * (m1 * cl + m2 * sl);
    const double s_lambda =   9.0e-3 * std::cos(theta)       * (m1 * sl - m2 * cl);
    return { s_lambda, -s_theta, s_r };
}

/// The same displacement from the ERP values themselves (xp, yp in arcsec).
Enu conventionalFromErp(double lat_rad, double lon_rad, const GnssTime& t, double xp, double yp) {
    return conventionalPoleTide(lat_rad, lon_rad, xp - xpMean(t), -(yp - ypMean(t)));
}

/// Projection of an ECEF displacement on the local East, North, Up axes.
Enu toEnu(const Vector3d& d, double lat_rad, double lon_rad) {
    const Vector3d east(-std::sin(lon_rad), std::cos(lon_rad), 0.0);
    const Vector3d north(-std::sin(lat_rad) * std::cos(lon_rad),
                         -std::sin(lat_rad) * std::sin(lon_rad),
                          std::cos(lat_rad));
    const Vector3d up(std::cos(lat_rad) * std::cos(lon_rad),
                      std::cos(lat_rad) * std::sin(lon_rad),
                      std::sin(lat_rad));
    return { d.dot(east), d.dot(north), d.dot(up) };
}

ErpData erp(const GnssTime& t, double xp, double yp) {
    ErpData e;
    e.time = t;
    e.xp = xp;
    e.yp = yp;
    return e;
}

const GnssTime kT{ 2300, 0.0 };   // 2024-02-04, decimal year 2024.0917

/// Pole tide (ENU, metres) the code returns for a station at (lat, lon, 0 m) with one ERP record.
Enu codePoleTide(double lat_deg, double lon_deg, const std::vector<ErpData>& erps, const GnssTime& t) {
    LogManager log;
    GnssCorrections gc(log, std::vector<ClockData>{}, erps);
    const double lat = lat_deg * kDeg, lon = lon_deg * kDeg;
    const Vector3d rx = Coordinates::llhToEcef(lat, lon, 0.0);
    return toEnu(gc.computePoleTideCorrection(rx, t), lat, lon);
}

}  // namespace

// ---------------------------------------------------------------------------
// A positive m1 (the pole displaced toward Greenwich by 0.1 arcsec, m2 = 0) at
// 40 N, 20 E.  The formula gives
//     East  = +9 sin(40) (0.1 sin 20)            = +0.198 mm
//     North = -9 cos(80) (0.1 cos 20)            = -0.147 mm
//     Up    = -33 sin(80) (0.1 cos 20)           = -3.054 mm
// Fails if the sign of any component flips, if the east term takes cos(lat)
// in place of sin(lat), or if the wobble is formed in radians.
// ---------------------------------------------------------------------------
TEST(CorrectionsPoleTide, PositiveM1AtMidLatitudeMatchesTheConventionalFormula) {
    const double xp = xpMean(kT) + 0.1;
    const double yp = ypMean(kT);
    const Enu got  = codePoleTide(40.0, 20.0, { erp(kT, xp, yp) }, kT);
    const Enu want = conventionalPoleTide(40.0 * kDeg, 20.0 * kDeg, 0.1, 0.0);

    EXPECT_GT(want.e, 0.0);
    EXPECT_LT(want.n, 0.0);
    EXPECT_LT(want.u, 0.0);
    EXPECT_NEAR(got.e, want.e, kTol);
    EXPECT_NEAR(got.n, want.n, kTol);
    EXPECT_NEAR(got.u, want.u, kTol);
    // Recorded values of the formula, kept so the case still checks a fixed number if the oracle is edited.
    EXPECT_NEAR(got.e,  1.9786168e-4, 1e-10);
    EXPECT_NEAR(got.n, -1.4685832e-4, 1e-10);
    EXPECT_NEAR(got.u, -3.0538747e-3, 1e-10);
}

// ---------------------------------------------------------------------------
// A positive m2 (yp 0.1 arcsec below its mean, m1 = 0) at the same station:
//     East  = +9 sin(40) (-0.1 cos 20)           = -0.544 mm
//     North = -9 cos(80) (0.1 sin 20)            = -0.053 mm
//     Up    = -33 sin(80) (0.1 sin 20)           = -1.112 mm
// Fails if the sign convention m2 = -(yp - yp_mean) is lost (every component
// flips) or if the m2 terms of the east component swap sign.
// ---------------------------------------------------------------------------
TEST(CorrectionsPoleTide, PositiveM2AtMidLatitudeMatchesTheConventionalFormula) {
    const double xp = xpMean(kT);
    const double yp = ypMean(kT) - 0.1;
    const Enu got  = codePoleTide(40.0, 20.0, { erp(kT, xp, yp) }, kT);
    const Enu want = conventionalPoleTide(40.0 * kDeg, 20.0 * kDeg, 0.0, 0.1);

    EXPECT_LT(want.e, 0.0);
    EXPECT_LT(want.n, 0.0);
    EXPECT_LT(want.u, 0.0);
    EXPECT_NEAR(got.e, want.e, kTol);
    EXPECT_NEAR(got.n, want.n, kTol);
    EXPECT_NEAR(got.u, want.u, kTol);
    EXPECT_NEAR(got.e, -5.4362050e-4, 1e-10);
    EXPECT_NEAR(got.n, -5.3452057e-5, 1e-10);
    EXPECT_NEAR(got.u, -1.1115195e-3, 1e-10);
}

// ---------------------------------------------------------------------------
// Mirror station at 40 S, 20 E with the same positive m1: sin(2 theta) and
// cos(theta) change sign, cos(2 theta) does not, so Up and East flip and North
// keeps its sign.  Fails if a component is written with |lat| or with the
// wrong parity in latitude.
// ---------------------------------------------------------------------------
TEST(CorrectionsPoleTide, SouthernMirrorFlipsUpAndEastButNotNorth) {
    const double xp = xpMean(kT) + 0.1;
    const double yp = ypMean(kT);
    const Enu north_site = codePoleTide( 40.0, 20.0, { erp(kT, xp, yp) }, kT);
    const Enu south_site = codePoleTide(-40.0, 20.0, { erp(kT, xp, yp) }, kT);
    const Enu want = conventionalPoleTide(-40.0 * kDeg, 20.0 * kDeg, 0.1, 0.0);

    EXPECT_NEAR(south_site.e, want.e, kTol);
    EXPECT_NEAR(south_site.n, want.n, kTol);
    EXPECT_NEAR(south_site.u, want.u, kTol);

    EXPECT_GT(south_site.u, 0.0);
    EXPECT_LT(south_site.e, 0.0);
    EXPECT_LT(south_site.n, 0.0);
    EXPECT_NEAR(south_site.u, -north_site.u, kTol);
    EXPECT_NEAR(south_site.e, -north_site.e, kTol);
    EXPECT_NEAR(south_site.n,  north_site.n, kTol);
}

// ---------------------------------------------------------------------------
// The pole is interpolated linearly between ERP records and held at the
// nearest record outside them.  Records a day apart carry xp = mean + 0.05 and
// mean + 0.25 arcsec (mean taken at the midpoint); at the midpoint the wobble
// is m1 = +0.15.  Before the first and after the last record the first and the
// last values are used, with the mean pole of the query epoch; both held
// wobbles are non-zero, so the held displacement is millimetre-level at either
// end.  Fails if the nearest record is taken in place of the interpolation (the
// midpoint then shows m1 = 0.05 or 0.25), or if either end is extrapolated or
// refused (a refusal returns zero).
// ---------------------------------------------------------------------------
TEST(CorrectionsPoleTide, ErpIsInterpolatedBetweenRecordsAndHeldBeyondThem) {
    const GnssTime t_lo{ 2300, 0.0 };
    const GnssTime t_mid{ 2300, 43200.0 };
    const GnssTime t_hi{ 2300, 86400.0 };
    const double xp_lo = xpMean(t_mid) + 0.05;
    const double xp_hi = xpMean(t_mid) + 0.25;
    const double yp    = ypMean(t_mid);
    const std::vector<ErpData> erps = { erp(t_lo, xp_lo, yp), erp(t_hi, xp_hi, yp) };

    const double lat = 40.0 * kDeg, lon = 20.0 * kDeg;
    {
        const Enu got  = codePoleTide(40.0, 20.0, erps, t_mid);
        const Enu want = conventionalPoleTide(lat, lon, 0.15, 0.0);
        EXPECT_NEAR(got.e, want.e, kTol);
        EXPECT_NEAR(got.n, want.n, kTol);
        EXPECT_NEAR(got.u, want.u, kTol);
    }
    {
        const GnssTime before{ 2299, 604800.0 - 3600.0 };
        const Enu got  = codePoleTide(40.0, 20.0, erps, before);
        const Enu want = conventionalFromErp(lat, lon, before, xp_lo, yp);
        EXPECT_NEAR(got.u, want.u, kTol);
        EXPECT_NEAR(got.e, want.e, kTol);
        // -33 sin(80) (0.05 cos 20) = -1.53 mm: a refusal (zero) fails here.
        EXPECT_LT(got.u, -1.0e-3) << "before the first record the 0.05 arcsec wobble is held";
    }
    {
        const GnssTime after{ 2300, 86400.0 + 7200.0 };
        const Enu got  = codePoleTide(40.0, 20.0, erps, after);
        const Enu want = conventionalFromErp(lat, lon, after, xp_hi, yp);
        EXPECT_NEAR(got.u, want.u, kTol);
        EXPECT_NEAR(got.e, want.e, kTol);
        EXPECT_LT(got.u, -5.0e-3) << "after the last record the 0.25 arcsec wobble is held";
    }
}

// ---------------------------------------------------------------------------
// No displacement without an ERP product, for a position that is not a station,
// or for a negative time of week.  Fails if a default pole is invented.
// ---------------------------------------------------------------------------
TEST(CorrectionsPoleTide, MissingProductOrInvalidInputGivesZero) {
    LogManager log;
    const Vector3d rx = Coordinates::llhToEcef(40.0 * kDeg, 20.0 * kDeg, 0.0);

    GnssCorrections no_erp(log);
    EXPECT_TRUE((no_erp.computePoleTideCorrection(rx, kT)).isZero(0.0));

    GnssCorrections gc(log, std::vector<ClockData>{}, { erp(kT, xpMean(kT) + 0.1, ypMean(kT)) });
    EXPECT_TRUE((gc.computePoleTideCorrection(Vector3d(10.0, 0.0, 0.0), kT)).isZero(0.0));
    EXPECT_TRUE((gc.computePoleTideCorrection(rx, GnssTime{ 2300, -1.0 })).isZero(0.0));
    EXPECT_GT(gc.computePoleTideCorrection(rx, kT).norm(), 1.0e-3);   // control
}

// ---------------------------------------------------------------------------
// An implausible wobble (2 arcsec, ten times the real range) at 45 N, 0 E would
// give a 66 mm radial displacement; the code refuses anything above 5 cm and
// returns zero rather than a clipped vector.  Fails if the guard is removed or
// changed to a clip.
// ---------------------------------------------------------------------------
TEST(CorrectionsPoleTide, ImplausibleWobbleIsRefusedNotClipped) {
    const Enu want = conventionalPoleTide(45.0 * kDeg, 0.0, 2.0, 0.0);
    ASSERT_NEAR(want.u, -0.066, 1e-12) << "premise: the formula exceeds the 5 cm guard";

    LogManager log;
    GnssCorrections gc(log, std::vector<ClockData>{}, { erp(kT, xpMean(kT) + 2.0, ypMean(kT)) });
    const Vector3d rx = Coordinates::llhToEcef(45.0 * kDeg, 0.0, 0.0);
    EXPECT_TRUE((gc.computePoleTideCorrection(rx, kT)).isZero(0.0));
}

// ---------------------------------------------------------------------------
// With ocean pole tide coefficients set, the displacement is the rotational
// term plus the ocean load term (IERS Conventions 2010, eq. 7.29):
//     [u_r, u_n, u_e] = K { (m1 g_R + m2 g_I) [u^R] + (m2 g_R - m1 g_I) [u^I] }
// with m1, m2 in radians, g = 0.6870 + 0.0036 i, and
//     K = 4 pi G a rho_w H_p / (3 g_e),  H_p = sqrt(8 pi / 15) Omega^2 a^4 / GM.
// The coefficients are synthetic.  Fails if the ocean term is added with the
// wrong sign, onto the wrong component, or with the real and imaginary parts
// exchanged.
// ---------------------------------------------------------------------------
TEST(CorrectionsPoleTide, OceanLoadTermAddsOnTopOfTheRotationalTerm) {
    // [u_n^R, u_n^I, u_e^R, u_e^I, u_r^R, u_r^I]
    const std::array<double, 6> c = { 0.012, -0.004, -0.020, 0.006, -0.050, 0.010 };
    const double m1 = 0.1, m2 = 0.05;
    const double xp = xpMean(kT) + m1;
    const double yp = ypMean(kT) - m2;
    const double lat = 40.0 * kDeg, lon = 20.0 * kDeg;
    const Vector3d rx = Coordinates::llhToEcef(lat, lon, 0.0);

    LogManager log;
    GnssCorrections plain(log, std::vector<ClockData>{}, { erp(kT, xp, yp) });
    GnssCorrections loaded(log, std::vector<ClockData>{}, { erp(kT, xp, yp) });
    loaded.setOceanPoleTideCoefficients(c);
    ASSERT_TRUE(loaded.hasOceanPoleTideCoefficients());
    ASSERT_FALSE(plain.hasOceanPoleTideCoefficients());

    const Enu rot = toEnu(plain.computePoleTideCorrection(rx, kT), lat, lon);
    const Enu sum = toEnu(loaded.computePoleTideCorrection(rx, kT), lat, lon);

    const double G = 6.67428e-11, a = 6378136.6, rho = 1025.0, omega = 7.292115e-5;
    const double gm = 3.986004418e14, g_e = 9.7803278;
    const double hp = std::sqrt(8.0 * Constants::PI / 15.0) * omega * omega * std::pow(a, 4) / gm;
    const double K = 4.0 * Constants::PI * G * a * rho * hp / (3.0 * g_e);
    const double gR = 0.6870, gI = 0.0036;
    const double m1r = m1 * kAs2Rad, m2r = m2 * kAs2Rad;
    const double re = m1r * gR + m2r * gI;
    const double im = m2r * gR - m1r * gI;
    const double ocean_n = K * (re * c[0] + im * c[1]);
    const double ocean_e = K * (re * c[2] + im * c[3]);
    const double ocean_u = K * (re * c[4] + im * c[5]);
    ASSERT_GT(std::abs(ocean_u), 1.0e-5) << "premise: the ocean term is large enough to see";

    const Enu want_rot = conventionalPoleTide(lat, lon, m1, m2);
    EXPECT_NEAR(rot.e, want_rot.e, kTol);
    EXPECT_NEAR(rot.n, want_rot.n, kTol);
    EXPECT_NEAR(rot.u, want_rot.u, kTol);

    EXPECT_NEAR(sum.e - rot.e, ocean_e, kTol);
    EXPECT_NEAR(sum.n - rot.n, ocean_n, kTol);
    EXPECT_NEAR(sum.u - rot.u, ocean_u, kTol);

    // The static load term agrees with the formula on its own.
    const Vector3d opt = GnssCorrections::computeOceanPoleTideDisplacement(m1, m2, c);
    EXPECT_NEAR(opt.x(), ocean_e, kTol);
    EXPECT_NEAR(opt.y(), ocean_n, kTol);
    EXPECT_NEAR(opt.z(), ocean_u, kTol);
}

}  // namespace GnssProcessor
