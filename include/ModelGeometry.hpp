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

	class ProductParser; // forward declaration to avoid unnecessary include in header
	class PppModel;

    struct Topocentric {
        double elevation_deg = 0.0;   // 0..90
        double azimuth_deg   = 0.0;   // 0..360
    };

    class Geometry {

    public:
        // Constructor (deferred to .cpp to avoid inline-init ordering issues)
        Geometry(const GnssConfig& config, ProductParser& parser, const RinexParser& rinex, LogManager& log_mgr);

        // Destructor
        ~Geometry() = default;

        // Computes satellite geometry and corrections (position, elevation, azimuth, delays)
        bool computeSatelliteGeometry(const RinexObservation& obs, const PppState& state,
            Eigen::Vector3d& sat_pos, Eigen::Vector3d& rx_pos, double& elevation,
            double& azimuth, Eigen::Vector3d& los, double& range, double& tropo_delay,
            double& sat_clock, double& shapiro_delay, double& sagnac_corr,
            double& atm_loading_corr, double& relativistic_corr, double& solid_tide,
            double& mf_w, double& mf_d) const;

        Topocentric computeTopocentric(const Eigen::Vector3d& rx_pos_ecef, const Eigen::Vector3d& sat_pos_ecef) const;

        // Interpolates satellite position at a given time using SP3 data
        Eigen::Vector3d getSatellitePosition(const std::string& satellite, const GnssTime& time) const;

        // Computes satellite antenna attitude
        void computeSatelliteAttitude(System system, const Eigen::Vector3d& sat_pos,
            const Eigen::Vector3d& sat_vel, Eigen::Vector3d& d_s,
            Eigen::Vector3d& y_s, const GnssTime& time) const;

        // Computes geometric range (meters) from receiver to satellite
        double computeGeometricRange(const Eigen::Vector3d& rx_pos, const Eigen::Vector3d& sat_pos) const;

        Eigen::Vector3d getSatelliteVelocity(const std::string& satellite, const GnssTime& time) const;

    private:
        GnssCorrections corrections_;                     // GNSS correction models
        const GnssConfig& config_;                        // Configuration settings
        Planetary planetary_;                             // Planetary (sun and moon) models
        Troposphere troposphere_;                         // Troposphere
        ProductParser& product_parser_;                   // Product data parser
        LogManager& log_manager_;                         // Logger for debugging and info
        const RinexParser& rinex_parser_;                 // RINEX data parser
        const double elevation_threshold_;                // Elevation mask (degrees)
        std::vector<OrbitData> orbits_;                   // Satellite orbit data
        std::vector<ClockData> clocks_;                   // Satellite clock data
        std::vector<AntennaData> antennas_;               // Antenna correction data
        std::vector<BiasData> biases_;                    // Bias correction data
        std::vector<ErpData> erps_;                       // Earth rotation parameters
        std::vector<double> klobuchar_coeffs_;            // Klobuchar ionospheric model coefficients
        std::vector<SunEphemeris> sun_ephemeris_;         // Sun ephemeris data
        mutable std::map<std::string, Eigen::Vector3d> last_interpolated_velocity_; // Cached satellite velocities
        const std::map<std::string, std::vector<std::string>> signal_priorities_; // Signal priorities per system

        // Internal loader
        void loadData();
    };

} // namespace GnssProcessor

#endif  // PPP_MODEL_GEOMETRY_HPP