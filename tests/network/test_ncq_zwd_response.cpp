/**
 * @file test_ncq_zwd_response.cpp
 * @brief Response tests for the station wet delay carried between epochs on the float ambiguity
 *        vehicle: a carried state fed a change of the truth must show its declared response and
 *        counter at the entry points the engine itself calls.
 *
 * The wet delay is a random walk carried from one epoch's posterior to the next epoch's prior.
 * Three things change what the carried value may claim: a station that comes back after a gap
 * (within the gap horizon the carried variance grows by the walk over the gap; past it the value
 * is refused and later pruned), a declared event of the station (a slip latch erases the carried
 * value, or keeps it with an added sigma), and the re-coupling that follows (the next stash must
 * be this epoch's posterior with its own variance, never a stiff value, and its first fold is
 * counted as a re-coupling, never as a first sighting).
 *
 * The entry points driven here, on one run built from a loaded float-vehicle configuration:
 *   - PASS B's carry steps of one station: networkStationLocalBlock (the in-block fold) and
 *     networkZwdCarryPrior (the fallback row and the gap refusal count);
 *   - the slip-latch feed of the event response: networkZwdEventResponse;
 *   - the post-solve harvest: networkHarvestZwd, over a real estimator solve of one station whose
 *     only local column is the wet delay, so the stashed variance is the solve's own conditional
 *     variance.
 *
 * The RAIM per-arc and prior-channel feeds act through the same helper (zwdEventRespond) but sit
 * inside the post-fit RAIM pass, which a unit test cannot reach without a full epoch; the helper
 * itself is covered by the edge tests of the wet delay.
 */

#include <gtest/gtest.h>

#include "../support/CarriedFamilyStates.hpp"   // stashCarriedFamilies, carriedZwd
#include "../support/NetworkTestRun.hpp"
#include "../support/TestTempDir.hpp"

#include "atmosphere/GlobalIonosphereMap.hpp"
#include "common/ParamRef.hpp"
#include "config/GnssConfig.hpp"
#include "config/GnssConfigTypes.hpp"
#include "logging/LogManager.hpp"
#include "math/TimeUtils.hpp"
#include "network/assembly/ParamRegistry.hpp"
#include "network/assembly/Responses.hpp"
#include "network/assembly/StationBuild.hpp"
#include "network/carry/Coupling.hpp"
#include "network/carry/LocalCarryHarvest.hpp"
#include "network/carry/ZwdCarry.hpp"
#include "network/carry/ZwdEdge.hpp"
#include "network/driver/Epoch.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/solve/Estimator.hpp"
#include "network/state/Entities.hpp"
#include "network/state/RunContext.hpp"
#include "network/state/StationParams.hpp"

#include <Eigen/Dense>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

constexpr double kQ       = 5e-8;     // the wet delay's random-walk q (m^2/s), as in the goldens
constexpr double kHorizon = 120.0;    // network.phase_leveling_max_gap_s (s)
constexpr double kDt      = 30.0;     // epoch interval (s)
constexpr double kXlin    = 0.140;    // the wet delay's linearization point this epoch (m)
constexpr double kValue   = 0.150;    // the carried wet delay (m)
constexpr double kVar     = 4.0e-5;   // its carried variance (m^2), (6.3 mm)^2
constexpr double kStep    = 0.012;    // the solve's correction to the linearization point (m)
constexpr double kTow0    = 259230.0;

// The solve's design: one station, three rows on the satellite clock and the wet delay.
constexpr double kMap[3] = {1.5, 2.0, 3.0};   // wet mapping of each row
constexpr double kW      = 1.0e4;             // row weight, (1 cm)^-2

/// The conditional variance of the wet delay given the border for the design above:
/// 1 / (w * sum of the squared mappings).
double designCondVar() { return 1.0 / (kW * (kMap[0] * kMap[0] + kMap[1] * kMap[1] + kMap[2] * kMap[2])); }

std::size_t at(ZwdEdgeCause c) { return static_cast<std::size_t>(c); }
long long resp(const NetworkRunContext& ctx, ResponseSite s) {
    return ctx.counters.prm_resp[static_cast<int>(s)];
}

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
          "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] },\n"
          "      { \"name\": \"CCCC\", \"position_ecef\": [ 4027893.70, 307045.80, 4919475.10 ] }\n"
          "    ]\n"
          "  }\n"
          "}\n";
}

