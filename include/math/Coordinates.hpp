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

    } // namespace Coordinates

} // namespace GnssProcessor