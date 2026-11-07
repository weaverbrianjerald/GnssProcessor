/**
 * @file test_resume_hold_restore.cpp
 * @brief Contract tests for the emit-side phase-bias hold across the resume seam
 *        (card CRS2, docs/refound/CRS2_resume_hold_and_latches_2026-11-06.md): the
 *        snapshot's hold records (@c PH per held key, @c PHM per constellation, @c PHC /
 *        @c PHF per satellite), their entity round trips, the loader's side of the two
 *        knobs (@c network.state_snapshot_hold_capture, @c network.resume_hold_restore),
 *        and the continuity claim: a hold restored from a snapshot continues the held
 *        value and the generation at its next emit where a cold hold first-sights.
 *
 * The tests lock:
 *   (a) each record round-trips through recordToSnapshot / serialize / parse /
 *       recordFromSnapshot field by field, with a '|'-bearing key, a generation above 1
 *       and every flag in both states, and a stale reader-side row is replaced;
 *   (b) a snapshot with no hold rows restores nothing (an older file keeps the
 *       first-sighting behaviour, and the containers stay empty);
 *   (c) a zero generation or a non-finite level is a hard parse failure, never a silent
 *       partial restore;
 *   (d) with the write knob off none of the rows is written: the file matches the format
 *       before the hold rows byte for byte, and arming it adds only the rows and the END
 *       line count they advance;
 *   (e) the loader's defaults and interlocks;
 *   (f) the continuity claim at the seam of networkPbHold itself.
 *
 * The seam on a real run (the wire continuous at the first leg emit, the consumer at the
 * seed's convergence) is graded on the harness legs, not here.
 */
#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "network/emit/PbHold.hpp"
#include "network/diag/RunTraces.hpp"
#include "network/state/Entities.hpp"
#include "network/state/RunContext.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkStateSnapshot.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

using C = net::ConstellationState;
using A = net::SatelliteState;
using G = net::SatSignalState;

/// A run context on the default configuration (the record functions read its knobs).
struct Ctx {
    GnssConfig cfg;
    NetworkRunContext ctx{cfg, cfg.getNetworkConfig()};
};

NetworkStateSnapshot baseSnapshot() {
    NetworkStateSnapshot s;
    s.snap_week = 2313;
    s.snap_tow  = 275580.0;
    s.wall_utc  = "2026-11-06T00:00:00Z";
    s.generation_id = 1399161629u;
    s.config_fingerprint = 0x212dada761590f60ull;
    s.n_emitted = 485;
    s.prev_week = 2313;
    s.prev_tow  = 275580.0;
    return s;
}

/// The record @p tag of @p from, written, serialized, parsed and restored into @p to;
/// the serialized text is returned for the row-format assertions.
std::string roundTrip(const char* tag, const NetworkRunContext& from, NetworkRunContext& to) {
    NetworkStateSnapshot snap = baseSnapshot();
    net::recordToSnapshot(tag, from, snap);
    std::ostringstream os;
    NetworkStateSnapshotIO::serialize(os, snap);
    std::istringstream is(os.str());
    NetworkStateSnapshot back;
    std::string err;
    EXPECT_TRUE(NetworkStateSnapshotIO::parse(is, back, &err)) << err;
    net::recordFromSnapshot(tag, back, to);
    return os.str();
}

net::PbHoldKey heldKey(std::uint32_t gen, double held) {
    net::PbHoldKey k;
    k.generation      = gen;
    k.held_m          = held;
    k.last_emit_s     = 1399436580.0;
    k.frz_ref         = -0.3125;
    k.frz_ok          = true;
    k.frz_last        = 0.0078125;
    k.smh_reverted    = false;
    k.smh_last        = 0.00390625;
    k.smh_ref_pending = true;
    k.smh_on_clone    = true;
    k.smh_lah_last    = -1.0000000000000002;
    return k;
}

