/**
 * @file test_second_phase_channel.cpp
 * @brief Contract tests for the second phase channel (card SID phase 6a,
 *        docs/refound/SID_signal_identity_2026-09-22.md section 4).
 *
 * With `network.second_phase_channel` a network station contributes, as ordinary
 * float-ambiguity phase rows, every allow-listed band-1 carrier phase it reports
 * besides the one its canonical latch selected, so the L1C and L1W satellite
 * phase-bias families share stations instead of competing for them.
 *
 * The file pins the pure kernels, the roster and the loader, which is every part
 * that can be tested without a network solve:
 *
 *   (a) the allow-list grammar: a 3-character phase type on band 1 and nothing
 *       else.  A code type belongs to card SCC and a band-2 entry to phase 6b;
 *   (b) second-phase selection: allow-list order, no duplicates, never the latched
 *       phase itself, never a phase the station does not report, and empty when the
 *       band has no latched phase (the fallback case, which adds nothing);
 *   (c) the cell edges of standing invariant #7, including the case the invariant
 *       was written for: a station that drops out entirely closes its cells, so its
 *       return reads as a re-coupling rather than as a first sighting;
 *   (d) the witness (R8): a slip on one channel appears as an integer step in
 *       (latched minus second), and a slip common to both cancels;
 *   (e) the roster's `modelled_extra`: filled only when armed, band 1 only, the
 *       latched phase required present, the allow list respected, and the modelled
 *       phase itself unchanged (the phase-2 tracker keeps reading it);
 *   (f) the loader contract: default off, the default allow-list, a refused entry
 *       dropped and named, the sigma scale's floor of 1, the interlocks that disarm
 *       the knob (the leveling vehicle, the float vehicle without a held basis, and
 *       a list with fewer than two admitted entries), channel_mode `all` subsuming
 *       the knob rather than disarming it, the gap-tier and IF-TD warnings, and the
 *       closed-loop consumer pinned off.
 *
 * The mechanism itself (which rows enter the solve, which families reach the wire)
 * is graded on the arms by the card's own gates.
 */
#include <gtest/gtest.h>

#include "network/assembly/SignalRoster.hpp"
#include "config/GnssConfig.hpp"
#include "network/assembly/SecondPhaseScope.hpp"
#include "logging/LogManager.hpp"

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

using GnssProcessor::BandRoster;
using GnssProcessor::GnssConfig;
using GnssProcessor::LogManager;
using GnssProcessor::RosterLatchRow;
using GnssProcessor::secondPhaseAllowEntryValid;
using GnssProcessor::secondPhaseCellEdges;
using GnssProcessor::secondPhaseCompanions;
using GnssProcessor::secondPhaseWitness;
using GnssProcessor::signalRosterBuildCells;

