/**
 * @file test_anchor_slip_handover.cpp
 * @brief Tests for the anchor-slip handover (network.cycle_slip.anchor_slip_handover,
 *        docs/SPEC_event_harness.md §12.6/§12.7): the staging seam, the
 *        directive precedence contract and the one-epoch lifetime.
 *
 * A slip response frees an arc.  When that arc realizes the gauge of a (sat, signal)
 * phase-bias family, the sticky election re-elects the same station and re-draws the
 * family's datum from an arc whose level is about to be rebuilt from one epoch of
 * code.  No error and no counter reports this at the slip site; the effect appears
 * later as an outlier cascade with no attribution back to the slip.  With the knob on,
 * the slip stages a handover directive that retires the incumbent at the same epoch's
 * election.
 *
 * Each case pins a decision that has no other observable:
 *
 *   1. a freed anchor arc stages exactly one directive per (sat, signal) family, and
 *      a freed non-anchor arc stages none; the card rests on this discrimination,
 *      because a slip at a station that anchors nothing does not touch any gauge;
 *   2. the same holds through the DD invariant-carry funnel, which carries the freed
 *      set on carry_invariant lineages where the leveling latch is empty, and a family
 *      named by both vehicles is staged once;
 *   3. `anchorFor` finds a production directive on an unarmed run: the injector's
 *      `armed` flag gates the harness schedule, not this card, and a lookup that
 *      required it would make the knob a no-op in every production posture;
 *   4. an injected directive takes precedence over a production one for the same
 *      family: a harness leg that scripts a `cold` handover tests the cold branch, and
 *      a production `continuous` directive shadowing it would turn the leg into a
 *      continuous one;
 *   5. both vectors clear at the same per-epoch point, on an unarmed run too; a
 *      directive that survived its epoch would re-ban the family at every later
 *      election (invariant #7's decoupling edge);
 *   6. the election consuming a staged directive moves the anchor and counts the
 *      branch it took, cold outcomes included (a cold outcome is a gauge
 *      re-realization, and the counters exist so that it is not hidden inside a
 *      continuous total);
 *   7. the knob defaults off and survives a round trip through the loader.
 *
 * The election is exercised directly: it reads only station names, the candidate
 * census and the handover ledger, never a StateManager (see
 * tests/test_eq34_reattach_hygiene.cpp for the same construction).
 */

#include <gtest/gtest.h>

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkAnchorHygiene.hpp"
#include "filter/NetworkInject.hpp"
#include "filter/NetworkPhaseDatum.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "filter/StateKeySchema.hpp"
#include "logging/LogManager.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

constexpr const char* kSigA = "L1C";
constexpr const char* kSigB = "L2W";

/// Station roster + this epoch's candidate census, exactly what the election reads.
struct Census {
    std::vector<NetStation>  stations;
    std::vector<bool>        present;
    PhaseDatumCandidates     cnd;
};

/// @param names     station names, in roster (index) order
/// @param sats      satellites every station tracks
/// @param signals   phase signals every station tracks on each satellite
/// @param coverage  per-station ranking key; the election walks `cand_set` in
///                  descending coverage, so the first eligible non-banned station
///                  in that order is the successor.
Census makeCensus(const std::vector<std::string>& names,
                  const std::vector<std::string>& sats,
                  const std::vector<std::string>& signals,
                  const std::vector<int>& coverage) {
    Census c;
    c.stations.resize(names.size());
    c.present.assign(names.size(), true);
    c.cnd.sat_el.resize(names.size());
    c.cnd.usable_keys.resize(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        c.stations[i].name = names[i];
        c.cnd.cover['G'][i] = coverage[i];
        c.cnd.cand_set['G'].insert(i);
        for (const auto& sat : sats) {
            c.cnd.sat_el[i][sat] = 45.0;
            for (const auto& sig : signals) {
                const std::string amb = StateKeys::makeAmbiguityKey(sat, sig);
                c.cnd.usable_keys[i][amb] = StateKeys::makeSatPhaseBiasKey(sat, sig);
                c.cnd.union_keys[amb]     = StateKeys::makeSatPhaseBiasKey(sat, sig);
            }
        }
    }
    return c;
}

/// amb key -> anchoring station name, for every (sat, signal) the census carries.
std::map<std::string, std::string> incumbency(const std::vector<std::string>& sats,
                                              const std::vector<std::string>& signals,
                                              const std::string& station) {
    std::map<std::string, std::string> m;
    for (const auto& sat : sats)
        for (const auto& sig : signals)
            m[StateKeys::makeAmbiguityKey(sat, sig)] = station;
    return m;
}

