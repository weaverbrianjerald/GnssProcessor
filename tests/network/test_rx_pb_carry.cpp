/**
 * @file test_rx_pb_carry.cpp
 * @brief The eq34 receiver phase bias carried inside the station's local block
 *        (network.phase_datum_eq34_rpb_in_block, network/carry/RxPhaseBiasCarry.hpp).
 *
 * What is tested here, each at the level a unit test can reach:
 *   1. the harvest's step rule (rxPbStepExceeds), the event response of the carried column;
 *   2. the key's interlocks in the loader and the predicates and tables that follow from it
 *      (the knob snapshot, the parameter registry's route, the response site, the block
 *      row's kind);
 *   3. the block's algebra with a receiver phase-bias column: its row, its counters, and the
 *      statement the route exists for: folded in the block with its cross covariance, the
 *      carried information is the joint posterior's, while the diagonal fold beside the
 *      block states the weakly determined direction about twice.
 *
 * The root row's read of the carried entry is a case of the chain election's oracle
 * (tests/network/test_network_arc_graph_oracles.cpp); the harvest and the fold on data are
 * graded by the arms of card IRDG.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "config/GnssConfigTypes.hpp"
#include "logging/LogManager.hpp"
#include "network/assembly/ContributionKind.hpp"
#include "network/assembly/Contributions.hpp"
#include "network/assembly/ParamRegistry.hpp"
#include "network/assembly/Responses.hpp"
#include "network/carry/Coupling.hpp"
#include "network/carry/RxPhaseBiasCarry.hpp"
#include "network/solve/Estimator.hpp"
#include "network/state/RunContext.hpp"

#include <Eigen/Dense>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include "../support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

/// A minimal two-station GPS network configuration with @p network_extra in its network section.
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

/// The posture the key needs: the leveling carry as conditional coupling rows with a local
/// block, and the eq34 chain (whose receiver phase bias is carried at its default q).
const char* const kChainBlockPosture =
    "    \"phase_leveling\": true,\n"
    "    \"carry_conditional_coupling\": true,\n"
    "    \"shared_carry_full_cov\": true,\n"
    "    \"phase_leveling_local_block\": true,\n"
    "    \"phase_datum_eq34\": true,\n";

/// Write @p body to a temporary file and load it into @p cfg.
bool loadInto(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = gnss_test::tempDir() / ("rx_pb_carry_cfg_" + tag + ".json");
    {
        std::ofstream f(p);
        f << body;
    }
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec;
    fs::remove(p, ec);
    return ok;
}

/// The estimator, the station's slot and a coupling-row builder over them.
struct BlockFixture {
    NetworkEstimator est;
    GlobalVtecShConfig shc{};
    net::SatSignalTable tu_reinit_hist;
    StationAssembly R;
    Eigen::Index pb_col{-1};

    BlockFixture() {
        est.registerClock("G01");
        pb_col = est.registerPhaseBias("G01", "L1C");
        est.beginEpoch();
        R.A_s = Eigen::MatrixXd::Zero(0, est.numShared());
        R.A_r = Eigen::MatrixXd::Zero(0, 6);
        R.w.resize(0);
        R.y.resize(0);
    }

    CouplingRowBuilder make() {
        return CouplingRowBuilder{est, shc, /*q_clock=*/1e-4, /*q_cb=*/1e-4, /*q_pb=*/1e-4,
                                  /*tu=*/false, /*gauge=*/false, /*reinit=*/false,
                                  tu_reinit_hist, R};
    }
};

