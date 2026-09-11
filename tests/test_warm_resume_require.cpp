/**
 * @file test_warm_resume_require.cpp
 * @brief HB-1 — `network.state_snapshot_require` must MEAN "refuse", on EVERY
 *        refusal branch (docs/SPEC_event_harness.md §6b, §11 Card A).
 *
 * The defect this file guards against is not "the guard is missing"; it is that
 * the guard said different things on different branches.  An event-harness leg
 * relaunched in place hits the NON-POSITIVE-AGE branch (it overwrote its own seed
 * at its first emitted epoch, so the snapshot IS the epoch being resumed to), and
 * what an operator saw was a `COLD START` warning followed, ~26 s later, by the
 * process dying at 0xC0000409 with the reason nowhere in the log.  The run was in
 * fact refused — that exit code is MSVC's `abort()` fast-fail from an unhandled
 * exception — but nothing readable said so, so the leg read as a cold one.
 *
 * The policy therefore lives in ONE pure function,
 * @c NetworkStateSnapshotIO::warmResumeRefusalMessage, which is what this file
 * tests: every refusal class produces a fatal message under `require`, no class
 * produces one without it, and the class the harness actually hits names its own
 * remedy.  The gate itself is tested for producing the right CLASS, so the two
 * halves cannot drift apart.
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

// ----------------------------------------------------- the gate's CLASSIFICATION
TEST(NetworkWarmResumeRequire, RelaunchInPlaceIsClassifiedNonPositiveAge) {
    // The event-harness case, exactly: the leg wrote its own snapshot at its first
    // emitted epoch, so the first live epoch IS the snapshot epoch — age 0.000 s.
    const NetworkStateSnapshot s = snapAt(2313, 279150.0, kFp);
    const double snap_s = 2313.0 * 604800.0 + 279150.0;
    std::string reason;
    WarmResumeRefusal why = WarmResumeRefusal::None;
    EXPECT_FALSE(NetworkStateSnapshotIO::warmResumeAllowed(s, kFp, snap_s, kHorizon,
                                                           &reason, &why));
    EXPECT_EQ(why, WarmResumeRefusal::NonPositiveAge);
    EXPECT_TRUE(contains(reason, "age 0.000 s")) << reason;

    // One epoch EARLIER is the same class (it would double-fold the carry harder).
    why = WarmResumeRefusal::None;
    EXPECT_FALSE(NetworkStateSnapshotIO::warmResumeAllowed(s, kFp, snap_s - 30.0, kHorizon,
                                                           &reason, &why));
    EXPECT_EQ(why, WarmResumeRefusal::NonPositiveAge);
}

TEST(NetworkWarmResumeRequire, EachGateRefusalGetsItsOwnClass) {
    const NetworkStateSnapshot s = snapAt(2313, 279150.0, kFp);
    const double snap_s = 2313.0 * 604800.0 + 279150.0;
    WarmResumeRefusal why = WarmResumeRefusal::None;

    // Fingerprint mismatch is checked FIRST (a stale-but-matching snapshot and a
    // fresh-but-mismatched one must not report the same thing).
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

// ------------------------------------------------------------- the REQUIRE policy
TEST(NetworkWarmResumeRequire, RequireOffNeverProducesAFatalMessage) {
    // Without `require` every refusal is a legal cold start — the pre-card behaviour,
    // which must stay byte-identical.
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
    // The regression the card exists for: ONE class silently not hard-failing.
    for (auto why : { WarmResumeRefusal::MissingFile, WarmResumeRefusal::ParseFailure,
                      WarmResumeRefusal::NoLiveEpochs, WarmResumeRefusal::Fingerprint,
                      WarmResumeRefusal::NonPositiveAge, WarmResumeRefusal::Stale,
                      WarmResumeRefusal::EmitterRestore, WarmResumeRefusal::Other }) {
        const std::string m =
                NetworkStateSnapshotIO::warmResumeRefusalMessage(true, why, "DETAIL-42");
        EXPECT_FALSE(m.empty());
        EXPECT_TRUE(contains(m, "state_snapshot_require")) << m;
        EXPECT_TRUE(contains(m, "REFUSED")) << m;
        EXPECT_TRUE(contains(m, "DETAIL-42")) << m;   // the reason is NAMED, never swallowed
    }
}

TEST(NetworkWarmResumeRequire, NonPositiveAgeNamesTheHarnessRemedy) {
    // The one refusal with a specific remedy: re-seed, do not relaunch in place.
    const std::string m = NetworkStateSnapshotIO::warmResumeRefusalMessage(
            true, WarmResumeRefusal::NonPositiveAge, "age 0.000 s");
    EXPECT_TRUE(contains(m, "event_leg.py rerun")) << m;
    EXPECT_TRUE(contains(m, "NOT OLDER")) << m;
    // Another class must NOT claim the harness remedy (it would be wrong advice).
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
