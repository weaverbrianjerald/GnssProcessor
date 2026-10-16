/**
 * @file test_network_arc_life.cpp
 * @brief Unit tests of the arc life stamp and the search's track of a life (card AGT, step
 *        AGT-5; include/filter/NetworkArcLife.hpp).
 *
 * Suite @c NetworkArcLife:
 *  - the stamp read from its three sources (the arc ledger's admitted side, the leveling carry,
 *    the held-basis flag map), on its own and through the run context;
 *  - the change mask and the field names;
 *  - the gate of card AGT section 4: exactly one life edge per ledger birth or re-entry, and per
 *    carry erase, drop, re-level or flag, on an epoch loop that runs the owners in the
 *    producer's order (the ledger opens the epoch, the pre-search writers act, the search reads,
 *    then the roster audit admits the rows, the leveling harvest stashes and RAIM acts);
 *  - the echo rule's limits: an erase, a drop or a flag is never an echo.
 *
 * Suite name owned by this file alone (suite-hygiene rule in CLAUDE.md).
 */

#include <gtest/gtest.h>

#include "network/state/NetworkRunContext.hpp"
#include "common/GnssIdentity.hpp"
#include "config/GnssConfig.hpp"
#include "network/state/ArcLedger.hpp"
#include "network/ar/NetworkArcLife.hpp"
#include "network/driver/NetworkRunTypes.hpp"
#include "filter/StateKeySchema.hpp"
#include "logging/LogManager.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace GnssProcessor {
namespace {

namespace fs = std::filesystem;

constexpr double kDt = 30.0;   // the producer's epoch interval

ArcId arcOf(int sta, const char* sat, const char* sig) {
    return ArcId{StationId{sta}, *SatId::parse(sat), *SignalId::fromObsCode(System::GPS, sig)};
}

// ------------------------------------------------------------------ the stamp

TEST(NetworkArcLife, AnAbsentSourceReadsAsItsDefault) {
    const ArcLifeStamp s = arcLifeStampOf(nullptr, nullptr, nullptr);
    EXPECT_EQ(s.adm_generation, 0);
    EXPECT_EQ(s.adm_run_start, -1);
    EXPECT_FALSE(s.has_carry);
    EXPECT_EQ(s.flag_ep, -1);
    EXPECT_EQ(s, ArcLifeStamp{});
}

TEST(NetworkArcLife, TheRunStartChangesOncePerRun) {
    ArcRecord r;
    r.adm_generation       = 1;
    r.adm_state            = ArcState::Tracking;
    r.ep_adm_state_changed = 7;   // born at 7
    EXPECT_EQ(arcLifeStampOf(&r, nullptr, nullptr).adm_run_start, 7);
    r.adm_state            = ArcState::Gap;
    r.ep_adm_state_changed = 12;
    EXPECT_EQ(arcLifeStampOf(&r, nullptr, nullptr).adm_run_start, -1);   // not live
    r.adm_state            = ArcState::Reentered;
    r.ep_adm_state_changed = 15;  // back at 15 ...
    EXPECT_EQ(arcLifeStampOf(&r, nullptr, nullptr).adm_run_start, 16);
    r.adm_state            = ArcState::Tracking;
    r.ep_adm_state_changed = 16;  // ... and Tracking one epoch later, silently: the same run
    EXPECT_EQ(arcLifeStampOf(&r, nullptr, nullptr).adm_run_start, 16);
    EXPECT_EQ(arcLifeStampOf(&r, nullptr, nullptr).adm_generation, 1);
}

TEST(NetworkArcLife, TheCarryFieldsAreReadThroughTheOneReader) {
    LocalCarry lc{};
    lc.birth_ep   = 3;
    lc.birth_s    = 90.0;
    lc.dropped_ep = 8;
    lc.relevel_s  = 240.0;
    const CarryLife life = carryLifeOf(lc);
    EXPECT_EQ(life.birth_ep, 3);
    EXPECT_EQ(life.birth_s, 90.0);
    EXPECT_EQ(life.dropped_ep, 8);
    EXPECT_EQ(life.relevel_s, 240.0);
    const long long flag = 11;
    const ArcLifeStamp s = arcLifeStampOf(nullptr, &lc, &flag);
    EXPECT_TRUE(s.has_carry);
    EXPECT_EQ(s.birth_ep, 3);
    EXPECT_EQ(s.dropped_ep, 8);
    EXPECT_EQ(s.relevel_s, 240.0);
    EXPECT_EQ(s.flag_ep, 11);
}

TEST(NetworkArcLife, TheChangeMaskHasOneBitPerField) {
    const ArcLifeStamp a;
    std::vector<ArcLifeStamp> b(kArcLifeFieldCount, a);
    b[0].adm_generation = 1;
    b[1].adm_run_start  = 4;
    b[2].has_carry      = true;
    b[3].birth_ep       = 2;
    b[4].dropped_ep     = 5;
    b[5].relevel_s      = 1.5;
    b[6].flag_ep        = 9;
    for (int k = 0; k < kArcLifeFieldCount; ++k) {
        EXPECT_EQ(arcLifeChange(a, b[static_cast<std::size_t>(k)]), 1u << k) << arcLifeFieldName(k);
        EXPECT_NE(a, b[static_cast<std::size_t>(k)]);
    }
    EXPECT_EQ(arcLifeChange(a, a), 0u);
    EXPECT_STREQ(arcLifeFieldName(0), "adm_generation");
    EXPECT_STREQ(arcLifeFieldName(6), "flag_ep");
    EXPECT_STREQ(arcLifeFieldName(7), "?");
}

std::string ctxConfig() {
    return "{\n"
           "  \"processing\": { \"mode\": \"ppp-rtk-network\", \"active_systems\": [ \"GPS\" ],\n"
           "                    \"signal_priorities\": { \"GPS\": \"WC\" } },\n"
           "  \"network\": {\n"
           "    \"clock_datum_station\": \"AAAA\",\n"
           "    \"stations\": [\n"
           "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
           "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
           "    ]\n"
           "  }\n"
           "}\n";
}

TEST(NetworkArcLife, TheRunContextIsReadByStationSatelliteAndPhaseCode) {
    LogManager log;
    GnssConfig cfg;
    const fs::path p = fs::temp_directory_path() / "arc_life_ctx.json";
    { std::ofstream f(p); f << ctxConfig(); }
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    std::error_code ec; fs::remove(p, ec);
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());

