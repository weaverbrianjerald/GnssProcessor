/**
 * @file test_network_phase_frame_hold.cpp
 * @brief The phase-visible frame hold (`network.emit_phase_frame_hold`): the sample
 *        `phaseFrameSample`, the pure epoch `phaseFrameHoldEpoch` and the loader.
 *
 * Each constellation's median innovation of the emitted phase-visible correction against the
 * raw a priori (C - P_IF + U), over the satellites continuing in their generation, accumulates
 * into a frame G that is added to every phase bias of the constellation.
 *
 * Covered:
 *   - the sample: the modelled pairs in order (GPS L1W then L1C with L2W, Galileo L1C with L5Q
 *     then L1X with L5X), the ionosphere-free combination, U added, the generations carried; no
 *     sample without a clock or a complete pair;
 *   - the first epoch stores and does not vote; a common move of every satellite is the step and
 *     starts the frame; one satellite's own move does not move it (median);
 *   - fewer voters than the minimum, a pending mark, a new generation, a changed pair and a
 *     sample older than the gap horizon keep a satellite out of the vote;
 *   - the leash re-anchors the frame to 0 and reports the peak;
 *   - the constellations are independent;
 *   - the loader: off by default, refused without the phase-bias hold, a non-positive leash refused.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "network/emit/PhaseFrameHold.hpp"
#include "products/NetworkProductEmitter.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include "../support/TestTempDir.hpp"

using namespace GnssProcessor;

namespace {

constexpr double kF1 = 1575.42e6, kF2 = 1227.60e6, kF5 = 1176.45e6;

double ifBias(double p1, double p2, double f1, double f2) {
    const double a = f1 * f1 / (f1 * f1 - f2 * f2);
    return a * p1 - (a - 1.0) * p2;
}

NetworkSatSolution sat(const std::string& id, double clock,
                       const std::map<std::string, std::pair<double, std::uint32_t>>& pb) {
    NetworkSatSolution s;
    s.sat        = id;
    s.has_clock  = true;
    s.clock_c0_m = clock;
    for (const auto& [sig, v] : pb) {
        NetworkPhaseBias b;
        b.bias_m          = v.first;
        b.hold_generation = v.second;
        s.phase_bias[sig] = b;
    }
    return s;
}

/// A sample of pair L1W+L2W, generations 1, value v at time t.
PhaseFrameSample smp(double v, double t, std::uint32_t g1 = 1, std::uint32_t g2 = 1,
                     const std::string& pair = "L1W+L2W") {
    PhaseFrameSample s;
    s.pair = pair;
    s.gen1 = g1;
    s.gen2 = g2;
    s.v_m  = v;
    s.t_s  = t;
    return s;
}

PhaseFrameEntry ent(const std::string& id, const PhaseFrameSample& s, bool forced = false) {
    PhaseFrameEntry e;
    e.sat    = id;
    e.now    = s;
    e.forced = forced;
    return e;
}

/// Five GPS satellites at time t with values base + offsets[i].
std::vector<PhaseFrameEntry> gpsEpoch(double t, double common, const std::vector<double>& own = {}) {
    std::vector<PhaseFrameEntry> out;
    for (int i = 0; i < 5; ++i) {
        const double o = (static_cast<std::size_t>(i) < own.size()) ? own[i] : 0.0;
        out.push_back(ent("G0" + std::to_string(i + 1), smp(0.1 * i + common + o, t)));
    }
    return out;
}

}  // namespace

TEST(PhaseFrameHold, TheSampleIsTheIonosphereFreePhaseVisibleCorrectionPlusTheTrack) {
    const NetworkSatSolution s = sat("G05", 1.25, { { "L1W", { 0.40, 3 } }, { "L2W", { -0.20, 4 } },
                                                    { "L1C", { 9.0, 1 } } });
    const auto out = phaseFrameSample(s, 0.03, 100.0);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->pair, "L1W+L2W");
    EXPECT_EQ(out->gen1, 3u);
    EXPECT_EQ(out->gen2, 4u);
    EXPECT_NEAR(out->v_m, 1.25 - ifBias(0.40, -0.20, kF1, kF2) + 0.03, 1e-12);
    EXPECT_DOUBLE_EQ(out->t_s, 100.0);
}

TEST(PhaseFrameHold, TheSampleFallsBackThroughTheModelledPairs) {
    const auto g = phaseFrameSample(sat("G07", 0.5, { { "L1C", { 0.1, 1 } }, { "L2W", { 0.2, 1 } } }), 0.0, 0.0);
    ASSERT_TRUE(g.has_value());
    EXPECT_EQ(g->pair, "L1C+L2W");
    const auto e1 = phaseFrameSample(sat("E11", 0.5, { { "L1C", { 0.1, 2 } }, { "L5Q", { 0.3, 2 } },
                                                      { "L1X", { 0.7, 1 } }, { "L5X", { 0.9, 1 } } }), 0.0, 0.0);
    ASSERT_TRUE(e1.has_value());
    EXPECT_EQ(e1->pair, "L1C+L5Q");
    EXPECT_NEAR(e1->v_m, 0.5 - ifBias(0.1, 0.3, kF1, kF5), 1e-12);
    const auto e2 = phaseFrameSample(sat("E12", 0.5, { { "L1X", { 0.7, 1 } }, { "L5X", { 0.9, 1 } } }), 0.0, 0.0);
    ASSERT_TRUE(e2.has_value());
    EXPECT_EQ(e2->pair, "L1X+L5X");
}

TEST(PhaseFrameHold, NoSampleWithoutAClockOrACompletePair) {
    NetworkSatSolution s = sat("G05", 1.0, { { "L1W", { 0.4, 1 } }, { "L2W", { 0.2, 1 } } });
    s.has_clock = false;
    EXPECT_FALSE(phaseFrameSample(s, 0.0, 0.0).has_value());
    EXPECT_FALSE(phaseFrameSample(sat("G05", 1.0, { { "L1W", { 0.4, 1 } } }), 0.0, 0.0).has_value());
    EXPECT_FALSE(phaseFrameSample(sat("R05", 1.0, { { "L1C", { 0.4, 1 } }, { "L2C", { 0.2, 1 } } }), 0.0, 0.0)
                     .has_value());
}

TEST(PhaseFrameHold, TheFirstEpochStoresAndDoesNotVote) {
    std::map<std::string, PhaseFrameSample> prev;
    std::map<char, double> g;
    const auto st = phaseFrameHoldEpoch(gpsEpoch(0.0, 0.0), 0.0, prev, g, PhaseFrameConfig{});
    ASSERT_EQ(st.count('G'), 1u);
    EXPECT_FALSE(st.at('G').moved);
    EXPECT_EQ(st.at('G').voters, 0);
    EXPECT_TRUE(g.empty());
    EXPECT_EQ(prev.size(), 5u);
}

TEST(PhaseFrameHold, ACommonMoveIsTheStepAndStartsTheFrame) {
    std::map<std::string, PhaseFrameSample> prev;
    std::map<char, double> g;
    phaseFrameHoldEpoch(gpsEpoch(0.0, 0.0), 0.0, prev, g, PhaseFrameConfig{});
    const auto st = phaseFrameHoldEpoch(gpsEpoch(30.0, 0.012), 30.0, prev, g, PhaseFrameConfig{});
    EXPECT_TRUE(st.at('G').moved);
    EXPECT_TRUE(st.at('G').started);
    EXPECT_EQ(st.at('G').voters, 5);
    EXPECT_NEAR(st.at('G').step_m, 0.012, 1e-12);
    EXPECT_NEAR(g.at('G'), 0.012, 1e-12);
    const auto st2 = phaseFrameHoldEpoch(gpsEpoch(60.0, 0.020), 60.0, prev, g, PhaseFrameConfig{});
    EXPECT_FALSE(st2.at('G').started);
    EXPECT_NEAR(st2.at('G').step_m, 0.008, 1e-12);
    EXPECT_NEAR(g.at('G'), 0.020, 1e-12);
    EXPECT_NEAR(st2.at('G').g_m, 0.020, 1e-12);
}

TEST(PhaseFrameHold, OneSatellitesOwnMoveDoesNotMoveTheFrame) {
    std::map<std::string, PhaseFrameSample> prev;
    std::map<char, double> g;
    phaseFrameHoldEpoch(gpsEpoch(0.0, 0.0), 0.0, prev, g, PhaseFrameConfig{});
    const auto st = phaseFrameHoldEpoch(gpsEpoch(30.0, 0.0, { 0.0, 0.0, 0.25 }), 30.0, prev, g,
                                        PhaseFrameConfig{});
    EXPECT_TRUE(st.at('G').moved);
    EXPECT_DOUBLE_EQ(st.at('G').step_m, 0.0);
}

TEST(PhaseFrameHold, TooFewVotersKeepTheFrame) {
    std::map<std::string, PhaseFrameSample> prev;
    std::map<char, double> g{ { 'G', 0.005 } };
    std::vector<PhaseFrameEntry> e0 = gpsEpoch(0.0, 0.0);
    e0.resize(3);
    phaseFrameHoldEpoch(e0, 0.0, prev, g, PhaseFrameConfig{});
    std::vector<PhaseFrameEntry> e1 = gpsEpoch(30.0, 0.05);
    e1.resize(3);
    const auto st = phaseFrameHoldEpoch(e1, 30.0, prev, g, PhaseFrameConfig{});
    EXPECT_FALSE(st.at('G').moved);
    EXPECT_EQ(st.at('G').voters, 3);
    EXPECT_DOUBLE_EQ(g.at('G'), 0.005);
}

TEST(PhaseFrameHold, APendingMarkKeepsASatelliteOutOfTheVote) {
    std::map<std::string, PhaseFrameSample> prev;
    std::map<char, double> g;
    phaseFrameHoldEpoch(gpsEpoch(0.0, 0.0), 0.0, prev, g, PhaseFrameConfig{});
    std::vector<PhaseFrameEntry> e1 = gpsEpoch(30.0, 0.01);
    for (auto& e : e1) e.forced = true;   // a datum change of the constellation
    const auto st = phaseFrameHoldEpoch(e1, 30.0, prev, g, PhaseFrameConfig{});
    EXPECT_FALSE(st.at('G').moved);
    EXPECT_EQ(st.at('G').forced, 5);
    EXPECT_EQ(st.at('G').voters, 0);
    EXPECT_TRUE(g.empty());
    // The forced epoch's samples are stored, so the next epoch votes against them.
    const auto st2 = phaseFrameHoldEpoch(gpsEpoch(60.0, 0.013), 60.0, prev, g, PhaseFrameConfig{});
    EXPECT_NEAR(st2.at('G').step_m, 0.003, 1e-12);
}

TEST(PhaseFrameHold, ANewGenerationAChangedPairOrAStaleSampleDoesNotVote) {
    std::map<std::string, PhaseFrameSample> prev;
    std::map<char, double> g;
    phaseFrameHoldEpoch(gpsEpoch(0.0, 0.0), 0.0, prev, g, PhaseFrameConfig{});
    std::vector<PhaseFrameEntry> e1 = gpsEpoch(30.0, 0.01);
    e1[0].now->gen2 = 2;           // G01: a new generation of L2W
    e1[1].now->pair = "L1C+L2W";   // G02: the pair changed
    const auto st = phaseFrameHoldEpoch(e1, 30.0, prev, g, PhaseFrameConfig{});
    EXPECT_EQ(st.at('G').voters, 3);
    EXPECT_FALSE(st.at('G').moved);
    // A previous sample beyond the gap horizon: nobody votes.
    const auto st2 = phaseFrameHoldEpoch(gpsEpoch(1000.0, 0.02), 1000.0, prev, g, PhaseFrameConfig{});
    EXPECT_EQ(st2.at('G').voters, 0);
}

TEST(PhaseFrameHold, TheLeashReanchorsTheFrame) {
    std::map<std::string, PhaseFrameSample> prev;
    std::map<char, double> g{ { 'G', 0.95 } };
    PhaseFrameConfig cfg;
    cfg.leash_m = 1.0;
    phaseFrameHoldEpoch(gpsEpoch(0.0, 0.0), 0.0, prev, g, cfg);
    const auto st = phaseFrameHoldEpoch(gpsEpoch(30.0, 0.08), 30.0, prev, g, cfg);
    EXPECT_TRUE(st.at('G').reanchored);
    EXPECT_NEAR(st.at('G').g_peak_m, 1.03, 1e-12);
    EXPECT_DOUBLE_EQ(st.at('G').g_m, 0.0);
    EXPECT_DOUBLE_EQ(g.at('G'), 0.0);
}

TEST(PhaseFrameHold, TheConstellationsAreIndependent) {
    std::map<std::string, PhaseFrameSample> prev;
    std::map<char, double> g;
    auto both = [](double t, double gps, double gal) {
        std::vector<PhaseFrameEntry> out = gpsEpoch(t, gps);
        for (int i = 0; i < 4; ++i)
            out.push_back(ent("E0" + std::to_string(i + 1), smp(0.2 * i + gal, t, 1, 1, "L1C+L5Q")));
        return out;
    };
    phaseFrameHoldEpoch(both(0.0, 0.0, 0.0), 0.0, prev, g, PhaseFrameConfig{});
    const auto st = phaseFrameHoldEpoch(both(30.0, 0.01, -0.004), 30.0, prev, g, PhaseFrameConfig{});
    EXPECT_NEAR(st.at('G').step_m, 0.01, 1e-12);
    EXPECT_NEAR(st.at('E').step_m, -0.004, 1e-12);
    EXPECT_NEAR(g.at('G'), 0.01, 1e-12);
    EXPECT_NEAR(g.at('E'), -0.004, 1e-12);
}

namespace {

namespace fs = std::filesystem;

std::string frameConfig(const std::string& network_extra) {
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

bool loadConfig(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = gnss_test::tempDir() / ("phase_frame_cfg_" + tag + ".json");
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

TEST(PhaseFrameHold, ConfigDefaultsOff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadConfig("default", frameConfig(""), cfg));
    EXPECT_FALSE(cfg.networkEmitPhaseFrameHold());
    EXPECT_DOUBLE_EQ(cfg.networkPhaseFrameLeashM(), 1.0);
}

TEST(PhaseFrameHold, ConfigArmsWithThePhaseBiasHold) {
    GnssConfig cfg;
    ASSERT_TRUE(loadConfig("armed", frameConfig("    \"emit_pb_hold\": true,\n"
                                                "    \"emit_phase_frame_hold\": true,\n"
                                                "    \"phase_frame_leash_m\": 0.5,\n"), cfg));
    EXPECT_TRUE(cfg.networkEmitPhaseFrameHold());
    EXPECT_DOUBLE_EQ(cfg.networkPhaseFrameLeashM(), 0.5);
}

TEST(PhaseFrameHold, ConfigRefusesTheFrameWithoutThePhaseBiasHold) {
    GnssConfig cfg;
    EXPECT_FALSE(loadConfig("no_hold", frameConfig("    \"emit_phase_frame_hold\": true,\n"), cfg));
}

TEST(PhaseFrameHold, ConfigRefusesANonPositiveLeash) {
    GnssConfig cfg;
    EXPECT_FALSE(loadConfig("leash", frameConfig("    \"emit_pb_hold\": true,\n"
                                                 "    \"emit_phase_frame_hold\": true,\n"
                                                 "    \"phase_frame_leash_m\": 0.0,\n"), cfg));
}
