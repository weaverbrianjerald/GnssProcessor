/**
 * @file test_n3c_switches.cpp
 * @brief The signal-role switches (network.n3c_*): each site's answer with its switch on,
 *        and the default (switch off) answer unchanged.  Suite N3cSwitchesTest.
 */
#include <gtest/gtest.h>

#include "network/solve/Sbasis.hpp"

#include <cmath>
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

}  // namespace
}  // namespace GnssProcessor
