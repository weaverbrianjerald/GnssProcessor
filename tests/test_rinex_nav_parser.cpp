/**
 * @file test_rinex_nav_parser.cpp
 * @brief Tests for the RINEX 3/4 navigation-message parser (RTF-1 Card 2).
 *
 * Two layers:
 *   (1) Self-contained UNIT tests against programmatically-formatted RINEX
 *       records (guaranteed 19-column D-field alignment, chosen values) that
 *       lock the field mapping, the per-constellation time conventions (the
 *       BeiDou NATIVE-BDT week is the load-bearing one), GLONASS/SBAS/CNAV
 *       skipping, RINEX-4 record framing, and Fortran-D exponent tolerance.
 *   (2) An INTEGRATION test (skip-if-missing, like the Sp3 fixture test) that
 *       parses the bundled BRDC file and validates the reconstructed broadcast
 *       orbit/clock against the precise SP3/CLK products for the same epoch to a
 *       broadcast-grade tolerance — the decisive units/scale/week check.
 */

#include <gtest/gtest.h>

#include "products/RinexNavParser.hpp"
#include "products/LiveEphemerisStore.hpp"
#include "products/ProductParser.hpp"
#include "products/ProductTypes.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "math/TimeUtils.hpp"
#include "common/GnssTypes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

constexpr double kC = 299792458.0;   // speed of light [m/s]

#ifndef GNSS_TEST_DATA_DIR
#define GNSS_TEST_DATA_DIR ""
#endif

// Format a double as a 19-column RINEX D-field: "% .12E" gives
// " d.ddddddddddddE±dd" (19 chars) for a 2-digit exponent; normalise a rare
// 3-digit exponent and left-pad defensively so column boundaries land at
// 4/23/42/61 exactly.
std::string navField(double v) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "% .12E", v);
    std::string s(buf);
    if (s.size() > 19) {
        const auto e = s.find_first_of("Ee");
        if (e != std::string::npos && s.size() - e == 5 && s[e + 2] == '0')
            s.erase(e + 2, 1);   // "E+002" -> "E+02"
    }
    if (s.size() < 19) s = std::string(19 - s.size(), ' ') + s;
    return s.substr(0, 19);
}

// A continuation line: 4-space indent + up to four 19-char value fields.
std::string navLine(std::initializer_list<double> vals) {
    std::string s = "    ";
    for (double v : vals) s += navField(v);
    return s + "\n";
}

// The SV/EPOCH/CLK line: "S## YYYY MM DD hh mm ss" (23 cols) + three clock terms.
std::string navEpoch(char sys, int prn, int y, int mo, int d, int h, int mi, int se,
                     double af0, double af1, double af2) {
    char hdr[40];
    std::snprintf(hdr, sizeof(hdr), "%c%02d %4d %02d %02d %02d %02d %02d",
                  sys, prn, y, mo, d, h, mi, se);
    return std::string(hdr) + navField(af0) + navField(af1) + navField(af2) + "\n";
}

const char* kHdrV3 =
    "     3.04           N: GNSS NAV DATA    M: MIXED            RINEX VERSION / TYPE\n"
    "                                                            END OF HEADER\n";
const char* kHdrV4 =
    "     4.00           N: GNSS NAV DATA    M: MIXED            RINEX VERSION / TYPE\n"
    "                                                            END OF HEADER\n";

