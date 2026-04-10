#pragma once

/**
 * @file GnssCorrections.hpp
 * @brief Satellite and geodetic correction utilities used by GNSS processing.
 *
 * This header declares the GnssCorrections class which encapsulates common
 * corrections applied to GNSS measurements and navigation products:
 * - Satellite clock interpolation and relativistic corrections
 * - Sagnac and Shapiro path delays
 * - Tide and loading corrections (solid earth, pole tide, ocean, atmospheric)
 * - Bias retrieval and phase wind-up calculation
 *
 * The class is constructed with optional product data (clocks, ERPs, biases,
 * orbits) and may reference configuration and RINEX parser objects for
 * behaviour that depends on runtime settings or observation metadata.
 */

#include "LogManager.hpp"
#include "TimeUtils.hpp"
#include "PppTypes.hpp"
#include "OceanTideModel.hpp"
#include <array>
#include <set>
#include <unordered_map>
#include <vector>
#include <Eigen/Dense>
#include <string>

namespace GnssProcessor {

    // Forward declarations to avoid header cycles
    class GnssConfig;
    class RinexParser;

    /**
     * @class GnssCorrections
     * @brief Compute and provide GNSS-related corrections for measurements.
     *
     * GnssCorrections provides a central place to compute corrections that are
     * frequently required when forming GNSS measurement models (e.g. for PPP).
     * Implementations typically depend on externally parsed product files such
     * as CLK (satellite clocks), ERP (Earth rotation parameters), orbit
     * products, and optional bias files. A LogManager reference is required
     * for diagnostic logging.
     */
    class GnssCorrections {

    public:
        /**
         * @brief Minimal constructor without product data.
         * @param log_mgr Reference to the LogManager used for logging.
         *
         * Use this constructor when product files are not available but logging
         * is still required.
         */
        explicit GnssCorrections(LogManager& log_mgr);

        /**
         * @brief Construct with clocks and ERPs.
         * @param log_mgr Reference to the LogManager used for logging.
         * @param clocks Vector of satellite clock samples (ClockData).
         * @param erps Vector of Earth rotation parameter samples (ErpData).
         *
         * Commonly used by product/geometry parsers which already have clocks
         * and ERP loaded at construction time.
         */
        GnssCorrections(LogManager& log_mgr,
            const std::vector<ClockData>& clocks,
            const std::vector<ErpData>& erps);

        /**
         * @brief Full constructor with optional biases and RINEX access.
         * @param log_mgr Reference to the LogManager used for logging.
         * @param config Reference to the processing configuration (may be used
         *               to control correction options).
         * @param clocks Vector of satellite clock samples (ClockData).
         * @param erps Vector of Earth rotation parameter samples (ErpData).
         * @param biases Vector of bias entries (BiasData).
         * @param rinex Reference to a RinexParser for receiver/observation metadata.
         *
         * Use when full product context is available and some corrections
         * depend on configuration or RINEX header information.
         */
        GnssCorrections(LogManager& log_mgr,
            const GnssConfig& config,
            const std::vector<ClockData>& clocks,
            const std::vector<ErpData>& erps,
            const std::vector<BiasData>& biases,
            const RinexParser& rinex);

        /**
         * @brief Update clocks and ERPs after construction.
         * @param clocks Vector of satellite clock samples (ClockData).
         * @param erps Vector of Earth rotation parameter samples (ErpData).
         *
         * Allows replacing or refreshing time-dependent product containers
         * without recreating the GnssCorrections object.
         */
        void setClocksAndErps(const std::vector<ClockData>& clocks, const std::vector<ErpData>& erps);

        /**
         * @brief Get satellite clock correction in seconds.
         * @param satellite Satellite identifier string (e.g. "G01", "E11").
         * @param time Epoch at which the correction is requested.
         * @return Clock correction in seconds (bias + interpolation).
         *
         * This routine uses available CLK data to provide a best-effort clock
         * correction for the requested satellite and epoch.
         */
        double getSatelliteClockCorrection(const std::string& satellite, const GnssTime& time) const;

