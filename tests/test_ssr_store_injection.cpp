/**
 * @file test_ssr_store_injection.cpp
 * @brief DI-seam tests for SsrCorrectionStore injection (PPP-RTK closed-loop harness).
 *
 * The network closed loop feeds a held-out consumer the producer's emitted
 * products from an owned SsrCorrectionStore, not the process-wide singleton.
 * @c setSsrStore() is the dependency-injection seam: a null override (the
 * default) keeps every existing caller on the singleton, byte-identical, while a
 * non-null pointer routes SSR consumption to the injected store.
 *
 * These tests check that contract at the GnssCorrections satellite-clock consume
 * site, the one SSR consumer that can be constructed in a unit test without a full
 * PppModel / RINEX-parser chain.  MeasurementModel and the RealTimeEngine
 * forwarder share the same override member and resolver, and are exercised end to
 * end by the network closed-loop driver.  The first two tests use owned stores
 * only; the consumer-behaviour tests also use the LiveEphemerisStore singleton, which each of
 * them clears before use (and, where it inserts ephemerides, again at the end).
 */

#include <gtest/gtest.h>

#include "corrections/GnssCorrections.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "products/BroadcastEphemerisTypes.hpp"
#include "products/LiveEphemerisStore.hpp"
#include "logging/LogManager.hpp"
#include "common/Constants.hpp"
#include "rtcm/RtcmTypes.hpp"

