#pragma once

/**
 * @file Vmf3.hpp
 * @brief Vienna Mapping Function 3 (VMF3) — empirical from GPT3.
 *
 * VMF3 (Landskron & Boehm 2018) shares the Marini continued-fraction
 * mapping form with VMF1/GMF but supplies its b and c coefficients
 * from a degree/order-12 spherical-harmonic expansion of ECMWF-derived
 * ray-traced data (2001–2010), with five seasonal harmonic columns per
 * spherical-harmonic term.  The "a" coefficients come from the GPT3
 * 5° grid (@c Gpt3) which is itself derived from the same NWM
 * climatology — so a single GPT3 grid load supports both the
 * Saastamoinen a-priori P/T and the VMF3 mapping function in one pass.
 *
 * The empirical VMF3 differs from operational (time-varying) VMF3
 * primarily in the @c a coefficients: GPT3 supplies a climatological
 * annual + semi-annual harmonic, whereas operational VMF3 carries
 * NWM-derived @c a_h/@c a_w at 6-hour cadence.  At cm-level PPP this
 * climatology is ~1 mm worse than operational VMF3 — but it requires
 * no product chain and no 6-hourly downloads.
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
     * Holds no state — the spherical-harmonic coefficients live in
     * @c Vmf3Coefficients.hpp as constexpr arrays.  Every call
     * recomputes Legendre functions and synthesizes b/c at the
     * receiver location and DOY, which is cheap and amortized over
     * many satellites in a single epoch by the caller.
     */
    class Vmf3 {
    public:
        /**
         * @brief Compute VMF3 hydrostatic and wet mapping functions.
         *
         * Steps (matches @c vmf3.m verbatim):
         *  1. Build Legendre V(n,m), W(n,m) arrays up to degree 12 from
         *     the unit vector at (lat, lon) using the standard
         *     Cartesian recursion.
         *  2. Synthesize seasonal-harmonic columns of b_h, c_h, b_w,
         *     c_w via spherical-harmonic sum (1·91 + 2·91 + … = 8 sums).
         *  3. Evaluate the resulting 5-coefficient harmonics at @p doy.
         *  4. Apply the Marini continued fraction with the provided
         *     @p a_h, @p a_w and synthesized b/c values.
         *
         * The function is pure: no global state, no allocation beyond
         * a fixed 13 × 13 Legendre table on the stack.
         *
         * @param lat_rad        Geodetic latitude (radians).
         * @param lon_rad        Geodetic longitude (radians).
         * @param doy            Fractional day-of-year.
         * @param elevation_rad  Elevation angle (radians, 0..π/2).
         * @param a_h            VMF hydrostatic "a" coefficient (from GPT3).
         * @param a_w            VMF wet "a" coefficient (from GPT3).
         * @param[out] mf_d      Hydrostatic mapping function (unitless, ≥1).
         * @param[out] mf_w      Wet mapping function (unitless, ≥1).
         */
        static void computeMapping(double lat_rad, double lon_rad, double doy,
                                    double elevation_rad, double a_h, double a_w,
                                    double& mf_d, double& mf_w);
    };

} // namespace GnssProcessor
