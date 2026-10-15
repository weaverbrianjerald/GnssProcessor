/**
 * @file test_gap_reentry_coresumed.cpp
 * @brief The co-resumed share of the per-signal gap re-entry tier
 *        (network.cycle_slip.gap_reentry_coresumed_share).
 *
 * The case under test is the measured field case (three-band GPS, MAJU00MHL G08 at
 * TOW 260040): L1C and L2W drop for one or two epochs while L5Q tracks through, and
 * at the return L1C and L5Q step together (a channel delay change at
 * re-acquisition).  The per-signal tier differences each returning signal against a
 * partner that tracked through, so L1C reads clean against L5Q (d_gf 0.026 m) and
 * its carry is bridged, while L2W reads shifted (d_gf 11.98 m) and is freed.  The
 * share treats the signals of one (station, satellite) that resumed at the same
 * epoch as one re-acquisition, so a verdict on L2W frees L1C as well; L5Q, which
 * tracked through, is never in the group.
 *
 * Every case drives the response directly over a synthetic @c RawSlipEpoch: no
 * compute pass, no solve.  The station, carry and relevel scaffolding is the shape
 * of tests/test_tracker_modeled_first.cpp (duplicated rather than shared so that a
 * change to one file's fixture does not affect the other).  The event stream is
 * opened on a scratch file so the `reentry_relevel` rows can be read back.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "filter/ArcLedger.hpp"
#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "slip/NetworkRawObservables.hpp"
#include "slip/NetworkSlipQc.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace GnssProcessor {
namespace {

constexpr const char* kSta  = "MAJU00MHL";
constexpr const char* kSta2 = "KOKB00USA";
constexpr const char* kSat  = "G08";

/// One or two stations, each with a leveling carry on the three bands of @ref kSat,
/// a warm carry with the free-epoch latch (each switchable), and the event stream
/// opened on a scratch file.
struct Harness {
    LogManager              log_manager;
    GnssConfig              config;
    std::vector<NetStation> stations;
    std::vector<bool>       present;
    std::set<std::pair<int, std::string>> carry_slipped_ambs;
    RawSlipEpoch            raw;
    SlipQcParams            params;
    SlipQcCounters          counters;
    RelevelEpochState       relevel;
    std::map<std::tuple<int, std::string, std::string>, RelevelPend> pends;
    net::ArcTable  local_carry;
    ArcReentryEpoch         sid;
    ArcReentryOutcomeMap    outcome;
    int                     i_lo{ 0 };               ///< gauge-pinned stations are those below it.
    bool                    carry_warm{ true };
    bool                    carry_invariant{ false };
    bool                    use_relevel{ true };     ///< relevel_free_epoch armed.
    double                  pl_now_s{ 260040.0 };
    double                  pl_max_gap_s{ 900.0 };
    SyncedEpoch             se;
    std::filesystem::path   events_csv;
    NetworkRunTraces        traces{ std::filesystem::temp_directory_path()
                                    / "gnss_gap_reentry_coresumed_test_unused" };
    std::shared_ptr<spdlog::logger> log{ std::make_shared<spdlog::logger>(
        "test_gap_reentry_coresumed", std::make_shared<spdlog::sinks::null_sink_mt>()) };

    /// @param share       network.cycle_slip.gap_reentry_coresumed_share.
    /// @param tag         Distinguishes the scratch event file of two harnesses in one test.
    /// @param n_stations  1 (@ref kSta) or 2 (@ref kSta, then @ref kSta2).
    explicit Harness(bool share, const std::string& tag = "a", int n_stations = 1) {
        for (int k = 0; k < n_stations; ++k) {
            stations.emplace_back();
            stations.back().name = (k == 0) ? kSta : kSta2;
            stations.back().sm =
                std::make_unique<StateManager>(/*core_state_size=*/5, log_manager, config);
            present.push_back(true);
            for (const char* s : { "L1C", "L2W", "L5Q" }) addCarry(kSat, s, k);
        }
        se.time = GnssTime{ 2313, 260040.0 };
        raw.reset(static_cast<std::size_t>(n_stations));
        // The production posture of the share: the row-level response and its
        // per-signal rider, both armed.
        params.gap_reentry                 = true;
        params.gap_reentry_per_signal      = true;
        params.gap_reentry_coresumed_share = share;
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        events_csv = std::filesystem::temp_directory_path()
                   / (std::string("gnss_gap_reentry_coresumed_") + info->name() + "_"
                      + tag + ".csv");
        traces.events_trace.open(events_csv, std::ios::trunc);
    }

    void addCarry(const char* sat, const char* sig, int station = 0) {
        LocalCarry lc{};
        lc.var         = 1.0e-5;
        lc.last_seen_s = pl_now_s - 60.0;   // inside the max-gap window: protectable
        net::putArcIn(local_carry, std::make_tuple(station, std::string(sat), std::string(sig)), &net::ArcState::amb) = lc;
    }

    /// One per-signal resumption of (@p station, @p sat, @p signal), judged against
    /// @p partner.
    void addSigAt(int station, const char* sat, const char* signal, const char* partner,
                  bool shifted, double d_gf, int lli = 0, bool measurable = true) {
        SigResumption g;
        g.station         = station;
        g.satellite       = sat;
        g.signal          = signal;
        g.partner         = measurable ? partner : "";
        g.partner_through = measurable;
        g.gap_epochs      = 2;
        g.measurable      = measurable;
        g.shifted         = shifted;
        g.d_gf            = measurable ? d_gf : 0.0;
        g.gate_gf         = 0.04;
        g.lli             = lli;
        raw.sig_resumptions.push_back(std::move(g));
    }
    /// @ref addSigAt at station 0.
    void addSig(const char* sat, const char* signal, const char* partner, bool shifted,
                double d_gf, int lli = 0, bool measurable = true) {
        addSigAt(0, sat, signal, partner, shifted, d_gf, lli, measurable);
    }

    /// The measured field case at @p station, in the order the raw pass lists it
    /// (the row's signal order): L1C clean against L5Q, then L2W shifted against
    /// L5Q.  L5Q tracked through and has no resumption.
    void fieldCase(int station = 0) {
        addSigAt(station, kSat, "L1C", "L5Q", /*shifted=*/false, 0.026);
        addSigAt(station, kSat, "L2W", "L5Q", /*shifted=*/true, 11.98);
    }

    /// A ledger arc of (@p station, @ref kSat, @p sig) returning after two epochs.
    void addLedgerArc(const char* sig, int station = 0) {
        ArcReentryArc a;
        a.station   = station;
        a.satellite = kSat;
        a.signal    = sig;
        a.gap_ep    = 2;
        a.gap_s     = 60.0;
        sid.arcs.push_back(a);
    }

    void runRowLevel() {
        networkSlipDrainGapReentry(stations, present, i_lo,
                                   static_cast<int>(stations.size()), carry_invariant,
                                   carry_warm, carry_slipped_ambs, raw, params,
                                   local_carry, pl_now_s, pl_max_gap_s, counters, se,
                                   traces, log, use_relevel ? &relevel : nullptr, &pends,
                                   sid.armed ? &sid : nullptr, &outcome,
                                   /*arc_ledger=*/nullptr);
    }
    void runPerSignal() {
        networkSlipDrainGapReentryPerSignal(stations, present, i_lo,
                                            static_cast<int>(stations.size()),
                                            carry_invariant, carry_warm,
                                            carry_slipped_ambs, raw, params, local_carry,
                                            pl_now_s, pl_max_gap_s, counters, se, traces,
                                            log, use_relevel ? &relevel : nullptr, &pends,
                                            sid.armed ? &sid : nullptr, &outcome);
    }

    bool freed(const char* sig, const char* sat = kSat, int station = 0) const {
        return relevel.free_ambs.count(
                   { station, StateKeys::makeAmbiguityKey(sat, sig) }) != 0;
    }

    /// The `reentry_relevel` rows written so far.
    std::vector<std::string> relevelEvents() {
        traces.events_trace.flush();
        std::vector<std::string> out;
        std::ifstream f(events_csv);
        for (std::string line; std::getline(f, line);)
            if (line.find(",reentry_relevel,") != std::string::npos) out.push_back(line);
        return out;
    }
    /// The one `reentry_relevel` row of @p station whose detail starts with @p sig,
    /// or "".
    std::string relevelEventOf(const std::string& sig, const std::string& station = kSta) {
        for (const auto& line : relevelEvents())
            if (line.find("," + station + ",") != std::string::npos
                && line.find("," + sig + ";") != std::string::npos)
                return line;
        return {};
    }

    /// The tier's own arc closure (GapReentryLedger).
    bool tierClosureHolds() const {
        const auto& s = counters.gap_sig;
        return s.arcs == s.freed_shifted + s.freed_lli + s.freed_unmeas
                       + s.no_relevel + s.dd_only + s.cold_skipped;
    }
    /// The share's own closure, in signals (CoresumedShareLedger).
    bool shareClosureHolds() const {
        const auto& s = counters.gap_sig_share;
        return s.signals == s.already_freed + s.skipped + s.no_carry + s.freed_shifted
                            + s.freed_lli + s.freed_unmeas + s.cold_skipped
                            + s.no_relevel + s.dd_only;
    }
    long long shareFreed() const {
        const auto& s = counters.gap_sig_share;
        return s.freed_shifted + s.freed_lli + s.freed_unmeas;
    }
};

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ===========================================================================
// The field case, both states
// ===========================================================================

