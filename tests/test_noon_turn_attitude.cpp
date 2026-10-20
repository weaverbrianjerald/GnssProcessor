/**
 * @file test_noon_turn_attitude.cpp
 * @brief Tests for the shared satellite attitude model: the beta-angle sign
 *        validation, the GPS Block IIF/IIR noon turn (Kouba 2009), its use by the
 *        phase wind-up, and the @c processing.enable_noon_turn_attitude knob.
 *
 * Three groups:
 *
 *  - Validation of @c SatAttitude::computeBeta, @c orbitGeometry and the
 *    yaw-steering frame of @c SatAttitude::compute against reference rows built
 *    without any project code (scripts/attitude_beta_check.py: SP3 positions in
 *    the inertial frame, an Almanac Sun, the orbit normal from the SP3 chord, and
 *    a velocity-free second beta from the broadcast inclination and node; DOY 129
 *    of 2024, CODE MGX orbits).  The rows cover both signs of beta.  They are the
 *    external anchor of the sign convention: beta positive on the +(r x v) side
 *    of the orbit plane, yaw positive about nadir from the velocity direction.
 *  - The noon-turn yaw law: thresholds, continuity, the constant rate, the turn
 *    direction, symmetry in beta and the analytic duration at beta 0.
 *  - The consumers: the frame returned by @c applyNoonTurn, the wind-up with and
 *    without the law (bit-identical when the law is absent), and the config knob.
 *
 * The law and its geometry are exact restatements of the model; a test that
 * pins a number from a run says so.  Regenerate the reference rows with
 *   python scripts/attitude_beta_check.py csv --sp3 <COD0MGXFIN ORB.SP3>
 *       --nav <BRDC MN.rnx> --sats G01,G03,G10,G13,G25,G26,G32 --out rows.csv
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "common/Constants.hpp"
#include "corrections/GnssCorrections.hpp"
#include "logging/LogManager.hpp"
#include "math/Coordinates.hpp"
#include "model/SatAttitude.hpp"
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace fs = std::filesystem;
using namespace GnssProcessor;
using Eigen::Vector3d;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

/// Difference of two angles in degrees, wrapped to [-180, 180].
double wrapDeg(double d) {
    d = std::fmod(d + 180.0, 360.0);
    if (d < 0.0) d += 360.0;
    return d - 180.0;
}

// ---------------------------------------------------------------------------
// Reference rows from the independent computation (see the file banner).
// pos/vel in ECEF (m, m/s; vel is d(pos)/dt of the ECEF series), sun in ECEF (m).
// beta: from the SP3 orbit normal in the inertial frame; beta_kep: from the
// broadcast inclination and node (NaN for none); mu: orbit angle from the orbit
// midnight; psi: Kouba nominal yaw atan2(-tan beta, sin mu).
// ---------------------------------------------------------------------------
struct RefRow {
    const char* sat;
    double jd;
    double pos[3];
    double vel[3];
    double sun[3];
    double beta, beta_kep, mu, psi;
};

const RefRow kRows[] = {
    {"G25", 2460438.86111111, {16367724.443000, 19295065.773000, 8158596.488000}, {242.440835826, 1084.720871126, -2903.956215861}, {93350725944.394, 108141858875.643, 44390350782.002}, -0.745904, -0.745918, 179.8050, 75.3472},
    {"G26", 2460438.98611111, {26190235.632000, 3685663.943000, 3905634.002000}, {411.971795695, 453.281516228, -3034.336378924}, {142451151767.815, 10453177412.261, 44473850294.128}, 2.449055, 2.449459, 189.3254, -165.2142},
    {"G26", 2460439.34027778, {-8648063.209000, 15863286.822000, 19200497.841000}, {-1161.457452284, -2212.479942152, 1331.111289063}, {-78394001426.662, -119310432605.924, 44709329336.030}, 2.777527, 2.778515, 84.8115, -2.7889},
    {"G01", 2460439.34027778, {-10799098.865000, 12840766.531000, 20137856.964000}, {-941.019448589, -2482.271400177, 1091.124631703}, {-78394001426.662, -119310432605.924, 44709329336.030}, -1.857661, -1.856828, 93.1433, 1.8605},
    {"G10", 2460438.61111111, {-3162636.909000, -14740593.810000, 22043732.587000}, {2491.849718509, -1154.562685645, -449.571059282}, {-108186405337.475, 93378656533.229, 44222744398.994}, 21.766435, 21.766496, 270.9184, -158.2310},
    {"G13", 2460439.25694444, {-5644172.981000, -14202653.792000, -21943629.611000}, {2680.869098126, -527.922658967, -328.420680964}, {-8234606949.237, -142540268870.240, 44654069431.186}, 71.713148, 71.712946, 150.3663, -80.7199},
    {"G03", 2460439.40277778, {10854746.336000, 22271856.913000, -9894489.299000}, {-939.747491847, -807.489132852, -2881.229782012}, {-118074998741.213, -80219642938.325, 44750714822.048}, 21.331097, 21.330369, 19.1904, -49.9112},
    {"G32", 2460438.50694444, {-15151654.964000, -18022430.547000, -12271155.888000}, {-297.099919097, -1572.284005660, 2610.825079784}, {-142697196821.629, 8220381868.365, 44152669898.451}, 67.544518, 67.543991, 193.9928, -95.7070},
};

/// Yaw (deg) of a body X axis about nadir, from the inertial velocity direction.
double yawOf(const Vector3d& x_axis, const Vector3d& pos, const Vector3d& vel_ecef) {
    const Vector3d n = pos.cross(SatAttitude::inertialVelocity(pos, vel_ecef)).normalized();
    const Vector3d v_hat = n.cross(pos.normalized());
    return std::atan2(-x_axis.dot(n), x_axis.dot(v_hat)) / kDeg;
}

// ---------------------------------------------------------------------------
// A synthetic circular GPS orbit with a chosen beta and orbit angle.  The ECEF
// frame is taken to coincide with the inertial frame at the instant, which is all
// the attitude model needs; the ECEF velocity is the inertial one minus omega x r.
// ---------------------------------------------------------------------------
struct Fix {
    Vector3d pos, vel_ecef, sun, n_hat, v_hat;
};

Fix makeFix(double beta_deg, double mu_deg) {
    const Vector3d n = Vector3d(0.3, -0.5, 0.8).normalized();
    const Vector3d p = n.cross(Vector3d::UnitZ()).normalized();
    const Vector3d q = n.cross(p);
    const double mu = mu_deg * kDeg, b = beta_deg * kDeg;
    const Vector3d r_hat = -std::cos(mu) * p - std::sin(mu) * q;
    const double a = 26560.0e3, vc = 3874.0;
    Fix f;
    f.pos = a * r_hat;
    f.n_hat = n;
    f.v_hat = n.cross(r_hat);
    const Vector3d v_in = vc * f.v_hat;
    const double w = Constants::EARTH_ANGULAR_VELOCITY;
    f.vel_ecef = Vector3d(v_in.x() + w * f.pos.y(), v_in.y() - w * f.pos.x(), v_in.z());
    // Far enough that the Sun direction from the satellite equals the one from the
    // Earth centre to 1e-9 deg, so beta and mu are exactly the requested values.
    f.sun = 1.0e17 * (std::cos(b) * p + std::sin(b) * n);
    return f;
}

const SatAttitude::NoonTurnLaw kIif{ 0.11, 0.00836 };
const SatAttitude::NoonTurnLaw kIir{ 0.20, 0.00836 };

}  // namespace

// ===========================================================================
// Beta, orbit angle and the nominal frame against the independent reference
// ===========================================================================

TEST(NoonTurnBetaReference, ComputeBetaMatchesIndependentValueAndSign) {
    int checked_sign = 0;
    for (const RefRow& r : kRows) {
        const Vector3d pos(r.pos[0], r.pos[1], r.pos[2]);
        const Vector3d vel(r.vel[0], r.vel[1], r.vel[2]);
        const Vector3d sun(r.sun[0], r.sun[1], r.sun[2]);
        const double beta = SatAttitude::computeBeta(pos, vel, sun, /*correct_velocity_frame=*/true);
        EXPECT_NEAR(beta, r.beta, 0.015) << r.sat;
        // The velocity-free broadcast-element beta agrees with the SP3 one.
        EXPECT_NEAR(r.beta_kep, r.beta, 0.01) << r.sat;
        if (std::abs(r.beta) > 1.0) {
            EXPECT_GT(beta * r.beta, 0.0) << r.sat << ": sign differs from the reference";
            ++checked_sign;
        }
    }
    EXPECT_GE(checked_sign, 6);
    bool saw_pos = false, saw_neg = false;
    for (const RefRow& r : kRows) { saw_pos |= r.beta > 1.0; saw_neg |= r.beta < -1.0; }
    EXPECT_TRUE(saw_pos && saw_neg) << "reference rows must cover both signs of beta";
}