/// A carried local column at row @p idx of @p blk, with one border term (the satellite's phase
/// bias) so its row carries a regressor.
LocalCarry carryAt(double value_abs, const std::shared_ptr<PlHarvestBlock>& blk, int idx) {
    LocalCarry lc;
    lc.value_abs   = value_abs;
    lc.var         = blk ? blk->C(idx, idx) : 1.0;
    lc.last_seen_s = 0.0;
    lc.coupling    = {CoupledShared{SharedParamKind::PhaseBias, "G01", "L1C", 0, 0, 'C',
                                    /*sh_layer=*/0, /*coeff=*/-1.0, /*s_prev=*/0.5,
                                    /*p_prev=*/0.01}};
    lc.blk     = blk;
    lc.blk_idx = idx;
    return lc;
}

/// The information the buffered rows of @p cb put on the local columns: sum over rows of the
/// outer product of each row's local coefficients, times its weight.
Eigen::MatrixXd localInformation(const CouplingRowBuilder& cb, Eigen::Index n_local) {
    Eigen::MatrixXd info = Eigen::MatrixXd::Zero(n_local, n_local);
    for (const PendCplRow& row : cb.crows) {
        Eigen::VectorXd h = Eigen::VectorXd::Zero(n_local);
        if (row.local.empty())
            h(row.local_col) = 1.0;   // a scalar row: +1 on its column
        else
            for (const auto& [col, coeff] : row.local) h(col) = coeff;
        info += row.w * h * h.transpose();
    }
    return info;
}

}  // namespace

// 1: the step rule, the in-block route's response to a jump of the carried bias
TEST(RxPhaseBiasCarry, StepRuleComparesTwoHarvestsAgainstTheirVariancesTheWalkAndTheGuard) {
    const double q = 3e-6, horizon = 900.0;
    // Both variances zero and no gap: the guard alone, 10 * 0.05 m.
    EXPECT_FALSE(rxPbStepExceeds(10.49, 0.0, 10.0, 0.0, q, 0.0, horizon));
    EXPECT_TRUE(rxPbStepExceeds(10.51, 0.0, 10.0, 0.0, q, 0.0, horizon));
    EXPECT_TRUE(rxPbStepExceeds(9.49, 0.0, 10.0, 0.0, q, 0.0, horizon));   // either sign
    // The two harvested variances widen the test: sigma^2 = 1 + 1 + guard, 10 sigma = 14.15 m.
    EXPECT_FALSE(rxPbStepExceeds(24.0, 1.0, 10.0, 1.0, q, 0.0, horizon));
    EXPECT_TRUE(rxPbStepExceeds(24.3, 1.0, 10.0, 1.0, q, 0.0, horizon));
    // The walk over the gap widens it too: q * 900 s = 0.0027 m^2 beside the 0.0025 m^2 guard.
    const double thr = kRxPbStepSigma * std::sqrt(q * 900.0 + kRxPbStepGuardVarM2);
    EXPECT_FALSE(rxPbStepExceeds(10.0 + thr - 1e-6, 0.0, 10.0, 0.0, q, 900.0, horizon));
    EXPECT_TRUE(rxPbStepExceeds(10.0 + thr + 1e-6, 0.0, 10.0, 0.0, q, 900.0, horizon));
    // An entry outside the gap horizon, or stamped in the future, is the gap gate's: no test.
    EXPECT_FALSE(rxPbStepExceeds(99.0, 0.0, 10.0, 0.0, q, 900.1, horizon));
    EXPECT_FALSE(rxPbStepExceeds(99.0, 0.0, 10.0, 0.0, q, -30.0, horizon));
    // A non-finite value or variance never trips (the harvest refuses those before the test).
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(rxPbStepExceeds(nan, 0.0, 10.0, 0.0, q, 0.0, horizon));
    EXPECT_FALSE(rxPbStepExceeds(99.0, nan, 10.0, 0.0, q, 0.0, horizon));
}

