/**
 * @file test_network_contributions.cpp
 * @brief Card NOM step N2a: the contribution declarations, the row tags and the
 *        "contributions as armed" table of filter/NetworkContributions.hpp.
 *
 * The contracts:
 *  - every kind has a declaration (name, unit, weight, builder) in enum order, the names
 *    read apart, @c Unset closes the enum and only the data rows are observations;
 *  - the data-row tagger gives the base kinds the split's weight passes then refine, and
 *    keeps a row without identities aligned; the receiver-key helpers read the key's
 *    signal; a whitened block row's kind follows its column's family flags;
 *  - @c appendCouplingRows keeps the tags aligned with the rows on the scalar path (a
 *    whitened block row included), the joint path and the joint path's scalar fallback;
 *    the within-epoch re-whiten's tags follow the rows the whitening kernel reports;
 *  - the tag check sees a missing tag, an unset tag, a row out of place and a builder
 *    counter that disagrees, and is silent on a well-formed block;
 *  - the station drain counts by its rules (taken stations only, pre-PD entries always,
 *    post-PD entries only when folded, merged entries under their own kind, the GIM
 *    outcomes of every station the builder ran on);
 *  - the estimator's fold log records the kind a site named (per row for the code-bias
 *    datum stack), a fold with none as @c Unset, and a replay's re-folds apart;
 *  - the table prints every kind with its unit (a zero included, with the GIM and Q3
 *    notes) and the integrity lines.
 */
#include <gtest/gtest.h>

#include "common/GnssIdentity.hpp"
#include "config/GnssConfigTypes.hpp"     // GlobalVtecShConfig (the coupling builder)
#include "filter/NetworkContributions.hpp"
#include "filter/NetworkCoupling.hpp"     // CouplingRowBuilder, localBlockColKind
#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkParamRegistry.hpp"
#include "filter/NetworkRunTypes.hpp"     // StationAssembly, CplRowRaw, whitenCouplingRows
#include "filter/StateKeySchema.hpp"

#include <Eigen/Dense>

#include <map>
#include <set>
#include <string>
#include <tuple>
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

SatId sat(const char* id) { return *SatId::parse(id); }

RowTag tagOf(CK k) { return RowTag{k, {}, {}}; }

/// A station block of @p n rows with @p tags (n = tags.size()), zero coefficients.
void setRows(StationAssembly& R, const std::vector<RowTag>& tags, Eigen::Index ncol = 4) {
    const Eigen::Index n = static_cast<Eigen::Index>(tags.size());
    R.A_r = Eigen::MatrixXd::Zero(n, ncol);
    R.A_s = Eigen::MatrixXd::Zero(n, 2);
    R.w = Eigen::VectorXd::Ones(n);
    R.y = Eigen::VectorXd::Zero(n);
    R.row_tags = tags;
}

/// A coupling-row builder over a registered estimator (one clock and one phase bias),
/// whose station block starts with @p data_rows tagged data rows (the fixture of
/// test_network_coupling.cpp, with rows already in the block so that an offset shows).
struct BuilderFixture {
    NetworkEstimator est;
    GlobalVtecShConfig shc{};
    std::map<std::tuple<SharedParamKind, std::string, std::string>, double> tu_reinit_hist;
    StationAssembly R;
    Eigen::Index clk_col{-1}, pb_col{-1};

    explicit BuilderFixture(Eigen::Index data_rows = 2) {
        clk_col = est.registerClock("G01");
        pb_col  = est.registerPhaseBias("G01", "L1C");
        est.beginEpoch();
        R.A_s = Eigen::MatrixXd::Zero(data_rows, est.numShared());
        R.A_r = Eigen::MatrixXd::Zero(data_rows, 4);
        R.w = Eigen::VectorXd::Ones(data_rows);
        R.y = Eigen::VectorXd::Zero(data_rows);
        R.row_tags.assign(static_cast<std::size_t>(data_rows), RowTag{CK::DataPhase, {}, {}});
        R.n_data_rows = data_rows;
    }

    CouplingRowBuilder make(bool joint) {
        return CouplingRowBuilder{est, shc, /*q_clock=*/1e-4, /*q_cb=*/1e-4, /*q_pb=*/1e-4,
                                  joint, /*tu=*/false, /*shmarg=*/false, /*gauge=*/false,
                                  /*reinit=*/false, tu_reinit_hist, R};
    }

