/**
 * @file test_ambiguity_resolver_setup.cpp
 * @brief The one builder of the ambiguity resolver's configuration: it copies every field,
 *        and the real-time engine, which used to drop partial AR and the per-candidate
 *        maturity gate, now builds its resolver from it.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "filter/AmbiguityResolverSetup.hpp"
#include "logging/LogManager.hpp"
#include "realtime/RealTimeEngine.hpp"
#include "support/TestTempDir.hpp"

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace GnssProcessor {

// House friend-shim idiom: expose only the engine seam this suite needs.
struct AmbiguityResolverSetupTestAccess {
    static void initAmbiguityResolver(RealTimeEngine& e) { e.initAmbiguityResolver(); }
    static const AmbiguityResolver* resolver(const RealTimeEngine& e) { return e.ambiguity_resolver_.get(); }
};

} // namespace GnssProcessor

using namespace GnssProcessor;
namespace fs = std::filesystem;

namespace {

/// A PPP-RTK config whose ambiguity-resolution block is @p ar_body.
std::string json(const std::string& ar_body, const std::string& estimation_extra = "") {
    return R"({
      "processing": { "mode": "ppp-rtk", "active_systems": ["GPS"] },
      "estimation": { )" + estimation_extra + R"("ambiguity_resolution": { )" + ar_body + R"( } }
    })";
}

bool loadJson(const std::string& text, GnssConfig& cfg, LogManager& log, const std::string& tag) {
    const fs::path path = gnss_test::tempDir() / ("ar_setup_" + tag + ".json");
    {
        std::ofstream f(path);
        f << text;
    }
    const bool ok = cfg.loadFromJson(path, log);
    std::error_code ec;
    fs::remove(path, ec);
    return ok;
}

TEST(AmbiguityResolverSetupTest, CopiesEveryFieldOfTheResolverConfiguration) {
    LogManager log;
    GnssConfig cfg;
    // Every field set away from the struct's default, each to a different value.
    ASSERT_TRUE(loadJson(json(R"(
        "mode": 2, "ratio_threshold": 2.5, "elevation_mask": 17.0, "hold_variance": 0.002,
        "min_epochs": 7, "max_position_sigma_m": 0.07, "ratio_upper_bound": 5000.0,
        "wl_only": true, "wl_only_pseudo_var": 0.0002, "wl_max_frac": 0.35,
        "min_success_rate": 0.9995, "wl_only_ffr": false, "wl_only_min_arc_count": 12,
        "wl_only_max_innov_cyc": 0.45, "min_arc_info": 2.5, "nl_max_per_pair_frac": 0.12,
        "nl_iono_free_row": true, "enable_partial_ar": true,
        "active_systems": ["GPS", "Galileo"])", "\"state_ambiguity_basis\": \"nl_wl\", "),
        cfg, log, "all"));

    const AmbiguityResolverConfig def;                 // the struct's defaults
    const AmbiguityResolverConfig c = makeAmbiguityResolverConfig(cfg);
    EXPECT_EQ(c.ar_mode, 2);                            EXPECT_NE(c.ar_mode, def.ar_mode);
    EXPECT_DOUBLE_EQ(c.ratio_threshold, 2.5);           EXPECT_NE(c.ratio_threshold, def.ratio_threshold);
    EXPECT_DOUBLE_EQ(c.elevation_mask_ar, 17.0);        EXPECT_NE(c.elevation_mask_ar, def.elevation_mask_ar);
    EXPECT_DOUBLE_EQ(c.hold_variance, 0.002);           EXPECT_NE(c.hold_variance, def.hold_variance);
    EXPECT_EQ(c.min_epochs, 7);                         EXPECT_NE(c.min_epochs, def.min_epochs);
    EXPECT_DOUBLE_EQ(c.max_position_sigma_m, 0.07);     EXPECT_NE(c.max_position_sigma_m, def.max_position_sigma_m);
    EXPECT_DOUBLE_EQ(c.ratio_upper_bound, 5000.0);      EXPECT_NE(c.ratio_upper_bound, def.ratio_upper_bound);
    EXPECT_TRUE(c.wl_only);                             EXPECT_NE(c.wl_only, def.wl_only);
    EXPECT_DOUBLE_EQ(c.wl_only_pseudo_var, 0.0002);     EXPECT_NE(c.wl_only_pseudo_var, def.wl_only_pseudo_var);
    EXPECT_DOUBLE_EQ(c.wl_max_frac, 0.35);              EXPECT_NE(c.wl_max_frac, def.wl_max_frac);
    EXPECT_DOUBLE_EQ(c.min_success_rate, 0.9995);       EXPECT_NE(c.min_success_rate, def.min_success_rate);
    EXPECT_FALSE(c.wl_only_ffr);                        EXPECT_NE(c.wl_only_ffr, def.wl_only_ffr);
    EXPECT_EQ(c.wl_only_min_arc_count, 12);             EXPECT_NE(c.wl_only_min_arc_count, def.wl_only_min_arc_count);
    EXPECT_DOUBLE_EQ(c.wl_only_max_innov_cyc, 0.45);    EXPECT_NE(c.wl_only_max_innov_cyc, def.wl_only_max_innov_cyc);
    EXPECT_DOUBLE_EQ(c.min_arc_info, 2.5);              EXPECT_NE(c.min_arc_info, def.min_arc_info);
    EXPECT_DOUBLE_EQ(c.nl_max_per_pair_frac, 0.12);     EXPECT_NE(c.nl_max_per_pair_frac, def.nl_max_per_pair_frac);
    EXPECT_TRUE(c.nl_iono_free_row);                    EXPECT_NE(c.nl_iono_free_row, def.nl_iono_free_row);
    EXPECT_TRUE(c.enable_partial_ar);                   EXPECT_NE(c.enable_partial_ar, def.enable_partial_ar);
    EXPECT_EQ(c.ar_active_systems, (std::vector<std::string>{ "GPS", "Galileo" }));
    EXPECT_EQ(c.state_ambiguity_basis, StateAmbiguityBasis::NlWl);
    EXPECT_NE(c.state_ambiguity_basis, def.state_ambiguity_basis);
}

TEST(AmbiguityResolverSetupTest, TheTwoFieldsTheOtherBuildersDroppedDefaultToTheStructsDefaults) {
    // A run that sets neither builds the resolver it always built.
    LogManager log;
    GnssConfig cfg;
    ASSERT_TRUE(loadJson(json("\"mode\": 1"), cfg, log, "defaults"));
    const AmbiguityResolverConfig def;
    const AmbiguityResolverConfig c = makeAmbiguityResolverConfig(cfg);
    EXPECT_EQ(c.enable_partial_ar, def.enable_partial_ar);
    EXPECT_FALSE(c.enable_partial_ar);
    EXPECT_DOUBLE_EQ(c.min_arc_info, def.min_arc_info);
    EXPECT_DOUBLE_EQ(c.min_arc_info, 0.0);
}

/// Initialize an engine's resolver with @p ar_body and return what the rt_engine logger said.
struct RealtimeBuild {
    std::string text;
    bool built{ false };
    bool partial{ false };
    double min_arc_info{ -1.0 };
};

RealtimeBuild buildRealtimeResolver(const std::string& ar_body, const std::string& tag) {
    RealtimeBuild out;
    LogManager log;
    const fs::path dir = gnss_test::tempDir() / ("ar_setup_logs_" + tag);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    spdlog::drop("rt_engine");
    log.initialize(dir, LogLevel::Info);
    std::ostringstream oss;
    log.getLogger("rt_engine")->sinks().push_back(std::make_shared<spdlog::sinks::ostream_sink_mt>(oss));

    GnssConfig cfg;
    const fs::path path = dir / "cfg.json";
    {
        std::ofstream f(path);
        f << json(ar_body);
    }
    if (!cfg.loadFromJson(path, log)) return out;
    RealTimeEngine engine(cfg, log);
    AmbiguityResolverSetupTestAccess::initAmbiguityResolver(engine);
    log.getLogger("rt_engine")->flush();
    out.text = oss.str();
    if (const AmbiguityResolver* r = AmbiguityResolverSetupTestAccess::resolver(engine)) {
        out.built = true;
        out.partial = r->getConfig().enable_partial_ar;
        out.min_arc_info = r->getConfig().min_arc_info;
    }
    spdlog::drop("rt_engine");
    return out;
}

TEST(AmbiguityResolverSetupTest, TheRealtimeResolverNowHonoursPartialArAndTheMaturityGate) {
    const RealtimeBuild b = buildRealtimeResolver(
        "\"mode\": 1, \"enable_partial_ar\": true, \"min_arc_info\": 2.5", "on");
    ASSERT_TRUE(b.built);
    EXPECT_TRUE(b.partial);
    EXPECT_DOUBLE_EQ(b.min_arc_info, 2.5);
    // The existing line is unchanged, and one new line says the two values now reach the resolver.
    EXPECT_NE(b.text.find("Real-time AR enabled: mode=1, wl_only=off, ratio>=3.0, elev_mask=20.0 deg"),
              std::string::npos) << b.text;
    EXPECT_NE(b.text.find("Real-time AR: partial_ar=on, min_arc_info=2.50 now reach the resolver"),
              std::string::npos) << b.text;
}

TEST(AmbiguityResolverSetupTest, TheRealtimeLogIsUnchangedWhenNeitherIsSet) {
    const RealtimeBuild b = buildRealtimeResolver("\"mode\": 1", "off");
    ASSERT_TRUE(b.built);
    EXPECT_FALSE(b.partial);
    EXPECT_DOUBLE_EQ(b.min_arc_info, 0.0);
    EXPECT_NE(b.text.find("Real-time AR enabled: mode=1, wl_only=off, ratio>=3.0, elev_mask=20.0 deg"),
              std::string::npos) << b.text;
    EXPECT_EQ(b.text.find("now reach the resolver"), std::string::npos) << b.text;
}

TEST(AmbiguityResolverSetupTest, NoResolverIsBuiltWithTheModeOff) {
    const RealtimeBuild b = buildRealtimeResolver("\"mode\": 0", "mode0");
    EXPECT_FALSE(b.built);
}

} // namespace
