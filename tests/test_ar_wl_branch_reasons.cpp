/**
 * @file test_ar_wl_branch_reasons.cpp
 * @brief Boundary and ordering tests for the wide-lane helpers of user integer fixing:
 *        the wide-lane decision of a satellite pair, the persistence of that decision,
 *        the float wide lane, the N_1 search-row coefficients and the shared-reference
 *        error floor of the search covariance.
 *
 * The neighbouring files test each row of the decision table and the basic algebra of
 * the helpers.  This file pins what they leave open: that every comparison of the
 * decision is inclusive at its limit, the order in which the rows are tried, what the
 * decision does with the not-a-number inputs the resolver hands it for a pair it cannot
 * form an iono-free row for, that on Galileo E1/E5a a narrow-lane floor of 0.04 cycle
 * leaves the phase without a say in any branch of the table (code admitting or not),
 * that persistence follows the absolute wide-lane integer rather than its W0/W1 label,
 * and the guards and sign conventions of the float wide lane.  Every expected number is
 * derived in the test from the frequencies or from exactly representable inputs.
 *
 * Suite name @c ArWlBranchReasons is owned solely by this file.
 */

#include <gtest/gtest.h>

#include "common/Constants.hpp"
#include "filter/AmbiguityResolver.hpp"
#include "logging/LogManager.hpp"
#include "support/TestTempDir.hpp"

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace GnssProcessor {

// The three access shims below are befriended in AmbiguityResolver.hpp and are also
// defined in tests/test_ar_float_widelane.cpp, tests/test_nl_search_row.cpp and
// tests/test_ar_wl_branch.cpp.  One type defined in several translation units must be
// token for token the same in each, so these are exact copies: change all or none.

/// Test seam for the private static helpers (friend in AmbiguityResolver.hpp).
struct ArFloatWlTestAccess {
    using Pair = AmbiguityResolver::DdPair;
    using Fw   = AmbiguityResolver::FloatWidelane;
    static Fw wl(const Pair& p1, const Pair& p2, double l1, double l2,
                 const Eigen::VectorXd& x, const Eigen::MatrixXd& P) {
        return AmbiguityResolver::floatWidelane_(p1, p2, l1, l2, x, P);
    }
    static void floor(Eigen::MatrixXd& Q, const std::vector<std::string>& ref, double f) {
        AmbiguityResolver::addSharedReferenceFloor_(Q, ref, f);
    }
};

/// Test seam: nlRowCoefficients is a private static (see the friend
/// declaration in AmbiguityResolver.hpp).  Defined only in the test binary.
struct NlRowTestAccess {
    using Coefs = AmbiguityResolver::NlRowCoefficients;
    static Coefs row(double f1, double f2, double lam1, double lam2, bool iono_free) {
        return AmbiguityResolver::nlRowCoefficients(f1, f2, lam1, lam2, iono_free);
    }
};

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

constexpr double kFL1  = 1575.42e6;   // GPS L1, Galileo E1
constexpr double kFL2  = 1227.60e6;   // GPS L2
constexpr double kFE5a = 1176.45e6;   // Galileo E5a, GPS L5

double lamOf(double f) { return Constants::SPEED_OF_LIGHT / f; }

/// Wrap to [-0.5, 0.5].
double wrap(double v) { return v - std::round(v); }

/// The wide-lane shift of the iono-free N_1 float, -f2/(f1-f2), from the frequencies.
double ifShift(double f1, double f2) { return -f2 / (f1 - f2); }

/// The branch gap as the resolver forms it: the distance of the shift from an integer.
double gapOf(double f1, double f2) {
    const double s = ifShift(f1, f2);
    return std::abs(s - std::round(s));
}

constexpr double kWlMax       = 0.40;
constexpr double kSigmaTiny   = 0.005;   // a well determined wide lane
constexpr double kNotDecisive = 0.30;    // a fraction no phase test passes on these gaps

} // namespace

// ---------------------------------------------------------------------------
// The decision: limits are inclusive
// ---------------------------------------------------------------------------

TEST(ArWlBranchReasons, ReachIsInclusiveWhenOnePlusDEqualsThreeSigma) {
    // w = 3.5 rounds away from zero to 4, so d = -0.5 and 1 + |d| = 1.5 = 3 * 0.5 exactly.
    const auto at = decideWlBranch(3.5, 0.5, kNotDecisive, kNotDecisive, 0.01, 0.4706, kWlMax);
    EXPECT_EQ(at.reason, WlBranchReason::Reach);
    EXPECT_EQ(at.integer, WlBranchChoice::None);
    // Just below the limit the reach test passes and the row the code refuses is reached.
    const auto below = decideWlBranch(3.5, std::nextafter(0.5, 0.0), kNotDecisive, kNotDecisive,
                                      0.01, 0.4706, kWlMax);
    EXPECT_EQ(below.reason, WlBranchReason::Undecided);
    EXPECT_TRUE(below.w1_candidate);
}

