/**
 * @file test_network_phase_frame_level_vote.cpp
 * @brief The phase-visible frame's vote in the phase-bias hold's level
 *        (`network.phase_frame_hold_level_vote`): the sample with the offset -M, the step floor
 *        of `phaseFrameHoldEpoch`, the frame on synthetic streams of the two holds, and the
 *        loader.
 *
 * Each satellite votes on w = C - P_IF - M, M the hold's accumulated median step of its
 * constellation's emitted clocks.  The phase-clock hold writes each satellite's de-commoned clock
 * step c~ = C - M into its held phase biases, so w is constant within a generation; the smoothed
 * hold writes the same transfer minus the clone's passed motion Phi, so w moves with Phi.
 *
 * Covered:
 *   - the sample with the offset -M is C - P_IF - M;
 *   - a median step of at most the floor is not applied (G stays exactly 0, the step counted as
 *     floored); a larger one is; without a floor every step applies;
 *   - the phase-clock hold's stream: the frame stays exactly 0 over many epochs;
 *   - the smoothed hold's stream: the frame follows the common mode of Phi, so the phase-visible
 *     clock with the frame follows M;
 *   - the loader: off by default, armed with the frame and the phase-clock or the smoothed hold,
 *     refused without the frame and without either hold.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "network/emit/PhaseFrameHold.hpp"
#include "products/NetworkProductEmitter.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include "../support/TestTempDir.hpp"

using namespace GnssProcessor;

namespace {

constexpr double kF1 = 1575.42e6, kF2 = 1227.60e6;

double ifBias(double p1, double p2) {
    const double a = kF1 * kF1 / (kF1 * kF1 - kF2 * kF2);
    return a * p1 - (a - 1.0) * p2;
}

NetworkSatSolution gpsSat(const std::string& id, double clock, double p1, double p2,
                          std::uint32_t gen = 1) {
    NetworkSatSolution s;
    s.sat        = id;
    s.has_clock  = true;
    s.clock_c0_m = clock;
    NetworkPhaseBias b1, b2;
    b1.bias_m = p1;
    b2.bias_m = p2;
    b1.hold_generation = gen;
    b2.hold_generation = gen;
    s.phase_bias["L1W"] = b1;
    s.phase_bias["L2W"] = b2;
    return s;
}

PhaseFrameSample smp(double v, double t) {
    PhaseFrameSample s;
    s.pair = "L1W+L2W";
    s.gen1 = 1;
    s.gen2 = 1;
    s.v_m  = v;
    s.t_s  = t;
    return s;
}

PhaseFrameEntry ent(const std::string& id, const PhaseFrameSample& s) {
    PhaseFrameEntry e;
    e.sat = id;
    e.now = s;
    return e;
}

/// The median of a copy (the mean of the two middle values for an even count), as the hold takes it.
double medianOf(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return (n % 2 == 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/**
 * A synthetic hold over @p epochs epochs of five GPS satellites: clocks C_s(t) with a common walk
 * and each satellite's own motion, M accumulated as the phase-bias hold accumulates it, each
 * satellite's phase biases written as held + (c~ - c~_ref) - phi_s(t), c~ = C - M.  phi = 0 is the
 * phase-clock hold; phi != 0 the smoothed hold's passed motion.  Runs the frame in the hold's level
 * and returns G per epoch (0 before the frame starts) and, for satellite G01, the phase-visible
 * clock with the frame, C - (P_IF + G).
 */
struct HoldRun {
    std::vector<double> g;
    std::vector<double> pv_g01;
    std::vector<double> m;
    std::vector<bool>   floored;
};

HoldRun runHold(int epochs, double (*phi)(int k, int i)) {
    const std::vector<std::string> ids{"G01", "G02", "G03", "G04", "G05"};
    const std::vector<double> h1{12.345678901, -7.25, 33.125, 0.5, -18.75};
    const std::vector<double> h2{-3.5, 21.0625, -9.75, 14.2, 6.125};
    HoldRun out;
    std::vector<double> c_prev, cref;
    double M = 0.0;
    std::map<std::string, PhaseFrameSample> prev;
    std::map<char, double> frame_g;
    PhaseFrameConfig cfg;
    cfg.step_floor_m = kPhaseFrameLevelVoteFloorM;
    for (int k = 0; k < epochs; ++k) {
        std::vector<double> c(ids.size());
        for (std::size_t i = 0; i < ids.size(); ++i)
            c[i] = 100.0 + 0.0123 * k + 0.001 * std::sin(0.7 * k + static_cast<double>(i))
                   + 0.37 * static_cast<double>(i);
        if (k > 0) {
            std::vector<double> d(ids.size());
            for (std::size_t i = 0; i < ids.size(); ++i) d[i] = c[i] - c_prev[i];
            M += medianOf(d);
        }
        if (k == 0) {
            cref.resize(ids.size());
            for (std::size_t i = 0; i < ids.size(); ++i) cref[i] = c[i] - M;
        }
        std::vector<PhaseFrameEntry> entries;
        std::vector<NetworkSatSolution> sats;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const double xfer = (c[i] - M) - cref[i] - phi(k, static_cast<int>(i));
            sats.push_back(gpsSat(ids[i], c[i], h1[i] + xfer, h2[i] + xfer));
            PhaseFrameEntry e;
            e.sat = ids[i];
            e.now = phaseFrameSample(sats.back(), -M, 30.0 * k);
            entries.push_back(e);
        }
        const auto steps = phaseFrameHoldEpoch(entries, 30.0 * k, prev, frame_g, cfg);
        const double g = frame_g.count('G') ? frame_g.at('G') : 0.0;
        out.g.push_back(g);
        out.m.push_back(M);
        out.floored.push_back(steps.count('G') && steps.at('G').floored);
        const auto& p = sats[0].phase_bias;
        out.pv_g01.push_back(c[0] - (ifBias(p.at("L1W").bias_m, p.at("L2W").bias_m) + g));
        c_prev = c;
    }
    return out;
}

