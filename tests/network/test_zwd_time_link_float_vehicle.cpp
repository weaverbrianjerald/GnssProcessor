/**
 * @file test_zwd_time_link_float_vehicle.cpp
 * @brief Contract tests for the station wet delay's time link on the float ambiguity vehicle:
 *        the in-block wet-delay carry made reachable under @c network.ambiguity_vehicle
 *        @c float_kf, and its event response (standing invariants 1 and 7).
 *
 * The float vehicle forces the F3 knob (@c network.phase_leveling_local_block) off and supplies
 * the station block itself, so an interlock that tested the F3 member disabled
 * @c network.carry_zwd_in_block on every float-vehicle posture.  The cases here pin:
 *   - the loader accepts the pair (@c carry_zwd + @c carry_zwd_in_block) under the vehicle,
 *     refuses @c carry_zwd alone there, and leaves the level vehicle's F3 rule unchanged;
 *   - the knob snapshot resolves the in-block route and the event response from the vehicle;
 *   - the block gains a wet-delay column with the diagonal the driver folds (stashed variance
 *     plus q times the gap) and the harvested cross terms;
 *   - the event response's two helpers: the slip-latch station set (re-entry-only latches are
 *     not a feed) and the erase of one station's carried entry;
 *   - the goldens: both arm the route, its q and the response, and declare both response
 *     sites (armed 2026-11-09, card ZTL section 8); with the keys removed the route and the
 *     response are disarmed and the sites are not declared.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "config/GnssConfigTypes.hpp"
#include "logging/LogManager.hpp"
#include "network/assembly/Responses.hpp"
#include "network/carry/Coupling.hpp"
#include "network/carry/ZwdCarry.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/solve/Estimator.hpp"
#include "network/state/Entities.hpp"
#include "network/state/RunContext.hpp"

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "../support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

// --- loader fixture ----------------------------------------------------------

/// A minimal loadable network configuration; @p extra is spliced into the network block.
std::string networkConfig(const std::string& extra) {
    return std::string("{\n")
        + "  \"processing\": {\n"
          "    \"mode\": \"ppp-rtk-network\",\n"
          "    \"active_systems\": [ \"GPS\" ],\n"
          "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
          "  },\n"
          "  \"network\": {\n"
          "    \"clock_datum_station\": \"AAAA\",\n"
        + extra +
          "    \"stations\": [\n"
          "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
          "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
          "    ]\n"
          "  }\n"
          "}\n";
}

/// The float vehicle on the leveling carry folded as conditional coupling rows (the vehicle's
/// own prerequisites); F3 is absent and therefore off.
const char* kFloatVehicle =
    "    \"phase_leveling\": true,\n"
    "    \"carry_invariant\": false,\n"
    "    \"carry_conditional_coupling\": true,\n"
    "    \"shared_carry_full_cov\": true,\n"
    "    \"ambiguity_vehicle\": \"float_kf\",\n";

/// The level vehicle with the same carry and no F3.
const char* kLevelVehicle =
    "    \"phase_leveling\": true,\n"
    "    \"carry_invariant\": false,\n"
    "    \"carry_conditional_coupling\": true,\n"
    "    \"shared_carry_full_cov\": true,\n";

bool loadInto(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = gnss_test::tempDir() / ("ztl_cfg_" + tag + ".json");
    {
        std::ofstream f(p);
        f << body;
    }
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec;
    fs::remove(p, ec);
    return ok;
}

bool loadGolden(const std::string& bed, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = fs::path(GNSS_PROJECT_ROOT) / "config" / "golden" /
                       ("golden_" + bed + "_refound.json");
    return cfg.loadFromJson(p, log);
}

/// The golden of @p bed with the wet-delay link and the phase colour state removed (the
/// posture before 2026-11-09), loaded into @p cfg.
bool loadGoldenPairOff(const std::string& bed, GnssConfig& cfg) {
    std::ifstream in(fs::path(GNSS_PROJECT_ROOT) / "config" / "golden" /
                     ("golden_" + bed + "_refound.json"));
    nlohmann::json j = nlohmann::json::parse(in, nullptr, true, /*ignore_comments=*/true);
    for (const char* key : {"carry_zwd", "carry_zwd_in_block", "zwd_carry_q", "phase_colour_state"})
        j["network"].erase(key);
    return loadInto("pair_off_" + bed, j.dump(2), cfg);
}

// --- seam fixture (a registered estimator, as in test_carry_zwd_in_block.cpp) ---------

struct BlockFixture {
    NetworkEstimator est;
    GlobalVtecShConfig shc{};
    net::SatSignalTable tu_reinit_hist;
    StationAssembly R;
    Eigen::Index clk_col{-1};

