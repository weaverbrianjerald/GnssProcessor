/**
 * @file test_pb_hold_phase_clock.cpp
 * @brief `network.emit_pb_hold_phase_clock` (card LAH): a held (satellite, signal)
 *        keeps the satellite's phase-visible clock c - p instead of its phase bias.
 *
 * The held emission is p(t0) + [c~(t) - c~(t0)], with c~ = c - M and M the accumulated
 * median emitted-clock step over the satellites present at consecutive emissions.
 * Covered here:
 *   - the loader requires network.emit_pb_hold and refuses network.clock_level_carry;
 *   - a level move of one satellite (its clock and phase bias together) leaves the
 *     wire's c - p at its generation-start value, where the plain hold moves it;
 *   - a clock move common to every satellite stays on the clock (the held phase bias
 *     does not move);
 *   - a satellite absent for a few epochs inside max_gap picks up its whole move
 *     since t0 on return; an epoch with a phase bias but no clock reuses the last
 *     transfer; a generation started without a clock is held plainly;
 *   - a new generation re-bases the reference;
 *   - pbHoldCommonClockStep takes the median over the satellites present at both
 *     emissions;
 *   - the S line writes pbhold=2, the reader recovers the mode, and checkContract
 *     names a disagreement.
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
#include "rinex/MultiRinexSync.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace GnssProcessor {
namespace {

std::shared_ptr<spdlog::logger> lahLogger() {
    static auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    return std::make_shared<spdlog::logger>("lah_test", sink);
}

/// One satellite of a test epoch: its emitted clock, its L1W phase bias, and whether
/// the clock is present.
struct LahSat {
    std::string sat;
    double      clk;
    double      pb;
    bool        has_clock{true};
};

NetworkSolutionEpoch lahEpoch(double tow, const std::vector<LahSat>& sats) {
    NetworkSolutionEpoch sol;
    sol.epoch_time = GnssTime{ 2313, tow };
    sol.iod_ssr = 3; sol.provider_id = 42; sol.solution_id = 1;
    for (const auto& x : sats) {
        NetworkSatSolution s;
        s.sat = x.sat; s.has_clock = x.has_clock; s.clock_c0_m = x.clk;
        s.code_bias_m["C1W"] = 0.2;
        s.phase_bias["L1W"] = NetworkPhaseBias{ x.pb, true, 1 };
        sol.sats.push_back(s);
    }
    return sol;
}

double pbOf(const NetworkSolutionEpoch& sol, const std::string& sat) {
    for (const auto& s : sol.sats)
        if (s.sat == sat) return s.phase_bias.at("L1W").bias_m;
    ADD_FAILURE() << "no phase bias for " << sat;
    return 0.0;
}

double clkOf(const NetworkSolutionEpoch& sol, const std::string& sat) {
    for (const auto& s : sol.sats)
        if (s.sat == sat) return s.clock_c0_m;
    ADD_FAILURE() << "no clock for " << sat;
    return 0.0;
}

std::uint32_t genOf(const NetworkSolutionEpoch& sol, const std::string& sat) {
    for (const auto& s : sol.sats)
        if (s.sat == sat) return s.phase_bias.at("L1W").hold_generation;
    ADD_FAILURE() << "no phase bias for " << sat;
    return 0;
}

std::string lahConfig(bool hold, bool phase_clock, bool clv, double max_gap_s) {
    return std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"emit_pb_hold\": " + (hold ? "true" : "false") + ",\n"
        "    \"emit_pb_hold_phase_clock\": " + (phase_clock ? "true" : "false") + ",\n"
        "    \"clock_level_carry\": " + (clv ? "true" : "false") + ",\n"
        "    \"emit_pb_hold_max_gap_s\": " + std::to_string(max_gap_s) + ",\n"
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

bool lahLoads(bool hold, bool phase_clock, bool clv, const char* tag) {
    LogManager log_mgr;
    GnssConfig cfg;
    const fs::path p = fs::temp_directory_path() / (std::string("lah_load_") + tag + ".json");
    { std::ofstream f(p); f << lahConfig(hold, phase_clock, clv, 300.0); }
    const bool ok = cfg.loadFromJson(p, log_mgr);
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

/// A run context + traces + emitter, wired the way networkEmitStore wires them, with
/// the knobs set through the JSON loader (ctx.knobs is a const snapshot).
struct LahHarness {
    LogManager                             log_mgr;
    GnssConfig                             cfg;
    fs::path                               cfg_path;
    std::unique_ptr<NetworkRunContext>     ctx;
    std::unique_ptr<NetworkRunTraces>      traces;
    std::unique_ptr<NetworkProductEmitter> emitter;
    SyncedEpoch                            se;

    LahHarness(bool phase_clock, const char* tag, double max_gap_s = 300.0) {
        cfg_path = fs::temp_directory_path()
                   / ("lah_cfg_" + std::string(tag) + (phase_clock ? "_on" : "_off") + ".json");
        { std::ofstream f(cfg_path); f << lahConfig(true, phase_clock, false, max_gap_s); }
        EXPECT_TRUE(cfg.loadFromJson(cfg_path, log_mgr));
        EXPECT_TRUE(cfg.getNetworkConfig().emit_pb_hold);
        EXPECT_EQ(cfg.getNetworkConfig().emit_pb_hold_phase_clock, phase_clock);
        ctx = std::make_unique<NetworkRunContext>(cfg, cfg.getNetworkConfig());
        EXPECT_EQ(ctx->knobs.emit_pb_hold_phase_clock, phase_clock)
            << "the knob must reach the run snapshot";
        const fs::path dir = fs::temp_directory_path() / "lah_traces";
        fs::create_directories(dir);
        traces = std::make_unique<NetworkRunTraces>(dir);
        emitter = std::make_unique<NetworkProductEmitter>(0.05, /*emit_clock_disc=*/false);
    }
    ~LahHarness() { std::error_code ec; fs::remove(cfg_path, ec); }

    void step(NetworkSolutionEpoch& sol, SsrCorrectionStore& store) {
        se.time = sol.epoch_time;
        networkPbHold(*ctx, cfg, lahLogger(), *traces, *emitter, se, sol);
        emitter->emitToStore(sol, store);
    }
};

