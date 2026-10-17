/**
 * @file test_candidacy_usable.cpp
 * @brief Cold anchor fallback (network.phase_datum_candidacy_usable,
 *        docs/SPEC_event_harness.md §15): the candidacy screen, its skip reasons,
 *        and the byte-identity with the knob off of the election it sits in front of.
 *
 * The defect is one of ordering, and it leaves no trace at the site where it
 * happens.  `networkElectPhaseAnchors` sweeps the coverage-ranked candidates and
 * takes the first that tracks the key; only afterwards does the continuous-handover
 * branch look that successor up in the one-epoch ledger, and it may refuse the
 * carried value as a datum.  The family is then pinned at 0, its gauge
 * re-realizes, it is marked for re-reference and its leveling carries are erased
 * at every station.  Nothing errors.  A lower-ranked station holding a mature,
 * converged arc for the same key sits unused, and this is visible only by
 * comparing the elected station with the ones passed over (the storm-arm census
 * in §15 found mature alternatives passed over in most cold handovers).
 *
 * The cases pin the decisions that have no other observable:
 *
 *   1. the knob defaults off, parses, carries a 60-epoch maturity default, and
 *      the loader refuses a negative maturity bar;
 *   2. with the knob off the election is unchanged (same successor, same branch,
 *      same tallies) even with a populated ledger that would let the pass move it.
 *      This is the invariant #6 byte-identity, asserted as an equality against a
 *      call without the view on the identical fixture, counters included;
 *   3. with it on, a handover whose top-ranked candidate has no ledger entry lands
 *      on the lower-ranked candidate that has a usable one, takes the continuous
 *      branch at that candidate's own carried value, and reports it
 *      (`candidacy_rank_changed` = 1, `handover_cold` = 0);
 *   4. each refusal reason is separately reachable and separately counted (no
 *      entry, variance or ratio refused, freed this epoch, younger than the
 *      maturity bar), because "the pass fell back" and "the pass fell back because
 *      the bar is too tight" are different findings;
 *   5. with no usable candidate the election is the unscreened one, and the cold
 *      outcome is counted as a fallback;
 *   6. the pass is not entered when the sticky incumbent is kept, nor for a key
 *      without an incumbent (first sighting): in both cases there is no outgoing
 *      frame to preserve, and a screen could only churn the election.
 *
 * The election is exercised directly: it reads only station names, the candidate
 * census, the handover ledger and the carry view (same construction as
 * tests/test_anchor_slip_handover.cpp and tests/test_eq34_reattach_hygiene.cpp).
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "network/solve/AnchorHygiene.hpp"
#include "network/carry/Inject.hpp"
#include "network/solve/PhaseDatum.hpp"
#include "network/driver/RunTypes.hpp"
#include "filter/StateKeySchema.hpp"
#include "logging/LogManager.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

constexpr const char* kSig = "L1C";
constexpr const char* kSat = "G10";
constexpr double kMaxVar = 1e-2;   // the golden's phase_datum_handover_max_var_m2

std::string ambKey() { return StateKeys::makeAmbiguityKey(kSat, kSig); }

/// Station roster + this epoch's candidate census, exactly what the election reads.
struct Roster {
    std::vector<NetStation> stations;
    std::vector<bool>       present;
    PhaseDatumCandidates    cnd;
};

/// Stations all tracking G10/L1C, ranked by `coverage` (the tests use four, A > B >
/// C > D), so the election's sweep walks them in that order.
Roster makeRoster(const std::vector<std::string>& names,
                  const std::vector<int>& coverage) {
    Roster r;
    r.stations.resize(names.size());
    r.present.assign(names.size(), true);
    r.cnd.sat_el.resize(names.size());
    r.cnd.usable_keys.resize(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        r.stations[i].name = names[i];
        r.cnd.cover['G'][i] = coverage[i];
        r.cnd.cand_set['G'].insert(i);
        r.cnd.sat_el[i][kSat] = 45.0;
        r.cnd.usable_keys[i][ambKey()] = StateKeys::makeSatPhaseBiasKey(kSat, kSig);
        r.cnd.union_keys[ambKey()]     = StateKeys::makeSatPhaseBiasKey(kSat, kSig);
    }
    return r;
}

/// The outcome of one election call, as the assertions need it.
struct Outcome {
    std::string           elected;      ///< this_epoch_anchor for the family ("-" = none)
    double                target = 0.0; ///< the pin the elected station got
    bool                  pinned = false;
    NetworkAnchorTallies  tally;
    std::vector<AnchorHandoverEvent> events;
};

/// One full election over the fixture.  `usable` is the candidacy view (a
/// default-constructed view gives the unscreened election); `ban_incumbent` stages
/// the one-epoch candidacy ban that makes the handover happen at all.
Outcome elect(Roster& r,
              std::map<std::string, std::string> incumbent,
              const AnchorHandoverLedger& ledger,
              const AnchorCandidacyUsable& usable,
              bool ban_incumbent,
              double max_var_ratio = 0.0) {
    NetworkInjectState inject;
    if (ban_incumbent) {
        InjectAnchorDirective d;
        d.sat    = kSat;
        d.signal = kSig;
        d.cold   = false;      // continuous: production rules pick the successor
        inject.anchor_now_slip.push_back(std::move(d));
    }
    AnchorHygiene hyg;
    Outcome o;
    std::map<std::string, double>      pin_target;
    std::map<std::string, std::string> this_epoch_anchor;
    std::set<std::string>              shared;
    networkElectPhaseAnchors(/*sticky=*/true, r.stations, r.present, r.cnd, hyg,
                             o.tally, incumbent, pin_target, this_epoch_anchor,
                             shared, &ledger, &o.events, kMaxVar, nullptr,
                             max_var_ratio,
                             ban_incumbent ? &inject : nullptr, &usable);
    const auto it = this_epoch_anchor.find(ambKey());
    o.elected = it == this_epoch_anchor.end() ? "-" : it->second;
    for (const auto& st : r.stations) {
        const auto pt = st.pinned_amb_targets.find(ambKey());
        if (pt != st.pinned_amb_targets.end() && st.name == o.elected) {
            o.target = pt->second;
            o.pinned = true;
        }
    }
    return o;
}

