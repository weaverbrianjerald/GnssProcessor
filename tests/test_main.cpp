/**
 * @file test_main.cpp
 * @brief Unit tests for GnssProcessor math-foundation modules.
 *
 * Scope: Coordinates, TimeUtils, Interpolate. These are the pure-math layers
 * the rest of the engine builds on (geodesy, time scales, polynomial /
 * spline / Hermite interpolation used by SP3, CLK and ionosphere products).
 *
 * Depth: smoke + key numerical invariants. Round-trips, identities at nodes,
 * and a small number of hand-computed reference values. Not a full coverage
 * sweep.
 *
 * Fixtures: most tests are synthetic. A single fixture-backed smoke test
 * validates TimeUtils against the SP3 product header under data/ (the file
 * carries both a calendar timestamp and the GPS week/TOW it should map to).
 * The fixture path is injected via the GNSS_TEST_DATA_DIR macro defined by
 * CMake; if the file is absent the test is skipped, not failed.
 *
 * NOT covered here (deferred):
 *  - WlsEstimator: its only public API (estimate) requires GnssConfig +
 *    StateManager + LogManager, which is not a unit-test surface; better
 *    exercised via integration tests.
 *  - Filter / measurement / ambiguity / smoother modules: tracked separately.
 */

#include <gtest/gtest.h>

#include "atmosphere/Gpt3.hpp"
#include "atmosphere/HigherOrderIonosphere.hpp"
#include "atmosphere/Troposphere.hpp"
#include "atmosphere/Vmf3.hpp"
#include "common/Constants.hpp"
#include "config/GnssConfig.hpp"
#include "corrections/GnssCorrections.hpp"
#include "logging/LogManager.hpp"
#include "math/Coordinates.hpp"
#include "math/Interpolate.hpp"
#include "math/TimeUtils.hpp"
#include "model/Antenna.hpp"
#include "products/ProductParser.hpp"

#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef GNSS_TEST_DATA_DIR
#define GNSS_TEST_DATA_DIR ""
#endif

