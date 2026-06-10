/**
 * @file SbfStreamReader.cpp
 * @brief Implementation of the incremental SBF block extractor.
 */

#include "sbf/SbfStreamReader.hpp"

namespace GnssProcessor {

void SbfStreamReader::feed(const uint8_t* data, std::size_t len) {
    buf_.insert(buf_.end(), data, data + len);
}

uint16_t SbfStreamReader::crc16Xmodem(const uint8_t* data, std::size_t len) {
    uint16_t crc = 0x0000;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x8000u) ? static_cast<uint16_t>((crc << 1) ^ 0x1021u)
                                  : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

std::optional<std::vector<uint8_t>> SbfStreamReader::nextFrame() {
    for (;;) {
        // Resync: drop bytes until the buffer starts with the "$@" preamble.
        while (buf_.size() >= 2 && !(buf_[0] == 0x24 && buf_[1] == 0x40)) {
            buf_.pop_front();
            ++bytes_discarded_;
        }
        if (buf_.size() < 8) return std::nullopt;   // need the full 8-byte header

        const std::size_t length =
            static_cast<std::size_t>(buf_[6]) | (static_cast<std::size_t>(buf_[7]) << 8);

        // A valid SBF block length is a multiple of 4 and at least the header.
        if (length < 8 || (length % 4) != 0 || length > kMaxBlock) {
            buf_.pop_front();                       // false sync — drop '$' and rescan
            ++bytes_discarded_;
            continue;
        }
        if (buf_.size() < length) return std::nullopt;   // wait for the whole block

        std::vector<uint8_t> block(buf_.begin(),
                                   buf_.begin() + static_cast<std::ptrdiff_t>(length));
        const uint16_t crc_field =
            static_cast<uint16_t>(block[2]) | (static_cast<uint16_t>(block[3]) << 8);
        const uint16_t crc_calc = crc16Xmodem(block.data() + 4, length - 4);
        if (crc_calc != crc_field) {
            buf_.pop_front();                       // bad CRC — treat as false sync
            ++bytes_discarded_;
            continue;
        }

        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(length));
        ++frames_extracted_;
        return block;
    }
}

} // namespace GnssProcessor
