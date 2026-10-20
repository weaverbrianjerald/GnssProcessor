/**
 * @file test_station_params.cpp
 * @brief The station parameter set of network/state/StationParams.hpp: its layout, its typed
 *        maps, its writers (births, prunes, marks, the split's settlement), the lookups and
 *        labels the readers rely on, and the births and shared-parameter admission that write it
 *        from observations (network/assembly/LocalBirth.hpp, SharedAdmission.hpp).
 *
 * The fixture is a station over GPS, Galileo and GLONASS whose columns are born in an order
 * unlike the key order, so a map that iterated in birth order instead of key order would fail.
 * Contracts:
 *  - a new station holds the receiver position (held at the roster coordinate), the clock, the
 *    wet delay and one inter-system bias per active non-GPS constellation, in that column order;
 *  - the set has one slot per column in birth order, and every typed lookup finds its slot;
 *  - every typed map iterates in the order of the state-key texts of its family;
 *  - a prune removes the parameters outside a keep set, keeps the survivors in column order,
 *    renumbers every map, never removes a core column and records the removed ambiguities;
 *  - a re-seed mark is the set's: it outlives a column, and is not a property of a slot;
 *  - the split's settlement gives each slot its disposition, keeps a withheld parameter exactly
 *    while the column that takes it up is held, and lists an identity no constellation carries;
 *  - the inventory recorded from the set lists every named column once, with the settled
 *    disposition, and counts a column past the split's vector;
 *  - the key texts and state labels the traces print are the state-key formats;
 *  - the strings and predicates the carried-prior steps take from a typed entry are the ones
 *    the key parsers give;
 *  - the SD-EWL candidates and the trace labels read from the set;
 *  - the births give a station the parameters its observations imply, in the order the Schur
 *    kernel factorizes them, and the shared-parameter admission applies its gates;
 *  - a step of the station build that throws leaves the births it made in the set.
 */

#include <gtest/gtest.h>

#include "atmosphere/GlobalIonosphereMap.hpp"
#include "common/GnssIdentity.hpp"
#include "common/ParamRef.hpp"
#include "config/GnssConfig.hpp"
#include "filter/StateKeySchema.hpp"
#include "logging/LogManager.hpp"
#include "measurement/UserParamView.hpp"
#include "network/ar/SdEwlRows.hpp"
#include "network/assembly/LocalBirth.hpp"
#include "network/assembly/ParamInventory.hpp"
#include "network/assembly/SharedAdmission.hpp"
#include "network/assembly/StationBuild.hpp"
#include "network/assembly/StationRows.hpp"
#include "network/diag/StationBlock.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/qc/RedundancyCensus.hpp"
#include "network/solve/Estimator.hpp"
#include "network/state/Entities.hpp"
#include "network/state/StationParams.hpp"
#include "../support/ModelStack.hpp"
#include "../support/NetworkTestRun.hpp"
#include "../support/StationBirthFixture.hpp"
#include "../support/StationParamsBuilder.hpp"
#include "../support/TestTempDir.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace GnssProcessor;
namespace K  = GnssProcessor::StateKeys;
namespace fs = std::filesystem;
using gnss_test::BirthFixture;
using gnss_test::bornAmbiguity;
using gnss_test::bornPhaseColour;
using gnss_test::bornRxCodeBias;
using gnss_test::bornRxPhaseBias;
using gnss_test::bornSatClock;
using gnss_test::bornSatCodeBias;
using gnss_test::bornSatPhaseBias;
using gnss_test::bornSlantIono;
using gnss_test::dcbKey;
using gnss_test::ifbKey;
using gnss_test::iscKey;
using gnss_test::slotIfbKey;

namespace {

using PD = ParamDisposition;

/// A network config over GPS, Galileo and GLONASS, so a station has the inter-system biases of
/// the two non-GPS systems.
void loadThreeSystemConfig(GnssConfig& cfg, LogManager& log) {
    static const unsigned run_id = std::random_device{}();
    static std::atomic<unsigned> n{0};
    const fs::path p = gnss_test::tempDir()
                     / ("station_params_cfg_" + std::to_string(run_id) + "_" + std::to_string(n++) + ".json");
    {
        std::ofstream f(p);
        f << "{\n"
             "  \"processing\": {\n"
             "    \"mode\": \"ppp-rtk-network\",\n"
             "    \"active_systems\": [ \"GPS\", \"Galileo\", \"GLONASS\" ],\n"
             "    \"signal_priorities\": { \"GPS\": \"WC\", \"Galileo\": \"QX\", \"GLONASS\": \"PC\" }\n"
             "  },\n"
             "  \"network\": {\n"
             "    \"clock_datum_station\": \"AAAA\",\n"
             "    \"stations\": [\n"
             "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
             "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
             "    ]\n"
             "  }\n"
             "}\n";
    }
    EXPECT_TRUE(cfg.loadFromJson(p, log));
    std::error_code ec;
    fs::remove(p, ec);
}

/// A station-shaped set and the split's dispositions for it.
struct Station {
    LogManager                    lm;
    GnssConfig                    cfg;
    StationParams                 set;
    std::vector<ParamDisposition> disp;

    Station() {
        loadThreeSystemConfig(cfg, lm);
        set = startStationParams(cfg, Eigen::Vector3d(-2493304.99, -4655214.79, 3565497.67));
        // Columns in a birth order that is not the key order: later satellites and systems
        // first, signals reversed.
        double v = 0.25;
        auto next = [&] { return v += 0.375; };
        double elevation = 12.5;
        for (const char* sat : {"R07", "E24", "G12", "G05", "E11", "R02"}) {
            bornSlantIono(set, sat, next());
            bornPhaseColour(set, sat, elevation += 6.25);
        }
        for (const char* sat : {"G12", "E24", "G05", "E11", "R07", "R02"}) {
            const std::string s = sat;
            if (s[0] == 'G') {
                bornAmbiguity(set, s, "L2W", next());
                bornAmbiguity(set, s, "L1C", next());
            } else if (s[0] == 'E') {
                bornAmbiguity(set, s, "L5Q", next());
                bornAmbiguity(set, s, "L1C", next());
            } else {
                bornAmbiguity(set, s, "L2C", next());
                bornAmbiguity(set, s, "L1C", next());
            }
        }
        bornRxCodeBias(set, iscKey(System::GPS, "C1W"), next());
        bornRxCodeBias(set, slotIfbKey(System::GLONASS, '2', -4), next());
        bornRxCodeBias(set, slotIfbKey(System::GLONASS, '2', 5), next());
        bornRxCodeBias(set, ifbKey(System::GPS, '5'), next());
        bornRxCodeBias(set, dcbKey(System::Galileo), next());
        bornRxCodeBias(set, dcbKey(System::GPS), next());
        bornRxPhaseBias(set, System::Galileo, "L1C", next());
        bornRxPhaseBias(set, System::GPS, "L2W", next());
        bornRxPhaseBias(set, System::GPS, "L1C", next());
        for (const char* sat : {"E24", "G12", "G05", "E11"}) {
            const bool galileo = sat[0] == 'E';
            bornSatClock(set, sat, next());
            bornSatPhaseBias(set, sat, galileo ? "L5Q" : "L2W", next());
            bornSatPhaseBias(set, sat, "L1C", next());
            bornSatCodeBias(set, sat, galileo ? "C5Q" : "C2W", next());
            bornSatCodeBias(set, sat, "C1C", next());
        }
        // The split's decisions: position pinned, the rest cycling through the other
        // dispositions so that a copied value is visible.
        disp.assign(set.slots.size(), PD::EstimatedLocal);
        for (std::size_t k = 0; k < disp.size(); ++k)
            disp[k] = static_cast<PD>((k * 5 + 1) % kParamDispositions);
        for (std::size_t k = 0; k < 3; ++k) disp[static_cast<std::size_t>(set.position[k])] = PD::Pinned;
    }

