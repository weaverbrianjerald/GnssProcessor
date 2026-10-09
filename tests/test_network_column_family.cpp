/**
 * @file test_network_column_family.cpp
 * @brief The one shared-column classifier, @c sharedColumnFamily
 *        (filter/NetworkRowClassify.hpp; card NOM step N0b).
 *
 * The family of a shared column is read from the estimator's column identity
 * (@c NetworkEstimator::sharedColumnIdentity), and a column with no satellite identity
 * is classified by exclusion: an SH-VTEC coefficient unless it is a clock clone (card
 * SCL) or a shared-history column (card SCL2 X2).  The run-time gates exercise only the
 * columns of the armed posture, which registers neither a clone nor a history column, so
 * the exclusion is tested here over an estimator that holds every kind of column: two
 * satellites' clocks, code biases and phase biases, SH coefficients of both shells, a
 * clock clone and two history columns.  @c buildSharedColumnMap (the map the row
 * classifier reads) and the frame diagnostic's mass partition both take the family from
 * this function.  The bias clones of card BCL (@c NetworkEstimator::registerClone) are
 * tested on their own: they carry a bias kind with the clone flag, and every reader that
 * used to parse the label prefix gave them no family.
 */

#include <gtest/gtest.h>

#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkRowClassify.hpp"

#include <Eigen/Dense>

#include <string>

using namespace GnssProcessor;

namespace {

/// The column indices of the test layout, registered in the driver's order (the
/// satellite columns and the SH coefficients first, then the clones, then the history).
struct Layout {
    Eigen::Index clk_g01, cb_g01, pb_g01, clk_e05, cb_e05, pb_e05;
    Eigen::Index sh_mean_l1, sh_c10_l1, sh_s11_l1, sh_mean_l2;
    Eigen::Index clone_g01, hist0, hist1;
};

Layout registerEveryKind(NetworkEstimator& est) {
    Layout L{};
    L.clk_g01    = est.registerClock("G01");
    L.cb_g01     = est.registerCodeBias("G01", "C1W");
    L.pb_g01     = est.registerPhaseBias("G01", "L1C");
    L.clk_e05    = est.registerClock("E05");
    L.cb_e05     = est.registerCodeBias("E05", "C5Q");
    L.pb_e05     = est.registerPhaseBias("E05", "L5Q");
    L.sh_mean_l1 = est.registerVtecCoeff(0, 0, 'C', 0);
    L.sh_c10_l1  = est.registerVtecCoeff(1, 0, 'C', 0);
    L.sh_s11_l1  = est.registerVtecCoeff(1, 1, 'S', 0);
    L.sh_mean_l2 = est.registerVtecCoeff(0, 0, 'C', 1);
    L.clone_g01  = est.registerClockClone("G01");
    L.hist0      = est.registerHistoryColumn(0);
    L.hist1      = est.registerHistoryColumn(1);
    return L;
}

}  // namespace

// Every kind of column gets its family, and a satellite column also gives its satellite.
TEST(NetworkColumnFamilyTest, EveryKindOfColumnGetsItsFamily) {
    NetworkEstimator est;
    const Layout L = registerEveryKind(est);
    ASSERT_EQ(est.numShared(), 13);

    struct Want { Eigen::Index col; NetColFam fam; const char* sat; };
    const Want want[] = {
        {L.clk_g01, NetColFam::Clk, "G01"}, {L.cb_g01, NetColFam::Cb, "G01"},
        {L.pb_g01, NetColFam::Pb, "G01"},   {L.clk_e05, NetColFam::Clk, "E05"},
        {L.cb_e05, NetColFam::Cb, "E05"},   {L.pb_e05, NetColFam::Pb, "E05"},
        {L.sh_mean_l1, NetColFam::Sh, ""},  {L.sh_c10_l1, NetColFam::Sh, ""},
        {L.sh_s11_l1, NetColFam::Sh, ""},   {L.sh_mean_l2, NetColFam::Sh, ""},
        {L.clone_g01, NetColFam::Other, ""}, {L.hist0, NetColFam::Other, ""},
        {L.hist1, NetColFam::Other, ""},
    };
    for (const Want& w : want) {
        std::string sat = "unset";
        EXPECT_EQ(sharedColumnFamily(est, w.col, &sat), w.fam) << "column " << w.col;
        // The satellite is written for a satellite column only; the others leave it as
        // the caller had it.
        EXPECT_EQ(sat, std::string(*w.sat ? w.sat : "unset")) << "column " << w.col;
        EXPECT_EQ(sharedColumnFamily(est, w.col), w.fam) << "column " << w.col << " (no out)";
    }
}