// With the knob off nothing changes: the shifted L2W is freed on its own verdict,
// the clean L1C is bridged, and no share counter moves.
TEST(GapReentryCoresumedTest, KnobOffFreesOnlyTheShiftedSignal) {
    Harness h(/*share=*/false);
    ASSERT_FALSE(h.params.coresumedShareActive());
    h.fieldCase();
    h.runPerSignal();

    EXPECT_TRUE(h.freed("L2W"));
    EXPECT_FALSE(h.freed("L1C"));
    EXPECT_FALSE(h.freed("L5Q"));
    EXPECT_EQ(h.relevel.free_ambs.size(), 1u);
    EXPECT_EQ(h.counters.gap_sig.verdicts, 1);
    EXPECT_EQ(h.counters.gap_sig.arcs, 1);
    EXPECT_EQ(h.counters.gap_sig.freed_shifted, 1);
    EXPECT_TRUE(h.tierClosureHolds());
    const auto& sh = h.counters.gap_sig_share;
    EXPECT_EQ(sh.los_coresumed, 0);
    EXPECT_EQ(sh.los_answered, 0);
    EXPECT_EQ(sh.signals, 0);
    EXPECT_EQ(h.shareFreed(), 0);

    const auto ev = h.relevelEvents();
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_TRUE(contains(ev[0], "L2W;scope=signal;"));
    EXPECT_FALSE(contains(ev[0], "los_shared"));
}

