/**
 * @file test_phase_datum_handover.cpp
 * @brief Gates for the two-edge continuous anchor handover
 *        (network.phase_datum_handover_continuous).
 *
 * The suite names here (`PhaseDatumHandover`, `PhaseDatumHandoverLoader`) are owned
 * by this file alone; the repository keeps one suite per file with no cross-file
 * overlap.
 *
 * Under test: the phase-bias gauge is one constraint per (sat,signal), "station r*'s
 * ambiguity for this key = target".  Every mechanism that moves r* re-pins the
 * successor at 0, which realizes a different frame: the whole family steps by the
 * inter-station ambiguity difference and the satellite clock absorbs the step
 * (docs/SPEC_pivot_visibility_gauge_defect.md has the fixture measurement).
 *
 * The continuous handover pins the successor at its own value in the outgoing frame,
 * so the gauge change is an S-transform and nothing steps.  Each case below names
 * the silent failure it guards, because each such failure produces a run that
 * completes and reports plausible counters while the frame walks.
 *
 * The carry side of the same edge, network.phase_anchor_carry_reref (QUEUE item
 * 1(d)), is tested here too.  Per §5.07 the datum re-realizes on the design matrix
 * only, while the carried bˢ_σ is folded forward in the retired frame at
 * q_pb = 1e-12 and the satellite clock (q = 0.01, a 1e10:1 ratio) absorbs the step.
 * The election therefore marks each handover's (sat,signal) for a one-epoch carry
 * marginalization and local-carry erase, as eq34_reref does for the chain datum.
 */

#include <gtest/gtest.h>

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkAnchorHygiene.hpp"
#include "filter/NetworkPhaseDatum.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "filter/StateKeySchema.hpp"
#include "logging/LogManager.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <tuple>     // 1(d): the LocalCarry map key (station, sat, signal)
#include <utility>
#include <vector>

using namespace GnssProcessor;

namespace {

constexpr const char* kSig = "L1C";
/// A well-converged conditional variance (m^2), below any reasonable ceiling.
constexpr double kGoodVar = 1.0e-4;
/// The ceiling the tests drive the election with.
constexpr double kMaxVar = 1.0;

/// Per-station census entry: which satellites this station tracks on kSig.
struct StationSpec {
    std::string name;
    std::vector<std::string> sats;
    int coverage = 0;   ///< the election's per-constellation ranking key (higher wins)
};

/// The inputs the anchor election consumes.  No StateManagers: the election reads
/// only station names and the candidate census (the same property the eq34
/// chain-datum tests rely on), so the handover can be tested in isolation.
struct Census {
    std::vector<NetStation> stations;
    std::vector<bool> present;
    PhaseDatumCandidates cnd;
};

Census makeCensus(const std::vector<StationSpec>& spec) {
    Census c;
    c.stations.resize(spec.size());
    c.present.assign(spec.size(), true);
    c.cnd.sat_el.resize(spec.size());
    c.cnd.usable_keys.resize(spec.size());
    for (std::size_t i = 0; i < spec.size(); ++i) {
        c.stations[i].name = spec[i].name;
        c.cnd.cover['G'][i] = spec[i].coverage;
        c.cnd.cand_set['G'].insert(i);
        for (const auto& sat : spec[i].sats) {
            const std::string amb = sat + "_" + kSig;
            c.cnd.sat_el[i][sat] = 45.0;
            c.cnd.usable_keys[i][amb] = StateKeys::makeSatPhaseBiasKey(sat, kSig);
            c.cnd.union_keys[amb] = StateKeys::makeSatPhaseBiasKey(sat, kSig);
        }
    }
    return c;
}

/// Drive one election.  `ledger == nullptr` is the legacy (handover-off) path.
struct Elected {
    std::map<std::string, std::string> incumbent;
    std::map<std::string, double> pin_target;   ///< sticky continuous targets (R2)
    std::map<std::string, std::string> this_epoch;
    std::set<std::string> shared;
    NetworkAnchorTallies tally;
    std::vector<AnchorHandoverEvent> events;
};

void elect(Census& c, Elected& e, const AnchorHandoverLedger* ledger,
           bool sticky = true, double max_var = kMaxVar,
           std::set<std::pair<std::string, std::string>>* reref = nullptr) {
    AnchorHygiene hyg;   // default-constructed = inactive
    e.events.clear();
    networkElectPhaseAnchors(sticky, c.stations, c.present, c.cnd, hyg, e.tally,
                             e.incumbent, e.pin_target, e.this_epoch, e.shared,
                             ledger, &e.events, max_var, reref);
}

double pinOf(const Census& c, const std::string& station, const std::string& amb) {
    for (const auto& st : c.stations)
        if (st.name == station) {
            auto it = st.pinned_amb_targets.find(amb);
            return it == st.pinned_amb_targets.end()
                       ? std::numeric_limits<double>::quiet_NaN() : it->second;
        }
    return std::numeric_limits<double>::quiet_NaN();
}

} // namespace

// ---------------------------------------------------------------------------
// The election
// ---------------------------------------------------------------------------

