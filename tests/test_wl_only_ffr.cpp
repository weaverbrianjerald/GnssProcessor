/**
 * @file test_wl_only_ffr.cpp
 * @brief The WL-only AR acceptance gate (@ref AmbiguityResolverConfig::wl_only_ffr),
 *        card Q3 (§8.11 of the card).
 *
 * The WL-only / MW-WL-constraint paths assert MW wide-lane integers directly: there
 * is no LAMBDA search, so no ratio test, and without this gate no success-rate gate
 * either, only the `wl_max_frac` point test on each arc's running residual.  A point
 * test cannot bound P(the integer is wrong), which is what an acceptance decision has
 * to control.  Without the gate a wrong fix passes unflagged: an ungated 5-constraint
 * fix placed a held-out consumer 35 m off while `ratio` reported the
 * "+inf = no LAMBDA ran" sentinel.
 *
 * The tests pin the fail-safe behaviour, because a field run cannot demonstrate it: a
 * clean run shows only that nothing went wrong, not that the guard would have fired.
 */
#include <gtest/gtest.h>

#include "filter/AmbiguityResolver.hpp"
#include "filter/IlsCore.hpp"

#include <cmath>
#include <map>
#include <set>
#include <string>

namespace GnssProcessor {

/// Befriended in AmbiguityResolver.hpp; forwards to the private gate.  It has to be
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

// --- The gate agrees with the LAMBDA tier's own functional ----------------
// Both tiers report one comparable number; otherwise "p_succ" would mean something
// different depending on which machinery fixed, which is the ambiguity this card
// removes.
TEST(WlOnlyFfr, PerArcTermMatchesTheLambdaTierBootstrap) {
    AmbiguityResolver r = makeResolver();
    const double sigma = 0.08;                       // cycles
    std::map<std::string, WlArcQuality> q{
        {"G01_L1_L2", WlArcQuality{sigma, 50, false}}};
    const double got = WlFfrTestAccess::rate(r, {"G01_L1_L2"}, q, nullptr);

    // ils::computeBootstrapSuccessRate takes variances; one arc of variance
    // sigma^2 gives the identical number.
    Eigen::VectorXd d(1);
    d(0) = sigma * sigma;
    EXPECT_NEAR(got, ils::computeBootstrapSuccessRate(d), 1e-12);
    EXPECT_NEAR(got, arcTerm(sigma), 1e-12);
}

// --- The product runs over distinct arcs ----------------------------------
TEST(WlOnlyFfr, ProductIsOverDistinctArcs) {
    AmbiguityResolver r = makeResolver();
    std::map<std::string, WlArcQuality> q{
        {"G01_L1_L2", WlArcQuality{0.05, 40, false}},
        {"G02_L1_L2", WlArcQuality{0.09, 40, false}},
    };
    const double got = WlFfrTestAccess::rate(r, {"G01_L1_L2", "G02_L1_L2"}, q, nullptr);
    EXPECT_NEAR(got, arcTerm(0.05) * arcTerm(0.09), 1e-12);
}

// --- Fail-safe: an arc that cannot be bounded contributes 0, not 1 --------
// "No quality entry" means the arc's failure rate is unknown.  Treating unknown as
// fine is the defect this gate closes.
TEST(WlOnlyFfr, MissingArcQualityIsZeroNotOne) {
    AmbiguityResolver r = makeResolver();
    std::map<std::string, WlArcQuality> q{
        {"G01_L1_L2", WlArcQuality{0.01, 100, false}}};
    std::string worst;
    const double got = WlFfrTestAccess::rate(r, {"G01_L1_L2", "G99_L1_L2"}, q, &worst);
    EXPECT_DOUBLE_EQ(got, 0.0);
    EXPECT_EQ(worst, "G99_L1_L2");   // and it is named, so elimination can drop it
}

// --- Fail-safe: an immature arc contributes zero even with a tiny sigma ----
// Two samples that happen to agree give sigma ~ 0 and so a success rate of ~1
// on one degree of freedom.  Without the maturity requirement a meaningless
// variance buys a fix.
TEST(WlOnlyFfr, ImmatureArcIsRejectedDespiteTinySigma) {
    AmbiguityResolver r = makeResolver(/*min_arc_count=*/10);
    std::map<std::string, WlArcQuality> q{
        {"G01_L1_L2", WlArcQuality{1e-9, 2, false}}};   // sigma ~ 0, count 2
    EXPECT_DOUBLE_EQ(WlFfrTestAccess::rate(r, {"G01_L1_L2"}, q, nullptr), 0.0);

    // The same sigma on a mature arc is a decided integer and saturates at 1.
    std::map<std::string, WlArcQuality> mature{
        {"G01_L1_L2", WlArcQuality{1e-9, 40, false}}};
    EXPECT_NEAR(WlFfrTestAccess::rate(r, {"G01_L1_L2"}, mature, nullptr), 1.0, 1e-9);
}

// --- A zero-sigma mature arc does not divide by zero ----------------------
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
// true worst arc to drop, not whichever zero came first in key order.
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

// --- The field case: one marginal arc pulls the set below target ----------
// One 0.25-cycle arc among three good ones is enough to fail a 0.999 target
// (its own term is erf(1/(2√2·0.25)) ≈ 0.954); this is the behaviour that stops
// the 35 m fix.
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

