/**
 * @file test_network_rhs_attrib.cpp
 * @brief Card NOM step N2b: the reduced-RHS attribution iterates exactly the channels the
 *        kernel folds, so its closure holds by construction (network/qc/RhsAttrib.hpp).
 *
 * Card NOM section 16 (suspect 2) found that the attribution read a station's rows and its
 * post-PD diagonal prior but not the pre-PD station prior that @c addStation folds with
 * the block (@c network.station_prior_before_pd, the receiver DCB datum on the goldens),
 * so a dump would have left that channel's share of the reduced RHS unattributed.  The
 * fixture here is a small network whose stations carry every station channel (rows of
 * several kinds, a pre-PD prior on the receiver code-bias column, a post-PD prior on an
 * ambiguity) and whose border receives a diagonal prior and a constraint-row fold with one
 * kind per row.  The contracts:
 *  - the classes sum to the kernel's reduced RHS (relative closure at rounding);
 *  - the pre-PD channel has its own non-zero class, and leaving it out breaks the closure
 *    (the defect the step closes);
 *  - the post-PD channel is attributed only when the consume folded it;
 *  - each shared fold is classed by the kind its site named, one class per row kind for a
 *    constraint-row fold;
 *  - the replay's single re-fold of the captured priors, which are of several kinds, is
 *    classed and logged per kind (one kind per captured prior) and folds exactly what the
 *    unnamed call folds, under the plain and the gauge-free fold.
 */
#include <gtest/gtest.h>

#include "network/assembly/Contributions.hpp"
#include "network/solve/Estimator.hpp"
#include "network/solve/NormalEquations.hpp"
#include "network/qc/RhsAttrib.hpp"
#include "network/driver/RunTypes.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <Eigen/Dense>

#include <algorithm>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

using CK = ContributionKind;

/// The fixture network: two satellites (G01, G02), each with a clock and an L1C phase
/// bias on the border; three stations, each with four local columns (receiver clock,
/// the two ambiguities, a receiver code bias that only the code rows see).
struct Fixture {
    NetworkEstimator             est;
    std::vector<NetStation>      stations;
    std::vector<StationAssembly> asm_res;
    std::vector<int>             est_index_of;
    NetworkEstimator::Datum      datum;
    Eigen::Index clk[2]{-1, -1};
    Eigen::Index pb[2]{-1, -1};
};

/// One station's rows: per satellite a code row (rx clock + rx code bias - sat clock) and a
/// phase row (rx clock - sat clock + phase bias + ambiguity); the first station also pins
/// its two ambiguities.  Innovations are arbitrary but distinct.
void buildRows(Fixture& f, int r, StationAssembly& R) {
    const bool pivot = (r == 0);
    const Eigen::Index m = 4 + (pivot ? 2 : 0);
    const Eigen::Index ns = f.est.numShared();
    R = StationAssembly{};
    R.ok = true;
    R.A_s = Eigen::MatrixXd::Zero(m, ns);
    R.A_r = Eigen::MatrixXd::Zero(m, 4);
    R.w = Eigen::VectorXd::Ones(m);
    R.y = Eigen::VectorXd::Zero(m);
    const char* sats[2] = {"G01", "G02"};
    Eigen::Index row = 0;
    for (int s = 0; s < 2; ++s) {
        R.A_s(row, f.clk[s]) = -1.0;
        R.A_r(row, 0) = 1.0;
        R.A_r(row, 3) = 1.0;
        R.w(row) = 4.0;
        R.y(row) = 1.5 + 0.7 * r - 0.3 * s;
        R.row_tags.push_back(makeRowTag(CK::DataCode, sats[s], "C1C"));
        ++row;
        R.A_s(row, f.clk[s]) = -1.0;
        R.A_s(row, f.pb[s]) = 1.0;
        R.A_r(row, 0) = 1.0;
        R.A_r(row, 1 + s) = 1.0;
        R.w(row) = 1.0e4;
        R.y(row) = 2.25 - 0.4 * r + 0.9 * s;
        R.row_tags.push_back(makeRowTag(CK::DataPhase, sats[s], "L1C"));
        ++row;
    }
    R.n_data_rows = row;
    if (pivot) {
        for (int s = 0; s < 2; ++s) {
            R.A_r(row, 1 + s) = 1.0;
            R.w(row) = 1.0e10;
            R.row_tags.push_back(makeRowTag(CK::AmbiguityPin, sats[s], "L1C"));
            ++row;
        }
        R.np = 2;
    }
    // The pre-PD channel: a zero-mean datum on the receiver code bias (eta = lambda (0 -
    // x_lin), x_lin != 0), folded with the block by addStation.
    R.prior_cols = {3};
    R.prior_dNrr = Eigen::VectorXd::Constant(1, 0.01);
    R.prior_dur = Eigen::VectorXd::Constant(1, 0.01 * (0.0 - (0.92 + 0.5 * r)));
    R.prior_tags = {RowTag{CK::PriorRxCbDatumPrePd, {}, {}}};
    // The post-PD channel: a carried prior on the second ambiguity (non-pivot stations).
    if (!pivot) {
        R.carry_cols = {2};
        R.carry_dNrr = Eigen::VectorXd::Constant(1, 50.0);
        R.carry_dur = Eigen::VectorXd::Constant(1, 50.0 * (0.125 * r));
        R.carry_tags = {makeRowTag(CK::PriorAmbiguityCarry, "G02", "L1C")};
    }
}

