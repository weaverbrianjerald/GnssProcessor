/**
 * @file test_ar_observe_search.cpp
 * @brief The observe-mode integer search on synthetic floats: the SD formation and its
 *        covariance, the grouping, the decorrelated branch against the producer's own call,
 *        the conditioned position on analytic cases, and the rows.
 *
 * The state is built by hand (a position block and one ambiguity state per candidate), so
 * every expectation is an explicit formula: the SD rows D x / lambda and D P D^T / lambda^2
 * with D written out, and the conditioning x_c = x - Q_xK Q_KK^-1 (a_K - afix_K) on 2 by 2
 * matrices inverted by hand.
 */

#include <gtest/gtest.h>

#include "realtime/ArObserve.hpp"

#include <Eigen/Dense>

#include <cmath>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

constexpr double kC  = 299792458.0;
constexpr double kF1 = 1575.42e6;
constexpr double kF2 = 1227.60e6;
constexpr double kL1 = kC / kF1;
constexpr double kL2 = kC / kF2;

/// The producer's own gate assignment (src/core/network/ar/ProducerAr.cpp), with the
/// consumer's defaults for the bars it reads from the AR settings.
ils::IlsGateConfig producerGates() {
    ils::IlsGateConfig g;
    g.min_success_rate     = 0.9999;
    g.ratio_threshold      = 3.0;
    g.ratio_upper_bound    = 1e4;
    g.nl_max_per_pair_frac = 0.10;
    g.enable_partial_ar    = true;
    return g;
}

/// A hand-built epoch: a three-state position block, then one ambiguity state per candidate.
struct Epoch {
    std::vector<ArCandidate> cands;
    Eigen::VectorXd x;
    Eigen::MatrixXd P;

    /// Add the candidate (sat, signal) at the next state index with the given float value
    /// in cycles and elevation.
    void add(const std::string& sat, const std::string& signal, double cycles, double elev,
             std::uint32_t cmp = 0, std::uint32_t gen = 0) {
        ArCandidate c;
        c.sat = sat;
        c.signal = signal;
        c.state_idx = 3 + static_cast<int>(cands.size());
        c.elevation_deg = elev;
        c.lambda_m = (signal[1] == '1') ? kL1 : kL2;
        c.cmp = cmp;
        c.hold_generation = gen;
        cands.push_back(c);
        values.push_back(cycles * c.lambda_m);
    }
    void finish(const Eigen::Vector3d& pos) {
        const int dim = 3 + static_cast<int>(cands.size());
        x = Eigen::VectorXd::Zero(dim);
        x.head<3>() = pos;
        for (std::size_t i = 0; i < cands.size(); ++i) x(3 + static_cast<int>(i)) = values[i];
        P = Eigen::MatrixXd::Zero(dim, dim);
        P.topLeftCorner(3, 3) = Eigen::Matrix3d::Identity();
    }
    std::vector<double> values;
};

/// A diagonally dominant (hence positive definite) covariance with correlations, for the
/// formation test.
Eigen::MatrixXd structuredCovariance(int dim) {
    Eigen::MatrixXd P(dim, dim);
    for (int i = 0; i < dim; ++i)
        for (int j = 0; j < dim; ++j)
            P(i, j) = (i == j) ? 0.02 + 0.003 * i : 0.004 / (1.0 + std::abs(i - j));
    return P;
}

// ---------------------------------------------------------------------------------------
// SD formation: D x / lambda and D P D^T / lambda^2
// ---------------------------------------------------------------------------------------

TEST(ArObserveSearchTest, SdFloatAndCovarianceAreDxOverLambdaAndDPDtOverLambdaSquared) {
    // Three satellites on L1C; G02 has the highest elevation and is the reference.
    Epoch e;
    e.add("G01", "L1C", 100.05, 30.0);
    e.add("G02", "L1C", -12.30, 60.0);
    e.add("G03", "L1C", 7.80, 45.0);
    e.finish({ 1000.0, 2000.0, 3000.0 });
    e.P = structuredCovariance(static_cast<int>(e.x.size()));
    ArObserveOptions opt;
    const ArSdProblem prob = formArSdProblem(e.x, e.P, e.cands, opt);
    ASSERT_EQ(prob.a.size(), 2);
    // SD order: satellite order within the group, the reference left out: G01-G02, G03-G02.
    ASSERT_EQ(prob.cand, (std::vector<int>{ 0, 2 }));
    ASSERT_EQ(prob.ref,  (std::vector<int>{ 1, 1 }));

    // D written out over the state vector (+1 on the satellite, -1 on the reference).
    Eigen::MatrixXd D = Eigen::MatrixXd::Zero(2, e.x.size());
    D(0, 3) = 1.0; D(0, 4) = -1.0;
    D(1, 5) = 1.0; D(1, 4) = -1.0;
    const Eigen::VectorXd a_expected = D * e.x / kL1;
    const Eigen::MatrixXd Q_expected = D * e.P * D.transpose() / (kL1 * kL1);
    EXPECT_NEAR(prob.a(0), 100.05 + 12.30, 1e-9);
    EXPECT_NEAR(prob.a(1), 7.80 + 12.30, 1e-9);
    EXPECT_LT((prob.a - a_expected).norm(), 1e-9);
    EXPECT_LT((prob.Q - Q_expected).norm(), 1e-9 * Q_expected.norm());

    // The same entry, by the closed form for a pair: (P_ii + P_rr - 2 P_ir) / lambda^2.
    const double q00 = (e.P(3, 3) + e.P(4, 4) - 2.0 * e.P(3, 4)) / (kL1 * kL1);
    EXPECT_NEAR(prob.Q(0, 0), q00, 1e-12 * q00);

    // The position cross-covariance, P_pos,. D^T / lambda.
    const Eigen::MatrixXd Pxs = e.P.topRows(3);
    const Eigen::MatrixXd Qxa_expected = Pxs * D.transpose() / kL1;
    EXPECT_LT((prob.Qxa - Qxa_expected).norm(), 1e-12);
}

