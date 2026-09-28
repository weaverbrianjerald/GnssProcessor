/**
 * @file test_pb_emit_hold.cpp
 * @brief `network.emit_pb_hold`: the emitted phase bias of a (satellite, signal) is
 *        held at the first value of its generation, and a new generation is the
 *        declaration a consumer re-floats on (card PBH).
 *
 * Graded against truth on both beds, the within-arc phase-bias error (0.0227 m sim,
 * 0.0467 m field RMS) exceeds the clock's (0.0030 / 0.0274 m), the first differences
 * of the two errors are uncorrelated (+0.006), and the simulator's true bias is
 * constant in time.  The movement is therefore error rather than a frame transfer,
 * so the wire passes an identity (a held value plus a generation) rather than deltas.
 *
 * Covered here:
 *   - with the knob off, every emitted value and every entry is untouched and
 *     `hold_generation` is 0 (byte identity in miniature);
 *   - the held value stays constant while the estimator's value moves, and the
 *     deviation the hold hides is recorded on every held emission;
 *   - a first sighting starts generation 1 and emits the estimate itself;
 *   - a gap beyond `emit_pb_hold_max_gap_s` starts a new generation; a gap at or
 *     below it does not;
 *   - the hold runs before the emitter's value-jump test, so the test screens the
 *     held value: a key held across a large estimator step is not declared;
 *   - the stamp reaches `SsrPhaseBiasEntry::hold_generation` in the store;
 *   - the consumer ledger ignores a discontinuity-counter bump under a hold and
 *     re-floats on a generation change; a first sighting does neither;
 *   - the `gen=` P-record token and the `pbhold=1` S-line token round-trip, an off
 *     contract writes no token, and `checkContract` names a contract/payload
 *     disagreement.
 */

#include <gtest/gtest.h>

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "config/GnssConfigTypes.hpp"
#include "filter/NetworkPbHold.hpp"
#include "logging/LogManager.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkProductIO.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "realtime/PbHoldLedger.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace GnssProcessor {
namespace {

std::shared_ptr<spdlog::logger> pbhLogger() {
    static auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    return std::make_shared<spdlog::logger>("pbh_test", sink);
}

/// Two GPS satellites, one clock + one code bias + one phase bias each.  The
/// phase bias of G32 is the quantity under test; G12 is a constant control (held
/// as well when the knob is on) that gives the de-commoning clock-step median a
/// second satellite.
NetworkSolutionEpoch pbhEpoch(double tow, double g32_pb, double g32_clk = 1.0) {
    NetworkSolutionEpoch sol;
    sol.epoch_time = GnssTime{ 2313, tow };
    sol.iod_ssr = 3; sol.provider_id = 42; sol.solution_id = 1;
    NetworkSatSolution a;
    a.sat = "G32"; a.has_clock = true; a.clock_c0_m = g32_clk;
    a.code_bias_m["C1W"] = 0.2;
    a.phase_bias["L1W"] = NetworkPhaseBias{ g32_pb, true, 1 };
    sol.sats.push_back(a);
    NetworkSatSolution b;
    b.sat = "G12"; b.has_clock = true; b.clock_c0_m = 1.0;
    b.code_bias_m["C1W"] = 0.2;
    b.phase_bias["L1W"] = NetworkPhaseBias{ 0.10, true, 1 };
    sol.sats.push_back(b);
    return sol;
}

double pbOf(const NetworkSolutionEpoch& sol, const std::string& sat) {
    for (const auto& s : sol.sats)
        if (s.sat == sat) return s.phase_bias.at("L1W").bias_m;
    ADD_FAILURE() << "no phase bias for " << sat;
    return 0.0;
}

std::uint32_t genOf(const NetworkSolutionEpoch& sol, const std::string& sat) {
    for (const auto& s : sol.sats)
        if (s.sat == sat) return s.phase_bias.at("L1W").hold_generation;
    ADD_FAILURE() << "no phase bias for " << sat;
    return 0;
}

/// A run context + traces + emitter, wired the way networkEmitStore wires them.
///
/// `ctx.knobs` is a const snapshot, so the harness sets the knobs through the JSON
/// loader; every case below therefore also round-trips the JSON keys.
struct PbhHarness {
    LogManager                             log_mgr;
    GnssConfig                             cfg;
    fs::path                               cfg_path;
    std::unique_ptr<NetworkRunContext>     ctx;
    std::unique_ptr<NetworkRunTraces>      traces;
    std::unique_ptr<NetworkProductEmitter> emitter;
    SyncedEpoch                            se;

