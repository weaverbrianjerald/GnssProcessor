/**
 * @file test_realtime_seams.cpp
 * @brief Stream-level real-time invariants the position grader cannot see.
 *
 * The real-time replay tier (manifest_realtime.json) grades the converged
 * position vs an independent site coordinate -- a strong end-to-end guard, but
 * blind to two failure modes that can still produce a *plausible-looking*
 * position while being wrong:
 *
 *   1. An old observation consuming a FUTURE-stamped SSR correction and
 *      extrapolating the orbit/clock backward (the obs<->SSR sync hole: high-rate
 *      obs back up behind real-time while sparse corrections stay fresh, so an
 *      old obs queries a fresh correction with a large *negative* age).
 *      Contract lives in SsrCorrectionStore freshness gating.
 *
 *   2. A broadcast ephemeris extrapolated far beyond its fit interval (a
 *      never-refreshed sat, or obs/eph desync) silently yielding a junk orbit or
 *      clock instead of dropping the satellite.  Contract lives in the
 *      LiveEphemerisStore fit-window guard (the BSSD epoch-blowup / RT
 *      model-degradation root).
 *
 * Both are unit-constructible without a PppModel / RINEX chain, deterministic,
 * and complementary to the position-accuracy regression cell.  See
 * docs/test_suite.md (Real-time replay tier).
 */

#include <gtest/gtest.h>

#include "products/SsrCorrectionStore.hpp"
#include "products/BroadcastEphemerisTypes.hpp"
#include "products/LiveEphemerisStore.hpp"
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
// (1) obs<->SSR sync: freshness gating is three-region, and the FUTURE side is
// the one the obs-backlog bug exercised.  age = query - record.received_time:
//   age >  max_age   -> stale past record      -> REJECT  (covered elsewhere)
//   -max_age<=age<0  -> mild future (jitter)    -> ACCEPT
//   age < -max_age   -> record far in the future-> REJECT  (the backlog hole)
// Tested as a black box through getOrbitClock so it pins the consumed behaviour,
// not a private helper.
// ===========================================================================

// A correction received well AFTER the obs being processed (old obs, fresh
// correction) must be rejected -- never extrapolated backward onto stale obs.
TEST(RealtimeSsrFreshnessTest, FutureRecordBeyondMaxAgeRejected) {
    auto& store = SsrCorrectionStore::getInstance();
    store.clear();
    // record stamped 100 s in the FUTURE relative to the query; max_age 30 s.
    store.pushClock(makeClock("G07", 0.05, GnssTime{2300, 200.0}), SsrHeader{});
    auto q = store.getOrbitClock("G07", GnssTime{2300, 100.0}, /*max_age_s=*/30.0);
    EXPECT_FALSE(q.has_value());
    store.clear();
}

// A small negative age (receipt jitter / sub-epoch drift) is within tolerance
// and must still be consumed -- otherwise normal stream timing would drop
// corrections.
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
// (2) Fit-interval guard: an ephemeris extrapolated beyond ~2x its broadcast fit
// interval is rejected (sat skipped) rather than emitting a silently-wrong
// orbit/clock.  Per-system windows: GPS/QZSS 4 h, GAL/BDS 2 h, GLONASS 0.5 h.
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

// The clock path enforces the same guard (so a stale sat drops entirely, never
// contributing a junk clock that the position filter would have to reject).
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

// Galileo's window is tighter (2 h): a query stale for GPS-standards (3 h) is
// already rejected for Galileo -- pins the per-system table, not a global const.
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

} // namespace
} // namespace GnssProcessor