// Build a canonical 8-line legacy Keplerian record (epoch + 7 ORBIT lines) with
// chosen, individually-checkable values.  @p week goes verbatim into ORBIT-5
// field 2, @p iode/@p toe into ORBIT-1/ORBIT-3, tgd0/tgd1 into ORBIT-6.
std::string keplerRecord(char sys, int prn, int y, int mo, int d, int h, int mi, int se,
                         double af0, double af1, double af2,
                         double iode, double crs, double dn, double m0,
                         double cuc, double e, double cus, double sqrtA,
                         double toe, double cic, double om0, double cis,
                         double i0, double crc, double w, double omdot,
                         double idot, double week,
                         double health, double tgd0, double tgd1) {
    std::string s = navEpoch(sys, prn, y, mo, d, h, mi, se, af0, af1, af2);
    s += navLine({iode, crs, dn, m0});                 // ORBIT-1
    s += navLine({cuc, e, cus, sqrtA});                // ORBIT-2
    s += navLine({toe, cic, om0, cis});                // ORBIT-3
    s += navLine({i0, crc, w, omdot});                 // ORBIT-4
    s += navLine({idot, 1.0, week, 0.0});              // ORBIT-5 (codes/flag = filler)
    s += navLine({2.0, health, tgd0, tgd1});           // ORBIT-6 (accuracy = filler)
    s += navLine({0.0, 4.0});                          // ORBIT-7 (transmit/fit = filler)
    return s;
}

const BroadcastEphemeris* findSat(const std::vector<BroadcastEphemeris>& v, const std::string& s) {
    for (const auto& e : v) if (e.satellite == s) return &e;
    return nullptr;
}

} // namespace

// ===========================================================================
// Unit tests — field mapping + conventions
// ===========================================================================

// GPS record: every Keplerian field, iode, week/toe on the GPS scale.
TEST(RinexNavParser, ParsesGpsRecordFields) {
    const std::string body = keplerRecord(
        'G', 5, 2024, 5, 8, 12, 0, 0,
        /*af0*/ 1.5e-4, /*af1*/ -4.6e-12, /*af2*/ 0.0,
        /*iode*/ 42, /*crs*/ 146.21875, /*dn*/ 5.451298e-9, /*m0*/ 2.16624,
        /*cuc*/ 7.44e-6, /*e*/ 1.34e-2, /*cus*/ 1.12e-5, /*sqrtA*/ 5153.766,
        /*toe*/ 302400.0, /*cic*/ 2.42e-8, /*om0*/ 0.31882, /*cis*/ -3.03e-7,
        /*i0*/ 0.95509, /*crc*/ 157.9375, /*w*/ 1.03205, /*omdot*/ -7.80e-9,
        /*idot*/ 1.64e-10, /*week*/ 2313,
        /*health*/ 0, /*tgd0*/ -1.955e-8, /*tgd1*/ 0.0);
    const auto v = RinexNavParser::parseString(std::string(kHdrV3) + body);
    ASSERT_EQ(v.size(), 1u);
    const BroadcastEphemeris& e = v[0];
    EXPECT_EQ(e.satellite, "G05");
    EXPECT_EQ(e.system, System::GPS);
    EXPECT_EQ(e.iode, 42);
    EXPECT_EQ(e.kepler.toe.week, 2313);
    EXPECT_DOUBLE_EQ(e.kepler.toe.tow, 302400.0);
    EXPECT_DOUBLE_EQ(e.kepler.toc.week, 2313);   // GPS scale: toc week == toe week
    EXPECT_NEAR(e.kepler.sqrt_a, 5153.766, 1e-3);
    EXPECT_NEAR(e.kepler.e, 1.34e-2, 1e-9);
    EXPECT_NEAR(e.kepler.delta_n, 5.451298e-9, 1e-15);
    EXPECT_NEAR(e.kepler.m0, 2.16624, 1e-5);
    EXPECT_NEAR(e.kepler.omega0, 0.31882, 1e-5);
    EXPECT_NEAR(e.kepler.crs, 146.21875, 1e-4);
    EXPECT_NEAR(e.kepler.af0, 1.5e-4, 1e-12);
    EXPECT_NEAR(e.kepler.af1, -4.6e-12, 1e-18);
    EXPECT_NEAR(e.kepler.tgd0, -1.955e-8, 1e-12);
    EXPECT_DOUBLE_EQ(e.kepler.tgd1, 0.0);        // GPS carries only one group delay
}

