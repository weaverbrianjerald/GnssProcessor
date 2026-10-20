/**
 * @file test_rtcm_stream_dating.cpp
 * @brief RTCM decode defects that put a record on the wrong day or under the wrong name.
 *
 * Coverage:
 *   - GLONASS 1020: the reference time is a bare time of day and is dated from the
 *     parser's stream-time tracker (the observation and SSR epochs), not from the host
 *     clock, so a replay of historical data keeps the ephemeris on the day of its
 *     observations.  A 1020 that arrives before any stream epoch is held, dated by the
 *     first epoch, and a full hold is dated from the host clock.
 *   - GPS 1019: the week hint derived from the same anchor is unchanged.
 *   - SBAS satellite strings: MSM and legacy SSR name SBAS satellite 1 "S20" (PRN 120),
 *     as RINEX and SBF do, not "S120".
 *
 * The synthetic frames carry zero CRC bytes; the parser assumes the framer verified them.
 */

#include <gtest/gtest.h>

#include "common/Constants.hpp"
#include "math/TimeUtils.hpp"
#include "products/BroadcastEphemerisTypes.hpp"
#include "rtcm/RtcmParser.hpp"
#include "rtcm/RtcmSatId.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

namespace GnssProcessor {
namespace {

// MSB-first bit writer used to hand-build RTCM payloads.
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
    /// Sign-magnitude field: the sign bit, then the magnitude in n - 1 bits.
    void putSignMagnitude(int n, int64_t value) {
        const uint64_t sign = value < 0 ? 1ull : 0ull;
        const uint64_t mag  = static_cast<uint64_t>(value < 0 ? -value : value);
        put(n, (sign << (n - 1)) | mag);
    }
    std::size_t bitCount() const { return bit_count_; }
    std::vector<uint8_t> finish() {
        while (bit_count_ % 8) put(1, 0);
        return bytes_;
    }
private:
    std::vector<uint8_t> bytes_;
    std::size_t          bit_count_{0};
};

// 0xD3 | length(10) | payload | CRC(0,0,0).
std::vector<uint8_t> buildFrame(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame;
    const auto pl = static_cast<uint16_t>(payload.size());
    frame.push_back(0xD3);
    frame.push_back(static_cast<uint8_t>((pl >> 8) & 0x03u));
    frame.push_back(static_cast<uint8_t>(pl & 0xFFu));
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(0); frame.push_back(0); frame.push_back(0);
    return frame;
}

constexpr double kWeekS = 604800.0;
constexpr double kDayS  = 86400.0;

// RTCM 1020 with the PZ-90 state at fixed values; the slot and tb (15 min units) vary.
std::vector<uint8_t> glonass1020(int slot, int tb) {
    BitWriter pw;
    pw.put(12, 1020);                    // DF002
    pw.put(6,  slot);                    // DF038 slot
    pw.put(5,  10);                      // DF039 frequency channel 10 -> k = +3
    pw.put(1,  0);                       // DF040
    pw.put(1,  0);                       // DF104
    pw.put(2,  0);                       // DF105 P1
    pw.put(12, 0);                       // DF106 tk
    pw.put(1,  0);                       // DF107
    pw.put(1,  0);                       // DF108 P2
    pw.put(7,  tb);                      // DF109 tb
    for (int axis = 0; axis < 3; ++axis) {
        pw.putSignMagnitude(24, 1000 * (axis + 1));    // velocity
        pw.putSignMagnitude(27, 5000000 * (axis + 1)); // position
        pw.putSignMagnitude(5,  0);                    // acceleration
    }
    pw.put(1,  0);                       // DF119 P3
    pw.putSignMagnitude(11, 0);          // DF120 gamma
    pw.put(2,  0);                       // DF121 P
    pw.put(1,  0);                       // DF122 ln
    pw.putSignMagnitude(22, 0);          // DF123 tau
    return buildFrame(pw.finish());
}

// MSM7 for one satellite and one signal (RTCM signal id 2), zero fine fields.
std::vector<uint8_t> msm7(int msg_type, uint32_t epoch_ms, int sat_id) {
    BitWriter pw;
    pw.put(12, msg_type);
    pw.put(12, 100);                     // DF003 station
    pw.put(30, epoch_ms);                // epoch (ms of week)
    pw.put(1,  0); pw.put(3, 0); pw.put(7, 0);
    pw.put(2,  0); pw.put(2, 0); pw.put(1, 0); pw.put(3, 0);
    pw.put(64, 1ull << (63 - (sat_id - 1)));
    pw.put(32, 1u << (31 - 1));          // signal id 2
    pw.put(1,  1);                       // cell mask
    pw.put(8,  67);                      // rough range, ms
    pw.put(4,  0);                       // DF419
    pw.put(10, 0);
    pw.putSigned(14, -100);
    pw.putSigned(20, 0);
    pw.putSigned(24, 0);
    pw.put(10, 64);
    pw.put(1,  0);
    pw.put(10, 720);
    pw.putSigned(15, 0);
    return buildFrame(pw.finish());
}

// GPS MSM7 for G05 at a chosen time of week (s), used to set the stream time.
std::vector<uint8_t> gpsEpochFrame(double tow_s) {
    return msm7(1077, static_cast<uint32_t>(std::llround(tow_s * 1000.0)), 5);
}

// A time of week about `days_from_now` away from the host clock, so that the stream epoch
// the parser adopts is that far from the host's day (nearest continuation of "now").
double towAwayFromHost(double days_from_now) {
    const double now = TimeUtils::nowContinuousGpsSeconds();
    const double now_tow = std::fmod(now, kWeekS);
    return std::fmod(now_tow + days_from_now * kDayS, kWeekS);
}

double continuous(const GnssTime& t) { return TimeUtils::continuousGpsSeconds(t); }

// ---------------------------------------------------------------------------
// GLONASS 1020 reference time
// ---------------------------------------------------------------------------

TEST(RtcmGlonassDatingTest, EphemerisIsDatedOnTheStreamEpochNotTheHostClock) {
    RtcmParser parser;
    // A stream epoch 3.2 days from the host's now: the parser adopts it by nearest
    // continuation, so the tracker sits 3.2 days from the host clock.
    ASSERT_TRUE(parser.parseFrame(gpsEpochFrame(towAwayFromHost(3.2))));
    ASSERT_EQ(parser.getObservations().size(), 1u);
    const double tracker = parser.timeAnchorGpsSeconds();
    const double host    = TimeUtils::nowContinuousGpsSeconds();
    ASSERT_GT(std::fabs(tracker - host), 3.0 * kDayS);

    ASSERT_TRUE(parser.parseFrame(glonass1020(/*slot=*/7, /*tb=*/48)));   // tb 48 -> 12:00 GLONASS day
    ASSERT_EQ(parser.getBroadcastEphemeris().size(), 1u);
    const auto& eph = parser.getBroadcastEphemeris().front();
    EXPECT_EQ(eph.satellite, "R07");
    EXPECT_EQ(eph.system, System::GLONASS);
    EXPECT_EQ(eph.glonass.freq_channel, 3);

    // The reference time is the nearest match to the stream epoch, within 12 h of it,
    // and therefore a long way from the host's day.
    const double toe = continuous(eph.glonass.toe);
    EXPECT_LE(std::fabs(toe - tracker), 12.0 * 3600.0 + 1.0);
    EXPECT_GT(std::fabs(toe - host), 2.0 * kDayS);
    const GnssTime expected = TimeUtils::glonassTodToGpsTime(48 * 900.0, tracker);
    EXPECT_EQ(eph.glonass.toe.week, expected.week);
    EXPECT_NEAR(eph.glonass.toe.tow, expected.tow, 1e-6);
}

TEST(RtcmGlonassDatingTest, EphemerisBeforeAnyStreamEpochIsHeldAndDatedByTheFirstEpoch) {
    RtcmParser parser;
    // Nothing time-bearing yet: the record is accepted but not released.
    ASSERT_TRUE(parser.parseFrame(glonass1020(3, 20)));
    ASSERT_TRUE(parser.parseFrame(glonass1020(4, 21)));
    EXPECT_TRUE(parser.getBroadcastEphemeris().empty());

    // The first stream epoch releases both, dated on that epoch.
    parser.clear();
    ASSERT_TRUE(parser.parseFrame(gpsEpochFrame(towAwayFromHost(3.2))));
    const double tracker = parser.timeAnchorGpsSeconds();
    ASSERT_EQ(parser.getBroadcastEphemeris().size(), 2u);
    EXPECT_EQ(parser.getBroadcastEphemeris()[0].satellite, "R03");
    EXPECT_EQ(parser.getBroadcastEphemeris()[1].satellite, "R04");
    for (const auto& eph : parser.getBroadcastEphemeris())
        EXPECT_LE(std::fabs(continuous(eph.glonass.toe) - tracker), 12.0 * 3600.0 + 1.0);

    // Once the stream time is known a 1020 is decoded at once.
    parser.clear();
    ASSERT_TRUE(parser.parseFrame(glonass1020(5, 22)));
    ASSERT_EQ(parser.getBroadcastEphemeris().size(), 1u);
    EXPECT_EQ(parser.getBroadcastEphemeris()[0].satellite, "R05");
}

TEST(RtcmGlonassDatingTest, HeldRecordsSurviveClearUntilAStreamEpochArrives) {
    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(glonass1020(9, 30)));
    parser.clear();   // the engine clears before every frame
    EXPECT_TRUE(parser.getBroadcastEphemeris().empty());
    ASSERT_TRUE(parser.parseFrame(gpsEpochFrame(towAwayFromHost(1.0))));
    ASSERT_EQ(parser.getBroadcastEphemeris().size(), 1u);
    EXPECT_EQ(parser.getBroadcastEphemeris()[0].satellite, "R09");
}

