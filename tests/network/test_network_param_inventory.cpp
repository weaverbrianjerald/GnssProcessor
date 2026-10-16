/**
 * @file test_network_param_inventory.cpp
 * @brief Card NOM step N1: the parameter inventory of network/assembly/ParamInventory.hpp.
 *
 * Five contracts: the one classifier reads every key family a station's state manager
 * can hold (each key built by its @c StateKeySchema maker and read back); the receiver
 * IFB rule names exactly the columns the former substring test named, and its premise is
 * read from the configuration (with and without the GIM weighting, and on the field
 * golden); the station walk lists every column once and leaves none unclassified on a
 * network-shaped state manager, and flags a column the split gave no disposition; the
 * drain keeps a station rejected after its split out of the classes; and the table prints
 * one line per class with the WARNING line where the rule's premise does not hold.
 *
 * Card NOM N1b adds three: the reason table names every gate that withholds a shared
 * state and what takes it up; a withheld state is recorded as absorbed only while the
 * station holds its absorbing column, and the table's note and reason lines state the
 * counts per reason; and the drain counts every roster station that did not reach its
 * split, by cause.
 */
#include <gtest/gtest.h>

#include "common/GnssIdentity.hpp"
#include "config/GnssConfig.hpp"
#include "network/assembly/ParamInventory.hpp"
#include "network/driver/RunTypes.hpp"   // StationAssembly (the drain's input)
#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace GnssProcessor;
namespace K = GnssProcessor::StateKeys;
namespace fs = std::filesystem;

namespace {

using SC = StateContainer;
using PK = NetworkParamKind;
using PD = ParamDisposition;

const std::vector<System> kSystems = {System::GPS, System::Galileo, System::GLONASS,
                                      System::BeiDou, System::QZSS, System::SBAS};

ParamInstanceId classify(SC container, const std::string& key) {
    const auto id = classifyStateColumn(container, key);
    EXPECT_TRUE(id.has_value()) << stateContainerName(container) << " '" << key << "'";
    return id.value_or(ParamInstanceId{});
}

SignalId sig(System s, const char* code) { return *SignalId::fromObsCode(s, code); }

/// A temp config under a name no other test process or test case uses: the tag, a random
/// draw per process and a counter per call.
fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    static const unsigned run_id = std::random_device{}();
    static std::atomic<unsigned> n{0};
    const fs::path p = fs::temp_directory_path()
                     / ("nom_inventory_cfg_" + tag + "_" + std::to_string(run_id) + "_"
                        + std::to_string(n++) + ".json");
    std::ofstream f(p);
    f << body;
    return p;
}

/// A minimal loadable GPS network config; @p network_extra is spliced into the network block.
std::string networkConfig(const std::string& network_extra) {
    return std::string("{\n") +
        "  \"processing\": {\n"
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

RxIfbRule ruleOf(const std::string& tag, const std::string& network_extra) {
    LogManager log;
    const fs::path p = writeTempConfig(tag, networkConfig(network_extra));
    GnssConfig cfg;
    EXPECT_TRUE(cfg.loadFromJson(p, log)) << tag;
    std::error_code ec;
    fs::remove(p, ec);
    return networkRxIfbRule(cfg);
}

}  // namespace

// ---------------------------------------------------------------------------
// The classifier, family by family, from the makers
// ---------------------------------------------------------------------------

TEST(NetworkParamInventoryTest, ClassifiesTheFixedCoreAndTheReceiverClockFamily) {
    EXPECT_EQ(classify(SC::PositionIndex, "").cls.kind, PK::RxPosition);
    const ParamClass clk = classify(SC::ClockIndex, "").cls;
    EXPECT_EQ(clk.kind, PK::RxClock);
    EXPECT_EQ(clk.form, ParamForm::None);
    EXPECT_EQ(clk.sys, System::Unknown);   // the receiver clock: no constellation
    EXPECT_EQ(classify(SC::TropoIndex, "").cls.kind, PK::Zwd);
    for (System s : kSystems) {
        // The inter-system bias: the registry's rx_clock kind, its own form.
        const ParamClass isb = classify(SC::SystemTimeOffsetStates, K::makeSystemTimeOffsetKey(traitsOf(s).name)).cls;
        EXPECT_EQ(isb.kind, PK::RxClock);
        EXPECT_EQ(isb.form, ParamForm::Isb);
        EXPECT_EQ(isb.sys, s);
    }
    EXPECT_FALSE(classifyStateColumn(SC::SystemTimeOffsetStates, "IRNSS").has_value());
    EXPECT_FALSE(classifyStateColumn(SC::Unlisted, "VEL_X").has_value());
}

TEST(NetworkParamInventoryTest, ClassifiesTheSatelliteKeyedFamilies) {
    for (System s : kSystems)
        for (int prn : {1, 9, 10, 37}) {
            const SatId sat{s, prn};
            const ParamInstanceId io = classify(SC::IonosphereStates, K::makeIonosphereKey(sat.str()));
            EXPECT_EQ(io.cls.kind, PK::SlantIono);
            EXPECT_EQ(io.sat, sat);
            EXPECT_EQ(io.cls.sys, s);
            // The phase-colour map is keyed by the bare satellite id
            // (StateManager::addPhaseColourState), as the ionosphere map is: the same key,
            // a different container, a different kind.
            const ParamInstanceId pc = classify(SC::PhaseColourStates, sat.str());
            EXPECT_EQ(pc.cls.kind, PK::PhaseColour);
            EXPECT_EQ(pc.sat, sat);
            const ParamInstanceId clk = classify(SC::SatClockStates, K::makeSatClockKey(sat.str()));
            EXPECT_EQ(clk.cls.kind, PK::SatClock);
            EXPECT_EQ(clk.sat, sat);
        }
    EXPECT_FALSE(classifyStateColumn(SC::IonosphereStates, "G5X").has_value());
    EXPECT_FALSE(classifyStateColumn(SC::SatClockStates, "G05").has_value());   // no "|SC"
}