// BeiDou: the ORBIT-5 week field is the NATIVE BDT week and must be stored
// verbatim (NOT converted to a GPS week) — the RTCM-decoder convention the store
// evaluates against.  AODE lands in iode; both TGDs are kept.
TEST(RinexNavParser, BeiDouUsesNativeBdtWeekAndBothTgds) {
    const std::string body = keplerRecord(
        'C', 21, 2024, 5, 8, 12, 0, 0,
        1.0e-4, 1.9e-12, 0.0,
        /*iode(AODE)*/ 9, 304.5, 3.116e-9, 2.33621,
        9.86e-6, 2.70e-4, 3.40e-5, /*sqrtA*/ 5282.6,
        /*toe*/ 302400.0, 5.58e-8, -2.96508, 2.00e-7,
        0.9600, -1046.6875, -3.08096, -2.05e-9,
        1.09e-9, /*week(BDT)*/ 957,
        /*health*/ 0, /*tgd0*/ -4.9e-9, /*tgd1*/ -1.0e-8);
    const auto v = RinexNavParser::parseString(std::string(kHdrV3) + body);
    ASSERT_EQ(v.size(), 1u);
    const BroadcastEphemeris& e = v[0];
    EXPECT_EQ(e.satellite, "C21");
    EXPECT_EQ(e.system, System::BeiDou);
    EXPECT_EQ(e.iode, 9);
    EXPECT_EQ(e.kepler.toe.week, 957);           // NATIVE BDT week, not 2313
    EXPECT_DOUBLE_EQ(e.kepler.toe.tow, 302400.0);
    EXPECT_EQ(e.kepler.toc.week, 957);
    EXPECT_NEAR(e.kepler.tgd0, -4.9e-9, 1e-13);
    EXPECT_NEAR(e.kepler.tgd1, -1.0e-8, 1e-13);  // BDS TGD2 kept
}

// Galileo: IODnav in iode, GPS-aligned week (RINEX stores it continuous with
// GPS), both BGDs kept.
TEST(RinexNavParser, GalileoIodnavWeekAndBgds) {
    const std::string body = keplerRecord(
        'E', 2, 2024, 5, 7, 23, 30, 0,
        1.25e-4, 3.05e-12, 0.0,
        /*iode(IODnav)*/ 48, 210.0, 2.584e-9, -3.06769,
        9.83e-6, 2.84e-4, 1.05e-5, /*sqrtA*/ 5440.624,
        /*toe*/ 257400.0, -1.32e-7, 0.34170, 5.21e-8,
        0.96719, 119.0, 0.73339, -5.27e-9,
        1.09e-10, /*week*/ 2313,
        /*health*/ 0, /*bgd_a*/ -3.725e-9, /*bgd_b*/ -4.1e-9);
    const auto v = RinexNavParser::parseString(std::string(kHdrV3) + body);
    ASSERT_EQ(v.size(), 1u);
    const BroadcastEphemeris& e = v[0];
    EXPECT_EQ(e.satellite, "E02");
    EXPECT_EQ(e.system, System::Galileo);
    EXPECT_EQ(e.iode, 48);
    EXPECT_EQ(e.kepler.toe.week, 2313);
    EXPECT_DOUBLE_EQ(e.kepler.toe.tow, 257400.0);
    EXPECT_NEAR(e.kepler.tgd0, -3.725e-9, 1e-13);
    EXPECT_NEAR(e.kepler.tgd1, -4.1e-9, 1e-13);
}

// QZSS: prefix J, System::QZSS, GPS-aligned week; structurally identical to GPS.
TEST(RinexNavParser, QzssParsed) {
    const std::string body = keplerRecord(
        'J', 2, 2024, 5, 8, 0, 0, 0,
        -1.1e-7, 0.0, 0.0,
        61, 1023.97, 1.46e-9, -2.76483,
        3.51e-5, 7.59e-2, 3.13e-5, 6493.12,
        302400.0, 1.03e-5, 0.54109, -3.47e-6,
        0.70999, -1024.0, -1.58759, -4.28e-9,
        -2.92e-9, 2313,
        /*health*/ 1, 1.86e-9, 0.0);
    const auto v = RinexNavParser::parseString(std::string(kHdrV3) + body);
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0].satellite, "J02");
    EXPECT_EQ(v[0].system, System::QZSS);
    EXPECT_EQ(v[0].iode, 61);
    EXPECT_EQ(v[0].health, 1);
}

