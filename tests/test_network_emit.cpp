/**
 * @file test_network_emit.cpp
 * @brief Unit tests for the PPP-RTK network product emitter (W8a, Sink A).
 *
 * Characterizes the producer/consumer round-trip contract through an OWNED
 * (non-singleton) SsrCorrectionStore: the network's estimated satellite clock +
 * code/phase bias must reappear verbatim through the store's consumer getters,
 * and the emitter-owned discontinuity counter must bump exactly on a bias jump
 * (or a forced datum change) — the signal the consumer's noteSsrDiscontinuity
 * turns into an ambiguity reset.  The full integer-recovery closure (feeding a
 * held-out station through the PPP_RTK_User KF) lands once W4 (estimator) and W7
 * (consumer mode) exist.
 */

#include <gtest/gtest.h>

#include "products/NetworkProductEmitter.hpp"
#include "products/SsrCorrectionStore.hpp"

namespace GnssProcessor {
namespace {

const GnssTime kT{2313, 259200.0};

// One-satellite (G01) solution: clock + C1W code bias + L1C phase bias.
NetworkSolutionEpoch makeEpoch(const GnssTime& t, double clk, double cbias,
                               double pbias, int wl = 1, bool integer = true) {
    NetworkSolutionEpoch sol;
    sol.epoch_time  = t;
    sol.iod_ssr     = 3;
    sol.provider_id = 42;
    sol.solution_id = 1;

    NetworkSatSolution s;
    s.sat            = "G01";
    s.has_clock      = true;
    s.clock_c0_m     = clk;
    s.code_bias_m["C1W"] = cbias;
    s.phase_bias["L1C"]  = NetworkPhaseBias{pbias, integer, wl};
    sol.sats.push_back(s);
    return sol;
}

TEST(NetworkProductEmitterTest, RoundTripsThroughStore) {
    SsrCorrectionStore store;
    NetworkProductEmitter emitter;
    emitter.emitToStore(makeEpoch(kT, /*clk=*/12.5, /*cb=*/0.3, /*pb=*/-0.7), store);

    auto oc = store.getOrbitClock("G01", kT);
    ASSERT_TRUE(oc.has_value());
    EXPECT_TRUE(oc->has_clock);
    EXPECT_FALSE(oc->has_orbit);            // emitter pushes clock only (orbit from IGS)
    EXPECT_NEAR(oc->c0, 12.5, 1e-12);

    EXPECT_NEAR(store.getCodeBias("G01", "C1W"), 0.3, 1e-12);

    auto pb = store.getPhaseBias("G01", "L1C");
    ASSERT_TRUE(pb.has_value());
    EXPECT_NEAR(pb->bias_m, -0.7, 1e-12);
    EXPECT_TRUE(pb->signal_integer);
    EXPECT_EQ(pb->wide_lane_integer, 1);
    EXPECT_EQ(pb->discontinuity_counter, 0);   // first emit
}

TEST(NetworkProductEmitterTest, DiscontinuityCounterBumpsOnBiasJumpOnly) {
    SsrCorrectionStore store;
    NetworkProductEmitter emitter;   // default 0.05 m threshold

    GnssTime t = kT;
    emitter.emitToStore(makeEpoch(t, 12.5, 0.3, -0.70), store);
    EXPECT_EQ(emitter.discontinuityCounter("G01", "L1C"), 0);

    // Steady bias (1 cm change < 0.05 m) -> no bump.
    t.tow += 30.0;
    emitter.emitToStore(makeEpoch(t, 12.6, 0.3, -0.71), store);
    EXPECT_EQ(emitter.discontinuityCounter("G01", "L1C"), 0);
    {
        auto pb = store.getPhaseBias("G01", "L1C");
        ASSERT_TRUE(pb.has_value());
        EXPECT_EQ(pb->discontinuity_counter, 0);
    }

    // Cycle-level jump (0.61 m > 0.05 m) -> bump, and it shows in the store.
    t.tow += 30.0;
    emitter.emitToStore(makeEpoch(t, 12.7, 0.3, -0.10), store);
    EXPECT_EQ(emitter.discontinuityCounter("G01", "L1C"), 1);
    {
        auto pb = store.getPhaseBias("G01", "L1C");
        ASSERT_TRUE(pb.has_value());
        EXPECT_EQ(pb->discontinuity_counter, 1);
    }

    // An untracked (sat,signal) reports 0.
    EXPECT_EQ(emitter.discontinuityCounter("G02", "L1C"), 0);
}

TEST(NetworkProductEmitterTest, MarkDatumChangeForcesBumpForExactlyOneEmit) {
    SsrCorrectionStore store;
    NetworkProductEmitter emitter;

    emitter.emitToStore(makeEpoch(kT, 12.5, 0.3, -0.70), store);
    EXPECT_EQ(emitter.discontinuityCounter("G01", "L1C"), 0);

    // Forced datum change bumps even though the bias is identical.
    GnssTime t = kT; t.tow += 30.0;
    emitter.markDatumChange();
    emitter.emitToStore(makeEpoch(t, 12.5, 0.3, -0.70), store);
    EXPECT_EQ(emitter.discontinuityCounter("G01", "L1C"), 1);

    // The forced bump applies to exactly one emit.
    t.tow += 30.0;
    emitter.emitToStore(makeEpoch(t, 12.5, 0.3, -0.70), store);
    EXPECT_EQ(emitter.discontinuityCounter("G01", "L1C"), 1);
}

// De-singletoning means a producer's store is isolated: pushing into one owned
// store must not leak into another (nor into the global singleton).
TEST(NetworkProductEmitterTest, OwnedStoresAreIsolated) {
    SsrCorrectionStore store_a;
    SsrCorrectionStore store_b;
    NetworkProductEmitter emitter;

    emitter.emitToStore(makeEpoch(kT, 12.5, 0.3, -0.70), store_a);

    EXPECT_FALSE(store_a.isEmpty());
    EXPECT_TRUE(store_b.isEmpty());
}

} // namespace
} // namespace GnssProcessor
