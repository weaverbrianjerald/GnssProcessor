/**
 * @file test_ssr_clock_orbit_pairing.cpp
 * @brief An RTCM / IGS-SSR clock applies only with the orbit correction it belongs to.
 *
 * Coverage:
 *   - The decoder marks every clock record as computed against the stream's own
 *     orbit correction (SsrClockCorrection::needs_orbit_correction).
 *   - SsrCorrectionStore::getOrbitClock withholds such a clock until a fresh orbit
 *     record of the same IOD-SSR is merged with it: no orbit at all (start-up), a
 *     stale orbit after a correction outage, an orbit of another IOD-SSR.  The
 *     high-rate clock of a withheld clock is not added either.
 *   - A clock that declares no such dependence (the network producer's records)
 *     still applies on its own, and a combined record is unaffected.
 *
 * The defect: on 2026-11-03 the CAS stream (orbit and clock in separate messages)
 * came back from a 99 s outage with the clocks of 17 satellites and the orbits of 8;
 * for 6 epochs the store returned the fresh clocks with no orbit part and the engine
 * modelled those satellites with corrected clocks on uncorrected broadcast orbits,
 * counted as fully corrected (card SSR-CLK-ORB).
 *
 * Reference: IGS SSR Format v1.00 (2020-10-05), section 4.  The synthetic frame
 * carries zero CRC bytes, which the parser does not inspect.
 */

#include <gtest/gtest.h>

#include "rtcm/RtcmParser.hpp"
#include "rtcm/RtcmTypes.hpp"
#include "products/SsrCorrectionStore.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

// MSB-first bit writer and RTCM3 framing (as in test_rtcm.cpp).
class BitWriter {
public:
    void put(int n, uint64_t value) {
        for (int i = n - 1; i >= 0; --i) {
            const bool bit = ((value >> i) & 0x1ull) != 0;
            if ((bit_count_ & 7u) == 0) bytes_.push_back(0u);
            if (bit) bytes_.back() |= static_cast<uint8_t>(1u << (7 - (bit_count_ & 7u)));
            ++bit_count_;
        }
    }
    void putSigned(int n, int64_t value) {
        const uint64_t mask = (n == 64) ? ~0ull : ((1ull << n) - 1ull);
        put(n, static_cast<uint64_t>(value) & mask);
    }
    std::vector<uint8_t> finish() {
        while (bit_count_ % 8) put(1, 0);
        return bytes_;
    }
private:
    std::vector<uint8_t> bytes_;
    std::size_t          bit_count_{0};
};

std::vector<uint8_t> buildFrame(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame;
    const auto pl = static_cast<uint16_t>(payload.size());
    frame.push_back(0xD3);
    frame.push_back(static_cast<uint8_t>((pl >> 8) & 0x03u));
    frame.push_back(static_cast<uint8_t>(pl & 0xFFu));
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(0); frame.push_back(0); frame.push_back(0);   // CRC, unchecked
    return frame;
}

// RTCM 1058 (GPS clock), one satellite G05 with C0 = 0.01 m, 5 s interval.
std::vector<uint8_t> gpsClock1058(int epoch_tow_s, int iod_ssr) {
    BitWriter pw;
    pw.put(12, 1058);
    pw.put(20, static_cast<uint64_t>(epoch_tow_s));   // DF385 GPS epoch time
    pw.put(4,  2);                                    // DF391 update interval, 5 s
    pw.put(1,  0);                                    // DF388 multi-message
    pw.put(4,  static_cast<uint64_t>(iod_ssr));       // DF413 IOD-SSR
    pw.put(16, 0);                                    // DF414 provider id
    pw.put(4,  0);                                    // DF415 solution id
    pw.put(6,  1);                                    // DF387 one satellite
    pw.put(6,  5);                                    // DF068 G05
    pw.putSigned(22, 100);                            // DF376 C0 = 0.01 m
    pw.putSigned(21, 0);                              // DF377 C1
    pw.putSigned(27, 0);                              // DF378 C2
    return pw.finish();
}

constexpr int kWeek = 2300;
GnssTime at(double tow) { return GnssTime{kWeek, tow}; }

// A clock as the decoder builds it: computed against the stream's orbit correction.
SsrClockCorrection streamClock(double epoch_tow, int iod_ssr) {
    SsrClockCorrection c;
    c.sat                    = "G05";
    c.c0                     = 0.10;
    c.received_time          = at(epoch_tow + 1.0);
    c.epoch_time             = at(epoch_tow);
    c.update_interval_s      = 5.0;
    c.iod_ssr                = iod_ssr;
    c.needs_orbit_correction = true;
    return c;
}

SsrOrbitCorrection streamOrbit(double epoch_tow, int iod_ssr) {
    SsrOrbitCorrection o;
    o.sat               = "G05";
    o.iode              = 7;
    o.delta_rac         = Eigen::Vector3d{0.30, -0.20, 0.05};
    o.received_time     = at(epoch_tow + 1.0);
    o.epoch_time        = at(epoch_tow);
    o.update_interval_s = 5.0;
    o.iod_ssr           = iod_ssr;
    return o;
}

constexpr double kMaxAge = 60.0;

TEST(SsrClockOrbitPairingTest, DecodedClockNeedsItsOrbitCorrection) {
    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(buildFrame(gpsClock1058(100, 3))));
    ASSERT_EQ(parser.getSsrClocks().size(), 1u);
    EXPECT_TRUE(parser.getSsrClocks().front().needs_orbit_correction);
}

