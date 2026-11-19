/**
 * @file test_corrections_windup_continuity.cpp
 * @brief Epoch-to-epoch behaviour of the phase wind-up: the fractional staircase,
 *        the accumulated unwrap, the restart and what follows it, and the census.
 *
 * The sign of the wind-up at one geometry, the one-cycle yaw turn and the
 * restart premise are covered by the sign tests, and the gap bridge and the arc
 * record by its own tests.  What those leave open, and what this file pins:
 *   - fractional mode (the loader default) over consecutive epochs: the value
 *     stays inside half a cycle and steps by one whole cycle where it crosses;
 *   - accumulated mode over the same epochs: no whole-cycle step anywhere, and
 *     at every epoch a whole number of cycles away from the fractional value;
 *   - a restart: the value after it is the raw angle, the step against the
 *     continued value is a whole cycle, the restart ledger names the gap and the
 *     epoch, and the epochs after it unwrap again from the new start;
 *   - the horizon edge (a gap equal to the horizon restarts), the horizon
 *     argument, and an out-of-order epoch (a restart with a negative gap);
 *   - a second call in one epoch (another signal) returns the same cycles;
 *   - the census counters, which no other test reads.
 *
 * The geometry is the one of the sign tests: a station at 40 N, 105 W, 1650 m;
 * a GPS satellite at elevation 45, azimuth 120 on the 26 560 km sphere; the Sun
 * held in the satellite's transverse plane and turned about its nadir axis at
 * 10 degrees per 30 s epoch.  A right-handed turn of the Sun about the nadir is
 * a yaw turn of the satellite, which retards the modelled wind-up by exactly one
 * cycle per turn (Wu et al. 1993), so between two epochs the true change is
 * exactly -(Sun angle change) / 360 cycles.  At Sun angle 0 the raw wind-up of
 * this geometry is -0.0827 cycle.
 *
 * Every case builds its own GnssCorrections: the accumulator and the census are
 * per instance and per satellite.
 */

#include <gtest/gtest.h>

#include "common/Constants.hpp"
#include "common/GnssTypes.hpp"
#include "corrections/GnssCorrections.hpp"
#include "logging/LogManager.hpp"
#include "math/Coordinates.hpp"
#include "math/TimeUtils.hpp"

#include <Eigen/Dense>
#include <cmath>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

using Eigen::Vector3d;

constexpr double kL1 = Constants::GPS_L1_WAVELENGTH;
constexpr double kL2 = Constants::GPS_L2_WAVELENGTH;
constexpr double kDeg = Constants::PI / 180.0;
constexpr double kAu = 1.495978707e11;
constexpr double kGpsRadius = 26560000.0;
constexpr int    kWeek = 2300;
constexpr double kTow0 = 100000.0;
constexpr double kEpoch = 30.0;                 // s between epochs
constexpr double kRateDegPerS = 10.0 / 30.0;    // Sun turn rate: 10 degrees per epoch
constexpr double kStepCyc = -10.0 / 360.0;      // true wind-up change per epoch (cycles)

/// The fixed station and satellite, with the Sun direction a function of time.
struct Scene {
    Vector3d rx_pos, sat_pos, a1, a2;