TEST(ArWlBranchReasons, CodeAdmissionIsInclusiveAtTheLimit) {
    // |d| + 3 sigma_w = 0.25 + 0.1875 = 0.4375, all exact in binary.
    const auto at = decideWlBranch(3.25, 0.0625, kNotDecisive, kNotDecisive, 0.01, 0.4706, 0.4375);
    EXPECT_TRUE(at.code_admits);
    EXPECT_EQ(at.integer, WlBranchChoice::W0);
    EXPECT_EQ(at.reason, WlBranchReason::Code);
    const auto past = decideWlBranch(3.25, 0.0625, kNotDecisive, kNotDecisive, 0.01, 0.4706,
                                     std::nextafter(0.4375, 0.0));
    EXPECT_FALSE(past.code_admits);
    EXPECT_EQ(past.reason, WlBranchReason::Undecided);
}

TEST(ArWlBranchReasons, PhaseDecisivenessIsInclusiveAtHalfTheGapAndUsesTheSizeOfTheFraction) {
    // gap 0.5: |r| + 3 sigma_n = 0.15625 + 0.09375 = 0.25 = gap / 2, all exact in binary.
    // The code refuses (|d| = 0.45), so a decisive phase for W0 decides.
    const double gap = 0.5, sigma_n = 0.03125, r = 0.15625;
    for (const double sign : { 1.0, -1.0 }) {
        const auto at = decideWlBranch(3.45, kSigmaTiny, sign * r, 0.4, sigma_n, gap, kWlMax);
        EXPECT_EQ(at.phase, WlBranchChoice::W0) << "sign " << sign;
        EXPECT_EQ(at.reason, WlBranchReason::PhaseW0) << "sign " << sign;
        // One ulp of r would round back to 0.25 in the sum, so the step past the limit is 1e-9.
        const auto past = decideWlBranch(3.45, kSigmaTiny, sign * (r + 1e-9), 0.4,
                                         sigma_n, gap, kWlMax);
        EXPECT_EQ(past.phase, WlBranchChoice::None) << "sign " << sign;
        EXPECT_EQ(past.reason, WlBranchReason::Undecided) << "sign " << sign;
    }
}

TEST(ArWlBranchReasons, TheW1CandidateBoundIsInclusive) {
    // d = 0.25, 1 - |d| = 0.75 = 3 * 0.25 exactly; the code refuses (0.25 + 0.75 > 0.40).
    const auto at = decideWlBranch(3.25, 0.25, kNotDecisive, 0.0, 0.001, 0.4706, kWlMax);
    EXPECT_TRUE(at.w1_candidate);
    EXPECT_EQ(at.reason, WlBranchReason::PhaseW1);
    const auto below = decideWlBranch(3.25, std::nextafter(0.25, 0.0), kNotDecisive, 0.0, 0.001,
                                      0.4706, kWlMax);
    EXPECT_FALSE(below.w1_candidate);
    EXPECT_EQ(below.reason, WlBranchReason::Undecided);
}

// ---------------------------------------------------------------------------
// The decision: the order of the rows
// ---------------------------------------------------------------------------

TEST(ArWlBranchReasons, ReachIsTriedBeforeACodeAdmissionUnderALooseLimit) {
    // With a limit of 2 cycles the code admits (0 + 1.2 <= 2), yet three integers lie within
    // reach (1 <= 1.2): reach wins, and the code's own verdict is still reported.
    const auto d = decideWlBranch(3.0, 0.4, 0.0, kNotDecisive, 0.001, 0.4706, 2.0);
    EXPECT_EQ(d.reason, WlBranchReason::Reach);
    EXPECT_EQ(d.integer, WlBranchChoice::None);
    EXPECT_TRUE(d.code_admits);
    EXPECT_EQ(d.phase, WlBranchChoice::None);   // the phase is not evaluated after reach
}

TEST(ArWlBranchReasons, WhenTheCodeAdmitsW1IsNeverACandidateEvenIfTheCodeCannotExcludeIt) {
    // Loose limit 2: d = 0.4, sigma_w = 0.25.  1 - 0.4 = 0.6 <= 0.75, so the code alone could
    // not exclude W1, but it admits W0 (0.4 + 0.75 <= 2).  A phase decisive for W1 is then a
    // contradiction, never a decision for W1.
    const auto d = decideWlBranch(3.4, 0.25, kNotDecisive, 0.0, 0.001, 0.4706, 2.0);
    EXPECT_TRUE(d.code_admits);
    EXPECT_FALSE(d.w1_candidate);
    EXPECT_EQ(d.phase, WlBranchChoice::W1);
    EXPECT_EQ(d.reason, WlBranchReason::Contradiction);
    EXPECT_EQ(d.integer, WlBranchChoice::None);
}

TEST(ArWlBranchReasons, InputsClaimingBothIntegersDecisiveLeaveTheCodeAdmissionStanding) {
    // The table's "decisive for neither" reading of inconsistent fractions must not turn a
    // code admission into a contradiction.
    const auto d = decideWlBranch(3.1, kSigmaTiny, 0.01, -0.01, 0.001, 0.4706, kWlMax);
    EXPECT_EQ(d.phase, WlBranchChoice::None);
    EXPECT_EQ(d.integer, WlBranchChoice::W0);
    EXPECT_EQ(d.reason, WlBranchReason::Code);
}

