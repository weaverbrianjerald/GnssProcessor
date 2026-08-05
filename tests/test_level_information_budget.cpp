/**
 * @file test_level_information_budget.cpp
 * @brief The ionosphere-LEVEL information budget: cards LV-1 and LV-2
 *        (docs/SPEC_level_information_budget.md).
 *
 * Both cards are one line of arithmetic each, sitting on a path where a wrong
 * answer produces a plausible number rather than a crash — the spec asks for unit
 * coverage exactly there and nowhere else.  What is pinned here:
 *
 *   LV-1 (bias-free per-LOS iono seeding)
 *     1. The station-common offset is a MEDIAN, not a mean: one mis-tracked
 *        channel (the geometry-free code seed can be tens of metres off) must not
 *        move the frame every other line of sight is then born into.  A mean would
 *        pass a "does it return a number" test and fail this one.
 *     2. The min_sats FALLBACK is real: below the floor there is NO offset, and the
 *        caller must seed RAW and count it — a silently-zero offset would reframe
 *        every station to its own single channel while reading as "reframed".
 *     3. The knob is INERT-SAFE: armed without an SH field there is nothing to
 *        seed into, and the loader must force it off and say so rather than let a
 *        run report a reframe it never performed (invariant #4).
 *
 *   LV-2 (rx-CB carry common-mode de-mean)
 *     4. μ is the INFORMATION-weighted mean (Σλ·innov / Σλ), not the arithmetic
 *        one.  With unequal λ the two differ, and only the weighted one makes the
 *        card's claim true.
 *     5. The identity it rests on, stated as the property it is: Σλ·(innov − μ) ≡ 0
 *        in DELTA coordinates, with every between-station DIFFERENCE keeping full
 *        stiffness (a uniform shift of all innovations moves μ by exactly that
 *        shift and leaves every demeaned innovation unchanged).  ⚠ That is NOT the
 *        same as leaving the common mode free — see the test's own note and
 *        SPEC §13; in absolute terms the de-meaned carry pins it to mean(x_lin).
 *     6. The thin-mean FREEZE (invariant #7's decoupling edge): below the
 *        contributing-station floor, or with no weight behind the sum, μ HOLDS its
 *        previous value instead of adopting a mean nothing supports.
 *     7. Armed without the carry it modifies, the de-mean is forced off — there is
 *        no fold to de-mean, and an armed-but-inert knob reads as working.
 *
 * The two arithmetic helpers are pure by construction (a static on
 * StateInitializer and an inline in NetworkRunTypes.hpp) precisely so this file
 * needs no estimator and no run loop; the contract tests load a minimal network
 * config the same way the other config-contract suites do.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "logging/LogManager.hpp"
#include "measurement/StateInitializer.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

// ===========================================================================
// LV-1 — the station-common seed offset
// ===========================================================================

TEST(LevelInformationBudget, Lv1OffsetIsTheMedianOfTheRawSeeds) {
    std::vector<double> odd{3.0, 1.0, 2.0, 5.0, 4.0};
    const auto m_odd = StateInitializer::stationSeedOffset(odd, 5);
    ASSERT_TRUE(m_odd.has_value());
    EXPECT_DOUBLE_EQ(*m_odd, 3.0);

    std::vector<double> even{4.0, 1.0, 3.0, 2.0, 6.0, 5.0};
    const auto m_even = StateInitializer::stationSeedOffset(even, 5);
    ASSERT_TRUE(m_even.has_value());
    EXPECT_DOUBLE_EQ(*m_even, 3.5);   // mean of the two central order statistics
}

// A single garbage channel is the failure mode the median exists for: the raw
// geometry-free seed of a mis-tracked satellite is bounded only by the |seed| <
// 200 m sanity check, so one sample can sit 100 m from the truth.  The mean
// swallows it whole and drags the frame EVERY other LOS of that station is then
// born into; the median does not move at all.
TEST(LevelInformationBudget, Lv1OffsetIsRobustToOneWildChannel) {
    const std::vector<double> clean{9.0, 10.0, 11.0, 10.5, 9.5};
    std::vector<double> polluted = clean;
    polluted.push_back(150.0);

    std::vector<double> a = clean, b = polluted;
    const auto m_clean    = StateInitializer::stationSeedOffset(a, 5);
    const auto m_polluted = StateInitializer::stationSeedOffset(b, 5);
    ASSERT_TRUE(m_clean.has_value());
    ASSERT_TRUE(m_polluted.has_value());
    EXPECT_NEAR(*m_polluted, *m_clean, 0.5);

    const double mean_polluted =
        std::accumulate(polluted.begin(), polluted.end(), 0.0)
        / static_cast<double>(polluted.size());
    EXPECT_GT(mean_polluted - *m_polluted, 20.0);   // what a mean would have cost
}

TEST(LevelInformationBudget, Lv1BelowMinSatsThereIsNoOffsetAtAll) {
    std::vector<double> four{1.0, 2.0, 3.0, 4.0};
    EXPECT_FALSE(StateInitializer::stationSeedOffset(four, 5).has_value());

    std::vector<double> five{1.0, 2.0, 3.0, 4.0, 5.0};
    EXPECT_TRUE(StateInitializer::stationSeedOffset(five, 5).has_value());  // exactly at the floor

    std::vector<double> none;
    EXPECT_FALSE(StateInitializer::stationSeedOffset(none, 5).has_value());

    // A degenerate floor still needs one sample — never "median of nothing".
    std::vector<double> one{7.0};
    const auto m_one = StateInitializer::stationSeedOffset(one, 0);
    ASSERT_TRUE(m_one.has_value());
    EXPECT_DOUBLE_EQ(*m_one, 7.0);
}

// ===========================================================================
// LV-2 — the information-weighted common-mode mean
// ===========================================================================

TEST(LevelInformationBudget, Lv2MuIsInformationWeightedNotArithmetic) {
    // Two stations, 9:1 in information.  The arithmetic mean would be 0.5; only
    // the information-weighted mean answers the question the fold asks.
    const double lam1 = 9.0, innov1 = 0.0;
    const double lam2 = 1.0, innov2 = 1.0;
    const auto red = rxCbDemeanMu(lam1 * innov1 + lam2 * innov2, lam1 + lam2,
                                  /*n_sta=*/2, /*min_sta=*/2, /*prev_mu=*/0.0);
    ASSERT_TRUE(red.fresh);
    EXPECT_DOUBLE_EQ(red.mu, 0.1);
    EXPECT_NE(red.mu, 0.5);
}

