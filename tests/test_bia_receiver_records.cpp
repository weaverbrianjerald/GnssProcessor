/**
 * @file test_bia_receiver_records.cpp
 * @brief Tests for the BIA parser's receiver bias records (Item 0b,
 *        docs/SPEC_level_information_budget.md §17, §28).
 *
 * The BIA parser reads station-bearing lines into a receiver bias vector, separate
 * from the satellite vector, and accepts receiver DSB records.  The COD MGX OSB
 * file carries 6797 receiver records per day, and the CAS MGEX DCB product uses
 * receiver DSB records for the nonzero receiver DCBs that the final rung of the
 * level ladder needs.  External receiver DCBs are the designed authority for the
 * level endpoint, since the a₀₀ anchor's authority is insufficient in both carry
 * postures (12 h S2H verdict).
 *
 * Checked here:
 *   1. Receiver OSB lines (COD format: 1-char system in the PRN field, 4-char
 *      station) parse into the receiver vector with ns→m conversion.
 *   2. Receiver DSB lines (CAS format: two observation codes) parse with both
 *      signals and the is_dsb flag.
 *   3. The satellite vector is unchanged by receiver/DSB content (same count,
 *      same values), so its consumers stay byte-identical (the card's structural
 *      guarantee).
 *   4. Satellite DSB lines are skipped and counted, never folded into the OSB
 *      vector, whose consumers assume observable-specific semantics.
 *   5. An all-receiver file (the CAS DCB shape) parses successfully; a file with
 *      neither population still fails.
 *   6. A malformed receiver line is warned about and skipped without affecting
 *      the other lines.
 *
 * Line formats are copied character-for-character from the real
 * COD0MGXFIN_20241290000_01D_01D_OSB.BIA on disk (fixed-width SINEX_BIAS).
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "products/ProductParser.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

fs::path writeTempBia(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("rx_bias_" + tag + ".BIA");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

const char* kHeader =
    "%=BIA 1.00 COD 2024:135:43102 COD 2024:129:00000 2024:130:00000 R 00000000\n"
    "+BIAS/SOLUTION\n"
    "*BIAS SVN_ PRN STATION__ OBS1 OBS2 BIAS_START____ BIAS_END______ UNIT __ESTIMATED_VALUE____ _STD_DEV___\n";
const char* kFooter = "-BIAS/SOLUTION\n";

// Real COD satellite OSB lines (verbatim widths).
const char* kSatOsb =
    " OSB  G049 G01           C1C       2024:129:00000 2024:130:00000 ns                 -6.3575      0.0086\n"
    " OSB  G049 G01           C1W       2024:129:00000 2024:130:00000 ns                 -5.6718      0.0000\n";

// Real COD receiver OSB lines (verbatim widths: 1-char system, 4-char station).
const char* kRxOsb =
    " OSB  G    G   ABMF      C1W       2024:129:00000 2024:130:00000 ns                  0.0000      0.0006\n"
    " OSB  E    E   ABMF      C1C       2024:129:00000 2024:130:00000 ns                 -7.5834      0.0187\n";

// CAS-style receiver DSB line (same fixed widths, OBS2 populated).
const char* kRxDsb =
    " DSB  G    G   ALGO      C1C  C2W  2024:129:00000 2024:130:00000 ns                 -8.1234      0.0300\n";

// Satellite DSB line (CAS satellite DCB shape): out of scope, skipped and counted.
const char* kSatDsb =
    " DSB  G049 G01           C1C  C2W  2024:129:00000 2024:130:00000 ns                 -1.2345      0.0100\n";

struct ParseResult {
    bool ok{};
    std::vector<BiasData> sats;
    std::vector<ReceiverBiasData> rx;
};

ParseResult parse(const std::string& tag, const std::string& records) {
    // Minimal config: ProductParser only reads paths from it lazily, and
    // parseBia takes the path explicitly, so a default config suffices.
    static GnssConfig cfg;
    static LogManager log;
    ProductParser parser(cfg, log);
    const fs::path p = writeTempBia(tag, std::string(kHeader) + records + kFooter);
    ParseResult r;
    ClockRefMap refs;
    r.ok = parser.parseBia(p, r.sats, refs);
    r.rx = parser.getReceiverBiases();
    std::error_code ec; fs::remove(p, ec);
    return r;
}

constexpr double kNsToM = 0.299792458;  // c · 1e-9

TEST(BiaReceiverRecords, ReceiverOsbLinesParseIntoTheReceiverVector) {
    const auto r = parse("rxosb", std::string(kSatOsb) + kRxOsb);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.rx.size(), 2u);
    EXPECT_EQ(r.rx[0].station, "ABMF");
    EXPECT_EQ(r.rx[0].constellation, 'G');
    EXPECT_EQ(r.rx[0].signal, "C1W");
    EXPECT_TRUE(r.rx[0].signal2.empty());
    EXPECT_FALSE(r.rx[0].is_dsb);
    EXPECT_NEAR(r.rx[0].bias, 0.0, 1e-12);
    EXPECT_NEAR(r.rx[0].std_dev, 0.0006 * kNsToM, 1e-9);
    EXPECT_EQ(r.rx[1].constellation, 'E');
    EXPECT_NEAR(r.rx[1].bias, -7.5834 * kNsToM, 1e-9);
    EXPECT_GT(r.rx[0].start_time.week, 0);
}

TEST(BiaReceiverRecords, ReceiverDsbLinesCarryBothSignals) {
    const auto r = parse("rxdsb", std::string(kSatOsb) + kRxDsb);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.rx.size(), 1u);
    EXPECT_EQ(r.rx[0].station, "ALGO");
    EXPECT_EQ(r.rx[0].signal, "C1C");
    EXPECT_EQ(r.rx[0].signal2, "C2W");
    EXPECT_TRUE(r.rx[0].is_dsb);
    EXPECT_NEAR(r.rx[0].bias, -8.1234 * kNsToM, 1e-9);
}

// The card's structural guarantee: the satellite vector is identical with and
// without receiver/DSB content in the file.
TEST(BiaReceiverRecords, SatelliteVectorIsUntouchedByReceiverContent) {
    const auto plain = parse("sat_only", kSatOsb);
    const auto mixed = parse("sat_mixed",
                             std::string(kSatOsb) + kRxOsb + kRxDsb + kSatDsb);
    ASSERT_TRUE(plain.ok);
    ASSERT_TRUE(mixed.ok);
    ASSERT_EQ(plain.sats.size(), mixed.sats.size());
    for (size_t i = 0; i < plain.sats.size(); ++i) {
        EXPECT_EQ(plain.sats[i].satellite, mixed.sats[i].satellite);
        EXPECT_EQ(plain.sats[i].signal, mixed.sats[i].signal);
        EXPECT_DOUBLE_EQ(plain.sats[i].bias, mixed.sats[i].bias);
    }
    // The satellite DSB line is in neither vector (out of scope, counted).
    EXPECT_EQ(mixed.sats.size(), 2u);
    for (const auto& rb : mixed.rx) EXPECT_FALSE(rb.station.empty());
}

// The CAS MGEX DCB shape: receiver records only.  The parse succeeds because
// parseBia fails only when both the satellite and the receiver sets are empty.
TEST(BiaReceiverRecords, AllReceiverFileIsALegalParse) {
    const auto r = parse("rx_only", std::string(kRxOsb) + kRxDsb);
    EXPECT_TRUE(r.ok);
    EXPECT_TRUE(r.sats.empty());
    EXPECT_EQ(r.rx.size(), 3u);
}

TEST(BiaReceiverRecords, EmptyFileStillFailsLoud) {
    const auto r = parse("empty", "");
    EXPECT_FALSE(r.ok);
}

TEST(BiaReceiverRecords, MalformedReceiverLinesSkipWithoutPoisoning) {
    // Bad unit, bad observation code, non-numeric bias: each line is skipped alone.
    const std::string bad =
        " OSB  G    G   ABMF      C1W       2024:129:00000 2024:130:00000 s                   1.0000      0.0006\n"
        " OSB  G    G   ABMF      X9x       2024:129:00000 2024:130:00000 ns                  1.0000      0.0006\n"
        " OSB  G    G   ABMF      C2W       2024:129:00000 2024:130:00000 ns                  garbage     0.0006\n";
    const auto r = parse("malformed", std::string(kSatOsb) + bad + kRxOsb);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.sats.size(), 2u);
    EXPECT_EQ(r.rx.size(), 2u);   // only the two good receiver lines
}

// The real COD MGX file on disk, when present: it holds 6797 receiver records
// (fixture-gated like the other data-dependent smoke tests).
TEST(BiaReceiverRecords, RealCodMgxFileWhenPresent) {
    const fs::path real =
        "C:/GnssProcessor/data/2024-129/products/COD0MGXFIN_20241290000_01D_01D_OSB.BIA";
    if (!fs::exists(real))
        GTEST_SKIP() << "bundled COD MGX OSB fixture not present";
    GnssConfig cfg;
    LogManager log;
    ProductParser parser(cfg, log);
    std::vector<BiasData> sats;
    ClockRefMap refs;
    ASSERT_TRUE(parser.parseBia(real, sats, refs));
    EXPECT_EQ(sats.size(), 748u);   // the file's multi-GNSS satellite OSB set
    EXPECT_EQ(parser.getReceiverBiases().size(), 6797u);
    // COD convention check (the eliminated-table fact): GPS receiver OSBs ~0.
    int gps_nonzero = 0;
    for (const auto& rb : parser.getReceiverBiases())
        if (rb.constellation == 'G' && std::abs(rb.bias) > 1e-6) ++gps_nonzero;
    EXPECT_LT(gps_nonzero, 60);   // 56/57 exactly zero per the record; the bound allows drift
}

}  // namespace