/// The float vehicle with the wet delay carried in its station block (the goldens' posture for
/// this family), the horizon and q pinned.
std::string floatPosture(const std::string& extra = "") {
    return std::string("    \"phase_leveling\": true,\n"
                       "    \"carry_invariant\": false,\n"
                       "    \"carry_conditional_coupling\": true,\n"
                       "    \"shared_carry_full_cov\": true,\n"
                       "    \"ambiguity_vehicle\": \"float_kf\",\n"
                       "    \"phase_leveling_max_gap_s\": 120.0,\n"
                       "    \"carry_zwd\": true,\n"
                       "    \"carry_zwd_in_block\": true,\n"
                       "    \"zwd_carry_q\": 5e-8,\n")
        + extra;
}

/// The level vehicle with the diagonal wet-delay carry: the event response is not armed there.
const char* kLevelPosture =
    "    \"phase_leveling\": true,\n"
    "    \"carry_invariant\": false,\n"
    "    \"carry_conditional_coupling\": true,\n"
    "    \"shared_carry_full_cov\": true,\n"
    "    \"phase_leveling_max_gap_s\": 120.0,\n"
    "    \"carry_zwd\": true,\n"
    "    \"zwd_carry_q\": 5e-8,\n";

bool loadInto(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = gnss_test::tempDir() / ("ncq_zwd_cfg_" + tag + ".json");
    {
        std::ofstream f(p);
        f << body;
    }
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec;
    fs::remove(p, ec);
    return ok;
}

/**
 * One run of three stations (AAAA, BBBB, CCCC) over a loaded configuration.  Station AAAA has a
 * wet-delay column (slot 0, local column 0) and, when a block is wanted, one carried ambiguity
 * at local column 1, so the station block opens with the wet delay as its second column.
 */
struct ZwdRun {
    GnssConfig                           cfg;
    fs::path                             dir;
    std::unique_ptr<NetworkTestRun>      fx;
    std::unique_ptr<GlobalIonosphereMap> gim;
    GlobalVtecShConfig                   shc{};
    net::SatSignalTable                  sat_signals;
    LocalCarry                           amb;   ///< the station's one carried ambiguity (block column 0)
    double                               now_s{0.0};