/// The set the drains hand the seam: (station index, "<sat>_<signal>").
std::set<std::pair<int, std::string>> freed(int sta,
                                            const std::string& sat,
                                            const std::vector<std::string>& signals) {
    std::set<std::pair<int, std::string>> s;
    for (const auto& sig : signals) s.insert({sta, StateKeys::makeAmbiguityKey(sat, sig)});
    return s;
}

/// The (sat, signal) families a staged directive set names, as sorted amb keys.
std::vector<std::string> families(const std::vector<InjectAnchorDirective>& d) {
    std::vector<std::string> out;
    for (const auto& x : d) out.push_back(x.sat + "_" + x.signal);
    std::sort(out.begin(), out.end());
    return out;
}

std::shared_ptr<spdlog::logger> nullLogger() {
    static auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    return std::make_shared<spdlog::logger>("anc_slip_test", sink);
}

/// A loaded GnssConfig, for the loader round trip.
struct Loaded {
    LogManager log;
    GnssConfig cfg;
    fs::path   path;
    Loaded(const std::string& tag, const std::string& network_extra) {
        const std::string body = std::string("{\n") +
            "  \"processing\": {\n"
            "    \"mode\": \"ppp-rtk-network\",\n"
            "    \"active_systems\": [ \"GPS\" ],\n"
            "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
            "  },\n"
            "  \"network\": {\n"
            "    \"clock_datum_station\": \"AAAA\",\n"
            "    \"phase_datum_prioritized\": true,\n"
            + network_extra +
            "    \"stations\": [\n"
            "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
            "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
            "    ]\n"
            "  }\n"
            "}\n";
        path = fs::temp_directory_path() / ("anc_slip_cfg_" + tag + ".json");
        std::ofstream f(path);
        f << body;
        f.close();
        EXPECT_TRUE(cfg.loadFromJson(path, log));
    }
    ~Loaded() { std::error_code ec; fs::remove(path, ec); }
};

}  // namespace

// ===========================================================================
// Cases 1–2: the staging seam, which freed arcs are gauge arcs
// ===========================================================================

TEST(AnchorSlipHandoverStage, FreedAnchorArcStagesOnePerFamily) {
    const std::vector<std::string> sats{"G10", "G27"};
    const std::vector<std::string> sigs{kSigA, kSigB};
    auto c = makeCensus({"S29", "S06"}, sats, sigs, {9, 7});
    // S29 anchors every family; the slip frees both of its G10 bands.
    const auto inc = incumbency(sats, sigs, "S29");

    NetworkInjectState inject;
    const int n = networkStageAnchorSlipHandovers(
            c.stations, inc, freed(0, "G10", sigs), {}, inject);

    EXPECT_EQ(n, 2);
    ASSERT_EQ(inject.anchor_now_slip.size(), 2u);
    EXPECT_EQ(families(inject.anchor_now_slip),
              (std::vector<std::string>{"G10_L1C", "G10_L2W"}));
    // The staged directive asks for the continuous branch; a directive that asked
    // for cold would re-realize the gauge, which the card exists to avoid.
    for (const auto& d : inject.anchor_now_slip) EXPECT_FALSE(d.cold);
    // The injector's own vector is untouched.
    EXPECT_TRUE(inject.anchor_now.empty());
    EXPECT_FALSE(inject.armed);
}

TEST(AnchorSlipHandoverStage, FreedNonAnchorArcStagesNothing) {
    const std::vector<std::string> sats{"G10"};
    const std::vector<std::string> sigs{kSigA, kSigB};
    auto c = makeCensus({"S29", "S06"}, sats, sigs, {9, 7});
    const auto inc = incumbency(sats, sigs, "S29");

    NetworkInjectState inject;
    // S06 (index 1) slips on the same satellite.  It anchors nothing, so the
    // family's gauge is untouched and the slip is an ordinary arc refloat.
    const int n = networkStageAnchorSlipHandovers(
            c.stations, inc, freed(1, "G10", sigs), {}, inject);

    EXPECT_EQ(n, 0);
    EXPECT_TRUE(inject.anchor_now_slip.empty());
}

