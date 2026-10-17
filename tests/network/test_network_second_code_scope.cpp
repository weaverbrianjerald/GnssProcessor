/**
 * @file test_network_second_code_scope.cpp
 * @brief Contract tests for the second code channel (
 *        docs/refound/SCC_second_code_channel_2026-09-21.md).
 *
 * With `network.second_code_channel` a network station contributes, as code-only
 * rows, every allow-listed band-1 code it reports besides the one its canonical
 * latch selected, so the C1C and C1W satellite code-bias families stop competing
 * for the same 59 stations.
 *
 * Covered here are the pure kernels and the loader, that is, everything that can
 * be tested without a network solve:
 *
 *   (a) the allow-list grammar: a 3-character code type on band 1, and nothing
 *       else.  This feature refuses a band-2 entry (it would make the
 *       `network.sat_isc_state` bridging roster non-empty for the first time),
 *       and a phase type belongs to a different feature;
 *   (b) companion selection: allow-list order, no duplicates, never the selected
 *       code itself, never a code the station does not report, and empty when the
 *       band has no selected code (that is the fallback case, not a companion);
 *   (c) the cell edges of standing invariant #7, including the case the invariant
 *       was written for: a station that drops out entirely closes its cells, so
 *       its return reads as a re-coupling rather than as a first sighting;
 *   (d) the receiver inter-signal key, with its negative parser pins.  The key
 *       must match none of `parseIfbKey`, `parseDcbKey` and
 *       `isGlonassPerSlotIfbKey`: `Application::networkSplitStation_` drops every
 *       receiver column whose key contains "_IFB", and the receiver code-bias
 *       zero-mean datum prior is gated on `parseDcbKey` and must not act on this
 *       state (R2: it carries no zero-mean prior);
 *   (e) the loader contract: default off, the default allow-list, a refused entry
 *       dropped and named, the sigma scale's floor of 1, the `carry_rx_code_bias`
 *       interlock, the disarm on fewer than two admitted codes, the subsumption
 *       by channel_mode `all`, and the closed-loop consumer
 *       pinned off.
 *
 * The mechanism itself (which rows enter the solve, which columns reach two
 * stations) is graded on the arms by gates G0 to G2.
 */
#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "network/assembly/SecondCodeScope.hpp"
#include "filter/StateKeySchema.hpp"
#include "logging/LogManager.hpp"

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <vector>
#include "../support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace fs = std::filesystem;

using GnssProcessor::GnssConfig;
using GnssProcessor::LogManager;
using GnssProcessor::secondCodeAllowEntryValid;
using GnssProcessor::secondCodeCellEdges;
using GnssProcessor::secondCodeCompanions;

