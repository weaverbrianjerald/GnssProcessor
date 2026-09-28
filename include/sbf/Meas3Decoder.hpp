#pragma once
/**
 * @file Meas3Decoder.hpp
 * @brief Stateful decoder for the Septentrio Meas3Ranges (SBF 4109) block.
 *
 * Meas3Ranges is the compact replacement for MeasEpoch (4027): same 1 mm / 0.001
 * cycle / 1 dB-Hz resolution at roughly a quarter of the byte size.  The size
 * reduction comes from a differential encoding: full @em reference epochs are
 * emitted only every N seconds (the interval is carried in the block and is
 * controlled on the receiver by @c setMeas3MaxRefInterval), and the epochs in
 * between are @em delta epochs that encode only the change since the applicable
 * reference epoch.  A delta epoch cannot be decoded until its reference epoch
 * has been seen and cached, so this decoder owns persistent cross-block state,
 * unlike the otherwise stateless @c SbfParser.
 *
 * The public SBF Reference Guide does not document the Meas3 wire format; it
 * refers implementers to the proprietary @c sbfread_meas.c shipped in RxTools.
 * This implementation is a clean-room port of the independent open-source
 * decoder in the rtklibexplorer/RTKLIB @c demo5 fork (@c src/rcv/septentrio.c,
 * @c decode_meas3ranges, added 2024), adapted to emit the engine's
 * @c RinexObservation type.  It was validated against the 4027 MeasEpoch blocks
 * present for the same epochs in a live mosaic-X5 capture: pseudorange agreement
 * < 1 mm, carrier phase < 0.002 cycles, identical observation sets.
 *
 * Only the @b code and @b phase observables are reconstructed here.  The
 * Meas3Ranges companions (Meas3CN0HiRes 4110 with fractional C/N0, Meas3Doppler
 * 4111, and Meas3MP/Meas3PP 4112/4113) are not decoded; the coarse 1 dB-Hz
 * C/N0 carried in 4109 is emitted as the @c S observable.
 *
 * SBF is little-endian; this decoder assumes a little-endian host (x64/MSVC),
 * matching the rest of @c SbfParser.
 */

#include "rinex/RinexParser.hpp"   // RinexObservation
#include <cstdint>
#include <cstddef>
#include <vector>

namespace GnssProcessor {

/**
 * @class Meas3Decoder
 * @brief Persistent reference-epoch state machine for Meas3Ranges (4109).
 *
 * A single decoder instance must see the SBF blocks in stream order so that
 * delta epochs resolve against the most recent reference epoch.  Because the
 * realtime path constructs a fresh @c SbfParser per frame (and @c SbfParser is
 * the natural caller), the cross-frame reference state is held here behind a
 * process-wide @c instance() accessor, the same pattern used for the other
 * cross-frame SBF/RTCM decode state (@c LiveEphemerisStore,
 * @c SsrCorrectionStore).  @c reset() drops all state (used by tests).
 */
class Meas3Decoder {
public:
    /// Process-wide decoder holding the live reference-epoch cache.
    static Meas3Decoder& instance();

    /**
     * @brief Decode one complete Meas3Ranges (4109) block.
     * @param block Pointer to the full SBF block (Sync..body), as handed to
     *              @c SbfParser::parseFrame.
     * @param len   Block length in bytes.
     * @return One @c RinexObservation per satellite carrying the decoded C/L/S
     *         observables for this epoch.  Empty when the block is an empty /
     *         auxiliary-antenna set, when it is a delta epoch whose reference
     *         epoch has not yet been seen, or when the block is malformed.
     *         Reference-epoch blocks additionally refresh the internal cache as
     *         a side effect.
     */
    std::vector<RinexObservation> decode(const uint8_t* block, std::size_t len);

    /// Drop all cached reference-epoch state (no reference seen yet).
    void reset();

private:
    Meas3Decoder() { reset(); }

    static constexpr int kSys = 7;    ///< GPS,GLO,GAL,BDS,SBAS,QZS,IRN
    static constexpr int kSat = 64;   ///< satellites per constellation
    static constexpr int kSig = 16;   ///< signals per satellite (compacted index)

    /// Cached observables of one satellite at the last reference epoch, indexed
    /// by the Meas3 compacted signal index (0..15).
    struct SatRef {
        std::uint8_t  master_sig{0};     ///< signal index of the master observable
        std::uint32_t slave_mask{0};     ///< bitmask of slave signal indices
        std::int16_t  pr_rate{0};        ///< pseudorange rate, 64 mm/s units
        double        P[kSig]{};         ///< reference pseudorange [m]
        double        L[kSig]{};         ///< reference carrier phase [cycles]
        double        S[kSig]{};         ///< reference C/N0 [dB-Hz]
        std::uint8_t  lli[kSig]{};       ///< reference loss-of-lock flags
    };

    std::uint32_t ref_tow_ms_{0};                 ///< TOW of the cached reference epoch
    SatRef        ref_[kSys][kSat];               ///< per-sat reference observables
    std::uint8_t  con_hdr_[kSys][32]{};           ///< cached per-constellation header bytes
    std::uint8_t  con_hdr_len_[kSys]{};           ///< length of each cached header
    std::uint32_t lock_run_[kSys][kSat][kSig]{};  ///< running PLL lock time [ms] (slip detection)
    std::uint32_t lock_ref_[kSys][kSat][kSig]{};  ///< reference snapshot of the lock time
};

} // namespace GnssProcessor
