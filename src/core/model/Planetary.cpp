#include "Constants.hpp"
#include "Planetary.hpp"
#include "PppTypes.hpp"

namespace GnssProcessor {

    Planetary::Planetary(LogManager& log_mgr) : log_manager_(log_mgr), sun_ephemeris_{} {
        // Stub: Load from DE430/PM or config (O(N); skip if runtime)
        log_manager_.getLogger("planetary_model")->info("Planetary model initialized: {} sun ephemeris entries", sun_ephemeris_.size());
    }

    Eigen::Vector3d Planetary::computeSunPosition(double mjd) const {
        const double AU = 149597870700.0;  // Astronomical unit (m)
        const double d2r = Constants::PI / 180.0;

        double fday = mjd - std::floor(mjd);
        double JDN = mjd - 15019.5;
        double v1 = std::fmod(279.696678 + 0.9856473354 * JDN, 360.0);
        double gstr = std::fmod(279.690983 + 0.9856473354 * JDN + 360.0 * fday + 180.0, 360.0);
        double g = std::fmod(358.475845 + 0.9856002670 * JDN, 360.0) * d2r;
        double slong = v1 + (1.91946 - 0.004789 * JDN / 36525.0) * std::sin(g) + 0.020094 * std::sin(2.0 * g);
        double obliq = (23.45229 - 0.0130125 * JDN / 36525.0) * d2r;
        double slp = (slong - 0.005686) * d2r;
        double snd = std::sin(obliq) * std::sin(slp);
        double csd = std::sqrt(1.0 - snd * snd);
        double sdec = std::atan2(snd, csd) * 180.0 / Constants::PI;
        double sra = 180.0 - std::atan2(snd / csd / std::tan(obliq), -std::cos(slp) / csd) * 180.0 / Constants::PI;

        Eigen::Vector3d s_pos;
        s_pos << std::cos(sdec * d2r) * std::cos(sra * d2r) * AU,
            std::cos(sdec * d2r)* std::sin(sra * d2r)* AU,
            std::sin(sdec * d2r)* AU;

        s_pos = rotateVector(s_pos, gstr * d2r, 3);

        log_manager_.getLogger("ppp_model")->trace(
            "Sun position at MJD {:.3f}: ({:.3f}, {:.3f}, {:.3f}) m",
            mjd, s_pos.x(), s_pos.y(), s_pos.z());

        return s_pos;
    }

