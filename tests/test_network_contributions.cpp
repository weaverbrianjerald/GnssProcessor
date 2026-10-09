/**
 * @file test_network_contributions.cpp
 * @brief Card NOM step N2a: the contribution declarations, the row tags and the
 *        "contributions as armed" table of filter/NetworkContributions.hpp.
 *
 * Six contracts: every kind has a declaration with a non-empty name, weight and builder,
 * and the table is in enum order with unique names; the data-row tagger follows the split's
 * weight passes (latched code, companion code through an inter-signal state, latched phase,
 * second phase) and keeps a row without identities aligned; the receiver-key tags read the
 * key's signal; the tag-alignment helper of the within-epoch re-whiten keeps the head rows
 * and the kept coupling rows, and the integrity count sees a missing tag; the estimator's
 * fold log records the kind a site named (per row for the code-bias datum stack), a fold
 * with none, and a replay's re-folds apart; and the table prints every kind (a zero
 * included, with the GIM note) and the integrity lines.
 */
#include <gtest/gtest.h>

#include "common/GnssIdentity.hpp"
#include "filter/NetworkContributions.hpp"
#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkParamRegistry.hpp"
#include "filter/NetworkRunTypes.hpp"   // StationAssembly, CplRowRaw
#include "filter/StateKeySchema.hpp"

#include <Eigen/Dense>

#include <set>
#include <string>
#include <vector>

using namespace GnssProcessor;
namespace K = GnssProcessor::StateKeys;

namespace {

using CK = ContributionKind;

bool anyLineHas(const std::vector<std::string>& lines, const std::string& needle) {
    for (const std::string& l : lines)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}

const std::string* lineStarting(const std::vector<std::string>& lines, const std::string& head) {
    for (const std::string& l : lines)
        if (l.rfind(head, 0) == 0) return &l;
    return nullptr;
}

SignalId sigOf(System sys, const char* code) { return *SignalId::fromObsCode(sys, code); }

}  // namespace

// ---------------------------------------------------------------------------
// The declarations
// ---------------------------------------------------------------------------

TEST(NetworkContributionsTest, EveryKindHasADeclarationInEnumOrder) {
    const auto& specs = contributionSpecs();
    ASSERT_EQ(specs.size(), kContributionKinds);
    std::set<std::string> names;
    for (std::size_t k = 0; k < kContributionKinds; ++k) {
        const ContributionSpec& s = specs[k];
        EXPECT_EQ(static_cast<std::size_t>(s.kind), k) << "table out of enum order at " << k;
        ASSERT_NE(s.name, nullptr);
        EXPECT_FALSE(std::string(s.name).empty()) << k;
        EXPECT_TRUE(names.insert(s.name).second) << "duplicate name " << s.name;
        ASSERT_NE(s.weight, nullptr);
        EXPECT_FALSE(std::string(s.weight).empty()) << s.name;
        ASSERT_NE(s.builder, nullptr);
        EXPECT_FALSE(std::string(s.builder).empty()) << s.name;
        EXPECT_NE(std::string(contributionChannelName(s.channel)), "?") << s.name;
        EXPECT_NE(std::string(contributionStageName(s.stage)), "?") << s.name;
        EXPECT_NE(std::string(contributionSubjectName(s.subject)), "?") << s.name;
        EXPECT_EQ(&contributionSpec(s.kind), &s);
        EXPECT_STREQ(contributionKindName(s.kind), s.name);
    }
    // The last value closes the enum: the count matches it.
    EXPECT_EQ(static_cast<std::size_t>(CK::DdPriorBlock) + 1, kContributionKinds);
}

TEST(NetworkContributionsTest, OnlyTheDataRowsAreObservations) {
    std::set<CK> obs;
    for (const ContributionSpec& s : contributionSpecs())
        if (s.is_observation) obs.insert(s.kind);
    EXPECT_EQ(obs, (std::set<CK>{CK::DataCode, CK::DataCodeCompanion, CK::DataPhase,
                                 CK::DataPhaseSecond}));
}

