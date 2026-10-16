/**
 * @file test_signal_roster.cpp
 * @brief The per-station signal roster, card SID phase 1
 *        (docs/refound/SID_signal_identity_2026-09-22.md section 5, P5).
 *
 * The roster reproduces the rules that already decide which signal stands for a
 * band; it does not re-derive them.  Phase 1's armed byte-identity protects the
 * solve, but only these tests protect the role assignment that phases 2 to 6 read.
 * Pinned here, over synthetic header contents with no station, model or solve:
 *
 *  - the Modelled role is the latch's own row, under the priority rule and under the
 *    count rule alike (the roster never re-runs the decision, so both cases are one
 *    code path fed two different latches, and the roster does not depend on which
 *    rule produced the latch);
 *  - a card SCC companion carries the Companion role and is not an exclusion;
 *  - every other tracked signal is an exclusion with a reason, and the reasons
 *    distinguish the allow list, a band the latch decided against, and a band the
 *    latch does not carry;
 *  - a band the model does not carry is Tracked only;
 *  - the arc-level tracker's own pick is reproduced, so the disagreement the card is
 *    named for is stated in one place instead of inferred.
 */

#include <gtest/gtest.h>

#include "network/state/Entities.hpp"
#include "network/assembly/SignalRoster.hpp"
#include "common/Constants.hpp"
#include "common/PppObservation.hpp"
#include "common/PppUtils.hpp"
#include "logging/LogManager.hpp"
#include "network/solve/AnchorReentry.hpp"
#include "network/slip/RawObservables.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

/// A station tracking the civil and the semi-codeless band-1 channels plus L2.
const std::vector<std::string> kTrackedL1cL1w{
    "C1C", "L1C", "C1W", "L1W", "C2W", "L2W"
};

std::vector<RosterLatchRow> latch(std::initializer_list<std::pair<char, const char*>> rows) {
    std::vector<RosterLatchRow> out;
    for (const auto& [t, s] : rows) out.push_back(RosterLatchRow{ t, std::string(s) });
    return out;
}

bool hasExclusion(const BandRoster& br, const std::string& sig, const std::string& reason) {
    return std::any_of(br.excluded.begin(), br.excluded.end(),
                       [&](const std::pair<std::string, const char*>& e) {
                           return e.first == sig && reason == e.second;
                       });
}

}  // namespace

// ---------------------------------------------------------------------------
// The Modelled role reproduces the latch
// ---------------------------------------------------------------------------

TEST(SignalRosterTest, ModelledRoleIsTheLatchUnderThePriorityRule) {
    // The priority list of the RF-0hv postures is W before C, so a station that
    // tracks both latches L1W / C1W.  The roster reports that and does not fall back
    // to the lexically first channel, which is what the arc-level tracker does and
    // what the card is about.
    const auto cells = signalRosterBuildCells(
        kTrackedL1cL1w, latch({ { 'L', "L1W" }, { 'C', "C1W" },
                                { 'L', "L2W" }, { 'C', "C2W" } }),
        /*companion_allow=*/{}, /*companions_armed=*/false, /*allowlist=*/nullptr);

    ASSERT_EQ(cells.count('1'), 1u);
    const BandRoster& b1 = cells.at('1');
    EXPECT_EQ(b1.modelled_phase, "L1W");
    EXPECT_EQ(b1.modelled_code, "C1W");
    EXPECT_EQ(b1.tracked_phase, (std::vector<std::string>{ "L1C", "L1W" }));
    EXPECT_EQ(b1.tracked_code, (std::vector<std::string>{ "C1C", "C1W" }));
    EXPECT_TRUE(b1.modelled_extra.empty()) << "phase 6 only";
    // The channels the latch decided against are exclusions with the reason that
    // says so, not silent omissions.
    EXPECT_TRUE(hasExclusion(b1, "L1C", "not_latched"));
    EXPECT_TRUE(hasExclusion(b1, "C1C", "not_latched"));
    EXPECT_EQ(b1.excluded.size(), 2u);

    const BandRoster& b2 = cells.at('2');
    EXPECT_EQ(b2.modelled_phase, "L2W");
    EXPECT_EQ(b2.modelled_code, "C2W");
    EXPECT_TRUE(b2.excluded.empty());
}

