/**
 * @file test_raim_pool_data_rows_only.cpp
 * @brief The carried-phase RAIM pools restricted to observation phase rows
 *        (@c network.raim_pool_data_rows_only,
 *        docs/refound/RPA_raim_pool_data_rows_2026-10-08.md).
 *
 * The contracts:
 *  - the pools' admission predicate (@c raimPoolAdmitsRow) with the knob off is the
 *    declared rule exactly: the coefficient clears 0.5 and
 *    @c raimPoolAdmitsKind admits the kind, whatever column subject is passed;
 *  - with the knob on it admits an observation phase row (@c data_phase,
 *    @c data_phase_second) whose tag names the column's satellite and signal, and nothing
 *    else: no carried-prior kind, no SD-EWL row, no phase row of another satellite or
 *    signal, whatever the coefficient test says; a column whose subject does not parse
 *    admits nothing;
 *  - the own-column check counts, per phase row and column, agreement and both kinds of
 *    disagreement between the tag and the coefficient test, and ignores other kinds;
 *  - the knob defaults off, loads on with @c network.raim, and the loader disables it
 *    without @c network.raim (there is no pool to restrict).
 *
 * Suite ownership: `RaimPoolDataRowsOnly` lives only in this file.
 */
#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "network/assembly/Contributions.hpp"
#include "network/qc/Raim.hpp"          // RaimPoolAdmissionTally
#include "network/assembly/RowClassify.hpp"
#include "logging/LogManager.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

using CK = ContributionKind;

/// Every family, in enum order.
std::vector<CK> allKinds() {
    std::vector<CK> out;
    for (std::size_t k = 0; k < kContributionKinds; ++k) out.push_back(static_cast<CK>(k));
    return out;
}

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("raim_pool_rpa_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

/// A minimal loadable PPP-RTK network config; @p network_extra is spliced into the
/// network block (each entry must end with a comma).
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

}  // namespace

TEST(RaimPoolDataRowsOnly, OffIsTheDeclaredKindRule) {
    const RowTag own   = raimPoolColumnSubject("G05", "L1W");
    const RowTag other = raimPoolColumnSubject("G06", "L2W");
    for (const CK k : allKinds()) {
        const RowTag row = makeRowTag(k, "G05", "L1W");
        EXPECT_EQ(raimPoolAdmitsRow(row, true, own, false), raimPoolAdmitsKind(k))
            << contributionKindName(k);
        EXPECT_EQ(raimPoolAdmitsRow(row, true, other, false), raimPoolAdmitsKind(k))
            << contributionKindName(k) << ": the column subject is not read when off";
        EXPECT_FALSE(raimPoolAdmitsRow(row, false, own, false))
            << contributionKindName(k) << ": off, a row that does not clear 0.5 never enters";
    }
    EXPECT_FALSE(raimPoolAdmitsRow(RowTag{}, true, own, false)) << "an untagged row";
}

