/**
 * @file test_wl_only_ffr.cpp
 * @brief Card Q3 §8.11 — the WL-only AR acceptance gate
 *        (@ref AmbiguityResolverConfig::wl_only_ffr).
 *
 * The WL-only / MW-WL-constraint paths assert MW wide-lane integers directly:
 * no LAMBDA search, so no ratio test and — until this gate existed — no
 * success-rate gate either, only the `wl_max_frac` POINT test on each arc's
 * running residual.  A point test cannot bound P(the integer is wrong), and
 * that is what an acceptance decision has to control.  Measured cost of the
 * gap: an ungated 5-constraint fix put a held-out consumer 35 m off while
 * `ratio` reported the "+inf = no LAMBDA ran" sentinel.
 *
 * What is pinned here is the FAIL-SAFE behaviour, because that is precisely
 * what a field run cannot demonstrate — a clean run only shows that nothing
 * went wrong, never that the guard would have fired.
 */
#include <gtest/gtest.h>

#include "filter/AmbiguityResolver.hpp"
#include "filter/IlsCore.hpp"

#include <cmath>
#include <map>
#include <set>
#include <string>

namespace GnssProcessor {

/// Befriended in AmbiguityResolver.hpp — forwards to the private gate.  MUST be
/// declared in this namespace: the friend declaration names
/// GnssProcessor::WlFfrTestAccess, and a global-scope twin is a different type.
struct WlFfrTestAccess {
    static double rate(const AmbiguityResolver& r,
                       const std::set<std::string>& arcs,
                       const std::map<std::string, WlArcQuality>& q,
                       std::string* worst) {
        return r.wlArcSuccessRate_(arcs, q, worst);
    }
};

}  // namespace GnssProcessor

using namespace GnssProcessor;

namespace {

/// A resolver whose only relevant setting is the maturity threshold.
AmbiguityResolver makeResolver(int min_arc_count = 10) {
    AmbiguityResolverConfig cfg;
    cfg.wl_only_ffr = true;
    cfg.wl_only_min_arc_count = min_arc_count;
    LogManager lm;
    AmbiguityResolver r(cfg, lm);
    return r;
}

constexpr double kInv2Sqrt2 = 0.353553390593273762200422181052424519;
double arcTerm(double sigma_cycles) { return std::erf(kInv2Sqrt2 / sigma_cycles); }

}  // namespace

// --- The gate must agree with the LAMBDA tier's own functional ------------
// Both tiers have to report ONE comparable number, or "p_succ" means something
// different depending on which machinery fixed — which is the ambiguity this
// whole card is about.
TEST(WlOnlyFfr, PerArcTermMatchesTheLambdaTierBootstrap) {
    AmbiguityResolver r = makeResolver();
    const double sigma = 0.08;                       // cycles
    std::map<std::string, WlArcQuality> q{
        {"G01_L1_L2", WlArcQuality{sigma, 50, false}}};
    const double got = WlFfrTestAccess::rate(r, {"G01_L1_L2"}, q, nullptr);

    // ils::computeBootstrapSuccessRate takes VARIANCES; one arc of variance
    // sigma^2 must give the identical number.
    Eigen::VectorXd d(1);
    d(0) = sigma * sigma;
    EXPECT_NEAR(got, ils::computeBootstrapSuccessRate(d), 1e-12);
    EXPECT_NEAR(got, arcTerm(sigma), 1e-12);
}

// --- Product runs over DISTINCT ARCS --------------------------------------
TEST(WlOnlyFfr, ProductIsOverDistinctArcs) {
    AmbiguityResolver r = makeResolver();
    std::map<std::string, WlArcQuality> q{
        {"G01_L1_L2", WlArcQuality{0.05, 40, false}},
        {"G02_L1_L2", WlArcQuality{0.09, 40, false}},
    };
    const double got = WlFfrTestAccess::rate(r, {"G01_L1_L2", "G02_L1_L2"}, q, nullptr);
    EXPECT_NEAR(got, arcTerm(0.05) * arcTerm(0.09), 1e-12);
}

// --- FAIL-SAFE: an arc we cannot bound contributes ZERO, never 1 -----------
// "No quality entry" means the arc's failure rate is UNKNOWN.  Treating unknown
// as fine is the exact shape of the defect this gate closes.
TEST(WlOnlyFfr, MissingArcQualityIsZeroNotOne) {
    AmbiguityResolver r = makeResolver();
    std::map<std::string, WlArcQuality> q{
        {"G01_L1_L2", WlArcQuality{0.01, 100, false}}};
    std::string worst;
    const double got = WlFfrTestAccess::rate(r, {"G01_L1_L2", "G99_L1_L2"}, q, &worst);
    EXPECT_DOUBLE_EQ(got, 0.0);
    EXPECT_EQ(worst, "G99_L1_L2");   // and it is named, so elimination can drop it
}