TEST(NoonTurnBetaReference, EcefVelocityGivesWrongBetaWhichIsWhyTheTurnConverts) {
    // computeBeta with the ECEF velocity as is (correct_velocity_frame false) is
    // not the inertial orbit plane.  On the G01 row the independent beta is -1.9
    // deg and the uncorrected value is many degrees off.
    const RefRow& g01 = kRows[3];
    const Vector3d pos(g01.pos[0], g01.pos[1], g01.pos[2]);
    const Vector3d vel(g01.vel[0], g01.vel[1], g01.vel[2]);
    const Vector3d sun(g01.sun[0], g01.sun[1], g01.sun[2]);
    const double raw = SatAttitude::computeBeta(pos, vel, sun, false);
    EXPECT_GT(std::abs(raw - g01.beta), 5.0);
    // The noon-turn geometry always converts, whatever the flag.
    const SatAttitude::OrbitGeometry g = SatAttitude::orbitGeometry(pos, vel, sun);
    ASSERT_TRUE(g.valid);
    EXPECT_NEAR(g.beta_rad / kDeg, g01.beta, 0.015);
}

TEST(NoonTurnBetaReference, OrbitAngleMatchesIndependentValue) {
    for (const RefRow& r : kRows) {
        const SatAttitude::OrbitGeometry g = SatAttitude::orbitGeometry(
            Vector3d(r.pos[0], r.pos[1], r.pos[2]), Vector3d(r.vel[0], r.vel[1], r.vel[2]),
            Vector3d(r.sun[0], r.sun[1], r.sun[2]));
        ASSERT_TRUE(g.valid) << r.sat;
        EXPECT_NEAR(wrapDeg(g.orbit_angle_rad / kDeg - r.mu), 0.0, 0.05) << r.sat;
    }
}

