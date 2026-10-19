/**
 * @file test_windup_restart_targets.cpp
 * @brief @c networkWindupRestartTargets, the arcs a wind-up accumulator
 *        restart steps (docs/refound/WUP_windup_accumulated_promotion_2026-10-01.md).
 *
 * ## Why this exists
 *
 * A restart is a return after a gap longer than the wind-up horizon (300 s), so the
 * arc has usually left the model set: its ambiguity column was pruned with the gap,
 * and PASS B re-creates it only after the slip drains.  The response walked the live
 * ambiguity map alone and so matched nothing on the 6 h field arm (28 of 28 in-horizon
 * restarts), while the PASS-B inject folded the leveling carry across the step.  The
 * helper under test resolves the target the way the gap re-entry drain does: the live
 * columns when they exist, else the leveling carry entries inside the carry horizon on
 * the synthesised key the inject reads.
 *
 * The drain itself needs a model whose wind-up ledger holds a restart, so it is
 * graded end to end on the field arm's run-end counters; this
 * file pins the decision it was missing.
 */

#include <gtest/gtest.h>

#include "network/assembly/SignalRoster.hpp"
#include "config/GnssConfig.hpp"
#include "network/state/Entities.hpp"
#include "network/state/ArcLedger.hpp"
#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"
#include "network/slip/SlipQc.hpp"

#include <map>
#include <set>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace GnssProcessor {
namespace {

constexpr const char* kSat  = "G22";
constexpr const char* kSig1 = "L1C";
constexpr const char* kSig2 = "L2W";

using CarryMap = net::ArcTable;

struct Harness {
    LogManager                    log_manager;
    GnssConfig                    config;
    std::unique_ptr<StateManager> sm;
    StationParams                 params;   // the set the targets read, rebuilt by addLive
    CarryMap                      carry;
    double                        now_s{271200.0};
    double                        max_gap_s{900.0};

    Harness() { sm = std::make_unique<StateManager>(/*core_state_size=*/5, log_manager, config); }

    void addLive(const char* sat, const char* sig) {
        sm->addAmbiguity(StateKeys::makeAmbiguityKey(sat, sig), 0.0);
        buildStationParams(*sm, {}, StationAbsorbed{}, params);
    }
    void addCarry(int station, const char* sat, const char* sig, double age_s) {
        LocalCarry lc{};
        lc.value_abs   = 0.0;
        lc.var         = 1.0e-5;
        lc.last_seen_s = now_s - age_s;
        net::putArcIn(carry, std::make_tuple(station, std::string(sat), std::string(sig)), &net::ArcState::amb) = lc;
    }
    std::vector<std::pair<std::string, std::string>> targets(const CarryMap* c, bool* via) const {
        return networkWindupRestartTargets(params, 0, kSat, c, now_s, max_gap_s, via);
    }
};

std::vector<std::pair<std::string, std::string>> keys(std::initializer_list<const char*> sigs) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const char* s : sigs) out.emplace_back(StateKeys::makeAmbiguityKey(kSat, s), s);
    return out;
}

// The arc kept its columns: they are the target, as before the change, and the carry
// is not consulted even when it holds entries.
TEST(WindupRestartTargets, LiveColumnsAreTheTargetWhenPresent) {
    Harness h;
    h.addLive(kSat, kSig1);
    h.addLive(kSat, kSig2);
    h.addLive("G05", kSig1);                  // another satellite: not a target
    h.addCarry(0, kSat, kSig1, 420.0);
    h.addCarry(0, kSat, kSig2, 420.0);
    bool via = true;
    EXPECT_EQ(h.targets(&h.carry, &via), keys({ kSig1, kSig2 }));
    EXPECT_FALSE(via);
}

// The field case: the column was pruned with the gap, the carry survived inside the
// horizon, and both bands are reached on the synthesised key.
TEST(WindupRestartTargets, ReenteringArcIsReachedThroughTheCarry) {
    Harness h;
    h.addCarry(0, kSat, kSig1, 420.0);
    h.addCarry(0, kSat, kSig2, 420.0);
    bool via = false;
    EXPECT_EQ(h.targets(&h.carry, &via), keys({ kSig1, kSig2 }));
    EXPECT_TRUE(via);
}

