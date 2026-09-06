/**
 * @file test_phase_datum_handover.cpp
 * @brief The TWO-EDGE CONTINUOUS ANCHOR HANDOVER
 *        (network.phase_datum_handover_continuous).
 *
 * Suite names here (`PhaseDatumHandover`, `PhaseDatumHandoverLoader`) are owned
 * by THIS FILE ONLY — the repo keeps one suite per file with zero cross-file
 * overlap.
 *
 * WHAT IS UNDER TEST.  The phase-bias gauge is one constraint per (sat,signal):
 * "station r*'s ambiguity for this key = target".  Every mechanism that moves r*
 * re-pins the SUCCESSOR at 0, which realizes a DIFFERENT frame — the whole
 * family steps by the inter-station ambiguity difference and the satellite clock
 * absorbs it.  Measured on a truth-clean fixture: a legacy pivot losing G15
 * stepped the emitted clock -12.06 m and never recovered
 * (docs/SPEC_pivot_visibility_gauge_defect.md).
 *
 * The fix hands the pin over AT THE SUCCESSOR'S OWN VALUE IN THE OUTGOING FRAME,
 * so the gauge change is an S-transform and nothing steps.  Every case below
 * names the silent failure it guards, because each produces a run that completes
 * cleanly and reports plausible counters while the frame walks.
 *
 * QUEUE item 1(d) (network.phase_anchor_carry_reref) is tested here too: the
 * CARRY side of the same edge.  §5.07 named the third root — the datum
 * re-realizes on the design matrix only while the carried bˢ_σ is folded forward
 * in the RETIRED frame at q_pb = 1e-12, and the satellite clock (q = 0.01,
 * 1e10:1) absorbs the step.  The election now marks each handover's (sat,signal)
 * for a one-epoch carry marginalization + local-carry erase, exactly as
 * eq34_reref does for the chain datum.
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
/// A comfortably-converged conditional variance (m^2) — under any sane ceiling.
constexpr double kGoodVar = 1.0e-4;
/// The ceiling the tests drive the election with.
constexpr double kMaxVar = 1.0;

/// Per-station census entry: which satellites this station tracks on kSig.
struct StationSpec {
    std::string name;
    std::vector<std::string> sats;
    int coverage = 0;   ///< the election's per-constellation ranking key (higher wins)
};

/// The pieces the anchor election consumes.  No StateManagers: the election reads
/// only station NAMES and the candidate census (same property the eq34 chain-datum
/// tests rely on), which is what makes the handover testable in isolation.
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

/// Drive one election.  `ledger == nullptr` is the pre-card path.
struct Elected {
    std::map<std::string, std::string> incumbent;
    std::map<std::string, double> pin_target;   ///< R2 sticky continuous targets
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

/// A FIRST pin is not a handover: there is no outgoing frame to preserve, so it
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

/// SILENT FAILURE this guards: a settled network whose anchors never move must be
/// byte-identical with the fix armed.  If a non-handover epoch ever retargets, the
/// gauge walks every epoch instead of only at handovers — strictly worse than the
/// defect being fixed.
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

/// THE FIX.  When the incumbent anchor disappears and the gauge moves to a peer,
/// the peer is pinned at ITS OWN value in the outgoing frame — so the realization
/// is unchanged and the emitted family does not step.
TEST(PhaseDatumHandover, HandoverPinsSuccessorAtItsOutgoingFrameValue) {
    auto c = makeCensus({{"STA_A", {"G01"}, 10}, {"STA_B", {"G01"}, 5}});
    Elected e;
    AnchorHandoverLedger led;
    led.primed = true;
    elect(c, e, &led);
    ASSERT_EQ(e.this_epoch["G01_L1C"], "STA_A") << "higher coverage wins first";

    // STA_B's absolute ambiguity in the frame STA_A realized.
    led.value_abs[{1, "G01_L1C"}] = {-12.0554, kGoodVar};
    // STA_A drops out entirely: the gauge must move.
    auto c2 = makeCensus({{"STA_B", {"G01"}, 5}});
    c2.stations[0].name = "STA_B";
    Elected e2;
    e2.incumbent = e.this_epoch;             // carry the incumbent across the epoch
    // the ledger is keyed on station INDEX, and STA_B is index 0 in the new census
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

/// The HONEST-σ-SCALE ceiling (network.phase_datum_handover_max_var_ratio): with the
/// ratio armed a ledger value is usable only if var <= ratio × the arc's FIRST-STASH
/// conditional variance.  A converged arc (1e-3 vs 1.0 at ratio 0.01) is pinned
/// continuously and its var / var_first ride the event; an arc that has not converged
/// by the factor is refused as COLD and counted on the RATIO counter, not the absolute
/// one; an arc with an UNKNOWN first-stash variance is refused — the 378-σ
/// unvalidated-target guard has to stay demonstrably armed under the new test; the
/// absolute ceiling stays a rail and keeps its own counter; ratio 0 is the old test.
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

    const Out off = run(unknown, 0.0);         // ratio OFF: the absolute test alone
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

/// SILENT FAILURE this guards: a successor with no ledger entry (a genuinely fresh
/// arc, or the run's first epoch) CANNOT be made continuous.  It must pin at 0 —
/// and be COUNTED, because that is a real frame step the consumer has to be told
/// about.  Burying it in the continuous counter would hide exactly the event the
/// mechanism exists to surface.
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

/// A non-finite ledger value must be refused, not pinned.  A NaN target would
/// poison the whole normal-equation row rather than merely stepping the frame.
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

/// OFF PATH = byte-identical.  A null ledger must reproduce the pre-card election
/// exactly: every target 0.0, neither edge counted — while the observational
/// event list still reports the handover, so a legacy arm's churn is MEASURABLE
/// before the fix is armed.
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

/// Stickiness and continuity are complementary, not alternatives: stickiness
/// minimizes the NUMBER of handovers, continuity makes the survivors free.  A
/// sticky incumbent that still tracks the key must not hand over at all.
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
/// a DIFFERENT station or a different key must never be applied.  A mis-keyed
/// lookup would pin the successor at a stranger's ambiguity — a far larger step
/// than the one being fixed, and one that would look like a successful handover.
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

/// THE RE-COUPLING EDGE MUST RE-VALIDATE (invariant #7).  The pin is a hard
/// ~1e10-weight constraint, so an ill-determined estimate stamped in as a datum
/// injects its own error — measured, an unvalidated +17.08 m target produced a
/// 378-sigma residual spike the next epoch, WORSE than the step it replaced.  A
/// value above the variance ceiling must be refused to the honest COLD path.
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
    loose.value_abs[{0, "G01_L1C"}] = {17.0761, 25.0};   // sigma = 5 m: not a datum
    elect(c2, e2, &loose, /*sticky=*/true, /*max_var=*/1.0);
    EXPECT_EQ(pinOf(c2, "STA_B", "G01_L1C"), 0.0) << "must NOT pin an unvalidated value";
    EXPECT_EQ(e2.tally.handover_continuous, 0);
    EXPECT_EQ(e2.tally.handover_cold, 1);
    EXPECT_EQ(e2.tally.handover_refused_var, 1) << "the refusal must be COUNTED separately";
}