TEST(ArWlBranchReasons, NotANumberFractionsLeaveTheDecisionToTheCodeAlone) {
    // For a pair whose frequencies are not in descending order the resolver passes NaN
    // fractions, a NaN sigma_n and a gap of 0.  The phase then decides nothing.
    const auto admit = decideWlBranch(3.02, kSigmaTiny, kNaN, kNaN, kNaN, 0.0, kWlMax);
    EXPECT_EQ(admit.phase, WlBranchChoice::None);
    EXPECT_EQ(admit.integer, WlBranchChoice::W0);
    EXPECT_EQ(admit.reason, WlBranchReason::Code);
    const auto refuse = decideWlBranch(3.45, 0.2, kNaN, kNaN, kNaN, 0.0, kWlMax);
    EXPECT_EQ(refuse.phase, WlBranchChoice::None);
    EXPECT_EQ(refuse.integer, WlBranchChoice::None);
    EXPECT_EQ(refuse.reason, WlBranchReason::Undecided);
    // Finite fractions with a NaN sigma_n are not decisive either.
    const auto nan_sigma = decideWlBranch(3.45, 0.2, 0.0, kNotDecisive, kNaN, 0.4706, kWlMax);
    EXPECT_EQ(nan_sigma.phase, WlBranchChoice::None);
    EXPECT_EQ(nan_sigma.reason, WlBranchReason::Undecided);
}

TEST(ArWlBranchReasons, ANotFiniteFloatLeavesEveryOtherFieldAtItsDefault) {
    const auto d = decideWlBranch(kNaN, kSigmaTiny, 0.0, kNotDecisive, 0.001, 0.4706, kWlMax);
    EXPECT_EQ(d.reason, WlBranchReason::NotFinite);
    EXPECT_FALSE(d.code_admits);
    EXPECT_FALSE(d.w1_candidate);
    EXPECT_EQ(d.phase, WlBranchChoice::None);
    const auto neg_inf = decideWlBranch(3.02, -std::numeric_limits<double>::infinity(), 0.0,
                                        kNotDecisive, 0.001, 0.4706, kWlMax);
    EXPECT_EQ(neg_inf.reason, WlBranchReason::NotFinite);
    EXPECT_FALSE(neg_inf.code_admits);
}

// ---------------------------------------------------------------------------
// The decision on Galileo E1/E5a
// ---------------------------------------------------------------------------

TEST(ArWlBranchReasons, TheBranchGapFollowsFromTheIonoFreeRowShift) {
    // The resolver takes the gap from the shift of the iono-free row; the row's shift is
    // -f2/(f1-f2), so the gap is that number's distance from an integer.
    const auto gps = NlRowTestAccess::row(kFL1, kFL2, lamOf(kFL1), lamOf(kFL2), true);
    const auto gal = NlRowTestAccess::row(kFL1, kFE5a, lamOf(kFL1), lamOf(kFE5a), true);
    EXPECT_NEAR(gps.shift, ifShift(kFL1, kFL2), 1e-12);
    EXPECT_NEAR(gal.shift, ifShift(kFL1, kFE5a), 1e-12);
    EXPECT_NEAR(std::abs(gps.shift - std::round(gps.shift)), gapOf(kFL1, kFL2), 1e-12);
    EXPECT_NEAR(std::abs(gal.shift - std::round(gal.shift)), gapOf(kFL1, kFE5a), 1e-12);
    // Sanity on the magnitudes: about 0.47 cycle on L1/L2 and 0.05 cycle on E1/E5a.
    EXPECT_NEAR(gapOf(kFL1, kFL2), 0.4706, 1e-4);
    EXPECT_NEAR(gapOf(kFL1, kFE5a), 0.0513, 1e-4);
}

TEST(ArWlBranchReasons, AtAFloorOfFourHundredthsTheGalileoE1E5aPhaseDecidesNothingInAnyBranch) {
    // sigma_n is sqrt(var + floor^2) >= floor, so the best case is a row of zero variance.
    // 3 * 0.04 = 0.12 exceeds half the gap (about 0.026) for every fraction, so the phase is
    // never decisive: a code admission stays W0 by the code (never a contradiction) and a
    // refusal stays undecided (never a decision by the phase), with or without a W1 candidate.
    const double gap     = gapOf(kFL1, kFE5a);
    const double shift   = ifShift(kFL1, kFE5a);
    const double sigma_n = std::sqrt(0.0 + 0.04 * 0.04);
    struct Float { double w, sigma_w; bool admits; };
    const Float floats[] = { { 3.02, kSigmaTiny, true },  { 2.98, kSigmaTiny, true },
                             { 3.45, kSigmaTiny, false }, { 3.45, 0.2, false },
                             { 2.55, 0.2, false } };
    int tested = 0;
    for (const auto& fl : floats) {
        const double side = (fl.w - std::round(fl.w)) >= 0.0 ? 1.0 : -1.0;
        for (double r0 = -0.5; r0 <= 0.5; r0 += 0.001) {
            const double r1 = wrap(r0 + shift * side);
            const auto d = decideWlBranch(fl.w, fl.sigma_w, r0, r1, sigma_n, gap, kWlMax);
            ASSERT_EQ(d.phase, WlBranchChoice::None) << "w " << fl.w << " r0 " << r0;
            ASSERT_EQ(d.code_admits, fl.admits) << "w " << fl.w;
            ASSERT_EQ(d.reason, fl.admits ? WlBranchReason::Code : WlBranchReason::Undecided)
                << "w " << fl.w << " r0 " << r0;
            ++tested;
        }
    }
    EXPECT_GT(tested, 4000);
}

