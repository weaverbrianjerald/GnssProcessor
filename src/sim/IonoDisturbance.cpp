/**
 * @file IonoDisturbance.cpp
 * @brief Implementation of the structured slant-TEC disturbance field.
 */

#include "sim/IonoDisturbance.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

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
                                     double quiet_slant_stec_tecu, double storm_gain,
                                     double tid_period_override_s, int tid_slot,
                                     double tid_vtec_explicit, double turb_vtec) const {
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
    // Temporal phase with an optional linear-period CHIRP across the window: the
    // instantaneous period sweeps P0 (high-rate) -> P1 (low-rate).  The phase is the
    // time-integral of the instantaneous angular frequency (a log for a linear ramp),
    // so the sinusoid stays continuous as its frequency slides.
    const double P0 = (cfg_.tid_period_start_s > 0.0) ? cfg_.tid_period_start_s : cfg_.tid_period_s;
    const double P1 = (cfg_.tid_period_end_s > 0.0) ? cfg_.tid_period_end_s : cfg_.tid_period_s;
    const double D = cfg_.window_duration_s;
    double temporal_phase;
    if (D <= 0.0 || std::abs(P1 - P0) < 1.0e-9 || P0 <= 0.0) {
        const double p = (P0 > 0.0) ? P0 : cfg_.tid_period_s;
        temporal_phase = (2.0 * Constants::PI / p) * (tow - t0);
    } else {
        const double s_in = std::min(std::max(tow - t0, 0.0), D);   // clamp to the window
        const double a = (P1 - P0) / D;                              // dP/dt
        temporal_phase = (2.0 * Constants::PI / a) * std::log((P0 + a * s_in) / P0);
    }
    const double tid_phase = k * d_along - temporal_phase;
    // Positive-definite pulse 0.5*(1-cos) (only ever enhances) or the bipolar sin.
    const double tid_wave = cfg_.tid_positive ? 0.5 * (1.0 - std::cos(tid_phase))
                                              : std::sin(tid_phase);

    double dvtec_tid;
    if (cfg_.tid_pulse_count > 0) {
        // --- PULSE-TRAIN TID: a few sporadic ~10-min bumps, not a continuous wave. --
        // Up to tid_pulse_count raised-cosine bumps of width tid_pulse_duration_s,
        // spaced evenly across the storm window; the storm-wide raised cosine W is NOT
        // applied (the bumps ARE the envelope).  Most TID satellites carry the smooth
        // bump; the one satellite given tid_period_override_s>0 has that bump filled
        // with a rapid temporal carrier (its flicker), so it fluctuates epoch-to-epoch
        // while the others rise and fall once over ~10 min.
        const double Tp = cfg_.tid_pulse_duration_s;
        double env = 0.0;
        if (Tp > 0.0 && D > 0.0) {
            for (int i = 0; i < cfg_.tid_pulse_count; ++i) {
                // Staggering: with tid_n_sats>0, pulse i belongs to exactly one satellite
                // (slot i % tid_n_sats), so the satellites are disturbed at different times.
                if (cfg_.tid_n_sats > 0 && tid_slot >= 0 && (i % cfg_.tid_n_sats) != tid_slot)
                    continue;
                const double center = t0 + D * (static_cast<double>(i) + 0.5) / cfg_.tid_pulse_count;
                const double u = (tow - center) / Tp;           // (-0.5, 0.5) inside the bump
                if (u > -0.5 && u < 0.5)
                    env = std::max(env, 0.5 * (1.0 - std::cos(2.0 * Constants::PI * (u + 0.5))));
            }
        }
        double carrier = 1.0;
        if (tid_period_override_s > 0.0)
            carrier = 0.5 * (1.0 - std::cos(2.0 * Constants::PI * (tow - t0) / tid_period_override_s));
        dvtec_tid = cfg_.tid_amp_tecu * env * carrier;
    } else {
        dvtec_tid = cfg_.tid_amp_tecu * W * tid_wave;   // legacy continuous chirp (byte-identical)
    }

    // --- Quiet from the caller (already slant); storm vertical -> slant. ------
    // Extra low-elevation emphasis: obliquity^sed_elev_exp ON TOP of the standard
    // obliquity, so low-elevation rays see a much larger storm (0 => plain mapping).
    // storm_gain leaves "stable" satellites (gain 0) on the quiet background only.
    const double elev_emphasis = (cfg_.sed_elev_exp != 0.0)
                                     ? std::pow(s.obliquity, cfg_.sed_elev_exp) : 1.0;
    const double slant_gain = s.obliquity * elev_emphasis * storm_gain;
    s.stec_quiet_tecu = quiet_slant_stec_tecu;
    // sed_bulk drops the per-satellite storm_gain from the SED term only: the blob is a
    // spatial feature and spatial_weight already decides who is under it.
    s.dstec_sed_tecu = cfg_.sed_bulk ? dvtec_sed * s.obliquity * elev_emphasis
                                     : dvtec_sed * slant_gain;
    // Explicit per-satellite pulses (tid_vtec_explicit >= 0) are pre-summed by the
    // caller and mapped with the STANDARD obliquity only, so the configured amplitude
    // (vertical TECU) maps predictably to slant.  Otherwise use the internal TID model.
    s.dstec_tid_tecu = (tid_vtec_explicit >= 0.0)
                           ? tid_vtec_explicit * s.obliquity * storm_gain
                           : dvtec_tid * slant_gain;
    // Turbulence: mapped by the standard obliquity like the explicit pulses, and NOT
    // gated by storm_gain -- the caller only supplies it for the satellites it listed,
    // so gating again would silently zero it on any ray the storm-gain hash calls
    // "stable".  Deliberately NOT multiplied by the time window either: a rough field
    // is rough for the whole pass, not only at the window's raised-cosine peak.
    s.dstec_turb_tecu = turb_vtec * s.obliquity;
    // Floor the TOTAL at zero: turbulence is signed and can be large, and a negative
    // total slant TEC is unphysical (it would hand out a phase delay and a code advance).
    s.stec_total_tecu = std::max(0.0, s.stec_quiet_tecu + s.dstec_sed_tecu
                                      + s.dstec_tid_tecu + s.dstec_turb_tecu);
    return s;
}