TEST(NetworkParamInventoryTest, ClassifiesEveryAmbiguityForm) {
    for (System s : kSystems) {
        const SatId sat{s, 5};
        for (const char* code : {"L1C", "L2W", "L5Q", "L7I"}) {
            const auto g = SignalId::fromObsCode(s, code);
            const ParamInstanceId a = classify(SC::Ambiguities, K::makeAmbiguityKey(sat.str(), code));
            EXPECT_EQ(a.cls.kind, PK::Ambiguity);
            EXPECT_EQ(a.cls.form, ParamForm::None);
            EXPECT_EQ(a.sat, sat);
            EXPECT_EQ(a.cls.signal, *g);
            EXPECT_EQ(a.cls.signal_b.digit, '\0');
        }
    }
    const ParamClass iff = classify(SC::Ambiguities, K::makeAmbiguityKey("G05", "L1C_L2W")).cls;
    EXPECT_EQ(iff.form, ParamForm::IfPair);
    EXPECT_EQ(iff.signal, sig(System::GPS, "L1C"));
    EXPECT_EQ(iff.signal_b, sig(System::GPS, "L2W"));
    const ParamInstanceId nl = classify(SC::Ambiguities, K::makeNlAmbiguityKey("E11", '1'));
    EXPECT_EQ(nl.sat, (SatId{System::Galileo, 11}));
    EXPECT_EQ(nl.cls.form, ParamForm::NarrowLane);
    EXPECT_EQ(nl.cls.band, FrequencyBand::GalE1);
    const ParamClass wl = classify(SC::Ambiguities, K::makeWlAmbiguityKey("G05", '5', '1')).cls;
    EXPECT_EQ(wl.form, ParamForm::WideLane);
    EXPECT_EQ(wl.band, FrequencyBand::GpsL1);
    EXPECT_EQ(wl.band_b, FrequencyBand::GpsL5);
    EXPECT_EQ(paramClassLabel(wl), "wide-lane ambiguity, GPS L1-L5");
    // A lane band the constellation does not have is a part that does not parse.
    EXPECT_FALSE(classifyStateColumn(SC::Ambiguities, K::makeNlAmbiguityKey("G05", '7')).has_value());
    // The baseline mode's double-difference key: no network station holds one.
    EXPECT_FALSE(classifyStateColumn(SC::Ambiguities, K::makeDdAmbiguityKey("GPS", "G05", "L1", "G01")).has_value());
    EXPECT_FALSE(classifyStateColumn(SC::Ambiguities, "G05L1C").has_value());
    EXPECT_FALSE(classifyStateColumn(SC::Ambiguities, "G05_L1C_X").has_value());
}

TEST(NetworkParamInventoryTest, ClassifiesTheBiasFamilies) {
    for (System s : kSystems) {
        const std::string name = traitsOf(s).name;
        const ParamClass rpb = classify(SC::ReceiverPhaseBiasStates, K::makeReceiverPhaseBiasKey(name, "L1C")).cls;
        EXPECT_EQ(rpb.kind, PK::RxPhaseBias);
        EXPECT_EQ(rpb.sys, s);
        EXPECT_EQ(rpb.signal, sig(s, "L1C"));

        const ParamClass dcb = classify(SC::CodeBiasStates, K::makeDcbKey(name)).cls;
        EXPECT_EQ(dcb.kind, PK::RxCodeBias);
        EXPECT_EQ(dcb.form, ParamForm::Dcb);
        EXPECT_EQ(dcb.sys, s);
        for (char digit = '1'; digit <= '9'; ++digit) {
            const ParamClass ifb = classify(SC::CodeBiasStates, K::makeIfbKey(name, digit)).cls;
            EXPECT_EQ(ifb.form, ParamForm::Ifb);
            EXPECT_EQ(ifb.sys, s);
            EXPECT_EQ(ifb.band, bandOfDigit(s, digit));   // Unknown for a digit s does not use
        }
        const ParamClass isc = classify(SC::CodeBiasStates, K::makeReceiverIscKey(name, "C1W")).cls;
        EXPECT_EQ(isc.form, ParamForm::Isc);
        EXPECT_EQ(isc.signal, sig(s, "C1W"));

        const SatId sat{s, 12};
        const ParamInstanceId pb = classify(SC::SatPhaseBiasStates, K::makeSatPhaseBiasKey(sat.str(), "L2W"));
        EXPECT_EQ(pb.cls.kind, PK::SatPhaseBias);
        EXPECT_EQ(pb.sat, sat);
        EXPECT_EQ(pb.cls.signal, sig(s, "L2W"));
        const ParamInstanceId cb = classify(SC::SatBiasStates, K::makeSatBiasKey(sat.str(), "C1C"));
        EXPECT_EQ(cb.cls.kind, PK::SatCodeBias);
        EXPECT_EQ(cb.cls.signal, sig(s, "C1C"));
    }
    EXPECT_EQ(paramClassLabel(classify(SC::CodeBiasStates, K::makeIfbKey("GPS", '5')).cls), "receiver IFB, GPS L5");
    for (int slot = -7; slot <= 6; ++slot)
        for (char digit : {'1', '2'}) {
            const ParamInstanceId g = classify(SC::CodeBiasStates, K::makeGlonassPerSlotIfbKey("GLONASS", digit, slot));
            EXPECT_EQ(g.cls.form, ParamForm::GloSlotIfb);
            EXPECT_EQ(g.cls.sys, System::GLONASS);
            EXPECT_EQ(g.cls.band, digit == '1' ? FrequencyBand::GloG1 : FrequencyBand::GloG2);
            EXPECT_EQ(g.glo_slot, slot);
        }
    // The refusal policy.  The forms without a signal are read from the key's shape: an
    // unknown system name stays a receiver IFB (and DCB).  A part that must parse and does
    // not refuses the key: the ISC's signal needs a known system, as the receiver phase
    // bias's does.
    const ParamClass unk = classify(SC::CodeBiasStates, K::makeIfbKey("IRNSS", '5')).cls;
    EXPECT_EQ(unk.form, ParamForm::Ifb);
    EXPECT_EQ(unk.sys, System::Unknown);
    EXPECT_EQ(classify(SC::CodeBiasStates, K::makeDcbKey("IRNSS")).cls.form, ParamForm::Dcb);
    EXPECT_FALSE(classifyStateColumn(SC::CodeBiasStates, K::makeReceiverIscKey("IRNSS", "C5A")).has_value());
    EXPECT_FALSE(classifyStateColumn(SC::ReceiverPhaseBiasStates, K::makeReceiverPhaseBiasKey("IRNSS", "L5A")).has_value());
    EXPECT_FALSE(classifyStateColumn(SC::CodeBiasStates, "GPS_XYZ").has_value());
}

