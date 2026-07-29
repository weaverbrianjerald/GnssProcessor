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
#include "filter/NetworkPivotHandover.hpp"   // A1 detector unit tests

#include <map>
#include <set>
#include <sstream>
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

// Build a one-sat epoch carrying an arbitrary set of phase biases (signal -> value).
static NetworkSolutionEpoch phaseEpoch(const GnssTime& t, const std::string& sat,
                                       const std::map<std::string, double>& biases) {
    NetworkSolutionEpoch sol;
    sol.epoch_time = t; sol.iod_ssr = 3; sol.provider_id = 42; sol.solution_id = 1;
    NetworkSatSolution s; s.sat = sat;
    for (const auto& [sig, b] : biases) s.phase_bias[sig] = NetworkPhaseBias{b, true, 1};
    sol.sats.push_back(s);
    sol.sats.back().has_clock = false;
    return sol;
}

// H3: with network.disc_threshold_frac_cycles armed, the jump gate is frac·λ
// PER (sat,signal), so the SAME step magnitude crosses the shorter-λ band but not
// the longer-λ one — the whole point of making the gate uniform in cycles.
TEST(NetworkProductEmitterTest, PerLambdaThresholdSelectsByBand) {
    // frac = 0.25 ⇒ L1 gate 0.25·0.1903 = 4.76 cm, L5 gate 0.25·0.2548 = 6.37 cm.
    NetworkProductEmitter emitter(NetworkProductEmitter::kDefaultDiscThresholdM,
                                  /*emit_clock_disc=*/false, /*stale=*/0.0,
                                  /*code_bias_jump_test=*/false,
                                  /*clock_disc_on_set_change=*/false,
                                  /*disc_threshold_frac_cycles=*/0.25);
    SsrCorrectionStore store;
    GnssTime t = kT;
    // Baseline for two GPS bands on the same satellite.
    emitter.emitToStore(phaseEpoch(t, "G01", {{"L1C", 0.0}, {"L5Q", 0.0}}), store);
    EXPECT_EQ(emitter.discontinuityCounter("G01", "L1C"), 0);
    EXPECT_EQ(emitter.discontinuityCounter("G01", "L5Q"), 0);
    // A 0.055 m step on BOTH bands: above the L1 gate (4.76 cm), below the L5 (6.37 cm).
    t.tow += 30.0;
    emitter.emitToStore(phaseEpoch(t, "G01", {{"L1C", 0.055}, {"L5Q", 0.055}}), store);
    EXPECT_EQ(emitter.discontinuityCounter("G01", "L1C"), 1);   // L1 fired
    EXPECT_EQ(emitter.discontinuityCounter("G01", "L5Q"), 0);   // L5 held (shorter cycles)
    // The legacy flat 0.05 m gate would have fired BOTH — the per-λ contract is
    // what separates them.
}

// H3: frac = 0 is the legacy flat-metre gate, byte-for-byte — same disc decisions
// AND a byte-identical serialized snapshot (no v3 line) as the default emitter.
TEST(NetworkProductEmitterTest, PerLambdaFracZeroIsLegacyByteIdentical) {
    NetworkProductEmitter dflt;                              // flat 0.05 m default
    NetworkProductEmitter frac0(NetworkProductEmitter::kDefaultDiscThresholdM,
                                false, 0.0, false, false, /*frac=*/0.0);
    SsrCorrectionStore s1, s2;
    GnssTime t = kT;
    for (double b : {0.0, 0.03, -0.02, 0.10, 0.101}) {       // a sequence straddling 0.05 m
        dflt.emitToStore(makeEpoch(t, 0.0, 0.3, b), s1);
        frac0.emitToStore(makeEpoch(t, 0.0, 0.3, b), s2);
        t.tow += 30.0;
    }
    EXPECT_EQ(dflt.discontinuityCounter("G01", "L1C"),
              frac0.discontinuityCounter("G01", "L1C"));
    EXPECT_GT(dflt.discontinuityCounter("G01", "L1C"), 0);    // the sequence actually fired
    std::ostringstream o1, o2;
    dflt.serializeState(o1);
    frac0.serializeState(o2);
    EXPECT_EQ(o1.str(), o2.str());                            // v1/v2, no discfrac ⇒ byte-identical
}

