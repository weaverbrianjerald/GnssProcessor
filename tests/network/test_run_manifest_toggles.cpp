/**
 * @file test_run_manifest_toggles.cpp
 * @brief The run manifest's startup record carries the solve-affecting toggles of the goldens.
 *
 * The record is hand-enumerated, so a solve-affecting toggle that nobody added is silently
 * absent from the provenance of every run that arms it. This file pins four that the
 * golden configs arm and the record once omitted: gross_los_screen,
 * phase_datum_held_basis, carry_coupling_exact_update and clock_anchor_q. Each is written
 * only when armed, so a run that does not arm them keeps the manifest it always had.
 */

#include <gtest/gtest.h>

#include "ConfigLoadCapture.hpp"
#include "network/diag/RunTraces.hpp"
#include "network/state/RunContext.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>

namespace GnssProcessor {
namespace {

namespace fs = std::filesystem;

using TestSupport::LoadedConfig;
using TestSupport::loadNetworkConfigCapturing;

/// Write the startup manifest record for @p cfg and return its parsed "network" object.
nlohmann::json startupNetworkObject(const GnssConfig& cfg, const std::string& tag) {
    const fs::path dir = fs::temp_directory_path() / ("run_manifest_toggles_" + tag);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    NetworkConfig nc = cfg.getNetworkConfig();
    nc.write_run_manifest = true;                      // the switch the record sits behind
    NetworkRunTraces traces(dir);
    const NetworkKnobs knobs{};
    auto log = std::make_shared<spdlog::logger>("run_manifest_toggles");
    traces.writeRunManifestStartup(cfg, nc, knobs, dir / "config.json", 2, 1, log);

    std::ifstream in(dir / "network_run_manifest.json");
    std::string line;
    std::getline(in, line);
    fs::remove_all(dir, ec);
    const nlohmann::json record = nlohmann::json::parse(line);   // throws if the record is not JSON
    return record.at("network");
}

}  // namespace

TEST(RunManifestToggles, ArmedGoldenTogglesAreRecorded) {
    const LoadedConfig r = loadNetworkConfigCapturing("armed",
        "    \"phase_leveling\": true,\n"
        "    \"ambiguity_vehicle\": \"float_kf\",\n"
        "    \"phase_datum_prioritized\": true,\n"
        "    \"phase_datum_held_basis\": true,\n"
        "    \"gross_los_screen\": true,\n"
        "    \"shared_carry_full_cov\": true,\n"
        "    \"carry_conditional_coupling\": true,\n"
        "    \"carry_coupling_time_update\": true,\n"
        "    \"carry_coupling_exact_update\": true,\n"
        "    \"clock_temporal_anchor\": true,\n"
        "    \"clock_anchor_q\": 2.9e-8,\n");
    ASSERT_TRUE(r.ok) << r.text;

    const nlohmann::json net = startupNetworkObject(r.config, "armed");
    EXPECT_EQ(net.value("gross_los_screen", false), true);
    EXPECT_EQ(net.value("phase_datum_held_basis", false), true);
    EXPECT_EQ(net.value("carry_coupling_exact_update", false), true);
    ASSERT_TRUE(net.contains("clock_anchor_q"));
    EXPECT_DOUBLE_EQ(net.at("clock_anchor_q").get<double>(), 2.9e-8);
}

TEST(RunManifestToggles, UnarmedRunKeepsItsManifest) {
    const LoadedConfig r = loadNetworkConfigCapturing("unarmed", "");
    ASSERT_TRUE(r.ok) << r.text;

    const nlohmann::json net = startupNetworkObject(r.config, "unarmed");
    EXPECT_FALSE(net.contains("gross_los_screen"));
    EXPECT_FALSE(net.contains("phase_datum_held_basis"));
    EXPECT_FALSE(net.contains("carry_coupling_exact_update"));
    EXPECT_FALSE(net.contains("clock_anchor_q"));
}

}  // namespace GnssProcessor
