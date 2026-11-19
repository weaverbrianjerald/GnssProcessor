/**
 * @file test_ar_wl_branch.cpp
 * @brief Unit tests for the wide-lane decision by the ionosphere-free phase
 *        (@c estimation.ambiguity_resolution.wl_branch_check).
 *
 * Four layers.  The provisional decision of one epoch is a pure function
 * (@ref decideWlBranch) and is tested row by row without a filter; so is the persistence step
 * (@ref stepWlBranchPersistence) that decides when the phase's decision takes effect.  The resolver is driven through resolveWlConstrained on a
 * constructed state and covariance, where the float wide lane and the fraction of the
 * ionosphere-free N_1 float of each candidate integer are set by algebra: for one pair the
 * N_1 float of the iono-free row for a wide-lane integer W is
 * @f$N_1 + \frac{f_2}{f_1-f_2}(w - W)@f$ with @f$N_1@f$ the float DD ambiguity of the
 * higher frequency in cycles.  The key and its loader behaviour close the file.
 *
 * Suite names @c ArWlBranchDecisionTest, @c ArWlBranchPersistenceTest,
 * @c ArWlBranchResolverTest and @c ArWlBranchConfigTest are owned solely by this file.
 */

#include <gtest/gtest.h>

#include "common/Constants.hpp"
#include "config/GnssConfig.hpp"
#include "filter/AmbiguityResolver.hpp"
#include "filter/AmbiguityResolverSetup.hpp"
#include "logging/LogManager.hpp"
#include "support/TestTempDir.hpp"

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace GnssProcessor {

/// Test seam for the private pieces of the resolver (friend in AmbiguityResolver.hpp).
struct ArWlBranchTestAccess {
    using Pair = AmbiguityResolver::DdPair;
    static AmbiguityResolution run(AmbiguityResolver& r, const std::vector<Pair>& pairs,
                                   const Eigen::VectorXd& x, const Eigen::MatrixXd& P,
                                   const std::vector<PppObservation>& obs) {
        return r.resolveWlConstrained(pairs, x, P, {}, obs);
    }
    static std::size_t runCount(const AmbiguityResolver& r) { return r.wl_branch_runs_.size(); }
    static void addRun(AmbiguityResolver& r, const std::string& key, const WlBranchRun& run) {
        r.wl_branch_runs_[key] = run;
    }
    static void prune(AmbiguityResolver& r, double tow) { r.pruneWlBranchRuns_(tow); }
};

} // namespace GnssProcessor

using namespace GnssProcessor;
namespace fs = std::filesystem;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// ---------------------------------------------------------------------------
// The pure decision
// ---------------------------------------------------------------------------

constexpr double kGapGps    = 0.4706;   // |f2/(f1-f2) - round| for GPS L1/L2
constexpr double kGapGal    = 0.0514;   // the same for Galileo E1/E5a
constexpr double kWlMax     = 0.40;
constexpr double kSigmaN    = 0.01;     // a converged iono-free row
constexpr double kSigmaTiny = 0.005;    // sigma_w of a well determined wide lane

/// A fraction that the phase calls decisive at kSigmaN on the GPS gap.
constexpr double kDecisive = 0.02;
/// A fraction that it does not.
constexpr double kNotDecisive = 0.30;

/// Wrap to [-0.5, 0.5].
double wrap(double v) { return v - std::round(v); }

} // namespace

TEST(ArWlBranchDecisionTest, NotFiniteIsNotAdmitted) {
    for (const double bad : { kNaN, std::numeric_limits<double>::infinity() }) {
        const auto d1 = decideWlBranch(bad, kSigmaTiny, kDecisive, kNotDecisive, kSigmaN, kGapGps, kWlMax);
        EXPECT_EQ(d1.integer, WlBranchChoice::None);
        EXPECT_EQ(d1.reason, WlBranchReason::NotFinite);
        const auto d2 = decideWlBranch(3.1, bad, kDecisive, kNotDecisive, kSigmaN, kGapGps, kWlMax);
        EXPECT_EQ(d2.integer, WlBranchChoice::None);
        EXPECT_EQ(d2.reason, WlBranchReason::NotFinite);
    }
}

TEST(ArWlBranchDecisionTest, ThreeIntegersWithinReachAreRefusedEvenWhenThePhaseIsDecisive) {
    // d = 0.1, 3 sigma_w = 1.2 >= 1 + 0.1: the code cannot tell W0, W1 and W0 - 1 apart.
    const auto d = decideWlBranch(3.1, 0.4, kDecisive, kNotDecisive, kSigmaN, kGapGps, kWlMax);
    EXPECT_EQ(d.integer, WlBranchChoice::None);
    EXPECT_EQ(d.reason, WlBranchReason::Reach);
    EXPECT_FALSE(d.code_admits);
    // The same with the phase decisive for the other integer: reach still wins.
    const auto e = decideWlBranch(3.1, 0.4, kNotDecisive, kDecisive, kSigmaN, kGapGps, kWlMax);
    EXPECT_EQ(e.integer, WlBranchChoice::None);
    EXPECT_EQ(e.reason, WlBranchReason::Reach);
}

TEST(ArWlBranchDecisionTest, RowCodeAdmitsAndThePhaseDoesNotContradict) {
    // Decisive for W0.
    const auto a = decideWlBranch(3.1, kSigmaTiny, kDecisive, kNotDecisive, kSigmaN, kGapGps, kWlMax);
    EXPECT_EQ(a.integer, WlBranchChoice::W0);
    EXPECT_EQ(a.reason, WlBranchReason::Code);
    EXPECT_TRUE(a.code_admits);
    EXPECT_EQ(a.phase, WlBranchChoice::W0);
    // Decisive for neither.
    const auto b = decideWlBranch(3.1, kSigmaTiny, kNotDecisive, kNotDecisive, kSigmaN, kGapGps, kWlMax);
    EXPECT_EQ(b.integer, WlBranchChoice::W0);
    EXPECT_EQ(b.reason, WlBranchReason::Code);
    EXPECT_EQ(b.phase, WlBranchChoice::None);
}

TEST(ArWlBranchDecisionTest, RowContradictionTakesNoIntegerAndIsNotACandidateForW1) {
    const auto d = decideWlBranch(3.1, kSigmaTiny, kNotDecisive, kDecisive, kSigmaN, kGapGps, kWlMax);
    EXPECT_TRUE(d.code_admits);
    EXPECT_EQ(d.phase, WlBranchChoice::W1);
    EXPECT_EQ(d.integer, WlBranchChoice::None);
    EXPECT_EQ(d.reason, WlBranchReason::Contradiction);
    EXPECT_FALSE(d.w1_candidate);       // the code admits W0: the neighbour is never a candidate
}

