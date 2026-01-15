#include "GnssCorrections.hpp"
#include "Constants.hpp"
#include "GnssConfig.hpp"    // required for accessing ReceiverAntennaOrientation in .cpp
#include "RinexParser.hpp"   // required to call RinexParser methods if provided
#include <algorithm>
#include <cmath>
#include <numeric>

namespace GnssProcessor {

    // Minimal constructor
    GnssCorrections::GnssCorrections(LogManager& log_mgr)
        : log_manager_(log_mgr),
        clocks_(), erps_(), biases_(), orbits_(),
        config_(nullptr), rinex_parser_(nullptr) {
        log_manager_.getLogger("corrections")->trace("GnssCorrections constructed (minimal)");
    }

    // Constructor with clocks and ERPs
    GnssCorrections::GnssCorrections(LogManager& log_mgr,
        const std::vector<ClockData>& clocks,
        const std::vector<ErpData>& erps)
        : log_manager_(log_mgr),
        clocks_(clocks), erps_(erps), biases_(), orbits_(),
        config_(nullptr), rinex_parser_(nullptr) {
        log_manager_.getLogger("corrections")->trace("GnssCorrections constructed with clocks and ERPs: {} clocks, {} erps",
            clocks_.size(), erps_.size());
    }

    // Full constructor with optional config, biases and rinex parser
    GnssCorrections::GnssCorrections(LogManager& log_mgr,
        const GnssConfig& config,
        const std::vector<ClockData>& clocks,
        const std::vector<ErpData>& erps,
        const std::vector<BiasData>& biases,
        const RinexParser& rinex)
        : log_manager_(log_mgr),
        clocks_(clocks), erps_(erps), biases_(biases), orbits_(),
        config_(&config), rinex_parser_(&rinex) {
        log_manager_.getLogger("corrections")->trace("GnssCorrections constructed (full) with {} clocks, {} erps, {} biases",
            clocks_.size(), erps_.size(), biases_.size());
    }

    double GnssCorrections::getSatelliteClockCorrection(const std::string& satellite, const GnssTime& time) const {
        // Validate inputs
        if (satellite.empty()) {
            log_manager_.getLogger("ppp_model")->warn("Empty satellite ID for clock correction at TOW {:.3f}", time.tow);
            return 0.0;
        }
        if (time.tow < 0.0 || time.week < 0) {
            log_manager_.getLogger("ppp_model")->warn("Invalid time for clock correction for satellite {}: week {}, TOW {:.3f}",
                satellite, time.week, time.tow);
            return 0.0;
        }
        if (clocks_.empty()) {
            log_manager_.getLogger("ppp_model")->warn("No clock data available for satellite {} at TOW {:.3f}", satellite, time.tow);
            return 0.0;
        }

        // Interpolate clock correction
        double clock_corr = interpolateClock(satellite, time, clocks_);
        std::string system_str = satellite.substr(0, 1) == "G" ? "GPS" : satellite.substr(0, 1) == "E" ? "Galileo" : "Unknown";
        if (std::isfinite(clock_corr) && std::abs(clock_corr) < 1e-3) { // Max 1 ms
            log_manager_.getLogger("ppp_model")->trace("Interpolated clock correction for {} ({}) at TOW {:.3f}: {:.3e} s",
                satellite, system_str, time.tow, clock_corr);
            return clock_corr;
        }

        log_manager_.getLogger("ppp_model")->warn("Invalid clock correction for {} ({}) at TOW {:.3f}: {:.3e} s",
            satellite, system_str, time.tow, clock_corr);
        return 0.0;
    }