// The identity the card rests on, as a property: after de-meaning, the folded RHS
// sums to zero along the common-mode direction IN DELTA COORDINATES, and every
// between-station difference is untouched.
//
// ⚠ Read the scope right (SPEC §13, measured 2026-08-05).  This identity does NOT
// mean the common mode is left free: the de-mean moves the RHS, never the fold's
// information, so in ABSOLUTE terms the targets' weighted mean becomes mean(x_lin)
// — the carry re-pins the common mode to the frozen first-epoch seed frame instead
// of re-asserting last epoch's realization.  What the identity buys is the removal
// of the FEEDBACK, which is exactly why the lag mattered (LV-5).  Do not let this
// test be read as proof of a free frame; it proves the arithmetic, not the claim.
TEST(LevelInformationBudget, Lv2DemeanedFoldsSumToZeroInDeltaCoordinates) {
    const std::vector<double> lam{100.0, 25.0, 4.0, 400.0, 1.0, 64.0, 9.0, 36.0};
    const std::vector<double> innov{-1.20, 0.35, 2.75, -0.40, 5.00, 0.05, -3.10, 0.90};
    ASSERT_EQ(lam.size(), innov.size());

    double s_lam = 0.0, s_lam_innov = 0.0;
    for (std::size_t i = 0; i < lam.size(); ++i) {
        s_lam += lam[i];
        s_lam_innov += lam[i] * innov[i];
    }
    const auto red = rxCbDemeanMu(s_lam_innov, s_lam,
                                  static_cast<int>(lam.size()), 8, 0.0);
    ASSERT_TRUE(red.fresh);

    // Σλ(innov − μ) = 0 — the fold's summed RHS along the common-mode direction.
    double net = 0.0;
    for (std::size_t i = 0; i < lam.size(); ++i) net += lam[i] * (innov[i] - red.mu);
    EXPECT_NEAR(net, 0.0, 1e-9 * s_lam);

    // …while the differences the carry exists to transport are bit-preserved.
    for (std::size_t i = 1; i < lam.size(); ++i) {
        const double raw_diff = innov[i] - innov[0];
        const double dm_diff  = (innov[i] - red.mu) - (innov[0] - red.mu);
        EXPECT_DOUBLE_EQ(dm_diff, raw_diff);
    }

    // A COHERENT FRAME MOVE (every station shifts by the same amount — the
    // flywheel this card removes) moves μ by exactly that amount and leaves every
    // demeaned innovation, and therefore every folded prior, unchanged.
    constexpr double kShift = 0.37;
    double s2 = 0.0;
    for (std::size_t i = 0; i < lam.size(); ++i) s2 += lam[i] * (innov[i] + kShift);
    const auto red2 = rxCbDemeanMu(s2, s_lam, static_cast<int>(lam.size()), 8, 0.0);
    ASSERT_TRUE(red2.fresh);
    EXPECT_NEAR(red2.mu, red.mu + kShift, 1e-12);
    for (std::size_t i = 0; i < lam.size(); ++i)
        EXPECT_NEAR((innov[i] + kShift) - red2.mu, innov[i] - red.mu, 1e-12);
}

