/**
 * @file test_gap_reentry_relevel.cpp
 * @brief Gap re-entry response: @c networkSlipDrainGapReentry, the wired invariant-#1
 *        response to an arc that re-enters across a tracking break while its stiff
 *        zero-q leveling level is still bridged
 *        (docs/SPEC_slip_qc_hardening.md §G3/W1).
 *
 * ## What is under test, and what is not
 *
 * The drain is a pure decision layer over @ref GapResumption records: it reads this
 * epoch's resumptions, applies three gates (raw level shifted, RINEX LLI on the
 * return epoch, and, behind a sub-knob, an unmeasurable re-entry), and for a slipped
 * verdict marks the ambiguities of both bands as a detected slip would.  Every test
 * drives it with hand-built resumptions, so the qualification pass that produces
 * those records is out of scope (it is covered in tests/network/test_network_slip_qc.cpp,
 * suite SlipQcQualify), and the decision logic is pinned without a network, a solve
 * or a RINEX file.
 *
 * ## State absent at drain time
 *
 * A re-entering arc has left the model set, so its ambiguity column was pruned with
 * the gap, and PASS B's measurement pre-pass re-creates it only after the PASS-A slip
 * drains.  A drain that looks for a live ambiguity (as the MW/LLI drain's
 * @c refloat_signal does) therefore matches nothing.  The drain keys on the leveling
 * carry entry instead and latches the synthesised ambiguity key.  The harness builds
 * the carry by default (it is what a re-entry threatens), and
 * @c StateAbsentAtDrainTimeIsTheCommonCaseAndStillFrees pins the field case: carry
 * in-window, no state, both bands freed on the synthesised key.
 *
 * ## Negative controls
 *
 * Four of the thirteen cases assert that nothing happens: the knob off, a clean
 * measured return, an unmeasurable return with the conservative sub-knob off, and a
 * return with no carry (or an expired one) to protect.  Freeing an arc re-levels it
 * from one epoch of code, which is the re-coupling edge of invariant #1 (the
 * metre-class level walk of docs/SPEC_slip_relevel_walk.md), so these cases bound
 * the freed population by the evidence.
 *
 * Every case also asserts the counters, not just the state: a drain that marked the
 * right arcs while reporting zeros would look the same as dead wiring in the run-end
 * line, which is the failure standing invariant #4 exists to expose.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"
#include "network/diag/RunTraces.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "network/slip/SlipQc.hpp"

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
#include "support/StationParamsOfState.hpp"   // syncStationParams
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

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

/// One station carrying a leveling entry for both bands of @ref kSat (the state a
/// re-entry threatens), optionally the live ambiguity states, and the epoch
/// scaffolding the drain needs.  Nothing here is opened for writing: the traces
/// object is constructed over a scratch path and its event stream stays closed, so
/// @c emitEvent is a no-op and the test does not touch the filesystem.
///
/// @c with_states defaults to true so the earlier cases keep testing what they were
/// written to test; the field posture is @c false (see the file banner), and it is
/// pinned explicitly below.
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
    net::ArcTable  local_carry;
    /// Continuous-GPS seconds of this epoch, and the FLD63 chain's max-gap
    /// window.  The default carry is stamped 210 s back (7 × 30 s epochs = the
    /// default `gap_epochs`), well inside the window: the state the PASS-B inject
    /// would fold across the break.
    double                     pl_now_s{271200.0};
    double                     pl_max_gap_s{900.0};
    SyncedEpoch                se;
    NetworkRunTraces           traces{gnss_test::tempDir()
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
            gnss_test::syncStationParams(stations.front());
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
        net::putArcIn(local_carry, std::make_tuple(0, std::string(kSat), std::string(sig)), &net::ArcState::amb) = lc;
    }
    void dropCarry(const char* sig) {
        net::dropArc(local_carry, std::make_tuple(0, std::string(kSat), std::string(sig)), &net::ArcState::amb);
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
    /// The closure the run-end gap re-entry line asserts (SlipQcCounters block comment).
    long long bucketSum() const {
        return counters.gap_reentry_freed_shifted + counters.gap_reentry_freed_lli
             + counters.gap_reentry_freed_unmeas + counters.gap_reentry_no_relevel
             + counters.gap_reentry_dd_only + counters.gap_reentry_cold_skipped;
    }
};

// ===========================================================================
// The slipped case: a shifted re-entry on a warm leveling carry
// ===========================================================================

// Modelled on the field defect (runs/fldf3_pl2, BSHM00ISR/G22 @ TOW 271200): the raw
// MW/GF level moved across the break, LLI was set on both bands, and no detector
// fired that epoch because none can run on a return epoch.  Both bands come out
// marked pending-reseed and latched into the free set.  The mark alone is consumed
// by the PASS-B pre-pass before the leveling inject reads it
// (SPEC_slip_relevel_walk §6), so the latch is what delivers the free epoch.
TEST(GapReentryRelevel, ShiftedReturnFreesBothBandsOnAWarmLevelingCarry) {
    Harness h;
    h.addResumption(/*measurable=*/true, /*shifted=*/true, /*lli1=*/1, /*lli2=*/1);
    h.run(/*carry_invariant=*/false, /*carry_warm=*/true, /*relevel_armed=*/true);

    EXPECT_TRUE(h.pendingReseed(kSig1));
    EXPECT_TRUE(h.pendingReseed(kSig2));
    EXPECT_TRUE(h.freed(kSig1));
    EXPECT_TRUE(h.freed(kSig2));
    // The cause tag this response is classified under.
    EXPECT_EQ(h.cause(kSig1) & RelevelEpochState::kCauseGap, RelevelEpochState::kCauseGap);
    EXPECT_EQ(h.cause(kSig2) & RelevelEpochState::kCauseGap, RelevelEpochState::kCauseGap);
    // No DD funnel insert: carry_invariant is off, so that vehicle is not live.
    EXPECT_FALSE(h.ddMarked(kSig1));
    EXPECT_EQ(h.counters.gap_reentry_dd_marked, 0);

    // Counters (invariant #4): one resumption, two arcs, both attributed to the
    // shifted gate because it is the strongest evidence present.
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