TEST(RtcmGlonassDatingTest, FullHoldIsDatedFromTheHostClock) {
    RtcmParser parser;
    const std::size_t cap = RtcmParser::kMaxDeferredGlonass;
    // A navigation-only input never sets the tracker: the hold fills ...
    for (std::size_t i = 0; i < cap; ++i)
        ASSERT_TRUE(parser.parseFrame(glonass1020(static_cast<int>(i % 24) + 1, 40)));
    EXPECT_TRUE(parser.getBroadcastEphemeris().empty());

    // ... and the next record releases it, dated from the host clock.
    ASSERT_TRUE(parser.parseFrame(glonass1020(1, 40)));
    ASSERT_EQ(parser.getBroadcastEphemeris().size(), cap);
    const double host = TimeUtils::nowContinuousGpsSeconds();
    for (const auto& eph : parser.getBroadcastEphemeris())
        EXPECT_LE(std::fabs(continuous(eph.glonass.toe) - host), 12.0 * 3600.0 + 5.0);
}

TEST(RtcmGlonassDatingTest, TruncatedRecordIsRefusedAndNotHeld) {
    RtcmParser parser;
    auto frame = glonass1020(2, 10);
    // Cut the payload to 20 bytes: well short of the fields the decoder reads.
    std::vector<uint8_t> payload(frame.begin() + 3, frame.begin() + 3 + 20);
    EXPECT_FALSE(parser.parseFrame(buildFrame(payload)));
    ASSERT_TRUE(parser.parseFrame(gpsEpochFrame(towAwayFromHost(1.0))));
    EXPECT_TRUE(parser.getBroadcastEphemeris().empty());
}

