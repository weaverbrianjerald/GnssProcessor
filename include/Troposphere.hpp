/**
 * @file Troposphere.hpp
 * @brief Troposphere model and utility functions for tropospheric delays.
 *
 * Provides routines to compute tropospheric mapping functions and the
 * Saastamoinen zenith hydrostatic delay (ZHD) used by the GNSS processor.
 */

#ifndef TROPOSPHERE_HPP
#define TROPOSPHERE_HPP

#include "LogManager.hpp"
#include "GnssConfig.hpp"
#include "TimeUtils.hpp"
#include <Eigen/Dense>

namespace GnssProcessor {

    /**
     * @class Troposphere
     * @brief Troposphere helper for mapping functions and ZHD computation.
     *
     * This class implements tropospheric utilities used by the processing
     * pipeline: the Global Mapping Function (GMF) for hydrostatic and wet
     * mapping, and estimation of the Saastamoinen zenith hydrostatic delay
     * given a receiver position in ECEF coordinates.
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
         * @brief Compute tropospheric mapping functions using the Global Mapping Function (GMF).
         *
         * Computes the hydrostatic (dry) and wet mapping functions for a given
         * elevation angle using the GMF model (Boehm et al. 2006), which accounts
         * for latitude, height and day-of-year. Falls back to simple empirical
         * mappings if receiver position is unavailable.
         *
         * @param elevation Elevation angle in degrees (0..90).
         * @param receiver_pos Receiver ECEF position (meters) for GMF computation.
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
         * Estimates station pressure from height using a standard atmosphere
         * approximation (reads sea-level pressure/temperature from the
         * configuration) and returns the Saastamoinen hydrostatic ZHD in meters.
         *
         * @param receiver_pos Receiver position in ECEF coordinates (meters).
         * @return Zenith hydrostatic delay (ZHD) in meters.
         */
        double computeSaastamoinenZHD(const Eigen::Vector3d& receiver_pos) const;
    private:
        LogManager& log_manager_;                         /**< Logger for debugging and info. */
        const GnssConfig& config_;                        /**< Configuration settings (read-only). */

        /**
         * @brief Evaluate the continued-fraction mapping function form.
         *
         * Computes 1/mf using the Marini (1972) continued fraction:
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

#endif // TROPOSPHERE_HPP