/*!
 * \file GnssCorrections.cpp
 * \brief GNSS measurement and geodetic corrections.
 *
 * Organization:
 *  - Construction / product management
 *  - Clock interpolation
 *  - Relativistic & propagation corrections
 *  - Geophysical site displacements (pole tide, solid Earth tide, ocean/atm loading)
 *  - IERS 2010 Step 2 helpers (fundamental arguments, constituent tables)
 *  - Bias retrieval
 *  - Phase wind-up
 */

#include "GnssCorrections.hpp"
#include "Coordinates.hpp"
#include "Constants.hpp"
#include "GnssConfig.hpp"
#include "RinexParser.hpp"
#include "Interpolate.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <fmt/format.h>

namespace GnssProcessor {

    // Shared validation constants for satellite geometry
    namespace {
        constexpr double MIN_SAT_NORM  = 2.0e7;  // ~20 000 km (lowest MEO orbit)
        constexpr double MAX_SAT_NORM  = 4.5e7;  // ~45 000 km (above BeiDou GEO)
        constexpr double MIN_RX_NORM   = 6.1e6;  // Earth surface min with margin
        constexpr double MAX_RX_NORM   = 6.6e6;  // Earth surface max + moderate altitude
        constexpr double MAX_VEL_NORM  = 5.0e3;  // ~5 km/s (GNSS satellites)
    }

    // =================================================================
    // Construction / product management
    // =================================================================

    GnssCorrections::GnssCorrections(LogManager& log_mgr)
        : log_manager_(log_mgr),
        clocks_(), erps_(), biases_(), orbits_(),
        config_(nullptr), rinex_parser_(nullptr),
        ocean_tide_model_(log_mgr) {
        log_manager_.getLogger("corrections")->trace("GnssCorrections constructed (minimal)");
        buildClockIndex();
    }

    GnssCorrections::GnssCorrections(LogManager& log_mgr,
        const std::vector<ClockData>& clocks,
        const std::vector<ErpData>& erps)
        : log_manager_(log_mgr),
        clocks_(clocks), erps_(erps), biases_(), orbits_(),
        config_(nullptr), rinex_parser_(nullptr),
        ocean_tide_model_(log_mgr) {
        log_manager_.getLogger("corrections")->trace("GnssCorrections constructed with {} clocks, {} erps",
            clocks_.size(), erps_.size());
        buildClockIndex();
    }

    GnssCorrections::GnssCorrections(LogManager& log_mgr,
        const GnssConfig& config,
        const std::vector<ClockData>& clocks,
        const std::vector<ErpData>& erps,
        const std::vector<BiasData>& biases,
        const RinexParser& rinex)
        : log_manager_(log_mgr),
        clocks_(clocks), erps_(erps), biases_(biases), orbits_(),
        config_(&config), rinex_parser_(&rinex),
        ocean_tide_model_(log_mgr) {
        log_manager_.getLogger("corrections")->trace("GnssCorrections constructed (full) with {} clocks, {} erps, {} biases",
            clocks_.size(), erps_.size(), biases_.size());
        buildClockIndex();
    }

    void GnssCorrections::setClocksAndErps(const std::vector<ClockData>& clocks, const std::vector<ErpData>& erps) {
        clocks_ = clocks;
        erps_ = erps;
        buildClockIndex();
        log_manager_.getLogger("corrections")->trace("GnssCorrections updated: {} clocks, {} erps", clocks_.size(), erps_.size());
    }

    void GnssCorrections::buildClockIndex() {
        clock_index_.clear();
        clock_index_.reserve(300);

        for (const auto& c : clocks_)
            clock_index_[c.satellite].push_back(&c);

        for (auto& [sat, vec] : clock_index_) {
            std::sort(vec.begin(), vec.end(),
                [](const ClockData* a, const ClockData* b) {
                    return (a->time.week * TimeUtils::SECONDS_IN_WEEK + a->time.tow) <
                        (b->time.week * TimeUtils::SECONDS_IN_WEEK + b->time.tow);
                });
        }

        log_manager_.getLogger("corrections")->debug(
            "Built clock index: {} satellites indexed", clock_index_.size());
    }

    // =================================================================
    // Clock interpolation
    // =================================================================

    double GnssCorrections::getSatelliteClockCorrection(const std::string& satellite, const GnssTime& time) const {
        auto log = log_manager_.getLogger("ppp_model");

        if (satellite.empty()) {
            log->warn("Empty satellite ID for clock correction at TOW {:.3f}", time.tow);
            return 0.0;
        }
        if (time.tow < 0.0 || time.week < 0) {
            log->warn("Invalid time for clock correction for {}: week {}, TOW {:.3f}", satellite, time.week, time.tow);
            return 0.0;
        }
        if (clocks_.empty()) {
            log->warn("No clock data available for {} at TOW {:.3f}", satellite, time.tow);
            return 0.0;
        }

        double clock_corr = interpolateClock(satellite, time, clocks_);
        if (std::isfinite(clock_corr) && std::abs(clock_corr) < 1e-1) {
            log->trace("Interpolated clock for {} ({}) at TOW {:.3f}: {:.3e} s",
                satellite, PppUtils::getSystemString(satellite), time.tow, clock_corr);
            return clock_corr;
        }

        log->warn("Invalid clock correction for {} ({}) at TOW {:.3f}: {:.3e} s",
            satellite, PppUtils::getSystemString(satellite), time.tow, clock_corr);
        return 0.0;
    }

