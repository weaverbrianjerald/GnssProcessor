/**
 * @file test_ar_ils_core.cpp
 * @brief Hand-worked cases for the integer least-squares core of user integer
 *        fixing (filter/IlsCore.hpp): the reduction, the search, the full
 *        MLAMBDA call and the bootstrap success rate.
 *
 * The user engine's fixes rest on these few free functions.  A regression in
 * the reduction or the search changes which integers are fixed, and one in the
 * bootstrap rate changes which fixes the acceptance gate lets through, without
 * any other visible symptom.  Every expected number below is worked out from
 * the arithmetic of the code (Q = Lᵀ·D·L eliminated from the last row up,
 * Gauss steps rounding L(i,j), the 1e-6 permutation margin, the Schnorr-Euchner
 * zig-zag from an initial search radius of 1e18, and
 * P = Π erf(1 / (2·√(2·d_i)))), and the correlated case is checked again by a
 * brute-force enumeration of the integer grid.
 *
 * The functions are public free functions in namespace ils and are called
 * directly; none of them logs.
 *
 * Suite name @c ArIlsCore is owned solely by this file.
 */

#include <gtest/gtest.h>

#include "filter/IlsCore.hpp"

#include <Eigen/Dense>

#include <cmath>
#include <limits>

namespace GnssProcessor {
namespace {

// Per-dimension bootstrap term, written out independently of the code under
// test: P(|N(0, d)| < 0.5) = erf(0.5 / √(2·d)).
double bootstrapTerm(double d) {
    return std::erf(0.5 / std::sqrt(2.0 * d));
}

// Default user gates, spelled out so that a change of the struct defaults does
// not silently change these cases.
ils::IlsGateConfig userGates() {
    ils::IlsGateConfig g;
    g.ratio_threshold      = 3.0;
    g.ratio_upper_bound    = 1e4;
    g.min_success_rate     = 0.999;
    g.nl_max_per_pair_frac = 0.15;
    g.enable_partial_ar    = false;
    return g;
}

// The correlated 2-D problem used by several cases: a₂ has variance 1 and
// a₁ − a₂ has variance 0.01, so Q = [[1.01, 1], [1, 1]].  The precise
// combination is a₁ − a₂, which only the reduction exposes.
Eigen::MatrixXd correlatedQ() {
    Eigen::MatrixXd Q(2, 2);
    Q << 1.01, 1.0,
         1.0,  1.0;
    return Q;
}

// ---------------------------------------------------------------------------
// One dimension
// ---------------------------------------------------------------------------

// In one dimension the reduction is the identity and the search returns the
// nearest integer, then the next nearest one on the other side, each with its
// squared distance divided by the variance.  A positive residual (2.3) steps
// up first, a negative one (-1.4) steps down first.
TEST(ArIlsCore, OneDimensionalSearchReturnsNearestThenNextInteger) {
    {
        Eigen::MatrixXd L(1, 1);
        L << 1.0;
        Eigen::VectorXd d(1);
        d << 0.04;
        Eigen::MatrixXd Z;
        ils::reduction(L, d, Z);
        ASSERT_EQ(Z.rows(), 1);
        EXPECT_DOUBLE_EQ(Z(0, 0), 1.0);
        EXPECT_DOUBLE_EQ(L(0, 0), 1.0);
        EXPECT_DOUBLE_EQ(d(0), 0.04);
    }

    Eigen::VectorXd a(1);
    a << 2.3;
    Eigen::MatrixXd Q(1, 1);
    Q << 0.04;
    Eigen::VectorXd afix, s, d_out;
    ASSERT_TRUE(ils::mlambda(a, Q, afix, s, &d_out));
    ASSERT_EQ(afix.size(), 1);
    EXPECT_DOUBLE_EQ(afix(0), 2.0);
    ASSERT_EQ(s.size(), 2);
    EXPECT_NEAR(s(0), 0.09 / 0.04, 1e-9);   // 0.3² / 0.04 = 2.25
    EXPECT_NEAR(s(1), 0.49 / 0.04, 1e-9);   // 0.7² / 0.04 = 12.25
    EXPECT_NEAR(s(1) / s(0), 49.0 / 9.0, 1e-9);
    ASSERT_EQ(d_out.size(), 1);
    EXPECT_NEAR(d_out(0), 0.04, 1e-15);

    Eigen::VectorXd b(1);
    b << -1.4;
    Eigen::MatrixXd Q1(1, 1);
    Q1 << 1.0;
    ASSERT_TRUE(ils::mlambda(b, Q1, afix, s, nullptr));
    EXPECT_DOUBLE_EQ(afix(0), -1.0);
    EXPECT_NEAR(s(0), 0.16, 1e-12);          // to -1
    EXPECT_NEAR(s(1), 0.36, 1e-12);          // to -2, not to 0 (1.96)
}

// ---------------------------------------------------------------------------
// Correlated two dimensions
// ---------------------------------------------------------------------------

// Worked by hand: the factorization gives d = (0.01, 1) and L(1,0) = 1.  The
// Gauss step rounds L(1,0) = 1 away (Z column 0 becomes (1, -1)); then
// d₀ + L(1,0)²·d₁ = 0.01 is below d₁ = 1, so columns 0 and 1 swap, giving
// d = (1, 0.01), L = I and Z = [[0, 1], [1, -1]], i.e. z = (a₂, a₁ − a₂).
// The precise combination ends last, where the search starts.
TEST(ArIlsCore, CorrelatedTwoDimReductionMatchesHandWork) {
    const Eigen::MatrixXd Q = correlatedQ();
    Eigen::MatrixXd L;
    Eigen::VectorXd d;
    ASSERT_TRUE(ils::ldlDecompose(Q, L, d));
    EXPECT_NEAR(d(0), 0.01, 1e-12);
    EXPECT_NEAR(d(1), 1.0, 1e-12);
    EXPECT_NEAR(L(1, 0), 1.0, 1e-12);

    Eigen::MatrixXd Z;
    ils::reduction(L, d, Z);

    EXPECT_NEAR(d(0), 1.0, 1e-12);
    EXPECT_NEAR(d(1), 0.01, 1e-12);
    EXPECT_NEAR(L(0, 0), 1.0, 1e-12);
    EXPECT_NEAR(L(1, 1), 1.0, 1e-12);
    EXPECT_NEAR(L(1, 0), 0.0, 1e-12);
    EXPECT_NEAR(L(0, 1), 0.0, 1e-12);

    EXPECT_DOUBLE_EQ(Z(0, 0), 0.0);
    EXPECT_DOUBLE_EQ(Z(0, 1), 1.0);
    EXPECT_DOUBLE_EQ(Z(1, 0), 1.0);
    EXPECT_DOUBLE_EQ(Z(1, 1), -1.0);

    // The reduced factors describe Zᵀ·Q·Z, which is diagonal here.
    const Eigen::MatrixXd ZtQZ = Z.transpose() * Q * Z;
    EXPECT_NEAR(ZtQZ(0, 0), 1.0, 1e-12);
    EXPECT_NEAR(ZtQZ(1, 1), 0.01, 1e-12);
    EXPECT_NEAR(ZtQZ(0, 1), 0.0, 1e-12);
}

// â = (2.45, 1.6), so z = (1.6, 0.85).  The search rounds z₁ = 0.85 to 1
// (0.15² / 0.01 = 2.25) and then z₀ = 1.6 to 2 (+0.16): best z = (2, 1), that
// is a = (3, 2), at 2.41.  The second is z = (1, 1), a = (2, 1), at 2.61.
// Rounding each float on its own would give (2, 2), at 72.41.
TEST(ArIlsCore, CorrelatedTwoDimSearchFindsTheHandWorkedLatticePoint) {
    const Eigen::MatrixXd Q = correlatedQ();
    Eigen::VectorXd a(2);
    a << 2.45, 1.6;

    Eigen::VectorXd afix, s, d_out;
    ASSERT_TRUE(ils::mlambda(a, Q, afix, s, &d_out));
    EXPECT_NEAR(afix(0), 3.0, 1e-12);
    EXPECT_NEAR(afix(1), 2.0, 1e-12);
    EXPECT_NEAR(s(0), 2.41, 1e-9);
    EXPECT_NEAR(s(1), 2.61, 1e-9);
    ASSERT_EQ(d_out.size(), 2);
    EXPECT_NEAR(d_out(0), 1.0, 1e-12);
    EXPECT_NEAR(d_out(1), 0.01, 1e-12);

    // The search itself, on the reduced problem, returns both candidates in z.
    Eigen::MatrixXd L;
    Eigen::VectorXd d;
    ASSERT_TRUE(ils::ldlDecompose(Q, L, d));
    Eigen::MatrixXd Z;
    ils::reduction(L, d, Z);
    const Eigen::VectorXd zs = Z.transpose() * a;
    EXPECT_NEAR(zs(0), 1.6, 1e-12);
    EXPECT_NEAR(zs(1), 0.85, 1e-12);
    Eigen::MatrixXd zn;
    Eigen::VectorXd sq;
    ASSERT_EQ(ils::search(L, d, zs, zn, sq, 2), 2);
    EXPECT_DOUBLE_EQ(zn(0, 0), 2.0);
    EXPECT_DOUBLE_EQ(zn(1, 0), 1.0);
    EXPECT_DOUBLE_EQ(zn(0, 1), 1.0);
    EXPECT_DOUBLE_EQ(zn(1, 1), 1.0);
    EXPECT_NEAR(sq(0), 2.41, 1e-9);
    EXPECT_NEAR(sq(1), 2.61, 1e-9);

    // Independent check: enumerate the integer grid around the float.
    const Eigen::MatrixXd Qi = Q.inverse();
    double best = std::numeric_limits<double>::infinity();
    double second = std::numeric_limits<double>::infinity();
    Eigen::Vector2d best_a(0.0, 0.0);
    for (int i = -5; i <= 10; ++i) {
        for (int j = -5; j <= 10; ++j) {
            Eigen::VectorXd r(2);
            r << a(0) - i, a(1) - j;
            const double q = r.dot(Qi * r);
            if (q < best) {
                second = best;
                best = q;
                best_a << static_cast<double>(i), static_cast<double>(j);
            }
            else if (q < second) { second = q; }
        }
    }
    EXPECT_DOUBLE_EQ(best_a(0), 3.0);
    EXPECT_DOUBLE_EQ(best_a(1), 2.0);
    EXPECT_NEAR(best, s(0), 1e-9);
    EXPECT_NEAR(second, s(1), 1e-9);

    // The componentwise rounding (2, 2) is far from the answer.
    Eigen::VectorXd r22(2);
    r22 << a(0) - 2.0, a(1) - 2.0;
    EXPECT_NEAR(r22.dot(Qi * r22), 72.41, 1e-6);
}

// ---------------------------------------------------------------------------
// A float that is exactly integer
// ---------------------------------------------------------------------------

// An exactly integer float is its own best candidate at distance zero; the
// second candidate moves the imprecise z₀ = a₂ by one, at distance 1.  The
// acceptance battery treats a zero best distance as invalid: ilsFix stays
// float with BadDistances and does not try the subset search, even with
// partial fixing on.
TEST(ArIlsCore, ExactlyIntegerFloatHasZeroBestDistanceAndIsNotAccepted) {
    const Eigen::MatrixXd Q = correlatedQ();
    Eigen::VectorXd a(2);
    a << 3.0, 2.0;

    Eigen::VectorXd afix, s;
    ASSERT_TRUE(ils::mlambda(a, Q, afix, s, nullptr));
    EXPECT_DOUBLE_EQ(afix(0), 3.0);
    EXPECT_DOUBLE_EQ(afix(1), 2.0);
    EXPECT_DOUBLE_EQ(s(0), 0.0);
    EXPECT_NEAR(s(1), 1.0, 1e-12);

    ils::IlsGateConfig g = userGates();
    g.enable_partial_ar = true;
    const ils::IlsResult r = ils::ilsFix(a, Q, g);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.partial);
    EXPECT_TRUE(r.kept.empty());
    EXPECT_EQ(r.full_fail, ils::IlsFailReason::BadDistances);
    EXPECT_DOUBLE_EQ(r.full_s1, 0.0);
    EXPECT_NEAR(r.full_s2, 1.0, 1e-12);
    EXPECT_DOUBLE_EQ(r.full_ratio, 0.0);   // never computed
}

// ---------------------------------------------------------------------------
// Fewer than two candidates
// ---------------------------------------------------------------------------

// The search accepts a candidate only below its radius, which starts at 1e18
// and shrinks only once two candidates are held.  With a variance of 1e-20 a
// float on an integer yields one candidate (the next costs 1 / 1e-20 = 1e20),
// and a float 0.3 away yields none (0.09 / 1e-20 = 9e18).  MLAMBDA then
// fails, and ilsFix reports MlambdaFailed.
TEST(ArIlsCore, FewerThanTwoCandidatesFailTheSearchAndTheFix) {
    Eigen::MatrixXd L(1, 1);
    L << 1.0;
    Eigen::VectorXd d(1);
    d << 1e-20;
    Eigen::MatrixXd zn;
    Eigen::VectorXd sq;

    Eigen::VectorXd zs(1);
    zs << 0.0;
    EXPECT_EQ(ils::search(L, d, zs, zn, sq, 2), 1);
    EXPECT_DOUBLE_EQ(zn(0, 0), 0.0);
    EXPECT_DOUBLE_EQ(sq(0), 0.0);

    zs << 0.3;
    EXPECT_EQ(ils::search(L, d, zs, zn, sq, 2), 0);

    // Asking for one candidate on an ordinary problem returns exactly one.
    Eigen::VectorXd d1(1);
    d1 << 1.0;
    EXPECT_EQ(ils::search(L, d1, zs, zn, sq, 1), 1);
    EXPECT_DOUBLE_EQ(zn(0, 0), 0.0);
    EXPECT_NEAR(sq(0), 0.09, 1e-12);

    Eigen::VectorXd a(1);
    a << 0.0;
    Eigen::MatrixXd Q(1, 1);
    Q << 1e-20;
    Eigen::VectorXd afix, s;
    EXPECT_FALSE(ils::mlambda(a, Q, afix, s, nullptr));

    ils::IlsGateConfig g = userGates();
    g.enable_partial_ar = true;
    const ils::IlsResult r = ils::ilsFix(a, Q, g);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.full_fail, ils::IlsFailReason::MlambdaFailed);
}

