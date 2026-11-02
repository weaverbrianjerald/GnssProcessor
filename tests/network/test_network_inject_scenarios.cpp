/**
 * @file test_network_inject_scenarios.cpp
 * @brief Event harness: the scenario kinds of card SMX (scoped outages, a broadcast
 *        health flip, a held-coordinate step), parse contract and epoch semantics.
 *
 * Two halves, both structural, on the pattern of test_network_inject.cpp:
 *
 *  * `NetworkInjectScenarioConfig`: the new kinds and scopes parse, and every field that
 *    would read as a scope the kind does not have fails the load, by name.
 *  * `NetworkInjectScenarioEpoch`: each kind acts on exactly its scope, for exactly its
 *    window, and is an event at both edges (an onset and a closing edge, counted); the
 *    held coordinate comes back to the bit; an event that cannot act fails the run end.
 *
 * The epoch half drives `networkInjectEpoch` against a hand-built `SyncedEpoch` and, for
 * the two kinds that act on the setup, a hand-built `NetworkSetupResult`.  No fixture.
 */
#include <gtest/gtest.h>

#include "network/state/RunContext.hpp"
#include "network/driver/SetupResult.hpp"
#include "network/state/StationParams.hpp"
#include "config/GnssConfig.hpp"
#include "network/carry/Inject.hpp"
#include "logging/LogManager.hpp"
#include "math/Coordinates.hpp"
#include "network/diag/RunTraces.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include "../support/TestTempDir.hpp"   // per-process, per-test scratch directory

using namespace GnssProcessor;

namespace {

    namespace fs = std::filesystem;

    const Eigen::Vector3d kSta1(-2493304.99, -4655214.79, 3565497.67);
    const Eigen::Vector3d kSta2(-2281621.75, -1453585.08, 5756964.89);

    /// Minimum loadable PPP-RTK network config with @p extra_network spliced into the
    /// network section (a "sat_health" mode, an "inject" block).
    std::string networkConfig(const std::string& extra_network) {
        return std::string(
            "{\n"
            "  \"processing\": {\n"
            "    \"mode\": \"ppp-rtk-network\",\n"
            "    \"active_systems\": [ \"GPS\" ],\n"
            "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
            "  },\n"
            "  \"network\": {\n"
            "    \"clock_datum_station\": \"STA1\",\n"
            "    \"trace_events\": true,\n")
            + extra_network +
            "    \"stations\": [\n"
            "      { \"name\": \"STA1\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
            "      { \"name\": \"STA2\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
            "    ]\n"
            "  }\n"
            "}\n";
    }

    /// The "inject" block for @p events, plus any other network keys in @p more.
    std::string injectBlock(const nlohmann::json& events, const std::string& more = "") {
        return more + "    \"inject\": " + nlohmann::json{{"events", events}}.dump() + ",\n";
    }

    bool loadWith(const std::string& extra_network, GnssConfig& cfg, LogManager& log,
                  const std::string& tag) {
        const fs::path p = gnss_test::tempDir() / ("netinjsc_cfg_" + tag + ".json");
        {
            std::ofstream f(p);
            f << networkConfig(extra_network);
        }
        const bool ok = cfg.loadFromJson(p, log);
        std::error_code ec;
        fs::remove(p, ec);
        return ok;
    }

    /// One GPS record with L1C/C1C/D1C and L2W/C2W, or only the first band when @p l2.
    RinexObservation obs(const std::string& sat, bool l2 = true) {
        RinexObservation o;
        o.satellite = sat;
        o.system = System::GPS;
        o.measurements["L1C"] = 1.0e8;
        o.measurements["C1C"] = 2.0e7;
        o.measurements["D1C"] = -1000.0;
        if (l2) {
            o.measurements["L2W"] = 0.8e8;
            o.measurements["C2W"] = 2.0e7 + 1.0;
        }
        return o;
    }

    SyncedEpoch epochAt(double tow) {
        SyncedEpoch se;
        se.time.week = 2313;
        se.time.tow = tow;
        se.by_station["STA1"] = { obs("G05"), obs("G07") };
        se.by_station["STA2"] = { obs("G05") };
        return se;
    }

    const RinexObservation* find(const SyncedEpoch& se, const std::string& sta,
                                 const std::string& sat) {
        const auto it = se.by_station.find(sta);
        if (it == se.by_station.end()) return nullptr;
        for (const auto& o : it->second)
            if (o.satellite == sat) return &o;
        return nullptr;
    }

    NetStation station(const GnssConfig& cfg, const std::string& name, const Eigen::Vector3d& x) {
        NetStation s;
        s.name = name;
        s.ecef = x;
        s.params = startStationParams(cfg, x);
        s.pstate.position = x;
        return s;
    }