    Scene() {
        const double lat = 40.0 * kDeg, lon = -105.0 * kDeg;
        rx_pos = Coordinates::llhToEcef(lat, lon, 1650.0);
        const Vector3d north(-std::sin(lat) * std::cos(lon), -std::sin(lat) * std::sin(lon), std::cos(lat));
        const Vector3d east(-std::sin(lon), std::cos(lon), 0.0);
        const Vector3d up(std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat));
        const double el = 45.0 * kDeg, az = 120.0 * kDeg;
        const Vector3d los = std::cos(el) * (std::cos(az) * north + std::sin(az) * east) + std::sin(el) * up;
        const double b = 2.0 * rx_pos.dot(los);
        const double c = rx_pos.squaredNorm() - kGpsRadius * kGpsRadius;
        sat_pos = rx_pos + 0.5 * (-b + std::sqrt(b * b - 4.0 * c)) * los;
        // Right-handed basis (a1, a2, nadir) of the satellite's transverse plane.
        const Vector3d ez = -sat_pos.normalized();
        a1 = ez.cross(rx_pos).normalized();
        a2 = ez.cross(a1);
    }

    /// Sun position at @p dt_s seconds after the first epoch.
    Vector3d sunAt(double dt_s) const {
        const double th = kRateDegPerS * dt_s * kDeg;
        return sat_pos + kAu * (std::cos(th) * a1 + std::sin(th) * a2);
    }

    /// The wind-up (metres) the code returns for @p sat at @p dt_s.
    double call(GnssCorrections& gc, const std::string& sat, double dt_s, bool accumulate,
                double max_gap_s = 300.0, double wavelength = kL1) const {
        return gc.computePhaseWindUp(sat, System::GPS, rx_pos, sat_pos, wavelength,
                                     GnssTime{ kWeek, kTow0 + dt_s }, sunAt(dt_s), Vector3d::Zero(),
                                     accumulate, max_gap_s);
    }

    /// The raw (fractional) wind-up in cycles at @p dt_s, from a fresh instance.
    double rawCycles(double dt_s) const {
        LogManager log;
        GnssCorrections fresh(log);
        return call(fresh, "G30", dt_s, /*accumulate=*/false) / kL1;
    }
};

double continuousSeconds(double dt_s) { return kWeek * 604800.0 + kTow0 + dt_s; }

bool isWhole(double cycles, double tol = 1e-9) { return std::abs(cycles - std::round(cycles)) < tol; }

}  // namespace

// ---------------------------------------------------------------------------
// Fractional mode over two Sun turns (72 epochs of -1/36 cycle).  Every value
// stays within half a cycle; every step is either the true -1/36 cycle or that
// plus one whole cycle; the whole-cycle steps happen exactly twice (the value
// starts at -0.083 and falls two cycles).  The census counts 72 forward steps,
// 2 wraps and no restart.  Fails if fractional mode starts unwrapping (no
// wraps), if a step other than one cycle appears, or if the census buckets
// change.
// ---------------------------------------------------------------------------
TEST(CorrectionsWindupContinuity, FractionalModeStepsOneWholeCycleAtEachCrossing) {
    const Scene sc;
    LogManager log;
    GnssCorrections gc(log);
    gc.enableWindUpCensus(300.0, 0.0);

    constexpr int kSteps = 72;
    double prev = 0.0;
    int whole_steps = 0;
    for (int i = 0; i <= kSteps; ++i) {
        const double v = sc.call(gc, "G21", kEpoch * i, /*accumulate=*/false);
        EXPECT_LE(std::abs(v), 0.5 * kL1 + 1e-12) << "epoch " << i;
        if (i > 0) {
            const double step = (v - prev) / kL1;
            if (std::abs(step - kStepCyc) < 1e-9) {
                // the true change
            } else if (std::abs(step - (kStepCyc + 1.0)) < 1e-9) {
                ++whole_steps;
            } else {
                ADD_FAILURE() << "epoch " << i << ": step " << step << " cycle is neither -1/36 nor 1 - 1/36";
            }
        } else {
            EXPECT_NEAR(v / kL1, -0.082688, 1e-5) << "premise: the raw angle of this geometry at Sun angle 0";
        }
        prev = v;
    }
    EXPECT_EQ(whole_steps, 2);

    const auto& census = gc.windUpCensus();
    EXPECT_EQ(census.steps, kSteps);
    EXPECT_EQ(census.gap_lt_wu, kSteps);
    EXPECT_EQ(census.wraps, 2);
    EXPECT_EQ(census.restarts, 0);
    EXPECT_EQ(census.backwards, 0);
    EXPECT_EQ(census.gap_steps, 0);
    EXPECT_NEAR(census.max_abs_step_in_cyc, 1.0 + kStepCyc, 1e-9);
    EXPECT_NEAR(census.sum_abs_step_cyc, 70.0 / 36.0 + 2.0 * (1.0 + kStepCyc), 1e-8);

    // Fractional mode keeps no accumulator, so neither ledger nor arcs exist,
    // even across a gap far beyond the horizon.
    sc.call(gc, "G21", kEpoch * kSteps + 900.0, false);
    EXPECT_TRUE(gc.wuRestarts().empty());
    EXPECT_TRUE(gc.windUpArcs().empty());
}

