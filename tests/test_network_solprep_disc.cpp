/**
 * @file test_network_solprep_disc.cpp
 * @brief The emit-site landings of per-satellite discontinuities in
 *        networkSolPrep: the Q3 trap-1 rule (docs/SPEC_clock_temporal_anchor.md)
 *        applied to the sibling landing sites eph_cutover, cb_excursion, the IF-pin
 *        re-selection (T3c), and the CB re-seed queue (Card-2).
 *
 * The contract under test: a queue's landed mark does not stop at
 * markSatDiscontinuity.  That call bumps only the emitter's in-process
 * counters; on a lineage without network.serialize_disc_counters the R record
 * is the only carrier a NetworkProductIO replay can read.  A mark that is not
 * folded into retro_emit_sats is visible live but not on replay: the live
 * consumer resets while a replay of the same product integrates straight
 * across the step, and the two graders disagree about the same wire.  Each
 * case therefore checks the full chain per queue:
 *
 *   pending queue → present/pending split → retro_emit_sats (the fold under
 *   test) → NetworkProductWriter R record → NetworkProductReader::replay →
 *   the same per-sat discontinuity counters as the live emitter.
 *
 * Also pinned:
 *  - a satellite landed by more than one queue in the same epoch steps its
 *    counters once (the Q3 "one event, one step" dedupe, chained across all
 *    four queues);
 *  - a queued satellite absent from this epoch's emission stays pending and
 *    is neither marked nor serialized (mark == land, RT0-5 P0-1);
 *  - each queue writes its own trace_events row when it lands.
 */

#include <gtest/gtest.h>

#include "app/NetworkRunContext.hpp"
#include "app/NetworkSetupResult.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkSbasis.hpp"
#include "filter/NetworkSolPrep.hpp"
#include "logging/LogManager.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "support/NetworkTestRun.hpp"   // the run and epoch networkSolPrep's epoch form takes
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkProductIO.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

fs::path writeTempConfig(const std::string& tag) {
    const std::string body =
        "{\n"
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"estimation\": {\n"
        "    \"estimate_ifb\": true\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"AAAA\",\n"
        "    \"trace_events\": true,\n"
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
    const fs::path p = fs::temp_directory_path() / ("solprep_disc_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    return p;
}

std::shared_ptr<spdlog::logger> nullLogger() {
    static auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    return std::make_shared<spdlog::logger>("solprep_disc_test", sink);
}

/// Everything one networkSolPrep call needs, with a four-satellite clock-only
/// shared estimate (G01..G04) so the present/pending split sees real sats.
/// Traces are held by unique_ptr so a test can destroy them (flush + close)
/// before grepping the events CSV.
struct Harness {
    LogManager   logman;
    GnssConfig   cfg;
    fs::path     cfg_path;
    fs::path     trace_dir;
    std::unique_ptr<NetworkTestRun> fx;   ///< The run networkSolPrep's epoch form takes.
    NetworkRunContext*  ctx;              ///< fx->ctx()
    NetworkRunTraces*   traces;           ///< fx->traces(), opened in trace_dir
    NetworkSetupResult& setup;            ///< fx->setup()
    NetworkEstimator&   est;              ///< fx->est()
    Eigen::VectorXd    x_s;
    NetworkSbasis::Plan plan;
    SyncedEpoch        se;

    NetworkSolutionEpoch warm_sol;   ///< The unmarked epoch emitted before the marked one.

    std::unique_ptr<NetworkTestRun> makeRun(const std::string& tag) {
        cfg_path = writeTempConfig(tag);
        EXPECT_TRUE(cfg.loadFromJson(cfg_path, logman));
        trace_dir = fs::temp_directory_path() / ("solprep_disc_traces_" + tag);
        fs::create_directories(trace_dir);
        return std::make_unique<NetworkTestRun>(cfg, nullLogger(), trace_dir);
    }

    explicit Harness(const std::string& tag)
        : fx(makeRun(tag)), ctx(&fx->ctx()), traces(&fx->traces()), setup(fx->setup()),
          est(fx->est()) {
        traces->openAll(cfg, cfg.getNetworkConfig(), cfg.getGlobalVtecShConfig(),
                        /*sh_model=*/nullptr, /*cl_trace_armed=*/false, nullLogger());
        setup.emitter.emplace(0.05, /*emit_clock_disc=*/true);
        for (const char* s : {"G01", "G02", "G03", "G04"}) est.registerClock(s);
        x_s = Eigen::VectorXd::Zero(est.numShared());
        se.time = GnssTime{2313, 259200.0};
        // The clock disc counter has edge semantics: a first sighting only records
        // (NetworkProductEmitter's seen_before gate), so a mark can step only a
        // satellite with emitted history.  One unmarked warm-up epoch, as every
        // real lineage has, is emitted before the marked epoch under test.
        warm_sol = est.toSolutionEpoch(x_s, GnssTime{2313, 259170.0}, /*iod_ssr=*/0);
        setup.emitter->emitToStore(warm_sol, setup.network_store);
    }

    ~Harness() {
        std::error_code ec;
        fs::remove(cfg_path, ec);
        traces->closeAll();
        fs::remove_all(trace_dir, ec);
    }

    NetworkSolPrepResult run() {
        // The solve's hand-off as the solve glue leaves it on a plain epoch: the datum
        // unchanged, no sigma0 adaptation, no per-constellation publication (the defaults).
        NetworkEpoch& e = fx->newEpoch(se.time);
        e.solve.x_s = x_s;
        e.solve.plan = plan;
        e.clock.pl_now_s = 1000.0;
        return networkSolPrep(fx->run(), e);
    }

    /// Destroy the traces unit (flush + close) and return the events CSV body.
    std::string eventsCsv() {
        traces->closeAll();
        std::ifstream f(trace_dir / "network_events.csv");
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }
};

/// Write the warm-up epoch + `sol` with its R-record set the way SB_EMITSTORE
/// does, read the file back, replay every record through a fresh emitter, and
/// return that emitter's clock-disc counter for `sat` (-1 on any structural
/// failure).  This is the replay grader's entire view of the mark: if the fold
/// did not put `sat` into retro_emit_sats, this comes back 0 no matter what
/// the live counters say.
int replayedClockDisc(const Harness& h, const NetworkSolPrepResult& r,
                      const std::string& sat, const std::string& tag) {
    const fs::path path = fs::temp_directory_path() / ("solprep_disc_prod_" + tag + ".txt");
    fs::remove(path);
    {
        NetworkProductWriter w(path.string());
        if (!w.good()) return -1;
        w.write(h.warm_sol);
        w.write(r.sol, r.global_bump, r.serialized_consts, r.retro_emit_sats);
    }
    bool ok = false;
    const std::vector<NetworkProductRecord> recs =
        NetworkProductReader::read(path.string(), &ok);
    fs::remove(path);
    if (!ok || recs.size() != 2u) return -1;
    SsrCorrectionStore    store;
    NetworkProductEmitter emit(0.05, /*emit_clock_disc=*/true);
    for (const NetworkProductRecord& rec : recs)
        NetworkProductReader::replay(rec, store, emit);
    return emit.clockDiscontinuityCounter(sat);
}

}  // namespace