// H3: a frac-armed emitter's snapshot round-trips (v3 carries the frac; a mismatched
// restore is rejected).
TEST(NetworkProductEmitterTest, PerLambdaSnapshotRoundTrip) {
    NetworkProductEmitter e(NetworkProductEmitter::kDefaultDiscThresholdM,
                            false, 0.0, false, false, /*frac=*/0.25);
    SsrCorrectionStore store;
    e.emitToStore(phaseEpoch(kT, "G01", {{"L1C", 0.0}}), store);
    std::ostringstream os; e.serializeState(os);
    EXPECT_NE(os.str().find("EMITTER,v3,"), std::string::npos);   // frac armed ⇒ v3 header
    // A matching-config emitter restores; a flat (frac=0) one rejects the v3 snapshot.
    NetworkProductEmitter match(NetworkProductEmitter::kDefaultDiscThresholdM,
                                false, 0.0, false, false, 0.25);
    NetworkProductEmitter flat;
    std::istringstream im(os.str()), ifl(os.str());
    EXPECT_TRUE(match.restoreState(im));
    EXPECT_FALSE(flat.restoreState(ifl));
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

// --- A2: clock discontinuity channel + per-constellation bump scoping ----------

TEST(NetworkProductEmitterTest, ClockDiscOffByDefaultIsByteIdentical) {
    SsrCorrectionStore store;
    NetworkProductEmitter emitter;   // emit_clock_disc = false (default)
    emitter.emitToStore(makeCodeEpoch(kT, {{"G01", {{"C1W", 0.3}}}}), store);
    GnssTime t = kT; t.tow += 30.0;
    emitter.markDatumChange();       // global datum change
    emitter.emitToStore(makeCodeEpoch(t, {{"G01", {{"C1W", 0.3}}}}), store);
    // Clock disc channel disabled: counter stays 0 and the pushed record carries 0.
    EXPECT_EQ(emitter.clockDiscontinuityCounter("G01"), 0);
    EXPECT_EQ(store.getClockDiscontinuity("G01").value_or(-1), 0);
}

TEST(NetworkProductEmitterTest, ClockDiscBumpsOnDatumChangeWhenEnabled) {
    SsrCorrectionStore store;
    NetworkProductEmitter emitter(0.05, /*emit_clock_disc=*/true);
    emitter.emitToStore(makeCodeEpoch(kT, {{"G01", {{"C1W", 0.3}}}}), store);
    EXPECT_EQ(emitter.clockDiscontinuityCounter("G01"), 0);   // first sighting records only

    GnssTime t = kT; t.tow += 30.0;
    emitter.markDatumChange();                                // global (all constellations)
    emitter.emitToStore(makeCodeEpoch(t, {{"G01", {{"C1W", 0.3}}}}), store);
    EXPECT_EQ(emitter.clockDiscontinuityCounter("G01"), 1);
    EXPECT_EQ(store.getClockDiscontinuity("G01").value_or(-1), 1);

    // Applies to exactly one emit.
    t.tow += 30.0;
    emitter.emitToStore(makeCodeEpoch(t, {{"G01", {{"C1W", 0.3}}}}), store);
    EXPECT_EQ(emitter.clockDiscontinuityCounter("G01"), 1);
}

TEST(NetworkProductEmitterTest, MarkDatumChangeScopesToConstellation) {
    SsrCorrectionStore store;
    NetworkProductEmitter emitter(0.05, /*emit_clock_disc=*/true);
    const std::vector<std::pair<std::string, std::map<std::string, double>>> two =
        {{"G01", {{"C1W", 0.3}}}, {"E11", {{"C1C", 0.2}}}};
    emitter.emitToStore(makeCodeEpoch(kT, two), store);

    GnssTime t = kT; t.tow += 30.0;
    emitter.markDatumChange({'G'});   // only GPS's clock datum re-referenced
    emitter.emitToStore(makeCodeEpoch(t, two), store);

    // Clock disc: G bumped, E untouched.
    EXPECT_EQ(emitter.clockDiscontinuityCounter("G01"), 1);
    EXPECT_EQ(emitter.clockDiscontinuityCounter("E11"), 0);
    // Code-bias disc follows the same scoping (satellite sets/bands unchanged).
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G01", "C1W"), 1);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("E11", "C1C"), 0);
}

// --- RT0-5 seam: forced bump PENDS across an epoch with zero constellation sats ---

// A datum change signalled on an epoch where the affected constellation was
// stripped from the emission (runaway quarantine / all-set) must NOT be consumed
// by that emit — none of the constellation's keys stepped, so consuming would
// lose the discontinuity signal.  The flag stays pending and lands (exactly once)
// at the constellation's next actual emit, while a constellation whose sats WERE
// present consumes its flag normally on the first emit.
TEST(NetworkProductEmitterTest, DatumChangePendsAcrossEpochWithNoConstellationSats) {
    SsrCorrectionStore store;
    NetworkProductEmitter emitter(0.05, /*emit_clock_disc=*/true);
    const std::vector<std::pair<std::string, std::map<std::string, double>>> both =
        {{"G01", {{"C1W", 0.3}}}, {"E11", {{"C1C", 0.2}}}};
    const std::vector<std::pair<std::string, std::map<std::string, double>>> e_only =
        {{"E11", {{"C1C", 0.2}}}};

    emitter.emitToStore(makeCodeEpoch(kT, both), store);   // first sight — records only

    // Datum change on BOTH constellations, but the emitted epoch carries only
    // Galileo (GPS quarantined): E consumes its flag now; G's stays pending.
    GnssTime t = kT; t.tow += 30.0;
    emitter.markDatumChange({'G', 'E'});
    emitter.emitToStore(makeCodeEpoch(t, e_only), store);
    EXPECT_EQ(emitter.clockDiscontinuityCounter("E11"), 1);           // present → consumed
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("E11", "C1C"), 1);
    EXPECT_EQ(emitter.clockDiscontinuityCounter("G01"), 0);           // absent → nothing landed
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G01", "C1W"), 0);

    // GPS returns: the pending bump lands NOW (and E, already consumed, does not
    // bump again — no double-count from the coincidence).
    t.tow += 30.0;
    emitter.emitToStore(makeCodeEpoch(t, both), store);
    EXPECT_EQ(emitter.clockDiscontinuityCounter("G01"), 1);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G01", "C1W"), 1);
    EXPECT_EQ(emitter.clockDiscontinuityCounter("E11"), 1);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("E11", "C1C"), 1);

    // ...and exactly once: the landed flag is consumed.
    t.tow += 30.0;
    emitter.emitToStore(makeCodeEpoch(t, both), store);
    EXPECT_EQ(emitter.clockDiscontinuityCounter("G01"), 1);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G01", "C1W"), 1);
}

// Card 2 (network.code_datum_time_linked): the driver signals a time-linked datum
// RE-SEED (a cold sum-to-zero channel or an IF-pin split forced to the 0 seed) by
// markSatDiscontinuity for the re-seeded satellites, and the emitter must land that
// bump on the CODE disc counter AT the emit that carries the stepped value — even with
// the clock-disc channel OFF (the code OSB re-references regardless of whether the
// clock channel is published).  This is the "landing" half of the value-step ↔ disc
// coincidence the card's falsifier checks end-to-end.
TEST(NetworkProductEmitterTest, ForcedSatDiscBumpsCodeCounterOnSteppedEmit) {
    SsrCorrectionStore store;
    NetworkProductEmitter emitter;   // emit_clock_disc = false (default) — code channel only
    // First sight records the baseline; no counter movement yet.
    emitter.emitToStore(makeCodeEpoch(kT, {{"G01", {{"C1W", 0.30}}}}), store);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G01", "C1W"), 0);

    // The driver's re-seed: force G01's discontinuity, then emit the STEPPED value in
    // the same epoch.  The code disc counter bumps on THIS emit (coincident with the
    // step) despite the clock-disc channel being off.
    GnssTime t = kT; t.tow += 30.0;
    emitter.markSatDiscontinuity({"G01"});
    emitter.emitToStore(makeCodeEpoch(t, {{"G01", {{"C1W", 0.90}}}}), store);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G01", "C1W"), 1);
    EXPECT_EQ(store.getCodeBiasDiscontinuity("G01", "C1W").value_or(-1), 1);

    // Continuity afterwards: no forced bump, value steady → counter holds (exactly once).
    t.tow += 30.0;
    emitter.emitToStore(makeCodeEpoch(t, {{"G01", {{"C1W", 0.90}}}}), store);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G01", "C1W"), 1);
}

