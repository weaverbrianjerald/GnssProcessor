/**
 * @file test_realtime_seams.cpp
 * @brief Stream-level real-time invariants the position grader cannot see.
 *
 * The real-time replay tier (manifest_realtime.json) grades the converged
 * position against an independent site coordinate.  That is an end-to-end
 * guard, but it cannot see two failure modes that still produce a plausible
 * but wrong position:
 *
 *   1. An old observation consuming a future-stamped SSR correction and
 *      extrapolating the orbit/clock backward.  High-rate observations back up
 *      behind real time while sparse corrections stay fresh, so an old
 *      observation queries a fresh correction with a large negative age.  The
 *      contract is the SsrCorrectionStore freshness gate.
 *
 *   2. A broadcast ephemeris extrapolated far beyond its fit interval (a
 *      satellite never refreshed, or observation/ephemeris desync) giving a
 *      wrong orbit or clock instead of dropping the satellite.  The contract is
 *      the LiveEphemerisStore fit-window guard (the root of the BSSD epoch
 *      blowup and the RT model degradation).
 *
 * Both are unit-constructible without a PppModel / RINEX chain, deterministic,
 * and complementary to the position-accuracy regression cell.  See
 * docs/test_suite.md (Real-time replay tier).
 */

#include <gtest/gtest.h>

#include <cmath>

#include "products/SsrCorrectionStore.hpp"
#include "products/BroadcastEphemerisTypes.hpp"
#include "products/LiveEphemerisStore.hpp"
#include "products/ProductResolver.hpp"  // ULT naming statics, header-only use (RTB-4)
#include "realtime/RealTimeEngine.hpp"   // ingest-filter statics, header-only use (RTA-8(b))
#include "rtcm/RtcmTypes.hpp"
#include "common/Namespaces.hpp"

