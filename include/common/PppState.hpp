#pragma once

/**
 * @file PppState.hpp
 * @brief PPP state-vector container (receiver position / clock / atmosphere).
 *
 * Plain-data view of the filter state used at @c PppModel call sites
 * for geometry computation and at the @c MeasurementModel sync points.
 * The authoritative state lives in @c StateManager (which owns the
 * underlying Eigen vector + covariance + dynamic-state index maps);
 * @c PppState is a snapshot with named fields, in place of reading
 * individual indices on the StateManager.
 */

#include "math/TimeUtils.hpp"
#include <Eigen/Dense>
#include <map>
#include <string>

namespace GnssProcessor {

    /**
     * @brief State vector container used by PPP filters.
     *
     * Holds receiver position, clock and atmospheric states and their covariance.
     */
    struct PppState {
        Eigen::Vector3d position;                    /**< Receiver ECEF position (meters) */
        Eigen::Vector3d velocity = Eigen::Vector3d::Zero(); /**< Receiver ECEF velocity (meters/second) */
        double clock_bias;                           /**< Receiver clock bias (meters) */
        double troposphere_zwd;                      /**< Zenith wet delay (meters) */
        double tropo_gradient_north{ 0.0 };          /**< Tropospheric north gradient (meters) */
        double tropo_gradient_east{ 0.0 };           /**< Tropospheric east gradient (meters) */
        double phase_clock_offset{ 0.0 };            /**< Receiver phase clock offset from code clock (meters) */
        double ionosphere_zenith_delay;              /**< Ionospheric zenith delay (meters), if modeled as a single parameter */
        std::map<std::string, double> slant_ionosphere; /**< Per-satellite slant ionosphere delays (meters), keyed by satellite ID (e.g. "G01") */
        std::map<std::string, double> slant_ionosphere_rate; /**< Per-satellite slant-iono rate Idot_s (meters/second), keyed by satellite ID; populated only under the constant-velocity iono model (enable_iono_rate) */
        std::map<std::string, double> system_time_offsets; /**< Inter-system time biases (meters) keyed by system name */
        Eigen::MatrixXd covariance;                  /**< State covariance matrix */
        std::map<std::string, double> ambiguities;   /**< Carrier-phase ambiguity estimates keyed by satellite-signal */
        GnssTime epoch_time;                         /**< Epoch associated with this state */
    };

} // namespace GnssProcessor