// GLONASS ('R', 3 orbit lines) and SBAS ('S', 3 orbit lines) are skipped
// cleanly; the surrounding Keplerian records still parse.
TEST(RinexNavParser, SkipsGlonassAndSbas) {
    std::string body;
    body += keplerRecord('G', 1, 2024, 5, 8, 0, 0, 0, 1e-4, 0, 0,
                         1, 100, 5e-9, 1.0, 1e-6, 1e-2, 1e-6, 5153.0,
                         259200, 1e-8, 0.3, 1e-7, 0.95, 150, 1.0, -7e-9,
                         1e-10, 2313, 0, -1e-8, 0);
    // GLONASS record: epoch + 3 PZ-90 state lines.
    body += navEpoch('R', 1, 2024, 5, 7, 23, 45, 0, 8.46e-5, 9.09e-13, 257400.0);
    body += navLine({1.71e4, 1.15, 2.79e-9, 0.0});
    body += navLine({1.34e4, 1.40, -1.86e-9, 1.0});
    body += navLine({1.32e4, -2.91, -2.79e-9, 0.0});
    // SBAS record: epoch + 3 lines.
    body += navEpoch('S', 21, 2024, 5, 7, 23, 59, 12, 0.0, 0.0, 259221.0);
    body += navLine({4.20e4, 0.0, 0.0, 63.0});
    body += navLine({-3.67e3, 0.0, 0.0, 3.27e4});
    body += navLine({0.0, 0.0, 0.0, 179.0});
    body += keplerRecord('E', 11, 2024, 5, 8, 0, 0, 0, 2e-4, 0, 0,
                         2, 120, 3e-9, 1.5, 2e-6, 3e-4, 2e-6, 5440.0,
                         259200, 2e-8, 0.4, 2e-7, 0.97, 130, 0.8, -5e-9,
                         1e-10, 2313, 0, -3e-9, -4e-9);

    const auto v = RinexNavParser::parseString(std::string(kHdrV3) + body);
    ASSERT_EQ(v.size(), 2u);                         // only G01 + E11
    EXPECT_NE(findSat(v, "G01"), nullptr);
    EXPECT_NE(findSat(v, "E11"), nullptr);
    EXPECT_EQ(findSat(v, "R01"), nullptr);           // GLONASS skipped
    EXPECT_EQ(findSat(v, "S21"), nullptr);           // SBAS skipped
    for (const auto& e : v) EXPECT_NE(e.system, System::GLONASS);
}

// Fortran 'D' exponent markers parse identically to 'E'.
TEST(RinexNavParser, HandlesFortranDExponent) {
    std::string body = keplerRecord('G', 7, 2024, 5, 8, 12, 0, 0, 1.5e-4, -4.6e-12, 0,
                                    30, 146.0, 5.4e-9, 2.16, 7e-6, 1.3e-2, 1e-5, 5153.7,
                                    302400, 2e-8, 0.31, -3e-7, 0.955, 157.0, 1.03, -7e-9,
                                    1.6e-10, 2313, 0, -1.9e-8, 0);
    std::replace(body.begin(), body.end(), 'E', 'D');   // Fortran-style exponents
    const auto v = RinexNavParser::parseString(std::string(kHdrV3) + body);
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0].satellite, "G07");
    EXPECT_NEAR(v[0].kepler.sqrt_a, 5153.7, 1e-2);
    EXPECT_NEAR(v[0].kepler.af0, 1.5e-4, 1e-12);
    EXPECT_EQ(v[0].kepler.toe.week, 2313);
}

