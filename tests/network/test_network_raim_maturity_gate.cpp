/**
 * @file test_network_raim_maturity_gate.cpp
 * @brief The isolated-arc refloat maturity gate (network.raim_refloat_maturity_gate): the
 *        gate rules of the T4 attribution, and the telemetry its two edges owe (standing
 *        invariants 4 and 7 of CLAUDE.md).
 *
 * The gate holds every isolated-arc T4 refloat until the run has emitted
 * max(horizon, carry warm-up) epochs and the sigma0 EWMA has received horizon updates.
 * It used to be silent: no arming line, no count of what it held, no event at the lift, and
 * the attribution trace marked a held arc `acted`.  The suites below pin the repair.
 *
 * Suite @c RaimIsolatedArcs (raimSelectIsolatedArcs, the T4 gates as a pure function):
 *  - with both gates open every flag is refloated, in table order, and nothing is held;
 *  - a closed maturity gate holds every flag, refloats none, carries no persistence streak
 *    and never reads one;
 *  - a satellite caught wholesale is neither held nor refloated nor counted as a flag;
 *  - the persistence gate holds an arc until its K-th consecutive flag, and the arc it
 *    holds is not refloated (the `acted` column reads RaimIsolatedArcs::refloats);
 *  - the flag bar is strict and scaled by sigma0.
 *
 * Suite @c RaimMaturityTally (RaimMaturityGateTally::noteScan): the arcs and epochs held,
 * the first and last held epoch, and the lift as the first scan with the gate open.
 *
 * Suite @c RaimMaturityNote (networkRaimMaturityGateNote on a NetworkTestRun): one
 * RAIM_MATURITY arc_held row per held flag with its identities, one lift row carrying
 * n_emitted and the sigma0 EWMA update count, and no second lift row.
 *
 * Suite @c RaimMaturityRunEnd (the run-end section): the four outcomes the denominators tell
 * apart (never consulted, never lifted, held then lifted, open from the first scan) and the
 * silence of an unarmed gate.
 *
 * Suite @c RaimMaturityLoader (the arming line): ARMED on a live posture, INERT without
 * attribution, and a warning when the horizon does not exceed the carry warm-up with the sigma0 EWMA unfed.
 *
 * Not certified here: that networkPostFitRaim passes these functions the right arguments.
 * The seam is not a unit-testable one (it needs a solved network); the wiring is certified
 * by a 2 h field arm with the gate on, whose counters, events and `acted` values are read
 * against the arm of an exe without this telemetry (docs/refound/MGT_maturity_gate_telemetry_2026-10-30.md).
 *
 * The five suite names are owned by this file alone (suite-hygiene rule in CLAUDE.md).
 */

#include <gtest/gtest.h>

#include "network/qc/Raim.hpp"
#include "network/state/RunContext.hpp"
#include "network/driver/Epoch.hpp"
#include "network/driver/Run.hpp"
#include "network/driver/RunGroups.hpp"
#include "network/driver/SetupResult.hpp"
#include "network/diag/RunTraces.hpp"
#include "network/runend/RunEnd.hpp"
#include "config/GnssConfig.hpp"
#include "../support/NetworkTestRun.hpp"   // the run and epoch networkRaimMaturityGateNote takes
#include "../support/TestTempDir.hpp"      // per-process, per-test scratch directory
#include "ConfigLoadCapture.hpp"

#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;

