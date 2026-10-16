/**
 * @file test_channel_mode_all.cpp
 * @brief Contract tests for `processing.channel_mode: all` on the network path
 *        (card SID phase 6b, docs/refound/SID_signal_identity_2026-09-22.md).
 *
 * Under the widening, every admitted code and phase attribute of every selected
 * band is modelled at every station.  The widening subsumes card SCC (the second
 * code channel) and card SID phase 6a (the second phase channel) instead of being
 * refused beside them: their allow-lists are ignored, their sigma scales apply to
 * every non-canonical code row and non-canonical phase row, and the rows enter
 * through the same two sets so every downstream consumer keeps working.
 *
 * What is pinned here: the pure kernels, the roster and the loader, i.e. every
 * part that can be tested without a network solve:
 *
 *   (a) the extra-channel predicate: a signal is extra exactly when its band's
 *       latched signal of the same type is known, present and different;
 *   (b) the widening's admitted set: every band digit, both observation types, the
 *       network coverage allow-list respected, a channel absent from the priority
 *       list admitted all the same, and nothing on an unselected band;
 *   (c) the roster under the widening: `modelled_extra` and `companion_code` filled
 *       on every band from the tracked attributes, the modelled roles untouched,
 *       and an extra channel not reported as an excluded signal;
 *   (d) the loader: subsumption in place of the two refusals (both knobs stay
 *       armed and their resolved row predicates answer true), the float-vehicle
 *       interlock clamping the mode back to single, and the consumer override
 *       (`network.closed_loop.channel_mode`, default single).
 *
 * The mechanism itself (which rows enter the solve, which families reach the wire,
 * what it costs) is graded on the arms by the card's own gates.
 */
#include <gtest/gtest.h>

#include "network/assembly/SignalRoster.hpp"
#include "config/GnssConfig.hpp"
#include "network/assembly/ChannelAll.hpp"
#include "logging/LogManager.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

using GnssProcessor::BandRoster;
using GnssProcessor::ChannelMode;
using GnssProcessor::GnssConfig;
using GnssProcessor::LogManager;
using GnssProcessor::RosterLatchRow;
using GnssProcessor::channelAllExtraChannels;
using GnssProcessor::channelAllIsExtra;
using GnssProcessor::signalRosterBuildCells;

