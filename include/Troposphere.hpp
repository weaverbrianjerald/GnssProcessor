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
#include <Eigen/Dense>

namespace GnssProcessor {

    /**
     * @class Troposphere
     * @brief Simple troposphere helper for mapping functions and ZHD computation.
     *
     * This class implements lightweight tropospheric utilities used by the
     * processing pipeline: conversion of elevation to hydrostatic and wet
     * mapping functions and estimation of the Saastamoinen zenith hydrostatic
     * delay given a receiver position in ECEF coordinates.
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
         * @brief Compute tropospheric mapping functions.
         *
         * Computes the hydrostatic (dry) and wet mapping functions for a given
         * elevation angle using simple empirical formulas. Elevation is expected
         * in degrees.
         *
         * @param elevation Elevation angle in degrees (0..90). Values outside
         *                  this range will produce default mappings of 1.0 and
         *                  a warning will be logged.
         * @param[out] mf_d Output hydrostatic (dry) mapping function (unitless).
         * @param[out] mf_w Output wet mapping function (unitless).
         */
        void getTroposphereMapping(double elevation, double& mf_d, double& mf_w) const;
        
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
    };

}

#endif // TROPOSPHERE_HPP