TEST(ArWlBranchDecisionTest, TheRealCaseOfAOneEpochTransientIsAContradictionWithNoIntegerTaken) {
    // Measured: the float had moved 0.36 cycle within one epoch and its sigma was ten times too
    // small; the phase happened to sit on the neighbouring integer.
    const auto d = decideWlBranch(-0.0508, 0.0591, 0.4448, -0.0257, 0.0616, 0.4706, 0.40);
    EXPECT_TRUE(d.code_admits);
    EXPECT_EQ(d.phase, WlBranchChoice::W1);
    EXPECT_EQ(d.integer, WlBranchChoice::None);
    EXPECT_EQ(d.reason, WlBranchReason::Contradiction);
}

TEST(ArWlBranchDecisionTest, RowsTheCodeRefusesAndThePhaseDecidesForW0OrForACandidateW1) {
    // |d| = 0.45 + 3 sigma_w > 0.40: the code refuses; the phase is decisive for W0.
    const auto w0 = decideWlBranch(3.45, kSigmaTiny, kDecisive, kNotDecisive, kSigmaN, kGapGps, kWlMax);
    EXPECT_FALSE(w0.code_admits);
    EXPECT_EQ(w0.integer, WlBranchChoice::W0);
    EXPECT_EQ(w0.reason, WlBranchReason::PhaseW0);
    // sigma_w = 0.2: 1 - 0.45 = 0.55 <= 0.6, W1 is a candidate, and the phase is decisive for it.
    const auto w1 = decideWlBranch(3.45, 0.2, kNotDecisive, kDecisive, kSigmaN, kGapGps, kWlMax);
    EXPECT_FALSE(w1.code_admits);
    EXPECT_TRUE(w1.w1_candidate);
    EXPECT_EQ(w1.integer, WlBranchChoice::W1);
    EXPECT_EQ(w1.reason, WlBranchReason::PhaseW1);
}

TEST(ArWlBranchDecisionTest, W1WithThePhaseDecisiveButNoCandidateIsUndecided) {
    // sigma_w small and |d| = 0.45: the code excludes W1 (0.55 > 3 * 0.005), so a phase decisive
    // for W1 alone is not enough.
    const auto d = decideWlBranch(3.45, kSigmaTiny, kNotDecisive, kDecisive, kSigmaN, kGapGps, kWlMax);
    EXPECT_FALSE(d.code_admits);
    EXPECT_FALSE(d.w1_candidate);
    EXPECT_EQ(d.phase, WlBranchChoice::W1);
    EXPECT_EQ(d.integer, WlBranchChoice::None);
    EXPECT_EQ(d.reason, WlBranchReason::Undecided);
    // A small |d| with a large sigma_w that the code refuses: 1 - 0.02 > 3 * 0.15 = 0.45.
    const auto e = decideWlBranch(3.02, 0.15, kNotDecisive, kDecisive, kSigmaN, kGapGps, kWlMax);
    EXPECT_FALSE(e.code_admits);
    EXPECT_FALSE(e.w1_candidate);
    EXPECT_EQ(e.integer, WlBranchChoice::None);
    EXPECT_EQ(e.reason, WlBranchReason::Undecided);
}

TEST(ArWlBranchDecisionTest, TheCandidateBoundaryIsOneMinusDAgainstThreeSigmaW) {
    // d = 0.4: the code refuses at sigma_w = 0.1 (0.4 + 0.3 > 0.40).  W1 is a candidate from
    // 3 sigma_w >= 0.6, that is sigma_w = 0.2; reach (1.4 <= 3 sigma_w) needs sigma_w >= 0.4667.
    EXPECT_FALSE(decideWlBranch(3.4, 0.1, kNotDecisive, kDecisive, kSigmaN, kGapGps, kWlMax).w1_candidate);
    EXPECT_FALSE(decideWlBranch(3.4, 0.19, kNotDecisive, kDecisive, kSigmaN, kGapGps, kWlMax).w1_candidate);
    EXPECT_TRUE(decideWlBranch(3.4, 0.2001, kNotDecisive, kDecisive, kSigmaN, kGapGps, kWlMax).w1_candidate);
    EXPECT_EQ(decideWlBranch(3.4, 0.4, kNotDecisive, kDecisive, kSigmaN, kGapGps, kWlMax).reason,
              WlBranchReason::PhaseW1);
}

TEST(ArWlBranchDecisionTest, ReachTakesPrecedenceOverACandidateW1WithThePhaseDecisive) {
    // d = 0.49, sigma_w = 0.4: W1 is a candidate (0.51 <= 1.2) and the phase is decisive for it,
    // but 1.49 > 1.2 holds only below; at sigma_w = 0.5 three integers are within reach.
    const auto d = decideWlBranch(3.49, 0.5, kNotDecisive, kDecisive, kSigmaN, kGapGps, kWlMax);
    EXPECT_EQ(d.reason, WlBranchReason::Reach);
    EXPECT_EQ(d.integer, WlBranchChoice::None);
    EXPECT_FALSE(d.w1_candidate);
}

TEST(ArWlBranchDecisionTest, RowTheCodeRefusesAndThePhaseIsNotDecisiveIsNotAdmitted) {
    const auto d = decideWlBranch(3.45, kSigmaTiny, kNotDecisive, kNotDecisive, kSigmaN, kGapGps, kWlMax);
    EXPECT_EQ(d.integer, WlBranchChoice::None);
    EXPECT_EQ(d.reason, WlBranchReason::Undecided);
    EXPECT_EQ(d.phase, WlBranchChoice::None);
}

TEST(ArWlBranchDecisionTest, AFloatBelowTheNearestIntegerOffersTheLowerNeighbourAsW1) {
    // The meaning of W1 (the side of the float) belongs to the caller; the decision only names it.
    // A decisive r1 on a float with d < 0 and a refusing code gives W1 when it is a candidate.
    const auto d = decideWlBranch(2.55, 0.2, kNotDecisive, kDecisive, kSigmaN, kGapGps, kWlMax);
    EXPECT_EQ(d.integer, WlBranchChoice::W1);
    EXPECT_EQ(d.reason, WlBranchReason::PhaseW1);
}

TEST(ArWlBranchDecisionTest, ASmallGapIsNeverDecisiveAtAFortyThousandthFloor) {
    // Galileo E1/E5a gap 0.051, floor 0.04: half a gap is 0.0257 and 3 sigma_n is at least 0.12.
    const double sigma_n = std::sqrt(0.004 * 0.004 + 0.04 * 0.04);
    for (double r0 = -0.5; r0 <= 0.5; r0 += 0.001) {
        const auto d = decideWlBranch(3.45, kSigmaTiny, r0, wrap(r0 + kGapGal), sigma_n, kGapGal, kWlMax);
        ASSERT_EQ(d.phase, WlBranchChoice::None) << "r0 " << r0;
        EXPECT_EQ(d.integer, WlBranchChoice::None);
    }
}