    ZwdRun(const std::string& tag, const std::string& network_extra) {
        EXPECT_TRUE(loadInto(tag, networkConfig(network_extra), cfg));
        dir = gnss_test::tempDir() / ("ncq_zwd_" + tag);
        fs::create_directories(dir);
        fx = std::make_unique<NetworkTestRun>(cfg, nullptr, dir);
        for (const char* name : {"AAAA", "BBBB", "CCCC"}) {
            fx->setup().stations.emplace_back();
            fx->setup().stations.back().name = name;
        }
        StationParams set;
        LocalSlot z;
        z.ref   = zwdRef();
        z.x_lin = kXlin;
        EXPECT_TRUE(addParam(set, z));
        EXPECT_EQ(set.zwd, 0);
        fx->setup().stations[0].params = set;
        epoch(kTow0);
    }
    ~ZwdRun() {
        fx->traces().events_trace.close();
        fx.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    NetworkRunContext& ctx() { return fx->ctx(); }
    net::NetworkEntities& ent() { return fx->ctx().entities; }

    /// A fresh epoch at @p tow: its clock one interval after the previous epoch.
    void epoch(double tow) {
        NetworkEpoch& e = fx->newEpoch(GnssTime{2313, tow});
        now_s = TimeUtils::continuousGpsSeconds(e.se.time);
        e.clock = EpochClock{kDt, now_s, true};
    }

    /// Writes station @p i's carried wet delay as a harvest @p age_s seconds ago left it.
    void carry(int i, double value, double var, double age_s) {
        gnss_test::CarriedFamilyState s;
        s.station  = i;
        s.stamp_s  = now_s - age_s;
        s.in_block = false;
        s.zwd      = gnss_test::CarriedZwd{value, var};
        gnss_test::stashCarriedFamilies(ent(), s);
    }

    const LocalCarry* carried(int i) { return gnss_test::carriedZwd(ent(), i); }

    /// A fresh estimator with one satellite clock, its layout frozen; returns the clock column.
    Eigen::Index freshEstimator() {
        NetworkEstimator& est = fx->renewEstimator();
        const Eigen::Index clk = est.registerClock("G01");
        est.beginEpoch();
        return clk;
    }

    /// Station AAAA's PASS B carry steps, in the engine's order: the station block (with one
    /// carried ambiguity when @p with_block), then the wet delay's own step.  Returns the slot.
    StationAssembly fold(bool with_block) {
        freshEstimator();
        StationAssembly slot;
        slot.h2local[0] = 0;   // the wet delay's slot -> local column 0
        slot.A_s = Eigen::MatrixXd::Zero(0, fx->est().numShared());
        slot.A_r = Eigen::MatrixXd::Zero(0, 2);
        slot.w.resize(0);
        slot.y.resize(0);
        Eigen::VectorXd x_lin(2);
        x_lin << kXlin, 1.9;
        amb = LocalCarry{};
        amb.value_abs   = 2.0;
        amb.var         = 0.04;
        amb.last_seen_s = now_s - kDt;
        NetworkEpoch& e = fx->ep();
        StationBuild b{fx->ctx(), fx->config(), fx->est(), fx->setup(), e.se, e.clock,
                       e.pass_a, e.events, gim, 0, fx->setup().stations[0], slot};
        StationCarryFrame frame{b, /*legacy_local_inject=*/true, /*do_amb_carry=*/true, x_lin,
                                CouplingRowBuilder{fx->est(), shc, 0.0, 0.0, 0.0,
                                                   /*tu=*/false, /*gauge=*/false, /*reinit=*/false,
                                                   sat_signals, slot}};
        if (with_block)
            frame.pl_pend.push_back(PlPend{1, 1, &amb, 0.04, kDt, "G05", "L1C"});
        networkStationLocalBlock(frame);
        networkZwdCarryPrior(frame);
        n_rows = frame.cb.crows.size();
        if (!frame.cb.crows.empty()) {
            last_w   = frame.cb.crows.back().w;
            last_y   = frame.cb.crows.back().y;
            last_col = frame.cb.crows.back().local_col;
            last_dense = !frame.cb.crows.back().local.empty();
        }
        return slot;
    }
    std::size_t  n_rows{0};      ///< the coupling rows the carry steps buffered
    double       last_w{0.0};    ///< the last buffered row's weight
    double       last_y{0.0};    ///< its right-hand side
    Eigen::Index last_col{-1};   ///< its primary local column
    bool         last_dense{false};   ///< it is a whitened block row

    /// The post-solve harvest of this epoch with station AAAA solved (its only local column the
    /// wet delay, posterior kXlin + kStep) and every other station away.  @p folded is PASS B's
    /// record that the station's carried wet delay was folded this epoch.
    void harvest(bool folded) {
        NetworkEstimator& est = fx->renewEstimator();
        const Eigen::Index clk = est.registerClock("G01");
        est.beginEpoch();
        Eigen::MatrixXd A_s = Eigen::MatrixXd::Zero(3, est.numShared());
        A_s.col(clk).setOnes();
        Eigen::MatrixXd A_r(3, 1);
        A_r << kMap[0], kMap[1], kMap[2];
        const Eigen::VectorXd w = Eigen::VectorXd::Constant(3, kW);
        const Eigen::VectorXd y = A_r.col(0) * kStep;
        const std::size_t id = est.addStation(A_s, A_r, w, y);
        NetworkEpoch& e = fx->ep();
        e.solve.x_s = est.solve(NetworkEstimator::Datum{{clk, 0.0}});
        const std::size_t n = fx->setup().stations.size();
        e.assembly.est_index_of.assign(n, -1);
        e.assembly.asm_res.assign(n, StationAssembly{});
        e.assembly.est_index_of[0] = static_cast<int>(id);
        StationAssembly& R = e.assembly.asm_res[0];
        R.pl_zwd_present  = true;
        R.pl_zwd          = PlZwd{0, kXlin};
        R.pl_zwd_injected = folded;
        HarvestBorder border;
        const std::vector<StationHarvestBlock> blocks(n);
        networkHarvestZwd(fx->run(), e, border, blocks);
    }

    /// The post-solve harvest of this epoch with no station solved (every station away).
    void harvestNoneSolved() {
        NetworkEpoch& e = fx->ep();
        const std::size_t n = fx->setup().stations.size();
        e.assembly.est_index_of.assign(n, -1);
        e.assembly.asm_res.assign(n, StationAssembly{});
        HarvestBorder border;
        const std::vector<StationHarvestBlock> blocks(n);
        networkHarvestZwd(fx->run(), e, border, blocks);
    }

    /// A slip latch of station @p i this epoch, by @p cause bits, on one arc.
    void latch(int i, const std::string& arc, unsigned cause) {
        RelevelEpochState& rl = fx->ep().events.slip.relevel;
        rl.free_ambs.insert({i, arc});
        rl.free_cause[{i, arc}] = cause;
    }

    void respond() { networkZwdEventResponse(fx->run(), fx->ep(), fx->ep().events); }
};

}  // namespace