    double GnssCorrections::interpolateClock(const std::string& satellite,
        const GnssTime& time,
        const std::vector<ClockData>& clocks) const
    {
        const SatelliteClockInterpolationMode mode = config_
            ? config_->getSatelliteClockInterpolationMode()
            : SatelliteClockInterpolationMode::Linear;

        constexpr double MAX_WINDOW     = 3600.0;
        constexpr double MAX_CLOCK_BIAS = 1e-3;
        constexpr size_t LAGRANGE_POINTS = 9;

        if (satellite.empty()) {
            log_manager_.getLogger("corrections")->debug("Clock interp: empty satellite ID");
            return 0.0;
        }

        auto idx_it = clock_index_.find(satellite);
        if (idx_it == clock_index_.end() || idx_it->second.empty()) {
            log_manager_.getLogger("corrections")->debug("Clock interp for {}: no data in index", satellite);
            return 0.0;
        }

        const auto& sat_clocks = idx_it->second;
        const double t_query = TimeUtils::continuousGpsSeconds(time);
        auto log = log_manager_.getLogger("corrections");

        auto valid_node = [&](const ClockData* c) -> bool {
            return std::isfinite(c->clock_bias)
                && std::abs(c->clock_bias) < MAX_CLOCK_BIAS
                && std::abs(TimeUtils::continuousGpsSeconds(c->time) - t_query) < MAX_WINDOW;
        };

        // Binary search helper used by both modes
        auto find_upper = [&]() {
            return std::lower_bound(sat_clocks.begin(), sat_clocks.end(), t_query,
                [](const ClockData* c, double t) {
                    return TimeUtils::continuousGpsSeconds(c->time) < t;
                });
        };

        // ---------------------------------------------------------------
        // LINEAR MODE
        // ---------------------------------------------------------------
        if (mode == SatelliteClockInterpolationMode::Linear) {
            auto upper_it = find_upper();

            // Exact epoch match
            if (upper_it != sat_clocks.end()) {
                double t_node = TimeUtils::continuousGpsSeconds((*upper_it)->time);
                if (std::abs(t_node - t_query) < 1e-6 && valid_node(*upper_it))
                    return (*upper_it)->clock_bias;
            }

            // Boundary: fall back to nearest single node
            if (upper_it == sat_clocks.begin() || upper_it == sat_clocks.end()) {
                const ClockData* nearest = (upper_it == sat_clocks.end())
                    ? sat_clocks.back() : sat_clocks.front();
                if (valid_node(nearest)) {
                    log->debug("Clock interp (linear nearest) for {} at tow={:.3f}: boundary, result={:.3e}s",
                        satellite, time.tow, nearest->clock_bias);
                    return nearest->clock_bias;
                }
                return 0.0;
            }

            const ClockData* c_lo = *(upper_it - 1);
            const ClockData* c_hi = *upper_it;

            if (!valid_node(c_lo) || !valid_node(c_hi)) {
                log->warn("Clock interp (linear): invalid bracketing nodes for {} at tow={:.3f}", satellite, time.tow);
                return 0.0;
            }

            const double x0 = TimeUtils::continuousGpsSeconds(c_lo->time);
            const double x1 = TimeUtils::continuousGpsSeconds(c_hi->time);
            const double dt = x1 - x0;

            if (dt < 1e-9) {
                log->warn("Clock interp (linear): degenerate bracket dt={:.3e}s for {} at tow={:.3f}",
                    dt, satellite, time.tow);
                return c_lo->clock_bias;
            }

            // Reject interpolation across clock gaps
            constexpr double MAX_BRACKET_GAP = 65.0;
            if (dt > MAX_BRACKET_GAP) {
                log->warn("Clock interp (linear): bracket gap {:.1f}s exceeds {:.1f}s for {} at tow={:.3f} — rejecting",
                    dt, MAX_BRACKET_GAP, satellite, time.tow);
                return 0.0;
            }

            const double result = c_lo->clock_bias
                + (c_hi->clock_bias - c_lo->clock_bias) * (t_query - x0) / dt;

            log->debug("Clock interp (linear) for {} at tow={:.3f}: bracket=[{:.3f},{:.3f}]s span={:.1f}s "
                "result={:.3e}s ({:.3f}m)",
                satellite, time.tow, x0, x1, dt, result, result * Constants::SPEED_OF_LIGHT);
            return result;
        }

        // ---------------------------------------------------------------
        // LAGRANGE MODE — centered window of LAGRANGE_POINTS nodes
        //
        // Not recommended for standard 30 s IGS CLK files — use Linear instead.
        // Retained for high-rate (1 Hz) products.
        // ---------------------------------------------------------------
        auto upper_it = find_upper();

        size_t nearest_idx = 0;
        if (upper_it == sat_clocks.end()) {
            nearest_idx = sat_clocks.size() - 1;
        } else if (upper_it == sat_clocks.begin()) {
            nearest_idx = 0;
        } else {
            size_t hi = std::distance(sat_clocks.begin(), upper_it);
            size_t lo = hi - 1;
            double t_lo = TimeUtils::continuousGpsSeconds(sat_clocks[lo]->time);
            double t_hi = TimeUtils::continuousGpsSeconds(sat_clocks[hi]->time);
            nearest_idx = (t_query - t_lo <= t_hi - t_query) ? lo : hi;
        }

        const size_t half_before = LAGRANGE_POINTS / 2;
        size_t start_idx = (nearest_idx > half_before) ? nearest_idx - half_before : 0;
        if (start_idx + LAGRANGE_POINTS > sat_clocks.size())
            start_idx = (sat_clocks.size() > LAGRANGE_POINTS) ? sat_clocks.size() - LAGRANGE_POINTS : 0;
        const size_t end_idx = std::min(start_idx + LAGRANGE_POINTS, sat_clocks.size());

        std::vector<const ClockData*> nodes;
        nodes.reserve(LAGRANGE_POINTS);
        for (size_t i = start_idx; i < end_idx; ++i)
            if (valid_node(sat_clocks[i])) nodes.push_back(sat_clocks[i]);

        if (nodes.empty()) {
            log->warn("Clock interp (lagrange) for {} at tow={:.3f}: no valid nodes", satellite, time.tow);
            return 0.0;
        }
        if (nodes.size() == 1) return nodes[0]->clock_bias;

        // Extrapolation guard: fall back to linear to avoid oscillation
        const double t_first = TimeUtils::continuousGpsSeconds(nodes.front()->time);
        const double t_last  = TimeUtils::continuousGpsSeconds(nodes.back()->time);
        if (t_query < t_first || t_query > t_last) {
            std::sort(nodes.begin(), nodes.end(), [t_query](const ClockData* a, const ClockData* b) {
                return std::abs(TimeUtils::continuousGpsSeconds(a->time) - t_query) <
                       std::abs(TimeUtils::continuousGpsSeconds(b->time) - t_query);
            });
            const double x0 = TimeUtils::continuousGpsSeconds(nodes[0]->time);
            const double x1 = TimeUtils::continuousGpsSeconds(nodes[1]->time);
            const double dt = x1 - x0;
            const double result = (std::abs(dt) > 1e-9)
                ? nodes[0]->clock_bias + (nodes[1]->clock_bias - nodes[0]->clock_bias) * (t_query - x0) / dt
                : nodes[0]->clock_bias;
            log->debug("Clock interp (lagrange->linear extrap) for {} at tow={:.3f}: result={:.3e}s ({:.3f}m)",
                satellite, time.tow, result, result * Constants::SPEED_OF_LIGHT);
            return result;
        }

        const size_t n = nodes.size();
        std::vector<double> x(n), y(n);
        for (size_t i = 0; i < n; ++i) {
            x[i] = TimeUtils::continuousGpsSeconds(nodes[i]->time);
            y[i] = nodes[i]->clock_bias;
        }

        std::vector<double> w = Interpolate::barycentricWeights(x);
        const double result   = Interpolate::barycentricEval(x, w, y, t_query);

        log->debug("Clock interp (lagrange) for {} at tow={:.3f}: {} pts, span={:.1f}s, result={:.3e}s ({:.3f}m)",
            satellite, time.tow, n, x.back() - x.front(), result, result * Constants::SPEED_OF_LIGHT);
        return result;
    }