TEST(NetworkContributionsTest, ChannelsStagesAndParametersAreDeclared) {
    EXPECT_EQ(contributionSpec(CK::DataPhase).channel, ContributionChannel::StationRows);
    EXPECT_EQ(contributionSpec(CK::PriorRxcbDatumPrePd).channel,
              ContributionChannel::StationPriorPrePd);
    EXPECT_EQ(contributionSpec(CK::PriorRxcbDatum).channel, ContributionChannel::StationPriorPostPd);
    EXPECT_EQ(contributionSpec(CK::CbSumToZero).channel, ContributionChannel::SharedRows);
    EXPECT_EQ(contributionSpec(CK::BorderCarryDense).channel, ContributionChannel::SharedDensePrior);
    EXPECT_EQ(contributionSpec(CK::ClockPivotPin).channel, ContributionChannel::Pins);
    EXPECT_EQ(contributionSpec(CK::EnsemblePhaseGauge).channel, ContributionChannel::Gauges);
    EXPECT_EQ(contributionSpec(CK::DdPriorBlock).channel, ContributionChannel::DdBlocks);
    EXPECT_EQ(contributionSpec(CK::PbHeal).stage, ContributionStage::AtGate);
    EXPECT_EQ(contributionSpec(CK::BorderCarryDiagonal).stage, ContributionStage::AfterGate);
    EXPECT_EQ(contributionSpec(CK::Q3Refold).stage, ContributionStage::AfterSolve);
    EXPECT_EQ(contributionSpec(CK::DdPriorBlock).stage, ContributionStage::InSolve);
    // A code row touches the satellite clock and code bias, not the phase bias.
    const ContributionSpec& code = contributionSpec(CK::DataCode);
    EXPECT_TRUE(contributionTouches(code, NetworkParamKind::SatClock));
    EXPECT_TRUE(contributionTouches(code, NetworkParamKind::SatCodeBias));
    EXPECT_FALSE(contributionTouches(code, NetworkParamKind::SatPhaseBias));
    EXPECT_TRUE(contributionTouches(contributionSpec(CK::DataPhase), NetworkParamKind::Ambiguity));
    EXPECT_EQ(paramKindSetText(contributionSpec(CK::AmbiguityPin).params), "ambiguity");
    EXPECT_EQ(paramKindSetText(0), "");
}

// ---------------------------------------------------------------------------
// Row tags
// ---------------------------------------------------------------------------

TEST(NetworkContributionsTest, DataRowTagsFollowTheSplitWeightPasses) {
    const std::vector<std::string> sats = {"G05", "G05", "G07", "G07", "E11"};
    const std::vector<std::string> sigs = {"C1W", "C1C", "L1W", "L1C", "C1C"};
    const std::set<std::string> isc = {"C1C"};   // C1C routes through GPS_ISC_C1C
    const std::set<std::string> spc = {K::makeSatPhaseBiasKey("G07", "L1C")};
    std::vector<RowTag> tags;
    tags.push_back(RowTag{CK::SdEwl, {}, {}});   // cleared by the tagger
    // One row more than the identity vectors: tagged, with no identity.
    tagDataRows(tags, 6, sats, sigs, &isc, spc);
    ASSERT_EQ(tags.size(), 6u);
    EXPECT_EQ(tags[0].kind, CK::DataCode);
    EXPECT_EQ(tags[0].sat.str(), "G05");
    EXPECT_EQ(tags[0].signal, sigOf(System::GPS, "C1W"));
    EXPECT_EQ(tags[1].kind, CK::DataCodeCompanion);
    EXPECT_EQ(tags[2].kind, CK::DataPhase);
    EXPECT_EQ(tags[2].signal, sigOf(System::GPS, "L1W"));
    EXPECT_EQ(tags[3].kind, CK::DataPhaseSecond);
    EXPECT_EQ(tags[4].kind, CK::DataCodeCompanion);   // the test reads the signal, as the split does
    EXPECT_EQ(tags[4].sat.sys, System::Galileo);
    EXPECT_EQ(tags[5].kind, CK::DataCode);
    EXPECT_FALSE(tags[5].sat.valid());
    // The second code channel off: no companion row.
    tagDataRows(tags, 5, sats, sigs, nullptr, spc);
    EXPECT_EQ(tags[1].kind, CK::DataCode);
    EXPECT_EQ(tags[3].kind, CK::DataPhaseSecond);
}

