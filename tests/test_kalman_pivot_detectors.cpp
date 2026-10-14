// Unit tests for the coherent-offset pivot detectors of KalmanFilter's post-fit screen
// (card UOM step U2d): the detector table kalman::kPivotDetectors and the pure test it
// shares, kalman::testCoherentOffset, with its median helper.
//
//   Table:    the four rows, in the order they run, with the parameters the former four
//             detector loops hard-coded.
//   Median:   odd and even counts.
//   Criteria: a coherent group fires; each of the four criteria (rows, bands for a pool,
//             fraction over the threshold, median floor, MAD ratio) stops a group that
//             misses it, and the statistics stop where the test stopped.

#include "filter/KalmanPostFitQc.hpp"

#include <gtest/gtest.h>

#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

using kalman::CoherentOffset;
using kalman::PivotDetectorSpec;
using kalman::PivotRowGroup;
using kalman::kPivotDetectors;
using kalman::medianInPlace;
using kalman::testCoherentOffset;

const PivotDetectorSpec& phaseSignal() { return kPivotDetectors[0]; }
const PivotDetectorSpec& phasePool()   { return kPivotDetectors[1]; }
const PivotDetectorSpec& codeSignal()  { return kPivotDetectors[2]; }

/// A group of @p residuals whose first @p above rows are over the per-row threshold.
PivotRowGroup group(const std::vector<double>& residuals, std::size_t above,
                    std::set<std::string> bands = {}) {
    PivotRowGroup g;
    for (std::size_t i = 0; i < residuals.size(); ++i) {
        g.indices.push_back(i);
        g.residuals.push_back(residuals[i]);
    }
    g.above = above;
    g.bands = std::move(bands);
    return g;
}

TEST(KalmanPivotDetectors, TableHoldsTheFourDetectorsInRunOrder) {
    ASSERT_EQ(std::size(kPivotDetectors), 4u);

    EXPECT_STREQ(kPivotDetectors[0].label, "phase per signal");
    EXPECT_EQ(kPivotDetectors[0].obs_type, 'L');
    EXPECT_FALSE(kPivotDetectors[0].pooled);
    EXPECT_EQ(kPivotDetectors[0].min_rows, 4u);
    EXPECT_DOUBLE_EQ(kPivotDetectors[0].median_floor_m, 0.005);
    EXPECT_TRUE(kPivotDetectors[0].wavelength_floor);
    EXPECT_DOUBLE_EQ(kPivotDetectors[0].tight_cluster_mad_ratio, 0.2);
    EXPECT_FALSE(kPivotDetectors[0].reinflate_iono);

    EXPECT_STREQ(kPivotDetectors[1].label, "phase cross-band");
    EXPECT_EQ(kPivotDetectors[1].obs_type, 'L');
    EXPECT_TRUE(kPivotDetectors[1].pooled);
    EXPECT_EQ(kPivotDetectors[1].min_rows, 3u);
    EXPECT_EQ(kPivotDetectors[1].min_bands, 2u);
    EXPECT_FALSE(kPivotDetectors[1].wavelength_floor);

    EXPECT_STREQ(kPivotDetectors[2].label, "code per signal");
    EXPECT_EQ(kPivotDetectors[2].obs_type, 'C');
    EXPECT_FALSE(kPivotDetectors[2].pooled);
    EXPECT_DOUBLE_EQ(kPivotDetectors[2].median_floor_m, 0.30);
    EXPECT_TRUE(kPivotDetectors[2].reinflate_iono);

    EXPECT_STREQ(kPivotDetectors[3].label, "code cross-band");
    EXPECT_EQ(kPivotDetectors[3].obs_type, 'C');
    EXPECT_TRUE(kPivotDetectors[3].pooled);
    EXPECT_TRUE(kPivotDetectors[3].reinflate_iono);

    for (const PivotDetectorSpec& spec : kPivotDetectors) {
        EXPECT_DOUBLE_EQ(spec.above_fraction_min, 0.5) << spec.label;
        EXPECT_DOUBLE_EQ(spec.mad_ratio_max, 0.4) << spec.label;
    }
}

