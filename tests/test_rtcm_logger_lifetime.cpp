/**
 * @file test_rtcm_logger_lifetime.cpp
 * @brief Regression guard: the RTCM parse path must survive an empty spdlog
 *        registry.
 *
 * The bare `spdlog::warn()` / `spdlog::info()` / `spdlog::trace()` free
 * functions expand to `spdlog::default_logger_raw()->warn(...)`, i.e. they
 * dereference a RAW BORROWED pointer into the registry's `default_logger_`
 * slot.  `spdlog::drop_all()` empties that slot, so the next such call
 * dereferences nullptr and takes an access violation (0xC0000005) rather than
 * dropping the message.
 *
 * `LogManager` calls `drop_all()` from `shutdown()` (LogManager.cpp:201) and
 * from the re-initialize branch of `initialize()` (:115).  Neither is reachable
 * in a production run today — `shutdown()` has no callers, and no path
 * initializes one `LogManager` instance twice — so in the shipped binaries this
 * is latent.  It is live in THIS binary, where one test dropping the registry
 * poisons every later test in the process, which is exactly how it was found.
 *
 * Historically this made four RtcmParserDecodeTest cases latent crashes: any
 * earlier test in the same process that dropped the registry turned
 * `IgsSsr4076UnsupportedImReturnsFalse`, `Rtcm1033ReceiverAntennaDescriptor`
 * and both `Rtcm1230GlonassBiases*` cases into SEH faults — those are exactly
 * the RtcmParserDecodeTest cases whose decode path reaches a log call.  The
 * parse path now resolves its logger per call through logging/SafeLogger.hpp,
 * which returns a shared_ptr and never returns null.
 *
 * Each test here drops the registry only for the duration of the case and
 * restores it afterwards, so the blast radius does not leak into the rest of
 * the binary.
 */

#include <gtest/gtest.h>

#include "rtcm/RtcmParser.hpp"
#include "logging/SafeLogger.hpp"
#include "SpdlogRegistryGuard.hpp"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <sstream>
#include <memory>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

using TestSupport::DroppedRegistry;
using TestSupport::SinkCapture;

// ============================================================================
// Local helpers — MSB-first bit writer + frame wrapper.
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
    std::size_t bitCount() const { return bit_count_; }
    const std::vector<uint8_t>& bytes() const { return bytes_; }

private:
    std::vector<uint8_t> bytes_;
    std::size_t          bit_count_{0};
};

/// Wrap a payload in the 0xD3 preamble + 10-bit length header and three zero
/// CRC bytes (CRC is verified upstream by RtcmStreamReader, not by the parser).
std::vector<uint8_t> buildFrame(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame;
    frame.push_back(0xD3);
    frame.push_back(static_cast<uint8_t>((payload.size() >> 8) & 0x03));
    frame.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.insert(frame.end(), {0x00, 0x00, 0x00});
    return frame;
}

std::vector<uint8_t> build1033Frame() {
    auto putString = [](BitWriter& pw, const std::string& s) {
        pw.put(8, s.size());
        for (char c : s) pw.put(8, static_cast<uint8_t>(c));
    };
    BitWriter pw;
    pw.put(12, 1033);
    pw.put(12, 1234);
    putString(pw, "TRM57971.00     NONE");
    pw.put(8, 3);
    putString(pw, "12345");
    putString(pw, "SEPT ASTERX-U");
    putString(pw, "4.10.0");
    putString(pw, "3009901");
    while (pw.bitCount() % 8) pw.put(1, 0);
    return buildFrame(pw.bytes());
}

std::vector<uint8_t> build1230Frame() {
    BitWriter pw;
    pw.put(12, 1230);
    pw.put(12, 77);
    pw.put(1, 1);
    pw.put(3, 0);
    pw.put(4, 0xF);
    pw.putSigned(16, 50);
    pw.putSigned(16, -25);
    pw.putSigned(16, 0);
    pw.putSigned(16, 5);
    while (pw.bitCount() % 8) pw.put(1, 0);
    return buildFrame(pw.bytes());
}