double IonoDisturbance::turbVtec(const std::string& prn, const GnssTime& t) const {
    if (cfg_.turb_sigma_tecu <= 0.0 || cfg_.turb_sats.empty()) return 0.0;
    if (std::find(cfg_.turb_sats.begin(), cfg_.turb_sats.end(), prn) == cfg_.turb_sats.end())
        return 0.0;

    // splitmix64 on (PRN, epoch-millisecond): a hash, not a stream, so the value is
    // reproducible, independent between epochs, and cannot perturb any shared RNG.
    auto mix = [](std::uint64_t x) {
        x += 0x9E3779B97F4A7C15ULL;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
        return x ^ (x >> 31);
    };
    std::uint64_t h = 0xC0FFEEULL;
    for (unsigned char c : prn) h = mix(h ^ static_cast<std::uint64_t>(c));
    const std::uint64_t ms = static_cast<std::uint64_t>(
        std::llround(t.tow * 1000.0)) ^ (static_cast<std::uint64_t>(t.week) << 40);

    // One standard normal per (sat, epoch), Box-Muller from two hashed uniforms.
    auto draw = [&](std::uint64_t k) {
        const double u1 = std::max(1.0e-12, (mix(h ^ k) >> 11) * (1.0 / 9007199254740992.0));
        const double u2 = (mix(h ^ (k + 1ULL)) >> 11) * (1.0 / 9007199254740992.0);
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * Constants::PI * u2);
    };
    // CORRELATED, not white.  A white series inflects at EVERY epoch, which does not
    // look like a real disturbed arc: the observed G5 state turns over every couple of
    // epochs, i.e. it has structure on a 60-120 s scale.  A short moving average over
    // adjacent epochs' draws produces exactly that (lag-1 correlation 2/3 for MA(3)) and
    // stays STATELESS -- still a pure function of (sat, epoch), so it consumes no RNG
    // stream and remains reproducible under any evaluation order.
    // Normalised by sqrt(3) so the smoothing does not change the marginal sigma.
    const std::uint64_t step = static_cast<std::uint64_t>(
        std::llround(cfg_.turb_step_s > 0.0 ? cfg_.turb_step_s * 1000.0 : 30000.0));
    const double z = (draw(ms) + draw(ms - step) + draw(ms - 2ULL * step))
                     / std::sqrt(3.0);

    // Per-satellite severity in [0.6, 1.4], stable across the pass: the storm does not
    // rough up every affected ray equally, and a spread makes the QC plot honest.
    const double sev = 0.6 + 0.8 * ((mix(h) >> 11) * (1.0 / 9007199254740992.0));

    // Episodic envelope: raised cosine over the turbulence window, so the arc is smooth
    // early and rough later (the post-sunset blow-up the ADIS reference shows).  A
    // non-positive duration means "whole arc", i.e. no envelope.
    double env = 1.0;
    if (cfg_.turb_window_duration_s > 0.0) {
        const double u = (t.tow - cfg_.turb_window_start_tow_s) / cfg_.turb_window_duration_s;
        env = (u < 0.0 || u > 1.0) ? 0.0
                                   : 0.5 * (1.0 - std::cos(2.0 * Constants::PI * u));
    }
    return cfg_.turb_sigma_tecu * sev * env * z;
}

double IonoDisturbance::scintSigmaPhiRad(const IonoSample& s, double el_deg) const {
    const double deg = Constants::PI / 180.0;
    const double el = std::max(el_deg, cfg_.scint_elev_clamp_deg) * deg;
    const double elev_factor = 1.0 / std::sin(el); // low-elevation rays scintillate harder
    return cfg_.scint_sigma_phi_max_rad * s.spatial_weight * s.time_window * elev_factor;
}

} // namespace sim
} // namespace GnssProcessor