TEST(NoonTurnBetaReference, YawSteeringFrameCarriesKoubaNominalYaw) {
    // The vector construction of the nominal frame (j = k x e_sun, i = j x k) must
    // sit at the Kouba yaw atan2(-tan beta, sin mu) measured about nadir from the
    // inertial velocity, with beta and mu from the independent computation.  This
    // fixes the yaw sign convention the noon-turn rotation relies on.
    for (const RefRow& r : kRows) {
        const Vector3d pos(r.pos[0], r.pos[1], r.pos[2]);
        const Vector3d vel(r.vel[0], r.vel[1], r.vel[2]);
        const Vector3d sun(r.sun[0], r.sun[1], r.sun[2]);
        const SatAttitude::Frame fr = SatAttitude::compute(System::GPS, 5, pos, vel, sun, false, true);
        EXPECT_NEAR(wrapDeg(yawOf(fr.i_s, pos, vel) - r.psi), 0.0, 0.02) << r.sat;
    }
}

// ===========================================================================
// The law table and the yaw law
// ===========================================================================

TEST(NoonTurnLawTable, BlockSelectsTheRateAndOtherSatellitesHaveNone) {
    auto iif = SatAttitude::noonTurnLaw(System::GPS, "G25_BLOCKIIF_G062");
    ASSERT_TRUE(iif.has_value());
    EXPECT_DOUBLE_EQ(iif->max_yaw_rate_deg_s, 0.11);
    EXPECT_NEAR(iif->beta0Deg(), 4.35, 0.01);

    for (const char* t : { "G14_BLOCKIIR-A_G041", "G13_BLOCKIIR-B_G043", "G12_BLOCKIIR-M_G048", "G11_BLOCKIIR_G046" }) {
        auto iir = SatAttitude::noonTurnLaw(System::GPS, t);
        ASSERT_TRUE(iir.has_value()) << t;
        EXPECT_DOUBLE_EQ(iir->max_yaw_rate_deg_s, 0.20) << t;
        EXPECT_NEAR(iir->beta0Deg(), 2.39, 0.01) << t;
    }

    for (const char* t : { "G04_BLOCKIIIA_G074", "G26_BLOCKIIA_G026", "G01_BLOCKII_G013", "G01_BLOCKI_G004",
                           "G25", "", "TRM59800.00_NONE" }) {
        EXPECT_FALSE(SatAttitude::noonTurnLaw(System::GPS, t).has_value()) << t;
    }
    // Same block string under another system: no law (Galileo stays nominal).
    EXPECT_FALSE(SatAttitude::noonTurnLaw(System::Galileo, "E21_GALILEO-2_E205").has_value());
    EXPECT_FALSE(SatAttitude::noonTurnLaw(System::Galileo, "E21_BLOCKIIF_E205").has_value());
    EXPECT_FALSE(SatAttitude::noonTurnLaw(System::GLONASS, "R01_BLOCKIIR_R701").has_value());
}

