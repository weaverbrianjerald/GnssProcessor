/**
 * @file test_ssr_reference_time.cpp
 * @brief SSR timing: reference time, high-rate clock pairing and receipt stamps.
 *
 * Coverage:
 *   - ssrUpdateIntervalSeconds: the IDF004 / DF391 indicator table.
 *   - ssrReferenceTime: the SSR epoch plus half the update interval; indicator 0
 *     (1 s) and an undeclared interval (0, the network product records) keep the
 *     epoch; the week rollover.
 *   - SsrCorrectionStore::getOrbitClock: the clock and orbit dt bases are the
 *     reference times of their own messages; a high-rate clock is added only to
 *     the clock message it refines (same IOD-SSR, epoch inside the clock's update
 *     interval); a high-rate clock alone is neither applied nor counted as clock
 *     coverage.
 *   - Decoding (hand-built RTCM 1058 / 1062 / 4076 frames): records carry the SSR
 *     epoch and the update interval as sent, the high-rate record its epoch, the
 *     decode result names the message kind, and the receipt stamp is on the GPS
 *     time scale (host clock plus the leap seconds).
 *
 * Reference: IGS SSR Format v1.00 (2020-10-05), sections 4 and 4.2 and Table 7
 * (IDF004).  The synthetic frames carry zero CRC bytes, which the parser does not
 * inspect (RtcmStreamReader verifies the CRC before frames reach it).
 */

#include <gtest/gtest.h>

#include "rtcm/RtcmParser.hpp"
#include "rtcm/RtcmSsrDecoder.hpp"
#include "rtcm/RtcmTypes.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "math/TimeUtils.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

// ============================================================================
// Local helpers: MSB-first bit writer and RTCM3 framing (as in test_rtcm.cpp).
// ============================================================================
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
    const std::vector<uint8_t>& bytes() const { return bytes_; }
    std::size_t bitCount() const { return bit_count_; }
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

// Legacy SSR header of a GPS message without the datum flag (clock, high-rate
// clock): DF385 epoch, DF391 update interval, DF388, DF413, DF414, DF415, DF387.
void putGpsSsrHeader(BitWriter& pw, int epoch_tow_s, int interval_index, int iod_ssr) {
    pw.put(20, static_cast<uint64_t>(epoch_tow_s));   // DF385 GPS epoch time
    pw.put(4,  static_cast<uint64_t>(interval_index));// DF391 update interval
    pw.put(1,  0);                                    // DF388 multi-message
    pw.put(4,  static_cast<uint64_t>(iod_ssr));       // DF413 IOD-SSR
    pw.put(16, 0);                                    // DF414 provider id
    pw.put(4,  0);                                    // DF415 solution id
    pw.put(6,  1);                                    // DF387 one satellite
}

// RTCM 1058 (GPS clock), one satellite G05 with C0 = 0.01 m.
std::vector<uint8_t> gpsClock1058(int epoch_tow_s, int interval_index, int iod_ssr) {
    BitWriter pw;
    pw.put(12, 1058);
    putGpsSsrHeader(pw, epoch_tow_s, interval_index, iod_ssr);
    pw.put(6, 5);                 // DF068 G05
    pw.putSigned(22, 100);        // DF376 C0 = 0.01 m
    pw.putSigned(21, 0);          // DF377 C1
    pw.putSigned(27, 0);          // DF378 C2
    while (pw.bitCount() % 8) pw.put(1, 0);
    return pw.bytes();
}

// RTCM 1062 (GPS high-rate clock), one satellite G05 with a 0.025 m delta.
std::vector<uint8_t> gpsHighRate1062(int epoch_tow_s, int interval_index, int iod_ssr) {
    BitWriter pw;
    pw.put(12, 1062);
    putGpsSsrHeader(pw, epoch_tow_s, interval_index, iod_ssr);
    pw.put(6, 5);                 // DF068 G05
    pw.putSigned(22, 250);        // DF390 high-rate clock = 0.025 m
    while (pw.bitCount() % 8) pw.put(1, 0);
    return pw.bytes();
}

constexpr int kWeek = 2300;
GnssTime at(double tow) { return GnssTime{kWeek, tow}; }

double tow(const GnssTime& t) { return t.tow; }

