/**
 * @file test_network_gf_arc_jump.cpp
 * @brief Card ARL slip-ladder defect 1: the arc-scoped geometry-free jump trigger
 *        (include/network/slip/NetworkGfArcJump.hpp), its per-arc test and its PASS-A drain.
 *
 * ## What is under test
 *
 * Suite @c GfArcJumpKernel pins the pure per-arc test, @c gfArcJumpStep: the reference
 * is the arc's last CLEAN step, so an isolated slip flags once and a slip repeated at
 * the next epoch flags again (ladder rung S7); a gap, a pair change or a span change
 * restarts the arc; a persistent GF-rate change flags at most @c kGfArcJumpMaxFlagRun
 * times; the scale term raises the bar only when armed and only once the arc holds
 * enough clean values.  The step sizes are the ladder's: the (4,5) slip of rung S8
 * (GF -0.4555 m), the (1,1) slip of rung S4b (GF lambda1 - lambda2 = -0.0539 m).
 *
 * Suite @c GfArcJumpDrain pins the response on one hand-built station: what a flag
 * frees (both bands of the pair, under the cause bit @c kCauseGfJump), what it leaves
 * alone (a cold carry, a band an earlier drain already freed, a band the common-band
 * verdict keeps, a gauge-pinned station), and the counters' flag closure.  Every case
 * asserts the counters as well as the state: a drain that acted while reporting zeros
 * would look like dead wiring at run end (standing invariant 4).
 *
 * The raw observables pass that produces the rows, and the network around the drain,
 * are out of scope; the rows are built by hand.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"
#include "network/diag/NetworkRunTraces.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "network/slip/NetworkGfArcJump.hpp"
#include "network/slip/NetworkSlipQc.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>

#include <cmath>
#include <cstdint>
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

constexpr double kLam1 = 0.190293672798;   // GPS L1 wavelength (m)
constexpr double kLam2 = 0.244210213425;   // GPS L2 wavelength (m)
constexpr double kEpochS = 30.0;

// ===========================================================================
// The per-arc test
// ===========================================================================

/// Drives one arc through epochs of a GF series.
struct ArcDriver {
    GfArcJumpArc    arc;
    GfArcJumpParams p;
    long long       epoch{0};
    double          t_s{1.0e6};
    std::string     sig1{"L1C"}, sig2{"L2W"};

    ArcDriver() { p.armed = true; }

    /// One sample; @p skip_epochs processed epochs are missed before it.
    GfArcJumpVerdict sample(double gf_m, long long skip_epochs = 0, double extra_s = 0.0) {
        epoch += 1 + skip_epochs;
        t_s   += kEpochS * static_cast<double>(1 + skip_epochs) + extra_s;
        return gfArcJumpStep(arc, epoch, t_s, sig1, sig2, gf_m, p);
    }
};

/// A slowly varying ionospheric GF level plus a deterministic sub-centimetre wobble.
double smoothGf(int k) {
    const double t = static_cast<double>(k);
    return -2.5 + 0.0031 * t + 1.2e-5 * t * t + 0.0015 * std::sin(0.7 * t);
}

TEST(GfArcJumpKernel, ACleanDriftingArcNeverFlags) {
    ArcDriver d;
    int tested = 0;
    for (int k = 0; k < 200; ++k) {
        const GfArcJumpVerdict v = d.sample(smoothGf(k));
        EXPECT_FALSE(v.flagged) << "epoch " << k << " r=" << v.r_m;
        EXPECT_FALSE(v.restarted);
        if (v.tested) ++tested;
    }
    // The first sample opens the arc, the second seeds the reference, the rest test.
    EXPECT_EQ(tested, 198);
}

TEST(GfArcJumpKernel, ANonFiniteSampleLeavesTheArcEmpty) {
    // A NaN sample restarts the arc but cannot open it: the next finite sample opens a
    // fresh arc, the one after seeds the reference, and testing resumes with finite r.
    ArcDriver d;
    for (int k = 0; k < 10; ++k) d.sample(smoothGf(k));
    const GfArcJumpVerdict nan_v = d.sample(std::nan(""));
    EXPECT_TRUE(nan_v.restarted);
    EXPECT_FALSE(nan_v.tested);
    EXPECT_EQ(d.arc.last_epoch, -1);
    const GfArcJumpVerdict open_v = d.sample(smoothGf(11));
    EXPECT_FALSE(open_v.restarted);   // an empty arc has no sample to restart from
    EXPECT_FALSE(open_v.tested);
    d.sample(smoothGf(12));           // seeds the reference
    const GfArcJumpVerdict v = d.sample(smoothGf(13));
    ASSERT_TRUE(v.tested);
    EXPECT_TRUE(std::isfinite(v.r_m));
    EXPECT_FALSE(v.flagged);
}

TEST(GfArcJumpKernel, AnIsolatedSlipFlagsOnceAtItsEpoch) {
    // Rung S8: the (4,5) slip, GF step 4*lambda1 - 5*lambda2 = -0.4600 m in truth; the
    // raw step measured on the ladder was -0.4555 m.
    const double step = 4.0 * kLam1 - 5.0 * kLam2;
    ArcDriver d;
    double offset = 0.0;
    for (int k = 0; k < 60; ++k) {
        if (k == 30) offset += step;
        const GfArcJumpVerdict v = d.sample(smoothGf(k) + offset);
        if (k == 30) {
            ASSERT_TRUE(v.tested);
            EXPECT_TRUE(v.flagged);
            EXPECT_NEAR(v.r_m, step, 0.01);
            EXPECT_DOUBLE_EQ(v.thr_m, 0.04);
        } else {
            EXPECT_FALSE(v.flagged) << "epoch " << k;   // the step after the slip is ordinary
        }
    }
}

TEST(GfArcJumpKernel, ASlipRepeatedAtTheNextEpochFlagsAgainThenTheReferenceReseeds) {
    // Rung S7: +2 cycles on L1 at two consecutive epochs.  The second repetition has
    // no IF-TD sample (that detector restarts the chain on its first flag), and its
    // step equals the first; against the previous step it would read r = 0.
    const double step = 2.0 * kLam1;
    ArcDriver d;
    double offset = 0.0;
    for (int k = 0; k < 30; ++k) d.sample(smoothGf(k));
    offset += step;
    const GfArcJumpVerdict v1 = d.sample(smoothGf(30) + offset);
    offset += step;
    const GfArcJumpVerdict v2 = d.sample(smoothGf(31) + offset);
    EXPECT_TRUE(v1.flagged);
    EXPECT_FALSE(v1.reseeded);
    EXPECT_TRUE(v2.flagged);
    EXPECT_NEAR(v2.r_m, step, 0.01);
    EXPECT_TRUE(v2.reseeded);          // the run cap is 2
    // The next step seeds the fresh reference untested; testing resumes after it.
    const GfArcJumpVerdict v3 = d.sample(smoothGf(32) + offset);
    EXPECT_FALSE(v3.tested);
    const GfArcJumpVerdict v4 = d.sample(smoothGf(33) + offset);
    EXPECT_TRUE(v4.tested);
    EXPECT_FALSE(v4.flagged);
}

TEST(GfArcJumpKernel, TheEqualCountSlipClearsTheFloorAndSubFloorStepsDoNot) {
    // Rung S4b: (1,1) moves GF by lambda1 - lambda2 = -0.0539 m, above the 0.04 m floor.
    ArcDriver d;
    for (int k = 0; k < 20; ++k) d.sample(-1.0);
    const GfArcJumpVerdict v = d.sample(-1.0 + (kLam1 - kLam2));
    EXPECT_TRUE(v.flagged);
    EXPECT_NEAR(v.r_m, kLam1 - kLam2, 1e-12);
    // A 3.5 cm step (the order of the sim control's largest clean |r|) does not flag.
    ArcDriver q;
    for (int k = 0; k < 20; ++k) q.sample(-1.0);
    EXPECT_FALSE(q.sample(-1.0 + 0.035).flagged);
}

TEST(GfArcJumpKernel, AGapRestartsTheArcAndASlipInItsSeedStepIsSeenOneEpochLate) {
    ArcDriver d;
    for (int k = 0; k < 10; ++k) d.sample(smoothGf(k));
    const GfArcJumpVerdict r0 = d.sample(smoothGf(13), /*skip_epochs=*/2);
    EXPECT_TRUE(r0.restarted);
    EXPECT_FALSE(r0.tested);
    // The seed step is untested, so a slip inside it is not flagged there ...
    const GfArcJumpVerdict r1 = d.sample(smoothGf(14) + 0.5);
    EXPECT_FALSE(r1.restarted);
    EXPECT_FALSE(r1.tested);
    // ... but it became the reference, so the next ordinary step departs from it by the
    // slip with the opposite sign and flags one epoch late.
    const GfArcJumpVerdict r2 = d.sample(smoothGf(15) + 0.5);
    EXPECT_TRUE(r2.tested);
    EXPECT_TRUE(r2.flagged);
    EXPECT_NEAR(r2.r_m, -0.5, 0.01);
    // A clean seed step leaves the arc quiet.
    ArcDriver q;
    for (int k = 0; k < 10; ++k) q.sample(smoothGf(k));
    q.sample(smoothGf(13), /*skip_epochs=*/2);
    q.sample(smoothGf(14));
    const GfArcJumpVerdict c = q.sample(smoothGf(15));
    EXPECT_TRUE(c.tested);
    EXPECT_FALSE(c.flagged);
}