    // =================================================================
    // Relativistic & propagation corrections
    // =================================================================

    double GnssCorrections::computeRelativisticCorrection(const std::string& satellite, const Eigen::Vector3d& sat_pos,
        const Eigen::Vector3d& sat_vel) const {
        // dtr = -2 * (r . v) / c^2  [seconds]
        constexpr double C_SQUARED = Constants::SPEED_OF_LIGHT * Constants::SPEED_OF_LIGHT;
        constexpr double MAX_CORRECTION = 1e-6;

        auto log = log_manager_.getLogger("ppp_model");
        std::string sys = PppUtils::getSystemString(satellite);

        if (!sat_pos.allFinite() || !sat_vel.allFinite()) {
            log->warn("Invalid relativistic inputs for {} ({}): pos=({:.3f},{:.3f},{:.3f}), vel=({:.3f},{:.3f},{:.3f})",
                satellite, sys, sat_pos.x(), sat_pos.y(), sat_pos.z(), sat_vel.x(), sat_vel.y(), sat_vel.z());
            return 0.0;
        }

        const double pos_norm = sat_pos.norm();
        const double vel_norm = sat_vel.norm();
        if (pos_norm < MIN_SAT_NORM || pos_norm > MAX_SAT_NORM || vel_norm > MAX_VEL_NORM) {
            log->warn("Unrealistic relativistic norms for {} ({}): pos={:.3e} m, vel={:.3e} m/s",
                satellite, sys, pos_norm, vel_norm);
            return 0.0;
        }

        const double r_dot_v = sat_pos.dot(sat_vel);
        const double correction = -2.0 * r_dot_v / C_SQUARED;

        if (!std::isfinite(correction) || std::abs(correction) > MAX_CORRECTION) {
            log->warn("Excessive relativistic correction for {} ({}): {:.3e} s, r.v={:.3e}", satellite, sys, correction, r_dot_v);
            return 0.0;
        }

        log->trace("Relativistic correction for {} ({}): {:.3e} s, r.v={:.3e}", satellite, sys, correction, r_dot_v);
        return correction;
    }

    double GnssCorrections::computeShapiroDelay(const std::string& satellite, const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos) const {
        // rpath = (2*GM/c^2) * ln((r_sat + r_rec + r_sr) / (r_sat + r_rec - r_sr))  [meters]
        constexpr double GM = Constants::GRAVITATIONAL_CONSTANT;
        constexpr double C_SQUARED = Constants::SPEED_OF_LIGHT * Constants::SPEED_OF_LIGHT;
        constexpr double MAX_DELAY = 0.03;

        auto log = log_manager_.getLogger("ppp_model");
        std::string sys = PppUtils::getSystemString(satellite);

        if (!rx_pos.allFinite() || !sat_pos.allFinite()) {
            log->warn("Invalid Shapiro inputs for {} ({}): rx=({:.3f},{:.3f},{:.3f}), sat=({:.3f},{:.3f},{:.3f})",
                satellite, sys, rx_pos.x(), rx_pos.y(), rx_pos.z(), sat_pos.x(), sat_pos.y(), sat_pos.z());
            return 0.0;
        }

        const double rsat = sat_pos.norm();
        const double rrec = rx_pos.norm();
        const double rrs  = (sat_pos - rx_pos).norm();

        if (rrs < 1.9e7 || rsat < MIN_SAT_NORM || rsat > MAX_SAT_NORM) {
            log->warn("Unrealistic Shapiro norms for {} ({}): range={:.3e} m, sat={:.3e} m", satellite, sys, rrs, rsat);
            return 0.0;
        }

        const double delay = (2.0 * GM / C_SQUARED) * std::log((rsat + rrec + rrs) / (rsat + rrec - rrs));

        if (!std::isfinite(delay) || std::abs(delay) > MAX_DELAY) {
            log->warn("Invalid Shapiro delay for {} ({}): {:.3e} m", satellite, sys, delay);
            return 0.0;
        }

        log->trace("Shapiro delay for {} ({}): {:.6f} m, range={:.3e} m", satellite, sys, delay, rrs);
        return delay;
    }

    double GnssCorrections::computeSagnacCorrection(const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos) const {
        // correction = (omega_E / c) * (x_s * y_r - y_s * x_r)  [meters]
        constexpr double EARTH_ANGULAR_VELOCITY = 7.2921151467e-5;
        constexpr double C = Constants::SPEED_OF_LIGHT;
        constexpr double MAX_SAGNAC = 40.0;

        auto log = log_manager_.getLogger("ppp_model");

        if (!rx_pos.allFinite() || !sat_pos.allFinite()) {
            log->warn("Invalid Sagnac inputs: rx=({},{},{}), sat=({},{},{})",
                rx_pos.x(), rx_pos.y(), rx_pos.z(), sat_pos.x(), sat_pos.y(), sat_pos.z());
            return 0.0;
        }
        double rx_norm = rx_pos.norm();
        double sat_norm = sat_pos.norm();
        if (rx_norm < MIN_RX_NORM || rx_norm > MAX_RX_NORM || sat_norm < MIN_SAT_NORM || sat_norm > MAX_SAT_NORM) {
            log->warn("Unrealistic Sagnac norms: rx={:.3e} m, sat={:.3e} m", rx_norm, sat_norm);
            return 0.0;
        }

        double cross_term = sat_pos.x() * rx_pos.y() - sat_pos.y() * rx_pos.x();
        double correction = (EARTH_ANGULAR_VELOCITY / C) * cross_term;
        if (!std::isfinite(correction) || std::abs(correction) > MAX_SAGNAC) {
            log->warn("Excessive Sagnac correction: {:.3e} m", correction);
            return 0.0;
        }

        log->trace("Sagnac correction: {:.3f} m", correction);
        return correction;
    }

