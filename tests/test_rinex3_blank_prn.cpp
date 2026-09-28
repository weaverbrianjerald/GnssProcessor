/**
 * @file test_rinex3_blank_prn.cpp
 * @brief RINEX 3 blank-padded satellite numbers (card RXP,
 *        docs/refound/RXP_rinex3_blank_prn_2026-09-21.md).
 *
 * A RINEX 3 record may write its satellite number blank-padded, "G 7" for "G07";
 * 16 of the 310 DOY-129 observation files do.  Taken verbatim, such an identifier
 * matches no orbit, no product record and no other station's satellite, so every
 * satellite numbered below 10 drops out of that station without a warning.  The
 * RINEX 2 path of the observation parser normalizes the blank (its
 * @c extractSatsFromLine lambda); the RINEX 3 paths of the observation parser and of
 * the navigation parser apply the same rule, unconditionally.
 *
 * Checked here:
 *   (a) an observation epoch mixing "G 7" and "G17" parses to G07 and G17, with
 *       each satellite's measurements landing on its own codes;
 *   (b) a navigation record written "G 7" is keyed G07, so it matches the
 *       observation of the same satellite;
 *   (c) an already-zero-padded file is unchanged by the normalization.
 *
 * Suite: Rinex3BlankPrnTest (one suite per file, per the suite ownership rule).
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "products/ProductParser.hpp"
#include "products/RinexNavParser.hpp"
#include "rinex/RinexParser.hpp"

using namespace GnssProcessor;

namespace {

/// Minimal RINEX 3.04 observation file: one epoch, three GPS satellites.  The
/// identifiers are written exactly as given, so a caller can pass the
/// blank-padded form under test.  Observation slots are exactly 16 chars
/// (F14.3 value + LLI + SNR), built programmatically.
std::filesystem::path writeObsFixture(const std::filesystem::path& dir,
                                      const std::string& name,
                                      const std::vector<std::string>& sat_ids) {
    const auto path = dir / name;
    auto fld = [](double v) {
        char b[24];
        std::snprintf(b, sizeof b, "%14.3f  ", v);
        return std::string(b);   // 16 chars: value + blank LLI + blank SNR
    };
    std::ofstream f(path);
    f << "     3.04           OBSERVATION DATA    M                   RINEX VERSION / TYPE\n"
         "BLNK                                                        MARKER NAME\n"
         "unit test           unit test                               OBSERVER / AGENCY\n"
         "00000               TESTRX              0.0                 REC # / TYPE / VERS\n"
         "00000               TESTANT     NONE                        ANT # / TYPE\n"
         "  3967283.1000   996406.2000  4970111.4000                  APPROX POSITION XYZ\n"
         "        0.0000        0.0000        0.0000                  ANTENNA: DELTA H/E/N\n"
         "G    4 C1C L1C C2W L2W                                      SYS / # / OBS TYPES\n"
         "    30.000                                                  INTERVAL\n"
         "  2024     5     8    19    25    0.0000000     GPS         TIME OF FIRST OBS\n"
         "                                                            END OF HEADER\n"
      << "> 2024 05 08 19 25  0.0000000  0  " << sat_ids.size() << "\n";
    // One distinguishable value block per satellite, so a mis-keyed record is
    // visible in the values and not only in the identifier.
    double v = 20000000.0;
    for (const auto& id : sat_ids) {
        f << id << fld(v) << fld(v * 5.0) << fld(v + 6.0) << fld(v * 4.0) << "\n";
        v += 1000000.0;
    }
    return path;
}

/// Load the fixture through the real JSON loader (no knob: the fix is
/// unconditional, so there is no OFF state to select).
void loadConfig(GnssConfig& cfg, LogManager& lm, const std::filesystem::path& dir,
                const std::filesystem::path& obs) {
    const auto jpath = dir / "blank_prn.json";
    std::ofstream j(jpath);
    std::string obs_fwd = obs.string();
    for (char& c : obs_fwd) if (c == '\\') c = '/';
    j << "{\n"
      << "  \"processing\": { \"mode\": \"ppp\" },\n"
      << "  \"files\": { \"observation_path\": \"" << obs_fwd << "\" }\n"
      << "}\n";
    j.close();
    ASSERT_TRUE(cfg.loadFromJson(jpath, lm)) << "config load failed";
}

std::set<std::string> satelliteIds(const std::vector<RinexObservation>& v) {
    std::set<std::string> out;
    for (const auto& o : v) out.insert(o.satellite);
    return out;
}

const RinexObservation* findSat(const std::vector<RinexObservation>& v,
                                const std::string& sat) {
    for (const auto& o : v) if (o.satellite == sat) return &o;
    return nullptr;
}

std::filesystem::path testDir() {
    const auto dir = std::filesystem::temp_directory_path() / "gnss_rinex3_blank_prn";
    std::filesystem::create_directories(dir);
    return dir;
}

/// A legacy GPS Keplerian navigation record whose SV/EPOCH/CLK line carries the
/// identifier verbatim (so it can be written blank-padded).  Only the identifier
/// matters here; the orbit fields just have to make the record decodable.
std::string navRecord(const std::string& sat_id) {
    auto f = [](double v) {
        char b[40];
        std::snprintf(b, sizeof b, "% .12E", v);
        std::string s(b);
        if (s.size() < 19) s = std::string(19 - s.size(), ' ') + s;
        return s.substr(0, 19);
    };
    auto line = [&](double a, double b, double c, double d) {
        return "    " + f(a) + f(b) + f(c) + f(d) + "\n";
    };
    std::string s = sat_id + " 2024 05 08 12 00 00" + f(1.5e-4) + f(-4.6e-12) + f(0.0) + "\n";
    s += line(42.0, 146.21875, 5.451298e-9, 2.16624);        // ORBIT-1
    s += line(7.44e-6, 1.34e-2, 1.12e-5, 5153.766);          // ORBIT-2  (sqrtA > 0)
    s += line(302400.0, 2.42e-8, 0.31882, -3.03e-7);         // ORBIT-3
    s += line(0.95509, 157.9375, 1.03205, -7.80e-9);         // ORBIT-4
    s += line(1.64e-10, 1.0, 2313.0, 0.0);                   // ORBIT-5  (week)
    s += line(2.0, 0.0, -1.955e-8, 0.0);                     // ORBIT-6
    s += line(0.0, 4.0, 0.0, 0.0);                           // ORBIT-7
    return s;
}

const char* kNavHdrV3 =
    "     3.04           N: GNSS NAV DATA    M: MIXED            RINEX VERSION / TYPE\n"
    "                                                            END OF HEADER\n";

} // namespace

// --------------------------------------------------------------------------
// (a) the observation path (the defect named in card RXP)
// --------------------------------------------------------------------------

TEST(Rinex3BlankPrnTest, BlankPaddedNumberIsZeroPaddedAndKeepsItsMeasurements) {
    const auto dir = testDir();
    // One epoch carrying "G 7" and "G17" together, as the affected DOY-129 files
    // write them.
    const auto obs = writeObsFixture(dir, "blank_G_30S_MO.rnx", { "G 7", "G17" });
    LogManager lm;
    GnssConfig cfg;
    loadConfig(cfg, lm, dir, obs);
    ProductParser pp(cfg, lm);
    RinexParser rp(cfg, lm, pp);
    ASSERT_TRUE(rp.parse());
    const auto& v = rp.getObservations();

    // The blank-padded record arrives under the canonical identifier, the form
    // that every orbit, clock, bias and state key in the project uses.
    EXPECT_EQ(satelliteIds(v), (std::set<std::string>{ "G07", "G17" }));

    const auto* g07 = findSat(v, "G07");
    ASSERT_NE(g07, nullptr) << "the satellite written 'G 7' was dropped";
    EXPECT_EQ(g07->system, System::GPS);
    ASSERT_TRUE(g07->measurements.count("C1C"));
    // Its own value block, not the next record's: a mis-keyed record would
    // still carry an identifier but the wrong measurements.
    EXPECT_NEAR(g07->measurements.at("C1C"), 20000000.000, 1e-3);
    EXPECT_NEAR(g07->measurements.at("L1C"), 100000000.000, 1e-3);
    EXPECT_NEAR(g07->measurements.at("C2W"), 20000006.000, 1e-3);

    const auto* g17 = findSat(v, "G17");
    ASSERT_NE(g17, nullptr);
    EXPECT_NEAR(g17->measurements.at("C1C"), 21000000.000, 1e-3);
    EXPECT_NEAR(g17->measurements.at("L1C"), 105000000.000, 1e-3);
}

TEST(Rinex3BlankPrnTest, ZeroPaddedFileIsUnchangedByTheNormalization) {
    const auto dir = testDir();
    // The same epoch written the usual way: the normalization is a no-op, which
    // is why the fix needs no knob (every already-correct file is untouched).
    const auto obs = writeObsFixture(dir, "padded_G_30S_MO.rnx", { "G07", "G17" });
    LogManager lm;
    GnssConfig cfg;
    loadConfig(cfg, lm, dir, obs);
    ProductParser pp(cfg, lm);
    RinexParser rp(cfg, lm, pp);
    ASSERT_TRUE(rp.parse());
    const auto& v = rp.getObservations();
    EXPECT_EQ(satelliteIds(v), (std::set<std::string>{ "G07", "G17" }));
    const auto* g07 = findSat(v, "G07");
    ASSERT_NE(g07, nullptr);
    EXPECT_NEAR(g07->measurements.at("C1C"), 20000000.000, 1e-3);
}

// --------------------------------------------------------------------------
// (b) the navigation path: the same identifier field in the other reader
// --------------------------------------------------------------------------

TEST(Rinex3BlankPrnTest, NavigationRecordWithABlankNumberIsKeyedZeroPadded) {
    const auto v = RinexNavParser::parseString(
        std::string(kNavHdrV3) + navRecord("G 7") + navRecord("G17"));
    ASSERT_EQ(v.size(), 2u);
    std::set<std::string> ids;
    for (const auto& e : v) ids.insert(e.satellite);
    // An ephemeris keyed "G 7" matches no observation of G07, so the satellite
    // would be dropped downstream even with the observation fix in place.
    EXPECT_EQ(ids, (std::set<std::string>{ "G07", "G17" }));
    for (const auto& e : v) EXPECT_EQ(e.system, System::GPS);
}
