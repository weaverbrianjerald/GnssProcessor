/**
 * @file test_windup_gap_bridge.cpp
 * @brief Unit gate for the phase wind-up gap bridge
 *        (@c estimation.phase_windup_gap_bridge).
 *
 * The bridge carries the accumulated wind-up angle across a data gap of 300 s or
 * more, inside the leveling-carry horizon, by walking it through virtual epochs
 * instead of restarting the accumulator from the raw angle.
 *
 * Four suites:
 *  - @c WindUpBridgeWalk: the arithmetic of WindUpBridge::walk against analytic
 *    angle functions (whole cycles, step count and spacing, the half-cycle
 *    refusal, the umbra and noon-turn refusals, the closure with the epoch's own
 *    angle, the counters).
 *  - @c WindUpBridgeCorrections: GnssCorrections::computePhaseWindUp with a
 *    synthetic satellite-state provider.  The reference is the accumulator fed
 *    every epoch (no gap): a bridged gap must reproduce it, and the unbridged
 *    restart must differ from it by whole cycles.
 *  - @c NoonTurnWindowPredicate: SatAttitude::inNoonTurnWindow.
 *  - @c WindUpBridgeConfig: the three keys, their defaults and their interlock
 *    with accumulated wind-up.
 *
 * The simulator injects its wind-up through the same computePhaseWindUp, so no
 * simulated data can grade this option: what is checked here is the arithmetic,
 * not the physics of a real gap.
 */

#include <gtest/gtest.h>

#include "common/Constants.hpp"
#include "common/GnssTypes.hpp"
#include "config/GnssConfig.hpp"
#include "corrections/GnssCorrections.hpp"
#include "corrections/WindUpGapBridge.hpp"
#include "logging/LogManager.hpp"
#include "math/Coordinates.hpp"
#include "model/SatAttitude.hpp"
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

#include <Eigen/Dense>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace GnssProcessor {
namespace {

using Eigen::Vector3d;
namespace WB = WindUpBridge;

constexpr double kTwoPi = 2.0 * Constants::PI;
constexpr double kDeg = Constants::PI / 180.0;

double wrapPi(double x) { return x - kTwoPi * std::round(x / kTwoPi); }

/// Sampler from an analytic angle function; every flag false.
WB::Sampler analytic(double (*f)(double)) {
    return [f](double t) {
        WB::Sample s;
        s.ok = true;
        s.angle_rad = wrapPi(f(t));
        return s;
    };
}

constexpr double kT0 = 1.0e6;   // an arbitrary continuous-GPS-second origin

}  // namespace

// ---------------------------------------------------------------------------
// WindUpBridgeWalk
// ---------------------------------------------------------------------------

TEST(WindUpBridgeWalk, KeepsWholeCyclesAndTakesTheFractionFromTheData) {
    // 0.0019 cycle/s: 1.14 cycles over 600 s, 0.057 cycle per 30 s step.
    auto angle = [](double t) { return kTwoPi * (0.3 + 0.0019 * (t - kT0)); };
    const double a_prev = wrapPi(angle(kT0)) + 7.0 * kTwoPi;        // accumulated, seven turns up
    const double raw_now = wrapPi(angle(kT0 + 600.0));              // 1.44 cycles, wrapped to 0.44

    WB::Options opt;
    const auto res = WB::walk(kT0, kT0 + 600.0, a_prev, raw_now, opt, [&](double t) {
        WB::Sample s; s.ok = true; s.angle_rad = wrapPi(angle(t)); return s; });

    ASSERT_EQ(res.outcome, WB::Outcome::Bridged);
    // The walked angle is the accumulated one plus the true change.
    EXPECT_NEAR(res.angle_rad, a_prev + kTwoPi * 0.0019 * 600.0, 1e-9);
    // The fraction is the epoch's own: the result differs from the raw angle by whole cycles.
    EXPECT_NEAR(res.angle_rad - raw_now, kTwoPi * static_cast<double>(res.cycles), 1e-12);
    EXPECT_EQ(res.cycles, 8);   // 7.3 + 1.14 = 8.44 cycles against a raw 0.44
}

