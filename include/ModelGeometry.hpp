#pragma once

/**
 * @file ModelGeometry.hpp
 * @brief Satellite/receiver geometry helper and GNSS geometric corrections.
 *
 * Declares the `GnssProcessor::Model::Geometry` class which computes satellite
 * geometry (position, velocity, line-of-sight, elevation/azimuth) and
 * related corrections used throughout the PPP processing stack.
 *
 * This header contains only declarations; implementation details live in
 * `src/core/model/ModelGeometry.cpp`.
 */

#include <Eigen/Dense>
#include "PppTypes.hpp"
#include "Constants.hpp"
#include "LogManager.hpp"
#include "GnssConfig.hpp"
#include "ProductCache.hpp"
#include "RinexParser.hpp"
#include "GnssCorrections.hpp"
#include "Planetary.hpp"
#include "Troposphere.hpp"
#include <map>
#include <vector>
#include <string>
#include <unordered_map>

namespace GnssProcessor {

    namespace Model {

    /**
     * @brief Simple container for topocentric coordinates.
     *
     * Elevation is given in degrees in the range [0, 90], azimuth in degrees
     * in the range [0, 360).
     */
    struct Topocentric {
        double elevation_deg = 0.0;   /**< Elevation angle in degrees (0..90). */
        double azimuth_deg   = 0.0;   /**< Azimuth angle in degrees (0..360). */
    };

    /**
     * @class Geometry
     * @brief Computes satellite/receiver geometry and auxiliary corrections.
     *
     * `Geometry` encapsulates the geometry computations required by the PPP
     * model: satellite position/velocity interpolation, line-of-sight vectors,
     * elevation/azimuth, geometric range and standard corrections such as:
     * - tropospheric delay (mapping + ZWD)
     * - satellite clock correction
     * - Shapiro and Sagnac delays
     * - solid Earth tide and atmospheric loading
     *
     * The class holds product-derived containers (orbits/clocks/antennas/ERPs)
     * and exposes thread-safe const accessors for geometry used by the
     * measurement modelling code.
     */
    class Geometry {

    public:
        /**
         * @brief Construct a Geometry helper.
         * @param config Read-only processing configuration.
         * @param cache ProductCache singleton (or application cache) providing parsed products.
         * @param rinex Reference to parsed RINEX observation/header metadata.
         * @param log_mgr Logger used for diagnostic messages.
         *
         * Construction defers heavy loading to `loadData()` implemented in the
         * translation unit to avoid inline-initialization ordering issues.
         */
        Geometry(const GnssConfig& config, ProductCache& cache, const RinexParser& rinex, LogManager& log_mgr);

        /** @brief Default virtual destructor. */
        ~Geometry() = default;

        /**
         * @brief Compute satellite geometry and common corrections for an observation.
         *
         * This routine performs the end-to-end geometry computation needed to
         * model a single satellite observation: it obtains (or interpolates)
         * the satellite position/velocity, computes receiver-satellite line-of-sight,
         * elevation/azimuth, geometric range and applies the standard corrections
         * used by the PPP observation model.
         *
         * @param[in] obs RINEX observation record (contains satellite id, signal, epoch).
         * @param[in] state Current PPP state (receiver position, troposphere, etc.).
         * @param[out] sat_pos Satellite ECEF position (meters).
         * @param[out] rx_pos Receiver ECEF position (meters).
         * @param[out] elevation Satellite elevation angle at the receiver (radians or degrees? documented below).
         * @param[out] azimuth Satellite azimuth at the receiver (radians or degrees? documented below).
         * @param[out] los Line-of-sight unit vector from receiver to satellite (ECEF).
         * @param[out] range Geometric range (meters) between receiver and satellite.
         * @param[out] tropo_delay Total tropospheric delay (meters).
         * @param[out] sat_clock Satellite clock correction (seconds).
         * @param[out] shapiro_delay Shapiro relativistic path delay (meters).
         * @param[out] sagnac_corr Sagnac correction (meters).
         * @param[out] atm_loading_corr Atmospheric loading correction (meters).
         * @param[out] relativistic_corr Relativistic orbit-clock correction (meters or seconds depending on caller convention).
         * @param[out] solid_tide Solid Earth tide radial correction (meters).
         * @param[out] mf_w Wide-lane Melbourne-Wübbena combination factor (if applicable).
         * @param[out] mf_d Narrow-lane / dual-frequency mapping factor (if applicable).
         * @param[out] sat_vel Satellite ECEF velocity (meters/second).
         * @return True on success, false if geometry/corrections could not be computed.
         */
        bool computeSatelliteGeometry(const RinexObservation& obs, const PppState& state,
            Eigen::Vector3d& sat_pos, Eigen::Vector3d& rx_pos, double& elevation,
            double& azimuth, Eigen::Vector3d& los, double& range, double& tropo_delay,
            double& sat_clock, double& shapiro_delay, double& sagnac_corr,
            double& atm_loading_corr, double& relativistic_corr, double& solid_tide,
            double& mf_w, double& mf_d, Eigen::Vector3d& sat_vel) const;

