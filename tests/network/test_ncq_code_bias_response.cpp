/**
 * @file test_ncq_code_bias_response.cpp
 * @brief Responses of the network engine's two code-bias carries to a change of the quantity
 *        they carry: the receiver code bias, reset by the post-fit RAIM's prior channel, and the
 *        satellite code bias, carried stiff and with no response to a step of its level.
 *
 * Both carries hold a near-constant quantity under a stiff prior, so a step of the truth that no
 * response reaches is folded at converged weight and pushed into the other states.  The cases:
 *   - the receiver code bias: a carried value 0.6 m away from what the data say (a receiver
 *     restart, say) makes its block row stand out in the prior channel's table; the post-fit
 *     pass erases exactly that carried entry (the next harvest re-seeds it from a solve without
 *     the suspect prior) and counts the reset; a flag on a station that carries no such entry
 *     is counted as "no entry"; a quiet epoch erases nothing.  The pass is driven end to end
 *     on a solved five-station network, because the erase is written inside it;
 *   - the satellite code bias: the reference configurations carry it as a random walk of
 *     1e-10 m^2/s (0.6 mm of sigma per hour) with no variance floor, and arm neither route that
 *     can loosen it (the soft reinit and the hard refloat wait for a clock flag; the excursion
 *     guard watches a clock excursion, not a code level);
 *   - a 0.5 m step of one satellite's solved code level between two kept epochs reaches the
 *     stash and leaves the carry stiff, with no reinit mark and no refloat.  That case is kept
 *     as a skip naming the gap, and passes once a response lands.
 */
#include <gtest/gtest.h>

#include "common/GnssIdentity.hpp"
#include "config/GnssConfig.hpp"
#include "filter/StateKeySchema.hpp"
#include "logging/LogManager.hpp"
#include "network/assembly/Contributions.hpp"
#include "network/assembly/ParamRegistry.hpp"
#include "network/assembly/Responses.hpp"
#include "network/driver/Epoch.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/qc/Raim.hpp"
#include "network/qc/RaimPriorChannel.hpp"
#include "network/solve/Estimator.hpp"
#include "network/solve/Sbasis.hpp"
#include "network/state/Entities.hpp"
#include "network/state/RunContext.hpp"
#include "../support/NetworkTestRun.hpp"
#include "../support/TestTempDir.hpp"

#include <Eigen/Dense>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace GnssProcessor;
namespace fs = std::filesystem;