TEST(RaimPoolDataRowsOnly, OnAdmitsTheOwnPhaseRowsOnly) {
    const RowTag col = raimPoolColumnSubject("G05", "L1W");
    for (const CK k : allKinds()) {
        const bool phase = (k == CK::DataPhase || k == CK::DataPhaseSecond);
        const RowTag row = makeRowTag(k, "G05", "L1W");
        EXPECT_EQ(raimPoolAdmitsRow(row, true, col, true), phase) << contributionKindName(k);
        EXPECT_EQ(raimPoolAdmitsRow(row, false, col, true), phase)
            << contributionKindName(k) << ": on, the tag decides, not the coefficient";
    }
    // The carried-prior rows the off rule admits are refused, whatever they clear.
    for (const CK k : {CK::CouplingScalar, CK::BlockAmbiguityCarry,
                       CK::BlockZwdCarry, CK::BlockIonoCarry, CK::BlockRxClockCarry,
                       CK::BlockRxCbCarry, CK::BlockPhaseColourCarry, CK::SdEwl}) {
        EXPECT_TRUE(raimPoolAdmitsRow(makeRowTag(k, "G05", "L1W"), true, col, false))
            << contributionKindName(k);
        EXPECT_FALSE(raimPoolAdmitsRow(makeRowTag(k, "G05", "L1W"), true, col, true))
            << contributionKindName(k);
    }
    // A phase row enters the pool of its own column only.
    EXPECT_FALSE(raimPoolAdmitsRow(makeRowTag(CK::DataPhase, "G06", "L1W"), true, col, true))
        << "another satellite";
    EXPECT_FALSE(raimPoolAdmitsRow(makeRowTag(CK::DataPhase, "G05", "L2W"), true, col, true))
        << "another signal of the same satellite";
    EXPECT_FALSE(raimPoolAdmitsRow(makeRowTag(CK::DataPhase, "G05", "L1C"), true, col, true))
        << "another tracking code of the same band";
    EXPECT_TRUE(raimPoolAdmitsRow(makeRowTag(CK::DataPhaseSecond, "G05", "L2L"), true,
                                  raimPoolColumnSubject("G05", "L2L"), true))
        << "a second phase channel row in the pool of its own column";
    EXPECT_FALSE(raimPoolAdmitsRow(makeRowTag(CK::DataPhase, "G05"), true, col, true))
        << "a phase row whose tag names no signal names no column";
    // A column whose subject does not parse admits nothing (and its phase rows are then
    // counted as disagreements by the own-column check).
    const RowTag unparsed = raimPoolColumnSubject("G05", "");
    EXPECT_FALSE(rowTagNamesColumn(makeRowTag(CK::DataPhase, "G05"), unparsed));
    EXPECT_FALSE(raimPoolAdmitsRow(makeRowTag(CK::DataPhase, "G05"), true, unparsed, true));
    EXPECT_FALSE(raimPoolAdmitsRow(RowTag{}, true, RowTag{}, true)) << "two default tags";
}

TEST(RaimPoolDataRowsOnly, OwnColumnCheckCountsPhaseRowsOnly) {
    RaimPoolAdmissionTally t;
    t.noteOwnColumn(CK::DataPhase, true, true);           // agree
    t.noteOwnColumn(CK::DataPhaseSecond, true, true);     // agree
    t.noteOwnColumn(CK::DataPhase, true, false);          // clears another column
    t.noteOwnColumn(CK::DataPhase, false, true);          // own column at or below 0.5
    t.noteOwnColumn(CK::DataPhase, false, false);         // neither: not counted
    t.noteOwnColumn(CK::BlockAmbiguityCarry, true, false);  // not a phase row: ignored
    t.noteOwnColumn(CK::DataCode, true, true);              // not a phase row: ignored
    EXPECT_EQ(t.own_col_agree, 2);
    EXPECT_EQ(t.own_col_cleared_other, 1);
    EXPECT_EQ(t.own_col_not_cleared, 1);
    const std::string text = t.ownColumnText();
    EXPECT_NE(text.find("agree 2"), std::string::npos) << text;
    EXPECT_NE(text.find("another column 1"), std::string::npos) << text;
    EXPECT_NE(text.find("at or below 0.5 1"), std::string::npos) << text;
    // The off line is unchanged by the new members.
    EXPECT_EQ(RaimPoolAdmissionTally{}.text(),
              "walks 0; admitted: none; refused by kind: none");
}

TEST(RaimPoolDataRowsOnly, KnobDefaultsOff) {
    LogManager log;
    const fs::path p = writeTempConfig("default", networkConfig("    \"raim\": true,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.networkRaim());
    EXPECT_FALSE(cfg.networkRaimPoolDataRowsOnly());
    std::error_code ec; fs::remove(p, ec);
}

TEST(RaimPoolDataRowsOnly, KnobLoadsWithRaim) {
    LogManager log;
    const fs::path p = writeTempConfig("armed",
        networkConfig("    \"raim\": true,\n"
                      "    \"phase_leveling\": true,\n"
                      "    \"raim_pool_data_rows_only\": true,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.networkRaimPoolDataRowsOnly());
    std::error_code ec; fs::remove(p, ec);
}

TEST(RaimPoolDataRowsOnly, KnobDisabledWithoutRaim) {
    LogManager log;
    const fs::path p = writeTempConfig("noraim",
        networkConfig("    \"raim_pool_data_rows_only\": true,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(cfg.networkRaim());
    EXPECT_FALSE(cfg.networkRaimPoolDataRowsOnly()) << "no RAIM pool to restrict";
    std::error_code ec; fs::remove(p, ec);
}