TEST(LevelInformationBudget, Lv2ThinEpochFreezesMuInsteadOfTrustingIt) {
    constexpr double kPrev = -0.25;

    // Below the contributing-station floor: HOLD, do not adopt.
    const auto thin = rxCbDemeanMu(/*sum_lam_innov=*/7.0, /*sum_lam=*/2.0,
                                   /*n_sta=*/3, /*min_sta=*/8, kPrev);
    EXPECT_FALSE(thin.fresh);
    EXPECT_DOUBLE_EQ(thin.mu, kPrev);   // NOT 3.5

    // At the floor it is fresh (the floor is inclusive).
    const auto at_floor = rxCbDemeanMu(7.0, 2.0, 8, 8, kPrev);
    EXPECT_TRUE(at_floor.fresh);
    EXPECT_DOUBLE_EQ(at_floor.mu, 3.5);

    // No weight behind the sum is not a measurement — freeze.
    EXPECT_FALSE(rxCbDemeanMu(0.0, 0.0, 60, 8, kPrev).fresh);
    EXPECT_DOUBLE_EQ(rxCbDemeanMu(0.0, 0.0, 60, 8, kPrev).mu, kPrev);
    EXPECT_FALSE(rxCbDemeanMu(1.0, -3.0, 60, 8, kPrev).fresh);   // negative Σλ is impossible ⇒ reject

    // Non-finite inputs can never become a folded prior.
    const double nan = std::nan("");
    const double inf = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(rxCbDemeanMu(nan, 10.0, 60, 8, kPrev).fresh);
    EXPECT_FALSE(rxCbDemeanMu(1.0, nan, 60, 8, kPrev).fresh);
    EXPECT_FALSE(rxCbDemeanMu(inf, 10.0, 60, 8, kPrev).fresh);
    EXPECT_DOUBLE_EQ(rxCbDemeanMu(inf, 10.0, 60, 8, kPrev).mu, kPrev);
}

// A cold start has no previous μ: the first epoch injects UN-demeaned (μ = 0),
// which is one epoch of legacy behaviour by design, and the record must say
// "frozen" rather than pretend a frame was measured.
TEST(LevelInformationBudget, Lv2ColdStartHoldsZeroAndSaysSo) {
    const auto cold = rxCbDemeanMu(0.0, 0.0, 0, 8, /*prev_mu=*/0.0);
    EXPECT_FALSE(cold.fresh);
    EXPECT_DOUBLE_EQ(cold.mu, 0.0);
}

// ===========================================================================
// LV-4 — the frame-INVARIANT carry screen
// ===========================================================================
//
// The claim is a symmetry, so it is tested as one: shifting the linearization
// point (which the network solve is invariant to, and which the absolute
// estimates therefore absorb) must not change the screen's verdict.  The retired
// statistic fails exactly that test, and the second case below pins the failure
// so nobody reinstates it by accident.

// A quiet 61-station epoch: everyone re-realizes by ~±1 m (this is what the network
// actually does at ignition), and ONE receiver steps by 30 m.  The screen must find
// exactly that one — and must do so identically in any linearization frame.
std::vector<double> quietSteps() {
    std::vector<double> s;
    for (int i = 0; i < 60; ++i) s.push_back(0.9 * std::sin(0.7 * i) + 0.1);
    return s;
}