TEST(NoonTurnYawLaw, ThresholdIsBeta0) {
    // Just below beta0 a turn exists, just above it the satellite follows the Sun.
    for (const auto& law : { kIif, kIir }) {
        const double b0 = law.beta0Deg();
        const auto below = SatAttitude::noonTurnYaw((b0 - 0.05) * kDeg, kPi, law);
        const auto above = SatAttitude::noonTurnYaw((b0 + 0.05) * kDeg, kPi, law);
        EXPECT_TRUE(below.active) << "rate " << law.max_yaw_rate_deg_s;
        EXPECT_FALSE(above.active);
        EXPECT_DOUBLE_EQ(above.actual_yaw_deg, above.nominal_yaw_deg);
        EXPECT_DOUBLE_EQ(above.start_angle_deg, 0.0);
    }
    // IIF turns at 4.0 deg where IIR does not.
    EXPECT_TRUE(SatAttitude::noonTurnYaw(4.0 * kDeg, kPi, kIif).active);
    EXPECT_FALSE(SatAttitude::noonTurnYaw(4.0 * kDeg, kPi, kIir).active);
}

TEST(NoonTurnYawLaw, FollowsNominalAwayFromNoon) {
    for (double mu : { 0.0, 30.0, 90.0, 150.0, 170.0, 200.0, 270.0, 359.0 }) {
        for (double beta : { -3.0, 0.5, 2.0 }) {
            const auto y = SatAttitude::noonTurnYaw(beta * kDeg, mu * kDeg, kIif);
            EXPECT_FALSE(y.active) << "mu " << mu << " beta " << beta;
            EXPECT_DOUBLE_EQ(y.actual_yaw_deg, y.nominal_yaw_deg);
            // Kouba nominal yaw.
            EXPECT_NEAR(y.nominal_yaw_deg,
                        std::atan2(-std::tan(beta * kDeg), std::sin(mu * kDeg)) / kDeg, 1e-9);
        }
    }
}

TEST(NoonTurnYawLaw, ContinuousAtConstantRateAndRejoinsNominal) {
    constexpr double dmu = 4e-3;                             // deg of orbit angle per step
    for (const auto& law : { kIif, kIir }) {
        const double dt = dmu / law.orbit_rate_deg_s;        // seconds per step
        for (double beta : { -2.2, -0.75, 0.0, 0.3, 1.0, 2.0 }) {
            double prev = 0.0;
            bool have = false, prev_active = false;
            int n_active = 0;
            double rate_min = 1e9, rate_max = 0.0;
            for (double mu = 174.0; mu <= 196.0; mu += dmu) {
                const auto y = SatAttitude::noonTurnYaw(beta * kDeg, mu * kDeg, law);
                if (have) {
                    const double step = std::abs(wrapDeg(y.actual_yaw_deg - prev));
                    const double rate = step / dt;                             // deg/s
                    // No jump anywhere: the yaw never moves faster than the controller allows.
                    EXPECT_LT(step, 1.05 * law.max_yaw_rate_deg_s * dt) << "beta " << beta << " mu " << mu;
                    // Only steps with both ends inside the turn: a step across the
                    // start or the end mixes the nominal and the ramp.
                    if (y.active && prev_active) {
                        ++n_active;
                        rate_min = std::min(rate_min, rate);
                        rate_max = std::max(rate_max, rate);
                    }
                }
                prev = y.actual_yaw_deg;
                prev_active = y.active;
                have = true;
            }
            ASSERT_GT(n_active, 10) << "beta " << beta;
            // Inside the turn the yaw rate is the controller maximum.
            EXPECT_NEAR(rate_min, law.max_yaw_rate_deg_s, 0.01 * law.max_yaw_rate_deg_s);
            EXPECT_NEAR(rate_max, law.max_yaw_rate_deg_s, 0.01 * law.max_yaw_rate_deg_s);

            // The turn starts and ends on the nominal curve.
            const auto at = SatAttitude::noonTurnYaw(beta * kDeg, 180.0 * kDeg, law);
            for (double edge : { at.start_angle_deg, at.end_angle_deg }) {
                const auto e = SatAttitude::noonTurnYaw(beta * kDeg, edge * kDeg, law);
                EXPECT_NEAR(wrapDeg(e.actual_yaw_deg - e.nominal_yaw_deg), 0.0, 1e-6)
                    << "beta " << beta << " edge " << edge;
            }
            EXPECT_LT(at.start_angle_deg, 180.0 + 1e-9);
            EXPECT_GT(at.end_angle_deg, 180.0);
        }
    }
}