namespace GnssProcessor {
namespace {

// ============================================================================
// Coordinates
// ============================================================================

// LLH -> ECEF -> LLH must round-trip to sub-millimeter precision for any
// reasonable terrestrial location. This is the core consistency check on
// the Bowring iteration and the WGS84 ellipsoid parameters.
TEST(CoordinatesTest, LlhEcefRoundTripVariousSites) {
    struct Site { double lat_deg; double lon_deg; double h_m; };
    const std::vector<Site> sites = {
        {  0.0,    0.0,    0.0 },     // Equator + prime meridian, on ellipsoid
        { 45.0,  -75.0,  100.0 },     // Northern mid-latitude
        {-33.86, 151.21,   58.0 },    // Sydney
        { 64.85, -147.83, 136.0 },    // Fairbanks (high latitude)
        { 27.99,  86.93, 8848.0 },    // Everest summit
        { -1.0, 179.5,    -50.0 },    // Negative height (below ellipsoid)
    };

    constexpr double DEG2RAD = Constants::PI / 180.0;
    for (const auto& s : sites) {
        Eigen::Vector3d ecef = Coordinates::llhToEcef(
            s.lat_deg * DEG2RAD, s.lon_deg * DEG2RAD, s.h_m);
        Coordinates::Llh llh = Coordinates::ecefToLlh(ecef);

        EXPECT_NEAR(llh.lat_rad, s.lat_deg * DEG2RAD, 1e-9)
            << "lat round-trip @ (" << s.lat_deg << "," << s.lon_deg << ")";
        EXPECT_NEAR(llh.lon_rad, s.lon_deg * DEG2RAD, 1e-9)
            << "lon round-trip @ (" << s.lat_deg << "," << s.lon_deg << ")";
        EXPECT_NEAR(llh.h_m, s.h_m, 1e-4)
            << "height round-trip @ (" << s.lat_deg << "," << s.lon_deg << ")";
    }
}

// At the equator on the ellipsoid the ECEF position must equal the
// semi-major axis along X. This pins the absolute coordinate convention.
TEST(CoordinatesTest, LlhToEcefEquatorOnEllipsoidIsSemiMajorAxis) {
    Eigen::Vector3d ecef = Coordinates::llhToEcef(0.0, 0.0, 0.0);
    EXPECT_NEAR(ecef.x(), Constants::Ellipsoid::WGS84::a, 1e-6);
    EXPECT_NEAR(ecef.y(), 0.0, 1e-6);
    EXPECT_NEAR(ecef.z(), 0.0, 1e-6);
}

// Pole handling: a point on the rotation axis above the surface should
// round-trip to lat = +/-pi/2 and a sane height.
TEST(CoordinatesTest, EcefToLlhNorthPole) {
    // Polar radius b = a * (1 - f). Add 1000 m of altitude.
    const double a = Constants::Ellipsoid::WGS84::a;
    const double f = Constants::Ellipsoid::WGS84::f;
    const double b = a * (1.0 - f);
    Eigen::Vector3d ecef(0.0, 0.0, b + 1000.0);

    Coordinates::Llh llh = Coordinates::ecefToLlh(ecef);
    EXPECT_NEAR(llh.lat_rad, Constants::PI / 2.0, 1e-9);
    EXPECT_NEAR(llh.h_m, 1000.0, 1e-3);
}

// ENU rotation matrix must be orthonormal (R^T R = I, det = +1).
TEST(CoordinatesTest, EnuRotationMatrixIsOrthonormal) {
    constexpr double DEG2RAD = Constants::PI / 180.0;
    Eigen::Matrix3d R = Coordinates::enuToEcefMatrix(45.0 * DEG2RAD, -75.0 * DEG2RAD);

    Eigen::Matrix3d should_be_identity = R.transpose() * R;
    Eigen::Matrix3d I = Eigen::Matrix3d::Identity();
    EXPECT_TRUE(should_be_identity.isApprox(I, 1e-12));
    EXPECT_NEAR(R.determinant(), 1.0, 1e-12);
}

// The ENU "Up" axis at a given (lat,lon) must be the outward radial unit
// vector at the corresponding ellipsoid point. This is the geometric
// definition of local-up.
TEST(CoordinatesTest, EnuUpIsRadialOutward) {
    constexpr double DEG2RAD = Constants::PI / 180.0;
    const double lat = 30.0 * DEG2RAD;
    const double lon = 45.0 * DEG2RAD;

    Eigen::Vector3d up_ecef = Coordinates::enuToEcefMatrix(lat, lon).col(2);

    // Expected up vector from spherical formulation
    Eigen::Vector3d expected(
        std::cos(lat) * std::cos(lon),
        std::cos(lat) * std::sin(lon),
        std::sin(lat));

    EXPECT_TRUE(up_ecef.isApprox(expected, 1e-12));
}

// Round-trip: ENU -> ECEF -> ENU at a given reference point.
TEST(CoordinatesTest, EnuEcefRoundTrip) {
    constexpr double DEG2RAD = Constants::PI / 180.0;
    Coordinates::Llh ref{ 40.0 * DEG2RAD, -100.0 * DEG2RAD, 1500.0 };

    Eigen::Vector3d enu_in(123.4, -67.8, 9.0);
    Eigen::Vector3d ecef = Coordinates::enuToEcef(enu_in, ref);
    Eigen::Vector3d enu_out = Coordinates::ecefToEnu(ecef, ref);

    EXPECT_TRUE(enu_out.isApprox(enu_in, 1e-9));
}

// ============================================================================
// TimeUtils
// ============================================================================

// SP3 / CLK products carry timestamps already on the GPS time scale, so
// gregorianGpsToGpsTime must NOT add leap seconds. The fixture under
// data/products/sp3/ has header lines stating both 2024-05-08 00:00:00
// and "GPS week 2313, TOW 259200" -- those values are the ground truth.
TEST(TimeUtilsTest, GregorianGpsToGpsTimeMatchesSp3Header) {
    GnssTime t = TimeUtils::gregorianGpsToGpsTime(2024, 5, 8, 0, 0, 0.0);
    EXPECT_EQ(t.week, 2313);
    EXPECT_NEAR(t.tow, 259200.0, 1e-9);
}

// gregorianToGpsTime treats input as UTC and applies the leap-second
// offset (18 s after 2017-01-01). So the same calendar moment as above,
// interpreted as UTC, should give TOW 259200 + 18 = 259218.
TEST(TimeUtilsTest, GregorianUtcToGpsTimeAppliesLeapSeconds) {
    GnssTime t = TimeUtils::gregorianToGpsTime(2024, 5, 8, 0, 0, 0.0);
    EXPECT_EQ(t.week, 2313);
    EXPECT_NEAR(t.tow, 259200.0 + 18.0, 1e-9);
}

// GPS-time round-trip via Gregorian. The conversion goes through UTC
// (gpsTimeToGregorian subtracts leap seconds), so we re-apply them on
// the way back with gregorianToGpsTime to close the loop.
TEST(TimeUtilsTest, GpsTimeGregorianRoundTrip) {
    GnssTime original{ 2200, 123456.789 };

    int y, m, d, hh, mm; double ss;
    TimeUtils::gpsTimeToGregorian(original, y, m, d, hh, mm, ss);

    GnssTime back = TimeUtils::gregorianToGpsTime(y, m, d, hh, mm, ss);
    EXPECT_EQ(back.week, original.week);
    EXPECT_NEAR(back.tow, original.tow, 1e-3);
}

// JD <-> Gregorian round-trip on a non-trivial date.
TEST(TimeUtilsTest, JulianDayGregorianRoundTrip) {
    const int year = 2024, month = 5, day = 8;
    const int hour = 12, minute = 34;
    const double second = 56.789;

    double jd = TimeUtils::gregorianToJulianDay(year, month, day, hour, minute, second);

    int y, m, d, hh, mm; double ss;
    TimeUtils::julianDayToGregorian(jd, y, m, d, hh, mm, ss);

    EXPECT_EQ(y, year);
    EXPECT_EQ(m, month);
    EXPECT_EQ(d, day);
    EXPECT_EQ(hh, hour);
    EXPECT_EQ(mm, minute);
    EXPECT_NEAR(ss, second, 1e-3);
}

// Leap-second table sanity:
//  - At the GPS epoch (1980-01-06) the offset is 0.
//  - After 2017-01-01 the offset is 18 (current era).
//  - Before the GPS epoch the function returns 0 (degenerate but documented).
TEST(TimeUtilsTest, LeapSecondsLookup) {
    EXPECT_DOUBLE_EQ(TimeUtils::leapSecondsGpsUtc(TimeUtils::GPS_EPOCH_JD), 0.0);

    GnssTime t_2024{ 2313, 259200.0 };
    double jd_2024 = TimeUtils::gpsTimeToJulianDate(t_2024);
    EXPECT_DOUBLE_EQ(TimeUtils::leapSecondsGpsUtc(jd_2024), 18.0);

    EXPECT_DOUBLE_EQ(TimeUtils::leapSecondsGpsUtc(0.0), 0.0);

    // TAI - UTC = (GPS - UTC) + 19, by definition.
    EXPECT_DOUBLE_EQ(
        TimeUtils::leapSecondsTaiUtc(jd_2024),
        TimeUtils::leapSecondsGpsUtc(jd_2024) + TimeUtils::TAI_GPS_OFFSET);
}

// Day-of-year arithmetic: leap-year handling.
TEST(TimeUtilsTest, DayOfYearToDateLeapAndNonLeap) {
    int month = 0, day = 0;

    // 2024 is a leap year. DOY 60 == Feb 29.
    ASSERT_TRUE(TimeUtils::dayOfYearToDate(2024, 60, month, day));
    EXPECT_EQ(month, 2);
    EXPECT_EQ(day, 29);

    // 2023 is not. DOY 60 == Mar 1.
    ASSERT_TRUE(TimeUtils::dayOfYearToDate(2023, 60, month, day));
    EXPECT_EQ(month, 3);
    EXPECT_EQ(day, 1);

    // DOY 129 in 2024 corresponds to the SP3 / RINEX fixture date 2024-05-08.
    ASSERT_TRUE(TimeUtils::dayOfYearToDate(2024, 129, month, day));
    EXPECT_EQ(month, 5);
    EXPECT_EQ(day, 8);

    // Out-of-range DOY rejected.
    EXPECT_FALSE(TimeUtils::dayOfYearToDate(2023, 366, month, day));
    EXPECT_FALSE(TimeUtils::dayOfYearToDate(2023, 0, month, day));
}

TEST(TimeUtilsTest, IsValidGregorianDateEdgeCases) {
    EXPECT_TRUE(TimeUtils::isValidGregorianDate(2024, 2, 29, 0, 0, 0.0));   // Leap day OK
    EXPECT_FALSE(TimeUtils::isValidGregorianDate(2023, 2, 29, 0, 0, 0.0));  // Not leap
    EXPECT_FALSE(TimeUtils::isValidGregorianDate(2024, 13, 1, 0, 0, 0.0));  // Bad month
    EXPECT_FALSE(TimeUtils::isValidGregorianDate(2024, 4, 31, 0, 0, 0.0));  // April has 30
    EXPECT_FALSE(TimeUtils::isValidGregorianDate(2024, 1, 1, 24, 0, 0.0));  // Bad hour
    EXPECT_FALSE(TimeUtils::isValidGregorianDate(2024, 1, 1, 0, 0, 60.0));  // Bad seconds

    // The validity floor is 1970, NOT the GPS epoch (1980-01-06). Pre-GPS-epoch
    // dates back to 1970 are accepted by design so Block I satellite ATX
    // "VALID FROM" lines (from 1978) parse. See commit a2fa2ab and the comment
    // in TimeUtils::isValidGregorianDate.
    EXPECT_TRUE(TimeUtils::isValidGregorianDate(1979, 12, 31, 0, 0, 0.0));  // Pre-GPS epoch, >= 1970: OK
    EXPECT_FALSE(TimeUtils::isValidGregorianDate(1969, 12, 31, 0, 0, 0.0)); // Before 1970 floor: rejected
}

// continuousGpsSeconds must be monotonically increasing and wrap correctly
// across the 604800 s week boundary.
TEST(TimeUtilsTest, ContinuousGpsSecondsMonotonic) {
    GnssTime a{ 2200, 100.0 };
    GnssTime b{ 2200, 200.0 };
    GnssTime c{ 2201, 0.0 };

    double sa = TimeUtils::continuousGpsSeconds(a);
    double sb = TimeUtils::continuousGpsSeconds(b);
    double sc = TimeUtils::continuousGpsSeconds(c);

    EXPECT_LT(sa, sb);
    EXPECT_LT(sb, sc);
    EXPECT_NEAR(sc - TimeUtils::continuousGpsSeconds({ 2200, 604800.0 - 1.0 }), 1.0, 1e-9);
}

// J2000 epoch sanity check: jdToJ2000Centuries(J2000_JD) == 0.
TEST(TimeUtilsTest, J2000CenturiesAtJ2000IsZero) {
    EXPECT_NEAR(TimeUtils::jdToJ2000Centuries(TimeUtils::J2000_JD), 0.0, 1e-15);
    EXPECT_NEAR(TimeUtils::mjdToDecimalYear(TimeUtils::J2000_MJD), 2000.0, 1e-12);
}

// GPS -> TT scale conversion is a constant 51.184 seconds.
TEST(TimeUtilsTest, GpsToTtJdOffsetIsConstant) {
    const double gps_jd = 2460438.5;  // arbitrary 2024 JD
    double tt_jd = TimeUtils::gpsJdToTtJd(gps_jd);
    // Recovering a sub-second offset by differencing two ~2.46e6-magnitude
    // Julian Dates is limited by double precision: the ULP at this magnitude is
    // ~4.7e-10 days (~4e-5 s), so the round-trip cannot resolve the offset to
    // better than ~1e-5 s. The tolerance reflects that floor, not a model error
    // — the applied offset is exactly TT_GPS_OFFSET (51.184 s).
    EXPECT_NEAR((tt_jd - gps_jd) * 86400.0, TimeUtils::TT_GPS_OFFSET, 1e-4);
}

// BDS time = GPS - 14 s, so a BDS calendar moment converted to GPS time
// should add 14 s of TOW (no week roll on a non-boundary moment).
TEST(TimeUtilsTest, GnssTimeFromDateBdsAdds14Seconds) {
    GnssTime gps = TimeUtils::GnssTimeFromDate(2024, 5, 8, 0, 0, 0.0, "GPS");
    GnssTime bds = TimeUtils::GnssTimeFromDate(2024, 5, 8, 0, 0, 0.0, "BDS");
    EXPECT_EQ(bds.week, gps.week);
    EXPECT_NEAR(bds.tow - gps.tow, 14.0, 1e-9);
}

// ============================================================================
// Interpolate
// ============================================================================

// Edge cases: empty input / single node.
TEST(InterpolateTest, BarycentricWeightsDegenerateInputs) {
    EXPECT_TRUE(Interpolate::barycentricWeights({}).empty());

    auto w = Interpolate::barycentricWeights({ 5.0 });
    ASSERT_EQ(w.size(), 1u);
    EXPECT_DOUBLE_EQ(w[0], 1.0);
}

// Defining identity at nodes: barycentricEval at any node x[j] returns y[j]
// exactly (the implementation special-cases this to avoid division by zero).
TEST(InterpolateTest, BarycentricEvalIsExactAtNodes) {
    std::vector<double> x = { 0.0, 1.0, 2.5, 4.0 };
    std::vector<double> y = { 3.0, -2.0, 7.5, 0.0 };
    auto w = Interpolate::barycentricWeights(x);
    ASSERT_EQ(w.size(), x.size());

    for (size_t j = 0; j < x.size(); ++j) {
        double yq = Interpolate::barycentricEval(x, w, y, x[j]);
        EXPECT_NEAR(yq, y[j], 1e-12) << "node index " << j;
    }
}

// On nodes drawn from a low-degree polynomial, barycentric Lagrange
// interpolation is exact (since the interpolant is the same polynomial).
TEST(InterpolateTest, BarycentricEvalExactForPolynomial) {
    auto poly = [](double x) { return 0.5 * x * x * x - 2.0 * x * x + x + 4.0; };

    std::vector<double> x = { -2.0, -0.5, 1.0, 2.0, 3.5 };
    std::vector<double> y;
    y.reserve(x.size());
    for (double xi : x) y.push_back(poly(xi));

    auto w = Interpolate::barycentricWeights(x);

    for (double xq : { -1.7, 0.25, 1.7, 3.0 }) {
        double got = Interpolate::barycentricEval(x, w, y, xq);
        EXPECT_NEAR(got, poly(xq), 1e-9) << "xq=" << xq;
    }
}

// Derivative on a quadratic: p(x) = a x^2 + b x + c => p'(x) = 2 a x + b.
// barycentricDeriv must reproduce this exactly.
TEST(InterpolateTest, BarycentricDerivExactOnQuadratic) {
    const double a = 0.7, b = -1.3, c = 2.0;
    auto p = [&](double x) { return a * x * x + b * x + c; };
    auto dp = [&](double x) { return 2.0 * a * x + b; };

    std::vector<double> x = { -1.5, 0.0, 1.0, 2.5 };
    std::vector<double> y;
    for (double xi : x) y.push_back(p(xi));
    auto w = Interpolate::barycentricWeights(x);

    for (double xq : { -1.0, 0.5, 1.7 }) {
        double dgot = Interpolate::barycentricDeriv(x, w, y, xq);
        EXPECT_NEAR(dgot, dp(xq), 1e-9) << "xq=" << xq;
    }
}

// Linear interp matches the obvious analytical answer.
TEST(InterpolateTest, LinearInterpInteriorAndExtrapolation) {
    std::vector<double> x = { 0.0, 1.0, 3.0 };
    std::vector<double> y = { 0.0, 2.0, 8.0 };

    EXPECT_NEAR(Interpolate::linearInterp(x, y, 0.5), 1.0, 1e-12);  // between 0 and 1
    EXPECT_NEAR(Interpolate::linearInterp(x, y, 2.0), 5.0, 1e-12);  // between 1 and 3
    // Extrapolation uses the boundary segment slope.
    EXPECT_NEAR(Interpolate::linearInterp(x, y, -1.0), -2.0, 1e-12); // slope = 2 => y = 0 - 2
    EXPECT_NEAR(Interpolate::linearInterp(x, y, 4.0), 11.0, 1e-12);  // slope = 3 => y = 8 + 3
}

TEST(InterpolateTest, LinearInterpDegenerateInputs) {
    EXPECT_DOUBLE_EQ(Interpolate::linearInterp({}, {}, 1.0), 0.0);
    EXPECT_DOUBLE_EQ(Interpolate::linearInterp({ 1.0, 2.0 }, { 3.0 }, 1.5), 0.0); // size mismatch
    EXPECT_DOUBLE_EQ(Interpolate::linearInterp({ 5.0 }, { 7.0 }, 9999.0), 7.0);   // single node
}

// Natural cubic spline reproduces a linear function exactly.
TEST(InterpolateTest, SplineEvalExactOnLinear) {
    auto f = [](double x) { return 3.0 * x - 5.0; };
    std::vector<double> x = { 0.0, 1.0, 2.0, 4.0, 7.5 };
    std::vector<double> y;
    for (double xi : x) y.push_back(f(xi));

    auto c = Interpolate::naturalSplineSecondDerivs(x, y);
    ASSERT_EQ(c.size(), x.size());

    for (double xq : { 0.5, 1.5, 3.0, 5.5 }) {
        EXPECT_NEAR(Interpolate::splineEval(x, y, c, xq), f(xq), 1e-9) << "xq=" << xq;
    }

    // Slope on a linear function is constant.
    for (double xq : { 0.5, 1.5, 5.5 }) {
        EXPECT_NEAR(Interpolate::splineDeriv(x, y, c, xq), 3.0, 1e-9) << "xq=" << xq;
    }
}

// Spline must equal the node ordinates at the nodes themselves.
TEST(InterpolateTest, SplineEvalIsExactAtNodes) {
    std::vector<double> x = { 0.0, 1.0, 2.5, 4.0, 6.0 };
    std::vector<double> y = { 1.0, 0.5, -1.2, 2.7, 4.0 };

    auto c = Interpolate::naturalSplineSecondDerivs(x, y);
    for (size_t i = 0; i < x.size(); ++i) {
        EXPECT_NEAR(Interpolate::splineEval(x, y, c, x[i]), y[i], 1e-12)
            << "node index " << i;
    }
}

// Cubic Hermite (Catmull-Rom) is exact at the nodes by construction.
TEST(InterpolateTest, HermiteEvalExactAtNodes) {
    std::vector<double> x = { 0.0, 1.0, 2.0, 3.5, 5.0 };
    std::vector<double> y = { 0.5, -0.2, 1.1, 0.8, -1.5 };

    auto m = Interpolate::catmullRomSlopes(x, y);
    ASSERT_EQ(m.size(), x.size());

    for (size_t i = 0; i < x.size(); ++i) {
        EXPECT_NEAR(Interpolate::hermiteEval(x, y, m, x[i]), y[i], 1e-12)
            << "node index " << i;
    }
}

// Catmull-Rom slopes use central differences at interior nodes:
//   m[i] = (y[i+1] - y[i-1]) / (x[i+1] - x[i-1]).
TEST(InterpolateTest, CatmullRomInteriorSlopeIsCentralDifference) {
    std::vector<double> x = { 0.0, 1.0, 3.0, 6.0 };
    std::vector<double> y = { 1.0, 4.0, 9.0, 25.0 };

    auto m = Interpolate::catmullRomSlopes(x, y);
    ASSERT_EQ(m.size(), 4u);

    EXPECT_NEAR(m[1], (y[2] - y[0]) / (x[2] - x[0]), 1e-12);
    EXPECT_NEAR(m[2], (y[3] - y[1]) / (x[3] - x[1]), 1e-12);
}

// Neville's algorithm yields the unique polynomial through the nodes;
// on a quadratic it must reproduce both value and derivative exactly.
TEST(InterpolateTest, NevilleInterpDerivExactOnQuadratic) {
    auto p = [](double x) { return 1.5 * x * x - 4.0 * x + 2.0; };
    auto dp = [](double x) { return 3.0 * x - 4.0; };

    std::vector<double> x = { -1.0, 0.5, 2.0, 3.0 };
    std::vector<double> y;
    for (double xi : x) y.push_back(p(xi));

    for (double xq : { -0.5, 1.0, 2.5 }) {
        double val = 0.0, deriv = 0.0;
        Interpolate::nevilleInterpDeriv(x, y, xq, val, deriv);
        EXPECT_NEAR(val, p(xq), 1e-9) << "val @ xq=" << xq;
        EXPECT_NEAR(deriv, dp(xq), 1e-9) << "deriv @ xq=" << xq;
    }
}

TEST(InterpolateTest, NevilleHandlesEmptyInput) {
    double val = 99.0, deriv = 99.0;
    Interpolate::nevilleInterpDeriv({}, {}, 0.0, val, deriv);
    EXPECT_DOUBLE_EQ(val, 0.0);
    EXPECT_DOUBLE_EQ(deriv, 0.0);
}

// ============================================================================
// Fixture-backed smoke test
// ============================================================================

// The SP3 product file has two ground-truth time references in its header:
//   line 1: "#dP YYYY MM DD HH MM SS.ssss ..."  (calendar, GPS time scale)
//   line 2: "## week tow.ssss interval mjd ..." (GPS week + TOW)
// If TimeUtils::gregorianGpsToGpsTime on the calendar produces the week/tow,
// the time conversion is consistent with how IGS/CODE products are time-
// tagged. This is the most basic real-data smoke test for TimeUtils.
TEST(FixtureSmokeTest, Sp3HeaderTimeConversionMatches) {
    namespace fs = std::filesystem;

    const std::string data_dir = GNSS_TEST_DATA_DIR;
    if (data_dir.empty()) {
        GTEST_SKIP() << "GNSS_TEST_DATA_DIR not defined; skipping fixture test.";
    }

    fs::path sp3 = fs::path(data_dir)
        / "products" / "sp3"
        / "COD0MGXFIN_20241290000_01D_05M_ORB.SP3";

    if (!fs::exists(sp3)) {
        GTEST_SKIP() << "SP3 fixture not present at " << sp3.string();
    }

    std::ifstream in(sp3);
    ASSERT_TRUE(in.is_open()) << "Could not open " << sp3.string();

    std::string line1, line2;
    ASSERT_TRUE(std::getline(in, line1)) << "Could not read SP3 line 1";
    ASSERT_TRUE(std::getline(in, line2)) << "Could not read SP3 line 2";

    // Line 1: "#dPYYYY MM DD HH MM SS.sssssss ..."
    //         char positions are SP3-c/d fixed-width; we tokenize after the
    //         "#dP" tag and let the stream skip whitespace.
    ASSERT_GE(line1.size(), 30u) << "SP3 line 1 unexpectedly short";
    ASSERT_EQ(line1.substr(0, 3), "#dP") << "SP3 line 1 does not start with #dP";

    int year = 0, month = 0, day = 0, hour = 0, minute = 0;
    double second = 0.0;
    {
        std::istringstream iss(line1.substr(3));
        ASSERT_TRUE(iss >> year >> month >> day >> hour >> minute >> second)
            << "Could not parse SP3 calendar header";
    }

    // Line 2: "## week tow.ssss interval mjd ..."
    int header_week = 0;
    double header_tow = 0.0;
    {
        std::string tag;
        std::istringstream iss(line2);
        ASSERT_TRUE(iss >> tag >> header_week >> header_tow)
            << "Could not parse SP3 week/tow header";
        EXPECT_EQ(tag, "##");
    }

    GnssTime computed = TimeUtils::gregorianGpsToGpsTime(
        year, month, day, hour, minute, second);

    EXPECT_EQ(computed.week, header_week)
        << "SP3 calendar -> GPS week mismatch";
    EXPECT_NEAR(computed.tow, header_tow, 1e-3)
        << "SP3 calendar -> GPS TOW mismatch";
}

// ============================================================================
// Gpt3 — pressure/temperature lookup from the 5° grid
// ============================================================================

// Shared GPT3 fixture: spdlog's global logger registry doesn't allow
// re-registering "gnss_processor", and the binary grid load (~1 MB) is
// the same for every test, so one LogManager + one loaded grid is reused
// across the suite.
class Gpt3Test : public ::testing::Test {
protected:
    static LogManager log_mgr_;
    static Gpt3       model_;
    static bool       have_fixture_;

