/**
 * @file test_pb_hold_trip_generation.cpp
 * @brief `network.emit_pb_hold_trip_generation`: a satellite whose clock the anchor guard's test
 *        tripped at an epoch starts a new phase-bias hold generation on every key it has.
 *
 * Without the knob the hold keeps the satellite in its first generation, so the wire's
 * phase-visible clock never carries the step and the consumer has no declared event to
 * refloat on.  Armed, the tripped satellite's keys start a generation at that epoch, the edge a
 * gap opens: the estimate becomes the held value and the stamped generation advances.
 *
 * Covered:
 *   - armed, a key of a satellite in `clk_anchor.stepped` opens generation 2 at that epoch,
 *     holds the estimate of that epoch, counts in `n_pbh_gen_trip` (not in `n_pbh_gen_gap`),
 *     and every signal of the satellite does so while another satellite stays in generation 1;
 *   - the new generation then holds against a later drift, and a later epoch without the
 *     satellite in the set opens nothing;
 *   - knob off (the hold still armed), the same trip leaves the satellite in generation 1, holds
 *     its first value and counts nothing;
 *   - a key past the gap bar that is also tripped counts and reports as a gap, once;
 *   - a first sighting of a tripped satellite is a first generation, not a trip;
 *   - the loader parses the key, and the knob snapshot needs the hold as well;
 *   - under `network.emit_pb_hold_trip_generation_trigger = clock_event`, a satellite the
 *     guard's test tripped stays in its generation and counts in `n_pbh_trip_test_skipped`,
 *     while a satellite clock event opens generation 2 as before;
 *   - the trigger defaults to any_trip, and an unknown trigger is refused by the loader.
 */

#include <gtest/gtest.h>

#include "network/state/RunContext.hpp"
#include "config/GnssConfig.hpp"
#include "config/GnssConfigTypes.hpp"
#include "network/emit/PbHold.hpp"
#include "logging/LogManager.hpp"
#include "network/diag/RunTraces.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkProductIO.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include "../support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace fs = std::filesystem;

namespace GnssProcessor {
namespace {

std::shared_ptr<spdlog::logger> tripLogger() {
    static auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    return std::make_shared<spdlog::logger>("pbh_trip_test", sink);
}

/// G32 on two signals (the satellite under test) and G12 on one (the control), each with a
/// clock.  The phase biases of G32 are the quantities under test.
NetworkSolutionEpoch tripEpoch(double tow, double g32_l1, double g32_l2) {
    NetworkSolutionEpoch sol;
    sol.epoch_time = GnssTime{ 2313, tow };
    sol.iod_ssr = 3; sol.provider_id = 42; sol.solution_id = 1;
    NetworkSatSolution a;
    a.sat = "G32"; a.has_clock = true; a.clock_c0_m = 1.0;
    a.code_bias_m["C1W"] = 0.2;
    a.phase_bias["L1W"] = NetworkPhaseBias{ g32_l1, true, 1 };
    a.phase_bias["L2W"] = NetworkPhaseBias{ g32_l2, true, 1 };
    sol.sats.push_back(a);
    NetworkSatSolution b;
    b.sat = "G12"; b.has_clock = true; b.clock_c0_m = 1.0;
    b.code_bias_m["C1W"] = 0.2;
    b.phase_bias["L1W"] = NetworkPhaseBias{ 0.10, true, 1 };
    sol.sats.push_back(b);
    return sol;
}

double pbOf(const NetworkSolutionEpoch& sol, const std::string& sat, const std::string& sig) {
    for (const auto& s : sol.sats)
        if (s.sat == sat) return s.phase_bias.at(sig).bias_m;
    ADD_FAILURE() << "no phase bias for " << sat;
    return 0.0;
}

std::uint32_t genOf(const NetworkSolutionEpoch& sol, const std::string& sat, const std::string& sig) {
    for (const auto& s : sol.sats)
        if (s.sat == sat) return s.phase_bias.at(sig).hold_generation;
    ADD_FAILURE() << "no phase bias for " << sat;
    return 0;
}

/// A run context + traces + emitter, as networkEmitStore wires them.  The knobs are set through
/// the JSON loader because `ctx.knobs` is a snapshot.
struct TripHarness {
    LogManager                             log_mgr;
    GnssConfig                             cfg;
    fs::path                               cfg_path;
    std::unique_ptr<NetworkRunContext>     ctx;
    std::unique_ptr<NetworkRunTraces>      traces;
    std::unique_ptr<NetworkProductEmitter> emitter;
    SyncedEpoch                            se;
    bool                                   loaded = false;

