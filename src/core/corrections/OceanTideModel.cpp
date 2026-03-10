/**
 * @file OceanTideModel.cpp
 * @brief Implementation of the ocean tidal loading displacement model.
 *
 * Computes station displacements due to ocean tidal loading using the
 * 11 major tidal constituents with Doodson-based astronomical arguments.
 *
 * References:
 *   - IERS Conventions (2010), Chapter 7.1.2
 *   - Scherneck, H.-G. (1991), "A parametrized solid Earth tide model and
 *     ocean tide loading effects for global geodetic baseline measurements"
 *   - Petit & Luzum (2010), IERS Technical Note 36
 */

#include "OceanTideModel.hpp"
#include "Coordinates.hpp"
#include "Constants.hpp"
#include <cmath>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace GnssProcessor {

    OceanTideModel::OceanTideModel(LogManager& log_mgr)
        : log_manager_(log_mgr), station_coeffs_{}
    {
        log_manager_.getLogger("ocean_tide")->info("OceanTideModel initialized (no station coefficients loaded)");
    }

    void OceanTideModel::setStationCoefficients(const OtlStationCoefficients& coeffs) {
        station_coeffs_ = coeffs;

        if (coeffs.valid) {
            double max_amp = 0.0;
            for (size_t j = 0; j < 11; ++j) {
                max_amp = std::max(max_amp, coeffs.constituents[j].amp_up);
            }
            log_manager_.getLogger("ocean_tide")->info(
                "OTL coefficients loaded for station '{}': max radial amplitude = {:.4f} m",
                coeffs.station_name, max_amp);
        }
    }

    /**
     * @brief Compute the six fundamental Delaunay arguments and GMST.
     *
     * Implements the IERS 2010 expressions (Simon et al. 1994) for the
     * fundamental arguments of nutation theory:
     *   l   — Mean anomaly of the Moon  (F1)
     *   l'  — Mean anomaly of the Sun   (F2)
     *   F   — Mean argument of latitude of the Moon (F3)
     *   D   — Mean elongation of Moon from Sun (F4)
     *   ?   — Longitude of ascending node of Moon (F5)
     *   ?   — Greenwich Mean Sidereal Time (GMST)
     *
     * @param t_centuries Julian centuries of TT from J2000.0.
     * @param ut1_frac UT1 day fraction (for GMST).
     * @return Array of 6 arguments in radians.
     */
    std::array<double, 6> OceanTideModel::computeFundamentalArguments(double t_centuries, double ut1_frac) const {
        constexpr double D2R = Constants::PI / 180.0;
        constexpr double AS2R = D2R / 3600.0;  // arcsecond to radian

        const double T = t_centuries;
        const double T2 = T * T;
        const double T3 = T2 * T;
        const double T4 = T3 * T;

        // l: Mean anomaly of the Moon (IERS 2010, Eq. 5.43)
        double l = std::fmod(
            485868.249036 + 1717915923.2178 * T + 31.8792 * T2
            + 0.051635 * T3 - 0.00024470 * T4, 1296000.0) * AS2R;

        // l': Mean anomaly of the Sun (IERS 2010, Eq. 5.44)
        double lp = std::fmod(
            1287104.79305 + 129596581.0481 * T - 0.5532 * T2
            + 0.000136 * T3 - 0.00001149 * T4, 1296000.0) * AS2R;

        // F: Mean argument of latitude of the Moon (IERS 2010, Eq. 5.45)
        double F = std::fmod(
            335779.526232 + 1739527262.8478 * T - 12.7512 * T2
            - 0.001037 * T3 + 0.00000417 * T4, 1296000.0) * AS2R;

        // D: Mean elongation of Moon from Sun (IERS 2010, Eq. 5.46)
        double D = std::fmod(
            1072260.70369 + 1602961601.2090 * T - 6.3706 * T2
            + 0.006593 * T3 - 0.00003169 * T4, 1296000.0) * AS2R;

        // ?: Longitude of ascending node of Moon (IERS 2010, Eq. 5.47)
        double Omega = std::fmod(
            450160.398036 - 6962890.5431 * T + 7.4722 * T2
            + 0.007702 * T3 - 0.00005939 * T4, 1296000.0) * AS2R;

        // GMST: Greenwich Mean Sidereal Time (IERS 2010, Eq. 5.32 / Capitaine et al. 2003)
        // GMST = 280.46061837 + 360.98564736629*(JD_UT1 - 2451545.0) + higher-order terms
        // We approximate JD_UT1 from GPS time (GPS-UT1 < 1s, negligible for OTL).
        // The TT centuries (T) are used only for the small quadratic/cubic corrections.
        double JD_UT1_approx = TimeUtils::J2000_JD + (T * TimeUtils::DAYS_PER_JULIAN_CENTURY)
                             - TimeUtils::TT_GPS_OFFSET / TimeUtils::SECONDS_IN_DAY;
        double gmst_deg = std::fmod(
            280.46061837 + 360.98564736629 * (JD_UT1_approx - TimeUtils::J2000_JD)
            + 0.000387933 * T2 - T3 / 38710000.0, 360.0);
        double gmst = gmst_deg * D2R;

        return { l, lp, F, D, Omega, gmst };
    }

    /**
     * @brief Compute tidal arguments for all 11 constituents using Doodson multipliers.
     *
     * The tidal argument for constituent j is:
     *   ?_j = n1·(? + ?) + n2·s + n3·h + n4·p + n5·(-N') + n6·ps
     *
     * where [n1..n6] are the Doodson multipliers and:
     *   ? = GMST
     *   s = F + ?     (mean longitude of Moon)
     *   h = F + ? - D (mean longitude of Sun, approximation)
     *   p = F + ? - l (longitude of Moon's perigee, approximation)
     *   N' = -?       (negative longitude of Moon's node)
     *   ps = s - D + l' (longitude of Sun's perigee, approximation)
     *
     * More precisely, using the standard mapping from Delaunay to Doodson:
     *   ? = ? - s  (but we include ? directly via GMST+? convention)
     *   s = F + ?
     *   h = s - D = F + ? - D
     *   p = s - l = F + ? - l
     *   N' = -?
     *   ps = s - D + l' ? h + l' (but for the major tides, ps column is 0)
     *
     * For practical computation we map Doodson arguments to Delaunay:
     *   argument = n1·(GMST + ?) + (n2 + n1)·(F + ?) - n1·(F + ?)  ... etc.
     *
     * Simplified direct mapping for each constituent using Delaunay + GMST:
     *
     * @param fund_args Array [l, l', F, D, ?, GMST].
     * @return Array of 11 tidal arguments in radians.
     */
    std::array<double, 11> OceanTideModel::computeTidalArguments(const std::array<double, 6>& fund_args) const {
        const double l = fund_args[0];
        const double lp = fund_args[1];
        const double F = fund_args[2];
        const double D = fund_args[3];
        const double Omega = fund_args[4];
        const double gmst = fund_args[5];

        // Doodson variables from Delaunay arguments:
        const double s = F + Omega;          // mean longitude of Moon
        const double h = s - D;              // mean longitude of Sun
        const double p = s - l;              // longitude of Moon's perigee
        const double N_prime = -Omega;       // negative of Moon's node longitude
        const double ps = h + lp;            // longitude of Sun's perigee (approximation, rarely needed)

        // ? = GMST + ? - s  (lunar time angle)
        // But for our Doodson encoding: argument = n1·? + n2·s + n3·h + n4·p + n5·N' + n6·ps
        // where ? = GMST + ? - s
        // So: argument = n1·(GMST + ?) + (n2 - n1)·s + n3·h + n4·p + n5·N' + n6·ps
        //
        // Actually the standard Doodson encoding uses:
        //   argument = n1·(GMST + ?) + n2·s + n3·h + n4·p + n5·N' + n6·ps
        // where our n1 multiplies (GMST + ?), and n2 accounts for the 's' that is
        // NOT already subtracted by ?. The table in the header already encodes the
        // arguments directly as multipliers of [GMST+?, s, h, p, N', ps].

        std::array<double, 11> arguments{};
        const double gmst_pi = gmst + Constants::PI;

        const double doodson_vars[6] = { gmst_pi, s, h, p, N_prime, ps };

        for (size_t j = 0; j < 11; ++j) {
            double arg = 0.0;
            for (size_t k = 0; k < 6; ++k) {
                arg += DOODSON_MULTIPLIERS[j][k] * doodson_vars[k];
            }
            // Normalize to [0, 2?)
            arg = std::fmod(arg, 2.0 * Constants::PI);
            if (arg < 0.0) arg += 2.0 * Constants::PI;
            arguments[j] = arg;
        }

        return arguments;
    }

    Eigen::Vector3d OceanTideModel::computeDisplacement(const Eigen::Vector3d& rx_pos, const GnssTime& time) const {
        if (!rx_pos.allFinite() || rx_pos.norm() < 1e3) {
            log_manager_.getLogger("ocean_tide")->warn(
                "Invalid receiver position for OTL at TOW {:.3f}", time.tow);
            return Eigen::Vector3d::Zero();
        }

        if (!station_coeffs_.valid) {
            return computeApproximateDisplacement(rx_pos, time);
        }

        // Convert GPS time to TT Julian centuries from J2000.0
        double t_centuries = TimeUtils::gpsTimeToTtCenturies(time);

        // UT1 fraction (approximate: use GPS time, ignore UT1-UTC which is < 1 s)
        double ut1_frac = std::fmod(TimeUtils::gpsTimeToMjd(time), 1.0);

        // Compute fundamental arguments
        auto fund_args = computeFundamentalArguments(t_centuries, ut1_frac);

        // Compute tidal arguments for all 11 constituents
        auto tidal_args = computeTidalArguments(fund_args);

        constexpr double D2R = Constants::PI / 180.0;

        // Sum contributions from all 11 constituents
        // Convention: displacement in (Up, West, South)
        double disp_up = 0.0;
        double disp_west = 0.0;
        double disp_south = 0.0;

        for (size_t j = 0; j < 11; ++j) {
            const auto& c = station_coeffs_.constituents[j];
            const double chi = tidal_args[j];

            // displacement_k = A_k * cos(?_j - ?_k)
            // where ?_k is the Greenwich phase lag in radians
            disp_up    += c.amp_up    * std::cos(chi - c.phase_up    * D2R);
            disp_west  += c.amp_west  * std::cos(chi - c.phase_west  * D2R);
            disp_south += c.amp_south * std::cos(chi - c.phase_south * D2R);
        }

        // Convert from (Up, West, South) to (East, North, Up) = (-West, -South, Up)
        Eigen::Vector3d enu_disp(-disp_west, -disp_south, disp_up);

        // Convert ENU to ECEF
        Coordinates::Llh llh = Coordinates::ecefToLlh(rx_pos);
        Eigen::Vector3d ecef_disp = Coordinates::enuToEcef(enu_disp, llh);

        if (!ecef_disp.allFinite() || ecef_disp.norm() > 0.5) {
            log_manager_.getLogger("ocean_tide")->warn(
                "OTL displacement out of range at TOW {:.3f}: ({:.4f}, {:.4f}, {:.4f}) m, norm={:.4f}",
                time.tow, ecef_disp.x(), ecef_disp.y(), ecef_disp.z(), ecef_disp.norm());
            return Eigen::Vector3d::Zero();
        }

        log_manager_.getLogger("ocean_tide")->trace(
            "OTL displacement at TOW {:.3f}: up={:.4f}, west={:.4f}, south={:.4f} m, "
            "ECEF=({:.4f}, {:.4f}, {:.4f}) m",
            time.tow, disp_up, disp_west, disp_south,
            ecef_disp.x(), ecef_disp.y(), ecef_disp.z());

        return ecef_disp;
    }

    Eigen::Vector3d OceanTideModel::computeApproximateDisplacement(const Eigen::Vector3d& rx_pos, const GnssTime& time) const {
        if (!rx_pos.allFinite() || rx_pos.norm() < 1e3) {
            return Eigen::Vector3d::Zero();
        }

        // Convert GPS time to TT Julian centuries from J2000.0
        double t_centuries = TimeUtils::gpsTimeToTtCenturies(time);
        double ut1_frac = std::fmod(TimeUtils::gpsTimeToMjd(time), 1.0);

        auto fund_args = computeFundamentalArguments(t_centuries, ut1_frac);
        auto tidal_args = computeTidalArguments(fund_args);

        // Approximate global-average coastal amplitudes (meters) for the radial component.
        // These represent typical mid-latitude coastal station values.
        // Horizontal components are typically ~30-40% of radial.
        // Interior/continental stations have smaller values.
        //
        // Order: M2, S2, N2, K2, K1, O1, P1, Q1, Mf, Mm, Ssa
        static constexpr double APPROX_AMP_UP[11] = {
            0.0120, 0.0040, 0.0025, 0.0011,  // semi-diurnal: M2, S2, N2, K2
            0.0032, 0.0025, 0.0010, 0.0005,  // diurnal: K1, O1, P1, Q1
            0.0003, 0.0001, 0.0002            // long-period: Mf, Mm, Ssa
        };
        static constexpr double APPROX_AMP_HOR[11] = {
            0.0035, 0.0012, 0.0007, 0.0003,
            0.0009, 0.0007, 0.0003, 0.0002,
            0.0001, 0.0000, 0.0001
        };

        // Scale by approximate latitude dependence:
        // Coastal stations have larger OTL; interior stations much smaller.
        // Distance from coast is hard to model, so we use a simple latitude scaling.
        Coordinates::Llh llh = Coordinates::ecefToLlh(rx_pos);

        // Simple scale: full amplitude near equator (~1.0x), reduce for high-latitude.
        double lat_scale = std::cos(llh.lat_rad) * 0.8 + 0.2;

        // Use a zero phase offset (no station-specific information available).
        // This means the model captures the correct frequency content and
        // approximate amplitude but not the exact phase relationship.
        // For PPP, even an approximate amplitude and period match helps the
        // filter absorb less of the signal into the position states.
        double disp_up = 0.0;
        double disp_east = 0.0;
        double disp_north = 0.0;

        for (size_t j = 0; j < 11; ++j) {
            double chi = tidal_args[j];
            disp_up    += APPROX_AMP_UP[j]  * lat_scale * std::cos(chi);
            disp_east  += APPROX_AMP_HOR[j] * lat_scale * std::cos(chi + Constants::PI / 4.0); // approximate phase offset
            disp_north += APPROX_AMP_HOR[j] * lat_scale * std::cos(chi + Constants::PI / 2.0);
        }

        Eigen::Vector3d enu_disp(disp_east, disp_north, disp_up);
        Eigen::Vector3d ecef_disp = Coordinates::enuToEcef(enu_disp, llh);

        if (!ecef_disp.allFinite() || ecef_disp.norm() > 0.5) {
            return Eigen::Vector3d::Zero();
        }

        log_manager_.getLogger("ocean_tide")->trace(
            "Approximate OTL at TOW {:.3f}: up={:.4f}, E={:.4f}, N={:.4f} m (lat_scale={:.3f})",
            time.tow, disp_up, disp_east, disp_north, lat_scale);

        return ecef_disp;
    }

    /**
     * @brief Parse a BLQ file to extract OTL coefficients for a station.
     *
     * BLQ format (Onsala / Scherneck convention):
     *   - Lines starting with "$$" are comments
     *   - Station header line starts with "  " followed by station name
     *   - 6 data lines follow:
     *     Lines 1-3: amplitudes [M2 S2 N2 K2 K1 O1 P1 Q1 Mf Mm Ssa]
     *       Line 1: Up (radial)
     *       Line 2: West (tangential)
     *       Line 3: South (tangential)
     *     Lines 4-6: phases (degrees) [same order]
     *       Line 4: Up
     *       Line 5: West
     *       Line 6: South
     */
    bool OceanTideModel::parseBLQ(const std::string& blq_path, const std::string& station_name,
                                   OtlStationCoefficients& coeffs) const {
        std::ifstream file(blq_path);
        if (!file.is_open()) {
            log_manager_.getLogger("ocean_tide")->error("Cannot open BLQ file: {}", blq_path);
            return false;
        }

        // Normalize station name for case-insensitive comparison
        std::string target = station_name;
        std::transform(target.begin(), target.end(), target.begin(), ::toupper);

        std::string line;
        bool found = false;

        while (std::getline(file, line)) {
            // Skip comments
            if (line.size() >= 2 && line[0] == '$' && line[1] == '$') continue;

            // Check for station header line (starts with "  " and contains station name)
            if (line.size() >= 4 && line[0] == ' ' && line[1] == ' ') {
                std::string header = line.substr(2);
                // Trim and uppercase
                header.erase(header.find_last_not_of(" \t\r\n") + 1);
                header.erase(0, header.find_first_not_of(" \t"));
                std::string header_upper = header;
                std::transform(header_upper.begin(), header_upper.end(), header_upper.begin(), ::toupper);

                if (header_upper.find(target) != std::string::npos) {
                    found = true;
                    break;
                }
            }
        }

        if (!found) {
            log_manager_.getLogger("ocean_tide")->warn(
                "Station '{}' not found in BLQ file: {}", station_name, blq_path);
            return false;
        }

        // Read 6 data lines (3 amplitude lines + 3 phase lines)
        double amp_up[11]{}, amp_west[11]{}, amp_south[11]{};
        double phase_up[11]{}, phase_west[11]{}, phase_south[11]{};

        auto readLine = [&](double* values, int count) -> bool {
            std::string data_line;
            while (std::getline(file, data_line)) {
                // Skip comments and empty lines
                if (data_line.size() >= 2 && data_line[0] == '$' && data_line[1] == '$') continue;
                if (data_line.empty() || data_line.find_first_not_of(" \t\r\n") == std::string::npos) continue;
                break;
            }
            std::istringstream iss(data_line);
            for (int i = 0; i < count; ++i) {
                if (!(iss >> values[i])) {
                    return false;
                }
            }
            return true;
        };

        if (!readLine(amp_up, 11) || !readLine(amp_west, 11) || !readLine(amp_south, 11) ||
            !readLine(phase_up, 11) || !readLine(phase_west, 11) || !readLine(phase_south, 11)) {
            log_manager_.getLogger("ocean_tide")->error(
                "Failed to parse 6 data lines for station '{}' in BLQ file", station_name);
            return false;
        }

        coeffs.station_name = station_name;
        coeffs.valid = true;
        for (size_t j = 0; j < 11; ++j) {
            coeffs.constituents[j].amp_up = amp_up[j];
            coeffs.constituents[j].amp_west = amp_west[j];
            coeffs.constituents[j].amp_south = amp_south[j];
            coeffs.constituents[j].phase_up = phase_up[j];
            coeffs.constituents[j].phase_west = phase_west[j];
            coeffs.constituents[j].phase_south = phase_south[j];
        }

        log_manager_.getLogger("ocean_tide")->info(
            "Parsed BLQ for station '{}': M2 amp_up={:.4f} m, S2={:.4f} m, K1={:.4f} m",
            station_name, amp_up[0], amp_up[1], amp_up[4]);

        return true;
    }

} // namespace GnssProcessor