TEST(LevelInformationBudget, Lv4StepScreenIsInvariantToTheLinearizationFrame) {
    std::vector<double> steps = quietSteps();
    steps.push_back(30.0);                       // the one real receiver jump
    std::vector<double> work = steps;
    const auto scale = rxCbStepScale(work, /*formal_sigma_floor=*/0.15, /*min_n=*/8);
    ASSERT_TRUE(scale.valid);

    EXPECT_TRUE(rxCbStepVerdict(30.0, scale, 10.0).jumped);
    for (std::size_t i = 0; i + 1 < steps.size(); ++i)
        EXPECT_FALSE(rxCbStepVerdict(steps[i], scale, 10.0).jumped) << "i=" << i;

    // FRAME INVARIANCE.  Shifting x_lin by δ shifts every ABSOLUTE estimate by δ
    // (x_abs = x_lin + x_r is what the solve preserves), so every STEP — a
    // difference of two absolutes at the same station — is unchanged, and with it
    // the scale, the z and the verdict.  The retired screen changes by δ itself.
    // Checked to a relative tolerance, not bit-for-bit: the differencing still
    // carries ordinary cancellation.
    for (double d : {0.5, -2.75, 137.0, -1e4}) {
        std::vector<double> shifted;
        shifted.reserve(steps.size());
        for (double e : steps) shifted.push_back((100.0 + d + e) - (100.0 + d));
        std::vector<double> w2 = shifted;
        const auto sc2 = rxCbStepScale(w2, 0.15, 8);
        ASSERT_TRUE(sc2.valid);
        EXPECT_NEAR(sc2.sigma, scale.sigma, 1e-6 * (1.0 + scale.sigma));
        EXPECT_NEAR(sc2.median, scale.median, 1e-6 * (1.0 + std::abs(scale.median)));
        EXPECT_TRUE(rxCbStepVerdict(shifted.back(), sc2, 10.0).jumped);
    }
}

// A step COMMON to the whole network is a frame move, not N simultaneous receiver
// resets — the same distinction LV-2 draws for the fold.  Adding a constant to
// every station must change nothing about who is flagged.
TEST(LevelInformationBudget, Lv4CommonModeStepIsNotAnEvent) {
    std::vector<double> steps = quietSteps();
    steps.push_back(30.0);
    std::vector<double> w1 = steps;
    const auto base = rxCbStepScale(w1, 0.15, 8);

    std::vector<double> moved;
    for (double e : steps) moved.push_back(e + 7.5);      // whole network steps 7.5 m
    std::vector<double> w2 = moved;
    const auto shifted = rxCbStepScale(w2, 0.15, 8);
    ASSERT_TRUE(shifted.valid);
    EXPECT_NEAR(shifted.median, base.median + 7.5, 1e-9);
    EXPECT_NEAR(shifted.sigma, base.sigma, 1e-9);         // spread unchanged

    for (std::size_t i = 0; i + 1 < moved.size(); ++i)
        EXPECT_FALSE(rxCbStepVerdict(moved[i], shifted, 10.0).jumped) << "i=" << i;
    EXPECT_TRUE(rxCbStepVerdict(moved.back(), shifted, 10.0).jumped);
}

// WHY THE SCALE MUST BE ROBUST — the measured failure, pinned.  A formal-σ test
// (σ ≈ 0.52 m from var + guard) calls the network's ordinary metre-class
// re-realization a jump: on the real arm it erased 432 of 777 carried entries and
// the level went unstable.  The MAD scale adapts to what the epoch is actually
// doing, so the same population yields exactly one flag.
TEST(LevelInformationBudget, Lv4RobustScaleSurvivesMetreClassReRealization) {
    std::vector<double> steps = quietSteps();
    steps.push_back(30.0);
    std::vector<double> work = steps;
    const auto scale = rxCbStepScale(work, 0.52, 8);
    ASSERT_TRUE(scale.valid);
    EXPECT_GT(scale.sigma, 0.52);          // the epoch's own spread dominates the floor

    int formal_flags = 0, robust_flags = 0;
    for (double e : steps) {
        if (std::abs(e) > 10.0 * 0.52) ++formal_flags;                 // the retired scale
        if (rxCbStepVerdict(e, scale, 10.0).jumped) ++robust_flags;    // this card's
    }
    EXPECT_EQ(robust_flags, 1);
    EXPECT_GE(formal_flags, 1);
    EXPECT_GE(formal_flags, robust_flags);

    // …but the floor still protects a freakishly quiet epoch from hypersensitivity.
    std::vector<double> tiny(40, 0.0);
    for (std::size_t i = 0; i < tiny.size(); ++i) tiny[i] = 1e-6 * static_cast<double>(i % 3);
    std::vector<double> w = tiny;
    const auto quiet = rxCbStepScale(w, 0.52, 8);
    ASSERT_TRUE(quiet.valid);
    EXPECT_DOUBLE_EQ(quiet.sigma, 0.52);
    EXPECT_FALSE(rxCbStepVerdict(1.0, quiet, 10.0).jumped);   // 1 m < 10·0.52
}

