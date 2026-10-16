// Unit tests for the run-end counters of KalmanFilter's post-fit QC events:
// the counter rows are a complete declared table, and the run-end block prints one
// row per counter with its count.

#include "filter/KalmanQcCounters.hpp"
#include "filter/KalmanPostFitQc.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

TEST(KalmanQcCounters, EveryCounterHasExactlyOneRow) {
    constexpr std::size_t n_rows = std::size(kKalmanQcCounterRows);
    // Every member of KalmanQcCounters is a std::size_t counter, so a member without a row
    // (or a row without a member) breaks this equality.
    EXPECT_EQ(sizeof(KalmanQcCounters), n_rows * sizeof(std::size_t));
    for (std::size_t i = 0; i < n_rows; ++i) {
        EXPECT_LE(std::strlen(kKalmanQcCounterRows[i].label), 30u) << kKalmanQcCounterRows[i].label;
        for (std::size_t j = i + 1; j < n_rows; ++j)
            EXPECT_FALSE(kKalmanQcCounterRows[i].member == kKalmanQcCounterRows[j].member)
                << kKalmanQcCounterRows[i].label << " and " << kKalmanQcCounterRows[j].label;
    }
}

TEST(KalmanQcCounters, RunEndBlockPrintsOneRowPerCounter) {
    KalmanQcCounters c;
    c.pivot_group_slips = 3;
    c.complete_resets = 1;
    c.stuck_bias_resets = 12;

    std::istringstream in(formatKalmanQcCounters(c));
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) lines.push_back(line);

    ASSERT_EQ(lines.size(), 1 + std::size(kKalmanQcCounterRows));
    EXPECT_EQ(lines[0], "KF QC event counters (post-fit screen and its responses):");
    EXPECT_EQ(lines[1], "  pivot group slips (phase)      : 3");
    EXPECT_EQ(lines[2], "  pivot code disturbances        : 0");
    EXPECT_EQ(lines[10], "  complete resets                : 1");
    EXPECT_EQ(lines[11], "  stuck code-bias resets         : 12");
}

TEST(KalmanQcCounters, PivotDetectorRowsNameTheirCounter) {
    for (const kalman::PivotDetectorSpec& spec : kalman::kPivotDetectors) {
        if (spec.obs_type == 'L')
            EXPECT_TRUE(spec.counter == &KalmanQcCounters::pivot_group_slips) << spec.label;
        else
            EXPECT_TRUE(spec.counter == &KalmanQcCounters::pivot_code_disturbances) << spec.label;
    }
    KalmanQcCounters c;
    ++(c.*kalman::kPivotDetectors[2].counter);
    EXPECT_EQ(c.pivot_code_disturbances, 1u);
    EXPECT_EQ(c.pivot_group_slips, 0u);
}

}  // namespace
}  // namespace GnssProcessor