TEST(WindUpBridgeWalk, StepsAreEqualNoLongerThanStepAndCoverTheGap) {
    std::vector<double> times;
    WB::Options opt;
    opt.step_s = 30.0;
    const auto res = WB::walk(kT0, kT0 + 450.0, 0.0, 0.0, opt, [&](double t) {
        times.push_back(t);
        WB::Sample s; s.ok = true; s.angle_rad = 0.0; return s; });
    ASSERT_EQ(res.outcome, WB::Outcome::Bridged);
    ASSERT_EQ(res.cycles, 0);
    // 450 s in steps of at most 30 s: 15 steps, each sampled at its middle and end, plus the start.
    EXPECT_EQ(times.size(), 1u + 2u * 15u);
    EXPECT_EQ(res.samples, 31);
    EXPECT_DOUBLE_EQ(times.front(), kT0);
    EXPECT_DOUBLE_EQ(times.back(), kT0 + 450.0);

    // A gap that is not a multiple of the step: equal steps, none above the limit.
    times.clear();
    WB::walk(kT0, kT0 + 500.0, 0.0, 0.0, opt, [&](double t) {
        times.push_back(t);
        WB::Sample s; s.ok = true; return s; });
    ASSERT_EQ(times.size(), 1u + 2u * 17u);   // ceil(500 / 30) = 17 steps
    // The samples go start, middle, end, middle, end, ...: the step ends are every second one.
    for (std::size_t i = 0; i + 2 < times.size(); i += 2)
        EXPECT_LE(times[i + 2] - times[i], 30.0 + 1e-9);
    EXPECT_DOUBLE_EQ(times.back(), kT0 + 500.0);
}

TEST(WindUpBridgeWalk, RefusesAChangeOfHalfACyclePerStep) {
    WB::Options opt;
    opt.step_s = 30.0;
    // 0.02 cycle/s is 0.6 cycle per step: the wrapped step alone would read -0.4 cycle, its alias.
    const double raw_fast = wrapPi(kTwoPi * 0.02 * 300.0);
    EXPECT_EQ(WB::walk(kT0, kT0 + 300.0, 0.0, raw_fast, opt,
                       analytic(+[](double t) { return kTwoPi * 0.02 * (t - kT0); })).outcome,
              WB::Outcome::StepTooLarge);
    // 0.015 cycle/s is 0.45 cycle per step: within what the unwrap resolves, and carried.
    const double raw_slow = wrapPi(kTwoPi * 0.015 * 300.0);
    EXPECT_EQ(WB::walk(kT0, kT0 + 300.0, 0.0, raw_slow, opt,
                       analytic(+[](double t) { return kTwoPi * 0.015 * (t - kT0); })).outcome,
              WB::Outcome::Bridged);
}

TEST(WindUpBridgeWalk, ALargeHalfStepIsNotHiddenByTheNextOne) {
    // The angle is 0 at the start, 0.55 cycle at the middle of the first step and 0.60 at its
    // end.  The first half step wraps to -0.45 cycle and the second adds 0.05: their sum, -0.40,
    // is inside half a cycle, and only the limit on each half step shows the alias.
    WB::Options opt;
    opt.step_s = 30.0;
    auto sampler = [](double t) {
        WB::Sample s; s.ok = true;
        if (t < kT0 + 1.0)                         s.angle_rad = 0.0;
        else if (t > kT0 + 14.0 && t < kT0 + 16.0) s.angle_rad = 0.55 * kTwoPi;
        else                                       s.angle_rad = 0.60 * kTwoPi;
        return s;
    };
    EXPECT_EQ(WB::walk(kT0, kT0 + 300.0, 0.0, 0.60 * kTwoPi, opt, sampler).outcome,
              WB::Outcome::StepTooLarge);
}

TEST(WindUpBridgeWalk, ASecondsValueJustAboveAWholeNumberOfStepsDoesNotAddAStep) {
    // Continuous GPS seconds near 1.4e9 carry about 2e-7 s of noise; 630 s in 30 s steps is 21
    // steps, and a gap of 630.0000002 s must not be cut into 22.
    WB::Options opt;
    opt.step_s = 30.0;
    const auto res = WB::walk(kT0, kT0 + 630.0000002, 0.0, 0.0, opt, [](double) {
        WB::Sample s; s.ok = true; return s; });
    ASSERT_EQ(res.outcome, WB::Outcome::Bridged);
    EXPECT_EQ(res.samples, 1 + 2 * 21);
}