    BlockFixture() {
        clk_col = est.registerClock("G01");
        est.registerPhaseBias("G01", "L1C");
        est.beginEpoch();
        R.A_s = Eigen::MatrixXd::Zero(0, est.numShared());
        R.A_r = Eigen::MatrixXd::Zero(0, 8);
        R.w.resize(0);
        R.y.resize(0);
    }

    CouplingRowBuilder make() {
        return CouplingRowBuilder{est, shc, /*q_clock=*/1e-4, /*q_cb=*/1e-4, /*q_pb=*/1e-4,
                                  /*tu=*/false, /*gauge=*/false, /*reinit=*/false,
                                  tu_reinit_hist, R};
    }
};

LocalCarry carryAt(double value_abs, double var, const std::shared_ptr<PlHarvestBlock>& blk,
                   int idx) {
    LocalCarry lc;
    lc.value_abs   = value_abs;
    lc.var         = var;
    lc.last_seen_s = 0.0;
    lc.coupling    = { CoupledShared{SharedParamKind::Clock, "G01", "", 0, 0, 'C',
                                     /*sh_layer=*/0, /*coeff=*/0.8, /*s_prev=*/1.5,
                                     /*p_prev=*/0.2} };
    lc.blk     = blk;
    lc.blk_idx = idx;
    return lc;
}

}  // namespace

// =============================================================================
// (1) The loader: the vehicle is a station block for the wet delay
// =============================================================================

// The pair loads under the float vehicle and the sub-knob survives the interlock (it was
// disabled before, because the interlock tested the F3 member the vehicle forces off).
TEST(ZwdTimeLinkFloatVehicleTest, PairLoadsUnderTheFloatVehicle) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("pair", networkConfig(std::string(kFloatVehicle)
                                               + "    \"carry_zwd\": true,\n"
                                                 "    \"carry_zwd_in_block\": true,\n"
                                                 "    \"zwd_carry_q\": 5e-8,\n"),
                         cfg));
    EXPECT_TRUE(cfg.networkCarryZwd());
    EXPECT_TRUE(cfg.networkCarryZwdInBlock());
    EXPECT_FALSE(cfg.networkPhaseLevelingLocalBlock());   // the vehicle, not F3, is the block
    EXPECT_DOUBLE_EQ(cfg.networkZwdCarryQ(), 5e-8);
}

// The diagonal carry alone is the falsified double count inside a station whose ambiguities
// fold as one block: refused, never silently degraded.
TEST(ZwdTimeLinkFloatVehicleTest, DiagonalCarryAloneIsRefusedUnderTheFloatVehicle) {
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("diag", networkConfig(std::string(kFloatVehicle)
                                                + "    \"carry_zwd\": true,\n"),
                          cfg));
}

// The level vehicle keeps its rule: without F3 the sub-knob is disabled and the diagonal carry
// itself still loads (the refusal is the float vehicle's only).
TEST(ZwdTimeLinkFloatVehicleTest, LevelVehicleWithoutF3KeepsItsRule) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("level", networkConfig(std::string(kLevelVehicle)
                                                + "    \"carry_zwd\": true,\n"
                                                  "    \"carry_zwd_in_block\": true,\n"),
                         cfg));
    EXPECT_TRUE(cfg.networkCarryZwd());
    EXPECT_FALSE(cfg.networkCarryZwdInBlock());
}

// The slant-ionosphere and receiver-clock in-block interlocks are vehicle-aware too.
TEST(ZwdTimeLinkFloatVehicleTest, IonoAndClockInBlockSurviveTheInterlockUnderTheVehicle) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("ionoclk", networkConfig(std::string(kFloatVehicle)
                                                  + "    \"carry_iono_in_block\": true,\n"
                                                    "    \"carry_rx_clock_in_block\": true,\n"),
                         cfg));
    EXPECT_TRUE(cfg.networkCarryIonoInBlock());
    EXPECT_TRUE(cfg.networkCarryRxClockInBlock());
}

// =============================================================================
// (2) The knob snapshot resolves the route and the response from the vehicle
// =============================================================================

TEST(ZwdTimeLinkFloatVehicleTest, SnapshotArmsTheInBlockRouteAndTheResponse) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("snap", networkConfig(std::string(kFloatVehicle)
                                               + "    \"carry_zwd\": true,\n"
                                                 "    \"carry_zwd_in_block\": true,\n"
                                                 "    \"zwd_carry_q\": 2e-7,\n"),
                         cfg));
    const NetworkKnobs k = NetworkKnobs::snapshot(cfg, cfg.getNetworkConfig());
    EXPECT_TRUE(k.amb_float_kf);
    EXPECT_TRUE(k.pl_local_block);     // the vehicle supplies the block
    EXPECT_TRUE(k.zwd_on);
    EXPECT_TRUE(k.zwd_in_block);
    EXPECT_TRUE(k.zwd_blk_on);
    EXPECT_TRUE(k.zwd_event_on);
    EXPECT_DOUBLE_EQ(k.zwdQEff(false), 2e-7);   // the rate every fold site reads
}

