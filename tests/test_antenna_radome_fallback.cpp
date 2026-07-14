/**
 * @file test_antenna_radome_fallback.cpp
 * @brief Unit tests for the always-on receiver-antenna radome→NONE fallback
 *        (Tier 1) and the exclude_unmodeled_antenna_stations config toggle
 *        (Tier 2).
 *
 * Tier 1 (Antenna::matchReceiverAntenna): a RINEX receiver antenna whose
 * (type, radome) has no exact ATX entry retries the SAME antenna type with a
 * "NONE" radome — the documented IGS/EPN practice of borrowing a physical
 * antenna's radome-agnostic calibration (mm-level).  These tests lock that the
 * fallback (1) recovers an uncalibrated-radome station to its type's NONE
 * entry, (2) is flagged distinctly from an exact match, (3) NEVER remaps the
 * antenna TYPE (a truly unknown model still resolves to nothing — the Tier-2
 * exclusion predicate), (4) leaves already-matched lookups byte-identical
 * (exact match wins, fallback never fires), and (5) does not cross to a
 * different type's NONE entry.
 *
 * Tier 2 (GnssConfig): the network.exclude_unmodeled_antenna_stations toggle
 * parses and defaults OFF (byte-identical).
 */

#include <gtest/gtest.h>

#include "model/Antenna.hpp"
#include "products/ProductTypes.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

// Build a 20-char ANTEX/RINEX receiver antenna field: a 16-char left-justified
// model region followed by a 4-char radome code — the exact layout the
// production RINEX parser hands to the matcher (line.substr(20,20)).
std::string rxField(const std::string& model, const std::string& radome) {
    std::string s = model.substr(0, std::min<size_t>(16, model.size()));
    s.append(16 - s.size(), ' ');
    std::string r = radome.substr(0, std::min<size_t>(4, radome.size()));
    r.append(4 - r.size(), ' ');
    return s + r;  // 20 chars
}

// A small ATX antenna list.  Receiver ATX entry types are stored in the
// underscore-normalized "<model>_<radome>" form (mirrors ProductParser).  One
// entry carries a PCO so we can prove the matcher returns the real entry whose
// per-frequency data is untouched (the frequency chain is orthogonal).
std::vector<AntennaData> makeAntennas() {
    std::vector<AntennaData> v;
    AntennaData a;
    a.type = "AOAD/M_B_NONE";
    a.pco["G01"] = Eigen::Vector3d(0.0, 0.0, 0.0837);
    v.push_back(a);
    AntennaData b; b.type = "TRM59800.00_NONE"; v.push_back(b);
    AntennaData c; c.type = "SEPCHOKE_B3E6_SPKE"; v.push_back(c);  // calibrated non-NONE radome
    return v;
}

} // namespace

// (1) An uncalibrated radome on a known antenna resolves to that antenna's NONE
// entry and is flagged as a radome→NONE match.
TEST(AntennaRadomeFallback, UncalibratedRadomeResolvesToNone) {
    const auto antennas = makeAntennas();
    const auto m = Antenna::matchReceiverAntenna(
        antennas, rxField("AOAD/M_B", "OSOD"), /*enable_type_substitution=*/false);
    ASSERT_NE(m.entry, nullptr);
    EXPECT_EQ(m.entry->type, "AOAD/M_B_NONE");
    EXPECT_TRUE(m.via_radome_none);
    EXPECT_TRUE(m.type_substitution.empty());
    // The returned entry is the real object — its per-frequency PCO is intact
    // (radome→NONE swaps the calibration entry, not the frequency handling).
    ASSERT_EQ(m.entry->pco.count("G01"), 1u);
    EXPECT_DOUBLE_EQ(m.entry->pco.at("G01").z(), 0.0837);
}

// (2) An exact (type, NONE) match is NOT flagged as a radome fallback — it hit
// directly.  Byte-identical to the historical behaviour.
TEST(AntennaRadomeFallback, ExactNoneMatchIsNotFlagged) {
    const auto antennas = makeAntennas();
    const auto m = Antenna::matchReceiverAntenna(
        antennas, rxField("AOAD/M_B", "NONE"), false);
    ASSERT_NE(m.entry, nullptr);
    EXPECT_EQ(m.entry->type, "AOAD/M_B_NONE");
    EXPECT_FALSE(m.via_radome_none);
}

// (3) An exact match on a CALIBRATED non-NONE radome wins at the primary pass;
// the radome→NONE step never fires (there is no ...NONE entry to steal it, and
// exact match precedes the fallback anyway) — matched lookups are byte-identical.
TEST(AntennaRadomeFallback, CalibratedRadomeExactMatchWinsNoFallback) {
    const auto antennas = makeAntennas();
    const auto m = Antenna::matchReceiverAntenna(
        antennas, rxField("SEPCHOKE_B3E6", "SPKE"), false);
    ASSERT_NE(m.entry, nullptr);
    EXPECT_EQ(m.entry->type, "SEPCHOKE_B3E6_SPKE");
    EXPECT_FALSE(m.via_radome_none);
}

