/**
 * @file test_gap_reentry_relevel.cpp
 * @brief Card G RESPONSE — @c networkSlipDrainGapReentry, the wired
 *        invariant-#1 answer to an arc that re-enters across a tracking break
 *        with its stiff zero-q leveling level still bridged
 *        (docs/SPEC_slip_qc_hardening.md §G3/W1).
 *
 * ## What is under test, and what is deliberately NOT
 *
 * The drain is a pure decision layer over @ref GapResumption records: it reads
 * this epoch's resumptions, applies three gates (raw level SHIFTED, RINEX LLI on
 * the return epoch, and — behind a sub-knob — an UNMEASURABLE re-entry), and for
 * a slipped verdict marks BOTH bands' ambiguities exactly as a detected slip
 * would.  Every test here drives it with hand-built resumptions, so the
 * qualification pass that PRODUCES those records is out of scope (it has its own
 * coverage in tests/test_network_slip_qc.cpp, suite SlipQcQualify) and the
 * decision logic is pinned without a network, a solve or a RINEX file.
 *
 * ## ⚠ The case the first cut got wrong (2026-09-09) — and why it is here twice
 *
 * The drain originally mirrored the MW/LLI drain's @c refloat_signal and walked
 * the station's LIVE ambiguity map for a (satellite, signal) match.  For a
 * RE-ENTRY that is the wrong question by construction: the arc left the model
 * set, so its ambiguity column was pruned with the gap, and PASS B's measurement
 * pre-pass re-creates it only AFTER the PASS-A slip drains.  The walk matched
 * nothing — a 12 h / 63-station field arm reported 396 SLIPPED verdicts, 775
 * signals with "no live ambiguity", ZERO freed, and came back BYTE-IDENTICAL to
 * its OFF control.  The old harness built the state and never built a CARRY, so
 * it tested the one configuration the field never presents.  It now builds the
 * carry by default (that is what a re-entry threatens) and
 * @c StateAbsentAtDrainTimeIsTheCommonCaseAndStillFrees pins the field's actual
 * shape: carry in-window, NO state, both bands freed on the synthesised key.
 *
 * ## The negative controls are the point
 *
 * Four of the thirteen cases assert that NOTHING happens: the knob off, a clean
 * measured return, an unmeasurable return with the conservative sub-knob off,
 * and (post-fix) a return with no carry — or an expired one — to protect.  A
 * response that frees an arc is cheap to write and expensive to be wrong
 * about — freeing re-levels the arc from ONE epoch of code, which is invariant
 * #1's own RE-COUPLING edge (the metre-class level walk of
 * docs/SPEC_slip_relevel_walk.md) — so the tests that matter most are the ones
 * proving the population is not wider than the evidence.
 *
 * Every case also asserts the COUNTERS, not just the state: a drain that marked
 * the right arcs while reporting zeros would be indistinguishable from dead
 * wiring in the run-end line, which is precisely the failure standing invariant
 * #4 exists to expose.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
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

constexpr const char* kSta = "TEST00XXX";
constexpr const char* kSat = "G22";
constexpr const char* kSig1 = "L1C";
constexpr const char* kSig2 = "L2W";

std::shared_ptr<spdlog::logger> nullLogger() {
    return std::make_shared<spdlog::logger>(
        "test_gap_reentry", std::make_shared<spdlog::sinks::null_sink_mt>());
}

/// One station carrying a leveling entry for both bands of @ref kSat — the thing
/// a re-entry actually threatens — plus (optionally) the live ambiguity states,
/// and the epoch scaffolding the drain needs.  Nothing here is opened for
/// writing: the traces object is constructed over a scratch path and its event
/// stream stays closed, so @c emitEvent is a no-op and the test does not touch
/// the filesystem.
///
/// @c with_states defaults TRUE only so the pre-existing cases keep testing what
/// they were written to test; the FIELD posture is @c false (see the file
/// banner), and it is pinned explicitly below.
struct Harness {
    LogManager                 log_manager;
    GnssConfig                 config;
    std::vector<NetStation>    stations;
    std::vector<bool>          present{true};
    std::set<std::pair<int, std::string>> carry_slipped_ambs;
    RawSlipEpoch               raw;
    SlipQcParams               params;
    SlipQcCounters             counters;
    RelevelEpochState          relevel;
    std::map<std::tuple<int, std::string, std::string>, RelevelPend> pends;
    std::map<std::tuple<int, std::string, std::string>, LocalCarry>  local_carry;
    /// Continuous-GPS seconds of this epoch, and the FLD63 chain's max-gap
    /// window.  The default carry is stamped 210 s back (7 × 30 s epochs = the
    /// default `gap_epochs`), i.e. comfortably INSIDE the window, which is
    /// exactly the state the PASS-B inject would have folded across the break.
    double                     pl_now_s{271200.0};
    double                     pl_max_gap_s{900.0};
    SyncedEpoch                se;
    NetworkRunTraces           traces{std::filesystem::temp_directory_path()
                                      / "gnss_gap_reentry_test_unused"};
    std::shared_ptr<spdlog::logger> log{nullLogger()};

    explicit Harness(bool with_states = true) {
        stations.emplace_back();
        stations.front().name = kSta;
        stations.front().sm =
            std::make_unique<StateManager>(/*core_state_size=*/5, log_manager, config);
        if (with_states) {
            stations.front().sm->addAmbiguity(StateKeys::makeAmbiguityKey(kSat, kSig1), 0.0);
            stations.front().sm->addAmbiguity(StateKeys::makeAmbiguityKey(kSat, kSig2), 0.0);
        }
        addCarry(kSig1);
        addCarry(kSig2);
        se.time = GnssTime{2313, 271200.0};
        raw.reset(1);
        params.gap_reentry = true;          // the master arm; sub-knobs at their defaults
    }

    /// Stash a leveling carry entry for @p sig, last seen @p age_s ago.
    void addCarry(const char* sig, double age_s = 210.0) {
        LocalCarry lc{};
        lc.value_abs   = 0.0;
        lc.var         = 1.0e-5;            // the field's fold_var scale
        lc.last_seen_s = pl_now_s - age_s;
        local_carry[std::make_tuple(0, std::string(kSat), std::string(sig))] = lc;
    }
    void dropCarry(const char* sig) {
        local_carry.erase(std::make_tuple(0, std::string(kSat), std::string(sig)));
    }

    /// Push one resumption for @ref kSat at station 0.
    void addResumption(bool measurable, bool shifted, int lli1, int lli2,
                       bool dual = true, long long gap_epochs = 7) {
        GapResumption g;
        g.station    = 0;
        g.satellite  = kSat;
        g.sig1       = kSig1;
        g.sig2       = dual ? kSig2 : std::string{};
        g.gap_epochs = gap_epochs;
        g.measurable = measurable;
        g.shifted    = shifted;
        g.d_mw       = shifted ? -1250.98 : 0.0031;
        g.d_gf       = shifted ? -276.67 : 0.0012;
        g.lli1       = lli1;
        g.lli2       = lli2;
        raw.resumptions.push_back(std::move(g));
    }

    /// Run the drain in the carry posture under test.
    void run(bool carry_invariant, bool carry_warm, bool relevel_armed) {
        networkSlipDrainGapReentry(stations, present, /*i_lo=*/0, /*n_sta_total=*/1,
                                   carry_invariant, carry_warm, carry_slipped_ambs,
                                   raw, params, local_carry, pl_now_s, pl_max_gap_s,
                                   counters, se, traces, log,
                                   relevel_armed ? &relevel : nullptr,
                                   relevel_armed ? &pends : nullptr);
    }

    bool pendingReseed(const char* sig) const {
        return stations.front().sm->isAmbiguityPendingReseed(
            StateKeys::makeAmbiguityKey(kSat, sig));
    }
    bool freed(const char* sig) const {
        return relevel.free_ambs.count({0, StateKeys::makeAmbiguityKey(kSat, sig)}) != 0;
    }
    bool ddMarked(const char* sig) const {
        return carry_slipped_ambs.count({0, StateKeys::makeAmbiguityKey(kSat, sig)}) != 0;
    }
    std::uint8_t cause(const char* sig) const {
        const auto it = relevel.free_cause.find(
            {0, StateKeys::makeAmbiguityKey(kSat, sig)});
        return it == relevel.free_cause.end() ? 0 : it->second;
    }
    /// The closure the run-end Card-G line asserts (SlipQcCounters block comment).
    long long bucketSum() const {
        return counters.gap_reentry_freed_shifted + counters.gap_reentry_freed_lli
             + counters.gap_reentry_freed_unmeas + counters.gap_reentry_no_relevel
             + counters.gap_reentry_dd_only + counters.gap_reentry_cold_skipped;
    }
};