TEST(ArWlBranchReasons, TheGalileoPhaseCanDecideOnlyBelowASixthOfTheGap) {
    // The sweep above is not vacuous: with r = 0 the phase is decisive exactly while
    // 3 sigma_n <= gap / 2, that is sigma_n <= gap / 6 (about 0.0085 cycle on E1/E5a).
    const double gap   = gapOf(kFL1, kFE5a);
    const double shift = ifShift(kFL1, kFE5a);
    const double limit = gap / 6.0;
    const auto inside = decideWlBranch(3.45, kSigmaTiny, 0.0, wrap(shift), 0.99 * limit, gap, kWlMax);
    EXPECT_EQ(inside.phase, WlBranchChoice::W0);
    EXPECT_EQ(inside.reason, WlBranchReason::PhaseW0);
    const auto outside = decideWlBranch(3.45, kSigmaTiny, 0.0, wrap(shift), 1.01 * limit, gap, kWlMax);
    EXPECT_EQ(outside.phase, WlBranchChoice::None);
    EXPECT_LT(limit, 0.04);
    // The same floor of 0.04 on GPS L1/L2 still leaves the phase decisive near an integer.
    const auto gps = decideWlBranch(3.45, kSigmaTiny, 0.0, wrap(ifShift(kFL1, kFL2)), 0.04,
                                    gapOf(kFL1, kFL2), kWlMax);
    EXPECT_EQ(gps.phase, WlBranchChoice::W0);
}

// ---------------------------------------------------------------------------
// The persistence step
// ---------------------------------------------------------------------------

TEST(ArWlBranchReasons, ARipeRunLosesItsRipenessOnAChangeOfIntegerAndRipensAgainAfterTheTime) {
    const WlBranchRun run{ 4, 0.0, 200.0 };
    const auto ripe = stepWlBranchPersistence(run, true, 4, 230.0, 120.0);
    ASSERT_TRUE(ripe.ripe);
    EXPECT_DOUBLE_EQ(ripe.persisted_s, 230.0);
    const auto changed = stepWlBranchPersistence(ripe.run, true, 5, 260.0, 120.0);
    EXPECT_TRUE(changed.keep);
    EXPECT_FALSE(changed.ripe);
    EXPECT_EQ(changed.run.integer, 5);
    EXPECT_DOUBLE_EQ(changed.run.began_tow, 260.0);
    EXPECT_DOUBLE_EQ(changed.run.last_tow, 260.0);
    EXPECT_DOUBLE_EQ(changed.persisted_s, 0.0);
    const auto waiting = stepWlBranchPersistence(changed.run, true, 5, 379.0, 120.0);
    EXPECT_FALSE(waiting.ripe);
    const auto again = stepWlBranchPersistence(waiting.run, true, 5, 380.0, 120.0);
    EXPECT_TRUE(again.ripe);
    EXPECT_DOUBLE_EQ(again.run.began_tow, 260.0);
}

TEST(ArWlBranchReasons, TheIntegerIsComparedWithItsSign) {
    const WlBranchRun run{ -1, 100.0, 150.0 };
    EXPECT_DOUBLE_EQ(stepWlBranchPersistence(run, true, -1, 160.0, 120.0).run.began_tow, 100.0);
    EXPECT_DOUBLE_EQ(stepWlBranchPersistence(run, true, 1, 160.0, 120.0).run.began_tow, 160.0);
}

TEST(ArWlBranchReasons, ARepeatedEvaluationAtTheSameTimeContinuesTheRun) {
    // A time since the last evaluation of exactly 0 lies inside [0, persist].
    const WlBranchRun run{ 4, 100.0, 150.0 };
    const auto step = stepWlBranchPersistence(run, true, 4, 150.0, 120.0);
    EXPECT_DOUBLE_EQ(step.run.began_tow, 100.0);
    EXPECT_DOUBLE_EQ(step.persisted_s, 50.0);
}

TEST(ArWlBranchReasons, AFractionalPersistenceTimeRipensAtThatTime) {
    const auto first = stepWlBranchPersistence(std::nullopt, true, 4, 100.0, 0.5);
    EXPECT_FALSE(first.ripe);
    EXPECT_FALSE(stepWlBranchPersistence(first.run, true, 4, 100.25, 0.5).ripe);
    const auto at = stepWlBranchPersistence(first.run, true, 4, 100.5, 0.5);
    EXPECT_TRUE(at.ripe);
    EXPECT_DOUBLE_EQ(at.persisted_s, 0.5);
}