TEST(GfArcJumpKernel, APairChangeRestartsTheArc) {
    ArcDriver d;
    for (int k = 0; k < 10; ++k) d.sample(smoothGf(k));
    d.sig2 = "L5Q";   // the GF is now formed on another pair: its level is a different quantity
    const GfArcJumpVerdict v = d.sample(smoothGf(10) + 3.0);
    EXPECT_TRUE(v.restarted);
    EXPECT_FALSE(v.flagged);
    EXPECT_EQ(d.arc.sig2, "L5Q");
}

TEST(GfArcJumpKernel, AStepOverADifferentSpanRestartsTheArc) {
    // A skipped processed epoch leaves the index consecutive but doubles the span.
    ArcDriver d;
    for (int k = 0; k < 10; ++k) d.sample(smoothGf(k));
    const GfArcJumpVerdict v = d.sample(smoothGf(11), 0, /*extra_s=*/kEpochS);
    EXPECT_TRUE(v.restarted);
    EXPECT_FALSE(v.tested);
}

TEST(GfArcJumpKernel, APersistentRateChangeFlagsAtMostTheRunCap) {
    // An ionospheric front: the GF rate steps by 6 cm per epoch and stays.  Against a
    // locked reference every epoch would flag; the run cap bounds it.
    ArcDriver d;
    double gf = 0.0;
    for (int k = 0; k < 20; ++k) d.sample(gf += 0.001);
    int flags = 0;
    for (int k = 0; k < 40; ++k) {
        if (d.sample(gf += 0.061).flagged) ++flags;
    }
    EXPECT_EQ(flags, kGfArcJumpMaxFlagRun);
}