    TripHarness(bool hold_on, bool trip_on, const char* tag, double max_gap_s = 300.0,
                const char* trigger = nullptr, bool expect_load = true) {
        const std::string trigger_line =
            trigger ? std::string("    \"emit_pb_hold_trip_generation_trigger\": \"") + trigger + "\",\n"
                    : std::string{};
        const std::string body = std::string("{\n") +
            "  \"processing\": {\n"
            "    \"mode\": \"ppp-rtk-network\",\n"
            "    \"active_systems\": [ \"GPS\" ],\n"
            "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
            "  },\n"
            "  \"network\": {\n"
            "    \"emit_pb_hold\": " + (hold_on ? "true" : "false") + ",\n"
            "    \"emit_pb_hold_trip_generation\": " + (trip_on ? "true" : "false") + ",\n"
            "    \"emit_pb_hold_max_gap_s\": " + std::to_string(max_gap_s) + ",\n" +
            trigger_line +
            "    \"stations\": [\n"
            "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
            "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
            "    ]\n"
            "  }\n"
            "}\n";
        cfg_path = gnss_test::tempDir() / ("pbh_trip_cfg_" + std::string(tag) + ".json");
        { std::ofstream f(cfg_path); f << body; }
        loaded = cfg.loadFromJson(cfg_path, log_mgr);
        EXPECT_EQ(loaded, expect_load);
        if (!loaded) return;
        EXPECT_EQ(cfg.getNetworkConfig().emit_pb_hold_trip_generation, trip_on);
        ctx = std::make_unique<NetworkRunContext>(cfg, cfg.getNetworkConfig());
        EXPECT_EQ(ctx->knobs.emit_pb_hold_trip_gen, hold_on && trip_on)
            << "the snapshot is the knob and the hold it acts in";
        const fs::path dir = gnss_test::tempDir() / ("pbh_trip_traces_" + std::string(tag));
        fs::create_directories(dir);
        traces = std::make_unique<NetworkRunTraces>(dir);
        emitter = std::make_unique<NetworkProductEmitter>(0.05, /*emit_clock_disc=*/false);
    }
    ~TripHarness() { std::error_code ec; fs::remove(cfg_path, ec); }

    /// The guard's trip set for the epoch about to be held, every satellite with one cause.
    void tripped(const std::set<std::string>& sats,
                 ClockStepCause cause = ClockStepCause::TestTrip) {
        ctx->carry.clk_anchor.stepped.clear();
        for (const auto& sat : sats) ctx->carry.clk_anchor.stepped[sat] = cause;
    }

