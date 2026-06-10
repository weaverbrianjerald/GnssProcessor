/**
 * @file test_signal_math.cpp
 * @brief Unit tests for PppUtils frequency/wavelength/system-string helpers and
 *        the shared RobustStats median / MAD-sigma utility.
 *
 * PppUtils is the single source of per-(system, signal) carrier frequencies and
 * the GLONASS FDMA channel arithmetic that turns a base frequency into a
 * satellite-specific one.  Every wavelength, every linear combination, and the
 * geometry of every observation depends on these returning the right number,
 * so the lookup logic and the FDMA channel math are worth pinning explicitly.
 *
 * All expected values are exact: frequencies compare against the named
 * Constants (and the GLONASS FDMA results are exactly representable doubles),
 * wavelengths against SPEED_OF_LIGHT / frequency plus a coarse physical sanity
 * check.  No data fixtures — PppUtils only needs a LogManager, and a
 * default-constructed one creates its loggers lazily (the StateManager tests
 * use the same bare-LogManager pattern).
 */

#include <gtest/gtest.h>

#include "common/PppUtils.hpp"
#include "common/Constants.hpp"
#include "logging/LogManager.hpp"
#include "math/RobustStats.hpp"

#include <cmath>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

// PppUtils holds a LogManager& and sets the GLONASS slot map per instance, so
// each test gets a fresh harness (no slot-override bleed between tests).
struct PppUtilsHarness {
    LogManager log_manager;
    PppUtils ppp{ log_manager };
};

// ---------------------------------------------------------------------------
// getFrequency
// ---------------------------------------------------------------------------

// Every observable-type prefix (C/L/D/S) on a band resolves to the same
// nominal carrier; a band the system doesn't broadcast returns 0.
TEST(PppUtilsFrequencyTest, GpsBandsResolveToNominalCarriers) {
    PppUtilsHarness h;
    EXPECT_DOUBLE_EQ(h.ppp.getFrequency("L1C", System::GPS, "G01"), Constants::GPS_L1_FREQ);
    EXPECT_DOUBLE_EQ(h.ppp.getFrequency("C1W", System::GPS, "G01"), Constants::GPS_L1_FREQ);
    EXPECT_DOUBLE_EQ(h.ppp.getFrequency("L2W", System::GPS, "G01"), Constants::GPS_L2_FREQ);
    EXPECT_DOUBLE_EQ(h.ppp.getFrequency("C5Q", System::GPS, "G01"), Constants::GPS_L5_FREQ);

    // Pin the constants themselves to their textbook values.
    EXPECT_DOUBLE_EQ(Constants::GPS_L1_FREQ, 1575.42e6);
    EXPECT_DOUBLE_EQ(Constants::GPS_L2_FREQ, 1227.60e6);
    EXPECT_DOUBLE_EQ(Constants::GPS_L5_FREQ, 1176.45e6);

    // GPS does not broadcast a band-6 signal -> 0.
    EXPECT_DOUBLE_EQ(h.ppp.getFrequency("L6X", System::GPS, "G01"), 0.0);
}

TEST(PppUtilsFrequencyTest, GalileoAndBeiDouBands) {
    PppUtilsHarness h;
    EXPECT_DOUBLE_EQ(h.ppp.getFrequency("C1X", System::Galileo, "E11"), Constants::GAL_E1_FREQ);
    EXPECT_DOUBLE_EQ(h.ppp.getFrequency("L5Q", System::Galileo, "E11"), Constants::GAL_E5A_FREQ);
    // E1 shares GPS L1's 1575.42 MHz; E5a shares L5's 1176.45 MHz.
    EXPECT_DOUBLE_EQ(Constants::GAL_E1_FREQ, Constants::GPS_L1_FREQ);
    EXPECT_DOUBLE_EQ(Constants::GAL_E5A_FREQ, Constants::GPS_L5_FREQ);

    // BeiDou B1I rides the band-2 code slot in this table.
    EXPECT_DOUBLE_EQ(h.ppp.getFrequency("C2I", System::BeiDou, "C01"), Constants::BDS_B1_FREQ);
}