// ===========================================================================
// The poison case: a SHIFTED re-entry on a warm leveling carry
// ===========================================================================

// The measured field defect (runs/fldf3_pl2, BSHM00ISR/G22 @ TOW 271200): the
// raw MW/GF level moved across the break, LLI was set on both bands, and no
// detector fired that epoch because none can run on a return epoch.  BOTH bands
// must come out marked pending-reseed AND latched into the free set — the mark
// alone is consumed by the PASS-B pre-pass before the leveling inject reads it
// (SPEC_slip_relevel_walk §6), so the latch is what actually delivers the free
// epoch.
TEST(GapReentryRelevel, ShiftedReturnFreesBothBandsOnAWarmLevelingCarry) {
    Harness h;
    h.addResumption(/*measurable=*/true, /*shifted=*/true, /*lli1=*/1, /*lli2=*/1);
    h.run(/*carry_invariant=*/false, /*carry_warm=*/true, /*relevel_armed=*/true);

    EXPECT_TRUE(h.pendingReseed(kSig1));
    EXPECT_TRUE(h.pendingReseed(kSig2));
    EXPECT_TRUE(h.freed(kSig1));
    EXPECT_TRUE(h.freed(kSig2));
    // The cause tag the lane observatory grades this response under.
    EXPECT_EQ(h.cause(kSig1) & RelevelEpochState::kCauseGap, RelevelEpochState::kCauseGap);
    EXPECT_EQ(h.cause(kSig2) & RelevelEpochState::kCauseGap, RelevelEpochState::kCauseGap);
    // No DD funnel insert: carry_invariant is off, so that vehicle is not live.
    EXPECT_FALSE(h.ddMarked(kSig1));
    EXPECT_EQ(h.counters.gap_reentry_dd_marked, 0);

    // Counters (invariant #4): one resumption, two arcs, both attributed to the
    // SHIFTED gate because it is the strongest evidence present.
    EXPECT_EQ(h.counters.gap_reentry_verdicts, 1);
    EXPECT_EQ(h.counters.gap_reentry_arcs, 2);
    EXPECT_EQ(h.counters.gap_reentry_freed_shifted, 2);
    EXPECT_EQ(h.counters.gap_reentry_freed_lli, 0);
    EXPECT_EQ(h.counters.gap_reentry_freed_unmeas, 0);
    EXPECT_EQ(h.counters.gap_reentry_cold_skipped, 0);
    EXPECT_EQ(h.counters.gap_reentry_no_relevel, 0);
    EXPECT_EQ(h.counters.gap_reentry_no_carry, 0);      // both bands carry an in-window level
    EXPECT_EQ(h.counters.gap_reentry_state_absent, 0);  // …and this harness built the states
    EXPECT_EQ(h.counters.gap_reentry_arcs, h.bucketSum());
    // The free-epoch latch's own counter, shared with the detector drains.
    EXPECT_EQ(h.counters.relevel_free_marked, 2);
}