    const ArcId arc = arcOf(1, "G05", "L2W");
    // The ledger admits (1, G05, L2W) at epoch 4.
    ctx.arc_ledger.beginEpoch(4, 4 * kDt);
    std::vector<std::pair<ArcLedger::Key, ArcStep>> out;
    ctx.arc_ledger.admitEpoch({ArcLedger::Key{1, "G05", "L2W"}}, out);
    LocalCarry lc{};
    lc.birth_ep  = 4;
    lc.relevel_s = 4 * kDt;
    net::putArc(ctx.entities, std::make_tuple(1, std::string("G05"), std::string("L2W")), &net::ArcState::amb) = lc;
    net::putAmbFlagEp(ctx.entities, 1, StateKeys::makeAmbiguityKey("G05", "L2W")) = 3;
    // Another band and another station are other arcs.
    net::putArc(ctx.entities, std::make_tuple(1, std::string("G05"), std::string("L1C")), &net::ArcState::amb) = LocalCarry{};
    net::putAmbFlagEp(ctx.entities, 0, StateKeys::makeAmbiguityKey("G05", "L2W")) = 9;

    const ArcLifeStamp s = arcLifeStamp(ctx, arc);
    EXPECT_EQ(s.adm_generation, 1);
    EXPECT_EQ(s.adm_run_start, 4);
    EXPECT_TRUE(s.has_carry);
    EXPECT_EQ(s.birth_ep, 4);
    EXPECT_EQ(s.relevel_s, 4 * kDt);
    EXPECT_EQ(s.flag_ep, 3);
    const ArcLifeStamp other = arcLifeStamp(ctx, arcOf(0, "G05", "L2W"));
    EXPECT_EQ(other.adm_generation, 0);
    EXPECT_FALSE(other.has_carry);
    EXPECT_EQ(other.flag_ep, 9);
}

// ------------------------------------------------------------------ one life edge per event