// =============================================================================
// (3) The block gains the wet-delay column at the driver's variance, with cross terms
// =============================================================================
//
// The driver (networkStationLocalBlock) pushes the carried wet delay with
// var_eff = stashed variance + zwdQEff * gap and vce_fac 1; the block's Σ must hold exactly
// that diagonal and the harvested (N_rr^-1) cross terms against the ambiguities.
TEST(ZwdTimeLinkFloatVehicleTest, BlockGainsTheWetDelayColumnAtTheDriversVariance) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("blk", networkConfig(std::string(kFloatVehicle)
                                              + "    \"carry_zwd\": true,\n"
                                                "    \"carry_zwd_in_block\": true,\n"
                                                "    \"zwd_carry_q\": 5e-8,\n"),
                         cfg));
    const NetworkKnobs k = NetworkKnobs::snapshot(cfg, cfg.getNetworkConfig());

    BlockFixture f;
    const double cross = 0.0004;
    auto blk = std::make_shared<PlHarvestBlock>();
    blk->C = Eigen::MatrixXd::Constant(3, 3, cross);
    blk->C(0, 0) = 0.040;
    blk->C(1, 1) = 0.030;
    blk->C(2, 2) = 4.0e-5;   // the wet delay's conditional variance, (6.3 mm)^2
    blk->stamp_s = 1000.0;
    LocalCarry a0 = carryAt(2.00, 0.040, blk, 0);
    LocalCarry a1 = carryAt(2.25, 0.030, blk, 1);
    LocalCarry zw = carryAt(0.16, 4.0e-5, blk, 2);

    const double gap = 30.0;
    const double zvar_eff = zw.var + k.zwdQEff(false) * gap;   // the driver's expression
    std::vector<LocalBlockCol> bc = {
        LocalBlockCol{0, 0.5, &a0, 0.040, gap, 1.0},
        LocalBlockCol{1, -0.25, &a1, 0.030, gap, 1.0},
        LocalBlockCol{2, 0.15, &zw, zvar_eff, gap, /*vce_fac=*/1.0, /*is_zwd=*/true},
    };
    auto cb = f.make();
    ASSERT_TRUE(cb.buildLocalBlock(bc));
    const Eigen::MatrixXd& S = f.R.pl_blk.Sigma;
    ASSERT_EQ(S.rows(), 3);
    EXPECT_DOUBLE_EQ(S(2, 2), 4.0e-5 + 5e-8 * 30.0);
    EXPECT_DOUBLE_EQ(S(2, 0), cross);
    EXPECT_DOUBLE_EQ(S(0, 2), cross);
    EXPECT_DOUBLE_EQ(S(2, 1), cross);
    EXPECT_EQ(f.R.pl_blk_zwd_cols, 1);
    EXPECT_EQ(f.R.pl_blk_zwd_missing_cross, 0);
    ASSERT_EQ(cb.crows.size(), 3u);
    EXPECT_EQ(cb.crows[2].local_col, 2);   // the wet delay's whitened row
}

// =============================================================================
// (4) The event response
// =============================================================================

// A station with a slip-caused latch is a feed; a station latched only by a re-entry response
// is not; a latch with no recorded cause counts as a slip (the latch's own split).
TEST(ZwdTimeLinkFloatVehicleTest, SlipLatchStationsExcludeReentryOnlyLatches) {
    RelevelEpochState rl;
    rl.free_ambs.insert({1, "G05_L1C"});
    rl.free_cause[{1, "G05_L1C"}] = RelevelEpochState::kCauseMw;
    rl.free_ambs.insert({2, "G07_L2W"});
    rl.free_cause[{2, "G07_L2W"}] = RelevelEpochState::kCauseGap;      // re-entry only
    rl.free_ambs.insert({3, "G09_L1C"});
    rl.free_cause[{3, "G09_L1C"}] = RelevelEpochState::kCauseWindup
                                    | RelevelEpochState::kCauseLli;  // a slip with it
    rl.free_ambs.insert({4, "E11_L1C"});                               // no cause recorded
    rl.free_ambs.insert({2, "G08_L2W"});
    rl.free_cause[{2, "G08_L2W"}] = RelevelEpochState::kCauseWindup;   // re-entry only
    EXPECT_EQ(zwdSlipLatchStations(rl), (std::set<int>{1, 3, 4}));
    EXPECT_TRUE(zwdSlipLatchStations(RelevelEpochState{}).empty());
}