// ⚠ THE FIELD CASE, and the one the first cut could not act on.  A real
// re-entering arc has NO ambiguity state at drain time — the column was pruned
// with the gap and PASS B's measurement pre-pass re-creates it AFTER every
// PASS-A slip drain — while its leveling CARRY entry is alive and in-window,
// which is precisely why the inject would bridge the break.  The response must
// therefore key off the carry and latch the SYNTHESISED ambiguity key (the same
// format the inject site and the Card-G arc classification use), or it is inert
// on 100 % of the population it exists for.  Measured before the fix: 396
// SLIPPED verdicts, 775 signals "with no live ambiguity", 0 freed, and a 12 h
// field A/B byte-identical to its control.
TEST(GapReentryRelevel, StateAbsentAtDrainTimeIsTheCommonCaseAndStillFrees) {
    Harness h(/*with_states=*/false);
    ASSERT_TRUE(h.stations.front().sm->getAmbiguities().empty());
    h.addResumption(/*measurable=*/true, /*shifted=*/true, /*lli1=*/1, /*lli2=*/1);
    h.run(/*carry_invariant=*/false, /*carry_warm=*/true, /*relevel_armed=*/true);

    // The latch is keyed on the string, so it lands even with no column yet —
    // and the PASS-B inject reads it after the pre-pass has created one.
    EXPECT_TRUE(h.freed(kSig1));
    EXPECT_TRUE(h.freed(kSig2));
    EXPECT_EQ(h.cause(kSig1) & RelevelEpochState::kCauseGap, RelevelEpochState::kCauseGap);
    EXPECT_EQ(h.cause(kSig2) & RelevelEpochState::kCauseGap, RelevelEpochState::kCauseGap);
    EXPECT_EQ(h.counters.relevel_free_marked, 2);
    // No pending-reseed mark: a mark on an absent key would sit in the set and
    // be consumed by some LATER epoch's pre-pass — a response acting on history.
    EXPECT_FALSE(h.pendingReseed(kSig1));
    EXPECT_FALSE(h.pendingReseed(kSig2));

    EXPECT_EQ(h.counters.gap_reentry_verdicts, 1);
    EXPECT_EQ(h.counters.gap_reentry_arcs, 2);
    EXPECT_EQ(h.counters.gap_reentry_state_absent, 2);   // the expected majority
    EXPECT_EQ(h.counters.gap_reentry_no_carry, 0);       // ⚠ NOT the old `no_arc` verdict
    EXPECT_EQ(h.counters.gap_reentry_freed_shifted, 2);
    EXPECT_EQ(h.counters.gap_reentry_arcs, h.bucketSum());
}