    /// Config + context + traces + a setup with the two roster stations.
    struct Harness {
        LogManager lm;
        GnssConfig cfg;
        std::unique_ptr<NetworkRunContext> ctx;
        std::unique_ptr<NetworkRunTraces> traces;
        std::shared_ptr<spdlog::logger> log;
        NetworkSetupResult setup;

        Harness(const std::string& extra_network, const std::string& tag) {
            EXPECT_TRUE(loadWith(extra_network, cfg, lm, tag));
            ctx = std::make_unique<NetworkRunContext>(cfg, cfg.getNetworkConfig());
            traces = std::make_unique<NetworkRunTraces>(gnss_test::tempDir());
            log = lm.getLogger("main_log");
            setup.window_t0_s = 2313.0 * 604800.0 + 900.0;
            setup.stations.push_back(station(cfg, "STA1", kSta1));
            setup.stations.push_back(station(cfg, "STA2", kSta2));
            networkInjectArm(*ctx, cfg, lm, log, &setup);
        }
        const SyncedEpoch& step(SyncedEpoch& se, SyncedEpoch& scratch) {
            return networkInjectEpoch(*ctx, log, *traces, se, scratch);
        }
    };

    /// One event object from its fields (an explicit object: a braced list of pairs
    /// would otherwise build an array of arrays).
    nlohmann::json ev(std::initializer_list<std::pair<const std::string, nlohmann::json>> f) {
        nlohmann::json j = nlohmann::json::object();
        for (const auto& kv : f) j[kv.first] = kv.second;
        return j;
    }

} // namespace

// ---------------------------------------------------------------- parse contract
TEST(NetworkInjectScenarioConfig, EachScenarioKindParses) {
    LogManager lm;
    GnssConfig cfg;
    const nlohmann::json events = nlohmann::json::array({
        ev({{"kind", "outage"}, {"tow", 1.0}, {"station", "STA1"}, {"sat", "G05"},
            {"signal", "L1C;L2W"}, {"duration_s", 60.0}, {"delta_cycles", 9.0}, {"flag_lli", true}}),
        ev({{"kind", "outage"}, {"tow", 1.0}, {"station", "STA1"}, {"sat", "all"},
            {"duration_s", 60.0}}),
        ev({{"kind", "sat_health"}, {"tow", 1.0}, {"sat", "G05"}, {"duration_s", 600.0}}),
        ev({{"kind", "station_step"}, {"tow", 1.0}, {"station", "STA2"}, {"east_m", 0.01},
            {"up_m", -1.0}})});
    ASSERT_TRUE(loadWith(injectBlock(events), cfg, lm, "parse"));
    const auto& ic = cfg.getNetworkConfig().inject;
    ASSERT_EQ(ic.events.size(), 4u);
    EXPECT_EQ(ic.events[0].signal, "L1C;L2W");
    EXPECT_DOUBLE_EQ(ic.events[0].delta_cycles, 9.0);
    EXPECT_EQ(ic.events[1].sat, "all");
    EXPECT_EQ(ic.events[2].kind, "sat_health");
    EXPECT_DOUBLE_EQ(ic.events[3].east_m, 0.01);
    EXPECT_DOUBLE_EQ(ic.events[3].north_m, 0.0);
    EXPECT_DOUBLE_EQ(ic.events[3].up_m, -1.0);
}

class NetworkInjectScenarioRejects : public ::testing::TestWithParam<nlohmann::json> {};

TEST_P(NetworkInjectScenarioRejects, MalformedScenarioEventFailsTheLoad) {
    LogManager lm;
    GnssConfig cfg;
    EXPECT_FALSE(loadWith(injectBlock(nlohmann::json::array({GetParam()})), cfg, lm, "rej"));
}

INSTANTIATE_TEST_SUITE_P(
    Malformed, NetworkInjectScenarioRejects,
    ::testing::Values(
        // an outage signal scope that names a CODE observable
        nlohmann::json{{"kind", "outage"}, {"tow", 1.0}, {"station", "STA1"}, {"sat", "G05"},
                       {"signal", "L1C;C2W"}, {"duration_s", 60.0}},
        // sat_health with no satellite, with "all", with a station, with a signal
        nlohmann::json{{"kind", "sat_health"}, {"tow", 1.0}},
        nlohmann::json{{"kind", "sat_health"}, {"tow", 1.0}, {"sat", "all"}},
        nlohmann::json{{"kind", "sat_health"}, {"tow", 1.0}, {"sat", "G05"}, {"station", "STA1"}},
        nlohmann::json{{"kind", "sat_health"}, {"tow", 1.0}, {"sat", "G05"}, {"signal", "L1C"}},
        // sat_health with a negative duration
        nlohmann::json{{"kind", "sat_health"}, {"tow", 1.0}, {"sat", "G05"}, {"duration_s", -1.0}},
        // station_step with no station, a zero step, a sat (it reads as a scope)
        nlohmann::json{{"kind", "station_step"}, {"tow", 1.0}, {"east_m", 1.0}},
        nlohmann::json{{"kind", "station_step"}, {"tow", 1.0}, {"station", "STA1"}},
        nlohmann::json{{"kind", "station_step"}, {"tow", 1.0}, {"station", "STA1"},
                       {"sat", "G05"}, {"up_m", 1.0}}));