// Armed, the clean L1C takes L2W's verdict and is freed through the same response:
// bucketed under the trigger's primary cause (so the tier's closure holds), counted
// in the share's own ledger, and traced with scope=los_shared naming the trigger.
// The trigger itself keeps its scope=signal row.
TEST(GapReentryCoresumedTest, KnobOnFreesTheCleanCoresumedSibling) {
    Harness h(/*share=*/true);
    ASSERT_TRUE(h.params.coresumedShareActive());
    h.fieldCase();
    h.runPerSignal();

    EXPECT_TRUE(h.freed("L2W"));
    EXPECT_TRUE(h.freed("L1C"));
    EXPECT_EQ(h.counters.gap_sig.verdicts, 2);
    EXPECT_EQ(h.counters.gap_sig.arcs, 2);
    EXPECT_EQ(h.counters.gap_sig.freed_shifted, 2);
    EXPECT_EQ(h.counters.relevel_free_marked, 2);
    EXPECT_TRUE(h.tierClosureHolds());
    const auto& sh = h.counters.gap_sig_share;
    EXPECT_EQ(sh.los_coresumed, 1);
    EXPECT_EQ(sh.los_answered, 1);
    EXPECT_EQ(sh.los_trigger_uncarried, 0);   // L2W is a carried signal
    EXPECT_EQ(sh.signals, 1);
    EXPECT_EQ(sh.freed_shifted, 1);           // under the trigger's cause
    EXPECT_EQ(sh.freed_lli + sh.freed_unmeas, 0);
    EXPECT_EQ(sh.already_freed + sh.skipped + sh.no_carry, 0);
    EXPECT_TRUE(h.shareClosureHolds());

    const auto ev = h.relevelEvents();
    ASSERT_EQ(ev.size(), 2u);
    const std::string l1c = h.relevelEventOf("L1C");
    ASSERT_FALSE(l1c.empty());
    EXPECT_TRUE(contains(l1c, "L1C;scope=los_shared;"));
    EXPECT_TRUE(contains(l1c, ";partner=L5Q;"));
    EXPECT_TRUE(contains(l1c, ";dGF=0.0260;"));       // its own test, which read clean
    EXPECT_TRUE(contains(l1c, ";cause=shared;"));
    EXPECT_TRUE(contains(l1c, ";trigger=L2W;"));
    EXPECT_TRUE(contains(l1c, ";trigger_cause=shifted;"));
    EXPECT_TRUE(contains(l1c, ";trigger_dGF=11.9800;"));
    EXPECT_TRUE(contains(l1c, ",refloat,"));          // the action of a warm, latched free
    const std::string l2w = h.relevelEventOf("L2W");
    ASSERT_FALSE(l2w.empty());
    EXPECT_TRUE(contains(l2w, "L2W;scope=signal;"));
    EXPECT_TRUE(contains(l2w, ";cause=shifted;"));
}