/// amb -> anchoring station, for the single family this fixture carries.
std::map<std::string, std::string> incumbency(const std::string& sta) {
    return {{ambKey(), sta}};
}

/// The candidacy view, armed.  `satellite_clause` defaults to false in this helper
/// (the loader default is on), so the iteration-1 cases below run the per-family
/// screen; the iteration-2 cases arm it explicitly, which is also the single-knob A/B.
AnchorCandidacyUsable armed(
        int min_age_ep,
        long long now_ep = 0,
        const std::set<std::pair<int, std::string>>* freed = nullptr,
        const net::ArcTable* lc =
            nullptr,
        bool satellite_clause = false) {
    AnchorCandidacyUsable u;
    u.on           = true;
    u.satellite_clause = satellite_clause;
    u.min_age_ep   = min_age_ep;
    u.now_ep       = now_ep;
    u.free_ambs    = freed;
    u.local_carry  = lc;
    return u;
}

// ---------------------------------------------------------------------------
// Iteration 2: the satellite-clause fixture (three bands of one satellite)
// ---------------------------------------------------------------------------

const std::vector<std::string> kSigs3{"L1C", "L2W", "L5Q"};

std::string amb3(const std::string& sig) {
    return StateKeys::makeAmbiguityKey(kSat, sig);
}

/// Four stations ranked A > B > C > D by coverage.  `tracks[i]` names the bands
/// station i tracks (its `usable_keys`), so a station can be a candidate for two
/// of the satellite's bands and not the third.
Roster makeRoster3(const std::vector<std::string>& names,
                   const std::vector<int>& coverage,
                   const std::vector<std::vector<std::string>>& tracks) {
    Roster r;
    r.stations.resize(names.size());
    r.present.assign(names.size(), true);
    r.cnd.sat_el.resize(names.size());
    r.cnd.usable_keys.resize(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        r.stations[i].name = names[i];
        r.cnd.cover['G'][i] = coverage[i];
        r.cnd.cand_set['G'].insert(i);
        r.cnd.sat_el[i][kSat] = 45.0;
        for (const auto& sig : tracks[i])
            r.cnd.usable_keys[i][amb3(sig)] = StateKeys::makeSatPhaseBiasKey(kSat, sig);
    }
    for (const auto& sig : kSigs3)
        r.cnd.union_keys[amb3(sig)] = StateKeys::makeSatPhaseBiasKey(kSat, sig);
    return r;
}

struct Outcome3 {
    std::map<std::string, std::string> anchor;   ///< amb -> station ("" = unpinned)
    std::map<std::string, double>      target;
    std::map<std::string, bool>        continuous;
    NetworkAnchorTallies               tally;
    /// Rule (c): distinct anchor stations across the satellite's families.
    std::size_t distinctStations() const {
        std::set<std::string> s;
        for (const auto& kv : anchor) s.insert(kv.second);
        return s.size();
    }
};

/// One election over the three-band fixture.  `ban_signals` names the signals whose
/// incumbent is retired this epoch ("all" retires every band).
Outcome3 elect3(Roster& r,
                std::map<std::string, std::string> incumbent,
                const AnchorHandoverLedger& ledger,
                const AnchorCandidacyUsable& usable,
                const std::vector<std::string>& ban_signals) {
    NetworkInjectState inject;
    for (const auto& sig : ban_signals) {
        InjectAnchorDirective d;
        d.sat    = kSat;
        d.signal = sig;
        d.cold   = false;
        inject.anchor_now_slip.push_back(std::move(d));
    }
    AnchorHygiene hyg;
    Outcome3 o;
    std::map<std::string, double>      pin_target;
    std::map<std::string, std::string> this_epoch_anchor;
    std::set<std::string>              shared;
    std::vector<AnchorHandoverEvent>   events;
    networkElectPhaseAnchors(/*sticky=*/true, r.stations, r.present, r.cnd, hyg,
                             o.tally, incumbent, pin_target, this_epoch_anchor,
                             shared, &ledger, &events, kMaxVar, nullptr, 0.0,
                             ban_signals.empty() ? nullptr : &inject, &usable);
    o.anchor = this_epoch_anchor;
    for (const auto& st : r.stations)
        for (const auto& kv : st.pinned_amb_targets)
            o.target[kv.first] = kv.second;
    for (const auto& e : events) o.continuous[e.amb] = e.continuous;
    return o;
}

/// A loaded GnssConfig, for the loader round trip.  `ok_out` receives the parse
/// result so the negative case can assert a refusal instead of expecting success.
struct Loaded {
    LogManager log;
    GnssConfig cfg;
    fs::path   path;
    bool       ok = false;
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
        path = gnss_test::tempDir() / ("candidacy_cfg_" + tag + ".json");
        std::ofstream f(path);
        f << body;
        f.close();
        ok = cfg.loadFromJson(path, log);
    }
    ~Loaded() { std::error_code ec; fs::remove(path, ec); }
};

}  // namespace

