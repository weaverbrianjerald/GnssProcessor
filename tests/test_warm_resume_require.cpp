/**
 * @file test_warm_resume_require.cpp
 * @brief `network.state_snapshot_require` means "refuse" on every refusal branch
 *        (docs/SPEC_event_harness.md §6b, §11 Card A; HB-1).
 *
 * The requirement is that every refusal branch reports the same way under
 * `require`.  An event-harness leg relaunched in place hits the non-positive-age
 * branch: it overwrote its own seed at its first emitted epoch, so the snapshot is
 * the epoch being resumed to.  If that branch logs only a `COLD START` warning and
 * the refusal then surfaces as an unhandled exception, the process exits with
 * 0xC0000409 (MSVC's `abort()` fast-fail) and no reason in the log, and the leg
 * reads as a cold one.
 *
 * The policy therefore lives in one pure function,
 * @c NetworkStateSnapshotIO::warmResumeRefusalMessage, which this file tests:
 * every refusal class produces a fatal message under `require`, no class produces
 * one without it, and the class the harness hits names its own remedy.  The gate
 * is tested for producing the right class, so the two halves cannot drift apart.
 *
 * No fixture: `warmResumeAllowed` is pure and the message builder is pure.
 */
#include <gtest/gtest.h>

#include "products/NetworkStateSnapshot.hpp"

#include <string>

using namespace GnssProcessor;

namespace {

    /// A snapshot stamped at (week, tow) with a known fingerprint; nothing else matters
    /// to the gate.
    NetworkStateSnapshot snapAt(int week, double tow, uint64_t fp) {
        NetworkStateSnapshot s;
        s.snap_week = week;
        s.snap_tow = tow;
        s.config_fingerprint = fp;
        return s;
    }

    constexpr uint64_t kFp = 0x6a3531a1afb2b40bULL;   // the OFF seed's real fingerprint
    constexpr double kHorizon = 900.0;                 // phase_leveling_max_gap_s

    bool contains(const std::string& hay, const char* needle) {
        return hay.find(needle) != std::string::npos;
    }

} // namespace

// ----------------------------------------------------- the gate's classification
TEST(NetworkWarmResumeRequire, RelaunchInPlaceIsClassifiedNonPositiveAge) {
    // The event-harness case: the leg wrote its own snapshot at its first emitted
    // epoch, so the first live epoch is the snapshot epoch (age 0.000 s).
    const NetworkStateSnapshot s = snapAt(2313, 279150.0, kFp);
    const double snap_s = 2313.0 * 604800.0 + 279150.0;
    std::string reason;
    WarmResumeRefusal why = WarmResumeRefusal::None;
    EXPECT_FALSE(NetworkStateSnapshotIO::warmResumeAllowed(s, kFp, snap_s, kHorizon,
                                                           &reason, &why));
    EXPECT_EQ(why, WarmResumeRefusal::NonPositiveAge);
    EXPECT_TRUE(contains(reason, "age 0.000 s")) << reason;

    // One epoch earlier is the same class (it would double-fold the carry further).
    why = WarmResumeRefusal::None;
    EXPECT_FALSE(NetworkStateSnapshotIO::warmResumeAllowed(s, kFp, snap_s - 30.0, kHorizon,
                                                           &reason, &why));
    EXPECT_EQ(why, WarmResumeRefusal::NonPositiveAge);
}

TEST(NetworkWarmResumeRequire, EachGateRefusalGetsItsOwnClass) {
    const NetworkStateSnapshot s = snapAt(2313, 279150.0, kFp);
    const double snap_s = 2313.0 * 604800.0 + 279150.0;
    WarmResumeRefusal why = WarmResumeRefusal::None;

    // Fingerprint mismatch is checked first, so a fresh but mismatched snapshot
    // reports Fingerprint, not an age class.
    EXPECT_FALSE(NetworkStateSnapshotIO::warmResumeAllowed(s, kFp + 1, snap_s + 30.0,
                                                           kHorizon, nullptr, &why));
    EXPECT_EQ(why, WarmResumeRefusal::Fingerprint);

    why = WarmResumeRefusal::None;
    EXPECT_FALSE(NetworkStateSnapshotIO::warmResumeAllowed(s, kFp, snap_s + kHorizon + 1.0,
                                                           kHorizon, nullptr, &why));
    EXPECT_EQ(why, WarmResumeRefusal::Stale);

    // And a legal resume clears the class rather than leaving the caller's value.
    why = WarmResumeRefusal::Stale;
    EXPECT_TRUE(NetworkStateSnapshotIO::warmResumeAllowed(s, kFp, snap_s + 30.0, kHorizon,
                                                          nullptr, &why));
    EXPECT_EQ(why, WarmResumeRefusal::None);
}