        /**
         * @brief Interpolate clock values from clock samples.
         * @param satellite Satellite identifier string.
         * @param time Epoch for interpolation.
         * @param clocks Container of ClockData from which to interpolate.
         * @return Interpolated clock value in seconds.
         *
         * Performs interpolation (or nearest-sample retrieval) using the
         * provided clock samples. Exact interpolation method (linear, spline)
         * is implementation-defined.
         */
        double interpolateClock(const std::string& satellite, const GnssTime& time,
            const std::vector<ClockData>& clocks) const;

        /**
         * @brief Compute relativistic clock correction due to satellite orbit.
         * @param satellite Satellite identifier string.
         * @param sat_pos Satellite ECEF position (meters).
         * @param sat_vel Satellite ECEF velocity (meters/second).
         * @return Relativistic correction in seconds.
         *
         * Accounts for the classical relativistic effect from the satellite's
         * orbital motion and eccentricity as used in GNSS clock modelling.
         */
        double computeRelativisticCorrection(const std::string& satellite, const Eigen::Vector3d& sat_pos,
            const Eigen::Vector3d& sat_vel) const;

        /**
         * @brief Compute the Shapiro (relativistic path) delay.
         * @param satellite Satellite identifier string (unused in pure geometric form).
         * @param rx_pos Receiver ECEF position (meters).
         * @param sat_pos Satellite ECEF position (meters).
         * @return Shapiro delay in meters.
         *
         * The Shapiro delay is the additional path length caused by the
         * curvature of spacetime near massive bodies (primarily the Sun).
         */
        double computeShapiroDelay(const std::string& satellite, const Eigen::Vector3d& rx_pos,
            const Eigen::Vector3d& sat_pos) const;

        /**
         * @brief Compute Sagnac correction (rotation of the Earth).
         * @param rx_pos Receiver ECEF position (meters).
         * @param sat_pos Satellite ECEF position (meters).
         * @return Sagnac correction in meters.
         *
         * Accounts for signal path difference caused by Earth's rotation
         * between transmit and receive events.
         */
        double computeSagnacCorrection(const Eigen::Vector3d& rx_pos, const Eigen::Vector3d& sat_pos) const;

        /**
         * @brief Compute pole tide correction in ECEF coordinates.
         * @param rx_pos Receiver ECEF position (meters).
         * @param time Epoch for ERP/pole values.
         * @return ECEF displacement vector (meters) due to pole tide.
         *
         * Applies pole tide displacement to station coordinates using ERP data.
         */
        Eigen::Vector3d computePoleTideCorrection(const Eigen::Vector3d& rx_pos, const GnssTime& time) const;

        /**
         * @brief Compute solid Earth tide correction (LOS displacement).
         * @param receiver_pos Receiver ECEF position (meters).
         * @param sat_pos Satellite ECEF position (meters).
         * @param sun_pos Sun ECEF position (meters).
         * @param moon_pos Moon ECEF position (meters).
         * @param time Epoch for Step 2 frequency-dependent corrections (GnssTime: week + tow).
         * @return LOS displacement in meters to apply to the geometric range.
         *
         * Implements the IERS 2010 solid Earth tide displacement model
         * (Section 7.1.1, Steps 1+2) producing **conventional tide-free**
         * coordinates. This is consistent with IGS products (SP3/CLK/ITRF),
         * IERS 2010 Conventions, and RTKLIB. Step 2 includes long-period (m=0),
         * diurnal (m=1), and semi-diurnal (m=2) corrections from Tables 7.3a/b,
         * with the permanent tide handled by the 055.555 zero-frequency constituent.
         */
        double computeSolidEarthTide(const Eigen::Vector3d& receiver_pos, const Eigen::Vector3d& sat_pos,
            const Eigen::Vector3d& sun_pos, const Eigen::Vector3d& moon_pos,
            const GnssTime& time) const;