TEST(NetworkContributionsTest, TagsReadTheReceiverKeys) {
    const RowTag rpb = rxPhaseBiasTag(CK::Eq34RpbPin, K::makeReceiverPhaseBiasKey("GPS", "L2W"));
    EXPECT_EQ(rpb.kind, CK::Eq34RpbPin);
    EXPECT_FALSE(rpb.sat.valid());
    EXPECT_EQ(rpb.signal, sigOf(System::GPS, "L2W"));
    const RowTag isc = rxCodeBiasTag(CK::PriorRxCodeBias, K::makeReceiverIscKey("GPS", "C1C"));
    EXPECT_EQ(isc.signal, sigOf(System::GPS, "C1C"));
    const RowTag dcb = rxCodeBiasTag(CK::PriorRxCodeBias, "GPS_DCB");
    EXPECT_EQ(dcb.signal, SignalId{});
    const RowTag bad = makeRowTag(CK::GimIono, "not-a-sat", "C1C");
    EXPECT_FALSE(bad.sat.valid());
    EXPECT_EQ(makeRowTag(CK::ShIonoTie, "G12").sat.str(), "G12");
}

TEST(NetworkContributionsTest, JointRewhitenKeepsTheHeadAndTheKeptCouplingRows) {
    // A synthetic block: three data rows, then three jointly whitened coupling rows on
    // local columns 5, 6, 7.  The refloat drops column 6.
    const std::vector<RowTag> tags = {
        RowTag{CK::DataCode, {}, {}}, RowTag{CK::DataPhase, {}, {}},
        RowTag{CK::AmbiguityPin, {}, {}},
        RowTag{CK::CouplingJoint, *SatId::parse("G01"), {}},
        RowTag{CK::CouplingJoint, *SatId::parse("G02"), {}},
        RowTag{CK::CouplingJoint, *SatId::parse("G03"), {}}};
    std::vector<CplRowRaw> raw(3);
    for (int k = 0; k < 3; ++k) {
        raw[static_cast<std::size_t>(k)].local_col = 5 + k;
        raw[static_cast<std::size_t>(k)].tag = tags[static_cast<std::size_t>(3 + k)];
    }
    const std::set<Eigen::Index> skip = {6};
    const std::vector<RowTag> out = rowTagsAfterJointRewhiten(tags, 3, raw, skip, 2);
    ASSERT_EQ(out.size(), 5u);
    EXPECT_EQ(out[2].kind, CK::AmbiguityPin);
    EXPECT_EQ(out[3].sat.str(), "G01");
    EXPECT_EQ(out[4].sat.str(), "G03");
    // A failed re-whiten appends no coupling row.
    EXPECT_EQ(rowTagsAfterJointRewhiten(tags, 3, raw, skip, 0).size(), 3u);
}

TEST(NetworkContributionsTest, UntaggedRowsSeesAGapInEveryChannel) {
    StationAssembly R;
    R.A_r = Eigen::MatrixXd::Zero(4, 2);
    R.row_tags.assign(4, RowTag{});
    R.carry_cols = {0, 1};
    R.carry_tags.assign(2, RowTag{CK::PriorZwd, {}, {}});
    EXPECT_EQ(untaggedRows(R), 0);
    R.row_tags.pop_back();
    EXPECT_EQ(untaggedRows(R), 1);
    R.prior_cols = {1};
    EXPECT_EQ(untaggedRows(R), 2);
}

// ---------------------------------------------------------------------------
// The estimator's fold log
// ---------------------------------------------------------------------------

TEST(NetworkContributionsTest, EstimatorRecordsTheKindEachFoldWasGiven) {
    NetworkEstimator est;
    const Eigen::Index c1 = est.registerClock("G01");
    const Eigen::Index c2 = est.registerClock("G02");
    est.beginEpoch();
    NetworkEstimator::SharedPrior p;
    p.kind = SharedParamKind::Clock; p.sat = "G01"; p.value = 0.1; p.info = 4.0;
    est.contributionKind(CK::BorderCarryDiagonal);
    est.addSharedPrior({p});
    est.addSharedPrior({p});   // no kind named: recorded without one
    // The code-bias datum stack: one kind per row.
    Eigen::MatrixXd H = Eigen::MatrixXd::Zero(3, est.numShared());
    H(0, c1) = 1.0; H(1, c2) = 1.0; H(2, c1) = 1.0; H(2, c2) = 1.0;
    const std::vector<CK> rk = {CK::CbIfPin, CK::CbIfPin, CK::CbSumToZero};
    est.contributionRowKinds(&rk);
    est.addSharedConstraintRows(H, Eigen::VectorXd::Zero(3), 1e3);
    // A replay's re-folds count apart.
    est.contributionReplay(true);
    est.contributionKind(CK::ShPrior);
    est.addSharedPrior({p});
    est.contributionReplay(false);

    const SharedFoldLog& log = est.sharedFoldLog();
    ASSERT_EQ(log.folds.size(), 4u);
    EXPECT_TRUE(log.folds[0].has_kind);
    EXPECT_EQ(log.folds[0].kind, CK::BorderCarryDiagonal);
    EXPECT_EQ(log.folds[0].dim, 1);
    EXPECT_FALSE(log.folds[1].has_kind);
    EXPECT_EQ(log.folds[2].kind, CK::CbIfPin);
    EXPECT_EQ(log.folds[2].dim, 2);
    EXPECT_EQ(log.folds[3].kind, CK::CbSumToZero);
    EXPECT_EQ(log.folds[3].dim, 1);
    EXPECT_EQ(log.replay_builds, 1);
    EXPECT_EQ(log.replay_folds, 1);
    EXPECT_EQ(log.replay_dim, 1);
    EXPECT_FALSE(log.solved);
    // reset() empties the log for the next epoch.
    est.reset();
    EXPECT_TRUE(est.sharedFoldLog().folds.empty());
    EXPECT_EQ(est.sharedFoldLog().replay_builds, 0);
}