// Invariant #7: a thin epoch must not define what "normal" is.
TEST(LevelInformationBudget, Lv4ThinEpochStandsTheScreenDown) {
    std::vector<double> few{0.1, -0.2, 40.0};
    const auto scale = rxCbStepScale(few, 0.15, /*min_n=*/8);
    EXPECT_FALSE(scale.valid);
    EXPECT_FALSE(rxCbStepVerdict(40.0, scale, 10.0).jumped);   // no scale ⇒ no erase
    EXPECT_EQ(scale.n, 3);
}

// THE DEFECT, pinned.  The retired inject test compares a carried ABSOLUTE value
// with x_lin — one absolute quantity and one frozen frame — so a station whose
// first-epoch seed simply landed 6 m away is withheld forever, with no receiver
// event anywhere.  Same numbers, both screens, opposite answers.
TEST(LevelInformationBudget, Lv4RetiredFrameScreenWithholdsAQuietReceiverForever) {
    constexpr double kVar = 1e-2, kQ = 1e-9, kGap = 30.0, kK = 10.0, kGuard = 0.25;
    constexpr double kXlin = -6.0;        // where the first-epoch seed happened to land
    constexpr double kAbsPrev = 0.02, kAbsNew = 0.03;   // a perfectly quiet receiver

    double var_eff = 0.0;
    const double frame_innov = kAbsPrev - kXlin;        // 6.02 m of pure frame
    EXPECT_EQ(rxCbInjectGate(kGap, 900.0, kVar, kQ, frame_innov, kK, kGuard, var_eff),
              RxCbInjectAction::SkipInnov);             // withheld — nothing happened

    // The same row, judged on the invariant step against its peers: quiet, folds.
    std::vector<double> peers = quietSteps();
    peers.push_back(kAbsNew - kAbsPrev);
    const auto scale = rxCbStepScale(peers, 0.15, 8);
    ASSERT_TRUE(scale.valid);
    EXPECT_FALSE(rxCbStepVerdict(kAbsNew - kAbsPrev, scale, kK).jumped);
    var_eff = 0.0;
    EXPECT_EQ(rxCbInjectGate(kGap, 900.0, kVar, kQ, frame_innov, kK, kGuard, var_eff,
                             /*innov_test=*/false),
              RxCbInjectAction::Fold);
    EXPECT_GT(var_eff, 0.0);              // the frame-free checks still ran

    // Disabling the innovation test does NOT disable the frame-free ones.
    var_eff = 0.0;
    EXPECT_EQ(rxCbInjectGate(/*gap_s=*/5000.0, 900.0, kVar, kQ, 0.0, kK, kGuard, var_eff,
                             /*innov_test=*/false),
              RxCbInjectAction::SkipGap);
    var_eff = 0.0;
    EXPECT_EQ(rxCbInjectGate(kGap, 900.0, /*var=*/-1.0, kQ, 0.0, kK, kGuard, var_eff,
                             /*innov_test=*/false),
              RxCbInjectAction::SkipGap);
}

