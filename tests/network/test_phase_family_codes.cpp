// Unit tests of the phase-family table's codes-shared form (network.phase_family_codes; card NRF section 118): the
// intake reads a member's codes under the identity's names too, and the stream's member code-bias records repeat the
// identity's value and discontinuity counter at every emission, as the member phase-bias records do.

#include "network/assembly/PhaseFamily.hpp"
#include "network/emit/EmitStore.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/SsrCorrectionStore.hpp"
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

PhaseFamilyTable galileoFamilies() {
    return {{*SignalId::fromObsCode(System::Galileo, "L1X"), *SignalId::fromObsCode(System::Galileo, "L1C")},
            {*SignalId::fromObsCode(System::Galileo, "L5X"), *SignalId::fromObsCode(System::Galileo, "L5Q")}};
}

/// A RINEX 3.04 file of one Galileo satellite and one epoch with the given types and values.
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

}  // namespace

TEST(PhaseFamilyCodes, TheIntakeReadsAMembersCodesUnderTheIdentity) {
    const auto dir = gnss_test::tempDir() / "gnss_phase_family_codes";
    std::filesystem::create_directories(dir);
    const auto obs = writeFixture(dir, "pfamc_member_E_30S_MO.rnx", {"C1X", "L1X", "D1X", "S1X", "C5X", "L5X"},
                                  {23456789.125, 123456789.250, -1234.500, 45.000, 23456791.375, 92345678.500});
    LogManager lm;
    GnssConfig cfg;
    loadConfig(cfg, lm, dir, obs);
    ProductParser pp(cfg, lm);
    RinexParser rp(cfg, lm, pp);
    std::map<System, std::map<std::string, std::string>> renames;
    for (const PhaseFamilyRow& f : galileoFamilies())
        for (const ObsType ty : {ObsType::Phase, ObsType::Doppler, ObsType::Snr, ObsType::Code})
            renames[System::Galileo][f.member.obsCode(ty)] = f.identity.obsCode(ty);
    rp.setObsCodeRenames(renames);
    ASSERT_TRUE(rp.parse());
    EXPECT_EQ(rp.getHeader().obs_types.at(System::Galileo),
              (std::vector<std::string>{"C1C", "L1C", "D1C", "S1C", "C5Q", "L5Q"}));
    ASSERT_FALSE(rp.getObservations().empty());
    const RinexObservation& o = rp.getObservations().front();
    EXPECT_DOUBLE_EQ(o.measurements.at("C1C"), 23456789.125);
    EXPECT_DOUBLE_EQ(o.measurements.at("C5Q"), 23456791.375);
    EXPECT_FALSE(o.measurements.count("C1X"));
}

TEST(PhaseFamilyCodes, MemberCodeRecordsRepeatTheIdentityValueAndCounter) {
    const PhaseFamilyTable t = galileoFamilies();
    SsrCorrectionStore store;
    // The code jump test on, so a jump of the identity's code bias steps its counter.
    NetworkProductEmitter emitter(NetworkProductEmitter::kDefaultDiscThresholdM, false, 0.0, /*code_bias_jump_test=*/true);
    emitter.setPhaseFamilyMembers({{{'E', "L1X"}, "L1C"}, {{'E', "L5X"}, "L5Q"},
                                   {{'E', "C1X"}, "C1C"}, {{'E', "C5X"}, "C5Q"}});
    const auto epoch = [](double tow, double c1) {
        NetworkSolutionEpoch sol;
        sol.epoch_time = GnssTime{2420, tow};
        sol.iod_ssr = 3;
        NetworkSatSolution s;
        s.sat = "E11";
        s.has_clock = true;
        s.code_bias_m["C1C"] = c1;
        s.code_bias_m["C5Q"] = -1.2;
        s.phase_bias["L1C"] = NetworkPhaseBias{0.3, true, 1};
        s.phase_bias["L5Q"] = NetworkPhaseBias{0.4, true, 1};
        sol.sats.push_back(s);
        return sol;
    };
    // Epoch 1 without the codes-shared copy (the identity alone gets a baseline); epoch 2 with it and a 0.3 m jump of
    // C1C: the identity steps, the member is a first sighting and would stay at 0 by its own rules.
    NetworkSolutionEpoch e1 = epoch(259200.0, 0.50);
    EXPECT_EQ(networkPhaseFamilyMemberRecords(t, e1, /*codes=*/false), 2u);
    EXPECT_EQ(e1.sats.front().code_bias_m.count("C1X"), 0u);
    emitter.emitToStore(e1, store);
    NetworkSolutionEpoch e2 = epoch(259230.0, 0.80);
    EXPECT_EQ(networkPhaseFamilyMemberRecords(t, e2, /*codes=*/true), 4u);
    EXPECT_DOUBLE_EQ(e2.sats.front().code_bias_m.at("C1X"), 0.80);
    EXPECT_DOUBLE_EQ(e2.sats.front().code_bias_m.at("C5X"), -1.2);
    emitter.emitToStore(e2, store);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("E11", "C1C"), 1);
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("E11", "C1X"), 1);   // follows its identity
    EXPECT_EQ(emitter.codeBiasDiscontinuityCounter("E11", "C5X"), emitter.codeBiasDiscontinuityCounter("E11", "C5Q"));
    const auto a = store.getCodeBiasDiscontinuity("E11", "C1C");
    const auto b = store.getCodeBiasDiscontinuity("E11", "C1X");
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(*b, *a);
    EXPECT_DOUBLE_EQ(store.getCodeBias("E11", "C1X"), store.getCodeBias("E11", "C1C"));
}