/// A first pin is not a handover: there is no outgoing frame to preserve, so it
/// goes in at 0 and is not counted as either edge.
TEST(PhaseDatumHandover, FirstPinIsNotAHandover) {
    auto c = makeCensus({{"STA_A", {"G01"}, 10}});
    Elected e;
    AnchorHandoverLedger led;
    led.value_abs[{0, "G01_L1C"}] = {7.5, kGoodVar};   // present, but must be ignored
    led.primed = true;
    elect(c, e, &led);
    EXPECT_EQ(pinOf(c, "STA_A", "G01_L1C"), 0.0);
    EXPECT_EQ(e.tally.handover_continuous, 0);
    EXPECT_EQ(e.tally.handover_cold, 0);
    EXPECT_TRUE(e.events.empty());
}

/// Silent failure guarded: a settled network whose anchors never move is
/// byte-identical with the handover armed.  If a non-handover epoch retargeted, the
/// gauge would walk every epoch instead of only at handovers, which is worse than
/// the step the handover removes.
TEST(PhaseDatumHandover, NoHandoverMeansNoRetarget) {
    auto c = makeCensus({{"STA_A", {"G01"}, 10}});
    Elected e;
    AnchorHandoverLedger led;
    led.primed = true;
    elect(c, e, &led);                       // epoch 1: first pin
    led.value_abs[{0, "G01_L1C"}] = {3.25, kGoodVar};    // A now has an estimate
    elect(c, e, &led);                       // epoch 2: same anchor, no move
    EXPECT_EQ(pinOf(c, "STA_A", "G01_L1C"), 0.0) << "a settled anchor must stay at 0";
    EXPECT_EQ(e.tally.handover_continuous, 0);
    EXPECT_EQ(e.tally.handover_cold, 0);
    EXPECT_TRUE(e.events.empty());
}

/// The continuous handover: when the incumbent anchor disappears and the gauge moves
/// to a peer, the peer is pinned at its own value in the outgoing frame, so the
/// realization is unchanged and the emitted family does not step.
TEST(PhaseDatumHandover, HandoverPinsSuccessorAtItsOutgoingFrameValue) {
    auto c = makeCensus({{"STA_A", {"G01"}, 10}, {"STA_B", {"G01"}, 5}});
    Elected e;
    AnchorHandoverLedger led;
    led.primed = true;
    elect(c, e, &led);
    ASSERT_EQ(e.this_epoch["G01_L1C"], "STA_A") << "higher coverage wins first";

    // STA_B's absolute ambiguity in the frame STA_A realized.
    led.value_abs[{1, "G01_L1C"}] = {-12.0554, kGoodVar};
    // STA_A drops out entirely, so the gauge moves.
    auto c2 = makeCensus({{"STA_B", {"G01"}, 5}});
    c2.stations[0].name = "STA_B";
    Elected e2;
    e2.incumbent = e.this_epoch;             // carry the incumbent across the epoch
    // the ledger is keyed on station index, and STA_B is index 0 in the new census
    AnchorHandoverLedger led2;
    led2.primed = true;
    led2.value_abs[{0, "G01_L1C"}] = {-12.0554, kGoodVar};
    elect(c2, e2, &led2);

    EXPECT_EQ(e2.this_epoch["G01_L1C"], "STA_B");
    EXPECT_DOUBLE_EQ(pinOf(c2, "STA_B", "G01_L1C"), -12.0554)
        << "the successor must be pinned in the OUTGOING frame, not at 0";
    EXPECT_EQ(e2.tally.handover_continuous, 1);
    EXPECT_EQ(e2.tally.handover_cold, 0);
    EXPECT_DOUBLE_EQ(e2.tally.handover_max_abs_m, 12.0554);
    ASSERT_EQ(e2.events.size(), 1u);
    EXPECT_EQ(e2.events[0].from, "STA_A");
    EXPECT_EQ(e2.events[0].to, "STA_B");
    EXPECT_TRUE(e2.events[0].continuous);
}