// ---------------------------------------------------------------------------
// The receiver IFB rule
// ---------------------------------------------------------------------------

TEST(NetworkParamInventoryTest, RuleNamesTheColumnsTheSubstringTestNamed) {
    // Every receiver code-bias key StateInitializer can produce: per-band IFB and DCB for
    // every system name, the ISC of every code signal, the GLONASS per-slot IFB.
    std::vector<std::string> keys;
    for (System s : kSystems) {
        const std::string name = traitsOf(s).name;
        keys.push_back(K::makeDcbKey(name));
        for (char b = '0'; b <= '9'; ++b) keys.push_back(K::makeIfbKey(name, b));
        for (char b = '1'; b <= '9'; ++b)
            for (char a : {'C', 'W', 'X', 'Q', 'I', 'P', 'L', 'S'})
                keys.push_back(K::makeReceiverIscKey(name, std::string{'C', b, a}));
    }
    for (int slot = -7; slot <= 6; ++slot)
        for (char band : {'1', '2'}) keys.push_back(K::makeGlonassPerSlotIfbKey("GLONASS", band, slot));

    RxIfbRule rule;
    rule.drop = true;
    long long n_dropped = 0;
    for (const std::string& key : keys) {
        const bool legacy = !K::isGlonassPerSlotIfbKey(key) && key.find("_IFB") != std::string::npos;
        const auto id = classifyStateColumn(SC::CodeBiasStates, key);
        ASSERT_TRUE(id.has_value()) << key;
        EXPECT_EQ(rule.drops(*id), legacy) << key;
        EXPECT_EQ(RxIfbRule::governs(id->cls), legacy) << key;
        n_dropped += legacy ? 1 : 0;
    }
    EXPECT_EQ(n_dropped, 6 * 10);
    rule.drop = false;
    const ParamInstanceId ifb5 = classify(SC::CodeBiasStates, K::makeIfbKey("GPS", '5'));
    EXPECT_FALSE(rule.drops(ifb5));
    EXPECT_TRUE(RxIfbRule::governs(ifb5.cls));   // governed whether or not it drops on this run
}

TEST(NetworkParamInventoryTest, RulePremiseFollowsTheIonosphereWeighting) {
    // W6 in the "auto" posture: the loader forces the GIM weighting on; the premise holds.
    const RxIfbRule gim = ruleOf("gim", "    \"estimate_sat_code_bias\": true,\n");
    EXPECT_TRUE(gim.drop);
    EXPECT_TRUE(gim.iono_weighting);
    EXPECT_FALSE(gim.w6_iono_none);
    EXPECT_TRUE(gim.premise_holds);
    EXPECT_EQ(gim.disposition, PD::HeldAtSeed);

    // W6 with no ionosphere constraint: the weighting is forced off; the premise fails.
    const RxIfbRule none = ruleOf("none", "    \"estimate_sat_code_bias\": true,\n"
                                          "    \"w6_iono_constraint\": \"none\",\n");
    EXPECT_TRUE(none.drop);
    EXPECT_FALSE(none.iono_weighting);
    EXPECT_TRUE(none.w6_iono_none);
    EXPECT_FALSE(none.premise_holds);

    // No satellite code bias: nothing is dropped.
    const RxIfbRule off = ruleOf("off", "");
    EXPECT_FALSE(off.drop);

    // The field golden: W6 with the ionosphere constraint "none".
    LogManager log;
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(fs::path(GNSS_PROJECT_ROOT) / "config" / "golden" /
                                 "golden_field_refound.json", log));
    const RxIfbRule golden = networkRxIfbRule(cfg);
    EXPECT_TRUE(golden.drop);
    EXPECT_FALSE(golden.premise_holds);
}

// ---------------------------------------------------------------------------
// The station walk and the per-station record
// ---------------------------------------------------------------------------

