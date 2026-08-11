/**
 * @file test_network_solprep_disc.cpp
 * @brief The emit-site per-satellite discontinuity LANDINGS in networkSolPrep —
 *        the Q3 trap-1 rule (docs/SPEC_clock_temporal_anchor.md) applied to the
 *        sibling landing sites: eph_cutover, cb_excursion, basis_relatch, the
 *        T3c hold-OFF IF-pin re-selection, and the Card-2 CB re-seed queue.
 *
 * The contract under test: a queue's landed mark must not stop at
 * markSatDiscontinuity.  That call bumps only the emitter's IN-PROCESS
 * counters; on a lineage without network.serialize_disc_counters the R record
 * is the ONLY carrier a NetworkProductIO replay can read, so a mark that is
 * not folded into retro_emit_sats is live-visible but replay-INVISIBLE — the
 * live consumer resets while a replay of the same product integrates straight
 * across the step, and the two graders disagree about the same wire.  Each
 * case therefore proves the full chain per queue:
 *
 *   pending queue → present/pending split → retro_emit_sats (the fold under
 *   test) → NetworkProductWriter R record → NetworkProductReader::replay →
 *   the SAME per-sat discontinuity counters as the live emitter.
 *
 * Also pinned:
 *  - a satellite landed by MORE than one queue in the same epoch steps its
 *    counters ONCE (the Q3 "one event, one step" dedupe, now chained across
 *    all four queue blocks);
 *  - a queued satellite ABSENT from this epoch's emission stays PENDING and
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
    std::unique_ptr<NetworkRunContext> ctx;
    std::unique_ptr<NetworkRunTraces>  traces;
    NetworkSetupResult setup;
    NetworkEstimator   est;
    Eigen::VectorXd    x_s;
    NetworkSbasis::Plan plan;
    SyncedEpoch        se;

    NetworkSolutionEpoch warm_sol;   ///< The unmarked epoch emitted before the marked one.

    explicit Harness(const std::string& tag) {
        cfg_path = writeTempConfig(tag);
        EXPECT_TRUE(cfg.loadFromJson(cfg_path, logman));
        ctx = std::make_unique<NetworkRunContext>(cfg, cfg.getNetworkConfig());
        trace_dir = fs::temp_directory_path() / ("solprep_disc_traces_" + tag);
        fs::create_directories(trace_dir);
        traces = std::make_unique<NetworkRunTraces>(trace_dir);
        traces->openAll(cfg, cfg.getNetworkConfig(), cfg.getGlobalVtecShConfig(),
                        /*sh_model=*/nullptr, /*cl_trace_armed=*/false, nullLogger());
        setup.emitter.emplace(0.05, /*emit_clock_disc=*/true);
        for (const char* s : {"G01", "G02", "G03", "G04"}) est.registerClock(s);
        x_s = Eigen::VectorXd::Zero(est.numShared());
        se.time = GnssTime{2313, 259200.0};
        // The clock disc counter is edge-semantics: first sighting records only
        // (NetworkProductEmitter's seen_before gate), so a mark can only step a
        // satellite with emitted HISTORY.  Emit one unmarked warm-up epoch — as
        // every real lineage has — before the marked epoch under test.
        warm_sol = est.toSolutionEpoch(x_s, GnssTime{2313, 259170.0}, /*iod_ssr=*/0);
        setup.emitter->emitToStore(warm_sol, setup.network_store);
    }

    ~Harness() {
        std::error_code ec;
        fs::remove(cfg_path, ec);
        traces.reset();
        fs::remove_all(trace_dir, ec);
    }

    NetworkSolPrepResult run() {
        return networkSolPrep(*ctx, cfg, nullLogger(), *traces, setup, se, est, x_s,
                              plan, /*pl_now_s=*/1000.0, /*datum_changed=*/false,
                              /*changed_consts=*/{}, /*sigma0_adapt=*/0.0,
                              /*perconst_active=*/false, /*perconst_publish=*/{},
                              /*perconst_dec_withhold=*/{}, /*perconst_stats=*/{});
    }

    /// Destroy the traces unit (flush + close) and return the events CSV body.
    std::string eventsCsv() {
        traces.reset();
        std::ifstream f(trace_dir / "network_events.csv");
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }
};