double noPhi(int, int) { return 0.0; }
/// A passed motion common to the constellation (0.70 m over 60 epochs, inside the 1 m leash) plus
/// one satellite's own (G04), which the median leaves out.
double commonPhi(int k, int i) { return 0.0002 * k * k + (i == 3 ? 0.002 * k : 0.0); }

}  // namespace

TEST(PhaseFrameLevelVote, TheSampleWithMinusMIsTheCorrectionInTheHoldsLevel) {
    const NetworkSatSolution s = gpsSat("G07", 1.25, 0.40, -0.30);
    const auto v = phaseFrameSample(s, -0.75, 60.0);
    ASSERT_TRUE(v.has_value());
    EXPECT_NEAR(v->v_m, 1.25 - ifBias(0.40, -0.30) - 0.75, 1e-12);
    EXPECT_EQ(v->pair, "L1W+L2W");
}

TEST(PhaseFrameLevelVote, AStepAtMostTheFloorIsNotApplied) {
    std::map<std::string, PhaseFrameSample> prev;
    std::map<char, double> g;
    PhaseFrameConfig cfg;
    cfg.step_floor_m = kPhaseFrameLevelVoteFloorM;
    std::vector<PhaseFrameEntry> e0, e1;
    for (const char* id : {"G01", "G02", "G03", "G04", "G05"}) {
        e0.push_back(ent(id, smp(1.0, 0.0)));
        e1.push_back(ent(id, smp(1.0 + 4e-13, 30.0)));
    }
    phaseFrameHoldEpoch(e0, 0.0, prev, g, cfg);
    const auto st = phaseFrameHoldEpoch(e1, 30.0, prev, g, cfg).at('G');
    EXPECT_TRUE(st.moved);
    EXPECT_TRUE(st.floored);
    EXPECT_TRUE(st.started);
    EXPECT_EQ(st.step_m, 0.0);
    EXPECT_EQ(g.at('G'), 0.0);   // exactly: nothing is shifted
}

TEST(PhaseFrameLevelVote, AStepAboveTheFloorApplies) {
    std::map<std::string, PhaseFrameSample> prev;
    std::map<char, double> g;
    PhaseFrameConfig cfg;
    cfg.step_floor_m = kPhaseFrameLevelVoteFloorM;
    std::vector<PhaseFrameEntry> e0, e1;
    for (const char* id : {"G01", "G02", "G03", "G04", "G05"}) {
        e0.push_back(ent(id, smp(1.0, 0.0)));
        e1.push_back(ent(id, smp(1.0 + 2e-6, 30.0)));
    }
    phaseFrameHoldEpoch(e0, 0.0, prev, g, cfg);
    const auto st = phaseFrameHoldEpoch(e1, 30.0, prev, g, cfg).at('G');
    EXPECT_FALSE(st.floored);
    EXPECT_NEAR(st.step_m, 2e-6, 1e-15);
    EXPECT_NEAR(g.at('G'), 2e-6, 1e-15);
}

TEST(PhaseFrameLevelVote, WithoutAFloorEveryStepApplies) {
    std::map<std::string, PhaseFrameSample> prev;
    std::map<char, double> g;
    const PhaseFrameConfig cfg;   // the vote against the raw a priori: no floor
    std::vector<PhaseFrameEntry> e0, e1;
    for (const char* id : {"G01", "G02", "G03", "G04", "G05"}) {
        e0.push_back(ent(id, smp(1.0, 0.0)));
        e1.push_back(ent(id, smp(1.0 + 4e-13, 30.0)));
    }
    phaseFrameHoldEpoch(e0, 0.0, prev, g, cfg);
    const auto st = phaseFrameHoldEpoch(e1, 30.0, prev, g, cfg).at('G');
    EXPECT_FALSE(st.floored);
    EXPECT_NE(g.at('G'), 0.0);
}

