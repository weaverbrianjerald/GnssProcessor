/**
 * @file test_network_sbasis_complete.cpp
 * @brief Unit tests for SBC, the per-epoch S-basis dimension completion
 *        (@c network.sbasis_complete → NetworkSbasis::completeSbasisDimension).
 *
 * The declared S-basis dimension counts the structural gauges only
 * (per-constellation clock), but a multi-constellation epoch can realize further
 * near-null directions: per-satellite phase-bias↔slant-iono trades confined to one
 * receiver channel family (the Galileo C/Q-vs-X tracking split), with eigenvector
 * components ∝ μ_σ, resisted only by the SH-VTEC prior (λ ≈ 1e-6 ≪ cutoff).  On
 * such an epoch the rank gate observes more null directions than declared and
 * withholds the epoch, or card PA soft-anchors columns (FT63 GPS+Galileo,
 * DOY-129).  The completion derives the realized dimension and constrains each
 * extra direction along its own unit eigenvector at its carried level, so:
 *
 *  - only the deficient combination is pinned (nothing estimable moves);
 *  - the pinned combination holds its level instead of stepping (continuity
 *    target);
 *  - the gate still runs, unchanged, against the declared structural dimension
 *    and passes because the folded rows lift the extras above the cutoff.
 *
 * These tests pin the selection arithmetic (same cutoff math as the gate), the
 * eigenvector/target contract (sign-invariant), every refusal path (cap,
 * degenerate split, K2 weight margin; each refusal folds nothing), the
 * fold-then-gate integration, and the loader contract of the two knobs.
 * Spectra are planted directly (diagonal, or spectral synthesis for a mixed
 * eigenvector), since the spectrum is the completion's own input; decade
 * placement is therefore exact.
 */

#include <gtest/gtest.h>
#include <Eigen/Dense>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "config/GnssConfig.hpp"
#include "network/solve/Estimator.hpp"
#include "network/solve/Sbasis.hpp"
#include "logging/LogManager.hpp"

