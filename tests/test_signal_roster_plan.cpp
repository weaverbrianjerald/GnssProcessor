/**
 * @file test_signal_roster_plan.cpp
 * @brief The "signals as planned" table of the signal roster (card NOM step N3a,
 *        docs/refound/NOM2_network_object_model_rows_2026-10-09.md section 4).
 *
 * Since step N3a the roster and its disagreement census are built on every network
 * run, and the table is the readout a reader meets first: per constellation and band,
 * which signal holds which role at how many stations, then one row per decision site
 * with the count of its disagreements with the roster.  Pinned here, over synthetic
 * cells with no station, model or solve:
 *
 *  - every decision site is declared once, in enum order, with all four texts;
 *  - each site's compared and disagreeing counts are read off the counters the
 *    census already keeps, by the one definition the table states;
 *  - the plan sums the cells over the stations role by role, orders each role by
 *    station count, keeps the roster's exclusion-reason order, and counts the datum
 *    designation's votes in satellites;
 *  - a roster built before the latch is decided carries no modelled role, which is
 *    why the first table waits for a later epoch;
 *  - the table prints every role (an empty one as "-") and every site, flags a site
 *    with no case compared, names the run's latch rule on the latch-rule row, and the
 *    manifest record carries the same numbers;
 *  - a run end with no audited epoch says so instead of printing week 0 TOW 0.0.
 */

#include <gtest/gtest.h>

#include "app/SignalRoster.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

/// A station tracking the civil and the semi-codeless band-1 channels plus L2.
const std::vector<std::string> kTracked{ "C1C", "L1C", "C1W", "L1W", "C2W", "L2W", "C2L" };

std::vector<RosterLatchRow> latchOf(const char* phase1, const char* code1) {
    return { RosterLatchRow{ 'L', phase1 }, RosterLatchRow{ 'C', code1 },
             RosterLatchRow{ 'L', "L2W" }, RosterLatchRow{ 'C', "C2W" } };
}

/// Three stations: two latch L1C / C1C with C1W as a companion code, one latches
/// L1W / C1W.  The datum seam pinned C1C on band 1 (two satellites) against C1W (one
/// satellite), and C2W on band 2.
std::vector<StationRoster> threeStations() {
    std::vector<StationRoster> out(3);
    for (int i = 0; i < 3; ++i) {
        const bool civil = i < 2;
        out[static_cast<std::size_t>(i)]["GPS"] = signalRosterBuildCells(
            kTracked, civil ? latchOf("L1C", "C1C") : latchOf("L1W", "C1W"),
            { "C1W" }, /*companions_armed=*/civil, nullptr);
    }
    std::map<std::string, std::pair<std::string, std::string>> pins{
        { "G01", { "C1C", "C2W" } }, { "G02", { "C1C", "C2W" } }, { "G03", { "C1W", "C2W" } } };
    std::set<std::string> ambiguous;
    const auto picks = signalRosterDatumPicks(pins, nullptr, &ambiguous);
    for (StationRoster& sr : out) signalRosterApplyDatum(sr, picks, &ambiguous);
    return out;
}

std::map<std::string, std::map<std::string, long long>> threeStationVotes() {
    return { { "GPS|1", { { "C1C", 2 }, { "C1W", 1 } } }, { "GPS|2", { { "C2W", 3 } } } };
}

const RosterPlanBand* bandOf(const RosterPlan& plan, const std::string& cons, char band) {
    for (const RosterPlanBand& b : plan.bands)
        if (b.constellation == cons && b.band == band) return &b;
    return nullptr;
}

std::vector<std::pair<std::string, int>> asPairs(const std::vector<RosterPlanEntry>& v) {
    std::vector<std::pair<std::string, int>> out;
    for (const RosterPlanEntry& e : v) out.emplace_back(e.signal, e.count);
    return out;
}

using Pairs = std::vector<std::pair<std::string, int>>;

RosterPlanHeader header() {
    RosterPlanHeader h;
    h.scope = "first emitted epoch with a modelled role";
    h.week = 2313;
    h.tow = 259230.0;
    h.epochs = 2;
    h.week_first = 2313;
    h.tow_first = 259200.0;
    h.week_last = 2313;
    h.tow_last = 259230.0;
    return h;
}

}  // namespace

// ---------------------------------------------------------------------------
// The decision sites
// ---------------------------------------------------------------------------

