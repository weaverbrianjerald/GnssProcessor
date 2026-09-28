/**
 * @file SphericalHarmonicIonosphere.cpp
 * @brief Implementation of the single-layer spherical-harmonic VTEC model, the
 *        shared IPP geometry, and the sub-solar longitude (Stage 0).
 */

#include "atmosphere/SphericalHarmonicIonosphere.hpp"

#include "common/Constants.hpp"
#include "config/GnssConfigTypes.hpp"
#include "math/Coordinates.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace GnssProcessor {

IppGeometry computeIppGeometry(const Eigen::Vector3d& rx_pos_ecef,
                               const Eigen::Vector3d& sat_pos_ecef,
                               double earth_radius_m, double shell_height_m) {
    IppGeometry g;

    const Coordinates::Llh rx = Coordinates::ecefToLlh(rx_pos_ecef);
    const Eigen::Vector3d enu =
        Coordinates::ecefToEnu(sat_pos_ecef - rx_pos_ecef, rx);
    const double el = std::atan2(enu.z(), std::hypot(enu.x(), enu.y()));  // rad
    const double az = std::atan2(enu.x(), enu.y());                       // rad, North→East
    if (el <= 0.0) return g;                                              // below horizon

    const double Re = earth_radius_m;
    const double H  = shell_height_m;
    const double sin_zp = (Re / (Re + H)) * std::cos(el);  // sin of the zenith angle at the IPP
    const double cos_zp = std::sqrt(std::max(0.0, 1.0 - sin_zp * sin_zp));
    if (cos_zp <= 1.0e-6) return g;
    const double mapping = 1.0 / cos_zp;                   // thin-shell VTEC → STEC

    auto safeAsin = [](double x) { return std::asin(std::clamp(x, -1.0, 1.0)); };
    const double psi = (Constants::PI / 2.0) - el - safeAsin(sin_zp);  // earth-centred angle
    const double lat_ipp = safeAsin(std::sin(rx.lat_rad) * std::cos(psi) +
                                    std::cos(rx.lat_rad) * std::sin(psi) * std::cos(az));
    const double lon_ipp = rx.lon_rad +
        safeAsin(std::sin(psi) * std::sin(az) / std::max(1.0e-9, std::cos(lat_ipp)));
    if (!std::isfinite(lat_ipp) || !std::isfinite(lon_ipp)) return g;

    g.elevation_rad = el;
    g.azimuth_rad   = az;
    g.mapping       = mapping;
    g.ipp_lat_rad   = lat_ipp;
    g.ipp_lon_rad   = lon_ipp;
    g.valid         = true;
    return g;
}

double meanSunLongitude(const GnssTime& time) {
    // Low-order NOAA solar series + Greenwich Mean Sidereal Time → the sub-solar
    // longitude λ_ss = α_⊙ − GMST (east-positive).  Includes the equation of time
    // (via the Sun's right ascension), so the solar term is accurate to ~0.01°.
    // The GPS−UT1 offset (~18 s) is neglected; it costs < 0.1° of GMST, well
    // inside the sun-fixed frame's tolerance (the field coefficients are
    // estimated, so a small consistent offset is absorbed into them).
    const double jd = TimeUtils::gpsTimeToJulianDate(time);
    const double n  = jd - 2451545.0;            // days since J2000.0
    const double deg2rad = Constants::PI / 180.0;
    auto wrap360 = [](double d) {
        d = std::fmod(d, 360.0);
        if (d < 0.0) d += 360.0;
        return d;
    };
    const double L = wrap360(280.460 + 0.9856474 * n);                  // mean solar longitude (deg)
    const double g = wrap360(357.528 + 0.9856003 * n) * deg2rad;        // mean anomaly (rad)
    const double lambda =
        (L + 1.915 * std::sin(g) + 0.020 * std::sin(2.0 * g)) * deg2rad; // ecliptic longitude (rad)
    const double eps = (23.439 - 4.0e-7 * n) * deg2rad;                  // obliquity of the ecliptic
    const double ra =
        std::atan2(std::cos(eps) * std::sin(lambda), std::cos(lambda));  // right ascension (rad)
    const double gmst = wrap360(280.46061837 + 360.98564736629 * n) * deg2rad;  // GMST (rad)

    double lon = ra - gmst;                       // sub-solar geographic longitude (east+)
    lon = std::fmod(lon, 2.0 * Constants::PI);
    if (lon > Constants::PI)  lon -= 2.0 * Constants::PI;
    if (lon < -Constants::PI) lon += 2.0 * Constants::PI;
    return lon;
}