// The GLOBAL (empty-arg) datum bump gets the same deferral: a tracked
// constellation absent from the bump's emit is demoted to a pending scoped flag
// and lands at its next actual emit — exactly once.
TEST(NetworkProductEmitterTest, GlobalDatumChangePendsForAbsentTrackedConstellation) {
    SsrCorrectionStore store;
    NetworkProductEmitter emitter(0.05, /*emit_clock_disc=*/true);
    const std::vector<std::pair<std::string, std::map<std::string, double>>> both =
        {{"G01", {{"C1W", 0.3}}}, {"E11", {{"C1C", 0.2}}}};
    const std::vector<std::pair<std::string, std::map<std::string, double>>> e_only =
        {{"E11", {{"C1C", 0.2}}}};

    emitter.emitToStore(makeCodeEpoch(kT, both), store);   // G tracked from here on

    GnssTime t = kT; t.tow += 30.0;
    emitter.markDatumChange();                              // global bump
    emitter.emitToStore(makeCodeEpoch(t, e_only), store);   // GPS quarantined
    EXPECT_EQ(emitter.clockDiscontinuityCounter("E11"), 1);
    EXPECT_EQ(emitter.clockDiscontinuityCounter("G01"), 0);

    t.tow += 30.0;
    emitter.emitToStore(makeCodeEpoch(t, both), store);     // GPS returns → pending lands
    EXPECT_EQ(emitter.clockDiscontinuityCounter("G01"), 1);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G01", "C1W"), 1);
    EXPECT_EQ(emitter.clockDiscontinuityCounter("E11"), 1);  // consumed at the bump epoch

    t.tow += 30.0;
    emitter.emitToStore(makeCodeEpoch(t, both), store);     // exactly once
    EXPECT_EQ(emitter.clockDiscontinuityCounter("G01"), 1);
}

// --- B1 (P0-2/P1-3): per-satellite retro-bump (markSatDiscontinuity) ------------

// The retro-bump steps ONLY the named satellite's phase/code/clock counters for
// exactly one emit — NOT the whole constellation (no reset storm), distinguishing it
// from the per-constellation markDatumChange.
TEST(NetworkProductEmitterTest, MarkSatDiscontinuityScopesToSatAndOneEmit) {
    SsrCorrectionStore store;
    NetworkProductEmitter emitter(0.05, /*emit_clock_disc=*/true);

    // Two GPS sats, each clock + code + phase, biases identical across epochs.
    auto ep = [](const GnssTime& t) {
        NetworkSolutionEpoch sol;
        sol.epoch_time = t; sol.iod_ssr = 3; sol.provider_id = 42; sol.solution_id = 1;
        for (const char* s : {"G01", "G02"}) {
            NetworkSatSolution ns;
            ns.sat = s; ns.has_clock = true; ns.clock_c0_m = 1.0;
            ns.code_bias_m["C1W"] = 0.2;
            ns.phase_bias["L1C"]  = NetworkPhaseBias{0.1, true, 1};
            sol.sats.push_back(ns);
        }
        return sol;
    };

    emitter.emitToStore(ep(kT), store);   // first sight — records only

    GnssTime t = kT; t.tow += 30.0;
    emitter.markSatDiscontinuity({"G01"});   // retro-bump G01 only
    emitter.emitToStore(ep(t), store);

    // G01 bumped on phase, code AND clock; G02 untouched on all three.
    EXPECT_EQ(emitter.discontinuityCounter("G01", "L1C"), 1);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G01", "C1W"), 1);
    EXPECT_EQ(emitter.clockDiscontinuityCounter("G01"), 1);
    EXPECT_EQ(emitter.discontinuityCounter("G02", "L1C"), 0);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G02", "C1W"), 0);
    EXPECT_EQ(emitter.clockDiscontinuityCounter("G02"), 0);

    // Applies to exactly one emit.
    t.tow += 30.0;
    emitter.emitToStore(ep(t), store);
    EXPECT_EQ(emitter.discontinuityCounter("G01", "L1C"), 1);
    EXPECT_EQ(emitter.clockDiscontinuityCounter("G01"), 1);
}

// --- A1: pivot-handover datum-change detector ----------------------------------

TEST(NetworkPivotHandoverTest, AdjacentEpochChangeAlwaysSignals) {
    const std::map<std::string, std::string> prev = {{"G", "G05"}};
    const std::map<std::string, std::string> now  = {{"G", "G07"}};
    std::set<char> cc;
    EXPECT_TRUE(pivotHandoverSignalled(now, prev, /*last=*/{}, /*check_last_ever=*/false, cc));
    EXPECT_EQ(cc.count('G'), 1u);
}

TEST(NetworkPivotHandoverTest, VanishReturnDifferentPivotSignalsOnlyWhenArmed) {
    const std::map<std::string, std::string> prev = {{"G", "G05"}};                 // E absent last epoch
    const std::map<std::string, std::string> last = {{"G", "G05"}, {"E", "E11"}};   // E's last-ever pivot
    const std::map<std::string, std::string> now  = {{"G", "G05"}, {"E", "E24"}};   // E returns w/ new pivot
    std::set<char> off, on;
    // OFF: the legacy adjacent-epoch arm treats E as a first sighting -> missed (the bug).
    EXPECT_FALSE(pivotHandoverSignalled(now, prev, last, /*check_last_ever=*/false, off));
    EXPECT_TRUE(off.empty());
    // ON: the persistent last-ever map catches the re-reference, scoped to E.
    EXPECT_TRUE(pivotHandoverSignalled(now, prev, last, /*check_last_ever=*/true, on));
    EXPECT_EQ(on.count('E'), 1u);
    EXPECT_EQ(on.count('G'), 0u);   // G unchanged
}

TEST(NetworkPivotHandoverTest, FirstEverSightingAndSteadyPivotNeverSignal) {
    // A constellation seen for the first time ever (absent from prev AND last) is
    // not a change even when armed.
    const std::map<std::string, std::string> now  = {{"G", "G05"}, {"E", "E11"}};
    std::set<char> cc;
    EXPECT_FALSE(pivotHandoverSignalled(now, /*prev=*/{{"G", "G05"}}, /*last=*/{{"G", "G05"}},
                                        /*check_last_ever=*/true, cc));
    EXPECT_TRUE(cc.empty());
    // A steady pivot map never signals.
    std::set<char> cc2;
    EXPECT_FALSE(pivotHandoverSignalled(now, now, now, /*check_last_ever=*/true, cc2));
    EXPECT_TRUE(cc2.empty());
}

