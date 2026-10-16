/**
 * @file test_phase_gate_drop_edge.cpp
 * @brief Coverage-gate drop-edge predicate
 *        (@ref GnssProcessor::networkPhaseGateDropEdges,
 *        network/solve/NetworkPhaseGateDrop.hpp) and the knob that gates its response
 *        (@c network.phase_datum_gate_drop_erase).  Card EV7 §6, requirement F3.
 *
 * The defect produces no signal at the site where it happens
 * (docs/refound/EV7_all_sat_clock_decouple_2026-09-21.md §5.3).  The coverage gate
 * removes a (satellite, signal) phase-bias column when it thins to one tracker, on
 * the assumption that each station's float ambiguity absorbs the bias.  On the float
 * vehicle that float is a carried state, so it absorbs the whole removed bias in one
 * epoch against a centimetre prior, with no refloat, no re-seed and no variance
 * inflation.  Nothing errors and no counter moves; the visible symptom is satellite
 * clocks tripping the Q3 anchor gate in that epoch.
 *
 * The rule is one line of set arithmetic.  Each property below is one the run loop
 * cannot state any other way:
 *
 *   1. an edge is a gated key that held an incumbent; the second clause separates
 *      the one epoch where a mature carry is still defined against the departing
 *      column from the rest of the outage;
 *   2. a gated key with no incumbent is not an edge, which makes the response fire
 *      once: the election never elects a gated key, so from the epoch after the
 *      edge the incumbent map no longer names it (requirement F1's "at the edge
 *      only", shown here as a two-epoch sequence);
 *   3. an incumbent that was not gated is not an edge either: that family is alive
 *      and its carry is still in frame;
 *   4. both empty cases short-circuit to the empty set, which is the whole cost of
 *      the predicate on an epoch with no edge (nearly every epoch);
 *   5. the knob defaults to off and survives a round trip through the loader,
 *      because an arm that names a knob the loader does not parse is silently null.
 *
 * Not asserted here: that the erase happens.  That is the existing
 * @c networkApplyPhaseAnchorCarryReref path, covered by its own suite; this card
 * adds which families enter it and when.
 */
#include <gtest/gtest.h>

#include "network/solve/NetworkPhaseGateDrop.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>

using GnssProcessor::GnssConfig;
using GnssProcessor::LogManager;
using GnssProcessor::networkPhaseGateDropEdges;

namespace fs = std::filesystem;

