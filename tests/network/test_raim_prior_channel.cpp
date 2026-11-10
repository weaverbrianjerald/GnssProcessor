/**
 * @file test_raim_prior_channel.cpp
 * @brief The post-fit RAIM's prior channel (@c network.raim_prior_channel,
 *        docs/record/refound/RPC_raim_prior_channel_2026-10-30.md).
 *
 * The contracts:
 *  - the subject of a carried-prior row follows its kind and its own column: a line of
 *    sight for the ambiguity, ionosphere and phase-colour block rows and a scalar coupling
 *    row that names a satellite; a receiver code bias, with its state key, for a code-bias
 *    block row (or a scalar row with no satellite) whose own column is one of the station's
 *    code-bias columns; unwired otherwise;
 *  - the station walk tests the coupling region only, once per row, with the pools'
 *    statistic and the redundancy normalization and floor, and counts the rows whose largest
 *    coefficient is on another carried column;
 *  - the table has one cell per (station, subject), the largest statistic of its rows; the
 *    scale is the channel's own robust scale, floored; the isolated-arc test flags a cell that
 *    stands out, and the flag goes to the response its subject takes;
 *  - a satellite the observation channel caught wholesale is not flagged, an arc it refloats
 *    is not refloated twice, and a closed maturity gate holds every flag;
 *  - the run-end lines say NEVER RAN, INERT, NO FLAG and the unwired warning in words;
 *  - the key defaults off, loads with @c network.raim and @c network.raim_attribution, and
 *    the loader disables it without either.
 *
 * Not certified here: that networkPostFitRaim passes these functions the right arguments and
 * takes the responses.  The seam needs a solved network; its wiring is read on the data arms
 * of the card (the run-end section against the T4_prior event rows).
 *
 * Suite ownership: `RaimPriorChannel` lives only in this file.
 */
#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "network/assembly/Contributions.hpp"
#include "network/driver/RunTypes.hpp"          // StationAssembly
#include "network/qc/RaimPriorChannel.hpp"
#include "logging/LogManager.hpp"

#include <Eigen/Dense>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "../support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

using CK = ContributionKind;
using SK = RaimPriorSubjectKind;

/// One station block: two observation rows, then a coupling region of @p tags.size() rows over
/// local columns 0..n-1 (row k's own column is k), with the residuals in @p y and unit weights.
StationAssembly coupledBlock(const std::vector<RowTag>& tags, const Eigen::MatrixXd& cpl_Ar) {
    const Eigen::Index n_cpl = static_cast<Eigen::Index>(tags.size());
    const Eigen::Index n_rows = 2 + n_cpl;
    StationAssembly R;
    R.ok  = true;
    R.A_s = Eigen::MatrixXd::Zero(n_rows, 1);
    R.A_r = Eigen::MatrixXd::Zero(n_rows, cpl_Ar.cols());
    R.w   = Eigen::VectorXd::Ones(n_rows);
    R.y   = Eigen::VectorXd::Zero(n_rows);
    R.A_r(0, 0) = 1.0;   // observation rows on the first column
    R.A_r(1, 0) = 1.0;
    R.A_r.bottomRows(n_cpl) = cpl_Ar;
    R.n_data_rows = 2;
    R.cpl_row_start = 2;
    for (Eigen::Index k = 0; k < n_cpl; ++k) R.cpl_row_local_cols.push_back(k);
    R.row_tags = {makeRowTag(CK::DataPhase, "G07", "L1C"), makeRowTag(CK::DataCode, "G07", "C1C")};
    for (const RowTag& t : tags) R.row_tags.push_back(t);
    return R;
}

/// A tested row of a synthetic table.
RaimPriorRow priorRow(int station, SK kind, const std::string& sat_or_key, double stat,
                      CK row_kind = CK::BlockAmbiguityCarry) {
    RaimPriorRow r;
    r.station   = station;
    r.kind      = row_kind;
    r.statistic = stat;
    r.subject.kind = kind;
    if (kind == SK::LineOfSight) r.subject.sat = sat_or_key;
    if (kind == SK::RxCodeBias)  r.subject.cb_key = sat_or_key;
    return r;
}