// ===========================================================================
// 1: the config contract
// ===========================================================================

TEST(CandidacyUsableConfig, DefaultsOffWithASixtyEpochMaturityBar) {
    Loaded l("default", "");
    ASSERT_TRUE(l.ok);
    EXPECT_FALSE(l.cfg.networkPhaseDatumCandidacyUsable())
        << "new work is default-OFF (standing invariant #6)";
    EXPECT_EQ(l.cfg.networkPhaseDatumCandidacyMinAgeEp(), 60)
        << "the §15 census separated the cold (p50 39 ep) and continuous (p50 236 ep) "
           "successor populations at 60 epochs";
}

TEST(CandidacyUsableConfig, ParsesBothKnobs) {
    Loaded l("on", "    \"phase_datum_candidacy_usable\": true,\n"
                   "    \"phase_datum_candidacy_min_age_ep\": 20,\n");
    ASSERT_TRUE(l.ok);
    EXPECT_TRUE(l.cfg.networkPhaseDatumCandidacyUsable());
    EXPECT_EQ(l.cfg.networkPhaseDatumCandidacyMinAgeEp(), 20);
}

TEST(CandidacyUsableConfig, ZeroMaturityBarIsLegalAndMeansNoAgeTest) {
    Loaded l("zero", "    \"phase_datum_candidacy_usable\": true,\n"
                     "    \"phase_datum_candidacy_min_age_ep\": 0,\n");
    ASSERT_TRUE(l.ok);
    EXPECT_EQ(l.cfg.networkPhaseDatumCandidacyMinAgeEp(), 0);
}

TEST(CandidacyUsableConfig, NegativeMaturityBarIsRefused) {
    Loaded l("neg", "    \"phase_datum_candidacy_usable\": true,\n"
                    "    \"phase_datum_candidacy_min_age_ep\": -1,\n");
    EXPECT_FALSE(l.ok) << "a negative bar has no meaning; 0 is the 'no age test' "
                          "spelling and the loader must not silently accept -1";
}

// ===========================================================================
// 2: with the knob off the election is unchanged, ledger and all (invariant #6)
// ===========================================================================

TEST(CandidacyUsableElection, KnobOffElectsTheTopRankedCandidateAndGoesCold) {
    // A anchors and the ban retires it; B outranks C but has no ledger entry, while
    // C has a converged one.  This is the defect, stated as a test.
    auto r = makeRoster({"AAAA", "BBBB", "CCCC", "DDDD"}, {9, 8, 7, 6});
    AnchorHandoverLedger ledger;
    ledger.value_abs[{2, ambKey()}] = {12.5, 1e-6, 1e-3};   // CCCC: usable

    AnchorCandidacyUsable off;   // knob off: the pass is not entered
    const Outcome o = elect(r, incumbency("AAAA"), ledger, off, /*ban=*/true);

    EXPECT_EQ(o.elected, "BBBB");
    EXPECT_TRUE(o.pinned);
    EXPECT_DOUBLE_EQ(o.target, 0.0) << "no ledger entry for BBBB ⇒ the COLD pin";
    EXPECT_EQ(o.tally.handover_cold, 1);
    EXPECT_EQ(o.tally.handover_continuous, 0);
    EXPECT_EQ(o.tally.candidacy_usable_elected, 0);
    EXPECT_EQ(o.tally.candidacy_skipped_unusable, 0);
    EXPECT_EQ(o.tally.candidacy_no_usable_fallback, 0);
    EXPECT_EQ(o.tally.candidacy_rank_changed, 0);
    ASSERT_EQ(o.events.size(), 1u);
    EXPECT_EQ(o.events.front().cand_skipped, 0);
    EXPECT_TRUE(o.events.front().cand_first_unusable.empty());
}

TEST(CandidacyUsableElection, KnobOffIsByteIdenticalToTheUnarmedCall) {
    // The invariant #6 assertion: the same fixture, once through the
    // default-constructed view (the code path compiled but not entered) and once
    // through a nullptr view (the call without the screen), must agree on the
    // elected station, the pin, the events and the tally fields compared below.
    auto r1 = makeRoster({"AAAA", "BBBB", "CCCC", "DDDD"}, {9, 8, 7, 6});
    auto r2 = makeRoster({"AAAA", "BBBB", "CCCC", "DDDD"}, {9, 8, 7, 6});
    AnchorHandoverLedger ledger;
    ledger.value_abs[{2, ambKey()}] = {12.5, 1e-6, 1e-3};
    ledger.value_abs[{3, ambKey()}] = {-3.25, 1e-7, 1e-4};

    AnchorCandidacyUsable off;
    const Outcome a = elect(r1, incumbency("AAAA"), ledger, off, /*ban=*/true);

    // The nullptr-view call, spelled out (elect() always passes a view).
    NetworkInjectState inject;
    InjectAnchorDirective d;
    d.sat = kSat; d.signal = kSig; d.cold = false;
    inject.anchor_now_slip.push_back(std::move(d));
    AnchorHygiene hyg;
    NetworkAnchorTallies tally;
    std::map<std::string, std::string> inc = incumbency("AAAA");
    std::map<std::string, double> pin_target;
    std::map<std::string, std::string> this_epoch_anchor;
    std::set<std::string> shared;
    std::vector<AnchorHandoverEvent> events;
    networkElectPhaseAnchors(true, r2.stations, r2.present, r2.cnd, hyg, tally, inc,
                             pin_target, this_epoch_anchor, shared, &ledger, &events,
                             kMaxVar, nullptr, 0.0, &inject, nullptr);

    EXPECT_EQ(a.elected, this_epoch_anchor.at(ambKey()));
    EXPECT_DOUBLE_EQ(a.target, r2.stations[1].pinned_amb_targets.at(ambKey()));
    EXPECT_EQ(a.tally.handover_cold, tally.handover_cold);
    EXPECT_EQ(a.tally.handover_continuous, tally.handover_continuous);
    EXPECT_EQ(a.tally.reattaches, tally.reattaches);
    EXPECT_EQ(a.tally.cold_fallback_pins, tally.cold_fallback_pins);
    EXPECT_EQ(a.tally.handover_refused_var, tally.handover_refused_var);
    EXPECT_EQ(a.tally.handover_refused_ratio, tally.handover_refused_ratio);
    ASSERT_EQ(a.events.size(), events.size());
    EXPECT_EQ(a.events.front().to, events.front().to);
    EXPECT_EQ(a.events.front().continuous, events.front().continuous);
}