// ===========================================================================
// What the share never frees
// ===========================================================================

// A group with no verdict is bridged in both states: the share adds a verdict only
// where one co-resumed signal already has its own.
TEST(GapReentryCoresumedTest, CleanOnlyGroupFreesNothingInEitherState) {
    for (const bool share : { false, true }) {
        Harness h(share, share ? "on" : "off");
        h.addSig(kSat, "L1C", "L5Q", /*shifted=*/false, 0.026);
        h.addSig(kSat, "L2W", "L5Q", /*shifted=*/false, 0.031);
        h.runPerSignal();
        const auto& sh = h.counters.gap_sig_share;
        EXPECT_TRUE(h.relevel.free_ambs.empty()) << "share=" << share;
        EXPECT_EQ(h.counters.gap_sig.verdicts, 0) << "share=" << share;
        EXPECT_EQ(h.counters.gap_sig.arcs, 0) << "share=" << share;
        EXPECT_EQ(sh.los_answered, 0) << "share=" << share;
        EXPECT_EQ(sh.signals, 0) << "share=" << share;
        EXPECT_TRUE(h.relevelEvents().empty()) << "share=" << share;
        // The population is still counted when armed, so a zero share reads as
        // "nothing mixed", not as dead wiring.
        EXPECT_EQ(sh.los_coresumed, share ? 1 : 0);
    }
}

// L5Q tracked through the gap, so it has no resumption and is not co-resumed: the
// share never frees it, although its carry is protectable.  A clean return on
// another line of sight at the same station is not shared either.
TEST(GapReentryCoresumedTest, TrackedThroughPartnerAndOtherLosAreNeverFreed) {
    Harness h(/*share=*/true);
    h.addCarry("G09", "L1C");
    h.fieldCase();
    h.addSig("G09", "L1C", "L5Q", /*shifted=*/false, 0.012);
    h.runPerSignal();

    EXPECT_TRUE(h.freed("L1C"));
    EXPECT_TRUE(h.freed("L2W"));
    EXPECT_FALSE(h.freed("L5Q"));               // tracked through: never in the group
    EXPECT_FALSE(h.freed("L1C", "G09"));        // a different line of sight, all clean
    EXPECT_EQ(h.relevel.free_ambs.size(), 2u);
    EXPECT_EQ(h.counters.gap_sig_share.los_coresumed, 1);   // G09 has one resumption only
    EXPECT_EQ(h.counters.gap_sig_share.signals, 1);
}