// RINEX-4 "> EPH" framing: EPH/LNAV+INAV records parse; STO/ION and CNAV
// (non-legacy layout) records are skipped.
TEST(RinexNavParser, Rinex4EphFramingSkipsStoIonAndCnav) {
    std::string body;
    body += "> EPH G01 LNAV\n";
    body += keplerRecord('G', 1, 2024, 5, 8, 12, 0, 0, 1e-4, 0, 0,
                         1, 100, 5e-9, 1.0, 1e-6, 1e-2, 1e-6, 5153.0,
                         302400, 1e-8, 0.3, 1e-7, 0.95, 150, 1.0, -7e-9,
                         1e-10, 2313, 0, -1e-8, 0);
    body += "> STO GPUT\n";
    body += navEpoch('G', 0, 2024, 5, 8, 0, 0, 0, 0.0, 0.0, 0.0);   // STO body (skipped)
    body += navLine({5.184e5, 2.29e3});
    body += "> ION GAL\n";
    body += navEpoch('E', 0, 2024, 5, 8, 0, 0, 0, 65.0, 1.5e-2, -3.9e-3);  // ION body
    body += navLine({0.0});
    body += "> EPH C30 CNVX\n";                                       // non-legacy: skip
    body += keplerRecord('C', 30, 2024, 5, 8, 12, 0, 0, 1e-4, 0, 0,
                         1, 100, 5e-9, 1.0, 1e-6, 1e-2, 1e-6, 5282.0,
                         302400, 1e-8, 0.3, 1e-7, 0.95, 150, 1.0, -7e-9,
                         1e-10, 957, 0, -1e-8, -1e-8);
    body += "> EPH E02 INAV\n";
    body += keplerRecord('E', 2, 2024, 5, 8, 0, 0, 0, 2e-4, 0, 0,
                         2, 120, 3e-9, 1.5, 2e-6, 3e-4, 2e-6, 5440.0,
                         302400, 2e-8, 0.4, 2e-7, 0.97, 130, 0.8, -5e-9,
                         1e-10, 2313, 0, -3e-9, -4e-9);

    const auto v = RinexNavParser::parseString(std::string(kHdrV4) + body);
    ASSERT_EQ(v.size(), 2u);                         // G01 + E02 only
    EXPECT_NE(findSat(v, "G01"), nullptr);
    EXPECT_NE(findSat(v, "E02"), nullptr);
    EXPECT_EQ(findSat(v, "C30"), nullptr);           // CNVX (non-legacy) skipped
}

// A non-conformant RINEX-4 header written without the space after '>'
// (">EPH ..." instead of "> EPH ...") still parses the ephemeris record.
TEST(RinexNavParser, Rinex4TolerantOfSpacelessHeader) {
    std::string body = ">EPH G09 LNAV\n";
    body += keplerRecord('G', 9, 2024, 5, 8, 12, 0, 0, 1e-4, 0, 0,
                         1, 100, 5e-9, 1.0, 1e-6, 1e-2, 1e-6, 5153.0,
                         302400, 1e-8, 0.3, 1e-7, 0.95, 150, 1.0, -7e-9,
                         1e-10, 2313, 0, -1e-8, 0);
    const auto v = RinexNavParser::parseString(std::string(kHdrV4) + body);
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0].satellite, "G09");
}

// A RINEX-2 file (unsupported epoch format) yields an empty vector, not garbage.
TEST(RinexNavParser, RejectsPreRinex3) {
    const std::string content =
        "     2.11           N: GPS NAV DATA                         RINEX VERSION / TYPE\n"
        "                                                            END OF HEADER\n"
        "G01 24  5  8  0  0  0.0 1.5e-4 0 0\n";
    EXPECT_TRUE(RinexNavParser::parseString(content).empty());
}

// No END OF HEADER → treated as not-a-nav-file, empty (no header-line misparse).
TEST(RinexNavParser, MissingEndOfHeaderReturnsEmpty) {
    EXPECT_TRUE(RinexNavParser::parseString("just some text\nnot rinex\n").empty());
}

// ===========================================================================
// Integration test — bundled BRDC vs precise SP3/CLK (the decisive check)
// ===========================================================================

