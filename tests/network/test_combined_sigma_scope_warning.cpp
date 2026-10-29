/**
 * @file test_combined_sigma_scope_warning.cpp
 * @brief The loader warns when the consumer folds the combined-correction sigma while it
 *        also takes the network clock.
 *
 * The combined sigma (network.closed_loop.consume_combined_sigma, M4 contract v2) is
 * validated only for a consumer that takes the biases alone.  On the field and sim goldens,
 * whose consumer takes the network clock, arming it moved the held-out consumer's hour-1
 * p50 from 57 mm to 1.47 m and from 99 mm to 0.60 m.  The loader keeps loading such a
 * config (seam-control replays pair the two on purpose) and says so in a warning.
 */

#include <gtest/gtest.h>

#include "ConfigLoadCapture.hpp"

using namespace GnssProcessor;
using TestSupport::contains;
using TestSupport::loadNetworkConfigCapturing;

namespace {
const char* kWarning = "consume_combined_sigma is on with consume_network_clock";
}  // namespace

TEST(CombinedSigmaScopeTest, WarnsOnANetworkClockConsumer) {
    const auto r = loadNetworkConfigCapturing(
        "csig_clock", "    \"closed_loop\": { \"consume_combined_sigma\": true },\n");
    ASSERT_TRUE(r.ok) << "the combination is warned, not refused";
    EXPECT_TRUE(r.config.getNetworkConfig().closed_loop.consume_combined_sigma);
    EXPECT_TRUE(r.config.getNetworkConfig().closed_loop.consume_network_clock);
    EXPECT_TRUE(contains(r.text, kWarning)) << r.text;
}

TEST(CombinedSigmaScopeTest, SilentOnABiasOnlyConsumer) {
    const auto r = loadNetworkConfigCapturing(
        "csig_bias_only",
        "    \"closed_loop\": { \"consume_combined_sigma\": true, \"consume_network_clock\": false },\n");
    ASSERT_TRUE(r.ok);
    EXPECT_FALSE(contains(r.text, kWarning)) << r.text;
}

TEST(CombinedSigmaScopeTest, SilentWhenTheSigmaIsNotConsumed) {
    const auto r = loadNetworkConfigCapturing(
        "csig_off", "    \"closed_loop\": { \"consume_network_clock\": true },\n");
    ASSERT_TRUE(r.ok);
    EXPECT_FALSE(contains(r.text, kWarning)) << r.text;
}
