/**
 * @file test_orbit_enhancements.cpp
 * @brief Unit tests for the M-PAGES orbit-layer port (backlog item #14):
 *        Interpolate::trigSvdInterp (sidereal-trig SVD SP3 interpolation) and
 *        SatAttitude (beta-driven orbit-normal yaw attitude).
 *
 * Both features are public statics, so no friend shim is needed.
 *
 * Coverage:
 *   trigSvdInterp
 *     - Reproduces a function lying exactly in the 9-term trig span (value AND
 *       analytic derivative) — the rigorous basis/solve/derivative check.
 *     - Passes through the nodes when n == 9 (square exact fit).
 *     - With 11 nodes is an LS smoother (does NOT pass through, residual small).
 *     - A uniform (all-equal) sigma set is inert: identical to an empty/unweighted
 *       fit (mirrors M-PAGES' per-satellite SVsig).
 *     - Agrees with the legacy Neville fit on a smooth orbit-like function.
 *   SatAttitude
 *     - Yaw-steering frame is bit-identical (<1e-12) to the legacy inline engine
 *       construction (flag-off byte-identity for GPS/GAL/GLO/BDS-MEO).
 *     - BeiDou-2 GEO orbit-normal frame is bit-identical to the legacy inline ON
 *       construction (flag-off byte-identity for the GEO PRN list).
 *     - A non-GEO BeiDou PRN on the legacy path stays yaw-steering.
 *     - The returned frame is orthonormal and right-handed.
 *     - determineMode() reproduces the M-PAGES signed-beta threshold table.
 *     - computeBeta() sign tracks the +(r x v) orbit normal.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include <Eigen/Dense>

#include "math/Interpolate.hpp"
#include "model/SatAttitude.hpp"
#include "common/GnssTypes.hpp"

using namespace GnssProcessor;

namespace {

constexpr double kPi = 3.14159265358979;
// Same fundamental as trigSvdInterp: sidereal day expressed per second.
const double kP0 = 2.0 * kPi / (0.99726956634 * 86400.0);

// A function lying exactly in the 9-term basis span {1, sin(f*P0*t), cos(f*P0*t)}
// for f = 1..4, evaluated at the window-local offset tau (seconds).
double trigRef(double tau, double& deriv) {
    static const double c0 = 137.0;
    static const double a[4] = { 50.0, -12.0, 7.0, -3.0 };   // sin coeffs
    static const double b[4] = { 30.0,  18.0, -9.0, 4.0 };   // cos coeffs
    double v = c0;
    deriv = 0.0;
    for (int f = 1; f <= 4; ++f) {
        const double wf = static_cast<double>(f) * kP0;
        const double arg = tau * wf;
        v     += a[f - 1] * std::sin(arg) + b[f - 1] * std::cos(arg);
        deriv += a[f - 1] * (wf * std::cos(arg)) + b[f - 1] * (-wf * std::sin(arg));
    }
    return v;
}

}  // namespace

// ---------------------------------------------------------------------------
//  trigSvdInterp
// ---------------------------------------------------------------------------

TEST(TrigSvdInterp, ReproducesTrigSpanValueAndDerivative) {
    // 9 nodes at 900 s spacing on a large GPS-seconds base (exercises the
    // x[0]-subtraction conditioning the same way the engine does).
    const double X0 = 1.36e8;
    std::vector<double> x(9), y(9);
    for (int i = 0; i < 9; ++i) {
        x[i] = X0 + i * 900.0;
        double d;
        y[i] = trigRef(x[i] - X0, d);
    }
    // Interior query midway between node 4 and 5.
    const double xq = X0 + 4.5 * 900.0;
    double dref;
    const double vref = trigRef(xq - X0, dref);

    double val, deriv;
    Interpolate::trigSvdInterp(x, y, {}, xq, val, deriv);

    EXPECT_NEAR(val, vref, 1e-4);          // ~1e-6 relative on a ~150 m scale
    EXPECT_NEAR(deriv, dref, 1e-6);        // m/s
}

TEST(TrigSvdInterp, PassesThroughNodesWhenNine) {
    const double X0 = 1.36e8;
    std::vector<double> x(9), y(9);
    for (int i = 0; i < 9; ++i) {
        x[i] = X0 + i * 900.0;
        double d;
        y[i] = trigRef(x[i] - X0, d);
    }
    for (int k = 0; k < 9; ++k) {
        double val, deriv;
        Interpolate::trigSvdInterp(x, y, {}, x[k], val, deriv);
        EXPECT_NEAR(val, y[k], 1e-4) << "node " << k;
    }
}

TEST(TrigSvdInterp, ElevenNodesIsSmootherNotInterpolator) {
    // 11 nodes carrying a high-frequency alternating component the smooth 9-term
    // basis cannot represent -> an overdetermined LS fit (rank 9 < 11 nodes) that
    // does NOT pass through the nodes.  A Neville degree-10 fit WOULD pass through
    // all 11 exactly; the trig smoother leaves clear residuals.
    const double X0 = 1.36e8;
    std::vector<double> x(11), y(11);
    for (int i = 0; i < 11; ++i) {
        x[i] = X0 + i * 900.0;
        double d;
        y[i] = trigRef(x[i] - X0, d) + (i % 2 == 0 ? 2.0 : -2.0);  // +-2 m sawtooth
    }
    double max_resid = 0.0, max_abs = 0.0;
    for (int k = 0; k < 11; ++k) {
        double val, deriv;
        Interpolate::trigSvdInterp(x, y, {}, x[k], val, deriv);
        max_resid = std::max(max_resid, std::abs(val - y[k]));
        max_abs = std::max(max_abs, std::abs(val));
    }
    EXPECT_GT(max_resid, 0.1);              // clearly does not interpolate the nodes
    EXPECT_LT(max_resid, 10.0);             // but the smoother stays bounded
    EXPECT_GT(max_abs, 0.0);
}

TEST(TrigSvdInterp, UniformSigmaIsInert) {
    const double X0 = 1.36e8;
    std::vector<double> x(9), y(9);
    for (int i = 0; i < 9; ++i) {
        x[i] = X0 + i * 900.0;
        double d;
        y[i] = trigRef(x[i] - X0, d);
    }
    const double xq = X0 + 3.3 * 900.0;

    double v_empty, d_empty, v_uniform, d_uniform;
    Interpolate::trigSvdInterp(x, y, {}, xq, v_empty, d_empty);
    std::vector<double> sig(9, 0.032);      // all-equal (per-satellite SVsig case)
    Interpolate::trigSvdInterp(x, y, sig, xq, v_uniform, d_uniform);

    EXPECT_DOUBLE_EQ(v_empty, v_uniform);
    EXPECT_DOUBLE_EQ(d_empty, d_uniform);
}

TEST(TrigSvdInterp, AgreesWithNevilleOnSmoothOrbit) {
    // A slowly varying (sub-period) smooth function both bases represent well.
    const double X0 = 1.36e8;
    std::vector<double> x(9), yn(9);
    for (int i = 0; i < 9; ++i) {
        x[i] = X0 + i * 900.0;
        const double tau = x[i] - X0;
        yn[i] = 20000e3 + 1234.0 * std::sin(kP0 * tau) + 0.5e-3 * tau * tau;
    }
    const double xq = X0 + 4.2 * 900.0;
    double v_trig, d_trig, v_nev, d_nev;
    Interpolate::trigSvdInterp(x, yn, {}, xq, v_trig, d_trig);
    Interpolate::nevilleInterpDeriv(x, yn, xq, v_nev, d_nev);
    EXPECT_NEAR(v_trig, v_nev, 1e-2);       // < 1 cm in-window
}

// ---------------------------------------------------------------------------
//  SatAttitude — flag-off byte-identity vs the legacy inline construction
// ---------------------------------------------------------------------------

namespace {

// Independent re-implementation of the legacy Antenna.cpp yaw-steering block.
void legacyYaw(const Eigen::Vector3d& pos, const Eigen::Vector3d& vel,
    const Eigen::Vector3d& sun, Eigen::Vector3d& i_s, Eigen::Vector3d& j_s,
    Eigen::Vector3d& k_s) {
    k_s = -pos.normalized();
    Eigen::Vector3d e_sun = (sun - pos).normalized();
    j_s = k_s.cross(e_sun);
    if (j_s.norm() < 1e-8) {
        Eigen::Vector3d vhat = vel.norm() > 1e-8 ? vel.normalized() : Eigen::Vector3d::UnitY();
        j_s = k_s.cross(vhat);
    }
    j_s.normalize();
    i_s = j_s.cross(k_s);
    if (i_s.norm() < 1e-8) i_s = Eigen::Vector3d::UnitX();
    i_s.normalize();
}

// Independent re-implementation of the legacy Antenna.cpp orbit-normal block.
void legacyOrbitNormal(const Eigen::Vector3d& pos, const Eigen::Vector3d& vel,
    Eigen::Vector3d& i_s, Eigen::Vector3d& j_s, Eigen::Vector3d& k_s) {
    k_s = -pos.normalized();
    Eigen::Vector3d on = pos.cross(vel);
    if (on.norm() < 1e-8) on = Eigen::Vector3d::UnitY();
    j_s = on.normalized();
    i_s = j_s.cross(k_s);
    if (i_s.norm() < 1e-8) i_s = Eigen::Vector3d::UnitX();
    i_s.normalize();
    j_s = k_s.cross(i_s);
    j_s.normalize();
}

}  // namespace

TEST(SatAttitude, YawSteerMatchesLegacyEngineBitForBit) {
    const Eigen::Vector3d pos(15600e3, 7540e3, 20140e3);
    const Eigen::Vector3d vel(-2000.0, 2500.0, 1000.0);
    const Eigen::Vector3d sun(1.40e11, 0.50e11, 0.20e11);

    Eigen::Vector3d ie, je, ke;
    legacyYaw(pos, vel, sun, ie, je, ke);

    auto att = SatAttitude::compute(System::GPS, 5, pos, vel, sun, /*enable_on=*/false);
    EXPECT_LT((att.i_s - ie).norm(), 1e-12);
    EXPECT_LT((att.j_s - je).norm(), 1e-12);
    EXPECT_LT((att.k_s - ke).norm(), 1e-12);
    EXPECT_EQ(att.mode, SatAttitude::Mode::YawSteering);
}