SsrClockCorrection clockAt(double epoch_tow, double interval_s, int iod_ssr) {
    SsrClockCorrection c;
    c.sat               = "G05";
    c.c0                = 0.10;
    c.received_time     = at(epoch_tow + 2.0);
    c.epoch_time        = at(epoch_tow);
    c.update_interval_s = interval_s;
    c.iod_ssr           = iod_ssr;
    return c;
}

SsrHighRateClock highRateAt(double epoch_tow, int iod_ssr, double dc_m) {
    SsrHighRateClock h;
    h.sat               = "G05";
    h.dc                = dc_m;
    h.received_time     = at(epoch_tow + 1.0);
    h.epoch_time        = at(epoch_tow);
    h.update_interval_s = 1.0;
    h.iod_ssr           = iod_ssr;
    return h;
}

// Query a few seconds after the records, well inside the freshness window.
SsrCorrectionStore::OrbitClockCorrection query(const SsrCorrectionStore& store, double t) {
    const auto q = store.getOrbitClock("G05", at(t), 60.0);
    EXPECT_TRUE(q.has_value());
    return q.value_or(SsrCorrectionStore::OrbitClockCorrection{});
}

// ============================================================================
// Update-interval table and reference time
// ============================================================================
TEST(SsrReferenceTimeTest, IndicatorTable) {
    EXPECT_DOUBLE_EQ(ssrUpdateIntervalSeconds(0), 1.0);
    EXPECT_DOUBLE_EQ(ssrUpdateIntervalSeconds(1), 2.0);
    EXPECT_DOUBLE_EQ(ssrUpdateIntervalSeconds(2), 5.0);
    EXPECT_DOUBLE_EQ(ssrUpdateIntervalSeconds(6), 60.0);
    EXPECT_DOUBLE_EQ(ssrUpdateIntervalSeconds(11), 900.0);
    EXPECT_DOUBLE_EQ(ssrUpdateIntervalSeconds(15), 10800.0);
    EXPECT_DOUBLE_EQ(ssrUpdateIntervalSeconds(-1), 0.0);
    EXPECT_DOUBLE_EQ(ssrUpdateIntervalSeconds(16), 0.0);
}

TEST(SsrReferenceTimeTest, HalfTheIntervalAfterTheEpoch) {
    EXPECT_DOUBLE_EQ(tow(ssrReferenceTime(at(100.0), 5.0)), 102.5);
    EXPECT_DOUBLE_EQ(tow(ssrReferenceTime(at(100.0), 60.0)), 130.0);
    EXPECT_EQ(ssrReferenceTime(at(100.0), 60.0).week, kWeek);
}

TEST(SsrReferenceTimeTest, OneSecondAndUndeclaredKeepTheEpoch) {
    // Indicator 0 (1 s) is the exception in the standard; 0 is what the network
    // product records carry, so their reference time stays the epoch.
    EXPECT_EQ(ssrReferenceTime(at(100.0), 1.0), at(100.0));
    EXPECT_EQ(ssrReferenceTime(at(100.0), 0.0), at(100.0));
}

TEST(SsrReferenceTimeTest, RollsIntoTheNextWeek) {
    const GnssTime t0 = ssrReferenceTime(GnssTime{kWeek, 604790.0}, 60.0);
    EXPECT_EQ(t0.week, kWeek + 1);
    EXPECT_DOUBLE_EQ(t0.tow, 20.0);
}

TEST(SsrReferenceTimeTest, SeparateMessagesUseTheirOwnReferenceTimes) {
    SsrCorrectionStore store;
    SsrOrbitCorrection orb;
    orb.sat               = "G05";
    orb.received_time     = at(102.0);
    orb.epoch_time        = at(100.0);
    orb.update_interval_s = 60.0;
    store.pushOrbit(orb, SsrHeader{});
    store.pushClock(clockAt(100.0, 5.0, 3), SsrHeader{});

    const auto q = query(store, 103.0);
    EXPECT_DOUBLE_EQ(tow(q.orbit_epoch_time), 130.0);   // orbit: 100 + 60 / 2
    EXPECT_DOUBLE_EQ(tow(q.epoch_time), 102.5);         // clock: 100 + 5 / 2
}

