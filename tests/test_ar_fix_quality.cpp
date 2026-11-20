/**
 * @file test_ar_fix_quality.cpp
 * @brief Unit tests of the user resolver's endings and held census, and of their trace
 *        columns.
 *
 * The cases:
 *   - every ending has its word, and only the search verdicts count as verdicts;
 *   - the held-scale rule: a DD variance at most ten times the hold variance is held;
 *   - the census counts held satellite pairs (two held signals needed) and the edges of
 *     the held set: joins, leaves while observed, leaves by absence, with HELDSET lines;
 *   - a change of reference satellite moves no satellite in or out of the held set;
 *   - the WL-constrained search reports its own ending and the ratio it computed, on
 *     success and on each refusal of the acceptance battery;
 *   - the trace writes the ending in ar_gate on a failed search and the five appended
 *     columns.
 *
 * A one-candidate search has the ratio ((1 - f) / f)^2 for a float f cycles from its
 * integer whatever its sigma, so each ending below is reached by the choice of f.
 *
 * Suite names @c ArFixQuality and @c ArFixQualityTrace are owned solely by this file.
 */

#include <gtest/gtest.h>

#include "realtime/ConsumerTrace.hpp"
#include "support/ArWlcStageAccess.hpp"

#include <sstream>
#include <string>
#include <vector>

namespace GnssProcessor {

/// Test seam for the held census and the endings (friend in AmbiguityResolver.hpp).
struct ArFixQualityTestAccess {
    static void census(AmbiguityResolver& r, const std::vector<AmbiguityResolver::DdPair>& pairs,
                       const Eigen::MatrixXd& P, const std::vector<PppObservation>& obs,
                       AmbiguityResolution& out) {
        r.censusHeldPairs_(pairs, P, obs, out);
    }
    static AmbiguityResolution search(AmbiguityResolver& r,
                                      const std::vector<AmbiguityResolver::DdPair>& pairs,
                                      const Eigen::VectorXd& x, const Eigen::MatrixXd& P,
                                      const std::vector<PppObservation>& obs) {
        return r.resolveWlConstrained(pairs, x, P, {}, obs);
    }
};

} // namespace GnssProcessor

using namespace GnssProcessor;
using gnss_test::ArScene;
using gnss_test::ArStageRig;
using Access = ArFixQualityTestAccess;

namespace {

AmbiguityResolverConfig baseConfig() {
    AmbiguityResolverConfig c;
    c.ar_mode           = 1;
    c.wl_from_float     = true;
    c.hold_variance     = 1e-7;
    c.ratio_threshold   = 5.0;
    c.ratio_upper_bound = 1e4;
    return c;
}

std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> out;
    std::string cell;
    std::istringstream in(line.substr(0, line.find('\n')));
    while (std::getline(in, cell, ',')) out.push_back(cell);
    return out;
}

TEST(ArFixQuality, EveryEndingHasItsWordAndOnlyVerdictsCountAsVerdicts) {
    EXPECT_STREQ(arEndingName(ArEnding::None), "none");
    EXPECT_STREQ(arEndingName(ArEnding::NoPairs), "no_candidates");
    EXPECT_STREQ(arEndingName(ArEnding::NoNlCandidate), "no_nl_candidate");
    EXPECT_STREQ(arEndingName(ArEnding::RatioLow), "ratio_low");
    EXPECT_STREQ(arEndingName(ArEnding::RatioHigh), "ratio_high");
    EXPECT_STREQ(arEndingName(ArEnding::SuccessRate), "success_rate");
    EXPECT_STREQ(arEndingName(ArEnding::PairFrac), "pair_frac");
    EXPECT_STREQ(arEndingName(ArEnding::AllHeld), "all_held");
    EXPECT_STREQ(arEndingName(ArEnding::FixedPartial), "fixed_partial");
    for (std::size_t k = 0; k < kArEndingCount; ++k)
        EXPECT_STRNE(arEndingName(static_cast<ArEnding>(k)), "?") << k;
    for (ArEnding e : { ArEnding::None, ArEnding::Disabled, ArEnding::MinEpochs,
                        ArEnding::SigmaGate, ArEnding::NoPairs })
        EXPECT_FALSE(arEndingIsSearchVerdict(e)) << arEndingName(e);
    for (ArEnding e : { ArEnding::NoNlCandidate, ArEnding::SearchFailed, ArEnding::RatioLow,
                        ArEnding::RatioHigh, ArEnding::SuccessRate, ArEnding::PairFrac,
                        ArEnding::AllHeld, ArEnding::Degenerate, ArEnding::Fixed,
                        ArEnding::FixedPartial })
        EXPECT_TRUE(arEndingIsSearchVerdict(e)) << arEndingName(e);
}