/// The relative variance ceiling (network.phase_datum_handover_max_var_ratio): with
/// the ratio armed, a ledger value is usable only if var <= ratio × the arc's
/// first-stash conditional variance.  Cases:
///  - a converged arc (1e-3 against 1.0 at ratio 0.01) is pinned continuously, and
///    its var and var_first are reported on the event;
///  - an arc that has not converged by the factor is refused as cold and counted on
///    the ratio counter, not the absolute one;
///  - an arc with an unknown first-stash variance is refused, so the
///    unvalidated-target guard stays armed under the ratio test;
///  - the absolute ceiling remains a rail with its own counter;
///  - ratio 0 is the absolute test alone.
TEST(PhaseDatumHandover, RatioCeilingRefusesUnconvergedAndUnknownArcs) {
    auto seed = makeCensus({{"STA_A", {"G01"}, 10}, {"STA_B", {"G01"}, 5}});
    Elected e;
    AnchorHandoverLedger led;
    led.primed = true;
    elect(seed, e, &led);
    ASSERT_EQ(e.this_epoch["G01_L1C"], "STA_A");

    struct Out { double pin; NetworkAnchorTallies t; std::vector<AnchorHandoverEvent> ev; };
    const auto run = [&](AnchorHandoverLedger::Entry entry, double ratio) {
        auto c2 = makeCensus({{"STA_B", {"G01"}, 5}});
        Elected e2;
        e2.incumbent = e.this_epoch;
        AnchorHandoverLedger l2;
        l2.primed = true;
        l2.value_abs[{0, "G01_L1C"}] = entry;
        AnchorHygiene hyg;
        networkElectPhaseAnchors(true, c2.stations, c2.present, c2.cnd, hyg, e2.tally,
                                 e2.incumbent, e2.pin_target, e2.this_epoch, e2.shared,
                                 &l2, &e2.events, kMaxVar, nullptr, ratio);
        return Out{pinOf(c2, "STA_B", "G01_L1C"), e2.tally, e2.events};
    };

    AnchorHandoverLedger::Entry conv;
    conv.value = -12.0554; conv.var = 1e-3; conv.var_first = 1.0;
    const Out a = run(conv, 0.01);             // converged: 1e-3 <= 0.01 x 1.0
    EXPECT_DOUBLE_EQ(a.pin, -12.0554);
    EXPECT_EQ(a.t.handover_continuous, 1);
    EXPECT_EQ(a.t.handover_refused_ratio, 0);
    ASSERT_EQ(a.ev.size(), 1u);
    EXPECT_TRUE(a.ev[0].continuous);
    EXPECT_DOUBLE_EQ(a.ev[0].var, 1e-3);
    EXPECT_DOUBLE_EQ(a.ev[0].var_first, 1.0);

    AnchorHandoverLedger::Entry young = conv;
    young.var = 0.05;                          // 0.05 > 0.01 x 1.0: not converged by the factor
    const Out b = run(young, 0.01);
    EXPECT_EQ(b.pin, 0.0);
    EXPECT_EQ(b.t.handover_cold, 1);
    EXPECT_EQ(b.t.handover_refused_ratio, 1);
    EXPECT_EQ(b.t.handover_refused_var, 0) << "0.05 m^2 is under the 1 m^2 absolute rail";
    ASSERT_EQ(b.ev.size(), 1u);
    EXPECT_FALSE(b.ev[0].continuous);
    EXPECT_DOUBLE_EQ(b.ev[0].var, 0.05);

    AnchorHandoverLedger::Entry unknown = conv;
    unknown.var_first = 0.0;                   // never carried: the unvalidated-target guard
    const Out u = run(unknown, 0.01);
    EXPECT_EQ(u.pin, 0.0);
    EXPECT_EQ(u.t.handover_cold, 1);
    EXPECT_EQ(u.t.handover_refused_ratio, 1);

    const Out off = run(unknown, 0.0);         // ratio off: the absolute test alone
    EXPECT_DOUBLE_EQ(off.pin, -12.0554);
    EXPECT_EQ(off.t.handover_continuous, 1);
    EXPECT_EQ(off.t.handover_refused_ratio, 0);

    AnchorHandoverLedger::Entry huge = conv;
    huge.var = kMaxVar * 10.0; huge.var_first = 1e6;   // passes the ratio, fails the rail
    const Out h = run(huge, 0.01);
    EXPECT_EQ(h.pin, 0.0);
    EXPECT_EQ(h.t.handover_refused_var, 1);
    EXPECT_EQ(h.t.handover_refused_ratio, 0);
}

/// Silent failure guarded: a successor with no ledger entry (a fresh arc, or the
/// run's first epoch) cannot be made continuous.  It pins at 0 and is counted as
/// cold, because it is a real frame step the consumer has to be told about;
/// counting it as continuous would hide the event the mechanism exists to report.
TEST(PhaseDatumHandover, ColdSuccessorPinsAtZeroAndIsCounted) {
    auto c = makeCensus({{"STA_A", {"G01"}, 10}, {"STA_B", {"G01"}, 5}});
    Elected e;
    AnchorHandoverLedger led;
    led.primed = true;
    elect(c, e, &led);
    ASSERT_EQ(e.this_epoch["G01_L1C"], "STA_A");

    auto c2 = makeCensus({{"STA_B", {"G01"}, 5}});
    Elected e2;
    e2.incumbent = e.this_epoch;
    AnchorHandoverLedger empty;              // primed but no entry for STA_B
    empty.primed = true;
    elect(c2, e2, &empty);

    EXPECT_EQ(pinOf(c2, "STA_B", "G01_L1C"), 0.0);
    EXPECT_EQ(e2.tally.handover_continuous, 0);
    EXPECT_EQ(e2.tally.handover_cold, 1) << "an unavoidable re-realization must be COUNTED";
    ASSERT_EQ(e2.events.size(), 1u);
    EXPECT_FALSE(e2.events[0].continuous);
    EXPECT_EQ(e2.events[0].target_m, 0.0);
}

/// A non-finite ledger value is refused, not pinned.  A NaN target would corrupt the
/// whole normal-equation row rather than only stepping the frame.
TEST(PhaseDatumHandover, NonFiniteLedgerValueIsRefusedAsCold) {
    auto c = makeCensus({{"STA_A", {"G01"}, 10}, {"STA_B", {"G01"}, 5}});
    Elected e;
    AnchorHandoverLedger led;
    led.primed = true;
    elect(c, e, &led);
    auto c2 = makeCensus({{"STA_B", {"G01"}, 5}});
    Elected e2;
    e2.incumbent = e.this_epoch;
    AnchorHandoverLedger bad;
    bad.primed = true;
    bad.value_abs[{0, "G01_L1C"}] = {std::numeric_limits<double>::quiet_NaN(), kGoodVar};
    elect(c2, e2, &bad);
    EXPECT_EQ(pinOf(c2, "STA_B", "G01_L1C"), 0.0);
    EXPECT_EQ(e2.tally.handover_cold, 1);
    EXPECT_EQ(e2.tally.handover_continuous, 0);
}