// The other half of the re-partition: with nothing to protect there is no
// response to make.  Two ways to have nothing — no carry entry at all (a truly
// new arc), and an entry whose gap already exceeds phase_leveling_max_gap_s (THE
// CLIFF: the inject skips it and the arc solves free anyway).  Both must land in
// `no_carry`, OUTSIDE the closure, and latch nothing: freeing here would be a
// gratuitous re-level from one epoch of code, which is invariant #1's own
// RE-COUPLING edge.
TEST(GapReentryRelevel, NoCarryAndExpiredCarryAreBothLeftAlone) {
    {
        Harness none(/*with_states=*/false);
        none.dropCarry(kSig1);
        none.dropCarry(kSig2);
        none.addResumption(/*measurable=*/true, /*shifted=*/true, /*lli1=*/1, /*lli2=*/1);
        none.run(/*carry_invariant=*/false, /*carry_warm=*/true, /*relevel_armed=*/true);

        EXPECT_EQ(none.counters.gap_reentry_verdicts, 1);   // still judged slipped…
        EXPECT_EQ(none.counters.gap_reentry_arcs, 0);       // …but nothing to protect
        EXPECT_EQ(none.counters.gap_reentry_no_carry, 2);
        EXPECT_EQ(none.counters.gap_reentry_state_absent, 0);
        EXPECT_TRUE(none.relevel.free_ambs.empty());
        EXPECT_EQ(none.counters.relevel_free_marked, 0);
        EXPECT_EQ(none.bucketSum(), 0);
    }
    {
        Harness stale(/*with_states=*/false);
        stale.addCarry(kSig1, /*age_s=*/1200.0);            // > pl_max_gap_s (900 s)
        stale.addCarry(kSig2, /*age_s=*/1200.0);
        stale.addResumption(/*measurable=*/true, /*shifted=*/true, /*lli1=*/1, /*lli2=*/1);
        stale.run(/*carry_invariant=*/false, /*carry_warm=*/true, /*relevel_armed=*/true);

        EXPECT_EQ(stale.counters.gap_reentry_arcs, 0);
        EXPECT_EQ(stale.counters.gap_reentry_no_carry, 2);
        EXPECT_TRUE(stale.relevel.free_ambs.empty());
        EXPECT_EQ(stale.bucketSum(), 0);
    }
}

