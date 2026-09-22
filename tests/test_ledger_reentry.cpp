/**
 * @file test_ledger_reentry.cpp
 * @brief Card SID phase 3 — the gap RE-ENTRY RESPONSE driven by the arc ledger's
 *        ADMITTED lifecycle (`network.ledger_reentry`, R5 of the build brief).
 *
 * ## What is under test
 *
 * Phases 1 and 2 built the roster and the ledger and gave the ledger one reader.
 * Phase 3 makes the ledger the POPULATION of the response, which is the first
 * time anything the ledger says changes the solve.  Three things therefore have
 * to be pinned before that population is trusted:
 *
 *  - the POPULATION itself: the admitted Gap-to-Reentered set, restricted to
 *    modelled signals and to the carry horizon.  It is a QUERY on the ledger's
 *    admitted timestamps, not a step, because the admitted machine is advanced
 *    once per epoch far downstream of the response;
 *  - the VERDICT LOOKUP and the response it drives: an arc whose satellite has a
 *    row-level `shifted` resumption is freed, and the response is keyed by the
 *    same (station, satellite, signal) as the population, so card GRK's
 *    `gap_reentry_unreachable` cannot be fed from this path;
 *  - the NO-VERDICT policy: an arc no raw tier saw is bridged and counted under
 *    `bridge`, and freed under `refloat` when the existing unmeasurable sub-knob
 *    admits it.  That population is the whole reason the phase exists (phase 1
 *    measured it as tens of thousands of raw-Tracking / admitted-Gap arcs), so
 *    the two policies must be separable by counters alone.
 *
 * The prune edge of R3 is pinned on the ledger itself (the horizon death), where
 * it can be driven without a network.
 *
 * Everything below drives the machine and the drain directly: no station build,
 * no RINEX, no solve.  The traces object is constructed over a scratch path and
 * its streams stay closed, so no file is written.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "filter/ArcLedger.hpp"
#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "slip/NetworkSlipQc.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>

#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace GnssProcessor {
namespace {

constexpr const char* kSta  = "TEST00XXX";
constexpr const char* kSat  = "G22";
constexpr const char* kSig1 = "L1C";
constexpr const char* kSig2 = "L2W";
constexpr double kDt = 30.0;          // the producer's epoch interval on both beds

std::shared_ptr<spdlog::logger> nullLogger() {
    return std::make_shared<spdlog::logger>(
        "test_ledger_reentry", std::make_shared<spdlog::sinks::null_sink_mt>());
}

ArcLedger::Key K(int sta, const char* sat, const char* sig) {
    return ArcLedger::Key{ sta, std::string(sat), std::string(sig) };
}

/// Advance the ADMITTED lifecycle by one epoch over @p admitted.
void admit(ArcLedger& led, long long ep, const std::set<ArcLedger::Key>& admitted) {
    led.beginEpoch(ep, static_cast<double>(ep) * kDt);
    std::vector<std::pair<ArcLedger::Key, ArcStep>> edges;
    led.admitEpoch(admitted, edges);
}

/// A station with a leveling carry on both bands of @ref kSat, a ledger whose
/// admitted side has been driven to a chosen shape, and the epoch scaffolding the
/// drain needs.  The carry posture mirrors tests/test_gap_reentry_relevel.cpp so
/// the two tiers are compared on the same bed.
struct Harness {
    LogManager              log_manager;
    GnssConfig              config;
    std::vector<NetStation> stations;
    std::vector<bool>       present{true};
    std::set<std::pair<int, std::string>> carry_slipped_ambs;
    RawSlipEpoch            raw;
    SlipQcParams            params;
    SlipQcCounters          counters;
    RelevelEpochState       relevel;
    std::map<std::tuple<int, std::string, std::string>, RelevelPend> pends;
    std::map<std::tuple<int, std::string, std::string>, LocalCarry>  local_carry;
    ArcLedger               ledger;
    ArcReentryEpoch         sid;
    ArcReentryOutcomeMap    outcome;
    double                  pl_now_s{271200.0};
    double                  pl_max_gap_s{900.0};
    SyncedEpoch             se;
    NetworkRunTraces        traces{std::filesystem::temp_directory_path()
                                   / "gnss_ledger_reentry_test_unused"};
    std::shared_ptr<spdlog::logger> log{nullLogger()};

    Harness() {
        stations.emplace_back();
        stations.front().name = kSta;
        stations.front().sm =
            std::make_unique<StateManager>(/*core_state_size=*/5, log_manager, config);
        addCarry(kSig1);
        addCarry(kSig2);
        se.time = GnssTime{2313, 271200.0};
        raw.reset(1);
        params.gap_reentry  = true;
        params.ledger_reentry = true;
        ledger.configure(/*horizon_s=*/3900.0, kArcLedgerForgetEp);
        sid.armed = true;
    }

    void addCarry(const char* sig, double age_s = 210.0) {
        LocalCarry lc{};
        lc.value_abs   = 0.0;
        lc.var         = 1.0e-5;
        lc.last_seen_s = pl_now_s - age_s;
        local_carry[std::make_tuple(0, std::string(kSat), std::string(sig))] = lc;
    }

    /// Admit both bands for @p n_epochs, then miss them for @p gap_ep, then build
    /// the population as the response site would at the return epoch.  Returns
    /// the epoch index of the return.
    long long openGap(int n_epochs, int gap_ep,
                      const std::set<ArcLedger::Key>& returning) {
        const std::set<ArcLedger::Key> both{ K(0, kSat, kSig1), K(0, kSat, kSig2) };
        long long ep = 0;
        for (int i = 0; i < n_epochs; ++i, ++ep) admit(ledger, ep, both);
        // A station that contributes NO admitted row is frozen by the machine, so
        // the gap has to be opened with the station still present: one other arc
        // of the same station keeps it in the admitted set.
        const std::set<ArcLedger::Key> keeper{ K(0, "G01", kSig1) };
        for (int i = 0; i < gap_ep; ++i, ++ep) admit(ledger, ep, keeper);
        // The return epoch: the drain runs UPSTREAM of the admitted advance, so
        // the ledger is still open at this index and the population is a query.
        ledger.beginEpoch(ep, static_cast<double>(ep) * kDt);
        pl_now_s = static_cast<double>(ep) * kDt;
        pl_max_gap_s = 900.0;
        // Re-stamp the carries so they sit inside the window at the new epoch.
        for (auto& [key, lc] : local_carry) { (void)key; lc.last_seen_s = pl_now_s - 60.0; }
        arcLedgerReentryPopulation(ledger, returning, ep, pl_now_s, pl_max_gap_s, sid);
        return ep;
    }

    /// Push one ROW-level resumption for @ref kSat at station 0.
    void addRowResumption(bool measurable, bool shifted, int lli1, int lli2,
                          long long gap_epochs = 3) {
        GapResumption g;
        g.station    = 0;
        g.satellite  = kSat;
        g.sig1       = kSig1;
        g.sig2       = kSig2;
        g.gap_epochs = gap_epochs;
        g.measurable = measurable;
        g.shifted    = shifted;
        g.d_mw       = shifted ? -1250.98 : 0.0031;
        g.d_gf       = shifted ? -276.67 : 0.0012;
        g.lli1       = lli1;
        g.lli2       = lli2;
        raw.resumptions.push_back(std::move(g));
    }

    void run() {
        networkSlipDrainGapReentry(stations, present, /*i_lo=*/0, /*n_sta_total=*/1,
                                   /*carry_invariant=*/false, /*carry_warm=*/true,
                                   carry_slipped_ambs, raw, params, local_carry,
                                   pl_now_s, pl_max_gap_s, counters, se, traces, log,
                                   &relevel, &pends, &sid, &outcome, &ledger);
    }

    bool freed(const char* sig) const {
        return relevel.free_ambs.count({0, StateKeys::makeAmbiguityKey(kSat, sig)}) != 0;
    }
    ArcReentryOutcome outcomeOf(const char* sig) const {
        const auto it = outcome.find(K(0, kSat, sig));
        return it == outcome.end() ? ArcReentryOutcome::None : it->second;
    }
};

}  // namespace

