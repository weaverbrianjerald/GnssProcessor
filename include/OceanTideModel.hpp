#pragma once
#ifndef OCEAN_TIDE_MODEL_HPP
#define OCEAN_TIDE_MODEL_HPP

/**
 * @file OceanTideModel.hpp
 * @brief Ocean tidal loading (OTL) displacement model using 11 major tidal constituents.
 *
 * Implements the IERS Conventions (2010) approach for computing station
 * displacements due to ocean tidal loading. The model evaluates 11 major
 * tidal constituents (M2, S2, N2, K2, K1, O1, P1, Q1, Mf, Mm, Ssa) using
 * astronomically-derived tidal arguments (Doodson variables).
 *
 * Station-specific amplitude and phase coefficients must be supplied externally,
 * e.g. from the Onsala Space Observatory ocean loading provider
 * (http://holt.oso.chalmers.se/loading/) or the IERS ocean loading tables.
 *
 * When no station-specific coefficients are available, the model provides a
 * latitude-dependent approximate model as fallback.
 */

#include "LogManager.hpp"
#include "TimeUtils.hpp"
#include <Eigen/Dense>
#include <array>
#include <string>
#include <vector>

namespace GnssProcessor {

    /**
     * @brief Per-constituent amplitude and Greenwich phase lag for one station.
     *
     * Each of the three components (radial/Up, West/tangential, South/tangential)
     * has an amplitude (meters) and a Greenwich phase lag (degrees).
     *
     * Convention follows the BLQ format from the Onsala loading service:
     *   - amp_up, amp_west, amp_south  (meters)
     *   - phase_up, phase_west, phase_south  (degrees)
     */
    struct OtlConstituentCoeffs {
        double amp_up{ 0.0 };        ///< Radial (Up) amplitude [m]
        double amp_west{ 0.0 };      ///< West (tangential) amplitude [m]
        double amp_south{ 0.0 };     ///< South (tangential) amplitude [m]
        double phase_up{ 0.0 };      ///< Radial (Up) Greenwich phase lag [deg]
        double phase_west{ 0.0 };    ///< West Greenwich phase lag [deg]
        double phase_south{ 0.0 };   ///< South Greenwich phase lag [deg]
    };

    /**
     * @brief Complete set of OTL coefficients for one station (11 constituents).
     *
     * Constituent order: M2, S2, N2, K2, K1, O1, P1, Q1, Mf, Mm, Ssa
     * (matches the standard BLQ file format).
     */
    struct OtlStationCoefficients {
        std::string station_name;                          ///< Station identifier (e.g. "AMC4")
        std::array<OtlConstituentCoeffs, 11> constituents; ///< Coefficients for 11 major constituents
        bool valid{ false };                               ///< True if coefficients have been loaded
    };

    /**
     * @brief Names of the 11 major tidal constituents in standard BLQ order.
     */
    static constexpr std::array<const char*, 11> OTL_CONSTITUENT_NAMES = {
        "M2", "S2", "N2", "K2", "K1", "O1", "P1", "Q1", "Mf", "Mm", "Ssa"
    };

    /**
     * @class OceanTideModel
     * @brief Compute ocean tidal loading station displacement.
     *
     * The model evaluates:
     *   dr_k(t) = sum_j A_{k,j} · cos(omega_j·t + chi_j(t) + u_j(t) - phi_{k,j})
     *
     * where k in {up, west, south}, j indexes the 11 constituents, omega_j is the
     * tidal angular frequency, chi_j(t) is the astronomical argument, u_j(t) is
     * the nodal correction angle, A_{k,j} is the station amplitude, and phi_{k,j}
     * is the Greenwich phase lag.
     *
     * The result is converted from a local (Up, West, South) frame to ECEF.
     */
    class OceanTideModel {
    public:
        /**
         * @brief Construct the OceanTideModel.
         * @param log_mgr Reference to the LogManager for diagnostic output.
         */
        explicit OceanTideModel(LogManager& log_mgr);

        /**
         * @brief Set station-specific OTL coefficients.
         * @param coeffs OTL station coefficients (amplitudes and phases for 11 constituents).
         */
        void setStationCoefficients(const OtlStationCoefficients& coeffs);

        /**
         * @brief Check whether station-specific OTL coefficients are loaded.
         * @return True if valid coefficients are available.
         */
        bool hasStationCoefficients() const { return station_coeffs_.valid; }

        /**
         * @brief Compute the ocean tidal loading displacement in ECEF coordinates.
         *
         * @param rx_pos Receiver ECEF position (meters).
         * @param time   GNSS epoch (GPS week + TOW).
         * @return ECEF displacement vector (meters). Returns zero vector on error.
         */
        Eigen::Vector3d computeDisplacement(const Eigen::Vector3d& rx_pos, const GnssTime& time) const;

