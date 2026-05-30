/**
 * @file test_geometry.cpp
 * @brief Unit tests for pure positioning geometry: Model::Geometry geometric
 *        range / topocentric and WlsEstimator::predictObs.
 *
 * `computeGeometricRange` and `computeTopocentric` are pure functions of their
 * ECEF arguments (no product/orbit state), so they are now static and tested
 * directly.  `WlsEstimator::predictObs` is a private static reached through the
 * befriended `WlsEstimatorTestAccess` shim (same pattern as the MLAMBDA suite).
 *
 * Expected values are analytic.  A receiver on the equator at the prime
 * meridian sits at ECEF (a, 0, 0), where the local ENU axes are East = +Y,
 * North = +Z, Up = +X — so satellites placed along those ECEF axes produce
 * exact elevation/azimuth.
 */

#include <gtest/gtest.h>

#include "model/ModelGeometry.hpp"
#include "math/WlsEstimator.hpp"

#include <Eigen/Dense>

namespace GnssProcessor {

// Befriended in WlsEstimator.hpp: builds the private WlsObs and forwards to the
// private static predictObs.  Only the fields predictObs reads are set.
struct WlsEstimatorTestAccess {
    static double predictObs(double modeled, double initial_range, double mf_w,
                             const Eigen::Vector3d& sat_pos,
                             const Eigen::Vector3d& pos, double clock, double trop_z) {
        WlsEstimator::WlsObs obs;
        obs.modeled = modeled;
        obs.initial_range = initial_range;
        obs.mf_w = mf_w;
        obs.sat_pos = sat_pos;
        return WlsEstimator::predictObs(obs, pos, clock, trop_z);
    }
};

namespace {

using Model::Geometry;
using Model::Topocentric;

// Receiver on the equator at the prime meridian: ECEF (a, 0, 0).
// Local ENU axes in ECEF there: Up = +X, East = +Y, North = +Z.
const Eigen::Vector3d kRxEquator(6378137.0, 0.0, 0.0);

// ---------------------------------------------------------------------------
// computeGeometricRange
// ---------------------------------------------------------------------------

TEST(ModelGeometryTest, GeometricRangeIsEuclideanDistance) {
    EXPECT_DOUBLE_EQ(Geometry::computeGeometricRange(Eigen::Vector3d(0, 0, 0),
                                                     Eigen::Vector3d(3, 4, 0)), 5.0);
    EXPECT_DOUBLE_EQ(Geometry::computeGeometricRange(Eigen::Vector3d(6378137, 0, 0),
                                                     Eigen::Vector3d(6378137, 100, 0)), 100.0);
    EXPECT_DOUBLE_EQ(Geometry::computeGeometricRange(kRxEquator, kRxEquator), 0.0);  // coincident
}

// ---------------------------------------------------------------------------
// computeTopocentric
// ---------------------------------------------------------------------------

TEST(ModelGeometryTest, TopocentricOverheadIsNinetyDegrees) {
    // Straight up (+X = Up at this site).
    Topocentric t = Geometry::computeTopocentric(kRxEquator, kRxEquator + Eigen::Vector3d(1e7, 0, 0));
    EXPECT_NEAR(t.elevation_deg, 90.0, 1e-4);
}

TEST(ModelGeometryTest, TopocentricCardinalAzimuths) {
    // East (+Y): elevation 0, azimuth 90.
    Topocentric e = Geometry::computeTopocentric(kRxEquator, kRxEquator + Eigen::Vector3d(0, 1e6, 0));
    EXPECT_NEAR(e.elevation_deg, 0.0, 1e-4);
    EXPECT_NEAR(e.azimuth_deg, 90.0, 1e-4);
    // North (+Z): azimuth 0.
    Topocentric n = Geometry::computeTopocentric(kRxEquator, kRxEquator + Eigen::Vector3d(0, 0, 1e6));
    EXPECT_NEAR(n.azimuth_deg, 0.0, 1e-4);
    // West (-Y): azimuth 270.
    Topocentric w = Geometry::computeTopocentric(kRxEquator, kRxEquator + Eigen::Vector3d(0, -1e6, 0));
    EXPECT_NEAR(w.azimuth_deg, 270.0, 1e-4);
    // South (-Z): azimuth 180.
    Topocentric s = Geometry::computeTopocentric(kRxEquator, kRxEquator + Eigen::Vector3d(0, 0, -1e6));
    EXPECT_NEAR(s.azimuth_deg, 180.0, 1e-4);
}

TEST(ModelGeometryTest, TopocentricFortyFiveDegreesEast) {
    // Equal Up (+X) and East (+Y) components -> 45 deg elevation toward east.
    Topocentric t = Geometry::computeTopocentric(kRxEquator, kRxEquator + Eigen::Vector3d(5e6, 5e6, 0));
    EXPECT_NEAR(t.elevation_deg, 45.0, 1e-4);
    EXPECT_NEAR(t.azimuth_deg, 90.0, 1e-4);
}

TEST(ModelGeometryTest, TopocentricClampsBelowHorizonAndIsDegenerateSafe) {
    // Below the horizon (-X = straight down) -> elevation clamped to 0, azimuth in range.
    Topocentric below = Geometry::computeTopocentric(kRxEquator, kRxEquator + Eigen::Vector3d(-1e7, 0, 0));
    EXPECT_DOUBLE_EQ(below.elevation_deg, 0.0);
    EXPECT_GE(below.azimuth_deg, 0.0);
    EXPECT_LT(below.azimuth_deg, 360.0);

    // Degenerate (coincident) input -> {0, 0}, no crash.
    Topocentric deg = Geometry::computeTopocentric(kRxEquator, kRxEquator);
    EXPECT_DOUBLE_EQ(deg.elevation_deg, 0.0);
    EXPECT_DOUBLE_EQ(deg.azimuth_deg, 0.0);
}

// ---------------------------------------------------------------------------
// WlsEstimator::predictObs
//   predicted = (modeled + (||sat - pos|| - initial_range)) + clock + mf_w*trop_z
// ---------------------------------------------------------------------------

TEST(WlsEstimatorTest, PredictObsZeroDeltaAddsClockAndTropo) {
    // Current range == initial_range (100) -> no range delta.
    const double p = WlsEstimatorTestAccess::predictObs(
        /*modeled*/ 1000.0, /*initial_range*/ 100.0, /*mf_w*/ 2.0,
        /*sat_pos*/ Eigen::Vector3d(0, 0, 100), /*pos*/ Eigen::Vector3d(0, 0, 0),
        /*clock*/ 5.0, /*trop_z*/ 0.5);
    EXPECT_DOUBLE_EQ(p, 1006.0);  // 1000 + 0 + 5 + 2*0.5
}

TEST(WlsEstimatorTest, PredictObsTracksRangeDelta) {
    // Current range 100, initial 90 -> +10.
    EXPECT_DOUBLE_EQ(WlsEstimatorTestAccess::predictObs(
        1000.0, 90.0, 0.0, Eigen::Vector3d(0, 0, 100), Eigen::Vector3d(0, 0, 0), 0.0, 0.0), 1010.0);
    // Current range 100, initial 130 -> -30.
    EXPECT_DOUBLE_EQ(WlsEstimatorTestAccess::predictObs(
        1000.0, 130.0, 0.0, Eigen::Vector3d(0, 0, 100), Eigen::Vector3d(0, 0, 0), 0.0, 0.0), 970.0);
}

TEST(WlsEstimatorTest, PredictObsTropoMappingAndCombined) {
    // Pure wet-tropo term: mf_w * trop_z.
    EXPECT_DOUBLE_EQ(WlsEstimatorTestAccess::predictObs(
        0.0, 0.0, 4.0, Eigen::Vector3d(0, 0, 0), Eigen::Vector3d(0, 0, 0), 0.0, 0.25), 1.0);
    // All terms; offset receiver: ||(1,2,103) - (1,2,3)|| = 100 = initial -> delta 0.
    EXPECT_DOUBLE_EQ(WlsEstimatorTestAccess::predictObs(
        2.0e7, 100.0, 1.0, Eigen::Vector3d(1, 2, 103), Eigen::Vector3d(1, 2, 3), 1234.0, 8.0),
        20001242.0);  // 2e7 + 0 + 1234 + 1*8
}

}  // namespace
}  // namespace GnssProcessor