// Without the carry map the helper reaches live columns only: the behaviour that
// matched nothing on the field arm.
TEST(WindupRestartTargets, WithoutTheCarryAReenteringArcIsUnreached) {
    Harness h;
    h.addCarry(0, kSat, kSig1, 420.0);
    bool via = true;
    EXPECT_TRUE(h.targets(nullptr, &via).empty());
    EXPECT_FALSE(via);
}

// The horizon bounds the population: an entry older than the carry horizon is what
// the inject prunes, and one stamped in the future is not a carry of this epoch.
TEST(WindupRestartTargets, CarryOutsideTheHorizonIsNotATarget) {
    Harness h;
    h.addCarry(0, kSat, kSig1, 900.0);        // on the edge: inside
    h.addCarry(0, kSat, kSig2, 930.0);        // one epoch past: outside
    h.addCarry(0, kSat, "L1W", -30.0);        // future-stamped: outside
    bool via = false;
    EXPECT_EQ(h.targets(&h.carry, &via), keys({ kSig1 }));
    EXPECT_TRUE(via);
}

// Only this (station, satellite) group is walked.
TEST(WindupRestartTargets, OtherStationsAndSatellitesAreNotTargets) {
    Harness h;
    h.addCarry(1, kSat, kSig1, 420.0);        // another station
    h.addCarry(0, "G21", kSig1, 420.0);       // neighbours in the key order
    h.addCarry(0, "G23", kSig2, 420.0);
    bool via = true;
    EXPECT_TRUE(h.targets(&h.carry, &via).empty());
    EXPECT_FALSE(via);
    h.addCarry(0, kSat, kSig2, 60.0);
    EXPECT_EQ(h.targets(&h.carry, &via), keys({ kSig2 }));
    EXPECT_TRUE(via);
}

// With the arc ledger given, the ledger rules on each carried arc.
// An admitted arc that is dead is not a target; a carried arc the ledger holds no
// admission for (one admitted before a warm resume, which restores the carry and not
// the ledger) is named by the carry and counted as the site's fallback.
TEST(WindupRestartTargets, LedgerNamesTheCarriedArcsAndTheCarryAnswersBeforeIt) {
    Harness h;
    h.addCarry(0, kSat, kSig1, 420.0);
    h.addCarry(0, kSat, kSig2, 420.0);
    net::NetworkEntities ents;
    ents.arcs = h.carry;   // one arc table: the carry and the ledger's records (step N4f2)
    ArcLedger ledger{ ents };
    ledger.configure(/*horizon_s=*/3600.0, /*forget_ep=*/0);
    ledger.beginEpoch(0, h.now_s);
    const ArcLedger::Key k1{ 0, kSat, kSig1 };
    ledger.observe(k1, true);
    std::vector<std::pair<ArcLedger::Key, ArcStep>> edges;
    ledger.admitEpoch(std::set<ArcLedger::Key>{ k1 }, edges);
    RosterN3bCensus census;
    const auto site = static_cast<std::size_t>(RosterN3bSite::WindupRestartArcs);
    bool via = false;
    // L1C admitted in the ledger; L2W carried with no ledger record: both named.
    EXPECT_EQ(networkWindupRestartTargets(h.params, 0, kSat, &ents.arcs, h.now_s, h.max_gap_s,
                                          &via, &ledger, &census),
              keys({ kSig1, kSig2 }));
    EXPECT_TRUE(via);
    EXPECT_EQ(census.count[site].compared, 1);
    EXPECT_EQ(census.count[site].disagree, 0);
    EXPECT_EQ(census.fallback[site], 1);
    // Both admitted: the ledger answers alone, no fallback.
    const ArcLedger::Key k2{ 0, kSat, kSig2 };
    ledger.beginEpoch(1, h.now_s + 30.0);
    ledger.observe(k1, true);
    ledger.observe(k2, true);
    ledger.admitEpoch(std::set<ArcLedger::Key>{ k1, k2 }, edges);
    EXPECT_EQ(networkWindupRestartTargets(h.params, 0, kSat, &ents.arcs, h.now_s, h.max_gap_s,
                                          &via, &ledger, &census),
              keys({ kSig1, kSig2 }));
    EXPECT_EQ(census.count[site].compared, 2);
    EXPECT_EQ(census.fallback[site], 1);
}

}  // namespace
}  // namespace GnssProcessor
