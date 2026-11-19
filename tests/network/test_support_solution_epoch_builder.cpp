/**
 * @file test_support_solution_epoch_builder.cpp
 * @brief Checks the shared solution-epoch builder against the real correction-stream writer
 *        and parser.
 *
 * Round-trip and response tests of the correction stream build their epochs with
 * tests/support/SolutionEpochBuilder.hpp.  These cases prove that what the builder sets is
 * what reaches the file and comes back from it: the default epoch, every field a later test
 * varies, the discontinuity counters through the builder's counter source, a satellite
 * without a clock, and epochs pushed through a real emitter first, as the producer does
 * before it writes.  A failure here means either the builder or the writer and parser are
 * wrong, and every test built on the builder would be reading the wrong thing.
 */

// The support header comes first so that this file proves it compiles on its own.
#include "../support/SolutionEpochBuilder.hpp"

#include <gtest/gtest.h>

#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkProductIO.hpp"
#include "products/SsrCorrectionStore.hpp"

#include "../support/TestTempDir.hpp"

#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

using gnss_test::SolutionEpochBuilder;

/// Writes each builder as one epoch of a fresh product file and returns the parsed records.
std::vector<NetworkProductRecord> roundTrip(const std::vector<const SolutionEpochBuilder*>& epochs,
                                            bool with_counters, bool* ok) {
    const std::string path = gnss_test::tempPath("product.txt").string();
    {
        NetworkProductWriter w(path);
        EXPECT_TRUE(w.good());
        for (const SolutionEpochBuilder* b : epochs) b->writeTo(w, with_counters);
    }
    return NetworkProductReader::read(path, ok);
}

// The default epoch (three satellites, two constellations, two code and two phase signals
// each) comes back field for field, with no optional record and no counter.
TEST(SupportSolutionEpochBuilder, DefaultEpochRoundTrips) {
    const SolutionEpochBuilder b;
    ASSERT_EQ(b.epoch().sats.size(), 3u);

    bool ok = false;
    const auto recs = roundTrip({&b}, /*with_counters=*/false, &ok);
    EXPECT_TRUE(ok);
    ASSERT_EQ(recs.size(), 1u);
    b.expectArrivedIn(recs[0], /*with_counters=*/false);

    // A few values spelled out, so the builder's documented defaults are pinned too.
    const NetworkSatSolution& e11 = recs[0].epoch.sats[2];
    EXPECT_EQ(e11.sat, "E11");
    EXPECT_EQ(e11.clock_c0_m, 3.0001);
    EXPECT_EQ(e11.code_bias_m.at("C5Q"), 0.78);
    EXPECT_EQ(e11.phase_bias.at("L1C").wide_lane_integer, 2);
    EXPECT_TRUE(e11.phase_bias.at("L1C").signal_integer);
    EXPECT_FALSE(e11.phase_bias.at("L5Q").signal_integer);
    EXPECT_EQ(recs[0].epoch.generation_id, 0u);
    EXPECT_FALSE(recs[0].datum_changed);
}

