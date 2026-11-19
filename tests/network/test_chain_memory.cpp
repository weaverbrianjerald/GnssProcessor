/**
 * @file test_chain_memory.cpp
 * @brief The chain memory section of the snapshot (network.state_snapshot_chain_capture, network.resume_chain_restore):
 *        each owner's lines written, read back into a fresh state and written again give the same text, every member
 *        survives with its own value, the snapshot carries the section verbatim (and nothing without it), the section's
 *        fingerprint follows the keys that shape the memory and stays out of the snapshot's own, the loader's
 *        interlocks, the restore's outcomes (restored, refused, nothing staged), and the rule that judges a restored
 *        pinned life on the resumed run's own ledger.
 */
#include <gtest/gtest.h>

#include <fmt/format.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "config/GnssConfig.hpp"
#include "network/ar/ArcLife.hpp"
#include "network/ar/RingFix.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/state/ChainMemory.hpp"
#include "network/state/Entities.hpp"
#include "network/state/RunContext.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkStateSnapshot.hpp"

#include "../support/NetworkTestRun.hpp"
#include "ConfigLoadCapture.hpp"

using namespace GnssProcessor;
using TestSupport::contains;
using TestSupport::LoadedConfig;
using TestSupport::loadNetworkConfigCapturing;

namespace {

/// The lines of a section, without empty ones.
std::vector<std::string> linesOf(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream is(text);
    std::string line;
    while (std::getline(is, line))
        if (!line.empty()) out.push_back(line);
    return out;
}

/// The comma-separated fields of a line (empty fields kept).
std::vector<std::string> fieldsOf(const std::string& line) {
    std::vector<std::string> f;
    std::string cur;
    for (const char c : line) {
        if (c == ',') {
            f.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    f.push_back(cur);
    return f;
}

/// A run context on the default configuration.
struct Ctx {
    GnssConfig cfg;
    NetworkRunContext ctx{cfg, cfg.getNetworkConfig()};
};

/// The float chain the chain's event rows are written for, with @p extra spliced into "network".
std::string floatChain(const std::string& extra) {
    return std::string("    \"phase_leveling\": true,\n"
                       "    \"shared_carry_full_cov\": true,\n"
                       "    \"carry_conditional_coupling\": true,\n"
                       "    \"ambiguity_vehicle\": \"float_kf\",\n"
                       "    \"phase_datum_eq34\": true,\n")
         + extra;
}

/// The snapshot keys every capture and restore below needs: a path, the frame and datum halves.
const char* kSnapshotKeys =
    "    \"state_snapshot_path\": \"snap.txt\",\n"
    "    \"state_snapshot_frame_capture\": true,\n"
    "    \"state_snapshot_datum_capture\": true,\n"
    "    \"resume_frame_restore\": true,\n"
    "    \"resume_datum_restore\": true,\n";

/// The snapshot's own fingerprint a run of @p cfg computes (two stations, the first the pivot).
std::uint64_t resumeFingerprint(const GnssConfig& cfg) {
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    ctx.carry.snap_path = "state_snapshot.snap";   // the builder is a no-op without a path
    std::vector<NetStation> st(2);
    st[0].name = "AAAA"; st[0].is_pivot = true;
    st[1].name = "BBBB"; st[1].is_pivot = false;
    networkSnapshotFingerprint(ctx, cfg, st);
    return ctx.carry.live_fingerprint;
}

/// The roster of the write and restore cases.
const std::vector<std::string> kRoster{"WTZR00DEU", "GOPE00CZE", "ONSA00SWE"};

/// A run of @p cfg for the write and restore cases: the roster assigned, a logger that keeps what it prints.
struct MemoryRun {
    std::ostringstream              text;
    std::shared_ptr<spdlog::logger> log;
    std::unique_ptr<NetworkTestRun> fx;

    MemoryRun(const std::string& tag, const GnssConfig& cfg) {
        log = std::make_shared<spdlog::logger>("chain_memory_" + tag,
                                               std::make_shared<spdlog::sinks::ostream_sink_mt>(text));
        log->set_level(spdlog::level::info);
        fx = std::make_unique<NetworkTestRun>(cfg, log);
        fx->ctx().entities.names.assign(kRoster);
        for (const std::string& name : kRoster) {
            fx->setup().stations.emplace_back();
            fx->setup().stations.back().name = name;
        }
    }
    net::NetworkEntities& entities() { return fx->ctx().entities; }
    NetworkCarryState&    carry() { return fx->ctx().carry; }
};

}  // namespace

// ---- Each owner's lines --------------------------------------------------------------------------

TEST(ChainMemory, RingFixMemoryRoundTripsExactly) {
    NetworkRingFix a;
    a.t0_s = 1399161600.0000195;
    // Every member of a link at a value of its own, different from its default and from every other
    // member of its type, so that a member missing from both the writer and the reader, or two members
    // swapped, changes a value below.
    RingFixLink& L = a.links[RingFixLinkId{3, "E04", "L1C", "L5Q"}];
    L.lambda1 = 0.19029367279836487;
    L.lambda2 = 0.25482804879085386;
    L.chord_since_s = 1399165200.0;
    L.last_chord_s = 1399168770.5;
    L.streak = 7;
    L.streak_n1 = -12;
    L.streak_n2 = -9;
    L.held = true;
    L.n1 = -13;
    L.n2 = -8;
    L.ring = 3;
    L.decided_s = 1399166000.25;
    L.stamp1 = 1399160000.0;
    L.stamp2 = 1399160010.5;
    L.root = "WTZR00DEU";
    L.absent_since_s = 1399168000.75;
    L.monitor_bad = 2;
    L.monitor_trip = true;
    L.released_s = 1399150000.125;
    L.wrong_now = true;
    L.rebase_stamps = true;
    L.inherited = true;
    L.oracle = true;
    L.log_decided = true;
    L.log_n1 = 5;
    L.log_n2 = 4;
    L.nn_n = 60;
    L.nn_gf_m = 0.4481234567891234;
    L.nn_if_m = -0.0123;
    L.nn_moved = true;
    // The flags of a link have two values only: one link per flag, that flag alone set, tells a swap of two flags.
    const std::vector<bool RingFixLink::*> flags{&RingFixLink::held,          &RingFixLink::monitor_trip,
                                                 &RingFixLink::wrong_now,     &RingFixLink::rebase_stamps,
                                                 &RingFixLink::inherited,     &RingFixLink::oracle,
                                                 &RingFixLink::log_decided,   &RingFixLink::nn_moved};
    for (std::size_t i = 0; i < flags.size(); ++i)
        a.links[RingFixLinkId{10 + static_cast<int>(i), "G07", "L1C", "L2W"}].*flags[i] = true;
    a.links[RingFixLinkId{0, "G05", "L1C", "L2W"}];   // a link with every field at its default, root empty
    a.last_tree_s[{3, "E04"}] = 1399168770.5;
    a.certified_edges.insert({3, "E04", "L1C"});
    a.settled_incumbent["E:L1C/L5Q"] = {"E04", "E11"};
    a.settled_incumbent["G:L1C/L2W"];                  // an empty set is kept
    a.settled_run["E:L1C/L5Q"]["E04"] = 11;
    a.settled_run["G:L1C/L2W"];                        // an empty map is kept
    RingFixCascadeLink& K = a.cascade_links[RingFixLinkId{1, "G10", "L1C", "L5Q"}];
    K.held = true;
    K.n3 = 17;
    K.n_ref = -3;
    K.lambda3 = 0.2548;
    K.streak = 6;
    K.streak_n3 = 19;
    K.decided_s = 1399160020.5;
    K.stamp3 = 1399160001.25;
    K.absent_since_s = 1399160040.75;
    K.monitor_bad = 1;
    K.monitor_trip = true;
    K.released_s = 1399160030.0;
    a.cascade_links[RingFixLinkId{2, "G10", "L1C", "L5Q"}].monitor_trip = true;   // the other flag alone
    a.prev_targets[{3, "E04", "L1C"}] = -2.28352407357992;
    a.untied_since_s[{"E:L1C/L5Q", "E11"}] = 1399167000.0;
    a.untied_moves[{"L1C", "E11"}] = RingFixUntiedMove{5, "E:L1C/L5Q"};
    a.untied_moved_s[{"E:L1C/L5Q", "E11"}] = 1399167030.0;
    a.untied_last_move_s[{"E:L1C/L5Q", "E11"}] = 1399167060.0;
    a.nn_reopen.push_back(RingFixLinkId{3, "E04", "L1C", "L5Q"});

    std::ostringstream os;
    ringFixWriteMemory(a, os);
    NetworkRingFix b;
    for (const std::string& line : linesOf(os.str())) EXPECT_TRUE(ringFixReadMemoryLine(b, fieldsOf(line))) << line;
    std::ostringstream os2;
    ringFixWriteMemory(b, os2);
    EXPECT_EQ(os.str(), os2.str());

    const RingFixLink& r = b.links.at(RingFixLinkId{3, "E04", "L1C", "L5Q"});
    EXPECT_EQ(r.lambda1, L.lambda1);
    EXPECT_EQ(r.lambda2, L.lambda2);
    EXPECT_EQ(r.chord_since_s, L.chord_since_s);
    EXPECT_EQ(r.last_chord_s, L.last_chord_s);
    EXPECT_EQ(r.streak, 7);
    EXPECT_EQ(r.streak_n1, -12);
    EXPECT_EQ(r.streak_n2, -9);
    EXPECT_EQ(r.n1, -13);
    EXPECT_EQ(r.n2, -8);
    EXPECT_EQ(r.ring, 3);
    EXPECT_EQ(r.decided_s, L.decided_s);
    EXPECT_EQ(r.stamp1, L.stamp1);
    EXPECT_EQ(r.stamp2, L.stamp2);
    EXPECT_EQ(r.root, "WTZR00DEU");
    EXPECT_EQ(r.absent_since_s, L.absent_since_s);
    EXPECT_EQ(r.monitor_bad, 2);
    EXPECT_EQ(r.released_s, L.released_s);
    EXPECT_EQ(r.log_n1, 5);
    EXPECT_EQ(r.log_n2, 4);
    EXPECT_EQ(r.nn_n, 60);
    EXPECT_EQ(r.nn_gf_m, L.nn_gf_m);
    EXPECT_EQ(r.nn_if_m, L.nn_if_m);
    for (bool RingFixLink::*f : flags) EXPECT_TRUE(r.*f);
    for (std::size_t i = 0; i < flags.size(); ++i) {
        const RingFixLink& one = b.links.at(RingFixLinkId{10 + static_cast<int>(i), "G07", "L1C", "L2W"});
        for (std::size_t j = 0; j < flags.size(); ++j) EXPECT_EQ(one.*flags[j], i == j) << "link " << i << " flag " << j;
    }
    EXPECT_TRUE(b.links.at(RingFixLinkId{0, "G05", "L1C", "L2W"}).root.empty());
    const RingFixCascadeLink& k = b.cascade_links.at(RingFixLinkId{1, "G10", "L1C", "L5Q"});
    EXPECT_TRUE(k.held);
    EXPECT_EQ(k.n3, 17);
    EXPECT_EQ(k.n_ref, -3);
    EXPECT_EQ(k.lambda3, K.lambda3);
    EXPECT_EQ(k.streak, 6);
    EXPECT_EQ(k.streak_n3, 19);
    EXPECT_EQ(k.decided_s, K.decided_s);
    EXPECT_EQ(k.stamp3, K.stamp3);
    EXPECT_EQ(k.absent_since_s, K.absent_since_s);
    EXPECT_EQ(k.monitor_bad, 1);
    EXPECT_TRUE(k.monitor_trip);
    EXPECT_EQ(k.released_s, K.released_s);
    const RingFixCascadeLink& k2 = b.cascade_links.at(RingFixLinkId{2, "G10", "L1C", "L5Q"});
    EXPECT_FALSE(k2.held);
    EXPECT_TRUE(k2.monitor_trip);
    EXPECT_EQ(b.t0_s, a.t0_s);
    EXPECT_EQ(b.settled_incumbent.size(), 2u);
    EXPECT_TRUE(b.settled_incumbent.at("G:L1C/L2W").empty());
    EXPECT_EQ(b.settled_run.size(), 2u);
    EXPECT_EQ(b.untied_moves.at({"L1C", "E11"}).family, "E:L1C/L5Q");
    EXPECT_EQ(b.untied_moves.at({"L1C", "E11"}).station, 5);
    ASSERT_EQ(b.nn_reopen.size(), 1u);
    EXPECT_EQ(b.prev_targets.at({3, "E04", "L1C"}), -2.28352407357992);
}

TEST(ChainMemory, RingFixReadRefusesOtherAndMalformedLines) {
    NetworkRingFix rf;
    EXPECT_FALSE(ringFixReadMemoryLine(rf, fieldsOf("ESC,0|E04,1,2,3")));       // the emitter's
    EXPECT_FALSE(ringFixReadMemoryLine(rf, fieldsOf("RL,3,E04,L1C")));          // too few fields
    EXPECT_FALSE(ringFixReadMemoryLine(rf, fieldsOf("RT,x,E04,1399168770.5")));  // not a station index
    EXPECT_TRUE(rf.links.empty());
    EXPECT_TRUE(rf.last_tree_s.empty());
}

TEST(ChainMemory, EmitterHeldValueStateRoundTripsExactly) {
    const std::vector<std::string> lines{
        "ESP,1,268320.0000195503",
        "ESC,0|E04,0.12345678901234566,-0.30000000000000004,268290.00002",   // each number in its shortest text
        "ESC,1|G05,3,-1,268290",
        "ESR,E04|L1C",
        "EUB,E:L1C/L5Q|E04,1",
        "EUB,G:L1C/L2W|G05,0",
        "ECE,E:L1C/L5Q|E04,0.25,-0.5",
        "ECD,E:L1C/L5Q,0.01,0.02",
        "ECR,E:L1C/L5Q|E04,1.5,4",
        "ECH,E04|L1C,19",
    };
    NetworkProductEmitter em;
    for (const std::string& line : lines) EXPECT_TRUE(em.readHeldValueStateLine(fieldsOf(line))) << line;
    std::ostringstream os;
    em.writeHeldValueState(os);
    EXPECT_EQ(linesOf(os.str()), lines);
    EXPECT_FALSE(em.readHeldValueStateLine(fieldsOf("RL,1,2")));
    EXPECT_FALSE(em.readHeldValueStateLine(fieldsOf("ESC,0|E04,x,1,2")));
}

TEST(ChainMemory, EntityLinesRoundTripExactly) {
    using G  = net::SatSignalState;
    using S  = net::StationState;
    using SS = net::StationSignalState;
    using R  = net::ArcState;
    Ctx a, b;
    a.ctx.entities.names.assign(kRoster);
    b.ctx.entities.names.assign(kRoster);
    net::NetworkEntities& e = a.ctx.entities;
    net::mark(e.sat_signals, *net::satSignalKey("E04_L1C"), &G::eq34_tree_seen);
    net::put(e.sat_signals, *net::satSignalKey("E04_L1C"), &G::eq34_tree_root) = "GOPE00CZE";
    net::put(e.sat_signals, *net::satSignalKey("G05_L2W"), &G::emitted_anchor) = "ONSA00SWE";
    net::put(e.sat_signals, *net::satSignalKey("G05_L2W"), &G::emitted_anchor_s) = 1399168770.5;
    net::mark(e.stations, StationId{1}, &S::signals, std::string("E|L1C"), &SS::eq34_tree_seen);
    net::put(e.stations, StationId{1}, &S::signals, std::string("E|L1C"), &SS::eq34_tree_root) = "GOPE00CZE";
    net::put(e.stations, StationId{2}, &S::signals, std::string("rpb|E|L5Q"), &SS::eq34_root_rpb_target) = -0.125;
    ArcLifeStamp life;
    life.adm_generation = 2;
    life.adm_run_start = 118;
    life.has_carry = true;
    life.birth_ep = 117;
    life.dropped_ep = 3;
    life.relevel_s = 1399165200.0000195;
    life.flag_ep = -1;
    net::putArcAmb(e, 1, "E04_L1C", &R::eq34_pin_life) = life;
    net::putArcAmb(e, 0, "G05_L2W", &R::anchor_arc_age) = 42;
    net::put(e.stations, StationId{2}, &S::topo_prev_tie) = 6;
    net::putLos(e, 1, "E04", &net::LosState::topo_prev_kind) = 3u;
    // A leveling carry with a re-level stamp, and one never re-levelled (no line); the reader stamps the carry the
    // resume restored, so the second state has the carries, unstamped.
    net::putArcAmb(e, 1, "E04_L1C", &R::amb).relevel_s = 1399165230.5;
    net::putArcAmb(e, 2, "E04_L1C", &R::amb);
    net::putArcAmb(b.ctx.entities, 1, "E04_L1C", &R::amb);
    net::putArcAmb(b.ctx.entities, 2, "E04_L1C", &R::amb);
    net::putSat(e, "E04", &net::SatelliteState::code_ref_event_s) = 1399166100.25;

    std::ostringstream os;
    net::chainMemoryWriteEntities(e, os);
    for (const std::string& line : linesOf(os.str()))
        EXPECT_TRUE(net::chainMemoryReadEntityLine(b.ctx.entities, fieldsOf(line))) << line;
    std::ostringstream os2;
    net::chainMemoryWriteEntities(b.ctx.entities, os2);
    EXPECT_EQ(os.str(), os2.str());
    EXPECT_EQ(linesOf(os.str()).size(), 13u);
    const unsigned* kind = net::getLos(b.ctx.entities.los, 1, "E04", &net::LosState::topo_prev_kind);
    ASSERT_NE(kind, nullptr);
    EXPECT_EQ(*kind, 3u);

    const ArcLifeStamp* r = net::getArcAmb(b.ctx.entities.arcs, 1, "E04_L1C", &R::eq34_pin_life);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->relevel_s, life.relevel_s);
    EXPECT_EQ(r->adm_run_start, 118);
    EXPECT_TRUE(r->has_carry);
    EXPECT_EQ(r->flag_ep, -1);
    const double* tgt = net::get(b.ctx.entities.stations, StationId{2}, &S::signals, std::string("rpb|E|L5Q"),
                                 &SS::eq34_root_rpb_target);
    ASSERT_NE(tgt, nullptr);
    EXPECT_EQ(*tgt, -0.125);
    const LocalCarry* lc = net::getArcAmb(b.ctx.entities.arcs, 1, "E04_L1C", &R::amb);
    ASSERT_NE(lc, nullptr);
    EXPECT_EQ(lc->relevel_s, 1399165230.5);
    const LocalCarry* lc2 = net::getArcAmb(b.ctx.entities.arcs, 2, "E04_L1C", &R::amb);
    ASSERT_NE(lc2, nullptr);
    EXPECT_EQ(lc2->relevel_s, 0.0);
    const double* code_event = net::getSat(b.ctx.entities.sats, "E04", &net::SatelliteState::code_ref_event_s);
    ASSERT_NE(code_event, nullptr);
    EXPECT_EQ(*code_event, 1399166100.25);
    EXPECT_FALSE(net::chainMemoryReadEntityLine(b.ctx.entities, fieldsOf("CTR,NOTA00XXX,E|L1C")));   // not in the roster
    EXPECT_FALSE(net::chainMemoryReadEntityLine(b.ctx.entities, fieldsOf("RL,1")));                   // not the chain's
    EXPECT_FALSE(net::chainMemoryReadEntityLine(b.ctx.entities, fieldsOf("CRL,0,G05_L2W,1399165230.5")));   // no carry
}

TEST(ChainMemory, SnapshotCarriesTheSectionVerbatimAndNothingWithoutIt) {
    NetworkStateSnapshot snap;
    snap.snap_week = 2313;
    snap.snap_tow = 279150.0;
    snap.n_emitted = 665;
    snap.config_fingerprint = 0xc5b5df834d185ac8ULL;
    std::ostringstream plain;
    NetworkStateSnapshotIO::serialize(plain, snap);
    EXPECT_EQ(plain.str().find("CHAIN,"), std::string::npos);

    snap.chain_memory =
        "CHAIN,v2,0123456789abcdef\nGW,1399161600,1399172400\nGD,E04,-0.4188\nRF0,1399161600\nCHEND\n";
    std::ostringstream os;
    NetworkStateSnapshotIO::serialize(os, snap);
    std::istringstream is(os.str());
    NetworkStateSnapshot back;
    std::string err;
    ASSERT_TRUE(NetworkStateSnapshotIO::parse(is, back, &err)) << err;
    EXPECT_EQ(back.chain_memory, snap.chain_memory);

    // A section cut before its terminator fails the parse (the cold-start signal).
    std::string cut = os.str();
    const auto at = cut.find("CHEND\n");
    ASSERT_NE(at, std::string::npos);
    cut.erase(at, 6);
    std::istringstream is2(cut);
    NetworkStateSnapshot back2;
    EXPECT_FALSE(NetworkStateSnapshotIO::parse(is2, back2, &err));
}

// ---- The restored pinned lives ---------------------------------------------------------------------

TEST(ChainMemoryRestoredLife, JudgedOnTheResumedRunsOwnLedgerAndFlags) {
    // The writer pinned an arc its ledger admitted at its epoch 118; the resumed run's ledger admits it at its own
    // epoch 0.  Compared as two stamps of one run, the life reads as ended; on the resumed run's own ledger it
    // continues.
    ArcLifeStamp saved;
    saved.adm_generation = 1;
    saved.adm_run_start = 118;
    ArcLifeStamp live;
    live.adm_generation = 1;
    live.adm_run_start = 0;
    EXPECT_FALSE(arcPinnedLifeContinues(saved, live));
    EXPECT_TRUE(arcRestoredLifeContinues(live));
    // At the first epoch's election the ledger has admitted nothing yet.
    const ArcLifeStamp cold;
    EXPECT_FALSE(arcPinnedLifeContinues(saved, cold));
    EXPECT_TRUE(arcRestoredLifeContinues(cold));
    // A flag raised in the resumed run ends it, before or after the ledger's first admission.
    ArcLifeStamp flagged = cold;
    flagged.flag_ep = 0;
    EXPECT_FALSE(arcRestoredLifeContinues(flagged));
    ArcLifeStamp live_flagged = live;
    live_flagged.flag_ep = 3;
    EXPECT_FALSE(arcRestoredLifeContinues(live_flagged));
    // An arc the ledger holds in a gap or dead (admitted before, not live now) ends it.
    ArcLifeStamp gap;
    gap.adm_generation = 1;
    gap.adm_run_start = -1;
    EXPECT_FALSE(arcRestoredLifeContinues(gap));
    // The carry fields of the stamp are not read (a pinned arc's carry is a stale chord carry).
    ArcLifeStamp carry_moved = live;
    carry_moved.has_carry = true;
    carry_moved.birth_ep = 9;
    carry_moved.dropped_ep = 4;
    carry_moved.relevel_s = 1399165230.5;
    EXPECT_TRUE(arcRestoredLifeContinues(carry_moved));
}

// ---- The section's fingerprint ---------------------------------------------------------------------

TEST(ChainMemoryFingerprint, FollowsTheKeysThatShapeTheMemory) {
    const LoadedConfig base  = loadNetworkConfigCapturing("cmfp_base", "");
    const LoadedConfig rf    = loadNetworkConfigCapturing("cmfp_rf", "    \"ring_fix\": { \"wl_frac_max\": 0.2 },\n");
    const LoadedConfig steps = loadNetworkConfigCapturing("cmfp_steps",
                                                          "    \"disc_step_class_pairs\": [ \"G:L1C/L2W\" ],\n");
    const LoadedConfig other = loadNetworkConfigCapturing("cmfp_other", "    \"trace_topology\": true,\n");
    ASSERT_TRUE(base.ok) << base.text;
    ASSERT_TRUE(rf.ok) << rf.text;
    ASSERT_TRUE(steps.ok) << steps.text;
    ASSERT_TRUE(other.ok) << other.text;
    const std::vector<std::string> tokens = networkChainMemoryFingerprintTokens(base.config);
    ASSERT_FALSE(tokens.empty());
    EXPECT_EQ(tokens.front().rfind("rf=", 0), 0u);
    EXPECT_EQ(networkChainMemoryFingerprint(base.config), networkChainMemoryFingerprint(base.config));
    EXPECT_NE(networkChainMemoryFingerprint(rf.config), networkChainMemoryFingerprint(base.config));
    EXPECT_NE(networkChainMemoryFingerprint(steps.config), networkChainMemoryFingerprint(base.config));
    EXPECT_EQ(networkChainMemoryFingerprint(other.config), networkChainMemoryFingerprint(base.config));
    const std::vector<std::string> step_tokens = networkChainMemoryFingerprintTokens(steps.config);
    EXPECT_EQ(step_tokens.back(), "step_pair=G:L1C/L2W");
}

TEST(ChainMemoryFingerprint, TheCaptureAndTheRestoreLeaveTheSnapshotFingerprint) {
    const LoadedConfig off = loadNetworkConfigCapturing("cmfp_off", floatChain(kSnapshotKeys));
    const LoadedConfig on  = loadNetworkConfigCapturing(
        "cmfp_on", floatChain(std::string(kSnapshotKeys) + "    \"state_snapshot_chain_capture\": true,\n"
                                                           "    \"resume_chain_restore\": true,\n"));
    ASSERT_TRUE(off.ok) << off.text;
    ASSERT_TRUE(on.ok) << on.text;
    EXPECT_NE(resumeFingerprint(off.config), 0u);
    EXPECT_EQ(resumeFingerprint(on.config), resumeFingerprint(off.config));
}

// ---- The loader's interlocks -----------------------------------------------------------------------

TEST(ChainMemoryConfig, KeysDefaultOffAndEachHalfNeedsItsDatumHalf) {
    const LoadedConfig def = loadNetworkConfigCapturing("cmcfg_def", "");
    ASSERT_TRUE(def.ok) << def.text;
    EXPECT_FALSE(def.config.getNetworkConfig().state_snapshot_chain_capture);
    EXPECT_FALSE(def.config.networkResumeChainRestore());
    // The capture without the datum capture, and the restore without the datum restore, are refused.
    EXPECT_FALSE(loadNetworkConfigCapturing("cmcfg_cap", "    \"state_snapshot_path\": \"snap.txt\",\n"
                                                         "    \"state_snapshot_chain_capture\": true,\n").ok);
    EXPECT_FALSE(loadNetworkConfigCapturing("cmcfg_res", "    \"state_snapshot_path\": \"snap.txt\",\n"
                                                         "    \"resume_chain_restore\": true,\n").ok);
    const LoadedConfig both = loadNetworkConfigCapturing(
        "cmcfg_both", std::string(kSnapshotKeys) + "    \"state_snapshot_chain_capture\": true,\n"
                                                   "    \"resume_chain_restore\": true,\n");
    ASSERT_TRUE(both.ok) << both.text;
    EXPECT_TRUE(both.config.getNetworkConfig().state_snapshot_chain_capture);
    EXPECT_TRUE(both.config.networkResumeChainRestore());
}

TEST(ChainMemoryConfig, TheRootAndMergeRowsNeedTheCaptureWhenASnapshotIsWritten) {
    const std::string rows = "    \"phase_datum_eq34_event_rows\": [ \"kept\", \"root\", \"merge_split\" ],\n";
    const LoadedConfig without = loadNetworkConfigCapturing("cmrows_off", floatChain(std::string(kSnapshotKeys) + rows));
    ASSERT_TRUE(without.ok) << without.text;
    EXPECT_TRUE(without.config.networkPhaseDatumEq34EventRowArmed("kept"));
    EXPECT_FALSE(without.config.networkPhaseDatumEq34EventRowArmed("root"));
    EXPECT_FALSE(without.config.networkPhaseDatumEq34EventRowArmed("merge_split"));
    EXPECT_TRUE(contains(without.text, "entry 'root' is set with network.state_snapshot_path - dropped"));
    const LoadedConfig with = loadNetworkConfigCapturing(
        "cmrows_on", floatChain(std::string(kSnapshotKeys) + rows + "    \"state_snapshot_chain_capture\": true,\n"));
    ASSERT_TRUE(with.ok) << with.text;
    EXPECT_TRUE(with.config.networkPhaseDatumEq34EventRowArmed("root"));
    EXPECT_TRUE(with.config.networkPhaseDatumEq34EventRowArmed("merge_split"));
}

// ---- The write and the restore ---------------------------------------------------------------------

TEST(ChainMemoryRestore, ASectionWithItsOwnFingerprintIsRestoredAndConsumed) {
    const std::string keys = floatChain(std::string(kSnapshotKeys)
                                        + "    \"phase_datum_eq34_event_rows\": [ \"kept\" ],\n"
                                          "    \"state_snapshot_chain_capture\": true,\n"
                                          "    \"resume_chain_restore\": true,\n");
    const LoadedConfig cfg = loadNetworkConfigCapturing("cmres_ok", keys);
    ASSERT_TRUE(cfg.ok) << cfg.text;

    MemoryRun writer("w_ok", cfg.config);
    ArcLifeStamp life;
    life.adm_generation = 1;
    life.adm_run_start = 118;
    net::putArcAmb(writer.entities(), 1, "E04_L1C", &net::ArcState::eq34_pin_life) = life;
    net::putArcAmb(writer.entities(), 1, "E04_L1C", &net::ArcState::amb).relevel_s = 1399165230.5;
    net::putSat(writer.entities(), "E04", &net::SatelliteState::code_ref_event_s) = 1399166100.25;
    writer.fx->setup().window_t0_s = 1399161600.0;
    writer.fx->setup().window_t1_s = 1399172400.0;
    writer.fx->setup().sat_gf_target_m["E04"] = -0.4188;
    const std::string text = networkChainMemoryWrite(writer.fx->run());
    const std::vector<std::string> lines = linesOf(text);
    ASSERT_GE(lines.size(), 2u);
    EXPECT_EQ(lines.front(), fmt::format("CHAIN,v2,{:016x}", networkChainMemoryFingerprint(cfg.config)));
    EXPECT_EQ(lines.back(), "CHEND");

    MemoryRun leg("r_ok", cfg.config);
    net::putArcAmb(leg.entities(), 1, "E04_L1C", &net::ArcState::amb);   // the carry the resume restored, unstamped
    leg.fx->setup().sat_gf_target_m["E04"] = -0.4000;                   // the leg's own latch
    leg.fx->setup().sat_gf_target_m["G05"] = 0.1000;
    leg.carry().resume.chain_memory = text;                               // what the warm resume staged
    networkChainMemoryRestore(leg.fx->run());
    EXPECT_TRUE(leg.carry().resume.chain_memory.empty());
    EXPECT_TRUE(leg.carry().resume.chain_pin_lives);
    EXPECT_EQ(leg.fx->ctx().counters.chain_restore_restored, 1);
    EXPECT_EQ(leg.fx->ctx().counters.chain_restore_unread, 0);
    // The saved latch replaces the leg's own whole.
    ASSERT_EQ(leg.fx->setup().sat_gf_target_m.size(), 1u);
    EXPECT_EQ(leg.fx->setup().sat_gf_target_m.at("E04"), -0.4188);
    const ArcLifeStamp* r = net::getArcAmb(leg.entities().arcs, 1, "E04_L1C", &net::ArcState::eq34_pin_life);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->adm_run_start, 118);
    const LocalCarry* lc = net::getArcAmb(leg.entities().arcs, 1, "E04_L1C", &net::ArcState::amb);
    ASSERT_NE(lc, nullptr);
    EXPECT_EQ(lc->relevel_s, 1399165230.5);
    const double* code_event = net::getSat(leg.entities().sats, "E04", &net::SatelliteState::code_ref_event_s);
    ASSERT_NE(code_event, nullptr);
    EXPECT_EQ(*code_event, 1399166100.25);
    EXPECT_TRUE(contains(leg.text.str(), "the chain memory section restored at the resume"));
}