TEST(ArObserveSearchTest, EachSignalUsesItsOwnWavelength) {
    Epoch e;
    e.add("G01", "L1C", 10.0, 70.0);
    e.add("G02", "L1C", 4.0, 30.0);
    e.add("G01", "L2W", 20.0, 70.0);
    e.add("G02", "L2W", 9.0, 30.0);
    e.finish({ 0.0, 0.0, 0.0 });
    e.P = structuredCovariance(static_cast<int>(e.x.size()));
    ArObserveOptions opt;
    const ArSdProblem prob = formArSdProblem(e.x, e.P, e.cands, opt);
    ASSERT_EQ(prob.a.size(), 2);
    EXPECT_NEAR(prob.a(0), 4.0 - 10.0, 1e-9);     // L1C: G02 - G01
    EXPECT_NEAR(prob.a(1), 9.0 - 20.0, 1e-9);     // L2W: G02 - G01
    // L2W entry: states 6 (G02) and 5 (G01) with lambda_2; L1C entry: states 4 and 3 with lambda_1.
    const double expected_l2 = (e.P(6, 6) + e.P(5, 5) - 2.0 * e.P(6, 5)) / (kL2 * kL2);
    const double expected_l1 = (e.P(4, 4) + e.P(3, 3) - 2.0 * e.P(4, 3)) / (kL1 * kL1);
    EXPECT_NEAR(prob.Q(1, 1), expected_l2, 1e-12 * expected_l2);
    EXPECT_NEAR(prob.Q(0, 0), expected_l1, 1e-12 * expected_l1);
    // The cross term between the two bands is divided by both wavelengths.
    const double cross = (e.P(4, 6) - e.P(4, 5) - e.P(3, 6) + e.P(3, 5)) / (kL1 * kL2);
    EXPECT_NEAR(prob.Q(0, 1), cross, 1e-12 * std::abs(cross));
}

// ---------------------------------------------------------------------------------------
// Grouping
// ---------------------------------------------------------------------------------------

TEST(ArObserveSearchTest, GroupsAreConstellationAndSignalAndTheReferenceIsTheHighestElevation) {
    Epoch e;
    e.add("G01", "L1C", 0.0, 20.0);
    e.add("G02", "L1C", 0.0, 55.0);
    e.add("G03", "L1C", 0.0, 40.0);
    e.add("E01", "L1C", 0.0, 35.0);
    e.add("E02", "L1C", 0.0, 50.0);
    e.add("G01", "L2W", 0.0, 20.0);
    e.add("G03", "L2W", 0.0, 40.0);
    e.finish({ 0.0, 0.0, 0.0 });
    e.P = structuredCovariance(static_cast<int>(e.x.size()));
    const ArSdProblem prob = formArSdProblem(e.x, e.P, e.cands, ArObserveOptions{});
    // Groups in letter order, then signal: E|L1C (ref E02), G|L1C (ref G02), G|L2W (ref G03).
    ASSERT_EQ(prob.a.size(), 4);
    EXPECT_EQ(prob.cand, (std::vector<int>{ 3, 0, 2, 5 }));
    EXPECT_EQ(prob.ref,  (std::vector<int>{ 4, 1, 1, 6 }));
}

TEST(ArObserveSearchTest, AGroupOfOneFormsNothing) {
    Epoch e;
    e.add("G01", "L1C", 0.0, 20.0);
    e.add("E01", "L1C", 0.0, 35.0);
    e.finish({ 0.0, 0.0, 0.0 });
    e.P = structuredCovariance(static_cast<int>(e.x.size()));
    EXPECT_EQ(formArSdProblem(e.x, e.P, e.cands, ArObserveOptions{}).a.size(), 0);
    const ArObserveEpoch out = observeIntegerSearch(e.x, e.P, e.cands, ArObserveOptions{});
    EXPECT_TRUE(out.valid);
    EXPECT_TRUE(out.rows.empty());
    EXPECT_EQ(out.summary.n_entries, 0);
    EXPECT_EQ(out.summary.fix_ecef, out.summary.float_ecef);
}

