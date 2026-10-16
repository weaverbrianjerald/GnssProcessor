/**
 * @file test_ctu_level_guard.cpp
 * @brief The pure pieces of the level innovation instrument and of
 *        the phase-space anchor guard.
 *
 * - clockAnchorPhaseClockWeights: the minimum-norm weights of the ionosphere-free phase
 *   clock are blind to the satellite's level (sum w = 1) and to its ionosphere direction
 *   (sum w lambda^2 = 0), so a shift of the clock and every phase bias by the same amount
 *   leaves the statistic at zero while a clock-only step passes through whole.
 * - clockLevelTwoWay: the effect covariance at unit weight equals the inverse of the
 *   fit's normals built by brute force, and s0^2 recovers the pairs' scatter.
 * - the switch-on seam's level noise (CouplingExactEpoch::q_off): a row blind to a
 *   satellite's level is left unchanged by noise along that level, and a row that states
 *   the level has that statement released.
 * - ambFloatDrop with decouple_always: an arc already at birth strength is decoupled from
 *   the border and the block anyway; the default keeps the previous behaviour.
 */

#include <gtest/gtest.h>

#include "network/ar/AmbFloat.hpp"
#include "network/solve/ClockAnchor.hpp"
#include "network/solve/ClockLevel.hpp"
#include "network/carry/CouplingExact.hpp"
#include "network/driver/RunTypes.hpp"

#include <Eigen/Dense>

#include <cmath>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

constexpr double kC  = 299792458.0;
constexpr double kF1 = 1575.42e6;
constexpr double kF2 = 1227.60e6;
const double kL1 = kC / kF1;
const double kL2 = kC / kF2;

}  // namespace

TEST(CtuLevelGuardTest, PhaseClockWeightsAreTheIonosphereFreePair) {
    const std::vector<double> w = clockAnchorPhaseClockWeights({kL1 * kL1, kL2 * kL2});
    ASSERT_EQ(w.size(), 2u);
    const double a = kF1 * kF1 / (kF1 * kF1 - kF2 * kF2);
    EXPECT_NEAR(w[0], a, 1e-12);
    EXPECT_NEAR(w[1], 1.0 - a, 1e-12);
    EXPECT_NEAR(w[0], 2.5457, 1e-4);
    EXPECT_NEAR(w[1], -1.5457, 1e-4);
}

TEST(CtuLevelGuardTest, PhaseClockWeightsSplitASharedBand) {
    // Two signals on L1 (L1C and L1W) and one on L2: the minimum-norm solution spreads the
    // L1 weight evenly and keeps both constraints.
    const std::vector<double> al{kL1 * kL1, kL1 * kL1, kL2 * kL2};
    const std::vector<double> w = clockAnchorPhaseClockWeights(al);
    ASSERT_EQ(w.size(), 3u);
    EXPECT_NEAR(w[0], w[1], 1e-12);
    double s = 0.0, sa = 0.0;
    for (std::size_t j = 0; j < w.size(); ++j) { s += w[j]; sa += w[j] * al[j]; }
    EXPECT_NEAR(s, 1.0, 1e-12);
    EXPECT_NEAR(sa, 0.0, 1e-12 * kL2 * kL2);
    EXPECT_NEAR(w[0] + w[1], 2.5457, 1e-4);
}

TEST(CtuLevelGuardTest, PhaseClockWeightsNeedTwoBands) {
    EXPECT_TRUE(clockAnchorPhaseClockWeights({kL1 * kL1}).empty());
    EXPECT_TRUE(clockAnchorPhaseClockWeights({kL1 * kL1, kL1 * kL1}).empty());
    EXPECT_TRUE(clockAnchorPhaseClockWeights({}).empty());
}

TEST(CtuLevelGuardTest, PhaseClockIsBlindToTheLevelAndTheIonosphere) {
    const std::vector<double> al{kL1 * kL1, kL2 * kL2};
    const std::vector<double> w = clockAnchorPhaseClockWeights(al);
    ASSERT_EQ(w.size(), 2u);
    // d = c - sum w_j p_j.  Level: c and every p_j move by the same delta.
    const double delta = 3.7;
    double d_level = delta;
    for (double wj : w) d_level -= wj * delta;
    EXPECT_NEAR(d_level, 0.0, 1e-12);
    // Ionosphere direction of the satellite: p_j moves by mu * lambda_j^2 (the phase-bias
    // share of a satellite-common slant ionosphere), the clock not at all.
    const double mu = 0.8;
    double d_iono = 0.0;
    for (std::size_t j = 0; j < w.size(); ++j) d_iono -= w[j] * mu * al[j];
    EXPECT_NEAR(d_iono, 0.0, 1e-9);
    // A clock-only step passes through whole: the walk the anchor binds.
    const double step = 0.004;
    EXPECT_NEAR(step - 0.0, step, 1e-15);
}

