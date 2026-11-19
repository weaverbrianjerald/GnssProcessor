/**
 * @file test_ar_hold_release.cpp
 * @brief Unit tests for the held-pair release of the WL-constrained resolver
 *        (@c estimation.ambiguity_resolution.hold_release_cyc).
 *
 * The misfit is exact algebra on synthetic DDs built from known integers: a float
 * equal to the held integers gives zero, a widelane slip moves both measures, and
 * a pure ionosphere change moves the widelane but leaves the ionosphere-free
 * combination where it was.  The satellite choice is checked on small held sets, and
 * the partner refloat on a real StateManager: a discontinuity recorded with
 * noteAmbiguityDiscontinuity alone (the post-fit screen's single-row reset) must
 * trigger it.
 *
 * Suite name @c ArHoldReleaseTest is owned solely by this file.
 */

#include <gtest/gtest.h>

#include "filter/AmbiguityResolver.hpp"
#include "common/Constants.hpp"
#include "config/GnssConfig.hpp"
#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"

#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

/// Test seam for the private static helpers (friend in AmbiguityResolver.hpp).
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

constexpr double kF1 = 1575.42e6;
constexpr double kF2 = 1227.60e6;
const double kL1 = Constants::SPEED_OF_LIGHT / kF1;
const double kL2 = Constants::SPEED_OF_LIGHT / kF2;

ArHoldReleaseTestAccess::Held heldPair(int n1, int wl) {
    return { "G06", "G31", "L1W", "L2W", kF1, kF2, kL1, kL2, n1, wl };
}

} // namespace

TEST(ArHoldReleaseTest, FloatAtTheHeldIntegersHasNoMisfit) {
    const auto h = heldPair(0, 4);
    const auto m = ArHoldReleaseTestAccess::misfit(h, 0.0 * kL1, -4.0 * kL2);
    EXPECT_NEAR(m.d_wl, 0.0, 1e-12);
    EXPECT_NEAR(m.d_if, 0.0, 1e-12);
}

TEST(ArHoldReleaseTest, WidelaneOneCycleOffMovesBothMeasures) {
    // Held N_1 = 0, WL = 3 (N_2 = -3); the data say N_2 = -4 (WL = 4).
    const auto h = heldPair(0, 3);
    const auto m = ArHoldReleaseTestAccess::misfit(h, 0.0 * kL1, -4.0 * kL2);
    EXPECT_NEAR(m.d_wl, 1.0, 1e-12);
    // One L2 cycle in the ionosphere-free combination: f2^2 lambda_2 / (f1^2 - f2^2),
    // in narrow-lane cycles lambda_NL = c / (f1 + f2): f2 / (f1 - f2) = 3.53 cycles.
    EXPECT_NEAR(m.d_if, kF2 / (kF1 - kF2), 1e-9);
}

TEST(ArHoldReleaseTest, IonosphereChangeMovesOnlyTheWidelane) {
    // Phase ambiguities in metres absorb -I on L1 and -gamma I on L2 when the
    // slant ionosphere is mis-split: the ionosphere-free combination is unchanged.
    const auto h = heldPair(2, 1);
    const double gamma = (kF1 / kF2) * (kF1 / kF2);
    const double I = 0.05;
    const double dd1 = 2.0 * kL1 - I;
    const double dd2 = 1.0 * kL2 - gamma * I;
    const auto m = ArHoldReleaseTestAccess::misfit(h, dd1, dd2);
    EXPECT_NEAR(m.d_if, 0.0, 1e-12);
    EXPECT_NEAR(m.d_wl, -I / kL1 + gamma * I / kL2, 1e-12);
}

TEST(ArHoldReleaseTest, OneContradictedPairRefloatsItsNonReferenceSatellite) {
    const ArHoldReleaseTestAccess::Pairs held = {
        { "G06", "G31" }, { "G03", "G31" }, { "G04", "G31" } };
    const auto out = ArHoldReleaseTestAccess::choose(held, { { "G06", "G31" } });
    EXPECT_EQ(out, (std::vector<std::string>{ "G06" }));
}

TEST(ArHoldReleaseTest, MostPairsOfOneReferenceRefloatTheReference) {
    const ArHoldReleaseTestAccess::Pairs held = {
        { "G06", "G31" }, { "G03", "G31" }, { "G04", "G31" } };
    const auto out = ArHoldReleaseTestAccess::choose(held, { { "G06", "G31" }, { "G03", "G31" } });
    EXPECT_EQ(out, (std::vector<std::string>{ "G31" }));
}