// Every field the builder can set reaches the parsed record: header, generation id and time,
// clocks and drift, URA, code and phase values, both integer flags, the hold generation, the
// satellite and code level steps, the datum-change constellations and the retro-marked
// satellites.
TEST(SupportSolutionEpochBuilder, EveryVariedFieldArrives) {
    SolutionEpochBuilder b;
    b.time(2314, 1234.25)
        .iodSsr(11).providerId(7).solutionId(9)
        .generationId(1398912345u)
        .generationTime(2314, 1240.125)
        .clock("G05", -1.0e-3).clockDrift("G05", 2.5e-5)
        .ura("E11", 0.031)
        .codeBias("G12", "C2W", -0.0078125)
        .phaseBias("E11", "L5Q", 0.30000000000000004)
        .signalInteger("G12", "L2W", true).wideLaneInteger("G12", "L2W", 3)
        .signalInteger("G05", "L1C", false).wideLaneInteger("G05", "L1C", 0)
        .holdGeneration("G05", "L2W", 4u)
        .levelStep("G12", 0.0125)
        .codeLevelStep("E11", "C5Q", -0.003)
        .datumChanged(true, {'E'})
        .retroSats({"G12"});

    bool ok = false;
    const auto recs = roundTrip({&b}, /*with_counters=*/false, &ok);
    EXPECT_TRUE(ok);
    ASSERT_EQ(recs.size(), 1u);
    b.expectArrivedIn(recs[0], /*with_counters=*/false);

    const NetworkProductRecord& r = recs[0];
    EXPECT_EQ(r.epoch.generation_id, 1398912345u);
    EXPECT_EQ(r.epoch.generation_time.tow, 1240.125);
    EXPECT_EQ(r.epoch.sats[0].clock_c1_m_s, 2.5e-5);
    EXPECT_EQ(r.epoch.sats[0].phase_bias.at("L2W").hold_generation, 4u);
    EXPECT_EQ(r.epoch.sats[1].level_step_m, 0.0125);
    EXPECT_EQ(r.epoch.sats[1].phase_bias.at("L2W").wide_lane_integer, 3);
    EXPECT_EQ(r.epoch.sats[2].cb_level_step_m.at("C5Q"), -0.003);
    EXPECT_TRUE(r.epoch.sats[2].has_ura);
    EXPECT_EQ(r.datum_consts, std::set<char>{'E'});
    EXPECT_EQ(r.retro_sats, std::set<std::string>{"G12"});
}

// The counters set on the builder are the ones the writer serializes through the counter
// source: each C, B and P record carries its own value, and an unset key carries 0.  The
// clock drift field the counter path always writes coexists with the clock counter.
TEST(SupportSolutionEpochBuilder, CountersArriveThroughTheCounterSource) {
    SolutionEpochBuilder b;
    b.clockDisc("G05", 2).clockDisc("E11", 1)
        .codeDisc("E11", "C5Q", 3)
        .phaseDisc("G05", "L1C", 5).phaseDisc("E11", "L5Q", 7)
        .clockDrift("E11", -4.0e-6);

    bool ok = false;
    const auto recs = roundTrip({&b}, /*with_counters=*/true, &ok);
    EXPECT_TRUE(ok);
    ASSERT_EQ(recs.size(), 1u);
    b.expectArrivedIn(recs[0], /*with_counters=*/true);

    const auto& sd = recs[0].serialized_disc;
    EXPECT_EQ(sd.clock.at("G05"), 2);
    EXPECT_EQ(sd.clock.at("G12"), 0);
    EXPECT_EQ(sd.code.at("E11|C5Q"), 3);
    EXPECT_EQ(sd.code.at("G05|C1C"), 0);
    EXPECT_EQ(sd.phase.at("E11|L5Q"), 7);
    EXPECT_EQ(sd.phase.at("G12|L2W"), 0);
    EXPECT_EQ(recs[0].epoch.sats[2].clock_c1_m_s, -4.0e-6);
}

// A satellite without a clock keeps its code and phase biases and their counters, writes no
// clock counter, and reads back without a clock.
TEST(SupportSolutionEpochBuilder, ClocklessSatelliteKeepsItsBiases) {
    SolutionEpochBuilder b;
    b.noClock("G12").phaseDisc("G12", "L1C", 2);

    bool ok = false;
    const auto recs = roundTrip({&b}, /*with_counters=*/true, &ok);
    EXPECT_TRUE(ok);
    ASSERT_EQ(recs.size(), 1u);
    b.expectArrivedIn(recs[0], /*with_counters=*/true);

    EXPECT_FALSE(recs[0].epoch.sats[1].has_clock);
    EXPECT_EQ(recs[0].serialized_disc.clock.count("G12"), 0u);
    EXPECT_EQ(recs[0].serialized_disc.phase.at("G12|L1C"), 2);
}