// The group key is the line of sight, station included: the same satellite at a
// second station, re-acquired at the same epoch with its own all-clean co-resumed
// pair, is a different group and stays bridged.
TEST(GapReentryCoresumedTest, SameSatelliteAtAnotherStationIsNotShared) {
    Harness h(/*share=*/true, "a", /*n_stations=*/2);
    h.fieldCase(/*station=*/0);
    h.addSigAt(1, kSat, "L1C", "L2W", /*shifted=*/false, 0.011);
    h.addSigAt(1, kSat, "L5Q", "L2W", /*shifted=*/false, 0.014);
    h.runPerSignal();

    EXPECT_TRUE(h.freed("L1C", kSat, 0));
    EXPECT_TRUE(h.freed("L2W", kSat, 0));
    EXPECT_FALSE(h.freed("L1C", kSat, 1));
    EXPECT_FALSE(h.freed("L5Q", kSat, 1));
    EXPECT_EQ(h.relevel.free_ambs.size(), 2u);
    const auto& sh = h.counters.gap_sig_share;
    EXPECT_EQ(sh.los_coresumed, 2);
    EXPECT_EQ(sh.los_answered, 1);
    EXPECT_EQ(sh.signals, 1);
    EXPECT_TRUE(h.relevelEventOf("L1C", kSta2).empty());
    EXPECT_TRUE(h.shareClosureHolds());
}

// ===========================================================================
// The other verdicts, and the gates
// ===========================================================================

// An LLI verdict (gap_reentry_lli, default on) and an unmeasurable one
// (gap_reentry_per_signal_unmeasurable, default on) trigger the share like a level
// shift, and the shared free lands in the trigger's bucket.  With either sub-gate
// off the same evidence is no verdict, and nothing is shared.
TEST(GapReentryCoresumedTest, LliAndUnmeasurableVerdictsTriggerTheShare) {
    {
        Harness h(/*share=*/true, "lli");
        h.addSig(kSat, "L1C", "L5Q", /*shifted=*/false, 0.026);
        h.addSig(kSat, "L2W", "L5Q", /*shifted=*/false, 0.020, /*lli=*/1);
        h.runPerSignal();
        EXPECT_TRUE(h.freed("L1C"));
        EXPECT_TRUE(h.freed("L2W"));
        EXPECT_EQ(h.counters.gap_sig.freed_lli, 2);
        EXPECT_EQ(h.counters.gap_sig_share.freed_lli, 1);
        EXPECT_TRUE(contains(h.relevelEventOf("L1C"), ";trigger_cause=lli;"));
    }
    {
        Harness h(/*share=*/true, "lli_off");
        h.params.gap_reentry_lli = false;
        h.addSig(kSat, "L1C", "L5Q", /*shifted=*/false, 0.026);
        h.addSig(kSat, "L2W", "L5Q", /*shifted=*/false, 0.020, /*lli=*/1);
        h.runPerSignal();
        EXPECT_TRUE(h.relevel.free_ambs.empty());
        EXPECT_EQ(h.counters.gap_sig_share.signals, 0);
    }
    {
        Harness h(/*share=*/true, "unmeas");
        h.addSig(kSat, "L1C", "L5Q", /*shifted=*/false, 0.026);
        h.addSig(kSat, "L2W", "", /*shifted=*/false, 0.0, /*lli=*/0,
                 /*measurable=*/false);
        h.runPerSignal();
        EXPECT_TRUE(h.freed("L1C"));
        EXPECT_TRUE(h.freed("L2W"));
        EXPECT_EQ(h.counters.gap_sig.freed_unmeas, 2);
        EXPECT_EQ(h.counters.gap_sig_share.freed_unmeas, 1);
        EXPECT_TRUE(contains(h.relevelEventOf("L1C"), ";trigger_cause=unmeas;"));
    }
    {
        Harness h(/*share=*/true, "unmeas_off");
        h.params.gap_reentry_per_signal_unmeasurable = false;
        h.addSig(kSat, "L1C", "L5Q", /*shifted=*/false, 0.026);
        h.addSig(kSat, "L2W", "", /*shifted=*/false, 0.0, /*lli=*/0,
                 /*measurable=*/false);
        h.runPerSignal();
        EXPECT_TRUE(h.relevel.free_ambs.empty());
        EXPECT_EQ(h.counters.gap_sig.verdicts, 0);
        EXPECT_EQ(h.counters.gap_sig_share.signals, 0);
    }
}

