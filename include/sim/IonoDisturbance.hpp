#pragma once

/**
 * @file IonoDisturbance.hpp
 * @brief Structured slant-TEC field for the exam scenario's ionospheric disturbance.
 *
 * Evaluates a total slant ionospheric electron content (STEC) for a ray
 * (receiver -> satellite) at the moving ionospheric pierce point (IPP), as the
 * sum of a quiet background plus two disturbance terms tied to the hurricane:
 *   - a Storm-Enhanced-Density (SED) blob: a drifting 2-D Gaussian in IPP
 *     latitude/longitude, gated by a raised-cosine time window (the dominant
 *     single-frequency bias source), and
 *   - a Travelling Ionospheric Disturbance (TID): a propagating sinusoid giving
 *     a horizontal STEC gradient a single zenith model cannot capture.
 * A third, optional term is epoch-to-epoch turbulence (see turbVtec()).
 *
 * The field is the simulator's own physics.  The processor carries no
 * first-order iono term in the dual-frequency ionosphere-free combination
 * (`PppModel` returns 0 when >=2 bands are present), so injecting a structured
 * STEC is non-circular: there is no processor model a student can invert to
 * remove the storm.  Per band the first-order delay is `+40.3*1e16*STEC/f^2`
 * metres on code (group delay) and the negative of that on phase (advance).
 * Because every band shares the same STEC scaled by 1/f^2, the dual-frequency
 * ionosphere-free combination cancels it exactly (DESIGN.md §6).
 *
 * Scintillation co-located with the SED blob is realised only as inflated phase
 * measurement noise, without cycle slips.  This class supplies the 1-sigma
 * envelope; the caller draws and applies the noise.
 */

