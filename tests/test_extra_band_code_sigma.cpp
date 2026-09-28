/**
 * @file test_extra_band_code_sigma.cpp
 * @brief Contract tests for `network.extra_band_code_sigma_scale` (card L5P,
 *        docs/refound/L5P_l5_phase_posture_2026-09-21.md knob B).
 *
 * In the network solve, the code rows of a signal whose band is neither band of
 * the constellation's ionosphere-free pin pair fold at sigma x scale.  With card
 * B5D armed, the extra-band code-bias column stays determined by those same rows,
 * so the wire keeps a band-5 code bias while the rows lose their pull on the
 * shared states.
 *
 * Covered here:
 *   (a) the pure extra-band predicate: band-5 signals are extra against a GPS
 *       L1/L2 pin, band-1 and band-2 signals never are, the tracking attribute
 *       is irrelevant, and no constellation is hard-coded;
 *   (b) an unresolved pin pair marks nothing as extra-band, so the weights stay
 *       as they were rather than being scaled on a guess;
 *   (c) the loader contract: 1.0 by default, a value below 1 reset to 1.0, a
 *       legal value parsed, and the closed-loop consumer pinned at 1.0 whatever
 *       the producer config sets.
 *
 * The weight arithmetic itself (`w /= scale * scale` on the code rows, phase rows
 * untouched) is graded on the arms by the `w` column of `network_pf_rows.csv` at
 * the first epoch, which gate G1 of the card compares against the base.
 */
#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "filter/NetworkExtraBandScope.hpp"
#include "logging/LogManager.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

using GnssProcessor::GnssConfig;
using GnssProcessor::LogManager;
using GnssProcessor::NetworkPinBands;
using GnssProcessor::networkSignalIsExtraBand;

namespace {

/// The GPS pin pair of the RF-0hv posture: the BIA clock reference is C1W/C2W.
constexpr NetworkPinBands kGps{ '1', '2' };
/// A Galileo-shaped pin pair: E1/E5a.
constexpr NetworkPinBands kGal{ '1', '5' };

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("l5p_eb_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

/// A minimal loadable PPP-RTK network config; @p network_extra splices into the
/// network block (the test_codebias_network_config.cpp shape).
std::string networkConfig(const std::string& network_extra) {
    return std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"AAAA\",\n"
        + network_extra +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

}  // namespace

// --------------------------------------------------------------------------
// (a) the pure predicate
// --------------------------------------------------------------------------

TEST(ExtraBandCodeSigma, BandFiveIsExtraAgainstAGpsPinPair) {
    EXPECT_TRUE(networkSignalIsExtraBand("C5Q", kGps));
    EXPECT_TRUE(networkSignalIsExtraBand("C5X", kGps));
    EXPECT_TRUE(networkSignalIsExtraBand("L5Q", kGps));   // the predicate is band-only
}

TEST(ExtraBandCodeSigma, ThePinBandsAreNeverExtraWhateverTheAttribute) {
    EXPECT_FALSE(networkSignalIsExtraBand("C1W", kGps));
    EXPECT_FALSE(networkSignalIsExtraBand("C1C", kGps));
    EXPECT_FALSE(networkSignalIsExtraBand("C2W", kGps));
    EXPECT_FALSE(networkSignalIsExtraBand("C2L", kGps));
    EXPECT_FALSE(networkSignalIsExtraBand("L1C", kGps));
    EXPECT_FALSE(networkSignalIsExtraBand("L2W", kGps));
}

TEST(ExtraBandCodeSigma, TheExtraBandFollowsTheConstellationsOwnPinPair) {
    // Band 5 is a pin band for Galileo and band 2 is the extra one: the inversion
    // that a hard-coded band 5 would get wrong.
    EXPECT_FALSE(networkSignalIsExtraBand("C5Q", kGal));
    EXPECT_TRUE(networkSignalIsExtraBand("C7Q", kGal));
    EXPECT_TRUE(networkSignalIsExtraBand("C2W", kGal));
}

// --------------------------------------------------------------------------
// (b) an unresolved pin pair changes nothing
// --------------------------------------------------------------------------

TEST(ExtraBandCodeSigma, UnresolvedPinMakesNothingExtraBand) {
    constexpr NetworkPinBands none{};
    constexpr NetworkPinBands same{ '1', '1' };   // degenerate, therefore invalid
    EXPECT_FALSE(none.valid());
    EXPECT_FALSE(same.valid());
    EXPECT_FALSE(networkSignalIsExtraBand("C5Q", none));
    EXPECT_FALSE(networkSignalIsExtraBand("C1W", none));
    EXPECT_FALSE(networkSignalIsExtraBand("C5Q", same));
}

TEST(ExtraBandCodeSigma, MalformedCodesAreNeverExtraBand) {
    EXPECT_FALSE(networkSignalIsExtraBand("", kGps));
    EXPECT_FALSE(networkSignalIsExtraBand("C", kGps));
}

// --------------------------------------------------------------------------
// (c) the loader contract
// --------------------------------------------------------------------------

TEST(ExtraBandCodeSigma, DefaultsToOneSoAnUnarmedRunIsUnscaled) {
    LogManager log;
    const fs::path p = writeTempConfig("default", networkConfig(""));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_DOUBLE_EQ(cfg.networkExtraBandCodeSigmaScale(), 1.0);
    std::error_code ec; fs::remove(p, ec);
}

TEST(ExtraBandCodeSigma, ParsesALegalScale) {
    LogManager log;
    const fs::path p = writeTempConfig("ten",
        networkConfig("    \"extra_band_code_sigma_scale\": 10.0,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_DOUBLE_EQ(cfg.networkExtraBandCodeSigmaScale(), 10.0);
    std::error_code ec; fs::remove(p, ec);
}

TEST(ExtraBandCodeSigma, RefusesAScaleBelowOne) {
    // The knob only deweights.  A value below 1 would weight an extra band above
    // the pinned pair, so the loader treats it as a typo and resets it to the
    // no-op default of 1.0.
    LogManager log;
    const fs::path p = writeTempConfig("low",
        networkConfig("    \"extra_band_code_sigma_scale\": 0.25,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_DOUBLE_EQ(cfg.networkExtraBandCodeSigmaScale(), 1.0);
    std::error_code ec; fs::remove(p, ec);
}

TEST(ExtraBandCodeSigma, ExactlyOneIsLegalAndIsTheNoOp) {
    LogManager log;
    const fs::path p = writeTempConfig("one",
        networkConfig("    \"extra_band_code_sigma_scale\": 1.0,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_DOUBLE_EQ(cfg.networkExtraBandCodeSigmaScale(), 1.0);
    std::error_code ec; fs::remove(p, ec);
}

TEST(ExtraBandCodeSigma, TheClosedLoopConsumerIsPinnedAtOne) {
    // The deweight is a producer-side lever on the network's normal equations;
    // the derived consumer is a full copy of the producer config and must not
    // deweight its own band-5 code as a side effect (card L5P section 1).
    LogManager log;
    const fs::path p = writeTempConfig("consumer",
        networkConfig("    \"extra_band_code_sigma_scale\": 10.0,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    ASSERT_DOUBLE_EQ(cfg.networkExtraBandCodeSigmaScale(), 10.0);
    const GnssConfig consumer = cfg.deriveClosedLoopConsumerConfig();
    EXPECT_DOUBLE_EQ(consumer.networkExtraBandCodeSigmaScale(), 1.0);
    std::error_code ec; fs::remove(p, ec);
}