/// What happens to the subject arc at one epoch.
struct EpochPlan {
    bool in_pool{true};          ///< Its row is admitted and the search meets it.
    bool free_pre{false};        ///< The free-epoch latch holds it (pre-search; the float vehicle drops it and the held basis flags it).
    bool drop_pre{false};        ///< A pre-search drop alone (the stale-gap drop).
    bool relevel_harvest{false}; ///< The harvest re-levels it with nothing else (the re-level safety net's writer).
    bool raim_erase{false};      ///< RAIM T4 erases its carry (post-search).
    bool raim_drop{false};       ///< RAIM drops it (post-search).
    bool raim_flag{false};       ///< RAIM T4 flags it under the held basis (post-search).
};

/// The producer's owners of an arc's life, run in epoch order for one subject arc (station 0,
/// G05, L1C) and one companion (station 0, G07, L1C) that keeps the station present.
struct LifeLoop {
    /// The ledger's records (step N4f2), on the heap so a returned loop keeps the ledger's
    /// binding valid (the ledger holds a pointer to them).
    std::unique_ptr<net::NetworkEntities>                         ents{std::make_unique<net::NetworkEntities>()};
    ArcLedger                                                     ledger{*ents};
    std::map<std::tuple<int, std::string, std::string>, LocalCarry> carry;
    std::map<std::pair<int, std::string>, long long>              flags;
    std::optional<ArcLifeTrack> last;
    long long                   last_search_ep{0};
    std::vector<ArcLifeStep>    steps;   ///< One per search that met the subject.
    std::vector<long long>      step_ep;

    const ArcLedger::Key subject{0, "G05", "L1C"};
    const ArcLedger::Key companion{0, "G07", "L1C"};

    LocalCarry* subjectCarry() {
        const auto it = carry.find(subject);
        return it == carry.end() ? nullptr : &it->second;
    }

    void epoch(long long e, const EpochPlan& plan) {
        const double t = static_cast<double>(e) * kDt;
        ledger.beginEpoch(e, t);
        // Pre-search writers (PASS A, SB_PREASM).
        if (plan.free_pre) flags[{0, "G05_L1C"}] = e;
        if (plan.free_pre || plan.drop_pre)
            if (LocalCarry* lc = subjectCarry()) { lc->dropped_ep = e; lc->relevel_s = t; }
        // The search (SB_NETAR).
        if (plan.in_pool) {
            const auto fl = flags.find({0, "G05_L1C"});
            const ArcLifeStamp s = arcLifeStampOf(ledger.find(subject), subjectCarry(),
                                                  fl == flags.end() ? nullptr : &fl->second);
            const ArcLifeStep st = arcLifeStep(last ? &*last : nullptr, last_search_ep, s, plan.free_pre, e);
            steps.push_back(st);
            step_ep.push_back(e);
            last = st.track;
        } else {
            last.reset();   // the memory holds only the last search's arcs
        }
        last_search_ep = e;
        // The roster audit admits the epoch's rows.
        std::set<ArcLedger::Key> admitted{companion};
        if (plan.in_pool) admitted.insert(subject);
        std::vector<std::pair<ArcLedger::Key, ArcStep>> out;
        ledger.admitEpoch(admitted, out);
        // The leveling harvest (SB_LEVELING): a first stash, or the latched arc's re-stash.
        if (plan.in_pool) {
            LocalCarry* lc = subjectCarry();
            if (!lc) {
                LocalCarry fresh{};
                fresh.birth_ep  = e;
                fresh.birth_s   = t;
                fresh.relevel_s = t;
                carry[subject] = fresh;
            } else if (plan.free_pre || plan.relevel_harvest) {
                lc->relevel_s = t;
            }
        }
        // RAIM (SB_RAIM).
        if (plan.raim_erase) carry.erase(subject);
        if (plan.raim_drop)
            if (LocalCarry* lc = subjectCarry()) { lc->dropped_ep = e; lc->relevel_s = t; }
        if (plan.raim_flag) flags[{0, "G05_L1C"}] = e;
    }

    /// New lives the search began after epoch @p after.
    int newLivesAfter(long long after) const {
        int n = 0;
        for (std::size_t i = 0; i < steps.size(); ++i)
            if (step_ep[i] > after && steps[i].newLife()) ++n;
        return n;
    }

    ArcLifeEdge edgeAt(long long e) const {
        for (std::size_t i = 0; i < steps.size(); ++i)
            if (step_ep[i] == e) return steps[i].edge;
        return ArcLifeEdge::None;
    }
};