// Under carry_invariant the vehicle is the C3c funnel, which is keyed on the
// SAME synthesised ambiguity string — so the re-entry must reach it with no
// state and (since the DD chord test needs the carry driver, a runNetwork local
// that does not reach this seam) with no leveling carry either: every re-entry
// is protectable there, the conservative direction.
TEST(GapReentryRelevel, CarryInvariantFunnelTakesTheSynthesisedKeyWithNoState) {
    Harness h(/*with_states=*/false);
    h.dropCarry(kSig1);
    h.dropCarry(kSig2);
    h.addResumption(/*measurable=*/true, /*shifted=*/true, /*lli1=*/0, /*lli2=*/0);
    h.run(/*carry_invariant=*/true, /*carry_warm=*/true, /*relevel_armed=*/false);

    EXPECT_TRUE(h.ddMarked(kSig1));
    EXPECT_TRUE(h.ddMarked(kSig2));
    EXPECT_EQ(h.counters.gap_reentry_dd_marked, 2);
    EXPECT_EQ(h.counters.gap_reentry_arcs, 2);
    EXPECT_EQ(h.counters.gap_reentry_no_carry, 0);       // carry_invariant ⇒ protectable
    EXPECT_EQ(h.counters.gap_reentry_state_absent, 2);
    EXPECT_EQ(h.counters.gap_reentry_dd_only, 2);
    EXPECT_EQ(h.counters.gap_reentry_arcs, h.bucketSum());
}

// Under carry_invariant the vehicle is the Card-4 C3c funnel, and the loader
// disarms relevel_free_epoch there (the leveling local carry is superseded and
// structurally empty).  The response must still fire — into the funnel — and the
// arcs must be counted as answered BY DESIGN, not as a response that failed to
// land.  That distinction is the whole reason gap_reentry_dd_only is separate
// from gap_reentry_no_relevel.
TEST(GapReentryRelevel, CarryInvariantRoutesToTheC3cFunnelAndIsNotAFailedResponse) {
    Harness h;
    h.addResumption(/*measurable=*/true, /*shifted=*/true, /*lli1=*/0, /*lli2=*/0);
    h.run(/*carry_invariant=*/true, /*carry_warm=*/true, /*relevel_armed=*/false);

    EXPECT_TRUE(h.ddMarked(kSig1));
    EXPECT_TRUE(h.ddMarked(kSig2));
    EXPECT_EQ(h.counters.gap_reentry_dd_marked, 2);
    EXPECT_EQ(h.counters.gap_reentry_dd_only, 2);
    EXPECT_EQ(h.counters.gap_reentry_no_relevel, 0);
    EXPECT_EQ(h.counters.gap_reentry_freed_shifted, 0);
    EXPECT_EQ(h.counters.gap_reentry_arcs, h.bucketSum());
    // The pending-reseed mark is still set (harmless there) but nothing was
    // latched, because there is no latch.
    EXPECT_TRUE(h.relevel.free_ambs.empty());
}