// The share acts only with the per-signal tier and the row-level response it rides
// on; every other combination is inert (the loader warns).
TEST(GapReentryCoresumedTest, ActiveOnlyWithBothTiers) {
    for (const bool share : { false, true })
        for (const bool row : { false, true })
            for (const bool per_signal : { false, true }) {
                SlipQcParams p;
                p.gap_reentry_coresumed_share = share;
                p.gap_reentry                 = row;
                p.gap_reentry_per_signal      = per_signal;
                EXPECT_EQ(p.coresumedShareActive(), share && row && per_signal)
                    << "share=" << share << " row=" << row << " sig=" << per_signal;
            }
}

// A trigger the station does not carry (here an unmodelled L2L with no carry entry)
// still triggers the share, and the line of sight is counted as such, so a field arm
// can see that part of the scope act.
TEST(GapReentryCoresumedTest, UncarriedTriggerIsCounted) {
    Harness h(/*share=*/true);
    h.addSig(kSat, "L1C", "L5Q", /*shifted=*/false, 0.026);
    h.addSig(kSat, "L2L", "L5Q", /*shifted=*/true, 3.10);
    h.runPerSignal();

    EXPECT_TRUE(h.freed("L1C"));
    EXPECT_FALSE(h.freed("L2L"));                     // its own response: no carry
    const auto& sh = h.counters.gap_sig_share;
    EXPECT_EQ(sh.los_answered, 1);
    EXPECT_EQ(sh.los_trigger_uncarried, 1);
    EXPECT_EQ(sh.freed_shifted, 1);
    EXPECT_EQ(h.counters.gap_sig.no_carry, 1);       // the trigger
    EXPECT_TRUE(contains(h.relevelEventOf("L1C"), ";trigger=L2L;"));
}

// ===========================================================================
// The share's dispositions other than a free
// ===========================================================================

// At a gauge-pinned station (index below i_lo) a shared verdict, like the tier's own,
// gets no response: counted skipped.  The station is outside the share's
// denominator.
TEST(GapReentryCoresumedTest, SharedSignalAtGaugePinnedStationIsSkipped) {
    Harness h(/*share=*/true, "a", /*n_stations=*/2);
    h.i_lo = 1;
    h.fieldCase(/*station=*/0);
    h.runPerSignal();

    EXPECT_TRUE(h.relevel.free_ambs.empty());
    EXPECT_EQ(h.counters.gap_sig.verdicts, 2);
    EXPECT_EQ(h.counters.gap_sig.pivot_skipped, 2);
    const auto& sh = h.counters.gap_sig_share;
    EXPECT_EQ(sh.los_coresumed, 0);
    EXPECT_EQ(sh.los_answered, 0);
    EXPECT_EQ(sh.signals, 1);
    EXPECT_EQ(sh.skipped, 1);
    EXPECT_TRUE(h.shareClosureHolds());
    EXPECT_TRUE(h.relevelEvents().empty());
}

// A shared signal with no protectable carry is responded to and finds nothing to
// free: counted no_carry, with no reentry_relevel row.
TEST(GapReentryCoresumedTest, SharedSignalWithNoCarryIsCountedNoCarry) {
    Harness h(/*share=*/true);
    net::dropArc(h.local_carry, std::make_tuple(0, std::string(kSat), std::string("L1C")), &net::ArcState::amb);
    h.fieldCase();
    h.runPerSignal();

    EXPECT_TRUE(h.freed("L2W"));
    EXPECT_FALSE(h.freed("L1C"));
    EXPECT_EQ(h.counters.gap_sig.no_carry, 1);
    const auto& sh = h.counters.gap_sig_share;
    EXPECT_EQ(sh.signals, 1);
    EXPECT_EQ(sh.no_carry, 1);
    EXPECT_EQ(h.shareFreed(), 0);
    EXPECT_TRUE(h.shareClosureHolds());
    EXPECT_TRUE(h.relevelEventOf("L1C").empty());
}