void expectSameKey(const net::PbHoldKey* a, const net::PbHoldKey* b) {
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->generation, b->generation);
    EXPECT_EQ(a->held_m, b->held_m);
    EXPECT_EQ(a->last_emit_s, b->last_emit_s);
    EXPECT_EQ(a->frz_ref, b->frz_ref);
    EXPECT_EQ(a->frz_ok, b->frz_ok);
    EXPECT_EQ(a->frz_last, b->frz_last);
    EXPECT_EQ(a->smh_reverted, b->smh_reverted);
    EXPECT_EQ(a->smh_last, b->smh_last);
    EXPECT_EQ(a->smh_ref_pending, b->smh_ref_pending);
    EXPECT_EQ(a->smh_on_clone, b->smh_on_clone);
    EXPECT_EQ(a->smh_lah_last, b->smh_lah_last);
}

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = gnss_test::tempDir() / ("resumehold_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

std::string networkConfig(const std::string& extra = "") {
    return std::string("{\n")
        + "  \"processing\": {\n"
          "    \"mode\": \"ppp-rtk-network\",\n"
          "    \"active_systems\": [ \"GPS\", \"Galileo\" ],\n"
          "    \"signal_priorities\": { \"GPS\": \"WC\", \"Galileo\": \"CB\" }\n"
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

bool loadInto(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = writeTempConfig(tag, body);
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

}  // namespace

// ---------------------------------------------------------------------------
// (a) the entity round trips, one per record
// ---------------------------------------------------------------------------

TEST(ResumeHoldRestore, RecordRoundTripPH) {
    Ctx a, b;
    net::NetworkEntities& e = a.ctx.entities;
    net::putSig(e, "G05|L1C", &G::pb_hold) = heldKey(2u, 17.140625);
    net::PbHoldKey flipped = heldKey(1u, -0.5009765625);
    flipped.frz_ok = false; flipped.smh_reverted = true;
    flipped.smh_ref_pending = false; flipped.smh_on_clone = false;
    net::putSig(e, "E11|L5Q", &G::pb_hold) = flipped;
    net::putSig(e, "G07|L2W", &G::anchor_incumbent) = "AAAA";   // a row without the field
    // A stale value on the reader's side is replaced, not merged ("PH" clears first).
    net::putSig(b.ctx.entities, "G09|L1C", &G::pb_hold) = heldKey(3u, 9.0);

    const std::string text = roundTrip("PH", a.ctx, b.ctx);
    // The '|' in the key survives the comma-split format, the generation is written as
    // an integer and the flags as 0/1 in the declaration order of PbHoldRow.
    EXPECT_NE(text.find("PH,G05|L1C,2,17.140625,1399436580,-0.3125,1,0.0078125,0,0.00390625,1,1,-1.0000000000000002\n"),
              std::string::npos) << text;
    EXPECT_NE(text.find("PH,E11|L5Q,1,-0.5009765625,1399436580,-0.3125,0,0.0078125,1,0.00390625,0,0,"),
              std::string::npos) << text;
    // The hold rows use the existing v1 header (as in RS-3 / RS-5): on their own they
    // claim neither frame nor coupling state.
    EXPECT_EQ(text.rfind("# gnss_network_state_snapshot v1\n", 0), 0u) << text.substr(0, 60);

    const net::SatSignalTable& r = b.ctx.entities.sat_signals;
    EXPECT_EQ(net::present(r, &G::pb_hold), 2u);
    expectSameKey(net::getSig(e.sat_signals, "G05|L1C", &G::pb_hold), net::getSig(r, "G05|L1C", &G::pb_hold));
    expectSameKey(net::getSig(e.sat_signals, "E11|L5Q", &G::pb_hold), net::getSig(r, "E11|L5Q", &G::pb_hold));
    EXPECT_EQ(net::getSig(r, "G07|L2W", &G::pb_hold), nullptr);
    EXPECT_EQ(net::getSig(r, "G09|L1C", &G::pb_hold), nullptr);
    EXPECT_EQ(b.ctx.entities.reach.failures[static_cast<std::size_t>(net::Converter::SatSignal)], 0);
}

TEST(ResumeHoldRestore, RecordRoundTripPHM) {
    Ctx a, b;
    net::put(a.ctx.entities.constellations, System::GPS, &C::pb_hold_frz_M) = 0.123456789012345;
    net::put(a.ctx.entities.constellations, System::Galileo, &C::pb_hold_frz_M) = -2.5;
    net::put(b.ctx.entities.constellations, System::BeiDou, &C::pb_hold_frz_M) = 9.0;   // stale
    const std::string text = roundTrip("PHM", a.ctx, b.ctx);
    EXPECT_NE(text.find("PHM,G,0.123456789012345\n"), std::string::npos) << text;
    EXPECT_NE(text.find("PHM,E,-2.5\n"), std::string::npos) << text;
    const net::ConstellationTable& r = b.ctx.entities.constellations;
    EXPECT_EQ(net::present(r, &C::pb_hold_frz_M), 2u);
    ASSERT_NE(net::get(r, System::GPS, &C::pb_hold_frz_M), nullptr);
    EXPECT_EQ(*net::get(r, System::GPS, &C::pb_hold_frz_M), 0.123456789012345);
    ASSERT_NE(net::get(r, System::Galileo, &C::pb_hold_frz_M), nullptr);
    EXPECT_EQ(*net::get(r, System::Galileo, &C::pb_hold_frz_M), -2.5);
    EXPECT_EQ(net::get(r, System::BeiDou, &C::pb_hold_frz_M), nullptr);
}

TEST(ResumeHoldRestore, RecordRoundTripPHC) {
    Ctx a, b;
    net::putSat(a.ctx.entities, "G12", &A::pb_hold_prev_clock_m) = 34875.0625;
    net::putSat(a.ctx.entities, "E02", &A::pb_hold_prev_clock_m) = -0.00048828125;
    net::putSat(a.ctx.entities, "G25", &A::pb_hold_frz_prev_clk) = 1.0;   // a row without the field
    net::putSat(b.ctx.entities, "G31", &A::pb_hold_prev_clock_m) = 9.0;   // stale
    const std::string text = roundTrip("PHC", a.ctx, b.ctx);
    EXPECT_NE(text.find("PHC,G12,34875.0625\n"), std::string::npos) << text;
    const net::SatTable& r = b.ctx.entities.sats;
    EXPECT_EQ(net::present(r, &A::pb_hold_prev_clock_m), 2u);
    EXPECT_EQ(*net::getSat(r, "G12", &A::pb_hold_prev_clock_m), 34875.0625);
    EXPECT_EQ(*net::getSat(r, "E02", &A::pb_hold_prev_clock_m), -0.00048828125);
    EXPECT_EQ(net::getSat(r, "G25", &A::pb_hold_prev_clock_m), nullptr);
    EXPECT_EQ(net::getSat(r, "G31", &A::pb_hold_prev_clock_m), nullptr);
}

TEST(ResumeHoldRestore, RecordRoundTripPHF) {
    Ctx a, b;
    net::putSat(a.ctx.entities, "G12", &A::pb_hold_frz_prev_clk) = 34875.0625;
    net::putSat(a.ctx.entities, "E02", &A::pb_hold_frz_prev_clk) = -0.00048828125;
    net::putSat(a.ctx.entities, "G25", &A::pb_hold_prev_clock_m) = 1.0;   // a row without the field
    const std::string text = roundTrip("PHF", a.ctx, b.ctx);
    EXPECT_NE(text.find("PHF,E02,-0.00048828125\n"), std::string::npos) << text;
    const net::SatTable& r = b.ctx.entities.sats;
    EXPECT_EQ(net::present(r, &A::pb_hold_frz_prev_clk), 2u);
    EXPECT_EQ(*net::getSat(r, "G12", &A::pb_hold_frz_prev_clk), 34875.0625);
    EXPECT_EQ(net::getSat(r, "G25", &A::pb_hold_frz_prev_clk), nullptr);
}

// ---------------------------------------------------------------------------
// (b) / (c) / (d) absence, corruption and the write knob at the format level
// ---------------------------------------------------------------------------

TEST(ResumeHoldRestore, SnapshotWithoutHoldRowsRestoresNothing) {
    const NetworkStateSnapshot s = baseSnapshot();
    ASSERT_FALSE(s.hasHoldState());
    std::ostringstream os;
    NetworkStateSnapshotIO::serialize(os, s);
    const std::string text = os.str();
    EXPECT_EQ(text.find("\nPH,"), std::string::npos) << text;
    EXPECT_EQ(text.find("\nPHM,"), std::string::npos) << text;

    NetworkStateSnapshot r;
    std::string err;
    std::istringstream is(text);
    ASSERT_TRUE(NetworkStateSnapshotIO::parse(is, r, &err)) << err;
    EXPECT_FALSE(r.hasHoldState());

    // Restoring from such a file leaves the reader's tables as they were: an older
    // snapshot keeps the first-sighting behaviour.
    Ctx b;
    net::recordFromSnapshot("PH", r, b.ctx);
    net::recordFromSnapshot("PHM", r, b.ctx);
    EXPECT_EQ(net::present(b.ctx.entities.sat_signals, &G::pb_hold), 0u);
    EXPECT_EQ(net::present(b.ctx.entities.constellations, &C::pb_hold_frz_M), 0u);
}

/// A zero generation never appears on a live key, and a non-finite held value or
/// reference, which strtod accepts, would reach the wire at the first post-resume
/// emit.  Both are hard parse failures (the caller's documented cold-start signal).
TEST(ResumeHoldRestore, ZeroGenerationOrNonFiniteLevelIsAHardParseFailure) {
    NetworkStateSnapshot s = baseSnapshot();
    NetworkStateSnapshot::PbHoldRow h;
    h.generation = 1; h.held_m = 1.0;
    s.pb_hold["G01|L1C"] = h;
    s.pb_hold_frz_M["G"] = 0.5;
    std::ostringstream os;
    NetworkStateSnapshotIO::serialize(os, s);
    const std::string good = os.str();
    const auto replaceLine = [&](const std::string& prefix, const std::string& with) {
        std::string t = good;
        const std::size_t pos = t.find(prefix);
        EXPECT_NE(pos, std::string::npos) << prefix;
        const std::size_t eol = t.find('\n', pos);
        t.replace(pos, eol - pos, with);
        return t;
    };
    NetworkStateSnapshot r;
    std::string err;
    {
        std::istringstream is(good);
        ASSERT_TRUE(NetworkStateSnapshotIO::parse(is, r, &err)) << err;
    }
    {
        std::istringstream is(replaceLine("PH,G01|L1C,", "PH,G01|L1C,0,1,0,0,0,0,0,0,0,0,0"));
        EXPECT_FALSE(NetworkStateSnapshotIO::parse(is, r, &err));
        EXPECT_NE(err.find("generation"), std::string::npos) << err;
    }
    {
        std::istringstream is(replaceLine("PH,G01|L1C,", "PH,G01|L1C,1,nan,0,0,0,0,0,0,0,0,0"));
        EXPECT_FALSE(NetworkStateSnapshotIO::parse(is, r, &err));
        EXPECT_NE(err.find("PH"), std::string::npos) << err;
    }
    {
        std::istringstream is(replaceLine("PH,G01|L1C,", "PH,G01|L1C,1,1,0,0,2,0,0,0,0,0,0"));   // a flag that is not 0/1
        EXPECT_FALSE(NetworkStateSnapshotIO::parse(is, r, &err));
    }
    {
        std::istringstream is(replaceLine("PHM,G,", "PHM,G,inf"));
        EXPECT_FALSE(NetworkStateSnapshotIO::parse(is, r, &err));
        EXPECT_NE(err.find("PHM"), std::string::npos) << err;
    }
}

/// With the write knob off the driver never fills the containers, so the file the same
/// snapshot produces is byte-identical to the format before the hold rows (standing
/// invariant #6, at the format level); arming it adds only the rows and the END count.
TEST(ResumeHoldRestore, WriteKnobOffEmitsNoneOfTheRows) {
    const NetworkStateSnapshot off = baseSnapshot();
    std::ostringstream a;
    NetworkStateSnapshotIO::serialize(a, off);
    NetworkStateSnapshot on = baseSnapshot();
    NetworkStateSnapshot::PbHoldRow h;
    h.generation = 1; h.held_m = 1.0;
    on.pb_hold["G01|L1C"] = h;
    on.pb_hold_frz_M["G"] = 0.5;
    on.pb_hold_prev_clock_m["G01"] = 2.0;
    on.pb_hold_frz_prev_clk["G01"] = 2.0;
    std::ostringstream b;
    NetworkStateSnapshotIO::serialize(b, on);
    const auto lines = [](const std::string& t) {
        std::vector<std::string> v;
        std::istringstream is(t);
        for (std::string ln; std::getline(is, ln); ) v.push_back(ln);
        return v;
    };
    const auto la = lines(a.str()), lb = lines(b.str());
    ASSERT_EQ(la.size() + 4u, lb.size());
    std::vector<std::string> diff;
    std::size_t ia = 0;
    for (const auto& ln : lb) {
        if (ia < la.size() && la[ia] == ln) { ++ia; continue; }
        diff.push_back(ln);
    }
    EXPECT_EQ(ia + 1u, la.size()) << "only the END line may also differ";
    ASSERT_EQ(diff.size(), 5u);
    EXPECT_EQ(diff[0], "PH,G01|L1C,1,1,0,0,0,0,0,0,0,0,0");
    EXPECT_EQ(diff[1], "PHM,G,0.5");
    EXPECT_EQ(diff[2], "PHC,G01,2");
    EXPECT_EQ(diff[3], "PHF,G01,2");
    EXPECT_EQ(diff[4].rfind("END,", 0), 0u) << diff[4];
}

// ---------------------------------------------------------------------------
// (e) the loader
// ---------------------------------------------------------------------------

TEST(ResumeHoldRestoreConfig, DefaultsAreOff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("defaults", networkConfig(), cfg));
    EXPECT_FALSE(cfg.getNetworkConfig().state_snapshot_hold_capture);
    EXPECT_FALSE(cfg.networkResumeHoldRestore());
    const NetworkRunContext ctx{cfg, cfg.getNetworkConfig()};
    EXPECT_FALSE(ctx.knobs.resume_hold_restore);
}

TEST(ResumeHoldRestoreConfig, CaptureKnobParsesAndDoesNotArmTheReadSide) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("capture",
        networkConfig("    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"emit_pb_hold\": true,\n"
                      "    \"state_snapshot_hold_capture\": true,\n"), cfg));
    EXPECT_TRUE(cfg.getNetworkConfig().state_snapshot_hold_capture);
    EXPECT_FALSE(cfg.networkResumeHoldRestore());
}