// 1b: the block-column step's decision: the gap gate, the block rule and the variance
TEST(RxPhaseBiasCarry, BlockGateFoldsAnEntryInsideTheHorizonWithASharedBlockRow) {
    const double q = 3e-6, horizon = 900.0;
    double var_eff = -1.0;
    // The ordinary case: 30 s old, a block row the station's ambiguities share.
    EXPECT_EQ(rxPbBlockGate(30.0, horizon, 4.0, q, true, true, true, var_eff), RxPbBlockAction::Fold);
    EXPECT_DOUBLE_EQ(var_eff, 4.0 + q * 30.0);
    // Exactly at the horizon is inside it; the walk over the gap is in the variance.
    EXPECT_EQ(rxPbBlockGate(horizon, horizon, 4.0, q, true, true, true, var_eff), RxPbBlockAction::Fold);
    EXPECT_DOUBLE_EQ(var_eff, 4.0 + q * 900.0);
    // No other column pending: the bias's columns fold among themselves, whatever the block.
    EXPECT_EQ(rxPbBlockGate(30.0, horizon, 4.0, q, true, false, false, var_eff), RxPbBlockAction::Fold);
}

TEST(RxPhaseBiasCarry, BlockGateDoesNotFoldPastTheHorizonOrWithoutASharedBlockRow) {
    const double q = 3e-6, horizon = 900.0;
    double var_eff = -1.0;
    // The gap's decoupling edge: past the horizon, or stamped in the future.
    EXPECT_EQ(rxPbBlockGate(900.1, horizon, 4.0, q, true, true, true, var_eff), RxPbBlockAction::SkipGap);
    EXPECT_EQ(rxPbBlockGate(-30.0, horizon, 4.0, q, true, true, true, var_eff), RxPbBlockAction::SkipGap);
    EXPECT_EQ(rxPbBlockGate(std::numeric_limits<double>::quiet_NaN(), horizon, 4.0, q, true, true, true, var_eff),
              RxPbBlockAction::SkipGap);
    // No block row: the entry would fold as a diagonal prior.
    EXPECT_EQ(rxPbBlockGate(30.0, horizon, 4.0, q, false, true, false, var_eff), RxPbBlockAction::SkipNoBlock);
    EXPECT_EQ(rxPbBlockGate(30.0, horizon, 4.0, q, false, false, false, var_eff), RxPbBlockAction::SkipNoBlock);
    // A block row no pending column shares (a datum receiver that lost its seat, with a stale
    // entry beside freshly harvested ambiguities): every cross term would be zeroed.
    EXPECT_EQ(rxPbBlockGate(30.0, horizon, 4.0, q, true, true, false, var_eff), RxPbBlockAction::SkipNoBlock);
    // The gap gate is read first: a stale entry with no block is the gap's.
    EXPECT_EQ(rxPbBlockGate(1200.0, horizon, 4.0, q, false, true, false, var_eff), RxPbBlockAction::SkipGap);
    // A variance that is not a positive number folds nothing.
    EXPECT_EQ(rxPbBlockGate(0.0, horizon, 0.0, q, true, true, true, var_eff), RxPbBlockAction::SkipVariance);
    EXPECT_EQ(rxPbBlockGate(30.0, horizon, std::numeric_limits<double>::quiet_NaN(), q, true, true, true, var_eff),
              RxPbBlockAction::SkipVariance);
    EXPECT_DOUBLE_EQ(var_eff, -1.0);   // written on Fold only
}

