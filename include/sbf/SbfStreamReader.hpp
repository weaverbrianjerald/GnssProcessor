#pragma once
/**
 * @file SbfStreamReader.hpp
 * @brief Stateful SBF block extractor over an incremental byte stream.
 *
 * The SBF analogue of @c RtcmStreamReader: the caller pushes raw bytes (from
 * TCP, NTRIP, serial, or a file) via feed(), then pulls complete, CRC-verified
 * SBF blocks via nextFrame().  This decouples the transport from the SBF
 * parser so the same parser serves both file replay and live streams.
 *
 * SBF block format (little-endian):
 * @code
 *   [Sync 0x24 0x40] [CRC u16] [ID u16] [Length u16] [body ...]
 * @endcode
 * Length is the total block size in bytes, including the 8-byte header, and is
 * always a multiple of 4.  The CRC-16/CCITT-XMODEM (poly 0x1021, init 0x0000,
 * no reflection, no final XOR) covers bytes [offset 4 .. Length-1], that is,
 * everything after the 2 CRC bytes (ID + Length + body).
 */

#include <cstdint>
#include <cstddef>
#include <deque>
#include <optional>
#include <vector>

namespace GnssProcessor {

/**
 * @brief Incrementally extracts complete, CRC-verified SBF blocks.
 */
class SbfStreamReader {
public:
    /// Push raw bytes into the internal buffer.
    void feed(const uint8_t* data, std::size_t len);

    /// Convenience overload.
    void feed(const std::vector<uint8_t>& data) { feed(data.data(), data.size()); }

    /**
     * @brief Extract the next complete SBF block from the buffer.
     * @return A complete block (Sync..body), or std::nullopt if no complete,
     *         valid block is available yet.  Resyncs past garbage / bad CRC.
     */
    std::optional<std::vector<uint8_t>> nextFrame();

    /// Number of raw bytes currently buffered.
    std::size_t buffered() const { return buf_.size(); }

    /// Total blocks successfully extracted since construction.
    uint64_t framesExtracted() const { return frames_extracted_; }

    /// Total bytes discarded while scanning for a valid block.
    uint64_t bytesDiscarded() const { return bytes_discarded_; }

private:
    /// Computes the CRC-16/CCITT-XMODEM (poly 0x1021, init 0x0000) over [data, data+len).
    static uint16_t crc16Xmodem(const uint8_t* data, std::size_t len);

    static constexpr std::size_t kMaxBlock = 65532;  // max u16 length, multiple of 4

    std::deque<uint8_t> buf_;
    uint64_t frames_extracted_{0};
    uint64_t bytes_discarded_{0};
};

} // namespace GnssProcessor
