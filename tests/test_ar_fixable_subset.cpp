/**
 * @file test_ar_fixable_subset.cpp
 * @brief Hand-worked cases for the fixable-subset selection of user integer
 *        fixing (ils::selectFixableSubset, filter/IlsCore.hpp).
 *
 * When the full set of ambiguities fails the acceptance battery and partial
 * fixing is on, the resolver retries on smaller subsets: after a failed MLAMBDA
 * call, a non-positive squared distance, or a failed ratio, upper-bound or
 * bootstrap gate it drops the entry with the largest
 * float variance, after a failed per-entry fraction cap it drops the entry
 * farthest from its integer, and it repeats down to a single entry.  Which
 * entry goes first decides which integers are fixed, so each rule is pinned on
 * a problem where the other rule would end on a different subset.
 *
 * All covariances are diagonal, so every squared distance is a sum of
 * (float − integer)² / variance and every expected number is worked out in
 * the comments.  The resolver's own member (AmbiguityResolver's private
 * selectFixableSubset_) only copies its configuration into the gate struct and
 * forwards here, so the free function is the code under test.
 *
 * Suite name @c ArFixableSubset is owned solely by this file.
 */

#include <gtest/gtest.h>

#include "filter/IlsCore.hpp"

#include <Eigen/Dense>

#include <cmath>
#include <initializer_list>
#include <limits>
#include <vector>