    /// The inventory's record of the station, as the split's seam writes it.
    StationParamSlot inventory(int station = 4) {
        settleStationParams(set, disp);
        StationParamSlot slot;
        recordStationParams(set, station, slot);
        return slot;
    }
};

/// The sequence of slot indices a typed map visits.
template <class Map>
std::vector<int> visited(const Map& m) {
    std::vector<int> out;
    for (const auto& kv : m) out.push_back(kv.second);
    return out;
}

/// The key texts of a typed map, in the order the map visits them.
template <class Map>
std::vector<std::string> keyTexts(const StationParams& set, const Map& m) {
    std::vector<std::string> out;
    for (const auto& kv : m) out.push_back(keyText(slotAt(set, kv.second).ref));
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// The layout and the typed maps
// ---------------------------------------------------------------------------

TEST(StationParamsTest, ANewStationHoldsPositionClockWetDelayAndOneBiasPerNonGpsSystem) {
    LogManager lm;
    GnssConfig cfg;
    loadThreeSystemConfig(cfg, lm);
    const StationParams set = startStationParams(cfg, Eigen::Vector3d(1.5, -2.5, 3.5));
    ASSERT_EQ(set.slots.size(), 7u);   // 3 position, clock, wet delay, Galileo and GLONASS biases
    for (int axis = 0; axis < 3; ++axis) {
        EXPECT_EQ(set.position[static_cast<std::size_t>(axis)], axis);
        EXPECT_EQ(set.slots[static_cast<std::size_t>(axis)].ref, rxPositionRef(static_cast<std::uint8_t>(axis)));
    }
    EXPECT_EQ(set.slots[0].x_lin, 1.5);
    EXPECT_EQ(set.slots[1].x_lin, -2.5);
    EXPECT_EQ(set.slots[2].x_lin, 3.5);
    EXPECT_EQ(set.clock, 3);
    EXPECT_EQ(set.zwd, 4);
    // The inter-system biases follow in the order of the configuration's list.
    EXPECT_EQ(set.isb.at(System::Galileo), 5);
    EXPECT_EQ(set.isb.at(System::GLONASS), 6);
    for (std::size_t k = 3; k < set.slots.size(); ++k) EXPECT_EQ(set.slots[k].x_lin, 0.0) << k;
    EXPECT_EQ(set.untyped, 0);
    EXPECT_EQ(set.decided, 0u);
}

TEST(StationParamsTest, TheSetHasOneSlotPerColumnInBirthOrderAndEveryLookupFindsIt) {
    Station st;
    const StationParams& set = st.set;
    EXPECT_TRUE(set.unclassified.empty());
    EXPECT_EQ(set.untyped, 0);
    for (std::size_t k = 0; k < set.slots.size(); ++k)
        EXPECT_EQ(findParam(set, set.slots[k].ref), static_cast<int>(k)) << describe(set.slots[k].ref);

    // The fixture's own counts: the 7 core slots, then the families in the order they were born.
    EXPECT_EQ(set.isb.size(), 2u);   // Galileo and GLONASS
    EXPECT_EQ(set.iono.size(), 6u);
    EXPECT_EQ(set.colour.size(), 6u);
    EXPECT_EQ(set.amb.size(), 12u);
    EXPECT_EQ(set.rx_pb.size(), 3u);
    EXPECT_EQ(set.rx_cb.size(), 6u);
    EXPECT_EQ(set.shared.sat_clock.size(), 4u);
    EXPECT_EQ(set.shared.sat_phase_bias.size(), 8u);
    EXPECT_EQ(set.shared.sat_code_bias.size(), 8u);
    EXPECT_EQ(set.slots.size(), 7u + 6u + 6u + 12u + 3u + 6u + 4u + 8u + 8u);

    // A born parameter is the last slot; the first ambiguity born (G12 L2W) is the first of
    // its family in column order, though it is not first in key order.
    const int g12 = findAmbiguity(set, "G12_L2W");
    ASSERT_GE(g12, 0);
    EXPECT_EQ(g12, 7 + 6 + 6);
    EXPECT_EQ(set.amb.begin()->first.sat, *SatId::parse("E11"));   // "E11_L1C" < "E11_L5Q" < ...
}

TEST(StationParamsTest, TypedMapsIterateInTheOrderOfTheStateKeys) {
    Station st;
    const StationParams& set = st.set;

    // The fixture adds its columns out of key order, so the slot indices a map visits are not
    // ascending: a map that iterated in birth order would visit them ascending.
    for (const std::vector<int>& v : {visited(set.amb), visited(set.iono), visited(set.rx_cb)})
        EXPECT_FALSE(std::is_sorted(v.begin(), v.end()));

    // The name order of the systems: "GLONASS" < "Galileo" (the letter order puts E first).
    ASSERT_EQ(set.isb.size(), 2u);
    EXPECT_EQ(set.isb.begin()->first, System::GLONASS);
    EXPECT_EQ(set.isb.rbegin()->first, System::Galileo);

    // Each map visits the key texts in the string order of the texts: the order the rank-1
    // updates and the folds walk.
    const auto sorted = [](const std::vector<std::string>& v) { return std::is_sorted(v.begin(), v.end()); };
    EXPECT_TRUE(sorted(keyTexts(set, set.isb)));
    EXPECT_TRUE(sorted(keyTexts(set, set.iono)));
    EXPECT_TRUE(sorted(keyTexts(set, set.colour)));
    EXPECT_TRUE(sorted(keyTexts(set, set.amb)));
    EXPECT_TRUE(sorted(keyTexts(set, set.rx_pb)));
    EXPECT_TRUE(sorted(keyTexts(set, set.rx_cb)));
    EXPECT_TRUE(sorted(keyTexts(set, set.shared.sat_clock)));
    EXPECT_TRUE(sorted(keyTexts(set, set.shared.sat_phase_bias)));
    EXPECT_TRUE(sorted(keyTexts(set, set.shared.sat_code_bias)));
    EXPECT_EQ(keyTexts(set, set.rx_cb),
              (std::vector<std::string>{"GLONASS_IFB2_k+5", "GLONASS_IFB2_k-4", "GPS_DCB", "GPS_IFB5",
                                        "GPS_ISC_C1W", "Galileo_DCB"}));
}

TEST(StationParamsTest, AddParamKeepsEverySlotAndCountsWhatNoMapHolds) {
    StationParams set;
    const SatId g05 = *SatId::parse("G05");
    const SignalId l1c = *SignalId::fromObsCode(System::GPS, "L1C");
    const SignalId l2w = *SignalId::fromObsCode(System::GPS, "L2W");
    const auto slot = [](const ParamRef& ref, double x) {
        LocalSlot s;
        s.ref   = ref;
        s.x_lin = x;
        return s;
    };

    EXPECT_TRUE(addParam(set, slot(ambiguityRef(g05, l1c), 1.0)));
    // The same key a second time: the slot stays (column order), the lookup keeps the first.
    EXPECT_FALSE(addParam(set, slot(ambiguityRef(g05, l1c), 2.0)));
    // Forms only the single-receiver filter allocates have no typed map.
    EXPECT_FALSE(addParam(set, slot(ambiguityIfRef(g05, SignalPair{l1c, l2w}), 3.0)));
    EXPECT_FALSE(addParam(set, slot(phaseClockRef(), 4.0)));
    EXPECT_FALSE(addParam(set, slot(gradientRef(true), 5.0)));
    EXPECT_TRUE(addParam(set, slot(rxClockRef(), 6.0)));
    EXPECT_FALSE(addParam(set, slot(rxClockRef(), 7.0)));

    EXPECT_EQ(set.slots.size(), 7u);
    EXPECT_EQ(set.untyped, 5);
    EXPECT_EQ(findParam(set, ambiguityRef(g05, l1c)), 0);
    EXPECT_EQ(findParam(set, ambiguityRef(g05, l2w)), -1);
    EXPECT_EQ(findParam(set, ambiguityIfRef(g05, SignalPair{l1c, l2w})), -1);
    EXPECT_EQ(findParam(set, rxClockRef()), 5);
    EXPECT_EQ(findParam(set, zwdRef()), -1);
    EXPECT_EQ(findParam(set, rxPositionRef(0)), -1);
}

// ---------------------------------------------------------------------------
// The writers
// ---------------------------------------------------------------------------

TEST(StationParamsTest, APruneKeepsTheSurvivorsInColumnOrderAndRenumbersEveryMap) {
    Station st;
    StationParams& set = st.set;
    const std::size_t before = set.slots.size();
    std::vector<ParamRef> order;   // the surviving columns, in column order, before the prune
    std::set<SatSignalId> keep;
    for (const auto& kv : set.amb)
        if (kv.first.sat.sys != System::GLONASS) keep.insert(kv.first);   // drops R02, R07 (4 ambiguities)
    const std::size_t dropped = set.amb.size() - keep.size();
    ASSERT_EQ(dropped, 4u);
    for (const LocalSlot& s : set.slots) {
        const bool glonass_amb = s.ref.kind == NetworkParamKind::Ambiguity && s.ref.sat.sys == System::GLONASS;
        if (!glonass_amb) order.push_back(s.ref);
    }

    set.record_pruned = true;
    pruneAmbiguitiesExcept(set, keep);
    EXPECT_EQ(set.slots.size(), before - dropped);
    EXPECT_EQ(set.amb.size(), keep.size());
    ASSERT_EQ(set.slots.size(), order.size());
    for (std::size_t k = 0; k < set.slots.size(); ++k) {
        EXPECT_EQ(set.slots[k].ref, order[k]) << k;                  // survivors keep their order
        EXPECT_EQ(findParam(set, order[k]), static_cast<int>(k)) << describe(order[k]);   // and every map follows
    }
    EXPECT_EQ(set.clock, 3);
    EXPECT_EQ(set.zwd, 4);

    // The pruned ambiguities are recorded in key order, and taken once.
    const std::vector<SatSignalId> pruned = takePrunedAmbiguities(set);
    ASSERT_EQ(pruned.size(), dropped);
    EXPECT_TRUE(std::is_sorted(pruned.begin(), pruned.end()));
    for (const SatSignalId& id : pruned) EXPECT_EQ(id.sat.sys, System::GLONASS);
    EXPECT_TRUE(takePrunedAmbiguities(set).empty());

    // Without the recorder nothing is recorded.
    set.record_pruned = false;
    pruneAmbiguitiesExcept(set, std::set<SatSignalId>{});
    EXPECT_TRUE(set.pruned_ambiguities.empty());
    EXPECT_TRUE(set.amb.empty());
}

TEST(StationParamsTest, EachFamilyPruneTouchesOnlyItsFamilyAndNeverTheCore) {
    Station st;
    StationParams& set = st.set;
    const std::size_t core = 7;

    // Everything of the family goes; the other families and the core stay.
    pruneSlantIonoExcept(set, {});
    EXPECT_TRUE(set.iono.empty());
    EXPECT_EQ(set.colour.size(), 6u);
    prunePhaseColourExcept(set, {});
    EXPECT_TRUE(set.colour.empty());
    pruneReceiverCodeBiasExcept(set, {dcbKey(System::GPS)});
    EXPECT_EQ(set.rx_cb.size(), 1u);
    pruneReceiverPhaseBiasExcept(set, {});
    EXPECT_TRUE(set.rx_pb.empty());
    pruneSatClockExcept(set, {*SatId::parse("G05")});
    EXPECT_EQ(set.shared.sat_clock.size(), 1u);
    pruneSatPhaseBiasExcept(set, {});
    EXPECT_TRUE(set.shared.sat_phase_bias.empty());
    pruneSatCodeBiasExcept(set, {});
    EXPECT_TRUE(set.shared.sat_code_bias.empty());
    EXPECT_EQ(set.amb.size(), 12u);
    EXPECT_EQ(set.slots.size(), core + 12u + 1u + 1u);   // core, ambiguities, one code bias, one clock

    // The core columns have no prune; a request that names one removes nothing.
    removeSlots(set, {0, 1, 2, set.clock, set.zwd});
    EXPECT_EQ(set.slots.size(), core + 12u + 1u + 1u);
    EXPECT_EQ(findParam(set, rxClockRef()), 3);
    for (const auto& kv : set.amb) EXPECT_EQ(findParam(set, slotAt(set, kv.second).ref), kv.second);
}

TEST(StationParamsTest, TheReseedMarkOutlivesTheColumnAndIsNotAPropertyOfASlot) {
    Station st;
    StationParams& set = st.set;
    const std::string held    = K::makeAmbiguityKey("G05", "L1C");
    const std::string absent  = K::makeAmbiguityKey("G30", "L1C");   // no column
    markPendingReseed(set, held);
    markPendingReseed(set, absent);
    markPendingReseed(set, "not an ambiguity key");   // marks nothing
    EXPECT_TRUE(pendingReseed(set, held));
    EXPECT_TRUE(pendingReseed(set, absent));
    EXPECT_FALSE(pendingReseed(set, K::makeAmbiguityKey("E24", "L1C")));
    EXPECT_FALSE(pendingReseed(set, "not an ambiguity key"));
    EXPECT_EQ(set.pending_reseed.size(), 2u);

    // A prune of the marked column leaves the mark; the birth that comes next consumes it.
    pruneAmbiguitiesExcept(set, {});
    EXPECT_TRUE(pendingReseed(set, held));
    clearPendingReseed(set, held);
    EXPECT_FALSE(pendingReseed(set, held));
    clearPendingReseed(set, SatSignalId{*SatId::parse("G30"), gnss_test::signalOf(System::GPS, "L1C")});
    EXPECT_TRUE(set.pending_reseed.empty());
}

TEST(StationParamsTest, TheSettlementGivesDispositionsAndSortsTheWithheldParameters) {
    Station st;
    StationParams& set = st.set;
    const SatId g05 = *SatId::parse("G05");
    const SatId g30 = *SatId::parse("G30");
    const SatId r07 = *SatId::parse("R07");

    // Three withheld parameters: a phase bias whose absorbing ambiguity the station holds, one
    // whose ambiguity it does not, and a code bias whose satellite clock it holds; and one
    // identity no constellation carries.
    noteWithheldPhaseBias(set, SatSignalId{g05, gnss_test::signalOf(System::GPS, "L1C")},
                          AbsorbedReason::CoverageGate);
    noteWithheldPhaseBias(set, SatSignalId{g30, gnss_test::signalOf(System::GPS, "L1C")},
                          AbsorbedReason::Allowlist);
    set.shared.withheld.push_back(AbsorbedParam{
        satCodeBiasRef(g05, gnss_test::signalOf(System::GPS, "C1C")), AbsorbedReason::AnchoredUnionCoverage});
    set.shared.withheld_unnamed.push_back("absorbed sat_phase_bias_states:not a key");
    (void)r07;

    settleStationParams(set, st.disp);
    EXPECT_EQ(set.decided, set.slots.size());
    for (std::size_t k = 0; k < set.slots.size(); ++k) EXPECT_EQ(set.slots[k].disp, st.disp[k]) << k;
    ASSERT_EQ(set.shared.absorbed.size(), 2u);
    EXPECT_EQ(set.shared.absorbed[0].reason, AbsorbedReason::CoverageGate);
    EXPECT_EQ(set.shared.absorbed[0].ref, satPhaseBiasRef(g05, gnss_test::signalOf(System::GPS, "L1C")));
    EXPECT_EQ(set.shared.absorbed[1].reason, AbsorbedReason::AnchoredUnionCoverage);
    EXPECT_EQ(set.shared.absorbed_no_absorber, 1);
    ASSERT_EQ(set.unclassified.size(), 1u);
    EXPECT_EQ(set.unclassified[0], "absorbed sat_phase_bias_states:not a key");

    // A second settlement starts from the notes again, and a shorter vector leaves the rest
    // undecided.
    st.disp.resize(st.disp.size() - 3);
    settleStationParams(set, st.disp);
    EXPECT_EQ(set.decided, set.slots.size() - 3);
    EXPECT_EQ(set.slots.back().disp, PD::Unobserved);
    EXPECT_EQ(set.shared.absorbed.size(), 2u);
}

TEST(StationParamsTest, TheInventoryRecordedFromTheSetListsEveryColumnOnce) {
    Station st;
    noteWithheldPhaseBias(st.set, SatSignalId{*SatId::parse("G05"), gnss_test::signalOf(System::GPS, "L1C")},
                          AbsorbedReason::CoverageGate);
    noteWithheldPhaseBias(st.set, SatSignalId{*SatId::parse("G30"), gnss_test::signalOf(System::GPS, "L1C")},
                          AbsorbedReason::Allowlist);
    const StationParamSlot slot = st.inventory(4);
    EXPECT_TRUE(slot.filled);
    EXPECT_TRUE(slot.unclassified.empty());
    EXPECT_EQ(slot.no_disposition, 0);
    EXPECT_EQ(slot.absorbed_no_absorber, 1);   // G30 L1C: no ambiguity to absorb it
    ASSERT_EQ(slot.records.size(), st.set.slots.size() + 1u);   // every column, and the one absorbed
    for (std::size_t k = 0; k < st.set.slots.size(); ++k) {
        EXPECT_EQ(slot.records[k].id.station.index, 4);
        EXPECT_EQ(slot.records[k].disp, st.disp[k]) << k;
        EXPECT_TRUE(slot.records[k].id == paramInstanceOf(st.set.slots[k].ref, StationId{4})) << k;
    }
    EXPECT_EQ(slot.records.back().disp, PD::Absorbed);
    EXPECT_EQ(slot.records.back().reason, AbsorbedReason::CoverageGate);

    // A disposition vector shorter than the set: the columns past its end are counted, never
    // given a disposition by default.
    st.disp.resize(st.disp.size() - 3);
    st.set.shared.withheld.clear();
    const StationParamSlot short_slot = st.inventory(4);
    EXPECT_EQ(short_slot.no_disposition, 3);
    EXPECT_EQ(short_slot.records.size(), st.set.slots.size() - 3);
}

// ---------------------------------------------------------------------------
// What the readers rely on
// ---------------------------------------------------------------------------

TEST(StationParamsTest, KeyTextAndStateLabelAreTheStateKeyFormats) {
    Station st;
    const StationParams& set = st.set;
    EXPECT_EQ(stateLabel(set.slots[0], 0), "POS_X");
    EXPECT_EQ(stateLabel(set.slots[1], 1), "POS_Y");
    EXPECT_EQ(stateLabel(set.slots[2], 2), "POS_Z");
    EXPECT_EQ(stateLabel(set.slots[3], 3), "CLK");
    EXPECT_EQ(stateLabel(set.slots[4], 4), "ZWD");
    EXPECT_EQ(stateLabel(set.slots[5], 5), "ISB_Galileo");
    EXPECT_EQ(stateLabel(set.slots[6], 6), "ISB_GLONASS");

    const auto label = [&](const ParamRef& ref) {
        const int k = findParam(set, ref);
        EXPECT_GE(k, 0) << describe(ref);
        return stateLabel(slotAt(set, k), static_cast<std::size_t>(k));
    };
    const SatId g05 = *SatId::parse("G05");
    EXPECT_EQ(label(slantIonoRef(g05)), "IONO:G05");
    EXPECT_EQ(label(phaseColourRef(g05)), "PCOL:G05");
    EXPECT_EQ(label(ambiguityRef(g05, gnss_test::signalOf(System::GPS, "L1C"))), "AMB:G05_L1C");
    EXPECT_EQ(label(rxCodeBiasRef(dcbKey(System::GPS))), "CBIAS:GPS_DCB");
    EXPECT_EQ(label(rxCodeBiasRef(ifbKey(System::GPS, '5'))), "CBIAS:GPS_IFB5");
    EXPECT_EQ(label(rxCodeBiasRef(iscKey(System::GPS, "C1W"))), "CBIAS:GPS_ISC_C1W");
    EXPECT_EQ(label(rxCodeBiasRef(slotIfbKey(System::GLONASS, '2', -4))), "CBIAS:GLONASS_IFB2_k-4");
    EXPECT_EQ(label(rxPhaseBiasRef(System::GPS, gnss_test::signalOf(System::GPS, "L1C"))), "RPB:GPS_RPB_L1C");
    EXPECT_EQ(label(satClockRef(g05)), "SATCLK:G05|SC");
    EXPECT_EQ(label(satPhaseBiasRef(g05, gnss_test::signalOf(System::GPS, "L2W"))), "SATPB:G05|L2W|PB");
    EXPECT_EQ(label(satCodeBiasRef(g05, gnss_test::signalOf(System::GPS, "C1C"))), "SATB:G05|C1C");

    // The texts the epoch's shared-key sets carry.
    EXPECT_EQ(satPhaseBiasText(SatSignalId{g05, gnss_test::signalOf(System::GPS, "L2W")}), "G05|L2W|PB");
    EXPECT_EQ(satCodeBiasText(SatSignalId{g05, gnss_test::signalOf(System::GPS, "C1C")}), "G05|C1C");
}

TEST(StationParamsTest, TheCarryReadersGetWhatTheKeyParsersGive) {
    Station st;
    const StationParams& set = st.set;

    // Ambiguity: the satellite, the signal and the key the readers build from an entry are the
    // ones the key parser splits out of the key text, at the same column.
    for (const auto& [arc, index] : set.amb) {
        const std::string text = keyText(slotAt(set, index).ref);
        const auto parts = K::parseAmbiguityKey(text);
        ASSERT_TRUE(parts.has_value()) << text;
        EXPECT_EQ(net::satKey(arc.sat), parts->satellite);
        EXPECT_EQ(net::signalKey(arc.signal), parts->signal);
        EXPECT_EQ(net::ambKey(arc), text);
        EXPECT_EQ(findAmbiguity(set, text), index);
    }
    // Slant ionosphere and phase colour: the satellite string is the key.
    for (const auto& [sat, index] : set.iono) EXPECT_EQ(keyText(slotAt(set, index).ref), net::satKey(sat));
    for (const auto& [sat, index] : set.colour) EXPECT_EQ(keyText(slotAt(set, index).ref), net::satKey(sat));
    // Inter-system bias: the constellation name is the key.
    for (const auto& [sys, index] : set.isb) EXPECT_EQ(keyText(slotAt(set, index).ref), systemName(sys));

    // Receiver code bias: the form predicates the readers use are the key predicates.
    long long per_slot = 0, dcb = 0, ifb = 0;
    for (const auto& [key, index] : set.rx_cb) {
        const std::string text = keyText(slotAt(set, index).ref);
        EXPECT_EQ(key.form == ParamForm::GloSlotIfb, K::isGlonassPerSlotIfbKey(text)) << text;
        EXPECT_EQ(key.form == ParamForm::Dcb, K::parseDcbKey(text).has_value()) << text;
        EXPECT_EQ(key.form == ParamForm::Ifb, K::parseIfbKey(text).has_value()) << text;
        // The rule reads a key as it reads the class of the same column.
        EXPECT_EQ(RxIfbRule::governs(key), RxIfbRule::governs(paramInstanceOf(slotAt(set, index).ref).cls)) << text;
        per_slot += key.form == ParamForm::GloSlotIfb;
        dcb += key.form == ParamForm::Dcb;
        ifb += RxIfbRule::governs(key);
        // The key text reads back as the key.
        const std::optional<RxCbKey> back = receiverCodeBiasKey(text);
        ASSERT_TRUE(back.has_value()) << text;
        EXPECT_EQ(*back, key) << text;
    }
    EXPECT_EQ(per_slot, 2);
    EXPECT_EQ(dcb, 2);
    EXPECT_EQ(ifb, 1);
    EXPECT_FALSE(receiverCodeBiasKey("GPS_XYZ").has_value());
    EXPECT_FALSE(receiverCodeBiasKey("").has_value());
    EXPECT_FALSE(receiverCodeBiasKey("GPS_RPB_L1C").has_value());   // another family
    EXPECT_FALSE(receiverCodeBiasKey(K::makeReceiverIscKey("IRNSS", "C5A")).has_value());   // needs a known system
    ASSERT_TRUE(receiverCodeBiasKey(K::makeDcbKey("IRNSS")).has_value());   // the form without a signal keeps its class
    EXPECT_EQ(receiverCodeBiasKey(K::makeDcbKey("IRNSS"))->sys, System::Unknown);
}

namespace {

/// The local-column map of a station whose estimated-local columns are the block, as the split
/// builds it: state index to local column, in state-index order.
void localColumns(const StationParams& set, StationAssembly& R) {
    int next = 0;
    for (std::size_t k = 0; k < set.slots.size(); ++k)
        if (set.slots[k].disp == PD::EstimatedLocal) R.h2local[static_cast<int>(k)] = next++;
    R.A_r.resize(1, next);
}

/// The span label of each local column, built straight from the typed maps.
std::vector<std::string> expectedBlockLabels(const StationAssembly& R, const StationParams& set, Eigen::Index n_local) {
    std::vector<std::string> labels(static_cast<std::size_t>(n_local));
    auto put = [&](int state_idx, std::string label) {
        const auto hit = R.h2local.find(state_idx);
        if (hit == R.h2local.end() || hit->second < 0 || hit->second >= n_local) return;
        labels[static_cast<std::size_t>(hit->second)] = std::move(label);
    };
    put(set.position[0], "pos:x"); put(set.position[1], "pos:y"); put(set.position[2], "pos:z");
    put(set.clock, "clk");
    put(set.zwd, "zwd");
    for (const auto& [key, idx] : set.rx_cb)  put(idx, "rxcb:" + keyText(rxCodeBiasRef(key)));
    for (const auto& [sat, idx] : set.iono)   put(idx, "iono:" + net::satKey(sat));
    for (const auto& [sys, idx] : set.isb)    put(idx, std::string("isb:") + systemName(sys));
    for (const auto& [id, idx] : set.shared.sat_code_bias)   put(idx, "satbias:" + satCodeBiasText(id));
    for (const auto& [id, idx] : set.shared.sat_phase_bias)  put(idx, "satpb:" + satPhaseBiasText(id));
    for (const auto& [sig, idx] : set.rx_pb)  put(idx, "rxpb:" + keyText(rxPhaseBiasRef(sig.sys, sig)));
    for (const auto& [sat, idx] : set.shared.sat_clock)      put(idx, "satclk:" + net::satKey(sat) + "|SC");
    for (const auto& [arc, idx] : set.amb)    put(idx, "amb:" + net::satKey(arc.sat) + ':' + net::signalKey(arc.signal));
    return labels;
}

/// The labels the redundancy census prints, built straight from the typed maps.
std::map<Eigen::Index, std::string> expectedCensusLabels(const StationAssembly& R, const StationParams& set) {
    std::map<Eigen::Index, std::string> out;
    auto put = [&](int state_idx, const std::string& label) {
        if (state_idx < 0) return;
        const auto hit = R.h2local.find(state_idx);
        if (hit == R.h2local.end() || hit->second < 0) return;
        out.emplace(static_cast<Eigen::Index>(hit->second), label);
    };
    for (const auto& kv : set.iono)  put(kv.second, "iono:" + net::satKey(kv.first));
    for (const auto& kv : set.amb)   put(kv.second, "amb:" + net::ambKey(kv.first));
    for (const auto& kv : set.rx_cb) put(kv.second, "rxcb:" + keyText(rxCodeBiasRef(kv.first)));
    for (const auto& kv : set.rx_pb) put(kv.second, "rpb:" + keyText(rxPhaseBiasRef(kv.first.sys, kv.first)));
    put(set.zwd, "zwd");
    put(set.clock, "rx_clock");
    return out;
}

}  // namespace

TEST(StationParamsTest, TheTraceLabelsNameEveryFamilyOfTheBlock) {
    Station st;
    // Make the whole non-position set the block, so that every family is labelled.
    st.disp.assign(st.set.slots.size(), PD::EstimatedLocal);
    for (std::size_t k = 0; k < 3; ++k) st.disp[static_cast<std::size_t>(st.set.position[k])] = PD::Pinned;
    settleStationParams(st.set, st.disp);
    StationAssembly R;
    localColumns(st.set, R);
    const Eigen::Index n_local = R.A_r.cols();
    ASSERT_GT(n_local, 0);

    const std::vector<std::string> block = stationBlockLabels(R, st.set, n_local);
    const std::vector<std::string> expected = expectedBlockLabels(R, st.set, n_local);
    ASSERT_EQ(block.size(), expected.size());
    for (std::size_t k = 0; k < block.size(); ++k) EXPECT_EQ(block[k], expected[k]) << "column " << k;
    // The phase colour has no span label: its columns read col<k> in the audit.
    long long colour_cols = 0, empty = 0;
    for (const std::string& l : block) empty += l.empty();
    for (const auto& kv : st.set.colour) colour_cols += R.h2local.count(kv.second);
    EXPECT_EQ(empty, colour_cols);
    EXPECT_GT(colour_cols, 0);

    EXPECT_EQ(localColumnLabels(R, st.set), expectedCensusLabels(R, st.set));
}

TEST(StationParamsTest, TheTextLookupsFindTheColumnsOfTheTypedMaps) {
    Station st;
    const StationParams& set = st.set;

    for (const auto& [arc, col] : set.amb) EXPECT_EQ(findAmbiguity(set, net::ambKey(arc)), col);
    EXPECT_EQ(findAmbiguity(set, K::makeAmbiguityKey("G07", "L1C")), -1);   // a satellite the station lacks
    EXPECT_EQ(findAmbiguity(set, "G12"), -1);                               // no signal
    EXPECT_EQ(findAmbiguity(set, ""), -1);

    for (const auto& [sat, col] : set.iono) EXPECT_EQ(findSlantIono(set, net::satKey(sat)), col);
    EXPECT_EQ(findSlantIono(set, "G07"), -1);
    EXPECT_EQ(findSlantIono(set, "not a satellite"), -1);

    for (const auto& [sig, col] : set.rx_pb) {
        const std::string text = keyText(slotAt(set, col).ref);
        EXPECT_EQ(findReceiverPhaseBias(set, text), col) << text;
        EXPECT_TRUE(receiverPhaseBiasSignal(text).has_value()) << text;
    }
    EXPECT_EQ(findReceiverPhaseBias(set, K::makeReceiverPhaseBiasKey("GPS", "L5X")), -1);
    EXPECT_EQ(findReceiverPhaseBias(set, "GPS_DCB"), -1);
    EXPECT_FALSE(receiverPhaseBiasSignal("GPS_RPB_").has_value());
    EXPECT_FALSE(receiverPhaseBiasSignal("_RPB_L1C").has_value());
    EXPECT_FALSE(receiverPhaseBiasSignal("Nowhere_RPB_L1C").has_value());

    for (const auto& [key, col] : set.rx_cb) {
        const std::string text = keyText(slotAt(set, col).ref);
        EXPECT_EQ(findReceiverCodeBias(set, text), col) << text;
    }
    EXPECT_EQ(findReceiverCodeBias(set, K::makeDcbKey("GLONASS")), -1);       // a system with no code bias here
    EXPECT_EQ(findReceiverCodeBias(set, K::makeIfbKey("GPS", '2')), -1);      // a band the station lacks
    EXPECT_EQ(findReceiverCodeBias(set, K::makeReceiverPhaseBiasKey("GPS", "L1C")), -1);   // another family
    EXPECT_EQ(findReceiverCodeBias(set, ""), -1);
}

TEST(StationParamsTest, TheSdEwlCandidatesAreTheAmbiguitiesWithARowGroupedBySatelliteAndBand) {
    Station st;
    NetStation net;
    net.params = st.set;

    // This epoch's phase rows: every second ambiguity column.
    std::map<int, int> h2local;
    int n = 0, col = 0;
    for (const auto& kv : st.set.amb)
        if (n++ % 2 == 0) h2local[kv.second] = col++;

    // The candidates: the phase signals that have a row, grouped by satellite and band digit,
    // each list ascending by signal name.
    SdEwlCandidates expected;
    for (const auto& [arc, sidx] : st.set.amb) {
        const auto hit = h2local.find(sidx);
        if (hit == h2local.end()) continue;
        expected[{net::satKey(arc.sat), arc.signal.digit}].emplace_back(net::signalKey(arc.signal), hit->second, sidx);
    }
    for (auto& kv : expected) std::sort(kv.second.begin(), kv.second.end());
    ASSERT_FALSE(expected.empty());

    NetworkEstimator est;
    std::vector<PppObservation> sta_obs;
    GnssTime t{};
    Eigen::MatrixXd A_s, A_r;
    Eigen::VectorXd w, y;
    std::vector<RowTag> tags;
    std::vector<std::string> trace_rows;
    SdEwlConfig cfg;
    NetStation::SdEwlCounters ctr;
    SdEwlTraceFull tr2 = [](const char*, const std::string&, const std::string&, const std::string&,
                            const std::string&, const std::string&, const std::string&, std::size_t,
                            std::size_t, bool, double, double, double, double, double, long long, bool,
                            bool) {};
    SdEwlTraceShort tr = [](const char*, const std::string&, const std::string&, const std::string&,
                            const std::string&, const std::string&, const std::string&, std::size_t,
                            std::size_t, bool, double, double, double, double, double) {};
    const SdEwlRowView view{st.cfg, net, est, sta_obs, h2local, t, A_s, A_r, w, y, tags, trace_rows,
                            nullptr, 0, cfg, ctr, false, tr2, tr};
    SdEwlCandidates got;
    sdEwlFillCandidates(view, got);
    EXPECT_EQ(got, expected);
}

TEST(StationParamsTest, AnUnresolvableAmbiguityIsMarkedInTheSlotAndClearedByTheNextBirth) {
    Station st;
    StationParams& set = st.set;
    const int marked = findAmbiguity(set, K::makeAmbiguityKey("G05", "L1C"));
    ASSERT_GE(marked, 0);
    set.slots[static_cast<std::size_t>(marked)].unresolvable = true;
    int n_marked = 0;
    for (const LocalSlot& s : set.slots) n_marked += s.unresolvable ? 1 : 0;
    EXPECT_EQ(n_marked, 1);
    EXPECT_TRUE(slotAt(set, findAmbiguity(set, K::makeAmbiguityKey("G05", "L1C"))).unresolvable);
    EXPECT_FALSE(slotAt(set, findAmbiguity(set, K::makeAmbiguityKey("E24", "L1C"))).unresolvable);
}

TEST(StationParamsTest, TheSharedKeyConvertersReadWhatTheKeyParsersRead) {
    Station st;
    ASSERT_FALSE(st.set.shared.sat_phase_bias.empty());
    for (const auto& [id, col] : st.set.shared.sat_phase_bias) {
        const std::string text = satPhaseBiasText(id);
        const auto back = net::satPhaseBiasKey(text);
        ASSERT_TRUE(back.has_value()) << text;
        EXPECT_EQ(*back, id) << text;
        EXPECT_EQ(keyText(slotAt(st.set, col).ref), text);
    }
    // A satellite code-bias key ("G05|C1C") is no phase-bias key.
    ASSERT_FALSE(st.set.shared.sat_code_bias.empty());
    for (const auto& [id, col] : st.set.shared.sat_code_bias) {
        EXPECT_FALSE(net::satPhaseBiasKey(satCodeBiasText(id)).has_value());
        EXPECT_EQ(keyText(slotAt(st.set, col).ref), satCodeBiasText(id));
    }
    for (const char* bad : {"", "|PB", "G05|PB", "G05||PB", "G05|L1C", "G05_L1C|PB", "X99|L1C|PB", "G05|??C|PB"})
        EXPECT_FALSE(net::satPhaseBiasKey(bad).has_value()) << bad;
}

// ---------------------------------------------------------------------------
// The births and the admission
// ---------------------------------------------------------------------------

TEST(StationParamsTest, TheBirthsGiveAStationTheParametersItsObservationsImply) {
    BirthFixture f;
    ASSERT_TRUE(f.stack.config.isUncombinedEnabled());
    const StationRowContext c = f.context();
    const std::vector<PppObservation> obs = f.epoch(0);
    ASSERT_EQ(f.set.slots.size(), 6u);   // core and the Galileo bias

    networkBirthLocalStates(c, f.set, obs);
    EXPECT_EQ(f.set.iono.size(), 3u);
    EXPECT_EQ(f.set.amb.size(), 6u);   // L1C and L2W of two GPS satellites, L1C and L5Q of one Galileo
    EXPECT_TRUE(f.set.colour.empty());   // the phase colour is not armed
    EXPECT_EQ(f.set.slots.size(), 6u + 3u + 6u);
    // Born in the order of the observations: the slant ionospheres first, then the ambiguities
    // satellite by satellite, each satellite's signals in signal order.
    EXPECT_EQ(f.set.iono.at(*SatId::parse("G05")), 6);
    EXPECT_EQ(f.set.iono.at(*SatId::parse("G12")), 7);
    EXPECT_EQ(f.set.iono.at(*SatId::parse("E24")), 8);
    EXPECT_EQ(findAmbiguity(f.set, "G05_L1C"), 9);
    EXPECT_EQ(findAmbiguity(f.set, "G05_L2W"), 10);
    EXPECT_EQ(findAmbiguity(f.set, "G12_L1C"), 11);
    EXPECT_EQ(findAmbiguity(f.set, "G12_L2W"), 12);
    EXPECT_EQ(findAmbiguity(f.set, "E24_L1C"), 13);
    EXPECT_EQ(findAmbiguity(f.set, "E24_L5Q"), 14);
    EXPECT_TRUE(f.set.seeded.isb);

    // The same epoch again changes nothing; a satellite that leaves takes its parameters with it.
    const StationParams again = f.set;
    networkBirthLocalStates(c, f.set, obs);
    ASSERT_EQ(f.set.slots.size(), again.slots.size());
    for (std::size_t k = 0; k < again.slots.size(); ++k) EXPECT_EQ(f.set.slots[k].ref, again.slots[k].ref) << k;
    f.set.record_pruned = true;
    networkBirthLocalStates(c, f.set, f.epoch(1, /*with_g12=*/false));
    EXPECT_EQ(f.set.iono.size(), 2u);
    EXPECT_EQ(f.set.amb.size(), 4u);
    EXPECT_EQ(f.set.slots.size(), 6u + 2u + 4u);
    EXPECT_EQ(takePrunedAmbiguities(f.set).size(), 2u);
    // The survivors kept their relative order and are renumbered.
    EXPECT_EQ(findAmbiguity(f.set, "G05_L1C"), 8);
    EXPECT_EQ(findAmbiguity(f.set, "E24_L5Q"), 11);
}

TEST(StationParamsTest, AMarkedAmbiguityIsGivenANewValueAndTheMarkIsConsumed) {
    BirthFixture f;
    const StationRowContext c = f.context();
    networkBirthLocalStates(c, f.set, f.epoch(0));
    const int col = findAmbiguity(f.set, "G05_L1C");
    ASSERT_GE(col, 0);
    f.set.slots[static_cast<std::size_t>(col)].x_lin = 12345.0;

    // Unmarked, the value is kept; marked, it is re-seeded from the new epoch's observation.
    networkBirthLocalStates(c, f.set, f.epoch(1));
    EXPECT_EQ(f.set.slots[static_cast<std::size_t>(col)].x_lin, 12345.0);
    markPendingReseed(f.set, "G05_L1C");
    markPendingReseed(f.set, "G30_L1C");   // no column and no observation: stays marked
    networkBirthLocalStates(c, f.set, f.epoch(2));
    EXPECT_NE(f.set.slots[static_cast<std::size_t>(col)].x_lin, 12345.0);
    EXPECT_FALSE(pendingReseed(f.set, "G05_L1C"));
    EXPECT_TRUE(pendingReseed(f.set, "G30_L1C"));
    EXPECT_EQ(f.set.amb.size(), 6u);   // a re-seed adds no column
}

TEST(StationParamsTest, TheRowsAreOneRowPerUsableSignalOverTheColumnsOfTheSet) {
    BirthFixture f;
    const StationRowContext c = f.context();
    const std::vector<PppObservation> obs = f.epoch(0);
    networkBirthLocalStates(c, f.set, obs);
    networkAdmitSharedStates(c, f.set, obs, {}, nullptr);

    const EpochMeasurement em = networkStationRows(c, f.set, obs);
    const Eigen::Index rows = em.innovations.size();
    EXPECT_EQ(rows, 12);   // two code and two phase signals of three satellites
    EXPECT_EQ(em.H.rows(), rows);
    EXPECT_EQ(em.H.cols(), static_cast<Eigen::Index>(f.set.slots.size()));
    EXPECT_EQ(em.satellite_ids.size(), static_cast<std::size_t>(rows));
    EXPECT_EQ(em.signal_types.size(), static_cast<std::size_t>(rows));
    for (Eigen::Index r = 0; r < rows; ++r) {
        EXPECT_TRUE(std::isfinite(em.innovations(r))) << r;
        EXPECT_GT(em.R_v(r), 0.0) << r;
        const std::string& sat = em.satellite_ids[static_cast<std::size_t>(r)];
        const std::string& sig = em.signal_types[static_cast<std::size_t>(r)];
        const SatId s = *SatId::parse(sat);
        // The receiver clock enters every row with +1; a phase row carries its own ambiguity.
        EXPECT_EQ(em.H(r, f.set.clock), 1.0) << sat << " " << sig;
        const int amb = findAmbiguity(f.set, K::makeAmbiguityKey(sat, sig));
        if (sig[0] == 'L') {
            ASSERT_GE(amb, 0);
            EXPECT_EQ(em.H(r, amb), 1.0);
        } else {
            EXPECT_LT(amb, 0);
        }
        // The slant ionosphere of the row's satellite is its own column.
        EXPECT_NE(em.H(r, f.set.iono.at(s)), 0.0);
        // The satellite clock is a shared column of the set.
        EXPECT_EQ(em.H(r, f.set.shared.sat_clock.at(s)), kSatClockPartial);
    }
}

TEST(StationParamsTest, TheAdmissionGivesAStationTheSharedParametersItsRowsWrite) {
    BirthFixture f;
    const StationRowContext c = f.context();
    const std::vector<PppObservation> obs = f.epoch(0);
    networkBirthLocalStates(c, f.set, obs);
    networkAdmitSharedStates(c, f.set, obs, {}, nullptr);
    EXPECT_EQ(f.set.shared.sat_clock.size(), 3u);
    EXPECT_EQ(f.set.shared.sat_phase_bias.size(), 6u);   // one per phase signal
    EXPECT_TRUE(f.set.shared.sat_code_bias.empty());      // the satellite code bias is not armed
    EXPECT_TRUE(f.set.shared.withheld.empty());
    // A shared parameter is born at 0 and appended: after the local columns of the epoch.
    for (const auto& kv : f.set.shared.sat_clock) EXPECT_EQ(slotAt(f.set, kv.second).x_lin, 0.0);
    EXPECT_GT(f.set.shared.sat_clock.begin()->second, f.set.amb.rbegin()->second);

    // The legacy pivot datum shares only the pivot's keys; the others are withheld with the gate.
    StationParams fresh = startStationParams(f.stack.config, Eigen::Vector3d(6378137.0, 0.0, 0.0));
    const std::set<std::string> pivot_keys = {"G05|L1C|PB", "G05|L2W|PB"};
    networkAdmitSharedStates(c, fresh, obs, pivot_keys, nullptr);
    EXPECT_EQ(fresh.shared.sat_phase_bias.size(), 2u);
    EXPECT_EQ(fresh.shared.withheld.size(), 4u);
    for (const AbsorbedParam& w : fresh.shared.withheld) EXPECT_EQ(w.reason, AbsorbedReason::Allowlist);

    // The code-census co-gate withholds a phase bias whose companion code pair was quarantined.
    StationParams cogated = startStationParams(f.stack.config, Eigen::Vector3d(6378137.0, 0.0, 0.0));
    const std::set<std::string> admitted_codes = {"G05|C1C", "G05|C2W", "G12|C1C", "G12|C2W", "E24|C1C"};
    networkAdmitSharedStates(c, cogated, obs, {}, &admitted_codes);
    EXPECT_EQ(cogated.shared.sat_phase_bias.size(), 5u);   // E24 L5Q (code C5Q) is withheld
    ASSERT_EQ(cogated.shared.withheld.size(), 1u);
    EXPECT_EQ(cogated.shared.withheld[0].reason, AbsorbedReason::CodeCensusCogate);
    EXPECT_EQ(cogated.shared.withheld[0].ref, satPhaseBiasRef(*SatId::parse("E24"), gnss_test::signalOf(System::Galileo, "L5Q")));

    // Settled, the withheld phase bias is absorbed by the float ambiguity of the same signal.
    networkBirthLocalStates(c, cogated, obs);
    settleStationParams(cogated, {});
    ASSERT_EQ(cogated.shared.absorbed.size(), 1u);
    EXPECT_EQ(cogated.shared.absorbed_no_absorber, 0);
}

// ---------------------------------------------------------------------------
// The produce loop's failure path
// ---------------------------------------------------------------------------

namespace {

/// The births make a column and then the update throws.  The satellite is "G3<Last>", so that
/// two calls on one station birth two columns.
template <char Last>
StationStep bornThenThrows(StationBuild& b) {
    bornAmbiguity(b.st.params, std::string("G3") + Last, "L1C", 0.5);
    throw std::runtime_error("update failed");
}

/// The same, with an exception that is not a std::exception.
StationStep bornThenThrowsNonStandard(StationBuild& b) {
    bornAmbiguity(b.st.params, "G33", "L1C", 0.5);
    throw 7;
}

StationStep goesOn(StationBuild&) { return StationStep::Next; }
StationStep endsTheBuild(StationBuild&) { return StationStep::Done; }

/// A step table of steps that do nothing, with @p first as its first.
StationBuildSteps stepsOf(StationBuildFn first, StationBuildFn second = &goesOn) {
    StationBuildSteps steps = kStationBuildSteps;
    for (StationBuildStep& step : steps) step.build = &goesOn;
    steps[0].build = first;
    steps[1].build = second;
    return steps;
}

/// A run with one station over the station-shaped set, and the build of the first epoch.
struct ProduceFixture {
    Station                              shape;
    NetworkTestRun                       run;
    std::unique_ptr<GlobalIonosphereMap> gim;
    StationAssembly                      slot;

    ProduceFixture() : run(shape.cfg) {
        run.setup().stations.resize(1);
        station().name   = "AAAA";
        station().params = shape.set;
        run.newEpoch(GnssTime{2313, 259230.0});
    }

    NetStation& station() { return run.setup().stations[0]; }

    StationBuild build() {
        NetworkEpoch& e = run.ep();
        return StationBuild{run.ctx(), run.config(), run.est(), run.setup(), e.se, e.clock,
                            e.pass_a, e.events, gim, 0, station(), slot};
    }
};

}  // namespace

TEST(StationParamsTest, AStepThatThrowsAfterAColumnWasBornLeavesTheColumnInTheSet) {
    ProduceFixture f;
    NetStation& net = f.station();
    StationBuild b = f.build();
    const std::size_t before = net.params.slots.size();

    // The walk alone lets the throw escape, with the column born.
    EXPECT_THROW(networkStationBuild(b, stepsOf(&bornThenThrows<'0'>)), std::runtime_error);
    EXPECT_EQ(net.params.slots.size(), before + 1);
    EXPECT_GE(findAmbiguity(net.params, K::makeAmbiguityKey("G30", "L1C")), 0);

    // The produce loop's turn: the throw is recorded in the slot, and the set holds the column.
    networkStationAssemble(b, stepsOf(&bornThenThrows<'1'>));
    EXPECT_TRUE(f.slot.threw);
    EXPECT_FALSE(f.slot.ok);
    EXPECT_EQ(f.slot.err, "update failed");
    EXPECT_EQ(net.params.slots.size(), before + 2);
    EXPECT_GE(findAmbiguity(net.params, K::makeAmbiguityKey("G31", "L1C")), 0);
}

TEST(StationParamsTest, AThrowThatIsNotAStdExceptionIsRecorded) {
    ProduceFixture f;
    NetStation& net = f.station();
    StationBuild b = f.build();
    const std::size_t before = net.params.slots.size();

    networkStationAssemble(b, stepsOf(&bornThenThrowsNonStandard));
    EXPECT_TRUE(f.slot.threw);
    EXPECT_EQ(f.slot.err, "unknown exception");
    EXPECT_EQ(net.params.slots.size(), before + 1);
}

TEST(StationParamsTest, ABuildThatCompletesOrEndsEarlyRecordsItsCause) {
    ProduceFixture f;
    StationBuild b = f.build();

    networkStationAssemble(b, stepsOf(&goesOn));
    EXPECT_TRUE(f.slot.ok);
    EXPECT_FALSE(f.slot.threw);

    // A step that ends the build leaves its cause in the slot; the loop adds nothing.
    f.slot = StationAssembly{};
    networkStationAssemble(b, stepsOf(&endsTheBuild));
    EXPECT_FALSE(f.slot.ok);
    EXPECT_FALSE(f.slot.threw);
}