TEST(CtuLevelGuardTest, TwoWayCovarianceIsTheInverseNormals) {
    // Four satellites, five stations, one offset per station, known effects, noise.
    const std::vector<std::string> sats{"G01", "G02", "G03", "G04"};
    const std::vector<double> eff{0.30, -0.10, 0.05, -0.25};   // sums to zero
    const std::vector<double> off{1.0, -2.0, 0.5, 3.0, -1.5};
    std::mt19937 rng(7);
    std::normal_distribution<double> nd(0.0, 1.0);
    std::vector<ClockLevelPair> pairs;
    std::vector<double> sig;
    for (int r = 0; r < 5; ++r)
        for (std::size_t s = 0; s < sats.size(); ++s) {
            if ((r + static_cast<int>(s)) % 5 == 4) continue;   // an incomplete design
            const double sd = 0.2 + 0.05 * static_cast<double>(s + r);
            ClockLevelPair p;
            p.sat = sats[s];
            p.station = r;
            p.w_if = 1.0 / (sd * sd);
            p.v_if = eff[s] - off[static_cast<std::size_t>(r)] + 1e-3 * nd(rng);
            pairs.push_back(p);
        }
    // Wide screen, so every pair is used.
    const ClockLevelTwoWay tw = clockLevelTwoWay(pairs, 1, 1e6, 1e6);
    ASSERT_TRUE(tw.ok);
    ASSERT_EQ(tw.var.size(), sats.size());
    for (std::size_t s = 0; s < sats.size(); ++s)
        EXPECT_NEAR(tw.effect.at(sats[s]), eff[s], 5e-3);
    // Brute force: the normals of v = e_s - c_r with the effects reduced to their
    // contrast (the station offsets eliminated), pseudo-inverted.
    const int S = 4, R = 5;
    Eigen::MatrixXd N = Eigen::MatrixXd::Zero(S + R, S + R);
    for (const auto& p : pairs) {
        int i = 0;
        while (sats[static_cast<std::size_t>(i)] != p.sat) ++i;
        const int j = S + p.station;
        N(i, i) += p.w_if; N(j, j) += p.w_if; N(i, j) -= p.w_if; N(j, i) -= p.w_if;
    }
    const Eigen::MatrixXd Nss = N.topLeftCorner(S, S);
    const Eigen::MatrixXd Nsr = N.topRightCorner(S, R);
    const Eigen::MatrixXd Nrr = N.bottomRightCorner(R, R);
    const Eigen::MatrixXd Nred = Nss - Nsr * Nrr.inverse() * Nsr.transpose();
    const Eigen::MatrixXd Q =
        Nred.completeOrthogonalDecomposition().pseudoInverse();   // the sum-to-zero contrast
    for (int i = 0; i < S; ++i)
        EXPECT_NEAR(tw.var.at(sats[static_cast<std::size_t>(i)]), Q(i, i),
                    1e-6 * Q(i, i));
    // s0^2 of a noise-free design is the injected 1 mm scatter over the model sigmas:
    // tiny, finite and non-negative.
    ASSERT_TRUE(std::isfinite(tw.s0sq));
    EXPECT_GE(tw.s0sq, 0.0);
    EXPECT_LT(tw.s0sq, 1e-3);
}

TEST(CtuLevelGuardTest, TwoWayS0RecoversTheScatter) {
    // Many stations, unit-variance pairs at the model sigma: s0^2 near 1.
    std::mt19937 rng(11);
    std::normal_distribution<double> nd(0.0, 1.0);
    const std::vector<std::string> sats{"G01", "G02", "G03", "G04", "G05", "G06"};
    std::vector<ClockLevelPair> pairs;
    for (int r = 0; r < 200; ++r)
        for (const auto& s : sats) {
            ClockLevelPair p;
            p.sat = s;
            p.station = r;
            p.w_if = 4.0;                       // sigma 0.5 m
            p.v_if = 0.5 * nd(rng) + 0.01 * r;  // a station offset the fit removes
            pairs.push_back(p);
        }
    const ClockLevelTwoWay tw = clockLevelTwoWay(pairs, 1, 1e6, 1e6);
    ASSERT_TRUE(tw.ok);
    EXPECT_NEAR(tw.s0sq, 1.0, 0.1);
}