// ===========================================================================
// 3: knob on: the usable candidate is elected, and the branch is continuous
// ===========================================================================

TEST(CandidacyUsableElection, ElectsTheLowerRankedCandidateWhoseCarryIsUsable) {
    auto r = makeRoster({"AAAA", "BBBB", "CCCC", "DDDD"}, {9, 8, 7, 6});
    AnchorHandoverLedger ledger;
    ledger.value_abs[{2, ambKey()}] = {12.5, 1e-6, 1e-3};   // CCCC alone

    const Outcome o = elect(r, incumbency("AAAA"), ledger, armed(/*min_age=*/0),
                            /*ban=*/true);

    EXPECT_EQ(o.elected, "CCCC") << "BBBB outranks CCCC but has no usable carry";
    EXPECT_DOUBLE_EQ(o.target, 12.5) << "pinned at the successor's OWN value in the "
                                        "outgoing frame — the S-transform";
    EXPECT_EQ(o.tally.handover_continuous, 1);
    EXPECT_EQ(o.tally.handover_cold, 0);
    EXPECT_EQ(o.tally.candidacy_usable_elected, 1);
    EXPECT_EQ(o.tally.candidacy_rank_changed, 1) << "the elected station is NOT the "
                                                    "one today's sweep would take";
    EXPECT_EQ(o.tally.candidacy_no_usable_fallback, 0);
    EXPECT_EQ(o.tally.candidacy_skipped_unusable, 1);
    EXPECT_EQ(o.tally.candidacy_skip_no_entry, 1);
    ASSERT_EQ(o.events.size(), 1u);
    EXPECT_EQ(o.events.front().cand_skipped, 1);
    EXPECT_EQ(o.events.front().cand_first_unusable, "BBBB");
}

TEST(CandidacyUsableElection, AgreeingWithTodaysPickIsCountedButNotAsAChange) {
    // The pass ran, found the top-ranked candidate usable, and elected it: the
    // same station the unscreened sweep takes.  `candidacy_rank_changed` stays 0,
    // because an arm whose elected station always matches the unscreened one
    // changed nothing, whatever `candidacy_usable_elected` counts.
    auto r = makeRoster({"AAAA", "BBBB", "CCCC", "DDDD"}, {9, 8, 7, 6});
    AnchorHandoverLedger ledger;
    ledger.value_abs[{1, ambKey()}] = {7.0, 1e-6, 1e-3};    // BBBB: usable

    const Outcome o = elect(r, incumbency("AAAA"), ledger, armed(0), true);

    EXPECT_EQ(o.elected, "BBBB");
    EXPECT_DOUBLE_EQ(o.target, 7.0);
    EXPECT_EQ(o.tally.candidacy_usable_elected, 1);
    EXPECT_EQ(o.tally.candidacy_rank_changed, 0);
    EXPECT_EQ(o.tally.candidacy_skipped_unusable, 0);
    EXPECT_EQ(o.tally.handover_continuous, 1);
}

// ===========================================================================
// 4: the four refusal reasons, each separately reachable and counted
// ===========================================================================

TEST(CandidacyUsableElection, VarianceCeilingRefusesTheTopCandidate) {
    auto r = makeRoster({"AAAA", "BBBB", "CCCC", "DDDD"}, {9, 8, 7, 6});
    AnchorHandoverLedger ledger;
    ledger.value_abs[{1, ambKey()}] = {7.0, 1.0, 1e-3};     // BBBB: var >> ceiling
    ledger.value_abs[{2, ambKey()}] = {12.5, 1e-6, 1e-3};   // CCCC: usable

    const Outcome o = elect(r, incumbency("AAAA"), ledger, armed(0), true);

    EXPECT_EQ(o.elected, "CCCC");
    EXPECT_DOUBLE_EQ(o.target, 12.5);
    EXPECT_EQ(o.tally.candidacy_skip_var_refused, 1);
    EXPECT_EQ(o.tally.candidacy_skip_no_entry, 0);
    EXPECT_EQ(o.tally.candidacy_rank_changed, 1);
    EXPECT_EQ(o.tally.handover_cold, 0);
}