TEST(WindUpBridgeWalk, ARefusingSampleEndsTheWalkWithItsReason) {
    WB::Options opt;
    auto make = [&](int which) {
        return [which](double t) {
            WB::Sample s; s.ok = true; s.angle_rad = 0.0;
            if (t > kT0 + 299.0 && t < kT0 + 301.0) {   // one sample in the middle of the gap
                if (which == 0) s.ok = false;
                if (which == 1) s.umbra = true;
                if (which == 2) s.noon_turn = true;
            }
            return s;
        };
    };
    EXPECT_EQ(WB::walk(kT0, kT0 + 600.0, 0.0, 0.0, opt, make(0)).outcome, WB::Outcome::NoState);
    EXPECT_EQ(WB::walk(kT0, kT0 + 600.0, 0.0, 0.0, opt, make(1)).outcome, WB::Outcome::Umbra);
    EXPECT_EQ(WB::walk(kT0, kT0 + 600.0, 0.0, 0.0, opt, make(2)).outcome, WB::Outcome::NoonTurn);
    EXPECT_EQ(WB::walk(kT0, kT0 + 600.0, 0.0, 0.0, opt, make(3)).outcome, WB::Outcome::Bridged);

    // The end points are sampled too: a hazard at the current epoch refuses the walk.
    auto at_end = [](double t) {
        WB::Sample s; s.ok = true; s.umbra = (t >= kT0 + 600.0 - 1e-9); return s; };
    EXPECT_EQ(WB::walk(kT0, kT0 + 600.0, 0.0, 0.0, opt, at_end).outcome, WB::Outcome::Umbra);
}

TEST(WindUpBridgeWalk, RefusesAWalkThatDisagreesWithTheEpochsOwnAngle) {
    WB::Options opt;
    auto flat = [](double) { WB::Sample s; s.ok = true; s.angle_rad = 0.0; return s; };
    // The walk ends at 0; the epoch's raw angle is 0.3 cycle away: the integer is not trusted.
    EXPECT_EQ(WB::walk(kT0, kT0 + 600.0, 0.0, 0.3 * kTwoPi, opt, flat).outcome, WB::Outcome::Closure);
    // 0.2 cycle away is inside the quarter-cycle tolerance, and the data's fraction wins.
    const auto ok = WB::walk(kT0, kT0 + 600.0, 0.0, 0.2 * kTwoPi, opt, flat);
    ASSERT_EQ(ok.outcome, WB::Outcome::Bridged);
    EXPECT_EQ(ok.cycles, 0);
    EXPECT_DOUBLE_EQ(ok.angle_rad, 0.2 * kTwoPi);
}

TEST(WindUpBridgeWalk, DegenerateRequestsAreNotBridged) {
    WB::Options opt;
    auto flat = [](double) { WB::Sample s; s.ok = true; return s; };
    EXPECT_EQ(WB::walk(kT0, kT0, 0.0, 0.0, opt, flat).outcome, WB::Outcome::NoState);          // no gap
    EXPECT_EQ(WB::walk(kT0 + 10.0, kT0, 0.0, 0.0, opt, flat).outcome, WB::Outcome::NoState);   // backwards
    EXPECT_EQ(WB::walk(kT0, kT0 + 600.0, 0.0, 0.0, opt, WB::Sampler{}).outcome, WB::Outcome::NoState);
}

TEST(WindUpBridgeWalk, CountersRecordEveryOutcomeAndAdd) {
    WB::Counters c;
    WB::Result r;
    r.samples = 10;
    r.outcome = WB::Outcome::Bridged; r.cycles = 0;  c.record(r);
    r.outcome = WB::Outcome::Bridged; r.cycles = -2; c.record(r);
    r.outcome = WB::Outcome::NoState;                c.record(r);
    r.outcome = WB::Outcome::Umbra;                  c.record(r);
    r.outcome = WB::Outcome::NoonTurn;               c.record(r);
    r.outcome = WB::Outcome::StepTooLarge;           c.record(r);
    r.outcome = WB::Outcome::Closure;                c.record(r);
    EXPECT_EQ(c.attempts, 7);
    EXPECT_EQ(c.bridged, 2);
    EXPECT_EQ(c.bridged_stepped, 1);
    EXPECT_EQ(c.refused(), 5);
    EXPECT_EQ(c.samples, 70);

    WB::Counters sum;
    sum.beyond_horizon = 3;
    sum.add(c);
    sum.add(c);
    EXPECT_EQ(sum.attempts, 14);
    EXPECT_EQ(sum.bridged_stepped, 2);
    EXPECT_EQ(sum.beyond_horizon, 3);
    EXPECT_EQ(sum.refused(), 10);
}