// The erase acts on the flagged station's entry only, once: the second call finds nothing.
TEST(ZwdTimeLinkFloatVehicleTest, EventDropErasesTheFlaggedStationOnly) {
    net::StationTable rows;
    net::put(rows, StationId{0}, &net::StationState::zwd) = LocalCarry{0.12, 4e-5, 100.0, {}};
    net::put(rows, StationId{1}, &net::StationState::zwd) = LocalCarry{0.20, 5e-5, 100.0, {}};
    EXPECT_TRUE(zwdEventDrop(rows, 1));
    EXPECT_EQ(net::get(rows, StationId{1}, &net::StationState::zwd), nullptr);
    ASSERT_NE(net::get(rows, StationId{0}, &net::StationState::zwd), nullptr);
    EXPECT_DOUBLE_EQ(net::get(rows, StationId{0}, &net::StationState::zwd)->value_abs, 0.12);
    EXPECT_FALSE(zwdEventDrop(rows, 1));   // nothing carried, nothing to erase
    EXPECT_FALSE(zwdEventDrop(rows, 7));   // a station with no row
}

// The response sites are declared armed on a posture with the route and the response.
TEST(ZwdTimeLinkFloatVehicleTest, ResponseSitesAreDeclaredWhenArmed) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("sites", networkConfig(std::string(kFloatVehicle)
                                                + "    \"carry_zwd\": true,\n"
                                                  "    \"carry_zwd_in_block\": true,\n"
                                                  "    \"cycle_slip\": { \"mw_detect\": true,"
                                                  " \"relevel_free_epoch\": true },\n"),
                         cfg));
    const NetworkKnobs k = NetworkKnobs::snapshot(cfg, cfg.getNetworkConfig());
    ASSERT_TRUE(k.zwd_event_on);
    EXPECT_EQ(responseSiteArmed(ResponseSite::ZwdSlipErase, k, cfg),
              cfg.networkCycleSlipRelevelFreeEpoch()
                  && (cfg.networkCycleSlipMwDetect() || cfg.networkCycleSlipIfPhaseTd()));
    const ResponseSiteSpec& s = responseSiteSpec(ResponseSite::ZwdRaimErase);
    EXPECT_EQ(s.kind, NetworkParamKind::Zwd);
    EXPECT_EQ(s.policy, ResponsePolicy::EraseAndRebirth);
}

// =============================================================================
// (5) The goldens: both arm the route, its q and the response (2026-11-09); with the keys
//     removed the wet delay is white and neither response site is declared
// =============================================================================

TEST(ZwdTimeLinkFloatVehicleTest, GoldensArmTheRouteAndTheResponse) {
    for (const char* bed : {"field", "sim"}) {
        GnssConfig cfg;
        ASSERT_TRUE(loadGolden(bed, cfg)) << bed;
        const NetworkKnobs k = NetworkKnobs::snapshot(cfg, cfg.getNetworkConfig());
        EXPECT_TRUE(k.amb_float_kf) << bed;
        EXPECT_TRUE(k.zwd_on) << bed;
        EXPECT_TRUE(k.zwd_in_block) << bed;
        EXPECT_TRUE(k.zwd_blk_on) << bed;
        EXPECT_TRUE(k.zwd_event_on) << bed;
        EXPECT_DOUBLE_EQ(k.zwd_q, 5e-8) << bed;
        EXPECT_TRUE(k.pc_on) << bed;   // its pair, the phase colour state
        EXPECT_TRUE(cfg.networkCycleSlipRelevelFreeEpoch()) << bed;
        EXPECT_TRUE(responseSiteArmed(ResponseSite::ZwdSlipErase, k, cfg)) << bed;
        EXPECT_TRUE(responseSiteArmed(ResponseSite::ZwdRaimErase, k, cfg)) << bed;
    }
}

TEST(ZwdTimeLinkFloatVehicleTest, GoldensWithThePairOffKeepTheRouteAndTheResponseOff) {
    for (const char* bed : {"field", "sim"}) {
        GnssConfig cfg;
        ASSERT_TRUE(loadGoldenPairOff(bed, cfg)) << bed;
        const NetworkKnobs k = NetworkKnobs::snapshot(cfg, cfg.getNetworkConfig());
        EXPECT_TRUE(k.amb_float_kf) << bed;
        EXPECT_FALSE(k.zwd_on) << bed;
        EXPECT_FALSE(k.zwd_blk_on) << bed;
        EXPECT_FALSE(k.zwd_event_on) << bed;
        EXPECT_FALSE(k.pc_on) << bed;
        EXPECT_FALSE(responseSiteArmed(ResponseSite::ZwdSlipErase, k, cfg)) << bed;
        EXPECT_FALSE(responseSiteArmed(ResponseSite::ZwdRaimErase, k, cfg)) << bed;
    }
}
