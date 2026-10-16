// Unit gate U0 of card PFQ (docs/refound/PFQ_pivot_frame_process_noise_2026-10-08.md):
// the carried satellite clocks' process noise in the pivot frame.
//
// The clock datum pins one satellite clock per constellation, so a carried clock is
// x_j = c_j - c_p and its state equation is the S-transform of the absolute one
// (Snow, Adjustment Computations, 15.9 with chapter 18: Q_S = S Q S').
//
//   (a) The helpers equal S Q S' of the absolute per-satellite increments, unequal
//       intervals included (the pivot's walk over the overlap of two clocks' windows).
//   (b) A pivot-frame Kalman recursion with the term reproduces the absolute-frame
//       recursion exactly.  Without it the stated variance of the constellation's common
//       direction is optimistic (the true error variance of that filter, propagated
//       deterministically, is 2.8 times the stated one here), while a contrast between two
//       non-pivot clocks is unaffected.
//   (c) The exact coupling update (card CTU) with the pairwise Q_S equals a brute-force
//       M'BM over the dense Q_S, for one row and for the cross term of two rows.

#include "network/carry/NetworkCouplingExact.hpp"
#include "network/solve/NetworkEstimator.hpp"
#include "network/carry/NetworkPivotFrameQ.hpp"

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <random>
#include <set>
#include <vector>