/// A non-positive variance means the harvest could not determine one at all; that
/// is not a licence to pin.
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
// QUEUE item 1(d): the PHASE_ANCHOR carry re-reference
// (docs/SPEC_pivot_visibility_gauge_defect.md §5.07.5 R1)
// ---------------------------------------------------------------------------

/// The re-reference set marks every pin NOT continuously inherited from an
/// incumbent: a handover, and an incumbent-less pin (first sighting, or a
/// bridge-exit re-pin whose incumbent record evaporated during the bridge era —
/// the review-confirmed invariant-#7 "return read as a first sighting" hole).
/// A SETTLED anchor is never marked — marking it would marginalize a healthy
/// prior every epoch, the strictly-worse failure NoHandoverMeansNoRetarget
/// guards on the pin side.  In production the set is cleared by the CARRY STASH
/// (so a withheld epoch's marks persist); the test models that clear explicitly.
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

/// The bridge-exit shape specifically: the incumbent record is GONE (a bridged
/// key skips the election before any incumbent bookkeeping, so one bridged
/// epoch evaporates it) while the family's carry is still alive.  The eventual
/// re-pin must be marked even though `is_handover` is false — otherwise the
/// full-ambiguity frame step folds against the bridge-frame carry at
/// q_pb = 1e-12 and the clock absorbs it, unmarked and uncounted.
TEST(PhaseDatumHandover, CarryRerefMarksBridgeExitRepin) {
    std::set<std::pair<std::string, std::string>> reref;
    auto c = makeCensus({{"STA_B", {"G01"}, 5}});
    Elected e;
    // No incumbent entry for G01_L1C at all — the post-bridge state.
    elect(c, e, nullptr, true, kMaxVar, &reref);
    EXPECT_EQ(e.this_epoch["G01_L1C"], "STA_B");
    EXPECT_EQ(e.tally.reattaches, 0) << "not a handover — no incumbent to hand from";
    EXPECT_TRUE(reref.count({"G01", "L1C"}))
        << "the incumbent-less re-pin must be marked regardless";
}