TEST(SignalRosterPlanTest, EverySiteIsDeclaredOnceInEnumOrderWithItsTexts) {
    const auto& specs = signalRosterSiteSpecs();
    ASSERT_EQ(specs.size(), kRosterSites);
    std::set<std::string> names;
    for (std::size_t k = 0; k < specs.size(); ++k) {
        EXPECT_EQ(specs[k].site, static_cast<RosterSite>(k));
        names.insert(specs[k].name);
        const std::string group = specs[k].group;
        EXPECT_TRUE(group == "signal" || group == "arc") << specs[k].name;
        EXPECT_FALSE(std::string(specs[k].where).empty()) << specs[k].name;
        EXPECT_FALSE(std::string(specs[k].compared).empty()) << specs[k].name;
        EXPECT_FALSE(std::string(specs[k].disagreement).empty()) << specs[k].name;
    }
    EXPECT_EQ(names.size(), kRosterSites) << "site names are JSON keys and must be unique";
    // The signal-choice sites come first, then the arc sites, as the table prints them.
    EXPECT_STREQ(specs.front().name, "canonical_latch");
    EXPECT_STREQ(specs.back().name, "coverage_gate");
}

TEST(SignalRosterPlanTest, EachSiteReadsItsDeclaredCounters) {
    NetworkSignalRosterCounters c;
    SignalRoster rs;
    c.latch_cells_checked = 120;            c.roster_latch_disagree = 0;
    rs.rule_reached = { "A", "B", "C" };    rs.rule_differs = { "B" };
    c.trk_rows = 400;                       c.trk_rows_mismatch = 31;
    c.datum_cells = 116;                    c.datum_cells_off_model = 40;
    c.datum_cells_unmodelled = 10;          // first-epoch cells: not comparable
    c.wire_pb_present = { { "L1C", 50 }, { "L2W", 60 } };
    c.wire_pb_missing = { { "L1W", 10 }, { "L2W", 2 } };
    c.wire_cb_present = { { "C1C", 70 } };
    c.wire_cb_missing = { { "C1W", 3 } };
    c.lc_entries = 900;   c.lc_adm_arc_dead = 4;   c.lc_adm_arc_unknown = 5;
    c.amb_states = 800;   c.amb_adm_arc_gapped = 6; c.amb_adm_arc_unknown = 7;
    c.anchor_first_sightings = 20;          c.anchor_first_sighting_reentry = 3;
    c.ans_total = 15;                       c.ans_bridged_nv = 2;
    c.gate_drop_edges = 9;                  c.gate_drop_edges_gapped = 1;

    const RosterSiteCounts n = signalRosterSiteCounts(c, rs);
    const auto at = [&n](RosterSite s) { return n[static_cast<std::size_t>(s)]; };
    EXPECT_EQ(at(RosterSite::CanonicalLatch).compared, 120);
    EXPECT_EQ(at(RosterSite::CanonicalLatch).disagree, 0);
    EXPECT_EQ(at(RosterSite::LatchRule).compared, 3);
    EXPECT_EQ(at(RosterSite::LatchRule).disagree, 1);
    EXPECT_EQ(at(RosterSite::SlipTrackerBand).compared, 400);
    EXPECT_EQ(at(RosterSite::SlipTrackerBand).disagree, 31);
    EXPECT_EQ(at(RosterSite::IfPinPair).compared, 106);
    EXPECT_EQ(at(RosterSite::IfPinPair).disagree, 30);
    EXPECT_EQ(at(RosterSite::WirePhaseBias).compared, 122);   // 50 + 60 present + 12 missing
    EXPECT_EQ(at(RosterSite::WirePhaseBias).disagree, 12);
    EXPECT_EQ(at(RosterSite::WireCodeBias).compared, 73);
    EXPECT_EQ(at(RosterSite::WireCodeBias).disagree, 3);
    EXPECT_EQ(at(RosterSite::LocalCarry).compared, 900);
    EXPECT_EQ(at(RosterSite::LocalCarry).disagree, 9);
    EXPECT_EQ(at(RosterSite::AmbiguityStates).compared, 800);
    EXPECT_EQ(at(RosterSite::AmbiguityStates).disagree, 13);
    EXPECT_EQ(at(RosterSite::AnchorElection).compared, 20);
    EXPECT_EQ(at(RosterSite::AnchorElection).disagree, 3);
    EXPECT_EQ(at(RosterSite::GapReentry).compared, 15);
    EXPECT_EQ(at(RosterSite::GapReentry).disagree, 2);
    EXPECT_EQ(at(RosterSite::CoverageGate).compared, 9);
    EXPECT_EQ(at(RosterSite::CoverageGate).disagree, 1);
}

