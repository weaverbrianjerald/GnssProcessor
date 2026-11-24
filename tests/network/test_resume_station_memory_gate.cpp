/**
 * @file test_resume_station_memory_gate.cpp
 * @brief The resume's tie to the station memory (card RSG): the loader's refusal of the chain restore without it, and
 *        the warm resume's refusal of a snapshot whose station memory section is absent or refused.
 *
 * What these cases pin:
 *   - the loader refuses network.resume_chain_restore under accumulated wind-up without
 *     network.resume_station_memory_restore, and accepts it with it, or without accumulated wind-up;
 *   - a station memory section restores only with this build's version and the run's fingerprint, and a missing one
 *     reads as absent;
 *   - the resume is refused for the station memory only when its restore is armed under accumulated wind-up and the
 *     section is absent or refused;
 *   - under network.state_snapshot_require that refusal is a run error with its reason; without it there is no error
 *     (the run starts cold).
 */

#include <gtest/gtest.h>

#include <fmt/format.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "network/state/StationMemory.hpp"
#include "products/NetworkStateSnapshot.hpp"

using namespace GnssProcessor;

namespace {

struct Loaded {
    bool ok{false};
    GnssConfig config;
    std::string text;
};

/// Loads a two-station network configuration with @p estimation spliced into "estimation" and @p network into
/// "network"; the loader's refusal is a thrown exception, caught here as not ok.
Loaded loadGateConfig(const std::string& tag, const std::string& estimation, const std::string& network) {
    Loaded out;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("resume_sm_gate_" + tag);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path path = dir / "config.json";
    {
        std::ofstream f(path);
        f << "{\n  \"processing\": { \"mode\": \"ppp-rtk-network\", \"active_systems\": [ \"GPS\", \"Galileo\" ] },\n"
             "  \"estimation\": {\n"
          << estimation
          << "    \"estimate_troposphere\": true\n  },\n"
             "  \"network\": {\n    \"clock_datum_station\": \"AAAA\",\n"
             "    \"phase_leveling\": true,\n    \"shared_carry_full_cov\": true,\n"
             "    \"carry_conditional_coupling\": true,\n    \"ambiguity_vehicle\": \"float_kf\",\n"
             "    \"phase_datum_eq34\": true,\n"
             "    \"state_snapshot_path\": \"snap.txt\",\n"
             "    \"state_snapshot_frame_capture\": true,\n    \"state_snapshot_datum_capture\": true,\n"
             "    \"resume_frame_restore\": true,\n    \"resume_datum_restore\": true,\n"
          << network
          << "    \"stations\": [ { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
             "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] } ]\n"
             "  }\n}\n";
    }
    spdlog::drop("gnss_processor");
    spdlog::drop("gnss_config");
    LogManager log;
    log.initialize(dir, LogLevel::Info);
    std::ostringstream oss;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    log.getLogger("gnss_config")->sinks().push_back(sink);
    log.getLogger()->sinks().push_back(sink);
    try {
        out.ok = out.config.loadFromJson(path, log);
    } catch (const std::exception& e) {
        out.ok = false;
        oss << e.what();
    }
    log.getLogger("gnss_config")->flush();
    log.getLogger()->flush();
    out.text = oss.str();
    spdlog::drop("gnss_processor");
    spdlog::drop("gnss_config");
    std::filesystem::remove_all(dir, ec);
    return out;
}

const char* kAccumulated = "    \"accumulate_phase_windup\": true,\n";
const char* kChain = "    \"state_snapshot_chain_capture\": true,\n    \"resume_chain_restore\": true,\n";
const char* kStationMemory =
    "    \"state_snapshot_station_memory_capture\": true,\n    \"resume_station_memory_restore\": true,\n";

bool contains(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

/// A station memory section's header line for @p config (records left out: the header decides).
std::string sectionFor(const GnssConfig& config, const std::string& version = "v4") {
    return fmt::format("STMEM,{},{:016x}\nSMEND\n", version, networkStationMemoryFingerprint(config));
}

}  // namespace

// ---- The loader ------------------------------------------------------------------------------------

TEST(ResumeStationMemoryGateConfig, TheChainRestoreUnderAccumulatedWindUpRequiresTheStationMemory) {
    const Loaded refused = loadGateConfig("refused", kAccumulated, kChain);
    EXPECT_FALSE(refused.ok);
    EXPECT_TRUE(contains(refused.text, "network.resume_chain_restore requires network.resume_station_memory_restore"))
        << refused.text;
    const Loaded tied = loadGateConfig("tied", kAccumulated, std::string(kChain) + kStationMemory);
    ASSERT_TRUE(tied.ok) << tied.text;
    EXPECT_TRUE(tied.config.networkResumeChainRestore());
    EXPECT_TRUE(tied.config.networkResumeStationMemoryRestore());
    const Loaded fractional = loadGateConfig("fractional", "", kChain);
    ASSERT_TRUE(fractional.ok) << fractional.text;
    EXPECT_TRUE(fractional.config.networkResumeChainRestore());
    const Loaded neither = loadGateConfig("neither", kAccumulated, "");
    ASSERT_TRUE(neither.ok) << neither.text;
}

// ---- The section and the resume's refusal ------------------------------------------------------------

TEST(ResumeStationMemoryGate, ASectionRestoresOnlyWithThisBuildsVersionAndTheRunsFingerprint) {
    const Loaded cfg = loadGateConfig("sec", kAccumulated, kStationMemory);
    ASSERT_TRUE(cfg.ok) << cfg.text;
    EXPECT_TRUE(networkStationMemorySectionRefusal(sectionFor(cfg.config), cfg.config).empty());
    EXPECT_TRUE(contains(networkStationMemorySectionRefusal("", cfg.config), "absent"));
    EXPECT_TRUE(contains(networkStationMemorySectionRefusal("STMEM,v4,0000000000000000\nSMEND\n", cfg.config),
                         "fingerprint 0000000000000000 does not match"));
    EXPECT_TRUE(contains(networkStationMemorySectionRefusal(sectionFor(cfg.config, "v1"), cfg.config),
                         "is not this build's"));
}

TEST(ResumeStationMemoryGate, TheResumeIsRefusedOnlyWithTheRestoreArmedUnderAccumulatedWindUp) {
    const Loaded armed = loadGateConfig("armed", kAccumulated, kStationMemory);
    ASSERT_TRUE(armed.ok) << armed.text;
    EXPECT_TRUE(networkStationMemoryResumeRefusal(sectionFor(armed.config), armed.config).empty());
    EXPECT_FALSE(networkStationMemoryResumeRefusal("", armed.config).empty());
    EXPECT_FALSE(networkStationMemoryResumeRefusal("STMEM,v4,0000000000000000\nSMEND\n", armed.config).empty());
    const Loaded unarmed = loadGateConfig("unarmed", kAccumulated, "");
    ASSERT_TRUE(unarmed.ok) << unarmed.text;
    EXPECT_TRUE(networkStationMemoryResumeRefusal("", unarmed.config).empty());
    const Loaded fractional = loadGateConfig("frac", "", kStationMemory);
    ASSERT_TRUE(fractional.ok) << fractional.text;
    EXPECT_FALSE(fractional.config.networkResumeStationMemoryRestore());   // disarmed: no accumulator to save
    EXPECT_TRUE(networkStationMemoryResumeRefusal("", fractional.config).empty());
}

TEST(ResumeStationMemoryGate, UnderRequireTheRefusalIsARunErrorWithItsReasonAndOtherwiseNone) {
    const std::string msg = NetworkStateSnapshotIO::warmResumeRefusalMessage(
        true, WarmResumeRefusal::StationMemory, "snapshot 'x': station memory absent");
    EXPECT_TRUE(contains(msg, "STATION MEMORY section is absent or refused")) << msg;
    EXPECT_TRUE(contains(msg, "station memory absent")) << msg;
    EXPECT_TRUE(NetworkStateSnapshotIO::warmResumeRefusalMessage(false, WarmResumeRefusal::StationMemory, "x").empty());
}
