/**
 * @file test_rtcm_ssr_phase_bias.cpp
 * @brief Bit layout and signal ids of the SSR bias messages (IGS-SSR 4076 IM x5 and x6,
 *        legacy 1265-1270).
 *
 * Coverage:
 *   - The dispersive-bias and Melbourne-Wübbena consistency indicators are read
 *     once, in the message header after the solution id and before the satellite
 *     count, and copied onto every record (the layout of the cssrlib reference
 *     decoder).  Reading them per satellite instead shifts the satellite count by
 *     two bits and misaligns every satellite block after the first.
 *   - Every satellite of a multi-satellite message decodes with its own id, yaw
 *     and per-signal fields, in both dialects, and the message is consumed
 *     exactly (the decode reports ok).
 *   - Code- and phase-bias signal ids are read in the SSR numbering (GPS L1C is
 *     id 0, L2W id 11; Galileo L5Q id 6), not in the MSM observation numbering.
 *   - The wire bias (a correction added to the observation) is stored negated, as the
 *     bias present in the observation.
 *
 * The synthetic frames carry zero CRC bytes; the parser assumes the framer verified them.
 */

#include <gtest/gtest.h>

#include "rtcm/RtcmParser.hpp"
#include "rtcm/RtcmTypes.hpp"

#include <cstdint>
#include <string>
#include <utility>
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

// One signal of a phase-bias satellite block.
struct PbSignal {
    int  sig_id;        // SSR signal and tracking mode id (5 bits)
    bool integer;       // signal integer indicator
    int  wl;            // wide-lane integer indicator (2 bits)
    int  disc;          // discontinuity counter (4 bits)
    int  bias_raw;      // phase bias, 0.1 mm units (20 bits, two's complement)
};

// One satellite block of a phase-bias message.
struct PbSat {
    int                   sat_id;
    int                   yaw_raw;       // 1/256 semicircle (9 bits)
    int                   yaw_rate_raw;  // 1/8192 semicircle/s (8 bits, two's complement)
    std::vector<PbSignal> sigs;
};

// Phase-bias header from the epoch field to the satellite count.  The two
// consistency indicators sit after the solution id, once per message.
void putPhaseBiasHeader(BitWriter& pw, int epoch_bits, uint32_t epoch_s, int iod_ssr,
                        int dispersive, int mw, int n_sats) {
    pw.put(epoch_bits, epoch_s);   // epoch (GPS TOW, or GLONASS TOD in legacy 1266)
    pw.put(4,  0);                 // update interval
    pw.put(1,  0);                 // multi-message indicator
    pw.put(4,  iod_ssr);           // IOD-SSR
    pw.put(16, 1234);              // provider id
    pw.put(4,  2);                 // solution id
    pw.put(1,  dispersive);        // dispersive bias consistency indicator
    pw.put(1,  mw);                // Melbourne-Wübbena consistency indicator
    pw.put(6,  n_sats);            // number of satellites
}

// One satellite block: the id, then the signal count, yaw and yaw rate; no flags.
void putPhaseBiasSat(BitWriter& pw, int sat_id_bits, const PbSat& s) {
    pw.put(sat_id_bits, s.sat_id);
    pw.put(5, s.sigs.size());
    pw.put(9, s.yaw_raw);
    pw.putSigned(8, s.yaw_rate_raw);
    for (const auto& g : s.sigs) {
        pw.put(5, g.sig_id);
        pw.put(1, g.integer ? 1 : 0);
        pw.put(2, g.wl);
        pw.put(4, g.disc);
        pw.putSigned(20, g.bias_raw);
    }
}

const SsrPhaseBias* findSat(const std::vector<SsrPhaseBias>& pbs, const std::string& sat) {
    for (const auto& pb : pbs) if (pb.sat == sat) return &pb;
    return nullptr;
}

