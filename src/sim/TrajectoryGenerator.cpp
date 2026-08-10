/**
 * @file TrajectoryGenerator.cpp
 * @brief Implementation of the analytic SAR-vessel truth trajectory.
 */

#include "sim/TrajectoryGenerator.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

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

    // ---- Landfall mode: drift start -> destination, grounding at the end. ----
    if (c.kinematic && std::isfinite(c.dest_lat_deg) && std::isfinite(c.dest_lon_deg)) {
        const double T = (c.n_epochs > 1) ? (c.n_epochs - 1) * c.interval_s : 1.0;
        const double df = std::min(std::max(c.landfall_decel_frac, 0.05), 0.95);
        const double tf = std::min(std::max(c.landfall_terminal_frac, 0.0), 1.0);
        auto decel = [&](double tt) {                          // 1 -> tf over the last df of the arc
            const double tau = tt / T;
            const double d = (tau > 1.0 - df)                  // raised-cosine ease-in, 1 -> 0
                                 ? 0.5 * (1.0 - std::cos(Constants::PI * (1.0 - tau) / df))
                                 : 1.0;
            return tf + (1.0 - tf) * d;                        // floor at tf (0 = dead stop; >0 = still creeping)
        };
        // Station-keeping prefix: the shape is IDENTICALLY zero for the first
        // `static_prefix_s`, so `along` never advances and every epoch in the
        // window maps to the same lat/lon => an exactly static segment.  The
        // 300 s ramp-up is measured from the END of the hold, and because the
        // normalisation sum S below integrates this same shape, the path length
        // still comes out exactly L (the vessel reaches the destination at the
        // last epoch, just travelling slightly faster for the rest of the arc).
        const double hold = std::max(0.0, c.static_prefix_s);
        auto shape = [&](double tt) {                          // speed shape in [0,1]
            if (tt < hold) return 0.0;
            const double s = tt - hold;
            const double up = s < 300.0 ? 0.5 * (1.0 - std::cos(Constants::PI * s / 300.0)) : 1.0;
            return up * decel(tt);
        };
        auto heave = [&](double tt) {                          // sea-state, fading as it grounds
            if (tt < hold) return 0.0;                         // held station => exactly static, vertically too
            return c.heave_amp_m * std::sin(2.0 * Constants::PI * tt / c.heave_period_s) * decel(tt);
        };

        const double sub = 1.0;
        const long step_per_epoch = std::lround(c.interval_s / sub);
        const long total_steps = step_per_epoch * (c.n_epochs - 1);
        // Normalise the speed so the integrated along-track distance == path length
        // exactly (matches the epoch loops below => the last epoch lands on dest).
        double S = 0.0;
        for (long k = 0; k < total_steps; ++k) S += shape(k * sub) * sub;

        // Every landfall path is interpolated GEODETICALLY at height h0 so the
        // vessel stays on the sea surface (a flat ENU tangent plane would bow
        // ~L^2/(2R), hundreds of m, off the ellipsoid over a multi-km arc).
        std::vector<double> tEp;
        std::vector<Eigen::Vector3d> rEp;

        if (!c.waypoints.empty()) {
            // --- Meandering path: a smooth Catmull-Rom spline through waypoints. ---
            // Control points start -> waypoints... -> dest; longitudes unwrapped so a
            // +/-180 dateline crossing stays continuous.  The spline passes THROUGH
            // every control point, so start and dest (grounding) are hit exactly.
            std::vector<double> clat{ c.lat0_deg }, clon{ c.lon0_deg };
            auto unwrap = [](double lon, double ref) {
                while (lon - ref > 180.0) lon -= 360.0;
                while (lon - ref < -180.0) lon += 360.0;
                return lon;
            };
            for (const auto& w : c.waypoints) { clat.push_back(w[0]); clon.push_back(unwrap(w[1], clon.back())); }
            clat.push_back(c.dest_lat_deg);
            clon.push_back(unwrap(c.dest_lon_deg, clon.back()));
            const int nc = static_cast<int>(clat.size());

            auto cr = [](double p0, double p1, double p2, double p3, double u) {
                const double u2 = u * u, u3 = u2 * u;
                return 0.5 * (2.0 * p1 + (-p0 + p2) * u + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * u2
                              + (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * u3);
            };
            const int per = 200;                               // dense samples per segment
            std::vector<double> slat, slon;
            for (int i = 0; i < nc - 1; ++i) {
                const int im1 = std::max(0, i - 1), ip2 = std::min(nc - 1, i + 2);
                for (int k = 0; k < per; ++k) {
                    const double u = static_cast<double>(k) / per;
                    slat.push_back(cr(clat[im1], clat[i], clat[i + 1], clat[ip2], u));
                    slon.push_back(cr(clon[im1], clon[i], clon[i + 1], clon[ip2], u));
                }
            }
            slat.push_back(clat[nc - 1]); slon.push_back(clon[nc - 1]);  // exact endpoint
            const int M = static_cast<int>(slat.size());

            std::vector<double> arc(M, 0.0);
            std::vector<Eigen::Vector3d> P(M);
            for (int k = 0; k < M; ++k) P[k] = Coordinates::llhToEcef(slat[k] * deg, slon[k] * deg, c.h0_m);
            for (int k = 1; k < M; ++k) arc[k] = arc[k - 1] + (P[k] - P[k - 1]).norm();
            const double L = arc[M - 1];
            const double Vmax = (S > 0.0) ? L / S : 0.0;

            int cur = 0;                                       // monotonic arc-length cursor
            auto latlonAt = [&](double along, double& la, double& lo) {
                along = std::min(std::max(along, 0.0), L);
                while (cur + 1 < M && arc[cur + 1] < along) ++cur;
                const double seg = arc[cur + 1] - arc[cur];
                const double f = (seg > 1e-9) ? (along - arc[cur]) / seg : 0.0;
                la = slat[cur] + f * (slat[cur + 1] - slat[cur]);
                lo = slon[cur] + f * (slon[cur + 1] - slon[cur]);
            };
            double la, lo, along = 0.0, tt = 0.0;
            latlonAt(0.0, la, lo);
            tEp.push_back(0.0);
            rEp.push_back(Coordinates::llhToEcef(la * deg, lo * deg, c.h0_m + heave(0.0)));
            for (long k = 1; k <= total_steps; ++k) {
                along += Vmax * shape(tt) * sub;
                tt += sub;
                if (k % step_per_epoch == 0) {
                    latlonAt(along, la, lo);
                    tEp.push_back(tt);
                    rEp.push_back(Coordinates::llhToEcef(la * deg, lo * deg, c.h0_m + heave(tt)));
                }
            }
        } else {
            // --- Straight geodetic drift start -> dest, with a lateral meander. ---
            const double latMid = 0.5 * (c.lat0_deg + c.dest_lat_deg) * deg;
            const double mPerDegLat = 111132.0;
            const double mPerDegLon = 111320.0 * std::cos(latMid);
            double dLat = c.dest_lat_deg - c.lat0_deg;
            double dLon = c.dest_lon_deg - c.lon0_deg;
            while (dLon > 180.0)  dLon -= 360.0;
            while (dLon < -180.0) dLon += 360.0;
            const double aE = dLon * mPerDegLon, aN = dLat * mPerDegLat;
            const double L = std::hypot(aE, aN);
            const double uE = (L > 0.0) ? aE / L : 0.0, uN = (L > 0.0) ? aN / L : 1.0;
            const double pE = uN, pN = -uE;                    // cross-track unit
            const double Vmax = (S > 0.0) ? L / S : 0.0;
            // Cross-track meander, sin^2(pi*tau) envelope: zero VALUE and SLOPE at
            // both ends => the track starts and grounds exactly on-axis.
            auto crossOff = [&](double tt) {
                const double e = std::sin(Constants::PI * tt / T);
                return c.approach_weave_amp_m
                       * std::sin(2.0 * Constants::PI * tt / std::max(c.approach_weave_period_s, 1.0)) * e * e;
            };
            auto posOf = [&](double tt, double along) -> Eigen::Vector3d {
                const double fr = (L > 0.0) ? std::min(std::max(along / L, 0.0), 1.0) : 0.0;
                const double crv = crossOff(tt);
                const double lat = c.lat0_deg + fr * dLat + (crv * pN) / mPerDegLat;
                const double lon = c.lon0_deg + fr * dLon + (crv * pE) / mPerDegLon;
                return Coordinates::llhToEcef(lat * deg, lon * deg, c.h0_m + heave(tt));
            };
            tEp.push_back(0.0);
            rEp.push_back(posOf(0.0, 0.0));
            double along = 0.0, tt = 0.0;
            for (long k = 1; k <= total_steps; ++k) {
                along += Vmax * shape(tt) * sub;
                tt += sub;
                if (k % step_per_epoch == 0) { tEp.push_back(tt); rEp.push_back(posOf(tt, along)); }
            }
        }

        // ---- Emit epochs; ECEF velocity via central finite difference. ----
        const std::size_t n = rEp.size();
        for (std::size_t i = 0; i < n; ++i) {
            TruthState st;
            st.time = addSeconds(c.t0, tEp[i]);
            st.ecef = rEp[i];
            Eigen::Vector3d v = Eigen::Vector3d::Zero();
            if (n >= 2) {
                if (i == 0)          v = (rEp[1] - rEp[0]) / c.interval_s;
                else if (i + 1 == n) v = (rEp[i] - rEp[i - 1]) / c.interval_s;
                else                 v = (rEp[i + 1] - rEp[i - 1]) / (2.0 * c.interval_s);
            }
            st.vel_ecef = v;
            st.clock_bias_s = c.clock_bias0_s + c.clock_drift_sps * tEp[i];
            const auto llh = Coordinates::ecefToLlh(st.ecef);
            st.lat_deg = llh.lat_rad / deg;
            st.lon_deg = llh.lon_rad / deg;
            st.h_m = llh.h_m;
            const Eigen::Vector3d off = enu2ecef.transpose() * (st.ecef - r0);
            st.east_m = off.x(); st.north_m = off.y(); st.up_m = off.z();
            // Heading/speed from horizontal velocity in the LOCAL ENU frame.
            const Eigen::Matrix3d e2e = Coordinates::enuToEcefMatrix(llh.lat_rad, llh.lon_rad);
            const Eigen::Vector3d venu = e2e.transpose() * v;
            st.speed_mps = std::hypot(venu.x(), venu.y());
            double hd = std::atan2(venu.x(), venu.y()) / deg;  // 0 = north, cw
            st.heading_deg = hd < 0.0 ? hd + 360.0 : hd;
            states_.push_back(st);
        }
        return;
    }

    // Smooth analytic motion as functions of elapsed time (s).
    const double hold = std::max(0.0, c.static_prefix_s);      // station-keeping prefix (see header)
    auto speed = [&](double tt) -> double {
        if (!c.kinematic || tt < hold) return 0.0;
        const double s = tt - hold;
        const double ramp = s < 300.0 ? 0.5 * (1.0 - std::cos(Constants::PI * s / 300.0)) : 1.0;
        const double surge = 1.0 + 0.10 * std::sin(2.0 * Constants::PI * s / 120.0);
        return c.cruise_speed_mps * ramp * surge;
    };
    auto heading = [&](double tt) -> double {
        return (c.heading0_deg + c.heading_amp_deg * std::sin(2.0 * Constants::PI * tt / c.heading_period_s)) * deg;
    };
    auto heave = [&](double tt) -> double {
        if (!c.kinematic || tt < hold) return 0.0;             // held station => exactly static
        return c.heave_amp_m * std::sin(2.0 * Constants::PI * tt / c.heave_period_s);
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