// ---------------------------------------------------------------------------
// The decision and its persistence through resolveWlConstrained
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

Band gpsL1L2() { return { System::GPS, "G13", "G12", "L1C", "L2W", kFL1, kFL2 }; }
Band galE1E5a() { return { System::Galileo, "E05", "E09", "L1C", "L5Q", kFL1, kFE5a }; }

/// States 0 and 1 on the higher frequency, 2 and 3 on the lower.
std::vector<ArWlBranchTestAccess::Pair> pairsOf(const Band& b) {
    return { { 0, 1, b.sat_i, b.sat_ref, b.sig_hi }, { 2, 3, b.sat_i, b.sat_ref, b.sig_lo } };
}

/**
 * The state with float wide lane @p w whose iono-free N_1 float for the wide-lane integer
 * @p w_target has the fraction @p frac_target (N_1 near 8).  The N_1 float for another
 * integer W is then 8 + frac_target + slope * (w_target - W).
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

/// Adds variance that raises sigma_w to about @p sigma_w_cyc and leaves the iono-free row alone.
void widenWidelaneOnly(Eigen::MatrixXd& P, const Band& b, double sigma_w_cyc) {
    const double c  = Constants::SPEED_OF_LIGHT;
    const double t1 = b.f1 * b.f1 / ((b.f1 - b.f2) * c);
    const double t2 = -(b.f2 * b.f2) / ((b.f1 - b.f2) * c);
    const double u1 = -t2, u2 = t1;                      // t1 u1 + t2 u2 = 0
    const double along_w = u1 / b.lam1() - u2 / b.lam2();
    const double s = sigma_w_cyc / std::abs(along_w);
    Eigen::Vector4d u(u1, 0.0, u2, 0.0);
    P += s * s * u * u.transpose();
}

AmbiguityResolverConfig branchConfig(double floor_cyc, double persist_s) {
    AmbiguityResolverConfig c;
    c.ar_mode             = 3;
    c.wl_from_float       = true;
    c.nl_iono_free_row    = true;
    c.wl_branch_check     = true;
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

/// A resolver that is driven directly, with its "ambiguity_resolver" log captured.
struct Fixture {
    LogManager         log;
    std::ostringstream text;
    std::unique_ptr<AmbiguityResolver> resolver;
    Band band;

    Fixture(const AmbiguityResolverConfig& cfg, const Band& b, const std::string& tag) : band(b) {
        const fs::path dir = gnss_test::tempDir() / ("ar_wl_branch_reasons_" + tag);
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

    AmbiguityResolution run(const Eigen::VectorXd& x, const Eigen::MatrixXd& P, double tow) {
        auto r = ArWlBranchTestAccess::run(*resolver, pairsOf(band), x, P, obsOf(band, tow));
        log.getLogger("ambiguity_resolver")->flush();
        return r;
    }

    /// The WLBRANCH line of the epoch at @p tow, or an empty string.
    std::string branchLine(double tow) {
        std::ostringstream key;
        key.setf(std::ios::fixed);
        key.precision(1);
        key << "WLBRANCH tow=" << tow << ' ';
        const std::string all = text.str();
        const auto at = all.find(key.str());
        if (at == std::string::npos) return {};
        return all.substr(at, all.find('\n', at) - at);
    }
};

/// True when @p r constrained the pair (it enters the search and the fix is accepted).
bool admitted(const AmbiguityResolution& r) { return r.success && !r.fixed_dd_rows.empty(); }

} // namespace

TEST(ArWlBranchReasons, PersistenceFollowsTheAbsoluteIntegerNotItsW0OrW1Label) {
    // The phase stays decisive for the wide-lane integer 4 while the float crosses the half
    // cycle: at 3.45 the integer 4 is W1, at 3.55 it is W0.  The run continues across the
    // change of label and ripens 120 s after it began.
    Fixture f(branchConfig(0.0, 120.0), gpsL1L2(), "label");
    Eigen::MatrixXd P = tightP();
    widenWidelaneOnly(P, f.band, 0.2);                  // W1 is a candidate at |d| = 0.45
    const Eigen::VectorXd as_w1 = stateFor(f.band, 3.45, 4, 0.02);
    const Eigen::VectorXd as_w0 = stateFor(f.band, 3.55, 4, 0.02);
    EXPECT_FALSE(admitted(f.run(as_w1, P, 1000.0)));
    EXPECT_FALSE(admitted(f.run(as_w0, P, 1060.0)));
    EXPECT_NE(f.branchLine(1060.0).find("w0=4 w1=3"), std::string::npos) << f.branchLine(1060.0);
    EXPECT_NE(f.branchLine(1060.0).find("phase=w0 persist=60.0 decision=none reason=waiting"),
              std::string::npos) << f.branchLine(1060.0);
    const auto r = f.run(as_w1, P, 1120.0);
    ASSERT_TRUE(admitted(r)) << f.branchLine(1120.0);
    EXPECT_EQ(r.held_pairs.at(0).wl, 4);
    EXPECT_NE(f.branchLine(1120.0).find("persist=120.0 decision=w1 reason=phase_w1"),
              std::string::npos) << f.branchLine(1120.0);
    EXPECT_EQ(f.resolver->wlBranchCounters().waiting, 2);
    EXPECT_EQ(f.resolver->wlBranchCounters().by_phase, 1);
}

TEST(ArWlBranchReasons, AnAlternatingIntegerRestartsTheRunAtEveryChange) {
    // The same float, with the phase decisive for 4, then 3, then 4 again: each change of
    // the integer restarts the run, so nothing is admitted 120 s after the first epoch, and
    // the pair is admitted only 120 s after the last change.
    Fixture f(branchConfig(0.0, 120.0), gpsL1L2(), "alternate");
    Eigen::MatrixXd P = tightP();
    widenWidelaneOnly(P, f.band, 0.2);
    const Eigen::VectorXd for4 = stateFor(f.band, 3.45, 4, 0.02);   // W1 = 4 decisive
    const Eigen::VectorXd for3 = stateFor(f.band, 3.45, 3, 0.02);   // W0 = 3 decisive
    EXPECT_FALSE(admitted(f.run(for4, P, 1000.0)));
    EXPECT_FALSE(admitted(f.run(for3, P, 1060.0)));
    EXPECT_NE(f.branchLine(1060.0).find("phase=w0 persist=0.0 decision=none reason=waiting"),
              std::string::npos) << f.branchLine(1060.0);
    EXPECT_FALSE(admitted(f.run(for4, P, 1120.0)));
    EXPECT_NE(f.branchLine(1120.0).find("phase=w1 persist=0.0 decision=none reason=waiting"),
              std::string::npos) << f.branchLine(1120.0);
    EXPECT_FALSE(admitted(f.run(for4, P, 1180.0)));
    const auto r = f.run(for4, P, 1240.0);
    ASSERT_TRUE(admitted(r)) << f.branchLine(1240.0);
    EXPECT_EQ(r.held_pairs.at(0).wl, 4);
    EXPECT_EQ(f.resolver->wlBranchCounters().waiting, 4);
    EXPECT_EQ(f.resolver->wlBranchCounters().by_phase, 1);
}

TEST(ArWlBranchReasons, OnGalileoE1E5aTheFloorTurnsAContradictionIntoACodeAdmission) {
    // d = 0.2: the code admits W0 = 3.  The N_1 float for W1 = 4 sits on an integer and the
    // one for W0 sits 0.051 cycle (the gap) away.  Without a floor the tight row makes the
    // phase decisive for W1: a contradiction, the satellite is returned and nothing is fixed.
    // With the floor of 0.04 the phase cannot decide, and the code's W0 is fixed.
    const Band gal = galE1E5a();
    const Eigen::VectorXd x = stateFor(gal, 3.2, 4, 0.0);
    {
        Fixture f(branchConfig(0.0, 0.0), gal, "gal_nofloor");
        const auto r = f.run(x, tightP(), 500.0);
        EXPECT_FALSE(admitted(r));
        EXPECT_EQ(r.bias_reset_satellites, (std::vector<std::string>{ "E05" }));
        EXPECT_EQ(f.resolver->wlBranchCounters().contradictions, 1);
        EXPECT_NE(f.branchLine(500.0).find("code=admit cand1=0 phase=w1"), std::string::npos)
            << f.branchLine(500.0);
        EXPECT_NE(f.branchLine(500.0).find("reason=contradiction"), std::string::npos);
    }
    {
        Fixture f(branchConfig(0.04, 0.0), gal, "gal_floor");
        const auto r = f.run(x, tightP(), 500.0);
        ASSERT_TRUE(admitted(r)) << f.branchLine(500.0);
        EXPECT_EQ(r.held_pairs.at(0).wl, 3);
        EXPECT_TRUE(r.bias_reset_satellites.empty());
        EXPECT_EQ(f.resolver->wlBranchCounters().contradictions, 0);
        EXPECT_NE(f.branchLine(500.0).find("phase=none persist=0.0 decision=w0 reason=code"),
                  std::string::npos) << f.branchLine(500.0);
    }
}

// ---------------------------------------------------------------------------
// The float wide lane
// ---------------------------------------------------------------------------

namespace {

// State layout: 0 = sat_i L1, 1 = ref L1, 2 = sat_i L2, 3 = ref L2 (metres).
ArFloatWlTestAccess::Pair hiPair() { return { 0, 1, "G13", "G12", "L1C" }; }
ArFloatWlTestAccess::Pair loPair() { return { 2, 3, "G13", "G12", "L2W" }; }

} // namespace

TEST(ArWlBranchReasons, ANegativeVarianceGivesNaNInBothTheFloatAndItsSigma) {
    const double l1 = lamOf(kFL1), l2 = lamOf(kFL2);
    Eigen::VectorXd x(4);
    x << 5 * l1, 0.0, 3 * l2, 0.0;
    const Eigen::MatrixXd P = -Eigen::MatrixXd::Identity(4, 4) * 1e-6;
    const auto fw = ArFloatWlTestAccess::wl(hiPair(), loPair(), l1, l2, x, P);
    EXPECT_TRUE(std::isnan(fw.w));        // not only the square root: the float is withheld too
    EXPECT_TRUE(std::isnan(fw.sigma));
}

TEST(ArWlBranchReasons, AnIndexOutsideTheCovarianceOrBelowZeroGivesNaN) {
    const double l1 = lamOf(kFL1), l2 = lamOf(kFL2);
    const Eigen::VectorXd x = Eigen::VectorXd::Ones(4);
    const Eigen::MatrixXd P3 = Eigen::MatrixXd::Identity(3, 3);     // the state has 4 entries
    const auto short_p = ArFloatWlTestAccess::wl(hiPair(), loPair(), l1, l2, x, P3);
    EXPECT_TRUE(std::isnan(short_p.w));
    EXPECT_TRUE(std::isnan(short_p.sigma));
    ArFloatWlTestAccess::Pair bad = hiPair();
    bad.state_idx_ref = -1;
    const auto negative = ArFloatWlTestAccess::wl(bad, loPair(), l1, l2, x,
                                                  Eigen::MatrixXd::Identity(4, 4));
    EXPECT_TRUE(std::isnan(negative.w));
    EXPECT_TRUE(std::isnan(negative.sigma));
}

TEST(ArWlBranchReasons, TheReferenceEntersWithTheOppositeSignAndSwappingTheBandsNegates) {
    const double l1 = lamOf(kFL1), l2 = lamOf(kFL2);
    // Only the reference's L1 ambiguity is non-zero: 3 cycles enter as -3.
    Eigen::VectorXd x = Eigen::VectorXd::Zero(4);
    x(1) = 3 * l1;
    Eigen::MatrixXd P(4, 4);
    P << 4.0, 1.0, 0.5, 0.2,
         1.0, 3.0, 0.1, 0.4,
         0.5, 0.1, 5.0, 1.5,
         0.2, 0.4, 1.5, 2.0;
    P *= 1e-4;
    const auto fw = ArFloatWlTestAccess::wl(hiPair(), loPair(), l1, l2, x, P);
    EXPECT_NEAR(fw.w, -3.0, 1e-9);
    // The lower band first (as the extra-wide lane uses it) gives the negated float, same sigma.
    x << 7 * l1, 2 * l1, 4 * l2, 1 * l2;                 // DD 5 and 3: wide lane 2
    const auto ab = ArFloatWlTestAccess::wl(hiPair(), loPair(), l1, l2, x, P);
    const auto ba = ArFloatWlTestAccess::wl(loPair(), hiPair(), l2, l1, x, P);
    EXPECT_NEAR(ab.w, 2.0, 1e-9);
    EXPECT_NEAR(ba.w, -2.0, 1e-9);
    EXPECT_NEAR(ba.sigma, ab.sigma, 1e-15);
}

TEST(ArWlBranchReasons, EntriesOutsideThePairDoNotEnterTheFloatOrItsSigma) {
    const double l1 = lamOf(kFL1), l2 = lamOf(kFL2);
    Eigen::VectorXd x4(4);
    x4 << 7 * l1, 2 * l1, 4 * l2, 1 * l2;
    Eigen::MatrixXd P4 = Eigen::MatrixXd::Identity(4, 4) * 1e-4;
    P4(0, 2) = P4(2, 0) = 3e-5;
    // The same pair embedded in a six-entry state with large unrelated values and covariances.
    Eigen::VectorXd x6(6);
    x6 << 7 * l1, 2 * l1, 4 * l2, 1 * l2, 1e3, -2e3;
    Eigen::MatrixXd P6 = Eigen::MatrixXd::Identity(6, 6) * 50.0;
    P6.topLeftCorner(4, 4) = P4;
    P6(0, 4) = P6(4, 0) = 0.7;
    P6(3, 5) = P6(5, 3) = -0.9;
    const auto a = ArFloatWlTestAccess::wl(hiPair(), loPair(), l1, l2, x4, P4);
    const auto b = ArFloatWlTestAccess::wl(hiPair(), loPair(), l1, l2, x6, P6);
    EXPECT_DOUBLE_EQ(a.w, b.w);
    EXPECT_DOUBLE_EQ(a.sigma, b.sigma);
}

// ---------------------------------------------------------------------------
// The shared-reference floor
// ---------------------------------------------------------------------------

TEST(ArWlBranchReasons, AZeroFloorLeavesTheMatrixUnchanged) {
    Eigen::MatrixXd Q(3, 3);
    Q << 2e-4, 3e-5, -1e-5,
         3e-5, 4e-4, 2e-5,
        -1e-5, 2e-5, 9e-4;
    const Eigen::MatrixXd before = Q;
    ArFloatWlTestAccess::floor(Q, { "G12", "G12", "E05" }, 0.0);
    EXPECT_TRUE(Q == before);
}

TEST(ArWlBranchReasons, TheFloorAddsToExistingCovarianceOnlyBetweenEntriesOnOneReference) {
    // Entries 0 and 2 share G12, entry 1 is on E05.  Existing correlations are kept and the
    // floor adds f^2 on the diagonal and f^2/2 between 0 and 2 only.
    Eigen::MatrixXd Q(3, 3);
    Q << 2e-4, 3e-5, -1e-5,
         3e-5, 4e-4, 2e-5,
        -1e-5, 2e-5, 9e-4;
    const Eigen::MatrixXd before = Q;
    const double f = 0.04, f2 = f * f;
    ArFloatWlTestAccess::floor(Q, { "G12", "E05", "G12" }, f);
    for (int k = 0; k < 3; ++k) EXPECT_NEAR(Q(k, k), before(k, k) + f2, 1e-15) << k;
    EXPECT_NEAR(Q(0, 2), before(0, 2) + 0.5 * f2, 1e-15);
    EXPECT_NEAR(Q(2, 0), before(2, 0) + 0.5 * f2, 1e-15);
    EXPECT_EQ(Q(0, 1), before(0, 1));
    EXPECT_EQ(Q(1, 0), before(1, 0));
    EXPECT_EQ(Q(1, 2), before(1, 2));
    EXPECT_EQ(Q(2, 1), before(2, 1));
    EXPECT_TRUE(Q == Q.transpose());
}

TEST(ArWlBranchReasons, AnEmptyMatrixIsLeftEmpty) {
    Eigen::MatrixXd Q(0, 0);
    ArFloatWlTestAccess::floor(Q, {}, 0.04);
    EXPECT_EQ(Q.rows(), 0);
}

// ---------------------------------------------------------------------------
// The N_1 search-row coefficients
// ---------------------------------------------------------------------------

TEST(ArWlBranchReasons, TheIonoFreeRowIsTheClosedFormOfTheTwoFrequencies) {
    struct Pair { const char* name; double f1, f2; };
    const Pair pairs[] = { { "GPS L1/L2", kFL1, kFL2 }, { "Galileo E1/E5a", kFL1, kFE5a } };
    const double c = Constants::SPEED_OF_LIGHT;
    for (const auto& p : pairs) {
        const auto row = NlRowTestAccess::row(p.f1, p.f2, lamOf(p.f1), lamOf(p.f2), true);
        const double df = p.f1 - p.f2;
        EXPECT_DOUBLE_EQ(row.t_1, p.f1 * p.f1 / (df * c)) << p.name;
        EXPECT_DOUBLE_EQ(row.t_2, -(p.f2 * p.f2) / (df * c)) << p.name;
        EXPECT_DOUBLE_EQ(row.shift, -p.f2 / df) << p.name;
        EXPECT_DOUBLE_EQ(row.bt_2, -p.f1 / df) << p.name;
        EXPECT_LT(row.shift, 0.0) << p.name;   // the iono-free row moves N_1 against the wide lane
        EXPECT_LT(row.t_2, 0.0) << p.name;
    }
    // The magnitudes the closed form gives: -3.529 on L1/L2 and -2.949 on E1/E5a (and GPS L1/L5).
    EXPECT_NEAR(NlRowTestAccess::row(kFL1, kFL2, lamOf(kFL1), lamOf(kFL2), true).shift, -3.5294, 1e-4);
    EXPECT_NEAR(NlRowTestAccess::row(kFL1, kFE5a, lamOf(kFL1), lamOf(kFE5a), true).shift, -2.9487, 1e-4);
}

TEST(ArWlBranchReasons, TheIonoFreeRowIgnoresTheWavelengthArgumentsAndTheNarrowLaneRowUsesThem) {
    // The iono-free branch is built from the frequencies and the speed of light only; the
    // narrow-lane branch divides by the wavelengths it is given.
    const double l1 = lamOf(kFL1), l2 = lamOf(kFL2);
    const auto if_true  = NlRowTestAccess::row(kFL1, kFL2, l1, l2, true);
    const auto if_odd   = NlRowTestAccess::row(kFL1, kFL2, 1.5 * l1, 0.5 * l2, true);
    EXPECT_EQ(if_true.t_1, if_odd.t_1);
    EXPECT_EQ(if_true.t_2, if_odd.t_2);
    EXPECT_EQ(if_true.shift, if_odd.shift);
    EXPECT_EQ(if_true.bt_2, if_odd.bt_2);
    const auto nl_true = NlRowTestAccess::row(kFL1, kFL2, l1, l2, false);
    const auto nl_odd  = NlRowTestAccess::row(kFL1, kFL2, 1.5 * l1, 0.5 * l2, false);
    EXPECT_NEAR(nl_odd.t_1, nl_true.t_1 / 1.5, 1e-12);
    EXPECT_NEAR(nl_odd.t_2, nl_true.t_2 / 0.5, 1e-12);
    EXPECT_EQ(nl_odd.shift, nl_true.shift);          // the shifts depend on the frequencies only
    EXPECT_EQ(nl_odd.bt_2, nl_true.bt_2);
}