TEST(SatAttitude, GeoOrbitNormalMatchesLegacyEngineOnInertialVelocity) {
    // A BeiDou GEO PRN (C01) on the legacy path uses orbit-normal.
    //
    // This was originally a bit-for-bit pin against the inline Antenna.cpp block,
    // proving the SatAttitude extraction changed nothing.  The extraction is still
    // exact -- but the orbit-normal branch now converts the incoming ECEF velocity
    // to inertial first, because the orbit plane is an inertial construct and
    // r x v_ecef = h - r x (omega x r) is not the orbit normal (see
    // SatAttitude::inertialVelocity; measured 2.2-30.3 deg for GPS, up to 65 deg
    // for BeiDou IGSO on real CODE MGX orbits).  So the pin is against the same
    // legacy construction fed the CORRECTED velocity.
    //
    // The branch is dormant on current lineages: it is reachable only for the
    // BeiDou-2 GEO PRNs C01-C05/C59/C60, which are absent from the CODE MGX
    // products this repo consumes.
    const Eigen::Vector3d pos(-30000e3, 28000e3, 1500e3);   // near-GEO geometry
    const Eigen::Vector3d vel(-2000.0, -2100.0, 50.0);      // ECEF, as getSatellitePV returns
    const Eigen::Vector3d sun(1.40e11, 0.40e11, 0.10e11);

    Eigen::Vector3d ie, je, ke;
    legacyOrbitNormal(pos, SatAttitude::inertialVelocity(pos, vel), ie, je, ke);

    auto att = SatAttitude::compute(System::BeiDou, 1, pos, vel, sun, /*enable_on=*/false,
                                   /*correct_velocity_frame=*/true);
    EXPECT_LT((att.i_s - ie).norm(), 1e-12);
    EXPECT_LT((att.j_s - je).norm(), 1e-12);
    EXPECT_LT((att.k_s - ke).norm(), 1e-12);
    EXPECT_EQ(att.mode, SatAttitude::Mode::OrbitNormalPositive);

    // Pin the repair as ACTIVE: the raw-ECEF-velocity frame is measurably different,
    // so this test fails if the conversion is ever dropped.
    Eigen::Vector3d ir, jr, kr;
    legacyOrbitNormal(pos, vel, ir, jr, kr);
    EXPECT_GT((att.i_s - ir).norm(), 1e-6);
    EXPECT_GT((att.j_s - jr).norm(), 1e-6);
}