    LocalCarry carryOnClock() const {
        LocalCarry lc;
        lc.value_abs = 2.0;
        lc.last_seen_s = 0.0;
        lc.coupling = {CoupledShared{SharedParamKind::Clock, "G01", "", 0, 0, 'C',
                                     /*sh_layer=*/0, /*coeff=*/0.8, /*s_prev=*/1.5,
                                     /*p_prev=*/0.2}};
        return lc;
    }
};

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
        EXPECT_NE(std::string(contributionUnitName(s.unit)), "?") << s.name;
        EXPECT_EQ(&contributionSpec(s.kind), &s);
        EXPECT_STREQ(contributionKindName(s.kind), s.name);
    }
    // Unset closes the enum: it is not a family, and it names itself.
    EXPECT_EQ(static_cast<std::size_t>(CK::Unset), kContributionKinds);
    EXPECT_EQ(static_cast<std::size_t>(CK::DdPriorBlock) + 1, kContributionKinds);
    EXPECT_STREQ(contributionKindName(CK::Unset), "unset");
    EXPECT_EQ(names.count("unset"), 0u);
    // Every tag and fold record starts unset.
    EXPECT_EQ(RowTag{}.kind, CK::Unset);
    EXPECT_EQ(SharedFoldRecord{}.kind, CK::Unset);
    EXPECT_EQ(CplRowRaw{}.tag.kind, CK::Unset);
    EXPECT_EQ(PendCplRow{}.tag.kind, CK::Unset);
}

TEST(NetworkContributionsTest, OnlyTheDataRowsAreObservations) {
    std::set<CK> obs;
    for (const ContributionSpec& s : contributionSpecs())
        if (s.is_observation) obs.insert(s.kind);
    EXPECT_EQ(obs, (std::set<CK>{CK::DataCode, CK::DataCodeCompanion, CK::DataPhase,
                                 CK::DataPhaseSecond}));
}

TEST(NetworkContributionsTest, ChannelsStagesUnitsAndParametersAreDeclared) {
    EXPECT_EQ(contributionSpec(CK::DataPhase).channel, ContributionChannel::StationRows);
    EXPECT_EQ(contributionSpec(CK::PriorRxCbDatumPrePd).channel,
              ContributionChannel::StationPriorPrePd);
    EXPECT_EQ(contributionSpec(CK::PriorRxCbDatum).channel, ContributionChannel::StationPriorPostPd);
    EXPECT_EQ(contributionSpec(CK::CbSumToZero).channel, ContributionChannel::SharedRows);
    EXPECT_EQ(contributionSpec(CK::BorderCarryDense).channel, ContributionChannel::SharedDensePrior);
    EXPECT_EQ(contributionSpec(CK::ClockPivotPin).channel, ContributionChannel::Pins);
    EXPECT_EQ(contributionSpec(CK::DdPriorBlock).channel, ContributionChannel::DdBlocks);
    EXPECT_EQ(contributionSpec(CK::PbHeal).stage, ContributionStage::AtGate);
    EXPECT_EQ(contributionSpec(CK::BorderCarryDiagonal).stage, ContributionStage::AfterGate);
    EXPECT_EQ(contributionSpec(CK::Q3Withdraw).stage, ContributionStage::AfterSolve);
    EXPECT_EQ(contributionSpec(CK::Q3Restore).stage, ContributionStage::AfterSolve);
    EXPECT_EQ(contributionSpec(CK::DdPriorBlock).stage, ContributionStage::InSolve);
    // A code row touches the satellite clock and code bias, not the phase bias.
    const ContributionSpec& code = contributionSpec(CK::DataCode);
    EXPECT_TRUE(contributionTouches(code, NetworkParamKind::SatClock));
    EXPECT_TRUE(contributionTouches(code, NetworkParamKind::SatCodeBias));
    EXPECT_FALSE(contributionTouches(code, NetworkParamKind::SatPhaseBias));
    EXPECT_TRUE(contributionTouches(contributionSpec(CK::DataPhase), NetworkParamKind::Ambiguity));
    EXPECT_EQ(paramKindSetText(contributionSpec(CK::AmbiguityPin).params), "ambiguity");
    EXPECT_EQ(paramKindSetText(0), "");
    // The unit follows the channel: rows of a block or a constraint fold, entries of a
    // diagonal station prior, keys handed to a shared prior (walks for the clone walks),
    // and pins, gauges and DD blocks for their own channels.
    for (const ContributionSpec& s : contributionSpecs()) {
        ContributionUnit want = ContributionUnit::Keys;
        switch (s.channel) {
            case ContributionChannel::StationRows:
            case ContributionChannel::SharedRows:         want = ContributionUnit::Rows; break;
            case ContributionChannel::StationPriorPrePd:
            case ContributionChannel::StationPriorPostPd: want = ContributionUnit::Entries; break;
            case ContributionChannel::Pins:               want = ContributionUnit::Pins; break;
            case ContributionChannel::Gauges:             want = ContributionUnit::Gauges; break;
            case ContributionChannel::DdBlocks:           want = ContributionUnit::Blocks; break;
            default: want = s.kind == CK::CloneWalk ? ContributionUnit::Walks
                                                    : ContributionUnit::Keys;
        }
        EXPECT_EQ(s.unit, want) << s.name;
    }
    // The Q3 kinds say how their keys count.
    ASSERT_NE(contributionSpec(CK::Q3Withdraw).note, nullptr);
    EXPECT_NE(std::string(contributionSpec(CK::Q3Withdraw).note).find("whole captured block"),
              std::string::npos);
}