namespace GnssProcessor {
namespace {

using TestSupport::contains;
using TestSupport::LoadedConfig;
using TestSupport::loadNetworkConfigCapturing;

using Cells = std::vector<std::tuple<int, std::string, double>>;

/// A five-cell table: three isolated flags at k = 6 and sigma0 = 1, (0, G01) at 10, (1, G02) at 9
/// and (2, G03) at 8, and two quiet cells.  The value column is unused by the selection.
struct Table {
    Cells cells{{0, "G01", 0.0}, {0, "G02", 0.0}, {1, "G01", 0.0}, {1, "G02", 0.0}, {2, "G03", 0.0}};
    std::vector<double> interaction{10.0, 0.5, 0.2, 9.0, 8.0};
    double sigma0{1.0};
    double k{6.0};
};

const std::function<int(int, const std::string&)> kNoStreak =
    [](int, const std::string&) { return 0; };

using Arc = std::pair<int, std::string>;

// ---------------------------------------------------------------------------
// The gates as a pure function
// ---------------------------------------------------------------------------

TEST(RaimIsolatedArcs, BothGatesOpenRefloatsEveryFlagInTableOrder) {
    const Table t;
    const RaimIsolatedArcs r = raimSelectIsolatedArcs(t.cells, t.interaction, t.sigma0, t.k, {},
                                                      /*mature=*/true, /*persist=*/1, kNoStreak);
    EXPECT_EQ(r.refloat, (std::vector<Arc>{{0, "G01"}, {1, "G02"}, {2, "G03"}}));
    EXPECT_TRUE(r.held.empty());
    EXPECT_TRUE(r.next_streak.empty());
    EXPECT_EQ(r.flagged, 3u);
    EXPECT_TRUE(r.refloats(0, "G01"));
    EXPECT_FALSE(r.refloats(0, "G02"));   // under the bar: not flagged, not refloated
}

TEST(RaimIsolatedArcs, ClosedMaturityGateHoldsEveryFlagAndRefloatsNone) {
    const Table t;
    const RaimIsolatedArcs r = raimSelectIsolatedArcs(t.cells, t.interaction, t.sigma0, t.k, {},
                                                      /*mature=*/false, /*persist=*/1, kNoStreak);
    EXPECT_TRUE(r.refloat.empty());
    EXPECT_EQ(r.flagged, 3u);
    ASSERT_EQ(r.held.size(), 3u);
    // The identities and the flag statistic (interaction over sigma0), in table order.
    EXPECT_EQ(r.held[0].station, 0);  EXPECT_EQ(r.held[0].sat, "G01");  EXPECT_DOUBLE_EQ(r.held[0].statistic, 10.0);
    EXPECT_EQ(r.held[1].station, 1);  EXPECT_EQ(r.held[1].sat, "G02");  EXPECT_DOUBLE_EQ(r.held[1].statistic, 9.0);
    EXPECT_EQ(r.held[2].station, 2);  EXPECT_EQ(r.held[2].sat, "G03");  EXPECT_DOUBLE_EQ(r.held[2].statistic, 8.0);
    // The acted column: a held arc is not refloated.
    EXPECT_FALSE(r.refloats(0, "G01"));
    EXPECT_FALSE(r.refloats(1, "G02"));
    EXPECT_FALSE(r.refloats(2, "G03"));
}

TEST(RaimIsolatedArcs, ClosedMaturityGateCarriesNoStreakAndReadsNone) {
    const Table t;
    int reads = 0;
    const RaimIsolatedArcs r = raimSelectIsolatedArcs(
        t.cells, t.interaction, t.sigma0, t.k, {}, /*mature=*/false, /*persist=*/3,
        [&reads](int, const std::string&) { ++reads; return 2; });
    // The held arcs restart their persistence streak: absent from the carry, never read.
    EXPECT_TRUE(r.next_streak.empty());
    EXPECT_EQ(reads, 0);
    EXPECT_TRUE(r.refloat.empty());
    EXPECT_EQ(r.held.size(), 3u);
}

TEST(RaimIsolatedArcs, SatelliteCaughtWholesaleIsNeitherHeldNorRefloatedNorCounted) {
    const Table t;
    const std::set<std::string> wholesale{"G03"};
    const RaimIsolatedArcs closed = raimSelectIsolatedArcs(
        t.cells, t.interaction, t.sigma0, t.k, wholesale, /*mature=*/false, 1, kNoStreak);
    EXPECT_EQ(closed.flagged, 2u);
    ASSERT_EQ(closed.held.size(), 2u);
    EXPECT_EQ(closed.held[0].sat, "G01");
    EXPECT_EQ(closed.held[1].sat, "G02");
    const RaimIsolatedArcs open = raimSelectIsolatedArcs(
        t.cells, t.interaction, t.sigma0, t.k, wholesale, /*mature=*/true, 1, kNoStreak);
    EXPECT_EQ(open.refloat, (std::vector<Arc>{{0, "G01"}, {1, "G02"}}));
    EXPECT_FALSE(open.refloats(2, "G03"));   // the whole-satellite refloat takes it, not this scan
}

TEST(RaimIsolatedArcs, PersistenceGateHoldsAnArcUntilItsKthFlagAndDoesNotRefloatIt) {
    const Table t;
    // First flag of every arc (prior streak 0, K = 3): all held by persistence, none by maturity.
    const RaimIsolatedArcs first = raimSelectIsolatedArcs(
        t.cells, t.interaction, t.sigma0, t.k, {}, /*mature=*/true, /*persist=*/3, kNoStreak);
    EXPECT_TRUE(first.refloat.empty());
    EXPECT_TRUE(first.held.empty());   // the maturity gate held nothing
    EXPECT_EQ(first.flagged, 3u);
    EXPECT_EQ(first.next_streak.size(), 3u);
    EXPECT_EQ(first.next_streak.at({0, "G01"}), 1);
    // The persistence gate has the defect the maturity gate had: an arc it holds is not acted.
    EXPECT_FALSE(first.refloats(0, "G01"));

    // (0, G01) has two prior consecutive flags: its third refloats and its streak is dropped.
    const RaimIsolatedArcs third = raimSelectIsolatedArcs(
        t.cells, t.interaction, t.sigma0, t.k, {}, /*mature=*/true, /*persist=*/3,
        [](int sta, const std::string& sat) { return (sta == 0 && sat == "G01") ? 2 : 0; });
    EXPECT_EQ(third.refloat, (std::vector<Arc>{{0, "G01"}}));
    EXPECT_TRUE(third.refloats(0, "G01"));
    EXPECT_FALSE(third.refloats(1, "G02"));
    EXPECT_EQ(third.next_streak.count({0, "G01"}), 0u);
    EXPECT_EQ(third.next_streak.size(), 2u);
}

TEST(RaimIsolatedArcs, BarIsStrictAndScaledBySigma0) {
    Table t;
    t.interaction = {6.0, 6.0001, 12.0, 11.9, 0.0};
    t.sigma0 = 1.0;
    const RaimIsolatedArcs at_bar = raimSelectIsolatedArcs(t.cells, t.interaction, t.sigma0, t.k, {},
                                                           true, 1, kNoStreak);
    // 6.0 is not above the bar of 6; 6.0001, 12 and 11.9 are.
    EXPECT_EQ(at_bar.flagged, 3u);
    EXPECT_FALSE(at_bar.refloats(0, "G01"));
    EXPECT_TRUE(at_bar.refloats(0, "G02"));
    // The same table at sigma0 = 2: only the interaction above 12 stands, and none does.
    const RaimIsolatedArcs scaled = raimSelectIsolatedArcs(t.cells, t.interaction, 2.0, t.k, {},
                                                           true, 1, kNoStreak);
    EXPECT_EQ(scaled.flagged, 0u);
    EXPECT_TRUE(scaled.refloat.empty());
}

// ---------------------------------------------------------------------------
// The tally
// ---------------------------------------------------------------------------

RaimScanStamp stampAt(long long epoch, long long n_emitted, long long ewma, double tow = 0.0) {
    RaimScanStamp s;
    s.epoch = epoch;  s.week = 2313;  s.tow = tow;  s.n_emitted = n_emitted;  s.ewma_updates = ewma;
    return s;
}

TEST(RaimMaturityTally, CountsTheArcsAndEpochsHeldWhileClosed) {
    RaimMaturityGateTally g;
    EXPECT_FALSE(g.noteScan(2, 2, false, stampAt(5, 5, 5)));
    EXPECT_FALSE(g.noteScan(0, 0, false, stampAt(6, 6, 6)));   // a scan with no flag
    EXPECT_FALSE(g.noteScan(1, 1, false, stampAt(9, 9, 9)));
    EXPECT_EQ(g.scans, 3);
    EXPECT_EQ(g.flags, 3);
    EXPECT_EQ(g.held, 3);           // arcs
    EXPECT_EQ(g.held_epochs, 2);    // epochs
    EXPECT_EQ(g.first_scan_epoch, 5);
    EXPECT_EQ(g.first_held_epoch, 5);
    EXPECT_EQ(g.last_held_epoch, 9);
    EXPECT_FALSE(g.lifted);
}

TEST(RaimMaturityTally, LiftIsTheFirstScanWithTheGateOpenAndHappensOnce) {
    RaimMaturityGateTally g;
    g.noteScan(2, 2, false, stampAt(60, 125, 119));
    EXPECT_TRUE(g.noteScan(1, 0, true, stampAt(61, 126, 120, 266580.0)));
    EXPECT_TRUE(g.lifted);
    EXPECT_EQ(g.lift.epoch, 61);
    EXPECT_EQ(g.lift.n_emitted, 126);
    EXPECT_EQ(g.lift.ewma_updates, 120);
    EXPECT_DOUBLE_EQ(g.lift.tow, 266580.0);
    // A later open scan is not a second lift and leaves the stamp alone; its flags count as
    // passed (flags - held), not as held.
    EXPECT_FALSE(g.noteScan(3, 0, true, stampAt(62, 127, 121)));
    EXPECT_EQ(g.lift.epoch, 61);
    EXPECT_EQ(g.flags, 6);
    EXPECT_EQ(g.held, 2);
    EXPECT_EQ(g.last_held_epoch, 60);
}

TEST(RaimMaturityTally, OpenFromTheFirstScanLiftsWithNothingHeld) {
    RaimMaturityGateTally g;
    EXPECT_TRUE(g.noteScan(4, 0, true, stampAt(7, 7, 7)));
    EXPECT_EQ(g.held, 0);
    EXPECT_EQ(g.held_epochs, 0);
    EXPECT_EQ(g.first_held_epoch, -1);
    EXPECT_EQ(g.lift.epoch, g.first_scan_epoch);
}

// ---------------------------------------------------------------------------
// The events and the lift line, on a run
// ---------------------------------------------------------------------------

/// The network-block lines that arm the gate on a posture where it acts: attribution drives
/// the action and the sigma0 EWMA is fed.
std::string gateKnobs(int horizon, const std::string& extra = "") {
    return std::string(
               "    \"phase_leveling\": true,\n"
               "    \"raim\": true,\n"
               "    \"raim_attribution\": true,\n"
               "    \"emit_ura\": true,\n"
               "    \"trace_events\": true,\n"
               "    \"raim_refloat_maturity_gate\": true,\n"
               "    \"raim_refloat_maturity_epochs\": ")
           + std::to_string(horizon) + ",\n" + extra;
}

/// A run with the gate armed, a capturing logger and the events trace open.
struct GateRun {
    LoadedConfig                    loaded;
    std::ostringstream              log_text;
    std::shared_ptr<spdlog::logger> log;
    fs::path                        trace_dir;
    std::unique_ptr<NetworkTestRun> fx;