TEST(SatAttitude, NonGeoBeiDouLegacyPathIsYawSteering) {
    const Eigen::Vector3d pos(15600e3, 7540e3, 20140e3);
    const Eigen::Vector3d vel(-2000.0, 2500.0, 1000.0);
    const Eigen::Vector3d sun(1.40e11, 0.50e11, 0.20e11);

    // PRN 30 is not in the GEO list {1-5,59,60} -> yaw-steering even on flag-off.
    auto att = SatAttitude::compute(System::BeiDou, 30, pos, vel, sun, /*enable_on=*/false);
    Eigen::Vector3d ie, je, ke;
    legacyYaw(pos, vel, sun, ie, je, ke);
    EXPECT_LT((att.i_s - ie).norm(), 1e-12);
    EXPECT_EQ(att.mode, SatAttitude::Mode::YawSteering);
}

TEST(SatAttitude, FrameIsOrthonormalRightHanded) {
    const Eigen::Vector3d pos(15600e3, 7540e3, 20140e3);
    const Eigen::Vector3d vel(-2000.0, 2500.0, 1000.0);
    const Eigen::Vector3d sun(1.40e11, 0.50e11, 0.20e11);
    auto att = SatAttitude::compute(System::GPS, 5, pos, vel, sun, false);

    EXPECT_NEAR(att.i_s.norm(), 1.0, 1e-12);
    EXPECT_NEAR(att.j_s.norm(), 1.0, 1e-12);
    EXPECT_NEAR(att.k_s.norm(), 1.0, 1e-12);
    EXPECT_NEAR(att.i_s.dot(att.j_s), 0.0, 1e-12);
    EXPECT_NEAR(att.i_s.dot(att.k_s), 0.0, 1e-12);
    EXPECT_NEAR(att.j_s.dot(att.k_s), 0.0, 1e-12);
    EXPECT_LT((att.i_s.cross(att.j_s) - att.k_s).norm(), 1e-12);   // right-handed
}