        Topocentric computeTopocentric(const Eigen::Vector3d& rx_pos_ecef, const Eigen::Vector3d& sat_pos_ecef) const;

        /**
         * @brief Get interpolated satellite ECEF position from available orbit products.
         * @param satellite Satellite identifier string (e.g. "G01", "E11").
         * @param time Epoch at which the position is requested.
         * @return Satellite ECEF position in meters.
         *
         * If precise orbit (SP3) or propagated ephemeris data are available the
         * position is interpolated from `orbits_`, otherwise an empty/zero
         * vector may be returned (caller must check validity).
         */
        std::pair<Eigen::Vector3d, Eigen::Vector3d> getSatellitePV(
            const std::string& satellite, const GnssTime& time) const;

        /**
         * @brief Compute satellite antenna attitude vectors.
         * @param system GNSS system identifier (enum).
         * @param sat_pos Satellite ECEF position (meters).
         * @param sat_vel Satellite ECEF velocity (meters/second).
         * @param[out] d_s Satellite body-to-sun unit vector (or satellite-specific boresight).
         * @param[out] y_s Satellite cross-track unit vector (system-dependent).
         * @param time Epoch corresponding to satellite state.
         *
         * The routine fills attitude-related unit vectors used for antenna
         * phase center variation (PCV) correction and carrier-phase sign conventions.
         */
        void computeSatelliteAttitude(System system, const Eigen::Vector3d& sat_pos,
            const Eigen::Vector3d& sat_vel, Eigen::Vector3d& d_s,
            Eigen::Vector3d& y_s, const GnssTime& time) const;

        /**
         * @brief Compute geometric range (meters) between receiver and satellite.
         * @param rx_pos Receiver ECEF position (meters).
         * @param sat_pos Satellite ECEF position (meters).
         * @return Euclidean distance in meters.
         */
        double computeGeometricRange(const Eigen::Vector3d& rx_pos, const Eigen::Vector3d& sat_pos) const;

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
         * @brief Get mutable reference to the corrections object.
         * @return Reference to the internal GnssCorrections instance.
         *
         * Allows external components to access correction methods directly,
         * including phase wind-up reset functionality.
         */
        GnssCorrections& getCorrections() { return corrections_; }

        /**
         * @brief Get const reference to the corrections object.
         * @return Const reference to the internal GnssCorrections instance.
         */
        const GnssCorrections& getCorrections() const { return corrections_; }

        /**
         * @brief Get the cached precise Sun position (ECEF, meters).
         *
         * Returns the most recently computed rigorous Sun position from the
         * IAU 1976/1980 precession/nutation model. The cache is populated by
         * computeSatelliteGeometry() and computeSatelliteAttitude().
         *
         * @return Cached Sun ECEF position (meters). May be zero if no epoch has been processed yet.
         */
        const Eigen::Vector3d& getCachedSunPosition() const { return cached_sun_pos_; }

        std::unordered_map<std::string, std::vector<const OrbitData*>> orbit_index_;