namespace {

/// The card's default allow-list.
const std::vector<std::string> kDefaultList{ "L1C", "L1W" };

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("spc_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

/// A minimal loadable PPP-RTK network config; @p network_extra splices into the
/// network block, and @p processing_extra into the processing block.
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

/// The network block that satisfies every interlock, so a test can vary one knob
/// at a time without the loader disarming the card for an unrelated reason.
/// `ambiguity_vehicle: float_kf` has its own prerequisites (phase leveling, the
/// conditional coupling and the full shared covariance it rests on), which the
/// loader checks first; without them the load itself would fail.
const char* kVehicle =
    "    \"ambiguity_vehicle\": \"float_kf\",\n"
    "    \"phase_leveling\": true,\n"
    "    \"shared_carry_full_cov\": true,\n"
    "    \"carry_conditional_coupling\": true,\n"
    "    \"phase_datum_prioritized\": true,\n";

const char* kArmable =
    "    \"second_phase_channel\": true,\n"
    "    \"ambiguity_vehicle\": \"float_kf\",\n"
    "    \"phase_leveling\": true,\n"
    "    \"shared_carry_full_cov\": true,\n"
    "    \"carry_conditional_coupling\": true,\n"
    "    \"phase_datum_held_basis\": true,\n"
    "    \"phase_datum_prioritized\": true,\n";

/// One station's band-1 cell, built the way the roster builds it.
BandRoster band1(const std::vector<std::string>& tracked,
                 const std::vector<RosterLatchRow>& latch,
                 const std::vector<std::string>& allow, bool armed,
                 const std::set<std::string>* allowlist = nullptr) {
    const auto cells = signalRosterBuildCells(tracked, latch, {}, false, allowlist,
                                              allow, armed);
    const auto it = cells.find('1');
    return it == cells.end() ? BandRoster{} : it->second;
}

}  // namespace

// --------------------------------------------------------------------------
// (a) the allow-list grammar
// --------------------------------------------------------------------------

TEST(SecondPhaseChannelTest, AdmitsABandOnePhaseType) {
    std::string why{ "unset" };
    EXPECT_TRUE(secondPhaseAllowEntryValid("L1C", &why));
    EXPECT_TRUE(why.empty());
    EXPECT_TRUE(secondPhaseAllowEntryValid("L1W", nullptr));
    EXPECT_TRUE(secondPhaseAllowEntryValid("L1X", nullptr));
}

TEST(SecondPhaseChannelTest, RefusesACodeTypeBecauseThatIsCardScc) {
    std::string why;
    EXPECT_FALSE(secondPhaseAllowEntryValid("C1W", &why));
    EXPECT_NE(why.find("PHASE"), std::string::npos);
}

TEST(SecondPhaseChannelTest, RefusesAnyBandOtherThanOne) {
    std::string why;
    EXPECT_FALSE(secondPhaseAllowEntryValid("L2W", &why));
    EXPECT_NE(why.find("band 1"), std::string::npos);
    EXPECT_FALSE(secondPhaseAllowEntryValid("L5Q", nullptr));
}

TEST(SecondPhaseChannelTest, RefusesAnythingThatIsNotAThreeCharacterPhase) {
    EXPECT_FALSE(secondPhaseAllowEntryValid("", nullptr));
    EXPECT_FALSE(secondPhaseAllowEntryValid("L1", nullptr));
    EXPECT_FALSE(secondPhaseAllowEntryValid("L1CX", nullptr));
    EXPECT_FALSE(secondPhaseAllowEntryValid("l1c", nullptr));
}

// --------------------------------------------------------------------------
// (b) second-phase selection
// --------------------------------------------------------------------------

TEST(SecondPhaseChannelTest, PicksTheAllowListedPhaseThatIsNotTheLatchedOne) {
    const std::set<std::string> reported{ "L1C", "L1W" };
    EXPECT_EQ(secondPhaseCompanions(reported, "L1C", kDefaultList),
              std::vector<std::string>({ "L1W" }));
    EXPECT_EQ(secondPhaseCompanions(reported, "L1W", kDefaultList),
              std::vector<std::string>({ "L1C" }));
}

TEST(SecondPhaseChannelTest, AStationThatReportsOnlyItsLatchedPhaseContributesNothing) {
    const std::set<std::string> reported{ "L1C" };
    EXPECT_TRUE(secondPhaseCompanions(reported, "L1C", kDefaultList).empty());
}

TEST(SecondPhaseChannelTest, TheResultIsInAllowListOrderNotReceiverOrder) {
    const std::set<std::string> reported{ "L1C", "L1W", "L1X" };   // set iterates ascending
    const std::vector<std::string> list{ "L1X", "L1W", "L1C" };
    EXPECT_EQ(secondPhaseCompanions(reported, "L1C", list),
              std::vector<std::string>({ "L1X", "L1W" }));
}

TEST(SecondPhaseChannelTest, ARepeatedAllowListEntryIsNotARepeatedRow) {
    const std::set<std::string> reported{ "L1C", "L1W" };
    const std::vector<std::string> list{ "L1C", "L1W", "L1W" };
    EXPECT_EQ(secondPhaseCompanions(reported, "L1C", list),
              std::vector<std::string>({ "L1W" }));
}

TEST(SecondPhaseChannelTest, NoLatchedPhaseMeansNothingIsAddedBecauseThatIsTheFallback) {
    // The latch decides what "second" means.  An allow-listed phase arriving with
    // no latched phase beside it is the fallback case, counted by the serial sweep
    // and never promoted to the only phase of the band (R1).
    const std::set<std::string> reported{ "L1C", "L1W" };
    EXPECT_TRUE(secondPhaseCompanions(reported, "", kDefaultList).empty());
}

TEST(SecondPhaseChannelTest, AnEmptyAllowListMakesTheMechanismInertRatherThanWide) {
    const std::set<std::string> reported{ "L1C", "L1W", "L1X" };
    EXPECT_TRUE(secondPhaseCompanions(reported, "L1C", {}).empty());
}

TEST(SecondPhaseChannelTest, APhaseTheStationDoesNotReportIsNeverAdded) {
    const std::set<std::string> reported{ "L1C" };
    const std::vector<std::string> list{ "L1C", "L1W", "L1X" };
    EXPECT_TRUE(secondPhaseCompanions(reported, "L1C", list).empty());
}

// --------------------------------------------------------------------------
// (c) the two cell edges (standing invariant #7)
// --------------------------------------------------------------------------

TEST(SecondPhaseChannelTest, ReportsBothEdgesOfTheOpenCellSet) {
    const std::set<std::string> prev{ "G01|L1W|PB", "G02|L1W|PB" };
    const std::set<std::string> cur{ "G02|L1W|PB", "G03|L1W|PB" };
    std::vector<std::string> opened, closed;
    secondPhaseCellEdges(prev, cur, &opened, &closed);
    EXPECT_EQ(opened, std::vector<std::string>({ "G03|L1W|PB" }));
    EXPECT_EQ(closed, std::vector<std::string>({ "G01|L1W|PB" }));
}

TEST(SecondPhaseChannelTest, AStationDroppingOutClosesEveryCellItHeld) {
    // The failure standing invariant #7 names: a station with zero rows does not
    // leave the map without an edge and then return as a first sighting.
    const std::set<std::string> prev{ "G01|L1W|PB", "G02|L1W|PB" };
    std::vector<std::string> opened, closed;
    secondPhaseCellEdges(prev, {}, &opened, &closed);
    EXPECT_TRUE(opened.empty());
    EXPECT_EQ(closed.size(), 2u);
}

TEST(SecondPhaseChannelTest, AnUnchangedCellSetHasNoEdges) {
    const std::set<std::string> s{ "G01|L1W|PB" };
    std::vector<std::string> opened, closed;
    secondPhaseCellEdges(s, s, &opened, &closed);
    EXPECT_TRUE(opened.empty());
    EXPECT_TRUE(closed.empty());
}

TEST(SecondPhaseChannelTest, EitherEdgeOutputMayBeNull) {
    const std::set<std::string> prev{ "G01|L1W|PB" };
    const std::set<std::string> cur{ "G02|L1W|PB" };
    std::vector<std::string> only;
    secondPhaseCellEdges(prev, cur, &only, nullptr);
    EXPECT_EQ(only, std::vector<std::string>({ "G02|L1W|PB" }));
    secondPhaseCellEdges(prev, cur, nullptr, &only);
    EXPECT_EQ(only, std::vector<std::string>({ "G01|L1W|PB" }));
}

// --------------------------------------------------------------------------
// (d) the between-channel witness (R8)
// --------------------------------------------------------------------------

TEST(SecondPhaseChannelTest, TheWitnessIsTheDifferenceOfTheTwoBandOnePhases) {
    EXPECT_DOUBLE_EQ(secondPhaseWitness(1.0e8, 1.0e8 - 3.25), 3.25);
}

TEST(SecondPhaseChannelTest, AOneChannelSlipMovesTheWitnessByThatInteger) {
    // A synthetic pair over two epochs: both channels drift by the same 0.02 cycles
    // of geometry and one of them takes a 4-cycle step.  The witness change is the
    // integer alone, so the 0.5-cycle bar fires.
    const double l1_a = 1.0e8, l1w_a = 1.0e8 - 0.75;
    const double l1_b = l1_a + 0.02, l1w_b = l1w_a + 0.02 + 4.0;
    const double d0 = secondPhaseWitness(l1_a, l1w_a);
    const double d1 = secondPhaseWitness(l1_b, l1w_b);
    EXPECT_NEAR(std::abs(d1 - d0), 4.0, 1e-9);
    EXPECT_GT(std::abs(d1 - d0), 0.5);
}

TEST(SecondPhaseChannelTest, ASlipCommonToBothChannelsIsInvisibleToTheWitness) {
    const double l1_a = 1.0e8, l1w_a = 1.0e8 - 0.75;
    const double l1_b = l1_a + 0.02 + 7.0, l1w_b = l1w_a + 0.02 + 7.0;
    EXPECT_NEAR(std::abs(secondPhaseWitness(l1_b, l1w_b)
                         - secondPhaseWitness(l1_a, l1w_a)), 0.0, 1e-9);
}

TEST(SecondPhaseChannelTest, OrdinaryNoiseStaysUnderTheHalfCycleBar) {
    const double d0 = secondPhaseWitness(1.0e8, 1.0e8 - 0.75);
    const double d1 = secondPhaseWitness(1.0e8 + 0.013, 1.0e8 - 0.75 - 0.011);
    EXPECT_LT(std::abs(d1 - d0), 0.5);
}

// --------------------------------------------------------------------------
// (e) the roster's modelled_extra
// --------------------------------------------------------------------------

TEST(SecondPhaseChannelTest, TheRosterCarriesTheSecondPhaseOnlyWhenArmed) {
    const std::vector<std::string> tracked{ "C1C", "C1W", "L1C", "L1W", "C2W", "L2W" };
    const std::vector<RosterLatchRow> latch{ { 'L', "L1C" }, { 'C', "C1C" },
                                             { 'L', "L2W" }, { 'C', "C2W" } };
    EXPECT_TRUE(band1(tracked, latch, kDefaultList, false).modelled_extra.empty());
    const BandRoster on = band1(tracked, latch, kDefaultList, true);
    EXPECT_EQ(on.modelled_extra, std::vector<std::string>({ "L1W" }));
    EXPECT_EQ(on.modelled_phase, "L1C");   // phase 2's tracker still reads the latch
}

TEST(SecondPhaseChannelTest, TheRosterLeavesBandTwoUntouched) {
    const std::vector<std::string> tracked{ "C2W", "L2W", "L2L", "C1C", "L1C" };
    const std::vector<RosterLatchRow> latch{ { 'L', "L2W" }, { 'C', "C2W" },
                                             { 'L', "L1C" }, { 'C', "C1C" } };
    const auto cells = signalRosterBuildCells(
        tracked, latch, {}, false, nullptr,
        std::vector<std::string>({ "L1C", "L1W", "L2W", "L2L" }), true);
    ASSERT_TRUE(cells.count('2'));
    EXPECT_TRUE(cells.at('2').modelled_extra.empty());
}

TEST(SecondPhaseChannelTest, TheRosterAddsNothingWhenTheLatchedPhaseIsAbsentThisEpoch) {
    const std::vector<std::string> tracked{ "C1C", "L1W" };     // no L1C this epoch
    const std::vector<RosterLatchRow> latch{ { 'L', "L1C" }, { 'C', "C1C" } };
    EXPECT_TRUE(band1(tracked, latch, kDefaultList, true).modelled_extra.empty());
}

TEST(SecondPhaseChannelTest, TheRosterHonoursTheNetworkSignalAllowList) {
    const std::vector<std::string> tracked{ "C1C", "L1C", "L1W" };
    const std::vector<RosterLatchRow> latch{ { 'L', "L1C" }, { 'C', "C1C" } };
    const std::set<std::string> allowlist{ "C1C", "L1C" };      // L1W not admitted
    EXPECT_TRUE(band1(tracked, latch, kDefaultList, true, &allowlist).modelled_extra.empty());
}

TEST(SecondPhaseChannelTest, ASecondPhaseIsNotReportedAsAnExcludedSignal) {
    // The second phase is modelled, so it does not appear in the excluded list;
    // otherwise the roster's census would count the card's rows as unused.
    const std::vector<std::string> tracked{ "C1C", "L1C", "L1W" };
    const std::vector<RosterLatchRow> latch{ { 'L', "L1C" }, { 'C', "C1C" } };
    const BandRoster on = band1(tracked, latch, kDefaultList, true);
    for (const auto& [sig, why] : on.excluded) {
        (void)why;
        EXPECT_NE(sig, "L1W");
    }
    const BandRoster off = band1(tracked, latch, kDefaultList, false);
    bool found = false;
    for (const auto& [sig, why] : off.excluded) {
        (void)why;
        if (sig == "L1W") found = true;
    }
    EXPECT_TRUE(found);   // unarmed, it is an excluded tracked signal
}

// --------------------------------------------------------------------------
// (f) the loader contract
// --------------------------------------------------------------------------

TEST(SecondPhaseChannelTest, DefaultsToOffWithTheDefaultAllowList) {
    LogManager log;
    const fs::path p = writeTempConfig("default", networkConfig(""));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(cfg.networkSecondPhaseChannel());
    EXPECT_EQ(cfg.networkSecondPhaseSignals(), kDefaultList);
    EXPECT_DOUBLE_EQ(cfg.networkSecondPhaseSigmaScale(), 1.0);
    std::error_code ec; fs::remove(p, ec);
}

TEST(SecondPhaseChannelTest, ArmsWhenEveryInterlockIsSatisfied) {
    LogManager log;
    const fs::path p = writeTempConfig("armed", networkConfig(kArmable));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.networkSecondPhaseChannel());
    std::error_code ec; fs::remove(p, ec);
}