// ---------------------------------------------------------------------------
// GPS 1019 week hint, derived from the same anchor
// ---------------------------------------------------------------------------

TEST(RtcmGlonassDatingTest, GpsWeekHintStillComesFromTheTracker) {
    BitWriter pw;
    pw.put(12, 1019); pw.put(6, 5);
    pw.put(10, 200);                     // DF076 week, truncated to 10 bits
    pw.put(4, 0); pw.put(2, 0); pw.putSigned(14, 0); pw.put(8, 1);
    pw.put(16, 0); pw.putSigned(8, 0); pw.putSigned(16, 0); pw.putSigned(22, 0);
    pw.put(10, 0); pw.putSigned(16, 0); pw.putSigned(16, 0); pw.putSigned(32, 0);
    pw.putSigned(16, 0); pw.put(32, 0); pw.putSigned(16, 0); pw.put(32, 0);
    pw.put(16, 450); pw.putSigned(16, 0); pw.putSigned(32, 0); pw.putSigned(16, 0);
    pw.putSigned(32, 0); pw.putSigned(16, 0); pw.putSigned(32, 0); pw.putSigned(24, 0);
    pw.putSigned(8, 0); pw.put(6, 0); pw.put(1, 0); pw.put(1, 0);

    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(gpsEpochFrame(towAwayFromHost(0.5))));
    const int anchor_week = static_cast<int>(parser.timeAnchorGpsSeconds() / kWeekS);
    ASSERT_TRUE(parser.parseFrame(buildFrame(pw.finish())));
    ASSERT_EQ(parser.getBroadcastEphemeris().size(), 1u);
    const int week = parser.getBroadcastEphemeris().front().kepler.toe.week;
    EXPECT_EQ((week - 200) % 1024, 0);                 // the broadcast week modulo 1024
    EXPECT_LE(std::abs(week - anchor_week), 512);      // resolved to the anchor's rollover
}

