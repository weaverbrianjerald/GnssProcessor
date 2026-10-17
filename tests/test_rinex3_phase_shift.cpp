/**
 * @file test_rinex3_phase_shift.cpp
 * @brief RINEX 3 "SYS / PHASE SHIFT" satellite count and satellite list columns.
 *
 * The record carries the satellite count as I2.2 in 1-based columns 17-18 and
 * the satellites as 10(1X,A3) in columns 19-58, with further satellites on
 * continuation records whose columns 1-18 are blank.  The parser read the count
 * one column to the left (1-based 16-17), so "09" read as 0 and a per-satellite
 * record was stored and applied as system-wide, and "32" read as 3, so its
 * continuation records were not consumed.  The header lines below are copied
 * from ARHT00ATA_R_20241290000_01D_30S_MO.rnx (DOY 2024-129), which writes
 * both shapes.
 *
 * What is pinned here:
 *   (a) the count is read from columns 17-18 and the satellite list, including
 *       its continuation records, is stored per satellite;
 *   (b) with @c model_corrections.apply_phase_shifts on, a per-satellite shift
 *       reaches only the listed satellites and a system-wide one reaches all;
 *   (c) a count that the continuation records do not match is not widened to
 *       the whole system, and the next header record is still parsed;
 *   (d) with the knob off (the default), every observation is identical to the
 *       same file parsed without any SYS / PHASE SHIFT record.
 *
 * Suite: Rinex3PhaseShiftTest (one suite per file, ownership rule).
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "products/ProductParser.hpp"
#include "rinex/RinexParser.hpp"
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

using namespace GnssProcessor;

namespace {

/// SYS / PHASE SHIFT records as ARHT00ATA writes them: a 32-satellite list with
/// three continuation records, a 9-satellite list on one record, and one
/// system-wide record (no count) in the ABMF00GLP shape.
const char* kArhtShiftRecords =
    "G L1C  0.00000  32 G01 G02 G03 G04 G05 G06 G07 G08 G09 G10  SYS / PHASE SHIFT\n"
    "                   G11 G12 G13 G14 G15 G16 G17 G18 G19 G20  SYS / PHASE SHIFT\n"
    "                   G21 G22 G23 G24 G25 G26 G27 G28 G29 G30  SYS / PHASE SHIFT\n"
    "                   G31 G32                                  SYS / PHASE SHIFT\n"
    "G L1X -0.25000  09 G01 G02 G03 G04 G11 G14 G18 G23 G28      SYS / PHASE SHIFT\n"
    "G L2W  0.25000                                              SYS / PHASE SHIFT\n";

/// A declared count of 15 with no continuation record, followed directly by the
/// next record: the list holds 10 satellites and the L1X record must survive.
const char* kShortListRecords =
    "G L2W  0.25000  15 G01 G02 G03 G04 G05 G06 G07 G08 G09 G10  SYS / PHASE SHIFT\n"
    "G L1X -0.25000  09 G01 G02 G03 G04 G11 G14 G18 G23 G28      SYS / PHASE SHIFT\n";

const std::vector<std::string> kSats = { "G01", "G05", "G11", "G28", "G32" };

/// Minimal RINEX 3.04 observation file: one epoch, the satellites of @c kSats,
/// observation types C1C L1C L1X L2W.  @p shift_records is inserted verbatim
/// into the header.  Observation slots are exactly 16 chars (F14.3 + LLI + SNR).
std::filesystem::path writeObsFixture(const std::filesystem::path& dir,
                                      const std::string& name,
                                      const std::string& shift_records) {
    const auto path = dir / name;
    auto fld = [](double v) {
        char b[24];
        std::snprintf(b, sizeof b, "%14.3f  ", v);
        return std::string(b);
    };
    std::ofstream f(path);
    f << "     3.04           OBSERVATION DATA    M                   RINEX VERSION / TYPE\n"
         "PSHF                                                        MARKER NAME\n"
         "unit test           unit test                               OBSERVER / AGENCY\n"
         "00000               TESTRX              0.0                 REC # / TYPE / VERS\n"
         "00000               TESTANT     NONE                        ANT # / TYPE\n"
         "  3967283.1000   996406.2000  4970111.4000                  APPROX POSITION XYZ\n"
         "        0.0000        0.0000        0.0000                  ANTENNA: DELTA H/E/N\n"
         "G    4 C1C L1C L1X L2W                                      SYS / # / OBS TYPES\n"
      << shift_records
      << "    30.000                                                  INTERVAL\n"
         "  2024     5     8    19    25    0.0000000     GPS         TIME OF FIRST OBS\n"
         "                                                            END OF HEADER\n"
      << "> 2024 05 08 19 25  0.0000000  0  " << kSats.size() << "\n";
    double v = 20000000.0;
    for (const auto& id : kSats) {
        f << id << fld(v) << fld(v * 5.0 + 0.123) << fld(v * 5.0 + 0.456)
          << fld(v * 4.0 + 0.789) << "\n";
        v += 1000000.0;
    }
    return path;
}

void loadConfig(GnssConfig& cfg, LogManager& lm, const std::filesystem::path& dir,
                const std::filesystem::path& obs, bool apply_phase_shifts) {
    const auto jpath = dir / (obs.stem().string() + ".json");
    std::ofstream j(jpath);
    std::string obs_fwd = obs.string();
    for (char& c : obs_fwd) if (c == '\\') c = '/';
    j << "{\n"
      << "  \"processing\": { \"mode\": \"ppp\" },\n"
      << "  \"model_corrections\": { \"apply_phase_shifts\": "
      << (apply_phase_shifts ? "true" : "false") << " },\n"
      << "  \"files\": { \"observation_path\": \"" << obs_fwd << "\" }\n"
      << "}\n";
    j.close();
    ASSERT_TRUE(cfg.loadFromJson(jpath, lm)) << "config load failed";
}

std::filesystem::path testDir() {
    const auto dir = gnss_test::tempDir() / "gnss_rinex3_phase_shift";
    std::filesystem::create_directories(dir);
    return dir;
}

const RinexObservation* findSat(const std::vector<RinexObservation>& v,
                                const std::string& sat) {
    for (const auto& o : v) if (o.satellite == sat) return &o;
    return nullptr;
}

/// Parse @p obs and hand back the parser (observations and header).
struct Parsed {
    LogManager lm;
    GnssConfig cfg;
    std::unique_ptr<ProductParser> pp;
    std::unique_ptr<RinexParser> rp;
};

std::unique_ptr<Parsed> parseFixture(const std::filesystem::path& dir,
                                     const std::filesystem::path& obs, bool apply) {
    auto p = std::make_unique<Parsed>();
    loadConfig(p->cfg, p->lm, dir, obs, apply);
    p->pp = std::make_unique<ProductParser>(p->cfg, p->lm);
    p->rp = std::make_unique<RinexParser>(p->cfg, p->lm, *p->pp);
    EXPECT_TRUE(p->rp->parse());
    return p;
}

double shiftOf(const RinexHeader& h, const std::string& sat, const std::string& code) {
    return h.phase_shifts_by_sat.at(sat).at(code);
}

} // namespace

// --------------------------------------------------------------------------
// (a) count column and satellite list, continuation records included
// --------------------------------------------------------------------------

TEST(Rinex3PhaseShiftTest, CountAndSatelliteListAreReadFromTheSpecColumns) {
    const auto dir = testDir();
    const auto obs = writeObsFixture(dir, "arht_G_30S_MO.rnx", kArhtShiftRecords);
    auto p = parseFixture(dir, obs, true);
    const auto& h = p->rp->getHeader();

    // "09" is nine listed satellites, not a system-wide record.
    ASSERT_EQ(h.phase_shifts.count(System::GPS), 1u);
    EXPECT_EQ(h.phase_shifts.at(System::GPS).count("L1X"), 0u)
        << "a per-satellite record was stored as system-wide";
    for (const char* s : { "G01", "G02", "G03", "G04", "G11", "G14", "G18", "G23", "G28" }) {
        ASSERT_TRUE(h.phase_shifts_by_sat.count(s)) << s;
        EXPECT_DOUBLE_EQ(shiftOf(h, s, "L1X"), -0.25) << s;
    }
    EXPECT_EQ(h.phase_shifts_by_sat.at("G05").count("L1X"), 0u);

    // "32" is read whole, so all three continuation records are consumed and
    // their satellites stored (G11..G32 exist only on the continuations).
    for (int prn = 1; prn <= 32; ++prn) {
        char id[8];
        std::snprintf(id, sizeof id, "G%02d", prn);
        ASSERT_TRUE(h.phase_shifts_by_sat.count(id)) << id;
        EXPECT_EQ(h.phase_shifts_by_sat.at(id).count("L1C"), 1u) << id;
        EXPECT_DOUBLE_EQ(shiftOf(h, id, "L1C"), 0.0) << id;
    }
    EXPECT_EQ(h.phase_shifts_by_sat.size(), 32u);

    // The system-wide record after the lists is parsed as system-wide.
    EXPECT_DOUBLE_EQ(h.phase_shifts.at(System::GPS).at("L2W"), 0.25);
}

// --------------------------------------------------------------------------
// (b) application with the knob on
// --------------------------------------------------------------------------

TEST(Rinex3PhaseShiftTest, PerSatelliteShiftReachesOnlyTheListedSatellites) {
    const auto dir = testDir();
    const auto on_obs = writeObsFixture(dir, "arht_on_G_30S_MO.rnx", kArhtShiftRecords);
    const auto ref_obs = writeObsFixture(dir, "ref_on_G_30S_MO.rnx", "");
    auto on = parseFixture(dir, on_obs, true);
    auto ref = parseFixture(dir, ref_obs, true);   // no shift records: raw phases
    const auto& von = on->rp->getObservations();
    const auto& vref = ref->rp->getObservations();

    for (const auto& id : kSats) {
        const auto* a = findSat(von, id);
        const auto* r = findSat(vref, id);
        ASSERT_NE(a, nullptr) << id;
        ASSERT_NE(r, nullptr) << id;
        const bool listed_l1x = (id == "G01" || id == "G11" || id == "G28");
        // L1X: -0.25 cyc subtracted on the nine listed satellites only.
        EXPECT_NEAR(a->measurements.at("L1X"),
                    r->measurements.at("L1X") + (listed_l1x ? 0.25 : 0.0), 1e-9) << id;
        // L1C: listed with a zero shift, so unchanged.
        EXPECT_EQ(a->measurements.at("L1C"), r->measurements.at("L1C")) << id;
        // L2W: system-wide +0.25 cyc, so subtracted on every satellite.
        EXPECT_NEAR(a->measurements.at("L2W"), r->measurements.at("L2W") - 0.25, 1e-9) << id;
        // Code is never shifted.
        EXPECT_EQ(a->measurements.at("C1C"), r->measurements.at("C1C")) << id;
    }
}

// --------------------------------------------------------------------------
// (c) a list shorter than its count is not widened, the next record survives
// --------------------------------------------------------------------------

TEST(Rinex3PhaseShiftTest, MissingContinuationIsNotWidenedAndNextRecordSurvives) {
    const auto dir = testDir();
    const auto obs = writeObsFixture(dir, "short_G_30S_MO.rnx", kShortListRecords);
    auto p = parseFixture(dir, obs, true);
    const auto& h = p->rp->getHeader();

    // Count 15, ten listed: stored for the ten, never as system-wide.
    EXPECT_EQ(h.phase_shifts.count(System::GPS), 0u);
    for (int prn = 1; prn <= 10; ++prn) {
        char id[8];
        std::snprintf(id, sizeof id, "G%02d", prn);
        ASSERT_TRUE(h.phase_shifts_by_sat.count(id)) << id;
        EXPECT_DOUBLE_EQ(shiftOf(h, id, "L2W"), 0.25) << id;
    }
    // The L1X record that followed was not swallowed as a continuation.
    ASSERT_TRUE(h.phase_shifts_by_sat.count("G28"));
    EXPECT_DOUBLE_EQ(shiftOf(h, "G28", "L1X"), -0.25);
    EXPECT_DOUBLE_EQ(shiftOf(h, "G01", "L1X"), -0.25);
    EXPECT_EQ(h.phase_shifts_by_sat.at("G05").count("L1X"), 0u);
    // G11 carries L1X only: it is not in the ten-satellite L2W list.
    EXPECT_EQ(h.phase_shifts_by_sat.at("G11").count("L2W"), 0u);
}

// --------------------------------------------------------------------------
// (d) default path: knob off, observations independent of the shift records
// --------------------------------------------------------------------------

TEST(Rinex3PhaseShiftTest, KnobOffLeavesEveryObservationAsRecorded) {
    const auto dir = testDir();
    const auto off_obs = writeObsFixture(dir, "arht_off_G_30S_MO.rnx", kArhtShiftRecords);
    const auto ref_obs = writeObsFixture(dir, "ref_off_G_30S_MO.rnx", "");
    auto off = parseFixture(dir, off_obs, false);
    auto ref = parseFixture(dir, ref_obs, false);
    const auto& voff = off->rp->getObservations();
    const auto& vref = ref->rp->getObservations();
    ASSERT_EQ(voff.size(), vref.size());
    ASSERT_EQ(voff.size(), kSats.size());
    for (size_t i = 0; i < voff.size(); ++i) {
        EXPECT_EQ(voff[i].satellite, vref[i].satellite);
        for (const char* code : { "C1C", "L1C", "L1X", "L2W" }) {
            ASSERT_TRUE(voff[i].measurements.count(code)) << voff[i].satellite << " " << code;
            // Bit-exact: with the knob off, no shift is ever subtracted.
            EXPECT_EQ(voff[i].measurements.at(code), vref[i].measurements.at(code))
                << voff[i].satellite << " " << code;
        }
    }
}