TEST(NetworkContributionsTest, NamesReadApartAndNameTheirRole) {
    // The four receiver code-bias kinds.
    EXPECT_STREQ(contributionKindName(CK::PriorRxCbCarry), "prior_rx_cb_carry");
    EXPECT_STREQ(contributionKindName(CK::PriorRxCbDatum), "prior_rx_cb_datum");
    EXPECT_STREQ(contributionKindName(CK::PriorRxCbDatumPrePd), "prior_rx_cb_datum_pre_pd");
    EXPECT_STREQ(contributionKindName(CK::BlockRxCbCarry), "block_rx_cb_carry");
    // Every station prior and block row names its role.
    const auto endsWith = [](const std::string& s, const std::string& e) {
        return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0;
    };
    for (const ContributionSpec& s : contributionSpecs()) {
        const std::string n = s.name;
        if (n.rfind("prior_", 0) != 0 && n.rfind("block_", 0) != 0) continue;
        EXPECT_TRUE(endsWith(n, "_carry") || endsWith(n, "_datum") || endsWith(n, "_datum_pre_pd")
                    || endsWith(n, "_oracle_pin"))
            << n;
    }
}

TEST(NetworkContributionsTest, CouplingRegionKindsAreTheAppendedOnes) {
    std::set<CK> in;
    for (const ContributionSpec& s : contributionSpecs())
        if (contributionInCouplingRegion(s.kind)) in.insert(s.kind);
    EXPECT_EQ(in, (std::set<CK>{CK::CouplingScalar, CK::CouplingJoint, CK::BlockAmbiguityCarry,
                                CK::BlockZwdCarry, CK::BlockIonoCarry, CK::BlockRxClockCarry,
                                CK::BlockRxCbCarry, CK::BlockPhaseColourCarry}));
    EXPECT_FALSE(contributionInCouplingRegion(CK::Unset));
}

// ---------------------------------------------------------------------------
// Row tags
// ---------------------------------------------------------------------------

TEST(NetworkContributionsTest, DataRowTagsGiveTheBaseKindsAndStayAligned) {
    const std::vector<std::string> sats = {"G05", "G07", "E11"};
    const std::vector<std::string> sigs = {"C1W", "L1W", "C1C"};
    std::vector<RowTag> tags;
    tags.push_back(RowTag{CK::SdEwl, {}, {}});   // cleared by the tagger
    // One row more than the identity vectors: tagged, with no identity.
    tagDataRows(tags, 4, sats, sigs);
    ASSERT_EQ(tags.size(), 4u);
    EXPECT_EQ(tags[0].kind, CK::DataCode);
    EXPECT_EQ(tags[0].sat.str(), "G05");
    EXPECT_EQ(tags[0].signal, sigOf(System::GPS, "C1W"));
    EXPECT_EQ(tags[1].kind, CK::DataPhase);
    EXPECT_EQ(tags[1].signal, sigOf(System::GPS, "L1W"));
    EXPECT_EQ(tags[2].kind, CK::DataCode);
    EXPECT_EQ(tags[2].sat.sys, System::Galileo);
    EXPECT_EQ(tags[3].kind, CK::DataCode);
    EXPECT_FALSE(tags[3].sat.valid());
    // The companion and second-phase kinds are the split's to give, in its weight passes.
    for (const RowTag& t : tags) {
        EXPECT_NE(t.kind, CK::DataCodeCompanion);
        EXPECT_NE(t.kind, CK::DataPhaseSecond);
    }
}