// The interlock the loader WARNS about instead of disarming: on the leveling
// vehicle without relevel_free_epoch the mark cannot survive to the inject, so
// the arcs must be counted as NOT ACTED rather than silently reported as freed.
TEST(GapReentryRelevel, LevelingVehicleWithoutTheFreeEpochLatchCountsTheLoss) {
    Harness h;
    h.addResumption(/*measurable=*/true, /*shifted=*/true, /*lli1=*/0, /*lli2=*/0);
    h.run(/*carry_invariant=*/false, /*carry_warm=*/true, /*relevel_armed=*/false);

    EXPECT_EQ(h.counters.gap_reentry_no_relevel, 2);
    EXPECT_EQ(h.counters.gap_reentry_dd_only, 0);
    EXPECT_EQ(h.counters.gap_reentry_freed_shifted, 0);
    EXPECT_EQ(h.counters.gap_reentry_arcs, h.bucketSum());
    EXPECT_EQ(h.counters.relevel_free_marked, 0);
}

// A cold carry has nothing stale to refloat, so the verdict is DIAGNOSTIC — the
// same split Card E had to make when reporting only refloats made the MW/LLI arm
// look dead (limitation L3).
TEST(GapReentryRelevel, ColdCarryCountsTheDetectionWithoutActing) {
    Harness h;
    h.addResumption(/*measurable=*/true, /*shifted=*/true, /*lli1=*/1, /*lli2=*/1);
    h.run(/*carry_invariant=*/false, /*carry_warm=*/false, /*relevel_armed=*/true);

    EXPECT_EQ(h.counters.gap_reentry_verdicts, 1);
    EXPECT_EQ(h.counters.gap_reentry_arcs, 2);
    EXPECT_EQ(h.counters.gap_reentry_cold_skipped, 2);
    EXPECT_EQ(h.counters.gap_reentry_freed_shifted, 0);
    EXPECT_EQ(h.counters.gap_reentry_arcs, h.bucketSum());
    EXPECT_FALSE(h.pendingReseed(kSig1));
    EXPECT_FALSE(h.pendingReseed(kSig2));
    EXPECT_TRUE(h.relevel.free_ambs.empty());
    EXPECT_EQ(h.counters.relevel_free_marked, 0);
}

// ===========================================================================
// The LLI sub-gate — the receiver's own verdict about the break
// ===========================================================================

// A re-entry whose raw level did NOT clear the empirical gate but whose return
// epoch asserts LLI is still slipped: the level test is a *sub-threshold* test
// across an unbounded break, and the receiver is telling us it lost lock.  With
// the sub-knob off the same stream must leave the arc alone — that pair is the
// gate's own negative control, and without it "freed" could just mean "the drain
// frees everything it is handed".
TEST(GapReentryRelevel, LliOnReturnFreesOnlyUnderItsSubKnob) {
    {
        Harness on;                                    // gap_reentry_lli defaults TRUE
        ASSERT_TRUE(on.params.gap_reentry_lli);
        on.addResumption(/*measurable=*/true, /*shifted=*/false, /*lli1=*/1, /*lli2=*/0);
        on.run(false, true, true);
        EXPECT_TRUE(on.freed(kSig1));
        EXPECT_TRUE(on.freed(kSig2));                  // whole-satellite: both bands
        EXPECT_EQ(on.counters.gap_reentry_freed_lli, 2);
        EXPECT_EQ(on.counters.gap_reentry_freed_shifted, 0);
        EXPECT_EQ(on.counters.gap_reentry_arcs, on.bucketSum());
    }
    {
        Harness off;
        off.params.gap_reentry_lli = false;
        off.addResumption(/*measurable=*/true, /*shifted=*/false, /*lli1=*/1, /*lli2=*/0);
        off.run(false, true, true);
        EXPECT_FALSE(off.pendingReseed(kSig1));
        EXPECT_TRUE(off.relevel.free_ambs.empty());
        EXPECT_EQ(off.counters.gap_reentry_verdicts, 0);
        EXPECT_EQ(off.counters.gap_reentry_arcs, 0);
    }
}

// ===========================================================================
// The two negative controls that bound the population
// ===========================================================================