// A reached arc is not a freed arc: with the carry cold, with relevel_free_epoch
// off, or on the DD invariant vehicle, the shared signal lands in the bucket the
// responder put it in (cold, not acted, funnel only), never in the freed buckets.
TEST(GapReentryCoresumedTest, ReachedButNotFreedIsNotCountedFreed) {
    {
        Harness h(/*share=*/true, "cold");
        h.carry_warm = false;
        h.fieldCase();
        h.runPerSignal();
        const auto& sh = h.counters.gap_sig_share;
        EXPECT_EQ(sh.cold_skipped, 1);
        EXPECT_EQ(h.shareFreed(), 0);
        EXPECT_EQ(h.counters.gap_sig.cold_skipped, 2);
        EXPECT_TRUE(h.shareClosureHolds());
        EXPECT_TRUE(h.tierClosureHolds());
        EXPECT_TRUE(contains(h.relevelEventOf("L1C"), ",cold,"));
    }
    {
        Harness h(/*share=*/true, "no_relevel");
        h.use_relevel = false;
        h.fieldCase();
        h.runPerSignal();
        const auto& sh = h.counters.gap_sig_share;
        EXPECT_EQ(sh.no_relevel, 1);
        EXPECT_EQ(h.shareFreed(), 0);
        EXPECT_TRUE(h.shareClosureHolds());
        EXPECT_TRUE(contains(h.relevelEventOf("L1C"), ",no_relevel,"));
    }
    {
        Harness h(/*share=*/true, "dd_only");
        h.use_relevel     = false;
        h.carry_invariant = true;
        h.fieldCase();
        h.runPerSignal();
        const auto& sh = h.counters.gap_sig_share;
        EXPECT_EQ(sh.dd_only, 1);
        EXPECT_EQ(h.shareFreed(), 0);
        EXPECT_TRUE(h.shareClosureHolds());
        EXPECT_EQ(h.carry_slipped_ambs.count(
                      { 0, StateKeys::makeAmbiguityKey(kSat, "L1C") }), 1u);
    }
}

// A co-resumed signal an earlier drain already freed this epoch (the MW drain, say)
// is not freed a second time: no verdict, no second relevel mark, no second row.
TEST(GapReentryCoresumedTest, SiblingAlreadyFreedThisEpochIsNotFreedTwice) {
    Harness h(/*share=*/true);
    h.relevel.free_ambs.insert({ 0, StateKeys::makeAmbiguityKey(kSat, "L1C") });
    h.fieldCase();
    h.runPerSignal();

    EXPECT_TRUE(h.freed("L1C"));                      // still in the set
    EXPECT_TRUE(h.freed("L2W"));
    EXPECT_EQ(h.counters.gap_sig.verdicts, 1);
    EXPECT_EQ(h.counters.relevel_free_marked, 1);
    const auto& sh = h.counters.gap_sig_share;
    EXPECT_EQ(sh.signals, 1);
    EXPECT_EQ(sh.already_freed, 1);
    EXPECT_EQ(sh.los_answered, 0);
    EXPECT_EQ(h.shareFreed(), 0);
    EXPECT_TRUE(h.shareClosureHolds());
    const auto ev = h.relevelEvents();
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_TRUE(contains(ev[0], "L2W;scope=signal;"));
}

// ===========================================================================
// The ledger-driven population (card SID phase 3)
// ===========================================================================