/// Five stations, each with four line-of-sight cells (G01 to G04) and one code-bias cell, all
/// at statistic 1.0; the caller raises one.
std::vector<RaimPriorRow> quietTable() {
    std::vector<RaimPriorRow> rows;
    for (int s = 0; s < 5; ++s) {
        for (const char* sat : {"G01", "G02", "G03", "G04"})
            rows.push_back(priorRow(s, SK::LineOfSight, sat, 1.0));
        rows.push_back(priorRow(s, SK::RxCodeBias, "GPS_ISC_C1C", 1.0, CK::BlockRxCbCarry));
    }
    return rows;
}

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = gnss_test::tempDir() / ("raim_prior_channel_" + tag + ".json");
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

TEST(RaimPriorChannel, SubjectFollowsTheKindAndTheOwnColumn) {
    const std::vector<RowTag> tags = {
        makeRowTag(CK::BlockAmbiguityCarry, "G07", "L1C"),            // 0: a line of sight
        RowTag{CK::BlockRxCbCarry, SatId{}, SignalId{}},              // 1: a code bias in pl_cb
        RowTag{CK::BlockZwdCarry, SatId{}, SignalId{}},               // 2: the wet delay
        makeRowTag(CK::CouplingScalar, "G09", "L2W"),                 // 3: a scalar ambiguity row
        RowTag{CK::CouplingScalar, SatId{}, SignalId{}},              // 4: a scalar row on a code bias
        RowTag{CK::CouplingScalar, SatId{}, SignalId{}},              // 5: a scalar row on nothing known
        RowTag{CK::BlockAmbiguityCarry, SatId{}, SignalId{}},         // 6: no satellite: unresolved
        RowTag{CK::BlockRxCbCarry, SatId{}, SignalId{}},              // 7: a code bias not in pl_cb
        makeRowTag(CK::BlockIonoCarry, "E11"),                        // 8: a line of sight
    };
    StationAssembly R = coupledBlock(tags, Eigen::MatrixXd::Identity(9, 9));
    R.pl_cb = {{"GPS_ISC_C1C", 1, 0.0}, {"GPS_DCB_C1W_C2W", 4, 0.0}};

    const auto at = [&](Eigen::Index k) { return raimPriorSubjectOf(R, 2 + k, k); };
    EXPECT_EQ(at(0).kind, SK::LineOfSight);
    EXPECT_EQ(at(0).sat, "G07");
    EXPECT_EQ(at(0).label(CK::BlockAmbiguityCarry), "G07");
    EXPECT_EQ(at(1).kind, SK::RxCodeBias);
    EXPECT_EQ(at(1).cb_key, "GPS_ISC_C1C");
    EXPECT_EQ(at(1).label(CK::BlockRxCbCarry), "rxcb:GPS_ISC_C1C");
    EXPECT_EQ(at(2).kind, SK::Unwired);
    EXPECT_EQ(at(2).label(CK::BlockZwdCarry), "block_zwd_carry");
    EXPECT_EQ(at(3).kind, SK::LineOfSight);
    EXPECT_EQ(at(3).sat, "G09");
    EXPECT_EQ(at(4).kind, SK::RxCodeBias);
    EXPECT_EQ(at(4).cb_key, "GPS_DCB_C1W_C2W");
    EXPECT_EQ(at(5).kind, SK::Unwired) << "a scalar row with no satellite on no code-bias column";
    EXPECT_EQ(at(6).kind, SK::Unwired) << "a line-of-sight kind whose tag names no satellite";
    EXPECT_EQ(at(7).kind, SK::Unwired) << "a code-bias row whose column has no state key";
    EXPECT_EQ(at(8).kind, SK::LineOfSight);
    EXPECT_EQ(at(8).sat, "E11");
    EXPECT_STREQ(raimPriorSubjectKindName(SK::LineOfSight), "los");
    EXPECT_STREQ(raimPriorSubjectKindName(SK::RxCodeBias), "rxcb");
    EXPECT_STREQ(raimPriorSubjectKindName(SK::Unwired), "unwired");
}