namespace {

const OrbitData* sp3NodeAt(const std::vector<OrbitData>& orbits,
                           const std::string& sat, const GnssTime& t) {
    const double tc = TimeUtils::continuousGpsSeconds(t);
    for (const auto& o : orbits) {
        if (o.satellite == sat && o.position_valid &&
            std::abs(TimeUtils::continuousGpsSeconds(o.time) - tc) < 1.0)
            return &o;
    }
    return nullptr;
}

const ClockData* clkNodeAt(const std::vector<ClockData>& clocks,
                           const std::string& sat, const GnssTime& t) {
    const double tc = TimeUtils::continuousGpsSeconds(t);
    for (const auto& c : clocks) {
        if (c.satellite == sat &&
            std::abs(TimeUtils::continuousGpsSeconds(c.time) - tc) < 1.0)
            return &c;
    }
    return nullptr;
}

} // namespace

TEST(RinexNavParserFixture, ParsesBundledBrdcAndMatchesPreciseProducts) {
    const std::string data_dir = GNSS_TEST_DATA_DIR;
    if (data_dir.empty()) GTEST_SKIP() << "GNSS_TEST_DATA_DIR not defined";
    const fs::path prod = fs::path(data_dir) / "2024-129" / "products";
    const fs::path brdc = prod / "BRDC00IGS_R_20241290000_01D_MN.rnx";
    const fs::path sp3  = prod / "COD0MGXFIN_20241290000_01D_05M_ORB.SP3";
    const fs::path clk  = prod / "COD0MGXFIN_20241290000_01D_30S_CLK.CLK";
    if (!fs::exists(brdc)) GTEST_SKIP() << "BRDC fixture absent: " << brdc.string();

    // --- Parse coverage: Keplerian systems present, GLONASS/SBAS absent. ---
    const auto ephs = RinexNavParser::parse(brdc.string());
    ASSERT_GT(ephs.size(), 0u);
    int nG = 0, nE = 0, nC = 0, nJ = 0;
    for (const auto& e : ephs) {
        switch (e.system) {
        case System::GPS:     ++nG; break;
        case System::Galileo: ++nE; break;
        case System::BeiDou:  ++nC; break;
        case System::QZSS:    ++nJ; break;
        default: FAIL() << "non-Keplerian record leaked: " << e.satellite; break;
        }
        EXPECT_NE(e.satellite[0], 'R');
        EXPECT_NE(e.satellite[0], 'S');
    }
    EXPECT_GT(nG, 0); EXPECT_GT(nE, 0); EXPECT_GT(nC, 0); EXPECT_GT(nJ, 0);

    // loadIntoLiveStore inserts the parsed set (returns the parsed count).
    auto& store = LiveEphemerisStore::getInstance();
    store.clear();
    const std::size_t loaded = RinexNavParser::loadIntoLiveStore(store, brdc.string());
    EXPECT_EQ(loaded, ephs.size());
    EXPECT_GT(store.size(), 0u);

    // --- Validation: broadcast vs precise SP3/CLK at a mid-day epoch. ---
    if (!fs::exists(sp3) || !fs::exists(clk))
        GTEST_SKIP() << "precise SP3/CLK products absent; parse coverage validated";

    GnssConfig cfg;
    LogManager  log;
    ProductParser parser(cfg, log);
    std::vector<OrbitData> orbits;
    std::vector<ClockData> clocks;
    ASSERT_TRUE(parser.parseSp3({sp3}, orbits));
    ASSERT_TRUE(parser.parseClk({clk}, clocks));
    ASSERT_GT(orbits.size(), 0u);
    ASSERT_GT(clocks.size(), 0u);

    // Evaluate at 23:00:00 GPS (an SP3 5-min node and a CLK 30-s node), and, per
    // satellite, seat ONLY the parsed record whose toe-of-week is nearest that
    // epoch so the broadcast extrapolation stays inside its fit window (the
    // whole-file load keeps only the freshest toe, which for an early-day query
    // would be out of window).
    const GnssTime T = TimeUtils::gregorianGpsToGpsTime(2024, 5, 8, 23, 0, 0.0);
    store.clear();
    std::map<std::string, BroadcastEphemeris> nearest;
    std::map<std::string, double> best;
    for (const auto& e : ephs) {
        double d = std::abs(e.kepler.toe.tow - T.tow);
        d = std::min(d, 604800.0 - d);
        auto it = best.find(e.satellite);
        if (it == best.end() || d < it->second) {
            best[e.satellite] = d;
            nearest[e.satellite] = e;
        }
    }
    for (const auto& kv : nearest) store.insert(kv.second, /*from_igs=*/true);

    auto isTightOrbit = [](const std::string& s) {
        return s[0] == 'G' || s[0] == 'E' ||
               (s[0] == 'C' && std::stoi(s.substr(1)) >= 19);   // BDS MEO
    };
    auto isTightClock = [](const std::string& s) {
        return s[0] == 'G' || s[0] == 'E';   // clean IF-referenced broadcast clocks
    };

    int n_orbit = 0, n_clock = 0;
    double max_orbit = 0.0, max_clock = 0.0;
    for (const auto& kv : nearest) {
        const std::string& sat = kv.first;
        // Skip UNHEALTHY satellites: their broadcast ephemeris/clock is frozen or
        // degraded by the control segment and legitimately disagrees with the
        // precise product (e.g. G01 was set unhealthy on 2024-DOY-129, health=63,
        // ~150 m off), so it is not a valid parser check.  health == 0 is healthy
        // for every constellation's encoding.
        if (kv.second.health != 0) continue;
        const auto pv = store.computeSatellitePV(sat, T);
        if (!pv) continue;                       // out of fit window for this sat
        if (isTightOrbit(sat)) {
            if (const OrbitData* node = sp3NodeAt(orbits, sat, T)) {
                const double d = (pv->position - node->position).norm();
                max_orbit = std::max(max_orbit, d);
                ++n_orbit;
                EXPECT_LT(d, 10.0) << "orbit " << sat << " Δ=" << d << " m";
            }
        }
        if (isTightClock(sat)) {
            const auto bc = store.computeSatelliteClock(sat, T);
            const ClockData* cn = clkNodeAt(clocks, sat, T);
            if (bc && cn) {
                const double d = std::abs(bc->bias - cn->clock_bias) * kC;
                max_clock = std::max(max_clock, d);
                ++n_clock;
                EXPECT_LT(d, 10.0) << "clock " << sat << " Δ=" << d << " m";
            }
        }
    }
    EXPECT_GE(n_orbit, 10) << "too few orbit comparisons (validated " << n_orbit << ")";
    EXPECT_GE(n_clock, 8)  << "too few clock comparisons (validated " << n_clock << ")";
    RecordProperty("max_orbit_residual_m", std::to_string(max_orbit));
    RecordProperty("max_clock_residual_m", std::to_string(max_clock));
    store.clear();
}