namespace GnssProcessor {
namespace {

SsrClockCorrection makeClock(const std::string& sat, double c0_m,
                             const GnssTime& received) {
    SsrClockCorrection clk;
    clk.sat           = sat;
    clk.c0            = c0_m;
    clk.received_time = received;
    clk.epoch_time    = received;
    return clk;
}

// ===========================================================================
// (1) Observation/SSR sync: the freshness gate has three regions, and the
// future side is the one an observation backlog reaches.
// age = query - record.received_time:
//   age >  max_age      -> stale past record          -> reject  (covered elsewhere)
//   -max_age <= age < 0 -> mild future (jitter)       -> accept
//   age < -max_age      -> record far in the future   -> reject  (backlog case)
// Tested as a black box through getOrbitClock, so the tests pin the consumed
// behaviour rather than a private helper.
// ===========================================================================

// A correction received well after the observation being processed (old
// observation, fresh correction) is rejected, not extrapolated backward.
TEST(RealtimeSsrFreshnessTest, FutureRecordBeyondMaxAgeRejected) {
    auto& store = SsrCorrectionStore::getInstance();
    store.clear();
    // Record stamped 100 s in the future relative to the query; max_age 30 s.
    store.pushClock(makeClock("G07", 0.05, GnssTime{2300, 200.0}), SsrHeader{});
    auto q = store.getOrbitClock("G07", GnssTime{2300, 100.0}, /*max_age_s=*/30.0);
    EXPECT_FALSE(q.has_value());
    store.clear();
}

// A small negative age (receipt jitter, sub-epoch drift) is within tolerance and
// is consumed; otherwise normal stream timing would drop corrections.
TEST(RealtimeSsrFreshnessTest, MildFutureRecordAccepted) {
    auto& store = SsrCorrectionStore::getInstance();
    store.clear();
    // record only 20 s ahead of the query; max_age 30 s -> mild future -> accept.
    store.pushClock(makeClock("G07", 0.05, GnssTime{2300, 120.0}), SsrHeader{});
    auto q = store.getOrbitClock("G07", GnssTime{2300, 100.0}, /*max_age_s=*/30.0);
    ASSERT_TRUE(q.has_value());
    EXPECT_TRUE(q->has_clock);
    EXPECT_NEAR(q->c0, 0.05, 1e-12);
    store.clear();
}

// ===========================================================================
// (2) Fit-interval guard: an ephemeris extrapolated beyond about twice its
// broadcast fit interval is rejected (the satellite is skipped) instead of
// giving a wrong orbit/clock.  Per-system windows: GPS/QZSS 4 h, GAL/BDS 2 h,
// GLONASS 0.5 h.
// ===========================================================================

BroadcastEphemeris gpsEph(const std::string& sat = "G07") {
    BroadcastEphemeris eph;
    eph.satellite = sat;
    eph.system    = System::GPS;
    eph.iode      = 10;
    auto& k = eph.kepler;
    k.toe = GnssTime{2300, 7200.0};
    k.toc = GnssTime{2300, 7200.0};
    k.sqrt_a = 5153.6; k.e = 0.0;
    k.i0 = 0.96; k.omega0 = 0.5; k.omega = 0.3; k.m0 = 0.2;
    return eph;
}

// Just inside the GPS 4 h window: orbit resolves. Just beyond it: rejected.
TEST(RealtimeEphStalenessTest, GpsOrbitDroppedBeyondFitWindow) {
    auto& store = LiveEphemerisStore::getInstance();
    store.clear();
    store.insert(gpsEph());

    // toe + 3 h 50 m  (< 4 h) -> still usable.
    auto ok = store.computeSatellitePV("G07", GnssTime{2300, 7200.0 + 3.83 * 3600.0});
    EXPECT_TRUE(ok.has_value());

    // toe + 4 h 10 m  (> 4 h) -> rejected, not silently extrapolated.
    auto stale = store.computeSatellitePV("G07", GnssTime{2300, 7200.0 + 4.17 * 3600.0});
    EXPECT_FALSE(stale.has_value());
    store.clear();
}

// The clock path enforces the same guard, so a stale satellite drops entirely
// and never contributes a wrong clock that the position filter would have to
// reject.
TEST(RealtimeEphStalenessTest, GpsClockDroppedBeyondFitWindow) {
    auto& store = LiveEphemerisStore::getInstance();
    store.clear();
    store.insert(gpsEph());

    EXPECT_TRUE(store.computeSatelliteClock(
        "G07", GnssTime{2300, 7200.0 + 1800.0}).has_value());
    EXPECT_FALSE(store.computeSatelliteClock(
        "G07", GnssTime{2300, 7200.0 + 5.0 * 3600.0}).has_value());
    store.clear();
}

// Galileo's window is tighter (2 h): a 3 h query, inside the GPS window, is
// already rejected for Galileo.  Pins the per-system table, not a global const.
TEST(RealtimeEphStalenessTest, GalileoWindowTighterThanGps) {
    auto& store = LiveEphemerisStore::getInstance();
    store.clear();
    BroadcastEphemeris e = gpsEph("E11");
    e.system = System::Galileo;
    e.iode   = 44;
    store.insert(e);

    // toe + 3 h: inside GPS's 4 h but beyond Galileo's 2 h -> dropped.
    EXPECT_FALSE(store.computeSatellitePV(
        "E11", GnssTime{2300, 7200.0 + 3.0 * 3600.0}).has_value());
    // toe + 1 h: inside Galileo's 2 h -> usable.
    EXPECT_TRUE(store.computeSatellitePV(
        "E11", GnssTime{2300, 7200.0 + 1.0 * 3600.0}).has_value());
    store.clear();
}

// ===========================================================================
// (3) Clock-only IODE gate (RT0-1).  A network or IGS-SSR clock subtype carries
// no orbit IODE, so getOrbitClock reports iode = -1 ("unknown") for it.  The
// downstream clock gate (GnssCorrections) computes
//     ssr_iode  = has_clock ? (clock_iode >= 0 ? clock_iode : ssr->iode) : -1;
//                 (Card 7 pinned eval; clock_iode == -1 on every unstamped
//                  record, so this reduces to ssr->iode here)
//     brdc_iode = live broadcast IODE (>= 0 in RT, -1 in post-processing);
// and applies the SSR clock only when SsrCorrectionStore::iodeMatches passes.
// A default of {0} would collide with a real broadcast IODE and drop every
// network clock in live RT, while post-processing's empty live store
// (brdc_iode = -1) stays permissive and would not show the fault.  These tests
// pin both the store value and the consumed gate behaviour without a PppModel
// chain.
// ===========================================================================

// A standalone clock record reports the -1 "unknown" IODE sentinel, not {0}.
TEST(RealtimeSsrIodeGateTest, ClockOnlyRecordReportsUnknownIode) {
    auto& store = SsrCorrectionStore::getInstance();
    store.clear();
    store.pushClock(makeClock("G07", 0.05, GnssTime{2300, 100.0}), SsrHeader{});
    auto q = store.getOrbitClock("G07", GnssTime{2300, 100.0}, /*max_age_s=*/30.0);
    ASSERT_TRUE(q.has_value());
    EXPECT_TRUE(q->has_clock);
    EXPECT_FALSE(q->has_orbit);      // network emitter pushes clock only (orbit from IGS)
    EXPECT_EQ(q->iode, -1);          // unknown -> gate-permissive (not the {0} default)
    store.clear();
}

// With a populated live ephemeris store (brdc_iode >= 0, the RT condition), a
// clock-only network correction is applied, not dropped.  A {0} default would
// fail iodeMatches against a non-zero broadcast IODE; the last assertion pins
// that as the regression witness.
TEST(RealtimeSsrIodeGateTest, NetworkClockAppliesAgainstPopulatedBroadcastIode) {
    auto& eph_store = LiveEphemerisStore::getInstance();
    auto& ssr_store = SsrCorrectionStore::getInstance();
    eph_store.clear();
    ssr_store.clear();

    // Live RT: broadcast ephemeris present with a real, non-zero IODE (10).
    eph_store.insert(gpsEph("G07"));

    // Network producer pushes a clock-only correction (orbit comes from IGS).
    const GnssTime t{2300, 7200.0};
    ssr_store.pushClock(makeClock("G07", 0.05, t), SsrHeader{});

    const auto ssr = ssr_store.getOrbitClock("G07", t, /*max_age_s=*/30.0);
    ASSERT_TRUE(ssr.has_value());
    ASSERT_TRUE(ssr->has_clock);
    ASSERT_FALSE(ssr->has_orbit);

    // Reproduce the GnssCorrections clock gate (GnssCorrections.cpp: the
    // ssr_iode / brdc_iode / iodeMatches trio) against the shared gate function.
    // The baseline pins to a stamped clock_iode first; an unstamped record (-1,
    // as here) falls through to the orbit anchor, the RT0-1 expression (Card 7).
    const int  ssr_iode  = (ssr && ssr->has_clock)
        ? (ssr->clock_iode >= 0 ? ssr->clock_iode : ssr->iode)
        : -1;
    const auto bc        = eph_store.computeSatelliteClock("G07", t, ssr_iode);
    ASSERT_TRUE(bc.has_value());
    const int  brdc_iode = bc->iode;

    EXPECT_EQ(ssr_iode, -1);         // clock-only -> permissive
    EXPECT_GE(brdc_iode, 0);         // populated live store -> real IODE (the RT trap)
    EXPECT_EQ(brdc_iode, 10);        // gpsEph IODE, unchanged by the -1 re-reference

    // The clock gate keys on clock_iode.  A standalone network clock carries no
    // wire IODE and no orbit, so clock_iode inherits the -1 "unknown" anchor and
    // is permissive, the same result as a gate on ssr->iode (RTF-2, Card 3).
    EXPECT_EQ(ssr->clock_iode, -1);
    // The gate passes, so the network clock is applied.
    EXPECT_TRUE(SsrCorrectionStore::iodeMatches(brdc_iode, ssr->clock_iode));

    // The gate's finiteness condition also holds.  GnssCorrections.cpp applies
    // the clock when std::isfinite(clock_corr), the orbit-echo check and
    // iodeMatches all pass, where clock_corr is the broadcast baseline plus the
    // SSR delta.  An IODE match alone does not prove the clock is usable, so
    // assert a finite, non-zero correction.
    const double applied    = SsrCorrectionStore::applyClockToBias(
        ssr->c0, ssr->c1, ssr->c2, ssr->hr_clock, /*dt=*/0.0);   // query epoch == SSR epoch
    const double clock_corr = bc->bias + applied;                // seconds
    EXPECT_TRUE(std::isfinite(clock_corr));
    EXPECT_GT(std::abs(applied), 0.0);                           // the 0.05 m c0 actually contributes

    // Regression witness: a {0} default would collide here (brdc_iode 10 != 0)
    // and drop the clock.  A change that reintroduces a {0} default fails
    // ClockOnlyRecordReportsUnknownIode; this assertion records the reason.
    EXPECT_FALSE(SsrCorrectionStore::iodeMatches(brdc_iode, /*old default=*/0));

    eph_store.clear();
    ssr_store.clear();
}

// A brdc_rts producer stamps the clock's own broadcast IODE and an orbit
// coherence echo.  getOrbitClock propagates them onto the merged record's
// clock_iode / orbit_iod_ssr / orbit_epoch, and the consumer gates the clock on
// clock_iode: applied on a broadcast match, dropped on a mismatch (RTF-2, Card 3).
TEST(RealtimeSsrIodeGateTest, StampedClockIodePropagatesAndGates) {
    SsrCorrectionStore store;   // owned: no singleton coupling
    SsrClockCorrection c = makeClock("G07", 0.05, GnssTime{2300, 100.0});
    c.iode          = 25;                       // brdc_rts stamp (its own C0 baseline)
    c.orbit_iod_ssr = 3;                        // orbit coherence echo
    c.orbit_epoch   = GnssTime{2300, 90.0};
    store.pushClock(c, SsrHeader{});
    const auto q = store.getOrbitClock("G07", GnssTime{2300, 100.0}, /*max_age_s=*/30.0);
    ASSERT_TRUE(q.has_value());
    EXPECT_TRUE(q->has_clock);
    EXPECT_FALSE(q->has_orbit);
    EXPECT_EQ(q->iode, -1);                     // orbit anchor: unknown (no orbit record)
    EXPECT_EQ(q->clock_iode, 25);               // stamped clock IODE propagates
    EXPECT_EQ(q->orbit_iod_ssr, 3);             // echo carried through the store
    EXPECT_NEAR(q->orbit_epoch.tow, 90.0, 1e-9);
    EXPECT_TRUE(SsrCorrectionStore::iodeMatches(25, q->clock_iode));   // matching broadcast -> apply
    EXPECT_FALSE(SsrCorrectionStore::iodeMatches(24, q->clock_iode));  // mismatch -> drop
}

// Byte-identity: a standard SSR clock (no wire IODE) merged with a standalone
// orbit inherits the orbit's IODE for the clock gate, which matches a gate keyed
// on the orbit anchor (RTF-2, Card 3).
TEST(RealtimeSsrIodeGateTest, StandardClockInheritsOrbitIodeForGate) {
    SsrCorrectionStore store;   // owned
    SsrOrbitCorrection o;
    o.sat           = "G07";
    o.iode          = 30;
    o.received_time = GnssTime{2300, 100.0};
    o.epoch_time    = GnssTime{2300, 100.0};
    store.pushOrbit(o, SsrHeader{});
    store.pushClock(makeClock("G07", 0.05, GnssTime{2300, 100.0}), SsrHeader{});  // clk.iode == -1
    const auto q = store.getOrbitClock("G07", GnssTime{2300, 100.0}, /*max_age_s=*/30.0);
    ASSERT_TRUE(q.has_value());
    EXPECT_TRUE(q->has_orbit);
    EXPECT_TRUE(q->has_clock);
    EXPECT_EQ(q->iode, 30);                     // orbit anchor
    EXPECT_EQ(q->clock_iode, 30);               // inherited (unstamped clock) -> gate == orbit gate
    EXPECT_TRUE(SsrCorrectionStore::iodeMatches(30, q->clock_iode));
    EXPECT_FALSE(SsrCorrectionStore::iodeMatches(31, q->clock_iode));  // mismatch drops orbit and clock
}

// A combined record shares one IOD, so clock_iode == iode and the orbit echo is
// the record's own IOD-SSR / content epoch (RTF-2, Card 3).
TEST(RealtimeSsrIodeGateTest, CombinedPropagatesClockIode) {
    SsrCorrectionStore store;   // owned
    SsrCombinedCorrection cb;
    cb.sat           = "E11";
    cb.iode          = 44;
    cb.c0            = 0.3;
    cb.received_time = GnssTime{2300, 100.0};
    cb.epoch_time    = GnssTime{2300, 95.0};
    cb.iod_ssr       = 6;
    store.pushCombined(cb, SsrHeader{});
    const auto q = store.getOrbitClock("E11", GnssTime{2300, 100.0}, /*max_age_s=*/30.0);
    ASSERT_TRUE(q.has_value());
    EXPECT_EQ(q->iode, 44);
    EXPECT_EQ(q->clock_iode, 44);               // combined: clock shares the orbit IOD
    EXPECT_EQ(q->orbit_iod_ssr, 6);
    EXPECT_NEAR(q->orbit_epoch.tow, 95.0, 1e-9);
    // A combined record is self-coherent: the applied orbit's own IOD-SSR equals
    // the echo by construction, which is the condition the GnssCorrections echo
    // check short-circuits on (no second lookup).  (Card 7)
    EXPECT_EQ(q->applied_orbit_iod_ssr, 6);
    EXPECT_EQ(SsrCorrectionStore::classifyOrbitEcho(*q, q, /*max_skew_s=*/30.0),
              SsrCorrectionStore::OrbitEchoVerdict::Match);
}

// ===========================================================================
// Consumer pinned clock eval and orbit-coherence echo check (Card 7, RTF-2).
// The producer's brdc_rts clock is a delta against broadcast(clock_iode) and
// echoes the RTS orbit it was estimated against.  The consumer (a) evaluates
// the broadcast baseline pinned to that version (current/previous retention
// absorbs producer/consumer cutover skew) and (b) checks the echo against the
// orbit it applies, withholding the clock when the armed skew budget says the
// two products belong to different orbit generations.
// ===========================================================================

// Pinned eval (D7): a stamped clock_iode selects which retained broadcast
// version the baseline evaluates (the previous version across a cutover), and
// the gate drops the clock only when the pinned version is no longer held.
TEST(Card7ConsumerTest, PinnedBaselineEvaluatesStampedEphVersion) {
    auto& eph_store = LiveEphemerisStore::getInstance();
    eph_store.clear();

    // Version 10 (af0 = 1e-5 s), then a cutover to version 11 (af0 = 2e-5 s):
    // insert() demotes v10 to the retained "previous" slot.
    BroadcastEphemeris v10 = gpsEph("G07");
    v10.iode       = 10;
    v10.kepler.af0 = 1e-5;
    BroadcastEphemeris v11 = gpsEph("G07");
    v11.iode        = 11;
    v11.kepler.af0  = 2e-5;
    v11.kepler.toe  = GnssTime{2300, 9000.0};   // fresher toe: pick() prefers v11
    v11.kepler.toc  = GnssTime{2300, 9000.0};
    eph_store.insert(v10);
    eph_store.insert(v11);

    const GnssTime t{2300, 9000.0};

    // The producer stamped the clock against v10; the pinned eval returns the
    // retained previous version (its af0 and its IODE), not the freshest.
    SsrCorrectionStore ssr_store;   // owned
    SsrClockCorrection c;
    c.sat = "G07"; c.c0 = 0.05;
    c.received_time = t; c.epoch_time = t;
    c.iode = 10;
    ssr_store.pushClock(c, SsrHeader{});
    const auto q = ssr_store.getOrbitClock("G07", t, /*max_age_s=*/30.0);
    ASSERT_TRUE(q.has_value());
    EXPECT_EQ(q->clock_iode, 10);

    // The Card-7 baseline selection, restated locally: this test pins the
    // store's pinned-retention contract (computeSatelliteClock honors the pin
    // across a demotion); the production selection expression itself is pinned
    // end-to-end by SsrStoreInjectionTest.Card7PinnedEvalUsesStampedVersion,
    // whose fixture geometry makes a revert flip its assertions.
    const int ssr_iode = (q->clock_iode >= 0) ? q->clock_iode : q->iode;
    EXPECT_EQ(ssr_iode, 10);
    const auto bc = eph_store.computeSatelliteClock("G07", t, ssr_iode);
    ASSERT_TRUE(bc.has_value());
    EXPECT_EQ(bc->iode, 10);                       // pinned to the previous version
    EXPECT_NEAR(bc->bias, 1e-5, 1e-9);             // v10's polynomial, not v11's
    EXPECT_TRUE(SsrCorrectionStore::iodeMatches(bc->iode, q->clock_iode));  // gate passes

    // Un-pinned control: the freshest version answers with iode 11 / af0 2e-5.
    const auto cur = eph_store.computeSatelliteClock("G07", t);
    ASSERT_TRUE(cur.has_value());
    EXPECT_EQ(cur->iode, 11);
    EXPECT_NEAR(cur->bias, 2e-5, 1e-9);

    // Pinned version evicted (a stamp two generations old): pickForSsr falls
    // back to the freshest version, whose reported IODE then fails the gate, so
    // the clock is dropped and the RTA-9 staleness ladder governs (D7).
    const auto gone = eph_store.computeSatelliteClock("G07", t, /*ssr_iode=*/9);
    ASSERT_TRUE(gone.has_value());
    EXPECT_EQ(gone->iode, 11);
    EXPECT_FALSE(SsrCorrectionStore::iodeMatches(gone->iode, 9));

    eph_store.clear();
}

// classifyOrbitEcho truth table: not-stamped short-circuit, missing applied
// orbit, IOD-SSR identity, and the budget-gated content-epoch skew clause.
TEST(Card7ConsumerTest, OrbitEchoClassifyMatrix) {
    using V = SsrCorrectionStore::OrbitEchoVerdict;
    const GnssTime t{2300, 100.0};

    // The clock-bearing record under test (echo oiod=7, oepoch tow=100).
    SsrCorrectionStore::OrbitClockCorrection rec;
    rec.has_clock     = true;
    rec.orbit_iod_ssr = 7;
    rec.orbit_epoch   = t;

    // Applied-orbit records to compare against.
    SsrCorrectionStore::OrbitClockCorrection same;   // same generation
    same.has_orbit             = true;
    same.applied_orbit_iod_ssr = 7;
    same.orbit_epoch_time      = t;
    SsrCorrectionStore::OrbitClockCorrection other = same;  // different IOD-SSR
    other.applied_orbit_iod_ssr = 8;
    SsrCorrectionStore::OrbitClockCorrection skewed = same; // same IOD, 20 s older content
    skewed.orbit_epoch_time = GnssTime{2300, 80.0};
    SsrCorrectionStore::OrbitClockCorrection clock_only;    // no orbit part at all
    clock_only.has_clock = true;

    // No echo on the record -> nothing to verify, regardless of the applied side.
    SsrCorrectionStore::OrbitClockCorrection unstamped = rec;
    unstamped.orbit_iod_ssr = -1;
    EXPECT_EQ(SsrCorrectionStore::classifyOrbitEcho(unstamped, other, 10.0), V::NotStamped);
    EXPECT_EQ(SsrCorrectionStore::classifyOrbitEcho(unstamped, std::nullopt, 10.0), V::NotStamped);

    // Echo present but the consumer applies no SSR orbit at all.
    EXPECT_EQ(SsrCorrectionStore::classifyOrbitEcho(rec, std::nullopt, 10.0), V::NoAppliedOrbit);
    EXPECT_EQ(SsrCorrectionStore::classifyOrbitEcho(rec, clock_only,   10.0), V::NoAppliedOrbit);

    // Identity: different orbit update generation.
    EXPECT_EQ(SsrCorrectionStore::classifyOrbitEcho(rec, other, 10.0), V::IodSsrMismatch);
    EXPECT_EQ(SsrCorrectionStore::classifyOrbitEcho(rec, other,  0.0), V::IodSsrMismatch);  // counted even with the budget unset

    // Content-epoch skew: evaluated only when the budget is armed (> 0).
    EXPECT_EQ(SsrCorrectionStore::classifyOrbitEcho(rec, skewed, 10.0), V::EpochSkew);   // 20 s > 10 s
    EXPECT_EQ(SsrCorrectionStore::classifyOrbitEcho(rec, skewed, 30.0), V::Match);       // 20 s <= 30 s
    EXPECT_EQ(SsrCorrectionStore::classifyOrbitEcho(rec, skewed,  0.0), V::Match);       // budget unset -> skew clause off
    EXPECT_EQ(SsrCorrectionStore::classifyOrbitEcho(rec, same,   10.0), V::Match);
}

// checkOrbitEcho tallies per-(sat|reason) on the store; Match never counts;
// clear() resets the tallies but the armed budget (configuration) survives.
TEST(Card7ConsumerTest, OrbitEchoCheckCountsAndClear) {
    using V = SsrCorrectionStore::OrbitEchoVerdict;
    const GnssTime t{2300, 100.0};

    SsrCorrectionStore store;   // owned: plays the clock store (holds the counters)
    SsrCorrectionStore::OrbitClockCorrection rec;
    rec.has_clock     = true;
    rec.orbit_iod_ssr = 7;
    rec.orbit_epoch   = t;
    SsrCorrectionStore::OrbitClockCorrection other;
    other.has_orbit             = true;
    other.applied_orbit_iod_ssr = 8;
    other.orbit_epoch_time      = t;

    // Budget unset: identity mismatches still classify and count (count-only mode).
    EXPECT_EQ(store.checkOrbitEcho("G07", rec, other), V::IodSsrMismatch);
    EXPECT_EQ(store.checkOrbitEcho("G07", rec, std::nullopt), V::NoAppliedOrbit);
    auto counts = store.orbitEchoMismatchCounts();
    EXPECT_EQ(counts["G07|iod"],     1u);
    EXPECT_EQ(counts["G07|noorbit"], 1u);

    // Arm the budget: the skew clause activates and counts under "skew".
    store.setOrbitEchoMaxSkew(10.0);
    EXPECT_NEAR(store.orbitEchoMaxSkew(), 10.0, 1e-12);
    SsrCorrectionStore::OrbitClockCorrection skewed = other;
    skewed.applied_orbit_iod_ssr = 7;                       // identity agrees
    skewed.orbit_epoch_time      = GnssTime{2300, 80.0};    // 20 s > 10 s budget
    EXPECT_EQ(store.checkOrbitEcho("G07", rec, skewed), V::EpochSkew);
    counts = store.orbitEchoMismatchCounts();
    EXPECT_EQ(counts["G07|skew"], 1u);

    // A clean match never counts.
    SsrCorrectionStore::OrbitClockCorrection same = skewed;
    same.orbit_epoch_time = t;
    EXPECT_EQ(store.checkOrbitEcho("G07", rec, same), V::Match);
    EXPECT_EQ(store.orbitEchoMismatchCounts().size(), 3u);

    // clear(): tallies are per-run diagnostics; the budget is configuration.
    store.clear();
    EXPECT_TRUE(store.orbitEchoMismatchCounts().empty());
    EXPECT_NEAR(store.orbitEchoMaxSkew(), 10.0, 1e-12);
}

// The merge also carries the applied orbit's own IOD-SSR for a standalone orbit
// record (the split-store consumer's orbit-side lookup).
TEST(Card7ConsumerTest, StandaloneOrbitCarriesOwnIodSsr) {
    SsrCorrectionStore store;   // owned: plays the orbit store
    SsrOrbitCorrection o;
    o.sat           = "G07";
    o.iode          = 30;
    o.iod_ssr       = 9;
    o.received_time = GnssTime{2300, 100.0};
    o.epoch_time    = GnssTime{2300,  95.0};
    store.pushOrbit(o, SsrHeader{});
    const auto q = store.getOrbitClock("G07", GnssTime{2300, 100.0}, /*max_age_s=*/30.0);
    ASSERT_TRUE(q.has_value());
    EXPECT_TRUE(q->has_orbit);
    EXPECT_EQ(q->applied_orbit_iod_ssr, 9);
    EXPECT_NEAR(q->orbit_epoch_time.tow, 95.0, 1e-9);
    EXPECT_EQ(q->orbit_iod_ssr, -1);   // a bare orbit record carries no echo (the echo rides on the clock)
}

// ===========================================================================
// (4) Per-stream SSR ingest filter (RTA-8(b)).  The mapping from record kind to
// push decision is a pure static (ssrPushAction) factored out of feedRtcmFrame,
// so the policy is pinned without a parser or engine; the combined-record
// decomposition is pinned end-to-end through an owned store.
// ===========================================================================

// Default policy All: every record kind pushes (the byte-identical legacy path).
TEST(SsrIngestFilterTest, AllPolicyPushesEveryKind) {
    using RTE = RealTimeEngine;
    for (auto kind : { RTE::SsrRecordKind::Orbit,    RTE::SsrRecordKind::Clock,
                       RTE::SsrRecordKind::Combined, RTE::SsrRecordKind::CodeBias,
                       RTE::SsrRecordKind::PhaseBias, RTE::SsrRecordKind::Ura,
                       RTE::SsrRecordKind::HighRateClock }) {
        EXPECT_EQ(RTE::ssrPushAction(RTE::SsrAcceptPolicy::All, kind),
                  RTE::SsrPushAction::Push);
    }
}

// OrbitOnly: orbit and URA push; combined decomposes; every clock- or
// bias-bearing kind drops, including the high-rate clock (clock content).
TEST(SsrIngestFilterTest, OrbitOnlyPolicyMatrix) {
    using RTE = RealTimeEngine;
    const auto p = RTE::SsrAcceptPolicy::OrbitOnly;
    EXPECT_EQ(RTE::ssrPushAction(p, RTE::SsrRecordKind::Orbit),
              RTE::SsrPushAction::Push);
    EXPECT_EQ(RTE::ssrPushAction(p, RTE::SsrRecordKind::Ura),
              RTE::SsrPushAction::Push);
    EXPECT_EQ(RTE::ssrPushAction(p, RTE::SsrRecordKind::Combined),
              RTE::SsrPushAction::DecomposeOrbit);
    EXPECT_EQ(RTE::ssrPushAction(p, RTE::SsrRecordKind::Clock),
              RTE::SsrPushAction::Drop);
    EXPECT_EQ(RTE::ssrPushAction(p, RTE::SsrRecordKind::CodeBias),
              RTE::SsrPushAction::Drop);
    EXPECT_EQ(RTE::ssrPushAction(p, RTE::SsrRecordKind::PhaseBias),
              RTE::SsrPushAction::Drop);
    EXPECT_EQ(RTE::ssrPushAction(p, RTE::SsrRecordKind::HighRateClock),
              RTE::SsrPushAction::Drop);
}

// A combined orbit+clock record decomposed at the push site yields an
// orbit-only store entry: has_orbit without has_clock, the RAC fields / IODE /
// content epoch carried over, the clock polynomial gone.  The store's
// combined-wins merge logic is not involved, because the combined record never
// enters the store.
TEST(SsrIngestFilterTest, CombinedDecomposesToOrbitOnlyPush) {
    SsrCombinedCorrection comb;
    comb.sat           = "G07";
    comb.iode          = 42;
    comb.delta_rac     = Eigen::Vector3d{0.5, -0.2, 0.1};
    comb.delta_rac_dot = Eigen::Vector3d{1e-4, -2e-4, 3e-5};
    comb.c0            = 0.8;              // clock content that must not survive
    comb.c1            = 1e-4;
    comb.c2            = 1e-7;
    comb.received_time = GnssTime{2300, 100.0};
    comb.epoch_time    = GnssTime{2300,  95.0};
    comb.iod_ssr       = 7;

    const SsrOrbitCorrection orb = RealTimeEngine::combinedOrbitPart(comb);
    EXPECT_EQ(orb.sat, "G07");
    EXPECT_EQ(orb.iode, 42);
    EXPECT_TRUE(orb.delta_rac.isApprox(comb.delta_rac));
    EXPECT_TRUE(orb.delta_rac_dot.isApprox(comb.delta_rac_dot));
    EXPECT_EQ(orb.received_time.week, 2300);
    EXPECT_NEAR(orb.received_time.tow, 100.0, 1e-12);
    EXPECT_NEAR(orb.epoch_time.tow,     95.0, 1e-12);
    EXPECT_EQ(orb.iod_ssr, 7);

    SsrCorrectionStore store;   // owned: no singleton coupling
    store.pushOrbit(orb, SsrHeader{});
    const auto q = store.getOrbitClock("G07", GnssTime{2300, 100.0}, 30.0);
    ASSERT_TRUE(q.has_value());
    EXPECT_TRUE(q->has_orbit);
    EXPECT_FALSE(q->has_clock);              // no clock content leaked
    EXPECT_NEAR(q->c0, 0.0, 1e-15);
    EXPECT_EQ(q->iode, 42);
    EXPECT_TRUE(q->delta_rac.isApprox(comb.delta_rac));
    EXPECT_NEAR(q->orbit_epoch_time.tow, 95.0, 1e-12);  // orbit dt base preserved
}

// The combined-to-orbit decomposition carries the SSR reference-point tag.  A
// legacy (APC-referenced) 1060/1066 record decomposed under the orbit_only
// ingest policy is still an APC-referenced orbit; losing the tag would
// re-enable the satellite-PCO double count under
// realtime.honor_ssr_reference_point (RTB-5(b)).
TEST(SsrIngestFilterTest, CombinedDecompositionPreservesReferencePoint) {
    SsrCombinedCorrection comb;
    comb.sat             = "E11";
    comb.iode            = 44;
    comb.received_time   = GnssTime{2300, 100.0};
    comb.epoch_time      = GnssTime{2300, 100.0};
    comb.reference_point = SsrReferencePoint::APC;   // legacy dialect

    const SsrOrbitCorrection orb = RealTimeEngine::combinedOrbitPart(comb);
    EXPECT_EQ(orb.reference_point, SsrReferencePoint::APC);

    SsrCorrectionStore store;   // owned: no singleton coupling
    store.pushOrbit(orb, SsrHeader{});
    const auto q = store.getOrbitClock("E11", GnssTime{2300, 100.0}, 30.0);
    ASSERT_TRUE(q.has_value());
    EXPECT_TRUE(q->has_orbit);
    EXPECT_EQ(q->orbit_reference_point, SsrReferencePoint::APC);

    // And the default stays CoM so every non-decoder construction is unchanged.
    SsrCombinedCorrection def;
    EXPECT_EQ(RealTimeEngine::combinedOrbitPart(def).reference_point,
              SsrReferencePoint::CoM);
}

// Config-string parsing: "all"/empty → All, "orbit_only" (any case) →
// OrbitOnly, unknown → All with recognized=false so the loader can warn.
TEST(SsrIngestFilterTest, ParseAcceptPolicy) {
    using RTE = RealTimeEngine;
    bool ok = false;
    EXPECT_EQ(RTE::parseSsrAcceptPolicy("all", &ok), RTE::SsrAcceptPolicy::All);
    EXPECT_TRUE(ok);
    EXPECT_EQ(RTE::parseSsrAcceptPolicy("", &ok), RTE::SsrAcceptPolicy::All);
    EXPECT_TRUE(ok);
    EXPECT_EQ(RTE::parseSsrAcceptPolicy("orbit_only", &ok), RTE::SsrAcceptPolicy::OrbitOnly);
    EXPECT_TRUE(ok);
    EXPECT_EQ(RTE::parseSsrAcceptPolicy("ORBIT_ONLY", &ok), RTE::SsrAcceptPolicy::OrbitOnly);
    EXPECT_TRUE(ok);
    EXPECT_EQ(RTE::parseSsrAcceptPolicy("bogus", &ok), RTE::SsrAcceptPolicy::All);
    EXPECT_FALSE(ok);
}

// ===========================================================================
// (5) Clock staleness ladder (RTA-9(b)).  clockHoldWindow and classifyClockAge
// are the shared decision helpers of GnssCorrections (apply window) and
// MeasurementModel (R-inflation rung); the store-level test pins that a held
// clock is retrievable through the widened window.
// ===========================================================================

TEST(SsrClockStaleHoldTest, WindowAndClassification) {
    using Store = SsrCorrectionStore;
    // Factor 0 (default): the window is the base gate and the rung is off.
    EXPECT_DOUBLE_EQ(Store::clockHoldWindow(30.0, 0.0), 30.0);
    // Factor 3: window widens; factors ≤ 1 cannot shrink the base gate.
    EXPECT_DOUBLE_EQ(Store::clockHoldWindow(30.0, 3.0), 90.0);
    EXPECT_DOUBLE_EQ(Store::clockHoldWindow(30.0, 0.5), 30.0);

    using Rung = Store::ClockAgeRung;
    // Rung off (factor 0): the two-state gate.
    EXPECT_EQ(Store::classifyClockAge(20.0, 30.0, 0.0), Rung::Fresh);
    EXPECT_EQ(Store::classifyClockAge(45.0, 30.0, 0.0), Rung::Rejected);
    // Rung armed (factor 3): fresh ≤ 30 < held ≤ 90 < rejected.
    EXPECT_EQ(Store::classifyClockAge(30.0, 30.0, 3.0), Rung::Fresh);      // boundary: still fresh
    EXPECT_EQ(Store::classifyClockAge(30.1, 30.0, 3.0), Rung::StaleHeld);
    EXPECT_EQ(Store::classifyClockAge(90.0, 30.0, 3.0), Rung::StaleHeld);  // boundary: last held age
    EXPECT_EQ(Store::classifyClockAge(90.1, 30.0, 3.0), Rung::Rejected);
    // Future side mirrors freshEnough: mild negative fresh, far-future rejected.
    EXPECT_EQ(Store::classifyClockAge(-20.0,  30.0, 3.0), Rung::Fresh);
    EXPECT_EQ(Store::classifyClockAge(-100.0, 30.0, 3.0), Rung::Rejected);
}

// Store-level: a clock aged past ssr_max_age but inside the hold window is
// rejected by the base gate yet retrievable through clockHoldWindow, the lookup
// GnssCorrections performs when the rung is armed.
TEST(SsrClockStaleHoldTest, HeldClockRetrievableThroughWiderWindow) {
    SsrCorrectionStore store;   // owned: no singleton coupling
    SsrClockCorrection clk = makeClock("G07", 0.05, GnssTime{2300, 100.0});
    clk.c1 = 1e-3;              // drift, so the held C0+C1·dt evaluation matters
    store.pushClock(clk, SsrHeader{});

    const GnssTime q{2300, 145.0};   // age 45 s
    EXPECT_FALSE(store.getOrbitClock("G07", q, /*max_age_s=*/30.0).has_value());

    const double window = SsrCorrectionStore::clockHoldWindow(30.0, 3.0);
    const auto held = store.getOrbitClock("G07", q, window);
    ASSERT_TRUE(held.has_value());
    EXPECT_TRUE(held->has_clock);
    EXPECT_NEAR(held->c0, 0.05, 1e-12);
    EXPECT_NEAR(held->c1, 1e-3, 1e-15);
    EXPECT_EQ(SsrCorrectionStore::classifyClockAge(45.0, 30.0, 3.0),
              SsrCorrectionStore::ClockAgeRung::StaleHeld);
}

// ===========================================================================
// (6) Ultra-rapid (ULT) orbit naming (RTB-4).  ProductResolver's public naming
// statics pin the IGS long-name grammar, the per-analysis-centre sampling
// token, the token fallback pair and the start-hour walk-back candidate
// sequences, all without network access.  Span convention (documented at the
// statics, and the walk-back depends on it): 02D = 48 h from the filename
// epoch, 24 h observed then 24 h predicted, release at about epoch + 27 h.
// ===========================================================================

// Per-centre sampling token: COD publishes 05M ultra-rapids, the IGS combined
// product is 15M; unknown centres default to the IGS-combined 15M.  The
// alternate token is always the other member of the {05M, 15M} pair.
TEST(UltOrbitNamingTest, PerSourceSamplingTokenAndAlternate) {
    EXPECT_EQ(ProductResolver::ultSamplingToken("COD"), "05M");
    EXPECT_EQ(ProductResolver::ultSamplingToken("IGS"), "15M");
    EXPECT_EQ(ProductResolver::ultSamplingToken("GFZ"), "15M");   // unknown -> default
    EXPECT_EQ(ProductResolver::ultSamplingToken(""),    "15M");

    EXPECT_EQ(ProductResolver::ultAlternateSamplingToken("05M"), "15M");
    EXPECT_EQ(ProductResolver::ultAlternateSamplingToken("15M"), "05M");
}

// Long-name grammar: <SRC>0OPSULT_<YYYYDDDHHMM>_02D_<SMP>_ORB.SP3.gz with
// zero-padded DOY/hour and MM fixed at "00".
TEST(UltOrbitNamingTest, FilenameGrammar) {
    EXPECT_EQ(ProductResolver::buildUltOrbFilename("IGS", 2024, 129, 0, "15M"),
              "IGS0OPSULT_20241290000_02D_15M_ORB.SP3.gz");
    EXPECT_EQ(ProductResolver::buildUltOrbFilename("COD", 2024, 129, 18, "05M"),
              "COD0OPSULT_20241291800_02D_05M_ORB.SP3.gz");
    EXPECT_EQ(ProductResolver::buildUltOrbFilename("COD", 2026, 5, 6, "05M"),
              "COD0OPSULT_20260050600_02D_05M_ORB.SP3.gz");
}

// Obs-day walk-back (anchor (D, 00)): the first candidate's observed half
// covers the whole processing day; each subsequent candidate is 6 h earlier
// across the previous day, degrading day-D coverage toward the predicted
// half.  Six candidates cover the worst-case live release latency
// (newest published start epoch up to ~33 h old just before a release).
TEST(UltOrbitNamingTest, ObsDayWalkBackSequence) {
    const auto c = ProductResolver::ultStartCandidates(2024, 129, 0);
    ASSERT_EQ(c.size(), static_cast<size_t>(ProductResolver::kUltWalkBackTries));
    const int expected[][3] = {
        { 2024, 129,  0 },
        { 2024, 128, 18 },
        { 2024, 128, 12 },
        { 2024, 128,  6 },
        { 2024, 128,  0 },
        { 2024, 127, 18 },
    };
    for (size_t i = 0; i < c.size(); ++i) {
        EXPECT_EQ(c[i].year, expected[i][0]) << "candidate " << i;
        EXPECT_EQ(c[i].doy,  expected[i][1]) << "candidate " << i;
        EXPECT_EQ(c[i].hour, expected[i][2]) << "candidate " << i;
    }
}

// Previous-day margin walk-back (anchor (D-1, 18)): the freshest file whose
// start precedes D 00:00, walking back in the same 6 h steps.
TEST(UltOrbitNamingTest, PrevDayMarginCandidateSequence) {
    const auto c = ProductResolver::ultStartCandidates(2024, 128, 18);
    ASSERT_EQ(c.size(), static_cast<size_t>(ProductResolver::kUltWalkBackTries));
    const int expected[][3] = {
        { 2024, 128, 18 },
        { 2024, 128, 12 },
        { 2024, 128,  6 },
        { 2024, 128,  0 },
        { 2024, 127, 18 },
        { 2024, 127, 12 },
    };
    for (size_t i = 0; i < c.size(); ++i) {
        EXPECT_EQ(c[i].year, expected[i][0]) << "candidate " << i;
        EXPECT_EQ(c[i].doy,  expected[i][1]) << "candidate " << i;
        EXPECT_EQ(c[i].hour, expected[i][2]) << "candidate " << i;
    }
}

// Day and year boundaries: DOY 1 walks into the previous year's final day
// (leap-aware via shiftDate).  2023 is not a leap year (365); 2024 is (366).
TEST(UltOrbitNamingTest, WalkBackCrossesYearBoundary) {
    const auto c24 = ProductResolver::ultStartCandidates(2024, 1, 0);
    ASSERT_GE(c24.size(), 2u);
    EXPECT_EQ(c24[1].year, 2023);
    EXPECT_EQ(c24[1].doy,  365);
    EXPECT_EQ(c24[1].hour, 18);

    const auto c25 = ProductResolver::ultStartCandidates(2025, 1, 0);
    ASSERT_GE(c25.size(), 2u);
    EXPECT_EQ(c25[1].year, 2024);
    EXPECT_EQ(c25[1].doy,  366);   // leap year
    EXPECT_EQ(c25[1].hour, 18);
}

} // namespace
} // namespace GnssProcessor