// ===========================================================================
// R1 / R5 (a): the POPULATION
// ===========================================================================

// The population is the ADMITTED Gap-to-Reentered set: an arc that was admitted,
// then missed at least one epoch, and is admitted again now.  An arc that never
// left is not in it, and neither is one whose first admission is this epoch —
// both of those are exactly the mistakes the two earlier "answered" inferences
// made in the other direction.
TEST(LedgerReentryTest, PopulationIsTheAdmittedGapToReenteredSet) {
    ArcLedger led;
    led.configure(3900.0, kArcLedgerForgetEp);
    const ArcLedger::Key gap_arc   = K(0, kSat, kSig1);
    const ArcLedger::Key through   = K(0, kSat, kSig2);
    const ArcLedger::Key newcomer  = K(0, "G07", kSig1);

    admit(led, 0, { gap_arc, through });
    admit(led, 1, { gap_arc, through });
    admit(led, 2, { through });            // gap_arc misses one epoch
    admit(led, 3, { through });            // and a second

    const long long ep = 4;
    led.beginEpoch(ep, ep * kDt);
    ArcReentryEpoch pop;
    arcLedgerReentryPopulation(led, { gap_arc, through, newcomer }, ep, ep * kDt,
                               /*max_gap_s=*/900.0, pop);

    ASSERT_EQ(pop.arcs.size(), 1u);
    EXPECT_EQ(pop.arcs.front().station, 0);
    EXPECT_EQ(pop.arcs.front().satellite, kSat);
    EXPECT_EQ(pop.arcs.front().signal, kSig1);
    EXPECT_EQ(pop.arcs.front().gap_ep, 2);
    EXPECT_EQ(pop.outside_horizon, 0);
}