/// The read knob is only meaningful when the realization frame underneath it also
/// continues, as for the emit layer (RS-5).
TEST(ResumeHoldRestoreConfig, RestoreWithoutFrameRestoreIsRejected) {
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("noframe",
        networkConfig("    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"resume_hold_restore\": true,\n"), cfg));
}

TEST(ResumeHoldRestoreConfig, ArmedPostureLoadsAndReachesTheKnobSnapshot) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("armed",
        networkConfig("    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"emit_pb_hold\": true,\n"
                      "    \"state_snapshot_frame_capture\": true,\n"
                      "    \"state_snapshot_hold_capture\": true,\n"
                      "    \"resume_frame_restore\": true,\n"
                      "    \"resume_hold_restore\": true,\n"), cfg));
    EXPECT_TRUE(cfg.getNetworkConfig().state_snapshot_hold_capture);
    EXPECT_TRUE(cfg.networkResumeHoldRestore());
    EXPECT_TRUE(cfg.networkResumeFrameRestore());
    const NetworkRunContext ctx{cfg, cfg.getNetworkConfig()};
    EXPECT_TRUE(ctx.knobs.resume_hold_restore);
}

/// Armed with no snapshot path, or with the hold off, the loader warns and still loads
/// (inert, not fatal), as it does for every other resume-response knob.
TEST(ResumeHoldRestoreConfig, ArmedWithoutASnapshotPathOrTheHoldStillLoads) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("inert",
        networkConfig("    \"state_snapshot_hold_capture\": true,\n"
                      "    \"resume_frame_restore\": true,\n"
                      "    \"resume_hold_restore\": true,\n"), cfg));
    EXPECT_TRUE(cfg.networkResumeHoldRestore());
}