// ---------------------------------------------------------------------------
// WindUpBridgeCorrections
// ---------------------------------------------------------------------------

namespace {

constexpr double kLambda = Constants::GPS_L1_WAVELENGTH;
constexpr double kAu = 1.495978707e11;
constexpr double kGpsRadius = 26560000.0;
constexpr int kWeek = 2300;
constexpr double kTow0 = 100000.0;

/// One station and one satellite whose Sun direction turns about the satellite's
/// nadir axis at a constant rate.  With the Sun in the satellite's transverse
/// plane the yaw-steering X axis is the Sun direction, so the wind-up retreats
/// by exactly one cycle per turn of the Sun (see test_phase_windup_sign.cpp).
struct Scene {
    Vector3d rx_pos;
    Vector3d sat_pos;
    Vector3d a1, a2;
    double rate_deg_s{0.5};
    double theta0_deg{0.0};

    explicit Scene(double theta0 = 0.0, double rate = 0.5) : rate_deg_s(rate), theta0_deg(theta0) {
        const double lat = 40.0 * kDeg, lon = -105.0 * kDeg;
        rx_pos = Coordinates::llhToEcef(lat, lon, 1650.0);
        // A satellite 45 degrees up on the GPS-radius sphere, toward the south-east.
        const Vector3d north(-std::sin(lat) * std::cos(lon), -std::sin(lat) * std::sin(lon), std::cos(lat));
        const Vector3d east(-std::sin(lon), std::cos(lon), 0.0);
        const Vector3d up(std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat));
        const double el = 45.0 * kDeg, az = 120.0 * kDeg;
        const Vector3d los = std::cos(el) * (std::cos(az) * north + std::sin(az) * east) + std::sin(el) * up;
        const double b = 2.0 * rx_pos.dot(los);
        const double c = rx_pos.squaredNorm() - kGpsRadius * kGpsRadius;
        sat_pos = rx_pos + 0.5 * (-b + std::sqrt(b * b - 4.0 * c)) * los;
        const Vector3d ez = -sat_pos.normalized();
        a1 = ez.cross(rx_pos).normalized();
        a2 = ez.cross(a1);
    }

    /// Sun position at tow offset @p dt_s from the first epoch.
    Vector3d sunAt(double dt_s) const {
        const double th = (theta0_deg + rate_deg_s * dt_s) * kDeg;
        return sat_pos + kAu * (std::cos(th) * a1 + std::sin(th) * a2);
    }

    /// The provider the bridge reads: the same fixed satellite, the Sun at that time.
    GnssCorrections::SatStateProvider provider() const {
        return [this](const std::string&, const GnssTime& t, const Vector3d&,
                      Vector3d& pos, Vector3d& vel, Vector3d& sun) {
            pos = sat_pos;
            vel = Vector3d::Zero();
            sun = sunAt((t.week - kWeek) * 604800.0 + t.tow - kTow0);
            return true;
        };
    }

    double call(GnssCorrections& gc, double dt_s, const WB::Options* bridge) const {
        return gc.computePhaseWindUp("E11", System::Galileo, rx_pos, sat_pos, kLambda,
                                     GnssTime{ kWeek, kTow0 + dt_s }, sunAt(dt_s), Vector3d::Zero(),
                                     /*accumulate=*/true, /*max_gap_s=*/300.0,
                                     /*correct_velocity_frame=*/false, /*noon_turn_law=*/nullptr, bridge);
    }
};

/// Feed epochs every 30 s from @p from to @p to inclusive.
double feed(const Scene& sc, GnssCorrections& gc, double from, double to, const WB::Options* bridge) {
    double last = 0.0;
    for (double dt = from; dt <= to + 1e-9; dt += 30.0) last = sc.call(gc, dt, bridge);
    return last;
}

}  // namespace