// 2: the loader's interlocks: the key needs the chain's carried bias and a local block
TEST(RxPhaseBiasCarry, LoaderRefusesTheKeyWithoutTheChainItsCarryOrALocalBlock) {
    const std::string key = "    \"phase_datum_eq34_rpb_in_block\": true,\n";
    GnssConfig ok;
    ASSERT_TRUE(loadInto("ok", networkConfig(std::string(kChainBlockPosture) + key), ok));
    EXPECT_TRUE(ok.networkPhaseDatumEq34RpbInBlock());

    GnssConfig no_chain;   // the block posture without the chain
    EXPECT_FALSE(loadInto("no_chain",
                          networkConfig("    \"phase_leveling\": true,\n"
                                        "    \"carry_conditional_coupling\": true,\n"
                                        "    \"shared_carry_full_cov\": true,\n"
                                        "    \"phase_leveling_local_block\": true,\n" + key),
                          no_chain));
    GnssConfig no_link;    // the chain with the bias's carry switched off
    EXPECT_FALSE(loadInto("no_link",
                          networkConfig(std::string(kChainBlockPosture)
                                        + "    \"phase_datum_eq34_rpb_q_m2s\": 0.0,\n" + key),
                          no_link));
    GnssConfig no_block;   // the chain on coupling rows with no local block
    EXPECT_FALSE(loadInto("no_block",
                          networkConfig("    \"phase_leveling\": true,\n"
                                        "    \"carry_conditional_coupling\": true,\n"
                                        "    \"shared_carry_full_cov\": true,\n"
                                        "    \"phase_datum_eq34\": true,\n" + key),
                          no_block));
    GnssConfig off;        // the key absent: the default
    ASSERT_TRUE(loadInto("off", networkConfig(kChainBlockPosture), off));
    EXPECT_FALSE(off.networkPhaseDatumEq34RpbInBlock());
}

// 3: what follows from the key: the live predicate, the registry's route, the response site
TEST(RxPhaseBiasCarry, KeySetsTheLivePredicateTheRegistryRouteAndTheStepResponse) {
    for (const bool armed : {false, true}) {
        SCOPED_TRACE(armed ? "key on" : "key off");
        GnssConfig cfg;
        ASSERT_TRUE(loadInto(armed ? "k_on" : "k_off",
                             networkConfig(std::string(kChainBlockPosture)
                                           + (armed ? "    \"phase_datum_eq34_rpb_in_block\": true,\n" : "")),
                             cfg));
        const NetworkKnobs k = NetworkKnobs::snapshot(cfg, cfg.getNetworkConfig());
        ASSERT_TRUE(k.phase_datum_eq34);
        ASSERT_TRUE(k.pl_blk_on);
        ASSERT_GT(k.eq34_rpb_q, 0.0);
        EXPECT_EQ(k.rpb_blk, armed);
        EXPECT_EQ(k.rpb_blk_on, armed);
        const NetworkParamRegistry r = NetworkParamRegistry::build(k, cfg);
        const ParamSpec& rpb = r.spec(NetworkParamKind::RxPhaseBias);
        EXPECT_TRUE(rpb.carried);
        EXPECT_EQ(rpb.route, armed ? ParamRoute::LocalBlock : ParamRoute::PriorRow);
        EXPECT_DOUBLE_EQ(rpb.gap_horizon_s, k.pl_max_gap_s);
        // The step response exists on the in-block route only; the gap responses on both.
        EXPECT_EQ(responseSiteArmed(ResponseSite::RpbStepErase, k, cfg), armed);
        EXPECT_TRUE(responseSiteArmed(ResponseSite::RpbGapSkip, k, cfg));
        EXPECT_TRUE(responseSiteArmed(ResponseSite::RpbGapPrune, k, cfg));
        EXPECT_TRUE(responseSiteArmed(ResponseSite::RpbEq34Erase, k, cfg));
    }
    // The live predicate needs the carry: with the chain off the key cannot be loaded, and a
    // snapshot whose q is zero has the predicate off whatever the key says.
    NetworkKnobs k{};
    k.rpb_blk = true;
    k.pl_blk_on = true;
    k.eq34_rpb_q = 0.0;
    EXPECT_FALSE(k.rpb_blk && k.pl_blk_on && k.eq34_rpb_q > 0.0);
}