// =============================================================================
// RTF-2 Card 6: ephCutoverShiftMetres — the pure eph-cutover re-level delta.
// =============================================================================

namespace {
// A minimal GPS clock-only ephemeris: for GPS ssrMatchIode == iode, so the IODE
// gate is exactly the value set here.  Only the clock polynomial + toe/toc matter
// for computeSatelliteClock; the orbital elements are left zero.
BroadcastEphemeris gpsClockEph(const std::string& sat, int iode, const GnssTime& toe,
                               const GnssTime& toc, double af0, double af1 = 0.0) {
    BroadcastEphemeris e;
    e.satellite   = sat;
    e.system      = System::GPS;
    e.iode        = iode;
    e.health      = 0;
    e.kepler.toe  = toe;
    e.kepler.toc  = toc;
    e.kepler.af0  = af0;
    e.kepler.af1  = af1;
    return e;
}
} // namespace

// After an IODE roll (v2 fresher toe, demotes v1 to "previous"), the shift is the
// exact c·Δaf0 between the freshest and the pinned old version; a missing/evicted
// old version or an absent satellite is nullopt; querying the current IODE is a
// no-roll 0.  af1 = af2 = 0 ⇒ bias == af0 regardless of dt, so the shift is exact.
TEST(EphCutoverShiftTest, ExactAf0ShiftAcrossIodeRoll) {
    auto& store = LiveEphemerisStore::getInstance();
    store.clear();

    const GnssTime t{2313, 302400.0};
    const GnssTime toe_old{2313, 302400.0 - 5400.0};   // 1.5 h before t (in fit window)
    const GnssTime toe_new{2313, 302400.0 + 1800.0};   // 0.5 h after t  (fresher toe, closer)
    const double af0_old = 1.5e-4, af0_new = 1.6e-4;    // Δaf0 = 1e-5 s

    store.insert(gpsClockEph("G05", /*iode=*/10, toe_old, toe_old, af0_old), /*from_igs=*/true);
    store.insert(gpsClockEph("G05", /*iode=*/20, toe_new, toe_new, af0_new), /*from_igs=*/true);

    // Sanity: the store now serves v2 as freshest and still retains v1 pinned by IODE.
    ASSERT_TRUE(store.computeSatelliteClock("G05", t));
    EXPECT_EQ(store.computeSatelliteClock("G05", t)->iode, 20);
    ASSERT_TRUE(store.computeSatelliteClock("G05", t, /*ssr_iode=*/10));
    EXPECT_EQ(store.computeSatelliteClock("G05", t, 10)->iode, 10);

    // Roll 10 -> 20: shift = c·(af0_new − af0_old).
    const auto shift = store.ephCutoverShiftMetres("G05", t, /*old_iode=*/10);
    ASSERT_TRUE(shift.has_value());
    EXPECT_NEAR(*shift, kC * (af0_new - af0_old), 1e-6);

    // Old version no longer retained (never inserted iode 99) → nullopt (caller forces disc).
    EXPECT_FALSE(store.ephCutoverShiftMetres("G05", t, /*old_iode=*/99).has_value());
    // No ephemeris held for this satellite → nullopt.
    EXPECT_FALSE(store.ephCutoverShiftMetres("G09", t, /*old_iode=*/10).has_value());
    // Baseline already at the current version → no roll → exactly 0 (defensive branch).
    const auto none = store.ephCutoverShiftMetres("G05", t, /*old_iode=*/20);
    ASSERT_TRUE(none.has_value());
    EXPECT_DOUBLE_EQ(*none, 0.0);

    store.clear();
}