    explicit GateRun(const std::string& tag, const std::string& knobs = gateKnobs(120))
        : loaded(loadNetworkConfigCapturing("mg_" + tag, knobs)) {
        EXPECT_TRUE(loaded.ok) << loaded.text;
        auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(log_text);
        log = std::make_shared<spdlog::logger>("raim_maturity_" + tag, sink);
        log->set_level(spdlog::level::info);
        trace_dir = gnss_test::tempDir() / ("raim_maturity_" + tag);
        fs::create_directories(trace_dir);
        fx = std::make_unique<NetworkTestRun>(loaded.config, log, trace_dir);
        fx->traces().openAll(loaded.config, loaded.config.getNetworkConfig(),
                             loaded.config.getGlobalVtecShConfig(), /*sh_model=*/nullptr,
                             /*cl_trace_armed=*/false, log);
        fx->setup().stations.emplace_back();
        fx->setup().stations.back().name = "AAAA";
        fx->setup().stations.emplace_back();
        fx->setup().stations.back().name = "BBBB";
    }
    ~GateRun() {
        fx->traces().closeAll();
        std::error_code ec;
        fs::remove_all(trace_dir, ec);
    }

    /// One scan at the given point of the run.
    void scan(const RaimIsolatedArcs& isolated, bool mature, double tow, long long epoch,
              long long n_emitted, long long ewma_updates, double sigma0 = 1.5) {
        fx->ctx().counters.n_loop_epochs = static_cast<std::size_t>(epoch);
        fx->ctx().counters.n_emitted = n_emitted;
        fx->ctx().carry.ura_ewma_updates = static_cast<std::size_t>(ewma_updates);
        NetworkEpoch& e = fx->newEpoch(GnssTime{2313, tow});
        networkRaimMaturityGateNote(fx->run(), e, isolated, mature, sigma0);
    }