TEST(NetworkContributionsTest, TagsReadTheReceiverKeys) {
    const RowTag rpb = rxPhaseBiasTag(CK::Eq34RpbPin, K::makeReceiverPhaseBiasKey("GPS", "L2W"));
    EXPECT_EQ(rpb.kind, CK::Eq34RpbPin);
    EXPECT_FALSE(rpb.sat.valid());
    EXPECT_EQ(rpb.signal, sigOf(System::GPS, "L2W"));
    const RowTag isc = rxCodeBiasTag(CK::PriorRxCbCarry, K::makeReceiverIscKey("GPS", "C1C"));
    EXPECT_EQ(isc.signal, sigOf(System::GPS, "C1C"));
    EXPECT_EQ(rxCodeBiasSignal(K::makeReceiverIscKey("Galileo", "C1C")),
              sigOf(System::Galileo, "C1C"));
    const RowTag dcb = rxCodeBiasTag(CK::PriorRxCbCarry, "GPS_DCB");
    EXPECT_EQ(dcb.signal, SignalId{});
    EXPECT_EQ(rxPhaseBiasSignal("not-a-key"), SignalId{});
    const RowTag bad = makeRowTag(CK::GimIono, "not-a-sat", "C1C");
    EXPECT_FALSE(bad.sat.valid());
    EXPECT_EQ(makeRowTag(CK::ShIonoTie, "G12").sat.str(), "G12");
}

TEST(NetworkContributionsTest, BlockRowKindFollowsTheColumnFamilyFlags) {
    LocalBlockCol c{};
    EXPECT_EQ(localBlockColKind(c), CK::BlockAmbiguityCarry);   // no flag: an ambiguity
    c = LocalBlockCol{}; c.is_zwd = true;
    EXPECT_EQ(localBlockColKind(c), CK::BlockZwdCarry);
    c = LocalBlockCol{}; c.is_iono = true;
    EXPECT_EQ(localBlockColKind(c), CK::BlockIonoCarry);
    c = LocalBlockCol{}; c.is_rx_clk = true;
    EXPECT_EQ(localBlockColKind(c), CK::BlockRxClockCarry);
    c.is_rx_isb = true;                                          // an ISB is a clock-family column
    EXPECT_EQ(localBlockColKind(c), CK::BlockRxClockCarry);
    c = LocalBlockCol{}; c.is_rx_cb = true;
    EXPECT_EQ(localBlockColKind(c), CK::BlockRxCbCarry);
    c = LocalBlockCol{}; c.is_pc = true;
    EXPECT_EQ(localBlockColKind(c), CK::BlockPhaseColourCarry);
}

// The scalar path appends one tag per row after the rows already in the block: the scalar
// coupling row with its column's identity, then a whitened block row with its own tag.
TEST(NetworkContributionsTest, AppendCouplingRowsTagsTheScalarPath) {
    BuilderFixture f(/*data_rows=*/2);
    const LocalCarry lc = f.carryOnClock();
    auto cb = f.make(/*joint=*/false);
    cb.buildCplRow(/*col=*/0, /*x_lin_val=*/0.5, lc, /*var_eff=*/0.04, /*gap=*/30.0,
                   sat("G07"), sigOf(System::GPS, "L1C"));
    PendCplRow blk;   // a whitened block row (non-empty local), as buildLocalBlock buffers it
    blk.local_col = 3;
    blk.local = {{3, 2.0}};
    blk.w = 1.0;
    blk.tag = RowTag{CK::BlockZwdCarry, {}, {}};
    cb.crows.push_back(blk);
    cb.appendCouplingRows();

    ASSERT_EQ(f.R.A_r.rows(), 4);
    ASSERT_EQ(f.R.row_tags.size(), 4u);
    EXPECT_EQ(f.R.cpl_row_start, 2);
    EXPECT_EQ(f.R.row_tags[0].kind, CK::DataPhase);
    EXPECT_EQ(f.R.row_tags[2].kind, CK::CouplingScalar);
    EXPECT_EQ(f.R.row_tags[2].sat.str(), "G07");
    EXPECT_EQ(f.R.row_tags[2].signal, sigOf(System::GPS, "L1C"));
    EXPECT_EQ(f.R.row_tags[3].kind, CK::BlockZwdCarry);
    // The scalar row's own kind is set by the builder, whatever the caller passed.
    EXPECT_EQ(cb.crows[0].tag.kind, CK::CouplingScalar);
}