// --- FAIL-SAFE: an IMMATURE arc contributes zero even with a tiny sigma ----
// Two samples that happen to agree give sigma ~ 0 and so a success rate of ~1
// on one degree of freedom.  Without the maturity requirement a meaningless
// variance buys a fix.
TEST(WlOnlyFfr, ImmatureArcIsRejectedDespiteTinySigma) {
    AmbiguityResolver r = makeResolver(/*min_arc_count=*/10);
    std::map<std::string, WlArcQuality> q{
        {"G01_L1_L2", WlArcQuality{1e-9, 2, false}}};   // sigma ~ 0, count 2
    EXPECT_DOUBLE_EQ(WlFfrTestAccess::rate(r, {"G01_L1_L2"}, q, nullptr), 0.0);

    // The SAME sigma on a mature arc is a decided integer and saturates at 1.
    std::map<std::string, WlArcQuality> mature{
        {"G01_L1_L2", WlArcQuality{1e-9, 40, false}}};
    EXPECT_NEAR(WlFfrTestAccess::rate(r, {"G01_L1_L2"}, mature, nullptr), 1.0, 1e-9);
}

// --- A zero-sigma MATURE arc must not divide by zero ----------------------
TEST(WlOnlyFfr, ExactlyZeroSigmaOnMatureArcSaturatesAtOne) {
    AmbiguityResolver r = makeResolver();
    std::map<std::string, WlArcQuality> q{
        {"G01_L1_L2", WlArcQuality{0.0, 40, false}}};
    const double got = WlFfrTestAccess::rate(r, {"G01_L1_L2"}, q, nullptr);
    EXPECT_TRUE(std::isfinite(got));
    EXPECT_DOUBLE_EQ(got, 1.0);
}

// --- A BSSD pivot is exact by construction, maturity notwithstanding ------
TEST(WlOnlyFfr, ExactPivotContributesOneRegardlessOfCount) {
    AmbiguityResolver r = makeResolver(/*min_arc_count=*/10);
    std::map<std::string, WlArcQuality> q{
        {"G01_L1_L2", WlArcQuality{0.0, 0, /*exact=*/true}},
        {"G02_L1_L2", WlArcQuality{0.06, 40, false}},
    };
    const double got = WlFfrTestAccess::rate(r, {"G01_L1_L2", "G02_L1_L2"}, q, nullptr);
    EXPECT_NEAR(got, arcTerm(0.06), 1e-12);   // pivot term is exactly 1
}

// --- The weakest arc is identified for backward elimination ---------------
// Reported even when the product is already 0, because the caller needs the
// TRUE worst arc to drop, not whichever zero came first in key order.
TEST(WlOnlyFfr, WorstArcIsTheLowestContributorNotTheFirstKey) {
    AmbiguityResolver r = makeResolver();
    std::map<std::string, WlArcQuality> q{
        {"A_L1_L2", WlArcQuality{0.02, 40, false}},   // best
        {"B_L1_L2", WlArcQuality{0.30, 40, false}},   // worst (largest sigma)
        {"C_L1_L2", WlArcQuality{0.10, 40, false}},
    };
    std::string worst;
    WlFfrTestAccess::rate(r, {"A_L1_L2", "B_L1_L2", "C_L1_L2"}, q, &worst);
    EXPECT_EQ(worst, "B_L1_L2");
}

// --- A non-finite sigma is unknown, not perfect ---------------------------
TEST(WlOnlyFfr, NonFiniteSigmaIsRejected) {
    AmbiguityResolver r = makeResolver();
    std::map<std::string, WlArcQuality> q{
        {"G01_L1_L2", WlArcQuality{std::numeric_limits<double>::quiet_NaN(), 40, false}}};
    EXPECT_DOUBLE_EQ(WlFfrTestAccess::rate(r, {"G01_L1_L2"}, q, nullptr), 0.0);
}

// --- An empty arc set cannot be "certainly correct" ------------------------
TEST(WlOnlyFfr, EmptyArcSetIsZero) {
    AmbiguityResolver r = makeResolver();
    EXPECT_DOUBLE_EQ(WlFfrTestAccess::rate(r, {}, {}, nullptr), 0.0);
}

// --- The realistic case the field hit: a marginal arc drags the set under --
// One 0.25-cycle arc among four good ones is enough to fail a 0.999 target,
// which is the behaviour that stops the 35 m fix.
TEST(WlOnlyFfr, OneMarginalArcFailsA999Target) {
    AmbiguityResolver r = makeResolver();
    std::map<std::string, WlArcQuality> q{
        {"A_L1_L2", WlArcQuality{0.03, 40, false}},
        {"B_L1_L2", WlArcQuality{0.03, 40, false}},
        {"C_L1_L2", WlArcQuality{0.03, 40, false}},
        {"D_L1_L2", WlArcQuality{0.25, 40, false}},   // the marginal one
    };
    std::string worst;
    const double p = WlFfrTestAccess::rate(
        r, {"A_L1_L2", "B_L1_L2", "C_L1_L2", "D_L1_L2"}, q, &worst);
    EXPECT_LT(p, 0.999);
    EXPECT_EQ(worst, "D_L1_L2");

    // Drop it and the survivors clear the bar — the point of eliminating the
    // weakest arc rather than refusing the whole set.
    const double p_after =
        WlFfrTestAccess::rate(r, {"A_L1_L2", "B_L1_L2", "C_L1_L2"}, q, nullptr);
    EXPECT_GT(p_after, 0.999);
}
