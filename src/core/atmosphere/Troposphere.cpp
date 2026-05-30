/**
 * @file Troposphere.cpp
 * @brief Implementation of troposphere utilities: GMF mapping functions and ZHD.
 *
 * Implements:
 *  - Global Mapping Function (GMF) by Boehm et al. (2006) for hydrostatic/wet mapping.
 *  - Standard-atmosphere pressure model used to estimate station pressure from height.
 *  - Saastamoinen zenith hydrostatic delay (ZHD).
 *
 * Reference:
 *   Boehm, J., A. Niell, P. Tregoning, H. Schuh (2006),
 *   Global Mapping Function (GMF): A new empirical mapping function based on
 *   numerical weather model data, Geophys. Res. Lett., 33, L07304.
 */

#include "config/GnssConfig.hpp"
#include "atmosphere/Troposphere.hpp"
#include "atmosphere/Gpt3.hpp"
#include "atmosphere/Vmf3.hpp"
#include "common/Constants.hpp"
#include "math/Coordinates.hpp"
#include <Eigen/Dense>
#include <algorithm>
#include <cmath>

namespace GnssProcessor {

    namespace {
        /**
         * @brief Standard atmosphere pressure model (troposphere lapse-rate).
         *
         * Computes pressure at height h_m from sea-level pressure/temperature
         * using the tropospheric lapse-rate approximation:
         *   P = P0 * (1 - L*h/T0)^(g/(R*L))
         *
         * @param P0_mbar Sea-level pressure in mbar.
         * @param T0_K Sea-level temperature in Kelvin.
         * @param h_m Height above sea-level in meters.
         * @return Pressure at height h_m in mbar (clamped to [100, 1100]).
         */
        double pressureFromHeightStdAtm(double P0_mbar, double T0_K, double h_m) {
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

            return std::clamp(P, 100.0, 1100.0);
        }
    }

    Troposphere::Troposphere(const GnssConfig& config, LogManager& log_mgr)
        : config_(config), log_manager_(log_mgr) {
        auto logger = log_manager_.getLogger("troposphere");
        const bool want_vmf3 =
            config_.getTroposphereMappingFunction() == TroposphereMappingFunction::Vmf3;
        const bool want_gpt3_apriori =
            config_.getTroposphereAprioriSource() == TroposphereAprioriSource::Gpt3;
        logger->info("Troposphere model initialized (mapping={}, apriori={}): mode={}, elev_mask={}°",
            want_vmf3 ? "VMF3" : "GMF",
            want_gpt3_apriori ? "GPT3" : "stdatm",
            config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK",
            config_.getElevationMaskDeg());

        if (want_vmf3 || want_gpt3_apriori) {
            const auto& grid_path = config_.getGpt3GridPath();
            if (grid_path.empty()) {
                logger->warn("GPT3 grid required (mapping=VMF3 or apriori=GPT3) but gpt3_grid_path is empty; falling back to GMF/stdatm");
            } else {
                gpt3_ = std::make_unique<Gpt3>();
                if (!gpt3_->load(grid_path, log_manager_)) {
                    logger->warn("GPT3 grid load failed ('{}'); falling back to GMF/stdatm", grid_path.string());
                    gpt3_.reset();
                }
            }
        }
    }

    Troposphere::~Troposphere() = default;

    /**
     * @brief Evaluate the Marini continued-fraction mapping function form.
     *
     *   mf(el) = (1 + a/(1 + b/(1 + c))) / (sin(el) + a/(sin(el) + b/(sin(el) + c)))
     */
    double Troposphere::mariniMapping(double sin_el, double a, double b, double c) {
        double numer = 1.0 + a / (1.0 + b / (1.0 + c));
        double denom = sin_el + a / (sin_el + b / (sin_el + c));
        if (std::abs(denom) < 1e-12) return 1.0;
        return numer / denom;
    }

