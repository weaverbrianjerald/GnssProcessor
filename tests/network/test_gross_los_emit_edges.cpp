/**
 * @file test_gross_los_emit_edges.cpp
 * @brief The gross line-of-sight screen's emission mark at the edges of a declaration
 *        (`network.gross_los_emit_edges`, card GLM) and the injector kind `code_step`.
 *
 * Under `network.gross_los_emit_exclude` the screen marks a satellite discontinuous at
 * every epoch in which a station holds a declared line of sight to it.  On 2024 day 129 one
 * station kept six GPS satellites marked for 77 minutes, and every user lost their phase
 * continuity for as long.  Card GLM's measurement (A0) found a step in the emitted values at
 * a declaration's onset and at a cold-bound release (which re-seeds the carried ambiguity),
 * and none inside a standing declaration or at an ordinary clear.  The key marks the two
 * edges only.
 *
 * What is pinned here: the per-line-of-sight onset rule, the mark decision table (the key
 * off keeps the original rule on every input), the loader default and its parse, and the
 * `code_step` fault: it moves the code observables of one satellite at one station inside
 * its window and nothing else.  The delivery of an edge mark: every path that can keep a satellite
 * out of the emission between the screen's decision and the written R record (suite
 * GrossLosEmitMarkDelivery) lands exactly one mark at the satellite's first emitted epoch.
 * What a run does is graded by the card's readings G1 to G4.
 */
#include <gtest/gtest.h>

#include "network/state/RunContext.hpp"
#include "network/driver/SetupResult.hpp"
#include "network/state/StationParams.hpp"
#include "config/GnssConfig.hpp"
#include "network/carry/Inject.hpp"
#include "network/qc/GrossLos.hpp"
#include "network/emit/SolPrep.hpp"
#include "logging/LogManager.hpp"
#include "network/diag/RunTraces.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <nlohmann/json.hpp>

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

    /// Minimum loadable PPP-RTK network config with @p extra_network spliced into the network section.
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

    bool loadWith(const std::string& extra_network, GnssConfig& cfg, LogManager& log,
                  const std::string& tag) {
        const fs::path p = gnss_test::tempDir() / ("glm_cfg_" + tag + ".json");
        {
            std::ofstream f(p);
            f << networkConfig(extra_network);
        }
        const bool ok = cfg.loadFromJson(p, log);
        std::error_code ec;
        fs::remove(p, ec);
        return ok;
    }

    /// One GPS record with L1C/C1C/D1C and L2W/C2W.
    RinexObservation obs(const std::string& sat) {
        RinexObservation o;
        o.satellite = sat;
        o.system = System::GPS;
        o.measurements["L1C"] = 1.0e8;
        o.measurements["C1C"] = 2.0e7;
        o.measurements["D1C"] = -1000.0;
        o.measurements["L2W"] = 0.8e8;
        o.measurements["C2W"] = 2.0e7 + 1.0;
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

    /// Config + context + traces + a setup with the two roster stations, the injector armed.
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

    std::string codeStepBlock(double tow, double step_m, double duration_s) {
        nlohmann::json e = nlohmann::json::object();
        e["kind"] = "code_step";
        e["tow"] = tow;
        e["station"] = "STA1";
        e["sat"] = "G05";
        e["step_m"] = step_m;
        e["duration_s"] = duration_s;
        return "    \"inject\": " + nlohmann::json{{"events", nlohmann::json::array({e})}}.dump() + ",\n";
    }

} // namespace

// ---------------------------------------------------------------- the onset rule
TEST(GrossLosEmitEdges, AnOnsetIsTheFirstExcludedEpochOnly) {
    GrossLosState s;
    EXPECT_FALSE(networkGrossLosEmitOnset(s, false));   // in the set: no edge
    EXPECT_TRUE(networkGrossLosEmitOnset(s, true));     // leaves the set: the onset
    EXPECT_FALSE(networkGrossLosEmitOnset(s, true));    // standing: no edge
    EXPECT_FALSE(networkGrossLosEmitOnset(s, true));
    EXPECT_FALSE(networkGrossLosEmitOnset(s, false));   // back in the set: not an onset
    EXPECT_FALSE(s.emit_out);
    EXPECT_TRUE(networkGrossLosEmitOnset(s, true));     // a new declaration is a new onset
}