TEST(ArFixQuality, HeldScaleIsTenTimesTheHoldVariance) {
    EXPECT_TRUE(isHeldDdVariance(1e-7, 1e-7));
    EXPECT_TRUE(isHeldDdVariance(1.5e-7, 1e-7));    // the 99th percentile of held DDs on observed data
    EXPECT_TRUE(isHeldDdVariance(1e-6, 1e-7));
    EXPECT_FALSE(isHeldDdVariance(1.1e-6, 1e-7));
    EXPECT_FALSE(isHeldDdVariance(2.4e-4, 1e-7));   // the 0.1st percentile of float DDs
    EXPECT_FALSE(isHeldDdVariance(1e-9, 0.0));      // no hold variance, nothing is held
}

TEST(ArFixQuality, CensusCountsHeldPairsAndTheEdgesOfTheHeldSet) {
    ArStageRig rig(baseConfig());
    // DD variance 2e-8 m2 on every signal (two states of 1e-8 each): all three pairs held.
    ArScene s("G12", { { "G15", 3.0, 1.0 }, { "G05", 7.0, 2.0 }, { "G07", 4.0, 1.0 } });
    AmbiguityResolution r1;
    Access::census(*rig.resolver, s.pairs, s.P, s.obs, r1);
    EXPECT_EQ(r1.n_sat_pairs, 3);
    EXPECT_EQ(r1.n_held_pairs, 3);
    EXPECT_EQ(r1.held_joined, 4);   // G12, G15, G05, G07
    EXPECT_EQ(r1.held_left, 0);

    // G05 refloated on one signal only (state 4 is its L1C): one held signal is not a held pair.
    s.P(4, 4) = 1.0;
    AmbiguityResolution r2;
    Access::census(*rig.resolver, s.pairs, s.P, s.obs, r2);
    EXPECT_EQ(r2.n_held_pairs, 2);
    EXPECT_EQ(r2.held_joined, 0);
    EXPECT_EQ(r2.held_left, 1);

    // G07 sets: its pairs leave the epoch.
    std::vector<ArWlcStageTestAccess::Pair> without_g07(s.pairs.begin(), s.pairs.end() - 2);
    AmbiguityResolution r3;
    Access::census(*rig.resolver, without_g07, s.P, s.obs, r3);
    EXPECT_EQ(r3.n_sat_pairs, 2);
    EXPECT_EQ(r3.n_held_pairs, 1);
    EXPECT_EQ(r3.held_left, 1);

    const FixQualityCounters& c = rig.resolver->fixQualityCounters();
    EXPECT_EQ(c.epochs, 3);
    EXPECT_EQ(c.epochs_any_held, 3);
    EXPECT_EQ(c.pair_epochs, 8);
    EXPECT_EQ(c.held_pair_epochs, 6);
    EXPECT_EQ(c.joins, 4);
    EXPECT_EQ(c.leaves_present, 1);   // G05, still observed
    EXPECT_EQ(c.leaves_absent, 1);    // G07, gone
    const std::string text = rig.logText();
    EXPECT_NE(text.find("HELDSET tow=345600.0 sat=G05 leave cause=present"), std::string::npos);
    EXPECT_NE(text.find("HELDSET tow=345600.0 sat=G07 leave cause=absent"), std::string::npos);
    EXPECT_NE(text.find("HELDSET tow=345600.0 sat=G12 join"), std::string::npos);

    // An epoch without pairs empties the held set by absence.
    AmbiguityResolution r4;
    Access::census(*rig.resolver, {}, Eigen::MatrixXd(), {}, r4);
    EXPECT_EQ(r4.n_sat_pairs, 0);
    EXPECT_EQ(r4.held_left, 2);   // G12 and G15
    EXPECT_EQ(rig.resolver->fixQualityCounters().leaves_absent, 3);
    const std::string line = rig.resolver->fixQualityRunEndLine();
    EXPECT_NE(line.find("held census 4 epochs, 3 with a held pair (75.0 %), 6 of 8 pair-epochs held"),
              std::string::npos) << line;
    EXPECT_NE(line.find("4 joins, 1 leaves while observed, 3 by absence"), std::string::npos) << line;
}

TEST(ArFixQuality, AChangeOfReferenceMovesNoSatelliteInOrOutOfTheHeldSet) {
    ArStageRig rig(baseConfig());
    const ArScene a("G12", { { "G15", 3.0, 1.0 }, { "G05", 7.0, 2.0 } });
    const ArScene b("G15", { { "G12", -3.0, -1.0 }, { "G05", 4.0, 1.0 } });
    AmbiguityResolution r1, r2;
    Access::census(*rig.resolver, a.pairs, a.P, a.obs, r1);
    Access::census(*rig.resolver, b.pairs, b.P, b.obs, r2);
    EXPECT_EQ(r1.held_joined, 3);
    EXPECT_EQ(r2.held_joined, 0);
    EXPECT_EQ(r2.held_left, 0);
    EXPECT_EQ(r2.n_held_pairs, 2);
}

// One candidate f cycles from its integer on both signals (the wide lane exactly 2).
AmbiguityResolution searchOne(const AmbiguityResolverConfig& cfg, double f, double var_m2 = 1e-6) {
    ArStageRig rig(cfg);
    const ArScene s("G12", { { "G15", 3.0 + f, 1.0 + f } }, System::GPS, "L1C", "L2W",
                    gnss_test::kFL1, gnss_test::kFL2, var_m2);
    return Access::search(*rig.resolver, s.pairs, s.x, s.P, s.obs);
}

