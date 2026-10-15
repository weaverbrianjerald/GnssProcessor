/**
 * @file test_network_engine_spec.cpp
 * @brief The engines' constructor arguments (networkEngineSpec, card NOM3 step EP1) equal
 *        the setup expressions runNetwork evaluated before they moved.
 *
 * Card NOM3 step EP1 builds the run's engines inside NetworkRun from a NetworkEngineSpec,
 * a pure function of the configuration and the knobs whose body is the setup statements
 * that built the engines, cut from runNetwork.  The statements in frozenSetup() are
 * runNetwork's, copied from ApplicationNetwork.cpp as it stood before the step (180aa28,
 * the base after the REMOVE-key deletion, which took the lane layer, the PB emit filter and
 * the iono-deweight gate, and with them their specification fields), with their comments
 * shortened.  Each field of the specification is compared with the frozen expression on the
 * loader defaults, on both goldens and on the retired invariant-carry golden
 * (golden_field_gps_dual), and every field of each engine configuration is compared, so a
 * field the cut set or dropped by mistake fails here.
 *
 * Each armed posture also checks the values it targets (the census families, the clock
 * re-base stages, the invariant carry and its innovation gate), so a posture the loader
 * quietly disarms cannot pass by reading the default on both sides.
 */

#include <gtest/gtest.h>

#include "app/NetworkRun.hpp"
#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkCarryVce.hpp"
#include "filter/NetworkInvariantCarryDriver.hpp"
#include "logging/LogManager.hpp"
#include "products/NetworkClockEmitRebase.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

