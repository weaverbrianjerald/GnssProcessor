/**
 * @file Gpt3.cpp
 * @brief Implementation of the GPT3 5° empirical pressure/temperature model.
 *
 * Implements:
 *  - ASCII grid load (1 header line + 2592 data rows, 64 columns each).  The VMF3
 *    "a" coefficients are stored x1000 in the file and are returned unscaled.  A
 *    failed load leaves the instance empty and unloaded.
 *  - Annual + semi-annual harmonic evaluation per cell.
 *  - Bilinear lat/lon interpolation with longitude wrap-around.
 *  - Exponential pressure reduction with the cell's virtual temperature, as in
 *    the GPT2 / GPT3 reference code, and linear temperature lapse, from the grid
 *    orthometric reference height to the station's orthometric height.
 *
 * References:
 *   Landskron, D., and J. Boehm (2018), VMF3/GPT3: refined discrete and
 *   empirical troposphere mapping functions, J. Geod., 92(4), 349–360.
 *   Lagler, K., M. Schindelegger, J. Boehm, H. Krasna, and T. Nilsson (2013),
 *   GPT2: Empirical slant delay model for radio space geodetic techniques,
 *   Geophys. Res. Lett., 40, 1069–1073 (the pressure reduction GPT3 keeps).
 */

#include "atmosphere/Gpt3.hpp"
#include "common/Constants.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

namespace GnssProcessor {

    namespace {
        // Number of whitespace-separated columns per data row in gpt3_5.grd.
        constexpr int N_COLS = 64;
        // Pressure reduction of the GPT2 / GPT3 reference code:
        //   P(h) = P(Hs) * exp(-c * dh),  c = G_MEAN * M_DRY / (R_GAS * Tv),
        //   Tv   = T0 * (1 + VIRTUAL_T_Q * Q),
        // with T0 the cell's temperature and Q its specific humidity (kg/kg), both at
        // the grid height.  The scale height follows the cell's own air: near 7.2 km
        // over the Antarctic plateau against 8.4 km at 288 K, which matters where a
        // corner cell lies a kilometre or more above the station.
        constexpr double G_MEAN = 9.80665;      // m/s^2
        constexpr double M_DRY = 28.965e-3;     // kg/mol, molar mass of dry air
        constexpr double R_GAS = 8.3143;        // J/(mol K), as in the reference code
        constexpr double VIRTUAL_T_Q = 0.6077;  // virtual-temperature factor per kg/kg of humidity
        // The grid file stores specific humidity in g/kg.
        constexpr double Q_SCALE = 1.0e-3;
        // Temperature lapse uses dT [K/km] applied to dh [m] × 1e-3.
        constexpr double KM_PER_M = 1.0e-3;
        // The grid file stores the VMF3 hydrostatic and wet "a" coefficients
        // multiplied by 1000 (a_h a0 of about 1.2 for a physical 1.2e-3).  load()
        // converts them to the physical, dimensionless values.
        constexpr double A_COEFF_SCALE = 1.0e-3;
        // Plausible range of the stored (x1000) a0 values.  The shipped grid spans
        // 1.136 to 1.280 for a_h and 0.383 to 0.657 for a_w.  A cell outside these
        // ranges means the file is in other units, so the load is refused rather than
        // returning mapping coefficients that are off by orders of magnitude.
        constexpr double A_H_STORED_MIN = 0.8;
        constexpr double A_H_STORED_MAX = 1.8;
        constexpr double A_W_STORED_MIN = 0.2;
        constexpr double A_W_STORED_MAX = 1.2;
    } // namespace

    double Gpt3::harmonic(const double coeffs[5], double doy) {
        const double omega = 2.0 * Constants::PI * doy / 365.25;
        const double c1 = std::cos(omega);
        const double s1 = std::sin(omega);
        const double c2 = std::cos(2.0 * omega);
        const double s2 = std::sin(2.0 * omega);
        return coeffs[0] + coeffs[1] * c1 + coeffs[2] * s1 + coeffs[3] * c2 + coeffs[4] * s2;
    }

