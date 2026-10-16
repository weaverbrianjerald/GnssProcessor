/**
 * @file test_n3c_switches.cpp
 * @brief The signal-role switches (network.n3c_*): each site's answer with its switch on,
 *        and the default (switch off) answer unchanged.  Suite N3cSwitchesTest.
 */
#include <gtest/gtest.h>

#include "filter/StateKeySchema.hpp"
#include "network/solve/DatumGraph.hpp"
#include "common/Constants.hpp"
#include "common/PppObservation.hpp"
#include "common/PppUtils.hpp"
#include "logging/LogManager.hpp"
#include "network/assembly/CanonicalPairAdmission.hpp"
#include "network/assembly/SignalRoster.hpp"
#include "network/emit/SolPrep.hpp"
#include "network/slip/RawObservables.hpp"
#include "network/solve/Sbasis.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {
namespace {

using Pair = std::pair<std::string, std::string>;

/// GPS carrier frequency by RINEX band digit; an unknown band reads as unavailable.
double gpsFreq(const std::string& s) {
    if (s.size() < 2) return 0.0;
    switch (s[1]) {
        case '1': return 1575.42e6;
        case '2': return 1227.60e6;
        case '5': return 1176.45e6;
        default:  return 0.0;
    }
}

// ---- D1: the pin pair is the bias product's clock-reference pair ----

TEST(N3cSwitchesTest, PinPairFollowsTheReferencePairWhenBothSignalsAreRegistered) {
    const std::vector<std::string> sigs{ "C1C", "C1W", "C2W" };
    // Switch off (no reference): the lexically first pair.
    EXPECT_EQ(NetworkSbasis::selectIfPinPair(sigs, gpsFreq), (Pair{ "C1C", "C2W" }));
    // Switch on: the clock-reference pair, whatever the input order.
    const Pair ref{ "C1W", "C2W" };
    EXPECT_EQ(NetworkSbasis::selectIfPinPair(sigs, gpsFreq, ref), ref);
    EXPECT_EQ(NetworkSbasis::selectIfPinPair({ "C2W", "C1W", "C1C" }, gpsFreq, ref), ref);
}

TEST(N3cSwitchesTest, PinPairKeepsTheLexicalChoiceWhereTheReferenceCannotBeFormed) {
    const Pair ref{ "C1W", "C2W" };
    // The satellite registers no C1W: the reference pair is not available.
    EXPECT_EQ(NetworkSbasis::selectIfPinPair({ "C1C", "C2W" }, gpsFreq, ref),
              (Pair{ "C1C", "C2W" }));
    // Both reference signals on one frequency (a degenerate reference): ignored.
    EXPECT_EQ(NetworkSbasis::selectIfPinPair({ "C1C", "C1W", "C2W" }, gpsFreq,
                                             Pair{ "C1C", "C1W" }),
              (Pair{ "C1C", "C2W" }));
    // An empty reference is the switch off.
    EXPECT_EQ(NetworkSbasis::selectIfPinPair({ "C1C", "C1W", "C2W" }, gpsFreq, Pair{}),
              (Pair{ "C1C", "C2W" }));
}

// ---- D2: one channel-family definition, "X against the rest" ----

TEST(N3cSwitchesTest, ChannelFamilyIsXAgainstTheRestWhenTheSwitchIsOn) {
    // Default (the attribute letter): C1C and C5Q are two families on Galileo.
    EXPECT_NE(netDatumGraphFamilyOf("E03", "C1C", false), netDatumGraphFamilyOf("E03", "C5Q", false));
    EXPECT_EQ(netDatumGraphFamilyOf("E03", "C1C", false), netDatumGraphFamily("C1C"));
    // Switch on: they are one family, and the combined I+Q signals are the other.
    EXPECT_EQ(netDatumGraphFamilyOf("E03", "C1C", true), netDatumGraphFamilyOf("E03", "C5Q", true));
    EXPECT_EQ(netDatumGraphFamilyOf("E03", "C1C", true), kDatumGraphFamilyRest);
    EXPECT_EQ(netDatumGraphFamilyOf("E03", "L5X", true), kDatumGraphFamilyX);
    // GPS C1C and C2W are one family by the rule, two by the letter.
    EXPECT_NE(netDatumGraphFamilyOf("G05", "C1C", false), netDatumGraphFamilyOf("G05", "C2W", false));
    EXPECT_EQ(netDatumGraphFamilyOf("G05", "C1C", true), netDatumGraphFamilyOf("G05", "C2W", true));
    // A code with no attribute has no family under either definition.
    EXPECT_EQ(netDatumGraphFamilyOf("E03", "C1", true), kDatumGraphNoFamily);
}

TEST(N3cSwitchesTest, ChannelFamilyCellsAndDirectionIdsFollowTheDefinition) {
    const std::set<std::string> pb{ StateKeys::makeSatPhaseBiasKey("E03", "L1C"),
                                 StateKeys::makeSatPhaseBiasKey("E03", "L5Q") };
    const std::vector<std::pair<std::string, std::string>> cb{ { "E03", "C1C" }, { "E03", "C5Q" } };
    // The attribute letter: two cells (C and Q). The rule: one cell.
    EXPECT_EQ(netDatumGraphChannelFamilies(pb, cb, {}).size(), 2u);
    const auto cells = netDatumGraphChannelFamilies(pb, cb, {}, true);
    ASSERT_EQ(cells.size(), 1u);
    EXPECT_EQ(cells[0].family, kDatumGraphFamilyRest);
    EXPECT_EQ(cells[0].n_pb, 2);
    // A direction's identities name the family by the same definition.
    const auto ids = netDatumGraphDetailIds("PB:E03:L5X:0.5", true);
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], std::string("E03/") + kDatumGraphFamilyX);
}