TEST(SignalRosterPlanTest, TheEpochCountIsTheDifferenceFromTheEpochStart) {
    RosterSiteCounts start{}, now{};
    start[static_cast<std::size_t>(RosterSite::SlipTrackerBand)] = RosterSiteCount{ 300, 20 };
    now[static_cast<std::size_t>(RosterSite::SlipTrackerBand)]   = RosterSiteCount{ 400, 31 };
    now[static_cast<std::size_t>(RosterSite::LatchRule)]         = RosterSiteCount{ 3, 1 };
    const RosterSiteCounts d = signalRosterSiteDelta(now, start);
    EXPECT_EQ(d[static_cast<std::size_t>(RosterSite::SlipTrackerBand)].compared, 100);
    EXPECT_EQ(d[static_cast<std::size_t>(RosterSite::SlipTrackerBand)].disagree, 11);
    EXPECT_EQ(d[static_cast<std::size_t>(RosterSite::LatchRule)].compared, 3);
    EXPECT_EQ(d[static_cast<std::size_t>(RosterSite::CanonicalLatch)].compared, 0);
}

// ---------------------------------------------------------------------------
// The plan
// ---------------------------------------------------------------------------

TEST(SignalRosterPlanTest, ThePlanSumsTheStationsRoleByRole) {
    const RosterPlan plan = signalRosterPlanOf(threeStations(), threeStationVotes());
    EXPECT_EQ(plan.stations, 3);
    EXPECT_TRUE(plan.hasModelledRole());
    ASSERT_EQ(plan.bands.size(), 2u);

    const RosterPlanBand* b1 = bandOf(plan, "GPS", '1');
    ASSERT_NE(b1, nullptr);
    EXPECT_EQ(b1->stations_reporting, 3);
    EXPECT_EQ(b1->stations_modelling, 3);
    // Ordered by station count, highest first.
    EXPECT_EQ(asPairs(b1->phase), (Pairs{ { "L1C", 2 }, { "L1W", 1 } }));
    EXPECT_EQ(asPairs(b1->code), (Pairs{ { "C1C", 2 }, { "C1W", 1 } }));
    EXPECT_TRUE(b1->extra_phase.empty());
    EXPECT_EQ(asPairs(b1->companion), (Pairs{ { "C1W", 2 } }));
    // The designation written into every cell is the satellites' mode (C1C), counted
    // in stations; the votes behind it are counted in satellites; the datum phase is
    // named only at the two stations whose modelled code is C1C.
    EXPECT_EQ(asPairs(b1->datum_code), (Pairs{ { "C1C", 3 } }));
    EXPECT_EQ(asPairs(b1->datum_votes), (Pairs{ { "C1C", 2 }, { "C1W", 1 } }));
    EXPECT_EQ(asPairs(b1->datum_phase), (Pairs{ { "L1C", 2 } }));
    // The two civil stations exclude L1W (their latch chose L1C); the third excludes
    // C1C and L1C (its latch chose the W channel).  No allow list, so one reason.
    ASSERT_EQ(b1->excluded.size(), 1u);
    EXPECT_EQ(b1->excluded[0].first, "not_latched");
    EXPECT_EQ(asPairs(b1->excluded[0].second),
              (Pairs{ { "L1W", 2 }, { "C1C", 1 }, { "L1C", 1 } }));

    const RosterPlanBand* b2 = bandOf(plan, "GPS", '2');
    ASSERT_NE(b2, nullptr);
    EXPECT_EQ(asPairs(b2->phase), (Pairs{ { "L2W", 3 } }));
    EXPECT_EQ(asPairs(b2->datum_votes), (Pairs{ { "C2W", 3 } }));
    EXPECT_EQ(asPairs(b2->datum_phase), (Pairs{ { "L2W", 3 } }));
    ASSERT_EQ(b2->excluded.size(), 1u);
    EXPECT_EQ(asPairs(b2->excluded[0].second), (Pairs{ { "C2L", 3 } }));
}