// ---------------------------------------------------------------------------
//  SatAttitude — mode table + beta sign (M-PAGES parity)
// ---------------------------------------------------------------------------

TEST(SatAttitude, DetermineModeMatchesMpagesTable) {
    using M = SatAttitude::Mode;
    // GPS/GAL/GLO always yaw-steer.
    EXPECT_EQ(SatAttitude::determineMode(System::GPS, -50.0), M::YawSteering);
    EXPECT_EQ(SatAttitude::determineMode(System::Galileo, 0.0), M::YawSteering);
    EXPECT_EQ(SatAttitude::determineMode(System::GLONASS, 90.0), M::YawSteering);
    // BeiDou: signed beta >= 4 -> YS, else ON+.
    EXPECT_EQ(SatAttitude::determineMode(System::BeiDou, 3.9), M::OrbitNormalPositive);
    EXPECT_EQ(SatAttitude::determineMode(System::BeiDou, 4.0), M::YawSteering);
    EXPECT_EQ(SatAttitude::determineMode(System::BeiDou, -10.0), M::OrbitNormalPositive);
    // QZSS: signed beta >= 20 -> YS, else ON-.
    EXPECT_EQ(SatAttitude::determineMode(System::QZSS, 19.9), M::OrbitNormalNegative);
    EXPECT_EQ(SatAttitude::determineMode(System::QZSS, 20.0), M::YawSteering);
    EXPECT_EQ(SatAttitude::determineMode(System::QZSS, -30.0), M::OrbitNormalNegative);
    // SBAS / Unknown -> YS.
    EXPECT_EQ(SatAttitude::determineMode(System::SBAS, 0.0), M::YawSteering);
    EXPECT_EQ(SatAttitude::determineMode(System::Unknown, 0.0), M::YawSteering);
}

TEST(SatAttitude, BetaSignTracksOrbitNormal) {
    // Orbit in the x-y plane: r x v = +z.  Sun on +z -> beta > 0; on -z -> beta < 0.
    const Eigen::Vector3d pos(7000e3, 0.0, 0.0);
    const Eigen::Vector3d vel(0.0, 7000.0, 0.0);
    EXPECT_GT(SatAttitude::computeBeta(pos, vel, Eigen::Vector3d(0.0, 0.0, 1.0e11)), 0.0);
    EXPECT_LT(SatAttitude::computeBeta(pos, vel, Eigen::Vector3d(0.0, 0.0, -1.0e11)), 0.0);
    // Degenerate geometry returns 0 (no throw).
    EXPECT_DOUBLE_EQ(SatAttitude::computeBeta(pos, pos, Eigen::Vector3d(1.0e11, 0, 0)), 0.0);
}