// A column outside the layout has no family, on either side, and its satellite is not
// written.
TEST(NetworkColumnFamilyTest, ColumnOutsideTheLayoutIsOther) {
    NetworkEstimator est;
    registerEveryKind(est);
    for (const Eigen::Index col : {Eigen::Index{-1}, est.numShared(), est.numShared() + 5}) {
        std::string sat = "unset";
        EXPECT_EQ(sharedColumnFamily(est, col, &sat), NetColFam::Other) << col;
        EXPECT_EQ(sat, "unset") << col;
    }
    NetworkEstimator empty;
    EXPECT_EQ(sharedColumnFamily(empty, 0), NetColFam::Other);
}

// The clone and the history columns are told from an SH coefficient by the estimator's
// flags alone: on a layout without them, the same exclusion files every coefficient as SH.
TEST(NetworkColumnFamilyTest, ShIsTheExclusionOfCloneAndHistory) {
    NetworkEstimator est;
    const Eigen::Index clk = est.registerClock("G02");
    const Eigen::Index sh  = est.registerVtecCoeff(2, 1, 'C', 0);
    const Eigen::Index k   = est.registerClockClone("G02");
    const Eigen::Index h   = est.registerHistoryColumn(0);
    EXPECT_FALSE(est.isClockClone(sh));
    EXPECT_FALSE(est.isHistoryColumn(sh));
    EXPECT_TRUE(est.isClockClone(k));
    EXPECT_TRUE(est.isHistoryColumn(h));
    EXPECT_EQ(sharedColumnFamily(est, clk), NetColFam::Clk);
    EXPECT_EQ(sharedColumnFamily(est, sh), NetColFam::Sh);
    EXPECT_EQ(sharedColumnFamily(est, k), NetColFam::Other);
    EXPECT_EQ(sharedColumnFamily(est, h), NetColFam::Other);
}

