#pragma once

/**
 * @file Coordinates.hpp
 * @brief ECEF/LLH/ENU coordinate transformations and helpers.
 *
 * Provides a lightweight, header-only friendly API for common geodetic
 * conversions used across the processor: ECEF <-> geodetic LLH and
 * ECEF <-> ENU (local topocentric) transforms. Defaults use WGS84
 * ellipsoid parameters from `GnssProcessor::Constants::Ellipsoid::WGS84`.
 *
 * All angular values are radians, lengths in meters.
 */

#include "common/Constants.hpp"
#include <Eigen/Dense>

    namespace GnssProcessor {

    namespace Coordinates {

        /**
         * @brief Geodetic lat/lon/height container (radians, radians, meters).
         */
        struct Llh {
            double lat_rad = 0.0;   ///< Geodetic latitude in radians
            double lon_rad = 0.0;   ///< Geodetic longitude in radians
            double h_m = 0.0;       ///< Height above ellipsoid in meters
        };

        /**
         * @brief Convert ECEF (X,Y,Z) to geodetic latitude, longitude and height
         *        using a Bowring-style iterative method.
         *
         * Defaults to WGS84 ellipsoid parameters.
         *
         * @param ecef ECEF position vector (meters).
         * @param a Semi-major axis (meters). Default: WGS84::a.
         * @param f Flattening. Default: WGS84::f.
         * @return Llh struct (lat_rad, lon_rad, h_m).
         */
        Llh ecefToLlh(const Eigen::Vector3d& ecef,
            double a = Constants::Ellipsoid::WGS84::a,
            double f = Constants::Ellipsoid::WGS84::f);

        /**
         * @brief Convert geodetic coordinates (lat, lon, h) to ECEF (X,Y,Z).
         *
         * @param lat_rad Geodetic latitude (rad).
         * @param lon_rad Geodetic longitude (rad).
         * @param h_m Height above ellipsoid (m).
         * @param a Semi-major axis (m). Default: WGS84::a.
         * @param f Flattening. Default: WGS84::f.
         * @return ECEF vector (meters).
         */
        Eigen::Vector3d llhToEcef(double lat_rad, double lon_rad, double h_m,
            double a = Constants::Ellipsoid::WGS84::a,
            double f = Constants::Ellipsoid::WGS84::f);

        /**
         * @brief Build the ENU -> ECEF rotation matrix for a given geodetic
         *        latitude/longitude (columns are E,N,U unit vectors in ECEF).
         *
         * @param lat_rad Geodetic latitude (rad).
         * @param lon_rad Geodetic longitude (rad).
         * @return 3x3 rotation matrix that maps ENU -> ECEF.
         */
        Eigen::Matrix3d enuToEcefMatrix(double lat_rad, double lon_rad);

        /**
         * @brief Convert an ECEF vector (sat_pos - ref_pos) to local ENU coordinates
         *        at a reference location given by geodetic LLH.
         *
         * @param vec_ecef ECEF vector to convert (meters). Typically sat - ref.
         * @param ref_llh Geodetic reference location.
         * @return ENU vector (meters).
         */
        Eigen::Vector3d ecefToEnu(const Eigen::Vector3d& vec_ecef, const Llh& ref_llh);

        /**
         * @brief Convert an ENU vector at a reference location to ECEF.
         *
         * @param enu ENU vector (meters).
         * @param ref_llh Geodetic reference location.
         * @return ECEF vector (meters).
         */
        Eigen::Vector3d enuToEcef(const Eigen::Vector3d& enu, const Llh& ref_llh);

        /**
         * @brief Raise a marker (monument) ECEF coordinate to the antenna
         *        reference point using a RINEX antenna eccentricity.
         *
         * This function is the single definition of the `ANTENNA: DELTA H/E/N`
         * convention.  RINEX orders the eccentricity (H, E, N) = (Up, East, North),
         * which differs from the ENU order the rotation expects, and defines it as
         * the offset from the marker to the antenna reference point (ARP) in the
         * local topocentric frame at the marker.
         *
         * Published station coordinates (IGS SINEX/SSC, NGS SIF, a surveyed RINEX
         * `APPROX POSITION XYZ`) refer to the monument, whereas the estimator's
         * position state is the ARP (`igs20.atx` PCO/PCV are defined relative to
         * the ARP).  Mixing the two puts a constant bias equal to the eccentricity
         * into the position (into the vertical, for a height-only eccentricity);
         * 184 of the 305 stations in the bundled 2024-129 fixture set carry a
         * nonzero eccentricity (up to 6.2 m).
         * An eccentricity with norm ≤ 1e-6 m, or a non-finite marker position,
         * returns the input unchanged, so header-less streams and 0/0/0 stations
         * are bit-for-bit unaffected.
         *
         * @param marker_ecef Monument/marker ECEF position (meters).
         * @param delta_hen   RINEX `ANTENNA: DELTA H/E/N` eccentricity (meters),
         *                    in RINEX (H, E, N) order.
         * @return ARP ECEF position (meters).
         * @see arpToMarker for the inverse reduction.
         */
        Eigen::Vector3d markerToArp(const Eigen::Vector3d& marker_ecef,
                                    const Eigen::Vector3d& delta_hen);

        /**
         * @brief Reduce an antenna-reference-point ECEF coordinate back to the marker.
         *
         * Inverse of @c markerToArp, for comparing an ARP-referenced estimate with,
         * or publishing it in, the monument frame.  The local frame is defined at
         * the marker, which is the unknown, so the implementation takes one
         * fixed-point step; a single rotation at the ARP would leave an
         * O(|ecc|²/R) closure error from the horizontal components (zero for a
         * height-only eccentricity, about 8 µm for a 6.2 m horizontal offset).
         * The round trip with @c markerToArp closes below a picometre.  The same
         * no-op threshold applies as in @c markerToArp (norm ≤ 1e-6 m, or a
         * non-finite input).
         *
         * @param arp_ecef  Antenna reference point ECEF position (meters).
         * @param delta_hen RINEX `ANTENNA: DELTA H/E/N` eccentricity (meters), in
         *                  RINEX (H, E, N) order.
         * @return Marker/monument ECEF position (meters).
         */
        Eigen::Vector3d arpToMarker(const Eigen::Vector3d& arp_ecef,
                                    const Eigen::Vector3d& delta_hen);

    } // namespace Coordinates

} // namespace GnssProcessor