// GLONASS FDMA: G1 = 1602.0 MHz + k·0.5625 MHz, G2 = 1246.0 MHz + k·0.4375 MHz.
// The default channel table gives R01 k=+1 and R02 k=-4.  All results are
// exactly representable, so EXPECT_DOUBLE_EQ is appropriate.
TEST(PppUtilsFrequencyTest, GlonassFdmaUsesChannelOffset) {
    PppUtilsHarness h;
    EXPECT_DOUBLE_EQ(h.ppp.getFrequency("L1C", System::GLONASS, "R01"), 1602562500.0);  // 1602.0 + 1*0.5625
    EXPECT_DOUBLE_EQ(h.ppp.getFrequency("L2C", System::GLONASS, "R01"), 1246437500.0);  // 1246.0 + 1*0.4375
    EXPECT_DOUBLE_EQ(h.ppp.getFrequency("L1C", System::GLONASS, "R02"), 1599750000.0);  // 1602.0 - 4*0.5625

    // Band 3 is CDMA — a fixed frequency with no channel term.
    EXPECT_DOUBLE_EQ(h.ppp.getFrequency("L3I", System::GLONASS, "R01"),
                     Constants::GLONASS_G3_BASE_FREQ);
}

// An unknown satellite (channel sentinel -999) or an explicitly out-of-range
// slot is rejected with a 0 frequency rather than a bogus carrier.
TEST(PppUtilsFrequencyTest, GlonassRejectsOutOfRangeOrUnknownChannel) {
    PppUtilsHarness h;
    EXPECT_DOUBLE_EQ(h.ppp.getFrequency("L1C", System::GLONASS, "R99"), 0.0);  // unknown -> -999 -> reject

    h.ppp.setGlonassSlots({ {"R01", 99} });  // 99 > +6
    EXPECT_DOUBLE_EQ(h.ppp.getFrequency("L1C", System::GLONASS, "R01"), 0.0);
}

// ---------------------------------------------------------------------------
// getWavelength
// ---------------------------------------------------------------------------

TEST(PppUtilsWavelengthTest, IsSpeedOfLightOverFrequency) {
    PppUtilsHarness h;
    EXPECT_DOUBLE_EQ(h.ppp.getWavelength(System::GPS, "L1"),
                     Constants::SPEED_OF_LIGHT / Constants::GPS_L1_FREQ);
    EXPECT_NEAR(h.ppp.getWavelength(System::GPS, "L1"), 0.190294, 1e-5);
    EXPECT_NEAR(h.ppp.getWavelength(System::GPS, "L2"), 0.244210, 1e-5);

    // Two-arg overload == three-arg overload with empty satellite (non-GLONASS).
    EXPECT_DOUBLE_EQ(h.ppp.getWavelength(System::GPS, "L1"),
                     h.ppp.getWavelength(System::GPS, "L1", ""));

    // Empty frequency type -> 0.
    EXPECT_DOUBLE_EQ(h.ppp.getWavelength(System::GPS, ""), 0.0);
}

TEST(PppUtilsWavelengthTest, GlonassIsSatelliteSpecific) {
    PppUtilsHarness h;
    EXPECT_DOUBLE_EQ(h.ppp.getWavelength(System::GLONASS, "L1", "R01"),
                     Constants::SPEED_OF_LIGHT / 1602562500.0);
    // Different channel (R01 k=+1 vs R02 k=-4) => different wavelength.
    EXPECT_NE(h.ppp.getWavelength(System::GLONASS, "L1", "R01"),
              h.ppp.getWavelength(System::GLONASS, "L1", "R02"));
}

// ---------------------------------------------------------------------------
// getGlonassChannel
// ---------------------------------------------------------------------------

TEST(PppUtilsGlonassChannelTest, DefaultsOverridesAndUnknownSentinel) {
    PppUtilsHarness h;
    EXPECT_EQ(h.ppp.getGlonassChannel("R01"), 1);   // from DEFAULT_GLONASS_CHANNELS
    EXPECT_EQ(h.ppp.getGlonassChannel("R02"), -4);

    h.ppp.setGlonassSlots({ {"R01", 3} });          // user slot overrides default
    EXPECT_EQ(h.ppp.getGlonassChannel("R01"), 3);
    EXPECT_EQ(h.ppp.getGlonassChannel("R02"), -4);   // untouched entries still default

    // Unknown satellite -> -999 skip sentinel.  (The header doc says "0";
    // the implementation returns -999, which is the value getFrequency relies
    // on to reject the satellite.  Test pins the real behavior.)
    EXPECT_EQ(h.ppp.getGlonassChannel("R99"), -999);
}

// ---------------------------------------------------------------------------
// getSystemString (static)
// ---------------------------------------------------------------------------

