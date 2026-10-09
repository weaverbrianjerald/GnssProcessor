/**
 * @file test_ils_decorrelated_partial.cpp
 * @brief Unit tests for ils::ilsFixDecorrelatedPartial (card ARL).
 *
 * The motivating geometry is a free slant ionosphere on L1/L2: each single-signal
 * double difference carries an ionosphere-shaped error along
 * g = (1/λ1, γ/λ2) cycles per metre (γ = f1²/f2²), several tenths of a cycle per
 * band, while across that direction the floats are precise.  The integers are
 * then determined (the lattice repeats along g only every (77, 60) cycles), but
 * every single-signal float is far from its integer, so the entry-by-entry
 * search (ils::ilsFix) cannot accept them.  The decorrelated partial fix must.
 */

#include <gtest/gtest.h>

#include "filter/IlsCore.hpp"

#include <Eigen/Dense>

namespace GnssProcessor {
namespace {

constexpr double kC  = 299792458.0;
constexpr double kF1 = 1575.42e6;
constexpr double kF2 = 1227.60e6;

/// Two L1/L2 pairs (entries 0,1 and 2,3), independent of each other: covariance
/// sigma_iono_m² g gᵀ + sigma_cross_cyc² I per pair.
Eigen::MatrixXd pairCovariance(double sigma_iono_m, double sigma_cross_cyc) {
    const double l1 = kC / kF1, l2 = kC / kF2, gamma = (kF1 * kF1) / (kF2 * kF2);
    const Eigen::Vector2d g(1.0 / l1, gamma / l2);
    Eigen::MatrixXd Q = Eigen::MatrixXd::Zero(4, 4);
    for (int p = 0; p < 2; ++p)
        Q.block(2 * p, 2 * p, 2, 2) = sigma_iono_m * sigma_iono_m * g * g.transpose()
                                    + sigma_cross_cyc * sigma_cross_cyc * Eigen::Matrix2d::Identity();
    return Q;
}

/// Floats = integers + an ionosphere-shaped offset per pair + a small cross error.
Eigen::VectorXd pairFloats(const Eigen::VectorXd& N, double iono0_m, double iono1_m) {
    const double l1 = kC / kF1, l2 = kC / kF2, gamma = (kF1 * kF1) / (kF2 * kF2);
    Eigen::VectorXd a = N;
    a(0) += iono0_m / l1 + 0.002;  a(1) += iono0_m * gamma / l2 - 0.001;
    a(2) += iono1_m / l1 - 0.002;  a(3) += iono1_m * gamma / l2 + 0.002;
    return a;
}

ils::IlsGateConfig defaultGates() {
    ils::IlsGateConfig g;
    g.min_success_rate = 0.9999;
    g.ratio_threshold = 3.0;
    g.nl_max_per_pair_frac = 0.10;
    g.enable_partial_ar = true;
    return g;
}

TEST(IlsDecorrelatedPartialTest, RecoversSingleSignalIntegersUnderFreeIonosphere) {
    const Eigen::Vector4d N(-40.0, 5.0, 17.0, -3.0);
    // 5 cm of ionosphere uncertainty: about 0.26 / 0.34 cycle on L1 / L2.
    const Eigen::MatrixXd Q = pairCovariance(0.05, 0.005);
    const Eigen::VectorXd a = pairFloats(N, 0.04, -0.035);

    const ils::IlsResult r = ils::ilsFixDecorrelatedPartial(a, Q, defaultGates(), 0.05);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.z_total, 4);
    EXPECT_EQ(r.z_fixed, 4);
    ASSERT_EQ(r.kept.size(), 4u);
    for (std::size_t k = 0; k < r.kept.size(); ++k)
        EXPECT_DOUBLE_EQ(r.afix(static_cast<Eigen::Index>(k)), N(r.kept[k]));
    EXPECT_GE(r.ratio, 3.0);
    EXPECT_GE(r.p_succ, 0.9999);
}

TEST(IlsDecorrelatedPartialTest, EntryByEntrySearchRefusesTheSameProblem) {
    // The motivating contrast: the existing search with partial AR fixes nothing,
    // because every correct single-signal integer is more than 0.10 cycle from its float.
    const Eigen::Vector4d N(-40.0, 5.0, 17.0, -3.0);
    const ils::IlsResult r = ils::ilsFix(pairFloats(N, 0.04, -0.035), pairCovariance(0.05, 0.005),
                                         defaultGates());
    EXPECT_FALSE(r.ok);
}

TEST(IlsDecorrelatedPartialTest, ImpreciseFloatsFixNothing) {
    const Eigen::Vector4d N(-40.0, 5.0, 17.0, -3.0);
    const ils::IlsResult r = ils::ilsFixDecorrelatedPartial(
        pairFloats(N, 0.04, -0.035), pairCovariance(0.05, 0.3), defaultGates(), 0.05);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.z_fixed, 0);
    EXPECT_TRUE(r.kept.empty());
}

TEST(IlsDecorrelatedPartialTest, OnePreciseOneImprecisePairFixesOnlyThePrecisePair) {
    const Eigen::Vector4d N(-40.0, 5.0, 17.0, -3.0);
    Eigen::MatrixXd Q = pairCovariance(0.05, 0.005);
    Q.block(2, 2, 2, 2) = pairCovariance(0.05, 0.4).block(2, 2, 2, 2);
    const ils::IlsResult r = ils::ilsFixDecorrelatedPartial(pairFloats(N, 0.04, -0.035), Q,
                                                            defaultGates(), 0.05);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.kept.size(), 2u);
    EXPECT_EQ(r.kept[0], 0);
    EXPECT_EQ(r.kept[1], 1);
    EXPECT_DOUBLE_EQ(r.afix(0), N(0));
    EXPECT_DOUBLE_EQ(r.afix(1), N(1));
}

}  // namespace
}  // namespace GnssProcessor
