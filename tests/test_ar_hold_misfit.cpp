/**
 * @file test_ar_hold_misfit.cpp
 * @brief Unit tests for the two pure helpers of the held-pair release: the misfit of
 *        a float double difference against its held integers, and the choice of the
 *        satellites to refloat from the contradicted pairs.
 *
 * The release decides whether a remembered fix still agrees with the float state.
 * Its arithmetic must be right on every frequency pair the user engine fixes, so the
 * misfit is checked on GPS L1/L2 and on Galileo E1/E5a with synthetic double
 * differences built from known integers:
 *   - a float equal to the held integers gives zero on both measures;
 *   - the same error of e cycles on both bands (a narrow-lane error, the widelane
 *     integer unchanged) gives d_wl = 0 and d_if = e narrow-lane cycles exactly;
 *   - one cycle on the first band alone gives d_wl = 1 and d_if = f1 / (f1 - f2);
 *   - an equal error in metres on both bands (a range-like error) gives
 *     d_if = error / lambda_NL and d_wl = error / lambda_WL.
 * The satellite choice is checked at the edges of its rule (a reference is blamed
 * when at least two of its held pairs and more than half of them are contradicted):
 * exactly half is not enough, one contradicted pair of a reference holding only that
 * pair is not enough, and two references are judged separately.
 *
 * The neighbouring file tests/test_ar_hold_release.cpp holds the first cases of both
 * helpers (GPS zero misfit, one widelane cycle, an ionosphere change; one of three,
 * two of three and two of five contradicted); none of them is repeated here.
 *
 * Suite name @c ArHoldMisfit is owned solely by this file.
 */

#include <gtest/gtest.h>

#include "filter/AmbiguityResolver.hpp"
#include "common/Constants.hpp"

#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

// The private static helpers are reached through the access type the header
// befriends.  The same type is defined in tests/test_ar_hold_release.cpp; a class
// defined in two translation units must be token for token identical, so this
// definition repeats that one exactly and must change only together with it.
struct ArHoldReleaseTestAccess {
    using Held   = AmbiguityResolution::HeldPairFix;
    using Misfit = AmbiguityResolver::HoldMisfit;
    using Pairs  = std::vector<std::pair<std::string, std::string>>;
    static Misfit misfit(const Held& h, double dd1, double dd2) {
        return AmbiguityResolver::holdMisfit_(h, dd1, dd2);
    }
    static std::vector<std::string> choose(const Pairs& held, const Pairs& bad) {
        return AmbiguityResolver::chooseReleaseSatellites_(held, bad);
    }
};

} // namespace GnssProcessor

using namespace GnssProcessor;

