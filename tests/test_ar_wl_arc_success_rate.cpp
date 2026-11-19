/**
 * @file test_ar_wl_arc_success_rate.cpp
 * @brief Edge cases of the wide-lane arc success rate (AmbiguityResolver::wlArcSuccessRate_),
 *        the acceptance number of the wide-lane-only fixing tier.
 *
 * The rate is a product over the arcs backing a constraint set of erf(1/(2 sqrt(2) sigma)),
 * with every arc whose failure rate cannot be bounded contributing 0, and it also names the
 * weakest arc for backward elimination.  The neighbouring file pins the main fail-safe rows
 * (empty set, missing key, immature arc, zero and non-finite sigma, exact pivot).  This file
 * pins the edges those rows leave open: a negative or infinite sigma, the maturity bar at
 * exactly the minimum count, a negative minimum count, an exact pivot whatever its other
 * fields say, quality entries outside the set, and which key is named as the weakest when
 * the set is empty, perfect or tied.
 *
 * Suite name @c ArWlArcSuccessRate is owned solely by this file.
 */

#include <gtest/gtest.h>

#include "filter/AmbiguityResolver.hpp"
#include "logging/LogManager.hpp"

#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>

namespace GnssProcessor {

// The access shim is befriended in AmbiguityResolver.hpp and is also defined in
// tests/test_wl_only_ffr.cpp.  One type defined in several translation units must be
// token for token the same in each, so this is an exact copy: change both or neither.

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

/// A resolver whose only relevant setting is the maturity bar, with its log manager kept
/// alive for as long as the resolver (the resolver holds a reference to it).
struct Gate {
    LogManager        log;
    AmbiguityResolver resolver;
    explicit Gate(int min_arc_count) : resolver(configOf(min_arc_count), log) {}
    static AmbiguityResolverConfig configOf(int min_arc_count) {
        AmbiguityResolverConfig cfg;
        cfg.wl_only_ffr           = true;
        cfg.wl_only_min_arc_count = min_arc_count;
        return cfg;
    }
    double rate(const std::set<std::string>& arcs,
                const std::map<std::string, WlArcQuality>& q, std::string* worst = nullptr) const {
        return WlFfrTestAccess::rate(resolver, arcs, q, worst);
    }
};

/// The per-arc term, written out from its definition.
double arcTerm(double sigma_cycles) { return std::erf(1.0 / (2.0 * std::sqrt(2.0) * sigma_cycles)); }

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

} // namespace

TEST(ArWlArcSuccessRate, ANegativeSigmaOnAMatureArcContributesZeroAndIsNamedTheWeakest) {
    // A negative sigma is no variance at all; read as perfect it would buy a fix.
    Gate g(10);
    const std::map<std::string, WlArcQuality> q{
        { "G01_L1_L2", WlArcQuality{ 0.05, 40, false } },
        { "G02_L1_L2", WlArcQuality{ -0.05, 40, false } } };
    std::string worst;
    EXPECT_DOUBLE_EQ(g.rate({ "G01_L1_L2", "G02_L1_L2" }, q, &worst), 0.0);
    EXPECT_EQ(worst, "G02_L1_L2");
    // Even the smallest negative number.
    const std::map<std::string, WlArcQuality> tiny{
        { "G01_L1_L2", WlArcQuality{ -std::numeric_limits<double>::denorm_min(), 40, false } } };
    EXPECT_DOUBLE_EQ(g.rate({ "G01_L1_L2" }, tiny), 0.0);
}

TEST(ArWlArcSuccessRate, AnInfiniteSigmaContributesZero) {
    Gate g(10);
    const std::map<std::string, WlArcQuality> q{
        { "G01_L1_L2", WlArcQuality{ kInf, 40, false } } };
    EXPECT_DOUBLE_EQ(g.rate({ "G01_L1_L2" }, q), 0.0);
}

TEST(ArWlArcSuccessRate, TheMaturityBarIsReachedAtExactlyTheMinimumCount) {
    Gate g(10);
    const std::map<std::string, WlArcQuality> at{
        { "G01_L1_L2", WlArcQuality{ 0.08, 10, false } } };
    EXPECT_NEAR(g.rate({ "G01_L1_L2" }, at), arcTerm(0.08), 1e-12);
    const std::map<std::string, WlArcQuality> below{
        { "G01_L1_L2", WlArcQuality{ 0.08, 9, false } } };
    EXPECT_DOUBLE_EQ(g.rate({ "G01_L1_L2" }, below), 0.0);
}

