/**
 * @file test_phase_windup_sign.cpp
 * @brief Sign and effective-dipole gate for GnssCorrections::computePhaseWindUp.
 *
 * Under test
 * ----------
 * The two effective-dipole lines (Ds, satellite; Dr, receiver) in
 * computePhaseWindUp, src/core/corrections/GnssCorrections.cpp.  For a
 * right-handed antenna triad (x, y, z) and a unit LOS
 * k = (rx_pos - sat_pos).normalized(), the Wu et al. (1993) effective dipole is
 *
 *     D = x - k (k.x) - k x y                       <-- the model
 *
 * and both triads the function builds are right-handed with their third axis on
 * the +k side:
 *   - satellite: (ex, ey, ez), ez = -sat_pos.normalized() (geocentric nadir),
 *                ex x ey = ez;  eps_s = nadir angle  = angle(ez, k).
 *   - receiver:  x_r = north, y_r = east, whose implied third axis is
 *                north x east = down;  eps_r = zenith angle = angle(down, k).
 *
 * The magnitudes are exact, with no small-angle step (proved in
 * EffectiveDipoleMagnitudeIsOnePlusCosEps below):
 *
 *     | x - k(k.x) - k x y | = 1 + cos eps           <-- the model
 *     | x - k(k.x) + k x y | = 1 - cos eps           <-- what a "+" builds
 *
 * so a "+" sign does not perturb the dipole; it cancels the leading term, and the
 * effective dipole vanishes on boresight, where the antenna gain is maximal.
 * The two normalized directions are exact mirror images of each other about the
 * k x p axis, so the returned angle is the mirror of the modelled one.
 *
 * Design of the cases
 * -------------------
 * 1. The magnitudes are not directly observable through the public API.  The
 *    function normalizes both dipoles before taking the angle
 *    (Ds_hat = Ds / Ds_norm), so |Ds| and |Dr| cancel out of the return value,
 *    except where a magnitude collapses through the degenerate-dipole guard
 *
 *        if (Ds_norm < 1e-10 || Dr_norm < 1e-10) return 0.0;
 *
 *    That guard is an OR over the two magnitudes.  A geometry that drives
 *    exactly one eps to zero therefore makes exactly one of the two lines
 *    observable: the return either collapses to 0.0 (that line has the wrong
 *    sign) or stays at the modelled value (that line is right).  Two such
 *    geometries exist and they are independent:
 *       eps_s = 0  <=>  the receiver sits on the geocentric ray through the
 *                       satellite  -> SatelliteBoresightIsolatesSatelliteDipoleSign
 *       eps_r = 0  <=>  the satellite sits on the receiver's geodetic vertical
 *                       -> ReceiverZenithIsolatesReceiverDipoleSign
 *    They coincide only where the geodetic vertical is parallel to the
 *    geocentric radial (equator, poles), which is why the equatorial
 *    sub-satellite case pins both lines at once but distinguishes neither.
 *
 * 2. A half-fix must fail.  Flipping only one of the two lines is worse than
 *    flipping neither: it converts a clean sign inversion (which a constant
 *    ambiguity largely absorbs) into a phase error spread over a full cycle
 *    (median |error| ~ 0.29 cyc over 4000 random el >= 10 deg geometries).  A
 *    value assertion at a single geometry can pass a half-fix by chance (the
 *    measured minimum over those 4000 geometries was 8e-5 cyc), so the
 *    half-fix guard here is structural, not statistical: each of the two
 *    isolating cases pins one line's sign through that line's own dipole
 *    magnitude, and the two together admit only (-,-).  Measured behaviour of
 *    the four arms (satellite sign, receiver sign):
 *
 *      arm            | SatelliteBoresight     | ReceiverZenith
 *      ---------------+------------------------+------------------------
 *      (+,+) pre-fix  | 0.0        FAIL        | 0.0        FAIL
 *      (-,+) half-fix | +0.070646  FAIL        | 0.0        FAIL
 *      (+,-) half-fix | 0.0        FAIL        | -0.070633  FAIL
 *      (-,-) fixed    | -0.024501  pass        | -0.024514  pass
 *
 *    Both cases fail the wrong arms on value as well as on the guard, so the
 *    gate survives a future change to the 1e-10 threshold.  YawTurnIsMinusOne-
 *    Cycle is a third, independent handle on the satellite line alone: a
 *    receiver-only flip still yields +1 cyc/turn.
 *
 * 3. The oracle is independent of the expression under test.  Ref::windUp below
 *    builds the same two body triads (the triads are the model's input, not the
 *    defect) but forms the dipole from the algebraically equivalent closed form
 *
 *        D = (1 + k.z) x - (k + z)(k.x)
 *
 *    which contains no cross product with y and exhibits the (1 + cos eps)
 *    scale directly.  It takes the angle in an explicit 2-D basis of the plane
 *    perpendicular to k rather than through the atan2 triple-product form the
 *    production code uses, and it derives the receiver's third axis as
 *    x_r x y_r instead of assuming "down".
 *
 * 4. phi_(+,+) is not exactly -phi_model.  The two mirror axes differ by the
 *    deflection between the receiver's geodetic vertical and the geocentric
 *    radial (up to 0.19 deg), amplified by 1/sin(eps_r).  Measured over 20000
 *    geometries at el >= 10 deg, |phi_(+,+) + phi_model| mod 1 cyc has median
 *    5.6e-4 cyc, p90 1.4e-3 and max 5.4e-2 (largest near zenith).  A case must
 *    therefore assert against the reference itself, not "pre-fix == -reference".
 *
 * Every case constructs a fresh GnssCorrections.  prev_windup_ is a mutable
 * per-instance member written from the const computePhaseWindUp(), so any case
 * passing accumulate = true carries state across calls; sharing an instance
 * across cases would make the accumulated cases order-dependent.
 *
 * Measured four-arm behaviour of cases 1-8 (same build, only the two dipole
 * signs differing; every number quoted in the case comments below is from that
 * run):
 *
 *     (+,+) pre-fix   7 of 8 FAIL
 *     (-,+) half-fix  7 of 8 FAIL
 *     (+,-) half-fix  7 of 8 FAIL
 *     (-,-) fixed     8 of 8 PASS
 *
 * The one case that passes in all four arms is EffectiveDipoleMagnitudeIs-
 * OnePlusCosEps, which by construction tests the algebra rather than the
 * production function.  The two satellite-frame branch cases at the end of the
 * file (case 9) are not part of that four-arm measurement.
 *
 * @see docs/SPEC_phase_windup_sign.md
 * @see test_site_displacement.cpp   sibling GnssCorrections geometry coverage
 */