TEST(RaimPriorChannel, CollectTestsEachCoupledRowOnceAgainstItsOwnColumn) {
    // A whitened block of three columns (G05, G07, the C1C code bias), lower-triangular as
    // L^-1 is: row 1 has a larger coefficient on column 0 than on its own column.
    Eigen::MatrixXd Lin(3, 3);
    Lin << 2.0, 0.0, 0.0,
          -3.0, 1.5, 0.0,
           0.4, -0.2, 1.0;
    StationAssembly R = coupledBlock({makeRowTag(CK::BlockAmbiguityCarry, "G05", "L1C"),
                                      makeRowTag(CK::BlockAmbiguityCarry, "G07", "L1C"),
                                      RowTag{CK::BlockRxCbCarry, SatId{}, SignalId{}}},
                                     Lin);
    R.pl_cb = {{"GPS_ISC_C1C", 2, 0.0}};
    Eigen::VectorXd resid(5);
    resid << 9.0, -9.0, 0.5, -4.0, 1.5;   // the observation rows are not the channel's
    RaimPriorChannelTally tally;
    std::vector<RaimPriorRow> rows;
    const std::size_t n = raimPriorChannelCollect(R, 3, "SITE", resid, Eigen::VectorXd(),
                                                  /*normalize=*/false, 1e-6, 100.0, rows, tally);
    ASSERT_EQ(n, 3u);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0].row, 2);
    EXPECT_EQ(rows[0].own_col, 0);
    EXPECT_DOUBLE_EQ(rows[0].statistic, 0.5);
    EXPECT_DOUBLE_EQ(rows[0].own_coef, 2.0);
    EXPECT_DOUBLE_EQ(rows[0].max_other_coef, 0.0) << "row 0 of L^-1 touches its own column only";
    EXPECT_FALSE(rows[0].otherDominant());
    EXPECT_EQ(rows[1].own_col, 1);
    EXPECT_DOUBLE_EQ(rows[1].statistic, 4.0);
    EXPECT_DOUBLE_EQ(rows[1].own_coef, 1.5);
    EXPECT_DOUBLE_EQ(rows[1].max_other_coef, 3.0);
    EXPECT_EQ(rows[1].max_other_col, 0);
    EXPECT_TRUE(rows[1].otherDominant()) << "the Cholesky-order spread is counted";
    EXPECT_EQ(rows[2].subject.kind, SK::RxCodeBias);
    EXPECT_EQ(rows[2].subject.cb_key, "GPS_ISC_C1C");
    EXPECT_EQ(rows[1].station, 3);

    EXPECT_EQ(tally.rows[static_cast<std::size_t>(CK::BlockAmbiguityCarry)], 2);
    EXPECT_EQ(tally.rows[static_cast<std::size_t>(CK::BlockRxCbCarry)], 1);
    EXPECT_EQ(tally.rowsTested(), 3);
    EXPECT_EQ(tally.rows_other_dominant, 1);
    EXPECT_EQ(tally.station_walks, 1);
    EXPECT_DOUBLE_EQ(tally.max_statistic, 4.0);
    EXPECT_EQ(tally.max_station, "SITE");
    EXPECT_EQ(tally.max_label, "G07");
    EXPECT_EQ(tally.max_kind, CK::BlockAmbiguityCarry);
    EXPECT_DOUBLE_EQ(tally.max_tow, 100.0);
}

TEST(RaimPriorChannel, CollectNormalizesAndFloorsAsThePoolsDo) {
    StationAssembly R = coupledBlock({makeRowTag(CK::BlockAmbiguityCarry, "G05", "L1C"),
                                      makeRowTag(CK::BlockAmbiguityCarry, "G07", "L1C")},
                                     Eigen::MatrixXd::Identity(2, 2));
    R.w(2) = 4.0;                        // a scalar coupling row's weight enters as sqrt(w)
    Eigen::VectorXd resid(4);
    resid << 0.0, 0.0, 1.5, 2.0;
    Eigen::VectorXd row_r(4);
    row_r << 1.0, 1.0, 0.25, 1e-9;       // row 3 at the floor
    RaimPriorChannelTally tally;
    std::vector<RaimPriorRow> rows;
    raimPriorChannelCollect(R, 0, "SITE", resid, row_r, /*normalize=*/true, 1e-6, 0.0, rows, tally);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_DOUBLE_EQ(rows[0].statistic, 1.5 * 2.0 / 0.5) << "|v| sqrt(w) / sqrt(r)";
    EXPECT_EQ(tally.rows_floor, 1);

    // Without the normalization the floor is not applied.
    RaimPriorChannelTally t2;
    std::vector<RaimPriorRow> r2;
    raimPriorChannelCollect(R, 0, "SITE", resid, row_r, /*normalize=*/false, 1e-6, 0.0, r2, t2);
    ASSERT_EQ(r2.size(), 2u);
    EXPECT_DOUBLE_EQ(r2[0].statistic, 3.0);
    EXPECT_EQ(t2.rows_floor, 0);

    // A block without a coupling region has nothing to test.
    StationAssembly bare = R;
    bare.cpl_row_start = -1;
    std::vector<RaimPriorRow> r3;
    RaimPriorChannelTally t3;
    EXPECT_EQ(raimPriorChannelCollect(bare, 0, "SITE", resid, row_r, false, 1e-6, 0.0, r3, t3), 0u);
    EXPECT_EQ(t3.station_walks, 0);
}