// The joint path tags its rows CouplingJoint, and the retained ingredients keep the tags
// for the refloat re-whiten.
TEST(NetworkContributionsTest, AppendCouplingRowsTagsTheJointPath) {
    BuilderFixture f(/*data_rows=*/1);
    const LocalCarry lc = f.carryOnClock();
    auto cb = f.make(/*joint=*/true);
    cb.buildCplRow(/*col=*/1, 0.0, lc, /*var_eff=*/0.02, 30.0, sat("G03"),
                   sigOf(System::GPS, "L2W"));
    cb.buildCplRow(/*col=*/2, 0.0, lc, /*var_eff=*/0.03, 30.0, sat("G04"),
                   sigOf(System::GPS, "L2W"));
    cb.appendCouplingRows();

    ASSERT_FALSE(f.R.cpl_joint_fallback);
    ASSERT_EQ(f.R.A_r.rows(), 3);
    ASSERT_EQ(f.R.row_tags.size(), 3u);
    EXPECT_EQ(f.R.row_tags[1].kind, CK::CouplingJoint);
    EXPECT_EQ(f.R.row_tags[1].sat.str(), "G03");
    EXPECT_EQ(f.R.row_tags[2].kind, CK::CouplingJoint);
    EXPECT_EQ(f.R.row_tags[2].sat.str(), "G04");
    ASSERT_EQ(f.R.cpl_raw.size(), 2u);
    EXPECT_EQ(f.R.cpl_raw[1].tag.sat.str(), "G04");
}

// A joint covariance that is not SPD falls back to the scalar rows: the same rows, tagged
// CouplingScalar, with their identities.
TEST(NetworkContributionsTest, AppendCouplingRowsTagsTheJointFallbackAsScalar) {
    BuilderFixture f(/*data_rows=*/2);
    auto cb = f.make(/*joint=*/true);
    CplRowRaw r;
    r.local_col = 1;
    r.var_eff = -1.0;                   // indefinite: the joint factor fails
    r.tag = RowTag{CK::CouplingJoint, sat("G09"), sigOf(System::GPS, "L1C")};
    cb.craw.push_back(r);
    cb.appendCouplingRows();

    ASSERT_TRUE(f.R.cpl_joint_fallback);
    ASSERT_EQ(f.R.A_r.rows(), 3);
    ASSERT_EQ(f.R.row_tags.size(), 3u);
    EXPECT_EQ(f.R.row_tags[2].kind, CK::CouplingScalar);
    EXPECT_EQ(f.R.row_tags[2].sat.str(), "G09");
    EXPECT_EQ(f.R.cpl_row_start, 2);
}

// The re-whiten's tags are the head rows, then the coupling rows the whitening kernel
// reports as kept, in its output order.
TEST(NetworkContributionsTest, JointRewhitenTagsFollowTheKernelsKeptRows) {
    // Three data rows, then three jointly whitened coupling rows on local columns 5, 6, 7.
    const std::vector<RowTag> tags = {
        tagOf(CK::DataCode), tagOf(CK::DataPhase), tagOf(CK::AmbiguityPin),
        RowTag{CK::CouplingJoint, sat("G01"), {}}, RowTag{CK::CouplingJoint, sat("G02"), {}},
        RowTag{CK::CouplingJoint, sat("G03"), {}}};
    std::vector<CplRowRaw> raw(3);
    for (int k = 0; k < 3; ++k) {
        raw[static_cast<std::size_t>(k)].local_col = 5 + k;
        raw[static_cast<std::size_t>(k)].var_eff = 0.05;
        raw[static_cast<std::size_t>(k)].tag = tags[static_cast<std::size_t>(3 + k)];
    }
    // The refloat drops column 6: the kernel keeps rows 0 and 2.
    Eigen::MatrixXd Hr, Hs;
    Eigen::VectorXd z;
    std::vector<Eigen::Index> kept;
    ASSERT_TRUE(whitenCouplingRows(raw, {}, std::set<Eigen::Index>{6}, 8, 0, Hr, Hs, z, kept));
    const std::vector<RowTag> out = rowTagsAfterJointRewhiten(tags, 3, raw, kept, Hr.rows());
    ASSERT_EQ(out.size(), 5u);
    EXPECT_EQ(out[2].kind, CK::AmbiguityPin);
    EXPECT_EQ(out[3].sat.str(), "G01");
    EXPECT_EQ(out[4].sat.str(), "G03");
    // A failed re-whiten appends no coupling row.
    EXPECT_EQ(rowTagsAfterJointRewhiten(tags, 3, raw, kept, 0).size(), 3u);
}

// ---------------------------------------------------------------------------
// The tag check
// ---------------------------------------------------------------------------

