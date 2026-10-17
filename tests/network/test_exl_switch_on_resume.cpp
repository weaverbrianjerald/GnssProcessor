/**
 * @file test_exl_switch_on_resume.cpp
 * @brief The goldens arm the exact update, and the switch-on resume (a
 *        snapshot written without the exact update's tokens resumed into a run that arms them) is
 *        admitted only under network.ctu_switch_on_resume.
 *
 * Gate G5 of the exact update graded the switch-on seam and it failed (the restored ambiguities
 * hold the frozen level), and the ledger rows state the armed knobs as fresh
 * convergence only.  The resume gate admits a snapshot whose fingerprint equals the live
 * fingerprint's stripped twin (these knobs' tokens removed); the twin must therefore be 0
 * unless the knob asks for the seam.
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "network/state/RunContext.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "../support/TestTempDir.hpp"   // per-process, per-test scratch directory

using namespace GnssProcessor;

namespace {

bool loadFieldGoldenWith(const std::function<void(nlohmann::json&)>& edit, GnssConfig& cfg,
                         const std::string& tag) {
    const std::filesystem::path src = std::filesystem::path(GNSS_PROJECT_ROOT) / "config" /
                                      "golden" / "golden_field_refound.json";
    std::ifstream in(src);
    nlohmann::json j = nlohmann::json::parse(in, nullptr, true, /*ignore_comments=*/true);
    edit(j);
    const std::filesystem::path p =
        gnss_test::tempDir() / ("gnss_exl_" + tag + ".json");
    { std::ofstream out(p); out << j.dump(2); }
    LogManager log;
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec;
    std::filesystem::remove(p, ec);
    return ok;
}

/// Build the run context a real run would build and run the fingerprint builder.
void fingerprint(const GnssConfig& cfg, NetworkRunContext& ctx) {
    ctx.carry.snap_path = "state_snapshot.snap";   // the builder is a no-op without a path
    std::vector<NetStation> st(2);
    st[0].name = "AAAA"; st[0].is_pivot = true;
    st[1].name = "BBBB"; st[1].is_pivot = false;
    networkSnapshotFingerprint(ctx, cfg, st);
}

} // namespace

TEST(ExlSwitchOnResume, GoldenArmsTheExactUpdateAndRefusesTheSwitchOnSeam) {
    GnssConfig cfg;
    ASSERT_TRUE(loadFieldGoldenWith([](nlohmann::json&) {}, cfg, "off"));
    EXPECT_TRUE(cfg.networkCarryCouplingExactUpdate());
    EXPECT_FALSE(cfg.networkCtuSwitchOnResume());
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    fingerprint(cfg, ctx);
    EXPECT_NE(ctx.carry.live_fingerprint, 0u);
    // No stripped twin: a snapshot written without these knobs cannot match it.
    EXPECT_EQ(ctx.carry.live_fingerprint_ctu_off, 0u);
}

TEST(ExlSwitchOnResume, TheKnobAdmitsTheStrippedTwin) {
    GnssConfig cfg;
    ASSERT_TRUE(loadFieldGoldenWith([](nlohmann::json& j) {
        j["network"]["ctu_switch_on_resume"] = true;
    }, cfg, "on"));
    EXPECT_TRUE(cfg.networkCtuSwitchOnResume());
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    fingerprint(cfg, ctx);
    EXPECT_NE(ctx.carry.live_fingerprint_ctu_off, 0u);
    EXPECT_NE(ctx.carry.live_fingerprint_ctu_off, ctx.carry.live_fingerprint);
}

TEST(ExlSwitchOnResume, TheKnobIsNotPartOfTheFingerprint) {
    GnssConfig off, on;
    ASSERT_TRUE(loadFieldGoldenWith([](nlohmann::json&) {}, off, "fp_off"));
    ASSERT_TRUE(loadFieldGoldenWith([](nlohmann::json& j) {
        j["network"]["ctu_switch_on_resume"] = true;
    }, on, "fp_on"));
    NetworkRunContext c_off(off, off.getNetworkConfig()), c_on(on, on.getNetworkConfig());
    fingerprint(off, c_off);
    fingerprint(on, c_on);
    // A resume-path policy, not a model change: the live fingerprint is the same.
    EXPECT_EQ(c_off.carry.live_fingerprint, c_on.carry.live_fingerprint);
}