TEST(SignalRosterTest, ModelledRoleIsTheLatchUnderTheCountRule) {
    // The count rule at the same station latches the channel with the most
    // satellites, which on the six stations card CSL names is L1C.  The roster is
    // the same code path: it reads the latch it is given, so the rule that produced
    // it is invisible here.  That is the property, not an accident of the fixture.
    const auto cells = signalRosterBuildCells(
        kTrackedL1cL1w, latch({ { 'L', "L1C" }, { 'C', "C1C" },
                                { 'L', "L2W" }, { 'C', "C2W" } }),
        /*companion_allow=*/{}, /*companions_armed=*/false, /*allowlist=*/nullptr);

    const BandRoster& b1 = cells.at('1');
    EXPECT_EQ(b1.modelled_phase, "L1C");
    EXPECT_EQ(b1.modelled_code, "C1C");
    EXPECT_TRUE(hasExclusion(b1, "L1W", "not_latched"));
    EXPECT_TRUE(hasExclusion(b1, "C1W", "not_latched"));
}

TEST(SignalRosterTest, ALatchedBandTheReceiverDoesNotReportStillHasItsCell) {
    // The latch is the estimator's key, so a reader asking what this station
    // carries on band 2 needs an answer on the epochs the arc is absent too.  That
    // is the same reason the arc-level tracker reads the latch and not the epoch's
    // admitted row set.
    const auto cells = signalRosterBuildCells(
        { "C1W", "L1W" }, latch({ { 'L', "L1W" }, { 'C', "C1W" },
                                  { 'L', "L2W" }, { 'C', "C2W" } }),
        {}, false, nullptr);
    ASSERT_EQ(cells.count('2'), 1u);
    const BandRoster& b2 = cells.at('2');
    EXPECT_EQ(b2.modelled_phase, "L2W");
    EXPECT_TRUE(b2.tracked_phase.empty());
    EXPECT_TRUE(b2.excluded.empty());
}

// ---------------------------------------------------------------------------
// Bands the model does not carry
// ---------------------------------------------------------------------------

TEST(SignalRosterTest, ABandTheModelDoesNotCarryIsTrackedOnly) {
    // Band 5 on a dual posture: the receiver reports it, the latch does not name
    // it, and the roster reports it as tracked with no modelled role rather than
    // inventing one.  Its exclusions carry `no_latch`, which is a different
    // statement from `not_latched` and stays separable from it.
    const auto cells = signalRosterBuildCells(
        { "C1W", "L1W", "C5Q", "L5Q", "C5X", "L5X" },
        latch({ { 'L', "L1W" }, { 'C', "C1W" } }), {}, false, nullptr);

    ASSERT_EQ(cells.count('5'), 1u);
    const BandRoster& b5 = cells.at('5');
    EXPECT_TRUE(b5.modelled_phase.empty());
    EXPECT_TRUE(b5.modelled_code.empty());
    EXPECT_EQ(b5.tracked_phase, (std::vector<std::string>{ "L5Q", "L5X" }));
    EXPECT_EQ(b5.excluded.size(), 4u);
    for (const char* s : { "L5Q", "L5X", "C5Q", "C5X" })
        EXPECT_TRUE(hasExclusion(b5, s, "no_latch")) << s;
}

// ---------------------------------------------------------------------------
// Companions (card SCC)
// ---------------------------------------------------------------------------

TEST(SignalRosterTest, CompanionCodesCarryTheCompanionRoleAndAreNotExclusions) {
    const auto cells = signalRosterBuildCells(
        kTrackedL1cL1w, latch({ { 'L', "L1W" }, { 'C', "C1W" },
                                { 'L', "L2W" }, { 'C', "C2W" } }),
        /*companion_allow=*/{ "C1C", "C1X" }, /*companions_armed=*/true, nullptr);

    const BandRoster& b1 = cells.at('1');
    EXPECT_EQ(b1.companion_code, (std::vector<std::string>{ "C1C" }));
    // C1X is on the allow list but untracked here, so it is neither a companion nor
    // an exclusion: the roster reports what exists, not what a list mentions.
    EXPECT_FALSE(hasExclusion(b1, "C1X", "not_latched"));
    // The companion is not also an exclusion, and the phase it shares its attribute
    // with still is: the code side has a second channel and the phase side does not,
    // which is the asymmetry the audit measured.
    EXPECT_FALSE(hasExclusion(b1, "C1C", "not_latched"));
    EXPECT_TRUE(hasExclusion(b1, "L1C", "not_latched"));
    EXPECT_EQ(b1.excluded.size(), 1u);
}

