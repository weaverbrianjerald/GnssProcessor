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

#include "corrections/OceanTideModel.hpp"
#include "common/StationInfo.hpp"
#include "math/Coordinates.hpp"
#include "common/Constants.hpp"
#include <cmath>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <vector>
#include <netcdf.h>

namespace GnssProcessor {

    namespace {

        /**
         * @brief One node side line of a constituent: the ratio of its equilibrium-tide
         *        amplitude to that of the main line.
         *
         * Taken from the Cartwright-Tayler-Edden development tabulated in the IERS HARDISP
         * routine (ADMINT.F, tables TAMP and IDD): the lines that share the Doodson
         * numbers of a main line except the lunar node multiplier. scripts/otl_compare/
         * nodal_audit.py regenerates the rows.
         */
        struct NodalLine {
            int    constituent;  ///< Index in the BLQ order M2, S2, N2, K2, K1, O1, P1, Q1, Mf, Mm, Ssa.
            int    n5;           ///< Multiplier of N' = -Omega in the line's Doodson number.
            double ratio;        ///< Side-line amplitude over main-line amplitude (signed).
        };

        constexpr NodalLine NODAL_LINES[] = {
            {  0, -2, +0.000520 },  // M2
            {  0, -1, -0.037312 },  // M2
            {  1, -1, +0.002237 },  // S2
            {  2, -2, +0.000520 },  // N2
            {  2, -1, -0.037333 },  // N2
            {  3, -1, -0.012789 },  // K2
            {  3,  1, +0.298042 },  // K2
            {  3,  2, +0.032397 },  // K2
            {  4, -1, -0.019802 },  // K1
            {  4,  1, +0.135716 },  // K1
            {  4,  2, -0.002916 },  // K1
            {  5, -2, -0.005781 },  // O1
            {  5, -1, +0.188650 },  // O1
            {  6, -2, +0.000820 },  // P1
            {  6, -1, -0.011238 },  // P1
            {  7, -2, -0.005796 },  // Q1
            {  7, -1, +0.188595 },  // Q1
            {  8,  1, +0.414611 },  // Mf
            {  8,  2, +0.038780 },  // Mf
            {  8,  3, -0.000826 },  // Mf
            {  9, -1, -0.065655 },  // Mm
            {  9,  1, -0.064887 },  // Mm
            { 10,  1, -0.024719 },  // Ssa
            { 10,  2, -0.005486 },  // Ssa
        };

    } // namespace

    OtlNodalFactor OceanTideModel::nodalFactor(std::size_t constituent, double node_longitude_rad) {
        if (constituent >= OTL_CONSTITUENT_NAMES.size()) return {};
        // z = 1 + sum r * exp(-i n5 Omega); f = |z|, u = arg z.
        double re = 1.0;
        double im = 0.0;
        for (const NodalLine& line : NODAL_LINES) {
            if (static_cast<std::size_t>(line.constituent) != constituent) continue;
            const double a = line.n5 * node_longitude_rad;
            re += line.ratio * std::cos(a);
            im -= line.ratio * std::sin(a);
        }
        return { std::sqrt(re * re + im * im), std::atan2(im, re) };
    }

    OtlStationCoefficients otlCoefficientsFromSif(const StationInfo& station,
                                                  SifOtlHorizontal horizontal) {
        // The legacy mapping adds 180 degrees to the horizontal phases; the BLQ-row
        // mapping adds nothing.
        const double shift = (horizontal == SifOtlHorizontal::LegacyShifted) ? 180.0 : 0.0;
        OtlStationCoefficients coeffs;
        coeffs.station_name = station.id;
        for (std::size_t k = 0; k < 11; ++k) {
            const SifOtlConstituent& c = station.otl[k];
            OtlConstituentCoeffs& d = coeffs.constituents[k];
            d.amp_up    = c.up_amp;     d.phase_up    = c.up_phase;
            d.amp_west  = c.east_amp;   d.phase_west  = c.east_phase  + shift;
            d.amp_south = c.north_amp;  d.phase_south = c.north_phase + shift;
        }
        coeffs.valid = true;
        return coeffs;
    }

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
     * @brief Compute the five fundamental Delaunay arguments and GMST.
     *
     * Implements the IERS 2010 expressions (Simon et al. 1994) for the
     * fundamental arguments of nutation theory:
     *   l    Mean anomaly of the Moon  (F1)
     *   l'   Mean anomaly of the Sun   (F2)
     *   F    Mean argument of latitude of the Moon (F3)
     *   D    Mean elongation of Moon from Sun (F4)
     *   Ω    Longitude of ascending node of Moon (F5)
     *   GMST Greenwich Mean Sidereal Time
     *
     * @param t_centuries Julian centuries of TT from J2000.0.
     * @param ut1_frac Unused: GMST is computed from @p t_centuries.
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