/// Build, fold and solve the fixture.  @p fold_post_pd folds the post-PD channel (and
/// marks it folded) on the non-pivot stations.
void solveFixture(Fixture& f, bool fold_post_pd) {
    f.clk[0] = f.est.registerClock("G01");
    f.clk[1] = f.est.registerClock("G02");
    f.pb[0] = f.est.registerPhaseBias("G01", "L1C");
    f.pb[1] = f.est.registerPhaseBias("G02", "L1C");
    f.est.attribArm(true);
    f.est.beginEpoch();
    f.stations.resize(3);
    f.asm_res.resize(3);
    f.est_index_of.assign(3, -1);
    for (int r = 0; r < 3; ++r) {
        f.stations[static_cast<std::size_t>(r)].name = "STA" + std::to_string(r);
        StationAssembly& R = f.asm_res[static_cast<std::size_t>(r)];
        buildRows(f, r, R);
        const NetworkNormalEquations::StationPrior prior{R.prior_cols, R.prior_dNrr, R.prior_dur};
        const std::size_t slot = f.est.addStation(R.A_s, R.A_r, R.w, R.y, prior);
        f.est_index_of[static_cast<std::size_t>(r)] = static_cast<int>(slot);
        if (fold_post_pd && !R.carry_cols.empty()) {
            f.est.addStationPrior(slot, R.carry_cols, R.carry_dNrr, R.carry_dur);
            R.carry_prior_folded = true;
        }
    }
    // A diagonal border prior on G02's clock.
    NetworkEstimator::SharedPrior p;
    p.kind = SharedParamKind::Clock;
    p.sat = "G02";
    p.value = 0.1;
    p.info = 4.0;
    f.est.contributionKind(CK::BorderCarryDiagonal);
    f.est.addSharedPrior({p});
    // Two constraint rows on the phase biases, one kind each.
    Eigen::MatrixXd H = Eigen::MatrixXd::Zero(2, f.est.numShared());
    H(0, f.pb[0]) = 1.0;
    H(0, f.pb[1]) = 1.0;
    H(1, f.pb[1]) = 1.0;
    const Eigen::Vector2d z(0.05, -0.02);
    const std::vector<CK> kinds{CK::CbSumToZero, CK::CbIfPin};
    f.est.contributionRowKinds(&kinds);
    f.est.addSharedConstraintRows(H, z, 1.0e3);
    f.datum = {{f.clk[0], 0.0}};
    f.est.solve(f.datum);
}

NetworkRhsAttribDecomposition decompose(Fixture& f) {
    NetworkAttribFrame frame;
    SyncedEpoch se;
    se.time.week = 2313;
    se.time.tow = 259200.0;
    networkAttribCaptureFrame(frame, se, f.stations, f.est, f.datum, f.asm_res,
                              f.est_index_of, /*own_assemblies=*/false);
    EXPECT_TRUE(frame.valid);
    return networkRhsAttribDecompose(frame);
}

}  // namespace