TEST(NetworkParamInventoryTest, EveryColumnOfAStationIsListedOnceAndClassified) {
    LogManager lm;
    GnssConfig cfg;
    StateManager sm{5, lm, cfg};
    sm.addIonosphereState("G05", 0.0);
    sm.addAmbiguity(K::makeAmbiguityKey("G05", "L1C"), 0.0, 10.0);
    sm.addAmbiguity(K::makeAmbiguityKey("G05", "L2W"), 0.0, 10.0);
    sm.addCodeBiasState(K::makeDcbKey("GPS"), 0.0);
    sm.addCodeBiasState(K::makeIfbKey("GPS", '5'), 0.0);
    sm.addCodeBiasState(K::makeReceiverIscKey("GPS", "C1W"), 0.0);
    sm.addSatClockState(K::makeSatClockKey("G05"), 0.0);
    sm.addSatPhaseBiasState(K::makeSatPhaseBiasKey("G05", "L1C"), 0.0);
    sm.addSatBiasState(K::makeSatBiasKey("G05", "C1C"), 0.0);
    sm.addReceiverPhaseBiasState(K::makeReceiverPhaseBiasKey("GPS", "L1C"), 0.0);
    sm.addPhaseColourState("G05", 1e-6);

    const std::vector<StateColumn> columns = listStateColumns(sm);
    ASSERT_EQ(columns.size(), sm.getStateSize());
    std::set<std::size_t> idx;
    for (const StateColumn& c : columns) {
        EXPECT_TRUE(idx.insert(c.index).second) << c.index;
        EXPECT_NE(c.container, SC::Unlisted) << c.key;
    }

    // Every column classified; the dispositions are copied by state index.
    std::vector<ParamDisposition> disp(sm.getStateSize(), PD::EstimatedLocal);
    for (std::size_t k = 0; k < 3; ++k) disp[sm.getPositionIndex() + k] = PD::Pinned;
    StationParamSlot slot;
    recordStationParams(sm, 7, disp, StationAbsorbed{}, slot);
    EXPECT_TRUE(slot.filled);
    EXPECT_TRUE(slot.unclassified.empty());
    EXPECT_EQ(slot.no_disposition, 0);
    EXPECT_EQ(slot.records.size(), sm.getStateSize());
    long long pinned = 0;
    for (const StationParamRecord& r : slot.records) {
        EXPECT_EQ(r.id.station.index, 7);
        if (r.disp == PD::Pinned) {
            ++pinned;
            EXPECT_EQ(r.id.cls.kind, PK::RxPosition);
        }
    }
    EXPECT_EQ(pinned, 3);

    // A disposition vector shorter than the state: the columns past its end are counted,
    // never given a disposition by default.
    disp.resize(sm.getStateSize() - 2);
    recordStationParams(sm, 7, disp, StationAbsorbed{}, slot);
    EXPECT_EQ(slot.no_disposition, 2);
    EXPECT_EQ(slot.records.size(), sm.getStateSize() - 2);
    ParamInventoryTally t;
    t.addStation(7, slot);
    EXPECT_EQ(t.no_disposition, 2);
}

// ---------------------------------------------------------------------------
// The table
// ---------------------------------------------------------------------------

