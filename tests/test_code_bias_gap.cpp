/**
 * @file test_code_bias_gap.cpp
 * @brief Unit tests of the consumer's code-bias gap response (realtime/CodeBiasGap.hpp): the two
 *        edges, the rows left out, the bar on a new fix, the re-validation verdicts and their
 *        counters, and the arithmetic of the tested quantity.
 */

#include "realtime/CodeBiasGap.hpp"

#include <gtest/gtest.h>
#include <cmath>

using GnssProcessor::CodeBiasGap;

namespace {

CodeBiasGap armed(CodeBiasGap::Mode mode, int revalidate_ep = 3, int window_ep = 6) {
    CodeBiasGap g;
    g.configure({ mode, revalidate_ep, window_ep });
    return g;
}

}  // namespace

TEST(CodeBiasGapTest, OffIsNotArmedAndWritesNothing) {
    CodeBiasGap g;
    EXPECT_FALSE(g.armed());
    g.observe("E18", "C5X", false);   // the engine never calls it unarmed; the ledger stays inert anyway
    EXPECT_TRUE(g.excludedRowKeys().empty());
    EXPECT_FALSE(g.barred("E18"));
}

TEST(CodeBiasGapTest, ModeNamesRoundTrip) {
    using M = CodeBiasGap::Mode;
    EXPECT_EQ(GnssProcessor::codeBiasGapModeFromString("off"), M::Off);
    EXPECT_EQ(GnssProcessor::codeBiasGapModeFromString("revalidate"), M::Revalidate);
    EXPECT_EQ(GnssProcessor::codeBiasGapModeFromString("full"), M::Full);
    EXPECT_EQ(GnssProcessor::codeBiasGapModeFromString("anything"), M::Off);
    EXPECT_STREQ(GnssProcessor::codeBiasGapModeName(M::Full), "full");
}

TEST(CodeBiasGapTest, BothEdgesAreEventsWithTheirLength) {
    CodeBiasGap g = armed(CodeBiasGap::Mode::Full);
    g.observe("E18", "C1X", true);
    g.observe("E18", "C5X", true);
    EXPECT_TRUE(g.events().empty());
    for (int k = 0; k < 4; ++k) g.observe("E18", "C5X", false);
    ASSERT_EQ(g.events().size(), 1u);
    EXPECT_EQ(g.events()[0].kind, CodeBiasGap::EventKind::Open);
    EXPECT_EQ(g.events()[0].signal, "C5X");
    EXPECT_TRUE(g.inGap("E18", "C5X"));
    EXPECT_FALSE(g.inGap("E18", "C1X"));
    EXPECT_TRUE(g.satInGap("E18"));
    g.clearEvents();
    g.observe("E18", "C5X", true);
    ASSERT_EQ(g.events().size(), 1u);
    EXPECT_EQ(g.events()[0].kind, CodeBiasGap::EventKind::Close);
    EXPECT_EQ(g.events()[0].n, 4);
    EXPECT_FALSE(g.satInGap("E18"));
    EXPECT_EQ(g.counters().opened, 1u);
    EXPECT_EQ(g.counters().closed, 1u);
    EXPECT_EQ(g.counters().gap_epochs, 4u);
    EXPECT_EQ(g.counters().windows, 1u);
}

TEST(CodeBiasGapTest, FullLeavesTheRowOutAndBarsTheSatellite) {
    CodeBiasGap g = armed(CodeBiasGap::Mode::Full, 3, 6);
    g.observe("E32", "C5X", false);
    const auto keys = g.excludedRowKeys();
    ASSERT_EQ(keys.size(), 1u);
    EXPECT_EQ(keys[0], "E32_C5X");
    EXPECT_TRUE(g.barred("E32"));
    EXPECT_FALSE(g.barred("E26"));
    // The bias returns: the row is back, the bar lasts revalidate_ep epochs of the window.
    g.observe("E32", "C5X", true);
    EXPECT_TRUE(g.excludedRowKeys().empty());
    EXPECT_TRUE(g.barred("E32"));
    for (int k = 0; k < 3; ++k) g.sample("E32", std::nullopt);
    EXPECT_FALSE(g.barred("E32"));
}

TEST(CodeBiasGapTest, RevalidateWritesNothingAtTheOpenEdge) {
    CodeBiasGap g = armed(CodeBiasGap::Mode::Revalidate);
    g.observe("E18", "C5X", false);
    EXPECT_TRUE(g.excludedRowKeys().empty());
    EXPECT_FALSE(g.barred("E18"));
    g.observe("E18", "C5X", true);
    EXPECT_FALSE(g.barred("E18"));
    EXPECT_EQ(g.windowSatellites().size(), 1u);
}

TEST(CodeBiasGapTest, AHoldOneWideLaneOffIsReleasedAndTestedAgain) {
    CodeBiasGap g = armed(CodeBiasGap::Mode::Full, 3, 6);
    g.observe("E18", "C5X", false);
    g.observe("E18", "C5X", true);
    g.clearEvents();
    EXPECT_FALSE(g.sample("E18", 0.95).has_value());
    EXPECT_FALSE(g.sample("E18", 1.05).has_value());
    const auto v = g.sample("E18", 1.00);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, CodeBiasGap::EventKind::Released);
    ASSERT_EQ(g.events().size(), 1u);
    EXPECT_NEAR(g.events()[0].mean_cyc, 1.0, 1e-12);
    EXPECT_EQ(g.events()[0].n, 3);
    // A new window: barred again in full, and tested again.
    EXPECT_TRUE(g.barred("E18"));
    EXPECT_EQ(g.counters().released, 1u);
    EXPECT_EQ(g.counters().windows, 2u);
    EXPECT_TRUE(g.closureHolds());
}

