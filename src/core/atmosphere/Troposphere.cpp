/**
 * @file Troposphere.cpp
 * @brief Implementation of troposphere utilities: mapping functions and ZHD.
 *
 * Implements:
 *  - Hydrostatic/wet mapping from the Niell (1996) NMF coefficient tables (the default
 *    path, labelled "GMF" in the method name and log text), or VMF3 with GPT3 "a"
 *    coefficients when the VMF3 mapping function is configured and the GPT3 grid loads.
 *  - Standard-atmosphere pressure model used to estimate station pressure from height.
 *  - Saastamoinen zenith hydrostatic delay (ZHD).
 *
 * References:
 *   Niell, A. E. (1996), Global mapping functions for the atmosphere delay at radio
 *   wavelengths, J. Geophys. Res., 101(B2), 3227-3246.
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
#include <filesystem>

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
            // An explicit receiver.gpt3_grid_path takes precedence; otherwise the
            // bundled static grid under the internal data root is used.  GPT3 is a
            // date-independent static asset, so it is resolved here rather than
            // through PathResolver's date-aware product logic, as ModelGeometry does
            // for the ocean-pole-tide and atmospheric-loading grids.  The config
            // loader cannot set this default because the internal data root is
            // resolved only after the receiver section parses (see
            // GnssConfig::loadReceiverSection_); the root is known by the time this
            // constructor runs.
            std::filesystem::path grid_path = config_.getGpt3GridPath();
            if (grid_path.empty()) {
                grid_path = config_.getInternalDataRoot() / "static" / "gpt3" / "gpt3_5.grd";
            }
            gpt3_ = std::make_unique<Gpt3>();
            if (!gpt3_->load(grid_path, log_manager_)) {
                logger->warn("GPT3 grid load failed ('{}'); falling back to the GMF mapping and the standard-atmosphere pressure",
                             grid_path.string());
                gpt3_.reset();
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
     * @brief Compute hydrostatic and wet tropospheric mapping functions.
     *
     * With the VMF3 mapping function configured and a loaded GPT3 grid, VMF3 is
     * evaluated from the GPT3 "a" coefficients.  Otherwise (and when the GPT3 query
     * fails) the Niell (1996) NMF tables supply latitude- and season-dependent
     * coefficients (a, b, c) for the Marini continued fraction: hydrostatic terms are
     * a mean plus an annual harmonic, wet terms a mean only.  Both paths add the Niell
     * height correction to the hydrostatic mapping.  An invalid receiver position
     * selects a Black & Eisner fallback.
     *
     * References: Niell (1996), J. Geophys. Res., 101(B2), 3227-3246; Boehm et al.
     * (2006), Geophys. Res. Lett., 33, L07304.
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

        // --- VMF3 path (empirical, from GPT3): returns before the NMF path ---
        // Uses a_h/a_w bilinearly interpolated from the GPT3 grid and b/c
        // synthesized from the degree/order-12 spherical-harmonic expansion in
        // Vmf3Coefficients.hpp.  A failed GPT3 query falls through to the NMF path
        // without a log line, so a single bad sample does not abort the run.
        if (config_.getTroposphereMappingFunction() == TroposphereMappingFunction::Vmf3 && gpt3_) {
            const double lat_deg = lat * 180.0 / Constants::PI;
            const double lon_deg = llh.lon_rad * 180.0 / Constants::PI;
            Gpt3::Result r{};
            if (gpt3_->query(lat_deg, lon_deg, llh.h_m, doy, r)) {
                Vmf3::computeMapping(lat, llh.lon_rad, doy, el_rad, r.a_h, r.a_w, mf_d, mf_w);

                // Niell height correction for the hydrostatic mapping, the same
                // formula as the NMF path below.  It is applied with the full
                // station height h_km, which moves the VMF3 mapping from the height
                // its "a" coefficient refers to (the grid's orthometric reference
                // height) to the station height.
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
            // GPT3 query failed: fall through to the NMF path below.
        }

        // Phase offset: DOY 28 for northern hemisphere, DOY 28 + 182.625 for southern
        double doy_phase = (lat >= 0.0) ? 28.0 : 28.0 + 182.625;
        double cos_annual = std::cos(2.0 * Constants::PI * (doy - doy_phase) / 365.25);

        double abs_lat = std::abs(lat); // radians

        // === Niell Mapping Function coefficients (Niell 1996, NMF) ===
        // 5 data points at latitudes: 15°, 30°, 45°, 60°, 75°.  These are the NMF
        // tables, although the method and its log text say "GMF"; the
        // spherical-harmonic VMF3 path is the branch above.
        //
        // The hydrostatic seasonal amplitude is zero at 15° and grows poleward,
        // so amplitude[0] = 0.0.  POINT (Troposphere.cpp NMF abc_amp), cssrlib
        // (gnss.py tropmapfNiell) and RTKLIB (tropmapf) all carry the leading 0.0;
        // omitting it shifts every amplitude one latitude bin toward the equator.
        //
        // Hydrostatic: mean and annual amplitude
        static constexpr double ah_mean[] = { 1.2769934e-3, 1.2683230e-3, 1.2465397e-3,
                                               1.2196049e-3, 1.2045996e-3 };
        static constexpr double ah_amp[]  = { 0.0, 1.2709626e-5, 2.6523662e-5,
                                               3.4000452e-5, 4.1202191e-5 };

        static constexpr double bh_mean[] = { 2.9153695e-3, 2.9152299e-3, 2.9288445e-3,
                                               2.9022565e-3, 2.9024912e-3 };
        static constexpr double bh_amp[]  = { 0.0, 2.1414979e-5, 3.0160779e-5,
                                               7.2562722e-5, 11.723375e-5 };

        static constexpr double ch_mean[] = { 62.610505e-3, 62.837393e-3, 63.721774e-3,
                                               63.824265e-3, 64.258455e-3 };
        static constexpr double ch_amp[]  = { 0.0, 9.0128400e-5, 4.3497037e-5,
                                               84.795348e-5, 170.37206e-5 };

        // Wet: mean only (no annual variation in NMF wet)
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

        // Niell seasonal combination: avg − amp·cos(2π(doy − 28)/365.25), with a
        // minus sign.  cos_annual already folds the southern-hemisphere half-year
        // shift into its phase (doy_phase = 28 + 182.625 for the SH), so the one
        // minus sign is correct for both hemispheres.  POINT, cssrlib and RTKLIB
        // all use the minus; a plus puts the seasonal variation in anti-phase.
        double a_h = lerp(ah_mean, idx, frac) - lerp(ah_amp, idx, frac) * cos_annual;
        double b_h = lerp(bh_mean, idx, frac) - lerp(bh_amp, idx, frac) * cos_annual;
        double c_h = lerp(ch_mean, idx, frac) - lerp(ch_amp, idx, frac) * cos_annual;

        // Interpolate wet coefficients (no annual variation in NMF wet)
        double a_w = lerp(aw_mean, idx, frac);
        double b_w = lerp(bw_mean, idx, frac);
        double c_w = lerp(cw_mean, idx, frac);

        // Compute base mapping using Marini continued fraction
        mf_d = mariniMapping(sin_el, a_h, b_h, c_h);
        mf_w = mariniMapping(sin_el, a_w, b_w, c_w);

        // Niell (1996) height correction for the hydrostatic mapping function (also
        // used by VMF1, Boehm & Schuh 2004):
        //   mf_h(h) = mf_h(0) + [1/sin(el) - m(el; a_ht, b_ht, c_ht)] * h_km
        // where m is the Marini form with the height-correction coefficients below.
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

        // GPT3 path, taken only when the configured a priori source is Gpt3 and the
        // grid loaded: query the grid at this lat/lon/h/doy.  The VMF3 mapping
        // function also loads the grid, for its "a" coefficients, but it does not
        // choose the a priori source: with standard_atmosphere the configured
        // sea-level pair stays in force.  On a failed query the
        // standard-atmosphere value already in P is kept, with no log line.
        if (gpt3_ && gpt3_->isLoaded() &&
            config_.getTroposphereAprioriSource() == TroposphereAprioriSource::Gpt3) {
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