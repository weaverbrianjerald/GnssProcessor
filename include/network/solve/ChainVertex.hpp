/**
 * @file ChainVertex.hpp
 * @brief The chain's vertex identity: a receiver (by its roster index) or a satellite, and its one key.
 *
 * The chain's record (network/solve/ChainFrame.hpp), the observatory's traces and memory, and the ring fix's rings and
 * cuts (network/ar/RingFix.hpp) name a vertex of the chain's tree by a key: "R:<roster index>" for a receiver,
 * "S:<satellite>" for a satellite.  This header owns that form: a key is built by
 * @ref GnssProcessor::chainVertexKey "chainVertexKey" and read back by @ref GnssProcessor::chainVertexOfKey "chainVertexOfKey"
 * (or tested by @ref GnssProcessor::chainVertexIsStation "chainVertexIsStation"); no other code spells it.
 */
#pragma once

#include <cstdlib>
#include <optional>
#include <string>

namespace GnssProcessor {

/// A vertex of the chain's tree, as read back from its key.
struct ChainVertex {
    bool        is_sat{false};  ///< A satellite (else a receiver).
    int         station{-1};    ///< The receiver's roster index (a receiver vertex).
    std::string sat;            ///< The satellite as the record writes it (a satellite vertex).
};

/**
 * @brief The key of a receiver vertex.
 * @param station The receiver's roster index.
 * @return "R:<roster index>".
 */
inline std::string chainVertexKey(int station) { return "R:" + std::to_string(station); }

/**
 * @brief The key of a satellite vertex.
 * @param sat The satellite as the record writes it ("G05").
 * @return "S:<satellite>".
 */
inline std::string chainVertexKey(const std::string& sat) { return "S:" + sat; }

/**
 * @brief Whether a vertex key names a receiver.
 * @param key A vertex key.
 * @return True for a key that begins with the receiver's letter.
 */
inline bool chainVertexIsStation(const std::string& key) { return !key.empty() && key[0] == 'R'; }

/**
 * @brief The vertex a key names.
 * @param key A vertex key ("R:<roster index>" or "S:<satellite>").
 * @return The vertex; none for a string that is no vertex key (a receiver key needs an index after its colon).
 */
inline std::optional<ChainVertex> chainVertexOfKey(const std::string& key) {
    if (key.size() < 2 || key[1] != ':') return std::nullopt;
    ChainVertex v;
    if (key[0] == 'R') {
        if (key.size() <= 2) return std::nullopt;
        v.station = std::atoi(key.c_str() + 2);
        return v;
    }
    if (key[0] == 'S') {
        v.is_sat = true;
        v.sat = key.substr(2);
        return v;
    }
    return std::nullopt;
}

}  // namespace GnssProcessor
