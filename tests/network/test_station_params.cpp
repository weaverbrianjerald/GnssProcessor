/**
 * @file test_station_params.cpp
 * @brief The station parameter set of network/state/StationParams.hpp, built from a state
 *        manager, read by the inventory and the trace labels, and checked against the state
 *        manager (network/state/StationParamsShadow.hpp).
 *
 * The fixture is a station-shaped state manager over GPS, Galileo and GLONASS whose columns
 * are added in an order unlike the key order, so a map that iterated in birth order instead of
 * key order would fail.  Contracts:
 *  - the set has one slot per column, in column order, with the state's value, the split's
 *    disposition, the ambiguity's re-seed mark and the phase colour's elevation, and every
 *    typed lookup finds its slot;
 *  - every typed map iterates in the order of the state manager's map of its family;
 *  - the check passes on that set and counts each kind of damage when the set is damaged (a
 *    check that cannot fail proves nothing);
 *  - a column the classifier refuses is an unnamed slot, listed, and the other columns are named;
 *  - the inventory recorded from the set is the inventory the state manager alone gives, and
 *    the keys, the state labels and the trace labels read from the set are the state manager's;
 *  - the strings and predicates the carried-prior steps take from a typed entry (satellite,
 *    signal, key text, receiver code-bias form) are what the key parsers gave them;
 *  - an ambiguity's unresolvable mark (left out of the integer search) is in the set and is
 *    checked against the state manager's;
 *  - the SD-EWL candidates read from the set are the ones the state manager's ambiguity keys
 *    gave;
 *  - the withheld shared states are kept exactly while their absorbing column is held;
 *  - a parameter with no typed map keeps its slot and is counted;
 *  - the readers that hold an identity as text (an election's key, a funnel's key) find the
 *    column the state manager indexes, and the border key converters read what the key parsers
 *    read;
 *  - a set that no longer holds the state manager's local columns is counted at the start of
 *    an epoch, and a station whose assembly ends before its split has its set rebuilt;
 *  - a step of the station build that throws is recorded in the station's slot and leaves the
 *    set following the state manager (the pre-pass may have changed the columns first), whether
 *    the throw is a standard exception or not, before or after the split built the set;
 *  - the run-end table names the families and prints the findings.
 */

#include <gtest/gtest.h>

#include "atmosphere/GlobalIonosphereMap.hpp"
#include "common/GnssIdentity.hpp"
#include "common/ParamRef.hpp"
#include "config/GnssConfig.hpp"
#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"
#include "measurement/UserParamView.hpp"
#include "network/assembly/ParamInventory.hpp"
#include "network/ar/SdEwlRows.hpp"
#include "network/assembly/StationBuild.hpp"
#include "network/diag/StationBlock.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/state/Entities.hpp"
#include "network/qc/RedundancyCensus.hpp"
#include "network/solve/Estimator.hpp"
#include "network/state/StateColumns.hpp"
#include "network/state/StationParams.hpp"
#include "network/state/StationParamsShadow.hpp"
#include "../support/NetworkTestRun.hpp"
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

namespace {

using PD = ParamDisposition;

/// A network config over GPS, Galileo and GLONASS, so the state manager allocates the
/// inter-system biases of the two non-GPS systems.
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

/// A station-shaped state manager and the split's dispositions for it.
struct Station {
    LogManager                 lm;
    GnssConfig                 cfg;
    std::unique_ptr<StateManager> sm;
    std::vector<ParamDisposition> disp;
    StationAbsorbed            absorbed;

    Station() {
        loadThreeSystemConfig(cfg, lm);
        sm = std::make_unique<StateManager>(5, lm, cfg);
        // Columns in a birth order that is not the key order: later satellites and systems
        // first, signals reversed.
        double v = 0.25;
        auto next = [&] { return v += 0.375; };
        double elevation = 12.5;
        for (const char* sat : {"R07", "E24", "G12", "G05", "E11", "R02"}) {
            sm->addIonosphereState(sat, next());
            sm->addPhaseColourState(sat, 1e-6);
            sm->setPhaseColourElevation(sat, elevation += 6.25);
        }
        for (const char* sat : {"G12", "E24", "G05", "E11", "R07", "R02"}) {
            const std::string s = sat;
            if (s[0] == 'G') {
                sm->addAmbiguity(K::makeAmbiguityKey(s, "L2W"), next(), 10.0);
                sm->addAmbiguity(K::makeAmbiguityKey(s, "L1C"), next(), 10.0);
            } else if (s[0] == 'E') {
                sm->addAmbiguity(K::makeAmbiguityKey(s, "L5Q"), next(), 10.0);
                sm->addAmbiguity(K::makeAmbiguityKey(s, "L1C"), next(), 10.0);
            } else {
                sm->addAmbiguity(K::makeAmbiguityKey(s, "L2C"), next(), 10.0);
                sm->addAmbiguity(K::makeAmbiguityKey(s, "L1C"), next(), 10.0);
            }
        }
        sm->addCodeBiasState(K::makeReceiverIscKey("GPS", "C1W"), next());
        sm->addCodeBiasState(K::makeGlonassPerSlotIfbKey("GLONASS", '2', -4), next());
        sm->addCodeBiasState(K::makeGlonassPerSlotIfbKey("GLONASS", '2', 5), next());
        sm->addCodeBiasState(K::makeIfbKey("GPS", '5'), next());
        sm->addCodeBiasState(K::makeDcbKey("Galileo"), next());
        sm->addCodeBiasState(K::makeDcbKey("GPS"), next());
        sm->addReceiverPhaseBiasState(K::makeReceiverPhaseBiasKey("Galileo", "L1C"), next());
        sm->addReceiverPhaseBiasState(K::makeReceiverPhaseBiasKey("GPS", "L2W"), next());
        sm->addReceiverPhaseBiasState(K::makeReceiverPhaseBiasKey("GPS", "L1C"), next());
        for (const char* sat : {"E24", "G12", "G05", "E11"}) {
            sm->addSatClockState(K::makeSatClockKey(sat), next());
            sm->addSatPhaseBiasState(K::makeSatPhaseBiasKey(sat, sat[0] == 'E' ? "L5Q" : "L2W"), next());
            sm->addSatPhaseBiasState(K::makeSatPhaseBiasKey(sat, "L1C"), next());
            sm->addSatBiasState(K::makeSatBiasKey(sat, sat[0] == 'E' ? "C5Q" : "C2W"), next());
            sm->addSatBiasState(K::makeSatBiasKey(sat, "C1C"), next());
        }
        // The split's decisions: position pinned, border columns estimated in the border,
        // the rest cycling through the other dispositions so that a copied value is visible.
        disp.assign(sm->getStateSize(), PD::EstimatedLocal);
        for (std::size_t k = 0; k < disp.size(); ++k)
            disp[k] = static_cast<PD>((k * 5 + 1) % kParamDispositions);
        for (std::size_t k = 0; k < 3; ++k) disp[sm->getPositionIndex() + k] = PD::Pinned;
    }