TEST(NetworkRhsAttribClosureTest, ClassesSumToTheKernelWithAPrePdStationPrior) {
    Fixture f;
    solveFixture(f, /*fold_post_pd=*/true);
    const NetworkRhsAttribDecomposition d = decompose(f);
    ASSERT_TRUE(d.valid);
    EXPECT_LT(d.closure_rel, 1e-10) << "abs " << d.closure_abs;

    // Every channel has its class.
    for (const char* cls : {"obs_code", "obs_phase", "pivot_pin", "prior_rx_cb_datum_pre_pd",
                            "prior_ambiguity_carry", "shared:border_carry_diagonal",
                            "shared:cb_sum_to_zero", "shared:cb_if_pin"})
        EXPECT_EQ(d.class_total.count(cls), 1u) << cls;

    // The pre-PD channel carries a share of the reduced RHS, and without it the classes
    // would not close: the missed-channel defect.
    const Eigen::VectorXd& pre = d.class_total.at("prior_rx_cb_datum_pre_pd");
    EXPECT_GT(pre.norm(), 1e-6);
    NetworkAttribFrame frame;
    SyncedEpoch se;
    networkAttribCaptureFrame(frame, se, f.stations, f.est, f.datum, f.asm_res,
                              f.est_index_of, false);
    Eigen::VectorXd sum = Eigen::VectorXd::Zero(frame.b_red.size());
    for (const auto& [cls, v] : d.class_total)
        if (cls != "prior_rx_cb_datum_pre_pd") sum += v;
    EXPECT_GT((sum - frame.b_red).norm() / std::max(1.0, frame.b_red.norm()), 1e-6)
        << "leaving the pre-PD channel out must break the closure";

    // The per-station parts reduce to the class totals.
    Eigen::VectorXd parts = Eigen::VectorXd::Zero(frame.b_red.size());
    for (const auto& p : d.station_parts)
        if (p.cls == "prior_rx_cb_datum_pre_pd") parts += p.v;
    EXPECT_LT((parts - pre).norm(), 1e-12);
}

TEST(NetworkRhsAttribClosureTest, PostPdChannelCountsOnlyWhenFolded) {
    Fixture f;
    solveFixture(f, /*fold_post_pd=*/false);   // carry entries built, not folded
    const NetworkRhsAttribDecomposition d = decompose(f);
    ASSERT_TRUE(d.valid);
    EXPECT_LT(d.closure_rel, 1e-10) << "abs " << d.closure_abs;
    EXPECT_EQ(d.class_total.count("prior_ambiguity_carry"), 0u)
        << "an entry the consume did not fold is not in the kernel";
    EXPECT_EQ(d.class_total.count("prior_rx_cb_datum_pre_pd"), 1u);
}

TEST(NetworkRhsAttribClosureTest, SharedClassIsTheKindsName) {
    EXPECT_EQ(rhsAttribSharedClass(CK::BorderCarryDense), "shared:border_carry_dense");
    EXPECT_EQ(rhsAttribSharedClass(CK::CbIfPin), "shared:cb_if_pin");
    EXPECT_EQ(rhsAttribSharedClass(CK::Unset), "shared:unset");
    // A fold that named no kind is captured as Unset.
    NetworkEstimator est;
    est.registerClock("G01");
    est.attribArm(true);
    est.beginEpoch();
    NetworkEstimator::SharedPrior p;
    p.kind = SharedParamKind::Clock;
    p.sat = "G01";
    p.value = 1.0;
    p.info = 2.0;
    est.addSharedPrior({p});
    ASSERT_EQ(est.attribCaptures().size(), 1u);
    EXPECT_EQ(est.attribCaptures()[0].kind, CK::Unset);
    est.contributionKind(CK::ShPrior);
    est.addSharedPrior({p});
    ASSERT_EQ(est.attribCaptures().size(), 2u);
    EXPECT_EQ(est.attribCaptures()[1].kind, CK::ShPrior);
}

namespace {

/// The replay's captured priors: four kinds over three clocks of one
/// constellation and a phase bias, two kinds on the same clock column, so the gauge-free
/// downdate mixes the kinds within its group.
std::vector<NetworkEstimator::SharedPrior> mixedCapture() {
    auto prior = [](SharedParamKind kind, const char* sat, const char* sig, double v,
                    double info) {
        NetworkEstimator::SharedPrior p;
        p.kind = kind;
        p.sat = sat;
        p.signal = sig;
        p.value = v;
        p.info = info;
        return p;
    };
    return {prior(SharedParamKind::Clock, "G01", "", 0.3, 4.0),
            prior(SharedParamKind::Clock, "G02", "", -0.1, 2.0),
            prior(SharedParamKind::PhaseBias, "G01", "L1C", 0.05, 10.0),
            prior(SharedParamKind::Clock, "G03", "", 0.2, 1.0),
            prior(SharedParamKind::Clock, "G01", "", 0.1, 1.0)};
}

/// An armed estimator with the columns of @ref mixedCapture, its epoch open.
void openMixed(NetworkEstimator& est) {
    est.registerClock("G01");
    est.registerClock("G02");
    est.registerClock("G03");
    est.registerPhaseBias("G01", "L1C");
    est.attribArm(true);
    est.beginEpoch();
}

}  // namespace