TEST(NoonTurnYawLaw, DirectionFollowsTheSignOfBetaAndMirrors) {
    // beta > 0: the nominal yaw runs 0 -> -90 -> -180 through the noon and the
    // satellite yaws toward more negative angles; beta < 0 is the mirror image.
    const auto pos_mid = SatAttitude::noonTurnYaw(1.0 * kDeg, kPi, kIif);
    ASSERT_TRUE(pos_mid.active);
    const auto before = SatAttitude::noonTurnYaw(1.0 * kDeg, (180.0 - 0.5) * kDeg, kIif);
    const auto after = SatAttitude::noonTurnYaw(1.0 * kDeg, (180.0 + 0.5) * kDeg, kIif);
    EXPECT_GT(before.actual_yaw_deg, pos_mid.actual_yaw_deg);
    EXPECT_GT(pos_mid.actual_yaw_deg, after.actual_yaw_deg);

    for (double mu : { 176.0, 178.5, 180.0, 183.0, 188.0 }) {
        const auto a = SatAttitude::noonTurnYaw(1.3 * kDeg, mu * kDeg, kIif);
        const auto b = SatAttitude::noonTurnYaw(-1.3 * kDeg, mu * kDeg, kIif);
        EXPECT_EQ(a.active, b.active) << mu;
        EXPECT_NEAR(a.actual_yaw_deg, -b.actual_yaw_deg, 1e-9) << mu;
        EXPECT_NEAR(a.nominal_yaw_deg, -b.nominal_yaw_deg, 1e-9) << mu;
    }
}

TEST(NoonTurnYawLaw, ZeroBetaTurnsFromTheNoonAtTheMaximumRate) {
    // With the Sun in the orbit plane the nominal yaw jumps by 180 deg at the noon.
    // The satellite starts yawing at the noon and needs 180 deg / rate seconds.
    for (const auto& law : { kIif, kIir }) {
        const auto y = SatAttitude::noonTurnYaw(0.0, kPi, law);
        ASSERT_TRUE(y.active);
        const double duration_s = (y.end_angle_deg - y.start_angle_deg) / law.orbit_rate_deg_s;
        EXPECT_NEAR(duration_s, 180.0 / law.max_yaw_rate_deg_s, 1.0);
        EXPECT_NEAR(y.start_angle_deg, 180.0, 1e-3);
    }
}

TEST(NoonTurnYawLaw, DurationOfAMidBetaTurnIsPinned) {
    // Regression pin of this implementation (not an external value): a Block IIF
    // satellite at beta 2.44 deg, as G26 on DOY 129 of 2024, turns for about 1000 s
    // between orbit angles 177.84 and 186.20 deg.
    const auto y = SatAttitude::noonTurnYaw(2.44 * kDeg, kPi, kIif);
    ASSERT_TRUE(y.active);
    EXPECT_NEAR(y.start_angle_deg, 177.84, 0.01);
    EXPECT_NEAR(y.end_angle_deg, 186.20, 0.01);
    EXPECT_NEAR((y.end_angle_deg - y.start_angle_deg) / kIif.orbit_rate_deg_s, 999.5, 2.0);
}

TEST(NoonTurnYawLaw, TurnWindowsMatchTheRateLimitedFollower) {
    // Orbit angles (deg) between which the yaw saturates, from a time-stepped
    // simulation of a controller that tracks the nominal yaw at most R deg/s
    // (scripts/attitude_beta_check.py follower, dt 0.25 s).  It shares no code with
    // the closed form of noonTurnYaw, which finds the same two points by solving
    // for the rate crossing and for the ramp meeting the nominal curve.
    struct Window { double beta; bool iif; double start, end; };
    const Window table[] = {
        {  2.44, true,  177.841, 186.197 }, { -0.75, true,  178.355, 189.840 },
        { -0.75, false, 178.890, 184.604 }, {  1.00, true,  178.169, 189.200 },
        {  1.00, false, 178.819, 184.086 }, {  0.00, true,  180.000, 193.681 },
        {  0.00, false, 180.000, 187.526 }, {  4.00, true,  178.821, 182.481 },
        { -3.00, true,  177.987, 185.066 }, {  2.00, true,  177.831, 187.062 },
        {  2.00, false, 179.112, 181.983 },
    };
    for (const Window& w : table) {
        const auto y = SatAttitude::noonTurnYaw(w.beta * kDeg, kPi, w.iif ? kIif : kIir);
        ASSERT_TRUE(y.active) << "beta " << w.beta;
        EXPECT_NEAR(y.start_angle_deg, w.start, 0.01) << "beta " << w.beta << (w.iif ? " IIF" : " IIR");
        EXPECT_NEAR(y.end_angle_deg, w.end, 0.01) << "beta " << w.beta << (w.iif ? " IIF" : " IIR");
    }
}

