#pragma once
/**
 * @file RinexNavParser.hpp
 * @brief RINEX 3/4 navigation-message parser producing @c BroadcastEphemeris.
 *
 * The post-processing / replay analogue of the RTCM broadcast-nav decoders
 * (@c RtcmNav::decodeBroadcast, RTCM 1019/1042/1044/1046): it reads a RINEX
 * navigation file and emits the SAME @c BroadcastEphemeris records the live
 * stream produces, so a run can feed @c LiveEphemerisStore from a file exactly
 * as the real-time engine feeds it from the wire.  This is the "frame
 * consistency from the shared code path" requirement of the brdc_rts network
 * producer (see @c docs/rt_service_conventions.md §1, @c docs/rtf12_brdc_rts_plan.md
 * Card 2): the producer and every consumer resolve satellite state through the
 * same store, so replaying with a RINEX-nav-populated store is bit-consistent
 * with a live RTCM-nav-populated store.
 *
 * Scope (Card 2): the Keplerian constellations on their LEGACY navigation
 * messages — GPS LNAV, Galileo I/NAV (and F/NAV, same record layout), BeiDou
 * D1/D2, QZSS LNAV.  GLONASS and SBAS records are skipped cleanly (GLONASS
 * eph-anchoring is deferred per @c docs/glonass_enablement_plan.md / plan D8);
 * RINEX-4 CNAV/CNV1/CNV2 records (different orbit-line layout) are skipped.
 *
 * Units: RINEX navigation files already store angles in radians and rates in
 * rad/s (unlike RTCM, which broadcasts semicircles), so the Keplerian element
 * copy is direct — no scale factors.  Time convention MATCHES the RTCM
 * decoders exactly: GPS/QZSS @c toe/@c toc on the GPS week/TOW scale, Galileo
 * on the GPS-aligned week the RINEX file already carries, and BeiDou @c toe/
 * @c toc on the NATIVE BDT week numbering (the whole-week GPS↔BDT epoch offset
 * cancels under the @c normalizeTk half-week wrap in
 * @c LiveEphemerisStore::computeSatellitePV; the 14 s is handled there by the
 * BDT→GPS shift).
 *
 * Nothing in the shipped pipeline calls this yet — the brdc_rts producer core
 * (plan Card 5) is the first caller.  This translation unit is pure parse +
 * tests and does not alter any existing behaviour.
 */

#include "products/BroadcastEphemerisTypes.hpp"

#include <string>
#include <vector>

namespace GnssProcessor {

class LiveEphemerisStore;  // forward decl (loadIntoLiveStore takes a reference)

/**
 * @class RinexNavParser
 * @brief Stateless RINEX 3/4 navigation-message → @c BroadcastEphemeris parser.
 *
 * All entry points are static: the parser holds no state.  Parse failures on a
 * single record are logged and skipped (the remaining records still parse);
 * an unopenable file or an unsupported RINEX version yields an empty vector
 * with a warning.
 */
class RinexNavParser {
public:
    /**
     * @brief Parse a RINEX 3/4 navigation file into broadcast-ephemeris records.
     * @param path Filesystem path to an uncompressed @c .rnx navigation file
     *             (Hatanaka/gzip decompression is an upstream concern).
     * @return Every decoded Keplerian record (GPS/Galileo/BeiDou/QZSS), one per
     *         satellite-epoch, in file order.  GLONASS/SBAS and non-legacy
     *         (CNAV) records are omitted.  Empty on open failure or an
     *         unsupported RINEX version (< 3), with a logged warning.
     */
    static std::vector<BroadcastEphemeris> parse(const std::string& path);

    /**
     * @brief Parse RINEX navigation content already held in memory.
     *
     * Identical semantics to @ref parse but reads from a string — the unit-test
     * entry point (fixture strings) and any in-memory caller.
     * @param content Full RINEX navigation file content (header + records).
     * @return Decoded Keplerian records (see @ref parse).
     */
    static std::vector<BroadcastEphemeris> parseString(const std::string& content);

    /**
     * @brief Parse a RINEX navigation file and insert every record into @p store.
     *
     * Each record is pushed via @c LiveEphemerisStore::insert with
     * @c from_igs = true — the store's freshest-toe / current-previous
     * retention then keeps the operative ephemeris per satellite exactly as the
     * live path does.  @c from_igs is true because a RINEX BRDC file is the IGS
     * broadcast product the IGS-SSR delta is referenced against (single-source
     * callers all pass true; the receiver-vs-IGS source-priority gate is a
     * live-stream concern).
     * @param store Destination ephemeris store (e.g. @c LiveEphemerisStore::getInstance()).
     * @param path  RINEX 3/4 navigation file path.
     * @return Number of records inserted (== the size of @ref parse's result).
     */
    static std::size_t loadIntoLiveStore(LiveEphemerisStore& store, const std::string& path);
};

} // namespace GnssProcessor
