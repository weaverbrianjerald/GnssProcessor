// Iono-deweight release hygiene (docs/SPEC_deweight_release_hygiene.md): unit tests
// for the gate's release-edge capture and for the knob resolution that decides which
// increment can fire at all.
//
// The tests target what can go wrong without any visible symptom.  The seam itself
// (networkIonoDwRelease) needs a solved NetworkEstimator, an SH model and a live
// station roster to run one pass, which no unit fixture in this tree builds; its
// behaviour is graded on the replay legs.  What is unit-testable, and would fail
// without a visible symptom in the field, is:
//
//   ReleaseEdgeNotTransitionList: the edge must be captured around the slew.  A
//       station can rise off the floor with no engage/release flag flip (an engaged
//       station whose z decays inside the hysteresis band gets rampTarget > floor, and
//       exactly 1.0 at sigma_lo), so a consumer built on lastTransitions() would miss
//       real releases.  This is why the gate keeps a second list.
//   ReleaseEdgeFiresOncePerEra: one edge per era end, not once per epoch above eps.
//   ReleaseTrackingOptIn: an unarmed gate never populates the list (off costs
//       nothing) and the numerics are unchanged either way.
//   AdvanceWatermarkGuardsStaleList: the producer skips the whole gate block on a
//       datum-change epoch (there is no advance(false) caller), leaving both per-call
//       lists holding the previous epoch's contents.  advanceCount() lets a consumer
//       outside that block tell "nothing released" from "the gate did not run";
//       without it the response double-fires.
//   KnobsResolveOffToInert: with every increment at its default (off),
//       dw_hygiene_on resolves false, so the seam returns before allocating and the
//       assembly's two lookups stay a null test and an empty() test (invariant #6).
//   KnobsRequireDeviationChannel: the screens standardize the harvested T5 deviation;
//       without that channel they have no statistic and must disarm rather than fall
//       back to the linearization point, which is frame-arbitrary.
//   KnobsInc1RequiresAdaptiveGate: a static-list station holds one weight for the
//       whole run and cannot produce a release edge, so INC-1 must resolve inert.

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkIonoDeweightGate.hpp"
#include "filter/NetworkIonoDwRelease.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

// Same analytically-known population as test_network_iono_deweight_gate: five stations
// at 0.0 and five at 2.0 give median 2.0 for any PROBE >= 2, and MAD 2.0 for any
// PROBE >= 4, so there z_PROBE = (probe - 2) / (1.4826 * 2) is exact and the scale
// does not depend on the probe.  For 2 < PROBE < 4 the MAD is |probe - 2| (see the
// constants below).
void feedEpoch(NetworkIonoDeweightGate& g, double probe) {
    for (int i = 0; i < 5; ++i) {
        g.observe("BGLO" + std::to_string(i), 0.0);
        g.observe("BGHI" + std::to_string(i), 2.0);
    }
    g.observe("PROBE", probe);
    g.advance(true);
}

NetworkIonoDeweightGate makeAdaptive(double rate_limit, double floor) {
    // window_epochs tiny => phi ~ 0 => the EWMA tracks the latest sample exactly, which
    // isolates the state machine from the smoothing.
    return NetworkIonoDeweightGate(/*static*/ {}, /*static_w*/ 0.0, /*adaptive*/ true,
                                   /*window_epochs*/ 1.0e-3, /*sigma_hi*/ 3.0,
                                   /*sigma_lo*/ 2.0, floor, rate_limit, /*min_epochs*/ 1);
}

constexpr double kEps = 1e-9;

// With the probe at 20 the 11-station population is {0 x5, 2 x5, 20}: median 2.0,
// MAD 2.0 => mad_sigma = 2.9652 and z = (20 - 2) / 2.9652 = 6.07 > sigma_hi => engage,
// ramp target = floor.  With the probe at 3 it is {0 x5, 2 x5, 3}: median 2.0, MAD 1.0
// => mad_sigma = 1.4826 and z = 0.674 < sigma_lo => a real release flip.
//
// The probe must not be 2.0: at exactly 2.0 the deviation set is {2 x5, 0 x6}, whose
// median is 0, so mad_sigma == 0 and the gate skips its whole engage/release block
// (target 1.0, weight ramps up, `engaged` never cleared).  That is a third release
// route with no transition, covered in its own case below, but it is not the
// flag-flip path these two constants drive.
constexpr double kProbeEngage = 20.0;
constexpr double kProbeRelease = 3.0;