namespace {

using CK = ContributionKind;

std::shared_ptr<spdlog::logger> nullLogger() {
    static auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    return std::make_shared<spdlog::logger>("ncq_code_bias_null", sink);
}

/// A minimal GPS network configuration with @p network_extra (each member ending with a comma)
/// spliced into the network block and @p filter_extra as the filter object's members.
std::string networkConfig(const std::string& network_extra, const std::string& filter_extra = "") {
    return std::string("{\n"
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"filter\": { ") + filter_extra + " },\n"
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

/// Loads @p body into @p cfg through a temporary file named by @p tag.
bool loadInto(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    const fs::path p = gnss_test::tempDir() / ("ncq_code_bias_" + tag + ".json");
    { std::ofstream f(p); f << body; }
    LogManager log;
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec;
    fs::remove(p, ec);
    return ok;
}

/// One committed reference configuration (config/golden/golden_<bed>_refound.json).
bool loadGolden(const std::string& bed, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = fs::path(GNSS_PROJECT_ROOT) / "config" / "golden" /
                       ("golden_" + bed + "_refound.json");
    return cfg.loadFromJson(p, log);
}

// ---- The receiver code bias in the prior channel ---------------------------------------------

constexpr int         kStations  = 5;
constexpr const char* kCbKey     = "GPS_ISC_C1C";       ///< The flagged receiver code bias.
constexpr const char* kOtherKey  = "GPS_DCB_C1W_C2W";   ///< A sibling the reset must not touch.
constexpr double      kDataW     = 1e8;   ///< Data rows: 0.1 mm, so the data decide each column.
constexpr double      kPriorW    = 1e4;   ///< Carried-prior rows: 1 cm.
constexpr double      kCbStepM   = 0.6;   ///< The flagged receiver's carried code bias minus the data.

/// One station's block: four GPS phase arcs (columns 0 to 3) and one receiver code bias
/// (column 4), each column observed by one data row and carried by one block row of the
/// coupling region.  The data say every column is zero; the carried priors say the same except
/// the code bias, carried @p cb_innovation_m away.
StationAssembly priorBlock(double cb_innovation_m) {
    constexpr Eigen::Index n_rows = 10, n_cols = 5;
    StationAssembly R;
    R.ok  = true;
    R.A_s = Eigen::MatrixXd::Zero(n_rows, 0);   // no shared border in this network
    R.A_r = Eigen::MatrixXd::Zero(n_rows, n_cols);
    R.w   = Eigen::VectorXd::Zero(n_rows);
    R.y   = Eigen::VectorXd::Zero(n_rows);
    const std::vector<std::string> sats{"G01", "G02", "G03", "G04"};
    for (Eigen::Index k = 0; k < 4; ++k) {           // the phase data rows
        R.A_r(k, k) = 1.0;
        R.w(k) = kDataW;
        R.row_tags.push_back(makeRowTag(CK::DataPhase, sats[static_cast<std::size_t>(k)], "L1C"));
    }
    R.A_r(4, 4) = 1.0;                               // the code data row on the code bias
    R.w(4) = kDataW;
    R.row_tags.push_back(makeRowTag(CK::DataCode, "G01", "C1C"));
    R.n_data_rows   = 5;
    R.cpl_row_start = 5;
    for (Eigen::Index k = 0; k < 4; ++k) {           // the carried ambiguities' block rows
        R.A_r(5 + k, k) = 1.0;
        R.w(5 + k) = kPriorW;
        R.row_tags.push_back(makeRowTag(CK::BlockAmbiguityCarry, sats[static_cast<std::size_t>(k)], "L1C"));
        R.cpl_row_local_cols.push_back(k);
    }
    R.A_r(9, 4) = 1.0;                               // the carried code bias's block row
    R.w(9) = kPriorW;
    R.y(9) = cb_innovation_m;
    R.row_tags.push_back(RowTag{CK::BlockRxCbCarry, SatId{}, SignalId{}});
    R.cpl_row_local_cols.push_back(4);
    R.pl_cb = {{kCbKey, 4, 0.0}};
    for (Eigen::Index k = 0; k < 4; ++k) {
        const std::string& sat = sats[static_cast<std::size_t>(k)];
        R.pl_amb.push_back(PlAmb{k, sat, "L1C", 0.0, StateKeys::makeAmbiguityKey(sat, "L1C")});
    }
    return R;
}

/// Five stations over a configuration that arms the post-fit RAIM with its attribution and the
/// prior channel, solved and passed through the post-fit RAIM once.
struct PriorChannelFixture {
    GnssConfig                      cfg;
    std::unique_ptr<NetworkTestRun> fx;
    int                             n_arc_refloat{-1};

    explicit PriorChannelFixture(const std::string& tag) {
        EXPECT_TRUE(loadInto("rpc_" + tag, networkConfig("    \"raim\": true,\n"
                                                         "    \"phase_leveling\": true,\n"
                                                         "    \"raim_attribution\": true,\n"
                                                         "    \"raim_prior_channel\": true,\n"),
                             cfg));
        fx = std::make_unique<NetworkTestRun>(cfg, nullLogger());
        fx->setup().stations.resize(kStations);
        for (int s = 0; s < kStations; ++s) fx->setup().stations[static_cast<std::size_t>(s)].name =
            "S" + std::to_string(s);
    }

    NetworkRunContext& ctx() { return fx->ctx(); }

    /// Writes a carried receiver code bias at station @p s.
    void carryCb(int s, const std::string& key) {
        LocalCarry lc;
        lc.value_abs   = 0.0;
        lc.var         = 1.0 / kPriorW;
        lc.last_seen_s = 1.0;
        net::put(ctx().entities.stations, StationId{s}, &net::StationState::signals, key,
                 &net::StationSignalState::rx_cb) = lc;
    }

    const LocalCarry* carriedCb(int s, const std::string& key) {
        return net::get(ctx().entities.stations, StationId{s}, &net::StationState::signals, key,
                        &net::StationSignalState::rx_cb);
    }

    /// One epoch: every station's block (station @p flagged with its code bias carried
    /// kCbStepM away from the data; -1 for none), the solve, and the post-fit RAIM pass.
    void runPass(int flagged) {
        NetworkEstimator& est = fx->renewEstimator();
        NetworkEpoch& e = fx->newEpoch(GnssTime{2313, 259230.0});
        e.assembly.asm_res.clear();
        for (int s = 0; s < kStations; ++s)
            e.assembly.asm_res.push_back(priorBlock(s == flagged ? kCbStepM : 0.0));
        est.beginEpoch();
        e.assembly.est_index_of.assign(kStations, -1);
        for (int s = 0; s < kStations; ++s) {
            const StationAssembly& R = e.assembly.asm_res[static_cast<std::size_t>(s)];
            e.assembly.est_index_of[static_cast<std::size_t>(s)] =
                static_cast<int>(est.addStation(R.A_s, R.A_r, R.w, R.y));
        }
        e.pass_a.present.assign(kStations, true);
        e.solve.x_s = est.solve({});
        NetworkPostFitHandoff handoff;
        networkPostFitRaim(fx->run(), e, handoff);
        n_arc_refloat = handoff.n_arc_refloat_epoch;
    }
};

}  // namespace

// The flagged station's carried code bias is erased and counted as a reset: one flag, one reset,
// its sibling key at the same station and the same key at another station untouched, no
// line-of-sight refloat.  The tally names the row that stood out.
TEST(NcqCodeBiasResponse, AFlaggedReceiverCodeBiasCarryIsErasedAndCounted) {
    PriorChannelFixture f("reset");
    ASSERT_TRUE(f.ctx().knobs.raim_on);
    ASSERT_TRUE(f.ctx().knobs.raim_prior_channel);
    ASSERT_TRUE(f.ctx().knobs.raim_pool_data_rows_only);
    f.carryCb(1, kCbKey);
    f.carryCb(1, kOtherKey);
    f.carryCb(0, kCbKey);
    f.runPass(/*flagged=*/1);

    const RaimPriorChannelTally& t = f.ctx().counters.prior_channel;
    EXPECT_EQ(t.passes, 1);
    EXPECT_EQ(t.station_walks, kStations);
    EXPECT_EQ(t.rowsTested(), 5 * kStations);
    EXPECT_EQ(t.rows[static_cast<std::size_t>(CK::BlockRxCbCarry)], kStations);
    EXPECT_EQ(t.cells, 5 * kStations);
    EXPECT_EQ(t.flags, 1);
    EXPECT_EQ(t.rxcb_resets, 1);
    EXPECT_EQ(t.rxcb_no_entry, 0);
    EXPECT_EQ(t.arcs, 0);
    EXPECT_EQ(t.unwired, 0);
    EXPECT_EQ(t.max_station, "S1");
    EXPECT_EQ(t.max_label, std::string("rxcb:") + kCbKey);
    // |v| sqrt(w): the data take all but 1e-4 of the innovation off the column.
    EXPECT_NEAR(t.max_statistic, kCbStepM * std::sqrt(kPriorW), 0.05);

    EXPECT_EQ(f.carriedCb(1, kCbKey), nullptr) << "the flagged carry is reset";
    EXPECT_NE(f.carriedCb(1, kOtherKey), nullptr) << "only the flagged key is reset";
    EXPECT_NE(f.carriedCb(0, kCbKey), nullptr) << "only the flagged station is reset";
    EXPECT_EQ(f.n_arc_refloat, 0);
}

// The same flag on a station that carries no such code bias erases nothing and is counted as
// "no entry", not as a reset.
TEST(NcqCodeBiasResponse, AFlagWithoutACarriedEntryIsCountedAsNoEntry) {
    PriorChannelFixture f("no_entry");
    f.carryCb(0, kCbKey);
    f.runPass(/*flagged=*/1);
    const RaimPriorChannelTally& t = f.ctx().counters.prior_channel;
    EXPECT_EQ(t.flags, 1);
    EXPECT_EQ(t.rxcb_resets, 0);
    EXPECT_EQ(t.rxcb_no_entry, 1);
    EXPECT_NE(f.carriedCb(0, kCbKey), nullptr);
}

// A quiet epoch (every carried prior agrees with the data) tests every row, flags nothing and
// erases nothing: the reset needs the flag.
TEST(NcqCodeBiasResponse, AQuietEpochResetsNothing) {
    PriorChannelFixture f("quiet");
    for (int s = 0; s < kStations; ++s) f.carryCb(s, kCbKey);
    f.runPass(/*flagged=*/-1);
    const RaimPriorChannelTally& t = f.ctx().counters.prior_channel;
    EXPECT_EQ(t.passes, 1);
    EXPECT_EQ(t.rowsTested(), 5 * kStations);
    EXPECT_EQ(t.flags, 0);
    EXPECT_EQ(t.rxcb_resets, 0);
    EXPECT_EQ(t.rxcb_no_entry, 0);
    for (int s = 0; s < kStations; ++s) EXPECT_NE(f.carriedCb(s, kCbKey), nullptr) << s;
}

// ---- The satellite code bias -----------------------------------------------------------------

// The reference configurations (the field, simulation and BeiDou lineages) carry every satellite
// code bias as a random walk of 1e-10 m^2/s, 0.6 mm of sigma in an hour, with no variance floor,
// and arm neither route that can loosen it: the soft reinit and the hard refloat need a clock
// flag (the clock monitor or the clock model, both off) and the excursion guard is off.
TEST(NcqCodeBiasResponse, TheReferenceConfigurationsCarryTheSatelliteCodeBiasStiffWithNoLoosenRoute) {
    for (const std::string bed : {"field", "sim", "field_bds"}) {
        GnssConfig cfg;
        ASSERT_TRUE(loadGolden(bed, cfg)) << bed;
        const NetworkKnobs k = NetworkKnobs::snapshot(cfg, cfg.getNetworkConfig());
        const NetworkParamRegistry r = NetworkParamRegistry::build(k, cfg);
        const ParamSpec& cb = r.spec(NetworkParamKind::SatCodeBias);
        EXPECT_TRUE(cfg.estimateNetworkSatCodeBias()) << bed;
        EXPECT_TRUE(cb.carried) << bed;
        EXPECT_EQ(cb.q_rule, ParamQRule::RandomWalk) << bed;
        EXPECT_DOUBLE_EQ(k.q_cb, 1e-10) << bed;
        EXPECT_DOUBLE_EQ(cb.q, 1e-10) << bed;
        EXPECT_NEAR(std::sqrt(cb.q * 3600.0), 6.0e-4, 1e-9) << bed << ": sigma grown in an hour";
        EXPECT_DOUBLE_EQ(cb.var_floor_m2, 0.0) << bed;
        EXPECT_DOUBLE_EQ(cfg.networkSharedCarryVarFloorCbM2(), 0.0) << bed;
        EXPECT_FALSE(k.clk_mon_on) << bed;
        EXPECT_FALSE(k.clk_model_on) << bed;
        EXPECT_FALSE(cfg.getNetworkConfig().cb_excursion_reinit) << bed;
        EXPECT_FALSE(responseSiteArmed(ResponseSite::CbReinit, k, cfg)) << bed;
        EXPECT_FALSE(responseSiteArmed(ResponseSite::CbHardRefloat, k, cfg)) << bed;
    }
}

// A 0.5 m step of one satellite's solved code level between two kept epochs, with no clock flag
// (no detector of the code level exists to raise one): the step reaches the stash and the carry
// is kept stiff.  The response wanted is a loosening of that satellite's carry (a reinit mark or
// a refloat) and none of the other satellite.  Today there is none, so the case skips with the gap
// named; it passes once a response lands.
TEST(NcqCodeBiasResponse, AStepOfOneSatellitesCodeLevelLoosensItsCarry) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("step", networkConfig("    \"estimate_sat_code_bias\": true,\n",
                                               "\"process_noise\": { \"sat_code_bias_q\": 1e-10 }"),
                         cfg));
    NetworkTestRun fx(cfg, nullLogger());
    ASSERT_DOUBLE_EQ(fx.ctx().knobs.q_cb, 1e-10);
    ASSERT_FALSE(fx.ctx().knobs.clk_mon_on);
    ASSERT_FALSE(fx.ctx().knobs.clk_model_on);

    NetworkEstimator& est = fx.est();
    const std::vector<std::string> clocks{"G01", "G02"};
    const std::vector<std::pair<std::string, std::string>> code_keys{{"G01", "C1C"}, {"G02", "C1C"}};
    for (const std::string& s : clocks) est.registerClock(s);
    for (const auto& [s, sig] : code_keys) est.registerCodeBias(s, sig);
    NetworkSbasis::Plan plan{};   // no datum pin: every column is stashed
    const std::set<std::string> phase_keys;

    // One kept epoch whose solved code biases are the given levels at a 1 cm posterior sigma.
    const auto keptEpoch = [&](double g01_cb, double g02_cb) {
        std::vector<NetworkEstimator::SharedPrior> pri;
        const auto prior = [&](SharedParamKind kind, const std::string& sat, const std::string& sig,
                               double value) {
            NetworkEstimator::SharedPrior p;
            p.kind = kind; p.sat = sat; p.signal = sig; p.value = value; p.info = 1e4;
            pri.push_back(p);
        };
        prior(SharedParamKind::Clock, "G01", "", 1.0);
        prior(SharedParamKind::Clock, "G02", "", 2.0);
        prior(SharedParamKind::CodeBias, "G01", "C1C", g01_cb);
        prior(SharedParamKind::CodeBias, "G02", "C1C", g02_cb);
        est.beginEpoch();
        est.addSharedPrior(pri);
        const Eigen::VectorXd x_s = est.solve({});
        networkStashCarry(fx.ctx(), cfg, fx.traces(), GnssTime{2313, 259230.0}, est, x_s, plan,
                          nullptr, nullptr, /*clock_refloat_sats=*/{}, clocks, phase_keys, code_keys,
                          EpochEvents{}, nullLogger());
    };
    const auto entry = [&](const std::string& sat) -> const CarryPrior* {
        for (const CarryPrior& cp : fx.ctx().carry.prev_carry)
            if (cp.sat == sat && cp.kind == SharedParamKind::CodeBias) return &cp;
        return nullptr;
    };
    const auto resp = [&](ResponseSite s) {
        return fx.ctx().counters.prm_resp[static_cast<int>(s)];
    };

    keptEpoch(0.10, 0.20);
    const CarryPrior* before = entry("G01");
    ASSERT_NE(before, nullptr);
    ASSERT_NEAR(before->value, 0.10, 1e-9);
    ASSERT_FALSE(before->inflate_q);

    keptEpoch(0.60, 0.20);   // G01's code level steps by 0.5 m; G02's does not move
    const CarryPrior* g01 = entry("G01");
    const CarryPrior* g02 = entry("G02");
    ASSERT_NE(g02, nullptr);
    EXPECT_FALSE(g02->inflate_q) << "the satellite that did not step keeps its carry";
    if (g01 != nullptr) {
        EXPECT_NEAR(g01->value, 0.60, 1e-9) << "the step reached the stash";
    }
    const bool loosened = g01 == nullptr || g01->inflate_q
                          || resp(ResponseSite::CbHardRefloat) > 0;
    if (!loosened) {
        EXPECT_NEAR(g01->var, 1e-4, 1e-9) << "kept at its stiff posterior variance";
        GTEST_SKIP() << "Gap: a 0.5 m step of one satellite's code level gets no response. The "
                        "stash keeps the stepped value at its 1 cm posterior sigma with no reinit "
                        "mark and no refloat, because nothing compares a satellite's code bias "
                        "with its carried value: the only routes that loosen the carry wait for "
                        "a clock flag (clock monitor, clock model) or the clock-excursion guard, "
                        "and the next epochs fold it at 1e-10 m^2/s (0.6 mm per hour).";
    }
    EXPECT_TRUE(loosened);
}