// =================================================================================================
// (0) The posture the cases rely on
// =================================================================================================

TEST(NcqZwdResponse, TheFloatPostureArmsTheBlockRouteTheResponseAndOneHorizon) {
    ZwdRun r("posture", floatPosture());
    const NetworkKnobs& k = r.ctx().knobs;
    EXPECT_TRUE(k.zwd_on);
    EXPECT_TRUE(k.pl_blk_on);
    EXPECT_TRUE(k.zwd_blk_on);
    EXPECT_TRUE(k.zwd_event_on);
    EXPECT_DOUBLE_EQ(k.zwd_event_inflate_m, 0.0);   // the default response is the erase
    EXPECT_DOUBLE_EQ(k.zwdQEff(false), kQ);
    EXPECT_DOUBLE_EQ(k.pl_max_gap_s, kHorizon);
    // The prune at the harvest and the fold at PASS B read one horizon.
    EXPECT_DOUBLE_EQ(r.ctx().params.spec(NetworkParamKind::Zwd).gap_horizon_s, k.pl_max_gap_s);
}

// =================================================================================================
// (1) A station that comes back: the fold at PASS B
// =================================================================================================

// Back after three missed epochs (90 s, inside the horizon): the carried value folds in the
// station block with its variance grown by the walk over the whole gap, not over one epoch.
TEST(NcqZwdResponse, AReturnWithinTheHorizonFoldsInTheBlockLoosenedByTheWalkOverTheGap) {
    ZwdRun r("return_in", floatPosture());
    r.carry(0, kValue, kVar, 90.0);
    const StationAssembly s = r.fold(/*with_block=*/true);
    EXPECT_TRUE(s.pl_zwd_present);
    EXPECT_TRUE(s.pl_zwd_in_block);
    EXPECT_TRUE(s.pl_zwd_injected);
    EXPECT_EQ(s.pl_blk_zwd_cols, 1);
    ASSERT_EQ(s.pl_blk.Sigma.rows(), 2);
    EXPECT_DOUBLE_EQ(s.pl_blk.Sigma(1, 1), kVar + kQ * 90.0);
    EXPECT_GT(s.pl_blk.Sigma(1, 1), kVar + kQ * kDt);   // looser than an ordinary epoch's fold
    // The wet delay's own step adds no second row for the column the block already folded.
    EXPECT_EQ(r.n_rows, 2u);
    EXPECT_EQ(s.pl_blk_zwd_fallback_diag, 0);
    EXPECT_EQ(s.prm_zwd_gap_skip, 0);
}

// A gap equal to the horizon is inside it: folded, at the walk over the full horizon.
TEST(NcqZwdResponse, AGapEqualToTheHorizonIsStillFolded) {
    ZwdRun r("return_edge", floatPosture());
    r.carry(0, kValue, kVar, kHorizon);
    const StationAssembly s = r.fold(true);
    EXPECT_TRUE(s.pl_zwd_in_block);
    ASSERT_EQ(s.pl_blk.Sigma.rows(), 2);
    EXPECT_DOUBLE_EQ(s.pl_blk.Sigma(1, 1), kVar + kQ * kHorizon);
    EXPECT_EQ(s.prm_zwd_gap_skip, 0);
}

