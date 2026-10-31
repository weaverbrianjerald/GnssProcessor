/**
 * @file test_network_chain_frame.cpp
 * @brief The frame observatory of the eq34 chain datum: the chain-event table, the two-band
 *        certification of a chord and the classification of an epoch's tree against the
 *        memory of the last observed epoch.
 */

#include "network/solve/ChainFrame.hpp"

#include "filter/StateKeySchema.hpp"

#include <gtest/gtest.h>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

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
    const std::set<std::string> events{"", "rehost", "root_handover", "returning", "slip_deseat",
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
}

TEST(NetworkChainFrame, CertificationDeterminesAPreciseChordAndNotAnImpreciseOne) {
    ChainChordRow r;
    // sigma 0.01 cycle per band (0.02 after the scale), correlated: both bands determined.
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
    cnt.chain_frame_memory = memoryOf("A", {"R:0", "R:1", "S:G01", "S:G02"});
    ChainEpochObs obs;
    obs.armed = true;
    obs.comps.push_back(component(0, "A", {0}, {"G01"}));
    obs.edges = {edge(0, "G01", true)};
    const ChainEpochClass cls = chainClassifyEpoch(obs, cnt.chain_frame_memory, kNoFree, kNoCause, cnt);
    chainRememberEpoch(obs, cls, {}, 2313, 300.0, 200.0, cnt);
    const ChainFrameMemory& m = cnt.chain_frame_memory;
    EXPECT_TRUE(m.have);
    EXPECT_EQ(m.vertex_comp.at(kGroup).size(), 2u);
    EXPECT_EQ(m.vertex_parent.at(kGroup).at("S:G01"), "R:0");
    EXPECT_DOUBLE_EQ(m.last_seen_s.at(kGroup).at("S:G01"), 200.0);
    EXPECT_DOUBLE_EQ(m.last_seen_s.at(kGroup).at("S:G02"), 100.0);   // kept from before
    EXPECT_EQ(cnt.cf_departed, 2);   // R:1 and S:G02
    EXPECT_EQ(cnt.cf_epochs, 1);
}

}  // namespace
}  // namespace GnssProcessor