    Eigen::Vector3d Planetary::computeMoonPosition(double mjd) const {
        const double T = (mjd - 51544.5) / 36525.0;
        const double d2r = Constants::PI / 180.0;

        double L0 = 218.31617 + 481267.88088 * T - 1.3972 * T;
        double l = 134.96292 + 477198.86753 * T;
        double lp = 357.52543 + 35999.04944 * T;
        double F = 93.27283 + 483202.01873 * T;
        double D = 297.85027 + 445267.11135 * T;

        double long_terms = (22640.0 * std::sin(l * d2r) + 769.0 * std::sin(2.0 * l * d2r) -
            4586.0 * std::sin((l - 2.0 * D) * d2r) + 2370.0 * std::sin(2.0 * D * d2r) -
            668.0 * std::sin(lp * d2r) - 412.0 * std::sin(2.0 * F * d2r) -
            212.0 * std::sin((2.0 * l - 2.0 * D) * d2r) - 206.0 * std::sin((l + lp - 2.0 * D) * d2r) +
            192.0 * std::sin((l + 2.0 * D) * d2r) - 165.0 * std::sin((lp - 2.0 * D) * d2r) +
            148.0 * std::sin((l - lp) * d2r) - 125.0 * std::sin(D * d2r) -
            110.0 * std::sin((l + lp) * d2r) - 55.0 * std::sin((2.0 * F - 2.0 * D) * d2r)) / 3600.0;
        double long_ = std::fmod(L0 + long_terms, 360.0);

        double lat = (18520.0 * std::sin((F + long_ - L0 + (412.0 * std::sin(2.0 * F * d2r) + 541.0 * std::sin(lp * d2r)) / 3600.0) * d2r) -
            526.0 * std::sin((F - 2.0 * D) * d2r) + 44.0 * std::sin((l + F - 2.0 * D) * d2r) -
            31.0 * std::sin((-l + F - 2.0 * D) * d2r) - 25.0 * std::sin((-2.0 * l + F) * d2r) -
            23.0 * std::sin((lp + F - 2.0 * D) * d2r) + 21.0 * std::sin((-l + F) * d2r) +
            11.0 * std::sin((-lp + F - 2.0 * D) * d2r)) / 3600.0;
        lat = std::fmod(lat, 360.0);

        double dist = (385000.0 - 20905.0 * std::cos(l * d2r) - 3699.0 * std::cos((2.0 * D - l) * d2r) -
            2956.0 * std::cos(2.0 * D * d2r) - 570.0 * std::cos(2.0 * l * d2r) +
            246.0 * std::cos((2.0 * l - 2.0 * D) * d2r) - 205.0 * std::cos((lp - 2.0 * D) * d2r) -
            171.0 * std::cos((l + 2.0 * D) * d2r) - 152.0 * std::cos((l + lp - 2.0 * D) * d2r)) * 1000.0;

        Eigen::Vector3d m_pos;
        m_pos << dist * std::cos(long_ * d2r) * std::cos(lat * d2r),
            dist* std::sin(long_ * d2r)* std::cos(lat * d2r),
            dist* std::sin(lat * d2r);

        const double obl = 23.43929111 * d2r;
        m_pos = rotateVector(m_pos, -obl, 1);

        double fday = mjd - std::floor(mjd);
        double JDN = mjd - 15019.5;
        double gstr = std::fmod(279.690983 + 0.9856473354 * JDN + 360.0 * fday + 180.0, 360.0) * d2r;
        m_pos = rotateVector(m_pos, gstr, 3);

        log_manager_.getLogger("ppp_model")->trace(
            "Moon position at MJD {:.3f}: ({:.3f}, {:.3f}, {:.3f}) m",
            mjd, m_pos.x(), m_pos.y(), m_pos.z());

        return m_pos;
    }

    Eigen::Vector3d Planetary::interpolateSunPosition(const GnssTime& time) const {
        if (sun_ephemeris_.empty()) {
            return Eigen::Vector3d(0.0, 0.0, 0.0);
        }

        // Find 2 closest epochs for linear interpolation
        std::vector<SunEphemeris> candidates;
        for (const auto& data : sun_ephemeris_) {
            double time_diff = std::abs(data.time.tow - time.tow);
            if (time_diff < 86400.0) { // Within 1 day
                candidates.push_back(data);
            }
        }

        if (candidates.size() < 2) {
            if (!candidates.empty()) {
                return candidates[0].position;
            }
            return Eigen::Vector3d(0.0, 0.0, 0.0);
        }

        std::sort(candidates.begin(), candidates.end(),
            [&time](const SunEphemeris& a, const SunEphemeris& b) {
                return std::abs(a.time.tow - time.tow) < std::abs(b.time.tow - time.tow);
            });
        candidates.resize(2);

        double t = time.tow;
        double t0 = candidates[0].time.tow;
        double t1 = candidates[1].time.tow;
        Eigen::Vector3d p0 = candidates[0].position;
        Eigen::Vector3d p1 = candidates[1].position;

        if (t0 == t1) {
            return p0;
        }

        double frac = (t - t0) / (t1 - t0);
        return p0 + frac * (p1 - p0);
    }