SphericalHarmonicIonosphere::SphericalHarmonicIonosphere(int max_degree)
    : max_degree_(max_degree < 0 ? 0 : max_degree) {
    // Coefficient enumeration: per-degree blocks, cosine terms then sine terms.
    //   n: [ C(n,0) C(n,1) .. C(n,n)  S(n,1) .. S(n,n) ]   → 2n+1 terms.
    // Total Σ_{n=0}^{N} (2n+1) = (N+1)².  basis() emits values in this same order.
    coeffs_.reserve(static_cast<std::size_t>((max_degree_ + 1) * (max_degree_ + 1)));
    for (int n = 0; n <= max_degree_; ++n) {
        for (int m = 0; m <= n; ++m) coeffs_.push_back(Coeff{n, m, 'C'});
        for (int m = 1; m <= n; ++m) coeffs_.push_back(Coeff{n, m, 'S'});
    }

    // Real-harmonic normalization N_nm = sqrt[(2−δ_0m)(2n+1)(n−m)!/(n+m)!], so
    // {P̄_nm cos(mλ), P̄_nm sin(mλ)} are orthonormal under (1/4π)∫dΩ.  The factorial
    // ratio (n−m)!/(n+m)! = Π_{k=n−m+1}^{n+m} (1/k) is formed as a product to avoid
    // overflow of the individual factorials at high degree.
    norm_.assign(static_cast<std::size_t>(max_degree_ + 1), {});
    for (int n = 0; n <= max_degree_; ++n) {
        norm_[static_cast<std::size_t>(n)].assign(
            static_cast<std::size_t>(n + 1), 0.0);
        for (int m = 0; m <= n; ++m) {
            double ratio = 1.0;
            for (int k = n - m + 1; k <= n + m; ++k) ratio /= static_cast<double>(k);
            const double two_minus_delta = (m == 0) ? 1.0 : 2.0;
            norm_[static_cast<std::size_t>(n)][static_cast<std::size_t>(m)] =
                std::sqrt(two_minus_delta * (2.0 * n + 1.0) * ratio);
        }
    }
}

void SphericalHarmonicIonosphere::legendre_(
    double x, std::vector<std::vector<double>>& p) const {
    const int N = max_degree_;
    p.assign(static_cast<std::size_t>(N + 1), {});
    for (int n = 0; n <= N; ++n)
        p[static_cast<std::size_t>(n)].assign(static_cast<std::size_t>(n + 1), 0.0);

    // Unnormalized associated Legendre P_nm(x) (no Condon–Shortley phase), then
    // scaled by the precomputed normalization.  s = sqrt(1−x²) = cos(lat).
    const double s = std::sqrt(std::max(0.0, 1.0 - x * x));
    auto P = [&](int n, int m) -> double& {
        return p[static_cast<std::size_t>(n)][static_cast<std::size_t>(m)];
    };

    P(0, 0) = 1.0;
    // Sectorial: P_mm = (2m−1)!! s^m  via  P_mm = (2m−1) s P_{m−1,m−1}.
    for (int m = 1; m <= N; ++m)
        P(m, m) = (2.0 * m - 1.0) * s * P(m - 1, m - 1);
    // Off-sectorial + tesseral recurrence over increasing degree at fixed order.
    for (int m = 0; m <= N; ++m) {
        if (m + 1 <= N) P(m + 1, m) = (2.0 * m + 1.0) * x * P(m, m);
        for (int n = m + 2; n <= N; ++n)
            P(n, m) = ((2.0 * n - 1.0) * x * P(n - 1, m)
                       - (n + m - 1.0) * P(n - 2, m)) / (n - m);
    }

    // Apply the real-harmonic normalization in place.
    for (int n = 0; n <= N; ++n)
        for (int m = 0; m <= n; ++m)
            P(n, m) *= norm_[static_cast<std::size_t>(n)][static_cast<std::size_t>(m)];
}

void SphericalHarmonicIonosphere::basis(double lat_rad, double lon_sf_rad,
                                        std::vector<double>& out) const {
    out.assign(coeffs_.size(), 0.0);
    std::vector<std::vector<double>> pbar;
    legendre_(std::sin(lat_rad), pbar);
    for (std::size_t k = 0; k < coeffs_.size(); ++k) {
        const Coeff& c = coeffs_[k];
        const double pb = pbar[static_cast<std::size_t>(c.n)][static_cast<std::size_t>(c.m)];
        out[k] = (c.trig == 'C') ? pb * std::cos(c.m * lon_sf_rad)
                                 : pb * std::sin(c.m * lon_sf_rad);
    }
}

double SphericalHarmonicIonosphere::vtec(const Eigen::VectorXd& coeff_tecu,
                                         double lat_rad, double lon_sf_rad) const {
    std::vector<double> b;
    basis(lat_rad, lon_sf_rad, b);
    double v = 0.0;
    const Eigen::Index n =
        std::min<Eigen::Index>(coeff_tecu.size(), static_cast<Eigen::Index>(b.size()));
    for (Eigen::Index k = 0; k < n; ++k) v += coeff_tecu(k) * b[static_cast<std::size_t>(k)];
    return v;
}

std::vector<ShLayer> shLayers(const GlobalVtecShConfig& shc,
                              const SphericalHarmonicIonosphere* layer0,
                              const SphericalHarmonicIonosphere* layer1) {
    std::vector<ShLayer> out;
    if (!layer0) return out;
    ShLayer l0;
    l0.model = layer0;
    l0.layer = 0;
    l0.shell_height_m = shc.layer_height_km * 1000.0;
    l0.kaula_scale_tecu = shc.kaula_scale_tecu;
    l0.q_per_degree = shc.q_per_degree;
    l0.mean_prior_tecu = shc.mean_vtec_prior_tecu;
    l0.mean_prior_sigma_tecu = shc.mean_vtec_prior_sigma_tecu;
    out.push_back(l0);
    if (layer1) {
        ShLayer l1;
        l1.model = layer1;
        l1.layer = 1;
        l1.shell_height_m = shc.layer2_height_km * 1000.0;
        l1.kaula_scale_tecu = shc.layer2_kaula_scale_tecu;
        l1.q_per_degree = shc.layer2_q_per_degree;
        l1.mean_prior_tecu = shc.layer2_mean_vtec_prior_tecu;
        l1.mean_prior_sigma_tecu = shc.layer2_mean_vtec_prior_sigma_tecu;
        out.push_back(l1);
    }
    return out;
}

} // namespace GnssProcessor