// ---------------------------------------------------------------------------
// Accumulated mode over the same two turns: every step is exactly -1/36 cycle,
// the total is -2 cycles, the value at each epoch is a whole number of cycles
// from the fractional value, and the census sees no wrap and no restart.
// Fails if the unwrap is removed or confined to the fractional range, or if the
// accumulated and fractional values drift apart by a non-integer amount.
// ---------------------------------------------------------------------------
TEST(CorrectionsWindupContinuity, AccumulatedModeHasNoWholeCycleStep) {
    const Scene sc;
    LogManager log_a, log_f;
    GnssCorrections acc(log_a), frac(log_f);
    acc.enableWindUpCensus(300.0, 0.0);

    constexpr int kSteps = 72;
    double first = 0.0, prev = 0.0, last = 0.0;
    for (int i = 0; i <= kSteps; ++i) {
        const double a = sc.call(acc, "G22", kEpoch * i, true);
        const double f = sc.call(frac, "G22", kEpoch * i, false);
        EXPECT_TRUE(isWhole((a - f) / kL1)) << "epoch " << i << ": accumulated - fractional = " << (a - f) / kL1;
        if (i == 0) {
            first = a;
            EXPECT_DOUBLE_EQ(a, f) << "the first epoch seeds the accumulator with the raw angle";
        } else {
            EXPECT_NEAR((a - prev) / kL1, kStepCyc, 1e-9) << "epoch " << i;
        }
        prev = a;
        last = a;
    }
    EXPECT_NEAR((last - first) / kL1, -2.0, 1e-8);

    const auto& census = acc.windUpCensus();
    EXPECT_EQ(census.steps, kSteps);
    EXPECT_EQ(census.wraps, 0);
    EXPECT_EQ(census.restarts, 0);
    EXPECT_NEAR(census.max_abs_step_in_cyc, -kStepCyc, 1e-9);
    EXPECT_TRUE(acc.wuRestarts().empty());

    const auto arcs = acc.windUpArcs();
    ASSERT_EQ(arcs.size(), 1u);
    EXPECT_DOUBLE_EQ(arcs[0].second.start_epoch_s, continuousSeconds(0.0));
    EXPECT_NEAR(arcs[0].second.cycles, last / kL1, 1e-12);
}