namespace {

/// A slot with a pinned position, a local receiver clock and a held band-5 IFB.
StationParamSlot slotWithHeldIfb(int station) {
    StationParamSlot s;
    s.filled = true;
    for (int k = 0; k < 3; ++k)
        s.records.push_back({classify(SC::PositionIndex, ""), PD::Pinned});
    s.records.push_back({classify(SC::ClockIndex, ""), PD::EstimatedLocal});
    s.records.push_back({classify(SC::CodeBiasStates, K::makeIfbKey("GPS", '5')), PD::HeldAtSeed});
    s.records.push_back({classify(SC::SatClockStates, K::makeSatClockKey("G05")), PD::EstimatedBorder});
    for (auto& r : s.records) r.id.station = StationId{station};
    return s;
}

bool anyLineHas(const std::vector<std::string>& lines, const std::string& needle) {
    for (const auto& l : lines)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

TEST(NetworkParamInventoryTest, TablePrintsOneLinePerClassAndTheWarning) {
    ParamInventoryTally t;
    for (int sta = 0; sta < 3; ++sta) t.addStation(sta, slotWithHeldIfb(sta));
    StationParamSlot odd = slotWithHeldIfb(3);
    odd.unclassified.push_back("unlisted:VEL_X");
    t.addStation(3, odd);
    t.epochs = 1;
    t.week_first = t.week_last = 2313;
    t.tow_first = t.tow_last = 259215.0;

    RxIfbRule rule;
    rule.drop = true;
    rule.premise_holds = false;
    rule.w6_iono_none = true;
    const auto lines = paramInventoryTable(t, rule, "first emitted epoch");
    // Title, header, four classes, one warning, the five integrity lines.
    ASSERT_EQ(lines.size(), 2u + 4u + 1u + 5u);
    EXPECT_EQ(lines[0].rfind("first emitted epoch (week 2313 TOW 259215.0): 4 station(s), 1 assembled epoch(s)", 0), 0u)
        << lines[0];
    EXPECT_NE(lines[0].find("4 station(s)"), std::string::npos);
    EXPECT_TRUE(anyLineHas(lines, "receiver position"));
    EXPECT_TRUE(anyLineHas(lines, "receiver clock"));
    EXPECT_TRUE(anyLineHas(lines, "satellite clock, GPS"));
    EXPECT_TRUE(anyLineHas(lines, "rx_code_bias.ifb"));
    for (std::size_t k = 0; k < kParamDispositions; ++k)   // the header names every disposition
        EXPECT_NE(lines[1].find(paramDispositionName(static_cast<PD>(k))), std::string::npos);
    for (const auto& l : lines) EXPECT_NE(l.back(), ' ') << l;   // no trailing blanks
    const std::string& warn = lines[6];
    EXPECT_EQ(warn.rfind("WARNING: receiver IFB, GPS L5: held at its first-epoch seed at 4 station(s) (4 column(s))", 0), 0u) << warn;
    EXPECT_NE(warn.find("dropped because the satellite code biases are estimated"), std::string::npos);
    EXPECT_NE(warn.find("does not hold on this run"), std::string::npos);
    EXPECT_NE(warn.find("network.w6_iono_constraint none"), std::string::npos);
    EXPECT_EQ(lines[7], "station-epochs that did not reach the split: 0 (no usable observations 0, "
                        "no measurement rows 0, state size mismatch 0, exception 0)");
    EXPECT_EQ(lines[8], "station-epochs rejected after the split: 0");
    EXPECT_EQ(lines[9], "withheld states without their absorbing column at the split: 0");
    EXPECT_EQ(lines[10], "columns without a disposition from the split: 0");
    EXPECT_NE(lines.back().find("unclassified: 1 column(s): unlisted:VEL_X (1)"), std::string::npos) << lines.back();

    // The premise holding, or the rule not dropping: no WARNING line.
    rule.premise_holds = true;
    EXPECT_FALSE(anyLineHas(paramInventoryTable(t, rule, "x"), "WARNING"));
    rule.premise_holds = false;
    rule.drop = false;
    EXPECT_FALSE(anyLineHas(paramInventoryTable(t, rule, "x"), "WARNING"));

    // The manifest record carries the same classes and the rule.
    rule.drop = true;
    const std::string j = paramInventoryJson(t, rule, "first_emitted_epoch");
    EXPECT_EQ(j.rfind("{\"record\":\"param_inventory\",\"scope\":\"first_emitted_epoch\"", 0), 0u);
    EXPECT_NE(j.find("\"band\":\"GPS_L5\",\"band_b\":\"\",\"label\":\"receiver IFB, GPS L5\",\"counts\":[0,0,4,0,0,0,0],\"absorbed_reasons\":{},\"stations\":4,\"rule\":\"rx_ifb\""),
              std::string::npos) << j;
    // A station-wide kind has no constellation: "system" is empty, not "Unknown".
    EXPECT_NE(j.find("\"kind\":\"rx_position\",\"form\":\"\",\"system\":\"\""), std::string::npos) << j;
    EXPECT_EQ(j.find("Unknown"), std::string::npos) << j;
    EXPECT_NE(j.find("\"premise_holds\":false"), std::string::npos);
    EXPECT_NE(j.find("\"unclassified\":1"), std::string::npos);
}

TEST(NetworkParamInventoryTest, ClassOrderPutsTheReceiverClockBeforeItsInterSystemBiases) {
    const ParamClass clk = classify(SC::ClockIndex, "").cls;
    const ParamClass isb = classify(SC::SystemTimeOffsetStates, "Galileo").cls;
    EXPECT_LT(clk, isb);
    EXPECT_FALSE(isb < clk);
    EXPECT_EQ(paramClassLabel(clk), "receiver clock");
    EXPECT_EQ(paramClassLabel(isb), "inter-system bias, Galileo");
    // The table's kind column: the inter-system bias is its own form of rx_clock.
    ParamInventoryTally t;
    StationParamSlot s;
    s.filled = true;
    s.records.push_back({classify(SC::ClockIndex, ""), PD::EstimatedLocal});
    s.records.push_back({classify(SC::SystemTimeOffsetStates, "Galileo"), PD::EstimatedLocal});
    t.addStation(0, s);
    const auto lines = paramInventoryTable(t, RxIfbRule{}, "x");
    ASSERT_GE(lines.size(), 4u);
    EXPECT_EQ(lines[2].rfind("receiver clock", 0), 0u);
    EXPECT_NE(lines[3].find("inter-system bias, Galileo"), std::string::npos);
    EXPECT_NE(lines[3].find(" rx_clock.isb "), std::string::npos) << lines[3];
    // The registry kind order leads: a satellite clock before a receiver clock.
    EXPECT_LT(classify(SC::SatClockStates, K::makeSatClockKey("G05")).cls, clk);
}

TEST(NetworkParamInventoryTest, TallyMergeSumsCountsAndUnitesStations) {
    ParamInventoryTally a, b, run;
    a.addStation(0, slotWithHeldIfb(0));
    a.epochs = 1;
    a.week_first = a.week_last = 2313;
    a.tow_first = a.tow_last = 100.0;
    b.addStation(0, slotWithHeldIfb(0));
    b.addStation(5, slotWithHeldIfb(5));
    b.epochs = 1;
    b.week_first = b.week_last = 2313;
    b.tow_first = b.tow_last = 130.0;
    run.merge(a);
    run.merge(b);
    EXPECT_EQ(run.epochs, 2);
    EXPECT_EQ(run.station_epochs, 3);
    EXPECT_EQ(run.nStations(), 2);
    EXPECT_EQ(run.tow_first, 100.0);
    EXPECT_EQ(run.tow_last, 130.0);
    const ParamClass pos = classify(SC::PositionIndex, "").cls;
    ASSERT_TRUE(run.classes.count(pos));
    EXPECT_EQ(run.classes.at(pos).count(PD::Pinned), 9);
    EXPECT_EQ(run.classes.at(pos).nStations(), 2);
    // An unfilled slot is ignored.
    ParamInventoryTally c;
    c.addStation(9, StationParamSlot{});
    EXPECT_EQ(c.station_epochs, 0);
}

TEST(NetworkParamInventoryTest, DrainKeepsAStationRejectedAfterItsSplitOutOfTheClasses) {
    // Three stations reach the split; the produce phase then rejects one (an exception
    // after the split) and finds no usable block for another.  Only the first is counted
    // in the classes; the other two are counted on their own line.  A fourth had no usable
    // observations (not assembled, empty slot): counted on the unreached line only.
    std::vector<StationAssembly> asm_res(4);
    for (int i = 0; i < 3; ++i) asm_res[static_cast<std::size_t>(i)].param_inv = slotWithHeldIfb(i);
    asm_res[0].ok = true;
    asm_res[1].ok = true;
    asm_res[1].threw = true;
    asm_res[2].ok = false;
    const std::vector<bool> present = {true, true, true, false};
    NetworkParamInventory inv{RxIfbRule{}};
    networkParamInventoryDrain(inv, asm_res, present, 2313, 259200.0);
    EXPECT_EQ(inv.last_epoch.station_epochs, 1);
    EXPECT_EQ(inv.last_epoch.station_epochs_rejected, 2);
    EXPECT_EQ(inv.last_epoch.nStations(), 1);
    EXPECT_EQ(inv.last_epoch.classes.at(classify(SC::PositionIndex, "").cls).count(PD::Pinned), 3);
    EXPECT_EQ(inv.whole_run.epochs, 1);
    EXPECT_EQ(inv.whole_run.station_epochs_rejected, 2);
    EXPECT_EQ(inv.whole_run.station_epochs_unreached, 1);
    EXPECT_EQ(inv.whole_run.unreached_absent, 1);
    const auto lines = paramInventoryTable(inv.whole_run, inv.rx_ifb, "run");
    EXPECT_TRUE(anyLineHas(lines, "station-epochs rejected after the split: 2"));

    // An epoch on which no station reaches the split leaves no class in the last-epoch
    // tally and adds no assembled epoch to the run's; its stations are still counted on
    // the unreached line, and the run's time span reaches it.
    networkParamInventoryDrain(inv, std::vector<StationAssembly>(2), {}, 2313, 259230.0);
    EXPECT_EQ(inv.last_epoch.station_epochs, 0);
    EXPECT_EQ(inv.last_epoch.epochs, 0);
    EXPECT_TRUE(inv.last_epoch.classes.empty());
    EXPECT_EQ(inv.whole_run.epochs, 1);
    EXPECT_EQ(inv.whole_run.station_epochs_unreached, 3);
    EXPECT_EQ(inv.whole_run.unreached_absent, 3);
    EXPECT_EQ(inv.whole_run.tow_last, 259230.0);
}

TEST(NetworkParamInventoryTest, DrainCountsEveryStationThatDidNotReachItsSplitByCause) {
    // Five roster stations: one reaches its split; of the other four, one had no usable
    // observations (not assembled), and three were assembled but stopped before the split
    // (no measurement rows, an H of the wrong width, an exception).
    std::vector<StationAssembly> asm_res(5);
    asm_res[0].param_inv = slotWithHeldIfb(0);
    asm_res[0].ok = true;
    asm_res[2].empty_innov = true;
    asm_res[3].state_mismatch = true;
    asm_res[4].threw = true;
    const std::vector<bool> present = {true, false, true, true, true};
    NetworkParamInventory inv{RxIfbRule{}};
    networkParamInventoryDrain(inv, asm_res, present, 2313, 259200.0);
    const ParamInventoryTally& t = inv.last_epoch;
    EXPECT_EQ(t.station_epochs, 1);
    EXPECT_EQ(t.station_epochs_unreached, 4);
    EXPECT_EQ(t.unreached_absent, 1);
    EXPECT_EQ(t.unreached_no_rows, 1);
    EXPECT_EQ(t.unreached_state_mismatch, 1);
    EXPECT_EQ(t.unreached_exception, 1);
    EXPECT_EQ(t.station_epochs_rejected, 0);
    EXPECT_TRUE(anyLineHas(paramInventoryTable(t, inv.rx_ifb, "x"),
                           "station-epochs that did not reach the split: 4 (no usable "
                           "observations 1, no measurement rows 1, state size mismatch 1, "
                           "exception 1)"));
    const std::string j = paramInventoryJson(t, inv.rx_ifb, "first_emitted_epoch");
    EXPECT_NE(j.find("\"station_epochs_unreached\":4,\"unreached\":{\"absent\":1,\"no_rows\":1,"
                     "\"state_mismatch\":1,\"exception\":1}"),
              std::string::npos) << j;
    // The run's tally carries the same counts.
    EXPECT_EQ(inv.whole_run.station_epochs_unreached, 4);
    EXPECT_EQ(inv.whole_run.unreached_exception, 1);
}

// ---------------------------------------------------------------------------
// Absorbed instances (card NOM N1b)
// ---------------------------------------------------------------------------

TEST(NetworkParamInventoryTest, ReasonTableNamesEveryGateAndWhatTakesItUp) {
    using AR = AbsorbedReason;
    const std::vector<std::pair<AR, std::string>> names = {
        {AR::GlonassCodeOnlyScope,  "glonass_code_only_scope"},
        {AR::CoverageGate,          "coverage_gate"},
        {AR::Allowlist,             "allowlist"},
        {AR::CodeCensusCogate,      "code_census_cogate"},
        {AR::AnchoredUnionCoverage, "anchored_union_coverage"},
        {AR::AnchorHygieneBlock,    "anchor_hygiene_block"},
    };
    ASSERT_EQ(names.size(), kAbsorbedReasons);
    std::set<std::string> labels;
    for (const auto& [r, name] : names) {
        const AbsorbedReasonSpec& s = absorbedReasonSpec(r);
        EXPECT_EQ(s.reason, r);
        EXPECT_EQ(absorbedReasonName(r), name);
        EXPECT_TRUE(labels.insert(s.label).second) << s.label;   // labels are distinct
        EXPECT_NE(std::string(s.rule), "") << name;
        EXPECT_NE(std::string(s.site), "") << name;
        // Every gate withholds a satellite phase bias, which the station's float ambiguity
        // takes up; only the GLONASS scope withholds a code bias.
        EXPECT_EQ(std::string(absorbedBy(r, PK::SatPhaseBias)),
                  "the station's float ambiguity of the same satellite and signal") << name;
        EXPECT_EQ(std::string(absorbedBy(r, PK::SatCodeBias)).empty(),
                  r != AR::GlonassCodeOnlyScope) << name;
        EXPECT_EQ(std::string(absorbedBy(r, PK::Ambiguity)), "") << name;
    }
    EXPECT_NE(std::string(absorbedBy(AR::GlonassCodeOnlyScope, PK::SatCodeBias)).find("per-slot IFB"),
              std::string::npos);
    // The sites: four at allocation, two after it.
    EXPECT_EQ(std::string(absorbedReasonSpec(AR::CoverageGate).site),
              "MeasurementModel::ensureNetworkSharedStates");
    EXPECT_EQ(std::string(absorbedReasonSpec(AR::AnchoredUnionCoverage).site),
              "networkBuildPhaseDatumCandidates");
    EXPECT_EQ(std::string(absorbedReasonSpec(AR::AnchorHygieneBlock).site), "networkAnchorHygiene");
}

TEST(NetworkParamInventoryTest, NoteAbsorbedClassifiesTheWithheldKey) {
    // The gate notes the raw (container, key, reason); the inventory classifies the note.
    StationAbsorbed a;
    noteAbsorbed(a, SC::SatPhaseBiasStates, K::makeSatPhaseBiasKey("G05", "L1W"),
                 AbsorbedReason::Allowlist);
    noteAbsorbed(a, SC::SatBiasStates, K::makeSatBiasKey("R07", "C1C"),
                 AbsorbedReason::GlonassCodeOnlyScope);
    noteAbsorbed(a, SC::SatPhaseBiasStates, "not-a-key", AbsorbedReason::CoverageGate);
    ASSERT_EQ(a.notes.size(), 3u);
    const auto pb_opt = classifyAbsorbed(a.notes[0]);
    const auto cb_opt = classifyAbsorbed(a.notes[1]);
    ASSERT_TRUE(pb_opt.has_value());
    ASSERT_TRUE(cb_opt.has_value());
    const AbsorbedInstance& pb = *pb_opt;
    EXPECT_EQ(pb.id.cls.kind, PK::SatPhaseBias);
    EXPECT_EQ(pb.id.cls.sys, System::GPS);
    EXPECT_EQ(pb.id.sat.str(), "G05");
    EXPECT_EQ(pb.id.cls.signal, sig(System::GPS, "L1W"));
    EXPECT_EQ(pb.reason, AbsorbedReason::Allowlist);
    const AbsorbedInstance& cb = *cb_opt;
    EXPECT_EQ(cb.id.cls.kind, PK::SatCodeBias);
    EXPECT_EQ(cb.id.cls.sys, System::GLONASS);
    EXPECT_EQ(cb.id.cls.signal, sig(System::GLONASS, "C1C"));
    EXPECT_EQ(cb.reason, AbsorbedReason::GlonassCodeOnlyScope);
    // A key the classifier refuses is kept, as a label, never dropped.
    EXPECT_FALSE(classifyAbsorbed(a.notes[2]).has_value());
    EXPECT_EQ(absorbedNoteLabel(a.notes[2]), "sat_phase_bias_states:not-a-key");
}

TEST(NetworkParamInventoryTest, RecordAddsAnAbsorbedInstanceOnlyWithItsAbsorbingColumn) {
    LogManager lm;
    GnssConfig cfg;
    StateManager sm{5, lm, cfg};
    sm.addSatClockState(K::makeSatClockKey("G05"), 0.0);
    sm.addSatClockState(K::makeSatClockKey("R07"), 0.0);
    sm.addAmbiguity(K::makeAmbiguityKey("G05", "L1W"), 0.0, 10.0);

    StationAbsorbed a;
    // Absorbed by the station's G05 L1W ambiguity.
    noteAbsorbed(a, SC::SatPhaseBiasStates, K::makeSatPhaseBiasKey("G05", "L1W"),
                 AbsorbedReason::AnchoredUnionCoverage);
    // No G07 L1W ambiguity at the split: the observation left before it.
    noteAbsorbed(a, SC::SatPhaseBiasStates, K::makeSatPhaseBiasKey("G07", "L1W"),
                 AbsorbedReason::AnchoredUnionCoverage);
    // A satellite code bias is taken up through the satellite's code row (its clock column).
    noteAbsorbed(a, SC::SatBiasStates, K::makeSatBiasKey("R07", "C1C"),
                 AbsorbedReason::GlonassCodeOnlyScope);
    noteAbsorbed(a, SC::SatPhaseBiasStates, "not-a-key", AbsorbedReason::CoverageGate);
    ASSERT_EQ(a.notes.size(), 4u);
    EXPECT_TRUE(absorberPresent(sm, *classifyAbsorbed(a.notes[0])));
    EXPECT_FALSE(absorberPresent(sm, *classifyAbsorbed(a.notes[1])));
    EXPECT_TRUE(absorberPresent(sm, *classifyAbsorbed(a.notes[2])));

    std::vector<ParamDisposition> disp(sm.getStateSize(), PD::EstimatedLocal);
    StationParamSlot slot;
    recordStationParams(sm, 4, disp, a, slot);
    EXPECT_EQ(slot.records.size(), sm.getStateSize() + 2u);
    EXPECT_EQ(slot.absorbed_no_absorber, 1);
    long long absorbed = 0;
    for (const StationParamRecord& r : slot.records) {
        EXPECT_EQ(r.reason.has_value(), r.disp == PD::Absorbed);
        if (r.disp != PD::Absorbed) continue;
        ++absorbed;
        EXPECT_EQ(r.id.station.index, 4);
    }
    EXPECT_EQ(absorbed, 2);
    ASSERT_EQ(slot.unclassified.size(), 1u);
    EXPECT_EQ(slot.unclassified[0], "absorbed sat_phase_bias_states:not-a-key");

    ParamInventoryTally t;
    t.addStation(4, slot);
    EXPECT_EQ(t.absorbed_no_absorber, 1);
    EXPECT_EQ(t.unclassified, 1);
    const ParamClass pb = classify(SC::SatPhaseBiasStates, K::makeSatPhaseBiasKey("G05", "L1W")).cls;
    ASSERT_TRUE(t.classes.count(pb));
    EXPECT_EQ(t.classes.at(pb).count(PD::Absorbed), 1);
    EXPECT_EQ(t.classes.at(pb).countAbsorbed(AbsorbedReason::AnchoredUnionCoverage), 1);
    EXPECT_TRUE(anyLineHas(paramInventoryTable(t, RxIfbRule{}, "x"),
                           "withheld states without their absorbing column at the split: 1"));
}

TEST(NetworkParamInventoryTest, TableNoteAndReasonLinesStateTheAbsorbedCounts) {
    // GPS L1W: the border carries 3 satellite phase-bias columns; 2 more were withheld by
    // the coverage gate and 3 by the anchored-union coverage gate, so the ambiguity count
    // (8) equals border plus absorbed.
    const ParamInstanceId pb = classify(SC::SatPhaseBiasStates, K::makeSatPhaseBiasKey("G05", "L1W"));
    const ParamInstanceId amb = classify(SC::Ambiguities, K::makeAmbiguityKey("G05", "L1W"));
    StationParamSlot s;
    s.filled = true;
    for (int k = 0; k < 3; ++k) s.records.push_back({pb, PD::EstimatedBorder});
    for (int k = 0; k < 2; ++k) s.records.push_back({pb, PD::Absorbed, AbsorbedReason::CoverageGate});
    for (int k = 0; k < 3; ++k) s.records.push_back({pb, PD::Absorbed, AbsorbedReason::AnchoredUnionCoverage});
    for (int k = 0; k < 8; ++k) s.records.push_back({amb, PD::EstimatedLocal});
    ParamInventoryTally t;
    t.addStation(0, s);
    t.epochs = 1;
    const auto lines = paramInventoryTable(t, RxIfbRule{}, "first emitted epoch");

    std::string pb_line, amb_line;
    for (const auto& l : lines) {
        if (l.rfind("satellite phase bias, GPS L1W", 0) == 0) pb_line = l;
        if (l.rfind("ambiguity, GPS L1W", 0) == 0) amb_line = l;
    }
    ASSERT_FALSE(pb_line.empty());
    ASSERT_FALSE(amb_line.empty());
    EXPECT_NE(pb_line.find("absorbed: coverage allowlist 2, anchored-union coverage 3"), std::string::npos)
        << pb_line;
    EXPECT_EQ(amb_line.find("absorbed:"), std::string::npos) << amb_line;   // no note without absorbed
    // One reason line per gate that withheld something, after the classes.
    EXPECT_TRUE(anyLineHas(lines,
        "absorbed, coverage allowlist: 2 instance(s), withheld when the signal is outside the "
        "coverage allowlist built from network.phase_bias_min_station_fraction"));
    EXPECT_TRUE(anyLineHas(lines,
        "absorbed, anchored-union coverage: 3 instance(s), withheld when fewer than "
        "network.phase_datum_min_stations stations hold the key this epoch "
        "(networkBuildPhaseDatumCandidates); satellite phase bias taken up by the station's "
        "float ambiguity of the same satellite and signal"));
    EXPECT_FALSE(anyLineHas(lines, "absorbed, GLONASS"));
    for (const auto& l : lines) EXPECT_NE(l.back(), ' ') << l;

    // The manifest record: the absorbed count in the counts, the reasons beside it, and
    // the reason table.
    const std::string j = paramInventoryJson(t, RxIfbRule{}, "first_emitted_epoch");
    EXPECT_NE(j.find("\"label\":\"satellite phase bias, GPS L1W\",\"counts\":[3,0,0,5,0,0,0],"
                     "\"absorbed_reasons\":{\"coverage_gate\":2,\"anchored_union_coverage\":3}"),
              std::string::npos) << j;
    EXPECT_NE(j.find("\"reasons\":[{\"reason\":\"glonass_code_only_scope\""), std::string::npos) << j;
    EXPECT_NE(j.find("{\"reason\":\"anchor_hygiene_block\""), std::string::npos) << j;
    EXPECT_NE(j.find("\"absorbed_no_absorber\":0"), std::string::npos) << j;
}
