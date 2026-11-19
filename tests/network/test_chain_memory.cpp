/**
 * @file test_chain_memory.cpp
 * @brief The chain memory section of the snapshot (network.state_snapshot_chain_capture): each owner's lines written,
 *        read back into a fresh state and written again give the same text, the values survive exactly, and the
 *        snapshot carries the section verbatim (and nothing without it).
 */
#include <gtest/gtest.h>

#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "config/GnssConfig.hpp"
#include "network/ar/RingFix.hpp"
#include "network/state/Entities.hpp"
#include "network/state/RunContext.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkStateSnapshot.hpp"

using namespace GnssProcessor;

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

}  // namespace

TEST(ChainMemory, RingFixMemoryRoundTripsExactly) {
    NetworkRingFix a;
    a.t0_s = 1399161600.0000195;
    RingFixLink& L = a.links[RingFixLinkId{3, "E04", "L1C", "L5Q"}];
    L.lambda1 = 0.19029367279836487;
    L.lambda2 = 0.25482804879085386;
    L.chord_since_s = 1399165200.0;
    L.last_chord_s = 1399168770.5;
    L.streak = 7;
    L.streak_n1 = -12;
    L.streak_n2 = -9;
    L.held = true;
    L.n1 = -12;
    L.n2 = -9;
    L.ring = 3;
    L.decided_s = 1399166000.25;
    L.stamp1 = 1399160000.0;
    L.stamp2 = -1.0;
    L.root = "WTZR00DEU";
    L.monitor_bad = 2;
    L.monitor_trip = true;
    L.wrong_now = true;
    L.inherited = true;
    L.log_n1 = 5;
    L.log_n2 = 4;
    L.nn_n = 60;
    L.nn_gf_m = 0.4481234567891234;
    L.nn_if_m = -0.0123;
    L.nn_moved = true;
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
    K.released_s = 1399160030.0;
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
    EXPECT_EQ(r.nn_gf_m, L.nn_gf_m);
    EXPECT_EQ(r.decided_s, L.decided_s);
    EXPECT_EQ(r.root, "WTZR00DEU");
    EXPECT_TRUE(r.held);
    EXPECT_EQ(r.n1, -12);
    EXPECT_TRUE(b.links.at(RingFixLinkId{0, "G05", "L1C", "L2W"}).root.empty());
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
    const std::vector<std::string> roster{"WTZR00DEU", "GOPE00CZE", "ONSA00SWE"};
    Ctx a, b;
    a.ctx.entities.names.assign(roster);
    b.ctx.entities.names.assign(roster);
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

    std::ostringstream os;
    net::chainMemoryWriteEntities(e, os);
    for (const std::string& line : linesOf(os.str()))
        EXPECT_TRUE(net::chainMemoryReadEntityLine(b.ctx.entities, fieldsOf(line))) << line;
    std::ostringstream os2;
    net::chainMemoryWriteEntities(b.ctx.entities, os2);
    EXPECT_EQ(os.str(), os2.str());
    EXPECT_EQ(linesOf(os.str()).size(), 11u);
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
    EXPECT_FALSE(net::chainMemoryReadEntityLine(b.ctx.entities, fieldsOf("CTR,NOTA00XXX,E|L1C")));   // not in the roster
    EXPECT_FALSE(net::chainMemoryReadEntityLine(b.ctx.entities, fieldsOf("RL,1")));                   // not the chain's
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

    snap.chain_memory = "CHAIN,v1\nGW,1399161600,1399172400\nGD,E04,-0.4188\nRF0,1399161600\nCHEND\n";
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