namespace {

using Access = ArHoldReleaseTestAccess;
using Sats   = std::vector<std::string>;

/// One dual-frequency signal pair: the higher frequency first.
struct Band2 {
    const char* name;
    const char* sat_i;
    const char* sat_ref;
    const char* sig_hi;
    const char* sig_lo;
    double f1;
    double f2;
    double lam1() const { return Constants::SPEED_OF_LIGHT / f1; }
    double lam2() const { return Constants::SPEED_OF_LIGHT / f2; }
    double lamNl() const { return Constants::SPEED_OF_LIGHT / (f1 + f2); }
    double lamWl() const { return Constants::SPEED_OF_LIGHT / (f1 - f2); }
};

const Band2 kGpsL1L2{ "GPS L1/L2", "G06", "G31", "L1W", "L2W", 1575.42e6, 1227.60e6 };
const Band2 kGalE1E5a{ "Galileo E1/E5a", "E11", "E24", "L1C", "L5Q", 1575.42e6, 1176.45e6 };

/// The held fix of one pair: N_1 = n1 and WL = n1 - n2.
Access::Held held(const Band2& b, int n1, int wl) {
    Access::Held h;
    h.sat_i   = b.sat_i;
    h.sat_ref = b.sat_ref;
    h.sig_hi  = b.sig_hi;
    h.sig_lo  = b.sig_lo;
    h.f1      = b.f1;
    h.f2      = b.f2;
    h.lam1    = b.lam1();
    h.lam2    = b.lam2();
    h.n1      = n1;
    h.wl      = wl;
    return h;
}

// Held integers with non-zero values of both signs so that a sign slip in either
// held double difference shows.
constexpr int kN1 = -5;
constexpr int kWl = 3;
constexpr int kN2 = kN1 - kWl;   // -8

void checkZeroMisfit(const Band2& b) {
    SCOPED_TRACE(b.name);
    const auto m = Access::misfit(held(b, kN1, kWl), kN1 * b.lam1(), kN2 * b.lam2());
    EXPECT_NEAR(m.d_wl, 0.0, 1e-9);
    EXPECT_NEAR(m.d_if, 0.0, 1e-9);
}

void checkCommonNarrowLaneError(const Band2& b, double e_cyc) {
    SCOPED_TRACE(b.name);
    // e cycles on both bands keep N_1 - N_2 and move the ionosphere-free combination
    // by e (f1^2 lambda_1 - f2^2 lambda_2) / (f1^2 - f2^2) = e c / (f1 + f2), which is
    // e narrow-lane cycles exactly.
    const double dd1 = (kN1 + e_cyc) * b.lam1();
    const double dd2 = (kN2 + e_cyc) * b.lam2();
    const auto m = Access::misfit(held(b, kN1, kWl), dd1, dd2);
    EXPECT_NEAR(m.d_wl, 0.0, 1e-9);
    EXPECT_NEAR(m.d_if, e_cyc, 1e-9);
}

void checkOneCycleOnFirstBand(const Band2& b, int sign) {
    SCOPED_TRACE(b.name);
    // One cycle on the first band alone: the widelane moves one cycle, and the
    // ionosphere-free combination by f1^2 lambda_1 / (f1^2 - f2^2) metres, which in
    // narrow-lane cycles is f1 / (f1 - f2).
    const double dd1 = (kN1 + sign) * b.lam1();
    const double dd2 = kN2 * b.lam2();
    const auto m = Access::misfit(held(b, kN1, kWl), dd1, dd2);
    EXPECT_NEAR(m.d_wl, 1.0 * sign, 1e-9);
    EXPECT_NEAR(m.d_if, sign * b.f1 / (b.f1 - b.f2), 1e-9);
}

} // namespace

// ---------------------------------------------------------------------------
// holdMisfit_
// ---------------------------------------------------------------------------

TEST(ArHoldMisfit, FloatAtNonZeroHeldIntegersHasNoMisfitOnGps) {
    checkZeroMisfit(kGpsL1L2);
}

TEST(ArHoldMisfit, FloatAtNonZeroHeldIntegersHasNoMisfitOnGalileo) {
    checkZeroMisfit(kGalE1E5a);
}

TEST(ArHoldMisfit, CommonNarrowLaneErrorMovesOnlyTheIonosphereFreeMeasure) {
    for (const double e : { 1.0, 0.3, -0.4 }) {
        SCOPED_TRACE(e);
        checkCommonNarrowLaneError(kGpsL1L2, e);
        checkCommonNarrowLaneError(kGalE1E5a, e);
    }
}

TEST(ArHoldMisfit, OneCycleOnTheFirstBandOnGps) {
    checkOneCycleOnFirstBand(kGpsL1L2, +1);
    checkOneCycleOnFirstBand(kGpsL1L2, -1);
    // 1575.42 / 347.82 narrow-lane cycles: well past any release threshold.
    const auto m = Access::misfit(held(kGpsL1L2, kN1, kWl), (kN1 + 1) * kGpsL1L2.lam1(),
                                  kN2 * kGpsL1L2.lam2());
    EXPECT_NEAR(m.d_if, 4.5294, 1e-4);
}

TEST(ArHoldMisfit, OneCycleOnTheFirstBandOnGalileo) {
    checkOneCycleOnFirstBand(kGalE1E5a, +1);
    checkOneCycleOnFirstBand(kGalE1E5a, -1);
    // 1575.42 / 398.97 narrow-lane cycles: the pair's own frequencies, not GPS's.
    const auto m = Access::misfit(held(kGalE1E5a, kN1, kWl), (kN1 + 1) * kGalE1E5a.lam1(),
                                  kN2 * kGalE1E5a.lam2());
    EXPECT_NEAR(m.d_if, 3.9487, 1e-4);
}