// Call-site contract: prev_pivot/last_pivot advance ONLY when an epoch emits, so a
// handover epoch skipped after detection (solve failure, σ̂0 withhold) re-detects on
// every subsequent epoch until one emits — the discontinuity signal cannot be lost,
// and a transient never-emitted pivot flip resolves without a spurious bump.
TEST(NetworkPivotHandoverTest, SkippedEpochsRedetectUntilEmitAdvancesBaseline) {
    std::map<std::string, std::string> prev = {{"G", "G05"}};
    std::map<std::string, std::string> last = {{"G", "G05"}};

    // Epoch N: handover G05 -> G07, detected; the epoch is then SKIPPED
    // (baseline maps deliberately NOT advanced).
    const std::map<std::string, std::string> plan_n = {{"G", "G07"}};
    std::set<char> cc_n;
    EXPECT_TRUE(pivotHandoverSignalled(plan_n, prev, last, /*check_last_ever=*/true, cc_n));

    // Epoch N+1, same new pivot: still signals against the frozen baseline.
    std::set<char> cc_n1;
    EXPECT_TRUE(pivotHandoverSignalled(plan_n, prev, last, /*check_last_ever=*/true, cc_n1));
    EXPECT_EQ(cc_n1.count('G'), 1u);

    // Epoch N+1 emits: baseline advances exactly as the emit site does.
    prev = plan_n;
    for (const auto& kv : plan_n) last[kv.first] = kv.second;

    // Epoch N+2, steady pivot: no further signal.
    std::set<char> cc_n2;
    EXPECT_FALSE(pivotHandoverSignalled(plan_n, prev, last, /*check_last_ever=*/true, cc_n2));
    EXPECT_TRUE(cc_n2.empty());

    // Transient flip that never emits: N had G07 (skipped), N+1 is back to G05 —
    // vs the last-EMITTED baseline there is no change, so no spurious bump.
    const std::map<std::string, std::string> plan_back = {{"G", "G05"}};
    std::map<std::string, std::string> prev0 = {{"G", "G05"}};
    std::map<std::string, std::string> last0 = {{"G", "G05"}};
    std::set<char> cc_back;
    EXPECT_FALSE(pivotHandoverSignalled(plan_back, prev0, last0, /*check_last_ever=*/true, cc_back));
    EXPECT_TRUE(cc_back.empty());
}

// --- RTA-2: per-constellation scoping of the INTERNAL handover response ---------

// datumChangeAffectsKey answers only the SCOPE question (callers additionally gate
// on datum_changed): scoped OFF = every key affected (the legacy global carry skip /
// oscillator wipe, byte-identical); scoped ON = only keys whose satellite belongs to
// a changed constellation, with constellation-less keys conservatively affected.
TEST(NetworkPivotHandoverTest, DatumChangeScopingClassifiesKeysByConstellation) {
    const std::set<char> changed{'G'};

    // Scoped OFF: every key is affected, whatever its constellation.
    EXPECT_TRUE(datumChangeAffectsKey("G05", changed, /*scoped=*/false));
    EXPECT_TRUE(datumChangeAffectsKey("E11", changed, /*scoped=*/false));
    EXPECT_TRUE(datumChangeAffectsKey("", changed, /*scoped=*/false));

    // Scoped ON: only the changed constellation's keys are invalidated.
    EXPECT_TRUE(datumChangeAffectsKey("G05", changed, /*scoped=*/true));
    EXPECT_FALSE(datumChangeAffectsKey("E11", changed, /*scoped=*/true));
    EXPECT_FALSE(datumChangeAffectsKey("R08", changed, /*scoped=*/true));

    // Constellation-less key (no satellite id): conservatively AFFECTED even
    // under scoping — datum membership unknown, keep the global invalidation.
    EXPECT_TRUE(datumChangeAffectsKey("", changed, /*scoped=*/true));

    // A multi-constellation handover epoch scopes to the union of changed systems.
    const std::set<char> both{'G', 'E'};
    EXPECT_TRUE(datumChangeAffectsKey("E11", both, /*scoped=*/true));
    EXPECT_FALSE(datumChangeAffectsKey("C21", both, /*scoped=*/true));
}

// RTA-2 + A1/A2 integration: the scope set consumed by the internal response is
// EXACTLY the changed_consts the detector records for the emit bump — one source
// of truth, never re-derived.  A G-only handover therefore invalidates G keys
// (carry priors, clock oscillators) and no others.
TEST(NetworkPivotHandoverTest, ScopeSetIsTheDetectorsChangedConsts) {
    const std::map<std::string, std::string> now  = {{"G", "G07"}, {"E", "E11"}};
    const std::map<std::string, std::string> prev = {{"G", "G05"}, {"E", "E11"}};
    std::set<char> cc;
    ASSERT_TRUE(pivotHandoverSignalled(now, prev, /*last=*/{}, /*check_last_ever=*/false, cc));
    EXPECT_EQ(cc, std::set<char>({'G'}));
    EXPECT_TRUE(datumChangeAffectsKey("G21", cc, /*scoped=*/true));
    EXPECT_FALSE(datumChangeAffectsKey("E12", cc, /*scoped=*/true));
}

// --- RTB-6 (P2): compact pivot-map formatter for the service-health CSV ---------