TEST(CandidacyUsableElection, RatioCeilingRefusesTheTopCandidateWhenArmed) {
    // Both variances pass the absolute ceiling, but BBBB has not converged against
    // its own first stash.  The screen applies the ratio ceiling as the continuous
    // branch does; a screen that ignored it would elect a station the branch then
    // refuses, which reproduces the defect.
    auto r = makeRoster({"AAAA", "BBBB", "CCCC", "DDDD"}, {9, 8, 7, 6});
    AnchorHandoverLedger ledger;
    ledger.value_abs[{1, ambKey()}] = {7.0, 1e-4, 1e-4};    // var == var_first
    ledger.value_abs[{2, ambKey()}] = {12.5, 1e-6, 1e-3};   // var = 1e-3 x var_first

    const Outcome o = elect(r, incumbency("AAAA"), ledger, armed(0), true,
                            /*max_var_ratio=*/1e-2);

    EXPECT_EQ(o.elected, "CCCC");
    EXPECT_EQ(o.tally.candidacy_skip_var_refused, 1);
    EXPECT_EQ(o.tally.handover_continuous, 1);
    EXPECT_EQ(o.tally.handover_refused_ratio, 0) << "the refusal happened at "
                                                    "CANDIDACY, so the branch never "
                                                    "saw an unusable successor";
}

TEST(CandidacyUsableElection, AnArcFreedThisEpochIsNotACandidate) {
    // BBBB's ledger value passes every other test but is about to be stale: a slip
    // response freed that arc this epoch, so its level is re-drawn from one epoch
    // of code before the next harvest.  This is the re-coupling edge of invariant
    // #1, checked at candidacy time.
    auto r = makeRoster({"AAAA", "BBBB", "CCCC", "DDDD"}, {9, 8, 7, 6});
    AnchorHandoverLedger ledger;
    ledger.value_abs[{1, ambKey()}] = {7.0, 1e-6, 1e-3};
    ledger.value_abs[{2, ambKey()}] = {12.5, 1e-6, 1e-3};
    const std::set<std::pair<int, std::string>> freed{{1, ambKey()}};

    const Outcome o = elect(r, incumbency("AAAA"), ledger, armed(0, 0, &freed), true);

    EXPECT_EQ(o.elected, "CCCC");
    EXPECT_DOUBLE_EQ(o.target, 12.5);
    EXPECT_EQ(o.tally.candidacy_skip_freed, 1);
    EXPECT_EQ(o.tally.candidacy_skip_no_entry, 0);
    EXPECT_EQ(o.tally.candidacy_skip_var_refused, 0);
    EXPECT_EQ(o.tally.candidacy_rank_changed, 1);
}

TEST(CandidacyUsableElection, AYoungArcIsRefusedAndZeroDisablesTheTest) {
    auto r = makeRoster({"AAAA", "BBBB", "CCCC", "DDDD"}, {9, 8, 7, 6});
    AnchorHandoverLedger ledger;
    ledger.value_abs[{1, ambKey()}] = {7.0, 1e-6, 1e-3};
    ledger.value_abs[{2, ambKey()}] = {12.5, 1e-6, 1e-3};
    net::ArcTable lc;
    LocalCarry young{};
    young.birth_ep = 980;                       // 20 epochs old at now_ep 1000
    net::putArcIn(lc, {1, kSat, kSig}, &net::ArcState::amb) = young;
    LocalCarry mature{};
    mature.birth_ep = 100;                      // 900 epochs old
    net::putArcIn(lc, {2, kSat, kSig}, &net::ArcState::amb) = mature;

    const Outcome young_refused = elect(r, incumbency("AAAA"), ledger,
                                        armed(60, 1000, nullptr, &lc), true);
    EXPECT_EQ(young_refused.elected, "CCCC");
    EXPECT_EQ(young_refused.tally.candidacy_skip_young, 1);
    EXPECT_EQ(young_refused.tally.candidacy_rank_changed, 1);

    auto r2 = makeRoster({"AAAA", "BBBB", "CCCC", "DDDD"}, {9, 8, 7, 6});
    const Outcome no_bar = elect(r2, incumbency("AAAA"), ledger,
                                 armed(0, 1000, nullptr, &lc), true);
    EXPECT_EQ(no_bar.elected, "BBBB") << "min_age_ep = 0 disables the maturity test";
    EXPECT_EQ(no_bar.tally.candidacy_skip_young, 0);
    EXPECT_EQ(no_bar.tally.candidacy_rank_changed, 0);
}

TEST(CandidacyUsableElection, AnUnknownArcAgeReadsAsMature) {
    // The restored-entry convention: a warm-resume entry (or a missing one) has no
    // birth stamp, and refusing it would refuse every candidate on the first epoch
    // after a resume.  The variance ceiling decides instead.
    auto r = makeRoster({"AAAA", "BBBB", "CCCC", "DDDD"}, {9, 8, 7, 6});
    AnchorHandoverLedger ledger;
    ledger.value_abs[{1, ambKey()}] = {7.0, 1e-6, 1e-3};
    net::ArcTable lc;
    LocalCarry restored{};
    restored.birth_ep = 0;                      // unstamped
    net::putArcIn(lc, {1, kSat, kSig}, &net::ArcState::amb) = restored;

    const Outcome o = elect(r, incumbency("AAAA"), ledger,
                            armed(60, 1000, nullptr, &lc), true);

    EXPECT_EQ(o.elected, "BBBB");
    EXPECT_EQ(o.tally.candidacy_skip_young, 0);
    EXPECT_EQ(o.tally.handover_continuous, 1);
}

// ===========================================================================
// 5: no usable candidate: the unscreened election, counted as a fallback
// ===========================================================================

