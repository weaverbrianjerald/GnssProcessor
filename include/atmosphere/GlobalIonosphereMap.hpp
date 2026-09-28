#pragma once

/**
 * @file GlobalIonosphereMap.hpp
 * @brief Slant ionospheric delay queries from an IONEX global ionosphere map.
 */

#include "atmosphere/IonexParser.hpp"
#include "logging/LogManager.hpp"
#include "math/TimeUtils.hpp"
#include <Eigen/Dense>
#include <string>

namespace GnssProcessor {

    /**
     * @brief Result of a GIM slant-delay query.
     *
     * Besides @c delay_m / @c std_m, used for first-order ionosphere
     * weighting, the query exposes the underlying @c stec_tecu (absolute
     * slant TEC) and the ionospheric pierce point (@c ipp_lat_rad /
     * @c ipp_lon_rad / @c ipp_radius_m).  @c HigherOrderIonosphere uses
     * them to compute the B·cos(θ_B) and STEC² terms on the same geometry
     * as the first-order delay, so the higher-order correction inherits the
     * GIM's spatial and temporal interpolation.
     */
    struct GimQuery {
        double delay_m       = 0.0;   ///< Slant ionospheric delay at the requested frequency (m).
        double std_m         = 0.0;   ///< 1-sigma uncertainty of @c delay_m (m), from the IONEX RMS map (3 TECU where none is usable).
        double stec_tecu     = 0.0;   ///< Absolute slant TEC at the pierce point (TECU = 10^16 el/m²).
        double ipp_lat_rad   = 0.0;   ///< Latitude of the IPP on the IONEX shell (rad).  Copied
                                      ///< from @c IppGeometry::ipp_lat_rad; it is not plain
                                      ///< geodetic latitude (see the convention note there).
        double ipp_lon_rad   = 0.0;   ///< Longitude of the IPP on the IONEX shell (rad).
        double ipp_radius_m  = 0.0;   ///< IPP distance from geocentre (m) = IONEX base radius + shell height.
        bool   valid         = false; ///< False when the GIM has no usable value for this line of sight.
    };

    /**
     * @class GlobalIonosphereMap
     * @brief Wraps a parsed IONEX file and answers slant ionospheric delay queries.
     *
     * Construction parses an IONEX (.INX) file via IonexParser.  Each query
     * computes the ionospheric pierce point on the IONEX single-layer shell,
     * interpolates VTEC (and its RMS) bilinearly in latitude/longitude and with a
     * sun-fixed longitude rotation in time, applies the thin-shell mapping
     * function, and converts the result to a slant range delay at the caller's
     * carrier frequency.
     */
    class GlobalIonosphereMap {
    public:
        /**
         * @brief Construct from an IONEX file.
         * @param ionex_path  Path to a decompressed IONEX (.INX) file; may be empty.
         * @param log_manager Log manager for diagnostics.
         */
        GlobalIonosphereMap(const std::string& ionex_path, LogManager& log_manager);

        /**
         * @brief Whether a usable IONEX file was loaded.
         */
        bool isLoaded() const { return data_.valid && !data_.maps.empty(); }

        /**
         * @brief Read-only access to the parsed IONEX content.
         *
         * Exposed so callers can inspect the map-epoch span before querying.  A query
         * epoch outside the span is clamped to the endpoint map without any warning
         * (the longitude is still rotated), so a caller that must not extrapolate
         * checks the span itself.
         */
        const IonexData& data() const { return data_; }

        /**
         * @brief Compute the slant ionospheric delay for a satellite line of sight.
         *
         * @param time         Epoch (GPS time scale).
         * @param rx_pos_ecef  Receiver ECEF position (m).
         * @param sat_pos_ecef Satellite ECEF position (m).
         * @param frequency_hz Carrier frequency the delay is wanted at (Hz).
         * @return GimQuery; @c valid is false if the satellite is below the
         *         horizon or the map has no data at the pierce point.
         */
        GimQuery slantIonoDelay(const GnssTime& time,
                                const Eigen::Vector3d& rx_pos_ecef,
                                const Eigen::Vector3d& sat_pos_ecef,
                                double frequency_hz) const;

    private:
        /// Bilinear sample of one grid (NaN-tolerant); returns NaN if no node is valid.
        double sampleMap(const std::vector<double>& grid,
                         double lat_deg, double lon_deg) const;

        /// VTEC + RMS at a pierce point with sun-fixed temporal interpolation.
        bool interpolateVtec(const GnssTime& time, double ipp_lat_deg, double ipp_lon_deg,
                             double& vtec_tecu, double& vtec_rms_tecu) const;

        IonexData   data_;        ///< Parsed IONEX content.
        LogManager& log_manager_; ///< Log manager reference.
    };

} // namespace GnssProcessor