// Card BCL's phase- and code-bias clones are Other, as card SCL's clock clone is, and
// their satellite is not written: the answer the "PB:" / "CB:" label-prefix parses gave
// (a "PBC:" or "CBC:" label matches neither), which the migrated readers must keep.  The
// clone keys of every kind resolve through one key function.
TEST(NetworkColumnFamilyTest, BiasClonesAreNotBiasesOfTheirFamily) {
    NetworkEstimator est;
    const Eigen::Index clk = est.registerClock("G01");
    const Eigen::Index cb  = est.registerCodeBias("G01", "C1W");
    const Eigen::Index pb  = est.registerPhaseBias("G01", "L1C");
    const Eigen::Index kc  = est.registerClone(SharedParamKind::Clock, "G01", "");
    const Eigen::Index kcb = est.registerClone(SharedParamKind::CodeBias, "G01", "C1W");
    const Eigen::Index kpb = est.registerClone(SharedParamKind::PhaseBias, "G01", "L1C");
    ASSERT_EQ(est.numShared(), 6);
    EXPECT_EQ(est.numClockClones(), 3);

    // The keys: one per (kind, sat, signal), disjoint from the parameters' own keys.
    EXPECT_EQ(est.clockCloneColumn("G01"), kc);
    EXPECT_EQ(est.registerClockClone("G01"), kc);   // idempotent through either entry
    EXPECT_EQ(est.cloneColumn(SharedParamKind::Clock, "G01", ""), kc);
    EXPECT_EQ(est.cloneColumn(SharedParamKind::CodeBias, "G01", "C1W"), kcb);
    EXPECT_EQ(est.cloneColumn(SharedParamKind::PhaseBias, "G01", "L1C"), kpb);
    EXPECT_EQ(est.codeBiasColumn("G01", "C1W"), cb);
    EXPECT_EQ(est.phaseBiasColumn("G01", "L1C"), pb);

    struct Want { Eigen::Index col; NetColFam fam; const char* label; };
    const Want want[] = {
        {clk, NetColFam::Clk, "CLK:G01"},       {cb, NetColFam::Cb, "CB:G01:C1W"},
        {pb, NetColFam::Pb, "PB:G01:L1C"},      {kc, NetColFam::Other, "CLKC:G01"},
        {kcb, NetColFam::Other, "CBC:G01:C1W"}, {kpb, NetColFam::Other, "PBC:G01:L1C"},
    };
    for (const Want& w : want) {
        const std::string label = est.sharedColumnLabel(w.col);
        EXPECT_EQ(label, w.label) << w.col;
        // The family the label-prefix parse gave, before the readers took the identity.
        NetColFam by_label = NetColFam::Other;
        if      (label.rfind("SH", 0) == 0)   by_label = NetColFam::Sh;
        else if (label.rfind("PB:", 0) == 0)  by_label = NetColFam::Pb;
        else if (label.rfind("CB:", 0) == 0)  by_label = NetColFam::Cb;
        else if (label.rfind("CLK:", 0) == 0) by_label = NetColFam::Clk;
        std::string sat = "unset";
        EXPECT_EQ(sharedColumnFamily(est, w.col, &sat), w.fam) << label;
        EXPECT_EQ(sharedColumnFamily(est, w.col), by_label) << label;
        EXPECT_EQ(sat, w.fam == NetColFam::Other ? "unset" : "G01") << label;
        EXPECT_EQ(est.isClockClone(w.col), w.fam == NetColFam::Other) << label;
        SharedParamKind kind{};
        std::string s, sig;
        EXPECT_EQ(est.sharedColumnIdentity(w.col, kind, s, sig), w.fam != NetColFam::Other)
            << label;
    }
    const SharedColumnMap cm = buildSharedColumnMap(est, est.numShared());
    EXPECT_EQ(cm.fam[static_cast<std::size_t>(kpb)], NetColFam::Other);
    EXPECT_EQ(cm.fam[static_cast<std::size_t>(kcb)], NetColFam::Other);
    EXPECT_TRUE(cm.col_sat[static_cast<std::size_t>(kpb)].empty());
    EXPECT_TRUE(cm.col_sat[static_cast<std::size_t>(kcb)].empty());
}

// The map the row classifier reads takes every family and satellite from the function,
// and finds the two SH mean columns.
TEST(NetworkColumnFamilyTest, SharedColumnMapReadsTheClassifier) {
    NetworkEstimator est;
    const Layout L = registerEveryKind(est);
    const SharedColumnMap cm = buildSharedColumnMap(est, est.numShared());
    ASSERT_EQ(cm.n, est.numShared());
    ASSERT_EQ(cm.fam.size(), static_cast<std::size_t>(cm.n));
    for (Eigen::Index c = 0; c < cm.n; ++c) {
        std::string sat;
        EXPECT_EQ(cm.fam[static_cast<std::size_t>(c)], sharedColumnFamily(est, c, &sat)) << c;
        EXPECT_EQ(cm.col_sat[static_cast<std::size_t>(c)], sat) << c;
    }
    EXPECT_EQ(cm.c00_l1, L.sh_mean_l1);
    EXPECT_EQ(cm.c00_l2, L.sh_mean_l2);
    EXPECT_EQ(cm.label[static_cast<std::size_t>(L.clone_g01)], "CLKC:G01");
    EXPECT_EQ(cm.label[static_cast<std::size_t>(L.hist1)], "HIST:1");

    // Without SH columns, neither mean column is looked up.
    NetworkEstimator no_sh;
    no_sh.registerClock("G01");
    no_sh.registerClockClone("G01");
    const SharedColumnMap cm2 = buildSharedColumnMap(no_sh, no_sh.numShared());
    EXPECT_EQ(cm2.c00_l1, -1);
    EXPECT_EQ(cm2.c00_l2, -1);
    EXPECT_EQ(cm2.fam[1], NetColFam::Other);
}
