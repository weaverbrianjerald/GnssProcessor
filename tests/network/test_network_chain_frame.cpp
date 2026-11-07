/**
 * @file test_network_chain_frame.cpp
 * @brief The frame observatory of the eq34 chain datum: the chain-event table, the two-band
 *        certification of a chord and the classification of an epoch's tree against the
 *        memory of the last observed epoch.
 */

#include "network/solve/ChainFrame.hpp"

#include "filter/StateKeySchema.hpp"
#include "network/state/RunContext.hpp"   // NetworkKnobs (chainEventRowArmed)
#include "network/solve/ChainDatum.hpp"   // networkChainDatumRunEnd
#include "network/driver/Epoch.hpp"
#include "network/driver/Run.hpp"
#include "network/driver/RunGroups.hpp"
#include "network/driver/SetupResult.hpp"
#include "network/diag/RunTraces.hpp"
#include "network/runend/RunEnd.hpp"
#include "config/GnssConfig.hpp"
#include "../support/NetworkTestRun.hpp"   // the run and epoch the observer takes
#include "../support/TestTempDir.hpp"      // per-process, per-test scratch directory
#include "ConfigLoadCapture.hpp"

#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace GnssProcessor {
namespace {

using TestSupport::contains;

const std::string kGroup = "G|L1C";
constexpr double kLambda = 0.19029367;   // GPS L1 (m)

/// A tree edge of group G|L1C: station @p sta attaches satellite @p sat (or the reverse).
ChainEdgeObs edge(int sta, const std::string& sat, bool sat_is_child, int comp = 0) {
    ChainEdgeObs e;
    e.group = kGroup;
    e.sys = "G";
    e.signal = "L1C";
    e.comp = comp;
    e.r1 = "A";
    e.station = sta;
    e.station_name = std::string(1, static_cast<char>('A' + sta));
    e.sat = sat;
    e.sat_is_child = sat_is_child;
    const std::string rk = "R:" + std::to_string(sta), sk = "S:" + sat;
    e.child = sat_is_child ? sk : rk;
    e.parent = sat_is_child ? rk : sk;
    e.lambda_m = kLambda;
    return e;
}

/// One component of group G|L1C.
ChainComponentObs component(int comp, const std::string& r1, std::vector<int> stations,
                            std::vector<std::string> sats) {
    ChainComponentObs k;
    k.group = kGroup;
    k.sys = "G";
    k.signal = "L1C";
    k.comp = comp;
    k.r1 = r1;
    k.stations = std::move(stations);
    k.sats = std::move(sats);
    k.declared = static_cast<int>(k.stations.size() + k.sats.size());
    return k;
}

/// A memory of one component (index 0, root @p r1) holding the given vertices.
ChainFrameMemory memoryOf(const std::string& r1, const std::vector<std::string>& vertices) {
    ChainFrameMemory m;
    m.have = true;
    for (const std::string& v : vertices) {
        m.vertex_comp[kGroup][v] = 0;
        m.last_seen_s[kGroup][v] = 100.0;
    }
    m.comp_root[kGroup][0] = r1;
    return m;
}

const std::set<std::pair<int, std::string>> kNoFree;
const std::map<std::pair<int, std::string>, std::uint8_t> kNoCause;

TEST(NetworkChainFrame, EventTableHasOneRowPerClassInOrder) {
    const std::set<std::string> entries{"", "kept", "returning", "rehost", "root", "slip", "merge_split"};
    const std::set<std::string> events{"", "kept", "rehost", "root_handover", "returning", "slip_deseat",
                                       "merge", "split"};
    std::set<std::string> names;
    for (std::size_t r = 0; r < kChainEventRowCount; ++r) {
        const ChainEventSpec& s = kChainEventSpec[r];
        EXPECT_EQ(static_cast<std::size_t>(s.row), r) << s.name;
        EXPECT_TRUE(names.insert(s.name).second) << s.name;
        EXPECT_TRUE(entries.count(s.rows_entry)) << s.name;
        EXPECT_TRUE(events.count(s.event)) << s.name;
        EXPECT_NE(std::string(s.legacy_rule), "");
        EXPECT_NE(std::string(s.integer_rule), "");
        EXPECT_EQ(&chainEventSpec(s.row), &s);
    }
    EXPECT_STREQ(chainEventSpec(ChainEventRow::RehostUncertified).event, "rehost");
    EXPECT_EQ(chainEventSpec(ChainEventRow::RehostUncertified).step, ChainStep::CutRelease);
    EXPECT_EQ(chainEventSpec(ChainEventRow::Kept).wire, ChainWire::None);
    EXPECT_STREQ(chainEventSpec(ChainEventRow::Kept).event, "kept");   // written only with the row armed
}

// The loader takes its entries from the table: the distinct non-empty ones, in table order, with
// the rows built so far (the rows are built in table order: kept, returning, rehost, root, slip).
TEST(NetworkChainFrame, EventRowEntriesAreTheTablesAndEveryEntryIsBuilt) {
    EXPECT_EQ(chainEventRowEntries(), (std::vector<std::string>{"kept", "returning", "rehost", "root", "slip",
                                                               "merge_split"}));
    for (const char* entry : {"kept", "returning", "rehost", "root", "slip", "merge_split"})
        EXPECT_TRUE(chainEventRowEntryBuilt(entry)) << entry;
    for (const char* entry : {"", "bogus"})
        EXPECT_FALSE(chainEventRowEntryBuilt(entry)) << entry;
    // An entry two rows share is built when each of its rows is built or held and at least one is built.
    for (const ChainEventSpec& s : kChainEventSpec)
        if (std::string(s.rows_entry).empty()) EXPECT_FALSE(s.built || s.held) << s.name;   // Birth has no entry
    for (const ChainEventSpec& s : kChainEventSpec)
        EXPECT_EQ(s.held, s.row == ChainEventRow::RehostCertified) << s.name;
    EXPECT_TRUE(chainEventSpec(ChainEventRow::RehostUncertified).built);
    EXPECT_FALSE(chainEventSpec(ChainEventRow::RehostCertified).built);   // held: its edges take the other row's rule
    // The knob snapshot arms a row through chainEventRowArmed, and a row not built is never armed.
    NetworkKnobs k{};
    for (std::size_t r = 0; r < kChainEventRowCount; ++r)
        EXPECT_FALSE(chainEventRowArmed(k, kChainEventSpec[r].row)) << kChainEventSpec[r].name;
    k.eq34_row_kept = true;
    for (std::size_t r = 0; r < kChainEventRowCount; ++r)
        EXPECT_EQ(chainEventRowArmed(k, kChainEventSpec[r].row), kChainEventSpec[r].row == ChainEventRow::Kept)
            << kChainEventSpec[r].name;
    k.eq34_row_kept = false;
    k.eq34_row_returning = true;
    for (std::size_t r = 0; r < kChainEventRowCount; ++r)
        EXPECT_EQ(chainEventRowArmed(k, kChainEventSpec[r].row), kChainEventSpec[r].row == ChainEventRow::Returning)
            << kChainEventSpec[r].name;
    EXPECT_STREQ(chainEventSpec(ChainEventRow::Returning).event, "returning");
    // The rehost entry arms both re-host rows (the certified branch is held, not off).
    k.eq34_row_returning = false;
    k.eq34_row_rehost = true;
    for (std::size_t r = 0; r < kChainEventRowCount; ++r)
        EXPECT_EQ(chainEventRowArmed(k, kChainEventSpec[r].row),
                  kChainEventSpec[r].row == ChainEventRow::RehostCertified
                      || kChainEventSpec[r].row == ChainEventRow::RehostUncertified)
            << kChainEventSpec[r].name;
    // The root entry arms the root row alone.
    k.eq34_row_rehost = false;
    k.eq34_row_root = true;
    for (std::size_t r = 0; r < kChainEventRowCount; ++r)
        EXPECT_EQ(chainEventRowArmed(k, kChainEventSpec[r].row), kChainEventSpec[r].row == ChainEventRow::RootHandover)
            << kChainEventSpec[r].name;
    EXPECT_TRUE(chainEventSpec(ChainEventRow::RootHandover).built);
    EXPECT_FALSE(chainEventSpec(ChainEventRow::RootHandover).held);
    EXPECT_STREQ(chainEventSpec(ChainEventRow::RootHandover).event, "root_handover");
    EXPECT_STREQ(chainRootOutcomeName(ChainRootOutcome::Pinned), "pinned");
    EXPECT_STREQ(chainRootOutcomeName(ChainRootOutcome::NotVertex), "not_vertex");
    EXPECT_STREQ(chainRootOutcomeName(ChainRootOutcome::NotKeeper), "not_keeper");
    EXPECT_STREQ(chainRootOutcomeName(ChainRootOutcome::NoCarry), "no_carry");
    EXPECT_STREQ(chainReleaseRowName(ChainReleaseRow::Returning), "returning");
    EXPECT_STREQ(chainReleaseRowName(ChainReleaseRow::Rehost), "rehost");
    // The slip entry arms the slip row alone.
    k.eq34_row_root = false;
    k.eq34_row_slip = true;
    for (std::size_t r = 0; r < kChainEventRowCount; ++r)
        EXPECT_EQ(chainEventRowArmed(k, kChainEventSpec[r].row), kChainEventSpec[r].row == ChainEventRow::TreeEdgeSlip)
            << kChainEventSpec[r].name;
    EXPECT_TRUE(chainEventSpec(ChainEventRow::TreeEdgeSlip).built);
    EXPECT_FALSE(chainEventSpec(ChainEventRow::TreeEdgeSlip).held);
    EXPECT_STREQ(chainEventSpec(ChainEventRow::TreeEdgeSlip).event, "slip_deseat");
    EXPECT_STREQ(chainReleaseRowName(ChainReleaseRow::Slip), "slip");
}

TEST(NetworkChainFrame, CertificationDeterminesAPreciseChordAndNotAnImpreciseOne) {
    ChainChordRow r;
    // sigma 0.01 cycle per band (0.03 after the scale), correlated: both bands determined.
    chainCertifyChord(7.004, -3.003, 1e-4, 0.8e-4, 1e-4, 0.9999, 3.0, 1e4, 0.10, r);
    ASSERT_TRUE(r.have_int_1);
    ASSERT_TRUE(r.have_int_2);
    EXPECT_EQ(r.int_1, 7);
    EXPECT_EQ(r.int_2, -3);
    // sigma 0.5 cycle per band: nothing determined.
    ChainChordRow s;
    chainCertifyChord(7.3, -3.2, 0.25, 0.2, 0.25, 0.9999, 3.0, 1e4, 0.10, s);
    EXPECT_FALSE(s.have_int_1);
    EXPECT_FALSE(s.have_int_2);
    // A degenerate covariance certifies nothing.
    ChainChordRow d;
    chainCertifyChord(1.0, 1.0, 0.0, 0.0, 0.0, 0.9999, 3.0, 1e4, 0.10, d);
    EXPECT_FALSE(d.have_int_1 || d.have_int_2);
}

TEST(NetworkChainFrame, KeptEdgesAndFlips) {
    ChainEpochObs obs;
    obs.armed = true;
    obs.comps.push_back(component(0, "A", {0, 1}, {"G01", "G02"}));
    ChainEdgeObs same = edge(0, "G01", true);
    same.incumbent_same = true;
    same.had_parent = true;
    ChainEdgeObs flip = edge(1, "G02", false);
    flip.incumbent_flipped = true;
    flip.had_parent = true;   // the station had another parent edge as well
    obs.edges = {same, flip};
    ChainFrameCounters cnt;
    const ChainFrameMemory mem = memoryOf("A", {"R:0", "R:1", "S:G01", "S:G02"});
    const ChainEpochClass cls = chainClassifyEpoch(obs, mem, kNoFree, kNoCause, cnt);
    ASSERT_EQ(cls.edges.size(), 2u);
    EXPECT_EQ(cls.edges[0].row, ChainEventRow::Kept);
    EXPECT_EQ(cls.edges[1].row, ChainEventRow::Kept);
    EXPECT_EQ(cnt.cf_flipped, 1);
    EXPECT_EQ(cnt.cf_rows[static_cast<std::size_t>(ChainEventRow::Kept)], 2);
    EXPECT_EQ(cnt.cf_rehosts, 0);
    EXPECT_EQ(cnt.cf_rows_armed[static_cast<std::size_t>(ChainEventRow::Kept)], 0);   // no row armed
}

// An edge the kept row held is counted under the row it took, and its step is the one realized.
TEST(NetworkChainFrame, AnEdgeAnArmedRowDecidedIsCountedAsArmedWithItsRealizedStep) {
    ChainEpochObs obs;
    obs.armed = true;
    obs.comps.push_back(component(0, "A", {0, 1}, {"G01", "G02"}));
    ChainEdgeObs held = edge(0, "G01", true);
    held.incumbent_same = true;
    held.had_parent = true;
    held.row_armed = true;
    held.legacy_known = false;            // the as-built rule would have declared the step unknown ...
    held.legacy_pinmove_m = 0.03;
    held.known = true;                    // ... the row realized a known step of 0
    held.pinmove_m = 0.0;
    ChainEdgeObs plain = edge(1, "G02", false);
    plain.incumbent_same = true;
    plain.had_parent = true;              // an incumbent edge the row did not hold
    plain.known = false;                  // the as-built step stands, unknown
    obs.edges = {held, plain};
    ChainFrameCounters cnt;
    const ChainFrameMemory mem = memoryOf("A", {"R:0", "R:1", "S:G01", "S:G02"});
    const ChainEpochClass cls = chainClassifyEpoch(obs, mem, kNoFree, kNoCause, cnt);
    const std::size_t kept = static_cast<std::size_t>(ChainEventRow::Kept);
    ASSERT_EQ(cls.edges.size(), 2u);
    EXPECT_EQ(cls.edges[0].row, ChainEventRow::Kept);
    EXPECT_EQ(cls.edges[1].row, ChainEventRow::Kept);
    EXPECT_EQ(cnt.cf_rows[kept], 2);
    EXPECT_EQ(cnt.cf_rows_armed[kept], 1);       // the held edge only
    EXPECT_EQ(cnt.cf_rows_unknown[kept], 1);     // the realized steps: the edge the row did not hold
}

// The observer writes a kept row for a held edge only where the as-built rule would have moved it.
TEST(NetworkChainFrame, AHeldEdgeWritesARowOnlyWhereTheAsBuiltRuleWouldHaveMovedIt) {
    ChainEdgeObs e = edge(0, "G01", true);
    e.row_armed = true;
    EXPECT_FALSE(chainKeptEdgeMoved(e, nullptr));    // held, the as-built step 0 and known, no flip: nothing to say
    ChainEdgeObs unknown = e;
    unknown.legacy_known = false;
    EXPECT_TRUE(chainKeptEdgeMoved(unknown, nullptr));
    ChainEdgeObs stepped = e;
    stepped.legacy_pinmove_m = -0.04;
    EXPECT_TRUE(chainKeptEdgeMoved(stepped, nullptr));   // not held at the last observed epoch: against the realized target
    ChainEdgeObs flipped = e;
    flipped.incumbent_flipped = true;
    EXPECT_TRUE(chainKeptEdgeMoved(flipped, nullptr));
    // An edge no row decided writes none, whatever the as-built facts say.
    ChainEdgeObs unarmed = flipped;
    unarmed.row_armed = false;
    unarmed.legacy_known = false;
    EXPECT_FALSE(chainKeptEdgeMoved(unarmed, nullptr));
}

// A held arc whose as-built target differs from the frozen one keeps differing at every election, but
// the as-built rule would have moved the edge once, when its target changed.  Its step is measured
// against the as-built target of the last observed epoch, so the persistent difference is one event.
TEST(NetworkChainFrame, APersistentDifferenceFromTheFrozenTargetIsOneEventAtItsOnset) {
    const double frozen = 5.0 * kLambda;
    ChainEdgeObs e = edge(0, "G01", true);
    e.row_armed = true;
    e.target_m = e.prev_target_m = frozen;   // what the kept row realized
    e.legacy_target_m = 0.0;                 // the chord carry is pruned: the as-built rule re-reads 0 ...
    e.legacy_pinmove_m = 0.0 - frozen;       // ... against the frozen target, at every election
    // Not held at the last observed epoch: the as-built rule elected the arc then, so its previous
    // target is the realized one and the step is the draft's own displacement.
    EXPECT_TRUE(chainKeptEdgeMoved(e, nullptr));
    EXPECT_DOUBLE_EQ(chainKeptAsBuiltStep(e, nullptr), -frozen);
    // Held then, the as-built target at that epoch still the frozen value: the target falls now.
    const double before = frozen;
    EXPECT_TRUE(chainKeptEdgeMoved(e, &before));
    EXPECT_DOUBLE_EQ(chainKeptAsBuiltStep(e, &before), -frozen);
    // Held then, the as-built target already 0: the difference persists but the as-built rule would not move.
    const double zero = 0.0;
    EXPECT_FALSE(chainKeptEdgeMoved(e, &zero));
    EXPECT_EQ(chainKeptAsBuiltStep(e, &zero), 0.0);
    // The as-built target changes a second time.
    e.legacy_target_m = 2.0 * kLambda;
    e.legacy_pinmove_m = 2.0 * kLambda - frozen;
    EXPECT_TRUE(chainKeptEdgeMoved(e, &zero));
    EXPECT_DOUBLE_EQ(chainKeptAsBuiltStep(e, &zero), 2.0 * kLambda);
    // An unknown as-built step has no value and always writes a row.
    ChainEdgeObs unknown = e;
    unknown.legacy_known = false;
    EXPECT_TRUE(std::isnan(chainKeptAsBuiltStep(unknown, &zero)));
    EXPECT_TRUE(chainKeptEdgeMoved(unknown, &zero));
    // A reversed arc is a host change for the as-built rule: its displacement does not depend on a
    // previous target, and it always writes its pair.
    ChainEdgeObs flipped = e;
    flipped.incumbent_flipped = true;
    flipped.legacy_target_m = 0.0;           // the same as-built target as remembered
    flipped.legacy_pinmove_m = -0.0123;
    EXPECT_DOUBLE_EQ(chainKeptAsBuiltStep(flipped, &zero), -0.0123);
    EXPECT_TRUE(chainKeptEdgeMoved(flipped, &zero));
    // An edge no row decided has no step to move.
    ChainEdgeObs unarmed = e;
    unarmed.row_armed = false;
    EXPECT_FALSE(chainKeptEdgeMoved(unarmed, &zero));
}

// The memory keeps the as-built target of every edge the kept row held, replaced at each observed
// epoch: an edge the row did not hold, an arc no longer held and a run with no row leave nothing.
TEST(NetworkChainFrame, RememberKeepsTheAsBuiltTargetsOfTheHeldEdgesAndReplacesThem) {
    using Key = std::tuple<int, std::string, std::string>;
    ChainFrameCounters cnt;
    const auto remember = [&](std::vector<ChainEdgeObs> edges, double tow) {
        ChainEpochObs obs;
        obs.armed = true;
        obs.comps.push_back(component(0, "A", {0, 1}, {"G01", "G02"}));
        obs.edges = std::move(edges);
        const ChainFrameMemory none;
        const ChainFrameMemory& mem = cnt.chain_frame_memory ? *cnt.chain_frame_memory : none;
        const ChainEpochClass cls = chainClassifyEpoch(obs, mem, kNoFree, kNoCause, cnt);
        chainRememberEpoch(obs, cls, {}, 2313, tow, tow, cnt);
        return cnt.chain_frame_memory->kept_as_built_target;
    };
    ChainEdgeObs held = edge(0, "G01", true);
    held.incumbent_same = true;
    held.row_armed = true;
    held.legacy_target_m = 0.5;
    ChainEdgeObs plain = edge(1, "G02", false);      // an incumbent edge the row did not hold
    plain.incumbent_same = true;
    plain.legacy_target_m = 0.7;
    EXPECT_EQ(remember({held, plain}, 30.0), (std::map<Key, double>{{Key{0, "G01", "L1C"}, 0.5}}));
    // The next epoch holds the other arc and no longer the first: the memory is replaced, not added to.
    ChainEdgeObs no_longer = held;
    no_longer.row_armed = false;
    ChainEdgeObs other = plain;
    other.row_armed = true;
    other.legacy_target_m = -0.25;
    EXPECT_EQ(remember({no_longer, other}, 60.0), (std::map<Key, double>{{Key{1, "G02", "L1C"}, -0.25}}));
    // An epoch in which the row holds nothing empties it, as does a run with the row off.
    EXPECT_TRUE(remember({no_longer, plain}, 90.0).empty());
}

TEST(NetworkChainFrame, RememberKeepsTheRealizedTargetOfAReleasedEdgeSoTheNextHoldIsQuiet) {
    using Key = std::tuple<int, std::string, std::string>;
    ChainFrameCounters cnt;
    const auto observe = [&](std::vector<std::string> sats, std::vector<ChainEdgeObs> edges,
                             std::vector<ChainReleaseObs> releases, double tow) {
        ChainEpochObs obs;
        obs.armed = true;
        obs.comps.push_back(component(0, "A", {0, 1}, std::move(sats)));
        obs.edges = std::move(edges);
        obs.releases = std::move(releases);
        const ChainFrameMemory none;
        const ChainFrameMemory& mem = cnt.chain_frame_memory ? *cnt.chain_frame_memory : none;
        ChainEpochClass cls = chainClassifyEpoch(obs, mem, kNoFree, kNoCause, cnt);
        chainRememberEpoch(obs, cls, {}, 2313, tow, tow, cnt);
        return cls;
    };
    // G02 is a vertex, leaves, and returns under B: the returning row releases its edge with the
    // first-sighting target 0, where the as-built draft read the old-frame carry (37 cycles).
    observe({"G01", "G02"}, {edge(0, "G01", true), edge(1, "G02", true)}, {}, 30.0);
    observe({"G01"}, {edge(0, "G01", true)}, {}, 60.0);
    ChainEdgeObs held = edge(0, "G01", true);
    held.incumbent_same = true;
    held.row_armed = true;
    held.legacy_target_m = 0.5;
    held.target_m = 0.5;
    ChainEdgeObs released = edge(1, "G02", true);
    released.row_armed = true;
    released.legacy_target_m = 37.0 * kLambda;
    released.target_m = 0.0;
    ChainReleaseObs r;
    r.group = kGroup;
    r.signal = "L1C";
    r.station = 1;
    r.sat = "G02";
    r.vertex = "S:G02";
    r.sats = {"G02"};
    const ChainEpochClass cls = observe({"G01", "G02"}, {held, released}, {r}, 90.0);
    ASSERT_EQ(cls.edges.size(), 2u);
    EXPECT_EQ(cls.edges[1].row, ChainEventRow::Returning);
    // The held edge keeps the as-built draft; the released edge keeps the target it realized.
    EXPECT_EQ(cnt.chain_frame_memory->kept_as_built_target,
              (std::map<Key, double>{{Key{0, "G01", "L1C"}, 0.5}, {Key{1, "G02", "L1C"}, 0.0}}));
    // At the next election the kept row holds the edge and the as-built draft reads the carry of the
    // released target: no as-built step, so no hold row (against the old-frame draft it would be one).
    ChainEdgeObs next = edge(1, "G02", true);
    next.incumbent_same = true;
    next.row_armed = true;
    next.legacy_target_m = 0.0;
    const double& remembered = cnt.chain_frame_memory->kept_as_built_target.at(Key{1, "G02", "L1C"});
    EXPECT_EQ(chainKeptAsBuiltStep(next, &remembered), 0.0);
    EXPECT_FALSE(chainKeptEdgeMoved(next, &remembered));
    const double old_frame = 37.0 * kLambda;
    EXPECT_TRUE(chainKeptEdgeMoved(next, &old_frame));
}

TEST(NetworkChainFrame, RehostIsCertifiedFromThePreviousChordTable) {
    ChainEpochObs obs;
    obs.armed = true;
    obs.comps.push_back(component(0, "A", {0, 1}, {"G01", "G02"}));
    ChainEdgeObs e = edge(1, "G02", true);
    e.had_parent = true;
    e.prev_parent = "R:0";
    obs.edges = {e};
    ChainFrameMemory mem = memoryOf("A", {"R:0", "R:1", "S:G01", "S:G02"});
    ChainChordRow c;
    c.station = 1;
    c.sat = "G02";
    c.sig1 = "L1C";
    c.sig2 = "L2W";
    c.float_cyc_1 = 5.2;   // the band's own nearest integer is 5
    c.have_int_1 = true;
    c.int_1 = 6;
    mem.chords[{1, "G02", "L1C"}] = {c, 1};
    ChainFrameCounters cnt;
    ChainEpochClass cls = chainClassifyEpoch(obs, mem, kNoFree, kNoCause, cnt);
    ASSERT_EQ(cls.edges.size(), 1u);
    EXPECT_EQ(cls.edges[0].row, ChainEventRow::RehostCertified);
    EXPECT_TRUE(cls.edges[0].certified);
    EXPECT_EQ(cls.edges[0].cert_source, ChainCertSource::Pair);
    EXPECT_EQ(cls.edges[0].cert_int, 6);
    EXPECT_EQ(cnt.cf_wrong_cand, 1);   // 6 is not the nearest integer of 5.2
    EXPECT_EQ(cnt.cf_cert[static_cast<std::size_t>(ChainCertSource::Pair)], 1);

    // The lattice implies another integer: a conflict, counted uncertified.
    mem.chords[{1, "G02", "L1C"}].first.have_lat_1 = true;
    mem.chords[{1, "G02", "L1C"}].first.lat_1 = 5;
    ChainFrameCounters cnt2;
    cls = chainClassifyEpoch(obs, mem, kNoFree, kNoCause, cnt2);
    EXPECT_EQ(cls.edges[0].row, ChainEventRow::RehostUncertified);
    EXPECT_TRUE(cls.edges[0].conflict);
    EXPECT_EQ(cnt2.cf_conflicts, 1);
    EXPECT_EQ(cnt2.cf_cert[static_cast<std::size_t>(ChainCertSource::None)], 1);

    // No chord table entry: uncertified.
    mem.chords.clear();
    ChainFrameCounters cnt3;
    cls = chainClassifyEpoch(obs, mem, kNoFree, kNoCause, cnt3);
    EXPECT_EQ(cls.edges[0].row, ChainEventRow::RehostUncertified);
    EXPECT_FALSE(cls.edges[0].certified);
}

TEST(NetworkChainFrame, BirthReturningSlipAndMerge) {
    ChainEpochObs obs;
    obs.armed = true;
    obs.comps.push_back(component(0, "A", {0, 1, 2}, {"G01", "G03", "G04", "G05"}));
    ChainEdgeObs returning = edge(0, "G03", true);    // seen earlier, absent at the last epoch
    ChainEdgeObs birth = edge(0, "G04", true);        // never seen
    ChainEdgeObs slip = edge(0, "G01", true);         // the latch names its arc
    slip.incumbent_same = true;
    ChainEdgeObs merge = edge(2, "G05", false);       // joins two previous components
    obs.edges = {returning, birth, slip, merge};
    ChainFrameMemory mem = memoryOf("A", {"R:0", "R:1", "S:G01", "S:G05"});
    mem.vertex_comp[kGroup]["R:2"] = 1;               // a second previous component
    mem.comp_root[kGroup][1] = "C";
    mem.vertex_comp[kGroup]["S:G05"] = 0;
    mem.last_seen_s[kGroup]["S:G03"] = 50.0;          // seen once, not at the last epoch
    const std::set<std::pair<int, std::string>> free{{0, StateKeys::makeAmbiguityKey("G01", "L1C")}};
    const std::map<std::pair<int, std::string>, std::uint8_t> cause{
        {{0, StateKeys::makeAmbiguityKey("G01", "L1C")}, std::uint8_t{4}}};
    ChainFrameCounters cnt;
    const ChainEpochClass cls = chainClassifyEpoch(obs, mem, free, cause, cnt);
    ASSERT_EQ(cls.edges.size(), 4u);
    EXPECT_EQ(cls.edges[0].row, ChainEventRow::Returning);
    EXPECT_TRUE(cls.edges[0].returning);
    EXPECT_EQ(cls.edges[1].row, ChainEventRow::Birth);
    EXPECT_EQ(cls.edges[2].row, ChainEventRow::TreeEdgeSlip);
    EXPECT_EQ(cls.edges[2].slip_cause, 4);
    EXPECT_EQ(cls.edges[3].row, ChainEventRow::Merge);
    ASSERT_EQ(cls.comps.size(), 1u);
    EXPECT_EQ(cls.comps[0].merged, 1);
    EXPECT_EQ(cnt.cf_merges, 1);
    EXPECT_EQ(cnt.cf_returning, 1);
}

TEST(NetworkChainFrame, RootHandoverAndSplit) {
    // One previous component rooted at A holding stations 0, 1 and satellites G01, G02.
    const ChainFrameMemory mem = memoryOf("A", {"R:0", "R:1", "S:G01", "S:G02"});
    {
        // It splits: A keeps G01; station 1 and G02 form a piece rooted at B.
        ChainEpochObs obs;
        obs.armed = true;
        obs.comps.push_back(component(0, "A", {0}, {"G01"}));
        obs.comps.push_back(component(1, "B", {1}, {"G02"}));
        ChainEdgeObs kept = edge(1, "G02", true, 1);
        kept.incumbent_same = true;
        kept.root_unknown = true;
        obs.edges = {kept};
        ChainFrameCounters cnt;
        const ChainEpochClass cls = chainClassifyEpoch(obs, mem, kNoFree, kNoCause, cnt);
        ASSERT_EQ(cls.comps.size(), 2u);
        EXPECT_FALSE(cls.comps[0].split);
        EXPECT_TRUE(cls.comps[1].split);
        EXPECT_FALSE(cls.comps[1].root_changed);
        EXPECT_EQ(cls.comps[1].old_root, "A");
        EXPECT_EQ(cls.edges[0].row, ChainEventRow::Split);
        EXPECT_EQ(cnt.cf_splits, 1);
        EXPECT_EQ(cnt.cf_root_handovers, 0);
    }
    {
        // The same component re-roots at B.
        ChainEpochObs obs;
        obs.armed = true;
        obs.comps.push_back(component(0, "B", {0, 1}, {"G01", "G02"}));
        ChainEdgeObs kept = edge(1, "G02", true);
        kept.incumbent_same = true;
        kept.root_unknown = true;
        obs.edges = {kept};
        ChainFrameCounters cnt;
        const ChainEpochClass cls = chainClassifyEpoch(obs, mem, kNoFree, kNoCause, cnt);
        EXPECT_TRUE(cls.comps[0].root_changed);
        EXPECT_EQ(cls.comps[0].old_root, "A");
        EXPECT_EQ(cls.edges[0].row, ChainEventRow::RootHandover);
        EXPECT_EQ(cnt.cf_root_handovers, 1);
    }
}

TEST(NetworkChainFrame, RememberReplacesTheMemoryAndCountsDepartures) {
    ChainFrameCounters cnt;
    cnt.chain_frame_memory = std::make_shared<ChainFrameMemory>(memoryOf("A", {"R:0", "R:1", "S:G01", "S:G02"}));
    ChainEpochObs obs;
    obs.armed = true;
    obs.comps.push_back(component(0, "A", {0}, {"G01"}));
    obs.edges = {edge(0, "G01", true)};
    const ChainEpochClass cls = chainClassifyEpoch(obs, *cnt.chain_frame_memory, kNoFree, kNoCause, cnt);
    chainRememberEpoch(obs, cls, {}, 2313, 300.0, 200.0, cnt);
    ASSERT_TRUE(cnt.chain_frame_memory);
    const ChainFrameMemory& m = *cnt.chain_frame_memory;
    EXPECT_TRUE(m.have);
    EXPECT_EQ(m.vertex_comp.at(kGroup).size(), 2u);
    EXPECT_EQ(m.vertex_parent.at(kGroup).at("S:G01"), "R:0");
    EXPECT_DOUBLE_EQ(m.last_seen_s.at(kGroup).at("S:G01"), 200.0);
    EXPECT_DOUBLE_EQ(m.last_seen_s.at(kGroup).at("S:G02"), 100.0);   // kept from before
    EXPECT_EQ(cnt.cf_departed, 2);   // R:1 and S:G02
    EXPECT_EQ(cnt.cf_epochs, 1);
}

// ---- The observer and the run end of an armed row ------------------------------------------------

namespace fs = std::filesystem;

/// A run on the float chain with the chain trace and the events trace open and the kept row armed or
/// not, a logger that captures what the run-end reports print, and stations "A", "B", "C".
struct ChainObserverRun {
    TestSupport::LoadedConfig       loaded;
    std::ostringstream              log_text;
    std::shared_ptr<spdlog::logger> log;
    fs::path                        trace_dir;
    std::unique_ptr<NetworkTestRun> fx;

