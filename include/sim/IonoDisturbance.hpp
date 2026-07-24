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
    // Optional TID temporal-frequency CHIRP across the storm window: the instantaneous
    // period sweeps linearly from _start (high-rate) to _end (low-rate).  <=0 => use
    // tid_period_s (no chirp), so the field is unchanged for scenarios that omit them.
    double tid_period_start_s{ 0.0 };
    double tid_period_end_s{ 0.0 };

    // Optional PULSE-TRAIN envelope for the TID.  When tid_pulse_count>0 the TID is a
    // train of that many raised-cosine bumps, each tid_pulse_duration_s wide, spaced
    // evenly across the storm window — a few sporadic ~10-min disturbances rather than
    // one window-long oscillation.  0 (default) keeps the legacy continuous chirp, so
    // every existing scenario is byte-identical.  A per-ray temporal carrier period can
    // be passed to evaluate() (tid_period_override_s) so a single satellite can be made
    // to fluctuate rapidly inside the bumps while the rest carry the smooth pulse only.
    int    tid_pulse_count{ 0 };
    double tid_pulse_duration_s{ 600.0 };

    // When >0, the pulses are DEALT OUT round-robin to this many satellites (by the
    // per-ray tid_slot passed to evaluate): pulse i belongs only to the satellite whose
    // slot == i % tid_n_sats.  So the bumps are STAGGERED in time across satellites —
    // usually only one satellite is disturbed at a time — instead of every TID
    // satellite pulsing together.  0 (default) = every TID satellite gets every pulse.
    int    tid_n_sats{ 0 };

    // Extra LOW-ELEVATION emphasis on the storm disturbance (SED + TID): the slant
    // term is multiplied by obliquity^sed_elev_exp ON TOP of the standard
    // vertical->slant obliquity, so low-elevation rays are disturbed far more than
    // high-elevation ones (0 = no extra emphasis, i.e. plain obliquity mapping).
    double sed_elev_exp{ 0.0 };

    // When true the TID contributes a POSITIVE-DEFINITE oscillation 0.5*(1-cos)
    // (an amplification that pulses between 0 and the amplitude) instead of the
    // bipolar sin -- so the ionosphere is only ever ENHANCED, never depleted, and
    // the total slant delay can never go negative.  Default false = bipolar sin.
    bool tid_positive{ false };

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
     * @param storm_gain Per-ray multiplier on the SED+TID storm terms (the quiet
     *        background is untouched): 1 = fully disturbed, 0 = "stable" (quiet
     *        only).  Used to leave a fraction of satellites undisturbed in a storm.
     * @param tid_period_override_s When >0 AND a pulse train is active
     *        (tid_pulse_count>0), the TID inside each pulse is filled with a rapid
     *        temporal carrier of this period for THIS ray only — used to make one
     *        designated satellite flicker (e.g. ~60 s, the Nyquist limit of 30 s data)
     *        while every other TID satellite carries only the smooth ~10-min bump.
     *        0 (default) = smooth bump / legacy behaviour.
     * @param tid_slot This satellite's index among the TID satellites (0-based).  With
     *        cfg.tid_n_sats>0 the pulse train is staggered: this ray only shows pulse i
     *        when i % cfg.tid_n_sats == tid_slot, so satellites pulse at different times.
     *        -1 (default) = show every pulse (no staggering).
     * @param tid_vtec_explicit When >=0 this VERTICAL TID amplitude (TECU) is used
     *        DIRECTLY for this ray, bypassing the internal pulse/chirp model — the caller
     *        (main.cpp) has already summed this satellite's explicit tid_pulses at this
     *        epoch.  It is mapped to slant by the STANDARD obliquity only (no sed_elev_exp
     *        emphasis).  <0 (default) = compute the TID from the config as before.
     */
    IonoSample evaluate(double rx_lat_deg, double rx_lon_deg,
                        double az_deg, double el_deg, const GnssTime& t,
                        double quiet_slant_stec_tecu, double storm_gain = 1.0,
                        double tid_period_override_s = 0.0, int tid_slot = -1,
                        double tid_vtec_explicit = -1.0) const;

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