TEST(ArObserveSearchTest, ComponentsSplitTheGroupsOnlyWhenAsked) {
    Epoch e;
    e.add("G01", "L1C", 0.0, 80.0, /*cmp=*/1);
    e.add("G02", "L1C", 0.0, 60.0, /*cmp=*/1);
    e.add("G03", "L1C", 0.0, 70.0, /*cmp=*/2);
    e.add("G04", "L1C", 0.0, 40.0, /*cmp=*/2);
    e.finish({ 0.0, 0.0, 0.0 });
    e.P = structuredCovariance(static_cast<int>(e.x.size()));

    ArObserveOptions plain;
    const ArSdProblem one = formArSdProblem(e.x, e.P, e.cands, plain);
    ASSERT_EQ(one.a.size(), 3);                       // one group of four, reference G01
    for (int r : one.ref) EXPECT_EQ(r, 0);

    ArObserveOptions split;
    split.split_by_component = true;
    const ArSdProblem two = formArSdProblem(e.x, e.P, e.cands, split);
    ASSERT_EQ(two.a.size(), 2);                       // G02-G01 within 1, G04-G03 within 2
    EXPECT_EQ(two.cand, (std::vector<int>{ 1, 3 }));
    EXPECT_EQ(two.ref,  (std::vector<int>{ 0, 2 }));
}

TEST(ArObserveSearchTest, CandidatesWithoutAStateOrAWavelengthAreLeftOut) {
    Epoch e;
    e.add("G01", "L1C", 0.0, 80.0);
    e.add("G02", "L1C", 0.0, 60.0);
    e.add("G03", "L1C", 0.0, 50.0);
    e.finish({ 0.0, 0.0, 0.0 });
    e.P = structuredCovariance(static_cast<int>(e.x.size()));
    e.cands[1].state_idx = 99;        // beyond the state
    e.cands[2].lambda_m = 0.0;        // no wavelength
    EXPECT_EQ(formArSdProblem(e.x, e.P, e.cands, ArObserveOptions{}).a.size(), 0);
}

// ---------------------------------------------------------------------------------------
// The conditioned position, analytic
// ---------------------------------------------------------------------------------------

/// Two SDs on L1C against a reference of zero variance and zero value, with the covariance
/// and the position cross-covariance given in cycles (so the P entries are rescaled by lambda).
Epoch twoSdEpoch(double a0, double a1, double q00, double q01, double q11,
                 const Eigen::Matrix<double, 3, 2>& qxa, const Eigen::Vector3d& pos) {
    Epoch e;
    e.add("G01", "L1C", 0.0, 80.0);          // reference
    e.add("G02", "L1C", a0, 50.0);
    e.add("G03", "L1C", a1, 40.0);
    e.finish(pos);
    e.P(4, 4) = q00 * kL1 * kL1;
    e.P(4, 5) = e.P(5, 4) = q01 * kL1 * kL1;
    e.P(5, 5) = q11 * kL1 * kL1;
    for (int d = 0; d < 3; ++d) {
        e.P(d, 4) = e.P(4, d) = qxa(d, 0) * kL1;
        e.P(d, 5) = e.P(5, d) = qxa(d, 1) * kL1;
    }
    return e;
}