// ---------------------------------------------------------------------------
// The table
// ---------------------------------------------------------------------------

TEST(NetworkContributionsTest, TablePrintsEveryKindTheZerosAndTheIntegrityLines) {
    ContributionTally ep;
    ep.epochs = 1;
    ep.station_blocks = 2;
    ep.station_epochs_checked = 2;
    ep.kinds[static_cast<std::size_t>(CK::DataCode)] = ContributionCount{40, 2};
    ep.gim_calls = 2;
    ep.gim_no_gim = 2;
    ContributionTally run = ep;
    run.merge(ep);
    const std::vector<std::string> lines = contributionsTable(ep, run, "first emitted epoch");
    // A header, the column line, one line per kind, four integrity lines.
    ASSERT_EQ(lines.size(), 2u + kContributionKinds + 4u);
    for (const ContributionSpec& s : contributionSpecs())
        EXPECT_NE(lineStarting(lines, std::string(s.name) + " "), nullptr) << s.name;
    const std::string* code = lineStarting(lines, "data_code ");
    ASSERT_NE(code, nullptr);
    EXPECT_NE(code->find("40"), std::string::npos);
    EXPECT_NE(code->find("80"), std::string::npos);   // the run's rows
    // A declared kind with no instance reads 0; the GIM line says why.
    const std::string* gim = lineStarting(lines, "gim_iono ");
    ASSERT_NE(gim, nullptr);
    EXPECT_NE(gim->find(" 0 "), std::string::npos) << *gim;
    EXPECT_NE(gim->find("4 with no GIM loaded"), std::string::npos) << *gim;
    EXPECT_TRUE(anyLineHas(lines, "station rows without a tag: 0 (2 station block(s) checked"));
    EXPECT_TRUE(anyLineHas(lines, "shared folds without a kind: 0"));
    const std::string json = contributionsJson(ep, run, "first_emitted_epoch");
    EXPECT_NE(json.find("\"record\":\"contributions\""), std::string::npos);
    for (const ContributionSpec& s : contributionSpecs())
        EXPECT_NE(json.find(std::string("\"kind\":\"") + s.name + "\""), std::string::npos) << s.name;
}

TEST(NetworkContributionsTest, TallyMergeSumsAndExtendsTheTimeRange) {
    ContributionTally a, b, run;
    a.epochs = 1; a.week_first = a.week_last = 2313; a.tow_first = a.tow_last = 100.0;
    a.kinds[0].rows = 3; a.kinds[0].folds = 1;
    b.epochs = 1; b.week_first = b.week_last = 2313; b.tow_first = b.tow_last = 130.0;
    b.kinds[0].rows = 5; b.kinds[0].folds = 2; b.untagged_rows = 1;
    run.merge(a);
    run.merge(b);
    EXPECT_EQ(run.epochs, 2);
    EXPECT_EQ(run.kinds[0].rows, 8);
    EXPECT_EQ(run.kinds[0].folds, 3);
    EXPECT_EQ(run.untagged_rows, 1);
    EXPECT_DOUBLE_EQ(run.tow_first, 100.0);
    EXPECT_DOUBLE_EQ(run.tow_last, 130.0);
}