    /// The kept row armed or not.
    ChainObserverRun(const std::string& tag, bool armed)
        : ChainObserverRun(tag, armed ? std::vector<std::string>{"kept"} : std::vector<std::string>{}) {}

    /// The given entries of network.phase_datum_eq34_event_rows armed (none: every row as built).
    ChainObserverRun(const std::string& tag, const std::vector<std::string>& rows)
        : loaded(TestSupport::loadNetworkConfigCapturing(
              "chain_obs_" + tag,
              std::string("    \"phase_leveling\": true,\n"
                          "    \"shared_carry_full_cov\": true,\n"
                          "    \"carry_conditional_coupling\": true,\n"
                          "    \"ambiguity_vehicle\": \"float_kf\",\n"
                          "    \"phase_datum_eq34\": true,\n"
                          "    \"trace_chain_datum\": true,\n"
                          "    \"trace_events\": true,\n")
                  + rowsEntry(rows))) {
        EXPECT_TRUE(loaded.ok) << loaded.text;
        auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(log_text);
        log = std::make_shared<spdlog::logger>("chain_obs_" + tag, sink);
        log->set_level(spdlog::level::info);
        trace_dir = gnss_test::tempDir() / ("chain_obs_" + tag);
        fs::create_directories(trace_dir);
        fx = std::make_unique<NetworkTestRun>(loaded.config, log, trace_dir);
        fx->traces().openAll(loaded.config, loaded.config.getNetworkConfig(),
                             loaded.config.getGlobalVtecShConfig(), /*sh_model=*/nullptr,
                             /*cl_trace_armed=*/false, log);
        for (const char* name : {"A", "B", "C"}) {
            fx->setup().stations.emplace_back();
            fx->setup().stations.back().name = name;
        }
    }
    ~ChainObserverRun() {
        fx->traces().closeAll();
        std::error_code ec;
        fs::remove_all(trace_dir, ec);
    }

    /// Observe one epoch whose election recorded @p edges in one component.
    void observe(double tow, std::vector<ChainEdgeObs> edges) {
        auto rec = std::make_shared<ChainEpochObs>();
        rec->armed = true;
        rec->comps.push_back(component(0, "A", {0, 1, 2}, {"G01", "G02", "G03"}));
        rec->edges = std::move(edges);
        NetworkEpoch& e = fx->newEpoch(GnssTime{2313, tow});
        e.events.phase_datum.chain = rec;
        networkChainFrameObserve(fx->run(), e);
    }

    /// Observe one epoch whose election recorded @p rec, at the time @p tow (the epoch's clock too, so a
    /// vertex's gap since it was last seen is measured).
    void observeRecord(double tow, std::shared_ptr<ChainEpochObs> rec) {
        NetworkEpoch& e = fx->newEpoch(GnssTime{2313, tow});
        e.clock.pl_now_s = tow;
        e.events.phase_datum.chain = std::move(rec);
        networkChainFrameObserve(fx->run(), e);
    }

    /// The config line arming @p rows ("" for none).
    static std::string rowsEntry(const std::vector<std::string>& rows) {
        if (rows.empty()) return std::string();
        std::string s = "    \"phase_datum_eq34_event_rows\": [";
        for (std::size_t i = 0; i < rows.size(); ++i) s += (i ? ", \"" : "\"") + rows[i] + "\"";
        return s + "],\n";
    }

    /// The rows of a trace file of the run (header first), split at the commas; the traces are
    /// flushed first.
    std::vector<std::vector<std::string>> rows(const std::string& file) {
        fx->traces().closeAll();
        fx->traces().chain_edges_trace.flush();   // closeAll leaves the chain trace to the destructor
        std::ifstream f(trace_dir / file);
        std::vector<std::vector<std::string>> out;
        for (std::string line; std::getline(f, line);) {
            std::vector<std::string> cells;
            std::stringstream ss(line);
            for (std::string c; std::getline(ss, c, ',');) cells.push_back(c);
            out.push_back(std::move(cells));
        }
        return out;
    }