// formatPivotMap renders the emit-baseline last_pivot map as one CSV-safe field:
// semicolon-joined const:sat entries in deterministic (map key) order, empty string
// for an empty map (no product emitted yet), and never a comma.
TEST(NetworkPivotHandoverTest, FormatPivotMapRendersCsvSafeField) {
    // Empty map (before the first emit) -> empty field.
    EXPECT_EQ(formatPivotMap({}), "");

    // Single constellation.
    EXPECT_EQ(formatPivotMap({{"G", "G05"}}), "G:G05");

    // Multi-constellation: deterministic std::map key order (E < G < R).
    const std::map<std::string, std::string> pivots =
        {{"G", "G05"}, {"E", "E11"}, {"R", "R08"}};
    const std::string s = formatPivotMap(pivots);
    EXPECT_EQ(s, "E:E11;G:G05;R:R08");

    // CSV-safety: the rendered field must never contain a comma.
    EXPECT_EQ(s.find(','), std::string::npos);
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

// --- RTA-3: solution-generation id -> store epoch surface -----------------------

// The emitter publishes a stamped generation id to the store-wide accessor; an
// unstamped epoch (emit_generation_id off) leaves the store's marker absent —
// the byte-identical OFF path a legacy consumer sees — and clear() drops it.
TEST(NetworkProductEmitterTest, GenerationIdStampsStoreOnlyWhenSet) {
    SsrCorrectionStore store;
    NetworkProductEmitter emitter;

    emitter.emitToStore(makeEpoch(kT, 12.5, 0.3, -0.7), store);   // unstamped
    EXPECT_FALSE(store.getGenerationId().has_value());

    GnssTime t = kT; t.tow += 30.0;
    NetworkSolutionEpoch sol = makeEpoch(t, 12.5, 0.3, -0.7);
    sol.generation_id = 1399161600u;   // week·604800 + ⌊tow⌋ of a first emit
    emitter.emitToStore(sol, store);
    ASSERT_TRUE(store.getGenerationId().has_value());
    EXPECT_EQ(*store.getGenerationId(), 1399161600u);

    store.clear();
    EXPECT_FALSE(store.getGenerationId().has_value());
}

// --- RTA-11: emitter staleness horizon ------------------------------------------

// A key returning after a gap LONGER than the horizon is a first sighting: the
// jump test must not compare against the hours-stale baseline, so no bump lands
// on the phase, code, or clock channel — while the horizon-off control emitter
// mints the spurious phase bump from the identical sequence.
TEST(NetworkProductEmitterTest, StalenessHorizonGapTreatedAsFirstSighting) {
    SsrCorrectionStore store;
    NetworkProductEmitter emitter(0.05, /*emit_clock_disc=*/true,
                                  /*staleness_horizon_s=*/100.0);

    emitter.emitToStore(makeEpoch(kT, 1.0, 0.2, 0.10), store);

    GnssTime t = kT; t.tow += 200.0;   // gap 200 s > 100 s horizon
    emitter.emitToStore(makeEpoch(t, 1.0, 0.2, 5.00), store);   // phase "jump" = stale compare
    EXPECT_EQ(emitter.discontinuityCounter("G01", "L1C"), 0);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G01", "C1W"), 0);
    EXPECT_EQ(emitter.clockDiscontinuityCounter("G01"), 0);

    // Control: same sequence, horizon off -> the legacy jump-test bump fires.
    SsrCorrectionStore store2;
    NetworkProductEmitter legacy(0.05, /*emit_clock_disc=*/true);
    legacy.emitToStore(makeEpoch(kT, 1.0, 0.2, 0.10), store2);
    legacy.emitToStore(makeEpoch(t, 1.0, 0.2, 5.00), store2);
    EXPECT_EQ(legacy.discontinuityCounter("G01", "L1C"), 1);
}

// RTA-11 fix: a GENUINE forced datum change landing on a beyond-horizon return
// STILL signals a discontinuity on every channel.  Producer emission and
// consumer carrier tracking are decoupled: a consumer with an unbroken arc (e.g.
// the A5 bias-age gate turned off) holds old-datum state the change must reset.
// Only the SPURIOUS jump test is suppressed on a stale baseline (previous test),
// never a real markDatumChange.  The bias is held constant here so ONLY the
// forced bump — not the jump test — can account for the count.
TEST(NetworkProductEmitterTest, StalenessHorizonStillSignalsForcedBumpOnStaleReturn) {
    SsrCorrectionStore store;
    NetworkProductEmitter emitter(0.05, /*emit_clock_disc=*/true,
                                  /*staleness_horizon_s=*/100.0);

    emitter.emitToStore(makeEpoch(kT, 1.0, 0.2, 0.10), store);

    GnssTime t = kT; t.tow += 200.0;   // gap > horizon
    emitter.markDatumChange();          // forced global bump pending at the return
    emitter.emitToStore(makeEpoch(t, 1.0, 0.2, 0.10), store);   // bias unchanged
    EXPECT_EQ(emitter.discontinuityCounter("G01", "L1C"), 1);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G01", "C1W"), 1);
    EXPECT_EQ(emitter.clockDiscontinuityCounter("G01"), 1);
}

// Inside the horizon everything behaves exactly as before: a genuine jump bumps,
// and a forced datum change lands on all three channels.
TEST(NetworkProductEmitterTest, StalenessHorizonShortGapStillBumps) {
    SsrCorrectionStore store;
    NetworkProductEmitter emitter(0.05, /*emit_clock_disc=*/true,
                                  /*staleness_horizon_s=*/100.0);

    emitter.emitToStore(makeEpoch(kT, 1.0, 0.2, 0.10), store);

    GnssTime t = kT; t.tow += 50.0;    // gap 50 s < horizon
    emitter.emitToStore(makeEpoch(t, 1.0, 0.2, 5.00), store);   // real jump
    EXPECT_EQ(emitter.discontinuityCounter("G01", "L1C"), 1);

    t.tow += 30.0;
    emitter.markDatumChange();
    emitter.emitToStore(makeEpoch(t, 1.0, 0.2, 5.00), store);
    EXPECT_EQ(emitter.discontinuityCounter("G01", "L1C"), 2);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G01", "C1W"), 1);
    EXPECT_EQ(emitter.clockDiscontinuityCounter("G01"), 1);
}

// --- RTB-7: emitter stream-state serialize/restore (warm resume) ---------------

// Three-satellite epoch (G01, G02, E11): clock + code + phase per sat, with the
// phase bias settable per sat so a jump can be driven selectively.
NetworkSolutionEpoch makeTriEpoch(const GnssTime& t,
                                  const std::map<std::string, double>& phase_bias,
                                  const std::set<std::string>& present) {
    NetworkSolutionEpoch sol;
    sol.epoch_time = t;
    sol.iod_ssr = 3; sol.provider_id = 42; sol.solution_id = 1;
    for (const auto& [sat, pb] : phase_bias) {
        if (!present.count(sat)) continue;
        NetworkSatSolution s;
        s.sat        = sat;
        s.has_clock  = true;
        s.clock_c0_m = 1.0;
        s.code_bias_m[sat[0] == 'E' ? "C1C" : "C1W"] = 0.2;
        s.phase_bias["L1C"] = NetworkPhaseBias{pb, true, 1};
        sol.sats.push_back(s);
    }
    return sol;
}

// The determinism invariant of RTB-7: an emit sequence with datum events —
// a per-constellation markDatumChange left PENDING by an absent constellation,
// a B1 retro-bump, and an RTA-11 staleness-horizon gap — snapshotted
// mid-sequence via serializeState, restored into a FRESH emitter via
// restoreState, and continued, produces counters + store records IDENTICAL to
// an uninterrupted run.  This is exactly the producer-bounce warm-resume path.
TEST(NetworkProductEmitterTest, StateRoundTripMidSequenceMatchesUninterrupted) {
    const std::set<std::string> all{"G01", "G02", "E11"};
    const std::set<std::string> e_only{"E11"};
    const std::map<std::string, double> pb0{{"G01", 0.10}, {"G02", 0.20}, {"E11", 0.30}};
    std::map<std::string, double> pb_shift = pb0;   // ep3: +5 m (stale gap — no bump)
    for (auto& kv : pb_shift) kv.second += 5.0;
    std::map<std::string, double> pb_shift2 = pb_shift;   // ep4: +5 m again (fresh — jump)
    for (auto& kv : pb_shift2) kv.second += 5.0;

    GnssTime t0 = kT, t1 = kT, t2 = kT, t3 = kT, t4 = kT;
    t1.tow += 30.0;
    t2.tow += 60.0;
    t3.tow += 260.0;   // 200 s after t2 — beyond the 100 s staleness horizon
    t4.tow += 290.0;   // 30 s after t3 — inside the horizon

    // Drive one emitter through the FULL sequence (the uninterrupted reference).
    auto driveFull = [&](NetworkProductEmitter& em, SsrCorrectionStore& store,
                         bool from_ep2_only) {
        if (!from_ep2_only) {
            em.emitToStore(makeTriEpoch(t0, pb0, all), store);      // ep0: first sight
            em.markDatumChange({'G', 'E'});                          // datum event
            em.emitToStore(makeTriEpoch(t1, pb0, e_only), store);   // ep1: G PENDING
        }
        em.markSatDiscontinuity({"E11"});                            // B1 retro-bump
        em.emitToStore(makeTriEpoch(t2, pb0, all), store);          // ep2: pending G lands
        em.emitToStore(makeTriEpoch(t3, pb_shift, all), store);     // ep3: stale gap — reseed
        em.emitToStore(makeTriEpoch(t4, pb_shift2, all), store);    // ep4: genuine jump
    };

    // Reference: uninterrupted.
    SsrCorrectionStore store_ref;
    NetworkProductEmitter ref(0.05, /*emit_clock_disc=*/true, /*staleness=*/100.0);
    driveFull(ref, store_ref, /*from_ep2_only=*/false);

    // Bounced producer: run through ep1 (the G bump still PENDING), snapshot,
    // restore into a FRESH emitter, continue ep2..ep4.
    SsrCorrectionStore store_a;
    NetworkProductEmitter a(0.05, true, 100.0);
    a.emitToStore(makeTriEpoch(t0, pb0, all), store_a);
    a.markDatumChange({'G', 'E'});
    a.emitToStore(makeTriEpoch(t1, pb0, e_only), store_a);
    std::ostringstream blob;
    a.serializeState(blob);

    SsrCorrectionStore store_b;
    NetworkProductEmitter b(0.05, true, 100.0);
    {
        std::istringstream is(blob.str());
        ASSERT_TRUE(b.restoreState(is));
    }
    driveFull(b, store_b, /*from_ep2_only=*/true);

    // Every counter on every channel identical to the uninterrupted run.
    for (const std::string& sat : all) {
        SCOPED_TRACE(sat);
        EXPECT_EQ(b.discontinuityCounter(sat, "L1C"), ref.discontinuityCounter(sat, "L1C"));
        const std::string code_sig = (sat[0] == 'E') ? "C1C" : "C1W";
        EXPECT_EQ(b.codeBiasDiscontinuityCounter(sat, code_sig),
                  ref.codeBiasDiscontinuityCounter(sat, code_sig));
        EXPECT_EQ(b.clockDiscontinuityCounter(sat), ref.clockDiscontinuityCounter(sat));
        // ...and the store records the consumer reads carry the same values.
        auto pb_ref = store_ref.getPhaseBias(sat, "L1C");
        auto pb_b   = store_b.getPhaseBias(sat, "L1C");
        ASSERT_TRUE(pb_ref.has_value());
        ASSERT_TRUE(pb_b.has_value());
        EXPECT_EQ(pb_b->discontinuity_counter, pb_ref->discontinuity_counter);
        EXPECT_DOUBLE_EQ(pb_b->bias_m, pb_ref->bias_m);
        EXPECT_EQ(store_b.getCodeBiasDiscontinuity(sat, code_sig).value_or(-1),
                  store_ref.getCodeBiasDiscontinuity(sat, code_sig).value_or(-1));
        EXPECT_EQ(store_b.getClockDiscontinuity(sat).value_or(-1),
                  store_ref.getClockDiscontinuity(sat).value_or(-1));
    }

    // Spot-check the sequence semantics themselves (so the parity above is not
    // vacuously comparing two wrong runs):
    //  - the PENDING G datum bump (marked at ep1, G absent) landed at ep2 across
    //    the snapshot/restore boundary — the RT0-5 seam survives the bounce;
    EXPECT_EQ(ref.clockDiscontinuityCounter("G01"), 1);
    EXPECT_EQ(b.clockDiscontinuityCounter("G01"), 1);
    //  - E11 = ep1 datum bump + ep2 retro-bump on all channels, then the stale
    //    ep3 reseed suppressed the 5 m "jump", and ep4's genuine jump added one
    //    phase bump (RTA-11 last-seen stamps restored exactly);
    EXPECT_EQ(b.clockDiscontinuityCounter("E11"), 2);
    EXPECT_EQ(b.discontinuityCounter("E11", "L1C"), 3);
    EXPECT_EQ(b.discontinuityCounter("G01", "L1C"), 2);   // ep2 pending-land + ep4 jump
}

// restoreState is all-or-nothing: a truncated blob (no EEND) and a
// construction-parameter mismatch both fail and leave the emitter untouched.
TEST(NetworkProductEmitterTest, StateRestoreRejectsTruncationAndParamMismatch) {
    SsrCorrectionStore store;
    NetworkProductEmitter src(0.05, /*emit_clock_disc=*/true, /*staleness=*/100.0);
    src.emitToStore(makeEpoch(kT, 1.0, 0.2, 0.10), store);
    std::ostringstream blob;
    src.serializeState(blob);
    const std::string full = blob.str();

    // Truncation: drop the EEND terminator (and a bit more).
    {
        NetworkProductEmitter dst(0.05, true, 100.0);
        std::istringstream is(full.substr(0, full.size() - 8));
        EXPECT_FALSE(dst.restoreState(is));
        // Untouched: still no tracked state.
        EXPECT_EQ(dst.discontinuityCounter("G01", "L1C"), 0);
        GnssTime t = kT; t.tow += 30.0;
        SsrCorrectionStore s2;
        dst.emitToStore(makeEpoch(t, 1.0, 0.2, 5.0), s2);   // would be a jump IF baseline restored
        EXPECT_EQ(dst.discontinuityCounter("G01", "L1C"), 0);   // first sighting — no baseline
    }

    // Parameter mismatch: differently-configured emitter must refuse the blob.
    {
        NetworkProductEmitter dst(0.05, /*emit_clock_disc=*/false, /*staleness=*/100.0);
        std::istringstream is(full);
        EXPECT_FALSE(dst.restoreState(is));
    }
    {
        NetworkProductEmitter dst(0.10, true, 100.0);
        std::istringstream is(full);
        EXPECT_FALSE(dst.restoreState(is));
    }
    {
        NetworkProductEmitter dst(0.05, true, 0.0);
        std::istringstream is(full);
        EXPECT_FALSE(dst.restoreState(is));
    }

    // Control: the exact-match emitter accepts the same blob and the restored
    // baseline turns the identical follow-up emit into a genuine jump bump.
    {
        NetworkProductEmitter dst(0.05, true, 100.0);
        std::istringstream is(full);
        ASSERT_TRUE(dst.restoreState(is));
        GnssTime t = kT; t.tow += 30.0;
        SsrCorrectionStore s2;
        dst.emitToStore(makeEpoch(t, 1.0, 0.2, 5.0), s2);
        EXPECT_EQ(dst.discontinuityCounter("G01", "L1C"), 1);
    }
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

// --- Opt-in code-bias VALUE jump test (network.code_bias_disc_jump_test) --------

// The always-on structural set/band triggers detect GAUGE re-references only;
// the Jul-2026 wire audit measured large unsignalled code-bias VALUE jumps
// beyond them (weak-separation wander at thin rosters).  Armed, the phase jump
// test applies to code: a fresh-baseline step beyond disc_threshold_m bumps the
// counter; a first sighting and sub-threshold drift never do.  Satellite set +
// band composition are held constant across every emit so the bump is
// attributable PURELY to the value jump, not a structural trigger.
TEST(NetworkProductEmitterTest, CodeBiasJumpTestBumpsOnValueJump) {
    SsrCorrectionStore store;
    NetworkProductEmitter emitter(0.05, /*emit_clock_disc=*/false,
                                  /*staleness_horizon_s=*/0.0,
                                  /*code_bias_jump_test=*/true);

    GnssTime t = kT;
    emitter.emitToStore(makeCodeEpoch(t, {{"G01", {{"C1C", 0.10}}}}), store);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G01", "C1C"), 0);   // first sighting

    // 0.2 m step > 0.05 m threshold -> bump, visible to the consumer via the store.
    t.tow += 30.0;
    emitter.emitToStore(makeCodeEpoch(t, {{"G01", {{"C1C", 0.30}}}}), store);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G01", "C1C"), 1);
    EXPECT_EQ(store.getCodeBiasDiscontinuity("G01", "C1C").value_or(-1), 1);

    // 0.01 m step < threshold -> no bump (noise-level drift is not a datum event).
    t.tow += 30.0;
    emitter.emitToStore(makeCodeEpoch(t, {{"G01", {{"C1C", 0.31}}}}), store);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G01", "C1C"), 1);
}

// Default-OFF byte-identity: the IDENTICAL sequence with the flag off never
// bumps — a value jump alone is invisible, exactly the pre-flag behavior where
// the structural set/band triggers are the only code-disc sources.
TEST(NetworkProductEmitterTest, CodeBiasJumpTestOffIsInert) {
    SsrCorrectionStore store;
    NetworkProductEmitter emitter;   // code_bias_jump_test defaults false

    GnssTime t = kT;
    emitter.emitToStore(makeCodeEpoch(t, {{"G01", {{"C1C", 0.10}}}}), store);
    t.tow += 30.0;
    emitter.emitToStore(makeCodeEpoch(t, {{"G01", {{"C1C", 0.30}}}}), store);  // 0.2 m step
    t.tow += 30.0;
    emitter.emitToStore(makeCodeEpoch(t, {{"G01", {{"C1C", 0.31}}}}), store);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("G01", "C1C"), 0);
    EXPECT_EQ(store.getCodeBiasDiscontinuity("G01", "C1C").value_or(-1), 0);
}

// --- Sum-to-zero clock set-change trigger (network.clock_disc_on_set_change) ----

// Clock-only epoch (no code/phase biases) from a satellite list — isolates the
// clock-satellite SET as the only thing that can change between emits.
NetworkSolutionEpoch makeClockEpoch(const GnssTime& t,
                                    const std::vector<std::string>& sats) {
    NetworkSolutionEpoch sol;
    sol.epoch_time  = t;
    sol.iod_ssr     = 3;
    sol.provider_id = 42;
    sol.solution_id = 1;
    for (const auto& sat : sats) {
        NetworkSatSolution s;
        s.sat        = sat;
        s.has_clock  = true;
        s.clock_c0_m = 1.0;
        sol.sats.push_back(s);
    }
    return sol;
}

// Under Σδtˢ=0 a constellation clock-set membership change IS a datum
// re-reference — the departing/arriving satellite's common-mode redistributes
// ∓ΔC0/N onto every survivor, with no pivot handover to signal it.  Armed
// (emit_clock_disc + clock_disc_on_set_change), every emitted clock of the
// changed constellation with seen_before bumps; the constellation's first emit
// and a rising satellite's own first sighting never do (no consumer holds
// old-datum state for them yet).
TEST(NetworkProductEmitterTest, ClockSetChangeBumpsSurvivors) {
    SsrCorrectionStore store;
    NetworkProductEmitter emitter(0.05, /*emit_clock_disc=*/true,
                                  /*staleness_horizon_s=*/0.0,
                                  /*code_bias_jump_test=*/false,
                                  /*clock_disc_on_set_change=*/true);

    // First emit of the constellation: no previous set to differ from -> no bump.
    GnssTime t = kT;
    emitter.emitToStore(makeClockEpoch(t, {"G01", "G02"}), store);
    EXPECT_EQ(emitter.clockDiscontinuityCounter("G01"), 0);
    EXPECT_EQ(emitter.clockDiscontinuityCounter("G02"), 0);

    // G02 sets: {G01,G02} -> {G01} -> the survivor bumps.
    t.tow += 30.0;
    emitter.emitToStore(makeClockEpoch(t, {"G01"}), store);
    EXPECT_EQ(emitter.clockDiscontinuityCounter("G01"), 1);
    EXPECT_EQ(store.getClockDiscontinuity("G01").value_or(-1), 1);

    // G03 rises: {G01} -> {G01,G03} -> the survivor bumps again; the NEW sat is
    // a first sighting -> stays 0.
    t.tow += 30.0;
    emitter.emitToStore(makeClockEpoch(t, {"G01", "G03"}), store);
    EXPECT_EQ(emitter.clockDiscontinuityCounter("G01"), 2);
    EXPECT_EQ(emitter.clockDiscontinuityCounter("G03"), 0);
    EXPECT_EQ(store.getClockDiscontinuity("G01").value_or(-1), 2);
    EXPECT_EQ(store.getClockDiscontinuity("G03").value_or(-1), 0);
}

// Default-OFF byte-identity: with only emit_clock_disc armed (the pre-flag A2
// config) the IDENTICAL membership churn never bumps — under the pivot datum a
// set change is NOT a gauge re-reference, and the new trigger must stay inert
// unless explicitly opted into.
TEST(NetworkProductEmitterTest, ClockSetChangeOffIsInert) {
    SsrCorrectionStore store;
    NetworkProductEmitter emitter(0.05, /*emit_clock_disc=*/true);   // set-change flag defaults false

    GnssTime t = kT;
    emitter.emitToStore(makeClockEpoch(t, {"G01", "G02"}), store);
    t.tow += 30.0;
    emitter.emitToStore(makeClockEpoch(t, {"G01"}), store);
    t.tow += 30.0;
    emitter.emitToStore(makeClockEpoch(t, {"G01", "G03"}), store);
    EXPECT_EQ(emitter.clockDiscontinuityCounter("G01"), 0);
    EXPECT_EQ(emitter.clockDiscontinuityCounter("G02"), 0);
    EXPECT_EQ(emitter.clockDiscontinuityCounter("G03"), 0);
    EXPECT_EQ(store.getClockDiscontinuity("G01").value_or(-1), 0);
}

// --- RTB-7 v2 header: state round-trip with the optional channels armed ---------

// With either optional channel armed, serializeState writes the EMITTER,v2
// header (7 fields, carrying the two new flags) plus the v2-only stream state —
// the prev-emit clock-satellite sets ('eks' lines) and the code-bias value
// baselines — so a warm-resumed producer continues the set-change and jump-test
// bookkeeping EXACTLY where the bounced process left it.  A flags-off emitter
// still writes v1 (5 fields: default-config snapshots stay byte-identical to
// pre-channel builds), and a construction-parameter mismatch on the new flags
// fails the restore all-or-nothing, leaving the emitter untouched (cold start).
TEST(NetworkProductEmitterTest, EmitterStateRoundTripV2) {
    GnssTime t0 = kT, t1 = kT, t2 = kT, t3 = kT;
    t1.tow += 30.0;
    t2.tow += 60.0;
    t3.tow += 90.0;
    // Clock + one code band per sat; G02's presence is switchable so ep2 can
    // drive a clock-set change (survivor bump) after the restore.
    auto ep = [](const GnssTime& t, double cb, bool with_g02) {
        std::vector<std::pair<std::string, std::map<std::string, double>>> sats =
            {{"G01", {{"C1W", cb}}}};
        if (with_g02) sats.push_back({"G02", {{"C1W", cb}}});
        return makeCodeEpoch(t, sats);
    };

    // Uninterrupted reference, both channels armed: ep0 first sight, ep1 a
    // 0.2 m value jump (code counters -> 1 under the armed jump test).
    SsrCorrectionStore store_ref;
    NetworkProductEmitter ref(0.05, /*emit_clock_disc=*/true,
                              /*staleness_horizon_s=*/0.0,
                              /*code_bias_jump_test=*/true,
                              /*clock_disc_on_set_change=*/true);
    ref.emitToStore(ep(t0, 0.10, /*with_g02=*/true), store_ref);
    ref.emitToStore(ep(t1, 0.30, /*with_g02=*/true), store_ref);

    // Bounced producer: identical two emits, then snapshot.
    SsrCorrectionStore store_a;
    NetworkProductEmitter a(0.05, true, 0.0, true, true);
    a.emitToStore(ep(t0, 0.10, true), store_a);
    a.emitToStore(ep(t1, 0.30, true), store_a);
    std::ostringstream blob;
    a.serializeState(blob);
    const std::string full = blob.str();

    // Armed => the v2 header (the flags travel in the section itself).
    EXPECT_EQ(full.rfind("EMITTER,v2,", 0), 0u);

    // Restore into a fresh SAME-config emitter succeeds.
    SsrCorrectionStore store_b;
    NetworkProductEmitter b(0.05, true, 0.0, true, true);
    {
        std::istringstream is(full);
        ASSERT_TRUE(b.restoreState(is));
    }

    // Continue BOTH: ep2 drops G02 — the survivor clock bump requires the
    // restored 'eks' prev clock-set (and the (G,C1W) channel set change bumps
    // code); ep3 is a 0.06 m value jump — the bump requires the restored code
    // baseline.  Wrong/missing v2 state would diverge here.
    ref.emitToStore(ep(t2, 0.30, /*with_g02=*/false), store_ref);
    ref.emitToStore(ep(t3, 0.36, /*with_g02=*/false), store_ref);
    b.emitToStore(ep(t2, 0.30, /*with_g02=*/false), store_b);
    b.emitToStore(ep(t3, 0.36, /*with_g02=*/false), store_b);

    // Identical counters on every channel...
    EXPECT_EQ(b.clockDiscontinuityCounter("G01"), ref.clockDiscontinuityCounter("G01"));
    EXPECT_EQ(b.clockDiscontinuityCounter("G02"), ref.clockDiscontinuityCounter("G02"));
    EXPECT_EQ(b.codeBiasDiscontinuityCounter("G01", "C1W"),
              ref.codeBiasDiscontinuityCounter("G01", "C1W"));
    EXPECT_EQ(b.codeBiasDiscontinuityCounter("G02", "C1W"),
              ref.codeBiasDiscontinuityCounter("G02", "C1W"));
    // ...and the expected values (so the parity above is not vacuously comparing
    // two wrong runs): clock = ep2 set-change survivor bump; code = ep1 value
    // jump + ep2 channel set-change + ep3 value jump; G02 frozen at its ep1 jump.
    EXPECT_EQ(b.clockDiscontinuityCounter("G01"), 1);
    EXPECT_EQ(b.codeBiasDiscontinuityCounter("G01", "C1W"), 3);
    EXPECT_EQ(b.codeBiasDiscontinuityCounter("G02", "C1W"), 1);
    EXPECT_EQ(b.clockDiscontinuityCounter("G02"), 0);

    // A DIFFERENT config on the new flags refuses the v2 blob (all-or-nothing;
    // the emitter stays untouched = the cold-start signal).
    {
        NetworkProductEmitter dst(0.05, true, 0.0);   // both new flags off
        std::istringstream is(full);
        EXPECT_FALSE(dst.restoreState(is));
        EXPECT_EQ(dst.codeBiasDiscontinuityCounter("G01", "C1W"), 0);   // untouched
        EXPECT_EQ(dst.clockDiscontinuityCounter("G01"), 0);
    }
    {
        // Partial mismatch (jump test matches, set-change does not) also fails.
        NetworkProductEmitter dst(0.05, true, 0.0, /*code_bias_jump_test=*/true,
                                  /*clock_disc_on_set_change=*/false);
        std::istringstream is(full);
        EXPECT_FALSE(dst.restoreState(is));
    }

    // Flags-off control: a default-channel emitter still serializes the v1
    // header — default-config snapshots stay byte-identical to pre-channel
    // builds (and old v1 snapshots restore into off-emitters).
    {
        NetworkProductEmitter off(0.05, true, 0.0);
        std::ostringstream v1blob;
        off.serializeState(v1blob);
        EXPECT_EQ(v1blob.str().rfind("EMITTER,v1,", 0), 0u);
    }
}

} // namespace
} // namespace GnssProcessor