// ===========================================================================
// The frame
// ===========================================================================

TEST(NoonTurnFrame, InsideTheTurnTheYawFollowsTheLawAndTheTriadStaysRightHanded) {
    for (double beta : { -2.0, -0.75, 1.0, 2.44, 3.9 }) {
        for (double mu : { 178.5, 180.0, 182.0, 184.0 }) {
            const Fix f = makeFix(beta, mu);
            const auto nominal = SatAttitude::compute(System::GPS, 25, f.pos, f.vel_ecef, f.sun, false, true);
            const auto y = SatAttitude::noonTurnYaw(beta * kDeg, mu * kDeg, kIif);
            const auto turned = SatAttitude::applyNoonTurn(nominal, kIif, f.pos, f.vel_ecef, f.sun);

            // The nominal frame sits at the Kouba yaw (the premise of the rotation).
            ASSERT_NEAR(wrapDeg(yawOf(nominal.i_s, f.pos, f.vel_ecef) - y.nominal_yaw_deg), 0.0, 1e-6);

            if (!y.active) {
                EXPECT_EQ(turned.mode, SatAttitude::Mode::YawSteering);
                continue;
            }
            EXPECT_EQ(turned.mode, SatAttitude::Mode::NoonTurn);
            // The turned X axis is at the law's actual yaw.
            EXPECT_NEAR(wrapDeg(yawOf(turned.i_s, f.pos, f.vel_ecef) - y.actual_yaw_deg), 0.0, 1e-6)
                << "beta " << beta << " mu " << mu;
            EXPECT_NEAR(turned.yaw_offset_deg, wrapDeg(y.actual_yaw_deg - y.nominal_yaw_deg), 1e-6);
            // The angle between the nominal and the turned X axes is the offset.
            EXPECT_NEAR(std::acos(std::clamp(turned.i_s.dot(nominal.i_s), -1.0, 1.0)) / kDeg,
                        std::abs(turned.yaw_offset_deg), 1e-6);
            // Orthonormal, right-handed, nadir unchanged.
            EXPECT_NEAR(turned.i_s.norm(), 1.0, 1e-12);
            EXPECT_NEAR(turned.j_s.norm(), 1.0, 1e-12);
            EXPECT_NEAR(turned.i_s.dot(turned.j_s), 0.0, 1e-12);
            EXPECT_NEAR(turned.i_s.cross(turned.j_s).dot(turned.k_s), 1.0, 1e-12);
            EXPECT_EQ(turned.k_s, nominal.k_s);
            EXPECT_NEAR(turned.beta, beta, 1e-6);
        }
    }
}

TEST(NoonTurnFrame, OutsideTheTurnTheAxesAreBitIdenticalToNominal) {
    // Away from the noon, above beta0, with the IIR law at a beta only the IIF turns
    // on, and for a frame that is not yaw-steered.
    struct Case { double beta, mu; SatAttitude::NoonTurnLaw law; };
    for (const Case& c : { Case{ 1.0, 100.0, kIif }, Case{ 1.0, 300.0, kIif }, Case{ 5.0, 180.0, kIif },
                           Case{ 3.0, 180.0, kIir }, Case{ -2.0, 120.0, kIir } }) {
        const Fix f = makeFix(c.beta, c.mu);
        const auto nominal = SatAttitude::compute(System::GPS, 25, f.pos, f.vel_ecef, f.sun, false, true);
        const auto out = SatAttitude::applyNoonTurn(nominal, c.law, f.pos, f.vel_ecef, f.sun);
        EXPECT_EQ(out.i_s, nominal.i_s) << c.beta << " " << c.mu;
        EXPECT_EQ(out.j_s, nominal.j_s);
        EXPECT_EQ(out.k_s, nominal.k_s);
        EXPECT_EQ(out.mode, SatAttitude::Mode::YawSteering);
        EXPECT_DOUBLE_EQ(out.yaw_offset_deg, 0.0);
    }
    const Fix f = makeFix(1.0, 180.0);
    auto on = SatAttitude::compute(System::BeiDou, 1, f.pos, f.vel_ecef, f.sun, false, true);
    ASSERT_EQ(on.mode, SatAttitude::Mode::OrbitNormalPositive);
    const auto kept = SatAttitude::applyNoonTurn(on, kIif, f.pos, f.vel_ecef, f.sun);
    EXPECT_EQ(kept.i_s, on.i_s);
    EXPECT_EQ(kept.mode, SatAttitude::Mode::OrbitNormalPositive);
}