TEST(WindUpBridgeCorrections, BridgedGapReproducesTheContinuousAccumulator) {
    int offsets_with_a_step = 0;
    for (const double theta0 : { 0.0, 50.0, 100.0, 150.0, 200.0, 250.0, 300.0 }) {
        const Scene sc(theta0);
        WB::Options opt;

        // Reference: every epoch, no gap.
        LogManager log_a;
        GnssCorrections ref(log_a);
        const double want = feed(sc, ref, 0.0, 900.0, nullptr);

        // 600 s without data after the epoch at 300 s, bridged.
        LogManager log_b;
        GnssCorrections bridged(log_b);
        bridged.setSatStateProvider(sc.provider());
        feed(sc, bridged, 0.0, 300.0, &opt);
        const double got = sc.call(bridged, 900.0, &opt);
        EXPECT_NEAR(got, want, 1e-9) << "theta0 " << theta0;
        EXPECT_TRUE(bridged.wuRestarts().empty()) << "a bridged gap leaves no restart in the ledger";
        EXPECT_EQ(bridged.windUpBridgeCounters().attempts, 1);
        EXPECT_EQ(bridged.windUpBridgeCounters().bridged, 1);
        // The attempt is named: satellite, the epoch that closed the gap, the gap, the outcome.
        ASSERT_EQ(bridged.windUpBridgeEvents().size(), 1u);
        const auto& ev = bridged.windUpBridgeEvents().front();
        EXPECT_EQ(ev.satellite, "E11");
        EXPECT_DOUBLE_EQ(ev.t_now_s, kWeek * 604800.0 + kTow0 + 900.0);
        EXPECT_DOUBLE_EQ(ev.gap_s, 600.0);
        EXPECT_EQ(ev.outcome, WB::Outcome::Bridged);

        // The unbridged run restarts: the same value up to a whole number of cycles.
        LogManager log_c;
        GnssCorrections plain(log_c);
        feed(sc, plain, 0.0, 300.0, nullptr);
        const double restart = sc.call(plain, 900.0, nullptr);
        const double cycles = (want - restart) / kLambda;
        EXPECT_NEAR(cycles, std::round(cycles), 1e-9) << "theta0 " << theta0;
        EXPECT_EQ(plain.wuRestarts().size(), 1u);
        EXPECT_EQ(plain.windUpBridgeCounters().attempts, 0);
        if (std::abs(std::round(cycles)) >= 1.0) {
            ++offsets_with_a_step;
            EXPECT_EQ(bridged.windUpBridgeCounters().bridged_stepped, 1);
        }
    }
    // Premise: at 1.25 turns of the Sun over 900 s a restart steps the model every time.
    EXPECT_EQ(offsets_with_a_step, 7);
}

TEST(WindUpBridgeCorrections, TheEpochsSecondCallReusesTheBridgedValue) {
    const Scene sc;
    WB::Options opt;
    LogManager log;
    GnssCorrections gc(log);
    gc.setSatStateProvider(sc.provider());
    feed(sc, gc, 0.0, 300.0, &opt);
    const double first = sc.call(gc, 900.0, &opt);
    const double again = sc.call(gc, 900.0, &opt);   // another signal of the same epoch
    EXPECT_DOUBLE_EQ(first, again);
    EXPECT_EQ(gc.windUpBridgeCounters().attempts, 1);
}

TEST(WindUpBridgeCorrections, NoProviderCountsARefusalAndRestartsAsBefore) {
    const Scene sc;
    WB::Options opt;
    LogManager log;
    GnssCorrections gc(log);
    feed(sc, gc, 0.0, 300.0, &opt);
    const double got = sc.call(gc, 900.0, &opt);

    LogManager log_p;
    GnssCorrections plain(log_p);
    feed(sc, plain, 0.0, 300.0, nullptr);
    EXPECT_DOUBLE_EQ(got, sc.call(plain, 900.0, nullptr));
    EXPECT_EQ(gc.windUpBridgeCounters().refused_no_state, 1);
    EXPECT_EQ(gc.windUpBridgeCounters().bridged, 0);
    EXPECT_EQ(gc.wuRestarts().size(), 1u);
}