TEST(SignalRosterTest, CompanionsNeedTheCanonicalPresentAndStayOnBandOne) {
    // The canonical must be tracked this epoch, which is the rule
    // SecondCodeScope.cpp and PppModel implement.  Without it the second
    // channel would ride alone and its inter-signal state would have nothing to be
    // measured against.
    const auto no_canon = signalRosterBuildCells(
        { "C1C", "L1C", "L1W" }, latch({ { 'L', "L1W" }, { 'C', "C1W" } }),
        { "C1C" }, true, nullptr);
    EXPECT_TRUE(no_canon.at('1').companion_code.empty());

    // Band 2 has no companion mechanism at all (card SCC is band 1 only).
    const auto b2 = signalRosterBuildCells(
        { "C2W", "L2W", "C2L" }, latch({ { 'L', "L2W" }, { 'C', "C2W" } }),
        { "C2L" }, true, nullptr);
    EXPECT_TRUE(b2.at('2').companion_code.empty());
    EXPECT_TRUE(hasExclusion(b2.at('2'), "C2L", "not_latched"));
}

// ---------------------------------------------------------------------------
// The allow list
// ---------------------------------------------------------------------------

TEST(SignalRosterTest, TheAllowListIsAPerStationExclusionReason) {
    // network.signal_min_station_fraction is a global whole-run gate, so a dropped
    // channel is not visible per station.  As a roster reason it becomes visible at
    // each station where it removes a channel, which is what phase 1 demonstrates.
    const std::set<std::string> allow{ "C1W", "L1W", "C2W", "L2W" };
    const auto cells = signalRosterBuildCells(
        kTrackedL1cL1w, latch({ { 'L', "L1W" }, { 'C', "C1W" },
                                { 'L', "L2W" }, { 'C', "C2W" } }),
        {}, false, &allow);

    const BandRoster& b1 = cells.at('1');
    EXPECT_TRUE(hasExclusion(b1, "L1C", "allowlist"));
    EXPECT_TRUE(hasExclusion(b1, "C1C", "allowlist"));
    // The allow list is the earlier gate, so it names the exclusion rather than the
    // latch: the two reasons are not interchangeable.
    EXPECT_FALSE(hasExclusion(b1, "L1C", "not_latched"));
}

TEST(SignalRosterTest, AnEmptyAllowListAdmitsEverything) {
    const std::set<std::string> empty;
    const auto cells = signalRosterBuildCells(
        kTrackedL1cL1w, latch({ { 'L', "L1W" }, { 'C', "C1W" } }), {}, false, &empty);
    EXPECT_TRUE(hasExclusion(cells.at('1'), "L1C", "not_latched"));
}

// ---------------------------------------------------------------------------
// The tracker's own pick, stated in one place
// ---------------------------------------------------------------------------

TEST(SignalRosterTest, TrackerPicksTheLexicallyLowestPhaseWithAMatchingCode) {
    // RawObservables.cpp keeps a phase only when its matching code is
    // reported, then sorts on the band digit alone and takes the front.  On this
    // toolchain that is lexical order within a band, which is why a station
    // latching L1W is tracked as L1C.
    const auto picks = signalRosterTrackerPicks(kTrackedL1cL1w);
    EXPECT_EQ(picks.at('1'), "L1C");
    EXPECT_EQ(picks.at('2'), "L2W");

    // A phase with no matching code is not a candidate at all.
    const auto no_code = signalRosterTrackerPicks({ "L1C", "C1W", "L1W" });
    EXPECT_EQ(no_code.at('1'), "L1W");

    // The disagreement the card is named for, in one line.
    const auto cells = signalRosterBuildCells(
        kTrackedL1cL1w, latch({ { 'L', "L1W" }, { 'C', "C1W" } }), {}, false, nullptr);
    EXPECT_NE(picks.at('1'), cells.at('1').modelled_phase);
}

