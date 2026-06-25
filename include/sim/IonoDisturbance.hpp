#pragma once

/**
 * @file IonoDisturbance.hpp
 * @brief Structured slant-TEC field for the exam's ionospheric disturbance.
 *
 * Evaluates a total slant ionospheric electron content (STEC) for a ray
 * (receiver -> satellite) at the moving ionospheric pierce point (IPP), as the
 * sum of a quiet background plus two disturbance terms tied to the hurricane:
 *   - a Storm-Enhanced-Density (SED) blob: a drifting 2-D Gaussian in IPP
 *     latitude/longitude, gated by a raised-cosine time window (the dominant
 *     single-frequency bias source), and
 *   - a Travelling Ionospheric Disturbance (TID): a propagating sinusoid giving
 *     a horizontal STEC gradient a single zenith model cannot capture.
 *
 * The class is the simulator's own physics — the processor carries NO
 * first-order iono term in the dual-frequency ionosphere-free combination
 * (`PppModel` returns 0 when >=2 bands are present), so injecting a structured
 * STEC is non-circular: there is no processor model the student can invert to
 * zero the storm.  Per band the first-order delay is `+40.3*1e16*STEC/f^2`
 * metres on CODE (group delay) and the negative of that on PHASE (advance);
 * because every band shares the same STEC scaled by 1/f^2, the dual-frequency
 * ionosphere-free combination cancels it exactly (DESIGN.md §6).
 *
 * Scintillation co-located with the SED blob is realised purely as inflated
 * phase measurement noise (NO cycle slips); this class only supplies the 1-sigma
 * envelope, the caller draws and applies it.
 */

#include "math/TimeUtils.hpp" // GnssTime

namespace GnssProcessor {
namespace sim {

/** @brief Parameters of the ionospheric field (vertical-TEC amplitudes). */
struct IonoConfig {
    // Raised-cosine storm window (GPS-scale TOW seconds).
    double window_start_tow_s{ 0.0 };
    double window_duration_s{ 1800.0 };

    // Storm-Enhanced-Density blob (vertical TECU), centre drifting with the eye.
    double sed_amp_tecu{ 15.0 };
    double sed_sigma_lat_deg{ 6.0 };
    double sed_sigma_lon_deg{ 8.0 };
    double center_lat0_deg{ -18.5 };
    double center_lon0_deg{ -178.0 };
    double drift_lat_deg_per_min{ -0.15 };
    double drift_lon_deg_per_min{ 0.30 };

    // Travelling Ionospheric Disturbance (vertical TECU).
    double tid_amp_tecu{ 4.0 };
    double tid_wavelength_km{ 300.0 };
    double tid_period_s{ 1200.0 };
    double tid_azimuth_deg{ 135.0 }; ///< Propagation heading from north (SE-ward).

    // Quiet background (vertical TECU).  Analytic placeholder until the real
    // Klobuchar field is wired in for the single-frequency student path.
    double quiet_vtec_tecu{ 25.0 };

    // Scintillation phase-noise envelope.
    double scint_sigma_phi_max_rad{ 0.6 };
    double scint_elev_clamp_deg{ 10.0 };

    // Thin-shell geometry.
    double shell_height_km{ 350.0 };
    double earth_radius_km{ 6371.0 };
};

/** @brief Evaluated ionospheric quantities for one ray at one epoch. */
struct IonoSample {
    double ipp_lat_deg{ 0.0 }, ipp_lon_deg{ 0.0 }; ///< Pierce point (geodetic deg).
    double obliquity{ 1.0 };          ///< Vertical->slant mapping factor (1/cos z').
    double stec_quiet_tecu{ 0.0 };    ///< Slant quiet background (TECU).
    double dstec_sed_tecu{ 0.0 };     ///< Slant SED disturbance (TECU).
    double dstec_tid_tecu{ 0.0 };     ///< Slant TID disturbance (TECU).
    double stec_total_tecu{ 0.0 };    ///< Slant total (TECU).
    double time_window{ 0.0 };        ///< Raised-cosine weight in [0,1].
    double spatial_weight{ 0.0 };     ///< SED Gaussian in [0,1] (scintillation co-location).
    bool in_storm_window{ false };
};

/** @brief Builds the slant-TEC field; stateless apart from its config. */
class IonoDisturbance {
public:
    explicit IonoDisturbance(const IonoConfig& cfg) : cfg_(cfg) {}

    /**
     * @brief Evaluate the slant TEC field for a ray at epoch @p t.
     * @param rx_lat_deg,rx_lon_deg Receiver geodetic latitude/longitude (deg).
     * @param az_deg,el_deg         Satellite azimuth (from north) and elevation (deg).
     * @param quiet_slant_stec_tecu Slant quiet-background STEC (TECU) supplied by the
     *        caller — typically backed out of the reused broadcast Klobuchar L1 delay so
     *        a single-frequency student removes it exactly.  The SED/TID storm is added
     *        on top.  Pass the analytic @c config().quiet_vtec_tecu * obliquity as a
     *        fallback when no Klobuchar field is available.
     */
    IonoSample evaluate(double rx_lat_deg, double rx_lon_deg,
                        double az_deg, double el_deg, const GnssTime& t,
                        double quiet_slant_stec_tecu) const;

    /**
     * @brief First-order slant ionospheric delay (metres) at carrier @p f_hz.
     *
     * Positive return = CODE group delay (+I); carrier PHASE advance is the
     * negative of this value (-I).  Convert phase to cycles by dividing the
     * metric phase term by the wavelength.
     */
    static double slantDelayMetres(double stec_tecu, double f_hz);

    /** @brief Scintillation phase 1-sigma (radians) for a ray in the storm core. */
    double scintSigmaPhiRad(const IonoSample& s, double el_deg) const;

    const IonoConfig& config() const { return cfg_; }

private:
    IonoConfig cfg_;
};

} // namespace sim
} // namespace GnssProcessor