    double GnssCorrections::interpolateClock(const std::string& satellite, const GnssTime& time,
        const std::vector<ClockData>& clocks) const {
        // Robust piecewise-linear interpolation using the two bracketing epochs.
        // Avoids polynomial oscillations and is stable near gaps / boundaries.
        constexpr double MAX_WINDOW = 7200.0; // seconds (reject far-away candidates)

        auto fullSeconds = [](const GnssTime& t) {
            return static_cast<double>(t.week) * 604800.0 + t.tow;
            };

        if (satellite.empty() || clocks.empty()) {
            return 0.0;
        }

        const double tq = fullSeconds(time);

        const ClockData* before = nullptr; // latest tc <= tq
        const ClockData* after = nullptr;  // earliest tc >= tq
        double best_before_dt = std::numeric_limits<double>::infinity();
        double best_after_dt = std::numeric_limits<double>::infinity();

        for (const auto& c : clocks) {
            if (c.satellite != satellite) continue;
            if (!std::isfinite(c.clock_bias)) continue;

            const double tc = fullSeconds(c.time);
            const double dt = tc - tq;

            if (std::abs(dt) > MAX_WINDOW) continue;

            if (std::abs(dt) < 1e-12) {
                // Exact hit
                return c.clock_bias;
            }

            if (dt < 0.0) {
                const double d = -dt;
                if (d < best_before_dt) {
                    best_before_dt = d;
                    before = &c;
                }
            }
            else {
                const double d = dt;
                if (d < best_after_dt) {
                    best_after_dt = d;
                    after = &c;
                }
            }
        }

        if (!before && !after) {
            return 0.0;
        }

        // Nearest-neighbor fallback if we can't bracket.
        if (before && !after) return before->clock_bias;
        if (!before && after) return after->clock_bias;

        const double t0 = fullSeconds(before->time);
        const double t1 = fullSeconds(after->time);
        const double y0 = before->clock_bias;
        const double y1 = after->clock_bias;

        const double denom = t1 - t0;
        if (!std::isfinite(denom) || std::abs(denom) < 1e-9) {
            return std::isfinite(y0) ? y0 : 0.0;
        }

        const double w = (tq - t0) / denom;
        const double clk = y0 + w * (y1 - y0);

        return std::isfinite(clk) ? clk : (std::isfinite(y0) ? y0 : 0.0);
    }

    double GnssCorrections::computeRelativisticCorrection(const std::string& satellite, const Eigen::Vector3d& sat_pos,
        const Eigen::Vector3d& sat_vel) const {
        // GNSS relativistic correction used in PPP is the well-known term:
        //
        //   dtr = -2 * (r · v) / c^2   [seconds]
        //
        // where r and v are the satellite ECEF position/velocity at transmit time.
        // Do NOT add a "Schwarzschild / -2GM/(c^2 r)" term here; that is not applied as a per-epoch correction in
        // standard GNSS PPP observation modeling (it is already absorbed by clock/product definitions and constants).
        constexpr double C_SQUARED = Constants::SPEED_OF_LIGHT * Constants::SPEED_OF_LIGHT;

        // Conservative sanity limits
        constexpr double MAX_CORRECTION = 1e-6;        // 1 microsecond (~300 m), far above expected (~tens of ns)
        constexpr double MIN_POSITION_NORM = 1.8e7;    // ~20,000 km
        constexpr double MAX_POSITION_NORM = 3.2e7;    // ~32,000 km
        constexpr double MAX_VELOCITY_NORM = 1.5e4;    // ~15 km/s

        std::string system_str = satellite.substr(0, 1) == "G" ? "GPS" : satellite.substr(0, 1) == "E" ? "Galileo" : "Unknown";

        if (!sat_pos.allFinite() || !sat_vel.allFinite()) {
            log_manager_.getLogger("ppp_model")->warn(
                "Invalid relativistic inputs for {} ({}): sat_pos=({:.3f}, {:.3f}, {:.3f}), sat_vel=({:.3f}, {:.3f}, {:.3f})",
                satellite, system_str, sat_pos.x(), sat_pos.y(), sat_pos.z(), sat_vel.x(), sat_vel.y(), sat_vel.z());
            return 0.0;
        }

        const double pos_norm = sat_pos.norm();
        const double vel_norm = sat_vel.norm();
        if (pos_norm < MIN_POSITION_NORM || pos_norm > MAX_POSITION_NORM || vel_norm > MAX_VELOCITY_NORM) {
            log_manager_.getLogger("ppp_model")->warn(
                "Unrealistic relativistic input norms for {} ({}): pos_norm={:.3e} m, vel_norm={:.3e} m/s",
                satellite, system_str, pos_norm, vel_norm);
            return 0.0;
        }

        const double r_dot_v = sat_pos.dot(sat_vel);
        const double correction = -2.0 * r_dot_v / C_SQUARED;

        if (!std::isfinite(correction) || std::abs(correction) > MAX_CORRECTION) {
            log_manager_.getLogger("ppp_model")->warn(
                "Excessive relativistic correction for {} ({}): {:.3e} s, r_dot_v={:.3e}",
                satellite, system_str, correction, r_dot_v);
            return 0.0;
        }

        log_manager_.getLogger("ppp_model")->trace(
            "Relativistic correction for {} ({}): {:.3e} s, r_dot_v={:.3e}",
            satellite, system_str, correction, r_dot_v);

        return correction;
    }