// Degenerate input must not manufacture an erase: a false step costs a real
// prior, and the harvest's own finite checks already reject unusable values.
TEST(LevelInformationBudget, Lv4DegenerateInputReportsNoJump) {
    const double nan = std::nan("");
    const double inf = std::numeric_limits<double>::infinity();

    // Non-finite members are dropped from the population, not propagated into it.
    std::vector<double> dirty = quietSteps();
    dirty.push_back(nan);
    dirty.push_back(inf);
    const std::size_t clean_n = quietSteps().size();
    const auto scale = rxCbStepScale(dirty, 0.15, 8);
    ASSERT_TRUE(scale.valid);
    EXPECT_EQ(scale.n, static_cast<int>(clean_n));
    EXPECT_TRUE(std::isfinite(scale.sigma));
    EXPECT_TRUE(std::isfinite(scale.median));

    // A non-finite step of its own never erases.
    EXPECT_FALSE(rxCbStepVerdict(nan, scale, 10.0).jumped);
    EXPECT_FALSE(rxCbStepVerdict(inf, scale, 10.0).jumped);

    // An all-identical population has zero MAD, so the FLOOR is what stands: the
    // scale must never be zero (that would divide every step by nothing).
    std::vector<double> flat(30, 2.5);
    const auto degenerate = rxCbStepScale(flat, 0.15, 8);
    ASSERT_TRUE(degenerate.valid);
    EXPECT_DOUBLE_EQ(degenerate.sigma, 0.15);
    EXPECT_DOUBLE_EQ(degenerate.median, 2.5);

    // …and with no floor at all the scale is invalid rather than infinite-power.
    std::vector<double> flat2(30, 2.5);
    const auto no_floor = rxCbStepScale(flat2, 0.0, 8);
    EXPECT_FALSE(no_floor.valid);
    EXPECT_FALSE(rxCbStepVerdict(1e6, no_floor, 10.0).jumped);
}