namespace {

/// A well-formed block: two data rows, an ambiguity pin, two coupling-region rows (a
/// scalar coupling row and a whitened block row), a phase-colour birth row; a post-PD
/// prior entry on a carry and an oracle pin, a pre-PD datum entry.
StationAssembly wellFormedBlock() {
    StationAssembly R;
    R.ok = true;
    setRows(R, {tagOf(CK::DataCode), tagOf(CK::DataPhase), tagOf(CK::AmbiguityPin),
                tagOf(CK::CouplingScalar), tagOf(CK::BlockAmbiguityCarry),
                tagOf(CK::PhaseColourBirth)});
    R.n_data_rows = 2;
    R.np = 1;
    R.cpl_row_start = 3;
    R.cpl_row_local_cols = {0, 1};
    R.pl_blk_rows = 1;
    R.pc_birth_rows = 1;
    R.carry_cols = {2, 3};
    R.carry_tags = {tagOf(CK::PriorAmbiguityCarry), tagOf(CK::PriorAmbiguityOraclePin)};
    R.opin_rows = 1;
    R.prior_cols = {1};
    R.prior_tags = {tagOf(CK::PriorRxCbDatumPrePd)};
    R.pl_cb_datum_rows = 1;
    return R;
}

}  // namespace

TEST(NetworkContributionsTest, TagCheckIsSilentOnAWellFormedBlock) {
    const StationTagCheck c = checkStationTags(wellFormedBlock());
    EXPECT_EQ(c.untagged, 0);
    EXPECT_EQ(c.unset, 0);
    EXPECT_EQ(c.misplaced, 0);
    EXPECT_EQ(c.mismatches, 0);
    EXPECT_EQ(c.unclassified, 0);
}

TEST(NetworkContributionsTest, TagCheckSeesEachDefect) {
    {   // a missing tag, in each channel
        StationAssembly R = wellFormedBlock();
        R.row_tags.pop_back();
        R.prior_cols.push_back(2);
        EXPECT_EQ(untaggedRows(R), 2);
        EXPECT_EQ(checkStationTags(R).untagged, 2);
    }
    {   // an unset tag
        StationAssembly R = wellFormedBlock();
        R.row_tags[2] = RowTag{};
        R.carry_tags[0] = RowTag{};
        const StationTagCheck c = checkStationTags(R);
        EXPECT_EQ(c.unset, 2);
        EXPECT_EQ(c.untagged, 0);
        EXPECT_EQ(c.unclassified, 1) << "the unset row tag (the diagonal entry is not a row)";
    }
    {   // a data kind past the split's rows, and a non-data kind inside them
        StationAssembly R = wellFormedBlock();
        R.row_tags[2] = tagOf(CK::DataPhase);
        R.row_tags[1] = tagOf(CK::ShIonoTie);
        EXPECT_EQ(checkStationTags(R).misplaced, 2);
    }
    {   // a coupling kind outside the region, a non-coupling kind inside it
        StationAssembly R = wellFormedBlock();
        R.row_tags[5] = tagOf(CK::CouplingJoint);
        R.row_tags[4] = tagOf(CK::GimIono);
        EXPECT_EQ(checkStationTags(R).misplaced, 2);
    }
    {   // a kind on the wrong channel: a prior kind as a row, a pre-PD kind post-PD
        StationAssembly R = wellFormedBlock();
        R.row_tags[2] = tagOf(CK::PriorZwdCarry);
        R.carry_tags[0] = tagOf(CK::PriorRxCbDatumPrePd);
        EXPECT_EQ(checkStationTags(R).misplaced, 2);
        EXPECT_EQ(checkStationTags(R).unclassified, 1) << "a prior kind on a row";
    }
    {   // the builders' counters: one mismatch per counter that disagrees
        StationAssembly R = wellFormedBlock();
        R.np = 2;
        R.pc_birth_rows = 0;
        R.pc_backstop_rows = 1;
        R.pl_blk_rows = 2;
        R.sd_ewl_rows = 3;
        R.opin_rows = 0;
        R.pl_cb_datum_rows = 0;
        EXPECT_EQ(checkStationTags(R).mismatches, 7);
    }
    {   // a merged entry counts toward its counter
        StationAssembly R = wellFormedBlock();
        R.prior_merged = {tagOf(CK::PriorRxCbDatumPrePd)};
        EXPECT_EQ(checkStationTags(R).mismatches, 1);
        R.pl_cb_datum_rows = 2;
        EXPECT_EQ(checkStationTags(R).mismatches, 0);
    }
}

// ---------------------------------------------------------------------------
// The station drain
// ---------------------------------------------------------------------------