    /**
     * @brief Compute day-of-year from GnssTime.
     */
    double Troposphere::getDayOfYear(const GnssTime& time) {
        int year, month, day, hour, minute;
        double second;
        TimeUtils::gpsTimeToGregorian(time, year, month, day, hour, minute, second);

        // Day of year: accumulate days of preceding months
        static const int days_in_month[] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        int doy = day;
        for (int m = 1; m < month; ++m) {
            doy += days_in_month[m];
        }
        // Leap year correction
        bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        if (leap && month > 2) doy += 1;

        // Add fractional day
        double frac = (hour + minute / 60.0 + second / 3600.0) / 24.0;
        return doy + frac;
    }

    /**
     * @brief Compute tropospheric mapping functions using the Global Mapping Function (GMF).
     *
     * The GMF provides latitude-, height-, and season-dependent mapping coefficients
     * (a, b, c) for both hydrostatic and wet components using a Marini continued
     * fraction form. The coefficients are modelled as a mean plus annual harmonic.
     *
     * Reference: Boehm et al. (2006), Geophys. Res. Lett., 33, L07304.
     */
    void Troposphere::getTroposphereMapping(double elevation,
                                            const Eigen::Vector3d& receiver_pos,
                                            const GnssTime& time,
                                            double& mf_d, double& mf_w) const {
        if (elevation < 0.0 || elevation > 90.0) {
            log_manager_.getLogger("ppp_model")->warn("Invalid elevation for mapping: {:.3f} deg", elevation);
            mf_d = 1.0;
            mf_w = 1.0;
            return;
        }

        double el_rad = elevation * Constants::PI / 180.0;
        double sin_el = std::sin(el_rad);

        // If receiver position is invalid, fall back to simple mapping
        if (receiver_pos.norm() < 1e3 || !receiver_pos.allFinite()) {
            // Fallback: simple Black & Eisner mapping
            double tan_el = std::tan(el_rad);
            constexpr double MIN_DENOM = 0.01;
            double denom_d = sin_el + 0.00143 / (tan_el + 0.0445);
            double denom_w = sin_el + 0.00035 / (tan_el + 0.0383);
            if (denom_d < MIN_DENOM) denom_d = MIN_DENOM;
            if (denom_w < MIN_DENOM) denom_w = MIN_DENOM;
            mf_d = 1.0 / denom_d;
            mf_w = 1.0 / denom_w;
            return;
        }

        // Convert receiver position to geodetic
        Coordinates::Llh llh = Coordinates::ecefToLlh(receiver_pos);
        double lat = llh.lat_rad;        // radians
        double h_km = llh.h_m / 1000.0;  // km

        // Day-of-year for annual variation (phase reference: Jan 28 for NH)
        double doy = getDayOfYear(time);

        // --- VMF3 path (empirical from GPT3): early-out before GMF fallthrough ---
        // Uses a_h/a_w bilinearly interpolated from the GPT3 grid and b/c
        // synthesized from the degree/order-12 spherical-harmonic expansion in
        // Vmf3Coefficients.hpp.  On any GPT3 query failure we silently fall
        // through to GMF so the engine never aborts on a single bad sample.
        if (config_.getTroposphereMappingFunction() == TroposphereMappingFunction::Vmf3 && gpt3_) {
            const double lat_deg = lat * 180.0 / Constants::PI;
            const double lon_deg = llh.lon_rad * 180.0 / Constants::PI;
            Gpt3::Result r{};
            if (gpt3_->query(lat_deg, lon_deg, llh.h_m, doy, r)) {
                Vmf3::computeMapping(lat, llh.lon_rad, doy, el_rad, r.a_h, r.a_w, mf_d, mf_w);

                // Niell height correction for the hydrostatic mapping is the
                // same formula GMF uses; VMF3's a coefficient lives at the
                // grid's orthometric height so applying the correction here
                // brings it down to the station height.
                static constexpr double a_ht = 2.53e-5;
                static constexpr double b_ht = 5.49e-3;
                static constexpr double c_ht = 1.14e-3;
                const double ht_corr_coeff = 1.0 / sin_el - mariniMapping(sin_el, a_ht, b_ht, c_ht);
                mf_d += ht_corr_coeff * h_km;

                if (!std::isfinite(mf_d) || !std::isfinite(mf_w) || mf_d < 1.0 || mf_w < 1.0) {
                    log_manager_.getLogger("ppp_model")->warn(
                        "Invalid VMF3 mappings: mf_d={:.3f}, mf_w={:.3f}, elevation={:.3f} deg",
                        mf_d, mf_w, elevation);
                    mf_d = std::max(mf_d, 1.0);
                    mf_w = std::max(mf_w, 1.0);
                }
                log_manager_.getLogger("ppp_model")->trace(
                    "VMF3 mappings: elevation={:.3f} deg, lat={:.3f} deg, h={:.1f} m, doy={:.1f}, a_h={:.6e}, a_w={:.6e}, mf_d={:.6f}, mf_w={:.6f}",
                    elevation, lat_deg, llh.h_m, doy, r.a_h, r.a_w, mf_d, mf_w);
                return;
            }
            // GPT3 query failed — fall through to GMF below.
        }

        // Phase offset: DOY 28 for northern hemisphere, DOY 28 + 182.625 for southern
        double doy_phase = (lat >= 0.0) ? 28.0 : 28.0 + 182.625;
        double cos_annual = std::cos(2.0 * Constants::PI * (doy - doy_phase) / 365.25);

        double abs_lat = std::abs(lat); // radians

        // === GMF coefficients (Boehm et al. 2006, Table 1) ===
        // 5 data points at latitudes: 15°, 30°, 45°, 60°, 75°
        //
        // Hydrostatic: mean and annual amplitude
        static constexpr double ah_mean[] = { 1.2769934e-3, 1.2683230e-3, 1.2465397e-3,
                                               1.2196049e-3, 1.2045996e-3 };
        static constexpr double ah_amp[]  = { 1.2709626e-5, 2.6523662e-5,
                                               3.4000452e-5, 4.1202191e-5, 4.1202191e-5 };

        static constexpr double bh_mean[] = { 2.9153695e-3, 2.9152299e-3, 2.9288445e-3,
                                               2.9022565e-3, 2.9024912e-3 };
        static constexpr double bh_amp[]  = { 2.1414979e-5, 3.0160779e-5,
                                               7.2562722e-5, 11.723375e-5, 11.723375e-5 };

        static constexpr double ch_mean[] = { 62.610505e-3, 62.837393e-3, 63.721774e-3,
                                               63.824265e-3, 64.258455e-3 };
        static constexpr double ch_amp[]  = { 9.0128400e-5, 4.3497037e-5,
                                               84.795348e-5, 170.37206e-5, 170.37206e-5 };

        // Wet: mean only (no annual variation in GMF wet)
        static constexpr double aw_mean[] = { 5.8021897e-4, 5.6794847e-4, 5.8118019e-4,
                                               5.9727542e-4, 6.1641693e-4 };
        static constexpr double bw_mean[] = { 1.4275268e-3, 1.5138625e-3, 1.4572752e-3,
                                               1.5007428e-3, 1.7599082e-3 };
        static constexpr double cw_mean[] = { 43.472961e-3, 46.729510e-3, 43.908931e-3,
                                               44.626982e-3, 54.736038e-3 };

        // Latitude interpolation index
        // Grid starts at 15° with 15° spacing: 15°, 30°, 45°, 60°, 75°
        // Latitudes below 15° use the 15° value; above 75° use the 75° value.
        double lat_deg = abs_lat * 180.0 / Constants::PI;
        double idx_frac = (lat_deg - 15.0) / 15.0;
        idx_frac = std::clamp(idx_frac, 0.0, 4.0);  // clamp to [0, 4] for 5 grid points
        int idx = static_cast<int>(idx_frac);
        if (idx >= 4) idx = 3; // clamp to last interval [60°, 75°]
        double frac = idx_frac - idx;
        frac = std::clamp(frac, 0.0, 1.0);

        // Interpolate hydrostatic coefficients (mean + annual amplitude)
        auto lerp = [](const double* arr, int i, double f) {
            return arr[i] * (1.0 - f) + arr[i + 1] * f;
        };

        double a_h = lerp(ah_mean, idx, frac) + lerp(ah_amp, idx, frac) * cos_annual;
        double b_h = lerp(bh_mean, idx, frac) + lerp(bh_amp, idx, frac) * cos_annual;
        double c_h = lerp(ch_mean, idx, frac) + lerp(ch_amp, idx, frac) * cos_annual;

        // Interpolate wet coefficients (no annual variation in GMF wet)
        double a_w = lerp(aw_mean, idx, frac);
        double b_w = lerp(bw_mean, idx, frac);
        double c_w = lerp(cw_mean, idx, frac);

        // Compute base mapping using Marini continued fraction
        mf_d = mariniMapping(sin_el, a_h, b_h, c_h);
        mf_w = mariniMapping(sin_el, a_w, b_w, c_w);

        // Height correction for hydrostatic mapping function
        // Boehm & Schuh (2004): mf_h(h) = mf_h(0) + (1/sin(el) - mf_h(0)) * h_km * dh
        // where dh is height-correction gradient
        static constexpr double a_ht = 2.53e-5;
        static constexpr double b_ht = 5.49e-3;
        static constexpr double c_ht = 1.14e-3;
        double ht_corr_coeff = 1.0 / sin_el - mariniMapping(sin_el, a_ht, b_ht, c_ht);
        mf_d += ht_corr_coeff * h_km; // h_km is already in km (h_m / 1000)

        if (!std::isfinite(mf_d) || !std::isfinite(mf_w) || mf_d < 1.0 || mf_w < 1.0) {
            log_manager_.getLogger("ppp_model")->warn("Invalid GMF mappings: mf_d={:.3f}, mf_w={:.3f}, elevation={:.3f} deg",
                mf_d, mf_w, elevation);
            // Clamp to reasonable values
            mf_d = std::max(mf_d, 1.0);
            mf_w = std::max(mf_w, 1.0);
        }

        log_manager_.getLogger("ppp_model")->trace("GMF mappings: elevation={:.3f} deg, lat={:.3f} deg, h={:.1f} m, doy={:.1f}, mf_d={:.6f}, mf_w={:.6f}",
            elevation, lat * 180.0 / Constants::PI, llh.h_m, doy, mf_d, mf_w);
    }

