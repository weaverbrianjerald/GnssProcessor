/**
 * @file test_ssr_bias_coverage.cpp
 * @brief SSR bias coverage counts a fresh record that holds at least one bias.
 *
 * Coverage:
 *   - SsrCorrectionStore::coverageFor counts a code- or phase-bias record only when
 *     its bias map is not empty and its receipt stamp is within the age limit, the
 *     gate the orbit and clock counts already had.
 *   - A decoded IGS-SSR phase-bias message whose satellite block carries no bias
 *     (the BKG and GFZ streams of 2026-11-03) reaches the store as an empty record
 *     and is not counted.
 *
 * The defect: on 2026-11-03 the engine's coverage line read 18 of 18 satellites
 * with a phase bias on the BKG stream and 16 of 18 on GFZ while neither sent a
 * single phase bias (card SSR-BIAS-COV).
 *
 * The synthetic frame carries zero CRC bytes, which the parser does not inspect.
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

// MSB-first bit writer and RTCM3 framing (as in test_rtcm_ssr_phase_bias.cpp).
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

// IGS-SSR GPS phase-bias message (4076, IM 26) with one satellite block, G03,
// that carries no bias: the content of the BKG and GFZ phase-bias messages.
std::vector<uint8_t> emptyGpsPhaseBias4076(uint32_t epoch_s) {
    BitWriter pw;
    pw.put(12, 4076);
    pw.put(3,  1);              // IGS-SSR version
    pw.put(8,  26);             // IM 26: GPS phase bias
    pw.put(20, epoch_s);        // epoch (GPS TOW)
    pw.put(4,  2);              // update interval, 5 s
    pw.put(1,  0);              // multi-message indicator
    pw.put(4,  1);              // IOD-SSR
    pw.put(16, 0);              // provider id
    pw.put(4,  0);              // solution id
    pw.put(1,  0);              // dispersive bias consistency indicator
    pw.put(1,  0);              // Melbourne-Wuebbena consistency indicator
    pw.put(6,  1);              // one satellite
    pw.put(6,  3);              // G03
    pw.put(5,  0);              // no biases
    pw.put(9,  0);              // yaw
    pw.put(8,  0);              // yaw rate
    return pw.finish();
}

constexpr int kWeek = 2300;
GnssTime at(double tow) { return GnssTime{kWeek, tow}; }

constexpr double kMaxAge = 60.0;

SsrCodeBias codeBias(const std::string& sat, double received_tow, bool with_bias) {
    SsrCodeBias cb;
    cb.sat           = sat;
    cb.received_time = at(received_tow);
    if (with_bias) cb.bias_per_signal["C1C"] = -1.2;
    return cb;
}

SsrPhaseBias phaseBias(const std::string& sat, double received_tow, bool with_bias) {
    SsrPhaseBias pb;
    pb.sat           = sat;
    pb.received_time = at(received_tow);
    if (with_bias) {
        SsrPhaseBiasEntry e;
        e.bias_m = 0.05;
        pb.bias_per_signal["L1C"] = e;
    }
    return pb;
}

TEST(SsrBiasCoverageTest, FreshRecordWithABiasCounts) {
    SsrCorrectionStore store;
    store.pushCodeBias(codeBias("G05", 100.0, true), SsrHeader{});
    store.pushPhaseBias(phaseBias("G05", 100.0, true), SsrHeader{});
    const auto cov = store.coverageFor({"G05"}, at(102.0), kMaxAge);
    EXPECT_EQ(cov.total, 1);
    EXPECT_EQ(cov.code_bias, 1);
    EXPECT_EQ(cov.phase_bias, 1);
}

TEST(SsrBiasCoverageTest, RecordWithoutBiasesIsNotCoverage) {
    SsrCorrectionStore store;
    store.pushCodeBias(codeBias("G05", 100.0, false), SsrHeader{});
    store.pushPhaseBias(phaseBias("G05", 100.0, false), SsrHeader{});
    const auto cov = store.coverageFor({"G05"}, at(102.0), kMaxAge);
    EXPECT_EQ(cov.code_bias, 0);
    EXPECT_EQ(cov.phase_bias, 0);
}

TEST(SsrBiasCoverageTest, StaleRecordIsNotCoverage) {
    // A record never refreshed: counted inside the age limit, not after it.
    SsrCorrectionStore store;
    store.pushCodeBias(codeBias("G05", 100.0, true), SsrHeader{});
    store.pushPhaseBias(phaseBias("G05", 100.0, true), SsrHeader{});
    const auto inside = store.coverageFor({"G05"}, at(160.0), kMaxAge);
    EXPECT_EQ(inside.code_bias, 1);
    EXPECT_EQ(inside.phase_bias, 1);
    const auto after = store.coverageFor({"G05"}, at(161.0), kMaxAge);
    EXPECT_EQ(after.code_bias, 0);
    EXPECT_EQ(after.phase_bias, 0);
}

TEST(SsrBiasCoverageTest, CountsArePerSatellite) {
    SsrCorrectionStore store;
    store.pushPhaseBias(phaseBias("G05", 100.0, true), SsrHeader{});
    store.pushPhaseBias(phaseBias("G07", 100.0, false), SsrHeader{});
    store.pushPhaseBias(phaseBias("E11", 10.0, true), SsrHeader{});
    const auto cov = store.coverageFor({"G05", "G07", "E11", "E12"}, at(102.0), kMaxAge);
    EXPECT_EQ(cov.total, 4);
    EXPECT_EQ(cov.phase_bias, 1);
}

TEST(SsrBiasCoverageTest, DecodedPhaseBiasBlockWithoutBiasesIsNotCoverage) {
    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(buildFrame(emptyGpsPhaseBias4076(100))));
    ASSERT_EQ(parser.getSsrPhaseBiases().size(), 1u);
    SsrPhaseBias pb = parser.getSsrPhaseBiases().front();
    EXPECT_EQ(pb.sat, "G03");
    EXPECT_TRUE(pb.bias_per_signal.empty());

    // Stamp the receipt on the test's time line (the decoder uses the host clock).
    pb.received_time = at(101.0);
    SsrCorrectionStore store;
    store.pushPhaseBias(pb, SsrHeader{});
    EXPECT_EQ(store.coverageFor({"G03"}, at(102.0), kMaxAge).phase_bias, 0);
}

} // namespace
} // namespace GnssProcessor
