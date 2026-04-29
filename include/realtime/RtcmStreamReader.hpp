/**
 * @file RtcmStreamReader.hpp
 * @brief Stateful RTCM3 frame extractor that works on an incremental byte stream.
 *
 * The reader maintains an internal byte buffer.  The caller pushes raw bytes
 * (from NTRIP, TCP, serial, or a file) via feed(), then pulls complete RTCM3
 * frames via nextFrame().  This decouples the transport layer from the RTCM
 * parser so the same parser works for both file replay and live streams.
 */
#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace GnssProcessor {

/**
 * @brief Incrementally extracts RTCM3 frames from a raw byte stream.
 *
 * RTCM3 frame format:
 * @code
 *   [preamble 0xD3] [6-bit reserved | 10-bit length] [message body ...] [24-bit CRC]
 * @endcode
 * Total frame size = 3 (header) + length + 3 (CRC).
 */
class RtcmStreamReader {
public:
    /// Push raw bytes into the internal buffer.
    void feed(const uint8_t* data, size_t len);

    /// Convenience overload.
    void feed(const std::vector<uint8_t>& data) { feed(data.data(), data.size()); }

    /**
     * @brief Extract the next complete RTCM3 frame from the buffer.
     *
     * @return A complete frame (header + body + CRC), or std::nullopt if no
     *         complete frame is available yet.
     */
    std::optional<std::vector<uint8_t>> nextFrame();

    /// Number of raw bytes currently buffered.
    size_t buffered() const { return buf_.size(); }

    /// Total frames successfully extracted since construction.
    uint64_t framesExtracted() const { return frames_extracted_; }

    /// Total bytes discarded while scanning for the preamble.
    uint64_t bytesDiscarded() const { return bytes_discarded_; }

private:
    /// Compute the RTCM3 24-bit CRC over [data, data+len).
    static uint32_t crc24q(const uint8_t* data, size_t len);

    std::deque<uint8_t> buf_;
    uint64_t frames_extracted_{ 0 };
    uint64_t bytes_discarded_{ 0 };
};

} // namespace GnssProcessor