TEST(SsrClockOrbitPairingTest, ClockWithoutAnyOrbitIsWithheld) {
    // Start-up: the clock message of an epoch arrives before its orbit message.
    SsrCorrectionStore store;
    store.pushClock(streamClock(100.0, 0), SsrHeader{});
    EXPECT_FALSE(store.getOrbitClock("G05", at(102.0), kMaxAge).has_value());

    store.pushOrbit(streamOrbit(100.0, 0), SsrHeader{});
    const auto q = store.getOrbitClock("G05", at(102.0), kMaxAge);
    ASSERT_TRUE(q.has_value());
    EXPECT_TRUE(q->has_orbit);
    EXPECT_TRUE(q->has_clock);
    EXPECT_DOUBLE_EQ(q->c0, 0.10);
}

TEST(SsrClockOrbitPairingTest, ClockAfterAnOutageWaitsForAFreshOrbit) {
    // Orbit and clock until TOW 100, then a 99 s outage.  The clock comes back
    // first: the orbit in store is 100 s old and stale, so the clock is withheld
    // and nothing is applied.  The next orbit restores both.
    SsrCorrectionStore store;
    store.pushOrbit(streamOrbit(100.0, 0), SsrHeader{});
    store.pushClock(streamClock(100.0, 0), SsrHeader{});
    ASSERT_TRUE(store.getOrbitClock("G05", at(102.0), kMaxAge)->has_clock);

    store.pushClock(streamClock(200.0, 0), SsrHeader{});
    EXPECT_FALSE(store.getOrbitClock("G05", at(202.0), kMaxAge).has_value());

    store.pushOrbit(streamOrbit(200.0, 0), SsrHeader{});
    const auto q = store.getOrbitClock("G05", at(202.0), kMaxAge);
    ASSERT_TRUE(q.has_value());
    EXPECT_TRUE(q->has_orbit);
    EXPECT_TRUE(q->has_clock);
}

TEST(SsrClockOrbitPairingTest, ClockOfAnotherIodSsrIsWithheld) {
    // A change of IOD-SSR (a new generating configuration): the orbit of the new
    // solution is applied, the clock of the old one is not paired with it.
    SsrCorrectionStore store;
    store.pushOrbit(streamOrbit(100.0, 2), SsrHeader{});
    store.pushClock(streamClock(100.0, 1), SsrHeader{});
    const auto q = store.getOrbitClock("G05", at(102.0), kMaxAge);
    ASSERT_TRUE(q.has_value());
    EXPECT_TRUE(q->has_orbit);
    EXPECT_FALSE(q->has_clock);

    store.pushClock(streamClock(100.0, 2), SsrHeader{});
    EXPECT_TRUE(store.getOrbitClock("G05", at(102.0), kMaxAge)->has_clock);
}

TEST(SsrClockOrbitPairingTest, HighRateClockOfAWithheldClockIsNotAdded) {
    SsrCorrectionStore store;
    store.pushOrbit(streamOrbit(100.0, 2), SsrHeader{});
    store.pushClock(streamClock(100.0, 1), SsrHeader{});
    SsrHighRateClock h;
    h.sat           = "G05";
    h.dc            = 0.025;
    h.received_time = at(101.0);
    h.epoch_time    = at(101.0);
    h.update_interval_s = 1.0;
    h.iod_ssr       = 1;
    store.pushHighRateClock(h, SsrHeader{});
    const auto q = store.getOrbitClock("G05", at(102.0), kMaxAge);
    ASSERT_TRUE(q.has_value());
    EXPECT_FALSE(q->has_clock);
    EXPECT_DOUBLE_EQ(q->hr_clock, 0.0);
}

TEST(SsrClockOrbitPairingTest, NetworkClockStillAppliesOnItsOwn) {
    // The network producer's clock is defined against the orbit the user holds and
    // carries no orbit correction; its records leave needs_orbit_correction false.
    SsrCorrectionStore store;
    SsrClockCorrection c = streamClock(100.0, 0);
    c.needs_orbit_correction = false;
    store.pushClock(c, SsrHeader{});
    const auto q = store.getOrbitClock("G05", at(102.0), kMaxAge);
    ASSERT_TRUE(q.has_value());
    EXPECT_FALSE(q->has_orbit);
    EXPECT_TRUE(q->has_clock);
    EXPECT_DOUBLE_EQ(q->c0, 0.10);
}

TEST(SsrClockOrbitPairingTest, CombinedRecordIsUnaffected) {
    // A combined message carries its own orbit; a stale separate orbit beside it
    // does not matter.
    SsrCorrectionStore store;
    store.pushOrbit(streamOrbit(100.0, 0), SsrHeader{});
    SsrCombinedCorrection cb;
    cb.sat               = "G05";
    cb.iode              = 7;
    cb.c0                = 0.20;
    cb.received_time     = at(201.0);
    cb.epoch_time        = at(200.0);
    cb.update_interval_s = 5.0;
    store.pushCombined(cb, SsrHeader{});
    const auto q = store.getOrbitClock("G05", at(202.0), kMaxAge);
    ASSERT_TRUE(q.has_value());
    EXPECT_TRUE(q->has_orbit);
    EXPECT_TRUE(q->has_clock);
    EXPECT_DOUBLE_EQ(q->c0, 0.20);
}

} // namespace
} // namespace GnssProcessor