TEST(ChainMemoryRestore, AMismatchedSectionRestoresNothingAndTheResumeGoesOn) {
    const std::string common = floatChain(std::string(kSnapshotKeys)
                                          + "    \"phase_datum_eq34_event_rows\": [ \"kept\" ],\n"
                                            "    \"state_snapshot_chain_capture\": true,\n"
                                            "    \"resume_chain_restore\": true,\n");
    const LoadedConfig cfg = loadNetworkConfigCapturing("cmres_w", common);
    const LoadedConfig other = loadNetworkConfigCapturing(
        "cmres_other", common + "    \"ring_fix\": { \"wl_frac_max\": 0.2 },\n");
    ASSERT_TRUE(cfg.ok) << cfg.text;
    ASSERT_TRUE(other.ok) << other.text;

    MemoryRun writer("w_mis", cfg.config);
    ArcLifeStamp life;
    life.adm_generation = 1;
    life.adm_run_start = 118;
    net::putArcAmb(writer.entities(), 1, "E04_L1C", &net::ArcState::eq34_pin_life) = life;
    writer.fx->setup().sat_gf_target_m["E04"] = -0.4188;
    const std::string text = networkChainMemoryWrite(writer.fx->run());

    // Another configuration of the keys that shape the memory: refused, nothing restored.
    MemoryRun refused("r_mis", other.config);
    refused.fx->setup().sat_gf_target_m["E04"] = -0.4000;
    refused.carry().resume.chain_memory = text;
    networkChainMemoryRestore(refused.fx->run());
    EXPECT_EQ(refused.fx->ctx().counters.chain_restore_refused, 1);
    EXPECT_EQ(refused.fx->ctx().counters.chain_restore_restored, 0);
    EXPECT_TRUE(refused.carry().resume.chain_memory.empty());
    EXPECT_FALSE(refused.carry().resume.chain_pin_lives);
    EXPECT_EQ(refused.fx->setup().sat_gf_target_m.at("E04"), -0.4000);
    EXPECT_EQ(net::getArcAmb(refused.entities().arcs, 1, "E04_L1C", &net::ArcState::eq34_pin_life), nullptr);
    EXPECT_TRUE(contains(refused.text.str(), "is REFUSED"));

    // A section of another build's header: refused as well.
    MemoryRun old_build("r_old", cfg.config);
    old_build.carry().resume.chain_memory = "CHAIN,v1\nGD,E04,-0.4188\nCHEND\n";
    networkChainMemoryRestore(old_build.fx->run());
    EXPECT_EQ(old_build.fx->ctx().counters.chain_restore_refused, 1);
    EXPECT_TRUE(old_build.fx->setup().sat_gf_target_m.empty());

    // Nothing staged (a cold start, the restore off, or a snapshot without the section, which the warm resume
    // reports): a no-op that says nothing.
    MemoryRun idle("r_idle", cfg.config);
    idle.fx->setup().sat_gf_target_m["E04"] = -0.4000;
    networkChainMemoryRestore(idle.fx->run());
    EXPECT_EQ(idle.fx->ctx().counters.chain_restore_restored + idle.fx->ctx().counters.chain_restore_refused, 0);
    EXPECT_EQ(idle.fx->setup().sat_gf_target_m.at("E04"), -0.4000);
    EXPECT_FALSE(idle.carry().resume.chain_pin_lives);
    EXPECT_FALSE(contains(idle.text.str(), "chain restore"));
}