// A MEASURED, unshifted, unflagged return is the benign case Card G counts as
// `clean_bridged` and calls "the intended behaviour": the level demonstrably did
// NOT move across the break, so bridging the carry is correct and the response
// must not touch it.  This is the test that keeps the response from degenerating
// into refloat-always — the posture SPEC_lane_slip_response measured 16 points
// worse on settled EWL at-own-integer than an evidence-gated keep.
TEST(GapReentryRelevel, CleanMeasuredReturnIsLeftBridged) {
    Harness h;
    h.addResumption(/*measurable=*/true, /*shifted=*/false, /*lli1=*/0, /*lli2=*/0);
    h.run(/*carry_invariant=*/false, /*carry_warm=*/true, /*relevel_armed=*/true);

    EXPECT_FALSE(h.pendingReseed(kSig1));
    EXPECT_FALSE(h.pendingReseed(kSig2));
    EXPECT_TRUE(h.relevel.free_ambs.empty());
    EXPECT_TRUE(h.carry_slipped_ambs.empty());
    EXPECT_EQ(h.counters.gap_reentry_verdicts, 0);
    EXPECT_EQ(h.counters.gap_reentry_arcs, 0);
    EXPECT_EQ(h.bucketSum(), 0);
}

// An UNMEASURABLE return (no pre-gap dual-band level, no LLI) carries no
// evidence at all.  Default OFF: absence of evidence is never evidence of a
// slip.  Under the conservative sub-knob it becomes a reset — and the counter
// attributes it to the `unmeas` gate, not to `shifted`, so the run-end line can
// separate "we saw a jump" from "we could not check".
TEST(GapReentryRelevel, UnmeasurableReturnActsOnlyUnderTheConservativeSubKnob) {
    {
        Harness off;                                   // default
        ASSERT_FALSE(off.params.gap_reentry_unmeasurable);
        off.addResumption(/*measurable=*/false, /*shifted=*/false, /*lli1=*/0, /*lli2=*/0);
        off.run(false, true, true);
        EXPECT_EQ(off.counters.gap_reentry_verdicts, 0);
        EXPECT_EQ(off.counters.gap_reentry_arcs, 0);
        EXPECT_TRUE(off.relevel.free_ambs.empty());
    }
    {
        Harness on;
        on.params.gap_reentry_unmeasurable = true;
        on.addResumption(/*measurable=*/false, /*shifted=*/false, /*lli1=*/0, /*lli2=*/0);
        on.run(false, true, true);
        EXPECT_TRUE(on.freed(kSig1));
        EXPECT_TRUE(on.freed(kSig2));
        EXPECT_EQ(on.counters.gap_reentry_freed_unmeas, 2);
        EXPECT_EQ(on.counters.gap_reentry_freed_shifted, 0);
        EXPECT_EQ(on.counters.gap_reentry_freed_lli, 0);
        EXPECT_EQ(on.counters.gap_reentry_arcs, on.bucketSum());
    }
}

// The default-OFF / byte-identity contract (standing invariant #6).  With the
// master arm down the drain must not read a single resumption, whatever they
// say — this is the property the OFF arm of the field A/B rests on.
TEST(GapReentryRelevel, MasterArmOffTouchesNothing) {
    Harness h;
    h.params.gap_reentry = false;
    h.addResumption(/*measurable=*/true, /*shifted=*/true, /*lli1=*/1, /*lli2=*/1);
    h.run(/*carry_invariant=*/true, /*carry_warm=*/true, /*relevel_armed=*/true);

    EXPECT_FALSE(h.pendingReseed(kSig1));
    EXPECT_FALSE(h.pendingReseed(kSig2));
    EXPECT_TRUE(h.relevel.free_ambs.empty());
    EXPECT_TRUE(h.relevel.free_cause.empty());
    EXPECT_TRUE(h.carry_slipped_ambs.empty());
    EXPECT_EQ(h.counters.gap_reentry_verdicts, 0);
    EXPECT_EQ(h.counters.gap_reentry_arcs, 0);
    EXPECT_EQ(h.counters.gap_reentry_no_carry, 0);
    EXPECT_EQ(h.counters.gap_reentry_state_absent, 0);
    EXPECT_EQ(h.counters.gap_reentry_dd_marked, 0);
    EXPECT_EQ(h.counters.relevel_free_marked, 0);
    EXPECT_EQ(h.bucketSum(), 0);
}

