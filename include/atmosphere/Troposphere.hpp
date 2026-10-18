#pragma once

/**
 * @file Troposphere.hpp
 * @brief Troposphere model and utility functions for tropospheric delays.
 *
 * Provides routines to compute tropospheric mapping functions and the
 * Saastamoinen zenith hydrostatic delay (ZHD) used by the GNSS processor.
 */

#include "logging/LogManager.hpp"
#include "config/GnssConfig.hpp"
#include "math/TimeUtils.hpp"
#include <Eigen/Dense>
#include <memory>

namespace GnssProcessor {

    class Gpt3;

    /**
     * @class Troposphere
     * @brief Troposphere helper for mapping functions and ZHD computation.
     *
     * This class implements tropospheric utilities used by the processing
     * pipeline: hydrostatic and wet mapping (Niell NMF tables, or VMF3 from the
     * GPT3 grid when configured), and estimation of the Saastamoinen zenith
     * hydrostatic delay given a receiver position in ECEF coordinates.
     */
    class Troposphere {
    public:
        /**
         * @brief Construct a Troposphere helper.
         * @param config Reference to the global GNSS configuration (read-only).
         * @param log_mgr Reference to the LogManager for logging (kept by reference).
         */
        Troposphere(const GnssConfig& config, LogManager& log_mgr);

        /**
         * @brief Destructor (out-of-line so the forward-declared @c Gpt3
         *        type is complete in the .cpp).
         */
        ~Troposphere();

        /**
         * @brief Compute hydrostatic and wet tropospheric mapping functions.
         *
         * Computes the hydrostatic (dry) and wet mapping functions for a given
         * elevation angle.  With @c TroposphereMappingFunction::Vmf3 and a loaded
         * GPT3 grid, VMF3 is evaluated from the GPT3 "a" coefficients.  Otherwise,
         * or when the GPT3 query fails, the Niell (1996) NMF tables are used (the
         * method name and log text call this path "GMF", after the Global
         * Mapping Function of Boehm et al. 2006); they account for
         * latitude, height and day-of-year.  An invalid receiver position (norm
         * below 1 km or non-finite) selects the Black & Eisner mapping; an
         * elevation outside 0..90 deg returns 1.0 for both outputs.
         *
         * @param elevation Elevation angle in degrees (0..90).
         * @param receiver_pos Receiver ECEF position (meters) for the latitude/height terms.
         * @param time GNSS epoch used to derive day-of-year.
         * @param[out] mf_d Output hydrostatic (dry) mapping function (unitless).
         * @param[out] mf_w Output wet mapping function (unitless).
         */
        void getTroposphereMapping(double elevation, const Eigen::Vector3d& receiver_pos,
                                   const GnssTime& time,
                                   double& mf_d, double& mf_w) const;
        
        /**
         * @brief Compute Saastamoinen zenith hydrostatic delay (ZHD).
         *
         * Station pressure is reduced from the configured sea-level pair via
         * the standard-atmosphere lapse formula, unless the a priori source is
         * @c TroposphereAprioriSource::Gpt3 and the GPT3 grid loaded: then it
         * is queried from the grid at the receiver lat/lon/h/doy (temperature
         * is not used).  The mapping function does not choose the pressure
         * source: with @c TroposphereMappingFunction::Vmf3 and the a priori
         * source left at @c StandardAtmosphere the grid is loaded for the
         * mapping coefficients only, and the standard atmosphere still applies.
         * On a failed query the call falls back to the standard-atmosphere
         * value without a log line, so a single bad query does not abort the
         * run.
         *
         * @param receiver_pos Receiver position in ECEF coordinates (meters).
         * @param time         GNSS epoch (used to derive day-of-year for the
         *                     GPT3 harmonics; ignored when the pressure comes
         *                     from the standard atmosphere).
         * @return Zenith hydrostatic delay (ZHD) in meters.
         */
        double computeSaastamoinenZHD(const Eigen::Vector3d& receiver_pos,
                                       const GnssTime& time) const;
    private:
        LogManager& log_manager_;                         /**< Logger for debugging and info. */
        const GnssConfig& config_;                        /**< Configuration settings (read-only). */
        std::unique_ptr<Gpt3> gpt3_;                      /**< GPT3 grid (non-null when the a priori source is Gpt3 or the mapping function is Vmf3, and the load succeeded). */

        /**
         * @brief Evaluate the continued-fraction mapping function form.
         *
         * Computes mf using the Marini (1972) continued fraction (returns 1.0
         * when the denominator is below 1e-12 in magnitude):
         *   mf(el) = (1 + a/(1 + b/(1 + c))) / (sin(el) + a/(sin(el) + b/(sin(el) + c)))
         *
         * @param sin_el Sine of elevation angle.
         * @param a First coefficient.
         * @param b Second coefficient.
         * @param c Third coefficient.
         * @return Mapping function value (unitless, >= 1).
         */
        static double mariniMapping(double sin_el, double a, double b, double c);

        /**
         * @brief Compute day-of-year from a GnssTime epoch.
         * @param time GNSS time.
         * @return Day-of-year as a fractional double (1.0 = Jan 1 00:00).
         */
        static double getDayOfYear(const GnssTime& time);
    };

}