// 4: the kind and the declared treatment of the block row
TEST(RxPhaseBiasCarry, BlockRowKindIsDeclaredInTheCouplingRegionAfterTheReceiverCodeBias) {
    using CK = ContributionKind;
    LocalBlockCol c{};
    c.is_rx_pb = true;
    EXPECT_EQ(localBlockColKind(c), CK::BlockRxPbCarry);
    EXPECT_STREQ(contributionKindName(CK::BlockRxPbCarry), "block_rx_pb_carry");
    EXPECT_TRUE(contributionInCouplingRegion(CK::BlockRxPbCarry));
    EXPECT_EQ(static_cast<int>(CK::BlockRxPbCarry), static_cast<int>(CK::BlockRxCbCarry) + 1);
    EXPECT_EQ(static_cast<int>(CK::BlockPhaseColourCarry), static_cast<int>(CK::BlockRxPbCarry) + 1);
    const ContributionSpec& spec = contributionSpecs()[static_cast<std::size_t>(CK::BlockRxPbCarry)];
    EXPECT_EQ(spec.kind, CK::BlockRxPbCarry);
    EXPECT_TRUE(contributionTouches(spec, NetworkParamKind::RxPhaseBias));
    EXPECT_TRUE(contributionTouches(spec, NetworkParamKind::Ambiguity));     // pushed before it
    EXPECT_FALSE(contributionTouches(spec, NetworkParamKind::PhaseColour));  // pushed after it
    // A phase-colour row is pushed after the receiver phase bias and spans it.
    EXPECT_TRUE(contributionTouches(contributionSpecs()[static_cast<std::size_t>(CK::BlockPhaseColourCarry)],
                                    NetworkParamKind::RxPhaseBias));
}

// 5: a receiver phase-bias column joins the whitened block with its harvested cross terms
TEST(RxPhaseBiasCarry, ColumnJoinsTheWhitenedBlockWithItsCrossTermsAndItsOwnCounters) {
    BlockFixture f;
    // Two ambiguities and one receiver phase bias of one harvest: the bias is strongly
    // anti-correlated with both (their sums are what the phase rows determine).
    auto blk = std::make_shared<PlHarvestBlock>();
    blk->C.resize(3, 3);
    blk->C << 4.00, 3.90, -3.95,
              3.90, 4.00, -3.95,
             -3.95, -3.95, 4.00;
    blk->stamp_s = 1000.0;
    const LocalCarry a0 = carryAt(20.0, blk, 0), a1 = carryAt(-7.0, blk, 1), pb = carryAt(31.5, blk, 2);
    std::vector<LocalBlockCol> bc = {
        LocalBlockCol{0, 19.5, &a0, blk->C(0, 0), 30.0, 1.0},
        LocalBlockCol{1, -7.5, &a1, blk->C(1, 1), 30.0, 1.0},
        LocalBlockCol{2, 31.0, &pb, blk->C(2, 2), 30.0, 1.0},
    };
    bc[2].is_rx_pb = true;
    auto cb = f.make();
    ASSERT_TRUE(cb.buildLocalBlock(bc));
    ASSERT_EQ(cb.crows.size(), 3u);
    EXPECT_EQ(cb.crows[2].local_col, 2);
    EXPECT_EQ(cb.crows[2].tag.kind, ContributionKind::BlockRxPbCarry);
    EXPECT_EQ(cb.crows[0].tag.kind, ContributionKind::BlockAmbiguityCarry);
    const Eigen::MatrixXd& S = f.R.pl_blk.Sigma;
    ASSERT_EQ(S.rows(), 3);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) EXPECT_DOUBLE_EQ(S(i, j), blk->C(i, j));
    EXPECT_EQ(f.R.pl_blk_rxpb_cols, 1);
    EXPECT_EQ(f.R.pl_blk_rxpb_missing_cross, 0);
    EXPECT_EQ(f.R.pl_blk_pair_missing, 0);
    EXPECT_EQ(f.R.pl_blk_rows, 3);

    // An entry of another harvest pairs with nothing: its two pairs are zeroed and counted.
    BlockFixture g;
    auto other = std::make_shared<PlHarvestBlock>();
    other->C = Eigen::MatrixXd::Identity(1, 1) * 4.0;
    const LocalCarry stale = carryAt(31.5, other, 0);
    bc[2].lc = &stale;
    auto cb2 = g.make();
    ASSERT_TRUE(cb2.buildLocalBlock(bc));
    EXPECT_EQ(g.R.pl_blk_rxpb_cols, 1);
    EXPECT_EQ(g.R.pl_blk_rxpb_missing_cross, 2);
    EXPECT_EQ(g.R.pl_blk_pair_missing, 2);
    EXPECT_DOUBLE_EQ(g.R.pl_blk.Sigma(0, 2), 0.0);
}

