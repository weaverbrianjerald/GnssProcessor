#include "GnssConfig.hpp"
#include "Troposphere.hpp"
#include "Constants.hpp"
#include <Eigen/Dense>
#include <algorithm>
#include <cmath>

namespace GnssProcessor {

    namespace {
        struct LlhRadM {
            double lat_rad = 0.0;
            double lon_rad = 0.0;
            double h_m = 0.0;
        };

        // Minimal WGS84 ECEF -> geodetic LLH (radians, meters), Bowring-style iteration.
        // Assumes ECEF in meters.
        LlhRadM ecefToLlhWgs84(const Eigen::Vector3d& ecef) {
            constexpr double a = 6378137.0;                  // semi-major axis [m]
            constexpr double f = 1.0 / 298.257223563;        // flattening
            constexpr double e2 = f * (2.0 - f);             // first eccentricity squared

            const double x = ecef.x();
            const double y = ecef.y();
            const double z = ecef.z();

            LlhRadM out{};
            const double p = std::sqrt(x * x + y * y);
            out.lon_rad = std::atan2(y, x);

            if (p < 1e-6) {
                // Near pole: longitude undefined, set to 0
                out.lat_rad = (z >= 0.0) ? Constants::PI / 2.0 : -Constants::PI / 2.0;
                const double sin_lat = std::sin(out.lat_rad);
                const double N = a / std::sqrt(1.0 - e2 * sin_lat * sin_lat);
                out.h_m = std::abs(z) - N * (1.0 - e2);
                return out;
            }

            // Initial guess
            double lat = std::atan2(z, p * (1.0 - e2));
            double h = 0.0;

            // Iterate a few times (converges very quickly)
            for (int i = 0; i < 6; ++i) {
                const double sin_lat = std::sin(lat);
                const double N = a / std::sqrt(1.0 - e2 * sin_lat * sin_lat);
                h = p / std::cos(lat) - N;

                const double denom = p * (1.0 - e2 * (N / (N + h)));
                if (std::abs(denom) < 1e-12) break;

                const double lat_next = std::atan2(z, denom);
                if (std::abs(lat_next - lat) < 1e-12) {
                    lat = lat_next;
                    break;
                }
                lat = lat_next;
            }

            out.lat_rad = lat;
            out.h_m = h;
            return out;
        }

        // Standard atmosphere pressure model (troposphere lapse-rate approximation).
        // Interprets P0/T0 as sea-level defaults.
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

    Troposphere::Troposphere(const GnssConfig& config, LogManager& log_mgr)
        : config_(config), log_manager_(log_mgr) {
        log_manager_.getLogger("troposphere")->info("Troposphere model initialized: mode={}, elev_mask={}°",
            config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK", config_.getElevationMaskDeg());
    }

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

    double Troposphere::computeSaastamoinenZHD(const Eigen::Vector3d& receiver_pos) const {
        // WGS84 LLH (geodetic)
        const LlhRadM llh = ecefToLlhWgs84(receiver_pos);
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