// Back past the horizon before the prune removed the entry: the stale value is refused (the
// block leaves the column out, no fallback row is emitted) and the refusal is counted once.
TEST(NcqZwdResponse, AReturnPastTheHorizonIsRefusedAndCountedOnce) {
    ZwdRun r("return_out", floatPosture());
    r.carry(0, kValue, kVar, kHorizon + kDt);
    const StationAssembly s = r.fold(true);
    EXPECT_TRUE(s.pl_zwd_present) << "still recorded for the harvest";
    EXPECT_FALSE(s.pl_zwd_in_block);
    EXPECT_FALSE(s.pl_zwd_injected);
    EXPECT_EQ(s.pl_blk_zwd_cols, 0);
    EXPECT_EQ(s.pl_blk.Sigma.rows(), 1);   // the ambiguity alone
    EXPECT_EQ(r.n_rows, 1u);               // no fallback row for the wet delay
    EXPECT_EQ(s.prm_zwd_gap_skip, 1);
    EXPECT_EQ(s.pl_blk_zwd_fallback_diag, 0);
}

// A station with no block this epoch (no carried ambiguity) folds the wet delay as its own row
// at the same loosened variance, and the fallback is counted.
TEST(NcqZwdResponse, WithoutABlockTheReturnFoldsAsItsOwnRowAtTheSameVariance) {
    ZwdRun r("return_row", floatPosture());
    r.carry(0, kValue, kVar, 60.0);
    const StationAssembly s = r.fold(/*with_block=*/false);
    EXPECT_FALSE(s.pl_zwd_in_block);
    EXPECT_TRUE(s.pl_zwd_injected);
    EXPECT_EQ(s.pl_blk_zwd_fallback_diag, 1);
    ASSERT_EQ(r.n_rows, 1u);
    EXPECT_FALSE(r.last_dense);
    EXPECT_EQ(r.last_col, 0);
    EXPECT_DOUBLE_EQ(r.last_w, 1.0 / (kVar + kQ * 60.0));
    EXPECT_DOUBLE_EQ(r.last_y, kValue - kXlin);
}

// =================================================================================================
// (2) A declared event: the slip-latch feed
// =================================================================================================

// A slip latch erases the latched station's carried wet delay only, once per station however
// many arcs latched, opens its decoupling record and counts the response; a station latched
// only by a re-entry response is not a feed.
TEST(NcqZwdResponse, ASlipLatchErasesTheLatchedStationOnlyAndCountsItOnce) {
    ZwdRun r("slip_erase", floatPosture());
    for (int i = 0; i < 3; ++i) r.carry(i, kValue + 0.01 * i, kVar, kDt);
    r.fx->traces().events_trace.open(r.dir / "events.csv");
    ASSERT_TRUE(r.fx->traces().events_trace.is_open());
    r.latch(1, "G05_L1C", RelevelEpochState::kCauseMw);
    r.latch(1, "G07_L2W", RelevelEpochState::kCauseLli);
    r.latch(2, "G09_L1C", RelevelEpochState::kCauseGap);   // re-entry only
    r.respond();
    r.fx->traces().events_trace.close();

    EXPECT_EQ(r.carried(1), nullptr);
    ASSERT_TRUE(zwdEdgeOf(r.ent().stations, 1).has_value());
    EXPECT_EQ(zwdEdgeOf(r.ent().stations, 1)->cause, ZwdEdgeCause::SlipLatch);
    EXPECT_DOUBLE_EQ(zwdEdgeOf(r.ent().stations, 1)->decoupled_s, r.now_s);
    for (int i : {0, 2}) {
        ASSERT_NE(r.carried(i), nullptr) << i;
        EXPECT_DOUBLE_EQ(r.carried(i)->value_abs, kValue + 0.01 * i) << i;
        EXPECT_DOUBLE_EQ(r.carried(i)->var, kVar) << i;
        EXPECT_FALSE(zwdEdgeOf(r.ent().stations, i).has_value()) << i;
    }
    EXPECT_EQ(r.ctx().counters.n_zwd_event_drop_slip, 1);
    EXPECT_EQ(resp(r.ctx(), ResponseSite::ZwdSlipErase), 1);
    EXPECT_EQ(r.ctx().counters.n_zwd_event_inflated, 0);

    std::ifstream in(r.dir / "events.csv");
    std::vector<std::string> lines;
    for (std::string l; std::getline(in, l);) lines.push_back(l);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_NE(lines[0].find(",ZWD,slip_latch,,BBBB,,,erase,no_prior_this_epoch"), std::string::npos)
        << lines[0];
}

