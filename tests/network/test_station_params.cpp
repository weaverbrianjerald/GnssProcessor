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
 *    disposition and the ambiguity's re-seed mark, and every typed lookup finds its slot;
 *  - every typed map iterates in the order of the state manager's map of its family;
 *  - the check passes on that set and counts each kind of damage when the set is damaged (a
 *    check that cannot fail proves nothing);
 *  - a column the classifier refuses is an unnamed slot, listed, and the other columns are named;
 *  - the inventory recorded from the set is the inventory the state manager alone gives, and
 *    the keys, the state labels and the trace labels read from the set are the state manager's;
 *  - the withheld shared states are kept exactly while their absorbing column is held;
 *  - a parameter with no typed map keeps its slot and is counted;
 *  - the run-end table names the families and prints the findings.
 */

#include <gtest/gtest.h>

#include "common/GnssIdentity.hpp"
#include "common/ParamRef.hpp"
#include "config/GnssConfig.hpp"
#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"
#include "measurement/UserParamView.hpp"
#include "network/assembly/ParamInventory.hpp"
#include "network/diag/StationBlock.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/qc/RedundancyCensus.hpp"
#include "network/state/StateColumns.hpp"
#include "network/state/StationParams.hpp"
#include "network/state/StationParamsShadow.hpp"
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
        for (const char* sat : {"R07", "E24", "G12", "G05", "E11", "R02"}) {
            sm->addIonosphereState(sat, next());
            sm->addPhaseColourState(sat, 1e-6);
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
    EXPECT_TRUE(has("size 0, identity 0, x_lin 0, disposition 0, reseed 0, lookup 0, inventory 0, "
                    "typed count 0, typed order 0"));
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
