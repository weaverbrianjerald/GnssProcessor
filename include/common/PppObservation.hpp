#pragma once

/**
 * @file PppObservation.hpp
 * @brief Per-satellite, per-epoch observation record consumed by the
 *        measurement model and downstream BRSD / BSSD layers.
 *
 * One @c PppObservation captures everything the processing pipeline
 * needs to know about a single satellite at a single epoch: raw
 * observables (keyed by full RINEX-3 code), modeled values produced
 * by @c PppModel, per-observable variances from the stochastic model,
 * loss-of-lock indicators, and the satellite-side geometry (position,
 * velocity, line-of-sight unit vector, range, elevation, azimuth).
 *
 * The map keys throughout are full RINEX-3 codes (e.g. "C1C", "L2W")
 * to match what's emitted by @c RinexParser and consumed by the
 * BRSD / BSSD differencing layers.
 */

#include "common/GnssTypes.hpp"
#include "math/TimeUtils.hpp"
#include <Eigen/Dense>
#include <map>
#include <string>

namespace GnssProcessor {

    /**
     * @brief Observation information for a single satellite at an epoch.
     *
     * Contains raw and modeled observables, variances, satellite geometry,
     * and various auxiliary quantities used by PPP.
     *
     * Per-observation residuals are NOT cached on this struct — compute
     * them inline as @c (observed[code] - modeled[code]) at the call
     * site.  An earlier @c residuals member existed but was never
     * populated by @c PppModel::computeModels() and was removed.  The
     * @c residuals vector on @c EpochMeasurement is unrelated and
     * remains the authoritative post-fit residual store after a Kalman
     * update.
     */
    struct PppObservation {
        std::string satellite;                       /**< Satellite identifier (e.g. "G01") */
        System system;                               /**< Constellation/System for the satellite */
        GnssTime time;                               /**< Epoch time of the observation */
        std::map<std::string, double> observed;      /**< Raw observed measurement values keyed by full RINEX-3 code (e.g. "C1C", "L2W") */
        std::map<std::string, double> raw_observed;  /**< All RINEX code+phase observables (L1C, C1W, L5Q, etc.) for cycle slip detection — always contains every available signal regardless of signal_mode */
        std::map<std::string, double> modeled;       /**< Modeled values for the same observables */
        std::map<std::string, double> variances;     /**< Variance (or sigma^2) for each observable — stochastic-model output with elevation amplification applied */
        std::map<std::string, double> zenith_variances; /**< Variance (or sigma^2) at zenith for each observable — stochastic-model output evaluated at elevation 90° (sin²=1, no elevation amplification), retaining block/band/attribute/SNR scaling.  Used by MW slip detector as a pair-aware noise floor that doesn't scale with line-of-sight geometry. */
        std::map<std::string, int> lli_flags;        /**< Loss-of-lock indicators keyed by observable name */
        Eigen::Vector3d sat_pos;                     /**< Satellite ECEF position (meters) */
        Eigen::Vector3d sat_vel = Eigen::Vector3d::Zero(); /**< Satellite ECEF velocity (meters/second) */
        Eigen::Vector3d rx_pos_linearization;        /**< Receiver ECEF position used as the linearization point (meters) */
        double elevation;                            /**< Elevation angle (degrees) */
        double azimuth;                              /**< Azimuth angle (degrees) */
        Eigen::Vector3d los;                         /**< Line-of-sight unit vector from receiver to satellite */
        double range;                                /**< Geometric range (meters) */
        double mf_w;                                 /**< Wide-lane combination frequency value (Hz) or coefficient */
        double mf_d;                                 /**< Narrow-lane / difference combination frequency value (Hz) or coefficient */
        bool broadcast_only = false;                 /**< True when the range model is broadcast-grade (metre level): phase observables have been dropped and only code is used for this satellite. */
        double eph_toe_age_s = 0.0;                   /**< Signed age (s, obs epoch − ephemeris toe) of the broadcast ephemeris used for this satellite; |value| is the extrapolation magnitude. Real-time BSSD pivot selection avoids seating a much-staler-than-peers satellite as the pivot. 0 when no live ephemeris (post-processing / SP3). */
    };

} // namespace GnssProcessor
