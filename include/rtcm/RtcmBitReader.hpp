#pragma once
/**
 * @file RtcmBitReader.hpp
 * @brief Bit-level reader for RTCM 3.x message payloads.
 *
 * RTCM 3.x packs fields at arbitrary bit boundaries (MSB-first across the
 * byte stream).  This class provides typed extraction primitives:
 *   - @c u(n)   : unsigned integer in N bits
 *   - @c s(n)   : two's-complement signed integer in N bits
 *   - @c sm(n)  : sign-magnitude integer (MSB sign, remaining magnitude)
 *   - @c u64(n) / @c s64(n) / @c sm64(n) : 64-bit variants
 *
 * Sign-magnitude is required for several DFs (e.g. the GLONASS ephemeris
 * fields DF110-DF123 of message 1020, several SSR fields).  Two's-complement
 * is the common case for RTCM signed integers.
 *
 * The reader tracks a bit offset; reading past the end sets an error flag,
 * after which all subsequent reads return zero and @c ok() returns false.
 * Decoders should call @c ok() once after they finish a message to reject
 * malformed payloads.
 */

#include <cstdint>
#include <cstddef>

namespace GnssProcessor {

/**
 * @class RtcmBitReader
 * @brief Stateful, MSB-first bit-level reader over a contiguous byte buffer.
 *
 * Lifetime is bound to the buffer pointer @p data; the reader does not own
 * the bytes.  Typical use: construct over a payload pointer and the payload
 * length in bytes; call @c u / @c s / @c sm to extract DFs in declaration
 * order.
 */
class RtcmBitReader {
public:
    /**
     * @brief Construct a reader over a byte range.
     * @param data Pointer to the first byte.
     * @param len_bytes Length of the buffer in bytes.
     */
    RtcmBitReader(const uint8_t* data, std::size_t len_bytes)
        : data_(data), len_bits_(static_cast<std::size_t>(len_bytes) * 8u) {}

    /// @brief Read @p n bits (1..32) as an unsigned integer.
    ///
    /// @param n Field width in bits, for example 12 for the station id DF003.  Outside 1..32,
    ///        or with fewer than @p n bits left, the read fails: the error flag is set (see
    ///        @c ok), the position still advances by @p n when @p n is positive (clamped to the
    ///        end of the buffer) and 0 is returned.
    /// @return The field value, MSB first (the first bit read is the most significant); 0 on failure.
    uint32_t u(int n) {
        if (n <= 0 || n > 32 || !canRead(n)) { error_ = true; skip(n); return 0; }
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) {
            const std::size_t bit = bit_offset_ + static_cast<std::size_t>(i);
            const uint8_t byte = data_[bit >> 3];
            const int shift = 7 - static_cast<int>(bit & 7u);
            v = (v << 1) | ((byte >> shift) & 0x1u);
        }
        bit_offset_ += static_cast<std::size_t>(n);
        return v;
    }

    /// @brief Read @p n bits (1..32) as a two's-complement signed integer.
    ///
    /// @param n Field width in bits, for example 22 for the SSR radial orbit delta (0.1 mm per
    ///        count) or 16 for a GLONASS code-phase bias of message 1230 (0.02 m per count).
    ///        Failure behaviour as for @c u.
    /// @return The value sign-extended from bit @p n - 1 (negative when that bit is set);
    ///         0 on failure.
    int32_t s(int n) {
        const uint32_t raw = u(n);
        if (n <= 0 || n > 32) return 0;
        // Sign-extend if the high bit is set.
        const uint32_t mask = (n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1u);
        const uint32_t signbit = 1u << (n - 1);
        if (raw & signbit) {
            return static_cast<int32_t>(raw | ~mask);
        }
        return static_cast<int32_t>(raw);
    }

    /// @brief Read @p n bits (>=2) as a sign-magnitude signed integer.
    /// Bit layout: [sign(1)] [magnitude(n-1)].  Returns -magnitude if sign bit set.
    ///
    /// @param n Total field width in bits including the sign bit, for example 24 for the
    ///        GLONASS satellite velocity component DF110 or 22 for the clock offset tau_n DF123 of
    ///        message 1020.  The magnitude (@p n - 1 bits, at most 32) is read with @c u.
    ///        For @p n < 2 the error flag is set, nothing is consumed and 0 is returned.
    /// @return +magnitude, or -magnitude when the sign bit is set (a negative zero reads as 0);
    ///         0 on failure.
    int32_t sm(int n) {
        if (n < 2) { error_ = true; return 0; }
        const uint32_t sign = u(1);
        const uint32_t mag  = u(n - 1);
        const int32_t v = static_cast<int32_t>(mag);
        return sign ? -v : v;
    }