namespace {

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("cma_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

/// A minimal loadable PPP-RTK network config; @p network_extra splices into the
/// network block and @p processing_extra into the processing block.
std::string networkConfig(const std::string& network_extra,
                          const std::string& processing_extra = "") {
    return std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        + processing_extra +
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

/// The network block the widening's own interlock needs (the float vehicle with a
/// held basis), plus the vehicle's own prerequisites, which the loader checks
/// first; without them the load itself would fail.
const char* kVehicle =
    "    \"ambiguity_vehicle\": \"float_kf\",\n"
    "    \"phase_leveling\": true,\n"
    "    \"shared_carry_full_cov\": true,\n"
    "    \"carry_conditional_coupling\": true,\n"
    "    \"phase_datum_held_basis\": true,\n"
    "    \"phase_datum_prioritized\": true,\n";

const char* kChanAll = "    \"channel_mode\": \"all\",\n";

/// One station's cells, built the way the roster builds them under the widening.
std::map<char, BandRoster> cellsAll(const std::vector<std::string>& tracked,
                                    const std::vector<RosterLatchRow>& latch,
                                    bool all_channels,
                                    const std::set<std::string>* allow = nullptr) {
    return signalRosterBuildCells(tracked, latch, {}, false, allow, {}, false,
                                  all_channels);
}

bool hasExtra(const BandRoster& br, const std::string& sig) {
    return std::find(br.modelled_extra.begin(), br.modelled_extra.end(), sig)
           != br.modelled_extra.end();
}

bool hasCompanion(const BandRoster& br, const std::string& sig) {
    return std::find(br.companion_code.begin(), br.companion_code.end(), sig)
           != br.companion_code.end();
}

}  // namespace

// --------------------------------------------------------------------------
// (a) the extra-channel predicate
// --------------------------------------------------------------------------

TEST(ChannelModeAllTest, ASignalThatIsNotTheLatchedOneOfItsBandIsExtra) {
    EXPECT_TRUE(channelAllIsExtra("L1C", "L1W", true));
    EXPECT_TRUE(channelAllIsExtra("C2X", "C2W", true));
}

TEST(ChannelModeAllTest, TheLatchedSignalIsNeverExtraToItself) {
    EXPECT_FALSE(channelAllIsExtra("L1W", "L1W", true));
}

TEST(ChannelModeAllTest, AnUndecidedLatchAnswersFalseRatherThanPromoting) {
    // The "change nothing" contract every latch reader keeps: with no canonical
    // decided, no signal is second to another.
    EXPECT_FALSE(channelAllIsExtra("L1C", "", true));
}

TEST(ChannelModeAllTest, AnAbsentCanonicalMakesThisTheFallbackNotASecond) {
    // The band's canonical row has to be present this epoch, the same clause
    // cards SCC and SID phase 6a apply; without it the row is the fallback case.
    EXPECT_FALSE(channelAllIsExtra("L1C", "L1W", false));
}

TEST(ChannelModeAllTest, ASignalOfAnotherBandIsNotExtraToThisCanonical) {
    EXPECT_FALSE(channelAllIsExtra("L2W", "L1W", true));
}

// --------------------------------------------------------------------------
// (b) the widening's admitted set
// --------------------------------------------------------------------------

TEST(ChannelModeAllTest, AddsEveryOtherAttributeOfEverySelectedBand) {
    const std::vector<std::string> keys{ "C1C", "C1W", "L1C", "L1W",
                                         "C2W", "C2X", "L2W", "L2X" };
    const auto got = channelAllExtraChannels(
        keys, { '1', '2' }, { { '1', "C1W" }, { '2', "C2W" } },
        { { '1', "L1W" }, { '2', "L2W" } }, nullptr);
    EXPECT_EQ(got, (std::vector<std::string>{ "C1C", "C2X", "L1C", "L2X" }));
}

TEST(ChannelModeAllTest, WidensTheCHANNELDimensionAndNeverTheBAND) {
    // A band the run did not select contributes nothing, whatever it tracks.
    const std::vector<std::string> keys{ "C1C", "C1W", "L1C", "L1W", "C5Q", "L5Q" };
    const auto got = channelAllExtraChannels(
        keys, { '1' }, { { '1', "C1W" } }, { { '1', "L1W" } }, nullptr);
    EXPECT_EQ(got, (std::vector<std::string>{ "C1C", "L1C" }));
}

TEST(ChannelModeAllTest, AChannelAbsentFromThePriorityListStillEntersWhenAdmitted) {
    // The X channels are not in the working bases' GPS priority list, so the latch
    // can never choose one; the widening admits them all the same, which is what
    // makes band 2 the population that the knob of card RF-0f decides.
    const std::vector<std::string> keys{ "C2W", "C2X", "L2W", "L2X" };
    const auto got = channelAllExtraChannels(
        keys, { '2' }, { { '2', "C2W" } }, { { '2', "L2W" } }, nullptr);
    EXPECT_EQ(got, (std::vector<std::string>{ "C2X", "L2X" }));
}

TEST(ChannelModeAllTest, TheNetworkCoverageAllowListIsRespected) {
    const std::vector<std::string> keys{ "C1C", "C1W", "L1C", "L1W" };
    const std::vector<std::string> allowed{ "C1C", "C1W", "L1W" };   // L1C gated out
    const auto got = channelAllExtraChannels(
        keys, { '1' }, { { '1', "C1W" } }, { { '1', "L1W" } }, &allowed);
    EXPECT_EQ(got, (std::vector<std::string>{ "C1C" }));
}

TEST(ChannelModeAllTest, AStationWithOneAttributePerBandAddsNothing) {
    const std::vector<std::string> keys{ "C1W", "L1W", "C2W", "L2W" };
    const auto got = channelAllExtraChannels(
        keys, { '1', '2' }, { { '1', "C1W" }, { '2', "C2W" } },
        { { '1', "L1W" }, { '2', "L2W" } }, nullptr);
    EXPECT_TRUE(got.empty());
}

TEST(ChannelModeAllTest, AnUndecidedBandContributesNothingRatherThanEverything) {
    const std::vector<std::string> keys{ "C1C", "C1W", "L1C", "L1W" };
    const auto got = channelAllExtraChannels(keys, { '1' }, {}, {}, nullptr);
    EXPECT_TRUE(got.empty());
}

// --------------------------------------------------------------------------
// (c) the roster under the widening
// --------------------------------------------------------------------------

TEST(ChannelModeAllTest, TheRosterCarriesExtraPhasesAndCodesOnEveryBand) {
    const std::vector<std::string> tracked{ "C1C", "C1W", "L1C", "L1W",
                                            "C2W", "C2X", "L2W", "L2X" };
    const std::vector<RosterLatchRow> latch{ { 'C', "C1W" }, { 'L', "L1W" },
                                             { 'C', "C2W" }, { 'L', "L2W" } };
    const auto on = cellsAll(tracked, latch, true);
    ASSERT_EQ(on.count('1'), 1u);
    ASSERT_EQ(on.count('2'), 1u);
    EXPECT_TRUE(hasExtra(on.at('1'), "L1C"));
    EXPECT_TRUE(hasCompanion(on.at('1'), "C1C"));
    EXPECT_TRUE(hasExtra(on.at('2'), "L2X"));       // band 2 is phase 6b's own half
    EXPECT_TRUE(hasCompanion(on.at('2'), "C2X"));
    // The modelled roles are the latch's and are untouched: phase 2's tracker and
    // every phase-3 population read them.
    EXPECT_EQ(on.at('1').modelled_phase, "L1W");
    EXPECT_EQ(on.at('2').modelled_code, "C2W");
}

TEST(ChannelModeAllTest, TheRosterIsUnchangedWhenTheWideningIsOff) {
    const std::vector<std::string> tracked{ "C1C", "C1W", "L1C", "L1W" };
    const std::vector<RosterLatchRow> latch{ { 'C', "C1W" }, { 'L', "L1W" } };
    const auto off = cellsAll(tracked, latch, false);
    ASSERT_EQ(off.count('1'), 1u);
    EXPECT_TRUE(off.at('1').modelled_extra.empty());
    EXPECT_TRUE(off.at('1').companion_code.empty());
}

TEST(ChannelModeAllTest, TheRosterAddsNothingOnABandWhoseCanonicalIsAbsent) {
    const std::vector<std::string> tracked{ "C1C", "L1C" };   // no C1W / L1W today
    const std::vector<RosterLatchRow> latch{ { 'C', "C1W" }, { 'L', "L1W" } };
    const auto on = cellsAll(tracked, latch, true);
    ASSERT_EQ(on.count('1'), 1u);
    EXPECT_TRUE(on.at('1').modelled_extra.empty());
    EXPECT_TRUE(on.at('1').companion_code.empty());
}

TEST(ChannelModeAllTest, TheRosterHonoursTheNetworkSignalAllowList) {
    const std::vector<std::string> tracked{ "C1C", "C1W", "L1C", "L1W" };
    const std::vector<RosterLatchRow> latch{ { 'C', "C1W" }, { 'L', "L1W" } };
    const std::set<std::string> allow{ "C1C", "C1W", "L1W" };   // L1C gated out
    const auto on = cellsAll(tracked, latch, true, &allow);
    ASSERT_EQ(on.count('1'), 1u);
    EXPECT_FALSE(hasExtra(on.at('1'), "L1C"));
    EXPECT_TRUE(hasCompanion(on.at('1'), "C1C"));
}

TEST(ChannelModeAllTest, AnExtraChannelIsNotReportedAsAnExcludedSignal) {
    const std::vector<std::string> tracked{ "C1C", "C1W", "L1C", "L1W" };
    const std::vector<RosterLatchRow> latch{ { 'C', "C1W" }, { 'L', "L1W" } };
    const auto on = cellsAll(tracked, latch, true);
    ASSERT_EQ(on.count('1'), 1u);
    for (const auto& [sig, why] : on.at('1').excluded) {
        (void)why;
        EXPECT_NE(sig, "L1C");
        EXPECT_NE(sig, "C1C");
    }
}

// --------------------------------------------------------------------------
// (d) the loader: subsumption, the interlock, the consumer override
// --------------------------------------------------------------------------

TEST(ChannelModeAllTest, DefaultsToSingleOnTheNetworkPath) {
    LogManager log;
    const fs::path p = writeTempConfig("default", networkConfig(""));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_EQ(cfg.getChannelMode(), ChannelMode::Single);
    EXPECT_FALSE(cfg.networkChannelModeAll());
    EXPECT_FALSE(cfg.networkSecondCodeRowsActive());
    EXPECT_FALSE(cfg.networkSecondPhaseRowsActive());
    std::error_code ec; fs::remove(p, ec);
}

TEST(ChannelModeAllTest, ArmsOnTheFloatVehicleWithTheHeldBasis) {
    LogManager log;
    const fs::path p = writeTempConfig("armed", networkConfig(kVehicle, kChanAll));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.networkChannelModeAll());
    // Both row predicates answer true through the subsumption, with neither card's
    // own knob set; that lets one predicate serve both mechanisms.
    EXPECT_TRUE(cfg.networkSecondCodeRowsActive());
    EXPECT_TRUE(cfg.networkSecondPhaseRowsActive());
    EXPECT_FALSE(cfg.networkSecondCodeChannel());
    EXPECT_FALSE(cfg.networkSecondPhaseChannel());
    std::error_code ec; fs::remove(p, ec);
}

TEST(ChannelModeAllTest, IsClampedToSingleOnTheLevelingVehicle) {
    // Every extra phase channel would create its own zero-q level with no wired
    // event response, against standing invariant #1; card SID phase 6a's own
    // interlock applies the same rule to a single extra channel.
    LogManager log;
    const fs::path p = writeTempConfig("level", networkConfig("", kChanAll));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_EQ(cfg.getChannelMode(), ChannelMode::Single);
    EXPECT_FALSE(cfg.networkChannelModeAll());
    std::error_code ec; fs::remove(p, ec);
}

TEST(ChannelModeAllTest, SubsumesTheSecondCodeChannelInsteadOfRefusingIt) {
    LogManager log;
    const fs::path p = writeTempConfig("scc",
        networkConfig(std::string(kVehicle)
                      + "    \"second_code_channel\": true,\n"
                        "    \"carry_rx_code_bias\": true,\n"
                        "    \"second_code_sigma_scale\": 10.0,\n",
                      kChanAll));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.networkChannelModeAll());
    EXPECT_TRUE(cfg.networkSecondCodeChannel());        // armed, not disarmed
    EXPECT_TRUE(cfg.networkSecondCodeRowsActive());
    // The scale is still read under the subsumption; the allow-list is not.
    EXPECT_DOUBLE_EQ(cfg.networkSecondCodeSigmaScale(), 10.0);
    std::error_code ec; fs::remove(p, ec);
}