// Check one decoded record against the block that was encoded for it.
void expectRecord(const SsrPhaseBias& pb, const PbSat& s,
                  const std::vector<std::string>& phase_codes,
                  int dispersive, int mw, int iod_ssr) {
    EXPECT_EQ(pb.dispersive_bias_consistency, dispersive);
    EXPECT_EQ(pb.mw_consistency, mw);
    EXPECT_EQ(pb.iod_ssr, iod_ssr);
    EXPECT_NEAR(pb.yaw_angle_deg, s.yaw_raw * 180.0 / 256.0, 1e-12);
    EXPECT_NEAR(pb.yaw_rate_deg_per_s, s.yaw_rate_raw * 180.0 / 8192.0, 1e-12);
    ASSERT_EQ(pb.bias_per_signal.size(), s.sigs.size());
    for (std::size_t k = 0; k < s.sigs.size(); ++k) {
        const auto it = pb.bias_per_signal.find(phase_codes[k]);
        ASSERT_NE(it, pb.bias_per_signal.end()) << pb.sat << " " << phase_codes[k];
        // The wire value is the correction added to the phase; the record holds its negative.
        EXPECT_NEAR(it->second.bias_m, -s.sigs[k].bias_raw * 1e-4, 1e-12);
        EXPECT_EQ(it->second.signal_integer, s.sigs[k].integer);
        EXPECT_EQ(it->second.wide_lane_integer, s.sigs[k].wl);
        EXPECT_EQ(it->second.discontinuity_counter, s.sigs[k].disc);
    }
}

TEST(RtcmSsrPhaseBiasTest, IgsSsr4076GpsHeaderIndicatorsAndThreeSatellites) {
    // IM 26 = GPS phase bias.  SSR signal ids 0 = 1C, 11 = 2W.
    const std::vector<PbSat> sats = {
        { 2, 128,  -4, {{0, true, 2, 1,  12345}, {11, true, 2, 1, -6789}}},
        { 7,  64,   3, {{0, true, 2, 3, -40001}, {11, true, 2, 3,  20202}}},
        {31, 300,   0, {{0, true, 2, 0,      7}, {11, true, 2, 0,  -9999}}},
    };
    BitWriter pw;
    pw.put(12, 4076);              // DF002
    pw.put(3,  1);                 // IGS-SSR version
    pw.put(8,  26);                // IM: GPS phase bias
    putPhaseBiasHeader(pw, 20, 3600, /*iod_ssr=*/5, /*dispersive=*/1, /*mw=*/0,
                       static_cast<int>(sats.size()));
    for (const auto& s : sats) putPhaseBiasSat(pw, 6, s);
    const auto frame = buildFrame(pw.finish());

    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(frame));
    const auto& pbs = parser.getSsrPhaseBiases();
    ASSERT_EQ(pbs.size(), sats.size());

    const auto& hdr = parser.getLastSsrHeader();
    EXPECT_EQ(hdr.dispersive_bias_consistency, 1);
    EXPECT_EQ(hdr.mw_consistency, 0);
    EXPECT_EQ(hdr.provider_id, 1234);
    EXPECT_EQ(hdr.solution_id, 2);

    const std::vector<std::string> codes = {"L1C", "L2W"};
    const char* names[] = {"G02", "G07", "G31"};
    for (std::size_t i = 0; i < sats.size(); ++i) {
        const SsrPhaseBias* pb = findSat(pbs, names[i]);
        ASSERT_NE(pb, nullptr) << names[i];
        expectRecord(*pb, sats[i], codes, 1, 0, 5);
    }
}

TEST(RtcmSsrPhaseBiasTest, Legacy1267GalileoHeaderIndicatorsAndTwoSatellites) {
    // Legacy Galileo phase bias: 20-bit GPS epoch, 6-bit satellite id.
    // SSR signal ids 2 = 1C, 6 = 5Q.
    const std::vector<PbSat> sats = {
        { 5, 10,  1, {{2, true, 0, 2,  5000}, {6, true, 0, 2, -5000}}},
        {24, 20, -1, {{2, true, 0, 9, -1111}, {6, true, 0, 9,  2222}}},
    };
    BitWriter pw;
    pw.put(12, 1267);              // DF002
    putPhaseBiasHeader(pw, 20, 7200, /*iod_ssr=*/9, /*dispersive=*/0, /*mw=*/1,
                       static_cast<int>(sats.size()));
    for (const auto& s : sats) putPhaseBiasSat(pw, 6, s);
    const auto frame = buildFrame(pw.finish());

    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(frame));
    const auto& pbs = parser.getSsrPhaseBiases();
    ASSERT_EQ(pbs.size(), sats.size());

    const std::vector<std::string> codes = {"L1C", "L5Q"};
    const char* names[] = {"E05", "E24"};
    for (std::size_t i = 0; i < sats.size(); ++i) {
        const SsrPhaseBias* pb = findSat(pbs, names[i]);
        ASSERT_NE(pb, nullptr) << names[i];
        expectRecord(*pb, sats[i], codes, 0, 1, 9);
    }
}