        // Ω: Longitude of ascending node of Moon (IERS 2010, Eq. 5.47)
        double Omega = std::fmod(
            450160.398036 - 6962890.5431 * T + 7.4722 * T2
            + 0.007702 * T3 - 0.00005939 * T4, 1296000.0) * AS2R;

        // GMST in degrees:
        //   280.46061837 + 360.98564736629*(JD_UT1 - 2451545.0) + 0.000387933*T² - T³/38710000.
        // This is the IAU 1982 expression in the form of Meeus (Astronomical Algorithms,
        // eq. 12.4), not the ERA-based IERS 2010 Eq. 5.32 (Capitaine et al. 2003); the two
        // differ negligibly for OTL.  JD_UT1 is approximated by the GPS-time Julian date.
        // GPS - UT1 is the leap-second count (18 s since 2017) plus |UT1 - UTC| < 0.9 s,
        // which shifts a semidiurnal argument by about 0.15 deg; negligible for OTL.
        // The TT centuries (T) are used only for the small quadratic and cubic terms.
        double JD_UT1_approx = TimeUtils::J2000_JD + (T * TimeUtils::DAYS_PER_JULIAN_CENTURY)
                             - TimeUtils::TT_GPS_OFFSET / TimeUtils::SECONDS_IN_DAY;
        double gmst_deg = std::fmod(
            280.46061837 + 360.98564736629 * (JD_UT1_approx - TimeUtils::J2000_JD)
            + 0.000387933 * T2 - T3 / 38710000.0, 360.0);
        double gmst = gmst_deg * D2R;

