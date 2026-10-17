#pragma once

/**
 * @file Gpt3.hpp
 * @brief Global Pressure and Temperature 3 (GPT3) empirical model.
 *
 * Provides station pressure and temperature lookups from the 5° GPT3 grid
 * (Landskron & Boehm 2018), an alternative to the standard-atmosphere
 * assumption (fixed 1013.25 mbar / 288.15 K at sea level) for the Saastamoinen
 * zenith hydrostatic delay, plus the VMF3 "a" coefficients.  The grid is a
 * single ASCII file (@c gpt3_5.grd, 36 latitudes × 72 longitudes, 64
 * parameters per cell) obtained from https://vmf.geo.tuwien.ac.at/codes/.
 *
 * Reference:
 *   Landskron, D., and J. Boehm (2018), VMF3/GPT3: refined discrete and
 *   empirical troposphere mapping functions, J. Geod., 92(4), 349–360.
 */

#include "logging/LogManager.hpp"
#include <filesystem>
#include <vector>

namespace GnssProcessor {

    /**
     * @class Gpt3
     * @brief GPT3 5° empirical pressure/temperature lookup.
     *
     * Loads the GPT3 5° grid once and answers @c query() calls for any
     * station position and day-of-year.  Pressure is reduced from the
     * grid's orthometric reference height @c Hs to the station's
     * orthometric height (ellipsoidal height minus grid undulation)
     * using the standard barometric form; temperature is reduced with
     * the grid's seasonally-varying lapse rate @c dT.
     *
     * The implementation returns pressure, temperature and the VMF3
     * hydrostatic and wet "a" coefficients.  The same file also carries
     * water-vapour content, the water-vapour decrease factor (lambda),
     * weighted-mean temperature and tropospheric gradients; these can be
     * added without changing the grid load.
     */
    class Gpt3 {
    public:
        /**
         * @brief Output of a successful @c query().
         *
         * @c a_h and @c a_w are the bilinearly interpolated, harmonic
         * VMF mapping-function "a" coefficients at the station's lat/lon
         * and DOY.  They are filled even when the Saastamoinen ZHD path
         * does not use them, so the same query call can drive both the
         * a priori P/T and the VMF3 mapping function in a single pass.
         */
        struct Result {
            double pressure_mbar;  /**< Total pressure at station, mbar. */
            double temperature_K;  /**< Temperature at station, Kelvin. */
            double a_h;            /**< VMF hydrostatic mapping "a" coefficient (dimensionless, about 1.2e-3). */
            double a_w;            /**< VMF wet mapping "a" coefficient (dimensionless, about 0.55e-3). */
        };

        /**
         * @brief Default-construct an empty (unloaded) model.
         *
         * @c isLoaded() returns false until @c load() succeeds.
         */
        Gpt3() = default;

        /**
         * @brief Load the 5° grid from the given file.
         *
         * Parses an ASCII @c gpt3_5.grd: one header line starting with '%',
         * followed by 36 × 72 = 2592 data rows of 64 whitespace-separated
         * values each (blank lines are skipped).  Rows iterate
         * longitude-fastest from (lat = 87.5, lon = 2.5) southwards/eastwards
         * in 5° steps.
         *
         * The file stores the VMF3 hydrostatic and wet "a" coefficients
         * multiplied by 1000 (a_h a0 of about 1.2 for a physical 1.2e-3).  The
         * load converts them, so @c query() returns the physical values.  A row
         * whose a_h a0 lies outside [0.8, 1.8] or whose a_w a0 lies outside
         * [0.2, 1.2] in the file's units is refused, because a file in other
         * units would otherwise return coefficients off by orders of magnitude.
         *
         * @param grid_file Filesystem path to @c gpt3_5.grd.
         * @param log_mgr   Logger used to report load progress / errors.
         * @return @c true if the grid loaded successfully (full 2592
         *         cells with no parse failures and every coefficient in range),
         *         @c false otherwise.  On failure the instance is left empty and
         *         unloaded (@c isLoaded() false), including an instance that
         *         held a grid before the call.
         */
        bool load(const std::filesystem::path& grid_file, LogManager& log_mgr);

        /**
         * @brief @c true once @c load() has populated the grid.
         */
        bool isLoaded() const { return loaded_; }

        /**
         * @brief Look up pressure, temperature and VMF3 coefficients at a station.
         *
         * Combines four-corner bilinear interpolation in latitude /
         * longitude with annual + semi-annual harmonic evaluation in
         * day-of-year, then reduces pressure (barometric, exponent
         * 5.225) and temperature (linear lapse rate @c dT) from the
         * grid's orthometric reference height to the station's
         * orthometric height (ellipsoidal height minus geoid undulation).
         * The VMF3 coefficients have no height reduction.
         *
         * Latitudes outside [-87.5°, 87.5°] are clamped to the edge
         * row; longitudes are wrapped into [0°, 360°).
         *
         * @param lat_deg            Geodetic latitude in degrees.
         * @param lon_deg            Geodetic longitude in degrees (any
         *                           range; normalized internally).
         * @param h_ellipsoidal_m    Station ellipsoidal height (meters).
         * @param doy                Fractional day-of-year
         *                           (1.0 = 1 Jan 00:00 UTC).
         * @param[out] out           Populated with pressure (mbar),
         *                           temperature (K), @c a_h and @c a_w
         *                           on success.
         * @return @c true on success, @c false if the model is unloaded
         *         or the inputs are not finite.
         */
        bool query(double lat_deg, double lon_deg, double h_ellipsoidal_m, double doy,
                   Result& out) const;

    private:
        static constexpr int N_LAT = 36;        /**< Latitude grid count (5° × 36 = 180°). */
        static constexpr int N_LON = 72;        /**< Longitude grid count (5° × 72 = 360°). */
        static constexpr double LAT_FIRST = 87.5;   /**< Northernmost grid latitude (deg). */
        static constexpr double LON_FIRST = 2.5;    /**< Westernmost grid longitude (deg). */
        static constexpr double LAT_STEP = 5.0;     /**< Latitude spacing (deg). */
        static constexpr double LON_STEP = 5.0;     /**< Longitude spacing (deg). */

        /**
         * @brief Per-cell parameters used by the query.
         *
         * Only the columns needed for pressure, temperature and the VMF3
         * "a" coefficients are retained; the remaining GPT3 columns
         * (humidity, lambda, Tm, gradients) are parsed and discarded, so
         * the file is read in its published format.
         */
        struct Cell {
            double p[5];   /**< Pressure harmonic (a0, A1, B1, A2, B2) in Pa. */
            double T[5];   /**< Temperature harmonic (a0, A1, B1, A2, B2) in K. */
            double dT[5];  /**< Temperature lapse-rate harmonic in K/km. */
            double undu;   /**< Geoid undulation at grid cell (m). */
            double Hs;     /**< Orthometric grid height (m). */
            double a_h[5]; /**< VMF hydrostatic "a" harmonic (dimensionless, file value x 1e-3). */
            double a_w[5]; /**< VMF wet "a" harmonic (dimensionless, file value x 1e-3). */
        };

        /**
         * @brief Evaluate the 5-coefficient annual + semi-annual harmonic.
         *
         * @c V(doy) = a0 + A1 cos(ω) + B1 sin(ω) + A2 cos(2ω) + B2 sin(2ω)
         * with @c ω = 2π × doy / 365.25.
         */
        static double harmonic(const double coeffs[5], double doy);

        /** @brief Flat row-major store: index = lat_idx × N_LON + lon_idx. */
        std::vector<Cell> cells_;
        bool loaded_{ false };
    };

} // namespace GnssProcessor
