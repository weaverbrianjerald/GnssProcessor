/**
 * @file test_phase_held_basis.cpp
 * @brief Contract tests for RF-2 — the phase datum as a HELD S-BASIS
 *        (@c network.phase_datum_held_basis, @c phase_anchor_min_age_ep,
 *        @c phase_anchor_flag_quiet_ep).
 *
 * Card: docs/refound/RF-1_steps345_mechanism_2026-09-15.md, last section.
 *
 * WHY, in the sentence the cases are shaped around.  RF-1 made every network ambiguity
 * a float state with a covariance — and with it, accidentally, the ONE ambiguity that
 * realizes each satellite's phase-bias datum.  Measured on the 6 h field arm: that datum
 * converged, was dropped, and was re-flagged by T4 **454 times an hour**, and every one
 * of those moves is a phase-bias re-realization the consumer must re-converge through
 * (D_phase 2.9–26× worse than RF-0 while the internal instruments improved by two orders
 * of magnitude).  The level vehicle's frozen zero-q anchor had been supplying the held
 * property BY ACCIDENT; `float_kf` removed the accident.  The textbook makes the S-basis
 * ambiguities CONSTANTS, not states — that is this card.
 *
 * The four items, and what a unit test can reach of each:
 *  1. **held basis** — the value is FROZEN at election and re-pinned unchanged for the
 *     whole tenure.  Reachable: the loader, the ledger entry's semantics, and the
 *     held column's exemption from the float update (@ref networkIsGaugePinned_'s
 *     predicate, which the inject and the harvest share);
 *  2. **every handover declared** — Δ = new basis − outgoing basis, and the satellite
 *     reaches the consumer-facing retro-bump queue.  Reachable: the Δ arithmetic and
 *     the queue, on the same structures the driver mutates;
 *  3. **election** — highest elevation among arcs older than @c min_age and unflagged
 *     within @c flag_quiet_ep.  Reachable: the two gates' arithmetic, including the
 *     RESTORED-ARC convention they inherit from @ref localCarryYoung;
 *  4. **T4 on the anchor** — a flag deposes the basis holder BEFORE the response acts.
 *     Reachable: the flag map's window predicate, which is the same one item 3 uses.
 *
 * What a unit test CANNOT reach is the live election over a registered estimator with a
 * real candidate census; that is what the 6 h arms on both beds are for.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "config/GnssConfigTypes.hpp"
#include "filter/NetworkPhaseDatum.hpp"
#include "filter/StateKeySchema.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "logging/LogManager.hpp"

#include <cmath>
#include <filesystem>
#include <limits>
#include <vector>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <tuple>

using GnssProcessor::GnssConfig;
using GnssProcessor::LocalCarry;
using GnssProcessor::localCarryYoung;
using GnssProcessor::LogManager;
using GnssProcessor::NetworkAmbiguityVehicle;
using GnssProcessor::PhaseBasisEntry;
using GnssProcessor::PhaseHeldBasis;

namespace fs = std::filesystem;

namespace {

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("heldbasis_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

std::string networkConfig(const std::string& extra) {
    return std::string("{\n")
        + "  \"processing\": {\n"
          "    \"mode\": \"ppp-rtk-network\",\n"
          "    \"active_systems\": [ \"GPS\" ],\n"
          "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
          "  },\n"
          "  \"network\": {\n"
          "    \"clock_datum_station\": \"AAAA\",\n"
        + extra +
          "    \"stations\": [\n"
          "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
          "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
          "    ]\n"
          "  }\n"
          "}\n";
}

/// The RF-1 vehicle the held basis lives on, with the prioritized election it holds.
const char* kFloatKfPosture =
    "    \"phase_leveling\": true,\n"
    "    \"carry_invariant\": false,\n"
    "    \"carry_conditional_coupling\": true,\n"
    "    \"shared_carry_full_cov\": true,\n"
    "    \"phase_datum_prioritized\": true,\n"
    "    \"ambiguity_vehicle\": \"float_kf\",\n";

bool loadInto(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = writeTempConfig(tag, body);
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

/// The election's own eligibility rule, in the form both readers apply it (item 3's
/// candidate screen and item 4's incumbent-depose test): the arc must be MATURE and must
/// not have been flagged inside the quiet window.
bool basisEligible(const PhaseHeldBasis& h, int sta, const std::string& amb,
                   const std::string& sat, const std::string& sig) {
    if (h.min_age_ep > 0 && h.carry) {
        const auto it = h.carry->find(std::make_tuple(sta, sat, sig));
        if (it == h.carry->end() || localCarryYoung(it->second, h.now_ep, h.min_age_ep))
            return false;
    }
    if (h.flag_quiet_ep > 0 && h.flag_ep) {
        const auto ft = h.flag_ep->find({sta, amb});
        if (ft != h.flag_ep->end() && h.now_ep - ft->second < h.flag_quiet_ep) return false;
    }
    return true;
}

}  // namespace

// =============================================================================
// (1) THE KNOB AND ITS INTERLOCKS.
// =============================================================================

TEST(PhaseHeldBasisTest, DefaultsOffWithTheCardsGateValues) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("default", networkConfig(kFloatKfPosture), cfg));
    EXPECT_FALSE(cfg.networkPhaseDatumHeldBasis());
    EXPECT_EQ(cfg.networkPhaseAnchorMinAgeEp(), 10);
    EXPECT_EQ(cfg.networkPhaseAnchorFlagQuietEp(), 5);
}

TEST(PhaseHeldBasisTest, ArmsUnderFloatKfWithThePrioritizedElection) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("on", networkConfig(std::string(kFloatKfPosture)
                                             + "    \"phase_datum_held_basis\": true,\n"
                                               "    \"phase_anchor_min_age_ep\": 12,\n"
                                               "    \"phase_anchor_flag_quiet_ep\": 7,\n"),
                         cfg));
    EXPECT_TRUE(cfg.networkPhaseDatumHeldBasis());
    EXPECT_EQ(cfg.networkPhaseAnchorMinAgeEp(), 12);
    EXPECT_EQ(cfg.networkPhaseAnchorFlagQuietEp(), 7);
}

// On the "level" vehicle the zero-q anchor is ALREADY an (accidental) held basis, so
// arming this there would change nothing it claims to change — a hard error, not a warn,
// because an arm that cannot act reads exactly like a clean one (invariant #4).
TEST(PhaseHeldBasisTest, RefusedOnTheLevelVehicle) {
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("lvl",
                          networkConfig("    \"phase_leveling\": true,\n"
                                        "    \"carry_invariant\": false,\n"
                                        "    \"carry_conditional_coupling\": true,\n"
                                        "    \"shared_carry_full_cov\": true,\n"
                                        "    \"phase_datum_prioritized\": true,\n"
                                        "    \"phase_datum_held_basis\": true,\n"),
                          cfg));
}

// …and without the per-(satellite, signal) election there is no datum to hold: the
// legacy single-pivot realization pins every freedom at the pivot.
TEST(PhaseHeldBasisTest, RefusedWithoutThePrioritizedElection) {
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("noprio",
                          networkConfig("    \"phase_leveling\": true,\n"
                                        "    \"carry_invariant\": false,\n"
                                        "    \"carry_conditional_coupling\": true,\n"
                                        "    \"shared_carry_full_cov\": true,\n"
                                        "    \"ambiguity_vehicle\": \"float_kf\",\n"
                                        "    \"phase_datum_held_basis\": true,\n"),
                          cfg));
}

TEST(PhaseHeldBasisTest, NegativeGateValuesAreRejected) {
    GnssConfig a, b;
    EXPECT_FALSE(loadInto("negage",
                          networkConfig(std::string(kFloatKfPosture)
                                        + "    \"phase_anchor_min_age_ep\": -1,\n"), a));
    EXPECT_FALSE(loadInto("negq",
                          networkConfig(std::string(kFloatKfPosture)
                                        + "    \"phase_anchor_flag_quiet_ep\": -1,\n"), b));
}

// =============================================================================
// (2) ITEM 1 — THE HELD COLUMN.  Its variance does not change under the fold.
// =============================================================================

// The load-bearing line of the S-basis contract, and the one the card's first gate
// names: a gauge-pinned column is excluded from the carry at BOTH the inject and the
// harvest, on the SAME predicate, so the held basis is never folded as a prior and
// never harvested as a posterior — its variance is not a thing the filter updates.
// (@c ApplicationNetworkAssembly's networkIsGaugePinned_ is that predicate; the test
// pins the property it guarantees, on the map it reads.)
TEST(PhaseHeldBasisTest, AHeldColumnIsExcludedFromTheFloatUpdateOnOnePredicate) {
    GnssProcessor::NetStation st;
    st.name = "AAAA";
    st.pinned_amb_targets["G07_L1C"] = -12.5;      // the basis: held at its election value
    // The inject's exclusion and the harvest's are the same map lookup — that identity
    // is what stops a pinned column ever getting a prior that fights its own pin.
    EXPECT_EQ(st.pinned_amb_targets.count("G07_L1C"), 1u);
    EXPECT_EQ(st.pinned_amb_targets.count("G07_L2W"), 0u);
    EXPECT_DOUBLE_EQ(st.pinned_amb_targets.at("G07_L1C"), -12.5);
}

// The held value is FROZEN at election: a retained incumbent re-pins at the ledger's
// number, not at anything re-derived from a carry that has moved since.
TEST(PhaseHeldBasisTest, ARetainedIncumbentRePinsAtTheFrozenValue) {
    std::map<std::string, PhaseBasisEntry> basis;
    PhaseBasisEntry e;
    e.station = "AAAA"; e.value = -12.5; e.elected_ep = 100; e.cold = false;
    basis["G07_L1C"] = e;

    PhaseHeldBasis h;
    h.on = true; h.basis = &basis; h.now_ep = 137;
    // 37 epochs later the carry has converged somewhere else entirely; the basis has not.
    const auto be = h.basis->find("G07_L1C");
    ASSERT_NE(be, h.basis->end());
    EXPECT_EQ(be->second.station, "AAAA");
    EXPECT_DOUBLE_EQ(be->second.value, -12.5);
    EXPECT_EQ(h.now_ep - be->second.elected_ep, 37);   // the tenure clock
}

// =============================================================================
// (3) ITEM 2 — EVERY HANDOVER DECLARED, continuous and cold.
// =============================================================================

// A CONTINUOUS handover inherits the outgoing frame, so Δ ≈ 0 and the family does not
// step; a COLD one pins the successor at 0 against an outgoing basis of prev_value, so
// the emitted phase bias steps by EXACTLY that difference.  Both are declared.
TEST(PhaseHeldBasisTest, DeclaredHandoverDeltaIsTheBasisDifference) {
    PhaseBasisEntry cont;
    cont.station = "BBBB"; cont.value = -12.5; cont.prev_station = "AAAA";
    cont.prev_value = -12.5; cont.cold = false;
    EXPECT_DOUBLE_EQ(cont.value - cont.prev_value, 0.0);

    PhaseBasisEntry cold;
    cold.station = "BBBB"; cold.value = 0.0; cold.prev_station = "AAAA";
    cold.prev_value = -12.5; cold.cold = true;
    EXPECT_DOUBLE_EQ(cold.value - cold.prev_value, 12.5);   // the wire steps by exactly this
}

// The declaration reaches the consumer through the retro-bump queue NetworkSolPrep
// drains into markSatDiscontinuity — with MARK-==-LAND semantics, so a satellite
// withheld this epoch stays pending and is marked at its next ACTUAL emission, which is
// the epoch the consumer needs the reset.
TEST(PhaseHeldBasisTest, DeclaringAHandoverQueuesTheSatelliteForTheConsumer) {
    std::set<std::string> retro;
    for (const std::string& amb : {"G07_L1C", "G07_L2W", "G13_L1C"}) {
        const auto pk = GnssProcessor::StateKeys::parseAmbiguityKey(amb);
        ASSERT_TRUE(pk.has_value());
        retro.insert(pk->satellite);
    }
    // Per SATELLITE, not per family: two families of one satellite are one consumer
    // reset, and the set collapses them.
    EXPECT_EQ(retro.size(), 2u);
    EXPECT_EQ(retro.count("G07"), 1u);
    EXPECT_EQ(retro.count("G13"), 1u);
}

// =============================================================================
// (4) ITEM 3 — THE ELECTION REFUSES NEWBORN AND FLAGGED CANDIDATES.
// =============================================================================

TEST(PhaseHeldBasisTest, ElectionRefusesANewbornArc) {
    std::map<std::tuple<int, std::string, std::string>, LocalCarry> carry;
    LocalCarry young{0.0, 1e-3, 0.0};  young.birth_ep = 95;   // 5 epochs old
    LocalCarry mature{0.0, 1e-3, 0.0}; mature.birth_ep = 40;  // 60 epochs old
    carry[{0, "G07", "L1C"}] = young;
    carry[{1, "G07", "L1C"}] = mature;

    PhaseHeldBasis h;
    h.on = true; h.min_age_ep = 10; h.now_ep = 100; h.carry = &carry;
    EXPECT_FALSE(basisEligible(h, 0, "G07_L1C", "G07", "L1C"));
    EXPECT_TRUE (basisEligible(h, 1, "G07_L1C", "G07", "L1C"));
}

// "Never a newborn low arc" includes the arc that is not carried AT ALL — a station
// tracking the satellite for the first time this epoch has no age to qualify on.
TEST(PhaseHeldBasisTest, ElectionRefusesAnArcWithNoCarry) {
    std::map<std::tuple<int, std::string, std::string>, LocalCarry> carry;
    PhaseHeldBasis h;
    h.on = true; h.min_age_ep = 10; h.now_ep = 100; h.carry = &carry;
    EXPECT_FALSE(basisEligible(h, 3, "G07_L1C", "G07", "L1C"));
}

// The RESTORED-ARC convention rides along unchanged: a warm-resume-restored entry has
// no usable age and reads MATURE rather than being refused the basis for min_age epochs
// after every resume (SPEC_event_harness §15.4 — the defect that re-branded a whole
// network of restored arcs as newborns).
TEST(PhaseHeldBasisTest, ARestoredArcIsNotTreatedAsNewborn) {
    std::map<std::tuple<int, std::string, std::string>, LocalCarry> carry;
    carry[{0, "G07", "L1C"}] =
        GnssProcessor::makeRestoredLocalCarry(0.0, 1e-3, 0.0, 0.5);
    PhaseHeldBasis h;
    h.on = true; h.min_age_ep = 10; h.now_ep = 3; h.carry = &carry;
    EXPECT_TRUE(basisEligible(h, 0, "G07_L1C", "G07", "L1C"));
}

TEST(PhaseHeldBasisTest, ElectionRefusesARecentlyFlaggedArc) {
    std::map<std::tuple<int, std::string, std::string>, LocalCarry> carry;
    LocalCarry old{0.0, 1e-3, 0.0}; old.birth_ep = 10;
    carry[{0, "G07", "L1C"}] = old;
    carry[{1, "G07", "L1C"}] = old;
    std::map<std::pair<int, std::string>, long long> flags;
    flags[{0, "G07_L1C"}] = 97;    // 3 epochs ago — inside the 5-epoch quiet window
    flags[{1, "G07_L1C"}] = 90;    // 10 epochs ago — outside it

    PhaseHeldBasis h;
    h.on = true; h.min_age_ep = 10; h.flag_quiet_ep = 5; h.now_ep = 100;
    h.carry = &carry; h.flag_ep = &flags;
    EXPECT_FALSE(basisEligible(h, 0, "G07_L1C", "G07", "L1C"));
    EXPECT_TRUE (basisEligible(h, 1, "G07_L1C", "G07", "L1C"));
    // The window is half-open: a flag exactly flag_quiet_ep epochs ago has aged out.
    flags[{1, "G07_L1C"}] = 95;
    EXPECT_TRUE(basisEligible(h, 1, "G07_L1C", "G07", "L1C"));
    flags[{1, "G07_L1C"}] = 96;
    EXPECT_FALSE(basisEligible(h, 1, "G07_L1C", "G07", "L1C"));
}

// Both gates OFF (0) restore the pre-card behaviour exactly — the knob values are the
// card's, not the code's, and a 0 must never mean "refuse everything".
TEST(PhaseHeldBasisTest, ZeroGatesRefuseNothing) {
    std::map<std::tuple<int, std::string, std::string>, LocalCarry> carry;
    std::map<std::pair<int, std::string>, long long> flags;
    flags[{0, "G07_L1C"}] = 100;
    PhaseHeldBasis h;
    h.on = true; h.min_age_ep = 0; h.flag_quiet_ep = 0; h.now_ep = 100;
    h.carry = &carry; h.flag_ep = &flags;
    EXPECT_TRUE(basisEligible(h, 0, "G07_L1C", "G07", "L1C"));
}

// =============================================================================
// (5) ITEM 4 — A FLAG ON THE BASIS DEPOSES IT, BEFORE THE RESPONSE ACTS.
// =============================================================================

// The incumbent is sticky only WHILE IT QUALIFIES.  The election runs before PASS B and
// before any slip / T4 response touches that column, so a flag on the basis ambiguity
// hands the datum over FIRST and the response then applies to the ex-anchor as an
// ordinary float state — instead of re-realizing the datum in place, which is the churn
// the card measured at 454 T4 hits an hour on the field.
TEST(PhaseHeldBasisTest, AFlagOnTheBasisHolderDeposesIt) {
    std::map<std::tuple<int, std::string, std::string>, LocalCarry> carry;
    LocalCarry old{0.0, 1e-3, 0.0}; old.birth_ep = 10;
    carry[{0, "G07", "L1C"}] = old;     // the incumbent basis holder
    carry[{1, "G07", "L1C"}] = old;     // a qualifying successor
    std::map<std::pair<int, std::string>, long long> flags;

    PhaseHeldBasis h;
    h.on = true; h.min_age_ep = 10; h.flag_quiet_ep = 5; h.now_ep = 100;
    h.carry = &carry; h.flag_ep = &flags;
    // Quiet: the incumbent qualifies and is kept.
    EXPECT_TRUE(basisEligible(h, 0, "G07_L1C", "G07", "L1C"));
    // T4 flags it THIS epoch — the stamp lands at the flag, not at the response.
    flags[{0, "G07_L1C"}] = 100;
    EXPECT_FALSE(basisEligible(h, 0, "G07_L1C", "G07", "L1C"));
    EXPECT_TRUE (basisEligible(h, 1, "G07_L1C", "G07", "L1C"));   // …and a successor exists
}

// =============================================================================
// (6) RF-2b — THE COMPOSED RANK: elevation among LEDGER-USABLE candidates, with
//     elevation-only as the fallback, and the incumbent deposed only by its OWN
//     basis column.
//
// ⚠ THE DEFECT THIS CLOSES, measured.  RF-2's rank asked for AGE and QUIET but never
// whether the candidate's carried level is usable AS A DATUM by the continuous branch's
// own ledgerVerdict (var <= network.phase_datum_handover_max_var_m2, 0.01 on both RF-0
// bases).  On the FIELD most mature high-elevation arcs also pass that ceiling, so 89 %
// of handovers came out continuous and the arm's phase wire crossed every bar.  On the
// SIM, whose ambiguity scale is ~5x larger, they mostly do not: 1 049 of 1 562 elections
// (67 %) went COLD, each re-realized the datum at 0, sigma0 blew past the withhold
// ceiling, and 362 of 720 epochs were withheld with the WL integers falling 99.0 % to
// 35.2 %.  A rank that elects a station the continuous branch then REFUSES is the defect
// with extra steps — so the two must be ONE predicate, which is what these cases pin.
// =============================================================================

namespace {

/// The continuous branch's datum-usability verdict, in the form the composed rank asks
/// it: a finite value at a positive, finite variance at or under the absolute ceiling.
/// (The ratio ceiling is unarmed on both RF-0 bases and is not exercised here.)
bool ledgerUsable(bool has_entry, double value, double var, double max_var) {
    return has_entry && std::isfinite(value) && var > 0.0 && std::isfinite(var)
           && var <= max_var;
}

struct Cand { int sta; double elev; bool has_entry; double value; double var; };
struct RankResult { int pick = -1; bool fallback = false; int n_refused = 0; };

/// The composed rank: highest elevation among LEDGER-USABLE candidates; when none
/// qualifies, highest elevation overall — counted by the caller as a COLD pin.
RankResult composedRank(const std::vector<Cand>& cands, double max_var) {
    RankResult r;
    int best = -1, best_any = -1;
    double best_el = -2.0, best_any_el = -2.0;
    for (const Cand& c : cands) {
        if (c.elev > best_any_el) { best_any_el = c.elev; best_any = c.sta; }
        if (!ledgerUsable(c.has_entry, c.value, c.var, max_var)) { ++r.n_refused; continue; }
        if (c.elev > best_el) { best_el = c.elev; best = c.sta; }
    }
    if (best < 0 && best_any >= 0) { best = best_any; r.fallback = true; }
    r.pick = best;
    return r;
}

}  // namespace

// The composition itself: the HIGHEST-elevation candidate is passed over when its
// carried level is not a usable datum, and the rank takes the best one that is.
TEST(PhaseHeldBasisTest, ComposedRankPrefersElevationAmongLedgerUsableCandidates) {
    const std::vector<Cand> cands = {
        {0, 78.0, true, 3.0, 5.000},   // highest elevation, var 5 m² — NOT a datum
        {1, 55.0, true, 3.0, 0.004},   // usable
        {2, 41.0, true, 3.0, 0.001},   // usable, lower
    };
    const RankResult r = composedRank(cands, /*max_var=*/0.01);
    EXPECT_EQ(r.pick, 1);            // the best USABLE one, not the best one
    EXPECT_FALSE(r.fallback);
    EXPECT_EQ(r.n_refused, 1);
}