        return { l, lp, F, D, Omega, gmst };
    }

    double OceanTideModel::greenwichAngleUtc(const GnssTime& time, double t_centuries) const {
        constexpr double D2R = Constants::PI / 180.0;
        const double T  = t_centuries;
        const double T2 = T * T;
        const double T3 = T2 * T;
        const double jd_utc = TimeUtils::gpsTimeToUtcMjd(time) + 2400000.5;
        const double gmst_deg = std::fmod(
            280.46061837 + 360.98564736629 * (jd_utc - TimeUtils::J2000_JD)
            + 0.000387933 * T2 - T3 / 38710000.0, 360.0);
        return gmst_deg * D2R;
    }

    /**
     * @brief Compute tidal arguments for all 11 constituents using Doodson multipliers.
     *
     * The tidal argument for constituent j is
     *   χ_j = n1·(GMST + π) + n2·s + n3·h + n4·p + n5·N' + n6·ps + 2π·b_j
     *
     * where [n1..n6] is row j of DOODSON_MULTIPLIERS, b_j is
     * CONSTITUENT_PHASE_BIAS_CYCLES[j], and the Doodson variables follow from the
     * Delaunay arguments:
     *   s  = F + Ω       (mean longitude of the Moon)
     *   h  = s - D       (mean longitude of the Sun)
     *   p  = s - l       (longitude of the Moon's perigee)
     *   N' = -Ω          (negative longitude of the Moon's node)
     *   ps = h + l'      (longitude of the Sun's perigee, as coded; see below)
     *
     * The lunar time τ = GMST + π - s is not formed; its -n1·s part is folded into
     * the s column of DOODSON_MULTIPLIERS, so n1 multiplies (GMST + π) directly.
     * The result is normalized to [0, 2π).
     *
     * @param fund_args Array [l, l', F, D, Ω, GMST].
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
        // Longitude of the Sun's perigee.  The standard relation is ps = h - l'; the
        // sign here does not matter because the ps column is 0 for all 11 constituents.
        const double ps = h + lp;

        // The loop forms n1·(GMST + π) + n2·s + n3·h + n4·p + n5·N' + n6·ps.
        // The multiplier table in the header is already expressed against
        // [GMST+π, s, h, p, N', ps], with the lunar-time -n1·s folded into n2.

        std::array<double, 11> arguments{};
        const double gmst_pi = gmst + Constants::PI;

        const double doodson_vars[6] = { gmst_pi, s, h, p, N_prime, ps };

        for (size_t j = 0; j < 11; ++j) {
            double arg = 0.0;
            for (size_t k = 0; k < 6; ++k) {
                arg += DOODSON_MULTIPLIERS[j][k] * doodson_vars[k];
            }
            // Doodson-Warburg species phase bias (a quarter-cycle for the
            // diurnal band).  Without it the K1/O1/P1/Q1 arguments are 90 deg
            // out of phase with the IERS ARG.f / HARDISP convention to which
            // the BLQ and EOT20/FES phases are referenced.  See
            // CONSTITUENT_PHASE_BIAS_CYCLES.
            arg += CONSTITUENT_PHASE_BIAS_CYCLES[j] * 2.0 * Constants::PI;
            // Normalize to [0, 2*pi)
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

        // Day fraction on the GPS time scale, standing in for UT1.  The value is passed
        // on but computeFundamentalArguments() does not use it.
        double ut1_frac = std::fmod(TimeUtils::gpsTimeToMjd(time), 1.0);

        // Compute fundamental arguments
        auto fund_args = computeFundamentalArguments(t_centuries, ut1_frac);

        // Nodal factors (identity unless enabled) and the UTC Greenwich argument.
        std::array<OtlNodalFactor, 11> nodal{};
        if (nodal_corrections_) {
            fund_args[5] = greenwichAngleUtc(time, t_centuries);
            for (std::size_t j = 0; j < 11; ++j)
                nodal[j] = nodalFactor(j, fund_args[4]);
        }

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
            // With the nodal corrections off f is exactly 1 and u exactly 0, so the
            // sum is the original one bit for bit.
            const double chi = tidal_args[j] + nodal[j].u_rad;
            const double f   = nodal[j].f;

            // displacement_k = f * A_k * cos(χ_j + u - φ_k)
            // where φ_k is the Greenwich phase lag (stored in degrees, converted here)
            disp_up    += (f * c.amp_up)    * std::cos(chi - c.phase_up    * D2R);
            disp_west  += (f * c.amp_west)  * std::cos(chi - c.phase_west  * D2R);
            disp_south += (f * c.amp_south) * std::cos(chi - c.phase_south * D2R);
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

    Eigen::Vector3d OceanTideModel::computeApproximateDisplacement(const Eigen::Vector3d& /*rx_pos*/,
                                                                    const GnssTime& /*time*/) const {
        // No station-specific OTL coefficients (BLQ or EOT20) are loaded.
        //
        // A phase-blind, latitude-scaled OTL signal built from global-average
        // coastal amplitudes has the wrong Greenwich phase and adds a coherent
        // biased oscillation to the Up channel that the filter cannot absorb
        // cleanly; for many stations that is worse than no correction.  With
        // zero, the receiver clock and ZTD absorb the residual OTL signal.
        //
        // OTL displacement requires one of:
        //   (a) files.otl_blq_path  → station-specific BLQ from Onsala, or
        //   (b) data_download.nc_path → EOT20_load.nc (synthesized for any
        //       station via OceanTideModel::parseEot20Load).
        return Eigen::Vector3d::Zero();
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

    /**
     * @brief Synthesize OtlStationCoefficients from EOT20_load.nc.
     *
     * EOT20 schema (DGFI-TUM, Hart-Davis et al. 2021):
     *   dims: lon (2883, with -0.125° / 360.125° boundary repeats), lat (1441,
     *         descending 90 → -90), constituents (17)
     *   vars: lon[lon], lat[lat] (degrees);
     *         hRe[const, lat, lon], hIm[const, lat, lon] (int16, scale_factor);
     *         constituents.constituent_order = "2n2 j1 k1 k2 m2 m4 mf mm n2 o1 p1 q1 s1 s2 sa ssa t2"
     *
     * The function reads a 17 × 2 × 2 hyperslab of hRe and of hIm (68 values each,
     * 544 bytes as double), applies scale_factor / add_offset, bilinearly
     * interpolates each constituent at the station's (lat, lon), and picks the
     * 11 BLQ-order constituents (indices [4 13 8 3 2 9 10 11 6 7 15] in EOT20).
     */
    bool OceanTideModel::parseEot20Load(const std::string& nc_path,
                                        const Eigen::Vector3d& station_ecef,
                                        OtlStationCoefficients& coeffs) const {
        auto log = log_manager_.getLogger("ocean_tide");

        if (!station_ecef.allFinite() || station_ecef.norm() < 1e6) {
            log->error("EOT20: invalid station ECEF ({:.3e}, {:.3e}, {:.3e})",
                       station_ecef.x(), station_ecef.y(), station_ecef.z());
            return false;
        }

        int ncid = -1;
        if (int rc = nc_open(nc_path.c_str(), NC_NOWRITE, &ncid); rc != NC_NOERR) {
            log->error("EOT20: cannot open '{}': {}", nc_path, nc_strerror(rc));
            return false;
        }

        // RAII close: every early return below closes the file through this.
        struct NcCloser {
            int id;
            ~NcCloser() { if (id >= 0) nc_close(id); }
        } closer{ ncid };

        auto getDim = [&](const char* name, size_t& out) -> bool {
            int dimid;
            if (nc_inq_dimid(ncid, name, &dimid) != NC_NOERR) return false;
            return nc_inq_dimlen(ncid, dimid, &out) == NC_NOERR;
        };
        auto getVarId = [&](const char* name, int& out) -> bool {
            return nc_inq_varid(ncid, name, &out) == NC_NOERR;
        };

        size_t n_lon = 0, n_lat = 0, n_const = 0;
        if (!getDim("lon", n_lon) || !getDim("lat", n_lat) || !getDim("constituents", n_const)) {
            log->error("EOT20: missing required dimension(s) lon/lat/constituents in '{}'", nc_path);
            return false;
        }
        if (n_const < 17 || n_lon < 2 || n_lat < 2) {
            log->error("EOT20: unexpected dimensions (lon={}, lat={}, const={})", n_lon, n_lat, n_const);
            return false;
        }

        int varid_lon = -1, varid_lat = -1, varid_hRe = -1, varid_hIm = -1;
        if (!getVarId("lon", varid_lon) || !getVarId("lat", varid_lat) ||
            !getVarId("hRe", varid_hRe) || !getVarId("hIm", varid_hIm)) {
            log->error("EOT20: missing required variable(s) lon/lat/hRe/hIm in '{}'", nc_path);
            return false;
        }

        // Read scale_factor / add_offset from the packed int16 variables.
        // The netCDF C library, unlike the Python netCDF4 binding, does not
        // apply these: nc_get_vara_double returns the raw integer values cast
        // to double, so the caller scales them. EOT20 packs hRe/hIm as int16
        // with scale_factor ≈ 5.34e-5 (m/LSB); unscaled amplitudes would be
        // about 20 000× too large (meters instead of millimeters).
        auto getScaleOffset = [&](int varid, double& scale, double& offset) {
            scale  = 1.0;
            offset = 0.0;
            // Errors are non-fatal: an absent attribute means no packing,
            // i.e. scale=1, offset=0, which is the correct default.
            (void)nc_get_att_double(ncid, varid, "scale_factor", &scale);
            (void)nc_get_att_double(ncid, varid, "add_offset",   &offset);
        };
        double scale_re = 1.0, offset_re = 0.0;
        double scale_im = 1.0, offset_im = 0.0;
        getScaleOffset(varid_hRe, scale_re, offset_re);
        getScaleOffset(varid_hIm, scale_im, offset_im);

        std::vector<double> lon_arr(n_lon), lat_arr(n_lat);
        if (nc_get_var_double(ncid, varid_lon, lon_arr.data()) != NC_NOERR ||
            nc_get_var_double(ncid, varid_lat, lat_arr.data()) != NC_NOERR) {
            log->error("EOT20: failed to read lon/lat arrays");
            return false;
        }

        // Station geodetic coordinates
        const Coordinates::Llh llh = Coordinates::ecefToLlh(station_ecef);
        const double lat_deg = llh.lat_rad * 180.0 / Constants::PI;
        double lon_deg = llh.lon_rad * 180.0 / Constants::PI;
        // Normalize lon to [0, 360) so it falls inside lon_arr's interior
        // (lon_arr[0] = -0.125, lon_arr[n_lon-1] = 360.125 are boundary repeats).
        lon_deg = std::fmod(lon_deg, 360.0);
        if (lon_deg < 0.0) lon_deg += 360.0;

        // Find bracketing indices. lon ascending, lat descending.
        size_t j_lon = 0;
        {
            // Largest j_lon such that lon_arr[j_lon] <= lon_deg.
            // Binary search.
            size_t lo = 0, hi = n_lon - 1;
            while (lo + 1 < hi) {
                size_t m = (lo + hi) / 2;
                if (lon_arr[m] <= lon_deg) lo = m;
                else hi = m;
            }
            j_lon = lo;
        }
        size_t i_lat = 0;
        {
            // lat descending: largest i_lat such that lat_arr[i_lat] >= lat_deg.
            size_t lo = 0, hi = n_lat - 1;
            while (lo + 1 < hi) {
                size_t m = (lo + hi) / 2;
                if (lat_arr[m] >= lat_deg) lo = m;
                else hi = m;
            }
            i_lat = lo;
        }

        if (j_lon + 1 >= n_lon || i_lat + 1 >= n_lat) {
            log->error("EOT20: station at lat={:.4f} lon={:.4f} fell outside grid", lat_deg, lon_deg);
            return false;
        }

        const double dlon = lon_arr[j_lon + 1] - lon_arr[j_lon];
        const double dlat = lat_arr[i_lat + 1] - lat_arr[i_lat];  // negative (descending)
        if (!(std::abs(dlon) > 1e-9) || !(std::abs(dlat) > 1e-9)) {
            log->error("EOT20: degenerate grid spacing dlon={} dlat={}", dlon, dlat);
            return false;
        }
        const double flon = (lon_deg - lon_arr[j_lon]) / dlon;
        const double flat = (lat_deg - lat_arr[i_lat]) / dlat;  // ratio of two negatives → in [0, 1]

        // Read the 17 × 2 × 2 hyperslab. The C library returns raw int16 values as
        // double; scale_factor and add_offset are applied below.
        const size_t start[3] = { 0,        i_lat, j_lon };
        const size_t count[3] = { n_const,  2,     2     };
        std::vector<double> hRe_box(n_const * 4), hIm_box(n_const * 4);
        if (nc_get_vara_double(ncid, varid_hRe, start, count, hRe_box.data()) != NC_NOERR ||
            nc_get_vara_double(ncid, varid_hIm, start, count, hIm_box.data()) != NC_NOERR) {
            log->error("EOT20: failed to read hRe/hIm hyperslab at lat[{}], lon[{}]", i_lat, j_lon);
            return false;
        }
        for (auto& v : hRe_box) v = v * scale_re + offset_re;
        for (auto& v : hIm_box) v = v * scale_im + offset_im;

        // Bilinear at the 2×2 corner block, layout per constituent: [lat0,lon0; lat0,lon1; lat1,lon0; lat1,lon1]
        auto bilinear = [flat, flon](const double* box) {
            return (1.0 - flat) * ((1.0 - flon) * box[0] + flon * box[1])
                 +        flat  * ((1.0 - flon) * box[2] + flon * box[3]);
        };

        // EOT20 constituent order:
        //   0:2n2  1:j1  2:k1  3:k2  4:m2  5:m4  6:mf  7:mm
        //   8:n2   9:o1 10:p1 11:q1 12:s1 13:s2 14:sa 15:ssa 16:t2
        // BLQ order (OTL_CONSTITUENT_NAMES):  M2 S2 N2 K2 K1 O1 P1 Q1 Mf Mm Ssa
        static constexpr int EOT20_INDICES_FOR_BLQ[11] = { 4, 13, 8, 3, 2, 9, 10, 11, 6, 7, 15 };

        constexpr double R2D = 180.0 / Constants::PI;
        for (size_t k = 0; k < 11; ++k) {
            const size_t e = static_cast<size_t>(EOT20_INDICES_FOR_BLQ[k]);
            const double* re_corners = &hRe_box[e * 4];
            const double* im_corners = &hIm_box[e * 4];
            const double re = bilinear(re_corners);
            const double im = bilinear(im_corners);

            // FES/EOT convention: complex Z = re + i·im, with the real signal
            //   h(t) = Re{ Z · exp(i·χ(t)) } = A·cos(χ - φ),
            //   A = sqrt(re² + im²),  φ = atan2(-im, re).
            // φ in BLQ format is the Greenwich phase lag in degrees.
            const double amp = std::sqrt(re * re + im * im);
            const double phase_deg = std::atan2(-im, re) * R2D;

            coeffs.constituents[k].amp_up    = amp;
            coeffs.constituents[k].phase_up  = phase_deg;
            // EOT20_load.nc carries only the radial component. The horizontal
            // OTL signal (typically 1–3 mm peak) is left at zero.
            coeffs.constituents[k].amp_west   = 0.0;
            coeffs.constituents[k].amp_south  = 0.0;
            coeffs.constituents[k].phase_west = 0.0;
            coeffs.constituents[k].phase_south= 0.0;
        }

        coeffs.station_name = "EOT20-synthesized";
        coeffs.valid = true;

        log->info(
            "EOT20 synthesized OTL at lat={:.4f}° lon={:.4f}°: "
            "M2 up={:.4f} m / {:+.1f}°, S2 up={:.4f} m, K1 up={:.4f} m",
            lat_deg, lon_deg,
            coeffs.constituents[0].amp_up, coeffs.constituents[0].phase_up,
            coeffs.constituents[1].amp_up,
            coeffs.constituents[4].amp_up);

        return true;
    }

} // namespace GnssProcessor