// Under network.ledger_reentry the row-level drain's ledger loop owns the census and
// counts a clean per-signal verdict as bridged.  A shared verdict is a verdict on
// that ledger arc: the loop routes it to the per-signal tier, which frees it, so the
// four dispositions still partition the population.
TEST(GapReentryCoresumedTest, LedgerArcTakesTheSharedVerdict) {
    for (const bool share : { false, true }) {
        Harness h(share, share ? "on" : "off");
        h.params.ledger_reentry = true;
        h.sid.armed = true;
        for (const char* s : { "L1C", "L2W" }) h.addLedgerArc(s);
        h.fieldCase();
        h.runRowLevel();
        h.runPerSignal();

        const auto& q = h.counters;
        EXPECT_EQ(q.ledger_reentry_arcs, 2) << "share=" << share;
        EXPECT_EQ(q.ledger_reentry_sig_tier, 2) << "share=" << share;
        EXPECT_EQ(q.ledger_reentry_arcs,
                  q.ledger_reentry_freed + q.ledger_reentry_bridged
                      + q.ledger_reentry_no_carry + q.ledger_reentry_pivot_skipped)
            << "share=" << share;
        EXPECT_TRUE(h.freed("L2W")) << "share=" << share;
        if (!share) {
            EXPECT_FALSE(h.freed("L1C"));
            EXPECT_EQ(q.ledger_reentry_v_clean, 1);
            EXPECT_EQ(q.ledger_reentry_bridged, 1);
            EXPECT_EQ(q.ledger_reentry_v_shifted, 1);
            EXPECT_EQ(q.ledger_reentry_v_shared, 0);
            EXPECT_EQ(q.ledger_reentry_freed, 1);
        } else {
            EXPECT_TRUE(h.freed("L1C"));
            EXPECT_EQ(q.ledger_reentry_v_clean, 0);
            EXPECT_EQ(q.ledger_reentry_bridged, 0);
            EXPECT_EQ(q.ledger_reentry_v_shifted, 2);
            EXPECT_EQ(q.ledger_reentry_v_shared, 1);
            EXPECT_EQ(q.ledger_reentry_freed, 2);
            EXPECT_EQ(q.gap_sig_share.freed_shifted, 1);
            const auto it = h.outcome.find(ArcLedger::Key{ 0, kSat, "L1C" });
            ASSERT_NE(it, h.outcome.end());
            EXPECT_EQ(it->second, ArcReentryOutcome::Freed);
        }
    }
}

// Under the ledger, an arc an earlier drain already freed keeps its clean verdict in
// the ledger loop (bridged there) and is counted already_freed by the share, so both
// closures hold and the arc is answered once.
TEST(GapReentryCoresumedTest, LedgerArcAlreadyFreedKeepsItsCleanVerdict) {
    Harness h(/*share=*/true);
    h.params.ledger_reentry = true;
    h.sid.armed = true;
    for (const char* s : { "L1C", "L2W" }) h.addLedgerArc(s);
    h.relevel.free_ambs.insert({ 0, StateKeys::makeAmbiguityKey(kSat, "L1C") });
    h.fieldCase();
    h.runRowLevel();
    h.runPerSignal();

    const auto& q = h.counters;
    EXPECT_EQ(q.ledger_reentry_v_clean, 1);
    EXPECT_EQ(q.ledger_reentry_v_shared, 0);
    EXPECT_EQ(q.ledger_reentry_bridged, 1);
    EXPECT_EQ(q.ledger_reentry_freed, 1);
    EXPECT_EQ(q.ledger_reentry_arcs,
              q.ledger_reentry_freed + q.ledger_reentry_bridged
                  + q.ledger_reentry_no_carry + q.ledger_reentry_pivot_skipped);
    EXPECT_EQ(q.gap_sig_share.already_freed, 1);
    EXPECT_TRUE(h.shareClosureHolds());
    EXPECT_EQ(q.relevel_free_marked, 1);
}

// Under the ledger a shared signal outside the admitted population gets no
// response, as a verdict there gets none; it is counted skipped.
TEST(GapReentryCoresumedTest, LedgerSharedSignalOutsideThePopulationIsSkipped) {
    Harness h(/*share=*/true);
    h.params.ledger_reentry = true;
    h.sid.armed = true;
    h.addLedgerArc("L2W");           // only the trigger is in the admitted population
    h.fieldCase();
    h.runPerSignal();

    EXPECT_TRUE(h.freed("L2W"));
    EXPECT_FALSE(h.freed("L1C"));
    const auto& sh = h.counters.gap_sig_share;
    EXPECT_EQ(sh.signals, 1);
    EXPECT_EQ(sh.skipped, 1);
    EXPECT_EQ(sh.los_answered, 0);
    EXPECT_EQ(h.shareFreed(), 0);
    EXPECT_TRUE(h.shareClosureHolds());
}

}  // namespace GnssProcessor