TEST(ArObserveSearchTest, FullFixConditionsThePositionOnTheKeptSet) {
    Eigen::Matrix<double, 3, 2> qxa;
    qxa << 0.002, -0.001,
           0.0005, 0.003,
          -0.004, 0.002;
    const Eigen::Vector3d pos(1000.0, 2000.0, 3000.0);
    const double q00 = 0.0004, q01 = 0.0001, q11 = 0.0009;       // sigma 0.02 and 0.03 cycle
    Epoch e = twoSdEpoch(100.05, -29.92, q00, q01, q11, qxa, pos);
    ArObserveOptions opt;
    opt.mode = ArSearchMode::Full;
    opt.gates = producerGates();
    const ArObserveEpoch out = observeIntegerSearch(e.x, e.P, e.cands, opt);
    ASSERT_TRUE(out.fix.ok);
    ASSERT_EQ(out.fix.kept.size(), 2u);
    EXPECT_DOUBLE_EQ(out.fix.afix(0), 100.0);
    EXPECT_DOUBLE_EQ(out.fix.afix(1), -30.0);

    // x_c = x - Q_xa Q_aa^-1 (a - N), the 2 by 2 inverse written out.
    const double det = q00 * q11 - q01 * q01;
    const double r0 = 0.05, r1 = 0.08;
    const double w0 = (q11 * r0 - q01 * r1) / det;
    const double w1 = (-q01 * r0 + q00 * r1) / det;
    for (int d = 0; d < 3; ++d) {
        const double expected = pos(d) - (qxa(d, 0) * w0 + qxa(d, 1) * w1);
        EXPECT_NEAR(out.summary.fix_ecef(d), expected, 1e-9) << "axis " << d;
        EXPECT_DOUBLE_EQ(out.summary.float_ecef(d), pos(d));
    }
    EXPECT_TRUE(out.summary.conditioned);
    EXPECT_TRUE(out.summary.accepted);
    EXPECT_EQ(out.summary.z_fixed, 2);
    EXPECT_EQ(out.summary.z_total, 2);

    // Both SDs are kept: determined, sigma 0, the integer the search chose (one band, no wide lane).
    ASSERT_EQ(out.rows.size(), 2u);
    EXPECT_EQ(out.rows[0].fixed_int, 100);
    EXPECT_EQ(out.rows[1].fixed_int, -30);
    for (const ArFixRow& r : out.rows) {
        EXPECT_TRUE(r.kept);
        EXPECT_TRUE(r.determined);
        EXPECT_DOUBLE_EQ(r.cond_sigma_cyc, 0.0);
    }
    EXPECT_EQ(out.summary.n_band_det, 2);
    EXPECT_EQ(out.summary.n_wl_det, 0);
}

TEST(ArObserveSearchTest, FullPartialFixConditionsTheLooseEntryOnTheKeptOne) {
    Eigen::Matrix<double, 3, 2> qxa;
    qxa << 0.002, -0.001,
           0.0005, 0.003,
          -0.004, 0.002;
    const Eigen::Vector3d pos(1000.0, 2000.0, 3000.0);
    const double q00 = 0.0004, q01 = 0.0001, q11 = 0.25;        // the second SD is loose
    Epoch e = twoSdEpoch(100.05, -29.92, q00, q01, q11, qxa, pos);
    ArObserveOptions opt;
    opt.mode = ArSearchMode::Full;
    opt.gates = producerGates();                                  // partial AR on
    const ArObserveEpoch out = observeIntegerSearch(e.x, e.P, e.cands, opt);
    ASSERT_TRUE(out.fix.ok);
    ASSERT_TRUE(out.fix.partial);
    ASSERT_EQ(out.fix.kept.size(), 1u);
    EXPECT_EQ(out.fix.kept[0], 0);

    // Kept entry 0: x_c = x - Q_x0 / Q_00 (a_0 - N_0); entry 1 conditioned on it.
    const double r0 = 0.05;
    for (int d = 0; d < 3; ++d)
        EXPECT_NEAR(out.summary.fix_ecef(d), pos(d) - qxa(d, 0) / q00 * r0, 1e-9);
    ASSERT_EQ(out.rows.size(), 2u);
    const ArFixRow& kept = out.rows[0];
    const ArFixRow& loose = out.rows[1];
    EXPECT_TRUE(kept.kept);
    EXPECT_TRUE(kept.determined);
    EXPECT_EQ(kept.fixed_int, 100);
    EXPECT_DOUBLE_EQ(kept.cond_sigma_cyc, 0.0);
    EXPECT_NEAR(kept.float_cyc, 100.05, 1e-9);
    EXPECT_FALSE(loose.kept);
    EXPECT_FALSE(loose.determined);                               // full search: determined = kept
    EXPECT_NEAR(loose.float_cyc, -29.92, 1e-9);
    EXPECT_EQ(loose.fixed_int, -30);                              // round(-29.92 - (Q_10 / Q_00) * 0.05)
    EXPECT_NEAR(loose.cond_sigma_cyc * loose.cond_sigma_cyc, q11 - q01 * q01 / q00, 1e-12);
    EXPECT_NEAR(loose.cond_sigma_cyc, std::sqrt(q11 - q01 * q01 / q00), 1e-9);
    EXPECT_EQ(out.summary.z_fixed, 1);
    EXPECT_EQ(out.summary.n_band_det, 1);
}

TEST(ArObserveSearchTest, DecorrelatedBranchGivesTheSamePositionOnTheSameProblem) {
    // One SD pair of two satellites against a reference: a single decorrelated component,
    // so the decorrelated conditioning equals the full conditioning exactly.
    Eigen::Matrix<double, 3, 2> qxa;
    qxa << 0.002, -0.001,
           0.0005, 0.003,
          -0.004, 0.002;
    const Eigen::Vector3d pos(1000.0, 2000.0, 3000.0);
    Epoch e = twoSdEpoch(100.05, -29.92, 0.0004, 0.0001, 0.0009, qxa, pos);
    ArObserveOptions full, deco;
    full.mode = ArSearchMode::Full;           full.gates = producerGates();
    deco.mode = ArSearchMode::DecorrelatedPartial;  deco.gates = producerGates();
    const ArObserveEpoch f = observeIntegerSearch(e.x, e.P, e.cands, full);
    const ArObserveEpoch d = observeIntegerSearch(e.x, e.P, e.cands, deco);
    ASSERT_TRUE(f.fix.ok);
    ASSERT_TRUE(d.fix.ok);
    EXPECT_EQ(d.summary.z_fixed, 2);
    for (int k = 0; k < 3; ++k) EXPECT_NEAR(d.summary.fix_ecef(k), f.summary.fix_ecef(k), 1e-8);
}