TEST(IonoDwReleaseHygiene, ReleaseTrackingOptIn) {
    // Unarmed: the list stays empty even across a genuine 0 -> 1 rise, and the weights
    // themselves are identical to the armed run (the capture is observation-only).
    auto off = makeAdaptive(/*rate*/ 1.0, /*floor*/ 0.0);
    auto on = makeAdaptive(/*rate*/ 1.0, /*floor*/ 0.0);
    on.enableReleaseTracking(kEps);
    for (int e = 0; e < 3; ++e) { feedEpoch(off, kProbeEngage); feedEpoch(on, kProbeEngage); }
    EXPECT_DOUBLE_EQ(off.weight("PROBE"), 0.0);
    EXPECT_TRUE(off.lastReleased().empty());
    for (int e = 0; e < 2; ++e) { feedEpoch(off, kProbeRelease); feedEpoch(on, kProbeRelease); }
    EXPECT_DOUBLE_EQ(off.weight("PROBE"), on.weight("PROBE"));   // numerics untouched
    EXPECT_TRUE(off.lastReleased().empty());                     // but off never records
    EXPECT_FALSE(on.everDeweighted().empty());
}

TEST(IonoDwReleaseHygiene, ReleaseEdgeFiresOncePerEra) {
    auto g = makeAdaptive(/*rate*/ 1.0, /*floor*/ 0.0);
    g.enableReleaseTracking(kEps);

    // Drive the probe station to the zero-weight floor and hold it there for several epochs.
    for (int e = 0; e < 3; ++e) {
        feedEpoch(g, kProbeEngage);
        EXPECT_TRUE(g.lastReleased().empty()) << "engaging must not report a release";
    }
    ASSERT_DOUBLE_EQ(g.weight("PROBE"), 0.0);

    // The era ends: exactly one edge, naming exactly this station.
    feedEpoch(g, kProbeRelease);
    ASSERT_EQ(g.lastReleased().size(), 1u);
    EXPECT_EQ(g.lastReleased()[0], "PROBE");
    EXPECT_GT(g.weight("PROBE"), kEps);

    // Still above eps on the following epochs => not a release again.  A per-epoch
    // "weight is above eps" test would re-fire here every epoch for the rest of the run.
    for (int e = 0; e < 3; ++e) {
        feedEpoch(g, kProbeRelease);
        EXPECT_TRUE(g.lastReleased().empty()) << "the edge must be a transition, not a level";
    }
}

TEST(IonoDwReleaseHygiene, ReleaseEdgeNotTransitionList) {
    // The central case.  With rate_limit 0.25 the weight leaves the floor by a single
    // slew step on the epoch the flag flips, and the tie rows re-fold only on the
    // next epoch's assembly.
    auto g = makeAdaptive(/*rate*/ 0.25, /*floor*/ 0.0);
    g.enableReleaseTracking(kEps);
    for (int e = 0; e < 6; ++e) feedEpoch(g, kProbeEngage);
    ASSERT_DOUBLE_EQ(g.weight("PROBE"), 0.0);
    ASSERT_TRUE(g.engaged("PROBE"));

    feedEpoch(g, kProbeRelease);
    // Both the flip and the weight edge fire on this epoch (target 1.0, slew +0.25 off
    // the floor).  The assertions check that the weight edge is what is reported, with
    // the post-slew value, and that it agrees with the gate's own weight().
    ASSERT_EQ(g.lastReleased().size(), 1u);
    EXPECT_NEAR(g.weight("PROBE"), 0.25, 1e-12);
    EXPECT_FALSE(g.engaged("PROBE"));

    // Now the converse, which the transition list cannot express: an engaged station
    // whose z decays into the hysteresis band climbs off the floor with no flag flip.
    auto h = makeAdaptive(/*rate*/ 0.25, /*floor*/ 0.0);
    h.enableReleaseTracking(kEps);
    for (int e = 0; e < 6; ++e) feedEpoch(h, kProbeEngage);
    ASSERT_DOUBLE_EQ(h.weight("PROBE"), 0.0);
    ASSERT_TRUE(h.engaged("PROBE"));
    // probe 9.0 => z ~ 2.36: inside (sigma_lo, sigma_hi), so no release flip, but the
    // ramp target is above the floor and the weight slews up.
    feedEpoch(h, 9.0);
    EXPECT_TRUE(h.engaged("PROBE")) << "still engaged: no transition is emitted";
    EXPECT_TRUE(h.lastTransitions().empty()) << "the transition list is empty here";
    EXPECT_GT(h.weight("PROBE"), kEps) << "yet the tie rows re-fold next epoch";
    EXPECT_EQ(h.lastReleased().size(), 1u)
        << "this is the release a transition-list consumer would MISS";
}