/// IM = 99 is unrecognised, so the decoder rejects the frame and takes its
/// trace-level "unsupported sub-type" branch — a log call on a failure path.
std::vector<uint8_t> build4076UnsupportedImFrame() {
    BitWriter pw;
    pw.put(12, 4076);
    pw.put(3, 1);
    pw.put(8, 99);
    while (pw.bitCount() < 8 * 8) pw.put(8, 0);
    return buildFrame(pw.bytes());
}

// ============================================================================
// Tests
// ============================================================================

// 1033 logs once per parser at info — the first case that faulted.
TEST(RtcmLoggerLifetimeTest, Decode1033SurvivesDroppedRegistry) {
    const auto frame = build1033Frame();
    DroppedRegistry guard;

    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(frame));
    ASSERT_EQ(parser.getReceiverDescriptors().size(), 1u);
    EXPECT_EQ(parser.getReceiverDescriptors().front().reference_station_id, 1234);
}

// 1230 logs once per parser at info.
TEST(RtcmLoggerLifetimeTest, Decode1230SurvivesDroppedRegistry) {
    const auto frame = build1230Frame();
    DroppedRegistry guard;

    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(frame));
    ASSERT_EQ(parser.getGlonassBiases().size(), 1u);
    EXPECT_EQ(parser.getGlonassBiases().front().reference_station_id, 77);
}

// The 4076 reject path logs at trace.  Level filtering happens INSIDE the
// logger, so a trace call still dereferenced the null default logger before
// any level check could discard it — this case faulted too.
TEST(RtcmLoggerLifetimeTest, Rejected4076SurvivesDroppedRegistry) {
    const auto frame = build4076UnsupportedImFrame();
    DroppedRegistry guard;

    RtcmParser parser;
    EXPECT_FALSE(parser.parseFrame(frame));
    EXPECT_EQ(parser.lastMessageType(), 4076);
    EXPECT_TRUE(parser.getSsrClocks().empty());
}

// A frame whose type the parser does not decode takes the trailing
// "not yet decoded" trace branch in parseFrame() itself.
TEST(RtcmLoggerLifetimeTest, UndecodedTypeSurvivesDroppedRegistry) {
    BitWriter pw;
    pw.put(12, 1013);                       // system parameters — not decoded here
    while (pw.bitCount() < 8 * 8) pw.put(8, 0);
    const auto frame = buildFrame(pw.bytes());
    DroppedRegistry guard;

    RtcmParser parser;
    EXPECT_FALSE(parser.parseFrame(frame));
    EXPECT_EQ(parser.lastMessageType(), 1013);
}

// The guard itself must leave the registry usable, or every later test in the
// binary inherits the very defect this file guards against.
TEST(RtcmLoggerLifetimeTest, GuardRestoresDefaultLogger) {
    // Deliberately no assertion that a default exists on entry: this is a
    // round-trip check, and demanding a non-null default here would turn any
    // earlier test's registry damage into a second, misleading failure.
    const auto before = spdlog::default_logger();
    {
        DroppedRegistry guard;
        EXPECT_EQ(spdlog::default_logger(), nullptr);
    }
    EXPECT_EQ(spdlog::default_logger(), before);
}

// A crash-only guard cannot tell "degraded to the fallback" from "silently
// swallowed": a fix shaped `if (auto l = spdlog::default_logger()) l->info(...)`
// passes every test above while losing the diagnostic.  Bind the output and
// assert the message actually survives the dropped registry.
TEST(RtcmLoggerLifetimeTest, Decode1033DiagnosticSurvivesDroppedRegistry) {
    const auto frame = build1033Frame();
    std::ostringstream oss;
    {
        DroppedRegistry guard;
        SinkCapture capture(Logging::fallbackConsoleLogger(), oss);

        RtcmParser parser;
        ASSERT_TRUE(parser.parseFrame(frame));
        Logging::fallbackConsoleLogger()->flush();
    }
    EXPECT_NE(oss.str().find("RTCM 1033"), std::string::npos)
        << "the 1033 descriptor diagnostic was dropped rather than routed to the "
           "fallback; captured: [" << oss.str() << "]";
}

} // namespace
} // namespace GnssProcessor