TEST(ArWlBranchDecisionTest, ThePhaseIsNeverDecisiveForBothIntegersOverASweepOfConsistentInputs) {
    // The two N_1 floats of one pair differ by the shift of the iono-free row, so their fractions
    // differ by the shift modulo one.  Shifts of GPS L1/L2, Galileo E1/E5a, BeiDou B1I/B3I and
    // GPS L1/L5 (as -f2/(f1-f2)), both signs of d, and several sigmas.
    const double shifts[] = { -3.5294, -2.9486, -1.3363, -2.2599 };
    int tested = 0;
    for (const double shift : shifts) {
        const double gap = std::abs(shift - std::round(shift));
        for (const double sign : { 1.0, -1.0 }) {
            for (const double sigma_n : { 0.001, 0.005, 0.02, 0.05 }) {
                for (double r0 = -0.5; r0 <= 0.5; r0 += 0.0025) {
                    const double r1 = wrap(r0 + shift * sign);
                    const bool dec0 = std::abs(r0) + 3.0 * sigma_n <= 0.5 * gap;
                    const bool dec1 = std::abs(r1) + 3.0 * sigma_n <= 0.5 * gap;
                    ASSERT_FALSE(dec0 && dec1) << "shift " << shift << " r0 " << r0 << " sigma " << sigma_n;
                    // And the function never names an integer that contradicts the table.
                    const auto d = decideWlBranch(3.0 + 0.3 * sign, kSigmaTiny, r0, r1, sigma_n, gap, kWlMax);
                    if (d.phase == WlBranchChoice::W0) ASSERT_TRUE(dec0);
                    if (d.phase == WlBranchChoice::W1) ASSERT_TRUE(dec1);
                    ++tested;
                }
            }
        }
    }
    EXPECT_GT(tested, 1000);
}

TEST(ArWlBranchDecisionTest, InconsistentInputsThatClaimBothDecisiveAreReadAsNeither) {
    const auto d = decideWlBranch(3.45, kSigmaTiny, 0.01, -0.01, 0.001, kGapGps, kWlMax);
    EXPECT_EQ(d.phase, WlBranchChoice::None);
    EXPECT_EQ(d.integer, WlBranchChoice::None);
}

TEST(ArWlBranchDecisionTest, EveryReasonHasADistinctWord) {
    std::set<std::string> words;
    for (const auto r : { WlBranchReason::NotFinite, WlBranchReason::Reach, WlBranchReason::Code,
                          WlBranchReason::Contradiction, WlBranchReason::PhaseW0,
                          WlBranchReason::PhaseW1, WlBranchReason::Undecided,
                          WlBranchReason::Waiting })
        words.insert(wlBranchReasonWord(r));
    EXPECT_EQ(words.size(), 8u);
    EXPECT_STREQ(wlBranchReasonWord(WlBranchReason::Waiting), "waiting");
}

// ---------------------------------------------------------------------------
// The persistence step
// ---------------------------------------------------------------------------

TEST(ArWlBranchPersistenceTest, ARunReachingThePersistenceTimeBecomesRipe) {
    auto step = stepWlBranchPersistence(std::nullopt, true, 4, 100.0, 120.0);
    ASSERT_TRUE(step.keep);
    EXPECT_FALSE(step.ripe);
    EXPECT_DOUBLE_EQ(step.persisted_s, 0.0);
    std::optional<WlBranchRun> run = step.run;
    step = stepWlBranchPersistence(run, true, 4, 160.0, 120.0);
    EXPECT_FALSE(step.ripe);
    EXPECT_DOUBLE_EQ(step.persisted_s, 60.0);
    run = step.run;
    step = stepWlBranchPersistence(run, true, 4, 219.0, 120.0);
    EXPECT_FALSE(step.ripe);
    run = step.run;
    step = stepWlBranchPersistence(run, true, 4, 220.0, 120.0);
    EXPECT_TRUE(step.ripe);
    EXPECT_DOUBLE_EQ(step.persisted_s, 120.0);
    EXPECT_DOUBLE_EQ(step.run.began_tow, 100.0);
    EXPECT_DOUBLE_EQ(step.run.last_tow, 220.0);
}

TEST(ArWlBranchPersistenceTest, AChangeOfIntegerRestartsTheRun) {
    const WlBranchRun run{ 4, 100.0, 200.0 };
    const auto step = stepWlBranchPersistence(run, true, 5, 210.0, 120.0);
    ASSERT_TRUE(step.keep);
    EXPECT_EQ(step.run.integer, 5);
    EXPECT_DOUBLE_EQ(step.run.began_tow, 210.0);
    EXPECT_FALSE(step.ripe);
    EXPECT_DOUBLE_EQ(step.persisted_s, 0.0);
}

TEST(ArWlBranchPersistenceTest, AnEvaluationWhereThePhaseIsNotDecisiveErasesTheEntry) {
    const WlBranchRun run{ 4, 100.0, 200.0 };
    const auto step = stepWlBranchPersistence(run, false, 4, 210.0, 120.0);
    EXPECT_FALSE(step.keep);
    EXPECT_FALSE(step.ripe);
    EXPECT_DOUBLE_EQ(step.persisted_s, 0.0);
    // Also with no entry and persistence 0: nothing to keep and nothing ripe.
    const auto none = stepWlBranchPersistence(std::nullopt, false, 4, 210.0, 0.0);
    EXPECT_FALSE(none.keep);
    EXPECT_FALSE(none.ripe);
}

TEST(ArWlBranchPersistenceTest, AGapLongerThanThePersistenceTimeRestartsTheRun) {
    const WlBranchRun run{ 4, 100.0, 150.0 };
    // 120 s after the last decisive epoch is still continuous, 121 s is not.
    const auto kept = stepWlBranchPersistence(run, true, 4, 270.0, 120.0);
    EXPECT_DOUBLE_EQ(kept.run.began_tow, 100.0);
    EXPECT_TRUE(kept.ripe);
    const auto lost = stepWlBranchPersistence(run, true, 4, 271.0, 120.0);
    EXPECT_DOUBLE_EQ(lost.run.began_tow, 271.0);
    EXPECT_FALSE(lost.ripe);
}

TEST(ArWlBranchPersistenceTest, TheGapLimitIsAtLeastOneSecond) {
    const WlBranchRun run{ 4, 100.0, 100.0 };
    EXPECT_DOUBLE_EQ(stepWlBranchPersistence(run, true, 4, 101.0, 0.5).run.began_tow, 100.0);
    EXPECT_DOUBLE_EQ(stepWlBranchPersistence(run, true, 4, 101.5, 0.5).run.began_tow, 101.5);
}

TEST(ArWlBranchPersistenceTest, APersistenceOfZeroIsRipeAtOnce) {
    const auto first = stepWlBranchPersistence(std::nullopt, true, 4, 100.0, 0.0);
    EXPECT_TRUE(first.ripe);
    EXPECT_TRUE(first.keep);
    const auto again = stepWlBranchPersistence(first.run, true, 4, 130.0, 0.0);
    EXPECT_TRUE(again.ripe);
}