// The builder's epochs are valid emitter input: two epochs pushed through a real emitter, as
// the producer does before it writes, put the emitter's own counters on the file.  The second
// epoch steps G05 L1C by 1 m (above the 0.05 m default bar), so that key alone carries 1; a
// replay of the parsed file through a fresh emitter reaches the same counter.
TEST(SupportSolutionEpochBuilder, RealEmitterCountersArrive) {
    SolutionEpochBuilder first;
    SolutionEpochBuilder second;
    second.advance(30.0).phaseBias("G05", "L1C", 0.0421 + 1.0);

    const std::string path = gnss_test::tempPath("product.txt").string();
    {
        NetworkProductWriter  w(path);
        ASSERT_TRUE(w.good());
        NetworkProductEmitter emitter;
        SsrCorrectionStore    store;
        for (const SolutionEpochBuilder* b : {&first, &second}) {
            emitter.emitToStore(b->epoch(), store);
            w.write(b->epoch(), false, {}, {}, &emitter);
        }
    }

    bool ok = false;
    const auto recs = NetworkProductReader::read(path, &ok);
    EXPECT_TRUE(ok);
    ASSERT_EQ(recs.size(), 2u);
    gnss_test::expectSameSolutionEpoch(first.epoch(), recs[0].epoch);
    gnss_test::expectSameSolutionEpoch(second.epoch(), recs[1].epoch);

    for (const auto& [key, n] : recs[0].serialized_disc.phase) EXPECT_EQ(n, 0) << key;
    for (const auto& [key, n] : recs[1].serialized_disc.phase)
        EXPECT_EQ(n, key == "G05|L1C" ? 1 : 0) << key;
    for (const auto& [key, n] : recs[1].serialized_disc.code) EXPECT_EQ(n, 0) << key;
    EXPECT_EQ(recs[1].serialized_disc.phase.size(), 6u);

    NetworkProductEmitter replay_emitter;
    SsrCorrectionStore    replay_store;
    for (const auto& r : recs) NetworkProductReader::replay(r, replay_store, replay_emitter);
    EXPECT_EQ(replay_emitter.discontinuityCounter("G05", "L1C"), 1);
    EXPECT_EQ(replay_emitter.discontinuityCounter("G05", "L2W"), 0);
}

// The level mark reaches the replay: a satellite whose phase biases all move with a declared
// level step of the same size is not stepped, because the file carries the step and the
// replay emitter removes it before its jump test.  Without the mark the same move steps.
TEST(SupportSolutionEpochBuilder, LevelMarkReachesTheReplay) {
    SolutionEpochBuilder first;
    SolutionEpochBuilder marked;
    marked.advance(30.0)
        .phaseBias("G05", "L1C", 0.0421 + 1.0).phaseBias("G05", "L2W", -0.137 + 1.0)
        .levelStep("G05", 1.0);
    SolutionEpochBuilder unmarked = marked;
    unmarked.levelStep("G05", 0.0);

    for (const bool with_mark : {true, false}) {
        SCOPED_TRACE(with_mark ? "level step declared" : "no level step");
        bool ok = false;
        const auto recs = roundTrip({&first, with_mark ? &marked : &unmarked},
                                    /*with_counters=*/false, &ok);
        EXPECT_TRUE(ok);
        ASSERT_EQ(recs.size(), 2u);
        EXPECT_EQ(recs[1].epoch.sats[0].level_step_m, with_mark ? 1.0 : 0.0);

        NetworkProductEmitter emitter;
        SsrCorrectionStore    store;
        for (const auto& r : recs) NetworkProductReader::replay(r, store, emitter);
        EXPECT_EQ(emitter.discontinuityCounter("G05", "L1C"), with_mark ? 0 : 1);
        EXPECT_EQ(emitter.discontinuityCounter("G05", "L2W"), with_mark ? 0 : 1);
    }
}

}  // namespace
}  // namespace GnssProcessor