TEST(RaimPriorChannel, CellsAreTheLargestRowOfEachSubject) {
    std::vector<RaimPriorRow> rows = quietTable();
    rows.push_back(priorRow(2, SK::LineOfSight, "G03", 2.5));   // a second row of station 2's G03
    const RaimPriorSelection sel =
        raimPriorChannelSelect(rows, 1.0, 6.0, {}, {}, /*mature=*/true);
    ASSERT_EQ(sel.cells.size(), 25u) << "five stations x (four satellites and one code bias)";
    ASSERT_EQ(sel.cell_row.size(), sel.cells.size());
    ASSERT_EQ(sel.interaction.size(), sel.cells.size());
    bool found = false;
    for (std::size_t c = 0; c < sel.cells.size(); ++c) {
        const auto& [sta, label, value] = sel.cells[c];
        EXPECT_EQ(rows[sel.cell_row[c]].station, sta);
        if (sta == 2 && label == "G03") {
            found = true;
            EXPECT_DOUBLE_EQ(value, 2.5);
            EXPECT_EQ(sel.cell_row[c], rows.size() - 1);
        }
        if (label == "rxcb:GPS_ISC_C1C") EXPECT_DOUBLE_EQ(value, 1.0);
    }
    EXPECT_TRUE(found);
    EXPECT_DOUBLE_EQ(sel.scale, 1.4826) << "1.4826 x the median statistic, above the floor";
    EXPECT_TRUE(sel.isolated.refloat.empty()) << "2.5 is not 6 scales above the table";
}

TEST(RaimPriorChannel, AFlagGoesToTheResponseOfItsSubject) {
    std::vector<RaimPriorRow> rows = quietTable();
    rows[4 * 5 + 2].statistic = 60.0;                   // station 4, G03
    rows[1 * 5 + 4].statistic = 60.0;                   // station 1, the code bias
    // An unwired carry at every station (a column the polish can compare across stations),
    // raised at station 3.  A column held by one station alone is its own column effect and
    // never stands out: the observation channel's T4 has the same property.
    for (int s = 0; s < 5; ++s)
        rows.push_back(priorRow(s, SK::Unwired, "", s == 3 ? 60.0 : 1.0, CK::BlockZwdCarry));
    const RaimPriorSelection sel = raimPriorChannelSelect(rows, 1.0, 6.0, {}, {}, true);
    ASSERT_EQ(sel.arcs.size(), 1u);
    EXPECT_EQ(sel.arcs[0], std::make_pair(4, std::string("G03")));
    ASSERT_EQ(sel.arc_cells.size(), 1u);
    EXPECT_GT(sel.flagStatistic(sel.arc_cells[0]), 6.0);
    ASSERT_EQ(sel.rxcb.size(), 1u);
    EXPECT_EQ(rows[sel.cell_row[sel.rxcb[0]]].station, 1);
    EXPECT_EQ(rows[sel.cell_row[sel.rxcb[0]]].subject.cb_key, "GPS_ISC_C1C");
    ASSERT_EQ(sel.unwired.size(), 1u);
    EXPECT_EQ(rows[sel.cell_row[sel.unwired[0]]].station, 3);
    EXPECT_TRUE(sel.arcs_both.empty());
    EXPECT_TRUE(sel.held_cells.empty());
    EXPECT_TRUE(sel.acted(4, "G03"));
    EXPECT_TRUE(sel.acted(1, "rxcb:GPS_ISC_C1C"));
    EXPECT_FALSE(sel.acted(3, "block_zwd_carry")) << "an unwired flag takes no response";
    EXPECT_FALSE(sel.acted(0, "G01"));

    RaimPriorChannelTally t;
    raimPriorChannelNoteSelection(sel, true, t);
    EXPECT_EQ(t.passes, 1);
    EXPECT_EQ(t.scale_n, 1);
    EXPECT_EQ(t.cells, static_cast<long long>(sel.cells.size()));
    EXPECT_EQ(t.flags, 3);
    EXPECT_EQ(t.unwired, 1);
    EXPECT_EQ(t.arcs, 0) << "the arcs refloated are the caller's count";
}