    double GnssCorrections::computeShapiroDelay(const std::string& satellite, const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos) const {
        constexpr double GM = Constants::GRAVITATIONAL_CONSTANT; // GM = G * M_Earth (m^3/s^2)
        constexpr double C_CUBED = Constants::SPEED_OF_LIGHT * Constants::SPEED_OF_LIGHT * Constants::SPEED_OF_LIGHT;
        constexpr double MIN_POSITION_NORM = 1.8e7; // approx. 20,000 km
        constexpr double MAX_POSITION_NORM = 3.2e7; // approx. 32,000 km
        constexpr double MAX_DELAY = 1e-4; // Max 0.1 ms

        // Validate inputs
        std::string system_str = satellite.substr(0, 1) == "G" ? "GPS" : satellite.substr(0, 1) == "E" ? "Galileo" : "Unknown";
        if (!rx_pos.allFinite() || !sat_pos.allFinite()) {
            log_manager_.getLogger("ppp_model")->warn("Invalid Shapiro delay inputs for {} ({}): rx_pos=({:.3f}, {:.3f}, {:.3f}), sat_pos=({:.3f}, {:.3f}, {:.3f})",
                satellite, system_str, rx_pos.x(), rx_pos.y(), rx_pos.z(), sat_pos.x(), sat_pos.y(), sat_pos.z());
            return 0.0;
        }
        double r = (sat_pos - rx_pos).norm();
        double sat_norm = sat_pos.norm();
        if (r < 1e3 || sat_norm < MIN_POSITION_NORM || sat_norm > MAX_POSITION_NORM) {
            log_manager_.getLogger("ppp_model")->warn("Unrealistic Shapiro delay norms for {} ({}): range={:.3e} m, sat_norm={:.3e} m",
                satellite, system_str, r, sat_norm);
            return 0.0;
        }

        // Compute Shapiro delay: (2GM/c^3) * ln((r + r_s + r_r)/(r + r_s - r_r))
        double r_rx = rx_pos.norm();
        double delay = (2.0 * GM / C_CUBED) * std::log((r + sat_norm + r_rx) / (r + sat_norm - r_rx));
        if (!std::isfinite(delay) || std::abs(delay) > MAX_DELAY) {
            log_manager_.getLogger("ppp_model")->warn("Invalid Shapiro delay for {} ({}): {:.3e} s, range={:.3e} m, sat_norm={:.3e} m, rx_norm={:.3e} m",
                satellite, system_str, delay, r, sat_norm, r_rx);
            return 0.0;
        }

        // Convert to meters
        delay *= Constants::SPEED_OF_LIGHT;
        log_manager_.getLogger("ppp_model")->trace("Shapiro delay for {} ({}): {:.3e} m, range={:.3e} m, sat_norm={:.3e} m, rx_norm={:.3e} m",
            satellite, system_str, delay, r, sat_norm, r_rx);
        return delay;
    }