TEST(RtcmSsrPhaseBiasTest, Legacy1265GpsCountIsNotShiftedByIndicators) {
    // Both indicators set: a decoder that skipped them would read the count as
    // 0b110000 | (4 >> 2) = 49 instead of 4 and run off the end of the message.
    const std::vector<PbSat> sats = {
        { 1, 0, 0, {{0, true, 1, 0,  100}}},
        { 3, 0, 0, {{0, true, 1, 0,  200}}},
        {12, 0, 0, {{0, true, 1, 0,  300}}},
        {30, 0, 0, {{0, true, 1, 0, -400}}},
    };
    BitWriter pw;
    pw.put(12, 1265);              // DF002
    putPhaseBiasHeader(pw, 20, 100, /*iod_ssr=*/3, /*dispersive=*/1, /*mw=*/1,
                       static_cast<int>(sats.size()));
    for (const auto& s : sats) putPhaseBiasSat(pw, 6, s);
    const auto frame = buildFrame(pw.finish());

    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(frame));
    const auto& pbs = parser.getSsrPhaseBiases();
    ASSERT_EQ(pbs.size(), sats.size());

    const std::vector<std::string> codes = {"L1C"};
    const char* names[] = {"G01", "G03", "G12", "G30"};
    for (std::size_t i = 0; i < sats.size(); ++i) {
        const SsrPhaseBias* pb = findSat(pbs, names[i]);
        ASSERT_NE(pb, nullptr) << names[i];
        expectRecord(*pb, sats[i], codes, 1, 1, 3);
    }
}

TEST(RtcmSsrPhaseBiasTest, NonPhaseBiasHeadersCarryNoIndicators) {
    // IGS-SSR GPS clock (IM 22): no consistency indicators in its header; the
    // satellite count follows the solution id directly.
    BitWriter pw;
    pw.put(12, 4076);
    pw.put(3,  1);
    pw.put(8,  22);                // IM: GPS clock
    pw.put(20, 100);
    pw.put(4,  0);
    pw.put(1,  0);
    pw.put(4,  4);                 // IOD-SSR
    pw.put(16, 0);
    pw.put(4,  0);
    pw.put(6,  1);                 // one satellite
    pw.put(6,  9);                 // G09
    pw.putSigned(22, 250);         // C0 = 0.025 m
    pw.putSigned(21, 0);
    pw.putSigned(27, 0);
    const auto frame = buildFrame(pw.finish());

    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(frame));
    ASSERT_EQ(parser.getSsrClocks().size(), 1u);
    EXPECT_EQ(parser.getSsrClocks().front().sat, "G09");
    const auto& hdr = parser.getLastSsrHeader();
    EXPECT_EQ(hdr.dispersive_bias_consistency, 0);
    EXPECT_EQ(hdr.mw_consistency, 0);
}

// IGS-SSR code bias (IM x5): header without consistency indicators, then per
// satellite the id, the signal count and per signal the id and a 14-bit bias (0.01 m).
std::vector<uint8_t> buildCodeBias4076(int im, int sat_id,
                                       const std::vector<std::pair<int, int>>& sigs) {
    BitWriter pw;
    pw.put(12, 4076);
    pw.put(3,  1);
    pw.put(8,  im);
    pw.put(20, 100);               // epoch
    pw.put(4,  0);                 // update interval
    pw.put(1,  0);                 // multi-message indicator
    pw.put(4,  1);                 // IOD-SSR
    pw.put(16, 0);                 // provider id
    pw.put(4,  0);                 // solution id
    pw.put(6,  1);                 // one satellite
    pw.put(6,  sat_id);
    pw.put(5,  sigs.size());
    for (const auto& [id, raw] : sigs) {
        pw.put(5, id);
        pw.putSigned(14, raw);
    }
    return buildFrame(pw.finish());
}