// ---------------------------------------------------------------------------
// SBAS satellite strings
// ---------------------------------------------------------------------------

TEST(RtcmSbasSatIdTest, RtcmIdentifierMapsToRinexNumber) {
    EXPECT_EQ(RtcmSatId::sbas(1),  "S20");   // PRN 120
    EXPECT_EQ(RtcmSatId::sbas(16), "S35");   // PRN 135
    EXPECT_EQ(RtcmSatId::sbas(39), "S58");   // PRN 158
}

TEST(RtcmSbasSatIdTest, Msm7NamesSbasSatelliteOneS20) {
    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(msm7(1107, 100000, /*sat_id=*/1)));
    ASSERT_EQ(parser.getObservations().size(), 1u);
    EXPECT_EQ(parser.getObservations().front().satellite, "S20");
    EXPECT_EQ(parser.getObservations().front().system, System::SBAS);

    parser.clear();
    ASSERT_TRUE(parser.parseFrame(msm7(1107, 101000, /*sat_id=*/39)));
    ASSERT_EQ(parser.getObservations().size(), 1u);
    EXPECT_EQ(parser.getObservations().front().satellite, "S58");
}

TEST(RtcmSbasSatIdTest, OtherSystemsKeepTheirNumbering) {
    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(msm7(1077, 100000, /*sat_id=*/5)));
    ASSERT_EQ(parser.getObservations().size(), 1u);
    EXPECT_EQ(parser.getObservations().front().satellite, "G05");
}

TEST(RtcmSbasSatIdTest, LegacySsrNamesSbasSatelliteOneS20) {
    BitWriter pw;
    pw.put(12, 1253);                    // SBAS clock
    pw.put(20, 100);                     // epoch
    pw.put(4, 0); pw.put(1, 0);
    pw.put(4, 5);
    pw.put(16, 0); pw.put(4, 0);
    pw.put(6, 1);                        // one satellite
    pw.put(6, 1);                        // SBAS satellite id 1
    pw.putSigned(22, 100); pw.putSigned(21, 0); pw.putSigned(27, 0);
    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(buildFrame(pw.finish())));
    ASSERT_EQ(parser.getSsrClocks().size(), 1u);
    EXPECT_EQ(parser.getSsrClocks().front().sat, "S20");
}

} // namespace
} // namespace GnssProcessor