TEST(CandidacyUsableElection, NoUsableCandidateFallsThroughToTodaysPick) {
    auto r = makeRoster({"AAAA", "BBBB", "CCCC", "DDDD"}, {9, 8, 7, 6});
    AnchorHandoverLedger ledger;          // nobody has an entry

    const Outcome o = elect(r, incumbency("AAAA"), ledger, armed(0), true);

    EXPECT_EQ(o.elected, "BBBB") << "the fall-through must be today's election, "
                                    "unchanged";
    EXPECT_DOUBLE_EQ(o.target, 0.0);
    EXPECT_EQ(o.tally.handover_cold, 1) << "an unavoidable cold handover stays cold "
                                           "and stays counted";
    EXPECT_EQ(o.tally.candidacy_no_usable_fallback, 1);
    EXPECT_EQ(o.tally.candidacy_usable_elected, 0);
    EXPECT_EQ(o.tally.candidacy_rank_changed, 0);
    ASSERT_EQ(o.events.size(), 1u);
    EXPECT_EQ(o.events.front().cand_skipped, 0)
        << "a fall-through is today's election, not a screen that walked past three "
           "candidates — the trace must not claim otherwise";
}

TEST(CandidacyUsableElection, AForcedColdDirectiveMakesEveryCandidateUnusable) {
    // A harness directive for a cold handover is part of the shared predicate, so
    // the pass cannot silently re-pick the station a cold leg was written to land on.
    auto r = makeRoster({"AAAA", "BBBB", "CCCC", "DDDD"}, {9, 8, 7, 6});
    AnchorHandoverLedger ledger;
    ledger.value_abs[{1, ambKey()}] = {7.0, 1e-6, 1e-3};
    ledger.value_abs[{2, ambKey()}] = {12.5, 1e-6, 1e-3};

    NetworkInjectState inject;
    InjectAnchorDirective d;
    d.sat = kSat; d.signal = kSig; d.cold = true;    // forced cold
    inject.anchor_now.push_back(std::move(d));
    inject.armed = true;
    AnchorHygiene hyg;
    NetworkAnchorTallies tally;
    std::map<std::string, std::string> inc = incumbency("AAAA");
    std::map<std::string, double> pin_target;
    std::map<std::string, std::string> this_epoch_anchor;
    std::set<std::string> shared;
    const AnchorCandidacyUsable u = armed(0);
    networkElectPhaseAnchors(true, r.stations, r.present, r.cnd, hyg, tally, inc,
                             pin_target, this_epoch_anchor, shared, &ledger, nullptr,
                             kMaxVar, nullptr, 0.0, &inject, &u);

    EXPECT_EQ(this_epoch_anchor.at(ambKey()), "BBBB");
    EXPECT_DOUBLE_EQ(r.stations[1].pinned_amb_targets.at(ambKey()), 0.0);
    EXPECT_EQ(tally.handover_cold, 1);
    EXPECT_EQ(tally.candidacy_no_usable_fallback, 1);
    EXPECT_EQ(tally.candidacy_usable_elected, 0);
}

// ===========================================================================
// 6: where the pass is not entered
// ===========================================================================

TEST(CandidacyUsableElection, AKeptStickyIncumbentNeverEntersThePass) {
    auto r = makeRoster({"AAAA", "BBBB", "CCCC", "DDDD"}, {9, 8, 7, 6});
    AnchorHandoverLedger ledger;
    ledger.value_abs[{2, ambKey()}] = {12.5, 1e-6, 1e-3};

    const Outcome on  = elect(r, incumbency("AAAA"), ledger, armed(0), /*ban=*/false);
    auto r2 = makeRoster({"AAAA", "BBBB", "CCCC", "DDDD"}, {9, 8, 7, 6});
    AnchorCandidacyUsable off;
    const Outcome offo = elect(r2, incumbency("AAAA"), ledger, off, /*ban=*/false);

    EXPECT_EQ(on.elected, "AAAA");
    EXPECT_EQ(offo.elected, "AAAA");
    EXPECT_EQ(on.tally.candidacy_usable_elected, 0);
    EXPECT_EQ(on.tally.candidacy_no_usable_fallback, 0);
    EXPECT_EQ(on.tally.candidacy_skipped_unusable, 0);
    EXPECT_EQ(on.tally.handover_cold, 0);
    EXPECT_TRUE(on.events.empty()) << "no handover happened at all";
}

TEST(CandidacyUsableElection, AFirstSightingNeverEntersThePass) {
    // No incumbent: there is no outgoing frame, the continuous branch cannot run,
    // and every pin is a 0-pin whichever station takes it.  Screening there would
    // move elections the screening cannot improve, so the pass is not entered.
    auto r = makeRoster({"AAAA", "BBBB", "CCCC", "DDDD"}, {9, 8, 7, 6});
    AnchorHandoverLedger ledger;
    ledger.value_abs[{2, ambKey()}] = {12.5, 1e-6, 1e-3};

    const Outcome o = elect(r, /*incumbent=*/{}, ledger, armed(0), /*ban=*/false);

    EXPECT_EQ(o.elected, "AAAA") << "the top-ranked candidate, exactly as today";
    EXPECT_DOUBLE_EQ(o.target, 0.0);
    EXPECT_EQ(o.tally.candidacy_usable_elected, 0);
    EXPECT_EQ(o.tally.candidacy_no_usable_fallback, 0);
    EXPECT_EQ(o.tally.candidacy_skipped_unusable, 0);
}

// ===========================================================================
// 7: iteration 2, the satellite clause (§15.8).  Iteration 1 screens per family,
//    so a satellite's three bands can elect three hosts: correct one band at a
//    time and, jointly, an inter-frequency inconsistency the per-band producer
//    instruments cannot see (§15.8 has the storm-arm figures).
// ===========================================================================