    // =================================================================
    // Geophysical site displacements
    // =================================================================

    Eigen::Vector3d GnssCorrections::computePoleTideCorrection(const Eigen::Vector3d& rx_pos, const GnssTime& time) const {
        auto log = log_manager_.getLogger("ppp_model");

        if (!rx_pos.allFinite() || rx_pos.norm() < 1e3) {
            log->warn("Invalid receiver position for pole tide at TOW {:.3f}: norm={:.3e}", time.tow, rx_pos.norm());
            return Eigen::Vector3d::Zero();
        }
        if (time.tow < 0.0 || time.week < 0) {
            log->warn("Invalid time for pole tide: week {}, TOW {:.3f}", time.week, time.tow);
            return Eigen::Vector3d::Zero();
        }
        if (erps_.empty()) {
            log->warn("No ERP data for pole tide at TOW {:.3f}", time.tow);
            return Eigen::Vector3d::Zero();
        }

        // Find closest ERP entry
        const double t_query = TimeUtils::continuousGpsSeconds(time);
        auto it = std::min_element(erps_.begin(), erps_.end(),
            [t_query](const ErpData& a, const ErpData& b) {
                return std::abs(TimeUtils::continuousGpsSeconds(a.time) - t_query) <
                    std::abs(TimeUtils::continuousGpsSeconds(b.time) - t_query);
            });
        if (it == erps_.end() || !std::isfinite(it->xp) || !std::isfinite(it->yp)) {
            log->trace("No valid ERP data for pole tide at TOW {:.3f}", time.tow);
            return Eigen::Vector3d::Zero();
        }

        const Coordinates::Llh llh = Coordinates::ecefToLlh(rx_pos);
        const double phi = llh.lat_rad;
        const double lambda = llh.lon_rad;

        // IERS 2010 mean pole (linear model, epoch 2000.0)
        const double mjd = TimeUtils::gpsTimeToMjd(time);
        const double decimal_year = TimeUtils::mjdToDecimalYear(mjd);
        const double xp_mean_mas = 55.974 + 1.8243 * (decimal_year - 2000.0);
        const double yp_mean_mas = 346.346 + 1.7896 * (decimal_year - 2000.0);

        constexpr double AS2RAD  = 4.848136811e-6;
        constexpr double MAS2RAD = AS2RAD * 1e-3;

        const double m1 =  (it->xp * AS2RAD) - (xp_mean_mas * MAS2RAD);
        const double m2 = -(it->yp * AS2RAD) + (yp_mean_mas * MAS2RAD);

        // IERS 2010, Eq. 7.26 (simplified Love/Shida)
        const double radial = -0.033 * std::sin(2.0 * phi) * (m1 * std::cos(lambda) + m2 * std::sin(lambda));
        const double north  = -0.009 * std::cos(2.0 * phi) * (m1 * std::cos(lambda) + m2 * std::sin(lambda));
        const double east   =  0.009 * std::cos(phi)        * (m1 * std::sin(lambda) - m2 * std::cos(lambda));

        // Local (r,n,e) -> ECEF
        const double cp = std::cos(phi), sp = std::sin(phi);
        const double cl = std::cos(lambda), sl = std::sin(lambda);
        Eigen::Vector3d correction(
            radial * cp * cl - north * sp * cl - east * sl,
            radial * cp * sl - north * sp * sl + east * cl,
            radial * sp + north * cp);

        if (!correction.allFinite() || correction.norm() > 0.05) {
            log->warn("Invalid pole tide at TOW {:.3f}: norm={:.3e}", time.tow, correction.norm());
            return Eigen::Vector3d::Zero();
        }

        log->trace("Pole tide at TOW {:.3f}: ({:.3e},{:.3e},{:.3e}) m, m1={:.3e}, m2={:.3e}",
            time.tow, correction.x(), correction.y(), correction.z(), m1, m2);
        return correction;
    }

    Eigen::Vector3d GnssCorrections::computeOceanTideCorrection(const Eigen::Vector3d& rx_pos, const GnssTime& time) const {
        return ocean_tide_model_.computeDisplacement(rx_pos, time);
    }

    void GnssCorrections::setOceanTideCoefficients(const OtlStationCoefficients& coeffs) {
        ocean_tide_model_.setStationCoefficients(coeffs);
    }

    bool GnssCorrections::loadOceanTideFromBLQ(const std::string& blq_path, const std::string& station_name) {
        OtlStationCoefficients coeffs;
        if (ocean_tide_model_.parseBLQ(blq_path, station_name, coeffs)) {
            ocean_tide_model_.setStationCoefficients(coeffs);
            return true;
        }
        return false;
    }

    bool GnssCorrections::hasOceanTideCoefficients() const {
        return ocean_tide_model_.hasStationCoefficients();
    }

    double GnssCorrections::computeAtmosphericLoadingCorrection(const Eigen::Vector3d& rx_pos,
        const GnssTime& time) const {
        Coordinates::Llh ref_llh = Coordinates::ecefToLlh(rx_pos);
        double latitude = ref_llh.lat_rad * 180.0 / Constants::PI;
        double t = fmod(time.tow, TimeUtils::SECONDS_IN_DAY);
        double amplitude = 0.001;
        double correction = amplitude * std::cos(2.0 * Constants::PI * t / TimeUtils::SECONDS_IN_DAY)
                          * std::cos(latitude * Constants::PI / 180.0);
        log_manager_.getLogger("ppp_model")->trace("Atmospheric loading correction: {} m", correction);
        return correction;
    }