TEST(NetworkContributionsTest, StationDrainCountsByItsRules) {
    std::vector<StationAssembly> asm_res(4);
    // Station 0: taken, post-PD entries folded, one of them merged; no GIM loaded.
    {
        StationAssembly& R = asm_res[0];
        R.ok = true;
        setRows(R, {tagOf(CK::DataCode), tagOf(CK::DataCode), tagOf(CK::DataPhase)});
        R.n_data_rows = 3;
        R.carry_cols = {0};
        R.carry_tags = {tagOf(CK::PriorZwdCarry)};
        R.carry_merged = {tagOf(CK::PriorRxCbCarry)};
        R.carry_prior_folded = true;
        R.prior_cols = {1};
        R.prior_tags = {tagOf(CK::PriorRxCbDatumPrePd)};
        R.pl_cb_datum_rows = 1;
        R.gim_rows = GimRowsOutcome::NoGim;
    }
    // Station 1: assembled, not taken; the GIM weighting off.
    {
        StationAssembly& R = asm_res[1];
        R.ok = true;
        setRows(R, {tagOf(CK::DataCode)});
        R.n_data_rows = 1;
        R.gim_rows = GimRowsOutcome::WeightingOff;
    }
    // Station 2: taken, post-PD entries not folded; a pre-PD entry merged into another.
    {
        StationAssembly& R = asm_res[2];
        R.ok = true;
        setRows(R, {tagOf(CK::DataPhase)});
        R.n_data_rows = 1;
        R.carry_cols = {0};
        R.carry_tags = {tagOf(CK::PriorAmbiguityCarry)};
        R.prior_cols = {2};
        R.prior_tags = {tagOf(CK::PriorRxCbDatumPrePd)};
        R.prior_merged = {tagOf(CK::PriorRxCbDatumPrePd)};
        R.pl_cb_datum_rows = 2;
    }
    // Station 3: never assembled (its rows are not checked), the GIM builder ran.
    {
        StationAssembly& R = asm_res[3];
        setRows(R, {tagOf(CK::DataCode)});
        R.row_tags.clear();   // would be an untagged row if it were checked
        R.gim_rows = GimRowsOutcome::Appended;
    }
    const std::vector<int> est_index_of = {0, -1, 1, -1};

    NetworkContributionLedger led;
    led.replay_realigned = 3;
    led.replay_row_tags[1] = {tagOf(CK::DataCode)};
    networkContributionsStationDrain(led, asm_res, est_index_of, 2313, 266370.0);
    const ContributionTally& t = led.pending;
    const auto at = [&](CK k) { return t.kinds[static_cast<std::size_t>(k)]; };

    EXPECT_EQ(t.station_epochs_checked, 3);   // stations 0, 1, 2
    EXPECT_EQ(t.station_blocks, 2);           // stations 0 and 2
    EXPECT_EQ(at(CK::DataCode).count, 2);     // station 0's two (station 1 not taken)
    EXPECT_EQ(at(CK::DataCode).folds, 1);
    EXPECT_EQ(at(CK::DataPhase).count, 2);    // stations 0 and 2
    EXPECT_EQ(at(CK::DataPhase).folds, 2);
    EXPECT_EQ(at(CK::PriorZwdCarry).count, 1);    // station 0, folded
    EXPECT_EQ(at(CK::PriorRxCbCarry).count, 1);   // station 0's merged entry, its own kind
    EXPECT_EQ(at(CK::PriorAmbiguityCarry).count, 0);   // station 2's carry did not fold
    EXPECT_EQ(at(CK::PriorRxCbDatumPrePd).count, 3);   // pre-PD entries count always
    EXPECT_EQ(at(CK::PriorRxCbDatumPrePd).folds, 2);
    EXPECT_EQ(t.coalesced_entries, 2);
    EXPECT_EQ(t.gim_calls, 3);
    EXPECT_EQ(t.gim_no_gim, 1);
    EXPECT_EQ(t.gim_weighting_off, 1);
    // The checks: every block well formed.
    EXPECT_EQ(t.untagged_rows, 0);
    EXPECT_EQ(t.unset_tags, 0);
    EXPECT_EQ(t.misplaced_rows, 0);
    EXPECT_EQ(t.counter_mismatches, 0);
    EXPECT_EQ(led.checked_all, 3);
    // The epoch's replay state is cleared.
    EXPECT_EQ(led.replay_realigned, 0);
    EXPECT_TRUE(led.replay_row_tags.empty());
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
    est.addSharedPrior({p});   // no kind named: recorded as unset
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
    EXPECT_EQ(log.folds[0].kind, CK::BorderCarryDiagonal);
    EXPECT_EQ(log.folds[0].dim, 1);
    EXPECT_EQ(log.folds[1].kind, CK::Unset);
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

TEST(NetworkContributionsTest, TablePrintsEveryKindTheUnitsAndTheIntegrityLines) {
    ContributionTally ep;
    ep.epochs = 1;
    ep.station_blocks = 2;
    ep.station_epochs_checked = 2;
    ep.kinds[static_cast<std::size_t>(CK::DataCode)] = ContributionCount{40, 2};
    ep.kinds[static_cast<std::size_t>(CK::Q3Withdraw)] = ContributionCount{215, 1};
    ep.gim_calls = 2;
    ep.gim_no_gim = 2;
    ContributionTally run = ep;
    run.merge(ep);
    const std::vector<std::string> lines = contributionsTable(ep, run, "first emitted epoch");
    // A header, the column line, one line per kind, eight integrity lines.
    ASSERT_EQ(lines.size(), 2u + kContributionKinds + 8u);
    for (const ContributionSpec& s : contributionSpecs()) {
        const std::string* l = lineStarting(lines, std::string(s.name) + " ");
        ASSERT_NE(l, nullptr) << s.name;
        EXPECT_NE(l->find(std::string(" ") + contributionUnitName(s.unit) + " "), std::string::npos)
            << *l;
    }
    const std::string* code = lineStarting(lines, "data_code ");
    ASSERT_NE(code, nullptr);
    EXPECT_NE(code->find(" rows "), std::string::npos);
    EXPECT_NE(code->find("40"), std::string::npos);
    EXPECT_NE(code->find("80"), std::string::npos);   // the run's rows
    // The Q3 count reads as keys, with the note that says which keys.
    const std::string* q3 = lineStarting(lines, "q3_withdraw ");
    ASSERT_NE(q3, nullptr);
    EXPECT_NE(q3->find(" keys "), std::string::npos) << *q3;
    EXPECT_NE(q3->find("215"), std::string::npos) << *q3;
    EXPECT_NE(q3->find("note: keys handed to each call"), std::string::npos) << *q3;
    // A declared kind with no instance reads 0; the GIM line says why.
    const std::string* gim = lineStarting(lines, "gim_iono ");
    ASSERT_NE(gim, nullptr);
    EXPECT_NE(gim->find(" 0 "), std::string::npos) << *gim;
    EXPECT_NE(gim->find("4 with no GIM loaded"), std::string::npos) << *gim;
    EXPECT_TRUE(anyLineHas(lines, "station rows without a tag: 0 (2 station block(s) checked"));
    EXPECT_TRUE(anyLineHas(lines, "station rows and diagonal entries tagged unset: 0"));
    EXPECT_TRUE(anyLineHas(lines, "station rows the readers file as unclassified (obs_other"));
    EXPECT_TRUE(anyLineHas(lines, "station rows and entries out of place: 0"));
    EXPECT_TRUE(anyLineHas(lines, "tag counts against the builders' counters: 0 mismatch(es)"));
    EXPECT_TRUE(anyLineHas(lines, "shared folds without a kind: 0"));
    const std::string json = contributionsJson(ep, run, "first_emitted_epoch");
    EXPECT_NE(json.find("\"record\":\"contributions\""), std::string::npos);
    EXPECT_NE(json.find("\"unset_tags\":[0,0]"), std::string::npos);
    EXPECT_NE(json.find("\"counter_mismatches\":[0,0]"), std::string::npos);
    EXPECT_NE(json.find("\"unclassified_rows\":[0,0]"), std::string::npos);
    for (const ContributionSpec& s : contributionSpecs())
        EXPECT_NE(json.find(std::string("\"kind\":\"") + s.name + "\""), std::string::npos) << s.name;
}

TEST(NetworkContributionsTest, TallyMergeSumsAndExtendsTheTimeRange) {
    ContributionTally a, b, run;
    a.epochs = 1; a.week_first = a.week_last = 2313; a.tow_first = a.tow_last = 100.0;
    a.kinds[0].count = 3; a.kinds[0].folds = 1;
    b.epochs = 1; b.week_first = b.week_last = 2313; b.tow_first = b.tow_last = 130.0;
    b.kinds[0].count = 5; b.kinds[0].folds = 2; b.untagged_rows = 1; b.misplaced_rows = 2;
    run.merge(a);
    run.merge(b);
    EXPECT_EQ(run.epochs, 2);
    EXPECT_EQ(run.kinds[0].count, 8);
    EXPECT_EQ(run.kinds[0].folds, 3);
    EXPECT_EQ(run.untagged_rows, 1);
    EXPECT_EQ(run.misplaced_rows, 2);
    EXPECT_DOUBLE_EQ(run.tow_first, 100.0);
    EXPECT_DOUBLE_EQ(run.tow_last, 130.0);
}
