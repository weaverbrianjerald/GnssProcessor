/**
 * @file test_ar_float_widelane.cpp
 * @brief Unit tests for the float widelane source and the N_1 search covariance
 *        floor of the WL-constrained resolver
 *        (@c estimation.ambiguity_resolution.wl_from_float,
 *        @c estimation.ambiguity_resolution.nl_sigma_floor_cyc).
 *
 * The references are exact algebra on synthetic states: a float built from known
 * integers must give back their widelane, and the variance of a linear form must
 * match g' P g written out by hand.
 *
 * Suite name @c ArFloatWidelaneTest is owned solely by this file.
 */

#include <gtest/gtest.h>

#include "filter/AmbiguityResolver.hpp"
#include "common/Constants.hpp"

#include <cmath>
#include <string>
#include <vector>

namespace GnssProcessor {

/// Test seam for the private static helpers (friend in AmbiguityResolver.hpp).
struct ArFloatWlTestAccess {
    using Pair = AmbiguityResolver::DdPair;
    using Fw   = AmbiguityResolver::FloatWidelane;
    static Fw wl(const Pair& p1, const Pair& p2, double l1, double l2,
                 const Eigen::VectorXd& x, const Eigen::MatrixXd& P) {
        return AmbiguityResolver::floatWidelane_(p1, p2, l1, l2, x, P);
    }
    static void floor(Eigen::MatrixXd& Q, const std::vector<std::string>& ref, double f) {
        AmbiguityResolver::addSharedReferenceFloor_(Q, ref, f);
    }
};

} // namespace GnssProcessor

using namespace GnssProcessor;

namespace {

constexpr double kF1 = 1575.42e6;
constexpr double kF2 = 1227.60e6;
double lam(double f) { return Constants::SPEED_OF_LIGHT / f; }

// State layout: 0 = sat_i L1, 1 = ref L1, 2 = sat_i L2, 3 = ref L2 (metres).
ArFloatWlTestAccess::Pair pairL1() { return { 0, 1, "G13", "G12", "L1C" }; }
ArFloatWlTestAccess::Pair pairL2() { return { 2, 3, "G13", "G12", "L2W" }; }

} // namespace

TEST(ArFloatWidelaneTest, RecoversTheWidelaneOfKnownIntegers) {
    const double l1 = lam(kF1), l2 = lam(kF2);
    // SD integers: sat_i (L1 17, L2 12), ref (L1 5, L2 2) → DD L1 12, DD L2 10, WL 2.
    Eigen::VectorXd x(4);
    x << 17 * l1, 5 * l1, 12 * l2, 2 * l2;
    const Eigen::MatrixXd P = Eigen::MatrixXd::Identity(4, 4) * 1e-6;
    const auto fw = ArFloatWlTestAccess::wl(pairL1(), pairL2(), l1, l2, x, P);
    EXPECT_NEAR(fw.w, 2.0, 1e-9);
}

TEST(ArFloatWidelaneTest, SigmaIsTheVarianceOfTheLinearForm) {
    const double l1 = lam(kF1), l2 = lam(kF2);
    Eigen::VectorXd x = Eigen::VectorXd::Zero(4);
    Eigen::MatrixXd P(4, 4);
    P << 4.0, 1.0, 0.5, 0.2,
         1.0, 3.0, 0.1, 0.4,
         0.5, 0.1, 5.0, 1.5,
         0.2, 0.4, 1.5, 2.0;
    P *= 1e-4;
    Eigen::Vector4d g(1.0 / l1, -1.0 / l1, -1.0 / l2, 1.0 / l2);
    const double expect = std::sqrt(g.dot(P * g));
    const auto fw = ArFloatWlTestAccess::wl(pairL1(), pairL2(), l1, l2, x, P);
    EXPECT_NEAR(fw.sigma, expect, 1e-12);
}

TEST(ArFloatWidelaneTest, OutOfRangeIndexGivesNaN) {
    const double l1 = lam(kF1), l2 = lam(kF2);
    Eigen::VectorXd x = Eigen::VectorXd::Zero(3);
    Eigen::MatrixXd P = Eigen::MatrixXd::Identity(3, 3);
    const auto fw = ArFloatWlTestAccess::wl(pairL1(), pairL2(), l1, l2, x, P);
    EXPECT_TRUE(std::isnan(fw.w));
    EXPECT_TRUE(std::isnan(fw.sigma));
}

TEST(ArFloatWidelaneTest, FloorAddsFSquaredOnTheDiagonalAndHalfOnSharedReferences) {
    Eigen::MatrixXd Q = Eigen::MatrixXd::Identity(3, 3) * 1e-4;
    const std::vector<std::string> ref{ "G12", "G12", "E05" };
    ArFloatWlTestAccess::floor(Q, ref, 0.04);
    const double f2 = 0.04 * 0.04;
    EXPECT_NEAR(Q(0, 0), 1e-4 + f2, 1e-15);
    EXPECT_NEAR(Q(2, 2), 1e-4 + f2, 1e-15);
    EXPECT_NEAR(Q(0, 1), 0.5 * f2, 1e-15);
    EXPECT_NEAR(Q(1, 0), 0.5 * f2, 1e-15);
    EXPECT_EQ(Q(0, 2), 0.0);   // different references stay uncorrelated
    EXPECT_EQ(Q(2, 1), 0.0);
}

TEST(ArFloatWidelaneTest, FlooredMatrixStaysPositiveDefinite) {
    Eigen::MatrixXd Q = Eigen::MatrixXd::Identity(4, 4) * 1e-8;
    ArFloatWlTestAccess::floor(Q, { "G12", "G12", "G12", "G12" }, 0.05);
    Eigen::LLT<Eigen::MatrixXd> llt(Q);
    EXPECT_EQ(llt.info(), Eigen::Success);
}
