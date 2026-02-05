/**
 * @file Troposphere.cpp
 * @brief Implementation of troposphere utilities: mapping functions and ZHD.
 *
 * Implements:
 *  - Conversion ECEF -> geodetic LLH (minimal WGS84 Bowring-style iteration).
 *  - Standard-atmosphere pressure model used to estimate station pressure from height.
 *  - Troposphere class methods: constructor, mapping functions and Saastamoinen ZHD.
 */

#include "GnssConfig.hpp"
#include "Troposphere.hpp"
#include "Constants.hpp"
#include "Coordinates.hpp"
#include <Eigen/Dense>
#include <algorithm>
#include <cmath>

namespace GnssProcessor {

    namespace {
        /**
         * @brief Geodetic latitude/longitude/height container.
         *
         * Units:
         *  - lat_rad: latitude in radians
         *  - lon_rad: longitude in radians
         *  - h_m:     height above ellipsoid in meters
         */
        struct LlhRadM {
            double lat_rad = 0.0;
            double lon_rad = 0.0;
            double h_m = 0.0;
        };

        /**
         * @brief Standard atmosphere pressure model (troposphere lapse-rate).
         *
         * Computes pressure at height h_m from sea-level pressure/temperature
         * using the tropospheric lapse-rate approximation:
         *   P = P0 * (1 - L*h/T0)^(g/(R*L))
         *
         * The function clamps intermediate values to reasonable ranges to avoid
         * numerical issues. Inputs are expected as:
         *  - P0_mbar: sea-level pressure in millibar
         *  - T0_K:    sea-level temperature in Kelvin
         *  - h_m:     height above sea-level in meters
         *
         * @param P0_mbar Sea-level pressure in mbar.
         * @param T0_K Sea-level temperature in Kelvin.
         * @param h_m Height above sea-level in meters.
         * @return Pressure at height h_m in mbar (clamped to [100, 1100]).
         */
        double pressureFromHeightStdAtm(double P0_mbar, double T0_K, double h_m) {
            // P = P0 * (1 - L*h/T0)^(g/(R*L))
            constexpr double g = 9.80665;  // m/s^2
            constexpr double R = 287.05;   // J/(kg*K)
            constexpr double L = 0.0065;   // K/m

            if (!std::isfinite(P0_mbar) || P0_mbar <= 0.0 || !std::isfinite(T0_K) || T0_K <= 0.0) {
                return 1013.25;
            }

            double base = 1.0 - (L * h_m) / T0_K;
            base = std::clamp(base, 0.1, 2.0);

            const double exponent = g / (R * L); // ~5.25588
            double P = P0_mbar * std::pow(base, exponent);

            // Reasonable clamp (mbar)
            return std::clamp(P, 100.0, 1100.0);
        }
    }

    /**
     * @brief Construct a Troposphere helper.
     *
     * The constructor logs initialization details using the provided LogManager.
     *
     * @param config Reference to the GnssConfig (read-only).
     * @param log_mgr Reference to the LogManager instance.
     */
    Troposphere::Troposphere(const GnssConfig& config, LogManager& log_mgr)
        : config_(config), log_manager_(log_mgr) {
        log_manager_.getLogger("troposphere")->info("Troposphere model initialized: mode={}, elev_mask={}°",
            config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK", config_.getElevationMaskDeg());
    }

    /**
     * @brief Compute tropospheric mapping functions.
     *
     * Computes simple empirical hydrostatic (dry) and wet mapping functions
     * as a function of elevation angle (degrees). If elevation is outside
     * [0, 90] degrees, the function logs a warning and returns default 1.0
     * mappings.
     *
     * @param elevation Elevation angle in degrees.
     * @param[out] mf_d Output hydrostatic (dry) mapping function (unitless).
     * @param[out] mf_w Output wet mapping function (unitless).
     */
    void Troposphere::getTroposphereMapping(double elevation, double& mf_d, double& mf_w) const {
        if (elevation < 0.0 || elevation > 90.0) {
            log_manager_.getLogger("ppp_model")->warn("Invalid elevation for mapping: {:.3f} deg", elevation);
            mf_d = 1.0;
            mf_w = 1.0;
            return;
        }
        double el_rad = elevation * Constants::PI / 180.0;
        double sin_el = std::sin(el_rad);
        double tan_el = std::tan(el_rad);
        constexpr double MIN_DENOM = 0.01;
        double denom_d = sin_el + 0.00143 / (tan_el + 0.0445);
        double denom_w = sin_el + 0.00035 / (tan_el + 0.0383);
        if (denom_d < MIN_DENOM) denom_d = MIN_DENOM;
        if (denom_w < MIN_DENOM) denom_w = MIN_DENOM;
        mf_d = 1.0 / denom_d;  // Hydrostatic
        mf_w = 1.0 / denom_w;  // Wet
        if (!std::isfinite(mf_d) || !std::isfinite(mf_w)) {
            log_manager_.getLogger("ppp_model")->warn("Invalid mappings: mf_d={:.3f}, mf_w={:.3f}, elevation={:.3f} deg", mf_d, mf_w, elevation);
            mf_d = 1.0;
            mf_w = 1.0;
        }
        log_manager_.getLogger("ppp_model")->trace("Mappings: elevation={:.3f} deg, mf_d={:.3f}, mf_w={:.3f}", elevation, mf_d, mf_w);
    }

    /**
     * @brief Compute Saastamoinen zenith hydrostatic delay (ZHD) in meters.
     *
     * Converts the receiver ECEF position to geodetic latitude and height,
     * estimates station pressure from height using the standard atmosphere
     * model (sea-level P/T read from config), and computes the Saastamoinen
     * hydrostatic ZHD:
     *
     *   ZHD = (0.0022768 * P) / f(lat, h)
     *
     * where f(lat,h) applies a latitude and height correction.
     *
     * @param receiver_pos Receiver ECEF position in meters.
     * @return Zenith hydrostatic delay (ZHD) in meters.
     */
    double Troposphere::computeSaastamoinenZHD(const Eigen::Vector3d& receiver_pos) const {
        // WGS84 LLH (geodetic)
        const Coordinates::Llh llh = Coordinates::ecefToLlh(receiver_pos);
        const double lat_rad = llh.lat_rad;
        const double h_m = llh.h_m;

        // Estimate station pressure from height using standard atmosphere.
        // Interprets config pressure/temp as sea-level defaults.
        const double P0 = config_.getZhdPressure(); // mbar (sea-level default)
        const double T0 = config_.getZhdTemp();     // K (sea-level default)
        const double P = pressureFromHeightStdAtm(P0, T0, h_m);

        // Saastamoinen hydrostatic (ZHD) [m]
        const double f_lat = 1.0 - 0.00266 * std::cos(2.0 * lat_rad) - 0.00028 * h_m / 1000.0;
        double f_use = f_lat;
        if (!std::isfinite(f_use) || f_use <= 0.0) {
            log_manager_.getLogger("ppp_model")->warn("Invalid f_lat in ZHD computation: f_lat={:.6f}, lat_rad={:.6f}, h_m={:.3f}",
                f_use, lat_rad, h_m);
            f_use = 1.0;
        }

        const double zhd = (0.0022768 * P) / f_use;

        log_manager_.getLogger("ppp_model")->trace(
            "ZHD: lat={:.6f} rad, h={:.1f} m, P0={:.1f} mbar, T0={:.2f} K, P(h)={:.1f} mbar, zhd={:.3f} m",
            lat_rad, h_m, P0, T0, P, zhd);

        return zhd;
    }

}