// ===========================================================================
// Structural edges
// ===========================================================================

// A single-band return has no sig2, and a satellite with no carried level can be
// responded to by nothing.  Both are counted so the arc ledger closes:
// `no_carry` is deliberately OUTSIDE `arcs`, because an arc the inject would not
// bridge cannot be freed, cold-skipped or not-acted.
//
// ⚠ Semantics changed with the 2026-09-09 wiring fix: the ghost below used to
// qualify as `no_arc` because it had no live AMBIGUITY; it now qualifies as
// `no_carry` because it has no CARRY.  The numbers are the same and the test
// still bounds the population, but it is now testing the right absence.
TEST(GapReentryRelevel, SingleBandAndNoCarryClosureIsHonest) {
    Harness h;
    // sig2 empty (single-band return) — one arc only.
    h.addResumption(/*measurable=*/true, /*shifted=*/true, /*lli1=*/1, /*lli2=*/0,
                    /*dual=*/false);
    // A satellite with no carried level at this station — two signals named,
    // nothing the PASS-B inject could bridge.
    GapResumption ghost;
    ghost.station = 0;
    ghost.satellite = "G31";
    ghost.sig1 = kSig1;
    ghost.sig2 = kSig2;
    ghost.gap_epochs = 3;
    ghost.measurable = true;
    ghost.shifted = true;
    h.raw.resumptions.push_back(ghost);

    h.run(/*carry_invariant=*/false, /*carry_warm=*/true, /*relevel_armed=*/true);

    EXPECT_EQ(h.counters.gap_reentry_verdicts, 2);   // both re-entries were slipped
    EXPECT_EQ(h.counters.gap_reentry_arcs, 1);       // only kSat's carried L1C arc
    EXPECT_EQ(h.counters.gap_reentry_no_carry, 2);   // G31's two named signals
    EXPECT_EQ(h.counters.gap_reentry_freed_shifted, 1);
    EXPECT_EQ(h.counters.gap_reentry_arcs, h.bucketSum());
    EXPECT_TRUE(h.freed(kSig1));
    EXPECT_FALSE(h.freed(kSig2));                    // never named by the resumption
}

// The legacy pivot's ambiguities are GAUGE-PINNED: refloating one moves the
// frame every emitted phase bias references, which is the datum-slip
// re-reference path and not this one.  Skipped and COUNTED — a silent skip would
// read as "no re-entries at the datum station", which is a different fact.
TEST(GapReentryRelevel, GaugePinnedPivotIsSkippedAndCounted) {
    Harness h;
    h.addResumption(/*measurable=*/true, /*shifted=*/true, /*lli1=*/1, /*lli2=*/1);
    // i_lo = 1 is the legacy-pivot posture; the resumption sits at station 0.
    networkSlipDrainGapReentry(h.stations, h.present, /*i_lo=*/1, /*n_sta_total=*/1,
                               /*carry_invariant=*/false, /*carry_warm=*/true,
                               h.carry_slipped_ambs, h.raw, h.params,
                               h.local_carry, h.pl_now_s, h.pl_max_gap_s,
                               h.counters, h.se, h.traces, h.log, &h.relevel, &h.pends);

    EXPECT_EQ(h.counters.gap_reentry_verdicts, 1);
    EXPECT_EQ(h.counters.gap_reentry_pivot_skipped, 1);
    EXPECT_EQ(h.counters.gap_reentry_arcs, 0);
    EXPECT_FALSE(h.pendingReseed(kSig1));
    EXPECT_TRUE(h.relevel.free_ambs.empty());
}

} // namespace
} // namespace GnssProcessor