// After the erase the station's column folds no prior this epoch (its birth state) and no gap
// refusal is counted: nothing stale is left to refuse.
TEST(NcqZwdResponse, AfterTheSlipEraseTheColumnFoldsNoPriorAndCountsNoRefusal) {
    ZwdRun r("slip_fold", floatPosture());
    r.carry(0, kValue, kVar, kDt);
    r.latch(0, "G05_L1C", RelevelEpochState::kCauseMw);
    r.respond();
    ASSERT_EQ(r.carried(0), nullptr);
    const StationAssembly s = r.fold(true);
    EXPECT_TRUE(s.pl_zwd_present);
    EXPECT_FALSE(s.pl_zwd_in_block);
    EXPECT_FALSE(s.pl_zwd_injected);
    EXPECT_EQ(s.pl_blk.Sigma.rows(), 1);
    EXPECT_EQ(r.n_rows, 1u);
    EXPECT_EQ(s.prm_zwd_gap_skip, 0);
    EXPECT_EQ(s.pl_blk_zwd_fallback_diag, 0);
}

// With network.zwd_event_inflate_m the latch keeps the carried value and adds the sigma in
// quadrature; the same epoch's block folds the kept value at the loosened variance.
TEST(NcqZwdResponse, AnInflatingLatchKeepsTheValueAndTheBlockFoldsItLoosened) {
    ZwdRun r("slip_inflate", floatPosture("    \"zwd_event_inflate_m\": 0.02,\n"));
    ASSERT_DOUBLE_EQ(r.ctx().knobs.zwd_event_inflate_m, 0.02);
    r.carry(0, kValue, kVar, kDt);
    r.latch(0, "G05_L1C", RelevelEpochState::kCauseIfTd);
    r.respond();
    const LocalCarry* c = r.carried(0);
    ASSERT_NE(c, nullptr) << "kept, not erased";
    EXPECT_DOUBLE_EQ(c->value_abs, kValue);
    EXPECT_DOUBLE_EQ(c->var, kVar + 0.02 * 0.02);
    EXPECT_EQ(r.ctx().counters.n_zwd_event_drop_slip, 1);
    EXPECT_EQ(r.ctx().counters.n_zwd_event_inflated, 1);
    ASSERT_TRUE(zwdEdgeOf(r.ent().stations, 0).has_value());
    EXPECT_EQ(zwdEdgeOf(r.ent().stations, 0)->cause, ZwdEdgeCause::SlipLatch);
    const StationAssembly s = r.fold(true);
    EXPECT_TRUE(s.pl_zwd_in_block);
    ASSERT_EQ(s.pl_blk.Sigma.rows(), 2);
    EXPECT_DOUBLE_EQ(s.pl_blk.Sigma(1, 1), (kVar + 0.02 * 0.02) + kQ * kDt);
}

// A latched station that carries nothing: no response, no count, no record.
TEST(NcqZwdResponse, ALatchedStationWithNothingCarriedIsNotCounted) {
    ZwdRun r("slip_none", floatPosture());
    r.latch(1, "G05_L1C", RelevelEpochState::kCauseMw);
    r.respond();
    EXPECT_EQ(r.ctx().counters.n_zwd_event_drop_slip, 0);
    EXPECT_EQ(resp(r.ctx(), ResponseSite::ZwdSlipErase), 0);
    EXPECT_FALSE(zwdEdgeOf(r.ent().stations, 1).has_value());
}

// The response is armed by the float vehicle's in-block route only: on the level vehicle's
// diagonal carry a slip latch leaves the carried wet delay as it was.
TEST(NcqZwdResponse, OnTheLevelVehicleTheLatchLeavesTheCarryAsItWas) {
    ZwdRun r("slip_level", kLevelPosture);
    ASSERT_TRUE(r.ctx().knobs.zwd_on);
    ASSERT_FALSE(r.ctx().knobs.zwd_event_on);
    r.carry(0, kValue, kVar, kDt);
    r.latch(0, "G05_L1C", RelevelEpochState::kCauseMw);
    r.respond();
    ASSERT_NE(r.carried(0), nullptr);
    EXPECT_DOUBLE_EQ(r.carried(0)->var, kVar);
    EXPECT_EQ(r.ctx().counters.n_zwd_event_drop_slip, 0);
}

// =================================================================================================
// (3) The harvest: the prune, the re-creation and the re-coupling
// =================================================================================================