// ---------------------------------------------------------------------------
// The IF pin as a roster writer (card SID phase 2)
// ---------------------------------------------------------------------------

TEST(SignalRosterTest, TheIfPinDesignationIsPerConstellationAndBand) {
    // The pin is elected per satellite over the network's registered code keys and
    // names two code signals, while a roster cell is per (station, constellation,
    // band).  The resolution is therefore a vote, and a band whose satellites did
    // not agree is reported as ambiguous rather than collapsed silently.
    //
    // The key is the roster's constellation string, not the satellite id's first
    // character: the datum seam keys its own maps by the letter, and picks tallied
    // under the letter match no roster cell.
    std::map<std::string, std::pair<std::string, std::string>> pins{
        { "G01", { "C1W", "C2W" } },
        { "G02", { "C1W", "C2W" } },
        { "G03", { "C1C", "C2W" } },   // this satellite's C1W is not registered
        { "E01", { "C1C", "C5Q" } },
    };
    std::set<std::string> ambiguous;
    std::map<std::string, std::map<std::string, long long>> votes;
    const auto picks = signalRosterDatumPicks(pins, &votes, &ambiguous);

    EXPECT_EQ(picks.at("GPS|1"), "C1W");            // 2 votes against 1
    EXPECT_EQ(picks.at("GPS|2"), "C2W");
    EXPECT_EQ(picks.at("Galileo|1"), "C1C");
    EXPECT_EQ(picks.at("Galileo|5"), "C5Q");
    EXPECT_EQ(picks.count("G|1"), 0u) << "the RINEX letter is not the roster's key";
    EXPECT_EQ(ambiguous, (std::set<std::string>{ "GPS|1" }));
    EXPECT_EQ(votes.at("GPS|1").at("C1W"), 2);
    EXPECT_EQ(votes.at("GPS|1").at("C1C"), 1);
}

TEST(SignalRosterTest, TheDatumFieldsAreWrittenAndThePhaseIsNeverDerived) {
    StationRoster sr;
    sr["GPS"] = signalRosterBuildCells(
        kTrackedL1cL1w, latch({ { 'L', "L1W" }, { 'C', "C1W" },
                                { 'L', "L2W" }, { 'C', "C2W" } }),
        {}, false, nullptr);

    const std::map<std::string, std::string> picks{ { "GPS|1", "C1W" }, { "GPS|2", "C2W" } };
    const auto w = signalRosterApplyDatum(sr, picks, nullptr);

    EXPECT_EQ(w.cells, 2);
    EXPECT_EQ(w.with_phase, 2);
    EXPECT_EQ(w.off_model, 0);
    EXPECT_EQ(sr.at("GPS").at('1').datum_code, "C1W");
    EXPECT_EQ(sr.at("GPS").at('1').datum_phase, "L1W");
    EXPECT_EQ(sr.at("GPS").at('2').datum_code, "C2W");
    EXPECT_EQ(sr.at("GPS").at('2').datum_phase, "L2W");

    // A station whose latch chose the other band-1 channel gets the designation but
    // no datum phase: the pinned code is not what this station models, so naming a
    // phase would derive a signal the estimator does not carry.
    StationRoster other;
    other["GPS"] = signalRosterBuildCells(
        kTrackedL1cL1w, latch({ { 'L', "L1C" }, { 'C', "C1C" },
                                { 'L', "L2W" }, { 'C', "C2W" } }),
        {}, false, nullptr);
    const std::set<std::string> amb{ "GPS|1" };
    const auto w2 = signalRosterApplyDatum(other, picks, &amb);
    EXPECT_EQ(w2.cells, 2);
    EXPECT_EQ(w2.with_phase, 1);          // band 2 only
    EXPECT_EQ(w2.off_model, 1);
    EXPECT_EQ(w2.ambiguous, 1);
    EXPECT_EQ(other.at("GPS").at('1').datum_code, "C1W");
    EXPECT_TRUE(other.at("GPS").at('1').datum_phase.empty());

    // A band nothing pinned keeps both fields empty.
    StationRoster b5;
    b5["GPS"] = signalRosterBuildCells({ "C5Q", "L5Q" }, {}, {}, false, nullptr);
    const auto w3 = signalRosterApplyDatum(b5, picks, nullptr);
    EXPECT_EQ(w3.cells, 0);
    EXPECT_TRUE(b5.at("GPS").at('5').datum_code.empty());
    EXPECT_TRUE(b5.at("GPS").at('5').datum_phase.empty());
}