    double GnssCorrections::computeSagnacCorrection(const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos) const {
        // Sagnac effect: Correction for Earth's rotation during signal propagation
        // Formula: (ω_E / c) * (x_s * y_r - y_s * x_r), where ω_E is Earth's angular velocity, c is speed of light
        constexpr double EARTH_ANGULAR_VELOCITY = 7.2921151467e-5; // rad/s
        constexpr double C = Constants::SPEED_OF_LIGHT; // m/s
        constexpr double MAX_SAGNAC_CORRECTION = 50.0; // Max expected correction in meters
        constexpr double MIN_RX_NORM = 6e6; // Earth radius approx. 6.3e6 m
        constexpr double MAX_RX_NORM = 7e6;
        constexpr double MIN_SAT_NORM = 2e7; // Typical GNSS orbit approx. 2e7 m
        constexpr double MAX_SAT_NORM = 3e7;

        // Validate inputs
        if (!rx_pos.allFinite() || !sat_pos.allFinite()) {
            log_manager_.getLogger("ppp_model")->warn("Invalid Sagnac inputs: rx_pos=({}, {}, {}), sat_pos=({}, {}, {})",
                rx_pos.x(), rx_pos.y(), rx_pos.z(), sat_pos.x(), sat_pos.y(), sat_pos.z());
            return 0.0;
        }
        double rx_norm = rx_pos.norm();
        double sat_norm = sat_pos.norm();
        if (rx_norm < MIN_RX_NORM || rx_norm > MAX_RX_NORM || sat_norm < MIN_SAT_NORM || sat_norm > MAX_SAT_NORM) {
            log_manager_.getLogger("ppp_model")->warn("Unrealistic Sagnac input norms: rx_norm={:.3e} m, sat_norm={:.3e} m, "
                "rx_pos=({}, {}, {}), sat_pos=({}, {}, {})",
                rx_norm, sat_norm, rx_pos.x(), rx_pos.y(), rx_pos.z(), sat_pos.x(), sat_pos.y(), sat_pos.z());
            return 0.0;
        }

        // Compute correction: (ω_E / c) * (x_s * y_r - y_s * x_r)
        double cross_term = sat_pos.x() * rx_pos.y() - sat_pos.y() * rx_pos.x();
        double correction = (EARTH_ANGULAR_VELOCITY / C) * cross_term;
        if (!std::isfinite(correction) || std::abs(correction) > MAX_SAGNAC_CORRECTION) {
            log_manager_.getLogger("ppp_model")->warn("Excessive Sagnac correction: {:.3e} m, cross_term: {:.3e}, "
                "rx_pos=({}, {}, {}), sat_pos=({}, {}, {})",
                correction, cross_term, rx_pos.x(), rx_pos.y(), rx_pos.z(), sat_pos.x(), sat_pos.y(), sat_pos.z());
            return 0.0;
        }

        log_manager_.getLogger("ppp_model")->trace("Sagnac correction: {:.3f} m, cross_term: {:.3e}, "
            "rx_pos=({}, {}, {}), sat_pos=({}, {}, {})",
            correction, cross_term, rx_pos.x(), rx_pos.y(), rx_pos.z(), sat_pos.x(), sat_pos.y(), sat_pos.z());
        return correction;
    }

    Eigen::Vector3d GnssCorrections::computePoleTideCorrection(const Eigen::Vector3d& rx_pos, const GnssTime& time) const {
        // Validate inputs
        if (!rx_pos.allFinite() || rx_pos.norm() < 1e3) {
            log_manager_.getLogger("ppp_model")->warn("Invalid receiver position for pole tide correction at TOW {:.3f}: rx_pos=({:.3f}, {:.3f}, {:.3f}), norm={:.3e}",
                time.tow, rx_pos.x(), rx_pos.y(), rx_pos.z(), rx_pos.norm());
            return Eigen::Vector3d::Zero();
        }
        if (time.tow < 0.0 || time.week < 0) {
            log_manager_.getLogger("ppp_model")->warn("Invalid time for pole tide correction: week {}, TOW {:.3f}",
                time.week, time.tow);
            return Eigen::Vector3d::Zero();
        }
        if (erps_.empty()) {
            log_manager_.getLogger("ppp_model")->warn("No ERP data available for pole tide correction at TOW {:.3f}", time.tow);
            return Eigen::Vector3d::Zero();
        }

        // Find closest ERP data
        auto it = std::min_element(erps_.begin(), erps_.end(),
            [&time](const ErpData& a, const ErpData& b) {
                return std::abs(a.time.tow - time.tow) < std::abs(b.time.tow - time.tow);
            });
        if (it == erps_.end() || !std::isfinite(it->xp) || !std::isfinite(it->yp)) {
            log_manager_.getLogger("ppp_model")->trace("No valid ERP data for pole tide correction at TOW {:.3f}", time.tow);
            return Eigen::Vector3d::Zero();
        }

        // Compute pole tide correction (simplified model, IERS 2010 conventions)
        double phi = std::asin(rx_pos.z() / rx_pos.norm()); // Geodetic latitude (radians)
        double lambda = std::atan2(rx_pos.y(), rx_pos.x()); // Geodetic longitude (radians)
        double xp = it->xp * 4.848136811e-6; // Convert arcsec to radians
        double yp = it->yp * 4.848136811e-6; // Convert arcsec to radians

        // Pole tide displacement in radial, north, east directions (meters)
        double R = rx_pos.norm();
        double radial = -0.033 * std::sin(2.0 * phi) * (xp * std::cos(lambda) + yp * std::sin(lambda));
        double north = -0.009 * std::cos(2.0 * phi) * (xp * std::sin(lambda) - yp * std::cos(lambda));
        double east = -0.009 * std::sin(phi) * (xp * std::cos(lambda) + yp * std::sin(lambda));

        // Convert to ECEF coordinates
        Eigen::Vector3d correction(
            (radial * std::cos(phi) * std::cos(lambda) - north * std::sin(phi) * std::cos(lambda) - east * std::sin(lambda)),
            (radial * std::cos(phi) * std::sin(lambda) - north * std::sin(phi) * std::sin(lambda) + east * std::cos(lambda)),
            (radial * std::sin(phi) + north * std::cos(phi))
        );

        if (!correction.allFinite() || correction.norm() > 0.1) { // Max 10 cm
            log_manager_.getLogger("ppp_model")->warn("Invalid pole tide correction at TOW {:.3f}: ({:.3e}, {:.3e}, {:.3e}) m, norm={:.3e}",
                time.tow, correction.x(), correction.y(), correction.z(), correction.norm());
            return Eigen::Vector3d::Zero();
        }

        log_manager_.getLogger("ppp_model")->trace("Pole tide correction at TOW {:.3f}: ({:.3e}, {:.3e}, {:.3e}) m, xp={:.3e} rad, yp={:.3e} rad",
            time.tow, correction.x(), correction.y(), correction.z(), xp, yp);
        return correction;
    }

