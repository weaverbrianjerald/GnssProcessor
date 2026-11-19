/**
 * @file test_rx_sat_code_bias_key.cpp
 * @brief The receiver-satellite code-bias key "<satellite>|<code signal>" of filter/StateKeySchema.hpp:
 *        the maker, the parser, and the refusal of every sibling key shape.
 *
 * The key has the text of a satellite code-bias key; the two families keep separate maps.  The
 * parser is strict so that a key of any other family never reads as one of this family.
 */

#include <gtest/gtest.h>

#include "filter/StateKeySchema.hpp"

#include <string>
#include <vector>

using namespace GnssProcessor::StateKeys;

TEST(RxSatCodeBiasKeyTest, MakerAndParserRoundTrip) {
    for (const char* sat : { "G01", "G32", "E11", "R05", "C19", "J02" })
        for (const char* sig : { "C1W", "C2W", "C1C", "C5Q", "C7Q", "C2I" }) {
            const std::string key = makeRxSatCodeBiasKey(sat, sig);
            EXPECT_EQ(key, std::string(sat) + "|" + sig);
            const auto parts = parseRxSatCodeBiasKey(key);
            ASSERT_TRUE(parts.has_value()) << key;
            EXPECT_EQ(parts->satellite, sat);
            EXPECT_EQ(parts->signal, sig);
        }
}

TEST(RxSatCodeBiasKeyTest, TheKeyHasTheShapeOfASatelliteCodeBiasKey) {
    // The two families differ by map, not by text.
    EXPECT_EQ(makeRxSatCodeBiasKey("G05", "C1W"), makeSatBiasKey("G05", "C1W"));
}

TEST(RxSatCodeBiasKeyTest, MalformedKeysAreRefused) {
    for (const char* key : { "", "G01", "|C1W", "G01|", "|", "G01|C1", "G01|C1WW", "G01_C1W" })
        EXPECT_FALSE(parseRxSatCodeBiasKey(key).has_value()) << "'" << key << "'";
}

TEST(RxSatCodeBiasKeyTest, EverySiblingKeyShapeIsRefused) {
    const std::vector<std::string> siblings = {
        makeSatPhaseBiasKey("G05", "L1C"),          // "G05|L1C|PB"
        makeSatClockKey("G05"),                     // "G05|SC"
        makeSatBiasKey("G05", "C1W_C2W"),           // an iono-free code pair
        makeSatBiasKey("G05", "L1C"),               // a phase signal is no code
        makeAmbiguityKey("G05", "L1C"),             // "G05_L1C"
        makeIonosphereKey("G05"),                   // "G05"
        makeIfbKey("GPS", '2'),                     // a receiver code-bias key
        makeDcbKey("GPS"),
        makeReceiverIscKey("GPS", "C1W"),
        makeSystemTimeOffsetKey("Galileo"),
    };
    for (const std::string& key : siblings)
        EXPECT_FALSE(parseRxSatCodeBiasKey(key).has_value()) << key;
}

TEST(RxSatCodeBiasKeyTest, AKeyWithAnExtraPipeIsRefused) {
    EXPECT_FALSE(parseRxSatCodeBiasKey("G05|C1|W").has_value());
    EXPECT_FALSE(parseRxSatCodeBiasKey("G05|C1W|PB").has_value());
    EXPECT_FALSE(parseRxSatCodeBiasKey("DD|GPS|G05|C1W|G01").has_value());
}