// ---------------------------------------------------------------------------
// The tracker's lookup bound to the roster (card SID phase 2)
// ---------------------------------------------------------------------------

namespace {

/// One station's roster, as the run context holds it.
SignalRoster oneStationRoster(const std::vector<std::string>& tracked,
                              const std::vector<RosterLatchRow>& rows) {
    SignalRoster rs;
    rs.active = true;
    rs.by_station.assign(1, {});
    rs.by_station[0]["GPS"] = signalRosterBuildCells(tracked, rows, {}, false, nullptr);
    return rs;
}

const double kL1m = Constants::SPEED_OF_LIGHT / Constants::GPS_L1_FREQ;
const double kL2m = Constants::SPEED_OF_LIGHT / Constants::GPS_L2_FREQ;

/// A GPS observation reporting the listed phases (with their matching code unless
/// the name is listed in @p no_code) at a nominal range.
PppObservation rawObs(const std::string& sat, const std::vector<std::string>& phases,
                      const std::set<std::string>& no_code = {}) {
    PppObservation o{};
    o.satellite = sat;
    o.system    = System::GPS;
    o.time      = GnssTime{ 2400, 0.0 };
    o.elevation = 45.0;
    for (const auto& p : phases) {
        const double lam = (p[1] == '1') ? kL1m : kL2m;
        o.raw_observed[p] = 22'000'000.0 / lam;
        if (!no_code.count(p)) o.raw_observed["C" + p.substr(1)] = 22'000'000.0;
    }
    return o;
}

}  // namespace

TEST(SignalRosterTest, TheTrackerLookupBoundToTheRosterReturnsTheModelledPhase) {
    const auto rs = oneStationRoster(kTrackedL1cL1w,
                                     latch({ { 'L', "L1W" }, { 'C', "C1W" },
                                             { 'L', "L2W" }, { 'C', "C2W" } }));
    EXPECT_EQ(signalRosterModelledPhase(rs, 0, "GPS", '1'), "L1W");
    EXPECT_EQ(signalRosterModelledPhase(rs, 0, "GPS", '2'), "L2W");
    // Every way the roster can have no answer returns empty, never a guess: an unknown
    // station, an unknown constellation, an unknown band.  The caller decides what
    // an empty answer means, and the run-loop binding counts it.
    EXPECT_TRUE(signalRosterModelledPhase(rs, 1, "GPS", '1').empty());
    EXPECT_TRUE(signalRosterModelledPhase(rs, -1, "GPS", '1').empty());
    EXPECT_TRUE(signalRosterModelledPhase(rs, 0, "GAL", '1').empty());
    EXPECT_TRUE(signalRosterModelledPhase(rs, 0, "GPS", '5').empty());
}

