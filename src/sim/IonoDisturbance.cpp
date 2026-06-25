/**
 * @file IonoDisturbance.cpp
 * @brief Implementation of the structured slant-TEC disturbance field.
 */

#include "sim/IonoDisturbance.hpp"

#include <algorithm>
#include <cmath>

#include "common/Constants.hpp"

namespace GnssProcessor {
namespace sim {

namespace {
constexpr double kIonoConstant = 40.3;     // m * Hz^2 per (el/m^2); file-local in gnss_core too
constexpr double kTecuToElectrons = 1.0e16; // electrons/m^2 per TECU
constexpr double kKmPerDegLat = 111.195;    // mean great-circle km per degree

// Shortest signed longitude difference in degrees, in (-180, 180].  The scenario
// sits at lon ~-178 (right on the +/-180 dateline), so every IPP-vs-centre
// longitude delta is wrapped explicitly rather than relying on the (fragile)
// invariant that no longitude is ever normalized.
double wrapDeg180(double d) {
    d = std::fmod(d + 180.0, 360.0);
    if (d < 0.0) d += 360.0;
    return d - 180.0;
}
} // namespace

double IonoDisturbance::slantDelayMetres(double stec_tecu, double f_hz) {
    if (f_hz <= 0.0) return 0.0;
    return kIonoConstant * (stec_tecu * kTecuToElectrons) / (f_hz * f_hz);
}

IonoSample IonoDisturbance::evaluate(double rx_lat_deg, double rx_lon_deg,
                                     double az_deg, double el_deg, const GnssTime& t,
                                     double quiet_slant_stec_tecu) const {
    const double deg = Constants::PI / 180.0;
    IonoSample s;

    // --- Thin-shell ionospheric pierce point (geometric earth-central angle). --
    const double el = std::max(el_deg, 0.1) * deg;
    const double az = az_deg * deg;
    const double Re = cfg_.earth_radius_km;
    const double H = cfg_.shell_height_km;
    const double sin_zp = Re / (Re + H) * std::cos(el);      // sin of zenith at the shell
    const double psi = Constants::PI / 2.0 - el - std::asin(sin_zp); // earth-central angle
    const double lat_r = rx_lat_deg * deg;
    const double lat_ipp = std::asin(std::sin(lat_r) * std::cos(psi) +
                                     std::cos(lat_r) * std::sin(psi) * std::cos(az));
    const double lon_ipp = rx_lon_deg * deg +
                           std::asin(std::sin(psi) * std::sin(az) / std::cos(lat_ipp));
    s.ipp_lat_deg = lat_ipp / deg;
    s.ipp_lon_deg = wrapDeg180(lon_ipp / deg); // normalize for clean output near the dateline
    s.obliquity = 1.0 / std::sqrt(std::max(1.0e-9, 1.0 - sin_zp * sin_zp)); // 1/cos z'

    // --- Raised-cosine storm time window. ------------------------------------
    const double tow = t.tow;
    const double t0 = cfg_.window_start_tow_s;
    const double t1 = t0 + cfg_.window_duration_s;
    double W = 0.0;
    if (cfg_.window_duration_s > 0.0 && tow >= t0 && tow <= t1) {
        W = 0.5 * (1.0 - std::cos(2.0 * Constants::PI * (tow - t0) / cfg_.window_duration_s));
        s.in_storm_window = true;
    }
    s.time_window = W;

    // --- SED blob: drifting 2-D Gaussian in IPP lat/lon (vertical TECU). ------
    const double tmin = (tow - t0) / 60.0; // minutes since window start (may be < 0)
    const double clat = cfg_.center_lat0_deg + cfg_.drift_lat_deg_per_min * tmin;
    const double clon = cfg_.center_lon0_deg + cfg_.drift_lon_deg_per_min * tmin;
    const double dlat = s.ipp_lat_deg - clat;
    const double dlon = wrapDeg180(s.ipp_lon_deg - clon) * std::cos(clat * deg);
    const double gx = dlat / cfg_.sed_sigma_lat_deg;
    const double gy = dlon / cfg_.sed_sigma_lon_deg;
    s.spatial_weight = std::exp(-0.5 * (gx * gx + gy * gy));
    const double dvtec_sed = cfg_.sed_amp_tecu * s.spatial_weight * W;

    // --- TID: travelling sinusoid, coherent about the storm centre. ----------
    const double dlat_km = (s.ipp_lat_deg - clat) * kKmPerDegLat;
    const double dlon_km = wrapDeg180(s.ipp_lon_deg - clon) * kKmPerDegLat * std::cos(clat * deg);
    const double az_tid = cfg_.tid_azimuth_deg * deg;
    const double d_along = dlat_km * std::cos(az_tid) + dlon_km * std::sin(az_tid);
    const double k = 2.0 * Constants::PI / cfg_.tid_wavelength_km;
    const double omega = 2.0 * Constants::PI / cfg_.tid_period_s;
    const double tid_phase = k * d_along - omega * (tow - t0);
    const double dvtec_tid = cfg_.tid_amp_tecu * W * std::sin(tid_phase);

    // --- Quiet from the caller (already slant); storm vertical -> slant. ------
    s.stec_quiet_tecu = quiet_slant_stec_tecu;
    s.dstec_sed_tecu = dvtec_sed * s.obliquity;
    s.dstec_tid_tecu = dvtec_tid * s.obliquity;
    s.stec_total_tecu = s.stec_quiet_tecu + s.dstec_sed_tecu + s.dstec_tid_tecu;
    return s;
}

double IonoDisturbance::scintSigmaPhiRad(const IonoSample& s, double el_deg) const {
    const double deg = Constants::PI / 180.0;
    const double el = std::max(el_deg, cfg_.scint_elev_clamp_deg) * deg;
    const double elev_factor = 1.0 / std::sin(el); // low-elevation rays scintillate harder
    return cfg_.scint_sigma_phi_max_rad * s.spatial_weight * s.time_window * elev_factor;
}

} // namespace sim
} // namespace GnssProcessor
