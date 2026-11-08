/**
 * @file test_network_vcf.cpp
 * @brief The per-class variance factor (network/qc/Vcf.hpp).
 *
 * The factor is a ratio of two sums that both look plausible when either is wrong: a feed that
 * drops the redundancy reads half the true factor on a design of redundancy one half, and a
 * class map that pools the second code channel's companion rows (weighted at sigma times 10)
 * reads a code factor near zero.  The tests pin:
 *
 *   1. On synthetic least-squares blocks of known noise, with the local redundancy drawn from the
 *      blocks' own hat matrix, f = sum w v^2 / sum r and the robust g read 1.0 within 5 %, and
 *      0.25 when the noise is halved; sum w v^2 / n reads the redundancy per row instead.
 *   2. The class map: code and phase rows land in their per-signal class and in their
 *      aggregate; companion and second-phase rows in their counted classes only; a deweighted row
 *      is counted, not pooled; a non-observation row is ignored.
 *   3. The EWMA seeds on its first value and steps by alpha.
 *   4. The stage-2 scale is 1 until the aggregate class is identifiable with three sweeps, then
 *      the root of its robust EWMA; a class under the redundancy-per-row bar is not identifiable.
 *   5. The loader: off by default, the stock knobs inert, network.vcf without the census is a
 *      hard error, and network.vcf_apply without network.vcf is disarmed.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include <Eigen/Dense>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "network/assembly/Contributions.hpp"
#include "network/qc/Vcf.hpp"
#include "network/state/RunContext.hpp"
#include "../support/TestTempDir.hpp"

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

/// Feed @p n_blocks independent least-squares blocks (4 rows, 2 parameters, random design and
/// weights in [0.5, 5]) with noise of standard deviation @p noise_scale / sqrt(w) per row.  The
/// residual and the local redundancy of every row come from the block's own normal equations, the
/// quantities the census kernel forms for a station block.
void feedSyntheticBlocks(NetworkVcfEpoch& acc, int n_blocks, double noise_scale,
                         unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> gauss(0.0, 1.0);
    std::uniform_real_distribution<double> unif(0.5, 5.0);
    std::uniform_real_distribution<double> coef(-1.0, 1.0);
    const RowTag tag = makeRowTag(ContributionKind::DataCode, "G05", "C1C");
    NetworkVcfStationFeed feed(acc, 100.0);
    for (int b = 0; b < n_blocks; ++b) {
        Eigen::Matrix<double, 4, 2> A;
        Eigen::Vector4d w, y;
        for (int i = 0; i < 4; ++i) {
            A(i, 0) = 1.0;
            A(i, 1) = coef(rng);
            w(i) = unif(rng);
            y(i) = noise_scale * gauss(rng) / std::sqrt(w(i));   // true parameters zero
        }
        const Eigen::Matrix2d N = A.transpose() * w.asDiagonal() * A;
        const Eigen::Matrix2d Q = N.inverse();
        const Eigen::Vector2d x = Q * (A.transpose() * w.asDiagonal() * y);
        const Eigen::Vector4d v = y - A * x;
        for (int i = 0; i < 4; ++i) {
            const double r = 1.0 - w(i) * A.row(i).dot(Q * A.row(i).transpose());
            feed.add(tag, w(i), v(i), r, false);
        }
    }
}

// --- config-load fixtures (the house pattern, test_network_mdb) --------------------

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = gnss_test::tempDir() / ("netvcf_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

std::string networkConfig(const std::string& extra = "") {
    return std::string(
        "{\n"
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\", \"Galileo\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\", \"Galileo\": \"CB\" }\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"AAAA\",\n"
        + extra +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n");
}

bool loadConfig(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = writeTempConfig(tag, body);
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

}  // namespace

// ===========================================================================
//  1. The factor on known noise
// ===========================================================================

TEST(NetworkVcf, KnownNoiseReadsOneWithTheRedundancyDrawn) {
    NetworkVcfEpoch acc;
    feedSyntheticBlocks(acc, 5000, 1.0, 20261107u);
    ASSERT_EQ(acc.cls.count("code"), 1u);
    NetworkVcfClassAcc a = acc.cls.at("code");
    ASSERT_EQ(a.n, 20000);
    const NetworkVcfFactors fx = networkVcfClassFactors(a);
    ASSERT_TRUE(fx.have_f);
    ASSERT_TRUE(fx.have_g);
    EXPECT_NEAR(fx.f, 1.0, 0.05) << "the redundancy-weighted factor on calibrated noise";
    EXPECT_NEAR(fx.g, 1.0, 0.05) << "the robust factor on calibrated noise";
    // Two parameters per four rows: the redundancy per row is one half, and the unnormalized
    // statistic reads it instead of the factor (the reason the redundancy is drawn).
    EXPECT_NEAR(fx.rho, 0.5, 1e-9);
    EXPECT_NEAR(a.sum_wv2 / static_cast<double>(a.n), 0.5, 0.03);
    EXPECT_TRUE(fx.identifiable);
}

TEST(NetworkVcf, HalvedNoiseReadsAQuarter) {
    NetworkVcfEpoch acc;
    feedSyntheticBlocks(acc, 5000, 0.5, 777u);
    NetworkVcfClassAcc a = acc.cls.at("code");
    const NetworkVcfFactors fx = networkVcfClassFactors(a);
    EXPECT_NEAR(fx.f, 0.25, 0.25 * 0.05);
    EXPECT_NEAR(fx.g, 0.25, 0.25 * 0.05);
    // The per-signal class carries the same rows.
    NetworkVcfClassAcc s = acc.cls.at(vcfSignalClassKey('G', "C1C"));
    EXPECT_EQ(s.n, a.n);
    EXPECT_DOUBLE_EQ(s.sum_wv2, a.sum_wv2);
}

TEST(NetworkVcf, EmptyAndSmallClassesAreFlagged) {
    NetworkVcfClassAcc a;
    NetworkVcfFactors fx = networkVcfClassFactors(a);
    EXPECT_FALSE(fx.have_f);
    EXPECT_FALSE(fx.have_g);
    EXPECT_FALSE(fx.identifiable);
    // Enough rows but a redundancy per row under the bar: formed, not identifiable.
    for (int i = 0; i < 100; ++i) { ++a.n; a.sum_wv2 += 0.01; a.sum_r += 0.05; a.z.push_back(0.4); }
    fx = networkVcfClassFactors(a);
    EXPECT_TRUE(fx.have_f);
    EXPECT_TRUE(fx.enough);
    EXPECT_NEAR(fx.rho, 0.05, 1e-12);
    EXPECT_FALSE(fx.identifiable);
}

// ===========================================================================
//  2. The class map
// ===========================================================================

TEST(NetworkVcf, RowsLandInTheirClasses) {
    NetworkVcfEpoch acc;
    NetworkVcfStationFeed feed(acc, 30.0);
    feed.add(makeRowTag(ContributionKind::DataCode, "G05", "C1W"), 4.0, 0.1, 0.9, false);
    feed.add(makeRowTag(ContributionKind::DataPhase, "E11", "L5Q"), 1e5, 0.001, 0.4, false);
    feed.add(makeRowTag(ContributionKind::DataCodeCompanion, "G05", "C1C"), 0.04, 0.2, 1.0, false);
    feed.add(makeRowTag(ContributionKind::DataPhaseSecond, "G07", "L2L"), 1e5, 0.001, 0.4, false);
    feed.add(makeRowTag(ContributionKind::DataPhase, "G07", "L1C"), 1e5, 0.002, 0.4, true);
    feed.add(makeRowTag(ContributionKind::BlockAmbiguityCarry, "G07", "L1C"), 1e5, 0.002, 0.4, false);
    feed.add(makeRowTag(ContributionKind::DataCode, "G08", "C2W"), 0.0, 0.1, 0.9, false);   // no weight
    EXPECT_TRUE(acc.fed);
    EXPECT_DOUBLE_EQ(acc.tow, 30.0);
    EXPECT_EQ(acc.rows_skipped, 1);
    EXPECT_EQ(acc.cls.at("code").n, 1);
    EXPECT_EQ(acc.cls.at(vcfSignalClassKey('G', "C1W")).n, 1);
    EXPECT_EQ(acc.cls.at("phase").n, 1);
    EXPECT_EQ(acc.cls.at("phase").n_dw, 1);
    EXPECT_EQ(acc.cls.at(vcfSignalClassKey('E', "L5Q")).n, 1);
    EXPECT_EQ(acc.cls.at(vcfSignalClassKey('G', "L1C")).n, 0);
    EXPECT_EQ(acc.cls.at(vcfSignalClassKey('G', "L1C")).n_dw, 1);
    EXPECT_EQ(acc.cls.at("code_companion").n, 1);
    EXPECT_EQ(acc.cls.at("phase_second").n, 1);
    EXPECT_EQ(acc.cls.count(vcfSignalClassKey('G', "C1C")), 0u) << "companions have no per-signal class";
    EXPECT_EQ(acc.cls.count(vcfSignalClassKey('G', "L2L")), 0u);
    EXPECT_EQ(acc.cls.at("code").sys, '*');
    EXPECT_EQ(acc.cls.at(vcfSignalClassKey('E', "L5Q")).kind, VcfKind::Phase);
    // A new stamp resets the accumulator.
    NetworkVcfStationFeed next(acc, 60.0);
    EXPECT_TRUE(acc.cls.empty());
    EXPECT_DOUBLE_EQ(acc.tow, 60.0);
}

TEST(NetworkVcf, UncheckableRowsStayOutOfTheRobustPool) {
    NetworkVcfEpoch acc;
    NetworkVcfStationFeed feed(acc, 30.0);
    const RowTag t = makeRowTag(ContributionKind::DataCode, "G05", "C1W");
    feed.add(t, 4.0, 0.0, 0.0, false);
    feed.add(t, 4.0, 0.2, 0.5, false);
    const NetworkVcfClassAcc& a = acc.cls.at("code");
    EXPECT_EQ(a.n, 2);
    EXPECT_EQ(a.n_unchk, 1);
    EXPECT_EQ(a.z.size(), 1u);
    EXPECT_NEAR(a.z[0], 0.2 * std::sqrt(4.0 / 0.5), 1e-12);
}

// ===========================================================================
//  3. The EWMA
// ===========================================================================

TEST(NetworkVcf, EwmaSeedsThenSteps) {
    EXPECT_DOUBLE_EQ(networkVcfEwma(0.0, 0.3, 0.1, false), 0.3);
    EXPECT_DOUBLE_EQ(networkVcfEwma(0.3, 0.5, 0.1, true), 0.1 * 0.5 + 0.9 * 0.3);
    double e = 0.0;
    bool seeded = false;
    for (int k = 0; k < 200; ++k) { e = networkVcfEwma(e, 0.16, 0.1, seeded); seeded = true; }
    EXPECT_NEAR(e, 0.16, 1e-12) << "a constant input is a fixed point";
    // alpha 1 follows the input.
    EXPECT_DOUBLE_EQ(networkVcfEwma(0.3, 0.7, 1.0, true), 0.7);
}

// ===========================================================================
//  4. The stage-2 scale
// ===========================================================================

TEST(NetworkVcf, ApplyScaleWaitsForAnIdentifiableWarmClass) {
    NetworkVcfState st;
    EXPECT_DOUBLE_EQ(networkVcfApplyScale(st, VcfKind::Code), 1.0) << "no class yet";
    NetworkVcfClassState& c = st.classes["code"];
    c.kind = VcfKind::Code;
    c.ewma_g = 0.16;
    c.identifiable_last = true;
    c.sweeps = kVcfApplyMinSweeps - 1;
    EXPECT_DOUBLE_EQ(networkVcfApplyScale(st, VcfKind::Code), 1.0) << "not warm";
    c.sweeps = kVcfApplyMinSweeps;
    EXPECT_NEAR(networkVcfApplyScale(st, VcfKind::Code), 0.4, 1e-12);
    c.identifiable_last = false;
    EXPECT_DOUBLE_EQ(networkVcfApplyScale(st, VcfKind::Code), 1.0) << "not identifiable";
    // The phase scale reads the phase aggregate only; the counted kinds never apply.
    EXPECT_DOUBLE_EQ(networkVcfApplyScale(st, VcfKind::Phase), 1.0);
    EXPECT_DOUBLE_EQ(networkVcfApplyScale(st, VcfKind::CodeCompanion), 1.0);
    NetworkVcfClassState& p = st.classes["phase"];
    p.kind = VcfKind::Phase;
    p.ewma_g = 0.25;
    p.identifiable_last = true;
    p.sweeps = 10;
    EXPECT_NEAR(networkVcfApplyScale(st, VcfKind::Phase), 0.5, 1e-12);
}

// ===========================================================================
//  5. The loader
// ===========================================================================

TEST(NetworkVcf, StockConfigLeavesTheFactorInert) {
    GnssConfig cfg;
    ASSERT_TRUE(loadConfig("off", networkConfig(), cfg));
    EXPECT_FALSE(cfg.networkVcf());
    EXPECT_FALSE(cfg.networkVcfApply());
    EXPECT_DOUBLE_EQ(cfg.networkVcfEwmaAlpha(), 0.1);
    const NetworkKnobs k = NetworkKnobs::snapshot(cfg, cfg.getNetworkConfig());
    EXPECT_FALSE(k.vcf_on);
    EXPECT_FALSE(k.vcf_apply);
}

TEST(NetworkVcf, ArmingWithoutTheCensusIsRejected) {
    GnssConfig cfg;
    EXPECT_FALSE(loadConfig("nocensus", networkConfig("    \"vcf\": true,\n"), cfg));
}

TEST(NetworkVcf, ArmedOnTheCensusResolvesTheKnobs) {
    GnssConfig cfg;
    ASSERT_TRUE(loadConfig("armed", networkConfig(
        "    \"vcf\": true,\n"
        "    \"vcf_ewma_alpha\": 0.2,\n"
        "    \"redundancy_census_stride\": 10,\n"), cfg));
    const NetworkKnobs k = NetworkKnobs::snapshot(cfg, cfg.getNetworkConfig());
    EXPECT_TRUE(k.vcf_on);
    EXPECT_DOUBLE_EQ(k.vcf_alpha, 0.2);
    EXPECT_FALSE(k.vcf_apply);
}

TEST(NetworkVcf, ApplyWithoutTheMeasurementIsDisarmed) {
    GnssConfig cfg;
    ASSERT_TRUE(loadConfig("applyonly", networkConfig(
        "    \"vcf_apply\": true,\n"
        "    \"redundancy_census_stride\": 10,\n"), cfg));
    EXPECT_FALSE(cfg.networkVcfApply());
    GnssConfig bad;
    ASSERT_TRUE(loadConfig("alpha", networkConfig(
        "    \"vcf\": true,\n"
        "    \"vcf_ewma_alpha\": 0.0,\n"
        "    \"redundancy_census_stride\": 10,\n"), bad));
    EXPECT_DOUBLE_EQ(bad.networkVcfEwmaAlpha(), 0.1) << "an alpha outside (0, 1] is restored";
}

// The phase scale reaches the RAIM pools only under its own key, which needs network.vcf_apply.
TEST(NetworkVcf, ThePhaseScaleNeedsItsOwnKeyOnTopOfApply) {
    GnssConfig cfg;
    ASSERT_TRUE(loadConfig("phaseonly", networkConfig(
        "    \"vcf\": true,\n"
        "    \"vcf_apply_phase\": true,\n"
        "    \"redundancy_census_stride\": 10,\n"), cfg));
    EXPECT_FALSE(cfg.networkVcfApplyPhase()) << "disarmed without network.vcf_apply";
    GnssConfig code;
    ASSERT_TRUE(loadConfig("codeonly", networkConfig(
        "    \"vcf\": true,\n"
        "    \"vcf_apply\": true,\n"
        "    \"redundancy_census_stride\": 10,\n"), code));
    NetworkKnobs k = NetworkKnobs::snapshot(code, code.getNetworkConfig());
    EXPECT_TRUE(k.vcf_apply);
    EXPECT_FALSE(k.vcf_apply_phase);
    GnssConfig both;
    ASSERT_TRUE(loadConfig("both", networkConfig(
        "    \"vcf\": true,\n"
        "    \"vcf_apply\": true,\n"
        "    \"vcf_apply_phase\": true,\n"
        "    \"redundancy_census_stride\": 10,\n"), both));
    k = NetworkKnobs::snapshot(both, both.getNetworkConfig());
    EXPECT_TRUE(k.vcf_apply);
    EXPECT_TRUE(k.vcf_apply_phase);
}