// The field case.  A real re-entering arc has no ambiguity state at drain time (the
// column was pruned with the gap, and PASS B's measurement pre-pass re-creates it
// after every PASS-A slip drain), while its leveling carry entry is alive and
// in-window, which is why the inject would bridge the break.  The response therefore
// keys off the carry and latches the synthesised ambiguity key (the format the inject
// site and the gap re-entry arc classification use); a response keyed on the live state
// would be inert on the whole population it exists for.
TEST(GapReentryRelevel, StateAbsentAtDrainTimeIsTheCommonCaseAndStillFrees) {
    Harness h(/*with_states=*/false);
    ASSERT_TRUE(h.stations.front().sm->getAmbiguities().empty());
    h.addResumption(/*measurable=*/true, /*shifted=*/true, /*lli1=*/1, /*lli2=*/1);
    h.run(/*carry_invariant=*/false, /*carry_warm=*/true, /*relevel_armed=*/true);

    // The latch is keyed on the string, so it lands even with no column yet, and the
    // PASS-B inject reads it after the pre-pass has created one.
    EXPECT_TRUE(h.freed(kSig1));
    EXPECT_TRUE(h.freed(kSig2));
    EXPECT_EQ(h.cause(kSig1) & RelevelEpochState::kCauseGap, RelevelEpochState::kCauseGap);
    EXPECT_EQ(h.cause(kSig2) & RelevelEpochState::kCauseGap, RelevelEpochState::kCauseGap);
    EXPECT_EQ(h.counters.relevel_free_marked, 2);
    // No pending-reseed mark: a mark on an absent key would sit in the set and be
    // consumed by some later epoch's pre-pass, a response acting on stale evidence.
    EXPECT_FALSE(h.pendingReseed(kSig1));
    EXPECT_FALSE(h.pendingReseed(kSig2));

    EXPECT_EQ(h.counters.gap_reentry_verdicts, 1);
    EXPECT_EQ(h.counters.gap_reentry_arcs, 2);
    EXPECT_EQ(h.counters.gap_reentry_state_absent, 2);   // the expected majority
    EXPECT_EQ(h.counters.gap_reentry_no_carry, 0);       // not counted as a missing carry
    EXPECT_EQ(h.counters.gap_reentry_freed_shifted, 2);
    EXPECT_EQ(h.counters.gap_reentry_arcs, h.bucketSum());
}