std::string slurp(const fs::path& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// --- 1. The loader -----------------------------------------------------------------

TEST(PbHoldPhaseClockTest, LoaderRequiresTheHoldAndRefusesClockLevelCarry) {
    EXPECT_TRUE(lahLoads(true, true, false, "ok"));
    EXPECT_FALSE(lahLoads(false, true, false, "nohold"))
        << "the phase-clock mode is a mode of the hold";
    EXPECT_FALSE(lahLoads(true, true, true, "clv"))
        << "card CLV's transfer and the phase-clock mode both move the held phase bias";
    EXPECT_TRUE(lahLoads(true, false, true, "clv_off"))
        << "the refusal is the mode's: the plain hold with CLV still loads";
}

// --- 2. The level move --------------------------------------------------------------

TEST(PbHoldPhaseClockTest, ALevelMoveLeavesThePhaseVisibleClockHeld) {
    // G32's clock and phase bias rise together by 2 cm per epoch (a level move: phase
    // sees nothing); G12 and G05 stay put, so the median clock step is 0.
    for (const bool mode : { true, false }) {
        LahHarness h(mode, mode ? "lvl_on" : "lvl_off");
        SsrCorrectionStore store;
        double f0 = 0.0;
        for (int k = 0; k <= 5; ++k) {
            const double lvl = 0.02 * k;
            auto sol = lahEpoch(259200.0 + 30.0 * k, {
                { "G32", 1.00 + lvl, 0.30 + lvl },
                { "G12", -0.50, 0.10 },
                { "G05", 0.25, -0.20 } });
            h.step(sol, store);
            const double f = clkOf(sol, "G32") - pbOf(sol, "G32");
            if (k == 0) {
                f0 = f;
                EXPECT_DOUBLE_EQ(pbOf(sol, "G32"), 0.30) << "the generation's first epoch emits the estimate";
                continue;
            }
            if (mode) {
                EXPECT_NEAR(f, f0, 1e-12) << "epoch " << k << ": c - p must stay at its t0 value";
                EXPECT_NEAR(pbOf(sol, "G32"), 0.30 + lvl, 1e-12)
                    << "the held phase bias follows the level";
            } else {
                EXPECT_NEAR(f - f0, lvl, 1e-12) << "the plain hold puts the level on c - p";
                EXPECT_DOUBLE_EQ(pbOf(sol, "G32"), 0.30);
            }
            EXPECT_DOUBLE_EQ(pbOf(sol, "G12"), 0.10);
            EXPECT_EQ(genOf(sol, "G32"), 1u);
        }
        const auto& k = h.ctx->counters;
        if (mode) {
            EXPECT_EQ(k.n_pbh_frz_moved, 5) << "five held G32 emissions moved; G12 and G05 did not";
            EXPECT_EQ(k.n_pbh_frz_epochs, 5);
            EXPECT_NEAR(h.ctx->carry.pb_hold_frz_M.at('G'), 0.0, 1e-15);
            // What the hold hides is now the phase-visible wander only: here, nothing.
            for (double d : h.ctx->carry.pb_hold_dev_abs_m) EXPECT_NEAR(d, 0.0, 1e-12);
            EXPECT_EQ(h.ctx->carry.pb_hold_frz_step_abs_m.size(), 15u);   // 5 epochs x 3 keys
        } else {
            EXPECT_EQ(k.n_pbh_frz_moved, 0);
            EXPECT_EQ(k.n_pbh_frz_epochs, 0) << "the mode's state is untouched when it is off";
            EXPECT_TRUE(h.ctx->carry.pb_hold_frz_step_abs_m.empty());
        }
    }
}

TEST(PbHoldPhaseClockTest, ACommonClockMoveStaysOnTheClock) {
    // Every clock rises by 5 cm per epoch (a datum re-realization); the phase biases do
    // not move.  The median takes the whole move into M, so no held phase bias moves.
    LahHarness h(true, "common");
    SsrCorrectionStore store;
    for (int k = 0; k <= 4; ++k) {
        const double g = 0.05 * k;
        auto sol = lahEpoch(259200.0 + 30.0 * k, {
            { "G32", 1.00 + g, 0.30 }, { "G12", -0.50 + g, 0.10 }, { "G05", 0.25 + g, -0.20 } });
        h.step(sol, store);
        EXPECT_NEAR(pbOf(sol, "G32"), 0.30, 1e-12);
        EXPECT_NEAR(pbOf(sol, "G12"), 0.10, 1e-12);
        EXPECT_NEAR(pbOf(sol, "G05"), -0.20, 1e-12);
    }
    EXPECT_NEAR(h.ctx->carry.pb_hold_frz_M.at('G'), 0.20, 1e-12);
}

// --- 3. Gaps and missing clocks --------------------------------------------------------

TEST(PbHoldPhaseClockTest, ASatelliteBackInsideMaxGapPicksUpItsWholeMove) {
    LahHarness h(true, "gap");
    SsrCorrectionStore store;
    auto e0 = lahEpoch(259200.0, { { "G32", 1.00, 0.30 }, { "G12", -0.50, 0.10 }, { "G05", 0.25, -0.20 } });
    h.step(e0, store);
    // G32 is absent for two epochs, during which the others move by a common 1 cm per
    // epoch; on return its clock has risen by 7 cm, 2 cm of it common.
    for (int k = 1; k <= 2; ++k) {
        auto e = lahEpoch(259200.0 + 30.0 * k, {
            { "G12", -0.50 + 0.01 * k, 0.10 }, { "G05", 0.25 + 0.01 * k, -0.20 } });
        h.step(e, store);
    }
    auto e3 = lahEpoch(259290.0, {
        { "G32", 1.07, 0.30 }, { "G12", -0.47, 0.10 }, { "G05", 0.28, -0.20 } });
    h.step(e3, store);
    EXPECT_EQ(genOf(e3, "G32"), 1u) << "90 s is inside max_gap: the generation continues";
    // M = 0.01 + 0.01 + 0.01 = 0.03 (G32 is absent from the previous emission, so its
    // own 7 cm is not a step in the median), so c~ moved by 0.07 - 0.03 = 0.04 m and the
    // held phase bias with it.
    EXPECT_NEAR(h.ctx->carry.pb_hold_frz_M.at('G'), 0.03, 1e-12);
    EXPECT_NEAR(pbOf(e3, "G32"), 0.30 + 0.04, 1e-12);
}

TEST(PbHoldPhaseClockTest, AnEpochWithoutTheClockReusesTheLastTransfer) {
    LahHarness h(true, "noclk");
    SsrCorrectionStore store;
    auto e0 = lahEpoch(259200.0, { { "G32", 1.00, 0.30 }, { "G12", -0.50, 0.10 }, { "G05", 0.25, -0.20 } });
    h.step(e0, store);
    auto e1 = lahEpoch(259230.0, { { "G32", 1.03, 0.33 }, { "G12", -0.50, 0.10 }, { "G05", 0.25, -0.20 } });
    h.step(e1, store);
    EXPECT_NEAR(pbOf(e1, "G32"), 0.33, 1e-12);
    auto e2 = lahEpoch(259260.0, { { "G32", 9.99, 0.40, /*has_clock=*/false },
                                   { "G12", -0.50, 0.10 }, { "G05", 0.25, -0.20 } });
    h.step(e2, store);
    EXPECT_NEAR(pbOf(e2, "G32"), 0.33, 1e-12) << "no clock: the last transfer is reused";
    EXPECT_EQ(h.ctx->counters.n_pbh_frz_noclock, 1);
}

TEST(PbHoldPhaseClockTest, AGenerationStartedWithoutAClockIsHeldPlainly) {
    LahHarness h(true, "noref");
    SsrCorrectionStore store;
    auto e0 = lahEpoch(259200.0, { { "G32", 1.00, 0.30, /*has_clock=*/false },
                                   { "G12", -0.50, 0.10 }, { "G05", 0.25, -0.20 } });
    h.step(e0, store);
    auto e1 = lahEpoch(259230.0, { { "G32", 1.05, 0.35 }, { "G12", -0.50, 0.10 }, { "G05", 0.25, -0.20 } });
    h.step(e1, store);
    EXPECT_DOUBLE_EQ(pbOf(e1, "G32"), 0.30) << "no reference clock: the plain hold";
    EXPECT_EQ(h.ctx->counters.n_pbh_frz_noref, 1);
}

TEST(PbHoldPhaseClockTest, ANewGenerationRebasesTheReference) {
    LahHarness h(true, "regen", /*max_gap_s=*/60.0);
    SsrCorrectionStore store;
    auto e0 = lahEpoch(259200.0, { { "G32", 1.00, 0.30 }, { "G12", -0.50, 0.10 }, { "G05", 0.25, -0.20 } });
    h.step(e0, store);
    auto e1 = lahEpoch(259230.0, { { "G32", 1.02, 0.32 }, { "G12", -0.50, 0.10 }, { "G05", 0.25, -0.20 } });
    h.step(e1, store);
    EXPECT_NEAR(pbOf(e1, "G32"), 0.32, 1e-12);
    // G32 is absent for 90 s (beyond max_gap 60 s) and returns: a new generation emits
    // the estimate and re-bases the reference there.
    auto e2 = lahEpoch(259320.0, { { "G32", 1.50, 0.90 }, { "G12", -0.50, 0.10 }, { "G05", 0.25, -0.20 } });
    h.step(e2, store);
    EXPECT_EQ(genOf(e2, "G32"), 2u);
    EXPECT_DOUBLE_EQ(pbOf(e2, "G32"), 0.90);
    auto e3 = lahEpoch(259350.0, { { "G32", 1.51, 0.91 }, { "G12", -0.50, 0.10 }, { "G05", 0.25, -0.20 } });
    h.step(e3, store);
    EXPECT_NEAR(pbOf(e3, "G32"), 0.91, 1e-12);
    EXPECT_NEAR(clkOf(e3, "G32") - pbOf(e3, "G32"), 1.50 - 0.90, 1e-12);
}

// --- 4. The common step helper ---------------------------------------------------------

TEST(PbHoldPhaseClockTest, CommonClockStepIsTheMedianOverAConstellationsSatellitesAtBothEmissions) {
    const std::map<std::string, double> prev{ { "G01", 1.0 }, { "G02", 2.0 }, { "G03", 3.0 },
                                              { "G09", 9.0 }, { "E05", 5.0 } };
    double med = -1.0;
    // Steps 0.1, 0.3, 0.2 (G04 has no previous clock and is left out; E05's 4 m step is
    // another constellation's): median 0.2.
    auto a = lahEpoch(0.0, { { "G01", 1.1, 0.0 }, { "G02", 2.3, 0.0 }, { "G03", 3.2, 0.0 },
                             { "G04", 7.0, 0.0 }, { "E05", 9.0, 0.0 } });
    ASSERT_TRUE(pbHoldCommonClockStep(a, prev, 'G', med));
    EXPECT_NEAR(med, 0.2, 1e-12);
    ASSERT_TRUE(pbHoldCommonClockStep(a, prev, 'E', med));
    EXPECT_NEAR(med, 4.0, 1e-12);
    // An even count takes the mean of the two middle steps: (0.1 + 0.3) / 2.
    auto b = lahEpoch(0.0, { { "G01", 1.1, 0.0 }, { "G02", 2.3, 0.0 } });
    ASSERT_TRUE(pbHoldCommonClockStep(b, prev, 'G', med));
    EXPECT_NEAR(med, 0.2, 1e-12);
    // A satellite without a clock is not a step; nor is a constellation with no satellite.
    auto c = lahEpoch(0.0, { { "G01", 5.0, 0.0, /*has_clock=*/false }, { "G04", 7.0, 0.0 } });
    med = -1.0;
    EXPECT_FALSE(pbHoldCommonClockStep(c, prev, 'G', med));
    EXPECT_FALSE(pbHoldCommonClockStep(a, prev, 'C', med));
    EXPECT_DOUBLE_EQ(med, -1.0) << "the out value is untouched when there is no step";
}

TEST(PbHoldPhaseClockTest, AConstellationsCommonMoveStaysOnItsClocks) {
    // Three GPS and three Galileo satellites.  At epoch 1 every Galileo clock moves by
    // +0.5 m (its own frame re-anchor or pivot handover) and no GPS clock moves: a
    // single cross-constellation median would read 0 and push +0.5 m into every held
    // Galileo phase bias.  Per constellation, nothing is held differently.
    LahHarness h(true, "gal");
    SsrCorrectionStore store;
    for (int k = 0; k <= 2; ++k) {
        const double d = (k >= 1) ? 0.5 : 0.0;
        auto sol = lahEpoch(259200.0 + 30.0 * k, {
            { "G32", 1.00, 0.30 }, { "G12", -0.50, 0.10 }, { "G05", 0.25, -0.20 },
            { "E11", 2.00 + d, 0.40 }, { "E12", -1.00 + d, 0.50 }, { "E24", 0.70 + d, -0.60 } });
        h.step(sol, store);
        EXPECT_NEAR(pbOf(sol, "E11"), 0.40, 1e-12) << "epoch " << k;
        EXPECT_NEAR(pbOf(sol, "E24"), -0.60, 1e-12) << "epoch " << k;
        EXPECT_NEAR(pbOf(sol, "G32"), 0.30, 1e-12) << "epoch " << k;
    }
    EXPECT_NEAR(h.ctx->carry.pb_hold_frz_M.at('E'), 0.5, 1e-12);
    EXPECT_NEAR(h.ctx->carry.pb_hold_frz_M.at('G'), 0.0, 1e-15);
}

TEST(PbHoldPhaseClockTest, AnEmittedEpochWithoutSatellitesBreaksTheStep) {
    // An emitted epoch whose satellites were all pruned is still an emission, so the
    // next epoch has no previous clocks and M does not move: a common 20 cm move across
    // the empty epoch reaches the held phase biases as each satellite's own move.
    LahHarness h(true, "empty");
    SsrCorrectionStore store;
    auto e0 = lahEpoch(259200.0, { { "G32", 1.00, 0.30 }, { "G12", -0.50, 0.10 }, { "G05", 0.25, -0.20 } });
    h.step(e0, store);
    auto e1 = lahEpoch(259230.0, {});
    h.step(e1, store);
    auto e2 = lahEpoch(259260.0, { { "G32", 1.20, 0.30 }, { "G12", -0.30, 0.10 }, { "G05", 0.45, -0.20 } });
    h.step(e2, store);
    EXPECT_TRUE(h.ctx->carry.pb_hold_frz_M.empty() || h.ctx->carry.pb_hold_frz_M.at('G') == 0.0);
    EXPECT_NEAR(pbOf(e2, "G32"), 0.50, 1e-12);
    EXPECT_NEAR(pbOf(e2, "G12"), 0.30, 1e-12);
    // And the step after it is again against its own previous emission.
    auto e3 = lahEpoch(259290.0, { { "G32", 1.40, 0.30 }, { "G12", -0.10, 0.10 }, { "G05", 0.65, -0.20 } });
    h.step(e3, store);
    EXPECT_NEAR(h.ctx->carry.pb_hold_frz_M.at('G'), 0.20, 1e-12);
    EXPECT_NEAR(pbOf(e3, "G32"), 0.50, 1e-12);
}

TEST(PbHoldPhaseClockTest, ACoupledMarkIsCountedAsTransferred) {
    LahHarness h(true, "coupled");
    SsrCorrectionStore store;
    auto e0 = lahEpoch(259200.0, { { "G32", 1.00, 0.30 }, { "G12", -0.50, 0.10 }, { "G05", 0.25, -0.20 } });
    h.step(e0, store);
    // A mark naming G32's clock and phase bias together, then the epoch it lands on.
    h.emitter->markSatDiscontinuity({ "G32" }, NetworkProductEmitter::kDiscChanClock
                                                   | NetworkProductEmitter::kDiscChanPhaseBias);
    auto e1 = lahEpoch(259230.0, { { "G32", 1.40, 0.70 }, { "G12", -0.50, 0.10 }, { "G05", 0.25, -0.20 } });
    h.step(e1, store);
    EXPECT_NEAR(pbOf(e1, "G32"), 0.70, 1e-12) << "the coupled move reaches the held phase bias";
    EXPECT_EQ(h.ctx->counters.n_pbh_frz_coupled, 1);
    EXPECT_EQ(h.ctx->counters.n_pbh_coupled_marks, 0) << "not booked as a move the hold loses";
    EXPECT_TRUE(h.ctx->carry.pb_hold_coupled_step_m.empty());
}

TEST(PbHoldPhaseClockTest, ANewGenerationResetsTheLastTransferBeforeAMissingClock) {
    LahHarness h(true, "regen_noclk", /*max_gap_s=*/60.0);
    SsrCorrectionStore store;
    auto e0 = lahEpoch(259200.0, { { "G32", 1.00, 0.30 }, { "G12", -0.50, 0.10 }, { "G05", 0.25, -0.20 } });
    h.step(e0, store);
    auto e1 = lahEpoch(259230.0, { { "G32", 1.10, 0.40 }, { "G12", -0.50, 0.10 }, { "G05", 0.25, -0.20 } });
    h.step(e1, store);
    EXPECT_NEAR(pbOf(e1, "G32"), 0.40, 1e-12);
    // Beyond max_gap: a new generation, then an epoch without G32's clock.  The last
    // transfer is the new generation's (0), not the old one's (0.10).
    auto e2 = lahEpoch(259320.0, { { "G32", 2.00, 0.90 }, { "G12", -0.50, 0.10 }, { "G05", 0.25, -0.20 } });
    h.step(e2, store);
    auto e3 = lahEpoch(259350.0, { { "G32", 9.99, 0.95, /*has_clock=*/false },
                                   { "G12", -0.50, 0.10 }, { "G05", 0.25, -0.20 } });
    h.step(e3, store);
    EXPECT_EQ(genOf(e3, "G32"), 2u);
    EXPECT_NEAR(pbOf(e3, "G32"), 0.90, 1e-12);
    EXPECT_EQ(h.ctx->counters.n_pbh_frz_noclock, 1);
}

// --- 5. The contract -------------------------------------------------------------------

TEST(PbHoldPhaseClockTest, SLineWritesPbhold2AndCheckContractNamesADisagreement) {
    const fs::path p = fs::temp_directory_path() / "lah_contract.txt";
    SbasisContract c;
    c.present = true;
    c.apriori_product_id = "CODFIN2024129";
    c.emit_pb_hold = true;
    c.emit_pb_hold_phase_clock = true;
    { NetworkProductWriter w(p.string(), c); }
    std::string s_line;
    {
        std::istringstream ls(slurp(p));
        while (std::getline(ls, s_line))
            if (s_line.rfind("S,", 0) == 0) break;
    }
    ASSERT_EQ(s_line.rfind("S,", 0), 0u);
    EXPECT_NE(s_line.find(",pbhold=2"), std::string::npos);
    EXPECT_EQ(s_line.find(",pbhold=1"), std::string::npos);

    bool ok = false;
    SbasisContract file_c;
    NetworkProductReader::read(p.string(), &file_c, &ok);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(file_c.emit_pb_hold) << "pbhold=2 is still a hold to a reader that tests != 0";
    EXPECT_TRUE(file_c.emit_pb_hold_phase_clock);
    SbasisContract expected = file_c;
    expected.emit_pb_hold_phase_clock = false;
    std::vector<std::string> notes;
    EXPECT_FALSE(NetworkProductReader::checkContract(file_c, expected, &notes));
    bool named = false;
    for (const auto& n : notes)
        if (n.find("emit_pb_hold_phase_clock") != std::string::npos) named = true;
    EXPECT_TRUE(named);
    fs::remove(p);

    // The plain hold still writes pbhold=1 and reads back without the mode.
    const fs::path q = fs::temp_directory_path() / "lah_contract_plain.txt";
    SbasisContract plain = c;
    plain.emit_pb_hold_phase_clock = false;
    { NetworkProductWriter w(q.string(), plain); }
    const std::string body = slurp(q);
    EXPECT_NE(body.find(",pbhold=1"), std::string::npos);
    SbasisContract plain_back;
    NetworkProductReader::read(q.string(), &plain_back, &ok);
    EXPECT_TRUE(plain_back.emit_pb_hold);
    EXPECT_FALSE(plain_back.emit_pb_hold_phase_clock);
    fs::remove(q);
}

}  // namespace
}  // namespace GnssProcessor
