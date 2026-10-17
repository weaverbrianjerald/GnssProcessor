/**
 * @file ConfigLoadCapture.hpp
 * @brief Test helper: load a minimal PPP-RTK network config and capture the loader's log.
 *
 * Loader refusals and warnings are log text, so the tests that pin them need the text.
 * loadNetworkConfigCapturing() writes a loadable two-station network config with the
 * caller's lines spliced into the "network" block, loads it with a private LogManager, and
 * returns the success flag together with everything the loader logged (the "gnss_config"
 * logger and the default logger, where a refusal is reported).
 */
#pragma once

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>

namespace GnssProcessor {
namespace TestSupport {

/// A minimal loadable PPP-RTK network config; @p network_extra is spliced into "network".
inline std::string networkConfig(const std::string& network_extra) {
    return std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\", \"Galileo\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\", \"Galileo\": \"CB\" }\n"
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

struct LoadedConfig {
    bool ok{false};
    GnssConfig config;   ///< the loaded configuration (meaningful when @c ok)
    std::string text;   ///< everything the loader logged
};

/// Load @p network_extra as a network config and capture the loader's log output.
inline LoadedConfig loadNetworkConfigCapturing(const std::string& tag, const std::string& network_extra) {
    LoadedConfig out;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("config_load_capture_" + tag);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path cfg_path = dir / "config.json";
    {
        std::ofstream f(cfg_path);
        f << networkConfig(network_extra);
    }

    // Only the names this helper creates are dropped (never drop_all()): another live
    // object may hold the other loggers.
    spdlog::drop("gnss_processor");
    spdlog::drop("gnss_config");
    LogManager log;
    log.initialize(dir, LogLevel::Info);
    std::ostringstream oss;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto cfg_log = log.getLogger("gnss_config");
    auto main_log = log.getLogger();   // the loader reports a refusal here
    cfg_log->sinks().push_back(sink);
    main_log->sinks().push_back(sink);

    out.ok = out.config.loadFromJson(cfg_path, log);
    cfg_log->flush();
    main_log->flush();
    out.text = oss.str();
    spdlog::drop("gnss_processor");
    spdlog::drop("gnss_config");
    std::filesystem::remove_all(dir, ec);
    return out;
}

inline bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace TestSupport
}  // namespace GnssProcessor