#include <gtest/gtest.h>

#include "common/Constants.hpp"
#include "common/GnssTypes.hpp"
#include "corrections/GnssCorrections.hpp"
#include "model/SatAttitude.hpp"
#include "logging/LogManager.hpp"
#include "math/Coordinates.hpp"
#include "math/TimeUtils.hpp"

#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <string>

namespace GnssProcessor {
namespace {

using Eigen::Vector3d;

constexpr double kLambda    = Constants::GPS_L1_WAVELENGTH;   // 0.190293672798 m
constexpr double kTwoPi     = 2.0 * Constants::PI;
constexpr double kAu        = 1.495978707e11;                 // m
constexpr double kGpsRadius = 26560000.0;                     // m, |sat_pos|
constexpr double kDeg       = Constants::PI / 180.0;

// Agreement between the production function and the independent oracle.  Both
// are doubles over the same geometry; the measured spread over a 4000-geometry
// sweep was 4.2e-17 m.  1 nm leaves ~7 orders of headroom below the smallest
// failure signal any case here carries (1.07e-2 m).
constexpr double kMetreTol = 1e-9;

// The degenerate-dipole guard in computePhaseWindUp (GnssCorrections.cpp).
constexpr double kDipoleGuard = 1e-10;

// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------

Vector3d geodeticUp(double lat_rad, double lon_rad) {
    return Vector3d(std::cos(lat_rad) * std::cos(lon_rad),
                    std::cos(lat_rad) * std::sin(lon_rad),
                    std::sin(lat_rad));
}

Vector3d geodeticNorth(double lat_rad, double lon_rad) {
    return Vector3d(-std::sin(lat_rad) * std::cos(lon_rad),
                    -std::sin(lat_rad) * std::sin(lon_rad),
                     std::cos(lat_rad));
}

Vector3d geodeticEast(double lon_rad) {
    return Vector3d(-std::sin(lon_rad), std::cos(lon_rad), 0.0);
}

// Satellite on the |sat_pos| = kGpsRadius sphere at a given topocentric
// elevation / azimuth from rx_pos.
Vector3d satelliteAt(const Vector3d& rx_pos, double lat_rad, double lon_rad,
                     double el_deg, double az_deg) {
    const double el = el_deg * kDeg;
    const double az = az_deg * kDeg;
    const Vector3d los = std::cos(el) * (std::cos(az) * geodeticNorth(lat_rad, lon_rad)
                                       + std::sin(az) * geodeticEast(lon_rad))
                       + std::sin(el) * geodeticUp(lat_rad, lon_rad);
    const double b = 2.0 * rx_pos.dot(los);
    const double c = rx_pos.squaredNorm() - kGpsRadius * kGpsRadius;
    const double range = 0.5 * (-b + std::sqrt(b * b - 4.0 * c));
    return rx_pos + range * los;
}

Vector3d sunAt(const Vector3d& sat_pos, double dx, double dy, double dz) {
    return sat_pos + kAu * Vector3d(dx, dy, dz).normalized();
}

// ---------------------------------------------------------------------------
// Ref: the independent oracle (see header note 3)
// ---------------------------------------------------------------------------
namespace Ref {

struct Result {
    double phi_m{ 0.0 };       ///< modelled wind-up, metres (principal value)
    double ds_norm{ 0.0 };     ///< |Ds| from the closed form
    double dr_norm{ 0.0 };     ///< |Dr| from the closed form
    double eps_s_rad{ 0.0 };   ///< nadir angle  = angle(ez, k)
    double eps_r_rad{ 0.0 };   ///< zenith angle = angle(x_r x y_r, k)
};

// D = (1 + k.z) x - (k + z)(k.x).  Algebraically identical to
// x - k(k.x) - k x y for any right-handed (x, y, z), but written without any
// cross product with y, so it cannot inherit the defect under test.
Vector3d dipole(const Vector3d& k, const Vector3d& x, const Vector3d& z) {
    return (1.0 + k.dot(z)) * x - (k + z) * k.dot(x);
}

double angleAboutK(const Vector3d& k, const Vector3d& ds, const Vector3d& dr) {
    // Explicit 2-D basis of the plane perpendicular to k, instead of the
    // atan2(k . (Ds x Dr), Ds . Dr) triple-product form of the production code.
    const Vector3d seed = (std::abs(k.x()) > 0.9) ? Vector3d::UnitY() : Vector3d::UnitX();
    const Vector3d p = (seed - k * k.dot(seed)).normalized();
    const Vector3d q = k.cross(p);
    double a = std::atan2(dr.dot(q), dr.dot(p)) - std::atan2(ds.dot(q), ds.dot(p));
    a -= kTwoPi * std::round(a / kTwoPi);      // principal value, as atan2 gives
    return a;
}

Result windUp(const Vector3d& rx_pos, const Vector3d& sat_pos,
              const Vector3d& sun_pos, double lambda = kLambda) {
    const Vector3d k = (rx_pos - sat_pos).normalized();

    // Satellite yaw-steering triad: the model's input, reproduced as-is.
    const Vector3d ez = -sat_pos.normalized();
    Vector3d ey = ez.cross((sun_pos - sat_pos).normalized());
    ey.normalize();
    Vector3d ex = ey.cross(ez);
    ex.normalize();

    // Receiver pair, also the model's input.  The third axis is derived from the
    // pair the production code uses, not assumed to be "down".
    const Coordinates::Llh llh = Coordinates::ecefToLlh(rx_pos);
    const Vector3d xr = geodeticNorth(llh.lat_rad, llh.lon_rad);
    const Vector3d yr = geodeticEast(llh.lon_rad);
    const Vector3d zr = xr.cross(yr);                       // north x east == down

    const Vector3d ds = dipole(k, ex, ez);
    const Vector3d dr = dipole(k, xr, zr);

    Result out;
    out.ds_norm   = ds.norm();
    out.dr_norm   = dr.norm();
    out.eps_s_rad = std::acos(std::clamp(k.dot(ez), -1.0, 1.0));
    out.eps_r_rad = std::acos(std::clamp(k.dot(zr), -1.0, 1.0));
    out.phi_m     = angleAboutK(k, ds, dr) * lambda / kTwoPi;
    return out;
}

}  // namespace Ref

// ---------------------------------------------------------------------------
// 1. The identity that names the defect.
//
// This case tests the two algebraic expressions, not the production function:
// what a "+" line builds against what the model asks for, pinned over a sweep of
// eps and of the antenna's orientation about its own third axis.  The cases that
// follow make the link to the production code.  It also shows the mechanism
// those cases use: at eps = 0 the "+" branch falls below the 1e-10 dipole guard
// while the model branch is 2.
// ---------------------------------------------------------------------------
TEST(PhaseWindUpSignTest, EffectiveDipoleMagnitudeIsOnePlusCosEps) {
    int checked = 0;
    for (int e = 0; e <= 170; e += 5) {              // eps, degrees
        const double eps = e * kDeg;
        for (int b = 0; b < 360; b += 15) {          // azimuth of k about z
            const double beta = b * kDeg;
            // A right-handed triad (x, y, z) and a k at angle eps from z: the
            // same relationship both real triads have with the LOS.
            const Vector3d z = Vector3d::UnitZ();
            const Vector3d x = Vector3d::UnitX();
            const Vector3d y = z.cross(x);           // x x y == z  (right-handed)
            const Vector3d k(std::sin(eps) * std::cos(beta),
                             std::sin(eps) * std::sin(beta),
                             std::cos(eps));

            const Vector3d perp  = x - k * k.dot(x);
            const Vector3d d_neg = perp - k.cross(y);   // the model
            const Vector3d d_pos = perp + k.cross(y);   // what a "+" dipole line builds

            EXPECT_NEAR(d_neg.norm(), 1.0 + std::cos(eps), 5e-14)
                << "eps=" << e << " beta=" << b;
            EXPECT_NEAR(d_pos.norm(), 1.0 - std::cos(eps), 5e-14)
                << "eps=" << e << " beta=" << b;
            EXPECT_NEAR(d_neg.dot(k), 0.0, 5e-15);      // both lie in the plane
            EXPECT_NEAR(d_pos.dot(k), 0.0, 5e-15);      // perpendicular to k
            ++checked;
        }
    }
    EXPECT_EQ(checked, 35 * 24);

    // Boresight: the model dipole is maximal (2); the "+" one vanishes and lands
    // below the production degenerate-dipole guard (GnssCorrections.cpp).
    {
        const Vector3d z = Vector3d::UnitZ();
        const Vector3d x = Vector3d::UnitX();
        const Vector3d y = z.cross(x);
        const Vector3d k = z;
        EXPECT_NEAR((x - k * k.dot(x) - k.cross(y)).norm(), 2.0, 1e-15);
        EXPECT_LT((x - k * k.dot(x) + k.cross(y)).norm(), kDipoleGuard);
    }
}

// ---------------------------------------------------------------------------
// 2. Equatorial sub-satellite point: eps_s == eps_r == 0 exactly, and the whole
// answer is available in closed form without the oracle.
//
// Receiver at (0 N, 0 E, 0 m); satellite 26560 km straight up the X axis, so
//   k = (-1, 0, 0) = ez = north x east   (both eps vanish exactly).
// With the sun placed in the satellite's transverse plane the yaw-steering +x
// axis is exactly the sun unit vector; put it 30 deg east of north, i.e.
// ex = (0, sin30, cos30).  With the model dipoles Ds = 2 ex and Dr = 2 north,
// the signed angle about k is exactly -30 deg, so
//
//       phi = -(pi/6) * lambda / (2 pi) = -lambda/12 = -0.015857806067 m.
//
// This case pins the joint sign but distinguishes neither line: with either
// line carrying "+", the corresponding dipole vanishes and the guard returns
// exactly 0.0.  It is the cheapest full-fix gate and deliberately does not
// discriminate between the lines; cases 3 and 4 do the isolation.
// ---------------------------------------------------------------------------
TEST(PhaseWindUpSignTest, SubSatelliteBoresightMatchesClosedForm) {
    LogManager log_mgr;
    GnssCorrections gc(log_mgr);

    const double beta = 30.0 * kDeg;
    const Vector3d rx_pos = Coordinates::llhToEcef(0.0, 0.0, 0.0);
    const Vector3d sat_pos(kGpsRadius, 0.0, 0.0);
    const Vector3d sun_pos = sat_pos + kAu * Vector3d(0.0, std::sin(beta), std::cos(beta));

    // Premise: this really is the doubly-degenerate geometry.
    const Ref::Result ref = Ref::windUp(rx_pos, sat_pos, sun_pos);
    ASSERT_NEAR(ref.eps_s_rad, 0.0, 1e-7);
    ASSERT_NEAR(ref.eps_r_rad, 0.0, 1e-7);
    ASSERT_NEAR(ref.ds_norm, 2.0, 1e-12);
    ASSERT_NEAR(ref.dr_norm, 2.0, 1e-12);

    const double phi = gc.computePhaseWindUp("G01", System::GPS, rx_pos, sat_pos,
                                             kLambda, GnssTime{ 2300, 100000.0 },
                                             sun_pos, Vector3d::Zero(), false);

    EXPECT_NEAR(phi, -kLambda / 12.0, 1e-12);
    EXPECT_NEAR(phi, ref.phi_m, kMetreTol);
    // Redundant with the checks above, but it states the failure mode directly:
    // a vanished dipole trips the guard and the function returns exactly 0.0.
    EXPECT_GT(std::abs(phi), 1e-3);
}

// ---------------------------------------------------------------------------
// 3. Half-fix guard, satellite line (the Ds line in GnssCorrections.cpp).
//
// Receiver at 45 N / 75 W on the ellipsoid; satellite placed on the geocentric
// ray through it, so k = -rx.normalized() = ez and eps_s = 0 exactly, while
// eps_r = 0.192 deg (the geodetic-vs-geocentric vertical deflection at 45 deg).
// Consequently
//     |Ds| with "+" = 5.6e-17  -> below the 1e-10 guard
//     |Dr| with "+" = 5.6e-06  -> 5.6e4x above it, so the receiver line is
//                                 inert here
// and the return is 0.0 if and only if the Ds line carries the wrong sign.
// (The assertion message below cites the Ds line by its pre-fix line number.)
// Measured arms: (+,+) 0.0 | (-,+) +0.070646 | (+,-) 0.0 | (-,-) -0.024501 m.
// The value assertion also catches the (-,+) arm, which the guard does not, so
// the case does not depend on the 1e-10 threshold surviving unchanged.
// ---------------------------------------------------------------------------
TEST(PhaseWindUpSignTest, SatelliteBoresightIsolatesSatelliteDipoleSign) {
    LogManager log_mgr;
    GnssCorrections gc(log_mgr);

    const Vector3d rx_pos  = Coordinates::llhToEcef(45.0 * kDeg, -75.0 * kDeg, 0.0);
    const Vector3d sat_pos = rx_pos.normalized() * kGpsRadius;
    const Vector3d sun_pos = sunAt(sat_pos, 0.6, 0.48, 0.64);

    const Ref::Result ref = Ref::windUp(rx_pos, sat_pos, sun_pos);

    // Premise of the isolation: a "+" makes exactly one dipole vanish.
    ASSERT_LT(1.0 - std::cos(ref.eps_s_rad), kDipoleGuard);            // satellite
    ASSERT_GT(1.0 - std::cos(ref.eps_r_rad), 100.0 * kDipoleGuard);    // receiver
    ASSERT_NEAR(ref.ds_norm, 1.0 + std::cos(ref.eps_s_rad), 1e-12);
    ASSERT_NEAR(ref.dr_norm, 1.0 + std::cos(ref.eps_r_rad), 1e-12);
    ASSERT_NEAR(ref.ds_norm, 2.0, 1e-12);

    const double phi = gc.computePhaseWindUp("G02", System::GPS, rx_pos, sat_pos,
                                             kLambda, GnssTime{ 2300, 100000.0 },
                                             sun_pos, Vector3d::Zero(), false);

    EXPECT_NEAR(phi, ref.phi_m, kMetreTol);
    EXPECT_GT(std::abs(phi), 1e-3)
        << "satellite effective dipole annihilated on boresight: :1944 sign";
}

// ---------------------------------------------------------------------------
// 4. Half-fix guard, receiver line (the Dr line in GnssCorrections.cpp).
//
// Same site, but the satellite is placed 20200 km straight up the receiver's
// geodetic vertical, so k = -up exactly and eps_r = 0, while the satellite's
// geocentric nadir misses the LOS by eps_s = 0.046 deg.  Consequently
//     |Dr| with "+" = 1.1e-16  -> below the 1e-10 guard
//     |Ds| with "+" = 3.2e-07  -> 3.2e3x above it, so the satellite line is
//                                 inert here
// and the return is 0.0 if and only if the Dr line carries the wrong sign.
// (The assertion message below cites the Dr line by its pre-fix line number.)
// Measured arms: (+,+) 0.0 | (-,+) 0.0 | (+,-) -0.070633 | (-,-) -0.024514 m.
//
// Cases 3 and 4 together are the half-fix gate: 3 admits only {(-,+), (-,-)},
// 4 admits only {(+,-), (-,-)}, and the intersection is {(-,-)}.  Neither can
// be satisfied by changing the other's line: the returned angle normalizes both
// dipoles, so each dipole's magnitude is observable only through its own
// collapse, and each geometry trips the OR guard on exactly one side.
// ---------------------------------------------------------------------------
TEST(PhaseWindUpSignTest, ReceiverZenithIsolatesReceiverDipoleSign) {
    LogManager log_mgr;
    GnssCorrections gc(log_mgr);

    const Vector3d rx_pos = Coordinates::llhToEcef(45.0 * kDeg, -75.0 * kDeg, 0.0);
    // Use the lat/lon the production code derives, so k = -up to the last bit
    // rather than to the accuracy of a re-derivation.
    const Coordinates::Llh llh = Coordinates::ecefToLlh(rx_pos);
    const Vector3d sat_pos = rx_pos + 20200000.0 * geodeticUp(llh.lat_rad, llh.lon_rad);
    const Vector3d sun_pos = sunAt(sat_pos, 0.6, 0.48, 0.64);

    const Ref::Result ref = Ref::windUp(rx_pos, sat_pos, sun_pos);

    ASSERT_LT(1.0 - std::cos(ref.eps_r_rad), kDipoleGuard);            // receiver
    ASSERT_GT(1.0 - std::cos(ref.eps_s_rad), 100.0 * kDipoleGuard);    // satellite
    ASSERT_NEAR(ref.ds_norm, 1.0 + std::cos(ref.eps_s_rad), 1e-12);
    ASSERT_NEAR(ref.dr_norm, 1.0 + std::cos(ref.eps_r_rad), 1e-12);
    ASSERT_NEAR(ref.dr_norm, 2.0, 1e-12);

    const double phi = gc.computePhaseWindUp("G03", System::GPS, rx_pos, sat_pos,
                                             kLambda, GnssTime{ 2300, 100000.0 },
                                             sun_pos, Vector3d::Zero(), false);

    EXPECT_NEAR(phi, ref.phi_m, kMetreTol);
    EXPECT_GT(std::abs(phi), 1e-3)
        << "receiver effective dipole annihilated at zenith: :1945 sign";
}

// ---------------------------------------------------------------------------
// 5. The sign of the returned metres at an ordinary, non-degenerate geometry.
//
// Site near Boulder (40 N, 105 W, 1650 m), satellite at el 45 / az 120 on the
// 26560 km sphere, sun off the transverse plane.  Nadir 9.749167 deg, zenith
// 45 deg exactly.  Independently computed reference:
//
//     |Ds| = 1.985558519856 = 1 + cos(9.749167 deg)
//     |Dr| = 1.707106781187 = 1 + cos(45 deg) = 1 + sqrt(1/2)
//     phi  = -0.005243974194 m  =  -0.027557270 cyc
//
// The four arms land at (+,+) +0.005489534 | (-,+) +0.026471638 |
// (+,-) -0.026226078 | (-,-) -0.005243974 m.  The pre-fix (+,+) arm is not the
// exact negative of the model (header note 4): +0.005490 vs +0.005244.  The
// smallest failure signal here is 1.07e-2 m, seven orders above kMetreTol.
// ---------------------------------------------------------------------------
TEST(PhaseWindUpSignTest, SignedMetresMatchIndependentReference) {
    LogManager log_mgr;
    GnssCorrections gc(log_mgr);

    const double lat = 40.0 * kDeg;
    const double lon = -105.0 * kDeg;
    const Vector3d rx_pos  = Coordinates::llhToEcef(lat, lon, 1650.0);
    const Vector3d sat_pos = satelliteAt(rx_pos, lat, lon, 45.0, 120.0);
    const Vector3d sun_pos = sunAt(sat_pos, 0.30, 0.50, 0.81);

    const Ref::Result ref = Ref::windUp(rx_pos, sat_pos, sun_pos);
    ASSERT_NEAR(ref.eps_r_rad, 45.0 * kDeg, 1e-9);
    ASSERT_NEAR(ref.ds_norm, 1.0 + std::cos(ref.eps_s_rad), 1e-12);
    ASSERT_NEAR(ref.dr_norm, 1.0 + std::sqrt(0.5), 1e-12);

    const double phi = gc.computePhaseWindUp("G04", System::GPS, rx_pos, sat_pos,
                                             kLambda, GnssTime{ 2300, 100000.0 },
                                             sun_pos, Vector3d::Zero(), false);

    EXPECT_NEAR(phi, ref.phi_m, kMetreTol);
    // Recorded literal, so the case still checks a fixed value if the oracle is
    // edited.  Loose enough to absorb an ellipsoid-iteration epsilon, ~1e7
    // tighter than the smallest wrong-arm displacement.
    EXPECT_NEAR(phi, -0.005243974194, 1e-9);
    EXPECT_LT(phi, 0.0) << "sign of the modelled wind-up at this geometry";
}

// ---------------------------------------------------------------------------
// 6. Magnitude + value over a sweep of eps.
//
// Twelve deterministic geometries spanning nadir 1.2-13.4 deg and zenith
// 5-75 deg.  For each row the oracle's own dipole magnitudes are pinned to
// (1 + cos eps), and the production value is then pinned to the oracle.  The
// magnitude claim can be carried across a sweep only through the oracle: the
// production function normalizes both dipoles before it takes the angle, so it
// reports a magnitude only through the collapse that cases 3 and 4 use.  Each
// row is required to be sign-sensitive (|phi| well away from both 0 and 1/2
// cycle); otherwise a row could pass a sign flip.
// ---------------------------------------------------------------------------
TEST(PhaseWindUpSignTest, MatchesIndependentReferenceOverGeometrySweep) {
    struct Row {
        double lat_deg, lon_deg, h_m, el_deg, az_deg;
        double sun_x, sun_y, sun_z;
        double expect_cyc;   // recorded from the oracle; asserted loosely
    };
    const Row rows[] = {
        {   0.0,    0.0,    0.0, 15.0,  30.0,  0.30,  0.50,  0.81, -0.106769 },
        {  40.0, -105.0, 1650.0, 30.0, 200.0, -0.70,  0.20,  0.68, +0.113258 },
        { -33.9,  151.2,   50.0, 45.0, 120.0,  0.10, -0.90,  0.42, -0.262732 },
        {  64.1,  -21.9,  100.0, 60.0, 300.0, -0.35, -0.55,  0.76, +0.145987 },
        {  85.0,   10.0,  200.0, 75.0,  75.0,  0.86, -0.10,  0.50, +0.435290 },
        { -77.8,  166.7,   20.0, 85.0, 250.0,  0.00,  0.99,  0.14, +0.209622 },
        {  19.0,  -99.1, 2240.0, 20.0, 160.0, -0.20,  0.30, -0.93, +0.468576 },
        {  51.5,    0.0,   45.0, 35.0,  10.0,  0.55,  0.55, -0.63, -0.364180 },
        {   0.0,    0.0,    0.0, 50.0, 340.0, -0.90,  0.30,  0.31, -0.032661 },
        {  40.0, -105.0, 1650.0, 70.0, 110.0,  0.25, -0.25,  0.94, -0.028052 },
        { -33.9,  151.2,   50.0, 25.0, 285.0,  0.62,  0.70, -0.35, +0.358764 },
        {  64.1,  -21.9,  100.0, 80.0,  45.0, -0.45,  0.80,  0.40, -0.096943 },
    };

    int idx = 0;
    for (const Row& r : rows) {
        // Fresh instance per row: nothing here accumulates, but the wind-up
        // state is per-instance and per-satellite, so a fresh pair keeps the
        // rows independent of each other and of their order.
        LogManager log_mgr;
        GnssCorrections gc(log_mgr);

        const double lat = r.lat_deg * kDeg;
        const double lon = r.lon_deg * kDeg;
        const Vector3d rx_pos  = Coordinates::llhToEcef(lat, lon, r.h_m);
        const Vector3d sat_pos = satelliteAt(rx_pos, lat, lon, r.el_deg, r.az_deg);
        const Vector3d sun_pos = sunAt(sat_pos, r.sun_x, r.sun_y, r.sun_z);

        SCOPED_TRACE("sweep row " + std::to_string(idx) +
                     " el=" + std::to_string(r.el_deg) +
                     " az=" + std::to_string(r.az_deg));

        const Ref::Result ref = Ref::windUp(rx_pos, sat_pos, sun_pos);

        // (a) the magnitude claim, at this row's two eps values
        EXPECT_NEAR(ref.ds_norm, 1.0 + std::cos(ref.eps_s_rad), 1e-12);
        EXPECT_NEAR(ref.dr_norm, 1.0 + std::cos(ref.eps_r_rad), 1e-12);
        EXPECT_NEAR(ref.eps_r_rad, (90.0 - r.el_deg) * kDeg, 1e-9);

        // the row must be able to see a sign flip at all
        const double ref_cyc = ref.phi_m / kLambda;
        ASSERT_GT(std::abs(ref_cyc), 0.02);
        ASSERT_LT(std::abs(ref_cyc), 0.48);
        EXPECT_NEAR(ref_cyc, r.expect_cyc, 1e-5);

        // (b) the production function
        const double phi = gc.computePhaseWindUp("G05", System::GPS, rx_pos, sat_pos,
                                                 kLambda, GnssTime{ 2300, 100000.0 },
                                                 sun_pos, Vector3d::Zero(), false);
        EXPECT_NEAR(phi, ref.phi_m, kMetreTol);
        ++idx;
    }
}

// ---------------------------------------------------------------------------
// 7. Yaw-turn response: -1.0000 cycle per satellite yaw turn.
//
// Geometry frozen (40 N / 105 W, el 45 / az 120); the sun is walked once around
// the satellite's ez (nadir) axis in 72 steps of 5 deg.  With the sun in the
// satellite's transverse plane the yaw-steering +x axis is the sun unit vector,
// so this is exactly one right-handed yaw turn about the body +Z.  The modelled
// dipole direction then advances by +theta in the plane perpendicular to k and
// the wind-up (the signed angle from Ds to Dr) retreats by the same amount:
// exactly -1 cycle per turn, uniformly -1/72 cyc per step.
//
// Requires accumulate = true.  The epoch spacing is set by the accumulator
// contract: 30 s is inside the continuity horizon (the max_gap_s argument,
// default 300 s, of computePhaseWindUp), and the 5 deg step keeps the per-epoch
// change at 1/72 cyc, far inside the +-1/2 cycle the unwrap can resolve.  The
// first call seeds the accumulator and returns the raw atan2 angle, so the turn
// is measured as (last - first).
//
// This case pins the satellite line alone: a receiver-only flip leaves the
// (static) receiver dipole out of the turn, and both the pre-fix (+,+) and the
// (+,-) arms return +1.0000 cyc.
// Measured: (+,+) +1 | (-,+) -1 | (+,-) +1 | (-,-) -1.
// ---------------------------------------------------------------------------
TEST(PhaseWindUpSignTest, YawTurnIsMinusOneCycle) {
    LogManager log_mgr;
    GnssCorrections gc(log_mgr);          // fresh: prev_windup_ must start empty

    const double lat = 40.0 * kDeg;
    const double lon = -105.0 * kDeg;
    const Vector3d rx_pos  = Coordinates::llhToEcef(lat, lon, 1650.0);
    const Vector3d sat_pos = satelliteAt(rx_pos, lat, lon, 45.0, 120.0);

    // Right-handed basis (a1, a2, ez) of the satellite's transverse plane.
    const Vector3d ez = -sat_pos.normalized();
    const Vector3d a1 = ez.cross(rx_pos).normalized();
    const Vector3d a2 = ez.cross(a1);
    ASSERT_NEAR((a1.cross(a2) - ez).norm(), 0.0, 1e-12);

    constexpr int    kSteps = 72;
    constexpr double kDt    = 30.0;       // s, inside the 300 s continuity gate

    double first = 0.0, prev = 0.0, last = 0.0, max_step_cyc = 0.0;
    for (int i = 0; i <= kSteps; ++i) {
        const double yaw = kTwoPi * static_cast<double>(i) / kSteps;
        const Vector3d sun_pos =
            sat_pos + kAu * (std::cos(yaw) * a1 + std::sin(yaw) * a2);
        const GnssTime t{ 2300, 100000.0 + kDt * static_cast<double>(i) };

        const double phi = gc.computePhaseWindUp("G06", System::GPS, rx_pos, sat_pos,
                                                 kLambda, t, sun_pos,
                                                 Vector3d::Zero(), /*accumulate=*/true);
        if (i == 0) {
            first = phi;
            // The seeding call must still be the modelled fractional value.
            const Ref::Result ref = Ref::windUp(rx_pos, sat_pos, sun_pos);
            EXPECT_NEAR(phi, ref.phi_m, kMetreTol);
        } else {
            max_step_cyc = std::max(max_step_cyc, std::abs(phi - prev) / kLambda);
        }
        prev = phi;
        last = phi;
    }

    // The unwrap premise: no step came close to the +-1/2 cycle aliasing limit.
    EXPECT_LT(max_step_cyc, 0.05);
    EXPECT_NEAR(max_step_cyc, 1.0 / kSteps, 1e-9);

    EXPECT_NEAR((last - first) / kLambda, -1.0, 1e-6)
        << "one right-handed satellite yaw turn about ez must retard the "
           "wind-up by exactly one cycle";
}

// ---------------------------------------------------------------------------
// 8. Premise guard for case 7: the accumulator contract itself.
//
// Case 7 measures a yaw turn only if 30 s steps unwrap and the accumulator does
// not restart.  Walk 40 of the same 5 deg steps (the accumulated value reaches
// -0.638 cyc, past the fractional half-cycle), then repeat the last epoch's
// geometry on two different satellites: once 30 s later (must continue,
// -0.638 cyc) and once 301 s later (must restart from the raw atan2, +0.362 cyc,
// inside +-lambda/2).  If the default max_gap_s of computePhaseWindUp moves
// below 30 s, or the unwrap is removed, this fails before case 7 can report a
// misleading turn count.
// ---------------------------------------------------------------------------
TEST(PhaseWindUpSignTest, AccumulatorContinuityPremiseHolds) {
    LogManager log_mgr;
    GnssCorrections gc(log_mgr);

    const double lat = 40.0 * kDeg;
    const double lon = -105.0 * kDeg;
    const Vector3d rx_pos  = Coordinates::llhToEcef(lat, lon, 1650.0);
    const Vector3d sat_pos = satelliteAt(rx_pos, lat, lon, 45.0, 120.0);
    const Vector3d ez = -sat_pos.normalized();
    const Vector3d a1 = ez.cross(rx_pos).normalized();
    const Vector3d a2 = ez.cross(a1);

    auto sun_at_step = [&](int i) -> Vector3d {
        const double yaw = kTwoPi * static_cast<double>(i) / 72.0;
        return sat_pos + kAu * (std::cos(yaw) * a1 + std::sin(yaw) * a2);
    };

    constexpr int kLastStep = 40;
    double walked = 0.0;
    for (const char* sat_id : { "G07", "G08" }) {
        for (int i = 0; i <= kLastStep; ++i) {
            walked = gc.computePhaseWindUp(
                sat_id, System::GPS, rx_pos, sat_pos, kLambda,
                GnssTime{ 2300, 100000.0 + 30.0 * static_cast<double>(i) },
                sun_at_step(i), Vector3d::Zero(), true);
        }
    }
    // 40 steps of -1/72 cyc from a seed of -0.082688 cyc.
    ASSERT_NEAR(walked / kLambda, -0.638244, 1e-5);
    ASSERT_GT(std::abs(walked), kLambda / 2.0) << "premise: past the fractional range";

    const Vector3d sun_end = sun_at_step(kLastStep);
    const double t_end = 100000.0 + 30.0 * static_cast<double>(kLastStep);

    // 30 s later, same geometry: the accumulator continues.
    const double cont = gc.computePhaseWindUp("G07", System::GPS, rx_pos, sat_pos,
                                              kLambda, GnssTime{ 2300, t_end + 30.0 },
                                              sun_end, Vector3d::Zero(), true);
    EXPECT_NEAR(cont, walked, 1e-12);

    // 301 s later, same geometry: the accumulator restarts from the raw angle.
    const double restart = gc.computePhaseWindUp("G08", System::GPS, rx_pos, sat_pos,
                                                 kLambda, GnssTime{ 2300, t_end + 301.0 },
                                                 sun_end, Vector3d::Zero(), true);
    EXPECT_LE(std::abs(restart), kLambda / 2.0 + 1e-12);
    EXPECT_NEAR((restart - cont) / kLambda, 1.0, 1e-9);
}

// ---------------------------------------------------------------------------
// 9. The other two satellite-frame branches.
//
// computePhaseWindUp builds x_s / y_s in three places, and cases 1-8 above reach
// only the yaw-steering one.  The single unconditional minus on the Ds line is
// valid only if all three branches agree on handedness relative to k, i.e. all
// three satisfy x_s x y_s == ez (the geocentric nadir, ~ +k).  The pre-fix
// velocity fallback was framed about z_s = -ez and had the opposite handedness,
// so the pre-fix "+" was right on that branch and wrong on the other two.  These
// two cases pin the invariant that makes one hard-coded sign valid.
//
// Both branches are reachable in production:
//   - orbit-normal is unconditionally live for the BeiDou-2 GEO PRN list
//     (C01-C05, C59, C60);
//   - the velocity fallback is the `else` of `sun_pos.norm() > 1e6`, reached
//     whenever the Sun ephemeris fails (Antenna.cpp warns and continues).
// ---------------------------------------------------------------------------
TEST(PhaseWindUpSignTest, OrbitNormalBranchSharesTheNadirHandedness) {
    LogManager log_mgr;
    GnssCorrections gc(log_mgr);

    const double lat = 22.0 * kDeg, lon = 114.0 * kDeg;     // near Hong Kong, in BeiDou GEO view
    const Vector3d rx_pos  = Coordinates::llhToEcef(lat, lon, 30.0);
    const Vector3d sat_pos = satelliteAt(rx_pos, lat, lon, 55.0, 200.0);
    // A velocity with a real out-of-plane component so r x v is well conditioned.
    const Vector3d sat_vel(-1200.0, 2900.0, 180.0);

    // The oracle reproduces the branch's input triad, then builds the dipole
    // through the y-free closed form.  Orbit-normal: y_s = unit(r x v), then
    // x_s = y_s x ez and y_s = ez x x_s, so x_s x y_s == ez by construction.
    const Vector3d ez = -sat_pos.normalized();
    // With correct_velocity_frame = true (as passed below), the branch converts
    // the incoming ECEF velocity to inertial before forming the orbit normal
    // (SatAttitude::inertialVelocity); the oracle must match the branch's input
    // triad, so it applies the same conversion.  The dipole handedness this test
    // pins is independent of the velocity frame.
    Vector3d ys = sat_pos.cross(SatAttitude::inertialVelocity(sat_pos, sat_vel)).normalized();
    Vector3d xs = ys.cross(ez).normalized();
    ys = ez.cross(xs).normalized();
    ASSERT_NEAR(xs.cross(ys).dot(ez), 1.0, 1e-12) << "orbit-normal triad is not +ez right-handed";

    const Coordinates::Llh llh = Coordinates::ecefToLlh(rx_pos);
    const Vector3d xr = geodeticNorth(llh.lat_rad, llh.lon_rad);
    const Vector3d zr = xr.cross(geodeticEast(llh.lon_rad));
    const Vector3d k  = (rx_pos - sat_pos).normalized();

    const Vector3d ds = Ref::dipole(k, xs, ez);
    const Vector3d dr = Ref::dipole(k, xr, zr);
    ASSERT_NEAR(ds.norm(), 1.0 + k.dot(ez), 1e-12);          // 1 + cos(eps_s)
    ASSERT_NEAR(dr.norm(), 1.0 + k.dot(zr), 1e-12);          // 1 + cos(eps_r)
    const double expect_m = Ref::angleAboutK(k, ds, dr) * kLambda / kTwoPi;
    ASSERT_GT(std::abs(expect_m) / kLambda, 0.02) << "geometry is sign-blind";

    // "C01" selects the orbit-normal branch; the Sun is supplied but ignored there.
    const double got = gc.computePhaseWindUp("C01", System::BeiDou, rx_pos, sat_pos,
                                             kLambda, GnssTime{ 2300, 100000.0 },
                                             sunAt(sat_pos, 0.4, 0.5, 0.77), sat_vel, false,
                                             /*max_gap_s=*/300.0, /*correct_velocity_frame=*/true);
    EXPECT_NEAR(got, expect_m, kMetreTol);

    // A non-GEO BeiDou PRN must take the yaw-steering branch instead, and must
    // therefore disagree with the orbit-normal oracle; otherwise this case would
    // pass even if the PRN gate were broken.
    const Vector3d sun = sunAt(sat_pos, 0.4, 0.5, 0.77);
    const double yaw_expect = Ref::windUp(rx_pos, sat_pos, sun).phi_m;
    const double got_meo = gc.computePhaseWindUp("C21", System::BeiDou, rx_pos, sat_pos,
                                                 kLambda, GnssTime{ 2300, 100000.0 },
                                                 sun, sat_vel, false);
    EXPECT_NEAR(got_meo, yaw_expect, kMetreTol);
    EXPECT_GT(std::abs(got_meo - got), 1e-4) << "orbit-normal and yaw-steering must differ here";
}

TEST(PhaseWindUpSignTest, VelocityFallbackBranchSharesTheNadirHandedness) {
    LogManager log_mgr;
    GnssCorrections gc(log_mgr);

    const double lat = -33.0 * kDeg, lon = 18.5 * kDeg;
    const Vector3d rx_pos  = Coordinates::llhToEcef(lat, lon, 15.0);
    const Vector3d sat_pos = satelliteAt(rx_pos, lat, lon, 38.0, 295.0);
    const Vector3d sat_vel(2400.0, -1500.0, 2800.0);

    // Zero Sun => sun_pos.norm() <= 1e6 => the velocity fallback branch.
    const Vector3d no_sun = Vector3d::Zero();

    const Vector3d ez = -sat_pos.normalized();
    Vector3d ys = sat_vel.normalized().cross(ez);
    ys.normalize();
    Vector3d xs = ys.cross(ez);
    xs.normalize();
    ys = ez.cross(xs).normalized();
    ASSERT_NEAR(xs.cross(ys).dot(ez), 1.0, 1e-12) << "velocity-fallback triad is not +ez right-handed";

    const Coordinates::Llh llh = Coordinates::ecefToLlh(rx_pos);
    const Vector3d xr = geodeticNorth(llh.lat_rad, llh.lon_rad);
    const Vector3d zr = xr.cross(geodeticEast(llh.lon_rad));
    const Vector3d k  = (rx_pos - sat_pos).normalized();

    const Vector3d ds = Ref::dipole(k, xs, ez);
    const Vector3d dr = Ref::dipole(k, xr, zr);
    ASSERT_NEAR(ds.norm(), 1.0 + k.dot(ez), 1e-12);
    const double expect_m = Ref::angleAboutK(k, ds, dr) * kLambda / kTwoPi;
    ASSERT_GT(std::abs(expect_m) / kLambda, 0.02) << "geometry is sign-blind";

    const double got = gc.computePhaseWindUp("G11", System::GPS, rx_pos, sat_pos,
                                             kLambda, GnssTime{ 2300, 100000.0 },
                                             no_sun, sat_vel, false);
    EXPECT_NEAR(got, expect_m, kMetreTol);

    // Reframe gate.  The pre-fix branch was built about z_s = -ez and paired with
    // a plus.  Reframing about +ez negates y_s and leaves x_s unchanged, so the
    // satellite dipole is numerically unchanged by the combined reframe and sign
    // edit.  Reconstruct the old frame explicitly and assert that the old
    // (frame, sign) pair reproduces the new Ds to 1e-12.  If this fails, the
    // reframe is wrong, not the sign.
    const Vector3d zs_old = -ez;
    Vector3d ys_old = sat_vel.normalized().cross(zs_old);
    ys_old.normalize();
    Vector3d xs_old = ys_old.cross(zs_old);
    xs_old.normalize();
    const Vector3d ds_old_plus = xs_old - k * k.dot(xs_old) + k.cross(ys_old);
    const Vector3d ds_new_minus = xs - k * k.dot(xs) - k.cross(ys);
    EXPECT_LT((ds_old_plus - ds_new_minus).norm(), 1e-12)
        << "the velocity-fallback reframe moved Ds; it must be a numerical no-op";

    // On this branch only the receiver line changed: the pre-fix code was in the
    // half-fix state here (satellite right, receiver wrong), so the old
    // whole-function result must differ from the new one.
    const Vector3d dr_old_plus = xr - k * k.dot(xr) + k.cross(geodeticEast(llh.lon_rad));
    const double old_m = Ref::angleAboutK(k, ds_old_plus, dr_old_plus) * kLambda / kTwoPi;
    EXPECT_GT(std::abs(old_m - got), 1e-4)
        << "the pre-fix velocity-fallback result should NOT match the corrected one";
}

}  // namespace
}  // namespace GnssProcessor