TEST(SignalRosterPlanTest, ExclusionReasonsKeepTheRostersPrecedenceOrder) {
    // One station with an allow list that leaves L1W out, and a band-5 signal the
    // latch does not carry: all three reasons, listed allowlist, not_latched,
    // no_latch whatever their counts.
    const std::set<std::string> allow{ "C1C", "L1C", "C1W", "C2W", "L2W" };
    std::vector<StationRoster> st(1);
    st[0]["GPS"] = signalRosterBuildCells({ "C1C", "L1C", "C1W", "L1W", "C2W", "L2W", "L5Q" },
                                          latchOf("L1C", "C1C"), {}, false, &allow);
    const RosterPlan plan = signalRosterPlanOf(st, {});
    const RosterPlanBand* b1 = bandOf(plan, "GPS", '1');
    ASSERT_NE(b1, nullptr);
    ASSERT_EQ(b1->excluded.size(), 2u);
    EXPECT_EQ(b1->excluded[0].first, "allowlist");
    EXPECT_EQ(asPairs(b1->excluded[0].second), (Pairs{ { "L1W", 1 } }));
    EXPECT_EQ(b1->excluded[1].first, "not_latched");
    EXPECT_EQ(asPairs(b1->excluded[1].second), (Pairs{ { "C1W", 1 } }));
    const RosterPlanBand* b5 = bandOf(plan, "GPS", '5');
    ASSERT_NE(b5, nullptr);
    EXPECT_EQ(b5->stations_modelling, 0);
    ASSERT_EQ(b5->excluded.size(), 1u);
    EXPECT_EQ(b5->excluded[0].first, "allowlist") << "L5Q is outside the allow list too";
}

TEST(SignalRosterPlanTest, ARosterBuiltBeforeTheLatchHasNoModelledRole) {
    // The roster is built upstream of computeModels, so the first epoch of a
    // constellation has an empty latch: every signal is tracked, none modelled.
    std::vector<StationRoster> st(2);
    for (StationRoster& sr : st) sr["GPS"] = signalRosterBuildCells(kTracked, {}, {}, false, nullptr);
    const RosterPlan plan = signalRosterPlanOf(st, {});
    EXPECT_EQ(plan.stations, 2);
    EXPECT_FALSE(plan.hasModelledRole());
    EXPECT_FALSE(signalRosterPlanOf({}, {}).hasModelledRole());
}

// ---------------------------------------------------------------------------
// The table and the manifest record
// ---------------------------------------------------------------------------

TEST(SignalRosterPlanTest, TheTablePrintsEveryRoleAndEverySite) {
    const RosterPlan plan = signalRosterPlanOf(threeStations(), threeStationVotes());
    RosterSiteCounts epoch{}, run{};
    epoch[static_cast<std::size_t>(RosterSite::SlipTrackerBand)] = RosterSiteCount{ 90, 5 };
    run[static_cast<std::size_t>(RosterSite::SlipTrackerBand)]   = RosterSiteCount{ 180, 9 };
    run[static_cast<std::size_t>(RosterSite::CanonicalLatch)]    = RosterSiteCount{ 12, 0 };
    const std::vector<std::string> t = signalRosterPlanTable(plan, epoch, run, header());

    // First line: the scope, the epoch the band rows describe, and the run span.
    ASSERT_FALSE(t.empty());
    EXPECT_EQ(t[0].rfind("first emitted epoch with a modelled role: ", 0), 0u) << t[0];
    EXPECT_NE(t[0].find("week 2313 TOW 259230.0 (3 station(s) with a cell)"), std::string::npos);
    EXPECT_NE(t[0].find("run columns: 2 epoch(s), week 2313 TOW 259200.0 to week 2313 TOW "
                        "259230.0"),
              std::string::npos);

    const auto has = [&t](const std::string& line) {
        return std::find(t.begin(), t.end(), line) != t.end();
    };
    EXPECT_TRUE(has("system   band role          signals"));
    EXPECT_TRUE(has("GPS      1    stations      3 report the band, 3 model it"));
    EXPECT_TRUE(has("GPS      1    phase         L1C 2, L1W 1"));
    EXPECT_TRUE(has("GPS      1    code          C1C 2, C1W 1"));
    EXPECT_TRUE(has("GPS      1    second phase  -")) << "an empty role reads -";
    EXPECT_TRUE(has("GPS      1    companion     C1W 2"));
    EXPECT_TRUE(has("GPS      1    datum         code: C1C 3 | pinned by: C1C 2, C1W 1 | "
                    "phase: L1C 2"));
    EXPECT_TRUE(has("GPS      1    excluded      not_latched: L1W 2, C1C 1, L1C 1"));
    EXPECT_TRUE(has("GPS      2    excluded      not_latched: C2L 3"));

    // One row per site, in declared order, each with its four counts.
    const auto& specs = signalRosterSiteSpecs();
    std::size_t first_site = 0;
    for (std::size_t k = 0; k < t.size(); ++k)
        if (t[k].rfind("decision site ", 0) == 0) first_site = k + 1;
    ASSERT_GT(first_site, 0u);
    ASSERT_EQ(t.size(), first_site + kRosterSites + 1);
    for (std::size_t k = 0; k < kRosterSites; ++k)
        EXPECT_EQ(t[first_site + k].rfind(std::string(specs[k].name) + " ", 0), 0u)
            << t[first_site + k];
    const std::string& trk =
        t[first_site + static_cast<std::size_t>(RosterSite::SlipTrackerBand)];
    EXPECT_NE(trk.find("  90              5          180            9  signal | "),
              std::string::npos) << trk;
    // A site with no case compared over the run says that its 0 is not an agreement;
    // one with cases compared does not.
    const std::string& gate = t[first_site + static_cast<std::size_t>(RosterSite::CoverageGate)];
    EXPECT_NE(gate.find("so 0 is not an agreement"), std::string::npos) << gate;
    EXPECT_EQ(trk.find("not an agreement"), std::string::npos);
    const std::string& latch =
        t[first_site + static_cast<std::size_t>(RosterSite::CanonicalLatch)];
    EXPECT_EQ(latch.find("not an agreement"), std::string::npos);
    // The last line names what the table does not cover.
    EXPECT_EQ(t.back().rfind("not in this table: ", 0), 0u);
    for (const std::string& line : t)
        EXPECT_TRUE(line.empty() || line.back() != ' ') << "trailing blank in: " << line;
}