// ---------------------------------------------------------------------------------------
// The decorrelated branch against the producer's own call, free ionosphere
// ---------------------------------------------------------------------------------------

/// Two satellites G05 and G07 against G01 on L1C and L2W, with the covariance and floats
/// of the producer's decorrelated-partial test (an ionosphere-shaped error per pair, so each
/// single-signal float is far from its integer while the combinations are precise).
struct IonoEpoch {
    Epoch e;
    Eigen::Vector4d N;                 // syn order: pair A L1, pair A L2, pair B L1, pair B L2
    Eigen::MatrixXd Q_syn;
    Eigen::VectorXd a_syn;
};

IonoEpoch ionoEpoch(double sigma_iono_m, double sigma_cross_cyc) {
    IonoEpoch s;
    s.N << -40.0, 5.0, 17.0, -3.0;
    const double gamma = (kF1 * kF1) / (kF2 * kF2);
    const Eigen::Vector2d g(1.0 / kL1, gamma / kL2);
    s.Q_syn = Eigen::MatrixXd::Zero(4, 4);
    for (int p = 0; p < 2; ++p)
        s.Q_syn.block(2 * p, 2 * p, 2, 2) = sigma_iono_m * sigma_iono_m * g * g.transpose()
                                          + sigma_cross_cyc * sigma_cross_cyc * Eigen::Matrix2d::Identity();
    s.a_syn = s.N;
    const double iono0 = 0.04, iono1 = -0.035;
    s.a_syn(0) += iono0 / kL1 + 0.002;  s.a_syn(1) += iono0 * gamma / kL2 - 0.001;
    s.a_syn(2) += iono1 / kL1 - 0.002;  s.a_syn(3) += iono1 * gamma / kL2 + 0.002;

    // The SD order of the formation: L1C:G05, L1C:G07, L2W:G05, L2W:G07 = syn 0, 2, 1, 3.
    const int perm[4] = { 0, 2, 1, 3 };
    s.e.add("G01", "L1C", 0.0, 70.0);
    s.e.add("G01", "L2W", 0.0, 70.0);
    s.e.add("G05", "L1C", s.a_syn(perm[0]), 40.0);
    s.e.add("G05", "L2W", s.a_syn(perm[2]), 40.0);
    s.e.add("G07", "L1C", s.a_syn(perm[1]), 30.0);
    s.e.add("G07", "L2W", s.a_syn(perm[3]), 30.0);
    s.e.finish({ 4000000.0, 1000000.0, 4800000.0 });
    // States: 3 G01_L1C, 4 G01_L2W, 5 G05_L1C, 6 G05_L2W, 7 G07_L1C, 8 G07_L2W.
    const int state_of_sd[4] = { 5, 7, 6, 8 };
    const double lam_of_sd[4] = { kL1, kL1, kL2, kL2 };
    s.e.P(3, 3) = s.e.P(4, 4) = 1e-14;
    for (int k = 0; k < 4; ++k)
        for (int l = 0; l < 4; ++l)
            s.e.P(state_of_sd[k], state_of_sd[l]) =
                s.Q_syn(perm[k], perm[l]) * lam_of_sd[k] * lam_of_sd[l];
    // A weak position correlation with the ambiguities so the conditioning moves the position.
    for (int k = 0; k < 4; ++k) {
        const double c = 1.0e-4 * (k + 1) * lam_of_sd[k];
        s.e.P(0, state_of_sd[k]) = s.e.P(state_of_sd[k], 0) = c;
        s.e.P(1, state_of_sd[k]) = s.e.P(state_of_sd[k], 1) = -0.5 * c;
        s.e.P(2, state_of_sd[k]) = s.e.P(state_of_sd[k], 2) = 0.25 * c;
    }
    return s;
}