TEST(ChainMemoryRestore, TheDumpWritesTheMemoryAsTheLegHoldsIt) {
    // The dump is off by default and inert without the restore.
    const LoadedConfig plain = loadNetworkConfigCapturing("cmdump_off", floatChain(kSnapshotKeys));
    ASSERT_TRUE(plain.ok) << plain.text;
    EXPECT_FALSE(plain.config.networkResumeChainDump());
    const LoadedConfig no_restore = loadNetworkConfigCapturing(
        "cmdump_inert", floatChain(std::string(kSnapshotKeys) + "    \"resume_chain_dump\": true,\n"));
    ASSERT_TRUE(no_restore.ok) << no_restore.text;
    EXPECT_FALSE(no_restore.config.networkResumeChainDump());
    EXPECT_TRUE(contains(no_restore.text, "INERT"));

    // Armed with the restore, the dump is the section the leg restored, line for line, when the leg's window is the
    // writer's (the window line carries the leg's processing window).
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "gnss_chain_dump_case";
    std::filesystem::create_directories(dir);
    const std::string snap = (dir / "snap.txt").generic_string();
    std::string keys = floatChain(std::string(kSnapshotKeys)
                                  + "    \"phase_datum_eq34_event_rows\": [ \"kept\" ],\n"
                                    "    \"state_snapshot_chain_capture\": true,\n"
                                    "    \"resume_chain_restore\": true,\n"
                                    "    \"resume_chain_dump\": true,\n");
    keys.replace(keys.find("snap.txt"), std::string("snap.txt").size(), snap);
    const LoadedConfig cfg = loadNetworkConfigCapturing("cmdump_on", keys);
    ASSERT_TRUE(cfg.ok) << cfg.text;
    EXPECT_TRUE(cfg.config.networkResumeChainDump());

    MemoryRun writer("w_dump", cfg.config);
    ArcLifeStamp life;
    life.adm_generation = 1;
    life.adm_run_start = 118;
    net::putArcAmb(writer.entities(), 1, "E04_L1C", &net::ArcState::eq34_pin_life) = life;
    net::putArcAmb(writer.entities(), 1, "E04_L1C", &net::ArcState::amb).relevel_s = 1399165230.5;
    net::putSat(writer.entities(), "E04", &net::SatelliteState::code_ref_event_s) = 1399166100.25;
    writer.fx->setup().window_t0_s = 1399161600.0;
    writer.fx->setup().window_t1_s = 1399172400.0;
    writer.fx->setup().sat_gf_target_m["E04"] = -0.4188;
    const std::string text = networkChainMemoryWrite(writer.fx->run());

    MemoryRun leg("r_dump", cfg.config);
    net::putArcAmb(leg.entities(), 1, "E04_L1C", &net::ArcState::amb);   // the carry the resume restored, unstamped
    leg.fx->setup().window_t0_s = 1399161600.0;
    leg.fx->setup().window_t1_s = 1399172400.0;
    leg.fx->setup().sat_gf_target_m["E04"] = -0.4000;
    leg.carry().resume.chain_memory = text;
    const std::string dump_path = snap + ".chain_restored";
    std::filesystem::remove(dump_path);
    networkChainMemoryRestore(leg.fx->run());
    std::ifstream in(dump_path, std::ios::binary);
    ASSERT_TRUE(in.good()) << dump_path;
    const std::string dump((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(dump, text);
    EXPECT_TRUE(contains(leg.text.str(), "the restored memory is written to"));
    in.close();
    std::filesystem::remove_all(dir);
}