namespace {

/// The previous epoch's anchor map, in the election's own encoding:
/// ambiguity key "<sat>_<signal>" -> anchoring station name.
std::map<std::string, std::string> incumbents() {
    return {{"G30_L1W", "VOIM00MDG"},
            {"G30_L2W", "SGOC00LKA"},
            {"G21_L1W", "KOKV00USA"},
            {"G19_L1W", "URUM00CHN"}};
}

/// A minimal but loadable ppp-rtk-network config; @p extra is spliced into the
/// `network` object ahead of the station roster (the established pattern of
/// tests/network/test_amb_float_vehicle.cpp).
std::string networkConfig(const std::string& extra) {
    return std::string("{\n")
        + "  \"processing\": {\n"
          "    \"mode\": \"ppp-rtk-network\",\n"
          "    \"active_systems\": [ \"GPS\" ],\n"
          "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
          "  },\n"
          "  \"network\": {\n"
          "    \"clock_datum_station\": \"AAAA\",\n"
        + extra +
          "    \"stations\": [\n"
          "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
          "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
          "    ]\n"
          "  }\n"
          "}\n";
}

bool loadInto(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    const fs::path p =
        fs::temp_directory_path() / ("ev7_gatedrop_cfg_" + tag + ".json");
    { std::ofstream f(p); f << body; }
    LogManager log;
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

} // namespace

// 1. The edge rule: gated and previously anchored, nothing else.
TEST(PhaseGateDropEdgeTest, EdgeIsAGatedKeyThatHeldAnIncumbent) {
    const std::set<std::string> gated{"G30_L1W", "G07_L1W"};
    const auto edges = networkPhaseGateDropEdges(gated, incumbents());
    EXPECT_EQ(edges.size(), 1u);
    EXPECT_EQ(edges.count("G30_L1W"), 1u);
    // G07_L1W was gated but never anchored: a column that thins before it has ever
    // realized a datum has no carried level defined against it to strand.
    EXPECT_EQ(edges.count("G07_L1W"), 0u);
}

// 1b. Several families can leave in one epoch, and only the anchored ones count.
TEST(PhaseGateDropEdgeTest, ReportsEveryAnchoredGatedKeyInOneEpoch) {
    const std::set<std::string> gated{"G30_L1W", "G30_L2W", "G21_L1W", "G07_L5Q"};
    const auto edges = networkPhaseGateDropEdges(gated, incumbents());
    EXPECT_EQ(edges.size(), 3u);
    EXPECT_EQ(edges.count("G30_L1W"), 1u);
    EXPECT_EQ(edges.count("G30_L2W"), 1u);
    EXPECT_EQ(edges.count("G21_L1W"), 1u);
    EXPECT_EQ(edges.count("G07_L5Q"), 0u);
}

// 2. An incumbent that is not gated is not an edge: the family is alive.
TEST(PhaseGateDropEdgeTest, LiveAnchoredFamilyIsNotAnEdge) {
    EXPECT_TRUE(networkPhaseGateDropEdges({}, incumbents()).empty());
    const std::set<std::string> gated{"G05_L1W"};   // gated, never anchored
    EXPECT_TRUE(networkPhaseGateDropEdges(gated, incumbents()).empty());
}

// 3. The edge fires once.  Epoch T: the key is gated and still anchored -> edge.
//    The election never elects a gated key (the gate erases it from union_keys
//    before the election's loop), so the swap at the end of T drops it from the
//    incumbent map; at T+1 the same key is still gated and is no longer an edge.
//    This is requirement F1's "the re-born ambiguity absorbs the bias once and is
//    carried normally afterwards", stated as a sequence.
TEST(PhaseGateDropEdgeTest, EdgeFiresOnceThenTheOutageIsSilent) {
    const std::set<std::string> gated{"G30_L1W"};
    std::map<std::string, std::string> incumbent = incumbents();

    const auto at_t = networkPhaseGateDropEdges(gated, incumbent);
    EXPECT_EQ(at_t.size(), 1u);

    // The election's incumbent swap: the new map is built from the keys it elected,
    // and a gated key is not among them.
    std::map<std::string, std::string> new_incumbent;
    for (const auto& [amb, sta] : incumbent)
        if (!gated.count(amb)) new_incumbent[amb] = sta;
    incumbent.swap(new_incumbent);

    for (int ep = 0; ep < 3; ++ep)
        EXPECT_TRUE(networkPhaseGateDropEdges(gated, incumbent).empty())
            << "the drop edge re-fired " << ep + 1 << " epoch(s) into the outage";

    // The family's return is an existing edge: it re-enters the union with no
    // incumbent, which the election already marks (incumbent-less pin).
    EXPECT_EQ(incumbent.count("G30_L1W"), 0u);
}

// 4. Both empty cases short-circuit.  This is the whole cost on a quiet epoch.
TEST(PhaseGateDropEdgeTest, EmptyInputsGiveAnEmptyDropSet) {
    EXPECT_TRUE(networkPhaseGateDropEdges({}, {}).empty());
    EXPECT_TRUE(networkPhaseGateDropEdges({"G30_L1W"}, {}).empty());
    EXPECT_TRUE(networkPhaseGateDropEdges({}, incumbents()).empty());
}

// 5. The knob: default off, and parsed under its documented JSON name.  An arm that
//    names a knob the loader ignores is a null arm that reads as a refutation.
TEST(PhaseGateDropEdgeTest, KnobDefaultsOffAndLoadsFromJson) {
    GnssConfig def;
    ASSERT_TRUE(loadInto("default", networkConfig(""), def));
    EXPECT_FALSE(def.networkPhaseDatumGateDropErase());

    GnssConfig cfg;
    ASSERT_TRUE(loadInto("armed",
                         networkConfig("    \"phase_datum_prioritized\": true,\n"
                                       "    \"phase_datum_min_stations\": 2,\n"
                                       "    \"phase_anchor_carry_reref\": true,\n"
                                       "    \"phase_datum_gate_drop_erase\": true,\n"),
                         cfg));
    EXPECT_TRUE(cfg.networkPhaseDatumGateDropErase());
    // The two knobs it depends on are untouched by it (it neither arms nor disarms
    // them; the loader only warns when the combination is inert).
    EXPECT_EQ(cfg.networkPhaseDatumMinStations(), 2);
    EXPECT_TRUE(cfg.networkPhaseAnchorCarryReref());
}
