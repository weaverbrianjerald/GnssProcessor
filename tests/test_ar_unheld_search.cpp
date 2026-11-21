/**
 * @file test_ar_unheld_search.cpp
 * @brief Unit tests of the search over the not-yet-held candidates
 *        (@c AmbiguityResolverConfig::search_unheld_only).
 *
 * The cases:
 *   - every candidate held: no search (ending all_held), nothing written;
 *   - a new satellite whose float sits 0.005 cycle from its integer beside a held one:
 *     with the key off the joint search ends on the ratio upper bound; with it on the free
 *     candidate alone is searched, accepted, and only its two rows are written;
 *   - the upper bound still refuses a search whose conditioned covariance has a direction
 *     at hold scale (ending degenerate);
 *   - the free float and covariance are conditioned on the held candidates' integers
 *     (y_F|H = y_F - Q_FH Q_HH^-1 (y_H - z_H), Q_F|H = Q_FF - Q_FH Q_HH^-1 Q_HF);
 *   - the run counters and the run-end line.
 *
 * Suite name @c ArUnheldSearch is owned solely by this file.
 */

#include <gtest/gtest.h>

#include "support/ArWlcStageAccess.hpp"

#include <string>
#include <vector>

namespace GnssProcessor {

/// Test seam for the search over the not-yet-held candidates (friend in AmbiguityResolver.hpp).
struct ArUnheldSearchTestAccess {
    using Cand   = AmbiguityResolver::NlCandidate;
    using Pair   = AmbiguityResolver::DdPair;
    using Search = AmbiguityResolver::UnheldSearch;
    static AmbiguityResolution run(AmbiguityResolver& r, const std::vector<Pair>& pairs,
                                   const Eigen::VectorXd& x, const Eigen::MatrixXd& P,
                                   const std::vector<PppObservation>& obs) {
        return r.resolveWlConstrained(pairs, x, P, {}, obs);
    }
    static Search search(const AmbiguityResolver& r, const std::vector<Cand>& c,
                         const std::vector<Pair>& pairs, const Eigen::MatrixXd& P,
                         const Eigen::MatrixXd& T, const Eigen::VectorXd& y, const Eigen::MatrixXd& Q) {
        return r.searchUnheld_(c, pairs, P, T, y, Q);
    }
    static bool boundRejects(const AmbiguityResolver& r, const AmbiguityResolution& accepted) {
        return r.ratioBoundRejects_(accepted);
    }
};

} // namespace GnssProcessor

using namespace GnssProcessor;
using gnss_test::ArScene;
using gnss_test::ArStageRig;
using Access = ArUnheldSearchTestAccess;

namespace {

AmbiguityResolverConfig baseConfig(bool unheld) {
    AmbiguityResolverConfig c;
    c.ar_mode            = 1;
    c.wl_from_float      = true;
    c.hold_variance      = 1e-7;
    c.ratio_threshold    = 5.0;
    c.ratio_upper_bound  = 1e4;
    c.search_unheld_only = unheld;
    return c;
}

// G15 held (its states and the reference's at 1e-9 m2: DD variance 2e-9, under ten times
// the hold variance; float 0.00005 cycle from its integers) and G05 free (its states at
// 1e-5 m2, a narrow-lane sigma of about 0.011 cycle), both against G12.
ArScene heldAndFree(double f_free) {
    ArScene s("G12", { { "G15", 3.00005, 1.00005 }, { "G05", 7.0 + f_free, 2.0 + f_free } },
              System::GPS, "L1C", "L2W", gnss_test::kFL1, gnss_test::kFL2, 1e-5);
    for (int k : { 0, 1, 2, 3 }) s.P(k, k) = 1e-9;   // reference and G15
    return s;
}

TEST(ArUnheldSearch, EveryCandidateHeldSearchesNothingAndWritesNothing) {
    ArStageRig rig(baseConfig(true));
    ArScene s("G12", { { "G15", 3.0004, 1.0004 }, { "G05", 7.0002, 2.0002 } });
    s.P = Eigen::MatrixXd::Identity(s.P.rows(), s.P.cols()) * 1e-9;
    const AmbiguityResolution r = Access::run(*rig.resolver, s.pairs, s.x, s.P, s.obs);
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.ending, ArEnding::AllHeld);
    EXPECT_TRUE(r.fixed_dd_rows.empty());
    EXPECT_DOUBLE_EQ(r.search_ratio, 0.0);
    const UnheldSearchCounters& c = rig.resolver->unheldSearchCounters();
    EXPECT_EQ(c.searches, 1);
    EXPECT_EQ(c.all_held, 1);
    EXPECT_EQ(c.free_searched, 0);
    EXPECT_NE(rig.logText().find("UNHELD tow=345600.0 n_free=0 n_held=2"), std::string::npos);
}

