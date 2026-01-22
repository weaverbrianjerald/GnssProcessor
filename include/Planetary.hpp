/**
 * @file Planetary.hpp
 * @brief Planetary utilities: Sun & Moon positions and ephemeris handling.
 *
 * Provides lightweight routines to compute celestial body positions (Sun, Moon)
 * and to manage Sun ephemeris used by the GNSS processor. The class stores
 * an in-memory Sun ephemeris table and exposes interpolation and parsing
 * helpers.
 */

#ifndef PLANETARY_HPP
#define PLANETARY_HPP

#include "LogManager.hpp"
#include <Eigen/Dense>
#include "TimeUtils.hpp"
#include "PppTypes.hpp"
//#include "ProductParser.hpp"

namespace GnssProcessor {

    /**
     * @class Planetary
     * @brief Compute and manage planetary ephemerides used by GNSS models.
     *
     * The `Planetary` class offers simple routines to compute approximate Sun
     * and Moon positions for a given epoch, rotate vectors about body axes,
     * interpolate Sun positions from a stored ephemeris, and parse Sun
     * ephemeris for a set of observation epochs.
     *
     * All positions are represented as ECEF-like Cartesian vectors in meters
     * unless otherwise noted.
     */
    class Planetary {
    public:
        /**
         * @brief Construct a Planetary helper.
         * @param log_mgr Reference to the LogManager used for logging.
         */
        explicit Planetary(LogManager& log_mgr);

        /**
         * @brief Compute Sun position for a given epoch.
         *
         * Computes an approximate Sun position vector for the specified epoch.
         * The epoch is provided as a Modified Julian Date (MJD).
         *
         * @param mjd Modified Julian Date (days).
         * @return Sun position vector (meters).
         */
        Eigen::Vector3d computeSunPosition(double mjd) const;

        /**
         * @brief Compute Moon position for a given epoch.
         *
         * Computes an approximate Moon position vector for the specified epoch.
         * The epoch is provided as a Modified Julian Date (MJD).
         *
         * @param mjd Modified Julian Date (days).
         * @return Moon position vector (meters).
         */
        Eigen::Vector3d computeMoonPosition(double mjd) const;

        /**
         * @brief Rotate a vector about a principal axis.
         *
         * Rotates `vec` by `angle` radians about the specified axis.
         *
         * @param vec Input vector to rotate.
         * @param angle Rotation angle in radians.
         * @param axis Axis index: 0 = X, 1 = Y, 2 = Z. Other values are undefined.
         * @return Rotated vector.
         */
        Eigen::Vector3d rotateVector(const Eigen::Vector3d& vec, double angle, int axis) const;

        /**
         * @brief Interpolate Sun position from stored ephemeris.
         *
         * Uses the in-memory `sun_ephemeris_` table to interpolate the Sun
         * position for the provided GNSS epoch. The interpolation method is
         * implementation-defined (e.g. linear or spline) and should handle
         * times outside the ephemeris range gracefully (implementation may
         * clamp or return the nearest epoch position).
         *
         * @param time GNSS epoch at which to interpolate the Sun position.
         * @return Interpolated Sun position vector (meters).
         * @see GnssTime, SunEphemeris
         */
        Eigen::Vector3d interpolateSunPosition(const GnssTime& time) const;

        /**
         * @brief Build Sun ephemeris for observation epochs.
         *
         * Computes or parses Sun ephemeris entries for each epoch in
         * `epoch_times` and appends/fills `sun_ephemeris` with the resulting
         * entries. The output vector will contain one `SunEphemeris` per
         * requested epoch on success.
         *
         * @param epoch_times Input vector of GNSS epochs for which ephemeris is required.
         * @param[out] sun_ephemeris Output vector that will be filled with Sun ephemeris entries.
         * @return true if parsing/creation succeeded for all requested epochs; false otherwise.
         */
        bool parseSunEphemeris(const std::vector<GnssTime>& epoch_times, std::vector<SunEphemeris>& sun_ephemeris) const;

    private:
        LogManager& log_manager_;                         /**< Logger for debugging and info. */
        std::vector<SunEphemeris> sun_ephemeris_;         /**< In-memory Sun ephemeris table. */
    };

}

#endif // PLANETARY_HPP