/// Write the warm-up epoch + `sol` with its R-record set the way SB_EMITSTORE
/// does, read the file back, replay every record through a FRESH emitter, and
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
    h.ctx->carry.eph_cutover_disc_pending = {"G02"};

    const NetworkSolPrepResult r = h.run();

    EXPECT_EQ(r.retro_emit_sats.count("G02"), 1u);        // the fold under test
    EXPECT_TRUE(h.ctx->carry.eph_cutover_disc_pending.empty());

    h.setup.emitter->emitToStore(r.sol, h.setup.network_store);
    EXPECT_EQ(h.setup.emitter->clockDiscontinuityCounter("G02"), 1);   // live
    EXPECT_EQ(h.setup.emitter->clockDiscontinuityCounter("G01"), 0);
    EXPECT_EQ(replayedClockDisc(h, r, "G02", "eph"), 1);               // replay == live
    EXPECT_EQ(h.eventsCsv().find("eph_cutover_disc_landed") == std::string::npos, false);
}

TEST(NetworkSolPrepDiscTest, CbExcursionMarkLandsInRRecordAndReplays) {
    Harness h("cbexc");
    h.ctx->carry.cb_excursion_pending_bump = {"G03"};

    const NetworkSolPrepResult r = h.run();

    EXPECT_EQ(r.retro_emit_sats.count("G03"), 1u);
    EXPECT_TRUE(h.ctx->carry.cb_excursion_pending_bump.empty());
    EXPECT_EQ(h.ctx->carry.cb_exc_bumps, 1);              // run-end telemetry intact

    h.setup.emitter->emitToStore(r.sol, h.setup.network_store);
    EXPECT_EQ(h.setup.emitter->clockDiscontinuityCounter("G03"), 1);
    EXPECT_EQ(replayedClockDisc(h, r, "G03", "cbexc"), 1);
    EXPECT_EQ(h.eventsCsv().find("cb_excursion_disc_landed") == std::string::npos, false);
}

TEST(NetworkSolPrepDiscTest, BasisRelatchMarkLandsInRRecordAndReplays) {
    Harness h("relatch");
    h.ctx->carry.basis_relatch_disc_pending = {"G04"};

    const NetworkSolPrepResult r = h.run();

    EXPECT_EQ(r.retro_emit_sats.count("G04"), 1u);
    EXPECT_TRUE(h.ctx->carry.basis_relatch_disc_pending.empty());

    h.setup.emitter->emitToStore(r.sol, h.setup.network_store);
    EXPECT_EQ(h.setup.emitter->clockDiscontinuityCounter("G04"), 1);
    EXPECT_EQ(replayedClockDisc(h, r, "G04", "relatch"), 1);
    EXPECT_EQ(h.eventsCsv().find("relatch_disc_landed") == std::string::npos, false);
}

TEST(NetworkSolPrepDiscTest, IfPinReselectMarkLandsInRRecordAndReplays) {
    Harness h("reselect");
    // The emitted pair moved vs the last EMITTED product's pair → a genuine
    // re-selection: the emitted δtˢ↔dˢ split re-references, and the clock disc
    // counter (datum-change components only, no magnitude jump test) is
    // signalled ONLY by this mark — which must therefore reach the R record.
    h.ctx->carry.last_if_pin_sel["G02"]      = {"C1W", "C2W"};
    h.ctx->carry.this_epoch_if_pin_sel["G02"] = {"C1W", "C5Q"};

    const NetworkSolPrepResult r = h.run();

    EXPECT_EQ(r.retro_emit_sats.count("G02"), 1u);
    // The emit baseline advanced with the bump it pairs with.
    EXPECT_EQ(h.ctx->carry.last_if_pin_sel.at("G02"),
              (std::pair<std::string, std::string>{"C1W", "C5Q"}));

    h.setup.emitter->emitToStore(r.sol, h.setup.network_store);
    EXPECT_EQ(h.setup.emitter->clockDiscontinuityCounter("G02"), 1);
    EXPECT_EQ(replayedClockDisc(h, r, "G02", "reselect"), 1);
    EXPECT_EQ(h.eventsCsv().find("reselect") == std::string::npos, false);
}

