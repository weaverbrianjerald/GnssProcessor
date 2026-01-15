// PppTypes.hpp
#pragma once

#include <map>
#include <string>
#include <Eigen/Dense>
#include "TimeUtils.hpp"
#include "Constants.hpp"
#include "LogManager.hpp"

namespace GnssProcessor {

    enum class System {
        GPS,
        Galileo,
        GLONASS,
        BeiDou,
        QZSS,
        SBAS,
        Unknown
    };

    struct PppObservation {
        std::string satellite;
        System system;
        GnssTime time;
        std::map<std::string, double> observed;
        std::map<std::string, double> modeled;
        std::map<std::string, double> residuals;
        std::map<std::string, double> variances;
        std::map<std::string, int> lli_flags;
        Eigen::Vector3d sat_pos;
        double elevation;
        double azimuth;
        Eigen::Vector3d los;
        double range;
        double mf_w;
        double mf_d;
    };

    struct PppState {
        Eigen::Vector3d position;
        double clock_bias;
        double troposphere_zwd;
        double ionosphere_zenith_delay;
        Eigen::MatrixXd covariance;
        std::map<std::string, double> ambiguities;
        GnssTime epoch_time;
    };

    struct EpochMeasurement {
        Eigen::VectorXd innovations;
        Eigen::MatrixXd H;
        Eigen::VectorXd R_v;
        GnssTime time;
        std::vector<PppObservation> observations;
        std::vector<std::string> satellite_ids;
        std::vector<std::string> signal_types;
        Eigen::VectorXd residuals;
        Eigen::MatrixXd residuals_covariance;
        Eigen::VectorXd residuals_standardized;
        size_t bad_residual_count;
    };

    struct OrbitData {
        std::string satellite;
        GnssTime time;
        Eigen::Vector3d position;
        Eigen::Vector3d velocity;
        bool position_valid{ false };
        bool velocity_valid{ false };
    };

    struct ClockData {
        std::string satellite;
        GnssTime time;
        GnssTime start_time;
        GnssTime end_time;
        double clock_bias{ 0.0 };
    };

    struct AntennaData {
        std::string type;
        std::map<std::string, Eigen::Vector3d> pco;
        std::map<std::string, std::vector<std::vector<double>>> pcv;
        double dazi{ 0.0 };
        double dzen{ 0.0 };
        GnssTime valid_from;
        GnssTime valid_until;
    };

    struct BiasData {
        std::string satellite;
        std::string signal;
        double bias{ 0.0 };
        double std_dev;
        GnssTime start_time;
        GnssTime end_time;
    };

    struct ErpData {
        GnssTime time;
        double xp{ 0.0 };
        double yp{ 0.0 };
        double ut1_utc{ 0.0 };
    };

    struct OceanTideData {
        GnssTime time;
        Eigen::Vector3d displacement;
    };

    struct SunEphemeris {
        GnssTime time;
        Eigen::Vector3d position;
    };

    class PppUtils{
    public:
        explicit PppUtils(LogManager& log_mgr);
        double getFrequency(const std::string& freq_type, System system, const std::string& satellite) const;
        double getWavelength(System system, const std::string& freq_type) const;
        double getReferenceFrequency(const std::string& freq_type, System system) const;
        static std::string getSystemString(System system);
    private:
        LogManager& log_manager_;
    };

}
