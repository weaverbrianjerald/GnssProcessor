// Unit tests of the roster's phase-family table (include/network/assembly/PhaseFamily.hpp, network.phase_families):
// the code mapping, the intake at the parse of a station's file (a member's phase, Doppler and signal-strength codes
// read under the identity's names, codes kept, a file that declares both keeps both), and the slip layer's pairing of
// a family phase with the receiver's own code, and the members' stream records (values, settled flags and
// discontinuity counters equal to the identity's at every emission).

#include "network/assembly/PhaseFamily.hpp"
#include "network/slip/RawObservables.hpp"
#include "network/emit/EmitStore.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/SsrCorrectionStore.hpp"

#include "common/PppObservation.hpp"
#include "common/PppUtils.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "products/ProductParser.hpp"
#include "rinex/RinexParser.hpp"
#include "../support/TestTempDir.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

/// The Galileo E1 and E5a families of section 109.1: L1X shares L1C, L5X shares L5Q.
PhaseFamilyTable galileoFamilies() {
    return {{*SignalId::fromObsCode(System::Galileo, "L1X"), *SignalId::fromObsCode(System::Galileo, "L1C")},
            {*SignalId::fromObsCode(System::Galileo, "L5X"), *SignalId::fromObsCode(System::Galileo, "L5Q")}};
}

/// A RINEX 3.04 file of one Galileo satellite and one epoch, with the given type list and values (16-character
/// observation slots: F14.3 value, blank LLI and blank signal strength).
std::filesystem::path writeFixture(const std::filesystem::path& dir, const std::string& name,
                                   const std::vector<std::string>& types, const std::vector<double>& values) {
    const auto path = dir / name;
    std::string list;
    for (const std::string& t : types) list += " " + t;
    char head[128];
    std::snprintf(head, sizeof head, "E%5zu%-54s", types.size(), list.c_str());
    std::string rec = "E11";
    for (double v : values) {
        char b[24];
        std::snprintf(b, sizeof b, "%14.3f  ", v);
        rec += b;
    }
    std::ofstream f(path);
    f << "     3.04           OBSERVATION DATA    M                   RINEX VERSION / TYPE\n"
         "PFAM                                                        MARKER NAME\n"
         "unit test           unit test                               OBSERVER / AGENCY\n"
         "00000               TESTRX              0.0                 REC # / TYPE / VERS\n"
         "00000               TESTANT     NONE                        ANT # / TYPE\n"
         "  3967283.1000   996406.2000  4970111.4000                  APPROX POSITION XYZ\n"
         "        0.0000        0.0000        0.0000                  ANTENNA: DELTA H/E/N\n"
      << head << "SYS / # / OBS TYPES\n"
      << "    30.000                                                  INTERVAL\n"
         "  2026     5    27     0     0    0.0000000     GPS         TIME OF FIRST OBS\n"
         "                                                            END OF HEADER\n"
         "> 2026 05 27 00 00  0.0000000  0  1\n"
      << rec << "\n";
    return path;
}

/// A configuration whose observation path is @p obs (the real loader, as the RINEX tests load theirs).
void loadConfig(GnssConfig& cfg, LogManager& lm, const std::filesystem::path& dir, const std::filesystem::path& obs) {
    const auto jpath = dir / (obs.stem().string() + ".json");
    std::string fwd = obs.string();
    for (char& c : fwd)
        if (c == '\\') c = '/';
    std::ofstream j(jpath);
    j << "{\n  \"processing\": { \"mode\": \"ppp\" },\n  \"files\": { \"observation_path\": \"" << fwd << "\" }\n}\n";
    j.close();
    ASSERT_TRUE(cfg.loadFromJson(jpath, lm));
}

/// The intake's renames of the Galileo families: phase, Doppler and signal strength, codes left as they are.
std::map<System, std::map<std::string, std::string>> intakeRenames() {
    std::map<System, std::map<std::string, std::string>> r;
    for (const PhaseFamilyRow& f : galileoFamilies())
        for (const ObsType ty : {ObsType::Phase, ObsType::Doppler, ObsType::Snr})
            r[System::Galileo][f.member.obsCode(ty)] = f.identity.obsCode(ty);
    return r;
}

}  // namespace