TEST(KalmanPivotDetectors, MedianOfOddAndEvenCounts) {
    std::vector<double> odd{ 3.0, -1.0, 2.0 };
    EXPECT_DOUBLE_EQ(medianInPlace(odd), 2.0);
    std::vector<double> even{ 4.0, 1.0, 3.0, 2.0 };
    EXPECT_DOUBLE_EQ(medianInPlace(even), 2.5);
}

TEST(KalmanPivotDetectors, CoherentGroupFires) {
    const CoherentOffset t =
        testCoherentOffset(phaseSignal(), group({ 0.100, 0.101, 0.099, 0.100, 0.102 }, 5));
    EXPECT_TRUE(t.coherent);
    EXPECT_EQ(t.n, 5u);
    EXPECT_DOUBLE_EQ(t.frac, 1.0);
    EXPECT_DOUBLE_EQ(t.med, 0.100);
    EXPECT_NEAR(t.mad, 0.001, 1e-12);
    EXPECT_NEAR(t.mad_ratio, 0.01, 1e-9);
}

TEST(KalmanPivotDetectors, TooFewRowsStopsBeforeTheFraction) {
    const CoherentOffset t = testCoherentOffset(phaseSignal(), group({ 0.1, 0.1, 0.1 }, 3));
    EXPECT_FALSE(t.coherent);
    EXPECT_EQ(t.n, 3u);
    EXPECT_DOUBLE_EQ(t.frac, 0.0);   // not computed
}

TEST(KalmanPivotDetectors, FractionUnderTheMinimumStopsBeforeTheMedian) {
    const CoherentOffset t = testCoherentOffset(phaseSignal(), group({ 0.1, 0.1, 0.1, 0.1 }, 1));
    EXPECT_FALSE(t.coherent);
    EXPECT_DOUBLE_EQ(t.frac, 0.25);
    EXPECT_DOUBLE_EQ(t.med, 0.0);    // not computed
}

TEST(KalmanPivotDetectors, MedianFloorSeparatesPhaseFromCode) {
    // A tight 0.20 m offset is a phase pivot offset but under the code row's 0.30 m floor.
    const PivotRowGroup g = group({ 0.20, 0.20, 0.21, 0.19 }, 4);
    EXPECT_TRUE(testCoherentOffset(phaseSignal(), g).coherent);
    const CoherentOffset code = testCoherentOffset(codeSignal(), g);
    EXPECT_FALSE(code.coherent);
    EXPECT_DOUBLE_EQ(code.med, 0.20);
    EXPECT_DOUBLE_EQ(code.mad, 0.0);  // not computed

    // A 4 mm phase median is under the 5 mm floor.
    EXPECT_FALSE(testCoherentOffset(phaseSignal(), group({ 0.004, 0.004, 0.004, 0.004 }, 4)).coherent);
}

TEST(KalmanPivotDetectors, LooseSpreadIsNotCoherent) {
    // Median 0.2 m, MAD 0.2 m: ratio 1.0 > 0.4, independent outliers.
    const CoherentOffset t = testCoherentOffset(phaseSignal(), group({ 0.1, 0.3, -0.1, 0.5 }, 4));
    EXPECT_FALSE(t.coherent);
    EXPECT_DOUBLE_EQ(t.med, 0.2);
    EXPECT_NEAR(t.mad, 0.2, 1e-12);
    EXPECT_NEAR(t.mad_ratio, 1.0, 1e-12);
}

TEST(KalmanPivotDetectors, PoolNeedsTwoBands) {
    const std::vector<double> r{ 0.05, 0.05, 0.051 };
    EXPECT_FALSE(testCoherentOffset(phasePool(), group(r, 3, { "L1" })).coherent);
    EXPECT_TRUE(testCoherentOffset(phasePool(), group(r, 3, { "L1", "L2" })).coherent);
    // The per-signal row ignores bands and needs four rows.
    EXPECT_FALSE(testCoherentOffset(phaseSignal(), group(r, 3, { "L1", "L2" })).coherent);
}

}  // namespace
}  // namespace GnssProcessor