/// A minimal two-station GPS network configuration (the shape of
/// test_network_knobs_epoch_driver.cpp).
std::string networkConfig() {
    return std::string("{\n"
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"AAAA\",\n"
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n");
}

/// Write @p body to a temporary file and load it into @p cfg.
bool loadInto(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = fs::temp_directory_path() / ("engine_spec_cfg_" + tag + ".json");
    {
        std::ofstream f(p);
        f << body;
    }
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec;
    fs::remove(p, ec);
    return ok;
}

/// Load a golden configuration (config/golden/@p name) into @p cfg.
bool loadGolden(const char* name, GnssConfig& cfg) {
    LogManager log;
    return cfg.loadFromJson(fs::path(GNSS_PROJECT_ROOT) / "config" / "golden" / name, log);
}

/// The setup locals runNetwork built the engines from, before card NOM3 step EP1, and the
/// constructor arguments it computed inline.
struct FrozenSetup {
    std::vector<std::string>            vce_families;
    double                              vce_window_epochs{0};
    double                              vce_seed_floor{0};
    double                              vce_seed_rate{0};
    std::vector<std::string>            prires_families;
    double                              prires_window_epochs{0};
    double                              prires_min_rows{0};
    double                              prires_report_sigma{0};
    bool                                prires_corrob{false};
    bool                                prires_s0g{false};
    NetworkInvariantCarryDriver::Config carry_cfg;
    NetworkClockEmitRebase::Config      clk_rebase_cfg;
};

/// runNetwork's setup statements as they stood at 180aa28 (ApplicationNetwork.cpp 399-419,
/// 441-452, 617-654 and 669-674), one assignment per local or argument.
FrozenSetup frozenSetup(const GnssConfig& config_, const NetworkConfig& nc,
                        const NetworkRunContext& ctx) {
    FrozenSetup f;
    const double pl_q_amb      = ctx.knobs.pl_q_amb;
    const double pl_var_floor  = ctx.knobs.pl_var_floor;
    const bool   carry_vce     = ctx.knobs.carry_vce;
    const std::vector<std::string> vce_families =
        carry_vce ? NetworkCarryVce::parseComponents(config_.networkCarryVceComponents())
                  : std::vector<std::string>{};
    // NetworkCarryVce carry_vce_est(vce_families, <window>, true, pl_var_floor, pl_q_amb);
    f.vce_families      = vce_families;
    f.vce_window_epochs = static_cast<double>(config_.networkCarryVceWindowEpochs());
    f.vce_seed_floor    = pl_var_floor;
    f.vce_seed_rate     = pl_q_amb;
    // NetworkPriorResidual prires_est(...): its six arguments.
    f.prires_families = ctx.knobs.prires_on
            ? NetworkCarryVce::parseComponents(nc.prior_residual_families)
            : std::vector<std::string>{};
    f.prires_window_epochs = static_cast<double>(nc.prior_residual_window_epochs);
    f.prires_min_rows      = static_cast<double>(nc.prior_residual_min_rows);
    f.prires_report_sigma  = ctx.knobs.prires_report_sigma;
    f.prires_corrob        = ctx.knobs.prires_corrob;
    f.prires_s0g           = ctx.knobs.prires_s0g;

    const bool carry_invariant         = ctx.knobs.carry_invariant;
    NetworkInvariantCarryDriver::Config carry_cfg;
    carry_cfg.qz = config_.networkCarryInvariantQz();
    const bool carry_innov_gate = ctx.knobs.carry_innov_gate;
    carry_cfg.innov_gate = carry_innov_gate;
    carry_cfg.innov_k    = config_.networkCarryInvariantInnovK();
    carry_cfg.innov_calibrate        = carry_innov_gate && config_.networkCarryInvariantInnovCalibrate();
    carry_cfg.innov_corroborate      = carry_innov_gate && config_.networkCarryInvariantInnovCorroborate();
    carry_cfg.innov_corroborate_backstop_k = config_.networkCarryInvariantInnovCorroborateBackstopK();
    carry_cfg.innov_carried_only     = carry_innov_gate && config_.networkCarryInvariantInnovCarriedOnly();
    carry_cfg.innov_calibrate_window = config_.networkCarryInvariantInnovCalibrateWindow();
    carry_cfg.pd_safe = carry_invariant && config_.networkCarryInvariantPdSafe();
    carry_cfg.carry_lin    = carry_invariant
                             && config_.getNetworkConfig().state_snapshot_carry_lin;
    f.carry_cfg = carry_cfg;

    NetworkClockEmitRebase::Config clk_rebase_cfg;
    clk_rebase_cfg.frame_hold = ctx.knobs.clk_frame_hold;
    clk_rebase_cfg.leash_m    = config_.networkClockFrameLeashM();
    clk_rebase_cfg.jump_test  = ctx.knobs.clk_jump_test;
    clk_rebase_cfg.jump_m     = config_.networkClockDiscJumpM();
    clk_rebase_cfg.gap_jump_m = config_.networkClockDiscGapJumpM();
    f.clk_rebase_cfg = clk_rebase_cfg;
    return f;
}

/// Every field of the DD-carry driver's configuration (twelve at 180aa28).
void expectCarryCfgEq(const NetworkInvariantCarryDriver::Config& a,
                      const NetworkInvariantCarryDriver::Config& b, const std::string& tag) {
    EXPECT_EQ(a.qz, b.qz) << tag;
    EXPECT_EQ(a.fresh_prior_var, b.fresh_prior_var) << tag;
    EXPECT_EQ(a.innov_gate, b.innov_gate) << tag;
    EXPECT_EQ(a.innov_k, b.innov_k) << tag;
    EXPECT_EQ(a.innov_calibrate, b.innov_calibrate) << tag;
    EXPECT_EQ(a.innov_calibrate_window, b.innov_calibrate_window) << tag;
    EXPECT_EQ(a.innov_corroborate, b.innov_corroborate) << tag;
    EXPECT_EQ(a.innov_corroborate_backstop_k, b.innov_corroborate_backstop_k) << tag;
    EXPECT_EQ(a.innov_carried_only, b.innov_carried_only) << tag;
    EXPECT_EQ(a.pd_safe, b.pd_safe) << tag;
    EXPECT_EQ(a.carry_lin, b.carry_lin) << tag;
    EXPECT_EQ(a.innov_diagnostic_only, b.innov_diagnostic_only) << tag;
}

/// Every field of the clock emission re-base's configuration (seven at 180aa28).
void expectClkRebaseCfgEq(const NetworkClockEmitRebase::Config& a,
                          const NetworkClockEmitRebase::Config& b, const std::string& tag) {
    EXPECT_EQ(a.frame_hold, b.frame_hold) << tag;
    EXPECT_EQ(a.leash_m, b.leash_m) << tag;
    EXPECT_EQ(a.jump_test, b.jump_test) << tag;
    EXPECT_EQ(a.jump_m, b.jump_m) << tag;
    EXPECT_EQ(a.max_gap_s, b.max_gap_s) << tag;
    EXPECT_EQ(a.gap_jump_m, b.gap_jump_m) << tag;
    EXPECT_EQ(a.min_common, b.min_common) << tag;
}

/// Every field of networkEngineSpec equals its frozen expression on @p cfg; returns the
/// specification for the posture's own checks.
NetworkEngineSpec expectFrozenSpec(const GnssConfig& cfg, const std::string& tag) {
    const NetworkConfig& nc = cfg.getNetworkConfig();
    const auto ctx = std::make_unique<NetworkRunContext>(cfg, nc);
    const NetworkEngineSpec s = networkEngineSpec(cfg, nc, *ctx);
    const FrozenSetup f = frozenSetup(cfg, nc, *ctx);
    EXPECT_EQ(s.vce_families, f.vce_families) << tag;
    EXPECT_EQ(s.vce_window_epochs, f.vce_window_epochs) << tag;
    EXPECT_EQ(s.vce_seed_floor, f.vce_seed_floor) << tag;
    EXPECT_EQ(s.vce_seed_rate, f.vce_seed_rate) << tag;
    EXPECT_EQ(s.prires_families, f.prires_families) << tag;
    EXPECT_EQ(s.prires_window_epochs, f.prires_window_epochs) << tag;
    EXPECT_EQ(s.prires_min_rows, f.prires_min_rows) << tag;
    EXPECT_EQ(s.prires_report_sigma, f.prires_report_sigma) << tag;
    EXPECT_EQ(s.prires_corrob, f.prires_corrob) << tag;
    EXPECT_EQ(s.prires_s0g, f.prires_s0g) << tag;
    expectCarryCfgEq(s.carry_cfg, f.carry_cfg, tag + " carry_cfg");
    expectClkRebaseCfgEq(s.clk_rebase_cfg, f.clk_rebase_cfg, tag + " clk_rebase_cfg");
    return s;
}

}  // namespace