TEST(PhaseFrameLevelVote, ThePhaseClockHoldLeavesTheFrameAtExactlyZero) {
    const HoldRun r = runHold(240, &noPhi);
    for (std::size_t k = 0; k < r.g.size(); ++k) EXPECT_EQ(r.g[k], 0.0) << "epoch " << k;
    // Every voted epoch was the hold's rounding, and the phase-visible clock follows M exactly
    // as the hold writes it (a constant offset from M).
    for (std::size_t k = 2; k < r.g.size(); ++k) {
        EXPECT_TRUE(r.floored[k]) << "epoch " << k;
        EXPECT_NEAR(r.pv_g01[k] - r.m[k], r.pv_g01[1] - r.m[1], 1e-9);
    }
}

TEST(PhaseFrameLevelVote, TheSmoothedHoldsCommonPassedMotionIsAbsorbed) {
    const HoldRun r = runHold(60, &commonPhi);
    // G moves by the median of the satellites' Phi increments, the common one; the phase-visible
    // clock with the frame then keeps a constant offset from M, the code side's level.
    double worst = 0.0;
    for (std::size_t k = 1; k < r.g.size(); ++k)
        worst = std::max(worst, std::abs((r.pv_g01[k] - r.m[k]) - (r.pv_g01[1] - r.m[1])));
    EXPECT_LT(worst, 1e-9);
    // Without the frame the same clock would drift by the common Phi (0.70 m over the run).
    const double phi_drift = commonPhi(59, 0) - commonPhi(1, 0);
    EXPECT_GT(std::abs(phi_drift), 0.5);
    EXPECT_NEAR(r.g.back() - r.g[1], phi_drift, 1e-9);
    for (std::size_t k = 1; k < r.floored.size(); ++k) EXPECT_FALSE(r.floored[k]) << "epoch " << k;
}

namespace {

namespace fs = std::filesystem;

std::string voteConfig(const std::string& network_extra) {
    return std::string("{\n")
        + "  \"processing\": {\n"
          "    \"mode\": \"ppp-rtk-network\",\n"
          "    \"active_systems\": [ \"GPS\" ],\n"
          "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
          "  },\n"
          "  \"network\": {\n"
          "    \"clock_datum_station\": \"AAAA\",\n"
          + network_extra +
          "    \"stations\": [\n"
          "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
          "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
          "    ]\n"
          "  }\n"
          "}\n";
}

bool loadVoteConfig(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = gnss_test::tempDir() / ("phase_frame_level_vote_cfg_" + tag + ".json");
    { std::ofstream f(p); f << body; }
    bool ok = false;
    try {
        ok = cfg.loadFromJson(p, log);
    } catch (const std::exception&) {
        ok = false;
    }
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

}  // namespace

TEST(PhaseFrameLevelVote, ConfigDefaultsOff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadVoteConfig("default", voteConfig("    \"emit_pb_hold\": true,\n"
                                                      "    \"emit_phase_frame_hold\": true,\n"), cfg));
    EXPECT_TRUE(cfg.networkEmitPhaseFrameHold());
    EXPECT_FALSE(cfg.networkPhaseFrameHoldLevelVote());
}

TEST(PhaseFrameLevelVote, ConfigArmsWithTheFrameAndThePhaseClockHold) {
    GnssConfig cfg;
    ASSERT_TRUE(loadVoteConfig("armed", voteConfig("    \"emit_pb_hold\": true,\n"
                                                    "    \"emit_pb_hold_phase_clock\": true,\n"
                                                    "    \"emit_phase_frame_hold\": true,\n"
                                                    "    \"phase_frame_hold_level_vote\": true,\n"), cfg));
    EXPECT_TRUE(cfg.networkPhaseFrameHoldLevelVote());
}

TEST(PhaseFrameLevelVote, ConfigRefusesTheVoteWithoutTheFrame) {
    GnssConfig cfg;
    EXPECT_FALSE(loadVoteConfig("no_frame", voteConfig("    \"emit_pb_hold\": true,\n"
                                                        "    \"emit_pb_hold_phase_clock\": true,\n"
                                                        "    \"phase_frame_hold_level_vote\": true,\n"), cfg));
}

TEST(PhaseFrameLevelVote, ConfigRefusesTheVoteWithoutAHoldThatKeepsTheLevel) {
    GnssConfig cfg;
    EXPECT_FALSE(loadVoteConfig("no_level", voteConfig("    \"emit_pb_hold\": true,\n"
                                                        "    \"emit_phase_frame_hold\": true,\n"
                                                        "    \"phase_frame_hold_level_vote\": true,\n"), cfg));
}