    PbhHarness(bool hold_on, double max_gap_s = 300.0, const char* tag = "d") {
        const std::string body = std::string("{\n") +
            "  \"processing\": {\n"
            "    \"mode\": \"ppp-rtk-network\",\n"
            "    \"active_systems\": [ \"GPS\" ],\n"
            "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
            "  },\n"
            "  \"network\": {\n"
            "    \"emit_pb_hold\": " + (hold_on ? "true" : "false") + ",\n"
            "    \"emit_pb_hold_max_gap_s\": " + std::to_string(max_gap_s) + ",\n"
            "    \"stations\": [\n"
            "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
            "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
            "    ]\n"
            "  }\n"
            "}\n";
        cfg_path = fs::temp_directory_path()
                   / ("pbh_cfg_" + std::string(tag) + (hold_on ? "_on" : "_off") + ".json");
        { std::ofstream f(cfg_path); f << body; }
        EXPECT_TRUE(cfg.loadFromJson(cfg_path, log_mgr));
        EXPECT_EQ(cfg.getNetworkConfig().emit_pb_hold, hold_on);
        EXPECT_DOUBLE_EQ(cfg.getNetworkConfig().emit_pb_hold_max_gap_s, max_gap_s);
        ctx = std::make_unique<NetworkRunContext>(cfg, cfg.getNetworkConfig());
        EXPECT_EQ(ctx->knobs.emit_pb_hold, hold_on) << "the knob must reach the run snapshot";
        const fs::path dir = fs::temp_directory_path() / "pbh_traces";
        fs::create_directories(dir);
        traces = std::make_unique<NetworkRunTraces>(dir);
        emitter = std::make_unique<NetworkProductEmitter>(0.05, /*emit_clock_disc=*/false);
    }
    ~PbhHarness() { std::error_code ec; fs::remove(cfg_path, ec); }