// 6: the statement the route exists for.  In the block, with its cross covariance, the carried
//    information is the joint posterior's; the diagonal fold beside the block states the weakly
//    determined direction about twice.
TEST(RxPhaseBiasCarry, BlockFoldCarriesTheJointPosteriorWhereTheDiagonalFoldCountsTwice) {
    // One ambiguity N and the receiver phase bias d of one station.  The phase rows determine
    // N + d tightly and N - d loosely: a joint covariance with variances 4 and covariance -3.96.
    Eigen::Matrix2d C;
    C << 4.00, -3.96,
        -3.96,  4.00;
    auto blk = std::make_shared<PlHarvestBlock>();
    blk->C = C;
    const LocalCarry n = carryAt(20.0, blk, 0), d = carryAt(31.5, blk, 1);
    const Eigen::Vector2d loose = Eigen::Vector2d(1.0, -1.0) / std::sqrt(2.0);
    const Eigen::Vector2d tight = Eigen::Vector2d(1.0, 1.0) / std::sqrt(2.0);
    const double joint_loose = loose.dot(C.inverse() * loose);   // 1 / 7.96
    const double joint_tight = tight.dot(C.inverse() * tight);   // 1 / 0.04
    ASSERT_NEAR(joint_loose, 1.0 / 7.96, 1e-12);
    ASSERT_NEAR(joint_tight, 1.0 / 0.04, 1e-9);

    // The block route: both columns in one whitened block.
    BlockFixture f;
    std::vector<LocalBlockCol> both = {LocalBlockCol{0, 19.5, &n, C(0, 0), 30.0, 1.0},
                                       LocalBlockCol{1, 31.0, &d, C(1, 1), 30.0, 1.0}};
    both[1].is_rx_pb = true;
    auto cb = f.make();
    ASSERT_TRUE(cb.buildLocalBlock(both));
    const Eigen::MatrixXd info_block = localInformation(cb, 2);
    EXPECT_NEAR(loose.dot(info_block * loose), joint_loose, 1e-9);
    EXPECT_NEAR(tight.dot(info_block * tight), joint_tight, 1e-6);
    EXPECT_LT((info_block - C.inverse()).cwiseAbs().maxCoeff(), 1e-6);

    // The diagonal route: the ambiguity alone in the block (the bias marginalized out of it),
    // and the bias as a scalar prior at its own conditional variance.
    BlockFixture g;
    std::vector<LocalBlockCol> amb_only = {LocalBlockCol{0, 19.5, &n, C(0, 0), 30.0, 1.0}};
    auto cd = g.make();
    ASSERT_TRUE(cd.buildLocalBlock(amb_only));
    Eigen::MatrixXd info_diag = localInformation(cd, 2);
    info_diag(1, 1) += 1.0 / C(1, 1);   // the time link's row: +1 on the bias, weight 1 / var
    const double diag_loose = loose.dot(info_diag * loose);
    // The loose direction holds 1 / 7.96 of information; the diagonal route states 1 / 4,
    // twice as much, with no observation behind the excess.  Folded every epoch, that is the
    // collapse of the formal sigma the route removes.
    EXPECT_NEAR(diag_loose, 0.25, 1e-9);
    EXPECT_GT(diag_loose / joint_loose, 1.98);
    EXPECT_LT(diag_loose / joint_loose, 2.0);
}