/// The off path is byte-identical: a null ledger reproduces the legacy election
/// exactly (every target 0.0, neither edge counted), while the observational event
/// list still reports the handover, so anchor churn can be measured on a legacy arm
/// before the handover is armed.
TEST(PhaseDatumHandover, NullLedgerIsTheLegacyElectionButStillObservable) {
    auto c = makeCensus({{"STA_A", {"G01"}, 10}, {"STA_B", {"G01"}, 5}});
    Elected e;
    elect(c, e, nullptr);
    auto c2 = makeCensus({{"STA_B", {"G01"}, 5}});
    Elected e2;
    e2.incumbent = e.this_epoch;
    elect(c2, e2, nullptr);

    EXPECT_EQ(pinOf(c2, "STA_B", "G01_L1C"), 0.0);
    EXPECT_EQ(e2.tally.handover_continuous, 0);
    EXPECT_EQ(e2.tally.handover_cold, 0);
    EXPECT_EQ(e2.tally.reattaches, 1) << "the handover still happened; only the fix is off";
    ASSERT_EQ(e2.events.size(), 1u) << "churn must be measurable on the OFF path";
    EXPECT_FALSE(e2.events[0].continuous);
}

/// Stickiness and continuity are complementary: stickiness minimizes the number of
/// handovers, and continuity removes the step from the handovers that remain.  A
/// sticky incumbent that still tracks the key does not hand over at all.
TEST(PhaseDatumHandover, StickyIncumbentIsNotHandedOverEvenWhenOutranked) {
    auto c = makeCensus({{"STA_A", {"G01"}, 1}, {"STA_B", {"G01"}, 99}});
    Elected e;
    AnchorHandoverLedger led;
    led.primed = true;
    e.incumbent["G01_L1C"] = "STA_A";        // A is the sticky incumbent
    led.value_abs[{1, "G01_L1C"}] = {42.0, kGoodVar};    // B has a value, but must not be used
    elect(c, e, &led, /*sticky=*/true, kMaxVar);
    EXPECT_EQ(e.this_epoch["G01_L1C"], "STA_A");
    EXPECT_EQ(e.tally.handover_continuous, 0);
    EXPECT_EQ(e.tally.handover_cold, 0);
    EXPECT_TRUE(e.events.empty());
}

/// The ledger is keyed on (station index, ambiguity key), so a value belonging to
/// a different station or a different key is never applied.  A mis-keyed lookup
/// would pin the successor at another station's ambiguity: a far larger step than
/// the one the handover removes, and one that looks like a successful handover.
TEST(PhaseDatumHandover, LedgerLookupIsKeyedOnStationAndAmbiguity) {
    auto c = makeCensus({{"STA_A", {"G01", "G02"}, 10}, {"STA_B", {"G01", "G02"}, 5}});
    Elected e;
    AnchorHandoverLedger led;
    led.primed = true;
    elect(c, e, &led);
    auto c2 = makeCensus({{"STA_B", {"G01", "G02"}, 5}});
    Elected e2;
    e2.incumbent = e.this_epoch;
    AnchorHandoverLedger led2;
    led2.primed = true;
    led2.value_abs[{0, "G02_L1C"}] = {5.0, kGoodVar};    // only G02 has a value
    led2.value_abs[{1, "G01_L1C"}] = {99.0, kGoodVar};   // wrong station index for this census
    elect(c2, e2, &led2);
    EXPECT_DOUBLE_EQ(pinOf(c2, "STA_B", "G02_L1C"), 5.0);
    EXPECT_EQ(pinOf(c2, "STA_B", "G01_L1C"), 0.0) << "must NOT borrow another key's value";
    EXPECT_EQ(e2.tally.handover_continuous, 1);
    EXPECT_EQ(e2.tally.handover_cold, 1);
}

/// The re-coupling edge re-validates (invariant #7).  The pin is a hard constraint
/// of weight ~1e10, so an ill-determined estimate installed as a datum injects its
/// own error; an unvalidated +17.08 m target produced a 378-sigma residual spike on
/// the next epoch, larger than the step it replaced.  A value above the variance
/// ceiling is refused and takes the cold path.
TEST(PhaseDatumHandover, IllDeterminedLedgerValueIsRefused) {
    auto c = makeCensus({{"STA_A", {"G01"}, 10}, {"STA_B", {"G01"}, 5}});
    Elected e;
    AnchorHandoverLedger led;
    led.primed = true;
    elect(c, e, &led);
    auto c2 = makeCensus({{"STA_B", {"G01"}, 5}});
    Elected e2;
    e2.incumbent = e.this_epoch;
    AnchorHandoverLedger loose;
    loose.primed = true;
    loose.value_abs[{0, "G01_L1C"}] = {17.0761, 25.0};   // sigma = 5 m: too loose for a datum
    elect(c2, e2, &loose, /*sticky=*/true, /*max_var=*/1.0);
    EXPECT_EQ(pinOf(c2, "STA_B", "G01_L1C"), 0.0) << "must NOT pin an unvalidated value";
    EXPECT_EQ(e2.tally.handover_continuous, 0);
    EXPECT_EQ(e2.tally.handover_cold, 1);
    EXPECT_EQ(e2.tally.handover_refused_var, 1) << "the refusal must be COUNTED separately";
}