/// R2 (§5.09): a CONTINUOUS handover is NOT marked.  Under the sticky target the
/// successor realizes the outgoing frame for its whole tenure, so the family
/// provably does not step at T or T+1 and the carried state stays in-frame —
/// marking it would marginalize a valid fold and erase live leveling carries
/// (the measured arc truncation: 582/621 cold handovers erased 10 236 carries on
/// a 12 h zero-slip fixture).  A COLD handover in the same election still marks.
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
    led2.value_abs[{0, "G01_L1C"}] = {-12.0554, kGoodVar};   // G02 has no entry -> COLD
    elect(c2, e2, &led2, true, kMaxVar, &reref);
    ASSERT_EQ(e2.tally.handover_continuous, 1) << "precondition: G01 continuous";
    ASSERT_EQ(e2.tally.handover_cold, 1) << "precondition: G02 cold";
    EXPECT_FALSE(reref.count({"G01", "L1C"}))
        << "continuous + sticky = the frame never steps; the carry stays valid";
    EXPECT_TRUE(reref.count({"G02", "L1C"}))
        << "the cold edge in the SAME election must still mark";
}

/// R2 (§5.09): the continuous target is STICKY — re-applied every epoch the
/// successor remains incumbent.  This is the exact defect that made the unsticky
/// version measure null (§5.0): the pin reverted to 0 at T+1 and the family
/// stepped one epoch late, with the mark already spent at T.
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
    // T+1 and T+2: same incumbent, NO handover — the pin must NOT revert to 0.
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

/// A COLD handover drops any stored target: the frame re-realized at 0, so a
/// later retained epoch must keep realizing 0, not resurrect the dead frame.
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
    // B drops the key: handover B->C with NO ledger entry for C -> COLD.
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

/// A key that leaves the union drops its target with its incumbent, so its
/// eventual return is an incumbent-less pin at 0 — marked, honest, no stale
/// frame smuggled across the gap.
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

/// The null-pointer path is the OFF contract: nothing is populated, nothing
/// downstream can fire — the byte-identity leg of standing invariant #6.
/// (Every pre-1(d) case above drives the election through the default nullptr
/// and pins/tallies are asserted unchanged there; this case only pins the
/// pointer discipline itself.)
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

/// The erase drops the family at EVERY station — the frame stepped network-wide,
/// so a station-scoped erase would leave stale floats fighting the new frame at
/// every station it skipped.  An unrelated family must survive untouched.
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
/// tests/test_raim_carry_invariant_pool.cpp — the loader wants a real roster before
/// it will validate a network posture.
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

/// Default OFF — the byte-identity contract (standing invariant #6).
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