    // =================================================================
    // Solid Earth tide (IERS 2010)
    // =================================================================

    double GnssCorrections::computeSolidEarthTide(const Eigen::Vector3d& receiver_pos, const Eigen::Vector3d& sat_pos,
        const Eigen::Vector3d& sun_pos, const Eigen::Vector3d& moon_pos,
        const GnssTime& time) const {
        // IERS 2010 nominal Love numbers
        constexpr double h2 = 0.6078, l2 = 0.0847;
        constexpr double h3 = 0.292,  l3 = 0.015;
        // Mass ratios: Sun/Earth, Moon/Earth
        constexpr double MS2E = 332946.0, MM2E = 0.01230002;
        const double a = Constants::Ellipsoid::WGS84::a;

        if (!receiver_pos.allFinite() || !sat_pos.allFinite() || !sun_pos.allFinite() || !moon_pos.allFinite()) {
            log_manager_.getLogger("ppp_model")->warn("Solid Earth tide: non-finite inputs");
            return 0.0;
        }

        // LOS unit vector (sat -> rx)
        const Eigen::Vector3d rho = receiver_pos - sat_pos;
        const double rho_norm = rho.norm();
        if (!(rho_norm > 0.0)) return 0.0;
        const Eigen::Vector3d los = rho / rho_norm;

        const double r = receiver_pos.norm();
        if (!(r > 0.0)) return 0.0;

        // Geodetic coordinates & latitude-dependent Love number corrections
        const Coordinates::Llh llh = Coordinates::ecefToLlh(receiver_pos);
        const double lat = llh.lat_rad;
        const double lambda = llh.lon_rad;
        const double sin_lat = std::sin(lat);
        const double trm = 3.0 * sin_lat * sin_lat - 1.0;
        const double h2_eff = h2 + (-0.0006 * trm);
        const double l2_eff = l2 + ( 0.0002 * trm);

        const Eigen::Vector3d u = receiver_pos / r;

        // Degree-2/3 tide body contribution
        auto tideBody = [&](const Eigen::Vector3d& body_pos, double mass_ratio, int deg) -> Eigen::Vector3d {
            const double d = body_pos.norm();
            if (!(d > 0.0)) return Eigen::Vector3d::Zero();
            const Eigen::Vector3d ub = body_pos / d;
            const double cos_psi = ub.dot(u);
            const Eigen::Vector3d t = ub - cos_psi * u;

            if (deg == 2) {
                const double P2 = (3.0 * cos_psi * cos_psi - 1.0) / 2.0;
                const double scale = mass_ratio * std::pow(a, 4) / std::pow(d, 3);
                return scale * ((h2_eff * u) * P2 + (3.0 * l2_eff * cos_psi) * t);
            } else {
                const double P3 = (5.0 * cos_psi * cos_psi * cos_psi - 3.0 * cos_psi) / 2.0;
                const double dP3 = (15.0 / 2.0) * cos_psi * cos_psi - (3.0 / 2.0);
                const double scale = mass_ratio * std::pow(a, 5) / std::pow(d, 4);
                return scale * ((h3 * u) * P3 + (l3 * dP3) * t);
            }
        };

        // Step 1: degree 2 (Sun + Moon) + degree 3 (Moon)
        Eigen::Vector3d disp = tideBody(sun_pos, MS2E, 2) + tideBody(moon_pos, MM2E, 2) + tideBody(moon_pos, MM2E, 3);

        // Step 2: frequency-dependent corrections (Tables 7.3a / 7.3b)
        {
            const double t_centuries = TimeUtils::gpsTimeToTtCenturies(time);
            auto fund_args = computeFundamentalArguments(t_centuries);
            Eigen::Vector3d step2 = computeStep2Corrections(fund_args, lat, lambda, u);
            if (step2.allFinite()) disp += step2;
        }

        const double stide = disp.dot(los);
        if (!std::isfinite(stide)) return 0.0;

        log_manager_.getLogger("ppp_model")->trace("Solid Earth tide: {:.4f} m (lat={:.6f} deg)", stide, lat * 180.0 / Constants::PI);
        return stide;
    }

    // =================================================================
    // IERS 2010 Step 2 helpers
    // =================================================================

    std::array<double, 6> GnssCorrections::computeFundamentalArguments(double t_centuries) {
        constexpr double AS2R = Constants::PI / 180.0 / 3600.0;
        const double T = t_centuries;
        const double T2 = T * T, T3 = T2 * T, T4 = T3 * T;

        // Delaunay arguments (IERS 2010, Eq. 5.43-5.47)
        double l = std::fmod(485868.249036 + 1717915923.2178*T + 31.8792*T2
            + 0.051635*T3 - 0.00024470*T4, 1296000.0) * AS2R;
        double lp = std::fmod(1287104.79305 + 129596581.0481*T - 0.5532*T2
            + 0.000136*T3 - 0.00001149*T4, 1296000.0) * AS2R;
        double F = std::fmod(335779.526232 + 1739527262.8478*T - 12.7512*T2
            - 0.001037*T3 + 0.00000417*T4, 1296000.0) * AS2R;
        double D = std::fmod(1072260.70369 + 1602961601.2090*T - 6.3706*T2
            + 0.006593*T3 - 0.00003169*T4, 1296000.0) * AS2R;
        double Omega = std::fmod(450160.398036 - 6962890.5431*T + 7.4722*T2
            + 0.007702*T3 - 0.00005939*T4, 1296000.0) * AS2R;

        // GMST (Capitaine et al. 2003) — use UT1 centuries, not TT
        double JD_UT1_approx = TimeUtils::J2000_JD + (T * TimeUtils::DAYS_PER_JULIAN_CENTURY)
                             - TimeUtils::TT_GPS_OFFSET / static_cast<double>(TimeUtils::SECONDS_IN_DAY);
        double T_UT1 = (JD_UT1_approx - TimeUtils::J2000_JD) / TimeUtils::DAYS_PER_JULIAN_CENTURY;
        double gmst_sec = std::fmod(67310.54841
            + (3155760000.0 + 8640184.812866) * T_UT1
            + 0.093104 * T_UT1 * T_UT1
            - 6.2e-6 * T_UT1 * T_UT1 * T_UT1, 86400.0);
        double gmst = gmst_sec * (2.0 * Constants::PI / 86400.0);

        return { l, lp, F, D, Omega, gmst };
    }