    /// Apply the hold to one epoch, then hand it to the emitter exactly as the
    /// producer does, so the emitter's jump test sees the held value.
    void step(NetworkSolutionEpoch& sol, SsrCorrectionStore& store) {
        se.time = sol.epoch_time;
        networkPbHold(*ctx, cfg, pbhLogger(), *traces, *emitter, se, sol);
        emitter->emitToStore(sol, store);
    }
};

// --- 1. The OFF path ------------------------------------------------------

TEST(PbEmitHoldTest, KnobOffLeavesEveryValueAndGenerationUntouched) {
    PbhHarness h(/*hold_on=*/false, 300.0, "off1");
    SsrCorrectionStore store;
    for (int k = 0; k < 4; ++k) {
        auto sol = pbhEpoch(259200.0 + 30.0 * k, 0.10 + 0.01 * k);
        const double raw = pbOf(sol, "G32");
        h.step(sol, store);
        EXPECT_DOUBLE_EQ(pbOf(sol, "G32"), raw) << "the OFF path must not touch the value";
        EXPECT_EQ(genOf(sol, "G32"), 0u) << "the OFF path must leave hold_generation 0";
    }
    EXPECT_EQ(h.ctx->counters.n_pbh_keys, 0);
    EXPECT_EQ(h.ctx->counters.n_pbh_held_emissions, 0);
    EXPECT_TRUE(h.ctx->carry.pb_hold.empty());
    EXPECT_TRUE(h.ctx->carry.pb_hold_dev_abs_m.empty());
    // And the entry the consumer receives carries no generation either.
    const auto pb = store.getPhaseBias("G32", "L1W");
    ASSERT_TRUE(pb.has_value());
    EXPECT_EQ(pb->hold_generation, 0u);
}

// --- 2. The hold itself ---------------------------------------------------

TEST(PbEmitHoldTest, HeldValueIsConstantWhileTheEstimateMoves) {
    PbhHarness h(/*hold_on=*/true, 300.0, "const");
    SsrCorrectionStore store;
    // Epoch 0 seeds the generation at the estimate; epochs 1..4 drift by 1 cm.
    auto seed = pbhEpoch(259200.0, 0.10);
    h.step(seed, store);
    EXPECT_DOUBLE_EQ(pbOf(seed, "G32"), 0.10) << "the generation's first epoch emits the estimate";
    EXPECT_EQ(genOf(seed, "G32"), 1u);

    for (int k = 1; k <= 4; ++k) {
        auto sol = pbhEpoch(259200.0 + 30.0 * k, 0.10 + 0.01 * k);
        h.step(sol, store);
        EXPECT_DOUBLE_EQ(pbOf(sol, "G32"), 0.10)
            << "epoch " << k << ": the wire must carry the HELD value, not the estimate";
        EXPECT_EQ(genOf(sol, "G32"), 1u) << "no gap, no first sighting: the generation holds";
    }
    // Both keys were held, and only the four drifted emissions are deviations.
    EXPECT_EQ(h.ctx->counters.n_pbh_keys, 2);          // G32 and G12
    EXPECT_EQ(h.ctx->counters.n_pbh_gen_first, 2);
    EXPECT_EQ(h.ctx->counters.n_pbh_gen_gap, 0);
    EXPECT_EQ(h.ctx->counters.n_pbh_held_emissions, 8);  // 4 epochs x 2 keys
    // G12 never moves, so its four deviations are exactly 0 and G32's are 1..4 cm.
    double max_dev = 0.0;
    for (double d : h.ctx->carry.pb_hold_dev_abs_m) max_dev = std::max(max_dev, d);
    EXPECT_NEAR(max_dev, 0.04, 1e-12) << "the distribution must measure what the hold HIDES";
    EXPECT_EQ(h.ctx->carry.pb_hold_dev_abs_m.size(), 8u);
    // And the consumer's entry carries the held value + the generation.
    const auto pb = store.getPhaseBias("G32", "L1W");
    ASSERT_TRUE(pb.has_value());
    EXPECT_DOUBLE_EQ(pb->bias_m, 0.10);
    EXPECT_EQ(pb->hold_generation, 1u);
}

TEST(PbEmitHoldTest, FirstSightingStartsGenerationOne) {
    PbhHarness h(/*hold_on=*/true, 300.0, "first");
    SsrCorrectionStore store;
    auto sol = pbhEpoch(259200.0, 0.33);
    h.step(sol, store);
    EXPECT_EQ(genOf(sol, "G32"), 1u);
    EXPECT_EQ(genOf(sol, "G12"), 1u);
    EXPECT_DOUBLE_EQ(pbOf(sol, "G32"), 0.33) << "nothing is hidden on the seeding epoch";
    EXPECT_EQ(h.ctx->counters.n_pbh_gen_first, 2);
    EXPECT_TRUE(h.ctx->carry.pb_hold_dev_abs_m.empty());
    ASSERT_EQ(h.ctx->carry.pb_hold.count("G32|L1W"), 1u);
    EXPECT_EQ(h.ctx->carry.pb_hold.at("G32|L1W").generation, 1u);
    EXPECT_DOUBLE_EQ(h.ctx->carry.pb_hold.at("G32|L1W").held_m, 0.33);
}

TEST(PbEmitHoldTest, AGapBeyondMaxGapStartsANewGenerationAndOneBelowDoesNot) {
    PbhHarness h(/*hold_on=*/true, /*max_gap_s=*/300.0, "gap");
    SsrCorrectionStore store;
    auto seed = pbhEpoch(259200.0, 0.10);
    h.step(seed, store);
    ASSERT_EQ(genOf(seed, "G32"), 1u);

    // A gap equal to the bar (300 s) is not beyond it, so the generation holds.
    auto at_bar = pbhEpoch(259200.0 + 300.0, 0.77);
    h.step(at_bar, store);
    EXPECT_EQ(genOf(at_bar, "G32"), 1u) << "a gap AT max_gap_s is a continuation";
    EXPECT_DOUBLE_EQ(pbOf(at_bar, "G32"), 0.10) << "and it is still held";
    EXPECT_EQ(h.ctx->counters.n_pbh_gen_gap, 0);

    // One epoch beyond it re-seeds at the current estimate.
    auto beyond = pbhEpoch(259200.0 + 300.0 + 330.0, 0.77);
    h.step(beyond, store);
    EXPECT_EQ(genOf(beyond, "G32"), 2u) << "a gap BEYOND max_gap_s starts a generation";
    EXPECT_DOUBLE_EQ(pbOf(beyond, "G32"), 0.77) << "which re-seeds from the current estimate";
    EXPECT_EQ(h.ctx->counters.n_pbh_gen_gap, 2);   // both keys crossed the same gap

    // And the new generation then holds against a fresh drift.
    auto after = pbhEpoch(259200.0 + 300.0 + 360.0, 0.99);
    h.step(after, store);
    EXPECT_EQ(genOf(after, "G32"), 2u);
    EXPECT_DOUBLE_EQ(pbOf(after, "G32"), 0.77);
}

TEST(PbEmitHoldTest, TheHoldRunsAheadOfTheEmitterValueJumpTest) {
    // Unheld, a 0.5 m estimator step under the 0.05 m bar is declared.  Held, the
    // emitter never sees it: the hold is the last writer of the emitted value, and
    // the jump test screens what the consumer receives.
    SsrCorrectionStore store;
    {
        PbhHarness off(/*hold_on=*/false, 300.0, "jt");
        auto a = pbhEpoch(259200.0, 0.10); off.step(a, store);
        auto b = pbhEpoch(259230.0, 0.60); off.step(b, store);
        EXPECT_EQ(off.emitter->discontinuityCounter("G32", "L1W"), 1)
            << "unheld, a 0.5 m step is a declared discontinuity";
    }
    {
        PbhHarness on(/*hold_on=*/true, 300.0, "jt");
        SsrCorrectionStore store2;
        auto a = pbhEpoch(259200.0, 0.10); on.step(a, store2);
        auto b = pbhEpoch(259230.0, 0.60); on.step(b, store2);
        EXPECT_EQ(on.emitter->discontinuityCounter("G32", "L1W"), 0)
            << "held, the step never reaches the wire, so nothing is declared";
        EXPECT_DOUBLE_EQ(pbOf(b, "G32"), 0.10);
    }
}

// --- 3. The consumer ledger ----------------------------------------------

TEST(PbEmitHoldTest, ConsumerIgnoresADiscCounterBumpUnderAHold) {
    PbHoldLedger led;
    EXPECT_EQ(led.react("G32|L1W", 1u, /*disc_changed=*/false).action,
              PbHoldLedger::Action::FirstSight);
    // Three epochs inside the generation, the middle one carrying a counter bump.
    EXPECT_EQ(led.react("G32|L1W", 1u, false).action, PbHoldLedger::Action::Held);
    const auto d = led.react("G32|L1W", 1u, /*disc_changed=*/true);
    EXPECT_EQ(d.action, PbHoldLedger::Action::Held) << "a bump under a hold carries no value step";
    EXPECT_EQ(led.react("G32|L1W", 1u, false).action, PbHoldLedger::Action::Held);
    const auto& c = led.counters();
    EXPECT_EQ(c.refloats, 0u) << "the counter must NOT re-float anything";
    EXPECT_EQ(c.disc_ignored, 1u) << "but the suppression must be counted (invariant #4)";
    EXPECT_EQ(c.cells, 4u);
    EXPECT_EQ(c.keys, 1u);
    EXPECT_TRUE(c.closureHolds());
}

TEST(PbEmitHoldTest, ConsumerRefloatsOnAGenerationChangeAndNotOnAFirstSighting) {
    PbHoldLedger led;
    EXPECT_EQ(led.react("G32|L1W", 3u, false).action, PbHoldLedger::Action::FirstSight)
        << "a first sighting has no prior state to have drifted";
    EXPECT_EQ(led.react("G32|L1W", 3u, false).action, PbHoldLedger::Action::Held);
    const auto d = led.react("G32|L1W", 4u, false);
    EXPECT_EQ(d.action, PbHoldLedger::Action::Refloat);
    EXPECT_EQ(d.previous_generation, 3u);
    EXPECT_EQ(led.react("G32|L1W", 4u, false).action, PbHoldLedger::Action::Held)
        << "the new generation is adopted, so it re-floats exactly once";
    // An unheld entry (generation 0) changes no counter, which keeps the off path
    // byte-identical.
    const auto n = led.react("G12|L1W", 0u, /*disc_changed=*/true);
    EXPECT_EQ(n.action, PbHoldLedger::Action::NotHeld);
    const auto& c = led.counters();
    EXPECT_EQ(c.cells, 4u);
    EXPECT_EQ(c.keys, 1u);
    EXPECT_EQ(c.refloats, 1u);
    EXPECT_EQ(c.first_sight, 1u);
    EXPECT_EQ(c.disc_ignored, 0u);
    EXPECT_TRUE(c.closureHolds());
    EXPECT_FALSE(led.recorded("G12|L1W").has_value());
    EXPECT_EQ(led.recorded("G32|L1W").value_or(0u), 4u);
}

// --- 4. The wire ----------------------------------------------------------

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::stringstream body; body << in.rdbuf();
    return body.str();
}