// The CARRY horizon bounds it: past `phase_leveling_max_gap_s` the PASS-B inject
// would not fold the level across the break either, so there is nothing for the
// response to protect.  Counted, not silently dropped (standing invariant 4).
TEST(LedgerReentryTest, ReturnPastTheCarryHorizonIsCountedAndLeftOut) {
    ArcLedger led;
    led.configure(3900.0, kArcLedgerForgetEp);
    const ArcLedger::Key arc    = K(0, kSat, kSig1);
    const ArcLedger::Key keeper = K(0, "G01", kSig1);

    admit(led, 0, { arc, keeper });
    for (long long e = 1; e <= 40; ++e) admit(led, e, { keeper });   // 1200 s gap

    const long long ep = 41;
    led.beginEpoch(ep, ep * kDt);
    ArcReentryEpoch pop;
    arcLedgerReentryPopulation(led, { arc, keeper }, ep, ep * kDt,
                               /*max_gap_s=*/900.0, pop);

    EXPECT_TRUE(pop.arcs.empty());
    EXPECT_EQ(pop.outside_horizon, 1);
}

// R5 (d): a per-signal return of a signal the roster does not call Modelled is
// not in the population at all.  The population is built from the MODELLED arcs,
// so the restriction is upstream of the response: the drain never sees the arc.
TEST(LedgerReentryTest, NonModelledSignalIsNotInThePopulation) {
    ArcLedger led;
    led.configure(3900.0, kArcLedgerForgetEp);
    const ArcLedger::Key modelled     = K(0, kSat, kSig1);
    const ArcLedger::Key not_modelled = K(0, kSat, "L1W");
    const ArcLedger::Key keeper       = K(0, "G01", kSig1);

    // Both signals have an admitted history and both return after a gap; only the
    // MODELLED one is offered to the population builder, which is what
    // signalRosterModelledArcs does at the call site.
    admit(led, 0, { modelled, not_modelled, keeper });
    admit(led, 1, { keeper });
    const long long ep = 2;
    led.beginEpoch(ep, ep * kDt);
    ArcReentryEpoch pop;
    arcLedgerReentryPopulation(led, { modelled, keeper }, ep, ep * kDt, 900.0, pop);

    ASSERT_EQ(pop.arcs.size(), 1u);
    EXPECT_EQ(pop.arcs.front().signal, kSig1);
}

