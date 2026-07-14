// Iono deweight card: unit tests for the standalone NetworkIonoDeweightGate
// (include/filter/NetworkIonoDeweightGate.hpp) — the per-station SH-VTEC-constraint
// deweight factor with a v1 static list and a v2 network-relative IGG auto-trigger.
// The tests drive the class DIRECTLY with synthetic per-station statistics of KNOWN
// structure (the standalone-class precedent of test_network_carry_vce); no
// ApplicationNetwork coupling.
//
//   OffByteIdentical  — empty list + adaptive off (and a factor==1 list) ⇒ weight()
//                       is 1.0 for every station and active() is false: the producer
//                       is byte-identical to OFF (invariant #6).
//   StaticV1          — a listed station gets the fixed factor (0 ⇒ decouple);
//                       unlisted stays 1.0; telemetry reflects the static set.
//   RampTarget        — the smooth 3-segment IGG ramp is exact at the segment edges.
//   AdaptiveHysteresis— an outlier ENGAGES only above sigma_hi and RELEASES only below
//                       sigma_lo; a moderate outlier in the (lo,hi) band does NOT
//                       engage from released, but a still-engaged station HOLDS there
//                       (the anti-oscillation guarantee) — each z self-checked.
//   RateLimit         — the weight slews toward the floor by at most rate_limit/epoch.
//   MinEpochs         — a station cannot gate before min_epochs clean samples.
//   CleanGate         — a datum-change epoch (advance(false)) holds all state.
//   Degenerate        — an all-equal population (MAD==0) never engages / never NaNs.

#include "filter/NetworkIonoDeweightGate.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <set>
#include <string>