    Eigen::Vector3d GnssCorrections::computeOceanTideCorrection(const Eigen::Vector3d& rx_pos, const GnssTime& time) const {
        // Validate inputs
        if (!rx_pos.allFinite() || rx_pos.norm() < 1e3) {
            log_manager_.getLogger("ppp_model")->warn("Invalid receiver position for ocean tide correction at TOW {:.3f}: rx_pos=({:.3f}, {:.3f}, {:.3f}), norm={:.3e}",
                time.tow, rx_pos.x(), rx_pos.y(), rx_pos.z(), rx_pos.norm());
            return Eigen::Vector3d::Zero();
        }
        if (time.tow < 0.0 || time.week < 0) {
            log_manager_.getLogger("ppp_model")->warn("Invalid time for ocean tide correction: week {}, TOW {:.3f}",
                time.week, time.tow);
            return Eigen::Vector3d::Zero();
        }

        // Placeholder: Simplified ocean tide correction (requires external tidal model data)
        // Using a basic sinusoidal model based on position and time
        double phi = std::asin(rx_pos.z() / rx_pos.norm()); // Geodetic latitude (radians)
        double t = std::fmod(time.tow, 86400.0); // Seconds of day
        double amplitude = 0.01; // Typical ocean tide amplitude: 1 cm
        double period = 12.4206 * 3600.0; // M2 tidal constituent period (s)
        double radial = amplitude * std::cos(2.0 * Constants::PI * t / period) * std::cos(phi);
        double north = amplitude * std::sin(2.0 * Constants::PI * t / period) * std::sin(phi);
        double east = 0.0; // Simplified, no east component

        // Convert to ECEF coordinates
        double lambda = std::atan2(rx_pos.y(), rx_pos.x());
        Eigen::Vector3d correction(
            (radial * std::cos(phi) * std::cos(lambda) - north * std::sin(phi) * std::cos(lambda)),
            (radial * std::cos(phi) * std::sin(lambda) - north * std::sin(phi) * std::sin(lambda)),
            (radial * std::sin(phi) + north * std::cos(phi))
        );

        if (!correction.allFinite() || correction.norm() > 0.1) { // Max 10 cm
            log_manager_.getLogger("ppp_model")->warn("Invalid ocean tide correction at TOW {:.3f}: ({:.3e}, {:.3e}, {:.3e}) m, norm={:.3e}",
                time.tow, correction.x(), correction.y(), correction.z(), correction.norm());
            return Eigen::Vector3d::Zero();
        }

        log_manager_.getLogger("ppp_model")->trace("Ocean tide correction at TOW {:.3f}: ({:.3e}, {:.3e}, {:.3e}) m, amplitude={:.3e} m",
            time.tow, correction.x(), correction.y(), correction.z(), amplitude);
        return correction;
    }