// With everything usable the rank is pure elevation — the composition must NOT change
// the field's behaviour, where 89 % of handovers already came out continuous.
TEST(PhaseHeldBasisTest, ComposedRankIsPureElevationWhenEveryCandidateIsUsable) {
    const std::vector<Cand> cands = {
        {0, 41.0, true, 3.0, 0.001},
        {1, 78.0, true, 3.0, 0.004},
        {2, 55.0, true, 3.0, 0.002},
    };
    const RankResult r = composedRank(cands, 0.01);
    EXPECT_EQ(r.pick, 1);
    EXPECT_FALSE(r.fallback);
    EXPECT_EQ(r.n_refused, 0);
}

// THE FALLBACK.  A family with no usable candidate must still be PINNED — withholding it
// is strictly worse than a cold pin — and the fallback is COUNTED, because a cold pin is
// a different event from a clean handover.  This is the sim's whole failure mode, now
// visible on a run-end counter instead of only in the event CSV.
TEST(PhaseHeldBasisTest, ComposedRankFallsBackToElevationOnlyAndCountsIt) {
    const std::vector<Cand> cands = {
        {0, 41.0, true,  3.0, 5.0},    // var over the ceiling
        {1, 78.0, false, 0.0, 0.0},    // no ledger entry at all
        {2, 55.0, true,  3.0, 9.0},    // var over the ceiling
    };
    const RankResult r = composedRank(cands, 0.01);
    EXPECT_EQ(r.pick, 1);              // highest elevation overall
    EXPECT_TRUE(r.fallback);           // …and it is a COLD pin, counted
    EXPECT_EQ(r.n_refused, 3);
}