namespace GnssProcessor {
namespace {

// Background fixture: five stations at 0.0 and five at 2.0, plus a controllable PROBE.
// For any PROBE >= 2, the 11-station population has median 2.0 and MAD 2.0 (mad_sigma =
// 1.4826*2 = 2.965), both exactly invariant to the probe — so z_PROBE = (probe-2)/2.965
// is analytically known, and the test also self-checks it via zscore().
void feedEpoch(NetworkIonoDeweightGate& g, double probe) {
    for (int i = 0; i < 5; ++i) {
        g.observe("BGLO" + std::to_string(i), 0.0);
        g.observe("BGHI" + std::to_string(i), 2.0);
    }
    g.observe("PROBE", probe);
    g.advance(true);
}

NetworkIonoDeweightGate makeAdaptive(double rate_limit = 1.0, double floor = 0.0,
                                     int min_epochs = 1) {
    // window_epochs tiny ⇒ phi≈0 ⇒ EWMA tracks the latest sample exactly (isolates the
    // gate state-machine from the smoothing, which is exercised implicitly elsewhere).
    return NetworkIonoDeweightGate(/*static*/ {}, /*static_w*/ 0.0, /*adaptive*/ true,
                                   /*window_epochs*/ 1.0e-3, /*sigma_hi*/ 3.0,
                                   /*sigma_lo*/ 2.0, floor, rate_limit, min_epochs);
}

TEST(NetworkIonoDeweightGate, OffByteIdentical) {
    NetworkIonoDeweightGate off_default;
    EXPECT_FALSE(off_default.active());
    EXPECT_FALSE(off_default.adaptive());
    EXPECT_DOUBLE_EQ(off_default.weight("ANYSTA00XXX"), 1.0);

    NetworkIonoDeweightGate off_ctor({}, 0.0, false, 60.0, 3.0, 2.0, 0.0, 0.25, 30);
    EXPECT_FALSE(off_ctor.active());
    EXPECT_DOUBLE_EQ(off_ctor.weight("A"), 1.0);

    // A static list with factor==1 is inert (clamped to 1.0 ⇒ no deweight ⇒ not active).
    NetworkIonoDeweightGate unit_factor({"A", "B"}, 1.0, false, 60.0, 3.0, 2.0, 0.0, 0.25, 30);
    EXPECT_FALSE(unit_factor.active());
    EXPECT_DOUBLE_EQ(unit_factor.weight("A"), 1.0);
}

TEST(NetworkIonoDeweightGate, StaticV1) {
    // factor 0 ⇒ full decouple for the listed stations, 1.0 for everyone else.
    NetworkIonoDeweightGate g({"UFPR00BRA", "POAL00BRA"}, 0.0, false, 60.0, 3.0, 2.0,
                              0.0, 0.25, 30);
    EXPECT_TRUE(g.active());
    EXPECT_DOUBLE_EQ(g.weight("UFPR00BRA"), 0.0);
    EXPECT_DOUBLE_EQ(g.weight("POAL00BRA"), 0.0);
    EXPECT_DOUBLE_EQ(g.weight("MBAR00UGA"), 1.0);
    EXPECT_EQ(g.staticCount(), 2u);
    EXPECT_EQ(g.everDeweighted().count("UFPR00BRA"), 1u);

    // Partial deweight passes through verbatim.
    NetworkIonoDeweightGate p({"UFPR00BRA"}, 0.3, false, 60.0, 3.0, 2.0, 0.0, 0.25, 30);
    EXPECT_DOUBLE_EQ(p.weight("UFPR00BRA"), 0.3);
    EXPECT_DOUBLE_EQ(p.weight("Z"), 1.0);
}

TEST(NetworkIonoDeweightGate, RampTarget) {
    NetworkIonoDeweightGate g({}, 0.0, true, 60.0, /*hi*/ 3.0, /*lo*/ 2.0, /*floor*/ 0.1,
                              0.25, 30);
    EXPECT_DOUBLE_EQ(g.rampTarget(1.5), 1.0);   // below release edge ⇒ full weight
    EXPECT_DOUBLE_EQ(g.rampTarget(2.0), 1.0);   // at release edge
    EXPECT_DOUBLE_EQ(g.rampTarget(3.0), 0.1);   // at trigger edge ⇒ floor
    EXPECT_DOUBLE_EQ(g.rampTarget(4.0), 0.1);   // above trigger ⇒ clamped to floor
    // Midpoint z=2.5 ⇒ t=0.5, heal=0.5 ⇒ floor + (1-floor)*0.25 = 0.325.
    EXPECT_NEAR(g.rampTarget(2.5), 0.325, 1e-12);
}

TEST(NetworkIonoDeweightGate, AdaptiveHysteresis) {
    NetworkIonoDeweightGate g = makeAdaptive(/*rate*/ 1.0, /*floor*/ 0.0, /*min_epochs*/ 1);

    // Phase A — a big outlier (z≈33) engages and snaps to the floor.
    feedEpoch(g, 100.0);
    EXPECT_GT(g.zscore("PROBE"), 3.0);
    EXPECT_TRUE(g.engaged("PROBE"));
    EXPECT_LT(g.weight("PROBE"), 1e-9);
    EXPECT_EQ(g.triggerEvents(), 1);
    EXPECT_EQ(g.releaseEvents(), 0);
    EXPECT_DOUBLE_EQ(g.weight("BGLO0"), 1.0);   // a well-behaved station is untouched

    // Phase B — lower to z≈2.36 (in the (2,3) hysteresis band): STAYS engaged, no new
    // trigger or release event.  (probe 9.0 ⇒ (9-2)/2.965 = 2.36)
    feedEpoch(g, 9.0);
    EXPECT_GT(g.zscore("PROBE"), 2.0);
    EXPECT_LT(g.zscore("PROBE"), 3.0);
    EXPECT_TRUE(g.engaged("PROBE"));            // hysteresis: below hi but above lo ⇒ held
    EXPECT_EQ(g.triggerEvents(), 1);
    EXPECT_EQ(g.releaseEvents(), 0);

    // Phase C — into the cluster (z=0 < lo): releases and recovers to full weight.
    feedEpoch(g, 1.0);
    EXPECT_LT(g.zscore("PROBE"), 2.0);
    EXPECT_FALSE(g.engaged("PROBE"));
    EXPECT_DOUBLE_EQ(g.weight("PROBE"), 1.0);
    EXPECT_EQ(g.releaseEvents(), 1);

    // A never-engaged station sitting in the (lo,hi) band must NOT engage (needs > hi).
    NetworkIonoDeweightGate fresh = makeAdaptive(1.0, 0.0, 1);
    feedEpoch(fresh, 9.0);   // z≈2.36
    feedEpoch(fresh, 9.0);
    EXPECT_GT(fresh.zscore("PROBE"), 2.0);
    EXPECT_LT(fresh.zscore("PROBE"), 3.0);
    EXPECT_FALSE(fresh.engaged("PROBE"));
    EXPECT_DOUBLE_EQ(fresh.weight("PROBE"), 1.0);
    EXPECT_EQ(fresh.triggerEvents(), 0);
}

TEST(NetworkIonoDeweightGate, RateLimit) {
    NetworkIonoDeweightGate g = makeAdaptive(/*rate*/ 0.25, /*floor*/ 0.0, /*min_epochs*/ 1);
    feedEpoch(g, 100.0);   // engages, target = floor = 0
    EXPECT_TRUE(g.engaged("PROBE"));
    EXPECT_NEAR(g.weight("PROBE"), 0.75, 1e-9);   // slewed by exactly rate_limit, not snapped
    feedEpoch(g, 100.0);
    EXPECT_NEAR(g.weight("PROBE"), 0.50, 1e-9);
    feedEpoch(g, 100.0);
    feedEpoch(g, 100.0);
    EXPECT_NEAR(g.weight("PROBE"), 0.0, 1e-9);     // reaches the floor after 4 epochs
}

TEST(NetworkIonoDeweightGate, MinEpochs) {
    NetworkIonoDeweightGate g = makeAdaptive(/*rate*/ 1.0, /*floor*/ 0.0, /*min_epochs*/ 5);
    for (int e = 0; e < 3; ++e) feedEpoch(g, 100.0);
    EXPECT_FALSE(g.engaged("PROBE"));              // immature ⇒ no gating yet
    EXPECT_DOUBLE_EQ(g.weight("PROBE"), 1.0);
    for (int e = 0; e < 3; ++e) feedEpoch(g, 100.0);
    EXPECT_GE(g.samples("PROBE"), 5);
    EXPECT_TRUE(g.engaged("PROBE"));               // matured ⇒ engages
    EXPECT_LT(g.weight("PROBE"), 1e-9);
}

TEST(NetworkIonoDeweightGate, CleanGate) {
    NetworkIonoDeweightGate g = makeAdaptive(1.0, 0.0, 1);
    feedEpoch(g, 100.0);
    const double w_engaged = g.weight("PROBE");
    const int n_before = g.samples("PROBE");
    // A datum-change epoch: observe then advance(false) ⇒ discard + hold.
    for (int i = 0; i < 5; ++i) {
        g.observe("BGLO" + std::to_string(i), 0.0);
        g.observe("BGHI" + std::to_string(i), 2.0);
    }
    g.observe("PROBE", 1.0);   // would otherwise release
    g.advance(false);
    EXPECT_DOUBLE_EQ(g.weight("PROBE"), w_engaged);  // held, not slewed
    EXPECT_EQ(g.samples("PROBE"), n_before);         // sample count frozen
    EXPECT_TRUE(g.engaged("PROBE"));
}

TEST(NetworkIonoDeweightGate, DegenerateNoNaN) {
    NetworkIonoDeweightGate g = makeAdaptive(1.0, 0.0, 1);
    // All ten stations identical ⇒ MAD == 0 ⇒ no z ⇒ nobody engages, nothing is NaN.
    for (int e = 0; e < 4; ++e) {
        for (int i = 0; i < 10; ++i) g.observe("S" + std::to_string(i), 1.0);
        g.advance(true);
    }
    for (int i = 0; i < 10; ++i) {
        const double w = g.weight("S" + std::to_string(i));
        EXPECT_TRUE(std::isfinite(w));
        EXPECT_DOUBLE_EQ(w, 1.0);
        EXPECT_FALSE(g.engaged("S" + std::to_string(i)));
    }
    EXPECT_EQ(g.triggerEvents(), 0);
}

}  // namespace
}  // namespace GnssProcessor