TEST(GfArcJumpKernel, TheScaleTermRaisesTheBarOnlyWhenArmedAndOnceEnoughCleanValuesAreHeld) {
    // A noisy arc: step noise uniform in +-1.8 cm (a deterministic generator), so every
    // clean |r| stays under the 4 cm floor while the arc's robust scale is about 1.5 cm.
    std::uint32_t s = 12345u;
    const auto noise = [&s]() {
        s = s * 1664525u + 1013904223u;
        return (static_cast<double>(s >> 8) / 16777216.0 - 0.5) * 0.036;
    };
    ArcDriver on;  on.p.k  = 6.0;
    ArcDriver off; off.p.k = 0.0;
    double gf = 0.0;
    for (int k = 0; k < 40; ++k) {
        gf += 0.002 + noise();
        const GfArcJumpVerdict a = on.sample(gf);
        const GfArcJumpVerdict b = off.sample(gf);
        EXPECT_FALSE(a.flagged);
        EXPECT_FALSE(b.flagged);
        if (a.tested) {
            EXPECT_DOUBLE_EQ(a.thr_m, std::max(0.04, 6.0 * a.scale_m));
            // The scale applies only once min_count (10) clean values are held.
            if (static_cast<int>(on.arc.clean_r.size()) < on.p.min_count)
                EXPECT_EQ(a.n_clean, 0);
        }
        EXPECT_EQ(b.n_clean, 0);              // k = 0: the scale term is off
        if (b.tested) EXPECT_DOUBLE_EQ(b.thr_m, 0.04);
    }
    // A step whose r sits midway between the floor and the arc's own bar.
    const double bar = 6.0 * gfArcJumpRobustScale(on.arc.clean_r);
    ASSERT_GT(bar, 0.05);
    const double r_target = 0.5 * (0.04 + bar);
    gf += on.arc.ref_step_m + r_target;
    const GfArcJumpVerdict a = on.sample(gf);
    const GfArcJumpVerdict b = off.sample(gf);
    EXPECT_NEAR(b.r_m, r_target, 1e-9);
    EXPECT_TRUE(b.flagged);                  // the floor alone flags it
    EXPECT_FALSE(a.flagged);                 // the arc's own scale does not
    EXPECT_GT(a.n_clean, 0);
    EXPECT_DOUBLE_EQ(a.thr_m, bar);
}

