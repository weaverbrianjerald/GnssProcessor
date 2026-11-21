/**
 * @file test_float_phase_scale.cpp
 * @brief Unit tests of the receiver's own phase factor (realtime/FloatPhaseScale.hpp).
 *
 * The cases:
 *   - the scaling helper multiplies only the phase rows of the named satellites, and an empty map changes nothing;
 *   - a row identity "G05-G12" splits into its two satellites;
 *   - the factor is the median of the per-epoch values (sum f v^2 / R over sum r) and is formed only after the
 *     minimum number of epochs (1 before);
 *   - a row whose two satellites carry different applied factors does not inform the factor;
 *   - a row beyond the tail bar stays out of the estimate and puts its satellite in the tail set, with both edges
 *     counted; an epoch whose tail share exceeds the freeze share is withheld;
 *   - the map gives the factor to observed satellites that are not held, not in the tail and of a formed
 *     constellation, and counts each switch;
 *   - values older than the window leave it.
 *
 * Suite name @c FloatPhaseScale is owned solely by this file.
 */

#include <gtest/gtest.h>

#include "realtime/FloatPhaseScale.hpp"

#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

// Rows whose epoch value is exactly k2: r = sr / R = 0.5 and v^2 / R = 0.5 k2 on every row.
std::vector<FloatPhaseScaleRow> epochRows(double k2, const std::string& ref, const std::vector<std::string>& sats,
                                          double f = 1.0) {
    std::vector<FloatPhaseScaleRow> rows;
    const double R = 1e-6 * f, sr = 0.5e-6 * f;
    const double v = std::sqrt(0.5 * k2 / f * R);   // v^2 / R_applied = 0.5 k2 / f, so f v^2 / R = 0.5 k2
    for (const auto& s : sats) {
        rows.push_back({ s, ref, "L1C", v, R, sr });
        rows.push_back({ s, ref, "C1C", 10 * v, 0.09, 0.05 });   // code rows are ignored
    }
    return rows;
}

FloatPhaseScaleSettings fastSettings() {
    FloatPhaseScaleSettings s;
    s.min_epochs = 5;
    s.window_s = 3600.0;
    return s;
}

TEST(FloatPhaseScale, TheHelperScalesOnlyThePhaseRowsOfTheNamedSatellites) {
    Eigen::VectorXd R(4);
    R << 1.0, 2.0, 3.0, 4.0;
    const std::vector<std::string> sats = { "G05", "G05", "G07", "E11" };
    const std::vector<std::string> sigs = { "C1C", "L1C", "L2W", "L1X" };
    EXPECT_EQ(applyPhaseVarianceScale(R, sats, sigs, {}), 0);
    EXPECT_DOUBLE_EQ(R(1), 2.0);
    EXPECT_EQ(applyPhaseVarianceScale(R, sats, sigs, { { "G05", 0.25 }, { "E11", 0.5 } }), 2);
    EXPECT_DOUBLE_EQ(R(0), 1.0);   // code row of G05
    EXPECT_DOUBLE_EQ(R(1), 0.5);   // phase row of G05
    EXPECT_DOUBLE_EQ(R(2), 3.0);   // G07 not in the map
    EXPECT_DOUBLE_EQ(R(3), 2.0);
}

TEST(FloatPhaseScale, ARowIdentitySplitsIntoItsTwoSatellites) {
    EXPECT_EQ(splitRowSatellites("G05-G12"), (std::pair<std::string, std::string>{ "G05", "G12" }));
    EXPECT_EQ(splitRowSatellites("E11"), (std::pair<std::string, std::string>{ "E11", "" }));
}

TEST(FloatPhaseScale, TheFactorIsTheMedianOfTheEpochValuesOnceEnoughEpochsAreIn) {
    FloatPhaseScale est(fastSettings());
    const std::vector<double> values = { 0.20, 0.18, 0.25, 0.17, 0.22, 0.19 };
    for (std::size_t e = 0; e < values.size(); ++e) {
        est.observeEpoch(30.0 * e, epochRows(values[e], "G12", { "G05", "G07", "G15" }), {});
        if (e < 4) {
            EXPECT_FALSE(est.formed('G')) << e;
            EXPECT_DOUBLE_EQ(est.factor('G'), 1.0);
        }
    }
    ASSERT_TRUE(est.formed('G'));
    EXPECT_NEAR(est.factor('G'), 0.20, 1e-12);   // median of six: the upper middle of 0.17 .. 0.25 sorted
    EXPECT_FALSE(est.formed('E'));
    EXPECT_EQ(est.counters().values_pushed, 6);
    EXPECT_EQ(est.counters().rows_used, 18);
}