TEST(ArWlArcSuccessRate, ANegativeMinimumCountIsReadAsZero) {
    // The minimum is clamped at 0 before the comparison with the unsigned sample count, so a
    // negative setting admits every arc rather than wrapping to a huge bar that admits none.
    Gate g(-5);
    const std::map<std::string, WlArcQuality> q{
        { "G01_L1_L2", WlArcQuality{ 0.08, 0, false } } };
    EXPECT_NEAR(g.rate({ "G01_L1_L2" }, q), arcTerm(0.08), 1e-12);
    // With a minimum of 0 the same arc of no samples is admitted too.
    Gate zero(0);
    EXPECT_NEAR(zero.rate({ "G01_L1_L2" }, q), arcTerm(0.08), 1e-12);
}

TEST(ArWlArcSuccessRate, AnExactPivotContributesOneWhateverItsSigmaAndCount) {
    Gate g(10);
    for (const double sigma : { kNaN, -1.0, kInf, 5.0 }) {
        const std::map<std::string, WlArcQuality> q{
            { "G01_L1_L2", WlArcQuality{ sigma, 0, true } } };
        EXPECT_DOUBLE_EQ(g.rate({ "G01_L1_L2" }, q), 1.0) << "sigma " << sigma;
    }
}

TEST(ArWlArcSuccessRate, QualityEntriesOutsideTheSetDoNotEnter) {
    Gate g(10);
    const std::map<std::string, WlArcQuality> q{
        { "G01_L1_L2", WlArcQuality{ 0.07, 40, false } },
        { "G02_L1_L2", WlArcQuality{ 0.0, 1, false } },        // immature, but not in the set
        { "G03_L1_L2", WlArcQuality{ kNaN, 40, false } } };    // unbounded, but not in the set
    std::string worst;
    EXPECT_NEAR(g.rate({ "G01_L1_L2" }, q, &worst), arcTerm(0.07), 1e-12);
    EXPECT_EQ(worst, "G01_L1_L2");
}

TEST(ArWlArcSuccessRate, AnEmptySetReturnsZeroAndClearsAStaleWeakestKey) {
    Gate g(10);
    std::string worst = "stale";
    const std::map<std::string, WlArcQuality> q{
        { "G01_L1_L2", WlArcQuality{ 0.01, 40, false } } };
    EXPECT_DOUBLE_EQ(g.rate({}, q, &worst), 0.0);
    EXPECT_TRUE(worst.empty()) << worst;
}

TEST(ArWlArcSuccessRate, APerfectSetNamesTheFirstKeyInOrderAsTheWeakest) {
    // Every term is 1: the weakest is the first key of the ordered set, since only a strictly
    // lower term replaces it.
    Gate g(10);
    const std::map<std::string, WlArcQuality> q{
        { "E07_L1_L5", WlArcQuality{ 0.0, 0, true } },
        { "G04_L1_L2", WlArcQuality{ 0.0, 40, false } },
        { "G09_L1_L2", WlArcQuality{ 0.0, 0, true } } };
    std::string worst = "stale";
    EXPECT_DOUBLE_EQ(g.rate({ "G09_L1_L2", "G04_L1_L2", "E07_L1_L5" }, q, &worst), 1.0);
    EXPECT_EQ(worst, "E07_L1_L5");
}

TEST(ArWlArcSuccessRate, AmongTiedZeroArcsTheFirstInOrderIsNamed) {
    // Two arcs without a quality entry and one immature arc all contribute 0; the first in
    // key order is named, and a later good arc does not lift the product.
    Gate g(10);
    const std::map<std::string, WlArcQuality> q{
        { "G05_L1_L2", WlArcQuality{ 0.01, 3, false } },        // immature
        { "G08_L1_L2", WlArcQuality{ 0.02, 40, false } } };     // good
    std::string worst;
    EXPECT_DOUBLE_EQ(g.rate({ "G08_L1_L2", "G07_L1_L2", "G05_L1_L2", "G06_L1_L2" }, q, &worst), 0.0);
    EXPECT_EQ(worst, "G05_L1_L2");
}

TEST(ArWlArcSuccessRate, TheRateStaysInTheUnitIntervalAndFallsAsSigmaGrows) {
    Gate g(10);
    double previous = 1.0;
    for (const double sigma : { 0.01, 0.05, 0.1, 0.2, 0.5, 1.0, 5.0 }) {
        const std::map<std::string, WlArcQuality> q{
            { "G01_L1_L2", WlArcQuality{ sigma, 40, false } } };
        const double p = g.rate({ "G01_L1_L2" }, q);
        EXPECT_GT(p, 0.0) << sigma;
        EXPECT_LE(p, previous) << sigma;
        previous = p;
    }
    EXPECT_LT(previous, 0.1);   // erf(1/(2 sqrt(2) 5)) is about 0.08
}