TEST(RaimPriorChannel, TheObservationChannelsFlagsAreNotTakenTwice) {
    std::vector<RaimPriorRow> rows = quietTable();
    rows[4 * 5 + 2].statistic = 60.0;   // station 4, G03
    rows[0 * 5 + 1].statistic = 60.0;   // station 0, G02
    // G02 caught wholesale by the observation channel; station 4's G03 refloated by it.
    const RaimPriorSelection sel = raimPriorChannelSelect(
        rows, 1.0, 6.0, {"G02"}, {{4, "G03"}}, true);
    EXPECT_TRUE(sel.arcs.empty());
    ASSERT_EQ(sel.arcs_both.size(), 1u);
    EXPECT_EQ(std::get<1>(sel.cells[sel.arcs_both[0]]), "G03");
    EXPECT_EQ(sel.isolated.refloat.size(), 1u) << "a wholesale satellite is not a flag";
    EXPECT_TRUE(sel.acted(4, "G03")) << "refloated once, by the observation channel";
}

TEST(RaimPriorChannel, AClosedMaturityGateHoldsEveryFlag) {
    std::vector<RaimPriorRow> rows = quietTable();
    rows[4 * 5 + 2].statistic = 60.0;
    rows[1 * 5 + 4].statistic = 60.0;
    const RaimPriorSelection sel = raimPriorChannelSelect(rows, 1.0, 6.0, {}, {}, /*mature=*/false);
    EXPECT_TRUE(sel.arcs.empty());
    EXPECT_TRUE(sel.rxcb.empty());
    EXPECT_EQ(sel.held_cells.size(), 2u);
    EXPECT_TRUE(sel.acted_cells.empty());
    RaimPriorChannelTally t;
    raimPriorChannelNoteSelection(sel, true, t);
    EXPECT_EQ(t.held, 2);
    EXPECT_EQ(t.flags, 0);
}

TEST(RaimPriorChannel, TheScaleIsTheChannelsOwnAndFloored) {
    std::vector<RaimPriorRow> rows;
    for (int s = 0; s < 4; ++s) rows.push_back(priorRow(s, SK::LineOfSight, "G01", 3.0));
    EXPECT_DOUBLE_EQ(raimPriorChannelSelect(rows, 1.0, 6.0, {}, {}, true).scale, 1.4826 * 3.0);
    for (RaimPriorRow& r : rows) r.statistic = 0.1;
    EXPECT_DOUBLE_EQ(raimPriorChannelSelect(rows, 1.0, 6.0, {}, {}, true).scale, 1.0);
    const RaimPriorSelection empty = raimPriorChannelSelect({}, 1.0, 6.0, {}, {}, true);
    EXPECT_TRUE(empty.cells.empty());
    RaimPriorChannelTally t;
    raimPriorChannelNoteSelection(empty, false, t);
    EXPECT_EQ(t.passes, 1);
    EXPECT_EQ(t.scale_n, 0) << "a pass with no row does not enter the scale's mean";
}

TEST(RaimPriorChannel, TheEventDetailNamesTheRow) {
    RaimPriorRow r = priorRow(0, SK::RxCodeBias, "GPS_ISC_C1C", 12.5, CK::BlockRxCbCarry);
    r.own_col = 7;
    r.own_coef = 0.8;
    r.max_other_coef = 1.2;
    r.max_other_col = 3;
    EXPECT_EQ(raimPriorEventDetail(r, 1.25),
              "sigma0_p=1.25;row_kind=block_rx_cb_carry;own_col=7;row_stat=12.500;"
              "own_coef=0.8000;max_other_coef=1.2000;max_other_col=3;subject=rxcb:GPS_ISC_C1C");
}

