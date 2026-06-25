/**
 * @file TrajectoryGenerator.cpp
 * @brief Implementation of the analytic SAR-vessel truth trajectory.
 */

#include "sim/TrajectoryGenerator.hpp"

#include <cmath>

#include "common/Constants.hpp"
#include "math/Coordinates.hpp"

namespace GnssProcessor {
namespace sim {

namespace {
GnssTime addSeconds(GnssTime t, double dt_s) {
    t.tow += dt_s;
    const double week = static_cast<double>(TimeUtils::SECONDS_IN_WEEK);
    while (t.tow >= week) { t.tow -= week; t.week += 1; }
    while (t.tow < 0.0) { t.tow += week; t.week -= 1; }
    return t;
}
} // namespace

TrajectoryGenerator::TrajectoryGenerator(const TrajectoryConfig& c) {
    const double deg = Constants::PI / 180.0;
    const double lat0 = c.lat0_deg * deg, lon0 = c.lon0_deg * deg;
    const Eigen::Vector3d r0 = Coordinates::llhToEcef(lat0, lon0, c.h0_m);
    const Eigen::Matrix3d enu2ecef = Coordinates::enuToEcefMatrix(lat0, lon0);

    // Smooth analytic motion as functions of elapsed time (s).
    auto speed = [&](double tt) -> double {
        if (!c.kinematic) return 0.0;
        const double ramp = tt < 300.0 ? 0.5 * (1.0 - std::cos(Constants::PI * tt / 300.0)) : 1.0;
        const double surge = 1.0 + 0.10 * std::sin(2.0 * Constants::PI * tt / 120.0);
        return c.cruise_speed_mps * ramp * surge;
    };
    auto heading = [&](double tt) -> double {
        return (c.heading0_deg + c.heading_amp_deg * std::sin(2.0 * Constants::PI * tt / c.heading_period_s)) * deg;
    };
    auto heave = [&](double tt) -> double {
        return c.kinematic ? c.heave_amp_m * std::sin(2.0 * Constants::PI * tt / c.heave_period_s) : 0.0;
    };

    auto record = [&](double tt, double E, double N) {
        TruthState s;
        s.time = addSeconds(c.t0, tt);
        const double H = heading(tt), V = speed(tt), U = heave(tt);
        const Eigen::Vector3d enu(E, N, U);
        s.ecef = r0 + enu2ecef * enu;
        s.vel_ecef = enu2ecef * Eigen::Vector3d(V * std::sin(H), V * std::cos(H), 0.0);
        s.clock_bias_s = c.clock_bias0_s + c.clock_drift_sps * tt;
        const auto llh = Coordinates::ecefToLlh(s.ecef);
        s.lat_deg = llh.lat_rad / deg;
        s.lon_deg = llh.lon_rad / deg;
        s.h_m = llh.h_m;
        s.east_m = E;
        s.north_m = N;
        s.up_m = U;
        s.heading_deg = H / deg;
        s.speed_mps = V;
        states_.push_back(s);
    };

    // Integrate the horizontal track on a 1 s sub-grid; sample at each epoch.
    const double sub = 1.0;
    const double interval = c.interval_s;
    const long step_per_epoch = std::lround(interval / sub);
    double E = 0.0, N = 0.0, tt = 0.0;
    record(0.0, E, N);
    const long total_steps = step_per_epoch * (c.n_epochs - 1);
    for (long k = 1; k <= total_steps; ++k) {
        const double H = heading(tt), V = speed(tt);
        E += V * std::sin(H) * sub;
        N += V * std::cos(H) * sub;
        tt += sub;
        if (k % step_per_epoch == 0) record(tt, E, N);
    }
}

} // namespace sim
} // namespace GnssProcessor