    double Troposphere::computeSaastamoinenZHD(const Eigen::Vector3d& receiver_pos,
                                                const GnssTime& time) const {
        const Coordinates::Llh llh = Coordinates::ecefToLlh(receiver_pos);
        const double lat_rad = llh.lat_rad;
        const double h_m = llh.h_m;

        // Default: standard-atmosphere reduction from configured sea-level pair.
        double P = pressureFromHeightStdAtm(config_.getZhdPressure(), config_.getZhdTemp(), h_m);
        const char* source = "stdatm";

        // GPT3 path: query the grid at this lat/lon/h/doy.  On any failure
        // we silently keep the standard-atmosphere fallback already in P.
        if (gpt3_ && gpt3_->isLoaded()) {
            const double lat_deg = lat_rad * 180.0 / Constants::PI;
            const double lon_deg = llh.lon_rad * 180.0 / Constants::PI;
            const double doy = getDayOfYear(time);
            Gpt3::Result r{};
            if (gpt3_->query(lat_deg, lon_deg, h_m, doy, r) &&
                std::isfinite(r.pressure_mbar) && r.pressure_mbar > 0.0) {
                P = r.pressure_mbar;
                source = "gpt3";
            }
        }

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
            "ZHD: lat={:.6f} rad, h={:.1f} m, P(h)={:.1f} mbar [{}], zhd={:.3f} m",
            lat_rad, h_m, P, source, zhd);

        return zhd;
    }

}