TEST(SsrReferenceTimeTest, CombinedSharesOneReferenceTime) {
    SsrCorrectionStore store;
    SsrCombinedCorrection cb;
    cb.sat               = "G05";
    cb.received_time     = at(102.0);
    cb.epoch_time        = at(100.0);
    cb.update_interval_s = 10.0;
    store.pushCombined(cb, SsrHeader{});

    const auto q = query(store, 103.0);
    EXPECT_DOUBLE_EQ(tow(q.epoch_time), 105.0);
    EXPECT_DOUBLE_EQ(tow(q.orbit_epoch_time), 105.0);
    EXPECT_DOUBLE_EQ(tow(q.orbit_epoch), 105.0);   // the echo agrees by construction
}

TEST(SsrReferenceTimeTest, UndeclaredIntervalKeepsTheEpoch) {
    SsrCorrectionStore store;
    store.pushClock(clockAt(100.0, 0.0, 3), SsrHeader{});
    EXPECT_EQ(query(store, 103.0).epoch_time, at(100.0));
}

// ============================================================================
// High-rate clock pairing
// ============================================================================
TEST(SsrHighRateClockPairingTest, AddedInsideTheClockInterval) {
    SsrCorrectionStore store;
    store.pushClock(clockAt(100.0, 5.0, 3), SsrHeader{});

    store.pushHighRateClock(highRateAt(100.0, 3, 0.02), SsrHeader{});
    EXPECT_DOUBLE_EQ(query(store, 101.0).hr_clock, 0.02);   // at the clock epoch

    store.pushHighRateClock(highRateAt(104.0, 3, 0.03), SsrHeader{});
    EXPECT_DOUBLE_EQ(query(store, 105.0).hr_clock, 0.03);   // last second of the interval
}

TEST(SsrHighRateClockPairingTest, DroppedOnceANewerClockArrives) {
    SsrCorrectionStore store;
    store.pushClock(clockAt(100.0, 5.0, 3), SsrHeader{});
    store.pushHighRateClock(highRateAt(104.0, 3, 0.03), SsrHeader{});
    ASSERT_DOUBLE_EQ(query(store, 105.0).hr_clock, 0.03);

    // The 105 clock replaces the polynomial the 104 delta refined.
    store.pushClock(clockAt(105.0, 5.0, 3), SsrHeader{});
    EXPECT_DOUBLE_EQ(query(store, 106.0).hr_clock, 0.0);
}

TEST(SsrHighRateClockPairingTest, DroppedWhenAheadOfTheClock) {
    // A delta for the 105 interval refines a clock message not yet received.
    SsrCorrectionStore store;
    store.pushClock(clockAt(100.0, 5.0, 3), SsrHeader{});
    store.pushHighRateClock(highRateAt(105.0, 3, 0.03), SsrHeader{});
    EXPECT_DOUBLE_EQ(query(store, 106.0).hr_clock, 0.0);
}

TEST(SsrHighRateClockPairingTest, DroppedOnAnIodSsrChange) {
    SsrCorrectionStore store;
    store.pushClock(clockAt(100.0, 5.0, 3), SsrHeader{});
    store.pushHighRateClock(highRateAt(101.0, 4, 0.03), SsrHeader{});
    EXPECT_DOUBLE_EQ(query(store, 102.0).hr_clock, 0.0);
}

TEST(SsrHighRateClockPairingTest, PairsWithACombinedRecord) {
    SsrCorrectionStore store;
    SsrCombinedCorrection cb;
    cb.sat               = "G05";
    cb.received_time     = at(101.0);
    cb.epoch_time        = at(100.0);
    cb.update_interval_s = 5.0;
    cb.iod_ssr           = 2;
    store.pushCombined(cb, SsrHeader{});
    store.pushHighRateClock(highRateAt(102.0, 2, 0.04), SsrHeader{});
    EXPECT_DOUBLE_EQ(query(store, 103.0).hr_clock, 0.04);
}

TEST(SsrHighRateClockPairingTest, AloneIsNeitherAppliedNorCoverage) {
    SsrCorrectionStore store;
    SsrOrbitCorrection orb;
    orb.sat           = "G05";
    orb.received_time = at(101.0);
    orb.epoch_time    = at(100.0);
    store.pushOrbit(orb, SsrHeader{});
    store.pushHighRateClock(highRateAt(101.0, 0, 0.03), SsrHeader{});

    const auto q = query(store, 102.0);
    EXPECT_TRUE(q.has_orbit);
    EXPECT_FALSE(q.has_clock);
    EXPECT_DOUBLE_EQ(q.hr_clock, 0.0);

    const auto cov = store.coverageFor({"G05"}, at(102.0), 60.0);
    EXPECT_EQ(cov.orbit, 1);
    EXPECT_EQ(cov.clock, 0);
}