TEST(SignalRosterTest, TheRosterBoundTrackerNamesTheModelledPhaseAndFallsBackWithoutItsCode) {
    // The selection rule of card GRK, driven from the roster instead of the station
    // latch: card SID phase 2 in one pass.
    LogManager     lm;
    PppUtils       utils{ lm };
    const auto     rs = oneStationRoster(kTrackedL1cL1w,
                                         latch({ { 'L', "L1W" }, { 'C', "C1W" },
                                                 { 'L', "L2W" }, { 'C', "C2W" } }));
    const TrackerLatchLookup lookup =
        [&rs](int sta, const std::string& system, char band) -> std::string {
            return signalRosterModelledPhase(rs, sta, system, band);
        };

    // (1) Both band-1 channels reported with their codes: the roster's modelled
    //     phase wins, where the lexical rule would have named L1C.
    {
        const std::vector<std::vector<PppObservation>> modeled{
            { rawObs("G01", { "L1C", "L1W", "L2W" }) } };
        RawSlipEpoch   ep;
        SlipQcCounters cnt;
        ep.reset(1);
        networkComputeRawObservables(modeled, { true }, 1, utils, ep, false,
                                     /*tracker_modeled_first=*/true, &cnt, &lookup);
        ASSERT_EQ(ep.rows.at(0).size(), 1u);
        EXPECT_EQ(ep.rows.at(0)[0].sig1, "L1W");
        EXPECT_EQ(ep.rows.at(0)[0].sig2, "L2W");
        EXPECT_EQ(cnt.gap_tracker_key_mismatch, 0)
            << "armed, the tracker's signal IS the roster's by construction";
    }

    // (2) The modelled phase is reported without its matching code: the observables
    //     are built from the pair, so that band falls back to the lexical rule.
    //     This mismatch is what the counter exists for.
    {
        const std::vector<std::vector<PppObservation>> modeled{
            { rawObs("G01", { "L1C", "L1W", "L2W" }, /*no_code=*/{ "L1W" }) } };
        RawSlipEpoch   ep;
        SlipQcCounters cnt;
        ep.reset(1);
        networkComputeRawObservables(modeled, { true }, 1, utils, ep, false, true,
                                     &cnt, &lookup);
        ASSERT_EQ(ep.rows.at(0).size(), 1u);
        EXPECT_EQ(ep.rows.at(0)[0].sig1, "L1C");
        EXPECT_EQ(ep.rows.at(0)[0].sig2, "L2W");
    }

    // (3) Unarmed with the same lookup bound: the lexical rule is unchanged and the
    //     counter reports the size of the affected population instead (the
    //     both-states telemetry of card GRK, inherited by this phase).
    {
        const std::vector<std::vector<PppObservation>> modeled{
            { rawObs("G01", { "L1C", "L1W", "L2W" }) } };
        RawSlipEpoch   ep;
        SlipQcCounters cnt;
        ep.reset(1);
        networkComputeRawObservables(modeled, { true }, 1, utils, ep, false,
                                     /*tracker_modeled_first=*/false, &cnt, &lookup);
        ASSERT_EQ(ep.rows.at(0).size(), 1u);
        EXPECT_EQ(ep.rows.at(0)[0].sig1, "L1C");
        EXPECT_EQ(cnt.gap_tracker_key_mismatch, 1);
    }
}

// ---------------------------------------------------------------------------
// First-sighting-on-return detection (card SID phase 4, S2)
// ---------------------------------------------------------------------------
//
// The election calls a family a first sighting when it had no incumbent at the
// previous epoch.  The read that phase 4a adds asks the ledger whether that family
// had in fact just returned, and the answer is built from the family census below.
// These cases pin the census the detection is decided on: what "returning" counts
// as, what it does not, and that another family never contributes to it.

namespace {

ArcLedger::Key FK(int sta, const char* sat, const char* sig) {
    return ArcLedger::Key{ sta, std::string(sat), std::string(sig) };
}

/// One admitted epoch, raw side driven alongside (the producer's own ordering).
void fsAdmit(ArcLedger& led, long long ep, const std::vector<ArcLedger::Key>& arcs) {
    // One keeper arc per station, on a satellite no case queries: a station that
    // contributed nothing has its arcs frozen rather than gapped, so a test that
    // drops a station's only arc would exercise the freeze instead of the gap.
    std::vector<ArcLedger::Key> all = arcs;
    for (int sta = 0; sta < 3; ++sta) all.push_back(FK(sta, "G99", "L9X"));
    led.beginEpoch(ep, static_cast<double>(ep) * 30.0);
    for (const auto& k : all) led.observe(k, /*modelled=*/true);
    std::set<int> present;
    for (const auto& k : all) present.insert(std::get<0>(k));
    std::vector<std::pair<ArcLedger::Key, ArcStep>> raw_edges, adm_edges;
    led.endEpoch(present, raw_edges);
    led.admitEpoch(std::set<ArcLedger::Key>(all.begin(), all.end()), adm_edges);
}

}  // namespace