// ---------------------------------------------------------------- outage scopes
TEST(NetworkInjectScenarioEpoch, SignalOutageDropsOnlyTheListedChannelsForTheWindow) {
    Harness h{injectBlock(nlohmann::json::array({
        ev({{"kind", "outage"}, {"tow", 1000.0}, {"station", "STA1"}, {"sat", "G05"},
            {"signal", "L1C"}, {"duration_s", 60.0}})})), "sigout"};
    const double tows[] = { 970.0, 1000.0, 1030.0, 1060.0, 1090.0 };
    const bool  gone[]  = { false,  true,   true,   false,  false };
    for (int k = 0; k < 5; ++k) {
        SyncedEpoch se = epochAt(tows[k]), scratch;
        const SyncedEpoch& out = h.step(se, scratch);
        const RinexObservation* g05 = find(out, "STA1", "G05");
        ASSERT_NE(g05, nullptr) << "the record stays: L2W and C2W track through";
        // Band 1, attribute C: carrier, code and Doppler leave together.
        EXPECT_EQ(g05->measurements.count("L1C"), gone[k] ? 0u : 1u) << tows[k];
        EXPECT_EQ(g05->measurements.count("C1C"), gone[k] ? 0u : 1u) << tows[k];
        EXPECT_EQ(g05->measurements.count("D1C"), gone[k] ? 0u : 1u) << tows[k];
        EXPECT_EQ(g05->measurements.count("L2W"), 1u);
        EXPECT_EQ(g05->measurements.count("C2W"), 1u);
        // The other satellite and the other station keep band 1.
        EXPECT_EQ(find(out, "STA1", "G07")->measurements.count("L1C"), 1u);
        EXPECT_EQ(find(out, "STA2", "G05")->measurements.count("L1C"), 1u);
    }
    EXPECT_EQ(h.ctx->inject.n_window[0], 2);
    EXPECT_TRUE(h.ctx->inject.release_logged[0]) << "the return is the closing edge";
    EXPECT_EQ(h.ctx->inject.n_outage_signal_obs, 6);   // 3 observables x 2 epochs
    EXPECT_EQ(networkInjectRunEnd(*h.ctx, h.log), 0);
}

TEST(NetworkInjectScenarioEpoch, SignalOutageRemovesARecordLeftWithoutCodeOrPhase) {
    Harness h{injectBlock(nlohmann::json::array({
        ev({{"kind", "outage"}, {"tow", 1000.0}, {"station", "STA1"}, {"sat", "G09"},
            {"signal", "L1C"}, {"duration_s", 30.0}})})), "sigrec"};
    SyncedEpoch se = epochAt(1000.0), scratch;
    se.by_station["STA1"].push_back(obs("G09", /*l2=*/false));   // band 1 only
    const SyncedEpoch& out = h.step(se, scratch);
    EXPECT_EQ(find(out, "STA1", "G09"), nullptr);
    EXPECT_NE(find(out, "STA1", "G05"), nullptr);
}

TEST(NetworkInjectScenarioEpoch, WholeStationOutageEmptiesOneStationOnly) {
    Harness h{injectBlock(nlohmann::json::array({
        ev({{"kind", "outage"}, {"tow", 1000.0}, {"station", "STA1"}, {"sat", "all"},
            {"duration_s", 30.0}})})), "staout"};
    SyncedEpoch a = epochAt(1000.0), sa;
    const SyncedEpoch& oa = h.step(a, sa);
    EXPECT_TRUE(oa.by_station.at("STA1").empty());
    EXPECT_EQ(oa.by_station.at("STA2").size(), 1u);
    SyncedEpoch b = epochAt(1030.0), sb;
    const SyncedEpoch& ob = h.step(b, sb);
    EXPECT_EQ(&ob, &b) << "after the window nothing is due: no copy";
    EXPECT_EQ(ob.by_station.at("STA1").size(), 2u);
    EXPECT_EQ(h.ctx->inject.n_outage_station_recs, 2);
    EXPECT_TRUE(h.ctx->inject.release_logged[0]);
}