TEST(ArWlBranchPersistenceTest, ATimeThatRunsBackwardRestartsTheRun) {
    // A week rollover: the time of week falls below the last decisive time.
    const WlBranchRun run{ 4, 604000.0, 604790.0 };
    const auto step = stepWlBranchPersistence(run, true, 4, 10.0, 120.0);
    EXPECT_DOUBLE_EQ(step.run.began_tow, 10.0);
    EXPECT_FALSE(step.ripe);
}

// ---------------------------------------------------------------------------
// Through resolveWlConstrained
// ---------------------------------------------------------------------------

namespace {

/// One dual-frequency pair of satellites and the state that realizes a chosen float.
struct Band {
    System      system;
    const char* sat_i;
    const char* sat_ref;
    const char* sig_hi;
    const char* sig_lo;
    double      f1;
    double      f2;
    double lam1() const { return Constants::SPEED_OF_LIGHT / f1; }
    double lam2() const { return Constants::SPEED_OF_LIGHT / f2; }
    /// Cycles of N_1 float per cycle of wide lane in the iono-free row.
    double slope() const { return f2 / (f1 - f2); }
};

Band gpsL1L2() { return { System::GPS, "G13", "G12", "L1C", "L2W", 1575.42e6, 1227.60e6 }; }
Band galE1E5a() { return { System::Galileo, "E05", "E09", "L1C", "L5Q", 1575.42e6, 1176.45e6 }; }

/// The pair of DD pairs: states 0 and 1 on the higher frequency, 2 and 3 on the lower.
std::vector<ArWlBranchTestAccess::Pair> pairsOf(const Band& b) {
    return { { 0, 1, b.sat_i, b.sat_ref, b.sig_hi }, { 2, 3, b.sat_i, b.sat_ref, b.sig_lo } };
}

/**
 * The state with float wide lane @p w whose iono-free N_1 float for the wide-lane integer
 * @p w_target has the fraction @p frac_target (N_1 near 8).
 */
Eigen::VectorXd stateFor(const Band& b, double w, long w_target, double frac_target) {
    const double n1 = 8.0 + frac_target - b.slope() * (w - static_cast<double>(w_target));
    const double n2 = n1 - w;
    Eigen::VectorXd x(4);
    x << n1 * b.lam1(), 0.0, n2 * b.lam2(), 0.0;
    return x;
}

/// A well determined float: 0.1 mm on every ambiguity.
Eigen::MatrixXd tightP() { return Eigen::MatrixXd::Identity(4, 4) * 1e-8; }

/// Variance along a direction of the two DD ambiguities (metres), as a rank-one addition.
void addDirection(Eigen::MatrixXd& P, double dd1, double dd2, double sigma) {
    Eigen::Vector4d u(dd1, 0.0, dd2, 0.0);
    P += sigma * sigma * u * u.transpose();
}

/// Adds variance that raises sigma_w to @p sigma_w_cyc and leaves the iono-free row alone.
void widenWidelaneOnly(Eigen::MatrixXd& P, const Band& b, double sigma_w_cyc) {
    const double c  = Constants::SPEED_OF_LIGHT;
    const double t1 = b.f1 * b.f1 / ((b.f1 - b.f2) * c);
    const double t2 = -(b.f2 * b.f2) / ((b.f1 - b.f2) * c);
    const double u1 = -t2, u2 = t1;                      // t1 u1 + t2 u2 = 0
    const double along_w = u1 / b.lam1() - u2 / b.lam2();
    addDirection(P, u1, u2, sigma_w_cyc / std::abs(along_w));
}

/// Adds variance that raises sigma_n of the iono-free row to about @p sigma_n_cyc and leaves w alone.
void widenIonoFreeRowOnly(Eigen::MatrixXd& P, const Band& b, double sigma_n_cyc) {
    addDirection(P, b.lam1(), b.lam2(), sigma_n_cyc);    // one cycle on both: w unchanged, n moves by 1
}

AmbiguityResolverConfig branchConfig(bool branch_check, double floor_cyc = 0.0,
                                     double persist_s = 120.0) {
    AmbiguityResolverConfig c;
    c.ar_mode             = 3;
    c.wl_from_float       = true;
    c.nl_iono_free_row    = true;
    c.wl_branch_check     = branch_check;
    c.wl_branch_persist_s = persist_s;
    c.wl_max_frac         = kWlMax;
    c.ratio_upper_bound   = 1e30;
    c.min_success_rate    = 0.0;
    c.hold_release_cyc    = 1.0;      // only so that an accepted pair is reported with its integers
    c.nl_sigma_floor_cyc  = floor_cyc;
    return c;
}

std::vector<PppObservation> obsOf(const Band& b, double tow) {
    PppObservation o;
    o.satellite = b.sat_i;
    o.system    = b.system;
    o.time.tow  = tow;
    return { o };
}

/// A resolver that can be driven directly, with its log captured.
struct Fixture {
    LogManager        log;
    std::ostringstream text;
    std::unique_ptr<AmbiguityResolver> resolver;
    Band band;

    Fixture(const AmbiguityResolverConfig& cfg, const Band& b, const std::string& tag) : band(b) {
        const fs::path dir = gnss_test::tempDir() / ("ar_wl_branch_" + tag);
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        spdlog::drop("ambiguity_resolver");
        log.initialize(dir, LogLevel::Info);
        log.getLogger("ambiguity_resolver")->sinks().push_back(
            std::make_shared<spdlog::sinks::ostream_sink_mt>(text));
        resolver = std::make_unique<AmbiguityResolver>(cfg, log);
    }
    ~Fixture() { spdlog::drop("ambiguity_resolver"); }

    AmbiguityResolution run(const Eigen::VectorXd& x, const Eigen::MatrixXd& P, double tow = 123.0) {
        auto r = ArWlBranchTestAccess::run(*resolver, pairsOf(band), x, P, obsOf(band, tow));
        log.getLogger("ambiguity_resolver")->flush();
        return r;
    }
    std::string logText() { return text.str(); }
};

/// True when @p r constrained the pair (it enters the search and the fix is accepted).
bool admitted(const AmbiguityResolution& r) { return r.success && !r.fixed_dd_rows.empty(); }

} // namespace

TEST(ArWlBranchResolverTest, KeyOffAdmitsAPairTheCodeAdmitsWithItsNearestInteger) {
    Fixture f(branchConfig(false), gpsL1L2(), "off_admit");
    const auto r = f.run(stateFor(f.band, 3.02, 3, 0.02), tightP());
    ASSERT_TRUE(admitted(r));
    ASSERT_EQ(r.held_pairs.size(), 1u);
    EXPECT_EQ(r.held_pairs[0].wl, 3);
    EXPECT_TRUE(r.bias_reset_satellites.empty());
    EXPECT_EQ(f.resolver->wlBranchCounters().evaluated, 0);
    EXPECT_EQ(f.logText().find("WLBRANCH"), std::string::npos) << f.logText();
    EXPECT_NE(f.logText().find("WLFLOAT"), std::string::npos);
}

