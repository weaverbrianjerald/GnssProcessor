/**
 * @file test_amb_level_check.cpp
 * @brief Tests for the consumer's re-validation of its float ambiguities against
 *        their own code level: @c AmbLevelCheck and the
 *        @c network.closed_loop.amb_level_check plumbing.
 *
 * Checked here:
 *   - the two combinations: the WL statistic is geometry-free and first-order
 *     ionosphere-free, and so is the IF statistic, so an error common to all four
 *     rows and a dispersive (1/f^2) error on the phase/code pair both cancel
 *     exactly.  A receiver clock on the phase rows only does not cancel, so a
 *     trip alone does not prove an ambiguity error (see AmbLevelCheck.hpp);
 *   - (a) a 1.0 m ambiguity drift against a consistent code level trips the
 *     check once the window fills, at k = 4, with realistic noise
 *     (code sigma 0.5 m, phase sigma 5 mm);
 *   - (b) a consistent ambiguity never trips over 500 epochs at that noise;
 *   - (c) a translation epoch restarts the window: the pre-step samples are
 *     never tested against the post-step state, and the next test is
 *     window_ep epochs later;
 *   - no test runs before the window fills, the sigma floors bound an
 *     abnormally quiet window, a trip clears the window in both modes, and the
 *     counters close;
 *   - the config plumbing: defaults off, the harness knob reaches only the
 *     derived consumer, a standalone consumer sets the processing key, the three
 *     tuning knobs travel with it, and bad values are rejected rather than used.
 *
 * The engine-level part (the innovation harvest, the band pairing, the variance
 * half of the restart rule, the refloat through @c StateManager::resetAmbiguity
 * and the CST `refloat_level` kind) needs a full engine and product stream and is
 * validated on field/sim arms, as for the declared-step carry and the step-translate ledger.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "config/GnssConfigTypes.hpp"
#include "logging/LogManager.hpp"
#include "realtime/AmbLevelCheck.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <string>
#include <vector>
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

// GPS L1 / L2, the consumer's own pair on the RF-0hv posture.
constexpr double kC  = 299792458.0;
constexpr double kF1 = 1575.42e6;
constexpr double kF2 = 1227.60e6;
const double kLamWl  = kC / (kF1 - kF2);     // ~0.8619 m

/// The four pre-fit innovations of one satellite-epoch, built from an explicit
/// error budget so each test states exactly what it is injecting.
struct Rows {
    double ip1{ 0.0 }, ip2{ 0.0 }, ic1{ 0.0 }, ic2{ 0.0 };
};

/// @param e1,e2   ambiguity error on band 1 / band 2 (m), sign as it appears in
///                the phase innovation (innov = observed - predicted, and the
///                predicted carries +amb, so a state too large by d gives -d).
/// @param common  an error common to all four rows (m): geometry, receiver clock
///                common to code and phase, troposphere; the combinations cancel it.
/// @param iono1   first-order slant ionosphere error on band 1 (m); band 2 gets
///                iono1 * (f1/f2)^2, phase -I and code +I.
Rows makeRows(double e1, double e2, double common = 0.0, double iono1 = 0.0) {
    const double iono2 = iono1 * (kF1 / kF2) * (kF1 / kF2);
    Rows r;
    r.ip1 = common - iono1 + e1;
    r.ip2 = common - iono2 + e2;
    r.ic1 = common + iono1;
    r.ic2 = common + iono2;
    return r;
}

double wlOf(const Rows& r) {
    return AmbLevelCheck::wlLevelMinusState(kF1, kF2, kLamWl, r.ip1, r.ip2, r.ic1, r.ic2);
}
double ifOf(const Rows& r) {
    return AmbLevelCheck::ifLevelMinusState(kF1, kF2, r.ip1, r.ip2, r.ic1, r.ic2);
}

AmbLevelCheck::Config cfg(int window, double k,
                          AmbLevelCheck::Mode mode = AmbLevelCheck::Mode::Act) {
    AmbLevelCheck::Config c;
    c.window_ep = window;
    c.k = k;
    c.mode = mode;
    return c;
}

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = gnss_test::tempDir() / ("alc_amb_level_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

/// A loadable network config with the closed-loop block under test.
std::string networkConfig(const std::string& closed_loop_extra) {
    return std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"AAAA\",\n"
        "    \"closed_loop\": { \"enable\": false" + closed_loop_extra + " },\n"
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// The combinations cancel what they are meant to cancel.
// ---------------------------------------------------------------------------
TEST(AmbLevelCheckTest, BothCombinationsKillGeometryAndFirstOrderIonosphere) {
    // No ambiguity error, but 17 m of common mode and 6 m of slant iono: both
    // statistics are zero to double rounding.
    const Rows clean = makeRows(0.0, 0.0, 17.0, 6.0);
    EXPECT_NEAR(wlOf(clean), 0.0, 1e-9);
    EXPECT_NEAR(ifOf(clean), 0.0, 1e-9);

    // A one-cycle error on band 1 alone reads as one WL cycle, since the WL
    // statistic is e1/lambda1 - e2/lambda2 in cycles.
    const double lam1 = kC / kF1, lam2 = kC / kF2;
    const Rows wl1 = makeRows(-lam1, 0.0, 17.0, 6.0);   // state too large by lambda1 on band 1
    EXPECT_NEAR(wlOf(wl1), -1.0, 1e-9);
    // The IF statistic of the same error is the IF combination in metres.
    const double g1 = kF1 * kF1, g2 = kF2 * kF2;
    EXPECT_NEAR(ifOf(wl1), (g1 * (-lam1) - g2 * 0.0) / (g1 - g2), 1e-9);

    // A one-cycle error on both bands (N1 = N2 = 1) leaves N1 - N2 unchanged, so
    // the WL statistic is 0 while the IF statistic sees the level.
    const Rows both = makeRows(-lam1, -lam2, 0.0, 0.0);
    EXPECT_NEAR(wlOf(both), -1.0 + 1.0, 1e-9);          // N1 - N2 with N1 = N2 = 1 => 0
    EXPECT_NEAR(ifOf(both), (g1 * (-lam1) - g2 * (-lam2)) / (g1 - g2), 1e-9);
}

// ---------------------------------------------------------------------------
// (a) A 1.0 m ambiguity drift against a consistent code level trips.
// ---------------------------------------------------------------------------
TEST(AmbLevelCheckTest, OneMetreDriftTripsAfterTheWindowFillsAtKFour) {
    AmbLevelCheck chk;
    chk.configure(cfg(20, 4.0));

    std::mt19937 rng(20260915u);
    std::normal_distribution<double> code(0.0, 0.5);    // code sigma 0.5 m
    std::normal_distribution<double> phase(0.0, 0.005); // phase sigma 5 mm

    // The ambiguity of both bands is 1.0 m too large: the phase innovations carry
    // -1.0 m and the code rows are consistent (no error).
    bool tripped = false;
    int trip_epoch = -1;
    for (int ep = 0; ep < 20; ++ep) {
        Rows r = makeRows(-1.0, -1.0, 12.0, 4.0);
        r.ip1 += phase(rng); r.ip2 += phase(rng);
        r.ic1 += code(rng);  r.ic2 += code(rng);
        const auto v = chk.push("G12", wlOf(r), ifOf(r));
        EXPECT_EQ(v.tested, ep == 19) << "epoch " << ep;   // no test before the window fills
        if (v.test != AmbLevelCheck::Test::None) { tripped = true; trip_epoch = ep; }
    }
    ASSERT_TRUE(tripped);
    EXPECT_EQ(trip_epoch, 19);                             // when the window filled
    EXPECT_EQ(chk.counters().trips_wl + chk.counters().trips_if, 1u);
    EXPECT_EQ(chk.counters().checks, 1u);
    EXPECT_EQ(chk.counters().samples, 20u);
    // Which channel trips, and why: a 1.0 m error equal in metres on both bands is
    // 1.0 m of IF level (the IF coefficients sum to 1) and 1.0/lam1 - 1.0/lam2 =
    // 1.16 WL cycles (negative here).  At code sigma 0.5 m and n = 20 the WL
    // channel's 4-sigma bar is 0.37 cyc and the IF channel's is 1.33 m, so the
    // error is 3.1 WL bars but only 0.75 IF bars; the WL channel is about 4x more
    // sensitive here and is the one that fires.  The design quotes both bars.
    EXPECT_EQ(chk.counters().trips_wl, 1u);
}

// The IF channel detects a level error the WL channel cannot see: a drift that
// is band-common in cycles leaves N1 - N2 unchanged, so only the iono-free
// level moves.  The WL test does not cover this case; the design pre-registered
// it for the IF test.
TEST(AmbLevelCheckTest, TheIfTestCarriesAWlSilentLevelDriftOnItsOwn) {
    AmbLevelCheck chk;
    // 120 epochs (1 h at 30 s): the IF channel's 4-sigma bar at code sigma 0.5 m
    // falls to 0.54 m, so a 1.0 m IF-level drift clears it by ~3.4 sigma.  The
    // same drift at the 20-epoch default would not trip: the IF channel's
    // minimum detectable IF level there is 1.33 m, as the design states.
    chk.configure(cfg(120, 4.0));

    std::mt19937 rng(31415926u);
    std::normal_distribution<double> code(0.0, 0.5);
    std::normal_distribution<double> phase(0.0, 0.005);

    const double lam1 = kC / kF1, lam2 = kC / kF2;
    const double lam_nl = kC / (kF1 + kF2);
    const double n_cyc = 1.0 / lam_nl;                     // exactly 1.0 m of IF level
    for (int ep = 0; ep < 120; ++ep) {
        Rows r = makeRows(-n_cyc * lam1, -n_cyc * lam2, 12.0, 4.0);
        r.ip1 += phase(rng); r.ip2 += phase(rng);
        r.ic1 += code(rng);  r.ic2 += code(rng);
        const auto v = chk.push("G12", wlOf(r), ifOf(r));
        if (ep == 119) {
            EXPECT_NEAR(v.wl_mean, 0.0, 0.05);             // the WL channel is silent
            EXPECT_NEAR(v.if_mean, -1.0, 0.25);            // the IF channel carries the metre
        }
    }
    EXPECT_EQ(chk.counters().trips_wl, 0u);
    EXPECT_EQ(chk.counters().trips_if, 1u);
}

// ---------------------------------------------------------------------------
// (b) A consistent ambiguity never trips over 500 epochs at that noise.
// ---------------------------------------------------------------------------
TEST(AmbLevelCheckTest, ConsistentAmbiguityNeverTripsOverFiveHundredEpochs) {
    AmbLevelCheck chk;
    chk.configure(cfg(20, 4.0));

    std::mt19937 rng(11235813u);
    std::normal_distribution<double> code(0.0, 0.5);
    std::normal_distribution<double> phase(0.0, 0.005);

    for (int ep = 0; ep < 500; ++ep) {
        Rows r = makeRows(0.0, 0.0, 12.0 + 0.01 * ep, 4.0 - 0.002 * ep);  // drifting nuisances
        r.ip1 += phase(rng); r.ip2 += phase(rng);
        r.ic1 += code(rng);  r.ic2 += code(rng);
        chk.push("G12", wlOf(r), ifOf(r));
    }
    EXPECT_EQ(chk.counters().trips_wl, 0u);
    EXPECT_EQ(chk.counters().trips_if, 0u);
    EXPECT_EQ(chk.counters().samples, 500u);
    EXPECT_EQ(chk.counters().checks, 500u - 19u);      // every epoch from the 20th on
}

// ---------------------------------------------------------------------------
// (c) A translation epoch restarts the window.
// ---------------------------------------------------------------------------
TEST(AmbLevelCheckTest, ATranslationEpochRestartsTheWindow) {
    AmbLevelCheck chk;
    chk.configure(cfg(20, 4.0));

    // 19 clean samples: not yet testable.
    for (int ep = 0; ep < 19; ++ep) {
        const Rows r = makeRows(0.0, 0.0, 12.0, 4.0);
        EXPECT_FALSE(chk.push("G12", wlOf(r), ifOf(r)).tested);
    }
    EXPECT_EQ(chk.depth("G12"), 19u);

    // The engine saw a translation for G12 this epoch and restarts the window
    // before pushing the epoch's sample (RealTimeEngineEstimate.cpp).
    EXPECT_TRUE(chk.restart("G12"));
    EXPECT_EQ(chk.depth("G12"), 0u);
    EXPECT_EQ(chk.counters().restarts, 1u);

    // A grossly wrong state from here on: the first 19 post-restart epochs are
    // still untestable, so no window that contains the translation can trip; the
    // next test is window_ep epochs after the step.
    for (int ep = 0; ep < 19; ++ep) {
        const Rows r = makeRows(-3.0, -3.0, 12.0, 4.0);
        EXPECT_FALSE(chk.push("G12", wlOf(r), ifOf(r)).tested) << "epoch " << ep;
    }
    EXPECT_EQ(chk.counters().checks, 0u);
    const Rows r = makeRows(-3.0, -3.0, 12.0, 4.0);
    const auto v = chk.push("G12", wlOf(r), ifOf(r));
    EXPECT_TRUE(v.tested);
    EXPECT_NE(v.test, AmbLevelCheck::Test::None);
    EXPECT_EQ(v.n, 20);

    // A restart on a satellite with no window is not counted (nothing was lost).
    EXPECT_FALSE(chk.restart("G99"));
    EXPECT_EQ(chk.counters().restarts, 1u);
}

// ---------------------------------------------------------------------------
// The sigma floors, the trip's window clear, and the counter closure.
// ---------------------------------------------------------------------------
TEST(AmbLevelCheckTest, SigmaFloorsBoundAnAbnormallyQuietWindow) {
    // A noiseless window has sample sigma 0, so without a floor any non-zero mean
    // would trip at any k.  The floors turn that into a bar of k * 0.02 cyc /
    // k * 0.05 m.
    AmbLevelCheck quiet;
    quiet.configure(cfg(5, 4.0));
    // Offsets below both floored bars: 4 * 0.02 = 0.08 cyc and 4 * 0.05 = 0.20 m.
    for (int ep = 0; ep < 5; ++ep) quiet.push("G01", 0.05, 0.10);
    EXPECT_EQ(quiet.counters().checks, 1u);
    EXPECT_EQ(quiet.counters().trips_wl + quiet.counters().trips_if, 0u);

    AmbLevelCheck loud;
    loud.configure(cfg(5, 4.0));
    for (int ep = 0; ep < 5; ++ep) loud.push("G01", 0.50, 0.10);   // 0.50 > 0.08 cyc
    EXPECT_EQ(loud.counters().trips_wl, 1u);
    EXPECT_EQ(loud.counters().trips_if, 0u);                        // WL is tested first
}

TEST(AmbLevelCheckTest, ATripClearsTheWindowInBothModes) {
    for (auto mode : { AmbLevelCheck::Mode::Act, AmbLevelCheck::Mode::Observe }) {
        AmbLevelCheck chk;
        chk.configure(cfg(4, 4.0, mode));
        for (int ep = 0; ep < 4; ++ep) chk.push("G05", 0.90, 0.10);
        EXPECT_EQ(chk.counters().trips_wl, 1u);
        EXPECT_EQ(chk.depth("G05"), 0u);          // cleared, so the next test is 4 epochs away
        for (int ep = 0; ep < 3; ++ep)
            EXPECT_FALSE(chk.push("G05", 0.90, 0.10).tested);
        EXPECT_TRUE(chk.push("G05", 0.90, 0.10).tested);
        EXPECT_EQ(chk.counters().trips_wl, 2u);
        EXPECT_EQ(chk.counters().checks, 2u);
    }
}

TEST(AmbLevelCheckTest, CountersCloseOverAMixedSequence) {
    AmbLevelCheck chk;
    chk.configure(cfg(4, 4.0));
    // G01 trips twice, G02 never trips, G03 is restarted before it can fill.
    for (int ep = 0; ep < 8; ++ep) chk.push("G01", 0.90, 0.10);
    for (int ep = 0; ep < 8; ++ep) chk.push("G02", 0.01, 0.01);
    for (int ep = 0; ep < 3; ++ep) chk.push("G03", 5.00, 5.00);
    chk.restart("G03");
    for (int ep = 0; ep < 3; ++ep) chk.push("G03", 5.00, 5.00);

    const auto& c = chk.counters();
    EXPECT_EQ(c.sats, 3u);
    EXPECT_EQ(c.samples, 8u + 8u + 3u + 3u);
    EXPECT_EQ(c.checks, 2u + 5u + 0u);            // G01: 2 (each trip clears), G02: 5
    EXPECT_EQ(c.trips_wl, 2u);
    EXPECT_EQ(c.trips_if, 0u);
    EXPECT_EQ(c.restarts, 1u);
    EXPECT_LE(c.trips_wl + c.trips_if, c.checks);  // the closure inequality
    EXPECT_EQ(c.refloats, 0u);                     // the engine reports those
    chk.noteRefloat();
    EXPECT_EQ(chk.counters().refloats, 1u);
}

TEST(AmbLevelCheckTest, NonFiniteSamplesAndDegenerateWindowsAreRefused) {
    AmbLevelCheck chk;
    chk.configure(cfg(3, 4.0));
    chk.push("G07", std::nan(""), 0.0);
    chk.push("G07", 0.0, std::numeric_limits<double>::infinity());
    EXPECT_EQ(chk.counters().samples, 0u);
    EXPECT_EQ(chk.depth("G07"), 0u);

    AmbLevelCheck degenerate;
    degenerate.configure(cfg(1, 4.0));            // a window of 1 has no sample sigma
    for (int ep = 0; ep < 10; ++ep) degenerate.push("G07", 9.0, 9.0);
    EXPECT_EQ(degenerate.counters().samples, 0u);
    EXPECT_EQ(degenerate.counters().checks, 0u);
}

TEST(AmbLevelCheckTest, TestNamesAreTheEventKinds) {
    EXPECT_STREQ(AmbLevelCheck::testName(AmbLevelCheck::Test::Wl), "wl");
    EXPECT_STREQ(AmbLevelCheck::testName(AmbLevelCheck::Test::If), "if");
    EXPECT_STREQ(AmbLevelCheck::testName(AmbLevelCheck::Test::None), "");
}

// ---------------------------------------------------------------------------
// The config plumbing.
// ---------------------------------------------------------------------------
TEST(AmbLevelCheckTest, ConfigDefaultsAreOffAndTheKnobReachesOnlyTheDerivedConsumer) {
    {
        GnssConfig d;
        EXPECT_FALSE(d.ambLevelCheck());
        EXPECT_EQ(d.getAmbLevelCheckWindowEp(), 20);
        EXPECT_DOUBLE_EQ(d.getAmbLevelCheckK(), 4.0);
        EXPECT_EQ(d.getAmbLevelCheckMode(), "act");
        EXPECT_FALSE(d.getNetworkConfig().closed_loop.amb_level_check);
        EXPECT_EQ(d.getNetworkConfig().closed_loop.amb_level_check_window_ep, 20);
        EXPECT_DOUBLE_EQ(d.getNetworkConfig().closed_loop.amb_level_check_k, 4.0);
        EXPECT_EQ(d.getNetworkConfig().closed_loop.amb_level_check_mode, "act");
    }
    {
        LogManager log;
        GnssConfig cfg_h;
        const fs::path p = writeTempConfig(
            "harness", networkConfig(", \"amb_level_check\": true,"
                                     " \"amb_level_check_window_ep\": 30,"
                                     " \"amb_level_check_k\": 5.0,"
                                     " \"amb_level_check_mode\": \"observe\""));
        ASSERT_TRUE(cfg_h.loadFromJson(p, log));
        EXPECT_TRUE(cfg_h.getNetworkConfig().closed_loop.amb_level_check);
        EXPECT_FALSE(cfg_h.ambLevelCheck());                     // the producer never checks
        const GnssConfig consumer = cfg_h.deriveClosedLoopConsumerConfig();
        EXPECT_TRUE(consumer.ambLevelCheck());                   // the consumer does
        EXPECT_EQ(consumer.getAmbLevelCheckWindowEp(), 30);
        EXPECT_DOUBLE_EQ(consumer.getAmbLevelCheckK(), 5.0);
        EXPECT_EQ(consumer.getAmbLevelCheckMode(), "observe");
        std::error_code ec; fs::remove(p, ec);
    }
    {
        LogManager log;
        GnssConfig cfg_off;
        const fs::path p = writeTempConfig("off", networkConfig(""));
        ASSERT_TRUE(cfg_off.loadFromJson(p, log));
        EXPECT_FALSE(cfg_off.deriveClosedLoopConsumerConfig().ambLevelCheck());
        std::error_code ec; fs::remove(p, ec);
    }
    {
        LogManager log;
        GnssConfig cfg_s;
        const fs::path p = writeTempConfig(
            "standalone", "{ \"processing\": { \"amb_level_check\": true,"
                          " \"amb_level_check_window_ep\": 12,"
                          " \"amb_level_check_k\": 3.5 } }\n");
        ASSERT_TRUE(cfg_s.loadFromJson(p, log));
        EXPECT_TRUE(cfg_s.ambLevelCheck());
        EXPECT_EQ(cfg_s.getAmbLevelCheckWindowEp(), 12);
        EXPECT_DOUBLE_EQ(cfg_s.getAmbLevelCheckK(), 3.5);
        EXPECT_EQ(cfg_s.getAmbLevelCheckMode(), "act");          // the default survives
        std::error_code ec; fs::remove(p, ec);
    }
}

TEST(AmbLevelCheckTest, BadKnobValuesAreRejectedRatherThanUsed) {
    LogManager log;
    GnssConfig cfg_bad;
    const fs::path p = writeTempConfig(
        "bad", "{ \"processing\": { \"amb_level_check\": true,"
               " \"amb_level_check_window_ep\": 1,"
               " \"amb_level_check_k\": -2.0,"
               " \"amb_level_check_mode\": \"refloat-everything\" } }\n");
    ASSERT_TRUE(cfg_bad.loadFromJson(p, log));
    EXPECT_TRUE(cfg_bad.ambLevelCheck());
    EXPECT_EQ(cfg_bad.getAmbLevelCheckWindowEp(), 20);     // a 1-sample window has no sigma
    EXPECT_DOUBLE_EQ(cfg_bad.getAmbLevelCheckK(), 4.0);
    EXPECT_EQ(cfg_bad.getAmbLevelCheckMode(), "act");
    std::error_code ec; fs::remove(p, ec);
}