// Both clock polynomials are evaluated AT the query epoch, so a non-zero af1 on
// either version enters the shift through its own (t − toc) — the reconstruction
// stays continuous even when the drift term is live.
TEST(EphCutoverShiftTest, EvaluatesBothPolynomialsAtEpoch) {
    auto& store = LiveEphemerisStore::getInstance();
    store.clear();

    const GnssTime t{2313, 302400.0};
    const GnssTime toe_old{2313, 302400.0 - 5400.0};
    const GnssTime toc_old{2313, 302400.0};            // dt_old = 0 → bias_old = af0_old
    const GnssTime toe_new{2313, 302400.0 + 1800.0};
    const GnssTime toc_new{2313, 302400.0 - 900.0};    // dt_new = +900 s → bias_new = af0_new + af1_new·900
    const double af0_old = 1.5e-4;
    const double af0_new = 1.6e-4, af1_new = 2.0e-11;

    store.insert(gpsClockEph("G05", 10, toe_old, toc_old, af0_old),           /*from_igs=*/true);
    store.insert(gpsClockEph("G05", 20, toe_new, toc_new, af0_new, af1_new),  /*from_igs=*/true);

    const double bias_old = af0_old;
    const double bias_new = af0_new + af1_new * 900.0;
    const auto shift = store.ephCutoverShiftMetres("G05", t, /*old_iode=*/10);
    ASSERT_TRUE(shift.has_value());
    EXPECT_NEAR(*shift, kC * (bias_new - bias_old), 1e-6);

    store.clear();
}