    double GnssCorrections::computeSolidEarthTide(const Eigen::Vector3d& receiver_pos, const Eigen::Vector3d& sat_pos,
        const Eigen::Vector3d& sun_pos, const Eigen::Vector3d& moon_pos) const {
        // IERS 2010: Solid Earth tide (degree 2 + degree 3 terms).
        //
        // This implementation uses:
        //  1) WGS-84 geodetic latitude (not geocentric latitude derived from ECEF norm)
        //  2) The standard IERS 2010 approach (zero-frequency free-space Love numbers: h2,l2 and h3,l3)
        //
        // Output is the displacement projection onto the receiver->satellite line-of-sight [m].

        // IERS 2010 nominal Love numbers (latitude-dependent corrections omitted here; still a big improvement)
        constexpr double h2 = 0.6078;
        constexpr double l2 = 0.0847;
        constexpr double h3 = 0.292;
        constexpr double l3 = 0.015;

        // Mass ratios (IERS 2010): (M_sun / M_earth), (M_moon / M_earth)
        constexpr double MS2E = 332946.0;
        constexpr double MM2E = 0.01230002;

        // WGS-84 ellipsoid
        constexpr double a = 6378137.0;                 // semi-major [m]
        constexpr double f = 1.0 / 298.257223563;       // flattening
        constexpr double e2 = f * (2.0 - f);            // first eccentricity squared

        if (!receiver_pos.allFinite() || !sat_pos.allFinite() || !sun_pos.allFinite() || !moon_pos.allFinite()) {
            log_manager_.getLogger("ppp_model")->warn("Solid Earth tide: non-finite inputs");
            return 0.0;
        }

        const Eigen::Vector3d rho = sat_pos - receiver_pos;
        const double rho_norm = rho.norm();
        if (!(rho_norm > 0.0)) return 0.0;
        const Eigen::Vector3d los = rho / rho_norm;

        const double r = receiver_pos.norm();
        if (!(r > 0.0)) return 0.0;

        // --- Geodetic latitude from ECEF (iterative, stable) ---
        const double x = receiver_pos.x();
        const double y = receiver_pos.y();
        const double z = receiver_pos.z();
        const double p = std::sqrt(x * x + y * y);

        if (!(p > 0.0)) return 0.0;

        double lat = std::atan2(z, p * (1.0 - e2)); // initial guess
        for (int i = 0; i < 5; ++i) {
            const double sin_lat = std::sin(lat);
            const double N = a / std::sqrt(1.0 - e2 * sin_lat * sin_lat);
            // height not strictly required; improves convergence
            const double h = p / std::cos(lat) - N;
            const double lat_next = std::atan2(z, p * (1.0 - e2 * N / (N + h)));
            if (std::abs(lat_next - lat) < 1e-13) {
                lat = lat_next;
                break;
            }
            lat = lat_next;
        }

        const double sin_lat = std::sin(lat);
        const double cos_lat = std::cos(lat);
        const double trm = 3.0 * sin_lat * sin_lat - 1.0;

        // Small latitude-dependent corrections (IERS 2010, simplified)
        const double dh = -0.0006 * trm;
        const double dl =  0.0002 * trm;

        const double h2_eff = h2 + dh;
        const double l2_eff = l2 + dl;

        // Receiver unit vector
        const Eigen::Vector3d u = receiver_pos / r;

        auto tideBody = [&](const Eigen::Vector3d& body_pos, double mass_ratio, int deg) -> Eigen::Vector3d {
            const double d = body_pos.norm();
            if (!(d > 0.0)) return Eigen::Vector3d::Zero();

            const Eigen::Vector3d ub = body_pos / d;
            const double cos_psi = ub.dot(u);

            if (deg == 2) {
                // Degree-2 terms: P2 = (3 cos^2 - 1)/2
                const double P2 = (3.0 * cos_psi * cos_psi - 1.0) / 2.0;

                // Tangential direction component
                const Eigen::Vector3d t = ub - cos_psi * u;

                // Scale factor: (Re^4 / d^3) * (M_body/M_earth)
                const double scale = mass_ratio * std::pow(a, 4) / std::pow(d, 3);

                // IERS 2010: displacement = h2 * u * P2 + 3*l2*cosψ*t
                return scale * ((h2_eff * u) * P2 + (3.0 * l2_eff * cos_psi) * t);
            }
            else {
                // Degree-3 terms: P3 = (5 cos^3 - 3 cos)/2, dP3/dψ -> (5/2 cos^2 - 3/2)
                const double P3 = (5.0 * cos_psi * cos_psi * cos_psi - 3.0 * cos_psi) / 2.0;
                const double dP3 = (5.0 / 2.0) * cos_psi * cos_psi - (3.0 / 2.0);

                const Eigen::Vector3d t = ub - cos_psi * u;

                // Scale factor: (Re^5 / d^4) * (M_body/M_earth)
                const double scale = mass_ratio * std::pow(a, 5) / std::pow(d, 4);

                // IERS 2010: displacement = h3*u*P3 + l3*dP3*t
                return scale * ((h3 * u) * P3 + (l3 * dP3) * t);
            }
        };

        const Eigen::Vector3d d2s = tideBody(sun_pos, MS2E, 2);
        const Eigen::Vector3d d2m = tideBody(moon_pos, MM2E, 2);
        const Eigen::Vector3d d3m = tideBody(moon_pos, MM2E, 3); // IERS: degree-3 usually moon-only in simplified PPP implementations

        const Eigen::Vector3d disp = d2s + d2m + d3m;

        const double stide = disp.dot(los);
        if (!std::isfinite(stide)) return 0.0;

        // Keep logging lightweight; this is per-sat per-epoch
        log_manager_.getLogger("ppp_model")->trace("Solid Earth tide: {:.4f} m (lat_geod={:.6f} deg)", stide, lat * 180.0 / Constants::PI);

        return stide;
    }