TEST(SecondPhaseChannelTest, IsDisarmedOnTheLevelingVehicle) {
    // A second phase per band on the leveling vehicle would create a zero-q level
    // for every extra arc, and no arm exists there (standing invariant #1).
    LogManager log;
    const fs::path p = writeTempConfig("level",
        networkConfig("    \"second_phase_channel\": true,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(cfg.networkSecondPhaseChannel());
    std::error_code ec; fs::remove(p, ec);
}

TEST(SecondPhaseChannelTest, IsDisarmedOnTheFloatVehicleWithoutTheHeldBasis) {
    LogManager log;
    const fs::path p = writeTempConfig("nohold",
        networkConfig("    \"second_phase_channel\": true,\n" + std::string(kVehicle)));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(cfg.networkSecondPhaseChannel());
    std::error_code ec; fs::remove(p, ec);
}

TEST(SecondPhaseChannelTest, IsSubsumedByChannelModeAllRatherThanRefused) {
    // channel_mode `all` subsumes this knob instead of disarming it (card SID phase
    // 6b): the widening adds every admitted phase attribute of every selected band,
    // which contains the allow-listed band-1 set, so the knob stays armed, its
    // allow-list is ignored and only its sigma scale is read.  Downstream sites test
    // the resolved row predicate.
    LogManager log;
    const fs::path p = writeTempConfig("chanall",
        networkConfig(kArmable, "    \"channel_mode\": \"all\",\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.networkChannelModeAll());
    EXPECT_TRUE(cfg.networkSecondPhaseChannel());
    EXPECT_TRUE(cfg.networkSecondPhaseRowsActive());
    std::error_code ec; fs::remove(p, ec);
}

TEST(SecondPhaseChannelTest, DropsARefusedAllowListEntryAndKeepsTheRest) {
    LogManager log;
    const fs::path p = writeTempConfig("list",
        networkConfig(std::string(kArmable) +
                      "    \"second_phase_signals\": [ \"L1C\", \"L2W\", \"C1C\", \"L1W\" ],\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_EQ(cfg.networkSecondPhaseSignals(), kDefaultList);   // L2W and C1C dropped
    EXPECT_TRUE(cfg.networkSecondPhaseChannel());
    std::error_code ec; fs::remove(p, ec);
}

TEST(SecondPhaseChannelTest, AListWithFewerThanTwoAdmittedPhasesDisarmsTheCard) {
    LogManager log;
    const fs::path p = writeTempConfig("thin",
        networkConfig(std::string(kArmable) +
                      "    \"second_phase_signals\": [ \"L1C\", \"L2W\" ],\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_EQ(cfg.networkSecondPhaseSignals(), std::vector<std::string>({ "L1C" }));
    EXPECT_FALSE(cfg.networkSecondPhaseChannel());
    std::error_code ec; fs::remove(p, ec);
}

TEST(SecondPhaseChannelTest, AdmitsAnExplicitlyListedExtraAttribute) {
    LogManager log;
    const fs::path p = writeTempConfig("x",
        networkConfig(std::string(kArmable) +
                      "    \"second_phase_signals\": [ \"L1C\", \"L1W\", \"L1X\" ],\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_EQ(cfg.networkSecondPhaseSignals(),
              std::vector<std::string>({ "L1C", "L1W", "L1X" }));
    std::error_code ec; fs::remove(p, ec);
}

TEST(SecondPhaseChannelTest, RefusesASigmaScaleBelowOne) {
    LogManager log;
    const fs::path p = writeTempConfig("lowscale",
        networkConfig(std::string(kArmable) +
                      "    \"second_phase_sigma_scale\": 0.25,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_DOUBLE_EQ(cfg.networkSecondPhaseSigmaScale(), 1.0);
    std::error_code ec; fs::remove(p, ec);
}

TEST(SecondPhaseChannelTest, ParsesALegalSigmaScale) {
    LogManager log;
    const fs::path p = writeTempConfig("scale3",
        networkConfig(std::string(kArmable) +
                      "    \"second_phase_sigma_scale\": 3.0,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_DOUBLE_EQ(cfg.networkSecondPhaseSigmaScale(), 3.0);
    std::error_code ec; fs::remove(p, ec);
}

TEST(SecondPhaseChannelTest, WarnsRatherThanRefusesWithoutThePerSignalGapTier) {
    // The per-signal gap re-entry tier is the only row-level response to the
    // re-entry of a second-phase arc, but its absence is a warning, not a refusal:
    // the knob stays armed and the run-end readout restates the warning (R6).
    LogManager log;
    const fs::path p = writeTempConfig("noper", networkConfig(kArmable));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.networkSecondPhaseChannel());
    EXPECT_TRUE(cfg.getNetworkConfig().second_phase_warn_no_per_signal);
    std::error_code ec; fs::remove(p, ec);
}

TEST(SecondPhaseChannelTest, DoesNotWarnWhenThePerSignalGapTierIsArmed) {
    LogManager log;
    const fs::path p = writeTempConfig("per",
        networkConfig(std::string(kArmable) +
                      "    \"cycle_slip\": { \"gap_reentry\": true,\n"
                      "                      \"gap_reentry_relevel\": true,\n"
                      "                      \"gap_reentry_per_signal\": true },\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.networkSecondPhaseChannel());
    EXPECT_FALSE(cfg.getNetworkConfig().second_phase_warn_no_per_signal);
    std::error_code ec; fs::remove(p, ec);
}

TEST(SecondPhaseChannelTest, WarnsWhenTheIfTdTierCannotFormANonDegeneratePair) {
    // Two band-1 phases share a frequency, so with if_phase_td_pairs other than
    // "all" that tier's single pair is degenerate and produces no rows (R4).
    LogManager log;
    const fs::path p = writeTempConfig("iftd",
        networkConfig(std::string(kArmable) +
                      "    \"cycle_slip\": { \"if_phase_td\": true },\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.networkSecondPhaseChannel());
    EXPECT_TRUE(cfg.getNetworkConfig().second_phase_warn_iftd_pairs);
    std::error_code ec; fs::remove(p, ec);
}

TEST(SecondPhaseChannelTest, TheClosedLoopConsumerIsPinnedOff) {
    // The card is a producer-side mechanism: it exists so the satellite families
    // share the network's stations, and a consumer is one station with no family
    // to support (R6).
    LogManager log;
    const fs::path p = writeTempConfig("consumer",
        networkConfig(std::string(kArmable) +
                      "    \"second_phase_sigma_scale\": 3.0,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    ASSERT_TRUE(cfg.networkSecondPhaseChannel());
    const GnssConfig derived = cfg.deriveClosedLoopConsumerConfig();
    EXPECT_FALSE(derived.networkSecondPhaseChannel());
    EXPECT_DOUBLE_EQ(derived.networkSecondPhaseSigmaScale(), 1.0);
    std::error_code ec; fs::remove(p, ec);
}