namespace GnssProcessor {
namespace {

const GnssTime kT{2313, 259200.0};

SsrClockCorrection makeClock(const std::string& sat, double c0_m) {
    SsrClockCorrection rec;
    rec.sat           = sat;
    rec.c0            = c0_m;
    rec.received_time = kT;
    rec.epoch_time    = kT;
    rec.iod_ssr       = 1;
    return rec;
}

SsrHeader makeHeader() {
    SsrHeader h;
    h.epoch_time = kT;
    h.iod_ssr    = 1;
    h.system     = System::GPS;
    return h;
}

// An injected owned store routes the SSR clock overlay through the seam: with no
// precise CLK product loaded (post-processing minimal ctor), the returned clock
// is the emitted c0 (metres) converted to seconds.  Pointing the seam at a
// different (empty) owned store reverts to "no correction": the override is a
// plain swap, and the owned stores do not leak into one another or the singleton.
TEST(SsrStoreInjectionTest, RoutesToInjectedStore) {
    LogManager log;
    GnssCorrections corr(log);   // minimal ctor: no precise clocks

    SsrCorrectionStore with_clock, empty;
    with_clock.pushClock(makeClock("G01", 12.5), makeHeader());

    corr.setSsrStore(&with_clock);
    EXPECT_NEAR(corr.getSatelliteClockCorrection("G01", kT),
                12.5 / Constants::SPEED_OF_LIGHT, 1e-12);

    corr.setSsrStore(&empty);
    EXPECT_NEAR(corr.getSatelliteClockCorrection("G01", kT), 0.0, 1e-15);
}

// The override is per-instance, non-owning state: two GnssCorrections pointed at
// different owned stores consume independently (no global coupling).
TEST(SsrStoreInjectionTest, OverrideIsPerInstance) {
    LogManager log;
    GnssCorrections a(log), b(log);

    SsrCorrectionStore sa, sb;   // sb stays empty
    sa.pushClock(makeClock("G01", 9.0), makeHeader());

    a.setSsrStore(&sa);
    b.setSsrStore(&sb);

    EXPECT_NEAR(a.getSatelliteClockCorrection("G01", kT),
                9.0 / Constants::SPEED_OF_LIGHT, 1e-12);
    EXPECT_NEAR(b.getSatelliteClockCorrection("G01", kT), 0.0, 1e-15);
}

// ===========================================================================
// The two consumer behaviours, end to end through
// getSatelliteClockCorrection: (a) D7 pinned broadcast-baseline eval, (b) the
// orbit-coherence echo withhold under the store-armed skew budget.
// ===========================================================================

// Valid GPS Kepler skeleton with toe/toc at the given time (queries at kT fall well
// inside the 4 h fit window).
BroadcastEphemeris gpsEphAt(const std::string& sat, int iode, double af0,
                            const GnssTime& toe) {
    BroadcastEphemeris eph;
    eph.satellite = sat;
    eph.system    = System::GPS;
    eph.iode      = iode;
    auto& k = eph.kepler;
    k.toe = toe; k.toc = toe;
    k.sqrt_a = 5153.6; k.e = 0.0;
    k.i0 = 0.96; k.omega0 = 0.5; k.omega = 0.3; k.m0 = 0.2;
    k.af0 = af0;
    return eph;
}

// (a) D7 pinned eval: the returned clock is broadcast(clock_iode) + SSR delta,
// using the retained previous ephemeris version when the producer stamped that
// one.  A stamp whose version is no longer held drops the clock to broadcast grade.
TEST(SsrStoreInjectionTest, PinnedEvalUsesStampedVersion) {
    auto& eph_store = LiveEphemerisStore::getInstance();
    eph_store.clear();
    // v10 (af0 1e-5 s, toe kT-600) then v11 (af0 2e-5 s, toe at the query
    // epoch kT), so v10 is the retained "previous".  The toe values make the
    // un-pinned selection (pickForSsr(-1) -> pick() -> nearest toe) land on v11
    // (distance 0 against 600 s).  If the pinned-eval expression reverts
    // to ssr->iode == -1 on a clock-only merge, the lookup evaluates v11, fails
    // the clock_iode=10 gate, and both assertions below fail.
    eph_store.insert(gpsEphAt("G01", 10, 1e-5, GnssTime{kT.week, kT.tow - 600.0}));
    eph_store.insert(gpsEphAt("G01", 11, 2e-5, kT));

    LogManager log;
    GnssCorrections corr(log);   // minimal ctor: no precise CLK -> broadcast baseline

    SsrCorrectionStore clock_store;
    SsrClockCorrection c = makeClock("G01", 12.5);
    c.iode = 10;                 // producer's C0 baseline = broadcast(v10)
    clock_store.pushClock(c, makeHeader());
    corr.setSsrStore(&clock_store);

    // Baseline pins to v10: af0 1e-5 s, plus the 12.5 m delta applied through
    // the passing gate.  Un-pinned, the lookup would pick v11 (af0 2e-5) and drop
    // the clock on the 11 != 10 gate: a different value, and broadcast_grade set.
    bool broadcast_grade = true;
    EXPECT_NEAR(corr.getSatelliteClockCorrection("G01", kT, &broadcast_grade),
                1e-5 + 12.5 / Constants::SPEED_OF_LIGHT, 1e-12);
    EXPECT_FALSE(broadcast_grade);   // SSR-corrected -> cm-grade

    // A stamp two generations old (v9, not held): the pinned version is absent,
    // so pickForSsr falls back to pick() (nearest toe, which at kT is v11),
    // whose IODE (11) fails the gate against the stamp (9).  The clock is not
    // applied: the return is the bare broadcast polynomial (2e-5, no 12.5 m SSR
    // delta), flagged broadcast grade.
    SsrClockCorrection stale = makeClock("G01", 12.5);
    stale.iode = 9;
    clock_store.pushClock(stale, makeHeader());
    broadcast_grade = false;
    EXPECT_NEAR(corr.getSatelliteClockCorrection("G01", kT, &broadcast_grade),
                2e-5, 1e-12);        // fallback baseline only; no SSR delta applied
    EXPECT_TRUE(broadcast_grade);    // phase must not ride a metre-level clock

    eph_store.clear();
}

// (b) Orbit-coherence echo: count-only by default (clock still applied), the
// withhold arms with the store's skew budget, and a matching applied orbit
// passes untouched.  The orbit side reads a different store than the clock,
// which is the RTA-8 split routing the echo seam exists for.
TEST(SsrStoreInjectionTest, OrbitEchoWithholdUnderBudget) {
    LiveEphemerisStore::getInstance().clear();   // permissive gate (post-processing)

    LogManager log;
    GnssCorrections corr(log);

    // Clock store: a brdc_rts-style record echoing orbit generation oiod=7.
    SsrCorrectionStore clock_store;
    SsrClockCorrection c = makeClock("G01", 12.5);
    c.orbit_iod_ssr = 7;
    c.orbit_epoch   = kT;
    clock_store.pushClock(c, makeHeader());
    corr.setSsrStore(&clock_store);

    // Orbit store: the consumer's overlay applies a different generation (8).
    SsrCorrectionStore orbit_store;
    SsrOrbitCorrection o;
    o.sat = "G01"; o.iode = -1; o.iod_ssr = 8;
    o.received_time = kT; o.epoch_time = kT;
    orbit_store.pushOrbit(o, SsrHeader{});
    corr.setOrbitEchoStore(&orbit_store);

    // Budget unset (default): the mismatch is counted, and the clock still applies.
    EXPECT_NEAR(corr.getSatelliteClockCorrection("G01", kT),
                12.5 / Constants::SPEED_OF_LIGHT, 1e-12);
    EXPECT_EQ(clock_store.orbitEchoMismatchCounts()["G01|iod"], 1u);

    // Budget armed on the clock store: the same mismatch now withholds the clock.
    clock_store.setOrbitEchoMaxSkew(30.0);
    EXPECT_NEAR(corr.getSatelliteClockCorrection("G01", kT), 0.0, 1e-15);
    EXPECT_EQ(clock_store.orbitEchoMismatchCounts()["G01|iod"], 2u);

    // No applied orbit at all (empty orbit store) is a mismatch class of its own.
    SsrCorrectionStore empty_orbit;
    corr.setOrbitEchoStore(&empty_orbit);
    EXPECT_NEAR(corr.getSatelliteClockCorrection("G01", kT), 0.0, 1e-15);
    EXPECT_EQ(clock_store.orbitEchoMismatchCounts()["G01|noorbit"], 1u);

    // Coherent orbit (same generation, same content epoch): applied cleanly,
    // no new tallies.
    SsrCorrectionStore good_orbit;
    o.iod_ssr = 7;
    good_orbit.pushOrbit(o, SsrHeader{});
    corr.setOrbitEchoStore(&good_orbit);
    EXPECT_NEAR(corr.getSatelliteClockCorrection("G01", kT),
                12.5 / Constants::SPEED_OF_LIGHT, 1e-12);
    const auto counts = clock_store.orbitEchoMismatchCounts();
    EXPECT_EQ(counts.at("G01|iod"), 2u);
    EXPECT_EQ(counts.at("G01|noorbit"), 1u);
    EXPECT_EQ(counts.size(), 2u);
}

// The echo contract concerns the orbit the overlay applies.  An orbit record
// whose IOD-SSR matches the echo but whose referenced broadcast version is no
// longer held is not applied by Geometry's overlay gate; the echo check
// replicates that gate (through the same pinned pick) and classifies the record
// as "no applied orbit" instead of a false Match.
TEST(SsrStoreInjectionTest, OrbitEchoRequiresApplicableOrbit) {
    auto& eph_store = LiveEphemerisStore::getInstance();
    eph_store.clear();
    // The live store holds broadcast version 20 and no other.
    eph_store.insert(gpsEphAt("G01", 20, 1e-5, kT));

    LogManager log;
    GnssCorrections corr(log);

    // Clock echoes orbit generation 7; the pin (clock_iode 20) matches the
    // held broadcast, so the clock gate itself passes.
    SsrCorrectionStore clock_store;
    clock_store.setOrbitEchoMaxSkew(30.0);   // withhold armed
    SsrClockCorrection c = makeClock("G01", 12.5);
    c.iode          = 20;
    c.orbit_iod_ssr = 7;
    c.orbit_epoch   = kT;
    clock_store.pushClock(c, makeHeader());
    corr.setSsrStore(&clock_store);

    // Orbit record with the matching generation (7) but stamped against
    // broadcast version 25, which the store does not hold: the overlay would
    // not apply it, so record presence must not count as coherence.
    SsrCorrectionStore orbit_store;
    SsrOrbitCorrection o;
    o.sat = "G01"; o.iode = 25; o.iod_ssr = 7;
    o.received_time = kT; o.epoch_time = kT;
    orbit_store.pushOrbit(o, SsrHeader{});
    corr.setOrbitEchoStore(&orbit_store);

    bool broadcast_grade = false;
    EXPECT_NEAR(corr.getSatelliteClockCorrection("G01", kT, &broadcast_grade),
                1e-5, 1e-12);       // clock withheld -> bare broadcast baseline
    EXPECT_TRUE(broadcast_grade);
    EXPECT_EQ(clock_store.orbitEchoMismatchCounts().at("G01|noorbit"), 1u);

    // Same orbit re-stamped against the held version 20: the overlay would
    // apply it, the echo agrees, and the clock goes through.
    o.iode = 20;
    orbit_store.pushOrbit(o, SsrHeader{});
    EXPECT_NEAR(corr.getSatelliteClockCorrection("G01", kT),
                1e-5 + 12.5 / Constants::SPEED_OF_LIGHT, 1e-12);
    EXPECT_EQ(clock_store.orbitEchoMismatchCounts().size(), 1u);   // no new tallies

    eph_store.clear();
}

} // namespace
} // namespace GnssProcessor