        /**
         * @brief Compute ocean tide correction (ECEF displacement).
         * @param rx_pos Receiver ECEF position (meters).
         * @param time Epoch for tide constituents.
         * @return ECEF displacement vector (meters) caused by ocean loading/tides.
         *
         * Ocean tide correction is often modeled as frequency-dependent loading
         * using ocean tide models and load Love numbers.
         */
        Eigen::Vector3d computeOceanTideCorrection(const Eigen::Vector3d& rx_pos, const GnssTime& time) const;

        /**
         * @brief Compute atmospheric loading correction (radial displacement).
         * @param rx_pos Receiver ECEF position (meters).
         * @param time Epoch when the loading is evaluated.
         * @return Radial displacement in meters due to atmospheric loading.
         *
         * Uses configured or climatological pressure/atmospheric parameters to
         * estimate station displacement from atmospheric mass loading.
         */
        double computeAtmosphericLoadingCorrection(const Eigen::Vector3d& rx_pos, const GnssTime& time) const;

        /**
         * @brief Get bias correction for a given satellite/signal at an epoch.
         * @param satellite Satellite identifier or receiver id string.
         * @param freq_type Frequency or signal type string (e.g. "L1", "C1").
         * @param time Epoch for which the bias applies.
         * @return Bias correction in meters (or zero if no matching bias).
         *
         * Searches internal bias table for an applicable entry and returns the
         * bias value; behaviour on overlapping entries is implementation-defined.
         */
        double getBiasCorrection(const std::string& satellite, const std::string& freq_type,
            const GnssTime& time) const;

        /**
         * @brief Check whether the BIA product contains any entry for a satellite+signal pair.
         *
         * This is a fast O(1) lookup against a set built at construction time.
         * It does not check time validity — only whether the pair exists at all.
         *
         * @param satellite Satellite identifier (e.g. "G01").
         * @param signal    RINEX observation code (e.g. "C1W", "L5Q").
         * @return True if at least one BiasData entry matches the pair.
         */
        bool hasBiasEntry(const std::string& satellite, const std::string& signal) const;

        /**
         * @brief Compute phase wind-up correction for carrier-phase.
         * @param satellite Satellite identifier string.
         * @param system Satellite system enum (System).
         * @param rx_pos Receiver ECEF position (meters).
         * @param sat_pos Satellite ECEF position (meters).
         * @param wavelength Carrier wavelength in meters.
         * @param time Epoch for any time-dependent quantities.
         * @param sun_pos Sun ECEF position (meters). When provided (non-zero), the
         *        satellite attitude is computed using the proper yaw-steering model
         *        (Sun direction + nadir). When zero, falls back to velocity-based attitude.
         * @param sat_vel Satellite ECEF velocity (meters/second). Used as fallback
         *        for attitude when sun_pos is not available or during eclipse/noon turn.
         * @return Phase wind-up correction in meters (to add to carrier range).
         *
         * Computes the phase bias caused by relative antenna orientation
         * (receiver vs satellite) and returns the equivalent length offset.
         */
        double computePhaseWindUp(const std::string& satellite, System system, const Eigen::Vector3d& rx_pos,
            const Eigen::Vector3d& sat_pos, double wavelength, const GnssTime& time,
            const Eigen::Vector3d& sun_pos = Eigen::Vector3d::Zero(),
            const Eigen::Vector3d& sat_vel = Eigen::Vector3d::Zero(),
            bool accumulate = false) const;

        /**
         * @brief Reset phase wind-up state for a specific satellite.
         * @param satellite Satellite identifier string (e.g. "G01").
         *
         * Call this when a cycle slip or ambiguity reset is detected for the
         * satellite to ensure phase wind-up continuity is not falsely maintained
         * across the discontinuity.
         */
        void resetPhaseWindUp(const std::string& satellite);

