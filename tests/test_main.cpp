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

#include "common/Constants.hpp"
#include "math/Coordinates.hpp"
#include "math/Interpolate.hpp"
#include "math/TimeUtils.hpp"

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
    EXPECT_FALSE(TimeUtils::isValidGregorianDate(1979, 12, 31, 0, 0, 0.0)); // Pre-GPS epoch
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
    EXPECT_NEAR((tt_jd - gps_jd) * 86400.0, TimeUtils::TT_GPS_OFFSET, 1e-9);
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

}  // namespace
}  // namespace GnssProcessor

// main() is provided by GTest::Main (linked via CMake target_link_libraries).
// Multiple translation units in the gnss_tests target each defining main()
// would conflict at link time, so the entry point lives in the gtest_main lib.