namespace GnssProcessor {
namespace {

/// S maps the absolute clocks c (K) to the pivot-frame clocks x (K - 1): x_j = c_j - c_p.
Eigen::MatrixXd pivotFrameMap(int K, int pivot) {
    Eigen::MatrixXd S = Eigen::MatrixXd::Zero(K - 1, K);
    int row = 0;
    for (int j = 0; j < K; ++j) {
        if (j == pivot) continue;
        S(row, j) = 1.0;
        S(row, pivot) = -1.0;
        ++row;
    }
    return S;
}

/// The pivot-frame Q of the non-pivot clocks from the helpers: own increment on the
/// diagonal plus the pivot's walk, the pivot's walk over the overlap off the diagonal.
Eigen::MatrixXd helperQ(const std::vector<double>& rate, const std::vector<double>& dt,
                        int pivot) {
    std::vector<int> idx;
    for (int j = 0; j < static_cast<int>(rate.size()); ++j)
        if (j != pivot) idx.push_back(j);
    const int n = static_cast<int>(idx.size());
    Eigen::MatrixXd Q(n, n);
    for (int a = 0; a < n; ++a)
        for (int b = 0; b < n; ++b) {
            const int ja = idx[static_cast<std::size_t>(a)], jb = idx[static_cast<std::size_t>(b)];
            Q(a, b) = (a == b)
                ? rate[static_cast<std::size_t>(ja)] * dt[static_cast<std::size_t>(ja)]
                      + pivotFrameDiag(rate[static_cast<std::size_t>(pivot)], dt[static_cast<std::size_t>(ja)])
                : pivotFrameCross(rate[static_cast<std::size_t>(pivot)],
                                  dt[static_cast<std::size_t>(ja)], dt[static_cast<std::size_t>(jb)]);
        }
    return Q;
}

}  // namespace

// ---------------------------------------------------------------------------
// (a) S Q S' with unequal intervals.  Every clock's window ends at the fold epoch t;
// split [t - max dt, t] at each window's start, give each satellite an independent
// increment on every sub-interval, and map them to the pivot-frame window increments.
// ---------------------------------------------------------------------------
TEST(PivotFrameProcessNoiseTest, HelpersEqualTheSTransformOfTheAbsoluteIncrements) {
    const int K = 4, pivot = 0;
    const std::vector<double> rate = {2.0e-8, 3.0e-8, 1.0e-8, 5.0e-8};   // m^2/s
    const std::vector<double> dt   = {30.0, 30.0, 90.0, 3600.0};         // s (the last a seed gap)

    std::set<double> cuts = {0.0};
    for (int j = 0; j < K; ++j) if (j != pivot) cuts.insert(dt[static_cast<std::size_t>(j)]);
    const std::vector<double> edge(cuts.begin(), cuts.end());   // ages before t, ascending
    const int nsub = static_cast<int>(edge.size()) - 1;         // sub-interval i = [edge[i], edge[i+1]] before t

    // Absolute increments: one per (satellite, sub-interval), independent.
    const int nabs = K * nsub;
    Eigen::VectorXd qsub(nabs);
    for (int s = 0; s < K; ++s)
        for (int i = 0; i < nsub; ++i)
            qsub(s * nsub + i) = rate[static_cast<std::size_t>(s)] * (edge[i + 1] - edge[i]);
    // L: pivot-frame window increments from the absolute sub-interval increments.
    Eigen::MatrixXd L = Eigen::MatrixXd::Zero(K - 1, nabs);
    int row = 0;
    for (int j = 0; j < K; ++j) {
        if (j == pivot) continue;
        for (int i = 0; i < nsub; ++i) {
            if (edge[i + 1] > dt[static_cast<std::size_t>(j)] + 1e-9) continue;   // outside j's window
            L(row, j * nsub + i)     += 1.0;
            L(row, pivot * nsub + i) -= 1.0;
        }
        ++row;
    }
    const Eigen::MatrixXd C = L * qsub.asDiagonal() * L.transpose();
    const Eigen::MatrixXd Q = helperQ(rate, dt, pivot);
    for (int a = 0; a < K - 1; ++a)
        for (int b = 0; b < K - 1; ++b)
            EXPECT_NEAR(Q(a, b), C(a, b), 1e-12 * C.cwiseAbs().maxCoeff())
                << "entry (" << a << "," << b << ")";
    // The pivot's walk is the whole off-diagonal: two clocks share r_p over the overlap.
    EXPECT_NEAR(C(0, 2), rate[0] * 30.0, 1e-18);
    EXPECT_NEAR(C(1, 2), rate[0] * 90.0, 1e-18);
}

// ---------------------------------------------------------------------------
// (b) The recursion.  Absolute frame: all K clocks walk, the observations are the
// pivot-frame differences.  Pivot frame: the K - 1 differences observed directly.  With
// Q_S from the helpers the two give the same estimates and covariances (the reparametrized
// estimable part of a linear Gaussian model).  Without the term, the filter's true error
// covariance, propagated with the true Q_S and the filter's own gain, exceeds its stated
// covariance along the common direction and not along a contrast of two non-pivot clocks.
// ---------------------------------------------------------------------------
TEST(PivotFrameProcessNoiseTest, PivotFrameRecursionReproducesTheAbsoluteFrame) {
    const int K = 5, pivot = 0, T = 500;
    const double dt = 30.0, sigma = 0.02;
    const std::vector<double> q = {1.0e-6, 0.8e-6, 1.2e-6, 1.0e-6, 0.9e-6};
    const std::vector<double> dts(static_cast<std::size_t>(K), dt);
    const Eigen::MatrixXd S = pivotFrameMap(K, pivot);
    const Eigen::MatrixXd Qa = Eigen::Map<const Eigen::VectorXd>(q.data(), K).asDiagonal() * dt;
    const Eigen::MatrixXd Qs = helperQ(q, dts, pivot);
    Eigen::MatrixXd Qs0 = Eigen::MatrixXd::Zero(K - 1, K - 1);    // the term omitted
    for (int a = 0; a < K - 1; ++a) Qs0(a, a) = q[static_cast<std::size_t>(a + 1)] * dt;
    ASSERT_LT((Qs - S * Qa * S.transpose()).cwiseAbs().maxCoeff(), 1e-18);

    const Eigen::MatrixXd R = Eigen::MatrixXd::Identity(K - 1, K - 1) * sigma * sigma;
    const Eigen::MatrixXd I = Eigen::MatrixXd::Identity(K - 1, K - 1);
    Eigen::VectorXd xa = Eigen::VectorXd::Zero(K);
    Eigen::MatrixXd Pa = Eigen::MatrixXd::Identity(K, K);
    Eigen::VectorXd xs = S * xa;
    Eigen::MatrixXd Ps = S * Pa * S.transpose();
    Eigen::MatrixXd Pst = Ps, Ptr = Ps;      // the term omitted: stated and true error covariance

    std::mt19937 gen(20261008u);
    std::normal_distribution<double> nrm(0.0, 1.0);
    Eigen::VectorXd c = Eigen::VectorXd::Zero(K);
    double max_dx = 0.0, max_dP = 0.0;
    for (int t = 0; t < T; ++t) {
        for (int j = 0; j < K; ++j) c(j) += std::sqrt(q[static_cast<std::size_t>(j)] * dt) * nrm(gen);
        Eigen::VectorXd y = S * c;
        for (int a = 0; a < K - 1; ++a) y(a) += sigma * nrm(gen);
        // Absolute frame.
        Pa += Qa;
        const Eigen::MatrixXd Ka = Pa * S.transpose() * (S * Pa * S.transpose() + R).inverse();
        xa += Ka * (y - S * xa);
        Pa = (Eigen::MatrixXd::Identity(K, K) - Ka * S) * Pa;
        // Pivot frame with the term.
        Ps += Qs;
        const Eigen::MatrixXd Ks = Ps * (Ps + R).inverse();
        xs += Ks * (y - xs);
        Ps = (I - Ks) * Ps;
        // Pivot frame without it: stated, and the true error covariance under the true Q_S.
        Pst += Qs0;
        const Eigen::MatrixXd K0 = Pst * (Pst + R).inverse();
        Pst = (I - K0) * Pst;
        Ptr = (I - K0) * (Ptr + Qs) * (I - K0).transpose() + K0 * R * K0.transpose();
        max_dx = std::max(max_dx, (S * xa - xs).cwiseAbs().maxCoeff());
        max_dP = std::max(max_dP, (S * Pa * S.transpose() - Ps).cwiseAbs().maxCoeff());
    }
    EXPECT_LT(max_dx, 1e-9);
    EXPECT_LT(max_dP, 1e-12);

    const Eigen::VectorXd u = Eigen::VectorXd::Constant(K - 1, 1.0 / (K - 1));   // common direction
    Eigen::VectorXd w = Eigen::VectorXd::Zero(K - 1);                            // a contrast
    w(0) = 1.0 / std::sqrt(2.0);
    w(1) = -1.0 / std::sqrt(2.0);
    const double ratio_common   = u.dot(Ptr * u) / u.dot(Pst * u);
    const double ratio_contrast = w.dot(Ptr * w) / w.dot(Pst * w);
    EXPECT_GT(ratio_common, 1.5) << "omitting the term must leave the common direction optimistic";
    EXPECT_NEAR(ratio_contrast, 1.0, 0.05) << "a contrast of two non-pivot clocks carries no pivot walk";
}

// ---------------------------------------------------------------------------
// (c) The exact update with the pairwise Q_S.  Four clocks of one constellation (the
// pivot is pinned outside P, as in the producer) with unequal intervals and two phase
// biases; one key departed.  The production rule against the brute force over K = S ∪ D.
// ---------------------------------------------------------------------------
TEST(PivotFrameProcessNoiseTest, ExactUpdateWithPairwiseQEqualsBruteForce) {
    const int np = 7;                 // P rows: 0-3 clocks, 4-5 phase biases, 6 departed clock
    std::mt19937 gen(7u);
    std::normal_distribution<double> nrm(0.0, 1.0);
    Eigen::MatrixXd A(np, np);
    for (int i = 0; i < np; ++i)
        for (int j = 0; j < np; ++j) A(i, j) = nrm(gen);
    Eigen::MatrixXd P = 1e-4 * (A * A.transpose()) + 1e-5 * Eigen::MatrixXd::Identity(np, np);

    const double rate_p = 2.9e-8;
    const std::vector<double> dt_s = {30.0, 30.0, 120.0, 600.0};
    CouplingExactEpoch ep;
    ep.P = P;
    ep.s_of_p.assign(np, -1);
    ep.route_p.assign(np, static_cast<int>(CouplingExactDRoute::Departed));
    ep.clk_p.assign(np, 0);
    ep.cc_p.assign(np, 'G');
    std::vector<double> qs;
    for (int p = 0; p < 6; ++p) {
        ep.s_of_p[static_cast<std::size_t>(p)] = p;
        ep.route_p[static_cast<std::size_t>(p)] = static_cast<int>(CouplingExactDRoute::Present);
        ep.p_of_s.push_back(p);
        ep.col_of_s.push_back(p);
        const bool clk = p < 4;
        ep.clk_p[static_cast<std::size_t>(p)] = clk ? 1 : 0;
        qs.push_back(clk ? 2.9e-8 * dt_s[static_cast<std::size_t>(p)]
                             + pivotFrameDiag(rate_p, dt_s[static_cast<std::size_t>(p)])
                         : 1e-12 * 30.0);
    }
    ep.clk_p[6] = 1;
    ep.q_s = Eigen::Map<const Eigen::VectorXd>(qs.data(), static_cast<Eigen::Index>(qs.size()));
    ep.shat_s = Eigen::VectorXd::Zero(6);
    for (int a = 0; a < 4; ++a)
        for (int b = 0; b < 4; ++b)
            if (a != b)
                ep.q_off.emplace_back(a, b, pivotFrameCross(rate_p, dt_s[static_cast<std::size_t>(a)],
                                                            dt_s[static_cast<std::size_t>(b)]));
    ASSERT_TRUE(couplingExactFactor(ep));

    // Dense Q_S and the brute force.
    Eigen::MatrixXd Qs = ep.q_s.asDiagonal();
    for (const auto& [a, b, v] : ep.q_off) Qs(a, b) += v;
    const Eigen::MatrixXd Pss = P.topLeftCorner(6, 6);
    const Eigen::MatrixXd F = (Pss + Qs).inverse();
    const Eigen::MatrixXd B = P - P.leftCols(6) * F * P.topRows(6);   // over K = all 7 rows

    const auto rowOf = [&](unsigned seed) {
        std::mt19937 g(seed);
        std::vector<std::pair<int, double>> terms;
        Eigen::VectorXd M = Eigen::VectorXd::Zero(np);
        for (int p = 0; p < np; ++p) {
            const double c = std::normal_distribution<double>(0.0, 1.0)(g);
            terms.emplace_back(p, c);
            M(p) = c;
        }
        return std::make_pair(terms, M);
    };
    const auto [ta, Ma] = rowOf(11u);
    const auto [tb, Mb] = rowOf(12u);
    const CouplingExactRow ra = couplingExactRow(ep, ta);
    const CouplingExactRow rb = couplingExactRow(ep, tb);
    ASSERT_TRUE(ra.ok);
    ASSERT_TRUE(rb.ok);
    const Eigen::VectorXd m_new = F * P.topRows(6) * Ma;
    for (int s = 0; s < 6; ++s)
        EXPECT_NEAR(ra.m_new(s), m_new(s), 1e-9 * std::max(1.0, m_new.cwiseAbs().maxCoeff()));
    const double var_bf = Ma.dot(B * Ma);
    EXPECT_NEAR(ra.var_raw, var_bf, 1e-9 * std::abs(var_bf) + 1e-18);
    const double cross_bf = Ma.dot(B * Mb);
    EXPECT_NEAR(couplingExactCross(ep, ra, rb), cross_bf, 1e-9 * std::abs(cross_bf) + 1e-18);
}

// ---------------------------------------------------------------------------
// (d) Section 6: on card SCL's clone, the stood-in clocks' walks share the pivot's walk,
// so they fold as one factor A x = mean with the walks' joint covariance Qw.  The factor
// equals the brute force A' Qw^-1 A (and A' Qw^-1 mean); sign -1 withdraws it exactly; a
// diagonal Qw folds what the per-walk form folds; a Qw that is not positive definite throws
// before folding anything.
// ---------------------------------------------------------------------------
TEST(PivotFrameProcessNoiseTest, JointCloneWalkFactorIsTheInformationForm) {
    NetworkEstimator est;
    const std::vector<std::string> sats = {"G01", "G02", "G03"};
    std::vector<Eigen::Index> c, k;
    for (const auto& s : sats) c.push_back(est.registerClock(s));
    for (const auto& s : sats) k.push_back(est.registerClockClone(s));
    est.beginEpoch();
    const auto& neq = est.normals();
    const Eigen::MatrixXd N0 = neq.sharedNormals();
    const Eigen::VectorXd u0 = neq.sharedRhs();
    const Eigen::Index n = N0.rows();

    std::vector<NetworkEstimator::CloneWalk> walks(3);
    const std::vector<double> dt = {30.0, 30.0, 90.0};
    const double rate_p = 5e-6;
    Eigen::MatrixXd Qw(3, 3);
    for (int a = 0; a < 3; ++a) {
        walks[static_cast<std::size_t>(a)].sat  = sats[static_cast<std::size_t>(a)];
        walks[static_cast<std::size_t>(a)].mean = 0.001 * (a + 1);
        walks[static_cast<std::size_t>(a)].q    = 5e-6 * dt[static_cast<std::size_t>(a)]
                                                  + pivotFrameDiag(rate_p, dt[static_cast<std::size_t>(a)]);
    }
    for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b)
            Qw(a, b) = (a == b) ? walks[static_cast<std::size_t>(a)].q
                                : pivotFrameCross(rate_p, dt[static_cast<std::size_t>(a)],
                                                  dt[static_cast<std::size_t>(b)]);
    ASSERT_EQ(est.addCloneWalkJoint(walks, Qw), 3u);