// (4) A genuinely unknown antenna TYPE still resolves to nothing even after the
// radome→NONE fallback — the predicate the Tier-2 hard exclusion acts on.
TEST(AntennaRadomeFallback, UnknownTypeStillNoMatch) {
    const auto antennas = makeAntennas();
    const auto m = Antenna::matchReceiverAntenna(
        antennas, rxField("NOSUCHANT", "OSOD"), false);
    EXPECT_EQ(m.entry, nullptr);
    EXPECT_FALSE(m.via_radome_none);

    // Enabling the opt-in TYPE-remap table (empty by default) does not rescue it.
    const auto m2 = Antenna::matchReceiverAntenna(
        antennas, rxField("NOSUCHANT", "OSOD"), /*enable_type_substitution=*/true);
    EXPECT_EQ(m2.entry, nullptr);
}

// (5) The radome→NONE fallback is TYPE-preserving: it must not cross to a
// different antenna's NONE entry.  With only TRM's NONE present, an
// uncalibrated-radome AOAD antenna resolves to nothing (not to TRM).
TEST(AntennaRadomeFallback, DoesNotCrossAntennaTypes) {
    std::vector<AntennaData> only_trm;
    AntennaData b; b.type = "TRM59800.00_NONE"; only_trm.push_back(b);
    const auto m = Antenna::matchReceiverAntenna(
        only_trm, rxField("AOAD/M_B", "OSOD"), false);
    EXPECT_EQ(m.entry, nullptr);
}

// (6) A blank/short field carries no distinct radome, so there is nothing to
// override — the fallback must not fabricate a NONE variant.
TEST(AntennaRadomeFallback, BlankRadomeNoFabricatedFallback) {
    std::vector<AntennaData> antennas;
    AntennaData a; a.type = "AOAD/M_B_NONE"; antennas.push_back(a);
    // A bare, already-trimmed type (no 20-char radome region): substring match
    // finds the NONE entry directly, and it is NOT counted as a radome fallback.
    const auto m = Antenna::matchReceiverAntenna(antennas, "AOAD/M_B", false);
    ASSERT_NE(m.entry, nullptr);
    EXPECT_EQ(m.entry->type, "AOAD/M_B_NONE");
    EXPECT_FALSE(m.via_radome_none);  // matched via substring, not the radome→NONE step
}

// (7) Ambiguity accounting: when the winning pass has >1 substring candidate and
// no exact hit, ambiguous_candidates reflects the count and the first (list
// order) entry is chosen.
TEST(AntennaRadomeFallback, AmbiguousSubstringCountReported) {
    std::vector<AntennaData> antennas;
    AntennaData a; a.type = "PRE_AOAD/M_B_NONE_X"; antennas.push_back(a);  // contains, not equal
    AntennaData b; b.type = "AOAD/M_B_NONE_TWO";   antennas.push_back(b);  // contains, not equal
    const auto m = Antenna::matchReceiverAntenna(
        antennas, rxField("AOAD/M_B", "NONE"), false);
    ASSERT_NE(m.entry, nullptr);
    EXPECT_EQ(m.entry->type, "PRE_AOAD/M_B_NONE_X");  // first in list order
    EXPECT_GE(m.ambiguous_candidates, 2);
    EXPECT_FALSE(m.via_radome_none);  // primary substring pass, not the fallback
}

// ---------------------------------------------------------------------------
// Tier 2: config toggle contract.
// ---------------------------------------------------------------------------

namespace {

fs::path writeTempConfig(const std::string& tag, const std::string& network_extra) {
    const fs::path p = fs::temp_directory_path() / ("radome_net_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << "{\n"
         "  \"processing\": {\n"
         "    \"mode\": \"ppp-rtk-network\",\n"
         "    \"active_systems\": [ \"GPS\", \"Galileo\" ],\n"
         "    \"signal_priorities\": { \"GPS\": \"WC\", \"Galileo\": \"CB\" }\n"
         "  },\n"
         "  \"network\": {\n"
         "    \"clock_datum_station\": \"AAAA\",\n"
      << network_extra <<
         "    \"stations\": [\n"
         "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
         "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
         "    ]\n"
         "  }\n"
         "}\n";
    f.close();
    return p;
}

} // namespace

// The toggle parses when present.
TEST(ExcludeUnmodeledAntennaConfig, ParsesWhenSet) {
    LogManager log;
    const fs::path p = writeTempConfig(
        "on", "    \"exclude_unmodeled_antenna_stations\": true,\n");
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.networkExcludeUnmodeledAntennaStations());
    std::error_code ec; fs::remove(p, ec);
}

// Default is OFF (byte-identical: every loaded station is built).
TEST(ExcludeUnmodeledAntennaConfig, DefaultsOff) {
    LogManager log;
    const fs::path p = writeTempConfig("default", "");
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(cfg.networkExcludeUnmodeledAntennaStations());
    std::error_code ec; fs::remove(p, ec);
}