// ============================================================================
// Decoding: epochs and intervals as sent, message kind, receipt stamp
// ============================================================================
TEST(SsrTimingDecodeTest, ClockRecordKeepsTheEpochAndInterval) {
    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(buildFrame(gpsClock1058(100, /*5 s*/ 2, 5))));
    ASSERT_EQ(parser.getSsrClocks().size(), 1u);
    const auto& c = parser.getSsrClocks().front();
    EXPECT_DOUBLE_EQ(c.epoch_time.tow, 100.0);
    EXPECT_DOUBLE_EQ(c.update_interval_s, 5.0);
    EXPECT_EQ(parser.getLastSsrHeader().update_interval_index, 2);
}

TEST(SsrTimingDecodeTest, HighRateRecordCarriesItsEpoch) {
    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(buildFrame(gpsHighRate1062(103, /*1 s*/ 0, 6))));
    ASSERT_EQ(parser.getSsrHighRateClocks().size(), 1u);
    const auto& h = parser.getSsrHighRateClocks().front();
    EXPECT_EQ(h.sat, "G05");
    EXPECT_NEAR(h.dc, 0.025, 1e-9);
    EXPECT_DOUBLE_EQ(h.epoch_time.tow, 103.0);
    EXPECT_DOUBLE_EQ(h.update_interval_s, 1.0);
    EXPECT_EQ(h.iod_ssr, 6);
}

TEST(SsrTimingDecodeTest, ResultNamesTheMessageKind) {
    std::vector<SsrOrbitCorrection>    orbits;
    std::vector<SsrClockCorrection>    clocks;
    std::vector<SsrCombinedCorrection> combined;
    std::vector<SsrCodeBias>           code_biases;
    std::vector<SsrPhaseBias>          phase_biases;
    std::vector<SsrUra>                uras;
    std::vector<SsrHighRateClock>      hrclocks;

    const auto legacy = gpsClock1058(100, 2, 5);
    const auto r1 = RtcmSsr::decodeLegacy(legacy.data(), legacy.size(), 1058, 0.0,
                                          orbits, clocks, combined, code_biases,
                                          phase_biases, uras, hrclocks);
    ASSERT_TRUE(r1.ok);
    EXPECT_EQ(std::string(r1.body), "clock");
    EXPECT_EQ(r1.igs_message_number, 0);

    // IGS-SSR 4076, IM 22 (GPS clock): DF002, version, IM, then the shared header.
    BitWriter pw;
    pw.put(12, 4076);
    pw.put(3,  1);                 // IGS-SSR version
    pw.put(8,  22);                // IM: GPS clock
    putGpsSsrHeader(pw, 100, 2, 5);
    pw.put(6, 5);                  // G05
    pw.putSigned(22, 100);         // C0
    pw.putSigned(21, 0);           // C1
    pw.putSigned(27, 0);           // C2
    while (pw.bitCount() % 8) pw.put(1, 0);
    const auto igs = pw.bytes();
    const auto r2 = RtcmSsr::decodeIgs4076(igs.data(), igs.size(), 0.0,
                                           orbits, clocks, combined, code_biases,
                                           phase_biases, uras, hrclocks);
    ASSERT_TRUE(r2.ok);
    EXPECT_EQ(std::string(r2.body), "clock");
    EXPECT_EQ(r2.igs_message_number, 22);
}

TEST(SsrTimingDecodeTest, ReceiptStampIsOnTheGpsTimeScale) {
    // The freshness gate compares the stamp with GPS-time observation epochs.  A
    // UTC-scale stamp would sit the leap seconds (18 s) behind the GPS clock.
    RtcmParser parser;
    const double before = TimeUtils::nowContinuousGpsSeconds();
    ASSERT_TRUE(parser.parseFrame(buildFrame(gpsClock1058(100, 2, 5))));
    const double after = TimeUtils::nowContinuousGpsSeconds();
    ASSERT_EQ(parser.getSsrClocks().size(), 1u);
    const double stamp = TimeUtils::continuousGpsSeconds(parser.getSsrClocks().front().received_time);
    EXPECT_GE(stamp, before - 1.0);
    EXPECT_LE(stamp, after + 1.0);
}

} // namespace
} // namespace GnssProcessor
