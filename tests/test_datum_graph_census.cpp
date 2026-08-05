/**
 * @file test_datum_graph_census.cpp
 * @brief THE DATUM GRAPH CENSUS — structural S-basis dimension, predicted from
 *        topology and reconciled against the spectrum
 *        (docs/SPEC_datum_graph_census.md §5).
 *
 * This card is a PREDICTOR wired next to a DETECTOR, and every way it can be
 * wrong is quiet:
 *
 *   - a union-find bug that merges two disjoint networks reports k = 1, which is
 *     the healthy reading, so the one epoch the census exists for looks normal;
 *   - a tier-2 predicate that fires on every cell (the no-code-bias-layer case)
 *     produces hundreds of confident predictions that are all noise, and the
 *     contingency then reads as a catastrophically bad predictor rather than as
 *     a mis-armed one;
 *   - an over-claiming predicate that swallows the weak-support class would make
 *     `observed only` read empty, i.e. would delete exactly the measurement of
 *     this predictor's honest boundary;
 *   - a contingency that double-counts the many-to-many join produces a coverage
 *     percentage that cannot be checked against anything.
 *
 * So the tests pin the CLAIMS, not the arithmetic running:
 *
 *   1. Component multiplicity is the LASER 5-63 quantity — two disjoint station
 *      groups read k = 2, one bridging edge collapses them, and the predicted
 *      deficiency is (k−1)·d.
 *   2. The tier-2 predicate localizes to the exact (sat, family) whose code keys
 *      left, and restoring ONE code key clears it.
 *   3. The census UNDER-claims: thin-but-nonzero support predicts nothing, and a
 *      constellation with no code-bias layer at all predicts nothing either
 *      (the failure mode that would otherwise flood the run).
 *   4. The two contingency partitions are exact, over their own populations, and
 *      the `predicted only` label follows the KP arming and nothing else.
 *   5. OFF is structurally inert: default knobs, no resolved arm, nothing
 *      accumulated — and the trace/loader wiring exists (an armed config loads
 *      and resolves).
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkDatumGraph.hpp"
#include "filter/StateKeySchema.hpp"
#include "logging/LogManager.hpp"

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

// --- graph fixtures ---------------------------------------------------------

/// Fully connect @p stations to @p sats — the healthy global-roster shape.
void connect(std::vector<NetDatumGraphEdge>& e, const std::vector<int>& stations,
             const std::vector<std::string>& sats) {
    for (int s : stations)
        for (const auto& v : sats) e.push_back(NetDatumGraphEdge{s, v});
}

/// The component containing @p sat, or nullptr.
const NetDatumGraphComponent* componentWith(const NetDatumGraphConsCensus& t1,
                                            const std::string& sat) {
    for (const auto& c : t1.comps)
        if (c.sample_sat == sat) return &c;
    return nullptr;
}

// --- registration fixtures --------------------------------------------------

std::string pbKey(const std::string& sat, const std::string& sig) {
    return StateKeys::makeSatPhaseBiasKey(sat, sig);
}

/// Find the (sat, family) cell in a prediction list.
const NetDatumGraphPrediction* cellOf(const std::vector<NetDatumGraphPrediction>& v,
                                      const std::string& id) {
    for (const auto& p : v)
        if (p.id() == id) return &p;
    return nullptr;
}

std::vector<std::string> predictedIds(const std::vector<NetDatumGraphPrediction>& v) {
    std::vector<std::string> out;
    for (const auto& p : v)
        if (p.zero_keys) out.push_back(p.id());
    return out;
}

/// A direction carrying the given identities (the shape SBC hands over).
NetDatumGraphDir dir(const std::vector<std::string>& ids, double ev = 1e-7,
                     const std::string& top = "PB:E03:L5X") {
    NetDatumGraphDir d;
    d.ev        = ev;
    d.top_label = top;
    d.ids       = ids;
    std::sort(d.ids.begin(), d.ids.end());
    return d;
}

// --- config-load fixtures (the house pattern) -------------------------------

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("netdg_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

std::string networkConfig(const std::string& extra = "") {
    return std::string(
        "{\n"
        "  \"processing\": {\n"
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
        "}\n");
}

bool loadConfig(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = writeTempConfig(tag, body);
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec;
    fs::remove(p, ec);
    return ok;
}

}  // namespace

// ===========================================================================
//  1. TIER 1 — component multiplicity is the LASER 5-63 quantity
// ===========================================================================

// Two station groups tracking two disjoint satellite sets: k = 2, and the
// declared plan (which assumes k = 1) is short by (k−1)·d gauges.  This is the
// disjoint-network case slide 5-63 states as "29 independent networks ⇒ 87".
TEST(NetworkDatumGraphCensus, DisjointStationGroupsReadTwoComponents) {
    std::vector<NetDatumGraphEdge> e;
    connect(e, {0, 1, 2}, {"G01", "G02"});          // group A
    connect(e, {7, 8},    {"G20", "G21", "G22"});   // group B — no shared satellite

    const auto cen = netDatumGraphComponents(e);
    ASSERT_EQ(cen.size(), 1u) << "one constellation";
    const NetDatumGraphConsCensus& t1 = cen.at('G');
    EXPECT_EQ(t1.cons, 'G');
    EXPECT_EQ(t1.k, 2);
    EXPECT_EQ(t1.n_stations, 5);
    EXPECT_EQ(t1.n_sats, 5);
    EXPECT_EQ(t1.n_edges, 3 * 2 + 2 * 3);

    // The LASER prediction, with the producer's d = 1 satellite-clock gauge per
    // constellation.
    const int d = 1;
    EXPECT_EQ((t1.k - 1) * d, 1);

    // Both components are reported with their rosters, LARGEST FIRST — a
    // fragmentation event that cannot name the smaller island is not actionable.
    ASSERT_EQ(t1.comps.size(), 2u);
    EXPECT_GE(t1.comps[0].n_stations + t1.comps[0].n_sats,
              t1.comps[1].n_stations + t1.comps[1].n_sats);
    const NetDatumGraphComponent* a = componentWith(t1, "G01");
    const NetDatumGraphComponent* b = componentWith(t1, "G20");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->n_stations, 3);
    EXPECT_EQ(a->n_sats, 2);
    EXPECT_EQ(a->sample_station, 0);
    EXPECT_EQ(b->n_stations, 2);
    EXPECT_EQ(b->n_sats, 3);
    EXPECT_EQ(b->sample_station, 7);
}

// ONE bridging edge — a single station that tracks one satellite from each
// island — collapses the multiplicity.  That is the whole content of the
// rank-deficiency statement: connectivity, not counts.
TEST(NetworkDatumGraphCensus, OneBridgingEdgeCollapsesTheMultiplicity) {
    std::vector<NetDatumGraphEdge> e;
    connect(e, {0, 1, 2}, {"G01", "G02"});
    connect(e, {7, 8},    {"G20", "G21", "G22"});
    ASSERT_EQ(netDatumGraphComponents(e).at('G').k, 2);

    e.push_back(NetDatumGraphEdge{2, "G20"});   // station 2 also sees G20
    const auto cen = netDatumGraphComponents(e);
    const NetDatumGraphConsCensus& t1 = cen.at('G');
    EXPECT_EQ(t1.k, 1);
    EXPECT_EQ((t1.k - 1) * 1, 0) << "a connected graph predicts NO extra deficiency";
    ASSERT_EQ(t1.comps.size(), 1u);
    EXPECT_EQ(t1.comps[0].n_stations, 5);
    EXPECT_EQ(t1.comps[0].n_sats, 5);
}

// Constellations are independent graphs.  A Galileo outage must not read as a
// GPS fragmentation, and vice versa — the gauges are per-constellation.
TEST(NetworkDatumGraphCensus, ConstellationsAreSeparateGraphs) {
    std::vector<NetDatumGraphEdge> e;
    connect(e, {0, 1}, {"G01", "G02"});
    connect(e, {0},    {"E03"});          // one station, one Galileo satellite
    connect(e, {5},    {"E11"});          // an island of its own

    const auto cen = netDatumGraphComponents(e);
    ASSERT_EQ(cen.size(), 2u);
    EXPECT_EQ(cen.at('G').k, 1);
    EXPECT_EQ(cen.at('E').k, 2) << "the two Galileo islands share no station";
    EXPECT_EQ(cen.at('E').n_stations, 2);
    EXPECT_EQ(cen.at('E').n_sats, 2);
}

// Vertices with no edge are not components.  Counting an untracked satellite or
// a station that folded nothing would inflate k with vertices carrying no datum
// — a fragmentation alarm that fires on an empty roster.
TEST(NetworkDatumGraphCensus, EdgelessInputYieldsNoConstellations) {
    EXPECT_TRUE(netDatumGraphComponents({}).empty());
    // Malformed edges are dropped, never guessed into a vertex.
    std::vector<NetDatumGraphEdge> e{{-1, "G01"}, {0, ""}};
    EXPECT_TRUE(netDatumGraphComponents(e).empty());
}

// Determinism: the same epoch must produce the same rows whatever order the
// driver assembled stations in.  A diagnostic that is the one irreproducible
// artifact in the run is worse than none.
TEST(NetworkDatumGraphCensus, ComponentOrderIsIndependentOfEdgeOrder) {
    std::vector<NetDatumGraphEdge> e;
    connect(e, {0, 1, 2}, {"G01", "G02"});
    connect(e, {7, 8},    {"G20", "G21", "G22"});
    std::vector<NetDatumGraphEdge> shuffled(e.rbegin(), e.rend());

    const auto a = netDatumGraphComponents(e).at('G');
    const auto b = netDatumGraphComponents(shuffled).at('G');
    ASSERT_EQ(a.comps.size(), b.comps.size());
    for (std::size_t i = 0; i < a.comps.size(); ++i) {
        EXPECT_EQ(a.comps[i].n_stations, b.comps[i].n_stations);
        EXPECT_EQ(a.comps[i].n_sats,     b.comps[i].n_sats);
        EXPECT_EQ(a.comps[i].sample_sat, b.comps[i].sample_sat);
        EXPECT_EQ(a.comps[i].sample_station, b.comps[i].sample_station);
    }
}

// ===========================================================================
//  2. TIER 2 — the channel-family predicate
// ===========================================================================

// The family partition is the RINEX-3 tracking attribute — the thing that split
// the STATION set on Aug-2.  A two-character RINEX-2 code has no attribute and
// forms its OWN family: merging it into an attributed one would silently credit
// an unattributed code column with supporting an X-family phase bias.
TEST(NetworkDatumGraphCensus, FamilyIsTheRinexTrackingAttribute) {
    EXPECT_EQ(netDatumGraphFamily("L5X"), "X");
    EXPECT_EQ(netDatumGraphFamily("C5X"), "X");
    EXPECT_EQ(netDatumGraphFamily("L7Q"), "Q");
    EXPECT_EQ(netDatumGraphFamily("C1C"), "C");
    EXPECT_EQ(netDatumGraphFamily("L1"),  kDatumGraphNoFamily);
    EXPECT_EQ(netDatumGraphFamily(""),    kDatumGraphNoFamily);
    EXPECT_EQ(netDatumGraphFamily("C1"),  kDatumGraphNoFamily);
}

// The E03-style toy: the X-family stations drop out of code registration while
// the X-family PHASE-bias columns stay registered.  Exactly ONE direction is
// predicted, with the right (sat, family), and the Q-family cell — whose code
// keys are intact — predicts nothing.
TEST(NetworkDatumGraphCensus, XFamilyLosingItsCodeKeysPredictsOneDirection) {
    const std::set<std::string> pb{
        pbKey("E03", "L5X"), pbKey("E03", "L7X"), pbKey("E03", "L8X"),
        pbKey("E03", "L5Q"), pbKey("E03", "L7Q"),
    };
    // Q-family code columns survive; every X-family code column is gone.
    const std::vector<std::pair<std::string, std::string>> cb{
        {"E03", "C5Q"}, {"E03", "C7Q"},
    };
    std::map<std::string, int> support{
        {netDatumGraphSupportKey("E03", "Q"), 40},
    };

    const auto cells = netDatumGraphChannelFamilies(pb, cb, support);
    ASSERT_EQ(cells.size(), 2u) << "two families carry phase bias on E03";

    const NetDatumGraphPrediction* x = cellOf(cells, "E03/X");
    const NetDatumGraphPrediction* q = cellOf(cells, "E03/Q");
    ASSERT_NE(x, nullptr);
    ASSERT_NE(q, nullptr);

    EXPECT_TRUE(x->cb_layer) << "the constellation registers code columns";
    EXPECT_EQ(x->n_pb, 3);
    EXPECT_EQ(x->n_cb, 0);
    EXPECT_EQ(x->n_code_support, 0);
    EXPECT_TRUE(x->zero_keys);
    EXPECT_TRUE(x->zero_support) << "zero_keys must imply zero_support";

    EXPECT_EQ(q->n_pb, 2);
    EXPECT_EQ(q->n_cb, 2);
    EXPECT_EQ(q->n_code_support, 40);
    EXPECT_FALSE(q->zero_keys);
    EXPECT_FALSE(q->zero_support);

    EXPECT_EQ(predictedIds(cells), (std::vector<std::string>{"E03/X"}));
    // And the list is ascending by identity, which the CSV and the join both
    // rely on for determinism.
    std::vector<std::string> ids;
    for (const auto& c : cells) ids.push_back(c.id());
    EXPECT_TRUE(std::is_sorted(ids.begin(), ids.end()));
}

// Restoring ONE X-family code key clears the prediction.  The predicate is about
// the presence of an anchor, not about how strong it is — strength is the
// spectral side's question.
TEST(NetworkDatumGraphCensus, RestoringOneCodeKeyClearsThePrediction) {
    const std::set<std::string> pb{
        pbKey("E03", "L5X"), pbKey("E03", "L7X"), pbKey("E03", "L8X"),
    };
    std::vector<std::pair<std::string, std::string>> cb{{"E03", "C5Q"}};
    EXPECT_EQ(predictedIds(netDatumGraphChannelFamilies(pb, cb, {})),
              (std::vector<std::string>{"E03/X"}));

    cb.push_back({"E03", "C5X"});
    const auto cells = netDatumGraphChannelFamilies(
        pb, cb, {{netDatumGraphSupportKey("E03", "X"), 1}});
    EXPECT_TRUE(predictedIds(cells).empty());
    const NetDatumGraphPrediction* x = cellOf(cells, "E03/X");
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->n_cb, 1);
    EXPECT_FALSE(x->zero_keys);
    EXPECT_FALSE(x->zero_support);
}

// A cell with code columns but no phase bias is not a cell of this census: it
// cannot host the b<->I trade the predicate names.
TEST(NetworkDatumGraphCensus, CellsWithoutPhaseBiasAreNotCensused) {
    const std::vector<std::pair<std::string, std::string>> cb{{"G01", "C1W"},
                                                              {"G01", "C2W"}};
    EXPECT_TRUE(netDatumGraphChannelFamilies({}, cb, {}).empty());
}

// ===========================================================================
//  3. THE BOUNDARY — the census must UNDER-claim, never over-claim
// ===========================================================================

// Thin-but-nonzero support is the Aug-2 ORPHAN class (eigenvalue ~1e-6, well
// below the cutoff, invisible to topology because the column and its support
// both exist).  Predicting it would be a claim about WEIGHTS that this card
// deliberately does not make; the `observed only` cell is where it is supposed
// to land instead.
TEST(NetworkDatumGraphCensus, ThinButNonzeroSupportPredictsNothing) {
    const std::set<std::string> pb{pbKey("E03", "L5X"), pbKey("E03", "L7X")};
    const std::vector<std::pair<std::string, std::string>> cb{{"E03", "C5X"}};
    // A single admitted code observable across the whole network — as thin as
    // support gets without being zero.
    const auto cells = netDatumGraphChannelFamilies(
        pb, cb, {{netDatumGraphSupportKey("E03", "X"), 1}});
    EXPECT_TRUE(predictedIds(cells).empty());
    const NetDatumGraphPrediction* x = cellOf(cells, "E03/X");
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->n_code_support, 1);
    EXPECT_FALSE(x->zero_support) << "one observable is support; thinness is not this card's call";
}

// THE flooding failure mode.  With no satellite code-bias column anywhere, the
// literal predicate ("no code key in this family") is true of EVERY cell — and
// meaningless, because the code observations then pin the slant ionosphere
// directly and no trade direction exists.  Both predicates must be held off, and
// the cell must still be REPORTED (cb_layer = false) rather than vanish.
TEST(NetworkDatumGraphCensus, NoCodeBiasLayerHoldsBothPredicatesOff) {
    const std::set<std::string> pb{
        pbKey("E03", "L5X"), pbKey("E03", "L7Q"), pbKey("G01", "L1W"),
    };
    const auto cells = netDatumGraphChannelFamilies(pb, {}, {});
    ASSERT_EQ(cells.size(), 3u) << "the cells still exist and are reported";
    for (const auto& c : cells) {
        EXPECT_FALSE(c.cb_layer);
        EXPECT_FALSE(c.zero_keys);
        EXPECT_FALSE(c.zero_support);
    }
    EXPECT_TRUE(predictedIds(cells).empty());
}

// The layer precondition is PER CONSTELLATION: Galileo estimating code biases
// must not lend its layer to a GPS cell that has none, or the flooding failure
// returns one constellation at a time.
TEST(NetworkDatumGraphCensus, TheCodeBiasLayerIsPerConstellation) {
    const std::set<std::string> pb{pbKey("E03", "L5X"), pbKey("G01", "L1W")};
    const std::vector<std::pair<std::string, std::string>> cb{{"E11", "C1C"}};
    const auto cells = netDatumGraphChannelFamilies(pb, cb, {});
    const NetDatumGraphPrediction* e = cellOf(cells, "E03/X");
    const NetDatumGraphPrediction* g = cellOf(cells, "G01/W");
    ASSERT_NE(e, nullptr);
    ASSERT_NE(g, nullptr);
    EXPECT_TRUE(e->cb_layer);
    EXPECT_TRUE(e->zero_keys);
    EXPECT_FALSE(g->cb_layer);
    EXPECT_FALSE(g->zero_keys) << "GPS has no code-bias layer of its own";
}

// ===========================================================================
//  4. THE CONTINGENCY — exact, and over the right populations
// ===========================================================================

// Identity parsing rides SBC's OWN attribution string, so the census and the
// sbc_engage event can never disagree about what a direction is.  Only the two
// BIAS kinds carry a signal; a clock or SH participant contributes no identity.
TEST(NetworkDatumGraphCensus, DirectionIdentitiesComeFromTheSbcAttribution) {
    // The measured FT63 GPS+Gal engage line, verbatim in shape.
    const auto ids = netDatumGraphDetailIds(
        "PB:E03:L5X:0.35,CB:E03:C5X:0.35,PB:E03:L8X:0.34,CB:E03:C8X:0.34,"
        "PB:E03:L7X:0.33,CB:E03:C7X:0.33");
    EXPECT_EQ(ids, (std::vector<std::string>{"E03/X"}))
        << "one satellite, one family — deduplicated across six participants";

    const auto mixed = netDatumGraphDetailIds(
        "CLK:E08:0.71,PB:E08:L1C:0.40,SH:C:0:0:0.30,PB:E36:L7Q:0.28");
    EXPECT_EQ(mixed, (std::vector<std::string>{"E08/C", "E36/Q"}))
        << "clock and SH participants carry no (sat, family) identity";

    EXPECT_TRUE(netDatumGraphDetailIds("").empty());
    EXPECT_TRUE(netDatumGraphDetailIds("SH:C:0:0:0.99").empty());
}

// The join is many-to-many, so the two partitions are over DIFFERENT
// populations and each must be exact on its own.  Adding them into a single 2x2
// would require double-counting one side — the accounting error §5.4 pins.
TEST(NetworkDatumGraphCensus, TheThreeCellsPartitionBothPopulationsExactly) {
    const std::vector<std::string> pred{"E03/X", "E08/Q", "E14/X"};
    const std::vector<NetDatumGraphDir> dirs{
        dir({"E03/X"}),                    // matches one prediction
        dir({"E03/X", "E14/X"}),           // matches TWO — the many-to-many case
        dir({"E36/C"}),                    // matches none: observed only
        dir({}, 1e-14, "CLK:E22"),         // no identity at all: observed only
    };

    const auto c = netDatumGraphReconcile(pred, dirs);

    // Over PREDICTIONS.
    EXPECT_EQ(c.pred_observed, (std::vector<std::string>{"E03/X", "E14/X"}));
    EXPECT_EQ(c.pred_only,     (std::vector<std::string>{"E08/Q"}));
    EXPECT_EQ(c.pred_observed.size() + c.pred_only.size(), pred.size());

    // Over DIRECTIONS.
    EXPECT_EQ(c.n_obs_matched, 2);
    ASSERT_EQ(c.obs_only.size(), 2u);
    EXPECT_EQ(c.obs_only[0], "CLK:E22") << "an unnamed direction is named by its heaviest column";
    EXPECT_EQ(c.obs_only[1], "E36/C");
    EXPECT_EQ(c.obs_only.size() + static_cast<std::size_t>(c.n_obs_matched), dirs.size());
}

// Nothing is double-counted when the same prediction is named by several
// directions, and duplicate predictions collapse.
TEST(NetworkDatumGraphCensus, RepeatedMatchesAreCountedOnce) {
    const std::vector<std::string> pred{"E03/X", "E03/X"};
    const std::vector<NetDatumGraphDir> dirs{dir({"E03/X"}), dir({"E03/X"}),
                                             dir({"E03/X"})};
    const auto c = netDatumGraphReconcile(pred, dirs);
    EXPECT_EQ(c.pred_observed, (std::vector<std::string>{"E03/X"}));
    EXPECT_TRUE(c.pred_only.empty());
    EXPECT_EQ(c.n_obs_matched, 3) << "each DIRECTION is matched, once";
    EXPECT_TRUE(c.obs_only.empty());
}

// The degenerate arms, both of which are real configurations: nothing predicted
// (a healthy epoch — every direction is `observed only`) and nothing observed
// (the SBC+KP recipe — every prediction is `predicted only`, which IS the map).
TEST(NetworkDatumGraphCensus, EmptySidesDegradeIntoTheRightCell) {
    const auto no_pred = netDatumGraphReconcile({}, {dir({"E03/X"})});
    EXPECT_TRUE(no_pred.pred_observed.empty());
    EXPECT_TRUE(no_pred.pred_only.empty());
    EXPECT_EQ(no_pred.obs_only, (std::vector<std::string>{"E03/X"}));
    EXPECT_EQ(no_pred.n_obs_matched, 0);

    const auto no_obs = netDatumGraphReconcile({"E03/X", "E08/Q"}, {});
    EXPECT_TRUE(no_obs.pred_observed.empty());
    EXPECT_EQ(no_obs.pred_only, (std::vector<std::string>{"E03/X", "E08/Q"}));
    EXPECT_TRUE(no_obs.obs_only.empty());
    EXPECT_EQ(no_obs.n_obs_matched, 0);
}

// The `predicted only` cell means two DIFFERENT things and the label must follow
// the KP arming and nothing else: with KP it is the load-bearing map (the
// direction exists, the prior lifts it above the cutoff), without KP it is
// over-prediction — a finding about the predictor.
TEST(NetworkDatumGraphCensus, PredictedOnlyIsLabelledKpLiftedIffKpIsArmed) {
    EXPECT_STREQ(netDatumGraphPredOnlyEvent(true),  "dg_kp_load_bearing");
    EXPECT_STREQ(netDatumGraphPredOnlyAction(true), "kp_lifted");
    EXPECT_STREQ(netDatumGraphPredOnlyEvent(false),  "dg_predicted_only");
    EXPECT_STREQ(netDatumGraphPredOnlyAction(false), "over_predict");
}

// The spectral hand-off is a ONE-EPOCH value and `extras` never goes negative:
// observed < declared is the gate's "over-specified" verdict, a different
// finding, and treating it as −1 extras would corrupt the run totals.
TEST(NetworkDatumGraphCensus, TheSpectralHandoffClearsAndNeverReportsNegativeExtras) {
    NetDatumGraphSpectral s;
    EXPECT_FALSE(s.valid);
    EXPECT_EQ(s.observed, -1);
    EXPECT_EQ(s.extras(), 0);

    s.valid = true; s.from_sbc = true; s.declared = 2; s.observed = 4;
    s.dirs.push_back(dir({"E03/X"}));
    EXPECT_EQ(s.extras(), 2);

    s.observed = 1;                       // over-specified
    EXPECT_EQ(s.extras(), 0);

    s.clear();
    EXPECT_FALSE(s.valid);
    EXPECT_FALSE(s.from_sbc);
    EXPECT_EQ(s.observed, -1);
    EXPECT_EQ(s.declared, 0);
    EXPECT_TRUE(s.dirs.empty());
}

// ===========================================================================
//  5. OFF is structurally inert; ON resolves
// ===========================================================================

TEST(NetworkDatumGraphCensus, StockConfigLeavesEveryCensusPathInert) {
    GnssConfig cfg;
    ASSERT_TRUE(loadConfig("off", networkConfig(), cfg));
    const NetworkConfig& nc = cfg.getNetworkConfig();
    EXPECT_FALSE(nc.datum_graph_census);
    EXPECT_EQ(nc.datum_graph_event_holdoff_ep, 20);

    const NetworkKnobs k = NetworkKnobs::snapshot(cfg, nc);
    EXPECT_FALSE(k.dg_on);
    EXPECT_EQ(k.dg_event_holdoff, 20);

    // And the accumulators are inert until something feeds them.
    NetworkDatumGraphTallies t;
    EXPECT_EQ(t.epochs, 0);
    EXPECT_EQ(t.pred_tier1, 0);
    EXPECT_EQ(t.pred_tier2, 0);
    EXPECT_EQ(t.cell_pred_obs, 0);
    EXPECT_EQ(t.cell_obs_only, 0);
    EXPECT_EQ(t.max_k, 0);
    EXPECT_DOUBLE_EQ(t.ms, 0.0);
}

TEST(NetworkDatumGraphCensus, ArmingResolvesTheKnobAndClampsTheHoldoff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadConfig("on", networkConfig(
        "    \"datum_graph_census\": true,\n"
        "    \"datum_graph_event_holdoff_ep\": 5,\n"), cfg));
    const NetworkConfig& nc = cfg.getNetworkConfig();
    EXPECT_TRUE(nc.datum_graph_census);
    EXPECT_EQ(nc.datum_graph_event_holdoff_ep, 5);
    const NetworkKnobs k = NetworkKnobs::snapshot(cfg, nc);
    EXPECT_TRUE(k.dg_on);
    EXPECT_EQ(k.dg_event_holdoff, 5);

    // A negative holdoff is clamped, not honoured as "never rate-limit by
    // wrapping the comparison".
    GnssConfig neg;
    ASSERT_TRUE(loadConfig("neg", networkConfig(
        "    \"datum_graph_census\": true,\n"
        "    \"datum_graph_event_holdoff_ep\": -3,\n"), neg));
    EXPECT_EQ(neg.getNetworkConfig().datum_graph_event_holdoff_ep, 0);
    EXPECT_EQ(NetworkKnobs::snapshot(neg, neg.getNetworkConfig()).dg_event_holdoff, 0);
}

// The census does NOT require SBC.  Without it the spectral side still supplies
// the observed-vs-declared COUNT and tier 1 needs no spectrum at all, so
// AND-ing the two arms would silently delete the fragmentation reading.
TEST(NetworkDatumGraphCensus, ArmingIsIndependentOfTheSbasisCompletion) {
    GnssConfig cfg;
    ASSERT_TRUE(loadConfig("nosbc", networkConfig(
        "    \"datum_graph_census\": true,\n"), cfg));
    EXPECT_FALSE(cfg.networkSbasisComplete());
    EXPECT_TRUE(NetworkKnobs::snapshot(cfg, cfg.getNetworkConfig()).dg_on);
}
