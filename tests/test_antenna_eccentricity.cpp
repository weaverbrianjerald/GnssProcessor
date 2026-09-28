/**
 * @file test_antenna_eccentricity.cpp
 * @brief Unit tests for the marker <-> ARP antenna-eccentricity reduction
 *        (`Coordinates::markerToArp` / `arpToMarker`).
 *
 * Published station coordinates (IGS SINEX/SSC, NGS SIF, a surveyed RINEX
 * `APPROX POSITION XYZ`) refer to the monument; every position state in the
 * engine is the antenna reference point, because `igs20.atx` PCO/PCV are
 * ARP-relative.  The RINEX `ANTENNA: DELTA H/E/N` eccentricity connects the two
 * frames.  An error in its component order or sign produces a constant vertical
 * bias rather than a visible failure.  184 of the 305 stations in the bundled
 * 2024-129 fixture set carry a nonzero eccentricity (JPLM 0.0610 m, AGGO
 * 0.4100 m, NVSK 6.1661 m).  The tests cover:
 *
 *   1. RINEX (H, E, N) maps to (Up, East, North): a pure-H eccentricity moves
 *      geodetic height only, and by H;
 *   2. the E and N components land on east and north, not swapped;
 *   3. the sign is marker -> ARP (up, away from the monument);
 *   4. a zero eccentricity is a bitwise no-op, which keeps 0/0/0 stations and
 *      header-less live streams byte-identical;
 *   5. `markerToArp` and `arpToMarker` round-trip;
 *   6. a non-finite input is passed through rather than propagating NaN.
 */

#include <gtest/gtest.h>

#include "math/Coordinates.hpp"

#include <Eigen/Dense>
#include <cmath>

using namespace GnssProcessor;

namespace {

    /// JPLM00USA (2024-129 fixture) marker ECEF and its header eccentricity.
    const Eigen::Vector3d kJplmMarker(-2493304.1040, -4655215.4650, 3565497.5250);
    constexpr double kJplmUp = 0.0610;   ///< JPLM `ANTENNA: DELTA H/E/N` H term (m).

} // namespace

// A pure-up eccentricity changes geodetic height by H and leaves lat/lon
// unchanged; this is why an unreduced comparison shows a pure vertical bias.
TEST(AntennaEccentricityTest, PureUpMovesHeightOnly) {
    const Eigen::Vector3d hen(kJplmUp, 0.0, 0.0);
    const Eigen::Vector3d arp = Coordinates::markerToArp(kJplmMarker, hen);

    const Coordinates::Llh m = Coordinates::ecefToLlh(kJplmMarker);
    const Coordinates::Llh a = Coordinates::ecefToLlh(arp);

    EXPECT_NEAR(a.h_m - m.h_m, kJplmUp, 1e-6);
    EXPECT_NEAR(a.lat_rad, m.lat_rad, 1e-12);
    EXPECT_NEAR(a.lon_rad, m.lon_rad, 1e-12);
    // ...and the 3D displacement is the eccentricity, nothing more.
    EXPECT_NEAR((arp - kJplmMarker).norm(), kJplmUp, 1e-9);
}

// H/E/N ordering: the second and third header fields are East and North, and
// they must not swap.  Checked by projecting back into the local frame.
TEST(AntennaEccentricityTest, EastNorthComponentsAreNotSwapped) {
    const double up = 0.05, east = 0.30, north = -0.70;
    const Eigen::Vector3d hen(up, east, north);
    const Eigen::Vector3d arp = Coordinates::markerToArp(kJplmMarker, hen);

    const Coordinates::Llh m_llh = Coordinates::ecefToLlh(kJplmMarker);
    const Eigen::Vector3d enu = Coordinates::ecefToEnu(arp - kJplmMarker, m_llh);

    EXPECT_NEAR(enu.x(), east,  1e-9);   // E
    EXPECT_NEAR(enu.y(), north, 1e-9);   // N
    EXPECT_NEAR(enu.z(), up,    1e-9);   // U
}

// Direction: marker -> ARP goes up (the antenna sits above the monument), so the
// ARP is farther from the geocentre for a positive H.
TEST(AntennaEccentricityTest, SignIsMarkerToArpNotReverse) {
    const Eigen::Vector3d hen(kJplmUp, 0.0, 0.0);
    EXPECT_GT(Coordinates::markerToArp(kJplmMarker, hen).norm(), kJplmMarker.norm());
    EXPECT_LT(Coordinates::arpToMarker(kJplmMarker, hen).norm(), kJplmMarker.norm());
}

// Byte-identity guard: a zero (or absent) eccentricity returns the coordinate
// bitwise unchanged, not merely close.  This lets the reduction sit on shared
// paths without changing a golden run whose stations are 0/0/0 (for example
// CIT1 and NRMG).
TEST(AntennaEccentricityTest, ZeroEccentricityIsExactNoOp) {
    const Eigen::Vector3d zero = Eigen::Vector3d::Zero();
    const Eigen::Vector3d arp    = Coordinates::markerToArp(kJplmMarker, zero);
    const Eigen::Vector3d marker = Coordinates::arpToMarker(kJplmMarker, zero);

    EXPECT_EQ(arp.x(), kJplmMarker.x());
    EXPECT_EQ(arp.y(), kJplmMarker.y());
    EXPECT_EQ(arp.z(), kJplmMarker.z());
    EXPECT_EQ(marker.x(), kJplmMarker.x());
    EXPECT_EQ(marker.y(), kJplmMarker.y());
    EXPECT_EQ(marker.z(), kJplmMarker.z());
}

// Round-trip, including a large eccentricity (NVSK's 6.17 m tower), where the
// second-order error from evaluating the local frame at the ARP in arpToMarker
// is largest.  A single-shot inverse closes to only about 4 um there, so the
// 1e-9 m bound also requires arpToMarker to keep its fixed-point step.
TEST(AntennaEccentricityTest, RoundTripsThroughBothDirections) {
    for (const Eigen::Vector3d& hen : { Eigen::Vector3d(0.0083, 0.0, 0.0),
                                        Eigen::Vector3d(0.4100, 0.0, 0.0),
                                        Eigen::Vector3d(6.1661, 2.0, -3.0) }) {
        const Eigen::Vector3d arp  = Coordinates::markerToArp(kJplmMarker, hen);
        const Eigen::Vector3d back = Coordinates::arpToMarker(arp, hen);
        EXPECT_LT((back - kJplmMarker).norm(), 1e-9)
            << "round-trip failed for hen = " << hen.transpose();
    }
}

// A non-finite coordinate (an unparsed / absent header field upstream) must pass
// through untouched instead of seeding NaN into a position state.
TEST(AntennaEccentricityTest, NonFiniteInputPassesThrough) {
    const Eigen::Vector3d bad(std::nan(""), 0.0, 0.0);
    const Eigen::Vector3d hen(kJplmUp, 0.0, 0.0);
    const Eigen::Vector3d out = Coordinates::markerToArp(bad, hen);
    EXPECT_TRUE(std::isnan(out.x()));
    EXPECT_EQ(out.y(), 0.0);
    EXPECT_EQ(out.z(), 0.0);
}