    /// What the run-end reports print, with @p report run over the run.
    std::string runEnd(int (*report)(const NetworkRunEndView&)) {
        NetworkRun& run = fx->run();
        const NetworkRunEndInputs inputs{run.engines, run.emit, run.timing, run.closed_loop};
        const NetworkRunEndView view{fx->ctx(), fx->config(), log, fx->traces(), fx->setup(), inputs,
                                     /*loop_ms=*/0.0};
        EXPECT_EQ(report(view), 0);
        log->flush();
        return log_text.str();
    }
};

/// The column of a trace file by its header name (-1: none).
int columnOf(const std::vector<std::string>& header, const std::string& name) {
    for (std::size_t i = 0; i < header.size(); ++i)
        if (header[i] == name) return static_cast<int>(i);
    return -1;
}

/// An incumbent edge in the same direction the kept row held.
ChainEdgeObs heldEdge(int sta, const std::string& sat, bool sat_is_child) {
    ChainEdgeObs e = edge(sta, sat, sat_is_child);
    e.had_parent = true;
    e.incumbent_same = true;
    e.row_armed = true;
    return e;
}

// The edges of an armed row are marked in the edge trace, and the event trace says what the as-built
// rule would have done to the held edges it would have moved, and says nothing of the others.
TEST(NetworkChainFrame, TheObserverMarksTheHeldEdgesAndWritesKeptRowsOnlyWhereTheAsBuiltRuleWouldHaveMovedThem) {
    ChainObserverRun r("rows", /*armed=*/true);
    ASSERT_TRUE(r.fx->ctx().knobs.eq34_row_kept);
    ChainEdgeObs quiet = heldEdge(0, "G01", true);          // held, and the as-built step was zero and known
    ChainEdgeObs unknown = heldEdge(1, "G02", true);        // held, the as-built step would have been unknown
    unknown.legacy_known = false;
    unknown.cause = ChainLegacyCause::RehostCold;
    ChainEdgeObs flipped = heldEdge(2, "G02", false);       // held, the arc reversed: C under G02
    flipped.incumbent_same = false;
    flipped.incumbent_flipped = true;
    flipped.legacy_pinmove_m = 0.0123;
    flipped.legacy_target_m = 3.0 * kLambda;
    flipped.cause = ChainLegacyCause::RehostCarry;
    ChainEdgeObs plain = edge(1, "G03", true);              // an incumbent edge the row did not hold
    plain.had_parent = true;
    plain.incumbent_same = true;
    plain.known = false;
    r.observe(261060.0, {quiet, unknown, flipped, plain});

    const ChainFrameCounters& c = r.fx->ctx().counters;
    const std::size_t kept = static_cast<std::size_t>(ChainEventRow::Kept);
    EXPECT_EQ(c.cf_rows[kept], 4);
    EXPECT_EQ(c.cf_rows_armed[kept], 3);
    EXPECT_EQ(c.cf_flipped, 1);
    EXPECT_EQ(c.cf_events, 3);   // one hold row, and a reversed arc's leave and join
    EXPECT_EQ(c.cf_kept_reversed, 1);
    EXPECT_EQ(c.cf_kept_hold_unknown, 1);
    EXPECT_EQ(c.cf_kept_hold_whole + c.cf_kept_hold_other, 0);

    const auto edges = r.rows("network_chain_edges.csv");
    ASSERT_EQ(edges.size(), 5u);   // the header and four edges
    const int armed = columnOf(edges[0], "row_armed"), row = columnOf(edges[0], "row"),
              sat = columnOf(edges[0], "edge_sat"), known = columnOf(edges[0], "known");
    ASSERT_TRUE(armed >= 0 && row >= 0 && sat >= 0 && known >= 0);
    const std::vector<std::string> want_armed{"1", "1", "1", "0"};
    for (std::size_t k = 0; k < 4; ++k) {
        EXPECT_EQ(edges[k + 1][static_cast<std::size_t>(armed)], want_armed[k]) << k;
        EXPECT_EQ(edges[k + 1][static_cast<std::size_t>(row)], "Kept") << k;
    }
    EXPECT_EQ(edges[4][static_cast<std::size_t>(known)], "0");   // the realized step of the edge not held

    const auto events = r.rows("network_events.csv");
    const int tier = columnOf(events[0], "tier"), event = columnOf(events[0], "event"),
              action = columnOf(events[0], "action"), detail = columnOf(events[0], "detail"),
              station = columnOf(events[0], "station");
    ASSERT_TRUE(tier >= 0 && event >= 0 && action >= 0 && detail >= 0 && station >= 0);
    std::vector<std::vector<std::string>> chain;
    for (std::size_t k = 1; k < events.size(); ++k)
        if (events[k][static_cast<std::size_t>(tier)] == "CHAIN") chain.push_back(events[k]);
    ASSERT_EQ(chain.size(), 3u);
    for (const auto& ev : chain) EXPECT_EQ(ev[static_cast<std::size_t>(event)], "kept");
    EXPECT_EQ(chain[0][static_cast<std::size_t>(action)], "hold");
    EXPECT_EQ(chain[0][static_cast<std::size_t>(station)], "B");
    EXPECT_TRUE(contains(chain[0][static_cast<std::size_t>(detail)], "legacy_cause=rehost_cold legacy_known=0"))
        << chain[0][static_cast<std::size_t>(detail)];
    EXPECT_TRUE(contains(chain[0][static_cast<std::size_t>(detail)], " legacy_step_m= legacy_target_cyc="))   // an unknown step has no value
        << chain[0][static_cast<std::size_t>(detail)];
    EXPECT_TRUE(contains(chain[0][static_cast<std::size_t>(detail)], "flipped=0"));
    EXPECT_EQ(chain[1][static_cast<std::size_t>(action)], "leave");
    EXPECT_EQ(chain[2][static_cast<std::size_t>(action)], "join");
    for (std::size_t k : {std::size_t{1}, std::size_t{2}}) {
        const std::string& d = chain[k][static_cast<std::size_t>(detail)];
        EXPECT_EQ(chain[k][static_cast<std::size_t>(station)], "C");
        EXPECT_TRUE(contains(d, "legacy_cause=rehost_carry legacy_known=1 legacy_pinmove_m=0.012300 "
                                "legacy_step_m=0.012300 legacy_target_cyc=3.000000 flipped=1")) << d;
    }
    // C is this epoch's child under G02; the incumbent orientation had G02 as the child.
    EXPECT_TRUE(contains(chain[1][static_cast<std::size_t>(detail)], " child=G02 parent=C"));
    EXPECT_TRUE(contains(chain[2][static_cast<std::size_t>(detail)], " child=C parent=G02"));
}

// A held arc whose as-built target differs from the frozen one keeps differing at every election, but the
// as-built rule would have moved the edge once, when its target changed: one hold row at the onset, none at
// the elections after it, a second row at the next change of the as-built target, and a reversed arc still
// writes its leave and join pair.
TEST(NetworkChainFrame, TheKeptHoldRowIsWrittenAtTheOnsetOfAnAsBuiltChangeAndNotAtEveryElection) {
    ChainObserverRun r("onset", /*armed=*/true);
    const double frozen = 5.0 * kLambda;
    // The arc A - G01 held at the frozen target while the as-built draft is `drafted`, which the
    // election states against the realized previous target.
    const auto held = [&](double drafted) {
        ChainEdgeObs e = heldEdge(0, "G01", true);
        e.target_m = e.prev_target_m = frozen;
        e.legacy_target_m = drafted;
        e.legacy_pinmove_m = drafted - frozen;
        return e;
    };
    ChainEdgeObs freed = edge(0, "G01", true);               // the row does not hold it: its draft is its pin
    freed.had_parent = true;
    freed.incumbent_same = true;
    freed.target_m = freed.prev_target_m = freed.legacy_target_m = 0.0;
    ChainEdgeObs reversed = heldEdge(2, "G02", false);       // C under G02, the arc reversed
    reversed.incumbent_same = false;
    reversed.incumbent_flipped = true;
    reversed.legacy_target_m = frozen;
    reversed.legacy_pinmove_m = 0.02;
    reversed.cause = ChainLegacyCause::RehostCarry;
    ChainEdgeObs unknown = held(0.0);
    unknown.legacy_known = false;
    using Key = std::tuple<int, std::string, std::string>;
    struct Epoch {
        double                    tow;
        std::vector<ChainEdgeObs> edges;
        std::size_t               rows;     // the kept rows it writes
        std::map<Key, double>     memory;   // the memory after it
    };
    const Key a{0, "G01", "L1C"}, c{2, "G02", "L1C"};
    const std::vector<Epoch> epochs{
        {261060.0, {held(0.0)}, 1, {{a, 0.0}}},                      // held for the first time, the as-built draft 0: a row
        {261090.0, {held(0.0)}, 0, {{a, 0.0}}},                      // the same difference: no row
        {261120.0, {held(0.0)}, 0, {{a, 0.0}}},
        {261150.0, {held(2.5 * kLambda)}, 1, {{a, 2.5 * kLambda}}},  // the as-built target changes: a second row
        {261180.0, {held(2.5 * kLambda)}, 0, {{a, 2.5 * kLambda}}},
        {261210.0, {freed}, 0, {}},                                  // not held: the memory forgets the arc
        {261240.0, {held(0.0)}, 1, {{a, 0.0}}},                      // held again, not held at the last epoch: a row
        {261270.0, {held(0.0), reversed}, 2, {{a, 0.0}, {c, frozen}}},   // the reversed arc's leave and join, nothing else
        {261300.0, {unknown}, 1, {{a, 0.0}}},                        // an unknown as-built step is a row of its own kind
    };
    for (const Epoch& e : epochs) {
        r.observe(e.tow, e.edges);
        ASSERT_TRUE(r.fx->ctx().counters.chain_frame_memory) << e.tow;
        EXPECT_EQ(r.fx->ctx().counters.chain_frame_memory->kept_as_built_target, e.memory) << e.tow;
    }
    const ChainFrameCounters& cnt = r.fx->ctx().counters;
    EXPECT_EQ(cnt.cf_kept_hold_whole, 2);     // the fall to 0 at the first epoch, and again after the arc was not held
    EXPECT_EQ(cnt.cf_kept_hold_other, 1);     // 0 to 2.5 cycles
    EXPECT_EQ(cnt.cf_kept_hold_unknown, 1);
    EXPECT_EQ(cnt.cf_kept_reversed, 1);
    EXPECT_EQ(cnt.cf_events, 6);              // four hold rows, and the reversed arc's pair

    const auto events = r.rows("network_events.csv");
    const int tow = columnOf(events[0], "tow"), tier = columnOf(events[0], "tier"),
              action = columnOf(events[0], "action"), detail = columnOf(events[0], "detail");
    ASSERT_TRUE(tow >= 0 && tier >= 0 && action >= 0 && detail >= 0);
    std::map<std::string, std::vector<std::vector<std::string>>> by_epoch;   // the kept rows of each epoch
    for (std::size_t k = 1; k < events.size(); ++k)
        if (events[k][static_cast<std::size_t>(tier)] == "CHAIN")
            by_epoch[events[k][static_cast<std::size_t>(tow)]].push_back(events[k]);
    for (const Epoch& e : epochs) {
        const auto it = by_epoch.find(fmt::format("{:.3f}", e.tow));
        const std::size_t n = it == by_epoch.end() ? 0u : it->second.size();
        EXPECT_EQ(n, e.rows) << e.tow;
    }
    const auto row = [&](double t, std::size_t k) -> const std::vector<std::string>& {
        return by_epoch.at(fmt::format("{:.3f}", t)).at(k);
    };
    const auto stepToken = [](double step) { return " legacy_step_m=" + fmt::format("{:.6f}", step) + " "; };
    EXPECT_EQ(row(261060.0, 0)[static_cast<std::size_t>(action)], "hold");
    EXPECT_TRUE(contains(row(261060.0, 0)[static_cast<std::size_t>(detail)], stepToken(-frozen)))
        << row(261060.0, 0)[static_cast<std::size_t>(detail)];
    EXPECT_TRUE(contains(row(261150.0, 0)[static_cast<std::size_t>(detail)], stepToken(2.5 * kLambda)))
        << row(261150.0, 0)[static_cast<std::size_t>(detail)];
    EXPECT_TRUE(contains(row(261240.0, 0)[static_cast<std::size_t>(detail)], stepToken(-frozen)))
        << row(261240.0, 0)[static_cast<std::size_t>(detail)];
    EXPECT_EQ(row(261270.0, 0)[static_cast<std::size_t>(action)], "leave");
    EXPECT_EQ(row(261270.0, 1)[static_cast<std::size_t>(action)], "join");
    EXPECT_TRUE(contains(row(261270.0, 0)[static_cast<std::size_t>(detail)], stepToken(0.02)))   // the host change's own displacement
        << row(261270.0, 0)[static_cast<std::size_t>(detail)];
    EXPECT_TRUE(contains(row(261300.0, 0)[static_cast<std::size_t>(detail)], " legacy_step_m= legacy_target_cyc="))
        << row(261300.0, 0)[static_cast<std::size_t>(detail)];
}

// With the row off the memory stays empty and nothing is written, whatever the as-built facts say.
TEST(NetworkChainFrame, WithTheRowOffTheKeptMemoryStaysEmptyAndNothingIsWritten) {
    ChainObserverRun r("onset_off", /*armed=*/false);
    ASSERT_FALSE(r.fx->ctx().knobs.eq34_row_kept);
    for (int k = 0; k < 4; ++k) {
        ChainEdgeObs same = edge(0, "G01", true);
        same.had_parent = true;
        same.incumbent_same = true;
        same.legacy_target_m = 0.5 * k;                       // the as-built facts move; no row decided the edge
        same.legacy_pinmove_m = 0.5;
        same.legacy_known = k % 2 == 0;
        ChainEdgeObs flipped = edge(2, "G02", false);
        flipped.had_parent = true;
        flipped.incumbent_flipped = true;
        r.observe(261060.0 + 30.0 * k, {same, flipped});
        ASSERT_TRUE(r.fx->ctx().counters.chain_frame_memory);
        EXPECT_TRUE(r.fx->ctx().counters.chain_frame_memory->kept_as_built_target.empty()) << k;
    }
    const ChainFrameCounters& cnt = r.fx->ctx().counters;
    EXPECT_EQ(cnt.cf_events, 0);
    EXPECT_EQ(cnt.cf_kept_reversed + cnt.cf_kept_hold_whole + cnt.cf_kept_hold_other + cnt.cf_kept_hold_unknown, 0);
    EXPECT_EQ(cnt.cf_rows_armed[static_cast<std::size_t>(ChainEventRow::Kept)], 0);
    const auto events = r.rows("network_events.csv");
    const int tier = columnOf(events[0], "tier");
    ASSERT_GE(tier, 0);
    for (std::size_t k = 1; k < events.size(); ++k) EXPECT_NE(events[k][static_cast<std::size_t>(tier)], "CHAIN");
}

// The run-end report of the observatory names the armed rows and labels the counts of an armed row;
// a run with no row armed prints what it printed before the rows existed.
TEST(NetworkChainFrame, TheRunEndNamesTheArmedRowAndLabelsItsCounts) {
    ChainEdgeObs plain = edge(1, "G03", true);
    plain.had_parent = true;
    plain.incumbent_same = true;
    plain.known = false;                                   // an as-built re-reference
    {
        ChainObserverRun r("end_on", /*armed=*/true);
        r.observe(261060.0, {heldEdge(0, "G01", true), plain});
        const std::string text = r.runEnd(&networkChainFrameRunEnd);
        EXPECT_TRUE(contains(text, "event row(s) armed: kept; the other rows ran the as-built rule")) << text;
        EXPECT_FALSE(contains(text, "no event row is armed")) << text;
        EXPECT_TRUE(contains(text, "the armed rows cut and declare nothing")) << text;
        EXPECT_TRUE(contains(text, "Kept")) << text;
        EXPECT_TRUE(contains(text, "2 edge(s), 1 under the integer rule: realized step unknown 1, fractional 0")) << text;
        EXPECT_TRUE(contains(text, "Birth")) << text;
        EXPECT_TRUE(contains(text, "edge(s): legacy step unknown 0, fractional 0")) << text;   // the rows not armed
        EXPECT_TRUE(contains(text, "0 kept row(s) written to the event trace: 0 reversed arc(s) (a leave and a join "
                                   "row each) and 0 hold row(s), the hold rows by the as-built step 0 of whole "
                                   "cycles, 0 other nonzero and 0 unknown")) << text;
    }
    {
        ChainObserverRun r("end_off", /*armed=*/false);
        ChainEdgeObs same = edge(0, "G01", true);
        same.had_parent = true;
        same.incumbent_same = true;
        r.observe(261060.0, {same, plain});
        const std::string text = r.runEnd(&networkChainFrameRunEnd);
        EXPECT_TRUE(contains(text, "no event row is armed, so the election ran the legacy rule throughout (observe mode)"))
            << text;
        EXPECT_FALSE(contains(text, "under the integer rule")) << text;
        EXPECT_FALSE(contains(text, "event row(s) armed")) << text;
        EXPECT_FALSE(contains(text, "kept row(s) written")) << text;
        EXPECT_TRUE(contains(text, "declared key(s) (all 0 while no event row is armed)")) << text;
        EXPECT_TRUE(contains(text, "Kept")) << text;
        EXPECT_TRUE(contains(text, "2 edge(s): legacy step unknown 1, fractional 0")) << text;
    }
}

// The election's run-end report prints the kept row's tallies only when the row is armed, and says
// so when an armed row held nothing.
TEST(NetworkChainFrame, TheElectionRunEndReportsTheKeptRowOnlyWhenArmed) {
    {
        ChainObserverRun r("elect_on", /*armed=*/true);
        auto& c = r.fx->ctx().counters;
        c.eq34_kept_held = 9;
        c.eq34_kept_flipped = 2;
        c.eq34_kept_unknown = 3;
        c.eq34_kept_step_whole = 4;
        c.eq34_kept_step_frac = 1;
        c.eq34_kept_rehost = 5;
        c.eq34_kept_not_freed = 1;
        c.eq34_kept_not_root = 2;
        c.eq34_kept_not_target = 3;
        c.eq34_kept_not_life = 4;
        c.eq34_kept_not_ended = 5;
        const std::string text = r.runEnd(&networkChainDatumRunEnd);
        EXPECT_TRUE(contains(text, "9 incumbent tree edge(s) held at their previous target with no frame step of "
                                   "their own, 2 of them with the arc's direction reversed")) << text;
        EXPECT_TRUE(contains(text, "unknown step on 3 of them (re-referencing the subtree), a whole-cycle step "
                                   "on 4, another nonzero step on 1 and read a host change on 5")) << text;
        EXPECT_TRUE(contains(text, "15 incumbent tree edge(s) not held: 1 in the epoch's free-epoch latch, 2 with "
                                   "another remembered root, 3 with no previous target, 4 with no remembered pinned "
                                   "life, 5 whose pinned life ended")) << text;
        EXPECT_TRUE(contains(text, "All of these tallies are per election: an edge counts once per election, and a "
                                   "target the as-built rule would re-read differently counts at every election "
                                   "until the two agree")) << text;
        EXPECT_FALSE(contains(text, "held NO edge")) << text;
    }
    {
        ChainObserverRun r("elect_none", /*armed=*/true);   // armed, and nothing held
        const std::string text = r.runEnd(&networkChainDatumRunEnd);
        EXPECT_TRUE(contains(text, "Network (eq34 kept row): ARMED but held NO edge")) << text;
        EXPECT_TRUE(contains(text, "tallies are per election")) << text;
    }
    {
        ChainObserverRun r("elect_off", /*armed=*/false);
        const std::string text = r.runEnd(&networkChainDatumRunEnd);
        EXPECT_FALSE(contains(text, "eq34 kept row")) << text;
        EXPECT_FALSE(contains(text, "eq34 returning row")) << text;
    }
}

// ---- The returning row ------------------------------------------------------------------------------

/// The CHAIN rows of the events trace of a run, as cells, with the header's columns.
struct ChainRows {
    std::vector<std::vector<std::string>> rows;
    int tow{-1}, event{-1}, action{-1}, sat{-1}, station{-1}, detail{-1};
    explicit ChainRows(ChainObserverRun& r) {
        const auto events = r.rows("network_events.csv");
        const int tier = columnOf(events[0], "tier");
        tow = columnOf(events[0], "tow");
        event = columnOf(events[0], "event");
        action = columnOf(events[0], "action");
        sat = columnOf(events[0], "sat");
        station = columnOf(events[0], "station");
        detail = columnOf(events[0], "detail");
        EXPECT_TRUE(tier >= 0 && tow >= 0 && event >= 0 && action >= 0 && sat >= 0 && station >= 0 && detail >= 0);
        for (std::size_t k = 1; k < events.size(); ++k)
            if (events[k][static_cast<std::size_t>(tier)] == "CHAIN") rows.push_back(events[k]);
    }
    /// The cell @p col of row @p k.
    const std::string& at(std::size_t k, int col) const { return rows.at(k).at(static_cast<std::size_t>(col)); }
};

/// Observe the three epochs of a returning receiver: receiver B (station 1) is below G01 at the first
/// epoch (261030), away at the second (G02 is born under A there) and returns at the third (261120) below G02,
/// with a satellite G03, a receiver C and a satellite G04 below it (births, no row decided them).  @p armed:
/// the returning row released B's edge (and the epoch carries the release of its moved side) rather than
/// leaving the as-built edge.
void observeReturningReceiver(ChainObserverRun& r, bool armed) {
    {   // epoch 1: B below G01
        auto rec = std::make_shared<ChainEpochObs>();
        rec->armed = true;
        rec->comps.push_back(component(0, "A", {0, 1}, {"G01"}));
        rec->edges = {edge(0, "G01", true), edge(1, "G01", false)};
        r.observeRecord(261030.0, rec);
    }
    {   // epoch 2: B away, G02 born
        auto rec = std::make_shared<ChainEpochObs>();
        rec->armed = true;
        rec->comps.push_back(component(0, "A", {0}, {"G01", "G02"}));
        ChainEdgeObs g1 = edge(0, "G01", true);
        g1.had_parent = g1.incumbent_same = true;
        rec->edges = {g1, edge(0, "G02", true)};
        r.observeRecord(261060.0, rec);
    }
    {   // epoch 3: B returns below G02
        auto rec = std::make_shared<ChainEpochObs>();
        rec->armed = true;
        rec->comps.push_back(component(0, "A", {0, 1, 2}, {"G01", "G02", "G03", "G04"}));
        ChainEdgeObs g1 = edge(0, "G01", true);
        g1.had_parent = g1.incumbent_same = true;
        ChainEdgeObs g2 = edge(0, "G02", true);
        g2.had_parent = g2.incumbent_same = true;
        ChainEdgeObs back = edge(1, "G02", false);   // B below G02: the returning edge
        back.depth = 2;
        back.legacy_known = true;                    // the as-built rule: a known step of 5 cycles from the old carry
        back.legacy_pinmove_m = back.legacy_target_m = 5.0 * kLambda;
        if (armed) {                                 // the release replaced it
            back.row_armed = true;
            back.known = false;
            back.pinmove_m = 0.0;
            back.target_m = 0.0;
        } else {
            back.known = true;
            back.pinmove_m = back.target_m = 5.0 * kLambda;
        }
        ChainEdgeObs g3 = edge(1, "G03", true);      // births below it
        g3.known = false;
        ChainEdgeObs c3 = edge(2, "G03", false);
        c3.known = false;
        ChainEdgeObs g4 = edge(2, "G04", true);
        g4.known = false;
        rec->edges = {g1, g2, back, g3, c3, g4};
        if (armed) {
            ChainReleaseObs rel;
            rel.group = kGroup;
            rel.signal = "L1C";
            rel.comp = 0;
            rel.station = 1;
            rel.sat = "G02";
            rel.sat_is_child = false;
            rel.vertex = "R:1";
            rel.sats = {"G03", "G04"};
            rel.stations = {1, 2};
            rel.arcs = 5;
            rel.rpb = 2;
            rel.pb = 1;
            rec->releases = {rel};
        }
        r.observeRecord(261120.0, rec);
    }
}

// A released edge writes its leave and join rows with identities, one cut_release row and one declared_keys
// row per satellite of its moved side; the counters sum the release; the memory keeps the parent a vertex had
// when it was last seen across its absence.
TEST(NetworkChainFrame, TheObserverWritesTheLeaveJoinCutReleaseAndDeclaredKeyRowsOfAReleasedEdge) {
    ChainObserverRun r("ret_rows", std::vector<std::string>{"returning"});
    ASSERT_TRUE(r.fx->ctx().knobs.eq34_row_returning);
    ASSERT_FALSE(r.fx->ctx().knobs.eq34_row_kept);
    observeReturningReceiver(r, /*armed=*/true);

    // The memory: B's last parent is G01 across its absence and G02 now; the root has none.
    ASSERT_TRUE(r.fx->ctx().counters.chain_frame_memory);
    const ChainFrameMemory& mem = *r.fx->ctx().counters.chain_frame_memory;
    EXPECT_EQ(mem.last_parent.at(kGroup).at("R:1"), "S:G02");
    EXPECT_EQ(mem.last_parent.at(kGroup).at("R:0"), "");
    EXPECT_EQ(mem.last_parent.at(kGroup).at("S:G01"), "R:0");
    EXPECT_EQ(mem.last_parent.at(kGroup).at("S:G04"), "R:2");

    const ChainFrameCounters& c = r.fx->ctx().counters;
    const std::size_t returning = static_cast<std::size_t>(ChainEventRow::Returning);
    EXPECT_EQ(c.cf_rows[returning], 1);
    EXPECT_EQ(c.cf_rows_armed[returning], 1);
    EXPECT_EQ(c.cf_rows_unknown[returning], 1);     // the realized step: the release's unknown step
    EXPECT_EQ(c.cf_returning, 1);
    EXPECT_EQ(c.cf_ret_pairs, 1);
    EXPECT_EQ(c.cf_ret_asbuilt_whole, 1);           // the as-built step was 5 whole cycles
    EXPECT_EQ(c.cf_ret_asbuilt_zero + c.cf_ret_asbuilt_other + c.cf_ret_asbuilt_unknown, 0);
    EXPECT_EQ(c.cf_cut_releases, 1);
    EXPECT_EQ(c.cf_cut_arcs, 5);
    EXPECT_EQ(c.cf_cut_rpb, 2);
    EXPECT_EQ(c.cf_cut_pb, 1);
    EXPECT_EQ(c.cf_declared_keys, 2);
    EXPECT_EQ(c.cf_events, 5);                      // leave, join, cut_release and two declared_keys

    ChainRows ev(r);
    ASSERT_EQ(ev.rows.size(), 5u);
    for (const auto& row : ev.rows) EXPECT_EQ(row[static_cast<std::size_t>(ev.tow)], "261120.000");
    // leave: the vertex and the parent it had when it was last seen (the old parent in the sat column).
    EXPECT_EQ(ev.at(0, ev.event), "returning");
    EXPECT_EQ(ev.at(0, ev.action), "leave");
    EXPECT_EQ(ev.at(0, ev.sat), "G01");
    EXPECT_EQ(ev.at(0, ev.station), "B");
    EXPECT_TRUE(contains(ev.at(0, ev.detail), " gap_s=90.0 child=B parent=G01")) << ev.at(0, ev.detail);
    // join: the new edge.
    EXPECT_EQ(ev.at(1, ev.event), "returning");
    EXPECT_EQ(ev.at(1, ev.action), "join");
    EXPECT_EQ(ev.at(1, ev.sat), "G02");
    EXPECT_EQ(ev.at(1, ev.station), "B");
    EXPECT_TRUE(contains(ev.at(1, ev.detail), " gap_s=90.0 child=B parent=G02")) << ev.at(1, ev.detail);
    for (std::size_t k : {std::size_t{0}, std::size_t{1}}) {
        // The as-built facts the release replaced.
        const std::string& d = ev.at(k, ev.detail);
        EXPECT_TRUE(contains(d, "legacy_cause=same_edge legacy_known=1 legacy_pinmove_m=" + fmt::format("{:.6f}", 5.0 * kLambda)))
            << d;
        EXPECT_TRUE(contains(d, "legacy_target_cyc=5.000000")) << d;
        EXPECT_TRUE(contains(d, "known=0")) << d;    // the realized step
        EXPECT_FALSE(contains(d, "prev=")) << d;
    }
    // cut_release: one row per release, with the counts.
    EXPECT_EQ(ev.at(2, ev.event), "cut_release");
    EXPECT_EQ(ev.at(2, ev.action), "release");
    EXPECT_EQ(ev.at(2, ev.sat), "G02");
    EXPECT_EQ(ev.at(2, ev.station), "B");
    EXPECT_EQ(ev.at(2, ev.detail), "sig=L1C comp=0 root=B arcs=5 rpb=2 pb=1 n_rx=2 n_sat=2");
    // declared_keys: one row per satellite of the moved side, its key in the sat column and the signal a token.
    for (std::size_t k : {std::size_t{3}, std::size_t{4}}) {
        EXPECT_EQ(ev.at(k, ev.event), "declared_keys");
        EXPECT_EQ(ev.at(k, ev.action), "declare");
        EXPECT_EQ(ev.at(k, ev.detail), "sig=L1C cause=returning root=B");
    }
    EXPECT_EQ(ev.at(3, ev.sat), "G03");
    EXPECT_EQ(ev.at(4, ev.sat), "G04");

    // The edge trace marks the released edge and no other.
    const auto edges = r.rows("network_chain_edges.csv");
    const int armed = columnOf(edges[0], "row_armed"), row = columnOf(edges[0], "row"),
              tow = columnOf(edges[0], "tow"), known = columnOf(edges[0], "known"), sat = columnOf(edges[0], "edge_sat");
    ASSERT_TRUE(armed >= 0 && row >= 0 && tow >= 0 && known >= 0 && sat >= 0);
    int n_armed = 0;
    for (std::size_t k = 1; k < edges.size(); ++k) {
        if (edges[k][static_cast<std::size_t>(tow)] != "261120.000") continue;
        const bool is_armed = edges[k][static_cast<std::size_t>(armed)] == "1";
        n_armed += is_armed ? 1 : 0;
        if (is_armed) {
            EXPECT_EQ(edges[k][static_cast<std::size_t>(row)], "Returning");
            EXPECT_EQ(edges[k][static_cast<std::size_t>(known)], "0");
            EXPECT_EQ(edges[k][static_cast<std::size_t>(sat)], "G02");
        }
    }
    EXPECT_EQ(n_armed, 1);
}

// With the row off a returning edge writes the one join row it always has, with the gap and an empty
// previous parent, and the cut and declared counts stay 0.
TEST(NetworkChainFrame, WithTheRowOffAReturningEdgeWritesTheOneJoinRowItAlwaysHad) {
    ChainObserverRun r("ret_off", std::vector<std::string>{"kept"});   // another row armed: no returning row
    ASSERT_FALSE(r.fx->ctx().knobs.eq34_row_returning);
    observeReturningReceiver(r, /*armed=*/false);
    const ChainFrameCounters& c = r.fx->ctx().counters;
    EXPECT_EQ(c.cf_ret_pairs + c.cf_cut_releases + c.cf_cut_arcs + c.cf_cut_pb + c.cf_cut_rpb + c.cf_declared_keys, 0);
    EXPECT_EQ(c.cf_rows_armed[static_cast<std::size_t>(ChainEventRow::Returning)], 0);
    EXPECT_EQ(c.cf_returning, 1);
    // The memory still keeps the last parent (observatory-internal).
    EXPECT_EQ(c.chain_frame_memory->last_parent.at(kGroup).at("R:1"), "S:G02");
    ChainRows ev(r);
    // The births below B are not returning vertices; the Returning edge's one row is a join.
    std::vector<std::size_t> returning_rows;
    for (std::size_t k = 0; k < ev.rows.size(); ++k)
        if (ev.at(k, ev.event) == "returning") returning_rows.push_back(k);
    ASSERT_EQ(returning_rows.size(), 1u);
    EXPECT_EQ(ev.at(returning_rows[0], ev.action), "join");
    EXPECT_EQ(ev.at(returning_rows[0], ev.sat), "G02");
    EXPECT_EQ(ev.at(returning_rows[0], ev.station), "B");
    EXPECT_TRUE(contains(ev.at(returning_rows[0], ev.detail), " gap_s=90.0 prev=")) << ev.at(returning_rows[0], ev.detail);
    EXPECT_FALSE(contains(ev.at(returning_rows[0], ev.detail), "child=")) << ev.at(returning_rows[0], ev.detail);
    for (std::size_t k = 0; k < ev.rows.size(); ++k) {
        EXPECT_NE(ev.at(k, ev.event), "cut_release");
        EXPECT_NE(ev.at(k, ev.event), "declared_keys");
    }
}

// The run-end report of the observatory names the returning row as the one that cuts and declares, and counts
// what it wrote; with the row off (or another row alone) it says what it said before.
TEST(NetworkChainFrame, TheRunEndNamesTheReturningRowOnlyWhenArmed) {
    {
        ChainObserverRun r("ret_end_on", std::vector<std::string>{"kept", "returning"});
        observeReturningReceiver(r, /*armed=*/true);
        const std::string text = r.runEnd(&networkChainFrameRunEnd);
        EXPECT_TRUE(contains(text, "event row(s) armed: kept, returning; the other rows ran the as-built rule")) << text;
        EXPECT_TRUE(contains(text, "5 cut-released arc(s), 1 phase-bias prior(s) and 2 receiver phase bias(es); "
                                   "2 declared key(s) (cut and declared by the returning row)")) << text;
        EXPECT_FALSE(contains(text, "the armed rows cut and declare nothing")) << text;
        EXPECT_TRUE(contains(text, "1 edge(s), 1 under the integer rule: realized step unknown 1, fractional 0")) << text;
        EXPECT_TRUE(contains(text, "5 returning row(s) written to the event trace: 1 released edge(s) (a leave and a "
                                   "join row each; the as-built step was a known zero on 0, whole cycles on 1, another "
                                   "nonzero step on 0 and unknown on 0), 1 cut_release row(s) and 2 declared_keys row(s)"))
            << text;
    }
    {
        ChainObserverRun r("ret_end_kept", std::vector<std::string>{"kept"});
        observeReturningReceiver(r, /*armed=*/false);
        const std::string text = r.runEnd(&networkChainFrameRunEnd);
        EXPECT_TRUE(contains(text, "event row(s) armed: kept; the other rows ran the as-built rule")) << text;
        EXPECT_TRUE(contains(text, "0 declared key(s) (the armed rows cut and declare nothing)")) << text;
        EXPECT_FALSE(contains(text, "returning row(s) written")) << text;
        EXPECT_FALSE(contains(text, "cut and declared by the returning row")) << text;
    }
    {
        ChainObserverRun r("ret_end_off", std::vector<std::string>{});
        observeReturningReceiver(r, /*armed=*/false);
        const std::string text = r.runEnd(&networkChainFrameRunEnd);
        EXPECT_TRUE(contains(text, "no event row is armed, so the election ran the legacy rule throughout (observe mode)")) << text;
        EXPECT_TRUE(contains(text, "0 declared key(s) (all 0 while no event row is armed)")) << text;
        EXPECT_FALSE(contains(text, "returning row(s) written")) << text;
        EXPECT_FALSE(contains(text, "under the integer rule")) << text;
    }
}

// The election's run-end report prints the returning row's tallies only when the row is armed, says so when an
// armed row released nothing, and counts the edges left to the slip row.
TEST(NetworkChainFrame, TheElectionRunEndReportsTheReturningRowOnlyWhenArmed) {
    {
        ChainObserverRun r("ret_elect_on", std::vector<std::string>{"returning"});
        auto& c = r.fx->ctx().counters;
        c.eq34_ret_released = 7;
        c.eq34_ret_not_freed = 2;
        c.eq34_ret_with_state = 5;
        c.eq34_ret_asbuilt_zero = 1;
        c.eq34_ret_asbuilt_whole = 3;
        c.eq34_ret_asbuilt_frac = 1;
        c.eq34_ret_asbuilt_unknown = 2;
        c.eq34_ret_asbuilt_pinround = 4;
        c.eq34_ret_arcs = 41;
        c.eq34_ret_rpb = 6;
        c.eq34_ret_pb = 9;
        c.eq34_ret_declared = 11;
        const std::string text = r.runEnd(&networkChainDatumRunEnd);
        EXPECT_TRUE(contains(text, "7 returning edge(s) released, 5 of them with state (a leveling carry, a receiver "
                                   "phase-bias carry or a satellite phase-bias prior) on the moved side")) << text;
        EXPECT_TRUE(contains(text, "The releases erased 41 leveling carry(ies) and dropped 6 receiver phase-bias "
                                   "carry(ies), took 9 satellite phase-bias prior(s) and declared 11 (satellite, "
                                   "signal) key(s) moved")) << text;
        EXPECT_TRUE(contains(text, "known zero step on 1 of the released edges, a whole-cycle step on 3, another "
                                   "nonzero step on 1 and an unknown step on 2, and read its target from the arc's "
                                   "old-frame carry (pin-to-round) on 4")) << text;
        EXPECT_TRUE(contains(text, "2 returning edge(s) left to the slip row")) << text;
        EXPECT_FALSE(contains(text, "released NO edge")) << text;
        EXPECT_FALSE(contains(text, "eq34 kept row")) << text;
    }
    {
        ChainObserverRun r("ret_elect_none", std::vector<std::string>{"returning"});   // armed, nothing released
        const std::string text = r.runEnd(&networkChainDatumRunEnd);
        EXPECT_TRUE(contains(text, "Network (eq34 returning row): ARMED but released NO edge")) << text;
        EXPECT_TRUE(contains(text, "0 returning edge(s) left to the slip row")) << text;
    }
    {
        ChainObserverRun r("ret_elect_kept", std::vector<std::string>{"kept"});
        r.fx->ctx().counters.eq34_ret_released = 3;   // never read with the row off
        const std::string text = r.runEnd(&networkChainDatumRunEnd);
        EXPECT_FALSE(contains(text, "eq34 returning row")) << text;
    }
}

// ---- The rehost row -----------------------------------------------------------------------------------

/// Observe two epochs of a re-hosting satellite: at the first (261030) G01 hangs under A, B under G01 and G02 under
/// B; at the second (261060) G02 moves onto A (its edge A - G02 is a re-host that left B), G01 and B keep their
/// edges.  The as-built rule took a known step of 5 whole cycles from the new host's carry.  @p armed: the rehost
/// row released the edge (and the epoch carries the release of its moved side) rather than leaving the as-built
/// edge.  @p certified: the memory of the first epoch holds a chord table that certifies G02's integer at A, so
/// the observatory calls the re-host certified (the certified branch is held, so the release is the same).
void observeRehostSatellite(ChainObserverRun& r, bool armed, bool certified = false) {
    {   // epoch 1
        auto rec = std::make_shared<ChainEpochObs>();
        rec->armed = true;
        rec->comps.push_back(component(0, "A", {0, 1}, {"G01", "G02"}));
        rec->edges = {edge(0, "G01", true), edge(1, "G01", false), edge(1, "G02", true)};
        r.observeRecord(261030.0, rec);
    }
    if (certified) {
        auto mem = std::make_shared<ChainFrameMemory>(*r.fx->ctx().counters.chain_frame_memory);
        ChainChordRow c;
        c.station = 0;
        c.sat = "G02";
        c.sig1 = "L1C";
        c.sig2 = "L2W";
        c.float_cyc_1 = 5.1;
        c.have_int_1 = true;
        c.int_1 = 5;
        mem->chords[{0, "G02", "L1C"}] = {c, 1};
        r.fx->ctx().counters.chain_frame_memory = mem;
    }
    {   // epoch 2: G02 moves onto A
        auto rec = std::make_shared<ChainEpochObs>();
        rec->armed = true;
        rec->comps.push_back(component(0, "A", {0, 1}, {"G01", "G02"}));
        ChainEdgeObs g1 = edge(0, "G01", true);
        g1.had_parent = g1.incumbent_same = true;
        ChainEdgeObs b1 = edge(1, "G01", false);
        b1.had_parent = b1.incumbent_same = true;
        ChainEdgeObs g2 = edge(0, "G02", true);
        g2.had_parent = true;
        g2.prev_parent = "R:1";
        g2.edge_changed = true;
        g2.cause = ChainLegacyCause::RehostCarry;
        g2.depth = 1;
        g2.legacy_known = true;                      // the as-built rule: a known step of 5 cycles
        g2.legacy_pinmove_m = g2.legacy_target_m = 5.0 * kLambda;
        if (armed) {                                 // the release replaced the step, kept the rounded target
            g2.row_armed = true;
            g2.known = false;
            g2.pinmove_m = 0.0;
            g2.target_m = 5.0 * kLambda;
        } else {
            g2.known = true;
            g2.pinmove_m = g2.target_m = 5.0 * kLambda;
        }
        rec->edges = {g1, b1, g2};
        if (armed) {
            ChainReleaseObs rel;
            rel.row = ChainReleaseRow::Rehost;
            rel.group = kGroup;
            rel.signal = "L1C";
            rel.comp = 0;
            rel.station = 0;
            rel.sat = "G02";
            rel.sat_is_child = true;
            rel.vertex = "S:G02";
            rel.sats = {"G02"};
            rel.arcs = 2;
            rel.pb = 1;
            rec->releases = {rel};
        }
        r.observeRecord(261060.0, rec);
    }
}

// A released re-host writes its leave and join rows with the as-built facts the release replaced, one cut_release
// row and one declared_keys row per satellite of its moved side whose cause names the rehost row; the counters
// sum the release.  A re-host the chord table certifies is released the same way (its certified branch is held)
// and counted as such.
TEST(NetworkChainFrame, TheObserverWritesTheRehostRowsOfAReleasedReHostWithTheAsBuiltFacts) {
    for (const bool certified : {false, true}) {
        const std::string what = certified ? "certified" : "uncertified";
        ChainObserverRun r(std::string("rh_rows_") + (certified ? "c" : "u"), std::vector<std::string>{"rehost"});
        ASSERT_TRUE(r.fx->ctx().knobs.eq34_row_rehost);
        ASSERT_FALSE(r.fx->ctx().knobs.eq34_row_returning || r.fx->ctx().knobs.eq34_row_kept);
        observeRehostSatellite(r, /*armed=*/true, certified);

        const ChainFrameCounters& c = r.fx->ctx().counters;
        const std::size_t klass = static_cast<std::size_t>(certified ? ChainEventRow::RehostCertified
                                                                     : ChainEventRow::RehostUncertified);
        EXPECT_EQ(c.cf_rows[klass], 1) << what;
        EXPECT_EQ(c.cf_rows_armed[klass], 1) << what;
        EXPECT_EQ(c.cf_rows_unknown[klass], 1) << what;    // the realized step: the release's unknown step
        EXPECT_EQ(c.cf_rehosts, 1) << what;
        EXPECT_EQ(c.cf_rh_pairs, 1) << what;
        EXPECT_EQ(c.cf_rh_asbuilt_whole, 1) << what;       // the as-built step was 5 whole cycles
        EXPECT_EQ(c.cf_rh_asbuilt_zero + c.cf_rh_asbuilt_other + c.cf_rh_asbuilt_unknown, 0) << what;
        EXPECT_EQ(c.cf_rh_certified_held, certified ? 1 : 0) << what;
        EXPECT_EQ(c.cf_ret_pairs, 0) << what;
        EXPECT_EQ(c.cf_cut_releases, 1) << what;
        EXPECT_EQ(c.cf_cut_arcs, 2) << what;
        EXPECT_EQ(c.cf_cut_pb, 1) << what;
        EXPECT_EQ(c.cf_cut_rpb, 0) << what;
        EXPECT_EQ(c.cf_declared_keys, 1) << what;
        EXPECT_EQ(c.cf_events, 4) << what;                 // leave, join, cut_release and one declared_keys

        ChainRows ev(r);
        ASSERT_EQ(ev.rows.size(), 4u) << what;
        for (const auto& row : ev.rows) EXPECT_EQ(row[static_cast<std::size_t>(ev.tow)], "261060.000") << what;
        // leave: the parent edge it left (the old parent in the station column), join: the new edge.
        EXPECT_EQ(ev.at(0, ev.event), "rehost") << what;
        EXPECT_EQ(ev.at(0, ev.action), "leave") << what;
        EXPECT_EQ(ev.at(0, ev.sat), "G02") << what;
        EXPECT_EQ(ev.at(0, ev.station), "B") << what;
        EXPECT_EQ(ev.at(1, ev.event), "rehost") << what;
        EXPECT_EQ(ev.at(1, ev.action), "join") << what;
        EXPECT_EQ(ev.at(1, ev.sat), "G02") << what;
        EXPECT_EQ(ev.at(1, ev.station), "A") << what;
        for (std::size_t k : {std::size_t{0}, std::size_t{1}}) {
            const std::string& d = ev.at(k, ev.detail);
            EXPECT_TRUE(contains(d, std::string("row=") + (certified ? "RehostCertified cert=1" : "RehostUncertified cert=0")))
                << what << ": " << d;
            EXPECT_TRUE(contains(d, "known=0")) << what << ": " << d;   // the realized step
            EXPECT_TRUE(contains(d, " legacy_cause=rehost_carry legacy_known=1 legacy_pinmove_m="
                                        + fmt::format("{:.6f}", 5.0 * kLambda)
                                        + " legacy_target_cyc=5.000000")) << what << ": " << d;
        }
        EXPECT_EQ(ev.at(2, ev.event), "cut_release") << what;
        EXPECT_EQ(ev.at(2, ev.action), "release") << what;
        EXPECT_EQ(ev.at(2, ev.sat), "G02") << what;
        EXPECT_EQ(ev.at(2, ev.station), "A") << what;
        EXPECT_EQ(ev.at(2, ev.detail), "sig=L1C comp=0 root=G02 arcs=2 rpb=0 pb=1 n_rx=0 n_sat=1") << what;
        EXPECT_EQ(ev.at(3, ev.event), "declared_keys") << what;
        EXPECT_EQ(ev.at(3, ev.action), "declare") << what;
        EXPECT_EQ(ev.at(3, ev.sat), "G02") << what;
        EXPECT_EQ(ev.at(3, ev.detail), "sig=L1C cause=rehost root=G02") << what;
    }
}

// With the row off a re-host writes the leave and join rows it always has, with no as-built tokens, and
// the cut and declared counts stay 0.
TEST(NetworkChainFrame, WithTheRowOffAReHostWritesThePlainLeaveAndJoinRows) {
    ChainObserverRun r("rh_off", std::vector<std::string>{"kept", "returning"});   // other rows armed: no rehost row
    ASSERT_FALSE(r.fx->ctx().knobs.eq34_row_rehost);
    observeRehostSatellite(r, /*armed=*/false);
    const ChainFrameCounters& c = r.fx->ctx().counters;
    EXPECT_EQ(c.cf_rh_pairs + c.cf_rh_asbuilt_zero + c.cf_rh_asbuilt_whole + c.cf_rh_asbuilt_other
              + c.cf_rh_asbuilt_unknown + c.cf_rh_certified_held, 0);
    EXPECT_EQ(c.cf_cut_releases + c.cf_cut_arcs + c.cf_cut_pb + c.cf_cut_rpb + c.cf_declared_keys, 0);
    EXPECT_EQ(c.cf_rows_armed[static_cast<std::size_t>(ChainEventRow::RehostUncertified)], 0);
    EXPECT_EQ(c.cf_rehosts, 1);
    ChainRows ev(r);
    ASSERT_EQ(ev.rows.size(), 2u);
    for (std::size_t k = 0; k < 2; ++k) {
        EXPECT_EQ(ev.at(k, ev.event), "rehost");
        EXPECT_FALSE(contains(ev.at(k, ev.detail), "legacy_")) << ev.at(k, ev.detail);
        EXPECT_TRUE(contains(ev.at(k, ev.detail), "known=1")) << ev.at(k, ev.detail);
    }
    EXPECT_EQ(ev.at(0, ev.action), "leave");
    EXPECT_EQ(ev.at(0, ev.station), "B");
    EXPECT_EQ(ev.at(1, ev.action), "join");
    EXPECT_EQ(ev.at(1, ev.station), "A");
}

// A returning release and a rehost release in one epoch: each declared_keys row names the row that released.
TEST(NetworkChainFrame, TheDeclaredKeysRowNamesTheRowThatReleased) {
    ChainObserverRun r("rh_two_rows", std::vector<std::string>{"returning", "rehost"});
    auto rec = std::make_shared<ChainEpochObs>();
    rec->armed = true;
    rec->comps.push_back(component(0, "A", {0, 1}, {"G01", "G02"}));
    ChainReleaseObs ret;
    ret.row = ChainReleaseRow::Returning;
    ret.group = kGroup;
    ret.signal = "L1C";
    ret.station = 0;
    ret.sat = "G01";
    ret.vertex = "S:G01";
    ret.sats = {"G01"};
    ChainReleaseObs reh = ret;
    reh.row = ChainReleaseRow::Rehost;
    reh.sat = "G02";
    reh.vertex = "S:G02";
    reh.sats = {"G02"};
    rec->releases = {ret, reh};
    r.observeRecord(261030.0, rec);
    ChainRows ev(r);
    std::vector<std::string> causes;
    for (std::size_t k = 0; k < ev.rows.size(); ++k)
        if (ev.at(k, ev.event) == "declared_keys") causes.push_back(ev.at(k, ev.sat) + ":" + ev.at(k, ev.detail));
    EXPECT_EQ(causes, (std::vector<std::string>{"G01:sig=L1C cause=returning root=G01",
                                                 "G02:sig=L1C cause=rehost root=G02"}));
    const ChainFrameCounters& c = r.fx->ctx().counters;
    EXPECT_EQ(c.cf_cut_releases, 2);
    EXPECT_EQ(c.cf_declared_keys, 2);
    const std::string text = r.runEnd(&networkChainFrameRunEnd);
    EXPECT_TRUE(contains(text, "2 declared key(s) (cut and declared by the returning and rehost rows)")) << text;
    // Each row's line counts only its own rows.
    EXPECT_EQ(c.cf_ret_cut_releases, 1);
    EXPECT_EQ(c.cf_ret_declared_keys, 1);
    EXPECT_EQ(c.cf_rh_cut_releases, 1);
    EXPECT_EQ(c.cf_rh_declared_keys, 1);
    EXPECT_TRUE(contains(text, "2 returning row(s) written to the event trace: 0 released edge(s) (a leave and a join "
                               "row each; the as-built step was a known zero on 0, whole cycles on 0, another nonzero "
                               "step on 0 and unknown on 0), 1 cut_release row(s) and 1 declared_keys row(s)")) << text;
    EXPECT_TRUE(contains(text, "2 rehost row(s) written to the event trace: 0 released edge(s) (a leave and a join "
                               "row each; the as-built step was a known zero on 0, whole cycles on 0, another nonzero "
                               "step on 0 and unknown on 0; the previous chord table certified 0 of them and the held "
                               "certified branch released them anyway), 1 cut_release row(s) and 1 declared_keys "
                               "row(s)")) << text;
    EXPECT_TRUE(contains(text, "event row(s) armed: returning, rehost; the other rows ran the as-built rule")) << text;
}

// The run-end report of the observatory names the rehost row as one that cuts and declares, counts what it wrote
// and says the certified branch is held; with the row off it says what it said before.
TEST(NetworkChainFrame, TheRunEndNamesTheRehostRowAndItsHeldCertifiedBranchOnlyWhenArmed) {
    {
        ChainObserverRun r("rh_end_on", std::vector<std::string>{"kept", "rehost"});
        observeRehostSatellite(r, /*armed=*/true, /*certified=*/false);
        const std::string text = r.runEnd(&networkChainFrameRunEnd);
        EXPECT_TRUE(contains(text, "event row(s) armed: kept, rehost; the other rows ran the as-built rule")) << text;
        EXPECT_TRUE(contains(text, "1 edge(s), 1 under the integer rule: realized step unknown 1, fractional 0; integer "
                                   "rule CutRelease / CutRelease / DeclareMoved")) << text;
        EXPECT_TRUE(contains(text, "0 edge(s), 0 under the integer rule: HELD, its edges took the rule of the rehost "
                                   "entry's built row (RehostUncertified); realized step unknown 0, fractional 0")) << text;
        EXPECT_TRUE(contains(text, "4 rehost row(s) written to the event trace: 1 released edge(s) (a leave and a join "
                                   "row each; the as-built step was a known zero on 0, whole cycles on 1, another "
                                   "nonzero step on 0 and unknown on 0; the previous chord table certified 0 of them and "
                                   "the held certified branch released them anyway), 1 cut_release row(s) and 1 "
                                   "declared_keys row(s)")) << text;
        EXPECT_TRUE(contains(text, "4 rehost row(s) written")) << text;
        EXPECT_TRUE(contains(text, "2 cut-released arc(s), 1 phase-bias prior(s) and 0 receiver phase bias(es); "
                                   "1 declared key(s) (cut and declared by the rehost row)")) << text;
        EXPECT_FALSE(contains(text, "returning row(s) written")) << text;
    }
    {
        ChainObserverRun r("rh_end_cert", std::vector<std::string>{"rehost"});
        observeRehostSatellite(r, /*armed=*/true, /*certified=*/true);
        const std::string text = r.runEnd(&networkChainFrameRunEnd);
        EXPECT_TRUE(contains(text, "1 edge(s), 1 under the integer rule: HELD, its edges took the rule of the rehost "
                                   "entry's built row (RehostUncertified); realized step unknown 1, fractional 0")) << text;
        EXPECT_TRUE(contains(text, "the previous chord table certified 1 of them and the held certified branch "
                                   "released them anyway")) << text;
    }
    {
        ChainObserverRun r("rh_end_off", std::vector<std::string>{"kept"});
        observeRehostSatellite(r, /*armed=*/false);
        const std::string text = r.runEnd(&networkChainFrameRunEnd);
        EXPECT_TRUE(contains(text, "event row(s) armed: kept; the other rows ran the as-built rule")) << text;
        EXPECT_TRUE(contains(text, "0 declared key(s) (the armed rows cut and declare nothing)")) << text;
        EXPECT_FALSE(contains(text, "rehost row(s) written")) << text;
        EXPECT_FALSE(contains(text, "HELD")) << text;
        EXPECT_FALSE(contains(text, "cut and declared by")) << text;
    }
}

// The election's run-end report prints the rehost row's tallies only when the row is armed, says so when an armed
// row released nothing, and counts the edges left to the slip row.
TEST(NetworkChainFrame, TheElectionRunEndReportsTheRehostRowOnlyWhenArmed) {
    {
        ChainObserverRun r("rh_elect_on", std::vector<std::string>{"rehost"});
        auto& c = r.fx->ctx().counters;
        c.eq34_rh_released = 7;
        c.eq34_rh_not_freed = 2;
        c.eq34_rh_with_state = 5;
        c.eq34_rh_carry = 4;
        c.eq34_rh_cold = 3;
        c.eq34_rh_asbuilt_zero = 1;
        c.eq34_rh_asbuilt_whole = 3;
        c.eq34_rh_asbuilt_frac = 1;
        c.eq34_rh_asbuilt_unknown = 2;
        c.eq34_rh_arcs = 41;
        c.eq34_rh_rpb = 6;
        c.eq34_rh_pb = 9;
        c.eq34_rh_declared = 11;
        const std::string text = r.runEnd(&networkChainDatumRunEnd);
        EXPECT_TRUE(contains(text, "7 re-host edge(s) released, 5 of them with state (a leveling carry, a receiver "
                                   "phase-bias carry or a satellite phase-bias prior) on the moved side")) << text;
        EXPECT_TRUE(contains(text, "The releases erased 41 leveling carry(ies) and dropped 6 receiver phase-bias "
                                   "carry(ies), took 9 satellite phase-bias prior(s) and declared 11 (satellite, "
                                   "signal) key(s) moved")) << text;
        EXPECT_TRUE(contains(text, "known zero step on 1 of the released edges, a whole-cycle step on 3, another "
                                   "nonzero step on 1 and an unknown step on 2, and read its target from the new "
                                   "host's carried value (pin-to-round) on 4 (a cold re-host on 3)")) << text;
        EXPECT_TRUE(contains(text, "2 re-host edge(s) left to the slip row")) << text;
        EXPECT_FALSE(contains(text, "released NO edge")) << text;
        EXPECT_FALSE(contains(text, "eq34 returning row")) << text;
    }
    {
        ChainObserverRun r("rh_elect_none", std::vector<std::string>{"rehost"});   // armed, nothing released
        const std::string text = r.runEnd(&networkChainDatumRunEnd);
        EXPECT_TRUE(contains(text, "Network (eq34 rehost row): ARMED but released NO edge")) << text;
        EXPECT_TRUE(contains(text, "0 re-host edge(s) left to the slip row")) << text;
    }
    {
        ChainObserverRun r("rh_elect_ret", std::vector<std::string>{"kept", "returning"});
        r.fx->ctx().counters.eq34_rh_released = 3;   // never read with the row off
        const std::string text = r.runEnd(&networkChainDatumRunEnd);
        EXPECT_FALSE(contains(text, "eq34 rehost row")) << text;
    }
}


// ---- The root row -------------------------------------------------------------------------------------

/// Observe two epochs of a root handover: at the first (261030) the component is rooted at A with receivers A
/// and B and satellites G01 (under A) and G02 (under B), B under G01; at the second (261060) A is gone and the
/// component is rooted at B, whose edges to G01 (reversed) and G02 (as before) carry satellites that remember A.
/// The as-built rule marks both satellite edges unknown.  @p armed: the root row pinned the handover, so the
/// edges keep their frame (known, and decided by the row) and the epoch carries the row's record with
/// @p outcome; otherwise the as-built edges and no record.
void observeRootHandover(ChainObserverRun& r, bool armed, ChainRootOutcome outcome = ChainRootOutcome::Pinned) {
    {   // epoch 1: root A
        auto rec = std::make_shared<ChainEpochObs>();
        rec->armed = true;
        rec->comps.push_back(component(0, "A", {0, 1}, {"G01", "G02"}));
        rec->edges = {edge(0, "G01", true), edge(1, "G01", false), edge(1, "G02", true)};
        r.observeRecord(261030.0, rec);
    }
    {   // epoch 2: root B
        auto rec = std::make_shared<ChainEpochObs>();
        rec->armed = true;
        rec->comps.push_back(component(0, "B", {1}, {"G01", "G02"}));
        ChainEdgeObs g1 = edge(1, "G01", true);   // reversed: G01 now hangs under B
        g1.r1 = "B";
        g1.had_parent = g1.incumbent_flipped = g1.root_unknown = true;
        g1.legacy_known = false;
        g1.cause = ChainLegacyCause::RootHandover;
        ChainEdgeObs g2 = edge(1, "G02", true);   // as before
        g2.r1 = "B";
        g2.had_parent = g2.incumbent_same = g2.root_unknown = true;
        g2.legacy_known = false;
        g2.cause = ChainLegacyCause::RootHandover;
        const bool pinned = armed && outcome == ChainRootOutcome::Pinned;
        for (ChainEdgeObs* g : {&g1, &g2}) {
            g->known = pinned;
            g->row_armed = pinned;
        }
        rec->edges = {g1, g2};
        if (armed) {
            ChainRootObs ro;
            ro.group = kGroup;
            ro.signal = "L1C";
            ro.comp = 0;
            ro.old_root = "A";
            ro.new_root = "B";
            ro.outcome = outcome;
            ro.target_m = 0.07;
            ro.sigma_m = 0.01;
            rec->roots = {ro};
        }
        r.observeRecord(261060.0, rec);
    }
}

// The component event of a root handover keeps its leave and join rows; with the row armed they append the row's
// outcome and, for a pinned handover, the carried value and its standard deviation.  The edges the row kept are
// counted as armed and are not given rows of their own.
TEST(NetworkChainFrame, TheObserverAppendsTheRootRowsOutcomeToTheRootHandoverRows) {
    ChainObserverRun r("root_obs_on", std::vector<std::string>{"root"});
    observeRootHandover(r, /*armed=*/true);
    const ChainRows ev(r);
    std::vector<std::size_t> root_rows;
    for (std::size_t k = 0; k < ev.rows.size(); ++k)
        if (ev.at(k, ev.event) == "root_handover") root_rows.push_back(k);
    ASSERT_EQ(root_rows.size(), 2u);
    EXPECT_EQ(ev.at(root_rows[0], ev.action), "leave");
    EXPECT_EQ(ev.at(root_rows[0], ev.station), "A");
    EXPECT_EQ(ev.at(root_rows[1], ev.action), "join");
    EXPECT_EQ(ev.at(root_rows[1], ev.station), "B");
    for (std::size_t k : root_rows)
        EXPECT_EQ(ev.at(k, ev.detail), "sig=L1C comp=0 old_root=A new_root=B outcome=pinned rpb_target_m=0.070000 "
                                       "rpb_sigma_m=0.010000") << ev.at(k, ev.detail);
    // No per-edge row: the kept edges are in the edge trace, flagged as armed.
    for (std::size_t k = 0; k < ev.rows.size(); ++k) {
        EXPECT_NE(ev.at(k, ev.event), "kept");
        EXPECT_NE(ev.at(k, ev.event), "cut_release");
    }
    const auto edges = r.rows("network_chain_edges.csv");
    const int armed_col = columnOf(edges[0], "row_armed");
    const int row_col = columnOf(edges[0], "row");
    const int known_col = columnOf(edges[0], "known");
    ASSERT_TRUE(armed_col >= 0 && row_col >= 0 && known_col >= 0);
    int n_armed = 0;
    for (std::size_t k = 1; k < edges.size(); ++k)
        if (edges[k][static_cast<std::size_t>(row_col)] == "RootHandover") {
            EXPECT_EQ(edges[k][static_cast<std::size_t>(armed_col)], "1");
            EXPECT_EQ(edges[k][static_cast<std::size_t>(known_col)], "1");
            ++n_armed;
        }
    EXPECT_EQ(n_armed, 2);
    const ChainFrameCounters& c = r.fx->ctx().counters;
    EXPECT_EQ(c.cf_rows_armed[static_cast<std::size_t>(ChainEventRow::RootHandover)], 2);
    EXPECT_EQ(c.cf_root_handovers, 1);
    EXPECT_EQ(c.cf_root_pinned, 1);
    EXPECT_EQ(c.cf_root_left, 0);
}

// A handover the row left as built writes the outcome only, and the counters split it by cause.
TEST(NetworkChainFrame, ARootHandoverTheRowLeftAsBuiltNamesItsOutcomeAndIsCountedByCause) {
    const std::vector<std::pair<ChainRootOutcome, const char*>> causes{
        {ChainRootOutcome::NotVertex, "not_vertex"}, {ChainRootOutcome::NotKeeper, "not_keeper"},
        {ChainRootOutcome::NoCarry, "no_carry"}};
    for (const auto& [outcome, name] : causes) {
        ChainObserverRun r(std::string("root_obs_left_") + name, std::vector<std::string>{"root"});
        observeRootHandover(r, /*armed=*/true, outcome);
        const ChainRows ev(r);
        int n = 0;
        for (std::size_t k = 0; k < ev.rows.size(); ++k)
            if (ev.at(k, ev.event) == "root_handover") {
                EXPECT_EQ(ev.at(k, ev.detail), std::string("sig=L1C comp=0 old_root=A new_root=B outcome=") + name)
                    << ev.at(k, ev.detail);
                ++n;
            }
        EXPECT_EQ(n, 2) << name;
        const ChainFrameCounters& c = r.fx->ctx().counters;
        EXPECT_EQ(c.cf_root_pinned, 0) << name;
        EXPECT_EQ(c.cf_root_left, 1) << name;
        EXPECT_EQ(c.cf_root_not_vertex, outcome == ChainRootOutcome::NotVertex ? 1 : 0) << name;
        EXPECT_EQ(c.cf_root_not_keeper, outcome == ChainRootOutcome::NotKeeper ? 1 : 0) << name;
        EXPECT_EQ(c.cf_root_no_carry, outcome == ChainRootOutcome::NoCarry ? 1 : 0) << name;
        EXPECT_EQ(c.cf_rows_armed[static_cast<std::size_t>(ChainEventRow::RootHandover)], 0) << name;
    }
}

// With the row off the root handover rows are the rows they were, with or without another row armed, and no root
// counter moves.
TEST(NetworkChainFrame, WithTheRootRowOffTheRootHandoverRowsAreUnchanged) {
    for (const std::vector<std::string>& rows : {std::vector<std::string>{}, std::vector<std::string>{"kept"},
                                                 std::vector<std::string>{"kept", "returning", "rehost"}}) {
        ChainObserverRun r("root_obs_off_" + std::to_string(rows.size()), rows);
        observeRootHandover(r, /*armed=*/false);
        const ChainRows ev(r);
        int n = 0;
        for (std::size_t k = 0; k < ev.rows.size(); ++k)
            if (ev.at(k, ev.event) == "root_handover") {
                EXPECT_EQ(ev.at(k, ev.detail), "sig=L1C comp=0 old_root=A new_root=B") << ev.at(k, ev.detail);
                ++n;
            }
        EXPECT_EQ(n, 2) << rows.size();
        const ChainFrameCounters& c = r.fx->ctx().counters;
        EXPECT_EQ(c.cf_root_pinned + c.cf_root_left + c.cf_root_not_vertex + c.cf_root_not_keeper + c.cf_root_no_carry, 0)
            << rows.size();
        EXPECT_EQ(c.cf_rows_armed[static_cast<std::size_t>(ChainEventRow::RootHandover)], 0) << rows.size();
        EXPECT_EQ(c.cf_root_handovers, 1) << rows.size();   // the observatory's own census of the handover
    }
}

// The run-end report of the observatory names the root row's decisions only when the row is armed.
TEST(NetworkChainFrame, TheRunEndNamesTheRootRowOnlyWhenArmed) {
    {
        ChainObserverRun r("root_end_on", std::vector<std::string>{"root"});
        observeRootHandover(r, /*armed=*/true);
        const std::string text = r.runEnd(&networkChainFrameRunEnd);
        EXPECT_TRUE(contains(text, "event row(s) armed: root; the other rows ran the as-built rule")) << text;
        EXPECT_TRUE(contains(text, "2 edge(s), 2 under the integer rule: realized step unknown 0, fractional 0; integer "
                                   "rule Zero / Keep / None")) << text;
        EXPECT_TRUE(contains(text, "1 root handover(s) decided by the root row: 1 pinned at a carried receiver phase "
                                   "bias and 0 left as built (the new root was no vertex of the last tree on 0, a split "
                                   "piece that does not keep the old root on 0, no carried bias inside the horizon on "
                                   "0)")) << text;
        EXPECT_TRUE(contains(text, "0 declared key(s) (the armed rows cut and declare nothing)")) << text;
    }
    {
        ChainObserverRun r("root_end_left", std::vector<std::string>{"kept", "root"});
        observeRootHandover(r, /*armed=*/true, ChainRootOutcome::NoCarry);
        const std::string text = r.runEnd(&networkChainFrameRunEnd);
        EXPECT_TRUE(contains(text, "1 root handover(s) decided by the root row: 0 pinned at a carried receiver phase "
                                   "bias and 1 left as built (the new root was no vertex of the last tree on 0, a split "
                                   "piece that does not keep the old root on 0, no carried bias inside the horizon on "
                                   "1)")) << text;
    }
    {
        ChainObserverRun r("root_end_off", std::vector<std::string>{"kept"});
        observeRootHandover(r, /*armed=*/false);
        const std::string text = r.runEnd(&networkChainFrameRunEnd);
        EXPECT_TRUE(contains(text, "event row(s) armed: kept; the other rows ran the as-built rule")) << text;
        EXPECT_FALSE(contains(text, "decided by the root row")) << text;
        // The root-handover row prints as an unarmed row: its edges ran the as-built rule.
        EXPECT_TRUE(contains(text, "2 edge(s): legacy step unknown 2, fractional 0; integer rule Zero / Keep / None"))
            << text;
    }
}

// The election's run-end report prints the root row's tallies only when the row is armed, calls a run with no
// handover no event, and calls a run whose handovers were all left as built inert.
TEST(NetworkChainFrame, TheElectionRunEndReportsTheRootRowOnlyWhenArmed) {
    {
        ChainObserverRun r("root_elect_on", std::vector<std::string>{"root"});
        auto& c = r.fx->ctx().counters;
        c.eq34_root_handovers = 6;
        c.eq34_root_pinned = 3;
        c.eq34_root_kept_edges = 9;
        c.eq34_root_not_vertex = 1;
        c.eq34_root_not_keeper = 2;
        c.eq34_root_not_carry = 0;
        const std::string text = r.runEnd(&networkChainDatumRunEnd);
        EXPECT_TRUE(contains(text, "Network (eq34 root row): 6 root handover(s) seen, 3 pinned at the new root's carried "
                                   "receiver phase bias and 9 satellite edge(s) kept in the old root's frame (the "
                                   "as-built rule marks every one unknown); 3 left as built (1 the new root was no vertex "
                                   "of the last tree, 2 a split piece that does not keep the old root, 0 no carried bias "
                                   "inside the horizon)")) << text;
        EXPECT_FALSE(contains(text, "INERT")) << text;
        EXPECT_FALSE(contains(text, "eq34 rehost row")) << text;
    }
    {
        ChainObserverRun r("root_elect_none", std::vector<std::string>{"root"});   // armed, no handover at all
        const std::string text = r.runEnd(&networkChainDatumRunEnd);
        EXPECT_TRUE(contains(text, "Network (eq34 root row): ARMED, and the election saw no root handover")) << text;
        EXPECT_FALSE(contains(text, "INERT")) << text;
    }
    {
        ChainObserverRun r("root_elect_inert", std::vector<std::string>{"root"});   // armed, every handover left as built
        auto& c = r.fx->ctx().counters;
        c.eq34_root_handovers = 4;
        c.eq34_root_not_vertex = 1;
        c.eq34_root_not_keeper = 1;
        c.eq34_root_not_carry = 2;
        const std::string text = r.runEnd(&networkChainDatumRunEnd);
        EXPECT_TRUE(contains(text, "Network (eq34 root row): ARMED but pinned NO handover (4 seen: 1 left as built "
                                   "because the new root was no vertex of the last tree, 1 because the component was a "
                                   "split piece that does not keep the old root, 2 because the new root had no receiver "
                                   "phase-bias carry inside the horizon)")) << text;
        EXPECT_TRUE(contains(text, "INERT")) << text;
    }
    {
        ChainObserverRun r("root_elect_off", std::vector<std::string>{"kept", "returning", "rehost"});
        auto& c = r.fx->ctx().counters;
        c.eq34_root_handovers = 3;   // never read with the row off
        c.eq34_root_pinned = 3;
        const std::string text = r.runEnd(&networkChainDatumRunEnd);
        EXPECT_FALSE(contains(text, "eq34 root row")) << text;
    }
}


// ---- The slip row ------------------------------------------------------------------------------------

/// Observe a slip on the tree edge B-G02 at the second epoch (261060), with the row armed: the edge left the tree, C
/// re-attached through G03 and the rehost row released it, with G02 in its moved side.  The record carries the de-seat.
void observeDeseat(ChainObserverRun& r) {
    {   // epoch 1: G02 below B, C below G02
        auto rec = std::make_shared<ChainEpochObs>();
        rec->armed = true;
        rec->comps.push_back(component(0, "A", {0, 1, 2}, {"G01", "G02", "G03"}));
        rec->edges = {edge(0, "G01", true), edge(1, "G01", false), edge(1, "G02", true), edge(1, "G03", true),
                      edge(2, "G02", false)};
        r.observeRecord(261030.0, rec);
    }
    {   // epoch 2: the slip
        auto rec = std::make_shared<ChainEpochObs>();
        rec->armed = true;
        rec->comps.push_back(component(0, "A", {0, 1, 2}, {"G01", "G02", "G03"}));
        ChainEdgeObs join = edge(2, "G03", false);   // the new edge: C below G03
        join.had_parent = true;
        join.prev_parent = "S:G02";
        join.row_armed = true;
        join.known = false;
        join.step_m = std::numeric_limits<double>::quiet_NaN();
        join.legacy_known = true;
        join.legacy_pinmove_m = 0.3 * kLambda;
        ChainEdgeObs rev = heldEdge(2, "G02", true);   // G02 reversed, below C
        rev.incumbent_same = false;
        rev.incumbent_flipped = true;
        rec->edges = {heldEdge(0, "G01", true), heldEdge(1, "G01", false), heldEdge(1, "G03", true), join, rev};
        ChainDeseatObs d;
        d.group = kGroup;
        d.signal = "L1C";
        d.comp = 0;
        d.station = 1;
        d.station_name = "B";
        d.sat = "G02";
        d.sat_is_child = true;
        d.child = "S:G02";
        d.parent = "R:1";
        d.lambda_m = kLambda;
        d.prev_target_m = 3.0 * kLambda;
        d.cause = 132;
        d.band_latched = true;
        d.new_parent = "R:2";
        d.released = true;
        d.release_row = ChainReleaseRow::Rehost;
        d.release_station = 2;
        d.release_sat = "G03";
        d.release_vertex = "R:2";
        rec->deseats = {d};
        ChainReleaseObs rel;
        rel.row = ChainReleaseRow::Rehost;
        rel.group = kGroup;
        rel.signal = "L1C";
        rel.station = 2;
        rel.sat = "G03";
        rel.sat_is_child = false;
        rel.vertex = "R:2";
        rel.stations = {2};
        rel.sats = {"G02"};
        rel.arcs = 3;
        rec->releases = {rel};
        r.observeRecord(261060.0, rec);
    }
}

// A de-seated edge writes both edges of the event with their identities: the slipped arc leaves, naming its cause bits,
// and the old child joins through the edge of the release that took its moved side.
TEST(NetworkChainFrame, TheObserverWritesTheLeaveAndJoinRowsOfADeseatedEdge) {
    ChainObserverRun r("slip_obs_deseat", std::vector<std::string>{"kept", "rehost", "slip"});
    observeDeseat(r);
    const ChainRows ev(r);
    std::vector<std::size_t> slip_rows;
    for (std::size_t k = 0; k < ev.rows.size(); ++k)
        if (ev.at(k, ev.event) == "slip_deseat") slip_rows.push_back(k);
    ASSERT_EQ(slip_rows.size(), 2u);
    EXPECT_EQ(ev.at(slip_rows[0], ev.action), "leave");
    EXPECT_EQ(ev.at(slip_rows[0], ev.sat), "G02");
    EXPECT_EQ(ev.at(slip_rows[0], ev.station), "B");
    EXPECT_EQ(ev.at(slip_rows[0], ev.detail),
              "sig=L1C comp=0 slip_cause=132 deseated=1 band_latched=1 child=G02 parent=B prev_target_cyc=3.000000 "
              "new_parent=C released_by=rehost");
    EXPECT_EQ(ev.at(slip_rows[1], ev.action), "join");
    EXPECT_EQ(ev.at(slip_rows[1], ev.sat), "G03");
    EXPECT_EQ(ev.at(slip_rows[1], ev.station), "C");
    EXPECT_EQ(ev.at(slip_rows[1], ev.detail),
              "sig=L1C comp=0 slip_cause=132 deseated=1 band_latched=1 child=G02 parent=B prev_target_cyc=3.000000 "
              "new_parent=C released_by=rehost rejoin=C");
    const ChainFrameCounters& c = r.fx->ctx().counters;
    EXPECT_EQ(c.cf_sl_deseats, 1);
    EXPECT_EQ(c.cf_sl_pairs, 0);
    EXPECT_EQ(c.cf_rh_cut_releases, 1);
    EXPECT_EQ(c.cf_sl_cut_releases, 0);
}

// With no release the join row names the old child's new parent edge.
TEST(NetworkChainFrame, ADeseatedEdgeWithNoReleaseJoinsThroughItsNewParentEdge) {
    ChainObserverRun r("slip_obs_noreleases", std::vector<std::string>{"kept", "slip"});
    auto rec = std::make_shared<ChainEpochObs>();
    rec->armed = true;
    rec->comps.push_back(component(0, "A", {0, 1, 2}, {"G01", "G02"}));
    ChainDeseatObs sat_child;
    sat_child.group = kGroup;
    sat_child.signal = "L1C";
    sat_child.comp = 0;
    sat_child.station = 1;
    sat_child.station_name = "B";
    sat_child.sat = "G02";
    sat_child.child = "S:G02";
    sat_child.parent = "R:1";
    sat_child.lambda_m = kLambda;
    sat_child.new_parent = "R:2";
    ChainDeseatObs rx_child = sat_child;
    rx_child.sat = "G01";
    rx_child.sat_is_child = false;
    rx_child.child = "R:1";
    rx_child.parent = "S:G01";
    rx_child.new_parent = "S:G03";
    rx_child.band_latched = false;
    rec->deseats = {sat_child, rx_child};
    r.observeRecord(261030.0, rec);
    const ChainRows ev(r);
    std::vector<std::size_t> joins;
    for (std::size_t k = 0; k < ev.rows.size(); ++k)
        if (ev.at(k, ev.event) == "slip_deseat" && ev.at(k, ev.action) == "join") joins.push_back(k);
    ASSERT_EQ(joins.size(), 2u);
    EXPECT_EQ(ev.at(joins[0], ev.sat), "G02");    // the satellite joins under its new station
    EXPECT_EQ(ev.at(joins[0], ev.station), "C");
    EXPECT_TRUE(contains(ev.at(joins[0], ev.detail), "released_by=none")) << ev.at(joins[0], ev.detail);
    EXPECT_FALSE(contains(ev.at(joins[0], ev.detail), "rejoin=")) << ev.at(joins[0], ev.detail);
    EXPECT_EQ(ev.at(joins[1], ev.sat), "G03");    // the receiver joins under its new satellite
    EXPECT_EQ(ev.at(joins[1], ev.station), "B");
    EXPECT_TRUE(contains(ev.at(joins[1], ev.detail), "band_latched=0")) << ev.at(joins[1], ev.detail);
    EXPECT_EQ(r.fx->ctx().counters.cf_sl_deseats, 2);
}

// An old child with no parent edge now (a root, or no vertex of the band's trees) still names both edges: the join row
// keeps the arc's own names and says so, with no blank satellite or station.
TEST(NetworkChainFrame, ADeseatedEdgeWhoseChildHasNoParentNowJoinsOnTheArcsOwnNames) {
    ChainObserverRun r("slip_obs_noparent", std::vector<std::string>{"kept", "slip"});
    auto rec = std::make_shared<ChainEpochObs>();
    rec->armed = true;
    rec->comps.push_back(component(0, "B", {0, 1}, {"G01"}));
    ChainDeseatObs d;
    d.group = kGroup;
    d.signal = "L1C";
    d.comp = 0;
    d.station = 1;
    d.station_name = "B";
    d.sat = "G01";
    d.sat_is_child = false;
    d.child = "R:1";
    d.parent = "S:G01";
    d.lambda_m = kLambda;
    rec->deseats = {d};
    r.observeRecord(261030.0, rec);
    const ChainRows ev(r);
    std::vector<std::size_t> joins;
    for (std::size_t k = 0; k < ev.rows.size(); ++k)
        if (ev.at(k, ev.event) == "slip_deseat" && ev.at(k, ev.action) == "join") joins.push_back(k);
    ASSERT_EQ(joins.size(), 1u);
    EXPECT_EQ(ev.at(joins[0], ev.sat), "G01");
    EXPECT_EQ(ev.at(joins[0], ev.station), "B");
    EXPECT_TRUE(contains(ev.at(joins[0], ev.detail), "new_parent= released_by=none rejoin=none")) << ev.at(joins[0], ev.detail);
}

/// An edge on a slipped arc that stayed, as the election records it: @p released, the slip row released it.
ChainEdgeObs latchedEdge(bool released) {
    ChainEdgeObs e = edge(1, "G02", true);
    e.had_parent = e.incumbent_same = true;
    e.legacy_known = true;
    e.legacy_pinmove_m = -2.0 * kLambda;
    e.legacy_target_m = 5.0 * kLambda;
    e.target_m = 7.0 * kLambda;
    if (released) {
        e.row_armed = e.slip_released = true;
        e.known = false;
        e.pinmove_m = 0.0;
        e.step_m = std::numeric_limits<double>::quiet_NaN();
    } else {
        e.pinmove_m = -2.0 * kLambda;
        e.target_m = 5.0 * kLambda;
    }
    return e;
}

/// Observe the latched edge of @ref latchedEdge at one epoch, with the free-epoch latch naming it and, when released,
/// the slip row's release.
void observeLatchedEdge(ChainObserverRun& r, bool released) {
    auto rec = std::make_shared<ChainEpochObs>();
    rec->armed = true;
    rec->comps.push_back(component(0, "A", {0, 1}, {"G01", "G02"}));
    rec->edges = {edge(0, "G01", true), edge(1, "G01", false), latchedEdge(released)};
    if (released) {
        ChainReleaseObs rel;
        rel.row = ChainReleaseRow::Slip;
        rel.group = kGroup;
        rel.signal = "L1C";
        rel.station = 1;
        rel.sat = "G02";
        rel.vertex = "S:G02";
        rel.sats = {"G02"};
        rel.arcs = 2;
        rel.pb = 1;
        rec->releases = {rel};
    }
    NetworkEpoch& e = r.fx->newEpoch(GnssTime{2313, 261030.0});
    e.clock.pl_now_s = 261030.0;
    const std::pair<int, std::string> key{1, StateKeys::makeAmbiguityKey("G02", "L1C")};
    e.events.slip.relevel.free_ambs.insert(key);
    e.events.slip.relevel.free_cause[key] = 128;
    e.events.phase_datum.chain = std::move(rec);
    networkChainFrameObserve(r.fx->run(), e);
}

// An edge the slip row released writes a leave and a join row on the arc itself, with the as-built facts the release
// replaced, then the slip row's cut_release and declared_keys rows.
TEST(NetworkChainFrame, TheObserverWritesTheRowsOfAnEdgeTheSlipRowReleased) {
    ChainObserverRun r("slip_obs_released", std::vector<std::string>{"kept", "slip"});
    observeLatchedEdge(r, /*released=*/true);
    const ChainRows ev(r);
    std::vector<std::size_t> slip_rows, cut_rows, declared_rows;
    for (std::size_t k = 0; k < ev.rows.size(); ++k) {
        if (ev.at(k, ev.event) == "slip_deseat") slip_rows.push_back(k);
        if (ev.at(k, ev.event) == "cut_release") cut_rows.push_back(k);
        if (ev.at(k, ev.event) == "declared_keys") declared_rows.push_back(k);
    }
    ASSERT_EQ(slip_rows.size(), 2u);
    EXPECT_EQ(ev.at(slip_rows[0], ev.action), "leave");
    EXPECT_EQ(ev.at(slip_rows[1], ev.action), "join");
    for (std::size_t k : slip_rows) {
        EXPECT_EQ(ev.at(k, ev.sat), "G02");
        EXPECT_EQ(ev.at(k, ev.station), "B");
        const std::string& detail = ev.at(k, ev.detail);
        EXPECT_TRUE(contains(detail, "row=TreeEdgeSlip")) << detail;
        EXPECT_TRUE(contains(detail, "slip_cause=128 deseated=0 readmitted=1 child=G02 parent=B")) << detail;
        EXPECT_TRUE(contains(detail, "legacy_known=1")) << detail;
        EXPECT_TRUE(contains(detail, "legacy_target_cyc=5.000000")) << detail;
    }
    ASSERT_EQ(cut_rows.size(), 1u);
    ASSERT_EQ(declared_rows.size(), 1u);
    EXPECT_EQ(ev.at(declared_rows[0], ev.sat), "G02");
    EXPECT_EQ(ev.at(declared_rows[0], ev.detail), "sig=L1C cause=slip root=G02");
    const ChainFrameCounters& c = r.fx->ctx().counters;
    EXPECT_EQ(c.cf_sl_pairs, 1);
    EXPECT_EQ(c.cf_sl_asbuilt_whole, 1);
    EXPECT_EQ(c.cf_sl_asbuilt_zero + c.cf_sl_asbuilt_other + c.cf_sl_asbuilt_unknown, 0);
    EXPECT_EQ(c.cf_sl_cut_releases, 1);
    EXPECT_EQ(c.cf_sl_declared_keys, 1);
    EXPECT_EQ(c.cf_rh_cut_releases + c.cf_ret_cut_releases, 0);
    EXPECT_EQ(c.cf_rows_armed[static_cast<std::size_t>(ChainEventRow::TreeEdgeSlip)], 1);
    EXPECT_EQ(c.cf_rows_unknown[static_cast<std::size_t>(ChainEventRow::TreeEdgeSlip)], 1);
}

// With the row off a slipped edge in the tree writes the one observe row it always had, and no counter of the row moves.
TEST(NetworkChainFrame, WithTheRowOffASlippedEdgeWritesTheObserveRowItAlwaysHad) {
    for (const std::vector<std::string>& rows : {std::vector<std::string>{}, std::vector<std::string>{"kept", "rehost", "root"}}) {
        ChainObserverRun r("slip_obs_off_" + std::to_string(rows.size()), rows);
        observeLatchedEdge(r, /*released=*/false);
        const ChainRows ev(r);
        std::vector<std::size_t> slip_rows;
        for (std::size_t k = 0; k < ev.rows.size(); ++k)
            if (ev.at(k, ev.event) == "slip_deseat") slip_rows.push_back(k);
        ASSERT_EQ(slip_rows.size(), 1u) << rows.size();
        EXPECT_EQ(ev.at(slip_rows[0], ev.action), "observe") << rows.size();
        EXPECT_TRUE(contains(ev.at(slip_rows[0], ev.detail), "slip_cause=128 deseated=0")) << ev.at(slip_rows[0], ev.detail);
        EXPECT_FALSE(contains(ev.at(slip_rows[0], ev.detail), "readmitted")) << ev.at(slip_rows[0], ev.detail);
        const ChainFrameCounters& c = r.fx->ctx().counters;
        EXPECT_EQ(c.cf_sl_deseats + c.cf_sl_pairs + c.cf_sl_cut_releases + c.cf_sl_declared_keys, 0) << rows.size();
        EXPECT_EQ(c.cf_rows_armed[static_cast<std::size_t>(ChainEventRow::TreeEdgeSlip)], 0) << rows.size();
    }
}

// The run-end report of the observatory names the slip row's rows only when the row is armed.
TEST(NetworkChainFrame, TheRunEndNamesTheSlipRowOnlyWhenArmed) {
    {
        ChainObserverRun r("slip_end_on", std::vector<std::string>{"kept", "rehost", "slip"});
        observeDeseat(r);
        const std::string text = r.runEnd(&networkChainFrameRunEnd);
        EXPECT_TRUE(contains(text, "event row(s) armed: kept, rehost, slip; the other rows ran the as-built rule")) << text;
        EXPECT_TRUE(contains(text, "2 slip row(s) written to the event trace: 1 de-seated edge(s) and 0 edge(s) released "
                                   "because no other route reached the child")) << text;
        EXPECT_TRUE(contains(text, "cut and declared by the rehost and slip rows")) << text;
    }
    {
        ChainObserverRun r("slip_end_all", std::vector<std::string>{"kept", "returning", "rehost", "root", "slip"});
        observeLatchedEdge(r, /*released=*/true);
        const std::string text = r.runEnd(&networkChainFrameRunEnd);
        EXPECT_TRUE(contains(text, "4 slip row(s) written to the event trace: 0 de-seated edge(s) and 1 edge(s) released "
                                   "because no other route reached the child (a leave and a join row each; the as-built "
                                   "step of a released edge was a known zero on 0, whole cycles on 1, another nonzero step "
                                   "on 0 and unknown on 0), 1 cut_release row(s) and 1 declared_keys row(s) of the slip "
                                   "row's releases")) << text;
        EXPECT_TRUE(contains(text, "cut and declared by the returning, rehost and slip rows")) << text;
    }
    {
        ChainObserverRun r("slip_end_off", std::vector<std::string>{"kept", "rehost"});
        observeLatchedEdge(r, /*released=*/false);
        const std::string text = r.runEnd(&networkChainFrameRunEnd);
        EXPECT_FALSE(contains(text, "slip row(s) written")) << text;
        EXPECT_TRUE(contains(text, "cut and declared by the rehost row")) << text;
    }
}

// The election's run-end report prints the slip row's tallies only when the row is armed, and calls a run in which no
// tree edge slipped no event.
// ---- The merge and split row -------------------------------------------------------------------------

// The row's release is a Merge, ahead of every other row.  With the row off the classification is what it always was: an
// edge that joins two components through a child with a parent reads as a re-host, and a kept edge of the absorbed side
// whose satellite remembers another root as a root handover.  An edge the row marked inside an absorbed side stays Kept.
TEST(NetworkChainFrame, TheRowsReleaseIsAMergeAndWithoutItTheClassificationIsUnchanged) {
    ChainFrameMemory mem = memoryOf("A", {"R:0", "S:G01"});
    mem.vertex_comp[kGroup]["R:1"] = 1;                 // a second previous component: B, G02, G03
    mem.vertex_comp[kGroup]["S:G02"] = 1;
    mem.vertex_comp[kGroup]["S:G03"] = 1;
    mem.comp_root[kGroup][1] = "B";
    ChainEpochObs obs;
    obs.armed = true;
    obs.comps.push_back(component(0, "A", {0, 1}, {"G01", "G02", "G03"}));
    ChainEdgeObs re_host = edge(1, "G01", false);       // B below G01: B had a parent, the arc is new
    re_host.had_parent = true;
    re_host.prev_parent = "S:G09";
    ChainEdgeObs retained = edge(1, "G02", true);       // inside the second tree, same direction: both ends in comp 1
    retained.incumbent_same = true;
    retained.root_unknown = true;
    ChainEdgeObs marked = edge(1, "G03", true);         // the same, marked by the row
    marked.incumbent_same = true;
    marked.root_unknown = true;
    marked.absorbed = true;
    ChainEdgeObs stays = edge(0, "G01", true);
    stays.incumbent_same = true;
    obs.edges = {stays, re_host, retained, marked};
    ChainFrameCounters cnt;
    const ChainEpochClass cls = chainClassifyEpoch(obs, mem, {}, {}, cnt);
    ASSERT_EQ(cls.edges.size(), 4u);
    EXPECT_EQ(cls.edges[0].row, ChainEventRow::Kept);
    EXPECT_EQ(cls.edges[1].row, ChainEventRow::RehostUncertified);
    EXPECT_EQ(cls.edges[2].row, ChainEventRow::RootHandover);
    EXPECT_EQ(cls.edges[3].row, ChainEventRow::Kept);
    EXPECT_EQ(cnt.cf_merges, 1);                        // the component event counts the merge
    EXPECT_EQ(cnt.cf_rehosts, 1);
    // Released by the row, the same edge is a Merge, and asks no certification.
    ChainEpochObs released = obs;
    released.edges[1].merge_released = true;
    ChainFrameCounters cnt_r;
    EXPECT_EQ(chainClassifyEpoch(released, mem, {}, {}, cnt_r).edges[1].row, ChainEventRow::Merge);
    EXPECT_EQ(cnt_r.cf_rehosts, 0);
    // The row's release is a Merge even on an arc the latch names.
    ChainEpochObs obs2 = obs;
    obs2.edges = {edge(1, "G01", false)};
    obs2.edges[0].merge_released = true;
    const std::pair<int, std::string> key{1, StateKeys::makeAmbiguityKey("G01", "L1C")};
    ChainFrameCounters cnt2;
    EXPECT_EQ(chainClassifyEpoch(obs2, mem, {key}, {}, cnt2).edges[0].row, ChainEventRow::Merge);
    ChainEpochObs obs3 = obs2;
    obs3.edges[0].merge_released = false;
    ChainFrameCounters cnt3;
    EXPECT_EQ(chainClassifyEpoch(obs3, mem, {key}, {}, cnt3).edges[0].row, ChainEventRow::TreeEdgeSlip);
}

/// Observe two trees joined at the second epoch (261060) by C below G01: with the row armed its release, with the row
/// off the edge as built.
void observeMerge(ChainObserverRun& r, bool released) {
    {   // epoch 1: A - G01, and B - G02 - C
        auto rec = std::make_shared<ChainEpochObs>();
        rec->armed = true;
        rec->comps.push_back(component(0, "A", {0}, {"G01"}));
        rec->comps.push_back(component(1, "B", {1, 2}, {"G02"}));
        rec->comps[1].r1_station = 1;
        ChainEdgeObs g2 = edge(1, "G02", true, 1);
        g2.r1 = "B";
        ChainEdgeObs c = edge(2, "G02", false, 1);
        c.r1 = "B";
        rec->edges = {edge(0, "G01", true), g2, c};
        r.observeRecord(261030.0, rec);
    }
    auto rec = std::make_shared<ChainEpochObs>();
    rec->armed = true;
    rec->comps.push_back(component(0, "A", {0, 1, 2}, {"G01", "G02"}));
    ChainEdgeObs join = edge(2, "G01", false);
    join.had_parent = true;
    join.prev_parent = "S:G02";
    join.legacy_known = true;
    join.legacy_pinmove_m = 0.3 * kLambda;
    join.legacy_target_m = 5.0 * kLambda;
    join.target_m = 5.0 * kLambda;
    ChainEdgeObs g2 = heldEdge(2, "G02", true);
    g2.incumbent_same = false;
    g2.incumbent_flipped = true;
    g2.absorbed = released;
    g2.root_unknown = true;
    ChainEdgeObs b = heldEdge(1, "G02", false);
    b.incumbent_same = false;
    b.incumbent_flipped = true;
    b.absorbed = released;
    if (released) {
        join.row_armed = join.merge_released = true;
        join.known = false;
        join.pinmove_m = 0.0;
        join.step_m = std::numeric_limits<double>::quiet_NaN();
        join.merge_from = "B";
        join.merge_into = "A";
        ChainReleaseObs rel;
        rel.row = ChainReleaseRow::Merge;
        rel.group = kGroup;
        rel.signal = "L1C";
        rel.station = 2;
        rel.sat = "G01";
        rel.vertex = "R:2";
        rel.sats = {"G02"};
        rel.stations = {2, 1};
        rel.arcs = 4;
        rel.rpb = 2;
        rel.pb = 1;
        rec->releases = {rel};
    } else {
        join.pinmove_m = 0.3 * kLambda;
        g2.row_armed = false;
        g2.known = false;
    }
    rec->edges = {edge(0, "G01", true), join, g2, b};
    r.observeRecord(261060.0, rec);
}

// The row's release writes a leave row naming the absorbed old root and a join row on the new edge, then the release's
// cut_release and declared_keys rows; the edges inside the side write no event of their own.
TEST(NetworkChainFrame, TheObserverWritesTheRowsOfAnEdgeTheMergeRowReleased) {
    ChainObserverRun r("merge_obs_released", std::vector<std::string>{"kept", "root", "merge_split"});
    observeMerge(r, /*released=*/true);
    const ChainRows ev(r);
    std::vector<std::size_t> merge_rows, cut_rows, declared_rows;
    for (std::size_t k = 0; k < ev.rows.size(); ++k) {
        if (ev.at(k, ev.event) == "merge") merge_rows.push_back(k);
        if (ev.at(k, ev.event) == "cut_release") cut_rows.push_back(k);
        if (ev.at(k, ev.event) == "declared_keys") declared_rows.push_back(k);
    }
    ASSERT_EQ(merge_rows.size(), 2u);
    EXPECT_EQ(ev.at(merge_rows[0], ev.action), "leave");
    EXPECT_EQ(ev.at(merge_rows[0], ev.station), "B");
    EXPECT_EQ(ev.at(merge_rows[0], ev.sat), "");
    EXPECT_EQ(ev.at(merge_rows[1], ev.action), "join");
    EXPECT_EQ(ev.at(merge_rows[1], ev.station), "C");
    EXPECT_EQ(ev.at(merge_rows[1], ev.sat), "G01");
    for (std::size_t k : merge_rows) {
        const std::string& detail = ev.at(k, ev.detail);
        EXPECT_TRUE(contains(detail, "row=Merge")) << detail;
        EXPECT_TRUE(contains(detail, "old_root=B into_root=A child=C parent=G01")) << detail;
        EXPECT_TRUE(contains(detail, "legacy_known=1")) << detail;
        EXPECT_TRUE(contains(detail, "legacy_target_cyc=5.000000")) << detail;
    }
    ASSERT_EQ(cut_rows.size(), 1u);
    ASSERT_EQ(declared_rows.size(), 1u);
    EXPECT_EQ(ev.at(declared_rows[0], ev.sat), "G02");
    EXPECT_EQ(ev.at(declared_rows[0], ev.detail), "sig=L1C cause=merge root=C");
    const ChainFrameCounters& c = r.fx->ctx().counters;
    EXPECT_EQ(c.cf_mg_pairs, 1);
    EXPECT_EQ(c.cf_mg_asbuilt_other, 1);
    EXPECT_EQ(c.cf_mg_asbuilt_zero + c.cf_mg_asbuilt_whole + c.cf_mg_asbuilt_unknown, 0);
    EXPECT_EQ(c.cf_mg_cut_releases, 1);
    EXPECT_EQ(c.cf_mg_declared_keys, 1);
    EXPECT_EQ(c.cf_rh_cut_releases + c.cf_ret_cut_releases + c.cf_sl_cut_releases, 0);
    EXPECT_EQ(c.cf_merges, 1);
    EXPECT_EQ(c.cf_rows_armed[static_cast<std::size_t>(ChainEventRow::Merge)], 1);
    EXPECT_EQ(c.cf_rows_unknown[static_cast<std::size_t>(ChainEventRow::Merge)], 1);
}

// With the row off the edge reads as the re-host it always did and writes no merge row, an edge classified Merge writes
// the one join row it always had (BirthReturningSlipAndMerge's case), and no counter of the row moves.
TEST(NetworkChainFrame, WithTheRowOffAMergeWritesTheRowsItAlwaysHad) {
    ChainObserverRun r("merge_obs_off", std::vector<std::string>{"kept", "root"});
    observeMerge(r, /*released=*/false);
    const ChainRows ev(r);
    std::size_t n_merge = 0, n_rehost = 0;
    for (std::size_t k = 0; k < ev.rows.size(); ++k) {
        if (ev.at(k, ev.event) == "merge") ++n_merge;
        if (ev.at(k, ev.event) == "rehost") ++n_rehost;
    }
    EXPECT_EQ(n_merge, 0u);
    EXPECT_EQ(n_rehost, 2u);
    const ChainFrameCounters& c = r.fx->ctx().counters;
    EXPECT_EQ(c.cf_mg_pairs + c.cf_mg_cut_releases + c.cf_mg_declared_keys + c.cf_split_pinned, 0);
    EXPECT_EQ(c.cf_rows_armed[static_cast<std::size_t>(ChainEventRow::Merge)], 0);
    EXPECT_EQ(c.cf_merges, 1);
}

/// Observe one tree split at the second epoch: A keeps G01; B and G02 split off, with the record of the root row's pin.
void observeSplit(ChainObserverRun& r, ChainRootOutcome outcome) {
    {
        auto rec = std::make_shared<ChainEpochObs>();
        rec->armed = true;
        rec->comps.push_back(component(0, "A", {0, 1}, {"G01", "G02"}));
        rec->edges = {edge(0, "G01", true), edge(1, "G01", false), edge(1, "G02", true)};
        r.observeRecord(261030.0, rec);
    }
    auto rec = std::make_shared<ChainEpochObs>();
    rec->armed = true;
    rec->comps.push_back(component(0, "A", {0}, {"G01"}));
    rec->comps.push_back(component(1, "B", {1}, {"G02"}));
    rec->comps[1].r1_station = 1;
    ChainEdgeObs g2 = heldEdge(1, "G02", true);
    g2.comp = 1;
    g2.r1 = "B";
    g2.root_unknown = true;
    rec->edges = {edge(0, "G01", true), g2};
    ChainRootObs ro;
    ro.group = kGroup;
    ro.signal = "L1C";
    ro.comp = 1;
    ro.old_root = "A";
    ro.new_root = "B";
    ro.outcome = outcome;
    ro.target_m = 0.07;
    ro.sigma_m = 0.01;
    ro.split = true;
    rec->roots = {ro};
    r.observeRecord(261060.0, rec);
}

// A split piece's rows carry the row's outcome when the row is armed, and the counters split it from the root row's.
TEST(NetworkChainFrame, TheSplitRowsCarryTheOutcomeOfTheRowWhenArmed) {
    {
        ChainObserverRun r("split_obs_on", std::vector<std::string>{"kept", "root", "merge_split"});
        observeSplit(r, ChainRootOutcome::Pinned);
        const ChainRows ev(r);
        std::vector<std::size_t> split_rows;
        for (std::size_t k = 0; k < ev.rows.size(); ++k)
            if (ev.at(k, ev.event) == "split") split_rows.push_back(k);
        ASSERT_EQ(split_rows.size(), 2u);
        EXPECT_EQ(ev.at(split_rows[0], ev.action), "leave");
        EXPECT_EQ(ev.at(split_rows[0], ev.station), "A");
        EXPECT_EQ(ev.at(split_rows[1], ev.action), "join");
        EXPECT_EQ(ev.at(split_rows[1], ev.station), "B");
        for (std::size_t k : split_rows)
            EXPECT_TRUE(contains(ev.at(k, ev.detail), "old_root=A new_root=B outcome=pinned rpb_target_m=0.070000 "
                                                      "rpb_sigma_m=0.010000")) << ev.at(k, ev.detail);
        const ChainFrameCounters& c = r.fx->ctx().counters;
        EXPECT_EQ(c.cf_split_pinned, 1);
        EXPECT_EQ(c.cf_root_pinned, 1);
        EXPECT_EQ(c.cf_splits, 1);
        EXPECT_EQ(c.cf_rows[static_cast<std::size_t>(ChainEventRow::Split)], 1);
    }
    {
        ChainObserverRun r("split_obs_nocarry", std::vector<std::string>{"kept", "root", "merge_split"});
        observeSplit(r, ChainRootOutcome::NoCarry);
        const ChainFrameCounters& c = r.fx->ctx().counters;
        EXPECT_EQ(c.cf_split_pinned, 0);
        EXPECT_EQ(c.cf_split_no_carry, 1);
        EXPECT_EQ(c.cf_root_no_carry, 1);
    }
    {   // the row off: the rows the split always had
        ChainObserverRun r("split_obs_off", std::vector<std::string>{"kept", "root"});
        observeSplit(r, ChainRootOutcome::NotKeeper);
        const ChainRows ev(r);
        for (std::size_t k = 0; k < ev.rows.size(); ++k)
            if (ev.at(k, ev.event) == "split")
                EXPECT_FALSE(contains(ev.at(k, ev.detail), "outcome=")) << ev.at(k, ev.detail);
        EXPECT_EQ(r.fx->ctx().counters.cf_split_pinned + r.fx->ctx().counters.cf_split_no_carry, 0);
        EXPECT_EQ(r.fx->ctx().counters.cf_root_not_keeper, 1);
    }
}

// The run-end reports name the row only when it is armed, and call a run with no component event no event.
TEST(NetworkChainFrame, TheRunEndsNameTheMergeAndSplitRowOnlyWhenArmed) {
    {
        ChainObserverRun r("merge_end_on", std::vector<std::string>{"kept", "rehost", "root", "merge_split"});
        observeMerge(r, /*released=*/true);
        const std::string text = r.runEnd(&networkChainFrameRunEnd);
        EXPECT_TRUE(contains(text, "event row(s) armed: kept, rehost, root, merge_split; the other rows ran the as-built rule")) << text;
        EXPECT_TRUE(contains(text, "4 merge row(s) written to the event trace: 1 edge(s) joining two old trees released")) << text;
        EXPECT_TRUE(contains(text, "cut and declared by the rehost and merge and split rows")) << text;
    }
    {
        ChainObserverRun r("merge_end_off", std::vector<std::string>{"kept", "root"});
        observeMerge(r, /*released=*/false);
        const std::string text = r.runEnd(&networkChainFrameRunEnd);
        EXPECT_FALSE(contains(text, "merge row(s) written")) << text;
    }
    {
        ChainObserverRun r("merge_elect_on", std::vector<std::string>{"root", "merge_split"});
        auto& c = r.fx->ctx().counters;
        c.eq34_split_pinned = 2;
        c.eq34_split_no_carry = 1;
        c.eq34_mg_released = 3;
        c.eq34_mg_with_state = 2;
        c.eq34_mg_arcs = 9;
        c.eq34_mg_rpb = 4;
        c.eq34_mg_pb = 5;
        c.eq34_mg_declared = 6;
        c.eq34_mg_absorbed_held = 7;
        c.eq34_mg_asbuilt_unknown = 3;
        const std::string text = r.runEnd(&networkChainDatumRunEnd);
        EXPECT_TRUE(contains(text, "Network (eq34 merge and split row): 2 split piece(s) pinned at the new root's carried "
                                   "receiver phase bias, 1 left as built")) << text;
        EXPECT_TRUE(contains(text, "3 tree edge(s) joining two old trees released, 2 of them with state")) << text;
        EXPECT_TRUE(contains(text, "those releases erased 9 leveling carry(ies), dropped 4 receiver phase-bias carry(ies), "
                                   "took 5 satellite phase-bias prior(s) and declared 6 (satellite, signal) key(s) moved, "
                                   "and the kept row held 7 tree edge(s) inside the absorbed sides at their targets")) << text;
    }
    {
        ChainObserverRun r("merge_elect_none", std::vector<std::string>{"root", "merge_split"});
        const std::string text = r.runEnd(&networkChainDatumRunEnd);
        EXPECT_TRUE(contains(text, "Network (eq34 merge and split row): ARMED, and no tree edge joined two trees")) << text;
        EXPECT_FALSE(contains(text, "INERT")) << text;
    }
    {
        ChainObserverRun r("merge_elect_off", std::vector<std::string>{"root"});
        const std::string text = r.runEnd(&networkChainDatumRunEnd);
        EXPECT_FALSE(contains(text, "merge and split row")) << text;
    }
}

TEST(NetworkChainFrame, TheElectionRunEndReportsTheSlipRowOnlyWhenArmed) {
    {
        ChainObserverRun r("slip_elect_on", std::vector<std::string>{"slip"});
        auto& c = r.fx->ctx().counters;
        c.eq34_sl_deseated = 6;
        c.eq34_sl_deseated_family = 2;
        c.eq34_sl_by_cause = {1, 2, 3, 4, 5, 6, 7, 8};
        c.eq34_sl_released = 3;
        c.eq34_sl_birth = 1;
        c.eq34_sl_with_state = 2;
        c.eq34_sl_arcs = 9;
        c.eq34_sl_rpb = 1;
        c.eq34_sl_pb = 4;
        c.eq34_sl_declared = 5;
        c.eq34_sl_asbuilt_zero = 1;
        c.eq34_sl_asbuilt_whole = 1;
        c.eq34_sl_asbuilt_frac = 0;
        c.eq34_sl_asbuilt_unknown = 1;
        const std::string text = r.runEnd(&networkChainDatumRunEnd);
        EXPECT_TRUE(contains(text, "Network (eq34 slip row): 6 slipped tree edge(s) de-seated at the slip epoch, 2 of them "
                                   "on a band whose own arc did not slip")) << text;
        EXPECT_TRUE(contains(text, "3 edge(s) on a slipped arc released because no other route reached the child, 2 of them "
                                   "with state")) << text;
        EXPECT_TRUE(contains(text, "those releases erased 9 leveling carry(ies), dropped 1 receiver phase-bias carry(ies), "
                                   "took 4 satellite phase-bias prior(s) and declared 5 (satellite, signal) key(s) moved"))
            << text;
        EXPECT_TRUE(contains(text, "de-seated and released edges by the latch's cause bit: MW 1, LLI 2, IF-TD 3, RAIM "
                                   "attribution 4, RAIM consensus 5, wind-up restart 6, gap re-entry 7, GF jump 8")) << text;
    }
    {
        ChainObserverRun r("slip_elect_none", std::vector<std::string>{"slip"});   // armed, no tree edge slipped
        const std::string text = r.runEnd(&networkChainDatumRunEnd);
        EXPECT_TRUE(contains(text, "Network (eq34 slip row): ARMED, and no tree edge of any election had its arc in the "
                                   "free-epoch latch")) << text;
        EXPECT_FALSE(contains(text, "INERT")) << text;
    }
    {
        ChainObserverRun r("slip_elect_off", std::vector<std::string>{"kept", "returning", "rehost", "root"});
        auto& c = r.fx->ctx().counters;
        c.eq34_sl_deseated = 3;   // never read with the row off
        const std::string text = r.runEnd(&networkChainDatumRunEnd);
        EXPECT_FALSE(contains(text, "eq34 slip row")) << text;
    }
}

}  // namespace
}  // namespace GnssProcessor
