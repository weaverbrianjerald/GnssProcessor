/**
 * @file test_service_mode_clock_refusals.cpp
 * @brief The clock_model refusals judge the effective configuration, service_mode included.
 *
 * network.service_mode force-arms network.clock_model late in the network loader. The
 * refusals of network.clock_pivot_frame_q and network.carry_coupling_clock_clone used to
 * read the raw JSON key at their parse sites, so a service_mode config that pairs either
 * knob with the oscillator the profile arms was accepted and ran a combination the
 * loader refuses everywhere else. Both refusals now run after the profile has settled
 * the oscillator.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace GnssProcessor {
namespace {

namespace fs = std::filesystem;

/// A minimal loadable PPP-RTK network config; @p network_extra is spliced into "network".
std::string networkConfig(const std::string& network_extra) {
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

struct Loaded {
    bool ok{false};
    std::string text;   ///< everything the loader logged
};

/// Load @p network_extra as a network config and capture the loader's log output.
Loaded loadCapturing(const std::string& tag, const std::string& network_extra) {
    Loaded out;
    const fs::path dir = fs::temp_directory_path() / ("svc_clock_refusals_" + tag);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path cfg_path = dir / "config.json";
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

    GnssConfig cfg;
    out.ok = cfg.loadFromJson(cfg_path, log);
    cfg_log->flush();
    main_log->flush();
    out.text = oss.str();
    spdlog::drop("gnss_processor");
    spdlog::drop("gnss_config");
    fs::remove_all(dir, ec);
    return out;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// Control: the explicit pairing is refused, with the original wording.
TEST(ServiceModeClockRefusals, ExplicitClockModelWithPivotFrameQIsRefused) {
    const Loaded r = loadCapturing("pq_explicit",
        "    \"shared_carry_full_cov\": true,\n"
        "    \"clock_pivot_frame_q\": true,\n"
        "    \"clock_model\": true,\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(contains(r.text,
        "network.clock_pivot_frame_q refuses network.clock_model (the clock model carries no "
        "satellite clock in the shared border)."))
        << r.text;
}

// service_mode arms the oscillator itself; the refusal must still fire.
TEST(ServiceModeClockRefusals, ServiceModeArmedClockModelWithPivotFrameQIsRefused) {
    const Loaded r = loadCapturing("pq_service",
        "    \"service_mode\": true,\n"
        "    \"shared_carry_full_cov\": true,\n"
        "    \"clock_pivot_frame_q\": true,\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(contains(r.text, "network.clock_pivot_frame_q refuses network.clock_model"))
        << r.text;
    EXPECT_TRUE(contains(r.text, "force-armed by network.service_mode")) << r.text;
}

TEST(ServiceModeClockRefusals, ServiceModeArmedClockModelWithClockCloneIsRefused) {
    const Loaded r = loadCapturing("clone_service",
        "    \"service_mode\": true,\n"
        "    \"shared_carry_full_cov\": true,\n"
        "    \"carry_conditional_coupling\": true,\n"
        "    \"carry_coupling_time_update\": true,\n"
        "    \"carry_coupling_exact_update\": true,\n"
        "    \"scoped_handover_response\": true,\n"
        "    \"handover_stransform\": true,\n"
        "    \"handover_stransform_dense\": true,\n"
        "    \"carry_coupling_clock_clone\": true,\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(contains(r.text, "network.carry_coupling_clock_clone refuses network.clock_model"))
        << r.text;
    EXPECT_TRUE(contains(r.text, "force-armed by network.service_mode")) << r.text;
}

// Controls for the clone config above: without the oscillator it loads, and the explicit
// pairing is refused with the original wording.
TEST(ServiceModeClockRefusals, ClockCloneConfigLoadsWithoutClockModel) {
    const Loaded r = loadCapturing("clone_plain",
        "    \"shared_carry_full_cov\": true,\n"
        "    \"carry_conditional_coupling\": true,\n"
        "    \"carry_coupling_time_update\": true,\n"
        "    \"carry_coupling_exact_update\": true,\n"
        "    \"scoped_handover_response\": true,\n"
        "    \"handover_stransform\": true,\n"
        "    \"handover_stransform_dense\": true,\n"
        "    \"carry_coupling_clock_clone\": true,\n");
    EXPECT_TRUE(r.ok) << r.text;
}

TEST(ServiceModeClockRefusals, ExplicitClockModelWithClockCloneIsRefused) {
    const Loaded r = loadCapturing("clone_explicit",
        "    \"shared_carry_full_cov\": true,\n"
        "    \"carry_conditional_coupling\": true,\n"
        "    \"carry_coupling_time_update\": true,\n"
        "    \"carry_coupling_exact_update\": true,\n"
        "    \"scoped_handover_response\": true,\n"
        "    \"handover_stransform\": true,\n"
        "    \"handover_stransform_dense\": true,\n"
        "    \"carry_coupling_clock_clone\": true,\n"
        "    \"clock_model\": true,\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(contains(r.text,
        "network.carry_coupling_clock_clone refuses network.clock_model (the clock model "
        "carries no satellite clock in the dense border, so there is nothing to clone)."))
        << r.text;
}

// service_mode alone, with neither refused knob, still loads (the profile is untouched).
TEST(ServiceModeClockRefusals, ServiceModeAloneStillLoads) {
    const Loaded r = loadCapturing("svc_alone", "    \"service_mode\": true,\n");
    EXPECT_TRUE(r.ok) << r.text;
}

}  // namespace GnssProcessor
