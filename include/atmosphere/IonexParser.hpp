#pragma once

/**
 * @file IonexParser.hpp
 * @brief Parser for IONEX (IONosphere map EXchange) global ionosphere map files.
 *
 * Reads an IONEX file (e.g. a CODE final GIM,
 * @c COD0OPSFIN_<YYYYDOY>0000_01D_01H_GIM.INX) into an IonexData structure: the
 * grid definition plus a time-ordered series of VTEC maps and, when present,
 * their RMS companion maps.  The parser performs no interpolation — see
 * GlobalIonosphereMap for slant-delay queries.
 */

#include "logging/LogManager.hpp"
#include "math/TimeUtils.hpp"
#include <string>
#include <vector>

namespace GnssProcessor {

    /**
     * @brief A single global VTEC map (with optional RMS companion) at one epoch.
     *
     * Grid values are stored row-major: @c index = lat_index * n_lon + lon_index,
     * with @c lat_index running from IonexData::lat1_deg and @c lon_index from
     * IonexData::lon1_deg.  Values are in TECU.  A missing grid node is stored as
     * NaN (the IONEX 9999 sentinel).
     */
    struct IonexMap {
        GnssTime epoch{};            ///< Map epoch (converted to the GPS time scale).
        std::vector<double> vtec;    ///< VTEC per grid node (TECU); NaN = missing.
        std::vector<double> rms;     ///< VTEC RMS per grid node (TECU); empty if the file has no RMS maps.
    };

    /**
     * @brief Parsed contents of an IONEX file: grid definition and map series.
     */
    struct IonexData {
        bool   valid = false;            ///< True once a complete, usable file has been parsed.

        double lat1_deg = 0.0;           ///< First grid latitude (deg).
        double lat2_deg = 0.0;           ///< Last grid latitude (deg).
        double dlat_deg = 0.0;           ///< Latitude step (deg; typically negative).
        double lon1_deg = 0.0;           ///< First grid longitude (deg).
        double lon2_deg = 0.0;           ///< Last grid longitude (deg).
        double dlon_deg = 0.0;           ///< Longitude step (deg).
        int    n_lat = 0;                ///< Number of latitude rows.
        int    n_lon = 0;                ///< Number of longitude columns.

        double height_km = 450.0;        ///< Single-layer shell height (IONEX HGT1).
        double base_radius_km = 6371.0;  ///< IONEX BASE RADIUS.
        int    exponent = -1;            ///< Stored integers scale to TECU by 10^exponent.

        std::vector<IonexMap> maps;      ///< VTEC/RMS maps, ascending in epoch.
    };

    /**
     * @class IonexParser
     * @brief Stateless IONEX file reader.
     */
    class IonexParser {
    public:
        /**
         * @brief Parse an IONEX file into an IonexData structure.
         *
         * @param path        Filesystem path to a decompressed IONEX (.INX) file.
         * @param log_manager Log manager for diagnostics.
         * @return IonexData; @c valid is false if the file is missing or malformed.
         */
        static IonexData parse(const std::string& path, LogManager& log_manager);
    };

} // namespace GnssProcessor