TEST(CodeBiasGapTest, AHoldWithinHalfACycleIsValidated) {
    CodeBiasGap g = armed(CodeBiasGap::Mode::Revalidate, 3, 6);
    g.observe("E14", "C5X", false);
    g.observe("E14", "C5X", true);
    g.sample("E14", 0.40);
    g.sample("E14", 0.45);
    const auto v = g.sample("E14", 0.44);   // mean 0.43: the code still rounds to the held wide lane
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, CodeBiasGap::EventKind::Validated);
    EXPECT_TRUE(g.windowSatellites().empty());
    EXPECT_EQ(g.counters().validated, 1u);
    EXPECT_TRUE(g.closureHolds());
}

TEST(CodeBiasGapTest, ABrokenRunRestartsAndAnEmptyWindowExpires) {
    CodeBiasGap g = armed(CodeBiasGap::Mode::Revalidate, 3, 6);
    g.observe("E32", "C5Q", false);
    g.observe("E32", "C5Q", true);
    g.sample("E32", 1.0);
    g.sample("E32", 1.0);
    g.sample("E32", std::nullopt);          // the hold broke: the run restarts
    EXPECT_FALSE(g.sample("E32", 0.0).has_value());
    EXPECT_FALSE(g.sample("E32", 0.0).has_value());
    // Age 6 reached with a run under way: the run finishes past the window's end.
    const auto v = g.sample("E32", 0.0);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, CodeBiasGap::EventKind::Validated);

    CodeBiasGap h = armed(CodeBiasGap::Mode::Revalidate, 3, 4);
    h.observe("E14", "C5X", false);
    h.observe("E14", "C5X", true);
    std::optional<CodeBiasGap::EventKind> last;
    for (int k = 0; k < 4; ++k) last = h.sample("E14", std::nullopt);
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(*last, CodeBiasGap::EventKind::Expired);
    EXPECT_EQ(h.counters().expired, 1u);
    EXPECT_TRUE(h.closureHolds());
}

TEST(CodeBiasGapTest, ANewGapSupersedesARunningWindow) {
    CodeBiasGap g = armed(CodeBiasGap::Mode::Full, 3, 6);
    g.observe("E18", "C5X", false);
    g.observe("E18", "C5X", true);
    EXPECT_EQ(g.windowSatellites().size(), 1u);
    g.observe("E18", "C5X", false);         // lost again before a verdict
    EXPECT_TRUE(g.windowSatellites().empty());
    EXPECT_EQ(g.counters().expired, 1u);
    g.observe("E18", "C5X", true);
    EXPECT_EQ(g.windowSatellites().size(), 1u);
    EXPECT_TRUE(g.closureHolds());
}

TEST(CodeBiasGapTest, TheWindowOpensWhenTheLastSignalReturns) {
    CodeBiasGap g = armed(CodeBiasGap::Mode::Full);
    g.observe("E18", "C1X", false);
    g.observe("E18", "C5X", false);
    g.observe("E18", "C1X", true);
    EXPECT_TRUE(g.windowSatellites().empty());
    EXPECT_TRUE(g.barred("E18"));
    g.observe("E18", "C5X", true);
    EXPECT_EQ(g.windowSatellites().size(), 1u);
}

TEST(CodeBiasGapTest, MedianRemovalIsPerSystemAndNeedsFourSatellites) {
    std::map<std::string, double> d{ { "E04", 7.0 }, { "E06", 7.1 }, { "E09", 6.9 },
                                     { "E18", 8.0 }, { "G01", 1.0 }, { "G02", 1.0 } };
    const auto off = CodeBiasGap::medianRemoved(d);
    ASSERT_EQ(off.size(), 4u);                // GPS has two satellites: no offsets
    EXPECT_NEAR(off.at("E18"), 8.0 - 7.05, 1e-12);
    EXPECT_NEAR(off.at("E09"), 6.9 - 7.05, 1e-12);
    EXPECT_EQ(off.count("G01"), 0u);
}

TEST(CodeBiasGapTest, MelbourneWubbenaRecoversTheWideLaneAndSeesAMissingCodeBias) {
    // Galileo E1/E5a, an ionosphere of 3 m on E1, ambiguities (94, 103), satellite biases on all
    // four signals.  The corrected combination returns 94 - 103 = -9 exactly.
    const double c = 299792458.0, f1 = 1575.42e6, f5 = 1176.45e6;
    const double l1 = c / f1, l5 = c / f5, lw = c / (f1 - f5);
    const double rho = 2.3e7, iono1 = 3.0, iono5 = iono1 * (f1 * f1) / (f5 * f5);
    const double pb1 = 1.75, pb5 = 24.60, cb1 = -1.19, cb5 = -2.128;
    const double phi1 = rho - iono1 + l1 * 94 + pb1;
    const double phi5 = rho - iono5 + l5 * 103 + pb5;
    const double p1 = rho + iono1 + cb1, p5 = rho + iono5 + cb5;
    const double mw = CodeBiasGap::correctedMelbourneWubbena(f1, f5, phi1, phi5, p1, p5, pb1, pb5, cb1, cb5);
    EXPECT_NEAR(mw, -9.0, 1e-6);
    // The same with the E5a code bias missing (taken as 0): the wide lane moves by
    // f5 / (f1 + f5) * cb5 / lambda_WL = -1.21 cycles toward -8, the observed defect.
    const double mw0 = CodeBiasGap::correctedMelbourneWubbena(f1, f5, phi1, phi5, p1, p5, pb1, pb5, cb1, 0.0);
    EXPECT_NEAR(mw0 - mw, -f5 / (f1 + f5) * cb5 / lw, 1e-9);
    EXPECT_NEAR(mw0, -7.79, 0.01);
}