// The other half of the re-partition: with nothing to protect there is no
// response to make.  There are two ways to have nothing: no carry entry at all (a new
// arc), and an entry whose gap already exceeds phase_leveling_max_gap_s (the inject
// skips it and the arc solves free anyway).  Both land in `no_carry`, outside the
// closure, and latch nothing: freeing here would be an unneeded re-level from one
// epoch of code, the re-coupling edge of invariant #1.
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

// Under carry_invariant the vehicle is the C3c funnel, which is keyed on the same
// synthesised ambiguity string, so the re-entry reaches it with no state and with no
// leveling carry either (the DD chord test needs the carry driver, a runNetwork local
// that does not reach this seam).  Every re-entry counts as protectable there, which
// is the conservative direction.
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

// Under carry_invariant the vehicle is the C3c funnel, and the loader
// disarms relevel_free_epoch there (the leveling local carry is superseded and
// structurally empty).  The response still fires, into the funnel, and the arcs are
// counted as answered by design, not as a response that failed to land.  That
// distinction is why gap_reentry_dd_only is separate from gap_reentry_no_relevel.
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

// The interlock the loader warns about instead of disarming: on the leveling vehicle
// without relevel_free_epoch the mark cannot survive to the inject, so the arcs are
// counted as not acted on rather than reported as freed.
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

// A cold carry has nothing stale to refloat, so the verdict is diagnostic only.  This
// is the same split the earlier response makes between detections and refloats; reporting refloats
// alone makes a cold MW/LLI arm look dead (limitation L3).
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
// The LLI sub-gate: the receiver's own verdict about the break
// ===========================================================================

// A re-entry whose raw level did not clear the empirical gate but whose return epoch
// asserts LLI is still slipped: the level test is a sub-threshold test across an
// unbounded break, and the receiver reports a loss of lock.  With the sub-knob off the
// same stream leaves the arc alone.  That pair is the gate's own negative control;
// without it "freed" could just mean "the drain frees everything it is handed".
TEST(GapReentryRelevel, LliOnReturnFreesOnlyUnderItsSubKnob) {
    {
        Harness on;                                    // gap_reentry_lli defaults to true
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

// A measured, unshifted, unflagged return is the benign case the gap re-entry response counts as
// `clean_bridged` and calls "the intended behaviour": the level did not move across
// the break, so bridging the carry is correct and the response leaves it alone.  This
// test keeps the response evidence-gated rather than refloat-always (the two postures
// are compared on settled EWL at-own-integer in SPEC_lane_slip_response).
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

// An unmeasurable return (no pre-gap dual-band level, no LLI) carries no evidence at
// all.  Off by default: absence of evidence is not evidence of a slip.  Under the
// conservative sub-knob it becomes a reset, and the counter attributes it to the
// `unmeas` gate, not to `shifted`, so the run-end line separates "a jump was seen"
// from "the level could not be checked".
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

// The default-off / byte-identity contract (standing invariant #6).  With the master
// arm off the drain reads no resumption, whatever they say; the off arm of the field
// A/B rests on this property.
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
// responded to by nothing.  Both are counted so the arc ledger closes: `no_carry` is
// deliberately outside `arcs`, because an arc the inject would not bridge cannot be
// freed, cold-skipped or not-acted.
//
// The ghost satellite below counts as `no_carry` because it has no carry entry; a
// missing live ambiguity is not the criterion (see the file banner).
TEST(GapReentryRelevel, SingleBandAndNoCarryClosureIsHonest) {
    Harness h;
    // sig2 empty (single-band return): one arc only.
    h.addResumption(/*measurable=*/true, /*shifted=*/true, /*lli1=*/1, /*lli2=*/0,
                    /*dual=*/false);
    // A satellite with no carried level at this station: two signals named, and
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

// The legacy pivot's ambiguities are gauge-pinned: refloating one moves the frame
// every emitted phase bias references, which is the datum-slip re-reference path and
// not this one.  The arc is skipped and counted; an uncounted skip would read as "no
// re-entries at the datum station", which is a different fact.
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