// ---------------------------------------------------------------------------
// A 600 s gap (beyond the 300 s horizon) after 20 epochs.  At the restart epoch
// the Sun has turned 400 degrees since the first epoch, so the continued value
// would be -0.0827 - 400/360 = -1.194 cycles; the restart returns the raw
// angle, -0.194 cycle, exactly one cycle above it.  The ledger records the
// 600 s gap and the restart epoch, the census one restart, the arc starts at
// the restart, and the next epochs unwrap from there (-1/36 cycle each, no
// further step).  Clearing the ledger empties it and the next restart refills
// it.  Fails if the restart value is not the raw angle, if the ledger entry is
// missing or wrong, or if the epochs after a restart do not continue from it.
// ---------------------------------------------------------------------------
TEST(CorrectionsWindupContinuity, RestartTakesTheRawAngleAndThenUnwrapsAgain) {
    const Scene sc;
    LogManager log;
    GnssCorrections gc(log);
    gc.enableWindUpCensus(300.0, 0.0);

    double seed = 0.0, before_gap = 0.0;
    for (int i = 0; i <= 20; ++i) {
        const double v = sc.call(gc, "G23", kEpoch * i, true);
        if (i == 0) seed = v / kL1;
        before_gap = v;
    }
    EXPECT_NEAR(before_gap / kL1, seed + 20.0 * kStepCyc, 1e-9);

    const double t_restart = kEpoch * 20 + 600.0;
    const double restart = sc.call(gc, "G23", t_restart, true) / kL1;
    const double continued = seed - (kRateDegPerS * t_restart) / 360.0;   // -1.194 cycles
    EXPECT_NEAR(restart, sc.rawCycles(t_restart), 1e-12);
    EXPECT_LE(std::abs(restart), 0.5 + 1e-12);
    EXPECT_NEAR(restart - continued, 1.0, 1e-9) << "the restart steps the model by exactly one cycle here";

    ASSERT_EQ(gc.wuRestarts().count("G23"), 1u);
    EXPECT_DOUBLE_EQ(gc.wuRestarts().at("G23").gap_s, 600.0);
    EXPECT_DOUBLE_EQ(gc.wuRestarts().at("G23").epoch_s, continuousSeconds(t_restart));
    EXPECT_EQ(gc.windUpCensus().restarts, 1);
    ASSERT_EQ(gc.windUpArcs().size(), 1u);
    EXPECT_DOUBLE_EQ(gc.windUpArcs()[0].second.start_epoch_s, continuousSeconds(t_restart));

    double prev = restart;
    for (int j = 1; j <= 6; ++j) {
        const double v = sc.call(gc, "G23", t_restart + kEpoch * j, true) / kL1;
        EXPECT_NEAR(v - prev, kStepCyc, 1e-9) << "epoch " << j << " after the restart";
        prev = v;
    }
    EXPECT_EQ(gc.windUpCensus().restarts, 1) << "no further restart while the epochs stay inside the horizon";
    EXPECT_DOUBLE_EQ(gc.windUpArcs()[0].second.start_epoch_s, continuousSeconds(t_restart));

    gc.clearWuRestarts();
    EXPECT_TRUE(gc.wuRestarts().empty());
    sc.call(gc, "G23", t_restart + kEpoch * 6 + 1000.0, true);
    ASSERT_EQ(gc.wuRestarts().count("G23"), 1u);
    EXPECT_DOUBLE_EQ(gc.wuRestarts().at("G23").gap_s, 1000.0);
    EXPECT_EQ(gc.windUpCensus().restarts, 2);
}

// ---------------------------------------------------------------------------
// The horizon is exclusive: a gap of 299.5 s continues (the value moves by the
// true -299.5/30 x 10/360 = -0.2773 cycle), a gap of exactly 300 s restarts.
// The horizon is the caller's argument: with 120 s, a 150 s gap restarts.
// Fails if the comparison becomes "<=" or the argument is ignored.
// ---------------------------------------------------------------------------
TEST(CorrectionsWindupContinuity, GapEqualToTheHorizonRestartsAndTheHorizonIsTheArgument) {
    const Scene sc;
    LogManager log;
    GnssCorrections gc(log);

    // Walk two satellites past the half-cycle so a restart is visible as a step.
    for (const char* sat : { "G24", "G25" })
        for (int i = 0; i <= 20; ++i) sc.call(gc, sat, kEpoch * i, true);
    const double t_last = kEpoch * 20;
    const double at_last = sc.call(gc, "G24", t_last, true) / kL1;   // same epoch: reused value

    const double cont = sc.call(gc, "G24", t_last + 299.5, true) / kL1;
    EXPECT_NEAR(cont - at_last, -(kRateDegPerS * 299.5) / 360.0, 1e-9);
    EXPECT_EQ(gc.wuRestarts().count("G24"), 0u);

    const double edge = sc.call(gc, "G25", t_last + 300.0, true) / kL1;
    EXPECT_NEAR(edge, sc.rawCycles(t_last + 300.0), 1e-12);
    ASSERT_EQ(gc.wuRestarts().count("G25"), 1u);
    EXPECT_DOUBLE_EQ(gc.wuRestarts().at("G25").gap_s, 300.0);

    // A caller horizon of 120 s.
    LogManager log2;
    GnssCorrections short_horizon(log2);
    for (int i = 0; i <= 20; ++i) sc.call(short_horizon, "G26", kEpoch * i, true, 120.0);
    sc.call(short_horizon, "G26", t_last + 150.0, true, 120.0);
    ASSERT_EQ(short_horizon.wuRestarts().count("G26"), 1u);
    EXPECT_DOUBLE_EQ(short_horizon.wuRestarts().at("G26").gap_s, 150.0);
    // The same gap under the default horizon continues.
    LogManager log3;
    GnssCorrections default_horizon(log3);
    for (int i = 0; i <= 20; ++i) sc.call(default_horizon, "G26", kEpoch * i, true);
    sc.call(default_horizon, "G26", t_last + 150.0, true);
    EXPECT_TRUE(default_horizon.wuRestarts().empty());
}