// ---------------------------------------------------------------------------
// Per-queue: mark → retro_emit_sats → R record → replayed counters == live.
// ---------------------------------------------------------------------------

TEST(NetworkSolPrepDiscTest, EphCutoverMarkLandsInRRecordAndReplays) {
    Harness h("eph");
    net::assignSatFlags(h.ctx->entities, net::Disc::EphCutover, {"G02"});

    const NetworkSolPrepResult r = h.run();

    EXPECT_EQ(r.retro_emit_sats.count("G02"), 1u);        // the fold under test
    EXPECT_TRUE((net::present(h.ctx->entities.sats, net::Disc::EphCutover) == 0));

    h.setup.emitter->emitToStore(r.sol, h.setup.network_store);
    EXPECT_EQ(h.setup.emitter->clockDiscontinuityCounter("G02"), 1);   // live
    EXPECT_EQ(h.setup.emitter->clockDiscontinuityCounter("G01"), 0);
    EXPECT_EQ(replayedClockDisc(h, r, "G02", "eph"), 1);               // replay == live
    EXPECT_EQ(h.eventsCsv().find("eph_cutover_disc_landed") == std::string::npos, false);
}

TEST(NetworkSolPrepDiscTest, CbExcursionMarkLandsInRRecordAndReplays) {
    Harness h("cbexc");
    net::assignSatFlags(h.ctx->entities, net::Disc::CbExcursion, {"G03"});

    const NetworkSolPrepResult r = h.run();

    EXPECT_EQ(r.retro_emit_sats.count("G03"), 1u);
    EXPECT_TRUE((net::present(h.ctx->entities.sats, net::Disc::CbExcursion) == 0));
    EXPECT_EQ(h.ctx->carry.cb_exc_bumps, 1);              // run-end telemetry intact

    h.setup.emitter->emitToStore(r.sol, h.setup.network_store);
    EXPECT_EQ(h.setup.emitter->clockDiscontinuityCounter("G03"), 1);
    EXPECT_EQ(replayedClockDisc(h, r, "G03", "cbexc"), 1);
    EXPECT_EQ(h.eventsCsv().find("cb_excursion_disc_landed") == std::string::npos, false);
}