TEST(SignalRosterTest, FirstSightingOnReturnSeesTheReturningArcs) {
    // A family whose arcs are in a Gap or have just Reentered is a return, and the
    // election reading it as a birth is the class the audit named.  Both states
    // count toward the returning half; a Tracking arc does not.
    net::NetworkEntities ents;
    ArcLedger led{ ents };
    led.configure(/*horizon_s=*/3900.0, kArcLedgerForgetEp);
    const auto a = FK(0, "G30", "L1W");
    const auto b = FK(1, "G30", "L1W");
    const auto c = FK(2, "G30", "L1W");
    for (long long ep = 0; ep <= 2; ++ep) fsAdmit(led, ep, { a, b, c });

    // a and b drop out; c keeps the family present so the stations stay in the
    // admitted present set and the two gaps are genuine per-arc gaps.
    fsAdmit(led, 3, { b, c });          // a gaps
    fsAdmit(led, 4, { c, a });          // a re-enters, b gaps

    int tr = -1, gp = -1, dd = -1;
    anchorReentryFamilyCensus(led, "G30", "L1W", tr, gp, dd);
    EXPECT_EQ(gp, 2) << "one Reentered plus one Gap is the returning population";
    EXPECT_EQ(tr, 1);
    EXPECT_EQ(dd, 0);
}

TEST(SignalRosterTest, FirstSightingOnReturnIgnoresOtherFamilies) {
    // The detection is per (satellite, signal): the same satellite on another band
    // and another satellite on the same band are not counted into it.
    net::NetworkEntities ents;
    ArcLedger led{ ents };
    led.configure(3900.0, kArcLedgerForgetEp);
    const auto a  = FK(0, "G30", "L1W");
    const auto a2 = FK(0, "G30", "L2W");
    const auto o  = FK(0, "G31", "L1W");
    const auto keep = FK(1, "G30", "L1W");
    for (long long ep = 0; ep <= 2; ++ep) fsAdmit(led, ep, { a, a2, o, keep });
    fsAdmit(led, 3, { a, a2, o, keep });
    fsAdmit(led, 4, { a, keep });        // a2 and o gap; a keeps tracking

    int tr = -1, gp = -1, dd = -1;
    anchorReentryFamilyCensus(led, "G30", "L1W", tr, gp, dd);
    EXPECT_EQ(tr, 2);
    EXPECT_EQ(gp, 0) << "the L2W and G31 gaps belong to other families";

    anchorReentryFamilyCensus(led, "G30", "L2W", tr, gp, dd);
    EXPECT_EQ(gp, 1);
    EXPECT_EQ(tr, 0);
}

TEST(SignalRosterTest, AGenuineFirstSightingHasNoReturningArcs) {
    // The other side of the class, and the reason the counter needs a denominator:
    // a family rising for the first time reads zero returning arcs, so a run
    // whose first-sighting count is high and whose return count is zero is a run
    // with nothing to fix.
    net::NetworkEntities ents;
    ArcLedger led{ ents };
    led.configure(3900.0, kArcLedgerForgetEp);
    const auto a = FK(0, "G30", "L1W");
    fsAdmit(led, 0, { a });
    fsAdmit(led, 1, { a });

    int tr = -1, gp = -1, dd = -1;
    anchorReentryFamilyCensus(led, "G30", "L1W", tr, gp, dd);
    EXPECT_EQ(tr, 1);
    EXPECT_EQ(gp, 0);
    EXPECT_EQ(dd, 0);

    // A family the ledger has never seen at all is all zeros, not an error.
    anchorReentryFamilyCensus(led, "G99", "L1W", tr, gp, dd);
    EXPECT_EQ(tr, 0);
    EXPECT_EQ(gp, 0);
    EXPECT_EQ(dd, 0);
}

TEST(SignalRosterTest, RoleNamesAreStable) {
    // The trace and the run-end table are read by name offline.
    EXPECT_STREQ(signalRoleName(SignalRole::Tracked), "tracked");
    EXPECT_STREQ(signalRoleName(SignalRole::Modelled), "modelled");
    EXPECT_STREQ(signalRoleName(SignalRole::Datum), "datum");
    EXPECT_STREQ(signalRoleName(SignalRole::Companion), "companion");
    EXPECT_STREQ(signalRoleName(SignalRole::Excluded), "excluded");
}

}  // namespace GnssProcessor
