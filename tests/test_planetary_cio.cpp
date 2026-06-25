/**
 * @file test_planetary_cio.cpp
 * @brief Unit tests for the CIO-based IAU 2000A/2006 celestial-to-terrestrial
 *        frame transform (Planetary::celestialToTerrestrial).
 *
 * The transform is built from SOFA primitives via the (X, Y, s) path
 * (iauXys00a + iauC2ixys + iauEra00 + iauSp00 + iauPom00 + iauC2tcio).  With the
 * observed celestial-pole offsets dX = dY = 0, this is algebraically identical
 * to SOFA's all-in-one iauC2t00a (whose C2I is iauC2i00a, i.e. the same
 * iauPnm00a → iauBpn2xy → iauS00 → iauC2ixys chain).  We therefore pin the
 * result against iauC2t00a as an external oracle to machine precision, then
 * check rotation validity and the qualitative effect of injecting dX/dY.
 */

#include <gtest/gtest.h>

#include "model/Planetary.hpp"
#include "common/Constants.hpp"

#include <Eigen/Dense>
#include <cmath>

// SOFA oracle (vendored; sofa.h has its own extern "C" guards).
#include "sofa.h"

namespace GnssProcessor {

namespace {
constexpr double kMjdOffset = 2400000.5;
constexpr double kAs2Rad    = Constants::PI / 180.0 / 3600.0;

// A representative epoch: 2024-05-08 (IGS_Pasadena fixture day, MJD ~60438),
// with TT and UT1 deliberately distinct so ERA and C2I exercise different times.
constexpr double kTtMjd  = 60438.0008006;   // TT  as MJD (≈ UTC + 69.184 s)
constexpr double kUt1Mjd = 60438.0000123;   // UT1 as MJD (UT1 ≠ TT)

// Plausible polar motion (arcsec → rad).
const double kXpRad = 0.165 * kAs2Rad;
const double kYpRad = 0.398 * kAs2Rad;
}  // namespace

// celestialToTerrestrial(dX=dY=0) must reproduce SOFA's iauC2t00a bit-for-bit.
TEST(PlanetaryCio, MatchesSofaC2t00a) {
    const double tt_jd  = kMjdOffset + kTtMjd;
    const double ut1_jd = kMjdOffset + kUt1Mjd;

    const Eigen::Matrix3d R =
        Planetary::celestialToTerrestrial(tt_jd, ut1_jd, kXpRad, kYpRad, 0.0, 0.0);

    // Feed the oracle the SAME two-part (MJD_OFFSET, JD - MJD_OFFSET) split the
    // function uses internally, so this isolates the algebraic equivalence of the
    // (X, Y, s) path (iauXys00a + iauC2ixys) against SOFA's bundled iauC2t00a
    // (whose C2I is iauC2i00a) — not the sub-1e-10 noise of round-tripping a
    // full Julian Date through a subtraction.
    double rc2t[3][3];
    iauC2t00a(kMjdOffset, tt_jd - kMjdOffset, kMjdOffset, ut1_jd - kMjdOffset,
              kXpRad, kYpRad, rc2t);

    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            EXPECT_NEAR(R(i, j), rc2t[i][j], 1e-12)
                << "mismatch at (" << i << "," << j << ")";
}

// The transform must be a proper rotation: orthonormal with determinant +1.
TEST(PlanetaryCio, IsProperRotation) {
    const Eigen::Matrix3d R = Planetary::celestialToTerrestrial(
        kMjdOffset + kTtMjd, kMjdOffset + kUt1Mjd, kXpRad, kYpRad, 0.0, 0.0);

    const Eigen::Matrix3d should_be_I = R * R.transpose();
    EXPECT_LT((should_be_I - Eigen::Matrix3d::Identity()).cwiseAbs().maxCoeff(), 1e-12);
    EXPECT_NEAR(R.determinant(), 1.0, 1e-12);
}

// Injecting small celestial-pole offsets dX/dY perturbs the matrix by an amount
// on the order of the offset magnitude (a CIP rotation), and the perturbation
// scales linearly with the offset.
TEST(PlanetaryCio, PoleOffsetInjectionScales) {
    const double tt_jd  = kMjdOffset + kTtMjd;
    const double ut1_jd = kMjdOffset + kUt1Mjd;

    const Eigen::Matrix3d R0 =
        Planetary::celestialToTerrestrial(tt_jd, ut1_jd, kXpRad, kYpRad, 0.0, 0.0);

    const double dX = 2.0e-9;  // radians (~0.4 mas, realistic IERS magnitude)
    const double dY = -1.0e-9;
    const Eigen::Matrix3d R1 =
        Planetary::celestialToTerrestrial(tt_jd, ut1_jd, kXpRad, kYpRad, dX, dY);
    const Eigen::Matrix3d R2 =
        Planetary::celestialToTerrestrial(tt_jd, ut1_jd, kXpRad, kYpRad, 2 * dX, 2 * dY);

    const double d1 = (R1 - R0).cwiseAbs().maxCoeff();
    const double d2 = (R2 - R0).cwiseAbs().maxCoeff();

    // Non-zero, of the right order, and ~doubles when the offset doubles.
    EXPECT_GT(d1, 1e-11);
    EXPECT_LT(d1, 1e-7);
    EXPECT_NEAR(d2 / d1, 2.0, 0.05);
}

}  // namespace GnssProcessor
