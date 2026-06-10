#pragma once
/**
 * @file SbfParser.hpp
 * @brief Top-level dispatcher for decoded Septentrio SBF blocks.
 *
 * The SBF analogue of @c RtcmParser.  Accepts complete SBF blocks (already
 * sync- and CRC-verified by @c SbfStreamReader) and decodes:
 *   - MeasEpoch (4027)     -> RinexObservation (one per satellite/epoch)
 *   - Meas3Ranges (4109)   -> RinexObservation, via the stateful @c Meas3Decoder
 *                             (compact differential code+phase; see that class)
 *   - GPSNav (5891), GALNav (4002), BDSNav (4081), QZSNav (4095),
 *     GLONav (4004) -> BroadcastEphemeris
 * reusing the SAME downstream types as the RTCM path (RinexObservation,
 * BroadcastEphemeris fed into LiveEphemerisStore), so the realtime engine
 * is format-agnostic past the framer.
 *
 * Field layouts transcribed from the Septentrio SBF Reference Guide
 * (mosaic-X5 v4.8.0) and cross-checked against RTKLIB septentrio.c.  SBF is
 * little-endian; this parser assumes a little-endian host (x64/MSVC).
 */

#include "rinex/RinexParser.hpp"                // RinexObservation
#include "products/BroadcastEphemerisTypes.hpp" // BroadcastEphemeris
#include <cstdint>
#include <cstddef>
#include <optional>
#include <vector>

namespace GnssProcessor {

/**
 * @brief One raw navigation message lifted from an SBF RawNavBits block.
 *
 * The receiver emits its DECODED ephemeris blocks (GPSNav 5891, …) only on
 * ephemeris change, which is too sparse for a cold start.  The RawNavBits
 * blocks (GPSRawCA 4017, GALRawINAV 4023, BDSRaw 4047, GLORawCA 4026) instead
 * stream the raw navigation subframes/pages/strings continuously.  This carrier
 * holds one such message before @c SubframeAssembler stitches the subframes of
 * one satellite into a @c BroadcastEphemeris.
 *
 * @c words holds the SBF NAVBits verbatim — one little-endian u32 per entry;
 * for GPS/BeiDou each u32 carries one 30-bit nav word right-justified in bits
 * 29..0 (the GPS TLM preamble 0x8B sits in bits 29..22 of words[0]).
 */
struct RawNavMessage {
    std::string           sat;                    ///< Satellite ID ("G29","E18","C30","R18").
    System                system{System::Unknown};///< Constellation (selects the subframe format).
    int                   signal{0};              ///< SBF signal number (Source byte).
    int                   freq_nr{0};             ///< GLONASS FreqNr (k = freq_nr − 8); 0 otherwise.
    bool                  crc_passed{false};      ///< Receiver parity/CRC check passed.
    int                   wnc{0};                 ///< Block WNc (continuous GPS week) for ephemeris week resolution.
    double                tow_s{0.0};             ///< Block TOW (s of week) for the week-rollover guard.
    std::vector<uint32_t> words;                  ///< NAVBits words (one 30-bit nav word per u32 for GPS/BDS).
};

/**
 * @class SbfParser
 * @brief Dispatches complete SBF blocks to typed decoders.
 *
 * Outputs accumulate in per-type vectors until @c clear() is called.  The
 * realtime path constructs one parser per block, drains the outputs into the
 * engine, and discards it (mirroring the RTCM per-frame usage).
 */
class SbfParser {
public:
    /**
     * @brief Decode one complete SBF block (Sync..body).
     * @return true if the block was recognised and decoded; false for an
     *         unsupported block number or a malformed/short block.  Unknown
     *         blocks are NOT errors (logged at trace, return false).
     */
    bool parseFrame(const uint8_t* block, std::size_t len);

    /// Convenience overload.
    bool parseFrame(const std::vector<uint8_t>& block) {
        return parseFrame(block.data(), block.size());
    }

    /// Drop all accumulated decoded outputs.
    void clear();

    const std::vector<RinexObservation>&   getObservations()      const { return observations_; }
    const std::vector<BroadcastEphemeris>& getBroadcastEphemeris() const { return broadcast_eph_; }
    const std::vector<RawNavMessage>&      getRawNavMessages()    const { return raw_nav_; }

    /// Receiver ECEF position [m] from a PVTGeodetic (4007) block with a valid
    /// fix, or @c std::nullopt when no such block was in the parsed frame.  Used
    /// to seed the filter a-priori on a live stream (no RINEX header available).
    const std::optional<Eigen::Vector3d>&  getReceiverPositionEcef() const { return receiver_position_ecef_; }

    /// Block number of the most recently parsed block, or -1.
    int lastBlockNumber() const { return last_block_number_; }

private:
    bool decodeMeasEpoch(const uint8_t* block, std::size_t len, int revision);
    bool decodeMeas3Ranges(const uint8_t* block, std::size_t len);  // Meas3Ranges 4109 (compact code+phase)
    bool decodeGpsQzsNav(const uint8_t* block, std::size_t len);  // GPSNav 5891 / QZSNav 4095 (same layout)
    bool decodeGalNav(const uint8_t* block, std::size_t len);
    bool decodeBdsNav(const uint8_t* block, std::size_t len);
    bool decodeGlonav(const uint8_t* block, std::size_t len);
    bool decodeRawNav(const uint8_t* block, std::size_t len);    // RawNavBits container (4017/4023/4047/4026)
    bool decodePvtGeodetic(const uint8_t* block, std::size_t len); // PVTGeodetic 4007 -> receiver ECEF

    int last_block_number_{-1};
    std::vector<RinexObservation>   observations_;
    std::vector<BroadcastEphemeris> broadcast_eph_;
    std::vector<RawNavMessage>      raw_nav_;
    std::optional<Eigen::Vector3d>  receiver_position_ecef_;
};

} // namespace GnssProcessor
