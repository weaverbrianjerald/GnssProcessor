#pragma once
/**
 * @file SubframeAssembler.hpp
 * @brief Assembles broadcast ephemeris from raw SBF navigation subframes.
 *
 * The receiver emits its decoded ephemeris blocks (GPSNav 5891, …) only when the
 * ephemeris changes, which is too sparse for a cold start, while the RawNavBits
 * blocks stream the raw subframes and pages continuously.  This class buffers the
 * raw messages per satellite across SBF blocks and emits one
 * @c BroadcastEphemeris when a satellite's set is complete and self-consistent:
 *   - GPS / QZSS : LNAV subframes 1+2+3, IODE(SF2)==IODE(SF3)==IODC&0xFF
 *   - Galileo    : I/NAV word types 1+2+3+4 with a common IODnav (word type 5,
 *                  when present, adds the BGDs and health)
 *   - BeiDou D1  : subframes 1+2+3 with toe==toc (within 1 s)
 *   - GLONASS    : not assembled here (excluded from active_systems)
 *
 * The emitted struct uses the conventions of the decoded-Nav decoders (angles in
 * radians, BeiDou toe on native BDT), so @c LiveEphemerisStore consumes it
 * unchanged.  One instance is held for the whole streaming session because
 * @c SbfParser is reconstructed per block.
 */

#include "sbf/SbfParser.hpp"                      // RawNavMessage
#include "products/BroadcastEphemerisTypes.hpp"
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace GnssProcessor {

class SubframeAssembler {
public:
    /**
     * @brief Ingest one raw navigation message.
     * @return The ephemeris when this message completes a consistent set whose
     *         issue of data (GPS IODE, Galileo IODnav, BeiDou AODE) differs from
     *         the last one emitted for that satellite; otherwise @c std::nullopt
     *         (also for messages that failed the CRC).
     */
    std::optional<BroadcastEphemeris> consume(const RawNavMessage& msg);

    /// Drop all buffered subframes.
    void clear() { by_sat_.clear(); }

private:
    struct PerSat {
        std::map<int, std::vector<uint8_t>> slots;   ///< subframe-id / word-type / string-num -> packed bits.
        int last_emitted_iode{-100000};              ///< Last emitted issue of data; a set with the same value is not emitted again.
    };

    std::optional<BroadcastEphemeris> consumeGps(const RawNavMessage& msg, PerSat& sat);
    std::optional<BroadcastEphemeris> consumeGal(const RawNavMessage& msg, PerSat& sat);
    std::optional<BroadcastEphemeris> consumeBds(const RawNavMessage& msg, PerSat& sat);

    std::unordered_map<std::string, PerSat> by_sat_;
};

} // namespace GnssProcessor