    double GnssCorrections::computeAtmosphericLoadingCorrection(const Eigen::Vector3d& rx_pos,
        const GnssTime& time) const {
        // Placeholder: Simplified atmospheric loading correction
        // Model: Small sinusoidal variation based on time and position latitude
        // Note: Full implementation requires external loading models (e.g., IERS grids)
        double latitude = std::asin(rx_pos.z() / rx_pos.norm()) * 180.0 / Constants::PI; // Approx. latitude (degrees)
        double t = fmod(time.tow, 86400.0); // Seconds of day
        double amplitude = 0.001; // 1 mm amplitude (typical)
        double correction = amplitude * std::cos(2.0 * Constants::PI * t / 86400.0) * std::cos(latitude * Constants::PI / 180.0);
        log_manager_.getLogger("ppp_model")->trace("Atmospheric loading correction: {} m", correction);
        return correction;
    }

    double GnssCorrections::getBiasCorrection(const std::string& satellite,
        const std::string& freq_type,
        const GnssTime& time) const {
        // Validate inputs
        if (satellite.empty() || freq_type.empty()) {
            log_manager_.getLogger("ppp_model")->warn("Invalid inputs for bias correction: satellite='{}', freq_type='{}' at TOW {:.3f}",
                satellite, freq_type, time.tow);
            return 0.0;
        }
        if (time.tow < 0.0 || time.week < 0) {
            log_manager_.getLogger("ppp_model")->warn("Invalid time for bias correction for satellite {}: week {}, TOW {:.3f}",
                satellite, time.week, time.tow);
            return 0.0;
        }

        // Find bias data
        auto it = std::find_if(biases_.begin(), biases_.end(),
            [&satellite, &freq_type, &time](const BiasData& b) {
                return b.satellite == satellite && b.signal == freq_type &&
                    b.start_time.tow <= time.tow && time.tow <= b.end_time.tow;
            });
        if (it != biases_.end()) {
            log_manager_.getLogger("ppp_model")->trace("Bias correction for {} ({}) {} at TOW {:.3f}: {:.3e} m",
                satellite, satellite.substr(0, 1) == "G" ? "GPS" : satellite.substr(0, 1) == "E" ? "Galileo" : "Unknown",
                freq_type, time.tow, it->bias);
            return it->bias;
        }

        log_manager_.getLogger("ppp_model")->trace("No bias correction for {} ({}) {} at TOW {:.3f}",
            satellite, satellite.substr(0, 1) == "G" ? "GPS" : satellite.substr(0, 1) == "E" ? "Galileo" : "Unknown",
            freq_type, time.tow);
        return 0.0;
    }