    /// @brief Read @p n bits (1..64) as an unsigned 64-bit integer.
    ///
    /// @param n Field width in bits, for fields wider than the 32 bits that @c u accepts.
    ///        Outside 1..64, or with fewer than @p n bits left, the read fails as for @c u.
    /// @return The field value, MSB first; 0 on failure.
    uint64_t u64(int n) {
        if (n <= 0 || n > 64 || !canRead(n)) { error_ = true; skip(n); return 0; }
        uint64_t v = 0;
        for (int i = 0; i < n; ++i) {
            const std::size_t bit = bit_offset_ + static_cast<std::size_t>(i);
            const uint8_t byte = data_[bit >> 3];
            const int shift = 7 - static_cast<int>(bit & 7u);
            v = (v << 1) | static_cast<uint64_t>((byte >> shift) & 0x1u);
        }
        bit_offset_ += static_cast<std::size_t>(n);
        return v;
    }

    /// @brief Read @p n bits (1..64) as a two's-complement signed 64-bit integer.
    ///
    /// @param n Field width in bits, for example 38 for each ECEF coordinate of messages
    ///        1005 / 1006 (0.0001 m per count).  Failure behaviour as for @c u64.
    /// @return The value sign-extended from bit @p n - 1 (negative when that bit is set);
    ///         0 on failure.
    int64_t s64(int n) {
        const uint64_t raw = u64(n);
        if (n <= 0 || n > 64) return 0;
        const uint64_t mask = (n == 64) ? 0xFFFFFFFFFFFFFFFFull : ((1ull << n) - 1ull);
        const uint64_t signbit = 1ull << (n - 1);
        if (raw & signbit) {
            return static_cast<int64_t>(raw | ~mask);
        }
        return static_cast<int64_t>(raw);
    }

    /// @brief Read @p n bits (>=2) as a sign-magnitude signed 64-bit integer.
    ///
    /// @param n Total field width in bits including the sign bit.  The magnitude (@p n - 1 bits,
    ///        at most 64) is read with @c u64.  For @p n < 2 the error flag is set, nothing is
    ///        consumed and 0 is returned.
    /// @return +magnitude, or -magnitude when the sign bit is set; 0 on failure.
    int64_t sm64(int n) {
        if (n < 2) { error_ = true; return 0; }
        const uint64_t sign = u64(1);
        const uint64_t mag  = u64(n - 1);
        const int64_t v = static_cast<int64_t>(mag);
        return sign ? -v : v;
    }

    /// @brief Read 1 bit as a boolean.
    /// @return true when the bit is 1; false when it is 0 or the read fails (see @c ok).
    bool boolean() { return u(1) != 0; }

    /// @brief Advance past @p n bits without producing a value.
    /// @param n Number of bits to skip, for example 12 for the DF002 message number at the start
    ///        of a payload.  Zero or negative is a no-op.  Skipping past the end of the buffer
    ///        sets the error flag and leaves the position at the end.
    void skip(int n) {
        if (n <= 0) return;
        bit_offset_ += static_cast<std::size_t>(n);
        if (bit_offset_ > len_bits_) { error_ = true; bit_offset_ = len_bits_; }
    }

    /// @brief Number of bits consumed so far.
    /// @return Bit offset from the start of the buffer, counting skipped bits; at most the buffer
    ///         length in bits.  A decoder compares it with the payload length to detect a decode
    ///         that under-consumed the message.
    std::size_t pos() const { return bit_offset_; }

    /// @brief Number of bits remaining in the buffer.
    /// @return Buffer length in bits minus @c pos(); 0 once the end has been reached.
    std::size_t remaining() const { return (bit_offset_ < len_bits_) ? (len_bits_ - bit_offset_) : 0; }

    /// @brief True iff every read has succeeded (no over-read, no out-of-range N).
    /// @return false once any read or skip has overrun the buffer or requested an invalid
    ///         width; the flag is sticky and never clears.
    bool ok() const { return !error_; }

private:
    bool canRead(int n) const {
        return bit_offset_ + static_cast<std::size_t>(n) <= len_bits_;
    }

    const uint8_t* data_;
    std::size_t    len_bits_;
    std::size_t    bit_offset_{0};
    bool           error_{false};
};

} // namespace GnssProcessor
