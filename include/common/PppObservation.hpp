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
#include <set>
#include <string>

namespace GnssProcessor {

    /**
     * @brief Observation information for a single satellite at an epoch.
     *
     * Contains raw and modeled observables, variances, satellite geometry,
     * and various auxiliary quantities used by PPP.
     *
     * Per-observation residuals are not cached on this struct; compute them
     * inline as @c (observed[code] - modeled[code]) at the call site.  The
     * @c residuals vector on @c EpochMeasurement is unrelated and remains the
     * authoritative post-fit residual store after a Kalman update.
     */
    struct PppObservation {
        std::string satellite;                       /**< Satellite identifier (e.g. "G01") */
        System system;                               /**< Constellation/System for the satellite */
        GnssTime time;                               /**< Epoch time of the observation */
        std::map<std::string, double> observed;      /**< Raw observed measurement values keyed by full RINEX-3 code (e.g. "C1C", "L2W") */
        std::map<std::string, double> raw_observed;  /**< All RINEX code and phase observables (L1C, C1W, L5Q, etc.) for cycle-slip detection; always holds every available signal regardless of signal_mode */
        std::map<std::string, double> modeled;       /**< Modeled values for the same observables */
        std::map<std::string, double> variances;     /**< Variance (or sigma^2) for each observable: stochastic-model output with elevation amplification applied */
        std::map<std::string, double> zenith_variances; /**< Variance (or sigma^2) at zenith for each observable: stochastic-model output evaluated at elevation 90° (sin²=1, no elevation amplification), retaining block/band/attribute/SNR scaling.  Filled by PppModel; no code in the current tree reads it. */
        std::map<std::string, int> lli_flags;        /**< Loss-of-lock indicators keyed by observable name */
        Eigen::Vector3d sat_pos;                     /**< Satellite ECEF position (meters) */
        Eigen::Vector3d sat_vel = Eigen::Vector3d::Zero(); /**< Satellite ECEF velocity (meters/second) */
        Eigen::Vector3d rx_pos_linearization;        /**< Receiver ECEF position used as the linearization point (meters) */
        double elevation;                            /**< Elevation angle (degrees) */
        double azimuth;                              /**< Azimuth angle (degrees) */
        Eigen::Vector3d los;                         /**< Line-of-sight unit vector from receiver to satellite */
        double range;                                /**< Geometric range (meters) */
        double mf_w;                                 /**< Wet tropospheric mapping function (dimensionless); slant wet delay = mf_w × ZWD */
        double mf_d;                                 /**< Dry (hydrostatic) tropospheric mapping function (dimensionless); slant dry delay = ZHD × mf_d */
        bool broadcast_only = false;                 /**< True when the range model is broadcast-grade (metre level): phase observables have been dropped and only code is used for this satellite. */
        double eph_toe_age_s = 0.0;                   /**< Signed age (s, obs epoch − ephemeris toe) of the broadcast ephemeris used for this satellite; |value| is the extrapolation magnitude. Real-time BSSD pivot selection avoids a pivot whose ephemeris is much staler than its peers'. 0 when there is no live ephemeris (post-processing / SP3). */
        /**
         * @brief The band-1 code keys this observation carries as companions of the
         *        station's latched canonical code.
         *
         * Empty unless `network.second_code_channel` is armed on the network path, so
         * every other posture allocates nothing and is unchanged.  The keys are also
         * in @ref observed / @ref modeled / @ref variances (the measurement model needs
         * all three to accept a row); this set is what distinguishes a companion row
         * from the canonical one it accompanies.  Two consumers need that:
         *
         *  - the Melbourne-Wubbena detector (R4) keeps choosing the band's code as it
         *    does with the knob off: a companion is not KF-chosen for its purposes.
         *    Otherwise an armed run would move the detector's code (for example from
         *    C1C to C1W) and every slip event with it;
         *  - the run-end telemetry (R7) counts companion cells and their two edges,
         *    which it cannot derive from the row set alone (a fallback code is also a
         *    non-canonical code and is intentionally not in this set).
         *
         * @ref raw_observed is unrelated and still holds every raw signal.
         */
        std::set<std::string> companion_codes;
        /**
         * @brief The band-1 phase keys this observation carries besides the one the
         *        station's canonical latch selected (card SID phase 6a, R1).
         *
         * Empty unless `network.second_phase_channel` is armed on the network path, so
         * every other posture allocates nothing and is unchanged.  The keys are also in
         * @ref observed / @ref modeled / @ref variances (the measurement model needs
         * all three to accept a row); this set is what distinguishes a second-phase
         * row from the latched one it accompanies.
         *
         * Companion code rows can also be identified downstream by the
         * receiver inter-signal state they route through.  Second-phase rows have no
         * such state (a float ambiguity already absorbs a constant per arc, so the
         * state would sit in the null space), so this set is the only key: the sigma
         * scale (R5) and the cell telemetry (R7) both read it.
         *
         * @ref raw_observed is unrelated and still holds every raw signal.
         */
        std::set<std::string> second_phases;
    };

} // namespace GnssProcessor