    Eigen::MatrixXd A = Eigen::MatrixXd::Zero(3, n);
    Eigen::VectorXd mean(3);
    for (int a = 0; a < 3; ++a) {
        A(a, c[static_cast<std::size_t>(a)]) = 1.0;
        A(a, k[static_cast<std::size_t>(a)]) = -1.0;
        mean(a) = walks[static_cast<std::size_t>(a)].mean;
    }
    const Eigen::MatrixXd W = Qw.inverse();
    const Eigen::MatrixXd dN_bf = A.transpose() * W * A;
    const Eigen::VectorXd du_bf = A.transpose() * W * mean;
    const Eigen::MatrixXd dN = neq.sharedNormals() - N0;
    const Eigen::VectorXd du = neq.sharedRhs() - u0;
    EXPECT_LT((dN - dN_bf).cwiseAbs().maxCoeff(), 1e-9 * dN_bf.cwiseAbs().maxCoeff());
    EXPECT_LT((du - du_bf).cwiseAbs().maxCoeff(), 1e-9 * du_bf.cwiseAbs().maxCoeff());

    ASSERT_EQ(est.addCloneWalkJoint(walks, Qw, -1.0), 3u);
    EXPECT_LT((neq.sharedNormals() - N0).cwiseAbs().maxCoeff(), 1e-9 * dN_bf.cwiseAbs().maxCoeff());
    EXPECT_LT((neq.sharedRhs() - u0).cwiseAbs().maxCoeff(), 1e-9 * du_bf.cwiseAbs().maxCoeff());

    // A diagonal Qw folds the per-walk information.
    Eigen::MatrixXd Qd = Qw.diagonal().asDiagonal();
    est.addCloneWalkJoint(walks, Qd);
    const Eigen::MatrixXd dJ = neq.sharedNormals() - N0;
    est.addCloneWalkJoint(walks, Qd, -1.0);
    est.addCloneWalk(walks);
    const Eigen::MatrixXd dP = neq.sharedNormals() - N0;
    EXPECT_LT((dJ - dP).cwiseAbs().maxCoeff(), 1e-9 * dP.cwiseAbs().maxCoeff());
    est.addCloneWalk(walks, -1.0);

    // Not positive definite: throws, nothing folded.
    Eigen::MatrixXd Qbad = Qw;
    Qbad(0, 1) = Qbad(1, 0) = 10.0 * std::sqrt(Qw(0, 0) * Qw(1, 1));
    const Eigen::MatrixXd Nb = neq.sharedNormals();
    EXPECT_THROW(est.addCloneWalkJoint(walks, Qbad), std::runtime_error);
    EXPECT_EQ((neq.sharedNormals() - Nb).cwiseAbs().maxCoeff(), 0.0);
}

}  // namespace GnssProcessor