        /**
         * @brief Compute approximate OTL displacement when no station coefficients are available.
         *
         * Uses a latitude-dependent heuristic scaled from global average coastal amplitudes.
         * This is a rough approximation and should only be used when no BLQ data is available.
         *
         * @param rx_pos Receiver ECEF position (meters).
         * @param time   GNSS epoch (GPS week + TOW).
         * @return ECEF displacement vector (meters). Returns zero vector on error.
         */
        Eigen::Vector3d computeApproximateDisplacement(const Eigen::Vector3d& rx_pos, const GnssTime& time) const;

        /**
         * @brief Parse a BLQ-format ocean loading file and extract coefficients for a station.
         *
         * @param blq_path Path to the BLQ file.
         * @param station_name Station name to search for (case-insensitive).
         * @param[out] coeffs Output coefficients if found.
         * @return True if station was found and parsed successfully.
         */
        bool parseBLQ(const std::string& blq_path, const std::string& station_name,
                      OtlStationCoefficients& coeffs) const;

    private:
        LogManager& log_manager_;
        OtlStationCoefficients station_coeffs_;

        /**
         * @brief Compute the six fundamental Delaunay arguments and GMST at an epoch.
         *
         * Returns an array of 6 values (radians):
         *   [0] l   — Mean anomaly of the Moon
         *   [1] l'  — Mean anomaly of the Sun
         *   [2] F   — Mean argument of latitude of the Moon
         *   [3] D   — Mean elongation of the Moon from the Sun
         *   [4] ?   — Mean longitude of the ascending node of the Moon
         *   [5] ?   — Greenwich Mean Sidereal Time (GMST)
         *
         * @param t_centuries Julian centuries of TT from J2000.0.
         * @param ut1_frac UT1 fractional day for GMST computation.
         * @return Array of 6 fundamental arguments in radians.
         */
        std::array<double, 6> computeFundamentalArguments(double t_centuries, double ut1_frac) const;

        /**
         * @brief Compute the tidal argument (astronomical phase) for each constituent.
         *
         * Uses the Doodson multipliers to combine fundamental arguments into
         * the astronomical argument ?_j for each of the 11 constituents.
         *
         * @param fund_args Array of 6 fundamental arguments (radians).
         * @return Array of 11 tidal arguments (radians), one per constituent.
         */
        std::array<double, 11> computeTidalArguments(const std::array<double, 6>& fund_args) const;

        /**
         * @brief Doodson number multipliers for the 11 major constituents.
         *
         * Each row contains 6 integers corresponding to the multipliers for
         * [GMST+pi, s, h, p, N', ps] in the tidal argument, where the tau
         * (lunar time) variable has been expanded as tau = GMST + pi - s.
         * Column 2 (s) therefore equals (Doodson_s - Doodson_tau).
         *
         * Order: M2, S2, N2, K2, K1, O1, P1, Q1, Mf, Mm, Ssa
         */
        static constexpr int DOODSON_MULTIPLIERS[11][6] = {
            { 2, -2,  0,  0,  0,  0 },  // M2:  Doodson 255.555 -> tau=2,s=0,h=0,p=0
            { 2,  0, -2,  0,  0,  0 },  // S2:  Doodson 273.555 -> tau=2,s=2,h=-2,p=0
            { 2, -3,  0,  1,  0,  0 },  // N2:  Doodson 245.655 -> tau=2,s=-1,h=0,p=1
            { 2,  0,  0,  0,  0,  0 },  // K2:  Doodson 275.555 -> tau=2,s=2,h=0,p=0
            { 1,  0,  0,  0,  0,  0 },  // K1:  Doodson 165.555 -> tau=1,s=1,h=0,p=0
            { 1, -2,  0,  0,  0,  0 },  // O1:  Doodson 145.555 -> tau=1,s=-1,h=0,p=0
            { 1,  0, -2,  0,  0,  0 },  // P1:  Doodson 163.555 -> tau=1,s=1,h=-2,p=0
            { 1, -3,  0,  1,  0,  0 },  // Q1:  Doodson 135.655 -> tau=1,s=-2,h=0,p=1
            { 0,  2,  0,  0,  0,  0 },  // Mf:  Doodson 075.555 -> tau=0,s=2,h=0,p=0
            { 0,  1,  0, -1,  0,  0 },  // Mm:  Doodson 065.455 -> tau=0,s=1,h=0,p=-1
            { 0,  0,  2,  0,  0,  0 },  // Ssa: Doodson 057.555 -> tau=0,s=0,h=2,p=0
        };

        /**
         * @brief Angular speeds of the 11 constituents (degrees per hour).
         *
         * Used for the approximate model when Doodson arguments are not needed.
         * Order: M2, S2, N2, K2, K1, O1, P1, Q1, Mf, Mm, Ssa
         */
        static constexpr double CONSTITUENT_SPEEDS_DEG_PER_HOUR[11] = {
            28.9841042,   // M2
            30.0000000,   // S2
            28.4397295,   // N2
            30.0821373,   // K2
            15.0410686,   // K1
            13.9430356,   // O1
            14.9589314,   // P1
            13.3986609,   // Q1
             1.0980331,   // Mf
             0.5443747,   // Mm
             0.0821373,   // Ssa
        };
    };

} // namespace GnssProcessor

#endif // OCEAN_TIDE_MODEL_HPP