    static void SetUpTestSuite() {
        namespace fs = std::filesystem;
        const std::string data_dir = GNSS_TEST_DATA_DIR;
        if (data_dir.empty()) return;
        fs::path grid = fs::path(data_dir) / "static" / "gpt3" / "gpt3_5.grd";
        if (!fs::exists(grid)) return;

        const auto log_dir = fs::temp_directory_path() / "gnss_gpt3_test_logs";
        fs::create_directories(log_dir);
        log_mgr_.initialize(log_dir, LogLevel::Warn);
        have_fixture_ = model_.load(grid, log_mgr_);
    }

    void SetUp() override {
        if (!have_fixture_) GTEST_SKIP() << "GPT3 grid fixture not present";
    }
};
LogManager Gpt3Test::log_mgr_;
Gpt3       Gpt3Test::model_;
bool       Gpt3Test::have_fixture_ = false;

// A query at the exact first grid cell (87.5°N, 2.5°E) at the cell's own
// orthometric height should reproduce the harmonic value built from the
// line-2 a0/A1/B1/A2/B2 columns — independent of the load path, so this
// pins both the parse order and the harmonic evaluation.
TEST_F(Gpt3Test, QueryAtGridCornerMatchesHarmonic) {
    EXPECT_TRUE(model_.isLoaded());

    // Line 2 of gpt3_5.grd, copied verbatim into the expected coefficients:
    //   lat=87.5  lon=2.5
    //   p:  a0=101421   A1=21   B1=409   A2=-217   B2=-122   [Pa]
    //   T:  a0=259.2    A1=-13.2 B1=-6.1 A2=2.6    B2=0.3    [K]
    //   undu=19.36  Hs=-0.00
    constexpr double doy = 100.0;
    constexpr double omega = 2.0 * Constants::PI / 365.25;
    const double c1 = std::cos(omega * doy);
    const double s1 = std::sin(omega * doy);
    const double c2 = std::cos(2.0 * omega * doy);
    const double s2 = std::sin(2.0 * omega * doy);

    const double p_expected_Pa = 101421.0 + 21.0 * c1 + 409.0 * s1 + (-217.0) * c2 + (-122.0) * s2;
    const double T_expected_K  = 259.2 + (-13.2) * c1 + (-6.1) * s1 + 2.6 * c2 + 0.3 * s2;

    // Query at exactly the grid corner so bilinear weights collapse to (1,0,0,0).
    // Use the cell's own orthometric height (h_ellipsoidal = Hs + undu = 19.36 m)
    // so the barometric / lapse reduction is a no-op.
    Gpt3::Result r{};
    ASSERT_TRUE(model_.query(87.5, 2.5, /*h_ellipsoidal_m=*/19.36, doy, r));

    EXPECT_NEAR(r.pressure_mbar, p_expected_Pa * 0.01, 1e-4);
    EXPECT_NEAR(r.temperature_K, T_expected_K, 1e-4);
}

// Sanity: pressure must decrease with height (standard barometric form).
TEST_F(Gpt3Test, PressureDecreasesWithHeight) {
    Gpt3::Result lo{}, hi{};
    ASSERT_TRUE(model_.query(40.0, -75.0,    0.0, 200.0, lo));
    ASSERT_TRUE(model_.query(40.0, -75.0, 3000.0, 200.0, hi));

    // 3 km altitude difference should give roughly P_hi ≈ 0.7 × P_lo with
    // the standard 5.225 exponent; the precise factor is not the test —
    // monotone decrease is.
    EXPECT_LT(hi.pressure_mbar, lo.pressure_mbar);
    EXPECT_GT(hi.pressure_mbar, 0.5 * lo.pressure_mbar);  // sanity floor
    EXPECT_LT(hi.pressure_mbar, 0.9 * lo.pressure_mbar);  // sanity ceiling
}

// Longitude wrap: querying at lon=358° and lon=-2° (same physical
// location) must produce identical pressure and temperature.
TEST_F(Gpt3Test, LongitudeWrapsConsistently) {
    Gpt3::Result a{}, b{};
    ASSERT_TRUE(model_.query(30.0,  358.0, 100.0, 50.0, a));
    ASSERT_TRUE(model_.query(30.0,   -2.0, 100.0, 50.0, b));
    EXPECT_NEAR(a.pressure_mbar, b.pressure_mbar, 1e-9);
    EXPECT_NEAR(a.temperature_K, b.temperature_K, 1e-9);
}

// ============================================================================
// Vmf3 — empirical mapping function from GPT3 a + SH-synthesized b/c
// ============================================================================

// End-to-end VMF3 evaluation against an independent Python port of the
// same algorithm (scripts/vmf3_reference.py).  Both implementations
// read the same Vmf3Coefficients.hpp constants, so this pins:
//  - Legendre recursion (V, W tables)
//  - Spherical-harmonic synthesis loop and (n,m) ordering
//  - Seasonal harmonic evaluation
//  - Marini continued fraction
// to the canonical vmf3.m algorithm to full double precision.
TEST(Vmf3Test, MatchesPythonReference) {
    // Inputs intentionally avoid the equator / prime meridian / DOY 0
    // edge cases so the SH terms exercise non-trivial values.
    constexpr double lat = 0.5;                   // rad (~28.6°N)
    constexpr double lon = 1.0;                   // rad (~57.3°E)
    constexpr double doy = 100.0;
    const     double el  = 30.0 * Constants::PI / 180.0;
    constexpr double a_h = 0.00125;
    constexpr double a_w = 0.00060;

    // Reference values from scripts/vmf3_reference.py executed against
    // the same Vmf3Coefficients.hpp this binary links.
    constexpr double mf_d_ref = 1.9926259863958744;
    constexpr double mf_w_ref = 1.9964305176721975;

    double mf_d = 0.0, mf_w = 0.0;
    Vmf3::computeMapping(lat, lon, doy, el, a_h, a_w, mf_d, mf_w);

    EXPECT_NEAR(mf_d, mf_d_ref, 1e-12);
    EXPECT_NEAR(mf_w, mf_w_ref, 1e-12);
}

// Sanity: at zenith both mapping functions reduce to 1 because Marini
// at sin(el)=1 with positive a/b/c collapses to 1.
TEST(Vmf3Test, AtZenithIsUnity) {
    const double el_zenith = Constants::PI / 2.0;
    double mf_d = 0.0, mf_w = 0.0;
    Vmf3::computeMapping(0.7, 0.3, 150.0, el_zenith, 0.00128, 0.00057, mf_d, mf_w);
    EXPECT_NEAR(mf_d, 1.0, 1e-10);
    EXPECT_NEAR(mf_w, 1.0, 1e-10);
}

// Sanity: at low elevation the mapping factor grows large (cosec-like).
// 10° elevation gives 1/sin ≈ 5.76 as a lower bound; mf is slightly
// larger due to the (a,b,c) correction.
TEST(Vmf3Test, GrowsAtLowElevation) {
    const double el_low = 10.0 * Constants::PI / 180.0;
    double mf_d = 0.0, mf_w = 0.0;
    Vmf3::computeMapping(0.7, 0.3, 150.0, el_low, 0.00128, 0.00057, mf_d, mf_w);
    EXPECT_GT(mf_d, 5.0);
    EXPECT_LT(mf_d, 7.0);
    EXPECT_GT(mf_w, 5.0);
    EXPECT_LT(mf_w, 7.0);
}

// ============================================================================
// Earth-rotation pre-correction for SP3 orbit interpolation
// ============================================================================
//
// Geometry::getSatellitePV rotates each SP3 node's (X, Y) into the ECEF
// frame at the query epoch before fitting the Neville polynomial — the
// RTKLIB / cssrlib / IGS convention — and then converts the polynomial's
// inertial-frame velocity back to ECEF via v_ecef = v_inertial − ω×pos.
//
// This test exercises that arithmetic on a synthetic orbit with known
// analytical truth and confirms:
//   (a) the pre-rotated polynomial recovers ECEF position at an off-node
//       query to sub-mm;
//   (b) the inertial-to-ECEF velocity correction reproduces ECEF
//       velocity to mm/s.
//
// Note: we deliberately do NOT assert "pre-rotation has smaller error
// than no pre-rotation" on this trajectory.  For a circular equatorial
// orbit, the ECEF frequency is the beat |ω−n| and the inertial frequency
// is n (about 2× larger for GPS) — so on this particular toy case the
// pre-rotated polynomial fits a higher-frequency sinusoid and the
// comparison can go either way.  The real-world benefit of pre-rotation
// is for inclined / perturbed orbits where ECEF picks up an (ω+n)
// component whose amplitude is much larger than the smooth inertial
// trajectory; that's outside the scope of a deterministic synthetic
// fixture.
TEST(OrbitInterpTest, EarthRotationPreCorrectionGivesCorrectECEFState) {
    constexpr double OMEGA_E = 7.2921151467e-5;   // rad/s  (Constants::EARTH_ANGULAR_VELOCITY)
    constexpr double a       = 26.5610e6;         // m      (GPS orbital radius)
    constexpr double n_mean  = 1.45842e-4;        // rad/s  (GPS mean motion, period ≈ 12 h)
    constexpr double phi0    = 0.7;               // arbitrary phase
    constexpr double dt_sp3  = 900.0;             // s      (15 min, IGS Final)
    constexpr int    N       = 11;                // 11 nodes, degree-10 polynomial

    auto pos_eci = [&](double t) {
        const double a_ang = n_mean * t + phi0;
        return Eigen::Vector3d(a * std::cos(a_ang), a * std::sin(a_ang), 0.0);
    };
    auto vel_eci = [&](double t) {
        const double a_ang = n_mean * t + phi0;
        return Eigen::Vector3d(-a * n_mean * std::sin(a_ang),
                                a * n_mean * std::cos(a_ang),
                                0.0);
    };
    auto eci_to_ecef_at = [&](double t, const Eigen::Vector3d& v_eci) {
        // R_z(−ω·t) applied to v_eci.
        const double cs = std::cos(OMEGA_E * t);
        const double sn = std::sin(OMEGA_E * t);
        return Eigen::Vector3d( cs * v_eci.x() + sn * v_eci.y(),
                               -sn * v_eci.x() + cs * v_eci.y(),
                                v_eci.z());
    };
    auto pos_ecef = [&](double t) { return eci_to_ecef_at(t, pos_eci(t)); };
    auto vel_ecef = [&](double t) {
        // v_ecef = R(−ω t)·v_eci + ω·R'(−ω t)·r_eci
        //        = R(−ω t)·v_eci − ω × pos_ecef
        const Eigen::Vector3d p = pos_ecef(t);
        const Eigen::Vector3d v_inertial_in_ecef = eci_to_ecef_at(t, vel_eci(t));
        return Eigen::Vector3d(v_inertial_in_ecef.x() + OMEGA_E * p.y(),
                               v_inertial_in_ecef.y() - OMEGA_E * p.x(),
                               v_inertial_in_ecef.z());
    };

    // Centered 11-node window with off-node query time strictly inside.
    const double t_q = 5.0 * dt_sp3 + 233.7;
    std::vector<double>          t_node(N);
    std::vector<Eigen::Vector3d> p_node(N);
    for (int i = 0; i < N; ++i) {
        t_node[i]  = i * dt_sp3;
        p_node[i]  = pos_ecef(t_node[i]);
    }

    auto neville3 = [&](const std::vector<double>& x,
                        const std::vector<Eigen::Vector3d>& y,
                        double xq,
                        Eigen::Vector3d& pos_out,
                        Eigen::Vector3d& vel_out) {
        std::vector<double> yx(N), yy(N), yz(N);
        for (int i = 0; i < N; ++i) {
            yx[i] = y[i].x(); yy[i] = y[i].y(); yz[i] = y[i].z();
        }
        double pxo, pyo, pzo, vxo, vyo, vzo;
        Interpolate::nevilleInterpDeriv(x, yx, xq, pxo, vxo);
        Interpolate::nevilleInterpDeriv(x, yy, xq, pyo, vyo);
        Interpolate::nevilleInterpDeriv(x, yz, xq, pzo, vzo);
        pos_out = Eigen::Vector3d(pxo, pyo, pzo);
        vel_out = Eigen::Vector3d(vxo, vyo, vzo);
    };

    // --- Apply the same arithmetic Geometry::getSatellitePV uses ---------
    std::vector<Eigen::Vector3d> p_node_rot(N);
    for (int i = 0; i < N; ++i) {
        const double alpha = OMEGA_E * (t_node[i] - t_q);
        const double cs = std::cos(alpha), sn = std::sin(alpha);
        p_node_rot[i] = Eigen::Vector3d(cs * p_node[i].x() - sn * p_node[i].y(),
                                        sn * p_node[i].x() + cs * p_node[i].y(),
                                        p_node[i].z());
    }
    Eigen::Vector3d pos_interp, v_inertial;
    neville3(t_node, p_node_rot, t_q, pos_interp, v_inertial);
    // v_ecef = v_inertial − ω × pos  (matches getSatellitePV)
    const Eigen::Vector3d vel_interp(v_inertial.x() + OMEGA_E * pos_interp.y(),
                                      v_inertial.y() - OMEGA_E * pos_interp.x(),
                                      v_inertial.z());

    // --- Truth ------------------------------------------------------------
    const Eigen::Vector3d pos_truth = pos_ecef(t_q);
    const Eigen::Vector3d vel_truth = vel_ecef(t_q);

    // (a) Position recovery: sub-mm for a smooth degree-10 fit at this
    //     window size.  Generous bound (1 mm) to absorb any floating-point
    //     skew from x87 / fma differences across builds.
    const double err_pos = (pos_interp - pos_truth).norm();
    EXPECT_LT(err_pos, 1e-3) << "pre-rotated pos err: " << err_pos << " m";

    // (b) ECEF velocity recovery: v_ecef = v_inertial − ω×pos must match
    //     truth to mm/s.  This pins both the rotation arithmetic and the
    //     downstream conversion formula.
    const double err_vel = (vel_interp - vel_truth).norm();
    EXPECT_LT(err_vel, 1e-3) << "pre-rotated vel err: " << err_vel << " m/s";
}

// ============================================================================
// Solid Earth tide (IERS 2010) — official dehanttideinel test vector
// ============================================================================
//
// GnssCorrections::computeSolidEarthTideDisplacement is a faithful port of the
// IERS Conventions dehanttideinel routine (Step 1 in-phase degree 2/3, Step 1
// out-of-phase diurnal/semidiurnal + L1, Step 2 diurnal Table 7.3a and long-
// period Table 7.3b).  It is pinned here against the OFFICIAL published test
// vector from the IERS dehanttideinel.f header — a genuine external oracle,
// not a re-derivation of our own tables.  (The prior test parsed the Step-2
// table out of the C++ source and so was structurally blind to table-value
// errors; a transcription bug that zeroed K1 and inflated P1 — a ~1 cm diurnal
// radial error — slipped past it.  This vector test catches that class of bug:
// the displacement is sensitive to every band, including the sub-mm Step-1
// out-of-phase terms, at the 1e-4 m tolerance below.)
//
// Inputs and expected output are verbatim from the IERS reference:
//   XSTA = (4075578.385, 931852.890, 4801570.154) m            (φ≈49°)
//   XSUN = (137859926952.015, 54228127881.4350, 23509422341.6896) m
//   XMON = (-179996231.920342, -312468450.131567, -169288918.592160) m
//   2009-04-13 00:00 UT  →  T = (MJD 54934 − 51544.5)/36525,  FHR = 0
//   DXTIDE = (0.07700420357, 0.06304056322, 0.05516568153) m
TEST(SolidEarthTideTest, MatchesIersOfficialVector) {
    const Eigen::Vector3d xsta(4075578.385, 931852.890, 4801570.154);
    const Eigen::Vector3d xsun(137859926952.015, 54228127881.4350, 23509422341.6896);
    const Eigen::Vector3d xmon(-179996231.920342, -312468450.131567, -169288918.592160);
    const double jc  = (54934.0 - 51544.5) / 36525.0;  // 2009-04-13 0h UT
    const double fhr = 0.0;

    const Eigen::Vector3d dx =
        GnssCorrections::computeSolidEarthTideDisplacement(xsta, xsun, xmon, jc, fhr);

    const Eigen::Vector3d expected(0.07700420357108126,
                                   0.06304056321824968,
                                   0.05516568152597247);
    // The port reproduces the official vector to ~6e-5 m — within
    // dehanttideinel's own ~0.1 mm accuracy and ~200x below the ~12 mm
    // corruption it replaces. The residual is a constant/rounding-lineage
    // difference vs the reference Fortran, not a band omission (each band,
    // incl. the sub-mm Step-1 out-of-phase terms, is exercised here).
    EXPECT_NEAR(dx.x(), expected.x(), 1e-4) << "dx, got " << dx.x();
    EXPECT_NEAR(dx.y(), expected.y(), 1e-4) << "dy, got " << dx.y();
    EXPECT_NEAR(dx.z(), expected.z(), 1e-4) << "dz, got " << dx.z();
}

// ============================================================================
// Niell Mapping Function (default "GMF"/NMF tropo path) — convention lock
// ============================================================================
//
// Troposphere::getTroposphereMapping in its default (non-VMF3) mode is the
// Niell Mapping Function.  It is pinned against an independent Python port
// (scripts/nmf_reference.py) that mirrors cssrlib tropmapfNiell — itself a
// port of RTKLIB tropmapf, and matching POINT's NMF table.
//
// This locks the two May-2026 fixes:
//   1. the hydrostatic seasonal amplitude arrays carry a leading 0.0 at 15°
//      (the prior code dropped it, shifting every amplitude one bin), and
//   2. the seasonal combination is avg − amp·cos (the prior code used +).
// Either regression shifts the low-elevation hydrostatic mapping by ~1e-2,
// far above this test's 1e-6 tolerance.
TEST(NiellMappingTest, MatchesCanonicalReference) {
    LogManager log_mgr;          // bare — see SolidEarthTideTest note
    GnssConfig config;           // default → Gmf/NMF mapping, no GPT3 grid
    Troposphere tropo(config, log_mgr);

    // doy 129.25 → 2024-05-08 06:00 GPS ; doy 210.0 → 2024-07-28 00:00 GPS.
    const GnssTime t_129{ 2313, 280800.0 };
    const GnssTime t_210{ 2325, 0.0 };

    struct Case {
        const char* label;
        double lat_deg, lon_deg, h_m, el_deg, mfd, mfw;
        const GnssTime* time;
    };
    const std::vector<Case> cases = {
        { "mid45_doy129_el10",   45.0, -75.0, 200.0, 10.0, 5.5517428493167635, 5.657127344716983,  &t_129 },
        { "high70_doy129_el15",  70.0,  10.0, 100.0, 15.0, 3.8018203160938526, 3.8318732264095114, &t_129 },
        { "mid45_doy210_el5",    45.0, -75.0,   0.0,  5.0, 10.105663915673498, 10.750884210392691, &t_210 },
        { "south40_doy210_el20",-40.0, 150.0,  50.0, 20.0, 2.897534032127845,  2.9112923615249278, &t_210 },
        { "trop15_doy210_el8",   15.0, 100.0,   0.0,  8.0, 6.7889522601127394, 6.9911693990787045, &t_210 },
    };

    constexpr double DEG2RAD = Constants::PI / 180.0;
    for (const auto& c : cases) {
        const Eigen::Vector3d rx =
            Coordinates::llhToEcef(c.lat_deg * DEG2RAD, c.lon_deg * DEG2RAD, c.h_m);
        double mf_d = 0.0, mf_w = 0.0;
        tropo.getTroposphereMapping(c.el_deg, rx, *c.time, mf_d, mf_w);

        EXPECT_NEAR(mf_d, c.mfd, 1e-6) << "case " << c.label << " (mf_d)";
        EXPECT_NEAR(mf_w, c.mfw, 1e-6) << "case " << c.label << " (mf_w)";
    }
}

// ============================================================================
// Antenna PCV interpolation — bilinear, NOAZI, and the last-ring edge
// ============================================================================
//
// Antenna::interpolatePcv is a pure ANTEX PCV grid interpolator.  These cases
// pin the bilinear math, the NOAZI (azimuth-independent) path, the azimuth
// wrap, and — the May-2026 fix — the last-zenith-ring boundary, which
// previously dereferenced pcv_table[zen_idx + 1] out of bounds (the bounds
// check was placed AFTER the access).  A query at zenith == the table's
// largest ring must hold the value flat, not read past the table.
//
// Synthetic 4×5 grid (zenith rings 0/30/60/90, azimuth cols 0/90/180/270/360):
// value = 10·row + col, so every corner is identifiable by inspection.
TEST(AntennaPcvTest, BilinearNoaziAndLastRingClamp) {
    const std::vector<std::vector<double>> grid = {
        {  0,  1,  2,  3,  4 },   // zenith 0°
        { 10, 11, 12, 13, 14 },   // zenith 30°
        { 20, 21, 22, 23, 24 },   // zenith 60°
        { 30, 31, 32, 33, 34 },   // zenith 90°
    };
    constexpr double dzen = 30.0, dazi = 90.0;

    // (a) Interior bilinear at zen=15, az=45 (frac 0.5/0.5 in both):
    //     mean of corners 0,1,10,11 = 5.5.
    EXPECT_NEAR(Antenna::interpolatePcv(grid, 15.0, 45.0, dazi, dzen), 5.5, 1e-12);

    // (b) Last-ring edge (the fix): zen=90 sits exactly on the last ring.
    //     zen_frac=0, so the result is the last-ring value at az=45:
    //     (30 + 31)/2 = 30.5.  Pre-fix this read pcv_table[4] out of bounds.
    EXPECT_NEAR(Antenna::interpolatePcv(grid, 90.0, 45.0, dazi, dzen), 30.5, 1e-12);

    // (c) Beyond the last ring (zen=120) clamps to the last ring — same 30.5,
    //     and again no out-of-bounds access.
    EXPECT_NEAR(Antenna::interpolatePcv(grid, 120.0, 45.0, dazi, dzen), 30.5, 1e-12);

    // (d) Azimuth wrap: az=360 → az_idx=4, az_next=(4+1)%5=0, az_frac=0 →
    //     returns column 4 of row 0 = 4.0 (and exercises the %cols wrap path
    //     without going out of bounds).
    EXPECT_NEAR(Antenna::interpolatePcv(grid, 0.0, 360.0, dazi, dzen), 4.0, 1e-12);

    // (e) NOAZI (single column) path with its own last-ring clamp.
    const std::vector<std::vector<double>> noazi = { {0}, {10}, {20}, {30} };
    EXPECT_NEAR(Antenna::interpolatePcv(noazi, 15.0, 0.0, dazi, dzen),  5.0, 1e-12); // mid 0→30
    EXPECT_NEAR(Antenna::interpolatePcv(noazi, 90.0, 0.0, dazi, dzen), 30.0, 1e-12); // last ring
    EXPECT_NEAR(Antenna::interpolatePcv(noazi, 120.0, 0.0, dazi, dzen), 30.0, 1e-12); // clamp
}

// Regression: std::from_chars' float overload accepts '-' but rejects a leading
// '+'.  Some IGS calibration labs (Geo++ robot files — e.g. the LEIAR25 NONE /
// LEIT type-means in igs20.atx) prefix EVERY PCV value with '+', so the parser's
// from_chars-based PCV hot-loop stopped at the first value and stored an EMPTY
// PCV grid.  That silently zeroed receiver PCV and, under require_atx_coverage,
// dropped all of that antenna's signals (e.g. "Skipping C2W … no strict ATX
// coverage").  This ATX uses '+'-signed NOAZI + azimuth PCV; the grid must parse.
TEST(AntennaPcvTest, AtxPlusSignedPcvGridParses) {
    auto col60 = [](std::string body, const std::string& tag) {
        if (body.size() < 60) body.resize(60, ' ');
        return body + tag + "\n";
    };
    std::string atx;
    atx += col60("",                               "START OF ANTENNA");
    atx += col60("TESTANT         NONE",            "TYPE / SERIAL NO");
    atx += col60("    90.0",                        "DAZI");
    atx += col60("     0.0  10.0   5.0",            "ZEN1 / ZEN2 / DZEN");
    atx += col60("     1",                          "# OF FREQUENCIES");
    atx += col60("   G01",                          "START OF FREQUENCY");
    atx += col60("     +1.41     +1.00   +155.34",  "NORTH / EAST / UP");
    atx += "   NOAZI   +0.00   +0.50   +1.00\n";
    atx += "     0.0   +0.00   +0.50   +1.00\n";   // azimuth rows (DAZI 90°): 0,90,180,270,360
    atx += "    90.0   +0.10   +0.60   +1.10\n";
    atx += "   180.0   +0.20   +0.70   +1.20\n";
    atx += "   270.0   +0.30   +0.80   +1.30\n";
    atx += "   360.0   +0.00   +0.50   +1.00\n";
    atx += col60("   G01",                          "END OF FREQUENCY");
    atx += col60("",                                "END OF ANTENNA");

    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "gnss_test_plus_signed.atx";
    { std::ofstream o(tmp); o << atx; }

    GnssConfig    config;
    LogManager    lm;
    ProductParser pp(config, lm);
    std::vector<AntennaData> ants;
    ASSERT_TRUE(pp.parseAtx(tmp, ants));
    fs::remove(tmp);

    const AntennaData* e = nullptr;
    for (const auto& a : ants) if (a.type == "TESTANT_NONE") { e = &a; break; }
    ASSERT_NE(e, nullptr) << "receiver entry not parsed";

    // PCO (+-signed) parses via parseDouble — always worked.
    ASSERT_EQ(e->pco.count("G01"), 1u);
    EXPECT_NEAR(e->pco.at("G01").z(), 155.34, 1e-9);

    // PCV grid must be NON-EMPTY (the bug) — 3 zenith (0,5,10) × 5 azimuth.
    ASSERT_EQ(e->pcv.count("G01"), 1u);
    const auto& grid = e->pcv.at("G01");
    ASSERT_FALSE(grid.empty()) << "PCV grid empty — '+'-signed values failed to parse";
    ASSERT_EQ(grid.size(), 3u);
    ASSERT_EQ(grid.front().size(), 5u);
    // grid[zen][az]: zenith 10° (idx 2), azimuth 90° (idx 1) row value = +1.10.
    EXPECT_NEAR(grid[2][1], 1.10, 1e-9);
}

// ============================================================================
// Higher-order ionosphere (I₂ + I₃) — coefficient + convention lock
// ============================================================================
//
// HigherOrderIonosphere::slantHigherOrderComponents is pinned against an
// independent Python oracle (scripts/higher_order_iono_reference.py) that
// derives the coefficients from first principles and the POINT reference
// (higher_order_iono/Class_Iono.cpp).  The dipole B-field constants are shared
// with the engine (MagneticField, audited clean); the I₂/I₃ coefficients are
// derived independently, so this locks the two May-2026 fixes:
//
//   1. I₂ (group/code) coefficient is 7527·c, NOT 7527·c/2.  The /2 form is
//      the *phase* coefficient — using it for code made both code and phase
//      I₂ a factor of two too small.  This case's I₂ would halve on regression
//      (~4e-3 m at L1), thousands× this 1e-9 m tolerance.
//   2. I₃ (group/code) scales with ∫N_e²dl = η·N_max·STEC, NOT η·STEC².  The
//      old STEC² form was ~2.3e5× too large (hundreds of metres, not ~mm).
//
// Internal consistency the goldens also encode: phase = −½·I₂code and
// −⅓·I₃code; L2/L1 ratios are (f₁/f₂)³ for I₂ and (f₁/f₂)⁴ for I₃.
TEST(HigherOrderIonosphereTest, MatchesCanonicalReference) {
    const HigherOrderIonosphere hoi;  // stateless

    // Same fixed geometry as the Python oracle (explicit ECEF literals so the
    // test does not depend on the WGS84 conversion matching Python's).
    const Eigen::Vector3d rx(4.6e6, 0.8e6, 4.3e6);
    const Eigen::Vector3d sat(1.5e7, 1.0e7, 2.0e7);
    const double ipp_lat = 0.70;                       // rad
    const double ipp_lon = 0.18;                       // rad
    const double ipp_radius = (6371.0 + 450.0) * 1.0e3;  // m
    const double f_l1 = Constants::GPS_L1_FREQ;
    const double f_l2 = Constants::GPS_L2_FREQ;

    struct Case { const char* band; char meas; double freq; double stec; double i2; double i3; };
    const std::vector<Case> cases = {
        { "L1", 'C', f_l1,  50.0, -0.007714137670442714,  0.000288285519644309   },
        { "L1", 'L', f_l1,  50.0,  0.003857068835221357, -9.609517321476967e-05  },
        { "L2", 'C', f_l2,  50.0, -0.016304437097686218,  0.0007819531397965047  },
        { "L1", 'C', f_l1, 120.0, -0.018513930409062514,  0.00166052459315122    },
    };

    for (const auto& c : cases) {
        const auto comp = hoi.slantHigherOrderComponents(
            rx, sat, ipp_lat, ipp_lon, ipp_radius, c.stec, c.freq, c.meas);
        ASSERT_TRUE(comp.valid) << "case " << c.band << "/" << c.meas;
        EXPECT_NEAR(comp.i2_m, c.i2, 1e-9) << "case " << c.band << "/" << c.meas << " (I2)";
        EXPECT_NEAR(comp.i3_m, c.i3, 1e-9) << "case " << c.band << "/" << c.meas << " (I3)";
    }
}

}  // namespace
}  // namespace GnssProcessor

// main() is provided by GTest::Main (linked via CMake target_link_libraries).
// Multiple translation units in the gnss_tests target each defining main()
// would conflict at link time, so the entry point lives in the gtest_main lib.