TEST(ArFixQuality, TheSearchReportsItsEndingAndTheRatioItComputed) {
    const AmbiguityResolverConfig cfg = baseConfig();

    const AmbiguityResolution fixed = searchOne(cfg, 0.03);   // ratio (0.97 / 0.03)^2 = 1045
    EXPECT_TRUE(fixed.success);
    EXPECT_EQ(fixed.ending, ArEnding::Fixed);
    EXPECT_NEAR(fixed.search_ratio, 1045.4, 1.0);
    EXPECT_DOUBLE_EQ(fixed.search_ratio, fixed.ratio);

    const AmbiguityResolution low = searchOne(cfg, 0.40);     // (0.6 / 0.4)^2 = 2.25
    EXPECT_FALSE(low.success);
    EXPECT_EQ(low.ending, ArEnding::RatioLow);
    EXPECT_NEAR(low.search_ratio, 2.25, 0.01);

    const AmbiguityResolution high = searchOne(cfg, 0.005);   // (0.995 / 0.005)^2 = 39601
    EXPECT_FALSE(high.success);
    EXPECT_EQ(high.ending, ArEnding::RatioHigh);
    EXPECT_NEAR(high.search_ratio, 39601.0, 50.0);

    const AmbiguityResolution frac = searchOne(cfg, 0.20);    // ratio 16 passes, |frac| 0.2 > 0.15
    EXPECT_FALSE(frac.success);
    EXPECT_EQ(frac.ending, ArEnding::PairFrac);
    EXPECT_NEAR(frac.search_ratio, 16.0, 0.05);

    // The iono-free row carries about eight times the narrow-lane sigma: at 1e-4 m2 per state
    // the wide lane is still admitted (sigma 0.09 cycle) while the search sigma is about
    // 0.39 cycle, a bootstrap success rate near 0.80.
    AmbiguityResolverConfig ifrow = cfg;
    ifrow.nl_iono_free_row = true;
    const AmbiguityResolution rate = searchOne(ifrow, 0.03, 1e-4);
    EXPECT_FALSE(rate.success);
    EXPECT_EQ(rate.ending, ArEnding::SuccessRate);
    EXPECT_NEAR(rate.search_ratio, 1045.4, 1.0);
}

TEST(ArFixQuality, NoCandidateWhenTheWideLaneIsNotAdmitted) {
    ArStageRig rig(baseConfig());
    const ArScene s("G12", { { "G15", 3.5, 1.0 } });   // wide lane 2.5: half a cycle off
    const AmbiguityResolution r = Access::search(*rig.resolver, s.pairs, s.x, s.P, s.obs);
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.ending, ArEnding::NoNlCandidate);
    EXPECT_DOUBLE_EQ(r.search_ratio, 0.0);
}

TEST(ArFixQualityTrace, AFailedSearchWritesItsEndingAndTheAppendedColumns) {
    AmbiguityResolution ar;
    ar.ending       = ArEnding::RatioHigh;
    ar.search_ratio = 39601.234;
    ar.n_sat_pairs  = 9;
    ar.n_held_pairs = 7;
    ar.held_joined  = 1;
    ar.held_left    = 2;
    EXPECT_EQ(consumerArGateLabel(true, ar), "ratio_high");
    ConsumerTraceRow row;
    fillConsumerTraceAr(row, true, ar);
    const std::vector<std::string> f = splitCsv(formatConsumerTraceRow(row));
    ASSERT_EQ(f.size(), kConsumerTraceColumns);
    EXPECT_EQ(f[7], "0.00");          // the ratio column is unchanged on a failure
    EXPECT_EQ(f[12], "ratio_high");
    EXPECT_EQ(f[18], "39601.23");
    EXPECT_EQ(f[19], "9");
    EXPECT_EQ(f[20], "7");
    EXPECT_EQ(f[21], "1");
    EXPECT_EQ(f[22], "2");

    // Not run: every appended column is zero.
    fillConsumerTraceAr(row, false, ar);
    const std::vector<std::string> g = splitCsv(formatConsumerTraceRow(row));
    EXPECT_EQ(g[12], "not_run");
    EXPECT_EQ(g[18], "0.00");
    EXPECT_EQ(g[20], "0");

    // An early gate keeps its label; a success keeps "fixed".
    ar.gate_reason = 3;
    ar.gate_sigma_3d = 0.0612;
    ar.ending = ArEnding::SigmaGate;
    EXPECT_EQ(consumerArGateLabel(true, ar), "sigma_gate(0.061m)");
    ar.gate_reason = 0;
    ar.success = true;
    ar.ending = ArEnding::Fixed;
    EXPECT_EQ(consumerArGateLabel(true, ar), "fixed");
    // A path that records no ending keeps the generic words.
    ar.success = false;
    ar.ending = ArEnding::None;
    ar.ratio = 0.0;
    EXPECT_EQ(consumerArGateLabel(true, ar), "mlambda_fail");
}

} // namespace