TEST(ArObserveSearchTest, DecorrelatedBranchEqualsTheProducersCallOnTheSameQ) {
    IonoEpoch s = ionoEpoch(0.05, 0.005);
    ArObserveOptions opt;
    opt.mode = ArSearchMode::DecorrelatedPartial;
    opt.gates = producerGates();
    opt.determined_sigma_cyc = 0.05;
    const ArSdProblem prob = formArSdProblem(s.e.x, s.e.P, s.e.cands, opt);
    ASSERT_EQ(prob.a.size(), 4);
    // The formed problem is the synthetic one, permuted into SD order.
    const int perm[4] = { 0, 2, 1, 3 };
    for (int k = 0; k < 4; ++k) {
        EXPECT_NEAR(prob.a(k), s.a_syn(perm[k]), 1e-6);
        for (int l = 0; l < 4; ++l)
            EXPECT_NEAR(prob.Q(k, l), s.Q_syn(perm[k], perm[l]), 1e-7 * (1.0 + std::abs(s.Q_syn(perm[k], perm[l]))));
    }

    // The producer's call on the very same (a, Q): the module's IlsResult must be identical.
    const ils::IlsResult producer =
        ils::ilsFixDecorrelatedPartial(prob.a, prob.Q, producerGates(), 0.05);
    const ArObserveEpoch out = observeIntegerSearch(s.e.x, s.e.P, s.e.cands, opt);
    ASSERT_TRUE(producer.ok);
    EXPECT_EQ(out.fix.ok, producer.ok);
    EXPECT_EQ(out.fix.kept, producer.kept);
    EXPECT_EQ(out.fix.z_fixed, producer.z_fixed);
    EXPECT_EQ(out.fix.z_total, producer.z_total);
    EXPECT_EQ(out.fix.partial, producer.partial);
    EXPECT_DOUBLE_EQ(out.fix.ratio, producer.ratio);
    EXPECT_DOUBLE_EQ(out.fix.p_succ, producer.p_succ);
    ASSERT_EQ(out.fix.afix.size(), producer.afix.size());
    for (Eigen::Index i = 0; i < producer.afix.size(); ++i)
        EXPECT_DOUBLE_EQ(out.fix.afix(i), producer.afix(i));
    ASSERT_EQ(out.fix.a_cond.size(), producer.a_cond.size());
    EXPECT_EQ((out.fix.a_cond - producer.a_cond).norm(), 0.0);
    EXPECT_EQ((out.fix.Q_cond - producer.Q_cond).norm(), 0.0);

    // What it fixed: the four single-signal integers, in SD order.
    ASSERT_EQ(out.fix.kept.size(), 4u);
    for (std::size_t k = 0; k < 4; ++k)
        EXPECT_DOUBLE_EQ(out.fix.afix(static_cast<Eigen::Index>(k)),
                         s.N(perm[out.fix.kept[k]]));
    EXPECT_EQ(out.summary.z_fixed, 4);
    EXPECT_EQ(out.summary.z_total, 4);
    EXPECT_GE(out.summary.ratio, 3.0);
    EXPECT_GE(out.summary.p_s, 0.9999);

    // The conditioned position is x + Q_xa Q^-1 (a_cond - a_hat), here a_cond = N.
    Eigen::VectorXd Nsd(4);
    for (int k = 0; k < 4; ++k) Nsd(k) = s.N(perm[k]);
    const Eigen::VectorXd w = prob.Q.ldlt().solve(Nsd - prob.a);
    const Eigen::Vector3d expected = s.e.x.head<3>() + prob.Qxa * w;
    EXPECT_LT((out.summary.fix_ecef - expected).norm(), 1e-6);
    EXPECT_GT((out.summary.fix_ecef - out.summary.float_ecef).norm(), 0.0);
    EXPECT_TRUE(out.summary.conditioned);
}

TEST(ArObserveSearchTest, FullSearchRefusesWhatTheDecorrelatedSearchFixes) {
    IonoEpoch s = ionoEpoch(0.05, 0.005);
    ArObserveOptions full;
    full.mode = ArSearchMode::Full;
    full.gates = producerGates();
    const ArObserveEpoch f = observeIntegerSearch(s.e.x, s.e.P, s.e.cands, full);
    EXPECT_FALSE(f.fix.ok);
    EXPECT_FALSE(f.summary.accepted);
    EXPECT_FALSE(f.summary.conditioned);
    EXPECT_EQ(f.summary.fix_ecef, f.summary.float_ecef);          // nothing fixed: the float
    EXPECT_EQ(f.summary.n_band_det, 0);
    for (const ArFixRow& r : f.rows) {
        EXPECT_FALSE(r.kept);
        EXPECT_FALSE(r.determined);       // the wide lane's float sigma, about 0.075 cycle, is above the bar
    }
    // The full search on the same problem: the float sigma is the formed Q's diagonal.
    const ArSdProblem prob = formArSdProblem(s.e.x, s.e.P, s.e.cands, full);
    for (int i = 0; i < 4; ++i)
        EXPECT_NEAR(f.rows[static_cast<std::size_t>(i)].cond_sigma_cyc, std::sqrt(prob.Q(i, i)), 1e-12);
    EXPECT_GT(f.summary.ratio, 0.0);        // the attempt's ratio is reported even when rejected
}