TEST(NetworkEngineSpec, DefaultsMatchTheFrozenSetup) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("default", networkConfig(), cfg));
    const NetworkEngineSpec s = expectFrozenSpec(cfg, "defaults");
    EXPECT_TRUE(s.vce_families.empty());
    EXPECT_TRUE(s.prires_families.empty());
    EXPECT_FALSE(s.carry_cfg.innov_gate);
    EXPECT_FALSE(s.carry_cfg.pd_safe);
    EXPECT_FALSE(s.clk_rebase_cfg.frame_hold);
    EXPECT_FALSE(s.clk_rebase_cfg.jump_test);
}

TEST(NetworkEngineSpec, BothGoldens) {
    for (const char* name : {"golden_field_refound.json", "golden_sim_refound.json"}) {
        GnssConfig cfg;
        ASSERT_TRUE(loadGolden(name, cfg)) << name;
        const NetworkEngineSpec s = expectFrozenSpec(cfg, name);
        // Both goldens arm the prior-residual census with its five families, the carry-row
        // variance monitor and both stages of the clock emission re-base.
        EXPECT_EQ(s.prires_families.size(), 5u) << name;
        EXPECT_FALSE(s.vce_families.empty()) << name;
        EXPECT_TRUE(s.clk_rebase_cfg.frame_hold) << name;
        EXPECT_TRUE(s.clk_rebase_cfg.jump_test) << name;
        // The float vehicle: no invariant carry, so its innovation gate stays off.
        EXPECT_FALSE(s.carry_cfg.innov_gate) << name;
    }
}

TEST(NetworkEngineSpec, RetiredInvariantCarryGolden) {
    GnssConfig cfg;
    ASSERT_TRUE(loadGolden("golden_field_gps_dual.json", cfg));
    const auto ctx = std::make_unique<NetworkRunContext>(cfg, cfg.getNetworkConfig());
    ASSERT_TRUE(ctx->knobs.carry_invariant);
    ASSERT_TRUE(ctx->knobs.carry_innov_gate);
    const NetworkEngineSpec s = expectFrozenSpec(cfg, "golden_field_gps_dual");
    // The invariant carry's gated and carry_invariant-gated fields are taken.
    EXPECT_TRUE(s.carry_cfg.innov_gate);
    EXPECT_TRUE(s.carry_cfg.innov_calibrate);
    EXPECT_TRUE(s.carry_cfg.innov_corroborate);
    EXPECT_TRUE(s.carry_cfg.pd_safe);
    EXPECT_TRUE(s.carry_cfg.carry_lin);
    EXPECT_FALSE(s.vce_families.empty());
}