/// ARMED-BUT-INERT is the failure this rejects: the legacy datum elects no anchor,
/// so there is no handover to make continuous — it DELETES the column instead,
/// which is the more severe form of the same defect.
TEST(PhaseDatumHandoverLoader, DisabledWithoutThePrioritizedElection) {
    GnssConfig cfg;
    ASSERT_TRUE(loadNet("legacy",
                        "    \"phase_datum_handover_continuous\": true,\n", cfg));
    EXPECT_FALSE(cfg.networkPhaseDatumHandoverContinuous());
}

/// The ensemble datum holds no pin at all (its gauge is the kernel sum-to-zero
/// fold), so there is nothing to hand over.
TEST(PhaseDatumHandoverLoader, DisabledUnderTheEnsembleDatum) {
    GnssConfig cfg;
    ASSERT_TRUE(loadNet("ensemble",
                        "    \"phase_datum_ensemble\": true,\n"
                        "    \"phase_datum_handover_continuous\": true,\n", cfg));
    EXPECT_FALSE(cfg.networkPhaseDatumHandoverContinuous());
}

/// QUEUE item 1(d): default OFF — the byte-identity contract (invariant #6).
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

/// ARMED-BUT-INERT rejection, same ladder as the handover knob: the legacy datum
/// elects no anchor, so no PHASE_ANCHOR handover exists to re-reference.
TEST(PhaseDatumHandoverLoader, CarryRerefDisabledWithoutThePrioritizedElection) {
    GnssConfig cfg;
    ASSERT_TRUE(loadNet("reref_legacy",
                        "    \"phase_anchor_carry_reref\": true,\n", cfg));
    EXPECT_FALSE(cfg.networkPhaseAnchorCarryReref());
}

/// The eq34 chain owns its own eq34_reref carry hygiene — arming both would
/// double-treat the same edge, so the loader disarms this one.
TEST(PhaseDatumHandoverLoader, CarryRerefDisabledUnderTheChainDatum) {
    GnssConfig cfg;
    ASSERT_TRUE(loadNet("reref_eq34",
                        "    \"phase_datum_prioritized\": true,\n"
                        "    \"phase_datum_eq34\": true,\n"
                        "    \"phase_anchor_carry_reref\": true,\n", cfg));
    EXPECT_FALSE(cfg.networkPhaseAnchorCarryReref());
}

/// An eq34-ONLY config (prioritized not set) must still disarm, and via the
/// eq34 guard, which sits FIRST in this ladder for exactly this posture — the
/// prioritized-first order would emit the factually wrong "legacy datum elects
/// no anchor" diagnostic (review finding: eq34 elects a CHAIN datum).
TEST(PhaseDatumHandoverLoader, CarryRerefDisabledUnderEq34Alone) {
    GnssConfig cfg;
    ASSERT_TRUE(loadNet("reref_eq34only",
                        "    \"phase_datum_eq34\": true,\n"
                        "    \"phase_anchor_carry_reref\": true,\n", cfg));
    EXPECT_FALSE(cfg.networkPhaseAnchorCarryReref());
}

/// The ensemble datum elects no anchor — same disarm as the handover knob's.
TEST(PhaseDatumHandoverLoader, CarryRerefDisabledUnderTheEnsembleDatum) {
    GnssConfig cfg;
    ASSERT_TRUE(loadNet("reref_ensemble",
                        "    \"phase_datum_ensemble\": true,\n"
                        "    \"phase_anchor_carry_reref\": true,\n", cfg));
    EXPECT_FALSE(cfg.networkPhaseAnchorCarryReref());
}

/// THE PAIRING, COMPOSED (R2, §5.09).  The 2026-08-14 review disarmed this pair
/// because the unsticky ledger pin deferred the frame step to T+1 while the
/// carry mark covered T.  The R2 sticky target removes the revert and the
/// election marks only COLD re-realizations, so each mechanism treats exactly
/// its own edge — continuous handovers keep frame and carries, cold handovers
/// mark + marginalize + erase.  Both knobs must now survive the loader.
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

/// The honest-σ-scale ceiling: default OFF (0 — the absolute ceiling alone, byte-
/// identical), parses beside the absolute ceiling, and a negative ratio is refused.
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