    // With it dropped, the survivors clear the bar: the reason for eliminating the
    // weakest arc rather than refusing the whole set.
    const double p_after =
        WlFfrTestAccess::rate(r, {"A_L1_L2", "B_L1_L2", "C_L1_L2"}, q, nullptr);
    EXPECT_GT(p_after, 0.999);
}

// --- The default-on contract ----------------------------------------------
// Both guards default to on because they close a fail-unsafe path, and both are
// reversible.  An edit that flipped a default back would reopen that path with
// nothing in the logs to show it, so the contract is pinned here rather than left
// to review.
TEST(WlOnlyFfr, GuardsAreOnByDefaultAndReversible) {
    AmbiguityResolverConfig cfg;                     // untouched defaults
    EXPECT_TRUE(cfg.wl_only_ffr)
        << "the WL-only bootstrap FFR gate must default ON";
    EXPECT_GT(cfg.wl_only_max_innov_cyc, 0.0)
        << "the innovation screen must default ON (0 disables it)";
    EXPECT_DOUBLE_EQ(cfg.wl_only_max_innov_cyc, 0.5)
        << "half a widelane: the largest disagreement that can still be a "
           "rounding question rather than a frame question";
    EXPECT_GE(cfg.wl_only_min_arc_count, 2)
        << "a sigma from fewer than 2 samples has no degrees of freedom";
    EXPECT_DOUBLE_EQ(cfg.min_success_rate, 0.999)
        << "the WL tier reuses the LAMBDA tier's FFR target — one number, not two";
}

// --- The innovation screen's own arithmetic, at the boundary ---------------
// The screen itself is |asserted - predicted| > cap, applied identically in
// resolveWlOnly and applyMwWlConstraintsNlWl_; this test restates it in a local
// lambda to pin the semantics: the cap is on the disagreement, so a constraint
// exactly on the cap is kept and one beyond it is dropped, symmetrically in sign.
TEST(WlOnlyFfr, InnovationScreenIsSymmetricAndInclusiveAtTheCap) {
    const double cap = 0.5;
    auto dropped = [&](double asserted, double predicted) {
        return std::abs(asserted - predicted) > cap;
    };
    EXPECT_FALSE(dropped(7.0, 7.0));        // exact agreement
    EXPECT_FALSE(dropped(7.0, 6.5));        // exactly at the cap, either sign
    EXPECT_FALSE(dropped(7.0, 7.5));
    EXPECT_TRUE (dropped(7.0, 6.49));       // just beyond
    EXPECT_TRUE (dropped(7.0, 7.51));
    EXPECT_TRUE (dropped(7.0, 2.0));        // the frame-conflict scale
}