/// A non-positive variance means the harvest could not determine one, so the value
/// is not pinned.
TEST(PhaseDatumHandover, MissingVarianceIsRefused) {
    auto c = makeCensus({{"STA_A", {"G01"}, 10}, {"STA_B", {"G01"}, 5}});
    Elected e;
    AnchorHandoverLedger led;
    led.primed = true;
    elect(c, e, &led);
    auto c2 = makeCensus({{"STA_B", {"G01"}, 5}});
    Elected e2;
    e2.incumbent = e.this_epoch;
    AnchorHandoverLedger novar;
    novar.primed = true;
    novar.value_abs[{0, "G01_L1C"}] = {3.0, -1.0};
    elect(c2, e2, &novar);
    EXPECT_EQ(pinOf(c2, "STA_B", "G01_L1C"), 0.0);
    EXPECT_EQ(e2.tally.handover_cold, 1);
}

// ---------------------------------------------------------------------------
// The PHASE_ANCHOR carry re-reference (QUEUE item 1(d);
// docs/SPEC_pivot_visibility_gauge_defect.md §5.07.5 R1)
// ---------------------------------------------------------------------------

/// The re-reference set marks every pin not continuously inherited from an
/// incumbent: a handover, and an incumbent-less pin (a first sighting, or a
/// bridge-exit re-pin whose incumbent record was dropped during the bridge era,
/// which would otherwise read a return as a first sighting; invariant #7).  A
/// settled anchor is never marked: marking it would marginalize a valid prior every
/// epoch, the failure NoHandoverMeansNoRetarget guards on the pin side.  In
/// production the carry stash clears the set (so a withheld epoch's marks persist);
/// the test performs that clear explicitly.
TEST(PhaseDatumHandover, CarryRerefMarksHandoversAndIncumbentlessPins) {
    std::set<std::pair<std::string, std::string>> reref;
    auto c = makeCensus({{"STA_A", {"G01"}, 10}});
    Elected e;
    elect(c, e, nullptr, true, kMaxVar, &reref);
    EXPECT_TRUE(reref.count({"G01", "L1C"}))
        << "an incumbent-less pin realizes a fresh frame — marked (no-op fold "
           "when no carry survived; the stale-frame guard when one did)";
    reref.clear();   // the stash's clear, after a successful commit
    elect(c, e, nullptr, true, kMaxVar, &reref);
    EXPECT_TRUE(reref.empty()) << "a settled anchor's carry is valid";

    auto c2 = makeCensus({{"STA_B", {"G01"}, 5}});
    Elected e2;
    e2.incumbent = e.this_epoch;
    elect(c2, e2, nullptr, true, kMaxVar, &reref);
    ASSERT_EQ(reref.size(), 1u) << "the handover must mark the family";
    EXPECT_TRUE(reref.count({"G01", "L1C"}))
        << "keyed (sat, signal) — parsed from the registered PB key";
}

/// The bridge-exit case: the incumbent record is gone (a bridged key skips the
/// election before any incumbent bookkeeping, so one bridged epoch removes it)
/// while the family's carry is still alive.  The eventual re-pin is marked even
/// though `is_handover` is false; otherwise the full-ambiguity frame step folds
/// against the bridge-frame carry at q_pb = 1e-12 and the clock absorbs it,
/// unmarked and uncounted.
TEST(PhaseDatumHandover, CarryRerefMarksBridgeExitRepin) {
    std::set<std::pair<std::string, std::string>> reref;
    auto c = makeCensus({{"STA_B", {"G01"}, 5}});
    Elected e;
    // No incumbent entry for G01_L1C at all: the post-bridge state.
    elect(c, e, nullptr, true, kMaxVar, &reref);
    EXPECT_EQ(e.this_epoch["G01_L1C"], "STA_B");
    EXPECT_EQ(e.tally.reattaches, 0) << "not a handover — no incumbent to hand from";
    EXPECT_TRUE(reref.count({"G01", "L1C"}))
        << "the incumbent-less re-pin must be marked regardless";
}

