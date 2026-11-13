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
 *   - GPSRawCA (4017), GALRawINAV (4023), BDSRaw (4047), GLORawCA (4026)
 *                          -> RawNavMessage
 *   - PVTGeodetic (4007)   -> receiver ECEF a-priori position
 * The outputs use the same downstream types as the RTCM path (RinexObservation,
 * BroadcastEphemeris fed into LiveEphemerisStore), so the realtime engine
 * is format-agnostic past the framer.
 *
 * Field layouts are transcribed from the Septentrio SBF Reference Guide
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
 * The receiver emits its decoded ephemeris blocks (GPSNav 5891, …) only on
 * ephemeris change, which is too sparse for a cold start.  The RawNavBits
 * blocks (GPSRawCA 4017, GALRawINAV 4023, BDSRaw 4047, GLORawCA 4026) stream
 * the raw navigation subframes/pages/strings continuously.  This struct holds
 * one such message before @c SubframeAssembler stitches the subframes of one
 * satellite into a @c BroadcastEphemeris.
 *
 * @c words holds the SBF NAVBits verbatim, one little-endian u32 per entry.
 * For GPS/BeiDou each u32 carries one 30-bit nav word right-justified in bits
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
     * @param block Pointer to the full block as returned by @c SbfStreamReader::nextFrame,
     *              starting at the 0x24 0x40 sync bytes.
     * @param len   Block length in bytes; a block shorter than 14 bytes is rejected.
     * @return true if the block was recognised and decoded; false for an
     *         unsupported block number or a malformed/short block.  An unknown
     *         block is not an error: it is logged at trace level and returns false.
     */
    bool parseFrame(const uint8_t* block, std::size_t len);

    /// @brief Convenience overload.
    /// @param block Complete SBF block (Sync..body); same contract as the pointer overload.
    /// @return true if the block was recognised and decoded; false otherwise.
    bool parseFrame(const std::vector<uint8_t>& block) {
        return parseFrame(block.data(), block.size());
    }

    /// Drop all accumulated decoded outputs.
    void clear();

    /// @brief Observations decoded since the last @c clear() (MeasEpoch and Meas3Ranges blocks).
    /// @return Reference to the accumulated observations, one per satellite per epoch, in decode
    ///         order; empty when none was decoded.  Valid until @c clear() or destruction.
    const std::vector<RinexObservation>&   getObservations()      const { return observations_; }
    /// @brief Broadcast ephemerides decoded since the last @c clear() (GPSNav, GALNav, BDSNav,
    ///        QZSNav, GLONav blocks).
    /// @return Reference to the accumulated ephemerides in decode order; empty when none was
    ///         decoded.  Valid until @c clear() or destruction.
    const std::vector<BroadcastEphemeris>& getBroadcastEphemeris() const { return broadcast_eph_; }
    /// @brief Raw navigation messages lifted since the last @c clear() (GPSRawCA, GALRawINAV,
    ///        BDSRaw, GLORawCA blocks).
    /// @return Reference to the accumulated messages in decode order; empty when none was
    ///         lifted.  Valid until @c clear() or destruction.
    const std::vector<RawNavMessage>&      getRawNavMessages()    const { return raw_nav_; }

    /// @brief Receiver ECEF position [m] from a PVTGeodetic (4007) block with a valid
    /// fix, or @c std::nullopt when no such block has been parsed since the last
    /// @c clear().  Used
    /// to seed the filter a-priori on a live stream (no RINEX header available).
    /// @return Reference to the optional position (ECEF, metres); empty when no valid fix has
    ///         been parsed.  Valid until @c clear() or destruction.
    const std::optional<Eigen::Vector3d>&  getReceiverPositionEcef() const { return receiver_position_ecef_; }

    /// @brief Block number of the most recently parsed block, or -1.
    /// @return The 13-bit SBF block number (for example 4027 for MeasEpoch) of the last block
    ///         passed to @c parseFrame, whether or not it was decoded; -1 after @c clear() or
    ///         before the first valid block.
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