// ------------------------------------------------------------- the require policy
TEST(NetworkWarmResumeRequire, RequireOffNeverProducesAFatalMessage) {
    // Without `require` every refusal is a legal cold start, byte-identical to the
    // behaviour before the knob existed.
    for (auto why : { WarmResumeRefusal::MissingFile, WarmResumeRefusal::ParseFailure,
                      WarmResumeRefusal::NoLiveEpochs, WarmResumeRefusal::Fingerprint,
                      WarmResumeRefusal::NonPositiveAge, WarmResumeRefusal::Stale,
                      WarmResumeRefusal::EmitterRestore, WarmResumeRefusal::Other })
        EXPECT_TRUE(NetworkStateSnapshotIO::warmResumeRefusalMessage(false, why, "d").empty());
    // And `None` is not a refusal even under require.
    EXPECT_TRUE(NetworkStateSnapshotIO::warmResumeRefusalMessage(
                    true, WarmResumeRefusal::None, "d").empty());
}

TEST(NetworkWarmResumeRequire, RequireOnIsFatalOnEveryRefusalClass) {
    // The regression this guards: any single class failing to hard-fail.
    for (auto why : { WarmResumeRefusal::MissingFile, WarmResumeRefusal::ParseFailure,
                      WarmResumeRefusal::NoLiveEpochs, WarmResumeRefusal::Fingerprint,
                      WarmResumeRefusal::NonPositiveAge, WarmResumeRefusal::Stale,
                      WarmResumeRefusal::EmitterRestore, WarmResumeRefusal::Other }) {
        const std::string m =
                NetworkStateSnapshotIO::warmResumeRefusalMessage(true, why, "DETAIL-42");
        EXPECT_FALSE(m.empty());
        EXPECT_TRUE(contains(m, "state_snapshot_require")) << m;
        EXPECT_TRUE(contains(m, "REFUSED")) << m;
        EXPECT_TRUE(contains(m, "DETAIL-42")) << m;   // the reason appears in the message
    }
}

TEST(NetworkWarmResumeRequire, NonPositiveAgeNamesTheHarnessRemedy) {
    // The one refusal with a specific remedy: re-seed, do not relaunch in place.
    const std::string m = NetworkStateSnapshotIO::warmResumeRefusalMessage(
            true, WarmResumeRefusal::NonPositiveAge, "age 0.000 s");
    EXPECT_TRUE(contains(m, "event_leg.py rerun")) << m;
    EXPECT_TRUE(contains(m, "NOT OLDER")) << m;
    // Another class does not name the harness remedy (it would be wrong advice).
    const std::string other = NetworkStateSnapshotIO::warmResumeRefusalMessage(
            true, WarmResumeRefusal::Fingerprint, "fingerprint mismatch");
    EXPECT_FALSE(contains(other, "event_leg.py rerun")) << other;
}

TEST(NetworkWarmResumeRequire, TheGateClassFlowsIntoTheFatalMessage) {
    // End to end across the seam the driver uses: gate -> class -> message.
    const NetworkStateSnapshot s = snapAt(2313, 279150.0, kFp);
    const double snap_s = 2313.0 * 604800.0 + 279150.0;
    std::string gate;
    WarmResumeRefusal why = WarmResumeRefusal::None;
    ASSERT_FALSE(NetworkStateSnapshotIO::warmResumeAllowed(s, kFp, snap_s, kHorizon,
                                                           &gate, &why));
    const std::string m = NetworkStateSnapshotIO::warmResumeRefusalMessage(true, why, gate);
    ASSERT_FALSE(m.empty());
    EXPECT_TRUE(contains(m, "double-fold")) << m;        // the gate's own prose survived
    EXPECT_TRUE(contains(m, "event_leg.py rerun")) << m; // and the class picked the remedy
}