/// Run epochs 1..n with the subject quiet except where @p plan_of says otherwise.
template <class PlanOf>
LifeLoop runLoop(long long n, PlanOf plan_of) {
    LifeLoop L;
    for (long long e = 1; e <= n; ++e) L.epoch(e, plan_of(e));
    return L;
}

TEST(NetworkArcLife, AQuietArcHasOneLifeFromItsFirstSearch) {
    const LifeLoop L = runLoop(12, [](long long) { return EpochPlan{}; });
    EXPECT_EQ(L.edgeAt(1), ArcLifeEdge::FirstSeen);
    // Epoch 2 reads the audit's admission and the harvest's first stash: the echo.
    EXPECT_EQ(L.edgeAt(2), ArcLifeEdge::Echo);
    EXPECT_EQ(L.newLivesAfter(1), 0);
    EXPECT_EQ(L.steps.back().track.life_start, 1);
}

TEST(NetworkArcLife, ALedgerBirthIsOneLifeEdge) {
    const LifeLoop L = runLoop(12, [](long long e) {
        EpochPlan p;
        p.in_pool = e >= 4;   // first admitted and searched at 4
        return p;
    });
    EXPECT_EQ(L.edgeAt(4), ArcLifeEdge::FirstSeen);
    EXPECT_EQ(L.edgeAt(5), ArcLifeEdge::Echo);
    EXPECT_EQ(L.newLivesAfter(0), 1);
}

TEST(NetworkArcLife, ALedgerReentryIsOneLifeEdge) {
    const LifeLoop L = runLoop(14, [](long long e) {
        EpochPlan p;
        p.in_pool = !(e == 6 || e == 7);   // a two-epoch gap, back at 8
        return p;
    });
    EXPECT_EQ(L.edgeAt(8), ArcLifeEdge::FirstSeen);
    EXPECT_EQ(L.edgeAt(9), ArcLifeEdge::Echo);   // the ledger's Reentered catches up
    EXPECT_EQ(L.newLivesAfter(1), 1);
    // The ledger saw the re-entry, and the run start is the same through Reentered and Tracking.
    const ArcRecord* r = L.ledger.find(L.subject);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->adm_generation, 1);
    EXPECT_EQ(arcLifeStampOf(r, nullptr, nullptr).adm_run_start, 9);
}

TEST(NetworkArcLife, ACarryEraseIsOneLifeEdge) {
    const LifeLoop L = runLoop(12, [](long long e) {
        EpochPlan p;
        p.raim_erase = (e == 5);   // RAIM T4 erases after the search of 5
        return p;
    });
    EXPECT_EQ(L.edgeAt(6), ArcLifeEdge::StampChange);   // the carry is gone
    EXPECT_EQ(L.edgeAt(7), ArcLifeEdge::Echo);          // the harvest of 6 re-created it
    EXPECT_EQ(L.newLivesAfter(1), 1);
}

TEST(NetworkArcLife, ADropIsOneLifeEdgeBeforeOrAfterTheSearch) {
    const LifeLoop pre = runLoop(12, [](long long e) {
        EpochPlan p;
        p.drop_pre = (e == 5);
        return p;
    });
    EXPECT_EQ(pre.edgeAt(5), ArcLifeEdge::StampChange);
    EXPECT_EQ(pre.newLivesAfter(1), 1);
    const LifeLoop post = runLoop(12, [](long long e) {
        EpochPlan p;
        p.raim_drop = (e == 5);
        return p;
    });
    EXPECT_EQ(post.edgeAt(6), ArcLifeEdge::StampChange);
    EXPECT_EQ(post.newLivesAfter(1), 1);
}

TEST(NetworkArcLife, AReLevelIsOneLifeEdge) {
    // The latch frees the arc (its drop and its held-basis flag land in the same epoch, before
    // the search) and the harvest re-stashes it after the search, stamped with the same epoch
    // second: one edge.
    const LifeLoop latch = runLoop(12, [](long long e) {
        EpochPlan p;
        p.free_pre = (e == 5);
        return p;
    });
    EXPECT_EQ(latch.edgeAt(5), ArcLifeEdge::Freed);
    EXPECT_NE(latch.edgeAt(6), ArcLifeEdge::StampChange);
    EXPECT_EQ(latch.newLivesAfter(1), 1);
    // A re-level nothing else explains: one edge at the next search.
    const LifeLoop alone = runLoop(12, [](long long e) {
        EpochPlan p;
        p.relevel_harvest = (e == 5);
        return p;
    });
    EXPECT_EQ(alone.edgeAt(6), ArcLifeEdge::StampChange);
    EXPECT_EQ(alone.newLivesAfter(1), 1);
}