// ---------------------------------------------------------------------------
// (f) the continuity claim at networkPbHold
// ---------------------------------------------------------------------------

namespace {

std::shared_ptr<spdlog::logger> holdLogger() {
    static auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    return std::make_shared<spdlog::logger>("crs2_hold_test", sink);
}

/// Two GPS satellites with one clock and one phase bias each; G32's bias is the quantity
/// under test and G12 is a constant control.
NetworkSolutionEpoch holdEpoch(double tow, double g32_pb) {
    NetworkSolutionEpoch sol;
    sol.epoch_time = GnssTime{ 2313, tow };
    sol.iod_ssr = 3; sol.provider_id = 42; sol.solution_id = 1;
    NetworkSatSolution a;
    a.sat = "G32"; a.has_clock = true; a.clock_c0_m = 1.0;
    a.phase_bias["L1W"] = NetworkPhaseBias{ g32_pb, true, 1 };
    sol.sats.push_back(a);
    NetworkSatSolution b;
    b.sat = "G12"; b.has_clock = true; b.clock_c0_m = 1.0;
    b.phase_bias["L1W"] = NetworkPhaseBias{ 0.10, true, 1 };
    sol.sats.push_back(b);
    return sol;
}

double pbOf(const NetworkSolutionEpoch& sol, const std::string& sat) {
    for (const auto& s : sol.sats)
        if (s.sat == sat) return s.phase_bias.at("L1W").bias_m;
    ADD_FAILURE() << "no phase bias for " << sat;
    return 0.0;
}

std::uint32_t genOf(const NetworkSolutionEpoch& sol, const std::string& sat) {
    for (const auto& s : sol.sats)
        if (s.sat == sat) return s.phase_bias.at("L1W").hold_generation;
    ADD_FAILURE() << "no phase bias for " << sat;
    return 0;
}

/// A run context, traces and emitter wired as networkEmitStore wires them, with the
/// hold armed through the JSON loader (ctx.knobs is a const snapshot).
struct HoldHarness {
    LogManager                             log_mgr;
    GnssConfig                             cfg;
    fs::path                               cfg_path;
    std::unique_ptr<NetworkRunContext>     ctx;
    std::unique_ptr<NetworkRunTraces>      traces;
    std::unique_ptr<NetworkProductEmitter> emitter;
    SyncedEpoch                            se;