    Eigen::Vector3d GnssCorrections::computeStep2Corrections(
        const std::array<double, 6>& fund_args,
        double /*lat*/, double lon, const Eigen::Vector3d& u)
    {
        const double l     = fund_args[0];
        const double lp    = fund_args[1];
        const double F     = fund_args[2];
        const double D     = fund_args[3];
        const double Omega = fund_args[4];
        const double gmst  = fund_args[5];

        // Geocentric latitude from unit radial vector
        const double sin_lat = u.z();
        const double cos_lat = std::sqrt(u.x() * u.x() + u.y() * u.y());
        const double sin_2lat = 2.0 * sin_lat * cos_lat;
        const double cos_2lat = cos_lat * cos_lat - sin_lat * sin_lat;

        // Doodson variables from Delaunay arguments
        const double s_dood  = F + Omega;
        const double h_dood  = s_dood - D;
        const double p_dood  = s_dood - l;
        const double Np_dood = -Omega;
        const double ps_dood = h_dood + lp;
        const double tau     = gmst + Constants::PI - s_dood;

        auto tidalArg = [&](int n1, int n2, int n3, int n4, int n5, int n6) -> double {
            return n1*tau + n2*s_dood + n3*h_dood + n4*p_dood + n5*Np_dood + n6*ps_dood;
        };

        // -----------------------------------------------------------------
        // IERS 2010 Table 7.3a/7.3b constituent tables
        // Columns: Doodson [tau,s,h,p,N',ps], dR_ip, dR_op, dT_ip, dT_op [mm]
        // -----------------------------------------------------------------
        struct Entry { int n[6]; double dR_ip, dR_op, dT_ip, dT_op; };

        // Long-period (m=0, zonal) — Table 7.3a
        static constexpr Entry LP[] = {
            {{ 0, 0, 0, 0, 0, 0},  0.47,  0.23,  0.16,  0.07},  // 055.555 permanent
            {{ 0, 0, 0, 1, 0, 0}, -0.20, -0.12, -0.07, -0.05},  // Mm
            {{ 0, 0, 0, 2, 0, 0}, -0.11, -0.08, -0.04, -0.03},
            {{ 0, 0, 2,-2, 0, 0}, -0.13, -0.11, -0.05, -0.04},
            {{ 0, 0, 2, 0, 0, 0}, -4.89,  0.00, -1.68,  0.00},  // Ssa (dominant)
            {{ 0, 0, 2, 0, 0,-1},  0.14,  0.00,  0.05,  0.00},
            {{ 0, 1,-2, 1, 0, 0},  0.09,  0.06,  0.03,  0.02},
            {{ 0, 1, 0,-1,-1, 0},  0.10,  0.02,  0.03,  0.01},
            {{ 0, 1, 0,-1, 0, 0},  0.66,  0.12,  0.23,  0.04},  // Mf
            {{ 0, 1, 0,-1, 1, 0},  0.05,  0.01,  0.02,  0.00},
            {{ 0, 1, 0, 1, 0, 0}, -0.14, -0.02, -0.05, -0.01},
            {{ 0, 1, 0, 1, 1, 0}, -0.05,  0.00, -0.02,  0.00},
            {{ 0, 2,-2, 0, 0, 0}, -0.06, -0.03, -0.02, -0.01},
            {{ 0, 2, 0, 0, 0, 0}, -0.06, -0.03, -0.02, -0.01},
            {{ 0, 2, 0, 0, 1, 0}, -0.04, -0.02, -0.01, -0.01},
        };
        static constexpr size_t N_LP = sizeof(LP) / sizeof(LP[0]);

        // Diurnal (m=1) — Table 7.3a
        static constexpr Entry D1[] = {
            {{ 1,-3, 2, 0, 0, 0}, -0.05,  0.00, -0.01,  0.00},
            {{ 1,-2, 2, 0, 0, 0}, -0.06,  0.00, -0.01,  0.00},
            {{ 1,-2, 0, 1,-1, 0}, -0.11,  0.01, -0.02,  0.01},
            {{ 1,-2, 0, 1, 0, 0}, -0.72,  0.06, -0.13,  0.04},  // Q1
            {{ 1,-2, 0, 1, 1, 0}, -0.03,  0.00, -0.01,  0.00},
            {{ 1,-2, 2,-1, 0, 0}, -0.05,  0.00, -0.01,  0.00},  // rho1
            {{ 1,-1, 0, 0,-1, 0},  0.05,  0.00,  0.01,  0.00},
            {{ 1,-1, 0, 0, 0, 0},  0.10, -0.05,  0.02, -0.03},  // O1
            {{ 1,-1, 0, 2, 0, 0}, -0.05,  0.00, -0.01,  0.00},
            {{ 1,-1, 2, 0, 0, 0},  0.07,  0.00,  0.01,  0.00},
            {{ 1, 0, 0,-1, 0, 0}, -1.31, -0.07, -0.24,  0.03},  // M1
            {{ 1, 0, 0, 0, 0, 0}, -0.05,  0.00, -0.01,  0.00},
            {{ 1, 0, 0, 1, 0, 0},  0.25,  0.00,  0.05,  0.00},
            {{ 1, 0, 0, 1, 1, 0},  0.04,  0.00,  0.01,  0.00},
            {{ 1, 0, 2,-1, 0, 0},  0.07,  0.00,  0.01,  0.00},  // chi1
            {{ 1, 1,-3, 0, 0, 1}, -0.72, -0.33, -0.13, -0.19},  // pi1
            {{ 1, 1,-2, 0, 0, 0},-13.20,  0.00, -2.42,  0.00},  // P1
            {{ 1, 1,-2, 0, 0, 1}, -0.05,  0.00, -0.01,  0.00},
            {{ 1, 1,-1, 0, 0,-1}, -0.10,  0.05, -0.02,  0.03},
            {{ 1, 1,-1, 0, 0, 1}, -0.05,  0.00, -0.01,  0.00},
            {{ 1, 1, 0,-2,-1, 0},  0.04, -0.02,  0.01, -0.01},
            {{ 1, 1, 0, 0, 0, 0},  0.10, -0.05,  0.02, -0.03},  // K1
            {{ 1, 1, 0, 2, 0, 0}, -0.07,  0.04, -0.01,  0.02},
        };
        static constexpr size_t N_D1 = sizeof(D1) / sizeof(D1[0]);

        // Semi-diurnal (m=2) — Table 7.3b
        static constexpr Entry S2[] = {
            {{ 2,-1, 0, 1, 0, 0}, -0.02,  0.00,  0.00,  0.00},  // N2
            {{ 2,-1, 2,-1, 0, 0},  0.00,  0.00,  0.00,  0.00},
            {{ 2, 0, 0, 0, 0, 0}, -0.31, -0.07, -0.04, -0.01},  // M2
            {{ 2, 2,-2, 0, 0, 0}, -0.08,  0.00, -0.01,  0.01},  // S2
            {{ 2, 2, 0, 0, 0, 0}, -0.17,  0.00, -0.02,  0.01},  // K2
        };
        static constexpr size_t N_S2 = sizeof(S2) / sizeof(S2[0]);

        // -----------------------------------------------------------------
        // Accumulate displacement in local (radial, north, east) frame
        // -----------------------------------------------------------------
        double dr = 0.0, dn = 0.0, de = 0.0;

        // Precomputed latitude factors
        const double sin2_lat = sin_lat * sin_lat;
        const double lp_radial = (3.0 * sin2_lat - 1.0) / 2.0;
        const double lp_north  = sin_2lat / 2.0;

        // Helper: evaluate a single constituent and accumulate
        auto evalConstituent = [&](const Entry& e, double theta_f, int m) {
            const double cos_tf = std::cos(theta_f);
            const double sin_tf = std::sin(theta_f);
            const double dRip = e.dR_ip * 1e-3, dRop = e.dR_op * 1e-3;
            const double dTip = e.dT_ip * 1e-3, dTop = e.dT_op * 1e-3;
            const double R = dRip * cos_tf + dRop * sin_tf;
            const double Tc = dTip * cos_tf + dTop * sin_tf;
            const double Ts = dTip * sin_tf - dTop * cos_tf;

            switch (m) {
            case 0:
                dr += R * lp_radial;
                dn += Tc * lp_north;
                break;
            case 1:
                dr += R * sin_2lat;
                dn += Tc * cos_2lat;
                de -= Ts * sin_lat;
                break;
            case 2:
                dr += R * cos_lat * cos_lat;
                dn -= Tc * sin_2lat / 2.0;
                de -= Ts * cos_lat;
                break;
            }
        };

        for (size_t i = 0; i < N_LP; ++i) {
            double theta = tidalArg(LP[i].n[0], LP[i].n[1], LP[i].n[2], LP[i].n[3], LP[i].n[4], LP[i].n[5]);
            evalConstituent(LP[i], theta, 0);
        }
        for (size_t i = 0; i < N_D1; ++i) {
            double theta = tidalArg(D1[i].n[0], D1[i].n[1], D1[i].n[2], D1[i].n[3], D1[i].n[4], D1[i].n[5]) + lon;
            evalConstituent(D1[i], theta, 1);
        }
        for (size_t i = 0; i < N_S2; ++i) {
            double theta = tidalArg(S2[i].n[0], S2[i].n[1], S2[i].n[2], S2[i].n[3], S2[i].n[4], S2[i].n[5]) + 2.0 * lon;
            evalConstituent(S2[i], theta, 2);
        }

        // Local (r,n,e) -> ECEF (geocentric frame consistent with u)
        const double cos_lon = std::cos(lon), sin_lon = std::sin(lon);
        const double slgc = sin_lat, clgc = cos_lat; // geocentric
        Eigen::Vector3d e_north(-slgc * cos_lon, -slgc * sin_lon, clgc);
        Eigen::Vector3d e_east(-sin_lon, cos_lon, 0.0);

        return dr * u + dn * e_north + de * e_east;
    }