TEST(NetworkArcLife, AFlagIsOneLifeEdge) {
    const LifeLoop L = runLoop(12, [](long long e) {
        EpochPlan p;
        p.raim_flag = (e == 5);
        return p;
    });
    EXPECT_EQ(L.edgeAt(6), ArcLifeEdge::StampChange);
    EXPECT_EQ(L.newLivesAfter(1), 1);
}

TEST(NetworkArcLife, AnEraseDropOrFlagRightAfterALifeBeganIsNotAnEcho) {
    // A RAIM erase after the search of an epoch whose latch freed the arc.
    const LifeLoop E = runLoop(12, [](long long e) {
        EpochPlan p;
        p.free_pre   = (e == 5);
        p.raim_erase = (e == 5);
        return p;
    });
    EXPECT_EQ(E.edgeAt(5), ArcLifeEdge::Freed);
    EXPECT_EQ(E.edgeAt(6), ArcLifeEdge::StampChange);
    EXPECT_EQ(E.newLivesAfter(1), 2);
    // A RAIM drop or flag after the search that read a re-level (a life begun at 5).  (The
    // latch's own drop and flag carry its epoch, so a RAIM drop or flag in that same epoch
    // writes the same stamp and is part of the latch's edge.)
    for (int which = 0; which < 2; ++which) {
        const LifeLoop L = runLoop(12, [which](long long e) {
            EpochPlan p;
            p.relevel_harvest = (e == 4);
            p.raim_drop       = (e == 5 && which == 0);
            p.raim_flag       = (e == 5 && which == 1);
            return p;
        });
        EXPECT_EQ(L.edgeAt(5), ArcLifeEdge::StampChange) << which;
        EXPECT_EQ(L.edgeAt(6), ArcLifeEdge::StampChange) << which;
        EXPECT_EQ(L.newLivesAfter(1), 2) << which;
    }
}

TEST(NetworkArcLife, ANewLifeRestartsTheDurabilityAndAnEchoKeepsIt) {
    ArcLifeTrack t;
    t.life_start = 2;
    ArcLifeStamp s;
    s.has_carry = true;
    t.stamp = s;
    // Unchanged: the life continues from epoch 2.
    ArcLifeStep st = arcLifeStep(&t, 9, s, false, 10);
    EXPECT_EQ(st.edge, ArcLifeEdge::None);
    EXPECT_EQ(st.track.life_start, 2);
    EXPECT_FALSE(st.slipped());
    // A drop: a new life at 10, slipped.
    ArcLifeStamp d = s;
    d.dropped_ep = 10;
    st = arcLifeStep(&t, 9, d, false, 10);
    EXPECT_EQ(st.edge, ArcLifeEdge::StampChange);
    EXPECT_TRUE(st.slipped());
    EXPECT_EQ(st.track.life_start, 10);
    EXPECT_EQ(st.change, static_cast<unsigned>(kArcLifeDroppedEp));
    // A re-level read one search after the life began: the echo, not slipped.
    t.life_start = 9;
    ArcLifeStamp r = s;
    r.relevel_s = 270.0;
    st = arcLifeStep(&t, 9, r, false, 10);
    EXPECT_EQ(st.edge, ArcLifeEdge::Echo);
    EXPECT_FALSE(st.slipped());
    EXPECT_FALSE(st.newLife());
    EXPECT_EQ(st.track.life_start, 9);
    EXPECT_EQ(st.track.stamp, r);
    // The same re-level two searches later is a new life.
    st = arcLifeStep(&t, 8, r, false, 10);
    EXPECT_EQ(st.edge, ArcLifeEdge::StampChange);
    // Freed wins over everything, and the first sighting is not slipped.
    EXPECT_EQ(arcLifeStep(&t, 9, s, true, 10).edge, ArcLifeEdge::Freed);
    const ArcLifeStep first = arcLifeStep(nullptr, 9, s, false, 10);
    EXPECT_EQ(first.edge, ArcLifeEdge::FirstSeen);
    EXPECT_FALSE(first.slipped());
    EXPECT_TRUE(first.newLife());
}

}  // namespace
}  // namespace GnssProcessor