// ---------------------------------------------------------------------------
// Bootstrap success rate
// ---------------------------------------------------------------------------

// As a conditional variance goes to zero its term goes to one: a variance of
// 1e-12 gives exactly 1.0 in double precision, alone or three times over.  An
// exactly zero variance is refused by the declared guard (zero for any
// d_i <= 0); the factorization never produces a zero pivot, so the guard and
// the limit never meet on a real problem.
TEST(ArIlsCore, BootstrapTendsToOneAsAVarianceVanishes) {
    Eigen::VectorXd tiny(1);
    tiny << 1e-12;
    EXPECT_DOUBLE_EQ(ils::computeBootstrapSuccessRate(tiny), 1.0);

    Eigen::VectorXd tiny3 = Eigen::VectorXd::Constant(3, 1e-12);
    EXPECT_DOUBLE_EQ(ils::computeBootstrapSuccessRate(tiny3), 1.0);

    Eigen::VectorXd small(1);
    small << 1e-4;   // erf(35.36): one in double precision
    EXPECT_DOUBLE_EQ(ils::computeBootstrapSuccessRate(small), 1.0);

    Eigen::VectorXd zero(1);
    zero << 0.0;
    EXPECT_DOUBLE_EQ(ils::computeBootstrapSuccessRate(zero), 0.0);
}