TEST(GfArcJumpKernel, TheRobustScaleIsTheScaledMedianAbsoluteDeviation) {
    EXPECT_DOUBLE_EQ(gfArcJumpRobustScale({}), 0.0);
    // median 3; |dev| {2, 1, 0, 1, 97}, median 1.
    EXPECT_DOUBLE_EQ(gfArcJumpRobustScale({1.0, 2.0, 3.0, 4.0, 100.0}), 1.4826);
    EXPECT_DOUBLE_EQ(gfArcJumpRobustScale({0.01, 0.01, 0.01}), 0.0);
}

TEST(GfArcJumpKernel, TheCleanWindowIsBounded) {
    ArcDriver d;
    for (int k = 0; k < 100; ++k) d.sample(smoothGf(k));
    EXPECT_EQ(static_cast<int>(d.arc.clean_r.size()), d.p.window);
}

// ===========================================================================
// The PASS-A drain
// ===========================================================================

constexpr const char* kSta  = "TEST00XXX";
constexpr const char* kSat  = "G14";
constexpr const char* kSig1 = "L1C";
constexpr const char* kSig2 = "L2W";

std::shared_ptr<spdlog::logger> nullLogger() {
    return std::make_shared<spdlog::logger>(
        "test_gf_arc_jump", std::make_shared<spdlog::sinks::null_sink_mt>());
}

/// One station tracking @ref kSat on L1C/L2W with both ambiguity states live.  The
/// traces object is built over a scratch path and its event stream stays closed, so
/// @c emitEvent is a no-op and nothing touches the filesystem.
struct Harness {
    LogManager                 log_manager;
    GnssConfig                 config;
    std::vector<NetStation>    stations;
    std::vector<bool>          present{true};
    std::set<std::pair<int, std::string>> carry_slipped_ambs;
    SlipQcChannel              qc;
    RelevelEpochState          relevel;
    std::map<std::tuple<int, std::string, std::string>, RelevelPend> pends;
    CommonBandEpoch            common_band;
    SyncedEpoch                se;
    NetworkRunTraces           traces{std::filesystem::temp_directory_path()
                                      / "gnss_gf_arc_jump_test_unused"};
    std::shared_ptr<spdlog::logger> log{nullLogger()};
    double                     gf{-3.0};