TEST(ArUnheldSearch, ANewSatelliteNearItsIntegerJoinsWhereTheBoundRefusedTheJointSearch) {
    const ArScene s = heldAndFree(0.005);   // the free candidate alone has ratio (0.995/0.005)^2
    {
        ArStageRig off(baseConfig(false));
        const AmbiguityResolution r = Access::run(*off.resolver, s.pairs, s.x, s.P, s.obs);
        EXPECT_FALSE(r.success);
        EXPECT_EQ(r.ending, ArEnding::RatioHigh);
        EXPECT_GT(r.search_ratio, 1e4);
    }
    ArStageRig on(baseConfig(true));
    const AmbiguityResolution r = Access::run(*on.resolver, s.pairs, s.x, s.P, s.obs);
    ASSERT_TRUE(r.success);
    EXPECT_EQ(r.ending, ArEnding::Fixed);
    EXPECT_EQ(r.num_fixed, 1);
    EXPECT_GT(r.ratio, 1e4);   // above the bound, accepted: the conditioned problem is not degenerate
    // Rows for G05 only (its states 4 and 5 against the reference's 0 and 1).
    ASSERT_EQ(r.fixed_dd_rows.size(), 2u);
    EXPECT_EQ(r.fixed_dd_rows[0].state_idx_i, 4);
    EXPECT_EQ(r.fixed_dd_rows[1].state_idx_i, 5);
    const UnheldSearchCounters& c = on.resolver->unheldSearchCounters();
    EXPECT_EQ(c.free_searched, 1);
    EXPECT_EQ(c.fixed, 1);
    EXPECT_EQ(c.pairs_installed, 1);
    EXPECT_EQ(c.held_conditioning, 1);
    EXPECT_EQ(c.bound_would_fire, 1);
    EXPECT_EQ(c.degenerate, 0);
    const std::string text = on.logText();
    EXPECT_NE(text.find("NL pair G05-G12 L1C/L2W: N_1_float=7.005 N_1_fixed=7 dd_wl=5"), std::string::npos);
    EXPECT_NE(text.find("Unheld AR (WL/NL) succeeded"), std::string::npos);
    EXPECT_NE(on.resolver->unheldSearchRunEndLine().find(
                  "1 searches, 0 with every candidate held, 1 of the free set, 1 fixed (1 pairs installed, "
                  "1 held candidates conditioning them); the ratio upper bound would have refused 1, "
                  "0 refused as degenerate"),
              std::string::npos) << on.resolver->unheldSearchRunEndLine();
}

TEST(ArUnheldSearch, TheResolversOuterBoundLeavesTheUnheldSearchsVerdictStanding) {
    // resolveCore_ applies the ratio upper bound again to every accepted WL-constrained fix.
    // Under the key the search has applied it itself (only where degenerate), so the outer
    // check must not refuse the fix it accepted; without the key the outer check is unchanged.
    AmbiguityResolution accepted;
    accepted.success = true;
    accepted.ratio   = 39601.0;
    ArStageRig off(baseConfig(false));
    EXPECT_TRUE(Access::boundRejects(*off.resolver, accepted));
    ArStageRig on(baseConfig(true));
    EXPECT_FALSE(Access::boundRejects(*on.resolver, accepted));
    accepted.ratio = 50.0;
    EXPECT_FALSE(Access::boundRejects(*off.resolver, accepted));
}