TEST(NetworkRhsAttribClosureTest, ReplayPriorRefoldIsNamedPerKindAndFoldsTheSame) {
    const std::vector<NetworkEstimator::SharedPrior> priors = mixedCapture();
    std::vector<NetworkEstimator::SharedPrior> cap;
    std::vector<CK> cap_kinds;
    networkCapturePriors(cap, cap_kinds, {priors[0]}, CK::BorderCarryDiagonal);
    networkCapturePriors(cap, cap_kinds, {priors[1]}, CK::ClockOscillator);
    networkCapturePriors(cap, cap_kinds, {priors[2]}, CK::PinBridge);
    networkCapturePriors(cap, cap_kinds, {priors[3], priors[4]}, CK::ColdClockAnchor);
    ASSERT_EQ(cap.size(), 5u);
    ASSERT_EQ(cap_kinds.size(), cap.size()) << "the capture's kinds stay in step";

    for (const bool gauge_free : {false, true}) {
        SCOPED_TRACE(gauge_free ? "gauge-free" : "plain");
        auto fold = [&](NetworkEstimator& est) {
            if (gauge_free) est.addSharedPriorClockGaugeFree(cap);
            else            est.addSharedPrior(cap);
        };
        NetworkEstimator unnamed, named;
        openMixed(unnamed);
        openMixed(named);
        fold(unnamed);
        named.contributionPriorKinds(&cap_kinds);
        fold(named);

        // The fold is the same, bit for bit: the kinds change only the record.
        EXPECT_TRUE(named.normals().sharedNormals() == unnamed.normals().sharedNormals());
        EXPECT_TRUE(named.normals().sharedRhs() == unnamed.normals().sharedRhs());

        // One capture per kind, in the order a kind first appears, summing to the call's.
        ASSERT_EQ(unnamed.attribCaptures().size(), 1u);
        EXPECT_EQ(unnamed.attribCaptures()[0].kind, CK::Unset);
        const auto& caps = named.attribCaptures();
        ASSERT_EQ(caps.size(), 4u);
        EXPECT_EQ(caps[0].kind, CK::BorderCarryDiagonal);
        EXPECT_EQ(caps[1].kind, CK::ClockOscillator);
        EXPECT_EQ(caps[2].kind, CK::PinBridge);
        EXPECT_EQ(caps[3].kind, CK::ColdClockAnchor);
        Eigen::VectorXd sum = Eigen::VectorXd::Zero(named.numShared());
        for (const auto& c : caps) sum += c.delta_rhs;
        const Eigen::VectorXd& whole = unnamed.attribCaptures()[0].delta_rhs;
        EXPECT_LT((sum - whole).norm(), 1e-12 * std::max(1.0, whole.norm()));
        // Under the downdate a clock prior's part spreads over its whole group: the
        // oscillator prior on G02 moves the RHS of G01 and G03 as well.
        const Eigen::Index g01 = named.clockColumn("G01");
        const Eigen::Index g03 = named.clockColumn("G03");
        if (gauge_free) {
            EXPECT_NE(caps[1].delta_rhs(g01), 0.0);
            EXPECT_NE(caps[1].delta_rhs(g03), 0.0);
        } else {
            EXPECT_EQ(caps[1].delta_rhs(g01), 0.0);
            EXPECT_EQ(caps[1].delta_rhs(g03), 0.0);
        }

        // The fold log: one record per kind, each with its number of priors.
        const SharedFoldLog& log = named.sharedFoldLog();
        ASSERT_EQ(log.folds.size(), 4u);
        EXPECT_EQ(log.folds[3].kind, CK::ColdClockAnchor);
        EXPECT_EQ(log.folds[3].dim, 2);

        // The kinds are consumed by the call; a size mismatch is ignored (the call's kind).
        named.addSharedPrior({priors[0]});
        EXPECT_EQ(named.attribCaptures().back().kind, CK::Unset);
        const std::vector<CK> short_kinds{CK::PinBridge};
        named.contributionPriorKinds(&short_kinds);
        named.contributionKind(CK::ShPrior);
        fold(named);
        EXPECT_EQ(named.attribCaptures().back().kind, CK::ShPrior);
    }

    // In a replay the per-kind records count apart: one replay fold per kind.
    NetworkEstimator est;
    openMixed(est);
    est.contributionReplay(true);
    est.contributionPriorKinds(&cap_kinds);
    est.addSharedPrior(cap);
    est.contributionReplay(false);
    EXPECT_TRUE(est.sharedFoldLog().folds.empty());
    EXPECT_EQ(est.sharedFoldLog().replay_folds, 4);
    EXPECT_EQ(est.sharedFoldLog().replay_dim, 5);
}