TEST(ArWlBranchResolverTest, KeyOffRefusesAPairTheCodeRefusesEvenWhenThePhaseWouldDecide) {
    Fixture f(branchConfig(false), gpsL1L2(), "off_refuse");
    // d = 0.45: the code refuses; the phase is decisive for W1 = 4 but the key is off.
    const auto r = f.run(stateFor(f.band, 3.45, 4, 0.02), tightP());
    EXPECT_FALSE(admitted(r));
    EXPECT_TRUE(r.bias_reset_satellites.empty());
    EXPECT_EQ(f.logText().find("WLBRANCH"), std::string::npos);
    EXPECT_NE(f.logText().find("admitted=0"), std::string::npos) << f.logText();
}

TEST(ArWlBranchResolverTest, KeyOnTakesTheSameDecisionForAPairTheCodeAdmits) {
    Fixture f(branchConfig(true), gpsL1L2(), "on_admit");
    const auto r = f.run(stateFor(f.band, 3.02, 3, 0.02), tightP());
    ASSERT_TRUE(admitted(r));
    EXPECT_EQ(r.held_pairs.at(0).wl, 3);
    EXPECT_TRUE(r.bias_reset_satellites.empty());
    const auto& c = f.resolver->wlBranchCounters();
    EXPECT_EQ(c.evaluated, 1);
    EXPECT_EQ(c.by_phase, 0);
    EXPECT_EQ(c.contradictions, 0);
    EXPECT_NE(f.logText().find("decision=w0 reason=code"), std::string::npos) << f.logText();
    EXPECT_NE(f.logText().find("WLFLOAT"), std::string::npos);
}

TEST(ArWlBranchResolverTest, KeyOnAdmitsAPairTheCodeRefusesByLargeDWhenThePhaseIsDecisiveForACandidateW1) {
    Fixture f(branchConfig(true, 0.0, 0.0), gpsL1L2(), "on_phase_d");
    // d = 0.45 with sigma_w = 0.2: the code refuses, W1 = 4 is a candidate, the phase is decisive for it.
    Eigen::MatrixXd P = tightP();
    widenWidelaneOnly(P, f.band, 0.2);
    const auto r = f.run(stateFor(f.band, 3.45, 4, 0.02), P);
    ASSERT_TRUE(admitted(r));
    EXPECT_EQ(r.held_pairs.at(0).wl, 4);
    EXPECT_EQ(r.held_pairs.at(0).n1, 8);
    EXPECT_TRUE(r.bias_reset_satellites.empty());    // a pair the code refused is no contradiction
    const auto& c = f.resolver->wlBranchCounters();
    EXPECT_EQ(c.by_phase, 1);
    EXPECT_EQ(c.contradictions, 0);
    EXPECT_NE(f.logText().find("code=refuse cand1=1 phase=w1 persist=0.0 decision=w1 reason=phase_w1"),
              std::string::npos) << f.logText();
    // The WLFLOAT line reports the final decision.
    EXPECT_NE(f.logText().find("dd_wl=4 admitted=1"), std::string::npos) << f.logText();
}

TEST(ArWlBranchResolverTest, KeyOnRefusesW1WhenTheCodeExcludesItEvenWithThePhaseDecisive) {
    Fixture f(branchConfig(true, 0.0, 0.0), gpsL1L2(), "on_w1_no_candidate");
    // The same state with a tight wide lane: 1 - 0.45 is far above 3 sigma_w.
    const auto r = f.run(stateFor(f.band, 3.45, 4, 0.02), tightP());
    EXPECT_FALSE(admitted(r));
    EXPECT_EQ(f.resolver->wlBranchCounters().by_phase, 0);
    EXPECT_NE(f.logText().find("cand1=0 phase=w1 persist=0.0 decision=none reason=undecided"),
              std::string::npos) << f.logText();
}

TEST(ArWlBranchResolverTest, KeyOnAdmitsAPairTheCodeRefusesByLargeSigmaWhenThePhaseIsDecisiveForW0) {
    Fixture f(branchConfig(true, 0.0, 0.0), gpsL1L2(), "on_phase_sigma");
    // d = 0.02 but sigma_w = 0.15: 0.02 + 0.45 > 0.40, the code refuses; reach holds back (1.02 > 0.45).
    Eigen::MatrixXd P = tightP();
    widenWidelaneOnly(P, f.band, 0.15);
    const auto r = f.run(stateFor(f.band, 3.02, 3, 0.02), P);
    ASSERT_TRUE(admitted(r));
    EXPECT_EQ(r.held_pairs.at(0).wl, 3);
    const auto& c = f.resolver->wlBranchCounters();
    EXPECT_EQ(c.by_phase, 1);
    EXPECT_NE(f.logText().find("reason=phase_w0"), std::string::npos) << f.logText();
    // The same pair with the key off is refused.
    Fixture off(branchConfig(false), gpsL1L2(), "on_phase_sigma_off");
    EXPECT_FALSE(admitted(off.run(stateFor(off.band, 3.02, 3, 0.02), P)));
}

TEST(ArWlBranchResolverTest, WithTheDefaultPersistenceAPairThePhaseConfirmsIsAdmittedOnlyAfterTheTime) {
    Fixture f(branchConfig(true), gpsL1L2(), "persist_admit");   // 120 s
    Eigen::MatrixXd P = tightP();
    widenWidelaneOnly(P, f.band, 0.15);
    const Eigen::VectorXd x = stateFor(f.band, 3.02, 3, 0.02);   // the code refuses, the phase says W0
    EXPECT_FALSE(admitted(f.run(x, P, 1000.0)));
    EXPECT_NE(f.logText().find("persist=0.0 decision=none reason=waiting"), std::string::npos) << f.logText();
    EXPECT_EQ(f.resolver->wlBranchCounters().waiting, 1);
    EXPECT_FALSE(admitted(f.run(x, P, 1060.0)));
    EXPECT_FALSE(admitted(f.run(x, P, 1119.0)));
    EXPECT_EQ(f.resolver->wlBranchCounters().waiting, 3);
    EXPECT_EQ(f.resolver->wlBranchCounters().by_phase, 0);
    const auto r = f.run(x, P, 1120.0);
    ASSERT_TRUE(admitted(r));
    EXPECT_EQ(r.held_pairs.at(0).wl, 3);
    EXPECT_EQ(f.resolver->wlBranchCounters().by_phase, 1);
    EXPECT_NE(f.logText().find("persist=120.0 decision=w0 reason=phase_w0"), std::string::npos) << f.logText();
}

