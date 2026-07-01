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

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {
namespace {

const GnssTime kT{2313, 259200.0};

// Build a clock-only epoch from a list of (sat, {signal: code_bias}) — exercises
// the satellite CODE-bias discontinuity bookkeeping (no phase biases).
NetworkSolutionEpoch makeCodeEpoch(
    const GnssTime& t,
    const std::vector<std::pair<std::string, std::map<std::string, double>>>& sats) {
    NetworkSolutionEpoch sol;
    sol.epoch_time  = t;
    sol.iod_ssr     = 3;
    sol.provider_id = 42;
    sol.solution_id = 1;
    for (const auto& [sat, biases] : sats) {
        NetworkSatSolution s;
        s.sat         = sat;
        s.has_clock   = true;
        s.clock_c0_m  = 0.0;
        s.code_bias_m = biases;
        sol.sats.push_back(s);
    }
    return sol;
}

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

// --- Satellite code-bias discontinuity bookkeeping (H1) -------------------

// A per-(const,signal) sum-to-zero set re-reference (a satellite for that signal
// rises/sets) steps every CONTINUING member's code bias -> bump those counters.
TEST(NetworkProductEmitterTest, CodeBiasBumpsOnSatSetChange) {
    SsrCorrectionStore store;
    NetworkProductEmitter emitter;

    GnssTime t = kT;
    // Three GPS sats, each two stable bands (band-tuple [1,2] never changes, so
    // the bump is attributable purely to the channel set-change).
    const std::map<std::string, double> two_band{{"C1C", 0.10}, {"C2W", 0.20}};
    emitter.emitToStore(makeCodeEpoch(t, {{"G01", two_band},
                                          {"G02", two_band},
                                          {"G03", two_band}}), store);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G01", "C1C"), 0);  // first sight
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G03", "C2W"), 0);

    // G03 sets -> the (G,C1C) and (G,C2W) sets change -> G01,G02 bump on both.
    t.tow += 30.0;
    emitter.emitToStore(makeCodeEpoch(t, {{"G01", two_band},
                                          {"G02", two_band}}), store);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G01", "C1C"), 1);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G02", "C1C"), 1);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G01", "C2W"), 1);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G02", "C2W"), 1);

    // The bump is visible to the consumer through the store.
    auto d = store.getCodeBiasDiscontinuity("G01", "C1C");
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(*d, 1);
}

// The per-satellite ionosphere-free pin re-references when a satellite's
// distinct-band set changes WITHOUT a (const,signal) membership change (it drops
// a band while peers keep the channel populated) -> bump that satellite's
// surviving signals, and ONLY that satellite's.
TEST(NetworkProductEmitterTest, CodeBiasBumpsOnIfPinBandChange) {
    SsrCorrectionStore store;
    NetworkProductEmitter emitter;

    GnssTime t = kT;
    // G01 on three bands, G02 on two bands.  Both channels (G,C1C)/(G,C5Q) carry
    // {G01,G02}; (G,C2W) carries only {G01}.
    emitter.emitToStore(makeCodeEpoch(t, {{"G01", {{"C1C", 0.1}, {"C2W", 0.2}, {"C5Q", 0.3}}},
                                          {"G02", {{"C1C", 0.1}, {"C5Q", 0.3}}}}), store);

    // G01 drops C2W: (G,C1C) and (G,C5Q) sets are UNCHANGED ({G01,G02}); (G,C2W)
    // vanishes (not a continuing-channel set-change).  Only G01's band-tuple
    // [1,2,5] -> [1,5] changed -> G01's C1C and C5Q bump; G02 untouched.
    t.tow += 30.0;
    emitter.emitToStore(makeCodeEpoch(t, {{"G01", {{"C1C", 0.1}, {"C5Q", 0.3}}},
                                          {"G02", {{"C1C", 0.1}, {"C5Q", 0.3}}}}), store);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G01", "C1C"), 1);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G01", "C5Q"), 1);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G02", "C1C"), 0);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G02", "C5Q"), 0);
}

// A forced (clock-pivot) datum change bumps every code counter for exactly one
// emit, even with an identical satellite set and identical biases.
TEST(NetworkProductEmitterTest, CodeBiasForcedDatumChangeBumpsOneEmit) {
    SsrCorrectionStore store;
    NetworkProductEmitter emitter;

    GnssTime t = kT;
    const std::vector<std::pair<std::string, std::map<std::string, double>>> net{
        {"G01", {{"C1C", 0.1}}}, {"E11", {{"C1C", 0.2}}}};
    emitter.emitToStore(makeCodeEpoch(t, net), store);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G01", "C1C"), 0);

    t.tow += 30.0;
    emitter.markDatumChange();
    emitter.emitToStore(makeCodeEpoch(t, net), store);  // identical content
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G01", "C1C"), 1);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("E11", "C1C"), 1);

    // Applies to exactly one emit.
    t.tow += 30.0;
    emitter.emitToStore(makeCodeEpoch(t, net), store);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G01", "C1C"), 1);
}

// An unchanged satellite set + unchanged band composition never bumps, even as
// the bias value drifts (a code RW step is not a datum discontinuity — there is
// no magnitude threshold for code).
TEST(NetworkProductEmitterTest, CodeBiasNoBumpOnSteadyStateOrDrift) {
    SsrCorrectionStore store;
    NetworkProductEmitter emitter;

    GnssTime t = kT;
    emitter.emitToStore(makeCodeEpoch(t, {{"G01", {{"C1C", 0.10}, {"C2W", 0.20}}}}), store);
    t.tow += 30.0;
    // Same set/bands, biases drift by > a GPS L1 cycle — still no bump.
    emitter.emitToStore(makeCodeEpoch(t, {{"G01", {{"C1C", 0.55}, {"C2W", 0.95}}}}), store);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G01", "C1C"), 0);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G01", "C2W"), 0);
}

// A wholly new (constellation, signal) channel is a first-sighting -> counter 0,
// and it does not disturb an unrelated, unchanged channel.
TEST(NetworkProductEmitterTest, CodeBiasNewChannelFirstSightingNoBump) {
    SsrCorrectionStore store;
    NetworkProductEmitter emitter;

    GnssTime t = kT;
    emitter.emitToStore(makeCodeEpoch(t, {{"G01", {{"C1C", 0.1}}}}), store);

    // Introduce a brand-new Galileo channel; GPS is untouched.
    t.tow += 30.0;
    emitter.emitToStore(makeCodeEpoch(t, {{"G01", {{"C1C", 0.1}}},
                                          {"E11", {{"C1C", 0.2}}}}), store);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("E11", "C1C"), 0);  // first sight
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G01", "C1C"), 0);  // unrelated, steady
}

} // namespace
} // namespace GnssProcessor