TEST(GrossLosEmitEdges, EachLineOfSightHasItsOwnOnset) {
    // A second station joining a standing declaration of the same satellite is an onset of
    // its own line of sight, so the satellite is marked again (its contributor set changed).
    GrossLosState a, b;
    EXPECT_TRUE(networkGrossLosEmitOnset(a, true));
    EXPECT_FALSE(networkGrossLosEmitOnset(b, false));
    EXPECT_FALSE(networkGrossLosEmitOnset(a, true));
    EXPECT_TRUE(networkGrossLosEmitOnset(b, true));
}

// ---------------------------------------------------------------- the mark decision
TEST(GrossLosEmitEdges, WithTheKeyOffEveryExcludedEpochIsMarked) {
    for (bool onset : { false, true })
        for (bool reseed : { false, true })
            EXPECT_EQ(networkGrossLosEmitMark(false, onset, reseed), GrossLosEmitMark::Mark)
                << "onset=" << onset << " reseed=" << reseed;
}

TEST(GrossLosEmitEdges, WithTheKeyOnOnlyTheEdgesAreMarked) {
    EXPECT_EQ(networkGrossLosEmitMark(true, true, false), GrossLosEmitMark::Mark);
    EXPECT_EQ(networkGrossLosEmitMark(true, false, true), GrossLosEmitMark::Mark);
    EXPECT_EQ(networkGrossLosEmitMark(true, true, true), GrossLosEmitMark::Mark);
    EXPECT_EQ(networkGrossLosEmitMark(true, false, false), GrossLosEmitMark::Standing);
}

// ---------------------------------------------------------------- the key
TEST(GrossLosEmitEdgesConfig, TheKeyDefaultsOffAndParses) {
    {
        LogManager lm; GnssConfig cfg;
        ASSERT_TRUE(loadWith("", cfg, lm, "edges_off"));
        EXPECT_FALSE(cfg.getNetworkConfig().gross_los_emit_edges);
    }
    {
        LogManager lm; GnssConfig cfg;
        ASSERT_TRUE(loadWith("    \"gross_los_screen\": true,\n"
                             "    \"gross_los_emit_exclude\": true,\n"
                             "    \"gross_los_emit_edges\": true,\n", cfg, lm, "edges_on"));
        EXPECT_TRUE(cfg.getNetworkConfig().gross_los_emit_edges);
        EXPECT_TRUE(cfg.getNetworkConfig().gross_los_emit_exclude);
    }
}

// ---------------------------------------------------------------- the injector kind
TEST(GrossLosEmitEdgesInject, ACodeStepNeedsAStationASatAStepAndAWindow) {
    {
        LogManager lm; GnssConfig cfg;
        ASSERT_TRUE(loadWith(codeStepBlock(1000.0, 20.0, 1800.0), cfg, lm, "cs_ok"));
        const auto& ev = cfg.getNetworkConfig().inject.events;
        ASSERT_EQ(ev.size(), 1u);
        EXPECT_EQ(ev[0].kind, "code_step");
        EXPECT_DOUBLE_EQ(ev[0].step_m, 20.0);
        EXPECT_DOUBLE_EQ(ev[0].duration_s, 1800.0);
    }
    {
        LogManager lm; GnssConfig cfg;
        EXPECT_FALSE(loadWith(codeStepBlock(1000.0, 20.0, 0.0), cfg, lm, "cs_nowin"));
    }
    {
        LogManager lm; GnssConfig cfg;
        EXPECT_FALSE(loadWith(codeStepBlock(1000.0, 0.0, 600.0), cfg, lm, "cs_nostep"));
    }
}