TEST(IonoDwReleaseHygiene, ReleaseEdgeSurvivesDegenerateMad) {
    // The third release route with no transition: when the population's MAD collapses
    // to zero (a degenerate or highly clustered epoch) the gate skips its whole
    // engage-release block, so `engaged` is never cleared and no transition is emitted,
    // yet target falls back to 1.0 and the weight ramps off the floor, re-folding the tie
    // rows.  A consumer keyed on the transition list, or on the `engaged` flag, sees
    // nothing here; the slew-edge capture still reports it.
    auto g = makeAdaptive(/*rate*/ 1.0, /*floor*/ 0.0);
    g.enableReleaseTracking(kEps);
    for (int e = 0; e < 3; ++e) feedEpoch(g, kProbeEngage);
    ASSERT_DOUBLE_EQ(g.weight("PROBE"), 0.0);
    ASSERT_TRUE(g.engaged("PROBE"));

    feedEpoch(g, 2.0);   // deviation set {2 x5, 0 x6} => median 0 => mad_sigma == 0
    EXPECT_TRUE(g.engaged("PROBE")) << "the flag is never cleared on this path";
    EXPECT_TRUE(g.lastTransitions().empty()) << "and no transition is emitted";
    EXPECT_GT(g.weight("PROBE"), kEps) << "yet the weight left the floor";
    EXPECT_EQ(g.lastReleased().size(), 1u)
        << "the slew-edge capture is the ONLY thing that sees this release";
}

TEST(IonoDwReleaseHygiene, AdvanceWatermarkGuardsStaleList) {
    auto g = makeAdaptive(/*rate*/ 1.0, /*floor*/ 0.0);
    g.enableReleaseTracking(kEps);
    EXPECT_EQ(g.advanceCount(), 0);

    for (int e = 0; e < 3; ++e) feedEpoch(g, kProbeEngage);
    const long long after_engage = g.advanceCount();
    EXPECT_EQ(after_engage, 3);

    feedEpoch(g, kProbeRelease);
    ASSERT_EQ(g.lastReleased().size(), 1u);
    const long long at_release = g.advanceCount();
    EXPECT_EQ(at_release, 4);

    // A datum-change epoch: the producer does not call advance() at all (its guard is
    // adaptive() && !datum_changed).  The release list therefore still holds the
    // previous epoch's contents; the watermark lets a consumer detect that.
    EXPECT_EQ(g.lastReleased().size(), 1u) << "the stale list is still populated";
    EXPECT_EQ(g.advanceCount(), at_release) << "but the watermark has not moved";

    // A held advance (the documented but uncalled branch) clears both lists and does
    // not count, so a consumer sees neither a stale release nor a phantom advance.
    g.advance(false);
    EXPECT_TRUE(g.lastReleased().empty());
    EXPECT_EQ(g.advanceCount(), at_release);
}

// --- Knob resolution ------------------------------------------------------------
// NetworkKnobs::snapshot is the single place every increment is resolved against the
// run's actual capability, and it is what the seam and the assembly read.

TEST(IonoDwReleaseHygiene, KnobsResolveOffToInert) {
    GnssConfig cfg;
    NetworkConfig nc;                       // all defaults = off
    const auto k = NetworkKnobs::snapshot(cfg, nc);
    EXPECT_FALSE(k.dw_hygiene_on);
    EXPECT_FALSE(k.dw_release_reseed);
    EXPECT_DOUBLE_EQ(k.dw_park_bound_tecu, 0.0);
    EXPECT_DOUBLE_EQ(k.dw_release_validate_k, 0.0);
    EXPECT_DOUBLE_EQ(k.dw_admit_validate_k, 0.0);
}

TEST(IonoDwReleaseHygiene, KnobsRequireDeviationChannel) {
    // Armed screens with no SH field at all: nothing to screen against.
    GnssConfig cfg;
    NetworkConfig nc;
    nc.iono_dw_park_bound_tecu = 300.0;
    nc.iono_dw_release_validate_k = 6.0;
    nc.iono_dw_admit_validate_k = 6.0;
    const auto k = NetworkKnobs::snapshot(cfg, nc);
    EXPECT_FALSE(k.dw_hygiene_on)
        << "without estimate_iono_sh there is no field and no harvested deviation";
    EXPECT_DOUBLE_EQ(k.dw_park_bound_tecu, 0.0);
    EXPECT_DOUBLE_EQ(k.dw_release_validate_k, 0.0);
    EXPECT_DOUBLE_EQ(k.dw_admit_validate_k, 0.0);
}

TEST(IonoDwReleaseHygiene, KnobsInc1RequiresAdaptiveGate) {
    // The reseed (INC-1) without the adaptive gate: a static-list station holds one
    // weight for the whole run, so no release edge can exist and the reseed must
    // resolve inert.
    GnssConfig cfg;
    NetworkConfig nc;
    nc.iono_dw_release_reseed = true;
    const auto k = NetworkKnobs::snapshot(cfg, nc);
    EXPECT_FALSE(k.dw_release_reseed);
    EXPECT_FALSE(k.dw_hygiene_on);
}

}  // namespace
}  // namespace GnssProcessor