TEST(WindUpBridgeCorrections, GapsOutsideTheBridgedRangeAreUntouched) {
    const Scene sc;
    WB::Options opt;   // bridged range [300, 900) s

    // A gap shorter than the accumulator's own horizon continues as always.
    {
        LogManager log; GnssCorrections gc(log); gc.setSatStateProvider(sc.provider());
        LogManager log_p; GnssCorrections plain(log_p);
        feed(sc, gc, 0.0, 300.0, &opt);
        feed(sc, plain, 0.0, 300.0, nullptr);
        EXPECT_DOUBLE_EQ(sc.call(gc, 300.0 + 120.0, &opt), sc.call(plain, 300.0 + 120.0, nullptr));
        EXPECT_EQ(gc.windUpBridgeCounters().attempts, 0);
        EXPECT_EQ(gc.windUpBridgeCounters().beyond_horizon, 0);
        EXPECT_TRUE(gc.wuRestarts().empty());
    }
    // A gap at the horizon is counted and restarts exactly as before.
    {
        LogManager log; GnssCorrections gc(log); gc.setSatStateProvider(sc.provider());
        LogManager log_p; GnssCorrections plain(log_p);
        feed(sc, gc, 0.0, 300.0, &opt);
        feed(sc, plain, 0.0, 300.0, nullptr);
        EXPECT_DOUBLE_EQ(sc.call(gc, 300.0 + 900.0, &opt), sc.call(plain, 300.0 + 900.0, nullptr));
        EXPECT_EQ(gc.windUpBridgeCounters().attempts, 0);
        EXPECT_EQ(gc.windUpBridgeCounters().beyond_horizon, 1);
        EXPECT_EQ(gc.wuRestarts().size(), 1u);
    }
}

TEST(WindUpBridgeCorrections, AnUmbraAtAVirtualEpochRefusesAndRestartsAsBefore) {
    const Scene sc;
    WB::Options opt;
    LogManager log;
    GnssCorrections gc(log);
    // The provider puts the Sun 2 degrees off the anti-satellite direction: inside the shadow cylinder.
    gc.setSatStateProvider([&sc](const std::string&, const GnssTime&, const Vector3d&,
                                 Vector3d& pos, Vector3d& vel, Vector3d& sun) {
        pos = sc.sat_pos;
        vel = Vector3d::Zero();
        const Vector3d r_hat = sc.sat_pos.normalized();
        sun = -kAu * (std::cos(2.0 * kDeg) * r_hat + std::sin(2.0 * kDeg) * sc.a1);
        return true;
    });
    feed(sc, gc, 0.0, 300.0, &opt);
    const double got = sc.call(gc, 900.0, &opt);

    LogManager log_p;
    GnssCorrections plain(log_p);
    feed(sc, plain, 0.0, 300.0, nullptr);
    EXPECT_DOUBLE_EQ(got, sc.call(plain, 900.0, nullptr));
    EXPECT_EQ(gc.windUpBridgeCounters().refused_umbra, 1);
    EXPECT_EQ(gc.windUpBridgeCounters().bridged, 0);
    EXPECT_EQ(gc.wuRestarts().size(), 1u);
}

TEST(WindUpBridgeCorrections, AProviderThatCannotAnswerRefusesForWantOfState) {
    const Scene sc;
    WB::Options opt;
    LogManager log;
    GnssCorrections gc(log);
    gc.setSatStateProvider([](const std::string&, const GnssTime&, const Vector3d&,
                              Vector3d&, Vector3d&, Vector3d&) { return false; });
    feed(sc, gc, 0.0, 300.0, &opt);
    sc.call(gc, 900.0, &opt);
    EXPECT_EQ(gc.windUpBridgeCounters().refused_no_state, 1);
    EXPECT_EQ(gc.wuRestarts().size(), 1u);
}

TEST(WindUpBridgeCorrections, AWalkThatDisagreesWithTheEpochsOwnAngleRefusesOnClosure) {
    // The provider's Sun turns 1.4 times as fast as the one the epochs were given: over 600 s
    // the walk changes by 420 degrees of yaw against 300, a third of a cycle too much.
    const Scene sc;
    WB::Options opt;
    LogManager log;
    GnssCorrections gc(log);
    gc.setSatStateProvider([&sc](const std::string&, const GnssTime& t, const Vector3d&,
                                 Vector3d& pos, Vector3d& vel, Vector3d& sun) {
        pos = sc.sat_pos;
        vel = Vector3d::Zero();
        const double dt = (t.week - kWeek) * 604800.0 + t.tow - kTow0;
        const double th = (sc.theta0_deg + 1.4 * sc.rate_deg_s * dt) * kDeg;
        sun = sc.sat_pos + kAu * (std::cos(th) * sc.a1 + std::sin(th) * sc.a2);
        return true;
    });
    feed(sc, gc, 0.0, 300.0, &opt);
    sc.call(gc, 900.0, &opt);
    EXPECT_EQ(gc.windUpBridgeCounters().refused_closure, 1);
    EXPECT_EQ(gc.wuRestarts().size(), 1u);
}