TEST(NetworkSolPrepDiscTest, CbReseedMarkLandsInRRecordAndReplays) {
    Harness h("reseed");
    h.ctx->carry.cb_reseed_disc_pending = {"G03"};

    const NetworkSolPrepResult r = h.run();

    EXPECT_EQ(r.retro_emit_sats.count("G03"), 1u);
    EXPECT_TRUE(h.ctx->carry.cb_reseed_disc_pending.empty());

    h.setup.emitter->emitToStore(r.sol, h.setup.network_store);
    EXPECT_EQ(h.setup.emitter->clockDiscontinuityCounter("G03"), 1);
    EXPECT_EQ(replayedClockDisc(h, r, "G03", "reseed"), 1);
    EXPECT_EQ(h.eventsCsv().find("reseed_disc_landed") == std::string::npos, false);
}

// ---------------------------------------------------------------------------
// Cross-queue dedupe: one event, one step — even when several queues land the
// same satellite in the same epoch (the Q3 rule, chained across the blocks).
// ---------------------------------------------------------------------------

TEST(NetworkSolPrepDiscTest, SameSatInFiveQueuesStepsCountersOnce) {
    Harness h("dedupe");
    h.ctx->carry.eph_cutover_disc_pending  = {"G01"};
    h.ctx->carry.cb_excursion_pending_bump = {"G01"};
    h.ctx->carry.basis_relatch_disc_pending = {"G01"};
    h.ctx->carry.last_if_pin_sel["G01"]      = {"C1W", "C2W"};
    h.ctx->carry.this_epoch_if_pin_sel["G01"] = {"C1W", "C5Q"};
    h.ctx->carry.cb_reseed_disc_pending = {"G01"};

    const NetworkSolPrepResult r = h.run();

    EXPECT_EQ(r.retro_emit_sats.count("G01"), 1u);

    h.setup.emitter->emitToStore(r.sol, h.setup.network_store);
    EXPECT_EQ(h.setup.emitter->clockDiscontinuityCounter("G01"), 1);   // once, not 5×
    EXPECT_EQ(replayedClockDisc(h, r, "G01", "dedupe"), 1);            // one R entry, one step

    // Provenance stays honest: each queue still writes its own trace row.
    const std::string csv = h.eventsCsv();
    EXPECT_NE(csv.find("eph_cutover_disc_landed"), std::string::npos);
    EXPECT_NE(csv.find("cb_excursion_disc_landed"), std::string::npos);
    EXPECT_NE(csv.find("relatch_disc_landed"), std::string::npos);
    EXPECT_NE(csv.find("reselect"), std::string::npos);
    EXPECT_NE(csv.find("reseed_disc_landed"), std::string::npos);
}

// ---------------------------------------------------------------------------
// mark == land: a queued satellite absent from this emission must stay PENDING
// and appear in NO R record (the emitter would clear a mark that never stepped,
// and the R record would overstate a bump the product never carried).
// ---------------------------------------------------------------------------

TEST(NetworkSolPrepDiscTest, AbsentSatStaysPendingAndUnserialized) {
    Harness h("absent");
    h.ctx->carry.eph_cutover_disc_pending  = {"G09"};
    h.ctx->carry.cb_excursion_pending_bump = {"G09"};
    h.ctx->carry.basis_relatch_disc_pending = {"G09"};
    h.ctx->carry.cb_reseed_disc_pending     = {"G09"};

    const NetworkSolPrepResult r = h.run();

    EXPECT_TRUE(r.retro_emit_sats.empty());
    EXPECT_EQ(h.ctx->carry.eph_cutover_disc_pending.count("G09"), 1u);
    EXPECT_EQ(h.ctx->carry.cb_excursion_pending_bump.count("G09"), 1u);
    EXPECT_EQ(h.ctx->carry.basis_relatch_disc_pending.count("G09"), 1u);
    EXPECT_EQ(h.ctx->carry.cb_reseed_disc_pending.count("G09"), 1u);
    EXPECT_EQ(h.ctx->carry.cb_exc_bumps, 0);

    h.setup.emitter->emitToStore(r.sol, h.setup.network_store);
    EXPECT_EQ(h.setup.emitter->clockDiscontinuityCounter("G09"), 0);
    EXPECT_EQ(replayedClockDisc(h, r, "G09", "absent"), 0);
}