    bool Gpt3::load(const std::filesystem::path& grid_file, LogManager& log_mgr) {
        auto logger = log_mgr.getLogger("gpt3");

        // A failed load leaves the instance empty and unloaded, including an instance
        // that held a grid before: every return below leaves cells_ empty and
        // loaded_ false until the final success.
        loaded_ = false;
        cells_.clear();

        std::ifstream f(grid_file);
        if (!f.is_open()) {
            logger->error("GPT3: cannot open grid file '{}'", grid_file.string());
            return false;
        }

        std::string line;
        if (!std::getline(f, line)) {
            logger->error("GPT3: grid file '{}' is empty", grid_file.string());
            return false;
        }
        // Header sanity: starts with '%'.
        if (line.empty() || line.front() != '%') {
            logger->error("GPT3: grid file '{}' missing '%' header line", grid_file.string());
            return false;
        }

        cells_.clear();
        cells_.reserve(static_cast<size_t>(N_LAT) * N_LON);

        size_t row = 0;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::istringstream iss(line);
            double v[N_COLS];
            int parsed = 0;
            while (parsed < N_COLS && (iss >> v[parsed])) {
                ++parsed;
            }
            if (parsed < N_COLS) {
                logger->error("GPT3: row {} has only {} columns (expected {})",
                              row, parsed, N_COLS);
                cells_.clear();
                return false;
            }

            // The a0 of the two mapping coefficients is checked in the file's units
            // before scaling.
            if (!(v[24] >= A_H_STORED_MIN && v[24] <= A_H_STORED_MAX) ||
                !(v[29] >= A_W_STORED_MIN && v[29] <= A_W_STORED_MAX)) {
                logger->error("GPT3: row {} has a_h a0 {} and a_w a0 {}, outside the expected "
                              "[{}, {}] and [{}, {}] of a grid storing the coefficients x1000",
                              row, v[24], v[29], A_H_STORED_MIN, A_H_STORED_MAX,
                              A_W_STORED_MIN, A_W_STORED_MAX);
                cells_.clear();
                return false;
            }

            Cell c;
            for (int i = 0; i < 5; ++i) c.p[i]   = v[2  + i];  // pressure (Pa)
            for (int i = 0; i < 5; ++i) c.T[i]   = v[7  + i];  // temperature (K)
            for (int i = 0; i < 5; ++i) c.Q[i]   = v[12 + i];  // specific humidity (g/kg)
            for (int i = 0; i < 5; ++i) c.dT[i]  = v[17 + i];  // lapse (K/km)
            c.undu = v[22];
            c.Hs   = v[23];
            for (int i = 0; i < 5; ++i) c.a_h[i] = v[24 + i] * A_COEFF_SCALE;  // VMF hydrostatic a
            for (int i = 0; i < 5; ++i) c.a_w[i] = v[29 + i] * A_COEFF_SCALE;  // VMF wet a
            cells_.push_back(c);
            ++row;
        }

        const size_t expected = static_cast<size_t>(N_LAT) * N_LON;
        if (cells_.size() != expected) {
            logger->error("GPT3: loaded {} cells, expected {}", cells_.size(), expected);
            cells_.clear();
            return false;
        }