TEST(ArHoldMisfit, EqualMetricErrorOnBothBandsScalesByTheLaneWavelengths) {
    // A range-like error of the same metres on both bands passes unchanged through
    // the ionosphere-free combination (its coefficients sum to one) and moves the
    // widelane by delta / lambda_1 - delta / lambda_2 = delta / lambda_WL.
    const double delta = 0.012;
    for (const Band2* b : { &kGpsL1L2, &kGalE1E5a }) {
        SCOPED_TRACE(b->name);
        const auto m = Access::misfit(held(*b, kN1, kWl), kN1 * b->lam1() + delta,
                                      kN2 * b->lam2() + delta);
        EXPECT_NEAR(m.d_if, delta / b->lamNl(), 1e-9);
        EXPECT_NEAR(m.d_wl, delta / b->lamWl(), 1e-9);
    }
}

// ---------------------------------------------------------------------------
// chooseReleaseSatellites_
// ---------------------------------------------------------------------------

TEST(ArHoldMisfit, NoContradictedPairReleasesNothing) {
    const Access::Pairs held = { { "G06", "G31" }, { "G03", "G31" } };
    EXPECT_TRUE(Access::choose(held, {}).empty());
}

TEST(ArHoldMisfit, ReferenceWithASingleHeldPairIsNeverBlamed) {
    // The reference's only pair is contradicted (all of them), but one pair cannot
    // tell which satellite moved: the non-reference satellite is released.
    const Access::Pairs held = { { "E11", "E24" } };
    EXPECT_EQ(Access::choose(held, { { "E11", "E24" } }), (Sats{ "E11" }));
}

TEST(ArHoldMisfit, ReferenceContradictedInTwoOfFourIsNotBlamed) {
    // Exactly half is not more than half: each contradicted pair releases its own
    // non-reference satellite.
    const Access::Pairs held = {
        { "G06", "G31" }, { "G03", "G31" }, { "G04", "G31" }, { "G09", "G31" } };
    EXPECT_EQ(Access::choose(held, { { "G09", "G31" }, { "G04", "G31" } }),
              (Sats{ "G04", "G09" }));
}

TEST(ArHoldMisfit, ReferenceContradictedInThreeOfFourIsBlamedOnce) {
    const Access::Pairs held = {
        { "G06", "G31" }, { "G03", "G31" }, { "G04", "G31" }, { "G09", "G31" } };
    EXPECT_EQ(Access::choose(held, { { "G06", "G31" }, { "G03", "G31" }, { "G09", "G31" } }),
              (Sats{ "G31" }));
}

TEST(ArHoldMisfit, ReferenceContradictedInBothOfTwoIsBlamed) {
    const Access::Pairs held = { { "E11", "E24" }, { "E05", "E24" } };
    EXPECT_EQ(Access::choose(held, { { "E11", "E24" }, { "E05", "E24" } }), (Sats{ "E24" }));
}

TEST(ArHoldMisfit, TwoReferencesAreJudgedSeparately) {
    // GPS reference G31: contradicted in 2 of 3 (blamed).  Galileo reference E24:
    // contradicted in 1 of 3 (its non-reference satellite released).  The pairs of
    // one reference never count toward the other's share.
    const Access::Pairs held = {
        { "G06", "G31" }, { "G03", "G31" }, { "G04", "G31" },
        { "E11", "E24" }, { "E05", "E24" }, { "E30", "E24" } };
    const Access::Pairs bad = { { "E30", "E24" }, { "G06", "G31" }, { "G04", "G31" } };
    EXPECT_EQ(Access::choose(held, bad), (Sats{ "E30", "G31" }));
}

TEST(ArHoldMisfit, ReleasedSatellitesComeBackSortedWhateverTheInputOrder) {
    const Access::Pairs held = {
        { "G26", "G02" }, { "G10", "G02" }, { "G17", "G02" }, { "G05", "G02" },
        { "G08", "G02" } };
    const Access::Pairs bad = { { "G26", "G02" }, { "G05", "G02" } };
    EXPECT_EQ(Access::choose(held, bad), (Sats{ "G05", "G26" }));
}