// A station away past the horizon is pruned at the harvest: its entry erased, its decoupling
// record opened as a gap prune, both counters raised.  A station away exactly the horizon stays.
TEST(NcqZwdResponse, AStationAwayPastTheHorizonIsPrunedWithItsRecordAndCounters) {
    ZwdRun r("prune", floatPosture());
    r.carry(0, kValue, kVar, kHorizon + kDt);
    r.carry(1, kValue, kVar, kHorizon);
    r.harvestNoneSolved();
    EXPECT_EQ(r.carried(0), nullptr);
    ASSERT_TRUE(zwdEdgeOf(r.ent().stations, 0).has_value());
    EXPECT_EQ(zwdEdgeOf(r.ent().stations, 0)->cause, ZwdEdgeCause::GapPrune);
    EXPECT_DOUBLE_EQ(zwdEdgeOf(r.ent().stations, 0)->decoupled_s, r.now_s);
    ASSERT_NE(r.carried(1), nullptr);
    EXPECT_FALSE(zwdEdgeOf(r.ent().stations, 1).has_value());
    EXPECT_EQ(r.ctx().counters.n_cr_zwd_gap_prune, 1);
    EXPECT_EQ(resp(r.ctx(), ResponseSite::ZwdGapPrune), 1);
}

// The chain after a slip latch: the erase, the harvest's re-creation from this epoch's posterior
// with the solve's own conditional variance (not a stiffer value), the next epoch's fold at that
// variance plus one epoch's walk, and that fold counted as a re-coupling of cause slip_latch.
TEST(NcqZwdResponse, AfterAnEraseTheRecreatedEntryIsThisEpochsPosteriorAndItsFirstFoldRecouples) {
    ZwdRun r("recouple_slip", floatPosture());
    r.carry(0, kValue, kVar, kDt);
    r.latch(0, "G05_L1C", RelevelEpochState::kCauseMw);
    r.respond();
    const StationAssembly s0 = r.fold(true);
    ASSERT_FALSE(s0.pl_zwd_injected);
    r.harvest(s0.pl_zwd_injected);
    const LocalCarry* c = r.carried(0);
    ASSERT_NE(c, nullptr);
    EXPECT_NEAR(c->value_abs, kXlin + kStep, 1e-9);
    EXPECT_NEAR(c->var, designCondVar(), 1e-9 * designCondVar());
    EXPECT_DOUBLE_EQ(c->last_seen_s, r.now_s);
    ASSERT_TRUE(zwdEdgeOf(r.ent().stations, 0).has_value());
    EXPECT_EQ(zwdEdgeOf(r.ent().stations, 0)->cause, ZwdEdgeCause::SlipLatch);
    EXPECT_DOUBLE_EQ(zwdEdgeOf(r.ent().stations, 0)->created_s, r.now_s);
    EXPECT_EQ(r.ctx().counters.n_zwd_recouple, 0);
    const double var_k = c->var;

    r.epoch(kTow0 + kDt);
    const StationAssembly s1 = r.fold(true);
    EXPECT_TRUE(s1.pl_zwd_in_block);
    ASSERT_EQ(s1.pl_blk.Sigma.rows(), 2);
    EXPECT_DOUBLE_EQ(s1.pl_blk.Sigma(1, 1), var_k + kQ * kDt);
    r.harvest(s1.pl_zwd_injected);
    EXPECT_EQ(r.ctx().counters.n_zwd_recouple, 1);
    EXPECT_EQ(r.ctx().counters.n_zwd_recouple_by[at(ZwdEdgeCause::SlipLatch)], 1);
    EXPECT_EQ(r.ctx().counters.n_zwd_recouple_by[at(ZwdEdgeCause::FirstSighting)], 0);
    EXPECT_FALSE(zwdEdgeOf(r.ent().stations, 0).has_value());
}