namespace GnssProcessor {
namespace {

// Per-dimension bootstrap term, written out independently of the code under
// test: P(|N(0, d)| < 0.5) = erf(0.5 / √(2·d)).
double bootstrapTerm(double d) {
    return std::erf(0.5 / std::sqrt(2.0 * d));
}

// Default user gates with partial fixing on, spelled out so that a change of
// the struct defaults does not silently change these cases.
ils::IlsGateConfig gates() {
    ils::IlsGateConfig g;
    g.ratio_threshold      = 3.0;
    g.ratio_upper_bound    = 1e4;
    g.min_success_rate     = 0.999;
    g.nl_max_per_pair_frac = 0.15;
    g.enable_partial_ar    = true;
    return g;
}

Eigen::MatrixXd diag(std::initializer_list<double> v) {
    const Eigen::Index n = static_cast<Eigen::Index>(v.size());
    Eigen::MatrixXd Q = Eigen::MatrixXd::Zero(n, n);
    Eigen::Index i = 0;
    for (double x : v) { Q(i, i) = x; ++i; }
    return Q;
}

Eigen::VectorXd vec(std::initializer_list<double> v) {
    Eigen::VectorXd a(static_cast<Eigen::Index>(v.size()));
    Eigen::Index i = 0;
    for (double x : v) a(i++) = x;
    return a;
}

// ---------------------------------------------------------------------------
// Everything passes
// ---------------------------------------------------------------------------

// Three floats 0.05, 0.04 and 0.02 cycle from their integers, variance 0.01
// each.  s1 = (0.0025 + 0.0016 + 0.0004) / 0.01 = 0.45; the cheapest second
// candidate moves entry 0 (+0.90 / 0.01 = +90), so s2 = 90.45 and the ratio is
// 201.  Every gate passes on the full set, which is returned whole.
TEST(ArFixableSubset, AllEntriesPassKeepsTheFullSet) {
    const Eigen::VectorXd a = vec({1.05, -2.04, 3.02});
    const Eigen::MatrixXd Q = diag({0.01, 0.01, 0.01});

    const ils::PartialFix pf = ils::selectFixableSubset(a, Q, gates());

    ASSERT_TRUE(pf.success);
    EXPECT_EQ(pf.kept, (std::vector<int>{0, 1, 2}));
    ASSERT_EQ(pf.afix_cycles.size(), 3);
    EXPECT_DOUBLE_EQ(pf.afix_cycles(0), 1.0);
    EXPECT_DOUBLE_EQ(pf.afix_cycles(1), -2.0);
    EXPECT_DOUBLE_EQ(pf.afix_cycles(2), 3.0);
    EXPECT_NEAR(pf.ratio, 90.45 / 0.45, 1e-6);
    EXPECT_NEAR(pf.p_s, std::pow(bootstrapTerm(0.01), 3), 1e-12);
    EXPECT_NEAR(pf.max_frac, 0.05, 1e-12);
}

// ---------------------------------------------------------------------------
// Shrinking to one entry
// ---------------------------------------------------------------------------

// Entries 0 and 1 sit near half a cycle (ratios 1.14 and 1.27 on the sets that
// contain them); entry 2 is 0.03 cycle from 5 with variance 0.0025.  The loop
// drops entry 0, then entry 1, and accepts entry 2 alone: s1 = 0.03² / 0.0025,
// s2 = 0.97² / 0.0025, ratio 1045.4, rate one, fraction 0.03.  The scalar
// fields describe the lone accepted entry, not the full set.
TEST(ArFixableSubset, ShrinksToASingleEntry) {
    const Eigen::VectorXd a = vec({0.45, 0.47, 5.03});
    const Eigen::MatrixXd Q = diag({0.01, 0.01, 0.0025});

    const ils::PartialFix pf = ils::selectFixableSubset(a, Q, gates());

    ASSERT_TRUE(pf.success);
    EXPECT_EQ(pf.kept, (std::vector<int>{2}));
    ASSERT_EQ(pf.afix_cycles.size(), 1);
    EXPECT_DOUBLE_EQ(pf.afix_cycles(0), 5.0);
    EXPECT_NEAR(pf.ratio, (0.97 * 0.97) / (0.03 * 0.03), 1e-6);
    EXPECT_NEAR(pf.p_s, 1.0, 1e-12);
    EXPECT_NEAR(pf.max_frac, 0.03, 1e-12);
}

// ---------------------------------------------------------------------------
// Order of elimination
// ---------------------------------------------------------------------------

// A failed ratio drops the entry with the largest variance, even when it is
// not the cause.  Entry 0 (0.45 cycle off, variance 0.01) spoils the ratio:
// full set s1 = 20.25 + 0.02 + 0.09 = 20.36, s2 = 30.36, ratio 1.49.  The
// largest variance is entry 1 (0.02), so entry 1 goes first.  On {0, 2} the
// ratio still fails and the two variances tie at 0.01; the first in order
// (entry 0) goes.  Entry 2 is accepted alone.  Had the culprit been dropped
// first, {1, 2} would have passed (checked below on that sub-problem).
TEST(ArFixableSubset, RatioFailureDropsTheLargestVarianceFirst) {
    const Eigen::VectorXd a = vec({0.45, 4.02, -3.03});
    const Eigen::MatrixXd Q = diag({0.01, 0.02, 0.01});

    const ils::PartialFix pf = ils::selectFixableSubset(a, Q, gates());

    ASSERT_TRUE(pf.success);
    EXPECT_EQ(pf.kept, (std::vector<int>{2}));
    ASSERT_EQ(pf.afix_cycles.size(), 1);
    EXPECT_DOUBLE_EQ(pf.afix_cycles(0), -3.0);
    EXPECT_NEAR(pf.ratio, (0.97 * 0.97) / (0.03 * 0.03), 1e-6);

    // {1, 2} on its own: s1 = 0.02 + 0.09 = 0.11, s2 = 0.11 + 48 = 48.11
    // (ratio 437), rate erf-term(0.02) × erf-term(0.01) = 0.99959 >= 0.999,
    // worst fraction 0.03.  It passes whole.
    const Eigen::VectorXd a12 = vec({4.02, -3.03});
    const Eigen::MatrixXd Q12 = diag({0.02, 0.01});
    const ils::PartialFix pf12 = ils::selectFixableSubset(a12, Q12, gates());
    ASSERT_TRUE(pf12.success);
    EXPECT_EQ(pf12.kept, (std::vector<int>{0, 1}));
    EXPECT_NEAR(pf12.ratio, 48.11 / 0.11, 1e-6);
    EXPECT_NEAR(pf12.p_s, bootstrapTerm(0.02) * bootstrapTerm(0.01), 1e-12);
}

// A failed fraction cap drops the entry farthest from its integer, not the
// one with the largest variance.  Full set: s1 = 0.125 + 4 + 0.04 = 4.165,
// s2 = 4.165 + 45 = 49.165 (ratio 11.8), rate 0.99959 (passes), but entry 1
// is 0.20 cycle from -6, above the 0.15 cap.  Entry 1 goes, and {0, 2} passes:
// s1 = 0.165, s2 = 45.165, rate erf-term(0.02), worst fraction 0.05.  Dropping
// by variance instead (entry 0) would have ended on {2}.  The kept indices
// stay in input order and the integers follow them.
TEST(ArFixableSubset, FractionFailureDropsTheFarthestEntry) {
    const Eigen::VectorXd a = vec({4.05, -6.20, 11.01});
    const Eigen::MatrixXd Q = diag({0.02, 0.01, 0.0025});

    const ils::PartialFix pf = ils::selectFixableSubset(a, Q, gates());

    ASSERT_TRUE(pf.success);
    EXPECT_EQ(pf.kept, (std::vector<int>{0, 2}));
    ASSERT_EQ(pf.afix_cycles.size(), 2);
    EXPECT_DOUBLE_EQ(pf.afix_cycles(0), 4.0);
    EXPECT_DOUBLE_EQ(pf.afix_cycles(1), 11.0);
    EXPECT_NEAR(pf.ratio, 45.165 / 0.165, 1e-6);
    EXPECT_NEAR(pf.p_s, bootstrapTerm(0.02) * bootstrapTerm(0.0025), 1e-12);
    EXPECT_NEAR(pf.max_frac, 0.05, 1e-12);

    // Without the far entry the full set would have passed directly: the
    // fraction cap was the only failing gate.
    ils::IlsGateConfig loose = gates();
    loose.nl_max_per_pair_frac = 0.25;
    const ils::PartialFix pf_loose = ils::selectFixableSubset(a, Q, loose);
    ASSERT_TRUE(pf_loose.success);
    EXPECT_EQ(pf_loose.kept, (std::vector<int>{0, 1, 2}));
    EXPECT_NEAR(pf_loose.ratio, 49.165 / 4.165, 1e-6);
    EXPECT_NEAR(pf_loose.max_frac, 0.20, 1e-12);
}

// ---------------------------------------------------------------------------
// Nothing passes
// ---------------------------------------------------------------------------

// Both floats sit near half a cycle: full set ratio 47.29 / 43.29 = 1.09, then
// entry 1 alone 27.04 / 23.04 = 1.17.  No subset clears the ratio, the loop
// empties the set and reports no fix with empty fields.
TEST(ArFixableSubset, NothingPassesLeavesNoFix) {
    const Eigen::VectorXd a = vec({0.45, -1.48});
    const Eigen::MatrixXd Q = diag({0.01, 0.01});

    const ils::PartialFix pf = ils::selectFixableSubset(a, Q, gates());

    EXPECT_FALSE(pf.success);
    EXPECT_TRUE(pf.kept.empty());
    EXPECT_EQ(pf.afix_cycles.size(), 0);
    EXPECT_DOUBLE_EQ(pf.ratio, 0.0);
    EXPECT_DOUBLE_EQ(pf.p_s, 0.0);
    EXPECT_DOUBLE_EQ(pf.max_frac, 0.0);
}

// Floats close to integers but too loose for the rate: variance 0.1 gives a
// term of 0.886 per entry, so the full set (0.785) and each single entry fail
// 0.999 although every ratio passes (311 and 361).  Nothing is fixed.  With
// the rate lowered to 0.5 the full set passes, which shows the bootstrap gate
// was the one refusing.
TEST(ArFixableSubset, BootstrapFailureOnEverySubsetLeavesNoFix) {
    const Eigen::VectorXd a = vec({2.02, -0.05});
    const Eigen::MatrixXd Q = diag({0.1, 0.1});

    const ils::PartialFix pf = ils::selectFixableSubset(a, Q, gates());
    EXPECT_FALSE(pf.success);
    EXPECT_TRUE(pf.kept.empty());

    ils::IlsGateConfig lax = gates();
    lax.min_success_rate = 0.5;
    const ils::PartialFix pf_lax = ils::selectFixableSubset(a, Q, lax);
    ASSERT_TRUE(pf_lax.success);
    EXPECT_EQ(pf_lax.kept, (std::vector<int>{0, 1}));
    EXPECT_NEAR(pf_lax.ratio, 9.029 / 0.029, 1e-6);
    EXPECT_NEAR(pf_lax.p_s, bootstrapTerm(0.1) * bootstrapTerm(0.1), 1e-12);
}

// Floats exactly on integers give a zero best distance on every subset, which
// the selection treats like a failed search: every entry is dropped and
// nothing is fixed, even with no upper bound on the ratio.  The same floats
// moved 0.02 cycle are fixed whole (ratio 96.04 / 0.04 = 2401).  This pins
// the current rule; accepting exact integers would be a deliberate change.
TEST(ArFixableSubset, ExactlyIntegerFloatsAreNotFixed) {
    const Eigen::MatrixXd Q = diag({0.01, 0.01});

    ils::IlsGateConfig unbounded = gates();
    unbounded.ratio_upper_bound = std::numeric_limits<double>::infinity();
    const ils::PartialFix exact =
        ils::selectFixableSubset(vec({3.0, -1.0}), Q, unbounded);
    EXPECT_FALSE(exact.success);
    EXPECT_TRUE(exact.kept.empty());

    const ils::PartialFix offset =
        ils::selectFixableSubset(vec({3.02, -1.0}), Q, gates());
    ASSERT_TRUE(offset.success);
    EXPECT_EQ(offset.kept, (std::vector<int>{0, 1}));
    EXPECT_DOUBLE_EQ(offset.afix_cycles(0), 3.0);
    EXPECT_DOUBLE_EQ(offset.afix_cycles(1), -1.0);
    EXPECT_NEAR(offset.ratio, 96.04 / 0.04, 1e-6);
}

// An empty input is refused without searching.
TEST(ArFixableSubset, EmptyInputFindsNothing) {
    const ils::PartialFix pf =
        ils::selectFixableSubset(Eigen::VectorXd(), Eigen::MatrixXd(), gates());
    EXPECT_FALSE(pf.success);
    EXPECT_TRUE(pf.kept.empty());
}

}  // namespace
}  // namespace GnssProcessor