TEST(SignalRosterPlanTest, TheManifestRecordCarriesTheSameNumbers) {
    const RosterPlan plan = signalRosterPlanOf(threeStations(), threeStationVotes());
    RosterSiteCounts epoch{}, run{};
    epoch[static_cast<std::size_t>(RosterSite::IfPinPair)] = RosterSiteCount{ 6, 1 };
    run[static_cast<std::size_t>(RosterSite::IfPinPair)]   = RosterSiteCount{ 12, 2 };
    const std::string js = signalRosterPlanJson(plan, epoch, run, header(), "first_emitted_epoch");
    EXPECT_EQ(js.find('\n'), std::string::npos) << "one JSONL line";

    const nlohmann::json j = nlohmann::json::parse(js);
    EXPECT_EQ(j.at("record"), "signals_as_planned");
    EXPECT_EQ(j.at("scope"), "first_emitted_epoch");
    EXPECT_EQ(j.at("audited"), true);
    EXPECT_EQ(j.at("canonical_signal_rule"), "count");
    EXPECT_EQ(j.at("canonical_signal_priority_stations"), 0);
    EXPECT_EQ(j.at("week"), 2313);
    EXPECT_DOUBLE_EQ(j.at("tow").get<double>(), 259230.0);
    EXPECT_EQ(j.at("epochs"), 2);
    EXPECT_EQ(j.at("stations"), 3);
    ASSERT_EQ(j.at("bands").size(), 2u);
    const nlohmann::json& b1 = j.at("bands").at(0);
    EXPECT_EQ(b1.at("constellation"), "GPS");
    EXPECT_EQ(b1.at("band"), "1");
    EXPECT_EQ(b1.at("phase").at("L1C"), 2);
    EXPECT_EQ(b1.at("phase").at("L1W"), 1);
    EXPECT_EQ(b1.at("companion").at("C1W"), 2);
    EXPECT_TRUE(b1.at("second_phase").empty());
    EXPECT_EQ(b1.at("datum_code").at("C1C"), 3);
    EXPECT_EQ(b1.at("datum_pinned_by").at("C1W"), 1);
    EXPECT_EQ(b1.at("datum_phase").at("L1C"), 2);
    EXPECT_EQ(b1.at("excluded").at("not_latched").at("L1W"), 2);

    ASSERT_EQ(j.at("sites").size(), kRosterSites);
    const nlohmann::json& pin = j.at("sites").at(static_cast<std::size_t>(RosterSite::IfPinPair));
    EXPECT_EQ(pin.at("site"), "if_pin_pair");
    EXPECT_EQ(pin.at("group"), "signal");
    EXPECT_EQ(pin.at("epoch").at("compared"), 6);
    EXPECT_EQ(pin.at("epoch").at("disagree"), 1);
    EXPECT_EQ(pin.at("run").at("compared"), 12);
    EXPECT_EQ(pin.at("run").at("disagree"), 2);
}