TEST(NoonTurnFrame, ReferenceRowInsideAnIifTurnIsRotatedAndIirAgreesOnDirection) {
    // G25 on DOY 129 of 2024, 08:40 GPS time: beta -0.75 deg, orbit angle 179.8 deg,
    // inside the turn of both blocks.
    const RefRow& g25 = kRows[0];
    const Vector3d pos(g25.pos[0], g25.pos[1], g25.pos[2]);
    const Vector3d vel(g25.vel[0], g25.vel[1], g25.vel[2]);
    const Vector3d sun(g25.sun[0], g25.sun[1], g25.sun[2]);
    const auto nominal = SatAttitude::compute(System::GPS, 25, pos, vel, sun, false, false);
    const auto iif = SatAttitude::applyNoonTurn(nominal, kIif, pos, vel, sun);
    const auto iir = SatAttitude::applyNoonTurn(nominal, kIir, pos, vel, sun);
    ASSERT_EQ(iif.mode, SatAttitude::Mode::NoonTurn);
    ASSERT_EQ(iir.mode, SatAttitude::Mode::NoonTurn);
    // beta < 0: the nominal yaw is near +75 deg and runs toward +90 and +180; both
    // blocks lag it, IIF (slower) by more than IIR.
    EXPECT_LT(iif.yaw_offset_deg, 0.0);
    EXPECT_LT(iir.yaw_offset_deg, 0.0);
    EXPECT_LT(iif.yaw_offset_deg, iir.yaw_offset_deg);
    EXPECT_LT(std::abs(iif.yaw_offset_deg), 90.0);
}

// ===========================================================================
// The phase wind-up consumes the same frame
// ===========================================================================

namespace {

/// Wu et al. (1993) wind-up in metres for a given satellite X/Y pair, restated
/// independently of the production routine (receiver x = north, y = east).
double windUpReference(const Vector3d& rx, const Vector3d& sat, const Vector3d& xs, const Vector3d& ys,
                       double wavelength) {
    const Vector3d k = (rx - sat).normalized();
    const Coordinates::Llh llh = Coordinates::ecefToLlh(rx);
    const Vector3d xr(-std::sin(llh.lat_rad) * std::cos(llh.lon_rad),
                      -std::sin(llh.lat_rad) * std::sin(llh.lon_rad), std::cos(llh.lat_rad));
    const Vector3d yr(-std::sin(llh.lon_rad), std::cos(llh.lon_rad), 0.0);
    const Vector3d ds = xs - k * k.dot(xs) - k.cross(ys);
    const Vector3d dr = xr - k * k.dot(xr) - k.cross(yr);
    const Vector3d a = ds.normalized(), b = dr.normalized();
    return std::atan2(k.dot(a.cross(b)), a.dot(b)) * wavelength / (2.0 * kPi);
}

Vector3d receiverBelow(const Vector3d& sat_pos) {
    const Coordinates::Llh s = Coordinates::ecefToLlh(sat_pos);
    return Coordinates::llhToEcef(s.lat_rad - 15.0 * kDeg, s.lon_rad + 10.0 * kDeg, 100.0);
}

constexpr double kL1 = 0.1902936728;

}  // namespace

TEST(NoonTurnWindUp, NoLawIsBitIdenticalToTheDefaultAndToTheNominalFrame) {
    LogManager log_mgr;
    for (double mu : { 100.0, 180.0 }) {
        const Fix f = makeFix(1.0, mu);
        const Vector3d rx = receiverBelow(f.pos);
        const GnssTime t{ 2313, 100000.0 };
        GnssCorrections a(log_mgr), b(log_mgr);
        const double dflt = a.computePhaseWindUp("G25", System::GPS, rx, f.pos, kL1, t, f.sun, f.vel_ecef);
        const double none = b.computePhaseWindUp("G25", System::GPS, rx, f.pos, kL1, t, f.sun, f.vel_ecef,
                                                 false, 300.0, false, nullptr);
        EXPECT_EQ(dflt, none) << "mu " << mu;
    }
}