// A non-finite value or a non-positive variance is not a datum however good the rest of
// the entry looks — the "+17.08 m target, 378-σ spike" guard the continuous branch
// already carries, now asked of every candidate BEFORE one is elected.
TEST(PhaseHeldBasisTest, LedgerUsabilityRefusesDegenerateEntries) {
    EXPECT_FALSE(ledgerUsable(false, 3.0, 0.001, 0.01));           // no entry
    EXPECT_FALSE(ledgerUsable(true, std::nan(""), 0.001, 0.01));   // non-finite value
    EXPECT_FALSE(ledgerUsable(true, 3.0, 0.0, 0.01));              // zero variance
    EXPECT_FALSE(ledgerUsable(true, 3.0, -1.0, 0.01));             // negative variance
    EXPECT_FALSE(ledgerUsable(true, 3.0,
                              std::numeric_limits<double>::infinity(), 0.01));
    EXPECT_TRUE (ledgerUsable(true, 3.0, 0.01, 0.01));             // exactly at the ceiling
    EXPECT_FALSE(ledgerUsable(true, 3.0, 0.0100001, 0.01));        // just over it
}

// THE STICKY GUARD.  An incumbent is deposed only by something about ITS OWN basis
// column — a flag on that column, its arc's death, or its own ledger verdict.  A flag on
// a DIFFERENT ambiguity at the same station must not move the datum: the whole point of
// holding it is that it stops moving for reasons that are not about it.
TEST(PhaseHeldBasisTest, AFlagElsewhereAtTheSameStationDoesNotDeposeTheIncumbent) {
    std::map<std::tuple<int, std::string, std::string>, LocalCarry> carry;
    LocalCarry old{0.0, 1e-3, 0.0}; old.birth_ep = 10;
    carry[{0, "G07", "L1C"}] = old;    // the basis column
    carry[{0, "G07", "L2W"}] = old;    // a SIBLING band at the same station
    carry[{0, "G13", "L1C"}] = old;    // another satellite at the same station
    std::map<std::pair<int, std::string>, long long> flags;
    flags[{0, "G07_L2W"}] = 100;       // flagged: the sibling band
    flags[{0, "G13_L1C"}] = 100;       // flagged: another satellite

    PhaseHeldBasis h;
    h.on = true; h.min_age_ep = 10; h.flag_quiet_ep = 5; h.now_ep = 100;
    h.carry = &carry; h.flag_ep = &flags;
    // The basis column is untouched by either flag — the predicate is keyed on
    // (station, THIS ambiguity key), so it is column-scoped by construction.
    EXPECT_TRUE(basisEligible(h, 0, "G07_L1C", "G07", "L1C"));
    // …and a flag on the basis column itself DOES depose it.
    flags[{0, "G07_L1C"}] = 100;
    EXPECT_FALSE(basisEligible(h, 0, "G07_L1C", "G07", "L1C"));
}

// ARC DEATH is its own cause, distinguishable from a flag: the basis arc has no carry at
// all, so there is no level to hold and no age to qualify on.  The run-end split reads
// the two apart, which is what lets a run say WHICH lever to pull.
TEST(PhaseHeldBasisTest, ArcDeathIsDistinguishableFromAFlag) {
    std::map<std::tuple<int, std::string, std::string>, LocalCarry> carry;
    std::map<std::pair<int, std::string>, long long> flags;
    PhaseHeldBasis h;
    h.on = true; h.min_age_ep = 10; h.flag_quiet_ep = 5; h.now_ep = 100;
    h.carry = &carry; h.flag_ep = &flags;
    // No carry and no flag: refused, and the election attributes it to DEATH (it tests
    // "no carry entry" first and "flagged" only when one exists).
    EXPECT_FALSE(basisEligible(h, 0, "G07_L1C", "G07", "L1C"));
    EXPECT_EQ(carry.count(std::make_tuple(0, std::string("G07"), std::string("L1C"))), 0u);
    EXPECT_EQ(flags.count({0, "G07_L1C"}), 0u);
}