TEST(AnchorSlipHandoverStage, DdInvariantFunnelIsTheOtherVehicleAndDoesNotDoubleStage) {
    const std::vector<std::string> sats{"G10"};
    const std::vector<std::string> sigs{kSigA, kSigB};
    auto c = makeCensus({"S29", "S06"}, sats, sigs, {9, 7});
    const auto inc = incumbency(sats, sigs, "S29");

    // carry_invariant lineage: the leveling free-epoch latch is structurally empty
    // and the DD funnel carries the same pairs.
    NetworkInjectState a;
    EXPECT_EQ(networkStageAnchorSlipHandovers(
                  c.stations, inc, {}, freed(0, "G10", sigs), a), 2);
    EXPECT_EQ(a.anchor_now_slip.size(), 2u);

    // Both vehicles naming the same family stage it once: `anchorFor` returns the
    // first match, so a duplicate would shadow the ledger readback.
    NetworkInjectState b;
    EXPECT_EQ(networkStageAnchorSlipHandovers(
                  c.stations, inc, freed(0, "G10", sigs), freed(0, "G10", sigs), b), 2);
    EXPECT_EQ(b.anchor_now_slip.size(), 2u);
    EXPECT_EQ(families(b.anchor_now_slip),
              (std::vector<std::string>{"G10_L1C", "G10_L2W"}));
}

TEST(AnchorSlipHandoverStage, EmptyInputsAndOutOfRangeIndicesAreInert) {
    const std::vector<std::string> sats{"G10"};
    const std::vector<std::string> sigs{kSigA};
    auto c = makeCensus({"S29"}, sats, sigs, {9});
    const auto inc = incumbency(sats, sigs, "S29");

    NetworkInjectState inject;
    EXPECT_EQ(networkStageAnchorSlipHandovers(c.stations, inc, {}, {}, inject), 0);
    EXPECT_EQ(networkStageAnchorSlipHandovers(c.stations, {}, freed(0, "G10", sigs),
                                              {}, inject), 0);
    // A station index past the roster (a pruned station, a stale funnel entry) must
    // not index out of bounds.
    EXPECT_EQ(networkStageAnchorSlipHandovers(c.stations, inc, freed(7, "G10", sigs),
                                              {}, inject), 0);
    EXPECT_TRUE(inject.anchor_now_slip.empty());
}

// ===========================================================================
// Cases 3–4: the lookup contract
// ===========================================================================

TEST(AnchorSlipHandoverLookup, ProductionDirectiveIsFoundOnAnUnarmedRun) {
    NetworkInjectState inject;
    ASSERT_FALSE(inject.armed);      // no harness schedule anywhere in this run
    InjectAnchorDirective d;
    d.sat = "G10";
    d.signal = kSigA;
    inject.anchor_now_slip.push_back(d);

    const auto* found = inject.anchorFor("G10_L1C");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->sat, "G10");
    EXPECT_FALSE(found->cold);
    // Scope is exactly the one family: neither another band nor another satellite.
    EXPECT_EQ(inject.anchorFor("G10_L2W"), nullptr);
    EXPECT_EQ(inject.anchorFor("G27_L1C"), nullptr);
}

TEST(AnchorSlipHandoverLookup, InjectedDirectiveWinsForTheSameFamily) {
    NetworkInjectState inject;
    inject.armed = true;
    InjectAnchorDirective scripted;      // the harness leg: force the cold branch
    scripted.sat = "G10";
    scripted.signal = "all";
    scripted.cold = true;
    inject.anchor_now.push_back(scripted);

    InjectAnchorDirective production;    // the card, for the same family
    production.sat = "G10";
    production.signal = kSigA;
    production.cold = false;
    inject.anchor_now_slip.push_back(production);

    const auto* found = inject.anchorFor("G10_L1C");
    ASSERT_NE(found, nullptr);
    EXPECT_TRUE(found->cold) << "the scripted directive must not be shadowed by the "
                                "production one — a leg testing the cold branch would "
                                "silently become a continuous leg";
    EXPECT_EQ(found, &inject.anchor_now.front());
    // A family only the card staged still resolves to the card's directive.
    InjectAnchorDirective other;
    other.sat = "G27";
    other.signal = kSigB;
    inject.anchor_now_slip.push_back(other);
    const auto* f2 = inject.anchorFor("G27_L2W");
    ASSERT_NE(f2, nullptr);
    EXPECT_FALSE(f2->cold);
}

// ===========================================================================
// Case 5: the one-epoch lifetime
// ===========================================================================