TEST(GrossLosEmitEdgesInject, ACodeStepMovesOnlyTheCodeOfOneLineOfSightInsideItsWindow) {
    Harness h(codeStepBlock(1000.0, 20.0, 60.0), "cs_apply");
    SyncedEpoch scratch;

    SyncedEpoch before = epochAt(970.0);   // before the window: untouched
    const SyncedEpoch& b = h.step(before, scratch);
    ASSERT_NE(find(b, "STA1", "G05"), nullptr);
    EXPECT_DOUBLE_EQ(find(b, "STA1", "G05")->measurements.at("C1C"), 2.0e7);

    SyncedEpoch in = epochAt(1000.0);      // inside: STA1/G05 code moves, nothing else
    const SyncedEpoch& i = h.step(in, scratch);
    const RinexObservation* o = find(i, "STA1", "G05");
    ASSERT_NE(o, nullptr);
    EXPECT_DOUBLE_EQ(o->measurements.at("C1C"), 2.0e7 + 20.0);
    EXPECT_DOUBLE_EQ(o->measurements.at("C2W"), 2.0e7 + 1.0 + 20.0);
    EXPECT_DOUBLE_EQ(o->measurements.at("L1C"), 1.0e8);
    EXPECT_DOUBLE_EQ(o->measurements.at("L2W"), 0.8e8);
    EXPECT_DOUBLE_EQ(o->measurements.at("D1C"), -1000.0);
    EXPECT_DOUBLE_EQ(find(i, "STA1", "G07")->measurements.at("C1C"), 2.0e7);
    EXPECT_DOUBLE_EQ(find(i, "STA2", "G05")->measurements.at("C1C"), 2.0e7);

    SyncedEpoch after = epochAt(1060.0);   // at the window's end: untouched again
    const SyncedEpoch& a = h.step(after, scratch);
    EXPECT_DOUBLE_EQ(find(a, "STA1", "G05")->measurements.at("C1C"), 2.0e7);
}

// ---------------------------------------------------------------- the delivery of a mark
//
// Under the key a declaration is marked at its edges only, so a mark that does not reach the
// product is not repeated at the next epoch.  The paths between the screen's decision and the
// written R record, and what closes each:
//   1. the satellite is withheld by the screen itself at its edge (too few contributors left);
//   2. a re-seed's response reaches the solve one epoch after the decision;
//   3. the whole epoch ends before the screen decides (rank gate, sigma0 ceiling);
//   4. the satellite is pruned after the screen by another emission step (the stand-down's
//      wire withhold, the clock-runaway quarantine, the per-constellation carve-out);
//   5. the satellite has no solution record at the epoch its mark is handed over;
//   6. a re-seed at a release with fewer contributors than the minimum.
// The model below composes the two real pieces: the screen's latch
// (networkGrossLosMarkLatch) and the product's mark queue drain (networkRetroDrainSplit).
namespace {

    /// One decided or skipped epoch of one satellite, as the screen and the emission see it.
    struct SatEpoch {
        bool decided{true};          ///< false: the epoch ended before the screen ran (path 3).
        bool onset{false};           ///< A line of sight of the satellite left the contributor set.
        bool reseed{false};          ///< The screen re-seeded the satellite's carried ambiguity.
        bool screen_withheld{false}; ///< Too few contributors left: withheld by the screen (path 1, 6).
        bool pruned_after{false};    ///< Pruned by a later emission step (path 4).
        bool in_solution{true};      ///< The satellite has a solution record (path 5).
    };

    /// The epochs (indices) at which an R record carries the satellite.
    std::vector<int> landings(const std::vector<SatEpoch>& eps) {
        const std::string sat = "G13";
        bool owed = false;
        std::set<std::string> queue;
        std::vector<int> out;
        for (int i = 0; i < static_cast<int>(eps.size()); ++i) {
            const SatEpoch& e = eps[static_cast<std::size_t>(i)];
            if (!e.decided) continue;   // the epoch loop returns before the screen and the drain
            const GrossLosMarkLatchStep st = networkGrossLosMarkLatch(owed, e.onset || e.reseed, e.reseed);
            if (st.queue_now) queue.insert(sat);
            owed = st.owed_after;
            std::set<std::string> present;
            if (e.in_solution && !e.screen_withheld && !e.pruned_after) present.insert(sat);
            std::set<std::string> land, pending;
            networkRetroDrainSplit(queue, present, land, pending);
            queue = pending;
            if (land.count(sat)) out.push_back(i);
        }
        return out;
    }

} // namespace