TEST(RaimPriorChannel, TheRunEndSaysEachOutcomeInWords) {
    RaimPriorChannelTally t;
    auto lines = raimPriorChannelReport(t, 6.0);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_TRUE(lines[0].warn);
    EXPECT_NE(lines[0].text.find("NEVER RAN"), std::string::npos);

    t.passes = 10;   // ran, with no carried-prior row
    lines = raimPriorChannelReport(t, 6.0);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_TRUE(lines[1].warn);
    EXPECT_NE(lines[1].text.find("INERT"), std::string::npos);

    t.station_walks = 30;   // rows tested, nothing flagged
    t.rows[static_cast<std::size_t>(CK::BlockAmbiguityCarry)] = 300;
    t.rows[static_cast<std::size_t>(CK::BlockRxCbCarry)] = 30;
    t.rows_other_dominant = 33;
    t.cells = 200;
    t.scale_n = 10;
    t.scale_sum = 10.0;
    t.scale_max = 1.0;
    lines = raimPriorChannelReport(t, 6.0);
    std::string all;
    bool any_warn = false;
    for (const auto& l : lines) { all += l.text + "\n"; any_warn = any_warn || l.warn; }
    EXPECT_NE(all.find("block_ambiguity_carry 300, block_rx_cb_carry 30"), std::string::npos) << all;
    EXPECT_NE(all.find("NO FLAG"), std::string::npos);
    EXPECT_NE(all.find("33 of 330 tested row(s) (10.0 %)"), std::string::npos) << all;
    EXPECT_FALSE(any_warn);

    t.flags = 4;
    t.arcs = 2;
    t.arcs_ambiguities = 3;
    t.rxcb_resets = 1;
    t.unwired = 1;
    lines = raimPriorChannelReport(t, 6.0);
    all.clear();
    any_warn = false;
    for (const auto& l : lines) { all += l.text + "\n"; any_warn = any_warn || l.warn; }
    EXPECT_EQ(all.find("NO FLAG"), std::string::npos);
    EXPECT_NE(all.find("2 line-of-sight refloat(s) reaching 3 carried ambiguity(ies)"),
              std::string::npos) << all;
    EXPECT_NE(all.find("NOTHING DONE"), std::string::npos);
    EXPECT_TRUE(any_warn) << "an unwired flag is warned";
}

TEST(RaimPriorChannel, KeyDefaultsOff) {
    LogManager log;
    const fs::path p = writeTempConfig("default", networkConfig("    \"raim\": true,\n"
                                                                "    \"raim_attribution\": true,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.networkRaim());
    EXPECT_FALSE(cfg.networkRaimPriorChannel());
    std::error_code ec; fs::remove(p, ec);
}

TEST(RaimPriorChannel, KeyLoadsWithRaimAndAttribution) {
    LogManager log;
    const fs::path p = writeTempConfig("armed",
        networkConfig("    \"raim\": true,\n"
                      "    \"phase_leveling\": true,\n"
                      "    \"raim_attribution\": true,\n"
                      "    \"raim_prior_channel\": true,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.networkRaimPriorChannel());
    EXPECT_FALSE(cfg.networkRaimPoolDataRowsOnly())
        << "the config key of card RPA is not changed; the run's knob resolves both";
    std::error_code ec; fs::remove(p, ec);
}

TEST(RaimPriorChannel, KeyDisabledWithoutRaimOrAttribution) {
    LogManager log;
    {
        const fs::path p = writeTempConfig("noraim",
            networkConfig("    \"raim_prior_channel\": true,\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.networkRaim());
        EXPECT_FALSE(cfg.networkRaimPriorChannel()) << "no post-fit pass to test in";
        std::error_code ec; fs::remove(p, ec);
    }
    {
        const fs::path p = writeTempConfig("noattrib",
            networkConfig("    \"raim\": true,\n"
                          "    \"phase_leveling\": true,\n"
                          "    \"raim_prior_channel\": true,\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.networkRaim());
        EXPECT_FALSE(cfg.networkRaimAttribution());
        EXPECT_FALSE(cfg.networkRaimPriorChannel()) << "no attribution refloat to answer through";
        std::error_code ec; fs::remove(p, ec);
    }
}