TEST(PbEmitHoldTest, PRecordGenTokenRoundTrips) {
    const fs::path p = fs::temp_directory_path() / "pbh_wire.txt";
    {
        NetworkProductWriter w(p.string());
        auto sol = pbhEpoch(259200.0, 0.10);
        for (auto& s : sol.sats)
            if (s.sat == "G32") s.phase_bias["L1W"].hold_generation = 7u;
        w.write(sol, /*datum_changed=*/false, {}, {}, nullptr);
    }
    const std::string body = slurp(p);
    EXPECT_NE(body.find(",gen=7"), std::string::npos) << "the token must be written";
    EXPECT_EQ(body.find("gen=0"), std::string::npos)
        << "an UNHELD record must emit no token at all (byte-identical OFF path)";
    EXPECT_NE(body.find("[,gen=<n>]"), std::string::npos)
        << "the legend must declare every field the writer can emit";

    bool ok = false;
    const auto recs = NetworkProductReader::read(p.string(), &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(recs.size(), 1u);
    bool saw32 = false, saw12 = false;
    for (const auto& s : recs[0].epoch.sats) {
        if (s.sat == "G32") { saw32 = true; EXPECT_EQ(s.phase_bias.at("L1W").hold_generation, 7u); }
        if (s.sat == "G12") { saw12 = true; EXPECT_EQ(s.phase_bias.at("L1W").hold_generation, 0u); }
    }
    EXPECT_TRUE(saw32);
    EXPECT_TRUE(saw12);
    fs::remove(p);
}

TEST(PbEmitHoldTest, SLinePbHoldTokenRoundTripsAndCheckContractNamesADisagreement) {
    const fs::path p = fs::temp_directory_path() / "pbh_contract.txt";
    SbasisContract c;
    c.present = true;
    c.apriori_product_id = "CODFIN2024129";
    c.emit_pb_hold = true;
    { NetworkProductWriter w(p.string(), c); }
    const std::string body = slurp(p);
    EXPECT_NE(body.find(",pbhold=1"), std::string::npos);
    EXPECT_NE(body.find("[,pbhold=1]"), std::string::npos)
        << "the legend must declare every field the writer can emit";

    bool ok = false;
    SbasisContract file_c;
    const auto recs = NetworkProductReader::read(p.string(), &file_c, &ok);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(recs.empty());
    EXPECT_TRUE(file_c.emit_pb_hold);
    // checkContract reports the contract/payload disagreement by field name.
    SbasisContract expected = file_c;
    expected.emit_pb_hold = false;
    std::vector<std::string> notes;
    EXPECT_FALSE(NetworkProductReader::checkContract(file_c, expected, &notes));
    bool named = false;
    for (const auto& n : notes) if (n.find("emit_pb_hold") != std::string::npos) named = true;
    EXPECT_TRUE(named) << "checkContract must name a pbhold= mismatch";
    fs::remove(p);

    // An off contract writes no pbhold= token, so its S line matches one written
    // before the knob existed.
    const fs::path q = fs::temp_directory_path() / "pbh_contract_off.txt";
    SbasisContract off = c;
    off.emit_pb_hold = false;
    { NetworkProductWriter w(q.string(), off); }
    std::string s_line;
    {
        std::istringstream ls(slurp(q));
        while (std::getline(ls, s_line))
            if (s_line.rfind("S,", 0) == 0) break;
    }
    ASSERT_EQ(s_line.rfind("S,", 0), 0u);
    EXPECT_EQ(s_line.find("pbhold="), std::string::npos);
    fs::remove(q);
}

}  // namespace
}  // namespace GnssProcessor