    explicit Harness(bool with_states = true) {
        stations.emplace_back();
        stations.front().name = kSta;
        stations.front().sm =
            std::make_unique<StateManager>(/*core_state_size=*/5, log_manager, config);
        if (with_states) {
            stations.front().sm->addAmbiguity(StateKeys::makeAmbiguityKey(kSat, kSig1), 0.0);
            stations.front().sm->addAmbiguity(StateKeys::makeAmbiguityKey(kSat, kSig2), 0.0);
        }
        qc.params.gf_jump.armed = true;
        qc.hist.resize(1);
        se.time = GnssTime{2313, 261000.0};
    }

    /// One epoch: the GF level moves by @p step_m, the latch is cleared as PASS A
    /// clears it, and the drain runs in the posture given.
    void epoch(double step_m, bool carry_warm = true, bool relevel_armed = true,
               bool carry_invariant = false, int i_lo = 0, bool scoped = false) {
        gf += step_m;
        se.time.tow += kEpochS;
        ++qc.epoch_idx;
        qc.epoch.reset(1);
        RawSlipObs row;
        row.satellite = kSat;
        row.sig1      = kSig1;
        row.sig2      = kSig2;
        row.dual      = true;
        row.gf_m      = gf;
        row.elev_deg  = 56.9;
        qc.epoch.rows[0].push_back(row);
        qc.epoch.computed = true;
        relevel.clear();
        networkSlipDrainGfArcJump(qc, stations, present, i_lo, /*n_sta_total=*/1,
                                  carry_invariant, carry_warm, carry_slipped_ambs, se,
                                  traces, log, relevel_armed ? &relevel : nullptr,
                                  relevel_armed ? &pends : nullptr,
                                  scoped ? &common_band : nullptr);
    }
    /// Three clean epochs: the arc opens, seeds its reference, and tests once.
    void warmUp(bool carry_warm = true) {
        for (int k = 0; k < 3; ++k) epoch(0.002, carry_warm);
    }

    bool pendingReseed(const char* sig) const {
        return stations.front().sm->isAmbiguityPendingReseed(
            StateKeys::makeAmbiguityKey(kSat, sig));
    }
    bool freed(const char* sig) const {
        return relevel.free_ambs.count({0, StateKeys::makeAmbiguityKey(kSat, sig)}) != 0;
    }
    std::uint8_t cause(const char* sig) const {
        const auto it = relevel.free_cause.find({0, StateKeys::makeAmbiguityKey(kSat, sig)});
        return it == relevel.free_cause.end() ? 0 : it->second;
    }
    const GfArcJumpCounters& C() const { return qc.counters.gf_jump; }
    /// The flag closure the run-end line states.
    long long closure() const {
        const auto& c = C();
        return c.flags_freed + c.flags_coincident + c.flags_cb_kept + c.flags_no_arc
             + c.flags_cold + c.flags_pivot;
    }
};

const double kS8Step = 4.0 * kLam1 - 5.0 * kLam2;   // the ladder's (4,5) slip

TEST(GfArcJumpDrain, AFlagFreesBothBandsUnderTheGfCause) {
    Harness h;
    h.warmUp();
    EXPECT_EQ(h.C().tests, 1);
    EXPECT_EQ(h.C().flags, 0);
    h.epoch(0.002 + kS8Step);
    EXPECT_TRUE(h.pendingReseed(kSig1));
    EXPECT_TRUE(h.pendingReseed(kSig2));
    EXPECT_TRUE(h.freed(kSig1));
    EXPECT_TRUE(h.freed(kSig2));
    EXPECT_EQ(h.cause(kSig1), RelevelEpochState::kCauseGfJump);
    EXPECT_EQ(h.cause(kSig2), RelevelEpochState::kCauseGfJump);
    EXPECT_TRUE(h.carry_slipped_ambs.empty());   // carry_invariant is off
    EXPECT_EQ(h.C().tests, 2);
    EXPECT_EQ(h.C().flags, 1);
    EXPECT_EQ(h.C().flags_freed, 1);
    EXPECT_EQ(h.C().bands_freed, 2);
    EXPECT_EQ(h.qc.counters.relevel_free_marked, 2);
    EXPECT_EQ(h.qc.hist[0].gf_jump_flags, 1);
    EXPECT_EQ(h.qc.hist[0].gf_jump_tests, 2);
    EXPECT_EQ(h.closure(), h.C().flags);
    EXPECT_NEAR(h.C().max_abs_r_m, std::abs(kS8Step), 1e-9);
    // The next epoch is ordinary: no second flag, the latch cleared with the epoch.
    h.epoch(0.002);
    EXPECT_EQ(h.C().flags, 1);
    EXPECT_FALSE(h.freed(kSig1));
}