// ===========================================================================
// R1 / R5 (b): an arc with a ROW-LEVEL verdict is freed under `shifted`
// ===========================================================================

TEST(LedgerReentryTest, ShiftedRowVerdictFreesTheLedgerArc) {
    Harness h;
    const ArcLedger::Key arc = K(0, kSat, kSig1);
    h.openGap(/*n_epochs=*/3, /*gap_ep=*/2, { arc });
    ASSERT_EQ(h.sid.arcs.size(), 1u);
    h.addRowResumption(/*measurable=*/true, /*shifted=*/true, /*lli1=*/0, /*lli2=*/0);

    h.run();

    EXPECT_TRUE(h.freed(kSig1));
    EXPECT_EQ(h.counters.ledger_reentry_arcs, 1);
    EXPECT_EQ(h.counters.ledger_reentry_with_verdict, 1);
    EXPECT_EQ(h.counters.ledger_reentry_row_tier, 1);
    EXPECT_EQ(h.counters.ledger_reentry_v_shifted, 1);
    EXPECT_EQ(h.counters.ledger_reentry_no_verdict, 0);
    EXPECT_EQ(h.counters.ledger_reentry_freed, 1);
    EXPECT_EQ(h.counters.ledger_reentry_bridged, 0);
    EXPECT_EQ(h.outcomeOf(kSig1), ArcReentryOutcome::Freed);
    // Card GRK's counter cannot be fed on this path: the response key IS the
    // ledger arc's key, so the two halves cannot name the band differently.
    EXPECT_EQ(h.counters.gap_reentry_unreachable, 0);
    // The band that tracked through is not in the population and is untouched.
    EXPECT_FALSE(h.freed(kSig2));
}

// The negative control on the same bed: the raw tier saw the return and called it
// CLEAN, so the arc is bridged by design and counted apart from the arcs nothing
// looked at.  That separation is the finding the split exists to make.
TEST(LedgerReentryTest, CleanRowVerdictBridgesAndIsCountedApart) {
    Harness h;
    const ArcLedger::Key arc = K(0, kSat, kSig1);
    h.openGap(3, 2, { arc });
    h.addRowResumption(/*measurable=*/true, /*shifted=*/false, 0, 0);

    h.run();

    EXPECT_FALSE(h.freed(kSig1));
    EXPECT_EQ(h.counters.ledger_reentry_arcs, 1);
    EXPECT_EQ(h.counters.ledger_reentry_with_verdict, 1);
    EXPECT_EQ(h.counters.ledger_reentry_v_clean, 1);
    EXPECT_EQ(h.counters.ledger_reentry_no_verdict, 0);
    EXPECT_EQ(h.counters.ledger_reentry_bridged, 1);
    EXPECT_EQ(h.counters.ledger_reentry_freed, 0);
    EXPECT_EQ(h.outcomeOf(kSig1), ArcReentryOutcome::BridgedVerdict);
}

// ===========================================================================
// R2 / R5 (c): the NO-VERDICT population and its two policies
// ===========================================================================

// `bridge` is today's behaviour made visible: the arc is left to the PASS-B
// inject and nothing is freed, but it is counted and separable.
TEST(LedgerReentryTest, NoVerdictArcIsBridgedAndCountedUnderBridge) {
    Harness h;
    const ArcLedger::Key arc = K(0, kSat, kSig1);
    h.openGap(3, 2, { arc });
    ASSERT_EQ(h.sid.arcs.size(), 1u);
    ASSERT_TRUE(h.raw.resumptions.empty());   // no raw tier saw this return
    h.sid.refloat_no_verdict = false;

    h.run();

    EXPECT_FALSE(h.freed(kSig1));
    EXPECT_EQ(h.counters.ledger_reentry_arcs, 1);
    EXPECT_EQ(h.counters.ledger_reentry_no_verdict, 1);
    EXPECT_EQ(h.counters.ledger_reentry_with_verdict, 0);
    EXPECT_EQ(h.counters.ledger_reentry_bridged, 1);
    EXPECT_EQ(h.counters.ledger_reentry_freed, 0);
    EXPECT_EQ(h.outcomeOf(kSig1), ArcReentryOutcome::BridgedNoVerdict);
}

