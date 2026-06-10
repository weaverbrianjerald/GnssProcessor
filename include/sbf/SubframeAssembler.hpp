#pragma once
/**
 * @file SubframeAssembler.hpp
 * @brief Assembles broadcast ephemeris from raw SBF navigation subframes.
 *
 * The receiver's DECODED ephemeris blocks (GPSNav 5891, …) are emitted only on
 * ephemeris change (too sparse for a cold start), but the RawNavBits blocks
 * stream the raw subframes/pages continuously.  This class buffers those raw
 * messages per satellite ACROSS SBF blocks and emits one @c BroadcastEphemeris
 * when a satellite's set is complete and self-consistent:
 *   - GPS / QZSS : LNAV subframes 1+2+3, IODE(SF2)==IODE(SF3)==IODC&0xFF
 *   - Galileo    : I/NAV word types 1+2+3+4 with a common IODnav
 *   - BeiDou D1  : subframes 1+2+3 with SOW spacing +6/+6 and toe==toc
 *   - GLONASS    : not yet assembled here (excluded from active_systems)
 *
 * The emitted struct uses the SAME conventions as the decoded-Nav decoders
 * (angles in radians, BeiDou toe on native BDT), so @c LiveEphemerisStore
 * consumes it unchanged.  It is held persistently (one per streaming session)
 * because @c SbfParser is reconstructed per block.
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
     * @return A freshly-completed ephemeris when this message completes a
     *         consistent set whose IODE differs from the last one emitted for
     *         that satellite; otherwise @c std::nullopt.
     */
    std::optional<BroadcastEphemeris> consume(const RawNavMessage& msg);

    /// Drop all buffered subframes.
    void clear() { by_sat_.clear(); }

private:
    struct PerSat {
        std::map<int, std::vector<uint8_t>> slots;   ///< subframe-id / word-type / string-num -> packed bits.
        int last_emitted_iode{-100000};              ///< De-dup: skip re-emitting the same IODE.
    };

    std::optional<BroadcastEphemeris> consumeGps(const RawNavMessage& msg, PerSat& sat);
    std::optional<BroadcastEphemeris> consumeGal(const RawNavMessage& msg, PerSat& sat);
    std::optional<BroadcastEphemeris> consumeBds(const RawNavMessage& msg, PerSat& sat);

    std::unordered_map<std::string, PerSat> by_sat_;
};

} // namespace GnssProcessor