/// A continuous handover is not marked (R2, §5.09).  Under the sticky target the
/// successor realizes the outgoing frame for its whole tenure, so the family does
/// not step at T or T+1 and the carried state stays in frame; marking it would
/// marginalize a valid fold and erase live leveling carries (§5.09 has the measured
/// arc truncation).  A cold handover in the same election is still marked.
TEST(PhaseDatumHandover, CarryRerefSkipsContinuousHandoversUnderTheStickyTarget) {
    std::set<std::pair<std::string, std::string>> reref;
    auto c = makeCensus({{"STA_A", {"G01", "G02"}, 10}, {"STA_B", {"G01", "G02"}, 5}});
    Elected e;
    AnchorHandoverLedger led;
    led.primed = true;
    elect(c, e, &led);
    auto c2 = makeCensus({{"STA_B", {"G01", "G02"}, 5}});
    Elected e2;
    e2.incumbent = e.this_epoch;
    AnchorHandoverLedger led2;
    led2.primed = true;
    led2.value_abs[{0, "G01_L1C"}] = {-12.0554, kGoodVar};   // G02 has no entry, so it is cold
    elect(c2, e2, &led2, true, kMaxVar, &reref);
    ASSERT_EQ(e2.tally.handover_continuous, 1) << "precondition: G01 continuous";
    ASSERT_EQ(e2.tally.handover_cold, 1) << "precondition: G02 cold";
    EXPECT_FALSE(reref.count({"G01", "L1C"}))
        << "continuous + sticky = the frame never steps; the carry stays valid";
    EXPECT_TRUE(reref.count({"G02", "L1C"}))
        << "the cold edge in the SAME election must still mark";
}

/// The continuous target is sticky: it is re-applied every epoch the successor
/// remains incumbent (R2, §5.09).  Without it the pin reverts to 0 at T+1 and the
/// family steps one epoch late, after the mark was spent at T; §5.0 records that
/// the non-sticky version measured null for this reason.
TEST(PhaseDatumHandover, StickyTargetPersistsForTheSuccessorsTenure) {
    auto c = makeCensus({{"STA_A", {"G01"}, 10}, {"STA_B", {"G01"}, 5}});
    Elected e;
    AnchorHandoverLedger led;
    led.primed = true;
    elect(c, e, &led);
    // T: handover A -> B, continuous at the ledger value.
    auto c2 = makeCensus({{"STA_B", {"G01"}, 5}});
    Elected e2;
    e2.incumbent = e.this_epoch;
    AnchorHandoverLedger led2;
    led2.primed = true;
    led2.value_abs[{0, "G01_L1C"}] = {-12.0554, kGoodVar};
    elect(c2, e2, &led2);
    ASSERT_EQ(pinOf(c2, "STA_B", "G01_L1C"), -12.0554);
    // T+1 and T+2: same incumbent, no handover; the pin does not revert to 0.
    for (int k = 0; k < 2; ++k) {
        auto c3 = makeCensus({{"STA_B", {"G01"}, 5}});
        AnchorHandoverLedger led3;   // empty: a retained incumbent needs no ledger
        led3.primed = true;
        elect(c3, e2, &led3);
        EXPECT_EQ(e2.tally.reattaches, 1) << "no further handover";
        EXPECT_EQ(pinOf(c3, "STA_B", "G01_L1C"), -12.0554)
            << "T+" << (k + 1) << ": the inherited frame must persist";
    }
}

/// A cold handover drops any stored target: the frame re-realized at 0, so a later
/// retained epoch keeps realizing 0 and does not restore the retired frame.
TEST(PhaseDatumHandover, ColdHandoverDropsTheStoredTarget) {
    auto c = makeCensus({{"STA_A", {"G01"}, 10}, {"STA_B", {"G01"}, 5}});
    Elected e;
    AnchorHandoverLedger led;
    led.primed = true;
    elect(c, e, &led);
    auto c2 = makeCensus({{"STA_B", {"G01"}, 5}, {"STA_C", {"G01"}, 3}});
    Elected e2;
    e2.incumbent = e.this_epoch;
    AnchorHandoverLedger led2;
    led2.primed = true;
    led2.value_abs[{0, "G01_L1C"}] = {7.25, kGoodVar};   // index 0 = STA_B here
    elect(c2, e2, &led2);
    ASSERT_EQ(pinOf(c2, "STA_B", "G01_L1C"), 7.25) << "continuous A->B";
    // B drops the key: handover B->C with no ledger entry for C, so it is cold.
    auto c3 = makeCensus({{"STA_C", {"G01"}, 3}});
    AnchorHandoverLedger led3;
    led3.primed = true;
    elect(c3, e2, &led3);
    EXPECT_EQ(e2.tally.handover_cold, 1);
    EXPECT_EQ(pinOf(c3, "STA_C", "G01_L1C"), 0.0) << "honest cold re-realization";
    // Retained epoch after the cold handover: still the 0-frame.
    auto c4 = makeCensus({{"STA_C", {"G01"}, 3}});
    AnchorHandoverLedger led4;
    led4.primed = true;
    elect(c4, e2, &led4);
    EXPECT_EQ(pinOf(c4, "STA_C", "G01_L1C"), 0.0)
        << "the dead frame must not resurrect on a retained epoch";
}

