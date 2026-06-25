#pragma once

/**
 * @file TrajectoryGenerator.hpp
 * @brief Analytic truth trajectory for the simulated SAR vessel.
 *
 * Produces a per-epoch truth state (ECEF position/velocity, receiver clock,
 * and diagnostics) from a smooth analytic ENU kinematic model: a speed ramp to
 * cruise with a small surge, a slowly weaving heading (a curving search track),
 * and sea-state heave.  The horizontal track is integrated on a 1 s sub-grid and
 * sampled at the epoch interval; the recorded ECEF positions ARE the truth
 * (self-consistent by construction).  Setting @c kinematic = false collapses the
 * model to a fixed point (the M2 static case).
 */

#include <cstddef>
#include <cstdint>
#include <vector>

#include <Eigen/Dense>

#include "math/TimeUtils.hpp" // GnssTime

namespace GnssProcessor {
namespace sim {

/** @brief One epoch of truth: ECEF state + clock + human-readable diagnostics. */
struct TruthState {
    GnssTime time;
    Eigen::Vector3d ecef{ Eigen::Vector3d::Zero() };     ///< Receiver ECEF position (m).
    Eigen::Vector3d vel_ecef{ Eigen::Vector3d::Zero() }; ///< Receiver ECEF velocity (m/s).
    double clock_bias_s{ 0.0 };                          ///< Receiver clock bias (s).
    double lat_deg{ 0.0 }, lon_deg{ 0.0 }, h_m{ 0.0 };   ///< Geodetic position.
    double east_m{ 0.0 }, north_m{ 0.0 }, up_m{ 0.0 };   ///< ENU offset from origin.
    double heading_deg{ 0.0 }, speed_mps{ 0.0 };         ///< Horizontal motion.
};

/** @brief Parameters for the analytic vessel trajectory. */
struct TrajectoryConfig {
    double lat0_deg{ -18.5 }, lon0_deg{ -178.2 }, h0_m{ 0.0 }; ///< Origin (near Fiji).
    GnssTime t0{ 2313, 302400.0 };                            ///< First epoch (GPS week/TOW).
    double interval_s{ 30.0 };                                ///< Epoch spacing.
    int n_epochs{ 120 };
    bool kinematic{ true };                                   ///< false => fixed point.
    double cruise_speed_mps{ 4.5 };
    double heading0_deg{ 35.0 };
    double heading_amp_deg{ 60.0 };   ///< Heading weave amplitude.
    double heading_period_s{ 1200.0 };///< Heading weave period.
    double heave_amp_m{ 1.0 };
    double heave_period_s{ 8.0 };
    double clock_bias0_s{ 1.0e-4 };
    double clock_drift_sps{ 0.0 };
};

/** @brief Builds and stores the per-epoch truth trajectory. */
class TrajectoryGenerator {
public:
    explicit TrajectoryGenerator(const TrajectoryConfig& cfg);

    const std::vector<TruthState>& states() const { return states_; }
    std::size_t size() const { return states_.size(); }

private:
    std::vector<TruthState> states_;
};

} // namespace sim
} // namespace GnssProcessor