TEST(GrossLosEmitMarkDelivery, TheLatchTable) {
    for (bool owed : { false, true })
        for (bool edge : { false, true })
            for (bool reseed : { false, true }) {
                const GrossLosMarkLatchStep st = networkGrossLosMarkLatch(owed, edge, reseed);
                EXPECT_EQ(st.queue_now, owed || edge);
                EXPECT_EQ(st.late, owed);
                EXPECT_EQ(st.owed_after, reseed);
            }
}

TEST(GrossLosEmitMarkDelivery, TheDrainLandsPresentSatellitesAndKeepsTheOthers) {
    std::set<std::string> land, pending;
    networkRetroDrainSplit({ "G13", "G15" }, { "G15", "E02" }, land, pending);
    EXPECT_EQ(land, (std::set<std::string>{ "G15" }));
    EXPECT_EQ(pending, (std::set<std::string>{ "G13" }));
}

TEST(GrossLosEmitMarkDelivery, AnEmittedOnsetLandsAtItsOwnEpochOnce) {
    std::vector<SatEpoch> eps(4);
    eps[1].onset = true;
    EXPECT_EQ(landings(eps), (std::vector<int>{ 1 }));
}

TEST(GrossLosEmitMarkDelivery, Path1AnOnsetInAWithheldEpochLandsAtTheNextEmission) {
    std::vector<SatEpoch> eps(5);
    eps[1].onset = true;
    eps[1].screen_withheld = true;
    eps[2].screen_withheld = true;   // still too few contributors: standing, no new edge
    EXPECT_EQ(landings(eps), (std::vector<int>{ 3 }));
}

TEST(GrossLosEmitMarkDelivery, Path2AReseedMarksItsEpochAndItsResponseEpoch) {
    std::vector<SatEpoch> eps(4);
    eps[1].reseed = true;
    EXPECT_EQ(landings(eps), (std::vector<int>{ 1, 2 }));
}

TEST(GrossLosEmitMarkDelivery, Path2AReseedInAWithheldStretchLandsOnceAtTheFirstEmission) {
    std::vector<SatEpoch> eps(5);
    eps[1].reseed = true;
    eps[1].screen_withheld = true;
    eps[2].screen_withheld = true;
    EXPECT_EQ(landings(eps), (std::vector<int>{ 3 }));
}

TEST(GrossLosEmitMarkDelivery, Path3AWholeEpochSkipKeepsTheLatch) {
    std::vector<SatEpoch> eps(5);
    eps[1].reseed = true;
    eps[2].decided = false;
    eps[3].decided = false;
    EXPECT_EQ(landings(eps), (std::vector<int>{ 1, 4 }));
}

TEST(GrossLosEmitMarkDelivery, Path4APruneAfterTheScreenKeepsTheMarkQueued) {
    std::vector<SatEpoch> eps(4);
    eps[1].onset = true;
    eps[1].pruned_after = true;
    EXPECT_EQ(landings(eps), (std::vector<int>{ 2 }));
}

TEST(GrossLosEmitMarkDelivery, Path5ASatelliteWithoutARecordAtTheHandOverWaits) {
    std::vector<SatEpoch> eps(5);
    eps[1].reseed = true;
    eps[2].in_solution = false;
    eps[3].in_solution = false;
    EXPECT_EQ(landings(eps), (std::vector<int>{ 1, 4 }));
}

TEST(GrossLosEmitMarkDelivery, Path6AReseedWithFewContributorsIsQueuedNotDropped) {
    // A release that re-seeds while the satellite has fewer contributors than the minimum, or
    // none and no record this epoch: queued directly, landing with the response mark at the
    // first emission, once.
    std::vector<SatEpoch> eps(4);
    eps[1].reseed = true;
    eps[1].in_solution = false;
    EXPECT_EQ(landings(eps), (std::vector<int>{ 2 }));
    std::vector<SatEpoch> few(4);
    few[1].reseed = true;   // few contributors but still emitted: lands at once, then the response
    EXPECT_EQ(landings(few), (std::vector<int>{ 1, 2 }));
}