// `refloat` treats it as UNMEASURABLE, which the existing sub-knob then decides.
// With the sub-knob ON the arc is freed.
TEST(LedgerReentryTest, NoVerdictArcIsFreedUnderRefloatWithTheUnmeasurableSubKnob) {
    Harness h;
    const ArcLedger::Key arc = K(0, kSat, kSig1);
    h.openGap(3, 2, { arc });
    h.sid.refloat_no_verdict = true;
    h.params.gap_reentry_unmeasurable = true;

    h.run();

    EXPECT_TRUE(h.freed(kSig1));
    EXPECT_EQ(h.counters.ledger_reentry_no_verdict, 1);
    EXPECT_EQ(h.counters.ledger_reentry_freed, 1);
    EXPECT_EQ(h.counters.ledger_reentry_bridged, 0);
    EXPECT_EQ(h.counters.gap_reentry_freed_unmeas, 1);
    EXPECT_EQ(h.outcomeOf(kSig1), ArcReentryOutcome::Freed);
}

// …and with the sub-knob at its loader DEFAULT (off) the policy is counted and
// INERT.  Pinned deliberately: an operator who arms `refloat` alone on the
// working posture gets the `bridge` behaviour, and the run-end line says so.
TEST(LedgerReentryTest, RefloatIsInertWhileTheUnmeasurableSubKnobIsOff) {
    Harness h;
    const ArcLedger::Key arc = K(0, kSat, kSig1);
    h.openGap(3, 2, { arc });
    h.sid.refloat_no_verdict = true;
    ASSERT_FALSE(h.params.gap_reentry_unmeasurable);   // the loader default

    h.run();

    EXPECT_FALSE(h.freed(kSig1));
    EXPECT_EQ(h.counters.ledger_reentry_no_verdict, 1);
    EXPECT_EQ(h.counters.ledger_reentry_bridged, 1);
    EXPECT_EQ(h.counters.ledger_reentry_freed, 0);
}

// The dispositions partition the population exactly: that closure is what makes a
// zero in any one of them readable (standing invariant 4).
TEST(LedgerReentryTest, DispositionsPartitionThePopulation) {
    Harness h;
    h.openGap(3, 2, { K(0, kSat, kSig1), K(0, kSat, kSig2) });
    ASSERT_EQ(h.sid.arcs.size(), 2u);
    h.addRowResumption(/*measurable=*/true, /*shifted=*/true, 0, 0);

    h.run();

    const long long disp = h.counters.ledger_reentry_freed
                         + h.counters.ledger_reentry_bridged
                         + h.counters.ledger_reentry_no_carry
                         + h.counters.ledger_reentry_pivot_skipped;
    EXPECT_EQ(disp, h.counters.ledger_reentry_arcs);
    EXPECT_EQ(h.counters.ledger_reentry_with_verdict
              + h.counters.ledger_reentry_no_verdict,
              h.counters.ledger_reentry_arcs);
}

// ===========================================================================
// R3: the prune and expiry edges
// ===========================================================================

// R5 (e): the horizon prune is a DEATH edge with a reason.  The ledger reports it
// on the admitted side exactly as it does on the raw one, so the carry's own gap
// prune has an edge to be reported against.
TEST(LedgerReentryTest, HorizonPruneEmitsArcDeathOnTheAdmittedSide) {
    ArcLedger led;
    led.configure(/*horizon_s=*/300.0, kArcLedgerForgetEp);   // 10 epochs at 30 s
    const ArcLedger::Key arc    = K(0, kSat, kSig1);
    const ArcLedger::Key keeper = K(0, "G01", kSig1);

    admit(led, 0, { arc, keeper });
    bool saw_death = false;
    int  death_gap = 0;
    for (long long e = 1; e <= 20; ++e) {
        led.beginEpoch(e, static_cast<double>(e) * kDt);
        std::vector<std::pair<ArcLedger::Key, ArcStep>> edges;
        led.admitEpoch({ keeper }, edges);
        for (const auto& [key, step] : edges)
            if (key == arc && step.transition == ArcTransition::Death) {
                saw_death = true;
                death_gap = step.gap_len_ep;
                EXPECT_NE(step.reason_mask & kArcReasonHorizon, 0u);
                EXPECT_EQ(std::string(arcStateName(step.to)), "dead");
            }
    }
    EXPECT_TRUE(saw_death);
    EXPECT_GT(death_gap, 10);
}