    // =================================================================
    // Bias retrieval
    // =================================================================

    double GnssCorrections::getBiasCorrection(const std::string& satellite,
        const std::string& freq_type,
        const GnssTime& time) const {
        auto log = log_manager_.getLogger("ppp_model");

        if (satellite.empty() || freq_type.empty()) {
            log->warn("Invalid bias inputs: sat='{}', freq='{}' at TOW {:.3f}", satellite, freq_type, time.tow);
            return 0.0;
        }
        if (time.tow < 0.0 || time.week < 0) {
            log->warn("Invalid time for bias for {}: week {}, TOW {:.3f}", satellite, time.week, time.tow);
            return 0.0;
        }

        const double t_query = TimeUtils::continuousGpsSeconds(time);

        auto it = std::find_if(biases_.begin(), biases_.end(),
            [&satellite, &freq_type, t_query](const BiasData& b) {
                if (b.satellite != satellite || b.signal != freq_type) return false;
                double t_start = TimeUtils::continuousGpsSeconds(b.start_time);
                double t_end = TimeUtils::continuousGpsSeconds(b.end_time);
                return (t_start <= t_query && t_query <= t_end);
            });

        if (it != biases_.end()) {
            log->trace("Bias for {} ({}) {} at TOW {:.3f}: {:.3e} m",
                satellite, PppUtils::getSystemString(satellite), freq_type, time.tow, it->bias);
            return it->bias;
        }

        if (!biases_.empty())
            log->debug("No bias entry for {} {} at t={:.3f}, {} entries total", satellite, freq_type, t_query, biases_.size());

        log->trace("No bias for {} ({}) {} at TOW {:.3f}",
            satellite, PppUtils::getSystemString(satellite), freq_type, time.tow);
        return 0.0;
    }

    // =================================================================
    // Phase wind-up
    // =================================================================