    double GnssCorrections::computePhaseWindUp(const std::string& satellite,
        System system,
        const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos,
        double wavelength,
        const GnssTime& time) const {

        // Phase wind-up correction for carrier phase
        Eigen::Vector3d los = (sat_pos - rx_pos);
        double los_norm = los.norm();
        if (los_norm == 0.0) {
            log_manager_.getLogger("ppp_model")->trace("Phase wind-up for {}: degenerate LOS", satellite);
            return 0.0;
        }
        los /= los_norm;
        Eigen::Vector3d k = los;

        // Satellite antenna orientation: attempt to use any cached orbit velocity if available (or zero)
        Eigen::Vector3d d_s(0.0, 0.0, 1.0), y_s(0.0, 1.0, 0.0);
        Eigen::Vector3d sat_vel(0.0, 0.0, 0.0);
        if (!orbits_.empty()) {
            auto orbit_it = std::find_if(orbits_.begin(), orbits_.end(),
                [&satellite](const OrbitData& o) { return o.satellite == satellite; });
            if (orbit_it != orbits_.end() && orbit_it->velocity_valid) {
                sat_vel = orbit_it->velocity;
            }
        }

        // Try to compute satellite attitude using a simple yaw-steering fallback if RINEX/planetary not available.
        // If the caller supplied rinex/config via constructor, they can provide better inputs; otherwise use simple fallback.
        // Basic fallback: z_s = -sat_pos.normalized(), y_s = sat_vel.cross(sun_dir) normalized (if sat_vel non-zero)
        Eigen::Vector3d sun_dir(1.0, 0.0, 0.0);
        if (config_ && rinex_parser_) {
            // No direct access to planetary ephemeris here; keep fallback if not available.
        }
        Eigen::Vector3d z_s = -sat_pos.normalized();
        if (sat_vel.norm() > 1e-8) {
            Eigen::Vector3d y_tmp = sat_vel.normalized().cross(sun_dir);
            if (y_tmp.norm() > 1e-8) {
                y_s = y_tmp.normalized();
            }
        }
        d_s = y_s.cross(z_s).normalized();

        // Receiver antenna orientation: use config if available, otherwise default
        double az_deg = 0.0, el_deg = 90.0;
        if (config_) {
            const auto& orient = config_->getReceiverAntennaOrientation();
            az_deg = orient.azimuth_deg;
            el_deg = orient.elevation_deg;
        }
        double az = az_deg * Constants::PI / 180.0;
        double el = el_deg * Constants::PI / 180.0;
        Eigen::Vector3d d_r(std::cos(el) * std::sin(az), std::cos(el) * std::cos(az), std::sin(el));

        // Override with RINEX antenna offsets if available
        if (rinex_parser_) {
            Eigen::Vector3d hen = rinex_parser_->getAntennaDeltaHen();
            if (hen.norm() > 1e-6) {
                // Approximate orientation from H/E/N (assuming H is vertical)
                d_r = Eigen::Vector3d(hen.y(), hen.x(), hen.z()).normalized();
            }
        }

        // Effective dipole vectors
        Eigen::Vector3d d_s_eff = d_s - k * (k.dot(d_s)) + k.cross(y_s);
        Eigen::Vector3d d_r_eff = d_r - k * (k.dot(d_r));
        if (d_s_eff.norm() < 1e-6 || d_r_eff.norm() < 1e-6) {
            log_manager_.getLogger("ppp_model")->trace("Phase wind-up for {}: 0 m (degenerate vectors)", satellite);
            return 0.0;
        }
        d_s_eff.normalize();
        d_r_eff.normalize();

        Eigen::Vector3d cross = d_s_eff.cross(d_r_eff);
        double dot = d_s_eff.dot(d_r_eff);
        if (dot > 1.0) dot = 1.0;
        if (dot < -1.0) dot = -1.0;
        double sign = k.dot(cross) >= 0.0 ? 1.0 : -1.0;
        double phi = sign * std::acos(dot) * wavelength / (2.0 * Constants::PI);

        log_manager_.getLogger("ppp_model")->trace("Phase wind-up for {}: {} m", satellite, phi);
        return phi;
    }

    void GnssCorrections::setClocksAndErps(const std::vector<ClockData>& clocks, const std::vector<ErpData>& erps) {
        clocks_ = clocks;
        erps_ = erps;
        log_manager_.getLogger("corrections")->trace("GnssCorrections updated: {} clocks, {} erps", clocks_.size(), erps_.size());
    }

} // namespace GnssProcessor