TEST(NetworkInjectScenarioEpoch, ReacquisitionShiftPersistsAndLliIsRaisedOnceAtTheReturn) {
    Harness h{injectBlock(nlohmann::json::array({
        ev({{"kind", "outage"}, {"tow", 1000.0}, {"station", "STA1"}, {"sat", "G05"},
            {"signal", "L1C;L2W"}, {"duration_s", 30.0}, {"delta_cycles", 9.0},
            {"flag_lli", true}})})), "reacq"};
    SyncedEpoch w = epochAt(1000.0), sw;
    h.step(w, sw);                                     // the window: both bands dropped
    for (int k = 1; k <= 3; ++k) {
        SyncedEpoch se = epochAt(1000.0 + 30.0 * k), scratch;
        const SyncedEpoch& out = h.step(se, scratch);
        const RinexObservation* g05 = find(out, "STA1", "G05");
        ASSERT_NE(g05, nullptr);
        EXPECT_DOUBLE_EQ(g05->measurements.at("L1C"), 1.0e8 + 9.0) << "epoch " << k;
        EXPECT_DOUBLE_EQ(g05->measurements.at("L2W"), 0.8e8 + 9.0) << "epoch " << k;
        EXPECT_DOUBLE_EQ(g05->measurements.at("C1C"), 2.0e7) << "code is not shifted";
        const int lli = g05->lli_flags.count("L1C") ? g05->lli_flags.at("L1C") : 0;
        EXPECT_EQ(lli & 0x1, k == 1 ? 1 : 0) << "LLI at the first epoch back only";
        EXPECT_DOUBLE_EQ(find(out, "STA1", "G07")->measurements.at("L1C"), 1.0e8);
    }
    EXPECT_EQ(h.ctx->inject.n_outage_return_obs, 2);
}

// ---------------------------------------------------------------- broadcast health
TEST(NetworkInjectScenarioEpoch, UnreadHealthFlipFiresWithoutTouchingTheEpoch) {
    Harness h{injectBlock(nlohmann::json::array({
        ev({{"kind", "sat_health"}, {"tow", 1000.0}, {"sat", "G05"}, {"duration_s", 60.0}})})),
              "hlt_unread"};
    EXPECT_EQ(h.ctx->inject.health_readers, "none");
    for (int k = 0; k < 4; ++k) {
        SyncedEpoch se = epochAt(1000.0 + 30.0 * k), scratch;
        const SyncedEpoch& out = h.step(se, scratch);
        EXPECT_EQ(&out, &se) << "nothing reads the flag: no copy";
    }
    EXPECT_EQ(h.ctx->inject.n_applied[0], 2);           // in force at 1000 and 1030
    EXPECT_TRUE(h.ctx->inject.release_logged[0]);
    EXPECT_TRUE(h.setup.sat_health.sats.empty()) << "no reader: the series is not written";
    EXPECT_EQ(networkInjectRunEnd(*h.ctx, h.log), 0);
}

TEST(NetworkInjectScenarioEpoch, ExcludeReadsTheFlagAtEveryStationForItsSpan) {
    Harness h{injectBlock(nlohmann::json::array({
        ev({{"kind", "sat_health"}, {"tow", 1000.0}, {"sat", "G05"}, {"duration_s", 60.0}})}),
                          "    \"sat_health\": \"exclude\",\n"), "hlt_excl"};
    EXPECT_EQ(h.ctx->inject.health_readers, "exclude");
    const double week_s = 2313.0 * 604800.0;
    // The span is in the series the elections query, and only the span.
    EXPECT_FALSE(h.setup.sat_health.unhealthyAt("G05", week_s + 970.0));
    EXPECT_TRUE(h.setup.sat_health.unhealthyAt("G05", week_s + 1000.0));
    EXPECT_TRUE(h.setup.sat_health.unhealthyAt("G05", week_s + 1030.0));
    EXPECT_FALSE(h.setup.sat_health.unhealthyAt("G05", week_s + 1060.0));
    EXPECT_FALSE(h.setup.sat_health.unhealthyAt("G07", week_s + 1000.0));
    const double tows[] = { 970.0, 1000.0, 1030.0, 1060.0 };
    const bool  gone[]  = { false, true,   true,   false };
    for (int k = 0; k < 4; ++k) {
        SyncedEpoch se = epochAt(tows[k]), scratch;
        const SyncedEpoch& out = h.step(se, scratch);
        EXPECT_EQ(find(out, "STA1", "G05") == nullptr, gone[k]) << tows[k];
        EXPECT_EQ(find(out, "STA2", "G05") == nullptr, gone[k]) << tows[k];
        EXPECT_NE(find(out, "STA1", "G07"), nullptr);
    }
    EXPECT_EQ(h.ctx->inject.n_health_obs_removed, 4);   // 2 stations x 2 epochs
    EXPECT_TRUE(h.ctx->inject.release_logged[0]);
}