TEST(CandidacyUsableConfig, SatelliteClauseDefaultsOnAndParses) {
    Loaded d("satdef", "");
    ASSERT_TRUE(d.ok);
    EXPECT_TRUE(d.cfg.networkPhaseDatumCandidacySatellite())
        << "the shipped form is the corrected one; OFF is the "
           "iteration-1 A/B, not the default";
    Loaded off("satoff", "    \"phase_datum_candidacy_satellite\": false,\n");
    ASSERT_TRUE(off.ok);
    EXPECT_FALSE(off.cfg.networkPhaseDatumCandidacySatellite());
}

TEST(CandidacyUsableElection, JointResolutionTakesTheHostUsableForTheMostBands) {
    // BBBB outranks CCCC and is usable for two of the three bands; CCCC is usable
    // for all three.  The family-only rule sends L1C/L2W to BBBB and L5Q to CCCC,
    // a split reference.  The clause sends all three to CCCC.
    auto r = makeRoster3({"AAAA", "BBBB", "CCCC", "DDDD"}, {9, 8, 7, 6},
                         {kSigs3, kSigs3, kSigs3, kSigs3});
    AnchorHandoverLedger ledger;
    ledger.value_abs[{1, amb3("L1C")}] = {1.0, 1e-6, 1e-3};
    ledger.value_abs[{1, amb3("L2W")}] = {2.0, 1e-6, 1e-3};
    // BBBB's L5Q arc is not carry-usable; CCCC's three all are.
    ledger.value_abs[{2, amb3("L1C")}] = {3.0, 1e-6, 1e-3};
    ledger.value_abs[{2, amb3("L2W")}] = {4.0, 1e-6, 1e-3};
    ledger.value_abs[{2, amb3("L5Q")}] = {5.0, 1e-6, 1e-3};
    std::map<std::string, std::string> inc;
    for (const auto& s : kSigs3) inc[amb3(s)] = "AAAA";

    const Outcome3 o = elect3(r, inc, ledger, armed(0, 0, nullptr, nullptr, true),
                              {"all"});

    for (const auto& s : kSigs3)
        EXPECT_EQ(o.anchor.at(amb3(s)), "CCCC") << "band " << s;
    EXPECT_EQ(o.distinctStations(), 1u) << "rule (c): one reference per satellite";
    EXPECT_EQ(o.tally.sat_clause_satellites, 1);
    EXPECT_EQ(o.tally.sat_clause_families, 3);
    EXPECT_EQ(o.tally.sat_clause_split_avoided, 2)
        << "L1C and L2W are the two the family-only rule would have sent to BBBB";
    EXPECT_EQ(o.tally.sat_clause_cold_colocated, 0);
    EXPECT_EQ(o.tally.handover_cold, 0);
    EXPECT_EQ(o.tally.handover_continuous, 3);
    EXPECT_DOUBLE_EQ(o.target.at(amb3("L5Q")), 5.0);
}

TEST(CandidacyUsableElection, IterationOneSplitsTheSameSatelliteTwoWays) {
    // The single-knob A/B for the case above: with the satellite clause off the
    // election is the iteration-1 one, and the satellite ends on two stations.
    auto r = makeRoster3({"AAAA", "BBBB", "CCCC", "DDDD"}, {9, 8, 7, 6},
                         {kSigs3, kSigs3, kSigs3, kSigs3});
    AnchorHandoverLedger ledger;
    ledger.value_abs[{1, amb3("L1C")}] = {1.0, 1e-6, 1e-3};
    ledger.value_abs[{1, amb3("L2W")}] = {2.0, 1e-6, 1e-3};
    ledger.value_abs[{2, amb3("L1C")}] = {3.0, 1e-6, 1e-3};
    ledger.value_abs[{2, amb3("L2W")}] = {4.0, 1e-6, 1e-3};
    ledger.value_abs[{2, amb3("L5Q")}] = {5.0, 1e-6, 1e-3};
    std::map<std::string, std::string> inc;
    for (const auto& s : kSigs3) inc[amb3(s)] = "AAAA";

    const Outcome3 o = elect3(r, inc, ledger,
                              armed(0, 0, nullptr, nullptr, /*satellite=*/false),
                              {"all"});

    EXPECT_EQ(o.anchor.at(amb3("L1C")), "BBBB");
    EXPECT_EQ(o.anchor.at(amb3("L2W")), "BBBB");
    EXPECT_EQ(o.anchor.at(amb3("L5Q")), "CCCC");
    EXPECT_EQ(o.distinctStations(), 2u) << "the defect §15.6 measured, in a test";
    EXPECT_EQ(o.tally.sat_clause_families, 0) << "the clause did not run";
}