    double GnssCorrections::computePhaseWindUp(const std::string& satellite,
        System system,
        const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos,
        double wavelength,
        const GnssTime& time,
        const Eigen::Vector3d& sun_pos,
        const Eigen::Vector3d& sat_vel,
        bool accumulate) const {

        // Wu et al. (1993): k = sat->rx, D = x - k(k.x) + k x y
        Eigen::Vector3d k_raw = rx_pos - sat_pos;
        double k_raw_norm = k_raw.norm();
        if (k_raw_norm < 1e-3) {
            log_manager_.getLogger("ppp_model")->trace("Phase wind-up for {}: degenerate LOS", satellite);
            return 0.0;
        }
        Eigen::Vector3d k = k_raw / k_raw_norm;

        // --- Satellite body frame ---
        Eigen::Vector3d ez = -sat_pos.normalized();
        Eigen::Vector3d x_s, y_s;

        // BeiDou-2 GEO (C01-C05, C59, C60): orbit-normal mode
        bool use_orbit_normal = false;
        if (system == System::BeiDou && satellite.size() >= 2) {
            int prn = 0;
            try { prn = std::stoi(satellite.substr(1)); } catch (...) {}
            if ((prn >= 1 && prn <= 5) || prn == 59 || prn == 60)
                use_orbit_normal = true;
        }

        if (use_orbit_normal) {
            Eigen::Vector3d orbit_normal = sat_pos.cross(sat_vel);
            if (orbit_normal.norm() < 1e-8) orbit_normal = Eigen::Vector3d::UnitY();
            y_s = orbit_normal.normalized();
            x_s = y_s.cross(ez);
            if (x_s.norm() < 1e-8) x_s = Eigen::Vector3d::UnitX();
            x_s.normalize();
            y_s = ez.cross(x_s);
            y_s.normalize();
        }
        else if (sun_pos.norm() > 1e6) {
            // Standard yaw-steering
            Eigen::Vector3d e_sun = (sun_pos - sat_pos).normalized();
            Eigen::Vector3d ey = ez.cross(e_sun);
            if (ey.norm() < 1e-8) {
                Eigen::Vector3d vhat = sat_vel.norm() > 1e-8 ? sat_vel.normalized() : Eigen::Vector3d::UnitY();
                ey = ez.cross(vhat);
            }
            ey.normalize();
            Eigen::Vector3d ex = ey.cross(ez);
            if (ex.norm() < 1e-8) ex = Eigen::Vector3d::UnitX();
            ex.normalize();
            x_s = ex;
            y_s = ey;
        } else {
            // Velocity-based fallback
            Eigen::Vector3d z_s = -ez;
            Eigen::Vector3d vel = sat_vel;
            if (vel.norm() < 1e-8 && !orbits_.empty()) {
                auto orbit_it = std::find_if(orbits_.begin(), orbits_.end(),
                    [&satellite](const OrbitData& o) { return o.satellite == satellite; });
                if (orbit_it != orbits_.end() && orbit_it->velocity_valid)
                    vel = orbit_it->velocity;
            }
            y_s = vel.norm() > 1e-8 ? vel.normalized().cross(z_s) : Eigen::Vector3d::Zero();
            if (y_s.norm() < 1e-8) y_s = Eigen::Vector3d(0, 1, 0);
            y_s.normalize();
            x_s = y_s.cross(z_s);
            if (x_s.norm() < 1e-8) x_s = Eigen::Vector3d::UnitX();
            x_s.normalize();
        }

        // --- Receiver local frame (North / East) ---
        if (rx_pos.norm() < 1e3) return 0.0;
        Coordinates::Llh rx_llh = Coordinates::ecefToLlh(rx_pos);
        double phi = rx_llh.lat_rad, lam = rx_llh.lon_rad;
        Eigen::Vector3d x_r(-std::sin(phi)*std::cos(lam), -std::sin(phi)*std::sin(lam), std::cos(phi));
        Eigen::Vector3d y_r(-std::sin(lam), std::cos(lam), 0.0);

        // Effective dipoles
        Eigen::Vector3d Ds = x_s - k * (k.dot(x_s)) + k.cross(y_s);
        Eigen::Vector3d Dr = x_r - k * (k.dot(x_r)) + k.cross(y_r);
        double Ds_norm = Ds.norm(), Dr_norm = Dr.norm();
        if (Ds_norm < 1e-10 || Dr_norm < 1e-10) {
            log_manager_.getLogger("ppp_model")->trace("Phase wind-up for {}: degenerate dipole", satellite);
            return 0.0;
        }

        // Compute signed angle between effective dipoles using atan2 for
        // full (-π, π] range.  This avoids the acos + sign approach which
        // loses information near 0 and π.
        Eigen::Vector3d Ds_hat = Ds / Ds_norm, Dr_hat = Dr / Dr_norm;
        double windUp_c = std::atan2(k.dot(Ds_hat.cross(Dr_hat)), Ds_hat.dot(Dr_hat));

        // Accumulated mode: unwrap relative to previous epoch for continuity.
        // Required for IAR where ambiguities must remain constant integers.
        // Fractional mode (default): raw atan2 angle; float ambiguity absorbs
        // integer-cycle component. See header for full design discussion.
        if (accumulate) {
            constexpr double TWO_PI = 2.0 * Constants::PI;
            auto pw_it = prev_windup_.find(satellite);
            if (pw_it != prev_windup_.end()) {
                double diff = windUp_c - pw_it->second;
                diff -= TWO_PI * std::round(diff / TWO_PI);
                windUp_c = pw_it->second + diff;
            }
        }

        prev_windup_[satellite] = windUp_c;

        double phi_m = windUp_c * wavelength / (2.0 * Constants::PI);
        log_manager_.getLogger("ppp_model")->trace("Phase wind-up for {}: {:.6f} m ({:.3f} rad, {})",
            satellite, phi_m, windUp_c, accumulate ? "accumulated" : "fractional");
        return phi_m;
    }

    void GnssCorrections::resetPhaseWindUp(const std::string& satellite) {
        auto it = prev_windup_.find(satellite);
        if (it != prev_windup_.end()) {
            log_manager_.getLogger("corrections")->debug("Reset phase wind-up for {}: prev={:.3f} rad", satellite, it->second);
            prev_windup_.erase(it);
        }
    }

    void GnssCorrections::resetAllPhaseWindUp() {
        if (!prev_windup_.empty()) {
            log_manager_.getLogger("corrections")->debug("Reset phase wind-up for all {} satellites", prev_windup_.size());
            prev_windup_.clear();
        }
    }

} // namespace GnssProcessor