    /// The inventory's record of the station, as the split's seam writes it: from the set.
    StationParamSlot inventory(int station = 4) const {
        StationParams set;
        buildStationParams(*sm, disp, absorbed, set);
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

/// A tally and the number of findings of one kind in it.
long long found(const StationParamsCheck& c, SetMismatch m) {
    return c.mismatch[static_cast<std::size_t>(m)];
}

}  // namespace

// ---------------------------------------------------------------------------
// The build
// ---------------------------------------------------------------------------

TEST(StationParamsTest, TheSetHasOneSlotPerColumnInColumnOrder) {
    Station st;
    StationParams set;
    buildStationParams(*st.sm, st.disp, st.absorbed, set);
    EXPECT_TRUE(set.unclassified.empty());
    ASSERT_EQ(set.slots.size(), st.sm->getStateSize());
    EXPECT_EQ(set.decided, set.slots.size());

    const Eigen::VectorXd x = st.sm->getStateVector();
    for (std::size_t k = 0; k < set.slots.size(); ++k) {
        const LocalSlot& s = set.slots[k];
        EXPECT_EQ(std::memcmp(&s.x_lin, &x(static_cast<Eigen::Index>(k)), sizeof(double)), 0) << k;
        EXPECT_EQ(s.disp, st.disp[k]) << k;
        EXPECT_EQ(findParam(set, s.ref), static_cast<int>(k)) << describe(s.ref);
        if (s.ref.kind == NetworkParamKind::PhaseColour) {
            const double want = st.sm->getPhaseColourElevation(userKey(s.ref));
            EXPECT_FALSE(std::isnan(s.elevation_deg)) << k;
            EXPECT_EQ(std::memcmp(&s.elevation_deg, &want, sizeof(double)), 0) << k;
        } else {
            EXPECT_TRUE(std::isnan(s.elevation_deg)) << k;
        }
    }
    EXPECT_EQ(set.untyped, 0);
    EXPECT_EQ(set.position[0], static_cast<int>(st.sm->getPositionIndex()));
    EXPECT_EQ(set.clock, static_cast<int>(st.sm->getClockIndex()));
    EXPECT_EQ(set.zwd, static_cast<int>(st.sm->getTropoIndex()));
    EXPECT_EQ(set.isb.size(), 2u);   // Galileo and GLONASS
    EXPECT_EQ(set.iono.size(), 6u);
    EXPECT_EQ(set.colour.size(), 6u);
    EXPECT_EQ(set.amb.size(), 12u);
    EXPECT_EQ(set.rx_pb.size(), 3u);
    EXPECT_EQ(set.rx_cb.size(), 6u);
    EXPECT_EQ(set.shared.sat_clock.size(), 4u);
    EXPECT_EQ(set.shared.sat_phase_bias.size(), 8u);
    EXPECT_EQ(set.shared.sat_code_bias.size(), 8u);
}

TEST(StationParamsTest, TypedMapsIterateInTheOrderOfTheStateKeys) {
    Station st;
    StationParams set;
    buildStationParams(*st.sm, st.disp, st.absorbed, set);

    // The fixture adds its columns out of key order, so the slot indices a map visits are not
    // ascending: a map that iterated in birth order would visit them ascending.
    for (const std::vector<int>& v : {visited(set.amb), visited(set.iono), visited(set.rx_cb)})
        EXPECT_FALSE(std::is_sorted(v.begin(), v.end()));

    // The name order of the systems: "GLONASS" < "Galileo" (the letter order puts E first).
    ASSERT_EQ(set.isb.size(), 2u);
    EXPECT_EQ(set.isb.begin()->first, System::GLONASS);
    EXPECT_EQ(set.isb.rbegin()->first, System::Galileo);

    // The state manager's map, walked beside the typed map, visits the same columns.
    std::vector<int> text;
    for (const auto& kv : st.sm->getAmbiguities()) text.push_back(static_cast<int>(kv.second));
    EXPECT_EQ(visited(set.amb), text);
    text.clear();
    for (const auto& kv : st.sm->getCodeBiasStates()) text.push_back(static_cast<int>(kv.second));
    EXPECT_EQ(visited(set.rx_cb), text);
    text.clear();
    for (const auto& kv : st.sm->getReceiverPhaseBiasStates()) text.push_back(static_cast<int>(kv.second));
    EXPECT_EQ(visited(set.rx_pb), text);
    text.clear();
    for (const auto& kv : st.sm->getSystemTimeOffsetStates()) text.push_back(static_cast<int>(kv.second));
    EXPECT_EQ(visited(set.isb), text);
}

TEST(StationParamsTest, TheReseedMarkAndTheWithheldStatesAreCarried) {
    Station st;
    const std::string marked = K::makeAmbiguityKey("G05", "L1C");
    st.sm->markAmbiguityPendingReseed(marked);

    // Three withheld states: one whose absorbing ambiguity the station holds, one whose
    // ambiguity it does not, and one whose key no classifier reads.
    noteAbsorbed(st.absorbed, StateContainer::SatPhaseBiasStates, K::makeSatPhaseBiasKey("G05", "L1C"),
                 AbsorbedReason::CoverageGate);
    noteAbsorbed(st.absorbed, StateContainer::SatPhaseBiasStates, K::makeSatPhaseBiasKey("G30", "L1C"),
                 AbsorbedReason::Allowlist);
    noteAbsorbed(st.absorbed, StateContainer::SatPhaseBiasStates, "not a key", AbsorbedReason::Allowlist);

    StationParams set;
    buildStationParams(*st.sm, st.disp, st.absorbed, set);
    long long marks = 0;
    for (const LocalSlot& s : set.slots) {
        if (!s.pending_reseed) continue;
        ++marks;
        EXPECT_EQ(s.ref.kind, NetworkParamKind::Ambiguity);
        EXPECT_EQ(userKey(s.ref), marked);
    }
    EXPECT_EQ(marks, 1);
    ASSERT_EQ(set.shared.absorbed.size(), 1u);
    EXPECT_EQ(set.shared.absorbed_no_absorber, 1);
    ASSERT_EQ(set.unclassified.size(), 1u);
    EXPECT_EQ(set.unclassified[0], "absorbed sat_phase_bias_states:not a key");
    EXPECT_EQ(set.shared.absorbed[0].reason, AbsorbedReason::CoverageGate);
    EXPECT_EQ(set.shared.absorbed[0].ref, satPhaseBiasRef(*SatId::parse("G05"), *SignalId::fromObsCode(System::GPS, "L1C")));
}

TEST(StationParamsTest, AColumnTheClassifierRefusesIsAnUnnamedSlotAndIsListed) {
    Station st;
    const std::size_t refused = st.sm->addAmbiguity("not a satellite key", 0.0, 10.0);
    st.disp.assign(st.sm->getStateSize(), PD::EstimatedLocal);
    StationParams set;
    buildStationParams(*st.sm, st.disp, st.absorbed, set);
    ASSERT_EQ(set.unclassified.size(), 1u);
    EXPECT_EQ(set.unclassified[0], "ambiguities:not a satellite key");

    // Slot k stays column k: the refused column keeps its slot, in no typed map, with its
    // value; every other column is named and found.
    ASSERT_EQ(set.slots.size(), st.sm->getStateSize());
    ASSERT_LT(refused, set.slots.size());
    EXPECT_TRUE(set.slots[refused].unnamed);
    EXPECT_EQ(set.untyped, 1);
    EXPECT_EQ(set.amb.size(), 12u);
    const Eigen::VectorXd x = st.sm->getStateVector();
    EXPECT_EQ(std::memcmp(&set.slots[refused].x_lin, &x(static_cast<Eigen::Index>(refused)), sizeof(double)), 0);
    for (std::size_t k = 0; k < set.slots.size(); ++k)
        if (k != refused) {
            EXPECT_FALSE(set.slots[k].unnamed) << k;
            EXPECT_EQ(findParam(set, set.slots[k].ref), static_cast<int>(k)) << k;
        }

    // The inventory records every named column and lists the refused one, as the state
    // manager alone does.  The check agrees with the state manager on everything but the
    // refused key, which its ambiguity map holds and the typed map does not.
    StationParamSlot inv;
    recordStationParams(set, 4, inv);
    EXPECT_EQ(inv.unclassified, set.unclassified);
    EXPECT_EQ(inv.records.size(), st.sm->getStateSize() - 1);
    StationParamsCheck check;
    checkStationParams(set, *st.sm, st.disp, st.absorbed, inv, 4, 100.0, check);
    EXPECT_EQ(check.mismatches(), 1) << check.first;
    EXPECT_EQ(found(check, SetMismatch::TypedCount), 1) << check.first;
}

TEST(StationParamsTest, ADispositionVectorShorterThanTheStateIsCounted) {
    Station st;
    st.disp.resize(st.disp.size() - 3);
    StationParams set;
    buildStationParams(*st.sm, st.disp, st.absorbed, set);
    EXPECT_EQ(set.decided, set.slots.size() - 3);
    EXPECT_EQ(set.slots.back().disp, PD::Unobserved);

    // The inventory counts the three columns and records none of them, never giving them a
    // disposition by default.
    StationParamSlot inv;
    recordStationParams(set, 4, inv);
    EXPECT_EQ(inv.no_disposition, 3);
    EXPECT_EQ(inv.records.size(), set.slots.size() - 3);
}

// ---------------------------------------------------------------------------
// The lookups
// ---------------------------------------------------------------------------

TEST(StationParamsTest, AddParamKeepsEverySlotAndCountsWhatNoMapHolds) {
    StationParams set;
    const SatId g05 = *SatId::parse("G05");
    const SignalId l1c = *SignalId::fromObsCode(System::GPS, "L1C");
    const SignalId l2w = *SignalId::fromObsCode(System::GPS, "L2W");

    EXPECT_TRUE(addParam(set, LocalSlot{ambiguityRef(g05, l1c), 1.0, PD::EstimatedLocal, false}));
    // The same key a second time: the slot stays (column order), the lookup keeps the first.
    EXPECT_FALSE(addParam(set, LocalSlot{ambiguityRef(g05, l1c), 2.0, PD::EstimatedLocal, false}));
    // Forms only the single-receiver filter allocates have no typed map.
    EXPECT_FALSE(addParam(set, LocalSlot{ambiguityIfRef(g05, SignalPair{l1c, l2w}), 3.0, PD::EstimatedLocal, false}));
    EXPECT_FALSE(addParam(set, LocalSlot{phaseClockRef(), 4.0, PD::EstimatedLocal, false}));
    EXPECT_FALSE(addParam(set, LocalSlot{gradientRef(true), 5.0, PD::EstimatedLocal, false}));
    EXPECT_TRUE(addParam(set, LocalSlot{rxClockRef(), 6.0, PD::EstimatedLocal, false}));
    EXPECT_FALSE(addParam(set, LocalSlot{rxClockRef(), 7.0, PD::EstimatedLocal, false}));

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
// The check
// ---------------------------------------------------------------------------

TEST(StationParamsTest, TheCheckPassesOnASetBuiltFromTheStateManager) {
    Station st;
    st.sm->markAmbiguityPendingReseed(K::makeAmbiguityKey("E24", "L1C"));
    noteAbsorbed(st.absorbed, StateContainer::SatBiasStates, K::makeSatBiasKey("G05", "C1C"),
                 AbsorbedReason::AnchoredUnionCoverage);   // absorbed by the satellite's clock column
    StationParams set;
    buildStationParams(*st.sm, st.disp, st.absorbed, set);
    ASSERT_EQ(set.shared.absorbed.size(), 1u);

    StationParamsCheck check;
    checkStationParams(set, *st.sm, st.disp, st.absorbed, st.inventory(), 4, 259200.0, check);
    EXPECT_EQ(check.mismatches(), 0) << check.first;
    EXPECT_EQ(check.slots, static_cast<long long>(st.sm->getStateSize()));
    EXPECT_TRUE(check.first.empty());
}

TEST(StationParamsTest, TheCheckCountsEachKindOfDamage) {
    // Each case damages a fresh set one way and expects the finding of that kind.
    struct Case {
        const char*  name;
        SetMismatch  kind;
        void (*damage)(StationParams&, StationParamSlot&);
    };
    const Case cases[] = {
        {"a slot dropped", SetMismatch::Size,
         [](StationParams& s, StationParamSlot&) { s.slots.pop_back(); }},
        {"two slots exchanged", SetMismatch::Identity,
         [](StationParams& s, StationParamSlot&) { std::swap(s.slots[10].ref, s.slots[11].ref); }},
        {"a linearization point moved by one ulp", SetMismatch::XLin,
         [](StationParams& s, StationParamSlot&) {
             s.slots[12].x_lin = std::nextafter(s.slots[12].x_lin, 1e300);
         }},
        {"a disposition changed", SetMismatch::Disposition,
         [](StationParams& s, StationParamSlot&) {
             s.slots[13].disp = s.slots[13].disp == PD::Pinned ? PD::Dropped : PD::Pinned;
         }},
        {"a re-seed mark set", SetMismatch::Reseed,
         [](StationParams& s, StationParamSlot&) {
             for (LocalSlot& l : s.slots)
                 if (l.ref.kind == NetworkParamKind::Ambiguity) { l.pending_reseed = true; return; }
         }},
        {"an unresolvable mark set", SetMismatch::Unresolvable,
         [](StationParams& s, StationParamSlot&) {
             for (LocalSlot& l : s.slots)
                 if (l.ref.kind == NetworkParamKind::Ambiguity) { l.unresolvable = true; return; }
         }},
        {"a phase colour elevation moved by one ulp", SetMismatch::Elevation,
         [](StationParams& s, StationParamSlot&) {
             LocalSlot& l = s.slots[static_cast<std::size_t>(s.colour.begin()->second)];
             l.elevation_deg = std::nextafter(l.elevation_deg, 1e300);
         }},
        {"a typed entry removed", SetMismatch::TypedCount,
         [](StationParams& s, StationParamSlot&) { s.amb.erase(s.amb.begin()); }},
        {"two typed entries pointing at each other's slots", SetMismatch::TypedOrder,
         [](StationParams& s, StationParamSlot&) { std::swap(s.iono.begin()->second, s.iono.rbegin()->second); }},
        {"an inventory record altered", SetMismatch::Inventory,
         [](StationParams&, StationParamSlot& inv) { inv.records[20].disp = PD::Dropped; }},
    };
    for (const Case& c : cases) {
        Station st;
        StationParams set;
        buildStationParams(*st.sm, st.disp, st.absorbed, set);
        StationParamSlot inv = st.inventory();
        // Make sure the case does not hide behind an inventory disposition that is already Dropped.
        if (c.kind == SetMismatch::Inventory && inv.records[20].disp == PD::Dropped)
            inv.records[20].disp = PD::Pinned;
        c.damage(set, inv);
        StationParamsCheck check;
        checkStationParams(set, *st.sm, st.disp, st.absorbed, inv, 4, 100.0, check);
        EXPECT_GT(found(check, c.kind), 0) << c.name;
        EXPECT_FALSE(check.first.empty()) << c.name;
    }
}

TEST(StationParamsTest, TheFirstFindingIsKeptWithItsTimeAndKind) {
    Station st;
    StationParams set;
    buildStationParams(*st.sm, st.disp, st.absorbed, set);
    set.slots[12].x_lin += 1.0;
    set.slots[13].x_lin += 1.0;
    StationParamsCheck check;
    checkStationParams(set, *st.sm, st.disp, st.absorbed, st.inventory(), 4, 259215.0, check);
    EXPECT_EQ(found(check, SetMismatch::XLin), 2);
    EXPECT_EQ(check.first.rfind("TOW 259215.0: x_lin: slot 12 ", 0), 0u) << check.first;
}

// ---------------------------------------------------------------------------
// What the readers of the set rely on
// ---------------------------------------------------------------------------

TEST(StationParamsTest, TheInventoryRecordedFromTheSetIsTheStateManagers) {
    Station st;
    noteAbsorbed(st.absorbed, StateContainer::SatPhaseBiasStates, K::makeSatPhaseBiasKey("G05", "L1C"),
                 AbsorbedReason::CoverageGate);
    noteAbsorbed(st.absorbed, StateContainer::SatPhaseBiasStates, K::makeSatPhaseBiasKey("G30", "L1C"),
                 AbsorbedReason::Allowlist);
    noteAbsorbed(st.absorbed, StateContainer::SatBiasStates, K::makeSatBiasKey("E24", "C5Q"),
                 AbsorbedReason::GlonassCodeOnlyScope);
    noteAbsorbed(st.absorbed, StateContainer::SatPhaseBiasStates, "not a key", AbsorbedReason::Allowlist);
    st.sm->addAmbiguity("not a satellite key", 0.0, 10.0);
    st.disp.assign(st.sm->getStateSize(), PD::EstimatedLocal);
    st.disp.resize(st.disp.size() - 2);

    const StationParamSlot got = st.inventory();
    StationParamSlot want;
    recordStationParamsFromState(*st.sm, 4, st.disp, st.absorbed, want);
    EXPECT_EQ(got.filled, want.filled);
    EXPECT_EQ(got.unclassified, want.unclassified);
    EXPECT_EQ(got.no_disposition, want.no_disposition);
    EXPECT_EQ(got.absorbed_no_absorber, want.absorbed_no_absorber);
    ASSERT_EQ(got.records.size(), want.records.size());
    for (std::size_t k = 0; k < got.records.size(); ++k) {
        EXPECT_TRUE(got.records[k].id == want.records[k].id) << k;
        EXPECT_EQ(got.records[k].disp, want.records[k].disp) << k;
        EXPECT_EQ(got.records[k].reason, want.records[k].reason) << k;
    }
    EXPECT_EQ(got.no_disposition, 1);       // the one named column among the two past the vector
    EXPECT_EQ(got.unclassified.size(), 2u); // the refused column, then the refused withheld state
    EXPECT_EQ(got.absorbed_no_absorber, 1);
}

TEST(StationParamsTest, KeyTextAndStateLabelAreTheStateManagers) {
    Station st;
    StationParams set;
    buildStationParams(*st.sm, st.disp, st.absorbed, set);
    const std::vector<std::string> labels = st.sm->getStateLabels();
    ASSERT_EQ(labels.size(), set.slots.size());
    for (std::size_t k = 0; k < set.slots.size(); ++k)
        EXPECT_EQ(stateLabel(set.slots[k], k), labels[k]) << k;

    // The text keys, family by family: the state manager's own keys.
    auto keys = [&](const auto& typed) {
        std::vector<std::string> out;
        for (const auto& kv : typed) out.push_back(keyText(set.slots[static_cast<std::size_t>(kv.second)].ref));
        return out;
    };
    auto textKeys = [](const std::map<std::string, std::size_t>& m) {
        std::vector<std::string> out;
        for (const auto& kv : m) out.push_back(kv.first);
        return out;
    };
    EXPECT_EQ(keys(set.isb), textKeys(st.sm->getSystemTimeOffsetStates()));
    EXPECT_EQ(keys(set.iono), textKeys(st.sm->getIonosphereStates()));
    EXPECT_EQ(keys(set.colour), textKeys(st.sm->getPhaseColourStates()));
    EXPECT_EQ(keys(set.amb), textKeys(st.sm->getAmbiguities()));
    EXPECT_EQ(keys(set.rx_pb), textKeys(st.sm->getReceiverPhaseBiasStates()));
    EXPECT_EQ(keys(set.rx_cb), textKeys(st.sm->getCodeBiasStates()));
    EXPECT_EQ(keys(set.shared.sat_clock), textKeys(st.sm->getSatClockStates()));
    EXPECT_EQ(keys(set.shared.sat_phase_bias), textKeys(st.sm->getSatPhaseBiasStates()));
    EXPECT_EQ(keys(set.shared.sat_code_bias), textKeys(st.sm->getSatBiasStates()));
}

TEST(StationParamsTest, TheCarryReadersGetWhatTheKeyParsersGave) {
    Station st;
    StationParams set;
    buildStationParams(*st.sm, st.disp, st.absorbed, set);

    // Ambiguity: the satellite, the signal and the key the readers build from an entry are the
    // ones the key parser split out of the state manager's key, at the same column.
    ASSERT_EQ(set.amb.size(), st.sm->getAmbiguities().size());
    auto amb = st.sm->getAmbiguities().begin();
    for (const auto& [arc, index] : set.amb) {
        const auto parts = K::parseAmbiguityKey(amb->first);
        ASSERT_TRUE(parts.has_value()) << amb->first;
        EXPECT_EQ(net::satKey(arc.sat), parts->satellite);
        EXPECT_EQ(net::signalKey(arc.signal), parts->signal);
        EXPECT_EQ(keyText(slotAt(set, index).ref), amb->first);
        EXPECT_EQ(static_cast<std::size_t>(index), amb->second);
        ++amb;
    }

    // Slant ionosphere and phase colour: the satellite string is the key.
    auto bySatellite = [&](const std::map<SatId, int>& typed, const std::map<std::string, std::size_t>& text) {
        ASSERT_EQ(typed.size(), text.size());
        auto t = text.begin();
        for (const auto& [sat, index] : typed) {
            EXPECT_EQ(net::satKey(sat), t->first);
            EXPECT_EQ(static_cast<std::size_t>(index), t->second);
            ++t;
        }
    };
    bySatellite(set.iono, st.sm->getIonosphereStates());
    bySatellite(set.colour, st.sm->getPhaseColourStates());

    // Inter-system bias: the constellation name is the key.
    ASSERT_EQ(set.isb.size(), st.sm->getSystemTimeOffsetStates().size());
    auto isb = st.sm->getSystemTimeOffsetStates().begin();
    for (const auto& [sys, index] : set.isb) {
        EXPECT_EQ(std::string(systemName(sys)), isb->first);
        EXPECT_EQ(static_cast<std::size_t>(index), isb->second);
        ++isb;
    }

    // The receiver clock and the wet delay: the state manager's indices, -1 where it has none.
    EXPECT_EQ(static_cast<std::size_t>(set.clock), st.sm->getClockIndex());
    EXPECT_EQ(static_cast<std::size_t>(set.zwd), st.sm->getTropoIndex());

    // Receiver code bias: the form predicates the readers use are the key predicates.
    ASSERT_EQ(set.rx_cb.size(), st.sm->getCodeBiasStates().size());
    auto cb = st.sm->getCodeBiasStates().begin();
    long long per_slot = 0, dcb = 0, ifb = 0;
    for (const auto& [key, index] : set.rx_cb) {
        EXPECT_EQ(keyText(slotAt(set, index).ref), cb->first);
        EXPECT_EQ(key.form == ParamForm::GloSlotIfb, K::isGlonassPerSlotIfbKey(cb->first)) << cb->first;
        EXPECT_EQ(key.form == ParamForm::Dcb, K::parseDcbKey(cb->first).has_value()) << cb->first;
        EXPECT_EQ(key.form == ParamForm::Ifb, K::parseIfbKey(cb->first).has_value()) << cb->first;
        // The rule reads a key as it reads the class of the same column.
        EXPECT_EQ(RxIfbRule::governs(key), RxIfbRule::governs(paramInstanceOf(slotAt(set, index).ref).cls))
            << cb->first;
        per_slot += key.form == ParamForm::GloSlotIfb;
        dcb += key.form == ParamForm::Dcb;
        ifb += RxIfbRule::governs(key);
        ++cb;
    }
    EXPECT_EQ(per_slot, 2);
    EXPECT_EQ(dcb, 2);
    EXPECT_EQ(ifb, 1);
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

/// The span labels the station-block audit built from the state manager's key maps before it
/// read the set (the reference the label builder is checked against).
std::vector<std::string> legacyBlockLabels(const StationAssembly& R, const StateManager& sm, Eigen::Index n_local) {
    std::vector<std::string> labels(static_cast<std::size_t>(n_local));
    auto put = [&](std::size_t state_idx, std::string label) {
        const auto hit = R.h2local.find(static_cast<int>(state_idx));
        if (hit == R.h2local.end() || hit->second < 0) return;
        const Eigen::Index c = static_cast<Eigen::Index>(hit->second);
        if (c >= n_local) return;
        labels[static_cast<std::size_t>(c)] = std::move(label);
    };
    const std::size_t p = sm.getPositionIndex();
    put(p + 0, "pos:x"); put(p + 1, "pos:y"); put(p + 2, "pos:z");
    put(sm.getClockIndex(), "clk");
    put(sm.getTropoIndex(), "zwd");
    for (const auto& [key, idx] : sm.getCodeBiasStates())         put(idx, "rxcb:" + key);
    for (const auto& [sat, idx] : sm.getIonosphereStates())       put(idx, "iono:" + sat);
    for (const auto& [sys, idx] : sm.getSystemTimeOffsetStates()) put(idx, "isb:" + sys);
    for (const auto& [key, idx] : sm.getSatBiasStates())          put(idx, "satbias:" + key);
    for (const auto& [key, idx] : sm.getSatPhaseBiasStates())     put(idx, "satpb:" + key);
    for (const auto& [key, idx] : sm.getReceiverPhaseBiasStates())put(idx, "rxpb:" + key);
    for (const auto& [sat, idx] : sm.getSatClockStates())         put(idx, "satclk:" + sat);
    for (const auto& [key, idx] : sm.getAmbiguities()) {
        const auto parts = K::parseAmbiguityKey(key);
        put(idx, parts ? ("amb:" + parts->satellite + ':' + parts->signal) : ("amb:" + key));
    }
    return labels;
}

/// The labels the redundancy census built from the state manager's key maps before it read
/// the set.
std::map<Eigen::Index, std::string> legacyCensusLabels(const StationAssembly& R, const StateManager& sm) {
    std::map<Eigen::Index, std::string> out;
    constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();
    auto put = [&](std::size_t state_idx, const std::string& label) {
        if (state_idx == kNone) return;
        const auto hit = R.h2local.find(static_cast<int>(state_idx));
        if (hit == R.h2local.end() || hit->second < 0) return;
        out.emplace(static_cast<Eigen::Index>(hit->second), label);
    };
    for (const auto& kv : sm.getIonosphereStates())        put(kv.second, "iono:" + kv.first);
    for (const auto& kv : sm.getAmbiguities())             put(kv.second, "amb:" + kv.first);
    for (const auto& kv : sm.getCodeBiasStates())          put(kv.second, "rxcb:" + kv.first);
    for (const auto& kv : sm.getReceiverPhaseBiasStates()) put(kv.second, "rpb:" + kv.first);
    put(sm.getTropoIndex(), "zwd");
    put(sm.getClockIndex(), "rx_clock");
    return out;
}

}  // namespace

TEST(StationParamsTest, TheTraceLabelsAreWhatTheStateManagersKeyMapsGave) {
    Station st;
    // Every disposition cycles through the fixture, so the block holds a mix of the families;
    // make the whole non-position state the block so that every family is labelled.
    st.disp.assign(st.sm->getStateSize(), PD::EstimatedLocal);
    for (std::size_t k = 0; k < 3; ++k) st.disp[st.sm->getPositionIndex() + k] = PD::Pinned;
    StationParams set;
    buildStationParams(*st.sm, st.disp, st.absorbed, set);
    StationAssembly R;
    localColumns(set, R);
    const Eigen::Index n_local = R.A_r.cols();
    ASSERT_GT(n_local, 0);

    const std::vector<std::string> block = stationBlockLabels(R, set, n_local);
    const std::vector<std::string> block_ref = legacyBlockLabels(R, *st.sm, n_local);
    EXPECT_EQ(block, block_ref);
    // The phase colour has no span label: its columns read col<k> in the audit.
    long long colour_cols = 0, empty = 0;
    for (const std::string& l : block) empty += l.empty();
    for (const auto& kv : set.colour) colour_cols += R.h2local.count(kv.second);
    EXPECT_EQ(empty, colour_cols);
    EXPECT_GT(colour_cols, 0);

    EXPECT_EQ(localColumnLabels(R, set), legacyCensusLabels(R, *st.sm));

    // An unnamed slot is never labelled.
    st.sm->addAmbiguity("not a satellite key", 0.0, 10.0);
    st.disp.assign(st.sm->getStateSize(), PD::EstimatedLocal);
    StationParams set2;
    buildStationParams(*st.sm, st.disp, st.absorbed, set2);
    StationAssembly R2;
    localColumns(set2, R2);
    const std::vector<std::string> block2 = stationBlockLabels(R2, set2, R2.A_r.cols());
    ASSERT_EQ(block2.size(), static_cast<std::size_t>(R2.A_r.cols()));
    EXPECT_TRUE(block2.back().empty());
}

// ---------------------------------------------------------------------------
// The run-end table
// ---------------------------------------------------------------------------

TEST(StationParamsTest, TheReportNamesTheFamiliesAndPrintsTheFindings) {
    Station st;
    std::vector<NetStation> stations(2);
    stations[0].name = "AAAA";
    stations[1].name = "BBBB";
    for (int i = 0; i < 2; ++i) {
        NetStation& n = stations[static_cast<std::size_t>(i)];
        buildStationParams(*st.sm, st.disp, st.absorbed, n.params);
        ++n.params_check.builds;
        checkStationParams(n.params, *st.sm, st.disp, st.absorbed, st.inventory(i), i, 1.0, n.params_check);
    }
    std::vector<std::string> lines = stationParamsReport(stations);
    auto has = [&](const std::string& needle) {
        return std::any_of(lines.begin(), lines.end(),
                           [&](const std::string& l) { return l.find(needle) != std::string::npos; });
    };
    EXPECT_TRUE(has("2 stations built, 2 station-epochs"));
    for (const char* family : {"receiver position", "receiver clock", "zenith wet delay", "inter-system bias",
                               "slant ionosphere", "phase colour", "ambiguity", "receiver phase bias",
                               "receiver code bias", "satellite clock", "satellite phase bias",
                               "satellite code bias"})
        EXPECT_TRUE(has(family)) << family;
    EXPECT_FALSE(has("no typed map"));   // listed only when a slot has none
    EXPECT_TRUE(has("size 0, identity 0, x_lin 0, disposition 0, reseed 0, unresolvable 0, elevation 0, "
                    "lookup 0, inventory 0, typed count 0, typed order 0, stale 0"));
    EXPECT_FALSE(has("WARNING"));
    EXPECT_TRUE(has("wall of the builds and checks"));

    // A finding in one station and a slot with no typed map in the other.
    stations[1].params.slots[12].x_lin += 1.0;
    stations[1].params_check = StationParamsCheck{};
    ++stations[1].params_check.builds;
    checkStationParams(stations[1].params, *st.sm, st.disp, st.absorbed, st.inventory(1), 1, 7.0, stations[1].params_check);
    addParam(stations[0].params, LocalSlot{phaseClockRef(), 0.0, PD::EstimatedLocal, false});
    lines = stationParamsReport(stations);
    EXPECT_TRUE(has("x_lin 1"));
    EXPECT_TRUE(has("WARNING: station BBBB: TOW 7.0: x_lin"));
    EXPECT_TRUE(has("no typed map"));
}

// ---------------------------------------------------------------------------
// Identities held as text, and the freshness of the set
// ---------------------------------------------------------------------------

TEST(StationParamsTest, TheTextLookupsFindTheColumnsTheStateManagerIndexes) {
    Station st;
    StationParams set;
    buildStationParams(*st.sm, st.disp, st.absorbed, set);

    for (const auto& [key, col] : st.sm->getAmbiguities())
        EXPECT_EQ(findAmbiguity(set, key), static_cast<int>(col)) << key;
    EXPECT_EQ(findAmbiguity(set, K::makeAmbiguityKey("G07", "L1C")), -1);   // a satellite the station lacks
    EXPECT_EQ(findAmbiguity(set, "G12"), -1);                               // no signal
    EXPECT_EQ(findAmbiguity(set, ""), -1);

    for (const auto& [sat, col] : st.sm->getIonosphereStates())
        EXPECT_EQ(findSlantIono(set, sat), static_cast<int>(col)) << sat;
    EXPECT_EQ(findSlantIono(set, "G07"), -1);
    EXPECT_EQ(findSlantIono(set, "not a satellite"), -1);

    for (const auto& [key, col] : st.sm->getReceiverPhaseBiasStates()) {
        EXPECT_EQ(findReceiverPhaseBias(set, key), static_cast<int>(col)) << key;
        EXPECT_TRUE(receiverPhaseBiasSignal(key).has_value()) << key;
        EXPECT_EQ(keyText(slotAt(set, static_cast<int>(col)).ref), key);
    }
    EXPECT_EQ(findReceiverPhaseBias(set, K::makeReceiverPhaseBiasKey("GPS", "L5X")), -1);
    EXPECT_EQ(findReceiverPhaseBias(set, "GPS_DCB"), -1);
    EXPECT_FALSE(receiverPhaseBiasSignal("GPS_RPB_").has_value());
    EXPECT_FALSE(receiverPhaseBiasSignal("_RPB_L1C").has_value());
    EXPECT_FALSE(receiverPhaseBiasSignal("Nowhere_RPB_L1C").has_value());

    for (const auto& [key, col] : st.sm->getCodeBiasStates()) {
        EXPECT_EQ(findReceiverCodeBias(set, key), static_cast<int>(col)) << key;
        EXPECT_EQ(keyText(slotAt(set, static_cast<int>(col)).ref), key);
    }
    EXPECT_EQ(findReceiverCodeBias(set, K::makeDcbKey("GLONASS")), -1);       // a system with no code bias here
    EXPECT_EQ(findReceiverCodeBias(set, K::makeIfbKey("GPS", '2')), -1);      // a band the station lacks
    EXPECT_EQ(findReceiverCodeBias(set, K::makeReceiverPhaseBiasKey("GPS", "L1C")), -1);   // another family
    EXPECT_EQ(findReceiverCodeBias(set, ""), -1);
}

TEST(StationParamsTest, TheSdEwlCandidatesAreWhatTheStateManagersAmbiguityKeysGave) {
    Station st;
    NetStation net;
    buildStationParams(*st.sm, st.disp, st.absorbed, net.params);

    // This epoch's phase rows: every second ambiguity column of the state manager.
    std::map<int, int> h2local;
    int n = 0, col = 0;
    for (const auto& kv : st.sm->getAmbiguities())
        if (n++ % 2 == 0) h2local[static_cast<int>(kv.second)] = col++;

    // The candidates the key parser gave: the phase signals that have a row, grouped by
    // satellite and band digit, each list ascending by signal name.
    SdEwlCandidates expected;
    for (const auto& [key, sidx] : st.sm->getAmbiguities()) {
        const auto parts = K::parseAmbiguityKey(key);
        ASSERT_TRUE(parts.has_value()) << key;
        const auto phase = ObservableId::parse(systemOfSat(parts->satellite), parts->signal);
        ASSERT_TRUE(phase.has_value()) << key;
        const auto hit = h2local.find(static_cast<int>(sidx));
        if (hit == h2local.end()) continue;
        expected[{parts->satellite, phase->signal.digit}].emplace_back(parts->signal, hit->second,
                                                                       static_cast<int>(sidx));
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

TEST(StationParamsTest, AnUnresolvableAmbiguityIsMarkedInTheSetAndTheMarkIsChecked) {
    Station st;
    const std::string marked = K::makeAmbiguityKey("G05", "L1C");
    const std::string other  = K::makeAmbiguityKey("E24", "L1C");
    st.sm->markAmbiguityUnresolvable(marked);
    ASSERT_TRUE(st.sm->isAmbiguityUnresolvable(marked));
    ASSERT_FALSE(st.sm->isAmbiguityUnresolvable(other));

    StationParams set;
    buildStationParams(*st.sm, st.disp, st.absorbed, set);
    int n_marked = 0;
    for (const LocalSlot& s : set.slots) {
        if (s.unresolvable) ++n_marked;
        if (s.ref.kind == NetworkParamKind::Ambiguity)
            EXPECT_EQ(s.unresolvable, userKey(s.ref) == marked) << userKey(s.ref);
        else
            EXPECT_FALSE(s.unresolvable) << describe(s.ref);
    }
    EXPECT_EQ(n_marked, 1);
    EXPECT_TRUE(slotAt(set, findAmbiguity(set, marked)).unresolvable);
    EXPECT_FALSE(slotAt(set, findAmbiguity(set, other)).unresolvable);

    StationParamsCheck check;
    checkStationParams(set, *st.sm, st.disp, st.absorbed, st.inventory(), 4, 100.0, check);
    EXPECT_EQ(check.mismatches(), 0) << check.first;

    // The state manager clears every mark at each pre-pass: a set built before that no
    // longer matches it, and the check says so.
    st.sm->clearUnresolvableAmbiguities();
    StationParamsCheck after;
    checkStationParams(set, *st.sm, st.disp, st.absorbed, st.inventory(), 4, 101.0, after);
    EXPECT_EQ(found(after, SetMismatch::Unresolvable), 1);
}

TEST(StationParamsTest, ThePhaseBiasKeyConverterReadsWhatTheKeyParserReads) {
    Station st;
    StationParams set;
    buildStationParams(*st.sm, st.disp, st.absorbed, set);

    ASSERT_FALSE(st.sm->getSatPhaseBiasStates().empty());
    for (const auto& [key, col] : st.sm->getSatPhaseBiasStates()) {
        const auto id = net::satPhaseBiasKey(key);
        ASSERT_TRUE(id.has_value()) << key;
        EXPECT_EQ(set.shared.sat_phase_bias.at(*id), static_cast<int>(col)) << key;
        EXPECT_EQ(keyText(slotAt(set, static_cast<int>(col)).ref), key);
    }
    // A satellite code-bias key ("G05|C1C") is no phase-bias key.
    ASSERT_FALSE(st.sm->getSatBiasStates().empty());
    for (const auto& [key, col] : st.sm->getSatBiasStates())
        EXPECT_FALSE(net::satPhaseBiasKey(key).has_value()) << key;
    for (const char* bad : {"", "|PB", "G05|PB", "G05||PB", "G05|L1C", "G05_L1C|PB", "X99|L1C|PB", "G05|??C|PB"})
        EXPECT_FALSE(net::satPhaseBiasKey(bad).has_value()) << bad;
}

TEST(StationParamsTest, TheCodeBiasColumnsAreTheStateManagersSatelliteBiasStates) {
    Station st;
    StationParams set;
    buildStationParams(*st.sm, st.disp, st.absorbed, set);

    const std::map<std::string, std::size_t>& states = st.sm->getSatBiasStates();
    ASSERT_FALSE(states.empty());
    EXPECT_EQ(set.shared.sat_code_bias.size(), states.size());
    for (const auto& [key, col] : states)
        EXPECT_EQ(keyText(slotAt(set, static_cast<int>(col)).ref), key);
    // Every identity the set holds sits on a column the state manager keys the same way.
    for (const auto& [id, slot] : set.shared.sat_code_bias) {
        const auto it = states.find(keyText(slotAt(set, slot).ref));
        ASSERT_NE(it, states.end()) << net::satKey(id.sat);
        EXPECT_EQ(static_cast<int>(it->second), slot) << net::satKey(id.sat);
    }
}

TEST(StationParamsTest, AStaleSetIsCountedAtTheStartOfTheEpochAndRebuiltAfterAnEarlyExit) {
    Station st;
    NetStation net;
    net.name = "AAAA";
    net.sm = std::move(st.sm);

    // An assembly that ended before its split: the set follows the state manager, with no
    // dispositions, and the start-of-epoch check finds nothing.
    networkRefreshStationParams(net);
    EXPECT_EQ(net.params_check.refreshed, 1);
    EXPECT_EQ(net.params_check.failed, 0);
    ASSERT_EQ(net.params.slots.size(), net.sm->getStateSize());
    EXPECT_EQ(net.params.decided, 0u);
    networkCheckStationParamsFresh(net, 1.0);
    EXPECT_EQ(net.params_check.fresh_checks, 1);
    EXPECT_EQ(found(net.params_check, SetMismatch::Stale), 0);

    // A border column does not make the set stale: the admissions change it every epoch.
    net.sm->addSatClockState(K::makeSatClockKey("G30"), 0.5);
    networkCheckStationParamsFresh(net, 2.0);
    EXPECT_EQ(found(net.params_check, SetMismatch::Stale), 0);

    // A local column born after the set was built does.
    net.sm->addAmbiguity(K::makeAmbiguityKey("G30", "L1C"), 0.5, 10.0);
    networkCheckStationParamsFresh(net, 3.0);
    EXPECT_EQ(found(net.params_check, SetMismatch::Stale), 1);
    EXPECT_NE(net.params_check.first.find("TOW 3.0: stale: ambiguity"), std::string::npos)
        << net.params_check.first;

    // A local column whose value moved does too, in any bit.
    networkRefreshStationParams(net);
    networkCheckStationParamsFresh(net, 4.0);
    EXPECT_EQ(found(net.params_check, SetMismatch::Stale), 1);
    Eigen::VectorXd x = net.sm->getStateVector();
    const std::size_t col = net.sm->getIonosphereStates().begin()->second;
    x(static_cast<Eigen::Index>(col)) = std::nextafter(x(static_cast<Eigen::Index>(col)), 1e9);
    net.sm->setStateVector(x);
    networkCheckStationParamsFresh(net, 5.0);
    EXPECT_EQ(found(net.params_check, SetMismatch::Stale), 2);
}

// ---------------------------------------------------------------------------
// The produce loop's failure path
// ---------------------------------------------------------------------------

namespace {

/// The measurement pre-pass births a column and then the update throws.  The satellite is
/// "G3<Last>", so that two calls on one station birth two columns.
template <char Last>
StationStep bornThenThrows(StationBuild& b) {
    b.st.sm->addAmbiguity(K::makeAmbiguityKey(std::string("G3") + Last, "L1C"), 0.5, 10.0);
    throw std::runtime_error("update failed");
}

/// The same, with an exception that is not a std::exception.
StationStep bornThenThrowsNonStandard(StationBuild& b) {
    b.st.sm->addAmbiguity(K::makeAmbiguityKey("G33", "L1C"), 0.5, 10.0);
    throw 7;
}

/// The station shape whose dispositions the stand-in split below uses.
const Station* g_split_shape = nullptr;

/// The split: the set is built from the state manager with the station's dispositions.
StationStep splitBuildsTheSet(StationBuild& b) {
    buildStationParams(*b.st.sm, g_split_shape->disp, g_split_shape->absorbed, b.st.params);
    return StationStep::Next;
}

StationStep goesOn(StationBuild&) { return StationStep::Next; }
StationStep endsTheBuild(StationBuild&) { return StationStep::Done; }
StationStep throwsLater(StationBuild&) { throw std::runtime_error("a later step failed"); }

/// A step table of steps that do nothing, with @p first and @p second as its first two.
StationBuildSteps stepsOf(StationBuildFn first, StationBuildFn second = &goesOn) {
    StationBuildSteps steps = kStationBuildSteps;
    for (StationBuildStep& step : steps) step.build = &goesOn;
    steps[0].build = first;
    steps[1].build = second;
    return steps;
}

/// A run with one station over the station-shaped state manager: its set as the station's
/// construction builds it, and the build of the first epoch.
struct ProduceFixture {
    Station                              shape;
    NetworkTestRun                       run;
    std::unique_ptr<GlobalIonosphereMap> gim;
    StationAssembly                      slot;

    ProduceFixture() : run(shape.cfg) {
        run.setup().stations.resize(1);
        station().name = "AAAA";
        station().sm   = std::move(shape.sm);
        networkStartStationParams(station());
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

TEST(StationParamsTest, AStepThatThrowsAfterAColumnWasBornLeavesTheSetFollowingTheStateManager) {
    ProduceFixture f;
    NetStation& net = f.station();
    StationBuild b = f.build();
    ASSERT_EQ(net.params.slots.size(), net.sm->getStateSize());

    // What the walk alone leaves behind: the throw escapes with the column born and the set
    // not rebuilt, and the start-of-epoch check sees the difference.
    EXPECT_THROW(networkStationBuild(b, stepsOf(&bornThenThrows<'0'>)), std::runtime_error);
    networkCheckStationParamsFresh(net, 1.0);
    EXPECT_EQ(found(net.params_check, SetMismatch::Stale), 1);
    EXPECT_LT(net.params.slots.size(), net.sm->getStateSize());

    // The produce loop's turn: the throw is recorded in the slot, the column is born and the
    // set holds it, with no dispositions.  The check finds nothing new.
    const long long refreshed = net.params_check.refreshed;
    networkStationAssemble(b, stepsOf(&bornThenThrows<'1'>));
    EXPECT_TRUE(f.slot.threw);
    EXPECT_FALSE(f.slot.ok);
    EXPECT_EQ(f.slot.err, "update failed");
    EXPECT_EQ(net.params_check.refreshed, refreshed + 1);
    EXPECT_EQ(net.params_check.failed, 0);
    ASSERT_EQ(net.params.slots.size(), net.sm->getStateSize());
    EXPECT_EQ(net.params.decided, 0u);
    EXPECT_EQ(net.params.amb.size(), net.sm->getAmbiguities().size());
    EXPECT_GE(findAmbiguity(net.params, K::makeAmbiguityKey("G31", "L1C")), 0);
    networkCheckStationParamsFresh(net, 2.0);
    EXPECT_EQ(found(net.params_check, SetMismatch::Stale), 1);   // the control's finding only
}

TEST(StationParamsTest, AThrowThatIsNotAStdExceptionIsRecordedAndTheSetRebuiltToo) {
    ProduceFixture f;
    NetStation& net = f.station();
    StationBuild b = f.build();

    networkStationAssemble(b, stepsOf(&bornThenThrowsNonStandard));
    EXPECT_TRUE(f.slot.threw);
    EXPECT_EQ(f.slot.err, "unknown exception");
    ASSERT_EQ(net.params.slots.size(), net.sm->getStateSize());
    networkCheckStationParamsFresh(net, 1.0);
    EXPECT_EQ(found(net.params_check, SetMismatch::Stale), 0);
}

TEST(StationParamsTest, AThrowAfterTheSplitBuiltTheSetRebuildsItWithoutDispositions) {
    ProduceFixture f;
    NetStation& net = f.station();
    g_split_shape = &f.shape;
    StationBuild b = f.build();

    // The split built the set with the station's dispositions; a later step throws.
    networkStationAssemble(b, stepsOf(&splitBuildsTheSet, &throwsLater));
    g_split_shape = nullptr;
    EXPECT_TRUE(f.slot.threw);
    EXPECT_EQ(f.slot.err, "a later step failed");
    ASSERT_EQ(net.params.slots.size(), net.sm->getStateSize());
    EXPECT_EQ(net.params.decided, 0u);
    networkCheckStationParamsFresh(net, 1.0);
    EXPECT_EQ(found(net.params_check, SetMismatch::Stale), 0);
}

TEST(StationParamsTest, ABuildThatCompletesOrEndsEarlyLeavesTheSetToItsSteps) {
    ProduceFixture f;
    NetStation& net = f.station();
    StationBuild b = f.build();
    const long long refreshed = net.params_check.refreshed;

    // A complete walk marks the slot ok and rebuilds nothing.
    networkStationAssemble(b, stepsOf(&goesOn));
    EXPECT_TRUE(f.slot.ok);
    EXPECT_FALSE(f.slot.threw);
    EXPECT_EQ(net.params_check.refreshed, refreshed);

    // A step that ends the build leaves its cause in the slot and rebuilds the set itself
    // when it must (networkStationDataRows); the loop adds nothing.
    f.slot = StationAssembly{};
    networkStationAssemble(b, stepsOf(&endsTheBuild));
    EXPECT_FALSE(f.slot.ok);
    EXPECT_FALSE(f.slot.threw);
    EXPECT_EQ(net.params_check.refreshed, refreshed);
}