namespace {

/// The default allow-list.
const std::vector<std::string> kDefaultList{ "C1C", "C1W" };

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = gnss_test::tempDir() / ("scc_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

/// A minimal loadable PPP-RTK network config; @p network_extra splices into the
/// network block (the test_extra_band_code_sigma.cpp shape).
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

/// The network block that satisfies the feature's interlocks, so a test can vary one
/// knob at a time without the loader disarming the feature for an unrelated reason.
/// `carry_rx_code_bias` itself needs `code_datum_time_linked`, which the loader
/// checks first; without it the loader forces the carry off.
const char* kArmable =
    "    \"second_code_channel\": true,\n"
    "    \"carry_rx_code_bias\": true,\n"
    "    \"code_datum_time_linked\": true,\n";

}  // namespace

// --------------------------------------------------------------------------
// (a) the allow-list grammar
// --------------------------------------------------------------------------

TEST(NetworkSecondCodeScope, AdmitsABandOneCodeType) {
    std::string why{ "unset" };
    EXPECT_TRUE(secondCodeAllowEntryValid("C1C", &why));
    EXPECT_TRUE(why.empty());
    EXPECT_TRUE(secondCodeAllowEntryValid("C1W", &why));
    EXPECT_TRUE(secondCodeAllowEntryValid("C1X", &why));   // admitted only if listed
    EXPECT_TRUE(secondCodeAllowEntryValid("C1L", &why));
    EXPECT_TRUE(secondCodeAllowEntryValid("C1P", nullptr));  // the out-param is optional
}

TEST(NetworkSecondCodeScope, RefusesAPhaseTypeBecauseThatIsADifferentCard) {
    std::string why;
    EXPECT_FALSE(secondCodeAllowEntryValid("L1C", &why));
    EXPECT_FALSE(why.empty());
    EXPECT_FALSE(secondCodeAllowEntryValid("S1C", &why));
    EXPECT_FALSE(secondCodeAllowEntryValid("D1C", &why));
}

TEST(NetworkSecondCodeScope, RefusesAnyBandOtherThanOne) {
    // This feature defers band 2; it is not unsupported in principle.  A band-2
    // entry would make the network.sat_isc_state bridging roster non-empty for
    // the first time.
    std::string why;
    EXPECT_FALSE(secondCodeAllowEntryValid("C2W", &why));
    EXPECT_FALSE(why.empty());
    EXPECT_FALSE(secondCodeAllowEntryValid("C2L", &why));
    EXPECT_FALSE(secondCodeAllowEntryValid("C5Q", &why));
}

TEST(NetworkSecondCodeScope, RefusesAnythingThatIsNotAThreeCharacterCode) {
    std::string why;
    EXPECT_FALSE(secondCodeAllowEntryValid("", &why));
    EXPECT_FALSE(secondCodeAllowEntryValid("C1", &why));
    EXPECT_FALSE(secondCodeAllowEntryValid("C1CX", &why));
    EXPECT_FALSE(secondCodeAllowEntryValid("c1c", &why));   // case is not normalized
}

// --------------------------------------------------------------------------
// (b) companion selection
// --------------------------------------------------------------------------

TEST(NetworkSecondCodeScope, PicksTheAllowListedCodeThatIsNotTheSelectedOne) {
    const std::set<std::string> reported{ "C1C", "C1W", "C2W" };
    EXPECT_EQ(secondCodeCompanions(reported, "C1C", kDefaultList),
              std::vector<std::string>({ "C1W" }));
    EXPECT_EQ(secondCodeCompanions(reported, "C1W", kDefaultList),
              std::vector<std::string>({ "C1C" }));
}

TEST(NetworkSecondCodeScope, AStationThatReportsOnlyItsCanonicalCodeContributesNothing) {
    // 18 of the 59 field stations are this case, and so is every simulated file,
    // so the sim bed needs the cloned second code (R9) to exercise the feature.
    const std::set<std::string> reported{ "C1C", "C2W" };
    EXPECT_TRUE(secondCodeCompanions(reported, "C1C", kDefaultList).empty());
}

TEST(NetworkSecondCodeScope, TheResultIsInAllowListOrderNotReceiverOrder) {
    // Determinism across stations and across runs: the added rows must not
    // depend on the order a receiver happens to write its observation types.
    const std::set<std::string> reported{ "C1C", "C1L", "C1W", "C1X" };
    const std::vector<std::string> list{ "C1C", "C1X", "C1W", "C1L" };
    EXPECT_EQ(secondCodeCompanions(reported, "C1C", list),
              std::vector<std::string>({ "C1X", "C1W", "C1L" }));
}

TEST(NetworkSecondCodeScope, ARepeatedAllowListEntryIsNotARepeatedRow) {
    const std::set<std::string> reported{ "C1C", "C1W" };
    const std::vector<std::string> list{ "C1C", "C1W", "C1W" };
    EXPECT_EQ(secondCodeCompanions(reported, "C1C", list),
              std::vector<std::string>({ "C1W" }));
}

TEST(NetworkSecondCodeScope, NoSelectedCodeMeansNoCompanionBecauseThatIsTheFallbackCase) {
    // A lone allow-listed code with no canonical beside it is the fallback case:
    // the receiver side still routes it through the inter-signal state (R2), but
    // it is not a companion and the run counts it separately.
    const std::set<std::string> reported{ "C1W", "C2W" };
    EXPECT_TRUE(secondCodeCompanions(reported, "", kDefaultList).empty());
}

TEST(NetworkSecondCodeScope, AnEmptyAllowListMakesTheMechanismInertRatherThanWide) {
    const std::set<std::string> reported{ "C1C", "C1W", "C1X" };
    EXPECT_TRUE(secondCodeCompanions(reported, "C1C", {}).empty());
}

// --------------------------------------------------------------------------
// (c) the cell edges (standing invariant #7)
// --------------------------------------------------------------------------

TEST(NetworkSecondCodeScope, ReportsBothEdgesOfTheOpenCellSet) {
    const std::set<std::string> prev{ "G01|C1W", "G02|C1W" };
    const std::set<std::string> cur{ "G02|C1W", "G03|C1W" };
    std::vector<std::string> opened, closed;
    secondCodeCellEdges(prev, cur, &opened, &closed);
    EXPECT_EQ(opened, std::vector<std::string>({ "G03|C1W" }));
    EXPECT_EQ(closed, std::vector<std::string>({ "G01|C1W" }));
}

TEST(NetworkSecondCodeScope, AStationDroppingOutClosesEveryCellItHeld) {
    const std::set<std::string> prev{ "G01|C1W", "G02|C1W", "G03|C1W" };
    std::vector<std::string> opened, closed;
    secondCodeCellEdges(prev, {}, &opened, &closed);
    EXPECT_TRUE(opened.empty());
    EXPECT_EQ(closed.size(), 3u);
}

TEST(NetworkSecondCodeScope, AReturnAfterADropoutIsAnOpeningNotSilence) {
    // The failure standing invariant #7 names: a station with zero rows drops
    // out of the map, and its return must read as a re-coupling edge.
    std::vector<std::string> opened, closed;
    secondCodeCellEdges({}, { "G01|C1W" }, &opened, &closed);
    EXPECT_EQ(opened, std::vector<std::string>({ "G01|C1W" }));
    EXPECT_TRUE(closed.empty());
}

TEST(NetworkSecondCodeScope, AnUnchangedCellSetHasNoEdges) {
    const std::set<std::string> same{ "G01|C1W", "G02|C1W" };
    std::vector<std::string> opened{ "stale" }, closed{ "stale" };
    secondCodeCellEdges(same, same, &opened, &closed);
    EXPECT_TRUE(opened.empty());    // the outputs are cleared, not appended to
    EXPECT_TRUE(closed.empty());
}

TEST(NetworkSecondCodeScope, EitherEdgeOutputMayBeNull) {
    std::vector<std::string> opened;
    secondCodeCellEdges({ "G01|C1W" }, { "G02|C1W" }, &opened, nullptr);
    EXPECT_EQ(opened, std::vector<std::string>({ "G02|C1W" }));
    std::vector<std::string> closed;
    secondCodeCellEdges({ "G01|C1W" }, { "G02|C1W" }, nullptr, &closed);
    EXPECT_EQ(closed, std::vector<std::string>({ "G01|C1W" }));
}

// --------------------------------------------------------------------------
// (d) the receiver inter-signal key, and its negative parser pins
// --------------------------------------------------------------------------

TEST(NetworkSecondCodeScope, TheIscKeyRoundTrips) {
    namespace K = GnssProcessor::StateKeys;
    const std::string key = K::makeReceiverIscKey("GPS", "C1W");
    EXPECT_EQ(key, "GPS_ISC_C1W");
    EXPECT_TRUE(K::isReceiverIscKey(key));
    const auto parts = K::parseReceiverIscKey(key);
    ASSERT_TRUE(parts.has_value());
    EXPECT_EQ(parts->system, "GPS");
    EXPECT_EQ(parts->signal, "C1W");
}

TEST(NetworkSecondCodeScope, TheIscKeyIsRefusedByEveryOtherReceiverBiasParser) {
    // networkSplitStation_ drops every receiver column whose key contains
    // "_IFB", and the receiver code-bias zero-mean datum prior is gated on
    // parseDcbKey.  The inter-signal state must survive the first and be skipped
    // by the second (R2); the rest of the feature depends on this pin.
    namespace K = GnssProcessor::StateKeys;
    const std::string key = K::makeReceiverIscKey("GPS", "C1W");
    EXPECT_EQ(key.find("_IFB"), std::string::npos);
    EXPECT_FALSE(K::parseIfbKey(key).has_value());
    EXPECT_FALSE(K::parseDcbKey(key).has_value());
    EXPECT_FALSE(K::isGlonassPerSlotIfbKey(key));
    EXPECT_FALSE(K::parseGlonassPerSlotIfbKey(key).has_value());
    EXPECT_FALSE(K::isReceiverPhaseBiasKey(key));
}

TEST(NetworkSecondCodeScope, TheOtherReceiverBiasKeysAreRefusedByTheIscParser) {
    // The converse: no existing receiver code-bias key may be mistaken for an
    // inter-signal state, or the R5 weight scale and the R7 census would act on
    // the wrong columns.
    namespace K = GnssProcessor::StateKeys;
    for (const std::string& k : { K::makeDcbKey("GPS"),
                                  K::makeIfbKey("GPS", '5'),
                                  K::makeGlonassPerSlotIfbKey("GLONASS", '1', -4),
                                  K::makeReceiverPhaseBiasKey("GPS", "L1C") }) {
        EXPECT_FALSE(K::isReceiverIscKey(k)) << k;
        EXPECT_FALSE(K::parseReceiverIscKey(k).has_value()) << k;
    }
}

TEST(NetworkSecondCodeScope, AMalformedIscKeyIsRejectedRatherThanHalfParsed) {
    namespace K = GnssProcessor::StateKeys;
    EXPECT_FALSE(K::isReceiverIscKey("_ISC_C1W"));     // no system
    EXPECT_FALSE(K::isReceiverIscKey("GPS_ISC_"));     // no signal
    EXPECT_FALSE(K::isReceiverIscKey("GPS_ISC"));      // not the infix form
    EXPECT_FALSE(K::isReceiverIscKey("ISC"));
    EXPECT_FALSE(K::parseReceiverIscKey("_ISC_C1W").has_value());
    EXPECT_FALSE(K::parseReceiverIscKey("GPS_ISC_").has_value());
}

// --------------------------------------------------------------------------
// (e) the loader contract
// --------------------------------------------------------------------------

TEST(NetworkSecondCodeScope, DefaultsToOffWithTheDefaultAllowList) {
    LogManager log;
    const fs::path p = writeTempConfig("default", networkConfig(""));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(cfg.networkSecondCodeChannel());
    EXPECT_EQ(cfg.networkSecondCodeSignals(), kDefaultList);
    EXPECT_DOUBLE_EQ(cfg.networkSecondCodeSigmaScale(), 1.0);
    std::error_code ec; fs::remove(p, ec);
}

TEST(NetworkSecondCodeScope, ArmsWhenBothInterlocksAreSatisfied) {
    LogManager log;
    const fs::path p = writeTempConfig("armed", networkConfig(kArmable));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.networkSecondCodeChannel());
    std::error_code ec; fs::remove(p, ec);
}