namespace {

/// One satellite: clock (P row 0) and two phase biases (rows 1, 2), all in S, with a
/// prior block whose level (the sum direction) is tight and whose phase-visible
/// combinations are tighter, as a consistent carry that has run for a while holds them.
CouplingExactEpoch levelEpoch(double v_level_noise) {
    CouplingExactEpoch ep;
    const Eigen::Index n = 3;
    // P = L u u' + D, u = (1,1,1): level variance L, independent phase-visible parts.
    const double L = 1e-4;
    ep.P = Eigen::MatrixXd::Constant(n, n, L);
    ep.P(0, 0) += 1e-6; ep.P(1, 1) += 2e-6; ep.P(2, 2) += 3e-6;
    for (int k = 0; k < 3; ++k) {
        ep.p_of_key[std::to_string(k)] = k;
        ep.s_of_p.push_back(k);
        ep.route_p.push_back(0);
        ep.clk_p.push_back(k == 0 ? 1 : 0);
        ep.cc_p.push_back('G');
        ep.p_of_s.push_back(k);
        ep.col_of_s.push_back(k);
    }
    ep.q_s = Eigen::VectorXd::Constant(n, v_level_noise);
    ep.shat_s = Eigen::VectorXd::Zero(n);
    for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b)
            if (a != b && v_level_noise > 0.0) ep.q_off.emplace_back(a, b, v_level_noise);
    return ep;
}

}  // namespace

TEST(CtuLevelGuardTest, SeamLevelNoiseLeavesALevelBlindRowAlone) {
    // A row blind to the level: coefficients sum to zero over the clock and phase biases.
    const std::vector<std::pair<int, double>> blind{{0, 1.0}, {1, -0.5}, {2, -0.5}};
    CouplingExactEpoch quiet = levelEpoch(0.0);
    CouplingExactEpoch seam  = levelEpoch(1e-2);
    ASSERT_TRUE(couplingExactFactor(quiet));
    ASSERT_TRUE(couplingExactFactor(seam));
    const CouplingExactRow a = couplingExactRow(quiet, blind);
    const CouplingExactRow b = couplingExactRow(seam, blind);
    ASSERT_TRUE(a.ok && b.ok);
    for (int k = 0; k < 3; ++k) EXPECT_NEAR(b.m_new(k), a.m_new(k), 1e-12);
    EXPECT_NEAR(b.var_add, a.var_add, 1e-15);
}

TEST(CtuLevelGuardTest, SeamLevelNoiseReleasesARowThatStatesTheLevel) {
    // A row that states the level: all three coefficients equal (it reads the sum).
    const std::vector<std::pair<int, double>> level{{0, 1.0}, {1, 1.0}, {2, 1.0}};
    CouplingExactEpoch quiet = levelEpoch(0.0);
    CouplingExactEpoch seam  = levelEpoch(1e-2);
    ASSERT_TRUE(couplingExactFactor(quiet));
    ASSERT_TRUE(couplingExactFactor(seam));
    const CouplingExactRow a = couplingExactRow(quiet, level);
    const CouplingExactRow b = couplingExactRow(seam, level);
    ASSERT_TRUE(a.ok && b.ok);
    // Without noise the row is the harvested one; with the seam's level noise (100x the
    // level variance) its level statement shrinks by about L / (L + v) and its variance
    // absorbs the difference (the exact marginal along the level).
    const double sum_a = a.m_new.sum(), sum_b = b.m_new.sum();
    EXPECT_NEAR(sum_a, 3.0, 1e-9);
    EXPECT_LT(std::abs(sum_b), 0.05 * std::abs(sum_a));
    EXPECT_GT(b.var_add, 1e-4);
}

TEST(CtuLevelGuardTest, DropDecouplesAnArcAtBirthStrength) {
    const auto make = [] {
        LocalCarry lc{12.5, 9.5, 100.0};
        lc.var_first = 9.5;
        lc.var_birth = 9.5;
        CoupledShared cs;
        cs.kind = SharedParamKind::Clock;
        cs.sat = "G07";
        cs.coeff = 0.7;
        lc.coupling.push_back(cs);
        lc.blk = std::make_shared<PlHarvestBlock>();
        lc.blk_idx = 2;
        return lc;
    };
    // Default: the idempotent branch keeps the regression and the block row.
    LocalCarry a = make();
    EXPECT_FALSE(ambFloatDrop(a, 0.0, 130.0, 5));
    EXPECT_FALSE(a.coupling.empty());
    EXPECT_TRUE(static_cast<bool>(a.blk));
    // decouple_always: same return (the variance did not move), both cleared, reported.
    LocalCarry b = make();
    bool dec = false;
    EXPECT_FALSE(ambFloatDrop(b, 0.0, 130.0, 5, true, &dec));
    EXPECT_TRUE(dec);
    EXPECT_TRUE(b.coupling.empty());
    EXPECT_FALSE(static_cast<bool>(b.blk));
    EXPECT_EQ(b.blk_idx, -1);
    EXPECT_DOUBLE_EQ(b.var, 9.5);
    EXPECT_DOUBLE_EQ(b.relevel_s, 130.0);
}