// Growing one variance while the other stays at 0.01 lowers the rate at every
// step, and each value is the product of the two per-dimension terms.  Two
// points are pinned to the normal distribution: d = 0.04 is P(|N| < 2.5) and
// d = 1 is P(|N| < 0.5).
TEST(ArIlsCore, BootstrapFallsMonotonicallyAsOneVarianceGrows) {
    double prev = 1.0;
    for (double x = 0.01; x <= 100.0; x *= 1.5) {
        Eigen::VectorXd d(2);
        d << 0.01, x;
        const double p = ils::computeBootstrapSuccessRate(d);
        EXPECT_GT(p, 0.0) << "x = " << x;
        EXPECT_LT(p, prev) << "x = " << x;
        EXPECT_NEAR(p, bootstrapTerm(0.01) * bootstrapTerm(x), 1e-15) << "x = " << x;
        prev = p;
    }

    Eigen::VectorXd d04(1);
    d04 << 0.04;
    EXPECT_NEAR(ils::computeBootstrapSuccessRate(d04), 0.98758066934845, 1e-12);
    Eigen::VectorXd d1(1);
    d1 << 1.0;
    EXPECT_NEAR(ils::computeBootstrapSuccessRate(d1), 0.38292492254803, 1e-12);
}

// One loose decorrelated ambiguity sets the rate of the whole set: with two
// variances of 1e-4 (terms of one) and one of 4 (P(|N| < 0.25) = 0.19741),
// the rate is that single term, in any order, and never above the smallest
// term.
TEST(ArIlsCore, OneDominantVarianceSetsTheBootstrapRate) {
    Eigen::VectorXd d(3);
    d << 1e-4, 1e-4, 4.0;
    const double p = ils::computeBootstrapSuccessRate(d);
    EXPECT_NEAR(p, 0.19741265136585, 1e-12);

    Eigen::VectorXd alone(1);
    alone << 4.0;
    EXPECT_DOUBLE_EQ(p, ils::computeBootstrapSuccessRate(alone));

    Eigen::VectorXd reordered(3);
    reordered << 4.0, 1e-4, 1e-4;
    EXPECT_DOUBLE_EQ(p, ils::computeBootstrapSuccessRate(reordered));

    // With a second, milder loose entry the product falls below both terms.
    Eigen::VectorXd two(3);
    two << 1e-4, 1.0, 4.0;
    const double p2 = ils::computeBootstrapSuccessRate(two);
    EXPECT_LT(p2, bootstrapTerm(4.0));
    EXPECT_LT(p2, bootstrapTerm(1.0));
    EXPECT_NEAR(p2, bootstrapTerm(1.0) * bootstrapTerm(4.0), 1e-15);
}

}  // namespace
}  // namespace GnssProcessor
