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

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
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
    /// Receiver clock: bias(t) = clock_bias0_s + clock_drift_sps*t + w(t), where w is a
    /// random walk of spectral density @c clock_rw_q_m2ps (m^2/s, converted to seconds
    /// internally) accumulated over the epoch grid from @c clock_seed.  Drift 0 + q 0 =
    /// the historical constant bias.  See SimConfig.hpp for the physical rationale.
    double clock_bias0_s{ 1.0e-4 };
    double clock_drift_sps{ 0.0 };
    double clock_rw_q_m2ps{ 0.0 };
    std::uint64_t clock_seed{ 20240315ULL ^ 0xC10CC0DEull };

    /// Seconds of EXACT station-keeping at the start of the arc before any motion
    /// begins (0 = disabled, the historical behaviour).  During the hold the
    /// along-track distance stays at zero, so every epoch in the window resolves
    /// to the identical start ECEF -- a genuine static segment a student can solve
    /// as a fixed point.  The speed ramp-up (and, in landfall mode, the path-length
    /// normalisation) start AFTER the hold, so the vessel still reaches the
    /// destination exactly at the final epoch; it simply has less of the arc in
    /// which to do it.  Applies to both the landfall and the analytic-weave model.
    double static_prefix_s{ 0.0 };

    // --- Optional "landfall" mode ------------------------------------------
    // When BOTH dest_* are finite, the vessel drifts from (lat0,lon0) to the
    // destination over the arc and *decelerates to a stop at grounding* (a SAR
    // vessel running aground), landing exactly on the destination.  The default
    // (NaN) disables it, so the analytic heading-weave model above runs and prior
    // datasets stay byte-identical.  In landfall mode heading0/heading_amp/
    // cruise_speed are unused: the along-track speed is derived so the integrated
    // path length equals the true start->destination distance.
    double dest_lat_deg{ std::numeric_limits<double>::quiet_NaN() };
    double dest_lon_deg{ std::numeric_limits<double>::quiet_NaN() };
    double landfall_decel_frac{ 0.30 };      ///< fraction of the arc spent easing to grounding.
    double landfall_terminal_frac{ 0.0 };    ///< terminal speed as a fraction of Vmax at grounding (0 = dead stop; a small value lets the vessel creep aground still moving).
    double approach_weave_amp_m{ 2000.0 };   ///< lateral meander amplitude (vanishes at both ends).
    double approach_weave_period_s{ 2400.0 };///< lateral meander period (s).
    /// Optional intermediate waypoints (lat_deg, lon_deg) for a meandering landfall:
    /// the path becomes a smooth Catmull-Rom spline start -> waypoints... -> dest.
    /// Empty => the straight start->dest drift above.
    std::vector<std::array<double, 2>> waypoints;
};

/** @brief Builds and stores the per-epoch truth trajectory. */
class TrajectoryGenerator {
public:
    explicit TrajectoryGenerator(const TrajectoryConfig& cfg);

    const std::vector<TruthState>& states() const { return states_; }
    std::size_t size() const { return states_.size(); }

private:
    /// Adds the accumulated clock random walk to every state's @c clock_bias_s.
    /// No-op (and no RNG draw) when @c clock_rw_q_m2ps is 0, so the deterministic
    /// bias+drift clock stays byte-identical.
    void applyClockRandomWalk(const TrajectoryConfig& cfg);

    std::vector<TruthState> states_;
};

} // namespace sim
} // namespace GnssProcessor