TEST(PhaseFamily, CodeOfMapsAMemberAndLeavesTheRest) {
    const PhaseFamilyTable t = galileoFamilies();
    EXPECT_EQ(phaseFamilyCodeOf(t, System::Galileo, "L1X"), "L1C");
    EXPECT_EQ(phaseFamilyCodeOf(t, System::Galileo, "L5X"), "L5Q");
    EXPECT_EQ(phaseFamilyCodeOf(t, System::Galileo, "L1C"), "L1C");
    EXPECT_EQ(phaseFamilyCodeOf(t, System::Galileo, "L7X"), "L7X");
    EXPECT_EQ(phaseFamilyCodeOf(t, System::GPS, "L1X"), "L1X");   // another constellation
    EXPECT_EQ(phaseFamilyCodeOf({}, System::Galileo, "L1X"), "L1X");   // no table
    const SignalId l1x = *SignalId::fromObsCode(System::Galileo, "L1X");
    EXPECT_EQ(phaseFamilyOf(t, l1x), *SignalId::fromObsCode(System::Galileo, "L1C"));
    EXPECT_EQ(phaseFamilyOf({}, l1x), l1x);
}

TEST(PhaseFamily, TheIntakeReadsAMemberUnderItsIdentityAndKeepsTheCodes) {
    const auto dir = gnss_test::tempDir() / "gnss_phase_family";
    std::filesystem::create_directories(dir);
    const auto obs = writeFixture(dir, "pfam_member_E_30S_MO.rnx", {"C1X", "L1X", "D1X", "S1X", "C5X", "L5X"},
                                  {23456789.125, 123456789.250, -1234.500, 45.000, 23456791.375, 92345678.500});
    LogManager lm;
    GnssConfig cfg;
    loadConfig(cfg, lm, dir, obs);
    ProductParser pp(cfg, lm);
    RinexParser rp(cfg, lm, pp);
    rp.setObsCodeRenames(intakeRenames());
    ASSERT_TRUE(rp.parse());
    const auto& types = rp.getHeader().obs_types.at(System::Galileo);
    EXPECT_EQ(types, (std::vector<std::string>{"C1X", "L1C", "D1C", "S1C", "C5X", "L5Q"}));
    ASSERT_FALSE(rp.getObservations().empty());
    const RinexObservation& o = rp.getObservations().front();
    ASSERT_TRUE(o.measurements.count("L1C"));
    EXPECT_DOUBLE_EQ(o.measurements.at("L1C"), 123456789.250);
    EXPECT_DOUBLE_EQ(o.measurements.at("L5Q"), 92345678.500);
    EXPECT_DOUBLE_EQ(o.measurements.at("C1X"), 23456789.125);   // the code keeps its name
    EXPECT_FALSE(o.measurements.count("L1X"));
    EXPECT_FALSE(o.measurements.count("C1C"));
}

TEST(PhaseFamily, AFileThatDeclaresTheIdentityKeepsTheMember) {
    const auto dir = gnss_test::tempDir() / "gnss_phase_family";
    std::filesystem::create_directories(dir);
    const auto obs = writeFixture(dir, "pfam_both_E_30S_MO.rnx", {"C1C", "L1C", "C1X", "L1X"},
                                  {23456789.125, 123456789.250, 23456789.500, 123456790.250});
    LogManager lm;
    GnssConfig cfg;
    loadConfig(cfg, lm, dir, obs);
    ProductParser pp(cfg, lm);
    RinexParser rp(cfg, lm, pp);
    rp.setObsCodeRenames(intakeRenames());
    ASSERT_TRUE(rp.parse());
    EXPECT_EQ(rp.getHeader().obs_types.at(System::Galileo),
              (std::vector<std::string>{"C1C", "L1C", "C1X", "L1X"}));
}

