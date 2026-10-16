/**
 * @file test_amb_level_band_scope.cpp
 * @brief Dependency witnesses for the existing and proposed ambiguity-level
 *        observables.
 *
 * The band-level formulas below are test-only design probes, not a new monitor.
 * Production WL/IF helpers provide the independently checked existing basis.
 */
#include <gtest/gtest.h>
#include "realtime/AmbLevelCheck.hpp"
#include <array>
#include <cmath>

using GnssProcessor::AmbLevelCheck;

namespace {
constexpr double f1 = 1575.42e6, f2 = 1227.60e6;
constexpr double lambda = 299792458.0 / (f1 - f2);
constexpr double q = (f1 / f2) * (f1 / f2);
struct Rows { double p1, p2, c1, c2; };

Rows rows(double e1, double e2, double common = 0, double iono = 0) {
    return {common - iono + e1, common - q * iono + e2,
            common + iono, common + q * iono};
}

std::array<double, 2> paired(const Rows& r) {
    return {AmbLevelCheck::wlLevelMinusState(f1, f2, lambda, r.p1, r.p2, r.c1, r.c2),
            AmbLevelCheck::ifLevelMinusState(f1, f2, r.p1, r.p2, r.c1, r.c2)};
}

std::array<double, 2> bandLevels(const Rows& r) {
    // Each band level needs its own phase and both same-epoch codes.
    return {r.p1 - (q + 1) / (q - 1) * r.c1 + 2 / (q - 1) * r.c2,
            r.p2 - 2 * q / (q - 1) * r.c1 + (q + 1) / (q - 1) * r.c2};
}
} // namespace

TEST(AmbLevelBandScope, EitherAmbiguityChangesBothExistingChannels) {
    const auto first = paired(rows(1, 0));
    const auto second = paired(rows(0, 1));
    EXPECT_NEAR(first[0], f1 / 299792458.0, 1e-12);
    EXPECT_NEAR(second[0], -f2 / 299792458.0, 1e-12);
    EXPECT_NEAR(first[1], q / (q - 1), 1e-12);
    EXPECT_NEAR(second[1], -1 / (q - 1), 1e-12);
    for (double value : {first[0], first[1], second[0], second[1]})
        EXPECT_GT(std::abs(value), 1.0);
}

TEST(AmbLevelBandScope, MixingBandSupportsDestroysCommonAndIonosphereCancellation) {
    const auto old = rows(0, 0, 0, 0);
    const auto fresh = rows(0, 0, 17, 6);
    for (double value : paired(old)) EXPECT_NEAR(value, 0, 1e-12);
    for (double value : paired(fresh)) EXPECT_NEAR(value, 0, 1e-12);
    // Equivalently: independently average two constant, disjoint band windows.
    const auto mixed = paired({old.p1, fresh.p2, old.c1, fresh.c2});
    EXPECT_GT(std::abs(mixed[0]), 1.0);
    EXPECT_GT(std::abs(mixed[1]), 1.0);
}

TEST(AmbLevelBandScope, DualCodeBandLevelsCancelNuisancesAndSeparatePhaseDependencies) {
    const auto baseline = rows(0.25, -0.75, 17, 6);
    const auto level = bandLevels(baseline);
    EXPECT_NEAR(level[0], 0.25, 1e-12);
    EXPECT_NEAR(level[1], -0.75, 1e-12);
    auto changed = baseline;
    changed.p2 += 3;
    EXPECT_DOUBLE_EQ(bandLevels(changed)[0], level[0]);
    EXPECT_NEAR(bandLevels(changed)[1], level[1] + 3, 1e-12);
    changed = baseline;
    changed.p1 += 3;
    EXPECT_NEAR(bandLevels(changed)[0], level[0] + 3, 1e-12);
    EXPECT_DOUBLE_EQ(bandLevels(changed)[1], level[1]);
    changed = baseline;
    changed.c2 += 0.5;
    EXPECT_GT(std::abs(bandLevels(changed)[0] - level[0]), 1);
    EXPECT_GT(std::abs(bandLevels(changed)[1] - level[1]), 1);
}

TEST(AmbLevelBandScope, BandBasisPreservesSameEpochInformationButHasCorrelatedCodeNoise) {
    // Verify the basis identity on each independent input row, including
    // arbitrary nuisance/noise rows, rather than only ambiguity-only inputs.
    for (const auto& r : {Rows{1, 0, 0, 0}, Rows{0, 1, 0, 0},
                          Rows{0, 0, 1, 0}, Rows{0, 0, 0, 1}}) {
        const auto a = bandLevels(r);
        const auto v = paired(r);
        EXPECT_NEAR(v[0], (f1 * a[0] - f2 * a[1]) / 299792458.0, 1e-12);
        EXPECT_NEAR(v[1], (q * a[0] - a[1]) / (q - 1), 1e-12);
    }
    const double w11 = -(q + 1) / (q - 1), w12 = 2 / (q - 1);
    const double w21 = -2 * q / (q - 1), w22 = (q + 1) / (q - 1);
    const double var1 = 0.005 * 0.005 + 0.25 * (w11*w11 + w12*w12);
    const double var2 = 0.005 * 0.005 + 0.25 * (w21*w21 + w22*w22);
    const double cov = 0.25 * (w11*w21 + w12*w22);
    EXPECT_NEAR(std::sqrt(var1), 2.56404008, 1e-8);
    EXPECT_NEAR(std::sqrt(var2), 3.26584707, 1e-8);
    EXPECT_GT(cov / std::sqrt(var1 * var2), 0.9995);
}