TEST(NetworkInjectScenarioEpoch, HealthSpanKeepsTheBroadcastStatusAroundIt) {
    Harness h{injectBlock(nlohmann::json::array({
        ev({{"kind", "sat_health"}, {"tow", 1000.0}, {"sat", "G05"}, {"duration_s", 60.0}})}),
                          "    \"sat_health\": \"pivot\",\n"), "hlt_keep"};
    // Re-arm on a series that already flags G05 from week + 2000 on (a broadcast flag).
    const double week_s = 2313.0 * 604800.0;
    h.setup.sat_health.sats["G05"] = { {week_s + 500.0, false}, {week_s + 2000.0, true} };
    networkInjectArm(*h.ctx, h.cfg, h.lm, h.log, &h.setup);
    EXPECT_FALSE(h.setup.sat_health.unhealthyAt("G05", week_s + 100.0));
    EXPECT_TRUE(h.setup.sat_health.unhealthyAt("G05", week_s + 1000.0));
    EXPECT_FALSE(h.setup.sat_health.unhealthyAt("G05", week_s + 1500.0));
    EXPECT_TRUE(h.setup.sat_health.unhealthyAt("G05", week_s + 2500.0));
}

// ---------------------------------------------------------------- held coordinate
TEST(NetworkInjectScenarioEpoch, StationStepMovesTheHeldCoordinateAndBackToTheBit) {
    Harness h{injectBlock(nlohmann::json::array({
        ev({{"kind", "station_step"}, {"tow", 1000.0}, {"station", "STA2"}, {"east_m", 1.0},
            {"duration_s", 60.0}})})), "step"};
    NetStation& s2 = h.setup.stations[1];
    const Eigen::Vector3d x0 = s2.ecef;
    const int kx = s2.params.position[0];
    ASSERT_GE(kx, 0);
    const double xlin0 = s2.params.slots[static_cast<std::size_t>(kx)].x_lin;
    const Coordinates::Llh llh = Coordinates::ecefToLlh(x0);
    const Eigen::Vector3d east = Coordinates::enuToEcefMatrix(llh.lat_rad, llh.lon_rad).col(0);

    SyncedEpoch a = epochAt(1000.0), sa;
    const SyncedEpoch& oa = h.step(a, sa);
    EXPECT_EQ(&oa, &a) << "the observations are not touched";
    EXPECT_NEAR((s2.ecef - x0 - east).norm(), 0.0, 1e-9);
    EXPECT_NEAR((s2.pstate.position - s2.ecef).norm(), 0.0, 0.0);
    EXPECT_NEAR(s2.params.slots[static_cast<std::size_t>(kx)].x_lin - xlin0, east.x(), 1e-9);
    EXPECT_EQ(h.setup.stations[0].ecef, kSta1) << "the other station stays";

    SyncedEpoch b = epochAt(1030.0), sb;
    h.step(b, sb);
    EXPECT_NEAR((s2.ecef - x0 - east).norm(), 0.0, 1e-9) << "a step, applied once";
    SyncedEpoch c = epochAt(1060.0), sc;
    h.step(c, sc);                                       // the closing edge
    EXPECT_EQ(s2.ecef, x0) << "the return restores the coordinate exactly";
    EXPECT_EQ(s2.pstate.position, x0);
    EXPECT_EQ(h.ctx->inject.n_station_steps, 1);
    EXPECT_EQ(h.ctx->inject.n_station_returns, 1);
    EXPECT_EQ(h.ctx->inject.n_applied[0], 2);
    EXPECT_EQ(networkInjectRunEnd(*h.ctx, h.log), 0);
}

TEST(NetworkInjectScenarioEpoch, StationStepOnAnUnknownStationIsAHardRunEndError) {
    Harness h{injectBlock(nlohmann::json::array({
        ev({{"kind", "station_step"}, {"tow", 1000.0}, {"station", "NOPE"}, {"up_m", 0.1}})})),
              "step_unk"};
    SyncedEpoch a = epochAt(1000.0), sa;
    h.step(a, sa);
    EXPECT_EQ(h.ctx->inject.n_applied[0], 0);
    EXPECT_EQ(networkInjectRunEnd(*h.ctx, h.log), 1);
}