        /**
         * @brief Reset phase wind-up state for all satellites.
         *
         * Call this on a complete filter reset or at the start of a new session
         * to clear all accumulated phase wind-up history.
         */
        void resetAllPhaseWindUp();

        /**
         * @brief Set station-specific ocean tidal loading coefficients.
         * @param coeffs OTL station coefficients (11 constituents).
         *
         * When set, computeOceanTideCorrection() uses the full 11-constituent
         * model instead of the approximate fallback.
         */
        void setOceanTideCoefficients(const OtlStationCoefficients& coeffs);

        /**
         * @brief Load OTL coefficients from a BLQ file for a station.
         * @param blq_path Path to the BLQ file.
         * @param station_name Station name to search for.
         * @return True if coefficients were found and loaded.
         */
        bool loadOceanTideFromBLQ(const std::string& blq_path, const std::string& station_name);

        /**
         * @brief Check whether station-specific OTL coefficients are available.
         * @return True if OTL coefficients have been loaded.
         */
        bool hasOceanTideCoefficients() const;

    private:
        LogManager& log_manager_;                         ///< Logger for debugging and info
        std::vector<ClockData> clocks_;                   ///< Satellite clock data samples
        std::vector<ErpData> erps_;                       ///< Earth rotation parameters
        std::vector<BiasData> biases_;                    ///< Bias correction entries
        std::vector<OrbitData> orbits_;                   ///< Satellite orbit/ephemeris data

        // Optional external references (nullable pointers)
        const GnssConfig* config_;                        ///< Optional processing configuration (may be nullptr)
        const RinexParser* rinex_parser_;                 ///< Optional reference to a RinexParser (may be nullptr)

        std::unordered_map<std::string, std::vector<const ClockData*>> clock_index_;
        void buildClockIndex();

        std::set<std::string> bias_signals_;   ///< Fast lookup: "satellite|signal" keys from BIA
        void buildBiasIndex();

        // Phase wind-up state per satellite (for continuity between epochs)
        mutable std::unordered_map<std::string, double> prev_windup_;

        OceanTideModel ocean_tide_model_;  ///< Ocean tidal loading model

        /**
         * @brief Compute Delaunay fundamental arguments and GMST for solid Earth tide Step 2.
         *
         * Returns [l, l', F, D, Omega, GMST] in radians, using the IERS 2010
         * polynomial expressions (same formulation as OceanTideModel).
         *
         * @param t_centuries Julian centuries of TT from J2000.0.
         * @return Array of 6 fundamental arguments in radians.
         */
        static std::array<double, 6> computeFundamentalArguments(double t_centuries);

        /**
         * @brief Compute IERS 2010 Step 2 frequency-dependent solid Earth tide corrections.
         *
         * Evaluates the per-constituent displacement corrections from IERS 2010
         * Tables 7.3a (diurnal) and 7.3b (semi-diurnal) in the geocentric
         * spherical (radial, north, east) frame and returns the ECEF displacement vector.
         *
         * Geocentric latitude is derived internally from the unit vector @p u
         * so that the latitude factors and local-frame basis vectors are
         * consistent with the geocentric radial direction required by the
         * IERS 2010 spherical harmonic formulas (Eq. 7.12).
         *
         * @param fund_args Delaunay arguments + GMST [l, l', F, D, Omega, GMST] (radians).
         * @param lat Geodetic latitude (radians). Retained for API compatibility;
         *            geocentric latitude is derived from @p u internally.
         * @param lon Geodetic longitude (radians). Identical to geocentric longitude;
         *            used in the tidal argument (m·?) for diurnal/semi-diurnal bands.
         * @param u   Unit vector from geocenter to station (ECEF). Used to derive
         *            geocentric latitude and as the radial basis vector.
         * @return ECEF displacement vector (meters).
         */
        static Eigen::Vector3d computeStep2Corrections(const std::array<double, 6>& fund_args,
            double lat, double lon, const Eigen::Vector3d& u);

    };

}