TEST(GfArcJumpDrain, ACleanArcIsLeftAlone) {
    Harness h;
    for (int k = 0; k < 50; ++k) h.epoch(0.002 + ((k % 2) ? 0.003 : -0.003));
    EXPECT_EQ(h.C().flags, 0);
    EXPECT_EQ(h.C().tests, 48);
    EXPECT_FALSE(h.pendingReseed(kSig1));
    EXPECT_TRUE(h.relevel.free_ambs.empty());
    EXPECT_EQ(h.qc.counters.relevel_free_marked, 0);
}

TEST(GfArcJumpDrain, ACarryNotYetWarmMakesTheFlagDiagnostic) {
    Harness h;
    h.warmUp(/*carry_warm=*/false);
    h.epoch(0.002 + kS8Step, /*carry_warm=*/false);
    EXPECT_EQ(h.C().flags, 1);
    EXPECT_EQ(h.C().flags_cold, 1);
    EXPECT_FALSE(h.pendingReseed(kSig1));
    EXPECT_TRUE(h.relevel.free_ambs.empty());
    EXPECT_EQ(h.closure(), h.C().flags);
}

TEST(GfArcJumpDrain, ABandAnEarlierDrainFreedTakesOnlyTheCauseBit) {
    Harness h;
    h.warmUp();
    // The IF-TD driver ran first this epoch: both bands freed under its bit and a bridge
    // pend created.  The epoch() helper clears the latch, so the pre-state is staged
    // through a one-off drain call instead.
    h.gf += 0.002 + kS8Step;
    h.se.time.tow += kEpochS;
    ++h.qc.epoch_idx;
    h.qc.epoch.reset(1);
    RawSlipObs row;
    row.satellite = kSat; row.sig1 = kSig1; row.sig2 = kSig2; row.dual = true;
    row.gf_m = h.gf; row.elev_deg = 56.9;
    h.qc.epoch.rows[0].push_back(row);
    h.qc.epoch.computed = true;
    h.relevel.clear();
    for (const char* sig : {kSig1, kSig2}) {
        const std::pair<int, std::string> key{0, StateKeys::makeAmbiguityKey(kSat, sig)};
        h.relevel.free_ambs.insert(key);
        h.relevel.free_cause[key] = RelevelEpochState::kCauseIfTd;
    }
    h.pends[std::make_tuple(0, std::string(kSat), std::string("L1C_L2W"))] = RelevelPend{};
    networkSlipDrainGfArcJump(h.qc, h.stations, h.present, 0, 1, false, true,
                              h.carry_slipped_ambs, h.se, h.traces, h.log, &h.relevel,
                              &h.pends, nullptr);
    EXPECT_EQ(h.cause(kSig1), RelevelEpochState::kCauseIfTd | RelevelEpochState::kCauseGfJump);
    EXPECT_EQ(h.cause(kSig2), RelevelEpochState::kCauseIfTd | RelevelEpochState::kCauseGfJump);
    EXPECT_EQ(h.pends.size(), 1u);             // the IF-TD pend survives
    EXPECT_FALSE(h.pendingReseed(kSig1));      // no second mark from this drain
    EXPECT_EQ(h.qc.counters.relevel_free_marked, 0);
    EXPECT_EQ(h.C().flags_coincident, 1);
    EXPECT_EQ(h.C().bands_coincident, 2);
    EXPECT_EQ(h.C().bands_freed, 0);
    EXPECT_EQ(h.closure(), h.C().flags);
}