TEST(ArObserveSearchTest, WideLaneRowsAreTheDifferenceOfTheTwoLowestBands) {
    IonoEpoch s = ionoEpoch(0.05, 0.005);
    ArObserveOptions opt;
    opt.mode = ArSearchMode::DecorrelatedPartial;
    opt.gates = producerGates();
    const ArObserveEpoch out = observeIntegerSearch(s.e.x, s.e.P, s.e.cands, opt);
    ASSERT_EQ(out.rows.size(), 6u);                    // four band rows, two wide lanes
    int n_wl = 0;
    for (const ArFixRow& r : out.rows) {
        if (r.signal != "WL") continue;
        ++n_wl;
        EXPECT_EQ(r.system, 'G');
        EXPECT_EQ(r.ref_sat, "G01");
        ASSERT_TRUE(r.sat == "G05" || r.sat == "G07");
        // N_L1 - N_L2 of the pair: G05 (-40) - 5 = -45, G07 17 - (-3) = 20.
        EXPECT_EQ(r.fixed_int, r.sat == "G05" ? -45 : 20);
        EXPECT_TRUE(r.determined);
        EXPECT_TRUE(r.kept);
        EXPECT_LE(r.cond_sigma_cyc, 0.05);
        // The float wide lane is the difference of the two float bands.
        EXPECT_NEAR(r.float_cyc, (r.sat == "G05" ? s.a_syn(0) - s.a_syn(1) : s.a_syn(2) - s.a_syn(3)), 1e-6);
    }
    EXPECT_EQ(n_wl, 2);
    EXPECT_EQ(out.summary.n_pairs, 2);
    EXPECT_EQ(out.summary.n_band_det, 4);
    EXPECT_EQ(out.summary.n_wl_det, 2);

    // Band rows: the integers are the producer's, every row kept and determined.
    for (const ArFixRow& r : out.rows) {
        if (r.signal == "WL") continue;
        EXPECT_TRUE(r.kept);
        EXPECT_TRUE(r.determined);
        const int syn = (r.sat == "G05" ? 0 : 2) + (r.signal == "L1C" ? 0 : 1);
        EXPECT_EQ(r.fixed_int, static_cast<long long>(s.N(syn)));
    }
}

TEST(ArObserveSearchTest, FullKeptPairGivesAnIntegerWideLaneWithZeroSigma) {
    // A tight problem the full search accepts: both bands of one pair kept.
    Epoch e;
    e.add("G01", "L1C", 0.0, 80.0);
    e.add("G01", "L2W", 0.0, 80.0);
    e.add("G02", "L1C", 50.03, 40.0);
    e.add("G02", "L2W", -8.97, 40.0);
    e.finish({ 0.0, 0.0, 0.0 });
    e.P(3, 3) = e.P(4, 4) = 1e-14;
    e.P(5, 5) = 0.02 * 0.02 * kL1 * kL1;
    e.P(6, 6) = 0.02 * 0.02 * kL2 * kL2;
    ArObserveOptions opt;
    opt.mode = ArSearchMode::Full;
    opt.gates = producerGates();
    const ArObserveEpoch out = observeIntegerSearch(e.x, e.P, e.cands, opt);
    ASSERT_TRUE(out.fix.ok);
    ASSERT_EQ(out.rows.size(), 3u);
    const ArFixRow& wl = out.rows[2];
    EXPECT_EQ(wl.signal, "WL");
    EXPECT_EQ(wl.fixed_int, 59);                       // 50 - (-9)
    EXPECT_DOUBLE_EQ(wl.cond_sigma_cyc, 0.0);
    EXPECT_TRUE(wl.determined);
    EXPECT_TRUE(wl.kept);
    EXPECT_NEAR(wl.float_cyc, 50.03 + 8.97, 1e-9);
    EXPECT_EQ(out.summary.n_wl_det, 1);
    EXPECT_EQ(out.summary.n_band_det, 2);
}

TEST(ArObserveSearchTest, HoldGenerationsAndComponentsReachTheRows) {
    Epoch e;
    e.add("G01", "L1C", 0.0, 80.0, /*cmp=*/3, /*gen=*/7);
    e.add("G02", "L1C", 10.02, 40.0, /*cmp=*/3, /*gen=*/9);
    e.finish({ 0.0, 0.0, 0.0 });
    e.P(3, 3) = 1e-14;
    e.P(4, 4) = 0.02 * 0.02 * kL1 * kL1;
    ArObserveOptions opt;
    opt.gates = producerGates();
    opt.split_by_component = true;
    const ArObserveEpoch out = observeIntegerSearch(e.x, e.P, e.cands, opt);
    ASSERT_EQ(out.rows.size(), 1u);
    EXPECT_EQ(out.rows[0].sat, "G02");
    EXPECT_EQ(out.rows[0].ref_sat, "G01");
    EXPECT_EQ(out.rows[0].cmp, 3u);
    EXPECT_EQ(out.rows[0].gen_sat, 9u);
    EXPECT_EQ(out.rows[0].gen_ref, 7u);
}