// ---------------------------------------------------------------------------
// NoonTurnWindowPredicate
// ---------------------------------------------------------------------------

namespace {

struct OrbitCase {
    Vector3d pos, vel_ecef, sun;
};

/// A circular GPS orbit of inclination 55 degrees whose Sun is at elevation @p beta_deg above
/// the plane, with the satellite at orbit angle @p mu_deg from the midnight.  The inertial and
/// Earth-fixed frames are taken to coincide at this instant.
OrbitCase orbitCase(double beta_deg, double mu_deg) {
    const double inc = 55.0 * kDeg;
    const Vector3d n(0.0, -std::sin(inc), std::cos(inc));
    const Vector3d p = Vector3d::UnitX();            // in the plane (n.x = 0)
    const Vector3d q = n.cross(p);
    const double beta = beta_deg * kDeg, mu = mu_deg * kDeg;
    const Vector3d e_sun = std::cos(beta) * p + std::sin(beta) * n;
    const Vector3d r_hat = -std::cos(mu) * p - std::sin(mu) * q;
    OrbitCase c;
    c.pos = kGpsRadius * r_hat;
    const Vector3d v_inertial = 3874.0 * n.cross(r_hat);
    const Vector3d omega(0.0, 0.0, Constants::EARTH_ANGULAR_VELOCITY);
    c.vel_ecef = v_inertial - omega.cross(c.pos);
    c.sun = kAu * e_sun;
    return c;
}

}  // namespace

TEST(NoonTurnWindowPredicate, InsideTheTurnAtLowBetaOnly) {
    const auto noon_low = orbitCase(2.0, 180.0);
    EXPECT_TRUE(SatAttitude::inNoonTurnWindow(System::GPS, noon_low.pos, noon_low.vel_ecef, noon_low.sun, 0.0));

    const auto noon_high = orbitCase(10.0, 180.0);
    EXPECT_FALSE(SatAttitude::inNoonTurnWindow(System::GPS, noon_high.pos, noon_high.vel_ecef, noon_high.sun, 0.0));

    const auto midnight = orbitCase(2.0, 0.0);
    EXPECT_FALSE(SatAttitude::inNoonTurnWindow(System::GPS, midnight.pos, midnight.vel_ecef, midnight.sun, 0.0));

    const auto quarter = orbitCase(2.0, 90.0);
    EXPECT_FALSE(SatAttitude::inNoonTurnWindow(System::GPS, quarter.pos, quarter.vel_ecef, quarter.sun, 0.0));
}

TEST(NoonTurnWindowPredicate, OnlyGpsHasALaw) {
    const auto noon = orbitCase(2.0, 180.0);
    EXPECT_FALSE(SatAttitude::inNoonTurnWindow(System::Galileo, noon.pos, noon.vel_ecef, noon.sun, 0.0));
    EXPECT_FALSE(SatAttitude::inNoonTurnWindow(System::BeiDou, noon.pos, noon.vel_ecef, noon.sun, 0.0));
}

TEST(NoonTurnWindowPredicate, TheMarginWidensTheWindowByTheOrbitAngleOfTheInterval) {
    const double beta_deg = 2.0;
    const SatAttitude::NoonTurnLaw law{ 0.11, 0.00836 };   // Block IIF, the law the predicate takes
    const auto y = SatAttitude::noonTurnYaw(beta_deg * kDeg, Constants::PI, law);
    ASSERT_GT(y.end_angle_deg, y.start_angle_deg);

    // 0.25 degree of orbit angle before the start of the turn: 60 s of orbit motion is 0.5 degree.
    const auto before = orbitCase(beta_deg, y.start_angle_deg - 0.25);
    EXPECT_FALSE(SatAttitude::inNoonTurnWindow(System::GPS, before.pos, before.vel_ecef, before.sun, 0.0));
    EXPECT_TRUE(SatAttitude::inNoonTurnWindow(System::GPS, before.pos, before.vel_ecef, before.sun, 60.0));
    EXPECT_FALSE(SatAttitude::inNoonTurnWindow(System::GPS, before.pos, before.vel_ecef, before.sun, 10.0));
    // A negative margin counts as none.
    EXPECT_FALSE(SatAttitude::inNoonTurnWindow(System::GPS, before.pos, before.vel_ecef, before.sun, -60.0));

    const auto after = orbitCase(beta_deg, y.end_angle_deg + 0.25);
    EXPECT_FALSE(SatAttitude::inNoonTurnWindow(System::GPS, after.pos, after.vel_ecef, after.sun, 0.0));
    EXPECT_TRUE(SatAttitude::inNoonTurnWindow(System::GPS, after.pos, after.vel_ecef, after.sun, 60.0));
}