    void step(NetworkSolutionEpoch& sol) {
        se.time = sol.epoch_time;
        networkPbHold(*ctx, cfg, tripLogger(), *traces, *emitter, se, sol);
    }
};

}  // namespace

TEST(PbHoldTripGeneration, ATrippedSatelliteOpensANewGenerationOnEveryKey) {
    TripHarness h(/*hold_on=*/true, /*trip_on=*/true, "on");
    auto seed = tripEpoch(259200.0, 0.10, 0.20);
    h.step(seed);
    ASSERT_EQ(genOf(seed, "G32", "L1W"), 1u);
    ASSERT_EQ(genOf(seed, "G32", "L2W"), 1u);

    // Epoch 1: the estimate drifts and the guard tripped G32.  Both of its keys start
    // generation 2 at the estimate of this epoch; G12 stays in generation 1 and is held.
    auto step = tripEpoch(259230.0, 0.60, 0.70);
    h.tripped({"G32"});
    h.step(step);
    EXPECT_EQ(genOf(step, "G32", "L1W"), 2u);
    EXPECT_EQ(genOf(step, "G32", "L2W"), 2u);
    EXPECT_DOUBLE_EQ(pbOf(step, "G32", "L1W"), 0.60) << "the generation's first epoch emits the estimate";
    EXPECT_DOUBLE_EQ(pbOf(step, "G32", "L2W"), 0.70);
    EXPECT_EQ(genOf(step, "G12", "L1W"), 1u);
    EXPECT_EQ(h.ctx->counters.n_pbh_gen_trip, 2) << "one per key";
    EXPECT_EQ(h.ctx->counters.n_pbh_gen_gap, 0) << "a trip is not counted as a gap";
    EXPECT_EQ(h.ctx->counters.n_pbh_gen_first, 3);

    // Epoch 2: no trip.  The new generation holds the value it started at.
    auto next = tripEpoch(259260.0, 0.95, 0.99);
    h.tripped({});
    h.step(next);
    EXPECT_EQ(genOf(next, "G32", "L1W"), 2u);
    EXPECT_DOUBLE_EQ(pbOf(next, "G32", "L1W"), 0.60);
    EXPECT_DOUBLE_EQ(pbOf(next, "G32", "L2W"), 0.70);
    EXPECT_EQ(h.ctx->counters.n_pbh_gen_trip, 2);
}

TEST(PbHoldTripGeneration, WithTheKnobOffTheSameTripLeavesTheFirstGeneration) {
    TripHarness h(/*hold_on=*/true, /*trip_on=*/false, "off");
    auto seed = tripEpoch(259200.0, 0.10, 0.20);
    h.step(seed);
    auto step = tripEpoch(259230.0, 0.60, 0.70);
    h.tripped({"G32"});
    h.step(step);
    EXPECT_EQ(genOf(step, "G32", "L1W"), 1u);
    EXPECT_DOUBLE_EQ(pbOf(step, "G32", "L1W"), 0.10) << "still the first value";
    EXPECT_EQ(h.ctx->counters.n_pbh_gen_trip, 0);
    EXPECT_EQ(h.ctx->counters.n_pbh_gen_gap, 0);
}

TEST(PbHoldTripGeneration, AKeyPastTheGapBarThatIsAlsoTrippedCountsAsAGapOnce) {
    TripHarness h(/*hold_on=*/true, /*trip_on=*/true, "gap", /*max_gap_s=*/300.0);
    auto seed = tripEpoch(259200.0, 0.10, 0.20);
    h.step(seed);
    auto late = tripEpoch(259200.0 + 330.0, 0.60, 0.70);
    h.tripped({"G32"});
    h.step(late);
    EXPECT_EQ(genOf(late, "G32", "L1W"), 2u) << "one generation, not two";
    EXPECT_EQ(h.ctx->counters.n_pbh_gen_gap, 3) << "all three keys crossed the gap";
    EXPECT_EQ(h.ctx->counters.n_pbh_gen_trip, 0);
}

TEST(PbHoldTripGeneration, AFirstSightingOfATrippedSatelliteIsAFirstGeneration) {
    TripHarness h(/*hold_on=*/true, /*trip_on=*/true, "first");
    auto sol = tripEpoch(259200.0, 0.33, 0.44);
    h.tripped({"G32"});
    h.step(sol);
    EXPECT_EQ(genOf(sol, "G32", "L1W"), 1u);
    EXPECT_EQ(h.ctx->counters.n_pbh_gen_first, 3);
    EXPECT_EQ(h.ctx->counters.n_pbh_gen_trip, 0);
}

TEST(PbHoldTripGeneration, TheKnobIsInertWithoutTheHold) {
    TripHarness h(/*hold_on=*/false, /*trip_on=*/true, "nohold");
    auto seed = tripEpoch(259200.0, 0.10, 0.20);
    h.step(seed);
    auto step = tripEpoch(259230.0, 0.60, 0.70);
    h.tripped({"G32"});
    h.step(step);
    EXPECT_FALSE(h.ctx->knobs.emit_pb_hold_trip_gen);
    EXPECT_EQ(genOf(step, "G32", "L1W"), 0u) << "no hold, no generation";
    EXPECT_DOUBLE_EQ(pbOf(step, "G32", "L1W"), 0.60);
    EXPECT_EQ(h.ctx->counters.n_pbh_gen_trip, 0);
}

TEST(PbHoldTripGeneration, TheClockEventTriggerHoldsATestTripAndCountsIt) {
    TripHarness h(/*hold_on=*/true, /*trip_on=*/true, "ce_test", 300.0, "clock_event");
    ASSERT_TRUE(h.loaded);
    EXPECT_EQ(h.cfg.getNetworkConfig().emit_pb_hold_trip_trigger, NetworkPbHoldTripTrigger::ClockEvent);
    EXPECT_TRUE(h.ctx->knobs.emit_pb_hold_trip_clock_event_only);
    auto seed = tripEpoch(259200.0, 0.10, 0.20);
    h.step(seed);
    auto step = tripEpoch(259230.0, 0.60, 0.70);
    h.tripped({"G32"}, ClockStepCause::TestTrip);
    h.step(step);
    EXPECT_EQ(genOf(step, "G32", "L1W"), 1u) << "a test trip opens no generation under clock_event";
    EXPECT_EQ(genOf(step, "G32", "L2W"), 1u);
    EXPECT_DOUBLE_EQ(pbOf(step, "G32", "L1W"), 0.10) << "still the first value";
    EXPECT_EQ(h.ctx->counters.n_pbh_gen_trip, 0);
    EXPECT_EQ(h.ctx->counters.n_pbh_trip_test_skipped, 2) << "one per held key";
}

TEST(PbHoldTripGeneration, TheClockEventTriggerOpensAGenerationOnAClockEvent) {
    TripHarness h(/*hold_on=*/true, /*trip_on=*/true, "ce_event", 300.0, "clock_event");
    ASSERT_TRUE(h.loaded);
    auto seed = tripEpoch(259200.0, 0.10, 0.20);
    h.step(seed);
    auto step = tripEpoch(259230.0, 0.60, 0.70);
    h.tripped({"G32"}, ClockStepCause::ClockEvent);
    h.step(step);
    EXPECT_EQ(genOf(step, "G32", "L1W"), 2u);
    EXPECT_EQ(genOf(step, "G32", "L2W"), 2u);
    EXPECT_DOUBLE_EQ(pbOf(step, "G32", "L1W"), 0.60);
    EXPECT_EQ(genOf(step, "G12", "L1W"), 1u);
    EXPECT_EQ(h.ctx->counters.n_pbh_gen_trip, 2);
    EXPECT_EQ(h.ctx->counters.n_pbh_trip_test_skipped, 0);
}

TEST(PbHoldTripGeneration, TheAnyTripTriggerIsTheDefaultAndOpensOnATestTrip) {
    TripHarness h(/*hold_on=*/true, /*trip_on=*/true, "any_default");
    ASSERT_TRUE(h.loaded);
    EXPECT_EQ(h.cfg.getNetworkConfig().emit_pb_hold_trip_trigger, NetworkPbHoldTripTrigger::AnyTrip);
    EXPECT_FALSE(h.ctx->knobs.emit_pb_hold_trip_clock_event_only);
    auto seed = tripEpoch(259200.0, 0.10, 0.20);
    h.step(seed);
    auto step = tripEpoch(259230.0, 0.60, 0.70);
    h.tripped({"G32"}, ClockStepCause::TestTrip);
    h.step(step);
    EXPECT_EQ(genOf(step, "G32", "L1W"), 2u);
    EXPECT_EQ(h.ctx->counters.n_pbh_gen_trip, 2);
    EXPECT_EQ(h.ctx->counters.n_pbh_trip_test_skipped, 0);
}

TEST(PbHoldTripGeneration, AnUnknownTriggerIsRefused) {
    TripHarness h(/*hold_on=*/true, /*trip_on=*/true, "bad_trigger", 300.0, "statistic",
                  /*expect_load=*/false);
    EXPECT_FALSE(h.loaded);
}

}  // namespace GnssProcessor