    private:
        GnssCorrections corrections_;                     /**< GNSS correction models. */
        const GnssConfig& config_;                        /**< Processing configuration. */
        Planetary planetary_;                             /**< Planetary (sun and moon) models. */
        Troposphere troposphere_;                         /**< Troposphere model instance. */
        ProductCache& product_cache_;                     /**< Product cache reference (replaces ProductParser). */
        LogManager& log_manager_;                         /**< Logger for diagnostics. */
        const RinexParser& rinex_parser_;                 /**< RINEX data parser (header/metadata). */
        const double elevation_threshold_;                /**< Elevation mask in degrees. */
        std::vector<OrbitData> orbits_;                   /**< Loaded satellite orbit records. */
        std::vector<ClockData> clocks_;                   /**< Loaded satellite clock records. */
        std::vector<AntennaData> antennas_;               /**< Antenna correction models. */
        std::vector<BiasData> biases_;                    /**< Receiver/satellite bias entries. */
        std::vector<ErpData> erps_;                       /**< Earth rotation parameter samples. */
        std::vector<double> klobuchar_coeffs_;            /**< Klobuchar ionospheric coefficients. */
        std::vector<SunEphemeris> sun_ephemeris_;         /**< Sun ephemeris samples. */
        const std::map<std::string, std::vector<std::string>> signal_priorities_; /**< Signal priorities per GNSS system. */

        /** @brief Load product and auxiliary data into internal containers. */
        void loadData();

        /**
         * @brief Ensure the Sun/Moon cache is current for the given epoch.
         *
         * Evaluates the rigorous IAU 1976/1980 model only when the epoch has
         * changed (tolerance 1 ms). Called by computeSatelliteGeometry() and
         * computeSatelliteAttitude() to avoid redundant evaluations.
         */
        void ensureSunMoonCached(const GnssTime& time) const;

        /**
         * @brief Extract the best available code pseudorange from an observation.
         *
         * If dual-frequency code measurements exist, forms an ionosphere-free
         * combination; otherwise falls back to a single code pseudorange.
         *
         * @param obs RINEX observation containing raw measurements.
         * @param[out] pseudorange Extracted (or IF-combined) pseudorange in meters.
         * @return True if a valid pseudorange was obtained.
         */
        bool extractPseudorange(const RinexObservation& obs, double& pseudorange) const;

        /**
         * @brief Compute satellite state at signal transmission time.
         *
         * Performs the single-pass emission-time computation: seeds TOF from
         * the pseudorange, applies the satellite clock, and evaluates orbit
         * and clock at the corrected emission epoch.
         *
         * @param obs          RINEX observation (satellite ID + reception time).
         * @param rx_pos       Receiver ECEF position (meters).
         * @param pseudorange  Code pseudorange used as TOF seed (meters).
         * @param[out] sat_pos_em Satellite ECEF position at emission time (meters).
         * @param[out] sat_vel_em Satellite ECEF velocity at emission time (m/s).
         * @param[out] sat_clock  Satellite clock correction at emission time (seconds).
         * @param[out] emission_time  GPS time of signal transmission.
         * @return True on success; false if orbit/clock data are unavailable or invalid.
         */
        bool computeEmissionState(const RinexObservation& obs,
            const Eigen::Vector3d& rx_pos, double pseudorange,
            Eigen::Vector3d& sat_pos_em, Eigen::Vector3d& sat_vel_em,
            double& sat_clock, GnssTime& emission_time) const;

        // Per-epoch cache for Sun/Moon ECEF positions.
        // The rigorous IAU 1976/1980 nutation model is epoch-dependent only
        // (not satellite-dependent), so caching avoids redundant 106-term
        // series evaluations across satellites within the same epoch.
        mutable GnssTime cached_sun_moon_epoch_{0, -1.0};  /**< Epoch of cached Sun/Moon. */
        mutable Eigen::Vector3d cached_sun_pos_{Eigen::Vector3d::Zero()};   /**< Cached Sun ECEF (m). */
        mutable Eigen::Vector3d cached_moon_pos_{Eigen::Vector3d::Zero()};  /**< Cached Moon ECEF (m). */
    };

    } // namespace Model

} // namespace GnssProcessor