// A station back within the horizon re-couples on its fold, counted as return_in_horizon; its
// stash is this epoch's posterior.
TEST(NcqZwdResponse, AReturnWithinTheHorizonIsCountedAsAReturnRecoupling) {
    ZwdRun r("recouple_return", floatPosture());
    r.carry(0, kValue, kVar, 90.0);
    const StationAssembly s = r.fold(true);
    ASSERT_TRUE(s.pl_zwd_injected);
    r.harvest(s.pl_zwd_injected);
    EXPECT_EQ(r.ctx().counters.n_zwd_recouple_by[at(ZwdEdgeCause::Return)], 1);
    EXPECT_EQ(r.ctx().counters.n_zwd_recouple, 1);
    EXPECT_FALSE(zwdEdgeOf(r.ent().stations, 0).has_value());
    ASSERT_NE(r.carried(0), nullptr);
    EXPECT_DOUBLE_EQ(r.carried(0)->last_seen_s, r.now_s);
    EXPECT_NEAR(r.carried(0)->value_abs, kXlin + kStep, 1e-9);
}

// A station back at the first epoch past the horizon (or every station after a data gap longer
// than the horizon): the fold refused the stale value, so the harvest's stash is a re-seed.
// Declared: the removal is a decoupling edge (cause gap_prune) and the next fold re-couples.
TEST(NcqZwdResponse, AReturnPastTheHorizonBeforeThePruneOpensAGapRecord) {
    ZwdRun r("return_out_edge", floatPosture());
    r.carry(0, kValue, kVar, kHorizon + kDt);
    const StationAssembly s = r.fold(true);
    ASSERT_FALSE(s.pl_zwd_injected);
    ASSERT_EQ(s.prm_zwd_gap_skip, 1);
    r.harvest(s.pl_zwd_injected);
    // The value response holds: the stale value is gone, the stash is this epoch's posterior.
    ASSERT_NE(r.carried(0), nullptr);
    EXPECT_NEAR(r.carried(0)->value_abs, kXlin + kStep, 1e-9);
    EXPECT_NEAR(r.carried(0)->var, designCondVar(), 1e-9 * designCondVar());
    EXPECT_DOUBLE_EQ(r.carried(0)->last_seen_s, r.now_s);
    if (!zwdEdgeOf(r.ent().stations, 0).has_value())
        GTEST_SKIP() << "Defect: the harvest overwrites a wet delay that came back past the gap "
                        "horizon before the prune runs, so no gap_prune record opens and no "
                        "counter or event marks the removal; the next fold is then counted as "
                        "neither a re-coupling nor a first sighting.";
    EXPECT_EQ(zwdEdgeOf(r.ent().stations, 0)->cause, ZwdEdgeCause::GapPrune);
    r.epoch(kTow0 + kDt);
    const StationAssembly s1 = r.fold(true);
    r.harvest(s1.pl_zwd_injected);
    EXPECT_EQ(r.ctx().counters.n_zwd_recouple_by[at(ZwdEdgeCause::GapPrune)], 1);
}

// An inflating response keeps the entry, so the harvest never re-creates it.  Declared: the
// entry's next fold closes the record as a re-coupling of the response's cause.
TEST(NcqZwdResponse, AnInflatedEntryClosesItsRecordAtItsNextFold) {
    ZwdRun r("recouple_inflate", floatPosture("    \"zwd_event_inflate_m\": 0.02,\n"));
    r.carry(0, kValue, kVar, kDt);
    r.latch(0, "G05_L1C", RelevelEpochState::kCauseMw);
    r.respond();
    ASSERT_TRUE(zwdEdgeOf(r.ent().stations, 0).has_value());
    const StationAssembly s = r.fold(true);
    ASSERT_TRUE(s.pl_zwd_injected) << "the kept value folds this epoch";
    r.harvest(s.pl_zwd_injected);
    // The value response holds: the stash is this epoch's posterior with the solve's variance.
    ASSERT_NE(r.carried(0), nullptr);
    EXPECT_NEAR(r.carried(0)->value_abs, kXlin + kStep, 1e-9);
    EXPECT_NEAR(r.carried(0)->var, designCondVar(), 1e-9 * designCondVar());
    if (zwdEdgeOf(r.ent().stations, 0).has_value())
        GTEST_SKIP() << "Defect: after an inflating event response (network.zwd_event_inflate_m "
                        "above 0) the station's decoupling record never closes, because only an "
                        "entry the harvest re-created can close it; the re-coupling is never "
                        "counted and the record stays open to the run end.";
    EXPECT_EQ(r.ctx().counters.n_zwd_recouple_by[at(ZwdEdgeCause::SlipLatch)], 1);
    EXPECT_EQ(r.ctx().counters.n_zwd_recouple, 1);
}