TEST(RtcmSsrSignalIdTest, GpsCodeBiasUsesSsrNumbering) {
    // SSR ids 0 = 1C, 2 = 1W, 8 = 2L, 11 = 2W.  In the MSM numbering id 0 is reserved,
    // 2 is 1C and 11 is unassigned, so an MSM read drops C1C and C2W and files the
    // C1W bias under C1C.
    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(buildCodeBias4076(25, 5, {{0, 120}, {2, -80}, {8, 45}, {11, 300}})));
    ASSERT_EQ(parser.getSsrCodeBiases().size(), 1u);
    const auto& cb = parser.getSsrCodeBiases().front();
    EXPECT_EQ(cb.sat, "G05");
    ASSERT_EQ(cb.bias_per_signal.size(), 4u);
    EXPECT_NEAR(cb.bias_per_signal.at("C1C"), -1.20, 1e-12);
    EXPECT_NEAR(cb.bias_per_signal.at("C1W"),  0.80, 1e-12);
    EXPECT_NEAR(cb.bias_per_signal.at("C2L"), -0.45, 1e-12);
    EXPECT_NEAR(cb.bias_per_signal.at("C2W"), -3.00, 1e-12);
}

TEST(RtcmSsrSignalIdTest, GalileoCodeBiasUsesSsrNumbering) {
    // SSR ids 2 = 1C, 6 = 5Q, 9 = 7Q, 16 = 6C (MSM: 1C, 1Z, 6A, 7X).
    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(buildCodeBias4076(65, 11, {{2, 10}, {6, 20}, {9, 30}, {16, 40}})));
    ASSERT_EQ(parser.getSsrCodeBiases().size(), 1u);
    const auto& cb = parser.getSsrCodeBiases().front();
    EXPECT_EQ(cb.sat, "E11");
    ASSERT_EQ(cb.bias_per_signal.size(), 4u);
    EXPECT_NEAR(cb.bias_per_signal.at("C1C"), -0.10, 1e-12);
    EXPECT_NEAR(cb.bias_per_signal.at("C5Q"), -0.20, 1e-12);
    EXPECT_NEAR(cb.bias_per_signal.at("C7Q"), -0.30, 1e-12);
    EXPECT_NEAR(cb.bias_per_signal.at("C6C"), -0.40, 1e-12);
}

// RTCM and IGS-SSR send each bias as the correction added to the observation (the cssrlib
// model is range - bias with the wire value).  The store holds the bias present in the
// observation, which the model adds (kSatBiasSign), so the decoder negates: a wire +2.50 m
// code bias and a wire +0.1234 m phase bias are stored as -2.50 m and -0.1234 m.
TEST(RtcmSsrSignalIdTest, WireBiasIsTheNegativeOfTheStoredBias) {
    RtcmParser code_parser;
    ASSERT_TRUE(code_parser.parseFrame(buildCodeBias4076(25, 3, {{0, 250}})));
    ASSERT_EQ(code_parser.getSsrCodeBiases().size(), 1u);
    EXPECT_NEAR(code_parser.getSsrCodeBiases().front().bias_per_signal.at("C1C"), -2.50, 1e-12);

    BitWriter pw;
    pw.put(12, 4076);
    pw.put(3,  1);
    pw.put(8,  26);                // IM: GPS phase bias
    putPhaseBiasHeader(pw, 20, 100, /*iod_ssr=*/1, /*dispersive=*/0, /*mw=*/0, 1);
    putPhaseBiasSat(pw, 6, PbSat{3, 0, 0, {{0, true, 0, 0, 1234}}});
    RtcmParser phase_parser;
    ASSERT_TRUE(phase_parser.parseFrame(buildFrame(pw.finish())));
    ASSERT_EQ(phase_parser.getSsrPhaseBiases().size(), 1u);
    EXPECT_NEAR(phase_parser.getSsrPhaseBiases().front().bias_per_signal.at("L1C").bias_m,
                -0.1234, 1e-12);
}

} // namespace
} // namespace GnssProcessor
