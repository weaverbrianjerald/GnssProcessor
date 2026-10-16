#pragma once

/**
 * @file HigherOrderIonosphere.hpp
 * @brief Second- and third-order ionospheric delay corrections.
 *
 * The first-order ionospheric delay @c I₁ = 40.3·STEC/f² is cancelled
 * (in IF) or estimated (in uncombined PPP-RTK) elsewhere in the engine.
 * Higher-order terms of the Appleton-Hartree refractive-index expansion
 * enter the observable with other frequency dependences:
 *
 *  - Second order (magnetic-field-dependent): @c I₂ ∝ STEC·B·cos(θ_k)/f³,
 *    with @c θ_k the angle between the geomagnetic field @c B and the
 *    propagation direction @c k̂ (satellite to receiver), so the code term
 *    is a delay when the signal travels along @c B and an advance when it
 *    travels against it.  Typically 0–2 cm at GPS L1 at high TEC.  The IF
 *    combination does not
 *    cancel an f⁻³ term.  Its residual is
 *    @c f₁²/(f₁²-f₂²) · I₂(f₁) − @c f₂²/(f₁²-f₂²) · I₂(f₂)
 *    = −f₁²/(f₂·(f₁+f₂)) · I₂(f₁), about −0.72·I₂(L1) for GPS L1/L2.
 *
 *  - Third order (dependent on the electron-density profile):
 *    @c I₃ ∝ η·N_max·STEC/f⁴, typically sub-mm to a few mm at L1.  It
 *    scales with @c ∫N_e²dl, which the shape factor @c η = 0.66
 *    (Hoque & Jakowski 2008) relates to @c N_max·STEC, @e not to @c STEC².
 *    @c N_max (F2-layer peak density) follows the Hoque-Jakowski linear fit
 *    in STEC.
 *
 * Sign convention (the same as in the rest of the engine):
 *  - For code observables, the correction is added to the modeled
 *    value: @c +7527·c·STEC·(B·k̂)/f³ for @c I₂ (signed, positive
 *    when @c B·k̂ > 0) and a positive group delay for @c I₃.  GPS is
 *    right-hand circularly polarized about @c k̂ (IEEE), which makes it the
 *    R (extraordinary) magnetoionic mode when @c k̂ is along @c B.  Using
 *    the receiver-to-satellite vector instead of @c k̂ inverts @c I₂; the
 *    engine did so until the sign fix
 *    (docs/refound/I2S_second_order_iono_sign_2026-10-01.md).
 *  - For carrier-phase observables, the correction has the opposite sign
 *    and is scaled by @c -1/2 (I₂) or @c -1/3 (I₃), from the ratio of
 *    phase to group delay at each order of the expansion.
 *
 * The class holds no state; the dipole field comes from the static
 * @c MagneticField helper.  All inputs (STEC, IPP geometry, frequency,
 * signal direction) come from the caller, in practice a @c GimQuery from
 * @c GlobalIonosphereMap plus the receiver and satellite positions passed
 * to @c PppModel::computeHigherOrderIonoCorrection.
 */

#include <Eigen/Dense>

namespace GnssProcessor {

    /**
     * @class HigherOrderIonosphere
     * @brief Closed-form @c I₂ + @c I₃ correction for a single line of sight.
     *
     * No external dependency beyond @c MagneticField.  Construction is
     * cheap; a single instance shared across all observations is fine.
     */
    class HigherOrderIonosphere {
    public:
        /**
         * @brief Default-construct.  No state to initialize.
         */
        HigherOrderIonosphere() = default;

        /**
         * @brief Compute the signed I₂ + I₃ correction to add to the
         *        modeled observable.
         *
         * The correction is referenced to a single-layer thin-shell
         * model: the magnetic field is evaluated at the ionospheric
         * pierce point on the IONEX shell and projected onto the
         * propagation unit vector @c k̂ (from @p sat_pos_ecef to
         * @p rx_pos_ecef).
         *
         * Returns 0 when an input is degenerate (frequency ≤ 0, non-finite
         * STEC, IPP radius ≤ 0 or non-finite, receiver-satellite distance
         * below 1 m, @p meas_type other than 'C' or 'L') or the result is
         * not finite.  A caller can treat 0 as "no correction applied" and
         * proceed without higher-order modeling on the row.
         *
         * @param rx_pos_ecef       Receiver ECEF position (m).
         * @param sat_pos_ecef      Satellite ECEF position (m).
         * @param ipp_lat_rad       Latitude of the IPP on the shell (rad), as produced
         *                          by @c GimQuery::ipp_lat_rad.  Used here as a
         *                          spherical latitude, which matches the thin shell,
         *                          but it is propagated spherically from a geodetic
         *                          receiver seed; see the convention note on
         *                          @c IppGeometry before reusing it.
         * @param ipp_lon_rad       Longitude of the IPP on the shell (rad).
         * @param ipp_radius_m      Distance of the IPP from geocentre (m).
         * @param stec_tecu         Absolute slant TEC at the IPP (TECU).
         * @param frequency_hz      Carrier frequency the correction is wanted at (Hz).
         * @param meas_type         @c 'C' for code (group delay: I₂ signed by
         *                          @c B·k̂, I₃ positive) or @c 'L' for phase
         *                          (advance, sign flipped and scaled by
         *                          1/2 and 1/3 for I₂ and I₃ respectively).
         *                          Any other character returns 0.
         * @return Total I₂ + I₃ correction in metres, signed so that it
         *         is added to the modeled value.
         */
        double slantHigherOrderDelay(const Eigen::Vector3d& rx_pos_ecef,
                                     const Eigen::Vector3d& sat_pos_ecef,
                                     double ipp_lat_rad,
                                     double ipp_lon_rad,
                                     double ipp_radius_m,
                                     double stec_tecu,
                                     double frequency_hz,
                                     char meas_type) const;

        /**
         * @brief Break the total into its individual components for
         *        diagnostics / tracing.
         *
         * @c i2_m and @c i3_m carry the same sign convention as the
         * total returned by @c slantHigherOrderDelay (sign already
         * applied for the given @c meas_type).
         */
        struct Components {
            double i2_m = 0.0;
            double i3_m = 0.0;
            bool   valid = false;
        };
        Components slantHigherOrderComponents(const Eigen::Vector3d& rx_pos_ecef,
                                              const Eigen::Vector3d& sat_pos_ecef,
                                              double ipp_lat_rad,
                                              double ipp_lon_rad,
                                              double ipp_radius_m,
                                              double stec_tecu,
                                              double frequency_hz,
                                              char meas_type) const;
    };

} // namespace GnssProcessor