    /// Flush and close the traces, return the events CSV body.
    std::string eventsCsv() {
        fx->traces().closeAll();
        std::ifstream f(trace_dir / "network_events.csv");
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    std::string logged() { log->flush(); return log_text.str(); }
};

std::size_t countOf(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) ++n;
    return n;
}

RaimIsolatedArcs heldTwo() {
    RaimIsolatedArcs r;
    r.flagged = 2;
    r.held = {{0, "G05", 7.5}, {1, "E11", 6.25}};
    return r;
}

TEST(RaimMaturityNote, EveryHeldFlagIsAnEventRowWithItsIdentity) {
    GateRun g("held");
    g.scan(heldTwo(), /*mature=*/false, 259200.0, 40, 40, 34);
    const std::string csv = g.eventsCsv();
    EXPECT_EQ(countOf(csv, ",RAIM_MATURITY,arc_held,"), 2u) << csv;
    // The columns: sat, station, statistic (interaction over sigma0), threshold (the bar),
    // action, then the point of the run the gate stood at.
    EXPECT_TRUE(contains(csv, "2313,259200.000,RAIM_MATURITY,arc_held,G05,AAAA,7.500000,6.000000,hold,"
                              "sigma0=1.50;n_emitted=40;need_epochs=120;ewma_updates=34;ewma_need=120"))
        << csv;
    EXPECT_TRUE(contains(csv, "2313,259200.000,RAIM_MATURITY,arc_held,E11,BBBB,6.250000,6.000000,hold,"))
        << csv;
    EXPECT_FALSE(contains(csv, "RAIM_MATURITY,lift")) << csv;
    // The tally: two arcs on one epoch, not lifted.
    const RaimMaturityGateTally& t = g.fx->ctx().counters.maturity_gate;
    EXPECT_EQ(t.held, 2);
    EXPECT_EQ(t.held_epochs, 1);
    EXPECT_FALSE(t.lifted);
    // No refloat event: the held flags are not T4_attrib rows, so a grader that counts the
    // refloats by tier is not shifted by the gate.
    EXPECT_FALSE(contains(csv, "T4_attrib")) << csv;
}

TEST(RaimMaturityNote, TheLiftIsOneRowCarryingNEmittedAndTheEwmaUpdateCount) {
    GateRun g("lift");
    g.scan(heldTwo(), false, 259200.0, 40, 40, 34);
    RaimIsolatedArcs open;
    open.flagged = 1;
    open.refloat = {{0, "G05"}};
    g.scan(open, /*mature=*/true, 262980.0, 126, 126, 120);
    g.scan(open, /*mature=*/true, 263010.0, 127, 127, 121);   // a later open scan: no second lift
    const std::string csv = g.eventsCsv();
    EXPECT_EQ(countOf(csv, ",RAIM_MATURITY,lift,"), 1u) << csv;
    EXPECT_TRUE(contains(csv, "2313,262980.000,RAIM_MATURITY,lift,,,126.000000,120.000000,lift,"
                              "n_emitted=126;need_epochs=120;ewma_updates=120;ewma_need=120;"
                              "held=2;held_epochs=1"))
        << csv;
    const RaimMaturityGateTally& t = g.fx->ctx().counters.maturity_gate;
    EXPECT_TRUE(t.lifted);
    EXPECT_EQ(t.lift.n_emitted, 126);
    EXPECT_EQ(t.lift.ewma_updates, 120);
    EXPECT_EQ(t.flags, 4);
    EXPECT_EQ(t.held, 2);
    const std::string text = g.logged();
    EXPECT_EQ(countOf(text, "LIFTED at TOW"), 1u) << text;
    EXPECT_TRUE(contains(text, "126 epoch(s) emitted (need 120), sigma0 EWMA 120 update(s) (need 120); "
                               "2 isolated-arc flag(s) were held on 1 epoch(s) before it"))
        << text;
}

TEST(RaimMaturityNote, TheNeedEpochsIsTheLargerOfTheHorizonAndTheCarryWarmup) {
    // Horizon 3 under the default carry warm-up of 6: the gate needs 6 emitted epochs.
    GateRun g("warm", gateKnobs(3));
    g.scan(heldTwo(), false, 259200.0, 4, 4, 4);
    EXPECT_TRUE(contains(g.eventsCsv(), "need_epochs=6;"));
}

// ---------------------------------------------------------------------------
// The run end
// ---------------------------------------------------------------------------

/// The RAIM subsystem's run-end report over the run, with its lines captured.
std::string runEndText(GateRun& g) {
    NetworkRun& run = g.fx->run();
    const NetworkRunEndInputs inputs{run.engines, run.emit, run.timing, run.closed_loop};
    const NetworkRunEndView view{g.fx->ctx(), g.fx->config(), g.log, g.fx->traces(),
                                 g.fx->setup(), inputs, /*loop_ms=*/0.0};
    EXPECT_EQ(networkRaimRunEnd(view), 0);
    return g.logged();
}

TEST(RaimMaturityRunEnd, AGateNeverConsultedSaysSoAndNamesWhy) {
    GateRun g("re_never");
    const std::string text = runEndText(g);
    EXPECT_TRUE(contains(text, "ARMED but NEVER CONSULTED")) << text;
    EXPECT_TRUE(contains(text, "post-fit RAIM never ran")) << text;
}

TEST(RaimMaturityRunEnd, AGateThatNeverLiftedSaysEveryFlagOfTheRunWasHeld) {
    GateRun g("re_held");
    g.scan(heldTwo(), false, 259200.0, 40, 40, 34);
    g.scan(heldTwo(), false, 259230.0, 41, 41, 35);
    const std::string text = runEndText(g);
    EXPECT_TRUE(contains(text, "4 flag(s) reached the gate: 4 HELD while it was closed "
                               "(2 epoch(s), processed epochs 40 to 41), 0 passed"))
        << text;
    EXPECT_TRUE(contains(text, "NEVER LIFTED")) << text;
    EXPECT_TRUE(contains(text, "every isolated-arc flag of the run (4) was held")) << text;
}

TEST(RaimMaturityRunEnd, AGateThatHeldThenLiftedReportsBothEdges) {
    GateRun g("re_lift");
    g.scan(heldTwo(), false, 259200.0, 40, 40, 34);
    RaimIsolatedArcs open;
    open.flagged = 3;
    g.scan(open, true, 266580.0, 126, 126, 120);
    const std::string text = runEndText(g);
    EXPECT_TRUE(contains(text, "the gate needs 120 emitted epochs and 120 sigma0 EWMA updates")) << text;
    EXPECT_TRUE(contains(text, "5 flag(s) reached the gate: 2 HELD while it was closed "
                               "(1 epoch(s), processed epochs 40 to 40), 3 passed"))
        << text;
    // TOW 266580 is 02:03:00 of the third day of the week.
    EXPECT_TRUE(contains(text, "lifted at GPS week 2313 TOW 266580.0 (02:03:00 of the day), "
                               "processed epoch 126 (the first scan was epoch 40)"))
        << text;
    EXPECT_FALSE(contains(text, "NEVER")) << text;
    EXPECT_FALSE(contains(text, "held NOTHING")) << text;
}

TEST(RaimMaturityRunEnd, AGateOpenFromItsFirstScanSaysItHeldNothing) {
    GateRun g("re_open");
    RaimIsolatedArcs open;
    open.flagged = 2;
    g.scan(open, true, 259200.0, 7, 7, 7);
    const std::string text = runEndText(g);
    EXPECT_TRUE(contains(text, "held NOTHING")) << text;
}

TEST(RaimMaturityRunEnd, AnUnarmedGatePrintsNothing) {
    GateRun g("re_off", "    \"phase_leveling\": true,\n    \"raim\": true,\n"
                        "    \"raim_attribution\": true,\n");
    EXPECT_FALSE(g.fx->ctx().knobs.raim_maturity_gate);
    EXPECT_FALSE(contains(runEndText(g), "maturity gate"));
}

// ---------------------------------------------------------------------------
// The arming line
// ---------------------------------------------------------------------------

TEST(RaimMaturityLoader, ALivePostureLogsTheArmedLineWithTheRuleItAppliesAtRunTime) {
    const LoadedConfig r = loadNetworkConfigCapturing("mgl_armed", gateKnobs(120));
    ASSERT_TRUE(r.ok) << r.text;
    EXPECT_TRUE(contains(r.text, "Network (RAIM refloat maturity gate): ARMED, horizon 120 epochs"))
        << r.text;
    EXPECT_TRUE(contains(r.text, "until the run has emitted 120 epochs (the larger of the horizon "
                                 "and the carry warm-up of 6) and the sigma0 EWMA has received 120 updates"))
        << r.text;
    EXPECT_TRUE(contains(r.text, "RAIM_MATURITY arc_held")) << r.text;
    EXPECT_FALSE(contains(r.text, "(INERT; it holds nothing")) << r.text;
    EXPECT_FALSE(contains(r.text, "does not exceed the carry warm-up")) << r.text;
}

TEST(RaimMaturityLoader, WithoutAttributionTheGateIsReportedInert) {
    // network.raim on, network.raim_attribution off: the isolated-arc path never runs.
    const LoadedConfig r = loadNetworkConfigCapturing("mgl_noattr",
        "    \"phase_leveling\": true,\n"
        "    \"raim\": true,\n"
        "    \"raim_refloat_maturity_gate\": true,\n"
        "    \"raim_refloat_maturity_epochs\": 120,\n");
    ASSERT_TRUE(r.ok) << r.text;
    EXPECT_TRUE(contains(r.text, "network.raim_refloat_maturity_gate is on but network.raim_attribution is OFF"))
        << r.text;
    EXPECT_TRUE(contains(r.text, "(INERT; it holds nothing")) << r.text;
    EXPECT_FALSE(contains(r.text, "ARMED, horizon")) << r.text;
}

TEST(RaimMaturityLoader, AHorizonWithinTheCarryWarmupWarnsOnlyWhenTheEwmaIsNotFed) {
    // The sigma0 EWMA unfed (no emit_ura): the epoch count alone decides, and a horizon within
    // the carry warm-up is open at the first post-fit pass, so the gate holds nothing.
    const LoadedConfig unfed = loadNetworkConfigCapturing("mgl_short",
        "    \"phase_leveling\": true,\n"
        "    \"raim\": true,\n"
        "    \"raim_attribution\": true,\n"
        "    \"raim_refloat_maturity_gate\": true,\n"
        "    \"raim_refloat_maturity_epochs\": 6,\n");
    ASSERT_TRUE(unfed.ok) << unfed.text;
    EXPECT_TRUE(contains(unfed.text, "ARMED, horizon 6 epochs")) << unfed.text;
    EXPECT_TRUE(contains(unfed.text, "the sigma0 EWMA is not fed")) << unfed.text;
    EXPECT_TRUE(contains(unfed.text, "the horizon (6) does not exceed the carry warm-up (6 epochs) "
                                     "and the sigma0 EWMA is not fed"))
        << unfed.text;
    // The EWMA fed: its update count binds (it starts after the carry warm-up), so the gate
    // holds for about `horizon` passes and no warning is owed.
    const LoadedConfig fed = loadNetworkConfigCapturing("mgl_short_fed", gateKnobs(6));
    ASSERT_TRUE(fed.ok) << fed.text;
    EXPECT_TRUE(contains(fed.text, "ARMED, horizon 6 epochs")) << fed.text;
    EXPECT_FALSE(contains(fed.text, "does not exceed the carry warm-up")) << fed.text;
}

TEST(RaimMaturityLoader, AnUnarmedGateLogsNothing) {
    const LoadedConfig r = loadNetworkConfigCapturing("mgl_off",
        "    \"phase_leveling\": true,\n    \"raim\": true,\n    \"raim_attribution\": true,\n");
    ASSERT_TRUE(r.ok) << r.text;
    EXPECT_FALSE(contains(r.text, "maturity gate")) << r.text;
}

}  // namespace
}  // namespace GnssProcessor