TEST(ArHoldReleaseTest, AMinorityOfAReferencesPairsRefloatsEachSatellite) {
    const ArHoldReleaseTestAccess::Pairs held = {
        { "G06", "G31" }, { "G03", "G31" }, { "G04", "G31" }, { "G09", "G31" }, { "G26", "G31" } };
    const auto out = ArHoldReleaseTestAccess::choose(held, { { "G06", "G31" }, { "G03", "G31" } });
    EXPECT_EQ(out, (std::vector<std::string>{ "G03", "G06" }));
}

namespace {

/// A resolver that only tracks arcs (fixing disabled) with the partner refloat on or off.
AmbiguityResolverConfig partnerConfig(bool on) {
    AmbiguityResolverConfig c;
    c.ar_mode = 0;
    c.refloat_partner_signal = on;
    return c;
}

} // namespace

TEST(ArHoldReleaseTest, PartnerRefloatSeesADiscontinuityRecordedWithoutThePendingMark) {
    GnssConfig cfg;
    LogManager log;
    StateManager sm{ 5, log, cfg };
    for (const char* sat : { "G01", "G02" })
        for (const char* sig : { "L1W", "L2W" })
            sm.addAmbiguity(StateKeys::makeAmbiguityKey(sat, sig), 0.0);
    AmbiguityResolver resolver(partnerConfig(true), log);
    const std::vector<PppObservation> none;
    for (int k = 0; k < 4; ++k)
        EXPECT_TRUE(resolver.resolve(sm, none).release_satellites.empty()) << "epoch " << k;

    // The post-fit screen resets one row on its own copy and records it this way.
    ASSERT_TRUE(sm.noteAmbiguityDiscontinuity(StateKeys::makeAmbiguityKey("G01", "L1W")));
    const auto r = resolver.resolve(sm, none);
    EXPECT_EQ(r.release_satellites, (std::vector<std::string>{ "G01" }));
    EXPECT_EQ(resolver.partnerReleaseCount(), 1);

    // The caller refloats both signals; they begin together and the rule stays quiet.
    EXPECT_EQ(resolver.refloatSatellites(sm, r.release_satellites, 10.0), 2);
    EXPECT_TRUE(resolver.resolve(sm, none).release_satellites.empty());
    EXPECT_TRUE(resolver.resolve(sm, none).release_satellites.empty());
    EXPECT_EQ(resolver.partnerReleaseCount(), 1);
}

TEST(ArHoldReleaseTest, PartnerRefloatOffRequestsNothing) {
    GnssConfig cfg;
    LogManager log;
    StateManager sm{ 5, log, cfg };
    for (const char* sig : { "L1W", "L2W" })
        sm.addAmbiguity(StateKeys::makeAmbiguityKey("G01", sig), 0.0);
    AmbiguityResolver resolver(partnerConfig(false), log);
    const std::vector<PppObservation> none;
    for (int k = 0; k < 4; ++k) resolver.resolve(sm, none);
    sm.noteAmbiguityDiscontinuity(StateKeys::makeAmbiguityKey("G01", "L1W"));
    EXPECT_TRUE(resolver.resolve(sm, none).release_satellites.empty());
    EXPECT_EQ(resolver.partnerReleaseCount(), 0);
}

TEST(ArHoldReleaseTest, RefloatSatellitesResetsOnlyTheListedSatellite) {
    GnssConfig cfg;
    LogManager log;
    StateManager sm{ 5, log, cfg };
    const auto i1 = sm.addAmbiguity(StateKeys::makeAmbiguityKey("G01", "L1W"), 0.0, 0.001);
    const auto i2 = sm.addAmbiguity(StateKeys::makeAmbiguityKey("G01", "L2W"), 0.0, 0.001);
    const auto i3 = sm.addAmbiguity(StateKeys::makeAmbiguityKey("G02", "L1W"), 0.0, 0.001);
    AmbiguityResolver resolver(partnerConfig(false), log);
    EXPECT_EQ(resolver.refloatSatellites(sm, { "G01" }, 10.0), 2);
    EXPECT_DOUBLE_EQ(sm.getStateVariance(static_cast<int>(i1)), 100.0);
    EXPECT_DOUBLE_EQ(sm.getStateVariance(static_cast<int>(i2)), 100.0);
    EXPECT_DOUBLE_EQ(sm.getStateVariance(static_cast<int>(i3)), 1e-6);
}