TEST(ArWlBranchResolverTest, ARunBrokenByAnEpochWithoutADecisivePhaseStartsAgain) {
    Fixture f(branchConfig(true), gpsL1L2(), "persist_broken");
    Eigen::MatrixXd P = tightP();
    widenWidelaneOnly(P, f.band, 0.15);
    const Eigen::VectorXd x = stateFor(f.band, 3.02, 3, 0.02);
    EXPECT_FALSE(admitted(f.run(x, P, 1000.0)));
    EXPECT_EQ(ArWlBranchTestAccess::runCount(*f.resolver), 1u);
    // An epoch where the phase is decisive for neither integer (0.3 cycle off both).
    EXPECT_FALSE(admitted(f.run(stateFor(f.band, 3.02, 3, 0.30), P, 1060.0)));
    EXPECT_EQ(ArWlBranchTestAccess::runCount(*f.resolver), 0u);
    EXPECT_FALSE(admitted(f.run(x, P, 1120.0)));            // would have been ripe without the break
    EXPECT_FALSE(admitted(f.run(x, P, 1239.0)));
    EXPECT_TRUE(admitted(f.run(x, P, 1240.0)));
}

TEST(ArWlBranchResolverTest, KeyOnRefusesTheSamePairWithAnUnconvergedIonoFreeRow) {
    Fixture f(branchConfig(true, 0.0, 0.0), gpsL1L2(), "on_unconverged");
    // The state of the W1 test, but sigma_n = 0.3 cycle: 3 sigma_n = 0.9 > half a gap.
    Eigen::MatrixXd P = tightP();
    widenWidelaneOnly(P, f.band, 0.2);
    widenIonoFreeRowOnly(P, f.band, 0.3);
    const auto r = f.run(stateFor(f.band, 3.45, 4, 0.02), P);
    EXPECT_FALSE(admitted(r));
    const auto& c = f.resolver->wlBranchCounters();
    EXPECT_EQ(c.by_phase, 0);
    EXPECT_EQ(c.evaluated, 1);
    EXPECT_NE(f.logText().find("phase=none persist=0.0 decision=none reason=undecided"), std::string::npos)
        << f.logText();
    EXPECT_NE(f.logText().find("admitted=0"), std::string::npos);
}

TEST(ArWlBranchResolverTest, KeyOnRefusesForReachAndCountsIt) {
    Fixture f(branchConfig(true), gpsL1L2(), "on_reach");
    Eigen::MatrixXd P = tightP();
    widenWidelaneOnly(P, f.band, 0.5);      // 3 sigma_w = 1.5 >= 1 + 0.02
    const auto r = f.run(stateFor(f.band, 3.02, 3, 0.02), P);
    EXPECT_FALSE(admitted(r));
    const auto& c = f.resolver->wlBranchCounters();
    EXPECT_EQ(c.refused_reach, 1);
    EXPECT_EQ(c.by_phase, 0);
    EXPECT_EQ(c.waiting, 0);
    EXPECT_NE(f.logText().find("reason=reach"), std::string::npos) << f.logText();
}

TEST(ArWlBranchResolverTest, AContradictionOfAPersistenceOfZeroIsAnEventThatTakesNoIntegerAndReturnsTheSatellite) {
    Fixture f(branchConfig(true, 0.0, 0.0), gpsL1L2(), "on_contradiction");
    // d = 0.2: the code admits W0 = 3; the phase is decisive for W1 = 4.
    const auto r = f.run(stateFor(f.band, 3.2, 4, 0.02), tightP());
    EXPECT_FALSE(admitted(r));
    EXPECT_TRUE(r.held_pairs.empty());
    EXPECT_EQ(r.bias_reset_satellites, (std::vector<std::string>{ "G13" }));
    const auto& c = f.resolver->wlBranchCounters();
    EXPECT_EQ(c.contradictions, 1);
    EXPECT_EQ(c.satellites_returned, 1);
    EXPECT_EQ(c.by_phase, 0);
    EXPECT_EQ(c.waiting, 0);
    EXPECT_NE(f.logText().find("code=admit cand1=0 phase=w1 persist=0.0 decision=none reason=contradiction"),
              std::string::npos) << f.logText();
    EXPECT_NE(f.logText().find("code-bias reset requested for sat=G13"), std::string::npos);
    const std::string line = f.resolver->wlBranchRunEndLine();
    EXPECT_NE(line.find("1 persisted contradictions"), std::string::npos) << line;
    EXPECT_NE(line.find("0 pair-epochs waiting"), std::string::npos) << line;
    EXPECT_NE(line.find("1 satellites returned"), std::string::npos) << line;
}

TEST(ArWlBranchResolverTest, AContradictionIsAnEventOnlyOncePersistedAndDoesNotRepeatEveryEpoch) {
    Fixture f(branchConfig(true), gpsL1L2(), "on_contradiction_persist");   // 120 s
    const Eigen::VectorXd x = stateFor(f.band, 3.2, 4, 0.02);
    auto r = f.run(x, tightP(), 500.0);
    EXPECT_TRUE(r.bias_reset_satellites.empty());
    EXPECT_EQ(f.resolver->wlBranchCounters().contradictions, 0);
    EXPECT_NE(f.logText().find("persist=0.0 decision=none reason=contradiction"), std::string::npos);
    EXPECT_EQ(f.logText().find("code-bias reset requested"), std::string::npos);
    r = f.run(x, tightP(), 560.0);
    EXPECT_TRUE(r.bias_reset_satellites.empty());
    r = f.run(x, tightP(), 620.0);                                       // 120 s of the same integer
    EXPECT_EQ(r.bias_reset_satellites, (std::vector<std::string>{ "G13" }));
    EXPECT_EQ(f.resolver->wlBranchCounters().contradictions, 1);
    EXPECT_FALSE(admitted(r));
    r = f.run(x, tightP(), 680.0);                                       // the run restarted at the event
    EXPECT_TRUE(r.bias_reset_satellites.empty());
    EXPECT_EQ(f.resolver->wlBranchCounters().contradictions, 1);
    EXPECT_EQ(f.resolver->wlBranchCounters().satellites_returned, 1);
    r = f.run(x, tightP(), 739.0);                                       // 119 s into the new run
    EXPECT_TRUE(r.bias_reset_satellites.empty());
    r = f.run(x, tightP(), 740.0);                                       // and ripens again 120 s after the event
    EXPECT_EQ(r.bias_reset_satellites, (std::vector<std::string>{ "G13" }));
    EXPECT_EQ(f.resolver->wlBranchCounters().contradictions, 2);
}