/// A key that leaves the union drops its target with its incumbent, so its eventual
/// return is an incumbent-less pin at 0, marked, with no stale frame carried across
/// the gap.
TEST(PhaseDatumHandover, KeyLeavingTheUnionDropsItsTarget) {
    std::set<std::pair<std::string, std::string>> reref;
    auto c = makeCensus({{"STA_A", {"G01"}, 10}, {"STA_B", {"G01"}, 5}});
    Elected e;
    AnchorHandoverLedger led;
    led.primed = true;
    elect(c, e, &led);
    auto c2 = makeCensus({{"STA_B", {"G01"}, 5}});
    Elected e2;
    e2.incumbent = e.this_epoch;
    AnchorHandoverLedger led2;
    led2.primed = true;
    led2.value_abs[{0, "G01_L1C"}] = {3.5, kGoodVar};
    elect(c2, e2, &led2);
    ASSERT_EQ(e2.pin_target.at("G01_L1C"), 3.5);
    // The key vanishes for one election (nobody tracks G01).
    auto c3 = makeCensus({{"STA_B", {"G02"}, 5}});
    AnchorHandoverLedger led3;
    led3.primed = true;
    elect(c3, e2, &led3);
    EXPECT_FALSE(e2.pin_target.count("G01_L1C")) << "target dies with the incumbent";
    // Return: incumbent-less -> 0-pin, marked.
    auto c4 = makeCensus({{"STA_B", {"G01"}, 5}});
    AnchorHandoverLedger led4;
    led4.primed = true;
    led4.value_abs[{0, "G01_L1C"}] = {3.5, kGoodVar};   // present, but must be ignored
    elect(c4, e2, &led4, true, kMaxVar, &reref);
    EXPECT_EQ(pinOf(c4, "STA_B", "G01_L1C"), 0.0)
        << "a first sighting has no outgoing frame to inherit";
    EXPECT_TRUE(reref.count({"G01", "L1C"})) << "and it is marked";
}

/// The null-pointer path is the off contract: nothing is populated and nothing
/// downstream can fire (the byte-identity part of invariant #6).  The election
/// cases above that do not pass a reref set use the default nullptr and assert
/// pins and tallies there; this case checks only the pointer handling itself.
TEST(PhaseDatumHandover, CarryRerefNullOutIsUntouched) {
    auto c = makeCensus({{"STA_A", {"G01"}, 10}, {"STA_B", {"G01"}, 5}});
    Elected e;
    elect(c, e, nullptr);
    auto c2 = makeCensus({{"STA_B", {"G01"}, 5}});
    Elected e2;
    e2.incumbent = e.this_epoch;
    elect(c2, e2, nullptr);   // handover happens, no reref sink supplied
    EXPECT_EQ(e2.tally.reattaches, 1);
    EXPECT_EQ(pinOf(c2, "STA_B", "G01_L1C"), 0.0);
}

/// The erase drops the family at every station: the frame stepped network-wide, so
/// a station-scoped erase would leave stale floats in conflict with the new frame at
/// every station it skipped.  An unrelated family survives unchanged.
TEST(PhaseDatumHandover, CarryRerefEraseDropsEveryStationOfTheFamily) {
    std::map<std::tuple<int, std::string, std::string>, LocalCarry> lc;
    lc[{0, "G01", "L1C"}] = LocalCarry{-3.2, 1e-4, 100.0, {}};
    lc[{1, "G01", "L1C"}] = LocalCarry{ 7.1, 2e-4, 100.0, {}};
    lc[{0, "G02", "L1C"}] = LocalCarry{ 0.5, 1e-4, 100.0, {}};
    const std::set<std::pair<std::string, std::string>> reref{{"G01", "L1C"}};
    EXPECT_EQ(networkApplyPhaseAnchorCarryReref(reref, lc), 2);
    EXPECT_EQ(lc.size(), 1u);
    EXPECT_TRUE(lc.count({0, "G02", "L1C"})) << "an unrelated family must survive";
    EXPECT_EQ(networkApplyPhaseAnchorCarryReref({}, lc), 0)
        << "an empty set is the no-op fast path";
}

// ---------------------------------------------------------------------------
// The loader
// ---------------------------------------------------------------------------

namespace {

/// A minimal loadable PPP-RTK network config; @p extra is spliced into the network
/// block (each entry must end with a comma).  Same shape as
/// tests/test_raim_carry_invariant_pool.cpp: the loader requires a real roster before
/// it validates a network posture.
std::string networkConfig(const std::string& extra) {
    return std::string("{\n") +
        "  \"processing\": {\n"
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

bool loadNet(const std::string& tag, const std::string& extra, GnssConfig& cfg) {
    namespace fs = std::filesystem;
    LogManager log;
    const fs::path p = fs::temp_directory_path() / ("pd_handover_" + tag + ".json");
    { std::ofstream f(p); f << networkConfig(extra); }
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec;
    fs::remove(p, ec);
    return ok;
}

} // namespace

/// Off by default: the byte-identity contract (invariant #6).
TEST(PhaseDatumHandoverLoader, DefaultsOff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadNet("default", "    \"phase_datum_prioritized\": true,\n", cfg));
    EXPECT_FALSE(cfg.networkPhaseDatumHandoverContinuous());
}

TEST(PhaseDatumHandoverLoader, ArmsUnderThePrioritizedElection) {
    GnssConfig cfg;
    ASSERT_TRUE(loadNet("armed",
                        "    \"phase_datum_prioritized\": true,\n"
                        "    \"phase_datum_handover_continuous\": true,\n", cfg));
    EXPECT_TRUE(cfg.networkPhaseDatumHandoverContinuous());
}

/// The loader rejects an armed but inert knob: the legacy datum elects no anchor, so
/// there is no handover to make continuous (it deletes the column instead, which is
/// a more severe form of the same frame step).
TEST(PhaseDatumHandoverLoader, DisabledWithoutThePrioritizedElection) {
    GnssConfig cfg;
    ASSERT_TRUE(loadNet("legacy",
                        "    \"phase_datum_handover_continuous\": true,\n", cfg));
    EXPECT_FALSE(cfg.networkPhaseDatumHandoverContinuous());
}