    explicit HoldHarness(const char* tag) {
        const std::string body = std::string("{\n") +
            "  \"processing\": {\n"
            "    \"mode\": \"ppp-rtk-network\",\n"
            "    \"active_systems\": [ \"GPS\" ],\n"
            "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
            "  },\n"
            "  \"network\": {\n"
            "    \"emit_pb_hold\": true,\n"
            "    \"emit_pb_hold_max_gap_s\": 300.0,\n"
            "    \"stations\": [\n"
            "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
            "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
            "    ]\n"
            "  }\n"
            "}\n";
        cfg_path = gnss_test::tempDir() / ("crs2_hold_cfg_" + std::string(tag) + ".json");
        { std::ofstream f(cfg_path); f << body; }
        EXPECT_TRUE(cfg.loadFromJson(cfg_path, log_mgr));
        ctx = std::make_unique<NetworkRunContext>(cfg, cfg.getNetworkConfig());
        EXPECT_TRUE(ctx->knobs.emit_pb_hold);
        const fs::path dir = gnss_test::tempDir() / ("crs2_hold_traces_" + std::string(tag));
        fs::create_directories(dir);
        traces = std::make_unique<NetworkRunTraces>(dir);
        emitter = std::make_unique<NetworkProductEmitter>(0.05, /*emit_clock_disc=*/false);
    }
    ~HoldHarness() { std::error_code ec; fs::remove(cfg_path, ec); }