TEST(ArWlBranchResolverTest, TheRealTransientAdmitsNothingAtOneEpochAndReturnsTheSatelliteOnceWhenItPersists) {
    // w = -0.0508 (sigma 0.0591), the phase decisive for W1 = -1 (r1 = -0.0257, r0 = 0.4448,
    // sigma_n 0.0616), code limit 0.40: the case where the neighbouring integer was wrong.
    auto build = [](const Band& b, Eigen::MatrixXd& P) {
        P = tightP();
        widenWidelaneOnly(P, b, 0.0591);
        widenIonoFreeRowOnly(P, b, 0.0616);
        return stateFor(b, -0.0508, -1, -0.0257);
    };
    {
        Fixture f(branchConfig(true), gpsL1L2(), "real_single");
        Eigen::MatrixXd P;
        const Eigen::VectorXd x = build(f.band, P);
        const auto r = f.run(x, P, 40000.0);
        EXPECT_FALSE(admitted(r));
        EXPECT_TRUE(r.bias_reset_satellites.empty());
        EXPECT_EQ(f.resolver->wlBranchCounters().contradictions, 0);
        EXPECT_NE(f.logText().find("reason=contradiction"), std::string::npos) << f.logText();
        EXPECT_NE(f.logText().find("w0=0 w1=-1"), std::string::npos) << f.logText();
    }
    {
        Fixture f(branchConfig(true), gpsL1L2(), "real_persist");
        Eigen::MatrixXd P;
        const Eigen::VectorXd x = build(f.band, P);
        AmbiguityResolution r;
        for (const double tow : { 40000.0, 40030.0, 40060.0, 40090.0 }) {
            r = f.run(x, P, tow);
            EXPECT_FALSE(admitted(r));
        }
        EXPECT_EQ(f.resolver->wlBranchCounters().contradictions, 0);
        r = f.run(x, P, 40120.0);
        EXPECT_FALSE(admitted(r));
        EXPECT_EQ(r.bias_reset_satellites, (std::vector<std::string>{ "G13" }));
        EXPECT_EQ(f.resolver->wlBranchCounters().contradictions, 1);
        EXPECT_EQ(f.resolver->wlBranchCounters().satellites_returned, 1);
        EXPECT_EQ(f.resolver->wlBranchCounters().by_phase, 0);
    }
}

TEST(ArWlBranchResolverTest, ARunOfAnotherSatelliteReferenceStartsANewRun) {
    Fixture f(branchConfig(true), gpsL1L2(), "persist_ref_change");
    Eigen::MatrixXd P = tightP();
    widenWidelaneOnly(P, f.band, 0.15);
    const Eigen::VectorXd x = stateFor(f.band, 3.02, 3, 0.02);
    f.run(x, P, 1000.0);
    // The same satellite against another reference is another pair key.
    Band other = f.band;
    other.sat_ref = "G20";
    auto r = ArWlBranchTestAccess::run(*f.resolver, pairsOf(other), x, P, obsOf(other, 1120.0));
    EXPECT_FALSE(admitted(r));
    EXPECT_EQ(ArWlBranchTestAccess::runCount(*f.resolver), 2u);
}

TEST(ArWlBranchResolverTest, TheRunsAreNotKeptLongerThanTenTimesThePersistenceTime) {
    Fixture f(branchConfig(true), gpsL1L2(), "prune");
    ArWlBranchTestAccess::addRun(*f.resolver, "G01|G02|L1C|L2W", WlBranchRun{ 4, 0.0, 10000.0 });
    ArWlBranchTestAccess::prune(*f.resolver, 11000.0);      // 1000 s <= 10 * 120 s
    EXPECT_EQ(ArWlBranchTestAccess::runCount(*f.resolver), 1u);
    ArWlBranchTestAccess::prune(*f.resolver, 11201.0);      // 1201 s > 1200 s
    EXPECT_EQ(ArWlBranchTestAccess::runCount(*f.resolver), 0u);
    // Short persistence: the floor of 30 s makes the limit 300 s.
    Fixture g(branchConfig(true, 0.0, 5.0), gpsL1L2(), "prune_short");
    ArWlBranchTestAccess::addRun(*g.resolver, "G01|G02|L1C|L2W", WlBranchRun{ 4, 0.0, 10000.0 });
    ArWlBranchTestAccess::prune(*g.resolver, 10300.0);
    EXPECT_EQ(ArWlBranchTestAccess::runCount(*g.resolver), 1u);
    ArWlBranchTestAccess::prune(*g.resolver, 10301.0);
    EXPECT_EQ(ArWlBranchTestAccess::runCount(*g.resolver), 0u);
    // A last time in the future (week rollover) goes too.
    ArWlBranchTestAccess::addRun(*g.resolver, "G01|G02|L1C|L2W", WlBranchRun{ 4, 600000.0, 604000.0 });
    ArWlBranchTestAccess::prune(*g.resolver, 100.0);
    EXPECT_EQ(ArWlBranchTestAccess::runCount(*g.resolver), 0u);
}

TEST(ArWlBranchResolverTest, AContradictionStillReturnsTheSatelliteWhenNothingIsLeftToSearch) {
    // No pair is admitted, so the search never runs, but the persisted contradiction is an
    // event of the decision and is reported.
    AmbiguityResolverConfig cfg = branchConfig(true, 0.0, 0.0);
    cfg.nl_max_per_pair_frac = 0.001;
    Fixture f(cfg, gpsL1L2(), "on_contradiction_nothing");
    const auto r = f.run(stateFor(f.band, 3.2, 4, 0.02), tightP());
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.bias_reset_satellites, (std::vector<std::string>{ "G13" }));
}

TEST(ArWlBranchResolverTest, GalileoE1E5aBehavesAsWithTheKeyOffAtAFortyThousandthFloor) {
    const Band gal = galE1E5a();
    // A pair the code admits.
    {
        Fixture on(branchConfig(true, 0.04, 0.0), gal, "gal_on_admit");
        Fixture off(branchConfig(false, 0.04), gal, "gal_off_admit");
        const Eigen::VectorXd x = stateFor(gal, 3.02, 3, 0.02);
        const auto a = on.run(x, tightP());
        const auto b = off.run(x, tightP());
        ASSERT_TRUE(admitted(b));
        ASSERT_TRUE(admitted(a));
        EXPECT_EQ(a.held_pairs.at(0).wl, b.held_pairs.at(0).wl);
        EXPECT_EQ(a.held_pairs.at(0).n1, b.held_pairs.at(0).n1);
        EXPECT_EQ(on.resolver->wlBranchCounters().by_phase, 0);
        EXPECT_EQ(on.resolver->wlBranchCounters().contradictions, 0);
    }
    // A pair the code refuses, with an N_1 float close to an integer for W1: the phase cannot decide
    // across a gap of 0.051 with a floor of 0.04, so it stays refused.
    {
        Fixture on(branchConfig(true, 0.04, 0.0), gal, "gal_on_refuse");
        Fixture off(branchConfig(false, 0.04), gal, "gal_off_refuse");
        const Eigen::VectorXd x = stateFor(gal, 3.45, 4, 0.002);
        EXPECT_FALSE(admitted(off.run(x, tightP())));
        EXPECT_FALSE(admitted(on.run(x, tightP())));
        EXPECT_EQ(on.resolver->wlBranchCounters().by_phase, 0);
        EXPECT_NE(on.logText().find("phase=none"), std::string::npos) << on.logText();
    }
}