TEST(SignalRosterPlanTest, TheLatchRuleRowNamesTheRulesOfThisRun) {
    const RosterPlan plan = signalRosterPlanOf(threeStations(), threeStationVotes());
    const RosterSiteCounts none{};
    const auto latchRow = [](const std::vector<std::string>& t) {
        for (const std::string& line : t)
            if (line.rfind("latch_rule ", 0) == 0) return line;
        return std::string();
    };

    // The global rule: the row says which one built the band rows.
    RosterPlanHeader h = header();
    h.latch_rule = "priority";
    std::string row = latchRow(signalRosterPlanTable(plan, none, none, h));
    ASSERT_FALSE(row.empty());
    EXPECT_NE(row.find("| this run: processing.canonical_signal_rule = priority, so every "
                       "station latched by the priority rule"),
              std::string::npos) << row;
    // No other row carries it.
    for (const std::string& line : signalRosterPlanTable(plan, none, none, h))
        if (line != row) EXPECT_EQ(line.find("this run:"), std::string::npos) << line;

    // A station list scopes the rule and overrides it (PppModel::canonicalRuleForStation).
    h.latch_rule = "count";
    h.latch_rule_priority_stations = 3;
    row = latchRow(signalRosterPlanTable(plan, none, none, h));
    EXPECT_NE(row.find("| this run: processing.canonical_signal_priority_stations lists 3 "
                       "station(s), which latch by the priority rule; every other station "
                       "latches by the count rule"),
              std::string::npos) << row;
    const nlohmann::json j =
        nlohmann::json::parse(signalRosterPlanJson(plan, none, none, h, "run"));
    EXPECT_EQ(j.at("canonical_signal_rule"), "count");
    EXPECT_EQ(j.at("canonical_signal_priority_stations"), 3);
}

TEST(SignalRosterPlanTest, ARunEndWithNoAuditedEpochSaysSo) {
    // Two builds, no audit: the plan is empty and the run columns still count.
    RosterSiteCounts run{};
    run[static_cast<std::size_t>(RosterSite::CanonicalLatch)] = RosterSiteCount{ 8, 0 };
    RosterPlanHeader h = header();
    h.scope = "run end";
    h.audited = false;
    h.week = 0;
    h.tow = 0.0;
    const RosterPlan empty;
    const std::vector<std::string> t = signalRosterPlanTable(empty, RosterSiteCounts{}, run, h);
    ASSERT_FALSE(t.empty());
    EXPECT_EQ(t[0].rfind("run end: no epoch reached the audit", 0), 0u) << t[0];
    EXPECT_NE(t[0].find("(2 epoch(s), week 2313 TOW 259200.0 to week 2313 TOW 259230.0)"),
              std::string::npos) << t[0];
    EXPECT_EQ(t[0].find("week 0 TOW 0.0"), std::string::npos) << t[0];
    for (const std::string& line : t)
        EXPECT_NE(line, "system   band role          signals") << "no band header row";
    std::string latch;
    for (const std::string& line : t)
        if (line.rfind("canonical_latch ", 0) == 0) latch = line;
    EXPECT_NE(latch.find("              -              -            8            0  signal | "),
              std::string::npos) << latch;

    const nlohmann::json j =
        nlohmann::json::parse(signalRosterPlanJson(empty, RosterSiteCounts{}, run, h, "run"));
    EXPECT_EQ(j.at("audited"), false);
    EXPECT_TRUE(j.at("week").is_null());
    EXPECT_TRUE(j.at("tow").is_null());
    EXPECT_EQ(j.at("epochs"), 2);
    EXPECT_TRUE(j.at("bands").empty());
    const nlohmann::json& site =
        j.at("sites").at(static_cast<std::size_t>(RosterSite::CanonicalLatch));
    EXPECT_TRUE(site.at("epoch").is_null());
    EXPECT_EQ(site.at("run").at("compared"), 8);

    // No epoch built at all: the run span says so rather than printing week 0.
    h.epochs = 0;
    const std::vector<std::string> t0 =
        signalRosterPlanTable(empty, RosterSiteCounts{}, RosterSiteCounts{}, h);
    EXPECT_NE(t0[0].find("(no epoch built)"), std::string::npos) << t0[0];
}

}  // namespace GnssProcessor