TEST(NetworkSolPrepDiscTest, IfPinReselectMarkLandsInRRecordAndReplays) {
    Harness h("reselect");
    // The emitted pair differs from the last emitted product's pair → a genuine
    // re-selection: the emitted δtˢ↔dˢ split re-references, and the clock disc
    // counter (datum-change components only, no magnitude jump test) is
    // signalled by this mark alone, so the mark has to reach the R record.
    net::putSat(h.ctx->entities, "G02", &net::SatelliteState::last_if_pin_sel) = {"C1W", "C2W"};
    h.ctx->carry.this_epoch_if_pin_sel["G02"] = {"C1W", "C5Q"};

    const NetworkSolPrepResult r = h.run();

    EXPECT_EQ(r.retro_emit_sats.count("G02"), 1u);
    // The emit baseline advanced with the bump it pairs with.
    EXPECT_EQ(*net::getSat(h.ctx->entities.sats, "G02", &net::SatelliteState::last_if_pin_sel),
              (std::pair<std::string, std::string>{"C1W", "C5Q"}));

    h.setup.emitter->emitToStore(r.sol, h.setup.network_store);
    EXPECT_EQ(h.setup.emitter->clockDiscontinuityCounter("G02"), 1);
    EXPECT_EQ(replayedClockDisc(h, r, "G02", "reselect"), 1);
    EXPECT_EQ(h.eventsCsv().find("reselect") == std::string::npos, false);
}

TEST(NetworkSolPrepDiscTest, CbReseedMarkLandsInRRecordAndReplays) {
    Harness h("reseed");
    net::assignSatFlags(h.ctx->entities, net::Disc::CbReseed, {"G03"});

    const NetworkSolPrepResult r = h.run();

    EXPECT_EQ(r.retro_emit_sats.count("G03"), 1u);
    EXPECT_TRUE((net::present(h.ctx->entities.sats, net::Disc::CbReseed) == 0));

    h.setup.emitter->emitToStore(r.sol, h.setup.network_store);
    EXPECT_EQ(h.setup.emitter->clockDiscontinuityCounter("G03"), 1);
    EXPECT_EQ(replayedClockDisc(h, r, "G03", "reseed"), 1);
    EXPECT_EQ(h.eventsCsv().find("reseed_disc_landed") == std::string::npos, false);
}

// ---------------------------------------------------------------------------
// Cross-queue dedupe: one event, one step, even when several queues land the
// same satellite in the same epoch (the Q3 rule, chained across the blocks).
// ---------------------------------------------------------------------------

TEST(NetworkSolPrepDiscTest, SameSatInFourQueuesStepsCountersOnce) {
    Harness h("dedupe");
    net::assignSatFlags(h.ctx->entities, net::Disc::EphCutover, {"G01"});
    net::assignSatFlags(h.ctx->entities, net::Disc::CbExcursion, {"G01"});
    net::putSat(h.ctx->entities, "G01", &net::SatelliteState::last_if_pin_sel) = {"C1W", "C2W"};
    h.ctx->carry.this_epoch_if_pin_sel["G01"] = {"C1W", "C5Q"};
    net::assignSatFlags(h.ctx->entities, net::Disc::CbReseed, {"G01"});

    const NetworkSolPrepResult r = h.run();

    EXPECT_EQ(r.retro_emit_sats.count("G01"), 1u);

    h.setup.emitter->emitToStore(r.sol, h.setup.network_store);
    EXPECT_EQ(h.setup.emitter->clockDiscontinuityCounter("G01"), 1);   // once, not 4×
    EXPECT_EQ(replayedClockDisc(h, r, "G01", "dedupe"), 1);            // one R entry, one step

    // Provenance is kept: each queue still writes its own trace row.
    const std::string csv = h.eventsCsv();
    EXPECT_NE(csv.find("eph_cutover_disc_landed"), std::string::npos);
    EXPECT_NE(csv.find("cb_excursion_disc_landed"), std::string::npos);
    EXPECT_NE(csv.find("reselect"), std::string::npos);
    EXPECT_NE(csv.find("reseed_disc_landed"), std::string::npos);
}

// ---------------------------------------------------------------------------
// mark == land: a queued satellite absent from this emission stays pending and
// appears in no R record (the emitter would clear a mark that never stepped,
// and the R record would overstate a bump the product never carried).
// ---------------------------------------------------------------------------

TEST(NetworkSolPrepDiscTest, AbsentSatStaysPendingAndUnserialized) {
    Harness h("absent");
    net::assignSatFlags(h.ctx->entities, net::Disc::EphCutover, {"G09"});
    net::assignSatFlags(h.ctx->entities, net::Disc::CbExcursion, {"G09"});
    net::assignSatFlags(h.ctx->entities, net::Disc::CbReseed, {"G09"});

    const NetworkSolPrepResult r = h.run();

    EXPECT_TRUE(r.retro_emit_sats.empty());
    EXPECT_EQ(static_cast<std::size_t>(net::hasSat(h.ctx->entities.sats, "G09", net::Disc::EphCutover)), 1u);
    EXPECT_EQ(static_cast<std::size_t>(net::hasSat(h.ctx->entities.sats, "G09", net::Disc::CbExcursion)), 1u);
    EXPECT_EQ(static_cast<std::size_t>(net::hasSat(h.ctx->entities.sats, "G09", net::Disc::CbReseed)), 1u);
    EXPECT_EQ(h.ctx->carry.cb_exc_bumps, 0);

    h.setup.emitter->emitToStore(r.sol, h.setup.network_store);
    EXPECT_EQ(h.setup.emitter->clockDiscontinuityCounter("G09"), 0);
    EXPECT_EQ(replayedClockDisc(h, r, "G09", "absent"), 0);
}