TEST(PppUtilsSystemStringTest, EnumOverload) {
    EXPECT_EQ(PppUtils::getSystemString(System::GPS), "GPS");
    EXPECT_EQ(PppUtils::getSystemString(System::Galileo), "Galileo");
    EXPECT_EQ(PppUtils::getSystemString(System::GLONASS), "GLONASS");
    EXPECT_EQ(PppUtils::getSystemString(System::BeiDou), "BeiDou");
    EXPECT_EQ(PppUtils::getSystemString(System::QZSS), "QZSS");
    EXPECT_EQ(PppUtils::getSystemString(System::SBAS), "SBAS");
    EXPECT_EQ(PppUtils::getSystemString(System::Unknown), "Unknown");
}

TEST(PppUtilsSystemStringTest, SatelliteIdOverload) {
    EXPECT_EQ(PppUtils::getSystemString(std::string("G01")), "GPS");
    EXPECT_EQ(PppUtils::getSystemString(std::string("E11")), "Galileo");
    EXPECT_EQ(PppUtils::getSystemString(std::string("R05")), "GLONASS");
    EXPECT_EQ(PppUtils::getSystemString(std::string("C01")), "BeiDou");
    EXPECT_EQ(PppUtils::getSystemString(std::string("J01")), "QZSS");
    EXPECT_EQ(PppUtils::getSystemString(std::string("S20")), "SBAS");
    EXPECT_EQ(PppUtils::getSystemString(std::string("")), "Unknown");
    EXPECT_EQ(PppUtils::getSystemString(std::string("X01")), "Unknown");
}

// ---------------------------------------------------------------------------
// RobustStats — the median / MAD-sigma math extracted from the slip detectors
// (Mw, GF, EWL, GRAPHIC, IonoVariabilityMonitor, IF-phase) into one utility.
// ---------------------------------------------------------------------------

TEST(RobustStatsTest, MedianOddEvenSingleAndEmpty) {
    EXPECT_DOUBLE_EQ(RobustStats::median(std::vector<double>{1, 2, 3, 4, 5}), 3.0);
    EXPECT_DOUBLE_EQ(RobustStats::median(std::vector<double>{1, 2, 3, 4}), 2.5);  // mean of middle pair
    EXPECT_DOUBLE_EQ(RobustStats::median(std::vector<double>{5}), 5.0);
    EXPECT_TRUE(std::isnan(RobustStats::median(std::vector<double>{})));
}

TEST(RobustStatsTest, MedianIgnoresInputOrderAndLeavesSourceUnmodified) {
    EXPECT_DOUBLE_EQ(RobustStats::median(std::vector<double>{5, 1, 4, 2, 3}), 3.0);
    // Works on a deque (the container the detector windows use) without
    // sorting the source in place.
    std::deque<double> dq{30, 10, 20};
    EXPECT_DOUBLE_EQ(RobustStats::median(dq), 20.0);
    ASSERT_EQ(dq.size(), 3u);
    EXPECT_DOUBLE_EQ(dq[0], 30.0);
}

TEST(RobustStatsTest, MadSigmaKnownValues) {
    EXPECT_DOUBLE_EQ(RobustStats::kMadToSigma, 1.4826);
    // {1,2,3,4,5}: median 3, |dev| {2,1,0,1,2}, MAD 1 -> sigma 1.4826.
    EXPECT_DOUBLE_EQ(RobustStats::madSigma(std::vector<double>{1, 2, 3, 4, 5}), 1.4826);
    // {1,2,3,4}: median 2.5, |dev| {1.5,.5,.5,1.5}, MAD 1 -> sigma 1.4826.
    EXPECT_DOUBLE_EQ(RobustStats::madSigma(std::vector<double>{1, 2, 3, 4}), 1.4826);
    // Constant data -> zero spread; deque path agrees with vector path.
    EXPECT_DOUBLE_EQ(RobustStats::madSigma(std::vector<double>{7, 7, 7, 7}), 0.0);
    EXPECT_DOUBLE_EQ(RobustStats::madSigma(std::deque<double>{1, 2, 3, 4, 5}), 1.4826);
}

TEST(RobustStatsTest, MadSigmaIsOutlierRobust) {
    // A lone gross outlier among identical values does not move the MAD:
    // median 1, |dev| {0,0,0,0,99}, MAD = 0.
    EXPECT_DOUBLE_EQ(RobustStats::madSigma(std::vector<double>{1, 1, 1, 1, 100}), 0.0);
}

TEST(RobustStatsTest, MadSigmaEmptyAndSingle) {
    EXPECT_TRUE(std::isnan(RobustStats::madSigma(std::vector<double>{})));
    EXPECT_DOUBLE_EQ(RobustStats::madSigma(std::vector<double>{5}), 0.0);  // single value -> dev 0
}

}  // namespace
}  // namespace GnssProcessor