// ---- D3: the canonical pair's bands come from the constellation traits ----

TEST(N3cSwitchesTest, CanonicalPairSlotsFollowTheTraitsWhenTheSwitchIsOn) {
    // Default: the digits 1 and 2 for every constellation, so Galileo band 5 fills no slot.
    EXPECT_EQ(canonicalPairSlot(System::GPS, '1', false), 1);
    EXPECT_EQ(canonicalPairSlot(System::GPS, '2', false), 2);
    EXPECT_EQ(canonicalPairSlot(System::Galileo, '5', false), 0);
    EXPECT_EQ(canonicalPairSlot(System::Galileo, '1', false), 1);
    // Switch on: GPS is unchanged, Galileo's pair is (1, 5).
    EXPECT_EQ(canonicalPairSlot(System::GPS, '1', true), 1);
    EXPECT_EQ(canonicalPairSlot(System::GPS, '2', true), 2);
    EXPECT_EQ(canonicalPairSlot(System::GPS, '5', true), 0);
    EXPECT_EQ(canonicalPairSlot(System::Galileo, '1', true), 1);
    EXPECT_EQ(canonicalPairSlot(System::Galileo, '5', true), 2);
    EXPECT_EQ(canonicalPairSlot(System::Galileo, '7', true), 0);
}

// ---- D4: the tracker's band signal is the Modelled role; lexical order never decides ----

/// One GPS satellite's tracker row for the given phase signals, each reported with its code
/// unless it appears in @p phase_only, under a lookup that names @p latch per band digit.
const RawSlipObs* trackerRow(RawSlipEpoch& epoch, PppUtils& utils,
                             const std::vector<std::string>& phases,
                             const std::vector<std::string>& phase_only,
                             const std::map<char, std::string>& latch, bool role_only) {
    PppObservation obs{};
    obs.satellite = "G05";
    obs.system    = System::GPS;
    obs.elevation = 45.0;
    for (const std::string& ph : phases) {
        obs.raw_observed[ph] = 115000000.5;
        if (std::find(phase_only.begin(), phase_only.end(), ph) == phase_only.end())
            obs.raw_observed["C" + ph.substr(1)] = 22000000.0;
    }
    const std::vector<std::vector<PppObservation>> modeled{ { obs } };
    epoch.reset(1);
    const TrackerLatchLookup lookup =
        [&latch](int, const std::string&, char band) -> std::string {
            const auto it = latch.find(band);
            return it == latch.end() ? std::string{} : it->second;
        };
    networkComputeRawObservables(modeled, { true }, 1, utils, epoch, false, true, nullptr,
                                 &lookup, role_only);
    return epoch.rows.at(0).empty() ? nullptr : &epoch.rows.at(0).front();
}