    Eigen::Vector3d Planetary::rotateVector(const Eigen::Vector3d& vec, double angle, int axis) const {
        Eigen::Matrix3d R;
        double c = std::cos(angle);
        double s = std::sin(angle);
        if (axis == 1) {  // X-axis
            R << 1, 0, 0,
                0, c, -s,
                0, s, c;
        }
        else if (axis == 3) {  // Z-axis
            R << c, -s, 0,
                s, c, 0,
                0, 0, 1;
        }
        else {
            log_manager_.getLogger("ppp_model")->warn("Invalid rotation axis: {}", axis);
            return vec;
        }
        return R * vec;
    }

    bool Planetary::parseSunEphemeris(const std::vector<GnssTime>& epoch_times, std::vector<SunEphemeris>& sun_ephemeris) const {
        try {
            sun_ephemeris.clear();
            if (epoch_times.empty()) {
                log_manager_.getLogger("ppp_model")->error("No epoch times provided for Sun ephemeris computation");
                return false;
            }

            // Constants for Sun position (JPL low-precision ephemeris approximation)
            const double AU = 1.495978707e11; // Astronomical unit in meters
            const double epsilon = 23.439281 * Constants::PI / 180.0; // Obliquity of ecliptic (radians)
            const double T_0 = 2451545.0; // J2000 epoch (JD)
            const double M_0 = 357.5291 * Constants::PI / 180.0; // Mean anomaly at J2000 (radians)
            const double n = 0.98560028 * Constants::PI / 180.0 / 86400.0; // Mean motion (rad/s)

            for (const auto& epoch_time : epoch_times) {
                // Validate epoch time
                if (epoch_time.week < 0 || epoch_time.tow < 0 || epoch_time.tow >= 604800.0) {
                    log_manager_.getLogger("ppp_model")->warn("Invalid epoch time: week {}, tow {:.3f}", epoch_time.week, epoch_time.tow);
                    continue;
                }

                // Convert GPS time to Julian Date
                double gps_t = TimeUtils::gpsTimeToJulianDate(epoch_time.week, epoch_time.tow);
                double T = (gps_t - T_0) / 36525.0; // Julian centuries since J2000

                // Mean anomaly
                double M = M_0 + n * (gps_t - T_0) * 86400.0;
                M = std::fmod(M, 2.0 * Constants::PI); // Normalize to [0, 2pi]

                // Approximate eccentric anomaly (simplified, low eccentricity approx. 0.0167)
                double E = M; // First-order approximation
                double cosE = std::cos(E);
                double sinE = std::sin(E);

                // Heliocentric coordinates in ecliptic plane
                double x_helio = AU * (cosE - 0.0167);
                double y_helio = AU * std::sqrt(1.0 - 0.0167 * 0.0167) * sinE;
                double z_helio = 0.0; // Assume Sun in ecliptic plane

                // Transform to ECEF (simplified, apply obliquity rotation)
                double x_ecef = x_helio;
                double y_ecef = y_helio * std::cos(epsilon);
                double z_ecef = y_helio * std::sin(epsilon);

                SunEphemeris data;
                data.time = epoch_time;
                data.position = Eigen::Vector3d(x_ecef, y_ecef, z_ecef);
                if (!data.position.allFinite()) {
                    log_manager_.getLogger("ppp_model")->warn("Non-finite Sun position at TOW {:.3f}: ({:.3e}, {:.3e}, {:.3e})",
                        epoch_time.tow, x_ecef, y_ecef, z_ecef);
                    continue;
                }
                sun_ephemeris.push_back(data);
            }

            if (sun_ephemeris.empty()) {
                log_manager_.getLogger("ppp_model")->error("No valid Sun ephemeris entries computed");
                return false;
            }
            log_manager_.getLogger("ppp_model")->info("Computed {} Sun ephemeris entries for observation epochs", sun_ephemeris.size());
            return true;
        }
        catch (const std::exception& e) {
            log_manager_.getLogger("ppp_model")->error("Exception computing Sun ephemeris: {}", e.what());
            sun_ephemeris.clear();
            return false;
        }
    }

}