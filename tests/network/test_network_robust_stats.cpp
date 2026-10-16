/**
 * @file test_network_robust_stats.cpp
 * @brief Unit tests for the network run loop's pure robust-stats and slip-QC
 *        helpers in network/qc/RobustStats.hpp and network/slip/SlipQc.hpp
 *        (moved verbatim out of ApplicationNetwork.cpp at decomposition INC-2).
 */

#include <gtest/gtest.h>

#include "network/qc/RobustStats.hpp"
#include "network/slip/SlipQc.hpp"

#include <cmath>
#include <map>
#include <string>
#include <tuple>
#include <vector>

using namespace GnssProcessor;

// ---------------------------------------------------------------------------
// medianCopy
// ---------------------------------------------------------------------------

TEST(NetworkRobustStats, MedianCopyOddCount) {
    EXPECT_DOUBLE_EQ(medianCopy({3.0, 1.0, 2.0}), 2.0);
}

TEST(NetworkRobustStats, MedianCopyEvenCountTakesUpperMiddle) {
    // File convention: upper-middle for even counts (nth_element at size/2).
    EXPECT_DOUBLE_EQ(medianCopy({1.0, 2.0, 3.0, 4.0}), 3.0);
}

TEST(NetworkRobustStats, MedianCopyEmptyReturnsZero) {
    EXPECT_DOUBLE_EQ(medianCopy({}), 0.0);
}

TEST(NetworkRobustStats, MedianCopySingleElement) {
    EXPECT_DOUBLE_EQ(medianCopy({42.5}), 42.5);
}

// ---------------------------------------------------------------------------
// robustProminence
// ---------------------------------------------------------------------------

TEST(NetworkRobustStats, RobustProminenceFlagsTheSpike) {
    // Nine near-identical effects and one spike: the spike's z must stand far
    // above the crowd's, and the crowd must sit near zero.
    std::map<std::string, double> eff;
    for (int i = 0; i < 9; ++i)
        eff["S" + std::to_string(i)] = 0.1 * i;   // 0.0 .. 0.8
    eff["SPIKE"] = 50.0;
    const auto z = robustProminence(eff);
    EXPECT_GT(z.at("SPIKE"), 10.0);
    for (int i = 0; i < 9; ++i)
        EXPECT_LT(std::abs(z.at("S" + std::to_string(i))), 3.0);
}

TEST(NetworkRobustStats, RobustProminenceDegenerateMadFallsBackToUnit) {
    // All-identical effects => MAD == 0 => guarded fallback MAD = 1, so every
    // z is exactly (v - median) / 1 = 0.  No NaN/inf.
    std::map<int, double> eff{{1, 5.0}, {2, 5.0}, {3, 5.0}};
    const auto z = robustProminence(eff);
    for (const auto& kv : z) {
        EXPECT_TRUE(std::isfinite(kv.second));
        EXPECT_DOUBLE_EQ(kv.second, 0.0);
    }
}

// ---------------------------------------------------------------------------
// medianPolish2Way
// ---------------------------------------------------------------------------

TEST(NetworkRobustStats, MedianPolishRecoversAdditiveTable) {
    // Exact additive table: value = overall + row_eff + col_eff, zero
    // interaction.  Polish must reproduce each cell's fit and leave the
    // interaction at (numerically) zero.
    const double overall = 10.0;
    const std::map<int, double> row{{0, -1.0}, {1, 0.0}, {2, 1.0}};
    const std::map<std::string, double> col{{"G01", -2.0}, {"G02", 0.0}, {"G03", 2.0}};
    std::vector<std::tuple<int, std::string, double>> cells;
    for (const auto& r : row)
        for (const auto& c : col)
            cells.emplace_back(r.first, c.first, overall + r.second + c.second);
    const MedianPolish mp = medianPolish2Way(cells);
    ASSERT_EQ(mp.interaction.size(), cells.size());
    for (std::size_t k = 0; k < cells.size(); ++k) {
        EXPECT_NEAR(mp.interaction[k], 0.0, 1e-12) << "cell " << k;
        const double fit = mp.overall + mp.row_eff.at(std::get<0>(cells[k]))
                         + mp.col_eff.at(std::get<1>(cells[k]));
        EXPECT_NEAR(fit, std::get<2>(cells[k]), 1e-12) << "cell " << k;
    }
}

TEST(NetworkRobustStats, MedianPolishIsolatesSingleCellOutlier) {
    // Flat table + one huge cell: the outlier must land in the interaction
    // term and leave the row/column effects at zero, since medians ignore it.
    std::vector<std::tuple<int, std::string, double>> cells;
    std::size_t outlier_idx = 0;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            const std::string col = "G0" + std::to_string(c + 1);
            double v = 1.0;
            if (r == 2 && c == 2) v = 100.0;
            if (r == 2 && c == 2) outlier_idx = cells.size();
            cells.emplace_back(r, col, v);
        }
    const MedianPolish mp = medianPolish2Way(cells);
    EXPECT_NEAR(mp.interaction[outlier_idx], 99.0, 1.0);
    for (const auto& kv : mp.row_eff) EXPECT_NEAR(kv.second, 0.0, 1e-9);
    for (const auto& kv : mp.col_eff) EXPECT_NEAR(kv.second, 0.0, 1e-9);
}

// ---------------------------------------------------------------------------
// ifElevMap
// ---------------------------------------------------------------------------

TEST(NetworkSlipQcHelpers, IfElevMapZenithIsUnity) {
    EXPECT_NEAR(ifElevMap(90.0), 1.0, 1e-12);
}

TEST(NetworkSlipQcHelpers, IfElevMapMonotoneTowardHorizonAndClamped) {
    EXPECT_GT(ifElevMap(10.0), ifElevMap(30.0));
    EXPECT_GT(ifElevMap(30.0), ifElevMap(60.0));
    // Below the 5-degree clamp everything maps like 5 degrees.
    EXPECT_DOUBLE_EQ(ifElevMap(2.0), ifElevMap(5.0));
    EXPECT_DOUBLE_EQ(ifElevMap(-10.0), ifElevMap(5.0));
}

// ---------------------------------------------------------------------------
// parseIfPairResetKey
// ---------------------------------------------------------------------------

TEST(NetworkSlipQcHelpers, ParseIfPairResetKeyWellFormed) {
    std::string sat, s1, s2;
    ASSERT_TRUE(parseIfPairResetKey("G01_L1C_L2W", sat, s1, s2));
    EXPECT_EQ(sat, "G01");
    EXPECT_EQ(s1, "L1C");
    EXPECT_EQ(s2, "L2W");
}

TEST(NetworkSlipQcHelpers, ParseIfPairResetKeyMalformedRejected) {
    std::string sat, s1, s2;
    EXPECT_FALSE(parseIfPairResetKey("", sat, s1, s2));
    EXPECT_FALSE(parseIfPairResetKey("G01", sat, s1, s2));           // no pair
    EXPECT_FALSE(parseIfPairResetKey("G01_L1C", sat, s1, s2));       // one signal
    EXPECT_FALSE(parseIfPairResetKey("_L1C_L2W", sat, s1, s2));      // empty sat
    EXPECT_FALSE(parseIfPairResetKey("G01__L2W", sat, s1, s2));      // empty sig1
    EXPECT_FALSE(parseIfPairResetKey("G01_L1C_", sat, s1, s2));      // empty sig2
}