// ---------------------------------------------------------------------------
// An epoch older than the last one processed restarts the accumulator from the
// raw angle and records a negative gap; the census counts it as backwards and
// as a restart.  The next epoch, 30 s after the out-of-order one, unwraps from
// the restarted value.  Fails if an out-of-order epoch is unwrapped against a
// later reference, or if the ledger loses the sign of the gap.
// ---------------------------------------------------------------------------
TEST(CorrectionsWindupContinuity, OutOfOrderEpochRestartsWithANegativeGap) {
    const Scene sc;
    LogManager log;
    GnssCorrections gc(log);
    gc.enableWindUpCensus(300.0, 0.0);

    for (int i = 0; i <= 20; ++i) sc.call(gc, "G27", kEpoch * i, true);
    const double t_back = kEpoch * 19;
    const double back = sc.call(gc, "G27", t_back, true) / kL1;
    EXPECT_NEAR(back, sc.rawCycles(t_back), 1e-12);

    ASSERT_EQ(gc.wuRestarts().count("G27"), 1u);
    EXPECT_DOUBLE_EQ(gc.wuRestarts().at("G27").gap_s, -kEpoch);
    EXPECT_DOUBLE_EQ(gc.wuRestarts().at("G27").epoch_s, continuousSeconds(t_back));
    EXPECT_EQ(gc.windUpCensus().backwards, 1);
    EXPECT_EQ(gc.windUpCensus().restarts, 1);
    EXPECT_EQ(gc.windUpCensus().steps, 20) << "the backwards transition is not a forward step";

    const double next = sc.call(gc, "G27", t_back + kEpoch, true) / kL1;
    EXPECT_NEAR(next - back, kStepCyc, 1e-9);
}

// ---------------------------------------------------------------------------
// A second call in the same epoch (another signal, or another pass over the
// epoch) returns the accumulated angle of the first call, scaled by its own
// wavelength: the cycles agree across L1 and L2.  The reuse takes the angle,
// not the geometry: a second call given another Sun direction still returns the
// first call's angle.  Fails if a same-epoch call overwrites the accumulator
// with the raw angle (accumulated mode would then silently revert to
// fractional) or recomputes the geometry.
// ---------------------------------------------------------------------------
TEST(CorrectionsWindupContinuity, SecondCallInAnEpochReturnsTheSameCycles) {
    const Scene sc;
    LogManager log;
    GnssCorrections gc(log);

    double l1 = 0.0;
    for (int i = 0; i <= 24; ++i) l1 = sc.call(gc, "G28", kEpoch * i, true);
    ASSERT_GT(std::abs(l1 / kL1), 0.5) << "premise: the accumulated value is outside the fractional range";

    const double t = kEpoch * 24;
    const double l2 = sc.call(gc, "G28", t, true, 300.0, kL2);
    EXPECT_NEAR(l2 / kL2, l1 / kL1, 1e-12);

    const double other_sun = gc.computePhaseWindUp("G28", System::GPS, sc.rx_pos, sc.sat_pos, kL1,
                                                   GnssTime{ kWeek, kTow0 + t }, sc.sunAt(t + 2700.0),
                                                   Vector3d::Zero(), true);
    EXPECT_DOUBLE_EQ(other_sun, l1);

    // The epoch after continues from the reused value, not from a raw angle.
    const double after = sc.call(gc, "G28", t + kEpoch, true);
    EXPECT_NEAR((after - l1) / kL1, kStepCyc, 1e-9);
}

}  // namespace GnssProcessor
