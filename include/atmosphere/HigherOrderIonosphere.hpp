#pragma once

/**
 * @file HigherOrderIonosphere.hpp
 * @brief Second- and third-order ionospheric delay corrections.
 *
 * The standard first-order ionospheric delay
 * @c I₁ = 40.3·STEC/f² is the term cancelled (in IF) or estimated (in
 * PPP-RTK uncombined) by the rest of the engine.  Higher-order terms in
 * the Appleton-Hartree refractive-index expansion enter the observable
 * with different frequency dependences:
 *
 *  - Second order (B-field-dependent group delay): @c I₂ ∝ STEC·B·cos(θ_B)/f³,
 *    typically 0–2 cm at GPS L1 during high TEC.  Cancels in IF only to
 *    the extent the B field is the same at both frequencies, which it is —
 *    so IF removes ~85% of I₂ but leaves a residual proportional to
 *    @c f₁²/(f₁²-f₂²) · I₂(f₁) − @c f₂²/(f₁²-f₂²) · I₂(f₂).
 *
 *  - Third order (TEC-shape-dependent): @c I₃ ∝ STEC²·η/f⁴, typically
 *    sub-mm to 5 mm at L1.  The shape factor η depends on the integrated
 *    profile of @c N_e² along the ray; here we use the conventional
 *    fixed value of @c η = 0.66 from Hoque & Jakowski (2008).
 *
 * Sign convention (matching the rest of the engine):
 *  - For code observables, the correction is added to the modeled
 *    value (positive group delay).
 *  - For carrier-phase observables, the correction has opposite sign
 *    and is scaled by @c -1/2 (I₂) or @c -1/3 (I₃) due to the specific
 *    frequency dependence of group vs phase velocity at each order.
 *
 * The class is stateless beyond the immutable physics constants and
 * the cached magnetic-field axis.  All inputs (STEC, IPP geometry,
 * frequency, signal direction) come from the caller — typically a
 * @c GimQuery from @c GlobalIonosphereMap plus the satellite-receiver
 * line of sight already computed by @c PppModel::computeModels.
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
         * pierce point on the IONEX shell, and projected onto the
         * line-of-sight unit vector.
         *
         * Returns 0 if any input is degenerate (zero frequency, zero
         * receiver radius) — caller can treat that as "no correction
         * applied" and proceed without higher-order modeling on the row.
         *
         * @param rx_pos_ecef       Receiver ECEF position (m).
         * @param sat_pos_ecef      Satellite ECEF position (m).
         * @param ipp_lat_rad       Geocentric latitude of the IPP (rad).
         *                          As produced by @c GimQuery::ipp_lat_rad.
         * @param ipp_lon_rad       Geocentric longitude of the IPP (rad).
         * @param ipp_radius_m      Distance of the IPP from geocentre (m).
         * @param stec_tecu         Absolute slant TEC at the IPP (TECU).
         * @param frequency_hz      Carrier frequency the correction is wanted at (Hz).
         * @param meas_type         @c 'C' for code (group delay, positive
         *                          for I₂ and I₃) or @c 'L' for phase
         *                          (advance, sign flipped and scaled by
         *                          1/2 and 1/3 for I₂ and I₃ respectively).
         *                          Any other character returns 0.
         * @return Total I₂ + I₃ correction in metres, with sign such
         *         that it should be **added** to the modeled value.
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