TEST(ArUnheldSearch, TheBoundStillRefusesADegenerateSearch) {
    // G05's L1C DD at 1e-9 m2 (held on its own) and its L2W DD at 1.5e-6 m2 (above ten
    // times the hold variance): the pair is free, but its conditioned search variance
    // (about 2.4e-6 cycle^2) sits below the hold scale (about 1.2e-5), so the bound applies.
    ArScene s = heldAndFree(0.005);
    s.P(4, 4) = 0.5e-9;   // G05 L1C
    s.P(0, 0) = 0.5e-9;   // reference L1C
    s.P(5, 5) = 0.75e-6;  // G05 L2W
    s.P(1, 1) = 0.75e-6;  // reference L2W (G15's L2W DD stays held at 7.5e-7 m2)
    ArStageRig rig(baseConfig(true));
    const AmbiguityResolution r = Access::run(*rig.resolver, s.pairs, s.x, s.P, s.obs);
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.ending, ArEnding::Degenerate);
    EXPECT_EQ(rig.resolver->unheldSearchCounters().degenerate, 1);
    EXPECT_TRUE(r.fixed_dd_rows.empty());
}

TEST(ArUnheldSearch, TheFreeFloatIsConditionedOnTheHeldIntegers) {
    ArStageRig rig(baseConfig(true));
    // Candidate 0 held (states 2, 3 and the reference's 0, 1 at 1e-9), candidate 1 free.
    std::vector<Access::Pair> pairs = { { 2, 0, "G15", "G12", "L1C" }, { 3, 1, "G15", "G12", "L2W" },
                                        { 4, 0, "G05", "G12", "L1C" }, { 5, 1, "G05", "G12", "L2W" } };
    Eigen::MatrixXd P = Eigen::MatrixXd::Identity(6, 6) * 1e-3;
    for (int k : { 0, 1, 2, 3 }) P(k, k) = 1e-9;
    std::vector<Access::Cand> cands(2);
    cands[0].dd_idx_1 = 0; cands[0].dd_idx_2 = 1;
    cands[1].dd_idx_1 = 2; cands[1].dd_idx_2 = 3;
    Eigen::MatrixXd T = Eigen::MatrixXd::Zero(2, 4);
    T(0, 0) = T(0, 1) = T(1, 2) = T(1, 3) = 1.0;
    Eigen::VectorXd y(2);
    y << 5.004, 7.3;
    Eigen::MatrixXd Q(2, 2);
    Q << 0.0016, 0.0008,
         0.0008, 0.0100;
    const Access::Search us = Access::search(*rig.resolver, cands, pairs, P, T, y, Q);
    ASSERT_EQ(us.held.size(), 1u);
    ASSERT_EQ(us.free.size(), 1u);
    EXPECT_EQ(us.held[0], 0);
    EXPECT_EQ(us.free[0], 1);
    EXPECT_NEAR(us.y_cond(0), 7.3 - 0.0008 / 0.0016 * 0.004, 1e-12);    // 7.298
    EXPECT_NEAR(us.min_eig, 0.0100 - 0.0008 * 0.0008 / 0.0016, 1e-12);  // 0.0096
    EXPECT_NEAR(us.hold_scale, 10.0 * 1e-7 * 2.0, 1e-18);
    EXPECT_NEAR(us.ratio, (0.702 / 0.298) * (0.702 / 0.298), 1e-6);
    EXPECT_EQ(us.ending, ArEnding::PairFrac);                            // |7.298 - 7| > 0.15
    EXPECT_NEAR(us.max_frac, 0.298, 1e-12);
}

} // namespace