TEST(GfArcJumpDrain, AFreshFreeVoidsTheStationSatellitePend) {
    Harness h;
    h.warmUp();
    h.pends[std::make_tuple(0, std::string(kSat), std::string("L1C_L2W"))] = RelevelPend{};
    h.pends[std::make_tuple(0, std::string("G05"), std::string("L1C_L2W"))] = RelevelPend{};
    h.epoch(0.002 + kS8Step);
    EXPECT_EQ(h.pends.size(), 1u);             // G05's pend is not this arc's
    EXPECT_EQ(h.pends.count(std::make_tuple(0, std::string("G05"), std::string("L1C_L2W"))), 1u);
    EXPECT_EQ(h.qc.counters.bridge_pend_voided, 1);
}

TEST(GfArcJumpDrain, TheCommonBandVerdictKeepsARefusedBand) {
    // Rung S3: an LLI-only verdict narrowed the response to L2W; a GF flag on the same
    // epoch must not free the clean L1C (the S2 against S3 contrast is that cost).
    Harness h;
    h.warmUp();
    h.common_band.armed = true;
    h.common_band.narrowed_at[{0, kSat}].implicated = {kSig2};
    h.epoch(0.002 - kLam2, true, true, false, 0, /*scoped=*/true);
    EXPECT_FALSE(h.freed(kSig1));
    EXPECT_TRUE(h.freed(kSig2));
    EXPECT_EQ(h.C().bands_cb_kept, 1);
    EXPECT_EQ(h.C().bands_freed, 1);
    EXPECT_EQ(h.C().flags_freed, 1);
    EXPECT_EQ(h.closure(), h.C().flags);
}

TEST(GfArcJumpDrain, AGaugePinnedStationIsNotRefloated) {
    Harness h;
    for (int k = 0; k < 3; ++k) h.epoch(0.002, true, true, false, /*i_lo=*/1);
    h.epoch(0.002 + kS8Step, true, true, false, /*i_lo=*/1);
    EXPECT_EQ(h.C().flags, 1);
    EXPECT_EQ(h.C().flags_pivot, 1);
    EXPECT_FALSE(h.pendingReseed(kSig1));
    EXPECT_TRUE(h.relevel.free_ambs.empty());
    EXPECT_EQ(h.closure(), h.C().flags);
}

TEST(GfArcJumpDrain, UnderCarryInvariantTheFunnelIsFed) {
    Harness h;
    for (int k = 0; k < 3; ++k) h.epoch(0.002, true, false, true);
    h.epoch(0.002 + kS8Step, /*carry_warm=*/true, /*relevel_armed=*/false,
            /*carry_invariant=*/true);
    EXPECT_EQ(h.carry_slipped_ambs.count({0, StateKeys::makeAmbiguityKey(kSat, kSig1)}), 1u);
    EXPECT_EQ(h.carry_slipped_ambs.count({0, StateKeys::makeAmbiguityKey(kSat, kSig2)}), 1u);
    EXPECT_TRUE(h.pendingReseed(kSig1));
    EXPECT_EQ(h.C().flags_freed, 1);
}

TEST(GfArcJumpDrain, AFlagWithNoLiveArcIsCountedNotLost) {
    Harness h(/*with_states=*/false);
    h.warmUp();
    h.epoch(0.002 + kS8Step);
    EXPECT_EQ(h.C().flags, 1);
    EXPECT_EQ(h.C().flags_no_arc, 1);
    EXPECT_TRUE(h.relevel.free_ambs.empty());
    EXPECT_EQ(h.closure(), h.C().flags);
}

TEST(GfArcJumpDrain, TheKnobOffTouchesNothing) {
    Harness h;
    h.qc.params.gf_jump.armed = false;
    for (int k = 0; k < 5; ++k) h.epoch(0.002 + (k == 4 ? kS8Step : 0.0));
    EXPECT_EQ(h.C().samples, 0);
    EXPECT_EQ(h.C().tests, 0);
    EXPECT_TRUE(h.qc.hist[0].gf_arcs.empty());
    EXPECT_TRUE(h.relevel.free_ambs.empty());
    EXPECT_FALSE(h.pendingReseed(kSig1));
}

}  // namespace
}  // namespace GnssProcessor