TEST(ChannelModeAllTest, SubsumesTheSecondPhaseChannelInsteadOfRefusingIt) {
    LogManager log;
    const fs::path p = writeTempConfig("spc",
        networkConfig(std::string(kVehicle)
                      + "    \"second_phase_channel\": true,\n"
                        "    \"second_phase_sigma_scale\": 3.0,\n",
                      kChanAll));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.networkChannelModeAll());
    EXPECT_TRUE(cfg.networkSecondPhaseChannel());       // armed, not disarmed
    EXPECT_TRUE(cfg.networkSecondPhaseRowsActive());
    EXPECT_DOUBLE_EQ(cfg.networkSecondPhaseSigmaScale(), 3.0);
    std::error_code ec; fs::remove(p, ec);
}

TEST(ChannelModeAllTest, TheConsumerKeepsItsOwnChannelModeByDefault) {
    // A producer at `all` does not widen the held-out consumer unless asked, so an
    // arm of the producer's widening moves only the wire (R6).
    LogManager log;
    const fs::path p = writeTempConfig("cons", networkConfig(kVehicle, kChanAll));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    ASSERT_TRUE(cfg.networkChannelModeAll());
    const GnssConfig consumer = cfg.deriveClosedLoopConsumerConfig();
    EXPECT_EQ(consumer.getChannelMode(), ChannelMode::Single);
    std::error_code ec; fs::remove(p, ec);
}

TEST(ChannelModeAllTest, TheConsumerWidensWhenTheHarnessAsksForIt) {
    LogManager log;
    const fs::path p = writeTempConfig("conall",
        networkConfig(std::string(kVehicle)
                      + "    \"closed_loop\": { \"channel_mode\": \"all\" },\n",
                      kChanAll));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    const GnssConfig consumer = cfg.deriveClosedLoopConsumerConfig();
    EXPECT_EQ(consumer.getChannelMode(), ChannelMode::All);
    std::error_code ec; fs::remove(p, ec);
}

TEST(ChannelModeAllTest, AProducerAtSingleLeavesTheConsumerAtSingleToo) {
    LogManager log;
    const fs::path p = writeTempConfig("consgl", networkConfig(kVehicle));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    const GnssConfig consumer = cfg.deriveClosedLoopConsumerConfig();
    EXPECT_EQ(consumer.getChannelMode(), ChannelMode::Single);
    std::error_code ec; fs::remove(p, ec);
}
