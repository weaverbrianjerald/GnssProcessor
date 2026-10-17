#pragma once

/**
 * @file Vmf3.hpp
 * @brief Vienna Mapping Function 3 (VMF3), empirical form driven by GPT3.
 *
 * VMF3 (Landskron & Boehm 2018) uses the Marini continued-fraction mapping
 * form, as VMF1 and GMF do, and supplies its b and c coefficients from a
 * degree/order-12 spherical-harmonic expansion of ECMWF-derived ray-traced
 * data (2001–2010), with five seasonal harmonic columns per spherical-harmonic
 * term.  The "a" coefficients come from the GPT3 5° grid (@c Gpt3), which is
 * derived from the same NWM climatology, so one GPT3 grid load serves both the
 * Saastamoinen a-priori P/T and the VMF3 mapping function.
 *
 * The empirical VMF3 differs from operational (time-varying) VMF3 mainly in
 * the @c a coefficients: GPT3 supplies a climatological annual plus semi-annual
 * harmonic, whereas operational VMF3 carries NWM-derived @c a_h/@c a_w at a
 * 6-hour cadence.  For cm-level PPP the climatology is about 1 mm worse than
 * operational VMF3, and it needs no product chain and no 6-hourly downloads.
 *
 * Reference:
 *   Landskron, D., and J. Boehm (2018), VMF3/GPT3: refined discrete and
 *   empirical troposphere mapping functions, J. Geod., 92(4), 349–360.
 */

namespace GnssProcessor {

    /**
     * @class Vmf3
     * @brief Static VMF3 mapping-function evaluator.
     *
     * Holds no state: the spherical-harmonic coefficients are constexpr arrays
     * in @c Vmf3Coefficients.hpp.  Each call recomputes the Legendre functions
     * and synthesizes b/c at the receiver location and DOY; the cost is small
     * and the caller amortizes it over the satellites of one epoch.
     */
    class Vmf3 {
    public:
        /**
         * @brief Compute VMF3 hydrostatic and wet mapping functions.
         *
         * Steps (following @c vmf3.m):
         *  1. Build the Legendre arrays V(n,m), W(n,m) up to degree 12 from
         *     the unit vector at (lat, lon) with the standard Cartesian
         *     recursion.
         *  2. Synthesize the five seasonal-harmonic columns of b_h, c_h, b_w
         *     and c_w as spherical-harmonic sums over the 91 (n, m) terms, each
         *     from two coefficient sets (anm on V, bnm on W): 8 sums in total.
         *  3. Evaluate the resulting 5-coefficient harmonics at @p doy.
         *  4. Apply the Marini continued fraction with the given @p a_h,
         *     @p a_w and the synthesized b/c values.
         *
         * The function is pure: no global state and no allocation beyond two
         * fixed 14 × 14 Legendre tables (V and W, 1-based) on the stack.
         *
         * @param lat_rad        Geodetic latitude (radians).
         * @param lon_rad        Geodetic longitude (radians).
         * @param doy            Fractional day-of-year.
         * @param elevation_rad  Elevation angle (radians, 0..π/2).
         * @param a_h            VMF hydrostatic "a" coefficient (from GPT3, dimensionless, about 1.2e-3).
         * @param a_w            VMF wet "a" coefficient (from GPT3, dimensionless, about 0.55e-3).
         * @param[out] mf_d      Hydrostatic mapping function (unitless, ≥1);
         *                       1.0 when the continued-fraction denominator is
         *                       below 1e-12 in magnitude.
         * @param[out] mf_w      Wet mapping function (unitless, ≥1); same
         *                       fallback as @p mf_d.
         */
        static void computeMapping(double lat_rad, double lon_rad, double doy,
                                    double elevation_rad, double a_h, double a_w,
                                    double& mf_d, double& mf_w);
    };

} // namespace GnssProcessor
