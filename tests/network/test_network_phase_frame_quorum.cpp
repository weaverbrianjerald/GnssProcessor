/**
 * @file test_network_phase_frame_quorum.cpp
 * @brief The phase-visible frame's quorum (`network.phase_frame_quorum`): the pure epoch
 *        `phaseFrameHoldEpoch` below and at the quorum, and the loader.
 *
 * The plain vote stands when its voters reach max(min_voters, ceil(quorum x n)), n the
 * constellation's satellites with a sample.  Below that, on a started frame, the satellites forced
 * by their own retro bump alone vote too, and the frame moves by the median of the wider set if it
 * reaches the same number, else it stays.
 *
 * Covered:
 *   - off (quorum 0): a minority of voters moves the frame, as without the key;
 *   - the mass decoupling of DOY 129 of 2024 (values rounded from the arm): 4 voters of 22, the frame
 *     takes the median over the 22 and not the minority's move;
 *   - at the quorum the plain vote stands and the marked satellites stay out;
 *   - a pending datum change of the constellation is not a quorum event and the frame stays;
 *   - no wider set to take (the others start a generation): the frame stays, below the quorum;
 *   - a marked satellite votes only if it continues in its generation;
 *   - the frame not yet started is not a quorum event;
 *   - with the floor of the vote in the hold's level, a widened vote of rounding residue moves nothing;
 *   - the next epoch's plain vote is back at the quorum;
 *   - the loader: off by default, a share outside [0, 1] refused, refused without the frame.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "network/emit/PhaseFrameHold.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include "../support/TestTempDir.hpp"

using namespace GnssProcessor;

namespace {

PhaseFrameSample smp(double v, double t, std::uint32_t gen = 1) {
    PhaseFrameSample s;
    s.pair = "L1C+L5Q";
    s.gen1 = gen;
    s.gen2 = gen;
    s.v_m  = v;
    s.t_s  = t;
    return s;
}

PhaseFrameEntry voter(const std::string& id, double v, double t, std::uint32_t gen = 1) {
    PhaseFrameEntry e;
    e.sat = id;
    e.now = smp(v, t, gen);
    return e;
}

PhaseFrameEntry ownMarked(const std::string& id, double v, double t, std::uint32_t gen = 1) {
    PhaseFrameEntry e = voter(id, v, t, gen);
    e.forced        = true;
    e.own_mark_only = true;
    return e;
}

PhaseFrameEntry constMarked(const std::string& id, double v, double t) {
    PhaseFrameEntry e = voter(id, v, t);
    e.forced = true;
    return e;
}

std::string galId(int i) {
    return std::string("E") + (i < 10 ? "0" : "") + std::to_string(i);
}

/// One epoch of satellites E01 .. En at value 0, storing the samples and starting nothing.
std::vector<PhaseFrameEntry> baseEpoch(int n) {
    std::vector<PhaseFrameEntry> out;
    for (int i = 1; i <= n; ++i) out.push_back(voter(galId(i), 0.0, 0.0));
    return out;
}

constexpr double kQuorum = 0.5;

/// A started frame: two epochs of n satellites with no common move, G present at 0.
struct Started {
    std::map<std::string, PhaseFrameSample> prev;
    std::map<char, double> g;
    PhaseFrameConfig cfg;
    explicit Started(int n, double quorum = kQuorum) {
        cfg.quorum = quorum;
        phaseFrameHoldEpoch(baseEpoch(n), 0.0, prev, g, cfg);
        std::vector<PhaseFrameEntry> e = baseEpoch(n);
        for (auto& x : e) x.now->t_s = 30.0;
        phaseFrameHoldEpoch(e, 30.0, prev, g, cfg);
    }
};

/// The vote of the mass decoupling: E01 .. E18 marked at about -43.7 mm, the 4 voters +402.0, -41.8,
/// +503.6, +405.0 mm.
std::vector<PhaseFrameEntry> massDecoupling(double t) {
    std::vector<PhaseFrameEntry> e;
    const std::vector<double> marked{-43.69, -43.69, -40.81, -44.74, -47.19, -37.40, -43.69, -43.69, -43.10,
                                     -41.57, -43.69, -51.44, -42.00, -43.69, -43.69, -47.64, -45.84, -45.93};
    for (std::size_t i = 0; i < marked.size(); ++i)
        e.push_back(ownMarked(galId(static_cast<int>(i) + 1), marked[i] * 1e-3, t));
    const std::vector<double> voters{402.0, -41.8, 503.6, 405.0};
    for (std::size_t i = 0; i < voters.size(); ++i)
        e.push_back(voter(galId(static_cast<int>(i) + 19), voters[i] * 1e-3, t));
    return e;
}

}  // namespace

TEST(PhaseFrameQuorum, OffAMinorityMovesTheFrame) {
    Started s(22, 0.0);
    const PhaseFrameStep st = phaseFrameHoldEpoch(massDecoupling(60.0), 60.0, s.prev, s.g, s.cfg).at('E');
    EXPECT_TRUE(st.moved);
    EXPECT_FALSE(st.below_quorum);
    EXPECT_FALSE(st.widened);
    EXPECT_EQ(st.voters, 4);
    EXPECT_NEAR(st.step_m, 0.4035, 1e-9);   // the median of the 4 voters
    EXPECT_NEAR(s.g.at('E'), 0.4035, 1e-9);
}

TEST(PhaseFrameQuorum, TheMassDecouplingTakesTheMedianOverTheConstellation) {
    Started s(22);
    const PhaseFrameStep st = phaseFrameHoldEpoch(massDecoupling(60.0), 60.0, s.prev, s.g, s.cfg).at('E');
    EXPECT_TRUE(st.below_quorum);
    EXPECT_TRUE(st.widened);
    EXPECT_TRUE(st.moved);
    EXPECT_EQ(st.voters, 4);
    EXPECT_EQ(st.forced, 18);
    EXPECT_EQ(st.own_marked, 18);
    EXPECT_EQ(st.present, 22);
    EXPECT_NEAR(st.plain_step_m, 0.4035, 1e-9);    // the step not taken
    EXPECT_NEAR(st.step_m, -0.04369, 1e-9);        // the median over the 22
    EXPECT_NEAR(s.g.at('E'), -0.04369, 1e-9);
}

TEST(PhaseFrameQuorum, AtTheQuorumThePlainVoteStands) {
    Started s(30);
    std::vector<PhaseFrameEntry> e;
    for (int i = 1; i <= 14; ++i) e.push_back(ownMarked(galId(i), -0.5, 60.0));     // marked, far away
    for (int i = 15; i <= 30; ++i) e.push_back(voter(galId(i), 0.002, 60.0));       // 16 of 30
    const PhaseFrameStep st = phaseFrameHoldEpoch(e, 60.0, s.prev, s.g, s.cfg).at('E');
    EXPECT_FALSE(st.below_quorum);
    EXPECT_FALSE(st.widened);
    EXPECT_EQ(st.voters, 16);
    EXPECT_EQ(st.own_marked, 14);
    EXPECT_NEAR(st.step_m, 0.002, 1e-12);
}

TEST(PhaseFrameQuorum, ADatumChangeOfTheConstellationIsNotAQuorumEvent) {
    Started s(22);
    const double g0 = s.g.at('E');
    std::vector<PhaseFrameEntry> e;
    for (int i = 1; i <= 22; ++i) e.push_back(constMarked(galId(i), 0.7, 60.0));
    const PhaseFrameStep st = phaseFrameHoldEpoch(e, 60.0, s.prev, s.g, s.cfg).at('E');
    EXPECT_FALSE(st.below_quorum);
    EXPECT_FALSE(st.widened);
    EXPECT_FALSE(st.moved);
    EXPECT_EQ(st.forced, 22);
    EXPECT_EQ(s.g.at('E'), g0);
}

TEST(PhaseFrameQuorum, NoWiderSetTheFrameStaysBelowTheQuorum) {
    Started s(22);
    std::vector<PhaseFrameEntry> e;
    for (int i = 1; i <= 18; ++i) e.push_back(voter(galId(i), 0.0, 60.0, 2));       // a new generation
    for (int i = 19; i <= 22; ++i) e.push_back(voter(galId(i), 0.4, 60.0));
    const PhaseFrameStep st = phaseFrameHoldEpoch(e, 60.0, s.prev, s.g, s.cfg).at('E');
    EXPECT_TRUE(st.below_quorum);
    EXPECT_FALSE(st.widened);
    EXPECT_FALSE(st.moved);
    EXPECT_EQ(st.voters, 4);
    EXPECT_NEAR(st.plain_step_m, 0.4, 1e-12);
    EXPECT_EQ(st.step_m, 0.0);
    EXPECT_EQ(s.g.at('E'), 0.0);
}

TEST(PhaseFrameQuorum, AMarkedSatelliteVotesOnlyIfItContinuesInItsGeneration) {
    Started s(22);
    std::vector<PhaseFrameEntry> e;
    for (int i = 1; i <= 12; ++i) e.push_back(ownMarked(galId(i), -0.1, 60.0, 2));  // marked and a new generation
    for (int i = 13; i <= 18; ++i) e.push_back(ownMarked(galId(i), -0.05, 60.0));
    for (int i = 19; i <= 22; ++i) e.push_back(voter(galId(i), 0.4, 60.0));
    const PhaseFrameStep st = phaseFrameHoldEpoch(e, 60.0, s.prev, s.g, s.cfg).at('E');
    EXPECT_EQ(st.own_marked, 6);
    EXPECT_TRUE(st.below_quorum);
    EXPECT_FALSE(st.widened);   // 4 + 6 = 10, the quorum of 22 is 11
    EXPECT_FALSE(st.moved);
}

TEST(PhaseFrameQuorum, AFrameNotYetStartedIsNotAQuorumEvent) {
    std::map<std::string, PhaseFrameSample> prev;
    std::map<char, double> g;
    PhaseFrameConfig cfg;
    cfg.quorum = kQuorum;
    phaseFrameHoldEpoch(baseEpoch(22), 0.0, prev, g, cfg);
    std::vector<PhaseFrameEntry> e;
    for (int i = 1; i <= 18; ++i) e.push_back(ownMarked(galId(i), -0.05, 30.0));
    for (int i = 19; i <= 22; ++i) e.push_back(voter(galId(i), 0.4, 30.0));
    const PhaseFrameStep st = phaseFrameHoldEpoch(e, 30.0, prev, g, cfg).at('E');
    EXPECT_FALSE(st.below_quorum);
    EXPECT_FALSE(st.moved);
    EXPECT_EQ(g.count('E'), 0u);
}

TEST(PhaseFrameQuorum, TheFloorHoldsAWidenedVoteOfRoundingResidue) {
    Started s(22);
    s.cfg.step_floor_m = kPhaseFrameLevelVoteFloorM;
    std::vector<PhaseFrameEntry> e;
    for (int i = 1; i <= 18; ++i) e.push_back(ownMarked(galId(i), 3e-14, 60.0));
    e.push_back(voter("E19", 0.447, 60.0));
    e.push_back(voter("E20", 0.447, 60.0));
    e.push_back(voter("E21", 0.0, 60.0));
    e.push_back(voter("E22", 0.545, 60.0));
    const PhaseFrameStep st = phaseFrameHoldEpoch(e, 60.0, s.prev, s.g, s.cfg).at('E');
    EXPECT_TRUE(st.widened);
    EXPECT_TRUE(st.floored);
    EXPECT_EQ(st.step_m, 0.0);
    EXPECT_EQ(s.g.at('E'), 0.0);
}

TEST(PhaseFrameQuorum, TheNextEpochsPlainVoteIsBackAtTheQuorum) {
    Started s(22);
    phaseFrameHoldEpoch(massDecoupling(60.0), 60.0, s.prev, s.g, s.cfg);
    const double g1 = s.g.at('E');
    // The 18 hold their level, the 3 excursions decay: their own motion, not the constellation's.
    std::vector<PhaseFrameEntry> e = massDecoupling(90.0);
    for (auto& x : e) {
        x.forced        = false;
        x.own_mark_only = false;
    }
    e[18].now->v_m -= 0.360;
    e[20].now->v_m -= 0.450;
    e[21].now->v_m -= 0.350;
    const PhaseFrameStep st = phaseFrameHoldEpoch(e, 90.0, s.prev, s.g, s.cfg).at('E');
    EXPECT_FALSE(st.below_quorum);
    EXPECT_EQ(st.voters, 22);
    EXPECT_NEAR(st.step_m, 0.0, 1e-12);
    EXPECT_NEAR(s.g.at('E'), g1, 1e-12);
}

namespace {

namespace fs = std::filesystem;

std::string quorumConfig(const std::string& network_extra) {
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

bool loadQuorumConfig(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = gnss_test::tempDir() / ("phase_frame_quorum_cfg_" + tag + ".json");
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

TEST(PhaseFrameQuorum, ConfigDefaultsOff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadQuorumConfig("default", quorumConfig("    \"emit_pb_hold\": true,\n"
                                                          "    \"emit_phase_frame_hold\": true,\n"), cfg));
    EXPECT_EQ(cfg.networkPhaseFrameQuorum(), 0.0);
}

TEST(PhaseFrameQuorum, ConfigArmsWithTheFrame) {
    GnssConfig cfg;
    ASSERT_TRUE(loadQuorumConfig("armed", quorumConfig("    \"emit_pb_hold\": true,\n"
                                                        "    \"emit_phase_frame_hold\": true,\n"
                                                        "    \"phase_frame_quorum\": 0.5,\n"), cfg));
    EXPECT_DOUBLE_EQ(cfg.networkPhaseFrameQuorum(), 0.5);
}

TEST(PhaseFrameQuorum, ConfigRefusesAShareOutsideTheUnitInterval) {
    GnssConfig a, b;
    EXPECT_FALSE(loadQuorumConfig("high", quorumConfig("    \"emit_pb_hold\": true,\n"
                                                        "    \"emit_phase_frame_hold\": true,\n"
                                                        "    \"phase_frame_quorum\": 1.5,\n"), a));
    EXPECT_FALSE(loadQuorumConfig("low", quorumConfig("    \"emit_pb_hold\": true,\n"
                                                       "    \"emit_phase_frame_hold\": true,\n"
                                                       "    \"phase_frame_quorum\": -0.1,\n"), b));
}

TEST(PhaseFrameQuorum, ConfigRefusesTheQuorumWithoutTheFrame) {
    GnssConfig cfg;
    EXPECT_FALSE(loadQuorumConfig("no_frame", quorumConfig("    \"emit_pb_hold\": true,\n"
                                                            "    \"phase_frame_quorum\": 0.5,\n"), cfg));
}