// ===========================================================================
// Loader contract — the knobs are OFF by default and INERT-SAFE when armed
// without the mechanism they modify (standing invariant #4)
// ===========================================================================

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("level_info_budget_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

std::string networkConfig(const std::string& network_extra,
                          const std::string& estimation_extra = "") {
    return std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"estimation\": {\n"
        "    \"estimate_ifb\": true"
        + estimation_extra +
        "\n  },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"AAAA\",\n"
        + network_extra +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

TEST(LevelInformationBudget, BothKnobsDefaultOff) {
    LogManager log;
    const fs::path p = writeTempConfig("default", networkConfig(""));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(cfg.networkIonoSeedFieldFrame());
    EXPECT_FALSE(cfg.networkRxCbCarryDemean());
    EXPECT_EQ(cfg.networkIonoSeedFieldFrameMinSats(), 5);
    EXPECT_EQ(cfg.networkRxCbCarryDemeanMinStations(), 8);
    std::error_code ec; fs::remove(p, ec);
}

// No field ⇒ no frame to seed into.  The reframe must not stay "armed" while
// doing nothing: a run whose log says LV-1 is on and whose seeds are all still in
// the receiver's bias-in frame is exactly the failure invariant #4 names.
TEST(LevelInformationBudget, Lv1IsForcedOffWithoutAnShField) {
    LogManager log;
    const fs::path p = writeTempConfig("lv1_nofield",
        networkConfig("    \"iono_seed_field_frame\": true,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    ASSERT_FALSE(cfg.estimateNetworkIonoSh());
    EXPECT_FALSE(cfg.networkIonoSeedFieldFrame());
    std::error_code ec; fs::remove(p, ec);
}

TEST(LevelInformationBudget, Lv1SurvivesWithAnShField) {
    LogManager log;
    const fs::path p = writeTempConfig("lv1_field",
        networkConfig("    \"estimate_iono_sh\": true,\n"
                      "    \"iono_seed_field_frame\": true,\n"
                      "    \"iono_seed_field_frame_min_sats\": 6,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    ASSERT_TRUE(cfg.estimateNetworkIonoSh());
    EXPECT_TRUE(cfg.networkIonoSeedFieldFrame());
    EXPECT_EQ(cfg.networkIonoSeedFieldFrameMinSats(), 6);
    std::error_code ec; fs::remove(p, ec);
}

// A median over one sample is that sample: it would reframe every station onto
// its own single channel while reporting a clean reframe.  Hard error.
TEST(LevelInformationBudget, Lv1RejectsADegenerateMedianFloor) {
    LogManager log;
    const fs::path p = writeTempConfig("lv1_minsats",
        networkConfig("    \"estimate_iono_sh\": true,\n"
                      "    \"iono_seed_field_frame\": true,\n"
                      "    \"iono_seed_field_frame_min_sats\": 1,\n"));
    GnssConfig cfg;
    EXPECT_FALSE(cfg.loadFromJson(p, log));
    std::error_code ec; fs::remove(p, ec);
}

// No carry ⇒ no fold ⇒ nothing to de-mean.
TEST(LevelInformationBudget, Lv2IsForcedOffWithoutTheCarry) {
    LogManager log;
    const fs::path p = writeTempConfig("lv2_nocarry",
        networkConfig("    \"rx_cb_carry_demean\": true,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    ASSERT_FALSE(cfg.networkCarryRxCodeBias());
    EXPECT_FALSE(cfg.networkRxCbCarryDemean());
    std::error_code ec; fs::remove(p, ec);
}

TEST(LevelInformationBudget, Lv2SurvivesWithTheCarry) {
    LogManager log;
    const fs::path p = writeTempConfig("lv2_carry",
        networkConfig("    \"code_datum_time_linked\": true,\n"
                      "    \"carry_rx_code_bias\": true,\n"
                      "    \"rx_cb_carry_demean\": true,\n"
                      "    \"rx_cb_carry_demean_min_stations\": 12,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    ASSERT_TRUE(cfg.networkCarryRxCodeBias());
    EXPECT_TRUE(cfg.networkRxCbCarryDemean());
    EXPECT_EQ(cfg.networkRxCbCarryDemeanMinStations(), 12);
    std::error_code ec; fs::remove(p, ec);
}

// LV-5 rides on LV-2: with no de-mean there is no μ to compute early.
TEST(LevelInformationBudget, Lv5IsForcedOffWithoutTheDemean) {
    LogManager log;
    const fs::path p = writeTempConfig("lv5_nodemean",
        networkConfig("    \"code_datum_time_linked\": true,\n"
                      "    \"carry_rx_code_bias\": true,\n"
                      "    \"rx_cb_carry_demean_within\": true,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    ASSERT_TRUE(cfg.networkCarryRxCodeBias());
    ASSERT_FALSE(cfg.networkRxCbCarryDemean());
    EXPECT_FALSE(cfg.networkRxCbCarryDemeanWithin());
    std::error_code ec; fs::remove(p, ec);
}

TEST(LevelInformationBudget, Lv5SurvivesWithTheDemean) {
    LogManager log;
    const fs::path p = writeTempConfig("lv5_demean",
        networkConfig("    \"code_datum_time_linked\": true,\n"
                      "    \"carry_rx_code_bias\": true,\n"
                      "    \"rx_cb_carry_demean\": true,\n"
                      "    \"rx_cb_carry_demean_within\": true,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.networkRxCbCarryDemean());
    EXPECT_TRUE(cfg.networkRxCbCarryDemeanWithin());
    std::error_code ec; fs::remove(p, ec);
}

TEST(LevelInformationBudget, Lv4IsForcedOffWithoutTheCarry) {
    LogManager log;
    const fs::path p = writeTempConfig("lv4_nocarry",
        networkConfig("    \"rx_cb_carry_step_gate\": true,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    ASSERT_FALSE(cfg.networkCarryRxCodeBias());
    EXPECT_FALSE(cfg.networkRxCbCarryStepGate());
    EXPECT_DOUBLE_EQ(cfg.networkRxCbCarryStepKSigma(), 10.0);   // legacy k by default
    std::error_code ec; fs::remove(p, ec);
}

TEST(LevelInformationBudget, Lv4SurvivesWithTheCarryAndRejectsANonPositiveK) {
    LogManager log;
    const fs::path ok = writeTempConfig("lv4_carry",
        networkConfig("    \"code_datum_time_linked\": true,\n"
                      "    \"carry_rx_code_bias\": true,\n"
                      "    \"rx_cb_carry_step_gate\": true,\n"
                      "    \"rx_cb_carry_step_k_sigma\": 6.0,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(ok, log));
    EXPECT_TRUE(cfg.networkRxCbCarryStepGate());
    EXPECT_DOUBLE_EQ(cfg.networkRxCbCarryStepKSigma(), 6.0);

    // k <= 0 would erase every carried bias on every epoch — hard error, not a clamp.
    const fs::path bad = writeTempConfig("lv4_badk",
        networkConfig("    \"code_datum_time_linked\": true,\n"
                      "    \"carry_rx_code_bias\": true,\n"
                      "    \"rx_cb_carry_step_gate\": true,\n"
                      "    \"rx_cb_carry_step_k_sigma\": 0.0,\n"));
    GnssConfig bad_cfg;
    EXPECT_FALSE(bad_cfg.loadFromJson(bad, log));
    std::error_code ec; fs::remove(ok, ec); fs::remove(bad, ec);
}

}  // namespace