        loaded_ = true;
        logger->info("GPT3: loaded {} cells from '{}'", cells_.size(), grid_file.string());
        return true;
    }

    bool Gpt3::query(double lat_deg, double lon_deg, double h_ellipsoidal_m, double doy,
                     Result& out) const {
        if (!loaded_) return false;
        if (!std::isfinite(lat_deg) || !std::isfinite(lon_deg) ||
            !std::isfinite(h_ellipsoidal_m) || !std::isfinite(doy)) {
            return false;
        }

        // --- latitude bracket ---
        // Grid latitudes run 87.5°, 82.5°, ..., -87.5° (north to south).
        double lat_idx = (LAT_FIRST - lat_deg) / LAT_STEP;
        lat_idx = std::clamp(lat_idx, 0.0, static_cast<double>(N_LAT - 1));
        int i_lat = static_cast<int>(std::floor(lat_idx));
        if (i_lat >= N_LAT - 1) i_lat = N_LAT - 2;
        const double f_lat = lat_idx - i_lat;

        // --- longitude bracket with wrap-around ---
        // Grid longitudes run 2.5°, 7.5°, ..., 357.5° east, wrapping.
        double lon_norm = std::fmod(lon_deg, 360.0);
        if (lon_norm < 0.0) lon_norm += 360.0;
        double lon_idx = (lon_norm - LON_FIRST) / LON_STEP;
        while (lon_idx < 0.0)               lon_idx += N_LON;
        while (lon_idx >= N_LON)            lon_idx -= N_LON;
        int i_lon = static_cast<int>(std::floor(lon_idx));
        if (i_lon < 0) i_lon = 0;
        if (i_lon >= N_LON) i_lon = N_LON - 1;
        const double f_lon = lon_idx - i_lon;
        const int i_lon_next = (i_lon + 1) % N_LON;

        // Per-corner reduction: harmonic eval of P/T/Q/dT/a_h/a_w followed by
        // the exponential (pressure) and lapse (temperature) extrapolation from
        // grid Hs to station h_ortho.  a_h/a_w have no height reduction: VMF puts
        // the height correction into the mapping function itself.
        struct CornerVals { double P_mbar, T_K, a_h, a_w; };
        auto reduce = [&](const Cell& c) -> CornerVals {
            const double p_grid_Pa  = harmonic(c.p,  doy);
            const double T_grid_K   = harmonic(c.T,  doy);
            const double q_grid     = harmonic(c.Q,  doy) * Q_SCALE;
            const double dT_per_km  = harmonic(c.dT, doy);

            const double h_ortho = h_ellipsoidal_m - c.undu;
            const double dh = h_ortho - c.Hs;

            const double Tv_K = T_grid_K * (1.0 + VIRTUAL_T_Q * q_grid);
            const double c_per_m = G_MEAN * M_DRY / (R_GAS * Tv_K);
            const double p_station_Pa = p_grid_Pa * std::exp(-c_per_m * dh);
            const double T_station_K  = T_grid_K + dT_per_km * dh * KM_PER_M;

            const double a_h_val = harmonic(c.a_h, doy);
            const double a_w_val = harmonic(c.a_w, doy);
            return { p_station_Pa * 0.01, T_station_K, a_h_val, a_w_val };
        };

        const Cell& c00 = cells_[static_cast<size_t>(i_lat)     * N_LON + i_lon];
        const Cell& c01 = cells_[static_cast<size_t>(i_lat)     * N_LON + i_lon_next];
        const Cell& c10 = cells_[static_cast<size_t>(i_lat + 1) * N_LON + i_lon];
        const Cell& c11 = cells_[static_cast<size_t>(i_lat + 1) * N_LON + i_lon_next];

        const CornerVals v00 = reduce(c00);
        const CornerVals v01 = reduce(c01);
        const CornerVals v10 = reduce(c10);
        const CornerVals v11 = reduce(c11);

        const double w00 = (1.0 - f_lat) * (1.0 - f_lon);
        const double w01 = (1.0 - f_lat) * f_lon;
        const double w10 = f_lat         * (1.0 - f_lon);
        const double w11 = f_lat         * f_lon;

        out.pressure_mbar = w00 * v00.P_mbar + w01 * v01.P_mbar + w10 * v10.P_mbar + w11 * v11.P_mbar;
        out.temperature_K = w00 * v00.T_K    + w01 * v01.T_K    + w10 * v10.T_K    + w11 * v11.T_K;
        out.a_h           = w00 * v00.a_h    + w01 * v01.a_h    + w10 * v10.a_h    + w11 * v11.a_h;
        out.a_w           = w00 * v00.a_w    + w01 * v01.a_w    + w10 * v10.a_w    + w11 * v11.a_w;
        return true;
    }

} // namespace GnssProcessor