TEST(ArWlBranchResolverTest, TheKeyIsInertWithoutWlFromFloatOrTheIonoFreeRow) {
    for (const bool wl_from_float : { false, true }) {
        AmbiguityResolverConfig cfg = branchConfig(true);
        cfg.wl_from_float    = wl_from_float;
        cfg.nl_iono_free_row = !wl_from_float;      // exactly one of the two needed keys is on
        Fixture f(cfg, gpsL1L2(), std::string("inert_") + (wl_from_float ? "a" : "b"));
        EXPECT_FALSE(f.resolver->wlBranchActive());
        f.run(stateFor(f.band, 3.45, 4, 0.02), tightP());
        EXPECT_EQ(f.resolver->wlBranchCounters().evaluated, 0);
        EXPECT_EQ(f.logText().find("WLBRANCH"), std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// The key
// ---------------------------------------------------------------------------

namespace {

struct Loaded {
    GnssConfig  cfg;
    std::string log_text;
    bool        ok{ false };
};

/// A configuration loaded through the loader, with the "gnss_config" log captured.
Loaded loadWithLog(const std::string& ar_body, const std::string& tag) {
    Loaded out;
    const fs::path dir = gnss_test::tempDir() / ("ar_wl_branch_cfg_" + tag);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    LogManager log;
    spdlog::drop("gnss_config");
    log.initialize(dir, LogLevel::Info);
    std::ostringstream text;
    log.getLogger("gnss_config")->sinks().push_back(
        std::make_shared<spdlog::sinks::ostream_sink_mt>(text));
    const fs::path file = dir / "config.json";
    {
        std::ofstream f(file);
        f << R"({ "processing": { "mode": "ppp-rtk", "active_systems": ["GPS"] },
                  "estimation": { "ambiguity_resolution": { )" << ar_body << R"( } } })";
    }
    out.ok = out.cfg.loadFromJson(file, log);
    log.getLogger("gnss_config")->flush();
    out.log_text = text.str();
    spdlog::drop("gnss_config");
    return out;
}

} // namespace

TEST(ArWlBranchConfigTest, TheDefaultIsOffInTheStructTheConfigAndTheLoadedFile) {
    EXPECT_FALSE(AmbiguityResolverConfig{}.wl_branch_check);
    EXPECT_FALSE(GnssConfig{}.getArWlBranchCheck());
    EXPECT_DOUBLE_EQ(AmbiguityResolverConfig{}.wl_branch_persist_s, 120.0);
    EXPECT_DOUBLE_EQ(GnssConfig{}.getArWlBranchPersistS(), 120.0);
    const Loaded l = loadWithLog(R"("mode": 3)", "default");
    ASSERT_TRUE(l.ok);
    EXPECT_FALSE(l.cfg.getArWlBranchCheck());
    EXPECT_DOUBLE_EQ(l.cfg.getArWlBranchPersistS(), 120.0);
    EXPECT_EQ(l.log_text.find("wl_branch_check"), std::string::npos) << l.log_text;
}

TEST(ArWlBranchConfigTest, ThePersistenceTimeIsParsedCopiedAndShownInTheInfoLine) {
    const Loaded l = loadWithLog(
        R"("mode": 3, "wl_from_float": true, "nl_iono_free_row": true, "wl_branch_check": true,
           "wl_branch_persist_s": 45.0)", "persist_parsed");
    ASSERT_TRUE(l.ok);
    EXPECT_DOUBLE_EQ(l.cfg.getArWlBranchPersistS(), 45.0);
    EXPECT_DOUBLE_EQ(makeAmbiguityResolverConfig(l.cfg).wl_branch_persist_s, 45.0);
    EXPECT_NE(l.log_text.find("decisive for 45 s (wl_branch_persist_s)"), std::string::npos) << l.log_text;
    // Zero is valid and means immediate.
    const Loaded z = loadWithLog(
        R"("mode": 3, "wl_from_float": true, "nl_iono_free_row": true, "wl_branch_check": true,
           "wl_branch_persist_s": 0)", "persist_zero");
    ASSERT_TRUE(z.ok);
    EXPECT_DOUBLE_EQ(z.cfg.getArWlBranchPersistS(), 0.0);
    EXPECT_EQ(z.log_text.find("negative"), std::string::npos) << z.log_text;
}

TEST(ArWlBranchConfigTest, ANegativePersistenceTimeIsRefusedWithAWarningAndTheDefaultIsKept) {
    const Loaded l = loadWithLog(
        R"("mode": 3, "wl_from_float": true, "nl_iono_free_row": true, "wl_branch_check": true,
           "wl_branch_persist_s": -5.0)", "persist_negative");
    ASSERT_TRUE(l.ok);
    EXPECT_DOUBLE_EQ(l.cfg.getArWlBranchPersistS(), 120.0);
    EXPECT_NE(l.log_text.find("wl_branch_persist_s = -5 is negative"), std::string::npos) << l.log_text;
}

TEST(ArWlBranchConfigTest, TheKeyIsParsedAndAnnouncedWhenBothKeysItNeedsAreOn) {
    const Loaded l = loadWithLog(
        R"("mode": 3, "wl_from_float": true, "nl_iono_free_row": true, "wl_branch_check": true)", "parsed");
    ASSERT_TRUE(l.ok);
    EXPECT_TRUE(l.cfg.getArWlBranchCheck());
    EXPECT_NE(l.log_text.find("(wl_branch_check)"), std::string::npos) << l.log_text;
    EXPECT_EQ(l.log_text.find("is inert"), std::string::npos) << l.log_text;
    EXPECT_TRUE(makeAmbiguityResolverConfig(l.cfg).wl_branch_check);
}

TEST(ArWlBranchConfigTest, TheKeyIsInertWithAWarningWithoutWlFromFloat) {
    const Loaded l = loadWithLog(
        R"("mode": 3, "nl_iono_free_row": true, "wl_branch_check": true)", "no_float");
    ASSERT_TRUE(l.ok);
    EXPECT_FALSE(l.cfg.getArWlBranchCheck());
    EXPECT_NE(l.log_text.find("wl_branch_check needs wl_from_float and nl_iono_free_row"), std::string::npos)
        << l.log_text;
    EXPECT_NE(l.log_text.find("wl_from_float off, nl_iono_free_row on"), std::string::npos) << l.log_text;
}

TEST(ArWlBranchConfigTest, TheKeyIsInertWithAWarningWithoutTheIonoFreeRow) {
    const Loaded l = loadWithLog(
        R"("mode": 3, "wl_from_float": true, "wl_branch_check": true)", "no_row");
    ASSERT_TRUE(l.ok);
    EXPECT_FALSE(l.cfg.getArWlBranchCheck());
    EXPECT_NE(l.log_text.find("wl_from_float on, nl_iono_free_row off"), std::string::npos) << l.log_text;
}
