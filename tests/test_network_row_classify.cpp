/**
 * @file test_network_row_classify.cpp
 * @brief Card NOM step N2b: every reader of "what is this row" reads the row's
 *        contribution tag (filter/NetworkRowClassify.hpp).
 *
 * The contracts:
 *  - one map from contribution kind to row class covers every kind: the observation
 *    kinds are code or phase by their declared type, the SH tie is an SH row, the
 *    coupling-region kinds are coupling rows, the station constraint and phase-colour
 *    prior rows are pins by their weight, and every kind no station row carries (a
 *    diagonal prior, a shared kind) and @c Unset is unclassified;
 *  - the classifier reads the tag and not the columns: the two cases card NOM section 16
 *    (suspect 3) found misfiled by the column inference are filed by their type (a phase
 *    row with no phase-bias column before the carry is warm, a code row whose only shared
 *    column is the clock), and a phase-colour prior row after the coupling region is a
 *    local prior row, not a coupling row;
 *  - an observation row's code comes from its tag;
 *  - the RAIM pools' declared admission rule admits exactly the observation phase rows,
 *    the coupling and block carried-prior rows and the SD-EWL constraint rows, and the
 *    per-kind tally counts visits, rows and refusals.
 */
#include <gtest/gtest.h>

#include "common/GnssIdentity.hpp"
#include "filter/NetworkContributions.hpp"
#include "filter/NetworkRaim.hpp"          // RaimPoolAdmissionTally
#include "filter/NetworkRowClassify.hpp"
#include "filter/NetworkRunTypes.hpp"   // StationAssembly

#include <Eigen/Dense>

#include <set>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

using CK = ContributionKind;

/// Every family, in enum order.
std::vector<CK> allKinds() {
    std::vector<CK> out;
    for (std::size_t k = 0; k < kContributionKinds; ++k) out.push_back(static_cast<CK>(k));
    return out;
}

}  // namespace

TEST(NetworkRowClassifyTest, OneMapCoversEveryKind) {
    const std::set<CK> code{CK::DataCode, CK::DataCodeCompanion};
    const std::set<CK> phase{CK::DataPhase, CK::DataPhaseSecond};
    const std::set<CK> pins{CK::AmbiguityPin, CK::Eq34RpbPin, CK::Eq34RpbRankFloor,
                            CK::Eq34RpbTimeLink, CK::GloIfbAnchor, CK::GloIonoAnchor,
                            CK::SimZeroIono,
                            CK::GimIono, CK::SdEwl, CK::PhaseColourBirth,
                            CK::PhaseColourBackstop};
    for (const CK k : allKinds()) {
        const ContributionSpec& s = contributionSpec(k);
        const NetRowClass light = rowClassOfKind(k, 1.0);
        const NetRowClass heavy = rowClassOfKind(k, 1e10);
        if (code.count(k)) {
            EXPECT_EQ(light, NetRowClass::ObsCode) << s.name;
        } else if (phase.count(k)) {
            EXPECT_EQ(light, NetRowClass::ObsPhase) << s.name;
        } else if (k == CK::ShIonoTie) {
            EXPECT_EQ(light, NetRowClass::IonoShRow);
        } else if (contributionInCouplingRegion(k)) {
            EXPECT_EQ(light, NetRowClass::CplRow) << s.name;
        } else if (pins.count(k)) {
            EXPECT_EQ(light, NetRowClass::LocalPinRow) << s.name;
            EXPECT_EQ(heavy, NetRowClass::PivotPin) << s.name;
        } else {
            // No station row carries it: unclassified, whatever the weight.
            EXPECT_NE(s.channel, ContributionChannel::StationRows) << s.name;
            EXPECT_EQ(light, NetRowClass::ObsOther) << s.name;
            EXPECT_EQ(heavy, NetRowClass::ObsOther) << s.name;
        }
        // Only the observation kinds are data rows of a type; the table's observation
        // flag and the class agree.
        EXPECT_EQ(s.is_observation, light == NetRowClass::ObsCode || light == NetRowClass::ObsPhase)
            << s.name;
        // The weight decides nothing but pin against prior.
        if (!pins.count(k)) EXPECT_EQ(light, heavy) << s.name;
    }
    EXPECT_EQ(rowClassOfKind(CK::Unset, 1.0), NetRowClass::ObsOther);
    // The pin boundary is the documented weight.
    EXPECT_EQ(rowClassOfKind(CK::AmbiguityPin, kNetPivotPinWeight), NetRowClass::PivotPin);
    EXPECT_EQ(rowClassOfKind(CK::AmbiguityPin, 0.5 * kNetPivotPinWeight),
              NetRowClass::LocalPinRow);
}