// R3(b): the state-vector prune is a MARK, not a transition, and the arc's next
// admitted re-entry carries it.  The prune itself is untouched; this only records
// that the column the return lands on is a fresh one.
TEST(LedgerReentryTest, StatePruneMarkTravelsOnTheNextReentry) {
    ArcLedger led;
    led.configure(3900.0, kArcLedgerForgetEp);
    const ArcLedger::Key arc    = K(0, kSat, kSig1);
    const ArcLedger::Key keeper = K(0, "G01", kSig1);

    admit(led, 0, { arc, keeper });
    led.beginEpoch(1, kDt);
    std::vector<std::pair<ArcLedger::Key, ArcStep>> edges;
    led.admitEpoch({ keeper }, edges);                       // the arc gaps
    EXPECT_TRUE(led.markStatePruned(arc));
    ASSERT_NE(led.find(arc), nullptr);
    EXPECT_TRUE(led.find(arc)->state_pruned);

    // The population query reports it, so the response can state it per arc.
    led.beginEpoch(2, 2 * kDt);
    ArcReentryEpoch pop;
    arcLedgerReentryPopulation(led, { arc, keeper }, 2, 2 * kDt, 900.0, pop);
    ASSERT_EQ(pop.arcs.size(), 1u);
    EXPECT_TRUE(pop.arcs.front().state_recreated);

    // …and the admitted edge carries it once, then clears it: the mark describes
    // ONE gap and must not leak into the next.
    std::vector<std::pair<ArcLedger::Key, ArcStep>> edges2;
    led.admitEpoch({ arc, keeper }, edges2);
    bool saw = false;
    for (const auto& [key, step] : edges2)
        if (key == arc && step.transition == ArcTransition::Reentry) {
            saw = true;
            EXPECT_TRUE(step.state_recreated);
        }
    EXPECT_TRUE(saw);
    EXPECT_FALSE(led.find(arc)->state_pruned);
}

// An unknown key is not marked: a key the ledger has never seen is not an arc it
// can say anything about, and inventing a record would manufacture a population.
TEST(LedgerReentryTest, StatePruneMarkOnAnUnknownArcIsRefused) {
    ArcLedger led;
    led.configure(3900.0, kArcLedgerForgetEp);
    led.beginEpoch(0, 0.0);
    EXPECT_FALSE(led.markStatePruned(K(3, "G31", "L5Q")));
    EXPECT_EQ(led.size(), 0u);
}

// ===========================================================================
// R6: the OFF path
// ===========================================================================

// Unarmed, the drain takes its arc list from the raw tier exactly as before and
// every phase-3 counter stays zero.  The outcome sink is still written, because
// the unified census has to be comparable across the flip.
TEST(LedgerReentryTest, UnarmedTheRawTierStillDrivesAndPhase3CountersStayZero) {
    Harness h;
    h.openGap(3, 2, { K(0, kSat, kSig1) });
    h.sid.armed = false;                     // network.ledger_reentry off
    h.params.ledger_reentry = false;
    h.addRowResumption(/*measurable=*/true, /*shifted=*/true, 0, 0);

    h.run();

    EXPECT_TRUE(h.freed(kSig1));
    EXPECT_TRUE(h.freed(kSig2));             // the raw tier frees BOTH bands
    EXPECT_EQ(h.counters.ledger_reentry_arcs, 0);
    EXPECT_EQ(h.counters.ledger_reentry_no_verdict, 0);
    EXPECT_EQ(h.counters.gap_reentry_freed_shifted, 2);
    EXPECT_EQ(h.outcomeOf(kSig1), ArcReentryOutcome::Freed);
    EXPECT_EQ(h.outcomeOf(kSig2), ArcReentryOutcome::Freed);
}

}  // namespace GnssProcessor
