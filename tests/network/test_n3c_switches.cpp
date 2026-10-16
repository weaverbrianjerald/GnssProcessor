/**
 * @file test_n3c_switches.cpp
 * @brief The signal-role switches (network.n3c_*): each site's answer with its switch on,
 *        and the default (switch off) answer unchanged.  Suite N3cSwitchesTest.
 */
#include <gtest/gtest.h>

#include "filter/StateKeySchema.hpp"
#include "network/solve/DatumGraph.hpp"
#include "network/solve/Sbasis.hpp"

#include <cmath>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {
namespace {

using Pair = std::pair<std::string, std::string>;

/// GPS carrier frequency by RINEX band digit; an unknown band reads as unavailable.
double gpsFreq(const std::string& s) {
    if (s.size() < 2) return 0.0;
    switch (s[1]) {
        case '1': return 1575.42e6;
        case '2': return 1227.60e6;
        case '5': return 1176.45e6;
        default:  return 0.0;
    }
}

// ---- D1: the pin pair is the bias product's clock-reference pair ----

TEST(N3cSwitchesTest, PinPairFollowsTheReferencePairWhenBothSignalsAreRegistered) {
    const std::vector<std::string> sigs{ "C1C", "C1W", "C2W" };
    // Switch off (no reference): the lexically first pair.
    EXPECT_EQ(NetworkSbasis::selectIfPinPair(sigs, gpsFreq), (Pair{ "C1C", "C2W" }));
    // Switch on: the clock-reference pair, whatever the input order.
    const Pair ref{ "C1W", "C2W" };
    EXPECT_EQ(NetworkSbasis::selectIfPinPair(sigs, gpsFreq, ref), ref);
    EXPECT_EQ(NetworkSbasis::selectIfPinPair({ "C2W", "C1W", "C1C" }, gpsFreq, ref), ref);
}

TEST(N3cSwitchesTest, PinPairKeepsTheLexicalChoiceWhereTheReferenceCannotBeFormed) {
    const Pair ref{ "C1W", "C2W" };
    // The satellite registers no C1W: the reference pair is not available.
    EXPECT_EQ(NetworkSbasis::selectIfPinPair({ "C1C", "C2W" }, gpsFreq, ref),
              (Pair{ "C1C", "C2W" }));
    // Both reference signals on one frequency (a degenerate reference): ignored.
    EXPECT_EQ(NetworkSbasis::selectIfPinPair({ "C1C", "C1W", "C2W" }, gpsFreq,
                                             Pair{ "C1C", "C1W" }),
              (Pair{ "C1C", "C2W" }));
    // An empty reference is the switch off.
    EXPECT_EQ(NetworkSbasis::selectIfPinPair({ "C1C", "C1W", "C2W" }, gpsFreq, Pair{}),
              (Pair{ "C1C", "C2W" }));
}

// ---- D2: one channel-family definition, "X against the rest" ----

TEST(N3cSwitchesTest, ChannelFamilyIsXAgainstTheRestWhenTheSwitchIsOn) {
    // Default (the attribute letter): C1C and C5Q are two families on Galileo.
    EXPECT_NE(netDatumGraphFamilyOf("E03", "C1C", false), netDatumGraphFamilyOf("E03", "C5Q", false));
    EXPECT_EQ(netDatumGraphFamilyOf("E03", "C1C", false), netDatumGraphFamily("C1C"));
    // Switch on: they are one family, and the combined I+Q signals are the other.
    EXPECT_EQ(netDatumGraphFamilyOf("E03", "C1C", true), netDatumGraphFamilyOf("E03", "C5Q", true));
    EXPECT_EQ(netDatumGraphFamilyOf("E03", "C1C", true), kDatumGraphFamilyRest);
    EXPECT_EQ(netDatumGraphFamilyOf("E03", "L5X", true), kDatumGraphFamilyX);
    // GPS C1C and C2W are one family by the rule, two by the letter.
    EXPECT_NE(netDatumGraphFamilyOf("G05", "C1C", false), netDatumGraphFamilyOf("G05", "C2W", false));
    EXPECT_EQ(netDatumGraphFamilyOf("G05", "C1C", true), netDatumGraphFamilyOf("G05", "C2W", true));
    // A code with no attribute has no family under either definition.
    EXPECT_EQ(netDatumGraphFamilyOf("E03", "C1", true), kDatumGraphNoFamily);
}

TEST(N3cSwitchesTest, ChannelFamilyCellsAndDirectionIdsFollowTheDefinition) {
    const std::set<std::string> pb{ StateKeys::makeSatPhaseBiasKey("E03", "L1C"),
                                 StateKeys::makeSatPhaseBiasKey("E03", "L5Q") };
    const std::vector<std::pair<std::string, std::string>> cb{ { "E03", "C1C" }, { "E03", "C5Q" } };
    // The attribute letter: two cells (C and Q). The rule: one cell.
    EXPECT_EQ(netDatumGraphChannelFamilies(pb, cb, {}).size(), 2u);
    const auto cells = netDatumGraphChannelFamilies(pb, cb, {}, true);
    ASSERT_EQ(cells.size(), 1u);
    EXPECT_EQ(cells[0].family, kDatumGraphFamilyRest);
    EXPECT_EQ(cells[0].n_pb, 2);
    // A direction's identities name the family by the same definition.
    const auto ids = netDatumGraphDetailIds("PB:E03:L5X:0.5", true);
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], std::string("E03/") + kDatumGraphFamilyX);
}

}  // namespace
}  // namespace GnssProcessor