#include <string>            // turb_sats
#include <vector>            // turb_sats

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
    // Optional linear chirp of the TID temporal period across the storm window: the
    // instantaneous period sweeps linearly from _start (high rate) to _end (low
    // rate).  Each value <=0 => tid_period_s, so with
    // both unset there is no chirp and the field is unchanged for scenarios that omit
    // them.
    double tid_period_start_s{ 0.0 };
    double tid_period_end_s{ 0.0 };

    // Optional pulse-train envelope for the TID.  When tid_pulse_count>0 the TID is a
    // train of up to that many raised-cosine bumps, each tid_pulse_duration_s wide,
    // centred at even spacing across the storm window: a few sporadic ~10-min
    // disturbances rather than one window-long oscillation.  The storm window's own
    // raised cosine is not applied in this mode (the bumps are the envelope).
    // 0 (default) keeps the continuous (optionally chirped) wave, so existing scenarios
    // are byte-identical.  A per-ray temporal carrier period can be passed to
    // evaluate() (tid_period_override_s) so a single satellite fluctuates rapidly
    // inside the bumps while the others carry only the smooth pulse.
    int    tid_pulse_count{ 0 };
    double tid_pulse_duration_s{ 600.0 };

    // When >0, the pulses are dealt out round-robin to this many satellites (by the
    // per-ray tid_slot passed to evaluate()): pulse i belongs only to the satellite
    // whose slot == i % tid_n_sats.  The bumps are therefore staggered in time across
    // satellites, usually with one satellite disturbed at a time, instead of every TID
    // satellite pulsing together.  0 (default) = every TID satellite gets every pulse.
    int    tid_n_sats{ 0 };

    // Extra low-elevation emphasis on the storm disturbance (SED + internal TID model):
    // the slant term is multiplied by obliquity^sed_elev_exp on top of the standard
    // vertical->slant obliquity, so low-elevation rays are disturbed far more than
    // high-elevation ones (0 = no extra emphasis, i.e. plain obliquity mapping).
    double sed_elev_exp{ 0.0 };

    // --- Epoch-to-epoch ionospheric turbulence (see IonoDisturbance::turbVtec) ---
    // A storm-roughened field: irregular STEC changes between consecutive epochs on a
    // handful of rays, with no trend and no period.  This is the only term in the model
    // that is not smooth in time, and therefore the only one a geometry-free QC plot
    // shows as activity rather than as a slowly varying level.  It is still one slant
    // STEC scaled by 1/f^2 per band, so the dual-frequency ionosphere-free combination
    // cancels it exactly and Melbourne-Wubbena (which is iono-free) is unaffected: slips
    // stay separable from ionosphere.  Real roughness is episodic: the ADIS reference
    // (equatorial station, Gannon storm) is flat all day and roughens after local
    // sunset.  The turbulence therefore has its own raised-cosine window, independent of
    // the SED/TID storm window, which the caller fills from arc fractions.  The
    // turbulence fields follow sed_bulk.
    //
    // sed_bulk: the SED blob is a bulk spatial feature, so which rays see it is decided
    // by their pierce point (spatial_weight), not by the per-satellite storm_gain.  With
    // explicit tid_pulses configured, that gain is 1 only for the pulse satellites, so
    // with sed_bulk false the SED enhancement is confined to those rays.  true = every
    // ray sees the blob according to its own IPP (storm_gain is dropped from the SED
    // term only).  Default false keeps the existing gating and every existing scenario
    // byte-identical; the TID term stays gated either way.
    bool sed_bulk{ false };

    std::vector<std::string> turb_sats{};   ///< PRNs carrying turbulence ({} = none).
    double turb_sigma_tecu{ 0.0 };          ///< Vertical 1-sigma (TECU) at the window peak; 0 = off.
    double turb_step_s{ 0.0 };              ///< Epoch spacing (s) for the MA smoothing; <=0 => 30.
    double turb_window_start_tow_s{ 0.0 };  ///< Turbulence window start (GPS tow, s).
    double turb_window_duration_s{ 0.0 };   ///< Turbulence window length (s); <=0 => whole arc.

    // When true the internal TID model contributes a positive-definite oscillation
    // 0.5*(1-cos), which pulses between 0 and the amplitude, instead of the bipolar
    // sin, so the TID only enhances the ionosphere and never depletes it.  Default
    // false = bipolar sin.  (evaluate() also floors the total slant TEC at zero.)
    bool tid_positive{ false };

    // Quiet background (vertical TECU) for the analytic background source, and the
    // fallback when no broadcast Klobuchar field is available.  The caller maps it to
    // slant and passes it to evaluate() as quiet_slant_stec_tecu; the Klobuchar and GIM
    // backgrounds are computed by the caller.
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
    /// Pierce point on the storm shell (deg), spherically propagated from the
    /// receiver's geodetic latitude (the same construction as GnssProcessor's
    /// computeIppGeometry), so it is not itself a geodetic latitude.
    double ipp_lat_deg{ 0.0 }, ipp_lon_deg{ 0.0 };
    double obliquity{ 1.0 };          ///< Vertical->slant mapping factor (1/cos z').
    double stec_quiet_tecu{ 0.0 };    ///< Slant quiet background (TECU).
    double dstec_sed_tecu{ 0.0 };     ///< Slant SED disturbance (TECU).
    double dstec_tid_tecu{ 0.0 };     ///< Slant TID disturbance (TECU).
    double dstec_turb_tecu{ 0.0 };    ///< Slant epoch-to-epoch turbulence (TECU); see turbVtec().
    double stec_total_tecu{ 0.0 };    ///< Slant total (TECU), floored at 0.
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
     *        caller, typically backed out of the reused broadcast Klobuchar L1 delay so
     *        that a single-frequency student removes it exactly.  The SED/TID storm is
     *        added on top.  Without a Klobuchar field the caller passes the analytic
     *        @c config().quiet_vtec_tecu mapped to slant.
     * @param storm_gain Per-ray multiplier on the SED+TID storm terms (the quiet
     *        background is untouched): 1 = fully disturbed, 0 = "stable" (quiet
     *        only).  Used to leave a fraction of satellites undisturbed in a storm.
     *        With cfg.sed_bulk the SED term ignores it; turbulence never uses it.
     * @param tid_period_override_s When >0 and a pulse train is active
     *        (tid_pulse_count>0), the TID inside each pulse is filled with a rapid
     *        temporal carrier of this period for this ray only.  Used to make one
     *        designated satellite flicker (e.g. ~60 s, the Nyquist limit of 30 s data)
     *        while every other TID satellite carries only the smooth ~10-min bump.
     *        0 (default) = smooth bump, or the continuous wave without a pulse train.
     * @param tid_slot This satellite's index among the TID satellites (0-based).  With
     *        cfg.tid_n_sats>0 the pulse train is staggered: this ray only shows pulse i
     *        when i % cfg.tid_n_sats == tid_slot, so satellites pulse at different times.
     *        -1 (default) = show every pulse (no staggering).
     * @param tid_vtec_explicit When >=0 this vertical TID amplitude (TECU) is used
     *        directly for this ray, bypassing the internal pulse/chirp model; the caller
     *        (main.cpp) has already summed this satellite's explicit tid_pulses at this
     *        epoch.  It is mapped to slant by the standard obliquity and storm_gain only
     *        (no sed_elev_exp emphasis).  <0 (default) = compute the TID from the config.
     * @param turb_vtec This ray's vertical turbulence amplitude (TECU) for this epoch,
     *        from turbVtec(): a caller-supplied signed value, mapped to slant by the
     *        standard obliquity only (not scaled by storm_gain, sed_elev_exp or the
     *        storm window).  0 (default) = no turbulence.
     */
    IonoSample evaluate(double rx_lat_deg, double rx_lon_deg,
                        double az_deg, double el_deg, const GnssTime& t,
                        double quiet_slant_stec_tecu, double storm_gain = 1.0,
                        double tid_period_override_s = 0.0, int tid_slot = -1,
                        double tid_vtec_explicit = -1.0, double turb_vtec = 0.0) const;

    /**
     * @brief Deterministic epoch-to-epoch ionospheric turbulence, vertical TECU.
     *
     * A severe geomagnetic storm does not only raise the TEC level; it makes the field
     * rough, so a ray's STEC changes between epochs with no trend and no period.  The
     * smooth GIM background plus raised-cosine pulses cannot produce that: they are
     * differentiable, so a geometry-free QC plot of such data looks quiet even at
     * 120 TECU.
     *
     * Returns a zero-mean value with 1-sigma @c cfg.turb_sigma_tecu, scaled per
     * satellite by a stable factor in [0.6, 1.4] (so the affected satellites are not
     * uniformly bad) and by the raised-cosine turbulence window (peak 1; no window when
     * @c cfg.turb_window_duration_s <= 0).  Each epoch's value is a 3-epoch moving
     * average of hashed standard normals of (PRN, epoch), normalised by sqrt(3) so the
     * marginal sigma is unchanged; epochs are @c cfg.turb_step_s apart (30 s when
     * <= 0).  The series is correlated between epochs (lag-1 correlation 2/3), which
     * gives structure on a 60–120 s scale at 30 s spacing.  Because the value is a hash
     * rather than an RNG stream, it is reproducible under any evaluation order and
     * consumes no shared random stream, so enabling turbulence cannot shift the
     * noise/multipath/slip realisation.
     *
     * Only satellites listed in @c cfg.turb_sats are affected; everything else returns 0.
     */
    double turbVtec(const std::string& prn, const GnssTime& t) const;

    /**
     * @brief First-order slant ionospheric delay (metres) at carrier @p f_hz.
     *
     * Positive return = code group delay (+I); carrier-phase advance is the
     * negative of this value (-I).  Convert phase to cycles by dividing the
     * metric phase term by the wavelength.  Returns 0 for @p f_hz <= 0.
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
