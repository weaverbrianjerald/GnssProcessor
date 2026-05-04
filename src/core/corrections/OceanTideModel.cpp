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
     *   l    Mean anomaly of the Moon  (F1)
     *   l'   Mean anomaly of the Sun   (F2)
     *   F    Mean argument of latitude of the Moon (F3)
     *   D    Mean elongation of Moon from Sun (F4)
     *   ?    Longitude of ascending node of Moon (F5)
     *   ?    Greenwich Mean Sidereal Time (GMST)
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

    Eigen::Vector3d OceanTideModel::computeApproximateDisplacement(const Eigen::Vector3d& /*rx_pos*/,
                                                                    const GnssTime& /*time*/) const {
        // No station-specific OTL coefficients (BLQ or EOT20) are loaded.
        //
        // The previous fallback synthesized a phase-blind, latitude-scaled OTL
        // signal from global-average coastal amplitudes. With a wrong Greenwich
        // phase that signal injects a coherent biased oscillation into the up
        // channel that the filter cannot absorb cleanly — for many stations it
        // is worse than no correction at all. Returning zero lets the receiver
        // clock and ZTD absorb residual OTL energy without fighting a fictitious
        // model.
        //
        // To get real OTL displacement, configure either:
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
     * We read a 17 × 2 × 2 hyperslab (≈ 544 bytes after auto-scaling), bilinear-
     * interpolate each constituent at the station's (lat, lon), and pick the
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

        // RAII close — every early return below will unwind through this.
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

        // Pull scale_factor / add_offset from the packed int16 variables.
        // The netCDF C library (unlike the Python netCDF4 binding) does NOT
        // auto-apply these — nc_get_vara_double returns the raw integer
        // values cast to double, and the user must scale manually. EOT20
        // packs hRe/hIm as int16 with scale_factor ≈ 5.34e-5 (m/LSB);
        // without applying it, the synthesized amplitudes come out ~20 000×
        // too large (meters of "OTL" instead of millimeters).
        auto getScaleOffset = [&](int varid, double& scale, double& offset) {
            scale  = 1.0;
            offset = 0.0;
            // Errors are non-fatal — absent attributes mean "no packing,"
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

        // Read 17 × 2 × 2 hyperslab. The C library returns raw int16-as-double;
        // we apply scale_factor and add_offset manually.
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
