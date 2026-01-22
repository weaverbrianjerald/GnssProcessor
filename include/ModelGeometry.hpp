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

#ifndef PPP_MODEL_GEOMETRY_HPP
#define PPP_MODEL_GEOMETRY_HPP

#include <Eigen/Dense>
#include "PppTypes.hpp"
#include "Constants.hpp"
#include "LogManager.hpp"
#include "GnssConfig.hpp"
#include "RinexParser.hpp"
#include "GnssCorrections.hpp"
#include "Planetary.hpp"
#include "Troposphere.hpp"
#include <map>
#include <vector>
#include <string>

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
         * @param parser Parser/loader for external products (SP3, CLK, ERP, etc.).
         * @param rinex Reference to parsed RINEX observation/header metadata.
         * @param log_mgr Logger used for diagnostic messages.
         *
         * Construction defers heavy loading to `loadData()` implemented in the
         * translation unit to avoid inline-initialization ordering issues.
         */
        Geometry(const GnssConfig& config, ProductParser& parser, const RinexParser& rinex, LogManager& log_mgr);

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

        /**
         * @brief Compute topocentric elevation/azimuth for a receiver and satellite.
         * @param rx_pos_ecef Receiver ECEF position (meters).
         * @param sat_pos_ecef Satellite ECEF position (meters).
         * @return Topocentric angles (elevation/azimuth) in degrees.
         */
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
        Eigen::Vector3d getSatellitePosition(const std::string& satellite, const GnssTime& time) const;

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
         * @brief Get interpolated satellite velocity from available orbit products.
         * @param satellite Satellite identifier string (e.g. "G01", "E11").
         * @param time Epoch at which the velocity is requested.
         * @return Satellite ECEF velocity in meters/second.
         */
        Eigen::Vector3d getSatelliteVelocity(const std::string& satellite, const GnssTime& time) const;

    private:
        GnssCorrections corrections_;                     /**< GNSS correction models. */
        const GnssConfig& config_;                        /**< Processing configuration. */
        Planetary planetary_;                             /**< Planetary (sun and moon) models. */
        Troposphere troposphere_;                         /**< Troposphere model instance. */
        ProductParser& product_parser_;                   /**< Product data parser. */
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
        mutable std::map<std::string, Eigen::Vector3d> last_interpolated_velocity_; /**< Cached satellite velocities. */
        const std::map<std::string, std::vector<std::string>> signal_priorities_; /**< Signal priorities per GNSS system. */

        /** @brief Load product and auxiliary data into internal containers. */
        void loadData();
    };

    } // namespace Model

} // namespace GnssProcessor

#endif  // PPP_MODEL_GEOMETRY_HPP