TEST(AnchorSlipHandoverLifetime, BothVectorsClearPerEpochIncludingUnarmedRuns) {
    Loaded L("life", "");
    NetworkRunContext ctx(L.cfg, L.cfg.getNetworkConfig());
    const fs::path dir = fs::temp_directory_path() / "anc_slip_traces";
    fs::create_directories(dir);
    NetworkRunTraces traces(dir);

    // An unarmed run: `networkInjectEpoch` returns early, and the production card
    // is the only writer.  The clear still happens there; without it the ban would
    // re-fire at every later election (invariant #7's decoupling edge).
    ASSERT_FALSE(ctx.inject.armed);
    InjectAnchorDirective d;
    d.sat = "G10";
    d.signal = kSigA;
    ctx.inject.anchor_now_slip.push_back(d);
    ctx.inject.anchor_now.push_back(d);

    SyncedEpoch se_raw, scratch;
    se_raw.time.week = 2312;
    se_raw.time.tow  = 279480.0;
    const SyncedEpoch& out = networkInjectEpoch(ctx, nullLogger(), traces, se_raw, scratch);

    EXPECT_EQ(&out, &se_raw) << "the unarmed path must return the raw epoch itself";
    EXPECT_TRUE(ctx.inject.anchor_now_slip.empty());
    EXPECT_TRUE(ctx.inject.anchor_now.empty());
    EXPECT_TRUE(ctx.inject.arc_erase_now.empty());
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ===========================================================================
// Case 6: the election consumes the directive and counts the branch it took
// ===========================================================================

TEST(AnchorSlipHandoverElection, StagedBanMovesTheAnchorAndTakesTheContinuousBranch) {
    const std::vector<std::string> sats{"G10"};
    const std::vector<std::string> sigs{kSigA};
    auto c = makeCensus({"S29", "S06"}, sats, sigs, {9, 7});
    auto inc = incumbency(sats, sigs, "S29");
    const std::string amb = StateKeys::makeAmbiguityKey("G10", kSigA);

    // The successor has a converged carried value_abs, so the continuous branch is
    // available: it is pinned in the outgoing frame and the family does not step.
    AnchorHandoverLedger ledger;
    ledger.value_abs[{1, amb}] = {12.5 /*value*/, 1e-6 /*var*/, 1e-3 /*var_first*/};

    NetworkInjectState inject;
    ASSERT_EQ(networkStageAnchorSlipHandovers(
                  c.stations, inc, freed(0, "G10", sigs), {}, inject), 1);

    AnchorHygiene hyg;
    NetworkAnchorTallies tally;
    std::map<std::string, double> pin_target;
    std::map<std::string, std::string> this_epoch_anchor;
    std::set<std::string> shared;
    std::vector<AnchorHandoverEvent> events;
    networkElectPhaseAnchors(/*sticky=*/true, c.stations, c.present, c.cnd, hyg, tally,
                             inc, pin_target, this_epoch_anchor, shared,
                             &ledger, &events, /*max_var=*/1e-2, nullptr,
                             /*max_var_ratio=*/0.0, &inject);

    EXPECT_EQ(this_epoch_anchor.at(amb), "S06") << "the ban must retire the slipped "
                                                   "incumbent within the same epoch";
    EXPECT_DOUBLE_EQ(c.stations[1].pinned_amb_targets.at(amb), 12.5);
    EXPECT_EQ(tally.handover_continuous, 1);
    EXPECT_EQ(tally.handover_cold, 0);

    const auto& d = inject.anchor_now_slip.front();
    EXPECT_EQ(d.n_families, 1);
    EXPECT_EQ(d.n_cold_fallback, 0);
    EXPECT_EQ(d.n_no_successor, 0);
    ASSERT_EQ(d.retired.size(), 1u);
    EXPECT_EQ(d.retired.front(), "S29");
    ASSERT_EQ(d.elected.size(), 1u);
    EXPECT_EQ(d.elected.front(), "S06");
}

TEST(AnchorSlipHandoverElection, ColdOutcomeIsCountedNotHidden) {
    const std::vector<std::string> sats{"G10"};
    const std::vector<std::string> sigs{kSigA};
    auto c = makeCensus({"S29", "S06"}, sats, sigs, {9, 7});
    auto inc = incumbency(sats, sigs, "S29");
    const std::string amb = StateKeys::makeAmbiguityKey("G10", kSigA);

    // The successor's carried value is too poorly determined to be a datum, so the
    // election falls back to the cold pin.  The handover still happens, but the
    // gauge re-realizes, and the directive's counters report that as a cold
    // fallback rather than as a continuous handover.
    AnchorHandoverLedger ledger;
    ledger.value_abs[{1, amb}] = {12.5, 1.0 /*var >> max_var*/, 1e-3};

    NetworkInjectState inject;
    ASSERT_EQ(networkStageAnchorSlipHandovers(
                  c.stations, inc, freed(0, "G10", sigs), {}, inject), 1);

    AnchorHygiene hyg;
    NetworkAnchorTallies tally;
    std::map<std::string, double> pin_target;
    std::map<std::string, std::string> this_epoch_anchor;
    std::set<std::string> shared;
    networkElectPhaseAnchors(true, c.stations, c.present, c.cnd, hyg, tally,
                             inc, pin_target, this_epoch_anchor, shared,
                             &ledger, nullptr, /*max_var=*/1e-2, nullptr, 0.0, &inject);

    EXPECT_EQ(this_epoch_anchor.at(amb), "S06");
    EXPECT_DOUBLE_EQ(c.stations[1].pinned_amb_targets.at(amb), 0.0);
    EXPECT_EQ(tally.handover_cold, 1);
    EXPECT_EQ(tally.handover_continuous, 0);

    const auto& d = inject.anchor_now_slip.front();
    EXPECT_EQ(d.n_families, 1);
    EXPECT_EQ(d.n_cold_fallback, 1) << "a cold OUTCOME must never fold into the "
                                       "continuous total";
    EXPECT_EQ(d.n_forced_cold, 0) << "nobody ASKED for cold here — that field is the "
                                     "harness's request, not the outcome";
}

TEST(AnchorSlipHandoverElection, NoSuccessorLeavesTheFamilyUnpinnedAndSaysSo) {
    const std::vector<std::string> sats{"G10"};
    const std::vector<std::string> sigs{kSigA};
    // One station tracks the family: the ban leaves no candidate at all.
    auto c = makeCensus({"S29"}, sats, sigs, {9});
    auto inc = incumbency(sats, sigs, "S29");
    const std::string amb = StateKeys::makeAmbiguityKey("G10", kSigA);

    NetworkInjectState inject;
    ASSERT_EQ(networkStageAnchorSlipHandovers(
                  c.stations, inc, freed(0, "G10", sigs), {}, inject), 1);

    AnchorHygiene hyg;
    NetworkAnchorTallies tally;
    std::map<std::string, double> pin_target;
    std::map<std::string, std::string> this_epoch_anchor;
    std::set<std::string> shared;
    networkElectPhaseAnchors(true, c.stations, c.present, c.cnd, hyg, tally,
                             inc, pin_target, this_epoch_anchor, shared,
                             nullptr, nullptr, 0.0, nullptr, 0.0, &inject);

    EXPECT_EQ(this_epoch_anchor.count(amb), 0u);
    EXPECT_TRUE(c.stations[0].pinned_amb_targets.empty());
    const auto& d = inject.anchor_now_slip.front();
    EXPECT_EQ(d.n_no_successor, 1);
    EXPECT_EQ(d.n_families, 0);
}

TEST(AnchorSlipHandoverElection, NullDirectiveSetLeavesTheStickyIncumbentInPlace) {
    // The knob-off control for the cases above: identical inputs, no directive.
    const std::vector<std::string> sats{"G10"};
    const std::vector<std::string> sigs{kSigA};
    auto c = makeCensus({"S29", "S06"}, sats, sigs, {9, 7});
    auto inc = incumbency(sats, sigs, "S29");
    const std::string amb = StateKeys::makeAmbiguityKey("G10", kSigA);

    AnchorHandoverLedger ledger;
    ledger.value_abs[{1, amb}] = {12.5, 1e-6, 1e-3};
    AnchorHygiene hyg;
    NetworkAnchorTallies tally;
    std::map<std::string, double> pin_target;
    std::map<std::string, std::string> this_epoch_anchor;
    std::set<std::string> shared;
    networkElectPhaseAnchors(true, c.stations, c.present, c.cnd, hyg, tally,
                             inc, pin_target, this_epoch_anchor, shared,
                             &ledger, nullptr, 1e-2, nullptr, 0.0, nullptr);

    EXPECT_EQ(this_epoch_anchor.at(amb), "S29");
    EXPECT_DOUBLE_EQ(c.stations[0].pinned_amb_targets.at(amb), 0.0);
    EXPECT_EQ(tally.handover_continuous, 0);
    EXPECT_EQ(tally.handover_cold, 0);
}

// ===========================================================================
// Case 7: the loader
// ===========================================================================

TEST(AnchorSlipHandoverConfig, DefaultsOffAndArmsFromJson) {
    {
        Loaded off("off", "");
        EXPECT_FALSE(off.cfg.networkCycleSlipAnchorSlipHandover());
        EXPECT_FALSE(off.cfg.getNetworkConfig().cycle_slip_anchor_slip_handover);
    }
    {
        Loaded on("on", "    \"cycle_slip\": { \"anchor_slip_handover\": true },\n");
        EXPECT_TRUE(on.cfg.networkCycleSlipAnchorSlipHandover());
        EXPECT_TRUE(on.cfg.getNetworkConfig().cycle_slip_anchor_slip_handover);
    }
}