TEST(FloatPhaseScale, ARowBetweenSatellitesOfDifferentFactorsIsSkipped) {
    FloatPhaseScale est(fastSettings());
    // G05 carries 0.2 (float), G12 the configured weight (held): its rows mix the two.
    est.observeEpoch(0.0, epochRows(0.2, "G12", { "G05", "G07", "G15" }), { { "G05", 0.2 } });
    EXPECT_EQ(est.counters().rows_mixed, 1);
    EXPECT_EQ(est.counters().rows_used, 2);
    // With every satellite at 0.2 the factor is taken back out: the value is still the configured-weight value.
    FloatPhaseScale est2(fastSettings());
    for (int e = 0; e < 5; ++e)
        est2.observeEpoch(30.0 * e, epochRows(0.3, "G12", { "G05", "G07", "G15" }, 0.2),
                          { { "G05", 0.2 }, { "G07", 0.2 }, { "G15", 0.2 }, { "G12", 0.2 } });
    ASSERT_TRUE(est2.formed('G'));
    EXPECT_NEAR(est2.factor('G'), 0.3, 1e-12);
}

TEST(FloatPhaseScale, TheTailIsKeptOutAndAHeavyTailEpochIsWithheld) {
    FloatPhaseScale est(fastSettings());
    std::vector<FloatPhaseScaleRow> rows = epochRows(0.2, "G12", { "G05", "G07", "G15", "G20", "G25",
                                                                    "G26", "G27", "G28", "G29", "G30", "G31" });
    rows[0].v = 0.01;   // w = 0.01 / sqrt(0.5e-6) = 14 beyond 3.29: G05 is in the tail (1 of 11 phase rows)
    est.observeEpoch(0.0, rows, {});
    EXPECT_EQ(est.counters().tail_rows, 1);
    EXPECT_EQ(est.counters().tail_enter, 1);
    EXPECT_EQ(est.tailSatellites(), (std::set<std::string>{ "G05" }));
    EXPECT_EQ(est.counters().values_pushed, 1);   // 1 of 11 is under the 10 % freeze share
    EXPECT_EQ(est.windowSize('G'), 1u);
    // Next epoch: clean, G05 leaves the tail; then an epoch with two of eleven in the tail is withheld.
    est.observeEpoch(30.0, epochRows(0.2, "G12", { "G05", "G07", "G15" }), {});
    EXPECT_EQ(est.counters().tail_leave, 1);
    std::vector<FloatPhaseScaleRow> heavy = epochRows(0.2, "G12", { "G05", "G07", "G15", "G20", "G25",
                                                                     "G26", "G27", "G28", "G29", "G30", "G31" });
    heavy[0].v = 0.01;
    heavy[2].v = -0.01;
    est.observeEpoch(60.0, heavy, {});
    EXPECT_EQ(est.counters().values_frozen, 1);
    EXPECT_EQ(est.windowSize('G'), 2u);
}

TEST(FloatPhaseScale, TheMapGivesTheFactorToFloatSatellitesAndCountsEachSwitch) {
    FloatPhaseScale est(fastSettings());
    for (int e = 0; e < 5; ++e) est.observeEpoch(30.0 * e, epochRows(0.16, "G12", { "G05", "G07", "G15" }), {});
    ASSERT_TRUE(est.formed('G'));
    // G12 and G05 held, E11 of an unformed constellation: G07 and G15 get the factor.
    auto map = est.scaleMap({ "G05", "G07", "G12", "G15", "E11", "G07" }, { "G12", "G05" });
    EXPECT_EQ(map, (std::map<std::string, double>{ { "G07", 0.16 }, { "G15", 0.16 } }));
    EXPECT_EQ(est.counters().to_bulk, 2);
    // G07 joins the held set: it switches back to the configured weight; G15 stays.
    map = est.scaleMap({ "G05", "G07", "G12", "G15" }, { "G12", "G05", "G07" });
    EXPECT_EQ(map, (std::map<std::string, double>{ { "G15", 0.16 } }));
    EXPECT_EQ(est.counters().to_present, 1);
    ASSERT_EQ(est.lastEdges().size(), 1u);
    EXPECT_EQ(est.lastEdges()[0], (std::pair<std::string, bool>{ "G07", false }));
    // G15 sets: leaving the map by absence is not a switch.
    map = est.scaleMap({ "G05", "G07", "G12" }, { "G12", "G05", "G07" });
    EXPECT_TRUE(map.empty());
    EXPECT_EQ(est.counters().to_present, 1);
    EXPECT_EQ(est.counters().scaled_sat_epochs, 3);
    EXPECT_NE(est.runEndLine().find("G k=0.400 (5 values)"), std::string::npos) << est.runEndLine();
}

TEST(FloatPhaseScale, ValuesOlderThanTheWindowLeaveIt) {
    FloatPhaseScaleSettings s = fastSettings();
    s.window_s = 100.0;
    FloatPhaseScale est(s);
    for (int e = 0; e < 8; ++e) est.observeEpoch(30.0 * e, epochRows(0.2, "G12", { "G05", "G07", "G15" }), {});
    EXPECT_EQ(est.windowSize('G'), 4u);   // tow 120 .. 210 within 100 s of 210
    EXPECT_FALSE(est.formed('G'));
}

} // namespace