/// Off by default: the byte-identity contract (invariant #6; QUEUE item 1(d)).
TEST(PhaseDatumHandoverLoader, CarryRerefDefaultsOff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadNet("reref_default",
                        "    \"phase_datum_prioritized\": true,\n", cfg));
    EXPECT_FALSE(cfg.networkPhaseAnchorCarryReref());
}

TEST(PhaseDatumHandoverLoader, CarryRerefArmsUnderThePrioritizedElection) {
    GnssConfig cfg;
    ASSERT_TRUE(loadNet("reref_armed",
                        "    \"phase_datum_prioritized\": true,\n"
                        "    \"phase_anchor_carry_reref\": true,\n", cfg));
    EXPECT_TRUE(cfg.networkPhaseAnchorCarryReref());
}

/// Armed-but-inert rejection, the same ladder as for the handover knob: the legacy
/// datum elects no anchor, so no PHASE_ANCHOR handover exists to re-reference.
TEST(PhaseDatumHandoverLoader, CarryRerefDisabledWithoutThePrioritizedElection) {
    GnssConfig cfg;
    ASSERT_TRUE(loadNet("reref_legacy",
                        "    \"phase_anchor_carry_reref\": true,\n", cfg));
    EXPECT_FALSE(cfg.networkPhaseAnchorCarryReref());
}

/// The eq34 chain has its own eq34_reref carry hygiene; arming both would treat the
/// same edge twice, so the loader disarms this one.
TEST(PhaseDatumHandoverLoader, CarryRerefDisabledUnderTheChainDatum) {
    GnssConfig cfg;
    ASSERT_TRUE(loadNet("reref_eq34",
                        "    \"phase_datum_prioritized\": true,\n"
                        "    \"phase_datum_eq34\": true,\n"
                        "    \"phase_anchor_carry_reref\": true,\n", cfg));
    EXPECT_FALSE(cfg.networkPhaseAnchorCarryReref());
}

/// An eq34-only config (prioritized not set) also disarms, through the eq34 guard,
/// which comes first in the ladder for this posture.  With the prioritized check
/// first, the loader would emit the wrong "legacy datum elects no anchor"
/// diagnostic, since eq34 elects a chain datum.
TEST(PhaseDatumHandoverLoader, CarryRerefDisabledUnderEq34Alone) {
    GnssConfig cfg;
    ASSERT_TRUE(loadNet("reref_eq34only",
                        "    \"phase_datum_eq34\": true,\n"
                        "    \"phase_anchor_carry_reref\": true,\n", cfg));
    EXPECT_FALSE(cfg.networkPhaseAnchorCarryReref());
}

/// The two knobs compose (R2, §5.09).  A non-sticky ledger pin would defer the
/// frame step to T+1 while the carry mark covers T, which is why the loader once
/// disarmed the pair.  The R2 sticky target removes the revert and the election
/// marks only cold re-realizations, so each mechanism treats its own edge:
/// continuous handovers keep the frame and the carries, cold handovers mark,
/// marginalize and erase.  Both knobs survive the loader.
TEST(PhaseDatumHandoverLoader, CarryRerefComposesWithTheContinuousPairing) {
    GnssConfig cfg;
    ASSERT_TRUE(loadNet("reref_pairing",
                        "    \"phase_datum_prioritized\": true,\n"
                        "    \"phase_datum_handover_continuous\": true,\n"
                        "    \"phase_anchor_carry_reref\": true,\n", cfg));
    EXPECT_TRUE(cfg.networkPhaseAnchorCarryReref()) << "the reref survives";
    EXPECT_TRUE(cfg.networkPhaseDatumHandoverContinuous())
        << "R2: the continuous handover survives the pairing (the T+1 revert is "
           "gone, so the mark-timing conflict no longer exists)";
}

/// The relative variance ceiling: off by default (0 means the absolute ceiling
/// alone, byte-identical), parsed beside the absolute ceiling, and a negative ratio
/// is refused.
TEST(PhaseDatumHandoverLoader, RatioCeilingDefaultsOffParsesAndRejectsNegative) {
    GnssConfig cfg;
    ASSERT_TRUE(loadNet("ratio_default", "    \"phase_datum_prioritized\": true,\n", cfg));
    EXPECT_EQ(cfg.networkPhaseDatumHandoverMaxVarRatio(), 0.0);
    GnssConfig on;
    ASSERT_TRUE(loadNet("ratio_on",
                        "    \"phase_datum_prioritized\": true,\n"
                        "    \"phase_datum_handover_continuous\": true,\n"
                        "    \"phase_datum_handover_max_var_ratio\": 0.01,\n", on));
    EXPECT_DOUBLE_EQ(on.networkPhaseDatumHandoverMaxVarRatio(), 0.01);
    EXPECT_TRUE(on.networkPhaseDatumHandoverContinuous());
    GnssConfig neg;
    EXPECT_FALSE(loadNet("ratio_neg",
                         "    \"phase_datum_prioritized\": true,\n"
                         "    \"phase_datum_handover_max_var_ratio\": -0.5,\n", neg));
}