    void step(NetworkSolutionEpoch& sol) {
        se.time = sol.epoch_time;
        networkPbHold(*ctx, cfg, holdLogger(), *traces, *emitter, se, sol);
    }
};

}  // namespace

/// The seam in miniature: a hold that ran for two epochs is written to a snapshot; a
/// fresh context restored from it emits the held value in the same generation at the
/// next epoch and first-sights nothing, while a cold context first-sights every key and
/// emits the estimate (the re-level of card CRS section 4).
TEST(ResumeHoldRestore, RestoredHoldContinuesTheHeldValueAndGeneration) {
    HoldHarness writer("writer");
    auto e0 = holdEpoch(1000.0, 0.30);
    writer.step(e0);
    ASSERT_EQ(genOf(e0, "G32"), 1u);
    auto e1 = holdEpoch(1030.0, 0.35);
    writer.step(e1);
    EXPECT_DOUBLE_EQ(pbOf(e1, "G32"), 0.30) << "the second emission is held at the first value";
    EXPECT_EQ(writer.ctx->counters.n_pbh_gen_first, 2);

    NetworkStateSnapshot snap = baseSnapshot();
    net::recordToSnapshot("PH", *writer.ctx, snap);
    net::recordToSnapshot("PHM", *writer.ctx, snap);
    net::recordToSnapshot("PHC", *writer.ctx, snap);
    net::recordToSnapshot("PHF", *writer.ctx, snap);
    ASSERT_EQ(snap.pb_hold.size(), 2u);
    EXPECT_EQ(snap.pb_hold.at("G32|L1W").generation, 1u);
    EXPECT_DOUBLE_EQ(snap.pb_hold.at("G32|L1W").held_m, 0.30);
    EXPECT_EQ(snap.pb_hold_prev_clock_m.size(), 2u);   // the coupled-mark baseline of both satellites
    std::ostringstream os;
    NetworkStateSnapshotIO::serialize(os, snap);
    NetworkStateSnapshot back;
    std::string err;
    std::istringstream is(os.str());
    ASSERT_TRUE(NetworkStateSnapshotIO::parse(is, back, &err)) << err;

    HoldHarness resumed("resumed"), cold("cold");
    net::recordFromSnapshot("PH", back, *resumed.ctx);
    net::recordFromSnapshot("PHM", back, *resumed.ctx);
    net::recordFromSnapshot("PHC", back, *resumed.ctx);
    net::recordFromSnapshot("PHF", back, *resumed.ctx);

    auto e2r = holdEpoch(1060.0, 0.40);
    auto e2c = holdEpoch(1060.0, 0.40);
    auto e2w = holdEpoch(1060.0, 0.40);
    resumed.step(e2r);
    cold.step(e2c);
    writer.step(e2w);
    // The restored hold continues the writer: the same held value, the same generation,
    // no first sighting, and the held emission counted as held.
    EXPECT_DOUBLE_EQ(pbOf(e2r, "G32"), pbOf(e2w, "G32"));
    EXPECT_DOUBLE_EQ(pbOf(e2r, "G32"), 0.30);
    EXPECT_EQ(genOf(e2r, "G32"), 1u);
    EXPECT_EQ(resumed.ctx->counters.n_pbh_gen_first, 0);
    EXPECT_EQ(resumed.ctx->counters.n_pbh_held_emissions, 2);
    // The cold hold first-sights both keys at the current estimate: the 0.10 m step the
    // restore removes.
    EXPECT_DOUBLE_EQ(pbOf(e2c, "G32"), 0.40);
    EXPECT_EQ(genOf(e2c, "G32"), 1u);
    EXPECT_EQ(cold.ctx->counters.n_pbh_gen_first, 2);
    EXPECT_EQ(cold.ctx->counters.n_pbh_held_emissions, 0);
}