TEST(NetworkRowClassifyTest, ClassifierReadsTheTagNotTheColumns) {
    // Six rows over two shared columns (0 a satellite clock, 1 a phase bias) and two local
    // columns (0 the receiver clock, 1 an ambiguity).  Rows 0 to 2 are data rows, row 3 a
    // coupling row, row 4 a phase-colour birth row appended after the coupling region,
    // row 5 has no tag.
    StationAssembly R;
    R.ok = true;
    R.A_s = Eigen::MatrixXd::Zero(6, 2);
    R.A_r = Eigen::MatrixXd::Zero(6, 2);
    R.w = Eigen::VectorXd::Ones(6);
    R.y = Eigen::VectorXd::Zero(6);
    // Row 0: a phase row of a (satellite, signal) tracked at this station alone: no
    // phase-bias column, and before the carry is warm no pl_amb entry (R.pl_amb empty).
    // Its only shared column is the clock; it carries +1 on its ambiguity.
    R.A_s(0, 0) = -1.0; R.A_r(0, 0) = 1.0; R.A_r(0, 1) = 1.0;
    // Row 1: a code row of a signal with no code-bias column: the clock alone.
    R.A_s(1, 0) = -1.0; R.A_r(1, 0) = 1.0;
    // Row 2: an ordinary phase row with its phase bias.
    R.A_s(2, 0) = -1.0; R.A_s(2, 1) = 1.0; R.A_r(2, 0) = 1.0; R.A_r(2, 1) = 1.0;
    // Row 3: a coupling row (+1 on the ambiguity, a shared term).
    R.A_s(3, 1) = 0.3; R.A_r(3, 1) = 1.0;
    // Row 4: a phase-colour birth row (a local prior row, after the coupling region).
    R.A_r(4, 0) = 1.0;
    R.n_data_rows = 3;
    R.cpl_row_start = 3;
    R.cpl_row_local_cols = {1};
    R.row_tags = {makeRowTag(CK::DataPhase, "G25", "L1W"), makeRowTag(CK::DataCode, "G25", "C1W"),
                  makeRowTag(CK::DataPhase, "G07", "L1C"),
                  makeRowTag(CK::CouplingScalar, "G07", "L1C"),
                  makeRowTag(CK::PhaseColourBirth, "G09")};   // row 5: no tag

    EXPECT_EQ(classifyRow(R, 0, nullptr), NetRowClass::ObsPhase)
        << "a phase row without a phase-bias column is phase (was obs_other / code)";
    EXPECT_EQ(classifyRow(R, 1, nullptr), NetRowClass::ObsCode)
        << "a code row whose only shared column is the clock is code (was obs_other)";
    EXPECT_EQ(classifyRow(R, 2, nullptr), NetRowClass::ObsPhase);
    EXPECT_EQ(classifyRow(R, 3, nullptr), NetRowClass::CplRow);
    EXPECT_EQ(classifyRow(R, 4, nullptr), NetRowClass::LocalPinRow)
        << "a prior row past the coupling region is a prior row, not a coupling row";
    EXPECT_EQ(classifyRow(R, 5, nullptr), NetRowClass::ObsOther) << "no tag: unclassified";
    EXPECT_EQ(rowTagAt(R, 5).kind, CK::Unset);
    EXPECT_EQ(rowTagAt(R, -1).kind, CK::Unset);

    // The satellite is the tag's, written only into an empty out-parameter.
    std::string sat;
    classifyRow(R, 1, &sat);
    EXPECT_EQ(sat, "G25");
    std::string seeded = "G01";
    classifyRow(R, 2, &seeded);
    EXPECT_EQ(seeded, "G01");
    std::string none;
    classifyRow(R, 5, &none);
    EXPECT_TRUE(none.empty());

    // The contributions table's unclassified count sees exactly the untagged row.
    EXPECT_EQ(checkStationTags(R).unclassified, 1);
}