namespace GnssProcessor {
namespace {

/// A planted spectrum as a diagonal matrix: the eigenvectors are the unit basis
/// vectors, so the expected rows and targets are exact.
Eigen::MatrixXd planted(const std::vector<double>& ev) {
    Eigen::MatrixXd D = Eigen::MatrixXd::Zero(static_cast<Eigen::Index>(ev.size()),
                                              static_cast<Eigen::Index>(ev.size()));
    for (std::size_t i = 0; i < ev.size(); ++i)
        D(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(i)) = ev[i];
    return D;
}

/// Column labeler mimicking NetworkEstimator::sharedColumnLabel over a fixed
/// synthetic layout (index-addressed).
std::function<std::string(Eigen::Index)> labels(std::vector<std::string> v) {
    return [v = std::move(v)](Eigen::Index c) -> std::string {
        return c >= 0 && static_cast<std::size_t>(c) < v.size()
                   ? v[static_cast<std::size_t>(c)]
                   : "col?";
    };
}

constexpr double kTol = 1e-9;   // the default rank-gate tolerance

}  // namespace

// ---------------------------------------------------------------------------
// Selection arithmetic: the completion counts with the gate's own cutoff and
// leaves both "nothing extra" cases alone.
// ---------------------------------------------------------------------------

TEST(NetworkSbasisCompleteTest, CleanEpochYieldsNoRows) {
    // λmax 1e6, cutoff 1e-3: two structural gauges at 1e-10, weakest estimable
    // direction at 1e-2 (above cutoff).  observed == declared ⇒ no completion.
    NetworkSbasis sb;
    const auto M = planted({1e-10, 1e-10, 1e-2, 1e6});
    const auto c = sb.completeSbasisDimension(M, 2, kTol, Eigen::VectorXd(),
                                              nullptr, 6);
    EXPECT_EQ(c.observed, 2);
    EXPECT_EQ(c.n_extra, 0);
    EXPECT_EQ(c.H.rows(), 0);
    EXPECT_FALSE(c.capped);
    EXPECT_FALSE(c.degenerate);
    EXPECT_FALSE(c.weight_marginal);
}

TEST(NetworkSbasisCompleteTest, OverSpecifiedIsTheGatesVerdictNotOurs) {
    // observed < declared (a tolerance below the config's gauge depth): the
    // completion does nothing.  The gate's "over-specified" throw is the
    // calibration signal, and folding rows could mask it.
    NetworkSbasis sb;
    const auto M = planted({1.0, 2.0, 1e6});
    const auto c = sb.completeSbasisDimension(M, 1, kTol, Eigen::VectorXd(),
                                              nullptr, 6);
    EXPECT_EQ(c.observed, 0);
    EXPECT_EQ(c.n_extra, 0);
    EXPECT_FALSE(c.capped || c.degenerate || c.weight_marginal);
}

// ---------------------------------------------------------------------------
// The core contract: one extra direction ⇒ one eigenvector row at the carried
// level, sign-invariant, with attribution.
// ---------------------------------------------------------------------------

TEST(NetworkSbasisCompleteTest, ExtraDirectionConstrainedAtCarriedLevel) {
    // λmax 1e6 ⇒ cutoff 1e-3.  One structural gauge at 1e-10 (machine-zero
    // scale) and one extra at 1e-5, the class measured on FT63: below the cutoff,
    // above the 1e-14·λmax = 1e-8 degeneracy floor, 5 decades off the gauge.
    NetworkSbasis sb;
    const auto M = planted({1e-10, 1e-5, 1e6});
    Eigen::VectorXd x_prev(3);
    x_prev << 4.0, -2.5, 7.0;
    const auto c = sb.completeSbasisDimension(
        M, 1, kTol, x_prev, labels({"CLK:G01", "PB:E03:L5X", "PB:G01:L1W"}), 6);
    ASSERT_EQ(c.n_extra, 1);
    EXPECT_EQ(c.observed, 2);
    ASSERT_EQ(c.H.rows(), 1);
    ASSERT_EQ(c.dirs.size(), 1u);
    // The row is ±e_1 (the 1e-5 column).  The constraint is sign-invariant:
    // H x = z with H = s·e₁ᵀ, z = s·x_prev(1) for s = ±1.
    EXPECT_NEAR(std::abs(c.H(0, 1)), 1.0, 1e-9);
    EXPECT_NEAR(std::abs(c.H(0, 0)), 0.0, 1e-9);
    EXPECT_NEAR(std::abs(c.H(0, 2)), 0.0, 1e-9);
    EXPECT_NEAR(c.z(0) / c.H(0, 1), x_prev(1), 1e-9)
        << "the pinned combination must HOLD its carried level — a zero target "
           "would step the emitted bias by the whole level at the engage edge";
    // Attribution: the direction is the E03 column; the satellite identity set
    // feeds the engage/release events.
    EXPECT_NEAR(c.dirs[0].ev, 1e-5, 1e-9);
    EXPECT_NEAR(c.dirs[0].top_span, 1.0, 1e-9);
    EXPECT_EQ(c.dirs[0].top_label, "PB:E03:L5X");
    EXPECT_EQ(c.dirs[0].sats, std::set<std::string>{"E03"});
    EXPECT_NE(c.dirs[0].detail.find("PB:E03:L5X"), std::string::npos);
}

TEST(NetworkSbasisCompleteTest, MixedEigenvectorRowAndTargetFollowTheDirection) {
    // Spectral synthesis: the extra direction is spread over two phase-bias
    // columns (the measured directions spread ∝ μ_σ over a channel family).
    // v = (0.6, 0.8, 0) at λ=1e-5, its orthonormal partner at λ=1e-10 (the
    // gauge), and a strong direction at λ=1e6.
    Eigen::Vector3d v(0.6, 0.8, 0.0), u(-0.8, 0.6, 0.0), w(0.0, 0.0, 1.0);
    const Eigen::MatrixXd M =
        1e-10 * u * u.transpose() + 1e-5 * v * v.transpose() + 1e6 * w * w.transpose();
    Eigen::VectorXd x_prev(3);
    x_prev << 2.0, 1.0, 7.0;   // v·x_prev = 2.0
    NetworkSbasis sb;
    const auto c = sb.completeSbasisDimension(
        M, 1, kTol, x_prev, labels({"PB:E09:L5X", "PB:E09:L7X", "CLK:G01"}), 6);
    ASSERT_EQ(c.n_extra, 1);
    const Eigen::VectorXd row = c.H.row(0).transpose();
    // The row spans v (up to sign): |row·v| = 1, and the target moves with the
    // same sign so the realized constraint is v·x = v·x_prev.
    EXPECT_NEAR(std::abs(row.dot(v)), 1.0, 1e-6);
    const double s = row.dot(v) > 0 ? 1.0 : -1.0;
    EXPECT_NEAR(c.z(0), s * 2.0, 1e-6);
    // Both participating columns are named; the identity set collapses to the
    // one satellite, which is what keeps the engage/release identity stable
    // when two weak satellites' eigenvectors mix.
    EXPECT_EQ(c.dirs[0].sats, std::set<std::string>{"E09"});
    EXPECT_NE(c.dirs[0].detail.find("PB:E09:L5X"), std::string::npos);
    EXPECT_NE(c.dirs[0].detail.find("PB:E09:L7X"), std::string::npos);
}

TEST(NetworkSbasisCompleteTest, MissingCarryMeansZeroTargets) {
    NetworkSbasis sb;
    const auto M = planted({1e-10, 1e-5, 1e6});
    // Wrong-length x_prev (e.g. cold start built nothing) ⇒ targets 0.
    const auto c = sb.completeSbasisDimension(M, 1, kTol, Eigen::VectorXd(),
                                              nullptr, 6);
    ASSERT_EQ(c.n_extra, 1);
    EXPECT_EQ(c.z(0), 0.0);
    EXPECT_EQ(c.dirs[0].top_label, "col1") << "no labeler ⇒ index fallback";
}

// ---------------------------------------------------------------------------
// Fold-then-gate integration: with the completion's rows folded, the unchanged
// gate passes at the declared structural dimension.
// ---------------------------------------------------------------------------

TEST(NetworkSbasisCompleteTest, FoldedRowsLiftTheGateToDeclared) {
    NetworkSbasis sb;
    const auto M = planted({1e-10, 1e-5, 3e-5, 1e6});   // declared 1 + 2 extras
    // Pre-completion the gate must refuse (observed 3 != declared 1).
    EXPECT_THROW(sb.assertNullSpaceRank(M, 1, kTol), std::runtime_error);
    const auto c = sb.completeSbasisDimension(M, 1, kTol, Eigen::VectorXd(),
                                              nullptr, 6);
    ASSERT_EQ(c.n_extra, 2);
    // The driver folds w·HᵀH (addSharedConstraintRows); reproduce the algebra.
    const Eigen::MatrixXd M2 =
        M + NetworkSbasis::kSbasisCompleteWeight * c.H.transpose() * c.H;
    int observed = -1;
    EXPECT_NO_THROW(sb.assertNullSpaceRank(M2, 1, kTol, &observed))
        << "the folded rows must lift exactly the extras above the cutoff";
    EXPECT_EQ(observed, 1);
}

// ---------------------------------------------------------------------------
// Refusal paths: each folds nothing and reports its cause (the driver counts
// them separately and lets the gate withhold).
// ---------------------------------------------------------------------------

TEST(NetworkSbasisCompleteTest, BurstBeyondCapRefuses) {
    NetworkSbasis sb;
    const auto M = planted({1e-5, 2e-5, 3e-5, 1e6});   // declared 0, 3 extras
    const auto c = sb.completeSbasisDimension(M, 0, kTol, Eigen::VectorXd(),
                                              nullptr, 2);
    EXPECT_TRUE(c.capped);
    EXPECT_EQ(c.n_extra, 0);
    EXPECT_EQ(c.H.rows(), 0);
    EXPECT_EQ(c.observed, 3) << "the realized dimension is still reported";
    // Uncapped (<= 0) admits the same burst; this is the diagnostic arm.
    const auto d = sb.completeSbasisDimension(M, 0, kTol, Eigen::VectorXd(),
                                              nullptr, 0);
    EXPECT_FALSE(d.capped);
    EXPECT_EQ(d.n_extra, 3);
}

TEST(NetworkSbasisCompleteTest, UnseparatedExtrasAreSkippedNotEpochFatal) {
    NetworkSbasis sb;
    // (a) Extra within a decade of the declared gauge: the eigenvectors of a
    // near-degenerate cluster mix, so a row could re-target a pin-owned
    // combination.  It is skipped (left to the drop-and-heal) and nothing is
    // folded.
    const auto Ma = planted({1e-6, 5e-6, 1e6});
    const auto ca = sb.completeSbasisDimension(Ma, 1, kTol, Eigen::VectorXd(),
                                               nullptr, 6);
    EXPECT_TRUE(ca.degenerate);
    EXPECT_EQ(ca.n_skipped, 1);
    EXPECT_EQ(ca.n_extra, 0);
    // (b) Extra at machine zero with a declared gauge at exactly 0.0: the
    // relative test alone (10·0 = 0) would admit it, so the absolute floor
    // (1e-14·λmax = 1e-8 here) catches it.  This is the orphaned-exact-gauge
    // class, left to card PA by design.
    const auto Mb = planted({0.0, 1e-12, 1e6});
    const auto cb = sb.completeSbasisDimension(Mb, 1, kTol, Eigen::VectorXd(),
                                               nullptr, 6);
    EXPECT_TRUE(cb.degenerate);
    EXPECT_EQ(cb.n_skipped, 1);
    EXPECT_EQ(cb.n_extra, 0);
    // (c) With no declared gauges (constraint-realized clock datums) there is
    // nothing to mix with, so an extra at machine zero is completable.
    const auto Mc = planted({1e-12, 1e6});
    const auto cc = sb.completeSbasisDimension(Mc, 0, kTol, Eigen::VectorXd(),
                                               nullptr, 6);
    EXPECT_FALSE(cc.degenerate);
    EXPECT_EQ(cc.n_skipped, 0);
    EXPECT_EQ(cc.n_extra, 1);
}

TEST(NetworkSbasisCompleteTest, OrphanPlusTradeComposesWithTheHeal) {
    // The withhold-cluster shape measured on FT63 (gg): declared gauge at
    // machine zero, one orphaned exact gauge in the same cluster (1.5e-10,
    // inside a decade of the gauge and below the 1e-8 floor), and one separated
    // weak trade (1e-5).  The completion skips the orphan, constrains the trade,
    // and leaves the gate throwing by exactly the skipped direction, which
    // remains for the drop-and-heal.
    NetworkSbasis sb;
    const auto M = planted({1e-10, 1.5e-10, 1e-5, 1e6});
    const auto c = sb.completeSbasisDimension(M, 1, kTol, Eigen::VectorXd(),
                                              nullptr, 6);
    EXPECT_TRUE(c.degenerate);
    EXPECT_EQ(c.n_skipped, 1);
    ASSERT_EQ(c.n_extra, 1);
    // The constrained row is the trade's eigenvector (column 2), not the
    // orphan's.
    EXPECT_NEAR(std::abs(c.H(0, 2)), 1.0, 1e-9);
    // After folding, the gate still counts gauge + orphan (observed 2 against
    // declared 1) and throws, which cues the drop-and-heal, but by one direction
    // instead of two.  That composition is what lets the drop-and-heal resolve
    // the PA5-insufficiency epochs.
    const Eigen::MatrixXd M2 =
        M + NetworkSbasis::kSbasisCompleteWeight * c.H.transpose() * c.H;
    int observed = -1;
    EXPECT_THROW(sb.assertNullSpaceRank(M2, 1, kTol, &observed),
                 std::runtime_error);
    EXPECT_EQ(observed, 2);
}

TEST(NetworkSbasisCompleteTest, WeightWithinTwoDecadesOfCutoffRefuses) {
    NetworkSbasis sb;
    // λmax 1e14 ⇒ cutoff 1e5: kSbasisCompleteWeight (1e3) sits below it, so a
    // folded row could not lift its direction past the gate, let alone clear the
    // K2 two-decade margin.  Extra at 1e2 (below cutoff, above the
    // 1e-14·λmax = 1.0 floor, 12 decades off the gauge).
    const auto M = planted({1e-10, 1e2, 1e14});
    const auto c = sb.completeSbasisDimension(M, 1, kTol, Eigen::VectorXd(),
                                              nullptr, 6);
    EXPECT_TRUE(c.weight_marginal);
    EXPECT_EQ(c.n_extra, 0);
}

// ---------------------------------------------------------------------------
// Loader contract: off by default (byte-identical), documented cap default of 6,
// and the uncapped diagnostic spelling parses.
// ---------------------------------------------------------------------------

namespace {

std::filesystem::path writeSbcCfg(const std::string& tag,
                                  const std::string& network_extra) {
    const std::filesystem::path p =
        std::filesystem::temp_directory_path() / ("sbc_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << "{\n"
         "  \"processing\": {\n"
         "    \"mode\": \"ppp-rtk-network\",\n"
         "    \"active_systems\": [ \"GPS\", \"Galileo\" ],\n"
         "    \"signal_priorities\": { \"GPS\": \"WC\", \"Galileo\": \"CB\" }\n"
         "  },\n"
         "  \"network\": {\n"
         "    \"clock_datum_station\": \"AAAA\",\n"
      << network_extra <<
         "    \"stations\": [\n"
         "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
         "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
         "    ]\n"
         "  }\n"
         "}\n";
    f.close();
    return p;
}

}  // namespace

TEST(NetworkSbasisCompleteTest, LoaderDefaultsOffWithDocumentedCap) {
    LogManager log;
    const auto p = writeSbcCfg("default", "");
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(cfg.networkSbasisComplete())
        << "default-OFF is the byte-identity claim for every existing config";
    EXPECT_EQ(cfg.networkSbasisCompleteMax(), 6);
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

TEST(NetworkSbasisCompleteTest, LoaderArmsAndParsesTheCap) {
    LogManager log;
    {
        const auto p = writeSbcCfg("armed",
                                   "    \"sbasis_complete\": true,\n"
                                   "    \"sbasis_complete_max\": 3,\n");
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.networkSbasisComplete());
        EXPECT_EQ(cfg.networkSbasisCompleteMax(), 3);
        std::error_code ec; std::filesystem::remove(p, ec);
    }
    {
        // Uncapped diagnostic spelling: accepted (warned, not refused).
        const auto p = writeSbcCfg("uncapped",
                                   "    \"sbasis_complete\": true,\n"
                                   "    \"sbasis_complete_max\": 0,\n");
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.networkSbasisComplete());
        EXPECT_EQ(cfg.networkSbasisCompleteMax(), 0);
        std::error_code ec; std::filesystem::remove(p, ec);
    }
}

} // namespace GnssProcessor