TEST(NoonTurnWindUp, MatchesTheRestatedModelOnTheNominalAndOnTheTurnedFrame) {
    LogManager log_mgr;
    for (double mu : { 100.0, 180.0, 183.0 }) {
        const double beta = 1.0;
        const Fix f = makeFix(beta, mu);
        const Vector3d rx = receiverBelow(f.pos);
        const GnssTime t{ 2313, 100000.0 };
        const Vector3d z_hat = -f.pos.normalized();

        // Reference frames from the Kouba yaw alone: x = cos(psi) v - sin(psi) n.
        auto frameAt = [&](double psi_deg) {
            const double p = psi_deg * kDeg;
            const Vector3d x = std::cos(p) * f.v_hat - std::sin(p) * f.n_hat;
            return std::pair<Vector3d, Vector3d>(x, z_hat.cross(x));
        };
        const auto y = SatAttitude::noonTurnYaw(beta * kDeg, mu * kDeg, kIif);
        const auto nom = frameAt(y.nominal_yaw_deg);
        const auto act = frameAt(y.actual_yaw_deg);

        GnssCorrections g_nominal(log_mgr), g_turn(log_mgr);
        const double got_nominal = g_nominal.computePhaseWindUp("G25", System::GPS, rx, f.pos, kL1, t, f.sun,
                                                                f.vel_ecef);
        const double got_turn = g_turn.computePhaseWindUp("G25", System::GPS, rx, f.pos, kL1, t, f.sun, f.vel_ecef,
                                                          false, 300.0, false, &kIif);
        EXPECT_NEAR(got_nominal, windUpReference(rx, f.pos, nom.first, nom.second, kL1), 1e-9) << "mu " << mu;
        EXPECT_NEAR(got_turn, windUpReference(rx, f.pos, act.first, act.second, kL1), 1e-9) << "mu " << mu;
        if (y.active) {
            // A turn lag of tens of degrees moves the wind-up by centimetres.
            EXPECT_GT(std::abs(got_turn - got_nominal), 0.005) << "mu " << mu;
        } else {
            EXPECT_EQ(got_turn, got_nominal) << "mu " << mu;
        }
    }
}

TEST(NoonTurnWindUp, GeoOrbitNormalAndVelocityFallbackIgnoreTheLaw) {
    // The law applies to a yaw-steered frame only: BeiDou-2 GEO keeps its
    // orbit-normal frame and a missing Sun keeps the velocity frame.
    LogManager log_mgr;
    const Fix f = makeFix(1.0, 180.0);
    const Vector3d rx = receiverBelow(f.pos);
    const GnssTime t{ 2313, 100000.0 };
    GnssCorrections a(log_mgr), b(log_mgr);
    EXPECT_EQ(a.computePhaseWindUp("C01", System::BeiDou, rx, f.pos, kL1, t, f.sun, f.vel_ecef),
              b.computePhaseWindUp("C01", System::BeiDou, rx, f.pos, kL1, t, f.sun, f.vel_ecef,
                                   false, 300.0, false, &kIif));
    GnssCorrections c(log_mgr), d(log_mgr);
    EXPECT_EQ(c.computePhaseWindUp("G25", System::GPS, rx, f.pos, kL1, t, Vector3d::Zero(), f.vel_ecef),
              d.computePhaseWindUp("G25", System::GPS, rx, f.pos, kL1, t, Vector3d::Zero(), f.vel_ecef,
                                   false, 300.0, false, &kIif));
}

// ===========================================================================
// The config knob
// ===========================================================================

namespace {

bool loadConfig(const std::string& tag, const std::string& processing_extra, GnssConfig& cfg) {
    const fs::path p = gnss_test::tempDir() / ("noonturn_cfg_" + tag + ".json");
    {
        std::ofstream f(p);
        f << "{\n  \"processing\": {\n"
             "    \"mode\": \"ppp\",\n"
             "    \"active_systems\": [ \"GPS\" ],\n"
          << processing_extra <<
             "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
             "  }\n}\n";
    }
    LogManager log;
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec;
    fs::remove(p, ec);
    return ok;
}

}  // namespace

TEST(NoonTurnConfig, DefaultsOffAndParsesFromTheProcessingSection) {
    GnssConfig absent;
    ASSERT_TRUE(loadConfig("absent", "", absent));
    EXPECT_FALSE(absent.enableNoonTurnAttitude());

    GnssConfig on;
    ASSERT_TRUE(loadConfig("on", "    \"enable_noon_turn_attitude\": true,\n", on));
    EXPECT_TRUE(on.enableNoonTurnAttitude());
    // The sibling orbit-normal knob is independent.
    EXPECT_FALSE(on.enableOrbitNormalAttitude());

    GnssConfig off;
    ASSERT_TRUE(loadConfig("off", "    \"enable_noon_turn_attitude\": false,\n", off));
    EXPECT_FALSE(off.enableNoonTurnAttitude());

    GnssConfig fresh;
    EXPECT_FALSE(fresh.enableNoonTurnAttitude());
}