TEST(NoonTurnWindowPredicate, DegenerateGeometryIsNotInTheWindow) {
    EXPECT_FALSE(SatAttitude::inNoonTurnWindow(System::GPS, Vector3d::Zero(), Vector3d::Zero(), Vector3d::Zero(), 30.0));
}

// ---------------------------------------------------------------------------
// WindUpBridgeConfig
// ---------------------------------------------------------------------------

namespace {

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = gnss_test::tempDir() / ("wubridge_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

std::string pppConfig(const std::string& estimation_extra) {
    return std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"estimation\": {\n"
        + estimation_extra +
        "    \"estimate_tropo_gradients\": false\n"
        "  }\n"
        "}\n";
}

bool loadInto(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = writeTempConfig(tag, body);
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

}  // namespace

TEST(WindUpBridgeConfig, DefaultsAreOffAndBehaviourPreserving) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("defaults", pppConfig(""), cfg));
    EXPECT_FALSE(cfg.phaseWindUpGapBridge());
    EXPECT_DOUBLE_EQ(cfg.phaseWindUpBridgeMaxGapS(), 900.0);
    EXPECT_DOUBLE_EQ(cfg.phaseWindUpBridgeStepS(), 30.0);
}

TEST(WindUpBridgeConfig, KeysAreHonouredWithAccumulatedWindUp) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("on", pppConfig(
        "    \"accumulate_phase_windup\": true,\n"
        "    \"phase_windup_gap_bridge\": true,\n"
        "    \"phase_windup_bridge_max_gap_s\": 600.0,\n"
        "    \"phase_windup_bridge_step_s\": 20.0,\n"), cfg));
    EXPECT_TRUE(cfg.phaseWindUpGapBridge());
    EXPECT_DOUBLE_EQ(cfg.phaseWindUpBridgeMaxGapS(), 600.0);
    EXPECT_DOUBLE_EQ(cfg.phaseWindUpBridgeStepS(), 20.0);
}

TEST(WindUpBridgeConfig, FractionalWindUpSwitchesTheBridgeOff) {
    // The restart the bridge replaces exists only in accumulated mode, so the knob would be
    // armed and inert; the loader says so and switches it off.
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("fractional", pppConfig("    \"phase_windup_gap_bridge\": true,\n"), cfg));
    EXPECT_FALSE(cfg.accumulatePhaseWindUp());
    EXPECT_FALSE(cfg.phaseWindUpGapBridge());
}

TEST(WindUpBridgeConfig, UnusableLimitsFallBackToTheDefaults) {
    for (const char* v : { "0.0", "-30.0", "121.0", "1000.0" }) {
        GnssConfig cfg;
        ASSERT_TRUE(loadInto("step_bad", pppConfig(
            std::string("    \"accumulate_phase_windup\": true,\n    \"phase_windup_gap_bridge\": true,\n"
                        "    \"phase_windup_bridge_step_s\": ") + v + ",\n"), cfg)) << v;
        EXPECT_DOUBLE_EQ(cfg.phaseWindUpBridgeStepS(), 30.0) << v;
    }
    for (const char* v : { "0.0", "-900.0" }) {
        GnssConfig cfg;
        ASSERT_TRUE(loadInto("gap_bad", pppConfig(
            std::string("    \"accumulate_phase_windup\": true,\n    \"phase_windup_gap_bridge\": true,\n"
                        "    \"phase_windup_bridge_max_gap_s\": ") + v + ",\n"), cfg)) << v;
        EXPECT_DOUBLE_EQ(cfg.phaseWindUpBridgeMaxGapS(), 900.0) << v;
    }
}

}  // namespace GnssProcessor