TEST(ArObserveSearchTest, AnUnconvergedPositionIsNotSearched) {
    // A search of a float whose position is still wide can run for an unbounded time, so
    // above the bar the epoch keeps its float position and forms no rows.
    IonoEpoch s = ionoEpoch(0.05, 0.005);
    for (int d = 0; d < 3; ++d) s.e.P(d, d) = 0.25 * 0.25 / 3.0;      // 3D sigma 0.25 m
    // The position cross-covariance stays, so the covariance is the same in every other respect.
    ArObserveOptions opt;
    opt.mode = ArSearchMode::DecorrelatedPartial;
    opt.gates = producerGates();
    opt.max_position_sigma_m = 0.20;
    const ArObserveEpoch gated = observeIntegerSearch(s.e.x, s.e.P, s.e.cands, opt);
    EXPECT_TRUE(gated.valid);
    EXPECT_TRUE(gated.summary.gated);
    EXPECT_EQ(gated.summary.n_entries, 4);
    EXPECT_EQ(gated.summary.n_pairs, 0);
    EXPECT_TRUE(gated.rows.empty());
    EXPECT_FALSE(gated.summary.accepted);
    EXPECT_EQ(gated.summary.fix_ecef, gated.summary.float_ecef);
    EXPECT_EQ(gated.fix.z_total, 0) << "no integer search was run";

    opt.max_position_sigma_m = 0.30;                                   // the bar above the sigma
    const ArObserveEpoch open = observeIntegerSearch(s.e.x, s.e.P, s.e.cands, opt);
    EXPECT_FALSE(open.summary.gated);
    EXPECT_EQ(open.rows.size(), 6u);

    opt.max_position_sigma_m = 0.0;                                    // gate off
    EXPECT_FALSE(observeIntegerSearch(s.e.x, s.e.P, s.e.cands, opt).summary.gated);

    ArObserveCounters c;
    accumulateArObserve(c, gated, 6, 6);
    accumulateArObserve(c, open, 6, 6);
    EXPECT_EQ(c.epochs, 2);
    EXPECT_EQ(c.epochs_gated, 1);
    EXPECT_EQ(c.epochs_searched, 1);
    EXPECT_EQ(c.sd_pairs, 2);
    EXPECT_TRUE(c.closureHolds());
}

TEST(ArObserveSearchTest, SearchNamesRoundTrip) {
    EXPECT_EQ(arSearchModeOf("full"), ArSearchMode::Full);
    EXPECT_EQ(arSearchModeOf("decorrelated_partial"), ArSearchMode::DecorrelatedPartial);
    EXPECT_STREQ(arSearchModeName(ArSearchMode::Full), "full");
    EXPECT_STREQ(arSearchModeName(ArSearchMode::DecorrelatedPartial), "decorrelated_partial");
}

TEST(ArObserveSearchTest, CountersAccumulateAndClose) {
    IonoEpoch s = ionoEpoch(0.05, 0.005);
    ArObserveOptions opt;
    opt.mode = ArSearchMode::DecorrelatedPartial;
    opt.gates = producerGates();
    const ArObserveEpoch fixed = observeIntegerSearch(s.e.x, s.e.P, s.e.cands, opt);
    ArObserveOptions refuse = opt;
    refuse.mode = ArSearchMode::Full;
    const ArObserveEpoch refused = observeIntegerSearch(s.e.x, s.e.P, s.e.cands, refuse);

    ArObserveCounters c;
    accumulateArObserve(c, fixed, /*candidates=*/6, /*eligible=*/6);
    accumulateArObserve(c, refused, /*candidates=*/8, /*eligible=*/6);
    ArObserveEpoch skipped;                              // not valid: counted nowhere
    accumulateArObserve(c, skipped, 99, 99);
    EXPECT_EQ(c.epochs, 2);
    EXPECT_EQ(c.epochs_searched, 2);
    EXPECT_EQ(c.epochs_accepted, 1);
    EXPECT_EQ(c.candidates, 14);
    EXPECT_EQ(c.eligible, 12);
    EXPECT_EQ(c.gated_out, 2);
    EXPECT_EQ(c.sd_pairs, 4);
    EXPECT_EQ(c.band_rows, 8);
    EXPECT_EQ(c.wl_rows, 4);
    EXPECT_EQ(c.band_kept, 4);
    EXPECT_EQ(c.wl_kept, 2);
    EXPECT_EQ(c.kept, 6);
    EXPECT_TRUE(c.closureHolds());
    c.eligible += 1;                                     // a candidate counted twice breaks it
    EXPECT_FALSE(c.closureHolds());
}

} // namespace