TEST(CandidacyUsableElection, ABandNoCandidateCanServeIsPinnedColdAtTheJointHost) {
    // BBBB is usable for two bands and no station is usable for L5Q.  The joint host
    // is still BBBB and L5Q is pinned there at 0: a frame step on the same
    // reference, which is what the design accepts in place of a split.
    auto r = makeRoster3({"AAAA", "BBBB", "CCCC", "DDDD"}, {9, 8, 7, 6},
                         {kSigs3, kSigs3, kSigs3, kSigs3});
    AnchorHandoverLedger ledger;
    ledger.value_abs[{1, amb3("L1C")}] = {1.0, 1e-6, 1e-3};
    ledger.value_abs[{1, amb3("L2W")}] = {2.0, 1e-6, 1e-3};
    ledger.value_abs[{2, amb3("L1C")}] = {3.0, 1e-6, 1e-3};   // CCCC: one band
    std::map<std::string, std::string> inc;
    for (const auto& s : kSigs3) inc[amb3(s)] = "AAAA";

    const Outcome3 o = elect3(r, inc, ledger, armed(0, 0, nullptr, nullptr, true),
                              {"all"});

    for (const auto& s : kSigs3)
        EXPECT_EQ(o.anchor.at(amb3(s)), "BBBB") << "band " << s;
    EXPECT_EQ(o.distinctStations(), 1u);
    EXPECT_EQ(o.tally.sat_clause_cold_colocated, 1) << "L5Q, pinned cold at BBBB";
    EXPECT_DOUBLE_EQ(o.target.at(amb3("L5Q")), 0.0);
    EXPECT_EQ(o.tally.handover_cold, 1);
    EXPECT_EQ(o.tally.handover_continuous, 2);
}

TEST(CandidacyUsableElection, PartialHandoverPrefersTheHostOfTheOtherBands) {
    // L1C and L2W are anchored at CCCC and keep it; only L5Q hands over.  BBBB
    // outranks CCCC and is usable, so the family-only rule would move L5Q there and
    // split the satellite; rule (b) keeps the satellite on CCCC.
    auto r = makeRoster3({"AAAA", "BBBB", "CCCC", "DDDD"}, {9, 8, 7, 6},
                         {kSigs3, kSigs3, kSigs3, kSigs3});
    AnchorHandoverLedger ledger;
    ledger.value_abs[{1, amb3("L5Q")}] = {7.0, 1e-6, 1e-3};   // BBBB usable
    ledger.value_abs[{2, amb3("L5Q")}] = {8.0, 1e-6, 1e-3};   // CCCC usable too
    std::map<std::string, std::string> inc{
        {amb3("L1C"), "CCCC"}, {amb3("L2W"), "CCCC"}, {amb3("L5Q"), "AAAA"}};

    const Outcome3 o = elect3(r, inc, ledger, armed(0, 0, nullptr, nullptr, true),
                              {"L5Q"});

    EXPECT_EQ(o.anchor.at(amb3("L1C")), "CCCC");
    EXPECT_EQ(o.anchor.at(amb3("L2W")), "CCCC");
    EXPECT_EQ(o.anchor.at(amb3("L5Q")), "CCCC")
        << "the held host wins over the higher-ranked stranger — rule (b)";
    EXPECT_EQ(o.distinctStations(), 1u);
    EXPECT_EQ(o.tally.sat_clause_colocated, 1);
    EXPECT_EQ(o.tally.sat_clause_split_avoided, 1) << "BBBB is the family-only pick";
    EXPECT_DOUBLE_EQ(o.target.at(amb3("L5Q")), 8.0);
    EXPECT_EQ(o.tally.handover_cold, 0);
}

TEST(CandidacyUsableElection, PartialHandoverFallsBackWhenTheHeldHostCannotServe) {
    // The held host CCCC does not track L5Q at all, so rule (b) has nothing to take
    // and rule (a) elects the best usable candidate.  The satellite splits here for
    // a structural reason (no single station can host all three), and the tallies
    // show it.
    auto r = makeRoster3({"AAAA", "BBBB", "CCCC", "DDDD"}, {9, 8, 7, 6},
                         {kSigs3, kSigs3, {"L1C", "L2W"}, kSigs3});
    AnchorHandoverLedger ledger;
    ledger.value_abs[{1, amb3("L5Q")}] = {7.0, 1e-6, 1e-3};
    std::map<std::string, std::string> inc{
        {amb3("L1C"), "CCCC"}, {amb3("L2W"), "CCCC"}, {amb3("L5Q"), "AAAA"}};

    const Outcome3 o = elect3(r, inc, ledger, armed(0, 0, nullptr, nullptr, true),
                              {"L5Q"});

    EXPECT_EQ(o.anchor.at(amb3("L5Q")), "BBBB");
    EXPECT_EQ(o.tally.sat_clause_colocated, 0);
    EXPECT_EQ(o.tally.sat_clause_families, 1);
    EXPECT_EQ(o.distinctStations(), 2u) << "unavoidable: CCCC cannot host L5Q";
}

TEST(CandidacyUsableElection, SatelliteClauseReportsWhenNoCandidateIsUsable) {
    // No station has a usable carry: the clause finds no host, the election is the
    // unscreened one, and the satellite still ends on one station, because the
    // coverage sweep is per-satellite-consistent when every band has the same
    // candidate order.
    auto r = makeRoster3({"AAAA", "BBBB", "CCCC", "DDDD"}, {9, 8, 7, 6},
                         {kSigs3, kSigs3, kSigs3, kSigs3});
    AnchorHandoverLedger ledger;   // empty
    std::map<std::string, std::string> inc;
    for (const auto& s : kSigs3) inc[amb3(s)] = "AAAA";

    const Outcome3 o = elect3(r, inc, ledger, armed(0, 0, nullptr, nullptr, true),
                              {"all"});

    for (const auto& s : kSigs3) EXPECT_EQ(o.anchor.at(amb3(s)), "BBBB");
    EXPECT_EQ(o.distinctStations(), 1u);
    EXPECT_EQ(o.tally.sat_clause_no_usable, 1);
    EXPECT_EQ(o.tally.sat_clause_families, 0);
    EXPECT_EQ(o.tally.candidacy_no_usable_fallback, 3);
    EXPECT_EQ(o.tally.handover_cold, 3);
}