TEST(NetworkSecondCodeScope, IsDisarmedWithoutTheReceiverCodeBiasCarry) {
    // The inter-signal state is a receiver code bias like any other and has to
    // be restored across epochs, or it cold-starts every epoch and determines
    // itself from one epoch of code (R2).
    LogManager log;
    const fs::path p = writeTempConfig("nocarry",
        networkConfig("    \"second_code_channel\": true,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(cfg.networkSecondCodeChannel());
    std::error_code ec; fs::remove(p, ec);
}

TEST(NetworkSecondCodeScope, IsSubsumedByChannelModeAllRatherThanRefused) {
    // Under channel_mode `all` the second code channel is subsumed rather than refused.
    // This posture is the leveling vehicle, so the widening's own
    // interlock clamps the mode back to single and the feature is then armed in its
    // ordinary form; the two assertions below cover that contract.
    LogManager log;
    const fs::path p = writeTempConfig("chanall",
        std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"channel_mode\": \"all\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"AAAA\",\n"
        + std::string(kArmable) +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n");
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(cfg.networkChannelModeAll());     // clamped: the leveling vehicle
    EXPECT_TRUE(cfg.networkSecondCodeChannel());   // armed, not disarmed
    std::error_code ec; fs::remove(p, ec);
}

TEST(NetworkSecondCodeScope, DropsARefusedAllowListEntryAndKeepsTheRest) {
    LogManager log;
    const fs::path p = writeTempConfig("list",
        networkConfig(std::string(kArmable) +
                      "    \"second_code_signals\": [ \"C1C\", \"C2W\", \"L1C\", \"C1W\" ],\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_EQ(cfg.networkSecondCodeSignals(), kDefaultList);   // C2W and L1C dropped
    EXPECT_TRUE(cfg.networkSecondCodeChannel());
    std::error_code ec; fs::remove(p, ec);
}

TEST(NetworkSecondCodeScope, AListWithFewerThanTwoAdmittedCodesDisarmsTheCard) {
    // A "second" code needs at least two band-1 codes in the list to ever be one;
    // arming on a one-entry list would only ever fold the fallback path.
    LogManager log;
    const fs::path p = writeTempConfig("thin",
        networkConfig(std::string(kArmable) +
                      "    \"second_code_signals\": [ \"C1C\", \"C2W\" ],\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_EQ(cfg.networkSecondCodeSignals(), std::vector<std::string>({ "C1C" }));
    EXPECT_FALSE(cfg.networkSecondCodeChannel());
    std::error_code ec; fs::remove(p, ec);
}

TEST(NetworkSecondCodeScope, AdmitsAnExplicitlyListedExtraAttribute) {
    LogManager log;
    const fs::path p = writeTempConfig("x",
        networkConfig(std::string(kArmable) +
                      "    \"second_code_signals\": [ \"C1C\", \"C1W\", \"C1X\" ],\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_EQ(cfg.networkSecondCodeSignals(),
              std::vector<std::string>({ "C1C", "C1W", "C1X" }));
    std::error_code ec; fs::remove(p, ec);
}

TEST(NetworkSecondCodeScope, RefusesASigmaScaleBelowOne) {
    LogManager log;
    const fs::path p = writeTempConfig("lowscale",
        networkConfig(std::string(kArmable) +
                      "    \"second_code_sigma_scale\": 0.25,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_DOUBLE_EQ(cfg.networkSecondCodeSigmaScale(), 1.0);
    std::error_code ec; fs::remove(p, ec);
}

TEST(NetworkSecondCodeScope, ParsesALegalSigmaScale) {
    LogManager log;
    const fs::path p = writeTempConfig("scale10",
        networkConfig(std::string(kArmable) +
                      "    \"second_code_sigma_scale\": 10.0,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_DOUBLE_EQ(cfg.networkSecondCodeSigmaScale(), 10.0);
    std::error_code ec; fs::remove(p, ec);
}

TEST(NetworkSecondCodeScope, TheClosedLoopConsumerIsPinnedOff) {
    // The feature is a producer-side mechanism: it exists so the satellite families
    // share the network's stations, and a consumer is one station with no family
    // to support (R6).
    LogManager log;
    const fs::path p = writeTempConfig("consumer",
        networkConfig(std::string(kArmable) +
                      "    \"second_code_sigma_scale\": 3.0,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    ASSERT_TRUE(cfg.networkSecondCodeChannel());
    const GnssConfig consumer = cfg.deriveClosedLoopConsumerConfig();
    EXPECT_FALSE(consumer.networkSecondCodeChannel());
    EXPECT_DOUBLE_EQ(consumer.networkSecondCodeSigmaScale(), 1.0);
    std::error_code ec; fs::remove(p, ec);
}