TEST(PhaseFamily, TheSlipLayerPairsAFamilyPhaseWithTheReceiversOwnCode) {
    // A receiver of the combined tracking after the intake: phases L1C and L5Q, codes C1X and C5X.
    PppObservation o{};
    o.satellite = "E11";
    o.system = System::Galileo;
    o.time = GnssTime{2420, 259200.0};
    o.elevation = 40.0;
    o.raw_observed["L1C"] = 120000000.25;
    o.raw_observed["C1X"] = 22836000.5;
    o.raw_observed["L5Q"] = 89600000.75;
    o.raw_observed["C5X"] = 22836003.0;
    const std::vector<std::vector<PppObservation>> modeled{{o}};
    const std::vector<bool> present{true};
    LogManager lm;
    PppUtils utils{lm};

    RawSlipEpoch plain;
    plain.reset(1);
    networkComputeRawObservables(modeled, present, 1, utils, plain);
    EXPECT_TRUE(plain.rows[0].empty());   // without the table no phase has a code of its name

    const PhaseFamilyTable t = galileoFamilies();
    RawSlipEpoch fam;
    fam.reset(1);
    networkComputeRawObservables(modeled, present, 1, utils, fam, false, false, nullptr, nullptr, false, &t);
    ASSERT_EQ(fam.rows[0].size(), 1u);
    const RawSlipObs& r = fam.rows[0].front();
    EXPECT_EQ(r.sig1, "L1C");
    EXPECT_EQ(r.sig2, "L5Q");
    EXPECT_EQ(r.code1, "C1X");
    EXPECT_EQ(r.code2, "C5X");
    EXPECT_TRUE(r.dual);
}

TEST(PhaseFamily, MemberRecordsRepeatTheIdentityValuesFlagsAndCounters) {
    const PhaseFamilyTable t = galileoFamilies();
    SsrCorrectionStore store;
    NetworkProductEmitter emitter;   // the default 0.05 m threshold
    const auto epoch = [](double tow, double b1, double b5, bool integer) {
        NetworkSolutionEpoch sol;
        sol.epoch_time = GnssTime{2420, tow};
        sol.iod_ssr = 3;
        NetworkSatSolution s;
        s.sat = "E11";
        s.has_clock = true;
        s.phase_bias["L1C"] = NetworkPhaseBias{b1, integer, 1};
        s.phase_bias["L5Q"] = NetworkPhaseBias{b5, integer, 1};
        sol.sats.push_back(s);
        return sol;
    };
    // A steady epoch, a small change, a cycle-level jump on L1C, the flags lowered.
    const std::vector<NetworkSolutionEpoch> seq{epoch(259200.0, -0.70, 0.40, true), epoch(259230.0, -0.71, 0.41, true),
                                                epoch(259260.0, -0.10, 0.41, true), epoch(259290.0, -0.10, 0.41, false)};
    for (NetworkSolutionEpoch sol : seq) {
        EXPECT_EQ(networkPhaseFamilyMemberRecords(t, sol), 2u);
        emitter.emitToStore(sol, store);
        for (const auto& [id, mem] : {std::pair<std::string, std::string>{"L1C", "L1X"}, {"L5Q", "L5X"}}) {
            const auto a = store.getPhaseBias("E11", id);
            const auto b = store.getPhaseBias("E11", mem);
            ASSERT_TRUE(a.has_value());
            ASSERT_TRUE(b.has_value());
            EXPECT_DOUBLE_EQ(b->bias_m, a->bias_m);
            EXPECT_EQ(b->signal_integer, a->signal_integer);
            EXPECT_EQ(b->wide_lane_integer, a->wide_lane_integer);
            EXPECT_EQ(b->discontinuity_counter, a->discontinuity_counter);
            EXPECT_EQ(emitter.discontinuityCounter("E11", mem), emitter.discontinuityCounter("E11", id));
        }
    }
    EXPECT_EQ(emitter.discontinuityCounter("E11", "L1X"), 1);   // the jump stepped the member with its identity
    EXPECT_EQ(emitter.discontinuityCounter("E11", "L5X"), 0);
    // No table: no member record; the chain's released keys gain their members.
    NetworkSolutionEpoch sol = epoch(259320.0, -0.10, 0.41, true);
    EXPECT_EQ(networkPhaseFamilyMemberRecords({}, sol), 0u);
    EXPECT_EQ(sol.sats.front().phase_bias.count("L1X"), 0u);
    const auto keys = withPhaseFamilyMembers(t, {{"E11", "L1C"}, {"G05", "L1C"}});
    EXPECT_EQ(keys.count({"E11", "L1X"}), 1u);
    EXPECT_EQ(keys.count({"G05", "L1X"}), 0u);
    EXPECT_EQ(keys.size(), 3u);
}