TEST(NetworkRowClassifyTest, ObservationCodeComesFromTheTag) {
    EXPECT_EQ(rowObservationCode(makeRowTag(CK::DataCode, "G05", "C1W")), "C1W");
    EXPECT_EQ(rowObservationCode(makeRowTag(CK::DataCodeCompanion, "G05", "C1C")), "C1C");
    EXPECT_EQ(rowObservationCode(makeRowTag(CK::DataPhase, "G05", "L2W")), "L2W");
    EXPECT_EQ(rowObservationCode(makeRowTag(CK::DataPhaseSecond, "E11", "L7Q")), "L7Q");
    EXPECT_EQ(rowObservationCode(makeRowTag(CK::ShIonoTie, "G05")), "");
    EXPECT_EQ(rowObservationCode(makeRowTag(CK::AmbiguityPin, "G05", "L1C")), "")
        << "a pin is not an observation";
    EXPECT_EQ(rowObservationCode(makeRowTag(CK::DataCode, "G05")), "") << "no signal named";
}

TEST(NetworkRowClassifyTest, RaimPoolAdmitsThePhaseRowsAndTheRowsOnAmbiguitiesOnly) {
    const std::set<CK> admitted{CK::DataPhase,          CK::DataPhaseSecond,
                                CK::CouplingScalar,
                                CK::BlockAmbiguityCarry, CK::BlockZwdCarry,
                                CK::BlockIonoCarry,     CK::BlockRxClockCarry,
                                CK::BlockRxCbCarry,     CK::BlockPhaseColourCarry,
                                CK::SdEwl};
    for (const CK k : allKinds())
        EXPECT_EQ(raimPoolAdmitsKind(k), admitted.count(k) > 0) << contributionKindName(k);
    EXPECT_FALSE(raimPoolAdmitsKind(CK::Unset));

    RaimPoolAdmissionTally t;
    t.walks = 2;
    t.note(CK::DataPhase, true, true);
    t.note(CK::BlockRxCbCarry, true, true);
    t.note(CK::BlockRxCbCarry, true, false);   // the same row on a second column
    t.note(CK::AmbiguityPin, false, false);
    t.note(CK::Unset, false, false);
    EXPECT_EQ(t.visits[static_cast<std::size_t>(CK::BlockRxCbCarry)], 2);
    EXPECT_EQ(t.rows[static_cast<std::size_t>(CK::BlockRxCbCarry)], 1);
    EXPECT_EQ(t.refused[static_cast<std::size_t>(CK::AmbiguityPin)], 1);
    EXPECT_EQ(t.refused[kContributionKinds], 1) << "Unset counts in the last slot";
    const std::string text = t.text();
    EXPECT_NE(text.find("walks 2"), std::string::npos) << text;
    EXPECT_NE(text.find("data_phase visits 1 rows 1"), std::string::npos) << text;
    EXPECT_NE(text.find("block_rx_cb_carry visits 2 rows 1"), std::string::npos) << text;
    EXPECT_NE(text.find("refused by kind: ambiguity_pin visits 1, unset visits 1"),
              std::string::npos) << text;
    EXPECT_NE(RaimPoolAdmissionTally{}.text().find("admitted: none; refused by kind: none"),
              std::string::npos);
}
