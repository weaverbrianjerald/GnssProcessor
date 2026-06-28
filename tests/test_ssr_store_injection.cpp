/**
 * @file test_ssr_store_injection.cpp
 * @brief DI-seam tests for SsrCorrectionStore injection (PPP-RTK closed-loop harness).
 *
 * The network closed loop feeds a held-out consumer the producer's emitted
 * products from an OWNED SsrCorrectionStore — never the process-wide singleton.
 * @c setSsrStore() is the dependency-injection seam: a null override keeps every
 * existing caller on the singleton (byte-identical, the default), while a non-null
 * pointer routes SSR consumption at the injected store.
 *
 * These tests pin that contract at the GnssCorrections satellite-clock consume
 * site — the one SSR consumer that is unit-constructible without a full
 * PppModel / RINEX-parser chain (MeasurementModel and the RealTimeEngine
 * forwarder share the identical override member + resolver, and are exercised
 * end-to-end by the network closed-loop driver).  No reliance on global singleton
 * state: every assertion uses owned stores so the suite stays order-independent.
 */

#include <gtest/gtest.h>

#include "corrections/GnssCorrections.hpp"
#include "products/SsrCorrectionStore.hpp"
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

// An injected OWNED store routes the SSR clock overlay through the seam: with no
// precise CLK product loaded (post-processing minimal ctor), the returned clock
// is exactly the emitted c0 (metres) converted to seconds.  Pointing the seam at
// a different (empty) owned store reverts to "no correction" — the override is a
// clean swap, and the owned stores never leak into one another or the singleton.
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

} // namespace
} // namespace GnssProcessor