TEST(N3cSwitchesTest, TrackerLeavesAnUnusableModelledBandOutInsteadOfTheLexicalPick) {
    LogManager log_manager;
    PppUtils utils{ log_manager };
    RawSlipEpoch epoch;
    // Band 1 is modelled as L1W, which is reported without its code; L1C is complete.
    const std::vector<std::string> phases{ "L1C", "L1W", "L2W" };
    const std::map<char, std::string> latch{ { '1', "L1W" }, { '2', "L2W" } };
    // Default: the lexical pick (L1C) stands in for the unusable L1W.
    const RawSlipObs* off = trackerRow(epoch, utils, phases, { "L1W" }, latch, false);
    ASSERT_NE(off, nullptr);
    EXPECT_EQ(off->sig1, "L1C");
    EXPECT_EQ(off->sig2, "L2W");
    // Switch on: the lowest band has no tracker signal, so the satellite has no row.
    EXPECT_EQ(trackerRow(epoch, utils, phases, { "L1W" }, latch, true), nullptr);
    // The second band unusable: a single-band row on the first band's Modelled signal.
    const std::map<char, std::string> latch2{ { '1', "L1C" }, { '2', "L2X" } };
    const RawSlipObs* two = trackerRow(epoch, utils, { "L1C", "L2W", "L2X" }, { "L2X" },
                                       latch2, true);
    ASSERT_NE(two, nullptr);
    EXPECT_EQ(two->sig1, "L1C");
    EXPECT_TRUE(two->sig2.empty());
    EXPECT_FALSE(two->dual);
    // A band the lookup does not name keeps the lexical pick under the switch.
    const RawSlipObs* kept = trackerRow(epoch, utils, phases, { "L1W" }, { { '2', "L2W" } },
                                        true);
    ASSERT_NE(kept, nullptr);
    EXPECT_EQ(kept->sig1, "L1C");
}

// ---- D6: the wire carries one bias per modelled signal ----

/// A roster whose one station models the given phase and code on band 1 of @p cons.
SignalRoster oneStationRoster(const std::string& cons, const std::string& phase,
                              const std::string& code) {
    SignalRoster rs;
    rs.by_station.resize(1);
    BandRoster& cell = rs.by_station[0][cons]['1'];
    cell.modelled_phase = phase;
    cell.modelled_code  = code;
    return rs;
}

TEST(N3cSwitchesTest, WireFillCopiesTheSameBandSignalOfTheSameFamily) {
    // The network models L1W / C1W on GPS; the satellite has estimated L1C / C1C only.
    const SignalRoster rs = oneStationRoster("GPS", "L1W", "C1W");
    NetworkSolutionEpoch sol;
    NetworkSatSolution s;
    s.sat = "G05";
    s.has_clock = true;
    s.phase_bias["L1C"].bias_m = 0.25;
    s.phase_bias["L1C"].sigma_m = 0.01;
    s.code_bias_m["C1C"] = 1.5;
    sol.sats.push_back(s);
    const NetworkWireFillCounts f = networkWireFillModelledSignals(sol, rs);
    EXPECT_EQ(f.phase, 1);
    EXPECT_EQ(f.code, 1);
    EXPECT_EQ(f.sats, 1);
    const NetworkSatSolution& r = sol.sats.front();
    ASSERT_EQ(r.phase_bias.count("L1W"), 1u);
    EXPECT_DOUBLE_EQ(r.phase_bias.at("L1W").bias_m, 0.25);
    EXPECT_DOUBLE_EQ(r.phase_bias.at("L1W").sigma_m, 0.01);
    EXPECT_DOUBLE_EQ(r.code_bias_m.at("C1W"), 1.5);
    // The estimated records stand, and the pass is idempotent.
    EXPECT_EQ(r.phase_bias.size(), 2u);
    EXPECT_EQ(networkWireFillModelledSignals(sol, rs).phase, 0);
}

TEST(N3cSwitchesTest, WireFillNeverCrossesFamiliesBandsOrConstellations) {
    // Galileo models the combined I+Q signals (X); the satellite has the pilot signals only.
    SignalRoster rs = oneStationRoster("Galileo", "L1X", "C1X");
    NetworkSolutionEpoch sol;
    NetworkSatSolution e;
    e.sat = "E03";
    e.has_clock = true;
    e.phase_bias["L1C"].bias_m = 0.4;
    e.code_bias_m["C1C"] = 2.0;
    e.code_bias_m["C5Q"] = 3.0;
    sol.sats.push_back(e);
    // A GPS satellite: the Galileo roster names nothing for it.
    NetworkSatSolution g;
    g.sat = "G05";
    g.has_clock = true;
    g.phase_bias["L1C"].bias_m = 0.1;
    sol.sats.push_back(g);
    // A satellite without a clock record gets nothing.
    NetworkSatSolution n;
    n.sat = "E04";
    n.phase_bias["L1C"].bias_m = 0.2;
    sol.sats.push_back(n);
    const NetworkWireFillCounts f = networkWireFillModelledSignals(sol, rs);
    EXPECT_EQ(f.phase + f.code, 0);
    EXPECT_EQ(sol.sats[0].phase_bias.count("L1X"), 0u);   // X is another family than C
    EXPECT_EQ(sol.sats[0].code_bias_m.count("C1X"), 0u);
    EXPECT_EQ(sol.sats[1].phase_bias.size(), 1u);
    EXPECT_EQ(sol.sats[2].phase_bias.size(), 1u);
}

}  // namespace
}  // namespace GnssProcessor
